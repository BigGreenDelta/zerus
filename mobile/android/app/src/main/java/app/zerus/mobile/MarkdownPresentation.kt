package app.zerus.mobile

import java.net.URI
import java.util.Collections
import kotlinx.coroutines.CoroutineDispatcher
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.CoroutineStart
import kotlinx.coroutines.Deferred
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.async
import kotlinx.coroutines.cancel
import kotlinx.coroutines.job
import kotlinx.coroutines.sync.Semaphore
import org.commonmark.ext.autolink.AutolinkExtension
import org.commonmark.ext.gfm.strikethrough.Strikethrough
import org.commonmark.ext.gfm.strikethrough.StrikethroughExtension
import org.commonmark.ext.gfm.tables.*
import org.commonmark.ext.task.list.items.TaskListItemMarker
import org.commonmark.ext.task.list.items.TaskListItemsExtension
import org.commonmark.node.*
import org.commonmark.parser.Parser

/** Presentation snapshots never retain mutable CommonMark nodes or load external resources. */
internal data class MarkdownDocument(
    val source: String,
    val blocks: List<MarkdownBlock>,
    val literalFallback: Boolean = false,
    val estimatedBytes: Long = source.length.toLong() * 4,
) {
    companion object {
        fun literal(source: String) = MarkdownDocument(source, emptyList(), literalFallback = true)
    }
}

internal sealed interface MarkdownBlock {
    data class Paragraph(val content: List<MarkdownInline>) : MarkdownBlock
    data class Heading(val level: Int, val content: List<MarkdownInline>) : MarkdownBlock
    data class Literal(val text: String, val code: Boolean, val language: String = "") : MarkdownBlock
    data class Quote(val blocks: List<MarkdownBlock>) : MarkdownBlock
    data class Items(val start: Int?, val tight: Boolean, val items: List<MarkdownListItem>) : MarkdownBlock
    data object Rule : MarkdownBlock
    data class Table(val rows: List<List<MarkdownTableCell>>) : MarkdownBlock
}
internal data class MarkdownListItem(val blocks: List<MarkdownBlock>, val checked: Boolean?)
internal data class MarkdownTableCell(
    val content: List<MarkdownInline>, val header: Boolean, val alignment: MarkdownAlignment?,
)
internal enum class MarkdownAlignment { Left, Center, Right }
internal sealed interface MarkdownInline {
    data class Text(val text: String) : MarkdownInline
    data class Styled(val style: MarkdownStyle, val content: List<MarkdownInline>) : MarkdownInline
    data class Code(val text: String) : MarkdownInline
    data class Link(val content: List<MarkdownInline>, val destination: String, val safeUrl: String?,
        val fileReference: ComputerFileReference? = null) : MarkdownInline
    data class Image(val alt: String) : MarkdownInline
    data object Break : MarkdownInline
}
internal enum class MarkdownStyle { Strong, Emphasis, Strike }

/** Validate both at parse time and on activation. No filesystem or app-specific URI handling. */
internal fun safeMarkdownUrl(destination: String): String? {
    if (destination.isEmpty() || destination.any { it.isISOControl() || it.isWhitespace() || it == '\\' }) return null
    return try {
        val uri = URI(destination)
        if (uri.scheme?.lowercase() !in setOf("http", "https") || !uri.isAbsolute || uri.isOpaque ||
            uri.host.isNullOrBlank() || uri.rawUserInfo != null || uri.port !in -1..65535) null
        else if (Regex("%(?:0[0-9a-f]|1[0-9a-f]|7f)", RegexOption.IGNORE_CASE).containsMatchIn(destination)) null
        else destination
    } catch (_: Exception) { null }
}

/** Text that users see and select, including inert destinations and image descriptions. */
internal fun markdownInlineText(content: List<MarkdownInline>): String = buildString {
    fun appendInline(inlines: List<MarkdownInline>) {
        inlines.forEach { inline -> when (inline) {
            is MarkdownInline.Text -> append(inline.text)
            is MarkdownInline.Code -> append(inline.text)
            is MarkdownInline.Styled -> appendInline(inline.content)
            is MarkdownInline.Link -> {
                appendInline(inline.content)
                if (inline.safeUrl == null && markdownInlineText(inline.content) != inline.destination)
                    append(" (${inline.destination})")
            }
            is MarkdownInline.Image -> append(if (inline.alt.isBlank()) "[Image]" else "[Image: ${inline.alt}]")
            MarkdownInline.Break -> append('\n')
        } }
    }
    appendInline(content)
}

internal class MarkdownParser(
    private val maxSourceBytes: Int = 256 * 1024,
    private val maxDepth: Int = 32,
    private val maxNodes: Int = 8192,
    private val maxTableCells: Int = 4096,
    private val maxCodeLineColumns: Int = 4096,
) {
    // A sentinel level beyond the presentation limit makes exceeded nesting detectable below.
    // CommonMark itself stops deeper block and inline construction, before extension visitors run.
    private val parser = Parser.builder()
        .extensions(listOf(TablesExtension.builder().maxCells(maxTableCells).build(), StrikethroughExtension.create(),
            TaskListItemsExtension.create(), AutolinkExtension.create()))
        .maxOpenBlockParsers(maxDepth + 1)
        .maxInlineNesting(maxDepth + 1)
        .build()

    fun parse(source: String): MarkdownDocument {
        if (source.length > maxSourceBytes || source.toByteArray(Charsets.UTF_8).size > maxSourceBytes)
            return MarkdownDocument.literal(source)
        return try {
            val root = parser.parse(source)
            val modelBytes = validate(root)
            MarkdownDocument(source, blocks(root), estimatedBytes = source.length.toLong() * 4 + modelBytes)
        } catch (_: Exception) {
            MarkdownDocument.literal(source)
        } catch (_: StackOverflowError) {
            MarkdownDocument.literal(source)
        }
    }

    private fun validate(root: Node): Long {
        val pending = ArrayDeque<Pair<Node, Int>>()
        children(root).forEach { pending.add(it to 1) }
        var nodes = 0
        var cells = 0
        var weight = 0L
        while (pending.isNotEmpty()) {
            val (node, depth) = pending.removeLast()
            check(++nodes <= maxNodes && depth <= maxDepth) { "Markdown model budget exceeded" }
            if (node is TableCell) check(++cells <= maxTableCells) { "Markdown table budget exceeded" }
            val code = when (node) {
                is FencedCodeBlock -> node.literal
                is IndentedCodeBlock -> node.literal
                else -> null
            }
            if (code != null) check(codeLinesFit(code)) { "Markdown code measurement budget exceeded" }
            weight += 128 + when (node) {
                is Text -> node.literal.length.toLong() * 2
                is Code -> node.literal.length.toLong() * 2
                is HtmlInline -> node.literal.length.toLong() * 2
                is HtmlBlock -> node.literal.length.toLong() * 2
                is FencedCodeBlock -> (node.literal.length.toLong() + node.info.length) * 2
                is IndentedCodeBlock -> node.literal.length.toLong() * 2
                is Link -> node.destination.length.toLong() * 2
                else -> 0
            }
            // The source cap also bounds the parser's temporary AST.
            children(node).forEach { pending.add(it to depth + 1) }
        }
        return weight
    }

    private fun codeLinesFit(text: String): Boolean {
        var columns = 0
        for (character in text) {
            columns = when (character) {
                '\n', '\r' -> 0
                '\t' -> columns + 4 - columns % 4
                else -> columns + 1
            }
            if (columns > maxCodeLineColumns) return false
        }
        return true
    }

    private fun blocks(parent: Node): List<MarkdownBlock> = frozen(children(parent).mapNotNull { node ->
        when (node) {
            is Paragraph -> MarkdownBlock.Paragraph(inlines(node))
            is Heading -> MarkdownBlock.Heading(node.level, inlines(node))
            is FencedCodeBlock -> MarkdownBlock.Literal(node.literal, true, node.info)
            is IndentedCodeBlock -> MarkdownBlock.Literal(node.literal, true)
            is HtmlBlock -> MarkdownBlock.Literal(node.literal, false)
            is BlockQuote -> MarkdownBlock.Quote(blocks(node))
            is BulletList -> items(node, null)
            is OrderedList -> items(node, node.markerStartNumber ?: 1)
            is ThematicBreak -> MarkdownBlock.Rule
            is TableBlock -> table(node)
            is TaskListItemMarker, is LinkReferenceDefinition -> null
            else -> error("Unsupported Markdown block")
        }
    }.toList())

    private fun items(node: org.commonmark.node.ListBlock, start: Int?) = MarkdownBlock.Items(start, node.isTight,
        frozen(children(node).map { child ->
            check(child is ListItem)
            val checked = children(child).filterIsInstance<TaskListItemMarker>().firstOrNull()?.isChecked
            MarkdownListItem(blocks(child), checked)
        }.toList()))

    private fun table(node: TableBlock): MarkdownBlock.Table = MarkdownBlock.Table(frozen(children(node).flatMap { section ->
        check(section is TableHead || section is TableBody)
        children(section).map { row ->
            check(row is TableRow)
            frozen(children(row).map { cell ->
                check(cell is TableCell)
                MarkdownTableCell(inlines(cell), cell.isHeader, when (cell.alignment) {
                    TableCell.Alignment.LEFT -> MarkdownAlignment.Left
                    TableCell.Alignment.CENTER -> MarkdownAlignment.Center
                    TableCell.Alignment.RIGHT -> MarkdownAlignment.Right
                    null -> null
                })
            }.toList())
        }
    }.toList()))

    private fun inlines(parent: Node): List<MarkdownInline> = frozen(children(parent).mapNotNull { node ->
        when (node) {
            is Text -> MarkdownInline.Text(node.literal)
            is Code -> MarkdownInline.Code(node.literal)
            is HtmlInline -> MarkdownInline.Text(node.literal)
            is StrongEmphasis -> MarkdownInline.Styled(MarkdownStyle.Strong, inlines(node))
            is Emphasis -> MarkdownInline.Styled(MarkdownStyle.Emphasis, inlines(node))
            is Strikethrough -> MarkdownInline.Styled(MarkdownStyle.Strike, inlines(node))
            is Link -> MarkdownInline.Link(inlines(node), node.destination, safeMarkdownUrl(node.destination),
                ComputerFileReference.parse(node.destination))
            is Image -> MarkdownInline.Image(imageAlt(node))
            is SoftLineBreak -> MarkdownInline.Text(" ")
            is HardLineBreak -> MarkdownInline.Break
            is TaskListItemMarker -> null
            else -> error("Unsupported Markdown inline")
        }
    }.toList())

    private fun imageAlt(parent: Node): String = buildString {
        children(parent).forEach { node -> when (node) {
            is Text -> append(node.literal)
            is Code -> append(node.literal)
            is HtmlInline -> append(node.literal)
            is SoftLineBreak, is HardLineBreak -> append(' ')
            else -> append(imageAlt(node))
        } }
    }
}

private fun children(parent: Node): Sequence<Node> = sequence {
    var child = parent.firstChild
    while (child != null) {
        yield(child)
        child = child.next
    }
}
private fun <T> frozen(values: List<T>): List<T> = Collections.unmodifiableList(ArrayList(values))

/** Exact-content in-memory LRU; cancelled viewers do not cancel work shared with another viewer. */
internal class MarkdownDocumentCache(
    private val maxEntries: Int = 128,
    private val maxBytes: Long = 4L * 1024 * 1024,
    dispatcher: CoroutineDispatcher = Dispatchers.Default,
    private val parse: (String) -> MarkdownDocument = MarkdownParser()::parse,
) {
    private val scope = CoroutineScope(SupervisorJob() + dispatcher)
    private val permits = Semaphore(2)
    private val lock = Any()
    private val entries = LinkedHashMap<String, MarkdownDocument>(16, 0.75f, true)
    private val flights = HashMap<String, Deferred<MarkdownDocument>>()
    private var bytes = 0L

    suspend fun document(source: String): MarkdownDocument {
        val existing = synchronized(lock) {
            entries[source]?.let { return it }
            flights[source]
        }
        existing?.let { return it.await() }

        // Admission belongs to the requesting coroutine, so cancelled waiting viewers leave no
        // registered source or lifecycle-independent queued job. Only two shared jobs can exist.
        permits.acquire()
        var transferredPermit = false
        val flight = try {
            synchronized(lock) {
                entries[source]?.let { return it }
                flights[source] ?: scope.async(start = CoroutineStart.LAZY) {
                    val document = try { parse(source) } catch (_: Exception) { MarkdownDocument.literal(source) }
                    val self = coroutineContext.job
                    synchronized(lock) {
                        // A finished flight leaves with its result: await() can return before the
                        // completion handler runs, and a later request must then find the retained
                        // document or parse again, never join this job.
                        if (flights[source] === self) flights.remove(source)
                        if (maxEntries > 0 && document.estimatedBytes <= maxBytes) {
                            entries.put(source, document)?.let { bytes -= it.estimatedBytes }
                            bytes += document.estimatedBytes
                            while (entries.size > maxEntries || bytes > maxBytes) {
                                val oldest = entries.entries.iterator()
                                val removed = oldest.next().value
                                oldest.remove()
                                bytes -= removed.estimatedBytes
                            }
                        }
                    }
                    document
                }.also { deferred ->
                    transferredPermit = true
                    flights[source] = deferred
                    deferred.invokeOnCompletion {
                        synchronized(lock) { if (flights[source] === deferred) flights.remove(source) }
                        permits.release()
                    }
                }
            }
        } finally {
            // A concurrent same-content request or cache fill may make this admission redundant.
            if (!transferredPermit) permits.release()
        }
        flight.start()
        return flight.await()
    }

    internal fun snapshot(): Pair<Int, Long> = synchronized(lock) { entries.size to bytes }
    internal fun inFlightCount(): Int = synchronized(lock) { flights.size }
    internal fun close() { scope.cancel() }
}
