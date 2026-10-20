import Foundation

public struct NotificationAttention: Hashable, Sendable {
    public let phase: String
    public let requestId: String
    public let startedAt: Double?
    public let run: String
    public static func == (lhs: Self, rhs: Self) -> Bool {
        lhs.phase.utf8.elementsEqual(rhs.phase.utf8) && lhs.requestId.utf8.elementsEqual(rhs.requestId.utf8) &&
            lhs.run.utf8.elementsEqual(rhs.run.utf8) && lhs.startedAt == rhs.startedAt
    }
    func matches(_ current: NotificationAttention) -> Bool {
        phase.utf8.elementsEqual(current.phase.utf8) && run.utf8.elementsEqual(current.run.utf8) && (requestId.isEmpty
            ? current.requestId.isEmpty && startedAt != nil && startedAt == current.startedAt : requestId.utf8.elementsEqual(current.requestId.utf8))
    }
}
public struct NotificationSuppression: Hashable, Sendable {
    public let replyId: String?
    public let signature: String?
    public let optionalIds: [String]
    public let main: NotificationAttention?
    public let children: [String: NotificationAttention]
    public static func == (lhs: Self, rhs: Self) -> Bool {
        lhs.replyId.map { Data($0.utf8) } == rhs.replyId.map { Data($0.utf8) } && lhs.signature == rhs.signature &&
            lhs.optionalIds.map { Data($0.utf8) } == rhs.optionalIds.map { Data($0.utf8) } && lhs.main == rhs.main &&
            lhs.children.count == rhs.children.count && lhs.children.allSatisfy { id,value in
                rhs.children.first { $0.key.utf8.elementsEqual(id.utf8) }?.value == value
            }
    }
}
public struct SessionNotificationPreference: Hashable, Sendable {
    public let conversation: String
    public let muted: Bool
    public let revision: Int64
    public let suppressed: NotificationSuppression
    public static func == (lhs: Self, rhs: Self) -> Bool {
        lhs.conversation.utf8.elementsEqual(rhs.conversation.utf8) && lhs.muted == rhs.muted && lhs.revision == rhs.revision && lhs.suppressed == rhs.suppressed
    }
}

/// Host-owned state and exact CAS receipts; missing metadata never becomes a device-local setting.
public enum SessionNotificationPreferences {
    public static let operation = "session_notifications"
    public static let unavailable = "Shared notification settings are unavailable. Update and refresh Zerus on this computer."
    private static func text(_ value: JSON?, empty: Bool = true, max: Int = 256, multiline: Bool = false) -> String? {
        guard case .string(let text)? = value, (empty || !text.isEmpty), text.utf8.count <= max,
              !text.unicodeScalars.contains(where:{ PairingInvite.isISOControl($0) && !(multiline && $0.value == 10) }) else { return nil }
        return text
    }
    private static func integer(_ value: JSON?) -> Int64? {
        guard case .int(let number)? = value, number >= 0 else { return nil };return number
    }
    private static func time(_ value: JSON?) -> Double? {
        guard let number = value?.numberValue, number.isFinite, number > 0 else { return nil };return number
    }
    private static func fields(_ value: JSONObject, _ keys: Set<String>) -> Bool { value.count == keys.count && keys.allSatisfy(value.has) }
    private static func fits(_ value: JSONObject) -> Bool {
        var bytes = 0
        // Compact JSON has already escaped quotes, backslashes and ASCII controls.
        for scalar in JSON.object(value).compact.unicodeScalars {
            bytes += scalar.value <= 127 ? 1 : scalar.value <= 65535 ? 6 : 12
            if bytes > 384 * 1024 { return false }
        }
        return true
    }
    private static func attention(_ value: JSONObject, main: Bool) -> NotificationAttention? {
        guard fields(value,main ? ["run_id","phase","request_id","started_at"] : ["phase","request_id","started_at"]) else { return nil }
        guard let phase = text(value["phase"]), let request = text(value["request_id"],max:65536,multiline:true), value.has("started_at"),
              value["started_at"] == .null || time(value["started_at"]) != nil else { return nil }
        guard !request.isEmpty || time(value["started_at"]) != nil else { return nil }
        let run: String
        if main { guard let reported = text(value["run_id"],empty:false,max:128) else { return nil };run = reported } else { run = "" }
        if !main && !["approval","input","attention"].contains(phase) { return nil }
        return NotificationAttention(phase:phase,requestId:request,startedAt:time(value["started_at"]),run:run)
    }
    public static func parse(_ raw: JSONObject?, conversation: String) -> SessionNotificationPreference? {
        guard let value = raw?.object("notification_preferences"), !conversation.isEmpty,
              fields(value,["conversation_id","muted","revision","suppressed"]),fits(value),
              text(value["conversation_id"],empty:false,max:256)?.utf8.elementsEqual(conversation.utf8) == true,
              case .bool(let muted)? = value["muted"], let revision = integer(value["revision"]), let suppressed = value.object("suppressed"),
              fields(suppressed,["reply_id","attention_signature","optional_question_ids","main_attention","child_attention"]),
              ["reply_id","attention_signature","main_attention"].allSatisfy(suppressed.has) else { return nil }
        let reply = text(suppressed["reply_id"],empty:false)
        let signature = text(suppressed["attention_signature"],empty:false)
        guard suppressed["reply_id"] == .null || reply != nil,
              suppressed["attention_signature"] == .null || signature?.range(of:"^[0-9a-f]{64}$",options:.regularExpression) != nil,
              let values = suppressed.array("optional_question_ids"), values.count <= 64 else { return nil }
        let ids = values.compactMap { text($0,empty:false) }
        guard ids.count == values.count, Set(ids.map { Data($0.utf8) }).count == ids.count,
              zip(ids,ids.dropFirst()).allSatisfy({ pair in pair.0.utf8.lexicographicallyPrecedes(pair.1.utf8) }),
              let childrenRaw = suppressed.object("child_attention"),childrenRaw.count <= 256 else { return nil }
        let main: NotificationAttention?
        if suppressed["main_attention"] == .null { main = nil }
        else { guard let object = suppressed.object("main_attention"),let parsed = attention(object,main:true) else { return nil };main = parsed }
        var children: [String:NotificationAttention] = [:]
        for id in childrenRaw.keys {
            guard text(.string(id),empty:false) != nil,let child = childrenRaw.object(id),let parsed = attention(child,main:false) else { return nil }
            children[id] = parsed
        }
        return SessionNotificationPreference(conversation:conversation,muted:muted,revision:revision,
            suppressed:NotificationSuppression(replyId:reply,signature:signature,optionalIds:ids,main:main,children:children))
    }
    public static func parse(_ raw: JSONObject?, _ target: Target) -> SessionNotificationPreference? { parse(raw,conversation:target.conversation) }
    public static func muted(_ session: Session) -> Bool { parse(session.raw,session.target)?.muted == true }
    public static func reason(_ target: Target, _ raw: JSONObject?) -> String {
        if !target.agentId.isEmpty || !target.archiveId.isEmpty { return "Notification controls belong to the live parent conversation." }
        return target.run.isEmpty || text(.string(target.run),empty:false,max:128) == nil || parse(raw,target) == nil ? unavailable : ""
    }
    public static func label(_ preference: SessionNotificationPreference?) -> String {
        guard let preference else { return "Shared notification settings" };return preference.muted ? "Unmute notifications" : "Mute notifications"
    }
    public static func arguments(_ raw: JSONObject) throws -> JSONObject {
        guard raw.count == 2, case .bool(_)? = raw["muted"], integer(raw["expected_revision"]) != nil else { throw ZerusError.invalid("Invalid shared notification setting.") }
        return raw
    }
    public static func payload(_ target: Target, _ args: JSONObject) -> JSONObject {
        ["run_id":.string(target.run),"conversation_id":.string(target.conversation),"muted":args["muted"] ?? .null,"expected_revision":args["expected_revision"] ?? .null]
    }
    public static func result(_ action: SessionAction, _ receipt: JSONObject) -> SessionAction {
        var copy = action
        let refusals = ["conflict":"Shared notification settings changed. Refresh and try again.",
            "baseline_unavailable":"The computer could not capture a complete notification baseline. The setting was not changed.",
            "stale_identity":"The conversation changed. Refresh its exact identity before applying.","not_found":"This conversation is no longer available.",
            "capacity":"The computer's notification preference storage is full. The setting was not changed.",
            "revision_exhausted":"The notification preference revision is exhausted. The setting was not changed."]
        if receipt["request_id"] == .string(action.requestId),receipt["state"] == .string("completed"),let value = receipt.object("result"),value["ok"] == .bool(false),
           !value.has("applied"),case .string(let error)? = value["error"],let message = refusals[error] {
            copy.status = "failed";copy.error = message;return copy
        }
        func uncertain() -> SessionAction {
            copy.status = "uncertain";copy.error = "The shared notification setting was not confirmed. Check its original receipt; it has not been retried.";return copy
        }
        guard receipt["request_id"] == .string(action.requestId),receipt["state"] == .string("completed"),let value = receipt.object("result"),value["ok"] == .bool(true),
              text(value["run_id"],empty:false,max:128)?.utf8.elementsEqual(action.target.run.utf8) == true,
              text(value["conversation_id"],empty:false)?.utf8.elementsEqual(action.target.conversation.utf8) == true,case .bool(let applied)? = value["applied"],
              let args = try? arguments(action.arguments),let revision = integer(args["expected_revision"]),!(applied && revision == Int64.max),
              let preferences = value.object("preferences"),let pref = parse(["notification_preferences":.object(preferences)],action.target),
              pref.muted == args.bool("muted"),pref.revision == revision + (applied ? 1 : 0) else { return uncertain() }
        copy.status = "completed";copy.error = "";copy.confirmedNotificationPreference = JSON.object(preferences).compact;return copy
    }

    private static func provider(_ target: Target) -> String { String(target.session.split(separator:"/",omittingEmptySubsequences:false).first ?? "") }
    public static func key(_ target: Target) -> String {
        Data(JSON.array([.string(target.connectionId),.string(target.computerId),.string(provider(target)),.string(target.conversation)]).compact.utf8).base64EncodedString()
    }
    public static func sameConversation(_ left: Target, _ right: Target) -> Bool {
        left.connectionId.utf8.elementsEqual(right.connectionId.utf8) && left.computerId.utf8.elementsEqual(right.computerId.utf8) &&
            provider(left).utf8.elementsEqual(provider(right).utf8) &&
            left.conversation.utf8.elementsEqual(right.conversation.utf8)
    }
    /// Valid host receipts in the existing bounded history take precedence over equal conflicting polls.
    public static func known(_ sessions: [Session] = [], actions: [SessionAction] = []) -> [String:JSONObject] {
        var result: [String:JSONObject] = [:]
        func retain(_ target: Target, _ raw: JSONObject) {
            guard let incoming = parse(raw,target) else { return }
            let previous = result[key(target)].flatMap { parse(["notification_preferences":.object($0)],target) }
            if previous == nil || incoming.revision > previous!.revision { result[key(target)] = raw.object("notification_preferences") }
        }
        for action in actions.reversed() where action.operation == operation && ["completed","reviewed"].contains(action.status) {
            guard let proof = action.confirmedNotificationPreference,let json = try? JSON.parse(proof),let pref = json.object,
                  let args = try? arguments(action.arguments),let parsed = parse(["notification_preferences":.object(pref)],action.target),
                  let revision = integer(args["expected_revision"]),parsed.muted == args.bool("muted"),
                  parsed.revision == revision || revision < Int64.max && parsed.revision == revision + 1 else { continue }
            retain(action.target,["notification_preferences":.object(pref)])
        }
        for row in sessions { retain(row.target,row.raw) }
        return result
    }
    public static func merge(_ raw: JSONObject, _ target: Target, _ known: [String:JSONObject]) -> JSONObject {
        guard let previous = known[key(target)],let retained = parse(["notification_preferences":.object(previous)],target) else { return raw }
        if let incoming = parse(raw,target),incoming.revision > retained.revision || incoming == retained { return raw }
        var copy = raw;copy["notification_preferences"] = .object(previous);return copy
    }
    public static func mergeRows(_ incoming: [Session], retained: [Session], actions: [SessionAction]) -> [Session] {
        let known = known(retained,actions:actions)
        return incoming.map { row in var copy = row;copy.raw = merge(row.raw,row.target,known);return copy }
    }
    public static func mergeDetail(_ raw: JSONObject, _ target: Target, retained: [JSONObject], actions: [SessionAction]) -> JSONObject {
        let proofs = known(actions:actions.filter { key($0.target) == key(target) })
        var merged = raw
        for previous in retained {
            let candidate = merge(previous,target,proofs)
            if parse(candidate,target) != nil,let preference = candidate.object("notification_preferences") {
                merged = merge(merged,target,[key(target):preference])
            }
        }
        return merge(merged,target,proofs)
    }
    /// Proof eviction never deletes durable request IDs, outcomes or pending actions.
    public static func boundedProofs(_ actions: [SessionAction]) -> [SessionAction] {
        var winners: [String:(Int,Int64)] = [:]
        for index in actions.indices.reversed() {
            let action = actions[index]
            guard let value = known(actions:[action])[key(action.target)],let revision = parse(["notification_preferences":.object(value)],action.target)?.revision else { continue }
            let identity = key(action.target)
            if winners[identity] == nil || revision > winners[identity]!.1 { winners[identity] = (index,revision) }
        }
        var bytes = 0
        return Array(actions.indices.reversed().map { index in
            var action = actions[index]
            if let proof = action.confirmedNotificationPreference {
                let cost = JSON.string(proof).compact.utf8.count + 64
                if winners[key(action.target)]?.0 != index || cost > 1024 * 1024 - bytes { action.confirmedNotificationPreference = nil }
                else { bytes += cost }
            }
            return action
        }.reversed())
    }
    public static func deliveryAllowed(_ current: NotificationCurrent, actions: [SessionAction]) -> Bool {
        let target = Target(computerId:current.slot.computer,session:current.slot.session,run:"",conversation:current.conversation,connectionId:current.slot.connection)
        if actions.contains(where:{ $0.operation == operation && $0.blocksSending && sameConversation($0.target,target) }) { return false }
        guard let value = known(actions:actions.filter { key($0.target) == key(target) })[key(target)],
              let latest = parse(["notification_preferences":.object(value)],target) else { return true }
        guard let observed = current.notificationPreference else { return false }
        return !observed.muted && (observed.revision > latest.revision || observed == latest)
    }
    private static func currentMain(_ raw: JSONObject, _ target: Target) -> NotificationAttention {
        NotificationAttention(phase:raw.string("phase"),requestId:raw.string("attention_id"),startedAt:time(raw["attention_started"]),run:target.run)
    }
    private static func currentChildren(_ raw: JSONObject) -> [(String,NotificationAttention)] {
        let roster = raw.object("subagents")
        let values: [(String,JSONObject)] = (roster?.keys.compactMap { id in roster?.object(id).map { (id,$0) } } ?? []) +
            raw.objects("subagent_previews").map { ($0.string("agent_id","id"),$0) }
        return values.compactMap { id,value in
            let phase = value.string("display_state","state")
            guard !id.isEmpty,["approval","input","attention"].contains(phase) else { return nil }
            return (id,NotificationAttention(phase:phase,requestId:value.string("attention_id","question_id"),
                startedAt:time(value["attention_started"]) ?? time(value["last_event_at"]),run:""))
        }
    }
    public static func hasChildAttention(_ raw: JSONObject) -> Bool { !currentChildren(raw).isEmpty }
    public static func attentionIdentity(_ raw: JSONObject, _ target: Target) -> JSON {
        func edge(_ value: NotificationAttention) -> JSON {
            .array([.string(value.run),.string(value.phase),.string(value.requestId),value.requestId.isEmpty ? value.startedAt.map(JSON.double) ?? .null : .null])
        }
        let children = Array(Set(currentChildren(raw).map { id,value in JSON.array([.string(id),edge(value)]).compact })).sorted()
        return .array([edge(currentMain(raw,target)),.array(children.map(JSON.string))])
    }
    public static func attentionSuppressed(_ raw: JSONObject, _ target: Target, _ pref: SessionNotificationPreference) -> Bool {
        let main = currentMain(raw,target)
        let children = currentChildren(raw)
        if children.contains(where:{ id,value in pref.suppressed.children.first { $0.key.utf8.elementsEqual(id.utf8) }?.value.matches(value) != true }) { return false }
        let aggregate: Bool
        if case .string(let signature)? = raw["attention_signature"] { aggregate = signature == pref.suppressed.signature } else { aggregate = false }
        // Recovery identities are absent from the host baseline ABI and attention signature.
        if ["uncertain","blocked","exhausted"].contains(raw.object("recovery")?.string("state") ?? "") { return false }
        if raw.objects("mobile_attention").contains(where:{ !QuestionPolicies.optionalAttention($0) }) || raw.int("pending_question_count") > 0 { return aggregate }
        if ["approval","input","error"].contains(main.phase) { return pref.suppressed.main?.matches(main) == true }
        return !children.isEmpty || aggregate
    }
    public static func alertKind(_ raw: JSONObject, _ target: Target, kind: SessionAlertKind?, optional: [String]) -> SessionAlertKind? {
        guard let pref = parse(raw,target) else { return kind }
        if !pref.muted,(kind == .input || kind == .error),attentionSuppressed(raw,target,pref),
           optional.contains(where:{ id in !pref.suppressed.optionalIds.contains { $0.utf8.elementsEqual(id.utf8) } }) { return .optional }
        return kind
    }
    public static func suppressed(_ raw: JSONObject, _ target: Target, kind: SessionAlertKind?, optional: [String]) -> Bool {
        guard let pref = parse(raw,target) else { return false }
        if pref.muted { return true }
        switch kind {
        case .input,.error: return attentionSuppressed(raw,target,pref)
        case .optional: return !optional.isEmpty && optional.allSatisfy { id in pref.suppressed.optionalIds.contains { $0.utf8.elementsEqual(id.utf8) } }
        default:
            guard case .string(let reply)? = raw["reply_id"],let baseline = pref.suppressed.replyId else { return false }
            return reply.utf8.elementsEqual(baseline.utf8)
        }
    }
}
