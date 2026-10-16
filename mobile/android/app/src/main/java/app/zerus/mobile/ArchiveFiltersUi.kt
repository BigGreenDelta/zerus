package app.zerus.mobile

import android.app.DatePickerDialog
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.ChevronLeft
import androidx.compose.material.icons.filled.ChevronRight
import androidx.compose.material.icons.filled.ExpandMore
import androidx.compose.material.icons.filled.Refresh
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import java.time.LocalDate
import java.time.format.DateTimeFormatter

private fun dateCaption(date: LocalDate) = date.format(DateTimeFormatter.ofPattern("d MMM yyyy"))
private fun periodCaption(period: ArchivePeriod) = when (period) {
    ArchivePeriod.Week -> "Last 7 days"
    ArchivePeriod.Month -> "Last 30 days"
    else -> period.caption
}
private fun rangeCaption(archive: ArchivePaging) = if (archive.period != ArchivePeriod.Custom) periodCaption(archive.period)
    else if (archive.first == archive.last) dateCaption(archive.first)
    else "${archive.first.format(DateTimeFormatter.ofPattern(if (archive.first.year == archive.last.year) "d MMM" else "d MMM yyyy"))} – ${dateCaption(archive.last)}"

@Composable fun ArchiveFiltersUi(archive: ArchivePaging, machineNames: Map<MachineKey,String>, unsupported: List<String>, visibleCount: Int) {
    var picker by rememberSaveable { mutableStateOf(false) }
    var details by rememberSaveable { mutableStateOf(false) }
    val windows = archive.windows
    val window = windows.values.singleOrNull()
    val summary = when {
        archive.loading -> "Loading archive…"
        !archive.totalsKnown && !archive.hasMore -> "$visibleCount loaded sessions"
        !archive.totalsKnown -> "${archive.rows.size} loaded"
        window?.position != null && window.count > 0 -> "${window.position + 1}–${window.position + window.count} of ${window.total}"
        else -> "${archive.rows.size} loaded of ${archive.total}"
    }
    Column(Modifier.fillMaxWidth(), verticalArrangement = Arrangement.spacedBy(4.dp)) {
        Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(8.dp), verticalAlignment = Alignment.CenterVertically) {
            OutlinedButton(onClick = { picker = true }, shape = RoundedCornerShape(12.dp),
                modifier = Modifier.weight(1f).heightIn(min = 48.dp).semantics { contentDescription = "Archived date: ${rangeCaption(archive)}. Choose period" },
                contentPadding = PaddingValues(horizontal = 12.dp, vertical = 8.dp)) {
                Icon(DesktopIcons.Calendar, null, Modifier.size(20.dp))
                Spacer(Modifier.width(8.dp))
                Text(rangeCaption(archive), Modifier.weight(1f), maxLines = 1, overflow = TextOverflow.Ellipsis)
                Icon(Icons.Default.ExpandMore, null, Modifier.size(18.dp))
            }
            OutlinedIconButton(onClick = { archive.refresh() }, enabled = !archive.loading && archive.range.valid,
                modifier = Modifier.size(48.dp), shape = RoundedCornerShape(12.dp)) {
                Icon(Icons.Default.Refresh, "Refresh archive", Modifier.size(20.dp))
            }
        }
        Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
            if (windows.size > 1) TextButton(onClick = { details = !details }, modifier = Modifier.weight(1f).heightIn(min = 48.dp),
                contentPadding = PaddingValues(horizontal = 4.dp), shape = RoundedCornerShape(8.dp)) {
                Text(summary, Modifier.weight(1f), style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
                Icon(Icons.Default.ExpandMore, if (details) "Hide archive details" else "Show archive details", Modifier.size(18.dp))
            } else Text(summary, Modifier.weight(1f).padding(end = 8.dp), style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
            if (archive.hasMore || archive.hasNewer || archive.loading) {
                IconButton(onClick = { archive.loadNewer() }, enabled = archive.hasNewer && !archive.loading && archive.range.valid, modifier = Modifier.size(48.dp)) {
                    Icon(Icons.Default.ChevronLeft, "Newer archived sessions")
                }
                IconButton(onClick = { archive.loadMore() }, enabled = archive.hasMore && !archive.loading && archive.range.valid, modifier = Modifier.size(48.dp)) {
                    Icon(Icons.Default.ChevronRight, "Older archived sessions")
                }
            }
        }
        if (details && windows.size > 1) windows.forEach { (machine, page) ->
            Text("${machineNames[machine] ?: "Computer"}: " + if (page.position != null && page.count > 0) "${page.position + 1}–${page.position + page.count} of ${page.total}" else "${page.count} shown of ${page.total}",
                style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
        }
        archive.errors.forEach { (machine, error) -> Text("${machineNames[machine] ?: "Computer"}: $error", color = MaterialTheme.colorScheme.error, style = MaterialTheme.typography.bodySmall) }
        if (unsupported.isNotEmpty()) Text("Archive paging is unavailable on ${unsupported.joinToString()}. Keep the computer online and update its relay, connector and CLI.", style = MaterialTheme.typography.bodySmall)
    }
    if (picker) ArchivePeriodDialog(archive) { picker = false }
}

@Composable private fun ArchivePeriodDialog(archive: ArchivePaging, dismiss: () -> Unit) {
    val context = LocalContext.current
    var period by rememberSaveable { mutableStateOf(archive.period) }
    val today = LocalDate.now()
    val initialFirst = when (archive.period) {
        ArchivePeriod.Today -> today
        ArchivePeriod.Week -> today.minusDays(6)
        ArchivePeriod.Month -> today.minusDays(29)
        else -> archive.first
    }
    var firstText by rememberSaveable { mutableStateOf(initialFirst.toString()) }
    var lastText by rememberSaveable { mutableStateOf((if (archive.period in listOf(ArchivePeriod.Today, ArchivePeriod.Week, ArchivePeriod.Month)) today else archive.last).toString()) }
    val first = LocalDate.parse(firstText)
    val last = LocalDate.parse(lastText)
    val valid = period == ArchivePeriod.All || first <= last && ArchiveRange.days(first, last).valid
    fun stage(preset: ArchivePeriod) {
        period = preset
        if (preset != ArchivePeriod.All) {
            val now = LocalDate.now()
            firstText = (when (preset) {
                ArchivePeriod.Today -> now
                ArchivePeriod.Week -> now.minusDays(6)
                else -> now.minusDays(29)
            }).toString()
            lastText = now.toString()
        }
    }
    fun pick(date: LocalDate, apply: (LocalDate) -> Unit) {
        val initial = if (period == ArchivePeriod.All) LocalDate.now() else date
        DatePickerDialog(context, { _, year, month, day ->
            if (period == ArchivePeriod.All) {
                firstText = LocalDate.now().minusDays(29).toString()
                lastText = LocalDate.now().toString()
            }
            apply(LocalDate.of(year, month + 1, day)); period = ArchivePeriod.Custom
        }, initial.year, initial.monthValue - 1, initial.dayOfMonth).apply {
            datePicker.minDate = LocalDate.of(1970, 1, 2).atStartOfDay(java.time.ZoneId.systemDefault()).toInstant().toEpochMilli()
        }.show()
    }
    ZerusAlertDialog(onDismissRequest = dismiss, title = { Text("Archived date") }, text = {
        Column(Modifier.fillMaxWidth().verticalScroll(rememberScrollState()), verticalArrangement = Arrangement.spacedBy(8.dp)) {
            Text("Quick ranges", style = MaterialTheme.typography.labelMedium, color = MaterialTheme.colorScheme.onSurfaceVariant)
            for (preset in ArchivePeriod.entries.filter { it != ArchivePeriod.Custom }) {
                FilterChip(selected = period == preset, onClick = { stage(preset) },
                    label = { Text(periodCaption(preset)) }, modifier = Modifier.fillMaxWidth().heightIn(min = 48.dp))
            }
            HorizontalDivider(Modifier.padding(vertical = 8.dp))
            Text("Custom range", style = MaterialTheme.typography.labelMedium)
            OutlinedButton(onClick = { pick(first) { firstText = it.toString() } },
                modifier = Modifier.fillMaxWidth().heightIn(min = 48.dp).semantics { contentDescription = if (period == ArchivePeriod.All) "From any date" else "From $first" }, shape = RoundedCornerShape(10.dp)) {
                Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(2.dp)) {
                    Text("From", style = MaterialTheme.typography.labelSmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
                    Text(if (period == ArchivePeriod.All) "Any date" else dateCaption(first))
                }
                Icon(DesktopIcons.Calendar, null, Modifier.size(20.dp))
            }
            OutlinedButton(onClick = { pick(last) { lastText = it.toString() } },
                modifier = Modifier.fillMaxWidth().heightIn(min = 48.dp).semantics { contentDescription = if (period == ArchivePeriod.All) "Through any date" else "Through $last" }, shape = RoundedCornerShape(10.dp)) {
                Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(2.dp)) {
                    Text("Through", style = MaterialTheme.typography.labelSmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
                    Text(if (period == ArchivePeriod.All) "Any date" else dateCaption(last))
                }
                Icon(DesktopIcons.Calendar, null, Modifier.size(20.dp))
            }
            Text(if (period == ArchivePeriod.All) "All archived dates (no date limits)" else if (valid) "Both dates included (local time)" else "End date must be on or after the start date.",
                style = MaterialTheme.typography.bodySmall, color = if (valid) MaterialTheme.colorScheme.onSurfaceVariant else MaterialTheme.colorScheme.error)
        }
    }, confirmButton = { Button(onClick = { archive.dates(period, first, last); dismiss() }, enabled = valid, modifier = Modifier.heightIn(min = 48.dp)) { Text("Apply range") } },
        dismissButton = { TextButton(onClick = dismiss, modifier = Modifier.heightIn(min = 48.dp)) { Text("Cancel") } })
}
