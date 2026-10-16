import AVFoundation
import UIKit
import ZerusKit

struct CapturedVideo: Sendable {
    let bytes: Data
    let durationMs: Int
    let width: Int
    let height: Int
    let posterBase64: String
}

struct CapturedMedia: Sendable {
    let pcm: Data
    var video: CapturedVideo?
}

/// One owned capture. `stop` keeps what was recorded; `cancel` discards it.
protocol CaptureDevice: AnyObject {
    @MainActor func capture(started: @escaping @MainActor () -> Void, progress: @escaping @MainActor (Int) -> Void) async throws -> CapturedMedia
    @MainActor func stop()
    @MainActor func cancel()
}

/// Bounded PCM shared with the realtime audio thread.
final class PcmSink: @unchecked Sendable {
    private let lock = NSLock()
    private var converter: MediaPcm16Converter

    init(rate: Int, channels: Int, limit: Int) { converter = MediaPcm16Converter(rate: rate, channels: channels, limit: limit) }

    func append(_ buffer: AVAudioPCMBuffer) {
        guard let channels = buffer.floatChannelData else { return }
        let count = Int(buffer.frameLength), width = Int(buffer.format.channelCount)
        lock.lock()
        defer { lock.unlock() }
        for frame in 0..<count where !converter.full {
            for channel in 0..<width { converter.sample(Int((min(max(channels[channel][frame], -1), 1) * 32_767).rounded())) }
        }
    }

    var count: Int { lock.withLock { converter.count } }
    var full: Bool { lock.withLock { converter.full } }
    var bytes: Data { lock.withLock { converter.bytes } }
}

/// Foreground microphone capture: 16 kHz mono PCM for up to two minutes.
@MainActor
final class AudioCaptureDevice: CaptureDevice {
    private let engine = AVAudioEngine()
    private var stopped = false
    private var discarded = false
    private var interruption: NSObjectProtocol?

    func stop() { stopped = true }
    func cancel() {
        discarded = true
        stopped = true
    }

    /// Built outside the main actor: the engine calls it on its realtime thread.
    nonisolated static func tap(_ sink: PcmSink) -> AVAudioNodeTapBlock {
        { buffer, _ in sink.append(buffer) }
    }

    func capture(started: @escaping @MainActor () -> Void, progress: @escaping @MainActor (Int) -> Void) async throws -> CapturedMedia {
        if stopped { return CapturedMedia(pcm: Data()) }
        guard AVAudioApplication.shared.recordPermission == .granted else { throw ZerusError.invalid("Allow microphone access to record your voice.") }
        let session = AVAudioSession.sharedInstance()
        try session.setCategory(.playAndRecord, mode: .spokenAudio, options: [.defaultToSpeaker, .allowBluetoothHFP])
        try session.setActive(true)
        defer { try? session.setActive(false, options: .notifyOthersOnDeactivation) }
        let input = engine.inputNode
        let format = input.outputFormat(forBus: 0)
        guard format.sampleRate >= 8_000, format.sampleRate <= 192_000, (1...8).contains(format.channelCount) else {
            throw ZerusError.invalid("This device could not open a microphone.")
        }
        let sink = PcmSink(rate: Int(format.sampleRate), channels: Int(format.channelCount), limit: VoiceProtocol.maxPcmBytes)
        input.installTap(onBus: 0, bufferSize: 4_096, format: format, block: Self.tap(sink))
        defer {
            input.removeTap(onBus: 0)
            engine.stop()
            if let interruption { NotificationCenter.default.removeObserver(interruption) }
        }
        interruption = NotificationCenter.default.addObserver(forName: AVAudioSession.interruptionNotification, object: nil, queue: .main) { [weak self] _ in
            MainActor.assumeIsolated { self?.stopped = true }
        }
        engine.prepare()
        do { try engine.start() } catch { throw ZerusError.invalid("The microphone is unavailable. Check other recording apps.") }
        started()
        var lastTenth = -1
        while !stopped && !sink.full {
            try await Task.sleep(for: .milliseconds(50))
            let elapsed = sink.count / 32, tenth = elapsed / 100
            if tenth != lastTenth {
                lastTenth = tenth
                progress(elapsed)
            }
        }
        return CapturedMedia(pcm: discarded ? Data() : sink.bytes)
    }
}

/// A square front-camera recording written as H.264/AAC MP4, bounded to 59 s and 10 MiB.
/// The temporary file never leaves the app's private temporary folder.
final class RoundVideoRecorder: NSObject, CaptureDevice, AVCaptureVideoDataOutputSampleBufferDelegate, AVCaptureAudioDataOutputSampleBufferDelegate,
    @unchecked Sendable {
    let session = AVCaptureSession()
    private let queue = DispatchQueue(label: "app.zerus.round-video")
    private let videoOutput = AVCaptureVideoDataOutput()
    private let audioOutput = AVCaptureAudioDataOutput()
    private var writer: AVAssetWriter?
    private var videoInput: AVAssetWriterInput?
    private var audioInput: AVAssetWriterInput?
    private var start: CMTime?
    private var finishing = false
    private var stopRequested = false
    private var discarded = false
    private var front = true
    private var startedHandler: (@MainActor () -> Void)?
    private var progressHandler: (@MainActor (Int) -> Void)?
    private var completion: CheckedContinuation<Void, Error>?
    private let output = FileManager.default.temporaryDirectory.appendingPathComponent("round-video-\(UUID().uuidString.lowercased()).mp4")

    static var available: Bool { AVCaptureDevice.default(.builtInWideAngleCamera, for: .video, position: .front) != nil ||
        AVCaptureDevice.default(.builtInWideAngleCamera, for: .video, position: .back) != nil }

    func stop() { queue.async { self.requestStop() } }

    func cancel() {
        queue.async {
            self.discarded = true
            self.requestStop()
        }
    }

    /// Switches between the front and back cameras while recording continues.
    func switchCamera() {
        queue.async {
            guard !self.finishing, let current = self.session.inputs.compactMap({ $0 as? AVCaptureDeviceInput }).first(where: { $0.device.hasMediaType(.video) }),
                  let device = AVCaptureDevice.default(.builtInWideAngleCamera, for: .video, position: self.front ? .back : .front),
                  let input = try? AVCaptureDeviceInput(device: device) else { return }
            self.session.beginConfiguration()
            self.session.removeInput(current)
            if self.session.canAddInput(input) {
                self.session.addInput(input)
                self.front.toggle()
            } else {
                self.session.addInput(current)
            }
            self.configureConnection()
            self.session.commitConfiguration()
        }
    }

    private func configureConnection() {
        guard let connection = videoOutput.connection(with: .video) else { return }
        if connection.isVideoRotationAngleSupported(90) { connection.videoRotationAngle = 90 }
        if connection.isVideoMirroringSupported {
            connection.automaticallyAdjustsVideoMirroring = false
            connection.isVideoMirrored = front
        }
    }

    @MainActor
    func capture(started: @escaping @MainActor () -> Void, progress: @escaping @MainActor (Int) -> Void) async throws -> CapturedMedia {
        guard AVCaptureDevice.authorizationStatus(for: .video) == .authorized, AVAudioApplication.shared.recordPermission == .granted else {
            throw ZerusError.invalid("Allow camera and microphone access to record a video message.")
        }
        let audioSession = AVAudioSession.sharedInstance()
        try audioSession.setCategory(.playAndRecord, mode: .videoRecording, options: [.defaultToSpeaker, .allowBluetoothHFP])
        try audioSession.setActive(true)
        defer {
            try? audioSession.setActive(false, options: .notifyOthersOnDeactivation)
            try? FileManager.default.removeItem(at: output)
        }
        startedHandler = started
        progressHandler = progress
        try await withCheckedThrowingContinuation { (continuation: CheckedContinuation<Void, Error>) in
            queue.async {
                self.completion = continuation
                do {
                    try self.configure()
                    if self.stopRequested { self.finish(error: nil) } else { self.session.startRunning() }
                } catch {
                    self.finish(error: error)
                }
            }
        }
        let discarded = await withCheckedContinuation { continuation in queue.async { continuation.resume(returning: self.discarded) } }
        if discarded { return CapturedMedia(pcm: Data()) }
        let file = output
        return try await Task.detached(priority: .userInitiated) { try await Self.prepare(file) }.value
    }

    private func configure() throws {
        session.beginConfiguration()
        defer { session.commitConfiguration() }
        session.sessionPreset = .vga640x480
        let position: AVCaptureDevice.Position = AVCaptureDevice.default(.builtInWideAngleCamera, for: .video, position: .front) != nil ? .front : .back
        front = position == .front
        guard let camera = AVCaptureDevice.default(.builtInWideAngleCamera, for: .video, position: position),
              let microphone = AVCaptureDevice.default(for: .audio) else { throw ZerusError.invalid("No camera is available.") }
        let cameraInput = try AVCaptureDeviceInput(device: camera), microphoneInput = try AVCaptureDeviceInput(device: microphone)
        guard session.canAddInput(cameraInput), session.canAddInput(microphoneInput), session.canAddOutput(videoOutput), session.canAddOutput(audioOutput) else {
            throw ZerusError.invalid("The camera could not start.")
        }
        session.addInput(cameraInput)
        session.addInput(microphoneInput)
        videoOutput.alwaysDiscardsLateVideoFrames = true
        videoOutput.setSampleBufferDelegate(self, queue: queue)
        audioOutput.setSampleBufferDelegate(self, queue: queue)
        session.addOutput(videoOutput)
        session.addOutput(audioOutput)
        configureConnection()
    }

    private func makeWriter() throws {
        let writer = try AVAssetWriter(outputURL: output, fileType: .mp4)
        let video = AVAssetWriterInput(mediaType: .video, outputSettings: [
            AVVideoCodecKey: AVVideoCodecType.h264, AVVideoWidthKey: 480, AVVideoHeightKey: 480, AVVideoScalingModeKey: AVVideoScalingModeResizeAspectFill,
            AVVideoCompressionPropertiesKey: [AVVideoAverageBitRateKey: 1_000_000, AVVideoMaxKeyFrameIntervalKey: 60],
        ])
        let audio = AVAssetWriterInput(mediaType: .audio, outputSettings: [
            AVFormatIDKey: kAudioFormatMPEG4AAC, AVSampleRateKey: 44_100, AVNumberOfChannelsKey: 1, AVEncoderBitRateKey: 64_000,
        ])
        video.expectsMediaDataInRealTime = true
        audio.expectsMediaDataInRealTime = true
        guard writer.canAdd(video), writer.canAdd(audio) else { throw ZerusError.invalid("The camera could not save this recording.") }
        writer.add(video)
        writer.add(audio)
        self.writer = writer
        videoInput = video
        audioInput = audio
    }

    func captureOutput(_ output: AVCaptureOutput, didOutput sampleBuffer: CMSampleBuffer, from connection: AVCaptureConnection) {
        guard !finishing else { return }
        let time = CMSampleBufferGetPresentationTimeStamp(sampleBuffer)
        if output === videoOutput {
            if start == nil {
                do {
                    try makeWriter()
                    guard let writer, writer.startWriting() else { throw ZerusError.invalid("The camera could not save this recording.") }
                    writer.startSession(atSourceTime: time)
                    start = time
                    if let startedHandler { Task { @MainActor in startedHandler() } }
                } catch {
                    finish(error: error)
                    return
                }
            }
            if let videoInput, videoInput.isReadyForMoreMediaData { videoInput.append(sampleBuffer) }
            guard let start else { return }
            let elapsed = Int(CMTimeGetSeconds(CMTimeSubtract(time, start)) * 1000)
            if let progressHandler { Task { @MainActor in progressHandler(elapsed) } }
            if elapsed >= VideoRecordingClock.limitMs { requestStop() }
        } else if start != nil, let audioInput, audioInput.isReadyForMoreMediaData {
            audioInput.append(sampleBuffer)
        }
    }

    private func requestStop() {
        stopRequested = true
        guard completion != nil, !finishing else { return }
        guard let writer, writer.status == .writing else {
            finish(error: discarded ? nil : ZerusError.invalid("Recording stopped before the camera opened."))
            return
        }
        finishing = true
        videoInput?.markAsFinished()
        audioInput?.markAsFinished()
        writer.finishWriting { [self] in
            queue.async { self.finish(error: self.writer?.status == .completed ? nil : ZerusError.invalid("The camera could not save this recording.")) }
        }
    }

    private func finish(error: Error?) {
        finishing = true
        if session.isRunning { session.stopRunning() }
        guard let completion else { return }
        self.completion = nil
        if let error { completion.resume(throwing: error) } else { completion.resume() }
    }

    /// Reads the bounded file, its geometry, a small poster and the speech audio track.
    nonisolated static func prepare(_ file: URL) async throws -> CapturedMedia {
        let bytes = try Data(contentsOf: file)
        guard (1...VideoProtocol.maxBytes).contains(bytes.count) else { throw ZerusError.invalid("The recording exceeds 10 MiB.") }
        let asset = AVURLAsset(url: file)
        guard let time = try? await asset.load(.duration), let video = try? await asset.loadTracks(withMediaType: .video).first,
              let audio = try? await asset.loadTracks(withMediaType: .audio).first,
              let natural = try? await video.load(.naturalSize), let transform = try? await video.load(.preferredTransform) else {
            throw ZerusError.invalid("The video recording is too short or unsupported.")
        }
        let duration = Int(CMTimeGetSeconds(time) * 1000)
        let size = natural.applying(transform)
        let width = Int(abs(size.width).rounded()), height = Int(abs(size.height).rounded())
        guard (100...VideoProtocol.maxDurationMs).contains(duration), (16...1920).contains(width), (16...1920).contains(height) else {
            throw ZerusError.invalid("The video recording is too short or unsupported.")
        }
        return CapturedMedia(pcm: try speech(asset, audio), video: CapturedVideo(bytes: bytes, durationMs: duration, width: width, height: height,
                                                                                    posterBase64: await poster(asset)))
    }

    nonisolated static func poster(_ asset: AVURLAsset) async -> String {
        let generator = AVAssetImageGenerator(asset: asset)
        generator.appliesPreferredTrackTransform = true
        generator.maximumSize = CGSize(width: 160, height: 160)
        guard let image = try? await generator.image(at: .zero).image else { return "" }
        for quality in [0.7, 0.5, 0.3] {
            if let data = UIImage(cgImage: image).jpegData(compressionQuality: quality), data.count <= 8_192 { return data.base64EncodedString() }
        }
        return ""
    }

    nonisolated static func speech(_ asset: AVURLAsset, _ track: AVAssetTrack) throws -> Data {
        let reader = try AVAssetReader(asset: asset)
        let output = AVAssetReaderTrackOutput(track: track, outputSettings: [
            AVFormatIDKey: kAudioFormatLinearPCM, AVSampleRateKey: 16_000, AVNumberOfChannelsKey: 1, AVLinearPCMBitDepthKey: 16,
            AVLinearPCMIsFloatKey: false, AVLinearPCMIsBigEndianKey: false, AVLinearPCMIsNonInterleaved: false,
        ])
        guard reader.canAdd(output) else { throw ZerusError.invalid("Unsupported recording audio format.") }
        reader.add(output)
        guard reader.startReading() else { throw ZerusError.invalid("Preparing the recording's audio failed.") }
        var pcm = Data()
        while pcm.count < 1_920_000, let sample = output.copyNextSampleBuffer() {
            guard let block = CMSampleBufferGetDataBuffer(sample) else { continue }
            let length = min(CMBlockBufferGetDataLength(block), 1_920_000 - pcm.count)
            guard length > 0 else { continue }
            var chunk = Data(count: length)
            let status = chunk.withUnsafeMutableBytes { CMBlockBufferCopyDataBytes(block, atOffset: 0, dataLength: length, destination: $0.baseAddress!) }
            guard status == kCMBlockBufferNoErr else { throw ZerusError.invalid("Preparing the recording's audio failed.") }
            pcm.append(chunk)
        }
        reader.cancelReading()
        pcm.count -= pcm.count % 2
        guard pcm.count >= 3_200 else { throw ZerusError.invalid("No speech audio was recorded.") }
        return pcm
    }
}
