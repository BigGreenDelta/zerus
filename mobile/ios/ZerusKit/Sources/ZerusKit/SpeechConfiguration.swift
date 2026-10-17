import CryptoKit
import Foundation

public struct SpeechConfigHost: Codable, Hashable, Sendable {
    public var key: MachineKey
    public var machineId: String
    public var name: String

    public init(key: MachineKey, machineId: String, name: String) {
        self.key = key
        self.machineId = machineId
        self.name = name
    }
}

public struct SpeechConfigProfile: Codable, Hashable, Sendable {
    public var id: String
    public var revision: Int64
    public var name: String
    public var adapter: String
    public var model: String
    public var language: String
    public var device: String
    public var baseUrl: String
    public var allowInsecureHttp: Bool
    public var credentialConfigured: Bool
    public var available: Bool
    public var reason: String

    public init(id: String, revision: Int64, name: String, adapter: String, model: String, language: String, device: String = "", baseUrl: String = "",
                allowInsecureHttp: Bool = false, credentialConfigured: Bool = false, available: Bool = false, reason: String = "") {
        self.id = id
        self.revision = revision
        self.name = name
        self.adapter = adapter
        self.model = model
        self.language = language
        self.device = device
        self.baseUrl = baseUrl
        self.allowInsecureHttp = allowInsecureHttp
        self.credentialConfigured = credentialConfigured
        self.available = available
        self.reason = reason
    }

    public func fields() -> JSONObject {
        var raw: JSONObject = ["id": .string(id), "name": .string(name), "adapter": .string(adapter), "model": .string(model), "language": .string(language)]
        if adapter == "gigaam" {
            raw["device"] = .string(device)
        } else {
            raw["base_url"] = .string(baseUrl)
            raw["allow_insecure_http"] = .bool(allowInsecureHttp)
        }
        return raw
    }
}

public struct SpeechConfigAdapter: Hashable, Sendable {
    public var id: String
    public var models: [String]
    public var devices: [String]
}

public struct SpeechConfigKey: Hashable, Sendable {
    public var id: String
    public var algorithm: String
    public var publicSpki: String

    public init(id: String, algorithm: String, publicSpki: String) {
        self.id = id
        self.algorithm = algorithm
        self.publicSpki = publicSpki
    }
}

public struct SpeechConfiguration: Hashable, Sendable {
    public var machineId: String
    public var revision: Int64
    public var serviceAvailable: Bool
    public var serviceReason: String
    public var adapters: [SpeechConfigAdapter]
    public var profiles: [SpeechConfigProfile]
    public var key: SpeechConfigKey?
}

/// A mutation whose exact identity and sealed envelope are durable before its only submission.
public struct SpeechConfigOperation: Codable, Hashable, Sendable {
    public var host: SpeechConfigHost
    public var id: String
    public var operation: String
    public var payload: JSONObject
}

public struct SpeechConfigResult: Hashable, Sendable {
    public var id: String
    public var state: String
    public var revision: Int64?
    public var profileId: String
    public var profileRevision: Int64?
    public var errorCode: String
    public var error: String
    public var terminal: Bool { ["completed", "failed"].contains(state) }
}

public enum SpeechConfigProtocol {
    public static let operations: Set<String> = ["speech_config", "speech_profile_upsert", "speech_profile_remove", "speech_profile_test", "speech_config_status"]
    public static let mutations: Set<String> = ["speech_profile_upsert", "speech_profile_remove", "speech_profile_test"]
    static let profileId = try! NSRegularExpression(pattern: "^[A-Za-z0-9_-]{1,64}$")
    static let languageCode = try! NSRegularExpression(pattern: "^[a-z]{2}$")

    static func matches(_ pattern: NSRegularExpression, _ value: String) -> Bool {
        pattern.firstMatch(in: value, range: NSRange(value.startIndex..., in: value)) != nil
    }

    static func invalid(_ message: String = "Check the provider fields.") -> ZerusError { ZerusError.invalid(message) }

    public static func bounded(_ value: String, _ max: Int, allowEmpty: Bool = false) throws -> String {
        guard allowEmpty || !value.trimmingCharacters(in: .whitespaces).isEmpty, value.utf8.count <= max,
              !value.unicodeScalars.contains(where: { $0.value < 32 || $0.value == 127 }) else { throw invalid() }
        return value
    }

    static func strings(_ raw: JSON?) throws -> [String] {
        guard let array = raw?.array else { return [] }
        guard array.count <= 32 else { throw invalid() }
        return try array.map { item in
            guard case .string(let value) = item else { throw invalid() }
            return try bounded(value, 128)
        }
    }

    public static func revision(_ raw: JSONObject, _ field: String) throws -> Int64 {
        guard case .int(let value)? = raw[field], (0...SpeechProtocol.maxRevision).contains(value) else { throw invalid("Invalid configuration revision.") }
        return value
    }

    public static func parse(_ raw: JSONObject, host: SpeechConfigHost) throws -> SpeechConfiguration {
        guard raw.int("schema") == 1, raw.string("machine_id") == host.machineId else {
            throw ZerusError.mismatch("The computer identity changed. Refresh its configuration.")
        }
        let configRevision = try Self.revision(raw, "revision")
        let adapters = try raw.objects("adapters").map { row in
            SpeechConfigAdapter(id: try bounded(row.string("id"), 64), models: try strings(row["models"]), devices: try strings(row["devices"]))
        }
        guard adapters.count <= 8, Set(adapters.map(\.id)).count == adapters.count else { throw invalid() }
        let profiles = try raw.objects("profiles").map { row -> SpeechConfigProfile in
            let id = row.string("id")
            guard matches(profileId, id), let credential = row.object("credential"), credential.has("configured") else { throw invalid() }
            let profileRevision = try Self.revision(row, "revision")
            guard profileRevision > 0 else { throw invalid() }
            return SpeechConfigProfile(id: id, revision: profileRevision, name: try bounded(row.string("name"), 128), adapter: try bounded(row.string("adapter"), 64),
                                       model: try bounded(row.string("model"), 128), language: try bounded(row.string("language"), 32, allowEmpty: true),
                                       device: try bounded(row.string("device"), 128, allowEmpty: true), baseUrl: try bounded(row.string("base_url"), 2048, allowEmpty: true),
                                       allowInsecureHttp: row.bool("allow_insecure_http"), credentialConfigured: credential.bool("configured"),
                                       available: row.bool("available"), reason: try bounded(row.string("reason"), 512, allowEmpty: true))
        }
        guard profiles.count <= 32, Set(profiles.map(\.id)).count == profiles.count, let service = raw.object("service") else { throw invalid() }
        let key = raw.object("credential_key").map { SpeechConfigKey(id: $0.string("key_id"), algorithm: $0.string("algorithm"), publicSpki: $0.string("public_key_spki")) }
        return SpeechConfiguration(machineId: host.machineId, revision: configRevision, serviceAvailable: service.bool("available"),
                                   serviceReason: try bounded(service.string("reason"), 512, allowEmpty: true), adapters: adapters, profiles: profiles, key: key)
    }

    public static func validate(_ profile: SpeechConfigProfile, _ config: SpeechConfiguration) throws {
        guard matches(profileId, profile.id) else { throw invalid("Use letters, numbers, underscores or hyphens for the provider ID.") }
        _ = try bounded(profile.name, 128)
        _ = try bounded(profile.model, 128)
        _ = try bounded(profile.language, 32, allowEmpty: true)
        guard profile.language.isEmpty || matches(languageCode, profile.language) else {
            throw invalid("Use a two-letter language code such as ru, or leave it empty.")
        }
        guard let adapter = config.adapters.first(where: { $0.id == profile.adapter }) else { throw invalid("This computer does not support that provider type.") }
        switch adapter.id {
        case "gigaam":
            guard adapter.models.contains(profile.model), adapter.devices.contains(profile.device) else { throw invalid("Choose a supported local model and device.") }
        case "openai_http":
            guard let url = try? bounded(profile.baseUrl, 2048), let components = URLComponents(string: url) else { throw invalid("Enter a valid server base URL.") }
            guard let scheme = components.scheme?.lowercased(), ["http", "https"].contains(scheme), components.host?.isEmpty == false,
                  components.user == nil, components.password == nil, components.percentEncodedQuery == nil, components.percentEncodedFragment == nil else {
                throw invalid("Use an HTTP(S) base URL without credentials, query or fragment.")
            }
            guard scheme == "https" || profile.allowInsecureHttp else { throw invalid("Allow local/private HTTP explicitly, or use HTTPS.") }
        default:
            throw invalid("Update Zerus to edit this provider type.")
        }
    }

    public static func upsert(_ host: SpeechConfigHost, _ config: SpeechConfiguration, _ profile: SpeechConfigProfile, credentialMode: String,
                              secret: String) throws -> SpeechConfigOperation {
        guard config.machineId == host.machineId else { throw invalid() }
        try validate(profile, config)
        guard ["keep", "replace", "remove"].contains(credentialMode) else { throw invalid() }
        guard profile.adapter == "openai_http" || credentialMode != "replace" else { throw invalid("Local GigaAM does not use an API key.") }
        let id = UUID().uuidString.lowercased()
        var payload: JSONObject = ["request_id": .string(id), "expected_machine_id": .string(host.machineId), "expected_revision": .int(config.revision),
                                   "profile": .object(profile.fields())]
        var credential: JSONObject = ["mode": .string(credentialMode)]
        if credentialMode == "replace" {
            guard let key = config.key else { throw invalid("This computer cannot receive API keys yet.") }
            credential["sealed_value"] = .object(try SpeechCredential.seal(key, payload, secret))
        }
        payload["credential"] = .object(credential)
        return SpeechConfigOperation(host: host, id: id, operation: "speech_profile_upsert", payload: payload)
    }

    public static func remove(_ host: SpeechConfigHost, _ config: SpeechConfiguration, _ profile: SpeechConfigProfile) -> SpeechConfigOperation {
        mutation(host, "speech_profile_remove", ["expected_revision": .int(config.revision), "profile_id": .string(profile.id)])
    }

    public static func test(_ host: SpeechConfigHost, _ profile: SpeechConfigProfile) -> SpeechConfigOperation {
        mutation(host, "speech_profile_test", ["expected_profile_revision": .int(profile.revision), "profile_id": .string(profile.id)])
    }

    static func mutation(_ host: SpeechConfigHost, _ operation: String, _ payload: JSONObject) -> SpeechConfigOperation {
        let id = UUID().uuidString.lowercased()
        var body = payload
        body["request_id"] = .string(id)
        body["expected_machine_id"] = .string(host.machineId)
        return SpeechConfigOperation(host: host, id: id, operation: operation, payload: body)
    }

    public static func result(_ raw: JSONObject, _ operation: SpeechConfigOperation) throws -> SpeechConfigResult {
        guard raw.int("schema") == 1, raw.string("machine_id") == operation.host.machineId, raw.string("request_id") == operation.id else {
            throw ZerusError.mismatch("The computer returned a different configuration operation.")
        }
        let state = raw.string("state")
        guard ["completed", "failed", "running", "uncertain"].contains(state) else { throw invalid("Unknown configuration status.") }
        return SpeechConfigResult(id: operation.id, state: state, revision: raw.has("revision") ? try revision(raw, "revision") : nil,
                                  profileId: raw.string("profile_id"), profileRevision: raw.has("profile_revision") ? try revision(raw, "profile_revision") : nil,
                                  errorCode: try bounded(raw.string("error_code"), 128, allowEmpty: true), error: try bounded(raw.string("error"), 512, allowEmpty: true))
    }

    /// Recovered operations must still name their exact request and machine.
    public static func restored(_ operations: [SpeechConfigOperation]) throws -> [SpeechConfigOperation] {
        guard operations.count <= 32, Set(operations.map(\.id)).count == operations.count else { throw invalid("Saved configuration operations are invalid.") }
        for operation in operations {
            guard AttachmentPolicy.validId(operation.id), AttachmentPolicy.validId(operation.host.machineId), mutations.contains(operation.operation),
                  operation.payload.string("request_id") == operation.id, operation.payload.string("expected_machine_id") == operation.host.machineId else {
                throw invalid("Saved configuration operations are invalid.")
            }
        }
        return operations
    }

    public static func readiness(_ host: SpeechConfigHost, _ configuration: SpeechConfiguration?, loading: Bool) -> String {
        if loading { return "Checking the speech service on \(host.name)…" }
        guard let configuration, configuration.machineId == host.machineId else {
            return "Speech service status on \(host.name) is not confirmed. Refresh configuration to check."
        }
        if configuration.serviceAvailable { return "Last check: the speech service on \(host.name) responds. Test the saved provider before using it." }
        return "Last check: the speech service on \(host.name) is unavailable. " + (configuration.serviceReason.nonEmpty ?? "Prepare or start it on that computer, then refresh.")
    }

    public static func documentation(_ adapter: String? = nil) -> (label: String, url: String) {
        let base = "https://github.com/ufna/zerus/blob/main/docs/speech-setup.md"
        switch adapter {
        case "gigaam": return ("Local GigaAM setup", base + "#local-gigaam")
        case "openai_http": return ("OpenAI-compatible server setup", base + "#openai-compatible-server")
        default: return ("Speech provider documentation", base)
        }
    }
}

/// Standard P256 ECDH / HKDF-SHA256 / AES-GCM envelope bound to the exact edit.
public enum SpeechCredential {
    public static let algorithm = "P256-HKDF-SHA256-A256GCM"

    public static func context(_ payload: JSONObject) throws -> Data {
        guard let profile = payload.object("profile"), case .int(let revision)? = payload["expected_revision"] else { throw ZerusError.invalid("Invalid credential context.") }
        let lines = ["zerus-speech-credential-v1", payload.string("expected_machine_id"), payload.string("request_id"), "profile_upsert", String(revision),
                     profile.string("id"), profile.string("name"), profile.string("adapter"), profile.string("model"), profile.string("language"),
                     profile.string("device"), profile.string("base_url"), profile.bool("allow_insecure_http") ? "true" : "false"]
        guard lines.allSatisfy({ !$0.unicodeScalars.contains(where: { $0.value < 32 || $0.value == 127 }) }) else { throw ZerusError.invalid("Invalid credential context.") }
        return Data(lines.joined(separator: "\n").utf8)
    }

    static func hex(_ data: some Sequence<UInt8>) -> String { data.map { String(format: "%02x", $0) }.joined() }

    /// Seals an API key to the computer's published key; the key never leaves this call in plaintext.
    public static func seal(_ key: SpeechConfigKey, _ payload: JSONObject, _ secret: String,
                            ephemeral: P256.KeyAgreement.PrivateKey = P256.KeyAgreement.PrivateKey(), nonce: Data? = nil) throws -> JSONObject {
        let plaintext = Data(secret.utf8)
        guard (1...4096).contains(plaintext.count), !secret.unicodeScalars.contains(where: { $0.value < 32 || $0.value == 127 }) else {
            throw ZerusError.invalid("Enter a valid API key (up to 4096 bytes).")
        }
        guard key.algorithm == algorithm else { throw ZerusError.invalid("Update Zerus to use this computer's credential encryption.") }
        guard let der = Data(base64Encoded: key.publicSpki), (1...128).contains(der.count), der.base64EncodedString() == key.publicSpki else {
            throw ZerusError.invalid("The computer returned an invalid credential key.")
        }
        let hash = Data(SHA256.hash(data: der))
        guard hex(hash) == key.id, let host = try? P256.KeyAgreement.PublicKey(derRepresentation: der), host.derRepresentation == der else {
            throw ZerusError.invalid("The computer returned an invalid credential key.")
        }
        let shared = try ephemeral.sharedSecretFromKeyAgreement(with: host).withUnsafeBytes { Data($0) }
        guard shared.count == 32 else { throw ZerusError.invalid("Credential encryption failed.") }
        let context = try context(payload)
        let prk = Data(HMAC<SHA256>.authenticationCode(for: shared, using: SymmetricKey(data: hash)))
        let aesKey = SymmetricKey(data: Data(HMAC<SHA256>.authenticationCode(for: context + Data([1]), using: SymmetricKey(data: prk))))
        let nonceBytes = nonce ?? Data(AES.GCM.Nonce())
        let sealed = try AES.GCM.seal(plaintext, using: aesKey, nonce: AES.GCM.Nonce(data: nonceBytes), authenticating: context)
        return ["key_id": .string(key.id), "algorithm": .string(algorithm), "ephemeral_public_key": .string(ephemeral.publicKey.derRepresentation.base64EncodedString()),
                "nonce": .string(nonceBytes.base64EncodedString()), "ciphertext": .string((sealed.ciphertext + sealed.tag).base64EncodedString())]
    }
}
