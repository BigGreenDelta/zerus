package app.zerus.mobile

import android.content.Context
import android.database.sqlite.SQLiteDatabase
import android.security.keystore.KeyGenParameterSpec
import android.security.keystore.KeyProperties
import org.json.JSONObject
import java.io.ByteArrayOutputStream
import java.io.File
import java.security.MessageDigest
import java.security.KeyStore
import javax.crypto.Cipher
import javax.crypto.SecretKey
import javax.crypto.KeyGenerator
import javax.crypto.spec.GCMParameterSpec

internal data class WorkspaceRoute(val computerId:String,val gatewayId:String,val routeId:String,val machineId:String) {
    fun json()=JSONObject().put("computer_id",computerId).put("gateway_id",gatewayId).put("route_id",routeId).put("machine_id",machineId)
    companion object { fun parse(j:JSONObject):WorkspaceRoute {
        WorkspaceProtocol.exactFields(j,setOf("computer_id","gateway_id","route_id","machine_id"))
        return WorkspaceRoute(WorkspaceProtocol.string(j,"computer_id"),WorkspaceProtocol.string(j,"gateway_id"),WorkspaceProtocol.string(j,"route_id"),WorkspaceProtocol.string(j,"machine_id")).also {
            listOf(it.computerId,it.gatewayId,it.routeId,it.machineId).forEach(WorkspaceProtocol::identifier)
        }
    } }
}

/** The secret-bearing state is sealed by the platform journal, never logged. */
internal class WorkspaceState(val identity:WorkspaceIdentity,val candidate:WorkspaceCandidate,val issuer:WorkspaceDevice,
    val pinnedRevision:Long,val pinnedHead:String,val membership:WorkspaceMembership?,val routes:Map<String,WorkspaceRoute> = emptyMap(),val enrollment:WorkspaceApproval? = null) {
    init {
        require(identity.device==candidate.device && candidate.device.role=="phone" && issuer.role=="computer" && pinnedRevision>=0 && routes.size<=4096)
        WorkspaceProtocol.decode(pinnedHead,32)
        require(membership==null || membership.workspaceId==candidate.workspaceId)
    }
    fun json():JSONObject = JSONObject().put("version",1).put("identity",identity.privateJson()).put("candidate",candidate.json())
        .put("issuer",issuer.json()).put("pinned_revision",pinnedRevision).put("pinned_head",pinnedHead)
        .put("membership",membership?.json() ?: JSONObject.NULL).put("routes",JSONObject().also { j -> routes.toSortedMap().forEach { (id,route) -> j.put(id,route.json()) } }).put("enrollment",enrollment?.json() ?: JSONObject.NULL)
    fun withMembership(next:WorkspaceMembership)=WorkspaceState(identity,candidate,issuer,pinnedRevision,pinnedHead,next,routes.filterValues { it.gatewayId in next.members },enrollment)
    fun withRoutes(next:Map<String,WorkspaceRoute>)=WorkspaceState(identity,candidate,issuer,pinnedRevision,pinnedHead,membership,next,enrollment)
    fun withEnrollment(next:WorkspaceApproval)=WorkspaceState(identity,candidate,issuer,pinnedRevision,pinnedHead,membership,routes,next)
    companion object { fun parse(j:JSONObject):WorkspaceState {
        WorkspaceProtocol.exactFields(j,setOf("version","identity","candidate","issuer","pinned_revision","pinned_head","membership","routes","enrollment"));require(WorkspaceProtocol.number(j,"version")==1L)
        val routes=j.getJSONObject("routes");require(routes.length()<=4096)
        return WorkspaceState(WorkspaceIdentity.parse(j.getJSONObject("identity")),WorkspaceCandidate.parse(j.getJSONObject("candidate")),WorkspaceDevice.parse(j.getJSONObject("issuer")),
            WorkspaceProtocol.number(j,"pinned_revision"),WorkspaceProtocol.string(j,"pinned_head"),if(j.isNull("membership")) null else WorkspaceMembership.parse(j.getJSONObject("membership")),
            routes.keys().asSequence().associateWith { WorkspaceRoute.parse(routes.getJSONObject(it)).also { r -> require(r.computerId==it) } },
            if(j.isNull("enrollment")) null else WorkspaceApproval.parse(j.getJSONObject("enrollment")))
    } }
}

internal interface WorkspaceJournal {
    fun create(connection:Connection,state:WorkspaceState)
    fun load(connection:Connection):WorkspaceState
    /** State, each consumed signed change and revision->head index share one transaction. */
    fun update(connection:Connection,expected:WorkspaceState,next:WorkspaceState,changes:List<WorkspaceMembershipChange> = emptyList())
    fun head(connection:Connection,revision:Long):String?
    /** Reserves exact ciphertext before HTTP. Existing identities may only be checked, never re-created. */
    fun reserve(connection:Connection,id:String,plainHash:String,envelope:WorkspaceEnvelope):WorkspaceEnvelope
    fun recordedRequest(connection:Connection,id:String):WorkspaceHeader?
    fun request(connection:Connection,id:String):WorkspaceHeader=checkNotNull(recordedRequest(connection,id)) { "The original encrypted request is unavailable. Delivery cannot be confirmed." }
    fun open(connection:Connection,envelope:WorkspaceEnvelope,expected:WorkspaceHeader):ByteArray {
        val state=load(connection);val membership=checkNotNull(state.membership)
        require(membership.members[state.identity.device.deviceId]==state.identity.device)
        val sender=membership.senderAtKnownHead(envelope,head(connection,envelope.header.membershipRevision));require(sender.role=="computer")
        return envelope.open(state.identity,sender,expected)
    }
    fun settle(connection:Connection,id:String)
    fun forget(connection:Connection)
}

/** Indexed durable crypto history and immutable outbox. No complete journal is ever loaded. */
internal class AndroidWorkspaceJournal(context:Context):WorkspaceJournal {
    private val file=File(context.noBackupFilesDir,"workspace-transport-v2.sqlite3")
    private val db:SQLiteDatabase by lazy { synchronized(LOCK) {
        SQLiteDatabase.openOrCreateDatabase(file,null).also {
            check(file.setReadable(false,false) && file.setWritable(false,false) && file.setReadable(true,true) && file.setWritable(true,true))
            it.execSQL("PRAGMA synchronous=FULL")
            it.execSQL("CREATE TABLE IF NOT EXISTS workspace_state (connection TEXT PRIMARY KEY,token_hash TEXT NOT NULL)")
            it.execSQL("CREATE TABLE IF NOT EXISTS membership (connection TEXT NOT NULL,revision INTEGER NOT NULL,head TEXT NOT NULL,change TEXT,PRIMARY KEY(connection,revision))")
            it.execSQL("CREATE TABLE IF NOT EXISTS outgoing (connection TEXT NOT NULL,request_id TEXT NOT NULL,plain_hash TEXT NOT NULL,header TEXT NOT NULL,envelope_bytes INTEGER,PRIMARY KEY(connection,request_id))")
            it.execSQL("CREATE TABLE IF NOT EXISTS workspace_blobs (connection TEXT NOT NULL,kind TEXT NOT NULL,object_id TEXT NOT NULL,part INTEGER NOT NULL,bytes BLOB NOT NULL,PRIMARY KEY(connection,kind,object_id,part))")
            it.execSQL("CREATE TABLE IF NOT EXISTS outgoing_quota (id INTEGER PRIMARY KEY CHECK(id=1),bytes INTEGER NOT NULL)")
            it.execSQL("INSERT OR IGNORE INTO outgoing_quota(id,bytes) VALUES(1,0)")
        }
    } }
    // API26/27 CursorWindow cannot grow to fit a large single cell. Keep private
    // state and outgoing ciphertext in small indexed rows, in their owning transaction.
    private fun putBlob(connection:Connection,kind:String,id:String,bytes:ByteArray,max:Int) {
        require(bytes.size in 1..max)
        db.execSQL("DELETE FROM workspace_blobs WHERE connection=? AND kind=? AND object_id=?",arrayOf(connection.id,kind,id))
        var offset=0;var part=0
        while(offset<bytes.size) {
            val end=minOf(offset+64*1024,bytes.size)
            db.execSQL("INSERT INTO workspace_blobs(connection,kind,object_id,part,bytes) VALUES(?,?,?,?,?)",arrayOf(connection.id,kind,id,part,bytes.copyOfRange(offset,end)))
            offset=end;part++
        }
    }
    private fun blob(connection:Connection,kind:String,id:String,max:Int):ByteArray {
        val bytes=ByteArrayOutputStream();var part=0L
        db.rawQuery("SELECT part,bytes FROM workspace_blobs WHERE connection=? AND kind=? AND object_id=? ORDER BY part",arrayOf(connection.id,kind,id)).use {
            while(it.moveToNext()) {
                check(it.getLong(0)==part++) { "Encrypted journal chunks are incomplete." }
                val chunk=it.getBlob(1);check(chunk.size in 1..64*1024 && bytes.size().toLong()+chunk.size<=max)
                bytes.write(chunk)
            }
        }
        check(part>0) { "Encrypted journal chunks are unavailable." }
        return bytes.toByteArray()
    }
    private fun hash(value:ByteArray)=WorkspaceProtocol.encode(MessageDigest.getInstance("SHA-256").digest(value))
    private fun token(connection:Connection)=hash(connection.token.toByteArray())
    private fun wrappingKey(connection:Connection,create:Boolean=false):SecretKey {
        WorkspaceProtocol.identifier(connection.id)
        val alias="zerus_workspace_v2:${connection.id}"
        val store=KeyStore.getInstance("AndroidKeyStore").apply { load(null) }
        (store.getKey(alias,null) as? SecretKey)?.let { return it }
        check(create) { "This encrypted workspace key is unavailable. Obtain a new phone invitation." }
        return KeyGenerator.getInstance(KeyProperties.KEY_ALGORITHM_AES,"AndroidKeyStore").apply {
            init(KeyGenParameterSpec.Builder(alias,KeyProperties.PURPOSE_ENCRYPT or KeyProperties.PURPOSE_DECRYPT)
                .setBlockModes(KeyProperties.BLOCK_MODE_GCM).setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_NONE).build())
        }.generateKey()
    }
    private fun seal(connection:Connection,state:WorkspaceState):ByteArray {
        val cipher=Cipher.getInstance("AES/GCM/NoPadding");cipher.init(Cipher.ENCRYPT_MODE,wrappingKey(connection));cipher.updateAAD(connection.id.toByteArray())
        return cipher.iv+cipher.doFinal(state.json().toString().toByteArray())
    }
    private fun open(connection:Connection,bytes:ByteArray):WorkspaceState {
        require(bytes.size>=28)
        val cipher=Cipher.getInstance("AES/GCM/NoPadding");cipher.init(Cipher.DECRYPT_MODE,wrappingKey(connection),GCMParameterSpec(128,bytes.copyOfRange(0,12)));cipher.updateAAD(connection.id.toByteArray())
        return WorkspaceState.parse(JSONObject(String(cipher.doFinal(bytes.copyOfRange(12,bytes.size)),Charsets.UTF_8)))
    }
    override fun create(connection:Connection,state:WorkspaceState)=synchronized(LOCK) {
        require(connection.protocolVersion==2)
        wrappingKey(connection,create=true)
        val record=seal(connection,state)
        db.beginTransaction()
        try {
            db.execSQL("INSERT INTO workspace_state(connection,token_hash) VALUES(?,?)",arrayOf(connection.id,token(connection)))
            putBlob(connection,"private","state",record,8*1024*1024)
            db.setTransactionSuccessful()
        } finally { db.endTransaction() }
    }
    override fun load(connection:Connection):WorkspaceState=synchronized(LOCK) {
        require(connection.protocolVersion==2)
        db.rawQuery("SELECT token_hash FROM workspace_state WHERE connection=?",arrayOf(connection.id)).use { c ->
            check(c.moveToFirst() && c.getString(0)==token(connection)) { "Encrypted workspace state is unavailable. Pair again using a new phone invitation." }
        }
        open(connection,blob(connection,"private","state",8*1024*1024))
    }
    override fun update(connection:Connection,expected:WorkspaceState,next:WorkspaceState,changes:List<WorkspaceMembershipChange>)=synchronized(LOCK) {
        db.beginTransaction()
        try {
            check(load(connection).json().toString()==expected.json().toString()) { "Encrypted workspace changed during this check. Refresh again." }
            require(expected.identity.device==next.identity.device && expected.candidate==next.candidate)
            putBlob(connection,"private","state",seal(connection,next),8*1024*1024)
            next.membership?.let { state ->
                if(expected.membership==null) db.execSQL("INSERT INTO membership(connection,revision,head,change) VALUES(?,?,?,NULL)",arrayOf<Any>(connection.id,next.pinnedRevision,next.pinnedHead))
                changes.forEach { change -> db.execSQL("INSERT INTO membership(connection,revision,head,change) VALUES(?,?,?,?)",arrayOf<Any>(connection.id,change.revision,change.head(),change.json().toString())) }
                check(head(connection,state.revision)==state.head) { "Workspace history was not saved with its authorization state." }
            }
            db.setTransactionSuccessful()
        } finally { db.endTransaction() }
    }
    override fun head(connection:Connection,revision:Long):String?=synchronized(LOCK) {
        db.rawQuery("SELECT head FROM membership WHERE connection=? AND revision=?",arrayOf(connection.id,revision.toString())).use { if(it.moveToFirst()) it.getString(0) else null }
    }
    override fun reserve(connection:Connection,id:String,plainHash:String,envelope:WorkspaceEnvelope):WorkspaceEnvelope=synchronized(LOCK) {
        db.beginTransaction()
        try {
            val state=load(connection);val membership=checkNotNull(state.membership)
            require(membership.members[state.identity.device.deviceId]==state.identity.device)
            membership.senderAtKnownHead(envelope,head(connection,envelope.header.membershipRevision))
            val retained=db.rawQuery("SELECT plain_hash,envelope_bytes FROM outgoing WHERE connection=? AND request_id=?",arrayOf(connection.id,id)).use {
                if(it.moveToFirst()) {
                    check(it.getString(0)==plainHash && !it.isNull(1)) { "This request identity is already retained. Check its original receipt." }
                    val bytes=blob(connection,"request",id,41*1024*1024);check(bytes.size.toLong()==it.getLong(1))
                    WorkspaceEnvelope.parse(JSONObject(String(bytes,Charsets.UTF_8)))
                } else null
            }
            if(retained!=null) { db.setTransactionSuccessful();return@synchronized retained }
            val text=envelope.json().toString()
            val encoded=text.toByteArray(Charsets.UTF_8)
            val bytes=encoded.size+envelope.header.json().toString().length+256L
            db.rawQuery("SELECT bytes FROM outgoing_quota WHERE id=1",emptyArray()).use {
                check(it.moveToFirst() && it.getLong(0)+bytes<=256L*1024*1024) { "Encrypted outgoing storage is full. Check outstanding deliveries." }
            }
            db.execSQL("INSERT INTO outgoing(connection,request_id,plain_hash,header,envelope_bytes) VALUES(?,?,?,?,?)",arrayOf(connection.id,id,plainHash,envelope.header.json().toString(),encoded.size))
            putBlob(connection,"request",id,encoded,41*1024*1024)
            db.execSQL("UPDATE outgoing_quota SET bytes=bytes+? WHERE id=1",arrayOf(bytes))
            db.setTransactionSuccessful()
            envelope
        } finally { db.endTransaction() }
    }
    override fun recordedRequest(connection:Connection,id:String):WorkspaceHeader?=synchronized(LOCK) {
        db.rawQuery("SELECT header FROM outgoing WHERE connection=? AND request_id=?",arrayOf(connection.id,id)).use {
            if(it.moveToFirst()) WorkspaceHeader.parse(JSONObject(it.getString(0))) else null
        }
    }
    override fun open(connection:Connection,envelope:WorkspaceEnvelope,expected:WorkspaceHeader):ByteArray=synchronized(LOCK) {
        super<WorkspaceJournal>.open(connection,envelope,expected)
    }
    override fun settle(connection:Connection,id:String)=synchronized(LOCK) {
        db.beginTransaction()
        try {
            val bytes=db.rawQuery("SELECT COALESCE(envelope_bytes,0) FROM outgoing WHERE connection=? AND request_id=?",arrayOf(connection.id,id)).use { if(it.moveToFirst()) it.getLong(0) else 0L }
            db.execSQL("UPDATE outgoing SET envelope_bytes=NULL WHERE connection=? AND request_id=?",arrayOf(connection.id,id))
            db.execSQL("DELETE FROM workspace_blobs WHERE connection=? AND kind='request' AND object_id=?",arrayOf(connection.id,id))
            db.execSQL("UPDATE outgoing_quota SET bytes=bytes-? WHERE id=1 AND bytes>=?",arrayOf(bytes,bytes))
            db.setTransactionSuccessful()
        } finally { db.endTransaction() }
    }
    override fun forget(connection:Connection)=synchronized(LOCK) {
        WorkspaceProtocol.identifier(connection.id)
        // Per-connection wrapping key deletion cryptographically erases old SQLite pages too.
        KeyStore.getInstance("AndroidKeyStore").apply { load(null);deleteEntry("zerus_workspace_v2:${connection.id}") }
        db.beginTransaction()
        try {
            val bytes=db.rawQuery("SELECT COALESCE(SUM(envelope_bytes),0) FROM outgoing WHERE connection=?",arrayOf(connection.id)).use { check(it.moveToFirst());it.getLong(0) }
            db.execSQL("UPDATE outgoing SET envelope_bytes=NULL WHERE connection=?",arrayOf(connection.id))
            db.execSQL("DELETE FROM workspace_blobs WHERE connection=?",arrayOf(connection.id))
            db.execSQL("UPDATE outgoing_quota SET bytes=bytes-? WHERE id=1 AND bytes>=?",arrayOf(bytes,bytes))
            db.execSQL("DELETE FROM workspace_state WHERE connection=?",arrayOf(connection.id))
            db.setTransactionSuccessful()
        } finally { db.endTransaction() }
    }
    companion object { private val LOCK=Any() }
}
