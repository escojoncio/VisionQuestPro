// VisionQuestPro: preparing a game on the headset, once.
//
//   QUEST/Cache/<juego>/tree/AndroidManifest.xml   the manifest as text (what Klepton reads)
//   QUEST/Cache/<juego>/tree/assets/...            the APK's assets/ (Unity reads them as files)
//   QUEST/Cache/<juego>/dylibs/<lib>.dylib          each lib/arm64-v8a/*.so through klepton-ld
//                                                  (in process), unsigned: survives reinstalls
//   QUEST/Cache/<juego>/stamp.txt                   what it was made from: APK size + date,
//                                                  converter version
//   Library/Application Support/VisionQuestPro/Signed/<juego>/<lib>.dylib
//                                                  the signed copies the game loads, with
//                                                  sign.txt (conversion stamp, certificate, variant)
//
// Why the signed copies live inside the app: VPKEngine's packs are only accepted by the system when
// signed as a copy in the app's own container (Application Support), with the app's identifier
// and requirement set and the certificate the app was signed with; libraries mapped from a folder
// outside the container are not something the system has ever been seen to accept. Signing is
// cheap next to the conversion (page hashes), so the copies are redone whenever the certificate
// changes or the app is reinstalled, while the conversion in QUEST is kept.
//
// Nothing is made again while the stamps match. The OBB and the data folder are not copied:
// the game reads them where they are.

import Foundation

enum VQPrepare {
    /// Bumped when the conversion changes (a new klepton-ld, a new layout): every cache is redone.
    static let converterVersion = "kld-3"

    struct Prepared {
        let tree: URL
        let dylibs: URL
    }

    enum Problem: Error, LocalizedError {
        case noFolder
        case failed(String)
        var errorDescription: String? {
            switch self {
            case .noFolder: return VQL("No hay carpeta QUEST.", "There is no QUEST folder.")
            case .failed(let m): return m
            }
        }
    }

    /// The file name a translation has on disk ('+' is not allowed: libc++_shared -> libcxx_shared),
    /// the same map as Klepton's kl_image.c.
    static func dylibName(for so: String) -> String {
        var stem = (so as NSString).lastPathComponent
        if let r = stem.range(of: ".so") { stem = String(stem[..<r.lowerBound]) }
        return stem.replacingOccurrences(of: "+", with: "x") + ".dylib"
    }

    private static func stamp(_ game: VQGame) -> String {
        let attrs = (try? FileManager.default.attributesOfItem(atPath: game.apk.path)) ?? [:]
        let size = (attrs[.size] as? NSNumber)?.int64Value ?? 0
        let date = (attrs[.modificationDate] as? Date)?.timeIntervalSince1970 ?? 0
        return "\(converterVersion)|\(game.apk.lastPathComponent)|\(size)|\(Int64(date))"
    }

    /// The app-internal folder with the signed copies of a game's libraries.
    static func signedFolder(for gameID: String) -> URL {
        FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("VisionQuestPro/Signed", isDirectory: true)
            .appendingPathComponent(gameID, isDirectory: true)
    }

    /// Whether the conversion is up to date (signing may still be needed, which is quick).
    static func isPrepared(_ game: VQGame) -> Bool {
        guard let cache = VQFolder.shared.cache(for: game.id),
              let text = try? String(contentsOf: cache.appendingPathComponent("stamp.txt"), encoding: .utf8) else { return false }
        return text.components(separatedBy: "\n").first == stamp(game)
    }

    /// Prepares `game` if needed. Slow the first time: call off the main thread. `progress` gets
    /// a fraction (0...1) and a line of text.
    static func prepare(_ game: VQGame, progress: @escaping (Double, String) -> Void) throws -> Prepared {
        guard let cache = VQFolder.shared.cache(for: game.id) else { throw Problem.noFolder }
        let tree = cache.appendingPathComponent("tree", isDirectory: true)
        let dylibs = cache.appendingPathComponent("dylibs", isDirectory: true)
        let work = cache.appendingPathComponent("work", isDirectory: true)
        let stampURL = cache.appendingPathComponent("stamp.txt")
        let identity: VQSigner.Identity
        do { identity = try VQSigner.storedIdentity() } catch { throw Problem.failed(error.localizedDescription) }
        let want = stamp(game)
        if let text = try? String(contentsOf: stampURL, encoding: .utf8), text.components(separatedBy: "\n").first == want {
            VQLog.log("Preparar: \(game.id) ya convertido (\(want))")
        } else {
            try convert(game, tree: tree, dylibs: dylibs, work: work, stampURL: stampURL, want: want, progress: progress)
        }
        writeVersionFile(game, tree: tree)
        let signed = try sign(game, from: dylibs, conversion: want, identity: identity, progress: progress)
        progress(1, VQL("Listo", "Done"))
        return Prepared(tree: tree, dylibs: signed)
    }

    /// Klepton names the game's OBB (main.<versionCode>.<package>.obb) from tree/apktool.yml; without
    /// it, it falls back to a fixed number and the game data is never found. Written on every launch
    /// from the APK's own manifest, so trees prepared before this existed are fixed too.
    private static func writeVersionFile(_ game: VQGame, tree: URL) {
        let m = game.manifestText
        guard let code = VQGames.attribute(m, element: "manifest", name: "android:versionCode"),
              Int(code) != nil else {
            VQLog.log("Preparar: \(game.id): el manifiesto no trae versionCode; Klepton usará su valor por defecto")
            return
        }
        let name = VQGames.attribute(m, element: "manifest", name: "android:versionName") ?? "0"
        let text = "versionInfo:\n  versionCode: '\(code)'\n  versionName: \(name)\n"
        let url = tree.appendingPathComponent("apktool.yml")
        if (try? String(contentsOf: url, encoding: .utf8)) == text { return }
        do {
            try text.write(to: url, atomically: true, encoding: .utf8)
            VQLog.log("Preparar: \(game.id): versionCode \(code) / \(name) -> apktool.yml")
        } catch {
            VQLog.log("Preparar: \(game.id): no se pudo escribir apktool.yml (\(error.localizedDescription))")
        }
    }

    private static func convert(_ game: VQGame, tree: URL, dylibs: URL, work: URL, stampURL: URL, want: String,
                                progress: @escaping (Double, String) -> Void) throws {
        let fm = FileManager.default
        VQLog.log("Preparar: \(game.id): convierte (\(want))")
        let started = Date()
        try? fm.removeItem(at: stampURL)
        for d in [tree, dylibs, work] { try? fm.removeItem(at: d) }
        for d in [tree, dylibs, work] { try fm.createDirectory(at: d, withIntermediateDirectories: true) }

        var err = [CChar](repeating: 0, count: 512)
        guard let zip = vq_zip_open(game.apk.path, &err, err.count) else {
            throw Problem.failed(VQL("No se pudo leer el APK: ", "The APK could not be read: ") + String(cString: err))
        }
        defer { vq_zip_close(zip) }

        // 1. The manifest as text, and assets/ (made even when empty: Klepton requires it).
        try fm.createDirectory(at: tree.appendingPathComponent("assets", isDirectory: true), withIntermediateDirectories: true)
        try game.manifestText.write(to: tree.appendingPathComponent("AndroidManifest.xml"), atomically: true, encoding: .utf8)
        var assetEntries: [(Int32, String)] = []
        var assetBytes: UInt64 = 0
        for i in 0..<vq_zip_count(zip) {
            guard let c = vq_zip_name(zip, i) else { continue }
            let name = String(cString: c)
            guard name.hasPrefix("assets/"), !name.hasSuffix("/"), !name.contains("..") else { continue }
            assetEntries.append((i, name))
            assetBytes += vq_zip_size(zip, i)
        }
        VQLog.log("Preparar: \(assetEntries.count) ficheros en assets/ (\(assetBytes >> 20) MB)")
        var done: UInt64 = 0
        for (k, (i, name)) in assetEntries.enumerated() {
            let out = tree.appendingPathComponent(name)
            try fm.createDirectory(at: out.deletingLastPathComponent(), withIntermediateDirectories: true)
            if vq_zip_extract(zip, i, out.path, &err, err.count) != 0 {
                throw Problem.failed(VQL("No se pudo extraer ", "Could not extract ") + "\(name): " + String(cString: err))
            }
            done += vq_zip_size(zip, i)
            if k % 25 == 0 || k == assetEntries.count - 1 {
                progress(0.3 * Double(done) / Double(max(assetBytes, 1)), VQL("Extrayendo assets", "Extracting assets") + " \(k + 1)/\(assetEntries.count)")
            }
        }

        // 2. Each library through klepton-ld (unsigned output, kept in QUEST).
        for (k, entry) in game.libraries.enumerated() {
            let so = (entry as NSString).lastPathComponent
            progress(0.3 + 0.5 * Double(k) / Double(max(game.libraries.count, 1)),
                     VQL("Convirtiendo", "Converting") + " \(so) (\(k + 1)/\(game.libraries.count))")
            let elf = work.appendingPathComponent(so)
            let i = vq_zip_find(zip, entry)
            if i < 0 || vq_zip_extract(zip, i, elf.path, &err, err.count) != 0 {
                throw Problem.failed(VQL("No se pudo extraer ", "Could not extract ") + "\(so): " + String(cString: err))
            }
            let out = dylibs.appendingPathComponent(dylibName(for: so))
            let t0 = Date()
            if klepton_ld_run(elf.path, out.path, &err, err.count) != 0 {
                throw Problem.failed("klepton-ld \(so): " + String(cString: err))
            }
            let mb = ((try? fm.attributesOfItem(atPath: out.path)[.size] as? NSNumber)?.int64Value ?? 0) >> 20
            VQLog.log("Preparar: \(so) -> \(out.lastPathComponent) (\(mb) MB, \(String(format: "%.1f", Date().timeIntervalSince(t0))) s)")
            try? fm.removeItem(at: elf)
        }
        try? fm.removeItem(at: work)
        try (want + "\n").write(to: stampURL, atomically: true, encoding: .utf8)
        VQLog.log("Preparar: \(game.id) convertido en \(Int(Date().timeIntervalSince(started))) s")
    }

    /// Copies the converted libraries into the app and signs the copies (VPKEngine's way: the app's
    /// identifier and requirement set, the variant the system accepts, tried with dlopen on the
    /// first library — a klepton-ld image runs no code when loaded).
    private static func sign(_ game: VQGame, from dylibs: URL, conversion: String, identity: VQSigner.Identity,
                             progress: @escaping (Double, String) -> Void) throws -> URL {
        let fm = FileManager.default
        let folder = signedFolder(for: game.id)
        let stampURL = folder.appendingPathComponent("sign.txt")
        let fingerprint = VQSigner.fingerprint(identity)
        let libs = ((try? fm.contentsOfDirectory(at: dylibs, includingPropertiesForKeys: nil)) ?? [])
            .filter { $0.pathExtension == "dylib" }
            .sorted { $0.lastPathComponent < $1.lastPathComponent }
        if libs.isEmpty { throw Problem.failed(VQL("La conversión no dejó librerías.", "The conversion left no libraries.")) }
        if let text = try? String(contentsOf: stampURL, encoding: .utf8) {
            let lines = text.components(separatedBy: "\n")
            if lines.count >= 2, lines[0] == conversion + "|" + fingerprint,
               libs.allSatisfy({ fm.fileExists(atPath: folder.appendingPathComponent($0.lastPathComponent).path) }) {
                VQLog.log("Preparar: firmas al día (\(lines[1]))")
                return folder
            }
        }
        try? fm.removeItem(at: folder)
        try fm.createDirectory(at: folder, withIntermediateDirectories: true)
        var variants = VQSigner.variants()
        var chosen: VQSigner.Variant?
        for (k, lib) in libs.enumerated() {
            progress(0.8 + 0.2 * Double(k) / Double(libs.count), VQL("Firmando", "Signing") + " \(lib.lastPathComponent)")
            let out = folder.appendingPathComponent(lib.lastPathComponent)
            while true {
                let partial = out.appendingPathExtension("partial")
                try? fm.removeItem(at: partial)
                try fm.copyItem(at: lib, to: partial)
                let variant = chosen ?? variants[0]
                do {
                    try VQSigner.sign(path: partial.path, variant: variant, identity: identity)
                } catch {
                    try? fm.removeItem(at: partial)
                    throw Problem.failed("\(lib.lastPathComponent): \(error.localizedDescription)")
                }
                try? fm.removeItem(at: out)
                try fm.moveItem(at: partial, to: out)
                if chosen != nil { break }
                if let h = dlopen(out.path, RTLD_NOW | RTLD_LOCAL) {
                    _ = h  // left open: the runtime dlopens the same path later
                    chosen = variant
                    VQSigner.remember(variant)
                    VQLog.log("Preparar: el sistema acepta la firma \(variant.name) (\(out.path))")
                    break
                }
                let reason = dlerror().map { String(cString: $0) } ?? "?"
                VQLog.log("Preparar: dlopen rechazó la firma \(variant.name): \(reason)")
                variants.removeFirst()
                if variants.isEmpty || !reason.contains("signature") {
                    try? fm.removeItem(at: out)
                    throw Problem.failed(VQL("El sistema no acepta la librería firmada: ", "The system refuses the signed library: ") + reason)
                }
            }
        }
        try (conversion + "|" + fingerprint + "\nvariante=\(chosen?.name ?? "?")\n").write(to: stampURL, atomically: true, encoding: .utf8)
        VQLog.log("Preparar: \(libs.count) librerías firmadas en la app (\(folder.path))")
        return folder
    }
}
