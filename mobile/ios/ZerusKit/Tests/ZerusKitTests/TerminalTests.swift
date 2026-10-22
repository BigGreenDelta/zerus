import Synchronization
import XCTest
@testable import ZerusKit

/// Ported from Android TerminalAnsiTest and TerminalPollingTest.
final class TerminalAnsiTests: XCTestCase {
    private let esc = "\u{1b}"

    func testBasicBrightAndResetColorsMatchMuxPod() {
        let screen = TerminalAnsi().parse("\(esc)[31mred\(esc)[92mgreen\(esc)[0mplain")
        XCTAssertEqual("redgreenplain", screen.text)
        XCTAssertEqual(0xFFCD_3131, screen.spans[0].style.foreground)
        XCTAssertEqual(0xFF23_D18B, screen.spans[1].style.foreground)
        XCTAssertEqual(TerminalStyle(), screen.spans[2].style)
    }

    func testExtendedColorsAndIndependentAttributes() {
        let screen = TerminalAnsi().parse("\(esc)[1;2;3;4;7;9;38;5;196;48;2;1;2;3ma\(esc)[22;23;24;27;29;39;49mb")
        XCTAssertEqual(TerminalStyle(foreground: 0xFFFF_0000, background: 0xFF01_0203, bold: true, dim: true, italic: true, underline: true, strike: true, inverse: true),
                       screen.spans[0].style)
        XCTAssertEqual(TerminalStyle(), screen.spans[1].style)
        XCTAssertEqual(0xFF08_0808, TerminalAnsi.palette(232))
        XCTAssertEqual(0xFFEE_EEEE, TerminalAnsi.palette(255))
    }

    func testStyleContinuesAcrossLinesAndCachedReusePreservesIt() {
        let parser = TerminalAnsi()
        let first = parser.parse("\(esc)[32mfirst\nsecond")
        let next = parser.parse("\(esc)[31mfirst\nsecond")
        XCTAssertEqual(0xFF0D_BC79, first.spans.last?.style.foreground)
        XCTAssertEqual(0xFFCD_3131, next.spans.last?.style.foreground)
        XCTAssertEqual(next, parser.parse("\(esc)[31mfirst\nsecond"))
    }

    func testOscClipboardTitleAndLinksNeverReachVisibleText() {
        let screen = TerminalAnsi().parse("A\(esc)]52;c;secret\u{07}B\(esc)]0;title\(esc)\\C\(esc)]8;;https://example.invalid\u{07}label\(esc)]8;;\(esc)\\D")
        XCTAssertEqual("ABClabelD", screen.text)
        XCTAssertFalse(screen.text.contains("secret"))
        // Only the OSC 8 target is kept, as a link range; it never becomes visible text.
        XCTAssertEqual([TerminalLink(start: 3, end: 8, uri: "https://example.invalid")], screen.links)
    }

    func testOsc8LinksBecomeRangesWithEitherTerminator() {
        // tmux 3.8 capture-pane -e output, plus a BEL-terminated link.
        let screen = TerminalAnsi().parse("see \(esc)]8;id=a;https://example.com/docs?q=1;x\(esc)\\docs link\(esc)]8;;\(esc)\\ and "
            + "\(esc)]8;;file:///tmp/a%20b.txt\u{07}a file\(esc)]8;;\u{07} end")
        XCTAssertEqual("see docs link and a file end", screen.text)
        XCTAssertEqual([TerminalLink(start: 4, end: 13, uri: "https://example.com/docs?q=1;x"),
                        TerminalLink(start: 18, end: 24, uri: "file:///tmp/a%20b.txt")], screen.links)
    }

    func testOsc8LinkContinuesAcrossLinesAndCachedLinesKeepIt() {
        let parser = TerminalAnsi()
        let input = "\(esc)]8;;https://example.com/\(esc)\\ab\ncd\(esc)]8;;\(esc)\\e\nab"
        let screen = parser.parse(input)
        XCTAssertEqual("ab\ncde\nab", screen.text)
        XCTAssertEqual([TerminalLink(start: 0, end: 2, uri: "https://example.com/"), TerminalLink(start: 3, end: 5, uri: "https://example.com/")], screen.links)
        // The last "ab" line has the same text but no open link, so the cache must not reuse the first one.
        XCTAssertEqual(screen, parser.parse(input))
    }

    func testInvalidOrUnterminatedOsc8TargetsEndLinksInsteadOfOpeningThem() {
        let long = "https://example.com/" + String(repeating: "a", count: TerminalAnsi.maxLinkLength)
        for bad in ["https://example.com/a b", "https://example.com/\u{e9}", long] {
            let screen = TerminalAnsi().parse("\(esc)]8;;https://example.com/\(esc)\\x\(esc)]8;;\(bad)\(esc)\\y")
            XCTAssertEqual("xy", screen.text)
            XCTAssertEqual([TerminalLink(start: 0, end: 1, uri: "https://example.com/")], screen.links)
        }
        XCTAssertEqual([], TerminalAnsi().parse("a\(esc)]8;;https://example.com/b").links)
        XCTAssertEqual([], TerminalAnsi().parse("\(esc)]8;https://example.com/\(esc)\\x").links)
    }

    func testLinkCountIsBoundedWithoutHidingText() {
        // Four links per row: 260 rows stay under the row bound and exceed the link bound.
        let input = (0..<260).map { row in
            (0..<4).map { "\(esc)]8;;https://example.com/\(row)/\($0)\(esc)\\\(row).\($0)\(esc)]8;;\(esc)\\" }.joined(separator: " ")
        }.joined(separator: "\n")
        let screen = TerminalAnsi().parse(input)
        XCTAssertEqual(TerminalAnsi.maxLinks, screen.links.count)
        XCTAssertEqual("https://example.com/255/3", screen.links.last?.uri)
        XCTAssertFalse(screen.truncated)
        XCTAssertTrue(screen.text.hasSuffix("\n259.0 259.1 259.2 259.3"))
    }

    private func web(_ uri: String) -> String? {
        if case .web(let uri, _) = TerminalLinkTarget.of(uri) { return uri }
        return nil
    }

    func testOnlyWellFormedWebLinksOpenInTheBrowser() {
        XCTAssertEqual("https://example.com/docs?q=1", web("https://example.com/docs?q=1"))
        XCTAssertEqual("http://example.com:8080/", web("http://example.com:8080/"))
        for other in ["https://user@example.com/", "https:example.com", "javascript:alert(1)", "mailto:a@example.com",
                      "intent://scan/#Intent;scheme=zxing;end", "ssh://example.com", "https://example.com/%0d"] {
            XCTAssertEqual(.other(other), TerminalLinkTarget.of(other), other)
        }
    }

    func testFileLinksKeepTheirHostAndReadThePathOnTheSessionComputer() {
        XCTAssertEqual(.file(uri: "file://archlinux/srv/a%20b.txt", host: "archlinux", reference: ComputerFileReference(path: "/srv/a b.txt")),
                       TerminalLinkTarget.of("file://archlinux/srv/a%20b.txt"))
        XCTAssertEqual(.file(uri: "file:///tmp/x.kt#L12", host: "", reference: ComputerFileReference(path: "/tmp/x.kt", location: "#L12")),
                       TerminalLinkTarget.of("file:///tmp/x.kt#L12"))
        XCTAssertEqual(.file(uri: "file://localhost/src/main.rs:42", host: "localhost", reference: ComputerFileReference(path: "/src/main.rs", location: ":42")),
                       TerminalLinkTarget.of("file://localhost/src/main.rs:42"))
        for other in ["file://host/a?b", "file://user@host/a", "file://host:22/a", "file:relative", "file://host//double", "file://host/a%5Cb"] {
            XCTAssertEqual(.other(other), TerminalLinkTarget.of(other), other)
        }
    }

    func testC1ControlsAndOtherEscapePayloadsAreDiscarded() {
        XCTAssertEqual("abcd", TerminalAnsi().parse("a\u{9d}52;c;private\u{9c}b\(esc)Pprivate\(esc)\\c\(esc)[2Jd").text)
        XCTAssertEqual("safe", TerminalAnsi().parse("safe\(esc)]52;unterminated").text)
    }

    func testUnicodeTabsUseCellsWithoutSplittingSurrogates() {
        XCTAssertEqual("界      x", TerminalAnsi().parse("界\tx").text)
        XCTAssertEqual("e\u{301}       x", TerminalAnsi().parse("e\u{301}\tx").text)
        XCTAssertEqual("😀      x", TerminalAnsi().parse("😀\tx").text)
        XCTAssertEqual(0..<2, TerminalAnsi.cursorRange("😀x", x: 1, y: 0))
        XCTAssertEqual(2..<3, TerminalAnsi.cursorRange("😀x", x: 2, y: 0))
        XCTAssertEqual(3..<4, TerminalAnsi.cursorRange("ab\ncd", x: 0, y: 1))
        // Blank end-of-line is rendered using the native cell coordinates, not a character range.
        XCTAssertNil(TerminalAnsi.cursorRange("x", x: 1, y: 0))
        XCTAssertNil(TerminalAnsi.cursorRange("", x: 0, y: 0))
    }

    func testSpansUseUtf16OffsetsAcrossLines() {
        let screen = TerminalAnsi().parse("😀\(esc)[31mx\ny")
        XCTAssertEqual([TerminalSpan(start: 0, end: 2, style: TerminalStyle()), TerminalSpan(start: 2, end: 3, style: TerminalStyle(foreground: 0xFFCD_3131)),
                        TerminalSpan(start: 4, end: 5, style: TerminalStyle(foreground: 0xFFCD_3131))], screen.spans)
    }

    func testEmptyAndMalformedExtendedSequencesDoNotInventColors() {
        XCTAssertEqual([TerminalStyle()], TerminalAnsi().parse("\(esc)[38;2;999;0;0mx").spans.map(\.style))
        XCTAssertEqual("text", TerminalAnsi().parse("text\(esc)[").text)
        XCTAssertTrue(TerminalAnsi().parse(String(repeating: "x", count: 140_000)).truncated)
        XCTAssertTrue(TerminalAnsi().parse(String(repeating: "\n", count: 1200)).truncated)
    }

    func testHeldKeyConfirmationKeepsReadonlyFreshnessWithoutAdoptingInputBinding() {
        XCTAssertTrue(TerminalPolling.readAllowed(foreground: true, focused: false, selecting: false, reviewing: false, ownedKeyConfirmation: true))
        XCTAssertFalse(TerminalPolling.readAllowed(foreground: true, focused: false, selecting: false, reviewing: false, ownedKeyConfirmation: false))
        XCTAssertFalse(TerminalPolling.readAllowed(foreground: false, focused: false, selecting: false, reviewing: false, ownedKeyConfirmation: true))
        XCTAssertFalse(TerminalPolling.readAllowed(foreground: true, focused: false, selecting: true, reviewing: false, ownedKeyConfirmation: true))
        XCTAssertFalse(TerminalPolling.readAllowed(foreground: true, focused: false, selecting: false, reviewing: true, ownedKeyConfirmation: true))
    }

    func testIdleReadsBackOffAndNewOutputRestoresResponsiveness() {
        var policy = TerminalPolling()
        XCTAssertEqual(250, policy.delayMilliseconds)
        policy = policy.observed(changed: false)
        XCTAssertEqual(500, policy.delayMilliseconds)
        policy = policy.observed(changed: false)
        XCTAssertEqual(1000, policy.delayMilliseconds)
        for _ in 0..<50 { policy = policy.observed(changed: false) }
        XCTAssertEqual(2000, policy.delayMilliseconds)
        XCTAssertEqual(250, policy.observed(changed: true).delayMilliseconds)
    }
}

/// Ported from Android TerminalFollowTest: a 50-row pane of 10 pt lines in a 200 pt viewport.
final class TerminalFollowTests: XCTestCase {
    func testCursorNearTheBottomFollowsTheBottomOfThePane() {
        XCTAssertEqual(47, TerminalFollow.row(cursorY: 47, rows: 50))
        XCTAssertEqual(300, TerminalFollow.vertical(row: 47, line: 10, viewport: 200, max: 300))
        XCTAssertEqual(300, TerminalFollow.vertical(row: 30, line: 10, viewport: 200, max: 300))
    }

    func testHigherCursorSitsAtTheBottomEdgeWithTheRowsAboveIt() {
        XCTAssertEqual(100, TerminalFollow.vertical(row: 29, line: 10, viewport: 200, max: 300))
        XCTAssertEqual(10, TerminalFollow.vertical(row: 20, line: 10, viewport: 200, max: 300))
        XCTAssertEqual(0, TerminalFollow.vertical(row: 0, line: 10, viewport: 200, max: 300))
        XCTAssertEqual(0, TerminalFollow.vertical(row: 5, line: 10, viewport: 200, max: 0))
    }

    func testMissingCursorFollowsTheLastRowAndTheLeftEdge() {
        XCTAssertEqual(49, TerminalFollow.row(cursorY: -1, rows: 50))
        XCTAssertEqual(49, TerminalFollow.row(cursorY: 50, rows: 50))
        XCTAssertEqual(300, TerminalFollow.vertical(row: TerminalFollow.row(cursorY: -1, rows: 50), line: 10, viewport: 200, max: 300))
        XCTAssertEqual(0, TerminalFollow.horizontal(cursorX: -1, cols: 200, cell: 5, viewport: 100, max: 900))
    }

    func testWideCursorColumnStaysAtTheRightEdge() {
        XCTAssertEqual(0, TerminalFollow.horizontal(cursorX: 19, cols: 200, cell: 5, viewport: 100, max: 900))
        XCTAssertEqual(5, TerminalFollow.horizontal(cursorX: 20, cols: 200, cell: 5, viewport: 100, max: 900))
        XCTAssertEqual(900, TerminalFollow.horizontal(cursorX: 199, cols: 200, cell: 5, viewport: 100, max: 900))
    }

    func testOnlyAReturnToTheFollowedPositionResumesFollowing() {
        XCTAssertTrue(TerminalFollow.atFollowed(y: 296, x: 2, followedY: 300, followedX: 0, line: 10, cell: 5))
        XCTAssertFalse(TerminalFollow.atFollowed(y: 290, x: 0, followedY: 300, followedX: 0, line: 10, cell: 5))
        XCTAssertFalse(TerminalFollow.atFollowed(y: 300, x: 10, followedY: 300, followedX: 0, line: 10, cell: 5))
    }
}

/// Ported from Android TerminalBufferTest.
@MainActor
final class TerminalBufferTests: XCTestCase {
    private let target = Target(computerId: "computer", session: "session", run: "run", conversation: "conversation", connectionId: "workspace")
    private var buffer: TerminalBuffer { TerminalBuffer(target: target, binding: "binding", text: "literal input", revision: "revision", generation: "generation") }
    private var action: SessionAction {
        SessionAction(requestId: "request", target: target, operation: "terminal_input",
                      arguments: ["terminal_binding_id": "binding", "text": .string(buffer.text), "enter": true])
    }

    private final class Gate: Sendable {
        let entered = Mutex(false)
        let released = Mutex(false)
        let failing: Bool
        init(failing: Bool = false) { self.failing = failing }
        func write() throws {
            if failing { throw ZerusError.invalid("Synthetic storage failure") }
            entered.withLock { $0 = true }
            while !released.withLock({ $0 }) { Thread.sleep(forTimeInterval: 0.005) }
        }
    }

    func testAcceptedInputWritePreservesNextTerminalTypingAndOrdinaryDraft() async throws {
        let chat = Draft(target: target, text: "Private chat draft", updatedAt: 1)
        let gate = Gate()
        let writer = OrderedStatePersistence(initial: MessageState(drafts: [chat], terminalBuffers: [buffer]), debounce: .milliseconds(10),
                                             maxDelay: .milliseconds(20)) { _ in try gate.write() }
        let buffer = self.buffer, action = self.action
        async let pending = writer.durable({ try TerminalBuffers.enqueue($0, buffer, action) },
                                           acknowledge: { current, written in TerminalBuffers.acknowledge(current, written, buffer, action) })
        while !gate.entered.withLock({ $0 }) { try await Task.sleep(for: .milliseconds(5)) }
        var typed = buffer
        typed.text = "Next terminal text"
        typed.revision = "new"
        let next = typed
        writer.edit { TerminalBuffers.edit($0, next) }
        gate.released.withLock { $0 = true }
        let written = try await pending
        _ = try await writer.flush()
        XCTAssertEqual([chat], writer.state.drafts)
        XCTAssertEqual("Next terminal text", writer.state.terminalBuffers.first?.text)
        XCTAssertEqual(written.terminalBuffers.first?.generation, writer.state.terminalBuffers.first?.generation)
        XCTAssertEqual("literal input", writer.state.sessionActions.first?.arguments.string("text"))
        writer.close()
    }

    func testFailureBeforeDurableEnqueueLeavesBothComposersUntouched() async {
        let initial = MessageState(drafts: [Draft(target: target, text: "Chat", updatedAt: 1)], terminalBuffers: [buffer])
        let gate = Gate(failing: true)
        let writer = OrderedStatePersistence(initial: initial) { _ in try gate.write() }
        let buffer = self.buffer, action = self.action
        do {
            _ = try await writer.durable { try TerminalBuffers.enqueue($0, buffer, action) }
            XCTFail("Expected storage failure")
        } catch {}
        XCTAssertEqual(initial, writer.state)
        writer.close()
    }

    func testRestartPreservesOriginalInputRequestAndIndependentBindings() throws {
        var other = buffer
        other.binding = "other-binding"
        other.text = "Other buffer"
        let state = try TerminalBuffers.enqueue(MessageState(terminalBuffers: [buffer, other]), buffer, action)
        let restored = try JSONDecoder().decode(MessageState.self, from: JSONEncoder().encode(state)).recovered()
        XCTAssertEqual("uncertain", restored.sessionActions.first?.status)
        XCTAssertEqual("request", restored.sessionActions.first?.requestId)
        XCTAssertEqual("Other buffer", restored.terminalBuffers.first { $0.binding == "other-binding" }?.text)
        XCTAssertEqual("", restored.terminalBuffers.first { $0.binding == "binding" }?.text)
    }

    func testChangedBufferOrInputCannotBeEnqueued() {
        var changed = buffer
        changed.revision = "other"
        XCTAssertThrowsError(try TerminalBuffers.enqueue(MessageState(terminalBuffers: [changed]), buffer, action))
        var edited = action
        edited.arguments["text"] = "different"
        XCTAssertThrowsError(try TerminalBuffers.enqueue(MessageState(terminalBuffers: [buffer]), buffer, edited))
    }

    func testTerminalAcceptsLiteralTextButRejectsUnlistedKeysAndMixedModes() throws {
        XCTAssertEqual(buffer.text, try SessionActionPolicies.arguments("terminal_input", action.arguments).string("text"))
        XCTAssertThrowsError(try SessionActionPolicies.arguments("terminal_input", action.arguments.putting("key", "C-c")))
        XCTAssertThrowsError(try SessionActionPolicies.arguments("terminal_input", ["terminal_binding_id": "binding", "key": "arbitrary-command"]))
    }
}
