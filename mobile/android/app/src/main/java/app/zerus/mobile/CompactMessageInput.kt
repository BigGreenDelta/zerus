package app.zerus.mobile

import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.BasicTextField
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.text.input.InputTransformation
import androidx.compose.foundation.text.input.TextFieldLineLimits
import androidx.compose.foundation.text.input.TextFieldState
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.CloseFullscreen
import androidx.compose.material.icons.filled.OpenInFull
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.focus.FocusRequester
import androidx.compose.ui.focus.focusRequester
import androidx.compose.ui.focus.onFocusChanged
import androidx.compose.ui.graphics.SolidColor
import androidx.compose.ui.layout.Layout
import androidx.compose.ui.text.input.ImeAction
import androidx.compose.ui.text.input.KeyboardCapitalization
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.Constraints
import androidx.compose.ui.unit.constrainHeight
import androidx.compose.ui.unit.dp

/** One leading control, one trailing action; expansion never moves the bottom action. */
@Composable internal fun CompactMessageInput(
    state: TextFieldState,
    placeholder: String,
    readOnly: Boolean,
    inputTransformation: InputTransformation,
    focusRequester: FocusRequester,
    onFocusChanged: (Boolean) -> Unit,
    modifier: Modifier = Modifier,
    leading: @Composable () -> Unit,
    action: @Composable () -> Unit,
) {
    var expanded by remember(state) { mutableStateOf(false) }
    var visualLines by remember(state) { mutableIntStateOf(1) }
    val style = conversationTextStyle().copy(color = MaterialTheme.colorScheme.onSurface)
    val controls = MaterialTheme.colorScheme.onSurfaceVariant
    Surface(modifier = modifier.fillMaxWidth(), color = MaterialTheme.colorScheme.surfaceContainer,
        shape = RoundedCornerShape(24.dp), border = BorderStroke(1.dp, MaterialTheme.colorScheme.outline)) {
        Layout(content = {
            Box(Modifier.heightIn(min = 48.dp), contentAlignment = Alignment.BottomCenter) { leading() }
            BasicTextField(state = state, modifier = Modifier.padding(horizontal = 4.dp, vertical = 8.dp)
                .heightIn(max = if (expanded) 280.dp else 148.dp).focusRequester(focusRequester)
                .onFocusChanged { onFocusChanged(it.isFocused) },
                readOnly = readOnly, inputTransformation = inputTransformation, textStyle = style,
                keyboardOptions = KeyboardOptions.Default.copy(imeAction = ImeAction.Default,
                    capitalization = KeyboardCapitalization.Sentences),
                lineLimits = TextFieldLineLimits.MultiLine(minHeightInLines = 1, maxHeightInLines = if (expanded) 14 else 6),
                cursorBrush = SolidColor(MaterialTheme.colorScheme.primary),
                onTextLayout = { result -> result()?.let { visualLines = it.lineCount } },
                decorator = { inner -> Box {
                    if (state.text.isEmpty()) Text(placeholder, maxLines = 1, overflow = TextOverflow.Ellipsis,
                        color = controls, style = style)
                    inner()
                } })
            Box(Modifier.heightIn(min = 48.dp), contentAlignment = Alignment.BottomCenter) { action() }
            if (visualLines > 3 || expanded) IconButton(onClick = { expanded = !expanded }, modifier = Modifier.size(48.dp)) {
                Icon(if (expanded) Icons.Default.CloseFullscreen else Icons.Default.OpenInFull,
                    if (expanded) "Collapse message" else "Expand message", tint = controls, modifier = Modifier.size(20.dp))
            }
        }, modifier = Modifier.fillMaxWidth()) { children, constraints ->
            val width = constraints.maxWidth
            val controlWidth = 48.dp.roundToPx()
            val controlConstraints = Constraints(minWidth = controlWidth, maxWidth = controlWidth,
                maxHeight = constraints.maxHeight)
            val attachment = children[0].measure(controlConstraints)
            val editorWidth = (width - controlWidth * 2).coerceAtLeast(0)
            val editor = children[1].measure(Constraints(minWidth = editorWidth, maxWidth = editorWidth,
                maxHeight = constraints.maxHeight))
            val send = children[2].measure(controlConstraints)
            val expand = children.getOrNull(3)?.measure(controlConstraints)
            val height = constraints.constrainHeight(maxOf(attachment.height, editor.height, send.height,
                if (expand == null) 0 else expand.height + send.height))
            layout(width, height) {
                attachment.placeRelative(0, height - attachment.height)
                editor.placeRelative(controlWidth, (height - editor.height) / 2)
                send.placeRelative(width - controlWidth, height - send.height)
                expand?.placeRelative(width - controlWidth, 0)
            }
        }
    }
}
