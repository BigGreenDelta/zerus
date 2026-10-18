package app.zerus.mobile

import androidx.compose.foundation.layout.*
import androidx.compose.foundation.ScrollState
import androidx.compose.foundation.clickable
import androidx.compose.foundation.text.selection.SelectionContainer
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.semantics.Role
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
    val octiconsLicense by produceState("Loading icon license…",context) {
        value = withContext(Dispatchers.IO) {
            context.assets.open("licenses/octicons.txt").bufferedReader().use { it.readText() }
        }
    }
    val libraries by produceState(LibraryNotices(emptyList(), emptyMap(), emptyList(), emptyList()),context) {
        value = withContext(Dispatchers.IO) {
            fun read(file: String) = context.assets.open("licenses/$file").bufferedReader().use { it.readText() }
            libraryNotices(read("libraries.tsv"),noticeSections(read("library-notices.txt")),
                if (BuildConfig.FIREBASE_ENABLED) noticeSections(read("google-notices.txt")) else emptyList())
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
        Text("GitHub mark — Primer Octicons (MIT).",style = MaterialTheme.typography.titleSmall)
        Text("Source: github.com/primer/octicons\nCommit 97825f832c98f817867f770d084c08e3edc6f78c",style = MaterialTheme.typography.bodySmall)
        Text(octiconsLicense,style = MaterialTheme.typography.bodySmall)
        HorizontalDivider()
        Text("Android libraries",style = MaterialTheme.typography.titleSmall)
        Text("The published app contains these libraries, listed by license with their Maven coordinates. Google Play services and the Firebase analytics libraries are provided by Google under the Android Software Development Kit License: developer.android.com/studio/terms.",style = MaterialTheme.typography.bodySmall)
        libraries.byLicense.forEach { (license, names) ->
            Text(licenseName(license),style = MaterialTheme.typography.labelLarge)
            Text(names.joinToString("\n"),style = MaterialTheme.typography.bodySmall)
        }
        ExpandableNotice("Apache License 2.0") { context.assets.open("licenses/Apache-2.0.txt").bufferedReader().use { it.readText() } }
        libraries.texts.forEach { (file, names) ->
            ExpandableNotice("License of ${names.joinToString(", ")}") { context.assets.open("licenses/$file").bufferedReader().use { it.readText() } }
        }
        libraries.notices.forEach { (names, text) -> ExpandableNotice("Notice of $names") { text } }
        if (libraries.google.isNotEmpty()) {
            Text("Open source code in Google Play services and Firebase",style = MaterialTheme.typography.titleSmall)
            libraries.google.forEach { (names, text) -> ExpandableNotice(names) { text } }
        }
    } }
}

/** One license or notice whose text appears only on request, so long texts stay off the first frame. */
@Composable private fun ExpandableNotice(title: String, load: suspend () -> String) {
    var expanded by rememberSaveable(title) { mutableStateOf(false) }
    Row(Modifier.fillMaxWidth().heightIn(min = 48.dp).clickable(role = Role.Button,onClickLabel = if (expanded) "Hide text" else "Show text") { expanded = !expanded },
        verticalAlignment = Alignment.CenterVertically,horizontalArrangement = Arrangement.spacedBy(12.dp)) {
        Text(title,Modifier.weight(1f),style = MaterialTheme.typography.bodyMedium)
        Text(if (expanded) "Hide" else "Show",color = MaterialTheme.colorScheme.primary,style = MaterialTheme.typography.labelLarge)
    }
    if (expanded) {
        val text by produceState("Loading…",title) { value = withContext(Dispatchers.IO) { runCatching { load() }.getOrDefault("Text is unavailable.") } }
        Text(text,style = MaterialTheme.typography.bodySmall)
    }
}

/**
 * The generated assets/licenses/libraries.tsv (scripts/android-notices.py): shipped
 * libraries by license, the reviewed license texts that name their copyright holders,
 * and the NOTICE and embedded Google notices as (names, text) sections.
 */
internal data class LibraryNotices(val byLicense: List<Pair<String, List<String>>>, val texts: Map<String, List<String>>,
                                   val notices: List<Pair<String, String>>, val google: List<Pair<String, String>>)

internal fun libraryNotices(tsv: String, notices: List<Pair<String, String>>, google: List<Pair<String, String>>): LibraryNotices {
    val byLicense = sortedMapOf<String, MutableList<String>>()
    val texts = sortedMapOf<String, MutableList<String>>()
    tsv.lineSequence().filter { it.isNotBlank() && !it.startsWith("#") }.forEach { line ->
        val fields = line.split('\t')
        if (fields[1] == "platform") return@forEach
        val (group, artifact, version) = fields[0].split(':', limit = 3)
        byLicense.getOrPut(fields[1]) { mutableListOf() } += "$group:$artifact $version"
        fields.getOrNull(2)?.let { texts.getOrPut(it) { mutableListOf() } += "$group:$artifact" }
    }
    return LibraryNotices(byLicense.toList(),texts,notices,google)
}

/** Sections of a generated notices file: each starts with a "### names" line. */
internal fun noticeSections(text: String): List<Pair<String, String>> =
    text.split(Regex("(?m)^### ")).filter { it.isNotBlank() }.map { it.substringBefore('\n') to it.substringAfter('\n').trim() }

internal fun licenseName(id: String): String = id.split(" AND ").joinToString(" and ") { part ->
    when (part) {
        "Apache-2.0" -> "Apache License 2.0"
        "BSD-2-Clause" -> "BSD 2-Clause License"
        "BSD-3-Clause" -> "BSD 3-Clause License"
        "MIT" -> "MIT License"
        "LicenseRef-Android-SDK" -> "Android Software Development Kit License"
        else -> part
    }
}
