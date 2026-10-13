// VisionQuestPro: the games in QUEST/Juegos and what each one is.
//
// One folder per game: its .apk (the largest one, if there are several) and its data beside it
// (an .obb or a folder of loose files). The card's facts come from the APK itself, read in place
// (vq_apk.c: its ZIP directory and its binary AndroidManifest.xml):
//   engine      Unity (libunity.so / libil2cpp.so), Unreal (libUE4.so / libUnreal.so), other
//   graphics    Vulkan if the manifest asks for android.hardware.vulkan.version or Unity's
//               boot.config says gfx-enable-vulkan / vulkan first; else OpenGL ES
//   Quest 3     com.oculus.supportedDevices lists quest3
//   compatible  arm64-v8a libraries, Unity (Klepton's synthesized target), Vulkan (no GLES in v1)

import Foundation

struct VQGame: Identifiable, Equatable {
    let id: String          // the folder's name
    let folder: URL
    let apk: URL
    var name: String
    var package: String
    var version: String
    var engine: String
    var graphics: String
    var quest3: Bool
    var libraries: [String] // lib/arm64-v8a/*.so entries
    var obbs: [URL]
    var problems: [String]  // why it cannot run; empty when it can
    var manifestText: String
    var contentNames: [String] = []  // lowercased APK entries + data file names (language detection)

    var compatible: Bool { problems.isEmpty }

    /// Klepton guest driver for this engine (KL_GUEST_KIND) and its entry library (KL_GUEST_ENTRY).
    var guestKind: String {
        switch engine {
        case "Unity": return "unity"
        case "Unreal": return "ue4"
        default: return "native"
        }
    }
    var entryLib: String {
        let libs = Set(libraries.map { ($0 as NSString).lastPathComponent })
        switch guestKind {
        case "unity": return "libmain"
        case "ue4": return libs.contains("libUnreal.so") ? "libUnreal" : "libUE4"
        default:
            // NativeActivity: the manifest's android.app.lib_name (default "main").
            let n = nativeLibName.isEmpty ? "main" : nativeLibName
            return n.hasPrefix("lib") && libs.contains("\(n).so") ? n : "lib\(n)"
        }
    }
    var nativeLibName: String = ""
    var nativeFuncName: String = ""   // android.app.func_name (entry other than ANativeActivity_onCreate)

    /// Stable key for per-game settings and runtime detections (VQ_GAME_KEY): the Android
    /// package (same game, any folder name), else the folder name.
    var aaKey: String {
        let p = package.trimmingCharacters(in: .whitespaces)
        let raw = (p.isEmpty || p == "?") ? id : p
        return String(raw.map { $0.isLetter || $0.isNumber || $0 == "." || $0 == "_" || $0 == "-" ? $0 : "_" })
    }
}

enum VQGames {
    static func scan() -> [VQGame] {
        guard let root = VQFolder.shared.games else { return [] }
        let fm = FileManager.default
        let folders = ((try? fm.contentsOfDirectory(at: root, includingPropertiesForKeys: [.isDirectoryKey])) ?? [])
            .filter { (try? $0.resourceValues(forKeys: [.isDirectoryKey]).isDirectory) == true }
            .sorted { $0.lastPathComponent.localizedStandardCompare($1.lastPathComponent) == .orderedAscending }
        var games: [VQGame] = []
        for folder in folders {
            if let game = inspect(folder) { games.append(game) }
        }
        VQLog.log("Juegos: \(games.count) en \(root.path)")
        return games
    }

    private static func files(in folder: URL, depth: Int = 2) -> [URL] {
        guard let e = FileManager.default.enumerator(at: folder, includingPropertiesForKeys: [.fileSizeKey, .isRegularFileKey],
                                                     options: [.skipsHiddenFiles]) else { return [] }
        var out: [URL] = []
        for case let url as URL in e {
            if e.level > depth { e.skipDescendants(); continue }
            if (try? url.resourceValues(forKeys: [.isRegularFileKey]).isRegularFile) == true { out.append(url) }
        }
        return out
    }

    private static func size(_ url: URL) -> Int {
        (try? url.resourceValues(forKeys: [.fileSizeKey]).fileSize) ?? 0
    }

    static func inspect(_ folder: URL) -> VQGame? {
        let all = files(in: folder)
        let apks = all.filter { $0.pathExtension.lowercased() == "apk" }.sorted { size($0) > size($1) }
        guard let apk = apks.first else {
            VQLog.log("Juegos: \(folder.lastPathComponent): sin .apk, se ignora")
            return nil
        }
        var game = VQGame(id: folder.lastPathComponent, folder: folder, apk: apk, name: folder.lastPathComponent,
                          package: "?", version: "?", engine: "?", graphics: "?", quest3: false, libraries: [],
                          obbs: all.filter { $0.pathExtension.lowercased() == "obb" }, problems: [], manifestText: "")
        var err = [CChar](repeating: 0, count: 256)
        guard let zip = vq_zip_open(apk.path, &err, err.count) else {
            game.problems.append(VQL("No se pudo leer el APK: ", "The APK could not be read: ") + String(cString: err))
            VQLog.log("Juegos: \(game.id): \(game.problems[0])")
            return game
        }
        defer { vq_zip_close(zip) }
        var names: [String] = []
        for i in 0..<vq_zip_count(zip) {
            if let n = vq_zip_name(zip, i) { names.append(String(cString: n)) }
        }
        game.contentNames = names.map { $0.lowercased() }
            + all.map { $0.path.dropFirst(folder.path.count).lowercased() }
        let prefix = "lib/arm64-v8a/"
        game.libraries = names.filter { $0.hasPrefix(prefix) && $0.hasSuffix(".so") && !$0.dropFirst(prefix.count).contains("/") }
        let libNames = Set(game.libraries.map { String($0.dropFirst(prefix.count)) })

        if libNames.contains("libunity.so") || libNames.contains("libil2cpp.so") || libNames.contains("libmonobdwgc-2.0.so") {
            game.engine = "Unity"
        } else if libNames.contains("libUE4.so") || libNames.contains("libUnreal.so") {
            game.engine = "Unreal"
        } else if libNames.isEmpty {
            game.engine = "—"
        } else {
            game.engine = VQL("Nativo/otro", "Native/other")
        }

        // The manifest, as text.
        let mi = vq_zip_find(zip, "AndroidManifest.xml")
        var len = 0
        if mi >= 0, let raw = vq_zip_read(zip, mi, 8 << 20, &len, &err, err.count) {
            if let text = vq_axml_to_text(raw, len) {
                game.manifestText = String(cString: text)
                free(text)
            }
            free(raw)
        }
        let m = game.manifestText
        if let p = attribute(m, element: "manifest", name: "package") { game.package = p }
        if let v = attribute(m, element: "manifest", name: "android:versionName") { game.version = v }
        if let label = attribute(m, element: "application", name: "android:label"), !label.hasPrefix("@") { game.name = label }
        if let ln = metaData(m, "android.app.lib_name") { game.nativeLibName = ln }
        if let fn = metaData(m, "android.app.func_name") { game.nativeFuncName = fn }
        var vulkan = m.contains("android.hardware.vulkan.version") || m.contains("android.hardware.vulkan.level")
        if let devices = metaData(m, "com.oculus.supportedDevices") {
            game.quest3 = devices.lowercased().contains("quest3")
        }
        // Unity's own choice of graphics API.
        let bi = vq_zip_find(zip, "assets/bin/Data/boot.config")
        if bi >= 0, let raw = vq_zip_read(zip, bi, 1 << 20, &len, &err, err.count) {
            let text = String(cString: UnsafeRawPointer(raw).assumingMemoryBound(to: CChar.self))
            free(raw)
            if text.contains("gfx-enable-vulkan=1") || text.contains("vulkan") { vulkan = true }
        }
        game.graphics = vulkan ? "Vulkan" : "OpenGL ES"

        if game.libraries.isEmpty {
            game.problems.append(VQL("El APK no trae librerías arm64-v8a.", "The APK has no arm64-v8a libraries."))
        }
        if game.guestKind == "native" && !game.libraries.isEmpty
            && !game.libraries.contains(where: { $0.hasSuffix("/\(game.entryLib).so") }) {
            game.problems.append(VQL("Motor nativo: no se encuentra su librería de entrada \(game.entryLib).so.",
                                     "Native engine: its entry library \(game.entryLib).so is missing."))
        }
        if !vulkan {
            game.problems.append(VQL("Solo OpenGL ES: esta versión solo ejecuta juegos Vulkan.",
                                     "OpenGL ES only: this version only runs Vulkan games."))
        }
        VQLog.log("Juegos: \(game.id): \(game.package) \(game.version), \(game.engine), \(game.graphics), "
                  + "Quest 3 \(game.quest3 ? "sí" : "no"), driver \(game.guestKind) \(game.entryLib).so, "
                  + "\(game.libraries.count) .so, \(game.obbs.count) OBB, "
                  + (game.compatible ? "compatible" : "NO compatible: \(game.problems.joined(separator: " / "))"))
        return game
    }

    /// `name="value"` inside the first <element ...> of the text manifest.
    static func attribute(_ xml: String, element: String, name: String) -> String? {
        guard let start = xml.range(of: "<\(element) ") ?? xml.range(of: "<\(element)>"),
              let end = xml[start.upperBound...].firstIndex(of: ">") else { return nil }
        return value(String(xml[start.upperBound..<end]), name)
    }

    /// The android:value of <meta-data android:name="key" ...>.
    static func metaData(_ xml: String, _ key: String) -> String? {
        var rest = xml[...]
        while let r = rest.range(of: "<meta-data ") {
            guard let end = rest[r.upperBound...].firstIndex(of: ">") else { return nil }
            let tag = String(rest[r.upperBound..<end])
            if value(tag, "android:name") == key { return value(tag, "android:value") }
            rest = rest[end...]
        }
        return nil
    }

    private static func value(_ tag: String, _ name: String) -> String? {
        guard let r = tag.range(of: " \(name)=\"") ?? (tag.hasPrefix("\(name)=\"") ? tag.range(of: "\(name)=\"") : nil),
              let q = tag[r.upperBound...].firstIndex(of: "\"") else { return nil }
        return String(tag[r.upperBound..<q])
            .replacingOccurrences(of: "&quot;", with: "\"").replacingOccurrences(of: "&lt;", with: "<")
            .replacingOccurrences(of: "&gt;", with: ">").replacingOccurrences(of: "&amp;", with: "&")
    }
}
