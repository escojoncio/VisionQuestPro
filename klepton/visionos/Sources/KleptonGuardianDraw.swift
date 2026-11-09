// KleptonGuardianDraw - what the guardian screen draws into the immersive space (Quest's guardian setup shows the room's mesh and the
// floor grid over passthrough): the scene mesh as a translucent wireframe and a grid at the guardian floor. Drawn by the compositor in
// the same Metal pass as the overlays (KleptonCompositor.drawGuardianScene), only while `active` (set by VQGuardianGate, which also
// forces passthrough): outside the guardian screen nothing here runs.
//
// Data: the mesh comes from kl_spatial (KleptonSpatial.handleMesh feeds it, throttled to 0.5 s per anchor); it is flattened to ONE
// world-space vertex/index list on a utility queue whenever kl_spatial_generation changes (at most once a second) and uploaded
// by the render thread. World space = ARKit's world, the frame of the head pose and of the guardian floor (before the game starts
// the guest space is that same frame; kl_spatial_from_guest undoes it anyway).
import Metal
import QuartzCore
import simd

enum KLGuardianScene {
    /// True while the guardian screen is up.
    nonisolated(unsafe) static var active = false

    private static let maxVerts = 200_000
    private static let maxTris = 300_000
    private static let lock = NSLock()
    nonisolated(unsafe) private static var pendingPos: [Float]?
    nonisolated(unsafe) private static var pendingIdx: [UInt32]?
    nonisolated(unsafe) private static var builtGen: UInt32 = .max
    nonisolated(unsafe) private static var building = false
    nonisolated(unsafe) private static var lastBuild: CFTimeInterval = 0
    // Render thread only.
    // Three slots in turn: the GPU may still be reading the previous frames' buffers when a new mesh is written.
    nonisolated(unsafe) private static var vbufs = [MTLBuffer?](repeating: nil, count: 3)
    nonisolated(unsafe) private static var ibufs = [MTLBuffer?](repeating: nil, count: 3)
    nonisolated(unsafe) private static var slot = 0
    nonisolated(unsafe) private static var indexCount = 0
    nonisolated(unsafe) private static var holding = false
    nonisolated(unsafe) private static var pipeline: MTLRenderPipelineState?
    nonisolated(unsafe) private static var pipelineFormat: MTLPixelFormat = .invalid
    nonisolated(unsafe) private static var pipelineFailed = false

    private static let msl = """
    #include <metal_stdlib>
    using namespace metal;
    struct VP { float4x4 m; };
    struct VO { float4 pos [[position]]; };
    vertex VO kl_gs_v(uint vid [[vertex_id]], constant VP *vp [[buffer(0)]],
                      device const packed_float3 *p [[buffer(1)]], ushort amp [[amplification_id]]) {
        VO o;
        o.pos = vp[amp].m * float4(float3(p[vid]), 1.0);
        return o;
    }
    fragment float4 kl_gs_f(constant float4 &c [[buffer(0)]]) { return c; }
    """

    // MARK: mesh (any thread -> utility queue -> render thread)

    /// Kicks a rebuild of the flattened mesh when the spatial store changed (render thread, cheap when nothing changed).
    private static func refreshMesh() {
        let gen = kl_spatial_generation()
        let now = CACurrentMediaTime()
        lock.lock()
        let go = gen != builtGen && !building && now - lastBuild >= 1.0
        if go { building = true; lastBuild = now }
        lock.unlock()
        guard go else { return }
        DispatchQueue.global(qos: .utility).async { build(gen) }
    }

    private static func build(_ gen: UInt32) {
        var pos = [Float]()
        var idx = [UInt32]()
        let kind = Int32(KLSP_MESH)
        for i in 0..<kl_spatial_count(kind) {
            var id = kl_uuid()
            var g = [Float](repeating: 0, count: 16)
            guard kl_spatial_get(kind, i, &id, &g) != 0 else { continue }
            var a = [Float](repeating: 0, count: 16)
            kl_spatial_from_guest(g, &a)
            let m = simd_float4x4(SIMD4<Float>(a[0], a[1], a[2], a[3]), SIMD4<Float>(a[4], a[5], a[6], a[7]),
                                  SIMD4<Float>(a[8], a[9], a[10], a[11]), SIMD4<Float>(a[12], a[13], a[14], a[15]))
            var nv: UInt32 = 0, nt: UInt32 = 0
            guard kl_spatial_mesh_info(id, 0, nil, &nv, 0, nil, &nt) != 0, nv > 0, nt > 0 else { continue }
            guard pos.count / 3 + Int(nv) <= maxVerts, idx.count / 3 + Int(nt) <= maxTris else { break }
            var v = [Float](repeating: 0, count: Int(nv) * 3)
            var t = [UInt32](repeating: 0, count: Int(nt) * 3)
            var gotV = nv, gotT = nt
            guard kl_spatial_mesh_info(id, nv, &v, &gotV, nt, &t, &gotT) != 0, gotV <= nv, gotT <= nt else { continue }
            v.removeLast(v.count - Int(gotV) * 3)
            t.removeLast(t.count - Int(gotT) * 3)
            let base = UInt32(pos.count / 3)
            for k in 0..<Int(gotV) {
                let p = m * SIMD4<Float>(v[3 * k], v[3 * k + 1], v[3 * k + 2], 1)
                pos.append(p.x); pos.append(p.y); pos.append(p.z)
            }
            for k in stride(from: 0, to: t.count, by: 3) where t[k] < gotV && t[k + 1] < gotV && t[k + 2] < gotV {
                idx.append(base + t[k]); idx.append(base + t[k + 1]); idx.append(base + t[k + 2])   // a bad index never reaches the GPU
            }
        }
        lock.lock()
        if active {                                    // the screen closed while this ran: nothing to keep
            pendingPos = pos
            pendingIdx = idx
            builtGen = gen
        }
        building = false
        lock.unlock()
    }

    /// Copies `data` into the next slot's buffer, reusing it when it is big enough (and not wastefully so).
    private static func fill(_ device: MTLDevice, _ bufs: inout [MTLBuffer?], _ data: UnsafeRawBufferPointer) -> Bool {
        guard data.count > 0 else { return false }
        if let b = bufs[slot], b.length >= data.count, b.length <= data.count * 2 {
            b.contents().copyMemory(from: data.baseAddress!, byteCount: data.count)
            return true
        }
        bufs[slot] = device.makeBuffer(bytes: data.baseAddress!, length: data.count, options: [])
        return bufs[slot] != nil
    }

    private static func upload(_ device: MTLDevice) {
        lock.lock()
        let pos = pendingPos, idx = pendingIdx
        pendingPos = nil; pendingIdx = nil
        lock.unlock()
        guard let pos, let idx else { return }
        slot = (slot + 1) % 3
        let okV = pos.withUnsafeBytes { fill(device, &vbufs, $0) }
        let okI = idx.withUnsafeBytes { fill(device, &ibufs, $0) }
        indexCount = okV && okI ? idx.count : 0
        holding = true
        NSLog("[guardian] room mesh for the setup screen: \(pos.count / 3) vertices, \(idx.count / 3) triangles")
    }

    /// The screen is closed: give back the GPU buffers and the cached lists (a rebuild starts again on the next activation).
    /// Called by the compositor every frame while inactive; one flag test once everything is released.
    static func releaseIfIdle() {
        guard holding || pendingPos != nil else { return }
        lock.lock()
        pendingPos = nil; pendingIdx = nil
        builtGen = .max
        lock.unlock()
        for k in 0..<3 { vbufs[k] = nil; ibufs[k] = nil }
        indexCount = 0
        holding = false
    }

    // MARK: pipeline

    private static func makePipeline(_ device: MTLDevice, format: MTLPixelFormat, amplification: Int) -> MTLRenderPipelineState? {
        if pipelineFormat == format, let pipeline { return pipeline }
        if pipelineFailed && pipelineFormat == format { return nil }
        pipelineFormat = format
        do {
            let lib = try device.makeLibrary(source: msl, options: nil)
            let d = MTLRenderPipelineDescriptor()
            d.vertexFunction = lib.makeFunction(name: "kl_gs_v")
            d.fragmentFunction = lib.makeFunction(name: "kl_gs_f")
            d.colorAttachments[0].pixelFormat = format
            d.depthAttachmentPixelFormat = .depth32Float
            d.maxVertexAmplificationCount = amplification
            // The overlay pipeline's blend: straight colour over what is there, alpha accumulating (transparent over passthrough).
            d.colorAttachments[0].isBlendingEnabled = true
            d.colorAttachments[0].rgbBlendOperation = .add
            d.colorAttachments[0].alphaBlendOperation = .add
            d.colorAttachments[0].sourceRGBBlendFactor = .sourceAlpha
            d.colorAttachments[0].sourceAlphaBlendFactor = .one
            d.colorAttachments[0].destinationRGBBlendFactor = .oneMinusSourceAlpha
            d.colorAttachments[0].destinationAlphaBlendFactor = .oneMinusSourceAlpha
            pipeline = try device.makeRenderPipelineState(descriptor: d)
            pipelineFailed = false
        } catch {
            pipeline = nil
            pipelineFailed = true
            NSLog("[guardian] the setup screen's wireframe pipeline failed to build: \(error)")
        }
        return pipeline
    }

    // MARK: draw

    /// Floor grid: 0.25 m cells over 6 x 6 m centred under the head (snapped to the cells so it does not swim), as a line list.
    private static func gridVertices(floorY: Float, centre: SIMD2<Float>) -> [Float] {
        let step: Float = 0.25, half: Float = 3
        let cx = (centre.x / step).rounded() * step, cz = (centre.y / step).rounded() * step
        var v = [Float]()
        var o = -half
        while o <= half + 0.001 {
            v += [cx + o, floorY, cz - half, cx + o, floorY, cz + half]
            v += [cx - half, floorY, cz + o, cx + half, floorY, cz + o]
            o += step
        }
        return v
    }

    /// Encodes the room mesh and the floor grid. `viewProj` is one projection * inverse(view) per view in the pass's order (the
    /// vertex shader indexes it by [[amplification_id]]). Render thread, inside the pass that holds the overlays.
    static func draw(_ enc: MTLRenderCommandEncoder, device: MTLDevice, colorFormat: MTLPixelFormat, amplification: Int,
                     depthState: MTLDepthStencilState?, viewProj: [simd_float4x4]) {
        guard active, !viewProj.isEmpty else { return }
        refreshMesh()
        upload(device)
        guard let pipe = makePipeline(device, format: colorFormat, amplification: amplification) else { return }
        let head = KLGuardian.headPosition()
        let floorY = KLGuardian.floorY() ?? ((head?.y ?? 1.15) - 1.15)
        enc.setRenderPipelineState(pipe)
        if let depthState { enc.setDepthStencilState(depthState) }
        enc.setCullMode(.none)
        viewProj.withUnsafeBytes { enc.setVertexBytes($0.baseAddress!, length: $0.count, index: 0) }

        if let vb = vbufs[slot], let ib = ibufs[slot], indexCount > 0 {
            enc.setTriangleFillMode(.lines)
            enc.setVertexBuffer(vb, offset: 0, index: 1)
            var c = SIMD4<Float>(0.25, 0.85, 1.0, 0.4)
            enc.setFragmentBytes(&c, length: MemoryLayout<SIMD4<Float>>.size, index: 0)
            enc.drawIndexedPrimitives(type: .triangle, indexCount: indexCount, indexType: .uint32, indexBuffer: ib, indexBufferOffset: 0)
            enc.setTriangleFillMode(.fill)
        }

        let grid = gridVertices(floorY: floorY, centre: SIMD2<Float>(head?.x ?? 0, head?.z ?? 0))
        grid.withUnsafeBytes { enc.setVertexBytes($0.baseAddress!, length: $0.count, index: 1) }
        // Brighter while the player holds it: the floor is following the hand.
        var c = KLGuardian.floorGrabbing ? SIMD4<Float>(1.0, 0.85, 0.2, 0.9) : SIMD4<Float>(0.4, 1.0, 0.5, 0.6)
        enc.setFragmentBytes(&c, length: MemoryLayout<SIMD4<Float>>.size, index: 0)
        enc.drawPrimitives(type: .line, vertexStart: 0, vertexCount: grid.count / 3)
    }
}
