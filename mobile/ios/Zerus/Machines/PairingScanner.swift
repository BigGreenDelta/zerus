import AVFoundation
import SwiftUI
import ZerusKit

/// Scans a `zerus://pair` QR code. Scanning only fills the form; pairing stays an
/// explicit action. Camera frames and codes are never saved or logged.
struct PairingScanner: View {
    @Environment(\.dismiss) private var dismiss
    let onInvite: (PairingInvite) -> Void
    @State private var status: AVAuthorizationStatus = AVCaptureDevice.authorizationStatus(for: .video)
    @State private var rejected = false

    var body: some View {
        NavigationStack {
            ZStack {
                Color.black.ignoresSafeArea()
                switch status {
                case .authorized:
                    QRCaptureView { value in
                        guard let invite = PairingInvite.parse(value) else {
                            rejected = true
                            return false
                        }
                        onInvite(invite)
                        dismiss()
                        return true
                    }
                    .ignoresSafeArea()
                    VStack {
                        Spacer()
                        Text(rejected ? "This code is not a Zerus pairing invitation." : "Point the camera at the invitation QR code.")
                            .font(.callout)
                            .padding(12)
                            .background(.ultraThinMaterial, in: RoundedRectangle(cornerRadius: 12))
                            .padding(.bottom, 40)
                    }
                case .notDetermined:
                    ProgressView()
                        .task {
                            _ = await AVCaptureDevice.requestAccess(for: .video)
                            status = AVCaptureDevice.authorizationStatus(for: .video)
                        }
                default:
                    ContentUnavailableView {
                        Label("Camera access is off", systemImage: "camera")
                    } description: {
                        Text("Allow camera access in Settings to scan an invitation, or enter the gateway and code manually.")
                    } actions: {
                        Button("Open Settings") {
                            if let url = URL(string: UIApplication.openSettingsURLString) { UIApplication.shared.open(url) }
                        }
                    }
                }
            }
            .navigationTitle("Scan invitation")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar { ToolbarItem(placement: .cancellationAction) { Button("Cancel") { dismiss() } } }
        }
    }
}

/// One scanner lifetime: a dismissed scanner cannot publish an old frame.
private struct QRCaptureView: UIViewRepresentable {
    /// Returns true when the value was accepted and scanning should stop.
    let onCode: @MainActor (String) -> Bool

    func makeCoordinator() -> Coordinator { Coordinator(onCode: onCode) }

    func makeUIView(context: Context) -> PreviewView {
        let view = PreviewView()
        context.coordinator.start(in: view)
        return view
    }

    func updateUIView(_ uiView: PreviewView, context: Context) {}

    static func dismantleUIView(_ uiView: PreviewView, coordinator: Coordinator) { coordinator.stop() }

    final class PreviewView: UIView {
        override class var layerClass: AnyClass { AVCaptureVideoPreviewLayer.self }
        var previewLayer: AVCaptureVideoPreviewLayer { layer as! AVCaptureVideoPreviewLayer }
    }

    final class Coordinator: NSObject, AVCaptureMetadataOutputObjectsDelegate, @unchecked Sendable {
        private let session = AVCaptureSession()
        private let queue = DispatchQueue(label: "app.zerus.mobile.qr")
        private let onCode: @MainActor (String) -> Bool
        private var delivered = false

        init(onCode: @escaping @MainActor (String) -> Bool) { self.onCode = onCode }

        @MainActor func start(in view: PreviewView) {
            guard let device = AVCaptureDevice.default(for: .video), let input = try? AVCaptureDeviceInput(device: device),
                  session.canAddInput(input) else { return }
            session.addInput(input)
            let output = AVCaptureMetadataOutput()
            guard session.canAddOutput(output) else { return }
            session.addOutput(output)
            output.setMetadataObjectsDelegate(self, queue: .main)
            output.metadataObjectTypes = [.qr]
            view.previewLayer.session = session
            view.previewLayer.videoGravity = .resizeAspectFill
            let session = self.session
            queue.async { session.startRunning() }
        }

        func stop() {
            let session = self.session
            queue.async { session.stopRunning() }
        }

        nonisolated func metadataOutput(_ output: AVCaptureMetadataOutput, didOutput objects: [AVMetadataObject], from connection: AVCaptureConnection) {
            guard let value = (objects.first as? AVMetadataMachineReadableCodeObject)?.stringValue,
                  value.count <= PairingInvite.maxLength else { return }
            MainActor.assumeIsolated {
                guard !delivered else { return }
                if onCode(value) {
                    delivered = true
                    stop()
                }
            }
        }
    }
}
