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
    // Graphics (VPEngine). The game's own Quest foveation is always off.
    enum Foveation: Int, CaseIterable { case off = 0, m2 = 1, m5 = 2, custom = 3 }
    @Published var foveation: Foveation { didSet { save() } }
    @Published var sharpZone: Double { didSet { save() } }      // 0.05...1
    @Published var periphery: Double { didSet { save() } }      // 0.1...1
    @Published var fovHeight: Double { didSet { save() } }      // -0.5...0.5
    @Published var vpktaa: Bool { didSet { save() } }
    @Published var vpktaaPercent: Double { didSet { save() } }  // 0...100
    @Published var vpktaaSharpness: Double { didSet { save() } } // 0...1
    @Published var renderScale: Double { didSet { save() } }    // 0.5...1.5
    @Published var aswDisabled: Bool { didSet { save() } }
    @Published var quest3Profile: Bool { didSet { save() } }

    private init() {
        let d = UserDefaults.standard
        offline = d.object(forKey: "vq.offline") as? Bool ?? true
        guestLog = d.object(forKey: "vq.guestLog") as? Bool ?? true
        watchdogSeconds = d.object(forKey: "vq.watchdog") as? Double ?? 120
        var fov = Foveation(rawValue: d.object(forKey: "vq.fov") as? Int ?? 0) ?? .off
        if fov == .m5 && !VQSettings.isM5 { fov = .m2 }  // M5 preset only on an M5
        foveation = fov
        sharpZone = d.object(forKey: "vq.fovSharp") as? Double ?? 0.35
        periphery = d.object(forKey: "vq.fovPeriphery") as? Double ?? 0.5
        fovHeight = d.object(forKey: "vq.fovHeight") as? Double ?? 0
        vpktaa = d.object(forKey: "vq.vpktaa") as? Bool ?? true
        vpktaaPercent = d.object(forKey: "vq.vpktaaPercent") as? Double ?? 80
        vpktaaSharpness = d.object(forKey: "vq.vpktaaSharp") as? Double ?? 0.5
        renderScale = d.object(forKey: "vq.renderScale") as? Double ?? 1
        aswDisabled = d.object(forKey: "vq.aswOff") as? Bool ?? true
        quest3Profile = d.object(forKey: "vq.quest3") as? Bool ?? false  // Klepton presents Quest 2 (JNI agrees)
    }

    /// First Apple Vision Pro (M2) is RealityDevice14,x; a later one is taken as an M5
    /// (same rule as PTVisionPro). Unknown: M2.
    static let isM5: Bool = {
        for name in ["hw.machine", "hw.model"] {
            var size = 0
            guard sysctlbyname(name, nil, &size, nil, 0) == 0, size > 0 else { continue }
            var buf = [CChar](repeating: 0, count: size)
            guard sysctlbyname(name, &buf, &size, nil, 0) == 0 else { continue }
            let m = String(cString: buf)
            if m.contains("RealityDevice14") { return false }
            if m.contains("RealityDevice") { return true }
        }
        return ProcessInfo.processInfo.physicalMemory >= 24 * 1024 * 1024 * 1024
    }()

    private func save() {
        let d = UserDefaults.standard
        d.set(offline, forKey: "vq.offline")
        d.set(guestLog, forKey: "vq.guestLog")
        d.set(watchdogSeconds, forKey: "vq.watchdog")
        d.set(foveation.rawValue, forKey: "vq.fov")
        d.set(sharpZone, forKey: "vq.fovSharp")
        d.set(periphery, forKey: "vq.fovPeriphery")
        d.set(fovHeight, forKey: "vq.fovHeight")
        d.set(vpktaa, forKey: "vq.vpktaa")
        d.set(vpktaaPercent, forKey: "vq.vpktaaPercent")
        d.set(vpktaaSharpness, forKey: "vq.vpktaaSharp")
        d.set(renderScale, forKey: "vq.renderScale")
        d.set(aswDisabled, forKey: "vq.aswOff")
        d.set(quest3Profile, forKey: "vq.quest3")
    }

    /// Klepton's knobs for these settings.
    func applyToEnvironment() {
        setenv("KL_NET_OFFLINE", offline ? "1" : "0", 1)
        setenv("KL_LOG_FILE", guestLog ? "1" : "0", 1)
        setenv("KL_ALARM", String(Int(watchdogSeconds)), 1)
        // The launcher already decided; BootView must boot straight away.
        setenv("KL_AUTOBOOT", "1", 1)
        setenv("KL_IMMERSIVE", "1", 1)
        // Eye texture size the game is told (Klepton's ovrp side) and headset identity:
        // 11 = ovrpSystemHeadset Meta_Quest_3; off: Klepton's per-game default (Quest 2 / Quest).
        setenv("KL_OVRP_EYE_SCALE", String(format: "%.2f", renderScale), 1)
        if quest3Profile { setenv("KL_OVRP_HEADSET", "11", 1) } else { unsetenv("KL_OVRP_HEADSET") }
        // ASW: Klepton never hands out SpaceWarp textures (ovrp_GetLayerTextureSpaceWarp
        // -> unsupported), so every frame is real; the setting is passed on for VPVulkan.
        var c = vq_gfx_config()
        c.foveation_mode = Int32(foveation.rawValue)
        c.sharp_zone = Float(sharpZone)
        c.periphery = Float(periphery)
        c.height = Float(fovHeight)
        c.vpktaa_enabled = vpktaa ? 1 : 0
        c.vpktaa_percent = Float(vpktaaPercent)
        c.vpktaa_sharpness = Float(vpktaaSharpness)
        c.render_scale = Float(renderScale)
        c.asw_disabled = aswDisabled ? 1 : 0
        c.quest3_profile = quest3Profile ? 1 : 0
        _ = vq_gfx_config_apply(&c)
        VQLog.log("Gráficos: foveado Quest siempre apagado; foveado propio \(foveation), VPKTAA \(vpktaa) \(Int(vpktaaPercent)) % nitidez \(vpktaaSharpness), escala \(renderScale), ASW apagado \(aswDisabled), perfil Quest 3 \(quest3Profile)")
        VQLog.log("Ajustes: sin red \(offline), log del juego \(guestLog), vigilante \(Int(watchdogSeconds)) s")
    }
}
