// VPKTAA: the Klepton adapter around VPKEngine's NATIVE VPKTAA pass (taa_native_metal.mm,
// vpvk_taa_native_encode) — the only VPKTAA there is. It runs INLINE (kl_vulkan_taa_inline() == 1:
// Vulkan guest on VPVulkan with the native pass on): VPKEngine jitters the eye draws' viewport
// itself (its own Halton(2,3)); at the guest's frame end kl_vulkan calls inlineFrame (guest
// thread), which gathers the frame (pose, unjittered tangents, crop, live depth incl. MSAA, AppSW
// motion when the guest wrote it) and encodes both eyes in one dispatch (+ RCAS) into a command
// buffer of VPKEngine's own queue (kl_vulkan_native_submit: ordered after the whole frame, and
// before the next one), writing a 2-layer output of a small ring, and returns at once — no CPU
// wait. VPKEngine's reactive R8 UI mask keeps the UI/2D drawn into the eye buffer sharp. When that
// command buffer COMPLETES the output is published with the frame's own record (acquireInline)
// and kl_vulkan's serial advances to that frame. At most 2 resolves in flight. A frame that is
// not resolved (AA gate closed, refused) is waited for by kl_vulkan and shown live with its record.
// Guests the native pass does not reach (GLES/ANGLE, MoltenVK, no frame record such as OpenXR)
// get NO TAA: there is no other resolver. A GLES guest on vpgles IS reached (kl_vulkan_taa_gles_frame_end,
// from EndFrame4 / xrEndFrame): its stage is the one the eye layer's submit named (kl_glfb's draw
// observation is ANGLE-only), and its eye images are bottom-up (GL rows, vpgles.h), registered with
// origin top_left = 0 (kl_glfb_note_eye_mtl_texture_o) so the compositor flips them and the tangents
// are mirrored here (inBottomUp: t' = -b, b' = -t) to reproject in the image's own sense. A Vulkan guest's
// images are top-left (top_left = 1, tangents as given). Both flags derive from the same fact.
//   * conventions: reversed-Z from kl_vulkan (eye-pass clear 0.0 / compare-op census), near
//     from the submit (KL_VPKTAA_NEARFAR_OFF) else KL_VPKTAA_NEAR, else the frustum's zNear
//     (kl_ovrp_frustum_near, 0.1 m), far KL_VPKTAA_FAR (0 = infinite); MV scale
//     KL_VPKTAA_MV_SCALE (0.5: NDC delta -> uv) with the sign KL_VPKTAA_MV_SIGN (1 | -1; default
//     +x+y), KL_VPKTAA_MV_YFLIP=1 flips y;
//   * diagnostics: [taa] inputs per eye + stats every 30 s (gpu ms, composites shown).
// Settings from the app as environment (KL_VPKTAA=1, KL_VPKTAA_PCT, KL_RCAS (RCAS intensity) — the pass
// itself reads percent/sharpness from vpvk_config_set_vpktaa; KL_AA_MODE, VQ_GAME_KEY: the
// automatic AA gate lives in kl_vulkan, the detection is stored here).
// Threads: the inline resolve state is under `coreLock` (guest thread); outputs/publication under
// `lock`; coreLock is never taken while `lock` is held.
import Metal
import simd
import Foundation

final class KleptonTAA {
    static let enabled: Bool = getenv("KL_VPKTAA").map { String(cString: $0) == "1" } ?? false
    /// RCAS (VPKEngine's sharpening pass, «Nitidez (RCAS)» in the app): intensity 0...1 from KL_RCAS,
    /// always on in every game and AA mode; 0 = no pass at all. Frames VPKTAA does not resolve (game
    /// MSAA, msaa/off modes, gate closed) go through the same hook as RCAS-only frames.
    static let rcasIntensity: Float = {
        guard let v = getenv("KL_RCAS"), let f = Float(String(cString: v)) else { return 0 }
        return min(max(f, 0), 1)
    }()
    /// RCAS applied by the compositor's composite pass to every frame the native VPKTAA did not resolve
    /// (kl_reproject.c kl_rcas): no pass, ring or hook of its own. KL_RCAS_COMPOSITOR=0 = the separate
    /// native pass (kl_vulkan_rcas_on reads the same variable).
    static let rcasInCompositor: Bool = rcasIntensity > 0 && klEnvOn("KL_RCAS_COMPOSITOR", default: true)
    /// The hook is needed for VPKTAA and/or a separate RCAS pass.
    static let passEnabled: Bool = enabled || (rcasIntensity > 0 && !rcasInCompositor)
    private static func envS(_ k: String) -> String? {
        guard let v = getenv(k) else { return nil }
        return String(cString: v)
    }
    private static func envF(_ k: String, _ d: Float) -> Float {
        guard let s = envS(k), let f = Float(s) else { return d }
        return f
    }
    private let percent = min(max(KleptonTAA.envF("KL_VPKTAA_PCT", 80), 0), 100)
    // Near plane for the depth linearisation when the submit carries none: KL_VPKTAA_NEAR if
    // set, else the zNear ovrp_GetNodeFrustum2 serves the guest (0.1 m).
    private let nearForced: Float? = KleptonTAA.envS("KL_VPKTAA_NEAR").flatMap { Float($0) }.map { max($0, 0.001) }
    private var nearDefault: Float { nearForced ?? max(kl_ovrp_frustum_near(), 0.001) }
    private let farDefault = max(KleptonTAA.envF("KL_VPKTAA_FAR", 0), 0)
    private let mvScaleMag = KleptonTAA.envF("KL_VPKTAA_MV_SCALE", 0.5)
    private var mvSign = SIMD2<Float>(1, 1)
    private let revZOverride: Int = {   // -1 = from kl_vulkan, 0/1 forced
        guard let s = KleptonTAA.envS("KL_VPKTAA_REVZ"), let v = Int(s) else { return -1 }
        return v == 0 ? 0 : 1
    }()
    private let useMV = KleptonTAA.envS("KL_VPKTAA_MV") != "0"
    private let mvMap = KleptonTAA.envS("KL_VPKTAA_MV_MAP") ?? "auto"
    private let depthMap = KleptonTAA.envS("KL_VPKTAA_DEPTH_MAP") ?? "auto"

    private let device: MTLDevice

    struct Published {
        let slot: Int
        let pose: kl_ovrp_render_pose
        let out: [MTLTexture]         // per eye, 2D array slice 0, rgba8 sRGB
        let flipY: [Bool]
        let taa: Bool                 // a real VPKTAA resolve (unsharpened); false = rcas_only (already sharpened by the engine)
    }
    private let lock = NSLock()
    // Stats / diagnostics.
    private var gpuMsSum = 0.0, gpuMsN = 0, gpuMsMax = 0.0
    private var saidCropOff = [false, false]

    init?(device: MTLDevice) {
        guard KleptonTAA.passEnabled else { return nil }
        self.device = device
        if KleptonTAA.envS("KL_VPKTAA_MV_SIGN") == "-1" { mvSign = SIMD2<Float>(-1, -1) }
        if KleptonTAA.envS("KL_VPKTAA_MV_YFLIP") == "1" { mvSign.y = -mvSign.y }
    }

    private static func fmtName(_ f: Int32) -> String {
        switch f {
        case 124: return "D16"
        case 125: return "X8D24"
        case 126: return "D32F"
        case 128: return "D16S8"
        case 129: return "D24S8"
        case 130: return "D32FS8"
        default: return "fmt\(f)"
        }
    }

    /// Depth of `eye` for exactly this frame record, or nil (+ why, for the log). Async path:
    /// `stage`/`tag` = the record's, single-sample only (its depth may be a copy). Inline path:
    /// tag 0 (the depth kl_vulkan picked for the frame ending right now, live) and multisampled
    /// depth accepted (the native pass reads the nearest sample).
    private func depthInput(_ eye: Int, _ r: kl_ovrp_render_pose, stage: Int, tag: UInt64,
                            allowMS: Bool) -> (VPKTAADepthInput?, String, Bool) {
        var info = kl_vulkan_eye_depth()
        let ok = kl_vulkan_eye_depth_get(Int32(stage), Int32(eye), tag, &info) != 0
        if !ok { return (nil, "none", false) }
        if info.samples > 1 && !allowMS { return (nil, "msaa\(info.samples)x(unusable)", true) }
        guard let p = info.mtl else { return (nil, "no-mtl", true) }
        let tex = Unmanaged<MTLTexture>.fromOpaque(p).takeUnretainedValue() as MTLTexture
        let rev: Bool
        switch revZOverride {
        case 0: rev = false
        case 1: rev = true
        default: rev = info.reversed_z != 0     // -1 (unknown) -> reversed: Vulkan engines' default
        }
        let fromSubmit = r.depth_near > 0
        let near = fromSubmit ? r.depth_near : nearDefault
        let far = fromSubmit ? max(r.depth_far, 0) : farDefault
        let src = info.source == 1 ? "eye-pass" : info.source == 2 ? "spacewarp" : "size-match"
        let nearSrc: String = fromSubmit ? "(submit)" : (nearForced != nil ? "(KL_VPKTAA_NEAR)" : "(frustum)")
        let desc = "\(src) \(info.snapshot != 0 ? "copy" : "live") \(KleptonTAA.fmtName(info.vk_format)) "
            + "\(info.w)x\(info.h)\(info.samples > 1 ? " ms\(info.samples)x" : "") s\(info.slice) revZ=\(rev ? 1 : 0)"
            + (revZOverride >= 0 ? "(forced)" : info.clear_depth >= 0
                ? "(clear \(String(format: "%.3f", info.clear_depth)))" : "(census)")
            + " near=\(String(format: "%.3f", near))\(nearSrc)"
            + " far=\(far > 0 ? String(format: "%.1f", far) : "inf")"
        return (VPKTAADepthInput(texture: tex, slice: Int(info.slice), reversedZ: rev, near: near, far: far),
                desc, true)
    }

    /// AppSW motion vectors of `eye` when the guest wrote them for this frame.
    private func motionInput(_ eye: Int, _ r: kl_ovrp_render_pose) -> (VPKTAAMotionInput?, String) {
        guard useMV else { return (nil, "off(KL_VPKTAA_MV=0)") }
        guard (r.asw & Int32(KL_OVRP_ASW_STORAGE)) != 0 else { return (nil, "none") }
        guard (r.asw & Int32(KL_OVRP_ASW_MV)) != 0 else { return (nil, "not-written") }
        var w: Int32 = 0, h: Int32 = 0, sl: Int32 = 0
        guard let p = kl_vulkan_spacewarp_mtl(Int32(eye), Int32(r.stage), Int32(KLVK_ASW_MOTION),
                                              &w, &h, &sl) else { return (nil, "no-mtl") }
        let tex = Unmanaged<MTLTexture>.fromOpaque(p).takeUnretainedValue() as MTLTexture
        let s = mvSign * mvScaleMag
        return (VPKTAAMotionInput(texture: tex, slice: Int(sl), scale: s),
                "live \(w)x\(h) s\(sl) scale=(\(String(format: "%+.2f", s.x)),\(String(format: "%+.2f", s.y)))")
    }

    /// The eye's input for VPKEngine's native pass. The jitter is not carried: VPKEngine chooses
    /// and applies it itself.
    private func makeInput(eye: Int, color: MTLTexture, slice: Int, flipY: Bool,
                           rendered r: kl_ovrp_render_pose,
                           depth: VPKTAADepthInput?, motion: VPKTAAMotionInput?) -> VPKTAAEyeInput? {
        guard color.textureType == .type2DArray else { return nil }
        let w = color.width, h = color.height
        let t = r.tangents
        let tan: SIMD4<Float> = eye == 0 ? SIMD4(t.0.0, t.0.1, t.0.2, t.0.3) : SIMD4(t.1.0, t.1.1, t.1.2, t.1.3)
        var crop = SIMD2<Float>(1, 1)
        var cropOff = SIMD2<Float>(0, 0)
        let vp = eye == 0 ? r.viewport.0 : r.viewport.1
        // Only when the rect describes THIS texture (the composite applies the same rule,
        // KleptonCompositor.unwarpGrid): the output keeps the source's layout.
        if vp.2 > 0, vp.3 > 0, r.viewport_of.0 > 0, r.viewport_of.1 > 0,
           Int(r.viewport_of.0) == w, Int(r.viewport_of.1) == h {
            crop = SIMD2(min(1, Float(vp.2) / Float(r.viewport_of.0)), min(1, Float(vp.3) / Float(r.viewport_of.1)))
            cropOff = SIMD2(min(max(Float(vp.0) / Float(r.viewport_of.0), 0), 1 - crop.x),
                            min(max(Float(vp.1) / Float(r.viewport_of.1), 0), 1 - crop.y))
        }
        if cropOff != .zero && !saidCropOff[eye] {
            saidCropOff[eye] = true
            NSLog("[taa] eye \(eye) renders into an offset rect: uv origin (\(cropOff.x), \(cropOff.y)) "
                  + "size (\(crop.x), \(crop.y)) of \(w)x\(h) — resolved in place (symmetric projection)")
        }
        // The eye camera the guest rendered this eye with: head (frame record) * eye-local pose.
        let qh = simd_quatf(ix: r.qx, iy: r.qy, iz: r.qz, r: r.qw).normalized
        var off: [Float] = [0, 0, 0]
        var rot: [Float] = [0, 0, 0, 1]
        kl_ovrp_eye_local(Int32(eye), &off, &rot)
        let qe = simd_quatf(ix: rot[0], iy: rot[1], iz: rot[2], r: rot[3]).normalized
        let pos = SIMD3<Float>(r.px, r.py, r.pz) + qh.act(SIMD3<Float>(off[0], off[1], off[2]))
        // Layout of depth / motion relative to the colour: a texture of the colour's size has
        // its layout; any other size covers the eye's crop rect (AppSW renders motion vectors —
        // and its own depth — at MotionVectorTextureSize with the eye's viewport: AC Nexus
        // 1549x1243 for a 3029x2485 crop of a 3840x2485 eye texture). KL_VPKTAA_MV_MAP /
        // KL_VPKTAA_DEPTH_MAP = full | crop force it.
        var dep = depth
        if var d = dep {
            let same = d.texture.width == w && d.texture.height == h
            d.mapsCrop = depthMap == "crop" ? true : depthMap == "full" ? false : !same
            dep = d
        }
        var mot = motion
        if var m = mot {
            let same = m.texture.width == w && m.texture.height == h
            m.mapsCrop = mvMap == "crop" ? true : mvMap == "full" ? false : !same
            mot = m
        }
        return VPKTAAEyeInput(color: color, slice: slice, flipY: flipY, tangents: tan,
                              crop: crop, cropOff: cropOff, orientation: qh * qe, position: pos,
                              depth: dep, motion: mot)
    }

    // ── INLINE mode (see the header) ─────────────────────────────────────────────────────────
    /// Box for the encode step kl_vulkan_native_submit calls back into (a C callback cannot
    /// capture; the box travels as its `user` pointer, alive for the synchronous call).
    private final class InlineEncode {
        let body: (MTLCommandBuffer) -> Void
        init(_ body: @escaping (MTLCommandBuffer) -> Void) { self.body = body }
    }
    /// One output of the ring: 2-layer array (layer = eye) in the eye's sRGB format, written
    /// per eye through a single-layer non-sRGB view (the kernel encodes sRGB itself) and
    /// sampled by the composite through a single-layer sRGB view (slice 0 = that eye).
    private struct InlineOut {
        let tex: MTLTexture
        let layers: [MTLTexture]
        let reads: [MTLTexture]
        let both: MTLTexture        // both layers through the non-sRGB view (VPKTAA nativo)
        let w: Int, h: Int
        let fmt: MTLPixelFormat
    }
    /// One committed resolve, from its encode (guest thread) to its completion (Metal's thread).
    private final class InlineJob {
        let k: Int, gen: Int
        let pose: kl_ovrp_render_pose
        let vkSerial: UInt64
        var ok = false                     // both eyes encoded
        let taa: Bool                      // a VPKTAA resolve (rcas_only = 0), not an RCAS-only frame
        init(k: Int, gen: Int, pose: kl_ovrp_render_pose, vkSerial: UInt64, taa: Bool) {
            self.k = k; self.gen = gen; self.pose = pose; self.vkSerial = vkSerial; self.taa = taa
        }
    }
    /// What the compositor shows this frame in inline mode.
    enum InlineShow {
        case resolved(Published)                    // a completed resolve, with its own record
        case raw(kl_ovrp_render_pose, Int)          // an unresolved, waited-for frame (record, stage)
        case hide                                   // nothing complete to show yet (eye not drawn)
    }
    private var hookSet = false                     // compositor thread only
    private var hookSetAt = Date.distantPast        // compositor thread only
    private var inlineLatched = false               // compositor thread only
    // The per-resolve state below is used by the guest thread (inline) and the stats: one lock
    // for all of it. Never taken while `lock` is held.
    private let coreLock = NSLock()
    // At most 2 resolves on the GPU: the third frame end waits (bounded) for the oldest.
    private let inFlight = DispatchSemaphore(value: 2)
    // Slots needed when a resolve takes one: inFlight (2) caps the busy ones at 1 besides the new
    // one, + the published one + 1 older slot still pinned by a composite on the GPU = 4. With 3 a
    // pinned-old slot starves the ring ("no free output": an unresolved frame); 4 saves a full
    // eye-size texture (76 MB at 2976x1912) over the old 5 without that risk.
    private static let ringSize = 4
    // The output ring, under `lock`. A slot is written only when it is neither in flight,
    // published, nor sampled by a composite still on the GPU (pinned) — the resolve runs on
    // VPKEngine's queue, the composite on the compositor's, and Metal tracks no hazards across
    // queues, so this is the whole of the synchronisation.
    private var inOuts: [InlineOut] = []
    private var inBusy: [Bool] = []
    private var inPins: [Int] = []
    private var inPublished: Published?             // slot = ring index
    private var inGen = 0
    private var inNext = 0
    private var inShown = false                     // under `lock`: the last composite showed a resolve
    private var inShowResolved = 0, inShowRaw = 0, inShowOld = 0, inShowNone = 0   // under `lock`
    private var inGpuErrors = 0                     // under `lock`
    private var inLateMs = 0.0, inLateMax = 0.0, inLateN = 0   // under `lock`: commit -> completion
    private var colourViews: [ObjectIdentifier: MTLTexture] = [:]
    private var inFrames = 0, inResolved = 0, inJitFrames = 0, inMixedFrames = 0
    private var inJittered: UInt64 = 0, inSkipped: UInt64 = 0, inMixed: UInt64 = 0
    private var inDepth = [0, 0], inDepthMS = [0, 0], inMV = [0, 0]
    private var inRawWhy: [String: Int] = [:]
    private var inLastJ = SIMD2<Float>(0, 0)
    private var inLastTarget = (Int32(0), Int32(0))
    private var inSaidFmt = false
    /// The textures the eye colour views were made from (kl_vulkan_taa_native_eye.color_base), per eye.
    private var inBase: [UnsafeMutableRawPointer?] = [nil, nil]
    /// vpgles memory is bottom-up (GL row 0 = memory row 0): the tangents are mirrored for the kernel.
    private var inBottomUp = false

    /// Called by the compositor every frame: true while the inline mode is on (the compositor
    /// then shows acquireInline's answer and leaves submit/acquire alone). Registers the
    /// frame-end hook the first time. Once kl_vulkan has seen a frame end on the hook path
    /// (record filed for the stage being finished) it stays inline for the rest of the run:
    /// loading screens and gpu.hold gaps without hook calls must not drop the resolves. Before that, 3 s of grace;
    /// a path that never reaches the hook (OpenXR) then gets no VPKTAA.
    func pollInline() -> Bool {
        guard kl_vulkan_taa_inline() == 1 else { return false }
        if !hookSet {
            hookSet = true
            hookSetAt = Date()
            // VPKEngine's queue resolves into textures made on the compositor's device.
            if let nd = kl_vulkan_native_device(),
               nd != Unmanaged.passUnretained(device as AnyObject).toOpaque() {
                NSLog("[taa-inline] WARNING VPKEngine's MTLDevice differs from the compositor's")
            }
            // Retained on purpose: the hook is called on the guest thread for the rest of the
            // run, independently of the compositor's lifetime (one KleptonTAA per run).
            let me = Unmanaged.passRetained(self).toOpaque()
            kl_vulkan_set_taa_hook({ stage, vkSerial, user in
                guard let user else { return 0 }
                return Unmanaged<KleptonTAA>.fromOpaque(user).takeUnretainedValue()
                    .inlineFrame(stage: Int(stage), vkSerial: UInt64(vkSerial))
            }, me)
            NSLog("%@", "[taa-inline] mode: resolve at the guest's frame end on VPKEngine's queue, no CPU "
                  + "wait (output shown when its command buffer completes, max 2 in flight), jitter "
                  + "applied by VPKEngine (viewport), pct=\(Int(percent)) rcas=\(String(format: "%.2f", KleptonTAA.rcasIntensity))")
        }
        if inlineLatched { return true }
        if kl_vulkan_taa_inline_frames() > 0 {
            inlineLatched = true
            NSLog("[taa-inline] frame-end hook path confirmed — inline for the rest of the run "
                  + "(gaps show the last resolve or the unresolved frame)")
            return true
        }
        if Date().timeIntervalSince(hookSetAt) < 3 { return true }
        return false
    }

    /// Inline: the last composite showed a resolved picture (HUD).
    var inlineShowing: Bool { lock.lock(); defer { lock.unlock() }; return inShown }

    /// Inline: what the composite encoded into `cmd` shows — the newest COMPLETED resolve,
    /// pinned until `cmd` completes, unless a newer frame went out unresolved (AA gate closed,
    /// refused resolve: kl_vulkan waited for it, so its live picture is complete), which is
    /// then shown with its own record. .hide = resolves were committed but none has completed
    /// and no frame was waited for: every live eye may still be on the GPU. nil = no inline
    /// frame at all yet (the compositor's own default).
    func acquireInline(for cmd: MTLCommandBuffer) -> InlineShow? {
        // Never compose an eye whose frame did not complete: a lost GPU shows nothing (belt and braces
        // with prepareTAA's check; kl_vulkan also stops filing raw records once lost).
        if kl_vulkan_gpu_lost() != 0 { return .hide }
        let raw = UInt64(kl_vulkan_taa_raw_record())
        lock.lock()
        let pub = inPublished
        if let p = pub, p.pose.serial > raw, let pinned = pinInline(p, cmd) {
            inShown = true; inShowResolved += 1
            lock.unlock()
            return .resolved(pinned)
        }
        inShown = false
        lock.unlock()
        // Only while its stage cannot be being redrawn yet: frames after it are not waited for,
        // and the guest draws into that stage again `stages` frames later.
        if raw != 0, let rec = KleptonTAA.stageRecord(serial: raw),
           KleptonTAA.newestRecordSerial() <= raw + UInt64(max(0, Int(kl_ovrp_stage_count()) - 2)) {
            lock.lock(); inShowRaw += 1; lock.unlock()
            return .raw(rec.0, rec.1)
        }
        // The unresolved frame's stage is already being redrawn: an older resolve still is a
        // complete picture with its own record.
        lock.lock(); defer { lock.unlock() }
        if let p = pub, let pinned = pinInline(p, cmd) { inShowOld += 1; return .resolved(pinned) }
        inShowNone += 1
        // Hook-path frames exist, yet none was waited for and no resolve completed: the newest
        // stages are all still on the GPU (the first frame or two after the hook path starts).
        return raw == 0 && pub == nil && kl_vulkan_taa_inline_frames() > 0 ? .hide : nil
    }

    /// Pins ring slot `p.slot` until `cmd` completes (under `lock`).
    private func pinInline(_ p: Published, _ cmd: MTLCommandBuffer) -> Published? {
        guard p.slot < inPins.count else { return nil }
        inPins[p.slot] += 1
        let k = p.slot, gen = inGen
        cmd.addCompletedHandler { [weak self] _ in
            guard let self else { return }
            self.lock.lock()
            if gen == self.inGen && k < self.inPins.count { self.inPins[k] -= 1 }
            self.lock.unlock()
        }
        return p
    }

    /// Serial of the newest complete frame record (0 = none).
    private static func newestRecordSerial() -> UInt64 {
        let st = Int(kl_ovrp_last_complete_stage())
        guard st >= 0 else { return 0 }
        var r = kl_ovrp_render_pose()
        guard withUnsafeMutablePointer(to: &r, { kl_ovrp_stage_render_pose(Int32(st), $0) }) != 0 else { return 0 }
        return r.serial
    }

    /// The complete frame record carrying `serial`, and its stage.
    private static func stageRecord(serial: UInt64) -> (kl_ovrp_render_pose, Int)? {
        let n = Int(kl_ovrp_stage_count())
        for st in 0..<max(0, min(n, 16)) {
            var r = kl_ovrp_render_pose()
            if withUnsafeMutablePointer(to: &r, { kl_ovrp_stage_render_pose(Int32(st), $0) }) != 0,
               r.serial == serial, r.complete != 0 { return (r, st) }
        }
        return nil
    }

    /// rgba8/bgra8 sRGB eye formats -> the plain view format the kernel writes through.
    private static func writeFormat(_ f: MTLPixelFormat) -> MTLPixelFormat? {
        switch f {
        case .rgba8Unorm_srgb: return .rgba8Unorm
        case .bgra8Unorm_srgb: return .bgra8Unorm
        default: return nil     // linear / float eyes: the kernel's sRGB encode would be wrong
        }
    }

    private func makeInlineOut(_ w: Int, _ h: Int, _ fmt: MTLPixelFormat, _ k: Int) -> InlineOut? {
        guard let wf = KleptonTAA.writeFormat(fmt) else { return nil }
        let d = MTLTextureDescriptor()
        d.textureType = .type2DArray
        d.pixelFormat = fmt
        d.width = w; d.height = h; d.arrayLength = 2
        d.usage = [.shaderRead, .shaderWrite, .pixelFormatView]
        d.storageMode = .private
        guard let t = device.makeTexture(descriptor: d) else { return nil }
        t.label = "vpktaa inline out \(k)"
        kl_mtl_track("taa.inline.out", t)
        var layers: [MTLTexture] = [], reads: [MTLTexture] = []
        for e in 0..<2 {
            guard let v = t.makeTextureView(pixelFormat: wf, textureType: .type2DArray,
                                            levels: 0..<1, slices: e..<(e + 1)),
                  let r = t.makeTextureView(pixelFormat: fmt, textureType: .type2DArray,
                                            levels: 0..<1, slices: e..<(e + 1)) else { return nil }
            layers.append(v); reads.append(r)
        }
        guard let both = t.makeTextureView(pixelFormat: wf, textureType: .type2DArray,
                                           levels: 0..<1, slices: 0..<2) else { return nil }
        return InlineOut(tex: t, layers: layers, reads: reads, both: both, w: w, h: h, fmt: fmt)
    }

    /// A free ring slot for a w x h fmt output, marked busy (under `lock`); (index, nil) or
    /// (nil, why not). A size/format change rebuilds the ring once nothing uses it.
    private func takeInlineSlot(_ w: Int, _ h: Int, _ fmt: MTLPixelFormat) -> (Int?, String?) {
        if inOuts.isEmpty || inOuts[0].w != w || inOuts[0].h != h || inOuts[0].fmt != fmt {
            if inBusy.contains(true) || inPins.contains(where: { $0 > 0 }) {
                return (nil, "output ring busy (eye size change)")
            }
            // Nothing is busy or pinned here: release the old ring BEFORE allocating the new one,
            // so the two never coexist (peak ~ one ring instead of old + new). The generation
            // bump makes any late completed handler of the old ring a no-op.
            let oldDesc = inOuts.isEmpty ? "" : "\(inOuts[0].w)x\(inOuts[0].h)"
            inOuts = []; inBusy = []; inPins = []; inPublished = nil
            inGen += 1
            var ring: [InlineOut] = []
            for k in 0..<KleptonTAA.ringSize {
                guard let o = makeInlineOut(w, h, fmt, k) else { return (nil, "output allocation failed") }
                ring.append(o)
            }
            if !oldDesc.isEmpty {
                NSLog("[taa-inline] eye \(oldDesc) -> \(w)x\(h): output ring rebuilt "
                      + "(old outputs released first)")
            }
            inOuts = ring
            inBusy = Array(repeating: false, count: ring.count)
            inPins = Array(repeating: 0, count: ring.count)
        }
        for i in 0..<inOuts.count {
            let k = (inNext + i) % inOuts.count
            if !inBusy[k] && inPins[k] == 0 && k != inPublished?.slot {
                inBusy[k] = true
                inNext = (k + 1) % inOuts.count
                return (k, nil)
            }
        }
        return (nil, "no free output (all in flight, shown or pinned)")
    }

    /// The colour as a 2D array (the native pass's input type); a 2D texture gets a cached view.
    private func colourArray(_ t: MTLTexture) -> MTLTexture? {
        if t.textureType == .type2DArray { return t }
        guard t.textureType == .type2D else { return nil }
        let k = ObjectIdentifier(t)
        if let v = colourViews[k] { return v }
        guard let v = t.makeTextureView(pixelFormat: t.pixelFormat, textureType: .type2DArray,
                                        levels: 0..<1, slices: 0..<1) else { return nil }
        if colourViews.count > 16 { colourViews.removeAll() }   // the view retains its texture
        colourViews[k] = v
        return v
    }

    /// The hook (guest thread, inside kl_vulkan_frame_done). 1 = a resolve was committed
    /// (kl_vulkan then does not wait; inlineCompleted advances the serial), 0 = unresolved.
    private func inlineFrame(stage: Int, vkSerial: UInt64) -> Int32 {
        coreLock.lock(); defer { coreLock.unlock() }
        inFrames += 1
        let why = inlineResolve(stage: stage, vkSerial: vkSerial)
        if let why { inRawWhy[why, default: 0] += 1 } else { inResolved += 1 }
        inlineStats()
        return why == nil ? 1 : 0
    }

    /// Encode and commit `stage`'s resolve; nil when committed with both eyes, else why not.
    private func inlineResolve(stage: Int, vkSerial: UInt64) -> String? {
        // Jitter VPKEngine applied to this frame. Read every frame, first: the read resets
        // VPKEngine's counters, so they always cover exactly one guest frame.
        // VPKTAA frame, or RCAS alone (no jitter, no resolve, no depth/motion): decided by kl_vulkan at this frame end.
        let taaFrame = KleptonTAA.enabled && kl_vulkan_hook_is_taa() != 0
        var jx: Float = 0, jy: Float = 0
        var tw: Int32 = 0, th: Int32 = 0
        var nj: UInt32 = 0, nsk: UInt32 = 0, nmx: UInt32 = 0
        let had = kl_vulkan_taa_applied(&jx, &jy, &tw, &th, &nj, &nsk, &nmx) != 0
        var applied: (px: SIMD2<Float>, target: SIMD2<Float>)? = nil
        if had {
            inJittered += UInt64(nj); inSkipped += UInt64(nsk); inMixed += UInt64(nmx)
            if nmx > 0 { inMixedFrames += 1 }   // still use the reported (last) value
            if nj > 0, jx.isFinite, jy.isFinite, abs(jx) <= 1, abs(jy) <= 1 {
                applied = (px: SIMD2<Float>(jx, jy), target: SIMD2<Float>(Float(max(tw, 0)), Float(max(th, 0))))
                inJitFrames += 1
                inLastJ = SIMD2(jx, jy); inLastTarget = (tw, th)
            }
        }

        var r = kl_ovrp_render_pose()
        guard withUnsafeMutablePointer(to: &r, { kl_ovrp_stage_render_pose(Int32(stage), $0) }) != 0,
              r.serial != 0 else { return "no frame record" }

        // Live colour of both eyes.
        var srcs: [(MTLTexture, Int)] = []
        for e in 0..<2 {
            var sl: Int32 = 0, cw: Int32 = 0, ch: Int32 = 0
            guard let p = kl_vulkan_eye_color_mtl(Int32(stage), Int32(e), &sl, &cw, &ch) else {
                return "no eye colour"
            }
            let t = Unmanaged<MTLTexture>.fromOpaque(p).takeUnretainedValue() as MTLTexture
            inBase[e] = p
            guard let a = colourArray(t) else { return "eye colour type \(t.textureType.rawValue)" }
            guard sl >= 0, Int(sl) < a.arrayLength else { return "eye colour slice \(sl) out of range" }
            srcs.append((a, Int(sl)))
        }
        let w = srcs[0].0.width, h = srcs[0].0.height, fmt = srcs[0].0.pixelFormat
        guard srcs[1].0.width == w, srcs[1].0.height == h, srcs[1].0.pixelFormat == fmt else {
            return "eyes differ in size/format"
        }
        guard KleptonTAA.writeFormat(fmt) != nil else {
            if !inSaidFmt {
                inSaidFmt = true
                NSLog("[taa-inline] eye format \(fmt.rawValue) is not rgba8/bgra8 sRGB — frames go out unresolved")
            }
            return "eye format \(fmt.rawValue)"
        }

        // 2 frames in flight: the third waits for the oldest (bounded; the GPU normally
        // finishes a resolve long before). Released by inlineCompleted, or below.
        if inFlight.wait(timeout: .now() + 0.25) == .timedOut { return "2 resolves still on the GPU after 250 ms" }
        lock.lock()
        let (slot, busyWhy) = takeInlineSlot(w, h, fmt)
        let gen = inGen
        let outOpt: InlineOut? = slot.map { inOuts[$0] }
        if slot == nil && busyWhy?.hasPrefix("output ring busy") == true {
            inPublished = nil      // old-size output: the unresolved frames show meanwhile
        }
        lock.unlock()
        guard let k = slot, let out = outOpt else { inFlight.signal(); return busyWhy ?? "no output" }
        // Until the command buffer owns the slot and the semaphore (its completed handler
        // releases both), every early return gives them back.
        var owned = false
        defer {
            if !owned {
                lock.lock(); if gen == inGen && k < inBusy.count { inBusy[k] = false }; lock.unlock()
                inFlight.signal()
            }
        }

        var inputs: [VPKTAAEyeInput] = []
        var dDesc = ["none", "none"], mDesc = ["none", "none"]
        for e in 0..<2 {
            var d: VPKTAADepthInput? = nil, m: VPKTAAMotionInput? = nil
            var dd = "rcas-only", md = "rcas-only"
            if taaFrame {   // RCAS alone needs neither depth nor motion
                (d, dd, _) = depthInput(e, r, stage: stage, tag: 0, allowMS: true)
                (m, md) = motionInput(e, r)
            }
            dDesc[e] = dd; mDesc[e] = md
            // flipY: a Vulkan eye image has its origin at the top left (kl_glfb notes the same).
            guard let inp = makeInput(eye: e, color: srcs[e].0, slice: srcs[e].1, flipY: kl_vulkan_eye_color_top_left(Int32(stage), Int32(e)) != 0,
                                      rendered: r, depth: d, motion: m) else { return "eye \(e) input" }
            inputs.append(inp)
        }

        let job = InlineJob(k: k, gen: gen, pose: r, vkSerial: vkSerial, taa: taaFrame)
        let committedAt = Date()
        // VPKTAA nativo: VPKEngine's own pass (its jitter, both eyes in one dispatch, UI mask; RCAS only on rcas_only frames).
        // The only resolver: without it the frame goes out unresolved.
        guard (taaFrame ? kl_vulkan_taa_native_on() : kl_vulkan_rcas_on()) != 0 else {
            return taaFrame ? "VPKTAA nativo off" : "RCAS off"
        }
        inBottomUp = kl_vulkan_taa_eye_bottom_up() != 0
        var nf = nativeFrame(inputs, output: out.both)
        nf.rcas_only = taaFrame ? 0 : 1
        let route = kl_vulkan_guest_active() != 0 ? "Vulkan" : "vpgles"
        let maskS = kl_vulkan_taa_ui_mask_on() != 0 ? "sí" : "no"
        if taaFrame && !inNativeSaid {
            inNativeSaid = true
            NSLog("%@", "VPKTAA nativo activo (máscara UI: \(maskS)) "
                  + "— ruta \(route), ojo \(w)x\(h), \(Int(percent)) %")
        }
        if taaFrame != inRcasTaa || !inRcasSaid {
            inRcasSaid = true
            inRcasTaa = taaFrame
            NSLog("%@", "RCAS activo (nitidez \(String(format: "%.2f", KleptonTAA.rcasIntensity)), máscara UI \(maskS), "
                  + "ruta \(route) + gancho de fin de frame ovrp/OpenXR, \(taaFrame ? "VPKTAA; el RCAS de estos frames lo aplica el compositor" : "sin VPKTAA: solo RCAS"), ojo \(w)x\(h))")
        }
        let box = InlineEncode { [self] cmd in
            // First: whatever is encoded, the command buffer now owns slot and semaphore.
            cmd.addCompletedHandler { [self] cb in self.inlineCompleted(job, cb, committedAt) }
            owned = true
            let ok = withUnsafePointer(to: &nf) {
                kl_vulkan_taa_native_encode(UnsafeRawPointer($0), Unmanaged.passUnretained(cmd).toOpaque())
            }
            if ok != 0 { job.ok = true }
        }
        let committed = withExtendedLifetime(box) { () -> Int32 in
            kl_vulkan_native_submit({ cb, user in
                guard let cb, let user else { return }
                let b = Unmanaged<InlineEncode>.fromOpaque(user).takeUnretainedValue()
                b.body(Unmanaged<MTLCommandBuffer>.fromOpaque(cb).takeUnretainedValue() as MTLCommandBuffer)
            }, Unmanaged.passUnretained(box).toOpaque())
        }
        noteInlineInputs(w: w, h: h, applied: applied != nil, depth: dDesc, motion: mDesc)
        guard committed == 1 else { return "native submit refused (no VPKEngine queue)" }
        guard job.ok else { return "encode failed" }
        for e in 0..<2 {
            if let d = inputs[e].depth {
                inDepth[e] += 1
                if d.texture.sampleCount > 1 { inDepthMS[e] += 1 }
            }
            if inputs[e].motion != nil { inMV[e] += 1 }
        }
        return nil
    }

    private var inNativeSaid = false
    private var inRcasSaid = false, inRcasTaa = true
    private var inNativeEye = SIMD2<Int>(0, 0)

    /// VPKTAA nativo: the frame description VPKEngine's pass takes, from the inputs
    /// gathered (pose, unjittered tangents, crop, live depth incl. MSAA, AppSW
    /// motion). Texture pointers are unretained: the inputs outlive the synchronous encode.
    private func nativeFrame(_ inputs: [VPKTAAEyeInput], output: MTLTexture) -> kl_vulkan_taa_native_frame {
        var f = kl_vulkan_taa_native_frame()
        func eye(_ i: VPKTAAEyeInput) -> kl_vulkan_taa_native_eye {
            var e = kl_vulkan_taa_native_eye()
            e.color = Unmanaged.passUnretained(i.color).toOpaque()
            e.color_slice = UInt32(i.slice)
            if let d = i.depth {
                e.depth = Unmanaged.passUnretained(d.texture).toOpaque()
                e.depth_slice = UInt32(d.slice)
            }
            if let m = i.motion {
                e.motion = Unmanaged.passUnretained(m.texture).toOpaque()
                e.motion_slice = UInt32(m.slice)
                e.motion_scale = (m.scale.x, m.scale.y)
            }
            let q = i.orientation.vector
            e.orientation = (q.x, q.y, q.z, q.w)
            e.position = (i.position.x, i.position.y, i.position.z)
            // {left,right,top,bottom}; bottom-up memory: t' = -b, b' = -t (the reprojection is linear).
            e.tangents = inBottomUp ? (i.tangents.x, i.tangents.y, -i.tangents.w, -i.tangents.z)
                                    : (i.tangents.x, i.tangents.y, i.tangents.z, i.tangents.w)
            e.crop = (i.crop.x, i.crop.y, i.cropOff.x, i.cropOff.y)
            return e
        }
        f.eye = (eye(inputs[0]), eye(inputs[1]))
        f.eye.0.color_base = inBase[0]
        f.eye.1.color_base = inBase[1]
        f.output = Unmanaged.passUnretained(output).toOpaque()
        f.output_srgb = 1                 // `output` is the unorm view of the sRGB ring texture
        let d = inputs[0].depth
        f.reversed_z = (d?.reversedZ ?? true) ? 1 : 0
        f.near_z = d?.near ?? nearDefault
        f.far_z = d?.far ?? farDefault
        // A new eye size starts the history again (the engine also checks the size).
        let sz = SIMD2<Int>(inputs[0].color.width, inputs[0].color.height)
        f.reset = sz != inNativeEye ? 1 : 0
        inNativeEye = sz
        return f
    }

    /// A resolve's command buffer completed (Metal's thread): publish its output with its own
    /// frame record, free the slot, and advance kl_vulkan's serial to that frame.
    private func inlineCompleted(_ job: InlineJob, _ cb: MTLCommandBuffer, _ committedAt: Date) {
        let ms = max(0, cb.gpuEndTime - cb.gpuStartTime) * 1000
        let late = Date().timeIntervalSince(committedAt) * 1000
        let done = cb.status == .completed
        lock.lock()
        if job.gen == inGen && job.k < inBusy.count {
            inBusy[job.k] = false
            if job.ok && done && job.pose.serial > (inPublished?.pose.serial ?? 0) {
                let o = inOuts[job.k]
                inPublished = Published(slot: job.k, pose: job.pose, out: o.reads, flipY: [kl_vulkan_eye_color_top_left(Int32(job.pose.stage), 0) != 0,
                                                             kl_vulkan_eye_color_top_left(Int32(job.pose.stage), 1) != 0], taa: job.taa)
            }
        }
        if !done { inGpuErrors += 1 }
        if done { gpuMsSum += ms; gpuMsN += 1; gpuMsMax = max(gpuMsMax, ms) }
        inLateMs += late; inLateMax = max(inLateMax, late); inLateN += 1
        lock.unlock()
        inFlight.signal()
        // The frame is complete on the GPU (the resolve ran after it on the same queue).
        if job.ok { kl_vulkan_taa_frame_complete(job.vkSerial) }
    }

    private var inLastInputsKey = ""
    private var inLastInputsLog = Date.distantPast
    private var inLastStats = Date()

    /// [taa] inputs line on a change (at most every 5 s) and with every stats line.
    private func noteInlineInputs(w: Int, h: Int, applied: Bool, depth: [String], motion: [String]) {
        let key = "\(applied)|\(depth[0].prefix(24))|\(depth[1].prefix(24))|\(motion[0].prefix(12))|\(motion[1].prefix(12))"
        let now = Date()
        let due = now.timeIntervalSince(inLastStats) > 30
        guard (key != inLastInputsKey && now.timeIntervalSince(inLastInputsLog) > 5) || due else { return }
        inLastInputsKey = key
        inLastInputsLog = now
        NSLog("%@", "[taa] inputs (inline) \(w)x\(h) jitter=\(applied ? "applied" : "none") "
              + "pct=\(Int(percent)) rcas=\(String(format: "%.2f", KleptonTAA.rcasIntensity)) | eye0 depth=\(depth[0]) mv=\(motion[0]) "
              + "| eye1 depth=\(depth[1]) mv=\(motion[1])")
    }

    /// The inline stats line, every 30 s (guest thread, under coreLock).
    private func inlineStats() {
        let now = Date()
        guard now.timeIntervalSince(inLastStats) > 30 else { return }
        inLastStats = now
        lock.lock()
        let gpuAvg = gpuMsN > 0 ? gpuMsSum / Double(gpuMsN) : 0
        let gpuMax = gpuMsMax, gpuN = gpuMsN
        let lateAvg = inLateN > 0 ? inLateMs / Double(inLateN) : 0, lateMax = inLateMax
        let shown = (inShowResolved, inShowRaw, inShowOld, inShowNone), gpuErr = inGpuErrors
        gpuMsSum = 0; gpuMsN = 0; gpuMsMax = 0; inLateMs = 0; inLateMax = 0; inLateN = 0
        inShowResolved = 0; inShowRaw = 0; inShowOld = 0; inShowNone = 0; inGpuErrors = 0
        lock.unlock()
        // Built in short statements: one long interpolated "+" chain can exceed the type
        // checker's budget ("expression too complex").
        let gpuS = String(format: "%.2f", gpuAvg), gpuMaxS = String(format: "%.1f", gpuMax)
        let lateS = String(format: "%.1f", lateAvg), lateMaxS = String(format: "%.1f", lateMax)
        let jxS = String(format: "%+.3f", inLastJ.x), jyS = String(format: "%+.3f", inLastJ.y)
        var line = "[taa] stats (30 s, inline): frames=\(inFrames) resolved=\(inResolved) "
        line += "gpu=\(gpuS) ms/frame (max \(gpuMaxS), n=\(gpuN), errors=\(gpuErr)) "
        line += "commit->complete=\(lateS) ms (max \(lateMaxS)) "
        line += "| composites: resolved=\(shown.0) raw=\(shown.1) older-resolve=\(shown.2) none/hidden=\(shown.3) "
        line += "| jitter: frames=\(inJitFrames) draws jittered=\(inJittered) skipped=\(inSkipped) "
        line += "mixed=\(inMixed) (in \(inMixedFrames) frames) "
        line += "last=(\(jxS),\(jyS)) px of \(inLastTarget.0)x\(inLastTarget.1) "
        for e in 0..<2 {
            line += " | eye\(e): frames depth=\(inDepth[e]) (ms=\(inDepthMS[e])) mv=\(inMV[e])"
        }
        if !inRawWhy.isEmpty {
            line += " | unresolved: " + inRawWhy.sorted { $0.value > $1.value }
                .map { "\($0.key)=\($0.value)" }.joined(separator: ", ")
        }
        NSLog("%@", line)
        inFrames = 0; inResolved = 0; inJitFrames = 0; inMixedFrames = 0
        inJittered = 0; inSkipped = 0; inMixed = 0
        inDepth = [0, 0]; inDepthMS = [0, 0]; inMV = [0, 0]
        inRawWhy.removeAll()
    }

    // ── Automatic AA: detection persisted for the app (contract) ─────────────────────────────
    nonisolated(unsafe) private static var aaSaved = 0
    nonisolated(unsafe) private static var aaSaidNoApi = false
    nonisolated(unsafe) private static var aaSavedMax = 0

    /// Compositor thread, every frame, whether or not VPKTAA is on: once VPKEngine has seen the
    /// game's eye pass, store its ORIGINAL sample count for the app (vq.aaDetected.<key>) and
    /// the device's maximum (vq.msaaDeviceMax), on the first detection and on every change.
    static func pollAADetection() {
        guard kl_vulkan_guest_active() != 0 else { return }
        var dmax: Int32 = -1
        let det = Int(kl_vulkan_aa_detected(&dmax))
        if det < 0 {
            if !aaSaidNoApi {
                aaSaidNoApi = true
                NSLog("[aa] the Vulkan driver has no vpvk_msaa_detected (MoltenVK?) — eye MSAA unknown, "
                      + "auto mode runs no VPKTAA; nothing stored")
            }
            return
        }
        if dmax > 0 && Int(dmax) != aaSavedMax {
            aaSavedMax = Int(dmax)
            UserDefaults.standard.set(Int(dmax), forKey: "vq.msaaDeviceMax")
        }
        guard det > 0, det != aaSaved else { return }
        let before = aaSaved
        aaSaved = det
        let key = envS("VQ_GAME_KEY") ?? ""
        if !key.isEmpty { UserDefaults.standard.set(det, forKey: "vq.aaDetected." + key) }
        let mode = envS("KL_AA_MODE") ?? "auto"
        let result: String
        switch mode {
        case "vpktaa": result = "vpktaa (game MSAA forced to 1x + VPKTAA)"
        case "msaa": result = "msaa (game's own AA, no VPKTAA)"
        case "off": result = "off (no VPKTAA, no MSAA override)"
        default: result = det == 1 ? "auto: VPKTAA inline" : "auto: game MSAA \(det)x kept, VPKTAA off"
        }
        NSLog("%@", "[aa] detected eye MSAA \(det)x -> mode \(result)"
              + (before > 0 ? " (changed from \(before)x)" : "")
              + " | device max \(dmax)x | stored vq.aaDetected.\(key.isEmpty ? "<no VQ_GAME_KEY: not stored>" : key)")
    }
}
