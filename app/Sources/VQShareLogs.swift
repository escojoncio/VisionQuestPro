// VisionQuestPro: "Save logs" — copies the logs to QUEST/Registros (Files app) with no system
// service involved. Only in Settings > Registros, next to the system ShareLink.
//
// The launcher toolbar keeps exactly the ShareLink it always had: builds 621d102/aae1aea/cefc682
// added a Menu, then a second toolbar button, beside it, and none of them launched on visionOS 27.2
// (a ShareLink that ends up inside a menu — explicit or the toolbar's overflow — has the share
// sheet's extension discovery run while the window is built, which aborted the app in log 60).

import SwiftUI

struct VQSaveLogsButton: View {
    @State private var note: String?
    @State private var saving = false

    var body: some View {
        Button {
            guard !saving else { return }   // a second press while the copy runs would write the same folder
            saving = true
            // Off the main thread: it copies tens of MB (and stages an Unreal game's engine log).
            Thread.detachNewThread {
                let saved = VQLog.saveToQuest("manual") != nil
                Task { @MainActor in
                    saving = false
                    if saved {
                        note = VQL("Guardado en QUEST/Registros", "Saved to QUEST/Registros")
                    } else {
                        note = VQL("Elige la carpeta QUEST", "Choose the QUEST folder")
                    }
                    DispatchQueue.main.asyncAfter(deadline: .now() + 4) { note = nil }
                }
            }
        } label: {
            Label(note ?? VQL("Guardar registros en QUEST", "Save logs to QUEST"),
                  systemImage: note == nil ? "folder" : "checkmark")
        }
    }
}
