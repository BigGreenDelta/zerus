import Foundation
import ZerusKit

/// One picked item, read only after the durable selection names its draft.
enum PickedSource: Sendable {
    case file(URL, name: String?, mime: String?, scoped: Bool, temporary: Bool)
    case data(Data, name: String, mime: String)
}

/// File selection survives termination: the originating draft is durable before any picker
/// opens, imports are sealed copies, and only files no draft or outbox references are removed.
extension ConversationModel {
    func attachmentReason(_ target: Target) -> String {
        let raw = activity.flatMap { $0.string("run_id") == target.run && $0.string("conversation_id") == target.conversation ? $0 : nil }
            ?? app.sessions.first { $0.target == target }?.raw
        return NativeAttachments.reason(target, raw)
    }

    /// Why files cannot be chosen now, or "" when the attach control is available.
    func attachBlockReason(_ target: Target) -> String {
        if demo { return "Pair to attach files." }
        if !app.storageReady || app.attachmentStore == nil { return "Private storage is not ready." }
        if !target.archiveId.isEmpty { return "Archived conversations are read-only." }
        let child = childSendReason(target)
        if !child.isEmpty { return child }
        let native = attachmentReason(target)
        if !native.isEmpty { return native }
        let current = draft(target)
        if current.status != "editing" { return "Review the saved draft before adding files." }
        if messageState.pickerDraft != nil || attachmentPreparing || attachmentImporting { return "Wait for the selected files to finish importing." }
        if preparingTargets.contains(target.key) { return "Preparing your message. Try again in a moment." }
        if current.attachments.count >= AttachmentPolicy.maxFiles { return "Attach at most 8 files." }
        return ""
    }

    /// Saves the originating draft and selection identity before a picker opens.
    func beginAttachmentPick(_ target: Target) async -> String? {
        guard attachBlockReason(target).isEmpty else { return nil }
        if continuation?.draft.target == target {
            cancelCompactContinuation("Selecting files canceled the automatic continuation. Your composition is preserved.")
        }
        let origin = draft(target)
        guard origin.status == "editing" else { return nil }
        let id = UUID().uuidString.lowercased()
        save(origin)
        attachmentPreparing = true
        preparingTargets.insert(target.key)
        defer {
            attachmentPreparing = false
            preparingTargets.remove(target.key)
        }
        do {
            try await app.durableMessages { state in
                var copy = state
                copy.pickerDraft = origin
                copy.pickerId = id
                return copy
            }
            return id
        } catch {
            app.error = "Could not save the file selection. Your draft is preserved."
            return nil
        }
    }

    func finishAttachmentPick(_ sources: [PickedSource], selectionId: String) {
        guard selectionId == messageState.pickerId, let origin = messageState.pickerDraft, importTask == nil, let store = app.attachmentStore else {
            Self.discardTemporary(sources)
            return
        }
        if sources.isEmpty {
            cancelAttachmentPick(selectionId)
            return
        }
        attachmentImporting = true
        importTask = Task { [weak self] in
            guard let self else { return }
            var added: [Attachment] = []
            let revision = UUID().uuidString.lowercased(), detachedId = UUID().uuidString.lowercased()
            do {
                guard origin.attachments.count + sources.count <= AttachmentPolicy.maxFiles else { throw ZerusError.invalid("Attach at most 8 files.") }
                for (index, source) in sources.enumerated() {
                    try Task.checkCancellation()
                    let current = origin.attachments + added
                    do {
                        added.append(try await Task.detached(priority: .userInitiated) { try Self.importSource(source, store: store, current: current) }.value)
                    } catch {
                        Self.discardTemporary(Array(sources[(index + 1)...]))
                        throw error
                    }
                }
                let files = added, ids = Set(added.map(\.id))
                let result = try await app.durableMessages({ state in
                    guard state.pickerId == selectionId else { throw ZerusError.invalid("File selection was canceled. The original draft is preserved.") }
                    var next = try state.selectionResult(origin, files)
                    next.drafts = next.drafts.map { draft in
                        guard draft.attachments.contains(where: { ids.contains($0.id) }) else { return draft }
                        var copy = draft
                        copy.revision = revision
                        copy.detachedId = draft.detachedId.isEmpty ? "" : detachedId
                        return copy
                    }
                    return next
                }, acknowledge: { current, written in
                    MessageChanges.selectionAcknowledged(current, origin: origin, written: written, addedIds: ids, selectionId: selectionId)
                })
                if result.drafts.contains(where: { $0.detachedId == detachedId }) {
                    app.error = "The original composition changed. Its selected files are preserved in Saved drafts."
                }
            } catch is CancellationError {
            } catch {
                app.error = (error as? ZerusError)?.message ?? "Could not import the selected files. The original draft is preserved."
            }
            if messageState.pickerId == selectionId { app.editMessages { AttachmentSelection.cancel($0, selectionId) } }
            let durable = try? await app.flushMessages()
            let protected = protectedAttachmentIds(durable)
            let orphaned = added.map(\.id).filter { !protected.contains($0) }
            if !orphaned.isEmpty {
                await Task.detached(priority: .utility) { for id in orphaned { _ = try? store.remove(id, protected: protected) } }.value
            }
            attachmentImporting = false
            importTask = nil
        }
    }

    nonisolated static func importSource(_ source: PickedSource, store: AttachmentStore, current: [Attachment]) throws -> Attachment {
        switch source {
        case .data(let data, let name, let mime):
            return try store.importData(data, name: name, mime: mime, current: current)
        case .file(let url, let name, let mime, let scoped, let temporary):
            let access = scoped && url.startAccessingSecurityScopedResource()
            defer {
                if access { url.stopAccessingSecurityScopedResource() }
                if temporary { try? FileManager.default.removeItem(at: url.deletingLastPathComponent()) }
            }
            return try store.importFile(url, name: name, mime: mime, current: current)
        }
    }

    nonisolated static func discardTemporary(_ sources: [PickedSource]) {
        for case .file(let url, _, _, _, true) in sources { try? FileManager.default.removeItem(at: url.deletingLastPathComponent()) }
    }

    /// A picker closed without a selection; a newer selection is never cleared.
    func cancelAttachmentPick(_ selectionId: String) {
        guard selectionId == messageState.pickerId, importTask == nil, !attachmentImporting else { return }
        app.editMessages { AttachmentSelection.cancel($0, selectionId) }
        app.flushPrivateState()
    }

    /// A selection saved before the app stopped cannot finish: its picker is gone.
    func recoverAttachmentPick() {
        let state = messageState
        let recovered = AttachmentSelection.recover(state, state.pickerId, launchedId: "", filesBusy: attachmentPreparing || attachmentImporting)
        guard recovered != state else { return }
        let id = state.pickerId
        app.editMessages { AttachmentSelection.recover($0, id, launchedId: "", filesBusy: false) }
        app.flushPrivateState()
        app.error = "File selection was interrupted. Your draft is preserved; choose files again."
    }

    func protectedAttachmentIds(_ durable: MessageState? = nil) -> Set<String> {
        func references(_ state: MessageState) -> [Attachment] {
            state.drafts.flatMap(\.attachments) + state.outgoing.flatMap(\.attachments) + (state.pickerDraft?.attachments ?? [])
        }
        return Set((references(messageState) + (durable.map(references) ?? [])).map(\.id))
    }

    func removeAttachment(_ target: Target, id: String) async {
        guard target.archiveId.isEmpty, messageState.pickerDraft?.target != target else { return }
        if continuation?.draft.target == target { cancelCompactContinuation() }
        let current = draft(target)
        guard current.status == "editing" else { return }
        app.editMessages { state in
            var copy = state
            copy.drafts = state.drafts.map { draft in
                guard draft.key == current.key, draft.generation == current.generation, draft.status == "editing" else { return draft }
                var updated = draft
                updated.attachments.removeAll { $0.id == id }
                return updated
            }
            return copy
        }
        do {
            let durable = try await app.flushMessages()
            let protected = protectedAttachmentIds(durable)
            if let store = app.attachmentStore { _ = try await Task.detached(priority: .utility) { try store.remove(id, protected: protected) }.value }
        } catch {
            app.error = (error as? ZerusError)?.message ?? "Could not save the draft change. Try again."
        }
    }
}
