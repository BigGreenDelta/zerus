/*
 * Copyright 2025 mox
 * Licensed under the Apache License, Version 2.0 (see assets/licenses/mux-pod.txt).
 * Adapted from MuxPod lib/screens/terminal/widgets/ansi_text_view.dart, commit
 * c33d0fdeb66ca7bbdf4745203578088961982c13.
 * Modified for Zerus: Compose snapshot renderer, frozen selection, cursor following, confirmed OSC 8 links,
 * explicit buffered input,
 * adaptive outbound relay reads, fixed keys, no SSH credentials or input replay.
 */
package app.zerus.mobile

import android.content.ActivityNotFoundException
import android.content.Intent
import android.net.Uri
import android.os.SystemClock
import android.widget.Toast
import androidx.compose.foundation.background
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.interaction.DragInteraction
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.text.BasicTextField
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.text.input.TextFieldState
import androidx.compose.foundation.text.input.TextFieldLineLimits
import androidx.compose.foundation.text.selection.SelectionContainer
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material.icons.filled.*
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.drawWithContent
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.SolidColor
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.platform.LocalClipboardManager
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.platform.LocalWindowInfo
import androidx.compose.ui.text.*
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontStyle
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.ImeAction
import androidx.compose.ui.text.style.TextDecoration
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.compose.ui.window.Dialog
import androidx.compose.ui.window.DialogProperties
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.LifecycleEventObserver
import androidx.lifecycle.compose.LocalLifecycleOwner
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.distinctUntilChanged
import kotlinx.coroutines.flow.merge
import kotlinx.coroutines.withContext
import org.json.JSONObject
import kotlin.math.roundToInt

private val TerminalBackground = Color(0xFF101517)
private val TerminalForeground = Color(0xFFD4D4D4)

@Composable internal fun TerminalDialog(model: ZerusViewModel, session: Session, onDismiss: () -> Unit) {
    val target = session.target
    ObscureConversation()
    Dialog(onDismissRequest = onDismiss,properties = DialogProperties(usePlatformDefaultWidth = false,decorFitsSystemWindows = false)) {
        val lifecycle = LocalLifecycleOwner.current.lifecycle
        val window = LocalWindowInfo.current
        var foreground by remember(lifecycle) { mutableStateOf(lifecycle.currentState.isAtLeast(Lifecycle.State.RESUMED)) }
        DisposableEffect(lifecycle) {
            val observer = LifecycleEventObserver { _,_ -> foreground = lifecycle.currentState.isAtLeast(Lifecycle.State.RESUMED) }
            lifecycle.addObserver(observer)
            onDispose { lifecycle.removeObserver(observer) }
        }
        DisposableEffect(target) {
            model.setTerminalVisible(target,true)
            onDispose { model.setTerminalVisible(target,false) }
        }
        var frozen by remember(target) { mutableStateOf<JSONObject?>(null) }
        var selecting by remember(target) { mutableStateOf(false) }
        var fontSize by remember(target) { mutableFloatStateOf(13f) }
        var dangerousKey by remember(target) { mutableStateOf<Pair<String,String>?>(null) }
        var reviewInput by remember(target) { mutableStateOf<SessionAction?>(null) }
        var link by remember(target) { mutableStateOf<TerminalLinkChoice?>(null) }
        var now by remember(target) { mutableLongStateOf(SystemClock.elapsedRealtime()) }
        val current = model.terminalState?.takeIf { it.string("name") == target.session && it.string("run_id") == target.run && it.string("conversation_id") == target.conversation }
        val displayed = if(selecting) frozen else current
        val readActive = TerminalPolling.readAllowed(foreground,window.isWindowFocused,selecting,reviewInput != null,dangerousKey != null)
        LaunchedEffect(target,readActive) {
            if(!readActive) return@LaunchedEffect
            var policy = TerminalPolling()
            var previous = current?.string("screen")
            while(true) {
                if(!model.terminalLoading) model.terminalSnapshot(target)
                while(model.terminalLoading) delay(50)
                val next = model.terminalState?.string("screen")
                policy = policy.observed(next != previous)
                previous = next
                delay(policy.delayMillis)
            }
        }
        LaunchedEffect(target,foreground) {
            if(foreground) while(true) { now = SystemClock.elapsedRealtime();delay(500) }
        }
        // now triggers only Terminal status freshness; it does not parse ANSI again.
        val fresh = now >= 0 && model.terminalFresh(target)
        val parser = remember(target) { TerminalAnsi() }
        val source = displayed?.string("screen").orEmpty()
        val parsed by produceState<Pair<String,TerminalScreen>?>(null,source,parser) {
            value = source to withContext(Dispatchers.Default) { synchronized(parser) { parser.parse(source) } }
        }
        val screen = parsed?.takeIf { it.first == source }?.second
        val recent = model.sessionActions.filter { it.target == target && it.operation == "terminal_input" }.sortedByDescending { it.createdAt }.take(4)
        Surface(color = TerminalBackground,modifier = Modifier.fillMaxSize()) {
            Column(Modifier.fillMaxSize().systemBarsPadding().imePadding()) {
                Row(Modifier.fillMaxWidth(),verticalAlignment = Alignment.CenterVertically) {
                    IconButton(onClick = onDismiss) { Icon(Icons.AutoMirrored.Filled.ArrowBack,"Back to Activity") }
                    Column(Modifier.weight(1f)) {
                        Text("Terminal",style = MaterialTheme.typography.titleMedium)
                        Text(session.title,maxLines = 1,style = MaterialTheme.typography.labelSmall)
                    }
                    IconButton(onClick = { fontSize = (fontSize - 1).coerceAtLeast(9f) }) { Icon(Icons.Default.Remove,"Zoom out") }
                    IconButton(onClick = { fontSize = (fontSize + 1).coerceAtMost(24f) }) { Icon(Icons.Default.Add,"Zoom in") }
                    TextButton(onClick = {
                        if(!selecting) frozen = current
                        selecting = !selecting
                    },enabled = current != null) { Text(if(selecting) "Live" else "Select") }
                }
                Text(if(selecting) "Selection frozen" else if(fresh) "Connected" else if(current != null) "Last known" else "Connecting…",
                    Modifier.padding(horizontal = 16.dp),color = if(fresh && !selecting) MaterialTheme.colorScheme.primary else MaterialTheme.colorScheme.onSurfaceVariant,
                    style = MaterialTheme.typography.labelSmall)
                if(model.terminalError.isNotBlank()) Text(model.terminalError,Modifier.padding(horizontal = 16.dp),color = MaterialTheme.colorScheme.secondary,style = MaterialTheme.typography.bodySmall)
                if(screen?.truncated == true || displayed?.optBoolean("truncated") == true) Text("Captured screen is truncated.",Modifier.padding(horizontal = 16.dp),style = MaterialTheme.typography.labelSmall)
                Box(Modifier.weight(1f).fillMaxWidth().padding(8.dp)) {
                    if(screen == null) Text("Loading captured screen…",color = MaterialTheme.colorScheme.onSurfaceVariant)
                    else TerminalScreenView(screen,displayed,fontSize,selecting) { chosen ->
                        link = TerminalLinkChoice(TerminalLinkTarget.of(chosen.uri),screen.text.substring(chosen.start,chosen.end).take(200))
                    }
                }
                recent.firstOrNull()?.let { action ->
                    Text(when(action.status) {
                        "sending" -> "Sending input…"
                        "submitted","completed" -> "Input handed to the native terminal."
                        "uncertain" -> "Input result unknown. Check the original receipt; do not repeat it."
                        "failed" -> "Input was not sent."
                        else -> action.status.replaceFirstChar { it.uppercase() }
                    },Modifier.padding(horizontal = 12.dp),style = MaterialTheme.typography.labelSmall)
                    if(action.status in listOf("failed","uncertain","reviewed")) Row {
                        TextButton(onClick = { model.checkSessionAction(action) },enabled = "action:${action.requestId}" !in model.receiptFlights) { Text("Check result") }
                        if(action.status == "uncertain") TextButton(onClick = { reviewInput = action }) { Text("Review input") }
                        if(action.status in listOf("failed","reviewed")) TextButton(onClick = { model.restoreTerminalInput(action) }) { Text("Restore input") }
                    }
                }
                TerminalInputEditor(model,target,selecting,fresh) { key ->
                    if(key in listOf("C-c","C-d")) current?.string("terminal_binding_id")?.let { dangerousKey = key to it }
                    else model.terminalInput(target,key = key)
                }
            }
        }
        reviewInput?.let { action -> ZerusAlertDialog(onDismissRequest = { reviewInput = null },title = { Text("Review uncertain Terminal input") },
            text = { Column(verticalArrangement = Arrangement.spacedBy(12.dp)) {
                Text("Input may already have reached ${session.title}. Check its original receipt and the native screen before preparing it again. Marking reviewed does not resend it.")
                val arguments = remember(action.arguments) { runCatching { JSONObject(action.arguments) }.getOrNull() }
                SelectionContainer { Text(arguments?.string("text","key").orEmpty(),modifier = Modifier.heightIn(max = 180.dp).verticalScroll(rememberScrollState())) }
            } },confirmButton = { TextButton(onClick = { model.reviewSessionAction(action);reviewInput = null }) { Text("Mark reviewed") } },
            dismissButton = { TextButton(onClick = { reviewInput = null }) { Text("Cancel") } }) }
        val computer = model.machines.find { it.id == target.computerId && it.connectionId == target.connectionId }?.name ?: target.computerId
        link?.let { choice -> TerminalLinkDialog(choice,computer,onDismiss = { link = null }) { reference ->
            link = null
            model.computerFiles.show(target,reference,computer)
        } }
        dangerousKey?.let { (key,binding) -> ZerusAlertDialog(onDismissRequest = { dangerousKey = null },
            title = { Text(if(key == "C-c") "Send Ctrl+C?" else "Send Ctrl+D?") },
            text = { Text("Send $key to ${session.title} on ${model.machines.find { it.id == target.computerId && it.connectionId == target.connectionId }?.name ?: target.computerId}? This may interrupt or close the native program.") },
            confirmButton = { TextButton(onClick = {
                if(model.terminalState?.string("terminal_binding_id") == binding && model.terminalReason(target).isBlank()) model.terminalInput(target,key = key)
                dangerousKey = null
            },enabled = model.terminalState?.string("terminal_binding_id") == binding && model.terminalReason(target).isBlank()) { Text("Send $key") } },
            dismissButton = { TextButton(onClick = { dangerousKey = null }) { Text("Cancel") } }) }
    }
}

/** Shows a link's whole target before anything opens; its visible text may say something else. */
@Composable internal fun TerminalLinkDialog(choice: TerminalLinkChoice,computer: String,onDismiss: () -> Unit,onFile: (ComputerFileReference) -> Unit) {
    val context = LocalContext.current
    val clipboard = LocalClipboardManager.current
    val target = choice.target
    ZerusAlertDialog(onDismissRequest = onDismiss,title = { Text(when(target) {
        is TerminalLinkTarget.Web -> "Open link?"
        is TerminalLinkTarget.File -> "Read file from $computer?"
        is TerminalLinkTarget.Other -> "Terminal link"
    }) },text = { Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
        Text("Shown as: ${choice.label}",style = MaterialTheme.typography.bodySmall,color = MaterialTheme.colorScheme.onSurfaceVariant)
        SelectionContainer { Text(target.uri,fontFamily = FontFamily.Monospace,modifier = Modifier.heightIn(max = 160.dp).verticalScroll(rememberScrollState())) }
        Text(when(target) {
            is TerminalLinkTarget.Web -> "Opens in your browser."
            is TerminalLinkTarget.File -> "Zerus reads ${target.reference.path} on $computer, the computer of this session." +
                (if(target.host.isNotEmpty() && !target.host.equals("localhost",true)) " The link names host ${target.host}." else "")
            is TerminalLinkTarget.Other -> "Zerus opens only web and file links. You can copy this one."
        },style = MaterialTheme.typography.bodySmall)
    } },confirmButton = { when(target) {
        is TerminalLinkTarget.Web -> TextButton(onClick = {
            onDismiss()
            try {
                context.startActivity(Intent(Intent.ACTION_VIEW,Uri.parse(target.uri)).addCategory(Intent.CATEGORY_BROWSABLE).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK))
            } catch(_: ActivityNotFoundException) {
                Toast.makeText(context,"No browser could open this link.",Toast.LENGTH_SHORT).show()
            } catch(_: SecurityException) {
                Toast.makeText(context,"This link could not be opened.",Toast.LENGTH_SHORT).show()
            }
        }) { Text("Open in browser") }
        is TerminalLinkTarget.File -> TextButton(onClick = { onFile(target.reference) }) { Text("Read on $computer") }
        is TerminalLinkTarget.Other -> TextButton(onClick = onDismiss) { Text("Close") }
    } },dismissButton = { Row {
        TextButton(onClick = { clipboard.setText(AnnotatedString(target.uri));onDismiss() }) { Text("Copy link") }
        if(target !is TerminalLinkTarget.Other) TextButton(onClick = onDismiss) { Text("Cancel") }
    } })
}

@Composable internal fun TerminalScreenView(screen: TerminalScreen,raw: JSONObject?,fontSize: Float,selecting: Boolean,onLink: (TerminalLink) -> Unit = {}) {
    val vertical = rememberScrollState()
    val horizontal = rememberScrollState()
    val density = LocalDensity.current
    val measurer = rememberTextMeasurer()
    val style = remember(fontSize) { TextStyle(fontSize = fontSize.sp,lineHeight = (fontSize * 1.3f).sp,fontFamily = FontFamily.Monospace,
        color = TerminalForeground,platformStyle = PlatformTextStyle(includeFontPadding = false)) }
    val cellWidth = remember(style) { measurer.measure("M",style).size.width.toFloat() }
    val lineHeight = with(density) { (fontSize * 1.3f).sp.toPx() }
    val linkTapped by rememberUpdatedState(onLink)
    val annotated = remember(screen,selecting) { buildAnnotatedString {
        append(screen.text)
        screen.spans.forEach { span ->
            val s = span.style
            val foreground = Color(if(s.inverse) s.background ?: TerminalBackground.toArgbCompat() else s.foreground ?: TerminalForeground.toArgbCompat())
            val background = Color(if(s.inverse) s.foreground ?: TerminalForeground.toArgbCompat() else s.background ?: TerminalBackground.toArgbCompat())
            addStyle(SpanStyle(color = if(s.dim) foreground.copy(alpha = .5f) else foreground,background = background,
                fontWeight = if(s.bold) FontWeight.Bold else FontWeight.Normal,fontStyle = if(s.italic) FontStyle.Italic else FontStyle.Normal,
                textDecoration = TextDecoration.combine(buildList { if(s.underline) add(TextDecoration.Underline);if(s.strike) add(TextDecoration.LineThrough) })),span.start,span.end)
        }
        // OSC 8 links stay plain underlined text while selecting; otherwise a tap asks first.
        val underline = SpanStyle(textDecoration = TextDecoration.Underline)
        screen.links.forEach { link ->
            if(selecting) addStyle(underline,link.start,link.end)
            else addLink(LinkAnnotation.Clickable(link.uri,TextLinkStyles(underline)) { linkTapped(link) },link.start,link.end)
        }
    } }
    val cols = raw?.optInt("cols")?.coerceIn(1,1000) ?: 1
    val rows = raw?.optInt("rows")?.coerceIn(1,1000) ?: 1
    val cursor = raw?.optJSONObject("cursor")
    val x = cursor?.optInt("x",-1) ?: -1
    val y = cursor?.optInt("y",-1) ?: -1
    val size = with(density) { (cols * cellWidth).toDp() to (rows * lineHeight).toDp() }
    // Following starts on open, stops when the user scrolls away and never moves a selection.
    var following by remember { mutableStateOf(true) }
    var dragged by remember { mutableStateOf(false) }
    val measured = vertical.maxValue != Int.MAX_VALUE && horizontal.maxValue != Int.MAX_VALUE
    val followedY = TerminalFollow.vertical(TerminalFollow.row(y,rows),lineHeight,vertical.viewportSize.toFloat(),vertical.maxValue.toFloat())
    val followedX = TerminalFollow.horizontal(x,cols,cellWidth,horizontal.viewportSize.toFloat(),horizontal.maxValue.toFloat())
    val followed by rememberUpdatedState(Offset(followedX,followedY))
    val cell by rememberUpdatedState(Size(cellWidth,lineHeight))
    LaunchedEffect(followedY,followedX,following,selecting,measured) {
        if(following && !selecting && measured) {
            vertical.scrollTo(followedY.roundToInt())
            horizontal.scrollTo(followedX.roundToInt())
        }
    }
    LaunchedEffect(vertical,horizontal) {
        merge(vertical.interactionSource.interactions,horizontal.interactionSource.interactions).collect {
            if(it is DragInteraction.Start) { dragged = true;following = false }
        }
    }
    LaunchedEffect(vertical,horizontal) {
        snapshotFlow { vertical.isScrollInProgress || horizontal.isScrollInProgress }.collect { moving ->
            if(!moving && dragged) {
                dragged = false
                following = TerminalFollow.atFollowed(vertical.value.toFloat(),horizontal.value.toFloat(),followed.y,followed.x,cell.height,cell.width)
            }
        }
    }
    @Composable fun CapturedText() {
        Text(annotated,style = style,softWrap = false,modifier = Modifier.widthIn(min = size.first).heightIn(min = size.second).drawWithContent {
            drawContent()
            // Native coordinates also cover a cursor on blank EOL, where no glyph exists.
            if(!selecting && x in 0 until cols && y in 0 until rows) drawRect(Color.White,
                Offset(x * cellWidth,y * lineHeight),Size(cellWidth,lineHeight),style = Stroke(1.dp.toPx()))
        })
    }
    Box(Modifier.fillMaxSize()) {
        Box(Modifier.fillMaxSize().horizontalScroll(horizontal)) {
            Box(Modifier.verticalScroll(vertical)) {
                if(selecting) SelectionContainer { CapturedText() } else CapturedText()
            }
        }
        if(!following && !selecting) FilledTonalButton(onClick = { following = true },Modifier.align(Alignment.BottomEnd).padding(8.dp)) {
            Icon(Icons.Default.VerticalAlignBottom,null)
            Spacer(Modifier.width(6.dp))
            Text("Follow cursor")
        }
    }
}

@Composable private fun TerminalInputEditor(model: ZerusViewModel,target: Target,selecting: Boolean,fresh: Boolean,onKey: (String) -> Unit) {
    val buffer = model.terminalBuffer(target)
    val lane = target.key + ":terminal:" + buffer?.binding.orEmpty()
    val editor = remember(lane) { TextFieldState(initialText = buffer?.text.orEmpty()) }
    var sync by remember(editor) { mutableStateOf(ComposerSync(buffer?.generation.orEmpty())) }
    LaunchedEffect(editor,buffer?.generation) {
        buffer?.let {
            val update = ComposerReconciler.external(sync,it.generation,it.text);sync = update.sync
            update.replacement?.let { text -> editor.edit { replace(0,length,text);selection = TextRange(text.length) } }
        }
    }
    LaunchedEffect(editor) {
        snapshotFlow { editor.text.toString() }.distinctUntilChanged().collect { text ->
            val current = model.terminalBuffer(target)
            if(current?.binding == buffer?.binding && text != current?.text) model.editTerminal(target,text)
        }
    }
    val reason = model.terminalReason(target)
    val tooLarge by remember(editor) { derivedStateOf { editor.text.toString().toByteArray(Charsets.UTF_8).size > 65536 } }
    val enabled = reason.isBlank() && fresh && !selecting && buffer != null
    Column(Modifier.fillMaxWidth().background(MaterialTheme.colorScheme.surface).padding(8.dp)) {
        if(tooLarge) Text("Terminal input exceeds 64 KiB. Shorten it before sending or saving.",style = MaterialTheme.typography.labelSmall,color = MaterialTheme.colorScheme.secondary)
        if(reason.isNotBlank()) Text(reason,style = MaterialTheme.typography.labelSmall,color = MaterialTheme.colorScheme.onSurfaceVariant)
        Row(Modifier.fillMaxWidth().horizontalScroll(rememberScrollState()),horizontalArrangement = Arrangement.spacedBy(4.dp)) {
            listOf("Escape" to "Esc","Tab" to "Tab","BTab" to "Shift+Tab","BSpace" to "⌫","Up" to "↑","Down" to "↓","Left" to "←","Right" to "→",
                "Home" to "Home","End" to "End","PPage" to "PgUp","NPage" to "PgDn","DC" to "Delete","C-c" to "Ctrl+C","C-d" to "Ctrl+D",
                "C-l" to "Ctrl+L","C-a" to "Ctrl+A","C-e" to "Ctrl+E","C-u" to "Ctrl+U","C-w" to "Ctrl+W").forEach { (key,label) ->
                OutlinedButton(onClick = { onKey(key) },enabled = enabled,contentPadding = PaddingValues(horizontal = 10.dp)) { Text(label) }
            }
        }
        Row(verticalAlignment = Alignment.Bottom) {
            BasicTextField(state = editor,modifier = Modifier.weight(1f).padding(10.dp),readOnly = buffer == null || selecting,
                textStyle = MaterialTheme.typography.bodyMedium.copy(color = MaterialTheme.colorScheme.onSurface,fontFamily = FontFamily.Monospace),
                cursorBrush = SolidColor(MaterialTheme.colorScheme.primary),lineLimits = TextFieldLineLimits.MultiLine(1,4),
                keyboardOptions = KeyboardOptions.Default.copy(imeAction = ImeAction.Default),decorator = { inner -> Box {
                    if(editor.text.isEmpty()) Text("Terminal input (kept separately)",color = MaterialTheme.colorScheme.onSurfaceVariant,style = MaterialTheme.typography.bodySmall)
                    inner()
                } })
            Column {
                OutlinedButton(onClick = {
                    model.editTerminal(target,editor.text.toString())
                    model.terminalInput(target,enter = false)
                },enabled = enabled && !tooLarge && editor.text.isNotEmpty()) { Text("Insert text") }
                Button(onClick = {
                    model.editTerminal(target,editor.text.toString())
                    if(editor.text.isEmpty()) onKey("Enter") else model.terminalInput(target,enter = true)
                },enabled = enabled && !tooLarge) { Text("Enter / Send") }
            }
        }
        Text("Insert text sends without a newline. Enter sends buffered text and submits it. Special keys go directly to the native program.",style = MaterialTheme.typography.labelSmall,color = MaterialTheme.colorScheme.onSurfaceVariant)
    }
}

private fun Color.toArgbCompat(): Int = (0xff shl 24) or ((red * 255).toInt() shl 16) or ((green * 255).toInt() shl 8) or (blue * 255).toInt()
