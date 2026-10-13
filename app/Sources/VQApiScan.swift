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
    /// the APK changes. Lines: "E sym" export, "I lib sym" / "W lib sym" import (weak), "C class".
    static func raw(apk: URL, tree: URL) -> [Substring] {
        let fm = FileManager.default
        let cache = tree.deletingLastPathComponent().appendingPathComponent("apiscan-raw.txt")
        let attrs = try? fm.attributesOfItem(atPath: apk.path)
        let stamp = "#v2 \((attrs?[.size] as? Int) ?? 0) \(Int(((attrs?[.modificationDate] as? Date) ?? .distantPast).timeIntervalSince1970))"
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
            if let p = vq_java_classes(tmp.path) {
                classes.formUnion(String(cString: p).split(separator: "\n").map(String.init))
                free(p)
            }
        }
        let metadata = tree.appendingPathComponent("assets/bin/Data/Managed/Metadata/global-metadata.dat")
        if fm.fileExists(atPath: metadata.path), let p = vq_java_classes(metadata.path) {
            classes.formUnion(String(cString: p).split(separator: "\n").map(String.init))
            free(p)
        }
        lines += classes.sorted().map { "C \($0)" }
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
