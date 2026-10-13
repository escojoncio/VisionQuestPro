// VPKTAA core — temporal antialiasing with per-pixel reprojection, as a self-contained module.
// Algorithm and constants ported from PTVisionPro's tuned shaders/vpktaa.frag ("pt-port v1"),
// adapted to a finished LDR sRGB eye picture (no HDR scene, no engine camera matrices).
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
//                  .zero = no jitter (then history is capped at 0.5 and the reconstruction does
//                  not sharpen: smoothing only; the spatial AA is the caller's fallback).
//     crop/cropOff the rect of `color` that holds the picture, as fractions of the texture
//                  (symmetric-projection guests render each eye into a sub-rect).
//     orientation  world-from-eye rotation of the camera this eye was rendered with.
//     position     that camera's position in the same world space, metres.
//     depth        optional: the depth buffer the picture was rendered with (same uv layout as
//                  `color`, any resolution; .depth16Unorm/.depth32Float/.depth32Float_stencil8,
//                  2D or 2D array, single-sample or MULTISAMPLED — then the samples of the
//                  texel are read, by a second library compiled with VPKTAA_DEPTH_MS=1, so a
//                  compiler that rejects the MS variant only loses MSAA depth) + reversedZ
//                  + near/far of the projection (far 0 = infinite). Vulkan clip z ∈ [0,1].
//     motion       optional: OBJECT motion vectors (camera motion excluded, as Meta AppSW),
//                  .xy of a float texture, same uv layout as `color`; `scale` maps a texel to
//                  (current − previous) in crop-uv units (Unity AppSW curNDC−prevNDC: 0.5, 0.5;
//                  a negative component flips that axis).
//   sharpRect[eye] optional rect (x, y, width, height in colour texels) that is resolved; the
//                  rest is a bilinear copy at the unjittered position (foveated periphery).
//                  nil = the whole picture. Keep it stable: a change restarts that eye's history.
//   encode(cmd:eye:input:output:)  writes `output` (an rgba8Unorm-writable 2D-array texture of
//   the colour's size: the caller's view of an rgba8Unorm_srgb texture; the kernel encodes sRGB)
//   on the colour's grid, UNjittered. Keeps its own history per eye (see "History" below).
//
// ── Algorithm (one compute pass per eye, 16x16 groups, 18x18 tile in threadgroup memory) ──
//   * the tile holds each source texel once, already sanitised (NaN/Inf → 0), compressed with
//     the reversible tonemap c / (1 + max(c)) and in YCoCg: every later blend is in that space,
//     so bright spots do not flicker or smear (PT Compress/Expand);
//   * reconstruction of the current frame at the UNjittered pixel centre from the jittered 3x3
//     (Gaussian fit of Blackman-Harris, exp(−2.29 d²); the jitter shift is the same for every
//     pixel, so the 9 weights are computed once per frame on the CPU), then sharpened against
//     the 3x3 mean, contrast-adaptive and stronger while moving (`sharpness`), clamped to the
//     3x3's min/max: the accumulation converges on a sharp picture with no separate pass;
//   * depth: one gather (2x2 around the sample point; MSAA: the texel's samples) → closest,
//     farthest and own linear distance; closest drives the reprojection (an edge moves with
//     what is in front), (far − near) / far marks depth edges (narrow colour box there:
//     SIGMA_EDGE 0.85, flat SIGMA_FLAT 1.25); camera motion from depth + pose (rotation AND
//     translation), object motion (SpaceWarp MV, read at the closest texel) applied first;
//     no depth / sky → rotation-only;
//   * history: Catmull-Rom (5 bilinear taps), clipped toward the box centre; keep
//     KEEP_STILL 0.93 → KEEP_MOVING 0.80 by on-screen speed (8 px/frame = moving), anti-flicker
//     (−0.12 where history and current disagree), disocclusion from the surface distance held
//     in the history's alpha (DISOCCLUSION 0.05 relative, wider on depth slopes), max 0.97;
//   * History: rgba16Float with the surface distance in alpha while depth is used (one texture:
//     8 B/px instead of 8 + 2 for a separate distance target; half keeps 0.05 % of a distance,
//     far below the 5 % tolerance), RG11B10Float (4 B/px, stochastic rounding in the kernel) when
//     there is no depth (no distance to keep). The format follows depth with 90 frames of hysteresis.
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
    /// Optional: the two jitters the frame may REALLY have been rendered with if the caller's
    /// frame record is one frame off (previous / next offset of the sequence), same units as
    /// `jitter`. With them, stat pixels vote which of the three best matches the history
    /// (`VPKTAAStats.jitterVotes`), so a jitter/frame pairing error shows up (and can be fixed).
    var jitterAlt: [SIMD2<Float>] = []
}

/// Counters of one eye since the last `takeStats`, on 1 pixel in 64.
struct VPKTAAStats {
    var pixels: UInt32 = 0, rejected: UInt32 = 0, offscreen: UInt32 = 0
    var depthValid: UInt32 = 0, disoccluded: UInt32 = 0
    var moving: UInt32 = 0, mvSum16: UInt32 = 0          // |mv| summed in 1/16 px
    var votes: [UInt32] = [0, 0, 0, 0]                   // (+x+y) (+x−y) (−x+y) (−x−y)
    var jitterVotes: [UInt32] = [0, 0, 0]                // used jitter, jitterAlt[0], jitterAlt[1]
    var meanMvPx: Double { moving > 0 ? Double(mvSum16) / 16 / Double(moving) : 0 }
}

/// Layout shared with the MSL `Params` below (20 x 16 = 320 bytes, every member 16-byte
/// aligned on both sides; keep the order).
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
    var shiftMain: SIMD4<Float>     // xy: source shift (px) of the used jitter, z: jitter vote on, w: weight 8
    var shiftAlt: SIMD4<Float>      // source shifts (px) of jitterAlt[0] (xy) and [1] (zw)
    var crop: SIMD4<Float>          // xy crop, zw cropOff
    var rect: SIMD4<Float>          // sharp rect x0 y0 x1 y1 in colour texels (x1/y1 exclusive)
    var keep: SIMD4<Float>          // keep still, keep moving, max keep, sharpness
    var misc: SIMD4<Float>          // disocclusion, spatial, 0, 0
    var shiftInt: SIMD4<Int32>      // xy: source texel of the output pixel's nearest sample − gid, z flags, w frame
    var slices: SIMD4<UInt32>       // colour, depth, motion slice, 0
}

final class VPKTAACore {
    /// Algorithm tag written to the log (bump on any change of the resolve's behaviour).
    static let version = "VPKTAA pt-port v1"

    /// 0..100: history strength. 80 = PTVisionPro's tuned KEEP_STILL 0.93 / KEEP_MOVING 0.80;
    /// lower keeps less (the frame's own share scales with 80 / percent). Without jitter the
    /// history is capped at 0.5 (smoothing only).
    var historyPercent: Float = 80
    /// 0..1: sharpening of the reconstruction against the 3x3 mean (contrast-adaptive, stronger
    /// while moving, never outside the 3x3's range). The accumulation converges on it: no
    /// separate sharpening pass. Only with jitter (an unjittered picture is already sharp).
    /// PTVisionPro's tuned value is 1.0.
    var sharpness: Float = 0.3
    /// 0..1: spatial edge AA (FXAA-style local edge blend) — FALLBACK only: applied when the
    /// frame carries no jitter (with jitter the temporal accumulation antialiases).
    var spatial: Float = 1
    /// Relative distance by which the history's surface may be NEARER than the expected one
    /// before it counts as disoccluded (PTVisionPro DISOCCLUSION 0.05; widened on depth slopes).
    var disocclusion: Float = 0.05
    /// Optional per eye: the rect (x, y, width, height in colour texels) that is resolved —
    /// the foveation's sharp zone. Outside it the output is a bilinear copy of the picture at
    /// the unjittered position (no history). nil = the whole picture. A change of the rect
    /// restarts that eye's history.
    var sharpRect: [SIMD4<Int>?] = [nil, nil]

    private let device: MTLDevice
    private let resolvePSO: MTLComputePipelineState
    /// The same kernel compiled with VPKTAA_DEPTH_MS=1 (multisampled depth at texture(8)); nil
    /// when it could not be built — multisampled depth is then refused.
    private let resolveMSPSO: MTLComputePipelineState?
    private let statBuf: MTLBuffer
    private let dummyDepth: MTLTexture
    private let dummyMV: MTLTexture

    private struct EyeState {
        var hist: [MTLTexture] = []
        var alphaHist = false               // rgba16Float with the distance in alpha
        var noDepthFrames = 0
        var cur = 0
        var prevQ: simd_quatf? = nil
        var prevP = SIMD3<Float>(0, 0, 0)
        var prevTan = SIMD4<Float>(0, 0, 0, 0)
        var prevFlip = false
        var prevCrop = SIMD2<Float>(1, 1)
        var prevCropOff = SIMD2<Float>(0, 0)
        var prevJitterOn = false
        var prevRect = SIMD4<Float>(0, 0, 0, 0)
    }
    private var eyes = [EyeState(), EyeState()]
    private var views: [ObjectIdentifier: MTLTexture] = [:]
    private var frame: UInt32 = 0
    private(set) var resets = 0
    /// Why the last offered depth / motion input was not used (nil = used or not offered).
    private(set) var depthRefused: [String?] = [nil, nil]
    private(set) var motionRefused: [String?] = [nil, nil]
    /// The history format each eye currently uses (for diagnostics).
    private(set) var historyFormat: [String] = ["none", "none"]

    init?(device: MTLDevice) {
        self.device = device
        do {
            let lib = try device.makeLibrary(source: VPKTAACore.msl, options: nil)
            guard let fr = lib.makeFunction(name: "vpktaa_resolve") else { return nil }
            resolvePSO = try device.makeComputePipelineState(function: fr)
            do {
                let o = MTLCompileOptions()
                o.preprocessorMacros = ["VPKTAA_DEPTH_MS": NSNumber(value: 1)]
                let libMS = try device.makeLibrary(source: VPKTAACore.msl, options: o)
                guard let fm = libMS.makeFunction(name: "vpktaa_resolve") else {
                    throw NSError(domain: "VPKTAA", code: 1)
                }
                let ms = try device.makeComputePipelineState(function: fm)
                resolveMSPSO = ms.maxTotalThreadsPerThreadgroup >= 256 ? ms : nil
            } catch {
                NSLog("[taa] VPKTAA multisampled-depth kernel failed to build: \(error) — MSAA depth refused")
                resolveMSPSO = nil
            }
        } catch {
            NSLog("[taa] VPKTAA shader failed to build: \(error) — VPKTAA off")
            return nil
        }
        if resolvePSO.maxTotalThreadsPerThreadgroup < 256 {
            NSLog("[taa] VPKTAA kernel allows only \(resolvePSO.maxTotalThreadsPerThreadgroup) "
                  + "threads per threadgroup (need 256) — VPKTAA off")
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
        NSLog("[taa] \(VPKTAACore.version) pipeline ready: history rgba16Float (distance in alpha) "
              + "with depth, rgba16Float without; reconstruction 3x3 + sharpen, "
              + "keep 0.93/0.80, sigma 1.25/0.85, disocclusion \(disocclusion), "
              + "MSAA depth kernel \(resolveMSPSO != nil ? "yes" : "no"), "
              + "threads/group max \(resolvePSO.maxTotalThreadsPerThreadgroup)")
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
        s.jitterVotes = [c[11], c[12], c[13]]
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
    /// Multisampled textures get a multisample-array view the same way.
    private func arrayView(_ t: MTLTexture) -> MTLTexture? {
        if t.textureType == .type2DArray || t.textureType == .type2DMultisampleArray { return t }
        let to: MTLTextureType
        switch t.textureType {
        case .type2D: to = .type2DArray
        case .type2DMultisample: to = .type2DMultisampleArray
        default: return nil
        }
        let k = ObjectIdentifier(t)
        if let v = views[k] { return v }
        guard let v = t.makeTextureView(pixelFormat: t.pixelFormat, textureType: to,
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
        guard w > 0, h > 0, output.width == w, output.height == h,
              input.slice >= 0, input.slice < input.color.arrayLength else { return false }
        var st = eyes[eye]
        var reset = false

        // ── Depth / motion inputs: validate, or fall back to rotation-only ──
        var depthTex: MTLTexture = dummyDepth
        var depthMSTex: MTLTexture? = nil       // multisampled depth (texture(8), MS kernel)
        var depthOn = false
        var depthSlice = 0
        depthRefused[eye] = nil
        if let d = input.depth {
            let ms = d.texture.sampleCount > 1
            if ms && resolveMSPSO == nil { depthRefused[eye] = "multisampled (no MS kernel: game MSAA kept, turn it off)" }
            else if !VPKTAACore.depthFormats.contains(d.texture.pixelFormat) {
                depthRefused[eye] = "pixel format \(d.texture.pixelFormat.rawValue) not a depth format"
            } else if !d.texture.usage.contains(.shaderRead) { depthRefused[eye] = "texture not shader-readable" }
            else if !(d.near > 0) { depthRefused[eye] = "near plane \(d.near)" }
            else if let v = arrayView(d.texture), d.slice >= 0, d.slice < v.arrayLength {
                if ms { depthMSTex = v } else { depthTex = v }
                depthOn = true; depthSlice = d.slice
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
            } else if let v = arrayView(m.texture), m.slice >= 0, m.slice < v.arrayLength {
                mvTex = v; mvOn = true; mvSlice = m.slice
            } else { motionRefused[eye] = "no 2D-array view / slice \(m.slice) out of range" }
        }

        // ── History: rgba16Float (distance in alpha) while depth is used, else rg11b10Float.
        // Back to the small format only after 90 frames without depth (no flapping). ──
        st.noDepthFrames = depthOn ? 0 : min(st.noDepthFrames + 1, 1000)
        let wantAlpha = depthOn || (st.alphaHist && st.noDepthFrames < 90)
        if st.hist.count != 2 || st.hist[0].width != w || st.hist[0].height != h || st.alphaHist != wantAlpha {
            let fmt: MTLPixelFormat = .rgba16Float  // rg11b10Float compute writes not guaranteed on every Apple GPU
            guard let h0 = makeTex(w, h, fmt, "vpktaa hist \(eye)a"),
                  let h1 = makeTex(w, h, fmt, "vpktaa hist \(eye)b") else { return false }
            st.hist = [h0, h1]; st.cur = 0; st.alphaHist = wantAlpha
            reset = true
            historyFormat[eye] = wantAlpha ? "rgba16Float+dist" : "rgba16Float"
            NSLog("[taa] \(VPKTAACore.version) eye \(eye) history \(w)x\(h) \(historyFormat[eye]) "
                  + "(\(wantAlpha ? "depth in use" : "no depth"))")
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
        if !(rot.columns.0 + rot.columns.1 + rot.columns.2).sum().isFinite { rot = matrix_identity_float3x3; reset = true }

        // ── Jitter: constant source shift and the 9 reconstruction weights ──
        let t = input.tangents
        var shiftInt = SIMD2<Int32>(0, 0)
        var wts = [Float](repeating: 0, count: 9)
        // Source position (px) of the output pixel centre = pixel centre + shift(jitter).
        func shift(_ j: SIMD2<Float>) -> SIMD2<Float> {
            SIMD2<Float>(-j.x / max(t.x + t.y, 1e-6) * input.crop.x * Float(w),
                         (input.flipY ? 1 : -1) * j.y / max(t.z + t.w, 1e-6) * input.crop.y * Float(h))
        }
        var sMain = jitterOn ? shift(input.jitter) : SIMD2<Float>(0, 0)
        if !sMain.x.isFinite || !sMain.y.isFinite { sMain = .zero }
        let jVote = jitterOn && input.jitterAlt.count == 2
        let sA0 = jVote ? shift(input.jitterAlt[0]) : SIMD2<Float>(0, 0)
        let sA1 = jVote ? shift(input.jitterAlt[1]) : SIMD2<Float>(0, 0)
        if jitterOn {
            let sx = sMain.x, sy = sMain.y
            // The nearest source texel to the unjittered centre: tile centre (k = 4).
            let ix = (0.5 + sx).rounded(.down), iy = (0.5 + sy).rounded(.down)
            shiftInt = SIMD2<Int32>(Int32(max(-64, min(64, ix))), Int32(max(-64, min(64, iy))))
            let fx = sx - Float(shiftInt.x), fy = sy - Float(shiftInt.y)
            // Gaussian fit of Blackman-Harris (PT: exp(−2.29 d²), output px = source px at 100 %).
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

        // History weights: PT's at 80 %; the current frame's share scales with 80 / percent.
        let pct = min(max(historyPercent, 1), 100)
        let maxKeep: Float = jitterOn ? 0.97 : 0.5
        func keepAt(_ k: Float) -> Float { min(max(1 - (1 - k) * 80 / pct, 0.2), 0.97) }
        let near = input.depth?.near ?? 0.05, far = input.depth?.far ?? 0
        let rev: Float = (input.depth?.reversedZ ?? true) ? 1 : 0
        let mvScale = input.motion?.scale ?? SIMD2<Float>(0.5, 0.5)
        // Colour texel position -> depth / motion texel position.
        func ab(_ tex: MTLTexture, _ mapsCrop: Bool) -> SIMD4<Float> {
            let ts = SIMD2<Float>(Float(tex.width), Float(tex.height))
            let cs = SIMD2<Float>(Float(w), Float(h))
            if !mapsCrop { let a = ts / cs; return SIMD4<Float>(a.x, a.y, 0, 0) }
            let a = ts / (cs * input.crop)
            let b = -input.cropOff / input.crop * ts
            return SIMD4<Float>(a.x, a.y, b.x, b.y)
        }
        let depthAB = ab(depthMSTex ?? depthTex, input.depth?.mapsCrop ?? false)
        let mvAB = ab(mvTex, input.motion?.mapsCrop ?? true)

        // Dispatch region: the rect the guest rendered (+2 texels of margin before, +2 after, so
        // the history taps at its edge never read texels this resolve did not write).
        let ox = min(max(Int((input.cropOff.x * Float(w)).rounded(.down)) - 2, 0), w - 1)
        let oy = min(max(Int((input.cropOff.y * Float(h)).rounded(.down)) - 2, 0), h - 1)
        let ex = max(1, min(w - ox, Int((input.crop.x * Float(w)).rounded(.up)) + 4))
        let ey = max(1, min(h - oy, Int((input.crop.y * Float(h)).rounded(.up)) + 4))

        // Sharp rect (foveation), clamped to the dispatch region; nil / empty / all = off.
        var rect = SIMD4<Float>(0, 0, 0, 0)
        var rectOn = false
        if let r = sharpRect[eye] {
            let x0 = max(r.x, ox), y0 = max(r.y, oy)
            let x1 = min(r.x + r.z, ox + ex), y1 = min(r.y + r.w, oy + ey)
            if x1 - x0 >= 8, y1 - y0 >= 8, !(x0 <= ox && y0 <= oy && x1 >= ox + ex && y1 >= oy + ey) {
                rect = SIMD4<Float>(Float(x0), Float(y0), Float(x1), Float(y1))
                rectOn = true
            }
        }
        if rect != st.prevRect { reset = true }

        var flags: Int32 = 0
        if reset { flags |= 1 }
        if input.flipY { flags |= 2 }
        if st.alphaHist { flags |= 4 }
        if rectOn { flags |= 8 }
        frame &+= 1
        var p = VPKTAAParams(
            tanCur: t, tanPrev: reset ? t : st.prevTan, rot: rot,
            trans: SIMD4<Float>(trans.x, trans.y, trans.z, 0),
            wA: SIMD4<Float>(wts[0], wts[1], wts[2], wts[3]),
            wB: SIMD4<Float>(wts[4], wts[5], wts[6], wts[7]),
            depthP: SIMD4<Float>(near, max(far, 0), rev, depthOn ? 1 : 0),
            mvP: SIMD4<Float>(mvScale.x, mvScale.y, mvOn ? 1 : 0, mvOn ? 1 : 0),
            depthAB: depthAB, mvAB: mvAB,
            shiftMain: SIMD4<Float>(sMain.x, sMain.y, jVote ? 1 : 0, wts[8]),
            shiftAlt: SIMD4<Float>(sA0.x, sA0.y, sA1.x, sA1.y),
            crop: SIMD4<Float>(input.crop.x, input.crop.y, input.cropOff.x, input.cropOff.y),
            rect: rect,
            keep: SIMD4<Float>(min(keepAt(0.93), maxKeep), min(keepAt(0.80), maxKeep), maxKeep,
                               jitterOn ? min(max(sharpness, 0), 1) : 0),
            misc: SIMD4<Float>(max(disocclusion, 0.01), jitterOn ? 0 : min(max(spatial, 0), 1), 0, 0),
            shiftInt: SIMD4<Int32>(shiftInt.x, shiftInt.y, flags, Int32(bitPattern: frame)),
            slices: SIMD4<UInt32>(UInt32(input.slice), UInt32(depthSlice), UInt32(mvSlice), 0))

        let prevH = st.hist[st.cur], nextH = st.hist[st.cur ^ 1]
        guard let enc = cmd.makeComputeCommandEncoder() else { return false }
        enc.label = "VPKTAA eye\(eye)"
        var org = SIMD2<UInt32>(UInt32(ox), UInt32(oy))
        let tg = MTLSize(width: 16, height: 16, depth: 1)
        let grid = MTLSize(width: (ex + 15) / 16, height: (ey + 15) / 16, depth: 1)
        if let m = depthMSTex, let msPSO = resolveMSPSO {
            enc.setComputePipelineState(msPSO)
            enc.setTexture(m, index: 8)
        } else {
            enc.setComputePipelineState(resolvePSO)
        }
        enc.setTexture(input.color, index: 0)
        enc.setTexture(prevH, index: 1)
        enc.setTexture(nextH, index: 2)
        enc.setTexture(depthTex, index: 3)
        enc.setTexture(mvTex, index: 4)
        enc.setTexture(output, index: 7)
        enc.setBytes(&p, length: MemoryLayout<VPKTAAParams>.stride, index: 0)
        enc.setBuffer(statBuf, offset: eye * 64, index: 1)
        enc.setBytes(&org, length: MemoryLayout<SIMD2<UInt32>>.stride, index: 2)
        enc.dispatchThreadgroups(grid, threadsPerThreadgroup: tg)
        enc.endEncoding()

        st.cur ^= 1
        st.prevQ = q
        st.prevP = input.position
        st.prevTan = t
        st.prevFlip = input.flipY
        st.prevCrop = input.crop
        st.prevCropOff = input.cropOff
        st.prevJitterOn = jitterOn
        st.prevRect = rect
        eyes[eye] = st
        if reset { resets += 1 }
        return true
    }

    static let msl = """
    #include <metal_stdlib>
    using namespace metal;
    // VPKTAA pt-port v1 (PTVisionPro shaders/vpktaa.frag, on a finished LDR eye picture).
    struct Params {
        float4 tanCur; float4 tanPrev; float3x3 rot; float4 trans; float4 wA; float4 wB;
        float4 depthP; float4 mvP; float4 depthAB; float4 mvAB; float4 shiftMain; float4 shiftAlt;
        float4 crop;        // xy crop, zw cropOff
        float4 rect;        // sharp rect x0 y0 x1 y1 (texels, x1/y1 exclusive), flag 8
        float4 keep;        // keep still, keep moving, max keep, sharpness
        float4 misc;        // disocclusion, spatial (fallback, no jitter)
        int4 shiftInt;      // xy nearest-sample texel shift, z flags (1 reset, 2 flipY,
                            // 4 history rgba16f + distance in alpha, 8 sharp rect), w frame
        uint4 slices;       // colour, depth, motion
    };
    #ifndef VPKTAA_DEPTH_MS
    #define VPKTAA_DEPTH_MS 0
    #endif
    // PTVisionPro tuned constants (KEEP_* scaled on the CPU by historyPercent).
    constant float SIGMA_FLAT = 1.25;
    constant float SIGMA_EDGE = 0.85;
    constant float kSky = 60000.0;          // distance stored for the far plane (half-safe)

    static float3 toYCoCg(float3 c) {
        return float3(0.25*c.r + 0.5*c.g + 0.25*c.b, 0.5*c.r - 0.5*c.b, -0.25*c.r + 0.5*c.g - 0.25*c.b);
    }
    static float3 fromYCoCg(float3 y) {
        return float3(y.x + y.y - y.z, y.x + y.z, y.x - y.y - y.z);
    }
    static float maxOf(float3 c) { return max(c.r, max(c.g, c.b)); }
    // Reversible tonemap: blending in it weighs each sample by 1 / (1 + brightness).
    static float3 compress(float3 c) { return c / (1.0 + maxOf(c)); }
    static float3 expand(float3 c) {
        float m = maxOf(c);
        if (m > 0.9999) c *= 0.9999 / m;
        return c / max(1.0 - maxOf(c), 1e-4);
    }
    // NaN/Inf by exponent bits: fast-math may fold the library NaN tests away.
    static bool3 kbad(float3 v) { return (as_type<uint3>(v) & 0x7f800000u) == 0x7f800000u; }
    static bool kbad1(float v) { return (as_type<uint>(v) & 0x7f800000u) == 0x7f800000u; }
    static float3 sane(float3 c) { return any(kbad(c)) ? float3(0.0) : clamp(c, 0.0, 60000.0); }
    // History colour (linear light). rg11b10Float keeps a 6-bit (R, G) / 5-bit (B) mantissa:
    // with keep 0.93 a round-to-nearest (or, worse, round-toward-zero) store would freeze or
    // darken the history (each step smaller than the rounding is lost every frame). So the
    // value is quantised HERE to an exactly representable one with stochastic rounding
    // (floor(v / ulp + U[0,1)) * ulp: unbiased whatever the hardware's write rounding is);
    // the Catmull-Rom tap and the blend average the noise away. rgba16f stores as is.
    static float sq(float v, float r, int mbits) {
        if (!(v > 0.0)) return 0.0;
        int e = max(int((as_type<uint>(v) >> 23) & 0xffu) - 127, -14);
        float ulp = as_type<float>(uint(e - mbits + 127) << 23);
        return min(floor(v / ulp + r) * ulp, 64512.0);
    }
    static float3 histEncode(float3 c, bool alphaHist, uint2 g, uint frame) {
        if (alphaHist) return min(c, 60000.0);
        uint x = g.x * 1973u + g.y * 9277u + frame * 26699u;
        x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; x ^= x >> 16;
        float3 r = float3(float(x & 0x3ffu), float((x >> 10) & 0x3ffu), float((x >> 20) & 0x3ffu)) * (1.0 / 1024.0);
        return float3(sq(c.r, r.x, 6), sq(c.g, r.y, 6), sq(c.b, r.z, 5));
    }
    // Crop-normalised uv -> view direction (x right, y up, z = -1), and back.
    static float3 dirFromUV(float2 uv, float4 t, bool flipY) {
        float x = mix(-t.x, t.y, uv.x);
        float y = flipY ? mix(t.z, -t.w, uv.y) : mix(-t.w, t.z, uv.y);
        return float3(x, y, -1.0);
    }
    static float2 uvFromDir(float3 d, float4 t, bool flipY) {
        float x = -d.x / d.z, y = -d.y / d.z;
        float u = (x + t.x) / (t.x + t.y);
        float v = flipY ? (t.z - y) / (t.z + t.w) : (y + t.w) / (t.z + t.w);
        return float2(u, v);
    }
    // Device depth -> distance along the view axis (metres); >= 1e29 = far plane / sky.
    static float linDist(float d, constant Params& p) {
        if (kbad1(d) || d < 0.0 || d > 1.0) return 1e30;
        float n = p.depthP.x, f = p.depthP.y;
        if (p.depthP.z > 0.5) {
            if (d <= 1e-7) return 1e30;
            return f > 0.0 ? n * f / (n + d * (f - n)) : n / d;
        }
        if (d >= 1.0 - 1e-7) return 1e30;
        return f > 0.0 ? n * f / (f - d * (f - n)) : n / (1.0 - d);
    }
    // Current crop-uv (+ distance; 0 = infinitely far) -> previous crop-uv and the distance the
    // point has from the previous eye (view z). False when it is behind the previous eye.
    static bool reproject(float2 cuv, float dist, constant Params& p, bool flipY,
                          thread float2& puv, thread float& pd) {
        float3 dir = dirFromUV(cuv, p.tanCur, flipY);
        float3 q = dist > 0.0 ? p.rot * (dir * dist) + p.trans.xyz : p.rot * dir;
        if (q.z > -1e-4) return false;
        puv = uvFromDir(q, p.tanPrev, flipY);
        pd = -q.z;
        return true;
    }
    // Catmull-Rom in 5 bilinear taps (corners dropped), uv in texture-normalised coords.
    static float3 sampleCR(texture2d_array<float, access::sample> t, sampler s, float2 uv, float2 size) {
        float2 pos = uv * size, c1 = floor(pos - 0.5) + 0.5, f = pos - c1;
        float2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
        float2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
        float2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
        float2 w3 = f * f * (-0.5 + 0.5 * f);
        float2 w12 = w1 + w2, o12 = w2 / w12;
        float2 t0 = (c1 - 1.0) / size, t3 = (c1 + 2.0) / size, t12 = (c1 + o12) / size;
        float a = w12.x * w0.y, b = w0.x * w12.y, c = w12.x * w12.y, d = w3.x * w12.y, e = w12.x * w3.y;
        float3 r = t.sample(s, float2(t12.x, t0.y), 0).rgb * a
                 + t.sample(s, float2(t0.x, t12.y), 0).rgb * b
                 + t.sample(s, t12, 0).rgb * c
                 + t.sample(s, float2(t3.x, t12.y), 0).rgb * d
                 + t.sample(s, float2(t12.x, t3.y), 0).rgb * e;
        return max(r / max(a + b + c + d + e, 1e-4), 0.0);
    }
    // Clip toward the box's centre (keeps the history's hue where a clamp would make it blocky).
    static float3 clipToBox(float3 h, float3 lo, float3 hi, thread float& largest) {
        float3 cen = 0.5 * (hi + lo), ext = max(0.5 * (hi - lo), float3(1e-5));
        float3 off = h - cen;
        float3 u = abs(off / ext);
        largest = max(u.x, max(u.y, u.z));
        return largest > 1.0 ? cen + off / largest : h;
    }
    // Linear -> sRGB transfer (outputs are written through plain rgba8Unorm views).
    static float3 enc_srgb(float3 c) {
        c = clamp(c, 0.0, 1.0);
        return select(1.055 * pow(c, float3(1.0 / 2.4)) - 0.055, c * 12.92, c <= 0.0031308);
    }

    kernel void vpktaa_resolve(texture2d_array<float, access::sample> src [[texture(0)]],
                               texture2d_array<float, access::sample> histIn [[texture(1)]],
                               texture2d_array<float, access::write> histOut [[texture(2)]],
                               depth2d_array<float> dep [[texture(3)]],
                               texture2d_array<float, access::read> mvTex [[texture(4)]],
                               texture2d_array<half, access::write> outTex [[texture(7)]],
    #if VPKTAA_DEPTH_MS
                               depth2d_ms_array<float> depMS [[texture(8)]],
    #endif
                               constant Params& p [[buffer(0)]],
                               device atomic_uint* stats [[buffer(1)]],
                               constant uint2& org [[buffer(2)]],
                               uint2 lid [[thread_position_in_threadgroup]],
                               uint2 tgp [[threadgroup_position_in_grid]],
                               uint li [[thread_index_in_threadgroup]]) {
        // 18x18 tile around the group's nearest samples (kernel scope, as MSL requires).
        threadgroup half4 tile[324];
        constexpr sampler lin(filter::linear, address::clamp_to_edge, coord::normalized);
        constexpr sampler pt(filter::nearest, address::clamp_to_edge, coord::normalized);
        const int flags = p.shiftInt.z;
        const bool reset = (flags & 1) != 0, flipY = (flags & 2) != 0;
        const bool alphaHist = (flags & 4) != 0, rectOn = (flags & 8) != 0;
        const uint slice = p.slices.x;
        uint w = src.get_width(), h = src.get_height();
        float2 size = float2(w, h);
        int2 po = int2(org) + int2(tgp) * 16;
        int2 gi = po + int2(lid);
        bool inside = gi.x < int(w) && gi.y < int(h);
        uint2 gid = uint2(gi);
        float2 spos = float2(gi) + 0.5 + p.shiftMain.xy;    // where this pixel's content was rendered

        // Foveated periphery: groups entirely outside the sharp rect get a bilinear copy at
        // the unjittered position (uniform per group: no barrier is skipped by part of it).
        if (rectOn && (po.x >= int(p.rect.z) || po.y >= int(p.rect.w)
                       || po.x + 16 <= int(p.rect.x) || po.y + 16 <= int(p.rect.y))) {
            if (inside) {
                float4 s = src.sample(lin, spos / size, slice);
                outTex.write(half4(half3(enc_srgb(sane(s.rgb))), half(saturate(s.a))), gid, 0);
            }
            return;
        }

        // Tile: each texel read once, sanitised, compressed and in YCoCg (xyz), alpha in w.
        int2 mx = int2(int(w) - 1, int(h) - 1);
        int2 base = po + p.shiftInt.xy - 1;
        for (uint i = li; i < 324u; i += 256u) {
            int2 c = clamp(base + int2(int(i % 18u), int(i / 18u)), int2(0), mx);
            float4 s = src.read(uint2(c), slice);
            tile[i] = half4(half3(toYCoCg(compress(sane(s.rgb)))), half(saturate(kbad1(s.a) ? 1.0 : s.a)));
        }
        threadgroup_barrier(mem_flags::mem_threadgroup);
        if (!inside) return;                    // no barrier after this point

        bool inRect = !rectOn || (float(gi.x) >= p.rect.x && float(gi.y) >= p.rect.y
                                  && float(gi.x) < p.rect.z && float(gi.y) < p.rect.w);
        if (!inRect) {
            float4 s = src.sample(lin, spos / size, slice);
            outTex.write(half4(half3(enc_srgb(sane(s.rgb))), half(saturate(s.a))), gid, 0);
            return;
        }

        bool statPx = ((gid.x | gid.y) & 7u) == 0u;
        uint sRej = 0, sOff = 0, sCnt = 0, sDep = 0, sDis = 0, sMov = 0, sMvSum = 0, vote = 4, jvote = 3;
        float2 uv = (float2(gi) + 0.5) / size;
        float2 cuv = (uv - p.crop.zw) / p.crop.xy;

        // ── 1. Reconstruction at the unjittered centre (Gaussian fit of Blackman-Harris) +
        //       moments and min/max of the 3x3 (compressed YCoCg) ──
        const float wts[9] = { p.wA.x, p.wA.y, p.wA.z, p.wA.w, p.wB.x, p.wB.y, p.wB.z, p.wB.w, p.shiftMain.w };
        uint t0 = lid.y * 18u + lid.x;              // top-left of this pixel's 3x3
        uint tc = t0 + 19u;                         // its nearest sample
        float3 gauss = float3(0.0), m1 = float3(0.0), m2 = float3(0.0);
        float3 boxLo = float3(1e9), boxHi = float3(-1e9);
        for (uint k = 0; k < 9u; ++k) {
            float3 s = float3(tile[t0 + (k / 3u) * 18u + (k % 3u)].xyz);
            gauss += s * wts[k];
            m1 += s; m2 += s * s;
            boxLo = min(boxLo, s); boxHi = max(boxHi, s);
        }
        float3 mean = m1 / 9.0;
        float ownA = float(tile[tc].w);

        // Spatial edge AA (FXAA 3.11 edge test + local blend across the edge): fallback for
        // frames without jitter only (p.misc.y is 0 with jitter).
        if (p.misc.y > 0.0) {
            float lC = float(tile[tc].x), lN = float(tile[tc - 18u].x), lS = float(tile[tc + 18u].x);
            float lW = float(tile[tc - 1u].x), lE = float(tile[tc + 1u].x);
            float lNW = float(tile[tc - 19u].x), lNE = float(tile[tc - 17u].x);
            float lSW = float(tile[tc + 17u].x), lSE = float(tile[tc + 19u].x);
            float lmn = min(lC, min(min(lN, lS), min(lW, lE)));
            float lmx = max(lC, max(max(lN, lS), max(lW, lE)));
            float range = lmx - lmn;
            if (range > max(0.0156, 0.125 * lmx)) {
                float eV = abs(0.25 * lNW - 0.5 * lW + 0.25 * lSW) + abs(0.5 * lN - lC + 0.5 * lS)
                         + abs(0.25 * lNE - 0.5 * lE + 0.25 * lSE);
                float eH = abs(0.25 * lNW - 0.5 * lN + 0.25 * lNE) + abs(0.5 * lW - lC + 0.5 * lE)
                         + abs(0.25 * lSW - 0.5 * lS + 0.25 * lSE);
                bool horz = eH >= eV;
                float l1 = horz ? lN : lW, l2 = horz ? lS : lE;
                bool second = abs(l2 - lC) >= abs(l1 - lC);
                uint ti = horz ? (second ? tc + 18u : tc - 18u) : (second ? tc + 1u : tc - 1u);
                float avg = (2.0 * (lN + lS + lW + lE) + lNW + lNE + lSW + lSE) / 12.0;
                float sub = smoothstep(0.0, 1.0, saturate(abs(avg - lC) / range));
                float bf = clamp(max(0.25, sub * sub * 0.75), 0.0, 0.5) * p.misc.y;
                gauss = mix(gauss, float3(tile[ti].xyz), bf);
            }
        }

        // ── 2. Depth: one 2x2 gather around the sample point (MSAA: the nearest texel's
        //       samples + the other three texels' sample 0) → closest / farthest / own ──
        bool depthOn = p.depthP.w > 0.5;
        float dist = 0.0;                   // closest surface (0 = none / sky: rotation-only)
        float ownDist = kSky, depthRange = 0.0;
        float2 closestColPos = spos;        // colour texel position of the closest depth texel
        if (depthOn) {
            float2 dpos = spos * p.depthAB.xy + p.depthAB.zw;
            float2 corner = floor(dpos + 0.5);              // shared corner of the nearest 2x2
            bool hiX = dpos.x >= corner.x, hiY = dpos.y >= corner.y;
            // Gather order (MSL): x (i0,j1) y (i1,j1) z (i1,j0) w (i0,j0); i0/j0 = corner - 1.
            uint ownK = hiY ? (hiX ? 1u : 0u) : (hiX ? 2u : 3u);
            float4 g;
            float farMS = 0.0;              // MSAA: farthest sample of the own texel (linear)
    #if VPKTAA_DEPTH_MS
            int2 dmx = int2(int(depMS.get_width()) - 1, int(depMS.get_height()) - 1);
            int2 i0 = int2(corner) - 1;
            uint2 q00 = uint2(clamp(i0, int2(0), dmx)), q11 = uint2(clamp(i0 + 1, int2(0), dmx));
            g = float4(depMS.read(uint2(q00.x, q11.y), p.slices.y, 0), depMS.read(q11, p.slices.y, 0),
                       depMS.read(uint2(q11.x, q00.y), p.slices.y, 0), depMS.read(q00, p.slices.y, 0));
            float ownD = g[ownK];
            {
                // The own texel's other samples: its nearest joins the closest search (an MSAA
                // edge inside the texel), its farthest the depth range.
                uint2 qo = uint2(hiX ? q11.x : q00.x, hiY ? q11.y : q00.y);
                uint ns = min(depMS.get_num_samples(), 8u);
                float ln = linDist(ownD, p), lf = ln;
                for (uint si = 1; si < ns; ++si) {
                    float ds = depMS.read(qo, p.slices.y, si);
                    float L = linDist(ds, p);
                    if (L < ln) { ln = L; g[ownK] = ds; }
                    lf = max(lf, L);
                }
                farMS = lf;
            }
    #else
            g = dep.gather(pt, corner / float2(dep.get_width(), dep.get_height()), p.slices.y);
            float ownD = g[ownK];
    #endif
            float lc = 1e30, lf = farMS;
            uint ck = ownK;
            for (uint k = 0; k < 4u; ++k) {
                float L = linDist(g[k], p);
                if (L < lc) { lc = L; ck = k; }
                lf = max(lf, L);
            }
            float lOwn = linDist(ownD, p);
            ownDist = lOwn >= 1e29 ? kSky : min(lOwn, kSky);
            if (lc < 1e29) {
                dist = lc; sDep = 1;
                depthRange = lf >= 1e29 ? 1.0 : (lf - lc) / max(lf, 1e-6);
                float2 cOff = float2((ck == 1u || ck == 2u) ? 0.5 : -0.5, (ck == 0u || ck == 1u) ? 0.5 : -0.5);
                closestColPos = (corner + cOff - p.depthAB.zw) / max(p.depthAB.xy, float2(1e-4));
            }
        }

        // ── 3. Motion: object MV (SpaceWarp) at the closest texel, then the camera's from
        //       depth + pose ──
        float2 mv = float2(0.0), cuvObj = cuv;
        bool mvOn = p.mvP.z > 0.5;
        if (mvOn) {
            int2 mc = int2(floor(closestColPos * p.mvAB.xy + p.mvAB.zw));
            int2 mmx = int2(int(mvTex.get_width()) - 1, int(mvTex.get_height()) - 1);
            mv = mvTex.read(uint2(clamp(mc, int2(0), mmx)), p.slices.z).xy;
            if (any(kbad(float3(mv, 0.0))) || any(abs(mv) > 4.0)) mv = float2(0.0);
            float2 mvd = mv * p.mvP.xy;          // (current - previous), crop-uv
            cuvObj = cuv - mvd;
            float mvpx = length(mvd * p.crop.xy * size);
            if (mvpx > 0.5) { sMov = 1; sMvSum = uint(min(mvpx * 16.0, 65535.0)); }
        }
        float2 puv = float2(0.0);
        float pd = 0.0;
        bool hit = !reset && all(cuv >= 0.0) && all(cuv <= 1.0)
                   && reproject(cuvObj, dist, p, flipY, puv, pd) && all(puv >= 0.0) && all(puv <= 1.0);
        float2 huv = puv * p.crop.xy + p.crop.zw;
        float2 hpx = huv * size;
        if (hit && rectOn) {
            // History exists only inside the sharp rect (2 texels in: the Catmull-Rom footprint).
            hit = all(hpx >= p.rect.xy + 2.0) && all(hpx <= p.rect.zw - 2.0);
        }
        float speed = hit ? min(length(hpx - float2(gi) - 0.5), 64.0) : 0.0;   // px a frame
        float still = 1.0 - saturate(speed * 0.5);

        // ── 4. Sharpen the reconstruction against the 3x3 mean (contrast-adaptive, more while
        //       moving), never outside the 3x3's range ──
        float3 current = gauss;
        if (p.keep.w > 0.0) {
            float amp = sqrt(saturate(min(boxLo.x, 1.0 - boxHi.x) / max(boxHi.x, 1e-4)));
            float amount = p.keep.w * amp * (1.0 + 0.6 * (1.0 - still));
            current = clamp(gauss + amount * 1.2 * (gauss - mean), boxLo, boxHi);
        }
        // Colour box: narrow at depth edges, wide on flat areas.
        float edge = smoothstep(0.02, 0.15, depthRange);
        float3 sigma = sqrt(max(m2 / 9.0 - mean * mean, 0.0));
        float kS = mix(SIGMA_FLAT, SIGMA_EDGE, edge);
        float3 lo = min(max(mean - kS * sigma, boxLo), current);
        float3 hi = max(min(mean + kS * sigma, boxHi), current);

        // ── 5. History ──
        float3 result = current;
        sCnt = 1;
        if (hit) {
            float3 hlin = sampleCR(histIn, lin, huv, size);
            if (any(kbad(hlin))) hit = false;
            else {
                // Jitter pairing vote (stat pixels on edges, static ones; caller-provided alts).
                if (statPx && p.shiftMain.z > 0.5 && sMov == 0u && sigma.x > 0.01) {
                    float lh = dot(hlin, float3(0.2126, 0.7152, 0.0722));
                    float2 c0 = float2(gi) + 0.5;
                    float3 lw = float3(0.2126, 0.7152, 0.0722);
                    float e0 = abs(dot(src.sample(lin, (c0 + p.shiftMain.xy) / size, slice).rgb, lw) - lh);
                    float e1 = abs(dot(src.sample(lin, (c0 + p.shiftAlt.xy) / size, slice).rgb, lw) - lh);
                    float e2 = abs(dot(src.sample(lin, (c0 + p.shiftAlt.zw) / size, slice).rgb, lw) - lh);
                    float lo3 = min(e0, min(e1, e2));
                    if (lo3 < 0.9 * max(e0, max(e1, e2))) jvote = e0 <= lo3 ? 0u : (e1 <= lo3 ? 1u : 2u);
                }
                float3 hist = toYCoCg(compress(sane(hlin)));
                float largest;
                hist = clipToBox(hist, lo, hi, largest);
                if (largest > 1.5) sRej = 1;
                float keep = mix(p.keep.x, p.keep.y, saturate(speed * 0.125));
                // Anti-flicker (after Lottes): agree → keep more; disagree → take more current.
                float lcur = current.x, lh = hist.x;
                float agree = 1.0 - abs(lcur - lh) / max(max(lcur, lh), 0.2);
                keep = mix(keep - 0.12, keep, agree * agree);
                // Disocclusion: the history held something nearer there (this surface was hidden).
                if (alphaHist && dist > 0.0 && pd > 0.0) {
                    float held = histIn.read(uint2(clamp(int2(hpx), int2(0), mx)), 0).a;
                    if (held > 0.0 && !kbad1(held)) {
                        float nearer = (pd - held) / pd;
                        float tol = max(p.misc.x, 1.5 * depthRange);
                        float f = smoothstep(tol, 2.0 * tol, nearer);
                        keep *= 1.0 - f;
                        if (f > 0.5) sDis = 1;
                    }
                }
                result = mix(current, hist, clamp(keep, 0.0, p.keep.z));
            }
        }
        if (!hit) sOff = 1;

        // Motion-vector sign vote (stat pixels that move): which of the 4 sign variants
        // reprojects onto history that matches the current colour best.
        if (statPx && mvOn && p.mvP.w > 0.5 && sMov != 0u && !reset) {
            float best = 1e30;
            float lcY = current.x;
            for (uint k = 0; k < 4u; ++k) {
                float2 sg = float2((k & 2u) != 0u ? -1.0 : 1.0, (k & 1u) != 0u ? -1.0 : 1.0);
                float2 pv; float pdv;
                if (!reproject(cuv - mv * abs(p.mvP.xy) * sg, dist, p, flipY, pv, pdv)) continue;
                if (any(pv < 0.0) || any(pv > 1.0)) continue;
                float3 hv = histIn.sample(lin, pv * p.crop.xy + p.crop.zw, 0).rgb;
                float e = abs(toYCoCg(compress(sane(hv))).x - lcY);
                if (e < best) { best = e; vote = k; }
            }
        }

        // ── 6. Write: next history (+ own distance in alpha), shown picture (sRGB) ──
        float3 color = sane(expand(max(fromYCoCg(result), 0.0)));
        histOut.write(float4(histEncode(color, alphaHist, gid, uint(p.shiftInt.w)),
                             alphaHist && depthOn ? ownDist : 0.0), gid, 0);
        outTex.write(half4(half3(enc_srgb(color)), half(ownA)), gid, 0);

        // Counted on 1 pixel in 64 (a 32-bit total over 30 s of full frames overflows).
        if (!simd_any(statPx)) return;
        if (!statPx) { sRej = 0; sOff = 0; sCnt = 0; sDep = 0; sDis = 0; sMov = 0; sMvSum = 0; vote = 4; jvote = 3; }
        uint r0 = simd_sum(sRej), r1 = simd_sum(sOff), r2 = simd_sum(sCnt), r3 = simd_sum(sDep);
        uint r4 = simd_sum(sDis), r5 = simd_sum(sMov), r6 = simd_sum(sMvSum);
        uint v0 = simd_sum(vote == 0u ? 1u : 0u), v1 = simd_sum(vote == 1u ? 1u : 0u);
        uint v2 = simd_sum(vote == 2u ? 1u : 0u), v3 = simd_sum(vote == 3u ? 1u : 0u);
        uint j0 = simd_sum(jvote == 0u ? 1u : 0u), j1 = simd_sum(jvote == 1u ? 1u : 0u);
        uint j2 = simd_sum(jvote == 2u ? 1u : 0u);
        if (simd_is_first()) {
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
            if (j0) atomic_fetch_add_explicit(&stats[11], j0, memory_order_relaxed);
            if (j1) atomic_fetch_add_explicit(&stats[12], j1, memory_order_relaxed);
            if (j2) atomic_fetch_add_explicit(&stats[13], j2, memory_order_relaxed);
        }
    }

    """
}
