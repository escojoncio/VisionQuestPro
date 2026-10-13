// VisionQuestPro: the Khronos conformance tests as one more "game" in QUEST/Juegos.
//
// The Vulkan CTS (deqp-vk, built for Android arm64 by .github/workflows/conformance.yml) arrives
// as VulkanCTS.apk: the app prepares it like any game (same conversion, signing, Klepton runtime
// and VPKEngine driver) and runs it through Klepton's executable door (guest kind "exe": its
// main() once per batch). Nothing about the run is special-cased in the graphics path, which is
// the point: what fails here fails for the games too.
//
// The case list (assets/caselists, made by conformance/vulkan/caselists.py) is split into batches.
// Each launch runs batches until the time budget, the memory limit or the Stop button; results
// land in QUEST/Partidas/<test>/cts/<batch>.<part>.qpa. A batch that crashed or hung the app is
// finished on the next launch from the case after the one that was running (that case counts as
// Crash), exactly like the Khronos runner does on Android. The summary goes to the log and to
// Registros/conformidad-vulkan.txt.

import Foundation

enum VQConformance {
    static let folderName = "Vulkan CTS (Khronos)"
    static let openXRPackage = "org.khronos.openxr.cts"
    static let openXRFolderName = "OpenXR CTS (Khronos)"

    /// The OpenXR CTS runs as a native game (immersive, like any OpenXR title) with Khronos's own
    /// launch extras: the Vulkan2 graphics plugin, non-interactive tests only, XML results.
    /// Its console report (pass/fail per test and the totals) lands in the guest log.
    static func prepareOpenXR() {
        let extras = [
            "graphicsPlugin=Vulkan2",
            "args=~[interactive]",
            "xmlFilename=openxr_cts.xml",
        ]
        setenv("KL_INTENT_EXTRAS", extras.joined(separator: "\n"), 1)
        VQLog.log("Conformidad OpenXR: plugin Vulkan2, sin tests interactivos; resultado en el registro del juego y en openxr_cts.xml")
    }
    static let profile = "rapida"

    struct Batch {
        let file: String      // e.g. 004-multiview-001.txt
        let group: String
        let count: Int
        var stem: String { (file as NSString).deletingPathExtension }
    }

    /// One case's outcome, as the .qpa says it.
    struct Outcome {
        let status: String    // Pass, Fail, NotSupported, QualityWarning, CompatibilityWarning, InternalError, Crash, Timeout, ...
        let detail: String
    }

    /// The run in progress (read by the window while main() runs).
    struct Live {
        var batchIndex = 0, batchCount = 0
        var running = false
        var batchName = ""
        var doneInBatch = 0, casesInBatch = 0
        var counts: [String: Int] = [:]
    }

    nonisolated(unsafe) static var plannedBatches: [(batch: Batch, qpa: URL, cases: Int)] = []
    nonisolated(unsafe) static var workDir: URL?
    nonisolated(unsafe) static var progressFile: URL?
    nonisolated(unsafe) static var stopFile: URL?
    nonisolated(unsafe) static var totalCases = 0
    nonisolated(unsafe) static var doneBefore = 0

    static var summaryFile: URL { VQLog.directory.appendingPathComponent("conformidad-vulkan.txt") }

    static func isConformance(_ game: VQGame) -> Bool { game.guestKind == "exe" }

    // MARK: - Before the run

    /// Plans this launch: what is already done (summarised to the log), and the remaining batches
    /// as deqp-vk command lines for Klepton's executable door. Sets the KL_EXE_* environment.
    static func prepareRun(game: VQGame, tree: URL, saves: URL) {
        let fm = FileManager.default
        let work = saves.appendingPathComponent("cts", isDirectory: true)
        try? fm.createDirectory(at: work, withIntermediateDirectories: true)
        workDir = work
        let lists = tree.appendingPathComponent("assets/caselists", isDirectory: true)
        let batches = readIndex(lists.appendingPathComponent("index.txt"))
        totalCases = batches.reduce(0) { $0 + $1.count }
        if batches.isEmpty {
            VQLog.log("Conformidad: no hay listas de casos en \(lists.path) (¿APK de tests antiguo?)")
        }
        let budget = VQSettings.shared.conformanceMinutes * 60
        let memLimit = VQSettings.shared.conformanceMemoryMB

        var lines: [String] = []
        var planned: [(Batch, URL, Int)] = []
        var done = 0
        for b in batches {
            let all = readCases(lists.appendingPathComponent(profile).appendingPathComponent(b.file))
            let results = outcomes(batch: b, in: work)
            let remaining = all.filter { results[$0] == nil }
            done += all.count - remaining.count
            if remaining.isEmpty { continue }
            let part = qpaFiles(batch: b, in: work).count
            let qpa = work.appendingPathComponent("\(b.stem).\(part).qpa")
            let list: URL
            if remaining.count == all.count {
                list = lists.appendingPathComponent(profile).appendingPathComponent(b.file)
            } else {
                list = work.appendingPathComponent("\(b.stem).\(part).txt")
                try? (remaining.joined(separator: "\n") + "\n").write(to: list, atomically: true, encoding: .utf8)
            }
            let args = [
                "--deqp-caselist-file=\(list.path)",
                "--deqp-log-filename=\(qpa.path)",
                "--deqp-archive-dir=\(tree.appendingPathComponent("assets").path)",
                "--deqp-shadercache-filename=\(work.appendingPathComponent("shadercache.bin").path)",
                "--deqp-log-flush=enable",
                "--deqp-log-images=disable",
                "--deqp-log-shader-sources=disable",
                "--deqp-log-decompiled-spirv=disable",
                "--quiet",
            ]
            lines.append(args.joined(separator: "\t"))
            planned.append((b, qpa, remaining.count))
        }
        doneBefore = done
        plannedBatches = planned.map { (batch: $0.0, qpa: $0.1, cases: $0.2) }

        let argsFile = work.appendingPathComponent("run.args")
        try? (lines.joined(separator: "\n") + "\n").write(to: argsFile, atomically: true, encoding: .utf8)
        let progress = work.appendingPathComponent("run.progress")
        try? fm.removeItem(at: progress)
        let stop = work.appendingPathComponent("run.stop")
        try? fm.removeItem(at: stop)
        progressFile = progress
        stopFile = stop
        setenv("KL_EXE_ARGS_FILE", argsFile.path, 1)
        setenv("KL_EXE_PROGRESS", progress.path, 1)
        setenv("KL_EXE_STOP_FILE", stop.path, 1)
        setenv("KL_EXE_BUDGET", String(Int(budget)), 1)
        setenv("KL_EXE_MEM_MB", String(memLimit), 1)
        // Window only: no immersive space, no compositor frames for a test run (a stalled test can
        // never take the immersive compositor down with it).
        setenv("KL_IMMERSIVE", "0", 1)
        // No pipeline prewarm in the driver: every vkCreateDevice of the tests would recompile the
        // recipes of earlier runs (log 59: ~1700 pipelines, 0.7 -> 1.4 GB, per device).
        setenv("VPVK_PREWARM", "0", 1)
        vq_prewarm_set_enabled(0)
        // A hung or faulting GPU command buffer loses the device, as Vulkan defines it: the case
        // ends as DeviceLost and the next one runs on a fresh device (Klepton's exe runner) instead
        // of the whole run waiting forever (log 59, draw).
        setenv("VPVK_DEVICE_LOST_STRICT", "1", 1)

        VQLog.log("Conformidad: \(batches.count) tandas, \(totalCases) casos (perfil \(profile)); hechos \(done), "
                  + "pendientes \(totalCases - done) en \(planned.count) tandas; esta sesión: hasta \(Int(budget / 60)) min, "
                  + "límite de memoria \(memLimit) MB")
        summarize(reason: VQL("antes de la sesión", "before the session"))
    }

    /// Stops after the batch that is running (the app's Stop button).
    static func requestStop() {
        guard let stopFile else { return }
        FileManager.default.createFile(atPath: stopFile.path, contents: Data("stop\n".utf8))
        VQLog.log("Conformidad: parada pedida; termina la tanda en curso")
    }

    // MARK: - While it runs

    /// What the running batch has done so far (cheap enough to poll every couple of seconds).
    static func live() -> Live {
        var l = Live()
        l.batchCount = plannedBatches.count
        guard let progressFile, let text = try? String(contentsOf: progressFile, encoding: .utf8) else { return l }
        let parts = text.split(separator: " ")
        guard let first = parts.first, let index = Int(first.trimmingCharacters(in: .whitespacesAndNewlines)) else { return l }
        let running = parts.count > 1 && parts[1].hasPrefix("running")
        let i = min(index, max(plannedBatches.count - 1, 0))
        guard i < plannedBatches.count else { return l }
        l.batchIndex = i
        l.running = running
        let p = plannedBatches[i]
        l.batchName = p.batch.group
        l.casesInBatch = p.cases
        // This launch's part of the batch and any part a crash recovery started after it
        // (<part>.r<k>.qpa, written by Klepton's exe runner).
        let base = p.qpa.deletingPathExtension().lastPathComponent
        var results: [String: Outcome] = [:]
        for f in qpaFiles(batch: p.batch, in: p.qpa.deletingLastPathComponent())
        where f.lastPathComponent.hasPrefix(base + ".") || f == p.qpa {
            for (k, v) in parse(f, assumeRunning: true) { results[k] = v }
        }
        l.doneInBatch = results.count
        for (_, o) in results { l.counts[o.status, default: 0] += 1 }
        return l
    }

    // MARK: - After the run

    static func finish() {
        summarize(reason: VQL("al terminar la sesión", "after the session"))
    }

    /// Everything run so far: totals, by group, and the failures (log + Registros/conformidad-vulkan.txt).
    static func summarize(reason: String) {
        guard let work = workDir, let choice = VQLaunch.choice else { return }
        let lists = choice.prepared.tree.appendingPathComponent("assets/caselists", isDirectory: true)
        let batches = readIndex(lists.appendingPathComponent("index.txt"))
        var totals: [String: Int] = [:]
        var byGroup: [String: [String: Int]] = [:]
        var failures: [(String, Outcome)] = []
        // Why cases are not supported, most common first: what to implement next (the target is the
        // Adreno 740 feature profile a Quest game sees, not everything).
        var notSupportedWhy: [String: Int] = [:]
        var ran = 0
        for b in batches {
            let res = outcomes(batch: b, in: work)
            ran += res.count
            for (name, o) in res {
                totals[o.status, default: 0] += 1
                byGroup[b.group, default: [:]][o.status, default: 0] += 1
                if !okStatuses.contains(o.status) { failures.append((name, o)) }
                if o.status == "NotSupported" { notSupportedWhy[notSupportedReason(o.detail), default: 0] += 1 }
            }
        }
        failures.sort { $0.0 < $1.0 }
        let pass = totals["Pass", default: 0] + totals["QualityWarning", default: 0] + totals["CompatibilityWarning", default: 0]
        let notSup = totals["NotSupported", default: 0]
        let bad = ran - pass - notSup
        var out = "Vulkan CTS (\(profile)) — \(reason)\n"
        out += "Ejecutados \(ran) de \(totalCases): superados \(pass), no soportados \(notSup), FALLOS \(bad)"
        if ran - notSup > 0 {
            out += String(format: " (%.1f %% de los soportados)", 100.0 * Double(pass) / Double(ran - notSup))
        }
        out += "\nPor estado: " + totals.sorted { $0.key < $1.key }.map { "\($0.key) \($0.value)" }.joined(separator: ", ") + "\n"
        out += "\nPor grupo (superados / fallos / no soportados):\n"
        for b in uniqueGroups(batches) {
            guard let g = byGroup[b] else { continue }
            let p = g["Pass", default: 0] + g["QualityWarning", default: 0] + g["CompatibilityWarning", default: 0]
            let n = g["NotSupported", default: 0]
            let f = g.values.reduce(0, +) - p - n
            out += "  \(b): \(p) / \(f) / \(n)" + (f > 0 ? "  <--" : "") + "\n"
        }
        // Cases that hung the GPU or the test itself first: they are what stops a run.
        let stuck = failures.filter { ["DeviceLost", "Timeout", "Crash"].contains($0.1.status) }
        if !stuck.isEmpty {
            out += "\nColgados / dispositivo perdido / crash (\(stuck.count)):\n"
            for (name, o) in stuck {
                out += "  \(o.status) \(name)" + (o.detail.isEmpty ? "" : " — \(o.detail)") + "\n"
            }
        }
        out += "\nFallos (\(failures.count)):\n"
        for (name, o) in failures {
            out += "  \(o.status) \(name)" + (o.detail.isEmpty ? "" : " — \(o.detail)") + "\n"
        }
        if !notSupportedWhy.isEmpty {
            out += "\nNo soportados por motivo (\(notSupportedWhy.count) motivos):\n"
            for (why, n) in notSupportedWhy.sorted(by: { $0.value != $1.value ? $0.value > $1.value : $0.key < $1.key }).prefix(40) {
                out += "  \(n)  \(why)\n"
            }
        }
        try? out.write(to: summaryFile, atomically: true, encoding: .utf8)
        // The log gets the head and the first failures; the file has all of them.
        let head = out.components(separatedBy: "\n")
        let cut = head.prefix(60 + min(failures.count, 240)).joined(separator: "\n")
        VQLog.log("Conformidad:\n" + cut + (head.count > 300 ? "\n  … (lista completa en Registros/conformidad-vulkan.txt)" : ""))
        fputs("\n=== Conformidad Vulkan (\(reason)) ===\n" + cut + "\n", stderr)
    }

    static let okStatuses: Set<String> = ["Pass", "NotSupported", "QualityWarning", "CompatibilityWarning"]

    /// A NotSupported message reduced to its reason: numbers and quoted values vary per case
    /// ("Format 37 not supported", "...: 4096") and would split one reason into hundreds.
    static func notSupportedReason(_ detail: String) -> String {
        var d = detail.trimmingCharacters(in: .whitespacesAndNewlines)
        if d.isEmpty { return VQL("(sin mensaje)", "(no message)") }
        // A trailing source location ("... at vktFoo.cpp:123") only; "at least" stays.
        d = d.replacingOccurrences(of: #"\s+at\s+\S+\.(cpp|hpp|h|c|inl):[0-9]+.*$"#, with: "", options: .regularExpression)
        d = d.replacingOccurrences(of: "[0-9]+", with: "N", options: .regularExpression)
        return String(d.prefix(140))
    }

    // MARK: - Files

    static func readIndex(_ url: URL) -> [Batch] {
        guard let text = try? String(contentsOf: url, encoding: .utf8) else { return [] }
        return text.split(separator: "\n").compactMap { line in
            let f = line.split(separator: "\t").map(String.init)
            guard f.count >= 4, f[0] == profile, let n = Int(f[3]) else { return nil }
            return Batch(file: f[1], group: f[2], count: n)
        }
    }

    static func readCases(_ url: URL) -> [String] {
        guard let text = try? String(contentsOf: url, encoding: .utf8) else { return [] }
        return text.split(separator: "\n").map { $0.trimmingCharacters(in: .whitespaces) }.filter { !$0.isEmpty }
    }

    static func qpaFiles(batch: Batch, in work: URL) -> [URL] {
        ((try? FileManager.default.contentsOfDirectory(at: work, includingPropertiesForKeys: nil)) ?? [])
            .filter { $0.pathExtension == "qpa" && $0.lastPathComponent.hasPrefix(batch.stem + ".") }
            .sorted { $0.lastPathComponent.localizedStandardCompare($1.lastPathComponent) == .orderedAscending }
    }

    /// Every case of a batch with a result, over all its parts. A case that began and never
    /// ended (the app crashed or was closed in the middle of it) is a Crash.
    static func outcomes(batch: Batch, in work: URL) -> [String: Outcome] {
        var all: [String: Outcome] = [:]
        for f in qpaFiles(batch: batch, in: work) {
            for (k, v) in parse(f, assumeRunning: false) { all[k] = v }
        }
        return all
    }

    /// The .qpa format: "#beginTestCaseResult <name>", the case's XML with
    /// <Result StatusCode="..."> detail </Result>, then "#endTestCaseResult" or
    /// "#terminateTestCaseResult <reason>".
    static func parse(_ url: URL, assumeRunning: Bool) -> [String: Outcome] {
        guard let data = try? Data(contentsOf: url) else { return [:] }
        let text = String(decoding: data, as: UTF8.self)
        var out: [String: Outcome] = [:]
        var current: String?
        var status = "", detail = ""
        text.enumerateLines { line, _ in
            if line.hasPrefix("#beginTestCaseResult ") {
                current = String(line.dropFirst("#beginTestCaseResult ".count)).trimmingCharacters(in: .whitespaces)
                status = ""; detail = ""
            } else if line.hasPrefix("#endTestCaseResult") {
                if let c = current { out[c] = Outcome(status: status.isEmpty ? "InternalError" : status, detail: detail) }
                current = nil
            } else if line.hasPrefix("#terminateTestCaseResult") {
                let why = line.dropFirst("#terminateTestCaseResult".count).trimmingCharacters(in: .whitespaces)
                if let c = current { out[c] = Outcome(status: why.isEmpty ? "Crash" : why, detail: detail) }
                current = nil
            } else if current != nil, let r = line.range(of: "StatusCode=\"") {
                let rest = line[r.upperBound...]
                if let q = rest.firstIndex(of: "\"") {
                    status = String(rest[..<q])
                    if let gt = rest.firstIndex(of: ">") {
                        var d = String(rest[rest.index(after: gt)...])
                        if let lt = d.range(of: "</Result>") { d = String(d[..<lt.lowerBound]) }
                        detail = String(d.prefix(200))
                    }
                }
            }
        }
        // A case left open: still running (live view) or the run died inside it.
        if let c = current, !assumeRunning {
            out[c] = Outcome(status: "Crash", detail: VQL("la app se cerró o se colgó durante este caso",
                                                          "the app closed or hung during this case"))
        }
        return out
    }

    private static func uniqueGroups(_ batches: [Batch]) -> [String] {
        var seen = Set<String>(), out: [String] = []
        for b in batches where !seen.contains(b.group) { seen.insert(b.group); out.append(b.group) }
        return out
    }

    // MARK: - Download

    /// Downloads VulkanCTS.apk into QUEST/Juegos/Vulkan CTS (Khronos)/ (replacing an older copy).
    static func download(openXR: Bool = false, progress: @escaping @Sendable (Double) -> Void,
                         done: @escaping @Sendable (String?) -> Void) {
        guard let games = VQFolder.shared.games else { done(VQL("Elige antes la carpeta QUEST.", "Choose the QUEST folder first.")); return }
        guard !vqReleaseRepository.isEmpty,
              let downloadURL = URL(string: "https://github.com/\(vqReleaseRepository)/releases/download/"
                                    + (openXR ? "conformance-openxr/OpenXRCTS.apk" : "conformance/VulkanCTS.apk"))
        else { done(VQL("Esta build no sabe de qué repositorio descargar.", "This build does not know which repository to download from.")); return }
        let folderName = openXR ? Self.openXRFolderName : Self.folderName
        let fileName = openXR ? "OpenXRCTS.apk" : "VulkanCTS.apk"
        let folder = games.appendingPathComponent(folderName, isDirectory: true)
        VQLog.log("Conformidad: descargando \(downloadURL.absoluteString)")
        let delegate = Downloader(progress: progress) { tmp, error in
            if let error { VQLog.log("Conformidad: descarga fallida: \(error)"); done(error); return }
            guard let tmp else { done("?"); return }
            do {
                try FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true)
                let dest = folder.appendingPathComponent(fileName)
                try? FileManager.default.removeItem(at: dest)
                try FileManager.default.moveItem(at: tmp, to: dest)
                // A new APK means new lists and a new binary: the old preparation must go.
                if let cache = VQFolder.shared.cache(for: folderName) { try? FileManager.default.removeItem(at: cache) }
                let size = (try? dest.resourceValues(forKeys: [.fileSizeKey]).fileSize) ?? 0
                VQLog.log("Conformidad: descargado \(dest.path) (\(size >> 20) MB)")
                done(nil)
            } catch {
                VQLog.log("Conformidad: no se pudo guardar: \(error.localizedDescription)")
                done(error.localizedDescription)
            }
        }
        let session = URLSession(configuration: .default, delegate: delegate, delegateQueue: nil)
        session.downloadTask(with: downloadURL).resume()
    }

    private final class Downloader: NSObject, URLSessionDownloadDelegate, @unchecked Sendable {
        let progress: (Double) -> Void
        let finished: (URL?, String?) -> Void
        private var reported = false
        init(progress: @escaping (Double) -> Void, finished: @escaping (URL?, String?) -> Void) {
            self.progress = progress; self.finished = finished
        }
        func urlSession(_ session: URLSession, downloadTask: URLSessionDownloadTask, didFinishDownloadingTo location: URL) {
            if let http = downloadTask.response as? HTTPURLResponse, http.statusCode != 200 {
                reported = true
                finished(nil, "HTTP \(http.statusCode)")
                session.finishTasksAndInvalidate()
                return
            }
            // The temporary file is deleted when this returns: keep it.
            let keep = FileManager.default.temporaryDirectory.appendingPathComponent("VulkanCTS-\(UUID().uuidString).apk")
            do {
                try FileManager.default.moveItem(at: location, to: keep)
                reported = true
                finished(keep, nil)
            } catch {
                reported = true
                finished(nil, error.localizedDescription)
            }
            session.finishTasksAndInvalidate()
        }
        func urlSession(_ session: URLSession, downloadTask: URLSessionDownloadTask, didWriteData _: Int64,
                        totalBytesWritten: Int64, totalBytesExpectedToWrite: Int64) {
            if totalBytesExpectedToWrite > 0 { progress(Double(totalBytesWritten) / Double(totalBytesExpectedToWrite)) }
        }
        func urlSession(_ session: URLSession, task: URLSessionTask, didCompleteWithError error: Error?) {
            if let error, !reported { reported = true; finished(nil, error.localizedDescription) }
            session.finishTasksAndInvalidate()
        }
    }

    /// Forgets every result (the next launch starts from the first batch).
    static func reset(game: VQGame) {
        guard let saves = VQFolder.shared.saves(for: game.id) else { return }
        try? FileManager.default.removeItem(at: saves.appendingPathComponent("cts"))
        try? FileManager.default.removeItem(at: summaryFile)
        VQLog.log("Conformidad: resultados borrados")
    }
}
