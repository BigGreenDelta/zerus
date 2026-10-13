//! The output parser of tmux's input.c: a DEC/ANSI state machine (after Paul
//! Williams' description, as tmux implements it) driving the screen.
//!
//! Queries are answered as tmux answers them, in order, into a reply buffer:
//! primary and secondary device attributes (as a tmux built without sixel),
//! status and cursor position reports, mode reports (DECRQM), the text area
//! size and the cursor style (DECRQSS). Answers that depend on the terminal
//! outside (its colours, pixel sizes, version or theme) are left to the host.

use super::grid::{
    Cell, Glyph, ATTR_ALL_UNDERSCORE, ATTR_BLINK, ATTR_BRIGHT, ATTR_CHARSET, ATTR_DIM, ATTR_HIDDEN,
    ATTR_ITALICS, ATTR_OVERLINE, ATTR_REVERSE, ATTR_STRIKETHROUGH, ATTR_UNDERSCORE,
    ATTR_UNDERSCORE_2, ATTR_UNDERSCORE_3, ATTR_UNDERSCORE_4, ATTR_UNDERSCORE_5, COLOUR_FLAG_256,
    COLOUR_FLAG_RGB, DEFAULT_CELL,
};
use super::utf8::{self, Decoded, Decoder};
use super::write::*;
use std::collections::HashMap;

const INTERM_MAX: usize = 3;
const PARAM_MAX: usize = 63;
const INPUT_MAX: usize = 1 << 20;
const PARAM_LIST_MAX: usize = 24;

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
enum State {
    Ground,
    EscEnter,
    EscIntermediate,
    CsiEnter,
    CsiParameter,
    CsiIntermediate,
    CsiIgnore,
    DcsEnter,
    DcsParameter,
    DcsIntermediate,
    DcsHandler,
    DcsEscape,
    DcsIgnore,
    OscString,
    ApcString,
    RenameString,
    ConsumeSt,
}

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
enum Action {
    None,
    C0,
    Print,
    TopBit,
    Intermediate,
    Parameter,
    Input,
    EscDispatch,
    CsiDispatch,
    DcsDispatch,
    EndBel,
}

#[derive(Clone, Debug)]
enum Param {
    Missing,
    Number(i32),
    Text(String),
}

/// The drawing pen with its character sets (`struct input_cell`).
#[derive(Clone, Copy, Debug)]
struct Pen {
    cell: Cell,
    set: u8,
    g0: bool,
    g1: bool,
}

const DEFAULT_PEN: Pen = Pen {
    cell: DEFAULT_CELL,
    set: 0,
    g0: false,
    g1: false,
};

#[derive(Clone, Debug)]
pub(super) struct Parser {
    state: State,
    pen: Pen,
    old_pen: Pen,
    old_cx: usize,
    old_cy: usize,
    old_mode: u32,
    interm: Vec<u8>,
    param: Vec<u8>,
    input: Vec<u8>,
    params: Vec<Param>,
    discard: bool,
    /// Answers to queries, in the order the queries arrived.
    pub(super) replies: Vec<u8>,
    last_printed: bool,
    last: Glyph,
    utf8: Option<Decoder>,
    links: HashMap<(Vec<u8>, Vec<u8>), u32>,
    /// URI and id of every hyperlink, by number minus one, as tmux keeps them.
    targets: Vec<(Vec<u8>, Vec<u8>)>,
}

impl Parser {
    pub(super) fn new() -> Self {
        Self {
            state: State::Ground,
            pen: DEFAULT_PEN,
            old_pen: DEFAULT_PEN,
            old_cx: 0,
            old_cy: 0,
            old_mode: 0,
            interm: Vec::new(),
            param: Vec::new(),
            input: Vec::new(),
            params: Vec::new(),
            discard: false,
            replies: Vec::new(),
            last_printed: false,
            last: Glyph::ascii(b' '),
            utf8: None,
            links: HashMap::new(),
            targets: Vec::new(),
        }
    }

    /// `input_parse`, followed by the end of collection `screen_write_stop`
    /// performs after every buffer.
    pub(super) fn parse(&mut self, screen: &mut Screen, bytes: &[u8]) {
        for &ch in bytes {
            let (action, next) = transition(self.state, ch);
            if action != Action::Print {
                screen.collect_end();
            }
            self.run(screen, action, ch);
            if let Some(next) = next {
                self.set_state(next);
            }
        }
        screen.collect_end();
    }

    /// `input_set_state`: leaving an OSC string dispatches it; entering a
    /// sequence or string clears what was collected.
    fn set_state(&mut self, next: State) {
        if self.state == State::OscString {
            self.exit_osc();
        }
        self.state = next;
        match next {
            State::EscEnter | State::CsiEnter => self.clear(),
            State::DcsEnter
            | State::OscString
            | State::ApcString
            | State::RenameString
            | State::ConsumeSt => {
                self.clear();
                self.last_printed = false;
            }
            _ => {}
        }
    }

    fn clear(&mut self) {
        self.interm.clear();
        self.param.clear();
        self.input.clear();
        self.discard = false;
    }

    fn run(&mut self, screen: &mut Screen, action: Action, ch: u8) {
        match action {
            Action::None | Action::EndBel => {}
            Action::C0 => self.c0_dispatch(screen, ch),
            Action::Print => self.print(screen, ch),
            Action::TopBit => self.top_bit_set(screen, ch),
            Action::Intermediate => {
                if self.interm.len() == INTERM_MAX {
                    self.discard = true;
                } else {
                    self.interm.push(ch);
                }
            }
            Action::Parameter => {
                if self.param.len() == PARAM_MAX {
                    self.discard = true;
                } else {
                    self.param.push(ch);
                }
            }
            Action::Input => {
                if self.input.len() + 1 >= INPUT_MAX {
                    self.discard = true;
                } else {
                    self.input.push(ch);
                }
            }
            Action::EscDispatch => self.esc_dispatch(screen, ch),
            Action::CsiDispatch => self.csi_dispatch(screen, ch),
            Action::DcsDispatch => self.dcs_dispatch(screen),
        }
    }

    /// `input_stop_utf8`: an unfinished UTF-8 character becomes U+FFFD.
    fn stop_utf8(&mut self, screen: &mut Screen) {
        if self.utf8.take().is_some() {
            self.write_replacement(screen);
        }
    }

    fn write_replacement(&mut self, screen: &mut Screen) {
        let mut cell = self.pen.cell;
        cell.data = utf8::replacement();
        screen.collect_add(&cell);
    }

    fn charset_pen(&mut self) {
        let set = if self.pen.set == 0 {
            self.pen.g0
        } else {
            self.pen.g1
        };
        if set {
            self.pen.cell.attr |= ATTR_CHARSET;
        } else {
            self.pen.cell.attr &= !ATTR_CHARSET;
        }
    }

    fn print(&mut self, screen: &mut Screen, ch: u8) {
        self.stop_utf8(screen);
        self.charset_pen();
        self.pen.cell.data = Glyph::ascii(ch);
        let cell = self.pen.cell;
        screen.collect_add(&cell);
        self.last = self.pen.cell.data;
        self.last_printed = true;
        self.pen.cell.attr &= !ATTR_CHARSET;
    }

    fn top_bit_set(&mut self, screen: &mut Screen, ch: u8) {
        self.last_printed = false;
        let Some(decoder) = self.utf8.as_mut() else {
            // A byte that cannot start a character is replaced at once.
            match Decoder::open(ch) {
                Some(decoder) => self.utf8 = Some(decoder),
                None => self.write_replacement(screen),
            }
            return;
        };
        match decoder.append(ch) {
            Decoded::More => {}
            Decoded::Error => self.stop_utf8(screen),
            Decoded::Done(glyph) => {
                self.utf8 = None;
                self.pen.cell.data = glyph;
                let cell = self.pen.cell;
                screen.collect_add(&cell);
                self.last = glyph;
                self.last_printed = true;
            }
        }
    }

    fn c0_dispatch(&mut self, screen: &mut Screen, ch: u8) {
        self.stop_utf8(screen);
        match ch {
            0x08 => screen.backspace(),
            0x09 => screen.tab(),
            0x0a..=0x0c => screen.linefeed(false, self.pen.cell.bg),
            0x0d => screen.carriage_return(),
            0x0e => self.pen.set = 1,
            0x0f => self.pen.set = 0,
            _ => {}
        }
        self.last_printed = false;
    }

    fn save_state(&mut self, screen: &Screen) {
        self.old_pen = self.pen;
        self.old_cx = screen.cx;
        self.old_cy = screen.cy;
        self.old_mode = screen.mode;
    }

    fn restore_state(&mut self, screen: &mut Screen) {
        self.pen = self.old_pen;
        if self.old_mode & MODE_ORIGIN != 0 {
            screen.mode |= MODE_ORIGIN;
        } else {
            screen.mode &= !MODE_ORIGIN;
        }
        screen.cursor_move(Some(self.old_cx), Some(self.old_cy), false);
    }

    fn reset_pen(&mut self) {
        self.pen = DEFAULT_PEN;
        self.old_pen = DEFAULT_PEN;
        self.old_cx = 0;
        self.old_cy = 0;
    }

    fn esc_dispatch(&mut self, screen: &mut Screen, ch: u8) {
        if self.discard {
            return;
        }
        let bg = self.pen.cell.bg;
        match (self.interm.as_slice(), ch) {
            (b"(", b'0') => self.pen.g0 = true,
            (b")", b'0') => self.pen.g1 = true,
            (b"(", b'B') => self.pen.g0 = false,
            (b")", b'B') => self.pen.g1 = false,
            (b"", b'7') => self.save_state(screen),
            (b"", b'8') => self.restore_state(screen),
            (b"#", b'8') => screen.alignment_test(),
            (b"", b'=') => screen.mode |= MODE_KKEYPAD,
            (b"", b'>') => screen.mode &= !MODE_KKEYPAD,
            (b"", b'D') => screen.linefeed(false, bg),
            (b"", b'E') => {
                screen.carriage_return();
                screen.linefeed(false, bg);
            }
            (b"", b'H') => {
                if screen.cx < screen.sx() {
                    screen.set_tab(screen.cx, true);
                }
            }
            (b"", b'M') => screen.reverse_index(bg),
            (b"", b'\\') => {}
            (b"", b'c') => {
                self.reset_pen();
                screen.reset();
            }
            _ => return,
        }
        self.last_printed = false;
    }

    /// `input_split`: the parameter list, or `None` when it is invalid.
    fn split(&mut self) -> bool {
        self.params.clear();
        if self.param.is_empty() {
            return true;
        }
        for part in self.param.split(|byte| *byte == b';') {
            let param = if part.is_empty() {
                Param::Missing
            } else if part.contains(&b':') {
                Param::Text(String::from_utf8_lossy(part).into_owned())
            } else {
                match number(part) {
                    Some(value) => Param::Number(value),
                    None => return false,
                }
            };
            self.params.push(param);
            if self.params.len() == PARAM_LIST_MAX {
                return false;
            }
        }
        true
    }

    /// `input_get`: a parameter, its default, or -1 for a sub-parameter list.
    fn get(&self, index: usize, minval: i32, defval: i32) -> i32 {
        match self.params.get(index) {
            None | Some(Param::Missing) => defval,
            Some(Param::Text(_)) => -1,
            Some(Param::Number(value)) => (*value).max(minval),
        }
    }

    fn count(&self, index: usize) -> Option<usize> {
        let value = self.get(index, 1, 1);
        (value != -1).then_some(value as usize)
    }

    fn csi_dispatch(&mut self, screen: &mut Screen, ch: u8) {
        if self.discard || !self.split() {
            return;
        }
        let bg = self.pen.cell.bg;
        let interm = self.interm.clone();
        match (interm.as_slice(), ch) {
            (b"", b'Z') => {
                if let Some(n) = self.count(0) {
                    screen.back_tab(n);
                }
            }
            (b"", b'D') => {
                if let Some(n) = self.count(0) {
                    screen.cursor_left(n);
                }
            }
            (b"", b'B') => {
                if let Some(n) = self.count(0) {
                    screen.cursor_down(n);
                }
            }
            (b"", b'C') => {
                if let Some(n) = self.count(0) {
                    screen.cursor_right(n);
                }
            }
            (b"", b'H' | b'f') => {
                if let (Some(n), Some(m)) = (self.count(0), self.count(1)) {
                    screen.cursor_move(Some(m - 1), Some(n - 1), true);
                }
            }
            (b">", b'm') | (b">", b'n') => {}
            (b"", b't') => self.winops(screen),
            (b"", b'A') => {
                if let Some(n) = self.count(0) {
                    screen.cursor_up(n);
                }
            }
            (b"", b'E') => {
                if let Some(n) = self.count(0) {
                    screen.carriage_return();
                    screen.cursor_down(n);
                }
            }
            (b"", b'F') => {
                if let Some(n) = self.count(0) {
                    screen.carriage_return();
                    screen.cursor_up(n);
                }
            }
            (b"", b'c') => {
                if self.get(0, 0, 0) == 0 {
                    self.replies.extend_from_slice(b"\x1b[?1;2c");
                }
            }
            (b">", b'c') => {
                if self.get(0, 0, 0) == 0 {
                    self.replies.extend_from_slice(b"\x1b[>84;0;0c");
                }
            }
            (b"", b'n') => match self.get(0, 0, 0) {
                5 => self.replies.extend_from_slice(b"\x1b[0n"),
                6 => self.reply(format!("\x1b[{};{}R", screen.cy + 1, screen.cx + 1)),
                _ => {}
            },
            (b"$", b'p') => {
                let m = self.get(0, 0, 0);
                let n = match m {
                    4 if screen.mode & MODE_INSERT != 0 => 1,
                    4 => 2,
                    _ => 0,
                };
                if m > 0 {
                    self.reply(format!("\x1b[{m};{n}$y"));
                }
            }
            (b"?$", b'p') => {
                let m = self.get(0, 0, 0);
                let n = private_mode_report(screen, m);
                if m > 0 {
                    self.reply(format!("\x1b[?{m};{n}$y"));
                }
            }
            // Theme reports, the terminal version and graphics attributes
            // depend on the terminal outside; the host answers those.
            (b"?", b'n') | (b">", b'q') | (b"?", b'S') => {}
            (b"", b'X') => {
                if let Some(n) = self.count(0) {
                    screen.clear_character(n, bg);
                }
            }
            (b"", b'P') => {
                if let Some(n) = self.count(0) {
                    screen.delete_character(n, bg);
                }
            }
            (b"", b'r') => {
                let n = self.get(0, 1, 1);
                let m = self.get(1, 1, screen.sy() as i32);
                if n != -1 && m != -1 {
                    screen.set_scroll_region(n as usize - 1, m as usize - 1);
                }
            }
            (b"", b'M') => {
                if let Some(n) = self.count(0) {
                    screen.delete_line(n, bg);
                }
            }
            (b"", b'J') => match self.get(0, 0, 0) {
                0 => screen.clear_end_of_screen(bg),
                1 => screen.clear_start_of_screen(bg),
                2 => screen.clear_screen(bg),
                3 => {
                    if self.get(1, 0, 0) == 0 {
                        screen.clear_history();
                    }
                }
                _ => {}
            },
            (b"", b'K') => match self.get(0, 0, 0) {
                0 => screen.clear_end_of_line(bg),
                1 => screen.clear_start_of_line(bg),
                2 => screen.clear_line(bg),
                _ => {}
            },
            (b"", b'G' | b'`') => {
                if let Some(n) = self.count(0) {
                    screen.cursor_move(Some(n - 1), None, true);
                }
            }
            (b"", b'@') => {
                if let Some(n) = self.count(0) {
                    screen.insert_character(n, bg);
                }
            }
            (b"", b'L') => {
                if let Some(n) = self.count(0) {
                    screen.insert_line(n, bg);
                }
            }
            (b"", b'b') => self.repeat(screen),
            (b"", b'u') => self.restore_state(screen),
            (b"", b'l') => self.reset_modes(screen),
            (b"?", b'l') => self.reset_private_modes(screen),
            (b"", b's') => self.save_state(screen),
            (b"", b'm') => self.sgr(),
            (b"", b'h') => self.set_modes(screen),
            (b"?", b'h') => self.set_private_modes(screen),
            (b"", b'S') => {
                if let Some(n) = self.count(0) {
                    screen.scroll_up(n, bg);
                }
            }
            (b"", b'T') => {
                if let Some(n) = self.count(0) {
                    screen.scroll_down(n, bg);
                }
            }
            (b"", b'g') => match self.get(0, 0, 0) {
                0 => {
                    if screen.cx < screen.sx() {
                        screen.set_tab(screen.cx, false);
                    }
                }
                3 => screen.clear_all_tabs(),
                _ => {}
            },
            (b"", b'd') => {
                if let Some(n) = self.count(0) {
                    screen.cursor_move(None, Some(n - 1), true);
                }
            }
            (b" ", b'q') => {
                let n = self.get(0, 0, 0);
                if n != -1 {
                    set_cursor_style(screen, n);
                    if n == 0 {
                        screen.mode &= !MODE_CURSOR_BLINKING_SET;
                    }
                }
            }
            _ => return,
        }
        self.last_printed = false;
    }

    fn reply(&mut self, reply: String) {
        self.replies.extend_from_slice(reply.as_bytes());
    }

    /// `CSI t`: reports of the text area size; title stacks and pixel sizes
    /// (which depend on the terminal outside) are left alone.
    fn winops(&mut self, screen: &Screen) {
        let mut m = 0;
        loop {
            let n = self.get(m, 0, -1);
            match n {
                -1 => return,
                3 | 4 | 8 => {
                    m += 1;
                    if self.get(m, 0, -1) == -1 {
                        return;
                    }
                    m += 1;
                    if self.get(m, 0, -1) == -1 {
                        return;
                    }
                }
                9 | 10 | 22 | 23 => {
                    m += 1;
                    if self.get(m, 0, -1) == -1 {
                        return;
                    }
                }
                18 => self.reply(format!("\x1b[8;{};{}t", screen.sy(), screen.sx())),
                19 => self.reply(format!("\x1b[9;{};{}t", screen.sy(), screen.sx())),
                _ => {}
            }
            m += 1;
        }
    }

    /// `input_dcs_dispatch`: only DECRQSS (`DCS $ q ... ST`) is answered;
    /// passthrough is off, as in tmux by default.
    fn dcs_dispatch(&mut self, screen: &Screen) {
        if self.discard || self.interm != b"$" || self.input.first() != Some(&b'q') {
            return;
        }
        if self.input.len() < 3 || self.input[1] != b' ' || self.input[2] != b'q' {
            self.replies.extend_from_slice(b"\x1bP0$r\x1b\\");
            return;
        }
        let blinking = screen.mode & MODE_CURSOR_BLINKING != 0;
        let ps = match screen.cstyle {
            CursorStyle::Block => 1,
            CursorStyle::Underline => 3,
            CursorStyle::Bar => 5,
            // tmux's default `cursor-style` option.
            CursorStyle::Default => 0,
        };
        let ps = if ps == 0 {
            0
        } else {
            ps + usize::from(!blinking)
        };
        self.reply(format!("\x1bP1$r q{ps} q\x1b\\"));
    }

    /// `CSI b` (REP): repeat the last printed character.
    fn repeat(&mut self, screen: &mut Screen) {
        let Some(n) = self.count(0) else {
            return;
        };
        let m = screen.sx().wrapping_sub(screen.cx) as i64;
        let n = (n as i64).min(m);
        if !self.last_printed {
            return;
        }
        self.charset_pen();
        self.pen.cell.data = self.last;
        let cell = self.pen.cell;
        for _ in 0..n.max(0) {
            screen.collect_add(&cell);
        }
    }

    fn set_modes(&mut self, screen: &mut Screen) {
        for i in 0..self.params.len() {
            match self.get(i, 0, -1) {
                4 => screen.mode |= MODE_INSERT,
                34 => screen.mode &= !MODE_CURSOR_VERY_VISIBLE,
                _ => {}
            }
        }
    }

    fn reset_modes(&mut self, screen: &mut Screen) {
        for i in 0..self.params.len() {
            match self.get(i, 0, -1) {
                4 => screen.mode &= !MODE_INSERT,
                34 => screen.mode |= MODE_CURSOR_VERY_VISIBLE,
                _ => {}
            }
        }
    }

    fn set_private_modes(&mut self, screen: &mut Screen) {
        for i in 0..self.params.len() {
            match self.get(i, 0, -1) {
                1 => screen.mode |= MODE_KCURSOR,
                3 => {
                    screen.cursor_move(Some(0), Some(0), true);
                    screen.clear_screen(self.pen.cell.bg);
                }
                6 => {
                    screen.mode |= MODE_ORIGIN;
                    screen.cursor_move(Some(0), Some(0), true);
                }
                7 => screen.mode |= MODE_WRAP,
                12 => screen.mode |= MODE_CURSOR_BLINKING | MODE_CURSOR_BLINKING_SET,
                25 => screen.mode |= MODE_CURSOR,
                1000 => screen.mode = (screen.mode & !ALL_MOUSE_MODES) | MODE_MOUSE_STANDARD,
                1002 => screen.mode = (screen.mode & !ALL_MOUSE_MODES) | MODE_MOUSE_BUTTON,
                1003 => screen.mode = (screen.mode & !ALL_MOUSE_MODES) | MODE_MOUSE_ALL,
                1004 => screen.mode |= MODE_FOCUSON,
                1005 => screen.mode |= MODE_MOUSE_UTF8,
                1006 => screen.mode |= MODE_MOUSE_SGR,
                47 | 1047 => screen.alternate_on(&self.pen.cell, false),
                1049 => screen.alternate_on(&self.pen.cell, true),
                2004 => screen.mode |= MODE_BRACKETPASTE,
                2031 => screen.mode |= MODE_THEME_UPDATES,
                2026 => screen.mode |= MODE_SYNC,
                _ => {}
            }
        }
    }

    fn reset_private_modes(&mut self, screen: &mut Screen) {
        for i in 0..self.params.len() {
            match self.get(i, 0, -1) {
                1 => screen.mode &= !MODE_KCURSOR,
                3 => {
                    screen.cursor_move(Some(0), Some(0), true);
                    screen.clear_screen(self.pen.cell.bg);
                }
                6 => {
                    screen.mode &= !MODE_ORIGIN;
                    screen.cursor_move(Some(0), Some(0), true);
                }
                7 => screen.mode &= !MODE_WRAP,
                12 => {
                    screen.mode &= !MODE_CURSOR_BLINKING;
                    screen.mode |= MODE_CURSOR_BLINKING_SET;
                }
                25 => screen.mode &= !MODE_CURSOR,
                1000..=1003 => screen.mode &= !ALL_MOUSE_MODES,
                1004 => screen.mode &= !MODE_FOCUSON,
                1005 => screen.mode &= !MODE_MOUSE_UTF8,
                1006 => screen.mode &= !MODE_MOUSE_SGR,
                47 | 1047 => screen.alternate_off(&mut self.pen.cell, false),
                1049 => screen.alternate_off(&mut self.pen.cell, true),
                2004 => screen.mode &= !MODE_BRACKETPASTE,
                2026 => screen.mode &= !MODE_SYNC,
                2031 => screen.mode &= !MODE_THEME_UPDATES,
                _ => {}
            }
        }
    }

    fn sgr_256(&mut self, fgbg: i32, c: i32) {
        let gc = &mut self.pen.cell;
        if c == -1 || c > 255 {
            match fgbg {
                38 => gc.fg = 8,
                48 => gc.bg = 8,
                _ => {}
            }
        } else {
            let value = c as u32 | COLOUR_FLAG_256;
            match fgbg {
                38 => gc.fg = value,
                48 => gc.bg = value,
                58 => gc.us = value,
                _ => {}
            }
        }
    }

    fn sgr_rgb(&mut self, fgbg: i32, r: i32, g: i32, b: i32) -> bool {
        if [r, g, b].iter().any(|v| *v == -1 || *v > 255) {
            return false;
        }
        let value = ((r as u32) << 16) | ((g as u32) << 8) | (b as u32) | COLOUR_FLAG_RGB;
        let gc = &mut self.pen.cell;
        match fgbg {
            38 => gc.fg = value,
            48 => gc.bg = value,
            58 => gc.us = value,
            _ => {}
        }
        true
    }

    /// `input_csi_dispatch_sgr_colon`.
    fn sgr_colon(&mut self, text: &str) {
        let mut p = [-1i32; 8];
        let mut n = 0;
        for part in text.split(':') {
            if !part.is_empty() {
                match number(part.as_bytes()) {
                    Some(value) => p[n] = value,
                    None => return,
                }
            }
            n += 1;
            if n == p.len() {
                return;
            }
        }
        if n == 0 {
            return;
        }
        if p[0] == 4 {
            if n != 2 {
                return;
            }
            let style = match p[1] {
                0 => 0,
                1 => ATTR_UNDERSCORE,
                2 => ATTR_UNDERSCORE_2,
                3 => ATTR_UNDERSCORE_3,
                4 => ATTR_UNDERSCORE_4,
                5 => ATTR_UNDERSCORE_5,
                _ => return,
            };
            self.pen.cell.attr = (self.pen.cell.attr & !ATTR_ALL_UNDERSCORE) | style;
            return;
        }
        if n < 2 || !matches!(p[0], 38 | 48 | 58) {
            return;
        }
        match p[1] {
            2 => {
                if n < 3 {
                    return;
                }
                let i = if n == 5 { 2 } else { 3 };
                if n < i + 3 {
                    return;
                }
                self.sgr_rgb(p[0], p[i], p[i + 1], p[i + 2]);
            }
            5 => {
                if n < 3 {
                    return;
                }
                self.sgr_256(p[0], p[2]);
            }
            _ => {}
        }
    }

    /// `input_csi_dispatch_sgr`.
    fn sgr(&mut self) {
        if self.params.is_empty() {
            self.pen.cell = DEFAULT_CELL;
            return;
        }
        let mut i = 0;
        while i < self.params.len() {
            if let Param::Text(text) = &self.params[i] {
                let text = text.clone();
                self.sgr_colon(&text);
                i += 1;
                continue;
            }
            let n = self.get(i, 0, 0);
            if n == -1 {
                i += 1;
                continue;
            }
            if n == 38 || n == 48 || n == 58 {
                i += 1;
                match self.get(i, 0, -1) {
                    2 => {
                        let (r, g, b) = (
                            self.get(i + 1, 0, -1),
                            self.get(i + 2, 0, -1),
                            self.get(i + 3, 0, -1),
                        );
                        if self.sgr_rgb(n, r, g, b) {
                            i += 3;
                        }
                    }
                    5 => {
                        let c = self.get(i + 1, 0, -1);
                        self.sgr_256(n, c);
                        i += 1;
                    }
                    _ => {}
                }
                i += 1;
                continue;
            }
            let gc = &mut self.pen.cell;
            match n {
                0 => {
                    let link = gc.link;
                    *gc = DEFAULT_CELL;
                    gc.link = link;
                }
                1 => gc.attr |= ATTR_BRIGHT,
                2 => gc.attr |= ATTR_DIM,
                3 => gc.attr |= ATTR_ITALICS,
                4 => gc.attr = (gc.attr & !ATTR_ALL_UNDERSCORE) | ATTR_UNDERSCORE,
                5 | 6 => gc.attr |= ATTR_BLINK,
                7 => gc.attr |= ATTR_REVERSE,
                8 => gc.attr |= ATTR_HIDDEN,
                9 => gc.attr |= ATTR_STRIKETHROUGH,
                21 => gc.attr = (gc.attr & !ATTR_ALL_UNDERSCORE) | ATTR_UNDERSCORE_2,
                22 => gc.attr &= !(ATTR_BRIGHT | ATTR_DIM),
                23 => gc.attr &= !ATTR_ITALICS,
                24 => gc.attr &= !ATTR_ALL_UNDERSCORE,
                25 => gc.attr &= !ATTR_BLINK,
                27 => gc.attr &= !ATTR_REVERSE,
                28 => gc.attr &= !ATTR_HIDDEN,
                29 => gc.attr &= !ATTR_STRIKETHROUGH,
                30..=37 => gc.fg = (n - 30) as u32,
                39 => gc.fg = 8,
                40..=47 => gc.bg = (n - 40) as u32,
                49 => gc.bg = 8,
                53 => gc.attr |= ATTR_OVERLINE,
                55 => gc.attr &= !ATTR_OVERLINE,
                59 => gc.us = 8,
                90..=97 => gc.fg = n as u32,
                100..=107 => gc.bg = (n - 10) as u32,
                _ => {}
            }
            i += 1;
        }
    }

    /// `input_exit_osc`: only hyperlinks (OSC 8) change cells.
    fn exit_osc(&mut self) {
        if self.discard {
            return;
        }
        let p = self.input.clone();
        if p.first().is_none_or(|c| !c.is_ascii_digit()) {
            return;
        }
        let digits = p.iter().take_while(|c| c.is_ascii_digit()).count();
        let option = p[..digits].iter().fold(0u32, |value, digit| {
            value.wrapping_mul(10).wrapping_add(u32::from(digit - b'0'))
        });
        let rest = &p[digits..];
        if !rest.is_empty() && rest[0] != b';' {
            return;
        }
        let rest = rest.strip_prefix(b";").unwrap_or(rest);
        if option == 8 {
            self.osc_8(rest);
        }
    }

    /// `input_osc_8`: start or end a hyperlink.
    fn osc_8(&mut self, p: &[u8]) {
        let mut id: Option<Vec<u8>> = None;
        let mut start = 0;
        let mut end = None;
        while let Some(offset) = p[start..].iter().position(|c| *c == b':' || *c == b';') {
            let at = start + offset;
            let part = &p[start..at];
            if part.len() >= 4 && part.starts_with(b"id=") {
                if id.is_some() {
                    return;
                }
                id = Some(part[3..].to_vec());
            }
            if p[at] == b';' {
                end = Some(at);
                break;
            }
            start = at + 1;
        }
        let Some(end) = end else {
            return;
        };
        let uri = &p[end + 1..];
        if uri.is_empty() {
            self.pen.cell.link = 0;
            return;
        }
        // `hyperlinks_put`: links with an id are shared, anonymous ones never.
        let uri = utf8::stravis(uri);
        let id = utf8::stravis(&id.unwrap_or_default());
        let key = (uri.clone(), id.clone());
        let named = !id.is_empty();
        if let Some(link) = self.links.get(&key).filter(|_| named) {
            self.pen.cell.link = *link;
            return;
        }
        self.targets.push((uri, id));
        let link = self.targets.len() as u32;
        if named {
            self.links.insert(key, link);
        }
        self.pen.cell.link = link;
    }

    /// The URI and id of a hyperlink (`hyperlinks_get`).
    pub(super) fn link_target(&self, link: u32) -> Option<(&[u8], &[u8])> {
        let (uri, id) = self.targets.get((link as usize).checked_sub(1)?)?;
        Some((uri, id))
    }
}

impl Parser {
    /// Repaint the cursor saved with DECSC (position, pen and character
    /// sets) when it differs from the initial state.
    pub(super) fn repaint_saved_cursor(&self, screen: &Screen, out: &mut Vec<u8>) {
        let initial = self.old_cx == 0
            && self.old_cy == 0
            && self.old_pen.cell == DEFAULT_CELL
            && self.old_pen.set == 0
            && !self.old_pen.g0
            && !self.old_pen.g1;
        if initial {
            return;
        }
        let row = self.old_cy.min(screen.sy() - 1);
        let col = self.old_cx.min(screen.sx() - 1);
        out.extend(format!("\x1b[{};{}H", row + 1, col + 1).into_bytes());
        pen_sequences(&self.old_pen, out);
        out.extend_from_slice(b"\x1b7");
    }

    /// Repaint the current drawing attributes and character sets.
    pub(super) fn repaint_pen(&self, out: &mut Vec<u8>) {
        pen_sequences(&self.pen, out);
    }
}

fn pen_sequences(pen: &Pen, out: &mut Vec<u8>) {
    out.extend(super::repaint_sgr(&pen.cell));
    out.extend_from_slice(if pen.g0 { b"\x1b(0" } else { b"\x1b(B" });
    out.extend_from_slice(if pen.g1 { b"\x1b)0" } else { b"\x1b)B" });
    out.push(if pen.set == 1 { 0x0e } else { 0x0f });
}

/// The DECRQM answer for a private mode: 1 set, 2 reset, 0 unknown.
fn private_mode_report(screen: &Screen, mode: i32) -> u8 {
    let flag = |bit: u32| if screen.mode & bit != 0 { 1 } else { 2 };
    match mode {
        1 => flag(MODE_KCURSOR),
        3 => 4,
        6 => flag(MODE_ORIGIN),
        7 => flag(MODE_WRAP),
        12 => {
            if screen.cstyle != CursorStyle::Default || screen.mode & MODE_CURSOR_BLINKING_SET != 0
            {
                flag(MODE_CURSOR_BLINKING)
            } else {
                // tmux's default `cursor-style` option is steady.
                2
            }
        }
        25 => flag(MODE_CURSOR),
        47 | 1047 | 1049 => {
            if screen.is_alternate() {
                1
            } else {
                2
            }
        }
        1000 => flag(MODE_MOUSE_STANDARD),
        1002 => flag(MODE_MOUSE_BUTTON),
        1003 => flag(MODE_MOUSE_ALL),
        1004 => flag(MODE_FOCUSON),
        1005 => flag(MODE_MOUSE_UTF8),
        1006 => flag(MODE_MOUSE_SGR),
        2004 => flag(MODE_BRACKETPASTE),
        2026 => flag(MODE_SYNC),
        2031 => flag(MODE_THEME_UPDATES),
        _ => 0,
    }
}

fn set_cursor_style(screen: &mut Screen, style: i32) {
    let (shape, blinking) = match style {
        0 => {
            screen.cstyle = CursorStyle::Default;
            return;
        }
        1 => (CursorStyle::Block, true),
        2 => (CursorStyle::Block, false),
        3 => (CursorStyle::Underline, true),
        4 => (CursorStyle::Underline, false),
        5 => (CursorStyle::Bar, true),
        6 => (CursorStyle::Bar, false),
        _ => return,
    };
    screen.cstyle = shape;
    if blinking {
        screen.mode |= MODE_CURSOR_BLINKING;
    } else {
        screen.mode &= !MODE_CURSOR_BLINKING;
    }
}

/// `strtonum(s, 0, INT_MAX)` on a run of digits.
fn number(digits: &[u8]) -> Option<i32> {
    if digits.is_empty() || !digits.iter().all(u8::is_ascii_digit) {
        return None;
    }
    let mut value: i64 = 0;
    for digit in digits {
        value = value * 10 + i64::from(digit - b'0');
        if value > i64::from(i32::MAX) {
            return None;
        }
    }
    Some(value as i32)
}

/// The transition tables of input.c.
fn transition(state: State, ch: u8) -> (Action, Option<State>) {
    use Action as A;
    use State as S;
    let anywhere = |ch: u8| match ch {
        0x18 | 0x1a => Some((A::C0, Some(S::Ground))),
        0x1b => Some((A::None, Some(S::EscEnter))),
        _ => None,
    };
    let c0 = |ch: u8| matches!(ch, 0x00..=0x17 | 0x19 | 0x1c..=0x1f);
    match state {
        S::Ground => anywhere(ch).unwrap_or(match ch {
            _ if c0(ch) => (A::C0, None),
            0x20..=0x7e => (A::Print, None),
            0x7f => (A::None, None),
            _ => (A::TopBit, None),
        }),
        S::EscEnter => anywhere(ch).unwrap_or(match ch {
            _ if c0(ch) => (A::C0, None),
            0x20..=0x2f => (A::Intermediate, Some(S::EscIntermediate)),
            0x50 => (A::None, Some(S::DcsEnter)),
            0x58 | 0x5e => (A::None, Some(S::ConsumeSt)),
            0x5b => (A::None, Some(S::CsiEnter)),
            0x5d => (A::None, Some(S::OscString)),
            0x5f => (A::None, Some(S::ApcString)),
            0x6b => (A::None, Some(S::RenameString)),
            0x30..=0x7e => (A::EscDispatch, Some(S::Ground)),
            _ => (A::None, None),
        }),
        S::EscIntermediate => anywhere(ch).unwrap_or(match ch {
            _ if c0(ch) => (A::C0, None),
            0x20..=0x2f => (A::Intermediate, None),
            0x30..=0x7e => (A::EscDispatch, Some(S::Ground)),
            _ => (A::None, None),
        }),
        S::CsiEnter => anywhere(ch).unwrap_or(match ch {
            _ if c0(ch) => (A::C0, None),
            0x20..=0x2f => (A::Intermediate, Some(S::CsiIntermediate)),
            0x30..=0x3b => (A::Parameter, Some(S::CsiParameter)),
            0x3c..=0x3f => (A::Intermediate, Some(S::CsiParameter)),
            0x40..=0x7e => (A::CsiDispatch, Some(S::Ground)),
            _ => (A::None, None),
        }),
        S::CsiParameter => anywhere(ch).unwrap_or(match ch {
            _ if c0(ch) => (A::C0, None),
            0x20..=0x2f => (A::Intermediate, Some(S::CsiIntermediate)),
            0x30..=0x3b => (A::Parameter, None),
            0x3c..=0x3f => (A::None, Some(S::CsiIgnore)),
            0x40..=0x7e => (A::CsiDispatch, Some(S::Ground)),
            _ => (A::None, None),
        }),
        S::CsiIntermediate => anywhere(ch).unwrap_or(match ch {
            _ if c0(ch) => (A::C0, None),
            0x20..=0x2f => (A::Intermediate, None),
            0x30..=0x3f => (A::None, Some(S::CsiIgnore)),
            0x40..=0x7e => (A::CsiDispatch, Some(S::Ground)),
            _ => (A::None, None),
        }),
        S::CsiIgnore => anywhere(ch).unwrap_or(match ch {
            _ if c0(ch) => (A::C0, None),
            0x40..=0x7e => (A::None, Some(S::Ground)),
            _ => (A::None, None),
        }),
        S::DcsEnter => anywhere(ch).unwrap_or(match ch {
            0x20..=0x2f => (A::Intermediate, Some(S::DcsIntermediate)),
            0x30..=0x39 | 0x3b => (A::Parameter, Some(S::DcsParameter)),
            0x3a => (A::None, Some(S::DcsIgnore)),
            0x3c..=0x3f => (A::Intermediate, Some(S::DcsParameter)),
            0x40..=0x7e => (A::Input, Some(S::DcsHandler)),
            _ => (A::None, None),
        }),
        S::DcsParameter => anywhere(ch).unwrap_or(match ch {
            0x20..=0x2f => (A::Intermediate, Some(S::DcsIntermediate)),
            0x30..=0x39 | 0x3b => (A::Parameter, None),
            0x3a | 0x3c..=0x3f => (A::None, Some(S::DcsIgnore)),
            0x40..=0x7e => (A::Input, Some(S::DcsHandler)),
            _ => (A::None, None),
        }),
        S::DcsIntermediate => anywhere(ch).unwrap_or(match ch {
            0x20..=0x2f => (A::Intermediate, None),
            0x30..=0x3f => (A::None, Some(S::DcsIgnore)),
            0x40..=0x7e => (A::Input, Some(S::DcsHandler)),
            _ => (A::None, None),
        }),
        S::DcsHandler => match ch {
            0x1b => (A::None, Some(S::DcsEscape)),
            _ => (A::Input, None),
        },
        S::DcsEscape => match ch {
            0x5c => (A::DcsDispatch, Some(S::Ground)),
            _ => (A::Input, Some(S::DcsHandler)),
        },
        S::DcsIgnore => anywhere(ch).unwrap_or((A::None, None)),
        S::OscString => anywhere(ch).unwrap_or(match ch {
            0x07 => (A::EndBel, Some(S::Ground)),
            0x20..=0xff => (A::Input, None),
            _ => (A::None, None),
        }),
        S::ApcString | S::RenameString => anywhere(ch).unwrap_or(match ch {
            0x20..=0xff => (A::Input, None),
            _ => (A::None, None),
        }),
        S::ConsumeSt => anywhere(ch).unwrap_or((A::None, None)),
    }
}
