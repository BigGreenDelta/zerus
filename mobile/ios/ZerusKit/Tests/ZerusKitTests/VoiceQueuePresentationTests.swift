import XCTest
@testable import ZerusKit

/// Native admission of a voice job does not consume OpenCode's transient inbox.
final class VoiceQueuePresentationTests: XCTestCase {
    func testSubmittedStatusDetailsExplainQueueAndNativeAdmissionOnly() {
        let status = RecordingDelivery.of("submitted")
        XCTAssertEqual("Accepted by native app", status.description)
        XCTAssertTrue(status.explanation.contains("native app accepted the recognized text"))
        XCTAssertTrue(status.explanation.contains("Queue status"))
        XCTAssertTrue(status.explanation.contains("does not confirm model delivery, reading or completion"))
        XCTAssertEqual(RecordingDelivery.failed.description, RecordingDelivery.failed.explanation)
    }

    private let id = "aaaaaaaa-aaaa-4aaa-aaaa-aaaaaaaaaaaa"
    private let target = Target(computerId: "computer", session: "opencode/example/local", run: "run", conversation: "conversation", connectionId: "workspace")
    private var message: VoiceMessage {
        VoiceMessage(id: id, runId: target.run, conversationId: target.conversation, createdAt: 50, revision: 2, state: "submitted",
                     audio: DisplayedAttachment(name: "voice.wav", mime: "audio/wav", bytes: 6444, messageId: id, index: 0), durationMs: 200, transcript: "Repeated text")
    }
    private func voice(_ binding: JSONObject = ["provider_source": "opencode_native", "provider_message_id": "msg_voice"]) -> JSONObject {
        ["source": "hgs_voice", "type": "UserMessage", "message_id": .string(id), "at": 50, "detail": .string(message.transcript),
         "attachments": [.object(VoiceProtocol.json(message).object("audio")!)],
         "voice": .object(VoiceProtocol.json(message).putting("native_binding", .object(binding)))]
    }
    private func native(_ state: String? = "queued", id: String = "msg_voice", source: String = "opencode_native") -> JSONObject {
        var row: JSONObject = ["source": .string(source), "type": "UserPromptSubmit", "message_id": .string(id), "detail": "Repeated text", "at": 51]
        if let state { row["delivery_state"] = .string(state) }
        return row
    }
    private func raw(_ voice: JSONObject, _ natives: JSONObject...) -> JSONObject {
        ["provider_messages": .array(natives.map(JSON.object)), "voice_messages": [.object(voice)]]
    }

    func testQueueStopRemovalAndConsumptionKeepMediaAndExactNativeStatus() throws {
        let voice = voice()
        let original = voice
        let boundMessage = try VoiceProtocol.message(voice.object("voice")!)
        let queued = NativeEvents.events(raw(voice, native()))
        XCTAssertEqual(2, queued.count)
        let media = try XCTUnwrap(queued.first { $0.voice != nil })
        let pending = try XCTUnwrap(queued.first { $0.voice == nil })
        XCTAssertEqual("queued", pending.deliveryState)
        XCTAssertFalse(media.originalIds.contains(pending.id))
        XCTAssertEqual(boundMessage, media.voice)
        var canonical = media
        canonical.id = "history:voice"; canonical.historyId = "voice"; canonical.historyCursor = "cursor"; canonical.historyEpoch = "epoch"
        canonical.originalIds = [media.id]
        let head = HistoryWindow(target: target, epoch: "epoch", events: [canonical], before: nil, after: "cursor", hasBefore: false, hasAfter: false, atHead: true)
        let waiting = head.inspection(queued)
        XCTAssertEqual(canonical, waiting.events.first) // inbox never acquires canonical read/cursor proof
        XCTAssertEqual(["queued"], waiting.events.filter { $0.voice == nil }.compactMap(\.deliveryState))
        let stopped = NativeEvents.events(raw(voice, native("not_delivered")))
        let afterStop = waiting.inspection(stopped, previous: queued)
        XCTAssertEqual(["not_delivered"], afterStop.events.filter { $0.voice == nil }.compactMap(\.deliveryState))
        XCTAssertEqual(2, RollingConversationHistory.merge(target: target, previousTarget: target, previous: queued, incoming: stopped, inspection: true).events.count)
        XCTAssertEqual(2, VoiceHistory.merge(target, native: stopped, outgoing: []).count)
        let removed = NativeEvents.events(raw(voice))
        XCTAssertEqual([canonical], afterStop.inspection(removed, previous: stopped).events)
        let consumed = NativeEvents.events(raw(voice, native(nil)))
        XCTAssertEqual(1, consumed.count)
        XCTAssertEqual(boundMessage, consumed.first?.voice)
        XCTAssertEqual(1, RollingConversationHistory.merge(target: target, previousTarget: target, previous: stopped, incoming: consumed, inspection: true).events.count)
        XCTAssertEqual(original, voice)
        XCTAssertEqual("Accepted by native app", RecordingDelivery.of("submitted").description)
        XCTAssertEqual("Accepted by native app", VoiceProtocol.label("submitted"))
    }

    func testWrongTargetMessageSourceAndAmbiguousBindingsNeverSuppressQueueRows() throws {
        XCTAssertEqual(1, NativeEvents.events(raw(voice(), native().putting("agent_id", "other-child"))).count)
        for other in [native(id: "msg_other"), native(source: "other_native")] {
            let rows = NativeEvents.events(raw(voice(), other))
            XCTAssertEqual(2, rows.count)
            XCTAssertEqual([], rows.first { $0.voice != nil }?.originalIds)
        }
        XCTAssertEqual(2, NativeEvents.events(raw(voice(), native(nil, id: "msg_other"))).count)
        let binding: JSONObject = ["provider_source": "opencode_native", "provider_message_id": "msg_voice", "journal_seq": 7]
        let ambiguous = raw(voice(binding), native()).putting("events", [.object(native("not_delivered").putting("seq", 7))])
        let rows = NativeEvents.events(ambiguous)
        XCTAssertEqual(3, rows.count)
        XCTAssertEqual([], rows.first { $0.voice != nil }?.originalIds)
        XCTAssertEqual(3, RollingConversationHistory.merge(target: target, previousTarget: target, previous: [], incoming: rows, inspection: true).events.count)
    }
}
