package app.zerus.mobile

import androidx.compose.foundation.text.input.TextFieldState
import androidx.compose.ui.text.TextRange

/** Main-thread bridge: speech must observe IME edits before the asynchronous draft mirror. */
internal class SpeechComposerBridge(
    val target: Target,
    val editor: TextFieldState,
    private val generation: () -> String,
    private val applied: (String) -> Unit,
) {
    fun isCurrent(draft: Draft) = target == draft.target && generation() == draft.generation

    fun apply(draft: Draft) {
        check(target == draft.target)
        editor.edit {
            replace(0, length, draft.text)
            selection = TextRange(draft.text.length)
        }
        applied(draft.generation)
    }
}
