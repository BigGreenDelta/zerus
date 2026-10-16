import Foundation
import ZerusKit

/// Terminal reads the existing native pane through bounded snapshots. Input is a separate
/// durable composition pinned to a fresh binding, and an uncertain input is never replayed.
extension ConversationModel {
    func terminalOpenReason(_ target: Target) -> String {
        if selected?.target != target || !target.agentId.isEmpty || !target.archiveId.isEmpty { return "Terminal is available only for the live parent session." }
        let row = activity.flatMap { $0.string("run_id") == target.run && $0.string("conversation_id") == target.conversation ? $0 : nil }
            ?? app.sessions.first { $0.target == target }?.raw
        let native = SessionActionsMenu.nativeTerminalReason(target, row)
        if !native.isEmpty { return native }
        if app.relayOperations[target.connectionId]?.contains("terminal_snapshot") != true || app.machineOperations[target.machineKey]?.contains("terminal_snapshot") != true {
            return "This native adapter does not provide remote Terminal."
        }
        return ""
    }

    func terminalCurrent(_ target: Target, binding: String) -> Bool {
        guard let raw = terminalState, selected?.target == target, terminalVisible, terminalVerified,
              (0...5).contains(ProcessInfo.processInfo.systemUptime - terminalReceivedAt) else { return false }
        return raw.string("name") == target.session && raw.string("run_id") == target.run && raw.string("conversation_id") == target.conversation &&
            raw.string("terminal_binding_id") == binding && !binding.isEmpty
    }

    func terminalFresh(_ target: Target) -> Bool { terminalCurrent(target, binding: terminalState?.string("terminal_binding_id") ?? "") }

    func terminalReason(_ target: Target) -> String {
        let unavailable = terminalOpenReason(target)
        if !unavailable.isEmpty { return unavailable }
        if !app.storageReady || app.relayOperations[target.connectionId]?.contains("terminal_input") != true ||
            app.machineOperations[target.machineKey]?.contains("terminal_input") != true { return "This computer does not support verified Terminal input." }
        if !terminalFresh(target) { return "Refresh Terminal before sending input." }
        if contextBlocked(target) || preparingTargets.contains(target.key) || normalizedOutgoing.contains(where: { $0.belongs(to: target) && $0.blocksSending }) ||
            attachmentImporting || attachmentPreparing { return "Resolve the pending delivery before typing into Terminal." }
        return ""
    }

    func setTerminalVisible(_ target: Target, _ visible: Bool) {
        guard selected?.target == target else { return }
        terminalVisible = visible
        if visible {
            terminalSnapshot(target)
        } else {
            terminalTask?.cancel()
            terminalLoading = false
            terminalVerified = false
            activityVerified = false
            activeFrameVerified = false
            refreshActivity()
        }
    }

    func terminalSnapshot(_ target: Target) {
        let reason = terminalOpenReason(target)
        guard reason.isEmpty else {
            terminalError = reason
            return
        }
        guard !terminalLoading, let connection = app.connections.first(where: { $0.id == target.connectionId }) else { return }
        let navigation = navigationId
        terminalLoading = true
        terminalError = ""
        let client = app.client
        terminalTask = Task { [weak self] in
            do {
                let id = UUID().uuidString.lowercased()
                var payload = try target.json()
                payload["request_id"] = .string(id)
                let receipt = try await client.waitForReceipt(connection, initial: client.submit(connection, target: target, operation: "terminal_snapshot", payload: payload, id: id),
                                                              maxPolls: 50, pollDelay: .milliseconds(100))
                guard receipt.string("state") == "completed", let result = receipt.object("result") else {
                    throw ZerusError(.unavailable, receipt.string("error").nonEmpty ?? "Terminal is not available yet.")
                }
                guard result.string("request_id") == id, result.string("name") == target.session, result.string("run_id") == target.run,
                      result.string("conversation_id") == target.conversation, !result.string("terminal_binding_id").isEmpty,
                      result.string("screen").utf8.count <= 512 * 1024 else { throw ZerusError.mismatch("Computer returned a different Terminal binding.") }
                guard let self, navigationId == navigation else { return }
                terminalLoading = false
                guard selected?.target == target, terminalVisible else { return }
                terminalState = result
                terminalVerified = true
                terminalReceivedAt = ProcessInfo.processInfo.systemUptime
            } catch {
                guard let self, navigationId == navigation else { return }
                terminalLoading = false
                if error is CancellationError { return }
                if selected?.target == target {
                    terminalError = AppModel.message(error, fallback: "Terminal is not available yet.")
                    terminalVerified = false
                }
            }
        }
    }

    func terminalBuffer(_ target: Target) -> TerminalBuffer? {
        guard let raw = terminalState, raw.string("name") == target.session, raw.string("run_id") == target.run,
              raw.string("conversation_id") == target.conversation else { return nil }
        let binding = raw.string("terminal_binding_id")
        guard !binding.isEmpty else { return nil }
        let key = target.key + ":terminal:" + binding
        if let saved = messageState.terminalBuffers.first(where: { $0.key == key }) { return saved }
        if let empty = emptyTerminalBuffers[key] { return empty }
        let created = TerminalBuffer(target: target, binding: binding)
        emptyTerminalBuffers[key] = created
        return created
    }

    func editTerminal(_ target: Target, _ text: String) {
        guard let buffer = terminalBuffer(target), app.storageReady, text.utf8.count <= 65_536, text != buffer.text else { return }
        var editor = buffer
        editor.text = text
        editor.revision = UUID().uuidString.lowercased()
        let next = editor
        app.editMessages { TerminalBuffers.edit($0, next) }
    }

    func restoreTerminalInput(_ action: SessionAction) {
        guard action.operation == "terminal_input", ["failed", "reviewed"].contains(action.status), let current = terminalBuffer(action.target) else { return }
        guard action.arguments.string("terminal_binding_id") == current.binding, action.arguments.has("text"), current.text.isEmpty else {
            terminalError = "The original Terminal input remains saved. Restore it only into its empty matching Terminal buffer."
            return
        }
        editTerminal(action.target, action.arguments.string("text"))
        app.flushPrivateState()
    }

    /// Sends buffered text (with or without Enter) or one fixed key to the verified binding.
    func terminalInput(_ target: Target, key: String? = nil, enter: Bool = false) {
        let reason = terminalReason(target)
        guard reason.isEmpty else {
            terminalError = reason
            return
        }
        guard let buffer = terminalBuffer(target) else { return }
        var args: JSONObject = ["terminal_binding_id": .string(buffer.binding)]
        if let key {
            args["key"] = .string(key)
        } else {
            args["text"] = .string(buffer.text)
            args["enter"] = .bool(enter)
        }
        submitSessionAction(target, "terminal_input", args, terminalBuffer: key == nil ? buffer : nil)
    }
}
