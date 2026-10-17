import Foundation

// MARK: Speech providers

public struct SpeechSelection: Codable, Hashable, Sendable {
    public var host: MachineKey?
    public var profileId: String
    public var profileRevision: Int64?

    public init(host: MachineKey? = nil, profileId: String = "", profileRevision: Int64? = nil) {
        self.host = host
        self.profileId = profileId
        self.profileRevision = profileRevision
    }
}

public struct SpeechProfile: Codable, Hashable, Sendable {
    public var id: String
    public var name: String
    public var model: String
    public var available: Bool
    public var reason: String
    public var maxDurationMs: Int
    public var profileRevision: Int64?

    public init(id: String, name: String, model: String, available: Bool, reason: String = "", maxDurationMs: Int = 120_000, profileRevision: Int64? = nil) {
        self.id = id
        self.name = name
        self.model = model
        self.available = available
        self.reason = reason
        self.maxDurationMs = maxDurationMs
        self.profileRevision = profileRevision
    }
}

public enum SpeechProtocol {
    public static let maxBytes = 3_840_000
    static let maxRevision: Int64 = 9_007_199_254_740_991

    /// An optional positive integer revision; any other present value is invalid.
    public static func revision(_ raw: JSONObject, _ field: String = "profile_revision") throws -> Int64? {
        guard let value = raw[field], !value.isNull else { return nil }
        guard case .int(let number) = value, (1...maxRevision).contains(number) else { throw ZerusError.invalid("Invalid provider revision.") }
        return number
    }

    public static func profiles(_ value: JSONObject) throws -> [SpeechProfile] {
        guard value.int("schema") == 1 else { throw ZerusError.invalid("This computer uses an unsupported voice protocol.") }
        let rows = value.objects("profiles")
        guard rows.count <= 100, Set(rows.map { $0.string("id") }).count == rows.count else { throw ZerusError.invalid("Invalid voice profiles.") }
        return try rows.map { row in
            let id = row.string("id")
            guard !id.trimmingCharacters(in: .whitespaces).isEmpty, id.count <= 128 else { throw ZerusError.invalid("Invalid voice profiles.") }
            return SpeechProfile(id: id, name: row.string("name").nonEmpty ?? id, model: row.string("model"), available: row.bool("available"),
                                 reason: row.string("reason"), maxDurationMs: min(max(row.has("max_duration_ms") ? row.int("max_duration_ms") : 120_000, 1_000), 120_000),
                                 profileRevision: try revision(row))
        }
    }
}

public struct VoiceComputer: Hashable, Sendable {
    public var id: String
    public var name: String
    public var profiles: [SpeechProfile]

    public init(id: String, name: String, profiles: [SpeechProfile]) {
        self.id = id
        self.name = name
        self.profiles = profiles
    }
}

// MARK: Recordings

public struct VideoDraft: Codable, Hashable, Sendable {
    public var file: Attachment
    public var durationMs: Int
    public var width: Int
    public var height: Int
    public var posterBase64: String

    public init(file: Attachment, durationMs: Int, width: Int, height: Int, posterBase64: String = "") {
        self.file = file
        self.durationMs = durationMs
        self.width = width
        self.height = height
        self.posterBase64 = posterBase64
    }

    public func displayed(_ id: String = "") -> VideoMessage {
        var file = file.displayed()
        file.messageId = id
        file.index = 1
        return VideoMessage(file: file, durationMs: durationMs, width: width, height: height, posterBase64: posterBase64)
    }
}

/// A stopped recording waiting for an explicit Send; its provider route is pinned.
public struct VoiceDraft: Codable, Hashable, Sendable {
    public var target: Target
    public var machineId: String
    public var profileId: String
    public var audio: Attachment
    public var durationMs: Int
    public var createdAt: Double
    public var profileRevision: Int64?
    public var video: VideoDraft?

    public init(target: Target, machineId: String, profileId: String, audio: Attachment, durationMs: Int, createdAt: Double,
                profileRevision: Int64? = nil, video: VideoDraft? = nil) {
        self.target = target
        self.machineId = machineId
        self.profileId = profileId
        self.audio = audio
        self.durationMs = durationMs
        self.createdAt = createdAt
        self.profileRevision = profileRevision
        self.video = video
    }

    public var selection: SpeechSelection {
        SpeechSelection(host: machineId.isEmpty ? nil : MachineKey(connectionId: target.connectionId, computerId: machineId), profileId: profileId,
                        profileRevision: profileRevision)
    }
}

/// One explicit upload, durable before transport; uncertain results are only ever read back by status.
public struct VoiceOutgoing: Codable, Hashable, Sendable {
    public var id: String
    public var target: Target
    public var machineId: String
    public var profileId: String
    public var audio: Attachment?
    public var durationMs: Int
    public var createdAt: Double
    public var message: VoiceMessage?
    public var uploadState: String
    public var error: String
    public var profileRevision: Int64?
    public var video: VideoDraft?

    public init(id: String, target: Target, machineId: String, profileId: String, audio: Attachment?, durationMs: Int, createdAt: Double,
                message: VoiceMessage? = nil, uploadState: String = "uploading", error: String = "", profileRevision: Int64? = nil, video: VideoDraft? = nil) {
        self.id = id
        self.target = target
        self.machineId = machineId
        self.profileId = profileId
        self.audio = audio
        self.durationMs = durationMs
        self.createdAt = createdAt
        self.message = message
        self.uploadState = uploadState
        self.error = error
        self.profileRevision = profileRevision
        self.video = video
    }

    public var state: String { message?.state ?? uploadState }
    public var terminal: Bool { ["submitted", "failed", "cancelled", "uncertain"].contains(state) }
    public var needsStatus: Bool { (message == nil && uploadState == "uncertain") || !terminal }
    public var canRestoreUnsent: Bool { message == nil && uploadState == "failed" && audio != nil }

    public var effectiveTarget: Target {
        guard target.conversation.isEmpty, let conversation = message?.conversationId, !conversation.isEmpty else { return target }
        var copy = target
        copy.conversation = conversation
        return copy
    }

    public func belongs(to other: Target) -> Bool { target == other || target.resolves(to: other) }

    public func recover() -> VoiceOutgoing {
        guard message == nil, uploadState == "uploading" else { return self }
        var copy = self
        copy.uploadState = "uncertain"
        copy.error = "Sending was interrupted. Check this message's status; it has not been resent."
        return copy
    }

    public func displayed() -> VoiceMessage {
        var result: VoiceMessage
        if let message {
            result = message
        } else {
            var file = audio?.displayed() ?? DisplayedAttachment(name: "voice.wav", mime: "audio/wav", bytes: 0)
            file.messageId = id
            file.index = 0
            result = VoiceMessage(id: id, runId: target.run, conversationId: target.conversation, createdAt: createdAt, revision: 0, state: uploadState,
                                  audio: file, durationMs: durationMs, error: error)
        }
        if let audio {
            result.audio.localId = audio.id
            result.audio.sha256 = audio.sha256
        }
        if let local = video {
            var shown = result.video ?? local.displayed(id)
            shown.file.localId = local.file.id
            shown.file.sha256 = local.file.sha256
            result.video = shown
        }
        return result
    }
}

public struct VoiceState: Codable, Hashable, Sendable {
    public var selection: SpeechSelection
    public var draft: VoiceDraft?
    public var outgoing: [VoiceOutgoing]
    public var videoMode: Bool
    public var previewBeforeSend: Bool

    public init(selection: SpeechSelection = SpeechSelection(), draft: VoiceDraft? = nil, outgoing: [VoiceOutgoing] = [], videoMode: Bool = false,
                previewBeforeSend: Bool = false) {
        self.selection = selection
        self.draft = draft
        self.outgoing = outgoing
        self.videoMode = videoMode
        self.previewBeforeSend = previewBeforeSend
    }

    /// After a restart an interrupted upload is unconfirmed and is only checked by status.
    public func recovered() throws -> VoiceState {
        guard outgoing.count <= 100, Set(outgoing.map(\.id)).count == outgoing.count, outgoing.allSatisfy({ AttachmentPolicy.validId($0.id) }) else {
            throw ZerusError.invalid("Saved voice messages are invalid.")
        }
        var copy = self
        copy.outgoing = outgoing.map { $0.recover() }
        return copy
    }

    /// Every local media copy still referenced by a preview or a receipt.
    public var protectedIds: Set<String> {
        Set(outgoing.flatMap { [$0.audio?.id, $0.video?.file.id].compactMap { $0 } } + [draft?.audio.id, draft?.video?.file.id].compactMap { $0 })
    }
}

/// Native submission is confirmed delivery to the agent, never evidence of reading or completion.
public enum RecordingDelivery: String, Sendable {
    case uploading, processing, sent, failed, cancelled, uncertain

    public var description: String {
        switch self {
        case .uploading: "Sending recording"
        case .processing: "Processing recording"
        case .sent: "Sent to agent"
        case .failed: "Recording failed"
        case .cancelled: "Delivery cancelled"
        case .uncertain: "Delivery unconfirmed"
        }
    }

    public static func of(_ state: String) -> RecordingDelivery {
        switch state {
        case "uploading": .uploading
        case "queued", "transcribing", "ready", "waiting_agent", "delivering": .processing
        case "submitted": .sent
        case "failed": .failed
        case "cancelled": .cancelled
        default: .uncertain
        }
    }
}

extension VideoProtocol {
    public static func json(_ video: VideoMessage) -> JSONObject {
        var raw: JSONObject = ["request_id": .string(video.file.messageId), "index": 1, "name": "video.mp4", "mime": "video/mp4", "bytes": .int(video.file.bytes),
                               "duration_ms": .int(Int64(video.durationMs)), "width": .int(Int64(video.width)), "height": .int(Int64(video.height))]
        if !video.posterBase64.isEmpty { raw["poster_base64"] = .string(video.posterBase64) }
        return raw
    }

    public static func wire(_ video: VideoDraft, _ bytes: Data) throws -> JSONObject {
        guard Int64(bytes.count) == video.file.bytes, (1...maxBytes).contains(bytes.count) else { throw ZerusError.invalid("The saved video changed.") }
        var raw: JSONObject = ["format": "mp4", "data_base64": .string(bytes.base64EncodedString()), "duration_ms": .int(Int64(video.durationMs)),
                               "width": .int(Int64(video.width)), "height": .int(Int64(video.height))]
        if !video.posterBase64.isEmpty { raw["poster_base64"] = .string(video.posterBase64) }
        return raw
    }
}

extension VoiceProtocol {
    public static func provider(_ draft: VoiceDraft) -> JSONObject {
        var raw: JSONObject = ["machine_id": .string(draft.machineId), "profile_id": .string(draft.profileId)]
        if let revision = draft.profileRevision { raw["profile_revision"] = .int(revision) }
        return raw
    }

    public static func payload(_ target: Target, requestId: String) -> JSONObject {
        ["request_id": .string(requestId), "expected_run_id": .string(target.run), "expected_conversation_id": .string(target.conversation)]
    }

    public static func computers(_ value: JSONObject) throws -> [VoiceComputer] {
        guard value.int("schema") == 1 else { throw ZerusError.invalid("This computer uses an unsupported voice message protocol.") }
        let rows = value.objects("computers")
        guard rows.count <= 100, Set(rows.map { $0.string("machine_id") }).count == rows.count else { throw ZerusError.invalid("Invalid voice computers.") }
        return try rows.map { row in
            let id = row.string("machine_id")
            guard AttachmentPolicy.validId(id) else { throw ZerusError.invalid("Invalid voice computers.") }
            return VoiceComputer(id: id, name: row.string("name").nonEmpty ?? id,
                                 profiles: try SpeechProtocol.profiles(["schema": 1, "profiles": row["profiles"] ?? .array([])]))
        }
    }

    public static func response(_ raw: JSONObject, target: Target, id: String) throws -> VoiceMessage {
        guard raw.int("schema") == 1, let body = raw.object("message") else { throw ZerusError.invalid("Unsupported voice message response.") }
        let message = try self.message(body)
        guard message.id == id, message.runId == target.run, target.conversation.isEmpty || message.conversationId == target.conversation else {
            throw ZerusError.mismatch("The computer returned a different voice message.")
        }
        return message
    }

    public static func json(_ message: VoiceMessage) -> JSONObject {
        var audio: JSONObject = ["request_id": .string(message.id), "index": 0, "name": .string(message.audio.name), "mime": "audio/wav",
                                 "bytes": .int(message.audio.bytes), "duration_ms": .int(Int64(message.durationMs))]
        if VoiceWaveform.valid(message.waveform) { audio["waveform"] = .array(message.waveform.map { .int(Int64($0)) }) }
        var raw: JSONObject = ["id": .string(message.id), "run_id": .string(message.runId), "conversation_id": .string(message.conversationId),
                               "created_at": .double(message.createdAt), "revision": .int(message.revision), "state": .string(message.state),
                               "transcript": .string(message.transcript), "error": .string(message.error), "error_code": .string(message.errorCode), "audio": .object(audio)]
        if !message.nativeBinding.isEmpty, let binding = try? JSON.parse(message.nativeBinding).object { raw["native_binding"] = .object(binding) }
        if let video = message.video { raw["video"] = .object(VideoProtocol.json(video)) }
        return raw
    }

    /// A canonical 16 kHz mono 16-bit WAV around the exact PCM bytes.
    public static func wav(_ pcm: Data) throws -> Data {
        guard (3_200...maxPcmBytes).contains(pcm.count), pcm.count % 2 == 0 else { throw ZerusError.invalid("The recording is too short or too long.") }
        var data = Data(capacity: pcm.count + 44)
        func uint32(_ value: UInt32) { withUnsafeBytes(of: value.littleEndian) { data.append(contentsOf: $0) } }
        func uint16(_ value: UInt16) { withUnsafeBytes(of: value.littleEndian) { data.append(contentsOf: $0) } }
        data.append(contentsOf: Array("RIFF".utf8))
        uint32(UInt32(pcm.count + 36))
        data.append(contentsOf: Array("WAVEfmt ".utf8))
        uint32(16)
        uint16(1)
        uint16(1)
        uint32(16_000)
        uint32(32_000)
        uint16(2)
        uint16(16)
        data.append(contentsOf: Array("data".utf8))
        uint32(UInt32(pcm.count))
        data.append(pcm)
        return data
    }

    public static func pcm(_ wav: Data) throws -> Data {
        guard (3_244...(maxPcmBytes + 44)).contains(wav.count) else { throw ZerusError.invalid("The saved recording is invalid.") }
        let pcm = Data(wav.dropFirst(44))
        guard let canonical = try? self.wav(pcm), canonical == wav else { throw ZerusError.invalid("The saved recording is invalid.") }
        return pcm
    }
}

public enum VoiceRoutes {
    public static func providerReason(_ draft: VoiceDraft, confirmedTarget: Target?, loading: Bool, computers: [VoiceComputer]) -> String {
        if loading { return "Loading voice profiles. Your recording is saved." }
        if confirmedTarget != draft.target { return "Refresh voice profiles for this recording's conversation before sending." }
        if draft.machineId.isEmpty || draft.profileId.isEmpty { return "Choose a voice computer and profile before sending." }
        guard let computer = computers.first(where: { $0.id == draft.machineId }) else {
            return "The selected voice computer is unavailable. Choose a provider or refresh."
        }
        guard let profile = computer.profiles.first(where: { $0.id == draft.profileId }) else {
            return "The selected voice profile is unavailable. Choose a provider or refresh."
        }
        if profile.profileRevision != draft.profileRevision { return "This provider has changed. Select it again to confirm where this recording will be sent." }
        if !profile.available { return profile.reason.nonEmpty ?? "The selected voice profile is unavailable." }
        if draft.durationMs > profile.maxDurationMs { return "This recording exceeds the selected profile's duration limit. Choose another profile." }
        return ""
    }

    /// The relay identity of the physical machine a stable voice selection names.
    public static func statistics(_ selection: MachineKey, machines: [Machine]) -> MachineKey? {
        let matches = machines.filter { $0.connectionId == selection.connectionId && $0.machineId == selection.computerId }
        guard matches.count == 1 else { return nil }
        return MachineKey(connectionId: matches[0].connectionId, computerId: matches[0].id)
    }

    /// A selection saved by relay identity moves to the stable machine UUID once discovery proves it.
    public static func migratedSelection(_ selection: SpeechSelection, computers: [VoiceComputer], machines: [Machine]) -> SpeechSelection {
        guard let saved = selection.host else { return selection }
        if computers.contains(where: { $0.id == saved.computerId }) { return selection }
        let matches = machines.filter { $0.connectionId == saved.connectionId && $0.id == saved.computerId }
        guard matches.count == 1, !matches[0].machineId.isEmpty, computers.contains(where: { $0.id == matches[0].machineId }) else { return selection }
        var copy = selection
        copy.host = MachineKey(connectionId: saved.connectionId, computerId: matches[0].machineId)
        return copy
    }
}

public enum VoiceHistory {
    /// Native voice rows adopt this phone's private media copies; local rows fill in until history catches up.
    public static func merge(_ target: Target, native: [Event], outgoing: [VoiceOutgoing]) -> [Event] {
        let local = Dictionary(outgoing.filter { $0.belongs(to: target) }.map { ($0.id, $0) }, uniquingKeysWith: { first, _ in first })
        var best: [String: Int] = [:]
        for (index, event) in native.enumerated() {
            guard let voice = event.voice else { continue }
            if let current = best[voice.id], let held = native[current].voice {
                let candidate = (voice.revision, event.historyId.isEmpty ? 0 : 1), kept = (held.revision, native[current].historyId.isEmpty ? 0 : 1)
                if candidate > kept { best[voice.id] = index }
            } else {
                best[voice.id] = index
            }
        }
        var seen: Set<String> = []
        var rows: [Event] = []
        for (index, event) in native.enumerated() {
            guard let voice = event.voice else {
                rows.append(event)
                continue
            }
            guard best[voice.id] == index else { continue }
            seen.insert(voice.id)
            let own = local[voice.id]
            var latest = own?.message.map { VoiceProtocol.newer(voice, $0) } ?? voice
            if let audio = own?.audio {
                latest.audio.localId = audio.id
                latest.audio.sha256 = audio.sha256
            }
            if var remote = latest.video {
                if let localVideo = own?.video {
                    remote.file.localId = localVideo.file.id
                    remote.file.sha256 = localVideo.file.sha256
                }
                latest.video = remote
            } else if let localVideo = own?.video {
                latest.video = localVideo.displayed(voice.id)
            }
            var row = event
            row.id = "voice:" + voice.id
            row.text = latest.transcript
            row.voice = latest
            row.attachments = [latest.audio] + (latest.video.map { [$0.file] } ?? [])
            row.requestId = voice.id
            row.outgoingId = voice.id
            rows.append(row)
        }
        for own in outgoing where own.belongs(to: target) && !seen.contains(own.id) && local[own.id] == own {
            let voice = own.displayed()
            let event = Event(id: "voice:" + own.id, role: "You", text: voice.transcript, time: "", at: own.createdAt, requestId: own.id,
                              attachments: [voice.audio] + (voice.video.map { [$0.file] } ?? []), source: "hgs_voice", outgoingId: own.id,
                              nativeType: "UserMessage", voice: voice)
            if let position = rows.firstIndex(where: { $0.at > event.at }) { rows.insert(event, at: position) } else { rows.append(event) }
        }
        return rows
    }
}

// MARK: Capture

/// Telegram's short-press/hold/lock state machine; side effects remain with the exact capture owner.
public final class VoiceGesture {
    public enum End: Sendable, Equatable { case send, preview, cancel }
    public enum Action: Sendable, Equatable {
        case switchMode
        case start
        case lock(Int64)
        case finish(Int64, End)
    }

    let cancelDistance: Double
    let lockDistance: Double
    private var pressed = false
    private var captureId: Int64?
    private var ended = false
    private var held = false
    public private(set) var locked = false
    public private(set) var progress: Double = 0
    public private(set) var lockProgress: Double = 0

    public init(cancelDistance: Double, lockDistance: Double = 57) {
        precondition(cancelDistance.isFinite && cancelDistance > 0 && lockDistance.isFinite && lockDistance > 0)
        self.cancelDistance = cancelDistance
        self.lockDistance = lockDistance
    }

    public func begin() -> Bool {
        if pressed { return false }
        pressed = true
        captureId = nil
        ended = false
        held = false
        locked = false
        progress = 0
        lockProgress = 0
        return true
    }

    public func captured(_ id: Int64?) { if pressed && !ended { captureId = id } }

    public func hold() -> Action? {
        guard pressed, !ended, !held else { return nil }
        held = true
        return .start
    }

    public func move(horizontal: Double, vertical: Double = 0) -> Action? {
        guard pressed, !ended, !locked, horizontal.isFinite, vertical.isFinite else { return nil }
        progress = min(max(-horizontal / cancelDistance, 0), 1)
        lockProgress = min(max(-vertical / lockDistance, 0), 1)
        if !held && (progress > 0.15 || lockProgress > 0.15) {
            ended = true
            return nil
        }
        if let captureId, lockProgress >= 1 {
            locked = true
            return .lock(captureId)
        }
        return progress >= 1 ? finish(.cancel) : nil
    }

    public func release() -> Action? {
        let result: Action?
        if locked { result = nil } else if pressed && !held && !ended { result = .switchMode } else { result = finish(progress >= 0.55 ? .cancel : .send) }
        pressed = false
        return result
    }

    /// A lost touch never switches mode or submits; an already locked capture keeps its own controls.
    public func interrupt() -> Action? {
        let result = locked ? nil : finish(.preview)
        pressed = false
        return result
    }

    private func finish(_ end: End) -> Action? {
        guard pressed, !ended else { return nil }
        ended = true
        return captureId.map { .finish($0, end) }
    }
}

/// One microphone capture owns its feedback, including delayed recorder callbacks.
public final class VoiceCaptureFeedback: @unchecked Sendable {
    public enum Event: Sendable, Equatable { case start, finish, cancel }
    private let emit: (Event) -> Void
    private var started = false
    private var ended = false
    private var released = false
    private var terminal: Event?

    public init(_ emit: @escaping (Event) -> Void = { _ in }) { self.emit = emit }

    public func didStart() {
        if started || ended { return }
        started = true
        emit(.start)
    }

    public func finish(userInitiated: Bool, cancelled: Bool = false) {
        if released { return }
        if !userInitiated {
            ended = true
            terminal = nil
            return
        }
        if ended { return }
        ended = true
        if started { terminal = cancelled ? .cancel : .finish }
    }

    /// Called only after the microphone has stopped, before saving the preview.
    public func stopped() {
        if released { return }
        released = true
        ended = true
        if let terminal { emit(terminal) }
        terminal = nil
    }
}

/// Based on captured PCM duration, so initialization never invents elapsed recording time.
public func voiceRecordingTime(_ elapsedMs: Int) -> String {
    let tenths = max(elapsedMs, 0) / 100
    let seconds = tenths / 10
    return "\(seconds / 60):" + String(format: "%02d", seconds % 60) + ".\(tenths % 10)"
}

extension VoiceWaveform {
    public static func seekPosition(_ fraction: Double, durationMs: Int) -> Int? {
        guard fraction.isFinite, (1...120_000).contains(durationMs) else { return nil }
        return Int((min(max(fraction, 0), 1) * Double(durationMs)).rounded())
    }

    public static func progress(_ positionMs: Int, durationMs: Int) -> Double {
        durationMs <= 0 ? 0 : min(max(Double(positionMs) / Double(durationMs), 0), 1)
    }

    public static func time(_ milliseconds: Int) -> String {
        let seconds = max(milliseconds, 0) / 1000
        return "\(seconds / 60):" + String(format: "%02d", seconds % 60)
    }
}

public enum VideoRecordingClock {
    public static let limitMs = 59_000
    public static func remainingSeconds(_ elapsedMs: Int) -> Int { (limitMs - min(max(elapsedMs, 0), limitMs) + 999) / 1000 }
    public static func progress(_ elapsedMs: Int) -> Double { Double(min(max(elapsedMs, 0), limitMs)) / Double(limitMs) }
}

/// Streaming mono 16 kHz conversion with a bounded box filter for any capture or AAC rate.
public struct MediaPcm16Converter: Sendable {
    let rate: Int
    let channels: Int
    let limit: Int
    private var output = Data()
    private var channel = 0
    private var channelSum: Int64 = 0
    private var phase = 0
    private var sum: Int64 = 0
    private var frames = 0

    public init(rate: Int, channels: Int, limit: Int = 1_920_000) {
        precondition((8_000...192_000).contains(rate) && (1...8).contains(channels))
        self.rate = rate
        self.channels = channels
        self.limit = limit - limit % 2
    }

    public var full: Bool { output.count >= limit }
    public var count: Int { output.count }
    public var bytes: Data { output }

    public mutating func sample(_ value: Int) {
        if full { return }
        channelSum += Int64(min(max(value, -32_768), 32_767))
        channel += 1
        if channel != channels { return }
        sum += channelSum / Int64(channels)
        frames += 1
        phase += 16_000
        channel = 0
        channelSum = 0
        while phase >= rate && !full {
            let sample = Int16(truncatingIfNeeded: sum / Int64(frames))
            output.append(UInt8(truncatingIfNeeded: sample))
            output.append(UInt8(truncatingIfNeeded: sample >> 8))
            phase -= rate
        }
        if phase < 16_000 {
            sum = 0
            frames = 0
        }
    }
}
