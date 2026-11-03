// VisionQuestPro: whether the game's shader cache is complete. A light background poll of the engine
// (vq_cache_state -> vpvk_cache_state) while the guest runs: the last state is kept for the compositor's
// HUD, and QUEST/Cache/<id>/cache-state.json is rewritten on every change so the launcher card can show
// it later (the process may die before the session ends). Global: no per-game or per-engine condition.

import Foundation

enum VQCacheState {
    private static let queue = DispatchQueue(label: "vq.cachestate", qos: .utility)
    private static let lock = NSLock()
    nonisolated(unsafe) private static var latest: vq_cache_state?   // under `lock`: written by the poll, read by the HUD
    // Touched only from `queue`.
    nonisolated(unsafe) private static var timer: DispatchSourceTimer?
    nonisolated(unsafe) private static var game: String?
    nonisolated(unsafe) private static var written: (complete: UInt32, reason: UInt32)?
    nonisolated(unsafe) private static var sawRecent = false   // a reason 2 (new keys) was seen in this session

    /// Call when the guest starts (once per session); polls every 2 s until `stop()`.
    static func start(game id: String) {
        queue.async {
            guard timer == nil else { return }
            game = id
            written = nil
            sawRecent = false
            let t = DispatchSource.makeTimerSource(queue: queue)
            t.schedule(deadline: .now() + 2, repeating: 2)
            t.setEventHandler { poll(final: false) }
            timer = t
            t.resume()
        }
    }

    /// Call when the session ends: stops the poll and writes the last state once more.
    static func stop() {
        queue.async {
            guard let t = timer else { return }
            t.cancel()
            timer = nil
            poll(final: true)
            game = nil
            lock.lock(); latest = nil; lock.unlock()
        }
    }

    private static func poll(final: Bool) {
        var s = vq_cache_state()
        guard vq_cache_state_get(&s) == 1 else { return }   // no device (yet / any more): keep the last state
        lock.lock(); latest = s; lock.unlock()
        if s.reason == 2 { sawRecent = true }
        // Start-up values (no pipelines / no frames / still compiling before any new key) must not overwrite
        // the last valid state on disk (e.g. a saved complete=true).
        if s.reason == 1 || s.reason == 3 || (s.complete == 0 && s.reason == 4 && !sawRecent) { return }
        let changed = written.map { $0.complete != s.complete || $0.reason != s.reason } ?? true
        guard changed || final, let id = game else { return }
        write(s, game: id)
        if changed {
            VQLog.log("Caché de shaders: completa=\(s.complete) motivo=\(s.reason) pipelines=\(s.pipelines_unique) "
                      + "recetas=\(s.recipes) módulos=\(s.modules_used)/\(s.modules_created) inactivo=\(s.idle_ms) ms")
        }
        written = (s.complete, s.reason)
    }

    private static func write(_ s: vq_cache_state, game id: String) {
        guard let dir = VQFolder.shared.cache(for: id) else { return }
        let json: [String: Any] = [
            "complete": s.complete == 1, "reason": Int(s.reason),
            "pipelines_unique": Int(s.pipelines_unique), "recipes": Int(s.recipes),
            "modules_created": Int(s.modules_created), "modules_used": Int(s.modules_used),
            "date": ISO8601DateFormatter().string(from: Date())]
        guard let data = try? JSONSerialization.data(withJSONObject: json, options: [.sortedKeys, .prettyPrinted]) else { return }
        try? data.write(to: dir.appendingPathComponent("cache-state.json"), options: .atomic)
    }

    /// The compositor's second HUD line (nil: nothing to say). Cheap and non-blocking: copies the last polled state.
    static var hudText: String? {
        lock.lock(); let s = latest; lock.unlock()
        guard let s else { return nil }
        if s.complete == 1 {
            return VQL("Caché de shaders completa: sal y vuelve a entrar", "Shader cache complete: quit and re-enter")
        }
        if s.reason == 2 && s.idle_ms < 20_000 {
            return VQL("Caché de shaders: compilando… \(s.pipelines_unique) pipelines",
                       "Shader cache: compiling… \(s.pipelines_unique) pipelines")
        }
        return nil
    }

    /// Last state written by an earlier session for the launcher card (nil: no file). Does file I/O: call off the main thread.
    static func isComplete(game id: String) -> Bool? {
        guard let dir = VQFolder.shared.cache(for: id),
              let data = try? Data(contentsOf: dir.appendingPathComponent("cache-state.json")),
              let obj = try? JSONSerialization.jsonObject(with: data) as? [String: Any] else { return nil }
        return obj["complete"] as? Bool
    }
}
