/*
 * Copyright 2025 mox
 * Licensed under the Apache License, Version 2.0 (see Zerus/Licenses/mux-pod.txt).
 * Adapted from MuxPod lib/services/terminal/ansi_parser.dart, commit
 * c33d0fdeb66ca7bbdf4745203578088961982c13.
 * Modified for Zerus: Swift model ported from the Kotlin adaptation, bounded parsing,
 * control suppression and cursor mapping.
 */
import Foundation

public struct TerminalStyle: Hashable, Sendable {
    public var foreground: UInt32?
    public var background: UInt32?
    public var bold = false
    public var dim = false
    public var italic = false
    public var underline = false
    public var strike = false
    public var inverse = false

    public init(foreground: UInt32? = nil, background: UInt32? = nil, bold: Bool = false, dim: Bool = false, italic: Bool = false,
                underline: Bool = false, strike: Bool = false, inverse: Bool = false) {
        self.foreground = foreground
        self.background = background
        self.bold = bold
        self.dim = dim
        self.italic = italic
        self.underline = underline
        self.strike = strike
        self.inverse = inverse
    }
}

/// A styled range of `TerminalScreen.text` in UTF-16 offsets.
public struct TerminalSpan: Hashable, Sendable {
    public let start: Int
    public let end: Int
    public let style: TerminalStyle
}

public struct TerminalScreen: Hashable, Sendable {
    public let text: String
    public let spans: [TerminalSpan]
    public let truncated: Bool
}

/// Parses captured text, not a terminal emulator. Escape sequences never execute actions.
public final class TerminalAnsi: @unchecked Sendable {
    struct Line {
        let text: String
        let spans: [TerminalSpan]
        let length: Int
        let endStyle: TerminalStyle
    }

    struct Key: Hashable {
        let style: TerminalStyle
        let raw: String
    }

    private var previous: [Key: Line] = [:]
    private let lock = NSLock()

    public init() {}

    static let escape: Unicode.Scalar = "\u{1b}"
    static let c1: Set<UInt32> = [0x9b, 0x9d, 0x90, 0x9e, 0x9f]

    public func parse(_ input: String) -> TerminalScreen {
        lock.lock()
        defer { lock.unlock() }
        let all = input.unicodeScalars
        let bounded = Array(all.prefix(128 * 1024))
        var truncated = bounded.count < all.count
        var clean = String.UnicodeScalarView()
        var i = 0
        // Keep only bounded SGR; remove OSC (including hyperlinks/title/clipboard), DCS and CSI controls.
        while i < bounded.count {
            let c = bounded[i]
            i += 1
            if c == Self.escape || Self.c1.contains(c.value) {
                if c == Self.escape && i == bounded.count { break }
                let control: Unicode.Scalar
                if c == Self.escape {
                    control = bounded[i]
                    i += 1
                } else {
                    switch c.value {
                    case 0x9b: control = "["
                    case 0x9d: control = "]"
                    case 0x90: control = "P"
                    case 0x9e: control = "^"
                    default: control = "_"
                    }
                }
                switch control {
                case "[":
                    let start = i
                    while i < bounded.count, !(0x40...0x7e).contains(bounded[i].value) { i += 1 }
                    if i < bounded.count {
                        let final = bounded[i]
                        i += 1
                        let body = bounded[start..<(i - 1)]
                        if final == "m", body.count <= 256, body.allSatisfy({ ("0"..."9").contains($0) || $0 == ";" }) {
                            clean.append(Self.escape)
                            clean.append("[")
                            clean.append(contentsOf: body)
                            clean.append("m")
                        }
                    }
                case "]", "P", "^", "_":
                    while i < bounded.count {
                        if bounded[i] == "\u{07}" || bounded[i].value == 0x9c {
                            i += 1
                            break
                        }
                        if bounded[i] == Self.escape, i + 1 < bounded.count, bounded[i + 1] == "\\" {
                            i += 2
                            break
                        }
                        i += 1
                    }
                case "(", ")":
                    if i < bounded.count { i += 1 } // Character set designation.
                default:
                    break
                }
            } else if c == "\n" || c == "\t" || (c.value >= 0x20 && !(0x7f...0x9f).contains(c.value)) {
                clean.append(c)
            }
        }
        var next: [Key: Line] = [:]
        var text = ""
        var length = 0
        var spans: [TerminalSpan] = []
        var style = TerminalStyle()
        let lines = String(clean).split(separator: "\n", omittingEmptySubsequences: false)
        for (index, raw) in lines.prefix(1000).enumerated() {
            let key = Key(style: style, raw: String(raw))
            let line = next[key] ?? previous[key] ?? Self.line(key.raw, style)
            next[key] = line
            if index > 0 {
                text += "\n"
                length += 1
            }
            let offset = length
            text += line.text
            length += line.length
            if spans.count + line.spans.count > 8192 {
                truncated = true
                break
            }
            spans += line.spans.map { TerminalSpan(start: $0.start + offset, end: $0.end + offset, style: $0.style) }
            style = line.endStyle
        }
        previous = next
        return TerminalScreen(text: text, spans: spans, truncated: truncated || lines.count > 1000)
    }

    static func line(_ raw: String, _ initial: TerminalStyle) -> Line {
        var style = initial
        var text = String.UnicodeScalarView()
        var length = 0
        var spans: [TerminalSpan] = []
        var start = 0
        var column = 0
        let scalars = Array(raw.unicodeScalars)
        var i = 0
        func flush() {
            if length > start { spans.append(TerminalSpan(start: start, end: length, style: style)) }
            start = length
        }
        while i < scalars.count {
            let scalar = scalars[i]
            if scalar == escape {
                flush()
                var end = i + 2
                while end < scalars.count, scalars[end] != "m" { end += 1 }
                style = sgr(String(String.UnicodeScalarView(scalars[min(i + 2, end)..<end])), style)
                i = end + 1
            } else if scalar == "\t" {
                let spaces = 8 - column % 8
                text.append(contentsOf: repeatElement(" " as Unicode.Scalar, count: spaces))
                length += spaces
                column += spaces
                i += 1
            } else {
                text.append(scalar)
                length += scalar.utf16.count
                column += cellWidth(scalar)
                i += 1
            }
        }
        flush()
        return Line(text: String(text), spans: spans, length: length, endStyle: style)
    }

    static func sgr(_ body: String, _ initial: TerminalStyle) -> TerminalStyle {
        if body.isEmpty { return TerminalStyle() }
        let codes = body.split(separator: ";", omittingEmptySubsequences: false).map { $0.isEmpty ? 0 : Int($0) ?? -1 }
        var style = initial
        var i = 0
        while i < codes.count {
            let code = codes[i]
            i += 1
            switch code {
            case 0: style = TerminalStyle()
            case 1: style.bold = true
            case 2: style.dim = true
            case 3: style.italic = true
            case 4: style.underline = true
            case 7: style.inverse = true
            case 9: style.strike = true
            case 21, 22:
                style.bold = false
                style.dim = false
            case 23: style.italic = false
            case 24: style.underline = false
            case 27: style.inverse = false
            case 29: style.strike = false
            case 30...37: style.foreground = palette(code - 30)
            case 40...47: style.background = palette(code - 40)
            case 90...97: style.foreground = palette(code - 90 + 8)
            case 100...107: style.background = palette(code - 100 + 8)
            case 39: style.foreground = nil
            case 49: style.background = nil
            case 38, 48:
                var color: UInt32?
                let mode = i < codes.count ? codes[i] : nil
                i += 1
                if mode == 5 {
                    if i < codes.count, (0...255).contains(codes[i]) { color = palette(codes[i]) }
                    i += 1
                } else if mode == 2 {
                    if i + 2 < codes.count {
                        let rgb = codes[i..<(i + 3)]
                        i += 3
                        if rgb.allSatisfy({ (0...255).contains($0) }) { color = argb(rgb[rgb.startIndex], rgb[rgb.startIndex + 1], rgb[rgb.startIndex + 2]) }
                    } else {
                        i = codes.count
                    }
                }
                if let color {
                    if code == 38 { style.foreground = color } else { style.background = color }
                }
            default: break
            }
        }
        return style
    }

    static let basic: [UInt32] = [0xFF00_0000, 0xFFCD_3131, 0xFF0D_BC79, 0xFFE5_E510, 0xFF24_72C8, 0xFFBC_3FBC, 0xFF11_A8CD, 0xFFE5_E5E5,
                                  0xFF66_6666, 0xFFF1_4C4C, 0xFF23_D18B, 0xFFF5_F543, 0xFF3B_8EEA, 0xFFD6_70D6, 0xFF29_B8DB, 0xFFFF_FFFF]

    static func argb(_ r: Int, _ g: Int, _ b: Int) -> UInt32 { 0xFF00_0000 | UInt32(r) << 16 | UInt32(g) << 8 | UInt32(b) }

    public static func palette(_ index: Int) -> UInt32 {
        precondition((0...255).contains(index))
        if index < 16 { return basic[index] }
        if index < 232 {
            let n = index - 16
            func level(_ value: Int) -> Int { value == 0 ? 0 : value * 40 + 55 }
            return argb(level(n / 36 % 6), level(n / 6 % 6), level(n % 6))
        }
        let gray = (index - 232) * 10 + 8
        return argb(gray, gray, gray)
    }

    public static func cellWidth(_ scalar: Unicode.Scalar) -> Int {
        switch scalar.properties.generalCategory {
        case .nonspacingMark, .enclosingMark, .spacingMark: return 0
        default: break
        }
        let cp = scalar.value
        let wide: [ClosedRange<UInt32>] = [0x1100...0x115f, 0x2e80...0xa4cf, 0xac00...0xd7a3, 0xf900...0xfaff, 0xfe10...0xfe6f, 0xff00...0xff60,
                                           0x1f300...0x1faff, 0x20000...0x3ffff]
        return wide.contains { $0.contains(cp) } ? 2 : 1
    }

    /// Maps the native cell position to a UTF-16 range without splitting surrogate pairs.
    public static func cursorRange(_ text: String, x: Int, y: Int) -> Range<Int>? {
        guard (0...999).contains(x), (0...999).contains(y) else { return nil }
        let lines = text.split(separator: "\n", omittingEmptySubsequences: false)
        guard y < lines.count else { return nil }
        var offset = lines[..<y].reduce(0) { $0 + $1.utf16.count + 1 }
        var cells = 0
        for scalar in lines[y].unicodeScalars {
            let units = scalar.utf16.count, width = cellWidth(scalar)
            if width > 0, (cells..<(cells + width)).contains(x) { return offset..<(offset + units) }
            cells += width
            offset += units
        }
        return nil
    }
}

/// Reading is adaptive; this policy never retries terminal input.
public struct TerminalPolling: Equatable, Sendable {
    public var unchanged = 0

    public init(unchanged: Int = 0) { self.unchanged = unchanged }

    public var delayMilliseconds: Int { min(250 << min(max(unchanged, 0), 3), 2000) }

    public static func readAllowed(foreground: Bool, focused: Bool, selecting: Bool, reviewing: Bool, ownedKeyConfirmation: Bool) -> Bool {
        foreground && !selecting && !reviewing && (focused || ownedKeyConfirmation)
    }

    public func observed(changed: Bool) -> TerminalPolling { changed ? TerminalPolling() : TerminalPolling(unchanged: min(unchanged + 1, 3)) }
}

/// Terminal input reducers: a buffer is cleared only by the durable write that records its request.
public enum TerminalBuffers {
    public static func edit(_ state: MessageState, _ editor: TerminalBuffer) -> MessageState {
        var next = editor
        if var current = state.terminalBuffers.first(where: { $0.key == editor.key }) {
            current.text = editor.text
            current.revision = editor.revision
            next = current
        }
        var copy = state
        copy.terminalBuffers = state.terminalBuffers.filter { $0.key != editor.key } + [next]
        return copy
    }

    public static func enqueue(_ state: MessageState, _ buffer: TerminalBuffer, _ action: SessionAction) throws -> MessageState {
        if let current = state.terminalBuffers.first(where: { $0.key == buffer.key }), current.revision != buffer.revision {
            throw ZerusError.invalid("The terminal buffer changed.")
        }
        guard action.arguments.string("text") == buffer.text, action.arguments.has("text") else { throw ZerusError.invalid("The terminal input changed.") }
        var cleared = buffer
        cleared.text = ""
        cleared.revision = UUID().uuidString.lowercased()
        cleared.generation = UUID().uuidString.lowercased()
        var copy = state.action(action)
        copy.terminalBuffers = state.terminalBuffers.filter { $0.key != buffer.key } + [cleared]
        return copy
    }

    public static func acknowledge(_ current: MessageState, _ written: MessageState, _ buffer: TerminalBuffer, _ action: SessionAction) -> MessageState {
        let editor = current.terminalBuffers.first { $0.key == buffer.key }
        guard let cleared = written.terminalBuffers.first(where: { $0.key == buffer.key }),
              let recorded = written.sessionActions.first(where: { $0.requestId == action.requestId }) else { return current }
        let next: TerminalBuffer
        if editor == nil || editor?.revision == buffer.revision {
            next = cleared
        } else if editor?.generation != buffer.generation {
            next = editor!
        } else {
            var typed = cleared
            typed.text = editor!.text
            typed.revision = editor!.revision
            next = typed
        }
        var copy = current.action(recorded)
        copy.terminalBuffers = current.terminalBuffers.filter { $0.key != buffer.key } + [next]
        return copy
    }
}
