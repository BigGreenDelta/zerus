#!/usr/bin/env python3
"""Differential test of the screen model against tmux, byte for byte.

Each stream of program output is written into a pane of a private tmux server
(its own socket and TMUX_TMPDIR, no configuration, no status line) and into
`hgs __screen-capture`; `capture-pane -p`, `capture-pane -p -e` and the pane
facts must match exactly. The model's public `-e` capture leaves out OSC 8
hyperlinks (consumers only strip them), so streams with links compare the
model's tmux form, links included. The model's repaint of every stream is
compared with tmux the same way and must show the same cells, cursor and
modes as the original screen.

The corpus covers attributes, colours, wide and joined characters, wrapping,
scroll regions, erasing, insertion and deletion, cursor movement, tabs, the
alternate screen, modes, character sets, strings that do not print, invalid
UTF-8, history and resizing, agent-like frames, the recorded screens in
tests/fixtures rendered three ways, and deterministic random streams. Raw
program output recorded elsewhere (such as ConPTY streams from Windows) joins
it as tests/fixtures/screen-streams/NAME.COLSxROWS.bin.
"""
from dataclasses import dataclass, field
import base64
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import time
import unittest

REPO = Path(__file__).resolve().parents[1]
HGS = Path(os.environ.get("HGS_TEST_BIN", REPO / "target/debug/hgs")).resolve()
TMUX = shutil.which("tmux")
FIXTURES = REPO / "tests/fixtures"
RECORDED = FIXTURES / "screen-streams"

INFO_FORMAT = (
    "#{cursor_x} #{cursor_y} #{pane_width} #{pane_height} #{alternate_on} #{bracket_paste_flag} "
    "#{keypad_cursor_flag} #{keypad_flag} #{cursor_flag} #{insert_flag} #{origin_flag} #{wrap_flag} "
    "#{mouse_standard_flag} #{mouse_button_flag} #{mouse_all_flag} #{mouse_any_flag} #{mouse_sgr_flag} "
    "#{mouse_utf8_flag} #{scroll_region_upper} #{scroll_region_lower} #{cursor_shape} #{cursor_blinking}"
)

# Writes each segment in small chunks that never split a run of printable
# ASCII, and waits until tmux has read each chunk (seen through pipe-pane)
# before the next: tmux, like the model, ends a run of collected text at the
# end of every read, so the chunks tmux reads must be the same safe ones.
# After a segment it asks for a status report and keeps what tmux answered
# before it: the answers to the segment's queries, in order.
FEEDER = r'''
import os, select, sys, time, tty
root, name = sys.argv[1], sys.argv[2]
tty.setraw(1)
pipe = os.path.join(root, name + '.pipe')
written = 0

def chunks(data, limit=512):
    def joined(at):
        return 0x20 <= data[at - 1] <= 0x7e and 0x20 <= data[at] <= 0x7e
    start = 0
    while start < len(data):
        end = min(len(data), start + limit)
        while start < end < len(data) and joined(end):
            end -= 1
        if end == start:
            end = start + 1
            while end < len(data) and joined(end):
                end += 1
        yield data[start:end]
        start = end

def send(data):
    global written
    view = memoryview(data)
    while view:
        view = view[os.write(1, view):]
    written += len(data)
    while not os.path.exists(pipe) or os.path.getsize(pipe) < written:
        time.sleep(0.001)

n = 0
while True:
    segment = os.path.join(root, '%s.%d' % (name, n))
    while not os.path.exists(segment + '.go'):
        time.sleep(0.002)
    with open(segment, 'rb') as file:
        data = file.read()
    for chunk in chunks(data):
        send(chunk)
    send(b'\x1b[5n')
    replies = b''
    while not replies.endswith(b'\x1b[0n'):
        if not select.select([0], [], [], 30)[0]:
            break
        replies += os.read(0, 4096)
    with open(os.path.join(root, name + '.replies'), 'ab') as file:
        file.write(replies[:-4])
    send(b'\x1b]2;%s-done-%d\x1b\\' % (name.encode(), n))
    n += 1
'''


def tmux_version():
    if not TMUX:
        return None
    output = subprocess.run([TMUX, "-V"], capture_output=True, text=True).stdout.strip()
    match = re.match(r"tmux (?:next-)?(\d+)\.(\d+)", output)
    return (tuple(map(int, match.groups())), output) if match else ((0, 0), output)


@dataclass
class Case:
    name: str
    rows: int
    cols: int
    steps: list = field(default_factory=list)

    def stream(self):
        return b"".join(step for step in self.steps if isinstance(step, bytes))

    def resizes(self):
        offset, result = 0, []
        for step in self.steps:
            if isinstance(step, bytes):
                offset += len(step)
            else:
                result.append("%d:%dx%d" % (offset, step[0], step[1]))
        return result


ESC = b"\x1b"
CSI = b"\x1b["


def sgr(*codes):
    return CSI + b";".join(str(code).encode() for code in codes) + b"m"


def text(value):
    return value.encode("utf-8")


def corpus():
    cases = []

    def add(name, data, rows=8, cols=40):
        steps = data if isinstance(data, list) else [data]
        cases.append(Case(name, rows, cols, steps))

    # Attributes: set, combine, remove (tmux restarts with \e[0m), styled underlines.
    for code in (1, 2, 3, 4, 5, 6, 7, 8, 9, 21, 53, "4:0", "4:1", "4:2", "4:3", "4:4", "4:5"):
        add("attr-%s" % code, b"plain " + CSI + str(code).encode() + b"mstyled" + sgr(0) + b" after")
    for name, codes in {
        "bold-dim": (1, 2), "bold-italic-underline": (1, 3, 4), "all-attributes": (1, 2, 3, 4, 5, 7, 8, 9, 53),
        "reverse-strike": (7, 9), "dim-reverse": (2, 7),
    }.items():
        add(name, b"a" + sgr(*codes) + b"bc" + sgr(0) + b"d")
    for remove in (22, 23, 24, 25, 27, 28, 29, 55, 59, 39, 49):
        add("remove-%d" % remove, sgr(1, 2, 3, 4, 5, 7, 8, 9, 53, 31, 42) + sgr(58, 5, 9) + b"on" + sgr(remove) + b"off" + sgr(0))
    add("bold-then-normal-intensity", sgr(1) + b"B" + sgr(2) + b"D" + sgr(22) + b"N")
    add("reset-forms", sgr(1, 31) + b"a" + CSI + b"mb" + sgr(1, 31) + b"c" + CSI + b";m" + b"d" + sgr(1) + b"e" + CSI + b"0;4m" + b"f")
    add("underline-styles-switch", CSI + b"4:3mcurl" + CSI + b"4:2mdbl" + sgr(21) + b"dbl21" + sgr(4) + b"single" + CSI + b"4:0moff")

    # Colours as programs write them; tmux keeps \e[31m apart from \e[38;5;1m.
    for code in list(range(30, 38)) + [39] + list(range(40, 48)) + [49] + list(range(90, 98)) + list(range(100, 108)):
        add("colour-%d" % code, b"x" + sgr(code) + b"colour" + sgr(0) + b"y")
    for index in (0, 1, 7, 8, 9, 15, 16, 100, 114, 231, 232, 246, 255):
        add("fg256-%d" % index, sgr(38, 5, index) + b"fg" + sgr(39) + b"-" + sgr(48, 5, index) + b"bg" + sgr(49) + b"-" + sgr(58, 5, index) + sgr(4) + b"us" + sgr(0))
    for rgb in ((0, 0, 0), (255, 255, 255), (1, 2, 3), (95, 175, 135), (128, 0, 255)):
        add("rgb-%d-%d-%d" % rgb, sgr(38, 2, *rgb) + b"fg" + sgr(48, 2, *rgb) + b"both" + sgr(39) + b"bg" + sgr(0) + sgr(58, 2, *rgb) + sgr(4) + b"u" + sgr(0))
    add("colon-colours", CSI + b"38:5:114mA" + CSI + b"38:2::10:20:30mB" + CSI + b"38:2:40:50:60mC" + CSI + b"48:2:0:1:2:3mD" + CSI + b"58:5:3m" + sgr(4) + b"E" + CSI + b"58:2::9:8:7mF" + sgr(0))
    add("bad-colours", sgr(38, 5, 300) + b"a" + sgr(48, 5) + b"b" + sgr(38, 2, 300, 1, 1) + b"c" + sgr(38, 7, 1) + b"d" + sgr(38) + b"e" + CSI + b"38:5mf" + CSI + b"38:2:1:2mg" + CSI + b"4:9mh" + sgr(0))
    add("colour-runs", b"".join(sgr(31 + i % 7, 41 + (i * 3) % 7) + bytes([65 + i]) for i in range(26)) + sgr(0) + b" end")
    add("same-colour-different-form", sgr(31) + b"a" + sgr(38, 5, 1) + b"b" + sgr(91) + b"c" + sgr(38, 5, 9) + b"d" + sgr(38, 2, 205, 0, 0) + b"e" + sgr(0))

    # State carried from row to row, trailing styled blanks and allocated cells.
    add("carry-fg", sgr(31) + b"red line\r\nstill red" + sgr(0) + b"\r\nplain")
    add("carry-attr-across-blank-row", sgr(1, 4) + b"bold\r\n\r\nafter blank" + sgr(0))
    add("bg-to-end-of-line", sgr(44) + b"blue" + CSI + b"K" + sgr(0) + b"\r\nnext")
    add("bg-erase-chars", b"ab" + sgr(41) + CSI + b"2X" + sgr(0), rows=2, cols=80)
    add("bg-erase-quarter-boundary", b"x" * 19 + sgr(42) + CSI + b"2X" + sgr(0), rows=2, cols=80)
    add("bg-erase-half-boundary", b"x" * 39 + sgr(43) + CSI + b"3X" + sgr(0), rows=2, cols=80)
    add("text-run-across-quarter", CSI + b"1;20H" + b"y" * 21 + CSI + b"1;10H" + sgr(45) + CSI + b"3X" + sgr(0), rows=2, cols=80)
    add("default-cells-after-colour", sgr(41) + b"  " + sgr(0) + b"abc" + CSI + b"3D" + CSI + b"K", rows=2, cols=80)
    add("trailing-spaces-styled", sgr(7) + b"rev   " + sgr(0) + b"   ")
    add("spaces-before-sgr", b"ab   " + sgr(32) + b"   " + sgr(0))

    # Wide and combining characters.
    add("cjk", text("中文 日本語テキスト 한국어\r\nmixed a中b文c"))
    add("cjk-cursor", text("中文xB"))
    add("cjk-wrap-last-column", text("abcdefghi中文"), cols=10)
    add("cjk-fill-exact", text("中文中文中"), cols=10)
    add("cjk-overwrite-left", text("中文") + b"\r" + b"X")
    add("cjk-overwrite-right", text("中文") + CSI + b"1;2H" + b"Y")
    add("cjk-overwrite-wide-shifted", text("中文中") + CSI + b"1;2H" + text("字"))
    add("cjk-coloured-overwrite", sgr(41) + text("中文") + sgr(0) + CSI + b"1;2HZ" + CSI + b"1;4HW")
    add("cjk-with-colours", sgr(31) + text("红") + sgr(32) + text("绿") + sgr(0) + text("白"))
    add("emoji", text("ok 😀 👍🏽 👨\u200d👩\u200d👧 🇺🇸 🇩🇪 ❤️ ✔ ✅ ⚡ ⏺ ❯ › ⎿"))
    add("emoji-wrap", text("12345678😀😀"), cols=10)
    add("combining", text("e\u0301 a\u0308\u0323 n\u0303 \u0301start x\u20dd"))
    add("combining-after-wide", text("中\u0301文\u0308"))
    add("combining-at-wrap", text("abcdefghij\u0301k"), cols=10)
    add("zero-width", text("a\u200bb\u200dc\ufe0fd\u2060e\u3164f"))
    add("vs16-at-last-column", text("abcdefghi\u2764\ufe0f"), cols=10)
    add("box-drawing", text("╭──────╮\r\n│ ❯\u00a0 │\r\n╰──────╯\r\n▐▛███▜▌ ▝▜█▀ ·•…"))
    add("hangul-jamo", text("\u1100\u1161\u11a8 \u1100x\u1161"))

    # Wrapping, line feeds and backspace.
    add("wrap-exact", b"0123456789" + b"abc", cols=10)
    add("wrap-exact-then-crlf", b"0123456789\r\nnext", cols=10)
    add("pending-wrap-moves", b"0123456789" + CSI + b"D!" + b"0123456789" + CSI + b"A?", cols=10)
    add("pending-wrap-cr", b"0123456789\rX", cols=10)
    add("pending-wrap-backspace", b"0123456789\bY", cols=10)
    add("wrap-many", b"".join(bytes([48 + i % 10]) for i in range(57)), rows=4, cols=10)
    add("backspace-over-wrap", b"0123456789ab\b\b\b\bZ", cols=10)
    add("backspace-start", b"\b\b\bA\r\n\bB")
    add("linefeed-scroll", b"".join(b"line %d\r\n" % i for i in range(12)) + b"last", rows=5)
    add("lf-without-cr", b"a\nb\nc\x0bd\x0ce")
    add("nel-ind-ri", b"one" + ESC + b"Etwo" + ESC + b"Dthree" + ESC + b"M" + ESC + b"M" + ESC + b"Mup", rows=4)

    # Scroll regions.
    add("region-scroll", CSI + b"2;4r" + CSI + b"4;1H" + b"a\r\nb\r\nc\r\nd" + CSI + b"r" + CSI + b"6;1Hbelow", rows=6)
    add("region-reverse-index", b"top\r\n" + CSI + b"2;5r" + CSI + b"2;1Hr2\r\nr3\r\nr4" + CSI + b"2;1H" + ESC + b"M" + ESC + b"Mnew", rows=6)
    add("region-scroll-bg", CSI + b"2;3r" + CSI + b"3;1H" + sgr(44) + b"x\n\n" + sgr(0) + CSI + b"r", rows=5)
    add("region-su-sd", b"".join(b"%d\r\n" % i for i in range(5)) + CSI + b"2;4r" + CSI + b"2S" + CSI + b"T" + CSI + b"r", rows=6)
    add("region-origin", CSI + b"3;5r" + CSI + b"?6h" + CSI + b"1;1Hin" + CSI + b"9;3Hlow" + CSI + b"?6l" + CSI + b"1;1Hout", rows=6)
    add("region-invalid", CSI + b"4;2rA" + CSI + b"3;3rB" + CSI + b"0;99rC", rows=5)
    add("region-history", b"".join(b"h%d\r\n" % i for i in range(4)) + CSI + b"2;4r" + CSI + b"4;1H" + b"x\n" * 6 + CSI + b"r", rows=5)

    # Erasing.
    for name, seq in (("el0", b"K"), ("el1", b"1K"), ("el2", b"2K"), ("ed0", b"J"), ("ed1", b"1J"), ("ed2", b"2J"), ("ed3", b"3J")):
        add("erase-" + name, b"aaaaaaaaaa\r\nbbbbbbbbbb\r\ncccccccccc" + CSI + b"2;5H" + CSI + seq + b"X", rows=4, cols=10)
        add("erase-bg-" + name, b"aaaaaaaaaa\r\nbbbbbbbbbb\r\ncccccccccc" + CSI + b"2;5H" + sgr(46) + CSI + seq + sgr(0) + b"X", rows=4, cols=10)
    add("erase-at-pending-wrap", b"0123456789" + CSI + b"K" + sgr(41) + CSI + b"1K" + sgr(0), rows=3, cols=10)
    add("erase-chars", b"0123456789" + CSI + b"1;3H" + CSI + b"4X" + CSI + b"1;9H" + CSI + b"9X", rows=2, cols=10)
    add("clear-home-into-history", b"x\r\ny\r\nz" + CSI + b"H" + CSI + b"J" + b"new", rows=4)
    add("clear-screen-bg", b"abc" + sgr(45) + CSI + b"2J" + sgr(0) + b"z", rows=3, cols=10)
    add("decaln", ESC + b"#8" + CSI + b"2;2H" + CSI + b"K", rows=3, cols=6)

    # Inserting and deleting characters and lines.
    add("ich", b"abcdef" + CSI + b"1;3H" + CSI + b"2@" + b"XY", rows=2, cols=10)
    add("ich-to-end", b"abcdefghij" + CSI + b"1;3H" + CSI + b"99@", rows=2, cols=10)
    add("ich-more-than-moves", b"abcdefghij" + CSI + b"1;3H" + CSI + b"7@", rows=2, cols=10)
    add("ich-bg", b"abcdef" + CSI + b"1;2H" + sgr(42) + CSI + b"3@" + sgr(0), rows=2, cols=10)
    add("dch", b"abcdefghij" + CSI + b"1;3H" + CSI + b"3P" + CSI + b"1;1H" + sgr(43) + CSI + b"P" + sgr(0), rows=2, cols=10)
    add("dch-wide", text("a中文b") + CSI + b"1;2H" + CSI + b"P", rows=2, cols=10)
    add("insert-mode", b"abcdef" + CSI + b"1;3H" + CSI + b"4hXY" + text("中") + CSI + b"4lZ", rows=2, cols=10)
    add("insert-mode-overflow", b"abcdefghij" + CSI + b"1;5H" + CSI + b"4h12345678" + CSI + b"4l", rows=3, cols=10)
    add("il-dl", b"".join(b"line%d\r\n" % i for i in range(5)) + CSI + b"2;1H" + CSI + b"L" + CSI + b"4;1H" + CSI + b"2M", rows=6)
    add("il-too-many", b"".join(b"r%d\r\n" % i for i in range(5)) + CSI + b"3;1H" + CSI + b"9L", rows=6)
    add("il-partial", b"".join(b"r%d\r\n" % i for i in range(9)) + CSI + b"5;1H" + CSI + b"4L", rows=10)
    add("dl-too-many", b"".join(b"r%d\r\n" % i for i in range(5)) + CSI + b"3;1H" + CSI + b"9M", rows=6)
    add("il-dl-region", b"".join(b"r%d\r\n" % i for i in range(6)) + CSI + b"2;5r" + CSI + b"3;1H" + CSI + b"L" + CSI + b"2M" + sgr(41) + CSI + b"9L" + sgr(0) + CSI + b"r", rows=7)
    add("il-outside-region", b"".join(b"r%d\r\n" % i for i in range(6)) + CSI + b"2;3r" + CSI + b"5;1H" + CSI + b"L" + CSI + b"1;1H" + CSI + b"M" + CSI + b"r", rows=7)

    # Cursor movement.
    add("cup-and-friends", CSI + b"3;5Ha" + CSI + b"10Bb" + CSI + b"99Cc" + CSI + b"99Dd" + CSI + b"99Ae" + CSI + b"2Ef" + CSI + b"Fg" + CSI + b"20Gh" + CSI + b"3`i" + CSI + b"4dj" + CSI + b"2;2fk", rows=6, cols=20)
    add("cup-zero-and-huge", CSI + b"0;0Ha" + CSI + b"999;999Hb" + CSI + b";5Hc" + CSI + b"3Hd", rows=5, cols=20)
    add("save-restore", sgr(31) + b"red" + ESC + b"7" + CSI + b"3;3H" + sgr(0) + b"plain" + ESC + b"8" + b"again" + CSI + b"s" + CSI + b"5;1H" + CSI + b"u" + b"!", rows=6, cols=20)
    add("restore-without-save", CSI + b"3;3H" + sgr(32) + b"x" + ESC + b"8" + b"y", rows=5, cols=20)
    add("save-restore-charset", ESC + b"(0" + ESC + b"7" + ESC + b"(B" + b"q" + ESC + b"8" + b"q", rows=3, cols=10)

    # Tabs.
    add("tab-basic", b"a\tb\tc\r\n\tx\t\ty", cols=40)
    add("tab-over-text", b"0123456789abcdef\r\tX", cols=40)
    add("tab-coloured-cells", sgr(44) + CSI + b"K" + sgr(0) + b"a\tb", cols=40)
    add("tab-pen-colour", sgr(41) + b"a\tb" + sgr(0), cols=40)
    add("tab-last-column", b"0123456789abcdefghijklmnopqrstuvwxyz012\tX\t", cols=40)
    add("tab-stops", ESC + b"H" + CSI + b"1;4H" + ESC + b"H" + b"\r\ta\tb" + CSI + b"3g\tc" + CSI + b"g", cols=40)
    add("tab-clear-all", CSI + b"3ga\tb", cols=40)
    add("tab-back", b"0123456789012345678901234" + CSI + b"Z!" + CSI + b"2Z?" + CSI + b"9Z#", cols=40)
    add("tab-overwritten", b"a\tb\r\nc\t\r\nd\t" + CSI + b"2;3HX" + CSI + b"3;2H" + text("中"), cols=40)
    add("tab-wide-content", text("中\t文\tx"), cols=40)

    # Alternate screen and modes.
    add("alt-1049", b"main text" + CSI + b"?1049h" + b"alt text" + CSI + b"?1049l" + b"!")
    add("alt-1049-active", sgr(33) + b"main" + CSI + b"?1049h" + sgr(0) + CSI + b"2;2Halt" + sgr(41))
    add("alt-1049-restores-pen", sgr(35) + b"m" + CSI + b"?1049h" + sgr(0) + b"a" + CSI + b"?1049l" + b"after")
    add("alt-47", b"main" + CSI + b"?47h" + b"alt" + CSI + b"?47l" + b"back")
    add("alt-1047", b"main" + CSI + b"?1047hx" + CSI + b"?1047l" + b"y")
    add("alt-nested", b"m" + CSI + b"?1049h" + b"a" + CSI + b"?1049h" + b"b" + CSI + b"?1049l" + b"c")
    add("alt-exit-without-enter", CSI + b"3;3Hx" + CSI + b"?1049l" + b"y" + CSI + b"?1049h" + CSI + b"?1049l" + CSI + b"?1049l" + b"z")
    add("alt-scroll-no-history", CSI + b"?1049h" + b"".join(b"a%d\r\n" % i for i in range(10)) + b"end", rows=4)
    add("modes-on", CSI + b"?2004h" + CSI + b"?1h" + ESC + b"=" + CSI + b"?1000h" + CSI + b"?1006h" + CSI + b"?1004h" + CSI + b"?25l" + CSI + b"4h" + CSI + b"5 q")
    add("modes-off", CSI + b"?2004h" + CSI + b"?2004l" + CSI + b"?1h" + CSI + b"?1l" + ESC + b"=" + ESC + b">" + CSI + b"?1002h" + CSI + b"?1003h" + CSI + b"?1005h" + CSI + b"?1003l" + CSI + b"?25l" + CSI + b"?25h")
    add("mouse-button", CSI + b"?1002h" + CSI + b"?1006h")
    add("mouse-any-utf8", CSI + b"?1003h" + CSI + b"?1005h")
    add("mouse-1001", CSI + b"?1001h" + CSI + b"?9h")
    add("cursor-styles", CSI + b"2 q" + b"a" + CSI + b"?12h" + CSI + b"6 q")
    add("cursor-blink-modes", CSI + b"?12h" + CSI + b"?12l")
    add("cursor-style-default", CSI + b"3 q" + CSI + b"0 q")
    add("multiple-private-modes", CSI + b"?1;2004;25;1049h" + b"x" + CSI + b"?1049;25l")
    add("nowrap", CSI + b"?7l" + b"0123456789abc" + text("中文") + CSI + b"?7h" + b"\r\nwrap", rows=3, cols=10)
    add("nowrap-wide", CSI + b"?7l" + b"012345678" + text("中") + CSI + b"1;9H" + text("文"), rows=3, cols=10)
    add("ris", CSI + b"?2004h" + sgr(31) + b"before" + CSI + b"2;3r" + ESC + b"c" + b"after")
    add("ris-in-alt", b"main" + CSI + b"?1049h" + b"alt" + ESC + b"c" + b"x")
    add("deccolm", b"abc" + sgr(44) + CSI + b"?3h" + sgr(0) + b"x", rows=3, cols=10)
    add("sync-pair", CSI + b"?2026h" + b"frame" + CSI + b"?2026l")

    # Character sets, controls and strings that do not print.
    add("acs", ESC + b"(0lqqk\r\nx  x\r\nmqqj" + ESC + b"(Bplain", rows=4, cols=10)
    add("acs-shift", ESC + b")0" + b"a\x0eqq\x0fb\x0eq", rows=2, cols=10)
    add("acs-coloured", sgr(32) + ESC + b"(0qq" + sgr(1) + b"q" + ESC + b"(B" + b"t" + sgr(0), rows=2, cols=10)
    add("osc-titles", b"a" + ESC + b"]0;title\x07b" + ESC + b"]2;other" + ESC + b"\\c" + ESC + b"]1;icon\x07d")
    add("osc-8-links", b"see " + ESC + b"]8;;https://example.com\x1b\\link" + ESC + b"]8;;\x1b\\ done\r\n" + ESC + b"]8;id=x;https://example.org\x07" + sgr(4) + b"two" + sgr(0) + ESC + b"]8;;\x07 end")
    add("osc-8-trailing-spaces", ESC + b"]8;;https://example.com\x1b\\linked   " + ESC + b"]8;;\x1b\\" + b"\r\n" + ESC + b"]8;;https://example.com\x1b\\x  ")
    add("osc-other", b"a" + ESC + b"]7;file://host/tmp\x07" + ESC + b"]52;c;aGVsbG8=\x07" + ESC + b"]133;A\x07" + ESC + b"]4;1;rgb:ff/00/00\x07" + ESC + b"]9999;x\x07b")
    add("dcs-apc-pm-sos", b"a" + ESC + b"P$q m" + ESC + b"\\b" + ESC + b"Ptmux;" + ESC + ESC + b"[31mx" + ESC + b"\\c" + ESC + b"_apc" + ESC + b"\\d" + ESC + b"^pm" + ESC + b"\\e" + ESC + b"Xsos" + ESC + b"\\f")
    add("rename", b"a" + ESC + b"kwindow" + ESC + b"\\b")
    add("queries", b"a" + CSI + b"c" + CSI + b">c" + CSI + b"6n" + CSI + b"5n" + CSI + b"?2004$p" + CSI + b"18t" + CSI + b">0q" + CSI + b"?u" + b"b")
    add("query-reports", CSI + b"?2004h" + CSI + b"4h" + CSI + b"3;5H" + CSI + b"6n" + CSI + b"0c" + CSI + b"1c" + CSI + b"4$p" + CSI + b"7$p"
        + b"".join(CSI + b"?%d$p" % mode for mode in (1, 3, 6, 7, 12, 25, 47, 1000, 1002, 1003, 1004, 1005, 1006, 1049, 2004, 2026, 2031, 9999))
        + CSI + b"19t" + CSI + b"14t" + CSI + b"22;0;18t" + ESC + b"P$q q" + ESC + b"\\" + CSI + b"3 q" + ESC + b"P$q q" + ESC + b"\\"
        + ESC + b"P$qm" + ESC + b"\\" + CSI + b"?1049h" + CSI + b"?1049$p" + b"0123456789" + CSI + b"6n", rows=5, cols=10)
    add("controls", b"a\x00b\x07c\x01d\x7fe\x1cf")
    add("can-sub", CSI + b"31\x18m" + b"x" + CSI + b"1;\x1a" + b"y" + ESC + b"\x18z")
    add("esc-inside-csi", CSI + b"31" + ESC + b"[32mgreen" + sgr(0))
    add("c0-inside-csi", CSI + b"2\r;3Hx" + CSI + b"\x085Cy", rows=4, cols=10)
    add("unknown-sequences", b"a" + CSI + b"?5W" + CSI + b"=1c" + CSI + b"1;2;3z" + ESC + b"(A" + ESC + b"#3" + ESC + b"Zb" + CSI + b"?J" + CSI + b"?K" + b"c" + CSI + b"1I" + CSI + b"2ad" + CSI + b"2ee")
    add("too-many-params", CSI + b";".join(b"1" for _ in range(30)) + b"mx" + sgr(0) + CSI + b"99999999999mY" + CSI + b"1:2:3:4:5:6:7:8:9mZ")
    add("long-params", CSI + b"0" * 70 + b"1mx" + CSI + b"1" + b"$" * 4 + b"my")
    add("csi-ignore", CSI + b"1<2mx" + CSI + b"1 =my")

    # Invalid UTF-8 and C1 controls.
    add("utf8-invalid", b"a\x80b\xc3c\xe4\xb8d\xff\xfe" + b"e\xc0\xafg\xed\xa0\x80h\xf4\x90\x80\x80i\xe4\xc3\xa9j")
    add("utf8-c1", b"x\xc2\x85y\xc2\x9bz")
    add("utf8-split-by-escape", b"\xe4" + sgr(31) + b"\xb8\xad" + sgr(0) + b"!")
    add("utf8-truncated-end", b"ok\xe4\xb8")

    # REP.
    add("rep", b"a" + CSI + b"3b" + text("中") + CSI + b"2b" + sgr(31) + b"x" + CSI + b"b" + CSI + b"b" + CSI + b"99b", cols=20)
    add("rep-after-control", b"a\r" + CSI + b"3b" + b"b" + ESC + b"]2;t\x07" + CSI + b"2b", cols=20)

    # History, clears and resizing (reflow on the main screen).
    add("history-then-grow", [b"".join(b"line %02d\r\n" % i for i in range(15)) + b"tail", (10, 40)], rows=5)
    add("history-then-shrink", [b"".join(b"line %02d\r\n" % i for i in range(6)) + b"tail" + CSI + b"2;1H", (3, 40)], rows=8)
    add("shrink-cursor-at-bottom", [b"".join(b"r%d\r\n" % i for i in range(7)) + b"r7", (4, 40), b"!"], rows=8)
    add("reflow-narrow", [b"0123456789abcdefghij0123456789\r\nshort\r\n" + text("中文中文中文中文"), (8, 7), b"+"], rows=8, cols=30)
    add("reflow-wide", [b"0123456789abcdefghij0123456789\r\nshort", (8, 50), b"+"], rows=8, cols=12)
    add("reflow-wide-chars", [text("中文中文"), (3, 9), b"x"], rows=3, cols=10)
    add("reflow-round-trip", [b"".join(b"%02d-abcdefghijklmnop\r\n" % i for i in range(5)), (6, 8), (6, 30), b"end"], rows=6, cols=30)
    add("reflow-styled", [sgr(41) + b"0123456789" * 3 + sgr(0) + CSI + b"K\r\n" + sgr(32) + b"tail", (6, 7), (6, 20)], rows=6, cols=20)
    add("resize-in-alt", [b"main line one\r\nmain two" + CSI + b"?1049h" + b"alt content here", (4, 8), b"+" + CSI + b"?1049l" + b"!"], rows=6, cols=20)
    add("resize-pending-wrap", [b"0123456789", (5, 12), b"x"], rows=5, cols=10)
    add("clear-then-grow", [b"".join(b"%d\r\n" % i for i in range(8)) + CSI + b"2J" + CSI + b"H" + b"top", (10, 20)], rows=5, cols=20)
    add("clear-history-then-grow", [b"".join(b"%d\r\n" % i for i in range(8)) + CSI + b"3J" + b"x", (10, 20)], rows=5, cols=20)

    # Agent-like frames.
    nbsp = "\u00a0"
    add("claude-like-frame", sgr(38, 5, 246) + text("╭" + "─" * 30 + "╮") + sgr(0) + b"\r\n" + sgr(38, 5, 246) + text("│") + sgr(0) + text(" ❯" + nbsp) + sgr(2) + b"Try \"refactor\"" + sgr(22) + CSI + b"34G" + sgr(38, 5, 246) + text("│") + sgr(0) + b"\r\n" + sgr(38, 5, 246) + text("╰" + "─" * 30 + "╯") + sgr(0) + b"\r\n" + sgr(38, 5, 114) + text("✔") + sgr(39) + b" done " + sgr(7) + b" Type something " + sgr(27) + CSI + b"2;5H", rows=6, cols=40)
    add("codex-like-frame", CSI + b"?2026h" + CSI + b"1;1H" + sgr(1) + text("›") + sgr(22) + b" " + sgr(2) + b"Ask Codex" + sgr(22) + CSI + b"K\r\n" + sgr(48, 2, 40, 40, 40) + text("› ") + sgr(38, 5, 8) + b"Implement {feature}" + sgr(39) + CSI + b"K" + sgr(49) + b"\r\n" + sgr(38, 5, 9) + b"error" + sgr(39) + b" " + sgr(38, 5, 2) + b"ok" + sgr(39) + CSI + b"K\r\n" + sgr(2) + b"  ? for shortcuts" + sgr(22) + CSI + b"2;3H" + CSI + b"?2026l", rows=6, cols=40)
    add("kimi-like-frame", text("╭─────────╮\r\n│ > ") + sgr(2) + b"msg" + sgr(22) + text("   │\r\n╰─────────╯") + CSI + b"2;6H", rows=4, cols=20)
    add("spinner-redraw", b"".join(b"\r" + CSI + b"K" + sgr(38, 5, 174) + text(frame) + sgr(39) + b" Working (%ds)" % i for i, frame in enumerate("✻✽✶✳✢·")) + b"\r\n" + CSI + b"A" + CSI + b"2K" + b"done", rows=3, cols=40)
    add("inline-viewport-scroll", b"".join(b"hist %d\r\n" % i for i in range(3)) + CSI + b"4;6r" + CSI + b"6;1H" + b"\n" + CSI + b"5;1Hinserted" + CSI + b"r" + CSI + b"1;1H" + CSI + b"2M", rows=6, cols=20)
    frames = []
    for frame in range(40):
        frames.append(CSI + b"%dA" % 4 if frame else b"")
        for line in range(4):
            frames.append(b"\r" + CSI + b"2K" + sgr(38, 5, 246 if line % 2 else 114) + text("⏺ step %d.%d " % (frame, line)) + sgr(2) + b"x" * (frame % 23) + sgr(0) + (b"\r\n" if line < 3 else b""))
    add("redraw-frames-long", b"".join(frames), rows=8, cols=40)

    # Width corrections to glibc and tmux 3.7's overwritten tab.
    add("glibc-widths", text("a­b क्षि ಕೀ 〮 xﾞ y"))
    add("tab-head-overwritten-utf8", b"\x1b[44m\x1b[K\x1b[0m\t\r\xc3\xa9", rows=2, cols=20)
    add("alt-resize-and-back", [b"main line that wraps around\r\nnext" + CSI + b"?1049h" + b"alt", (5, 10), b"+", (5, 30), CSI + b"?1049l" + b"!"], rows=5, cols=20)

    # Recorded screens, rendered three ways.
    for path in sorted(FIXTURES.glob("*.txt")):
        lines = path.read_text(encoding="utf-8").split("\n")
        while lines and lines[-1] == "":
            lines.pop()
        cols = max(40, max(len(line) for line in lines))
        rows = max(10, len(lines))
        stem = path.stem
        add("fixture-%s" % stem, b"\r\n".join(text(line) for line in lines), rows=rows, cols=cols)
        styles = [sgr(38, 5, 114), sgr(2), sgr(38, 5, 246), sgr(7), sgr(48, 2, 30, 30, 30), sgr(1, 3), sgr(0), sgr(36, 4)]
        styled = b"\r\n".join(styles[i % len(styles)] + text(line) for i, line in enumerate(lines)) + sgr(0)
        add("fixture-styled-%s" % stem, styled, rows=rows, cols=cols)
        positioned = CSI + b"2J" + b"".join(CSI + b"%d;1H" % (i + 1) + text(line) + CSI + b"K" for i, line in reversed(list(enumerate(lines))))
        add("fixture-positioned-%s" % stem, positioned, rows=rows, cols=cols)
    for path in sorted(RECORDED.glob("*.bin")) if RECORDED.is_dir() else []:
        match = re.fullmatch(r"(.+)\.([0-9]+)x([0-9]+)\.bin", path.name)
        if not match:
            raise AssertionError("name recorded streams NAME.COLSxROWS.bin: %s" % path.name)
        add("recorded-" + match[1], path.read_bytes(), rows=int(match[3]), cols=int(match[2]))
    first = int(os.environ.get("SCREEN_RANDOM_FIRST", "0"))
    cases += [fuzz_case(index) for index in range(first, first + RANDOM_STREAMS)]
    return cases


class Random:
    """A small deterministic generator (xorshift64*), the same on every Python."""

    def __init__(self, seed):
        self.state = (seed * 0x9E3779B97F4A7C15 + 0x2545F4914F6CDD1D) % (1 << 64) or 1

    def below(self, n):
        x = self.state
        x ^= x >> 12
        x ^= (x << 25) % (1 << 64)
        x ^= x >> 27
        self.state = x
        return ((x * 0x2545F4914F6CDD1D) % (1 << 64) >> 32) % n

    def pick(self, values):
        return values[self.below(len(values))]

    def chance(self, percent):
        return self.below(100) < percent


WIDE = ["中", "文", "字", "😀", "👍", "한", "テ", "🇺🇸", "✅", "⚡"]
NARROW = ["é", "ß", "─", "│", "❯", "›", "✔", "⏺", "·", "…", "\u00a0"]
JOINERS = ["\u0301", "\u0308", "\u20dd", "\u200d", "\ufe0f", "\U0001f3fd"]


def fuzz_sgr(rnd):
    codes = []
    for _ in range(1 + rnd.below(3)):
        kind = rnd.below(9)
        if kind == 0:
            codes.append(rnd.pick([0, 1, 2, 3, 4, 5, 7, 8, 9, 21, 22, 23, 24, 25, 27, 28, 29, 53, 55, 59]))
        elif kind == 1:
            codes.append(rnd.pick([30, 31, 32, 37, 39, 90, 97]))
        elif kind == 2:
            codes.append(rnd.pick([40, 41, 44, 47, 49, 100, 107]))
        elif kind == 3:
            codes += [rnd.pick([38, 48, 58]), 5, rnd.below(256)]
        elif kind == 4:
            codes += [rnd.pick([38, 48, 58]), 2, rnd.below(256), rnd.below(256), rnd.below(256)]
        elif kind == 5:
            codes.append(rnd.pick(["4:3", "4:2", "4:0", "38:5:%d" % rnd.below(256), "48:2::1:2:3"]))
        else:
            codes.append(rnd.pick([0, 1, 2, 7, 31, 32, 44, 39, 49]))
    return CSI + b";".join(str(code).encode() for code in codes) + b"m"


def fuzz_token(rnd, rows, cols, first):
    kind = rnd.below(100)
    if kind < 30:
        word = "".join(rnd.pick("abcdefghijklmnopqrstuvwxyz0123456789 .,-_") for _ in range(1 + rnd.below(12)))
        return word.encode()
    if kind < 36:
        return text("".join(rnd.pick(WIDE) for _ in range(1 + rnd.below(3))))
    if kind < 40:
        return text(rnd.pick(NARROW) + (rnd.pick(JOINERS) if rnd.chance(50) else ""))
    if kind < 52:
        return fuzz_sgr(rnd)
    if kind < 62:
        return rnd.pick([
            CSI + b"%d;%dH" % (rnd.below(rows + 2), rnd.below(cols + 2)),
            CSI + b"%dA" % rnd.below(4), CSI + b"%dB" % rnd.below(4),
            CSI + b"%dC" % rnd.below(cols), CSI + b"%dD" % rnd.below(cols),
            b"\r", b"\n", b"\r\n", b"\b", b"\t", CSI + b"%dG" % rnd.below(cols + 2),
            CSI + b"%dd" % rnd.below(rows + 2), CSI + b"E", CSI + b"F", ESC + b"E", ESC + b"M", ESC + b"D",
        ])
    if kind < 72:
        return rnd.pick([
            CSI + b"K", CSI + b"1K", CSI + b"2K", CSI + b"J", CSI + b"1J", CSI + b"2J",
            CSI + b"%dX" % rnd.below(cols), CSI + b"%d@" % rnd.below(cols), CSI + b"%dP" % rnd.below(cols),
            CSI + b"%dL" % rnd.below(rows), CSI + b"%dM" % rnd.below(rows), CSI + b"%dS" % rnd.below(rows),
            CSI + b"%dT" % rnd.below(rows),
        ])
    if kind < 76:
        top = 1 + rnd.below(rows)
        return CSI + b"%d;%dr" % (top, top + rnd.below(rows)) if rnd.chance(70) else CSI + b"r"
    if kind < 80:
        return rnd.pick([ESC + b"7", ESC + b"8", CSI + b"s", CSI + b"u", ESC + b"H", CSI + b"g", CSI + b"3g", CSI + b"Z"])
    if kind < 83 and not first:
        return CSI + b"%db" % rnd.below(5)
    if kind < 86:
        return rnd.pick([CSI + b"4h", CSI + b"4l", CSI + b"?7l", CSI + b"?7h", CSI + b"?6h", CSI + b"?6l",
                         CSI + b"?2004h", CSI + b"?1h", CSI + b"?1000h", CSI + b"?25l"])
    if kind < 89:
        return rnd.pick([CSI + b"?1049h", CSI + b"?1049l", CSI + b"?47h", CSI + b"?47l"])
    if kind < 92:
        return rnd.pick([ESC + b"(0", ESC + b"(B", b"\x0e", b"\x0f", ESC + b")0"])
    if kind < 95:
        return rnd.pick([ESC + b"]2;t\x07", ESC + b"]8;;https://example.com\x1b\\", ESC + b"]8;;\x07"])
    if kind < 97:
        return rnd.pick([CSI + b"?2026h", CSI + b"?2026l", ESC + b"#8", CSI + b"3J"])
    return "".join(rnd.pick("abc ") for _ in range(cols + rnd.below(cols))).encode()


def fuzz_case(index):
    rnd = Random(index)
    rows, cols = 2 + rnd.below(10), 3 + rnd.below(38)
    steps, segment, first = [], [], True
    for _ in range(40 + rnd.below(120)):
        if rnd.chance(2) and segment:
            steps.append(b"".join(segment))
            steps.append((2 + rnd.below(10), 3 + rnd.below(38)))
            segment, first = [], True
            continue
        segment.append(fuzz_token(rnd, rows, cols, first))
        first = False
    steps.append(b"".join(segment))
    return Case("random-%d" % index, rows, cols, steps)


# More random streams, or others, for exploring: SCREEN_RANDOM_STREAMS, SCREEN_RANDOM_FIRST.
RANDOM_STREAMS = int(os.environ.get("SCREEN_RANDOM_STREAMS", "160"))


def host_replies(replies):
    """tmux's answers without those the model leaves to the host: the version,
    pixel sizes, sixel graphics; a tmux built with sixel adds it to DA."""
    replies = replies.replace(b"\x1b[?1;2;4c", b"\x1b[?1;2c")
    replies = re.sub(rb"\x1bP>\|[^\x1b]*\x1b\\", b"", replies)
    replies = re.sub(rb"\x1b\[[456];[0-9]+;[0-9]+t", b"", replies)
    return re.sub(rb"\x1b\[\?[0-9]+;[0-9]+;[0-9]+S", b"", replies)


def comparable(result, original, key):
    """What a repaint can reproduce on a terminal: a cursor that a resize left
    beyond the last column waits to wrap at it, and a cursor outside the
    scroll region in origin mode cannot be addressed."""
    if key != "info":
        return result[key]
    fields = result["info"].split(" ")
    source = original["info"].split(" ")
    fields[0] = str(min(int(fields[0]), int(fields[2])))
    if source[10] == "1" and not int(source[18]) <= int(source[1]) <= int(source[19]):
        fields[0] = fields[1] = "outside-region"
    return " ".join(fields)


@unittest.skipUnless(TMUX, "tmux is required for the differential screen test")
class ScreenModelDifferential(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        version, text_version = tmux_version()
        if version < (3, 7):
            raise AssertionError("The screen model differential test requires tmux 3.7+: " + text_version)
        cls.compat = "3.7" if version == (3, 7) else "3.8"
        if not HGS.exists():
            raise unittest.SkipTest("build hgs first (cargo build)")
        cls.temp = tempfile.TemporaryDirectory(prefix="hgs-screen-")
        cls.root = Path(cls.temp.name)
        cls.socket = cls.root / "socket"
        cls.env = dict(os.environ, TMUX_TMPDIR=str(cls.root))
        cls.env.pop("TMUX", None)
        cls.env.pop("TMUX_PANE", None)
        (cls.root / "feeder.py").write_text(FEEDER)
        cls.tmux("new-session", "-d", "-s", "keeper", "-x", "20", "-y", "5", "sleep 100000")
        cls.tmux("set-option", "-g", "status", "off")
        cls.serial = 0
        cls.cases = corpus()

    @classmethod
    def tearDownClass(cls):
        subprocess.run([TMUX, "-S", str(cls.socket), "kill-server"], env=cls.env, capture_output=True)
        cls.temp.cleanup()

    @classmethod
    def tmux_bytes(cls, *args):
        result = subprocess.run([TMUX, "-S", str(cls.socket), "-f", "/dev/null", *args], env=cls.env,
                                capture_output=True, timeout=20)
        if result.returncode != 0:
            raise AssertionError("tmux %s failed: %s" % (args[0], result.stderr.decode(errors="replace")))
        return result.stdout

    @classmethod
    def tmux(cls, *args):
        return cls.tmux_bytes(*args).decode("utf-8", errors="replace")

    def model(self, case):
        command = [str(HGS), "__screen-capture", "--rows", str(case.rows), "--cols", str(case.cols),
                   "--tmux", self.compat, "--json"]
        for resize in case.resizes():
            command += ["--resize-after", resize]
        result = subprocess.run(command, input=case.stream(), capture_output=True, timeout=20)
        self.assertEqual(result.returncode, 0, result.stderr.decode(errors="replace"))
        value = json.loads(result.stdout)
        for key in ("plain", "styled", "styled_links", "repaint", "replies"):
            value[key] = base64.b64decode(value[key])
        return value

    def run_in_tmux(self, cases):
        """Feed several cases into their own panes at once; return their captures."""
        names = {}
        for case in cases:
            type(self).serial += 1
            name = "s%d" % self.serial
            names[case.name] = name
            self.tmux("new-session", "-d", "-s", name, "-x", str(case.cols), "-y", str(case.rows),
                      "python3 %s %s %s" % (self.root / "feeder.py", self.root, name))
            self.tmux("pipe-pane", "-O", "-t", "=%s:" % name, "cat >> %s" % (self.root / (name + ".pipe")))
        pending = {case.name: [0, list(case.steps)] for case in cases}
        deadline = time.monotonic() + 60
        while any(steps for _, steps in pending.values()):
            for case in cases:
                name = names[case.name]
                segment, steps = pending[case.name]
                while steps and isinstance(steps[0], tuple):
                    rows, cols = steps.pop(0)
                    self.tmux("resize-window", "-t", "=%s:" % name, "-x", str(cols), "-y", str(rows))
                if not steps:
                    continue
                path = self.root / ("%s.%d" % (name, segment))
                if not path.exists():
                    path.write_bytes(steps[0])
                    (self.root / ("%s.%d.go" % (name, segment))).touch()
            titles = dict(line.split(" ", 1) for line in self.tmux(
                "list-panes", "-a", "-F", "#{session_name} #{pane_title}").splitlines() if " " in line)
            for case in cases:
                name = names[case.name]
                segment, steps = pending[case.name]
                if steps and not isinstance(steps[0], tuple) and titles.get(name) == "%s-done-%d" % (name, segment):
                    steps.pop(0)
                    pending[case.name][0] += 1
            if time.monotonic() > deadline:
                raise AssertionError("tmux did not process the streams in time")
            time.sleep(0.005)
        results = {}
        for case in cases:
            target = "=%s:" % names[case.name]
            results[case.name] = {
                "plain": self.tmux_bytes("capture-pane", "-p", "-t", target),
                "styled": self.tmux_bytes("capture-pane", "-p", "-e", "-t", target),
                "info": self.tmux("display-message", "-p", "-t", target, INFO_FORMAT).rstrip("\n"),
                "replies": host_replies((self.root / (names[case.name] + ".replies")).read_bytes()),
            }
            self.tmux("kill-session", "-t", "=%s" % names[case.name])
        return results

    def compare(self, cases, label):
        failures = []
        for start in range(0, len(cases), 16):
            batch = cases[start:start + 16]
            actual = self.run_in_tmux(batch)
            for case in batch:
                model = self.model(case)
                # tmux prints OSC 8 hyperlinks; the model's public capture omits
                # them, so streams with links compare the model's tmux form.
                if b"\x1b]8;" in case.stream():
                    model["styled"] = model["styled_links"]
                for key in ("plain", "styled", "info", "replies"):
                    if model[key] != actual[case.name][key]:
                        failures.append("%s %s %s (%dx%d, steps %r):\n  tmux:  %r\n  model: %r" % (
                            label, case.name, key, case.rows, case.cols, case.steps,
                            actual[case.name][key], model[key]))
        return failures

    def character_widths(self, cases):
        """Characters whose width the installed tmux sees differently.

        tmux asks the C library (glibc on Linux) or utf8proc (macOS) for
        widths; the model follows glibc. Each character is drawn on its own
        row followed by a marker, so a different width shows in the capture.
        """
        chars = sorted({c for case in cases for c in case.stream().decode("utf-8", "ignore") if ord(c) > 0x7f})
        if not chars:
            return set()
        stream = b"".join(CSI + b"%d;1H" % (row + 1) + text(c) + CSI + b"%d;4H|" % (row + 1)
                          for row, c in enumerate(chars))
        case = Case("widths", len(chars), 8, [stream])
        actual = self.run_in_tmux([case])[case.name]["plain"].split(b"\n")
        model = self.model(case)["plain"].split(b"\n")
        return {c for row, c in enumerate(chars) if actual[row] != model[row]}

    def test_streams_and_repaints_match_tmux(self):
        cases = self.cases
        different = self.character_widths(cases)
        if different and sys.platform.startswith("linux"):
            self.fail("tmux and the model give these characters different widths: %r" % sorted(different))
        if different:
            # Another width table (utf8proc): compare the streams that do not
            # depend on it.
            cases = [case for case in cases
                     if not set(case.stream().decode("utf-8", "ignore")) & different]
            print("screen model: %d characters have other widths here; their streams are skipped: %r" % (
                len(different), sorted(different)))
        failures = self.compare(cases, "stream")
        repaints = []
        for case in cases:
            model = self.model(case)
            repaint = model["repaint"]
            info = model["info"].split(" ")
            rows, cols = int(info[3]), int(info[2])
            repaint_case = Case("repaint-" + case.name, rows, cols, [repaint])
            repainted = self.model(repaint_case)
            for key in ("cells", "info"):
                if comparable(repainted, model, key) != comparable(model, model, key):
                    failures.append("repaint %s %s:\n  original:  %r\n  repainted: %r" % (
                        case.name, key, model[key], repainted[key]))
            repaints.append(repaint_case)
        failures += self.compare(repaints, "repaint stream")
        print("screen model: %d streams and %d repaints compared with %s" % (
            len(cases), len(repaints), tmux_version()[1]))
        self.assertFalse(failures, "\n".join(failures[:40]) + "\n%d mismatches" % len(failures))


if __name__ == "__main__":
    unittest.main()
