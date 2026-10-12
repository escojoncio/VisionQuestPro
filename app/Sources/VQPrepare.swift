// VisionQuestPro: preparing a game on the headset, once.
//
//   QUEST/Cache/<juego>/tree/AndroidManifest.xml   the manifest as text (what Klepton reads)
//   QUEST/Cache/<juego>/tree/assets/...            the APK's assets/ (Unity reads them as files)
//   QUEST/Cache/<juego>/dylibs/<lib>.dylib          each lib/arm64-v8a/*.so through klepton-ld
//                                                  (in process), signed with the player's
//                                                  certificate (VPEngine's signature)
//   QUEST/Cache/<juego>/stamp.txt                   what it was made from: APK size + date,
//                                                  certificate, converter version, variant
//
// Nothing is made again while the stamp matches. The OBB and the data folder are not copied:
// the game reads them where they are.

import Foundation

enum VQPrepare {
    /// Bumped when the conversion changes (a new klepton-ld, a new layout): every cache is redone.
    static let converterVersion = "kld-1"

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

    private static func stamp(_ game: VQGame, _ identity: VQSigner.Identity) -> String {
        stamp(game, fingerprint: VQSigner.fingerprint(identity))
    }

    private static func stamp(_ game: VQGame, fingerprint: String) -> String {
        let attrs = (try? FileManager.default.attributesOfItem(atPath: game.apk.path)) ?? [:]
        let size = (attrs[.size] as? NSNumber)?.int64Value ?? 0
        let date = (attrs[.modificationDate] as? Date)?.timeIntervalSince1970 ?? 0
        return "\(converterVersion)|\(game.apk.lastPathComponent)|\(size)|\(Int64(date))|\(fingerprint)"
    }

    /// Whether the cache is up to date (no work to do).
    static func isPrepared(_ game: VQGame) -> Bool {
        guard let cache = VQFolder.shared.cache(for: game.id),
              let fingerprint = VQSigner.storedFingerprint,
              let text = try? String(contentsOf: cache.appendingPathComponent("stamp.txt"), encoding: .utf8) else { return false }
        return text.components(separatedBy: "\n").first == stamp(game, fingerprint: fingerprint)
    }

    /// Prepares `game` if needed. Slow the first time: call off the main thread. `progress` gets
    /// a fraction (0...1) and a line of text.
    static func prepare(_ game: VQGame, progress: @escaping (Double, String) -> Void) throws -> Prepared {
        guard let cache = VQFolder.shared.cache(for: game.id) else { throw Problem.noFolder }
        let fm = FileManager.default
        let tree = cache.appendingPathComponent("tree", isDirectory: true)
        let dylibs = cache.appendingPathComponent("dylibs", isDirectory: true)
        let work = cache.appendingPathComponent("work", isDirectory: true)
        let stampURL = cache.appendingPathComponent("stamp.txt")
        let identity: VQSigner.Identity
        do { identity = try VQSigner.storedIdentity() } catch { throw Problem.failed(error.localizedDescription) }
        let want = stamp(game, identity)
        if let text = try? String(contentsOf: stampURL, encoding: .utf8), text.components(separatedBy: "\n").first == want {
            VQLog.log("Preparar: \(game.id) ya preparado (\(want))")
            return Prepared(tree: tree, dylibs: dylibs)
        }
        VQLog.log("Preparar: \(game.id): empieza (\(want))")
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
                progress(0.4 * Double(done) / Double(max(assetBytes, 1)), VQL("Extrayendo assets", "Extracting assets") + " \(k + 1)/\(assetEntries.count)")
            }
        }

        // 2. Each library: extract, klepton-ld, sign. The first one also chooses the signature
        //    variant the system accepts (tried with dlopen: a klepton-ld image runs no code on load).
        var variants = VQSigner.variants()
        var chosen: VQSigner.Variant?
        for (k, entry) in game.libraries.enumerated() {
            let so = (entry as NSString).lastPathComponent
            let base = 0.4 + 0.6 * Double(k) / Double(max(game.libraries.count, 1))
            progress(base, VQL("Convirtiendo", "Converting") + " \(so) (\(k + 1)/\(game.libraries.count))")
            let elf = work.appendingPathComponent(so)
            let i = vq_zip_find(zip, entry)
            if i < 0 || vq_zip_extract(zip, i, elf.path, &err, err.count) != 0 {
                throw Problem.failed(VQL("No se pudo extraer ", "Could not extract ") + "\(so): " + String(cString: err))
            }
            let out = dylibs.appendingPathComponent(dylibName(for: so))
            while true {
                let t0 = Date()
                if klepton_ld_run(elf.path, out.path, &err, err.count) != 0 {
                    throw Problem.failed("klepton-ld \(so): " + String(cString: err))
                }
                let variant = chosen ?? variants[0]
                do {
                    try VQSigner.sign(path: out.path, variant: variant, identity: identity)
                } catch {
                    throw Problem.failed("\(so): \(error.localizedDescription)")
                }
                let mb = ((try? fm.attributesOfItem(atPath: out.path)[.size] as? NSNumber)?.int64Value ?? 0) >> 20
                VQLog.log("Preparar: \(so) -> \(out.lastPathComponent) (\(mb) MB, firma \(variant.name), \(String(format: "%.1f", Date().timeIntervalSince(t0))) s)")
                if chosen != nil { break }
                // Is this signature one the system accepts?
                if let h = dlopen(out.path, RTLD_NOW | RTLD_LOCAL) {
                    _ = h // left open: the runtime dlopens the same path later
                    chosen = variant
                    VQSigner.remember(variant)
                    VQLog.log("Preparar: el sistema acepta la firma \(variant.name)")
                    break
                }
                let reason = dlerror().map { String(cString: $0) } ?? "?"
                VQLog.log("Preparar: dlopen rechazó la firma \(variant.name): \(reason)")
                variants.removeFirst()
                if variants.isEmpty || !reason.contains("signature") {
                    throw Problem.failed(VQL("El sistema no acepta la librería firmada: ", "The system refuses the signed library: ") + reason)
                }
            }
            try? fm.removeItem(at: elf)
        }
        try? fm.removeItem(at: work)
        try (want + "\nvariante=\(chosen?.name ?? "?")\n").write(to: stampURL, atomically: true, encoding: .utf8)
        VQLog.log("Preparar: \(game.id) listo en \(Int(Date().timeIntervalSince(started))) s")
        progress(1, VQL("Listo", "Done"))
        return Prepared(tree: tree, dylibs: dylibs)
    }
}
