import Foundation
import ZerusKit

extension ConversationModel {
    /// Bounded display repair, separate from read-head observation, alias read marks and outbox settlement.
    func scheduleProjection(_ target: Target) {
        guard let window = historyWindows[target.key], let head = projectionHeads[target.key], head.epoch == window.epoch,
              head.generation > window.projectionApplied, projectionTasks[target.key] == nil, projectionTasks.count < 4 else { return }
        func supported() -> Bool {
            app.connections.contains { $0.id == target.connectionId } &&
                HistoryProjection.supported(relay: app.relayOperations[target.connectionId], native: app.machineOperations[target.machineKey])
        }
        guard supported() else { return }
        let epoch = window.epoch, start = window.projectionApplied, until = head.generation, token = UUID()
        let task = Task { [weak self] in
            guard let self else { return }
            defer { if self.projectionTasks[target.key]?.token == token { self.projectionTasks[target.key] = nil } }
            var progress = HistoryProjection.Run(start: start, until: until)
            do {
                while progress.canRead {
                    guard supported(), self.historyWindows[target.key]?.epoch == epoch,
                          let connection = self.app.connections.first(where: { $0.id == target.connectionId }) else { return }
                    let page = try await self.app.client.historyProjection(connection, target: target, epoch: epoch, after: progress.after, until: until)
                    let applied = page.applied(after: progress.after), entries = page.prefix
                    var committed: HistoryWindow?
                    // Rebase against every intervening page/inspection publication, never the captured run window.
                    while let latest = self.historyWindows[target.key] {
                        try Task.checkCancellation()
                        let updated = await Task.detached(priority: .utility) {
                            HistoryProjection.apply(latest, target: target, epoch: epoch, start: start, entries: entries, applied: applied)
                        }.value
                        guard let updated, supported() else { return }
                        if self.historyWindows[target.key] != latest { continue }
                        self.historyWindows[target.key] = updated
                        committed = updated
                        break
                    }
                    guard let updated = committed else { return }
                    if let frame = self.activeFrame, frame.target == target, self.selected?.target == target {
                        let revision = self.presentationRevision
                        let messages = self.normalizedOutgoing.filter { $0.belongs(to: target) }
                        let questions = self.questions
                        let display = await Task.detached(priority: .userInitiated) {
                            target.archiveId.isEmpty ? ConversationMerge.merge(target: target, native: updated.events, outgoing: messages).events : updated.events
                        }.value
                        if self.activeFrame === frame, self.historyWindows[target.key] == updated, self.selected?.target == target,
                           self.presentationRevision == revision, self.normalizedOutgoing.filter({ $0.belongs(to: target) }) == messages, self.questions == questions {
                            self.presentationTask?.cancel()
                            self.presentationRevision += 1
                            let next = frame.with(events: updated.events, truncated: frame.truncated, window: updated,
                                                  indexComplete: frame.indexComplete, indexing: frame.indexing, unreadBoundary: frame.unreadBoundary)
                            next.presentation = .init(messages: messages, events: display, questions: questions)
                            self.activeFrame = next
                            self.rememberFrame(next)
                            self.events = display
                            self.revision += 1
                        }
                    }
                    // Each encrypted slice keeps rows and the marker in the same atomic value.
                    if let pages = self.pages, let latest = self.historyWindows[target.key], latest.epoch == epoch {
                        let saved = self.readState.record(target)?.viewport?.cursor ?? ""
                        let anchor = latest.events.contains(where: { $0.historyCursor == saved }) ? saved : latest.events.last(where: { !$0.historyCursor.isEmpty })?.historyCursor
                        if let anchor, let retained = CachedHistoryPage.retained(latest, anchor: anchor) {
                            await pages.write(target, mode: "around", cursor: saved.isEmpty ? anchor : saved, page: retained)
                        }
                    }
                    progress.accepted(page)
                }
            } catch {
                // No acknowledgement of errors or unresolved entries; a later verified page resumes the prefix.
            }
        }
        projectionTasks[target.key] = (target, token, task)
    }
}
