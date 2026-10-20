package app.zerus.mobile

import androidx.compose.foundation.layout.*
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.Alignment
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.PathFillType
import androidx.compose.ui.graphics.SolidColor
import androidx.compose.ui.graphics.StrokeCap
import androidx.compose.ui.graphics.StrokeJoin
import androidx.compose.ui.graphics.luminance
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.graphics.vector.PathBuilder
import androidx.compose.ui.graphics.vector.path
import androidx.compose.ui.unit.dp
import androidx.compose.ui.text.style.TextOverflow
import java.util.Locale

/** Desktop geometry from tray/src/WorkspaceIcons.h and IdentityBadge.h.
 * These are existing Zerus glyphs, including its provider motifs, not vendor logos.
 */
object DesktopIcons {
    private fun stroke(name: String, width: Float = 1.6f, viewport: Float = 24f,
                       draw: PathBuilder.() -> Unit): ImageVector =
        ImageVector.Builder(name, viewport.dp, viewport.dp, viewport, viewport).apply {
            path(fill = null, stroke = SolidColor(Color.Black), strokeLineWidth = width,
                strokeLineCap = StrokeCap.Round, strokeLineJoin = StrokeJoin.Round,
                pathFillType = PathFillType.NonZero, pathBuilder = draw)
        }.build()

    private fun PathBuilder.rectangle(x: Float, y: Float, width: Float, height: Float) {
        moveTo(x, y); lineTo(x + width, y); lineTo(x + width, y + height)
        lineTo(x, y + height); close()
    }

    private fun PathBuilder.roundedRectangle(x: Float, y: Float, width: Float, height: Float, r: Float = 2f) {
        val k = r * 0.5522847498f
        moveTo(x + r, y); lineTo(x + width - r, y)
        curveTo(x + width - r + k, y, x + width, y + r - k, x + width, y + r)
        lineTo(x + width, y + height - r)
        curveTo(x + width, y + height - r + k, x + width - r + k, y + height, x + width - r, y + height)
        lineTo(x + r, y + height)
        curveTo(x + r - k, y + height, x, y + height - r + k, x, y + height - r)
        lineTo(x, y + r)
        curveTo(x, y + r - k, x + r - k, y, x + r, y); close()
    }

    val All: ImageVector = ImageVector.Builder("DesktopSessions", 24.dp, 24.dp, 24f, 24f).apply {
        path(fill = null, stroke = SolidColor(Color.Black), strokeLineWidth = 1.6f,
            strokeLineCap = StrokeCap.Round, strokeLineJoin = StrokeJoin.Round) {
            roundedRectangle(4f, 4f, 16f, 7f)
            roundedRectangle(4f, 14f, 16f, 6f)
            moveTo(12f, 7.5f); lineTo(16f, 7.5f)
        }
        // QPainter::drawPoint with a round 1.6px pen is a filled radius .8 circle.
        path(fill = SolidColor(Color.Black)) {
            moveTo(8.8f, 7.5f)
            arcTo(.8f, .8f, 0f, true, true, 7.2f, 7.5f)
            arcTo(.8f, .8f, 0f, true, true, 8.8f, 7.5f); close()
        }
    }.build()
    val Sessions get() = All
    val Attention = stroke("DesktopAttention") {
        moveTo(6f, 10f); curveTo(6f, 3f, 18f, 3f, 18f, 10f)
        lineTo(18f, 15f); lineTo(20f, 18f); lineTo(4f, 18f); lineTo(6f, 15f); close()
        moveTo(10f, 20f); arcTo(2f, 2f, 0f, false, false, 14f, 20f)
    }
    val Working = stroke("DesktopWorking") {
        moveTo(13f, 3f); lineTo(5f, 13f); lineTo(11f, 13f); lineTo(10f, 21f)
        lineTo(19f, 10f); lineTo(13f, 10f); close()
    }
    val Paused = stroke("DesktopPaused") {
        moveTo(8f, 5f); lineTo(8f, 19f); moveTo(16f, 5f); lineTo(16f, 19f)
    }
    val Saved get() = Paused
    val Archived = stroke("DesktopArchived") {
        rectangle(3f, 4f, 18f, 4f); rectangle(5f, 8f, 14f, 12f)
        moveTo(10f, 12f); lineTo(14f, 12f)
    }
    val Archive get() = Archived
    val Calendar = stroke("DesktopCalendar") {
        roundedRectangle(4f, 5f, 16f, 16f)
        moveTo(4f, 10f); lineTo(20f, 10f)
        moveTo(8f, 3f); lineTo(8f, 7f); moveTo(16f, 3f); lineTo(16f, 7f)
        moveTo(8f, 14f); lineTo(8.01f, 14f); moveTo(12f, 14f); lineTo(12.01f, 14f); moveTo(16f, 14f); lineTo(16.01f, 14f)
    }
    val Machines = stroke("DesktopMachines") {
        roundedRectangle(3f, 4f, 18f, 12f)
        moveTo(12f, 16f); lineTo(12f, 20f); moveTo(8f, 20f); lineTo(16f, 20f)
    }
    val Projects = stroke("DesktopProjects") {
        moveTo(3f, 8f); lineTo(3f, 5f); lineTo(10f, 5f); lineTo(12f, 8f)
        lineTo(21f, 8f); lineTo(21f, 19f); lineTo(3f, 19f); close()
    }
    val Terminal = stroke("DesktopTerminal") {
        roundedRectangle(3f, 4f, 18f, 16f)
        moveTo(7f, 9f); lineTo(10f, 12f); lineTo(7f, 15f)
        moveTo(13f, 15f); lineTo(17f, 15f)
    }
    /** WorkspaceIcons "folder": the same outline as Projects. */
    val Folder get() = Projects

    private fun PathBuilder.circle(x: Float, y: Float, r: Float) {
        moveTo(x + r, y); arcTo(r, r, 0f, true, true, x - r, y)
        arcTo(r, r, 0f, true, true, x + r, y); close()
    }

    /** WorkspaceIcons "git-branch". */
    val GitBranch = stroke("DesktopGitBranch") {
        moveTo(6f, 7.5f); lineTo(6f, 16.5f)
        moveTo(18f, 7.5f); lineTo(18f, 10f); curveTo(18f, 15f, 6f, 11f, 6f, 16.5f)
        circle(6f, 5f, 2.5f); circle(6f, 19f, 2.5f); circle(18f, 5f, 2.5f)
    }
    val GitDiff = stroke("DesktopGitDiff") {
        moveTo(13f, 3f); lineTo(5f, 3f); lineTo(5f, 21f); lineTo(19f, 21f); lineTo(19f, 9f); close()
        moveTo(13f, 3f); lineTo(13f, 9f); lineTo(19f, 9f)
        moveTo(8f, 11f); lineTo(12f, 11f); moveTo(10f, 9f); lineTo(10f, 13f)
        moveTo(8f, 17f); lineTo(14f, 17f)
    }
    private fun gitTransfer(push: Boolean) = stroke(if (push) "DesktopGitPush" else "DesktopGitPull") {
        moveTo(4f, 15f); lineTo(4f, 19f); quadTo(4f, 21f, 6f, 21f)
        lineTo(18f, 21f); quadTo(20f, 21f, 20f, 19f); lineTo(20f, 15f)
        moveTo(12f, 3f); lineTo(12f, 16f)
        moveTo(7f, if (push) 8f else 11f); lineTo(12f, if (push) 3f else 16f); lineTo(17f, if (push) 8f else 11f)
    }
    val GitPush = gitTransfer(true)
    val GitPull = gitTransfer(false)
    private fun gitPublication(synced: Boolean) = ImageVector.Builder(
        if (synced) "DesktopGitSynced" else "DesktopGitUnknown", 24.dp, 24.dp, 24f, 24f).apply {
        path(fill = null, stroke = SolidColor(Color.Black), strokeLineWidth = 1.6f,
            strokeLineCap = StrokeCap.Round, strokeLineJoin = StrokeJoin.Round) {
            moveTo(6f, 19f); curveTo(0f, 19f, 0f, 10f, 6f, 10f)
            curveTo(5f, 2f, 17f, 1f, 19f, 9f); curveTo(25f, 9f, 25f, 19f, 19f, 19f); close()
            if (synced) { moveTo(8f, 13f); lineTo(11f, 16f); lineTo(16f, 11f) }
            else { moveTo(10f, 10f); curveTo(10f, 7f, 15f, 7f, 15f, 10f); curveTo(15f, 12f, 12f, 11f, 12f, 14f) }
        }
        if (!synced) path(fill = SolidColor(Color.Black)) { circle(12f, 17f, .8f) }
    }.build()
    val GitSynced = gitPublication(true)
    val GitUnknown = gitPublication(false)
    // SessionCardDelegate.h paints this subagent counter mark inline, centered here in 16 units.
    val Subagents = stroke("DesktopSubagents", 1.2f, 16f) {
        circle(5.5f, 5.75f, 2f); circle(11.5f, 6.75f, 2f)
        moveTo(9.5f, 11.75f); arcTo(4f, 3f, 0f, false, false, 1.5f, 11.75f)
        moveTo(14.5f, 12.25f); arcTo(3.5f, 2.5f, 0f, false, false, 7.5f, 12.25f)
    }

    /** WorkspaceIcons "question": a circle around the "?" of "git-unknown", with its drawPoint dot. */
    val Question: ImageVector = ImageVector.Builder("DesktopQuestion", 24.dp, 24.dp, 24f, 24f).apply {
        path(fill = null, stroke = SolidColor(Color.Black), strokeLineWidth = 1.6f,
            strokeLineCap = StrokeCap.Round, strokeLineJoin = StrokeJoin.Round) {
            circle(12f, 12f, 9f)
            moveTo(10f, 10f); curveTo(10f, 7f, 15f, 7f, 15f, 10f)
            curveTo(15f, 12f, 12f, 11f, 12f, 14f)
        }
        path(fill = SolidColor(Color.Black)) { circle(12f, 17f, .8f) }
    }.build()

    fun forName(name: String): ImageVector = when (name.lowercase(Locale.ROOT)) {
        "all", "sessions" -> All
        "attention" -> Attention
        "working", "active" -> Working
        "paused", "pause", "saved" -> Paused
        "archive", "archived" -> Archived
        "machine", "machines" -> Machines
        "folder", "project", "projects", "group" -> Projects
        "git-branch" -> GitBranch
        "subagents" -> Subagents
        "question" -> Question
        else -> Terminal
    }

    private fun providerKey(agent: String) = when (agent.trim().lowercase(Locale.ROOT)) {
        "claude", "claude code", "claude-code" -> "claude"
        "codex" -> "codex"
        "opencode", "open code", "open-code" -> "opencode"
        "kimi", "kimi code", "kimi-code" -> "kimi"
        "dsh", "deepseek", "deepseek harness", "deepseek-harness" -> "dsh"
        "sh", "shell", "terminal", "bash", "zsh" -> "sh"
        else -> agent.trim().lowercase(Locale.ROOT)
    }

    fun providerName(agent: String): String = when (providerKey(agent)) {
        "claude" -> "Claude"
        "codex" -> "Codex"
        "opencode" -> "OpenCode"
        "kimi" -> "Kimi"
        "dsh" -> "DeepSeek"
        "sh" -> "Shell"
        else -> agent.ifBlank { "Terminal" }
    }

    // Desktop badge symbols occupy a 10px box within the 18px-high badge.
    private val ClaudeBadge = stroke("DesktopClaudeBadge", 1.4f, 18f) {
        repeat(8) { index ->
            val radians = Math.toRadians(index * 45.0)
            val x = -kotlin.math.sin(radians).toFloat()
            val y = kotlin.math.cos(radians).toFloat()
            moveTo(9f + x * 2, 9f + y * 2); lineTo(9f + x * 5, 9f + y * 5)
        }
    }
    private val KimiBadge = stroke("DesktopKimiBadge", 1.4f, 18f) {
        moveTo(6f, 5f); lineTo(6f, 13f)
        moveTo(6f, 9f); lineTo(12f, 5f); moveTo(6f, 9f); lineTo(12f, 13f)
    }
    private val OpenCodeBadge = stroke("DesktopOpenCodeBadge", 1.4f, 18f) {
        moveTo(7f,6f); lineTo(4f,9f); lineTo(7f,12f)
        moveTo(11f,6f); lineTo(14f,9f); lineTo(11f,12f)
    }
    private val TerminalBadge = stroke("DesktopTerminalBadge", 1.4f, 18f) {
        moveTo(5f, 6f); lineTo(8f, 9f); lineTo(5f, 12f)
        moveTo(10f, 12f); lineTo(13f, 12f)
    }
    private val CodexLogo = stroke("DesktopCodexLogo",1.6f) {
        repeat(6) { index ->
            val radians=Math.toRadians(index*60.0);val c=kotlin.math.cos(radians).toFloat();val s=kotlin.math.sin(radians).toFloat()
            fun x(x:Float,y:Float)=12+(x-12)*c-(y-12)*s
            fun y(x:Float,y:Float)=12+(x-12)*s+(y-12)*c
            moveTo(x(12f,5f),y(12f,5f));curveTo(x(20f,2f),y(20f,2f),x(23f,12f),y(23f,12f),x(16f,15f),y(16f,15f))
            lineTo(x(10f,12f),y(10f,12f));lineTo(x(10f,7f),y(10f,7f))
        }
    }
    private val ClaudeLogo = stroke("DesktopClaudeLogo",1.6f) {
        repeat(8) { index ->
            val radians=Math.toRadians(index*45.0);val x=-kotlin.math.sin(radians).toFloat();val y=kotlin.math.cos(radians).toFloat()
            moveTo(12+x*3,12+y*3);lineTo(12+x*9,12+y*9)
        }
    }
    private val KimiLogo = stroke("DesktopKimiLogo",1.6f) {
        moveTo(6f,4f);lineTo(6f,20f);moveTo(6f,12f);lineTo(18f,4f);moveTo(6f,12f);lineTo(18f,20f)
    }
    private val DeepSeekLogo = stroke("DesktopDeepSeekLogo",1.6f) {
        moveTo(3f,10f);curveTo(5f,5f,15f,4f,18f,10f);lineTo(23f,7f);lineTo(22f,13f);lineTo(18f,13f)
        curveTo(15f,22f,3f,19f,3f,10f);moveTo(8f,10f);lineTo(8.01f,10f)
    }
    fun providerLogo(agent:String):ImageVector = when(providerKey(agent)) {
        "codex" -> CodexLogo;"claude" -> ClaudeLogo;"kimi" -> KimiLogo;"dsh" -> DeepSeekLogo;else -> providerIcon(agent)
    }

    fun providerIcon(agent: String): ImageVector = when (providerKey(agent)) {
        "codex" -> CodexLogo
        "dsh" -> DeepSeekLogo
        "claude" -> ClaudeBadge
        "kimi" -> KimiBadge
        "opencode" -> OpenCodeBadge
        else -> TerminalBadge
    }
    fun providerColor(agent: String, dark: Boolean): Color = when (providerKey(agent)) {
        "claude" -> if (dark) Color(0xFFE8B29B) else Color(0xFF956044)
        "kimi" -> if (dark) Color(0xFFBAC1FF) else Color(0xFF5657A2)
        "dsh" -> if (dark) Color(0xFF8CB9FF) else Color(0xFF315BB1)
        else -> if (dark) Color(0xFFA6D8C5) else Color(0xFF38755D)
    }
    fun providerBackground(dark: Boolean): Color = if (dark) Color(0xFF303942) else Color(0xFFE5EBEF)
}

@Composable
fun ProviderBadge(agent: String, modifier: Modifier = Modifier) {
    Icon(imageVector = DesktopIcons.providerIcon(agent), contentDescription = DesktopIcons.providerName(agent),
        modifier = modifier.size(18.dp), tint = DesktopIcons.providerColor(agent, MaterialTheme.colorScheme.surface.luminance() < .5f))
}

/** Provider pill geometry and colours from the desktop IdentityBadges renderer. */
@Composable internal fun ProviderLabel(agent: String,modifier: Modifier = Modifier) {
    val dark = MaterialTheme.colorScheme.surface.luminance() < .5f
    val foreground = DesktopIcons.providerColor(agent,dark)
    Surface(modifier,color = DesktopIcons.providerBackground(dark),shape = RoundedCornerShape(4.dp)) {
        Row(Modifier.padding(horizontal = 7.dp,vertical = 3.dp),verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(4.dp)) {
            Icon(DesktopIcons.providerIcon(agent),null,Modifier.size(14.dp),tint = foreground)
            Text(DesktopIcons.providerName(agent),maxLines = 1,overflow = TextOverflow.Ellipsis,
                style = MaterialTheme.typography.labelSmall,color = foreground)
        }
    }
}
