// VPKTAA in the compositor: the Klepton adapter around VPKTAACore (VPKTAACore.swift, the
// self-contained TAA module). This file owns everything Klepton-specific:
//   * the ASYNC queue: each new guest frame is resolved on VPKTAA's own MTLCommandQueue into
//     one of 3 output slots; the compositor shows the newest COMPLETED slot with the pose of the
//     guest frame it came from, so a slow resolve costs one guest frame of latency, never a
//     missed display deadline (log 32). One job in flight; slots sampled by a composite still
//     on the GPU are pinned;
//   * building VPKTAAEyeInput from the frame record (kl_ovrp_render_pose: head pose, UNjittered
//     tangents, Halton jitter, per-eye viewport crop), the eye offsets the guest was told
//     (kl_ovrp_eye_local), the eye DEPTH kl_vulkan picked for that very frame
//     (kl_vulkan_eye_depth_get, tagged with the record serial) and the AppSW motion vectors
//     when the guest wrote them this frame (rendered.asw & KL_OVRP_ASW_MV);
//   * conventions: reversed-Z from kl_vulkan (eye-pass clear 0.0 / compare-op census), near
//     from the submit (KL_VPKTAA_NEARFAR_OFF) else KL_VPKTAA_NEAR (0.05 m), far
//     KL_VPKTAA_FAR (0 = infinite); MV scale KL_VPKTAA_MV_SCALE (0.5: NDC delta -> uv) with the
//     sign KL_VPKTAA_MV_SIGN = auto (default: starts +x+y, follows the shader's sign vote) |
//     1 | -1, KL_VPKTAA_MV_YFLIP=1 flips y;
//   * diagnostics: [taa] inputs per eye + stats every 30 s (gpu ms, depth-valid %, disocclusion,
//     mean |mv|, sign votes).
// Settings from the app as environment (KL_VPKTAA=1, KL_VPKTAA_PCT, KL_VPKTAA_SHARP).
import Metal
import simd
import Foundation

final class KleptonTAA {
    static let enabled: Bool = getenv("KL_VPKTAA").map { String(cString: $0) == "1" } ?? false
    private static func envS(_ k: String) -> String? {
        guard let v = getenv(k) else { return nil }
        return String(cString: v)
    }
    private static func envF(_ k: String, _ d: Float) -> Float {
        guard let s = envS(k), let f = Float(s) else { return d }
        return f
    }
    private let percent = min(max(KleptonTAA.envF("KL_VPKTAA_PCT", 80), 0), 100)
    private let sharp = min(max(KleptonTAA.envF("KL_VPKTAA_SHARP", 0.3), 0), 1)
    private let nearDefault = max(KleptonTAA.envF("KL_VPKTAA_NEAR", 0.05), 0.001)
    private let farDefault = max(KleptonTAA.envF("KL_VPKTAA_FAR", 0), 0)
    private let mvScaleMag = KleptonTAA.envF("KL_VPKTAA_MV_SCALE", 0.5)
    private let mvAuto: Bool
    private var mvSign = SIMD2<Float>(1, 1)
    private let revZOverride: Int = {   // -1 = from kl_vulkan, 0/1 forced
        guard let s = KleptonTAA.envS("KL_VPKTAA_REVZ"), let v = Int(s) else { return -1 }
        return v == 0 ? 0 : 1
    }()
    private let useMV = KleptonTAA.envS("KL_VPKTAA_MV") != "0"
    private let mvMap = KleptonTAA.envS("KL_VPKTAA_MV_MAP") ?? "auto"
    private let depthMap = KleptonTAA.envS("KL_VPKTAA_DEPTH_MAP") ?? "auto"
    private let depthWaitMs = Double(KleptonTAA.envF("KL_VPKTAA_DEPTH_WAIT_MS", 25))
    // Jitter/frame pairing: the frame record carries the LAST frustum offset kl_ovrp served; a
    // guest that reads the frustum for the NEXT frame while the GPU still renders this one (UE4
    // game thread one frame ahead of the render thread) renders frame N with the offset of
    // record N−1 (lag −1), or the reverse (lag +1). Wrongly paired jitter = every frame
    // un-shifted by the wrong sub-pixel amount: shimmer + blur. KL_VPKTAA_JITTER_LAG =
    // auto (default: starts 0, follows the shader's vote) | -1 | 0 | 1.
    private let jitterLagAuto: Bool
    // Spatial edge AA: KL_VPKTAA_SPATIAL = auto (default: on until the jitter votes PROVE the
    // guest renders with the paired jitter, i.e. "used" wins >= 50 %) | 0 | 1.
    private let spatialMode = KleptonTAA.envS("KL_VPKTAA_SPATIAL") ?? "auto"
    private var jitterLag = 0
    private var jitterLagUnknown = 0          // records whose jitter is not a Halton(2,3) x8 entry

    private let device: MTLDevice
    private let core: VPKTAACore

    struct Published {
        let slot: Int
        let pose: kl_ovrp_render_pose
        let out: [MTLTexture]         // per eye, 2D array slice 0, rgba8 sRGB
        let flipY: [Bool]
    }
    private let queue: MTLCommandQueue?
    private let lock = NSLock()
    private var slots: [[MTLTexture]] = []      // slots[k][eye]
    private var slotSize = (0, 0)
    private var pins: [Int] = []
    private var published: Published?
    private var inflight = false
    private var inflightSince = Date()
    private var submittedSerial: UInt64 = 0
    private var stuckSaid = false
    private var generation = 0
    private var gpuMsSum = 0.0, gpuMsN = 0, gpuMsMax = 0.0
    private var writeViews: [ObjectIdentifier: MTLTexture] = [:]
    // Depth gating: a frame record can be read before kl_vulkan filed (and tagged) its depth.
    private var lastSubmitAt = Date.distantPast
    private var lastDepthOK = Date.distantPast
    // Stats / diagnostics.
    private var rawFrames = 0, lastRawSerial: UInt64 = 0   // guest frames shown without TAA
    private var resolved = 0, depthFrames = [0, 0], mvFrames = [0, 0], depthLate = 0
    private var lastStats = Date()
    private var lastInputsKey = ""
    private var lastInputsLog = Date.distantPast
    private var lastMode: Int32 = -1
    private var resetsLogged = 0
    private var saidCropOff = [false, false]

    init?(device: MTLDevice) {
        guard KleptonTAA.enabled, let c = VPKTAACore(device: device) else { return nil }
        let sign = KleptonTAA.envS("KL_VPKTAA_MV_SIGN") ?? "auto"
        self.device = device
        self.core = c
        self.mvAuto = sign == "auto"
        let lag = KleptonTAA.envS("KL_VPKTAA_JITTER_LAG") ?? "auto"
        self.jitterLagAuto = Int(lag) == nil
        self.queue = device.makeCommandQueue()
        // Every stored property is set from here on.
        queue?.label = "VPKTAA"
        c.historyPercent = percent
        c.sharpness = sharp
        c.spatial = spatialMode == "0" ? 0 : 1
        c.disocclusion = min(max(KleptonTAA.envF("KL_VPKTAA_DISOCC", 0.1), 0.02), 1)
        if sign == "-1" { mvSign = SIMD2<Float>(-1, -1) }
        if let l = Int(lag) { jitterLag = max(-2, min(2, l)) }
        if KleptonTAA.envS("KL_VPKTAA_MV_YFLIP") == "1" { mvSign.y = -mvSign.y }
    }

    private static func halton(_ i: Int, _ b: Int) -> Float {
        var f: Float = 1, r: Float = 0, i = i
        while i > 0 { f /= Float(b); r += f * Float(i % b); i /= b }
        return r
    }

    /// kl_ovrp's offset k (1...8) of the sequence, in tangent units for this frustum/size.
    private static func haltonJitter(_ k: Int, _ tan: SIMD4<Float>, _ w: Int, _ h: Int) -> SIMD2<Float> {
        let kk = ((k - 1) % 8 + 8) % 8 + 1
        return SIMD2((halton(kk, 2) - 0.5) * (tan.x + tan.y) / Float(w),
                     (halton(kk, 3) - 0.5) * (tan.z + tan.w) / Float(h))
    }

    /// (jitter to use, [previous, next] alternatives) from the record's jitter and the lag.
    /// Same formula as klovrp_jit_frustum (pixels of the eye storage = colour texture size).
    private func pairedJitter(_ j: SIMD2<Float>, _ tan: SIMD4<Float>, _ w: Int, _ h: Int)
        -> (SIMD2<Float>, [SIMD2<Float>]) {
        var bestK = 0
        var bestE: Float = 1e9
        for k in 1...8 {
            let c = KleptonTAA.haltonJitter(k, tan, w, h)
            let d = (c - j) * SIMD2(Float(w) / max(tan.x + tan.y, 1e-6), Float(h) / max(tan.z + tan.w, 1e-6))
            let e = abs(d.x) + abs(d.y)
            if e < bestE { bestE = e; bestK = k }
        }
        guard bestE < 0.02 else { jitterLagUnknown += 1; return (j, []) }   // not ours: as is
        let k = bestK + jitterLag
        return (KleptonTAA.haltonJitter(k, tan, w, h),
                [KleptonTAA.haltonJitter(k - 1, tan, w, h), KleptonTAA.haltonJitter(k + 1, tan, w, h)])
    }

    /// Output picture: rgba8 sRGB, written by the kernels through a non-sRGB view with the
    /// encode in the shader, sampled sRGB by the composite.
    private func makeOut(_ w: Int, _ h: Int, _ label: String) -> (MTLTexture, MTLTexture)? {
        let d = MTLTextureDescriptor()
        d.textureType = .type2DArray
        d.pixelFormat = .rgba8Unorm_srgb
        d.width = w; d.height = h; d.arrayLength = 1
        d.usage = [.shaderRead, .shaderWrite, .pixelFormatView]
        d.storageMode = .private
        guard let t = device.makeTexture(descriptor: d),
              let v = t.makeTextureView(pixelFormat: .rgba8Unorm) else { return nil }
        t.label = label
        return (t, v)
    }

    /// The newest finished resolve, pinned until `cmd` (the composite that samples it) completes.
    /// nil when it is 2+ guest frames behind `newestSerial`: the compositor then shows the
    /// guest's own newest picture, so VPKTAA never drops the shown cadence below the game's
    /// (one frame behind = the resolve of the newest frame is still running: normal latency).
    func acquire(for cmd: MTLCommandBuffer, newestSerial: UInt64 = 0) -> Published? {
        lock.lock(); defer { lock.unlock() }
        guard let p = published, p.slot < pins.count else { return nil }
        if newestSerial > p.pose.serial + 1 {
            if lastRawSerial != newestSerial { lastRawSerial = newestSerial; rawFrames += 1 }
            return nil
        }
        pins[p.slot] += 1
        let slot = p.slot
        cmd.addCompletedHandler { [weak self] _ in
            guard let self else { return }
            self.lock.lock(); if slot < self.pins.count { self.pins[slot] -= 1 }; self.lock.unlock()
        }
        return p
    }

    /// True while VPKTAA is producing pictures (jitter must stay on).
    var running: Bool { lock.lock(); defer { lock.unlock() }; return published != nil || inflight }

    /// Drop everything shown (guest eye layer gone / size change): the next resolve starts clean.
    func invalidate() {
        lock.lock(); published = nil; generation += 1; lock.unlock()
        core.invalidate()
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

    /// Depth of `eye` for exactly this frame record, or nil (+ why, for the log).
    private func depthInput(_ eye: Int, _ r: kl_ovrp_render_pose) -> (VPKTAADepthInput?, String, Bool) {
        var info = kl_vulkan_eye_depth()
        let ok = kl_vulkan_eye_depth_get(Int32(r.stage), Int32(eye), r.serial, &info) != 0
        if !ok { return (nil, "none", false) }
        if info.samples > 1 { return (nil, "msaa\(info.samples)x(unusable)", true) }
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
        let desc = "\(src) \(info.snapshot != 0 ? "copy" : "live") \(KleptonTAA.fmtName(info.vk_format)) "
            + "\(info.w)x\(info.h) s\(info.slice) revZ=\(rev ? 1 : 0)"
            + (revZOverride >= 0 ? "(forced)" : info.clear_depth >= 0
                ? "(clear \(String(format: "%.3f", info.clear_depth)))" : "(census)")
            + " near=\(String(format: "%.3f", near))\(fromSubmit ? "(submit)" : "(default)")"
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
                "live \(w)x\(h) s\(sl) scale=(\(String(format: "%+.2f", s.x)),\(String(format: "%+.2f", s.y)))"
                + (mvAuto ? "auto" : ""))
    }

    /// Resolve one guest frame (both eyes) on the VPKTAA queue, if a new one is ready and no
    /// resolve is in flight. `newest` is the compositor's newest complete frame record;
    /// `sources(stage)` gives the eye textures of a stage. `wait` orders the job after the
    /// guest's GPU work (GL guests).
    ///
    /// Which frame: once the guest's depth has been arriving, the newest record whose depth
    /// kl_vulkan has already filed AND tagged (it does so after the guest's queue went idle at
    /// its frame end, so that frame's eye picture is complete too) — usually `newest` or the one
    /// before it. A record is read before its tag exists whenever the compositor looks during
    /// the guest's frame-end wait; without this choice those frames would resolve without depth.
    /// If no tagged record turns up for KL_VPKTAA_DEPTH_WAIT_MS, the newest goes without depth.
    func submit(newest: kl_ovrp_render_pose, wait: (MTLSharedEvent, UInt64)?,
                sources: (Int) -> [(MTLTexture, Int, Bool)]?) {
        guard queue != nil, newest.serial != 0 else { return }
        lock.lock()
        if inflight {
            let age = Date().timeIntervalSince(inflightSince)
            lock.unlock()
            if age > 2 && !stuckSaid {
                stuckSaid = true
                NSLog("[taa] resolve in flight for \(String(format: "%.1f", age)) s — guest fence not signalled?")
            }
            return
        }
        let done = submittedSerial
        lock.unlock()
        if newest.serial <= done { return }

        var rendered = newest
        var dIn: [VPKTAADepthInput?] = [nil, nil]
        var dDesc = ["none", "none"]
        let depthActive = Date().timeIntervalSince(lastDepthOK) < 2
        var found = false
        // Candidates newest-first: every complete, not-yet-resolved record of every stage.
        var cands: [kl_ovrp_render_pose] = [newest]
        let n = Int(kl_ovrp_stage_count())
        for st in 0..<max(0, min(n, 16)) {
            var r = kl_ovrp_render_pose()
            if withUnsafeMutablePointer(to: &r, { kl_ovrp_stage_render_pose(Int32(st), $0) }) != 0,
               r.complete != 0, r.serial > done, r.serial != newest.serial { cands.append(r) }
        }
        cands.sort { $0.serial > $1.serial }
        for r in cands {
            var dd: [VPKTAADepthInput?] = [nil, nil]
            var ds = ["none", "none"]
            var seen = false
            for e in 0..<2 {
                let (d, desc, sn) = depthInput(e, r)
                dd[e] = d; ds[e] = desc; seen = seen || sn
            }
            if seen || !depthActive {
                rendered = r; dIn = dd; dDesc = ds; found = true
                break
            }
        }
        if !found {
            // Depth has been arriving but no record carries it yet: retry on the next tick,
            // up to KL_VPKTAA_DEPTH_WAIT_MS after the previous resolve, then resolve the newest
            // without it (rotation-only for this frame).
            if Date().timeIntervalSince(lastSubmitAt) * 1000 < depthWaitMs { return }
            depthLate += 1
            rendered = newest
        }
        if dIn[0] != nil || dIn[1] != nil { lastDepthOK = Date() }
        guard let srcList = sources(Int(rendered.stage)), srcList.count == 2, let queue else { return }

        lock.lock()
        if inflight || rendered.serial == submittedSerial { lock.unlock(); return }
        let w = srcList[0].0.width, h = srcList[0].0.height
        if slotSize != (w, h) {
            // Resize: only once nothing samples the old slots.
            if pins.contains(where: { $0 > 0 }) { lock.unlock(); return }
            published = nil
            slots = []; pins = []; writeViews.removeAll()
            for k in 0..<3 {
                var pair: [MTLTexture] = []
                for e in 0..<2 {
                    guard let (t, v) = makeOut(w, h, "vpktaa out \(k)/\(e)") else { lock.unlock(); return }
                    writeViews[ObjectIdentifier(t)] = v
                    pair.append(t)
                }
                slots.append(pair); pins.append(0)
            }
            slotSize = (w, h)
        }
        // A slot that is neither shown (published) nor sampled by a composite on the GPU.
        guard let slot = (0..<slots.count).first(where: { $0 != published?.slot && pins[$0] == 0 }) else {
            lock.unlock(); return
        }
        inflight = true; inflightSince = Date()
        lastSubmitAt = inflightSince
        submittedSerial = rendered.serial
        let gen = generation
        let outs = slots[slot]
        lock.unlock()

        guard let cmd = queue.makeCommandBuffer() else { lock.lock(); inflight = false; lock.unlock(); return }
        cmd.label = "VPKTAA \(rendered.serial)"
        if let (ev, v) = wait { cmd.encodeWaitForEvent(ev, value: v) }
        var ok = true
        var mDesc = ["none", "none"]
        for eye in 0..<2 {
            let (src, slice, flip) = srcList[eye]
            guard let outView = writeViews[ObjectIdentifier(outs[eye])] else { ok = false; break }
            let (m, md) = motionInput(eye, rendered)
            mDesc[eye] = md
            guard let input = makeInput(eye: eye, color: src, slice: slice, flipY: flip,
                                        rendered: rendered, depth: dIn[eye], motion: m) else { ok = false; break }
            if !core.encode(cmd: cmd, eye: eye, input: input, output: outView) { ok = false; break }
            if input.depth != nil { depthFrames[eye] += 1 }
            if input.motion != nil { mvFrames[eye] += 1 }
            if let why = core.depthRefused[eye] { dDesc[eye] += " REFUSED: \(why)" }
            if let why = core.motionRefused[eye] { mDesc[eye] += " REFUSED: \(why)" }
        }
        let pub = Published(slot: slot, pose: rendered, out: outs, flipY: [srcList[0].2, srcList[1].2])
        let okFinal = ok
        cmd.addCompletedHandler { [weak self] cb in
            guard let self else { return }
            self.lock.lock()
            if okFinal && cb.status == .completed && gen == self.generation { self.published = pub }
            let ms = max(0, cb.gpuEndTime - cb.gpuStartTime) * 1000
            self.gpuMsSum += ms; self.gpuMsN += 1; self.gpuMsMax = max(self.gpuMsMax, ms)
            self.inflight = false
            self.lock.unlock()
        }
        cmd.commit()
        if ok { resolved += 1 }
        noteInputs(rendered: rendered, w: w, h: h, depth: dDesc, motion: mDesc)
    }

    private func makeInput(eye: Int, color: MTLTexture, slice: Int, flipY: Bool,
                           rendered r: kl_ovrp_render_pose,
                           depth: VPKTAADepthInput?, motion: VPKTAAMotionInput?) -> VPKTAAEyeInput? {
        guard color.textureType == .type2DArray else { return nil }
        let w = color.width, h = color.height
        let t = r.tangents
        let tan: SIMD4<Float> = eye == 0 ? SIMD4(t.0.0, t.0.1, t.0.2, t.0.3) : SIMD4(t.1.0, t.1.1, t.1.2, t.1.3)
        let j = r.jitter
        var jit: SIMD2<Float> = r.jitter_mode == 1 ? (eye == 0 ? SIMD2(j.0.0, j.0.1) : SIMD2(j.1.0, j.1.1)) : .zero
        var jitAlt: [SIMD2<Float>] = []
        if jit != .zero { (jit, jitAlt) = pairedJitter(jit, tan, w, h) }
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
        return VPKTAAEyeInput(color: color, slice: slice, flipY: flipY, tangents: tan, jitter: jit,
                              crop: crop, cropOff: cropOff, orientation: qh * qe, position: pos,
                              depth: dep, motion: mot, jitterAlt: jitAlt)
    }

    private func pct(_ a: UInt32, _ b: UInt32) -> String {
        String(format: "%.1f", b > 0 ? Double(a) / Double(b) * 100 : 0)
    }

    /// One [taa] line when the inputs change, and the stats line every 30 s.
    private func noteInputs(rendered r: kl_ovrp_render_pose, w: Int, h: Int, depth: [String], motion: [String]) {
        let mode = r.jitter_mode
        let key = "\(mode)|\(depth[0].prefix(24))|\(depth[1].prefix(24))|\(motion[0].prefix(12))|\(motion[1].prefix(12))"
        let now = Date()
        let due = now.timeIntervalSince(lastStats) > 30
        // On a change (at most every 5 s: AC writes its motion vectors in ~85 % of frames, so
        // "live"/"not-written" alternate) and with every stats line.
        if (key != lastInputsKey && now.timeIntervalSince(lastInputsLog) > 5) || due {
            lastInputsKey = key
            lastInputsLog = now
            lastMode = mode
            // "%@": the message carries '%' signs, which NSLog would read as conversions.
            NSLog("%@", "[taa] inputs \(w)x\(h) mode=\(mode == 1 ? "jitter" : mode == 0 ? "nojitter(probing)" : "nojitter") "
                  + "pct=\(Int(percent)) rcas=\(sharp) | eye0 depth=\(depth[0]) mv=\(motion[0]) "
                  + "| eye1 depth=\(depth[1]) mv=\(motion[1])")
        }
        guard due else { return }
        lastStats = now
        lock.lock()
        let gpuAvg = gpuMsN > 0 ? gpuMsSum / Double(gpuMsN) : 0
        let gpuMax = gpuMsMax
        gpuMsSum = 0; gpuMsN = 0; gpuMsMax = 0
        lock.unlock()
        var line = "[taa] stats (30 s, async queue): resolves=\(resolved) resets=\(core.resets - resetsLogged) "
            + "gpu=\(String(format: "%.2f", gpuAvg)) ms/frame (max \(String(format: "%.1f", gpuMax))) "
            + "depth-late=\(depthLate) raw-fallback=\(rawFrames) frames"
        var votes: [UInt32] = [0, 0, 0, 0]
        var jv: [UInt32] = [0, 0, 0]
        for e in 0..<2 {
            let s = core.takeStats(eye: e)
            for k in 0..<4 { votes[k] += s.votes[k] }
            for k in 0..<3 { jv[k] += s.jitterVotes[k] }
            line += " | eye\(e): frames depth=\(depthFrames[e]) mv=\(mvFrames[e]) "
                + "depthValid=\(pct(s.depthValid, s.pixels)) pct disocc=\(pct(s.disoccluded, s.pixels)) pct "
                + "rejected=\(pct(s.rejected, s.pixels)) pct offscreen=\(pct(s.offscreen, s.pixels)) pct "
                + "moving=\(pct(s.moving, s.pixels)) pct mean|mv|=\(String(format: "%.2f", s.meanMvPx)) px"
        }
        let tot = votes.reduce(0, +)
        line += " | mv sign votes (+x+y,+x-y,-x+y,-x-y)=\(votes[0]),\(votes[1]),\(votes[2]),\(votes[3]) "
            + "using (\(mvSign.x > 0 ? "+" : "-")x\(mvSign.y > 0 ? "+" : "-")y)"
        if mvAuto && tot >= 200, let best = (0..<4).max(by: { votes[$0] < votes[$1] }),
           Double(votes[best]) / Double(tot) >= 0.55 {
            let sg = SIMD2<Float>((best & 2) != 0 ? -1 : 1, (best & 1) != 0 ? -1 : 1)
            if sg != mvSign {
                mvSign = sg
                line += " -> AUTO switch to (\(sg.x > 0 ? "+" : "-")x\(sg.y > 0 ? "+" : "-")y)"
            }
        }
        // Jitter pairing: votes for the used offset / the previous one / the next one.
        let jt = jv.reduce(0, +)
        line += " | jitter votes (used,prev,next)=\(jv[0]),\(jv[1]),\(jv[2]) lag=\(jitterLag)"
            + (jitterLagAuto ? "(auto)" : "(forced)")
            + (jitterLagUnknown > 0 ? " non-halton=\(jitterLagUnknown)" : "")
        if spatialMode == "auto" && jt >= 300 {
            let proven = Double(jv[0]) / Double(jt) >= 0.5
            let want: Float = proven ? 0 : 1
            if want != core.spatial {
                core.spatial = want
                line += proven ? " -> jitter proven: spatial AA off" : " -> jitter NOT proven: spatial AA on"
            }
        }
        line += " spatial=\(core.spatial > 0 ? "on" : "off")"
        let rawNow = rawFrames
        if rawNow > resolved {
            line += " | WARN: TAA shown on only \(resolved * 100 / max(1, resolved + rawNow)) pct of guest frames "
                + "(GPU saturated, resolve cannot keep up): game MSAA is the cheaper AA here"
        }
        if jitterLagAuto && jt >= 300 {
            let alt = jv[1] >= jv[2] ? 1 : 2
            // Clear winner: alt is the top vote and leads "used" by 15 % -> paired one frame off.
            if jv[alt] > jv[0] && Double(jv[alt]) >= 1.15 * Double(jv[0]) {
                let nl = max(-2, min(2, jitterLag + (alt == 1 ? -1 : 1)))
                if nl != jitterLag {
                    jitterLag = nl
                    core.invalidate()
                    line += " -> AUTO jitter lag \(nl)"
                }
            }
        }
        jitterLagUnknown = 0
        NSLog("%@", line)
        lock.lock(); rawFrames = 0; lock.unlock()
        resolved = 0; depthFrames = [0, 0]; mvFrames = [0, 0]; depthLate = 0
        resetsLogged = core.resets
    }
}
