// VisionQuestPro: the conformance run's window (instead of the game loading screen) and the
// settings section that downloads the tests and sets how long a session runs.

import SwiftUI

/// Shown by BootView while the Vulkan CTS runs (window only, no immersive space).
struct VQConformanceView: View {
    let finished: Bool
    let succeeded: Bool
    let status: String
    let showLog: () -> Void
    @State private var live = VQConformance.Live()
    @State private var stopping = false
    @State private var summary = ""
    @State private var lastChange = Date()
    @State private var stalled = false

    var body: some View {
        VStack(spacing: 22) {
            Text(VQL("Tests oficiales de Vulkan (Khronos)", "Official Vulkan tests (Khronos)")).font(.largeTitle.bold())
            if !finished {
                let total = max(VQConformance.plannedBatches.count, 1)
                let n = min(live.batchIndex + 1, total)
                Text(VQL("Tanda \(n) de \(total): \(live.batchName)", "Batch \(n) of \(total): \(live.batchName)"))
                    .font(.title3)
                ProgressView(value: Double(live.doneInBatch), total: Double(max(live.casesInBatch, 1)))
                    .frame(maxWidth: 480)
                Text(VQL("\(live.doneInBatch) de \(live.casesInBatch) casos en esta tanda",
                         "\(live.doneInBatch) of \(live.casesInBatch) cases in this batch"))
                    .foregroundStyle(.secondary)
                HStack(spacing: 18) {
                    count("Pass", VQL("superados", "passed"), .green)
                    count("Fail", VQL("fallos", "failed"), .red)
                    count("NotSupported", VQL("no soportados", "not supported"), .secondary)
                }
                if stalled {
                    Text(VQL("Un test lleva más de 5 minutos sin avanzar: está colgado. Cierra la app y vuelve a abrir los tests; ese caso cuenta como fallo y se sigue con el siguiente.",
                             "A test has not moved for over 5 minutes: it is hung. Close the app and open the tests again; that case counts as a failure and the run goes on with the next one."))
                        .font(.callout).foregroundStyle(.orange).multilineTextAlignment(.center).frame(maxWidth: 560)
                }
                Text(VQL("Puedes dejarlo correr con el visor puesto. Se para solo al llegar al tiempo de la sesión; lo pendiente sigue en la próxima.",
                         "Let it run with the headset on. It stops by itself at the session time; the rest continues next time."))
                    .font(.callout).foregroundStyle(.secondary).multilineTextAlignment(.center).frame(maxWidth: 560)
                Button(role: .destructive) {
                    stopping = true
                    VQConformance.requestStop()
                } label: {
                    Label(stopping ? VQL("Parando tras esta tanda…", "Stopping after this batch…")
                                   : VQL("Parar tras esta tanda", "Stop after this batch"), systemImage: "stop.circle")
                }
                .buttonStyle(.bordered).disabled(stopping)
            } else {
                Image(systemName: succeeded ? "checkmark.circle" : "exclamationmark.triangle.fill")
                    .font(.system(size: 48, weight: .light)).foregroundStyle(succeeded ? Color.secondary : Color.orange)
                if !succeeded && !status.isEmpty { Text(status).foregroundStyle(.orange) }
                ScrollView {
                    Text(summary).font(.system(.caption, design: .monospaced)).textSelection(.enabled)
                        .frame(maxWidth: .infinity, alignment: .leading)
                }
                .frame(maxHeight: 360)
                Text(VQL("Para seguir con las tandas pendientes, cierra la app y vuelve a abrir los tests.",
                         "To continue with the remaining batches, close the app and open the tests again."))
                    .foregroundStyle(.secondary)
            }
            HStack(spacing: 16) {
                Button(action: showLog) { Label(VQL("Ver registro", "Show log"), systemImage: "doc.text.magnifyingglass") }
                    .buttonStyle(.bordered)
                ShareLink(items: VQLog.shareable) { Label(VQL("Compartir registros", "Share logs"), systemImage: "square.and.arrow.up") }
                    .buttonStyle(.bordered)
                if finished {
                    Button {
                        VQLog.log("Cerrar app tras los tests")
                        fflush(nil)
                        exit(0)
                    } label: { Label(VQL("Cerrar app", "Close app"), systemImage: "xmark.circle") }
                        .buttonStyle(.borderedProminent)
                }
            }
        }
        .padding(36)
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .task(id: finished) {
            // Polls while the run lasts; once finished, loads the summary once.
            while !Task.isCancelled {
                if finished {
                    summary = await Task.detached(priority: .utility) {
                        (try? String(contentsOf: VQConformance.summaryFile, encoding: .utf8)) ?? ""
                    }.value
                    return
                }
                let l = await Task.detached(priority: .utility) { VQConformance.live() }.value
                if l.doneInBatch != live.doneInBatch || l.batchIndex != live.batchIndex { lastChange = Date() }
                live = l
                // A test that has not moved in 5 minutes is hung: closing the app records it as a
                // crash and the next session goes on from the case after it.
                let s = Date().timeIntervalSince(lastChange) > 300
                if s && !stalled { VQLog.log("Conformidad: sin progreso en 5 min (tanda \(l.batchIndex + 1), caso \(l.doneInBatch + 1))") }
                stalled = s
                try? await Task.sleep(for: .seconds(2))
            }
        }
    }

    private func count(_ key: String, _ label: String, _ color: Color) -> some View {
        VStack {
            Text("\(live.counts[key, default: 0])").font(.title.bold()).foregroundStyle(color)
            Text(label).font(.caption).foregroundStyle(.secondary)
        }
    }
}

/// Settings > Conformidad: download the tests, session length, memory limit, reset.
struct VQConformanceSettings: View {
    @ObservedObject var settings: VQSettings
    @ObservedObject private var model = VQModel.shared
    @State private var downloading = false
    @State private var downloadingXR = false
    @State private var fraction = 0.0
    @State private var result: String?

    var body: some View {
        Section(VQL("Conformidad (tests oficiales Khronos)", "Conformance (official Khronos tests)")) {
            Text(VQL("Descarga el test oficial de Vulkan (el mismo con el que se certifican los drivers de Quest). Aparece como un juego más: «Vulkan CTS (Khronos)». Se ejecuta en una ventana, por tandas, y el resumen sale en el registro.",
                     "Downloads the official Vulkan test (the one Quest drivers are certified with). It shows up as one more game, “Vulkan CTS (Khronos)”. It runs in a window, in batches, and the summary goes to the log."))
                .font(.caption)
            if downloading {
                ProgressView(value: fraction) { Text(VQL("Descargando…", "Downloading…")).font(.caption) }
            } else {
                Button(VQL("Descargar / actualizar Vulkan CTS", "Download / update Vulkan CTS")) {
                    downloading = true; fraction = 0; result = nil
                    VQConformance.download(progress: { f in Task { @MainActor in fraction = f } },
                                           done: { err in
                        Task { @MainActor in
                            downloading = false
                            result = err.map { VQL("Error: ", "Error: ") + $0 } ?? VQL("Listo. Ya está en la lista de juegos.", "Done. It is in the game list now.")
                            model.refresh()
                        }
                    })
                }
                .disabled(model.folderPath == nil)
            }
            if downloadingXR {
                ProgressView(value: fraction) { Text(VQL("Descargando…", "Downloading…")).font(.caption) }
            } else {
                Button(VQL("Descargar / actualizar OpenXR CTS (inmersivo)", "Download / update OpenXR CTS (immersive)")) {
                    downloadingXR = true; fraction = 0; result = nil
                    VQConformance.download(openXR: true, progress: { f in Task { @MainActor in fraction = f } },
                                           done: { err in
                        Task { @MainActor in
                            downloadingXR = false
                            result = err.map { VQL("Error: ", "Error: ") + $0 } ?? VQL("Listo: «OpenXR CTS» ya está en la lista de juegos. Se juega como un juego (inmersivo); su resultado sale en el registro.", "Done: “OpenXR CTS” is in the game list. It plays like a game (immersive); its result goes to the log.")
                            model.refresh()
                        }
                    })
                }
                .disabled(model.folderPath == nil || downloading)
            }
            if let result { Text(result).font(.caption).foregroundStyle(.secondary) }
            Picker(VQL("Duración de cada sesión", "Length of each session"), selection: $settings.conformanceMinutes) {
                Text("5 min").tag(5.0); Text("15 min").tag(15.0); Text("30 min").tag(30.0); Text("60 min").tag(60.0)
            }
            Picker(VQL("Límite de memoria", "Memory limit"), selection: $settings.conformanceMemoryMB) {
                Text("2 GB").tag(2000); Text("3 GB").tag(3000); Text("4 GB").tag(4000)
            }
            if let cts = model.games.first(where: { VQConformance.isConformance($0) }) {
                Button(VQL("Borrar resultados (empezar de cero)", "Clear results (start over)"), role: .destructive) {
                    VQConformance.reset(game: cts)
                    result = VQL("Resultados borrados.", "Results cleared.")
                }
            }
        }
    }
}
