// VisionQuestPro: a cover picture for each game card, like the Quest library.
//
// The games never go online; only the app fetches the store picture, once, when the setting allows.
// Order: an image the user put in the game's folder (a file named cover/banner/poster wins, else
// the largest image), then the store cover cached as QUEST/Cache/<game>/cover-store.<ext>
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
        if let own = folderImage(game.folder) { return own }
        guard let cache = VQFolder.shared.cache(for: game.id) else { return nil }
        if let stored = cached("cover-store", in: cache) { return stored }
        if storeCovers { VQCoverStore.fetchIfNeeded(game, cache: cache) }
        if let splash = cached("cover-splash", in: cache) { return splash }
        let fm = FileManager.default
        let splashNone = cache.appendingPathComponent("cover-splash.none")
        if !fm.fileExists(atPath: splashNone.path) {
            let (splash, definitive) = extractSplash(game.apk, into: cache)
            if let splash { return splash }
            // Only a clean "no usable splash" is remembered; transient failures are retried next time.
            if definitive { fm.createFile(atPath: splashNone.path, contents: nil) }
        }
        if let apkArt = cached("cover-apk", in: cache) { return apkArt }
        return extractFromAPK(game.apk, into: cache)
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
        let named = images.first { n in
            let s = n.deletingPathExtension().lastPathComponent.lowercased()
            return s.contains("cover") || s.contains("banner") || s.contains("poster")
        }
        if let named { return named }
        return images.max { fileSize($0) < fileSize($1) }
    }

    private static func fileSize(_ u: URL) -> Int {
        (try? u.resourceValues(forKeys: [.fileSizeKey]).fileSize) ?? 0
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
/// Candidates, in order: `imageLink`, then those 1280x720 store images; the first that really decodes as
/// ~16:9 is kept. Result file: cover-store.<ext>; marker cover-store.none = "<unix time> <absent|net>"
/// (absent: the store does not know the package, never retried; net: retry after 7 days).
enum VQCoverStore {
    private static let host = "https://oculusdb.rui2015.me"
    private static let retryAfter: TimeInterval = 7 * 24 * 3600
    private static let lock = NSLock()
    private static var running: Set<String> = []

    static func fetchIfNeeded(_ game: VQGame, cache: URL) {
        let package = game.package.trimmingCharacters(in: .whitespaces)
        guard package.contains("."), package != VQConformance.openXRPackage else { return }
        let marker = cache.appendingPathComponent("cover-store.none")
        if let text = try? String(contentsOf: marker, encoding: .utf8) {
            let parts = text.split(separator: " ")
            if parts.count >= 2, parts[1] == "absent" { return }
            if let t = parts.first.flatMap({ TimeInterval($0) }), Date().timeIntervalSince1970 - t < retryAfter { return }
        }
        guard setRunning(game.id, true) else { return }
        Task.detached(priority: .utility) {
            await run(id: game.id, package: package, cache: cache, marker: marker)
            _ = setRunning(game.id, false)
        }
    }

    /// Marks a game as downloading (true) or done (false); false is returned when it was already running.
    private static func setRunning(_ id: String, _ on: Bool) -> Bool {
        lock.lock(); defer { lock.unlock() }
        return on ? running.insert(id).inserted : (running.remove(id) != nil)
    }

    private static func run(id: String, package: String, cache: URL, marker: URL) async {
        let cfg = URLSessionConfiguration.ephemeral
        cfg.timeoutIntervalForRequest = 15
        cfg.timeoutIntervalForResource = 40
        let session = URLSession(configuration: cfg)
        func mark(_ why: String) {
            try? "\(Int(Date().timeIntervalSince1970)) \(why)".write(to: marker, atomically: true, encoding: .utf8)
        }
        do {
            guard let encoded = package.addingPathComponentEncoded,
                  let search = URL(string: "\(host)/api/v1/search/\(encoded)") else {
                VQLog.log("Carátula de tienda: \(package): nombre de paquete no válido para la búsqueda")
                mark("absent"); return
            }
            let (body, resp) = try await session.data(from: search)
            guard (resp as? HTTPURLResponse)?.statusCode == 200,
                  let json = try? JSONSerialization.jsonObject(with: body) else {
                VQLog.log("Carátula de tienda: \(package): respuesta inesperada de la búsqueda")
                mark("net"); return
            }
            let list = (json as? [[String: Any]]) ?? ((json as? [String: Any])?["applications"] as? [[String: Any]]) ?? []
            guard let app = list.first(where: { ($0["packageName"] as? String)?.lowercased() == package.lowercased() }) else {
                VQLog.log("Carátula de tienda: \(package): la tienda no tiene este paquete")
                mark("absent"); return
            }
            var netFailure = false
            for candidate in candidates(app) {
                let data: Data
                do {
                    let (d, r) = try await session.data(from: candidate)
                    guard (r as? HTTPURLResponse)?.statusCode == 200 else { netFailure = true; continue }
                    data = d
                } catch { netFailure = true; continue }
                guard data.count <= 8 << 20, let ext = fileExtension(data), VQCover.isLandscape(data) else { continue }
                try data.write(to: cache.appendingPathComponent("cover-store.\(ext)"), options: .atomic)
                try? FileManager.default.removeItem(at: marker)
                VQLog.log("Carátula de tienda: \(package): guardada (\(data.count / 1024) KB)")
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
            VQLog.log("Carátula de tienda: \(package): fallo de red o de escritura: \(error.localizedDescription)")
            mark("net")
        }
    }

    private static func candidates(_ app: [String: Any]) -> [URL] {
        var out: [URL] = []
        if let link = app["imageLink"] as? String,
           let u = URL(string: link, relativeTo: URL(string: host))?.absoluteURL { out.append(u) }
        let text = app["display_long_description_unprocessed"] as? String ?? ""
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
        return out + wide.sorted { $0.w > $1.w }.map(\.u)
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
