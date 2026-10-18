import CryptoKit
import Foundation

/// A durable native admission barrier ("rejected_before_acceptance"), never inferred from transport errors or missing status.
/// Ported from Android VoiceRejection; the computer and relay define the proof (docs/voice-messages.md).
public enum VoiceRejection {
    static let fields: Set<String> = ["schema", "state", "request_id", "message_id", "run_id", "conversation_id", "intent", "payload_sha256",
                                      "generation", "retry_token", "code", "detail"]
    static let codes: Set<String> = ["route_unavailable", "storage_full", "queue_full"]

    static func invalid(_ message: String = "The computer returned an invalid refusal. Check status; it has not been resent.") -> ZerusError {
        ZerusError.invalid(message)
    }

    static func uuid(_ value: String) -> Bool {
        guard let parsed = UUID(uuidString: value) else { return false }
        return parsed.uuidString.lowercased() == value && value != "00000000-0000-0000-0000-000000000000"
    }

    static func hex(_ value: String) -> Bool { value.utf8.count == 64 && value.utf8.allSatisfy { (48...57).contains($0) || (97...102).contains($0) } }

    /// Key-order-insensitive equality; integers never equal their string or floating forms.
    static func same(_ a: JSON, _ b: JSON) -> Bool {
        switch (a, b) {
        case (.object(let left), .object(let right)):
            return Set(left.keys) == Set(right.keys) && left.keys.allSatisfy { key in same(left[key]!, right[key]!) }
        case (.array(let left), .array(let right)):
            return left.count == right.count && zip(left, right).allSatisfy { same($0, $1) }
        default:
            return a == b
        }
    }

    /// The exact recording and provider this phone submitted, as the computer's refusal records it.
    public static func intent(_ entry: VoiceOutgoing) throws -> JSONObject {
        guard let audio = entry.audio, (46...3_840_044).contains(audio.bytes), audio.bytes % 2 == 0, hex(audio.sha256) else {
            throw invalid("This recording cannot be resubmitted.")
        }
        var speech: JSONObject = ["machine_id": .string(entry.machineId), "profile_id": .string(entry.profileId)]
        if let revision = entry.profileRevision { speech["profile_revision"] = .int(revision) }
        var result: JSONObject = ["speech": .object(speech),
                                  "audio": ["format": "pcm_s16le", "sample_rate": 16_000, "channels": 1, "bytes": .int(audio.bytes),
                                            "sha256": .string(audio.sha256)]]
        if let video = entry.video {
            var media: JSONObject = ["format": "mp4", "duration_ms": .int(Int64(video.durationMs)), "width": .int(Int64(video.width)),
                                     "height": .int(Int64(video.height)), "bytes": .int(video.file.bytes), "sha256": .string(video.file.sha256)]
            if !video.posterBase64.isEmpty {
                guard let poster = Data(base64Encoded: video.posterBase64) else { throw invalid("This recording cannot be resubmitted.") }
                media["poster_sha256"] = .string(SHA256.hash(data: poster).map { String(format: "%02x", $0) }.joined())
            }
            result["video"] = .object(media)
        }
        return result
    }

    static func proof(_ text: String?) -> JSONObject? {
        guard let text, !text.isEmpty else { return nil }
        return (try? JSON.parse(Data(text.utf8)))?.object
    }

    /// The generation of a saved proof; 0 without one.
    public static func generation(_ text: String?) -> Int64 {
        guard case .int(let value)? = proof(text)?["generation"] else { return 0 }
        return value
    }

    /// Applies a definitive refusal of `operationId` to `entry`, or throws and changes nothing.
    public static func accept(_ raw: JSONObject, _ entry: VoiceOutgoing, operationId: String) throws -> VoiceOutgoing {
        // An accepted native receipt always takes precedence over an older refusal callback.
        if entry.message != nil { return entry }
        guard raw["schema"] == .int(1), let proof = raw.object("rejection"), Set(proof.keys) == fields,
              JSON.object(proof).compact.utf8.count <= 4096 else { throw invalid() }
        guard proof["schema"] == .int(1), proof.string("state") == "rejected_before_acceptance" else { throw invalid() }
        guard uuid(operationId), proof["request_id"] == .string(operationId), proof["message_id"] == .string(entry.id) else { throw invalid() }
        guard proof["run_id"] == .string(entry.target.run), proof["conversation_id"] == .string(entry.target.conversation) else { throw invalid() }
        guard case .string(let token)? = proof["retry_token"], uuid(token), case .string(let digest)? = proof["payload_sha256"], hex(digest),
              case .string(let code)? = proof["code"], codes.contains(code) else { throw invalid() }
        guard case .string(let detail)? = proof["detail"], (1...2000).contains(detail.utf8.count),
              !detail.unicodeScalars.contains(where: { CharacterSet.controlCharacters.contains($0) }) else { throw invalid() }
        guard case .int(let generation)? = proof["generation"] else { throw invalid() }
        guard generation > (entry.attemptedRejectionGeneration ?? 0), generation >= Self.generation(entry.rejection) else {
            throw invalid("This refusal does not settle the attempted retry. Check status; it has not been resent.")
        }
        if let old = Self.proof(entry.rejection) {
            guard old["payload_sha256"] == proof["payload_sha256"] else { throw invalid() }
            if old["generation"] == proof["generation"] && old["retry_token"] != proof["retry_token"] { throw invalid() }
        }
        guard let intent = proof.object("intent"), same(.object(intent), .object(try Self.intent(entry))) else {
            throw invalid("The refusal describes a different recording or provider.")
        }
        var refused = entry
        refused.uploadState = "failed"
        refused.rejection = JSON.object(proof).compact
        refused.error = "The computer did not accept this recording. Return to recording to send the same recording again."
        return refused
    }
}
