// VisionQuestPro: the guardian screen shown before each game (setting «Definir el guardián antes de cada juego»).
//
// It appears with the game's immersive space already open (ARKit is capturing floor and room) and the game NOT started: the
// compositor (KleptonCompositor.renderLoop) waits for VQGuardianGate.run before kl_app_guest_start. The rules (the 0.1524 m
// cap under the head, the 1.15 m minimum side, the stationary fallback) live in kl_guardian.c; this file only asks for them.
//
// As on a Quest: passthrough is on while the screen is up (kl_passthrough_force, nothing of the game is drawn yet), the room's mesh and a
// grid at the floor are drawn in the space (KleptonGuardianDraw.swift), and the player sets the floor by grabbing that grid with the
// controller's grip (or a hand's pinch without controllers) and sliding it to the real floor, up or down; letting go keeps it.
// Then the player DRAWS his play area on that floor: the controller's ray against the floor plane, trigger (or pinch without
// controllers) places a vertex, getting within 0.3 m of the first one or «Cerrar» closes it (kl_guardian_zone_*, which validates and sets the
// boundary); or he picks «Estacionario», the small circle. Confirm stores the floor and the zone as the global calibration, anchored to a
// WorldAnchor (VQCalibration), which later launches apply by themselves without this screen; the per-game save keeps no polygon.

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

    /// True when this game goes through `launch` before it starts (callable from the compositor thread): the guardian screen is on or a
    /// redo was asked, or a global calibration (VQCalibration) is waiting to be applied.
    static func applies(to game: VQGame) -> Bool {
        (VQSettings.shared.guardianAsk || VQCalibration.redo || VQCalibration.exists) && !VQConformance.isConformance(game)
    }

    /// The global calibration first: when it applies the screen is skipped. Otherwise the screen, if it is on («Definir el guardián antes de
    /// cada juego») or a redo was asked; with it off and no calibration the game goes on with the game's saved definition or the automatic one.
    @MainActor static func launch(game: VQGame) async -> Outcome {
        if await VQCalibration.applyAtLaunch() { return .skipped }
        return VQSettings.shared.guardianAsk || VQCalibration.redo ? await run(game: game) : .skipped
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
        KLGuardian.zoneClear()      // a draft left by another game in this process never reaches this screen
        // Real passthrough and the room mesh / floor grid while the screen is up; released below whatever ends it.
        kl_passthrough_force(1)
        KLGuardianScene.floorPending = KLGuardian.floorSource() != .manual   // a manual floor (redo) is the start; else the grid starts at head height
        KLGuardianScene.active = true
        KleptonChroma.shared.followPassthroughNow()
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
        KLGuardianScene.active = false
        kl_passthrough_force(0)
        KleptonChroma.shared.followPassthroughNow()
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
        KLGuardian.grabFloor(pressed: false, handY: .nan, headY: .nan)   // whatever ends the screen, no grab is left held in C
        KLGuardianScene.setCursor(nil, closing: false)
        KLGuardianScene.zoneVisible = false
        KLGuardianScene.floorPending = false
        if outcome != .confirmed { KLGuardian.zoneClear() }              // a zone drawn and abandoned does not reach the game
        guard let c = waiter else { return }
        waiter = nil
        c.resume(returning: outcome)
    }
}

struct VQGuardianSetupView: View {
    /// What the play area is made from on Confirm.
    enum Choice: Hashable { case draw, stationary }

    @ObservedObject private var live = KLGuardianLive.shared
    @Environment(\.dismissWindow) private var dismissWindow
    private let game: VQGame?
    private let hasSaved: Bool
    @State private var redefining: Bool
    @State private var choice: Choice
    @State private var head: SIMD3<Float>?
    @State private var floorY: Float?
    @State private var grabHand: Int?         // the hand (0 left, 1 right) holding the floor grid
    @State private var finished = false
    @State private var floorTouched = false   // the user moved the floor on this screen (not confirmed until Confirm)
    @State private var floorChosen = !KLGuardianScene.floorPending   // the floor is the player's (grabbed, detected one picked, or a manual one to start from)
    @State private var zone: [SIMD2<Float>] = []      // the zone being drawn (a copy of kl_guardian's draft, for the map)
    @State private var zoneClosed = false
    @State private var cursor: SIMD2<Float>?           // where the pointing hand meets the floor
    @State private var triggerDown = [false, false]   // per hand, last frame (a vertex goes in on the press, not while held)
    @State private var zoneNote: String?
    @State private var floorDone = false               // hands only: the floor is set, so a pinch near it draws instead of grabbing the grid

    init() {
        let g = VQLaunch.choice?.game
        game = g
        let saved = g.map { KLGuardian.hasSaved(game: $0) } ?? false
        hasSaved = saved
        // Only a saved stationary circle can be reused as it is: a drawn polygon is never saved (and an older rectangle / room is gone).
        let kind = g.flatMap { KLGuardian.savedKind(game: $0) }
        _redefining = State(initialValue: !saved || kind != "stationary" || VQCalibration.redo)
        let area = VQGameArea.load(key: g?.aaKey ?? "")
        // Drawing needs ARKit's world tracking (the floor plane and the hands live in that world): without it only the circle is possible.
        _choice = State(initialValue: area.seated || kind == "stationary" || KLGuardian.worldTrackingDenied ? .stationary : .draw)
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
                KLGuardian.zoneClear()      // a fresh screen starts with no draft (not in init: SwiftUI may rebuild the struct mid-drawing)
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
                Button(VQL("Rehacer calibración", "Redo calibration")) { redefining = true }.buttonStyle(.bordered)
                Button(VQL("Usar la guardada", "Use the saved one")) { end(.skipped) }.buttonStyle(.borderedProminent)
            }
        }
    }

    private var savedAreaText: String {
        if KLGuardian.mode == .stationary { return VQL("Estacionario, radio \(String(format: "%.1f", KLGuardian.radius)) m", "Stationary, radius \(String(format: "%.1f", KLGuardian.radius)) m") }
        if let p = KLGuardian.playspace() { return String(format: "%.1f × %.1f m", p.w, p.d) }
        return VQL("Automática", "Automatic")
    }

    // MARK: floor

    private var headY: Float { head?.y ?? live.headY }
    /// No world tracking (ARKit .worldSensing denied): the floor cannot be set or saved manually.
    private var noWorld: Bool { KLGuardian.worldTrackingDenied }

    /// Quest's floor step: no numbers, no sliders; the grid in the room is the floor and the player moves it with the hand.
    private var floorSection: some View {
        Section(VQL("Suelo", "Floor")) {
            LabeledContent(VQL("Altura del suelo", "Floor height"), value: floorChosen ? (floorY.map(meters) ?? VQL("Sin detectar", "Not detected")) : VQL("Sin fijar", "Not set"))
            Text(grabHand != nil
                 ? VQL("Mueve la mano hasta el suelo real y suelta para fijarlo.", "Move your hand to the real floor and let go to set it.")
                 : VQL("Agarra la rejilla del suelo con el grip del mando (o pellizca con la mano si no usas mandos) y deslízala hasta el suelo real, hacia arriba o hacia abajo. Al soltar queda fijada.",
                       "Grab the floor grid with the controller's grip (or pinch with your hand if you have no controllers) and slide it to the real floor, up or down. Letting go sets it."))
                .font(.caption)
            if GCController.controllers().isEmpty && !noWorld {
                // Hands: the same pinch near the floor is the grid's grab and a vertex, so the floor is declared done before drawing.
                Button(floorDone ? VQL("Ajustar el suelo otra vez", "Adjust the floor again") : VQL("Suelo listo", "Floor done")) { floorDone.toggle() }
                    .buttonStyle(.bordered)
                    .disabled(!floorChosen)
            }
            if let detected = live.detectedFloorY, !noWorld {
                Button(VQL("Usar suelo detectado", "Use detected floor")) {
                    KLGuardian.setFloor(detected, source: .manual)
                    floorTouched = true
                    setFloorChosen()
                }.buttonStyle(.bordered)
            }
            if noWorld {
                Text(VQL("Sin permiso de seguimiento del entorno: el suelo no se puede fijar ni guardar.",
                         "No world-sensing permission: the floor cannot be set or saved."))
                    .font(.caption).foregroundStyle(.orange)
            } else if !floorChosen {
                Text(VQL("Baja la rejilla hasta el suelo.", "Lower the grid to the floor."))
                    .font(.caption).foregroundStyle(.orange)
            } else if let f = floorY, head != nil, !floorConfirmable(f) {
                Text(VQL("El suelo no parece plausible respecto a tu cabeza (debe estar entre 0,3 y 2,5 m por debajo). Ajústalo.",
                         "The floor does not look plausible against your head (it should be 0.3 to 2.5 m below). Adjust it."))
                    .font(.caption).foregroundStyle(.orange)
            }
        }
    }

    /// A grab (grip or pinch) starts only with the hand / controller near the grid, as on a Quest where you reach for it: a pinch that
    /// presses a button in this window must not drag the floor.
    private static let grabReach: Float = 0.25

    /// Floor accepted to confirm: 0.3 to 2.5 m under the head (KLGuardian.plausible's SLAM minimum of 0.7 m would refuse a kneeling / lying player,
    /// though the grab allows the floor up to head - 0.1524).
    private func floorConfirmable(_ f: Float) -> Bool {
        let d = f - headY
        return d.isFinite && d >= -2.5 - 1e-5 && d <= -0.3 + 1e-5
    }

    /// The floor is now the player's: the grid stops being drawn at head height and Confirm may be allowed.
    private func setFloorChosen() {
        KLGuardianScene.floorPending = false
        if !floorChosen { floorChosen = true }
    }

    /// 60 Hz: reads the head and the floor, and moves the floor with the grabbing hand (kl_guardian_floor_grab keeps the rules).
    private func tick() {
        if let h = KLGuardian.headPosition(), head == nil || simd_distance(h, head!) > 0.005 { head = h }
        let f = KLGuardian.floorY()
        if f != floorY { floorY = f }
        guard let h = head, !noWorld else { return }
        let noControllers = GCController.controllers().isEmpty
        let reference = floorChosen ? floorY ?? (h.y - 1.15) : h.y - 0.1524   // where the grid is drawn: the grab starts there
        var holding: (hand: Int, y: Float)?
        for hand in (grabHand == 1 ? [1, 0] : [0, 1]) {
            var pos: [Float] = [0, 0, 0]
            var buttons: UInt32 = 0
            var index: Float = 0, grip: Float = 0
            guard kl_ovrp_hand_motion(Int32(hand), &pos, nil, nil, nil) != 0, pos[1].isFinite,
                  kl_ovrp_controller_input(Int32(hand), &buttons, nil, &index, &grip, nil, nil) != 0 else { continue }
            let gripDown = grip > 0.5 || buttons & (hand == 0 ? OVRPRawButton.lHandTrigger : OVRPRawButton.rHandTrigger) != 0
            let pinch = noControllers && index > 0.5 && !floorDone
            // A hand with no controller pose is skipped above; without controllers connected the pinch takes the grip's place.
            let started = grabHand == hand || abs(pos[1] - reference) <= Self.grabReach
            if (gripDown || pinch) && started { holding = (hand, pos[1]); break }
        }
        if let hold = holding {
            grabHand = hold.hand
            KLGuardian.grabFloor(pressed: true, handY: hold.y, headY: h.y)
            floorTouched = true
            setFloorChosen()
        } else if grabHand != nil {
            KLGuardian.grabFloor(pressed: false, handY: .nan, headY: h.y)
            grabHand = nil
        }
        floorY = KLGuardian.floorY()
        drawStep(noControllers: noControllers)
    }

    // MARK: drawing

    /// Where the hand points on the floor plane (y = the confirmed floor): the controller's ray (its pose, forward = -Z of the grip pose
    /// the guest gets) against that plane, at most 12 m away and pointing at least 3 degrees down; with hands and no controller, the hand
    /// itself projected straight down, and ONLY within 25 cm over the floor (a pinch anywhere else is the system's UI, not a vertex).
    /// nil when it does not meet the floor.
    private func floorPoint(pos: [Float], quat: [Float], noControllers: Bool, floorY: Float) -> SIMD2<Float>? {
        guard pos.count >= 3, pos.allSatisfy({ $0.isFinite }) else { return nil }
        if noControllers {                       // hands: only touching the floor counts (at most 25 cm over it, 15 cm under for tracking noise)
            let dy = pos[1] - floorY
            return dy <= Self.grabReach && dy >= -0.15 ? SIMD2<Float>(pos[0], pos[2]) : nil
        }
        guard quat.count >= 4, quat.allSatisfy({ $0.isFinite }) else { return nil }
        let q = simd_quatf(ix: quat[0], iy: quat[1], iz: quat[2], r: quat[3])
        guard q.length > 0.5 else { return nil }
        let dir = simd_normalize(q).act(SIMD3<Float>(0, 0, -1))
        guard dir.y < -0.05 else { return nil }
        let t = (floorY - pos[1]) / dir.y
        guard t > 0, t < 12 else { return nil }
        return SIMD2<Float>(pos[0] + dir.x * t, pos[2] + dir.z * t)
    }

    /// 60 Hz while «Dibujar zona» is on: follows the pointing hand, and a trigger press (a pinch with the hand down at the floor, without
    /// controllers) places a vertex. Nothing is placed while the floor is being grabbed or, with hands, before «Suelo listo». @State is
    /// only assigned when it changes (the body is not redrawn at 60 Hz for nothing).
    private func drawStep(noControllers: Bool) {
        let state = KLGuardian.zone()
        if state.points != zone { zone = state.points }
        if state.closed != zoneClosed { zoneClosed = state.closed }
        KLGuardianScene.zoneVisible = choice == .draw
        guard choice == .draw, !state.closed, grabHand == nil, floorChosen, !noControllers || floorDone, let floor = floorY else {
            if cursor != nil { cursor = nil }
            KLGuardianScene.setCursor(nil, closing: false)
            return
        }
        var point: SIMD2<Float>?
        for hand in [1, 0] {
            var pos: [Float] = [0, 0, 0], quat: [Float] = [0, 0, 0, 1]
            var index: Float = 0
            guard kl_ovrp_hand_motion(Int32(hand), &pos, &quat, nil, nil) != 0,
                  kl_ovrp_controller_input(Int32(hand), nil, nil, &index, nil, nil, nil) != 0 else {
                if triggerDown[hand] { triggerDown[hand] = false }
                continue
            }
            let p = floorPoint(pos: pos, quat: quat, noControllers: noControllers, floorY: floor)
            let down = index > 0.5
            let rising = down && !triggerDown[hand]
            if triggerDown[hand] != down { triggerDown[hand] = down }
            if point == nil || down { point = p }
            if rising, let p {
                let r = KLGuardian.zoneAdd(p)
                let note: String? = r == -2 ? zoneReason(KLGuardian.zoneValidate()) : (r >= 0 ? nil : zoneNote)
                if note != zoneNote { zoneNote = note }
            }
        }
        if let pt = point {
            if cursor == nil || simd_distance(cursor!, pt) > 0.01 { cursor = pt }
        } else if cursor != nil {
            cursor = nil
        }
        let near = point.map { p in zone.count >= 3 && simd_distance(p, zone[0]) <= Float(KLG_ZONE_CLOSE_DIST) } ?? false
        KLGuardianScene.setCursor(point, closing: near)
        let state2 = KLGuardian.zone()
        if state2.points != zone { zone = state2.points }
        if state2.closed != zoneClosed { zoneClosed = state2.closed }
    }

    private func zoneReason(_ code: Int) -> String? {
        switch Int32(code) {
        case Int32(KLG_ZONE_OK): return nil
        case Int32(KLG_ZONE_TOO_FEW): return VQL("Pon al menos 3 puntos.", "Place at least 3 points.")
        case Int32(KLG_ZONE_CROSSING): return VQL("Los lados se cruzan: deshaz el último punto.", "The sides cross: undo the last point.")
        case Int32(KLG_ZONE_TOO_SMALL): return VQL("La zona es demasiado pequeña (1,15 m de lado como mínimo).", "The area is too small (1.15 m on a side at least).")
        default: return VQL("La zona no es válida.", "The area is not valid.")
        }
    }

    // MARK: map

    private var mapSection: some View {
        // Snapshots: the canvas closure only reads these locals.
        let draft = zone
        let closed = zoneClosed
        let headXZ = head.map { SIMD2<Float>($0.x, $0.z) }
        let pointer = cursor
        let circle: Float? = choice == .stationary ? Float(KLG_DEFAULT_RADIUS) : nil
        return Section(VQL("Mapa (vista desde arriba)", "Map (top view)")) {
            Canvas { ctx, size in
                var extent: [SIMD2<Float>] = draft
                if let h = headXZ {
                    extent.append(h)
                    if let r = circle { extent.append(h + SIMD2<Float>(r, r)); extent.append(h - SIMD2<Float>(r, r)) }
                }
                if let c = pointer { extent.append(c) }
                guard let first = extent.first else { return }
                var lo = first, hi = first
                for p in extent { lo = simd_min(lo, p); hi = simd_max(hi, p) }
                let span = max(hi.x - lo.x, hi.y - lo.y, 1) + 1
                let scale = CGFloat(min(size.width, size.height)) / CGFloat(span)
                let mid = (lo + hi) / 2
                func pt(_ p: SIMD2<Float>) -> CGPoint {
                    CGPoint(x: size.width / 2 + CGFloat(p.x - mid.x) * scale, y: size.height / 2 + CGFloat(p.y - mid.y) * scale)
                }
                if let f = draft.first {
                    var path = Path()
                    path.move(to: pt(f))
                    for p in draft.dropFirst() { path.addLine(to: pt(p)) }
                    if closed { path.closeSubpath() } else if let c = pointer { path.addLine(to: pt(c)) }
                    ctx.stroke(path, with: .color(.green), lineWidth: 2)
                    for p in draft {
                        let c = pt(p)
                        ctx.fill(Path(ellipseIn: CGRect(x: c.x - 4, y: c.y - 4, width: 8, height: 8)), with: .color(.green))
                    }
                }
                if let c = pointer.map(pt), !closed {
                    ctx.stroke(Path(ellipseIn: CGRect(x: c.x - 6, y: c.y - 6, width: 12, height: 12)), with: .color(.white), lineWidth: 2)
                }
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
            Text(VQL("Verde: la zona que dibujas (o el círculo estacionario). Blanco: dónde apunta tu mano en el suelo. Amarillo: tu cabeza.",
                     "Green: the area you draw (or the stationary circle). White: where your hand points on the floor. Yellow: your head."))
                .font(.caption)
        }
    }

    // MARK: area

    private var canConfirm: Bool {
        guard head != nil else { return false }
        // The floor must be the player's and below the head (as the grid starts at head height, a confirm without lowering it is refused).
        if !noWorld { guard floorChosen, let f = floorY, floorConfirmable(f) else { return false } }
        return choice == .stationary || zoneClosed          // a drawn zone must be closed and valid (kl_guardian_zone_close checked it)
    }

    private var areaSection: some View {
        Section(VQL("Zona de juego", "Play area")) {
            Picker(VQL("Modo", "Mode"), selection: $choice) {
                Text(VQL("Dibujar zona", "Draw area")).tag(Choice.draw)
                Text(VQL("Estacionario", "Stationary")).tag(Choice.stationary)
            }
            switch choice {
            case .draw:
                Text(zoneClosed
                     ? VQL("Zona cerrada: \(zone.count) puntos. Confirma, o usa Deshacer para reabrirla.", "Area closed: \(zone.count) points. Confirm, or use Undo to reopen it.")
                     : VQL("Apunta con el mando al suelo y pulsa el gatillo para poner un punto. Con manos: pulsa «Suelo listo», baja la mano al suelo y pellizca (un pellizco más arriba no pone nada). Acércate a menos de 30 cm del primero, o pulsa Cerrar, para cerrar la zona. Puntos: \(zone.count).",
                           "Point the controller at the floor and press the trigger to place a point. With hands: press «Floor done», lower your hand to the floor and pinch (a pinch higher up places nothing). Get within 30 cm of the first one, or press Close, to close the area. Points: \(zone.count)."))
                    .font(.caption)
                if noWorld {
                    Text(VQL("Dibujar zona requiere seguimiento del entorno; usa Estacionario.", "Drawing the area needs world tracking; use Stationary."))
                        .font(.caption).foregroundStyle(.orange)
                }
                HStack {
                    Button(VQL("Deshacer", "Undo")) { KLGuardian.zoneUndo(); zoneNote = nil }.buttonStyle(.bordered).disabled(zone.isEmpty)
                    Button(VQL("Borrar", "Clear")) { KLGuardian.zoneClear(); zoneNote = nil }.buttonStyle(.bordered).disabled(zone.isEmpty)
                    Button(VQL("Cerrar", "Close")) {
                        zoneNote = zoneReason(KLGuardian.zoneClose())
                    }.buttonStyle(.bordered).disabled(zoneClosed || zone.count < 3)
                }
                if let n = zoneNote { Text(n).font(.caption).foregroundStyle(.orange) }
            case .stationary:
                Text(VQL("Un círculo pequeño alrededor de ti, como en Quest cuando no dibujas una zona.", "A small circle around you, as on a Quest when you do not draw an area."))
                    .font(.caption)
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
            Text(VQL("Omitir deja la definición automática y no guarda nada. Confirmar la guarda como calibración para todos los juegos.",
                     "Skip keeps the automatic definition and saves nothing. Confirm saves it as the calibration for every game."))
                .font(.caption)
        }
    }

    // MARK: finish

    private func confirm() {
        guard let game else { return }
        if !noWorld {
            // Confirming accepts the floor as shown: ARKit's proposal becomes the player's (manual), so it is saved and ARKit's later
            // estimates no longer move it during the game.
            if KLGuardian.floorSource() == .auto, let y = KLGuardian.floorY() { KLGuardian.setFloor(y, source: .manual) }
            KLGuardian.confirmFloor()
        }
        let result: Int
        let saved: KLGuardian.SavedArea
        switch choice {
        case .draw:
            // kl_guardian_zone_close already set the boundary (roomscale, user source); the polygon is not saved.
            result = zoneClosed ? 1 : -1
            saved = .drawn
        case .stationary:
            KLGuardian.zoneClear()                       // a drawn zone, if any, gives way to the circle
            result = KLGuardian.setBoundary(points: [], mode: .stationary, radius: Float(KLG_DEFAULT_RADIUS))
            saved = .stationary(radius: Float(KLG_DEFAULT_RADIUS))
        }
        KLGuardian.save(game: game, area: saved)
        if result == 1, !noWorld, let fy = KLGuardian.floorY(), let h = head {
            // The global calibration: the floor and the zone (as drawn) anchored to a WorldAnchor; the redo mark goes once it is stored.
            let drawn = choice == .draw ? KLGuardian.zone().points : []
            Task { await VQCalibration.save(zone: drawn, head: h, floorY: fy) }
        } else if noWorld {
            VQCalibration.redo = false                   // without world sensing nothing can be anchored: do not ask again every game
        }
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
