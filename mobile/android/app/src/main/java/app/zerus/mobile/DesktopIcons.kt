package app.zerus.mobile

import androidx.compose.foundation.layout.size
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
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
    // SessionCardDelegate.h paints this subagent counter mark inline, centered here in 16 units.
    val Subagents = stroke("DesktopSubagents", 1.2f, 16f) {
        circle(5.5f, 5.75f, 2f); circle(11.5f, 6.75f, 2f)
        moveTo(9.5f, 11.75f); arcTo(4f, 3f, 0f, false, false, 1.5f, 11.75f)
        moveTo(14.5f, 12.25f); arcTo(3.5f, 2.5f, 0f, false, false, 7.5f, 12.25f)
    }

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
        else -> Terminal
    }

    private fun providerKey(agent: String) = when (agent.trim().lowercase(Locale.ROOT)) {
        "claude", "claude code", "claude-code" -> "claude"
        "codex" -> "codex"
        "kimi", "kimi code", "kimi-code" -> "kimi"
        "dsh", "deepseek", "deepseek harness", "deepseek-harness" -> "dsh"
        "sh", "shell", "terminal", "bash", "zsh" -> "sh"
        else -> agent.trim().lowercase(Locale.ROOT)
    }

    fun providerName(agent: String): String = when (providerKey(agent)) {
        "claude" -> "Claude"
        "codex" -> "Codex"
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
    private val TerminalBadge = stroke("DesktopTerminalBadge", 1.4f, 18f) {
        moveTo(5f, 6f); lineTo(8f, 9f); lineTo(5f, 12f)
        moveTo(10f, 12f); lineTo(13f, 12f)
    }

    fun providerIcon(agent: String): ImageVector = when (providerKey(agent)) {
        "claude" -> ClaudeBadge
        "kimi" -> KimiBadge
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
