import Foundation

/// One native session row of a computer snapshot, with its raw native fields.
public struct Session: Hashable, Sendable, Identifiable {
    public var target: Target
    public var title: String
    public var agent: String
    public var status: String
    public var project: String
    public var preview: String
    public var pendingCount: Int
    public var raw: JSONObject
    public var projectKey: ProjectKey?

    public init(target: Target, title: String, agent: String, status: String, project: String, preview: String, pendingCount: Int,
                raw: JSONObject, projectKey: ProjectKey? = nil) {
        self.target = target
        self.title = title
        self.agent = agent
        self.status = status
        self.project = project
        self.preview = preview
        self.pendingCount = pendingCount
        self.raw = raw
        self.projectKey = projectKey
    }

    public var id: String { target.key }
}

public struct ProjectKey: Codable, Hashable, Sendable {
    public var connectionId: String
    public var swarmId: String
    public var id: String
    public init(connectionId: String, swarmId: String, id: String) {
        self.connectionId = connectionId
        self.swarmId = swarmId
        self.id = id
    }
    public var key: String { JSON([connectionId, swarmId, id]).compact }
}

public struct ProjectFolder: Codable, Hashable, Sendable {
    public var id: String
    public var name: String
    public var path: String
    public var computerId: String
    public var computerName: String
    public init(id: String, name: String, path: String, computerId: String, computerName: String) {
        self.id = id
        self.name = name
        self.path = path
        self.computerId = computerId
        self.computerName = computerName
    }
}

public struct ProjectSummary: Codable, Hashable, Sendable, Identifiable {
    public var key: ProjectKey
    public var name: String
    public var color: String
    public var folders: [ProjectFolder]
    /// Computer ID to name, in first-seen order.
    public var computers: [String: String]
    public var computerOrder: [String]
    public var stale: Bool
    public var catalogued: Bool

    public init(key: ProjectKey, name: String, color: String, folders: [ProjectFolder], computers: [(String, String)],
                stale: Bool = false, catalogued: Bool = true) {
        self.key = key
        self.name = name
        self.color = color
        self.folders = folders
        self.computers = [:]
        self.computerOrder = []
        self.stale = stale
        self.catalogued = catalogued
        for (id, name) in computers { addComputer(id, name) }
    }

    public var id: String { key.key }

    mutating func addComputer(_ id: String, _ name: String) {
        if computers.updateValue(name, forKey: id) == nil { computerOrder.append(id) }
    }
}

public struct ProjectCatalog: Sendable {
    public var projects: [ProjectSummary]
    public var sessions: [Session]
    public var unavailableComputers: [String]
    /// For each catalogued project, every computer's tasks digest (by computer ID).
    public var taskDigests: [ProjectKey: [String: String]] = [:]
}

/// Canonical IDs own membership; paths are only an exact fallback for uncatalogued sessions.
public enum ProjectParser {
    public static func parse(_ connection: Connection, _ computers: [JSONObject], _ sessions: [Session]) -> ProjectCatalog {
        var groups: [ProjectKey: ProjectSummary] = [:]
        var order: [ProjectKey] = []
        func put(_ summary: ProjectSummary) {
            if groups.updateValue(summary, forKey: summary.key) == nil { order.append(summary.key) }
        }
        var catalogs: [String: JSONObject] = [:]
        var digests: [ProjectKey: [String: String]] = [:]
        for computer in computers { if let catalog = computer.object("snapshot")?.object("mobile_projects") { catalogs[computer.string("id")] = catalog } }
        var assignments: [String: Set<ProjectKey>] = [:]
        var unavailable: [String] = []
        for computer in computers {
            let computerId = computer.string("id")
            let computerName = computer.string("name", "id")
            // An offline peer has no snapshot; Machines already reports it as offline.
            if computer.object("snapshot") == nil && !computer.bool("online") { continue }
            guard let catalog = catalogs[computerId], catalog.int("schema") == 1, catalog.bool("available") else {
                unavailable.append(computerName)
                continue
            }
            let swarm = catalog.string("swarm_id")
            guard !swarm.isEmpty else { unavailable.append(computerName); continue }
            for project in catalog.objects("projects") {
                let id = project.string("id")
                guard !id.isEmpty else { continue }
                let key = ProjectKey(connectionId: connection.id, swarmId: swarm, id: id)
                // Every computer of the swarm names its own digest of the project's tasks.
                let digest = project.string("tasks_digest")
                if digest.count == 64, digest.allSatisfy({ ("0"..."9").contains($0) || ("a"..."f").contains($0) }) {
                    digests[key, default: [:]][computerId] = digest
                }
                let folders = project.objects("folders").filter { $0.bool("local", default: true) }.map {
                    ProjectFolder(id: $0.string("id"), name: $0.string("name"), path: $0.string("path"), computerId: computerId, computerName: computerName)
                }.filter { !$0.path.isEmpty }
                let memberNames = (project.array("sessions") ?? []).map { $0.textValue ?? "" }
                let archiveIds = (project.array("archives") ?? []).map { $0.textValue ?? "" }
                let participates = !folders.isEmpty || sessions.contains { session in
                    session.target.computerId == computerId &&
                        (session.target.archiveId.isEmpty ? memberNames.contains(session.target.session) : archiveIds.contains(session.target.archiveId))
                }
                let existing = groups[key]
                var seenFolders = Set<String>()
                let merged = ((existing?.folders ?? []) + folders).filter {
                    seenFolders.insert($0.computerId + "\u{0}" + ($0.id.isEmpty ? $0.path : $0.id)).inserted
                }
                var summary = ProjectSummary(key: key, name: project.string("name").isEmpty ? "Project" : project.string("name"),
                                             color: project.string("color"), folders: merged, computers: [],
                                             stale: existing?.stale == true || catalog.bool("stale"))
                if let existing { for id in existing.computerOrder { summary.addComputer(id, existing.computers[id]!) } }
                if participates { summary.addComputer(computerId, computerName) }
                put(summary)
                for name in memberNames where !name.isEmpty { assignments["\(computerId)\u{0}live:\(name)", default: []].insert(key) }
                for archive in archiveIds where !archive.isEmpty { assignments["\(computerId)\u{0}archive:\(archive)", default: []].insert(key) }
            }
        }
        var computerNames: [String: String] = [:]
        for computer in computers where computerNames[computer.string("id")] == nil { computerNames[computer.string("id")] = computer.string("name", "id") }
        let mapped = sessions.map { session -> Session in
            let computerName = computerNames[session.target.computerId] ?? session.target.computerId
            let swarm = catalogs[session.target.computerId]?.string("swarm_id") ?? ""
            let explicit = session.raw.string("mobile_project_id")
            let membershipKey = "\(session.target.computerId)\u{0}" + (session.target.archiveId.isEmpty ? "live:\(session.target.session)" : "archive:\(session.target.archiveId)")
            let named = assignments[membershipKey].flatMap { $0.count == 1 ? $0.first : nil }
            let canonical = !explicit.isEmpty && !swarm.isEmpty ? ProjectKey(connectionId: connection.id, swarmId: swarm, id: explicit) : named
            let cwd = session.raw.string("cwd")
            var exactFolder: ProjectKey?
            if canonical == nil {
                let matches = order.compactMap { groups[$0] }.filter { group in
                    group.folders.contains { $0.computerId == session.target.computerId && $0.path == cwd }
                }
                if matches.count == 1 { exactFolder = matches[0].key }
            }
            var nativeName = ""
            if case .string(let name)? = session.raw["project"] { nativeName = name }
            let key = canonical ?? exactFolder ?? (!nativeName.isEmpty
                ? ProjectKey(connectionId: connection.id, swarmId: "alias:\(session.target.computerId)", id: "name:\(nativeName)")
                : ProjectKey(connectionId: connection.id, swarmId: "folder:\(session.target.computerId)", id: cwd.isEmpty ? "unassigned" : cwd))
            if var existing = groups[key] {
                existing.addComputer(session.target.computerId, computerName)
                if !existing.catalogued, !cwd.isEmpty, !existing.folders.contains(where: { $0.computerId == session.target.computerId && $0.path == cwd }) {
                    existing.folders.append(ProjectFolder(id: "", name: existing.name, path: cwd, computerId: session.target.computerId, computerName: computerName))
                }
                put(existing)
            } else {
                var trimmed = Substring(cwd)
                while trimmed.hasSuffix("/") { trimmed = trimmed.dropLast() }
                let leaf = trimmed.split(separator: "/", omittingEmptySubsequences: false).last.map(String.init) ?? ""
                let title = !nativeName.isEmpty ? nativeName : (leaf.isEmpty ? "Unassigned sessions" : leaf)
                put(ProjectSummary(key: key, name: title, color: "",
                                   folders: cwd.isEmpty ? [] : [ProjectFolder(id: "", name: title, path: cwd, computerId: session.target.computerId, computerName: computerName)],
                                   computers: [(session.target.computerId, computerName)], catalogued: false))
            }
            var copy = session
            copy.project = groups[key]!.name
            copy.projectKey = key
            return copy
        }
        return ProjectCatalog(projects: order.map { groups[$0]! }, sessions: mapped, unavailableComputers: unavailable,
                              taskDigests: digests)
    }
}

/// Session rows of one computer snapshot, ported from Android `NativeParser.sessions`.
public enum NativeSessions {
    public static func sessions(_ connection: Connection, _ machine: JSONObject) -> [Session] {
        guard let snapshot = machine.object("snapshot") else { return [] }
        let computerId = machine.string("id")
        return snapshot.objects("sessions").compactMap { raw in
            let archive = raw.string("archive_id")
            if raw.string("state") == "archived" || !archive.isEmpty {
                guard AttachmentPolicy.validId(archive) else { return nil }
            }
            let phase = raw.string("phase")
            let status = ["input", "approval", "error"].contains(phase) ? phase : raw.string("activity", "phase", "state")
            return Session(target: Target(computerId: computerId, session: raw.string("name"), run: raw.string("run_id"),
                                          conversation: raw.string("conversation_id"), connectionId: connection.id, archiveId: archive),
                           title: raw.string("title", "label", "name"), agent: raw.string("agent", "cmd"), status: status,
                           project: raw.string("project", "cwd"), preview: SessionPreview.render(raw.string("last_message", "prompt")),
                           pendingCount: QuestionPolicies.blockingRequests(raw), raw: raw)
        }
    }
}

/// Display-only native reply summary. Raw history and delivery identity are never rewritten.
public enum SessionPreview {
    static let open = "<send_user_message_question_reply>"
    static let close = "</send_user_message_question_reply>"
    static let ide = "# Context from my IDE setup:\n"
    static let request = "\n## My request for Codex:\n"
    static let maxParse = 32_768
    static let maxPreview = 240

    public static func render(_ original: String) -> String {
        if original.utf16.count > maxParse { return bounded(original) }
        var text = original.trimmingCharacters(in: .whitespacesAndNewlines)
        let originalTrimmed = text
        if text.hasPrefix(ide), let marker = text.range(of: request, options: .backwards) {
            text = String(text[marker.upperBound...]).trimmingCharacters(in: .whitespacesAndNewlines)
        }
        guard text.hasPrefix(open) else { return bounded(text != originalTrimmed ? text : original) }
        let body = String(text.dropFirst(open.count)).trimmingCharacters(in: CharacterSet(charactersIn: " \t\r\n"))
        guard text.hasSuffix(close) else {
            if (body.hasPrefix("{") || body.hasPrefix("[")) && !text.contains(close) && text.hasSuffix("…") {
                if let answer = partialAnswer(body), !answer.trimmingCharacters(in: .whitespaces).isEmpty {
                    return bounded("Answer: \(answer)", suffix: "…")
                }
                return "Question response"
            }
            return bounded(text)
        }
        let json = String(text.dropFirst(open.count).dropLast(close.count)).trimmingCharacters(in: CharacterSet(charactersIn: " \t\r\n"))
        var cursor = Cursor(json)
        guard cursor.complete(), let parsed = try? JSON.parse(json) else { return bounded(text) }
        let values: [JSON]
        switch parsed {
        case .object: values = [parsed]
        case .array(let items): values = items
        default: values = []
        }
        guard !values.isEmpty, values.count <= 16 else { return bounded(text) }
        var answers: [String] = []
        for value in values {
            guard let reply = value.object, case .string(let item)? = reply["questionItemId"], !item.isEmpty,
                  case .string(let question)? = reply["question"], !question.trimmingCharacters(in: .whitespaces).isEmpty,
                  case .string(let answer)? = reply["answer"] else { return bounded(text) }
            answers.append(answer.trimmingCharacters(in: .whitespaces).isEmpty ? "(empty answer)" : answer)
        }
        return bounded("Answer: " + answers.joined(separator: "; "))
    }

    static func partialAnswer(_ body: String) -> String? {
        var cursor = Cursor(body)
        cursor.space()
        if cursor.consume("[") { cursor.space() }
        guard cursor.consume("{"), let key = cursor.string(), case .string("answer")? = try? JSON.parse(key), cursor.consume(":"),
              let answer = cursor.string() else { return nil }
        cursor.space()
        guard let next = cursor.peek(), next == "," || next == "}" else { return nil }
        if case .string(let value)? = try? JSON.parse(answer) { return value }
        return nil
    }

    static func bounded(_ text: String, suffix: String = "") -> String {
        let scalars = Array(text.unicodeScalars)
        let limit = maxPreview - (suffix.isEmpty ? 0 : 1)
        if scalars.count > limit {
            var view = String.UnicodeScalarView()
            view.append(contentsOf: scalars.prefix(maxPreview - 1))
            return String(view) + "…"
        }
        return text + suffix
    }

    /// Strict bounded JSON grammar for question replies. Scalars, not graphemes: `\r\n` is two separators.
    struct Cursor {
        static let blank: Set<Unicode.Scalar> = [" ", "\t", "\r", "\n"]
        static let tokenEnd: Set<Unicode.Scalar> = [" ", "\t", "\r", "\n", ",", "]", "}"]
        let input: [Unicode.Scalar]
        var index = 0
        let replyDepth: Int

        init(_ text: String) {
            input = Array(text.unicodeScalars)
            replyDepth = input.first { !Cursor.blank.contains($0) } == "[" ? 1 : 0
        }

        mutating func space() { while index < input.count, Cursor.blank.contains(input[index]) { index += 1 } }
        func peek() -> Unicode.Scalar? { index < input.count ? input[index] : nil }
        mutating func consume(_ char: Unicode.Scalar) -> Bool {
            space()
            if peek() == char { index += 1; return true }
            return false
        }

        func text(_ range: Range<Int>) -> String {
            var view = String.UnicodeScalarView()
            view.append(contentsOf: input[range])
            return String(view)
        }

        mutating func string() -> String? {
            space()
            let start = index
            guard peek() == "\"" else { return nil }
            index += 1
            while index < input.count {
                let char = input[index]
                index += 1
                switch char {
                case "\"": return text(start..<index)
                case "\\":
                    guard index < input.count else { return nil }
                    let escape = input[index]
                    index += 1
                    if escape == "u" {
                        for _ in 0..<4 {
                            guard index < input.count, input[index].properties.isASCIIHexDigit else { return nil }
                            index += 1
                        }
                    } else if !"\"\\/bfnrt".unicodeScalars.contains(escape) { return nil }
                default:
                    if char.value < 32 { return nil }
                }
            }
            return nil
        }

        mutating func complete() -> Bool {
            guard value(0) else { return false }
            space()
            return index == input.count
        }

        mutating func value(_ depth: Int) -> Bool {
            if depth > 16 { return false }
            space()
            switch peek() {
            case "\"": return string() != nil
            case "{":
                index += 1
                if consume("}") { return true }
                repeat {
                    guard let key = string(), consume(":") else { return false }
                    space()
                    if depth == replyDepth, case .string(let name)? = try? JSON.parse(key),
                       ["answer", "question", "questionItemId"].contains(name), peek() != "\"" { return false }
                    guard value(depth + 1) else { return false }
                } while consume(",")
                return consume("}")
            case "[":
                index += 1
                if consume("]") { return true }
                repeat { guard value(depth + 1) else { return false } } while consume(",")
                return consume("]")
            default:
                let start = index
                while index < input.count, !Cursor.tokenEnd.contains(input[index]) { index += 1 }
                let token = text(start..<index)
                return ["true", "false", "null"].contains(token) || Cursor.isNumber(token)
            }
        }

        static func isNumber(_ token: String) -> Bool {
            token.range(of: "^-?(0|[1-9][0-9]*)(\\.[0-9]+)?([eE][+-]?[0-9]+)?$", options: .regularExpression) != nil
        }
    }
}
