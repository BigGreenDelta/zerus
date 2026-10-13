package app.zerus.mobile

import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.unit.dp

/** The visual circle is inset inside the separate 48 dp interaction target. */
@Composable internal fun ComposerActionFace(containerColor: Color, contentColor: Color, focused: Boolean = false,
    content: @Composable () -> Unit) {
    Surface(modifier = Modifier.size(40.dp), shape = CircleShape, color = containerColor, contentColor = contentColor,
        border = if (focused) BorderStroke(2.dp, MaterialTheme.colorScheme.onSurface) else null) {
        Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) { content() }
    }
}

@Composable internal fun ComposerActionButton(onClick: () -> Unit, enabled: Boolean,
    containerColor: Color, contentColor: Color, disabledContainerColor: Color, disabledContentColor: Color,
    content: @Composable () -> Unit) {
    IconButton(onClick = onClick, enabled = enabled, modifier = Modifier.size(48.dp)) {
        ComposerActionFace(if (enabled) containerColor else disabledContainerColor,
            if (enabled) contentColor else disabledContentColor, content = content)
    }
}
