package app.zerus.mobile

import org.json.JSONArray
import org.json.JSONObject
import java.security.MessageDigest

internal data class WorkspaceMembershipChange(val workspaceId: String, val revision: Long, val previousHead: String,
    val issuerDeviceId: String, val issuerKeyGeneration: Long, val action: String, val device: WorkspaceDevice?,
    val revokedDeviceId: String?, val signature: String) {
    fun frame(): ByteArray {
        WorkspaceProtocol.identifier(workspaceId);WorkspaceProtocol.identifier(issuerDeviceId)
        require(revision >= 0 && issuerKeyGeneration > 0 &&
            (action == "add" && device != null && revokedDeviceId == null || action == "revoke" && device == null && revokedDeviceId != null))
        revokedDeviceId?.let(WorkspaceProtocol::identifier)
        return WorkspaceProtocol.frame("zerus-e2ee-membership-v1",WorkspaceProtocol.text(workspaceId),WorkspaceProtocol.integer(revision),
            WorkspaceProtocol.decode(previousHead,32),WorkspaceProtocol.text(issuerDeviceId),WorkspaceProtocol.integer(issuerKeyGeneration),
            WorkspaceProtocol.text(action),device?.frame() ?: byteArrayOf(),WorkspaceProtocol.text(revokedDeviceId.orEmpty()))
    }
    fun verify(issuer: WorkspaceDevice) {
        require(issuer.role == "computer" && issuer.deviceId == issuerDeviceId && issuer.keyGeneration == issuerKeyGeneration)
        WorkspaceCrypto.verify(WorkspaceProtocol.decode(issuer.signingPublicKey,32),WorkspaceProtocol.decode(signature,64),frame())
    }
    fun head(): String = WorkspaceProtocol.encode(MessageDigest.getInstance("SHA-256").digest(WorkspaceProtocol.frame(
        "zerus-e2ee-membership-head-v1",frame(),WorkspaceProtocol.decode(signature,64))))
    fun json(): JSONObject = JSONObject().put("version",1).put("statement",JSONObject().put("workspace_id",workspaceId)
        .put("revision",revision).put("previous_head",previousHead).put("issuer_device_id",issuerDeviceId)
        .put("issuer_key_generation",issuerKeyGeneration).put("action",action).put("device",device?.json() ?: JSONObject.NULL)
        .put("revoked_device_id",revokedDeviceId ?: JSONObject.NULL)).put("signature",signature)
    companion object {
        fun parse(json: JSONObject): WorkspaceMembershipChange {
            WorkspaceProtocol.exactFields(json,setOf("version","statement","signature"));require(WorkspaceProtocol.number(json,"version")==1L)
            val s=json.getJSONObject("statement")
            WorkspaceProtocol.exactFields(s,setOf("workspace_id","revision","previous_head","issuer_device_id","issuer_key_generation","action","device","revoked_device_id"))
            return WorkspaceMembershipChange(WorkspaceProtocol.string(s,"workspace_id"),WorkspaceProtocol.number(s,"revision"),WorkspaceProtocol.string(s,"previous_head"),
                WorkspaceProtocol.string(s,"issuer_device_id"),WorkspaceProtocol.number(s,"issuer_key_generation"),WorkspaceProtocol.string(s,"action"),
                if(s.isNull("device")) null else WorkspaceDevice.parse(s.getJSONObject("device")),
                if(s.isNull("revoked_device_id")) null else WorkspaceProtocol.string(s,"revoked_device_id"),WorkspaceProtocol.string(json,"signature")).also { it.frame();WorkspaceProtocol.decode(it.signature,64) }
        }
    }
}

/** A relay response can extend this state only through a verified exact successor. */
internal data class WorkspaceMembership(val workspaceId: String, val revision: Long, val head: String,
    val members: Map<String,WorkspaceDevice>, val revokedDeviceIds: Set<String>) {
    init {
        WorkspaceProtocol.identifier(workspaceId);require(revision>=0);WorkspaceProtocol.decode(head,32)
        require(members.size+revokedDeviceIds.size<=4096 && members.all { it.key==it.value.deviceId } &&
            revokedDeviceIds.none { it in members })
        revokedDeviceIds.forEach(WorkspaceProtocol::identifier)
    }
    fun apply(change: WorkspaceMembershipChange): WorkspaceMembership {
        require(revision<Long.MAX_VALUE && change.workspaceId==workspaceId && change.revision==revision+1 && change.previousHead==head) { "Workspace membership changed or was rolled back." }
        val issuer=members[change.issuerDeviceId] ?: error("Unrecognized workspace member.")
        change.verify(issuer)
        val next=members.toMutableMap();val removed=revokedDeviceIds.toMutableSet()
        if(change.action=="add") {
            val device=requireNotNull(change.device)
            require(device.deviceId !in next && device.deviceId !in removed && next.size+removed.size<4096)
            next[device.deviceId]=device
        } else {
            val id=requireNotNull(change.revokedDeviceId);require(id in next) { "Unrecognized revoked member." }
            next.remove(id);removed.add(id)
        }
        return WorkspaceMembership(workspaceId,change.revision,change.head(),next,removed)
    }
    fun sender(envelope: WorkspaceEnvelope): WorkspaceDevice {
        val h=envelope.header
        require(h.workspaceId==workspaceId && h.membershipRevision==revision && h.membershipHead==head) { "Workspace membership must be updated before opening this payload." }
        return members[h.senderDeviceId] ?: error("Unrecognized workspace sender.")
    }
    fun frame():ByteArray {
        val fields=mutableListOf(WorkspaceProtocol.text(workspaceId),WorkspaceProtocol.integer(revision),WorkspaceProtocol.decode(head,32),WorkspaceProtocol.integer(members.size.toLong()))
        members.toSortedMap().values.forEach { fields+=it.frame() }
        fields+=WorkspaceProtocol.integer(revokedDeviceIds.size.toLong());revokedDeviceIds.sorted().forEach { fields+=WorkspaceProtocol.text(it) }
        return WorkspaceProtocol.frame("zerus-e2ee-membership-state-v1",*fields.toTypedArray())
    }
    fun json():JSONObject = JSONObject().put("workspace_id",workspaceId).put("revision",revision).put("head",head)
        .put("members",JSONObject().also { out -> members.toSortedMap().forEach { (id,device) -> out.put(id,device.json()) } })
        .put("revoked_device_ids",JSONArray(revokedDeviceIds.sorted()))
    companion object {
        fun parse(json:JSONObject):WorkspaceMembership {
            WorkspaceProtocol.exactFields(json,setOf("workspace_id","revision","head","members","revoked_device_ids"))
            val map=json.getJSONObject("members");val revoked=json.getJSONArray("revoked_device_ids")
            require(map.length()+revoked.length()<=4096)
            val ids=(0 until revoked.length()).map { revoked.get(it) as? String ?: error("Invalid revoked workspace device.") }
            require(ids.toSet().size==ids.size)
            return WorkspaceMembership(WorkspaceProtocol.string(json,"workspace_id"),WorkspaceProtocol.number(json,"revision"),WorkspaceProtocol.string(json,"head"),
                map.keys().asSequence().associateWith { WorkspaceDevice.parse(map.getJSONObject(it)) },ids.toSet())
        }
    }
}

internal data class WorkspaceBootstrap(val state:WorkspaceMembership,val issuerDeviceId:String,val issuerKeyGeneration:Long,val signature:String) {
    fun signatureFrame():ByteArray = WorkspaceProtocol.frame("zerus-e2ee-membership-snapshot-v1",state.frame(),WorkspaceProtocol.text(issuerDeviceId),WorkspaceProtocol.integer(issuerKeyGeneration))
    fun verify(workspaceId:String,revision:Long,head:String,issuer:WorkspaceDevice):WorkspaceMembership {
        require(state.workspaceId==workspaceId && state.revision==revision && state.head==head && issuerDeviceId==issuer.deviceId &&
            issuerKeyGeneration==issuer.keyGeneration && issuer.role=="computer" && state.members[issuerDeviceId]==issuer) { "Workspace invitation trust does not match this snapshot." }
        WorkspaceCrypto.verify(WorkspaceProtocol.decode(issuer.signingPublicKey,32),WorkspaceProtocol.decode(signature,64),signatureFrame())
        return state
    }
    companion object {
        fun parse(json:JSONObject):WorkspaceBootstrap {
            WorkspaceProtocol.exactFields(json,setOf("version","state","issuer_device_id","issuer_key_generation","signature"));require(WorkspaceProtocol.number(json,"version")==1L)
            return WorkspaceBootstrap(WorkspaceMembership.parse(json.getJSONObject("state")),WorkspaceProtocol.string(json,"issuer_device_id"),
                WorkspaceProtocol.number(json,"issuer_key_generation"),WorkspaceProtocol.string(json,"signature")).also {
                WorkspaceProtocol.identifier(it.issuerDeviceId);require(it.issuerKeyGeneration>0);WorkspaceProtocol.decode(it.signature,64)
            }
        }
    }
}

internal data class WorkspaceApproval(val invitationId:String,val candidateDigest:String,val membership:WorkspaceMembershipChange,val signature:String) {
    fun json():JSONObject=JSONObject().put("version",1).put("invitation_id",invitationId).put("candidate_digest",candidateDigest).put("membership",membership.json()).put("signature",signature)
    fun signatureFrame():ByteArray = WorkspaceProtocol.frame("zerus-e2ee-enrollment-approval-v1",WorkspaceProtocol.text(invitationId),
        WorkspaceProtocol.decode(candidateDigest,32),membership.frame(),WorkspaceProtocol.decode(membership.signature,64))
    fun verify(candidate:WorkspaceCandidate,issuer:WorkspaceDevice) {
        candidate.verify()
        require(invitationId==candidate.invitationId && candidateDigest==candidate.digest() && membership.workspaceId==candidate.workspaceId &&
            membership.action=="add" && membership.device==candidate.device) { "Workspace approval belongs to another device." }
        membership.verify(issuer)
        WorkspaceCrypto.verify(WorkspaceProtocol.decode(issuer.signingPublicKey,32),WorkspaceProtocol.decode(signature,64),signatureFrame())
    }
    companion object {
        fun parse(json:JSONObject):WorkspaceApproval {
            WorkspaceProtocol.exactFields(json,setOf("version","invitation_id","candidate_digest","membership","signature"));require(WorkspaceProtocol.number(json,"version")==1L)
            return WorkspaceApproval(WorkspaceProtocol.string(json,"invitation_id"),WorkspaceProtocol.string(json,"candidate_digest"),
                WorkspaceMembershipChange.parse(json.getJSONObject("membership")),WorkspaceProtocol.string(json,"signature")).also {
                WorkspaceProtocol.identifier(it.invitationId);WorkspaceProtocol.decode(it.candidateDigest,32);WorkspaceProtocol.decode(it.signature,64)
            }
        }
    }
}

/** Secret material stays in memory and never enters the candidate's HTTP JSON. */
internal class WorkspaceInvitation(val workspaceId:String,val invitationId:String,val issuer:WorkspaceDevice,val expiresAt:Long,
    val membershipRevision:Long,val membershipHead:String,private val secret:ByteArray) {
    fun candidate(identity:WorkspaceIdentity,now:Long):WorkspaceCandidate {
        require(now<expiresAt && identity.device.role=="phone") { "This phone invitation has expired." }
        val unsigned=WorkspaceCandidate(workspaceId,invitationId,identity.device,"","")
        val proof=WorkspaceProtocol.encode(WorkspaceCrypto.hmac(secret,unsigned.proofFrame()))
        val proven=unsigned.copy(proof=proof)
        return proven.copy(signature=WorkspaceProtocol.encode(WorkspaceCrypto.sign(identity.keys,proven.signatureFrame())))
    }
    companion object {
        fun parse(code:String):WorkspaceInvitation {
            require(code.startsWith("ze1.") && code.length<=4096) { "Invalid encrypted phone invitation." }
            val json=JSONObject(String(WorkspaceProtocol.decode(code.removePrefix("ze1."),max=3072),Charsets.UTF_8))
            WorkspaceProtocol.exactFields(json,setOf("version","workspace_id","invitation_id","purpose","issuer","expires_at","membership_revision","membership_head","secret"))
            require(WorkspaceProtocol.number(json,"version")==1L && WorkspaceProtocol.string(json,"purpose")=="phone") { "Use a phone invitation from an enrolled computer." }
            val issuer=WorkspaceDevice.parse(json.getJSONObject("issuer"));require(issuer.role=="computer")
            val result=WorkspaceInvitation(WorkspaceProtocol.string(json,"workspace_id"),WorkspaceProtocol.string(json,"invitation_id"),issuer,
                WorkspaceProtocol.number(json,"expires_at"),WorkspaceProtocol.number(json,"membership_revision"),WorkspaceProtocol.string(json,"membership_head"),
                WorkspaceProtocol.decode(WorkspaceProtocol.string(json,"secret"),32))
            WorkspaceProtocol.identifier(result.workspaceId);WorkspaceProtocol.identifier(result.invitationId)
            require(result.expiresAt>0);WorkspaceProtocol.decode(result.membershipHead,32)
            return result
        }
    }
}

internal data class WorkspaceCandidate(val workspaceId:String,val invitationId:String,val device:WorkspaceDevice,val proof:String,val signature:String) {
    fun proofFrame():ByteArray = WorkspaceProtocol.frame("zerus-e2ee-enrollment-proof-v1",WorkspaceProtocol.text(workspaceId),WorkspaceProtocol.text(invitationId),WorkspaceProtocol.text(device.role),device.frame())
    fun signatureFrame():ByteArray = WorkspaceProtocol.frame("zerus-e2ee-enrollment-candidate-v1",proofFrame(),WorkspaceProtocol.decode(proof,32))
    fun digest():String = WorkspaceProtocol.encode(MessageDigest.getInstance("SHA-256").digest(WorkspaceProtocol.frame("zerus-e2ee-candidate-digest-v1",signatureFrame(),WorkspaceProtocol.decode(signature,64))))
    fun verify() = WorkspaceCrypto.verify(WorkspaceProtocol.decode(device.signingPublicKey,32),WorkspaceProtocol.decode(signature,64),signatureFrame())
    fun json():JSONObject = JSONObject().put("version",1).put("workspace_id",workspaceId).put("invitation_id",invitationId).put("purpose",device.role)
        .put("device",device.json()).put("proof",proof).put("signature",signature)
    companion object {
        fun parse(json:JSONObject):WorkspaceCandidate {
            WorkspaceProtocol.exactFields(json,setOf("version","workspace_id","invitation_id","purpose","device","proof","signature"));require(WorkspaceProtocol.number(json,"version")==1L)
            val device=WorkspaceDevice.parse(json.getJSONObject("device"));require(WorkspaceProtocol.string(json,"purpose")==device.role)
            return WorkspaceCandidate(WorkspaceProtocol.string(json,"workspace_id"),WorkspaceProtocol.string(json,"invitation_id"),device,WorkspaceProtocol.string(json,"proof"),WorkspaceProtocol.string(json,"signature")).also {
                WorkspaceProtocol.identifier(it.workspaceId);WorkspaceProtocol.identifier(it.invitationId);it.verify()
            }
        }
    }
}
