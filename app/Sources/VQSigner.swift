// VisionQuestPro: signing the converted libraries on the headset with the player's own developer
// certificate (the one the app itself was signed with), so dyld maps them without JIT. The
// signature is VPEngine's (runtime/vp_codesign.c, vp_pkcs12.c, taken from VPEngine at build time);
// this file follows VPEngine's VPGamePack.swift: the certificate is a .p12 imported in the app and
// kept in the keychain, and each library is signed in up to four variants (the app's requirement
// set or none, 4 KiB or 16 KiB pages) until the system accepts one.

import Foundation
import Security

enum VQSigner {
    private static let service = "visionquestpro.signing-certificate"

    struct Identity {
        let certificate: SecCertificate
        let key: SecKey
        let chain: [SecCertificate]
    }

    enum Problem: Error, LocalizedError {
        case missing, wrongPassword, noIdentity
        case unreadable(String)
        case keychain(OSStatus)
        case wrongTeam(app: String, certificate: String)
        case signing(String)

        var errorDescription: String? {
            switch self {
            case .missing: return VQL("No hay certificado: importa tu .p12 en Ajustes.", "No certificate: import your .p12 in Settings.")
            case .wrongPassword: return VQL("La contraseña del certificado no es correcta.", "The certificate's password is wrong.")
            case .noIdentity: return VQL("El archivo no contiene un certificado con su clave.", "The file holds no certificate with its key.")
            case .unreadable(let d): return VQL("El certificado no se pudo leer: \(d)", "The certificate could not be read: \(d)")
            case .keychain(let s): return VQL("No se pudo guardar en el llavero (\(s)).", "Could not store it in the keychain (\(s)).")
            case .wrongTeam(let app, let cert):
                return VQL("El certificado (equipo \(cert)) no es el que firmó la app (equipo \(app)).",
                           "The certificate (team \(cert)) is not the one the app was signed with (team \(app)).")
            case .signing(let m): return VQL("No se pudo firmar: \(m)", "Could not sign: \(m)")
            }
        }
    }

    // MARK: - Certificate

    /// Checks and keeps a .p12. Returns a one-line description.
    @discardableResult
    static func store(p12: Data, password: String) throws -> String {
        let identity = try identity(p12: p12, password: password, quiet: false)
        try keep("p12", p12)
        try keep("password", Data(password.utf8))
        cachedFingerprint = nil
        let text = describe(identity.certificate)
        VQLog.log("Firma: certificado importado: \(text)")
        return text
    }

    static func remove() {
        for account in ["p12", "password"] {
            SecItemDelete([kSecClass as String: kSecClassGenericPassword, kSecAttrService as String: service,
                           kSecAttrAccount as String: account] as CFDictionary)
        }
        cachedFingerprint = nil
        VQLog.log("Firma: certificado borrado")
    }

    static var current: String? {
        (try? storedIdentity()).map { describe($0.certificate) }
    }

    static func storedIdentity() throws -> Identity {
        guard let p12 = read("p12") else { throw Problem.missing }
        let password = read("password").flatMap { String(data: $0, encoding: .utf8) } ?? ""
        return try identity(p12: p12, password: password, quiet: true)
    }

    /// The stored certificate's fingerprint, cached (reading the .p12 is slow; the launcher asks
    /// on every card).
    nonisolated(unsafe) private static var cachedFingerprint: String??
    static var storedFingerprint: String? {
        if let cachedFingerprint { return cachedFingerprint }
        let f = (try? storedIdentity()).map { fingerprint($0) }
        cachedFingerprint = .some(f)
        return f
    }

    /// SHA-256 of the leaf certificate, hex: part of the signed copies' tag.
    static func fingerprint(_ identity: Identity) -> String {
        let der = SecCertificateCopyData(identity.certificate) as Data
        var digest = [UInt8](repeating: 0, count: 32)
        der.withUnsafeBytes { vp_sha256($0.baseAddress, der.count, &digest) }
        return digest.prefix(8).map { String(format: "%02x", $0) }.joined()
    }

    private static func identity(p12: Data, password: String, quiet: Bool) throws -> Identity {
        var items: CFArray?
        let status = SecPKCS12Import(p12 as CFData, [kSecImportExportPassphrase as String: password] as CFDictionary, &items)
        if status == errSecSuccess, let first = (items as? [[String: Any]])?.first,
           let value = first[kSecImportItemIdentity as String] {
            let identity = value as! SecIdentity // swiftlint:disable:this force_cast
            var certificate: SecCertificate?
            var key: SecKey?
            if SecIdentityCopyCertificate(identity, &certificate) == errSecSuccess, let certificate,
               SecIdentityCopyPrivateKey(identity, &key) == errSecSuccess, let key {
                let chain = (first[kSecImportItemCertChain as String] as? [SecCertificate]) ?? []
                return Identity(certificate: certificate, key: key, chain: Array(chain.dropFirst()))
            }
        }
        if !quiet { VQLog.log("Firma: el lector PKCS#12 del sistema dio \(status); probando el de VPEngine") }
        var parsed = VpPkcs12()
        var error = [CChar](repeating: 0, count: 256)
        let rc = p12.withUnsafeBytes { raw in
            vp_pkcs12_read(raw.bindMemory(to: UInt8.self).baseAddress, p12.count, password, &parsed, &error, error.count)
        }
        defer { vp_pkcs12_free(&parsed) }
        let detail = String(cString: error)
        if rc != 0 && !quiet { VQLog.log("Firma: lector de VPEngine: \(rc), \(detail)") }
        if status == errSecAuthFailed && rc != 0 && rc != -4 { throw Problem.wrongPassword }
        switch rc {
        case 0: break
        case -2: throw Problem.wrongPassword
        case -4: throw Problem.noIdentity
        default: throw Problem.unreadable("\(status) \(detail)")
        }
        guard let keyBytes = parsed.key else { throw Problem.noIdentity }
        let ec = parsed.key_type == 2
        let attributes: [String: Any] = [
            kSecAttrKeyType as String: ec ? kSecAttrKeyTypeECSECPrimeRandom : kSecAttrKeyTypeRSA,
            kSecAttrKeyClass as String: kSecAttrKeyClassPrivate,
        ]
        var cfError: Unmanaged<CFError>?
        guard let key = SecKeyCreateWithData(Data(bytes: keyBytes, count: parsed.key_len) as CFData, attributes as CFDictionary, &cfError) else {
            throw Problem.unreadable(cfError?.takeRetainedValue().localizedDescription ?? "SecKeyCreateWithData")
        }
        var certificates: [SecCertificate] = []
        for i in 0..<Int(parsed.cert_count) {
            var length = 0
            guard let bytes = vp_pkcs12_cert(&parsed, Int32(i), &length),
                  let c = SecCertificateCreateWithData(nil, Data(bytes: bytes, count: length) as CFData) else { continue }
            certificates.append(c)
        }
        let publicKey = SecKeyCopyPublicKey(key).flatMap { SecKeyCopyExternalRepresentation($0, nil) as Data? }
        guard let leafIndex = certificates.firstIndex(where: { c in
            guard let publicKey, let theirs = SecCertificateCopyKey(c) else { return false }
            return (SecKeyCopyExternalRepresentation(theirs, nil) as Data?) == publicKey
        }) else { throw Problem.noIdentity }
        let leaf = certificates.remove(at: leafIndex)
        return Identity(certificate: leaf, key: key, chain: certificates)
    }

    private static func describe(_ certificate: SecCertificate) -> String {
        let name = SecCertificateCopySubjectSummary(certificate) as String? ?? "?"
        let der = SecCertificateCopyData(certificate) as Data
        var out = [CChar](repeating: 0, count: 64)
        let ok = der.withUnsafeBytes { vp_codesign_cert_team($0.bindMemory(to: UInt8.self).baseAddress, der.count, &out, out.count) } == 0
        return "\(name) — \(VQL("equipo", "team")) \(ok ? String(cString: out) : "?")"
    }

    private static func keep(_ account: String, _ data: Data) throws {
        let query: [String: Any] = [kSecClass as String: kSecClassGenericPassword,
                                    kSecAttrService as String: service, kSecAttrAccount as String: account]
        SecItemDelete(query as CFDictionary)
        var add = query
        add[kSecValueData as String] = data
        add[kSecAttrAccessible as String] = kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly
        let status = SecItemAdd(add as CFDictionary, nil)
        guard status == errSecSuccess else { throw Problem.keychain(status) }
    }

    private static func read(_ account: String) -> Data? {
        var item: CFTypeRef?
        let query: [String: Any] = [kSecClass as String: kSecClassGenericPassword, kSecAttrService as String: service,
                                    kSecAttrAccount as String: account, kSecReturnData as String: true,
                                    kSecMatchLimit as String: kSecMatchLimitOne]
        guard SecItemCopyMatching(query as CFDictionary, &item) == errSecSuccess else { return nil }
        return item as? Data
    }

    // MARK: - Variants

    struct Variant {
        let name: String
        let requirements: Data?
        let pageShift: Int32
    }

    private static let variantKey = "vq.signatureVariant"

    /// The variants in the order to try them (the one accepted last time first).
    static func variants() -> [Variant] {
        var requirements: Data?
        if let executable = Bundle.main.executablePath {
            var buffer = [UInt8](repeating: 0, count: 4096)
            let n = vp_codesign_file_requirements(executable, &buffer, buffer.count)
            if n >= 12 { requirements = Data(buffer.prefix(Int(n))) }
        }
        var all: [Variant] = []
        if let requirements {
            all.append(Variant(name: "req4k", requirements: requirements, pageShift: 12))
            all.append(Variant(name: "req16k", requirements: requirements, pageShift: 14))
        }
        all.append(Variant(name: "plain4k", requirements: nil, pageShift: 12))
        all.append(Variant(name: "plain16k", requirements: nil, pageShift: 14))
        if let remembered = UserDefaults.standard.string(forKey: variantKey),
           let i = all.firstIndex(where: { $0.name == remembered }) {
            all.insert(all.remove(at: i), at: 0)
        }
        return all
    }

    static func remember(_ variant: Variant) {
        UserDefaults.standard.set(variant.name, forKey: variantKey)
    }

    // MARK: - Signing

    /// Signs the Mach-O at `path` in place (it must carry an LC_CODE_SIGNATURE: klepton-ld's
    /// output does).
    static func sign(path: String, variant: Variant, identity: Identity) throws {
        let leaf = SecCertificateCopyData(identity.certificate) as Data
        if let executable = Bundle.main.executablePath {
            var appTeam = [CChar](repeating: 0, count: 64)
            if vp_codesign_file_team(executable, &appTeam, appTeam.count) == 0 {
                var mine = [CChar](repeating: 0, count: 64)
                let found = leaf.withUnsafeBytes { vp_codesign_cert_team($0.bindMemory(to: UInt8.self).baseAddress, leaf.count, &mine, mine.count) }
                if found == 0 && String(cString: appTeam) != String(cString: mine) {
                    throw Problem.wrongTeam(app: String(cString: appTeam), certificate: String(cString: mine))
                }
            }
        }
        let chain = chainFor(identity).map { [UInt8](SecCertificateCopyData($0) as Data) }
        let identifier = Bundle.main.bundleIdentifier ?? "com.kdt.livecontainer"
        let requirements = [UInt8](variant.requirements ?? Data())
        var error = [CChar](repeating: 0, count: 256)
        let session: OpaquePointer? = leaf.withUnsafeBytes { leafBytes in
            withPointers(chain) { pointers, lengths in
                requirements.withUnsafeBufferPointer { req in
                    vp_codesign_begin_ex(path, identifier, requirements.isEmpty ? nil : req.baseAddress, requirements.count,
                                         variant.pageShift, leafBytes.bindMemory(to: UInt8.self).baseAddress, leaf.count,
                                         pointers, lengths, Int32(chain.count), &error, error.count)
                }
            }
        }
        guard let session else { throw Problem.signing(String(cString: error)) }
        var length = 0
        guard let toSign = vp_codesign_to_sign(session, &length) else {
            vp_codesign_abort(session)
            throw Problem.signing("no attributes")
        }
        let message = Data(bytes: toSign, count: length)
        let isEC = (SecKeyCopyAttributes(identity.key) as? [String: Any])?[kSecAttrKeyType as String] as? String
            == (kSecAttrKeyTypeECSECPrimeRandom as String)
        var cfError: Unmanaged<CFError>?
        guard let signature = SecKeyCreateSignature(identity.key, isEC ? .ecdsaSignatureMessageX962SHA256 : .rsaSignatureMessagePKCS1v15SHA256,
                                                    message as CFData, &cfError) as Data? else {
            vp_codesign_abort(session)
            throw Problem.signing(cfError?.takeRetainedValue().localizedDescription ?? "SecKeyCreateSignature")
        }
        let status = signature.withUnsafeBytes {
            vp_codesign_finish(session, $0.bindMemory(to: UInt8.self).baseAddress, signature.count, isEC ? 1 : 0, &error, error.count)
        }
        guard status == 0 else { throw Problem.signing(String(cString: error)) }
    }

    /// Leaf to Apple's root: what the .p12 carried, else Apple's certificates bundled with the app
    /// (VPEngineCertificates/*.cer), matched by issuer.
    private static func chainFor(_ identity: Identity) -> [SecCertificate] {
        var candidates = identity.chain
        for url in Bundle.main.urls(forResourcesWithExtension: "cer", subdirectory: "VPEngineCertificates") ?? [] {
            if let data = try? Data(contentsOf: url), let c = SecCertificateCreateWithData(nil, data as CFData) {
                candidates.append(c)
            }
        }
        var chain: [SecCertificate] = []
        var current = identity.certificate
        for _ in 0..<4 {
            guard let issuer = SecCertificateCopyNormalizedIssuerSequence(current) as Data?,
                  let next = candidates.first(where: { (SecCertificateCopyNormalizedSubjectSequence($0) as Data?) == issuer }),
                  !chain.contains(where: { CFEqual($0, next) }), !CFEqual(next, current) else { break }
            chain.append(next)
            if (SecCertificateCopyNormalizedSubjectSequence(next) as Data?) == (SecCertificateCopyNormalizedIssuerSequence(next) as Data?) { break }
            current = next
        }
        return chain
    }
}

private func withPointers<R>(_ buffers: [[UInt8]], _ body: (UnsafePointer<UnsafePointer<UInt8>?>?, UnsafePointer<Int>?) -> R) -> R {
    var pointers: [UnsafePointer<UInt8>?] = []
    var lengths: [Int] = []
    var copies: [UnsafeMutablePointer<UInt8>] = []
    for b in buffers {
        let p = UnsafeMutablePointer<UInt8>.allocate(capacity: max(b.count, 1))
        p.initialize(from: b, count: b.count)
        copies.append(p)
        pointers.append(UnsafePointer(p))
        lengths.append(b.count)
    }
    defer { copies.forEach { $0.deallocate() } }
    return pointers.withUnsafeBufferPointer { pp in
        lengths.withUnsafeBufferPointer { lp in body(pp.baseAddress, lp.baseAddress) }
    }
}
