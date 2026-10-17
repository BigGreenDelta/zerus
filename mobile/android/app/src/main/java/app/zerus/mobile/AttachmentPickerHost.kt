package app.zerus.mobile

import android.net.Uri
import androidx.activity.result.ActivityResultLauncher
import androidx.activity.result.ActivityResultRegistry
import androidx.activity.result.contract.ActivityResultContracts

/** Activity-owned launch evidence is restored separately from encrypted, asynchronously loaded drafts. */
internal class AttachmentPickerHost(
    private val registry: ActivityResultRegistry,
    restoredId: String,
    private val onResult: (List<Uri>, String) -> Unit,
) {
    var launchedId: String = restoredId; private set
    private val launchers = mutableMapOf<String, ActivityResultLauncher<Array<String>>>()

    private fun launcher(id: String): ActivityResultLauncher<Array<String>> = launchers.getOrPut(id) {
        registry.register("zerus-files:$id", ActivityResultContracts.OpenMultipleDocuments()) { uris ->
            if (launchedId == id) launchedId = ""
            onResult(uris, id)
        }
    }

    fun launch(id: String) {
        require(id.isNotBlank())
        launchedId = id
        try { launcher(id).launch(arrayOf("*/*")) }
        catch (error: Exception) {
            if (launchedId == id) launchedId = ""
            throw error
        }
    }

    /** Register only after the matching durable origin is available; registration can deliver immediately. */
    fun restore(id: String): Boolean {
        if (id.isBlank() || id != launchedId) return false
        launcher(id)
        return true
    }

    fun retain(id: String) {
        launchers.keys.filter { it != id }.forEach { launchers.remove(it)?.unregister() }
        if (launchedId != id) launchedId = ""
    }

    fun close() {
        launchers.values.forEach { it.unregister() }
        launchers.clear()
    }
}
