package app.zerus.mobile

import android.content.Context
import android.security.keystore.KeyGenParameterSpec
import android.security.keystore.KeyProperties
import android.util.Base64
import org.json.JSONArray
import org.json.JSONObject
import java.security.KeyStore
import javax.crypto.Cipher
import javax.crypto.KeyGenerator
import javax.crypto.SecretKey
import javax.crypto.spec.GCMParameterSpec

/** All credentials and drafts are encrypted with a non-exportable Android Keystore key. */
class PrivateStore(context: Context) {
    private val notificationContext = context.applicationContext
    private val preferences = context.getSharedPreferences("zerus_private", Context.MODE_PRIVATE)
    internal val workspaceJournal by lazy { AndroidWorkspaceJournal(context.applicationContext) }
    companion object { private val keyCreationLock=Any(); internal val notificationLock=Any(); private val pushLock=Any() }
    private val key: SecretKey by lazy { synchronized(keyCreationLock) {
        val store = KeyStore.getInstance("AndroidKeyStore").apply { load(null) }
        (store.getKey("zerus_private_v1", null) as? SecretKey) ?: KeyGenerator.getInstance(
            KeyProperties.KEY_ALGORITHM_AES, "AndroidKeyStore").apply {
            init(KeyGenParameterSpec.Builder("zerus_private_v1", KeyProperties.PURPOSE_ENCRYPT or KeyProperties.PURPOSE_DECRYPT)
                .setBlockModes(KeyProperties.BLOCK_MODE_GCM).setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_NONE).build())
        }.generateKey()
    } }
    @Synchronized private fun read(name: String): JSONArray {
        val stored = preferences.getString(name, null) ?: return JSONArray()
        val packed = Base64.decode(stored, Base64.NO_WRAP)
        val cipher = Cipher.getInstance("AES/GCM/NoPadding")
        cipher.init(Cipher.DECRYPT_MODE, key, GCMParameterSpec(128, packed.copyOfRange(0, 12)))
        return JSONArray(String(cipher.doFinal(packed.copyOfRange(12, packed.size)), Charsets.UTF_8))
    }
    @Synchronized private fun write(name: String, data: JSONArray) {
        val cipher = Cipher.getInstance("AES/GCM/NoPadding")
        cipher.init(Cipher.ENCRYPT_MODE, key)
        val packed = cipher.iv + cipher.doFinal(data.toString().toByteArray(Charsets.UTF_8))
        check(preferences.edit().putString(name, Base64.encodeToString(packed, Base64.NO_WRAP)).commit()) {
            "Could not save private state. Message was not sent."
        }
    }
    fun connections():List<Connection> {
        val current=preferences.contains("connections_v2")
        val records=if(current) {
            val index=read("connections_v2").getJSONObject(0)
            WorkspaceProtocol.exactFields(index,setOf("version","connections"));require(WorkspaceProtocol.number(index,"version")==2L)
            index.getJSONArray("connections")
        } else read("computers")
        return records.objects().also { require(it.size==records.length()) }.map {
        require(!current || it.has("protocol_version")) { "Workspace transport marker is missing." }
        val version = if(it.has("protocol_version")) WorkspaceProtocol.number(it,"protocol_version").also { n -> require(n in 1L..2L) }.toInt() else 1
        require(current || version==1) { "Protected credentials cannot be loaded from the legacy index." }
        Connection(it.getString("id"), it.getString("label"), it.getString("url"), it.getString("token"),version)
        }.also { require(it.map { row -> row.id }.toSet().size==it.size) }
    }
    // Never put protected credentials in the legacy index: an older app ignores new fields.
    fun saveConnections(computers: List<Connection>) = synchronized(pushLock) { write("connections_v2", JSONArray().put(JSONObject().put("version",2).put("connections",JSONArray(computers.map {
        JSONObject().put("id", it.id).put("label", it.label).put("url", it.url).put("token", it.token).put("protocol_version",it.protocolVersion)
    })))) }
    fun messageState(): MessageState = if (preferences.contains("message_state"))
        MessageCodec.state(read("message_state").getJSONObject(0))
    else MessageState(read("drafts").objects().map(MessageCodec::draft))
    // Legacy encrypted drafts are retained; only a successful combined commit becomes authoritative.
    fun saveMessageState(state: MessageState) = write("message_state", JSONArray().put(MessageCodec.stateJson(state)))
    internal fun speechState() = SpeechProtocol.decode(read("speech_v1").optJSONObject(0) ?: JSONObject())
    internal fun saveSpeechState(state: SpeechState) = write("speech_v1", JSONArray().put(SpeechProtocol.encode(state)))
    internal fun voiceState(): VoiceState = read("voice_messages_v1").optJSONObject(0)?.let(VoiceCodec::decode)
        ?: VoiceState(selection = speechState().selection)
    internal fun saveVoiceState(state: VoiceState) = write("voice_messages_v1", JSONArray().put(VoiceCodec.encode(state)))
    internal fun speechConfigOperations() = SpeechConfigProtocol.decode(read("speech_configuration_ops_v1"))
    internal fun saveSpeechConfigOperations(pending: List<SpeechConfigOperation>) = write("speech_configuration_ops_v1", SpeechConfigProtocol.encode(pending))
    fun accounts()=AccountSnapshots.decode(read("account_catalog_v1"))
    fun saveAccounts(state:List<AccountCatalog>)=write("account_catalog_v1",AccountSnapshots.encode(state))
    fun readState()=ConversationReadPolicies.decode(read("conversation_read_v1"))
    fun saveReadState(state:ConversationReadState)=write("conversation_read_v1",ConversationReadPolicies.encode(state))
    fun notificationEnabled() = preferences.getBoolean("notifications", true)
    fun setNotificationEnabled(enabled: Boolean) = synchronized(NotificationSettings.deliveryLock) {
        check(preferences.edit().putBoolean("notifications", enabled).commit())
        try { NotificationSettings.masterChanged(notificationContext, enabled) }
        finally { if (!enabled) SessionNotifications.cancelDisabled(notificationContext, NotificationPreferences(master = false)) }
    }
    fun notificationPreferences(): NotificationPreferences = synchronized(notificationLock) {
        val value = read("notification_settings_v1").optJSONObject(0) ?: JSONObject()
        NotificationPreferences(value.optBoolean("input", true), value.optBoolean("errors", true),
            value.optBoolean("finished", false), value.optLong("generation"), notificationEnabled(), value.optLong("input_enabled"),
            value.optLong("errors_enabled"), value.optLong("finished_enabled"), value.optBoolean("optional", false),
            value.optLong("optional_enabled"))
    }
    fun saveNotificationPreferences(value: NotificationPreferences) = synchronized(notificationLock) {
        write("notification_settings_v1", JSONArray().put(JSONObject().put("input", value.input).put("errors", value.errors)
            .put("finished", value.finished).put("generation", value.generation).put("input_enabled", value.inputEnabledAt)
            .put("errors_enabled", value.errorsEnabledAt).put("finished_enabled", value.finishedEnabledAt)
            .put("optional", value.optional).put("optional_enabled", value.optionalEnabledAt)))
    }
    fun notificationState(connection: String): NotificationState = synchronized(notificationLock) {
        val value = read("notification_state_v1:$connection").optJSONObject(0) ?: return@synchronized NotificationState()
        NotificationStateCodec.decode(value)
    }
    fun saveNotificationState(connection: String, value: NotificationState) = synchronized(notificationLock) {
        val encoded = NotificationStateCodec.encode(value)
        require(encoded.toString().toByteArray(Charsets.UTF_8).size <= 2 * 1024 * 1024) { "Notification state exceeded its safety limit." }
        write("notification_state_v1:$connection", JSONArray().put(encoded))
    }
    fun pruneNotificationStates(connections: Set<String>) = synchronized(notificationLock) {
        val prefix = "notification_state_v1:"
        val removed = preferences.all.keys.filter { it.startsWith(prefix) && it.removePrefix(prefix) !in connections }
        if (removed.isNotEmpty()) check(preferences.edit().also { edit -> removed.forEach(edit::remove) }.commit())
    }
    // v2 re-registers FCM once: a v1 binding may predate a removed UnifiedPush registration.
    fun firebaseBinding(connection: String) = read("firebase_binding_v2:$connection").optJSONObject(0)?.string("token").orEmpty()
    fun saveFirebaseToken(token: String) = synchronized(pushLock) { write("firebase", JSONArray().put(JSONObject().put("token", token))) }
    fun firebaseToken() = read("firebase").optJSONObject(0)?.string("token").orEmpty()
    fun confirmFirebaseBinding(connection: Connection, token: String) = synchronized(pushLock) {
        if (connections().contains(connection) && firebaseToken() == token) {
            write("firebase_binding_v2:${connection.id}", JSONArray().put(JSONObject().put("token", token)))
            savePushStatus(connection.id, "Firebase push configured")
        }
    }
    fun savePushStatusIfCurrent(connection: Connection, status: String) = synchronized(pushLock) {
        if (connections().contains(connection)) savePushStatus(connection.id, status)
    }
    fun savePushStatus(connection: String, status: String) = write("push_status:$connection", JSONArray().put(JSONObject().put("status", status)))
    fun pushStatus(connection: String) = read("push_status:$connection").optJSONObject(0)?.string("status").orEmpty()
}
