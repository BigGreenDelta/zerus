import Foundation
import Observation
import QuickLook
import SwiftUI
import UIKit
import ZerusKit

struct ComputerFileSelection: Identifiable, Equatable {
    let id = UUID().uuidString.lowercased()
    let target: Target
    let reference: ComputerFileReference
    let computer: String
    var busy = false
    var progress: Double?
    var error = ""
}

/// A Markdown file link opened on the conversation's own computer. The selected link keeps its
/// original computer and conversation; reading and saving require an explicit choice.
@MainActor
@Observable
final class ComputerFiles {
    @ObservationIgnored private unowned let app: AppModel
    @ObservationIgnored let cache: AttachmentViewCache?
    var selection: ComputerFileSelection?
    var preview: URL?
    var export: URL?
    var notice = ""
    private(set) var clearing = false
    @ObservationIgnored private var task: Task<Void, Never>?

    init(app: AppModel, cache: AttachmentViewCache?) {
        self.app = app
        self.cache = cache
    }

    func show(_ target: Target, _ reference: ComputerFileReference) {
        task?.cancel()
        preview = nil
        export = nil
        selection = ComputerFileSelection(target: target, reference: reference, computer: app.machineName(target.machineKey, fallback: target.computerId))
    }

    func close() {
        task?.cancel()
        task = nil
        selection = nil
        preview = nil
        export = nil
    }

    func open(save: Bool) {
        guard let chosen = selection, !chosen.busy, export == nil, !clearing else { return }
        selection?.busy = true
        selection?.error = ""
        task = Task { [weak self] in
            guard let self else { return }
            do {
                let entry = try await load(chosen)
                guard selection?.id == chosen.id else { return }
                selection?.busy = false
                selection?.progress = nil
                if save { export = entry.file } else { preview = entry.file }
            } catch {
                guard selection?.id == chosen.id else { return }
                selection?.busy = false
                selection?.progress = nil
                selection?.error = error is CancellationError ? "" : Self.message(error)
            }
        }
    }

    private func load(_ chosen: ComputerFileSelection) async throws -> AttachmentViewCache.Entry {
        let target = chosen.target
        guard let cache else { throw AttachmentOpenError("Attachment storage on this phone is unavailable.") }
        if target.connectionId == "demo" { throw AttachmentOpenError("Computer files are not available in the demo.") }
        if target.run.isEmpty || AttachmentIdentity.conversation(target).isEmpty {
            throw AttachmentOpenError("Refresh this conversation, then open the file again.")
        }
        switch app.attachmentAccess(target, operation: ComputerFileProtocol.operation) {
        case .unsupported: throw AttachmentOpenError(ComputerFileProtocol.updateRequired)
        case .offline: throw AttachmentOpenError("This computer is offline. Connect it and try again.")
        case .unknown: throw AttachmentOpenError("Refresh this computer's connection, then try again.")
        case .supported: break
        }
        guard let connection = app.connections.first(where: { $0.id == target.connectionId }) else {
            throw AttachmentOpenError("Reconnect to the gateway, then try again.")
        }
        let client = app.client, id = chosen.id
        return try await ComputerFileDownloads.load(cache, target: target, reference: chosen.reference, read: { requestId, payload in
            try await client.waitForReceipt(connection, initial: client.submit(connection, target: target, operation: ComputerFileProtocol.operation,
                                                                               payload: payload, id: requestId), maxPolls: 150, pollDelay: .milliseconds(300))
        }, progress: { [weak self] value in
            await MainActor.run { if self?.selection?.id == id { self?.selection?.progress = value } }
        }).entry
    }

    static func message(_ error: Error) -> String {
        let text = AttachmentViewer.message(error)
        return text == "The attachment could not be opened. Try again." ? "The file could not be downloaded. Try again." : String(text.prefix(300))
    }

    func exported(_ success: Bool) {
        guard let file = export else { return }
        export = nil
        if success { notice = "Saved \(file.lastPathComponent)" }
    }

    /// Forgetting a gateway removes private copies after owned reads have stopped.
    func clear() {
        task?.cancel()
        let reading = task
        task = nil
        selection = nil
        preview = nil
        export = nil
        guard let cache, !clearing else { return }
        clearing = true
        Task { [weak self] in
            await reading?.value
            await Task.detached(priority: .utility) { cache.clear() }.value
            self?.clearing = false
        }
    }
}

/// Details of a tapped file link with explicit Open and Save choices.
struct ComputerFileSheet: View {
    @Environment(AppModel.self) private var app
    let selection: ComputerFileSelection

    var body: some View {
        let files = app.computerFiles!
        let chosen = files.selection ?? selection
        NavigationStack {
            VStack(alignment: .leading, spacing: 14) {
                HStack(alignment: .top) {
                    Image(systemName: "doc").font(.title2).foregroundStyle(Theme.mint)
                    Text(chosen.reference.name).font(.headline).lineLimit(2)
                    Spacer()
                    Button {
                        UIPasteboard.general.string = chosen.reference.path
                        app.notice = "File path copied"
                    } label: { Image(systemName: "doc.on.doc") }
                    .accessibilityLabel("Copy file path")
                }
                Text("On \(chosen.computer)").font(.subheadline).foregroundStyle(Theme.muted)
                Text(verbatim: chosen.reference.path + chosen.reference.location).font(.footnote.monospaced()).textSelection(.enabled)
                if chosen.busy {
                    VStack(alignment: .leading, spacing: 4) {
                        Text("Downloading…").font(.footnote).foregroundStyle(Theme.muted)
                        if let progress = chosen.progress { ProgressView(value: progress) } else { ProgressView().frame(maxWidth: .infinity, alignment: .leading) }
                    }
                }
                if !chosen.error.isEmpty { Text(chosen.error).font(.footnote).foregroundStyle(Theme.amber) }
                Text("The file is read from this conversation's computer, verified and kept on this phone for up to a day.")
                    .font(.caption).foregroundStyle(Theme.muted)
                HStack(spacing: 12) {
                    Button("Open") { files.open(save: false) }
                        .buttonStyle(.borderedProminent)
                    Button("Save…") { files.open(save: true) }
                        .buttonStyle(.bordered)
                }
                .disabled(chosen.busy || files.export != nil || files.clearing)
                Spacer()
            }
            .padding()
            .navigationTitle("Computer file")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) { Button(chosen.busy ? "Cancel" : "Close") { files.close() } }
            }
            .zerusScreen()
        }
        .presentationDetents([.medium, .large])
        .quickLookPreview(Binding(get: { files.preview }, set: { files.preview = $0 }))
        .sheet(isPresented: Binding(get: { files.export != nil }, set: { if !$0 { files.exported(false) } })) {
            if let file = files.export {
                DocumentExporter(file: file) { files.exported($0) }.ignoresSafeArea()
            }
        }
    }
}

/// Saves a verified private copy to a location the person picks in Files.
struct DocumentExporter: UIViewControllerRepresentable {
    let file: URL
    let finished: (Bool) -> Void

    func makeCoordinator() -> Coordinator { Coordinator(finished: finished) }

    func makeUIViewController(context: Context) -> UIDocumentPickerViewController {
        let picker = UIDocumentPickerViewController(forExporting: [file], asCopy: true)
        picker.delegate = context.coordinator
        return picker
    }

    func updateUIViewController(_ controller: UIDocumentPickerViewController, context: Context) {}

    final class Coordinator: NSObject, UIDocumentPickerDelegate {
        let finished: (Bool) -> Void
        init(finished: @escaping (Bool) -> Void) { self.finished = finished }
        func documentPicker(_ controller: UIDocumentPickerViewController, didPickDocumentsAt urls: [URL]) { finished(!urls.isEmpty) }
        func documentPickerWasCancelled(_ controller: UIDocumentPickerViewController) { finished(false) }
    }
}
