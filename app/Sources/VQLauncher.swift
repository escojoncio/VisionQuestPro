// VisionQuestPro: the launcher window. QUEST folder, a card per game, the preparation with its
// progress, settings (certificate, options) and the logs to share. Once a game is prepared and
// started, the window becomes Klepton's BootView, which boots it and opens the immersive space.

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
    @Published var certificate: String? = VQSigner.current

    private init() {
        VQLog.start()
        refresh()
    }

    func refresh() {
        folderPath = VQFolder.shared.url?.path
        let found = folderPath == nil ? [] : VQGames.scan()
        games = found
    }

    func choose(_ url: URL) {
        if let problem = VQFolder.shared.choose(url) { message = problem } else { message = nil }
        refresh()
    }

    func play(_ game: VQGame) {
        guard busy == nil else { return }
        guard VQSigner.current != nil else {
            message = VQSigner.Problem.missing.errorDescription
            return
        }
        busy = game.id
        progress = 0
        progressText = VQL("Preparando…", "Preparing…")
        message = nil
        Thread.detachNewThread {
            do {
                let prepared = try VQPrepare.prepare(game) { fraction, text in
                    Task { @MainActor in
                        self.progress = fraction
                        self.progressText = text
                    }
                }
                Task { @MainActor in
                    VQLaunch.choice = VQLaunch.Choice(game: game, prepared: prepared)
                    self.busy = nil
                    self.running = true
                }
            } catch {
                VQLog.log("Preparar: \(game.id) falló: \(error.localizedDescription)")
                let text = error.localizedDescription
                Task { @MainActor in
                    self.busy = nil
                    self.message = text
                }
            }
        }
    }
}

struct VQRootView: View {
    @ObservedObject private var model = VQModel.shared

    var body: some View {
        if model.running {
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
                if model.folderPath == nil {
                    noFolder
                } else if model.games.isEmpty {
                    ContentUnavailableView(VQL("No hay juegos", "No games"), systemImage: "shippingbox",
                                           description: Text(VQL("Copia cada juego (su .apk y su carpeta de datos) en su propia carpeta dentro de QUEST/Juegos.",
                                                                 "Copy each game (its .apk and its data folder) into its own folder inside QUEST/Juegos.")))
                } else {
                    ScrollView {
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
                    ShareLink(items: VQLog.shareable) { Label(VQL("Registros", "Logs"), systemImage: "doc.text") }
                    Button { showSettings = true } label: { Label(VQL("Ajustes", "Settings"), systemImage: "gearshape") }
                }
            }
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

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            Text(game.name).font(.title2.bold()).lineLimit(2)
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
                Button {
                    model.play(game)
                } label: {
                    Label(VQPrepare.isPrepared(game) ? VQL("Jugar", "Play") : VQL("Preparar y jugar", "Prepare and play"),
                          systemImage: "play.fill")
                }
                .buttonStyle(.borderedProminent)
                .disabled(!game.compatible || model.busy != nil)
            }
        }
        .padding(20)
        .frame(maxWidth: .infinity, alignment: .leading)
        .glassBackgroundEffect()
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
                            do {
                                result = try VQSigner.store(p12: p12!, password: password)
                                model.certificate = VQSigner.current
                                p12 = nil; password = ""
                            } catch { result = error.localizedDescription }
                        }
                    }
                    if model.certificate != nil {
                        Button(VQL("Borrar certificado", "Remove certificate"), role: .destructive) {
                            VQSigner.remove(); model.certificate = nil
                        }
                    }
                    if let result { Text(result).font(.caption) }
                }
                Section(VQL("Juego", "Game")) {
                    Toggle(VQL("Sin conexión (el juego no intenta conectarse)", "Offline (the game does not try to connect)"), isOn: $settings.offline)
                    Toggle(VQL("Guardar el registro del juego en archivo", "Save the game log to a file"), isOn: $settings.guestLog)
                    Stepper(VQL("Vigilante de arranque: \(Int(settings.watchdogSeconds)) s", "Boot watchdog: \(Int(settings.watchdogSeconds)) s"),
                            value: $settings.watchdogSeconds, in: 30...600, step: 30)
                }
                Section(VQL("Gráficos", "Graphics")) {
                    Toggle(VQL("Motor gráfico VPEngine (apagado: MoltenVK)", "VPEngine graphics (off: MoltenVK)"),
                           isOn: $settings.useVPEngine)
                    Picker(VQL("Foveado", "Foveation"), selection: $settings.foveation) {
                        Text(VQL("Apagado", "Off")).tag(VQSettings.Foveation.off)
                        Text("M2").tag(VQSettings.Foveation.m2)
                        if VQSettings.isM5 { Text("M5").tag(VQSettings.Foveation.m5) }
                        Text(VQL("Personalizado", "Custom")).tag(VQSettings.Foveation.custom)
                    }
                    if settings.foveation == .custom {
                        Stepper(VQL("Zona nítida: \(Int(settings.sharpZone * 100)) %", "Sharp zone: \(Int(settings.sharpZone * 100)) %"),
                                value: $settings.sharpZone, in: 0.05...1, step: 0.05)
                        Stepper(VQL("Periferia: \(Int(settings.periphery * 100)) %", "Periphery: \(Int(settings.periphery * 100)) %"),
                                value: $settings.periphery, in: 0.1...1, step: 0.05)
                        Stepper(VQL("Altura: \(Int(settings.fovHeight * 100)) %", "Height: \(Int(settings.fovHeight * 100)) %"),
                                value: $settings.fovHeight, in: -0.5...0.5, step: 0.05)
                    }
                    Toggle(VQL("VPKTAA (sustituye al AA/MSAA del juego)", "VPKTAA (replaces the game's AA/MSAA)"), isOn: $settings.vpktaa)
                    if settings.vpktaa {
                        Stepper(VQL("VPKTAA: \(Int(settings.vpktaaPercent)) %", "VPKTAA: \(Int(settings.vpktaaPercent)) %"),
                                value: $settings.vpktaaPercent, in: 0...100, step: 5)
                        Stepper(VQL("Nitidez: \(Int(settings.vpktaaSharpness * 100)) %", "Sharpness: \(Int(settings.vpktaaSharpness * 100)) %"),
                                value: $settings.vpktaaSharpness, in: 0...1, step: 0.1)
                    }
                    Stepper(VQL("Resolución de render: \(Int(settings.renderScale * 100)) %", "Render resolution: \(Int(settings.renderScale * 100)) %"),
                            value: $settings.renderScale, in: 0.5...1.5, step: 0.05)
                    Toggle(VQL("ASW desactivado (todos los frames reales)", "ASW off (every frame real)"), isOn: $settings.aswDisabled)
                    Toggle(VQL("Perfil Quest 3", "Quest 3 profile"), isOn: $settings.quest3Profile)
                    Text(VQL("El foveado propio del juego (Quest) está siempre apagado. Foveado y VPKTAA los aplica VPEngine (aún sin efecto con MoltenVK). M5 solo aparece en un Vision Pro M5. Se aplica al lanzar el juego.",
                             "The game's own (Quest) foveation is always off. Foveation and VPKTAA are applied by VPEngine (no effect yet with MoltenVK). M5 only appears on a Vision Pro M5. Applied when the game launches."))
                        .font(.caption)
                }
                Section(VQL("Carpeta QUEST", "QUEST folder")) {
                    Text(model.folderPath ?? VQL("No elegida", "Not chosen")).font(.caption)
                    Button(VQL("Elegir otra…", "Choose another…")) { pickingFolder = true }
                }
                // On the section, not the Form: two .fileImporter on one view and only one works.
                .fileImporter(isPresented: $pickingFolder, allowedContentTypes: [.folder]) { r in
                    if case .success(let url) = r { model.choose(url) }
                }
                Section(VQL("Registros", "Logs")) {
                    ShareLink(items: VQLog.shareable) { Label(VQL("Compartir registros", "Share logs"), systemImage: "square.and.arrow.up") }
                    ScrollView {
                        Text(VQLog.tail(VQLog.appLog)).font(.system(.caption2, design: .monospaced)).textSelection(.enabled)
                            .frame(maxWidth: .infinity, alignment: .leading)
                    }
                    .frame(height: 220)
                }
            }
            .navigationTitle(VQL("Ajustes", "Settings"))
            .toolbar { Button(VQL("Hecho", "Done")) { dismiss() } }
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
