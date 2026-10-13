package app.zerus.mobile

import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable

/** Outside recycled question cards; the ViewModel owns the intent across rotation. */
@Composable internal fun QuestionSendConfirmationDialog(model: ZerusViewModel) {
    val intent = model.pendingQuestionSend ?: return
    ObscureConversation()
    ZerusAlertDialog(onDismissRequest = { model.cancelQuestionSend(intent) },
        title = { Text("Submit with full context?") },
        text = { Text("The prompt cache for ${intent.target.session} is cold or expired. Submitting this answer may process the full conversation again, increasing token use or cost. Clearing or compacting context would invalidate this pending answer. Cancel to manage context separately; your answer stays saved.") },
        confirmButton = { TextButton(enabled = model.questionSendValid(intent),
            onClick = { model.confirmQuestionSend(intent) }) { Text("Submit full context") } },
        dismissButton = { TextButton(onClick = { model.cancelQuestionSend(intent) }) { Text("Cancel") } })
}
