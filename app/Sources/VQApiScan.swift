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

    static func scan(tree: URL) -> Report {
        var r = Report()
        let fm = FileManager.default
        let libDir = tree.appendingPathComponent("lib/arm64-v8a", isDirectory: true)
        let libs = ((try? fm.contentsOfDirectory(at: libDir, includingPropertiesForKeys: nil)) ?? [])
            .filter { $0.pathExtension == "so" }
            .sorted { $0.lastPathComponent < $1.lastPathComponent }
        r.libraries = libs.count
        var exports = Set<String>()
        var imports: [(lib: String, names: [String])] = []
        for lib in libs {
            if let p = vq_elf_symbols(lib.path, 1) {
                exports.formUnion(String(cString: p).split(separator: "\n").map(String.init))
                free(p)
            }
            if let p = vq_elf_symbols(lib.path, 0) {
                imports.append((lib.lastPathComponent, String(cString: p).split(separator: "\n").map(String.init)))
                free(p)
            }
        }
        for (lib, names) in imports {
            for raw in names {
                let weak = raw.hasPrefix("w ")
                let name = weak ? String(raw.dropFirst(2)) : raw
                r.imports += 1
                if exports.contains(name) { continue }
                switch kl_shim_known(name) {
                case 1: continue
                case 2: r.families += 1; continue
                default:
                    if weak { r.weakMissing[name, default: []].append(lib) } else { r.missing[name, default: []].append(lib) }
                }
            }
        }
        var classes = Set<String>()
        var sources = libs
        let metadata = tree.appendingPathComponent("assets/bin/Data/Managed/Metadata/global-metadata.dat")
        if fm.fileExists(atPath: metadata.path) { sources.append(metadata) }
        for f in sources {
            if let p = vq_java_classes(f.path) {
                classes.formUnion(String(cString: p).split(separator: "\n").map(String.init))
                free(p)
            }
        }
        r.classes = classes.sorted()
        r.unknownClasses = r.classes.filter { kl_jni_knows_class($0) == 0 }
        return r
    }

    /// Scans and logs (app log and the guest log's header).
    static func run(game: VQGame, tree: URL) {
        let t0 = Date()
        let r = scan(tree: tree)
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
