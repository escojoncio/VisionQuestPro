// VPKTAA in the compositor: temporal antialiasing of the guest's eye picture, per eye, at the
// guest's own resolution, before the reprojection pass places it on the display.
//
// Why here and not in the driver: this is the one place that has, for every engine and every
// graphics path (VPVulkan, MoltenVK, GLES), the finished per-eye image, the head pose it was
// rendered with and the previous frame's. One compute dispatch per eye per GUEST frame (not per
// display frame: a guest frame shown twice is not resolved twice).
//
// Algorithm (temporal resolve in the PTVisionPro VPKTAA family, from PTVisionPro):
//   * history reprojection by the head-rotation delta between the two frames' render poses, using
//     each frame's own frustum tangents (rotation-only: the guest's depth is not reachable here);
//   * 3x3 neighbourhood variance clipping in YCoCg (clip towards the mean, gamma 1);
//   * history weight from the "percent" setting (0..100 -> 0..0.95), lowered where the history
//     left the frame or was clipped hard;
//   * the history keeps the unsharpened result; the picture shown gets an RCAS-style contrast-
//     adaptive sharpen ("sharpness" 0..1), so sharpening never feeds back into the history.
// Reset: first frame, eye size change, tangents change, or a head turn > 8 degrees between frames.
// Settings come from the app as environment (KL_VPKTAA=1, KL_VPKTAA_PCT, KL_VPKTAA_SHARP);
// off means this file does nothing at all.
import Metal
import simd
import Foundation

final class KleptonTAA {
    struct Params {
        var tanCur: SIMD4<Float>    // l r t b of the current frame
        var tanPrev: SIMD4<Float>   // l r t b of the history frame
        var rot: simd_float3x3      // current view dir -> previous view dir (column-major)
        var crop: SIMD2<Float>      // fraction of the texture the guest rendered (u, v)
        var historyWeight: Float
        var sharpness: Float
        var slice: UInt32           // source slice
        var flipY: UInt32           // 1: texture row 0 is the top of the picture
        var reset: UInt32
        var pad: UInt32 = 0
    }

    static let enabled: Bool = getenv("KL_VPKTAA").map { String(cString: $0) == "1" } ?? false
    private static func envF(_ k: String, _ d: Float) -> Float {
        guard let v = getenv(k), let f = Float(String(cString: v)) else { return d }
        return f
    }
    private let percent = min(max(KleptonTAA.envF("KL_VPKTAA_PCT", 80), 0), 100)
    private let sharp = min(max(KleptonTAA.envF("KL_VPKTAA_SHARP", 0.5), 0), 1)

    private let device: MTLDevice
    private var pipeline: MTLComputePipelineState?
    // Per eye: two history textures (ping-pong) and the displayed output.
    private var history: [[MTLTexture]] = [[], []]
    private var output: [MTLTexture?] = [nil, nil]
    private var cur = [0, 0]
    private var lastSerial: [UInt64] = [0, 0]
    private var lastQuat: [simd_quatf?] = [nil, nil]
    private var lastTan: [SIMD4<Float>] = [.zero, .zero]
    private var lastFlip: [Bool] = [false, false]
    private var said = false
    // Stats.
    private var resolved = 0, resets = 0, reused = 0
    private var lastStats = Date()

    init?(device: MTLDevice) {
        guard KleptonTAA.enabled else { return nil }
        self.device = device
        do {
            let lib = try device.makeLibrary(source: KleptonTAA.msl, options: nil)
            guard let fn = lib.makeFunction(name: "vpktaa_resolve") else { return nil }
            pipeline = try device.makeComputePipelineState(function: fn)
        } catch {
            NSLog("[taa] VPKTAA shader failed to build: \(error) — VPKTAA off")
            return nil
        }
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
    func resolve(cmd: MTLCommandBuffer, eye: Int, source: MTLTexture, slice: Int, flipY: Bool,
                 rendered: kl_ovrp_render_pose, haveRendered: Bool) -> MTLTexture? {
        guard let pipeline, haveRendered, eye == 0 || eye == 1,
              source.textureType == .type2DArray else { return nil }
        // Same guest frame as last time: show the same resolve again.
        if rendered.serial == lastSerial[eye], let o = output[eye] { reused += 1; return o }

        let w = source.width, h = source.height
        var reset = false
        if output[eye] == nil || output[eye]!.width != w || output[eye]!.height != h {
            guard let o = makeTex(w, h, "vpktaa out \(eye)"),
                  let h0 = makeTex(w, h, "vpktaa hist \(eye)a"),
                  let h1 = makeTex(w, h, "vpktaa hist \(eye)b") else { return nil }
            output[eye] = o; history[eye] = [h0, h1]; cur[eye] = 0
            reset = true
        }
        let t = rendered.tangents
        let tan: SIMD4<Float> = eye == 0 ? SIMD4(t.0.0, t.0.1, t.0.2, t.0.3)
                                         : SIMD4(t.1.0, t.1.1, t.1.2, t.1.3)
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
        // Tangents changing beyond jitter size (a frustum change, not a jitter) invalidates history.
        let dt = tan - lastTan[eye]
        if (dt * dt).max() > 0.0001 { reset = true }

        var crop = SIMD2<Float>(1, 1)
        let vp = eye == 0 ? rendered.viewport.0 : rendered.viewport.1
        if vp.2 > 0, vp.3 > 0, rendered.viewport_of.0 > 0, rendered.viewport_of.1 > 0 {
            crop = SIMD2(min(1, Float(vp.2) / Float(rendered.viewport_of.0)),
                         min(1, Float(vp.3) / Float(rendered.viewport_of.1)))
        }

        var p = Params(tanCur: tan, tanPrev: reset ? tan : lastTan[eye], rot: rot, crop: crop,
                       historyWeight: min(0.95, percent / 100 * 0.95), sharpness: sharp,
                       slice: UInt32(slice), flipY: flipY ? 1 : 0, reset: reset ? 1 : 0)
        let prevH = history[eye][cur[eye]], nextH = history[eye][cur[eye] ^ 1]
        guard let enc = cmd.makeComputeCommandEncoder() else { return nil }
        enc.label = "VPKTAA eye\(eye)"
        enc.setComputePipelineState(pipeline)
        enc.setTexture(source, index: 0)
        enc.setTexture(prevH, index: 1)
        enc.setTexture(nextH, index: 2)
        enc.setTexture(output[eye]!, index: 3)
        enc.setBytes(&p, length: MemoryLayout<Params>.stride, index: 0)
        let tg = MTLSize(width: 16, height: 16, depth: 1)
        enc.dispatchThreadgroups(MTLSize(width: (w + 15) / 16, height: (h + 15) / 16, depth: 1),
                                 threadsPerThreadgroup: tg)
        enc.endEncoding()
        cur[eye] ^= 1
        lastSerial[eye] = rendered.serial
        lastQuat[eye] = q
        lastTan[eye] = tan
        lastFlip[eye] = flipY
        resolved += 1
        if reset { resets += 1 }

        if !said {
            said = true
            NSLog("[taa] VPKTAA on: \(w)x\(h) per eye, history=\(Int(percent))%, sharp=\(sharp), "
                  + "jitter=none(natural head motion), reprojection=rotation-only (no guest depth)")
        }
        if Date().timeIntervalSince(lastStats) > 30 {
            lastStats = Date()
            NSLog("[taa] stats: resolves=\(resolved) resets=\(resets) reused=\(reused) (last 30 s)")
            resolved = 0; resets = 0; reused = 0
        }
        return output[eye]
    }

    static let msl = """
    #include <metal_stdlib>
    using namespace metal;
    struct Params {
        float4 tanCur; float4 tanPrev; float3x3 rot; float2 crop;
        float historyWeight; float sharpness; uint slice; uint flipY; uint reset; uint pad;
    };
    static float3 toYCoCg(float3 c) {
        return float3(0.25*c.r + 0.5*c.g + 0.25*c.b, 0.5*c.r - 0.5*c.b, -0.25*c.r + 0.5*c.g - 0.25*c.b);
    }
    static float3 fromYCoCg(float3 y) {
        return float3(y.x + y.y - y.z, y.x + y.z, y.x - y.y - y.z);
    }
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
    kernel void vpktaa_resolve(texture2d_array<float, access::sample> src [[texture(0)]],
                               texture2d_array<float, access::sample> histIn [[texture(1)]],
                               texture2d_array<float, access::write> histOut [[texture(2)]],
                               texture2d_array<float, access::write> out [[texture(3)]],
                               constant Params& p [[buffer(0)]],
                               uint2 gid [[thread_position_in_grid]]) {
        uint w = src.get_width(), h = src.get_height();
        if (gid.x >= w || gid.y >= h) return;
        constexpr sampler lin(filter::linear, address::clamp_to_edge, coord::normalized);
        int2 ip = int2(gid);
        int2 mx = int2(w - 1, h - 1);
        float3 c = src.read(uint2(ip), p.slice).rgb;
        // 3x3 neighbourhood: moments in YCoCg, and the cross for the sharpen.
        float3 m1 = 0, m2 = 0;
        float3 cn = 0, cmin = c, cmax = c;
        for (int dy = -1; dy <= 1; ++dy)
        for (int dx = -1; dx <= 1; ++dx) {
            float3 s = src.read(uint2(clamp(ip + int2(dx, dy), int2(0), mx)), p.slice).rgb;
            float3 y = toYCoCg(s);
            m1 += y; m2 += y * y;
            if ((dx == 0) != (dy == 0)) { cn += s; cmin = min(cmin, s); cmax = max(cmax, s); }
        }
        m1 /= 9.0; m2 /= 9.0;
        float3 sigma = sqrt(max(m2 - m1 * m1, 0.0));

        float2 uv = (float2(ip) + 0.5) / float2(w, h);
        float2 cuv = uv / p.crop;
        float3 res = c;
        if (p.reset == 0u && all(cuv <= 1.0)) {
            float3 d = p.rot * dirFromUV(cuv, p.tanCur, p.flipY);
            float wgt = p.historyWeight;
            if (d.z < -1e-4) {
                float2 puv = uvFromDir(d, p.tanPrev, p.flipY);
                if (all(puv >= 0.0) && all(puv <= 1.0)) {
                    float3 hist = histIn.sample(lin, puv * p.crop, 0).rgb;
                    float3 hy = toYCoCg(hist);
                    // Clip towards the mean (variance clipping).
                    float3 dc = hy - m1;
                    float3 ext = max(abs(dc) / max(sigma, float3(1e-4)), float3(1.0));
                    float s = max(ext.x, max(ext.y, ext.z));
                    float3 hc = m1 + dc / s;
                    // Less history where it had to be clipped hard (disocclusion, motion).
                    wgt *= clamp(1.5 - 0.5 * s, 0.25, 1.0);
                    res = mix(c, fromYCoCg(hc), wgt);
                }
            }
        }
        res = max(res, 0.0);
        histOut.write(float4(res, 1.0), uint2(ip), 0);
        // RCAS-style contrast-adaptive sharpen on the shown picture only.
        float3 o = res;
        if (p.sharpness > 0.0) {
            float3 amp = clamp(min(cmin, 1.0 - cmax) / max(cmax, 1e-4), 0.0, 1.0);
            float3 lobe = -sqrt(amp) * mix(0.0625, 0.2, p.sharpness);
            o = (res + lobe * cn) / (1.0 + 4.0 * lobe);
            o = clamp(o, min(cmin, res), max(cmax, res));
        }
        out.write(float4(o, 1.0), uint2(ip), 0);
    }
    """
}
