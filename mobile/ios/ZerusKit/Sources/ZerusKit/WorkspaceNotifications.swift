import Foundation

/// Relay hints wake a refresh; current authenticated native facts grant every alert.
enum WorkspaceNotifications {
    private struct Key: Hashable { let computer: String; let scope: String }
    private struct Scope { let session: String; let kind: String?; let completedAt: Double?; let replyId: String }

    static func events(_ connection: Connection, records: [JSONObject], page: JSONObject, after: Int64, now: Double) throws -> JSONObject {
        let current = Dictionary(grouping: NotificationCatalogParser.parse(connection, records: records, aliases: []).sessions, by: { $0.slot.key })
        var scopes: [Key: Scope] = [:], ambiguous = Set<Key>()
        for computer in records { for session in NativeSessions.sessions(connection, computer) {
            let raw = session.raw, scope = raw.string("relay_scope_id")
            if scope.isEmpty || session.target.session.isEmpty { continue }
            let key = Key(computer: computer.string("id"), scope: scope)
            if scopes[key] != nil { ambiguous.insert(key); continue }
            let notices = current[NotificationSlot(connection: connection.id, computer: computer.string("id"), session: session.target.session).key] ?? []
            let notice = notices.count == 1 ? notices.first : nil
            let active = notice != nil && notice?.live == true && notice?.online == true && notice?.suppressed == false && notice?.target.isEmpty == false &&
                SessionFilters.currentActivity(session, true) && raw.string("run_id") == session.target.run && raw.string("conversation_id") == session.target.conversation
            var kind: String?
            if active && !SharedReadPolicies.attentionAcknowledged(session) {
                switch notice?.kind { case .input, .optional: kind = "attention"; case .error: kind = "error"; default: break }
            }
            let replyAt = raw.double("reply_at", default: 0), replyId = raw.string("reply_id")
            let completion = active && notice?.kind == nil && notice?.idle == true && (1...256).contains(replyId.utf8.count) &&
                !replyId.unicodeScalars.contains(where: { $0.properties.generalCategory == .control }) &&
                !SharedReadPolicies.replyRead(session) && !SessionFilters.replyInProgress(session, true) &&
                replyAt.isFinite && replyAt > 0 && (now - 120...now).contains(replyAt)
            scopes[key] = Scope(session: session.target.session, kind: kind, completedAt: completion ? replyAt : nil, replyId: replyId)
        } }
        guard let rows = page["events"]?.array, rows.count <= 100 else { throw ZerusError.invalid("Invalid encrypted event page.") }
        var events: [JSON] = []
        for value in rows {
            guard let row = value.object else { throw ZerusError.invalid("Invalid encrypted event.") }
            let computer = try WorkspaceProtocol.string(row, "computer_id"), key = Key(computer: computer, scope: row.string("scope_id"))
            let scope = ambiguous.contains(key) ? nil : scopes[key]
            let kind = row.string("alert_kind") == "input" ? "attention" : row.string("alert_kind")
            let mapped = row.string("kind") == "alert" && scope != nil && (kind == scope?.kind || kind == "completed" && scope?.completedAt != nil)
            let at: JSON = mapped && kind == "completed" ? .double(scope!.completedAt!) : (row["created_at"] ?? .null)
            events.append(.object(["id": row["id"] ?? .null, "computer_id": .string(computer), "session": .string(scope?.session ?? "encrypted-workspace-refresh"),
                                  "created_at": at, "kind": .string(mapped ? "alert" : "refresh"), "alert_kind": mapped ? .string(kind) : .null,
                                  "native_reply_id": mapped && kind == "completed" ? .string(scope!.replyId) : .null]))
        }
        return ["events": .array(events), "cursor": page["cursor"] ?? page["next_after"] ?? .int(after)]
    }
}
