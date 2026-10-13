// VPKTAA core — temporal antialiasing with per-pixel reprojection, as a self-contained module.
//
// Depends on Metal + simd only (no Klepton / kl_* symbols), so another port (a Vulkan PC/Quest
// title on Vision Pro) can drop this file in and feed it from its own frame records.
//
// ── Input API (per eye, per guest frame) ────────────────────────────────────────────────────
//   VPKTAAEyeInput
//     color        the guest's picture: .type2DArray texture, `slice` = layer of this eye.
//                  Any float-readable format; sRGB formats are read linear by Metal.
//     flipY        true when texture row 0 is the TOP of the picture (Vulkan, Metal).
//     tangents     l r t b (all positive) of the UNjittered frustum = the output grid.
//     jitter       sub-pixel offset the frame was rendered with, in TANGENT units (dx right,
//                  dy up): the guest's frustum was x ∈ [-l+dx, r+dx], y ∈ [-b+dy, t+dy].
//                  .zero = no jitter (then history is capped at 0.5: smoothing only).
//     crop/cropOff the rect of `color` that holds the picture, as fractions of the texture
//                  (symmetric-projection guests render each eye into a sub-rect).
//     orientation  world-from-eye rotation of the camera this eye was rendered with.
//     position     that camera's position in the same world space, metres.
//     depth        optional: the depth buffer the picture was rendered with (same uv layout as
//                  `color`, any resolution; .depth16Unorm/.depth32Float/.depth32Float_stencil8,
//                  single-sample, 2D or 2D array) + reversedZ + near/far of the projection
//                  (far 0 = infinite). Vulkan clip z ∈ [0,1].
//     motion       optional: OBJECT motion vectors (camera motion excluded, as Meta AppSW),
//                  .xy of a float texture, same uv layout as `color`; `scale` maps a texel to
//                  (current − previous) in crop-uv units (Unity AppSW curNDC−prevNDC: 0.5, 0.5;
//                  a negative component flips that axis).
//   encode(cmd:eye:input:output:)  writes `output` (an rgba8Unorm-writable 2D-array texture of
//   the colour's size: the caller's view of an rgba8Unorm_srgb texture; the kernel encodes sRGB)
//   on the colour's grid, UNjittered. Keeps its own history per eye (rgba16f + r16f distance).
//
// ── Algorithm ────────────────────────────────────────────────────────────────────────────────
//   * reconstruction of the current frame at the unjittered pixel centre from the jittered 3x3
//     (Blackman-Harris-like exp(−2.29 d²) weights; the jitter shift is the same for every pixel,
//     so the 9 weights are computed once per frame on the CPU); 18x18 colour tile in
//     threadgroup memory (one read per texel instead of nine);
//   * closest-depth dilation (centre + 4 diagonals) → linear distance → view point
//     P = dir·dist → previous eye view (rotation AND translation, eye offsets included) →
//     previous uv; object motion is applied first (uv − mv, sampled at the closest-depth texel);
//     no depth / far plane → rotation-only (the infinite-distance limit of the same formula);
//   * history: Catmull-Rom (5 bilinear taps); disocclusion: previous linear distance (r16f)
//     gathered 2x2 at the reprojected uv, history dropped when none is within `disocclusion`
//     (relative, default 10 %) of the expected distance; YCoCg variance clip towards the box
//     centre (gamma 1.25 jitter / 1.0 none) with history lowered by the clip distance;
//     velocity-weighted blend (up to −25 % history at ≥ 32 px/frame); Karis inverse-luma blend;
//   * RCAS sharpening on the shown picture only (history stays unsharpened), or a direct sRGB
//     write from the resolve when sharpness is 0;
//   * statistics on 1 pixel in 64 (depth-valid, disoccluded, rejected, offscreen, moving,
//     mean |mv|) and a motion-vector SIGN VOTE: on moving stat pixels the four sign variants
//     (±x, ±y) are reprojected and the one whose history matches the current colour best gets
//     the vote — so a wrong MV convention is visible (and fixable automatically) from a log.
import Metal
import simd
import Foundation

struct VPKTAADepthInput {
    var texture: MTLTexture
    var slice: Int
    var reversedZ: Bool
    var near: Float
    var far: Float          // 0 = infinite
    /// true: the texture covers the crop rect only (e.g. AppSW depth at motion resolution);
    /// false: it has the colour texture's layout (any resolution).
    var mapsCrop: Bool = false
}

struct VPKTAAMotionInput {
    var texture: MTLTexture
    var slice: Int
    var scale: SIMD2<Float>
    /// true: the texture covers the crop rect only (Meta AppSW: motion vectors are rendered at
    /// MotionVectorTextureSize with the eye's own viewport); false: the colour's layout.
    var mapsCrop: Bool = true
}

struct VPKTAAEyeInput {
    var color: MTLTexture
    var slice: Int
    var flipY: Bool
    var tangents: SIMD4<Float>
    var jitter: SIMD2<Float>
    var crop: SIMD2<Float> = SIMD2<Float>(1, 1)
    var cropOff: SIMD2<Float> = SIMD2<Float>(0, 0)
    var orientation: simd_quatf
    var position: SIMD3<Float>
    var depth: VPKTAADepthInput? = nil
    var motion: VPKTAAMotionInput? = nil
}

/// Counters of one eye since the last `takeStats`, on 1 pixel in 64.
struct VPKTAAStats {
    var pixels: UInt32 = 0, rejected: UInt32 = 0, offscreen: UInt32 = 0
    var depthValid: UInt32 = 0, disoccluded: UInt32 = 0
    var moving: UInt32 = 0, mvSum16: UInt32 = 0          // |mv| summed in 1/16 px
    var votes: [UInt32] = [0, 0, 0, 0]                   // (+x+y) (+x−y) (−x+y) (−x−y)
    var meanMvPx: Double { moving > 0 ? Double(mvSum16) / 16 / Double(moving) : 0 }
}

/// Layout shared with the MSL `Params` below (272 bytes; keep the order).
struct VPKTAAParams {
    var tanCur: SIMD4<Float>
    var tanPrev: SIMD4<Float>
    var rot: simd_float3x3          // current eye view -> previous eye view
    var trans: SIMD4<Float>         // xyz: current eye origin in the previous eye's view
    var wA: SIMD4<Float>            // reconstruction weights 0..3 (row-major 3x3)
    var wB: SIMD4<Float>            // 4..7
    var depthP: SIMD4<Float>        // near, far (0 = inf), reversed (1/0), depth on (1/0)
    var mvP: SIMD4<Float>           // scale.xy (signed), mv on, vote on
    var depthAB: SIMD4<Float>       // depth texel pos = colour texel pos * xy + zw
    var mvAB: SIMD4<Float>          // motion texel pos = colour texel pos * xy + zw
    var crop: SIMD2<Float>
    var cropOff: SIMD2<Float>
    var shiftInt: SIMD2<Int32>      // source texel of the output pixel's jittered sample − gid
    var w8: Float
    var historyWeight: Float
    var gamma: Float
    var sharpness: Float
    var disocc: Float
    var slice: UInt32
    var depthSlice: UInt32
    var mvSlice: UInt32
    var flipY: UInt32
    var reset: UInt32
    var writeOut: UInt32
    var pad: UInt32
}

final class VPKTAACore {
    /// 0..100 → max history weight 0.5..0.92 (capped at 0.5 without jitter).
    var historyPercent: Float = 80
    /// RCAS strength 0..1 (0 = no sharpening pass).
    var sharpness: Float = 0.3
    /// Relative depth difference above which history counts as disoccluded.
    var disocclusion: Float = 0.1

    private let device: MTLDevice
    private let resolvePSO: MTLComputePipelineState
    private let rcasPSO: MTLComputePipelineState
    private let statBuf: MTLBuffer
    private let dummyDepth: MTLTexture
    private let dummyMV: MTLTexture

    private struct EyeState {
        var hist: [MTLTexture] = []
        var dist: [MTLTexture] = []
        var cur = 0
        var prevQ: simd_quatf? = nil
        var prevP = SIMD3<Float>(0, 0, 0)
        var prevTan = SIMD4<Float>(0, 0, 0, 0)
        var prevFlip = false
        var prevCrop = SIMD2<Float>(1, 1)
        var prevCropOff = SIMD2<Float>(0, 0)
        var prevJitterOn = false
        var prevHadDepth = false
    }
    private var eyes = [EyeState(), EyeState()]
    private var views: [ObjectIdentifier: MTLTexture] = [:]
    private(set) var resets = 0
    /// Why the last offered depth / motion input was not used (nil = used or not offered).
    private(set) var depthRefused: [String?] = [nil, nil]
    private(set) var motionRefused: [String?] = [nil, nil]

    init?(device: MTLDevice) {
        self.device = device
        do {
            let lib = try device.makeLibrary(source: VPKTAACore.msl, options: nil)
            guard let fr = lib.makeFunction(name: "vpktaa_resolve"),
                  let fs = lib.makeFunction(name: "vpktaa_rcas") else { return nil }
            resolvePSO = try device.makeComputePipelineState(function: fr)
            rcasPSO = try device.makeComputePipelineState(function: fs)
        } catch {
            NSLog("[taa] VPKTAA shader failed to build: \(error) — VPKTAA off")
            return nil
        }
        if resolvePSO.maxTotalThreadsPerThreadgroup < 256 || rcasPSO.maxTotalThreadsPerThreadgroup < 256 {
            NSLog("[taa] VPKTAA kernels allow only \(resolvePSO.maxTotalThreadsPerThreadgroup)/"
                  + "\(rcasPSO.maxTotalThreadsPerThreadgroup) threads per threadgroup (need 256) — VPKTAA off")
            return nil
        }
        guard let sb = device.makeBuffer(length: 2 * 64, options: .storageModeShared) else { return nil }
        statBuf = sb
        memset(sb.contents(), 0, 2 * 64)
        let dd = MTLTextureDescriptor()
        dd.textureType = .type2DArray; dd.pixelFormat = .depth32Float
        dd.width = 1; dd.height = 1; dd.arrayLength = 1
        dd.usage = [.shaderRead]; dd.storageMode = .private
        let md = MTLTextureDescriptor()
        md.textureType = .type2DArray; md.pixelFormat = .rg16Float
        md.width = 1; md.height = 1; md.arrayLength = 1
        md.usage = [.shaderRead]; md.storageMode = .private
        guard let d0 = device.makeTexture(descriptor: dd), let m0 = device.makeTexture(descriptor: md) else { return nil }
        dummyDepth = d0; dummyMV = m0
    }

    /// Forget all history (the next encode of each eye starts clean).
    func invalidate() {
        for e in 0..<2 { eyes[e].prevQ = nil }
    }

    /// Read and clear the counters of `eye`. Approximate while a resolve is on the GPU.
    func takeStats(eye: Int) -> VPKTAAStats {
        let c = (statBuf.contents() + eye * 64).assumingMemoryBound(to: UInt32.self)
        var s = VPKTAAStats()
        s.rejected = c[0]; s.offscreen = c[1]; s.pixels = c[2]; s.depthValid = c[3]
        s.disoccluded = c[4]; s.moving = c[5]; s.mvSum16 = c[6]
        s.votes = [c[7], c[8], c[9], c[10]]
        memset(statBuf.contents() + eye * 64, 0, 64)
        return s
    }

    private func makeTex(_ w: Int, _ h: Int, _ fmt: MTLPixelFormat, _ label: String) -> MTLTexture? {
        let d = MTLTextureDescriptor()
        d.textureType = .type2DArray
        d.pixelFormat = fmt
        d.width = w; d.height = h; d.arrayLength = 1
        d.usage = [.shaderRead, .shaderWrite]
        d.storageMode = .private
        let t = device.makeTexture(descriptor: d)
        t?.label = label
        return t
    }

    /// A 2D-array view of a 2D texture (cached); the texture itself when it already is one.
    private func arrayView(_ t: MTLTexture) -> MTLTexture? {
        if t.textureType == .type2DArray { return t }
        if t.textureType != .type2D { return nil }
        let k = ObjectIdentifier(t)
        if let v = views[k] { return v }
        guard let v = t.makeTextureView(pixelFormat: t.pixelFormat, textureType: .type2DArray,
                                        levels: 0..<1, slices: 0..<1) else { return nil }
        if views.count > 64 { views.removeAll() }
        views[k] = v
        return v
    }

    private static let depthFormats: Set<MTLPixelFormat> = [.depth16Unorm, .depth32Float, .depth32Float_stencil8]
    private static let mvFormats: Set<MTLPixelFormat> = [.rg16Float, .rgba16Float, .rg32Float, .rgba32Float]

    /// Encode the resolve of one eye into `cmd`. Returns false when nothing was encoded
    /// (the caller then shows the guest's own picture).
    func encode(cmd: MTLCommandBuffer, eye: Int, input: VPKTAAEyeInput, output: MTLTexture) -> Bool {
        guard eye == 0 || eye == 1, input.color.textureType == .type2DArray else { return false }
        let w = input.color.width, h = input.color.height
        guard output.width == w, output.height == h else { return false }
        var st = eyes[eye]
        var reset = false
        if st.hist.count != 2 || st.hist[0].width != w || st.hist[0].height != h {
            guard let h0 = makeTex(w, h, .rgba16Float, "vpktaa hist \(eye)a"),
                  let h1 = makeTex(w, h, .rgba16Float, "vpktaa hist \(eye)b"),
                  let d0 = makeTex(w, h, .r16Float, "vpktaa dist \(eye)a"),
                  let d1 = makeTex(w, h, .r16Float, "vpktaa dist \(eye)b") else { return false }
            st.hist = [h0, h1]; st.dist = [d0, d1]; st.cur = 0
            reset = true
        }

        // ── Depth / motion inputs: validate, or fall back to rotation-only ──
        var depthTex: MTLTexture = dummyDepth
        var depthOn = false
        var depthSlice = 0
        depthRefused[eye] = nil
        if let d = input.depth {
            if d.texture.sampleCount > 1 { depthRefused[eye] = "multisampled (game MSAA kept: turn it off)" }
            else if !VPKTAACore.depthFormats.contains(d.texture.pixelFormat) {
                depthRefused[eye] = "pixel format \(d.texture.pixelFormat.rawValue) not a depth format"
            } else if !d.texture.usage.contains(.shaderRead) { depthRefused[eye] = "texture not shader-readable" }
            else if !(d.near > 0) { depthRefused[eye] = "near plane \(d.near)" }
            else if let v = arrayView(d.texture), d.slice < v.arrayLength {
                depthTex = v; depthOn = true; depthSlice = d.slice
            } else { depthRefused[eye] = "no 2D-array view / slice \(d.slice) out of range" }
        }
        var mvTex: MTLTexture = dummyMV
        var mvOn = false
        var mvSlice = 0
        motionRefused[eye] = nil
        if let m = input.motion {
            let aspC = m.mapsCrop ? Float(w) * input.crop.x / max(1, Float(h) * input.crop.y) : Float(w) / Float(max(h, 1))
            let aspM = Float(m.texture.width) / Float(max(m.texture.height, 1))
            if m.texture.sampleCount > 1 { motionRefused[eye] = "multisampled" }
            else if !VPKTAACore.mvFormats.contains(m.texture.pixelFormat) {
                motionRefused[eye] = "pixel format \(m.texture.pixelFormat.rawValue)"
            } else if !m.texture.usage.contains(.shaderRead) { motionRefused[eye] = "texture not shader-readable" }
            else if abs(aspC / max(aspM, 1e-3) - 1) > 0.05 {
                motionRefused[eye] = "aspect \(m.texture.width)x\(m.texture.height) does not match the eye "
                    + "\(m.mapsCrop ? "crop rect" : "texture") (\(w)x\(h) crop \(input.crop.x)x\(input.crop.y))"
            } else if let v = arrayView(m.texture), m.slice < v.arrayLength {
                mvTex = v; mvOn = true; mvSlice = m.slice
            } else { motionRefused[eye] = "no 2D-array view / slice \(m.slice) out of range" }
        }

        // ── Pose delta (eye camera, translation included) ──
        let q = input.orientation.normalized
        var rot = matrix_identity_float3x3
        var trans = SIMD3<Float>(0, 0, 0)
        if let pq = st.prevQ {
            let delta = pq.inverse * q
            let a = abs(delta.angle)
            let angle = a > Float.pi ? 2 * Float.pi - a : a
            if angle > 8 * Float.pi / 180 { reset = true }
            rot = simd_float3x3(delta)
            trans = pq.inverse.act(input.position - st.prevP)
            if simd_length(trans) > 0.25 { reset = true }
        } else { reset = true }
        if input.flipY != st.prevFlip { reset = true }
        let dt = input.tangents - st.prevTan
        if (dt * dt).max() > 1e-8 { reset = true }
        if input.crop != st.prevCrop || input.cropOff != st.prevCropOff { reset = true }
        let jitterOn = input.jitter != SIMD2<Float>(0, 0)
        if jitterOn != st.prevJitterOn { reset = true }
        if !trans.x.isFinite || !trans.y.isFinite || !trans.z.isFinite { trans = .zero; reset = true }

        // ── Jitter: constant source shift and the 9 reconstruction weights ──
        let t = input.tangents
        var shiftInt = SIMD2<Int32>(0, 0)
        var wts = [Float](repeating: 0, count: 9)
        if jitterOn {
            let sx = -input.jitter.x / max(t.x + t.y, 1e-6) * input.crop.x * Float(w)
            let sy = (input.flipY ? 1 : -1) * input.jitter.y / max(t.z + t.w, 1e-6) * input.crop.y * Float(h)
            let ix = (0.5 + sx).rounded(.down), iy = (0.5 + sy).rounded(.down)
            shiftInt = SIMD2<Int32>(Int32(max(-64, min(64, ix))), Int32(max(-64, min(64, iy))))
            let fx = sx - ix, fy = sy - iy
            var sum: Float = 0
            for k in 0..<9 {
                let dx = Float(k % 3 - 1) - fx, dy = Float(k / 3 - 1) - fy
                wts[k] = expf(-2.29 * (dx * dx + dy * dy))
                sum += wts[k]
            }
            for k in 0..<9 { wts[k] /= max(sum, 1e-6) }
        } else {
            wts[4] = 1
        }

        var hw = 0.5 + min(max(historyPercent, 0), 100) / 100 * 0.42
        if !jitterOn { hw = min(hw, 0.5) }
        let near = input.depth?.near ?? 0.05, far = input.depth?.far ?? 0
        let rev: Float = (input.depth?.reversedZ ?? true) ? 1 : 0
        let mvScale = input.motion?.scale ?? SIMD2<Float>(0.5, 0.5)
        let sharpOn = sharpness > 0.001
        // Colour texel position -> depth / motion texel position.
        func ab(_ tex: MTLTexture, _ mapsCrop: Bool) -> SIMD4<Float> {
            let ts = SIMD2<Float>(Float(tex.width), Float(tex.height))
            let cs = SIMD2<Float>(Float(w), Float(h))
            if !mapsCrop { let a = ts / cs; return SIMD4<Float>(a.x, a.y, 0, 0) }
            let a = ts / (cs * input.crop)
            let b = -input.cropOff / input.crop * ts
            return SIMD4<Float>(a.x, a.y, b.x, b.y)
        }
        let depthAB = ab(depthTex, input.depth?.mapsCrop ?? false)
        let mvAB = ab(mvTex, input.motion?.mapsCrop ?? true)
        var p = VPKTAAParams(
            tanCur: t, tanPrev: reset ? t : st.prevTan, rot: rot,
            trans: SIMD4<Float>(trans.x, trans.y, trans.z, 0),
            wA: SIMD4<Float>(wts[0], wts[1], wts[2], wts[3]),
            wB: SIMD4<Float>(wts[4], wts[5], wts[6], wts[7]),
            depthP: SIMD4<Float>(near, max(far, 0), rev, depthOn ? 1 : 0),
            mvP: SIMD4<Float>(mvScale.x, mvScale.y, mvOn ? 1 : 0, mvOn ? 1 : 0),
            depthAB: depthAB, mvAB: mvAB,
            crop: input.crop, cropOff: input.cropOff, shiftInt: shiftInt,
            w8: wts[8], historyWeight: hw, gamma: jitterOn ? 1.25 : 1.0,
            sharpness: min(max(sharpness, 0), 1), disocc: max(disocclusion, 0.01),
            slice: UInt32(input.slice), depthSlice: UInt32(depthSlice), mvSlice: UInt32(mvSlice),
            flipY: input.flipY ? 1 : 0, reset: reset ? 1 : 0, writeOut: sharpOn ? 0 : 1, pad: 0)

        let prevH = st.hist[st.cur], nextH = st.hist[st.cur ^ 1]
        let prevD = st.dist[st.cur], nextD = st.dist[st.cur ^ 1]
        guard let enc = cmd.makeComputeCommandEncoder() else { return false }
        enc.label = "VPKTAA eye\(eye)"
        // Only the rect the guest rendered (+1 texel of margin before, +2 after, so RCAS and
        // the history/distance taps at its edge never read texels this resolve did not write).
        let ox = min(max(Int((input.cropOff.x * Float(w)).rounded(.down)) - 1, 0), w - 1)
        let oy = min(max(Int((input.cropOff.y * Float(h)).rounded(.down)) - 1, 0), h - 1)
        let ex = min(w - ox, Int((input.crop.x * Float(w)).rounded(.up)) + 3)
        let ey = min(h - oy, Int((input.crop.y * Float(h)).rounded(.up)) + 3)
        var org = SIMD2<UInt32>(UInt32(ox), UInt32(oy))
        let tg = MTLSize(width: 16, height: 16, depth: 1)
        let grid = MTLSize(width: (max(ex, 1) + 15) / 16, height: (max(ey, 1) + 15) / 16, depth: 1)
        enc.setComputePipelineState(resolvePSO)
        enc.setTexture(input.color, index: 0)
        enc.setTexture(prevH, index: 1)
        enc.setTexture(nextH, index: 2)
        enc.setTexture(depthTex, index: 3)
        enc.setTexture(mvTex, index: 4)
        enc.setTexture(prevD, index: 5)
        enc.setTexture(nextD, index: 6)
        enc.setTexture(output, index: 7)
        enc.setBytes(&p, length: MemoryLayout<VPKTAAParams>.stride, index: 0)
        enc.setBuffer(statBuf, offset: eye * 64, index: 1)
        enc.setBytes(&org, length: MemoryLayout<SIMD2<UInt32>>.stride, index: 2)
        enc.dispatchThreadgroups(grid, threadsPerThreadgroup: tg)
        if sharpOn {
            enc.memoryBarrier(scope: .textures)
            enc.setComputePipelineState(rcasPSO)
            enc.setTexture(nextH, index: 0)
            enc.setTexture(output, index: 1)
            enc.setBytes(&p, length: MemoryLayout<VPKTAAParams>.stride, index: 0)
            enc.setBytes(&org, length: MemoryLayout<SIMD2<UInt32>>.stride, index: 2)
            enc.dispatchThreadgroups(grid, threadsPerThreadgroup: tg)
        }
        enc.endEncoding()

        st.cur ^= 1
        st.prevQ = q
        st.prevP = input.position
        st.prevTan = t
        st.prevFlip = input.flipY
        st.prevCrop = input.crop
        st.prevCropOff = input.cropOff
        st.prevJitterOn = jitterOn
        st.prevHadDepth = depthOn
        eyes[eye] = st
        if reset { resets += 1 }
        return true
    }

    static let msl = """
    #include <metal_stdlib>
    using namespace metal;
    struct Params {
        float4 tanCur; float4 tanPrev; float3x3 rot; float4 trans; float4 wA; float4 wB;
        float4 depthP; float4 mvP; float4 depthAB; float4 mvAB; float2 crop; float2 cropOff;
        int2 shiftInt; float w8; float historyWeight; float gamma;
        float sharpness; float disocc; uint slice; uint depthSlice; uint mvSlice; uint flipY;
        uint reset; uint writeOut; uint pad;
    };
    constant float kSky = 60000.0;          // distance stored for the far plane (r16f-safe)
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
    // Crop-normalised uv -> view direction (x right, y up, z = -1), and back.
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
    // Device depth -> distance along the view axis (metres); >= 1e29 = far plane / sky.
    static float linDist(float d, constant Params& p) {
        float n = p.depthP.x, f = p.depthP.y;
        if (p.depthP.z > 0.5) {
            if (d <= 1e-7) return 1e30;
            return f > 0.0 ? n * f / (n + d * (f - n)) : n / d;
        }
        if (d >= 1.0 - 1e-7) return 1e30;
        return f > 0.0 ? n * f / (f - d * (f - n)) : n / (1.0 - d);
    }
    // Current crop-uv (+ distance; 0 = infinitely far) -> previous crop-uv and the distance the
    // point has from the previous eye. False when it is behind the previous eye.
    static bool reproject(float2 cuv, float dist, constant Params& p, thread float2& puv, thread float& pd) {
        float3 dir = dirFromUV(cuv, p.tanCur, p.flipY);
        float3 q = dist > 0.0 ? p.rot * (dir * dist) + p.trans.xyz : p.rot * dir;
        if (q.z > -1e-4) return false;
        puv = uvFromDir(q, p.tanPrev, p.flipY);
        pd = -q.z;
        return true;
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
    // Linear -> sRGB transfer (outputs are written through plain rgba8Unorm views).
    static float3 enc_srgb(float3 c) {
        c = clamp(c, 0.0, 1.0);
        return select(1.055 * pow(c, float3(1.0 / 2.4)) - 0.055, c * 12.92, c <= 0.0031308);
    }

    kernel void vpktaa_resolve(texture2d_array<half, access::read> src [[texture(0)]],
                               texture2d_array<float, access::sample> histIn [[texture(1)]],
                               texture2d_array<float, access::write> histOut [[texture(2)]],
                               depth2d_array<float> dep [[texture(3)]],
                               texture2d_array<float, access::read> mvTex [[texture(4)]],
                               texture2d_array<float, access::sample> distIn [[texture(5)]],
                               texture2d_array<float, access::write> distOut [[texture(6)]],
                               texture2d_array<half, access::write> outTex [[texture(7)]],
                               constant Params& p [[buffer(0)]],
                               device atomic_uint* stats [[buffer(1)]],
                               constant uint2& org [[buffer(2)]],
                               uint2 tid [[thread_position_in_grid]],
                               uint2 lid [[thread_position_in_threadgroup]],
                               uint2 tgp [[threadgroup_position_in_grid]],
                               uint li [[thread_index_in_threadgroup]],
                               uint lane [[thread_index_in_simdgroup]]) {
        // 18x18 source tile around this 16x16 group's jittered samples: 1 read per texel.
        threadgroup half4 tile[324];
        uint w = src.get_width(), h = src.get_height();
        int2 mx = int2(int(w) - 1, int(h) - 1);
        int2 base = int2(org) + int2(tgp) * 16 + p.shiftInt - 1;
        for (uint i = li; i < 324u; i += 256u) {
            int2 c = clamp(base + int2(int(i % 18u), int(i / 18u)), int2(0), mx);
            tile[i] = src.read(uint2(c), p.slice);
        }
        threadgroup_barrier(mem_flags::mem_threadgroup);

        uint2 gid = tid + org;
        bool inside = gid.x < w && gid.y < h;
        bool statPx = ((gid.x | gid.y) & 7u) == 0u;
        uint sRej = 0, sOff = 0, sCnt = 0, sDep = 0, sDis = 0, sMov = 0, sMvSum = 0, vote = 4;
        if (inside) {
            constexpr sampler lin(filter::linear, address::clamp_to_edge, coord::normalized);
            constexpr sampler pt(filter::nearest, address::clamp_to_edge, coord::normalized);
            float2 size = float2(w, h);
            int2 ip = int2(gid);
            float2 uv = (float2(ip) + 0.5) / size;
            float2 cuv = (uv - p.cropOff) / p.crop;

            // Reconstruction at the unjittered centre + YCoCg moments of the 3x3.
            const float wts[9] = { p.wA.x, p.wA.y, p.wA.z, p.wA.w, p.wB.x, p.wB.y, p.wB.z, p.wB.w, p.w8 };
            uint t0 = lid.y * 18u + lid.x;
            half3 csum = half3(0.0h);
            half asum = 0.0h;
            float3 m1 = float3(0.0), m2 = float3(0.0);
            for (uint k = 0; k < 9u; ++k) {
                half4 s4 = tile[t0 + (k / 3u) * 18u + (k % 3u)];
                half3 s = max(s4.rgb, half3(0.0h));
                half wk = half(wts[k]);
                csum += s * wk;
                asum += s4.a * wk;
                float3 y = toYCoCg(float3(s));
                m1 += y; m2 += y * y;
            }
            float3 c = float3(csum);
            float a = float(asum);
            if (any(kbad(c))) c = float3(0.0);
            if (kbad1(a)) a = 1.0;
            m1 /= 9.0; m2 /= 9.0;
            float3 sigma = sqrt(max(m2 - m1 * m1, 0.0));
            float3 bmin = m1 - p.gamma * sigma, bmax = m1 + p.gamma * sigma;

            // Depth: closest of the centre and the 4 diagonals (dilation), at depth resolution.
            float dist = 0.0;               // 0 = no depth here (rotation-only)
            bool depthOn = p.depthP.w > 0.5, sky = false;
            float2 bestOff = float2(0.0);
            if (depthOn) {
                float2 spos = float2(ip + p.shiftInt) + 0.5;
                int2 dc = int2(floor(spos * p.depthAB.xy + p.depthAB.zw));
                int2 dmx = int2(int(dep.get_width()) - 1, int(dep.get_height()) - 1);
                bool rev = p.depthP.z > 0.5;
                float best = rev ? -1.0 : 2.0;
                const int2 offs[5] = { int2(0, 0), int2(-1, -1), int2(1, -1), int2(-1, 1), int2(1, 1) };
                for (int k = 0; k < 5; ++k) {
                    float d = dep.read(uint2(clamp(dc + offs[k], int2(0), dmx)), p.depthSlice);
                    if (kbad1(d)) continue;
                    if (rev ? d > best : d < best) { best = d; bestOff = float2(offs[k]); }
                }
                float L = (best >= 0.0 && best <= 1.0) ? linDist(best, p) : 1e30;
                if (L >= 1e29 || kbad1(L)) sky = true;
                else { dist = L; sDep = 1; }
            }

            // Object motion (sampled at the closest-depth texel), applied before the camera.
            float2 mv = float2(0.0), cuvObj = cuv;
            bool mvOn = p.mvP.z > 0.5;
            if (mvOn) {
                float2 cpos = float2(ip + p.shiftInt) + 0.5 + bestOff / max(p.depthAB.xy, float2(1e-4));
                int2 mc = int2(floor(cpos * p.mvAB.xy + p.mvAB.zw));
                int2 mmx = int2(int(mvTex.get_width()) - 1, int(mvTex.get_height()) - 1);
                mv = mvTex.read(uint2(clamp(mc, int2(0), mmx)), p.mvSlice).xy;
                if (any(kbad(float3(mv, 0.0)))) mv = float2(0.0);
                float2 mvd = mv * p.mvP.xy;          // (current - previous), crop-uv
                cuvObj = cuv - mvd;
                float mvpx = length(mvd * p.crop * size);
                if (mvpx > 0.5) { sMov = 1; sMvSum = uint(min(mvpx * 16.0, 65535.0)); }
            }

            float3 res = c;
            float distStore = sky ? kSky : (dist > 0.0 ? min(dist, kSky) : 0.0);
            sCnt = 1;
            if (p.reset == 0u && all(cuv >= 0.0) && all(cuv <= 1.0)) {
                float2 puv = float2(0.0);
                float pd = 0.0;
                bool hit = reproject(cuvObj, dist, p, puv, pd) && all(puv >= 0.0) && all(puv <= 1.0);
                if (hit) {
                    float2 huv = puv * p.crop + p.cropOff;
                    float4 hist4 = sampleCR(histIn, lin, huv, size);
                    if (any(kbad(hist4.rgb))) { hit = false; }
                    else {
                        float wgt = p.historyWeight;
                        // Disocclusion: did the previous frame see THIS surface there?
                        if (depthOn) {
                            float4 g = distIn.gather(pt, huv, 0);
                            if (any(g > 0.0)) {
                                bool ok;
                                if (sky) ok = any(g >= kSky * 0.5);
                                else ok = any(abs(g - pd) < p.disocc * max(pd, 1e-3));
                                if (!ok) { wgt = 0.0; sDis = 1; }
                            }
                        }
                        // Velocity: less history the faster the pixel moves on screen.
                        float speed = length((huv - uv) * size);
                        wgt *= mix(1.0, 0.75, saturate(speed / 32.0));
                        float3 hy = toYCoCg(hist4.rgb);
                        // Clip towards the AABB centre (Playdead/UE4), not a per-channel clamp.
                        float3 cen = 0.5 * (bmax + bmin), ext = max(0.5 * (bmax - bmin), float3(1e-4));
                        float3 v = hy - cen;
                        float3 r = abs(v / ext);
                        float s = max(r.x, max(r.y, r.z));
                        float3 hc = s > 1.0 ? cen + v / s : hy;
                        wgt *= clamp(1.75 - 0.75 * s, 0.0, 1.0);
                        if (s > 1.5) sRej = 1;
                        float3 hcol = max(fromYCoCg(hc), 0.0);
                        // Karis inverse-luma weights: bright outliers do not dominate (no flicker).
                        float wc = (1.0 - wgt) / (1.0 + luma(c));
                        float wh = wgt / (1.0 + luma(hcol));
                        res = (c * wc + hcol * wh) / max(wc + wh, 1e-6);
                    }
                }
                if (!hit) sOff = 1;     // per-pixel reset: current sample only

                // Motion-vector sign vote (stat pixels that move): which of the 4 sign
                // variants reprojects onto history that matches the current colour best.
                if (statPx && mvOn && p.mvP.w > 0.5 && sMov != 0u) {
                    float best = 1e30;
                    float lc = luma(c);
                    for (uint k = 0; k < 4u; ++k) {
                        float2 sg = float2((k & 2u) != 0u ? -1.0 : 1.0, (k & 1u) != 0u ? -1.0 : 1.0);
                        float2 pv; float pdv;
                        if (!reproject(cuv - mv * abs(p.mvP.xy) * sg, dist, p, pv, pdv)) continue;
                        if (any(pv < 0.0) || any(pv > 1.0)) continue;
                        float e = abs(luma(histIn.sample(lin, pv * p.crop + p.cropOff, 0).rgb) - lc);
                        if (e < best) { best = e; vote = k; }
                    }
                }
            }
            histOut.write(float4(max(res, 0.0), a), uint2(ip), 0);
            distOut.write(float4(distStore), uint2(ip), 0);
            if (p.writeOut != 0u) outTex.write(half4(half3(enc_srgb(max(res, 0.0))), half(a)), uint2(ip), 0);
        }
        // Counted on 1 pixel in 64 (a 32-bit total over 30 s of full frames overflows).
        if (!statPx) { sRej = 0; sOff = 0; sCnt = 0; sDep = 0; sDis = 0; sMov = 0; sMvSum = 0; vote = 4; }
        uint r0 = simd_sum(sRej), r1 = simd_sum(sOff), r2 = simd_sum(sCnt), r3 = simd_sum(sDep);
        uint r4 = simd_sum(sDis), r5 = simd_sum(sMov), r6 = simd_sum(sMvSum);
        uint v0 = simd_sum(vote == 0u ? 1u : 0u), v1 = simd_sum(vote == 1u ? 1u : 0u);
        uint v2 = simd_sum(vote == 2u ? 1u : 0u), v3 = simd_sum(vote == 3u ? 1u : 0u);
        if (lane == 0u) {
            if (r0) atomic_fetch_add_explicit(&stats[0], r0, memory_order_relaxed);
            if (r1) atomic_fetch_add_explicit(&stats[1], r1, memory_order_relaxed);
            if (r2) atomic_fetch_add_explicit(&stats[2], r2, memory_order_relaxed);
            if (r3) atomic_fetch_add_explicit(&stats[3], r3, memory_order_relaxed);
            if (r4) atomic_fetch_add_explicit(&stats[4], r4, memory_order_relaxed);
            if (r5) atomic_fetch_add_explicit(&stats[5], r5, memory_order_relaxed);
            if (r6) atomic_fetch_add_explicit(&stats[6], r6, memory_order_relaxed);
            if (v0) atomic_fetch_add_explicit(&stats[7], v0, memory_order_relaxed);
            if (v1) atomic_fetch_add_explicit(&stats[8], v1, memory_order_relaxed);
            if (v2) atomic_fetch_add_explicit(&stats[9], v2, memory_order_relaxed);
            if (v3) atomic_fetch_add_explicit(&stats[10], v3, memory_order_relaxed);
        }
    }

    // RCAS (AMD FidelityFX CAS, robust variant) on the resolved picture; history untouched.
    kernel void vpktaa_rcas(texture2d_array<half, access::read> hist [[texture(0)]],
                            texture2d_array<half, access::write> out [[texture(1)]],
                            constant Params& p [[buffer(0)]],
                            constant uint2& org [[buffer(2)]],
                            uint2 tid [[thread_position_in_grid]]) {
        uint2 gid = tid + org;
        uint w = hist.get_width(), h = hist.get_height();
        if (gid.x >= w || gid.y >= h) return;
        int2 ip = int2(gid), mx = int2(int(w) - 1, int(h) - 1);
        half4 e4 = hist.read(gid, 0);
        half3 e = max(e4.rgb, half3(0.0h));
        half3 b = max(hist.read(uint2(clamp(ip + int2(0, -1), int2(0), mx)), 0).rgb, half3(0.0h));
        half3 d = max(hist.read(uint2(clamp(ip + int2(-1, 0), int2(0), mx)), 0).rgb, half3(0.0h));
        half3 f = max(hist.read(uint2(clamp(ip + int2(1, 0), int2(0), mx)), 0).rgb, half3(0.0h));
        half3 hh = max(hist.read(uint2(clamp(ip + int2(0, 1), int2(0), mx)), 0).rgb, half3(0.0h));
        float3 mn4 = float3(min(min(b, d), min(f, hh))), mx4 = float3(max(max(b, d), max(f, hh)));
        float3 hitMin = mn4 / (4.0 * mx4 + 1e-5);
        float3 hitMax = (1.0 - mx4) / (4.0 * mn4 - 4.0 - 1e-5);
        float3 lobeRGB = max(-hitMin, hitMax);
        float lobe = max(-0.1875, min(max(lobeRGB.r, max(lobeRGB.g, lobeRGB.b)), 0.0)) * p.sharpness;
        float3 o = (lobe * float3(b + d + f + hh) + float3(e)) / (4.0 * lobe + 1.0);
        if (any(kbad(o))) o = any(kbad(float3(e))) ? float3(0.0) : float3(e);
        out.write(half4(half3(enc_srgb(max(o, 0.0))), e4.a), gid, 0);
    }
    """
}
