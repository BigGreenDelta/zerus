package app.zerus.mobile

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.ExperimentalLayoutApi
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.material3.AlertDialog as MaterialAlertDialog
import androidx.compose.runtime.Composable
import androidx.compose.runtime.CompositionLocalProvider
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.remember
import androidx.compose.runtime.snapshotFlow
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalLayoutDirection
import androidx.compose.ui.platform.LocalWindowInfo
import androidx.compose.ui.focus.FocusRequester
import androidx.compose.ui.focus.focusRequester
import androidx.compose.ui.unit.LayoutDirection
import androidx.compose.ui.unit.dp
import androidx.compose.ui.window.DialogProperties
import kotlinx.coroutines.flow.first

/** App-owned dialogs keep dismissal last; long actions wrap without losing buttons. */
@OptIn(ExperimentalLayoutApi::class)
@Composable internal fun ZerusAlertDialog(
    onDismissRequest: () -> Unit,
    confirmButton: @Composable () -> Unit,
    modifier: Modifier = Modifier,
    dismissButton: (@Composable () -> Unit)? = null,
    title: (@Composable () -> Unit)? = null,
    text: (@Composable () -> Unit)? = null,
    properties: DialogProperties = DialogProperties(),
) {
    MaterialAlertDialog(onDismissRequest = onDismissRequest, modifier = modifier, title = title, text = text,
        properties = properties, confirmButton = {
            // Material's two-slot footer reverses horizontal order. Supply one
            // complete action group so Cancel stays rightmost, or last when wrapped.
            CompositionLocalProvider(LocalLayoutDirection provides LayoutDirection.Ltr) {
                FlowRow(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(8.dp, Alignment.End),
                    verticalArrangement = Arrangement.spacedBy(8.dp)) {
                    confirmButton()
                    if (dismissButton != null) {
                        val cancelFocus = remember { FocusRequester() }
                        val window = LocalWindowInfo.current
                        LaunchedEffect(Unit) {
                            snapshotFlow { window.isWindowFocused }.first { it }
                            cancelFocus.requestFocus()
                        }
                        Box(Modifier.focusRequester(cancelFocus)) { dismissButton() }
                    }
                }
            }
        })
}
