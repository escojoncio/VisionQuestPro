// KleptonEnvDepth - the Metal side of environment depth (kl_envdepth.h): what
// Quest's XR_META_environment_depth / ovrp_*EnvironmentDepth* hand a game, made
// from the ARKit scene mesh.
//
// Each compositor frame `frame(recenter:)` asks the C side whether a picture is
// due (the guest created AND started a depth provider, and the newest picture is
// old or taken). If so, the scene mesh (or, when there is none yet, the planes)
// is rasterised from each eye's pose into a 320x320, 2-layer D16 image, FORWARD-Z (clear 1)
// in [0,1] with near/far from the job (see kl_envdepth.h for the exact
// convention). No game asking for depth = no work, not even a lock beyond one
// mutex in kl_envdepth_job_begin.
//
// Two ways the image reaches the guest:
//   * Vulkan guest: the target is the MTLTexture that backs the guest's own
//     VkImage, drawn into directly (no copy);
//   * GL guest: a depth image Metal shares with a GL name that already has its
//     own storage is not possible, so the same picture is drawn into a private
//     depth target while the fragment stage also writes the depth to an R16
//     colour target, which is read back into the C ring (kl_envdepth_acquire
//     uploads it on the guest's GL thread).
//
// Geometry. ARKit meshes are copied (vertices + triangles) into shared MTLBuffers
// on a background queue (one job at a time): counts are checked every
// `refreshInterval` (~0.1 s), everything is re-read every `fullCopyInterval`
// (~2 s; a mesh anchor that changed without changing its sizes), and the per-frame work is one draw per mesh with its CURRENT pose
// (kl_spatial_get, guest space: recenter and origin height already applied).
//
// Also here: the compositor's recenter is handed to kl_spatial (every consumer
// of a pose from the store needs it); it is the one thing this file does that is
// not depth.
import Metal
import QuartzCore
import simd

enum KleptonEnvDepth {
    nonisolated(unsafe) private static var renderer: Renderer?
    nonisolated(unsafe) private static var failed = false
    nonisolated(unsafe) private static var lastRecenter: simd_float4x4?

    /// Called once per compositor frame, on the compositor thread, with the
    /// compositor's `recenterXform`.
    static func frame(recenter: simd_float4x4) {
        setRecenter(recenter)
        var job = kl_envdepth_job()
        guard !failed, kl_envdepth_job_begin(&job) != 0 else { return }
        if renderer == nil {
            renderer = Renderer()
            if renderer == nil {
                failed = true
                NSLog("[envdepth] no Metal device / pipeline: environment depth stays empty")
            }
        }
        guard let renderer else {
            kl_envdepth_job_done(&job, 0)
            return
        }
        renderer.render(job)
    }

    private static func setRecenter(_ m: simd_float4x4) {
        if let l = lastRecenter,
           l.columns.0 == m.columns.0, l.columns.1 == m.columns.1,
           l.columns.2 == m.columns.2, l.columns.3 == m.columns.3 { return }
        lastRecenter = m
        withUnsafePointer(to: m) {
            $0.withMemoryRebound(to: Float.self, capacity: 16) { kl_spatial_set_recenter($0) }
        }
    }

    // MARK: - shader

    /// Vertices are tightly packed float3 (ARKit mesh vertices, or the plane's
    /// four corners), transformed by one matrix. The depth-only fragment writes
    /// nothing: the rasteriser's depth is the picture. The colour variant (GL
    /// guests) also writes that depth to an R16 target for the readback.
    private static let msl = """
    #include <metal_stdlib>
    using namespace metal;
    struct EDOut { float4 pos [[position]]; };
    vertex EDOut envdepth_vs(uint vid [[vertex_id]],
                             device const packed_float3 *p [[buffer(0)]],
                             constant float4x4 &mvp [[buffer(1)]]) {
        EDOut o;
        o.pos = mvp * float4(float3(p[vid]), 1.0);
        return o;
    }
    fragment void envdepth_fs() {}
    fragment float envdepth_fs_color(EDOut in [[stage_in]]) { return in.pos.z; }
    """

    // MARK: - geometry cache

    private struct Key: Hashable { var a: UInt64; var b: UInt64 }
    private static func key(_ id: kl_uuid) -> Key {
        withUnsafeBytes(of: id.b) { Key(a: $0.loadUnaligned(fromByteOffset: 0, as: UInt64.self),
                                        b: $0.loadUnaligned(fromByteOffset: 8, as: UInt64.self)) }
    }

    private struct Mesh {
        let vertices: MTLBuffer
        let indices: MTLBuffer
        let nv: UInt32
        let nt: UInt32
        let center: SIMD3<Float>
        let radius: Float
    }

    // MARK: - renderer

    private final class Renderer {
        let device: MTLDevice
        let queue: MTLCommandQueue
        let vs: MTLFunction
        let fs: MTLFunction
        let fsColor: MTLFunction
        let depthState: MTLDepthStencilState
        var psoDepth: [UInt: MTLRenderPipelineState] = [:]    // by depth pixel format
        var psoColor: MTLRenderPipelineState?
        var scratchDepth: MTLTexture?        // GL guest: private depth target
        var colorTex: MTLTexture?            // GL guest: R16 readback target, 2 layers

        let lock = NSLock()
        var meshes: [Key: Mesh] = [:]
        var refreshing = false
        var pictures = 0                     // pictures rendered (diagnostics)
        var lastDrawn = -1                   // meshes + planes drawn in the last picture logged
        var lastRefresh: CFTimeInterval = 0
        var lastFull: CFTimeInterval = 0
        static let refreshInterval: CFTimeInterval = 0.1     // geometry check (counts) at most this often
        static let fullCopyInterval: CFTimeInterval = 2.0    // full re-copy even when the sizes did not change
        let geo = DispatchQueue(label: "klepton.envdepth.geometry", qos: .utility)

        init?() {
            guard let d = MTLCreateSystemDefaultDevice(), let q = d.makeCommandQueue() else { return nil }
            do {
                let before = kl_mtl_attr_probe(d)
                let lib = try d.makeLibrary(source: KleptonEnvDepth.msl, options: nil)
                kl_mtl_track_obj("env.lib", lib, before)
                guard let v = lib.makeFunction(name: "envdepth_vs"),
                      let f = lib.makeFunction(name: "envdepth_fs"),
                      let fc = lib.makeFunction(name: "envdepth_fs_color") else { return nil }
                vs = v; fs = f; fsColor = fc
            } catch {
                NSLog("[envdepth] shader FAILED: \(error)")
                return nil
            }
            let dd = MTLDepthStencilDescriptor()
            dd.depthCompareFunction = .less         // forward-Z: the smaller d is the nearer surface
            dd.isDepthWriteEnabled = true
            guard let ds = d.makeDepthStencilState(descriptor: dd) else { return nil }
            device = d
            queue = q
            depthState = ds
            NSLog("[envdepth] depth convention: forward-Z (0 = near, 1 = far / no geometry), clear 1, near \(KLED_NEAR) far \(KLED_FAR), D16, \(KLED_W)x\(KLED_H) x2 eyes")
        }

        // MARK: pipelines

        func pipeline(depthFormat: MTLPixelFormat, color: Bool) -> MTLRenderPipelineState? {
            if color, let p = psoColor, depthFormat == .depth16Unorm { return p }
            if !color, let p = psoDepth[depthFormat.rawValue] { return p }
            let d = MTLRenderPipelineDescriptor()
            d.vertexFunction = vs
            d.fragmentFunction = color ? fsColor : fs
            d.depthAttachmentPixelFormat = depthFormat
            if color { d.colorAttachments[0].pixelFormat = .r16Unorm }
            do {
                let before = kl_mtl_attr_probe(device)
                let p = try device.makeRenderPipelineState(descriptor: d)
                kl_mtl_track_obj("env.pso", p, before)
                if color { psoColor = p } else { psoDepth[depthFormat.rawValue] = p }
                return p
            } catch {
                NSLog("[envdepth] pipeline FAILED (depth \(depthFormat.rawValue), color \(color)): \(error)")
                return nil
            }
        }

        // MARK: geometry refresh (background)

        func refreshIfDue() {
            let now = CACurrentMediaTime()
            lock.lock()
            let due = !refreshing && now - lastRefresh > Renderer.refreshInterval
            let full = due && now - lastFull > Renderer.fullCopyInterval
            if due { refreshing = true; lastRefresh = now }
            if full { lastFull = now }
            let old = meshes
            lock.unlock()
            guard due else { return }
            geo.async { [self] in
                var next: [Key: Mesh] = [:]
                let n = kl_spatial_count(Int32(KLSP_MESH))
                var m4 = [Float](repeating: 0, count: 16)
                for i in 0..<n {
                    var id = kl_uuid()
                    guard kl_spatial_get(Int32(KLSP_MESH), i, &id, &m4) != 0 else { continue }
                    var nv: UInt32 = 0, nt: UInt32 = 0
                    guard kl_spatial_mesh_info(id, 0, nil, &nv, 0, nil, &nt) != 0, nv > 0, nt > 0 else { continue }
                    let k = KleptonEnvDepth.key(id)
                    if !full, let o = old[k], o.nv == nv, o.nt == nt { next[k] = o; continue }
                    var v = [Float](repeating: 0, count: Int(nv) * 3)
                    var t = [UInt32](repeating: 0, count: Int(nt) * 3)
                    var gv = nv, gt = nt
                    guard kl_spatial_mesh_info(id, nv, &v, &gv, nt, &t, &gt) != 0, gv > 0, gt > 0 else { continue }
                    // The mesh can grow between the two calls: only what was copied is drawn.
                    let tris = min(gt, nt)
                    gv = min(gv, nv)
                    // A mesh replaced in between can carry indices past the vertices copied: skip it this round.
                    var maxIx: UInt32 = 0
                    for j in 0..<(Int(tris) * 3) { maxIx = max(maxIx, t[j]) }
                    guard gv > 0, tris > 0, maxIx < gv else { continue }
                    guard let vb = device.makeBuffer(bytes: v, length: v.count * 4, options: .storageModeShared),
                          let ib = device.makeBuffer(bytes: t, length: Int(tris) * 12, options: .storageModeShared)
                    else { continue }
                    kl_mtl_track("env.mesh", vb)
                    kl_mtl_track("env.mesh", ib)
                    var lo = SIMD3<Float>(repeating: .greatestFiniteMagnitude), hi = -lo
                    for j in 0..<Int(gv) {
                        let p = SIMD3<Float>(v[j * 3], v[j * 3 + 1], v[j * 3 + 2])
                        lo = simd_min(lo, p); hi = simd_max(hi, p)
                    }
                    let c = (lo + hi) * 0.5
                    next[k] = Mesh(vertices: vb, indices: ib, nv: gv, nt: tris, center: c, radius: simd_length(hi - c))
                }
                lock.lock()
                meshes = next
                refreshing = false
                lock.unlock()
            }
        }

        // MARK: matrices

        /// Eye pose {px,py,pz, qx,qy,qz,qw} -> view matrix (world -> eye).
        static func view(_ p: [Float]) -> simd_float4x4 {
            var m = simd_float4x4(simd_quatf(ix: p[3], iy: p[4], iz: p[5], r: p[6]))
            m.columns.3 = SIMD4<Float>(p[0], p[1], p[2], 1)
            return m.inverse
        }

        /// Asymmetric perspective from the four positive tangents, eye looks down -Z,
        /// Metal clip depth [0,1], FORWARD-Z: d = f (z - n) / ((f - n) z) (z = n -> 0, z = f -> 1), the
        /// convention of kl_envdepth.h (kled_proj_ab in kl_envdepth.c is the tested form of a and b).
        static func projection(_ t: [Float], near n: Float, far f: Float) -> simd_float4x4 {
            let l = t[0], r = t[1], u = t[2], d = t[3]
            let a = -f / (f - n), b = -f * n / (f - n)
            return simd_float4x4(columns: (
                SIMD4<Float>(2 / (l + r), 0, 0, 0),
                SIMD4<Float>(0, 2 / (u + d), 0, 0),
                SIMD4<Float>((r - l) / (r + l), (u - d) / (u + d), a, -1),
                SIMD4<Float>(0, 0, b, 0)))
        }

        // MARK: render

        func render(_ jobIn: kl_envdepth_job) {
            var job = jobIn
            refreshIfDue()
            let w = Int(job.w), h = Int(job.h)

            // Target. Vulkan: the guest image's own texture; GL: private.
            let depthTarget: MTLTexture
            let useColor = job.mtl_tex == nil
            if let raw = job.mtl_tex {
                depthTarget = Unmanaged<MTLTexture>.fromOpaque(raw).takeUnretainedValue()
                guard depthTarget.usage.contains(.renderTarget), depthTarget.arrayLength >= 2 else {
                    NSLog("[envdepth] the guest's depth image is not drawable (usage \(depthTarget.usage.rawValue), layers \(depthTarget.arrayLength))")
                    kl_envdepth_job_done(&job, 0)
                    return
                }
            } else {
                if scratchDepth == nil || scratchDepth!.width != w || scratchDepth!.height != h {
                    let dd = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .depth16Unorm, width: w, height: h, mipmapped: false)
                    dd.usage = .renderTarget
                    dd.storageMode = .private
                    scratchDepth = device.makeTexture(descriptor: dd)
                    kl_mtl_track("env.scratch_depth", scratchDepth)
                    let cd = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .r16Unorm, width: w, height: h, mipmapped: false)
                    cd.textureType = .type2DArray
                    cd.arrayLength = 2
                    cd.usage = .renderTarget
                    cd.storageMode = .shared
                    colorTex = device.makeTexture(descriptor: cd)
                    kl_mtl_track("env.color", colorTex)
                }
                guard let sd = scratchDepth, colorTex != nil else {
                    kl_envdepth_job_done(&job, 0)
                    return
                }
                depthTarget = sd
            }
            guard let pso = pipeline(depthFormat: depthTarget.pixelFormat, color: useColor),
                  let cmd = queue.makeCommandBuffer() else {
                kl_envdepth_job_done(&job, 0)
                return
            }

            lock.lock()
            let snapshot = meshes
            lock.unlock()
            let nMesh = kl_spatial_count(Int32(KLSP_MESH))
            let nPlane = snapshot.isEmpty ? kl_spatial_count(Int32(KLSP_PLANE)) : 0

            // Poses, once for both eyes.
            let poses = [withUnsafeBytes(of: job.pose.0) { Array($0.bindMemory(to: Float.self)) },
                         withUnsafeBytes(of: job.pose.1) { Array($0.bindMemory(to: Float.self)) }]
            let tans = [withUnsafeBytes(of: job.tan.0) { Array($0.bindMemory(to: Float.self)) },
                        withUnsafeBytes(of: job.tan.1) { Array($0.bindMemory(to: Float.self)) }]
            let mid = (SIMD3<Float>(poses[0][0], poses[0][1], poses[0][2]) +
                       SIMD3<Float>(poses[1][0], poses[1][1], poses[1][2])) * 0.5

            // The scene, in guest space, once: (mesh, model) pairs / plane quads.
            var drawMeshes: [(Mesh, simd_float4x4)] = []
            var m4 = [Float](repeating: 0, count: 16)
            for i in 0..<nMesh {
                var id = kl_uuid()
                guard kl_spatial_get(Int32(KLSP_MESH), i, &id, &m4) != 0,
                      let mesh = snapshot[KleptonEnvDepth.key(id)] else { continue }
                let model = m4.withUnsafeBytes { simd_float4x4(columns: (
                    $0.loadUnaligned(fromByteOffset: 0, as: SIMD4<Float>.self), $0.loadUnaligned(fromByteOffset: 16, as: SIMD4<Float>.self),
                    $0.loadUnaligned(fromByteOffset: 32, as: SIMD4<Float>.self), $0.loadUnaligned(fromByteOffset: 48, as: SIMD4<Float>.self))) }
                let c = model * SIMD4<Float>(mesh.center, 1)
                if simd_distance(SIMD3<Float>(c.x, c.y, c.z), mid) - mesh.radius > job.far_z { continue }
                drawMeshes.append((mesh, model))
            }
            var drawPlanes: [(simd_float4x4, Float, Float)] = []
            for i in 0..<nPlane {
                var id = kl_uuid()
                guard kl_spatial_get(Int32(KLSP_PLANE), i, &id, &m4) != 0,
                      kl_spatial_is_synthetic(id) == 0 else { continue }   // synthetic room (fixed 2.5 m ceiling) is not real geometry to occlude with
                var pw: Float = 0, ph: Float = 0
                var label: Int32 = 0
                var np: UInt32 = 0
                guard kl_spatial_plane_info(id, &pw, &ph, &label, 0, nil, &np) != 0, pw > 0, ph > 0 else { continue }
                let model = m4.withUnsafeBytes { simd_float4x4(columns: (
                    $0.loadUnaligned(fromByteOffset: 0, as: SIMD4<Float>.self), $0.loadUnaligned(fromByteOffset: 16, as: SIMD4<Float>.self),
                    $0.loadUnaligned(fromByteOffset: 32, as: SIMD4<Float>.self), $0.loadUnaligned(fromByteOffset: 48, as: SIMD4<Float>.self))) }
                drawPlanes.append((model, pw, ph))
            }

            // What goes into the picture: the first one, whenever it changes between nothing and something, and now and then.
            pictures += 1
            let drawn = drawMeshes.count + drawPlanes.count
            if pictures == 1 || (drawn == 0) != (lastDrawn == 0) || pictures % 900 == 0 {
                lastDrawn = drawn
                var tris = 0
                for (mesh, _) in drawMeshes { tris += Int(mesh.nt) }
                NSLog("[envdepth] picture \(pictures): \(drawMeshes.count) of \(nMesh) scene meshes (\(tris) triangles), \(drawPlanes.count) planes, eye0 at (\(poses[0][0]), \(poses[0][1]), \(poses[0][2])), tangents \(tans[0]), into \(useColor ? "a private image (GL readback)" : "the guest's image") \(depthTarget.width)x\(depthTarget.height) format \(depthTarget.pixelFormat.rawValue)")
            }

            for eye in 0..<2 {
                let pd = MTLRenderPassDescriptor()
                pd.depthAttachment.texture = depthTarget
                pd.depthAttachment.slice = useColor ? 0 : eye
                pd.depthAttachment.loadAction = .clear
                pd.depthAttachment.clearDepth = 1.0          // forward-Z: no geometry = 1 (the far plane)
                pd.depthAttachment.storeAction = useColor ? .dontCare : .store
                if useColor, let ct = colorTex {
                    pd.colorAttachments[0].texture = ct
                    pd.colorAttachments[0].slice = eye
                    pd.colorAttachments[0].loadAction = .clear
                    pd.colorAttachments[0].clearColor = MTLClearColor(red: 1, green: 0, blue: 0, alpha: 0)   // no geometry = 1
                    pd.colorAttachments[0].storeAction = .store
                }
                guard let enc = cmd.makeRenderCommandEncoder(descriptor: pd) else { continue }
                enc.setRenderPipelineState(pso)
                enc.setDepthStencilState(depthState)
                enc.setCullMode(.none)
                let vp = Renderer.projection(tans[eye], near: job.near_z, far: job.far_z) * Renderer.view(poses[eye])
                for (mesh, model) in drawMeshes {
                    var mvp = vp * model
                    enc.setVertexBuffer(mesh.vertices, offset: 0, index: 0)
                    enc.setVertexBytes(&mvp, length: MemoryLayout<simd_float4x4>.stride, index: 1)
                    enc.drawIndexedPrimitives(type: .triangle, indexCount: Int(mesh.nt) * 3,
                                              indexType: .uint32, indexBuffer: mesh.indices, indexBufferOffset: 0)
                }
                for (model, pw, ph) in drawPlanes {
                    // The plane's rectangle in the anchor's X/Z (normal +Y), centred.
                    var quad: [Float] = [-pw / 2, 0, -ph / 2,  pw / 2, 0, -ph / 2,
                                         -pw / 2, 0,  ph / 2,  pw / 2, 0,  ph / 2]
                    var mvp = vp * model
                    enc.setVertexBytes(&quad, length: quad.count * 4, index: 0)
                    enc.setVertexBytes(&mvp, length: MemoryLayout<simd_float4x4>.stride, index: 1)
                    enc.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
                }
                enc.endEncoding()
            }

            let done = job
            let ct = useColor ? colorTex : nil
            cmd.addCompletedHandler { b in
                var j = done
                let ok = b.status == .completed
                if !ok { NSLog("[envdepth] command buffer \(b.status.rawValue): \(String(describing: b.error))") }
                if ok, let ct, let dst = j.cpu {
                    let per = Int(j.w) * Int(j.h)
                    for eye in 0..<2 {
                        ct.getBytes(dst + eye * per, bytesPerRow: Int(j.w) * 2,
                                    bytesPerImage: 0,
                                    from: MTLRegionMake2D(0, 0, Int(j.w), Int(j.h)),
                                    mipmapLevel: 0, slice: eye)
                    }
                }
                kl_envdepth_job_done(&j, ok ? 1 : 0)
            }
            cmd.commit()
        }
    }
}
