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
        // VRP-style copies keep loose data (no .obb extension) in `<game>/<package>/`: use that
        // folder when it exists, as the Quest's /sdcard/Android/obb/<package>/ (AC Nexus `start0`).
        let pkgDir = game.folder.appendingPathComponent(game.package, isDirectory: true)
        var isDir: ObjCBool = false
        let hasPkgDir = !game.package.isEmpty
            && FileManager.default.fileExists(atPath: pkgDir.path, isDirectory: &isDir) && isDir.boolValue
        let obbFolder = game.obbs.first?.deletingLastPathComponent() ?? (hasPkgDir ? pkgDir : game.folder)
        let obbRel = relativePath(from: files, to: obbFolder)
        let documents = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0].path
        // Graphics libraries ride in the app (Frameworks/), not beside the converted game.
        let frameworks = Bundle.main.privateFrameworksPath ?? (Bundle.main.bundlePath + "/Frameworks")
        setenv("KL_MVK_DIR", frameworks, 1)
        setenv("KL_ANGLE_DIR", frameworks, 1)
        VQSettings.shared.applyToEnvironment(game: game)
        let psoCache = FileManager.default.urls(for: .cachesDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("VisionQuestPro/PipelineCache/\(game.id)", isDirectory: true)
        setenv("VP_PIPELINE_CACHE_DIR", psoCache.path, 1)
        // The conformance tests compile their own pipelines; a prewarm of earlier runs' recipes
        // would only slow every vkCreateDevice down (prepareRun turns it off in the driver too).
        if !VQConformance.isConformance(game) { VQPrewarm.run(gameFolder: game.folder, cache: psoCache) }
        VQLanguage.apply(game, useSystem: VQSettings.shared.systemLanguage)
        VQOVRPluginVersion.apply(apk: game.apk)
        VQLog.log("Arranque: \(game.id) (\(game.package)) tree=\(choice.prepared.tree.path) dylibs=\(choice.prepared.dylibs.path) "
                  + "files=\(files.path) obb=\(obbRel)")
        VQApiScan.run(game: game, tree: choice.prepared.tree)
        if game.package == VQConformance.openXRPackage { VQConformance.prepareOpenXR() } else { unsetenv("KL_INTENT_EXTRAS") }
        if VQConformance.isConformance(game) {
            // After applyToEnvironment: the test run is window-only (no immersive space).
            VQConformance.prepareRun(game: game, tree: choice.prepared.tree, saves: files)
        }
        setenv("KL_GUEST_KIND", game.guestKind, 1)
        setenv("KL_GUEST_ENTRY", game.entryLib, 1)
        if game.nativeFuncName.isEmpty { unsetenv("KL_GUEST_FUNC") } else { setenv("KL_GUEST_FUNC", game.nativeFuncName, 1) }
        let rc = kl_app_configure_game(game.id, choice.prepared.tree.path, game.apk.path, choice.prepared.dylibs.path,
                                       files.path, obbRel, documents)
        VQLog.log("Arranque: kl_app_configure_game -> \(rc): \(String(cString: kl_app_status()))")
        // Re-push the settings now that the driver is loaded and stderr reaches the log
        // (the first push in applyToEnvironment can predate both).
        let fwd = vq_gfx_config_forward()
        VQLog.log("Gráficos: ajustes enviados a VPVulkan \(fwd == 1 ? "sí" : "no (driver no cargado)")")
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
    // Graphics (VPKEngine). The game's own Quest foveation is always off.
    enum Foveation: Int, CaseIterable { case off = 0, m2 = 1, m5 = 2, custom = 3 }
    @Published var foveation: Foveation { didSet { save() } }
    @Published var sharpZone: Double { didSet { save() } }      // 0.05...1
    @Published var periphery: Double { didSet { save() } }      // 0.1...1
    @Published var fovHeight: Double { didSet { save() } }      // -0.5...0.5
    // The old global AA switches (vq.vpktaa4, vq.gameMSAA) are no longer read: antialiasing
    // is per game (VQGameAA), automatic by default.
    @Published var vpktaaPercent: Double { didSet { save() } }  // 0...100
    @Published var vpktaaSharpness: Double { didSet { save() } } // 0...1
    /// 16x anisotropic filtering on the game's mipmapped linear samplers (VPVulkan).
    /// Forced anisotropic filtering level for material samplers: 0 (off), 2, 4, 8 or 16.
    @Published var anisoLevel: Int { didSet { save() } }
    /// "Diagnóstico de rendimiento": per-pass GPU time lines (gpu.passtime) in the log.
    @Published var perfDiag: Bool { didSet { save() } }
    /// "Diagnóstico de ficheros": every guest file operation and its result in the log (KL_TRACE_FS=1).
    @Published var fsDiag: Bool { didSet { save() } }
    @Published var renderScale: Double { didSet { save() } }    // 0.5...1.5
    // Resolution base: false = the game's original (Quest 3 budget), true = Vision Pro's request.
    /// "original" (Quest 3 budget) | "compositor" (visionOS drawable) | "native" (AVP panel density, heaviest).
    @Published var resolutionBase: String { didSet { save() } }
    /// Head-locked FPS counter in the compositor (KL_FPS_HUD).
    @Published var showFps: Bool { didSet { save() } }
    @Published var aswDisabled: Bool { didSet { save() } }
    @Published var quest3Profile: Bool { didSet { save() } }
    @Published var forceVulkan: Bool { didSet { save() } }
    @Published var diagCaptures: Bool { didSet { save() } }
    /// Vulkan driver: VPKEngine's own (VPVulkan) or MoltenVK (to compare, or if VPKEngine fails).
    @Published var useVPKEngine: Bool { didSet { save() } }
    /// Initial posture for the OVRPlugin eye height (KL_OVRP_POSTURE); auto re-measure always on.
    @Published var posture: String { didSet { save() } }   // "auto" | "seated" | "standing"
    /// Passthrough (KL_PASSTHROUGH): "auto" follows the game, "off" always VR, "on" always mixed.
    @Published var passthrough: String { didSet { save() } }
    /// Run the game in the system language when it ships it, else English (LANG for Klepton's Locale/AConfiguration).
    @Published var systemLanguage: Bool { didSet { save() } }
    /// Compile the game's known shaders/pipelines before it starts (VPVulkan recipe store).
    @Published var precompileShaders: Bool { didSet { save() } }
    /// Conformance runs (Vulkan CTS): minutes per session and the memory limit between batches.
    @Published var conformanceMinutes: Double { didSet { save() } }
    @Published var conformanceMemoryMB: Int { didSet { save() } }

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
        vpktaaPercent = d.object(forKey: "vq.vpktaaPercent") as? Double ?? 80
        vpktaaSharpness = d.object(forKey: "vq.vpktaaSharp3") as? Double ?? 1.0  // 3: reconstruction sharpening (PT default 1.0)
        // Older builds stored a bool (vq.aniso16); off there stays off.
        anisoLevel = d.object(forKey: "vq.anisoLevel") as? Int
            ?? ((d.object(forKey: "vq.aniso16") as? Bool) == false ? 0 : 16)
        perfDiag = d.object(forKey: "vq.perfDiag") as? Bool ?? false
        fsDiag = d.object(forKey: "vq.fsDiag") as? Bool ?? false
        renderScale = d.object(forKey: "vq.renderScale") as? Double ?? 1
        if let rb = d.string(forKey: "vq.resBase"), ["original", "compositor", "native"].contains(rb) {
            resolutionBase = rb
        } else {
            resolutionBase = (d.object(forKey: "vq.resAVP") as? Bool ?? false) ? "compositor" : "original"
        }
        showFps = d.object(forKey: "vq.showFps") as? Bool ?? false
        aswDisabled = d.object(forKey: "vq.aswOff") as? Bool ?? true
        quest3Profile = d.object(forKey: "vq.quest3v2") as? Bool ?? true  // Quest 3 for every game by default
        forceVulkan = d.object(forKey: "vq.forceVulkan") as? Bool ?? true
        diagCaptures = d.object(forKey: "vq.diagCaptures2") as? Bool ?? false
        // Saved as "vq.vpengine" before the engine was renamed VPKEngine.
        useVPKEngine = (d.object(forKey: "vq.vpkengine") ?? d.object(forKey: "vq.vpengine")) as? Bool ?? true
        posture = d.string(forKey: "vq.posture") ?? "auto"
        if let pt = d.string(forKey: "vq.passthrough"), ["auto", "off", "on"].contains(pt) {
            passthrough = pt
        } else { passthrough = "auto" }
        systemLanguage = d.object(forKey: "vq.sysLang") as? Bool ?? true
        precompileShaders = d.object(forKey: "vq.precompile") as? Bool ?? true
        conformanceMinutes = d.object(forKey: "vq.ctsMinutes") as? Double ?? 15
        conformanceMemoryMB = d.object(forKey: "vq.ctsMemMB") as? Int ?? 3000
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
        d.set(vpktaaPercent, forKey: "vq.vpktaaPercent")
        d.set(vpktaaSharpness, forKey: "vq.vpktaaSharp3")
        d.set(anisoLevel, forKey: "vq.anisoLevel")
        d.set(perfDiag, forKey: "vq.perfDiag")
        d.set(fsDiag, forKey: "vq.fsDiag")
        d.set(renderScale, forKey: "vq.renderScale")
        d.set(resolutionBase, forKey: "vq.resBase")
        d.set(showFps, forKey: "vq.showFps")
        d.set(aswDisabled, forKey: "vq.aswOff")
        d.set(quest3Profile, forKey: "vq.quest3v2")
        d.set(forceVulkan, forKey: "vq.forceVulkan")
        d.set(diagCaptures, forKey: "vq.diagCaptures2")
        d.set(useVPKEngine, forKey: "vq.vpkengine")
        d.set(posture, forKey: "vq.posture")
        d.set(passthrough, forKey: "vq.passthrough")
        d.set(systemLanguage, forKey: "vq.sysLang")
        d.set(precompileShaders, forKey: "vq.precompile")
        d.set(conformanceMinutes, forKey: "vq.ctsMinutes")
        d.set(conformanceMemoryMB, forKey: "vq.ctsMemMB")
    }

    /// VPVulkan inside the app, if this build carries it.
    static var vpvulkanPath: String? {
        let frameworks = Bundle.main.privateFrameworksPath ?? (Bundle.main.bundlePath + "/Frameworks")
        let path = frameworks + "/VPVulkan.framework/VPVulkan"
        return FileManager.default.fileExists(atPath: path) ? path : nil
    }

    /// Chooses the Vulkan driver Klepton loads (KL_MVK_DYLIB). VPVulkan is opened here first, with
    /// RTLD_GLOBAL, so the settings reach it (vpvk_config_set_*) before the game creates its device;
    /// Klepton's own dlopen of the same path then returns this handle.
    func selectDriver() {
        if useVPKEngine, let path = VQSettings.vpvulkanPath {
            if dlopen(path, RTLD_NOW | RTLD_GLOBAL) != nil {
                setenv("KL_MVK_DYLIB", path, 1)
                VQLog.log("Gráficos: VPKEngine (VPVulkan) \(path)")
                return
            }
            let reason = dlerror().map { String(cString: $0) } ?? "?"
            VQLog.log("Gráficos: VPVulkan no carga (\(reason)); se usa MoltenVK")
        } else if useVPKEngine {
            VQLog.log("Gráficos: esta build no trae VPVulkan; se usa MoltenVK")
        } else {
            VQLog.log("Gráficos: MoltenVK (elegido en ajustes)")
        }
        unsetenv("KL_MVK_DYLIB")
    }

    /// Klepton's knobs for these settings (and the chosen game's own AA).
    func applyToEnvironment(game: VQGame?) {
        selectDriver()
        setenv("KL_NET_OFFLINE", offline ? "1" : "0", 1)
        setenv("KL_LOG_FILE", guestLog ? "1" : "0", 1)
        setenv("KL_ALARM", String(Int(watchdogSeconds)), 1)
        // The launcher already decided; BootView must boot straight away.
        setenv("KL_AUTOBOOT", "1", 1)
        setenv("KL_IMMERSIVE", "1", 1)
        setenv("KL_VK_SPOOF_ADRENO", "1", 1)  // Quest titles check the GPU identity before choosing Vulkan
        setenv("MVK_CONFIG_FULL_IMAGE_VIEW_SWIZZLE", "1", 1)  // MoltenVK: honour VkComponentMapping (R8 font atlases)
        // Unity player argument (the "unity" launch extra): use Vulkan even when Unity's own
        // device detection would fall back to GLES, which has no driver here.
        if forceVulkan { setenv("KL_UNITY_ARGS", "-force-vulkan", 1) } else { unsetenv("KL_UNITY_ARGS") }
        VQLog.log("Gráficos: forzar Vulkan en Unity \(forceVulkan)")
        // Frame captures (PNG in Documents/Registros) at compositor frames 120/600/1200.
        setenv("KL_CAPTURE", diagCaptures ? "1" : "0", 1)
        if let quest = VQFolder.shared.url {
            let dir = quest.appendingPathComponent("Registros", isDirectory: true)
            try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
            setenv("KL_CAPTURE_DIR", dir.path, 1)
            VQLog.log("Capturas en \(dir.path)")
            // VPKEngine per-pass dump (PNG per render/compute pass + gpu.dump lines) at GPU frame 600.
            if diagCaptures {
                setenv("VP_DUMP_FRAME", "600", 1)
                setenv("VP_DUMP_DIR", dir.path, 1)
            } else {
                unsetenv("VP_DUMP_FRAME")
                unsetenv("VP_DUMP_DIR")
            }
        } else {
            unsetenv("VP_DUMP_FRAME")
            unsetenv("VP_DUMP_DIR")
        }
        VQLog.log("Gráficos: capturas de diagnóstico \(diagCaptures)")
        // Eye texture size the game is told (Klepton's ovrp side) and headset identity:
        // 11 = ovrpSystemHeadset Meta_Quest_3; off: Klepton's per-game default (Quest 2 / Quest).
        setenv("KL_OVRP_POSTURE", posture, 1)
        setenv("KL_PASSTHROUGH", passthrough, 1)
        VQLog.log("Passthrough: \(passthrough)")
        VQLog.log("Gráficos: postura \(posture)")
        setenv("KL_OVRP_EYE_SCALE", String(format: "%.2f", renderScale), 1)
        setenv("KL_OVRP_EYE_BASE", resolutionBase, 1)
        VQLog.log("Gráficos: resolución base \(resolutionBase)")
        // Quest 3 identity for every game, in every place a title can check it: Android Build
        // (MODEL/DEVICE eureka), OVRPlugin headset type, OpenXR system name and the GPU.
        if quest3Profile {
            setenv("KL_QUEST3", "1", 1)
            setenv("KL_OVRP_HEADSET", "11", 1)
            setenv("KL_XR_SYSTEM_NAME", "Meta Quest 3", 1)
        } else {
            unsetenv("KL_QUEST3"); unsetenv("KL_OVRP_HEADSET"); unsetenv("KL_XR_SYSTEM_NAME")
        }
        // ASW: Klepton never hands out SpaceWarp textures (ovrp_GetLayerTextureSpaceWarp
        // -> unsupported), so every frame is real; the setting is passed on for VPVulkan.
        var c = vq_gfx_config()
        c.foveation_mode = Int32(foveation.rawValue)
        c.sharp_zone = Float(sharpZone)
        c.periphery = Float(periphery)
        c.height = Float(fovHeight)
        // Antialiasing, per game (VQGameAA): KL_AA_MODE auto|vpktaa|msaa|off, VPVK_MSAA
        // 0/2/4/8 (only where the game asked for MSAA), VQ_GAME_KEY for the detection
        // KleptonTAA stores (vq.aaDetected.<key>). VPKTAA is allowed in auto/vpktaa.
        let aaKey = game?.aaKey ?? ""
        let aa = VQGameAA.load(key: aaKey)
        let msaaOverride = aa.effectiveMSAA
        let vpktaa = aa.mode == .auto || aa.mode == .vpktaa
        if aaKey.isEmpty { unsetenv("VQ_GAME_KEY") } else { setenv("VQ_GAME_KEY", aaKey, 1) }
        setenv("KL_AA_MODE", aa.mode.rawValue, 1)
        setenv("VPVK_MSAA", String(msaaOverride), 1)
        let detected = VQGameAA.detected(key: aaKey)
        c.vpktaa_enabled = vpktaa ? 1 : 0
        c.msaa_override = Int32(msaaOverride)
        c.vpktaa_percent = Float(vpktaaPercent)
        c.vpktaa_sharpness = Float(vpktaaSharpness)
        c.anisotropy = Int32(anisoLevel)
        setenv("VPVK_ANISO", String(anisoLevel), 1)  // read by VPVulkan even if the dlsym forward misses
        // Per-pass GPU timing in VPKEngine (gpu.passtime lines, one rendering frame every 90).
        if perfDiag { setenv("VP_PASS_TIMING", "90", 1) } else { unsetenv("VP_PASS_TIMING") }
        if fsDiag { setenv("KL_TRACE_FS", "1", 1) } else { unsetenv("KL_TRACE_FS") }
        // Game MSAA forced to 1x ONLY in the explicit VPKTAA mode; auto keeps a game's MSAA
        // (and turns VPKTAA off for it, live, once VPKEngine sees the eye pass).
        c.force_single_sample = aa.mode == .vpktaa ? 1 : 0
        setenv("KL_FPS_HUD", showFps ? "1" : "0", 1)
        setenv("VPVK_FORCE_1X", c.force_single_sample != 0 ? "1" : "0", 1)
        c.render_scale = Float(renderScale)
        c.asw_disabled = aswDisabled ? 1 : 0
        c.quest3_profile = quest3Profile ? 1 : 0
        _ = vq_gfx_config_apply(&c)
        // VPKTAA runs in Klepton's compositor (KleptonTAA.swift), for every driver.
        setenv("KL_VPKTAA", vpktaa ? "1" : "0", 1)
        setenv("KL_VPKTAA_PCT", String(format: "%.0f", vpktaaPercent), 1)
        setenv("KL_VPKTAA_SHARP", String(format: "%.2f", vpktaaSharpness), 1)
        vq_prewarm_set_enabled(precompileShaders ? 1 : 0)
        // Also through the environment: the call above reaches VPVulkan only once it is loaded,
        // and its first vkCreateDevice may come before the app can repeat it.
        setenv("VPVK_PREWARM", precompileShaders ? "1" : "0", 1)
        // Games keep going after a single hung or faulting GPU frame (conformance runs set it to 1).
        unsetenv("VPVK_DEVICE_LOST_STRICT")
        let detText = detected.map { "\($0)x" } ?? "no"
        VQLog.log("Gráficos: foveado Quest siempre apagado; foveado propio \(foveation), AA: modo \(aa.mode.rawValue), MSAA override \(msaaOverride), detectado \(detText) (juego \(aaKey.isEmpty ? "?" : aaKey)\(aa.stored ? "" : ", por defecto")), VPKTAA permitido \(vpktaa) \(Int(vpktaaPercent)) % nitidez \(vpktaaSharpness), forzar 1x \(c.force_single_sample != 0), anisotrópico x\(anisoLevel), diagnóstico de rendimiento \(perfDiag), diagnóstico de ficheros \(fsDiag), escala \(renderScale), ASW apagado \(aswDisabled), perfil Quest 3 \(quest3Profile), FPS en pantalla \(showFps)")
        VQLog.log("Ajustes: sin red \(offline), log del juego \(guestLog), vigilante \(Int(watchdogSeconds)) s")
    }
}

/// Antialiasing chosen per game (the contract with Klepton/VPKEngine, via environment):
///   KL_AA_MODE  auto (default): VPKTAA on a game whose eye pass is 1x, the game's own MSAA
///               (VPKTAA off) on one that uses MSAA — decided live once VPKEngine sees it.
///               vpktaa: game MSAA forced to 1x (VPVK_FORCE_1X=1) + VPKTAA. msaa: never VPKTAA.
///               off: neither VPKTAA nor an MSAA override.
///   VPVK_MSAA   0 = the game's own sample count, else 2/4/8 where the game asked for MSAA
///               (VPKEngine clamps to what the chip supports).
/// Saved by game key (package, else folder name): vq.aaMode.<key>, vq.aaMSAA.<key>.
/// Read-only here, written by KleptonTAA: vq.aaDetected.<key> (1/2/4/8) and vq.msaaDeviceMax.
struct VQGameAA: Equatable {
    enum Mode: String, CaseIterable { case auto, vpktaa, msaa, off }
    var mode: Mode = .auto
    var msaa: Int = 0          // 0 (original) | 2 | 4 | 8
    var stored = false         // false: default (no per-game setting saved yet)

    /// The override actually sent: none in vpktaa (forced 1x) and off modes.
    var effectiveMSAA: Int { (mode == .auto || mode == .msaa) ? msaa : 0 }

    static func load(key: String) -> VQGameAA {
        let d = UserDefaults.standard
        var aa = VQGameAA()
        if !key.isEmpty, let m = d.string(forKey: "vq.aaMode." + key), let mode = Mode(rawValue: m) {
            aa.mode = mode
            aa.stored = true
        }
        if !key.isEmpty, let n = d.object(forKey: "vq.aaMSAA." + key) as? Int, [0, 2, 4, 8].contains(n) {
            aa.msaa = n
        }
        return aa
    }

    func save(key: String) {
        guard !key.isEmpty else { return }
        let d = UserDefaults.standard
        d.set(mode.rawValue, forKey: "vq.aaMode." + key)
        d.set(msaa, forKey: "vq.aaMSAA." + key)
    }

    /// The game's own eye-pass sample count seen on an earlier run (1 = no MSAA), nil if never seen.
    static func detected(key: String) -> Int? {
        guard !key.isEmpty, let n = UserDefaults.standard.object(forKey: "vq.aaDetected." + key) as? Int,
              n >= 1 else { return nil }
        return n
    }
}

/// Game language: the system language if the game ships it (APK entries or data files name it),
/// otherwise English. Klepton answers java.util.Locale and AConfiguration from LANG.
enum VQLanguage {
    static func apply(_ game: VQGame, useSystem: Bool) {
        // The user's preferred system language, not Locale.current (that one follows the app's own
        // localizations, English only here).
        let pref = Locale(identifier: Locale.preferredLanguages.first ?? "en-US")
        let code = (pref.language.languageCode?.identifier ?? "en").lowercased()
        let region = pref.region?.identifier ?? Locale.current.region?.identifier ?? ""
        var lang = "en_US"
        var reason = "por defecto"
        if !useSystem {
            reason = "opción desactivada"
        } else if code == "en" {
            lang = "en_" + (region.isEmpty ? "US" : region); reason = "sistema en inglés"
        } else if let hit = match(code, game.contentNames) {
            lang = code + "_" + (region.isEmpty ? code.uppercased() : region)
            reason = "el juego lo incluye (\(hit))"
        } else {
            reason = "el juego no incluye '\(code)'"
        }
        setenv("LANG", lang + ".UTF-8", 1)
        VQLog.log("Idioma: \(lang) (\(reason))")
    }

    /// First content name that names the language: the ISO code as a token (es, es-ES, es_419,
    /// (es-es)), or its English / native name (spanish, español).
    static func match(_ code: String, _ names: [String]) -> String? {
        var words: [String] = []
        if let en = Locale(identifier: "en").localizedString(forLanguageCode: code) { words.append(en.lowercased()) }
        if let native = Locale(identifier: code).localizedString(forLanguageCode: code) {
            let n = native.lowercased()
            words.append(n)
            words.append(n.folding(options: .diacriticInsensitive, locale: nil))
        }
        let esc = NSRegularExpression.escapedPattern(for: code)
        // A bare 2-letter code is too common in file names (no.png, it/…): require a region
        // (es-es, es_419) or a language context word before it (values-es, audio_es, lang/es).
        let regional = try? NSRegularExpression(pattern: "(^|[^a-z])" + esc + "[-_]([a-z]{2}|[0-9]{3})([^a-z0-9]|$)")
        let regions = Set(Locale.Region.isoRegions.map { $0.identifier.lowercased() })
        let patterns = ["(lang|language|locale|locali[sz]ation|values|audio|voice|vo|text|strings|subtitles?)[-_/.]" + esc + "([^a-z]|$)"]
        let res = patterns.compactMap { try? NSRegularExpression(pattern: $0) }
        for n in names {
            if words.contains(where: { $0.count > 2 && n.contains($0) }) { return n }
            let r = NSRange(n.startIndex..., in: n)
            if res.contains(where: { $0.firstMatch(in: n, range: r) != nil }) { return n }
            if let regional, let m = regional.firstMatch(in: n, range: r), let rr = Range(m.range(at: 2), in: n),
               regions.contains(String(n[rr])) || n[rr].allSatisfy(\.isNumber) { return n }
        }
        return nil
    }
}

/// The game's own OVRPlugin version (read from lib/arm64-v8a/libOVRPlugin.so inside the APK) → KL_OVRP_VERSION,
/// so the shim reports what the game's C# was built against.
enum VQOVRPluginVersion {
    static func apply(apk: URL) {
        unsetenv("KL_OVRP_VERSION")
        var err = [CChar](repeating: 0, count: 256)
        guard let zip = vq_zip_open(apk.path, &err, err.count) else { return }
        defer { vq_zip_close(zip) }
        let i = vq_zip_find(zip, "lib/arm64-v8a/libOVRPlugin.so")
        var len = 0
        guard i >= 0, let raw = vq_zip_read(zip, i, 64 << 20, &len, &err, err.count) else {
            VQLog.log("OVRPlugin: el juego no trae libOVRPlugin.so")
            return
        }
        defer { free(raw) }
        let bytes = UnsafeBufferPointer(start: raw, count: len)
        var best: [Int]? = nil
        var start = 0
        for j in 0...len {
            if j == len || bytes[j] == 0 {
                let n = j - start
                if n >= 5 && n <= 11, bytes[start] == UInt8(ascii: "1"), bytes[start + 1] == UInt8(ascii: ".") {
                    let s = String(decoding: bytes[start..<j], as: UTF8.self)
                    let parts = s.split(separator: ".", omittingEmptySubsequences: false).map { Int($0) }
                    if parts.count == 3, let a = parts[0], let b = parts[1], let c = parts[2],
                       a == 1, b >= 10, b < 1000, c >= 0 {
                        let v = [a, b, c]
                        if best == nil || best!.lexicographicallyPrecedes(v) { best = v }
                    }
                }
                start = j + 1
            }
        }
        if let v = best {
            let ver = v.map(String.init).joined(separator: ".")
            setenv("KL_OVRP_VERSION", ver, 1)
            VQLog.log("OVRPlugin: versión del juego \(ver)")
        } else {
            VQLog.log("OVRPlugin: sin versión en libOVRPlugin.so; se usa la de Klepton")
        }
    }
}

/// Shader precompilation before the game boots: VPVulkan rebuilds every pipeline recipe stored by
/// earlier runs (VP_PIPELINE_CACHE_DIR) on all cores and saves the Metal binary archive, so the
/// game's own pipeline creation is cache hits instead of 200-400 ms compiles mid-play.
/// Runs on the boot thread (configure()); progress is published for VQLoadingView.
final class VQPrewarm: ObservableObject {
    static let shared = VQPrewarm()
    @Published var done: UInt32 = 0
    @Published var total: UInt32 = 0
    @Published var active = false
    @Published var compiling = false  // first run: harvested SPIR-V being compiled ("Compilando shaders")

    /// Bump when the harvest or the SPIR-V -> MSL translator changes: the first-run precompile repeats.
    static let firstRunVersion = 2  // 2: VPKEngine per-variable private slots (new MSL)

    static func run(gameFolder: URL, cache: URL) {
        guard VQSettings.shared.precompileShaders else {
            VQLog.log("Shaders: precompilación desactivada en ajustes")
            return
        }
        firstRun(gameFolder: gameFolder, cache: cache)
        let count = vq_prewarm_count()  // 0 as well when VPVulkan is not the driver
        guard count > 0 else {
            VQLog.log("Shaders: nada que precompilar (primera vez con este juego o sin VPVulkan)")
            return
        }
        DispatchQueue.main.async { shared.total = count; shared.done = 0; shared.active = true }
        let start = Date()
        let n = vq_prewarm_run({ done, total, _ in
            DispatchQueue.main.async { VQPrewarm.shared.done = done; VQPrewarm.shared.total = total }
        }, nil)
        let ms = Int(Date().timeIntervalSince(start) * 1000)
        DispatchQueue.main.async { shared.active = false }
        VQLog.log("Shaders: precompilados \(n) pipelines en \(ms) ms")
    }

    /// First play of a game (or new translator version): every SPIR-V module in the game's data is
    /// harvested (APK/OBB/Unity bundles) and compiled before the game boots, so no shader compiles
    /// mid-play even the first time. Cached: a stamp file marks it done for this version.
    static func firstRun(gameFolder: URL, cache: URL) {
        let fm = FileManager.default
        // Stamp follows the translator's source fingerprint: any translator change re-runs the precompile.
        var tag = "v\(firstRunVersion)"
        if let sym = dlsym(UnsafeMutableRawPointer(bitPattern: -2), "vpmsl_translator_fingerprint") {
            typealias FP = @convention(c) () -> UnsafePointer<CChar>?
            if let c = unsafeBitCast(sym, to: FP.self)() { tag = "v\(firstRunVersion)." + String(String(cString: c).prefix(16)) }
        }
        let stamp = cache.appendingPathComponent("firstrun.\(tag)")
        if fm.fileExists(atPath: stamp.path) { return }
        // Only with VPVulkan loaded (MoltenVK path has no precompiler): avoids re-scanning every launch.
        guard dlsym(UnsafeMutableRawPointer(bitPattern: -2), "vpvk_precompile_spirv") != nil else {
            VQLog.log("Shaders: sin VPVulkan, no se precompilan")
            return
        }
        try? fm.createDirectory(at: cache, withIntermediateDirectories: true)
        let spirv = cache.appendingPathComponent("spirv", isDirectory: true)
        DispatchQueue.main.async { shared.done = 0; shared.total = 0; shared.compiling = true; shared.active = true }
        var stats = VqHarvestStats()
        var err = [CChar](repeating: 0, count: 256)
        let rc = vq_harvest_spirv(gameFolder.path, spirv.path, &stats, { done, total, _ in
            DispatchQueue.main.async { VQPrewarm.shared.done = done; VQPrewarm.shared.total = total }
        }, nil, { line, _ in
            if let line = line { VQLog.log(String(cString: line)) }
        }, &err, err.count)
        if rc != 0 {
            VQLog.log("Shaders: harvest falló: \(String(cString: err))")
        } else {
            DispatchQueue.main.async { shared.done = 0; shared.total = stats.unique }
            let start = Date()
            let libs = vq_precompile_run(spirv.path, { done, total, _ in
                DispatchQueue.main.async { VQPrewarm.shared.done = done; VQPrewarm.shared.total = total }
            }, nil)
            VQLog.log("Shaders: primera vez: \(stats.unique) módulos SPIR-V, \(libs) librerías Metal en "
                      + "\(Int(Date().timeIntervalSince(start) * 1000)) ms")
            // Only mark done when VPVulkan ran it (libs > 0) or nothing was found.
            if libs > 0 || stats.unique == 0 { fm.createFile(atPath: stamp.path, contents: Data()) }
        }
        DispatchQueue.main.async { shared.compiling = false; shared.active = false }
    }
}
