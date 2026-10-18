package app.zerus.mobile

/** Navigation never submits a partially answered native request. */
internal data class QuestionStep(val page: Int, val count: Int, val nextPage: Int?,
    val canAdvance: Boolean, val complete: Boolean) {
    val reviewing get() = nextPage != null && page == count - 1
}

internal object QuestionSteps {
    fun current(prompts: List<Prompt>, page: Int, options: Map<String, Set<String>>,
        texts: Map<String, String>, literalText: Boolean = false): QuestionStep {
        val index = page.coerceIn(0, (prompts.size - 1).coerceAtLeast(0))
        val answered = prompts.map { QuestionPolicies.complete(listOf(it), options, texts, literalText) }
        val complete = answered.isNotEmpty() && answered.all { it }
        val next = when {
            index < prompts.lastIndex -> index + 1
            !complete -> answered.indexOfFirst { !it }.takeIf { it in 0 until index }
            else -> null
        }
        return QuestionStep(index, prompts.size, next,
            next != null && (index == prompts.lastIndex || answered.getOrElse(index) { false }), complete)
    }
}
