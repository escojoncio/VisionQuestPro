// KleptonGuardian - the Swift side of the Guardian core (kl_guardian.h): a typed wrapper over its C functions,
// the automatic floor taken from ARKit's floor planes, an observable mirror for the UI, and the per-game
// persistence (UserDefaults). The rules live in kl_guardian.c; nothing here decides a rule.
//
// Spaces (as in kl_guardian.h): floor heights are in the visionOS world (the frame of the head pose kl_ovrp
// publishes, the one the [ovrp] `head Y=` log line prints); the boundary polygon is x,z pairs in the GUEST
// tracking space.
import ARKit
import Foundation
import simd

/// Typed access to kl_guardian.h. Thread-safe like the C side (it has its own mutex).
enum KLGuardian {
    enum Mode: Int32 { case roomscale = 0, stationary = 1 }
    /// Who set the floor: 0 unset, 1 ARKit, 2 the user.
    enum FloorSource: Int32 { case unset = 0, auto = 1, manual = 2 }

    /// True when ARKit's .worldSensing was denied (set by the compositor): no floor planes and no real head pose, so a
    /// manual floor means nothing across sessions and is neither confirmed nor saved.
    nonisolated(unsafe) static var worldTrackingDenied = false

    // MARK: floor

    /// Floor from ARKit (.auto) or the user (.manual); `.unset` clears it. Returns true when it changed.
    @discardableResult
    static func setFloor(_ y: Float, source: FloorSource) -> Bool { kl_guardian_set_floor(y, source.rawValue) != 0 }

    static func floorSource() -> FloorSource {
        var y: Float = 0
        return FloorSource(rawValue: kl_guardian_get_floor(&y)) ?? .unset
    }

    /// Current floor (visionOS world, m), nil without one.
    static func floorY() -> Float? {
        var y: Float = 0
        return kl_guardian_get_floor(&y) != 0 ? y : nil
    }

    /// Remembers the current floor as the confirmed one (cancel leaves it alone).
    static func confirmFloor() { kl_guardian_floor_confirm() }

    /// The confirmed floor, nil until one was confirmed.
    static func userPreferredFloor() -> Float? {
        let y = kl_guardian_get_user_preferred_floor()
        return y.isNaN ? nil : y
    }

    /// 1 when floorY - headY lies in the believable range (see kl_guardian.h).
    static func plausible(floorY: Float, headY: Float, fromSLAM: Bool = false) -> Bool {
        kl_guardian_floor_plausible(floorY, headY, fromSLAM ? 1 : 0) != 0
    }

    /// Quest's floor setup: while `pressed` (grip of a controller, pinch of a hand) the floor is `handY` (clamped under the head),
    /// marked manual; releasing keeps it. Once per frame. Returns true while grabbing.
    @discardableResult
    static func grabFloor(pressed: Bool, handY: Float, headY: Float) -> Bool { kl_guardian_floor_grab(pressed ? 1 : 0, handY, headY) != 0 }

    static var floorGrabbing: Bool { kl_guardian_floor_grabbing() != 0 }

    // MARK: boundary

    /// Returns -1 invalid (state unchanged), 0 stored but roomscale too small (the stationary circle is used), 1 stored and ok.
    @discardableResult
    static func setBoundary(points: [SIMD2<Float>], mode: Mode, radius: Float) -> Int {
        var flat = [Float]()
        flat.reserveCapacity(points.count * 2)
        for p in points { flat.append(p.x); flat.append(p.y) }
        let n = Int32(flat.count / 2)
        return flat.withUnsafeBufferPointer { b in
            Int(kl_guardian_set_boundary(b.baseAddress, n, mode.rawValue, radius))
        }
    }

    static func setStationaryRadius(_ radius: Float) { kl_guardian_set_stationary_radius(radius) }
    static func clear() { kl_guardian_clear() }
    static var generation: UInt32 { kl_guardian_generation() }
    static var configured: Bool { kl_guardian_configured() != 0 }
    /// 0 none, 1 taken from the room, 2 set by the user (KLG_BSRC_*).
    static var boundarySource: Int32 { kl_guardian_boundary_source() }
    static var mode: Mode { Mode(rawValue: kl_guardian_get_mode()) ?? .roomscale }
    static var radius: Float { kl_guardian_get_radius() }

    /// The polygon in use (x = x, y = z), counter-clockwise; empty when there is none.
    static func boundary() -> [SIMD2<Float>] {
        let n = Int(kl_guardian_get_boundary(nil, 0))
        guard n > 0 else { return [] }
        var flat = [Float](repeating: 0, count: n * 2)
        let got = flat.withUnsafeMutableBufferPointer { b in Int(kl_guardian_get_boundary(b.baseAddress, Int32(n))) }
        guard got > 0 else { return [] }
        return (0..<min(got, n)).map { SIMD2<Float>(flat[2 * $0], flat[2 * $0 + 1]) }
    }

    /// Largest oriented rectangle inside the polygon; nil when both sides are not at least 1.15 m.
    static func playspace() -> (w: Float, d: Float, cx: Float, cz: Float, yaw: Float)? {
        var w: Float = 0, d: Float = 0, cx: Float = 0, cz: Float = 0, yaw: Float = 0
        guard kl_guardian_playspace(&w, &d, &cx, &cz, &yaw) != 0 else { return nil }
        return (w, d, cx, cz, yaw)
    }

    static var playspaceOK: Bool { kl_guardian_playspace_ok() != 0 }

    /// The head as kl_ovrp last had it published (the `head Y=` value). Never nil in practice: before the first real pose
    /// kl_ovrp answers a synthetic one (standing eye height), and no API says whether a real pose was published, so this
    /// cannot tell "no pose yet" apart (the finite check only guards against NaN).
    static func headPosition() -> SIMD3<Float>? {
        var px: Float = 0, py: Float = 0, pz: Float = 0
        var qx: Float = 0, qy: Float = 0, qz: Float = 0, qw: Float = 0
        kl_ovrp_get_head_pose(&px, &py, &pz, &qx, &qy, &qz, &qw)
        return px.isFinite && py.isFinite && pz.isFinite ? SIMD3<Float>(px, py, pz) : nil
    }
}

/// The floor from ARKit: the .floor plane nearest (XZ) to the head, the larger on a tie, median of its last 15 heights.
/// Fed by KleptonSpatial.handlePlane; never overrides a manual floor.
enum KLFloorEstimator {
    private struct FloorPlane { var x: Float; var y: Float; var z: Float; var area: Float }
    private static let lock = NSLock()
    nonisolated(unsafe) private static var planes: [UUID: FloorPlane] = [:]
    nonisolated(unsafe) private static var samples: [Float] = []
    nonisolated(unsafe) private static var lastMedian: Float?
    nonisolated(unsafe) private static var logged = 0
    private static let window = 15
    private static let maxLogs = 10

    /// The latest median, nil until a floor plane was seen.
    static var latest: Float? {
        lock.lock(); defer { lock.unlock() }
        return lastMedian
    }

    static func plane(_ u: AnchorUpdate<PlaneAnchor>) {
        let a = u.anchor
        lock.lock()
        defer { lock.unlock() }
        if u.event == .removed {
            planes[a.id] = nil
            return
        }
        guard a.classification == .floor else {
            planes[a.id] = nil        // reclassified
            return
        }
        let t = a.originFromAnchorTransform.columns.3
        planes[a.id] = FloorPlane(x: t.x, y: t.y, z: t.z, area: a.geometry.extent.width * a.geometry.extent.height)
        guard let head = KLGuardian.headPosition() else { return }
        // Nearest in XZ (0.5 m steps, so a slightly nearer sliver does not beat the big floor), then the larger area.
        var best: FloorPlane?
        var bestKey: (Int, Float) = (Int.max, 0)
        for p in planes.values {
            let d = hypotf(p.x - head.x, p.z - head.z)
            guard d.isFinite else { continue }
            let key = (Int(d / 0.5), -p.area)
            if key.0 < bestKey.0 || (key.0 == bestKey.0 && key.1 < bestKey.1) { best = p; bestKey = key }
        }
        guard let best, best.y.isFinite else { return }
        samples.append(best.y)
        if samples.count > window { samples.removeFirst(samples.count - window) }
        let sorted = samples.sorted()
        let median = sorted.count % 2 == 1 ? sorted[sorted.count / 2]
                                           : (sorted[sorted.count / 2 - 1] + sorted[sorted.count / 2]) / 2
        lastMedian = median
        guard KLGuardian.floorSource() != .manual,
              KLGuardian.plausible(floorY: median, headY: head.y) else { return }
        if KLGuardian.setFloor(median, source: .auto), logged < maxLogs {
            logged += 1
            NSLog("[guardian] floor auto y=\(String(format: "%.3f", Double(median))) head=\(String(format: "%.3f", Double(head.y))) n=\(samples.count)")
        }
    }
}

/// What the Guardian UI shows, refreshed ~10 Hz only while the guardian window is up (started by its run loop, stopped
/// when it ends and by Lifecycle.immersiveEnded). The floor estimator keeps working during the game on its own.
/// There is no callback for the head pose (the compositor publishes it into kl_ovrp every frame), so this is one
/// cancellable task, not a timer that outlives the window.
@MainActor
final class KLGuardianLive: ObservableObject {
    static let shared = KLGuardianLive()

    @Published private(set) var headY: Float = 0
    @Published private(set) var detectedFloorY: Float?
    @Published private(set) var floorSource: KLGuardian.FloorSource = .unset
    @Published private(set) var boundary: [SIMD2<Float>] = []
    @Published private(set) var playspaceOK = false

    private var task: Task<Void, Never>?
    private var lastGeneration: UInt32?

    private init() {}

    func start() {
        guard task == nil else { return }
        task = Task { @MainActor [weak self] in
            while !Task.isCancelled {
                self?.refresh()
                try? await Task.sleep(nanoseconds: 100_000_000)
            }
        }
    }

    func stop() {
        task?.cancel()
        task = nil
    }

    /// Reads the C side once; publishes only what changed.
    func refresh() {
        if let h = KLGuardian.headPosition(), h.y != headY { headY = h.y }
        let est = KLFloorEstimator.latest
        if est != detectedFloorY { detectedFloorY = est }
        let src = KLGuardian.floorSource()
        if src != floorSource { floorSource = src }
        let gen = KLGuardian.generation
        if gen != lastGeneration {
            lastGeneration = gen
            boundary = KLGuardian.boundary()
            playspaceOK = KLGuardian.playspaceOK
        }
    }
}

/// Per-game persistence (UserDefaults), keyed by VQGame.aaKey like the other per-game settings:
/// vq.guardian.<k>.{floor, floorSource, enabled, kind, mode, radius, w, d}. Boundary polygons are NOT stored: they are in
/// the absolute coordinates of one session, so only the choice and its sizes are kept and the shape is rebuilt around the
/// current head. A key `poly` written by older builds is ignored.
extension KLGuardian {
    private static func key(_ game: VQGame, _ field: String) -> String { "vq.guardian.\(game.aaKey).\(field)" }

    /// What the user confirmed as the play area.
    enum SavedArea {
        case room
        case rectangle(w: Float, d: Float)
        case stationary(radius: Float)
    }

    /// A restored rectangle waiting to be centred on the head (see `recenterRestored`).
    nonisolated(unsafe) private static var pendingRect: (w: Float, d: Float)?

    /// Rectangle `w` x `d` (m) centred on `head` (x, z), counter-clockwise.
    static func rectangle(w: Float, d: Float, around head: SIMD3<Float>?) -> [SIMD2<Float>] {
        let hx = head?.x ?? 0, hz = head?.z ?? 0
        let a = w / 2, b = d / 2
        return [SIMD2(hx - a, hz - b), SIMD2(hx + a, hz - b), SIMD2(hx + a, hz + b), SIMD2(hx - a, hz + b)]
    }

    /// Stores the user's floor and play area for `game`. An automatic floor is not stored (ARKit finds it again), nor is
    /// any floor without world tracking; the room outline is not stored either, only the choice "Sala".
    static func save(game: VQGame, area: SavedArea) {
        let d = UserDefaults.standard
        if let y = floorY(), floorSource() == .manual, !worldTrackingDenied {
            d.set(y, forKey: key(game, "floor"))
            d.set(Int(FloorSource.manual.rawValue), forKey: key(game, "floorSource"))
        } else {
            d.removeObject(forKey: key(game, "floor"))
            d.removeObject(forKey: key(game, "floorSource"))
        }
        for f in ["mode", "radius", "w", "d", "poly"] { d.removeObject(forKey: key(game, f)) }
        pendingRect = nil
        d.set(true, forKey: key(game, "enabled"))
        switch area {
        case .room:
            d.set("room", forKey: key(game, "kind"))
        case .rectangle(let w, let dd):
            d.set("rect", forKey: key(game, "kind"))
            d.set(Int(Mode.roomscale.rawValue), forKey: key(game, "mode"))
            d.set(w, forKey: key(game, "w"))
            d.set(dd, forKey: key(game, "d"))
        case .stationary(let r):
            d.set("stationary", forKey: key(game, "kind"))
            d.set(Int(Mode.stationary.rawValue), forKey: key(game, "mode"))
            d.set(r, forKey: key(game, "radius"))
        }
    }

    /// The kind of play area `save` stored for `game` ("room" / "rect" / "stationary"); nil when none. Older builds stored no
    /// `kind`: a stationary mode is still a circle, anything else is read as the room (same rule as `restore`).
    static func savedKind(game: VQGame) -> String? {
        let d = UserDefaults.standard
        guard d.object(forKey: key(game, "enabled")) as? Bool == true else { return nil }
        let mode = (d.object(forKey: key(game, "mode")) as? Int).flatMap { Mode(rawValue: Int32($0)) } ?? .roomscale
        return d.string(forKey: key(game, "kind")) ?? (mode == .stationary ? "stationary" : "room")
    }

    /// True when `save` stored a floor or a play area for `game`.
    static func hasSaved(game: VQGame) -> Bool {
        let d = UserDefaults.standard
        return d.object(forKey: key(game, "floor")) != nil || d.object(forKey: key(game, "enabled")) as? Bool == true
    }

    /// Re-applies what `save` stored. Call before the game starts. A saved "Sala" sets nothing: the room outline of the
    /// current session stays in charge (it is not marked as set by the user).
    static func restore(game: VQGame) {
        let d = UserDefaults.standard
        pendingRect = nil
        if !worldTrackingDenied,
           let y = d.object(forKey: key(game, "floor")) as? Float,
           let s = d.object(forKey: key(game, "floorSource")) as? Int,
           let src = FloorSource(rawValue: Int32(s)), src != .unset {
            setFloor(y, source: src)
        }
        guard d.object(forKey: key(game, "enabled")) as? Bool == true else { return }
        // Older builds stored no `kind`: a stationary mode is still a circle, anything else is read as the room.
        let mode = (d.object(forKey: key(game, "mode")) as? Int).flatMap { Mode(rawValue: Int32($0)) } ?? .roomscale
        let kind = d.string(forKey: key(game, "kind")) ?? (mode == .stationary ? "stationary" : "room")
        switch kind {
        case "stationary":
            let r = setBoundary(points: [], mode: .stationary, radius: d.object(forKey: key(game, "radius")) as? Float ?? 0)
            NSLog("[guardian] restored for \(game.aaKey): stationary -> \(r)")
        case "rect":
            guard let w = d.object(forKey: key(game, "w")) as? Float, let dd = d.object(forKey: key(game, "d")) as? Float else { return }
            pendingRect = (w, dd)
            let r = setBoundary(points: rectangle(w: w, d: dd, around: headPosition()), mode: .roomscale, radius: 0)
            NSLog("[guardian] restored for \(game.aaKey): rectangle \(w) x \(dd) around the head (if known) -> \(r)")
        default:
            NSLog("[guardian] restored for \(game.aaKey): room (the session's own outline)")
        }
    }

    /// Centres a restored rectangle on the head as it is now (call right before the game starts, when the head pose exists).
    static func recenterRestored() {
        guard let r = pendingRect, let h = headPosition() else { return }
        pendingRect = nil
        let res = setBoundary(points: rectangle(w: r.w, d: r.d, around: h), mode: .roomscale, radius: 0)
        NSLog("[guardian] restored rectangle re-centred on the head -> \(res)")
    }
}
