// VisionQuestPro: whether the game's shader cache is complete. A light background poll of the engine
// (vq_cache_state -> vpvk_cache_state) while the guest runs: the last state is kept for the compositor's
// HUD, and QUEST/Cache/<id>/cache-state.json is rewritten on every change so the launcher card can show
// it later (the process may die before the session ends). Global: no per-game or per-engine condition.

import Foundation

enum VQCacheState {
    private static let queue = DispatchQueue(label: "vq.cachestate", qos: .utility)
    private static let lock = NSLock()
    nonisolated(unsafe) private static var latest: vq_cache_state?   // under `lock`: written by the poll, read by the HUD
    nonisolated(unsafe) private static var latestComplete = false    // under `lock`: vq_cache_complete(latest)
    nonisolated(unsafe) private static var latestNew: UInt32 = 0     // under `lock`: recipes added in this session
    // Touched only from `queue`.
    nonisolated(unsafe) private static var timer: DispatchSourceTimer?
    nonisolated(unsafe) private static var game: String?
    nonisolated(unsafe) private static var written: (complete: Bool, reason: UInt32, new: UInt32)?
    nonisolated(unsafe) private static var baseRecipes: UInt32?    // recipes stored at the first poll with a device (earlier runs)
    nonisolated(unsafe) private static var sawRecent = false   // a reason 2 (new keys) was seen in this session
    nonisolated(unsafe) private static var savedComplete = false   // complete stored by an earlier session (read at start)

    /// Call when the guest starts (once per session); polls every 2 s until `stop()`.
    static func start(game id: String) {
        queue.async {
            guard timer == nil else { return }
            game = id
            written = nil
            baseRecipes = nil
            sawRecent = false
            savedComplete = info(game: id)?.complete ?? false
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
            lock.lock(); latest = nil; latestComplete = false; latestNew = 0; lock.unlock()
        }
    }

    private static func poll(final: Bool) {
        var s = vq_cache_state()
        guard vq_cache_state_get(&s) == 1 else { return }   // no device (yet / any more): keep the last state
        // «New recipes this session» = recipes added since the first poll. It is NOT a count of real compilations:
        // the engine's struct has no compiled_in_session (that counter is only in its vk.pso.census log line).
        // Unverified: the baseline assumes the stored recipes are already counted when the device exists.
        if baseRecipes == nil { baseRecipes = s.recipes }
        let newRecipes = s.recipes > baseRecipes! ? s.recipes - baseRecipes! : 0
        let complete = vq_cache_complete(&s) == 1
        lock.lock(); latest = s; latestComplete = complete; latestNew = newRecipes; lock.unlock()
        if s.reason == 2 { sawRecent = true }
        // Start-up values (no pipelines / no frames / still compiling before any new key) must not overwrite
        // the last valid state on disk (e.g. a saved complete=true).
        if s.reason == 1 || s.reason == 3 || (s.complete == 0 && s.reason == 4 && !sawRecent) { return }
        let changed = written.map { $0.complete != complete || $0.reason != s.reason || $0.new != newRecipes } ?? true
        guard changed || final, let id = game else { return }
        // The file keeps an earlier complete=true while this session added no recipes (a short session that only
        // serves keys from the cache); the HUD and the log above/below show the real state.
        let stored = vq_cache_persisted_complete(savedComplete ? 1 : 0, complete ? 1 : 0, newRecipes) == 1
        write(s, complete: stored, new: newRecipes, game: id)
        if changed {
            VQLog.log("Caché de shaders: completa=\(complete) motivo=\(s.reason) pedidos=\(s.pipelines_unique) "
                      + "recetas_nuevas_en_sesión=\(newRecipes) recetas=\(s.recipes) "
                      + "módulos=\(s.modules_used)/\(s.modules_created) inactivo=\(s.idle_ms) ms")
        }
        written = (complete, s.reason, newRecipes)
    }

    private static func write(_ s: vq_cache_state, complete: Bool, new: UInt32, game id: String) {
        guard let dir = VQFolder.shared.cache(for: id) else { return }
        let json: [String: Any] = [
            "complete": complete, "reason": Int(s.reason),
            "pipelines_unique": Int(s.pipelines_unique), "recipes": Int(s.recipes), "new_recipes": Int(new),
            "modules_created": Int(s.modules_created), "modules_used": Int(s.modules_used),
            "date": ISO8601DateFormatter().string(from: Date())]
        guard let data = try? JSONSerialization.data(withJSONObject: json, options: [.sortedKeys, .prettyPrinted]) else { return }
        try? data.write(to: dir.appendingPathComponent("cache-state.json"), options: .atomic)
    }

    /// The compositor's second HUD line (nil: nothing to say). Cheap and non-blocking: copies the last polled state.
    /// `pipelines_unique` = pipelines the game requested (served from the cache or compiled); `new` = new recipes this session (not real compilations).
    static var hudText: String? {
        lock.lock(); let s = latest; let complete = latestComplete; let new = latestNew; lock.unlock()
        guard let s else { return nil }
        if complete {
            if new == 0 {
                return VQL("Caché de shaders estable (sin pipelines nuevos en esta sesión): \(s.pipelines_unique) pedidos, 0 recetas nuevas en la última sesión",
                           "Shader cache stable (no new pipelines this session): \(s.pipelines_unique) requested, 0 new recipes in the last session")
            }
            return VQL("Caché de shaders estable (sin pipelines nuevos en esta sesión): \(new) recetas nuevas en la última sesión; sal y vuelve a entrar",
                       "Shader cache stable (no new pipelines this session): \(new) new recipes in the last session; quit and re-enter")
        }
        if s.reason == 2 && s.idle_ms < 20_000 {
            if new == 0 {
                return VQL("Caché de shaders: cargando \(s.pipelines_unique) pedidos de la caché",
                           "Shader cache: loading \(s.pipelines_unique) requested from the cache")
            }
            return VQL("Caché de shaders: \(new) recetas nuevas en esta sesión, \(s.pipelines_unique) pedidos",
                       "Shader cache: \(new) new recipes this session, \(s.pipelines_unique) requested")
        }
        return nil
    }

    struct Info: Equatable { let complete: Bool; let requested: Int; let new: Int? }

    /// Last state written by an earlier session for the launcher card (nil: no file). Does file I/O: call off the main thread.
    /// `new` is nil for files written before the field existed.
    static func info(game id: String) -> Info? {
        guard let dir = VQFolder.shared.cache(for: id),
              let data = try? Data(contentsOf: dir.appendingPathComponent("cache-state.json")),
              let obj = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
              let complete = obj["complete"] as? Bool else { return nil }
        return Info(complete: complete, requested: obj["pipelines_unique"] as? Int ?? 0, new: obj["new_recipes"] as? Int)
    }
}
