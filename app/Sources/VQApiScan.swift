// VisionQuestPro: «Escáner de APIs» — what a game asks of Android, read from its own files
// before it runs, against what Klepton implements. Every launch (it takes a fraction of a second
// and the answer changes with every build of Klepton), so a log says from the first line which
// native imports would fail by name and which system Java classes get only a generic answer.
//
//   native   each library's undefined .dynsym imports, minus what its sibling libraries export,
//            checked with kl_shim_known (a shim, a served family — GL/Vulkan/OpenXR/audio/media —
//            or nothing). Weak imports are listed apart: a missing weak import is NULL, not a crash.
//   Java     system class names (android/, java/, dalvik/, com/oculus/…) found as strings in the
//            libraries and in IL2CPP's global-metadata.dat, checked with kl_jni_knows_class.

import Foundation

enum VQApiScan {
    struct Report {
        var libraries = 0
        var imports = 0
        var missing: [String: [String]] = [:]      // symbol -> libraries importing it
        var weakMissing: [String: [String]] = [:]
        var families = 0
        var classes: [String] = []
        var unknownClasses: [String] = []
    }

    /// The raw facts of a game's libraries (names only, no judgement): cached beside the tree,
    /// because preparation deletes the ELF files once they are converted. Rebuilt from the APK when
    /// the APK changes. Lines: "E sym" export, "I lib sym" / "W lib sym" import (weak), "C class", "N name" (ovrp_*/xr* string).
    static func raw(apk: URL, tree: URL) -> [Substring] {
        let fm = FileManager.default
        let cache = tree.deletingLastPathComponent().appendingPathComponent("apiscan-raw.txt")
        let attrs = try? fm.attributesOfItem(atPath: apk.path)
        let stamp = "#v3 \((attrs?[.size] as? Int) ?? 0) \(Int(((attrs?[.modificationDate] as? Date) ?? .distantPast).timeIntervalSince1970))"
        if let text = try? String(contentsOf: cache, encoding: .utf8), text.hasPrefix(stamp + "\n") {
            return text.split(separator: "\n").dropFirst().map { $0 }
        }
        var lines: [String] = [stamp]
        var err = [CChar](repeating: 0, count: 256)
        guard let zip = vq_zip_open(apk.path, &err, err.count) else { return [] }
        defer { vq_zip_close(zip) }
        let tmp = fm.temporaryDirectory.appendingPathComponent("vq-apiscan-\(UUID().uuidString).so")
        defer { try? fm.removeItem(at: tmp) }
        let prefix = "lib/arm64-v8a/"
        var classes = Set<String>()
        var apiNames = Set<String>()
        func addNames(_ path: String) {
            if let p = vq_api_names(path) {
                apiNames.formUnion(String(cString: p).split(separator: "\n").map(String.init))
                free(p)
            }
        }
        for i in 0..<vq_zip_count(zip) {
            guard let n = vq_zip_name(zip, i).map({ String(cString: $0) }),
                  n.hasPrefix(prefix), n.hasSuffix(".so"), !n.dropFirst(prefix.count).contains("/") else { continue }
            let lib = String(n.dropFirst(prefix.count))
            guard vq_zip_extract(zip, i, tmp.path, &err, err.count) == 0 else { continue }
            lines.append("L \(lib)")
            if let p = vq_elf_symbols(tmp.path, 1) {
                for sym in String(cString: p).split(separator: "\n") { lines.append("E \(sym)") }
                free(p)
            }
            if let p = vq_elf_symbols(tmp.path, 0) {
                for sym in String(cString: p).split(separator: "\n") {
                    lines.append(sym.hasPrefix("w ") ? "W \(lib) \(sym.dropFirst(2))" : "I \(lib) \(sym)")
                }
                free(p)
            }
            addNames(tmp.path)
            if let p = vq_java_classes(tmp.path) {
                classes.formUnion(String(cString: p).split(separator: "\n").map(String.init))
                free(p)
            }
        }
        // Managed code, read from the APK itself (no preparation needed): IL2CPP's metadata
        // (DllImport names, Java class names) and, in Mono, the DLLs of Managed/.
        let managed = "assets/bin/Data/Managed/"
        for i in 0..<vq_zip_count(zip) {
            guard let n = vq_zip_name(zip, i).map({ String(cString: $0) }), n.hasPrefix(managed),
                  n.hasSuffix("global-metadata.dat") || (n.hasSuffix(".dll") && !n.dropFirst(managed.count).contains("/")),
                  vq_zip_extract(zip, i, tmp.path, &err, err.count) == 0 else { continue }
            addNames(tmp.path)
            if n.hasSuffix(".dat"), let p = vq_java_classes(tmp.path) {
                classes.formUnion(String(cString: p).split(separator: "\n").map(String.init))
                free(p)
            }
        }
        lines += classes.sorted().map { "C \($0)" }
        lines += apiNames.sorted().map { "N \($0)" }
        // Nothing extracted (unreadable APK): do not cache an empty answer.
        if lines.contains(where: { $0.hasPrefix("L ") }) {
            try? (lines.joined(separator: "\n") + "\n").write(to: cache, atomically: true, encoding: .utf8)
        }
        return lines.dropFirst().map { Substring($0) }
    }

    static func scan(apk: URL, tree: URL) -> Report {
        var r = Report()
        var exports = Set<Substring>()
        var imports: [(lib: Substring, name: Substring, weak: Bool)] = []
        var libs = Set<Substring>()
        for line in raw(apk: apk, tree: tree) {
            let parts = line.split(separator: " ", maxSplits: 2)
            guard parts.count >= 2 else { continue }
            switch parts[0] {
            case "L": libs.insert(parts[1])
            case "E": exports.insert(parts[1])
            case "I", "W":
                guard parts.count == 3 else { continue }
                libs.insert(parts[1])
                imports.append((parts[1], parts[2], parts[0] == "W"))
            case "C": r.classes.append(String(parts[1]))
            default: break
            }
        }
        r.libraries = libs.count
        for imp in imports {
            r.imports += 1
            if exports.contains(imp.name) { continue }
            let name = String(imp.name)
            switch kl_shim_known(name) {
            case 1: continue
            case 2: r.families += 1; continue
            default:
                if imp.weak { r.weakMissing[name, default: []].append(String(imp.lib)) }
                else { r.missing[name, default: []].append(String(imp.lib)) }
            }
        }
        r.unknownClasses = r.classes.filter { kl_jni_knows_class($0) == 0 }
        return r
    }

    /// One game's Meta API inventory (ovrp_*, xr*) against what the runtime serves, from the cached
    /// raw facts (the APK is read once per game version). `unimplemented`: names with only a stub,
    /// a shared "unsupported" answer, or (xr) a refusal / no entry point.
    struct Inventory {
        var summary = ""
        var text = ""
        var names = 0
        var unimplemented: [String] = []
    }

    static func inventory(game: VQGame, cache: URL) -> Inventory {
        var src: [String: Set<Character>] = [:]     // name -> E exported, I imported, N string
        for line in raw(apk: game.apk, tree: cache.appendingPathComponent("tree", isDirectory: true)) {
            let parts = line.split(separator: " ", maxSplits: 2)
            guard parts.count >= 2, let k = parts[0].first else { continue }
            let name = String(parts.last!)
            guard name.hasPrefix("ovrp_") || (name.hasPrefix("xr") && name.dropFirst(2).first?.isUppercase == true) else { continue }
            switch k {
            case "E", "N": src[name, default: []].insert(k)
            case "I", "W": src[name, default: []].insert("I")
            default: break
            }
        }
        var inv = Inventory()
        var rows: [String] = []
        for name in src.keys.sorted() {
            let status: String
            if name.hasPrefix("ovrp_") {
                let c = Int(kl_ovrp_status(name))
                status = c == 2 ? "impl" : c == 1 ? "ok/genérico" : c == 3 ? "unsupported" : "stub"
                if c == 0 || c == 3 { inv.unimplemented.append(name) }
            } else {
                let c = Int(kl_openxr_status(name))
                status = c == 2 ? "impl" : c == 0 ? "rechazada" : "no servida"
                if c != 2 { inv.unimplemented.append(name) }
            }
            rows.append("\(status)\t\(name) [\(String(src[name]!.sorted()))]")
        }
        let ovrp = src.keys.filter { $0.hasPrefix("ovrp_") }.count
        inv.names = src.count
        inv.summary = "\(game.id) v\(game.version): \(src.count) nombres únicos (\(ovrp) ovrp_*, \(src.count - ovrp) xr*), "
            + "implementados \(src.count - inv.unimplemented.count), sin implementar \(inv.unimplemented.count)"
        inv.text = inv.summary + "\n# [E exportado por una librería del juego, I importado, N cadena en librería/código gestionado]\n"
            + "# estado: impl, ok/genérico (respuesta compartida), unsupported (compartida), stub (solo registra), rechazada/no servida (xr)\n"
            + rows.joined(separator: "\n") + "\n"
        return inv
    }

    private static let allLock = NSLock()
    nonisolated(unsafe) private static var allRunning = false
    nonisolated(unsafe) private static var allLatest: [VQGame] = []   // the newest list, guarded by allLock
    private static let appOpened = Date()
    /// The inventory is diagnostics: not before this long after the app opens, so the launcher's
    /// covers and buttons have the File Provider storage to themselves.
    private static let allDelay: TimeInterval = 60

    /// What the inventory of a game was made from: the APK (name, size, date; its libraries live
    /// inside it) and this build of the app (the statuses come from Klepton, which changes with it).
    private static let buildStamp: String = {
        let info = Bundle.main.infoDictionary ?? [:]
        let exe = Bundle.main.executableURL.flatMap { try? FileManager.default.attributesOfItem(atPath: $0.path) } ?? [:]
        let size = (exe[.size] as? NSNumber)?.int64Value ?? 0
        let date = Int64(((exe[.modificationDate] as? Date) ?? .distantPast).timeIntervalSince1970)
        return "\(info["CFBundleShortVersionString"] ?? "?")|\(info["CFBundleVersion"] ?? "?")|\(size)|\(date)"
    }()

    /// Nil when the APK cannot be read: then nothing is cached.
    private static func inventorySignature(_ game: VQGame) -> String? {
        guard let attrs = try? FileManager.default.attributesOfItem(atPath: game.apk.path) else { return nil }
        let size = (attrs[.size] as? NSNumber)?.int64Value ?? 0
        let date = Int64(((attrs[.modificationDate] as? Date) ?? .distantPast).timeIntervalSince1970)
        return "\(game.apk.lastPathComponent)|\(size)|\(date)|\(buildStamp)"
    }

    /// A game is being prepared or is running. Called from a background thread only (it waits for the main one).
    private static func launching() -> Bool {
        DispatchQueue.main.sync { MainActor.assumeIsolated { VQModel.shared.busy != nil || VQModel.shared.running } }
    }

    private static let unimplementedStatus: Set<Substring> = ["stub", "unsupported", "rechazada", "no servida"]

    /// The names without implementation in an inventory text (what `Inventory.unimplemented` held).
    private static func unimplementedNames(in text: String) -> [String] {
        var names: [String] = []
        for line in text.split(separator: "\n").dropFirst() where !line.hasPrefix("#") {
            guard let tab = line.firstIndex(of: "\t"), unimplementedStatus.contains(line[..<tab]),
                  let name = line[line.index(after: tab)...].split(separator: " ").first else { continue }
            names.append(String(name))
        }
        return names
    }

    /// In the background, ~60 s after the app opens and never while a game is being prepared or
    /// runs: the inventory of EVERY detected game, with no preparation or launch. A game is only
    /// scanned again when its APK or this build of the app changed (meta-api.sig beside its
    /// meta-api.txt); otherwise its cached meta-api.txt is read. Per game a line in the app log and
    /// <cache>/meta-api.txt; then a global summary (the unimplemented names of all games, with how
    /// many games use each). The same text goes to Registros/meta-api.txt, which the share-logs
    /// buttons include.
    static func inventoryAll(_ games: [VQGame]) {
        if games.isEmpty { return }
        allLock.lock()
        allLatest = games
        let busy = allRunning
        allRunning = true
        allLock.unlock()
        if busy { return }
        let wait = max(0, allDelay - Date().timeIntervalSince(appOpened))
        DispatchQueue.global(qos: .background).asyncAfter(deadline: .now() + wait) {
            while launching() { Thread.sleep(forTimeInterval: 10) }
            let t0 = Date()
            allLock.lock()
            let list = allLatest
            allLock.unlock()
            var uses: [String: Int] = [:]
            var report = ""
            for game in list {
                while launching() { Thread.sleep(forTimeInterval: 10) }
                guard let cache = VQFolder.shared.cache(for: game.id) else { continue }
                let metaURL = cache.appendingPathComponent("meta-api.txt")
                let sigURL = cache.appendingPathComponent("meta-api.sig")
                let sig = inventorySignature(game)
                let text: String
                if let sig, (try? String(contentsOf: sigURL, encoding: .utf8)) == sig,
                   let cached = try? String(contentsOf: metaURL, encoding: .utf8) {
                    text = cached
                } else {
                    let inv = inventory(game: game, cache: cache)
                    text = inv.text
                    try? text.write(to: metaURL, atomically: true, encoding: .utf8)
                    // An empty answer (unreadable APK) is never remembered.
                    if let sig, inv.names > 0 { try? sig.write(to: sigURL, atomically: true, encoding: .utf8) }
                    else { try? FileManager.default.removeItem(at: sigURL) }
                }
                VQLog.log("Meta API: " + (text.split(separator: "\n", maxSplits: 1).first.map(String.init) ?? ""))
                for n in unimplementedNames(in: text) { uses[n, default: 0] += 1 }
                report += "\n=== \(game.id) ===\n" + text
                Thread.sleep(forTimeInterval: 0.05)   // do not saturate the File Provider
            }
            let ranked = uses.sorted { $0.value != $1.value ? $0.value > $1.value : $0.key < $1.key }
            let global = "Meta API global: \(list.count) juegos, \(ranked.count) nombres sin implementar: "
                + ranked.map { "\($0.key) (\($0.value))" }.joined(separator: ", ")
                + String(format: " (%.2f s)", Date().timeIntervalSince(t0))
            VQLog.log(global)
            try? FileManager.default.createDirectory(at: VQLog.directory, withIntermediateDirectories: true)
            try? (global + "\n" + report).write(to: VQLog.directory.appendingPathComponent("meta-api.txt"), atomically: true, encoding: .utf8)
            allLock.lock(); allRunning = false; allLock.unlock()
        }
    }

    /// Scans and logs (app log and the guest log's header).
    static func run(game: VQGame, tree: URL) {
        let t0 = Date()
        let r = scan(apk: game.apk, tree: tree)
        func list(_ d: [String: [String]], _ max: Int) -> String {
            d.keys.sorted().prefix(max).map { "\($0) (\(Set(d[$0]!).sorted().joined(separator: ", ")))" }.joined(separator: "; ")
                + (d.count > max ? "; … y \(d.count - max) más" : "")
        }
        var text = "Escáner de APIs: \(game.id): \(r.libraries) librerías, \(r.imports) importaciones nativas "
            + "(\(r.families) a capas servidas: GL/Vulkan/OpenXR/audio/vídeo/plataforma); "
            + "SIN IMPLEMENTAR \(r.missing.count)"
        if !r.missing.isEmpty { text += ": " + list(r.missing, 60) }
        text += "; débiles sin implementar \(r.weakMissing.count) (son NULL, no fallan solas)"
        if !r.weakMissing.isEmpty { text += ": " + list(r.weakMissing, 25) }
        text += ". Clases Java del sistema citadas: \(r.classes.count); solo respuesta genérica (sin implementación propia): "
            + "\(r.unknownClasses.count)"
        if !r.unknownClasses.isEmpty {
            text += ": " + r.unknownClasses.prefix(80).joined(separator: ", ")
                + (r.unknownClasses.count > 80 ? ", … y \(r.unknownClasses.count - 80) más" : "")
        }
        text += String(format: " (%.2f s)", Date().timeIntervalSince(t0))
        VQLog.log(text)
    }
}
