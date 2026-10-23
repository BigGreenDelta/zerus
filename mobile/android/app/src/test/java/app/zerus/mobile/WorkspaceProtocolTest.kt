package app.zerus.mobile

import java.io.File
import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Test

class WorkspaceProtocolTest {
    private fun fixture():JSONObject = JSONObject(requireNotNull(javaClass.classLoader!!.getResource("encrypted-workspace-v1.json")).readText())
    private fun ByteArray.hex()=joinToString("") { "%02x".format(it) }
    @Test fun rustHpkeAndEd25519VectorOpensWithNativeProvider() {
        val f=fixture();val gateway=WorkspaceIdentity.parse(f.getJSONObject("gateway_identity"));val phone=WorkspaceIdentity.parse(f.getJSONObject("phone_identity"))
        val envelope=WorkspaceEnvelope.parse(f.getJSONObject("envelope"))
        assertEquals(f.getString("header_hex"),envelope.header.frame().hex())
        assertArrayEquals(WorkspaceProtocol.decode(f.getString("plaintext_b64url")),envelope.open(gateway,phone.device,envelope.header))
        assertThrows(IllegalArgumentException::class.java) { envelope.open(gateway,phone.device,envelope.header.copy(routeId="substituted")) }
        assertTrue(runCatching { envelope.copy(signature=WorkspaceProtocol.encode(ByteArray(64))).open(gateway,phone.device,envelope.header) }.isFailure)
    }
    @Test fun copiedSecretBindsExactCandidateAndInviterPinnedBootstrap() {
        val f=fixture();val phone=WorkspaceIdentity.parse(f.getJSONObject("phone_identity"));val code="ze1."+WorkspaceProtocol.encode(f.getJSONObject("invitation").toString().toByteArray())
        val invitation=WorkspaceInvitation.parse(code);val candidate=invitation.candidate(phone,1)
        assertEquals(WorkspaceCandidate.parse(f.getJSONObject("candidate")),candidate)
        assertEquals(f.getString("device_hex"),candidate.device.frame().hex())
        assertEquals(f.getString("candidate_proof_hex"),candidate.proofFrame().hex())
        assertEquals(f.getString("candidate_signing_hex"),candidate.signatureFrame().hex())
        assertFalse(candidate.json().toString().contains(f.getJSONObject("invitation").getString("secret")))
        val bootstrap=WorkspaceBootstrap.parse(f.getJSONObject("bootstrap_snapshot"))
        assertEquals(f.getString("snapshot_signing_hex"),bootstrap.signatureFrame().hex())
        val state=bootstrap.verify(invitation.workspaceId,invitation.membershipRevision,invitation.membershipHead,invitation.issuer)
        val approval=WorkspaceApproval.parse(f.getJSONObject("approval"));approval.verify(candidate,invitation.issuer)
        assertEquals(f.getString("approval_signing_hex"),approval.signatureFrame().hex())
        assertEquals(WorkspaceMembership.parse(f.getJSONObject("final_state")),state.apply(approval.membership))
        assertThrows(IllegalArgumentException::class.java) { state.apply(approval.membership.copy(previousHead=WorkspaceProtocol.encode(ByteArray(32)))) }
        assertThrows(IllegalArgumentException::class.java) { bootstrap.verify(invitation.workspaceId,1,invitation.membershipHead,invitation.issuer) }
    }
    @Test fun weakAndNoncanonicalPublicKeysAreRejectedAcrossNativeProviders() {
        val vector=JSONObject(javaClass.classLoader!!.getResource("encrypted-workspace-rejected-keys-v1.json")!!.readText()).getJSONArray("ed25519_rejected_public_keys")
        val keys=(0 until vector.length()).map { vector.getString(it) }.toSet()
        assertEquals(48,keys.size);assertEquals(keys,WorkspaceRejectedKeys.signing)
        val device=WorkspaceDevice.parse(fixture().getJSONObject("phone_identity").getJSONObject("device"))
        keys.forEach { key -> assertThrows(IllegalArgumentException::class.java) { device.copy(signingPublicKey=key) } }
        assertThrows(IllegalArgumentException::class.java) { device.copy(encryptionPublicKey=WorkspaceProtocol.encode(ByteArray(32))) }
    }
    @Test fun corruptStoredPrivateKeysNeverGenerateReplacementIdentity() {
        val f=fixture();val record=f.getJSONObject("phone_identity")
        record.put("signing_private_key",WorkspaceProtocol.encode(ByteArray(32)))
        assertTrue(runCatching { WorkspaceIdentity.parse(record) }.isFailure)
        listOf("=","AA=","A+","AA/","A").forEach { assertTrue(runCatching { WorkspaceProtocol.decode(it) }.isFailure) }
        val root=generateSequence(File("").absoluteFile) { it.parentFile }.first { File(it,"services/mobile/crypto/tests/vectors/protocol-v1.json").isFile }
        assertEquals(File(root,"services/mobile/crypto/tests/vectors/protocol-v1.json").readText(),javaClass.classLoader!!.getResource("encrypted-workspace-v1.json")!!.readText())
    }
}
