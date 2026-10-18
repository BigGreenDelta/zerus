package app.zerus.mobile

import android.content.Intent
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.lazy.itemsIndexed
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.text.selection.SelectionContainer
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.unit.dp
import androidx.compose.ui.window.Dialog
import androidx.compose.ui.window.DialogProperties
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.time.Instant
import java.time.ZoneOffset
import java.time.format.DateTimeFormatter
import java.util.Locale

internal data class AnalyticsProjection(val counters: Map<String, Long>, val daily: List<AnalyticsBucket>,
    val workHistogram: AnalyticsHistogram? = null, val overflow: Boolean = false)
internal fun projectPrivateAnalytics(computers: List<AnalyticsComputer>, machine: String, provider: String, producer: String): AnalyticsProjection {
    val snapshots = computers.filter { machine.isBlank() || it.machineId == machine }.mapNotNull { it.snapshot }
    return try {
        val buckets = AnalyticsFleet.buckets(snapshots, provider.ifBlank { null }, producer)
        val daily = buckets
            .groupBy { it.date to it.provider }.map { (key, rows) ->
                AnalyticsBucket(key.first, key.second, producer, AnalyticsFleet.counters(rows), AnalyticsFleet.workHistogram(rows))
            }.sortedWith(compareByDescending<AnalyticsBucket> { it.date }.thenBy { it.provider })
        AnalyticsProjection(AnalyticsFleet.counters(buckets), daily, AnalyticsFleet.workHistogram(buckets))
    } catch (_: ArithmeticException) { AnalyticsProjection(emptyMap(), emptyList(), overflow = true) }
}

private val metricLabels = linkedMapOf(
    "turn_started" to "Turns started", "turn_completed" to "Turns completed", "turn_interrupted" to "Turns interrupted",
    "work_ms" to "Known work", "approval_wait_ms" to "Known approval wait", "input_wait_ms" to "Known input wait",
    "approval_requested" to "Approvals requested", "approval_resolved" to "Approvals resolved", "input_requested" to "Inputs requested", "input_resolved" to "Inputs resolved",
    "delivery_submitted" to "Submissions confirmed", "delivery_failed" to "Submissions failed", "delivery_uncertain" to "Submissions uncertain",
    "recovery_attempt" to "Recovery attempts", "recovery_succeeded" to "Recovery successes", "recovery_exhausted" to "Recovery budgets exhausted",
    "input_tokens" to "Input tokens", "output_tokens" to "Output tokens", "cache_tokens" to "Cache tokens", "reasoning_tokens" to "Reasoning tokens")
private fun duration(value: Long) = String.format(Locale.ROOT, "%.1f min", value / 60_000.0)
private fun metric(value: Long?, key: String) = value?.let { if (key.endsWith("_ms")) duration(it) else it.toString() } ?: "Unknown"
private fun workSamples(histogram: AnalyticsHistogram?) = histogram?.let {
    val mean = it.meanMs?.let { value -> String.format(Locale.ROOT, "%.3f s", value / 1000.0) } ?: "Unknown"
    "Reported work samples: ${it.count}; weighted mean: $mean"
} ?: "Reported work samples and mean: Unknown"
private fun timestamp(value: Double?) = value?.let { DateTimeFormatter.ofPattern("uuuu-MM-dd HH:mm 'UTC'").withZone(ZoneOffset.UTC).format(Instant.ofEpochSecond(it.toLong())) } ?: "Unknown"
private fun providerName(value: String) = when (value) { "codex" -> "Codex"; "claude" -> "Claude Code"; "kimi" -> "Kimi Code"; "dsh" -> "DeepSeek"; "opencode" -> "OpenCode"; else -> "Other" }

@OptIn(ExperimentalMaterial3Api::class)
@Composable internal fun PrivateAnalyticsScreen(model: ZerusViewModel, onDismiss: () -> Unit) {
    val controller = model.privateAnalytics
    val state = controller.value
    var machine by rememberSaveable { mutableStateOf("") }
    var provider by rememberSaveable { mutableStateOf("") }
    var producer by rememberSaveable { mutableStateOf("main") }
    var days by rememberSaveable { mutableIntStateOf(7) }
    var preview by remember { mutableStateOf<String?>(null) }
    var previewBusy by remember { mutableStateOf(false) }
    var shareFailed by remember { mutableStateOf(false) }
    var now by remember { mutableDoubleStateOf(System.currentTimeMillis() / 1000.0) }
    var projection by remember { mutableStateOf(AnalyticsProjection(emptyMap(), emptyList())) }
    var projecting by remember { mutableStateOf(false) }
    val context = LocalContext.current
    val scope = rememberCoroutineScope()
    DisposableEffect(controller) { controller.refresh(AnalyticsRange.recent(days)); onDispose { controller.stop() } }
    LaunchedEffect(model.machines, model.connections) { controller.reconcileAvailability() }
    LaunchedEffect(Unit) { while (true) { now = System.currentTimeMillis() / 1000.0; delay(30_000) } }
    LaunchedEffect(state.computers, machine, provider, producer) {
        projecting = true
        projection = withContext(Dispatchers.Default) { projectPrivateAnalytics(state.computers, machine, provider, producer) }
        projecting = false
    }
    val selected = state.computers.filter { machine.isBlank() || it.machineId == machine }
    val current = selected.count { it.state == AnalyticsReadState.Current && it.snapshot?.let { s -> !AnalyticsFleet.stale(s, now) } == true }
    Dialog(onDismissRequest = onDismiss, properties = DialogProperties(usePlatformDefaultWidth = false, decorFitsSystemWindows = false)) {
        Surface(Modifier.fillMaxSize()) {
            Scaffold(topBar = {
                TopAppBar(title = { Text("Computer analytics") }, navigationIcon = {
                    IconButton(onClick = onDismiss) { Icon(Icons.AutoMirrored.Filled.ArrowBack, "Back") }
                }, actions = { TextButton(onClick = { controller.refresh(AnalyticsRange.recent(days)) }, enabled = !state.loading) { Text("Refresh") } })
            }) { padding ->
                LazyColumn(Modifier.padding(padding).consumeWindowInsets(padding).fillMaxSize(), contentPadding = PaddingValues(16.dp), verticalArrangement = Arrangement.spacedBy(12.dp)) {
                    item {
                        Text("Private summaries from your computers", style = MaterialTheme.typography.titleMedium)
                        Text("Read-only. Collection, retention and purge controls are on each computer. This data is separate from Firebase Usage analytics and is never sent to Firebase.", style = MaterialTheme.typography.bodySmall)
                    }
                    item {
                        FlowRow(horizontalArrangement = Arrangement.spacedBy(8.dp), verticalArrangement = Arrangement.spacedBy(4.dp)) {
                            AnalyticsChoice("Range", "Last $days UTC days", listOf(7, 30, 90).map { it.toString() to "$it days" }) { value ->
                                days = value.toInt(); controller.refresh(AnalyticsRange.recent(days))
                            }
                            AnalyticsChoice("Computer", if (machine.isBlank()) "All computers" else selected.singleOrNull()?.name ?: "Selected computer unavailable",
                                listOf("" to "All computers") + state.computers.filter { it.machineId.isNotBlank() }.map { it.machineId to it.name }) { machine = it }
                            AnalyticsChoice("Provider", provider.takeIf(String::isNotBlank)?.let(::providerName) ?: "All providers",
                                listOf("" to "All providers") + PrivateAnalyticsProtocol.providers.map { it to providerName(it) }) { provider = it }
                            AnalyticsChoice("Producer", if (producer == "main") "Main turns" else "Child turns", listOf("main" to "Main turns", "child" to "Child turns")) { producer = it }
                        }
                        Text("${state.range.from} through ${state.range.through}, inclusive UTC", style = MaterialTheme.typography.bodySmall)
                        Text("${selected.size} selected computers; $current current replies. Totals include known values only, including explicitly marked last-known replies. Missing measures remain unknown.", style = MaterialTheme.typography.bodySmall)
                        if (state.omitted > 0) Text("${state.omitted} additional computers are outside this view's ${PrivateAnalyticsProtocol.maxMachines}-computer limit. This is an incomplete fleet summary.", color = MaterialTheme.colorScheme.error)
                        if (state.loading || projecting) LinearProgressIndicator(Modifier.fillMaxWidth())
                        if (selected.isEmpty()) Text("Connect a computer with analytics support, then refresh.")
                    }
                    itemsIndexed(selected, key = { index, computer -> "computer:${computer.machineId}:$index" }) { _, computer ->
                        OutlinedCard(Modifier.fillMaxWidth()) {
                            Column(Modifier.padding(12.dp), verticalArrangement = Arrangement.spacedBy(4.dp)) {
                                Text(computer.name, style = MaterialTheme.typography.titleSmall)
                                val snapshot = computer.snapshot
                                Text(computer.state.label)
                                if (snapshot != null) {
                                    val stale = computer.state != AnalyticsReadState.Current || AnalyticsFleet.stale(snapshot, now)
                                    Text("${if (stale) "Last known" else "Generated"}: ${timestamp(snapshot.generatedAt)}", style = MaterialTheme.typography.bodySmall)
                                    Text("Collection ${if (snapshot.enabled) "enabled" else "disabled"}; event retention 14 days, aggregate retention ${snapshot.retentionDays} days.", style = MaterialTheme.typography.bodySmall)
                                    Text("Coverage: ${timestamp(snapshot.coverage.start)} to ${timestamp(snapshot.coverage.end)}${if (snapshot.coverage.partial) "; partial" else ""}", style = MaterialTheme.typography.bodySmall)
                                    if (snapshot.coverage.gaps.isNotEmpty()) Text("Coverage gaps: ${snapshot.coverage.gaps.sorted().joinToString { it.replace('_', ' ') }}", style = MaterialTheme.typography.bodySmall)
                                    if (snapshot.coverage.unsupported.isNotEmpty()) Text("Unsupported measures: ${snapshot.coverage.unsupported.sorted().joinToString { it.replace('_', ' ') }}", style = MaterialTheme.typography.bodySmall)
                                    snapshot.coverage.providerUnsupported[provider]?.takeIf { it.isNotEmpty() }?.let { unsupported ->
                                        Text("${providerName(provider)} unsupported: ${unsupported.sorted().joinToString { it.replace('_', ' ') }}", style = MaterialTheme.typography.bodySmall)
                                    }
                                    snapshot.unresolvedWaits?.let { Text("Computer-wide observed unresolved waits: $it; incomplete coverage.", style = MaterialTheme.typography.bodySmall) }
                                } else Text("No agent summary. Zero activity cannot be inferred.", style = MaterialTheme.typography.bodySmall)
                                SpeechAnalyticsSummary(computer)
                            }
                        }
                    }
                    item {
                        HorizontalDivider()
                        Text("Known ${if (producer == "main") "main" else "child"} turn totals", style = MaterialTheme.typography.titleMedium)
                        if (projection.overflow) Text("Reported values exceed safe total limits; totals are unavailable.", color = MaterialTheme.colorScheme.error)
                        Text("Work and waits are measured intervals, not elapsed session age. Token counts do not establish monetary cost.", style = MaterialTheme.typography.bodySmall)
                    }
                    if (!projecting) {
                        item {
                            Text(workSamples(projection.workHistogram), style = MaterialTheme.typography.bodySmall)
                            Text("Partial observations only. Missing intervals are unknown; percentiles are unavailable.", style = MaterialTheme.typography.bodySmall)
                        }
                        items(metricLabels.entries.toList(), key = { "metric:${it.key}" }) { (key, label) ->
                            Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween) { Text(label, Modifier.weight(1f)); Text(metric(projection.counters[key], key)) }
                        }
                        item { Text("Typed failures", style = MaterialTheme.typography.titleMedium) }
                        items(PrivateAnalyticsProtocol.failureCodes.toList(), key = { "failure:$it" }) { code ->
                            Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween) { Text(code.replace('_', ' '), Modifier.weight(1f)); Text(metric(projection.counters["failure_$code"], "count")) }
                        }
                        item { Text("Daily known values by provider", style = MaterialTheme.typography.titleMedium) }
                        items(projection.daily, key = { "day:${it.date}:${it.provider}:${it.producer}" }) { row ->
                            Column {
                                Text("${row.date}  ${providerName(row.provider)}", style = MaterialTheme.typography.titleSmall)
                                Text("Completed: ${metric(row.counters["turn_completed"], "count")}  Work: ${metric(row.counters["work_ms"], "work_ms")}", style = MaterialTheme.typography.bodySmall)
                                Text(workSamples(row.workHistogram), style = MaterialTheme.typography.bodySmall)
                            }
                        }
                        if (projection.daily.isEmpty()) item { Text("No daily values reported for this selection.") }
                    }
                    item {
                        HorizontalDivider()
                        Text("Private diagnostics", style = MaterialTheme.typography.titleMedium)
                        Text("Review a small report of build number, read states and coverage codes before sharing. It excludes computer names and IDs, routes, accounts, prompts, code, paths, recordings and transcripts.", style = MaterialTheme.typography.bodySmall)
                        TextButton(enabled = !previewBusy, onClick = {
                            previewBusy = true
                            scope.launch {
                                try { preview = withContext(Dispatchers.Default) { PrivateAnalyticsDiagnostics.preview(state, BuildConfig.VERSION_CODE, now) }; shareFailed = false }
                                finally { previewBusy = false }
                            }
                        }) { Text("Review diagnostics") }
                    }
                }
            }
        }
    }
    preview?.let { reviewed ->
        AlertDialog(onDismissRequest = { preview = null }, title = { Text("Review diagnostics before sharing") },
            text = { Column(Modifier.heightIn(max = 480.dp).verticalScroll(rememberScrollState())) {
                Text("Only the fields below will be shared with the app you choose.")
                SelectionContainer { Text(reviewed, style = MaterialTheme.typography.bodySmall) }
                if (shareFailed) Text("No app could open the share sheet.", color = MaterialTheme.colorScheme.error)
            } }, confirmButton = { TextButton(onClick = {
                try { context.startActivity(Intent.createChooser(Intent(Intent.ACTION_SEND).setType("text/plain").putExtra(Intent.EXTRA_TEXT, reviewed), "Share reviewed diagnostics")); preview = null }
                catch (_: Exception) { shareFailed = true }
            }) { Text("Share reviewed report") } }, dismissButton = { TextButton(onClick = { preview = null }) { Text("Cancel") } })
    }
}

@Composable private fun AnalyticsChoice(label: String, selected: String, options: List<Pair<String, String>>, onSelect: (String) -> Unit) {
    var expanded by remember { mutableStateOf(false) }
    Box {
        OutlinedButton(onClick = { expanded = true }) { Text("$label: $selected") }
        DropdownMenu(expanded = expanded, onDismissRequest = { expanded = false }) {
            options.forEach { (key, name) -> DropdownMenuItem(text = { Text(name) }, onClick = { expanded = false; onSelect(key) }) }
        }
    }
}

@Composable private fun SpeechAnalyticsSummary(computer: AnalyticsComputer) {
    Text("Voice on this processing computer", style = MaterialTheme.typography.labelLarge)
    val speech = computer.speech
    if (speech == null) { Text("Voice statistics unavailable; no zero is inferred.", style = MaterialTheme.typography.bodySmall); return }
    Text("${if (computer.speechCurrent) "Read" else "Last known"}: ${timestamp(computer.speechAt)}. Collection ${if (speech.enabled) "enabled" else "disabled"}.", style = MaterialTheme.typography.bodySmall)
    Text("Accepted: ${metric(speech.counters["submitted"], "count")} jobs, ${metric(speech.counters["audio_ms"], "audio_ms")} audio. Completed: ${metric(speech.counters["completed"], "count")}; failed: ${metric(speech.counters["failed"], "count")}; cancelled: ${metric(speech.counters["cancelled"], "count")}; uncertain: ${metric(speech.counters["uncertain"], "count")}.", style = MaterialTheme.typography.bodySmall)
    Text("Recorded queue total: ${metric(speech.counters["queue_ms"], "queue_ms")}; processing total: ${metric(speech.counters["processing_ms"], "processing_ms")}. Percentiles and complete coverage are unavailable.", style = MaterialTheme.typography.bodySmall)
    if (speech.errorCodes.isNotEmpty()) Text("Voice error counts: ${speech.errorCodes.entries.sortedBy { it.key }.joinToString { "${it.key.replace('_', ' ')}: ${it.value}" }}", style = MaterialTheme.typography.bodySmall)
    Text("One processing job is one voice use; phone recordings and agent turns are not added to these totals. Provider and turn filters apply only to agent measures.", style = MaterialTheme.typography.bodySmall)
}
