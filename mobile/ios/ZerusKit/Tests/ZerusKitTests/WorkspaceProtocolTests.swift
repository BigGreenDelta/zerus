import Foundation
import XCTest
@testable import ZerusKit

final class WorkspaceProtocolTests: XCTestCase {
    func fixture() throws -> JSONObject { try XCTUnwrap(SharedFixture.json("encrypted-workspace-v1.json").object) }
    func hex(_ data: Data) -> String { data.map { String(format: "%02x", $0) }.joined() }

    func testRustHpkeAndEd25519VectorOpensWithNativeProvider() throws {
        let f = try fixture(), gateway = try WorkspaceIdentity.parse(XCTUnwrap(f["gateway_identity"]?.object)), phone = try WorkspaceIdentity.parse(XCTUnwrap(f["phone_identity"]?.object))
        let envelope = try WorkspaceEnvelope.parse(XCTUnwrap(f["envelope"]?.object))
        XCTAssertEqual(f.string("header_hex"), try hex(envelope.header.frame()))
        XCTAssertEqual(try WorkspaceProtocol.decode(f.string("plaintext_b64url")), try envelope.open(identity: gateway, sender: phone.device, expected: envelope.header))
        let changed = try WorkspaceHeader(workspaceId: envelope.header.workspaceId, senderDeviceId: envelope.header.senderDeviceId, recipientDeviceId: envelope.header.recipientDeviceId,
            senderKeyGeneration: 1, recipientKeyGeneration: 1, gatewayId: envelope.header.gatewayId, routeId: "substituted", requestId: envelope.header.requestId,
            kind: "request", operationClass: "mutation", expiresAt: envelope.header.expiresAt, membershipRevision: envelope.header.membershipRevision, membershipHead: envelope.header.membershipHead)
        XCTAssertThrowsError(try envelope.open(identity: gateway, sender: phone.device, expected: changed))
        let forged = WorkspaceEnvelope(header: envelope.header, enc: envelope.enc, ciphertext: envelope.ciphertext, signature: WorkspaceProtocol.encode(Data(repeating: 0, count: 64)))
        XCTAssertThrowsError(try forged.open(identity: gateway, sender: phone.device, expected: envelope.header))
    }

    func testCopiedSecretBindsExactCandidateAndInviterPinnedBootstrap() throws {
        let f = try fixture(), phone = try WorkspaceIdentity.parse(XCTUnwrap(f["phone_identity"]?.object))
        let invitation = try WorkspaceInvitation.parse("ze1." + WorkspaceProtocol.encode(JSON.object(XCTUnwrap(f["invitation"]?.object)).data()))
        let generated = try invitation.candidate(identity: phone, now: 1)
        // CryptoKit deliberately randomizes Ed25519 signatures for side-channel resistance.
        // Compare canonical bytes and verify its signature; the fixed approval names the
        // exact persisted Rust candidate, including that candidate's valid signature.
        try generated.verify()
        let candidate = try WorkspaceCandidate.parse(XCTUnwrap(f["candidate"]?.object))
        XCTAssertEqual(candidate.device, generated.device)
        XCTAssertEqual(candidate.proof, generated.proof)
        XCTAssertEqual(try candidate.signatureFrame(), try generated.signatureFrame())
        XCTAssertEqual(f.string("device_hex"), try hex(candidate.device.frame()))
        XCTAssertEqual(f.string("candidate_proof_hex"), try hex(candidate.proofFrame()))
        XCTAssertEqual(f.string("candidate_signing_hex"), try hex(candidate.signatureFrame()))
        XCTAssertFalse(candidate.json.compactJSON.contains(f["invitation"]!.string("secret")))
        let bootstrap = try WorkspaceBootstrap.parse(XCTUnwrap(f["bootstrap_snapshot"]?.object))
        XCTAssertEqual(f.string("snapshot_signing_hex"), try hex(bootstrap.signatureFrame()))
        let state = try bootstrap.verify(workspaceId: invitation.workspaceId, revision: invitation.membershipRevision, head: invitation.membershipHead, issuer: invitation.issuer)
        let approval = try WorkspaceApproval.parse(XCTUnwrap(f["approval"]?.object))
        try approval.verify(candidate: candidate, issuer: invitation.issuer)
        XCTAssertEqual(f.string("approval_signing_hex"), try hex(approval.signatureFrame()))
        XCTAssertEqual(try WorkspaceMembership.parse(XCTUnwrap(f["final_state"]?.object)), try state.applying(approval.membership))
        XCTAssertThrowsError(try state.applying(WorkspaceMembershipChange(workspaceId: approval.membership.workspaceId, revision: 2, previousHead: approval.membership.previousHead,
            issuerDeviceId: approval.membership.issuerDeviceId, issuerKeyGeneration: 1, action: "add", device: approval.membership.device, revokedDeviceId: nil, signature: approval.membership.signature)))
        XCTAssertThrowsError(try bootstrap.verify(workspaceId: invitation.workspaceId, revision: 1, head: invitation.membershipHead, issuer: invitation.issuer))
    }

    func testWeakAndNoncanonicalPublicKeysAreRejectedAcrossNativeProviders() throws {
        let fixture = try SharedFixture.json("encrypted-workspace-rejected-keys-v1.json")
        let keys = Set(fixture.strings("ed25519_rejected_public_keys"))
        XCTAssertEqual(keys.count, 48); XCTAssertEqual(keys, WorkspaceRejectedKeys.signing)
        let device = try WorkspaceDevice.parse(XCTUnwrap(self.fixture()["phone_identity"]?["device"]?.object))
        for key in keys {
            XCTAssertThrowsError(try WorkspaceDevice(deviceId: device.deviceId, role: device.role, keyGeneration: 1, signingPublicKey: key, encryptionPublicKey: device.encryptionPublicKey))
        }
        XCTAssertThrowsError(try WorkspaceDevice(deviceId: device.deviceId, role: device.role, keyGeneration: 1, signingPublicKey: device.signingPublicKey, encryptionPublicKey: WorkspaceProtocol.encode(Data(repeating: 0, count: 32))))
    }
    func testCorruptStoredPrivateKeysNeverGenerateReplacementIdentity() throws {
        var record = try XCTUnwrap(fixture()["phone_identity"]?.object)
        record["signing_private_key"] = .string(WorkspaceProtocol.encode(Data(repeating: 0, count: 32)))
        XCTAssertThrowsError(try WorkspaceIdentity.parse(record))
        for value in ["=", "AA=", "A+", "AA/", "A"] { XCTAssertThrowsError(try WorkspaceProtocol.decode(value)) }
    }
}
