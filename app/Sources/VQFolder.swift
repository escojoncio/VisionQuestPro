// VisionQuestPro: the QUEST folder. Made once by the player (Files app, "On My Apple Vision Pro"),
// outside the app so the games, the conversions and the saves survive deleting or reinstalling it:
//
//   QUEST/Juegos/<carpeta>/     the game: its .apk and its data (OBB, bundles)
//   QUEST/Cache/<carpeta>/      what the app made from it on the headset (converted libraries)
//   QUEST/Partidas/<carpeta>/   saves and the game's own files
//   QUEST/capturas/<carpeta>/   frame captures (.vpcap) of the optional «Captura de frames» setting
//
// The app keeps a bookmark to it (settings and keychain, which outlives a reinstall), the same
// scheme as VPKEngine's VPS4 folder.

import Foundation
import Security

final class VQFolder: @unchecked Sendable {
    static let shared = VQFolder()

    static let folderName = "QUEST"
    static let gamesName = "Juegos"
    static let cacheName = "Cache"
    static let savesName = "Partidas"
    static let capturesName = "capturas"

    private static let defaultsKey = "vq.questFolderBookmark"
    private static let keychainService = "visionquestpro.quest"
    private static let keychainAccount = "folder-bookmark"

    private let lock = NSLock()
    private var opened: URL?
    private var generation = 0          // bumped (under `lock`) by choose() and forget(); url() drops what they overtook
    private let persistLock = NSLock()  // serialises the marker writes (settings + keychain); taken before `lock`, never inside it

    private init() {}

    /// The folder, opened for this process (nil: not chosen yet, or the bookmark no longer opens).
    /// The lock is held only to read / set `opened`: the keychain, the bookmark, the File Provider
    /// and the log run outside it, so a caller on the main thread never waits for them.
    var url: URL? {
        lock.lock()
        let current = opened
        let startGeneration = generation
        lock.unlock()
        if let current { return current }
        let stored: [(String, Data?)] = [("ajustes", UserDefaults.standard.data(forKey: Self.defaultsKey)),
                                         ("llavero", Self.keychainBookmark())]
        for (source, data) in stored {
            guard let data else { continue }
            var stale = false
            guard let resolved = try? URL(resolvingBookmarkData: data, options: [], relativeTo: nil, bookmarkDataIsStale: &stale) else {
                VQLog.log("QUEST: el marcador guardado en \(source) ya no abre")
                continue
            }
            let accessing = resolved.startAccessingSecurityScopedResource()
            guard accessing || FileManager.default.isReadableFile(atPath: resolved.path) else {
                VQLog.log("QUEST: sin acceso a \(resolved.path) (\(source))")
                continue
            }
            lock.lock()
            if let winner = opened {
                // Another thread (or choose()) opened it while this one resolved: keep that one.
                lock.unlock()
                if accessing { resolved.stopAccessingSecurityScopedResource() }
                return winner
            }
            if generation != startGeneration {
                // forget() ran while this one resolved: do not reopen what the player just forgot.
                lock.unlock()
                if accessing { resolved.stopAccessingSecurityScopedResource() }
                return nil
            }
            opened = resolved
            lock.unlock()
            // Only the winner refreshes the saved marker, and not if choose() / forget() got in first.
            if stale || source != "ajustes",
               let fresh = try? resolved.bookmarkData(options: [], includingResourceValuesForKeys: nil, relativeTo: nil) {
                persistLock.lock()
                lock.lock()
                let unchanged = generation == startGeneration
                lock.unlock()
                if unchanged { Self.save(fresh) }
                persistLock.unlock()
            }
            Self.makeSubfolders(resolved)
            VQLog.log("QUEST: \(resolved.path) (\(source))")
            return resolved
        }
        return nil
    }

    /// The folder the player picked. Nil, or why it cannot be used.
    func choose(_ picked: URL) -> String? {
        guard picked.lastPathComponent.caseInsensitiveCompare(Self.folderName) == .orderedSame else {
            return VQL("Esa carpeta se llama «\(picked.lastPathComponent)». Elige la carpeta llamada QUEST.",
                       "That folder is called “\(picked.lastPathComponent)”. Choose the folder called QUEST.")
        }
        let accessing = picked.startAccessingSecurityScopedResource()
        guard accessing || FileManager.default.isReadableFile(atPath: picked.path) else {
            return VQL("No se pudo abrir la carpeta QUEST.", "The QUEST folder could not be opened.")
        }
        guard let data = try? picked.bookmarkData(options: [], includingResourceValuesForKeys: nil, relativeTo: nil) else {
            picked.stopAccessingSecurityScopedResource()
            return VQL("No se pudo recordar la carpeta QUEST.", "The QUEST folder could not be remembered.")
        }
        persistLock.lock()
        Self.save(data)
        lock.lock()
        opened?.stopAccessingSecurityScopedResource()
        opened = picked
        generation += 1
        lock.unlock()
        persistLock.unlock()
        Self.makeSubfolders(picked)
        VQLog.log("QUEST: elegida \(picked.path)")
        return nil
    }

    func forget() {
        persistLock.lock()
        lock.lock()
        opened?.stopAccessingSecurityScopedResource()
        opened = nil
        generation += 1
        lock.unlock()
        UserDefaults.standard.removeObject(forKey: Self.defaultsKey)
        SecItemDelete(Self.keychainQuery() as CFDictionary)
        persistLock.unlock()
        VQLog.log("QUEST: olvidada")
    }

    var games: URL? { url?.appendingPathComponent(Self.gamesName, isDirectory: true) }

    func cache(for game: String) -> URL? {
        sub(Self.cacheName, game)
    }

    func saves(for game: String) -> URL? {
        sub(Self.savesName, game)
    }

    func captures(for game: String) -> URL? {
        sub(Self.capturesName, game)
    }

    private func sub(_ name: String, _ game: String) -> URL? {
        guard let folder = url?.appendingPathComponent(name, isDirectory: true)
            .appendingPathComponent(game, isDirectory: true) else { return nil }
        try? FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true)
        return folder
    }

    private static func makeSubfolders(_ root: URL) {
        for name in [gamesName, cacheName, savesName] {
            try? FileManager.default.createDirectory(at: root.appendingPathComponent(name, isDirectory: true),
                                                     withIntermediateDirectories: true)
        }
    }

    private static func save(_ bookmark: Data) {
        UserDefaults.standard.set(bookmark, forKey: defaultsKey)
        SecItemDelete(keychainQuery() as CFDictionary)
        var item = keychainQuery()
        item[kSecValueData as String] = bookmark
        item[kSecAttrAccessible as String] = kSecAttrAccessibleAfterFirstUnlock
        SecItemAdd(item as CFDictionary, nil)
    }

    private static func keychainBookmark() -> Data? {
        var query = keychainQuery()
        query[kSecReturnData as String] = true
        query[kSecMatchLimit as String] = kSecMatchLimitOne
        var result: CFTypeRef?
        guard SecItemCopyMatching(query as CFDictionary, &result) == errSecSuccess else { return nil }
        return result as? Data
    }

    private static func keychainQuery() -> [String: Any] {
        [kSecClass as String: kSecClassGenericPassword,
         kSecAttrService as String: keychainService,
         kSecAttrAccount as String: keychainAccount]
    }
}
