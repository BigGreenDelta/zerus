package app.zerus.mobile

import androidx.compose.runtime.Composable
import androidx.compose.material3.MaterialTheme
import androidx.compose.ui.text.PlatformTextStyle
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.unit.sp

/** Conversation text is denser than the editor; both keep natural character spacing. */
@Composable internal fun conversationTextStyle(): TextStyle = MaterialTheme.typography.bodyLarge.copy(
    fontSize = 16.sp, lineHeight = 20.sp, letterSpacing = 0.sp,
    platformStyle = PlatformTextStyle(includeFontPadding = false),
)

@Composable internal fun messageInputTextStyle(): TextStyle = MaterialTheme.typography.bodyLarge.copy(
    fontSize = 18.sp, lineHeight = 22.sp, letterSpacing = 0.sp,
    platformStyle = PlatformTextStyle(includeFontPadding = false),
)
