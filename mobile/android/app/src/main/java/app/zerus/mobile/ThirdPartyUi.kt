package app.zerus.mobile

import androidx.compose.foundation.layout.*
import androidx.compose.foundation.ScrollState
import androidx.compose.foundation.clickable
import androidx.compose.foundation.text.selection.SelectionContainer
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.unit.dp
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext

@Composable internal fun ThirdPartyContent(scrollState: ScrollState, modifier: Modifier = Modifier) {
    ObscureConversation()
    val context = LocalContext.current
    val scannerLicenses by produceState("Loading scanner licenses…",context) {
        value = withContext(Dispatchers.IO) { listOf("camerax.txt","zxing.txt","libyuv.txt").joinToString("\n\n") { file ->
            context.assets.open("licenses/$file").bufferedReader().use { it.readText() }
        } }
    }
    val license by produceState("Loading license…",context) {
        value = withContext(Dispatchers.IO) { runCatching {
            context.assets.open("licenses/mux-pod.txt").bufferedReader().use { it.readText() }
        }.getOrDefault("License text is unavailable.") }
    }
    val markdownLicenses by produceState("Loading Markdown licenses…",context) {
        value = withContext(Dispatchers.IO) {
            listOf("commonmark-java.txt","autolink-java.txt").joinToString("\n\n") { file ->
                context.assets.open("licenses/$file").bufferedReader().use { it.readText() }
            }
        }
    }
    // The task store's notices are about 1.5 MB: one entry per crate, its texts shown on demand.
    val replicaNotices by produceState<List<CrateNotice>?>(null,context) {
        value = withContext(Dispatchers.IO) { runCatching { rustNotices(context) }.getOrDefault(emptyList()) }
    }
    val octiconsLicense by produceState("Loading icon license…",context) {
        value = withContext(Dispatchers.IO) {
            context.assets.open("licenses/octicons.txt").bufferedReader().use { it.readText() }
        }
    }
    SelectionContainer { Column(modifier.verticalScroll(scrollState),verticalArrangement = Arrangement.spacedBy(12.dp)) {
        Text("MuxPod — Copyright 2025 mox",style = MaterialTheme.typography.titleSmall)
        Text("Terminal ANSI colors, styles, and selection behavior adapted to Kotlin/Compose from MuxPod (Apache License 2.0). Zerus adds bounded control suppression and explicit relay input.")
        Text("Source: github.com/moezakura/mux-pod\nCommit c33d0fdeb66ca7bbdf4745203578088961982c13",style = MaterialTheme.typography.bodySmall)
        Text(license,style = MaterialTheme.typography.bodySmall)
        Text("CameraX 1.6.2 — The Android Open Source Project. ZXing core 3.5.4 — ZXing authors. Both use Apache License 2.0. CameraX includes libyuv under BSD-3-Clause.",style = MaterialTheme.typography.titleSmall)
        Text("Sources: developer.android.com/jetpack/androidx/releases/camera; github.com/zxing/zxing; chromium.googlesource.com/libyuv/libyuv",style = MaterialTheme.typography.bodySmall)
        Text(scannerLicenses,style = MaterialTheme.typography.bodySmall)
        Text("CommonMark Java 0.30.0 (BSD-2-Clause) and autolink-java 0.12.0 (MIT) — Robin Stocker and contributors.",style = MaterialTheme.typography.titleSmall)
        Text("Sources: github.com/commonmark/commonmark-java; github.com/robinst/autolink-java",style = MaterialTheme.typography.bodySmall)
        Text(markdownLicenses,style = MaterialTheme.typography.bodySmall)
        Text("Task store — Loro, UniFFI and the other Rust crates of zerus-replica; JNA 5.17.0 (Apache License 2.0).",style = MaterialTheme.typography.titleSmall)
        Text("Sources: github.com/loro-dev/loro; github.com/mozilla/uniffi-rs; github.com/java-native-access/jna",style = MaterialTheme.typography.bodySmall)
        val notices = replicaNotices
        when {
            notices == null -> Text("Loading task store licenses…",style = MaterialTheme.typography.bodySmall)
            notices.isEmpty() -> Text("License texts are unavailable.",style = MaterialTheme.typography.bodySmall)
            else -> notices.forEach { CrateNoticeRow(it) }
        }
        Text("GitHub mark — Primer Octicons (MIT).",style = MaterialTheme.typography.titleSmall)
        Text("Source: github.com/primer/octicons\nCommit 97825f832c98f817867f770d084c08e3edc6f78c",style = MaterialTheme.typography.bodySmall)
        Text(octiconsLicense,style = MaterialTheme.typography.bodySmall)
    } }
}

/** One crate of the task store's library, or JNA, with its licence and notice texts. */
internal data class CrateNotice(val title: String, val text: String)

/** A crate's title; a tap shows or hides its texts, so only open crates are laid out. */
@Composable private fun CrateNoticeRow(notice: CrateNotice) {
    var open by rememberSaveable(notice.title) { mutableStateOf(false) }
    Column {
        Text((if (open) "▾ " else "▸ ") + notice.title,style = MaterialTheme.typography.bodyMedium,
            modifier = Modifier.fillMaxWidth().clickable { open = !open }.padding(vertical = 4.dp))
        if (open) Text(notice.text,style = MaterialTheme.typography.bodySmall)
    }
}

/** Each crate of the task store's library from its notices' inventory, in the inventory's
 * order; `files` lists every file below a folder and `read` reads one. */
internal fun crateNotices(inventory: String, files: (String) -> List<String>, read: (String) -> String): List<CrateNotice> {
    val packages = org.json.JSONObject(inventory).getJSONArray("packages")
    return (0 until packages.length()).map { index ->
        val crate = packages.getJSONObject(index)
        val name = "${crate.getString("name")} ${crate.getString("version")}"
        val folder = "rust-licenses/${crate.getString("name")}-${crate.getString("version")}"
        CrateNotice("$name (${crate.optString("license")})", files(folder).joinToString("\n\n") { read(it) })
    }
}

/** The task store's crates and then JNA, from the app's assets. */
private fun rustNotices(context: android.content.Context): List<CrateNotice> {
    val assets = context.assets
    fun read(path: String) = assets.open(path).bufferedReader().use { it.readText() }
    return crateNotices(read("rust-licenses/inventory.json"), { assetFiles(assets, it) }, ::read) +
        CrateNotice("JNA 5.17.0 (Apache-2.0 OR LGPL-2.1-or-later)", read("licenses/jna.txt"))
}

/** The files below an asset folder, depth first: some crates keep notices in subfolders. */
private fun assetFiles(assets: android.content.res.AssetManager, folder: String): List<String> =
    assets.list(folder).orEmpty().sorted().flatMap { name ->
        val path = "$folder/$name"
        if (assets.list(path).orEmpty().isEmpty()) listOf(path) else assetFiles(assets, path)
    }
