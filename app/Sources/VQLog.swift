// VisionQuestPro: the app's own log. Every diagnostic goes here (and to the system log): there is
// no Mac to attach. Documents/Registros/app.txt, shared from the app; the guest's own log is
// Documents/klepton-boot.log (kl_app.c).

import Foundation
import Metal

enum VQLog {
    private static let lock = NSLock()
    private static var handle: FileHandle?

    static var directory: URL {
        FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("Registros", isDirectory: true)
    }
    static var appLog: URL { directory.appendingPathComponent("app.txt") }
    static var bootLog: URL {
        FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0].appendingPathComponent("klepton-boot.log")
    }
    static var crashLog: URL {
        FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0].appendingPathComponent("klepton-crash.log")
    }

    /// Starts a new log for this session (the previous one is kept as app-anterior.txt).
    static func start() {
        lock.lock(); defer { lock.unlock() }
        try? FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        let previous = directory.appendingPathComponent("app-anterior.txt")
        try? FileManager.default.removeItem(at: previous)
        try? FileManager.default.moveItem(at: appLog, to: previous)
        for old in captures { try? FileManager.default.removeItem(at: old) }  // previous session's PNGs
        FileManager.default.createFile(atPath: appLog.path, contents: nil)
        handle = try? FileHandle(forWritingTo: appLog)
        let info = Bundle.main.infoDictionary ?? [:]
        let line = "VisionQuestPro \(info["CFBundleShortVersionString"] ?? "?") (\(info["CFBundleVersion"] ?? "?")), "
            + "visionOS \(ProcessInfo.processInfo.operatingSystemVersionString)\n"
        handle?.write(Data(line.utf8))
    }

    static func log(_ message: String) {
        NSLog("[vq] %@", message)
        let stamp = ISO8601DateFormatter.string(from: Date(), timeZone: .current, formatOptions: [.withTime, .withColonSeparatorInTime])
        lock.lock(); defer { lock.unlock() }
        handle?.write(Data("\(stamp) \(message)\n".utf8))
    }

    /// The logs that exist, for the share button.
    static var shareable: [URL] {
        [appLog, directory.appendingPathComponent("app-anterior.txt"), bootLog, crashLog,
         directory.appendingPathComponent("conformidad-vulkan.txt"),
         directory.appendingPathComponent("conformidad-detalle.txt")]
            .filter { FileManager.default.fileExists(atPath: $0.path) }
            + captures
    }

    /// Copies the logs to QUEST/Registros/<date>-<label>/ (Files app): a way to get them that does
    /// not go through the system share sheet (log 60: the sheet aborted the app inside
    /// ExtensionFoundation once new connections to system services were failing). Keeps the last 12.
    @discardableResult
    static func saveToQuest(_ label: String) -> URL? {
        guard let root = VQFolder.shared.url else { return nil }
        let fm = FileManager.default
        let base = root.appendingPathComponent("Registros", isDirectory: true)
        let f = DateFormatter()
        f.locale = Locale(identifier: "en_US_POSIX")
        f.dateFormat = "yyyy-MM-dd_HH-mm-ss"
        let dest = base.appendingPathComponent("\(f.string(from: Date()))-\(label)", isDirectory: true)
        do { try fm.createDirectory(at: dest, withIntermediateDirectories: true) } catch {
            log("Registros: no se pudo crear \(dest.path): \(error.localizedDescription)")
            return nil
        }
        var copied = 0
        // A crash report older than the boot log belongs to an earlier session: copying it next to
        // a newer log reads as "this session crashed" (2026-10-15: the 09:55 Wwise fault kept
        // reappearing in every later folder).
        let bootDate = (try? fm.attributesOfItem(atPath: bootLog.path))?[.modificationDate] as? Date
        let crashDate = (try? fm.attributesOfItem(atPath: crashLog.path))?[.modificationDate] as? Date
        let staleCrash = bootDate != nil && crashDate != nil && crashDate! < bootDate!.addingTimeInterval(-60)
        for src in shareable where !(staleCrash && src == crashLog) {
            let to = dest.appendingPathComponent(src.lastPathComponent)
            try? fm.removeItem(at: to)
            if (try? fm.copyItem(at: src, to: to)) != nil { copied += 1 }
        }
        let old = ((try? fm.contentsOfDirectory(at: base, includingPropertiesForKeys: nil)) ?? [])
            .filter { $0.hasDirectoryPath }.sorted { $0.lastPathComponent > $1.lastPathComponent }
        for stale in old.dropFirst(12) { try? fm.removeItem(at: stale) }
        log("Registros: \(copied) fichero(s) copiados a QUEST/Registros/\(dest.lastPathComponent)")
        return dest
    }

    /// At launch: the previous session's logs (guest log, crash report) go to QUEST/Registros once,
    /// so a session that ended in a crash can be read from the Files app.
    static func savePreviousSessionToQuest() {
        let key = "vq.lastSavedBootLogDate"
        guard let date = (try? FileManager.default.attributesOfItem(atPath: bootLog.path))?[.modificationDate] as? Date
        else { return }
        let last = UserDefaults.standard.double(forKey: key)
        guard date.timeIntervalSince1970 > last + 1 else { return }
        if saveToQuest("sesion-anterior") != nil {
            UserDefaults.standard.set(date.timeIntervalSince1970, forKey: key)
        }
    }

    /// Frame captures written by the compositor (capture-<frame>-*.png).
    static var captures: [URL] {
        ((try? FileManager.default.contentsOfDirectory(at: directory, includingPropertiesForKeys: nil)) ?? [])
            .filter { $0.lastPathComponent.hasPrefix("capture-") && $0.pathExtension == "png" }
            .sorted { $0.lastPathComponent < $1.lastPathComponent }
    }

    /// The last `bytes` of a log, for showing in the app.
    static func tail(_ url: URL, bytes: Int = 16_384) -> String {
        guard let h = try? FileHandle(forReadingFrom: url) else { return "" }
        defer { try? h.close() }
        let size = (try? h.seekToEnd()) ?? 0
        try? h.seek(toOffset: size > UInt64(bytes) ? size - UInt64(bytes) : 0)
        return String(decoding: h.readDataToEndOfFile(), as: UTF8.self)
    }
}

/// Every 15 s, a tiny Metal library compiled from a source never seen before (a real trip to
/// MTLCompilerService): logs when the compiler stops being reachable and when it comes back, plus a
/// line every 5 minutes. Log 60: from 2 s into the Vulkan CTS every NEW compiler connection failed
/// ("Connection init failed at lookup"); this says when, against the [hb] ports/fds counts.
enum VQCompilerProbe {
    // Touched only from the timer's serial event handler (and start() once, on the main thread).
    nonisolated(unsafe) private static var timer: DispatchSourceTimer?
    nonisolated(unsafe) private static var lastOK: Bool?
    nonisolated(unsafe) private static var count = 0

    /// The last probe could not reach the compiler: new connections to system services are failing
    /// in this process, and the share sheet would abort the app (log 60). Read from the UI.
    static var servicesBroken: Bool { lastOK == false }

    static func start() {
        guard timer == nil else { return }
        let t = DispatchSource.makeTimerSource(queue: .global(qos: .utility))
        t.schedule(deadline: .now() + 20, repeating: 15)
        t.setEventHandler { probe() }
        timer = t
        t.resume()
    }

    private static func probe() {
        count += 1
        guard let device = MTLCreateSystemDefaultDevice() else { return }
        let n = count
        let source = "#include <metal_stdlib>\nkernel void vq_probe_\(n)(device uint *o [[buffer(0)]]) { o[0] = \(n)u; }\n"
        let t0 = Date()
        var failure: String?
        do { _ = try device.makeLibrary(source: source, options: nil) } catch { failure = error.localizedDescription }
        let ms = Int(Date().timeIntervalSince(t0) * 1000)
        let ok = failure == nil
        if lastOK != ok || n % 20 == 0 {
            let line = ok ? "[xpc] compilador Metal OK (\(ms) ms, sonda \(n))"
                          : "[xpc] compilador Metal INACCESIBLE (sonda \(n), \(ms) ms): \(failure ?? "?")"
            fputs(line + "\n", stderr)
            VQLog.log(line)
        }
        lastOK = ok
    }
}

/// Spanish when the system language is Spanish, else English.
func VQL(_ spanish: String, _ english: String) -> String {
    (Locale.preferredLanguages.first ?? "en").lowercased().hasPrefix("es") ? spanish : english
}
