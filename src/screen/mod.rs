//! A terminal screen model that answers what `tmux capture-pane` answers.
//!
//! The Windows session host has no tmux, yet every screen parser in `state`
//! reads tmux's capture format: `capture-pane -p` (plain) and `-p -e` (with
//! SGR escapes). This module keeps the screen of a pane from the bytes the
//! program writes and serializes it exactly as tmux 3.7 and later print it:
//!
//! - the visible screen only, every row including trailing blank rows, each
//!   followed by a newline, with trailing spaces trimmed;
//! - one character per wide glyph (the cursor still counts cells), combining
//!   characters kept with their base, and a tab that was written over blank
//!   cells printed as one `\t`;
//! - SGR sequences only, with tmux's state carried from row to row: `\e[0m`
//!   before re-adding attributes when any attribute is removed, colours as
//!   `30`-`37`/`90`-`97`, `38;5;N` or `38;2;R;G;B` exactly as the program chose
//!   them, `39`/`49` for defaults, styled underlines as `4:N`, and overline as
//!   tmux writes it (`5:3`); cells in the alternate character set are wrapped
//!   in SO/SI;
//! - a styled cell followed by blank cells tmux has allocated prints the
//!   colour reset tmux prints, because lines grow in tmux's quarter, half and
//!   full width steps.
//!
//! The model follows tmux's own algorithms (input.c, screen-write.c, grid.c,
//! screen.c) rather than a generic terminal emulator, because byte-exact
//! output depends on tmux's storage details. `vt100` 0.16 cannot represent
//! them: it stores `\e[31m` and `\e[38;5;1m` alike, has no strikethrough,
//! blink, hidden, overline or underline styles, cannot hold bold and dim
//! together, has no tab cells and does not track allocated line lengths.
//!
//! tmux's own quirks are reproduced too: overline prints as `5:3`, inserting
//! characters clears only the columns whose contents moved (so inserting as
//! many as remain changes nothing), a padding cell counts as a column when
//! lines reflow, and a tab written over blank cells keeps their style.
//!
//! Differences from tmux, by design:
//!
//! - OSC 8 hyperlinks are tracked but not printed. tmux prints them as
//!   `\e]8;;URI\e\\` in `-e` output and every consumer only strips them;
//!   tmux also repeats the last cell's codes before closing a link at the end
//!   of a row, which stripping leaves behind, and keeps that row's trailing
//!   spaces. The differential test compares tmux's form exactly.
//! - Queries are answered as tmux answers them ([`ScreenModel::take_replies`]),
//!   except those that depend on the terminal outside: colours, pixel sizes,
//!   the terminal version and theme. Device attributes are those of a tmux
//!   built without sixel support.
//! - Character widths are those of glibc's `wcwidth` (as of 2.44, through
//!   `unicode-width` with corrections) plus tmux's override table. tmux on
//!   macOS uses utf8proc, which differs for some emoji and symbols.
//! - Like tmux, output is processed in the chunks it arrives in: a run of
//!   ASCII text that a chunk boundary splits can allocate a line differently,
//!   so the same bytes can capture differently in tmux too.
//!
//! `Compat::Tmux37` reproduces tmux 3.7 where 3.8 changed behaviour: when a
//! write breaks up a wide character or a tab, 3.7 resets the remaining
//! columns (and turns a broken tab's columns into one-column tabs) where 3.8
//! keeps their background; 3.8 also keeps the cursor still when a joined
//! character ends beyond the right edge.
//!
//! [`ScreenModel::repaint`] draws what a terminal can show. It cannot place a
//! cursor that a resize left beyond the last column (it waits to wrap at the
//! last column instead) or one outside the scroll region in origin mode (it
//! goes to the region's top), and wide characters that a resize cut at the
//! right edge show as blanks.

mod grid;
mod input;
mod reflow;
mod utf8;
mod write;

use grid::{
    Cell, Line, ATTR_BLINK, ATTR_BRIGHT, ATTR_CHARSET, ATTR_DIM, ATTR_HIDDEN, ATTR_ITALICS,
    ATTR_OVERLINE, ATTR_REVERSE, ATTR_STRIKETHROUGH, ATTR_UNDERSCORE, ATTR_UNDERSCORE_2,
    ATTR_UNDERSCORE_3, ATTR_UNDERSCORE_4, ATTR_UNDERSCORE_5, COLOUR_FLAG_256, COLOUR_FLAG_RGB,
    DEFAULT_CELL, FLAG_PADDING, FLAG_TAB, LINE_WRAPPED,
};
use write::*;

/// Which tmux release's behaviour to reproduce where 3.7 and 3.8 differ.
#[derive(Clone, Copy, PartialEq, Eq, Debug, Default)]
pub enum Compat {
    Tmux37,
    #[default]
    Tmux38,
}

/// The two forms of `tmux capture-pane`.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Capture {
    /// `capture-pane -p`.
    Plain,
    /// `capture-pane -p -e`.
    Styled,
}

/// Mouse reporting requested by the program (DECSET 1000, 1002, 1003).
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum MouseTracking {
    Off,
    Standard,
    Button,
    Any,
}

/// Cursor shape requested with DECSCUSR.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum CursorShape {
    Default,
    Block,
    Underline,
    Bar,
}

/// Pane facts: what tmux's `#{cursor_x}`, `#{alternate_on}`,
/// `#{bracket_paste_flag}` and related formats report.
#[derive(Clone, PartialEq, Eq, Debug)]
pub struct Info {
    /// Cursor column in cells; equals `width` after the last column was written.
    pub cursor_x: usize,
    pub cursor_y: usize,
    pub width: usize,
    pub height: usize,
    pub alternate_screen: bool,
    pub bracketed_paste: bool,
    pub application_cursor_keys: bool,
    pub application_keypad: bool,
    pub mouse: MouseTracking,
    pub mouse_utf8: bool,
    pub mouse_sgr: bool,
    pub focus_reporting: bool,
    pub cursor_visible: bool,
    pub cursor_shape: CursorShape,
    pub cursor_blinking: bool,
    pub insert_mode: bool,
    pub origin_mode: bool,
    pub autowrap: bool,
    pub scroll_region: (usize, usize),
}

impl Info {
    /// What `tmux display-message -p` prints for `#{cursor_x} #{cursor_y}
    /// #{pane_width} #{pane_height} #{alternate_on} #{bracket_paste_flag}
    /// #{keypad_cursor_flag} #{keypad_flag} #{cursor_flag} #{insert_flag}
    /// #{origin_flag} #{wrap_flag} #{mouse_standard_flag} #{mouse_button_flag}
    /// #{mouse_all_flag} #{mouse_any_flag} #{mouse_sgr_flag} #{mouse_utf8_flag}
    /// #{scroll_region_upper} #{scroll_region_lower} #{cursor_shape}
    /// #{cursor_blinking}`.
    pub fn tmux_line(&self) -> String {
        let flag = |value: bool| if value { "1" } else { "0" };
        let shape = match self.cursor_shape {
            CursorShape::Default => "default",
            CursorShape::Block => "block",
            CursorShape::Underline => "underline",
            CursorShape::Bar => "bar",
        };
        [
            self.cursor_x.to_string(),
            self.cursor_y.to_string(),
            self.width.to_string(),
            self.height.to_string(),
            flag(self.alternate_screen).into(),
            flag(self.bracketed_paste).into(),
            flag(self.application_cursor_keys).into(),
            flag(self.application_keypad).into(),
            flag(self.cursor_visible).into(),
            flag(self.insert_mode).into(),
            flag(self.origin_mode).into(),
            flag(self.autowrap).into(),
            flag(self.mouse == MouseTracking::Standard).into(),
            flag(self.mouse == MouseTracking::Button).into(),
            flag(self.mouse == MouseTracking::Any).into(),
            flag(self.mouse != MouseTracking::Off).into(),
            flag(self.mouse_sgr).into(),
            flag(self.mouse_utf8).into(),
            self.scroll_region.0.to_string(),
            self.scroll_region.1.to_string(),
            shape.into(),
            flag(self.cursor_blinking).into(),
        ]
        .join(" ")
    }
}

/// The screen of one pane.
#[derive(Clone, Debug)]
pub struct ScreenModel {
    screen: Screen,
    parser: input::Parser,
}

impl ScreenModel {
    /// An empty screen; sizes below one cell become one cell.
    pub fn new(rows: u16, cols: u16) -> Self {
        Self::with_compat(rows, cols, Compat::default())
    }

    pub fn with_compat(rows: u16, cols: u16, compat: Compat) -> Self {
        Self {
            screen: Screen::new(usize::from(cols), usize::from(rows), compat),
            parser: input::Parser::new(),
        }
    }

    /// Feed program output. Escape sequences and UTF-8 characters may be split
    /// across calls.
    pub fn process(&mut self, bytes: &[u8]) {
        self.parser.parse(&mut self.screen, bytes);
    }

    /// Answers to the queries in the output processed so far, in order, as
    /// tmux sends them to the program; the buffer is emptied.
    pub fn take_replies(&mut self) -> Vec<u8> {
        std::mem::take(&mut self.parser.replies)
    }

    /// Resize as tmux resizes a pane: the main screen reflows wrapped lines and
    /// pulls lines back from history when it grows; synchronized output ends.
    pub fn resize(&mut self, rows: u16, cols: u16) {
        let (sx, sy) = (usize::from(cols).max(1), usize::from(rows).max(1));
        if sx == self.screen.sx() && sy == self.screen.sy() {
            return;
        }
        self.screen.mode &= !MODE_SYNC;
        let reflow = !self.screen.is_alternate();
        self.screen.resize(sx, sy, reflow);
    }

    /// The visible screen as `tmux capture-pane -p` (or `-p -e`) prints it,
    /// read as UTF-8 the way callers read tmux's output (`from_utf8_lossy`).
    pub fn capture(&self, mode: Capture) -> String {
        String::from_utf8_lossy(&self.capture_bytes(mode)).into_owned()
    }

    /// The exact bytes tmux prints. They are UTF-8 except where a program
    /// wrote four-byte sequences beyond U+10FFFF, which tmux keeps.
    pub fn capture_bytes(&self, mode: Capture) -> Vec<u8> {
        self.serialize(mode == Capture::Styled, false, false)
    }

    /// The visible screen as text with wrapped rows joined, as `tmux
    /// capture-pane -p -J` joins them: a row the program's output wrapped
    /// onto the next one continues there with its trailing spaces, so a long
    /// line reads whole. Unlike `-J`, a row that ends a line is trimmed, as
    /// the Plain capture trims it.
    pub fn joined_text(&self) -> String {
        String::from_utf8_lossy(&self.serialize(false, false, true)).into_owned()
    }

    /// `capture-pane -p -e` including tmux's OSC 8 hyperlinks, for comparing
    /// with tmux. A row that ends inside a link repeats the last cell's codes
    /// before closing it, because tmux reuses that buffer.
    fn capture_with_links(&self) -> Vec<u8> {
        self.serialize(true, true, false)
    }

    /// `cmd_capture_pane_history` over the visible rows (`grid_string_cells`),
    /// with wrapped rows joined to the next when `join` (`-J`).
    fn serialize(&self, styled: bool, links: bool, join: bool) -> Vec<u8> {
        let screen = &self.screen;
        let mut out = Vec::new();
        let mut last = DEFAULT_CELL;
        let mut code = Vec::new();
        for y in 0..screen.sy() {
            let line = screen.grid.view_line(y);
            let mut row = Vec::new();
            let mut has_link = false;
            for x in 0..screen.sx().min(line.cellsize()) {
                let cell = line.cell(x);
                if cell.flags & FLAG_PADDING != 0 {
                    continue;
                }
                if styled {
                    code.clear();
                    tmux_sgr(&last, &cell, &mut code);
                    if links && last.link != cell.link {
                        if let Some((uri, id)) = self.parser.link_target(cell.link) {
                            has_link = add_hyperlink(&mut code, id, uri);
                        } else if has_link {
                            add_hyperlink(&mut code, b"", b"");
                            has_link = false;
                        }
                    }
                    last = cell;
                    row.extend_from_slice(&code);
                }
                if cell.flags & FLAG_TAB != 0 {
                    row.push(b'\t');
                } else {
                    row.extend_from_slice(cell.data.as_bytes());
                }
            }
            if has_link {
                add_hyperlink(&mut code, b"", b"");
                row.extend_from_slice(&code);
            }
            let wrapped = join && line.flags & LINE_WRAPPED != 0;
            while !wrapped && row.last() == Some(&b' ') {
                row.pop();
            }
            out.extend_from_slice(&row);
            if !wrapped {
                out.push(b'\n');
            }
        }
        out
    }

    pub fn info(&self) -> Info {
        let screen = &self.screen;
        let mode = screen.mode;
        let mouse = if mode & MODE_MOUSE_ALL != 0 {
            MouseTracking::Any
        } else if mode & MODE_MOUSE_BUTTON != 0 {
            MouseTracking::Button
        } else if mode & MODE_MOUSE_STANDARD != 0 {
            MouseTracking::Standard
        } else {
            MouseTracking::Off
        };
        Info {
            cursor_x: screen.cx,
            cursor_y: screen.cy,
            width: screen.sx(),
            height: screen.sy(),
            alternate_screen: screen.is_alternate(),
            bracketed_paste: mode & MODE_BRACKETPASTE != 0,
            application_cursor_keys: mode & MODE_KCURSOR != 0,
            application_keypad: mode & MODE_KKEYPAD != 0,
            mouse,
            mouse_utf8: mode & MODE_MOUSE_UTF8 != 0,
            mouse_sgr: mode & MODE_MOUSE_SGR != 0,
            focus_reporting: mode & MODE_FOCUSON != 0,
            cursor_visible: mode & MODE_CURSOR != 0,
            cursor_shape: match screen.cstyle {
                CursorStyle::Default => CursorShape::Default,
                CursorStyle::Block => CursorShape::Block,
                CursorStyle::Underline => CursorShape::Underline,
                CursorStyle::Bar => CursorShape::Bar,
            },
            cursor_blinking: mode & MODE_CURSOR_BLINKING != 0,
            insert_mode: mode & MODE_INSERT != 0,
            origin_mode: mode & MODE_ORIGIN != 0,
            autowrap: mode & MODE_WRAP != 0,
            scroll_region: (screen.rupper, screen.rlower),
        }
    }

    /// Escape sequences that bring a fresh terminal of the same size to this
    /// screen and its input modes: the main screen, then the alternate screen
    /// entered with DECSET 1049 when the program uses it, the scroll region,
    /// the cursor saved with DECSC, keyboard, mouse and paste modes, the cursor
    /// shape and position (including a pending wrap at the last column) and
    /// the current drawing attributes.
    pub fn repaint(&self) -> Vec<u8> {
        let screen = &self.screen;
        let (sx, sy) = (screen.sx(), screen.sy());
        let mut out = Vec::new();
        // Paint with autowrap on: rows are positioned explicitly, and a
        // combining character after the last column joins it as in tmux.
        out.extend_from_slice(
            b"\x1b[?25l\x1b[0m\x1b(B\x1b)0\x0f\x1b[r\x1b[?6l\x1b[4l\x1b[?7h\x1b[H\x1b[2J",
        );
        let visible: Vec<&Line> = (0..sy).map(|y| screen.grid.view_line(y)).collect();
        if let Some(main) = &screen.saved_grid {
            let lines: Vec<&Line> = main.lines.iter().take(sy).collect();
            paint(&mut out, &lines, sx);
            if let (Some(cx), Some(cy)) = screen.saved_cursor() {
                cursor_to(&mut out, cy.min(sy - 1), cx.min(sx - 1));
                out.extend(repaint_sgr(&screen.saved_pen_cell()));
            }
            out.extend_from_slice(b"\x1b[?1049h\x1b[0m\x1b[H\x1b[2J");
        }
        paint(&mut out, &visible, sx);
        if (screen.rupper, screen.rlower) != (0, sy - 1) {
            out.extend(format!("\x1b[{};{}r", screen.rupper + 1, screen.rlower + 1).into_bytes());
        }
        self.parser.repaint_saved_cursor(screen, &mut out);
        let mode = screen.mode;
        for (flag, sequence) in [
            (MODE_KCURSOR, &b"\x1b[?1h"[..]),
            (MODE_KKEYPAD, b"\x1b="),
            (MODE_BRACKETPASTE, b"\x1b[?2004h"),
            (MODE_MOUSE_STANDARD, b"\x1b[?1000h"),
            (MODE_MOUSE_BUTTON, b"\x1b[?1002h"),
            (MODE_MOUSE_ALL, b"\x1b[?1003h"),
            (MODE_MOUSE_UTF8, b"\x1b[?1005h"),
            (MODE_MOUSE_SGR, b"\x1b[?1006h"),
            (MODE_FOCUSON, b"\x1b[?1004h"),
            (MODE_ORIGIN, b"\x1b[?6h"),
        ] {
            if mode & flag != 0 {
                out.extend_from_slice(sequence);
            }
        }
        let blinking = mode & MODE_CURSOR_BLINKING != 0;
        let shape = match screen.cstyle {
            CursorStyle::Default => None,
            CursorStyle::Block => Some(1),
            CursorStyle::Underline => Some(3),
            CursorStyle::Bar => Some(5),
        };
        if shape.is_none() && blinking {
            // DECSCUSR 0 keeps the blinking flag of the previous style.
            out.extend_from_slice(b"\x1b[1 q\x1b[0 q");
        }
        if mode & MODE_CURSOR_BLINKING_SET != 0 {
            out.extend_from_slice(if blinking { b"\x1b[?12h" } else { b"\x1b[?12l" });
        }
        if let Some(base) = shape {
            out.extend(format!("\x1b[{} q", base + usize::from(!blinking)).into_bytes());
        }
        let origin = mode & MODE_ORIGIN != 0;
        // In origin mode a cursor outside the scroll region cannot be
        // addressed; it goes to the top of the region instead.
        let addressable = !origin || (screen.rupper..=screen.rlower).contains(&screen.cy);
        let row = if !origin {
            screen.cy
        } else if addressable {
            screen.cy - screen.rupper
        } else {
            0
        };
        if screen.cx >= sx && addressable {
            // Rewrite the last column so the terminal waits to wrap, as tmux
            // does after writing it (autowrap is still on here).
            let (start, text, cell) = drawable_cell(visible[screen.cy.min(sy - 1)], sx - 1, sx);
            cursor_to(&mut out, row, start);
            out.extend(repaint_sgr(&cell));
            out.extend_from_slice(b"\x1b(B\x1b)0");
            out.extend(charset_shift(cell.attr & ATTR_CHARSET != 0));
            out.extend_from_slice(&text);
            out.push(0x0f);
        } else {
            cursor_to(&mut out, row, screen.cx.min(sx - 1));
        }
        if mode & MODE_WRAP == 0 {
            out.extend_from_slice(b"\x1b[?7l");
        }
        if mode & MODE_INSERT != 0 {
            out.extend_from_slice(b"\x1b[4h");
        }
        self.parser.repaint_pen(&mut out);
        if mode & MODE_CURSOR != 0 {
            out.extend_from_slice(b"\x1b[?25h");
        }
        out
    }

    /// The stored cells, like tmux 3.8's `capture-pane -R`, for debugging.
    fn grid_dump(&self) -> String {
        let grid = &self.screen.grid;
        let mut out = format!(
            "G {}x{} ({}/{})\n",
            grid.sx, grid.sy, grid.hsize, grid.hlimit
        );
        for (y, line) in grid.lines.iter().enumerate() {
            out.push_str(&format!(
                "\tL {y} flags={:x} {}/{}\n",
                line.flags,
                line.cellused,
                line.cellsize()
            ));
            for (x, entry) in line.cells.iter().enumerate() {
                let cell = entry.cell;
                out.push_str(&format!(
                    "\t\tC {y},{x} data=({},{},{:?}) flags={:x} attr={:x} fg={:x} bg={:x} us={:x} link={} extended={}\n",
                    cell.data.width,
                    cell.data.size,
                    String::from_utf8_lossy(cell.data.as_bytes()),
                    cell.flags,
                    cell.attr,
                    cell.fg,
                    cell.bg,
                    cell.us,
                    cell.link,
                    entry.extended
                ));
            }
        }
        out
    }

    /// Visible cells as text for comparing what two screens show, regardless
    /// of how tmux happens to store them (tests only).
    #[cfg(test)]
    fn visual(&self) -> Vec<String> {
        visual_rows(&self.screen)
    }
}

/// Visible cells as one descriptor per cell and row: what a terminal shows,
/// whatever the storage (tab cells as spaces, unallocated cells as blanks).
pub fn visual_rows_of(model: &ScreenModel) -> Vec<String> {
    visual_rows(&model.screen)
}

fn visual_rows(screen: &Screen) -> Vec<String> {
    let sx = screen.sx();
    (0..screen.sy())
        .map(|y| {
            let line = screen.grid.view_line(y);
            let mut row = String::new();
            let mut x = 0;
            while x < sx {
                let (start, text, cell) = drawable_cell(line, x, sx);
                // A column inside a glyph that starts further left (wide
                // characters overlapping after a reflow) shows as a blank.
                let (width, text, cell) = if start == x {
                    (
                        visual_width(&cell),
                        String::from_utf8_lossy(&text).into_owned(),
                        cell,
                    )
                } else {
                    (1, " ".to_owned(), DEFAULT_CELL)
                };
                // The alternate character set only changes ASCII characters.
                let attr = if text.len() == 1 {
                    cell.attr
                } else {
                    cell.attr & !ATTR_CHARSET
                };
                row.push_str(&format!(
                    "[{}|{}|{:x}|{:x}|{:x}|{:x}]",
                    text, width, attr, cell.fg, cell.bg, cell.us
                ));
                x += width.max(1);
            }
            row
        })
        .collect()
}

/// [`visual_cell_at`], except that a wide character cut off by the right
/// edge (left there by a resize) shows as a blank: no terminal can draw it.
/// A glyph made wide by joining a narrow character still draws there.
fn drawable_cell(line: &Line, x: usize, sx: usize) -> (usize, Vec<u8>, Cell) {
    let (start, text, cell) = visual_cell_at(line, x);
    if start == x && x + visual_width(&cell) > sx {
        let first = utf8::first_char(&text).map_or(1, utf8::char_width);
        if first > 1 {
            return (x, b" ".to_vec(), DEFAULT_CELL);
        }
    }
    (start, text, cell)
}

fn visual_width(cell: &Cell) -> usize {
    if cell.flags & FLAG_TAB != 0 {
        1
    } else {
        usize::from(cell.data.width).max(1)
    }
}

/// The cell a terminal shows at column `x`: the start column, the text to
/// draw there and its style. Tabs show as spaces in their own style.
fn visual_cell_at(line: &Line, x: usize) -> (usize, Vec<u8>, Cell) {
    if x >= line.cellsize() {
        return (x, b" ".to_vec(), DEFAULT_CELL);
    }
    let cell = line.cell(x);
    if cell.flags & FLAG_PADDING != 0 {
        let mut start = x;
        while start > 0 {
            start -= 1;
            let owner = line.cell(start);
            if owner.flags & FLAG_PADDING != 0 {
                continue;
            }
            if owner.flags & FLAG_TAB != 0 {
                return (x, b" ".to_vec(), owner);
            }
            if usize::from(owner.data.width) > x - start {
                return (start, glyph_text(owner.data.as_bytes()), owner);
            }
            break;
        }
        // Padding left without its character draws as a plain blank.
        return (x, b" ".to_vec(), DEFAULT_CELL);
    }
    if cell.flags & FLAG_TAB != 0 || cell.data.size == 0 {
        return (x, b" ".to_vec(), cell);
    }
    (x, glyph_text(cell.data.as_bytes()), cell)
}

/// The text a terminal draws for a glyph. A tab can join a cell that ends in
/// a joiner (tmux combines its spaces); those spaces draw nothing.
fn glyph_text(bytes: &[u8]) -> Vec<u8> {
    let first = match bytes.first() {
        Some(0xc2..=0xdf) => 2,
        Some(0xe0..=0xef) => 3,
        Some(0xf0..=0xf4) => 4,
        _ => 1,
    };
    let (head, tail) = bytes.split_at(first.min(bytes.len()));
    head.iter()
        .chain(tail.iter().filter(|byte| **byte != b' '))
        .copied()
        .collect()
}

fn cursor_to(out: &mut Vec<u8>, row: usize, col: usize) {
    out.extend(format!("\x1b[{};{}H", row + 1, col + 1).into_bytes());
}

fn charset_shift(on: bool) -> &'static [u8] {
    if on {
        b"\x0e"
    } else {
        b"\x0f"
    }
}

/// Paint rows on a cleared screen; every row is positioned explicitly.
fn paint(out: &mut Vec<u8>, lines: &[&Line], sx: usize) {
    for (y, line) in lines.iter().enumerate() {
        let mut end = 0;
        for x in 0..sx.min(line.cellsize()) {
            let cell = line.cell(x);
            let blank = cell.flags & FLAG_PADDING == 0
                && cell.flags & FLAG_TAB == 0
                && cell.data.as_bytes() == b" "
                && cell.attr == 0
                && cell.fg == 8
                && cell.bg == 8
                && cell.us == 8;
            if !blank {
                end = x + 1;
            }
        }
        if end == 0 {
            continue;
        }
        // The glyphs to draw, as (column, width, text, style).
        let mut glyphs = Vec::new();
        let mut x = 0;
        while x < end {
            let (start, text, cell) = drawable_cell(line, x, sx);
            if start != x {
                // An orphaned half of a wide character shows as a blank.
                x += 1;
                continue;
            }
            let width = visual_width(&cell);
            glyphs.push((x, width, text, cell));
            x += width;
        }
        // tmux joins a character to the cell before it (after a joiner, a
        // regional indicator or a skin tone base), whatever wrote that cell.
        // Draw such runs from the right so each cell stays separate.
        let mut order = Vec::with_capacity(glyphs.len());
        let mut first = 0;
        while first < glyphs.len() {
            let mut last = first;
            while last + 1 < glyphs.len()
                && glyphs[last].0 + glyphs[last].1 == glyphs[last + 1].0
                && utf8::would_join(&glyphs[last].2, &glyphs[last + 1].2)
            {
                last += 1;
            }
            order.extend((first..=last).rev());
            first = last + 1;
        }
        let mut style = None;
        let mut shifted = false;
        let mut cursor = None;
        for index in order {
            let (x, width, text, cell) = &glyphs[index];
            if cursor != Some(*x) {
                cursor_to(out, y, *x);
            }
            let mut plain = *cell;
            plain.data = DEFAULT_CELL.data;
            plain.flags = 0;
            plain.link = 0;
            if style != Some(plain) {
                out.extend(repaint_sgr(cell));
                style = Some(plain);
            }
            let charset = cell.attr & ATTR_CHARSET != 0;
            if charset != shifted {
                out.extend_from_slice(charset_shift(charset));
                shifted = charset;
            }
            out.extend_from_slice(text);
            cursor = Some(x + width);
        }
        if shifted {
            out.push(0x0f);
        }
        out.extend_from_slice(b"\x1b[0m");
    }
}

/// SGR sequences for a cell's style, starting from a reset. Colours get
/// sequences of their own: terminals limit parameters (tmux to 63 bytes).
fn repaint_sgr(cell: &Cell) -> Vec<u8> {
    let mut codes = vec!["0".to_owned()];
    for (mask, code) in [
        (ATTR_BRIGHT, "1"),
        (ATTR_DIM, "2"),
        (ATTR_ITALICS, "3"),
        (ATTR_UNDERSCORE, "4"),
        (ATTR_UNDERSCORE_2, "4:2"),
        (ATTR_UNDERSCORE_3, "4:3"),
        (ATTR_UNDERSCORE_4, "4:4"),
        (ATTR_UNDERSCORE_5, "4:5"),
        (ATTR_BLINK, "5"),
        (ATTR_REVERSE, "7"),
        (ATTR_HIDDEN, "8"),
        (ATTR_STRIKETHROUGH, "9"),
        (ATTR_OVERLINE, "53"),
    ] {
        if cell.attr & mask != 0 {
            codes.push(code.into());
        }
    }
    let mut out = format!("\x1b[{}m", codes.join(";"));
    for (value, base, extended) in [(cell.fg, 30, 38), (cell.bg, 40, 48), (cell.us, 0, 58)] {
        if value & COLOUR_FLAG_256 != 0 {
            out.push_str(&format!("\x1b[{extended};5;{}m", value & 0xff));
        } else if value & COLOUR_FLAG_RGB != 0 {
            let [r, g, b] = split_rgb(value);
            out.push_str(&format!("\x1b[{extended};2;{r};{g};{b}m"));
        } else if base != 0 {
            match value {
                0..=7 => out.push_str(&format!("\x1b[{}m", base + value)),
                90..=97 => out.push_str(&format!("\x1b[{}m", value + base - 30)),
                _ => {}
            }
        }
    }
    out.into_bytes()
}

/// `grid_string_cells_code`: tmux's SGR transition from `last` to `gc`.
fn tmux_sgr(last: &Cell, gc: &Cell, out: &mut Vec<u8>) {
    const ATTRS: [(u16, u32); 13] = [
        (ATTR_BRIGHT, 1),
        (ATTR_DIM, 2),
        (ATTR_ITALICS, 3),
        (ATTR_UNDERSCORE, 4),
        (ATTR_BLINK, 5),
        (ATTR_REVERSE, 7),
        (ATTR_HIDDEN, 8),
        (ATTR_STRIKETHROUGH, 9),
        (ATTR_UNDERSCORE_2, 42),
        (ATTR_UNDERSCORE_3, 43),
        (ATTR_UNDERSCORE_4, 44),
        (ATTR_UNDERSCORE_5, 45),
        (ATTR_OVERLINE, 53),
    ];
    let attr = gc.attr;
    let mut lastattr = last.attr;
    let mut codes = Vec::new();
    for (mask, _) in ATTRS {
        if (attr & mask == 0 && lastattr & mask != 0) || (last.us != 8 && gc.us == 8) {
            codes.push(0);
            lastattr &= ATTR_CHARSET;
            break;
        }
    }
    for (mask, code) in ATTRS {
        if attr & mask != 0 && lastattr & mask == 0 {
            codes.push(code);
        }
    }
    if !codes.is_empty() {
        let parts: Vec<String> = codes
            .iter()
            .map(|code| {
                if *code < 10 {
                    code.to_string()
                } else {
                    format!("{}:{}", code / 10, code % 10)
                }
            })
            .collect();
        out.extend(format!("\x1b[{}m", parts.join(";")).into_bytes());
    }
    let reset = codes.first() == Some(&0);
    add_colour_code(out, reset, &fg_codes(gc.fg), &fg_codes(last.fg));
    add_colour_code(out, reset, &bg_codes(gc.bg), &bg_codes(last.bg));
    add_colour_code(out, reset, &us_codes(gc.us), &us_codes(last.us));
    if attr & ATTR_CHARSET != 0 && lastattr & ATTR_CHARSET == 0 {
        out.push(0x0e);
    }
    if attr & ATTR_CHARSET == 0 && lastattr & ATTR_CHARSET != 0 {
        out.push(0x0f);
    }
}

/// `grid_string_cells_add_hyperlink`: append an OSC 8 sequence unless it
/// would overflow tmux's 8 KiB code buffer.
fn add_hyperlink(out: &mut Vec<u8>, id: &[u8], uri: &[u8]) -> bool {
    if uri.len() + id.len() + 17 >= 8192 {
        return false;
    }
    out.extend_from_slice(b"\x1b]8;");
    if !id.is_empty() {
        out.extend_from_slice(b"id=");
        out.extend_from_slice(id);
    }
    out.push(b';');
    out.extend_from_slice(uri);
    out.extend_from_slice(b"\x1b\\");
    true
}

fn add_colour_code(out: &mut Vec<u8>, reset: bool, new: &[u32], old: &[u32]) {
    if new.is_empty() || (!reset && new == old) || (reset && (new[0] == 49 || new[0] == 39)) {
        return;
    }
    let parts: Vec<String> = new.iter().map(u32::to_string).collect();
    out.extend(format!("\x1b[{}m", parts.join(";")).into_bytes());
}

fn split_rgb(value: u32) -> [u32; 3] {
    [(value >> 16) & 0xff, (value >> 8) & 0xff, value & 0xff]
}

fn fg_codes(fg: u32) -> Vec<u32> {
    if fg & COLOUR_FLAG_256 != 0 {
        vec![38, 5, fg & 0xff]
    } else if fg & COLOUR_FLAG_RGB != 0 {
        let [r, g, b] = split_rgb(fg);
        vec![38, 2, r, g, b]
    } else {
        match fg {
            0..=7 => vec![fg + 30],
            8 => vec![39],
            90..=97 => vec![fg],
            _ => Vec::new(),
        }
    }
}

fn bg_codes(bg: u32) -> Vec<u32> {
    if bg & COLOUR_FLAG_256 != 0 {
        vec![48, 5, bg & 0xff]
    } else if bg & COLOUR_FLAG_RGB != 0 {
        let [r, g, b] = split_rgb(bg);
        vec![48, 2, r, g, b]
    } else {
        match bg {
            0..=7 => vec![bg + 40],
            8 => vec![49],
            90..=97 => vec![bg + 10],
            _ => Vec::new(),
        }
    }
}

fn us_codes(us: u32) -> Vec<u32> {
    if us & COLOUR_FLAG_256 != 0 {
        vec![58, 5, us & 0xff]
    } else if us & COLOUR_FLAG_RGB != 0 {
        let [r, g, b] = split_rgb(us);
        vec![58, 2, r, g, b]
    } else {
        Vec::new()
    }
}

/// `hgs __screen-capture`: feed standard input to a model and print what it
/// captures. A private test entry for the differential tmux test.
///
/// Options: `--rows R --cols C` (the size, required), `--tmux 3.7|3.8`,
/// `--resize-after OFFSET:ROWSxCOLS` (repeatable; resize once OFFSET bytes
/// were processed), and one output: `--plain` (default), `--styled`,
/// `--info`, `--repaint`, `--grid` (stored cells, for debugging) or `--json`
/// (captures, tmux's form with links, pane facts, repaint and visible cells;
/// bytes in base64).
pub fn cli(args: &[String]) -> Result<i32, String> {
    use std::io::{Read, Write};
    let mut rows = None;
    let mut cols = None;
    let mut compat = None;
    let mut resizes: Vec<(usize, u16, u16)> = Vec::new();
    let mut output = "--plain".to_owned();
    let mut index = 0;
    let value = |index: usize, name: &str| {
        args.get(index + 1)
            .cloned()
            .ok_or_else(|| format!("{name} needs a value"))
    };
    while index < args.len() {
        let arg = args[index].as_str();
        match arg {
            "--rows" | "--cols" => {
                let parsed: u16 = value(index, arg)?
                    .parse()
                    .map_err(|_| format!("invalid {arg}"))?;
                if arg == "--rows" {
                    rows = Some(parsed);
                } else {
                    cols = Some(parsed);
                }
                index += 2;
            }
            "--tmux" => {
                compat = match value(index, arg)?.as_str() {
                    "3.7" => Some(Compat::Tmux37),
                    "3.8" => Some(Compat::Tmux38),
                    other => return Err(format!("unsupported tmux behaviour {other}")),
                };
                index += 2;
            }
            "--resize-after" => {
                let spec = value(index, arg)?;
                let parsed = spec.split_once(':').and_then(|(offset, size)| {
                    let (r, c) = size.split_once('x')?;
                    Some((offset.parse().ok()?, r.parse().ok()?, c.parse().ok()?))
                });
                resizes.push(parsed.ok_or_else(|| format!("invalid --resize-after {spec}"))?);
                index += 2;
            }
            "--plain" | "--styled" | "--info" | "--repaint" | "--json" | "--grid" => {
                output = arg.to_owned();
                index += 1;
            }
            _ => return Err(format!("unknown option {arg}")),
        }
    }
    let (Some(rows), Some(cols)) = (rows, cols) else {
        return Err("usage: hgs __screen-capture --rows R --cols C [--tmux 3.7|3.8] \
             [--resize-after OFFSET:ROWSxCOLS]... [--plain|--styled|--info|--repaint|--grid|--json]"
            .into());
    };
    let mut stream = Vec::new();
    std::io::stdin()
        .read_to_end(&mut stream)
        .map_err(|e| e.to_string())?;
    let mut model = match compat {
        Some(compat) => ScreenModel::with_compat(rows, cols, compat),
        None => ScreenModel::new(rows, cols),
    };
    resizes.sort_by_key(|(offset, _, _)| *offset);
    let mut done = 0;
    for (offset, r, c) in resizes {
        let offset = offset.min(stream.len()).max(done);
        model.process(&stream[done..offset]);
        model.resize(r, c);
        done = offset;
    }
    model.process(&stream[done..]);
    let bytes = match output.as_str() {
        "--styled" => model.capture(Capture::Styled).into_bytes(),
        "--info" => format!("{}\n", model.info().tmux_line()).into_bytes(),
        "--repaint" => model.repaint(),
        "--grid" => model.grid_dump().into_bytes(),
        "--json" => {
            use base64::Engine as _;
            let encode = |bytes: Vec<u8>| base64::engine::general_purpose::STANDARD.encode(bytes);
            let value = serde_json::json!({
                "plain": encode(model.capture_bytes(Capture::Plain)),
                "styled": encode(model.capture_bytes(Capture::Styled)),
                "styled_links": encode(model.capture_with_links()),
                "info": model.info().tmux_line(),
                "repaint": encode(model.repaint()),
                "replies": encode(model.take_replies()),
                "cells": visual_rows_of(&model),
            });
            format!("{value}\n").into_bytes()
        }
        _ => model.capture(Capture::Plain).into_bytes(),
    };
    std::io::stdout()
        .write_all(&bytes)
        .map_err(|e| e.to_string())?;
    Ok(0)
}

#[cfg(test)]
mod tests;
