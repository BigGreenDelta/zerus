package app.zerus.mobile

import androidx.compose.runtime.Composable
import androidx.compose.material3.MaterialTheme
import androidx.compose.ui.text.PlatformTextStyle
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.unit.sp

/** Message text and its editor share natural character spacing and a compact baseline. */
@Composable internal fun conversationTextStyle(): TextStyle = MaterialTheme.typography.bodyLarge.copy(
    fontSize = 18.sp, lineHeight = 22.sp, letterSpacing = 0.sp,
    platformStyle = PlatformTextStyle(includeFontPadding = false),
)
