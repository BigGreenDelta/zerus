package app.zerus.mobile

import android.content.ActivityNotFoundException
import android.content.Intent
import android.net.Uri
import android.widget.Toast
import java.util.Collections
import java.util.IdentityHashMap
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyRow
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.selection.SelectionContainer
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.LocalTextStyle
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.*
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.draw.drawBehind
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.layout.onSizeChanged
import androidx.compose.ui.platform.LocalConfiguration
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.text.AnnotatedString
import androidx.compose.ui.text.LinkAnnotation
import androidx.compose.ui.text.LinkInteractionListener
import androidx.compose.ui.text.SpanStyle
import androidx.compose.ui.text.TextLinkStyles
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.buildAnnotatedString
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontStyle
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextDecoration
import androidx.compose.ui.text.withLink
import androidx.compose.ui.text.withStyle
import androidx.compose.ui.unit.dp

private val markdownDocuments by lazy { MarkdownDocumentCache() }
private val markdownAnnotationDispatcher = Dispatchers.Default.limitedParallelism(2)
private data class MarkdownPresentationKey(val source: String, val accent: Color, val codeBackground: Color, val fileScope: String)
private data class PreparedMarkdown(val document: MarkdownDocument,
    val annotations: Map<List<MarkdownInline>, AnnotatedString>)

/** Selectable Markdown for ordinary disclosures. Receipts and approval payloads stay literal at the caller. */
@Composable internal fun MarkdownText(text: String, modifier: Modifier = Modifier, fileScope: String = "",
    onFileReference: ((ComputerFileReference) -> Unit)? = null) {
    val currentContext by rememberUpdatedState(LocalContext.current)
    // The listener reads the current context on activation, including after a context replacement.
    val openLink: (String) -> Unit = remember { { destination ->
        safeMarkdownUrl(destination)?.let { safe ->
            try {
                currentContext.startActivity(Intent(Intent.ACTION_VIEW, Uri.parse(safe))
                    .addCategory(Intent.CATEGORY_BROWSABLE).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK))
            } catch (_: ActivityNotFoundException) {
                Toast.makeText(currentContext, "No browser could open this link.", Toast.LENGTH_SHORT).show()
            } catch (_: SecurityException) {
                Toast.makeText(currentContext, "This link could not be opened.", Toast.LENGTH_SHORT).show()
            }
        }
    } }
    val key = MarkdownPresentationKey(text, MaterialTheme.colorScheme.primary,
        MaterialTheme.colorScheme.surfaceContainerHigh, if (onFileReference != null) fileScope else "")
    // Same-content/theme recomposition retains this result. Parse and annotation conversion both
    // stay off the UI thread; until ready, users can still read and select the full original text.
    val parsed by produceState<Pair<MarkdownPresentationKey, PreparedMarkdown>?>(null, key) {
        val result = try {
            val document = markdownDocuments.document(text)
            val annotations = withContext(markdownAnnotationDispatcher) {
                prepareMarkdownAnnotations(document.blocks, key.accent, key.codeBackground, openLink, onFileReference)
            }
            PreparedMarkdown(document, annotations)
        } catch (cancelled: CancellationException) {
            throw cancelled
        } catch (_: Exception) {
            PreparedMarkdown(MarkdownDocument.literal(text), emptyMap())
        }
        value = key to result
    }
    // produceState retains its previous value on a new key; reject that late/retained result.
    val prepared = parsed?.takeIf { it.first == key }?.second
    val phoneWidth = LocalConfiguration.current.screenWidthDp.dp
    SelectionContainer(modifier.widthIn(max = phoneWidth)) {
        if (prepared == null || prepared.document.literalFallback) Text(text, softWrap = true)
        else MarkdownBlocks(prepared.document.blocks, prepared.annotations)
    }
}

private fun prepareMarkdownAnnotations(blocks: List<MarkdownBlock>, accent: Color, codeBackground: Color,
    openLink: (String) -> Unit, onFileReference: ((ComputerFileReference) -> Unit)?): Map<List<MarkdownInline>, AnnotatedString> {
    // Identity lookup avoids rehashing the entire immutable inline tree during UI rendering.
    val annotations = IdentityHashMap<List<MarkdownInline>, AnnotatedString>()
    fun prepare(content: List<MarkdownInline>) {
        annotations[content] = markdownAnnotatedString(content, accent, codeBackground, openLink, onFileReference)
    }
    fun visit(children: List<MarkdownBlock>) {
        children.forEach { block -> when (block) {
            is MarkdownBlock.Paragraph -> prepare(block.content)
            is MarkdownBlock.Heading -> prepare(block.content)
            is MarkdownBlock.Quote -> visit(block.blocks)
            is MarkdownBlock.Items -> block.items.forEach { visit(it.blocks) }
            is MarkdownBlock.Table -> block.rows.forEach { row -> row.forEach { prepare(it.content) } }
            else -> Unit
        } }
    }
    visit(blocks)
    return Collections.unmodifiableMap(annotations)
}

@Composable private fun MarkdownBlocks(blocks: List<MarkdownBlock>, annotations: Map<List<MarkdownInline>, AnnotatedString>, gap: Int = 6) {
    Column(Modifier.fillMaxWidth(), verticalArrangement = Arrangement.spacedBy(gap.dp)) {
        blocks.forEach { block -> when (block) {
            is MarkdownBlock.Paragraph -> MarkdownInlineText(block.content, annotations)
            is MarkdownBlock.Heading -> {
                val style = when (block.level) {
                    1 -> MaterialTheme.typography.headlineMedium
                    2 -> MaterialTheme.typography.headlineSmall
                    3 -> MaterialTheme.typography.titleLarge
                    4 -> MaterialTheme.typography.titleMedium
                    5 -> MaterialTheme.typography.titleSmall
                    else -> MaterialTheme.typography.labelLarge
                }.copy(fontWeight = FontWeight.Bold)
                Column(Modifier.fillMaxWidth(), verticalArrangement = Arrangement.spacedBy(5.dp)) {
                    MarkdownInlineText(block.content, annotations, style = style)
                    if (block.level <= 2) HorizontalDivider(color = MaterialTheme.colorScheme.outlineVariant)
                }
            }
            is MarkdownBlock.Literal -> if (block.code) MarkdownCode(block.text) else Text(block.text, softWrap = true)
            is MarkdownBlock.Quote -> {
                val border = MaterialTheme.colorScheme.outlineVariant
                Box(Modifier.fillMaxWidth().drawBehind {
                    drawRect(border, size = Size(3.dp.toPx(), size.height))
                }.padding(start = 13.dp)) { MarkdownBlocks(block.blocks, annotations) }
            }
            is MarkdownBlock.Items -> Column(Modifier.fillMaxWidth(),
                verticalArrangement = Arrangement.spacedBy(if (block.tight) 2.dp else 6.dp)) {
                block.items.forEachIndexed { index, item ->
                    val marker = when (item.checked) {
                        true -> "☑"
                        false -> "☐"
                        null -> block.start?.let { "${it.toLong() + index}." } ?: "•"
                    }
                    Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(4.dp)) {
                        Text(marker, Modifier.widthIn(min = if (block.start == null) 16.dp else 22.dp, max = 94.dp), textAlign = TextAlign.End)
                        Box(Modifier.weight(1f)) { MarkdownBlocks(item.blocks, annotations, if (block.tight) 2 else 6) }
                    }
                }
            }
            MarkdownBlock.Rule -> HorizontalDivider(Modifier.padding(vertical = 4.dp), thickness = 2.dp,
                color = MaterialTheme.colorScheme.outlineVariant)
            is MarkdownBlock.Table -> MarkdownTable(block, annotations)
        } }
    }
}

@Composable private fun MarkdownCode(text: String) {
    val shape = RoundedCornerShape(6.dp)
    Box(Modifier.fillMaxWidth().clip(shape).background(MaterialTheme.colorScheme.surfaceContainer)
        .horizontalScroll(rememberScrollState()).padding(10.dp)) {
        Text(text, fontFamily = FontFamily.Monospace, softWrap = false)
    }
}

@Composable private fun MarkdownTable(table: MarkdownBlock.Table, annotations: Map<List<MarkdownInline>, AnnotatedString>) {
    BoxWithConstraints(Modifier.fillMaxWidth()) {
        val cellWidth = (maxWidth * 0.6f).coerceIn(120.dp, 240.dp)
        val density = LocalDensity.current
        val heights = remember(table) { mutableStateMapOf<Int, Int>() }
        val columns = table.rows.maxOfOrNull { it.size } ?: 0
        // Lazy columns keep even a wide but budget-compliant table within Compose measurement bounds.
        LazyRow(Modifier.fillMaxWidth()) {
            items((0 until columns).toList(), key = { it }) { column ->
                Column(Modifier.width(cellWidth)) {
                    table.rows.forEachIndexed { rowIndex, row ->
                        val cell = row.getOrNull(column)
                        val background = if (rowIndex % 2 == 0) MaterialTheme.colorScheme.surface
                            else MaterialTheme.colorScheme.surfaceContainerLow
                        Box(Modifier.width(cellWidth)
                            .heightIn(min = with(density) { (heights[rowIndex] ?: 0).toDp() })
                            .background(background).border(0.5.dp, MaterialTheme.colorScheme.outlineVariant)
                            .onSizeChanged { size -> if (size.height > (heights[rowIndex] ?: 0)) heights[rowIndex] = size.height }
                            .padding(horizontal = 12.dp, vertical = 7.dp)) {
                            if (cell != null) MarkdownInlineText(cell.content, annotations,
                                style = LocalTextStyle.current.copy(
                                    fontWeight = if (cell.header) FontWeight.Bold else LocalTextStyle.current.fontWeight,
                                    textAlign = when (cell.alignment) {
                                        MarkdownAlignment.Left -> TextAlign.Start
                                        MarkdownAlignment.Center -> TextAlign.Center
                                        MarkdownAlignment.Right -> TextAlign.End
                                        null -> if (cell.header) TextAlign.Center else TextAlign.Start
                                    }))
                        }
                    }
                }
            }
        }
    }
}

@Composable private fun MarkdownInlineText(content: List<MarkdownInline>,
    annotations: Map<List<MarkdownInline>, AnnotatedString>, style: TextStyle = LocalTextStyle.current) {
    Text(checkNotNull(annotations[content]), Modifier.fillMaxWidth(), style = style, softWrap = true)
}

private fun markdownAnnotatedString(content: List<MarkdownInline>, accent: Color, codeBackground: Color,
    openLink: (String) -> Unit, onFileReference: ((ComputerFileReference) -> Unit)?): AnnotatedString = buildAnnotatedString {
    fun AnnotatedString.Builder.appendInlines(inlines: List<MarkdownInline>) {
        inlines.forEach { inline -> when (inline) {
            is MarkdownInline.Text -> append(inline.text)
            is MarkdownInline.Code -> withStyle(SpanStyle(fontFamily = FontFamily.Monospace, background = codeBackground)) {
                append(inline.text)
            }
            is MarkdownInline.Styled -> withStyle(when (inline.style) {
                MarkdownStyle.Strong -> SpanStyle(fontWeight = FontWeight.Bold)
                MarkdownStyle.Emphasis -> SpanStyle(fontStyle = FontStyle.Italic)
                MarkdownStyle.Strike -> SpanStyle(textDecoration = TextDecoration.LineThrough)
            }) { appendInlines(inline.content) }
            is MarkdownInline.Link -> if (inline.safeUrl != null) {
                withLink(LinkAnnotation.Clickable(inline.safeUrl,
                    styles = TextLinkStyles(style = SpanStyle(color = accent, textDecoration = TextDecoration.Underline)),
                    linkInteractionListener = LinkInteractionListener { openLink(inline.safeUrl) })) {
                    appendInlines(inline.content)
                }
            } else if (inline.fileReference != null && onFileReference != null) {
                withLink(LinkAnnotation.Clickable("computer-file:${inline.destination}",
                    styles = TextLinkStyles(style = SpanStyle(color = accent, textDecoration = TextDecoration.Underline)),
                    linkInteractionListener = LinkInteractionListener { onFileReference(inline.fileReference) })) {
                    appendInlines(inline.content)
                }
            } else {
                appendInlines(inline.content)
                if (markdownInlineText(inline.content) != inline.destination) append(" (${inline.destination})")
            }
            is MarkdownInline.Image -> append(if (inline.alt.isBlank()) "[Image]" else "[Image: ${inline.alt}]")
            MarkdownInline.Break -> append('\n')
        } }
    }
    appendInlines(content)
}
