package app.zerus.mobile

import android.app.Activity
import android.app.Instrumentation
import android.content.ContextWrapper
import android.os.Build
import android.os.Bundle
import java.io.File
import java.security.SecureRandom
import java.util.UUID

/** Isolated emulator-only Keystore/SQLite qualification. No app state or network is used. */
class WorkspaceJournalSmokeInstrumentation:Instrumentation() {
    override fun onCreate(arguments:Bundle?) { super.onCreate(arguments);start() }
    override fun onStart() {
        val result=Bundle();var connection:Connection?=null;var journal:AndroidWorkspaceJournal?=null
        var outcome=Activity.RESULT_CANCELED
        val directory=File(targetContext.cacheDir,"encrypted-journal-smoke-${UUID.randomUUID()}")
        try {
            check(BuildConfig.DEBUG && Build.HARDWARE in setOf("ranchu","goldfish"))
            check(directory.mkdirs())
            val context=object:ContextWrapper(targetContext) { override fun getNoBackupFilesDir()=directory }
            val keys=WorkspaceCrypto.generate();val gateway=WorkspaceIdentity(WorkspaceDevice(UUID.randomUUID().toString(),"computer",1,
                WorkspaceProtocol.encode(keys.signingPublic),WorkspaceProtocol.encode(keys.encryptionPublic)),keys)
            val phone=WorkspaceIdentity.generate(UUID.randomUUID().toString());val workspace=UUID.randomUUID().toString()
            val head=WorkspaceProtocol.encode(ByteArray(32) { 3 });val now=System.currentTimeMillis()/1000
            val invitation=WorkspaceInvitation(workspace,UUID.randomUUID().toString(),gateway.device,now+300,0,head,
                ByteArray(32).also { SecureRandom().nextBytes(it) })
            val candidate=invitation.candidate(phone,now)
            val unsigned=WorkspaceMembershipChange(workspace,1,head,gateway.device.deviceId,1,"add",phone.device,null,"")
            val change=unsigned.copy(signature=WorkspaceProtocol.encode(WorkspaceCrypto.sign(keys,unsigned.frame())))
            val initial=WorkspaceMembership(workspace,0,head,mapOf(gateway.device.deviceId to gateway.device),emptySet())
            val approvalUnsigned=WorkspaceApproval(candidate.invitationId,candidate.digest(),change,"")
            val approval=approvalUnsigned.copy(signature=WorkspaceProtocol.encode(WorkspaceCrypto.sign(keys,approvalUnsigned.signatureFrame())))
            connection=Connection(UUID.randomUUID().toString(),"Synthetic","https://relay.example","synthetic-bearer",2)
            val pending=WorkspaceState(phone,candidate,gateway.device,0,head,null)
            journal=AndroidWorkspaceJournal(context);journal.create(connection,pending)
            val bootstrapped=pending.withMembership(initial).withEnrollment(approval)
            journal.update(connection,pending,bootstrapped)
            // A valid maximum-size route map also exceeds a default CursorWindow cell.
            val routes=(0 until 4096).associate { index ->
                val id="native-${index.toString().padStart(6,'0')}-"+"m".repeat(108)
                id to WorkspaceRoute(id,gateway.device.deviceId,"r".repeat(128),id)
            }
            val active=bootstrapped.withMembership(initial.apply(change)).withRoutes(routes)
            check(active.json().toString().toByteArray().size>2*1024*1024)
            journal.update(connection,bootstrapped,active,listOf(change))
            val id=UUID.randomUUID().toString()
            val header=WorkspaceHeader(workspace,phone.device.deviceId,gateway.device.deviceId,1,1,gateway.device.deviceId,
                gateway.device.deviceId,id,"request","mutation",now+300,1,change.head())
            val original=WorkspaceEnvelope.seal(phone,gateway.device,header,ByteArray(3*1024*1024) { 7 })
            journal.reserve(connection,id,"synthetic-plaintext-digest",original)
            // A separate database handle follows the same recovery path as a new process.
            val reopened=AndroidWorkspaceJournal(context);journal=reopened
            check(reopened.load(connection).json().toString()==active.json().toString())
            check(reopened.head(connection,0)==head && reopened.head(connection,1)==change.head())
            val fresh=WorkspaceEnvelope.seal(phone,gateway.device,header,ByteArray(3*1024*1024) { 7 })
            check(reopened.reserve(connection,id,"synthetic-plaintext-digest",fresh).json().toString()==original.json().toString())
            check(runCatching { reopened.reserve(connection,id,"altered-plaintext-digest",fresh) }.isFailure)
            reopened.settle(connection,id)
            check(reopened.request(connection,id)==header)
            check(runCatching { reopened.reserve(connection,id,"synthetic-plaintext-digest",fresh) }.isFailure)
            reopened.forget(connection)
            check(runCatching { reopened.load(connection) }.isFailure)
            result.putString("stream","PASS: isolated large-state and large-ciphertext Keystore/SQLite recovery\n")
            outcome=Activity.RESULT_OK
        } catch(error:Throwable) {
            result.putString("stream",error.stackTraceToString())
        } finally {
            connection?.let { runCatching { journal?.forget(it) } }
            directory.deleteRecursively()
        }
        finish(outcome,result)
    }
}
