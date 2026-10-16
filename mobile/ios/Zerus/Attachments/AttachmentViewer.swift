import Foundation
import Observation
import SwiftUI
import ZerusKit

struct AttachmentRowState: Equatable {
    var loading = false
    var progress: Double?
    var error = ""
}

/// A verified private copy ready for Quick Look, which also offers Share and Save to Files.
struct OpenedAttachment: Identifiable, Equatable {
    let id = UUID()
    let key: String
    let file: URL
    let name: String
    let mime: String
}

/// Opens message attachments: a draft or outbox file from this phone's sealed store, else the
/// exact bytes from the owning computer over `attachment_read`. Reads are never retried
/// automatically; tapping a failed row tries again.
@MainActor
@Observable
final class AttachmentViewer {
    @ObservationIgnored private unowned let app: AppModel
    @ObservationIgnored let store: AttachmentStore?
    @ObservationIgnored let cache: AttachmentViewCache?
    private(set) var rows: [String: AttachmentRowState] = [:]
    var opened: OpenedAttachment?
    @ObservationIgnored private var tasks: [String: Task<Void, Never>] = [:]

    init(app: AppModel, store: AttachmentStore?, cache: AttachmentViewCache?) {
        self.app = app
        self.store = store
        self.cache = cache
    }

    func state(_ target: Target?, _ file: DisplayedAttachment) -> AttachmentRowState? { rows[AttachmentIdentity.rowKey(target, file)] }

    func open(_ target: Target?, _ file: DisplayedAttachment) {
        let key = AttachmentIdentity.rowKey(target, file)
        guard tasks[key] == nil else { return }
        rows[key] = AttachmentRowState(loading: true)
        tasks[key] = Task { [weak self] in
            guard let self else { return }
            defer { tasks[key] = nil }
            do {
                let entry = try await load(key, target, file)
                try Task.checkCancellation()
                rows[key] = nil
                opened = OpenedAttachment(key: key, file: entry.file, name: file.name, mime: file.mime.isEmpty ? entry.mime : file.mime)
            } catch is CancellationError {
                rows[key] = nil
            } catch {
                rows[key] = AttachmentRowState(error: Self.message(error))
            }
        }
    }

    /// The verified private copy of one attachment, from this phone's sealed store or the owning computer.
    func fetch(_ target: Target?, _ file: DisplayedAttachment) async throws -> AttachmentViewCache.Entry {
        try await load(AttachmentIdentity.rowKey(target, file), target, file)
    }

    private func load(_ key: String, _ target: Target?, _ file: DisplayedAttachment) async throws -> AttachmentViewCache.Entry {
        guard let cache else { throw AttachmentOpenError("Attachment storage on this phone is unavailable.") }
        let name = AttachmentViewing.name(file.name, mime: file.mime)
        if let local = AttachmentIdentity.local(file), let store {
            do {
                return try await Task.detached(priority: .userInitiated) { try store.open(local, cache: cache, name: name) }.value
            } catch {
                if !AttachmentIdentity.remote(file) || target == nil { throw error }
            }
        }
        let access = target.map { app.attachmentAccess($0, operation: AttachmentIdentity.operation) } ?? .unknown
        if let reason = AttachmentIdentity.unavailableReason(target, file, access) { throw AttachmentOpenError(reason) }
        guard let target else { throw AttachmentOpenError("This attachment cannot be opened from the phone.") }
        let remoteKey = AttachmentIdentity.remoteKey(target, file)
        if let cached = await Task.detached(priority: .userInitiated, operation: { cache.lookup(remoteKey) }).value { return cached }
        guard let connection = app.connections.first(where: { $0.id == target.connectionId }) else {
            throw AttachmentOpenError("Reconnect to this gateway, then try again.")
        }
        var named = file
        named.name = name
        let client = app.client
        return try await AttachmentDownloads.load(cache, key: remoteKey, target: target, file: named, read: { id, payload in
            try await client.waitForReceipt(connection, initial: client.submit(connection, target: target, operation: AttachmentIdentity.operation, payload: payload, id: id),
                                            maxPolls: 150, pollDelay: .milliseconds(300))
        }, progress: { [weak self] value in
            await MainActor.run { if self?.tasks[key] != nil { self?.rows[key] = AttachmentRowState(loading: true, progress: value) } }
        })
    }

    static func message(_ error: Error) -> String {
        switch error {
        case let failure as AttachmentOpenError: failure.message
        case let failure as ZerusError: failure.message
        case let failure as RelayError: AttachmentIdentity.relayFailure(failure.status)
        case is URLError: "The gateway connection was interrupted. Try again."
        default: "The attachment could not be opened. Try again."
        }
    }

    /// Removes every decrypted copy, for example when a gateway is disconnected.
    func clear() {
        tasks.values.forEach { $0.cancel() }
        tasks = [:]
        rows = [:]
        opened = nil
        if let cache { Task.detached(priority: .utility) { cache.clear() } }
    }
}

enum ByteCount {
    static func text(_ bytes: Int64) -> String {
        if bytes >= 1_048_576 { return String(format: "%.1f MiB", locale: Locale(identifier: "en_US_POSIX"), Double(bytes) / 1_048_576) }
        if bytes >= 1024 { return "\((bytes + 1023) / 1024) KiB" }
        return "\(bytes) bytes"
    }
}

/// One attachment row in a message; tapping opens it, and a failed row retries on tap.
struct AttachmentRow: View {
    @Environment(AppModel.self) private var app
    let target: Target?
    let file: DisplayedAttachment

    var body: some View {
        let state = app.viewer.state(target, file)
        Button { app.viewer.open(target, file) } label: {
            HStack(spacing: 8) {
                Image(systemName: Self.icon(file.mime)).foregroundStyle(Theme.muted).frame(width: 20)
                VStack(alignment: .leading, spacing: 2) {
                    Text(file.name).lineLimit(1).truncationMode(.middle).foregroundStyle(.primary)
                    if let state, !state.error.isEmpty {
                        Text(state.error).foregroundStyle(Theme.amber).lineLimit(3)
                    } else if file.bytes > 0 {
                        Text(ByteCount.text(file.bytes)).foregroundStyle(Theme.muted)
                    }
                }
                Spacer(minLength: 4)
                if let state, state.loading {
                    if let progress = state.progress {
                        ProgressView(value: progress).frame(width: 44)
                    } else {
                        ProgressView().controlSize(.small)
                    }
                } else if state?.error.isEmpty == false {
                    Image(systemName: "arrow.clockwise").foregroundStyle(Theme.muted)
                }
            }
            .font(.footnote)
            .padding(.horizontal, 10)
            .padding(.vertical, 7)
            .background(Color(hex: 0x243136), in: RoundedRectangle(cornerRadius: 12))
        }
        .buttonStyle(.plain)
        .disabled(state?.loading == true)
        .accessibilityLabel("Attachment \(file.name)")
        .accessibilityHint(state?.error.isEmpty == false ? "Try again" : "Opens the file")
    }

    static func icon(_ mime: String) -> String {
        if mime.hasPrefix("image/") { return "photo" }
        if mime.hasPrefix("video/") { return "film" }
        if mime.hasPrefix("audio/") { return "waveform" }
        if mime == "application/pdf" { return "doc.richtext" }
        if mime.hasPrefix("text/") || mime.contains("json") { return "doc.text" }
        return "doc"
    }
}
