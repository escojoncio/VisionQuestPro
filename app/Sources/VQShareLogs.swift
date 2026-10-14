// VisionQuestPro: the logs button. A menu with the system share sheet and a copy to
// QUEST/Registros (Files app). The share sheet is left out while the compiler probe says new
// connections to system services fail in this process: in that state it aborted the app inside
// ExtensionFoundation (log 60), and the copy needs no service at all.

import Combine
import SwiftUI

struct VQShareLogs: View {
    var title = VQL("Compartir registros", "Share logs")
    var icon = "square.and.arrow.up"
    @State private var note: String?
    @State private var broken = VQCompilerProbe.servicesBroken
    private let tick = Timer.publish(every: 5, on: .main, in: .common).autoconnect()

    var body: some View {
        Menu {
            if !broken {
                ShareLink(items: VQLog.shareable) { Label(VQL("Compartir…", "Share…"), systemImage: "square.and.arrow.up") }
            }
            Button {
                if let dest = VQLog.saveToQuest("manual") {
                    note = VQL("Guardados en QUEST/Registros/\(dest.lastPathComponent)",
                               "Saved to QUEST/Registros/\(dest.lastPathComponent)")
                } else {
                    note = VQL("Elige antes la carpeta QUEST.", "Choose the QUEST folder first.")
                }
            } label: { Label(VQL("Guardar en QUEST/Registros", "Save to QUEST/Registros"), systemImage: "folder") }
        } label: { Label(title, systemImage: icon) }
        .onAppear { broken = VQCompilerProbe.servicesBroken }
        .onReceive(tick) { _ in broken = VQCompilerProbe.servicesBroken }
        .alert(note ?? "", isPresented: Binding(get: { note != nil }, set: { if !$0 { note = nil } })) {
            Button("OK", role: .cancel) { note = nil }
        } message: {
            if broken {
                Text(VQL("Compartir está desactivado hasta reiniciar la app: los servicios del sistema no responden en este proceso.",
                         "Sharing is off until the app restarts: system services are not answering in this process."))
            }
        }
    }
}
