import CryptoKit
import Foundation

public struct WorkspaceMembershipChange: Hashable, Sendable {
    public let workspaceId: String
    public let revision: Int64
    public let previousHead: String
    public let issuerDeviceId: String
    public let issuerKeyGeneration: Int64
    public let action: String
    public let device: WorkspaceDevice?
    public let revokedDeviceId: String?
    public let signature: String

    public func frame() throws -> Data {
        try WorkspaceProtocol.identifier(workspaceId); try WorkspaceProtocol.identifier(issuerDeviceId)
        guard revision >= 0, issuerKeyGeneration > 0,
              (action == "add" && device != nil && revokedDeviceId == nil) || (action == "revoke" && device == nil && revokedDeviceId != nil) else {
            throw ZerusError.invalid("Invalid workspace membership change.")
        }
        if let revokedDeviceId { try WorkspaceProtocol.identifier(revokedDeviceId) }
        return try WorkspaceProtocol.frame("zerus-e2ee-membership-v1", [Data(workspaceId.utf8), WorkspaceProtocol.integer(revision), WorkspaceProtocol.decode(previousHead, size: 32),
            Data(issuerDeviceId.utf8), WorkspaceProtocol.integer(issuerKeyGeneration), Data(action.utf8), device?.frame() ?? Data(), Data((revokedDeviceId ?? "").utf8)])
    }
    public func verify(issuer: WorkspaceDevice) throws {
        guard issuer.role == "computer", issuer.deviceId == issuerDeviceId, issuer.keyGeneration == issuerKeyGeneration else {
            throw ZerusError.mismatch("Unrecognized workspace issuer.")
        }
        try WorkspaceCrypto.verify(publicKey: WorkspaceProtocol.decode(issuer.signingPublicKey, size: 32), signature: WorkspaceProtocol.decode(signature, size: 64), message: frame())
    }
    public func head() throws -> String {
        try WorkspaceProtocol.encode(Data(SHA256.hash(data: WorkspaceProtocol.frame("zerus-e2ee-membership-head-v1", [frame(), WorkspaceProtocol.decode(signature, size: 64)]))))
    }
    public var json: JSONObject { ["version": 1, "statement": .object(["workspace_id": .string(workspaceId), "revision": .int(revision), "previous_head": .string(previousHead),
        "issuer_device_id": .string(issuerDeviceId), "issuer_key_generation": .int(issuerKeyGeneration), "action": .string(action), "device": device.map { .object($0.json) } ?? .null,
        "revoked_device_id": revokedDeviceId.map(JSON.string) ?? .null]), "signature": .string(signature)] }
    public static func parse(_ json: JSONObject) throws -> WorkspaceMembershipChange {
        try WorkspaceProtocol.fields(json, ["version", "statement", "signature"])
        guard try WorkspaceProtocol.number(json, "version") == 1, let s = json["statement"]?.object else { throw ZerusError.invalid("Unsupported workspace membership change.") }
        try WorkspaceProtocol.fields(s, ["workspace_id", "revision", "previous_head", "issuer_device_id", "issuer_key_generation", "action", "device", "revoked_device_id"])
        let result = try WorkspaceMembershipChange(workspaceId: WorkspaceProtocol.string(s, "workspace_id"), revision: WorkspaceProtocol.number(s, "revision"),
            previousHead: WorkspaceProtocol.string(s, "previous_head"), issuerDeviceId: WorkspaceProtocol.string(s, "issuer_device_id"), issuerKeyGeneration: WorkspaceProtocol.number(s, "issuer_key_generation"),
            action: WorkspaceProtocol.string(s, "action"), device: s["device"]?.isNull == true ? nil : WorkspaceDevice.parse(s["device"]?.object ?? JSONObject()),
            revokedDeviceId: s["revoked_device_id"]?.isNull == true ? nil : WorkspaceProtocol.string(s, "revoked_device_id"), signature: WorkspaceProtocol.string(json, "signature"))
        _ = try result.frame(); _ = try WorkspaceProtocol.decode(result.signature, size: 64)
        return result
    }
}

/// A relay response can extend this state only through a verified exact successor.
public struct WorkspaceMembership: Hashable, Sendable {
    public let workspaceId: String
    public let revision: Int64
    public let head: String
    public let members: [String: WorkspaceDevice]
    public let revokedDeviceIds: Set<String>

    public init(workspaceId: String, revision: Int64, head: String, members: [String: WorkspaceDevice], revokedDeviceIds: Set<String>) throws {
        try WorkspaceProtocol.identifier(workspaceId); _ = try WorkspaceProtocol.decode(head, size: 32)
        guard revision >= 0, members.count + revokedDeviceIds.count <= 4096,
              members.allSatisfy({ $0.key == $0.value.deviceId }), revokedDeviceIds.isDisjoint(with: members.keys) else {
            throw ZerusError.invalid("Invalid workspace membership state.")
        }
        for id in revokedDeviceIds { try WorkspaceProtocol.identifier(id) }
        self.workspaceId = workspaceId; self.revision = revision; self.head = head; self.members = members; self.revokedDeviceIds = revokedDeviceIds
    }
    public func applying(_ change: WorkspaceMembershipChange) throws -> WorkspaceMembership {
        guard revision < Int64.max, change.workspaceId == workspaceId, change.revision == revision + 1, change.previousHead == head,
              let issuer = members[change.issuerDeviceId] else { throw ZerusError.mismatch("Workspace membership changed or was rolled back.") }
        try change.verify(issuer: issuer)
        var next = members, removed = revokedDeviceIds
        if change.action == "add" {
            guard let device = change.device, next[device.deviceId] == nil, !removed.contains(device.deviceId), next.count + removed.count < 4096 else {
                throw ZerusError.mismatch("Workspace device was already enrolled or removed.")
            }
            next[device.deviceId] = device
        } else {
            guard let id = change.revokedDeviceId, next[id] != nil else {
                throw ZerusError.mismatch("Cannot remove this workspace member.")
            }
            next[id] = nil; removed.insert(id)
        }
        return try WorkspaceMembership(workspaceId: workspaceId, revision: change.revision, head: change.head(), members: next, revokedDeviceIds: removed)
    }
    public func sender(_ envelope: WorkspaceEnvelope) throws -> WorkspaceDevice {
        let h = envelope.header
        guard h.workspaceId == workspaceId, h.membershipRevision == revision, h.membershipHead == head, let member = members[h.senderDeviceId] else {
            throw ZerusError.mismatch("Workspace membership must be updated before opening this payload.")
        }
        return member
    }
    public func frame() throws -> Data {
        var fields = try [Data(workspaceId.utf8), WorkspaceProtocol.integer(revision), WorkspaceProtocol.decode(head, size: 32), WorkspaceProtocol.integer(Int64(members.count))]
        for id in members.keys.sorted() { fields.append(try members[id]!.frame()) }
        fields.append(try WorkspaceProtocol.integer(Int64(revokedDeviceIds.count)))
        for id in revokedDeviceIds.sorted() { fields.append(Data(id.utf8)) }
        return WorkspaceProtocol.frame("zerus-e2ee-membership-state-v1", fields)
    }
    public var json: JSONObject {
        let devices = JSONObject(members.keys.sorted().map { ($0, .object(members[$0]!.json)) })
        return ["workspace_id": .string(workspaceId), "revision": .int(revision), "head": .string(head), "members": .object(devices), "revoked_device_ids": .array(revokedDeviceIds.sorted().map(JSON.string))]
    }
    public static func parse(_ json: JSONObject) throws -> WorkspaceMembership {
        try WorkspaceProtocol.fields(json, ["workspace_id", "revision", "head", "members", "revoked_device_ids"])
        guard let devices = json["members"]?.object, let raw = json["revoked_device_ids"]?.array, devices.count + raw.count <= 4096 else { throw ZerusError.invalid("Invalid workspace membership state.") }
        let ids = try raw.map { item -> String in guard case .string(let value) = item else { throw ZerusError.invalid("Invalid revoked workspace device.") }; return value }
        guard Set(ids).count == ids.count else { throw ZerusError.invalid("Duplicate revoked workspace device.") }
        var members: [String: WorkspaceDevice] = [:]
        for (id, value) in devices { members[id] = try WorkspaceDevice.parse(value.object ?? JSONObject()) }
        return try WorkspaceMembership(workspaceId: WorkspaceProtocol.string(json, "workspace_id"), revision: WorkspaceProtocol.number(json, "revision"), head: WorkspaceProtocol.string(json, "head"), members: members, revokedDeviceIds: Set(ids))
    }
}

public struct WorkspaceBootstrap: Sendable {
    public let state: WorkspaceMembership
    public let issuerDeviceId: String
    public let issuerKeyGeneration: Int64
    public let signature: String
    public func signatureFrame() throws -> Data {
        try WorkspaceProtocol.frame("zerus-e2ee-membership-snapshot-v1", [state.frame(), Data(issuerDeviceId.utf8), WorkspaceProtocol.integer(issuerKeyGeneration)])
    }
    public func verify(workspaceId: String, revision: Int64, head: String, issuer: WorkspaceDevice) throws -> WorkspaceMembership {
        guard state.workspaceId == workspaceId, state.revision == revision, state.head == head, issuerDeviceId == issuer.deviceId,
              issuerKeyGeneration == issuer.keyGeneration, issuer.role == "computer", state.members[issuerDeviceId] == issuer else {
            throw ZerusError.mismatch("Workspace invitation trust does not match this snapshot.")
        }
        try WorkspaceCrypto.verify(publicKey: WorkspaceProtocol.decode(issuer.signingPublicKey, size: 32), signature: WorkspaceProtocol.decode(signature, size: 64), message: signatureFrame())
        return state
    }
    public static func parse(_ json: JSONObject) throws -> WorkspaceBootstrap {
        try WorkspaceProtocol.fields(json, ["version", "state", "issuer_device_id", "issuer_key_generation", "signature"])
        guard try WorkspaceProtocol.number(json, "version") == 1, let state = json["state"]?.object else { throw ZerusError.invalid("Unsupported workspace bootstrap.") }
        let result = try WorkspaceBootstrap(state: WorkspaceMembership.parse(state), issuerDeviceId: WorkspaceProtocol.string(json, "issuer_device_id"),
            issuerKeyGeneration: WorkspaceProtocol.number(json, "issuer_key_generation"), signature: WorkspaceProtocol.string(json, "signature"))
        try WorkspaceProtocol.identifier(result.issuerDeviceId); _ = try WorkspaceProtocol.decode(result.signature, size: 64)
        guard result.issuerKeyGeneration > 0 else { throw ZerusError.invalid("Invalid bootstrap issuer.") }
        return result
    }
}

public struct WorkspaceApproval: Sendable {
    public let invitationId: String
    public let candidateDigest: String
    public let membership: WorkspaceMembershipChange
    public let signature: String
    public var json: JSONObject { ["version": 1, "invitation_id": .string(invitationId), "candidate_digest": .string(candidateDigest), "membership": .object(membership.json), "signature": .string(signature)] }
    public func signatureFrame() throws -> Data {
        try WorkspaceProtocol.frame("zerus-e2ee-enrollment-approval-v1", [Data(invitationId.utf8), WorkspaceProtocol.decode(candidateDigest, size: 32), membership.frame(), WorkspaceProtocol.decode(membership.signature, size: 64)])
    }
    public func verify(candidate: WorkspaceCandidate, issuer: WorkspaceDevice) throws {
        try candidate.verify()
        guard invitationId == candidate.invitationId, try candidateDigest == candidate.digest(), membership.workspaceId == candidate.workspaceId,
              membership.action == "add", membership.device == candidate.device else { throw ZerusError.mismatch("Workspace approval belongs to another device.") }
        try membership.verify(issuer: issuer)
        try WorkspaceCrypto.verify(publicKey: WorkspaceProtocol.decode(issuer.signingPublicKey, size: 32), signature: WorkspaceProtocol.decode(signature, size: 64), message: signatureFrame())
    }
    public static func parse(_ json: JSONObject) throws -> WorkspaceApproval {
        try WorkspaceProtocol.fields(json, ["version", "invitation_id", "candidate_digest", "membership", "signature"])
        guard try WorkspaceProtocol.number(json, "version") == 1, let membership = json["membership"]?.object else { throw ZerusError.invalid("Unsupported workspace approval.") }
        let result = try WorkspaceApproval(invitationId: WorkspaceProtocol.string(json, "invitation_id"), candidateDigest: WorkspaceProtocol.string(json, "candidate_digest"),
            membership: WorkspaceMembershipChange.parse(membership), signature: WorkspaceProtocol.string(json, "signature"))
        try WorkspaceProtocol.identifier(result.invitationId); _ = try WorkspaceProtocol.decode(result.candidateDigest, size: 32); _ = try WorkspaceProtocol.decode(result.signature, size: 64)
        return result
    }
}

/// Secret material stays in memory and never enters the candidate's HTTP JSON.
public struct WorkspaceInvitation: Sendable {
    public let workspaceId: String
    public let invitationId: String
    public let issuer: WorkspaceDevice
    public let expiresAt: Int64
    public let membershipRevision: Int64
    public let membershipHead: String
    private let secret: Data

    public func candidate(identity: WorkspaceIdentity, now: Int64) throws -> WorkspaceCandidate {
        guard now < expiresAt, identity.device.role == "phone" else { throw ZerusError.invalid("This phone invitation has expired.") }
        let unsigned = WorkspaceCandidate(workspaceId: workspaceId, invitationId: invitationId, device: identity.device, proof: "", signature: "")
        let proven = try WorkspaceCandidate(workspaceId: workspaceId, invitationId: invitationId, device: identity.device,
            proof: WorkspaceProtocol.encode(WorkspaceCrypto.hmac(secret: secret, message: unsigned.proofFrame())), signature: "")
        return try WorkspaceCandidate(workspaceId: workspaceId, invitationId: invitationId, device: identity.device, proof: proven.proof,
            signature: WorkspaceProtocol.encode(WorkspaceCrypto.sign(identity.keys, message: proven.signatureFrame())))
    }
    public static func parse(_ code: String) throws -> WorkspaceInvitation {
        guard code.hasPrefix("ze1."), code.utf8.count <= 4096,
              let json = try JSON.parse(WorkspaceProtocol.decode(String(code.dropFirst(4)), max: 3072)).object else { throw ZerusError.invalid("Invalid encrypted phone invitation.") }
        try WorkspaceProtocol.fields(json, ["version", "workspace_id", "invitation_id", "purpose", "issuer", "expires_at", "membership_revision", "membership_head", "secret"])
        guard try WorkspaceProtocol.number(json, "version") == 1, try WorkspaceProtocol.string(json, "purpose") == "phone", let issuerJSON = json["issuer"]?.object else {
            throw ZerusError.invalid("Use a phone invitation from an enrolled computer.")
        }
        let issuer = try WorkspaceDevice.parse(issuerJSON)
        guard issuer.role == "computer" else { throw ZerusError.invalid("Invalid invitation issuer.") }
        let result = try WorkspaceInvitation(workspaceId: WorkspaceProtocol.string(json, "workspace_id"), invitationId: WorkspaceProtocol.string(json, "invitation_id"), issuer: issuer,
            expiresAt: WorkspaceProtocol.number(json, "expires_at"), membershipRevision: WorkspaceProtocol.number(json, "membership_revision"), membershipHead: WorkspaceProtocol.string(json, "membership_head"),
            secret: WorkspaceProtocol.decode(WorkspaceProtocol.string(json, "secret"), size: 32))
        try WorkspaceProtocol.identifier(result.workspaceId); try WorkspaceProtocol.identifier(result.invitationId)
        guard result.expiresAt > 0 else { throw ZerusError.invalid("Invalid phone invitation.") }
        _ = try WorkspaceProtocol.decode(result.membershipHead, size: 32)
        return result
    }
}

public struct WorkspaceCandidate: Hashable, Sendable {
    public let workspaceId: String
    public let invitationId: String
    public let device: WorkspaceDevice
    public let proof: String
    public let signature: String
    public func proofFrame() throws -> Data { try WorkspaceProtocol.frame("zerus-e2ee-enrollment-proof-v1", [Data(workspaceId.utf8), Data(invitationId.utf8), Data(device.role.utf8), device.frame()]) }
    public func signatureFrame() throws -> Data { try WorkspaceProtocol.frame("zerus-e2ee-enrollment-candidate-v1", [proofFrame(), WorkspaceProtocol.decode(proof, size: 32)]) }
    public func digest() throws -> String { try WorkspaceProtocol.encode(Data(SHA256.hash(data: WorkspaceProtocol.frame("zerus-e2ee-candidate-digest-v1", [signatureFrame(), WorkspaceProtocol.decode(signature, size: 64)])))) }
    public func verify() throws { try WorkspaceCrypto.verify(publicKey: WorkspaceProtocol.decode(device.signingPublicKey, size: 32), signature: WorkspaceProtocol.decode(signature, size: 64), message: signatureFrame()) }
    public var json: JSONObject { ["version": 1, "workspace_id": .string(workspaceId), "invitation_id": .string(invitationId), "purpose": .string(device.role), "device": .object(device.json), "proof": .string(proof), "signature": .string(signature)] }
    public static func parse(_ json: JSONObject) throws -> WorkspaceCandidate {
        try WorkspaceProtocol.fields(json, ["version", "workspace_id", "invitation_id", "purpose", "device", "proof", "signature"])
        guard try WorkspaceProtocol.number(json, "version") == 1, let deviceJSON = json["device"]?.object else { throw ZerusError.invalid("Invalid enrollment candidate.") }
        let device = try WorkspaceDevice.parse(deviceJSON)
        guard try WorkspaceProtocol.string(json, "purpose") == device.role else { throw ZerusError.invalid("Invalid enrollment purpose.") }
        let result = try WorkspaceCandidate(workspaceId: WorkspaceProtocol.string(json, "workspace_id"), invitationId: WorkspaceProtocol.string(json, "invitation_id"), device: device,
            proof: WorkspaceProtocol.string(json, "proof"), signature: WorkspaceProtocol.string(json, "signature"))
        try WorkspaceProtocol.identifier(result.workspaceId); try WorkspaceProtocol.identifier(result.invitationId); try result.verify()
        return result
    }
}
