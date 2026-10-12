// VisionQuestPro: the bridge from the launcher to Klepton's boot (BootView in KleptonApp.swift):
// which game, where its files are, and the settings that reach the runtime as its environment
// (Klepton reads its knobs with getenv; the player sets them here, never in a text file).

import Combine
import Foundation

enum VQLaunch {
    struct Choice {
        let game: VQGame
        let prepared: VQPrepare.Prepared
    }

    /// Set by the launcher just before BootView boots.
    nonisolated(unsafe) static var choice: Choice?

    /// kl_app_configure_game with the chosen game's paths. Same contract as kl_app_configure.
    static func configure() -> Int32 {
        guard let choice else {
            VQLog.log("Arranque: no hay juego elegido")
            return 1
        }
        let game = choice.game
        guard let files = VQFolder.shared.saves(for: game.id) else { return 1 }
        // The OBB dir the game is told (relative to its files dir): the folder holding its .obb,
        // else the game folder itself (loose data).
        let obbFolder = game.obbs.first?.deletingLastPathComponent() ?? game.folder
        let obbRel = relativePath(from: files, to: obbFolder)
        let documents = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0].path
        // Graphics libraries ride in the app (Frameworks/), not beside the converted game.
        let frameworks = Bundle.main.privateFrameworksPath ?? (Bundle.main.bundlePath + "/Frameworks")
        setenv("KL_MVK_DIR", frameworks, 1)
        setenv("KL_ANGLE_DIR", frameworks, 1)
        VQSettings.shared.applyToEnvironment()
        VQLog.log("Arranque: \(game.id) (\(game.package)) tree=\(choice.prepared.tree.path) dylibs=\(choice.prepared.dylibs.path) "
                  + "files=\(files.path) obb=\(obbRel)")
        let rc = kl_app_configure_game(game.id, choice.prepared.tree.path, game.apk.path, choice.prepared.dylibs.path,
                                       files.path, obbRel, documents)
        VQLog.log("Arranque: kl_app_configure_game -> \(rc): \(String(cString: kl_app_status()))")
        return rc
    }

    /// `to` relative to `from` (both directories), with ".." where needed.
    static func relativePath(from: URL, to: URL) -> String {
        let a = from.standardizedFileURL.pathComponents, b = to.standardizedFileURL.pathComponents
        var i = 0
        while i < a.count && i < b.count && a[i] == b[i] { i += 1 }
        let parts = Array(repeating: "..", count: a.count - i) + b[i...]
        return parts.isEmpty ? "." : parts.joined(separator: "/")
    }
}

/// The settings the app has today. All in the app (UserDefaults), shown in the Settings sheet.
final class VQSettings: ObservableObject {
    static let shared = VQSettings()

    @Published var offline: Bool { didSet { save() } }
    @Published var guestLog: Bool { didSet { save() } }
    @Published var watchdogSeconds: Double { didSet { save() } }

    private init() {
        let d = UserDefaults.standard
        offline = d.object(forKey: "vq.offline") as? Bool ?? true
        guestLog = d.object(forKey: "vq.guestLog") as? Bool ?? true
        watchdogSeconds = d.object(forKey: "vq.watchdog") as? Double ?? 120
    }

    private func save() {
        let d = UserDefaults.standard
        d.set(offline, forKey: "vq.offline")
        d.set(guestLog, forKey: "vq.guestLog")
        d.set(watchdogSeconds, forKey: "vq.watchdog")
    }

    /// Klepton's knobs for these settings.
    func applyToEnvironment() {
        setenv("KL_NET_OFFLINE", offline ? "1" : "0", 1)
        setenv("KL_LOG_FILE", guestLog ? "1" : "0", 1)
        setenv("KL_ALARM", String(Int(watchdogSeconds)), 1)
        // The launcher already decided; BootView must boot straight away.
        setenv("KL_AUTOBOOT", "1", 1)
        setenv("KL_IMMERSIVE", "1", 1)
        VQLog.log("Ajustes: sin red \(offline), log del juego \(guestLog), vigilante \(Int(watchdogSeconds)) s")
    }
}
