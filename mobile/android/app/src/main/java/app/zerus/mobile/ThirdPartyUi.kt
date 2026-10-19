package app.zerus.mobile

import androidx.compose.foundation.layout.*
import androidx.compose.foundation.ScrollState
import androidx.compose.foundation.text.selection.SelectionContainer
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.*
import androidx.compose.runtime.*
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
    val replicaLicenses by produceState("Loading task store licenses…",context) {
        value = withContext(Dispatchers.IO) { runCatching { rustNotices(context) }.getOrDefault("License texts are unavailable.") }
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
        Text(replicaLicenses,style = MaterialTheme.typography.bodySmall)
        Text("GitHub mark — Primer Octicons (MIT).",style = MaterialTheme.typography.titleSmall)
        Text("Source: github.com/primer/octicons\nCommit 97825f832c98f817867f770d084c08e3edc6f78c",style = MaterialTheme.typography.bodySmall)
        Text(octiconsLicense,style = MaterialTheme.typography.bodySmall)
    } }
}

/** Each crate of the task store's library with its licence and notice texts, then JNA's. */
private fun rustNotices(context: android.content.Context): String {
    val assets = context.assets
    val inventory = org.json.JSONObject(assets.open("rust-licenses/inventory.json").bufferedReader().use { it.readText() })
    val packages = inventory.getJSONArray("packages")
    val crates = (0 until packages.length()).joinToString("\n\n") { index ->
        val crate = packages.getJSONObject(index)
        val folder = "rust-licenses/${crate.getString("name")}-${crate.getString("version")}"
        val texts = assetFiles(assets, folder).joinToString("\n\n") { file ->
            assets.open(file).bufferedReader().use { it.readText() }
        }
        "${crate.getString("name")} ${crate.getString("version")} (${crate.optString("license")})\n$texts"
    }
    return crates + "\n\n" + assets.open("licenses/jna.txt").bufferedReader().use { it.readText() }
}

/** The files below an asset folder, depth first: some crates keep notices in subfolders. */
private fun assetFiles(assets: android.content.res.AssetManager, folder: String): List<String> =
    assets.list(folder).orEmpty().sorted().flatMap { name ->
        val path = "$folder/$name"
        if (assets.list(path).orEmpty().isEmpty()) listOf(path) else assetFiles(assets, path)
    }
