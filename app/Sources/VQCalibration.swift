// VisionQuestPro: the global calibration (floor + play area), the same for every game and kept across launches.
//
// The world origin of ARKit changes every session, so a floor height or a polygon in raw coordinates means nothing the next time. The
// calibration is therefore stored relative to a WorldAnchor (ARKit keeps those between launches by itself) placed on the confirmed floor:
// `vq.calib.v1` = the anchor's id, the kind (drawn / stationary), the radius and the zone's vertices in the anchor's frame (x, z; y = floor).
// At launch the anchor is found again, its current transform gives the floor height and, through kl_guardian_zone_from_anchor, the polygon
// in this session's guest space. Guest XZ is the XZ of the ARKit origin: the head and hand poses kl_ovrp publishes (which the zone was
// drawn with) are the raw originFromAnchorTransform values, without KleptonCompositor's recenterXform; only Y gets a guest origin offset
// (kl_ovrp_guest_origin_y), and the floor stays in the world's Y like every floor kl_guardian holds.
import ARKit
import Foundation
import simd

enum VQCalibration {
    private static let storeKey = "vq.calib.v1"
    private static let redoKey = "vq.calib.redo"
    private static let orphanKey = "vq.calib.orphans"     // anchor ids that could not be removed yet (the tracker only runs inside a game)

    private struct Saved: Codable {
        var version: Int
        var anchorID: UUID
        var kind: String          // "drawn" | "stationary"
        var radius: Float
        var verts: [Float]        // x, z pairs in the anchor's frame; empty when stationary
    }

    /// «Recalibrar espacio»: the guardian screen shows on the next game and Confirm replaces the calibration.
    static var redo: Bool {
        get { UserDefaults.standard.bool(forKey: redoKey) }
        set { UserDefaults.standard.set(newValue, forKey: redoKey) }
    }

    static var exists: Bool { load() != nil }

    private static func load() -> Saved? {
        guard let data = UserDefaults.standard.data(forKey: storeKey),
              let s = try? JSONDecoder().decode(Saved.self, from: data), s.version == 1 else { return nil }
        return s
    }

    /// Removes WorldAnchors by id (plus the orphans of earlier calls); those the tracker cannot remove now are kept for the next call.
    private static func removeAnchors(_ ids: [UUID]) async {
        let d = UserDefaults.standard
        var left: [String] = []
        var seen = Set<UUID>()
        for id in ids + (d.stringArray(forKey: orphanKey) ?? []).compactMap({ UUID(uuidString: $0) }) where seen.insert(id).inserted {
            if let t = KleptonCompositor.headAtTracker, t.state == .running, (try? await t.removeAnchor(forID: id)) != nil { continue }
            left.append(id.uuidString)
        }
        d.set(left, forKey: orphanKey)
    }

    /// Anchors a calibration on the confirmed floor and stores it, replacing the previous one. `zone` empty = stationary (centred on `head`).
    static func save(zone: [SIMD2<Float>], head: SIMD3<Float>, floorY: Float) async {
        guard !KLGuardian.worldTrackingDenied, let tracker = KleptonCompositor.headAtTracker else {
            VQLog.log("[calib] not saved reason=no-world-tracking")
            return
        }
        let centre = zone.isEmpty ? SIMD2<Float>(head.x, head.z) : zone.reduce(SIMD2<Float>(0, 0), +) / Float(zone.count)
        guard centre.x.isFinite, centre.y.isFinite, floorY.isFinite else {
            VQLog.log("[calib] not saved reason=not-finite")
            return
        }
        var m = matrix_identity_float4x4          // yaw 0 in this world: the anchor's frame is the world's axes at the zone's centre
        m.columns.3 = SIMD4<Float>(centre.x, floorY, centre.y, 1)
        let anchor = WorldAnchor(originFromAnchorTransform: m)
        do {
            try await tracker.addAnchor(anchor)
        } catch {
            VQLog.log("[calib] not saved reason=addAnchor failed: \(error)")
            return
        }
        let old = load()
        let local = zone.flatMap { [$0.x - centre.x, $0.y - centre.y] }
        let s = Saved(version: 1, anchorID: anchor.id, kind: zone.isEmpty ? "stationary" : "drawn",
                      radius: Float(KLG_DEFAULT_RADIUS), verts: local)
        guard let data = try? JSONEncoder().encode(s) else {
            try? await tracker.removeAnchor(forID: anchor.id)
            VQLog.log("[calib] not saved reason=encode failed")
            return
        }
        UserDefaults.standard.set(data, forKey: storeKey)
        redo = false
        await removeAnchors(old.map { [$0.anchorID] } ?? [])
        VQLog.log("[calib] saved anchor=\(anchor.id) kind=\(s.kind) verts=\(local.count / 2) floor=\(String(format: "%.3f", floorY))")
    }

    /// «Borrar calibración»: forgets it and removes its WorldAnchor.
    static func remove() async {
        // Tolerant read of the id alone: an unknown version or a changed layout must not leave the anchor orphaned for ever.
        struct IDOnly: Decodable { var anchorID: UUID }
        let id = UserDefaults.standard.data(forKey: storeKey).flatMap { try? JSONDecoder().decode(IDOnly.self, from: $0) }?.anchorID
        UserDefaults.standard.removeObject(forKey: storeKey)
        redo = false
        await removeAnchors(id.map { [$0] } ?? [])
        VQLog.log("[calib] deleted anchor=\(id?.uuidString ?? "-")")
    }

    /// What the finder task hands back to the waiting caller (the stream may not honour cancellation, so nothing is awaited on it).
    private final class Found: @unchecked Sendable {
        private let lock = NSLock()
        private var matrix: simd_float4x4?
        private var stopped = false
        var value: simd_float4x4? { lock.lock(); defer { lock.unlock() }; return matrix }
        var isStopped: Bool { lock.lock(); defer { lock.unlock() }; return stopped }
        func set(_ m: simd_float4x4) { lock.lock(); matrix = m; lock.unlock() }
        func stop() { lock.lock(); stopped = true; lock.unlock() }
    }

    /// Applies the stored calibration to this launch: true when the floor and the play area were set (the guardian screen is skipped),
    /// false (reason logged) when there is none, a redo was asked, or the anchor was not found tracked in time.
    static func applyAtLaunch() async -> Bool {
        if redo { VQLog.log("[calib] anchor not found reason=redo requested"); return false }
        guard let c = load() else { VQLog.log("[calib] anchor not found reason=no calibration saved"); return false }
        // ARKit starts in parallel with the render loop: wait (at most 6 s) for the tracker to run, then 3 s for the anchor to be tracked.
        var tracker: WorldTrackingProvider?
        for _ in 0..<120 {
            if KLGuardian.worldTrackingDenied { VQLog.log("[calib] anchor not found reason=world sensing denied"); return false }
            if let t = KleptonCompositor.headAtTracker, t.state == .running { tracker = t; break }
            try? await Task.sleep(nanoseconds: 50_000_000)
        }
        guard let tracker else { VQLog.log("[calib] anchor not found reason=tracker not running in 6 s"); return false }
        let found = Found()
        let id = c.anchorID
        Task.detached {
            for await u in tracker.anchorUpdates {
                if found.isStopped { break }
                if u.anchor.id == id, u.event != .removed, u.anchor.isTracked { found.set(u.anchor.originFromAnchorTransform); break }
            }
        }
        for _ in 0..<60 where found.value == nil { try? await Task.sleep(nanoseconds: 50_000_000) }
        found.stop()
        await removeAnchors([])                  // retry the orphans now that the tracker runs
        guard let m = found.value else { VQLog.log("[calib] anchor not found reason=not tracked in 3 s anchor=\(id)"); return false }
        let ax = m.columns.3.x, ay = m.columns.3.y, az = m.columns.3.z
        let yaw = atan2f(m.columns.2.x, m.columns.2.z)
        guard ay.isFinite, abs(ay) <= 10 else { VQLog.log("[calib] anchor not found reason=floor out of range y=\(ay)"); return false }
        let n = c.verts.count / 2
        switch c.kind {
        case "drawn":
            var out = [Float](repeating: 0, count: n * 2)
            let got = out.withUnsafeMutableBufferPointer { o in
                c.verts.withUnsafeBufferPointer { v in Int(kl_guardian_zone_from_anchor(ax, az, yaw, v.baseAddress, Int32(n), o.baseAddress)) }
            }
            let points = (0..<max(got, 0)).map { SIMD2<Float>(out[2 * $0], out[2 * $0 + 1]) }
            guard got == n, KLGuardian.setBoundary(points: points, mode: .roomscale, radius: 0) == 1 else {
                if got == n { KLGuardian.clear() }       // setBoundary ran and refused: do not leave a half state; otherwise nothing changed
                VQLog.log("[calib] anchor not found reason=zone rejected (from_anchor=\(got))")
                return false
            }
        case "stationary":
            KLGuardian.setBoundary(points: [], mode: .stationary, radius: c.radius)
        default:
            VQLog.log("[calib] anchor not found reason=unknown kind \(c.kind)")
            return false
        }
        KLGuardian.setFloor(ay, source: .manual)
        KLGuardian.dropPendingRect()         // a rectangle restored for this game must not replace the calibration at start
        VQLog.log("[calib] applied anchor=\(id) verts=\(n) floor=\(String(format: "%.3f", ay)) source=saved")
        return true
    }
}
