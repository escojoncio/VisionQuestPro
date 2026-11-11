// VisionQuestPro: a cover picture for each game card, like the Quest library.
//
// The games never go online; only the app fetches the store picture, once, when the setting allows.
// Order: an image the user put in the game's folder (only a file named cover/banner/poster/caratula
// counts), then the store cover cached as QUEST/Cache/<game>/cover-store.<ext>
// (downloaded once by VQCoverStore), then the APK's assets/vr_splash.png when it is ~16:9
// (cover-splash.<ext>), then the art inside the APK (a banner first, else the launcher icon at
// its highest density; cover-apk.<ext>). No picture: the card draws a gradient with the game's name.

import SwiftUI
import UIKit
import ImageIO

extension Notification.Name {
    /// Posted on the main thread with the game id as `object` when a store cover was just saved.
    static let vqCoverUpdated = Notification.Name("vq.coverUpdated")
}

enum VQCover {
    private static let imageExts: Set<String> = ["png", "jpg", "jpeg", "webp"]

    /// The picture to show for `game`, or nil. Extracts from the APK on first use (cheap: one small entry).
    static func url(for game: VQGame, storeCovers: Bool) -> URL? {
        if let own = folderImage(game.folder) {
            logOnce(game.id, "Carátula de \(game.name): fuente folderImage (\(own.lastPathComponent))")
            return own
        }
        guard let cache = VQFolder.shared.cache(for: game.id) else {
            logOnce(game.id, "Carátula de \(game.name): sin carpeta de caché (cache nil); no hay carátula")
            return nil
        }
        if let stored = cached("cover-store", in: cache) {
            logOnce(game.id, "Carátula de \(game.name): fuente cover-store (\(stored.lastPathComponent))")
            return stored
        }
        if storeCovers { VQCoverStore.fetchIfNeeded(game, cache: cache) }
        else { logOnce(game.id, "Carátula de tienda: \(game.name): omitida, ajuste de carátulas de tienda apagado") }
        if let splash = cached("cover-splash", in: cache) {
            logOnce(game.id, "Carátula de \(game.name): fuente cover-splash (\(splash.lastPathComponent))")
            return splash
        }
        let fm = FileManager.default
        let splashNone = cache.appendingPathComponent("cover-splash.none")
        if !fm.fileExists(atPath: splashNone.path) {
            let (splash, definitive) = extractSplash(game.apk, into: cache)
            if let splash {
                logOnce(game.id, "Carátula de \(game.name): fuente cover-splash (\(splash.lastPathComponent))")
                return splash
            }
            // Only a clean "no usable splash" is remembered; transient failures are retried next time.
            if definitive { fm.createFile(atPath: splashNone.path, contents: nil) }
        }
        if let apkArt = cached("cover-apk", in: cache) {
            logOnce(game.id, "Carátula de \(game.name): fuente cover-apk/icono (\(apkArt.lastPathComponent))")
            return apkArt
        }
        let art = extractFromAPK(game.apk, into: cache)
        logOnce(game.id, "Carátula de \(game.name): " + (art.map { "fuente cover-apk/icono (\($0.lastPathComponent), recién extraído)" } ?? "sin carátula (ni icono en el APK)"))
        return art
    }

    private static let logLock = NSLock()
    private static var logged: Set<String> = []

    /// Logs `message` once per game and session (the card asks for the cover several times).
    static func logOnce(_ id: String, _ message: String) {
        logLock.lock()
        let first = logged.insert("\(id)|\(message)").inserted
        logLock.unlock()
        if first { VQLog.log(message) }
    }

    static func dimensions(_ data: Data) -> (w: Int, h: Int)? {
        guard let src = CGImageSourceCreateWithData(data as CFData, nil),
              let p = CGImageSourceCopyPropertiesAtIndex(src, 0, nil) as? [CFString: Any],
              let w = p[kCGImagePropertyPixelWidth] as? Int, let h = p[kCGImagePropertyPixelHeight] as? Int else { return nil }
        return (w, h)
    }

    /// JPEG (quality 0.85) scaled to at most `maxWidth` px wide, or nil when it cannot be done.
    static func downscaledJPEG(_ data: Data, maxWidth: Int) -> Data? {
        guard let src = CGImageSourceCreateWithData(data as CFData, nil) else { return nil }
        let opts: [CFString: Any] = [
            kCGImageSourceCreateThumbnailFromImageAlways: true,
            kCGImageSourceCreateThumbnailWithTransform: true,
            kCGImageSourceThumbnailMaxPixelSize: maxWidth,   // landscape: the width is the longest side
        ]
        guard let img = CGImageSourceCreateThumbnailAtIndex(src, 0, opts as CFDictionary) else { return nil }
        let out = NSMutableData()
        guard let dst = CGImageDestinationCreateWithData(out, "public.jpeg" as CFString, 1, nil) else { return nil }
        CGImageDestinationAddImage(dst, img, [kCGImageDestinationLossyCompressionQuality: 0.85] as CFDictionary)
        return CGImageDestinationFinalize(dst) ? out as Data : nil
    }

    private static func cached(_ stem: String, in cache: URL) -> URL? {
        for ext in imageExts {
            let u = cache.appendingPathComponent("\(stem).\(ext)")
            if FileManager.default.fileExists(atPath: u.path) { return u }
        }
        return nil
    }

    /// True when the picture is roughly 16:9 (1.6 ... 1.95), the shape of a library cover.
    static func isLandscape(_ data: Data) -> Bool {
        guard let src = CGImageSourceCreateWithData(data as CFData, nil),
              let p = CGImageSourceCopyPropertiesAtIndex(src, 0, nil) as? [CFString: Any],
              let w = p[kCGImagePropertyPixelWidth] as? Double, let h = p[kCGImagePropertyPixelHeight] as? Double,
              h > 0 else { return false }
        return w / h >= 1.6 && w / h <= 1.95
    }

    // Some APKs ship a 16:9 splash (assets/vr_splash.png): better than a tiny icon when the store cover is missing.
    // `definitive` is true only when the APK opened fine and the splash is missing or not ~16:9.
    private static func extractSplash(_ apk: URL, into cache: URL) -> (url: URL?, definitive: Bool) {
        var err = [CChar](repeating: 0, count: 256)
        guard let zip = vq_zip_open(apk.path, &err, err.count) else { return (nil, false) }
        defer { vq_zip_close(zip) }
        let i = vq_zip_find(zip, "assets/vr_splash.png")
        guard i >= 0 else { return (nil, true) }
        var len = 0
        guard let buf = vq_zip_read(zip, i, 16 << 20, &len, &err, err.count) else { return (nil, false) }
        defer { free(buf) }
        let data = Data(bytes: buf, count: len)
        guard isLandscape(data) else { return (nil, true) }
        let out = cache.appendingPathComponent("cover-splash.png")
        do { try data.write(to: out, options: .atomic) } catch {
            VQLog.log("Carátula: no se pudo guardar la imagen de arranque del APK: \(error.localizedDescription)")
            return (nil, false)
        }
        return (out, true)
    }

    private static func folderImage(_ folder: URL) -> URL? {
        let items = (try? FileManager.default.contentsOfDirectory(at: folder, includingPropertiesForKeys: [.fileSizeKey])) ?? []
        let images = items.filter { imageExts.contains($0.pathExtension.lowercased()) }
        return images.first { n in
            let s = n.deletingPathExtension().lastPathComponent.lowercased()
            return ["cover", "banner", "poster", "caratula"].contains { s.contains($0) }
        }
    }

    // APK art: res/<dir>/<name>.<raster>. Banners (wide, made for TV/launchers) beat icons; among
    // the same kind the biggest file is the highest density. Adaptive-icon XML is skipped.
    private static func extractFromAPK(_ apk: URL, into cache: URL) -> URL? {
        var err = [CChar](repeating: 0, count: 256)
        guard let zip = vq_zip_open(apk.path, &err, err.count) else { return nil }
        defer { vq_zip_close(zip) }
        var best: (index: Int32, rank: Int, size: UInt64, ext: String)?
        for i in 0..<vq_zip_count(zip) {
            guard let c = vq_zip_name(zip, i) else { continue }
            let name = String(cString: c)
            guard name.hasPrefix("res/") else { continue }
            let ext = (name as NSString).pathExtension.lowercased()
            guard imageExts.contains(ext) else { continue }
            let base = ((name as NSString).lastPathComponent as NSString).deletingPathExtension.lowercased()
            let rank: Int
            switch base {
            case "app_banner", "banner", "tv_banner": rank = 3
            case "app_icon", "ic_launcher", "icon": rank = 2
            case "ic_launcher_round", "ic_launcher_foreground", "app_icon_foreground": rank = 1
            default: continue
            }
            let size = vq_zip_size(zip, i)
            if let b = best, (b.rank, b.size) >= (rank, size) { continue }
            best = (Int32(i), rank, size, ext)
        }
        guard let b = best else { return nil }
        let out = cache.appendingPathComponent("cover-apk.\(b.ext)")
        guard vq_zip_extract(zip, b.index, out.path, &err, err.count) == 0 else {
            VQLog.log("Carátula: no se pudo extraer del APK: \(String(cString: err))")
            return nil
        }
        return out
    }
}

/// One-time download of the store cover, by package name. Only the app goes online (never the games).
///
/// Source: OculusDB, a public community index of the Meta Quest store (no account, no token):
///   GET https://oculusdb.rui2015.me/api/v1/search/<package name>
/// answers JSON with the matching apps (objects with `packageName`, `id`, `imageLink` = "/cdn/images/<id>"
/// (relative to the same host) and `display_long_description_unprocessed`, a markdown text whose
/// images look like `![{"height":720,"type":"image","width":1280}](https://scontent-….oculuscdn.com/…png?…)`).
/// Candidates, in order: the detail record GET /api/v1/id/<id> (landscape/hero/cover/banner/screenshot
/// image fields, or {width,height,uri} objects that are ~16:9; its fields are guessed generically because
/// the schema is not documented), its markdown images, then the search's `imageLink` and 1280x720 images;
/// the first that really decodes as ~16:9 is kept. Signed oculuscdn URLs expire: 401/403 on them is an
/// "expired candidate", not a network failure. With no 16:9 picture, the best square one (>= 256 px) is
/// kept as the store cover (VQCoverView centres it over a blur). Result file: cover-store.<ext>;
/// marker cover-store.none = "<unix time> <absent|soon|net> v4 [<consecutive net failures>]"
/// (absent: the store does not know the package or has no picture, retry after 3 days; soon: first miss, retry after 12 h
/// (a second miss becomes absent); net: retry after 2 min, then 10, 30 and 60 min (growing with the failure count; a net
/// marker without a count counts as 1; if the 60 min retry fails too, the marker becomes soon = 12 h); a marker
/// without the current version tag was written by older code and counts as missing).
enum VQCoverStore {
    private static let host = "https://oculusdb.rui2015.me"
    // A net failure is transient (the OculusDB search can take >15 s): short, growing waits instead of a fixed 1 h.
    private static let retryNetDelays: [TimeInterval] = [120, 600, 1800, 3600]
    private static let retryAbsentAfter: TimeInterval = 3 * 24 * 3600
    private static let retryAfterSoon: TimeInterval = 12 * 3600
    private static let markerVersion = "v4"
    private static let maxBytes = 16 << 20
    private static let maxWidth = 1920
    private static let lock = NSLock()
    private static var running: Set<String> = []

    static func fetchIfNeeded(_ game: VQGame, cache: URL) {
        let package = game.package.trimmingCharacters(in: .whitespaces)
        guard package.contains("."), package != VQConformance.openXRPackage else {
            VQCover.logOnce(game.id, "Carátula de tienda: \(game.name): omitida, paquete inválido (\"\(package)\")")
            return
        }
        let marker = cache.appendingPathComponent("cover-store.none")
        if let text = try? String(contentsOf: marker, encoding: .utf8) {
            let parts = text.split(separator: " ")
            if parts.count >= 3, parts[2] == markerVersion, let t = TimeInterval(parts[0]) {
                let age = Date().timeIntervalSince1970 - t
                let failures = parts.count >= 4 ? (Int(parts[3]) ?? 1) : 1
                let limit = parts[1] == "absent" ? retryAbsentAfter : (parts[1] == "soon" ? retryAfterSoon
                            : retryNetDelays[min(max(failures, 1), retryNetDelays.count) - 1])
                if age < limit {
                    VQCover.logOnce(game.id, "Carátula de tienda: \(package): omitida, marcador \(parts[1]) de hace \(Int(age / 60)) min (reintento a los \(Int(limit / 60)) min)")
                    return
                }
                VQCover.logOnce(game.id, "Carátula de tienda: \(package): marcador \(parts[1]) caducado (\(Int(age / 60)) min); se reintenta")
            } else {
                VQCover.logOnce(game.id, "Carátula de tienda: \(package): marcador de una versión anterior ignorado")
            }
        }
        guard setRunning(game.id, true) else {
            VQCover.logOnce(game.id, "Carátula de tienda: \(package): omitida, ya hay una descarga en curso")
            return
        }
        VQLog.log("Carátula de tienda: \(package): inicio de la descarga")
        Task.detached(priority: .utility) {
            await run(id: game.id, package: package, name: game.name, cache: cache, marker: marker)
            _ = setRunning(game.id, false)
        }
    }

    /// Marks a game as downloading (true) or done (false); false is returned when it was already running.
    private static func setRunning(_ id: String, _ on: Bool) -> Bool {
        lock.lock(); defer { lock.unlock() }
        return on ? running.insert(id).inserted : (running.remove(id) != nil)
    }

    private static func run(id: String, package: String, name: String, cache: URL, marker: URL) async {
        let cfg = URLSessionConfiguration.ephemeral
        cfg.timeoutIntervalForRequest = 30    // 15 s timed out on OculusDB's search for Deadpool / vulkancts
        cfg.timeoutIntervalForResource = 120
        let session = URLSession(configuration: cfg)
        let started = Date()
        let oldParts = ((try? String(contentsOf: marker, encoding: .utf8)) ?? "").split(separator: " ")
        let wasSoon = oldParts.count > 1 && oldParts[1] == "soon"
        // Consecutive net failures so far (a net marker without a count counts as 1).
        let netFailures = oldParts.count > 1 && oldParts[1] == "net" ? (oldParts.count >= 4 ? (Int(oldParts[3]) ?? 1) : 1) : 0
        func mark(_ why: String) {
            // After the last growing wait (retryNetDelays) fails too, stop the hourly retries: back off 12 h ("soon").
            let kind = why == "net" && netFailures + 1 > retryNetDelays.count ? "soon" : why
            let count = kind == "net" ? " \(netFailures + 1)" : ""
            try? "\(Int(Date().timeIntervalSince1970)) \(kind) \(markerVersion)\(count)".write(to: marker, atomically: true, encoding: .utf8)
        }
        do {
            guard let encoded = package.addingPathComponentEncoded,
                  let search = URL(string: "\(host)/api/v1/search/\(encoded)") else {
                VQLog.log("Carátula de tienda: \(package): nombre de paquete no válido para la búsqueda")
                mark("absent"); return
            }
            var found: [String: Any]?
            var route = "búsqueda por paquete"
            do {
                let (body, resp) = try await session.data(from: search)
                guard (resp as? HTTPURLResponse)?.statusCode == 200,
                      let json = try? JSONSerialization.jsonObject(with: body) else {
                    VQLog.log("Carátula de tienda: \(package): respuesta inesperada de la búsqueda")
                    throw URLError(.badServerResponse)
                }
                let list = searchResults(json)
                VQLog.log("Carátula de tienda: \(package): búsqueda por paquete: \(list.count) resultado(s)" + describe(list))
                found = list.first(where: { ($0["packageName"] as? String)?.lowercased() == package.lowercased() })
            } catch let e as URLError {
                // The package search failed (timeout/network/bad reply): try once by the exact visible name.
                // Only an identical normalized name counts; anything less clear is no cover (marker net, retried later).
                VQLog.log("Carátula de tienda: \(package): búsqueda por paquete fallida tras \(Int(Date().timeIntervalSince(started))) s (\(e.localizedDescription)); se prueba por nombre exacto \"\(name)\"")
                found = try await searchByName(session, name: name, package: package, exactOnly: true)
                route = "nombre exacto (respaldo tras fallo de red)"
                if found == nil { throw e }
            }
            if found == nil, route == "búsqueda por paquete" {
                // The package search worked but lists no such package; a network failure here propagates (marker net).
                found = try await searchByName(session, name: name, package: package, exactOnly: false)
                route = "búsqueda por nombre"
            }
            if found != nil { VQLog.log("Carátula de tienda: \(package): ficha elegida por \(route)") }
            guard let app = found else {
                VQLog.log("Carátula de tienda: \(package): la tienda no tiene este paquete (ni por nombre \"\(name)\"); marcador " + (wasSoon ? "absent (2.ª vez)" : "soon (12 h)"))
                mark(wasSoon ? "absent" : "soon"); return
            }
            var netFailure = false
            var square: (data: Data, px: Int)?
            var detail: [String: Any]?
            if let appId = (app["id"] as? String) ?? (app["id"] as? NSNumber)?.stringValue,
               let encId = appId.addingPathComponentEncoded, let durl = URL(string: "\(host)/api/v1/id/\(encId)") {
                do {
                    let (d, r) = try await session.data(from: durl)
                    let code = (r as? HTTPURLResponse)?.statusCode ?? -1
                    detail = (try? JSONSerialization.jsonObject(with: d)) as? [String: Any]
                    VQLog.log("Carátula de tienda: \(package): ficha /api/v1/id/\(appId): HTTP \(code), \(d.count) bytes, " + (detail.map { "campos: " + $0.keys.sorted().joined(separator: ",") } ?? "no es un objeto JSON"))
                    if let detail {
                        var urls: [String] = []
                        urlFields(detail, path: "", into: &urls)
                        VQLog.log("Carátula de tienda: \(package): ficha: \(urls.count) campo(s) con URL o /cdn/")
                        for u in urls.prefix(80) { VQLog.log("Carátula de tienda: \(package): ficha URL \(u)") }
                    }
                    if code != 200 { detail = nil }
                } catch {
                    VQLog.log("Carátula de tienda: \(package): ficha /api/v1/id/\(appId): error de red: \(error.localizedDescription)")
                }
            } else {
                VQLog.log("Carátula de tienda: \(package): la búsqueda no trae id; sin consulta de la ficha")
            }
            let list2 = candidates(app, detail: detail)
            VQLog.log("Carátula de tienda: \(package): \(list2.count) candidato(s)")
            for candidate in list2 {
                let short = shortURL(candidate)
                var data: Data
                do {
                    let (d, r): (Data, URLResponse)
                    do { (d, r) = try await session.data(from: candidate) }   // follows redirects
                    catch let e as URLError where (e.code == .cannotFindHost || e.code == .dnsLookupFailed) && (candidate.host ?? "").hasSuffix(".oculuscdn.com")
                                                  && candidate.host != "scontent.oculuscdn.com" {
                        // The store page quotes a regional edge host (scontent-fra5-2…) that may no longer
                        // resolve; the same signed path is served by the generic host.
                        var c = URLComponents(url: candidate, resolvingAgainstBaseURL: false)
                        c?.host = "scontent.oculuscdn.com"
                        guard let alt = c?.url else { throw e }
                        VQLog.log("Carátula de tienda: \(short) no resuelve; se prueba scontent.oculuscdn.com")
                        (d, r) = try await session.data(from: alt)
                    }
                    let http = r as? HTTPURLResponse
                    let type = http?.value(forHTTPHeaderField: "Content-Type") ?? "?"
                    let size = VQCover.dimensions(d).map { "\($0.w)x\($0.h)" } ?? "sin dimensiones"
                    func verdict(_ why: String) { VQLog.log("Carátula de tienda: candidato \(short): HTTP \(http?.statusCode ?? -1), \(d.count) bytes, \(size), \(type): descartado, \(why)") }
                    if let code = http?.statusCode, code == 401 || code == 403, isSigned(candidate) {
                        verdict("URL firmada caducada"); continue   // not a network failure: no 7-day retry
                    }
                    guard http?.statusCode == 200 else { netFailure = true; verdict("código HTTP"); continue }
                    let t = type.lowercased()
                    if t.contains("html") || t.contains("json") || t.hasPrefix("text/") { verdict("Content-Type no es imagen"); continue }
                    guard d.count <= maxBytes else { verdict("más de 16 MB"); continue }
                    guard fileExtension(d) != nil else { verdict("formato no reconocido"); continue }
                    guard VQCover.isLandscape(d) else {
                        if let dim = VQCover.dimensions(d), min(dim.w, dim.h) >= 256,
                           Double(max(dim.w, dim.h)) / Double(min(dim.w, dim.h)) <= 1.25,
                           dim.w > (square?.px ?? 0) { square = (d, dim.w) }
                        verdict("no es ~16:9" + (square?.data == d ? " (queda como carátula cuadrada de reserva)" : "")); continue
                    }
                    data = d
                } catch {
                    VQLog.log("Carátula de tienda: candidato \(short): error de red: \(error.localizedDescription)")
                    netFailure = true; continue
                }
                var ext = fileExtension(data) ?? "jpg"
                if let w = VQCover.dimensions(data)?.w, w > maxWidth || data.count > 8 << 20,
                   let small = VQCover.downscaledJPEG(data, maxWidth: maxWidth) {
                    VQLog.log("Carátula de tienda: \(package): recomprimida \(data.count / 1024) KB -> \(small.count / 1024) KB (JPEG 0.85, máx. \(maxWidth) px)")
                    data = small; ext = "jpg"
                }
                // A stale file of another extension would shadow the new one in `cached`.
                for e in ["png", "jpg", "jpeg", "webp"] { try? FileManager.default.removeItem(at: cache.appendingPathComponent("cover-store.\(e)")) }
                try data.write(to: cache.appendingPathComponent("cover-store.\(ext)"), options: .atomic)
                try? FileManager.default.removeItem(at: marker)
                VQLog.log("Carátula de tienda: \(package): guardada (\(data.count / 1024) KB, \(ext))")
                await MainActor.run { NotificationCenter.default.post(name: .vqCoverUpdated, object: id) }
                return
            }
            if let sq = square {
                var data = sq.data
                var ext = fileExtension(data) ?? "jpg"
                if sq.px > maxWidth || data.count > 8 << 20, let small = VQCover.downscaledJPEG(data, maxWidth: maxWidth) { data = small; ext = "jpg" }
                for e in ["png", "jpg", "jpeg", "webp"] { try? FileManager.default.removeItem(at: cache.appendingPathComponent("cover-store.\(e)")) }
                try data.write(to: cache.appendingPathComponent("cover-store.\(ext)"), options: .atomic)
                try? FileManager.default.removeItem(at: marker)
                VQLog.log("Carátula de tienda: \(package): sin imagen apaisada vigente; guardada la cuadrada (\(data.count / 1024) KB, \(ext))")
                await MainActor.run { NotificationCenter.default.post(name: .vqCoverUpdated, object: id) }
                return
            }
            if netFailure {
                VQLog.log("Carátula de tienda: \(package): fallo al descargar alguna imagen; se reintentará")
                mark("net")
            } else {
                VQLog.log("Carátula de tienda: \(package): la ficha no trae imagen apaisada")
                mark("absent")
            }
        } catch {
            VQLog.log("Carátula de tienda: \(package): fallo de red o de escritura tras \(Int(Date().timeIntervalSince(started))) s: \(error.localizedDescription)")
            mark("net")
        }
    }

    private static func searchResults(_ json: Any) -> [[String: Any]] {
        (json as? [[String: Any]]) ?? ((json as? [String: Any])?["applications"] as? [[String: Any]]) ?? []
    }

    private static let nameKeys = ["displayName", "display_name", "name", "title"]
    private static func resultName(_ r: [String: Any]) -> String? { nameKeys.lazy.compactMap { r[$0] as? String }.first }

    /// First 5 results as " [pkg | name; ...]" for the log.
    private static func describe(_ list: [[String: Any]]) -> String {
        list.isEmpty ? "" : ": " + list.prefix(5).map { "\($0["packageName"] as? String ?? "?") | \(resultName($0) ?? "?")" }.joined(separator: "; ")
    }

    /// Every string value at any depth that is an http URL or starts with /cdn/, as "key.path=value".
    private static func urlFields(_ node: Any, path: String, into out: inout [String]) {
        if let d = node as? [String: Any] {
            for k in d.keys.sorted() { urlFields(d[k]!, path: path.isEmpty ? k : "\(path).\(k)", into: &out) }
        } else if let a = node as? [Any] {
            for (i, v) in a.enumerated() { urlFields(v, path: "\(path)[\(i)]", into: &out) }
        } else if let s = node as? String, s.hasPrefix("http") || s.hasPrefix("/cdn/") {
            out.append("\(path)=\(s.count > 160 ? String(s.prefix(160)) + "…" : s)")
        }
    }

    /// Lowercase, no accents, letters/digits only, single spaces; apostrophes vanish ("Marvel's" = "Marvels").
    private static func normalize(_ s: String) -> String {
        let folded = s.folding(options: [.diacriticInsensitive, .caseInsensitive], locale: nil).lowercased()
            .replacingOccurrences(of: "'", with: "").replacingOccurrences(of: "\u{2019}", with: "")
        return folded.map { $0.isLetter || $0.isNumber ? String($0) : " " }.joined()
            .split(separator: " ").joined(separator: " ")
    }

    /// Fallback when the package is not in the search: search by the game's visible name (and the
    /// name cut at ":", " x ", " - "), keep the best name match (+ same organisation: first two package parts).
    /// `exactOnly`: only the full visible name is searched and only an identical normalized name from the same
    /// organisation (package prefix) is accepted (score 130). A network failure or bad reply throws (the caller marks net).
    private static func searchByName(_ session: URLSession, name: String, package: String, exactOnly: Bool) async throws -> [String: Any]? {
        var variants: [String] = []
        for sep in [":", " x ", " - "] {
            if let r = name.range(of: sep) { variants.append(String(name[..<r.lowerBound])) }
        }
        variants.insert(name, at: 0)
        var targets: [String] = []
        for v in variants {
            let n = normalize(v)
            if n.count >= 3, !targets.contains(n) { targets.append(n) }
        }
        if exactOnly { targets = Array(targets.prefix(1)) }
        let org = package.lowercased().split(separator: ".").prefix(2).joined(separator: ".")
        var best: (score: Int, app: [String: Any])?
        for t in targets {
            guard let enc = t.addingPathComponentEncoded, let url = URL(string: "\(host)/api/v1/search/\(enc)") else { continue }
            let body: Data, resp: URLResponse
            do { (body, resp) = try await session.data(from: url) } catch {
                VQLog.log("Carátula de tienda: \(package): búsqueda por nombre \"\(t)\": error de red: \(error.localizedDescription)")
                throw error
            }
            guard (resp as? HTTPURLResponse)?.statusCode == 200,
                  let json = try? JSONSerialization.jsonObject(with: body) else {
                VQLog.log("Carátula de tienda: \(package): búsqueda por nombre \"\(t)\": sin respuesta válida")
                throw URLError(.badServerResponse)
            }
            let list = searchResults(json)
            VQLog.log("Carátula de tienda: \(package): búsqueda por nombre \"\(t)\": \(list.count) resultado(s)" + describe(list))
            for r in list {
                guard let rn = resultName(r).map(normalize), !rn.isEmpty else { continue }
                var score = 0
                for (i, tt) in targets.enumerated() {
                    if rn == tt { score = max(score, i == 0 ? 100 : 90) }
                    else if " \(rn) ".contains(" \(tt) ") || (rn.count >= 4 && " \(tt) ".contains(" \(rn) ")) { score = max(score, 50) }
                }
                guard score > 0 else { continue }
                if !org.isEmpty, (r["packageName"] as? String)?.lowercased().hasPrefix(org + ".") == true { score += 30 }
                if score > (best?.score ?? 0) { best = (score, r) }
            }
        }
        if let best, best.score >= (exactOnly ? 130 : 50) {
            VQLog.log("Carátula de tienda: \(package): coincidencia por nombre: \(best.app["packageName"] as? String ?? "?") | \(resultName(best.app) ?? "?") (puntuación \(best.score))")
            return best.app
        }
        return nil
    }

    /// Signed CDN links (expire): the Meta CDN host or a signature/expiry query item.
    private static func isSigned(_ u: URL) -> Bool {
        let q = u.query ?? ""
        return (u.host ?? "").contains("oculuscdn") || q.contains("oh=") || q.contains("oe=") || q.contains("_nc_")
    }

    /// Image URLs found anywhere in the detail JSON, best first: ~16:9 {width,height,uri} objects and
    /// landscape/hero/cover/banner/wide keys, then screenshot/image/background keys. Icons and
    /// square/portrait keys are left out (the search's imageLink covers the square fallback).
    private static func detailImages(_ node: Any, key: String = "", into out: inout [(rank: Int, w: Int, u: URL)]) {
        func url(_ s: String) -> URL? {
            guard s.hasPrefix("http") || s.hasPrefix("/cdn/") else { return nil }
            return URL(string: s, relativeTo: URL(string: host))?.absoluteURL
        }
        if let d = node as? [String: Any] {
            if let w = (d["width"] as? NSNumber)?.intValue, let h = (d["height"] as? NSNumber)?.intValue, h > 0,
               Double(w) / Double(h) >= 1.6, Double(w) / Double(h) <= 1.95,
               let s = ["uri", "url", "src", "link", "image"].lazy.compactMap({ d[$0] as? String }).first, let u = url(s) {
                out.append((0, w, u))
            }
            for (k, v) in d { detailImages(v, key: k.lowercased(), into: &out) }
        } else if let a = node as? [Any] {
            for v in a { detailImages(v, key: key, into: &out) }
        } else if let s = node as? String, let u = url(s) {
            if ["icon", "square", "portrait", "logo", "avatar", "trailer", "video"].contains(where: { key.contains($0) }) { return }
            if ["landscape", "hero", "cover", "banner", "wide"].contains(where: { key.contains($0) }) { out.append((0, 0, u)) }
            else if ["screenshot", "image", "img", "poster", "background", "thumb", "promo"].contains(where: { key.contains($0) }) { out.append((1, 0, u)) }
        }
    }

    private static func markdownImages(_ text: String) -> [URL] {
        let re = try? NSRegularExpression(pattern: #"!\[(\{[^\]]*\})\]\((https?://[^)\s]+)\)"#)
        var wide: [(w: Int, u: URL)] = []
        for m in re?.matches(in: text, range: NSRange(text.startIndex..., in: text)) ?? [] {
            guard let a = Range(m.range(at: 1), in: text), let b = Range(m.range(at: 2), in: text),
                  let meta = try? JSONSerialization.jsonObject(with: Data(text[a].utf8)) as? [String: Any],
                  meta["type"] as? String == "image", let w = meta["width"] as? Int, let h = meta["height"] as? Int, h > 0,
                  Double(w) / Double(h) >= 1.6, Double(w) / Double(h) <= 1.95,
                  let u = URL(string: String(text[b])) else { continue }
            wide.append((w, u))
        }
        return wide.sorted { $0.w > $1.w }.map(\.u)
    }

    private static func candidates(_ app: [String: Any], detail: [String: Any]?) -> [URL] {
        var out: [URL] = []
        if let detail {
            var found: [(rank: Int, w: Int, u: URL)] = []
            detailImages(detail, into: &found)
            out += found.enumerated().sorted { ($0.element.rank, -$0.element.w, $0.offset) < ($1.element.rank, -$1.element.w, $1.offset) }.map(\.element.u)
            out += markdownImages(detail["display_long_description_unprocessed"] as? String ?? "")
        }
        if let link = app["imageLink"] as? String,
           let u = URL(string: link, relativeTo: URL(string: host))?.absoluteURL { out.append(u) }
        out += markdownImages(app["display_long_description_unprocessed"] as? String ?? "")
        var seen = Set<URL>()
        return Array(out.filter { seen.insert($0).inserted }.prefix(14))
    }

    /// host + path (no query), at most 70 characters.
    private static func shortURL(_ u: URL) -> String {
        let s = (u.host ?? "") + u.path
        return s.count > 70 ? String(s.prefix(70)) + "…" : s
    }

    private static func fileExtension(_ d: Data) -> String? {
        let b = [UInt8](d.prefix(12))
        if b.count >= 4, b[0] == 0x89, b[1] == 0x50 { return "png" }
        if b.count >= 3, b[0] == 0xFF, b[1] == 0xD8 { return "jpg" }
        if b.count >= 12, b[0] == 0x52, b[1] == 0x49, b[8] == 0x57, b[9] == 0x45 { return "webp" }
        return nil
    }
}

private extension String {
    /// Percent-encoded for use as one URL path segment, or nil.
    var addingPathComponentEncoded: String? {
        var allowed = CharacterSet.urlPathAllowed
        allowed.remove("/")
        return addingPercentEncoding(withAllowedCharacters: allowed)
    }
}

/// 16:9 cover at the top of a game card. Wide art fills it; a square icon sits centred over a
/// blurred, enlarged copy of itself (the Quest library look for apps without a banner).
struct VQCoverView: View {
    let game: VQGame
    @State private var image: UIImage?

    var body: some View {
        // The 16:9 box sets the size; the picture is an overlay so a filled image cannot grow the card.
        Color.clear.aspectRatio(16 / 9, contentMode: .fit).overlay { ZStack {
            if let image {
                if image.size.width >= image.size.height * 1.3 {
                    Image(uiImage: image).resizable().scaledToFill()
                } else {
                    Image(uiImage: image).resizable().scaledToFill().blur(radius: 30).opacity(0.7)
                    Image(uiImage: image).resizable().scaledToFit().padding(24)
                        .shadow(radius: 12)
                }
            } else {
                LinearGradient(colors: [.indigo, .purple.opacity(0.7)], startPoint: .topLeading, endPoint: .bottomTrailing)
                Text(game.name).font(.title.bold()).multilineTextAlignment(.center).padding()
                    .foregroundStyle(.white.opacity(0.9))
            }
        } }
        .frame(maxWidth: .infinity)
        .clipShape(RoundedRectangle(cornerRadius: 16, style: .continuous))
        // clipShape clips the drawing, not the hit-testing: a filled picture overflows the 16:9 box and
        // would take the taps meant for the buttons of the neighbouring card. Nothing here is tappable.
        .allowsHitTesting(false)
        .task(id: game.id) { await load() }
        .onReceive(NotificationCenter.default.publisher(for: .vqCoverUpdated)) { n in
            if n.object as? String == game.id { Task { await load() } }
        }
    }

    @MainActor private func load() async {
        let g = game
        let store = VQSettings.shared.storeCovers   // read on the main thread
        let result = await Task.detached(priority: .utility) { () -> (image: UIImage?, stale: Bool) in
            guard let u = VQCover.url(for: g, storeCovers: store), let data = try? Data(contentsOf: u) else { return (nil, false) }
            // A store cover may have been saved while this decoded: do not overwrite it with an older pick.
            if VQCover.url(for: g, storeCovers: store) != u { return (nil, true) }
            return (UIImage(data: data), false)
        }.value
        if !result.stale { image = result.image }
    }
}
