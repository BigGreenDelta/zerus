import Foundation

/// Messages the person did not write, as the hgs journal marks their native
/// prompts: background task notices, subagent reports and other sessions'
/// messages. Desktop and Android present the same cases of
/// activity-notice-contract.json. A notice names the sender its envelope
/// declares; an envelope is text anyone can write, so nothing here calls it
/// verified.
public enum ActivityNotices {
    public static let role = "Notice"
    public static let origins: Set<String> = ["task_notification", "subagent_report", "peer_message"]

    public static func label(origin: String, sender: String) -> String {
        switch origin {
        case "subagent_report": return "Subagent report"
        case "peer_message": return "Message from \(sender)"
        default: return "Background task"
        }
    }

    /// Another session's message is read in full; other notices show their summary.
    public static func body(origin: String, detail: String, report: String) -> String {
        origin == "peer_message" && !report.isEmpty ? report : detail
    }

    /// A subagent's report stays collapsed below its summary until opened.
    public static func report(origin: String, report: String) -> String {
        origin == "subagent_report" ? report : ""
    }

    /// Display caches cannot turn a notice or reply into this phone's outgoing receipt.
    public static func cached(_ event: Event) -> Event {
        guard event.role != "You" else { return event }
        var copy = event
        copy.requestId = ""
        copy.delivery = ""
        copy.outgoingId = ""
        copy.submittedText = ""
        copy.deliveryState = nil
        return copy
    }
}
