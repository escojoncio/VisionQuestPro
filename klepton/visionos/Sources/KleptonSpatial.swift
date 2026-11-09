// KleptonSpatial - the ARKit side of the spatial store (kl_spatial.h): plane
// detection, room tracking and scene reconstruction providers, and world
// anchors. KleptonCompositor.runARKit is the only caller of the two hooks below.
//
// What flows where:
//   ARKit anchorUpdates --(this file)--> kl_spatial_plane/_mesh/_room/_anchor
//   guest ovrp_/xr calls --(kl_spatial.c)--> kl_spatial_host --> addAnchor/removeAnchor
//                                         <-- kl_spatial_request_done
//
// Frames. The store keeps ARKit's world frame; kl_spatial.c converts to the
// guest's tracking space on every read. What this file sends:
//   * planes: the contract's originFromAnchor (ARKit frame: normal +Y, rectangle
//     in X/Z), but NORMALISED so that the consumers' Rx(-90deg) to the Meta
//     convention (rectangle in XY, normal +Z) lands right: floors/tables face
//     up, ceilings down, and walls are rolled so the Meta +Y (= -Z here) is up.
//     Built as extent frame (originFromAnchor * extent.anchorFromExtentTransform,
//     which is already XY/+Z), fixed, then multiplied by Rx(+90deg);
//   * meshes / anchors: originFromAnchor untouched (vertices stay local).
// No handedness flip: ARKit and the Oculus tracking space are both right
// handed, +Y up (see KleptonCompositor.decompose).
//
// The providers run in every immersive session (KL_ENV_CAPTURE, the app setting «Captura de
// entorno», default on) so the Guardian gets the floor and the room (KleptonGuardian.swift,
// kl_guardian). What the GAME sees is still decided by the manifest (kl_spatial_set_policy) or
// KL_SPATIAL=1: kl_spatial_set_available is told only the exposed part. With the setting off,
// nothing is requested unless the game asked for it, as before.
import ARKit
import simd
import QuartzCore

enum KleptonSpatial {
    nonisolated(unsafe) private static var plane: PlaneDetectionProvider?
    nonisolated(unsafe) private static var scene: SceneReconstructionProvider?
    nonisolated(unsafe) private static var room: RoomTrackingProvider?
    nonisolated(unsafe) private static var started = false
    nonisolated(unsafe) private static var wantsAnchors = false
    nonisolated(unsafe) private static var exposesScene = false   // the game may see planes / mesh / rooms (policy or KL_SPATIAL=1)
    nonisolated(unsafe) private static var pendingMesh: [UUID: MeshAnchor] = [:]
    nonisolated(unsafe) private static var meshStamp: [UUID: CFTimeInterval] = [:]
    private static let meshLock = NSLock()
    private static let meshMinInterval: CFTimeInterval = 0.5

    private static func envOn(_ name: String, _ dflt: Bool) -> Bool {
        guard let v = ProcessInfo.processInfo.environment[name] else { return dflt }
        return !(v == "0" || v.lowercased() == "false" || v.isEmpty)
    }

    /// Extra providers to add to the ARKit session's `run` (the set is fixed at
    /// `run`). Empty unless environment capture is on (KL_ENV_CAPTURE, default on) or the
    /// game asked for scene / anchors (kl_spatial_set_policy), and the user authorised world sensing.
    static func providers() -> [any DataProvider] {
        var wantsScene: Int32 = 0
        var wantsAnch: Int32 = 0
        kl_spatial_policy(&wantsScene, &wantsAnch)
        let force = envOn("KL_SPATIAL", false)
        wantsAnchors = wantsAnch != 0 || force
        exposesScene = wantsScene != 0 || force
        guard exposesScene || envOn("KL_ENV_CAPTURE", true) else { return [] }
        var list: [any DataProvider] = []
        if plane == nil, PlaneDetectionProvider.isSupported {
            plane = PlaneDetectionProvider(alignments: [.horizontal, .vertical])
        }
        if let plane { list.append(plane) }
        if room == nil, RoomTrackingProvider.isSupported {
            room = RoomTrackingProvider()
        }
        if let room { list.append(room) }
        // The mesh is the expensive one (continuous reconstruction): KL_SPATIAL_MESH=0 leaves it out.
        if scene == nil, envOn("KL_SPATIAL_MESH", true), SceneReconstructionProvider.isSupported {
            scene = SceneReconstructionProvider()
        }
        if let scene { list.append(scene) }
        return list
    }

    /// `run` refused the extra providers: forget them so nothing waits on their streams.
    static func dropProviders() { plane = nil; scene = nil; room = nil }

    /// Called right after `arSession.run` succeeded with `providers()`; start the
    /// tasks that consume their `anchorUpdates` and feed `kl_spatial_*`.
    static func sessionStarted(_ session: ARKitSession) {
        var wantsScene: Int32 = 0
        var wantsAnch: Int32 = 0
        kl_spatial_policy(&wantsScene, &wantsAnch)
        let wanted = plane != nil || room != nil || scene != nil || wantsAnchors || wantsAnch != 0
        guard wanted, !started else { return }
        started = true
        installHost()
        Task.detached { await monitor(session) }
    }

    // MARK: authorization and the consumer tasks

    private static func monitor(_ session: ARKitSession) async {
        let both = await session.queryAuthorization(for: [.handTracking, .worldSensing])
        NSLog("[spatial] authorization: handTracking=\(String(describing: both[.handTracking])) "
              + "worldSensing=\(String(describing: both[.worldSensing]))")
        var status = both[.worldSensing]
        // `run` already raised the prompt if it was needed: wait for the answer rather than asking twice.
        var waited = 0
        while status == .notDetermined && waited < 180 {
            try? await Task.sleep(nanoseconds: 1_000_000_000)
            waited += 1
            status = await session.queryAuthorization(for: [.worldSensing])[.worldSensing]
        }
        guard status == .allowed else {
            NSLog("[spatial] world sensing not authorised (\(String(describing: status))): scene and anchors unavailable")
            kl_spatial_set_available(0, 0, 0, 0)
            return
        }
        let tracker = KleptonCompositor.headAtTracker
        let anchorsOk = tracker != nil && WorldTrackingProvider.isSupported
        // Captured always, exposed to the game only if its manifest (or KL_SPATIAL=1) asked: a game without
        // USE_SCENE sees no planes / mesh / rooms, one without USE_SCENE and USE_ANCHOR_API no anchors.
        let exposeScene = exposesScene
        let exposeAnchors = anchorsOk && (exposeScene || wantsAnchors)
        kl_spatial_set_available(exposeScene && plane != nil ? 1 : 0, exposeScene && scene != nil ? 1 : 0,
                                 exposeScene && room != nil ? 1 : 0, exposeAnchors ? 1 : 0)
        NSLog("[spatial] running: planes=\(plane != nil) mesh=\(scene != nil) room=\(room != nil) anchors=\(anchorsOk) "
              + "exposed to the game: scene=\(exposeScene) anchors=\(exposeAnchors)")
        if let plane {
            Task.detached { for await u in plane.anchorUpdates { handlePlane(u) } }
        }
        if let room {
            Task.detached { for await u in room.anchorUpdates { handleRoom(u) } }
        }
        if let scene {
            Task.detached { for await u in scene.anchorUpdates { handleMesh(u) } }
            Task.detached { await flushMeshes() }
        }
        if exposeAnchors, let tracker {
            Task.detached { for await u in tracker.anchorUpdates { handleWorldAnchor(u) } }
        }
        // Live room: the capture keeps running while the game plays, so the room the game was handed is re-checked about once a
        // second (kl_spatial.c rate-limits to 2 s, skips when the store did not change, and tells the game when it changed for real).
        // Only a game that sees the scene is concerned; the guardian's floor is never written by it.
        if exposeScene && (plane != nil || room != nil || scene != nil) {
            Task.detached {
                while !Task.isCancelled {
                    try? await Task.sleep(nanoseconds: 1_000_000_000)
                    if Task.isCancelled { break }
                    _ = kl_ovrp_spatial_live_tick()
                }
            }
        }
    }

    // MARK: matrix plumbing

    private static func withM4<R>(_ m: simd_float4x4, _ body: (UnsafePointer<Float>) -> R) -> R {
        var mm = m
        return withUnsafePointer(to: &mm) { p in
            p.withMemoryRebound(to: Float.self, capacity: 16) { body($0) }
        }
    }

    private static func matrix(_ p: UnsafePointer<Float>) -> simd_float4x4 {
        simd_float4x4(SIMD4<Float>(p[0], p[1], p[2], p[3]), SIMD4<Float>(p[4], p[5], p[6], p[7]),
                      SIMD4<Float>(p[8], p[9], p[10], p[11]), SIMD4<Float>(p[12], p[13], p[14], p[15]))
    }

    private static func uuid(_ id: UUID) -> kl_uuid { kl_uuid(b: id.uuid) }

    // MARK: planes

    /// Rx(+90deg): extent/Meta frame -> ARKit anchor frame (inverse of the consumers' Rx(-90deg)).
    private static let rx90 = simd_float4x4(SIMD4<Float>(1, 0, 0, 0), SIMD4<Float>(0, 0, 1, 0),
                                            SIMD4<Float>(0, -1, 0, 0), SIMD4<Float>(0, 0, 0, 1))

    private static func label(_ c: PlaneAnchor.Classification) -> Int32 {
        switch c {
        case .floor: return Int32(KLSP_LBL_FLOOR)
        case .ceiling: return Int32(KLSP_LBL_CEILING)
        case .wall: return Int32(KLSP_LBL_WALL)
        case .table: return Int32(KLSP_LBL_TABLE)
        case .seat: return Int32(KLSP_LBL_COUCH)
        case .door: return Int32(KLSP_LBL_DOOR)
        case .window: return Int32(KLSP_LBL_WINDOW)
        default: return Int32(KLSP_LBL_OTHER)
        }
    }

    /// Meta frame for a plane rectangle: normal +Z up for floors/tables and down
    /// for ceilings; walls roll so local +Y is up (width/height follow the roll).
    private static func fixPlane(_ m: inout simd_float4x4, _ w: inout Float, _ h: inout Float,
                                 lbl: Int32, vertical: Bool) {
        if vertical {
            if abs(m.columns.0.y) > abs(m.columns.1.y) {
                let x = m.columns.0
                m.columns.0 = m.columns.1
                m.columns.1 = -x                      // +90 degrees about local Z
                swap(&w, &h)
            }
            if m.columns.1.y < 0 {
                m.columns.0 = -m.columns.0
                m.columns.1 = -m.columns.1            // 180 degrees about local Z
            }
        } else {
            let wantDown = lbl == Int32(KLSP_LBL_CEILING)
            let ny = m.columns.2.y
            if (wantDown && ny > 0) || (!wantDown && ny < 0) {
                m.columns.0 = -m.columns.0
                m.columns.2 = -m.columns.2            // 180 degrees about local Y
            }
        }
    }

    private static func handlePlane(_ u: AnchorUpdate<PlaneAnchor>) {
        KLFloorEstimator.plane(u)                       // the Guardian's automatic floor
        let a = u.anchor
        let id = uuid(a.id)
        var m = a.originFromAnchorTransform * a.geometry.extent.anchorFromExtentTransform
        var w = a.geometry.extent.width
        var h = a.geometry.extent.height
        let lbl = label(a.classification)
        let vertical = a.alignment == .vertical
        fixPlane(&m, &w, &h, lbl: lbl, vertical: vertical)
        m = m * rx90                                    // back to the anchor frame (normal +Y)
        let removed: Int32 = u.event == .removed ? 1 : 0
        withM4(m) { kl_spatial_plane(id, removed, $0, w, h, lbl, nil, 0) }
    }

    // MARK: rooms

    private static func handleRoom(_ u: AnchorUpdate<RoomAnchor>) {
        let r = u.anchor
        let ids = r.planeAnchorIDs.map { uuid($0) }
        let removed: Int32 = u.event == .removed ? 1 : 0
        ids.withUnsafeBufferPointer { b in
            kl_spatial_room(uuid(r.id), removed, b.baseAddress, UInt32(b.count))
        }
        // The room outline is the guardian's default play space whether or not the game asks for the scene (rate-limited in C).
        if removed == 0, let head = KLGuardian.headPosition() { _ = kl_spatial_update_guardian_default(head.x, head.z) }
    }

    // MARK: scene mesh

    private static func handleMesh(_ u: AnchorUpdate<MeshAnchor>) {
        let a = u.anchor
        if u.event == .removed {
            meshLock.lock(); pendingMesh[a.id] = nil; meshStamp[a.id] = nil; meshLock.unlock()
            withM4(matrix_identity_float4x4) { kl_spatial_mesh(uuid(a.id), 1, $0, nil, 0, nil, 0) }
            return
        }
        let now = CACurrentMediaTime()
        meshLock.lock()
        let last = meshStamp[a.id] ?? 0
        if now - last < meshMinInterval {
            pendingMesh[a.id] = a                     // sent by flushMeshes once it has settled
            meshLock.unlock()
            return
        }
        meshStamp[a.id] = now
        pendingMesh[a.id] = nil
        meshLock.unlock()
        pushMesh(a)
    }

    private static func takeDueMeshes() -> [MeshAnchor] {
        meshLock.lock()
        defer { meshLock.unlock() }
        let now = CACurrentMediaTime()
        let due = pendingMesh.values.filter { now - (meshStamp[$0.id] ?? 0) >= meshMinInterval }
        for a in due { pendingMesh[a.id] = nil; meshStamp[a.id] = now }
        return due
    }

    private static func flushMeshes() async {
        while true {
            try? await Task.sleep(nanoseconds: 500_000_000)
            for a in takeDueMeshes() { pushMesh(a) }
        }
    }

    private static func pushMesh(_ a: MeshAnchor) {
        let g = a.geometry
        let nv = g.vertices.count
        let nt = g.faces.count
        guard nv > 0, nt > 0, g.faces.bytesPerIndex == 4 || g.faces.bytesPerIndex == 2 else { return }
        var verts = [Float](repeating: 0, count: nv * 3)
        let vbase = g.vertices.buffer.contents().advanced(by: g.vertices.offset)
        for i in 0..<nv {
            let p = vbase.advanced(by: i * g.vertices.stride).assumingMemoryBound(to: Float.self)
            verts[3 * i] = p[0]; verts[3 * i + 1] = p[1]; verts[3 * i + 2] = p[2]
        }
        var tris = [UInt32](repeating: 0, count: nt * 3)
        let fbase = g.faces.buffer.contents()
        if g.faces.bytesPerIndex == 4 {
            let p = fbase.assumingMemoryBound(to: UInt32.self)
            for i in 0..<(nt * 3) { tris[i] = p[i] }
        } else {
            let p = fbase.assumingMemoryBound(to: UInt16.self)
            for i in 0..<(nt * 3) { tris[i] = UInt32(p[i]) }
        }
        let id = uuid(a.id)
        withM4(a.originFromAnchorTransform) { m in
            verts.withUnsafeBufferPointer { v in
                tris.withUnsafeBufferPointer { t in
                    kl_spatial_mesh(id, 0, m, v.baseAddress, UInt32(nv), t.baseAddress, UInt32(nt))
                }
            }
        }
    }

    // MARK: world anchors (WorldTrackingProvider, which already runs)

    private static func handleWorldAnchor(_ u: AnchorUpdate<WorldAnchor>) {
        let a = u.anchor
        let removed: Int32 = u.event == .removed ? 1 : 0
        withM4(a.originFromAnchorTransform) { kl_spatial_anchor(uuid(a.id), removed, $0, a.isTracked ? 1 : 0) }
    }

    /// The host side of kl_spatial: anchors the guest asks for are created and
    /// removed here. ARKit persists a WorldAnchor by itself (per app); which of
    /// them belong to the running game is anclas.json, kept by kl_spatial.c.
    private static func installHost() {
        var h = kl_spatial_host(
            create_anchor: { req, m in
                guard let m else { kl_spatial_request_done(req, 0, kl_uuid()); return }
                let mat = KleptonSpatial.matrix(m)
                Task.detached { await KleptonSpatial.createAnchor(req, mat) }
            },
            remove_anchor: { req, id in
                Task.detached { await KleptonSpatial.removeAnchor(req, id) }
            })
        kl_spatial_set_host(&h)
    }

    private static func createAnchor(_ req: UInt64, _ m: simd_float4x4) async {
        guard let tracker = KleptonCompositor.headAtTracker else {
            kl_spatial_request_done(req, 0, kl_uuid())
            return
        }
        let anchor = WorldAnchor(originFromAnchorTransform: m)
        do {
            try await tracker.addAnchor(anchor)
            // Visible to readers BEFORE the completion names it.
            withM4(anchor.originFromAnchorTransform) { kl_spatial_anchor(uuid(anchor.id), 0, $0, 1) }
            kl_spatial_request_done(req, 1, uuid(anchor.id))
        } catch {
            NSLog("[spatial] addAnchor failed: \(error)")
            kl_spatial_request_done(req, 0, kl_uuid())
        }
    }

    private static func removeAnchor(_ req: UInt64, _ id: kl_uuid) async {
        guard let tracker = KleptonCompositor.headAtTracker else {
            kl_spatial_request_done(req, 0, id)
            return
        }
        do {
            try await tracker.removeAnchor(forID: UUID(uuid: id.b))
            kl_spatial_request_done(req, 1, id)
        } catch {
            NSLog("[spatial] removeAnchor failed: \(error)")
            kl_spatial_request_done(req, 0, id)
        }
    }
}
