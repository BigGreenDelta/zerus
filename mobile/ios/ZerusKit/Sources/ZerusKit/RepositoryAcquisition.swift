import Foundation

/// Native checkout reservations survive transport interruption. Recovery only reads their original identity.
public enum RepositoryAcquisition {
    public static let clone = "repository_clone"
    public static let operations = ["repository_check", clone, "repository_status"]
    public static let uncertain = "Checkout unconfirmed. Check its original result; it has not been retried."
    public static func suggestedName(_ url: String) -> String {
        var leaf = url.trimmingCharacters(in: .whitespacesAndNewlines).trimmingCharacters(in: CharacterSet(charactersIn: "/"))
            .components(separatedBy: "/").last?.components(separatedBy: ":").last ?? ""
        if leaf.hasSuffix(".git") { leaf.removeLast(4) }
        return !leaf.isEmpty && ![".", ".."].contains(leaf) && !leaf.contains(where: { "/\\:*?\"<>|".contains($0) }) && !SessionActionPolicies.control(leaf) ? leaf : ""
    }
    public static func destination(_ parent: String, _ name: String) -> String { (parent == "/" ? "/" : parent + "/") + name }
    public static func validURL(_ url:String)->Bool {
        if url.isEmpty || url.utf8.count>4096 || url.contains(where: { $0.isWhitespace }) || SessionActionPolicies.control(url) ||
            url.contains("\\") || url.hasPrefix("//") || url.range(of:"^[A-Za-z]:",options:.regularExpression) != nil { return false }
        if url.hasPrefix("https://") {
            let rest=String(url.dropFirst(8)),host=String(url.dropFirst(8).prefix { $0 != "/" })
            return !host.isEmpty && !host.contains("@") && rest.contains("/") && !rest.contains(where: { "?#".contains($0) })
        }
        if url.hasPrefix("ssh://") {
            let rest=String(url.dropFirst(6)),host=String(url.dropFirst(6).prefix { $0 != "/" })
            return !host.isEmpty && !(host.components(separatedBy:"@").last ?? "").isEmpty &&
                (!host.contains("@") || !(host.components(separatedBy:"@").first ?? "").contains(":")) && rest.contains("/") && !rest.contains(where: { "?#".contains($0) })
        }
        if url.contains("://") || url.hasPrefix("-") || url.contains("::") { return false }
        let parts=url.split(separator:":",maxSplits:1,omittingEmptySubsequences:false)
        return parts.count==2 && !parts[0].isEmpty && !parts[0].contains("/") && !parts[1].isEmpty
    }
    public static func arguments(machineId: String, url: String, path: String) throws -> JSONObject {
        guard AttachmentPolicy.validId(machineId) else { throw ZerusError.invalid("Refresh this computer's identity.") }
        guard validURL(url) else { throw ZerusError.invalid("Use an SSH or HTTPS repository URL without embedded credentials.") }
        guard DirectoryCreation.pathError(path).isEmpty else { throw ZerusError.invalid("Enter an absolute Clone to path.") }
        let name = path.components(separatedBy: "/").last ?? ""
        guard name.utf8.count <= 240, name == name.trimmingCharacters(in: .whitespacesAndNewlines), !name.hasSuffix("."),
              !name.contains(where: { "/\\:*?\"<>|".contains($0) }) else { throw ZerusError.invalid("Choose a valid new folder name.") }
        let parent = String(path.dropLast(name.count + 1))
        return ["expected_machine_id": .string(machineId), "url": .string(url), "parent": .string(parent.isEmpty ? "/" : parent), "name": .string(name)]
    }
    public static func verifiedCheckout(_ raw:JSONObject?,path:String)->Bool {
        WorktreePresentation.forFolder(raw,path)?.string("path") == path && WorktreePresentation.contains(raw,path)
    }
    public static func pending(_ actions: [SessionAction], _ machine: MachineKey, path:String?=nil) -> SessionAction? {
        actions.last { action in
            let reserved=destination(action.arguments.string("parent"),action.arguments.string("name"))
            return action.operation == clone && action.target == Target(computerId:machine.computerId,session:"",run:"",conversation:"",connectionId:machine.connectionId) && action.blocksSending &&
                (path == nil || !DirectoryCreation.pathError(reserved).isEmpty || reserved == path)
        }
    }
    public static func result(_ action: SessionAction, _ receipt: JSONObject, readId: String? = nil) -> SessionAction {
        var copy = action
        guard action.target == Target(computerId:action.target.computerId,session:"",run:"",conversation:"",connectionId:action.target.connectionId), AttachmentPolicy.validId(action.requestId) else {
            copy.status="uncertain";copy.error=uncertain;return copy
        }
        let id = readId ?? action.requestId, p = action.arguments
        if id == action.requestId && receipt.string("request_id") == id && receipt.string("state") == "failed" {
            copy.status="failed";copy.error=receipt.string("error").nonEmpty ?? "Checkout was refused.";return copy
        }
        guard action.target.session.isEmpty, receipt.string("request_id") == id, receipt.string("state") == "completed",
              let raw = receipt.object("result"), raw["schema"] == .int(1), raw.string("request_id") == id,
              raw.string("clone_request_id") == action.requestId, raw.string("machine_id") == p.string("expected_machine_id"),
              raw.string("parent") == p.string("parent"), raw.string("name") == p.string("name"),
              raw.string("path") == destination(p.string("parent"), p.string("name")) else {
            copy.status = "uncertain"; copy.error = uncertain; return copy
        }
        switch raw.string("status") {
        case "cloned": copy.status = "completed"; copy.error = ""; copy.resultPath = raw.string("path")
        case "failed": copy.status = "failed"; copy.error = raw.string("detail").nonEmpty ?? "Checkout failed. Inspect the destination before choosing another folder."
        default: copy.status = "uncertain"; copy.error = uncertain
        }
        return copy
    }
}
