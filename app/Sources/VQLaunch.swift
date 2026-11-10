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
        // Always QUEST/Cache/<game>/Pipelines when the QUEST folder is available, so recipes, the MSL index
        // and the Metal binary archives survive reinstalling the app (Caches/ is wiped then).
        // Library/Caches only when QUEST is unavailable.
        let appCache = FileManager.default.urls(for: .cachesDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("VisionQuestPro/PipelineCache/\(game.id)", isDirectory: true)
        var psoCache = appCache
        if let q = VQFolder.shared.cache(for: game.id) {
            psoCache = q.appendingPathComponent("Pipelines", isDirectory: true)
            migratePipelineCache(from: appCache, to: psoCache)
        }
        VQLog.log("Shaders: caché de pipelines en \(psoCache.path)")
        // The conformance tests: no Metal binary archive. Their pipelines are one-off (nothing to
        // reuse) and log 60 had every archive save fail with MTLCompilerService unreachable from
        // 2 s into the run while blit libraries failed to compile in the same windows — the
        // archive work is the first suspect for overloading the compiler service.
        if VQConformance.isConformance(game) {
            unsetenv("VP_PIPELINE_CACHE_DIR")
            unsetenv("VPVK_ROBUST")  // the CTS keeps robustBufferAccess in the translated shaders as before
        } else {
            setenv("VP_PIPELINE_CACHE_DIR", psoCache.path, 1)
            // Before the prewarm below: it must see the same VPVK_ROBUST as the game.
            VQRobust.prepare(cache: psoCache)
        }
        // The conformance tests compile their own pipelines; a prewarm of earlier runs' recipes
        // would only slow every vkCreateDevice down (prepareRun turns it off in the driver too).
        if !VQConformance.isConformance(game) { VQPrewarm.run(gameFolder: game.folder, cache: psoCache) }
        VQLanguage.apply(game, useSystem: VQSettings.shared.systemLanguage,
                         fixed: VQSettings.shared.gameLanguage)
        VQOVRPluginVersion.apply(apk: game.apk)
        VQLog.log("Arranque: \(game.id) (\(game.package)) tree=\(choice.prepared.tree.path) dylibs=\(choice.prepared.dylibs.path) "
                  + "files=\(files.path) obb=\(obbRel)")
        VQLog.setGameFiles(files)
        VQApiScan.run(game: game, tree: choice.prepared.tree)
        VQSession.begin(game: game.id)
        VQCompilerProbe.start()   // only once a guest runs: nothing at app launch
        if !VQConformance.isConformance(game) { VQCacheState.start(game: game.id) }   // shader-cache state: HUD line + cache-state.json
        if game.package == VQConformance.openXRPackage { VQConformance.prepareOpenXR() } else { unsetenv("KL_INTENT_EXTRAS"); unsetenv("KL_STATUS_HUD"); unsetenv("KL_XR_STRICT_LAYERS") }
        if VQConformance.isConformance(game) {
            // After applyToEnvironment: the test run is window-only (no immersive space).
            VQConformance.prepareRun(game: game, tree: choice.prepared.tree, saves: files)
        }
        setenv("KL_GUEST_KIND", game.guestKind, 1)
        setenv("KL_GUEST_ENTRY", game.entryLib, 1)
        if game.nativeFuncName.isEmpty { unsetenv("KL_GUEST_FUNC") } else { setenv("KL_GUEST_FUNC", game.nativeFuncName, 1) }
        // What the APK's manifest asks of the room (Meta scene / anchor permissions): decides which ARKit providers runARKit adds, before it runs.
        let manifest = game.manifestText
        let wantsAnchors = manifest.contains("\"com.oculus.permission.USE_ANCHOR_API\"")
        let wantsScene = manifest.contains("\"com.oculus.permission.USE_SCENE\"")
        kl_spatial_set_policy(wantsScene ? 1 : 0, wantsAnchors ? 1 : 0)
        KLGuardian.restore(game: game)   // this game's saved floor / play space, before the guest starts
        let rc = kl_app_configure_game(game.id, choice.prepared.tree.path, game.apk.path, choice.prepared.dylibs.path,
                                       files.path, obbRel, documents)
        VQLog.log("Arranque: kl_app_configure_game -> \(rc): \(String(cString: kl_app_status()))")
        // Re-push the settings now that the driver is loaded and stderr reaches the log
        // (the first push in applyToEnvironment can predate both).
        let fwd = vq_gfx_config_forward()
        VQLog.log("Gráficos: ajustes enviados a VPVulkan \(fwd == 1 ? "sí" : "no (driver no cargado)")")
        return rc
    }

    /// Moves (not copies) an old Library/Caches pipeline cache into the QUEST one: every entry that does not
    /// already exist at the destination (existing ones are skipped), then the old folder is removed when empty.
    static func migratePipelineCache(from old: URL, to new: URL) {
        let fm = FileManager.default
        guard let oldItems = try? fm.contentsOfDirectory(atPath: old.path), !oldItems.isEmpty else { return }
        try? fm.createDirectory(at: new, withIntermediateDirectories: true)
        var moved = 0, skipped = 0
        for name in oldItems {
            let dst = new.appendingPathComponent(name)
            if fm.fileExists(atPath: dst.path) { skipped += 1; continue }
            if (try? fm.moveItem(at: old.appendingPathComponent(name), to: dst)) != nil { moved += 1 } else { skipped += 1 }
        }
        if (try? fm.contentsOfDirectory(atPath: old.path))?.isEmpty ?? false { try? fm.removeItem(at: old) }
        VQLog.log("[pso] migrated \(moved) files, skipped \(skipped)")
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
    // Graphics (VPKEngine). The game's own Quest foveation is always off, and so is the app's own: the setting is
    // hidden and applyToEnvironment forces it off (the properties below are kept only for the stored values).
    enum Foveation: Int, CaseIterable { case off = 0, m2 = 1, m5 = 2, custom = 3 }
    @Published var foveation: Foveation { didSet { save() } }
    @Published var sharpZone: Double { didSet { save() } }      // 0.05...1
    @Published var periphery: Double { didSet { save() } }      // 0.1...1
    @Published var fovHeight: Double { didSet { save() } }      // -0.5...0.5
    /// Global «MSAA» (0 = the game's own, 2 or 4): MSAA VPKEngine forces on the game's 1x scene passes
    /// (VPVK_MSAA_FORCE). Per game it can be overridden (VQGameMSAA).
    @Published var msaaForce: Int { didSet { save() } }
    /// Global «Nitidez (RCAS)» (0...1, 0 = off): the intensity of the sharpening pass.
    /// Per game it can be overridden (VQGameRCAS). Default 1.0. Stored as vq.vpktaaSharp3 (kept so the saved value survives).
    @Published var rcasIntensity: Double { didSet { save() } }
    /// 16x anisotropic filtering on the game's mipmapped linear samplers (VPVulkan).
    /// Forced anisotropic filtering level for material samplers: 0 (off), 2, 4, 8 or 16.
    @Published var anisoLevel: Int { didSet { save() } }
    /// "Diagnóstico de rendimiento": per-pass GPU time lines (gpu.passtime) in the log.
    @Published var perfDiag: Bool { didSet { save() } }
    /// "Diagnóstico de ficheros": every guest file operation and its result in the log (KL_TRACE_FS=1).
    @Published var fsDiag: Bool { didSet { save() } }
    /// «Resolución»: the FINAL per-eye size, 50...200 in whole percent. 100 = what the game gets on a
    /// Quest 3 (2064x2208 px); 200 = the Vision Pro panel's density; linear per axis in between
    /// (KL_OVRP_EYE_BASE=quest3, kl_ovrp_eyesize.h). Per game it can be overridden (VQGameRes).
    @Published var resolutionPct: Double { didSet { save() } }
    /// Head-locked FPS counter in the compositor (KL_FPS_HUD).
    @Published var showFps: Bool { didSet { save() } }
    @Published var aswDisabled: Bool { didSet { save() } }
    /// «Captura de entorno (suelo, sala y profundidad)»: ARKit scans floor, room and depth in every game, for the Guardian
    /// (KL_ENV_CAPTURE). Off: only what the game's manifest asks for. The game still sees only what its manifest declares.
    @Published var envCapture: Bool { didSet { save() } }
    /// «Memoria extendida (intercambio a disco)»: guest swap (kl_gswap.h). On: automatic, the game's large allocations go to
    /// temporary files once the app passes 45 % of its memory limit, so they stop counting towards it. Off: KL_GSWAP=0.
    @Published var guestSwap: Bool { didSet { save() } }
    /// «Definir el guardián antes de cada juego»: the guardian screen (VQGuardianSetupView) stops the start of every game, with the
    /// immersive space already open. Off: the game's saved definition (KLGuardian.restore) or the automatic one is used.
    @Published var guardianAsk: Bool { didSet { save() } }
    /// Global «Perfil Quest 3 cuando el juego lo tenga»: allows the Quest 3 profile (per game it can be overridden, VQGameProfile).
    @Published var quest3Profile: Bool { didSet { save() } }
    @Published var forceVulkan: Bool { didSet { save() } }
    @Published var diagCaptures: Bool { didSet { save() } }
    /// «Incluir capturas»: the .vpcap files of «Captura de frames» (QUEST/capturas) go in the share sheet too. They can
    /// be large, so off by default.
    @Published var includeCaptures: Bool { didSet { save() } }
    /// Vulkan driver: VPKEngine's own (VPVulkan) or MoltenVK (to compare, or if VPKEngine fails).
    @Published var useVPKEngine: Bool { didSet { save() } }
    /// Experimental: GLES games through VPKEngine's own OpenGL ES (vpgles in VPVulkan). Off by default:
    /// while off, GLES-only games stay marked incompatible ("solo Vulkan").
    @Published var glesVPK: Bool { didSet { save() } }
    /// Initial posture for the OVRPlugin eye height (KL_OVRP_POSTURE); auto re-measure always on.
    @Published var posture: String { didSet { save() } }   // "auto" | "seated" | "standing"
    /// «Sentado»: raises the guest's floor/stage space by (standing height − measured eye height) so a seated
    /// user stands at `standHeight` (KL_OVRP_SEATED, KL_OVRP_STAND_HEIGHT). Per game it can be overridden (VQGameSeat).
    @Published var seated: Bool { didSet { save() } }
    @Published var standHeight: Double { didSet { save() } }   // 1.40...2.00 m
    /// Passthrough (KL_PASSTHROUGH): "auto" follows the game, "off" always VR, "on" always mixed.
    @Published var passthrough: String { didSet { save() } }
    /// Run the game in the system language when it ships it, else English (LANG for Klepton's Locale/AConfiguration).
    @Published var systemLanguage: Bool { didSet { save() } }
    /// Let the app (never the games) download each game's cover from the Meta store listing, once.
    @Published var storeCovers: Bool { didSet { save() } }
    /// Fixed game language (LANG), overriding the system one: "auto" | "es_ES" | "es_MX" | "en_US".
    /// Spain and Mexico differ only by region, which games read through java.util.Locale.
    @Published var gameLanguage: String { didSet { save() } }
    /// Compile the game's known shaders/pipelines before it starts (VPVulkan recipe store).
    @Published var precompileShaders: Bool { didSet { save() } }
    /// AOT shaders (VPKEngine VPVK_AOT): the whole game's shaders/pipelines are built into the Metal
    /// binary archive before playing; unseen variants compile in the background without holding frames.
    @Published var aotShaders: Bool { didSet { save() } }
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
        let mf = d.object(forKey: "vq.msaaForce") as? Int ?? 0
        msaaForce = [0, 2, 4].contains(mf) ? mf : 0
        rcasIntensity = d.object(forKey: "vq.vpktaaSharp3") as? Double ?? 1.0
        // Older builds stored a bool (vq.aniso16); off there stays off.
        anisoLevel = d.object(forKey: "vq.anisoLevel") as? Int
            ?? ((d.object(forKey: "vq.aniso16") as? Bool) == false ? 0 : 16)
        perfDiag = d.object(forKey: "vq.perfDiag") as? Bool ?? false
        fsDiag = d.object(forKey: "vq.fsDiag") as? Bool ?? false
        // Older builds stored a base ("original" | "compositor" | "native") and a scale instead.
        if let p = d.object(forKey: "vq.resPct") as? Double, p >= 50, p <= 200 {
            resolutionPct = p.rounded()
        } else {
            let rb = d.string(forKey: "vq.resBase")
                ?? ((d.object(forKey: "vq.resAVP") as? Bool ?? false) ? "compositor" : "original")
            resolutionPct = VQGameRes.migratedPct(base: rb, scale: d.object(forKey: "vq.renderScale") as? Double ?? 1)
        }
        showFps = d.object(forKey: "vq.showFps") as? Bool ?? false
        aswDisabled = d.object(forKey: "vq.aswOff") as? Bool ?? true
        envCapture = d.object(forKey: "vq.envCapture") as? Bool ?? true
        guestSwap = d.object(forKey: "vq.guestSwap") as? Bool ?? true
        guardianAsk = d.object(forKey: "vq.guardianAsk") as? Bool ?? true
        d.removeObject(forKey: "vq.hapticsGain")  // old setting: vibration is calibrated automatically per controller
        quest3Profile = d.object(forKey: "vq.quest3v2") as? Bool ?? true  // global switch of «Perfil Quest 3 cuando el juego lo tenga»
        forceVulkan = d.object(forKey: "vq.forceVulkan") as? Bool ?? true
        diagCaptures = d.object(forKey: "vq.diagCaptures2") as? Bool ?? false
        includeCaptures = d.object(forKey: "vq.includeCaptures") as? Bool ?? false
        // Saved as "vq.vpengine" before the engine was renamed VPKEngine.
        useVPKEngine = (d.object(forKey: "vq.vpkengine") ?? d.object(forKey: "vq.vpengine")) as? Bool ?? true
        glesVPK = d.object(forKey: "vq.glesVPK") as? Bool ?? false
        posture = d.string(forKey: "vq.posture") ?? "auto"
        seated = d.object(forKey: "vq.seated") as? Bool ?? false
        standHeight = min(max(d.object(forKey: "vq.standHeight") as? Double ?? 1.65, 1.40), 2.00)
        if let pt = d.string(forKey: "vq.passthrough"), ["auto", "off", "on"].contains(pt) {
            passthrough = pt
        } else { passthrough = "auto" }
        systemLanguage = d.object(forKey: "vq.sysLang") as? Bool ?? true
        storeCovers = d.object(forKey: "vq.storeCovers") as? Bool ?? true
        gameLanguage = d.string(forKey: "vq.gameLang") ?? "auto"
        precompileShaders = d.object(forKey: "vq.precompile") as? Bool ?? true
        aotShaders = d.object(forKey: "vq.aot") as? Bool ?? false
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
        d.set(msaaForce, forKey: "vq.msaaForce")
        d.set(rcasIntensity, forKey: "vq.vpktaaSharp3")
        d.set(anisoLevel, forKey: "vq.anisoLevel")
        d.set(perfDiag, forKey: "vq.perfDiag")
        d.set(fsDiag, forKey: "vq.fsDiag")
        d.set(resolutionPct, forKey: "vq.resPct")
        d.set(showFps, forKey: "vq.showFps")
        d.set(aswDisabled, forKey: "vq.aswOff")
        d.set(envCapture, forKey: "vq.envCapture")
        d.set(guestSwap, forKey: "vq.guestSwap")
        d.set(guardianAsk, forKey: "vq.guardianAsk")
        d.set(quest3Profile, forKey: "vq.quest3v2")
        d.set(forceVulkan, forKey: "vq.forceVulkan")
        d.set(diagCaptures, forKey: "vq.diagCaptures2")
        d.set(includeCaptures, forKey: "vq.includeCaptures")
        d.set(useVPKEngine, forKey: "vq.vpkengine")
        d.set(glesVPK, forKey: "vq.glesVPK")
        d.set(posture, forKey: "vq.posture")
        d.set(seated, forKey: "vq.seated")
        d.set(standHeight, forKey: "vq.standHeight")
        d.set(passthrough, forKey: "vq.passthrough")
        d.set(systemLanguage, forKey: "vq.sysLang")
        d.set(storeCovers, forKey: "vq.storeCovers")
        d.set(gameLanguage, forKey: "vq.gameLang")
        d.set(precompileShaders, forKey: "vq.precompile")
        d.set(aotShaders, forKey: "vq.aot")
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

    /// Klepton's knobs for these settings (and the chosen game's own MSAA / RCAS).
    func applyToEnvironment(game: VQGame?) {
        setenv("VPVK_QUEST_IDENTITY", "1", 0)  // Quest/Adreno 740 identity, always on (before VPVulkan loads; also covers GLES)
        selectDriver()
        setenv("KL_NET_OFFLINE", offline ? "1" : "0", 1)
        setenv("KL_ENV_CAPTURE", envCapture ? "1" : "0", 1)
        if guestSwap { unsetenv("KL_GSWAP") } else { setenv("KL_GSWAP", "0", 1) }
        setenv("KL_LOG_FILE", guestLog ? "1" : "0", 1)
        setenv("KL_ALARM", String(Int(watchdogSeconds)), 1)
        // The launcher already decided; BootView must boot straight away.
        setenv("KL_AUTOBOOT", "1", 1)
        setenv("KL_IMMERSIVE", "1", 1)
        setenv("KL_VK_SPOOF_ADRENO", "1", 1)  // Quest titles check the GPU identity before choosing Vulkan
        setenv("MVK_CONFIG_FULL_IMAGE_VIEW_SWIZZLE", "1", 1)  // MoltenVK: honour VkComponentMapping (R8 font atlases)
        // Unity player argument (the "unity" launch extra): use Vulkan even when Unity's own
        // device detection would fall back to GLES, which has no driver here.
        // -force-vulkan is only valid when the build ships Vulkan: otherwise Unity fails with
        // "Forced GfxDevice 'Vulkan' was not built from editor" and cannot create its device.
        let unityVulkan = forceVulkan && (game?.guestKind != "unity" || game?.hasVulkan == true)
        if unityVulkan { setenv("KL_UNITY_ARGS", "-force-vulkan", 1) } else { unsetenv("KL_UNITY_ARGS") }
        if let g = game {
            let used = g.hasVulkan ? "Vulkan" : "OpenGL ES"
            VQLog.log("Gráficos: APIs del juego \(g.apiList) → se usa \(used)"
                      + (g.guestKind == "unity" ? (unityVulkan ? " (-force-vulkan)" : " (sin -force-vulkan)") : ""))
        }
        VQLog.log("Gráficos: forzar Vulkan en Unity \(forceVulkan)")
        // VPKEngine's own GLES (Klepton kl_gles.c): needs VPVulkan, so only with the VPKEngine driver.
        let gles = glesVPK && useVPKEngine && VQSettings.vpvulkanPath != nil
        setenv("KL_GLES_VPK", gles ? "1" : "0", 1)
        VQLog.log("Gráficos: OpenGL ES con VPKEngine \(gles)")
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
        // «Captura de frames (diagnóstico)», per game (VQGameCapture): VPKEngine tracks every object from the start
        // (VPVK_CAPTURE=1, more RAM) and writes the .vpcap to QUEST/capturas/<game>/; Klepton's kl_diagcap asks for one
        // when the player holds the chosen buttons in the game.
        let cap = VQGameCapture.load(key: game?.aaKey ?? "")
        if cap.enabled, let g = game, let dir = VQFolder.shared.captures(for: g.id) {
            setenv("VPVK_CAPTURE", "1", 1)
            setenv("VPVK_CAPTURE_DIR", dir.path + "/", 1)
            setenv("KL_DIAGCAP", "1", 1)
            setenv("KL_DIAGCAP_BTN", String(cap.button.rawValue), 1)
            setenv("KL_DIAGCAP_HOLD_MS", String(cap.holdSeconds * 1000), 1)
            VQLog.log("Captura de frames: activada (\(cap.button.title), \(cap.holdSeconds == 0 ? "al instante" : "\(cap.holdSeconds) s")) → \(dir.path)"
                      + (useVPKEngine ? "" : "; solo funciona con VPKEngine"))
        } else {
            for k in ["VPVK_CAPTURE", "VPVK_CAPTURE_DIR", "KL_DIAGCAP", "KL_DIAGCAP_BTN", "KL_DIAGCAP_HOLD_MS"] { unsetenv(k) }
            if cap.enabled { VQLog.log("Captura de frames: pedida, pero sin carpeta QUEST no se activa") }
        }
        // Eye texture size the game is told (Klepton's ovrp side) and headset identity:
        // 11 = ovrpSystemHeadset Meta_Quest_3; off: Klepton's per-game default (Quest 2 / Quest).
        setenv("KL_OVRP_POSTURE", posture, 1)
        setenv("KL_PASSTHROUGH", passthrough, 1)
        VQLog.log("Passthrough: \(passthrough)")
        VQLog.log("Gráficos: postura \(posture)")
        // «Sentado», per game (VQGameSeat) when the game fixes its own, else the global one.
        let seat = VQGameSeat.load(key: game?.aaKey ?? "")
        let effSeated = seat.useGlobal ? seated : seat.seated
        let effStand = seat.useGlobal ? standHeight : seat.height
        setenv("KL_OVRP_SEATED", effSeated ? "1" : "0", 1)
        setenv("KL_OVRP_STAND_HEIGHT", String(format: "%.2f", effStand), 1)
        VQLog.log("Gráficos: modo sentado \(effSeated), altura de pie \(String(format: "%.2f", effStand)) m (\(seat.useGlobal ? "global" : "del juego"))")
        // Per game (VQGameRes) when the game fixes its own, else the global ones.
        let gameKey = game?.aaKey ?? ""
        let res = VQGameRes.load(key: gameKey)
        // The slider is the final eye size: KL_OVRP_EYE_BASE=quest3 + KL_OVRP_EYE_SCALE = percent / 100
        // (kl_ovrp_eyesize.h: 100 = Quest 3, 200 = Vision Pro density; the game's own scale is compensated).
        let effPct = res.useGlobal ? resolutionPct : res.pct
        setenv("KL_OVRP_EYE_SCALE", String(format: "%.2f", effPct / 100), 1)
        setenv("KL_OVRP_EYE_BASE", "quest3", 1)
        VQLog.log("Gráficos: resolución de ojo \(Int(effPct.rounded())) % (\(res.useGlobal ? "global" : "del juego"))")
        // «Área de juego», per game (VQGameArea): the play area Klepton reports as the guardian and the size of the synthetic
        // room (vq_area_resolve clamps it; seated = circle, w = d = its diameter).
        let area = VQGameArea.load(key: gameKey)
        var areaW: Float = 0, areaD: Float = 0
        let areaMode = vq_area_resolve(area.seated ? 1 : 0, Float(area.w), Float(area.d), &areaW, &areaD)
        // Any of these variables makes kl_ovrp skip the room outline of the guardian, so they are exported only when the
        // user chose an area other than the default (free 3 x 3); otherwise they are removed and the room outline rules.
        let areaIsDefault = area == VQGameArea()
        if areaIsDefault {
            for k in ["KL_OVRP_BOUNDARY_MODE", "KL_OVRP_BOUNDARY_W", "KL_OVRP_BOUNDARY_D", "KL_OVRP_BOUNDARY_SIZE"] { unsetenv(k) }
        } else {
            setenv("KL_OVRP_BOUNDARY_MODE", String(areaMode), 1)
            setenv("KL_OVRP_BOUNDARY_W", String(format: "%.2f", areaW), 1)
            setenv("KL_OVRP_BOUNDARY_D", String(format: "%.2f", areaD), 1)
        }
        VQLog.log("[area] game=\(game?.id ?? "?") mode=\(areaMode) w=\(String(format: "%.2f", areaW)) d=\(String(format: "%.2f", areaD)) \(areaIsDefault ? "default: env not exported" : "user area: env exported")")
        // Final eye size from the last run with these settings, pushed BEFORE the guest boots so its
        // first eye layer is built at it (the compositor's first drawable arrives after the guest has
        // started). The compositor pushes the display's request later; the same result changes nothing.
        // The push goes through kl_ovrp's base/scale/cap, so those are neutralised for this one call.
        do {
            let env = { (k: String) -> String in getenv(k).map { String(cString: $0) } ?? "-" }
            let sig = [env("KL_OVRP_EYE_BASE"), env("KL_OVRP_EYE_SCALE"), env("KL_OVRP_EYE_MAX")].joined(separator: "|")
            if let saved = UserDefaults.standard.string(forKey: "vq.eyeSizeFinal"),
               saved.hasPrefix(sig + "="),
               case let dims = saved.dropFirst(sig.count + 1).split(separator: "x"), dims.count == 2,
               let w = Int32(dims[0]), let h = Int32(dims[1]), w > 0, h > 0 {
                let keys = ["KL_OVRP_EYE_BASE", "KL_OVRP_EYE_SCALE", "KL_OVRP_EYE_MAX"]
                let old = keys.map { getenv($0).map { String(cString: $0) } }
                setenv("KL_OVRP_EYE_BASE", "compositor", 1); setenv("KL_OVRP_EYE_SCALE", "1.0", 1); setenv("KL_OVRP_EYE_MAX", "0", 1)
                kl_ovrp_set_eye_texture_size(w, h)
                for (k, v) in zip(keys, old) { if let v { setenv(k, v, 1) } else { unsetenv(k) } }
                VQLog.log("Gráficos: tamaño de ojo \(w)x\(h) empujado antes de arrancar el juego")
            }
        }
        // The headset the game is shown, per game (VQGameProfile; vq_quest3_resolve decides), in every place a title can
        // check it: Android Build (MODEL/DEVICE eureka), OVRPlugin headset type, OpenXR system name and the GPU's name
        // (VPKEngine and Klepton's kl_vulkan.c read VPVK_QUEST_GPU_NAME; limits and ids stay the Quest 3 profile's).
        let profileChoice = VQGameProfile.load(key: gameKey)
        let isQuest3 = vq_quest3_resolve(Int32(profileChoice ?? 0), quest3Profile ? 1 : 0,
                                         game?.devicesDeclared == true ? 1 : 0, game?.quest3 == true ? 1 : 0) != 0
        if isQuest3 {
            setenv("KL_QUEST3", "1", 1)
            setenv("KL_OVRP_HEADSET", "11", 1)
            setenv("KL_XR_SYSTEM_NAME", "Meta Quest 3", 1)
            setenv("VPVK_QUEST_GPU_NAME", "Adreno (TM) 740", 1)
        } else {
            unsetenv("KL_QUEST3"); unsetenv("KL_OVRP_HEADSET"); unsetenv("KL_XR_SYSTEM_NAME")
            setenv("VPVK_QUEST_GPU_NAME", "Adreno (TM) 650", 1)
        }
        VQLog.log("[profile] game=\(game?.id ?? "?") quest3=\(isQuest3 ? 1 : 0) source=\(profileChoice == nil ? "auto" : "override") manifest_quest3=\(game?.quest3 == true ? 1 : 0) global=\(quest3Profile ? 1 : 0)")
        // ASW: Klepton serves SpaceWarp storage (ovrp_GetLayerTextureSpaceWarp, KL_VPKTAA_ASW default on; a game
        // that enables SpaceWarp fails without it), whatever KL_VPKTAA is. The setting is passed on for VPVulkan and
        // reaches the compositor's extrapolation (KL_CP_ASW, behind its own gate) as KL_CP_ASW_OFF.
        setenv("KL_CP_ASW_OFF", aswDisabled ? "1" : "0", 1)
        var c = vq_gfx_config()
        // Own foveation is removed from the app: always off (VPVK_FOVEATION=0 wins over any config call).
        c.foveation_mode = 0
        c.sharp_zone = 0.35
        c.periphery = 0.5
        c.height = 0
        setenv("VPVK_FOVEATION", "0", 1)
        // VPKTAA is removed from the app: never on, so no resolve hook and no frustum jitter (SpaceWarp storage does not depend on it).
        setenv("KL_VPKTAA", "0", 1)
        setenv("VPVK_TAA_NATIVE", "0", 1)
        // MSAA, per game (VQGameMSAA), else the global one: 0 = the game's own, 2 / 4 = forced by VPKEngine on the
        // game's 1x scene passes (a game with its own MSAA keeps it: VPVK_GAME_MSAA_AUTO, default on in the engine).
        // VQ_GAME_KEY: the per-game key, for the logs.
        let effMSAA = VQGameMSAA.load(key: gameKey) ?? msaaForce
        if gameKey.isEmpty { unsetenv("VQ_GAME_KEY") } else { setenv("VQ_GAME_KEY", gameKey, 1) }
        c.msaa_force = Int32(effMSAA)
        setenv("VPVK_MSAA_FORCE", String(effMSAA), 1)  // read by VPVulkan even before the forward
        let perGameMSAA = VQGameMSAA.load(key: gameKey)
        VQLog.log("[msaa] game=\(game?.id ?? "?") global=\(msaaForce) perGame=\(perGameMSAA.map(String.init) ?? "-") effective=\(effMSAA) "
                  + "source=\(effMSAA == 0 ? "game" : (perGameMSAA == nil ? "global" : "perGame")) "
                  + "env=\(getenv("VPVK_MSAA_FORCE").map { String(cString: $0) } ?? "-")")
        // RCAS (the sharpening pass, fused in the compositor, 3D only): per game, else the global one; 0 = off.
        let effRcas = VQGameRCAS.load(key: gameKey) ?? rcasIntensity
        c.rcas = Float(effRcas)
        setenv("KL_RCAS", String(format: "%.2f", effRcas), 1)   // Klepton's compositor and frame-end UI mask
        setenv("VPVK_RCAS", String(format: "%.2f", effRcas), 1)  // VPVulkan, even before the forward
        // Per game (VQGameAniso): -1 = the global level; 0 = the game's own; 2/4/8/16 forced.
        let effAniso = VQGameAniso.load(key: gameKey) ?? anisoLevel
        c.anisotropy = Int32(effAniso)
        setenv("VPVK_ANISO", String(effAniso), 1)  // read by VPVulkan even if the dlsym forward misses
        setenv("VPVK_FORCE_ANISO", String(effAniso), 1)  // VPVulkan: wins over VPVK_ANISO, 0 = the game's own
        VQLog.log("Gráficos: anisotrópico por juego x\(effAniso) (\(VQGameAniso.load(key: gameKey) == nil ? "global" : "del juego"))")
        // «Emular profundidad de 24 bits (experimental)», per game (VQGameD24): off unless the game asks for it.
        let d24 = VQGameD24.load(key: gameKey)
        if d24 { setenv("VPVK_D24_EMUL", "1", 1) } else { unsetenv("VPVK_D24_EMUL") }
        VQLog.log("Gráficos: profundidad de 24 bits emulada \(d24)")
        // Per-pass GPU timing in VPKEngine (gpu.passtime lines, one rendering frame every 90).
        if perfDiag { setenv("VP_PASS_TIMING", "90", 1) } else { unsetenv("VP_PASS_TIMING") }
        if fsDiag { setenv("KL_TRACE_FS", "1", 1) } else { unsetenv("KL_TRACE_FS") }
        unsetenv("KL_OVRP_POSE_LAST")  // build 14's "last sample" variant: measured worse, off
        setenv("KL_FPS_HUD", showFps ? "1" : "0", 1)
        c.render_scale = 1  // eye size comes only from the resolution slider (KL_OVRP_EYE_SCALE)
        c.asw_disabled = aswDisabled ? 1 : 0
        c.quest3_profile = isQuest3 ? 1 : 0
        _ = vq_gfx_config_apply(&c)
        vq_prewarm_set_enabled(precompileShaders ? 1 : 0)
        // Also through the environment: the call above reaches VPVulkan only once it is loaded,
        // and its first vkCreateDevice may come before the app can repeat it.
        setenv("VPVK_PREWARM", precompileShaders ? "1" : "0", 1)
        if aotShaders { setenv("VPVK_AOT", "1", 1) } else { unsetenv("VPVK_AOT") }
        // Games keep going after a single hung or faulting GPU frame (conformance runs set it to 1).
        unsetenv("VPVK_DEVICE_LOST_STRICT")
        // Conformance-only: games never refuse a device for features they enable without using (VQConformance sets it).
        unsetenv("VPVK_STRICT_FEATURES")
        // One Quest 3 profile for games and conformance: VPKEngine ignores VPVK_EXTRA_EXTENSIONS under the Quest identity.
        unsetenv("VPVK_EXTRA_EXTENSIONS")
        // Pipelines: Quest-faithful create. On Quest the driver compiles inside vkCreate*Pipelines, so an engine that counts
        // returned creates as warm (Unreal's PSO precache) must get them ready; asynchronous creation let its loading bar reach
        // 100 % while thousands still compiled (missing effects, low framerate). VPKEngine's own default stays asynchronous (AVP).
        setenv("VPVK_ASYNC_PIPELINES", "0", 1)
        // VPKEngine 454f7cc dead-store elision (heuristic, risks a corrupted frame) — off until the translator marks samples to keep
        unsetenv("VPVK_MSAA_DEAD_STORE")
        // Games: report geometry/tessellation like Adreno so Unity keeps subshaders that need
        // them (AC Nexus's magenta UI). The CTS unsets it (VQConformance).
        setenv("VPVK_REPORT_GEOM", "1", 1)
        // The engine's jitter env also switches off the RCAS UI mask (set_jitter still): games never inherit a 0 from an earlier run.
        unsetenv("VPVK_TAA_JITTER")
        VQLog.log("Gráficos: foveado Quest y propio siempre apagados, MSAA forzado \(effMSAA == 0 ? "el del juego" : "x\(effMSAA)") (juego \(gameKey.isEmpty ? "?" : gameKey), \(VQGameMSAA.load(key: gameKey) == nil ? "global" : "del juego")), Nitidez (RCAS) \(Int(effRcas * 100)) % (\(VQGameRCAS.load(key: gameKey) == nil ? "global" : "del juego")), anisotrópico x\(effAniso), diagnóstico de rendimiento \(perfDiag), diagnóstico de ficheros \(fsDiag), resolución \(Int(effPct.rounded())) %, ASW apagado \(aswDisabled), perfil Quest 3 \(isQuest3), FPS en pantalla \(showFps)")
        VQLog.log("Ajustes: sin red \(offline), log del juego \(guestLog), vigilante \(Int(watchdogSeconds)) s")
    }
}

/// MSAA per game: nil = follow the global one (VQSettings.msaaForce); 0 = the game's own; 2 / 4 = forced by VPKEngine on the
/// game's 1x scene passes (VPVK_MSAA_FORCE; the engine clamps it to what the chip supports and keeps the MSAA a game
/// already has). Saved as vq.msaaForce.<key>.
enum VQGameMSAA {
    static func load(key: String) -> Int? {
        guard !key.isEmpty, let n = UserDefaults.standard.object(forKey: "vq.msaaForce." + key) as? Int,
              [0, 2, 4].contains(n) else { return nil }
        return n
    }

    static func save(_ level: Int?, key: String) {
        guard !key.isEmpty else { return }
        if let level { UserDefaults.standard.set(level, forKey: "vq.msaaForce." + key) }
        else { UserDefaults.standard.removeObject(forKey: "vq.msaaForce." + key) }
    }
}

/// Headset shown to one game: nil = automatic (VQSettings.quest3Profile and the game's manifest decide, see
/// vq_quest3_resolve); 2 = Quest 2; 3 = Quest 3. Saved as vq.quest3Profile.<key>.
enum VQGameProfile {
    static func load(key: String) -> Int? {
        guard !key.isEmpty, let n = UserDefaults.standard.object(forKey: "vq.quest3Profile." + key) as? Int,
              [2, 3].contains(n) else { return nil }
        return n
    }

    static func save(_ choice: Int?, key: String) {
        guard !key.isEmpty else { return }
        if let choice { UserDefaults.standard.set(choice, forKey: "vq.quest3Profile." + key) }
        else { UserDefaults.standard.removeObject(forKey: "vq.quest3Profile." + key) }
    }
}

/// Resolution chosen per game: when `useGlobal` is false, `pct` (50...200, see VQSettings.resolutionPct)
/// replaces the global one. Saved by game key: vq.resMode.<key> ("game"; absent = global), vq.resPct.<key>.
struct VQGameRes: Equatable {
    var useGlobal = true
    var pct = 100.0

    static func load(key: String) -> VQGameRes {
        let d = UserDefaults.standard
        var r = VQGameRes()
        guard !key.isEmpty, d.string(forKey: "vq.resMode." + key) == "game" else { return r }
        r.useGlobal = false
        if let p = d.object(forKey: "vq.resPct." + key) as? Double, p >= 50, p <= 200 {
            r.pct = p.rounded()
        } else {
            // Older builds saved a base and a scale per game.
            r.pct = migratedPct(base: d.string(forKey: "vq.resBase." + key) ?? "original",
                                scale: d.object(forKey: "vq.resScale." + key) as? Double ?? 1)
        }
        return r
    }

    func save(key: String) {
        guard !key.isEmpty else { return }
        let d = UserDefaults.standard
        if useGlobal { d.removeObject(forKey: "vq.resMode." + key) } else { d.set("game", forKey: "vq.resMode." + key) }
        d.set(pct, forKey: "vq.resPct." + key)
    }

    /// The percentage closest to an older base + scale: original x scale -> scale in %, the visionOS
    /// drawable ("compositor") -> 150, the panel density ("native") -> 200.
    static func migratedPct(base: String, scale: Double) -> Double {
        switch base {
        case "native": return 200
        case "compositor": return 150
        default: return min(max((scale * 100).rounded(), 50), 200)
        }
    }

    /// Per-axis factor over the Quest 3 eye for a percentage: p/100 up to 100, then linear to the
    /// Vision Pro panel's density at 200. Mirrors kles_pct_factor (kl_ovrp_eyesize.h); keep in sync.
    static func factor(pct: Double) -> Double {
        let p = min(max(pct, 50), 200)
        if p <= 100 { return p / 100 }
        let avp = (3660.0 * 3200.0 / (2064.0 * 2208.0)).squareRoot()
        return 1 + (p - 100) / 100 * (avp - 1)
    }

    /// Per-eye pixels with the Quest 3's shape (2064x2208 at 100 %). The runtime keeps the same pixel
    /// count but gives it the shape of the headset's field of view (square pixels), so the real
    /// width x height differ; the pixel count is the same.
    static func approxEye(pct: Double) -> (w: Int, h: Int) {
        let f = factor(pct: pct)
        return (Int((2064.0 * f).rounded()), Int((2208.0 * f).rounded()))
    }
}

/// «Área de juego» per game: Libre (roomscale rectangle `w` × `d` m, each 2...6; default 3 × 3) or Sentado (stationary circle).
/// Saved by game key: vq.area.<key> (0 free, 1 seated), vq.areaW.<key>, vq.areaD.<key>.
struct VQGameArea: Equatable {
    var seated = false
    var w = 3.0
    var d = 3.0

    static func load(key: String) -> VQGameArea {
        let d = UserDefaults.standard
        var r = VQGameArea()
        guard !key.isEmpty else { return r }
        r.seated = d.object(forKey: "vq.area." + key) as? Int == 1
        r.w = min(max(d.object(forKey: "vq.areaW." + key) as? Double ?? 3, 2), 6)
        r.d = min(max(d.object(forKey: "vq.areaD." + key) as? Double ?? 3, 2), 6)
        return r
    }

    func save(key: String) {
        guard !key.isEmpty else { return }
        let u = UserDefaults.standard
        u.set(seated ? 1 : 0, forKey: "vq.area." + key)
        u.set(w, forKey: "vq.areaW." + key)
        u.set(d, forKey: "vq.areaD." + key)
    }
}

/// «Sentado» per game: when `useGlobal` is false, `seated` and `height` (1.40...2.00 m) replace the global ones.
/// Saved by game key: vq.seatMode.<key> ("game"; absent = global), vq.seatOn.<key>, vq.seatH.<key>.
struct VQGameSeat: Equatable {
    var useGlobal = true
    var seated = false
    var height = 1.65

    static func load(key: String) -> VQGameSeat {
        let d = UserDefaults.standard
        var r = VQGameSeat()
        guard !key.isEmpty, d.string(forKey: "vq.seatMode." + key) == "game" else { return r }
        r.useGlobal = false
        r.seated = d.object(forKey: "vq.seatOn." + key) as? Bool ?? false
        r.height = min(max(d.object(forKey: "vq.seatH." + key) as? Double ?? 1.65, 1.40), 2.00)
        return r
    }

    func save(key: String) {
        guard !key.isEmpty else { return }
        let d = UserDefaults.standard
        if useGlobal { d.removeObject(forKey: "vq.seatMode." + key) } else { d.set("game", forKey: "vq.seatMode." + key) }
        d.set(seated, forKey: "vq.seatOn." + key)
        d.set(height, forKey: "vq.seatH." + key)
    }
}

/// «Emular profundidad de 24 bits (experimental)» per game, off by default (VPVK_D24_EMUL). Saved as vq.d24emul.<key>.
enum VQGameD24 {
    static func load(key: String) -> Bool {
        !key.isEmpty && UserDefaults.standard.object(forKey: "vq.d24emul." + key) as? Bool == true
    }

    static func save(_ on: Bool, key: String) {
        guard !key.isEmpty else { return }
        if on { UserDefaults.standard.set(true, forKey: "vq.d24emul." + key) }
        else { UserDefaults.standard.removeObject(forKey: "vq.d24emul." + key) }
    }
}

/// Anisotropic filtering per game: nil = follow the global level; 0 = the game's own;
/// 2/4/8/16 forced on its material samplers. Saved as vq.aniso.<key>.
enum VQGameAniso {
    static func load(key: String) -> Int? {
        guard !key.isEmpty, let n = UserDefaults.standard.object(forKey: "vq.aniso." + key) as? Int,
              [0, 2, 4, 8, 16].contains(n) else { return nil }
        return n
    }

    static func save(_ level: Int?, key: String) {
        guard !key.isEmpty else { return }
        if let level { UserDefaults.standard.set(level, forKey: "vq.aniso." + key) }
        else { UserDefaults.standard.removeObject(forKey: "vq.aniso." + key) }
    }
}

/// «Nitidez (RCAS)» per game: nil = follow the global value (VQSettings.rcasIntensity); else 0...1
/// (0 = off). Saved as vq.rcas.<key>.
enum VQGameRCAS {
    static func load(key: String) -> Double? {
        guard !key.isEmpty, let v = UserDefaults.standard.object(forKey: "vq.rcas." + key) as? Double else { return nil }
        return min(max(v, 0), 1)
    }

    static func save(_ value: Double?, key: String) {
        guard !key.isEmpty else { return }
        if let value { UserDefaults.standard.set(min(max(value, 0), 1), forKey: "vq.rcas." + key) }
        else { UserDefaults.standard.removeObject(forKey: "vq.rcas." + key) }
    }
}

/// «Captura de frames (diagnóstico)» per game, off by default (VPKEngine's .vpcap capture, needs object tracking from the
/// start: more RAM). `button` is the combination pressed in the game to capture the next frame (held `holdSeconds`, 0 =
/// at the press itself); its raw value is KL_DIAGCAP_BTN (kl_diagcap.h). Saved by game key: vq.cap.on.<key>,
/// vq.cap.btn.<key>, vq.cap.hold.<key> (the old vq.cap.sec.<key> defaulted to 3 s and is no longer read).
struct VQGameCapture: Equatable {
    enum Combo: Int, CaseIterable {
        case stickL = 0, stickR = 1, sticks = 2, triggers = 3
        var title: String {
            switch self {
            case .stickL: return VQL("Stick izquierdo (pulsado)", "Left stick (clicked)")
            case .stickR: return VQL("Stick derecho (pulsado)", "Right stick (clicked)")
            case .sticks: return VQL("Los dos sticks (pulsados)", "Both sticks (clicked)")
            case .triggers: return VQL("Los dos gatillos / pellizcos", "Both triggers / pinches")
            }
        }
    }
    var enabled = false
    var button = Combo.stickL
    var holdSeconds = 0   // 0...10

    static func load(key: String) -> VQGameCapture {
        let d = UserDefaults.standard
        var r = VQGameCapture()
        guard !key.isEmpty else { return r }
        r.enabled = d.object(forKey: "vq.cap.on." + key) as? Bool ?? false
        r.button = Combo(rawValue: d.object(forKey: "vq.cap.btn." + key) as? Int ?? 0) ?? .stickL
        r.holdSeconds = min(max(d.object(forKey: "vq.cap.hold." + key) as? Int ?? 0, 0), 10)
        return r
    }

    func save(key: String) {
        guard !key.isEmpty else { return }
        let d = UserDefaults.standard
        d.set(enabled, forKey: "vq.cap.on." + key)
        d.set(button.rawValue, forKey: "vq.cap.btn." + key)
        d.set(holdSeconds, forKey: "vq.cap.hold." + key)
    }
}

/// Game language: the system language if the game ships it (APK entries or data files name it),
/// otherwise English. Klepton answers java.util.Locale and AConfiguration from LANG.
enum VQLanguage {
    static func apply(_ game: VQGame, useSystem: Bool, fixed: String = "auto") {
        if fixed != "auto", !fixed.isEmpty {
            setenv("LANG", fixed + ".UTF-8", 1)
            VQLog.log("Idioma: \(fixed) (elegido en Ajustes)")
            return
        }
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
        } else if let hit = match(code, game.contentNames) ?? matchCode(code, apk: game.apk) {
            lang = code + "_" + (region.isEmpty ? code.uppercased() : region)
            reason = "el juego lo incluye (\(hit))"
        } else {
            reason = "el juego no incluye '\(code)'"
        }
        setenv("LANG", lang + ".UTF-8", 1)
        VQLog.log("Idioma: \(lang) (\(reason))")
    }

    /// The game's own CODE names the language: an il2cpp identifier that carries the English
    /// language name plus more (WwiseLanguageSpanish, OculusPackageSpanish, SpanishSubtitles).
    /// A bare "Spanish" does not count: that is Unity's SystemLanguage enum, present in every
    /// game. Needed for titles whose text is internal and whose voice packs are downloadable
    /// (AC Nexus: only en.lang on disk, Spanish UI in the code, LanguagePack/AssetFile for audio).
    static func matchCode(_ code: String, apk: URL) -> String? {
        guard let en = Locale(identifier: "en").localizedString(forLanguageCode: code), en.count > 3 else { return nil }
        var err = [CChar](repeating: 0, count: 256)
        guard let zip = vq_zip_open(apk.path, &err, err.count) else { return nil }
        defer { vq_zip_close(zip) }
        let i = vq_zip_find(zip, "assets/bin/Data/Managed/Metadata/global-metadata.dat")
        var len = 0
        guard i >= 0, let raw = vq_zip_read(zip, i, 256 << 20, &len, &err, err.count) else { return nil }
        defer { free(raw) }
        let needle = Array(en.utf8)
        let bytes = UnsafeBufferPointer(start: raw, count: len)
        var start = 0
        for j in 0...len {
            if j == len || bytes[j] == 0 {
                let n = j - start
                if n > needle.count && n < 80 {
                    let tok = bytes[start..<j]
                    if tok.allSatisfy({ ($0 >= 65 && $0 <= 90) || ($0 >= 97 && $0 <= 122) || ($0 >= 48 && $0 <= 57) || $0 == 95 }),
                       tok.firstRange(of: needle) != nil {
                        return String(decoding: tok, as: UTF8.self)
                    }
                }
                start = j + 1
            }
        }
        return nil
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

/// robustBufferAccess per game. VPKEngine (55506b0) bakes it into the translated MSL and the recipe key: VPVK_ROBUST unset = on,
/// "0" = off, "app" = what the game enables at vkCreateDevice (on while no device exists, i.e. in the prewarm). Klepton writes what
/// the game asked for to <cache>/vq.robust ("0"/"1", kl_vulkan.c); with it known the launch gives "0" to a game that does not
/// ask (prewarm and game both translate without the checks), else "app" (the prewarm's "on" is the game's own request).
enum VQRobust {
    static let learnedFile = "vq.robust"
    static let translatorFile = "vq.translator"

    /// nil = not learned yet (first run of the game, or the cache was wiped).
    static func learned(cache: URL) -> Bool? {
        guard let t = try? String(contentsOf: cache.appendingPathComponent(learnedFile), encoding: .utf8) else { return nil }
        switch t.trimmingCharacters(in: .whitespacesAndNewlines) {
        case "1": return true
        case "0": return false
        default: return nil
        }
    }

    /// Value of VPVK_ROBUST for a game with this learned data.
    static func envValue(learned: Bool?) -> String { learned == false ? "0" : "app" }

    /// VPVK_ROBUST as set for this run ("0", "app" or "default"): part of the warm stamps, so a value learned after the first
    /// session repeats the precompilation with the right one.
    static var stampTag: String {
        guard let e = getenv("VPVK_ROBUST") else { return "rbdefault" }
        return "rb" + String(cString: e)
    }

    /// Whenever the translator version (vpvk_translator_tag) differs from the one stored in <cache>/vq.translator, or the
    /// file is missing (also caches of earlier builds, made without the VPVK_ROBUST key): the Metal archives and the
    /// warm stamps go (the next launch rebuilds them). recipes.bin is kept when the stamp exists and differs: it stores
    /// the SPIR-V and the options of every stage, and the prewarm's RecipeStore::refresh() retranslates each record whose
    /// translator id is not the current one (and drops the pipelines whose stage no longer translates), so a translator
    /// bump does not lose the preload. Without a stamp nothing proves the recipes carry the VPVK_ROBUST key: they go too.
    /// The new tag is written only when every removal succeeded (else it retries next launch). nil = same translator,
    /// nothing to do.
    static func purgeOnTranslatorChange(cache: URL, tag: String) -> (old: String?, removed: Int, failed: [String], sealed: Bool)? {
        let fm = FileManager.default
        let stamp = cache.appendingPathComponent(translatorFile)
        let old = (try? String(contentsOf: stamp, encoding: .utf8))?.trimmingCharacters(in: .whitespacesAndNewlines)
        if old == tag { return nil }
        var removed = 0
        var failed: [String] = []
        for f in (try? fm.contentsOfDirectory(atPath: cache.path)) ?? [] {
            let archive = f.hasPrefix("pipelines") && (f.hasSuffix(".metalar") || f.hasSuffix(".keys"))
            let warm = f.hasPrefix("firstrun.") || f.hasPrefix("aot.")
            let recipes = f == "recipes.bin" && (old ?? "").isEmpty  // with a stamp: kept (retranslated by the prewarm)
            if archive || warm || f == "pipelines.count" || recipes {
                do { try fm.removeItem(at: cache.appendingPathComponent(f)); removed += 1 } catch { failed.append("\(f): \(error.localizedDescription)") }
            }
        }
        if !failed.isEmpty { return (old, removed, failed, false) }
        try? fm.createDirectory(at: cache, withIntermediateDirectories: true)
        return (old, removed, failed, fm.createFile(atPath: stamp.path, contents: Data(tag.utf8)))
    }

    static func prepare(cache: URL) {
        if let tag = VQPrewarm.translatorTag() {
            if let r = purgeOnTranslatorChange(cache: cache, tag: tag) {
                VQLog.log("Shaders: traductor \(r.old ?? "sin sello") → \(tag), \(r.removed) ficheros borrados"
                          + (r.sealed ? "" : ", SIN sellar (se reintenta en el próximo arranque)"))
                for f in r.failed { VQLog.log("Shaders: no se pudo borrar \(f)") }
            }
        } else {
            VQLog.log("Shaders: sin vpvk_translator_tag (VPVulkan no cargado o antiguo), caché sin purgar")
        }
        let l = learned(cache: cache)
        let v = envValue(learned: l)
        setenv("VPVK_ROBUST", v, 1)
        let shown = l == nil ? "sin aprender" : (l == true ? "1" : "0")
        VQLog.log("Shaders: robustBufferAccess del juego \(shown) -> VPVK_ROBUST=\(v)")
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
    @Published var compiling = false  // harvested SPIR-V or stored recipes being compiled ("Compilando shaders")
    @Published var saving = false  // the pipeline archive being written after the recipes ("Guardando caché X/Y")

    /// Bump when the harvest or the SPIR-V -> MSL translator changes: the first-run precompile repeats.
    static let firstRunVersion = 2  // 2: VPKEngine per-variable private slots (new MSL)

    static func run(gameFolder: URL, cache: URL) {
        guard VQSettings.shared.precompileShaders else {
            VQLog.log("Shaders: precompilación desactivada en ajustes")
            return
        }
        if VQSettings.shared.aotShaders {
            aot(gameFolder: gameFolder, cache: cache)
            return
        }
        firstRun(gameFolder: gameFolder, cache: cache)
        let count = vq_prewarm_count()  // 0 as well when VPVulkan is not the driver
        guard count > 0 else {
            VQLog.log("Shaders: nada que precompilar (primera vez con este juego o sin VPVulkan)")
            return
        }
        // Blocking: the game does not boot until vq_prewarm_run returns. "Compilando N / M" on every run (the engine reports
        // no hit/miss count, so a run with misses cannot be told apart beforehand).
        DispatchQueue.main.async { shared.total = count; shared.done = 0; shared.compiling = true; shared.active = true }
        let start = Date()
        let n = vq_prewarm_run({ done, total, _ in
            // After the recipes the engine writes the binary archive (minutes on a first run) and counts entries archived.
            let saving = vq_prewarm_phase() == 1
            DispatchQueue.main.async {
                VQPrewarm.shared.saving = saving
                VQPrewarm.shared.done = done; VQPrewarm.shared.total = total
            }
        }, nil)
        let ms = Int(Date().timeIntervalSince(start) * 1000)
        DispatchQueue.main.async { shared.compiling = false; shared.saving = false; shared.active = false }
        VQLog.log("Shaders: precompilados \(n) pipelines en \(ms) ms")
    }

    /// Version of the translator's output (vpvk_translator_tag), not of its sources: it changes only when a
    /// component changes what it produces. An older VPVulkan without it falls back to the source fingerprint.
    static func translatorTag() -> String? {
        typealias FP = @convention(c) () -> UnsafePointer<CChar>?
        for name in ["vpvk_translator_tag", "vpmsl_translator_fingerprint"] {
            if let sym = dlsym(UnsafeMutableRawPointer(bitPattern: -2), name),
               let c = unsafeBitCast(sym, to: FP.self)() { return String(String(cString: c).prefix(32)) }
        }
        return nil
    }

    /// AOT (setting "Shaders AOT", VPVK_AOT=1): harvest the game's SPIR-V once, then translate it
    /// all and compile every known pipeline (recipes of earlier runs + compute variants) into the
    /// game's MTLBinaryArchive. Repeats only when the translator or the number of recipes changes
    /// (a run that met new variants adds recipes, so the next launch archives them too).
    static func aot(gameFolder: URL, cache: URL) {
        let fm = FileManager.default
        var tag = "v\(firstRunVersion)"
        if let c = translatorTag() { tag += "." + c }
        guard dlsym(UnsafeMutableRawPointer(bitPattern: -2), "vpvk_aot_build") != nil else {
            VQLog.log("Shaders AOT: VPVulkan sin vpvk_aot_build")
            return
        }
        try? fm.createDirectory(at: cache, withIntermediateDirectories: true)
        let recipes = vq_prewarm_count()
        let stamp = cache.appendingPathComponent("aot.\(tag).r\(recipes).\(VQRobust.stampTag)")
        if fm.fileExists(atPath: stamp.path) {
            VQLog.log("Shaders AOT: archivo al día (\(recipes) recetas), se carga al crear el dispositivo")
            return
        }
        let spirv = cache.appendingPathComponent("spirv", isDirectory: true)
        DispatchQueue.main.async { shared.done = 0; shared.total = 0; shared.compiling = true; shared.active = true }
        if !fm.fileExists(atPath: spirv.path) {
            var stats = VqHarvestStats()
            var err = [CChar](repeating: 0, count: 256)
            let rc = vq_harvest_spirv(gameFolder.path, spirv.path, &stats, { done, total, _ in
                DispatchQueue.main.async { VQPrewarm.shared.done = done; VQPrewarm.shared.total = total }
            }, nil, { line, _ in
                if let line = line { VQLog.log(String(cString: line)) }
            }, &err, err.count)
            if rc != 0 { VQLog.log("Shaders AOT: harvest falló: \(String(cString: err))") }
        }
        let start = Date()
        let n = vq_aot_run(fm.fileExists(atPath: spirv.path) ? spirv.path : nil, { done, total, _ in
            DispatchQueue.main.async { VQPrewarm.shared.done = done; VQPrewarm.shared.total = total }
        }, nil)
        VQLog.log("Shaders AOT: \(n) librerías+pipelines en \(Int(Date().timeIntervalSince(start) * 1000)) ms")
        if n > 0 {
            if let old = try? fm.contentsOfDirectory(atPath: cache.path) {
                for f in old where f.hasPrefix("aot.v") { try? fm.removeItem(at: cache.appendingPathComponent(f)) }
            }
            fm.createFile(atPath: stamp.path, contents: Data())
        }
        DispatchQueue.main.async { shared.compiling = false; shared.active = false }
    }

    /// First play of a game (or new translator version): every SPIR-V module in the game's data is
    /// harvested (APK/OBB/Unity bundles) and compiled before the game boots, so no shader compiles
    /// mid-play even the first time. Cached: a stamp file marks it done for this version.
    static func firstRun(gameFolder: URL, cache: URL) {
        let fm = FileManager.default
        // Stamp follows the translator's source fingerprint: any translator change re-runs the precompile.
        var tag = "v\(firstRunVersion)"
        if let c = translatorTag() { tag = "v\(firstRunVersion)." + c }
        let stamp = cache.appendingPathComponent("firstrun.\(tag).\(VQRobust.stampTag)")
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
