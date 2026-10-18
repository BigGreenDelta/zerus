import CryptoKit
import XCTest
@testable import ZerusKit

/// Ported from Android VoiceMessagesTest, VideoMessagesTest, VoicePlayerTest and VideoRecordingClockTest.
final class VoiceMessagesTests: XCTestCase {
    private let target = Target(computerId: "owner", session: "example", run: "run", conversation: "conversation", connectionId: "workspace")
    private let id = UUID().uuidString.lowercased()
    private let machine = "b4075bf0-9c22-4a67-b768-55af2d2936a4"
    private let audio = Attachment(id: UUID().uuidString.lowercased(), name: "voice.wav", mime: "audio/wav", bytes: 6444, sha256: String(repeating: "a", count: 64))

    private func message(_ state: String = "transcribing", revision: Int64 = 1) -> VoiceMessage {
        VoiceMessage(id: id, runId: target.run, conversationId: target.conversation, createdAt: 50, revision: revision, state: state,
                     audio: DisplayedAttachment(name: "voice.wav", mime: "audio/wav", bytes: 6444, messageId: id, index: 0), durationMs: 200,
                     transcript: state == "submitted" ? "Привет." : "")
    }
    private func entry(_ message: VoiceMessage? = nil) -> VoiceOutgoing {
        VoiceOutgoing(id: id, target: target, machineId: machine, profileId: "russian", audio: audio, durationMs: 200, createdAt: 50, message: message)
    }
    private func event(_ message: VoiceMessage? = nil) -> Event {
        let message = message ?? self.message()
        return Event(id: "native-voice", role: "You", text: message.transcript, time: "", at: 50, requestId: id, attachments: [message.audio],
                     source: "hgs_voice", nativeType: "UserMessage", voice: message)
    }
    private func roundTrip(_ state: VoiceState) throws -> VoiceState { try JSONDecoder().decode(VoiceState.self, from: JSONEncoder().encode(state)).recovered() }

    func testPreviewPreferenceRoundTripsWithoutChangingSavedMedia() throws {
        let pending = VoiceDraft(target: target, machineId: "", profileId: "", audio: audio, durationMs: 200, createdAt: 50)
        let state = VoiceState(draft: pending, outgoing: [entry(message("submitted"))], videoMode: true, previewBeforeSend: true)
        XCTAssertEqual(state, try roundTrip(state))
        var changed = state
        changed.previewBeforeSend = false
        let restored = try roundTrip(changed)
        XCTAssertEqual(state.draft, restored.draft)
        XCTAssertEqual(state.outgoing, restored.outgoing)
        XCTAssertTrue(restored.videoMode)
    }

    func testOnlyNativeSubmissionGetsASentIconAndUnknownStatesStayUnconfirmed() {
        XCTAssertEqual(.sent, RecordingDelivery.of("submitted"))
        for state in ["uploading", "queued", "transcribing", "ready", "waiting_agent", "delivering", "failed", "cancelled", "uncertain", "read", "finished", ""] {
            XCTAssertNotEqual(.sent, RecordingDelivery.of(state), state)
        }
        XCTAssertEqual(.uncertain, RecordingDelivery.of("read"))
        XCTAssertEqual("Accepted by native app", RecordingDelivery.sent.description)
    }

    func testAudioIsLosslesslyWrappedAndTamperingRejected() throws {
        let pcm = Data((0..<6400).map { UInt8($0 % 127) })
        let wav = try VoiceProtocol.wav(pcm)
        XCTAssertEqual(6444, wav.count)
        XCTAssertEqual(pcm, try VoiceProtocol.pcm(wav))
        for offset in [0, 4, 20, 22, 24, 32, 40] {
            var changed = wav
            changed[offset] &+= 1
            XCTAssertThrowsError(try VoiceProtocol.pcm(changed), "\(offset)")
        }
        XCTAssertThrowsError(try VoiceProtocol.wav(Data(count: 3199)))
        XCTAssertThrowsError(try VoiceProtocol.wav(Data(count: VoiceProtocol.maxPcmBytes + 2)))
    }

    func testWaveformBackfillSurvivesHistoryWithoutChangingAudioIdentity() throws {
        let previous = message("submitted", revision: 5)
        var backfilled = previous
        backfilled.revision = 6
        backfilled.waveform = (0..<64).map { $0 * 4 }
        XCTAssertEqual(backfilled, try VoiceProtocol.message(VoiceProtocol.json(backfilled)))
        let stored = VoiceState(outgoing: [entry(backfilled)])
        XCTAssertEqual(stored, try roundTrip(stored))
        var malformed = VoiceProtocol.json(previous)
        malformed["audio"] = .object(malformed.object("audio")!.putting("waveform", [255]))
        XCTAssertEqual(previous, try VoiceProtocol.message(malformed))
    }

    func testPreviewAndUploadIdentitySurviveRecoveryWithoutCreatingDelivery() throws {
        let preview = VoiceDraft(target: target, machineId: machine, profileId: "russian", audio: audio, durationMs: 200, createdAt: 50)
        let saved = VoiceState(selection: SpeechSelection(host: MachineKey(connectionId: "workspace", computerId: machine), profileId: "russian"), draft: preview)
        XCTAssertEqual(saved, try roundTrip(saved))
        var uploaded = saved
        uploaded.draft = nil
        uploaded.outgoing = [entry()]
        let recovered = try roundTrip(uploaded)
        XCTAssertNil(recovered.draft)
        XCTAssertEqual(id, recovered.outgoing.first?.id)
        XCTAssertEqual(audio, recovered.outgoing.first?.audio)
        XCTAssertEqual("uncertain", recovered.outgoing.first?.state)
        XCTAssertEqual(true, recovered.outgoing.first?.needsStatus)
        XCTAssertEqual(machine, recovered.outgoing.first?.machineId)
        XCTAssertEqual("owner", recovered.outgoing.first?.target.computerId)
    }

    func testRecordingWithoutDiscoveredProviderRemainsAnExactDurablePreview() throws {
        let preview = VoiceDraft(target: target, machineId: "", profileId: "", audio: audio, durationMs: 200, createdAt: 50)
        let saved = VoiceState(draft: preview)
        XCTAssertEqual(saved, try roundTrip(saved))
        XCTAssertEqual(SpeechSelection(), preview.selection)
    }

    func testSendingRequiresFreshExactProviderAndConversationWithoutFallback() {
        let draft = VoiceDraft(target: target, machineId: machine, profileId: "chosen", audio: audio, durationMs: 2_000, createdAt: 50)
        let chosen = SpeechProfile(id: "chosen", name: "Chosen", model: "synthetic", available: true)
        let fallback = SpeechProfile(id: "other", name: "Other", model: "synthetic", available: true)
        let computers = [VoiceComputer(id: machine, name: "GPU", profiles: [chosen, fallback])]
        func reason(_ value: VoiceDraft? = nil, confirmed: Target?? = .some(nil), loading: Bool = false, profiles: [VoiceComputer]? = nil) -> String {
            let confirmedTarget: Target? = confirmed == .some(nil) ? target : confirmed!
            return VoiceRoutes.providerReason(value ?? draft, confirmedTarget: confirmedTarget, loading: loading, computers: profiles ?? computers)
        }
        XCTAssertEqual("", reason())
        XCTAssertFalse(reason(loading: true).isEmpty)
        var others: [Target?] = [nil]
        for change in [{ (t: inout Target) in t.run = "different" }, { $0.conversation = "different" }, { $0.computerId = "different" }, { $0.connectionId = "different" }] {
            var copy = target
            change(&copy)
            others.append(copy)
        }
        for other in others { XCTAssertFalse(VoiceRoutes.providerReason(draft, confirmedTarget: other, loading: false, computers: computers).isEmpty) }
        for change in [{ (d: inout VoiceDraft) in d.machineId = "" }, { $0.profileId = "" }, { $0.profileId = "missing" }, { $0.machineId = UUID().uuidString.lowercased() }] {
            var copy = draft
            change(&copy)
            XCTAssertFalse(reason(copy).isEmpty)
        }
        var offline = chosen
        offline.available = false
        offline.reason = "Chosen worker offline"
        XCTAssertEqual("Chosen worker offline", reason(profiles: [VoiceComputer(id: machine, name: "GPU", profiles: [offline, fallback])]))
        var short = chosen
        short.maxDurationMs = 1_000
        XCTAssertFalse(reason(profiles: [VoiceComputer(id: machine, name: "GPU", profiles: [short])]).isEmpty)
        XCTAssertEqual(SpeechSelection(host: MachineKey(connectionId: target.connectionId, computerId: machine), profileId: "chosen"), draft.selection)
    }

    func testProviderRevisionPinsPreviewAndSubmitWithoutAdoptingEdits() throws {
        let preview = VoiceDraft(target: target, machineId: machine, profileId: "chosen", audio: audio, durationMs: 200, createdAt: 50, profileRevision: 7)
        XCTAssertEqual(.int(7), VoiceProtocol.provider(preview)["profile_revision"])
        let profile = SpeechProfile(id: "chosen", name: "Chosen", model: "synthetic", available: true, profileRevision: 7)
        func reason(_ p: SpeechProfile, _ draft: VoiceDraft? = nil) -> String {
            VoiceRoutes.providerReason(draft ?? preview, confirmedTarget: target, loading: false, computers: [VoiceComputer(id: machine, name: "GPU", profiles: [p])])
        }
        XCTAssertEqual("", reason(profile))
        var changed = profile
        changed.profileRevision = 8
        XCTAssertTrue(reason(changed).contains("changed"))
        changed.profileRevision = nil
        XCTAssertTrue(reason(changed).contains("changed"))
        var legacy = preview
        legacy.profileRevision = nil
        XCTAssertFalse(VoiceProtocol.provider(legacy).has("profile_revision"))
        XCTAssertFalse(reason(profile, legacy).isEmpty)
        let raw: JSONObject = ["schema": 1, "profiles": [["id": "chosen", "available": true, "profile_revision": 7]]]
        XCTAssertEqual(7, try SpeechProtocol.profiles(raw).first?.profileRevision)
        XCTAssertThrowsError(try SpeechProtocol.profiles(["schema": 1, "profiles": [["id": "chosen", "available": true, "profile_revision": 7.5]]]))
        XCTAssertThrowsError(try SpeechProtocol.profiles(["schema": 2]))
    }

    func testRejectsForeignIdentityAndUnboundedAudio() throws {
        let raw: JSONObject = ["schema": 1, "message": .object(VoiceProtocol.json(message()))]
        XCTAssertEqual(message(), try VoiceProtocol.response(raw, target: target, id: id))
        var otherRun = target
        otherRun.run = "other"
        var otherConversation = target
        otherConversation.conversation = "other"
        for other in [otherRun, otherConversation] { XCTAssertThrowsError(try VoiceProtocol.response(raw, target: other, id: id)) }
        XCTAssertThrowsError(try VoiceProtocol.response(raw, target: target, id: UUID().uuidString.lowercased()))
        var invalid = VoiceProtocol.json(message())
        invalid["audio"] = .object(invalid.object("audio")!.putting("request_id", .string(UUID().uuidString.lowercased())))
        XCTAssertThrowsError(try VoiceProtocol.message(invalid))
        var oversized = VoiceProtocol.json(message())
        oversized["audio"] = .object(oversized.object("audio")!.putting("bytes", .int(Int64(VoiceProtocol.maxPcmBytes + 45))))
        XCTAssertThrowsError(try VoiceProtocol.message(oversized))
    }

    func testOnlyProvenLocalNonDeliveryCanBecomeANewRecording() {
        var failed = entry()
        failed.uploadState = "failed"
        XCTAssertTrue(failed.canRestoreUnsent)
        XCTAssertFalse(entry().canRestoreUnsent)
        XCTAssertFalse(entry().recover().canRestoreUnsent)
        XCTAssertFalse(entry(message("failed", revision: 3)).canRestoreUnsent)
        XCTAssertFalse(entry(message("uncertain", revision: 3)).canRestoreUnsent)
        failed.audio = nil
        XCTAssertFalse(failed.canRestoreUnsent)
    }

    func testOlderStatusCannotUndoSubmittedTranscript() {
        let submitted = message("submitted", revision: 5)
        XCTAssertEqual(submitted, VoiceProtocol.newer(submitted, message("transcribing", revision: 4)))
        let native = event(submitted)
        let merged = VoiceHistory.merge(target, native: [native], outgoing: [entry(message())])
        XCTAssertEqual("submitted", merged.first?.voice?.state)
        XCTAssertEqual("Привет.", merged.first?.text)
        XCTAssertEqual(audio.id, merged.first?.voice?.audio.localId)
        XCTAssertEqual(1, VoiceHistory.merge(target, native: [event(), native], outgoing: [entry()]).count)
        XCTAssertEqual(5, VoiceHistory.merge(target, native: [event(), native], outgoing: [entry()]).first?.voice?.revision)
    }

    func testVoiceOverlayDoesNotMatchDifferentConversationOrEqualText() {
        let unrelated = Event(id: "text", role: "You", text: "Привет.", time: "", at: 50)
        XCTAssertEqual(2, VoiceHistory.merge(target, native: [unrelated], outgoing: [entry(message("submitted", revision: 2))]).count)
        var other = target
        other.conversation = "other"
        XCTAssertEqual([unrelated], VoiceHistory.merge(other, native: [unrelated], outgoing: [entry(message())]))
        other = target
        other.run = "other"
        XCTAssertEqual([unrelated], VoiceHistory.merge(other, native: [unrelated], outgoing: [entry(message())]))
    }

    func testLocalRowsKeepChronologicalPosition() {
        let early = Event(id: "early", role: "Codex", text: "Earlier", time: "", at: 40)
        let late = Event(id: "late", role: "Codex", text: "Later", time: "", at: 60)
        XCTAssertEqual(["early", "voice:" + id, "late"], VoiceHistory.merge(target, native: [early, late], outgoing: [entry()]).map(\.id))
    }

    func testProfilesKeepIndependentHostAndUnavailableSelections() throws {
        let row: JSONObject = ["machine_id": .string(machine), "name": "GPU", "profiles": [["id": "ru", "name": "Russian", "available": false]]]
        let computer = try XCTUnwrap(VoiceProtocol.computers(["schema": 1, "computers": [.object(row)]]).first)
        XCTAssertEqual(machine, computer.id)
        XCTAssertEqual(false, computer.profiles.first?.available)
        XCTAssertThrowsError(try VoiceProtocol.computers(["schema": 1, "computers": [.object(row.putting("machine_id", "../../other-host"))]]))
        XCTAssertThrowsError(try VoiceProtocol.computers(["schema": 1, "computers": [.object(row.putting("machine_id", .string(machine.uppercased())))]]))
    }

    func testStatsAndLegacySelectionResolvePhysicalMachineThroughCurrentRelayIdentity() throws {
        let relay = "83a06c3e-16a0-4312-96ab-1a63ec75a6f6"
        let parsed = try MachineCatalog.parse("workspace", ["id": .string(relay), "machine_id": .string(machine), "name": "GPU"])
        XCTAssertEqual(machine, parsed.machineId)
        XCTAssertEqual(MachineKey(connectionId: "workspace", computerId: relay), VoiceRoutes.statistics(MachineKey(connectionId: "workspace", computerId: machine), machines: [parsed]))
        XCTAssertNil(VoiceRoutes.statistics(MachineKey(connectionId: "other-workspace", computerId: machine), machines: [parsed]))
        let selected = SpeechSelection(host: MachineKey(connectionId: "workspace", computerId: relay), profileId: "ru")
        let computers = [VoiceComputer(id: machine, name: "GPU", profiles: [])]
        XCTAssertEqual(SpeechSelection(host: MachineKey(connectionId: "workspace", computerId: machine), profileId: "ru"),
                       VoiceRoutes.migratedSelection(selected, computers: computers, machines: [parsed]))
        XCTAssertEqual(selected, VoiceRoutes.migratedSelection(selected, computers: [], machines: [parsed]))
    }

    // MARK: Video

    private var video: VideoDraft {
        VideoDraft(file: Attachment(id: UUID().uuidString.lowercased(), name: "video.mp4", mime: "video/mp4", bytes: 100, sha256: String(repeating: "b", count: 64)),
                   durationMs: 1000, width: 480, height: 480)
    }

    func testMediaHaveOneMessageIdentityAcrossDurableRecoveryAndNativeReceipts() throws {
        let video = self.video
        let draft = VoiceDraft(target: target, machineId: "machine", profileId: "profile", audio: audio, durationMs: 1000, createdAt: 42, video: video)
        let pending = VoiceOutgoing(id: id, target: target, machineId: "machine", profileId: "profile", audio: audio, durationMs: 1000, createdAt: 42,
                                    uploadState: "failed", video: video)
        let state = VoiceState(draft: draft, outgoing: [pending], videoMode: true)
        XCTAssertEqual(state, try roundTrip(state))
        let displayed = pending.displayed()
        XCTAssertEqual([id, id], [displayed.audio.messageId, displayed.video?.file.messageId])
        XCTAssertEqual([0, 1], [displayed.audio.index, displayed.video?.file.index])
        var queued = displayed
        queued.state = "queued"
        let native = try VoiceProtocol.message(VoiceProtocol.json(queued))
        XCTAssertEqual(id, native.video?.file.messageId)
        XCTAssertEqual(video.durationMs, native.video?.durationMs)
        XCTAssertEqual(100, native.video?.file.bytes)
    }

    func testNativeHistoryAdoptsLocalVideoWithoutDuplicatingTheMessage() {
        let video = self.video
        let own = VoiceOutgoing(id: id, target: target, machineId: "machine", profileId: "profile", audio: audio, durationMs: 1000, createdAt: 42, video: video)
        var native = own.displayed()
        native.state = "submitted"
        native.revision = 2
        native.video = video.displayed(id)
        let row = Event(id: "native", role: "You", text: "", time: "", at: 42, requestId: id, attachments: [native.audio, native.video!.file], source: "hgs_voice",
                        nativeType: "UserMessage", voice: native)
        let merged = VoiceHistory.merge(target, native: [row], outgoing: [own])
        XCTAssertEqual(1, merged.count)
        XCTAssertEqual(id, merged.first?.voice?.id)
        XCTAssertEqual(video.file.id, merged.first?.voice?.video?.file.localId)
        XCTAssertEqual(video.file.sha256, merged.first?.voice?.video?.file.sha256)
        XCTAssertEqual([0, 1], merged.first?.attachments.map(\.index))
    }

    func testOptionalVideoDoesNotChangeLegacyAudio() {
        let audioOnly = VoiceOutgoing(id: id, target: target, machineId: "machine", profileId: "profile", audio: audio, durationMs: 1000, createdAt: 42)
        XCTAssertNil(audioOnly.displayed().video)
        XCTAssertFalse(VoiceProtocol.json(audioOnly.displayed()).has("video"))
        XCTAssertFalse(VoiceState().videoMode)
    }

    func testVideoMetadataAndWireAreBoundedAndExact() throws {
        let video = self.video
        let meta = VideoProtocol.json(video.displayed(id))
        XCTAssertEqual(VideoMessage(file: DisplayedAttachment(name: "video.mp4", mime: "video/mp4", bytes: 100, messageId: id, index: 1), durationMs: 1000,
                                    width: 480, height: 480), try VideoProtocol.message(meta, id: id))
        let replacements: [(String, JSON)] = [("duration_ms", 60001), ("width", 4000), ("index", 0), ("request_id", .string(UUID().uuidString.lowercased())),
                                              ("bytes", 10_485_761)]
        for (field, value) in replacements { XCTAssertThrowsError(try VideoProtocol.message(meta.putting(field, value), id: id), field) }
        XCTAssertThrowsError(try VideoProtocol.wire(video, Data(count: 99)))
        let wire = try VideoProtocol.wire(video, Data(count: 100))
        XCTAssertEqual("mp4", wire.string("format"))
        XCTAssertFalse(wire.has("poster_base64"))
    }

    func testExtractedAudioConversionHandlesStereoAndAacRatesWithoutChunkDependence() {
        for rate in [8000, 16000, 44100, 48000] {
            var converter = MediaPcm16Converter(rate: rate, channels: 2)
            for _ in 0..<rate {
                converter.sample(10000)
                converter.sample(-2000)
            }
            let bytes = converter.bytes
            XCTAssertEqual(32000, bytes.count, "\(rate)")
            for index in stride(from: 0, to: bytes.count, by: 2) {
                XCTAssertEqual(4000, Int16(bitPattern: UInt16(bytes[index]) | UInt16(bytes[index + 1]) << 8))
            }
        }
    }

    func testExtractedAudioHasAHardSixtySecondBound() {
        var converter = MediaPcm16Converter(rate: 8000, channels: 1)
        for _ in 0..<(8000 * 61) { converter.sample(200) }
        XCTAssertTrue(converter.full)
        XCTAssertEqual(1_920_000, converter.bytes.count)
        var microphone = MediaPcm16Converter(rate: 16000, channels: 1, limit: 9)
        for _ in 0..<100 { microphone.sample(100) }
        XCTAssertEqual(8, microphone.count)
    }

    func testPcmConversionPreservesSignAndEndian() {
        var converter = MediaPcm16Converter(rate: 16000, channels: 1)
        for value in [-32768, -1, 0, 32767] { converter.sample(value) }
        XCTAssertEqual(Data([0, 0x80, 0xFF, 0xFF, 0, 0, 0xFF, 0x7F]), converter.bytes)
    }

    // MARK: Player and clock

    func testSharedEncryptedAudioDoesNotSharePlaybackAcrossMessagesOrConversations() {
        let attachment = Attachment(id: "e1761172-d6de-42ad-bf01-c8b32e6f4847", name: "voice.wav", mime: "audio/wav", bytes: 6444, sha256: String(repeating: "a", count: 64)).displayed()
        var other = target
        other.conversation = "other"
        XCTAssertEqual(AttachmentIdentity.rowKey(target, attachment), AttachmentIdentity.rowKey(other, attachment))
        XCTAssertNotEqual(AttachmentIdentity.playbackKey(target, attachment), AttachmentIdentity.playbackKey(other, attachment))
        var another = attachment
        another.messageId = "another-message"
        another.index = 0
        XCTAssertNotEqual(AttachmentIdentity.playbackKey(target, attachment), AttachmentIdentity.playbackKey(target, another))
    }

    func testSeekingClampsEndpointsAndRejectsInvalidDurationsAndNonFiniteInput() {
        XCTAssertEqual(0, VoiceWaveform.seekPosition(-1, durationMs: 16_000))
        XCTAssertEqual(4_000, VoiceWaveform.seekPosition(0.25, durationMs: 16_000))
        XCTAssertEqual(16_000, VoiceWaveform.seekPosition(2, durationMs: 16_000))
        XCTAssertNil(VoiceWaveform.seekPosition(.nan, durationMs: 16_000))
        XCTAssertNil(VoiceWaveform.seekPosition(.infinity, durationMs: 16_000))
        XCTAssertNil(VoiceWaveform.seekPosition(0.5, durationMs: 0))
        XCTAssertNil(VoiceWaveform.seekPosition(0.5, durationMs: 120_001))
        XCTAssertEqual(0.25, VoiceWaveform.progress(4_000, durationMs: 16_000))
        XCTAssertEqual(1, VoiceWaveform.progress(20_000, durationMs: 16_000))
        XCTAssertEqual(0, VoiceWaveform.progress(-20, durationMs: 16_000))
        XCTAssertEqual("2:00", VoiceWaveform.time(120_000))
    }

    func testCountdownMatchesCaptureLimitAndRoundsUp() {
        XCTAssertEqual([59, 59, 58, 1, 0], [0, 1, 1_000, 58_999, 59_000].map(VideoRecordingClock.remainingSeconds))
        XCTAssertEqual(59, VideoRecordingClock.remainingSeconds(Int.min))
        XCTAssertEqual(0, VideoRecordingClock.remainingSeconds(Int.max))
        XCTAssertEqual(0, VideoRecordingClock.progress(-1))
        XCTAssertEqual(1, VideoRecordingClock.progress(60_000))
        XCTAssertEqual(0.5, VideoRecordingClock.progress(29_500), accuracy: 0.0001)
    }
}

/// Ported from Android VoiceGestureTest and VoiceCaptureFeedbackTest.
final class VoiceCaptureTests: XCTestCase {
    func testShortTapSwitchesAndHoldReleaseSendsOnce() {
        let g = VoiceGesture(cancelDistance: 80)
        XCTAssertTrue(g.begin())
        XCTAssertEqual(.switchMode, g.release())
        XCTAssertNil(g.release())
        XCTAssertTrue(g.begin())
        XCTAssertEqual(.start, g.hold())
        XCTAssertNil(g.hold())
        g.captured(42)
        XCTAssertNil(g.move(horizontal: -10))
        XCTAssertEqual(.finish(42, .send), g.release())
        XCTAssertNil(g.interrupt())
        XCTAssertNil(g.release())
    }

    func testCancelCommitsAndPartialLeftReleaseCancels() {
        let g = VoiceGesture(cancelDistance: 80)
        _ = g.begin(); _ = g.hold(); g.captured(7)
        XCTAssertEqual(.finish(7, .cancel), g.move(horizontal: -80))
        XCTAssertNil(g.move(horizontal: 0))
        XCTAssertNil(g.release())
        _ = g.begin(); _ = g.hold(); g.captured(8); _ = g.move(horizontal: -48)
        XCTAssertEqual(.finish(8, .cancel), g.release())
    }

    func testUpwardLockSurvivesReleaseAndInterruption() {
        let g = VoiceGesture(cancelDistance: 80, lockDistance: 57)
        _ = g.begin(); _ = g.hold(); g.captured(3)
        XCTAssertEqual(.lock(3), g.move(horizontal: 0, vertical: -57))
        XCTAssertTrue(g.locked)
        XCTAssertNil(g.move(horizontal: -100))
        XCTAssertNil(g.release())
        XCTAssertNil(g.interrupt())
    }

    func testInterruptedAndPermissionPressNeverSendsOrAcquiresLateCapture() {
        let g = VoiceGesture(cancelDistance: 80)
        _ = g.begin(); _ = g.hold()
        XCTAssertNil(g.release())
        g.captured(9)
        XCTAssertNil(g.release())
        _ = g.begin(); _ = g.hold(); g.captured(2)
        XCTAssertEqual(.finish(2, .preview), g.interrupt())
        g.captured(3)
        XCTAssertNil(g.release())
        _ = g.begin(); _ = g.move(horizontal: -20)
        XCTAssertNil(g.hold())
        XCTAssertNil(g.release())
        _ = g.begin(); _ = g.hold(); g.captured(4)
        XCTAssertNil(g.move(horizontal: .nan))
        XCTAssertNil(g.move(horizontal: -.infinity))
        XCTAssertEqual(.finish(4, .send), g.release())
    }

    private final class Events {
        var values: [VoiceCaptureFeedback.Event] = []
    }

    func testAcceptedCaptureHasOneStartAndOneExplicitFinish() {
        let events = Events()
        let feedback = VoiceCaptureFeedback { events.values.append($0) }
        feedback.didStart(); feedback.didStart()
        feedback.finish(userInitiated: true)
        XCTAssertEqual([.start], events.values)
        feedback.finish(userInitiated: true, cancelled: true)
        feedback.stopped(); feedback.stopped()
        feedback.didStart()
        XCTAssertEqual([.start, .finish], events.values)
    }

    func testCancelRemainsTerminalAfterFingerReturnsOrReleases() {
        let events = Events()
        let feedback = VoiceCaptureFeedback { events.values.append($0) }
        feedback.didStart()
        feedback.finish(userInitiated: true, cancelled: true)
        feedback.finish(userInitiated: true)
        feedback.stopped()
        XCTAssertEqual([.start, .cancel], events.values)
    }

    func testReleaseBeforeMicrophoneReadySuppressesLateStartAndFinish() {
        let events = Events()
        let feedback = VoiceCaptureFeedback { events.values.append($0) }
        feedback.finish(userInitiated: true)
        feedback.stopped()
        feedback.didStart()
        feedback.finish(userInitiated: true)
        XCTAssertEqual([], events.values)
    }

    func testBackgroundAndNaturalCompletionDoNotPulse() {
        let events = Events()
        let feedback = VoiceCaptureFeedback { events.values.append($0) }
        feedback.didStart()
        feedback.finish(userInitiated: false)
        feedback.finish(userInitiated: true, cancelled: true)
        feedback.stopped()
        XCTAssertEqual([.start], events.values)
        let natural = Events()
        let other = VoiceCaptureFeedback { natural.values.append($0) }
        other.didStart(); other.stopped()
        other.finish(userInitiated: true); other.stopped()
        XCTAssertEqual([.start], natural.values)
    }

    func testElapsedFormattingUsesCapturedTenthsAcrossMinuteBoundary() {
        XCTAssertEqual(["0:00.0", "0:00.0", "0:00.1", "0:59.9", "1:00.0", "2:00.0"], [-1, 99, 100, 59_999, 60_000, 120_000].map(voiceRecordingTime))
    }
}

/// Ported from Android SpeechConfigurationTest and SpeechProviderHelpTest.
final class SpeechConfigurationTests: XCTestCase {
    private let host = SpeechConfigHost(key: MachineKey(connectionId: "workspace", computerId: "relay-node"), machineId: "b4075bf0-9c22-4a67-b768-55af2d2936a4", name: "GPU")

    private func metadata(_ key: P256.KeyAgreement.PrivateKey) -> SpeechConfigKey {
        let der = key.publicKey.derRepresentation
        return SpeechConfigKey(id: SHA256.hash(data: der).map { String(format: "%02x", $0) }.joined(), algorithm: SpeechCredential.algorithm,
                               publicSpki: der.base64EncodedString())
    }
    private func config(_ key: SpeechConfigKey? = nil) -> SpeechConfiguration {
        SpeechConfiguration(machineId: host.machineId, revision: 3, serviceAvailable: true, serviceReason: "",
                            adapters: [SpeechConfigAdapter(id: "gigaam", models: ["v3_e2e_rnnt"], devices: ["cpu", "cuda"]),
                                       SpeechConfigAdapter(id: "openai_http", models: [], devices: [])], profiles: [], key: key)
    }
    private func profile() -> SpeechConfigProfile {
        SpeechConfigProfile(id: "remote", revision: 0, name: "Remote Russian", adapter: "openai_http", model: "gigaam", language: "ru",
                            baseUrl: "https://speech.example.test/v1")
    }
    private func decrypt(_ key: P256.KeyAgreement.PrivateKey, _ payload: JSONObject, _ sealed: JSONObject) throws -> String {
        let ephemeral = try P256.KeyAgreement.PublicKey(derRepresentation: XCTUnwrap(Data(base64Encoded: sealed.string("ephemeral_public_key"))))
        let shared = try key.sharedSecretFromKeyAgreement(with: ephemeral).withUnsafeBytes { Data($0) }
        let salt = Data(SHA256.hash(data: key.publicKey.derRepresentation))
        let context = try SpeechCredential.context(payload)
        let prk = Data(HMAC<SHA256>.authenticationCode(for: shared, using: SymmetricKey(data: salt)))
        let aes = SymmetricKey(data: Data(HMAC<SHA256>.authenticationCode(for: context + Data([1]), using: SymmetricKey(data: prk))))
        let combined = try XCTUnwrap(Data(base64Encoded: sealed.string("ciphertext")))
        let box = try AES.GCM.SealedBox(nonce: AES.GCM.Nonce(data: XCTUnwrap(Data(base64Encoded: sealed.string("nonce")))),
                                        ciphertext: combined.dropLast(16), tag: combined.suffix(16))
        return String(decoding: try AES.GCM.open(box, using: aes, authenticating: context), as: UTF8.self)
    }
    private func fixture() throws -> JSONObject {
        let url = URL(fileURLWithPath: #filePath).deletingLastPathComponent().appendingPathComponent("../../../../../tests/fixtures/speech-credential-v1.json").standardized
        return try XCTUnwrap(JSON.parse(Data(contentsOf: url)).object)
    }

    func testSharedKnownAnswerVectorMatchesExactlyAndDecrypts() throws {
        let fixture = try fixture()
        let host = try P256.KeyAgreement.PrivateKey(derRepresentation: XCTUnwrap(Data(base64Encoded: fixture.string("host_private_pkcs8"))))
        XCTAssertEqual(fixture.string("host_public_spki"), host.publicKey.derRepresentation.base64EncodedString())
        let request = try XCTUnwrap(fixture.object("request"))
        XCTAssertEqual(Data(base64Encoded: fixture.string("context_base64")), try SpeechCredential.context(request))
        let expected = try XCTUnwrap(request.object("credential")?.object("sealed_value"))
        XCTAssertEqual(fixture.string("plaintext"), try decrypt(host, request, expected))
        let scalar = Data(repeating: 0, count: 31) + Data([2])
        let produced = try SpeechCredential.seal(metadata(host), request, fixture.string("plaintext"),
                                                 ephemeral: P256.KeyAgreement.PrivateKey(rawRepresentation: scalar), nonce: Data(0..<12))
        XCTAssertEqual(expected, produced)
        let random = try SpeechCredential.seal(metadata(host), request, fixture.string("plaintext"))
        XCTAssertEqual(fixture.string("plaintext"), try decrypt(host, request, random))
    }

    func testFieldsUseUtf8ByteBoundsOptionalLanguageAndExactIntegerRevisions() throws {
        var value = profile()
        value.language = ""
        try SpeechConfigProtocol.validate(value, config())
        value.language = "en"
        try SpeechConfigProtocol.validate(value, config())
        for language in ["RU", "rus", "r", " ru", "ру"] {
            value.language = language
            XCTAssertThrowsError(try SpeechConfigProtocol.validate(value, config()), language)
        }
        value = profile()
        value.name = String(repeating: "я", count: 65)
        XCTAssertThrowsError(try SpeechConfigProtocol.validate(value, config()))
        for bad: JSON in [1.5, "7", true, -1, 9_007_199_254_740_992] {
            XCTAssertThrowsError(try SpeechConfigProtocol.revision(["revision": bad], "revision"))
        }
        XCTAssertEqual(0, try SpeechConfigProtocol.revision(["revision": 0], "revision"))
    }

    func testSealedKeyBindsEveryProfileFieldHostRevisionAndOperation() throws {
        let key = P256.KeyAgreement.PrivateKey(), secret = "synthetic-test-key"
        let op = try SpeechConfigProtocol.upsert(host, config(metadata(key)), profile(), credentialMode: "replace", secret: secret)
        XCTAssertFalse(JSON.object(op.payload).compact.contains(secret))
        let sealed = try XCTUnwrap(op.payload.object("credential")?.object("sealed_value"))
        XCTAssertEqual(secret, try decrypt(key, op.payload, sealed))
        let changes: [(String, JSON)] = [("expected_machine_id", .string(UUID().uuidString.lowercased())), ("request_id", .string(UUID().uuidString.lowercased())),
                                         ("expected_revision", 4)]
        for (field, value) in changes { XCTAssertThrowsError(try decrypt(key, op.payload.putting(field, value), sealed), field) }
        for field in ["id", "name", "adapter", "model", "language", "device", "base_url", "allow_insecure_http"] {
            let changed = op.payload.object("profile")!.putting(field, field == "allow_insecure_http" ? .bool(true) : "changed")
            XCTAssertThrowsError(try decrypt(key, op.payload.putting("profile", .object(changed)), sealed), field)
        }
        XCTAssertThrowsError(try decrypt(P256.KeyAgreement.PrivateKey(), op.payload, sealed))
        let restored = try SpeechConfigProtocol.restored(JSONDecoder().decode([SpeechConfigOperation].self, from: JSONEncoder().encode([op])))
        XCTAssertEqual([op], restored)
        XCTAssertFalse(String(decoding: try JSONEncoder().encode([op]), as: UTF8.self).contains(secret))
    }

    func testRejectsMismatchedKeyUnsupportedCurveControlFieldsAndUrlCredentials() throws {
        let key = P256.KeyAgreement.PrivateKey(), meta = metadata(key)
        var wrong = meta
        wrong.id = String(repeating: "0", count: 64)
        XCTAssertThrowsError(try SpeechConfigProtocol.upsert(host, config(wrong), profile(), credentialMode: "replace", secret: "synthetic"))
        var control = profile()
        control.name = "a\nb"
        XCTAssertThrowsError(try SpeechConfigProtocol.upsert(host, config(meta), control, credentialMode: "replace", secret: "synthetic"))
        XCTAssertThrowsError(try SpeechConfigProtocol.upsert(host, config(meta), profile(), credentialMode: "replace", secret: "synthetic\nkey"))
        for url in ["https://user:password@speech.example.test/v1", "https://speech.example.test/v1?key=test", "https://speech.example.test/v1#key", "http://127.0.0.1/v1"] {
            var value = profile()
            value.baseUrl = url
            XCTAssertThrowsError(try SpeechConfigProtocol.validate(value, config()), url)
        }
        var local = profile()
        local.baseUrl = "http://127.0.0.1:8000/v1"
        local.allowInsecureHttp = true
        try SpeechConfigProtocol.validate(local, config())
        var gigaam = SpeechConfigProfile(id: "local", revision: 1, name: "Local", adapter: "gigaam", model: "v3_e2e_rnnt", language: "ru", device: "cuda")
        try SpeechConfigProtocol.validate(gigaam, config())
        gigaam.device = "/tmp/executable"
        XCTAssertThrowsError(try SpeechConfigProtocol.validate(gigaam, config()))
        let otherCurve = P384.KeyAgreement.PrivateKey().publicKey.derRepresentation
        let p384 = SpeechConfigKey(id: SHA256.hash(data: otherCurve).map { String(format: "%02x", $0) }.joined(), algorithm: SpeechCredential.algorithm,
                                   publicSpki: otherCurve.base64EncodedString())
        XCTAssertThrowsError(try SpeechConfigProtocol.upsert(host, config(p384), profile(), credentialMode: "replace", secret: "synthetic"))
    }

    func testConfigurationMutationStatusAndCasStayBoundToTheirExactHost() throws {
        var saved = profile()
        saved.revision = 2
        let op = SpeechConfigProtocol.remove(host, config(), saved)
        XCTAssertEqual(3, op.payload.int("expected_revision"))
        XCTAssertEqual(host.machineId, op.payload.string("expected_machine_id"))
        XCTAssertEqual(2, SpeechConfigProtocol.test(host, saved).payload.int("expected_profile_revision"))
        let result: JSONObject = ["schema": 1, "machine_id": .string(host.machineId), "request_id": .string(op.id), "state": "failed", "error_code": "revision_conflict"]
        XCTAssertEqual("revision_conflict", try SpeechConfigProtocol.result(result, op).errorCode)
        XCTAssertThrowsError(try SpeechConfigProtocol.result(result.putting("request_id", .string(UUID().uuidString.lowercased())), op))
        XCTAssertThrowsError(try SpeechConfigProtocol.result(result.putting("machine_id", .string(UUID().uuidString.lowercased())), op))
        let list: JSONObject = ["schema": 1, "machine_id": .string(host.machineId), "revision": 0, "service": ["available": false], "adapters": [], "profiles": []]
        XCTAssertFalse(try SpeechConfigProtocol.parse(list, host: host).serviceAvailable)
        var other = host
        other.machineId = UUID().uuidString.lowercased()
        XCTAssertThrowsError(try SpeechConfigProtocol.parse(list, host: other))
    }

    func testDocumentationUsesOnlyKnownProviderLinksAndReadinessIsPerComputer() {
        let base = "https://github.com/ufna/zerus/blob/main/docs/speech-setup.md"
        XCTAssertEqual([base, base + "#local-gigaam", base + "#openai-compatible-server", base],
                       [nil, "gigaam", "openai_http", "https://private.example.test/?key=secret"].map { SpeechConfigProtocol.documentation($0).url })
        let configuration = SpeechConfiguration(machineId: host.machineId, revision: 1, serviceAvailable: true, serviceReason: "", adapters: [], profiles: [], key: nil)
        let unknown = SpeechConfigProtocol.readiness(host, nil, loading: false)
        var foreign = configuration
        foreign.machineId = "another-computer"
        XCTAssertEqual(unknown, SpeechConfigProtocol.readiness(host, foreign, loading: false))
        XCTAssertNotEqual(unknown, SpeechConfigProtocol.readiness(host, configuration, loading: false))
        XCTAssertNotEqual(SpeechConfigProtocol.readiness(host, configuration, loading: false), SpeechConfigProtocol.readiness(host, configuration, loading: true))
        var stopped = configuration
        stopped.serviceAvailable = false
        stopped.serviceReason = "Fixture service stopped"
        XCTAssertTrue(SpeechConfigProtocol.readiness(host, stopped, loading: false).contains("Fixture service stopped"))
    }
}
