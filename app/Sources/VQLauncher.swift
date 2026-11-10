// VisionQuestPro: the launcher window. QUEST folder, a card per game, the preparation with its
// progress, settings (certificate, options) and the logs to share. Once a game is prepared and
// started, the window becomes Klepton's BootView, which boots it and opens the immersive space.

import Metal
import SwiftUI
import UniformTypeIdentifiers

@MainActor
final class VQModel: ObservableObject {
    static let shared = VQModel()

    @Published var folderPath: String?
    @Published var games: [VQGame] = []
    @Published var message: String?
    @Published var busy: String?          // the game being prepared
    @Published var progress: Double = 0
    @Published var progressText = ""
    @Published var running = false        // BootView is showing
    @Published var ended = false { didSet { if ended { VQSession.end() } }}   // the game's immersive space has closed
    @Published var endMessage: String?    // why it closed when the engine asked to quit
    @Published var certificate: String?   // filled off the main thread (the keychain / PKCS#12 import is slow)
    @Published var shareable: [URL] = []  // what the share buttons offer, worked out off the main thread (disk)
    @Published var appLogTail = ""        // the end of app.txt for Settings, read off the main thread
    @Published var scanning = false       // the QUEST folder is being read (off the main thread)
    @Published var prepared: [String: Bool] = [:]   // game id -> converted and up to date; absent until worked out off the main thread
    @Published var cacheInfo: [String: VQCacheState.Info] = [:]   // game id -> shader cache state (cache-state.json); absent: no file / not worked out yet
    private var refreshes = 0             // generation of the last refresh(); an older scan that ends late is dropped
    private var logRefreshes = 0          // same, for refreshLogs()
    private var certificateGen = 0        // bumped by every certificate change; an older read that ends late is dropped
    private let certificateQueue = DispatchQueue(label: "vq.certificate", qos: .userInitiated)   // keychain / PKCS#12, one at a time

    private init() {
        VQLog.start()
        VQSession.startup()
        refresh()
        refreshLogs()
        let generation = certificateGen
        certificateQueue.async {
            let current = VQSigner.current
            Task { @MainActor in
                guard generation == self.certificateGen else { return }
                self.certificate = current
            }
        }
        // Off the main thread and well after the UI is up: copying the previous session's logs
        // into the QUEST folder (File Provider storage, tens of MB) must never hold up the launch.
        DispatchQueue.global(qos: .utility).asyncAfter(deadline: .now() + 10) {
            VQLog.savePreviousSessionToQuest(atStartup: true)
        }
    }

    /// Reads the QUEST folder and its games off the main thread (File Provider storage: opening every
    /// APK takes seconds) and publishes the result on the main one.
    func refresh() {
        scanning = true
        refreshes += 1
        let generation = refreshes
        DispatchQueue.global(qos: .userInitiated).async {
            let path = VQFolder.shared.url?.path
            let found = path == nil ? [] : VQGames.scan()
            Task { @MainActor in
                guard generation == self.refreshes else { return }
                self.folderPath = path
                self.games = found
                self.scanning = false
                self.updatePrepared(found)
                VQApiScan.inventoryAll(found)
            }
        }
    }

    /// Whether each game is converted and up to date (reads in File Provider storage), worked out off
    /// the main thread: the cards only read `prepared`, they never touch the storage themselves.
    func updatePrepared(_ list: [VQGame]) {
        DispatchQueue.global(qos: .userInitiated).async {
            for game in list {
                let ready = VQPrepare.isPrepared(game)
                Task { @MainActor in
                    if self.prepared[game.id] != ready { self.prepared[game.id] = ready }
                }
                let cache = VQCacheState.info(game: game.id)
                Task { @MainActor in
                    if self.cacheInfo[game.id] != cache { self.cacheInfo[game.id] = cache }
                }
            }
        }
    }

    /// Works out the share list and the log tail off the main thread (they stat / read files) and
    /// publishes them on the main one: the views only read `shareable` and `appLogTail`.
    func refreshLogs() {
        logRefreshes += 1
        let generation = logRefreshes
        DispatchQueue.global(qos: .utility).async {
            let items = VQLog.shareable
            let tail = VQLog.tail(VQLog.appLog)
            Task { @MainActor in
                guard generation == self.logRefreshes else { return }
                if self.shareable != items { self.shareable = items }
                if self.appLogTail != tail { self.appLogTail = tail }
            }
        }
    }

    /// Stores the .p12 and reads the certificate back off the main thread (keychain / PKCS#12);
    /// `done` runs on the main one with the text to show and whether it worked.
    func storeCertificate(p12: Data, password: String, done: @escaping @MainActor (String, Bool) -> Void) {
        certificateGen += 1
        let generation = certificateGen
        certificateQueue.async {
            do {
                let text = try VQSigner.store(p12: p12, password: password)
                let current = VQSigner.current
                Task { @MainActor in
                    if generation == self.certificateGen { self.certificate = current }
                    done(text, true)
                }
            } catch {
                let text = error.localizedDescription
                Task { @MainActor in done(text, false) }
            }
        }
    }

    func removeCertificate() {
        certificateGen += 1
        certificate = nil
        certificateQueue.async { VQSigner.remove() }
    }

    func choose(_ url: URL) {
        if let problem = VQFolder.shared.choose(url) { message = problem } else { message = nil }
        refresh()
    }

    func play(_ game: VQGame) {
        VQLog.log("Jugar: pulsado \(game.id) busy=\(busy ?? "-") running=\(running) ended=\(ended) compatible=\(game.compatible)")
        guard busy == nil else {
            VQLog.log("Jugar: ignorado, ya se prepara \(busy ?? "-")")
            message = VQL("Preparando \(busy ?? "otro juego")… espera a que termine.", "Preparing \(busy ?? "another game")… wait for it to finish.")
            return
        }
        guard game.compatible else {
            VQLog.log("Jugar: ignorado, \(game.id) no es compatible")
            return
        }
        busy = game.id
        progress = 0
        progressText = VQL("Preparando…", "Preparing…")
        message = nil
        Thread.detachNewThread {
            // The keychain / PKCS#12 read is off the main thread; no certificate = same message as before.
            guard VQSigner.current != nil else {
                let text = VQSigner.Problem.missing.errorDescription
                Task { @MainActor in
                    self.busy = nil
                    self.message = text
                }
                return
            }
            // The previous game's logs, BEFORE this one starts and overwrites them. The delayed
            // copy in init() fired 10 s after launch, i.e. inside the NEW session when a game was
            // started quickly: QUEST/Registros then held 10 s of the new run instead of the old
            // one (2026-10-15). Guarded by the boot log's date, so it never copies twice.
            // On its own thread (File Provider storage can be slow): preparing does not wait for
            // it, and the game starts after it ends or after 60 s, whichever comes first.
            let logsSaved = DispatchSemaphore(value: 0)
            Thread.detachNewThread {
                VQLog.savePreviousSessionToQuest()
                VQLog.archiveStaleCrashLogs()
                logsSaved.signal()
                Task { @MainActor in self.refreshLogs() }
            }
            do {
                let prepared = try VQPrepare.prepare(game) { fraction, text in
                    Task { @MainActor in
                        self.progress = fraction
                        self.progressText = text
                    }
                }
                // The game must not start until the copy has ended: the guest opens klepton-boot.log
                // for writing (truncating it) and archiveStaleCrashLogs could move the NEW game's
                // crash report. Long cap, background thread only; busy stays set meanwhile.
                if logsSaved.wait(timeout: .now()) == .timedOut {
                    let saving = VQL("Guardando registro anterior…", "Saving previous log…")
                    Task { @MainActor in self.message = saving }
                    if logsSaved.wait(timeout: .now() + 60) == .timedOut {
                        VQLog.log("Registros: la copia del registro anterior no terminó en 60 s; se inicia el juego")
                    }
                }
                Task { @MainActor in
                    VQLaunch.choice = VQLaunch.Choice(game: game, prepared: prepared)
                    self.prepared[game.id] = true
                    self.message = nil
                    self.busy = nil
                    self.running = true
                    self.refreshLogs()
                }
            } catch {
                VQLog.log("Preparar: \(game.id) falló: \(error.localizedDescription)")
                let text = error.localizedDescription
                Task { @MainActor in
                    self.busy = nil
                    self.message = text
                    self.updatePrepared([game])
                    self.refreshLogs()
                }
            }
        }
    }
}

/// The launcher window's scene id (dismissed while the game's immersive space is open).
enum VQMainWindow {
    static let id = "main"
}

struct VQRootView: View {
    @ObservedObject private var model = VQModel.shared

    var body: some View {
        if model.ended {
            VQEndedView()
        } else if model.running {
            BootView()
        } else {
            VQLauncherView()
        }
    }
}

struct VQLauncherView: View {
    @ObservedObject private var model = VQModel.shared
    @State private var pickingFolder = false
    @State private var showSettings = false

    var body: some View {
        NavigationStack {
            Group {
                if model.scanning && model.games.isEmpty {
                    ProgressView()
                } else if model.folderPath == nil {
                    noFolder
                } else if model.games.isEmpty {
                    ContentUnavailableView(VQL("No hay juegos", "No games"), systemImage: "shippingbox",
                                           description: Text(VQL("Copia cada juego (su .apk y su carpeta de datos) en su propia carpeta dentro de QUEST/Juegos.",
                                                                 "Copy each game (its .apk and its data folder) into its own folder inside QUEST/Juegos.")))
                } else {
                    ScrollView {
                        Text(VQL("Si un juego tiene caché de shaders, deja que complete al 100 %, sal del juego y vuelve a entrar.",
                                 "If a game has a shader cache, let it reach 100 %, quit the game and enter again."))
                            .font(.headline.bold())
                            .multilineTextAlignment(.center)
                            .padding(.horizontal, 24)
                            .padding(.top, 20)
                        LazyVGrid(columns: [GridItem(.adaptive(minimum: 360), spacing: 20)], spacing: 20) {
                            ForEach(model.games) { game in VQGameCard(game: game) }
                        }
                        .padding(24)
                    }
                }
            }
            .safeAreaInset(edge: .bottom) {
                if let message = model.message {
                    Text(message).font(.callout).foregroundStyle(.orange).padding().glassBackgroundEffect()
                }
            }
            .navigationTitle("VisionQuestPro")
            .toolbar {
                ToolbarItemGroup(placement: .topBarTrailing) {
                    Button { model.refresh() } label: { Label(VQL("Actualizar", "Refresh"), systemImage: "arrow.clockwise") }
                    ShareLink(items: model.shareable) { Label(VQL("Registros", "Logs"), systemImage: "doc.text") }
                    Button { showSettings = true } label: { Label(VQL("Ajustes", "Settings"), systemImage: "gearshape") }
                }
            }
            .onAppear { model.refreshLogs() }
            .sheet(isPresented: $showSettings) { VQSettingsView() }
            .fileImporter(isPresented: $pickingFolder, allowedContentTypes: [.folder]) { result in
                if case .success(let url) = result { model.choose(url) }
            }
        }
    }

    private var noFolder: some View {
        VStack(spacing: 16) {
            Image(systemName: "folder.badge.questionmark").font(.system(size: 56))
            Text(VQL("Elige la carpeta QUEST", "Choose the QUEST folder")).font(.title)
            Text(VQL("Créala una vez en «En mi Apple Vision Pro» (app Archivos). Dentro irán Juegos, Cache y Partidas, fuera de la app: no se pierden al reinstalarla.",
                     "Make it once in “On My Apple Vision Pro” (Files app). Juegos, Cache and Partidas go inside it, outside the app: they survive reinstalling it."))
                .multilineTextAlignment(.center).frame(maxWidth: 560).foregroundStyle(.secondary)
            Button(VQL("Elegir carpeta", "Choose folder")) { pickingFolder = true }.buttonStyle(.borderedProminent)
        }
        .padding(40)
    }
}

struct VQGameCard: View {
    let game: VQGame
    @ObservedObject private var model = VQModel.shared
    @State private var showGameSettings = false

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            VQCoverView(game: game)
            Text(game.name).font(.title2.bold()).lineLimit(2)
            if let cache = model.cacheInfo[game.id] {
                Text(cacheText(cache))
                    .font(.caption).foregroundStyle(cache.complete ? Color.green : Color.orange)
            }
            Text("\(game.package) · v\(game.version)").font(.caption).foregroundStyle(.secondary)
            HStack(spacing: 8) {
                tag(game.engine, "cpu")
                tag(game.graphics, "cube")
                if game.quest3 { tag("Quest 3", "visionpro") }
                if !game.obbs.isEmpty { tag("OBB", "archivebox") }
            }
            if !game.compatible {
                ForEach(game.problems, id: \.self) { p in
                    Label(p, systemImage: "exclamationmark.triangle.fill").font(.callout).foregroundStyle(.orange)
                }
            }
            if model.busy == game.id {
                ProgressView(value: model.progress) { Text(model.progressText).font(.caption) }
            } else {
                HStack(spacing: 12) {
                    Button {
                        model.play(game)
                    } label: {
                        Label(model.prepared[game.id] == true ? VQL("Jugar", "Play") : VQL("Preparar y jugar", "Prepare and play"),
                              systemImage: "play.fill")
                    }
                    .buttonStyle(.borderedProminent)
                    .disabled(!game.compatible)
                    Button {
                        VQLog.log("Ajustes del juego: abrir \(game.id) busy=\(model.busy ?? "-") running=\(model.running)")
                        showGameSettings = true
                    } label: {
                        Label(VQL("Ajustes del juego", "Game settings"), systemImage: "slider.horizontal.3")
                    }
                    .buttonStyle(.bordered)
                }
            }
        }
        .padding(20)
        .frame(maxWidth: .infinity, alignment: .leading)
        .glassBackgroundEffect()
        .sheet(isPresented: $showGameSettings) { VQGameSettingsView(game: game) }
    }

    /// «requested» = pipelines the game asked for (served from the cache or compiled); «compiled» (the text says new recipes) = recipes added in the last session, not real compilations.
    private func cacheText(_ c: VQCacheState.Info) -> String {
        let compiled = c.new.map { VQL(", \($0) recetas nuevas en la última sesión", ", \($0) new recipes in the last session") } ?? ""
        if c.complete {
            return VQL("Caché estable (sin pipelines nuevos en esta sesión; \(c.requested) pedidos\(compiled))",
                       "Cache stable (no new pipelines this session; \(c.requested) requested\(compiled))")
        }
        if c.new == 0 {   // nothing new was compiled: playing more would not change anything the last session did not already see
            return VQL("Caché sin compilaciones nuevas (\(c.requested) pedidos): la sesión no llegó a estabilizarse",
                       "Cache with nothing new compiled (\(c.requested) requested): the session did not settle")
        }
        return VQL("Caché incompleta (\(c.requested) pedidos\(compiled)): juega hasta que termine la precarga",
                   "Cache incomplete (\(c.requested) requested\(compiled)): keep playing until the preload finishes")
    }

    private func tag(_ text: String, _ symbol: String) -> some View {
        Label(text, systemImage: symbol).font(.caption.bold()).padding(.horizontal, 10).padding(.vertical, 5)
            .background(.thinMaterial, in: Capsule())
    }
}

struct VQSettingsView: View {
    @ObservedObject private var model = VQModel.shared
    @ObservedObject private var settings = VQSettings.shared
    @Environment(\.dismiss) private var dismiss
    @State private var importing = false
    @State private var p12: Data?
    @State private var password = ""
    @State private var result: String?
    @State private var pickingFolder = false
    @State private var calibNote: String?

    var body: some View {
        NavigationStack {
            Form {
                Section(VQL("Certificado de firma", "Signing certificate")) {
                    Text(model.certificate ?? VQL("Ninguno", "None")).foregroundStyle(.secondary)
                    Text(VQL("El .p12 con el que se firmó la app (el de SideStore/iloader). Las librerías convertidas se firman con él en el visor.",
                             "The .p12 the app was signed with (SideStore/iloader's). Converted libraries are signed with it on the headset."))
                        .font(.caption)
                    Button(VQL("Importar .p12…", "Import .p12…")) { importing = true }
                    if p12 != nil {
                        SecureField(VQL("Contraseña", "Password"), text: $password)
                        Button(VQL("Guardar certificado", "Save certificate")) {
                            model.storeCertificate(p12: p12!, password: password) { text, ok in
                                result = text
                                if ok { p12 = nil; password = "" }
                            }
                        }
                    }
                    if model.certificate != nil {
                        Button(VQL("Borrar certificado", "Remove certificate"), role: .destructive) {
                            model.removeCertificate()
                        }
                    }
                    if let result { Text(result).font(.caption) }
                }
                Section(VQL("Juego", "Game")) {
                    Toggle(VQL("Definir el guardián antes de cada juego", "Set the guardian before each game"), isOn: $settings.guardianAsk)
                    Text(VQL("Con el juego aún sin arrancar y la sala ya escaneándose, una ventana deja ajustar el suelo y la zona de juego. Apagado: se usa lo guardado de ese juego o lo automático. Con una calibración ya guardada (suelo y zona, la misma para todos los juegos) se aplica sola y la ventana no sale.",
                             "With the game not started yet and the room already being scanned, a window lets you adjust the floor and the play area. Off: the game's saved definition or the automatic one is used. With a calibration already saved (floor and play area, the same for every game) it is applied by itself and the window does not appear."))
                        .font(.caption)
                    Button(VQL("Recalibrar espacio", "Recalibrate space")) {
                        VQCalibration.redo = true
                        calibNote = VQL("La ventana saldrá en el próximo juego y al confirmar sustituye la calibración.",
                                        "The window shows on the next game and Confirm replaces the calibration.")
                    }
                    Button(VQL("Borrar calibración", "Delete calibration"), role: .destructive) {
                        Task { await VQCalibration.remove() }
                        calibNote = VQL("Calibración borrada.", "Calibration deleted.")
                    }
                    if let calibNote { Text(calibNote).font(.caption) }
                    Stepper(VQL("Vigilante de arranque: \(Int(settings.watchdogSeconds)) s", "Boot watchdog: \(Int(settings.watchdogSeconds)) s"),
                            value: $settings.watchdogSeconds, in: 30...600, step: 30)
                }
                Section(VQL("Gráficos", "Graphics")) {
                    VQMSAAPicker(selection: $settings.msaaForce)
                    VQRCASSlider(value: $settings.rcasIntensity)
                    Toggle(VQL("Descargar carátulas de la tienda de Meta", "Download covers from the Meta store"), isOn: $settings.storeCovers)
                    Text(VQL("Solo descarga la imagen de la ficha de cada juego, una vez y sin cuenta. Los juegos siguen sin conexión.",
                             "It only downloads the picture of each game's listing, once and without an account. Games stay offline."))
                        .font(.caption)
                    Toggle(VQL("Idioma del sistema si el juego lo incluye (si no, inglés)", "System language if the game ships it (else English)"), isOn: $settings.systemLanguage)
                    Picker(VQL("Idioma del juego", "Game language"), selection: $settings.gameLanguage) {
                        Text(VQL("Automático (sistema)", "Automatic (system)")).tag("auto")
                        Text(VQL("Español (España)", "Spanish (Spain)")).tag("es_ES")
                        Text(VQL("Español (México)", "Spanish (Mexico)")).tag("es_MX")
                        Text(VQL("Inglés", "English")).tag("en_US")
                    }
                    Picker(VQL("Postura", "Posture"), selection: $settings.posture) {
                        Text(VQL("Automática", "Automatic")).tag("auto")
                        Text(VQL("Sentado", "Seated")).tag("seated")
                        Text(VQL("De pie", "Standing")).tag("standing")
                    }
                    .pickerStyle(.segmented)
                    VQSeatedControls(seated: $settings.seated, height: $settings.standHeight)
                    Picker("Passthrough", selection: $settings.passthrough) {
                        Text(VQL("Automático", "Automatic")).tag("auto")
                        Text(VQL("Siempre VR", "Always VR")).tag("off")
                        Text(VQL("Siempre mixto", "Always mixed")).tag("on")
                    }
                    .pickerStyle(.segmented)
                    VQResolutionSlider(value: $settings.resolutionPct)
                    Toggle(VQL("Mostrar FPS", "Show FPS"), isOn: $settings.showFps)
                    Text(VQL("Panel delante de ti con los fps del juego, los Hz del visor y la resolución de render.",
                             "Panel in front of you with the game's fps, the headset's Hz and the render resolution."))
                        .font(.caption)
                    Picker(VQL("Filtrado anisotrópico", "Anisotropic filtering"), selection: $settings.anisoLevel) {
                        Text("Off").tag(0)
                        Text("x2").tag(2)
                        Text("x4").tag(4)
                        Text("x8").tag(8)
                        Text("x16").tag(16)
                    }
                    .pickerStyle(.segmented)
                    Toggle(VQL("ASW desactivado (todos los frames reales)", "ASW off (every frame real)"), isOn: $settings.aswDisabled)
                    Toggle(VQL("Shaders AOT (todo compilado antes de jugar, experimental)", "AOT shaders (all compiled before playing, experimental)"), isOn: $settings.aotShaders)
                    Toggle(VQL("Perfil Quest 3 cuando el juego lo tenga", "Quest 3 profile when the game has one"), isOn: $settings.quest3Profile)
                    Text(VQL("Activado: los juegos en «Automático» se presentan como Quest 3 solo si su manifiesto lista Quest 3 (si no declara dispositivos, también como Quest 3); si no, como Quest 2. Desactivado: como Quest 2. Cada juego puede forzar Quest 2 o Quest 3 en sus ajustes. Se aplica al lanzar el juego.",
                             "On: games set to «Automatic» appear as a Quest 3 only if their manifest lists Quest 3 (if it declares no devices, also as a Quest 3); otherwise as a Quest 2. Off: as a Quest 2. Each game can force Quest 2 or Quest 3 in its own settings. Applied when the game launches."))
                        .font(.caption)
                    Toggle(VQL("OpenGL ES con VPKEngine (experimental)", "OpenGL ES with VPKEngine (experimental)"),
                           isOn: $settings.glesVPK)
                    Toggle(VQL("Capturas de diagnóstico", "Diagnostic captures"), isOn: $settings.diagCaptures)
                    Toggle(VQL("Diagnóstico de rendimiento", "Performance diagnostics"), isOn: $settings.perfDiag)
                    Toggle(VQL("Diagnóstico de ficheros (log grande)", "File diagnostics (large log)"), isOn: $settings.fsDiag)
                }
                VQConformanceSettings(settings: settings)
                Section(VQL("Carpeta QUEST", "QUEST folder")) {
                    Text(model.folderPath ?? VQL("No elegida", "Not chosen")).font(.caption)
                    Button(VQL("Elegir otra…", "Choose another…")) { pickingFolder = true }
                }
                // On the section, not the Form: two .fileImporter on one view and only one works.
                .fileImporter(isPresented: $pickingFolder, allowedContentTypes: [.folder]) { r in
                    if case .success(let url) = r { model.choose(url) }
                }
                Section(VQL("Registros", "Logs")) {
                    ShareLink(items: model.shareable) { Label(VQL("Compartir registros", "Share logs"), systemImage: "square.and.arrow.up") }
                    Toggle(VQL("Incluir capturas (.vpcap)", "Include captures (.vpcap)"), isOn: $settings.includeCaptures)
                        .onChange(of: settings.includeCaptures) { _, _ in
                            VQLog.refreshQuestAttachments(force: true)
                            model.refreshLogs()
                        }
                    Text(VQL("Añade a «Compartir registros» las capturas de frames de QUEST/capturas. Pueden ocupar mucho.",
                             "Adds the frame captures in QUEST/capturas to “Share logs”. They can be large."))
                        .font(.caption)
                    VQSaveLogsButton()
                    ScrollView {
                        Text(model.appLogTail).font(.system(.caption2, design: .monospaced)).textSelection(.enabled)
                            .frame(maxWidth: .infinity, alignment: .leading)
                    }
                    .frame(height: 220)
                }
            }
            .navigationTitle(VQL("Ajustes", "Settings"))
            .toolbar { Button(VQL("Hecho", "Done")) { dismiss() } }
            .onAppear { model.refreshLogs() }
            .fileImporter(isPresented: $importing, allowedContentTypes: [UTType(filenameExtension: "p12") ?? .data, .data]) { r in
                if case .success(let url) = r {
                    let scoped = url.startAccessingSecurityScopedResource()
                    p12 = try? Data(contentsOf: url)
                    if scoped { url.stopAccessingSecurityScopedResource() }
                    result = p12 == nil ? VQL("No se pudo leer el archivo.", "The file could not be read.") : nil
                }
            }
        }
        .frame(minWidth: 640, minHeight: 640)
    }
}

/// What the player sees while the game boots: name, a quiet animation and a short status. The
/// boot log still goes to the log file (and to "Ver registro").
struct VQLoadingView: View {
    let running: Bool
    let finished: Bool
    let succeeded: Bool
    let openedSpace: Bool
    let status: String
    let showLog: () -> Void
    @State private var pulse = false
    @ObservedObject private var prewarm = VQPrewarm.shared
    @ObservedObject private var model = VQModel.shared

    private var failed: Bool { finished && !succeeded }
    private var phrase: String {
        if failed { return VQL("No se pudo iniciar el juego", "The game could not start") }
        if finished || openedSpace { return VQL("Iniciando VR…", "Starting VR…") }
        if prewarm.active && prewarm.saving {
            return VQL("Guardando caché \(prewarm.done) / \(prewarm.total)", "Saving cache \(prewarm.done) / \(prewarm.total)")
        }
        if prewarm.active && prewarm.compiling {
            return VQL("Compilando shaders \(prewarm.done) / \(prewarm.total)", "Compiling shaders \(prewarm.done) / \(prewarm.total)")
        }
        if prewarm.active {
            return VQL("Preparando shaders \(prewarm.done) / \(prewarm.total)", "Preparing shaders \(prewarm.done) / \(prewarm.total)")
        }
        if running { return VQL("Cargando juego…", "Loading game…") }
        return VQL("Preparando…", "Preparing…")
    }

    var body: some View {
        VStack(spacing: 28) {
            Spacer()
            ZStack {
                Circle().fill(.thinMaterial).frame(width: 140, height: 140)
                    .scaleEffect(pulse ? 1.08 : 0.94)
                    .opacity(pulse ? 0.9 : 0.5)
                Image(systemName: failed ? "exclamationmark.triangle.fill" : "visionpro")
                    .font(.system(size: 54, weight: .light))
                    .foregroundStyle(failed ? AnyShapeStyle(Color.orange) : AnyShapeStyle(.primary))
            }
            .animation(.easeInOut(duration: 1.4).repeatForever(autoreverses: true), value: pulse)
            .onAppear { pulse = true }

            Text(VQLaunch.choice?.game.name ?? "VisionQuestPro")
                .font(.largeTitle.bold()).multilineTextAlignment(.center).lineLimit(2)

            VStack(spacing: 10) {
                if !failed {
                    if prewarm.active && prewarm.total > 0 {
                        ProgressView(value: Double(prewarm.done), total: Double(prewarm.total)).frame(maxWidth: 360)
                    } else {
                        ProgressView().controlSize(.large)
                    }
                }
                Text(phrase).font(.title3).foregroundStyle(.secondary)
                if failed, !status.isEmpty {
                    Text(status).font(.callout).foregroundStyle(.orange).multilineTextAlignment(.center)
                }
            }
            Spacer()
            HStack(spacing: 16) {
                Button(action: showLog) { Label(VQL("Ver registro", "Show log"), systemImage: "doc.text.magnifyingglass") }
                    .buttonStyle(.bordered)
                ShareLink(items: model.shareable) { Label(VQL("Compartir registros", "Share logs"), systemImage: "square.and.arrow.up") }
                    .buttonStyle(.bordered)
            }
            .font(.callout)
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .padding(32)
        .onAppear { model.refreshLogs() }
        .onChange(of: finished) { _, _ in model.refreshLogs() }
    }
}

/// Shown when the game's immersive space has closed. The runtime boots a guest once per process,
/// so playing again needs the app to start again.
struct VQEndedView: View {
    @ObservedObject private var model = VQModel.shared

    var body: some View {
        VStack(spacing: 24) {
            Spacer()
            Image(systemName: "checkmark.circle").font(.system(size: 60, weight: .light)).foregroundStyle(.secondary)
            Text(VQLaunch.choice?.game.name ?? "VisionQuestPro").font(.largeTitle.bold()).multilineTextAlignment(.center)
            if let why = VQModel.shared.endMessage {
                Text(why).font(.title3).foregroundStyle(.red).multilineTextAlignment(.center)
            }
            Text(VQL("Partida cerrada. Para volver a jugar, cierra la app y ábrela de nuevo.",
                     "Game closed. To play again, close the app and open it again."))
                .font(.title3).foregroundStyle(.secondary).multilineTextAlignment(.center)
            Spacer()
            HStack(spacing: 16) {
                ShareLink(items: model.shareable) { Label(VQL("Compartir registros", "Share logs"), systemImage: "square.and.arrow.up") }
                    .buttonStyle(.bordered)
                Button {
                    VQLog.log("Cerrar app tras la partida")
                    VQSession.end()
                    fflush(nil)
                    exit(0)
                } label: { Label(VQL("Cerrar app", "Close app"), systemImage: "xmark.circle") }
                    .buttonStyle(.borderedProminent)
            }
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .padding(32)
        .onAppear { model.refreshLogs() }
    }
}

/// «MSAA»: 0 = the game's own, 2 / 4 = MSAA forced by VPKEngine on the game's 1x scene passes (VPVK_MSAA_FORCE).
/// `global` adds the «Global» choice (-1) for the per-game sheet.
struct VQMSAAPicker: View {
    @Binding var selection: Int
    var global = false

    var body: some View {
        Picker("MSAA", selection: $selection) {
            if global { Text(VQL("Global", "Global")).tag(-1) }
            Text(VQL("Juego", "Game")).tag(0)
            Text("x2").tag(2)
            Text("x4").tag(4)
        }
        .pickerStyle(.segmented)
        Text(VQL("x2/x4 sin coste de memoria; en juegos con MSAA propio se respeta el del juego.",
                 "x2/x4 at no memory cost; in games with their own MSAA the game's is respected."))
            .font(.caption)
    }
}

/// «Sentado»: raises the guest's floor/stage space so a seated user stands at the chosen height.
struct VQSeatedControls: View {
    @Binding var seated: Bool
    @Binding var height: Double

    var body: some View {
        Toggle(VQL("Modo sentado", "Seated mode"), isOn: $seated)
        if seated {
            Stepper(VQL("Altura de pie: \(height.formatted(.number.precision(.fractionLength(2)).locale(Locale(identifier: "es_ES")))) m", "Standing height: \(String(format: "%.2f", height)) m"),
                    value: $height, in: 1.40...2.00, step: 0.01)
        }
        Text(VQL("Para jugar sentado a juegos pensados para ir de pie: sube todo (cabeza, manos y suelo) lo que falta entre tu altura de ojos medida y la altura de pie elegida, solo en juegos que usan el suelo como origen. Si ya estás más alto, no baja nada. Se aplica al lanzar el juego.",
                 "To play seated in games made for standing: raises everything (head, hands and floor) by the gap between your measured eye height and the chosen standing height, only in games that use the floor as origin. If you are already taller, nothing is lowered. Applied when the game launches."))
            .font(.caption)
    }
}

/// «Nitidez (RCAS)»: the intensity of the sharpening pass (RCAS, as in FSR), 0–100 %. Applied in every game,
/// only to the 3D: menus and text are left untouched. 0 % = off, no cost.
struct VQRCASSlider: View {
    @Binding var value: Double

    var body: some View {
        VStack(alignment: .leading) {
            Text(VQL("Nitidez (RCAS): \(Int((value * 100).rounded())) %", "Sharpness (RCAS): \(Int((value * 100).rounded())) %"))
            Slider(value: $value, in: 0...1, step: 0.05)
        }
        Text(VQL("Afilado robusto por contraste (el de FSR), sin halos. Solo el 3D: menús y textos no se tocan. 100 % = el máximo; 0 % lo desactiva y no cuesta nada.",
                 "Contrast-adaptive sharpening (FSR's), without halos. 3D only: menus and text are left alone. 100 % = maximum; 0 % turns it off at no cost."))
            .font(.caption)
    }
}

/// «Resolución»: final per-eye size, 50...200 %. 100 % = the Quest 3's eye (2064x2208 px), 200 % = the Vision
/// Pro panel's density. Whole percents: slider, and − / + buttons of 1 %.
struct VQResolutionSlider: View {
    @Binding var value: Double

    var body: some View {
        VStack(alignment: .leading) {
            Text(VQL("Resolución: \(Int(value.rounded())) %", "Resolution: \(Int(value.rounded())) %"))
            HStack(spacing: 10) {
                Button { value = max(50, value.rounded() - 1) } label: { Image(systemName: "minus") }
                    .buttonStyle(.bordered)
                    .disabled(value <= 50)
                Slider(value: $value, in: 50...200, step: 1)
                Button { value = min(200, value.rounded() + 1) } label: { Image(systemName: "plus") }
                    .buttonStyle(.bordered)
                    .disabled(value >= 200)
            }
        }
        Text(VQL("Tamaño final de la imagen de cada ojo, sea cual sea el escalado que pida el juego. 100 % = lo que da un Quest 3 (2064 × 2208 px por ojo, ≈ 4,6 Mpx); 200 % = la densidad de la pantalla del Vision Pro (≈ 11,7 Mpx). Lineal por eje entre ambos; por debajo de 100 % baja en proporción (50 % = mitad de ancho y de alto). Se aplica al lanzar el juego.",
                 "Final size of each eye's image, whatever scaling the game asks for. 100 % = what a Quest 3 gives (2064 × 2208 px per eye, ≈ 4.6 Mpx); 200 % = the Vision Pro panel's density (≈ 11.7 Mpx). Linear per axis between the two; below 100 % it drops in proportion (50 % = half the width and height). Applied when the game launches."))
            .font(.caption)
    }
}

/// Per-game settings sheet (from the game card). Saved at once
/// (UserDefaults, by VQGame.aaKey); applied when the game launches.
struct VQGameSettingsView: View {
    let game: VQGame
    @ObservedObject private var settings = VQSettings.shared
    @Environment(\.dismiss) private var dismiss
    /// -1 = the global MSAA (nothing saved for this game), else 0/2/4.
    @State private var msaa: Int
    @State private var res: VQGameRes
    /// -1 = global level (nothing saved for this game), else 0/2/4/8/16.
    @State private var aniso: Int
    /// nil = the global «Nitidez (RCAS)»; else this game's own 0...1.
    @State private var rcas: Double?
    /// 0 = automatic (nothing saved for this game), 2 = Quest 2, 3 = Quest 3.
    @State private var profile: Int
    @State private var seat: VQGameSeat
    @State private var area: VQGameArea
    @State private var capture: VQGameCapture
    @State private var d24: Bool
    @State private var frameAsync: Bool
    @State private var sceneMesh: Bool

    init(game: VQGame) {
        self.game = game
        _msaa = State(initialValue: VQGameMSAA.load(key: game.aaKey) ?? -1)
        _res = State(initialValue: VQGameRes.load(key: game.aaKey))
        _aniso = State(initialValue: VQGameAniso.load(key: game.aaKey) ?? -1)
        _rcas = State(initialValue: VQGameRCAS.load(key: game.aaKey))
        _profile = State(initialValue: VQGameProfile.load(key: game.aaKey) ?? 0)
        _seat = State(initialValue: VQGameSeat.load(key: game.aaKey))
        _area = State(initialValue: VQGameArea.load(key: game.aaKey))
        _capture = State(initialValue: VQGameCapture.load(key: game.aaKey))
        _d24 = State(initialValue: VQGameD24.load(key: game.aaKey))
        _frameAsync = State(initialValue: VQGameFrameAsync.load(key: game.aaKey))
        _sceneMesh = State(initialValue: VQGameSceneMesh.load(key: game.aaKey))
    }

    var body: some View {
        NavigationStack {
            Form {
                Section("MSAA") {
                    VQMSAAPicker(selection: $msaa, global: true)
                }
                Section(VQL("Nitidez (RCAS)", "Sharpness (RCAS)")) {
                    Toggle(VQL("Usar la global", "Use the global one"), isOn: Binding(
                        get: { rcas == nil },
                        set: { rcas = $0 ? nil : settings.rcasIntensity }))
                    VQRCASSlider(value: Binding(
                        get: { rcas ?? settings.rcasIntensity },
                        set: { rcas = $0 }))
                        .disabled(rcas == nil)
                }
                Section(VQL("Resolución", "Resolution")) {
                    Toggle(VQL("Usar la global", "Use the global one"), isOn: $res.useGlobal)
                    if !res.useGlobal {
                        VQResolutionSlider(value: $res.pct)
                    }
                    Text(eyeResText).font(.caption)
                }
                Section(VQL("Modo sentado", "Seated mode")) {
                    Toggle(VQL("Usar el global", "Use the global one"), isOn: $seat.useGlobal)
                    if !seat.useGlobal {
                        VQSeatedControls(seated: $seat.seated, height: $seat.height)
                    }
                }
                Section(VQL("Área de juego", "Play area")) {
                    Picker(VQL("Área de juego", "Play area"), selection: $area.seated) {
                        Text(VQL("Libre", "Free")).tag(false)
                        Text(VQL("Sentado", "Seated")).tag(true)
                    }
                    .pickerStyle(.segmented)
                    if !area.seated {
                        Slider(value: $area.w, in: 2...6, step: 0.25) { Text(VQL("Ancho", "Width")) }
                        Text(VQL("Ancho: \(String(format: "%.2f", area.w)) m", "Width: \(String(format: "%.2f", area.w)) m")).font(.caption)
                        Slider(value: $area.d, in: 2...6, step: 0.25) { Text(VQL("Fondo", "Depth")) }
                        Text(VQL("Fondo: \(String(format: "%.2f", area.d)) m", "Depth: \(String(format: "%.2f", area.d)) m")).font(.caption)
                    }
                    Text(VQL("La zona de juego que el juego cree que tienes (guardián) y el tamaño de la sala si pide escanearla. «Libre»: rectángulo de ancho × fondo; «Sentado»: círculo de 1 m de radio.",
                             "The play area the game believes you have (guardian) and the size of the room if it asks to scan it. «Free»: a width × depth rectangle; «Seated»: a circle of 1 m radius."))
                        .font(.caption)
                }
                Section(VQL("Filtrado anisotrópico", "Anisotropic filtering")) {
                    Picker(VQL("Filtrado anisotrópico", "Anisotropic filtering"), selection: $aniso) {
                        Text(VQL("Global", "Global")).tag(-1)
                        Text(VQL("Del juego", "Game's own")).tag(0)
                        Text("2x").tag(2)
                        Text("4x").tag(4)
                        Text("8x").tag(8)
                        Text("16x").tag(16)
                    }
                    Text(VQL("Fuerza el nivel en los samplers de materiales del juego. «Global» usa el ajuste de Ajustes; «Del juego» no toca nada.",
                             "Forces the level on the game's material samplers. «Global» follows Settings; «Game's own» changes nothing."))
                        .font(.caption)
                }
                Section(VQL("Emular profundidad de 24 bits (experimental)", "Emulate 24-bit depth (experimental)")) {
                    Toggle(VQL("Emular profundidad de 24 bits (experimental)", "Emulate 24-bit depth (experimental)"), isOn: $d24)
                    Text(VQL("Para Into The Radius 2 y juegos con z-fighting; puede costar rendimiento", "For Into The Radius 2 and games with z-fighting; it may cost performance"))
                        .font(.caption)
                }
                Section(VQL("Fin de frame asíncrono (experimental)", "Asynchronous frame end (experimental)")) {
                    Toggle(VQL("Fin de frame asíncrono (experimental)", "Asynchronous frame end (experimental)"), isOn: $frameAsync)
                    Text(VQL("Apagado por defecto; puede cambiar el ritmo de los frames", "Off by default; it may change frame pacing"))
                        .font(.caption)
                }
                Section(VQL("Mostrar malla de escena (wireframe de algunos juegos)", "Show scene mesh (wireframe of some games)")) {
                    Toggle(VQL("Mostrar malla de escena (wireframe de algunos juegos)", "Show scene mesh (wireframe of some games)"), isOn: $sceneMesh)
                    Text(VQL("Apagado: se oculta la malla que el juego dibuja sobre el passthrough", "Off: hides the mesh the game draws over passthrough"))
                        .font(.caption)
                }
                Section(VQL("Perfil del visor", "Headset profile")) {
                    Picker(VQL("Perfil del visor", "Headset profile"), selection: $profile) {
                        Text(VQL("Automático", "Automatic")).tag(0)
                        Text("Quest 2").tag(2)
                        Text("Quest 3").tag(3)
                    }
                    .pickerStyle(.segmented)
                    Text(VQL("Qué visor cree el juego que es (modelo, tipo de mando, nombre de la GPU). «Automático»: Quest 3 solo si «Perfil Quest 3 cuando el juego lo tenga» (Ajustes) está activado y el manifiesto del juego lista Quest 3 (si no declara dispositivos, basta el ajuste); si no, Quest 2. Se aplica al lanzar el juego.",
                             "Which headset the game believes it runs on (model, controller type, GPU name). «Automatic»: Quest 3 only if «Quest 3 profile when the game has one» (Settings) is on and the game's manifest lists Quest 3 (if it declares no devices, the setting alone decides); otherwise Quest 2. Applied when the game launches."))
                        .font(.caption)
                }
                Section(VQL("Captura de frames (diagnóstico)", "Frame capture (diagnostics)")) {
                    Toggle(VQL("Captura de frames (diagnóstico)", "Frame capture (diagnostics)"), isOn: $capture.enabled)
                    Text(VQL("Usa más RAM: el motor gráfico guarda desde el arranque todo lo que crea el juego. Apágalo cuando no lo necesites.",
                             "Uses more RAM: the graphics engine keeps everything the game creates from the start. Turn it off when you do not need it."))
                        .font(.caption).foregroundStyle(.orange)
                    if capture.enabled {
                        Picker(VQL("Para capturar, mantén", "To capture, hold"), selection: $capture.button) {
                            ForEach(VQGameCapture.Combo.allCases, id: \.self) { Text($0.title).tag($0) }
                        }
                        Stepper(capture.holdSeconds == 0 ? VQL("Al instante (sin mantener)", "Instantly (no hold)")
                                    : VQL("Durante \(capture.holdSeconds) s", "For \(capture.holdSeconds) s"),
                                value: $capture.holdSeconds, in: 0...10)
                    }
                    Text(VQL("En la partida, pulsa esa combinación (mantenla el tiempo elegido, si no es «al instante»): vibran los mandos (fuerte y corto = capturando; largo y suave = no se pudo) y el motor guarda un frame en QUEST/capturas/\(game.id)/. El juego también recibe la pulsación. Solo con VPKEngine. Se aplica al lanzar el juego. Para compartirlas, marca «Incluir capturas» en Ajustes > Registros.",
                             "In the game, press that combination (hold it for the chosen time, unless it is “instantly”): the controllers vibrate (strong and short = capturing; long and soft = it could not start) and the engine saves one frame to QUEST/capturas/\(game.id)/. The game receives the press too. VPKEngine only. Applied when the game launches. To share them, tick “Include captures” in Settings > Logs."))
                        .font(.caption)
                }
            }
            .navigationTitle(game.name)
            .toolbar { Button(VQL("Hecho", "Done")) { dismiss() } }
            .onChange(of: capture) { _, new in
                new.save(key: game.aaKey)
                VQLog.log("Ajustes del juego \(game.aaKey): captura de frames \(new.enabled ? "sí, \(new.button.title), \(new.holdSeconds == 0 ? "al instante" : "\(new.holdSeconds) s")" : "no")")
            }
            .onChange(of: msaa) { _, new in
                VQGameMSAA.save(new < 0 ? nil : new, key: game.aaKey)
                VQLog.log("Ajustes del juego \(game.aaKey): MSAA \(new < 0 ? "global" : (new == 0 ? "el del juego" : "x\(new)"))")
            }
            .onChange(of: res) { _, new in
                new.save(key: game.aaKey)
                VQLog.log("Ajustes del juego \(game.aaKey): resolución \(new.useGlobal ? "global" : "\(Int(new.pct.rounded())) %")")
            }
            .onChange(of: seat) { _, new in
                new.save(key: game.aaKey)
                VQLog.log("Ajustes del juego \(game.aaKey): modo sentado \(new.useGlobal ? "global" : (new.seated ? "sí, \(String(format: "%.2f", new.height)) m" : "no"))")
            }
            .onChange(of: area) { _, new in
                new.save(key: game.aaKey)
                VQLog.log("Ajustes del juego \(game.aaKey): área de juego \(new.seated ? "sentado" : "libre \(String(format: "%.2f", new.w)) × \(String(format: "%.2f", new.d)) m")")
            }
            .onChange(of: rcas) { _, new in
                VQGameRCAS.save(new, key: game.aaKey)
                VQLog.log("Ajustes del juego \(game.aaKey): Nitidez (RCAS) \(new.map { "\(Int($0 * 100)) %" } ?? "global")")
            }
            .onChange(of: profile) { _, new in
                VQGameProfile.save(new == 0 ? nil : new, key: game.aaKey)
                VQLog.log("Ajustes del juego \(game.aaKey): perfil del visor \(new == 0 ? "automático" : "Quest \(new)")")
            }
            .onChange(of: d24) { _, new in
                VQGameD24.save(new, key: game.aaKey)
                VQLog.log("Ajustes del juego \(game.aaKey): profundidad de 24 bits emulada \(new ? "sí" : "no")")
            }
            .onChange(of: sceneMesh) { _, new in
                VQGameSceneMesh.save(new, key: game.aaKey)
                VQLog.log("Ajustes del juego \(game.aaKey): malla de escena visible \(new ? "sí" : "no")")
            }
            .onChange(of: frameAsync) { _, new in
                VQGameFrameAsync.save(new, key: game.aaKey)
                VQLog.log("Ajustes del juego \(game.aaKey): fin de frame asíncrono \(new ? "sí" : "no")")
            }
            .onChange(of: aniso) { _, new in
                VQGameAniso.save(new < 0 ? nil : new, key: game.aaKey)
                VQLog.log("Ajustes del juego \(game.aaKey): anisotrópico \(new < 0 ? "global" : "x\(new)")")
            }
        }
        .frame(minWidth: 560, minHeight: 480)
    }

    /// Per-eye resolution the choice gives (the global one when "Usar la global").
    private var eyeResText: String {
        let pct = res.useGlobal ? settings.resolutionPct : res.pct
        let e = VQGameRes.approxEye(pct: pct)
        return VQL("\(Int(pct.rounded())) % ≈ \(e.w) × \(e.h) px por ojo (forma del Quest 3)",
                   "\(Int(pct.rounded())) % ≈ \(e.w) × \(e.h) px per eye (Quest 3 shape)")
    }
}
