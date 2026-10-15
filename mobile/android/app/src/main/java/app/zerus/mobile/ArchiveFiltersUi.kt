package app.zerus.mobile

import android.app.DatePickerDialog
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.material3.*
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.unit.dp
import java.time.LocalDate

@OptIn(ExperimentalLayoutApi::class)
@Composable fun ArchiveFiltersUi(archive: ArchivePaging, machineNames: Map<MachineKey,String>, unsupported: List<String>, visibleCount: Int) {
    val context=LocalContext.current
    fun pick(date:LocalDate, action:(LocalDate)->Unit) {
        DatePickerDialog(context,{_,year,month,day -> action(LocalDate.of(year,month+1,day))},date.year,date.monthValue-1,date.dayOfMonth).show()
    }
    Column(Modifier.fillMaxWidth(),verticalArrangement=Arrangement.spacedBy(6.dp)) {
        Text("Archived date",style=MaterialTheme.typography.labelMedium)
        Row(Modifier.fillMaxWidth().horizontalScroll(rememberScrollState()),horizontalArrangement=Arrangement.spacedBy(6.dp)) {
            ArchivePeriod.entries.forEach { period -> FilterChip(modifier=Modifier.heightIn(min=48.dp),selected=archive.period==period,onClick={archive.dates(period)},label={Text(period.caption)}) }
        }
        if(archive.period==ArchivePeriod.Custom) FlowRow(horizontalArrangement=Arrangement.spacedBy(8.dp)) {
            OutlinedButton(modifier=Modifier.heightIn(min=48.dp),onClick={pick(archive.first){archive.dates(ArchivePeriod.Custom,from=it)}}){Text("From ${archive.first}")}
            OutlinedButton(modifier=Modifier.heightIn(min=48.dp),onClick={pick(archive.last){archive.dates(ArchivePeriod.Custom,through=it)}}){Text("Through ${archive.last}")}
        }
        if(!archive.range.valid) Text("Choose an end date on or after the start date.",color=MaterialTheme.colorScheme.error)
        else Text(if(archive.loading) "Loading archive…" else if(!archive.totalsKnown && !archive.hasMore) "$visibleCount previously loaded sessions shown" else "${archive.rows.size} shown of ${archive.total} · up to 50 per computer",style=MaterialTheme.typography.bodySmall)
        archive.windows.forEach { (machine,window) -> if(window.position!=null && window.count>0) Text("${machineNames[machine] ?: "Computer"}: ${window.position+1}–${window.position+window.count} of ${window.total}",style=MaterialTheme.typography.bodySmall) }
        archive.errors.forEach { (machine,error) -> Text("${machineNames[machine] ?: "Computer"}: $error",color=MaterialTheme.colorScheme.error,style=MaterialTheme.typography.bodySmall) }
        if(unsupported.isNotEmpty()) Text("Archive paging needs online computers with an updated relay, connector and CLI. Unavailable: ${unsupported.joinToString()}.",style=MaterialTheme.typography.bodySmall)
        TextButton(modifier=Modifier.heightIn(min=48.dp),onClick={archive.refresh()},enabled=!archive.loading && archive.range.valid){Text("Refresh archive")}
        if(archive.hasNewer) OutlinedButton(modifier=Modifier.heightIn(min=48.dp),onClick={archive.loadNewer()},enabled=!archive.loading && archive.range.valid){Text("Newer archived sessions")}
    }
}
