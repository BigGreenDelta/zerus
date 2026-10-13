//! Screen state and the writing operations of tmux's screen-write.c and
//! screen.c: cursor movement, cell writes with wide-character and combining
//! handling, scrolling, erasing, the alternate screen and resizing.

use super::grid::{
    Cell, Grid, ATTR_CHARSET, DEFAULT_CELL, FLAG_PADDING, FLAG_TAB, LINE_EXTENDED, LINE_WRAPPED,
};
use super::utf8;
use super::Compat;

pub(super) const MODE_CURSOR: u32 = 0x1;
pub(super) const MODE_INSERT: u32 = 0x2;
pub(super) const MODE_KCURSOR: u32 = 0x4;
pub(super) const MODE_KKEYPAD: u32 = 0x8;
pub(super) const MODE_WRAP: u32 = 0x10;
pub(super) const MODE_MOUSE_STANDARD: u32 = 0x20;
pub(super) const MODE_MOUSE_BUTTON: u32 = 0x40;
pub(super) const MODE_CURSOR_BLINKING: u32 = 0x80;
pub(super) const MODE_MOUSE_UTF8: u32 = 0x100;
pub(super) const MODE_MOUSE_SGR: u32 = 0x200;
pub(super) const MODE_BRACKETPASTE: u32 = 0x400;
pub(super) const MODE_FOCUSON: u32 = 0x800;
pub(super) const MODE_MOUSE_ALL: u32 = 0x1000;
pub(super) const MODE_ORIGIN: u32 = 0x2000;
pub(super) const MODE_CURSOR_VERY_VISIBLE: u32 = 0x10000;
pub(super) const MODE_CURSOR_BLINKING_SET: u32 = 0x20000;
pub(super) const MODE_THEME_UPDATES: u32 = 0x80000;
pub(super) const MODE_SYNC: u32 = 0x100000;
pub(super) const ALL_MOUSE_MODES: u32 = MODE_MOUSE_STANDARD | MODE_MOUSE_BUTTON | MODE_MOUSE_ALL;

/// tmux's default `history-limit`.
pub(super) const HISTORY_LIMIT: usize = 2000;

#[derive(Clone, Copy, PartialEq, Eq, Debug, Default)]
pub(super) enum CursorStyle {
    #[default]
    Default,
    Block,
    Underline,
    Bar,
}

#[derive(Clone, Debug)]
pub(super) struct Screen {
    pub(super) grid: Grid,
    pub(super) saved_grid: Option<Grid>,
    saved_cx: Option<usize>,
    saved_cy: Option<usize>,
    saved_cell: Cell,
    saved_history: bool,
    pub(super) cx: usize,
    pub(super) cy: usize,
    pub(super) rupper: usize,
    pub(super) rlower: usize,
    pub(super) mode: u32,
    pub(super) cstyle: CursorStyle,
    tabs: Vec<bool>,
    collect: Vec<u8>,
    collect_cell: Cell,
    pub(super) compat: Compat,
}

impl Screen {
    pub(super) fn new(sx: usize, sy: usize, compat: Compat) -> Self {
        let sx = sx.max(1);
        let sy = sy.max(1);
        let mut screen = Self {
            grid: Grid::new(sx, sy, HISTORY_LIMIT),
            saved_grid: None,
            saved_cx: None,
            saved_cy: None,
            saved_cell: DEFAULT_CELL,
            saved_history: false,
            cx: 0,
            cy: 0,
            rupper: 0,
            rlower: sy - 1,
            mode: MODE_CURSOR | MODE_WRAP,
            cstyle: CursorStyle::Default,
            tabs: Vec::new(),
            collect: Vec::new(),
            collect_cell: DEFAULT_CELL,
            compat,
        };
        screen.reset_tabs();
        screen
    }

    pub(super) fn sx(&self) -> usize {
        self.grid.sx
    }

    pub(super) fn sy(&self) -> usize {
        self.grid.sy
    }

    pub(super) fn is_alternate(&self) -> bool {
        self.saved_grid.is_some()
    }

    fn abs(&self, y: usize) -> usize {
        self.grid.hsize + y
    }

    fn view_cell(&self, x: usize, y: usize) -> Cell {
        self.grid.get_cell(x, self.abs(y))
    }

    fn view_set_cell(&mut self, x: usize, y: usize, gc: &Cell) {
        let py = self.abs(y);
        self.grid.set_cell(x, py, gc);
    }

    fn view_set_padding(&mut self, x: usize, y: usize, bg: u32) {
        let py = self.abs(y);
        if self.compat == Compat::Tmux37 {
            self.grid.set_padding(x, py);
        } else {
            self.grid.set_cell(
                x,
                py,
                &Cell {
                    bg,
                    ..padding_cell()
                },
            );
        }
    }

    /// Erase a cell that a write breaks up: tmux 3.7 resets it, 3.8 keeps
    /// its background (`screen_write_clear_cell`).
    fn erase_broken_cell(&mut self, x: usize, y: usize) {
        let cell = if self.compat == Compat::Tmux37 {
            DEFAULT_CELL
        } else {
            Cell {
                bg: self.view_cell(x, y).bg,
                ..DEFAULT_CELL
            }
        };
        self.view_set_cell(x, y, &cell);
    }

    fn view_clear(&mut self, px: usize, py: usize, nx: usize, ny: u32, bg: u32) {
        let py = self.grid.hsize as u32 + py as u32;
        self.grid.clear(px, py, nx, ny, bg);
    }

    pub(super) fn reset_tabs(&mut self) {
        let sx = self.sx();
        self.tabs = (0..sx).map(|x| x != 0 && x % 8 == 0).collect();
    }

    pub(super) fn tab_at(&self, x: usize) -> bool {
        self.tabs.get(x).copied().unwrap_or(false)
    }

    pub(super) fn set_tab(&mut self, x: usize, value: bool) {
        if let Some(tab) = self.tabs.get_mut(x) {
            *tab = value;
        }
    }

    pub(super) fn clear_all_tabs(&mut self) {
        self.tabs.iter_mut().for_each(|tab| *tab = false);
    }

    /// `screen_write_set_cursor`; `None` keeps a coordinate.
    pub(super) fn set_cursor(&mut self, cx: Option<usize>, cy: Option<usize>) {
        if cx == Some(self.cx) && cy == Some(self.cy) {
            return;
        }
        if let Some(mut cx) = cx {
            if cx > self.sx() {
                cx = self.sx() - 1;
            }
            self.cx = cx;
        }
        if let Some(mut cy) = cy {
            if cy > self.sy() - 1 {
                cy = self.sy() - 1;
            }
            self.cy = cy;
        }
    }

    pub(super) fn cursor_up(&mut self, ny: usize) {
        let mut ny = ny.max(1);
        let (mut cx, mut cy) = (self.cx, self.cy);
        if cy < self.rupper {
            ny = ny.min(cy);
        } else {
            ny = ny.min(cy.wrapping_sub(self.rupper));
        }
        if cx == self.sx() {
            cx -= 1;
        }
        cy -= ny;
        self.set_cursor(Some(cx), Some(cy));
    }

    pub(super) fn cursor_down(&mut self, ny: usize) {
        let mut ny = ny.max(1);
        let (mut cx, mut cy) = (self.cx, self.cy);
        if cy > self.rlower {
            ny = ny.min((self.sy() - 1).wrapping_sub(cy));
        } else {
            ny = ny.min(self.rlower - cy);
        }
        if cx == self.sx() {
            cx -= 1;
        } else if ny == 0 {
            return;
        }
        cy += ny;
        self.set_cursor(Some(cx), Some(cy));
    }

    pub(super) fn cursor_right(&mut self, nx: usize) {
        let mut nx = nx.max(1);
        let limit = (self.sx() - 1).wrapping_sub(self.cx);
        if nx > limit {
            nx = limit;
        }
        if nx == 0 {
            return;
        }
        let cx = self.cx.wrapping_add(nx);
        self.set_cursor(Some(cx), Some(self.cy));
    }

    pub(super) fn cursor_left(&mut self, nx: usize) {
        let nx = nx.max(1).min(self.cx);
        if nx == 0 {
            return;
        }
        self.set_cursor(Some(self.cx - nx), Some(self.cy));
    }

    pub(super) fn backspace(&mut self) {
        let (mut cx, mut cy) = (self.cx, self.cy);
        if cx == 0 {
            if cy == 0 {
                return;
            }
            let line = self.grid.line(self.abs(cy - 1));
            if line.flags & LINE_WRAPPED != 0 {
                cy -= 1;
                cx = self.sx() - 1;
            }
        } else {
            cx -= 1;
        }
        self.set_cursor(Some(cx), Some(cy));
    }

    /// `screen_write_cursormove`.
    pub(super) fn cursor_move(&mut self, px: Option<usize>, py: Option<usize>, origin: bool) {
        let mut py = py;
        if origin && self.mode & MODE_ORIGIN != 0 {
            if let Some(y) = py {
                py = Some(if y > self.rlower - self.rupper {
                    self.rlower
                } else {
                    y + self.rupper
                });
            }
        }
        let px = px.map(|x| x.min(self.sx() - 1));
        let py = py.map(|y| y.min(self.sy() - 1));
        self.set_cursor(px, py);
    }

    pub(super) fn carriage_return(&mut self) {
        self.set_cursor(Some(0), None);
    }

    pub(super) fn alignment_test(&mut self) {
        let mut cell = DEFAULT_CELL;
        cell.data = super::grid::Glyph::ascii(b'E');
        for yy in 0..self.sy() {
            for xx in 0..self.sx() {
                self.view_set_cell(xx, yy, &cell);
            }
        }
        self.set_cursor(Some(0), Some(0));
        self.rupper = 0;
        self.rlower = self.sy() - 1;
    }

    fn view_insert_cells(&mut self, px: usize, py: usize, nx: usize, bg: u32) {
        let sx = self.sx();
        let y = self.abs(py);
        if px >= sx - 1 {
            self.grid.clear(px, y as u32, 1, 1, bg);
        } else {
            self.grid
                .move_cells(px + nx, px, y, sx.wrapping_sub(px).wrapping_sub(nx), bg);
        }
    }

    fn character_count(&self, nx: usize) -> Option<usize> {
        let nx = nx.max(1).min(self.sx().wrapping_sub(self.cx));
        if nx == 0 || self.cx > self.sx() - 1 {
            return None;
        }
        Some(nx)
    }

    pub(super) fn insert_character(&mut self, nx: usize, bg: u32) {
        if let Some(nx) = self.character_count(nx) {
            self.view_insert_cells(self.cx, self.cy, nx, bg);
        }
    }

    pub(super) fn delete_character(&mut self, nx: usize, bg: u32) {
        if let Some(nx) = self.character_count(nx) {
            let sx = self.sx();
            let y = self.abs(self.cy);
            self.grid
                .move_cells(self.cx, self.cx + nx, y, sx - self.cx - nx, bg);
            self.grid.clear(sx - nx, y as u32, nx, 1, bg);
        }
    }

    pub(super) fn clear_character(&mut self, nx: usize, bg: u32) {
        if let Some(nx) = self.character_count(nx) {
            self.view_clear(self.cx, self.cy, nx, 1, bg);
        }
    }

    fn view_insert_lines(&mut self, py: usize, ny: usize, bg: u32) {
        let py = self.abs(py);
        let sy = self.abs(self.sy());
        self.grid
            .move_lines(py + ny, py, sy.wrapping_sub(py).wrapping_sub(ny), bg);
    }

    fn view_delete_lines(&mut self, py: usize, ny: usize, bg: u32) {
        let py = self.abs(py);
        let sy = self.abs(self.sy());
        let sx = self.sx();
        self.grid
            .move_lines(py, py + ny, sy.wrapping_sub(py).wrapping_sub(ny), bg);
        self.grid.clear(0, (sy - ny) as u32, sx, ny as u32, bg);
    }

    pub(super) fn insert_line(&mut self, ny: usize, bg: u32) {
        let mut ny = ny.max(1);
        let sy = self.sy();
        if self.cy < self.rupper || self.cy > self.rlower {
            ny = ny.min(sy - self.cy);
            if ny == 0 {
                return;
            }
            self.view_insert_lines(self.cy, ny, bg);
            return;
        }
        ny = ny.min(self.rlower + 1 - self.cy);
        if ny == 0 {
            return;
        }
        let rlower = self.abs(self.rlower);
        let py = self.abs(self.cy);
        let ny2 = rlower + 1 - py - ny;
        self.grid.move_lines(rlower + 1 - ny2, py, ny2, bg);
        let sx = self.sx();
        self.grid.clear(
            0,
            (py + ny2) as u32,
            sx,
            (ny as u32).wrapping_sub(ny2 as u32),
            bg,
        );
    }

    pub(super) fn delete_line(&mut self, ny: usize, bg: u32) {
        let mut ny = ny.max(1);
        let sy = self.sy();
        if self.cy < self.rupper || self.cy > self.rlower {
            ny = ny.min(sy - self.cy);
            if ny == 0 {
                return;
            }
            self.view_delete_lines(self.cy, ny, bg);
            return;
        }
        ny = ny.min(self.rlower + 1 - self.cy);
        if ny == 0 {
            return;
        }
        let rlower = self.abs(self.rlower);
        let py = self.abs(self.cy);
        let ny2 = rlower + 1 - py - ny;
        self.grid.move_lines(py, py + ny, ny2, bg);
        let sx = self.sx();
        self.grid.clear(
            0,
            (py + ny2) as u32,
            sx,
            (ny as u32).wrapping_sub(ny2 as u32),
            bg,
        );
    }

    pub(super) fn clear_line(&mut self, bg: u32) {
        let line = self.grid.line(self.abs(self.cy));
        if line.cellsize() == 0 && super::grid::colour_is_default(bg) {
            return;
        }
        let sx = self.sx();
        self.view_clear(0, self.cy, sx, 1, bg);
    }

    pub(super) fn clear_end_of_line(&mut self, bg: u32) {
        if self.cx == 0 {
            self.clear_line(bg);
            return;
        }
        let sx = self.sx();
        let line = self.grid.line(self.abs(self.cy));
        if self.cx > sx - 1 || (self.cx >= line.cellsize() && super::grid::colour_is_default(bg)) {
            return;
        }
        self.view_clear(self.cx, self.cy, sx - self.cx, 1, bg);
    }

    pub(super) fn clear_start_of_line(&mut self, bg: u32) {
        let sx = self.sx();
        if self.cx >= sx - 1 {
            self.clear_line(bg);
            return;
        }
        self.view_clear(0, self.cy, self.cx + 1, 1, bg);
    }

    pub(super) fn reverse_index(&mut self, bg: u32) {
        if self.cy == self.rupper {
            self.scroll_region_down(bg);
        } else if self.cy > 0 {
            self.set_cursor(None, Some(self.cy - 1));
        }
    }

    fn scroll_region_down(&mut self, bg: u32) {
        let upper = self.abs(self.rupper);
        let lower = self.abs(self.rlower);
        self.grid.move_lines(upper + 1, upper, lower - upper, bg);
    }

    fn scroll_region_up(&mut self, bg: u32) {
        if self.grid.history {
            self.grid.collect_history();
            if self.rupper == 0 && self.rlower == self.sy() - 1 {
                self.grid.scroll_history(bg);
            } else {
                let upper = self.abs(self.rupper);
                let lower = self.abs(self.rlower);
                self.grid.scroll_history_region(upper, lower, bg);
            }
        } else {
            let upper = self.abs(self.rupper);
            let lower = self.abs(self.rlower);
            self.grid.move_lines(upper, upper + 1, lower - upper, bg);
        }
    }

    pub(super) fn set_scroll_region(&mut self, rupper: usize, rlower: usize) {
        let rupper = rupper.min(self.sy() - 1);
        let rlower = rlower.min(self.sy() - 1);
        if rupper >= rlower {
            return;
        }
        self.set_cursor(Some(0), Some(0));
        self.rupper = rupper;
        self.rlower = rlower;
    }

    pub(super) fn linefeed(&mut self, wrapped: bool, bg: u32) {
        if wrapped {
            let py = self.abs(self.cy);
            self.grid.line_mut(py).flags |= LINE_WRAPPED;
        }
        if self.cy == self.rlower {
            self.scroll_region_up(bg);
        } else if self.cy < self.sy() - 1 {
            self.set_cursor(None, Some(self.cy + 1));
        }
    }

    pub(super) fn scroll_up(&mut self, lines: usize, bg: u32) {
        let region = self.rlower - self.rupper + 1;
        let lines = if lines == 0 { 1 } else { lines.min(region) };
        for _ in 0..lines {
            self.scroll_region_up(bg);
        }
    }

    pub(super) fn scroll_down(&mut self, lines: usize, bg: u32) {
        let region = self.rlower - self.rupper + 1;
        let lines = if lines == 0 { 1 } else { lines.min(region) };
        for _ in 0..lines {
            self.scroll_region_down(bg);
        }
    }

    /// `grid_view_clear_history`: `scroll-on-clear` moves used lines to history.
    fn clear_into_history(&mut self, bg: u32) {
        let mut last = 0;
        for yy in 0..self.sy() {
            if self.grid.view_line(yy).cellused != 0 {
                last = yy + 1;
            }
        }
        let (sx, sy) = (self.sx(), self.sy());
        if last == 0 {
            self.view_clear(0, 0, sx, sy as u32, bg);
            return;
        }
        for _ in 0..last {
            self.grid.collect_history();
            self.grid.scroll_history(bg);
        }
        if last < sy {
            self.view_clear(0, 0, sx, (sy - last) as u32, bg);
        }
        self.grid.hscrolled = 0;
    }

    pub(super) fn clear_end_of_screen(&mut self, bg: u32) {
        let (sx, sy) = (self.sx(), self.sy());
        if self.cx == 0 && self.cy == 0 && self.grid.history {
            self.clear_into_history(bg);
        } else {
            if self.cx < sx {
                self.view_clear(self.cx, self.cy, sx - self.cx, 1, bg);
            }
            self.view_clear(
                0,
                self.cy + 1,
                sx,
                (sy as u32).wrapping_sub(self.cy as u32 + 1),
                bg,
            );
        }
    }

    pub(super) fn clear_start_of_screen(&mut self, bg: u32) {
        let sx = self.sx();
        if self.cy > 0 {
            self.view_clear(0, 0, sx, self.cy as u32, bg);
        }
        if self.cx > sx - 1 {
            self.view_clear(0, self.cy, sx, 1, bg);
        } else {
            self.view_clear(0, self.cy, self.cx + 1, 1, bg);
        }
    }

    pub(super) fn clear_screen(&mut self, bg: u32) {
        if self.grid.history {
            self.clear_into_history(bg);
        } else {
            let (sx, sy) = (self.sx(), self.sy());
            self.view_clear(0, 0, sx, sy as u32, bg);
        }
    }

    pub(super) fn clear_history(&mut self) {
        self.grid.clear_history();
    }

    /// `screen_write_reset` (RIS).
    pub(super) fn reset(&mut self) {
        self.reset_tabs();
        let sy = self.sy();
        self.set_scroll_region(0, sy - 1);
        self.mode = MODE_CURSOR | MODE_WRAP;
        self.clear_screen(8);
        self.set_cursor(Some(0), Some(0));
    }

    /// `screen_write_collect_add`.
    pub(super) fn collect_add(&mut self, gc: &Cell) {
        let sx = self.sx();
        let collect = gc.data.width == 1
            && gc.data.size == 1
            && gc.data.bytes[0] < 0x7f
            && gc.flags & FLAG_TAB == 0
            && gc.attr & ATTR_CHARSET == 0
            && self.mode & MODE_WRAP != 0
            && self.mode & MODE_INSERT == 0;
        if !collect {
            self.collect_end();
            self.cell(gc);
            return;
        }
        if self.cx > sx - 1 || self.collect.len() > (sx - 1).wrapping_sub(self.cx) {
            self.collect_end();
        }
        if self.cx > sx - 1 {
            self.linefeed(true, 8);
            self.set_cursor(Some(0), None);
        }
        if self.collect.is_empty() {
            self.collect_cell = *gc;
        }
        self.collect.push(gc.data.bytes[0]);
    }

    /// `screen_write_collect_end`: write collected ASCII to the grid.
    pub(super) fn collect_end(&mut self) {
        if self.collect.is_empty() {
            return;
        }
        let (cx, cy) = (self.cx, self.cy);
        if cx != 0 {
            let mut xx = cx;
            let mut gc = DEFAULT_CELL;
            while xx > 0 {
                gc = self.view_cell(xx, cy);
                if gc.flags & FLAG_PADDING == 0 {
                    break;
                }
                self.erase_broken_cell(xx, cy);
                xx -= 1;
            }
            if xx != cx {
                if xx == 0 {
                    gc = self.view_cell(0, cy);
                }
                if gc.data.width > 1 || gc.flags & FLAG_PADDING != 0 {
                    self.erase_broken_cell(xx, cy);
                }
            }
        }
        let text = std::mem::take(&mut self.collect);
        let py = self.abs(cy);
        let cell = self.collect_cell;
        self.grid.set_cells(cx, py, &cell, &text);
        self.set_cursor(Some(cx + text.len()), None);
        let sx = self.sx();
        let mut xx = self.cx;
        while xx < sx {
            let gc = self.view_cell(xx, cy);
            if gc.flags & FLAG_PADDING == 0 {
                break;
            }
            self.erase_broken_cell(xx, cy);
            xx += 1;
        }
        self.collect = text;
        self.collect.clear();
    }

    /// `screen_write_cell`: write one cell that is not collected.
    pub(super) fn cell(&mut self, gc: &Cell) {
        if gc.flags & FLAG_PADDING != 0 {
            return;
        }
        if self.combine(gc) {
            return;
        }
        let (sx, sy) = (self.sx(), self.sy());
        let width = usize::from(gc.data.width);
        if self.mode & MODE_WRAP == 0
            && width > 1
            && (width > sx || (self.cx != sx && self.cx > sx - width))
        {
            return;
        }
        let mut skip = true;
        if self.mode & MODE_INSERT != 0 {
            self.view_insert_cells(self.cx, self.cy, width, 8);
            skip = false;
        }
        if self.mode & MODE_WRAP != 0 && self.cx > sx.wrapping_sub(width) {
            self.linefeed(true, 8);
            self.set_cursor(Some(0), None);
        }
        if self.cx > sx.wrapping_sub(width) || self.cy > sy - 1 {
            return;
        }
        let (cx, cy) = (self.cx, self.cy);
        let py = self.abs(cy);
        if self.grid.line(py).flags & LINE_EXTENDED != 0 {
            let now = self.view_cell(cx, cy);
            if self.overwrite(&now, width) {
                skip = false;
            }
        }
        for xx in cx + 1..cx + width {
            self.view_set_padding(xx, cy, gc.bg);
            skip = false;
        }
        if skip {
            let line = self.grid.line(py);
            skip = if cx >= line.cellsize() {
                gc.equals(&DEFAULT_CELL)
            } else {
                line.same_plain_entry(cx, gc)
            };
        }
        if !skip {
            self.view_set_cell(cx, cy, gc);
        }
        let not_wrap = usize::from(self.mode & MODE_WRAP == 0);
        if cx <= sx.wrapping_sub(not_wrap).wrapping_sub(width) {
            self.set_cursor(Some(cx + width), None);
        } else {
            self.set_cursor(Some(sx - not_wrap), None);
        }
    }

    /// `screen_write_combine`: attach a zero-width or joining character to
    /// the previous cell. Returns true when the character was consumed.
    fn combine(&mut self, gc: &Cell) -> bool {
        let ud = &gc.data;
        if utf8::is_hangul_filler(ud) {
            return true;
        }
        let mut force_wide = false;
        let zero_width = if utf8::is_zwj(ud) {
            true
        } else if utf8::is_vs(ud) {
            force_wide = true;
            true
        } else {
            ud.width == 0
        };
        let (mut cx, cy) = (self.cx, self.cy);
        if ud.size < 2 || cx == 0 {
            return zero_width;
        }
        let mut n = 1;
        let mut last = self.view_cell(cx - n, cy);
        if cx != 1 && last.flags & FLAG_PADDING != 0 {
            n = 2;
            last = self.view_cell(cx - n, cy);
        }
        if n != usize::from(last.data.width) || last.flags & FLAG_PADDING != 0 {
            return zero_width;
        }
        if !zero_width {
            match utf8::hangul_jamo_state(&last.data, ud) {
                utf8::JamoState::NotComposable => return true,
                utf8::JamoState::Choseong => return false,
                utf8::JamoState::Composable => {}
                utf8::JamoState::NotJamo => {
                    if utf8::should_combine(&last.data, ud) || utf8::should_combine(ud, &last.data)
                    {
                        force_wide = true;
                    } else if !utf8::has_zwj(&last.data) {
                        return false;
                    }
                }
            }
        }
        let total = usize::from(last.data.size) + usize::from(ud.size);
        if total > super::grid::UTF8_SIZE {
            return self.compat != Compat::Tmux37 && zero_width;
        }
        let start = usize::from(last.data.size);
        last.data.bytes[start..total].copy_from_slice(ud.as_bytes());
        last.data.size = total as u8;
        if last.data.width == 1 && force_wide {
            last.data.width = 2;
            n = 2;
            cx += 1;
        } else {
            force_wide = false;
        }
        self.view_set_cell(cx - n, cy, &last);
        if force_wide {
            self.view_set_padding(cx - 1, cy, last.bg);
        }
        // tmux stops before moving the cursor when the combined character
        // does not fit the window (here the size of the pane). tmux 3.7's
        // visible range wraps around when it starts past the right edge.
        let sx = self.sx();
        if cx > sx && (self.compat != Compat::Tmux37 || cx - n <= sx) {
            return true;
        }
        self.set_cursor(Some(cx - n), Some(cy));
        self.set_cursor(Some(cx), Some(cy));
        true
    }

    /// `screen_write_overwrite`: clear the rest of wide characters or tabs
    /// that the new character partly covers.
    fn overwrite(&mut self, gc: &Cell, width: usize) -> bool {
        let (cx, cy, sx) = (self.cx, self.cy, self.sx());
        let mut done = false;
        if gc.flags & FLAG_PADDING != 0 {
            let mut xx = cx + 1;
            loop {
                xx -= 1;
                if xx == 0 {
                    break;
                }
                let tmp = self.view_cell(xx, cy);
                if tmp.flags & FLAG_PADDING == 0 {
                    break;
                }
                self.erase_broken_cell(xx, cy);
            }
            self.erase_broken_cell(xx, cy);
            done = true;
        }
        if width != 1 || gc.data.width != 1 || gc.flags & FLAG_PADDING != 0 {
            let mut xx = cx + width - 1;
            loop {
                xx += 1;
                if xx >= sx {
                    break;
                }
                let tmp = self.view_cell(xx, cy);
                if tmp.flags & FLAG_PADDING == 0 {
                    break;
                }
                if self.compat == Compat::Tmux37 && gc.flags & FLAG_TAB != 0 {
                    let mut space = *gc;
                    space.data = super::grid::Glyph::ascii(b' ');
                    self.view_set_cell(xx, cy, &space);
                } else {
                    self.erase_broken_cell(xx, cy);
                }
                done = true;
            }
        }
        done
    }

    /// `screen_alternate_on`.
    pub(super) fn alternate_on(&mut self, gc: &Cell, cursor: bool) {
        if self.is_alternate() {
            return;
        }
        let (sx, sy) = (self.sx(), self.sy());
        let mut saved = Grid::new(sx, sy, 0);
        saved.duplicate_lines(0, &self.grid, self.grid.hsize, sy);
        self.saved_grid = Some(saved);
        if cursor {
            self.saved_cx = Some(self.cx);
            self.saved_cy = Some(self.cy);
        }
        self.saved_cell = *gc;
        self.view_clear(0, 0, sx, sy as u32, 8);
        self.saved_history = self.grid.history;
        self.grid.history = false;
    }

    /// `screen_alternate_off`.
    pub(super) fn alternate_off(&mut self, gc: &mut Cell, cursor: bool) {
        let (sx, sy) = (self.sx(), self.sy());
        if let Some(saved) = &self.saved_grid {
            let (ssx, ssy) = (saved.sx, saved.sy);
            self.resize(ssx, ssy, false);
        }
        if cursor {
            if let (Some(scx), Some(scy)) = (self.saved_cx, self.saved_cy) {
                self.cx = scx;
                self.cy = scy;
                *gc = self.saved_cell;
            }
        }
        let Some(saved) = self.saved_grid.take() else {
            self.cx = self.cx.min(self.sx() - 1);
            self.cy = self.cy.min(self.sy() - 1);
            return;
        };
        let hsize = self.grid.hsize;
        self.grid.duplicate_lines(hsize, &saved, 0, saved.sy);
        if self.saved_history {
            self.grid.history = true;
        }
        // Back to the current size, reflowing the restored main screen.
        self.resize(sx, sy, true);
        self.cx = self.cx.min(self.sx() - 1);
        self.cy = self.cy.min(self.sy() - 1);
    }

    /// `screen_resize_cursor` with `eat_empty` and `cursor` set.
    pub(super) fn resize(&mut self, sx: usize, sy: usize, reflow: bool) {
        let sx = sx.max(1);
        let sy = sy.max(1);
        let mut cx = self.cx;
        let mut cy = self.grid.hsize + self.cy;
        let mut reflow = reflow;
        if sx != self.sx() {
            self.grid.sx = sx;
            self.reset_tabs();
        } else {
            reflow = false;
        }
        if sy != self.sy() {
            self.resize_y(sy, &mut cy);
        }
        if reflow {
            let (wx, wy) = super::reflow::wrap_position(&self.grid, cx, cy);
            super::reflow::reflow(&mut self.grid, sx);
            (cx, cy) = super::reflow::unwrap_position(&self.grid, wx, wy);
        }
        if cy >= self.grid.hsize {
            self.cx = cx;
            self.cy = cy - self.grid.hsize;
        } else {
            self.cx = 0;
            self.cy = 0;
        }
    }

    /// `screen_resize_y` with `eat_empty` set.
    fn resize_y(&mut self, sy: usize, cy: &mut usize) {
        let oldy = self.sy();
        if sy < oldy {
            let mut needed = oldy - sy;
            // A cursor restored by DECRST 1049 can lie below the screen here;
            // tmux's unsigned arithmetic then deletes all it needs.
            let mut available = (oldy - 1).wrapping_sub(self.cy);
            if available > 0 {
                available = available.min(needed);
                self.view_delete_lines(oldy - available, available, 8);
            }
            needed -= available;
            available = self.cy;
            if self.grid.history {
                self.grid.hscrolled += needed;
                self.grid.hsize += needed;
            } else if needed > 0 && available > 0 {
                available = available.min(needed);
                self.view_delete_lines(0, available, 8);
                *cy -= available;
            }
        }
        let total = self.grid.hsize + sy;
        self.grid.lines.resize(total, super::grid::Line::default());
        if sy > oldy {
            let mut needed = sy - oldy;
            let mut available = self.grid.hscrolled;
            if self.grid.history && available > 0 {
                available = available.min(needed);
                self.grid.hscrolled -= available;
                self.grid.hsize -= available;
            } else {
                available = 0;
            }
            needed -= available;
            let hsize = self.grid.hsize;
            for i in hsize + sy - needed..hsize + sy {
                self.grid.empty_line(i, 8);
            }
        }
        self.grid.sy = sy;
        self.rupper = 0;
        self.rlower = sy - 1;
    }

    /// HT as `input_c0_dispatch` handles it: a tab over blank cells of one
    /// style becomes a tab cell (captured as `\t`), otherwise the cursor moves.
    pub(super) fn tab(&mut self) {
        let sx = self.sx();
        let mut cx = self.cx;
        if cx >= sx - 1 {
            return;
        }
        let first = self.view_cell(cx, self.cy);
        let mut has_content = false;
        loop {
            if !has_content {
                let gc = self.view_cell(cx, self.cy);
                if gc.data.size != 1 || gc.data.bytes[0] != b' ' || !gc.looks_equal(&first) {
                    has_content = true;
                }
            }
            cx += 1;
            if self.tab_at(cx) {
                break;
            }
            if cx >= sx - 1 {
                break;
            }
        }
        let width = cx - self.cx;
        if has_content || width > super::grid::UTF8_SIZE {
            self.cx = cx;
        } else {
            let mut gc = self.view_cell(self.cx, self.cy);
            gc.set_tab(width as u8);
            self.collect_add(&gc);
        }
    }

    /// `CSI Z`: back to the previous tab stop, n times.
    pub(super) fn back_tab(&mut self, n: usize) {
        let mut cx = self.cx.min(self.sx() - 1);
        let mut n = n;
        while cx > 0 && n > 0 {
            n -= 1;
            loop {
                cx -= 1;
                if cx == 0 || self.tab_at(cx) {
                    break;
                }
            }
        }
        self.cx = cx;
    }

    pub(super) fn saved_cursor(&self) -> (Option<usize>, Option<usize>) {
        (self.saved_cx, self.saved_cy)
    }

    pub(super) fn saved_pen_cell(&self) -> Cell {
        self.saved_cell
    }
}

fn padding_cell() -> Cell {
    let mut cell = DEFAULT_CELL;
    cell.flags = FLAG_PADDING;
    cell.data.size = 0;
    cell.data.width = 0;
    cell.data.bytes[0] = b'!';
    cell
}
