// VPKTAA in the compositor: temporal antialiasing of the guest's eye picture, per eye, at the
// guest's own resolution, before the reprojection pass places it on the display.
//
// Why here and not in the driver: this is the one place that has, for every engine and every
// graphics path, the finished per-eye image, the head pose it was rendered with and the
// previous frame's. One resolve per eye per GUEST frame (a guest frame shown twice is not
// resolved twice).
//
// Algorithm (Karis 2014 / UE4 TAA family):
//   * sub-pixel jitter: kl_ovrp folds a Halton(2,3) x8 offset into the frustum the guest reads
//     every frame (only once it has seen the guest re-read it every frame; kl_ovrp.c
//     klovrp_jit_*). The record keeps the UNjittered tangents (the composite's quad) plus the
//     offset; this pass reconstructs the current frame on the unjittered grid with a
//     Blackman-Harris-like 3x3 filter centred on the jittered sample positions;
//   * history reprojection by the head-rotation delta (rotation-only: the guest's depth is not
//     reachable — ovrp_GetLayerTexture2 hands out no depth storage), Catmull-Rom 5-tap;
//   * variance clipping in YCoCg (gamma 1.25 jitter / 1.0 no-jitter), clipped TOWARDS the AABB
//     centre; history weight lowered with the clip distance; offscreen -> per-pixel reset;
//   * Karis inverse-luma weighted blend (no fireflies/flicker);
//   * history keeps the unsharpened result; RCAS on the shown picture only.
// Modes: jitter (history weight 0.5..0.92 from the percent setting) or no-jitter (guest does
// not re-read the frustum: history capped at 0.5 — only light temporal smoothing).
// Linear light throughout (sRGB sources are read linear by Metal; history is rgba16f).
// Alpha rides along (passthrough games clear to alpha 0 where the room shows).
// Settings from the app as environment (KL_VPKTAA=1, KL_VPKTAA_PCT, KL_VPKTAA_SHARP).
import Metal
import simd
import Foundation

final class KleptonTAA {
    struct Params {
        var tanCur: SIMD4<Float>    // l r t b of the current frame, UNjittered (output grid)
        var tanPrev: SIMD4<Float>   // l r t b of the history frame (unjittered)
        var rot: simd_float3x3      // current view dir -> previous view dir (column-major)
        var jitter: SIMD2<Float>    // tangent offset the guest rendered with (dx right, dy up)
        var crop: SIMD2<Float>      // fraction of the texture the guest rendered (u, v)
        var historyWeight: Float
        var gamma: Float            // variance-clip box size, in sigmas
        var sharpness: Float        // RCAS strength 0..1
        var slice: UInt32           // source slice
        var flipY: UInt32           // 1: texture row 0 is the top of the picture
        var reset: UInt32
    }

    static let enabled: Bool = getenv("KL_VPKTAA").map { String(cString: $0) == "1" } ?? false
    private static func envF(_ k: String, _ d: Float) -> Float {
        guard let v = getenv(k), let f = Float(String(cString: v)) else { return d }
        return f
    }
    private let percent = min(max(KleptonTAA.envF("KL_VPKTAA_PCT", 80), 0), 100)
    private let sharp = min(max(KleptonTAA.envF("KL_VPKTAA_SHARP", 0.3), 0), 1)

    private let device: MTLDevice
    private var pipeline: MTLComputePipelineState?
    private var rcas: MTLComputePipelineState?
    // [0] = rejected (clipped hard) pixels, [1] = offscreen pixels, [2] = pixels resolved.
    private var statBuf: MTLBuffer?
    // Per eye: two history textures (ping-pong). The shown pictures live in `slots`.
    private var history: [[MTLTexture]] = [[], []]
    private var histSize = (0, 0)

    // ASYNC (log 32): the resolve used to be encoded into the COMPOSITOR's command buffer,
    // ~2x7.6 Mpx of compute ahead of the 100 Hz composite, and the compositor fell to a 20 ms
    // cadence (50 Hz, deadline missed) — judder and nausea. Now each new guest frame is
    // resolved on this class's OWN queue, into an output slot, and the compositor shows the
    // newest COMPLETED slot together with the pose of the guest frame it came from, so a
    // resolve that is not done yet costs one guest frame of latency (reprojected), never a
    // missed display deadline. One job in flight at most; slots in use by a composite
    // command buffer still on the GPU are pinned and never written.
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
    private var gpuMsSum = 0.0, gpuMsN = 0
    private var cur = [0, 0]
    private var lastSerial: [UInt64] = [0, 0]
    private var lastQuat: [simd_quatf?] = [nil, nil]
    private var lastTan: [SIMD4<Float>] = [.zero, .zero]
    private var lastFlip: [Bool] = [false, false]
    private var lastMode: Int32 = -1
    private var said = false
    // Stats.
    private var resolved = 0, resets = 0, reused = 0
    private var lastStats = Date()

    init?(device: MTLDevice) {
        guard KleptonTAA.enabled else { return nil }
        self.device = device
        do {
            let lib = try device.makeLibrary(source: KleptonTAA.msl, options: nil)
            guard let fn = lib.makeFunction(name: "vpktaa_resolve"),
                  let fs = lib.makeFunction(name: "vpktaa_rcas") else { return nil }
            pipeline = try device.makeComputePipelineState(function: fn)
            rcas = try device.makeComputePipelineState(function: fs)
        } catch {
            NSLog("[taa] VPKTAA shader failed to build: \(error) — VPKTAA off")
            return nil
        }
        statBuf = device.makeBuffer(length: 16, options: .storageModeShared)
        if let b = statBuf { memset(b.contents(), 0, 16) }
        queue = device.makeCommandQueue()
        queue?.label = "VPKTAA"
    }

    /// Output picture: rgba8 sRGB (30 MB per eye at 3072x2465 instead of 60 in rgba16f, three
    /// slots), written by the RCAS kernel through a non-sRGB view with the encode in the shader
    /// (sRGB storage writes are not something to rely on), sampled sRGB by the composite.
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
    private var writeViews: [ObjectIdentifier: MTLTexture] = [:]

    /// The newest finished resolve, pinned until `cmd` (the composite that samples it) completes.
    func acquire(for cmd: MTLCommandBuffer) -> Published? {
        lock.lock(); defer { lock.unlock() }
        guard let p = published, p.slot < pins.count else { return nil }
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
        lastQuat = [nil, nil]
    }

    /// Resolve guest frame `rendered.serial` (both eyes) on the VPKTAA queue, if it is new and
    /// no resolve is in flight. `wait` orders the job after the guest's GPU work (GL guests).
    func submit(sources: [(MTLTexture, Int, Bool)], rendered: kl_ovrp_render_pose,
                wait: (MTLSharedEvent, UInt64)?) {
        guard let queue, sources.count == 2, rendered.serial != 0 else { return }
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
        if rendered.serial == submittedSerial { lock.unlock(); return }
        let w = sources[0].0.width, h = sources[0].0.height
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
        submittedSerial = rendered.serial
        let gen = generation
        lock.unlock()

        guard let cmd = queue.makeCommandBuffer() else { lock.lock(); inflight = false; lock.unlock(); return }
        cmd.label = "VPKTAA \(rendered.serial)"
        if let (ev, v) = wait { cmd.encodeWaitForEvent(ev, value: v) }
        var ok = true
        for eye in 0..<2 {
            let (src, slice, flip) = sources[eye]
            if !resolve(cmd: cmd, eye: eye, source: src, slice: slice, flipY: flip,
                        rendered: rendered, out: slots[slot][eye]) { ok = false; break }
        }
        let pub = Published(slot: slot, pose: rendered, out: slots[slot],
                            flipY: [sources[0].2, sources[1].2])
        let okFinal = ok
        cmd.addCompletedHandler { [weak self] cb in
            guard let self else { return }
            self.lock.lock()
            if okFinal && cb.status == .completed && gen == self.generation { self.published = pub }
            self.gpuMsSum += max(0, cb.gpuEndTime - cb.gpuStartTime) * 1000; self.gpuMsN += 1
            self.inflight = false
            self.lock.unlock()
        }
        cmd.commit()
    }

    private func makeTex(_ w: Int, _ h: Int, _ label: String) -> MTLTexture? {
        let d = MTLTextureDescriptor()
        d.textureType = .type2DArray
        d.pixelFormat = .rgba16Float
        d.width = w; d.height = h; d.arrayLength = 1
        d.usage = [.shaderRead, .shaderWrite]
        d.storageMode = .private
        let t = device.makeTexture(descriptor: d)
        t?.label = label
        return t
    }

    /// Resolve one eye. Returns the texture (2D array, slice 0) the composite should sample in
    /// place of the guest's, or nil to sample the guest's own (VPKTAA unavailable this frame).
    /// The result is on the UNjittered grid of `rendered.tangents`, which is what the
    /// composite builds its quad from.
    private func resolve(cmd: MTLCommandBuffer, eye: Int, source: MTLTexture, slice: Int, flipY: Bool,
                         rendered: kl_ovrp_render_pose, out outTex: MTLTexture) -> Bool {
        guard let pipeline, let rcas, let statBuf, eye == 0 || eye == 1,
              source.textureType == .type2DArray,
              let outView = writeViews[ObjectIdentifier(outTex)] else { return false }
        let w = source.width, h = source.height
        var reset = false
        if history[eye].count != 2 || history[eye][0].width != w || history[eye][0].height != h {
            guard let h0 = makeTex(w, h, "vpktaa hist \(eye)a"),
                  let h1 = makeTex(w, h, "vpktaa hist \(eye)b") else { return false }
            history[eye] = [h0, h1]; cur[eye] = 0
            reset = true
        }
        let t = rendered.tangents
        let tan: SIMD4<Float> = eye == 0 ? SIMD4(t.0.0, t.0.1, t.0.2, t.0.3)
                                         : SIMD4(t.1.0, t.1.1, t.1.2, t.1.3)
        let j = rendered.jitter
        let mode = rendered.jitter_mode          // 1 = jittered frames
        let jit: SIMD2<Float> = mode == 1 ? (eye == 0 ? SIMD2(j.0.0, j.0.1) : SIMD2(j.1.0, j.1.1))
                                          : .zero
        let q = simd_quatf(ix: rendered.qx, iy: rendered.qy, iz: rendered.qz, r: rendered.qw).normalized
        var rot = matrix_identity_float3x3
        if let pq = lastQuat[eye] {
            // view-space dir d (current) -> world: q*d -> previous view: pq^-1 * q * d
            let delta = pq.inverse * q
            let angle = abs(delta.angle) > Float.pi ? 2 * Float.pi - abs(delta.angle) : abs(delta.angle)
            if angle > 8 * Float.pi / 180 { reset = true }
            rot = simd_float3x3(delta)
        } else { reset = true }
        if flipY != lastFlip[eye] { reset = true }
        // Unjittered tangents changing = a frustum change: history invalid.
        let dt = tan - lastTan[eye]
        if (dt * dt).max() > 1e-8 { reset = true }
        if mode != lastMode { reset = true }

        var crop = SIMD2<Float>(1, 1)
        let vp = eye == 0 ? rendered.viewport.0 : rendered.viewport.1
        if vp.2 > 0, vp.3 > 0, rendered.viewport_of.0 > 0, rendered.viewport_of.1 > 0 {
            crop = SIMD2(min(1, Float(vp.2) / Float(rendered.viewport_of.0)),
                         min(1, Float(vp.3) / Float(rendered.viewport_of.1)))
        }

        // percent 0..100 -> max history weight 0.5..0.92 (jitter); capped at 0.5 without jitter,
        // where the history holds no new samples and can only smooth (and smear).
        let jitterOn = mode == 1
        var hw = 0.5 + percent / 100 * 0.42
        if !jitterOn { hw = min(hw, 0.5) }
        var p = Params(tanCur: tan, tanPrev: reset ? tan : lastTan[eye], rot: rot, jitter: jit,
                       crop: crop, historyWeight: hw, gamma: jitterOn ? 1.25 : 1.0,
                       sharpness: sharp, slice: UInt32(slice), flipY: flipY ? 1 : 0,
                       reset: reset ? 1 : 0)
        let prevH = history[eye][cur[eye]], nextH = history[eye][cur[eye] ^ 1]
        guard let enc = cmd.makeComputeCommandEncoder() else { return false }
        enc.label = "VPKTAA eye\(eye)"
        let tg = MTLSize(width: 16, height: 16, depth: 1)
        let grid = MTLSize(width: (w + 15) / 16, height: (h + 15) / 16, depth: 1)
        enc.setComputePipelineState(pipeline)
        enc.setTexture(source, index: 0)
        enc.setTexture(prevH, index: 1)
        enc.setTexture(nextH, index: 2)
        enc.setBytes(&p, length: MemoryLayout<Params>.stride, index: 0)
        enc.setBuffer(statBuf, offset: 0, index: 1)
        enc.dispatchThreadgroups(grid, threadsPerThreadgroup: tg)
        // RCAS reads the new history (resolved, unsharpened) and writes the shown picture.
        enc.memoryBarrier(scope: .textures)
        enc.setComputePipelineState(rcas)
        enc.setTexture(nextH, index: 0)
        enc.setTexture(outView, index: 1)
        enc.setBytes(&p, length: MemoryLayout<Params>.stride, index: 0)
        enc.dispatchThreadgroups(grid, threadsPerThreadgroup: tg)
        enc.endEncoding()
        cur[eye] ^= 1
        lastSerial[eye] = rendered.serial
        lastQuat[eye] = q
        lastTan[eye] = tan
        lastFlip[eye] = flipY
        if eye == 1 || lastMode == -1 { lastMode = mode }
        resolved += 1
        if reset { resets += 1 }

        if !said || (eye == 1 && mode != loggedMode) {
            said = true; loggedMode = mode
            NSLog("[taa] mode=\(jitterOn ? "jitter" : "nojitter")\(mode == 0 ? "(probing)" : "") depth=no "
                  + "reverseZ=n/a (no guest depth: GetLayerTexture2 serves none) "
                  + "reprojection=rotation-only \(w)x\(h) per eye, history=\(String(format: "%.2f", hw)) "
                  + "gamma=\(jitterOn ? 1.25 : 1.0) rcas=\(sharp)")
        }
        if Date().timeIntervalSince(lastStats) > 30 {
            lastStats = Date()
            let c = statBuf.contents().assumingMemoryBound(to: UInt32.self)
            let rej = Double(c[0]), off = Double(c[1]), tot = max(1, Double(c[2]))
            lock.lock()
            let gpuMsAvg = gpuMsN > 0 ? gpuMsSum / Double(gpuMsN) : 0
            gpuMsSum = 0; gpuMsN = 0
            lock.unlock()
            // "%@": the message carries '%' signs, which NSLog would read as conversions.
            NSLog("%@", "[taa] stats (30 s, async queue): mode=\(jitterOn ? "jitter" : "nojitter") resolves=\(resolved) "
                  + "resets=\(resets) gpu=\(String(format: "%.2f", gpuMsAvg)) ms/frame rejected=\(String(format: "%.1f", rej / tot * 100)) pct "
                  + "offscreen=\(String(format: "%.1f", off / tot * 100)) pct")
            resolved = 0; resets = 0; reused = 0
            memset(statBuf.contents(), 0, 16)
        }
        return true
    }
    private var loggedMode: Int32 = -1

    static let msl = """
    #include <metal_stdlib>
    using namespace metal;
    struct Params {
        float4 tanCur; float4 tanPrev; float3x3 rot; float2 jitter; float2 crop;
        float historyWeight; float gamma; float sharpness; uint slice; uint flipY; uint reset;
    };
    static float3 toYCoCg(float3 c) {
        return float3(0.25*c.r + 0.5*c.g + 0.25*c.b, 0.5*c.r - 0.5*c.b, -0.25*c.r + 0.5*c.g - 0.25*c.b);
    }
    static float3 fromYCoCg(float3 y) {
        return float3(y.x + y.y - y.z, y.x + y.z, y.x - y.y - y.z);
    }
    static float luma(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }
    // NaN/Inf by exponent bits: fast-math may fold the library NaN tests away.
    static bool3 kbad(float3 v) { return (as_type<uint3>(v) & 0x7f800000u) == 0x7f800000u; }
    static bool kbad1(float v) { return (as_type<uint>(v) & 0x7f800000u) == 0x7f800000u; }
    // Texture uv (in the crop) -> view direction (x right, y up, -z forward), and back.
    static float3 dirFromUV(float2 uv, float4 t, uint flipY) {
        float x = mix(-t.x, t.y, uv.x);
        float y = flipY != 0u ? mix(t.z, -t.w, uv.y) : mix(-t.w, t.z, uv.y);
        return float3(x, y, -1.0);
    }
    static float2 uvFromDir(float3 d, float4 t, uint flipY) {
        float x = -d.x / d.z, y = -d.y / d.z;
        float u = (x + t.x) / (t.x + t.y);
        float v = flipY != 0u ? (t.z - y) / (t.z + t.w) : (y + t.w) / (t.z + t.w);
        return float2(u, v);
    }
    // Catmull-Rom in 5 bilinear taps (corners dropped), uv in texture-normalised coords.
    static float4 sampleCR(texture2d_array<float, access::sample> t, sampler s, float2 uv, float2 size) {
        float2 pos = uv * size, c1 = floor(pos - 0.5) + 0.5, f = pos - c1;
        float2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
        float2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
        float2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
        float2 w3 = f * f * (-0.5 + 0.5 * f);
        float2 w12 = w1 + w2, o12 = w2 / w12;
        float2 t0 = (c1 - 1.0) / size, t3 = (c1 + 2.0) / size, t12 = (c1 + o12) / size;
        float4 r = t.sample(s, float2(t12.x, t0.y), 0) * (w12.x * w0.y)
                 + t.sample(s, float2(t0.x, t12.y), 0) * (w0.x * w12.y)
                 + t.sample(s, t12, 0) * (w12.x * w12.y)
                 + t.sample(s, float2(t3.x, t12.y), 0) * (w3.x * w12.y)
                 + t.sample(s, float2(t12.x, t3.y), 0) * (w12.x * w3.y);
        float wsum = w12.x * w0.y + w0.x * w12.y + w12.x * w12.y + w3.x * w12.y + w12.x * w3.y;
        return max(r / wsum, 0.0);
    }
    kernel void vpktaa_resolve(texture2d_array<float, access::sample> src [[texture(0)]],
                               texture2d_array<float, access::sample> histIn [[texture(1)]],
                               texture2d_array<float, access::write> histOut [[texture(2)]],
                               constant Params& p [[buffer(0)]],
                               device atomic_uint* stats [[buffer(1)]],
                               uint2 gid [[thread_position_in_grid]],
                               uint lane [[thread_index_in_simdgroup]]) {
        uint w = src.get_width(), h = src.get_height();
        bool inside = gid.x < w && gid.y < h;
        uint rej = 0, off = 0, cnt = 0;
        if (inside) {
        constexpr sampler lin(filter::linear, address::clamp_to_edge, coord::normalized);
        float2 size = float2(w, h);
        int2 ip = int2(gid);
        int2 mx = int2(w - 1, h - 1);
        // Output pixel (unjittered grid) -> direction -> where it fell in the JITTERED source.
        float2 uv = (float2(ip) + 0.5) / size;
        float2 cuv = uv / p.crop;
        float3 dir = dirFromUV(cuv, p.tanCur, p.flipY);
        float4 tj = float4(p.tanCur.x - p.jitter.x, p.tanCur.y + p.jitter.x,
                           p.tanCur.z + p.jitter.y, p.tanCur.w - p.jitter.y);
        float2 spos = uvFromDir(dir, tj, p.flipY) * p.crop * size;   // source pixels
        int2 sc = clamp(int2(floor(spos)), int2(0), mx);
        float2 sf = spos - (float2(sc) + 0.5);                       // offset from that texel centre
        // 3x3: Blackman-Harris-like reconstruction of the current colour at the unjittered
        // position, YCoCg moments for the clip box, and the alpha range.
        float3 m1 = 0, m2 = 0, csum = 0;
        float wsum = 0, a = 0, amin = 1e9, amax = -1e9;
        for (int dy = -1; dy <= 1; ++dy)
        for (int dx = -1; dx <= 1; ++dx) {
            float4 s4 = src.read(uint2(clamp(sc + int2(dx, dy), int2(0), mx)), p.slice);
            float3 s = max(s4.rgb, 0.0);
            float2 d = float2(dx, dy) - sf;
            float wt = exp(-2.29 * dot(d, d));
            csum += s * wt; a += s4.a * wt; wsum += wt;
            amin = min(amin, s4.a); amax = max(amax, s4.a);
            float3 y = toYCoCg(s);
            m1 += y; m2 += y * y;
        }
        float3 c = csum / wsum;
        a /= wsum;
        // Never let a non-finite value in: one NaN in the history poisons it for good.
        if (any(kbad(c))) c = 0.0;
        if (kbad1(a)) a = 1.0;
        if (all(p.jitter == 0.0)) {
            // No jitter: the source grid IS the output grid — take the texel itself, a
            // reconstruction filter here would only blur.
            float4 c4 = src.read(uint2(sc), p.slice);
            c = max(c4.rgb, 0.0); a = c4.a;
        }
        m1 /= 9.0; m2 /= 9.0;
        float3 sigma = sqrt(max(m2 - m1 * m1, 0.0));
        float3 bmin = m1 - p.gamma * sigma, bmax = m1 + p.gamma * sigma;

        float3 res = c;
        float resA = a;
        cnt = 1;
        if (p.reset == 0u && all(cuv <= 1.0)) {
            float3 d = p.rot * dir;
            bool hit = false;
            if (d.z < -1e-4) {
                float2 puv = uvFromDir(d, p.tanPrev, p.flipY);
                if (all(puv >= 0.0) && all(puv <= 1.0)) {
                    hit = true;
                    float4 hist4 = sampleCR(histIn, lin, puv * p.crop, size);
                    if (any(kbad(hist4.rgb)) || kbad1(hist4.a)) { hit = false; off = 1; }
                    else {
                    float3 hy = toYCoCg(hist4.rgb);
                    // Clip towards the AABB centre (Playdead/UE4), not a per-channel clamp.
                    float3 cen = 0.5 * (bmax + bmin), ext = max(0.5 * (bmax - bmin), float3(1e-4));
                    float3 v = hy - cen;
                    float3 r = abs(v / ext);
                    float s = max(r.x, max(r.y, r.z));
                    float3 hc = s > 1.0 ? cen + v / s : hy;
                    // Less history the further outside the box it was (disocclusion, motion).
                    float wgt = p.historyWeight * clamp(1.75 - 0.75 * s, 0.0, 1.0);
                    if (s > 1.5) rej = 1;
                    float3 hcol = max(fromYCoCg(hc), 0.0);
                    // Karis inverse-luma weights: bright outliers do not dominate (no flicker).
                    float wc = (1.0 - wgt) / (1.0 + luma(c));
                    float wh = wgt / (1.0 + luma(hcol));
                    res = (c * wc + hcol * wh) / max(wc + wh, 1e-6);
                    resA = mix(a, clamp(hist4.a, amin, amax), wgt);
                    }
                }
            }
            if (!hit) off = 1;    // per-pixel reset: current sample only
        }
        histOut.write(float4(max(res, 0.0), resA), uint2(ip), 0);
        }
        // Counted on 1 pixel in 64: a 32-bit total over 30 s of 2x7.6 Mpx frames overflowed
        // (log 32 said rejected=132.9 %).
        if (((gid.x | gid.y) & 7u) != 0u) { rej = 0; off = 0; cnt = 0; }
        uint r1 = simd_sum(rej), r2 = simd_sum(off), r3 = simd_sum(cnt);
        if (lane == 0) {
            if (r1) atomic_fetch_add_explicit(&stats[0], r1, memory_order_relaxed);
            if (r2) atomic_fetch_add_explicit(&stats[1], r2, memory_order_relaxed);
            if (r3) atomic_fetch_add_explicit(&stats[2], r3, memory_order_relaxed);
        }
    }
    // Linear -> sRGB transfer (the output is written through a plain rgba8Unorm view).
    static float3 enc_srgb(float3 c) {
        c = clamp(c, 0.0, 1.0);
        return select(1.055 * pow(c, float3(1.0 / 2.4)) - 0.055, c * 12.92, c <= 0.0031308);
    }
    // RCAS (AMD FidelityFX CAS, robust variant) on the resolved picture; history untouched.
    kernel void vpktaa_rcas(texture2d_array<float, access::read> hist [[texture(0)]],
                            texture2d_array<float, access::write> out [[texture(1)]],
                            constant Params& p [[buffer(0)]],
                            uint2 gid [[thread_position_in_grid]]) {
        uint w = hist.get_width(), h = hist.get_height();
        if (gid.x >= w || gid.y >= h) return;
        int2 ip = int2(gid), mx = int2(w - 1, h - 1);
        float4 e4 = hist.read(gid, 0);
        if (p.sharpness <= 0.0) { out.write(float4(enc_srgb(max(e4.rgb, 0.0)), e4.a), gid, 0); return; }
        float3 e = e4.rgb;
        float3 b = hist.read(uint2(clamp(ip + int2(0, -1), int2(0), mx)), 0).rgb;
        float3 d = hist.read(uint2(clamp(ip + int2(-1, 0), int2(0), mx)), 0).rgb;
        float3 f = hist.read(uint2(clamp(ip + int2(1, 0), int2(0), mx)), 0).rgb;
        float3 hh = hist.read(uint2(clamp(ip + int2(0, 1), int2(0), mx)), 0).rgb;
        float3 mn4 = min(min(b, d), min(f, hh)), mx4 = max(max(b, d), max(f, hh));
        float3 peak = 1.0;
        float3 hitMin = mn4 / (4.0 * mx4 + 1e-5);
        float3 hitMax = (peak - mx4) / (4.0 * mn4 - 4.0 * peak - 1e-5);
        float3 lobeRGB = max(-hitMin, hitMax);
        float lobe = max(-0.1875, min(max(lobeRGB.r, max(lobeRGB.g, lobeRGB.b)), 0.0)) * p.sharpness;
        float3 o = (lobe * (b + d + f + hh) + e) / (4.0 * lobe + 1.0);
        if (any(kbad(o))) o = any(kbad(e)) ? float3(0.0) : e;
        out.write(float4(enc_srgb(max(o, 0.0)), e4.a), gid, 0);
    }
    """
}
