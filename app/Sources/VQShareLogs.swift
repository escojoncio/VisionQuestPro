// VisionQuestPro: the logs buttons. A plain button that copies the logs to QUEST/Registros (Files
// app, no system service involved) and, beside it, the system ShareLink as it always was.
//
// NOT a Menu: a ShareLink inside a Menu has the menu built with the share entry at launch (the
// launcher toolbar), and on visionOS 27.2 that path aborted the app before its window appeared —
// the same ExtensionFoundation cast failure (`_NSXPCDistantObject` → `_EXDiscoveryServiceProtocol`)
// the share sheet hit in log 60. A standalone ShareLink in the toolbar is what every build up to
// a1df14a shipped, and it opened fine on 27.2.

import Combine
import SwiftUI

struct VQShareLogs: View {
    var title = VQL("Compartir registros", "Share logs")
    var icon = "square.and.arrow.up"
    @State private var note: String?
    @State private var broken = VQCompilerProbe.servicesBroken
    private let tick = Timer.publish(every: 5, on: .main, in: .common).autoconnect()

    var body: some View {
        Group {
            // No alert: presentation modifiers hosted in a toolbar item are unreliable on visionOS.
            // The label itself says what happened, for 4 s.
            Button {
                if let dest = VQLog.saveToQuest("manual") {
                    note = VQL("Guardado en QUEST/Registros", "Saved to QUEST/Registros")
                    VQLog.log("Registros guardados a mano en \(dest.lastPathComponent)")
                } else {
                    note = VQL("Elige la carpeta QUEST", "Choose the QUEST folder")
                }
                DispatchQueue.main.asyncAfter(deadline: .now() + 4) { note = nil }
            } label: { Label(note ?? VQL("Guardar registros", "Save logs"), systemImage: note == nil ? "folder" : "checkmark") }
            if !broken {
                ShareLink(items: VQLog.shareable) { Label(title, systemImage: icon) }
            }
        }
        .onAppear { broken = VQCompilerProbe.servicesBroken }
        .onReceive(tick) { _ in broken = VQCompilerProbe.servicesBroken }
    }
}
