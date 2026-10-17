import Foundation
import Observation
import ZerusKit

/// The phone owns capture and one explicit upload; the conversation computer owns delivery.
/// A stopped recording is a durable preview; nothing is uploaded before Send.
@MainActor
@Observable
final class VoiceController {
    @ObservationIgnored private unowned let app: AppModel
    @ObservationIgnored private var store: SealedFileStore?
    @ObservationIgnored private var saving: Task<Void, Error>?

    private(set) var value = VoiceState()
    var selection: SpeechSelection { value.selection }
    var videoMode: Bool { value.videoMode }
    var previewBeforeSend: Bool { value.previewBeforeSend }
    var draft: VoiceDraft? { value.draft }
    var outgoing: [VoiceOutgoing] { value.outgoing }

    private(set) var ready = false
    private(set) var computers: [VoiceComputer] = []
    var profiles: [SpeechProfile] { computers.first { $0.id == selection.host?.computerId }?.profiles ?? [] }
    var profile: SpeechProfile? { profiles.first { $0.id == selection.profileId } }
    private(set) var loading = false
    private(set) var profileNotice = ""
    private(set) var profileTarget: Target?
    private(set) var recording = false
    private(set) var savingRecording = false
    private(set) var elapsedMs = 0
    var notice = ""
    private(set) var statistics = ""
    private(set) var activeRequests: Set<String> = []
    var busy: Bool { recording || savingRecording || !activeRequests.isEmpty }

    private final class Capture {
        let id: Int64
        let recorder: CaptureDevice
        let feedback: VoiceCaptureFeedback
        let gesture: Bool
        let target: Target
        let machine: String
        let profile: String
        let revision: Int64?
        let previewBeforeSend: Bool
        var discarded = false
        var stopping = false
        var sendWhenSaved = false
        var finishNotice = ""

        init(id: Int64, recorder: CaptureDevice, feedback: VoiceCaptureFeedback, gesture: Bool, target: Target, machine: String, profile: String,
             revision: Int64?, previewBeforeSend: Bool) {
            self.id = id
            self.recorder = recorder
            self.feedback = feedback
            self.gesture = gesture
            self.target = target
            self.machine = machine
            self.profile = profile
            self.revision = revision
            self.previewBeforeSend = previewBeforeSend
        }
    }

    @ObservationIgnored private var capture: Capture?
    @ObservationIgnored private var captureSequence: Int64 = 0
    private(set) var activeCaptureId: Int64?
    private(set) var captureNeedsPreview = false
    private(set) var recordingLocked = false
    private(set) var videoRecorder: RoundVideoRecorder?
    private(set) var gestureCaptureId: Int64?
    private(set) var gestureCancelProgress: Double = 0
    private(set) var gestureLockProgress: Double = 0
    private(set) var captureFailure: (id: Int64, message: String)?
    var touchRecording: Bool { recording && gestureCaptureId != nil && !recordingLocked }

    @ObservationIgnored private var profileTask: Task<Void, Never>?
    @ObservationIgnored private var profileGeneration = 0
    @ObservationIgnored private var requestedProfileTarget: Target?
    @ObservationIgnored private var statsTask: Task<Void, Never>?
    @ObservationIgnored private var statusTasks: [String: Task<Void, Never>] = [:]

    init(app: AppModel) { self.app = app }

    // MARK: Storage

    func open(_ sealed: SealedFileStore) async {
        store = sealed
        do {
            value = try await Task.detached { try (sealed.read("voice_state", as: VoiceState.self) ?? VoiceState()).recovered() }.value
            ready = true
        } catch {
            notice = "Private voice storage could not be opened. Recording is unavailable."
        }
    }

    /// Writes are serialized in order; each writes the snapshot it captured.
    private func persist() async throws {
        guard let store else { throw ZerusError.invalid("Private storage is not ready.") }
        let snapshot = value, previous = saving
        let task = Task.detached(priority: .userInitiated) {
            _ = await previous?.result
            try store.write("voice_state", snapshot)
        }
        saving = task
        try await task.value
    }

    private func update(_ id: String, _ change: (inout VoiceOutgoing) -> Void) {
        guard let index = value.outgoing.firstIndex(where: { $0.id == id }) else { return }
        change(&value.outgoing[index])
    }

    var protectedIds: Set<String> { value.protectedIds }

    private func remove(_ files: [Attachment]) async {
        guard let attachments = app.attachmentStore, !files.isEmpty else { return }
        let protected = app.conversation.protectedAttachmentIds()
        await Task.detached(priority: .utility) { for file in files where !protected.contains(file.id) { _ = try? attachments.remove(file.id, protected: protected) } }.value
    }

    // MARK: Providers

    private func choose(_ next: SpeechSelection, _ target: Target, previewId: String?) -> Bool {
        guard ready, !recording, !savingRecording, !loading, profileTarget == target else { return false }
        if let previewId {
            guard var captured = draft, captured.audio.id == previewId, captured.target == target else { return false }
            captured.machineId = next.host?.computerId ?? ""
            captured.profileId = next.profileId
            captured.profileRevision = next.profileRevision
            value.draft = captured
        }
        value.selection = next
        return true
    }

    private func choiceSelection(_ target: Target, previewId: String?) -> SpeechSelection? {
        guard let previewId else { return selection }
        return draft.flatMap { $0.audio.id == previewId && $0.target == target ? $0.selection : nil }
    }

    func chooseHost(_ host: MachineKey, target: Target, previewId: String? = nil) {
        guard host.connectionId == target.connectionId, computers.contains(where: { $0.id == host.computerId }),
              let current = choiceSelection(target, previewId: previewId) else { return }
        let same = current.host?.computerId == host.computerId
        guard choose(SpeechSelection(host: host, profileId: same ? current.profileId : "", profileRevision: same ? current.profileRevision : nil), target,
                     previewId: previewId) else { return }
        Task {
            do {
                try await persist()
                refreshStatistics()
            } catch { notice = "Could not save the voice computer." }
        }
    }

    func chooseProfile(_ id: String, target: Target, previewId: String? = nil) {
        guard let current = choiceSelection(target, previewId: previewId), let computer = computers.first(where: { $0.id == current.host?.computerId }),
              let chosen = computer.profiles.first(where: { $0.id == id }) else { return }
        var next = current
        next.host?.connectionId = target.connectionId
        next.profileId = id
        next.profileRevision = chosen.profileRevision
        guard choose(next, target, previewId: previewId) else { return }
        Task { do { try await persist() } catch { notice = "Could not save the voice profile." } }
    }

    /// Retained recordings keep their independently pinned route.
    func chooseDefault(_ next: SpeechSelection) -> Bool {
        guard ready, !recording, !savingRecording else { return false }
        value.selection = next
        Task {
            do {
                try await persist()
                refreshStatistics()
            } catch { notice = "Could not save the default voice provider." }
        }
        return true
    }

    func refreshProfiles(_ target: Target?) {
        guard ready, !(loading && requestedProfileTarget == target) else { return }
        profileGeneration += 1
        let generation = profileGeneration
        profileTask?.cancel()
        loading = false
        requestedProfileTarget = target
        profileTarget = nil
        guard let target, target.archiveId.isEmpty, target.agentId.isEmpty else {
            computers = []
            profileNotice = "Open a conversation to see the voice computers it can reach. Your selection is saved."
            return
        }
        let reason = app.conversation.voiceHostReason(target)
        guard reason.isEmpty, let connection = app.connections.first(where: { $0.id == target.connectionId }) else {
            computers = []
            profileNotice = reason
            return
        }
        loading = true
        profileNotice = ""
        let client = app.client
        profileTask = Task { [weak self] in
            do {
                let id = UUID().uuidString.lowercased()
                let received = try VoiceProtocol.computers(try await client.voice(connection, target: target, operation: "voice_profiles",
                                                                                  payload: VoiceProtocol.payload(target, requestId: id), id: id))
                guard let self, requestedProfileTarget == target, profileGeneration == generation else { return }
                computers = received
                profileTarget = target
                loading = false
                profileNotice = received.isEmpty ? "No voice profiles are available to this conversation computer." : ""
                let migrated = VoiceRoutes.migratedSelection(selection, computers: received, machines: app.machines)
                if migrated != selection {
                    value.selection = migrated
                    try? await persist()
                }
                refreshStatistics()
            } catch {
                guard let self, requestedProfileTarget == target, profileGeneration == generation else { return }
                loading = false
                if error is CancellationError { return }
                computers = []
                profileTarget = nil
                profileNotice = AppModel.message(error, fallback: "Voice profiles are unavailable.")
            }
        }
    }

    private func refreshStatistics() {
        statsTask?.cancel()
        statistics = ""
        guard let selected = selection.host else { return }
        guard let host = VoiceRoutes.statistics(selected, machines: app.machines) else {
            statistics = "Local voice statistics are unavailable through this gateway."
            return
        }
        guard app.relayOperations[host.connectionId]?.contains("speech_stats") == true, app.machineOperations[host]?.contains("speech_stats") == true,
              let connection = app.connections.first(where: { $0.id == host.connectionId }) else { return }
        let client = app.client
        statsTask = Task { [weak self] in
            let stats = try? await client.speech(connection, host: host, operation: "speech_stats", payload: [:], fallback: "Unavailable")
            guard let self, !Task.isCancelled, selection.host == selected else { return }
            guard let stats, stats.int("schema") == 1 else {
                statistics = "Local voice statistics are unavailable."
                return
            }
            let totals = stats.object("totals") ?? [:]
            statistics = !stats.bool("enabled") ? "Local voice statistics are disabled on this computer."
                : "Last \(stats.has("retention_days") ? stats.int("retention_days") : 90) days: \(totals.int64("completed")) transcribed, \(totals.int64("failed")) failed, \(totals.int64("audio_ms") / 60_000) audio minutes."
        }
    }

    // MARK: Capture

    func startReason(_ target: Target) -> String {
        if !ready || !app.storageReady || app.attachmentStore == nil { return "Private storage is not ready." }
        if app.demo { return "Open a connected conversation to record a voice message." }
        if recording || savingRecording || draft != nil { return "Send or discard the saved recording first." }
        if app.conversation.sendingBlocked(target) { return "Finish the pending message or session action before recording." }
        if !target.archiveId.isEmpty || !target.agentId.isEmpty { return "Voice messages are available in live parent conversations." }
        if target.run.isEmpty || target.conversation.isEmpty { return "Wait for the conversation to be ready before recording." }
        let stored = outgoing.reduce(Int64(0)) { $0 + ($1.audio?.bytes ?? 0) + ($1.video?.file.bytes ?? 0) }
        if stored > 100 * 1024 * 1024 - Int64(VoiceProtocol.maxPcmBytes) - 44 - (videoMode ? Int64(VideoProtocol.maxBytes) : 0) {
            return "Remove an old local voice copy in Drafts to make room for another recording."
        }
        if videoMode && !RoundVideoRecorder.available { return "No camera is available on this device." }
        if app.conversation.selected?.target != target { return "Open the conversation first." }
        return ""
    }

    func permissionDenied() {
        notice = videoMode ? "Camera or microphone access is off. Allow both in Settings to record video messages."
            : "Microphone access was denied. Allow microphone access in Settings to record."
    }

    func toggleMode(_ target: Target) {
        guard ready, !recording, !savingRecording, draft == nil, app.conversation.selected?.target == target else { return }
        value.videoMode.toggle()
        Task { do { try await persist() } catch { notice = "Could not save the recording mode." } }
    }

    func setPreviewBeforeSend(_ enabled: Bool) {
        guard ready, !recording, !savingRecording, enabled != value.previewBeforeSend else { return }
        value.previewBeforeSend = enabled
        Task { do { try await persist() } catch { notice = "Could not save the recording preview preference." } }
    }

    /// Starts one owned capture. A gesture capture is held; otherwise it is locked from the start.
    func start(_ target: Target, gesture: Bool, feedback: @escaping (VoiceCaptureFeedback.Event) -> Void = { _ in }) -> Int64? {
        let reason = startReason(target)
        guard reason.isEmpty else {
            notice = reason
            return nil
        }
        let device: CaptureDevice = videoMode ? RoundVideoRecorder() : AudioCaptureDevice()
        let machine = selection.host?.computerId ?? ""
        let revision = profileTarget == target && !loading && profile != nil ? profile?.profileRevision : selection.profileRevision
        app.playback.stop()
        elapsedMs = 0
        notice = ""
        captureFailure = nil
        recording = true
        captureSequence += 1
        let owned = Capture(id: captureSequence, recorder: device, feedback: VoiceCaptureFeedback(feedback), gesture: gesture, target: target, machine: machine,
                            profile: selection.profileId, revision: revision, previewBeforeSend: previewBeforeSend)
        capture = owned
        activeCaptureId = owned.id
        captureNeedsPreview = owned.previewBeforeSend
        recordingLocked = !gesture
        videoRecorder = device as? RoundVideoRecorder
        gestureCaptureId = gesture ? owned.id : nil
        gestureCancelProgress = 0
        gestureLockProgress = 0
        Task { await run(owned) }
        refreshProfiles(target)
        return owned.id
    }

    private func run(_ owned: Capture) async {
        var imported: [Attachment] = []
        var sendSaved = false
        do {
            let media = try await owned.recorder.capture(started: { [weak self] in
                if self?.capture === owned && self?.recording == true { owned.feedback.didStart() }
            }, progress: { [weak self] elapsed in
                if self?.capture === owned { self?.elapsedMs = elapsed }
            })
            // Natural completion and the duration limit retire this touch press before saving.
            owned.feedback.stopped()
            retireGesture(owned.id)
            recording = false
            if owned.discarded {
                notice = "Recording discarded."
            } else {
                guard media.pcm.count >= 3_200 else { throw ZerusError.invalid("The recording was too short. Try again.") }
                guard let attachments = app.attachmentStore else { throw ZerusError.invalid("Private storage is not ready.") }
                savingRecording = true
                let wav = try VoiceProtocol.wav(media.pcm)
                let audio = try await Task.detached(priority: .userInitiated) { try attachments.importData(wav, name: "voice.wav", mime: "audio/wav", current: []) }.value
                imported.append(audio)
                var video: VideoDraft?
                if let captured = media.video {
                    let file = try await Task.detached(priority: .userInitiated) {
                        try attachments.importData(captured.bytes, name: "video.mp4", mime: "video/mp4", current: [])
                    }.value
                    imported.append(file)
                    video = VideoDraft(file: file, durationMs: captured.durationMs, width: captured.width, height: captured.height, posterBase64: captured.posterBase64)
                }
                value.draft = VoiceDraft(target: owned.target, machineId: owned.machine, profileId: owned.profile, audio: audio, durationMs: media.pcm.count / 32,
                                         createdAt: Date().timeIntervalSince1970, profileRevision: owned.revision, video: video)
                try await persist()
                imported.removeAll()
                notice = owned.finishNotice
                sendSaved = owned.sendWhenSaved
            }
        } catch {
            notice = owned.discarded ? "Recording discarded." : AppModel.message(error, fallback: "Could not save the recording.")
            if !owned.discarded { captureFailure = (owned.id, notice) }
        }
        owned.feedback.stopped()
        if capture === owned {
            retireGesture(owned.id)
            gestureCancelProgress = 0
            gestureLockProgress = 0
            recording = false
            recordingLocked = false
            videoRecorder = nil
            savingRecording = false
            capture = nil
            activeCaptureId = nil
        }
        // A committed preview remains recoverable; only unpublished imports are removed.
        if !imported.isEmpty {
            let protected = protectedIds
            await remove(imported.filter { !protected.contains($0.id) })
        }
        if sendSaved { submit() }
    }

    private func retireGesture(_ id: Int64) { if gestureCaptureId == id { gestureCaptureId = nil } }

    func moveGesture(_ id: Int64, progress: Double, lockProgress: Double = 0) {
        guard gestureCaptureId == id, capture?.id == id else { return }
        gestureCancelProgress = min(max(progress, 0), 1)
        gestureLockProgress = min(max(lockProgress, 0), 1)
    }

    func lockCapture(_ id: Int64) {
        guard capture?.id == id, gestureCaptureId == id, recording else { return }
        retireGesture(id)
        recordingLocked = true
        gestureCancelProgress = 0
        gestureLockProgress = 0
    }

    func sendCapture(_ id: Int64) {
        guard let owned = capture, owned.id == id, recording, !owned.discarded, !owned.stopping else { return }
        // Only the retained preview's separate Send can authorize delivery in preview mode.
        if owned.previewBeforeSend {
            stopCapture(id)
            return
        }
        let chosen = computers.first { $0.id == owned.machine }?.profiles.first { $0.id == owned.profile }
        // The explicit send gesture cannot be revived by a later discovery result.
        owned.sendWhenSaved = profileTarget == owned.target && !loading && chosen?.available == true && chosen?.profileRevision == owned.revision
        if !owned.sendWhenSaved { owned.finishNotice = "Confirm a voice provider before sending." }
        stopCapture(id)
    }

    @discardableResult
    func finishGesture(_ id: Int64, cancel: Bool, userInitiated: Bool = true) -> Bool {
        guard let owned = capture, owned.id == id, gestureCaptureId == id else { return false }
        retireGesture(id)
        owned.stopping = true
        if cancel { owned.discarded = true }
        owned.feedback.finish(userInitiated: userInitiated, cancelled: cancel)
        if cancel { owned.recorder.cancel() } else { owned.recorder.stop() }
        return true
    }

    func stopCapture(_ id: Int64, userInitiated: Bool = true) {
        guard let owned = capture, owned.id == id else { return }
        retireGesture(id)
        owned.stopping = true
        owned.feedback.finish(userInitiated: userInitiated, cancelled: owned.discarded)
        if owned.discarded { owned.recorder.cancel() } else { owned.recorder.stop() }
    }

    func cancelCapture(_ id: Int64) {
        guard let owned = capture, owned.id == id, recording else { return }
        owned.discarded = true
        owned.sendWhenSaved = false
        stopCapture(id)
    }

    /// Leaving the app keeps what was recorded as a preview; it never sends.
    func background() {
        guard let owned = capture else { return }
        owned.sendWhenSaved = false
        stopCapture(owned.id, userInitiated: false)
    }

    func discardRecording() {
        if recording {
            if let id = activeCaptureId { cancelCapture(id) }
            return
        }
        guard let captured = draft, !savingRecording else { return }
        savingRecording = true
        Task {
            do {
                value.draft = nil
                try await persist()
                await remove([captured.audio] + (captured.video.map { [$0.file] } ?? []))
                notice = "Recording discarded."
            } catch {
                value.draft = captured
                notice = "Could not discard the saved recording."
            }
            savingRecording = false
        }
    }

    // MARK: Sending

    func providerReason(_ captured: VoiceDraft) -> String {
        VoiceRoutes.providerReason(captured, confirmedTarget: profileTarget, loading: loading, computers: computers)
    }

    func sendReason() -> String {
        guard let captured = draft else { return "Record a voice message first." }
        if savingRecording { return "Saving the recording…" }
        if outgoing.count >= 100 { return "Remove an old local voice receipt in Drafts before sending another." }
        if outgoing.contains(where: { $0.belongs(to: captured.target) && !$0.terminal }) { return "Wait for the previous voice message to finish." }
        if outgoing.contains(where: { $0.belongs(to: captured.target) && $0.state == "uncertain" }) { return "Check the previous voice message before sending another." }
        let conversation = app.conversation.voiceSendReason(captured.target)
        if !conversation.isEmpty { return conversation }
        if captured.video != nil {
            let video = app.conversation.videoHostReason(captured.target)
            if !video.isEmpty { return video }
        }
        return providerReason(captured)
    }

    private func validateSend(_ captured: VoiceDraft, allowColdCache: Bool) throws {
        guard app.conversation.voiceSendReason(captured.target).isEmpty else {
            throw ZerusError.invalid("The conversation changed before sending. The recording remains saved.")
        }
        let provider = providerReason(captured)
        guard provider.isEmpty else { throw ZerusError.invalid(provider) }
        if captured.video != nil, let reason = app.conversation.videoHostReason(captured.target).nonEmpty { throw ZerusError.invalid(reason) }
        guard allowColdCache || !app.conversation.voiceColdCache() else {
            throw ZerusError.invalid("The prompt cache expired. The recording remains saved without sending.")
        }
    }

    func submit(allowColdCache: Bool = false) {
        guard let captured = draft else { return }
        let reason = sendReason()
        guard reason.isEmpty else {
            notice = reason
            return
        }
        if app.conversation.voiceColdCache() && !allowColdCache {
            notice = "Confirm sending with full context first."
            return
        }
        guard let connection = app.connections.first(where: { $0.id == captured.target.connectionId }), let attachments = app.attachmentStore else { return }
        let id = UUID().uuidString.lowercased()
        let pending = VoiceOutgoing(id: id, target: captured.target, machineId: captured.machineId, profileId: captured.profileId, audio: captured.audio,
                                    durationMs: captured.durationMs, createdAt: captured.createdAt, profileRevision: captured.profileRevision, video: captured.video)
        savingRecording = true
        activeRequests.insert(id)
        let client = app.client
        Task {
            var attempted = false
            do {
                value.draft = nil
                value.outgoing.append(pending)
                try await persist() // The UUID and immutable media are durable before the only upload.
                savingRecording = false
                app.conversation.voiceSent(captured.target, id)
                try validateSend(captured, allowColdCache: allowColdCache)
                let payload = try await Task.detached(priority: .userInitiated) { () throws -> JSONObject in
                    var payload = VoiceProtocol.payload(captured.target, requestId: id)
                    payload["speech"] = .object(VoiceProtocol.provider(captured))
                    let pcm = try VoiceProtocol.pcm(attachments.decrypt(captured.audio))
                    payload["audio"] = ["format": "pcm_s16le", "sample_rate": 16_000, "channels": 1, "data_base64": .string(pcm.base64EncodedString())]
                    if let video = captured.video { payload["video"] = .object(try VideoProtocol.wire(video, attachments.decrypt(video.file))) }
                    return payload
                }.value
                try validateSend(captured, allowColdCache: allowColdCache)
                attempted = true
                try await accept(pending, client.voice(connection, target: captured.target, operation: "voice_submit", payload: payload, id: id))
            } catch {
                update(id) { entry in
                    entry.uploadState = attempted ? "uncertain" : "failed"
                    entry.error = attempted ? "Sending was not confirmed. Check this message's status; it has not been resent."
                        : AppModel.message(error, fallback: "The recording was not sent.")
                }
                do { try await persist() } catch { notice = "Could not save the outcome. The original voice request remains recoverable." }
            }
            savingRecording = false
            activeRequests.remove(id)
            if outgoing.first(where: { $0.id == id })?.needsStatus == true { poll(id) }
        }
    }

    private func accept(_ previous: VoiceOutgoing, _ raw: JSONObject) async throws {
        let next = try VoiceProtocol.response(raw, target: previous.effectiveTarget, id: previous.id)
        if let audio = previous.audio, next.audio.bytes != audio.bytes || next.durationMs != previous.durationMs {
            throw ZerusError.mismatch("The computer returned different audio metadata.")
        }
        if let local = previous.video {
            guard let remote = next.video, remote.file.bytes == local.file.bytes, remote.durationMs == local.durationMs, remote.width == local.width,
                  remote.height == local.height else { throw ZerusError.mismatch("The computer returned different video metadata.") }
        }
        update(previous.id) { entry in
            entry.message = VoiceProtocol.newer(entry.message, next)
            entry.error = ""
        }
        try await persist()
    }

    private func status(_ entry: VoiceOutgoing) async throws {
        let target = entry.effectiveTarget
        guard app.conversation.voiceHostReason(target).isEmpty, let connection = app.connections.first(where: { $0.id == target.connectionId }) else {
            throw ZerusError.invalid(app.conversation.voiceHostReason(target).nonEmpty ?? "The voice workspace is disconnected.")
        }
        let id = UUID().uuidString.lowercased()
        let payload = VoiceProtocol.payload(target, requestId: id).putting("message_id", .string(entry.id))
        try await accept(entry, app.client.voice(connection, target: target, operation: "voice_status", payload: payload, id: id))
    }

    func checkStatus(_ entry: VoiceOutgoing) { poll(entry.id, force: true) }

    private func poll(_ id: String, force: Bool = false) {
        guard statusTasks[id] == nil, !activeRequests.contains(id) else { return }
        activeRequests.insert(id)
        statusTasks[id] = Task { [weak self] in
            guard let self else { return }
            defer {
                activeRequests.remove(id)
                statusTasks[id] = nil
            }
            do {
                for index in 0..<300 {
                    guard let entry = outgoing.first(where: { $0.id == id }), entry.needsStatus || (force && index == 0) else { return }
                    if index > 0 { try await Task.sleep(for: .milliseconds(1_500)) }
                    try await status(entry)
                }
            } catch is CancellationError {
            } catch {
                update(id) { $0.error = AppModel.message(error, fallback: "Reconnect to check this voice message. No message has been resent.") }
                do { try await persist() } catch { notice = "Could not save voice status." }
            }
        }
    }

    /// Reconnecting only reads. In-progress rows from other devices use the same status path.
    func observe(_ target: Target, events: [Event]) {
        guard ready, target.archiveId.isEmpty, target.agentId.isEmpty else { return }
        var changed = false
        for voice in events.compactMap(\.voice) where voice.runId == target.run && voice.conversationId == target.conversation {
            if let previous = outgoing.first(where: { $0.id == voice.id && $0.belongs(to: target) }) {
                let next = VoiceProtocol.newer(previous.message, voice)
                if next != previous.message {
                    update(voice.id) { $0.message = next }
                    changed = true
                }
            } else if !voice.terminal && outgoing.count < 100 {
                value.outgoing.append(VoiceOutgoing(id: voice.id, target: target, machineId: "", profileId: "", audio: nil, durationMs: voice.durationMs,
                                                    createdAt: voice.createdAt, message: voice))
                changed = true
            }
        }
        if changed { Task { do { try await persist() } catch { notice = "Could not save voice status." } } }
        for entry in outgoing where entry.belongs(to: target) && entry.needsStatus { poll(entry.id) }
    }

    func cancel(_ entry: VoiceOutgoing) {
        guard !["submitted", "failed", "cancelled"].contains(entry.state) else { return }
        let previous = statusTasks.removeValue(forKey: entry.id)
        previous?.cancel()
        Task {
            await previous?.value
            guard !activeRequests.contains(entry.id) else { return }
            activeRequests.insert(entry.id)
            defer { activeRequests.remove(entry.id) }
            do {
                let target = entry.effectiveTarget
                guard let connection = app.connections.first(where: { $0.id == target.connectionId }), app.conversation.voiceHostReason(target).isEmpty else {
                    throw ZerusError.invalid(app.conversation.voiceHostReason(target).nonEmpty ?? "The voice workspace is disconnected.")
                }
                let id = UUID().uuidString.lowercased()
                let payload = VoiceProtocol.payload(target, requestId: id).putting("message_id", .string(entry.id))
                try await accept(entry, app.client.voice(connection, target: target, operation: "voice_cancel", payload: payload, id: id))
            } catch {
                update(entry.id) { $0.error = AppModel.message(error, fallback: "Cancellation was not confirmed. Check status.") }
                try? await persist()
            }
        }
    }

    /// Removes this phone's receipt and media; the conversation keeps its own copy.
    func dismiss(_ entry: VoiceOutgoing) {
        guard entry.terminal, !activeRequests.contains(entry.id) else { return }
        Task {
            do {
                value.outgoing.removeAll { $0.id == entry.id }
                try await persist()
                await remove([entry.audio, entry.video?.file].compactMap { $0 })
            } catch { notice = "Could not remove the local voice receipt." }
        }
    }

    /// Only a locally proven pre-transport failure may become a new editable recording.
    func restoreUnsent(_ entry: VoiceOutgoing) {
        guard let current = outgoing.first(where: { $0.id == entry.id }), current == entry, current.canRestoreUnsent, let audio = current.audio,
              !activeRequests.contains(current.id), draft == nil, !recording, !savingRecording else { return }
        let restored = VoiceDraft(target: current.target, machineId: current.machineId, profileId: current.profileId, audio: audio, durationMs: current.durationMs,
                                  createdAt: current.createdAt, profileRevision: current.profileRevision, video: current.video)
        savingRecording = true
        Task {
            do {
                value.draft = restored
                value.outgoing.removeAll { $0.id == current.id }
                try await persist()
                notice = "The unsent recording is ready. Open its original conversation before sending."
            } catch {
                notice = "Could not restore the recording. Its saved receipt remains recoverable after restart."
            }
            savingRecording = false
        }
    }

    /// Removes every voice receipt for a disconnected workspace with its local media.
    func forget(connection: String) async {
        let removed = outgoing.filter { $0.target.connectionId == connection }
        let preview = draft?.target.connectionId == connection ? draft : nil
        guard !removed.isEmpty || preview != nil else { return }
        removed.forEach { statusTasks.removeValue(forKey: $0.id)?.cancel() }
        value.outgoing.removeAll { $0.target.connectionId == connection }
        if preview != nil { value.draft = nil }
        if selection.host?.connectionId == connection { value.selection = SpeechSelection() }
        try? await persist()
        await remove(removed.flatMap { [$0.audio, $0.video?.file].compactMap { $0 } } + (preview.map { [$0.audio] + ($0.video.map { [$0.file] } ?? []) } ?? []))
    }
}
