import AVFoundation
import Observation
import SwiftUI
import ZerusKit

/// Plays one recording at a time, only after a Play tap and authenticated byte verification.
@MainActor
@Observable
final class VoicePlayback {
    @ObservationIgnored private unowned let app: AppModel
    private(set) var playingKey = ""
    private(set) var playing = false
    private(set) var loadingKey = ""
    private(set) var positionMs = 0
    private(set) var failure: (key: String, message: String)?
    /// The active video message player, shown inside its own circle.
    private(set) var video: AVPlayer?
    @ObservationIgnored private var audio: AVAudioPlayer?
    @ObservationIgnored private var loading: Task<Void, Never>?
    @ObservationIgnored private var ticker: Task<Void, Never>?
    @ObservationIgnored private var ended: NSObjectProtocol?

    init(app: AppModel) { self.app = app }

    func key(_ target: Target?, _ file: DisplayedAttachment) -> String { AttachmentIdentity.playbackKey(target, file) }

    func toggle(_ target: Target?, _ file: DisplayedAttachment) {
        guard ["audio/wav", "video/mp4"].contains(file.mime), !app.voice.recording else { return }
        let key = key(target, file)
        if playingKey == key {
            guard loadingKey.isEmpty else { return }
            if playing { pause() } else { resume() }
            return
        }
        stop()
        playingKey = key
        loadingKey = key
        failure = nil
        loading = Task { [weak self] in
            guard let self else { return }
            do {
                let entry = try await app.viewer.fetch(target, file)
                guard playingKey == key, !Task.isCancelled else { return }
                loadingKey = ""
                try AVAudioSession.sharedInstance().setCategory(.playback, mode: .spokenAudio)
                try AVAudioSession.sharedInstance().setActive(true)
                if file.mime == "video/mp4" {
                    let player = AVPlayer(url: entry.file)
                    video = player
                    ended = NotificationCenter.default.addObserver(forName: AVPlayerItem.didPlayToEndTimeNotification, object: player.currentItem, queue: .main) { [weak self] _ in
                        MainActor.assumeIsolated { self?.finished() }
                    }
                    player.play()
                } else {
                    let player = try AVAudioPlayer(contentsOf: entry.file)
                    audio = player
                    player.prepareToPlay()
                    guard player.play() else { throw ZerusError.invalid("Could not play this recording. Try again.") }
                }
                playing = true
                tick()
            } catch is CancellationError {
            } catch {
                guard playingKey == key else { return }
                let message = AttachmentViewer.message(error)
                stop()
                failure = (key, message == "The attachment could not be opened. Try again." ? "Could not play this recording. Try again." : message)
            }
        }
    }

    private func tick() {
        ticker?.cancel()
        ticker = Task { [weak self] in
            while !Task.isCancelled {
                guard let self, playing else { return }
                if let audio {
                    positionMs = Int(audio.currentTime * 1000)
                    if !audio.isPlaying {
                        finished()
                        return
                    }
                } else if let video {
                    positionMs = Int(CMTimeGetSeconds(video.currentTime()) * 1000)
                }
                try? await Task.sleep(for: .milliseconds(100))
            }
        }
    }

    private func finished() {
        playing = false
        positionMs = 0
        audio?.currentTime = 0
        video?.seek(to: .zero)
        ticker?.cancel()
    }

    private func pause() {
        audio?.pause()
        video?.pause()
        playing = false
        ticker?.cancel()
    }

    private func resume() {
        if let audio { audio.play() } else { video?.play() }
        playing = true
        tick()
    }

    func seek(_ target: Target?, _ file: DisplayedAttachment, fraction: Double, durationMs: Int) {
        guard playingKey == key(target, file), let position = VoiceWaveform.seekPosition(fraction, durationMs: durationMs) else { return }
        audio?.currentTime = Double(position) / 1000
        video?.seek(to: CMTime(value: CMTimeValue(position), timescale: 1000))
        positionMs = position
    }

    func stop() {
        loading?.cancel()
        ticker?.cancel()
        audio?.stop()
        video?.pause()
        if let ended { NotificationCenter.default.removeObserver(ended) }
        ended = nil
        audio = nil
        video = nil
        playing = false
        playingKey = ""
        loadingKey = ""
        positionMs = 0
        try? AVAudioSession.sharedInstance().setActive(false, options: .notifyOthersOnDeactivation)
    }
}

/// A video layer for the active round video message.
struct PlayerLayerView: UIViewRepresentable {
    let player: AVPlayer

    final class LayerView: UIView {
        override static var layerClass: AnyClass { AVPlayerLayer.self }
        var playerLayer: AVPlayerLayer { layer as! AVPlayerLayer }
    }

    func makeUIView(context: Context) -> LayerView {
        let view = LayerView()
        view.playerLayer.videoGravity = .resizeAspectFill
        view.playerLayer.player = player
        return view
    }

    func updateUIView(_ view: LayerView, context: Context) { view.playerLayer.player = player }
}

/// The live camera preview while a round video is recorded.
struct CapturePreviewView: UIViewRepresentable {
    let session: AVCaptureSession

    final class PreviewView: UIView {
        override static var layerClass: AnyClass { AVCaptureVideoPreviewLayer.self }
        var previewLayer: AVCaptureVideoPreviewLayer { layer as! AVCaptureVideoPreviewLayer }
    }

    func makeUIView(context: Context) -> PreviewView {
        let view = PreviewView()
        view.previewLayer.videoGravity = .resizeAspectFill
        view.previewLayer.session = session
        return view
    }

    func updateUIView(_ view: PreviewView, context: Context) {}
}
