package app.zerus.mobile

import android.content.Context
import android.security.keystore.KeyGenParameterSpec
import android.security.keystore.KeyProperties
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.NonCancellable
import kotlinx.coroutines.withContext
import org.json.JSONArray
import org.json.JSONObject
import java.io.File
import java.security.KeyStore
import java.security.MessageDigest
import javax.crypto.KeyGenerator
import javax.crypto.SecretKey

/** Presentation copies of projects, boards, tasks and decisions per gateway, under their own root and key. Never evidence for a write. */
class OvermindCache internal constructor(private val files: EncryptedConversationFiles) {
    suspend fun get(gateway: MachineKey, kind: String, id: String): JSONObject? = withContext(Dispatchers.IO) {
        files.read(storage(gateway, kind, id))?.let { value -> runCatching {
            JSONObject(value).takeIf { it.optString("kind") == kind && it.optString("id") == id }?.getJSONObject("value")
        }.getOrNull() }
    }
    suspend fun put(gateway: MachineKey, kind: String, id: String, value: JSONObject) {
        val internal = storage(gateway, kind, id); val generation = files.generation(gateway.connectionId)
        withContext(Dispatchers.IO) {
            files.write(internal, JSONObject().put("name", internal.session).put("kind", kind).put("id", id).put("value", value), System.currentTimeMillis(), generation)
        }
    }
    suspend fun purgeWorkspace(connectionId: String) {
        files.invalidate(connectionId)
        withContext(NonCancellable + Dispatchers.IO) { files.purgeWorkspace(connectionId) }
    }
    companion object {
        private const val KEY_ALIAS = "zerus_overmind_cache_v1"
        private val KEY_LOCK = Any()
        fun create(context: Context): OvermindCache {
            val root = File(context.applicationContext.noBackupFilesDir, "overmind-cache-v1")
            return OvermindCache(ConversationCacheEngines.get(root) { EncryptedConversationFiles(root, ::key, maxEntries = 128, maxBytes = 64L * 1024 * 1024) })
        }
        private fun key(): SecretKey = synchronized(KEY_LOCK) {
            val store = KeyStore.getInstance("AndroidKeyStore").apply { load(null) }
            (store.getKey(KEY_ALIAS, null) as? SecretKey) ?: KeyGenerator.getInstance("AES", "AndroidKeyStore").apply {
                init(KeyGenParameterSpec.Builder(KEY_ALIAS, KeyProperties.PURPOSE_ENCRYPT or KeyProperties.PURPOSE_DECRYPT).setKeySize(256)
                    .setBlockModes(KeyProperties.BLOCK_MODE_GCM).setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_NONE).build())
            }.generateKey()
        }
        /** The engine keys entries by conversation target: an entry's identity becomes a digest under its gateway. */
        private fun storage(gateway: MachineKey, kind: String, id: String) = Target(gateway.computerId, "overmind/$kind/" +
            MessageDigest.getInstance("SHA-256").digest(JSONArray(listOf(kind, id)).toString().toByteArray(Charsets.UTF_8)).joinToString("") { "%02x".format(it) },
            "", "", gateway.connectionId)
    }
}
