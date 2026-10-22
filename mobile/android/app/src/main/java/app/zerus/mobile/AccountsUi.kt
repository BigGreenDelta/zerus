package app.zerus.mobile

import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.text.selection.SelectionContainer
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.KeyboardArrowRight
import androidx.compose.material.icons.filled.HourglassEmpty
import androidx.compose.material.icons.filled.Repeat
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Modifier
import androidx.compose.ui.Alignment
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.semantics.clearAndSetSemantics
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.unit.dp
import kotlinx.coroutines.delay

@OptIn(ExperimentalMaterial3Api::class)
@Composable fun AccountsScreen(model:ZerusViewModel,onMachines:()->Unit) {
    var now by remember { mutableDoubleStateOf(System.currentTimeMillis()/1000.0) }
    var selection by remember { mutableStateOf<AccountSelection?>(null) }
    LaunchedEffect(Unit) { while(true) { delay(30_000);now=System.currentTimeMillis()/1000.0 } }
    val catalogs=model.displayedAccounts
    val groups=remember(catalogs) { AccountGrouping.group(catalogs) }
    val resolved=selection?.let { AccountGrouping.selected(groups,it) }
    LaunchedEffect(selection,catalogs) { if(selection!=null && resolved==null) selection=null }
    LazyColumn(Modifier.fillMaxSize(),contentPadding=PaddingValues(16.dp),verticalArrangement=Arrangement.spacedBy(12.dp)) {
        item { Column(verticalArrangement=Arrangement.spacedBy(4.dp)) {
            Text("Accounts",style=MaterialTheme.typography.headlineSmall,fontWeight=FontWeight.SemiBold)
            Text("Usage reports update independently on each computer. Phone sync reads the latest available report.",style=MaterialTheme.typography.bodySmall,color=MaterialTheme.colorScheme.onSurfaceVariant)
            if(model.demo) Text("PREVIEW / Sample account data",color=MaterialTheme.colorScheme.primary,style=MaterialTheme.typography.labelMedium)
            if(model.accountsError.isNotBlank()) Text(model.accountsError,style=MaterialTheme.typography.bodySmall,color=MaterialTheme.colorScheme.error)
        } }
        if(catalogs.isEmpty()) item { Column(verticalArrangement=Arrangement.spacedBy(10.dp)) {
            Text(if(model.connections.isEmpty()) "Pair a workspace to view its account profiles." else "No account snapshots available yet. Refresh to check your machines.")
            TextButton(onClick=onMachines) { Text("Open Machines") }
        } }
        catalogs.groupBy { it.key.connectionId }.forEach { (connectionId,machines) ->
            item(key="workspace:"+connectionId) { Text(workspaceName(model,machines.first()),style=MaterialTheme.typography.labelSmall,color=MaterialTheme.colorScheme.onSurfaceVariant) }
            machines.filter { !it.supported || !it.available || it.accounts.isEmpty() }.forEach { catalog ->
                item(key="availability:"+catalog.key.toString()) { Column(verticalArrangement=Arrangement.spacedBy(4.dp)) {
                    AccountMachineLabel(model,catalog)
                    Text(when { !catalog.supported && !catalog.online -> "Account profiles appear when this machine is online."; !catalog.supported -> "Update the machine connector to view accounts."; !catalog.available -> "Account catalog is not available yet."; else -> "No account profiles reported on this machine." },style=MaterialTheme.typography.bodySmall)
                    if(!catalog.online) Text("Offline",style=MaterialTheme.typography.bodySmall,color=MaterialTheme.colorScheme.onSurfaceVariant)
                } }
            }
            items(groups.filter { it.key.connectionId==connectionId },key={ "account:"+it.key.key }) { group ->
                CompactAccountCard(model,group,now) { selection=group.members.first().selection }
            }
        }
    }
    if(resolved!=null) {
        val catalog=resolved.snapshot.catalog
        ModalBottomSheet(onDismissRequest={ selection=null },sheetState=rememberModalBottomSheetState(skipPartiallyExpanded=true)) {
            LazyColumn(Modifier.fillMaxWidth().weight(1f,fill=false),contentPadding=PaddingValues(start=16.dp,end=16.dp,bottom=24.dp),verticalArrangement=Arrangement.spacedBy(12.dp)) {
                item { Row(Modifier.fillMaxWidth(),verticalAlignment=Alignment.CenterVertically) {
                    Text("Account details",Modifier.weight(1f),style=MaterialTheme.typography.titleLarge)
                    TextButton(onClick={ selection=null }) { Text("Close") }
                } }
                item { Column(verticalArrangement=Arrangement.spacedBy(6.dp)) {
                    Text(workspaceName(model,catalog),style=MaterialTheme.typography.labelSmall,color=MaterialTheme.colorScheme.onSurfaceVariant)
                } }
                item { AccountDetails(model,resolved,now) }
                item { Text("Limits update automatically about every five minutes. Refresh reads the latest report from your machine. Saved values remain available offline.",style=MaterialTheme.typography.bodySmall,color=MaterialTheme.colorScheme.onSurfaceVariant) }
            }
        }
    }
}

private fun workspaceName(model:ZerusViewModel,catalog:AccountCatalog)=model.connections.find { it.id==catalog.key.connectionId }?.displayName ?: if(model.demo) "Preview workspace" else "Workspace"

@OptIn(ExperimentalLayoutApi::class)
@Composable private fun CompactAccountCard(model:ZerusViewModel,group:GroupedAccount,now:Double,onClick:()->Unit) {
    val catalog=group.snapshot.catalog
    val account=group.snapshot.account.copy(label=group.label)
    val muted=MaterialTheme.colorScheme.onSurfaceVariant
    Surface(onClick=onClick,shape=MaterialTheme.shapes.large,color=MaterialTheme.colorScheme.surfaceContainer) {
        Column(Modifier.fillMaxWidth().padding(12.dp),verticalArrangement=Arrangement.spacedBy(6.dp)) {
            Row(verticalAlignment=Alignment.CenterVertically,horizontalArrangement=Arrangement.spacedBy(10.dp)) {
                ProviderBadge(account.provider,Modifier.size(24.dp))
                Column(Modifier.weight(1f)) {
                    Text(account.label,style=MaterialTheme.typography.titleSmall,fontWeight=FontWeight.SemiBold,maxLines=1,overflow=TextOverflow.Ellipsis)
                    Text(DesktopIcons.providerName(account.provider)+" / "+AccountPresentation.type(account),style=MaterialTheme.typography.bodySmall,color=muted,maxLines=1,overflow=TextOverflow.Ellipsis)
                }
                Icon(Icons.AutoMirrored.Filled.KeyboardArrowRight,"View account details",Modifier.size(20.dp),tint=muted)
            }
            Column(Modifier.fillMaxWidth().padding(start=34.dp),verticalArrangement=Arrangement.spacedBy(6.dp)) {
                FlowRow(horizontalArrangement=Arrangement.spacedBy(6.dp),verticalArrangement=Arrangement.spacedBy(4.dp)) {
                    group.members.groupBy { it.catalog.key }.forEach { (_,members) ->
                        Column(verticalArrangement=Arrangement.spacedBy(3.dp)) {
                            AccountMachineLabel(model,members.first().catalog)
                            members.map { member ->
                                val marker=AccountPresentation.compactStatus(member.catalog,member.account,now)
                                if(marker.isBlank()) "" else (if(members.size>1) "${member.account.id} / " else "")+marker
                            }.filter(String::isNotBlank).distinct().forEach { marker ->
                                Text(marker,style=MaterialTheme.typography.labelSmall,color=if(members.any { AccountPresentation.warning(it.account) || it.account.refreshError }) Color(0xFFF0A35B) else muted)
                            }
                            members.map { AccountPresentation.freshness(it.catalog,it.account,now) }.distinct().forEach { freshness ->
                                Text(freshness,style=MaterialTheme.typography.labelSmall,color=muted)
                            }
                        }
                    }
                }
                account.windows.take(2).forEach { window ->
                    val remaining=AccountPresentation.remaining(window,now)
                    val percentage=AccountPresentation.percent(window.usedPercent)
                    Row(Modifier.fillMaxWidth().clearAndSetSemantics {
                        contentDescription="${AccountPresentation.period(window)}, ${AccountPresentation.resetSummary(window,now)}, $percentage"
                    },horizontalArrangement=Arrangement.spacedBy(8.dp)) {
                        Row(Modifier.width(52.dp).alignByBaseline(),horizontalArrangement=Arrangement.spacedBy(4.dp),verticalAlignment=Alignment.CenterVertically) {
                            Icon(Icons.Filled.Repeat,null,Modifier.size(14.dp),tint=muted)
                            Text(AccountPresentation.shortPeriod(window),Modifier.alignByBaseline(),style=MaterialTheme.typography.bodySmall,maxLines=1,softWrap=false,overflow=TextOverflow.Ellipsis)
                        }
                        Row(Modifier.weight(1f).alignByBaseline(),horizontalArrangement=Arrangement.spacedBy(4.dp),verticalAlignment=Alignment.CenterVertically) {
                            Icon(Icons.Filled.HourglassEmpty,null,Modifier.size(14.dp),tint=muted)
                            Text(remaining,Modifier.weight(1f).alignByBaseline(),style=MaterialTheme.typography.bodySmall,color=if(AccountPresentation.ended(window,now)) Color(0xFFF0A35B) else muted,maxLines=1,softWrap=false,overflow=TextOverflow.Ellipsis)
                        }
                        Text(percentage,Modifier.alignByBaseline(),style=MaterialTheme.typography.bodySmall,color=windowTone(catalog,account,window,now),textAlign=TextAlign.End,maxLines=1,softWrap=false,overflow=TextOverflow.Ellipsis)
                    }
                }
                if(account.windows.size>2) Text("+${account.windows.size-2} more usage windows",style=MaterialTheme.typography.labelSmall,color=muted)
                if(account.windows.isEmpty()) Text(compactBalance(account) ?: "Usage unknown",style=MaterialTheme.typography.bodySmall,color=muted,maxLines=1,overflow=TextOverflow.Ellipsis)

            }
        }
    }
}
private fun compactBalance(account:ReportedAccount):String?=when {
    account.unlimited==true -> "Credits: Unlimited"
    account.creditBalance!=null -> "Credits: ${account.creditBalance}"
    account.balances.isNotEmpty() -> account.balances.first().let { listOf(it.kind.ifBlank { "Balance" },it.balance ?: "Unknown",it.currency).filter(String::isNotBlank).joinToString(" / ") }
    else -> null
}
@Composable private fun windowTone(catalog:AccountCatalog,account:ReportedAccount,window:AccountWindow,now:Double):Color=when {
    window.usedPercent==null || AccountPresentation.stale(catalog,account,now) || AccountPresentation.ended(window,now) -> MaterialTheme.colorScheme.onSurfaceVariant
    (window.usedPercent ?: 0.0)>=90 -> Color(0xFFF07878)
    (window.usedPercent ?: 0.0)>=70 -> Color(0xFFF0A35B)
    else -> Color(0xFF72CDB2)
}

@Composable private fun AccountMachineLabel(model:ZerusViewModel,catalog:AccountCatalog) {
    MachineLabel(model.machineName(catalog.key.connectionId,catalog.key.computerId,catalog.machineName),colorHex=model.machineColor(catalog.key))
}

@OptIn(ExperimentalLayoutApi::class)
@Composable private fun AccountDetails(model:ZerusViewModel,group:GroupedAccount,now:Double) {
    val catalog=group.snapshot.catalog
    val account=group.snapshot.account.copy(label=group.label)
    val muted=MaterialTheme.colorScheme.onSurfaceVariant
    Column(Modifier.fillMaxWidth(),verticalArrangement=Arrangement.spacedBy(10.dp)) {
        Row(verticalAlignment=Alignment.CenterVertically,horizontalArrangement=Arrangement.spacedBy(10.dp)) {
            ProviderBadge(account.provider,Modifier.size(24.dp))
            Column(Modifier.weight(1f)) {
                Text(account.label,style=MaterialTheme.typography.titleMedium,fontWeight=FontWeight.SemiBold)
                Text(DesktopIcons.providerName(account.provider),style=MaterialTheme.typography.bodySmall,color=muted)
            }
        }
        SelectionContainer { Column(verticalArrangement=Arrangement.spacedBy(3.dp)) {
            listOf(account.identity.name,account.identity.email,account.identity.organization).filter(String::isNotBlank).distinct().forEach { Text(it,style=MaterialTheme.typography.bodyMedium) }
            Text("Plan: "+account.identity.plan.ifBlank { "Not reported" },style=MaterialTheme.typography.bodyMedium)
            if(account.identity.authMethod.isNotBlank()) Text("Authentication: ${account.identity.authMethod}",style=MaterialTheme.typography.bodySmall,color=muted)
            if(account.identity.accountId.isNotBlank()) Text("Provider account: ${account.identity.accountId}",style=MaterialTheme.typography.bodySmall,color=muted)
        } }
        group.members.groupBy { it.catalog.key }.forEach { (_,members) ->
            Column(verticalArrangement=Arrangement.spacedBy(4.dp)) {
                AccountMachineLabel(model,members.first().catalog)
                members.forEach { member ->
                    val a=member.account
                    Text("Profile: ${a.id}"+(if(a.isDefault) " / Default" else ""),style=MaterialTheme.typography.bodySmall)
                    Text(AccountPresentation.reportedStatus(member.catalog,a,now),style=MaterialTheme.typography.bodySmall,color=if(AccountPresentation.warning(a)) Color(0xFFF0A35B) else muted)
                    val problem=AccountPresentation.compactStatus(member.catalog,a,now)
                    if(problem.isNotBlank()) Text(problem,style=MaterialTheme.typography.bodySmall,color=muted)
                    Text(AccountPresentation.freshness(member.catalog,a,now),style=MaterialTheme.typography.bodySmall,color=muted)
                    a.openCode?.let { OpenCodeProvidersDetails(it) }
                }
            }
        }
        FlowRow(horizontalArrangement=Arrangement.spacedBy(6.dp),verticalArrangement=Arrangement.spacedBy(4.dp)) {
            Text("Usage report",style=MaterialTheme.typography.labelMedium,color=muted)
            AccountMachineLabel(model,catalog)
        }
        Text(AccountPresentation.reportedStatus(catalog,account,now),style=MaterialTheme.typography.labelMedium,color=if(AccountPresentation.warning(account)) Color(0xFFF0A35B) else muted)
        val marker=AccountPresentation.compactStatus(catalog,account,now)
        if(marker.isNotBlank()) Text(marker,style=MaterialTheme.typography.bodySmall,color=muted)
        if(account.identityCached) Text("Identity retained from the last provider report",style=MaterialTheme.typography.bodySmall,color=muted)
        if(AccountPresentation.refreshing(catalog,account)) Text("Provider refresh in progress",style=MaterialTheme.typography.bodySmall,color=muted)
        if(account.refreshError) Text("The last provider refresh failed. Retained values may be out of date.",style=MaterialTheme.typography.bodySmall,color=muted)
        account.windows.forEach { window ->
            Column(verticalArrangement=Arrangement.spacedBy(4.dp)) {
                Row(Modifier.fillMaxWidth(),horizontalArrangement=Arrangement.spacedBy(8.dp)) {
                    Row(Modifier.weight(1f).alignByBaseline(),horizontalArrangement=Arrangement.spacedBy(4.dp),verticalAlignment=Alignment.CenterVertically) {
                        Icon(Icons.Filled.Repeat,null,Modifier.size(14.dp),tint=muted)
                        Text(AccountPresentation.period(window),Modifier.alignByBaseline(),style=MaterialTheme.typography.bodyMedium)
                    }
                    Text(AccountPresentation.percent(window.usedPercent),Modifier.alignByBaseline(),style=MaterialTheme.typography.labelLarge,color=windowTone(catalog,account,window,now))
                }
                window.usedPercent?.let { LinearProgressIndicator(progress={ (it/100.0).coerceIn(0.0,1.0).toFloat() },modifier=Modifier.fillMaxWidth(),color=windowTone(catalog,account,window,now),trackColor=MaterialTheme.colorScheme.surfaceVariant) }
                Row(horizontalArrangement=Arrangement.spacedBy(4.dp),verticalAlignment=Alignment.CenterVertically) {
                    Icon(Icons.Filled.HourglassEmpty,null,Modifier.size(14.dp),tint=muted)
                    Text(AccountPresentation.resetSummary(window,now)+(if(AccountPresentation.ended(window,now)) " / Refresh needed" else ""),style=MaterialTheme.typography.bodySmall,color=muted)
                }
                AccountPresentation.date(window.resetsAt)?.let { Text("Reported reset: $it",style=MaterialTheme.typography.bodySmall,color=muted) }
            }
        }
        if(account.windows.isEmpty()) Text("Usage limits not reported",style=MaterialTheme.typography.bodySmall,color=muted)
        if(account.unlimited==true) Text("Credits: Unlimited",style=MaterialTheme.typography.bodyMedium)
        else account.creditBalance?.let { Text("Credit balance: $it",style=MaterialTheme.typography.bodyMedium) }
        account.balances.forEach { balance -> Text(listOf(balance.kind.ifBlank { "Balance" },balance.balance ?: "Unknown",balance.currency).filter(String::isNotBlank).joinToString(" / "),style=MaterialTheme.typography.bodyMedium) }
        val checked=account.checkedAt?.takeIf { it<=now+60 }?.let(AccountPresentation::date)
        Text(if(checked==null) "Provider update time not reported" else "Provider updated $checked",style=MaterialTheme.typography.bodySmall,color=muted)
    }
}

@Composable private fun OpenCodeProvidersDetails(catalog:OpenCodeAccountCatalog) {
    Column(verticalArrangement=Arrangement.spacedBy(6.dp)) {
        Text("Providers",style=MaterialTheme.typography.titleSmall)
        catalog.providers.forEach { provider ->
            Text(provider.label,style=MaterialTheme.typography.bodyMedium)
            Text(listOf(OpenCodeCatalog.kindLabel(provider.kind),provider.activeLabel.ifBlank {
                provider.credentials?.takeIf { it>1 }?.let { "$it accounts" }.orEmpty()
            },provider.models?.let { "$it models" }.orEmpty()).filter(String::isNotBlank).joinToString(" · "),
                style=MaterialTheme.typography.bodySmall,color=MaterialTheme.colorScheme.onSurfaceVariant)
        }
        OpenCodeCatalog.stateMessage(catalog.state,catalog.reason).takeIf(String::isNotBlank)?.let {
            Text(it,style=MaterialTheme.typography.bodySmall)
        }
        if(catalog.state!="ready") AccountPresentation.date(catalog.catalogAt)?.let {
            Text("Last known catalog from $it",style=MaterialTheme.typography.bodySmall)
        }
        OpenCodeCatalog.truncationMessage(catalog.modelsTruncated,catalog.providersTruncated).takeIf(String::isNotBlank)?.let {
            Text(it,style=MaterialTheme.typography.bodySmall)
        }
    }
}
