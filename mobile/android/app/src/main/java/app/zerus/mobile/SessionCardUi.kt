package app.zerus.mobile

import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.ChevronRight
import androidx.compose.material.icons.filled.ExpandLess
import androidx.compose.material.icons.filled.ExpandMore
import androidx.compose.material3.*
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.alpha
import androidx.compose.ui.draw.clip
import androidx.compose.ui.draw.drawBehind
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.luminance
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.clearAndSetSemantics
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.stateDescription
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp

/** The desktop card's checkout line: folder / branch with a Git branch mark, or an ordinary folder's name. */
@Composable internal fun SessionCheckoutLine(session: Session, gitBadge: GitStatusBadge? = null) {
    val line = SessionCardPresentation.line(session.raw)
    if (line.isEmpty() && gitBadge == null) return
    val muted = MaterialTheme.colorScheme.onSurfaceVariant
    Column(verticalArrangement = Arrangement.spacedBy(5.dp)) {
        if (line.isNotEmpty()) Row(Modifier.fillMaxWidth().clearAndSetSemantics {
            contentDescription = SessionCardPresentation.checkoutDescription(session.raw)
        }, verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(5.dp)) {
            val icon = if (SessionCardPresentation.branch(session.raw).isEmpty()) DesktopIcons.Folder else DesktopIcons.GitBranch
            Icon(icon, null, Modifier.size(14.dp), tint = muted)
            Text(line, maxLines = 1, overflow = TextOverflow.MiddleEllipsis, color = muted, style = MaterialTheme.typography.bodySmall)
        }
        gitBadge?.let { SessionGitStatusBadge(it) }
    }
}

/** Read-only desktop Git marks. Full counts and verification state stay available to TalkBack. */
@OptIn(ExperimentalLayoutApi::class)
@Composable internal fun SessionGitStatusBadge(badge: GitStatusBadge) {
    val dark = MaterialTheme.colorScheme.surface.luminance() < .5f
    FlowRow(Modifier.fillMaxWidth().clearAndSetSemantics { contentDescription = badge.description },
        horizontalArrangement = Arrangement.spacedBy(4.dp), verticalArrangement = Arrangement.spacedBy(4.dp)) {
        badge.marks.forEach { mark ->
            val ink = when (mark.tone) {
                GitStatusTone.Attention -> Color(if (dark) 0xFFF0C77B else 0xFF885400)
                GitStatusTone.Incoming -> Color(if (dark) 0xFF92BDF1 else 0xFF356697)
                GitStatusTone.Synced -> Color(if (dark) 0xFF8BDFC0 else 0xFF187454)
                GitStatusTone.Conflict -> Color(if (dark) 0xFFF1A0A2 else 0xFFA7313A)
                GitStatusTone.Muted -> Color(if (dark) 0xFF8794A3 else 0xFF647386)
            }
            val icon = when (mark.kind) {
                GitStatusKind.Changed -> DesktopIcons.GitDiff
                GitStatusKind.Ahead -> DesktopIcons.GitPush
                GitStatusKind.Behind -> DesktopIcons.GitPull
                GitStatusKind.Unknown -> DesktopIcons.GitUnknown
                GitStatusKind.Synced -> DesktopIcons.GitSynced
            }
            Surface(color = ink.copy(alpha = if (dark) .095f else .07f), shape = RoundedCornerShape(4.dp)) {
                Row(Modifier.padding(horizontal = 4.dp, vertical = 2.dp), verticalAlignment = Alignment.CenterVertically,
                    horizontalArrangement = Arrangement.spacedBy(3.dp)) {
                    Icon(icon, null, Modifier.size(16.dp), tint = ink)
                    if (mark.count.isNotEmpty()) Text(mark.count, color = ink, style = MaterialTheme.typography.labelSmall)
                }
            }
        }
    }
}

private fun subagentChildren(model: ZerusViewModel, session: Session, online: Boolean) =
    SessionCardPresentation.children(session, online, drafted = SessionCardPresentation.draftedChildren(session.target, model.drafts))

/**
 * Pending optional questions, right after the status badge (and its separate new reply badge) in
 * the attention amber, like the desktop card. Information only: it changes no status or count, and
 * tapping the card opens the conversation, where Questions (n) offers them with Skip.
 */
@Composable internal fun SessionOptionalQuestionChip(session: Session, online: Boolean) {
    val chip = SessionCardPresentation.optionalQuestionChip(session, online) ?: return
    val colors = DesktopSessionPalette.colors(if(SessionNotificationPreferences.muted(session)) DesktopBadgeKind.Neutral else DesktopBadgeKind.Attention, MaterialTheme.colorScheme.background.luminance() < .5f)
    val foreground = Color(colors.foreground)
    Row(Modifier.clip(RoundedCornerShape(5.dp)).background(Color(colors.background))
        .clearAndSetSemantics { contentDescription = chip.description }
        .padding(start = 4.dp, end = 7.dp, top = 2.dp, bottom = 2.dp),
        verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(3.dp)) {
        Icon(DesktopIcons.Question, null, Modifier.size(16.dp), tint = foreground)
        Text(chip.label, color = foreground, style = MaterialTheme.typography.labelSmall, maxLines = 1)
    }
}

/** The subagent counter, at the end of the card's status row like the desktop card. */
@Composable internal fun SessionSubagentCounter(model: ZerusViewModel, session: Session, online: Boolean) {
    val counter = SessionCardPresentation.counter(session, online, subagentChildren(model, session, online)) ?: return
    SubagentCounter(counter, counter.expandable && session.target.key in model.expandedSubagents) { model.toggleSubagents(session.target) }
}

/** Once the counter is expanded, the visible child rows beneath the card. */
@Composable internal fun SessionSubagentRows(model: ZerusViewModel, session: Session, online: Boolean) {
    val children = subagentChildren(model, session, online)
    val counter = SessionCardPresentation.counter(session, online, children) ?: return
    // Nothing is composed while collapsed, so the card keeps no empty spaced slot.
    if (!counter.expandable || session.target.key !in model.expandedSubagents) return
    Column(Modifier.fillMaxWidth()) {
        run {
            val reasons = children.associate { child ->
                child.id to if (child.group) "" else model.agentListReason(session.target, child.id, online)
            }
            children.forEachIndexed { index, child ->
                SubagentRow(child, reasons.getValue(child.id), last = index == children.lastIndex, notificationsMuted=SessionNotificationPreferences.muted(session)) {
                    model.openAgent(session, child.id, online)
                }
            }
            reasons.values.filter { it.isNotBlank() }.distinct().forEach { reason ->
                Text(reason, Modifier.padding(start = 20.dp, top = 4.dp), color = MaterialTheme.colorScheme.onSurfaceVariant,
                    style = MaterialTheme.typography.bodySmall)
            }
        }
    }
}

@Composable private fun SubagentCounter(counter: SessionChildCounter, expanded: Boolean, onToggle: () -> Unit) {
    val dark = MaterialTheme.colorScheme.surface.luminance() < .5f
    val muted = MaterialTheme.colorScheme.onSurfaceVariant
    val content: @Composable () -> Unit = {
        Row(Modifier.padding(start = 8.dp, end = if (counter.expandable) 4.dp else 0.dp, top = 4.dp, bottom = 4.dp),
            verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(5.dp)) {
            Icon(DesktopIcons.Subagents, null, Modifier.size(16.dp), tint = muted)
            Text(counter.label, color = muted, style = MaterialTheme.typography.labelMedium, maxLines = 1)
            if (counter.expandable) Icon(if (expanded) Icons.Default.ExpandLess else Icons.Default.ExpandMore, null, Modifier.size(18.dp), tint = muted)
        }
    }
    // Without visible rows the count is information only, as on the desktop card footer.
    if (!counter.expandable) Box(Modifier.clearAndSetSemantics { contentDescription = counter.description }) { content() }
    else {
        val shape = RoundedCornerShape(8.dp)
        Box(Modifier.minimumInteractiveComponentSize().clip(shape).background(Color(if (dark) 0xFF28323C else 0xFFE7EDF2))
            .clickable(onClickLabel = if (expanded) "Hide subagents" else "Show subagents", role = Role.Button, onClick = onToggle)
            .clearAndSetSemantics {
                contentDescription = counter.description
                stateDescription = if (expanded) "Expanded" else "Collapsed"
            }) { content() }
    }
}

@Composable private fun SubagentRow(child: SessionCardChild, reason: String, last: Boolean, notificationsMuted: Boolean, onOpen: () -> Unit) {
    val enabled = reason.isBlank() && !child.group
    val dark = MaterialTheme.colorScheme.surface.luminance() < .5f
    val muted = MaterialTheme.colorScheme.onSurfaceVariant
    val tree = MaterialTheme.colorScheme.outline
    Row(Modifier.fillMaxWidth().heightIn(min = 48.dp)
        .drawBehind {
            // The desktop tree line joins each child to its parent card.
            val x = 7.dp.toPx(); val width = 1.dp.toPx()
            drawLine(tree, Offset(x, 0f), Offset(x, if (last) size.height / 2 else size.height), width)
            drawLine(tree, Offset(x, size.height / 2), Offset(14.dp.toPx(), size.height / 2), width)
        }
        // A disabled row absorbs taps instead of opening its parent card.
        .then(if (enabled) Modifier else Modifier.pointerInput(Unit) { detectTapGestures { } })
        .clickable(enabled = enabled, onClickLabel = "Open subagent activity", role = Role.Button, onClick = onOpen)
        .clearAndSetSemantics {
            contentDescription = child.description + if (child.detail.isEmpty()) "" else ". " + child.detail
            if (!enabled && reason.isNotBlank()) stateDescription = reason
        }
        .padding(start = 20.dp, top = 6.dp, bottom = 6.dp)
        .alpha(if (enabled || child.group) 1f else .6f),
        verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
        Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(2.dp)) {
            Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(7.dp)) {
                val colors = DesktopSessionPalette.colors(if(notificationsMuted && child.kind in setOf(DesktopBadgeKind.Attention,DesktopBadgeKind.Unread)) DesktopBadgeKind.Neutral else child.kind, dark)
                Surface(color = Color(colors.background), shape = RoundedCornerShape(5.dp)) {
                    Text(child.status, Modifier.padding(horizontal = 7.dp, vertical = 2.dp), color = Color(colors.foreground),
                        style = MaterialTheme.typography.labelSmall, maxLines = 1)
                }
                Text(child.name, Modifier.weight(1f, fill = false), fontWeight = FontWeight.Medium, maxLines = 1,
                    overflow = TextOverflow.Ellipsis, style = MaterialTheme.typography.bodyMedium)
            }
            if (child.detail.isNotEmpty()) Text(child.detail, maxLines = 1, overflow = TextOverflow.Ellipsis, color = muted,
                style = MaterialTheme.typography.bodySmall)
        }
        ProviderBadge(child.provider, Modifier.size(16.dp))
        if (enabled) Icon(Icons.Default.ChevronRight, null, Modifier.size(18.dp), tint = muted)
    }
}
