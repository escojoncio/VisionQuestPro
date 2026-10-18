// VisionQuestPro: a cover picture for each game card, like the Quest library.
//
// Offline only (no store servers). Order: an image the user put in the game's
// folder (a file named cover/banner/poster wins, else the largest image), then
// the art inside the APK (a banner first, else the launcher icon at its highest
// density), cached as QUEST/Cache/<game>/cover-apk.<ext>. No picture: the card
// draws a gradient with the game's name.

import SwiftUI
import UIKit

enum VQCover {
    private static let imageExts: Set<String> = ["png", "jpg", "jpeg", "webp"]

    /// The picture to show for `game`, or nil. Extracts from the APK on first use (cheap: one small entry).
    static func url(for game: VQGame) -> URL? {
        if let own = folderImage(game.folder) { return own }
        guard let cache = VQFolder.shared.cache(for: game.id) else { return nil }
        let fm = FileManager.default
        for ext in imageExts {
            let u = cache.appendingPathComponent("cover-apk.\(ext)")
            if fm.fileExists(atPath: u.path) { return u }
        }
        return extractFromAPK(game.apk, into: cache)
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
        .task(id: game.id) {
            let g = game
            let loaded = await Task.detached(priority: .utility) { () -> UIImage? in
                guard let u = VQCover.url(for: g), let data = try? Data(contentsOf: u) else { return nil }
                return UIImage(data: data)
            }.value
            image = loaded
        }
    }
}
