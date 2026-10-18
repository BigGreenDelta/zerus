import XCTest
@testable import ZerusKit

/// Ported from the refusal tests of Android VoiceMessagesTest.
final class VoiceRejectionTests: XCTestCase {
    private let target = Target(computerId: "owner", session: "example", run: "run", conversation: "conversation", connectionId: "workspace")
    private let machine = "b4075bf0-9c22-4a67-b768-55af2d2936a4"
    private let audio = Attachment(id: UUID().uuidString.lowercased(), name: "voice.wav", mime: "audio/wav", bytes: 6444, sha256: String(repeating: "a", count: 64))

    private func entry(_ id: String = UUID().uuidString.lowercased()) -> VoiceOutgoing {
        VoiceOutgoing(id: id, target: target, machineId: machine, profileId: "russian", audio: audio, durationMs: 200, createdAt: 50)
    }

    private func message(_ entry: VoiceOutgoing, _ state: String, revision: Int64) -> VoiceMessage {
        VoiceMessage(id: entry.id, runId: target.run, conversationId: target.conversation, createdAt: 50, revision: revision, state: state,
                     audio: DisplayedAttachment(name: "voice.wav", mime: "audio/wav", bytes: 6444, messageId: entry.id, index: 0), durationMs: 200)
    }

    private func refusal(_ entry: VoiceOutgoing, operation: String? = nil, generation: Int64 = 1, change: ((inout JSONObject) -> Void)? = nil) throws -> JSONObject {
        var proof: JSONObject = ["schema": 1, "state": "rejected_before_acceptance", "request_id": .string(operation ?? entry.operation),
                                 "message_id": .string(entry.id), "run_id": .string(entry.target.run), "conversation_id": .string(entry.target.conversation),
                                 "intent": .object(try VoiceRejection.intent(entry)), "payload_sha256": .string(String(repeating: "c", count: 64)),
                                 "generation": .int(generation), "retry_token": .string(UUID().uuidString.lowercased()), "code": "route_unavailable",
                                 "detail": "The recording was not accepted."]
        change?(&proof)
        return ["schema": 1, "rejection": .object(proof)]
    }

    private func roundTrip(_ state: VoiceState) throws -> VoiceState { try JSONDecoder().decode(VoiceState.self, from: JSONEncoder().encode(state)).recovered() }

    func testDefinitiveRefusalRestoresOriginalMediaIdentityAndPinsAcrossRestart() throws {
        var original = entry()
        original.profileRevision = 7
        let refused = try VoiceRejection.accept(try refusal(original), original, operationId: original.id)
        XCTAssertTrue(refused.canRestoreUnsent)
        XCTAssertEqual(original.audio, refused.audio)
        XCTAssertEqual(original.id, refused.id)
        XCTAssertEqual(original.machineId, refused.machineId)
        XCTAssertEqual(original.profileRevision, refused.profileRevision)
        XCTAssertNil(refused.message)
        XCTAssertEqual(refused, try roundTrip(VoiceState(outgoing: [refused])).outgoing.first)
        let draft = VoiceDraft(target: original.target, machineId: original.machineId, profileId: original.profileId, audio: audio, durationMs: 200,
                               createdAt: 50, profileRevision: 7, resubmissionId: original.id, rejection: refused.rejection)
        XCTAssertEqual(draft, try roundTrip(VoiceState(draft: draft)).draft)
    }

    func testOlderSavedStateWithoutRefusalFieldsStillDecodes() throws {
        let legacy = try JSONEncoder().encode(VoiceState(outgoing: [entry()]))
        var object = try XCTUnwrap(JSON.parse(legacy).object)
        XCTAssertNil(object.objects("outgoing").first?["operation_id"])
        object["outgoing"] = .array(object.objects("outgoing").map { row in
            var copy = row
            copy["rejection"] = nil
            return .object(copy)
        })
        let decoded = try JSONDecoder().decode(VoiceState.self, from: try JSON.object(object).data())
        XCTAssertEqual(decoded.outgoing.first?.operation, decoded.outgoing.first?.id)
    }

    func testConsumedRefusalCannotSettleALostRetryAndAcceptedReceiptWins() throws {
        let original = entry()
        let first = try VoiceRejection.accept(try refusal(original), original, operationId: original.id)
        let operation = UUID().uuidString.lowercased()
        var retry = first
        retry.uploadState = "uploading"
        retry.operationId = operation
        retry.attemptedRejectionGeneration = 1
        let cold = try XCTUnwrap(try roundTrip(VoiceState(outgoing: [retry])).outgoing.first)
        XCTAssertEqual("uncertain", cold.state)
        XCTAssertEqual(operation, cold.operation)
        XCTAssertEqual(1, cold.attemptedRejectionGeneration)
        let status = UUID().uuidString.lowercased()
        // A status copy at the consumed generation is not fresh authority: the lost retry might still execute.
        XCTAssertThrowsError(try VoiceRejection.accept(try refusal(cold, operation: status, generation: 1), cold, operationId: status))
        XCTAssertFalse(cold.canRestoreUnsent)
        let settled = try VoiceRejection.accept(try refusal(cold, operation: status, generation: 2), cold, operationId: status)
        XCTAssertTrue(settled.canRestoreUnsent)
        XCTAssertEqual(original.id, settled.id)
        var accepted = cold
        accepted.message = message(cold, "submitted", revision: 9)
        XCTAssertEqual(accepted, try VoiceRejection.accept(try refusal(cold, operation: status, generation: 2), accepted, operationId: status))
        XCTAssertThrowsError(try VoiceRejection.accept(["schema": 1], cold, operationId: status))
    }

    func testRefusalRequiresExactOperationConversationMediaAndProvider() throws {
        let video = VideoDraft(file: Attachment(id: UUID().uuidString.lowercased(), name: "video.mp4", mime: "video/mp4", bytes: 6444,
                                                sha256: String(repeating: "a", count: 64)), durationMs: 200, width: 480, height: 480)
        var original = entry()
        original.video = video
        original.profileRevision = 4
        let changes: [(String, JSON)] = [("request_id", .string(UUID().uuidString.lowercased())), ("message_id", .string(UUID().uuidString.lowercased())),
                                         ("run_id", "other"), ("conversation_id", "other"), ("generation", 0), ("retry_token", "bad"), ("path", "/synthetic")]
        for (key, value) in changes {
            XCTAssertThrowsError(try VoiceRejection.accept(try refusal(original) { $0[key] = value }, original, operationId: original.id), key)
        }
        var locals: [VoiceOutgoing] = []
        let edits: [(inout VoiceOutgoing) -> Void] = [{ $0.profileRevision = 5 }, { $0.profileId = "other" },
                                                      { $0.machineId = UUID().uuidString.lowercased() },
                                                      { $0.audio?.sha256 = String(repeating: "b", count: 64) }, { $0.video?.width = 640 }]
        for change in edits {
            var local = original
            change(&local)
            locals.append(local)
        }
        let proof = try refusal(original)
        for local in locals { XCTAssertThrowsError(try VoiceRejection.accept(proof, local, operationId: original.id)) }
        // Numeric fields must stay integers; a string that reads the same is a different intent.
        let coerced = try refusal(original) { proof in
            var intent = proof.object("intent")!
            var media = intent.object("audio")!
            media["bytes"] = .string(String(original.audio!.bytes))
            intent["audio"] = .object(media)
            proof["intent"] = .object(intent)
        }
        XCTAssertThrowsError(try VoiceRejection.accept(coerced, original, operationId: original.id))
        XCTAssertTrue(try VoiceRejection.accept(try refusal(original), original, operationId: original.id).canRestoreUnsent)
        // The key order of a proof never matters.
        let reordered = try refusal(original) { proof in
            let intent = proof.object("intent")!
            proof["intent"] = .object(JSONObject(intent.keys.reversed().map { ($0, intent[$0]!) }))
        }
        XCTAssertTrue(try VoiceRejection.accept(reordered, original, operationId: original.id).canRestoreUnsent)
    }

    func testRefusalMetadataRetainsLegacyShortWavWhileCaptureMinimumIsUnchanged() throws {
        var original = entry()
        original.audio?.bytes = 46
        original.durationMs = 1
        XCTAssertEqual(46, try VoiceRejection.intent(original).object("audio")?.int("bytes"))
        XCTAssertTrue(try VoiceRejection.accept(try refusal(original), original, operationId: original.id).canRestoreUnsent)
        XCTAssertThrowsError(try VoiceProtocol.wav(Data(count: 2)))
        for bytes: Int64 in [0, 44, 45, 3_840_046] {
            var invalid = original
            invalid.audio?.bytes = bytes
            XCTAssertThrowsError(try VoiceRejection.intent(invalid), String(bytes))
        }
    }
}
