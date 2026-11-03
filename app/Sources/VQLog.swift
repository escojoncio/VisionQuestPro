// VisionQuestPro: the app's own log. Every diagnostic goes here (and to the system log): there is
// no Mac to attach. Documents/Registros/app.txt, shared from the app; the guest's own log is
// Documents/klepton-boot.log (kl_app.c).

import Foundation
import Metal
#if canImport(MetricKit)
import MetricKit
#endif

enum VQLog {
    private static let lock = NSLock()
    private static var handle: FileHandle?

    static var directory: URL {
        FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("Registros", isDirectory: true)
    }
    static var appLog: URL { directory.appendingPathComponent("app.txt") }
    static var bootLog: URL {
        FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0].appendingPathComponent("klepton-boot.log")
    }
    static var crashLog: URL {
        FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0].appendingPathComponent("klepton-crash.log")
    }

    /// Starts a new log for this session (the previous one is kept as app-anterior.txt).
    static func start() {
        lock.lock(); defer { lock.unlock() }
        try? FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        sessionStart = Date()
        // BEFORE the rotation below: a launch that plays no game would otherwise push the last
        // game's app.txt out of app-anterior.txt (2026-10-20: AC's log vanished after two launches).
        let archived = archivePreviousSession()
        for old in gameLogs { try? FileManager.default.removeItem(at: old) }  // staged game logs are archived above; never carried into a new session
        refreshSessionFiles()
        let previous = directory.appendingPathComponent("app-anterior.txt")
        try? FileManager.default.removeItem(at: previous)
        try? FileManager.default.moveItem(at: appLog, to: previous)
        for old in captures { try? FileManager.default.removeItem(at: old) }  // previous session's PNGs
        FileManager.default.createFile(atPath: appLog.path, contents: nil)
        handle = try? FileHandle(forWritingTo: appLog)
        let info = Bundle.main.infoDictionary ?? [:]
        let line = "VisionQuestPro \(info["CFBundleShortVersionString"] ?? "?") (\(info["CFBundleVersion"] ?? "?")) commit \(vqBuildCommit.isEmpty ? "local" : vqBuildCommit), "
            + "visionOS \(ProcessInfo.processInfo.operatingSystemVersionString)\n"
        handle?.write(Data(line.utf8))
        if let archived = archived {
            let stamp = ISO8601DateFormatter.string(from: Date(), timeZone: .current, formatOptions: [.withTime, .withColonSeparatorInTime])
            handle?.write(Data("\(stamp) Registros: sesión anterior archivada en Registros/sesiones como \(archived)\n".utf8))
        }
        // Read by kl_fault_set_crash_path (C) for the header of klepton-crash.log.
        setenv("KL_BUILD_COMMIT", vqBuildCommit.isEmpty ? "local" : vqBuildCommit, 1)
        refreshQuestAttachments(force: true)   // warm the cache off the main thread
    }

    static func log(_ message: String) {
        NSLog("[vq] %@", message)
        let stamp = ISO8601DateFormatter.string(from: Date(), timeZone: .current, formatOptions: [.withTime, .withColonSeparatorInTime])
        lock.lock(); defer { lock.unlock() }
        handle?.write(Data("\(stamp) \(message)\n".utf8))
    }

    /// The files of the CURRENT process (and the previous app log), whole: what saveToQuest copies.
    private static var liveFiles: [URL] {
        [appLog, directory.appendingPathComponent("app-anterior.txt"), bootLog, crashLog,
         directory.appendingPathComponent("conformidad-vulkan.txt"),
         directory.appendingPathComponent("conformidad-detalle.txt"),
         directory.appendingPathComponent("meta-api.txt")]
            .filter { FileManager.default.fileExists(atPath: $0.path) }
            + gameLogs + captures + crashAttachments
    }

    /// The logs for the share button: the live files plus the last `shareSessions` games, each with
    /// its own app log, guest log and crash report under a name that says date, game and commit
    /// (Registros/sesiones/<fecha>_<juego>_<commit>__klepton-boot.log). Read by SwiftUI on every
    /// render: everything here is a stat or a cached list, never a directory scan on the spot.
    /// A live boot / crash log that is the previous game's is left out when its archived copy is in
    /// the list (no duplicated multi-MB logs).
    static var shareable: [URL] {
        sessionsLock.lock(); let sessions = sessionFilesCache; sessionsLock.unlock()
        let names = sessions.map { $0.lastPathComponent }
        let live = liveFiles.filter { (u: URL) -> Bool in
            guard u == bootLog || u == crashLog else { return true }
            let suffix = "__" + u.lastPathComponent
            guard names.contains(where: { $0.hasSuffix(suffix) }) else { return true }
            let m = (try? FileManager.default.attributesOfItem(atPath: u.path))?[.modificationDate] as? Date
            return m.map { $0 >= sessionStart } ?? true
        }
        return live + sessions + (VQSettings.shared.includeCaptures ? captureFiles : [])
    }

    // MARK: archived sessions

    /// One game = one process (the guest cannot run twice in one), so a session is what one
    /// process wrote: app.txt, plus the boot / crash log if the game started in it.
    private static let sessionsLock = NSLock()
    nonisolated(unsafe) private static var sessionFilesCache: [URL] = []
    nonisolated(unsafe) private static var sessionStart = Date.distantPast
    nonisolated(unsafe) private static var previousSessionTag: String?
    private static let keepSessions = 8
    private static let shareSessions = 6
    private static let headBytes = 2_000_000
    private static let tailBytes = 4_000_000

    static var sessionsDirectory: URL { directory.appendingPathComponent("sesiones", isDirectory: true) }

    private static func sessionName(_ u: URL) -> String {
        u.lastPathComponent.components(separatedBy: "__")[0]
    }

    /// Copies `src` to `dst`. A file over headBytes + tailBytes keeps its first headBytes and its last
    /// tailBytes (cut at line ends) with a marker in between; the whole file stays where it was
    /// and in QUEST/Registros (saveToQuest).
    private static func copyTrimmed(_ src: URL, to dst: URL) -> Bool {
        let fm = FileManager.default
        try? fm.removeItem(at: dst)
        let size = (try? fm.attributesOfItem(atPath: src.path))?[.size] as? UInt64 ?? 0
        if size <= UInt64(headBytes + tailBytes) { return (try? fm.copyItem(at: src, to: dst)) != nil }
        guard let h = try? FileHandle(forReadingFrom: src) else { return false }
        defer { try? h.close() }
        var head = h.readData(ofLength: headBytes)
        if let nl = head.lastIndex(of: 10) { head = Data(head[head.startIndex...nl]) }
        try? h.seek(toOffset: size - UInt64(tailBytes))
        var tail = h.readDataToEndOfFile()
        if let nl = tail.firstIndex(of: 10) { tail = Data(tail[tail.index(after: nl)...]) }
        let marker = Data("\n[... recortado: \(size) bytes en total, aquí los primeros y los últimos; el registro completo está en QUEST/Registros ...]\n".utf8)
        do { try (head + marker + tail).write(to: dst); return true } catch { return false }
    }

    /// At launch, BEFORE app.txt is rotated: if the previous process started a game, files its app
    /// log (and the guest / crash log written after that process began) in Registros/sesiones/.
    /// Cheap and synchronous on purpose (local sandbox, APFS clone or two small reads): the next
    /// launch may be a Digital Crown press away. Returns the session name, or nil.
    private static func archivePreviousSession() -> String? {
        let fm = FileManager.default
        guard let data = try? Data(contentsOf: appLog, options: .mappedIfSafe), !data.isEmpty,
              let r = data.range(of: Data("Arranque: ".utf8)) else { return nil }
        let rest = String(decoding: data[r.upperBound..<min(r.upperBound + 200, data.endIndex)], as: UTF8.self)
        let id = rest.components(separatedBy: "\n")[0].components(separatedBy: " (")[0]
        var game = ""
        for c in id {
            if c.isASCII && (c.isLetter || c.isNumber || c == "-") { game.append(c) }
            else if !game.hasSuffix("_") { game.append("_") }
        }
        game = String(game.prefix(24))
        while game.hasSuffix("_") { game.removeLast() }
        if game.isEmpty { game = "juego" }
        let first = String(decoding: data.prefix(300), as: UTF8.self).components(separatedBy: "\n")[0]
        var commit = "local"
        if let c = first.range(of: "commit ") {
            let hash = String(first[c.upperBound...].prefix(while: { $0.isLetter || $0.isNumber }))
            if !hash.isEmpty { commit = hash }
        }
        let attrs = try? fm.attributesOfItem(atPath: appLog.path)
        let started = (attrs?[.creationDate] as? Date) ?? (attrs?[.modificationDate] as? Date) ?? Date()
        let f = DateFormatter()
        f.locale = Locale(identifier: "en_US_POSIX")
        f.dateFormat = "yyyy-MM-dd_HH-mm"
        let name = "\(f.string(from: started))_\(game)_\(commit)"
        do { try fm.createDirectory(at: sessionsDirectory, withIntermediateDirectories: true) } catch { return nil }
        _ = copyTrimmed(appLog, to: sessionsDirectory.appendingPathComponent("\(name)__app.txt"))
        for src in [bootLog, crashLog] + gameLogs {
            // Written after this process began (the guest log is reopened "w" at each game launch);
            // an older one belongs to an earlier session.
            guard let m = (try? fm.attributesOfItem(atPath: src.path))?[.modificationDate] as? Date,
                  m >= started.addingTimeInterval(-5) else { continue }
            if copyTrimmed(src, to: sessionsDirectory.appendingPathComponent("\(name)__\(src.lastPathComponent)")),
               src == bootLog { previousSessionTag = "\(game)_\(commit)" }
        }
        // The staged game-*.log above are only as fresh as the last copy; if the process was killed
        // or crashed, the game's own logs (folder stored at launch) are the complete ones. Off the
        // main thread: it resolves the QUEST folder and reads up to 8 MB per log.
        if let path = UserDefaults.standard.string(forKey: gameFilesKey) {
            DispatchQueue.global(qos: .utility).async { archiveGameLogs(filesPath: path, as: name, since: started) }
        }
        let files = (try? fm.contentsOfDirectory(at: sessionsDirectory, includingPropertiesForKeys: nil)) ?? []
        let keep = Set(Set(files.map { sessionName($0) }).sorted(by: >).prefix(keepSessions))
        for u in files where !keep.contains(sessionName(u)) { try? fm.removeItem(at: u) }
        return name
    }

    /// Rebuilds the cached list of the last `shareSessions` archived sessions.
    private static func refreshSessionFiles() {
        let files = ((try? FileManager.default.contentsOfDirectory(at: sessionsDirectory, includingPropertiesForKeys: nil)) ?? [])
            .filter { $0.lastPathComponent.contains("__") }
        let newest = Set(Set(files.map { sessionName($0) }).sorted(by: >).prefix(shareSessions))
        let picked = files.filter { newest.contains(sessionName($0)) }
            .sorted { $0.lastPathComponent > $1.lastPathComponent }
        sessionsLock.lock()
        sessionFilesCache = picked
        sessionsLock.unlock()
    }

    /// Crash / hang evidence the app can read: the MSL of the shaders of a GPU hang
    /// (gpu-hang-*.metal, VPKEngine writes them to KL_CAPTURE_DIR = QUEST/Registros, else to this
    /// app's Registros), the MetricKit payloads saved by VQMetrics (metrickit-*.json) and any .ips
    /// report found there (the system keeps its own crash logs out of the sandbox, so only a copy
    /// someone put in the folder shows up). Same names in both folders: the app's own wins.
    /// `shareable` is read by SwiftUI on every render of the views that hold a ShareLink, so this
    /// must be cheap and must never block the main thread: the app's own folder (local sandbox) is
    /// listed on the spot; QUEST/Registros (File Provider, and `VQFolder.url` may resolve a bookmark /
    /// keychain) is listed on a background queue at most every 30 s and read from the cache here.
    private static let attachLock = NSLock()
    private static var questAttachments: [URL] = []
    private static var questAttachmentsAt = Date.distantPast
    private static var questAttachmentsBusy = false
    /// QUEST/capturas/<game>/*.vpcap (the «Captura de frames» files), listed with the attachments.
    private static var questCaptures: [URL] = []
    private static var capturesLogged = ""

    static var captureFiles: [URL] {
        refreshQuestAttachments()
        attachLock.lock(); defer { attachLock.unlock() }
        return questCaptures
    }

    private static func listCaptures(in root: URL) -> [URL] {
        let fm = FileManager.default
        let base = root.appendingPathComponent(VQFolder.capturesName, isDirectory: true)
        return ((try? fm.contentsOfDirectory(at: base, includingPropertiesForKeys: nil)) ?? [])
            .sorted { $0.lastPathComponent < $1.lastPathComponent }
            .flatMap { dir in ((try? fm.contentsOfDirectory(at: dir, includingPropertiesForKeys: nil)) ?? [])
                .filter { $0.pathExtension == "vpcap" }
                .sorted { $0.lastPathComponent < $1.lastPathComponent } }
    }

    /// One log line per capture (name and size) whenever the list or «Incluir capturas» changes.
    private static func logCaptures(_ files: [URL]) {
        let include = VQSettings.shared.includeCaptures
        let sizes = files.map { (try? FileManager.default.attributesOfItem(atPath: $0.path))?[.size] as? UInt64 ?? 0 }
        let lines = zip(files, sizes).map { "\($0.deletingLastPathComponent().lastPathComponent)/\($0.lastPathComponent) "
            + String(format: "%.1f MB", Double($1) / 1_048_576) }
        let signature = files.isEmpty ? "" : "\(include)|" + lines.joined(separator: "|")
        attachLock.lock()
        let changed = signature != capturesLogged
        capturesLogged = signature
        attachLock.unlock()
        guard changed else { return }
        log("Capturas: \(files.count) en QUEST/capturas, \(String(format: "%.1f MB", Double(sizes.reduce(0, +)) / 1_048_576)) en total; "
            + (include ? "se incluyen al compartir" : "no se incluyen al compartir («Incluir capturas» apagado)"))
        for l in lines { log("Capturas: \(l)") }
    }

    private static func isCrashAttachment(_ u: URL) -> Bool {
        let n = u.lastPathComponent
        return (n.hasPrefix("gpu-hang-") && u.pathExtension == "metal")
            || (n.hasPrefix("metrickit-") && u.pathExtension == "json")
            || u.pathExtension == "ips"
    }

    private static func attachments(in dir: URL) -> [URL] {
        ((try? FileManager.default.contentsOfDirectory(at: dir, includingPropertiesForKeys: nil)) ?? [])
            .filter { isCrashAttachment($0) }
            .sorted { $0.lastPathComponent < $1.lastPathComponent }
    }

    /// Re-lists QUEST/Registros in the background (no-op while one is running or the cache is fresh).
    static func refreshQuestAttachments(force: Bool = false) {
        attachLock.lock()
        if questAttachmentsBusy || (!force && Date().timeIntervalSince(questAttachmentsAt) < 30) {
            attachLock.unlock(); return
        }
        questAttachmentsBusy = true
        attachLock.unlock()
        DispatchQueue.global(qos: .utility).async {
            var found: [URL] = []
            var captures: [URL] = []
            if let quest = VQFolder.shared.url {
                found = attachments(in: quest.appendingPathComponent("Registros", isDirectory: true))
                captures = listCaptures(in: quest)
            }
            logCaptures(captures)
            stageGameLogs()
            attachLock.lock()
            questAttachments = found
            questCaptures = captures
            questAttachmentsAt = Date()
            questAttachmentsBusy = false
            attachLock.unlock()
        }
    }

    static var crashAttachments: [URL] {
        refreshQuestAttachments()
        attachLock.lock(); let quest = questAttachments; attachLock.unlock()
        var seen = Set<String>()
        var out: [URL] = []
        for u in attachments(in: directory) + quest {
            if seen.insert(u.lastPathComponent).inserted { out.append(u) }
        }
        return out
    }

    // MARK: The game's own logs

    /// A game writes its own logs under its files folder (<files> = QUEST/Partidas/<game>, the dir
    /// kl_app_configure_game gets; the guest sees it as Documents/.f when the path is long): Unreal's
    /// Saved/Logs/<Project>.log, Unity's Player.log... and `-stdout` does not carry them to
    /// klepton-boot.log. Set by VQLaunch.configure for every game; no engine is special-cased.
    nonisolated(unsafe) private static var gameFiles: URL?
    nonisolated(unsafe) private static var gameLogStamps: [String: String] = [:]
    private static let gameLogLock = NSLock()
    private static let gameLogBytes: UInt64 = 8 * 1_048_576
    private static let gameLogMax = 5
    private static let gameLogDepth = 6
    private static let gameLogVisits = 20_000
    private static let gameLogSkip: Set<String> = ["Cache", "tree", "obb", "assets", "Engine", "Content"]
    private static let gameFilesKey = "vq.gameFilesPath"
    private typealias GameLog = (url: URL, date: Date, size: UInt64)

    /// The copies in Registros/ (game-<name>.log).
    static var gameLogs: [URL] {
        ((try? FileManager.default.contentsOfDirectory(at: directory, includingPropertiesForKeys: nil)) ?? [])
            .filter { $0.lastPathComponent.hasPrefix("game-") && $0.pathExtension == "log" }
            .sorted { $0.lastPathComponent < $1.lastPathComponent }
    }

    /// The path is also stored (UserDefaults): if the process dies, the next launch archives that
    /// game's logs (archivePreviousSession). A different game drops the staged copies, so Registros
    /// never carries another game's logs.
    static func setGameFiles(_ files: URL) {
        gameLogLock.lock(); defer { gameLogLock.unlock() }
        if files.path != gameFiles?.path {
            gameLogStamps = [:]
            for old in gameLogs { try? FileManager.default.removeItem(at: old) }
        }
        gameFiles = files
        UserDefaults.standard.set(files.path, forKey: gameFilesKey)
    }

    /// Copies this session's game logs to Registros/game-*.log (the last gameLogBytes of each when
    /// bigger), so they travel with the other logs (ShareLink, saveToQuest, archived sessions). Reads
    /// and writes files: never call it from the main thread. Does nothing without a game or a log.
    static func stageGameLogs() {
        gameLogLock.lock()
        let notes = stageGameLogsLocked()
        gameLogLock.unlock()
        for note in notes { log(note) }
    }

    /// File name in Registros for a game log: its last four path components, sanitized (no "__",
    /// which separates the session name in sesiones/).
    private static func gameLogName(_ u: URL) -> String {
        var out = ""
        for c in u.deletingPathExtension().pathComponents.suffix(4).joined(separator: "_") {
            if c.isASCII && (c.isLetter || c.isNumber || c == "-" || c == ".") { out.append(c) }
            else if !out.hasSuffix("_") { out.append("_") }
        }
        return "game-" + String(out.suffix(100)) + ".log"
    }

    /// The newest gameLogMax `*.log` (not -backup- copies) under `files`, at most gameLogDepth deep, written
    /// at or after `since`. Skips folders that are big or hold no logs (gameLogSkip, Saved/Config, Saved/Crashes).
    private static func newestGameLogs(in files: URL, since: Date) -> [GameLog] {
        let keys: [URLResourceKey] = [.isDirectoryKey, .contentModificationDateKey, .fileSizeKey]
        guard let e = FileManager.default.enumerator(at: files, includingPropertiesForKeys: keys,
                                                     options: [.skipsHiddenFiles]) else { return [] }
        var found: [GameLog] = []
        var visited = 0
        while let u = e.nextObject() as? URL {
            visited += 1
            if visited > gameLogVisits { break }
            let v = try? u.resourceValues(forKeys: Set(keys))
            let name = u.lastPathComponent
            if v?.isDirectory == true {
                if e.level >= gameLogDepth || gameLogSkip.contains(name)
                    || ((name == "Config" || name == "Crashes") && u.deletingLastPathComponent().lastPathComponent == "Saved") {
                    e.skipDescendants()
                }
                continue
            }
            guard u.pathExtension.lowercased() == "log", !name.contains("-backup-"),
                  let m = v?.contentModificationDate, m >= since else { continue }
            found.append((url: u, date: m, size: UInt64(v?.fileSize ?? 0)))
        }
        return Array(found.sorted { $0.date > $1.date }.prefix(gameLogMax))
    }

    /// The log's content, or its last gameLogBytes (cut at a line end, with a marker) when it is bigger.
    private static func gameLogData(_ src: GameLog) -> Data? {
        guard let h = try? FileHandle(forReadingFrom: src.url) else { return nil }
        defer { try? h.close() }
        let size = (try? h.seekToEnd()) ?? 0
        guard size > gameLogBytes else {
            try? h.seek(toOffset: 0)
            return h.readDataToEndOfFile()
        }
        try? h.seek(toOffset: size - gameLogBytes)
        var data = h.readDataToEndOfFile()
        if let nl = data.firstIndex(of: 10) { data = Data(data[data.index(after: nl)...]) }
        return Data("[... recortado: \(size) bytes en total, aquí los últimos \(gameLogBytes) ...]\n".utf8) + data
    }

    private static func stageGameLogsLocked() -> [String] {
        guard let files = gameFiles else { return [] }
        let fm = FileManager.default
        var notes: [String] = []
        var keep = Set<String>()
        for src in newestGameLogs(in: files, since: sessionStart) {
            let name = gameLogName(src.url)
            let dst = directory.appendingPathComponent(name)
            keep.insert(name)
            let stamp = "\(src.url.path)|\(src.size)|\(src.date.timeIntervalSince1970)"
            if gameLogStamps[name] == stamp && fm.fileExists(atPath: dst.path) { continue }
            guard let data = gameLogData(src), (try? data.write(to: dst, options: .atomic)) != nil else { continue }
            if gameLogStamps[name] == nil {
                notes.append("Registros: log del juego \(src.url.lastPathComponent) (\(src.size) bytes) -> \(name) (\(data.count) bytes)")
            }
            gameLogStamps[name] = stamp
        }
        for stale in gameLogs where !keep.contains(stale.lastPathComponent) { try? fm.removeItem(at: stale) }
        return notes
    }

    /// At launch, for the session just archived as `name`: files the logs of the game that ran in the
    /// previous process (`filesPath`, stored by setGameFiles) as <name>__game-*.log. A log older than
    /// that process's start is an earlier session's and is left out. Does nothing without the QUEST
    /// folder, the path or a log.
    private static func archiveGameLogs(filesPath: String, as name: String, since started: Date) {
        guard VQFolder.shared.url != nil else { return }   // opens the folder's access before the stored path is read
        var filed: [String] = []
        for src in newestGameLogs(in: URL(fileURLWithPath: filesPath, isDirectory: true), since: started.addingTimeInterval(-5)) {
            let dst = sessionsDirectory.appendingPathComponent("\(name)__\(gameLogName(src.url))")
            guard let data = gameLogData(src), (try? data.write(to: dst, options: .atomic)) != nil else { continue }
            filed.append(dst.lastPathComponent)
        }
        guard !filed.isEmpty else { return }
        refreshSessionFiles()
        log("Registros: logs del juego de la sesión anterior archivados: \(filed.joined(separator: ", "))")
    }

    /// Copies the logs to QUEST/Registros/<date>-<label>/ (Files app): a way to get them that does
    /// not go through the system share sheet (log 60: the sheet aborted the app inside
    /// ExtensionFoundation once new connections to system services were failing). Keeps the last 12.
    @discardableResult
    static func saveToQuest(_ label: String) -> URL? {
        guard let root = VQFolder.shared.url else { return nil }
        let fm = FileManager.default
        let base = root.appendingPathComponent("Registros", isDirectory: true)
        let f = DateFormatter()
        f.locale = Locale(identifier: "en_US_POSIX")
        f.dateFormat = "yyyy-MM-dd_HH-mm-ss"
        let dest = base.appendingPathComponent("\(f.string(from: Date()))-\(label)", isDirectory: true)
        do { try fm.createDirectory(at: dest, withIntermediateDirectories: true) } catch {
            log("Registros: no se pudo crear \(dest.path): \(error.localizedDescription)")
            return nil
        }
        var copied = 0
        stageGameLogs()
        // A crash report older than the boot log belongs to an earlier session: copying it next to
        // a newer log reads as "this session crashed" (2026-10-15: the 09:55 Wwise fault kept
        // reappearing in every later folder).
        let bootDate = (try? fm.attributesOfItem(atPath: bootLog.path))?[.modificationDate] as? Date
        let crashDate = (try? fm.attributesOfItem(atPath: crashLog.path))?[.modificationDate] as? Date
        let staleCrash = bootDate != nil && crashDate != nil && crashDate! < bootDate!.addingTimeInterval(-60)
        for src in liveFiles where !(staleCrash && src == crashLog) {
            let to = dest.appendingPathComponent(src.lastPathComponent)
            try? fm.removeItem(at: to)
            if (try? fm.copyItem(at: src, to: to)) != nil { copied += 1 }
        }
        let old = ((try? fm.contentsOfDirectory(at: base, includingPropertiesForKeys: nil)) ?? [])
            .filter { $0.hasDirectoryPath }.sorted { $0.lastPathComponent > $1.lastPathComponent }
        for stale in old.dropFirst(12) { try? fm.removeItem(at: stale) }
        log("Registros: \(copied) fichero(s) copiados a QUEST/Registros/\(dest.lastPathComponent)")
        return dest
    }

    /// Moves every klepton-crash*.log left from earlier sessions (Documents and QUEST/Registros) to
    /// an `anteriores/` subfolder, named with the date, so the shareable logs only carry the crash
    /// report of the session about to start (a "klepton-crash (4).log" from another build and game
    /// misled a diagnosis). Call AFTER savePreviousSessionToQuest(), which files the old report with
    /// its own session. Keeps the newest 10 per folder.
    static func archiveStaleCrashLogs() {
        let fm = FileManager.default
        var dirs = [fm.urls(for: .documentDirectory, in: .userDomainMask)[0]]
        if let quest = VQFolder.shared.url {
            dirs.append(quest.appendingPathComponent("Registros", isDirectory: true))
        }
        let f = DateFormatter()
        f.locale = Locale(identifier: "en_US_POSIX")
        f.dateFormat = "yyyy-MM-dd_HH-mm-ss"
        let stamp = f.string(from: Date())
        var moved = 0
        for dir in dirs {
            let stale = ((try? fm.contentsOfDirectory(at: dir, includingPropertiesForKeys: nil)) ?? [])
                .filter { $0.lastPathComponent.hasPrefix("klepton-crash") && $0.pathExtension == "log" }
            if stale.isEmpty { continue }
            let dest = dir.appendingPathComponent("anteriores", isDirectory: true)
            try? fm.createDirectory(at: dest, withIntermediateDirectories: true)
            for src in stale {
                let name = src.deletingPathExtension().lastPathComponent
                let to = dest.appendingPathComponent("\(stamp)-\(name).log")
                if (try? fm.moveItem(at: src, to: to)) != nil { moved += 1 }
            }
            let kept = ((try? fm.contentsOfDirectory(at: dest, includingPropertiesForKeys: nil)) ?? [])
                .sorted { $0.lastPathComponent > $1.lastPathComponent }
            for old in kept.dropFirst(10) { try? fm.removeItem(at: old) }
        }
        if moved > 0 { log("Registros: \(moved) klepton-crash*.log de sesiones anteriores movidos a anteriores/") }
    }

    nonisolated(unsafe) private static var gameLaunched = false
    private static let previousSessionLock = NSLock()
    private static let savedDateKey = "vq.lastSavedBootLogDate"

    /// The game has ended in THIS process (immersive space closed): its logs go to QUEST/Registros
    /// now, while they are complete and the app is still alive — not on the next launch, which
    /// depended on the player opening the app again and the folder being reachable then
    /// (2026-10-15: "I have to open and close the app until it decides to save the log").
    /// Off the main thread (tens of MB into File Provider storage); `wait` blocks the caller until
    /// it is done, at most 20 s — for the path that _exit()s right after.
    static func saveSessionEnd(_ game: String?, wait: Bool) {
        fflush(nil)
        let label = "fin-" + (game.map { String($0.prefix(24)).replacingOccurrences(of: " ", with: "_") } ?? "juego")
        let done = DispatchSemaphore(value: 0)
        DispatchQueue.global(qos: .userInitiated).async {
            if saveToQuest(label) != nil,
               let date = (try? FileManager.default.attributesOfItem(atPath: bootLog.path))?[.modificationDate] as? Date {
                UserDefaults.standard.set(date.timeIntervalSince1970, forKey: savedDateKey)
                UserDefaults.standard.synchronize()
            }
            done.signal()
        }
        if wait { _ = done.wait(timeout: .now() + 20) }
    }

    /// At launch: the previous session's logs (guest log, crash report) go to QUEST/Registros once,
    /// so a session that ended in a crash can be read from the Files app.
    /// `atStartup`: the delayed call from the launcher's init. It does nothing once a game has been
    /// launched in this process — by then the boot log is that game's, newer than the saved date,
    /// and was copied a few seconds in (2026-10-15 12:19:03: AC's first 4 s filed as
    /// "sesion-anterior"). Launching a game copies the previous one first, as before.
    static func savePreviousSessionToQuest(atStartup: Bool = false) {
        previousSessionLock.lock()
        let skip = atStartup && gameLaunched
        if !atStartup { gameLaunched = true }
        previousSessionLock.unlock()
        if skip { return }
        guard let date = (try? FileManager.default.attributesOfItem(atPath: bootLog.path))?[.modificationDate] as? Date
        else { return }
        let last = UserDefaults.standard.double(forKey: savedDateKey)
        guard date.timeIntervalSince1970 > last + 1 else { return }
        // The QUEST folder (File Provider storage) is not always reachable right after launch:
        // the start-up copy keeps trying for a minute rather than giving up silently. The copy
        // made when a game is launched tries once (it is in the launch path) and stops the
        // start-up one, whose later attempts would otherwise file the NEW game's log.
        for attempt in 1...(atStartup ? 12 : 1) {
            if atStartup {
                previousSessionLock.lock()
                let launched = gameLaunched
                previousSessionLock.unlock()
                if launched { return }
            }
            if saveToQuest("sesion-anterior" + (previousSessionTag.map { "-" + $0 } ?? "")) != nil {
                UserDefaults.standard.set(date.timeIntervalSince1970, forKey: savedDateKey)
                return
            }
            log("Registros: la sesión anterior no se pudo copiar (intento \(attempt): carpeta QUEST \(VQFolder.shared.url == nil ? "no disponible" : "sin escritura"))")
            if atStartup { Thread.sleep(forTimeInterval: 5) }
        }
    }

    /// Frame captures written by the compositor (capture-<frame>-*.png).
    static var captures: [URL] {
        ((try? FileManager.default.contentsOfDirectory(at: directory, includingPropertiesForKeys: nil)) ?? [])
            .filter { $0.lastPathComponent.hasPrefix("capture-") && $0.pathExtension == "png" }
            .sorted { $0.lastPathComponent < $1.lastPathComponent }
    }

    /// The last `bytes` of a log, for showing in the app.
    static func tail(_ url: URL, bytes: Int = 16_384) -> String {
        guard let h = try? FileHandle(forReadingFrom: url) else { return "" }
        defer { try? h.close() }
        let size = (try? h.seekToEnd()) ?? 0
        try? h.seek(toOffset: size > UInt64(bytes) ? size - UInt64(bytes) : 0)
        return String(decoding: h.readDataToEndOfFile(), as: UTF8.self)
    }
}

/// Every 15 s, a tiny Metal library compiled from a source never seen before (a real trip to
/// MTLCompilerService): logs when the compiler stops being reachable and when it comes back, plus a
/// line every 5 minutes. Log 60: from 2 s into the Vulkan CTS every NEW compiler connection failed
/// ("Connection init failed at lookup"); this says when, against the [hb] ports/fds counts.
enum VQCompilerProbe {
    // Touched only from the timer's serial event handler (and start() once, on the main thread).
    nonisolated(unsafe) private static var timer: DispatchSourceTimer?
    nonisolated(unsafe) private static var lastOK: Bool?
    nonisolated(unsafe) private static var count = 0

    /// The last probe could not reach the compiler: new connections to system services are failing
    /// in this process, and the share sheet would abort the app (log 60). Read from the UI.
    static var servicesBroken: Bool { lastOK == false }

    static func start() {
        guard timer == nil else { return }
        let t = DispatchSource.makeTimerSource(queue: .global(qos: .utility))
        t.schedule(deadline: .now() + 20, repeating: 15)
        t.setEventHandler { probe() }
        timer = t
        t.resume()
    }

    private static func probe() {
        count += 1
        guard let device = MTLCreateSystemDefaultDevice() else { return }
        let n = count
        let source = "#include <metal_stdlib>\nkernel void vq_probe_\(n)(device uint *o [[buffer(0)]]) { o[0] = \(n)u; }\n"
        let t0 = Date()
        var failure: String?
        do { _ = try device.makeLibrary(source: source, options: nil) } catch { failure = error.localizedDescription }
        let ms = Int(Date().timeIntervalSince(t0) * 1000)
        let ok = failure == nil
        if lastOK != ok || n % 20 == 0 {
            let line = ok ? "[xpc] compilador Metal OK (\(ms) ms, sonda \(n))"
                          : "[xpc] compilador Metal INACCESIBLE (sonda \(n), \(ms) ms): \(failure ?? "?")"
            fputs(line + "\n", stderr)
            VQLog.log(line)
        }
        lastOK = ok
    }
}

/// Spanish when the system language is Spanish, else English.
func VQL(_ spanish: String, _ english: String) -> String {
    (Locale.preferredLanguages.first ?? "en").lowercased().hasPrefix("es") ? spanish : english
}

/// Closes that leave no trace in the log (jetsam, a crash in the first seconds): a sentinel file in
/// Registros/ written when a game is launched and removed on a clean close; if it is still there at
/// the next launch, the previous session did not end cleanly. MetricKit adds the system's own view
/// (crash/hang/CPU/disk diagnostics, delivered on a later launch) to the same log.
enum VQSession {
    private static var sentinel: URL { VQLog.directory.appendingPathComponent("sesion-en-curso.txt") }

    /// Call right after VQLog.start(): reports a leftover sentinel, then subscribes to MetricKit.
    static func startup() {
        if let text = try? String(contentsOf: sentinel, encoding: .utf8) {
            let parts = text.split(separator: "\n", maxSplits: 1).map(String.init)
            let when = parts.count > 1 ? Double(parts[1]).map { ISO8601DateFormatter().string(from: Date(timeIntervalSince1970: $0)) } : nil
            VQLog.log("Sesión anterior: terminó sin cierre limpio (juego \(parts.first ?? "?"), iniciada \(when ?? "hora desconocida"))")
            try? FileManager.default.removeItem(at: sentinel)
        }
        #if canImport(MetricKit)
        MXMetricManager.shared.add(VQMetrics.shared)
        VQLog.log("MetricKit: suscrito")
        #endif
    }

    static func begin(game: String) {
        try? "\(game)\n\(Int(Date().timeIntervalSince1970))".write(to: sentinel, atomically: true, encoding: .utf8)
    }

    static func end() {
        VQCacheState.stop()
        try? FileManager.default.removeItem(at: sentinel)
    }
}

#if canImport(MetricKit)
final class VQMetrics: NSObject, MXMetricManagerSubscriber {
    static let shared = VQMetrics()

    func didReceive(_ payloads: [MXDiagnosticPayload]) {
        for p in payloads {
            // The payload as the system gave it, next to the logs, so the share button carries it.
            let stamp = Int(p.timeStampEnd.timeIntervalSince1970)
            try? p.jsonRepresentation().write(to: VQLog.directory.appendingPathComponent("metrickit-\(stamp).json"))
            VQLog.log("MetricKit: diagnóstico \(p.timeStampBegin) - \(p.timeStampEnd): "
                      + "\(p.crashDiagnostics?.count ?? 0) cierre(s), \(p.hangDiagnostics?.count ?? 0) bloqueo(s), "
                      + "\(p.cpuExceptionDiagnostics?.count ?? 0) CPU, \(p.diskWriteExceptionDiagnostics?.count ?? 0) disco")
            for c in p.crashDiagnostics ?? [] {
                let tree = String(decoding: c.callStackTree.jsonRepresentation(), as: UTF8.self)
                VQLog.log("MetricKit: CIERRE exceptionType=\(c.exceptionType.map { "\($0)" } ?? "?") exceptionCode=\(c.exceptionCode.map { "\($0)" } ?? "?") "
                          + "signal=\(c.signal.map { "\($0)" } ?? "?") terminationReason=\(c.terminationReason ?? "?") "
                          + "versión=\(c.applicationVersion)\nvirtualMemoryRegionInfo=\(c.virtualMemoryRegionInfo ?? "?")\ncallStackTree=\(tree)")
            }
            for h in p.hangDiagnostics ?? [] {
                VQLog.log("MetricKit: BLOQUEO duración=\(h.hangDuration)\ncallStackTree=\(String(decoding: h.callStackTree.jsonRepresentation(), as: UTF8.self))")
            }
            for c in p.cpuExceptionDiagnostics ?? [] {
                VQLog.log("MetricKit: EXCESO DE CPU total=\(c.totalCPUTime) muestreado=\(c.totalSampledTime)\ncallStackTree=\(String(decoding: c.callStackTree.jsonRepresentation(), as: UTF8.self))")
            }
            for d in p.diskWriteExceptionDiagnostics ?? [] {
                VQLog.log("MetricKit: EXCESO DE ESCRITURA totalEscrito=\(d.totalWritesCaused)")
            }
        }
    }

    func didReceive(_ payloads: [MXMetricPayload]) {}
}
#endif
