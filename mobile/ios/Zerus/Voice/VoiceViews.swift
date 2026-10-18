import AVFoundation
import SwiftUI
import UIKit
import ZerusKit

// MARK: Playback

/// Play/pause, a seekable waveform drawn only from measured bins, and the time.
struct VoicePlayerView: View {
    @Environment(AppModel.self) private var app
    let target: Target?
    let audio: DisplayedAttachment
    let durationMs: Int
    var waveform: [Int] = []
    var status: String?
    @State private var seeking: Double?

    var body: some View {
        let playback = app.playback!
        let key = playback.key(target, audio)
        let active = playback.playingKey == key
        let loading = playback.loadingKey == key
        let position = active ? playback.positionMs : 0
        let fraction = seeking ?? VoiceWaveform.progress(position, durationMs: durationMs)
        let shown = seeking.flatMap { VoiceWaveform.seekPosition($0, durationMs: durationMs) } ?? position
        VStack(alignment: .leading, spacing: 2) {
            HStack(spacing: 10) {
                Button { playback.toggle(target, audio) } label: {
                    ZStack {
                        Circle().fill(Theme.mint.opacity(0.16))
                        if loading { ProgressView().controlSize(.small) } else {
                            Image(systemName: active && playback.playing ? "pause.fill" : "play.fill").foregroundStyle(Theme.mint)
                        }
                    }
                    .frame(width: 40, height: 40)
                }
                .buttonStyle(.plain)
                .disabled(loading)
                .accessibilityLabel(active && playback.playing ? "Pause" : "Play")
                WaveformTrack(bins: VoiceWaveform.valid(waveform) ? waveform : [], fraction: fraction)
                    .frame(height: 30)
                    .contentShape(Rectangle())
                    .overlay {
                        GeometryReader { proxy in
                            Color.clear.contentShape(Rectangle()).gesture(DragGesture(minimumDistance: 0)
                                .onChanged { seeking = min(max($0.location.x / max(proxy.size.width, 1), 0), 1) }
                                .onEnded { value in
                                    let target = min(max(value.location.x / max(proxy.size.width, 1), 0), 1)
                                    seeking = nil
                                    if active { playback.seek(self.target, audio, fraction: target, durationMs: durationMs) }
                                })
                        }
                    }
                    .accessibilityElement()
                    .accessibilityLabel("Recording playback position")
                    .accessibilityValue("\(VoiceWaveform.time(shown)) of \(VoiceWaveform.time(durationMs))")
                    .accessibilityAdjustableAction { direction in
                        guard active else { return }
                        let step = direction == .increment ? 0.1 : -0.1
                        playback.seek(target, audio, fraction: VoiceWaveform.progress(position, durationMs: durationMs) + step, durationMs: durationMs)
                    }
            }
            HStack {
                if let status { Text(status).font(.caption).foregroundStyle(Theme.muted) }
                Spacer()
                Text(active || shown > 0 ? "\(VoiceWaveform.time(shown)) / \(VoiceWaveform.time(durationMs))" : VoiceWaveform.time(durationMs))
                    .font(.caption.monospacedDigit())
                    .foregroundStyle(Theme.muted)
            }
            if let failure = playback.failure, failure.key == key { Text(failure.message).font(.caption).foregroundStyle(Theme.amber) }
        }
    }
}

struct WaveformTrack: View {
    let bins: [Int]
    let fraction: Double

    var body: some View {
        Canvas { context, size in
            func draw(_ color: Color) {
                if bins.isEmpty {
                    var line = Path()
                    line.move(to: CGPoint(x: 0, y: size.height / 2))
                    line.addLine(to: CGPoint(x: size.width, y: size.height / 2))
                    context.stroke(line, with: .color(color), style: StrokeStyle(lineWidth: 2, lineCap: .round))
                    return
                }
                let step = size.width / CGFloat(bins.count), stroke = min(step * 0.55, 3), bottom = size.height - stroke / 2
                for (index, bin) in bins.enumerated() {
                    let height = max(CGFloat(bin) / 255 * (size.height - stroke), stroke)
                    let x = (CGFloat(index) + 0.5) * step
                    var bar = Path()
                    bar.move(to: CGPoint(x: x, y: bottom))
                    bar.addLine(to: CGPoint(x: x, y: bottom - height))
                    context.stroke(bar, with: .color(color), style: StrokeStyle(lineWidth: stroke, lineCap: .round))
                }
            }
            draw(Theme.muted.opacity(0.65))
            context.clip(to: Path(CGRect(x: 0, y: 0, width: size.width * fraction, height: size.height)))
            draw(Theme.mint)
        }
    }
}

/// A frameless round video message; only an explicit tap opens and plays it.
struct VideoCircle: View {
    @Environment(AppModel.self) private var app
    let target: Target?
    let video: VideoMessage
    var diameter: CGFloat = 200

    var body: some View {
        let playback = app.playback!
        let key = playback.key(target, video.file)
        let active = playback.playingKey == key
        ZStack {
            Circle().fill(Theme.surfaceVariant)
            if let data = Data(base64Encoded: video.posterBase64), (4...8192).contains(data.count), let image = UIImage(data: data) {
                Image(uiImage: image).resizable().scaledToFill()
            }
            if active, let player = playback.video { PlayerLayerView(player: player) }
            if playback.loadingKey == key {
                ProgressView()
            } else if !active || !playback.playing {
                Image(systemName: "play.fill").font(.system(size: 36)).foregroundStyle(.white).shadow(radius: 4)
            }
            if active {
                Circle().trim(from: 0, to: VoiceWaveform.progress(playback.positionMs, durationMs: video.durationMs))
                    .stroke(Theme.mint, style: StrokeStyle(lineWidth: 3, lineCap: .round))
                    .rotationEffect(.degrees(-90))
                    .padding(2)
            }
        }
        .frame(width: diameter, height: diameter)
        .clipShape(Circle())
        .contentShape(Circle())
        .onTapGesture { playback.toggle(target, video.file) }
        .accessibilityElement()
        .accessibilityAddTraits(.isButton)
        .accessibilityLabel(active && playback.playing ? "Pause video message" : "Play video message")
    }
}

// MARK: Messages

/// One synchronized voice or video message: playable original, delivery state and transcript.
struct VoiceMessageContent: View {
    @Environment(AppModel.self) private var app
    let target: Target?
    let voice: VoiceMessage
    @State private var expanded = false

    var body: some View {
        let voiceState = app.voice!
        let local = voiceState.outgoing.first { $0.id == voice.id && target.map($0.belongs(to:)) == true }
        let pending = local != nil && !["submitted", "failed", "cancelled"].contains(voice.state)
        let restorable = local?.canRestoreUnsent == true
        let error = (local?.error).flatMap { $0.isEmpty ? nil : $0 } ?? voice.error
        VStack(alignment: .leading, spacing: 6) {
            if let video = voice.video, !expanded {
                VideoCircle(target: target, video: video, diameter: 220)
                    .frame(maxWidth: .infinity, alignment: .trailing)
            } else {
                VoicePlayerView(target: target, audio: voice.audio, durationMs: voice.durationMs, waveform: voice.waveform)
            }
            HStack(spacing: 10) {
                if !voice.transcript.isEmpty {
                    Button { expanded.toggle() } label: {
                        Image(systemName: expanded ? "chevron.up" : "text.alignleft")
                    }
                    .accessibilityLabel(expanded ? "Hide recognized text" : "Show recognized text")
                }
                if let local, pending || restorable {
                    Menu {
                        if pending {
                            Button("Check status") { voiceState.checkStatus(local) }.disabled(voiceState.activeRequests.contains(local.id))
                            Button("Cancel delivery", role: .destructive) { voiceState.cancel(local) }.disabled(voice.state == "uploading")
                        }
                        if restorable {
                            Button("Return to recording") { voiceState.restoreUnsent(local) }.disabled(voiceState.draft != nil || voiceState.savingRecording)
                        }
                    } label: { Image(systemName: "ellipsis.circle") }
                    .accessibilityLabel("Recording actions")
                }
                Spacer()
                DeliveryIcon(state: voice.state)
            }
            .font(.footnote)
            .buttonStyle(.borderless)
            if expanded && !voice.transcript.isEmpty { Text(voice.transcript).textSelection(.enabled) }
            if !error.isEmpty { Text(error).font(.caption).foregroundStyle(Theme.amber) }
        }
    }
}

struct DeliveryIcon: View {
    let state: String

    var body: some View {
        let delivery = RecordingDelivery.of(state)
        let color = [.failed, .uncertain].contains(delivery) ? Theme.amber : Theme.muted
        Group {
            switch delivery {
            case .processing: ProgressView().controlSize(.mini)
            case .sent: Image(systemName: "checkmark")
            case .failed: Image(systemName: "exclamationmark.circle")
            case .cancelled: Image(systemName: "xmark")
            case .uploading: Image(systemName: "icloud.and.arrow.up")
            case .uncertain: Image(systemName: "questionmark.circle")
            }
        }
        .foregroundStyle(color)
        .accessibilityLabel(delivery.description)
        .help(state == "submitted" ? delivery.explanation : VoiceProtocol.label(state))
        .accessibilityHint(state == "submitted" ? delivery.explanation : "")
    }
}

// MARK: Composer

/// Hold to record (slide up to lock, left to cancel); a short tap switches between voice and video.
struct VoiceButton: View {
    @Environment(AppModel.self) private var app
    @Environment(\.scenePhase) private var scenePhase
    let target: Target
    let saveEditor: () -> Void
    @State private var gesture = VoiceGesture(cancelDistance: 80, lockDistance: 57)
    @State private var pressing = false
    @State private var holdTask: Task<Void, Never>?
    @State private var settings = false
    @State private var accessDenied = false

    private var voice: VoiceController { app.voice }

    var body: some View {
        let enabled = !app.demo && app.storageReady && target.agentId.isEmpty && target.archiveId.isEmpty && !voice.savingRecording
        let label = voice.recordingLocked ? (voice.captureNeedsPreview ? "Stop and preview" : "Send recording")
            : voice.recording ? "Recording" : voice.videoMode ? "Video input" : "Voice input"
        ZStack {
            Circle().fill(voice.recording ? Color(hex: 0x5A2A2A) : enabled ? Theme.mint : Color(hex: 0x2B3B3B))
            Image(systemName: voice.recordingLocked ? (voice.captureNeedsPreview ? "stop.fill" : "arrow.up")
                : voice.videoMode ? "video.fill" : "mic.fill")
                .font(.body.weight(.semibold))
                .foregroundStyle(voice.recording ? Color.white : Theme.background)
        }
        .frame(width: 40, height: 40)
        .scaleEffect(voice.touchRecording ? 1.25 : 1)
        .animation(.spring(duration: 0.2), value: voice.touchRecording)
        .contentShape(Circle())
        // Global coordinates: the composer reflows while recording starts under the finger.
        .gesture(DragGesture(minimumDistance: 0, coordinateSpace: .global)
            .onChanged { value in changed(value, enabled: enabled) }
            .onEnded { _ in ended() })
        .accessibilityElement()
        .accessibilityLabel(label)
        .accessibilityAddTraits(.isButton)
        .accessibilityAction { activate(enabled: enabled) }
        .accessibilityAction(named: voice.videoMode ? "Switch to voice messages" : "Switch to video messages") { voice.toggleMode(target) }
        .onChange(of: scenePhase) { _, phase in
            if phase != .active {
                interrupt()
                voice.background()
            }
        }
        .onDisappear { interrupt() }
        .sheet(isPresented: $settings) { VoiceSettingsSheet(target: target, startable: true) }
        .alert("Recording access is off", isPresented: $accessDenied) {
            Button("Open Settings") { if let url = URL(string: UIApplication.openSettingsURLString) { UIApplication.shared.open(url) } }
            Button("Not now", role: .cancel) {}
        } message: {
            Text("Allow Microphone\(voice.videoMode ? " and Camera" : "") access for Zerus in Settings, then hold Voice input to record.")
        }
    }

    /// Keyboard and assistive activation: start a locked capture, or finish the active one.
    private func activate(enabled: Bool) {
        guard enabled else { return }
        if let id = voice.activeCaptureId { voice.sendCapture(id) } else { _ = start(gesture: false) }
    }

    private func changed(_ value: DragGesture.Value, enabled: Bool) {
        if !pressing {
            pressing = true
            guard enabled, gesture.begin() else { return }
            holdTask = Task {
                try? await Task.sleep(for: .milliseconds(250))
                guard !Task.isCancelled, pressing, gesture.hold() == .start else { return }
                gesture.captured(start(gesture: true))
            }
            return
        }
        guard let action = gesture.move(horizontal: value.translation.width, vertical: value.translation.height) else {
            if let id = voice.gestureCaptureId { voice.moveGesture(id, progress: gesture.progress, lockProgress: gesture.lockProgress) }
            return
        }
        perform(action)
    }

    private func ended() {
        pressing = false
        holdTask?.cancel()
        if let action = gesture.release() { perform(action) }
    }

    private func interrupt() {
        guard pressing else { return }
        pressing = false
        holdTask?.cancel()
        if let action = gesture.interrupt() { perform(action) }
    }

    private func perform(_ action: VoiceGesture.Action) {
        switch action {
        case .switchMode:
            voice.toggleMode(target)
        case .start:
            break
        case .lock(let id):
            voice.lockCapture(id)
        case .finish(let id, let end):
            switch end {
            case .send:
                guard voice.finishGesture(id, cancel: false) else { return }
                voice.sendCapture(id)
            case .preview:
                voice.finishGesture(id, cancel: false, userInitiated: false)
            case .cancel:
                voice.finishGesture(id, cancel: true)
            }
        }
    }

    private func start(gesture: Bool) -> Int64? {
        saveEditor()
        let current = app.conversation.draft(target)
        guard current.text.isEmpty, current.attachments.isEmpty, app.conversation.messageState.pickerDraft?.target != target else { return nil }
        guard voice.startReason(target).isEmpty else {
            settings = true
            return nil
        }
        let video = voice.videoMode
        let microphone = AVAudioApplication.shared.recordPermission
        let camera = AVCaptureDevice.authorizationStatus(for: .video)
        if microphone == .denied || (video && [.denied, .restricted].contains(camera)) {
            voice.permissionDenied()
            accessDenied = true
            return nil
        }
        if microphone == .undetermined || (video && camera == .notDetermined) {
            // The system prompt ends this press; granting access never revives it.
            Task {
                if microphone == .undetermined { _ = await AVAudioApplication.requestRecordPermission() }
                if video && camera == .notDetermined { _ = await AVCaptureDevice.requestAccess(for: .video) }
            }
            return nil
        }
        let haptics = UIImpactFeedbackGenerator(style: .light)
        haptics.prepare()
        return voice.start(target, gesture: gesture) { _ in haptics.impactOccurred() }
    }
}

/// The recording strip shown in place of the text field while a held capture runs.
struct RecordingRow: View {
    @Environment(AppModel.self) private var app

    var body: some View {
        let voice = app.voice!
        let elapsed = voiceRecordingTime(voice.elapsedMs)
        HStack(spacing: 8) {
            Circle().fill(Color.red).frame(width: 8, height: 8)
            Text(elapsed).font(.body.monospacedDigit()).accessibilityLabel("Recording \(elapsed)")
            if voice.touchRecording {
                Image(systemName: "lock")
                    .foregroundStyle(Theme.muted)
                    .offset(y: -12 * voice.gestureLockProgress)
                    .accessibilityLabel("Slide up to lock")
            }
            Spacer(minLength: 4)
            if voice.touchRecording {
                HStack(spacing: 2) {
                    Image(systemName: "chevron.left")
                    Text("Slide to cancel").lineLimit(1)
                }
                .foregroundStyle(Theme.muted)
                .offset(x: -32 * voice.gestureCancelProgress)
                .opacity(1 - voice.gestureCancelProgress)
                .accessibilityLabel("Slide to cancel")
            } else {
                Text("Recording").foregroundStyle(Theme.muted)
            }
            Spacer(minLength: 0)
        }
        .padding(.horizontal, 12)
        .frame(minHeight: 40)
        .background(Theme.surface, in: RoundedRectangle(cornerRadius: 20))
    }
}

/// Live square camera preview with the remaining time while a video message records.
struct RoundVideoRecordingOverlay: View {
    @Environment(AppModel.self) private var app

    var body: some View {
        let voice = app.voice!
        if voice.recording, let recorder = voice.videoRecorder {
            VStack(spacing: 8) {
                ZStack(alignment: .bottomTrailing) {
                    CapturePreviewView(session: recorder.session)
                        .frame(width: 240, height: 240)
                        .clipShape(Circle())
                        .overlay(Circle().trim(from: 0, to: VideoRecordingClock.progress(voice.elapsedMs))
                            .stroke(Color.red, style: StrokeStyle(lineWidth: 4, lineCap: .round)).rotationEffect(.degrees(-90)))
                    Button { recorder.switchCamera() } label: {
                        Image(systemName: "arrow.triangle.2.circlepath.camera").padding(10).background(.ultraThinMaterial, in: Circle())
                    }
                    .accessibilityLabel("Switch camera")
                }
                Text("\(VideoRecordingClock.remainingSeconds(voice.elapsedMs)) s left").font(.caption.monospacedDigit()).foregroundStyle(Theme.muted)
            }
            .padding(.vertical, 8)
            .frame(maxWidth: .infinity)
        }
    }
}

/// Recording controls, the saving state and the retained preview with its separate Send.
struct VoiceComposerStatus: View {
    @Environment(AppModel.self) private var app
    @State private var confirmCold: VoiceDraft?
    @State private var confirmDiscard: VoiceDraft?
    @State private var providerSettings: VoiceDraft?

    var body: some View {
        let voice = app.voice!
        let draft = voice.draft
        let sendReason = draft != nil ? voice.sendReason() : ""
        VStack(alignment: .leading, spacing: 4) {
            if voice.recording {
                if voice.touchRecording {
                    Text("Slide up to lock, left to cancel").font(.caption).foregroundStyle(Theme.muted)
                } else if let id = voice.activeCaptureId {
                    HStack(spacing: 16) {
                        Button("Stop recording") { voice.stopCapture(id) }
                        Button("Cancel", role: .destructive) { voice.cancelCapture(id) }
                        if !voice.captureNeedsPreview { Button("Send") { voice.sendCapture(id) } }
                    }
                    .font(.footnote.weight(.semibold))
                    .buttonStyle(.borderless)
                }
            } else if voice.savingRecording {
                Text("Saving recording…").font(.caption).foregroundStyle(Theme.muted)
                ProgressView().progressViewStyle(.linear).tint(Theme.mint)
            } else if let draft {
                preview(draft, sendReason: sendReason)
            }
            if !voice.notice.isEmpty && voice.notice != sendReason { Text(voice.notice).font(.caption).foregroundStyle(Theme.muted) }
        }
        .sheet(item: Binding(get: { providerSettings.map { IdentifiedDraft(draft: $0) } }, set: { providerSettings = $0?.draft })) { item in
            VoiceSettingsSheet(target: item.draft.target, preview: item.draft)
        }
        .alert("Send with full context?", isPresented: Binding(get: { confirmCold != nil }, set: { if !$0 { confirmCold = nil } }), presenting: confirmCold) { captured in
            Button("Send with full context") {
                confirmCold = nil
                if app.voice.draft == captured && app.voice.sendReason().isEmpty { app.voice.submit(allowColdCache: true) }
            }
            Button("Cancel", role: .cancel) { confirmCold = nil }
        } message: { captured in
            Text("The prompt cache for \(captured.target.session) has expired. Sending this voice message can process the full conversation again and use more tokens. Your typed draft will stay here.")
        }
        .alert("Discard recording?", isPresented: Binding(get: { confirmDiscard != nil }, set: { if !$0 { confirmDiscard = nil } }), presenting: confirmDiscard) { captured in
            Button("Discard recording", role: .destructive) {
                confirmDiscard = nil
                if app.voice.draft == captured && !app.voice.savingRecording {
                    app.playback.stop()
                    app.voice.discardRecording()
                }
            }
            Button("Keep recording", role: .cancel) { confirmDiscard = nil }
        } message: { captured in
            Text("Discard the saved voice recording for \(captured.target.session)? This removes only this unsent recording. Your typed draft and sent messages are kept.")
        }
    }

    struct IdentifiedDraft: Identifiable {
        let draft: VoiceDraft
        var id: String { draft.audio.id }
    }

    private func preview(_ draft: VoiceDraft, sendReason: String) -> some View {
        let voice = app.voice!
        let providerReason = voice.providerReason(draft)
        return VStack(alignment: .leading, spacing: 6) {
            if app.conversation.selected?.target != draft.target { Text("For \(draft.target.session)").font(.caption.weight(.semibold)) }
            HStack(alignment: .center, spacing: 10) {
                if let video = draft.video {
                    VideoCircle(target: draft.target, video: video.displayed(draft.audio.id), diameter: 120)
                    Text("Pending").font(.caption).foregroundStyle(Theme.muted)
                    Spacer()
                } else {
                    VoicePlayerView(target: draft.target, audio: draft.audio.displayed(), durationMs: draft.durationMs, status: "Pending")
                }
                Button { confirmDiscard = draft } label: { Image(systemName: "trash").frame(width: 36, height: 36) }
                    .buttonStyle(.plain)
                    .foregroundStyle(Theme.muted)
                    .accessibilityLabel("Discard")
                Button {
                    guard app.voice.draft == draft, app.voice.sendReason().isEmpty else { return }
                    app.playback.stop()
                    if app.conversation.voiceColdCache() { confirmCold = draft } else { app.voice.submit() }
                } label: {
                    Image(systemName: "arrow.up").font(.body.weight(.semibold)).frame(width: 36, height: 36)
                        .foregroundStyle(sendReason.isEmpty ? Theme.background : Theme.muted)
                        .background(sendReason.isEmpty ? Theme.mint : Color(hex: 0x2B3B3B), in: Circle())
                }
                .buttonStyle(.plain)
                .disabled(!sendReason.isEmpty)
                .accessibilityLabel(draft.video == nil ? "Send voice message" : "Send video message")
            }
            if voice.loading {
                Text("Checking transcription provider…").font(.caption).foregroundStyle(Theme.muted)
            } else if !providerReason.isEmpty {
                Text(providerReason).font(.caption).foregroundStyle(Theme.muted)
                Button(draft.machineId.isEmpty || draft.profileId.isEmpty ? "Choose provider" : "Review provider") { providerSettings = draft }
                    .font(.caption.weight(.semibold))
                    .accessibilityLabel("Transcription provider")
            }
            if !sendReason.isEmpty && sendReason != providerReason { Text(sendReason).font(.caption).foregroundStyle(Theme.muted) }
            if app.conversation.selected?.target != draft.target, let session = app.sessions.first(where: { $0.target == draft.target }) {
                Button("Open original conversation") { app.conversationRequest = session.target }.font(.caption.weight(.semibold))
            }
        }
        .padding(8)
        .background(Theme.surface, in: RoundedRectangle(cornerRadius: 16))
        .overlay(RoundedRectangle(cornerRadius: 16).stroke(Theme.muted.opacity(0.3)))
    }
}

// MARK: Settings

/// Computer and profile that transcribe recordings for this conversation, plus the preview preference.
struct VoiceSettingsSheet: View {
    @Environment(AppModel.self) private var app
    @Environment(\.dismiss) private var dismiss
    let target: Target?
    var preview: VoiceDraft?
    var startable = false
    @State private var manage = false

    var body: some View {
        let voice = app.voice!
        let choicePreview = voice.draft.flatMap { $0.target == target && (preview == nil || $0.audio.id == preview?.audio.id) ? $0 : nil }
        let current = preview == nil || choicePreview != nil
        let choice = choicePreview?.selection ?? voice.selection
        let selected = voice.computers.first { $0.id == choice.host?.computerId }
        let profiles = selected?.profiles ?? []
        let chosen = profiles.first { $0.id == choice.profileId }
        NavigationStack {
            List {
                if !current {
                    Text("This recording is no longer available.")
                } else {
                    if let preview { Section { Text("Recording for \(preview.target.session)").font(.footnote) } }
                    Section {
                        Toggle(isOn: Binding(get: { voice.previewBeforeSend }, set: { voice.setPreviewBeforeSend($0) })) {
                            VStack(alignment: .leading) {
                                Text("Preview before sending")
                                Text("Review audio and video recordings, then tap Send. Applies on this device.").font(.caption).foregroundStyle(Theme.muted)
                            }
                        }
                        .disabled(!voice.ready || voice.recording || voice.savingRecording)
                    }
                    if target == nil {
                        DefaultVoiceProviderSection()
                    } else {
                        Section {
                            Picker("Computer", selection: Binding(get: { choice.host?.computerId ?? "" }, set: { id in
                                guard let target else { return }
                                voice.chooseHost(MachineKey(connectionId: target.connectionId, computerId: id), target: target, previewId: choicePreview?.audio.id)
                            })) {
                                if selected == nil { Text(choice.host == nil ? "Choose voice computer" : "Selected computer unavailable").tag(choice.host?.computerId ?? "") }
                                ForEach(voice.computers, id: \.id) { Text($0.name).tag($0.id) }
                            }
                            .disabled(voice.loading || voice.computers.isEmpty || voice.recording)
                            Picker("Profile", selection: Binding(get: { choice.profileId }, set: { id in
                                guard let target else { return }
                                voice.chooseProfile(id, target: target, previewId: choicePreview?.audio.id)
                            })) {
                                if chosen == nil { Text(choice.profileId.isEmpty ? "Choose voice profile" : choice.profileId).tag(choice.profileId) }
                                ForEach(profiles, id: \.id) { profile in
                                    Text(profile.available ? profile.name : "\(profile.name) (\(profile.reason.isEmpty ? "unavailable" : profile.reason))").tag(profile.id)
                                }
                            }
                            .disabled(voice.loading || profiles.isEmpty || voice.recording)
                            if voice.loading { ProgressView().frame(maxWidth: .infinity) }
                            Button("Refresh voice profiles") { voice.refreshProfiles(target) }.disabled(voice.loading)
                        } footer: {
                            VStack(alignment: .leading, spacing: 6) {
                                Text("Choose the computer and profile that transcribe recordings for this conversation. The original audio stays with the conversation.")
                                if !voice.profileNotice.isEmpty { Text(voice.profileNotice) }
                                if !voice.statistics.isEmpty { Text(voice.statistics) }
                            }
                        }
                    }
                    Section {
                        Text((voice.previewBeforeSend ? "Hold to record and release to preview; tap Send when ready. " : "Hold to record and release to send. ") +
                             "Slide up to lock, or left to cancel. Tap to switch between voice and video. Zerus keeps the media in conversation history, transcribes it, then sends the recognized text to the agent. Your typed draft is kept separately. Your gateway can read the recordings.")
                            .font(.footnote)
                            .foregroundStyle(Theme.muted)
                        if !voice.notice.isEmpty { Text(voice.notice).font(.footnote).foregroundStyle(Theme.amber) }
                        Button("Manage speech providers") { manage = true }.disabled(app.demo || !app.storageReady)
                        if startable, let target {
                            Button("Start recording") {
                                dismiss()
                                _ = voice.start(target, gesture: false)
                            }
                            .disabled(!voice.startReason(target).isEmpty)
                        }
                    } footer: {
                        if startable, let target, let reason = voice.startReason(target).nonEmpty { Text(reason) }
                    }
                }
            }
            .navigationTitle("Voice messages")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar { ToolbarItem(placement: .confirmationAction) { Button("Done") { dismiss() } } }
            .navigationDestination(isPresented: $manage) { SpeechProvidersView(defaultSelection: choice) }
            .zerusScreen()
        }
        .task(id: target?.key) {
            if target == nil { voice.refreshDefaultProfiles() } else if current { voice.refreshProfiles(target) }
        }
    }
}

/// The default computer and profile for new recordings, chosen from verified online computers without a conversation.
private struct DefaultVoiceProviderSection: View {
    @Environment(AppModel.self) private var app

    var body: some View {
        let voice = app.voice!
        let host = voice.defaultHost
        let profiles = voice.defaultProfiles
        let selected = voice.selection.profileId
        Section {
            NavigationLink { DefaultVoiceComputerPicker() } label: {
                LabeledContent("Computer", value: host?.name ?? (voice.selection.host == nil ? "Choose voice computer" : "Selected computer unavailable"))
            }
            .disabled(!voice.ready || voice.recording || voice.savingRecording)
            Picker("Profile", selection: Binding(get: { selected }, set: { id in
                if let profile = profiles.first(where: { $0.id == id }) { voice.chooseDefaultProfile(profile) }
            })) {
                if !profiles.contains(where: { $0.id == selected }) { Text(selected.isEmpty ? "Choose voice profile" : selected).tag(selected) }
                ForEach(profiles, id: \.id) { profile in
                    Text(profile.available ? profile.name : "\(profile.name) (\(profile.reason.isEmpty ? "unavailable" : profile.reason))").tag(profile.id)
                }
            }
            .disabled(voice.defaultLoading || profiles.isEmpty || voice.recording || voice.savingRecording)
            if voice.defaultLoading { ProgressView().frame(maxWidth: .infinity) }
            Button("Refresh voice profiles") { voice.refreshDefaultProfiles() }.disabled(voice.defaultLoading)
        } footer: {
            VStack(alignment: .leading, spacing: 6) {
                Text("Choose the computer and profile that transcribe new recordings. Existing recordings keep their computer, profile and reviewed revision.")
                if !voice.defaultNotice.isEmpty { Text(voice.defaultNotice) }
                if !voice.statistics.isEmpty { Text(voice.statistics) }
            }
        }
    }
}

/// A searchable list of at most 100 verified online computers.
private struct DefaultVoiceComputerPicker: View {
    @Environment(AppModel.self) private var app
    @Environment(\.dismiss) private var dismiss
    @State private var query = ""

    var body: some View {
        let voice = app.voice!
        let hosts = voice.defaultHosts(query)
        List {
            ForEach(hosts, id: \.self) { host in
                let reason = voice.defaultReason(host)
                Button {
                    if voice.chooseDefaultHost(host) { dismiss() }
                } label: {
                    HStack {
                        VStack(alignment: .leading, spacing: 2) {
                            Text(host.name)
                            if !reason.isEmpty { Text(reason).font(.caption).foregroundStyle(Theme.muted) }
                        }
                        Spacer()
                        if voice.defaultHost == host { Image(systemName: "checkmark").accessibilityLabel("Selected") }
                    }
                }
                .disabled(!reason.isEmpty || voice.recording || voice.savingRecording)
            }
            if hosts.isEmpty {
                Text(query.isEmpty ? "No verified online voice computers are available. Refresh Machines and try again." : "No computers match.")
                    .foregroundStyle(Theme.muted)
            }
        }
        .searchable(text: $query, prompt: "Search computers")
        .navigationTitle("Voice computer")
        .navigationBarTitleDisplayMode(.inline)
        .zerusScreen()
    }
}

/// This phone's saved voice receipts, for status checks and local-copy removal.
struct VoiceSavedMessages: View {
    @Environment(AppModel.self) private var app
    @State private var remove: VoiceOutgoing?

    var body: some View {
        let voice = app.voice!
        ForEach(voice.outgoing.sorted { $0.createdAt > $1.createdAt }, id: \.id) { entry in
            VStack(alignment: .leading, spacing: 6) {
                Text(entry.target.session).font(.subheadline.weight(.semibold)).lineLimit(1)
                VoiceMessageContent(target: entry.effectiveTarget, voice: entry.displayed())
                HStack(spacing: 16) {
                    if let session = app.sessions.first(where: { entry.belongs(to: $0.target) }) {
                        Button("Open conversation") { app.conversationRequest = session.target }
                    }
                    if entry.terminal { Button("Remove local copy", role: .destructive) { remove = entry }.disabled(voice.activeRequests.contains(entry.id)) }
                }
                .font(.footnote.weight(.semibold))
                .buttonStyle(.borderless)
            }
        }
        .alert("Remove local voice copy?", isPresented: Binding(get: { remove != nil }, set: { if !$0 { remove = nil } }), presenting: remove) { entry in
            Button("Remove local copy", role: .destructive) {
                app.playback.stop()
                voice.dismiss(entry)
                remove = nil
            }
            Button("Keep", role: .cancel) { remove = nil }
        } message: { entry in
            Text("Remove this phone's saved audio and receipt for \(entry.target.session)? This does not cancel delivery or remove the message from the computer's conversation history.")
        }
    }
}
