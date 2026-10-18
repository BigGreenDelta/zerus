import Foundation
import ZerusKit

/// Private reading positions, unread evidence, Read all and shared read marks.
extension ConversationModel {
    func openReadState(_ sealed: SealedFileStore) async {
        do {
            let loaded = try await Task.detached { try sealed.read("conversation_read", as: ConversationReadState.self) ?? ConversationReadState() }.value
            let writer = OrderedStatePersistence(initial: loaded.validated()) { try sealed.write("conversation_read", $0) }
            writer.onChange = { [weak self] in self?.readState = $0 }
            writer.onResult = { [weak self] failure in
                if failure != nil {
                    self?.app.error = "Could not save private reading positions. They remain available here; restart may restore the earlier saved position."
                }
            }
            readWriter = writer
            readState = writer.state
            readTrackingReady = true
        } catch {
            app.error = "Private read positions could not be opened. They have been preserved; unread totals remain unknown."
        }
        readPositionReady = true
        if let target = selected?.target { restoreSavedWindow(target, navigation: navigationId) }
        if let frame = activeFrame, selected?.target == frame.target, let presentation = frame.presentation {
            captureUnreadBoundary(frame.target, presentation.events)
        }
        refreshReadAttention()
    }

    func editRead(_ reduce: @escaping @Sendable (ConversationReadState) -> ConversationReadState) {
        guard let readWriter else { return }
        readState = readWriter.edit(reduce)
    }

    func flushRead() { readWriter?.flushAsync() }

    func conversationViewport(_ target: Target) -> SavedConversationViewport? { readState.record(target)?.viewport }

    func saveViewport(_ target: Target, eventId: String, offset: Int) {
        guard selected?.target == target, activeFrame?.target == target, let event = events.first(where: { $0.id == eventId }) else { return }
        editRead { $0.viewport(target, eventId: eventId, offset: offset, cursor: event.historyCursor, epoch: event.historyEpoch) }
        retainViewport(target, cursor: event.historyCursor)
    }

    func visibleReadRows(_ target: Target, _ ids: Set<String>) -> [Event] {
        let visible = events.filter { ids.contains($0.id) }
        guard let window = historyWindows[target.key], window.target == target else { return visible.filter(ConversationReadPolicies.incoming) }
        return ConversationReadPolicies.incomingThroughPosition(readState.record(target)?.epoch ?? "", window.events, visible)
    }

    /// Messages that stayed visible in the conversation advance the private read position.
    func visibleMessages(_ target: Target, _ ids: Set<String>) {
        guard selected?.target == target, activeFrame?.target == target else { return }
        let incoming = visibleReadRows(target, ids)
        guard !incoming.isEmpty else { return }
        editRead { $0.visible(target, incoming) }
        refreshReadAttention()
        shareRead(target)
    }

    /// Replies this phone has loaded for the conversation: the open window, else a retained page or frame.
    func loadedRows(_ target: Target) -> [Event]? {
        if selected?.target == target { return events }
        return historyWindows[target.key]?.events ?? frames[target.key]?.events
    }

    func loadedUnreadCount(_ target: Target) -> Int? {
        guard readTrackingReady else { return nil }
        let record = readState.record(target)
        if record?.incomplete == true || (readState.records.count >= 500 && record == nil) { return nil }
        return ConversationReadPolicies.lowerBound(record, loadedRows(target))
    }

    func unreadCount(_ target: Target) -> Int? {
        guard readTrackingReady, let record = readState.record(target), record.headComplete,
              currentHistoryHeads.contains(ConversationReadPolicies.key(target)), let head = record.headIncoming else { return nil }
        return Int(clamping: max(0, head - record.readThrough))
    }

    func captureUnreadBoundary(_ target: Target, _ rows: [Event]) {
        guard !unreadBoundaryCaptured, readPositionReady else { return }
        let decision = OpeningUnreadBoundary.decide(ready: readTrackingReady, record: readState.record(target), rows: rows)
        guard decision.captured else { return }
        firstUnreadEventId = decision.eventId
        unreadBoundaryCaptured = true
    }

    func reviewLater(_ target: Target) -> Bool { readState.record(target)?.reviewLater == true }

    func captureReadAttention(_ target: Target) -> ReadAttentionEvidence {
        let record = readState.record(target)
        let raw = selected?.target == target ? activity : app.sessions.first { $0.target == target }?.raw
        let rows = loadedRows(target) ?? []
        let epoch = record?.epoch ?? ""
        return ReadAttentionEvidence(target: target, fingerprint: HistoryHeadHints.readFingerprint(target, raw, record), epoch: epoch, head: record?.headIncoming,
                                     authoritative: readTrackingReady && record?.headComplete == true && currentHistoryHeads.contains(ConversationReadPolicies.key(target)),
                                     loadedIds: Set(rows.filter(ConversationReadPolicies.incoming).map(\.id)),
                                     loadedThrough: rows.filter { ConversationReadPolicies.incoming($0) && !epoch.isEmpty && $0.historyEpoch == epoch }.compactMap(\.incomingSeq).max())
    }

    func markRead(_ target: Target, _ evidence: ReadAttentionEvidence) {
        guard evidence.target == target, readTrackingReady, captureReadAttention(target) == evidence else {
            app.error = "The displayed reply evidence changed. Review it before marking read."
            return
        }
        editRead { ReadAttentionPolicies.mark($0, evidence) }
        refreshReadAttention()
        shareRead(target)
        app.flushPrivateState()
    }

    func readAllEvidence(_ target: Target) -> ReadAttentionEvidence? {
        guard readTrackingReady, readWriter != nil, !demo, !target.run.isEmpty, !target.conversation.isEmpty,
              app.connections.contains(where: { $0.id == target.connectionId }) else { return nil }
        let row = app.sessions.first { $0.target == target }
        if row == nil && (selected?.target != target || activeFrame?.target != target) { return nil }
        var captured = captureReadAttention(target)
        let currentHint = row.map(ReadAllHeadCandidates.hint)
        let observedHint = historySnapshotHints[ConversationReadPolicies.key(target)]
        let freshHead = captured.authoritative && currentHint != nil && observedHint == currentHint && historyNetworkReason(target).isEmpty &&
            app.machines.contains { $0.key == target.machineKey && $0.online }
        captured.fingerprint += JSON.array([currentHint.map(JSON.string) ?? .null, observedHint.map(JSON.string) ?? .null]).compact
        captured.authoritative = freshHead
        return captured
    }

    func canMarkRead(_ target: Target) -> Bool { ReadAllPolicies.available(readAllEvidence(target)) }

    /// Something Read all could clear here once it has usable evidence: a reminder or replies counted unread.
    func readAllPending(_ target: Target) -> Bool {
        readTrackingReady && !demo && ReadAllPolicies.pending(readState.record(target), loadedRows(target) ?? [])
    }

    func canReadAll(_ target: Target) -> Bool {
        guard readAllPending(target) else { return false }
        let record = readState.record(target)
        return ReadAllPolicies.changes(record, readAllEvidence(target), loadedRows(target) ?? [], full: record == nil && readState.records.count >= 500)
    }

    func listReadAllCandidate(_ target: Target) -> Bool {
        guard let row = app.sessions.first(where: { $0.target == target }) else { return false }
        return ReadAllPolicies.listCandidate(row, online: app.online(row), reviewLater: reviewLater(target))
    }

    func readAllPartial(_ target: Target) -> Bool { readAllEvidence(target)?.authoritative != true }

    /// Clears only this phone's reminder; read markers stay and nothing is shared with the computer.
    func clearReviewLater(_ target: Target) {
        guard readTrackingReady, !demo, reviewLater(target) else { return }
        editRead { $0.review(target, false) }
        refreshReadAttention()
        app.flushPrivateState()
    }

    func reviewLater(_ target: Target, _ evidence: ReadAttentionEvidence) {
        guard evidence.target == target, readTrackingReady, captureReadAttention(target) == evidence else {
            app.error = "The displayed reply evidence changed. Review it again."
            return
        }
        editRead { $0.review(target, true) }
        refreshReadAttention()
        app.flushPrivateState()
    }

    /// Opening a session's actions asks the bounded head probe to check that row first.
    func prepareReadEvidence(_ target: Target) { if !demo { watchHistoryHeads(priority: target) } }

    @discardableResult
    func markAllRead(_ targets: [Target], visibleRepliesOnly: Bool = false) -> ReadAllResult {
        var seen = Set<String>()
        let distinct = targets.filter { seen.insert(ConversationReadPolicies.key($0)).inserted && (!visibleRepliesOnly || listReadAllCandidate($0)) }
        let captured = distinct.map { ReadAllCapture(target: $0, evidence: readAllEvidence($0)) }
        let current = distinct.map { ReadAllCapture(target: $0, evidence: readAllEvidence($0)) }
        guard let readWriter else { return ReadAllResult(skipped: distinct.count) }
        let applied = ReadAllPolicies.apply(readWriter.state, captured: captured, current: current)
        readState = readWriter.edit { state in ReadAllPolicies.apply(state, captured: captured, current: current).state }
        refreshReadAttention()
        readWriter.flushAsync()
        // One mark_read at a time, after any earlier Read all, so a large scope never floods the relay.
        let previous = readAllShares
        readAllShares = Task { [weak self] in
            await previous?.value
            for target in distinct {
                guard let self, let request = self.sharedReadRequest(target, acknowledge: false), self.sharedReadsSent.insert(request.key).inserted else { continue }
                await self.sendSharedRead(request)
            }
        }
        return applied.result
    }

    /// Annotates catalog rows with unread and review-later state off the main actor.
    func refreshReadAttention() {
        // Retire the opening divider once this exact current head is read; a later reply cannot resurrect it.
        if let target = selected?.target, unreadCount(target) == 0 {
            firstUnreadEventId = ""
            unreadBoundaryCaptured = true
        }
        readAttentionTask?.cancel()
        let rows = app.catalogRows, revision = app.catalogRevision, state = readState, ready = readTrackingReady, heads = currentHistoryHeads
        var loaded: [Target: [Event]] = [:]
        for frame in frames.values.values { loaded[frame.target] = frame.events }
        for window in historyWindows.values.values { loaded[window.target] = window.events }
        if let target = selected?.target { loaded[target] = events }
        let retained = loaded
        readAttentionTask = Task { [weak self] in
            let prepared = await Task.detached(priority: .utility) { SessionReadAttention.prepare(rows, state, ready: ready, heads: heads, loaded: retained) }.value
            guard let self, !Task.isCancelled, self.app.catalogRevision == revision, self.readState == state, self.currentHistoryHeads == heads else { return }
            self.app.annotateCatalog(prepared, revision: revision)
        }
    }

    struct SharedReadRequest {
        let connection: Connection
        let target: Target
        let payload: JSONObject
        let key: String
    }

    /// The `mark_read` this phone may send now; `acknowledge` also covers the current request.
    func sharedReadRequest(_ target: Target, acknowledge: Bool) -> SharedReadRequest? {
        guard !demo, readTrackingReady, let row = app.sessions.first(where: { $0.target == target }),
              app.relayOperations[target.connectionId]?.contains("mark_read") == true, app.machineOperations[target.machineKey]?.contains("mark_read") == true,
              let payload = SharedReadPolicies.payload(row, freshHead: readAllEvidence(target)?.authoritative == true, localUnread: unreadCount(target),
                                                       acknowledge: acknowledge),
              let connection = app.connections.first(where: { $0.id == target.connectionId }) else { return nil }
        return SharedReadRequest(connection: connection, target: target, payload: payload, key: target.key + "\n" + JSON.object(payload).compact)
    }

    /// A mark counts as shared only when the computer applied every field sent; a failed submission is forgotten so the
    /// next view retries.
    func sendSharedRead(_ request: SharedReadRequest) async {
        let receipt: JSONObject
        do { receipt = try await app.client.markRead(request.connection, target: request.target, payload: request.payload) }
        catch {
            sharedReadsSent.remove(request.key)
            return
        }
        guard receipt.string("state") == "completed" else {
            sharedReadsSent.remove(request.key)
            return
        }
        if SharedReadPolicies.applied(receipt, payload: request.payload) {
            sharedReadRetries.applied(request.key)
            return
        }
        // Completed without applying: the same mark may go again only after a delay, without holding Read all.
        // At capacity it stays sent, so nothing is ever resent beyond the bound.
        if sharedReadRetries.unapplied(request.key, connectionId: request.target.connectionId, now: sharedReadNow()) != nil {
            scheduleSharedReadRetries()
        }
    }

    /// Monotonic seconds that keep running while the phone sleeps, like the timer's own sleep.
    private func sharedReadNow() -> Double {
        let elapsed = ContinuousClock.now - sharedReadClock
        return Double(elapsed.components.seconds) + Double(elapsed.components.attoseconds) / 1e18
    }

    /// The one owned timer: it wakes at the earliest due mark and allows exactly the released marks to be sent again.
    /// Rescheduling cancels it; a cancelled or stale wakeup releases nothing.
    func scheduleSharedReadRetries() {
        sharedReadRetryTimer?.cancel()
        sharedReadRetryTimer = nil
        guard let due = sharedReadRetries.nextDue else { return }
        let wait = max(0, due - sharedReadNow())
        sharedReadRetryTimer = Task { [weak self] in
            try? await Task.sleep(for: .seconds(wait))
            guard !Task.isCancelled, let self else { return }
            for key in self.sharedReadRetries.release(now: self.sharedReadNow()) { self.sharedReadsSent.remove(key) }
            self.scheduleSharedReadRetries()
        }
    }

    /// Tells the session's computer that this phone showed its current reply.
    func shareRead(_ target: Target) {
        guard let request = sharedReadRequest(target, acknowledge: true), sharedReadsSent.insert(request.key).inserted else { return }
        Task { await sendSharedRead(request) }
    }

    /// A reply read on another device advances this phone's watermark, using only a fresh exact head.
    func adoptSharedReads() {
        guard let readWriter, !demo, readTrackingReady else { return }
        let captured = app.sessions.filter { SharedReadPolicies.replyRead($0) && (unreadCount($0.target) ?? 0) > 0 }.compactMap { row in
            readAllEvidence(row.target).flatMap { $0.authoritative ? ReadAllCapture(target: row.target, evidence: $0) : nil }
        }
        guard !captured.isEmpty else { return }
        readState = readWriter.edit { ReadAllPolicies.apply($0, captured: captured, current: captured).state }
        refreshReadAttention()
    }
}
