// VisionQuestPro: the app's own log. Every diagnostic goes here (and to the system log): there is
// no Mac to attach. Documents/Registros/app.txt, shared from the app; the guest's own log is
// Documents/klepton-boot.log (kl_app.c).

import Foundation

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

/// Spanish when the system language is Spanish, else English.
func VQL(_ spanish: String, _ english: String) -> String {
    (Locale.preferredLanguages.first ?? "en").lowercased().hasPrefix("es") ? spanish : english
}
