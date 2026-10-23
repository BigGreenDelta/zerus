import Foundation

/// A paired gateway. `url` is the original stored identity; `endpoint` is the
/// transport actually used, which may follow a managed-relay alias.
public struct Connection: Codable, Hashable, Sendable, Identifiable {
    public let id: String
    public var label: String
    public let url: String
    public let token: String
    public let protocolVersion: Int

    public init(id: String, label: String, url: String, token: String, protocolVersion: Int = 1) {
        precondition([1, 2].contains(protocolVersion), "Unsupported workspace transport.")
        self.id = id
        self.label = label
        self.url = url
        self.token = token
        self.protocolVersion = protocolVersion
    }

    enum CodingKeys: String, CodingKey { case id, label, url, token, protocolVersion }
    public init(from decoder: any Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        id = try c.decode(String.self, forKey: .id); label = try c.decode(String.self, forKey: .label)
        url = try c.decode(String.self, forKey: .url); token = try c.decode(String.self, forKey: .token)
        protocolVersion = c.contains(.protocolVersion) ? try c.decode(Int.self, forKey: .protocolVersion) : 1
        guard [1, 2].contains(protocolVersion) else { throw ZerusError.invalid("Unsupported workspace transport.") }
    }

    public var endpoint: String { ManagedRelay.transport(url) }

    public var displayName: String {
        let endpointHost = URLComponents(string: endpoint)?.host ?? ""
        let storedHost = URLComponents(string: url)?.host
        let generated = label.trimmingCharacters(in: .whitespaces).isEmpty || UUID(uuidString: label) != nil ||
            (endpoint != url && storedHost.map { label.caseInsensitiveCompare($0) == .orderedSame } == true)
        return generated ? (endpointHost.isEmpty ? "Workspace" : endpointHost) : label
    }
}

/// The exact native destination of a read or mutation: gateway, computer,
/// session, run and conversation, plus an archive or child identity.
public struct Target: Codable, Hashable, Sendable {
    public var computerId: String
    public var session: String
    public var run: String
    public var conversation: String
    public var connectionId: String
    public var archiveId: String
    public var agentId: String
    public var parentConversation: String

    public init(computerId: String, session: String, run: String, conversation: String, connectionId: String = "demo",
                archiveId: String = "", agentId: String = "", parentConversation: String = "") {
        self.computerId = computerId
        self.session = session
        self.run = run
        self.conversation = conversation
        self.connectionId = connectionId
        self.archiveId = archiveId
        self.agentId = agentId
        self.parentConversation = parentConversation
    }

    public var key: String {
        var parts = [connectionId, computerId, session, run, conversation]
        if !archiveId.isEmpty { parts.append(archiveId) }
        if !agentId.isEmpty { parts += ["agent", agentId, parentConversation] }
        return JSON(parts).compact
    }

    public struct ReadOnly: Error, LocalizedError {
        public var errorDescription: String? { "Archived conversations are read-only." }
    }

    /// Identity fields every mutation of a live conversation carries.
    public func json() throws -> JSONObject {
        guard archiveId.isEmpty else { throw ReadOnly() }
        var object: JSONObject = ["expected_run_id": .string(run),
                                  "expected_conversation_id": .string(agentId.isEmpty ? conversation : parentConversation)]
        if !agentId.isEmpty { object["agent_id"] = .string(agentId) }
        return object
    }
}
