// VisionQuestPro: the guardian screen shown before each game (setting «Definir el guardián antes de cada juego»).
//
// It appears with the game's immersive space already open (ARKit is capturing floor and room) and the game NOT started: the
// compositor (KleptonCompositor.renderLoop) waits for VQGuardianGate.run before kl_app_guest_start. The rules (the 0.1524 m
// cap under the head, the 1.15 m minimum side, the stationary fallback) live in kl_guardian.c; this file only asks for them.

import GameController
import SwiftUI
import simd

/// The wait between "space open" and "game starts".
enum VQGuardianGate {
    enum Outcome { case confirmed, skipped, cancelled }

    static let windowID = "guardian"
    /// Set by the guardian window when it appears: a window that never shows must not hold the game forever.
    @MainActor static var shown = false
    @MainActor private static var waiter: CheckedContinuation<Outcome, Never>?
    /// Closes the guardian window; set by the window while it is up.
    @MainActor static var closeWindow: (() -> Void)?

    /// True when this game stops at the guardian screen (callable from the compositor thread).
    static func applies(to game: VQGame) -> Bool {
        VQSettings.shared.guardianAsk && !VQConformance.isConformance(game)
    }

    /// Opens the guardian window and returns when it ends. Without a window to open, or if it never shows, the game goes on
    /// with what KLGuardian.restore / the automatic definition gave (`.skipped`).
    @MainActor static func run(game: VQGame) async -> Outcome {
        // The compositor can reach this a moment before BootView.spaceOpened has captured the window action.
        var waited = 0
        while Lifecycle.reopenLauncher == nil && waited < 100 {
            try? await Task.sleep(nanoseconds: 50_000_000)
            waited += 1
        }
        guard let open = Lifecycle.reopenLauncher else {
            VQLog.log("Guardián: no hay acción para abrir la ventana; el juego sigue con lo guardado o lo automático")
            return .skipped
        }
        shown = false
        VQLog.log("Guardián: pantalla de definición para \(game.aaKey) (guardada: \(KLGuardian.hasSaved(game: game) ? "sí" : "no"))")
        let outcome: Outcome = await withCheckedContinuation { (c: CheckedContinuation<Outcome, Never>) in
            waiter = c
            open(id: windowID)
            Task { @MainActor in
                try? await Task.sleep(nanoseconds: 10_000_000_000)
                if waiter != nil && !shown {
                    VQLog.log("Guardián: la ventana no apareció en 10 s; el juego sigue con lo guardado o lo automático")
                    finish(.skipped)
                }
            }
        }
        VQLog.log("Guardián: \(outcome)")
        return outcome
    }

    /// True while run() is waiting for the screen to end.
    @MainActor static var waiting: Bool { waiter != nil }

    /// The space ended (or the game was cancelled from outside): ends the wait as cancelled and closes the window if it is up.
    @MainActor static func cancelAndClose() {
        finish(.cancelled)
        closeWindow?()
        closeWindow = nil
    }

    /// Ends the wait (once; later calls do nothing).
    @MainActor static func finish(_ outcome: Outcome) {
        guard let c = waiter else { return }
        waiter = nil
        c.resume(returning: outcome)
    }
}

struct VQGuardianSetupView: View {
    /// What the play area is made from on Confirm.
    enum Choice: Hashable { case room, rectangle, stationary }

    @ObservedObject private var live = KLGuardianLive.shared
    @Environment(\.dismissWindow) private var dismissWindow
    private let game: VQGame?
    private let hasSaved: Bool
    @State private var redefining: Bool
    @State private var choice: Choice
    @State private var rectW: Double
    @State private var rectD: Double
    @State private var radius: Double
    @State private var head: SIMD3<Float>?
    @State private var floorY: Float?
    @State private var sliderCenter: Float?
    @State private var handsAvailable = false
    @State private var touchUntil: Date?
    @State private var touchLeft = 0
    @State private var touchLowest: Float = .infinity
    @State private var finished = false
    @State private var floorTouched = false   // the user moved the floor on this screen (not confirmed until Confirm)

    init() {
        let g = VQLaunch.choice?.game
        game = g
        let saved = g.map { KLGuardian.hasSaved(game: $0) } ?? false
        hasSaved = saved
        _redefining = State(initialValue: !saved)
        // The sizes the game already uses (vq.area / vq.areaW / vq.areaD), the radius the guardian has.
        let area = VQGameArea.load(key: g?.aaKey ?? "")
        _rectW = State(initialValue: min(max(area.w, 1.2), 6))
        _rectD = State(initialValue: min(max(area.d, 1.2), 6))
        _radius = State(initialValue: min(max(Double(KLGuardian.radius), 0.5), 2.5))
        var c: Choice = area.seated ? .stationary : .rectangle
        if let g, let kind = KLGuardian.savedKind(game: g) { c = kind == "stationary" ? .stationary : kind == "rect" ? .rectangle : .room }
        _choice = State(initialValue: c)
    }

    var body: some View {
        NavigationStack {
            Form {
                if game == nil {
                    Text(VQL("No hay juego elegido.", "No game chosen.")).foregroundStyle(.secondary)
                    Button(VQL("Cancelar", "Cancel"), role: .cancel) { end(.cancelled) }
                } else if hasSaved && !redefining {
                    savedSection
                } else {
                    statusSection
                    floorSection
                    mapSection
                    areaSection
                    buttonsSection
                }
            }
            .navigationTitle(VQL("Definir el guardián", "Set the guardian"))
        }
        .frame(minWidth: 640, minHeight: 760)
        .task { await runLoop() }
        .onAppear {
            Task { @MainActor in
                // Nobody is waiting (the 10 s timeout passed and the game already started, or the space ended): close it.
                guard VQGuardianGate.waiting else {
                    VQLog.log("Guardián: la ventana apareció sin nadie esperándola (juego ya en marcha); se cierra")
                    finished = true
                    dismissWindow(id: VQGuardianGate.windowID)
                    return
                }
                VQGuardianGate.shown = true
                VQGuardianGate.closeWindow = { dismissWindow(id: VQGuardianGate.windowID) }
            }
        }
        .onDisappear {
            Task { @MainActor in VQGuardianGate.closeWindow = nil }
            // Closed with the window's own button: the same as Cancelar.
            if !finished { Task { @MainActor in VQGuardianGate.finish(.cancelled) } }
        }
    }

    /// 60 Hz while the window is up (cancelled with it). The 10 Hz KLGuardianLive refresh runs only meanwhile.
    private func runLoop() async {
        guard VQGuardianGate.waiting else { return }
        live.start()
        defer { live.stop() }
        while !Task.isCancelled {
            tick()
            try? await Task.sleep(nanoseconds: 16_666_667)
        }
    }

    // MARK: saved definition

    private var savedSection: some View {
        Section(VQL("Definición guardada", "Saved definition")) {
            Text(VQL("Este juego tiene un suelo y una zona de juego guardados.", "This game has a saved floor and play area."))
            LabeledContent(VQL("Suelo", "Floor"), value: floorY.map(meters) ?? VQL("Sin detectar", "Not detected"))
            LabeledContent(VQL("Zona", "Area"), value: savedAreaText)
            HStack {
                Button(VQL("Cancelar", "Cancel"), role: .cancel) { end(.cancelled) }.buttonStyle(.bordered)
                Button(VQL("Redefinir", "Redefine")) { redefining = true }.buttonStyle(.bordered)
                Button(VQL("Usar la guardada", "Use the saved one")) { end(.skipped) }.buttonStyle(.borderedProminent)
            }
        }
    }

    private var savedAreaText: String {
        if KLGuardian.mode == .stationary { return VQL("Estacionario, radio \(String(format: "%.1f", KLGuardian.radius)) m", "Stationary, radius \(String(format: "%.1f", KLGuardian.radius)) m") }
        if let p = KLGuardian.playspace() { return String(format: "%.1f × %.1f m", p.w, p.d) }
        return VQL("Automática", "Automatic")
    }

    // MARK: status and floor

    private var headY: Float { head?.y ?? live.headY }
    /// No world tracking (ARKit .worldSensing denied): the floor cannot be set or saved manually.
    private var noWorld: Bool { KLGuardian.worldTrackingDenied }

    private var statusSection: some View {
        Section(VQL("Estado", "Status")) {
            LabeledContent(VQL("Altura de la cabeza", "Head height"), value: head == nil ? VQL("Sin datos", "No data") : meters(headY))
            LabeledContent(VQL("Suelo detectado", "Detected floor"), value: floorY.map(meters) ?? VQL("Sin detectar", "Not detected"))
            LabeledContent(VQL("Fuente del suelo", "Floor source"), value: sourceText)
            if let f = floorY, head != nil, !KLGuardian.plausible(floorY: f, headY: headY) {
                Text(VQL("El suelo no parece plausible respecto a tu cabeza (debe estar entre 0,7 y 2,5 m por debajo). Ajústalo.",
                         "The floor does not look plausible against your head (it should be 0.7 to 2.5 m below). Adjust it."))
                    .font(.caption).foregroundStyle(.orange)
            }
        }
    }

    private var sourceText: String {
        switch live.floorSource {
        case .auto: return VQL("Vision Pro (automático)", "Vision Pro (automatic)")
        case .manual: return VQL("Manual", "Manual")
        case .unset: return VQL("Sin suelo", "No floor")
        }
    }

    private var floorSection: some View {
        let center = sliderCenter ?? live.detectedFloorY ?? floorY ?? (headY - 1.15)
        return Section(VQL("Ajuste de suelo", "Floor adjustment")) {
            HStack {
                Button(VQL("Bajar 1 cm", "Lower 1 cm")) { moveFloor(-0.01) }.buttonStyle(.bordered)
                Button(VQL("Subir 1 cm", "Raise 1 cm")) { moveFloor(0.01) }.buttonStyle(.bordered)
            }
            .disabled(head == nil || noWorld)
            HStack {
                Button(VQL("Bajar 5 cm", "Lower 5 cm")) { moveFloor(-0.05) }.buttonStyle(.bordered)
                Button(VQL("Subir 5 cm", "Raise 5 cm")) { moveFloor(0.05) }.buttonStyle(.bordered)
            }
            .disabled(head == nil || noWorld)
            Slider(value: Binding(get: { floorY ?? center }, set: { setManualFloor($0) }), in: (center - 1)...(center + 1))
                .disabled(head == nil || noWorld)
            if noWorld {
                Text(VQL("Sin permiso de seguimiento del entorno: el suelo no se puede fijar ni guardar.",
                         "No world-sensing permission: the floor cannot be set or saved."))
                    .font(.caption).foregroundStyle(.orange)
            }
            Text(VQL("También con el stick vertical derecho de un mando conectado. El suelo no puede quedar a menos de 0,15 m de la cabeza.",
                     "Also with the right vertical stick of a connected controller. The floor cannot be closer than 0.15 m to the head."))
                .font(.caption)
            if handsAvailable {
                Button(touchUntil == nil ? VQL("Tocar el suelo con el mando", "Touch the floor with the controller")
                                         : VQL("Apoya el mando en el suelo… \(touchLeft) s", "Rest the controller on the floor… \(touchLeft) s")) {
                    touchLowest = .infinity
                    touchLeft = 5
                    touchUntil = Date().addingTimeInterval(5)
                }
                .buttonStyle(.bordered)
                .disabled(touchUntil != nil || noWorld)
                Text(VQL("Al pulsar, tienes 5 s para apoyar el mando (o la mano) en el suelo; el suelo baja hasta lo más bajo que alcance.",
                         "After pressing, you have 5 s to rest the controller (or hand) on the floor; the floor lowers to the lowest point reached."))
                    .font(.caption)
            }
        }
    }

    /// The floor the user sets, never closer than 0.1524 m to the head (kl_guardian.h) nor farther than the plausible 2.5 m.
    private func setManualFloor(_ y: Float) {
        guard head != nil, !noWorld else { return }
        KLGuardian.setFloor(min(max(y, headY - 2.5), headY - 0.1524), source: .manual)
        floorY = KLGuardian.floorY()
        floorTouched = true
        if sliderCenter == nil { sliderCenter = live.detectedFloorY ?? floorY }
    }

    private func moveFloor(_ delta: Float) { setManualFloor((floorY ?? (headY - 1.15)) + delta) }

    /// Lowest tracked controller / hand height (the pose kl_ovrp publishes, the head's frame); nil when none is tracked.
    private func lowestHandY() -> Float? {
        var best: Float?
        for hand in 0..<2 {
            var pos: [Float] = [0, 0, 0]
            guard kl_ovrp_hand_motion(Int32(hand), &pos, nil, nil, nil) != 0, pos[1].isFinite else { continue }
            best = min(best ?? pos[1], pos[1])
        }
        return best
    }

    /// 60 Hz: reads the head, the floor and the hands; applies the right stick and the "touch the floor" countdown.
    private func tick() {
        if let h = KLGuardian.headPosition(), head == nil || simd_distance(h, head!) > 0.005 { head = h }
        let f = KLGuardian.floorY()
        if f != floorY { floorY = f }
        let lowest = lowestHandY()
        if (lowest != nil) != handsAvailable { handsAvailable = lowest != nil }
        guard let h = head else { return }
        if let until = touchUntil {
            if let y = lowest { touchLowest = min(touchLowest, y) }
            let left = max(0, Int(until.timeIntervalSinceNow.rounded(.up)))
            if left != touchLeft { touchLeft = left }
            if Date() >= until {
                touchUntil = nil
                if touchLowest.isFinite { KLGuardian.lowerFloorTo(touchLowest); floorY = KLGuardian.floorY(); floorTouched = true }
                touchLowest = .infinity
            }
            return
        }
        // Only with the stick moved: nudge also pulls the floor to a hand below it and marks it manual.
        let stick = GCController.controllers().first?.extendedGamepad?.rightThumbstick.yAxis.value ?? 0
        if abs(stick) >= 0.1, !noWorld {
            KLGuardian.nudge(stickY: stick, headY: h.y, controllerY: lowest ?? .nan)
            floorY = KLGuardian.floorY()
            floorTouched = true
        }
    }

    // MARK: map

    private var mapSection: some View {
        // Snapshots: the canvas closure only reads these locals.
        let outline = choice == .room ? roomOutline : live.boundary
        let play: (w: Float, d: Float, cx: Float, cz: Float, yaw: Float)? = choice == .room && !roomOutline.isEmpty ? KLGuardian.playspace() : nil
        let headXZ = head.map { SIMD2<Float>($0.x, $0.z) }
        let rect: (Float, Float)? = choice == .rectangle ? (Float(rectW), Float(rectD)) : nil
        let circle: Float? = choice == .stationary ? Float(radius) : nil
        return Section(VQL("Mapa (vista desde arriba)", "Map (top view)")) {
            Canvas { ctx, size in
                let corners: [(Float, Float)] = [(-1, -1), (1, -1), (1, 1), (-1, 1)]
                var playPoly: [SIMD2<Float>] = []
                if let p = play {
                    let c = cosf(p.yaw), s = sinf(p.yaw)
                    playPoly = corners.map { k -> SIMD2<Float> in
                        let u = k.0 * p.w / 2, v = k.1 * p.d / 2
                        return SIMD2<Float>(p.cx + c * u - s * v, p.cz + s * u + c * v)   // kl_guardian.c: turn yaw, w along u
                    }
                }
                var preview: [SIMD2<Float>] = []
                if let r = rect, let h = headXZ {
                    preview = corners.map { SIMD2<Float>(h.x + $0.0 * r.0 / 2, h.y + $0.1 * r.1 / 2) }
                }
                var extent: [SIMD2<Float>] = outline + playPoly + preview
                if let h = headXZ {
                    extent.append(h)
                    if let r = circle { extent.append(h + SIMD2<Float>(r, r)); extent.append(h - SIMD2<Float>(r, r)) }
                }
                guard let first = extent.first else { return }
                var lo = first, hi = first
                for p in extent { lo = simd_min(lo, p); hi = simd_max(hi, p) }
                let span = max(hi.x - lo.x, hi.y - lo.y, 1) + 1
                let scale = CGFloat(min(size.width, size.height)) / CGFloat(span)
                let mid = (lo + hi) / 2
                func pt(_ p: SIMD2<Float>) -> CGPoint {
                    CGPoint(x: size.width / 2 + CGFloat(p.x - mid.x) * scale, y: size.height / 2 + CGFloat(p.y - mid.y) * scale)
                }
                func stroke(_ poly: [SIMD2<Float>], _ color: Color, _ width: CGFloat) {
                    guard let f = poly.first else { return }
                    var path = Path()
                    path.move(to: pt(f))
                    for p in poly.dropFirst() { path.addLine(to: pt(p)) }
                    path.closeSubpath()
                    ctx.stroke(path, with: .color(color), lineWidth: width)
                }
                if outline.count >= 3 { stroke(outline, .gray, 2) }
                if !playPoly.isEmpty { stroke(playPoly, .blue, 2) }
                if !preview.isEmpty { stroke(preview, .green, 2) }
                if let h = headXZ {
                    if let r = circle {
                        let c = pt(h), pr = CGFloat(r) * scale
                        ctx.stroke(Path(ellipseIn: CGRect(x: c.x - pr, y: c.y - pr, width: 2 * pr, height: 2 * pr)), with: .color(.green), lineWidth: 2)
                    }
                    let c = pt(h)
                    ctx.fill(Path(ellipseIn: CGRect(x: c.x - 5, y: c.y - 5, width: 10, height: 10)), with: .color(.yellow))
                }
            }
            .frame(height: 280)
            Text(VQL("Gris: contorno de la sala. Azul: rectángulo jugable dentro de ella. Verde: la zona que elijas. Amarillo: tu cabeza.",
                     "Grey: room outline. Blue: the playable rectangle inside it. Green: the area you choose. Yellow: your head."))
                .font(.caption)
        }
    }

    // MARK: area

    /// Longest side of the room outline's bounding box (m), 0 without one.
    private var roomExtent: Float {
        guard let f = roomOutline.first else { return 0 }
        var lo = f, hi = f
        for p in roomOutline { lo = simd_min(lo, p); hi = simd_max(hi, p) }
        return max(hi.x - lo.x, hi.y - lo.y)
    }

    /// The room's own outline: nothing when the boundary in use was set by the user (a restored rectangle is not the room).
    private var roomOutline: [SIMD2<Float>] { KLGuardian.boundarySource == 2 ? [] : live.boundary }

    private var roomTooSmall: Bool { choice == .room && !roomOutline.isEmpty && !live.playspaceOK }
    private var roomTooBig: Bool { choice == .room && roomExtent > 15 }
    private var canConfirm: Bool {
        guard head != nil else { return false }
        if choice == .room { return !roomOutline.isEmpty && !roomTooBig }
        return true
    }

    private var areaSection: some View {
        Section(VQL("Zona de juego", "Play area")) {
            Picker(VQL("Modo", "Mode"), selection: $choice) {
                Text(VQL("Sala (contorno de Vision Pro)", "Room (Vision Pro outline)")).tag(Choice.room)
                Text(VQL("Rectángulo (ancho × fondo)", "Rectangle (width × depth)")).tag(Choice.rectangle)
                Text(VQL("Estacionario (radio)", "Stationary (radius)")).tag(Choice.stationary)
            }
            switch choice {
            case .room:
                if roomOutline.isEmpty {
                    Text(VQL("Vision Pro aún no ha detectado la sala. Mira alrededor o elige otro modo.", "Vision Pro has not detected the room yet. Look around or pick another mode."))
                        .font(.caption).foregroundStyle(.orange)
                }
            case .rectangle:
                Stepper(VQL("Ancho: \(String(format: "%.1f", rectW)) m", "Width: \(String(format: "%.1f", rectW)) m"), value: $rectW, in: 1.2...6, step: 0.1)
                Stepper(VQL("Fondo: \(String(format: "%.1f", rectD)) m", "Depth: \(String(format: "%.1f", rectD)) m"), value: $rectD, in: 1.2...6, step: 0.1)
                Text(VQL("Se centra en tu cabeza al confirmar.", "Centred on your head when you confirm.")).font(.caption)
            case .stationary:
                Stepper(VQL("Radio: \(String(format: "%.1f", radius)) m", "Radius: \(String(format: "%.1f", radius)) m"), value: $radius, in: 0.5...2.5, step: 0.1)
            }
            if roomTooSmall || (choice == .rectangle && min(rectW, rectD) < 1.15) {
                Text(VQL("La zona mide menos de 1,15 m de lado: se usará el modo estacionario.", "The area is under 1.15 m on a side: stationary mode will be used."))
                    .font(.caption).foregroundStyle(.orange)
            }
            if roomTooBig {
                Text(VQL("El contorno de la sala mide \(String(format: "%.0f", roomExtent)) m (más de 15 m): no es válido. Usa el modo Rectángulo.",
                         "The room outline is \(String(format: "%.0f", roomExtent)) m (over 15 m): not valid. Use the Rectangle mode."))
                    .font(.caption).foregroundStyle(.red)
            } else if choice == .room && roomExtent > 8 {
                Text(VQL("El contorno de la sala mide \(String(format: "%.0f", roomExtent)) m (más de 8 m): comprueba que es correcto.",
                         "The room outline is \(String(format: "%.0f", roomExtent)) m (over 8 m): check that it is right."))
                    .font(.caption).foregroundStyle(.orange)
            }
        }
    }

    private var buttonsSection: some View {
        Section {
            HStack {
                Button(VQL("Cancelar", "Cancel"), role: .cancel) { end(.cancelled) }.buttonStyle(.bordered)
                Button(VQL("Omitir", "Skip")) { skip() }.buttonStyle(.bordered)
                Button(VQL("Confirmar", "Confirm")) { confirm() }.buttonStyle(.borderedProminent).disabled(!canConfirm)
            }
            Text(VQL("Omitir deja la definición automática y no guarda nada. Confirmar la guarda para este juego.",
                     "Skip keeps the automatic definition and saves nothing. Confirm saves it for this game."))
                .font(.caption)
        }
    }

    // MARK: finish

    private func confirm() {
        guard let game else { return }
        if !noWorld { KLGuardian.confirmFloor() }
        let result: Int
        let saved: KLGuardian.SavedArea
        switch choice {
        case .room:
            result = KLGuardian.setBoundary(points: roomOutline, mode: .roomscale, radius: 0)
            saved = .room
        case .rectangle:
            // counter-clockwise, centred on the head
            result = KLGuardian.setBoundary(points: KLGuardian.rectangle(w: Float(rectW), d: Float(rectD), around: head),
                                            mode: .roomscale, radius: 0)
            saved = .rectangle(w: Float(rectW), d: Float(rectD))
        case .stationary:
            result = KLGuardian.setBoundary(points: [], mode: .stationary, radius: Float(radius))
            saved = .stationary(radius: Float(radius))
        }
        KLGuardian.save(game: game, area: saved)
        VQLog.log("Guardián: confirmado (\(choice)) -> \(result), suelo \(floorY.map { String(format: "%.3f", $0) } ?? "-") (\(live.floorSource)), cabeza \(String(format: "%.3f", headY))")
        end(.confirmed)
    }

    /// Skip saves nothing: a floor set by hand on this screen and not confirmed goes back to the automatic one.
    private func skip() {
        if floorTouched, KLGuardian.floorSource() == .manual {
            KLGuardian.setFloor(0, source: .unset)
            if let auto = KLFloorEstimator.latest { KLGuardian.setFloor(auto, source: .auto) }
        }
        end(.skipped)
    }

    private func end(_ outcome: VQGuardianGate.Outcome) {
        finished = true
        Task { @MainActor in VQGuardianGate.finish(outcome) }
        dismissWindow(id: VQGuardianGate.windowID)
    }

    private func meters(_ y: Float) -> String { String(format: "%.2f m", y) }
}
