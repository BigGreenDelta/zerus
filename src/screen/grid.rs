//! Cell storage with tmux's grid semantics (grid.c, grid-view.c).
//!
//! Lines are allocated lazily and grow in the same quarter, half and full width
//! steps as tmux, because `capture-pane` walks every allocated cell: a styled
//! cell followed by allocated default cells prints a trailing colour reset that
//! an unallocated tail does not. Each stored cell keeps whether tmux would hold
//! it as an "extended" entry, since a plain entry reads back as one byte of
//! width 1 (a padding cell then counts one column when lines are reflowed).

pub(super) const UTF8_SIZE: usize = 32;

pub(super) const COLOUR_FLAG_256: u32 = 0x0100_0000;
pub(super) const COLOUR_FLAG_RGB: u32 = 0x0200_0000;

pub(super) const ATTR_BRIGHT: u16 = 0x1;
pub(super) const ATTR_DIM: u16 = 0x2;
pub(super) const ATTR_UNDERSCORE: u16 = 0x4;
pub(super) const ATTR_BLINK: u16 = 0x8;
pub(super) const ATTR_REVERSE: u16 = 0x10;
pub(super) const ATTR_HIDDEN: u16 = 0x20;
pub(super) const ATTR_ITALICS: u16 = 0x40;
pub(super) const ATTR_CHARSET: u16 = 0x80;
pub(super) const ATTR_STRIKETHROUGH: u16 = 0x100;
pub(super) const ATTR_UNDERSCORE_2: u16 = 0x200;
pub(super) const ATTR_UNDERSCORE_3: u16 = 0x400;
pub(super) const ATTR_UNDERSCORE_4: u16 = 0x800;
pub(super) const ATTR_UNDERSCORE_5: u16 = 0x1000;
pub(super) const ATTR_OVERLINE: u16 = 0x2000;
pub(super) const ATTR_ALL_UNDERSCORE: u16 =
    ATTR_UNDERSCORE | ATTR_UNDERSCORE_2 | ATTR_UNDERSCORE_3 | ATTR_UNDERSCORE_4 | ATTR_UNDERSCORE_5;

pub(super) const FLAG_FG256: u8 = 0x1;
pub(super) const FLAG_BG256: u8 = 0x2;
pub(super) const FLAG_PADDING: u8 = 0x4;
pub(super) const FLAG_CLEARED: u8 = 0x40;
pub(super) const FLAG_TAB: u8 = 0x80;

pub(super) const LINE_WRAPPED: u8 = 0x1;
pub(super) const LINE_EXTENDED: u8 = 0x2;
pub(super) const LINE_DEAD: u8 = 0x4;

pub(super) fn colour_is_default(colour: u32) -> bool {
    colour == 8 || colour == 9
}

/// The UTF-8 text of one cell: a character plus any combined characters.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub(super) struct Glyph {
    pub(super) bytes: [u8; UTF8_SIZE],
    pub(super) size: u8,
    pub(super) width: u8,
}

impl Glyph {
    pub(super) const fn ascii(byte: u8) -> Self {
        let mut bytes = [0; UTF8_SIZE];
        bytes[0] = byte;
        Self {
            bytes,
            size: 1,
            width: 1,
        }
    }

    pub(super) fn tab(width: u8) -> Self {
        let mut bytes = [0; UTF8_SIZE];
        let size = usize::from(width).min(UTF8_SIZE);
        bytes[..size].fill(b' ');
        Self {
            bytes,
            size: width,
            width,
        }
    }

    pub(super) fn as_bytes(&self) -> &[u8] {
        &self.bytes[..usize::from(self.size).min(UTF8_SIZE)]
    }
}

/// A cell as tmux's `grid_get_cell` returns it.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub(super) struct Cell {
    pub(super) data: Glyph,
    pub(super) attr: u16,
    pub(super) flags: u8,
    pub(super) fg: u32,
    pub(super) bg: u32,
    pub(super) us: u32,
    pub(super) link: u32,
}

pub(super) const DEFAULT_CELL: Cell = Cell {
    data: Glyph::ascii(b' '),
    attr: 0,
    flags: 0,
    fg: 8,
    bg: 8,
    us: 8,
    link: 0,
};

const PADDING_CELL: Cell = Cell {
    data: Glyph {
        bytes: padding_bytes(),
        size: 0,
        width: 0,
    },
    attr: 0,
    flags: FLAG_PADDING,
    fg: 8,
    bg: 8,
    us: 8,
    link: 0,
};

const fn padding_bytes() -> [u8; UTF8_SIZE] {
    let mut bytes = [0; UTF8_SIZE];
    bytes[0] = b'!';
    bytes
}

const CLEARED_CELL: Cell = Cell {
    flags: FLAG_CLEARED,
    ..DEFAULT_CELL
};

impl Cell {
    /// Whether two cells look the same (`grid_cells_look_equal`).
    pub(super) fn looks_equal(&self, other: &Cell) -> bool {
        self.fg == other.fg
            && self.bg == other.bg
            && self.attr == other.attr
            && (self.flags & !FLAG_CLEARED) == (other.flags & !FLAG_CLEARED)
            && self.link == other.link
    }

    /// Whether two cells are identical (`grid_cells_equal`).
    pub(super) fn equals(&self, other: &Cell) -> bool {
        self.looks_equal(other)
            && self.data.width == other.data.width
            && self.data.size == other.data.size
            && self.data.as_bytes() == other.data.as_bytes()
    }

    /// Turn this cell into a tab of the given width (`grid_set_tab`).
    pub(super) fn set_tab(&mut self, width: u8) {
        self.flags |= FLAG_TAB;
        self.flags &= !FLAG_PADDING;
        self.data = Glyph::tab(width);
    }
}

#[derive(Clone, Copy, Debug)]
pub(super) struct Entry {
    pub(super) cell: Cell,
    pub(super) extended: bool,
}

const CLEARED_ENTRY: Entry = Entry {
    cell: CLEARED_CELL,
    extended: false,
};

#[derive(Clone, Default, Debug)]
pub(super) struct Line {
    pub(super) cells: Vec<Entry>,
    pub(super) cellused: usize,
    pub(super) flags: u8,
}

impl Line {
    pub(super) fn cellsize(&self) -> usize {
        self.cells.len()
    }

    /// `grid_get_cell1`: the cell at an allocated position.
    pub(super) fn cell(&self, px: usize) -> Cell {
        self.cells.get(px).map_or(DEFAULT_CELL, |entry| entry.cell)
    }

    fn need_extended(entry: &Entry, gc: &Cell) -> bool {
        entry.extended
            || gc.attr > 0xff
            || gc.data.size > 1
            || gc.data.width > 1
            || gc.fg & COLOUR_FLAG_RGB != 0
            || gc.bg & COLOUR_FLAG_RGB != 0
            || gc.us != 8
            || gc.link != 0
            || gc.flags & FLAG_TAB != 0
    }

    /// Store a cell the way tmux does and keep what it would read back.
    fn store(&mut self, px: usize, gc: &Cell) {
        let entry = &mut self.cells[px];
        if Self::need_extended(entry, gc) {
            let mut cell = *gc;
            cell.flags &= !FLAG_CLEARED;
            if cell.flags & FLAG_TAB != 0 {
                cell.data = Glyph::tab(gc.data.width);
            }
            *entry = Entry {
                cell,
                extended: true,
            };
            self.flags |= LINE_EXTENDED;
        } else {
            let cell = Cell {
                data: Glyph::ascii(gc.data.bytes[0]),
                attr: gc.attr & 0xff,
                flags: gc.flags & !FLAG_CLEARED,
                fg: (gc.fg & 0xff) | (gc.fg & COLOUR_FLAG_256),
                bg: (gc.bg & 0xff) | (gc.bg & COLOUR_FLAG_256),
                us: 8,
                link: 0,
            };
            *entry = Entry {
                cell,
                extended: false,
            };
        }
    }

    /// `grid_clear_cell`.
    fn clear_cell(&mut self, px: usize, bg: u32, moved: bool) {
        let had_extended = self.cells[px].extended;
        if !moved && had_extended {
            let mut cell = CLEARED_CELL;
            cell.flags = 0;
            if bg != 8 {
                cell.bg = bg;
            }
            self.cells[px] = Entry {
                cell,
                extended: true,
            };
            self.flags |= LINE_EXTENDED;
        } else if bg != 8 && bg & COLOUR_FLAG_RGB != 0 {
            let mut cell = CLEARED_CELL;
            cell.flags = 0;
            cell.bg = bg;
            self.cells[px] = Entry {
                cell,
                extended: true,
            };
            self.flags |= LINE_EXTENDED;
        } else {
            let mut entry = CLEARED_ENTRY;
            if bg != 8 {
                entry.cell.bg = (bg & 0xff) | (bg & COLOUR_FLAG_256);
            }
            self.cells[px] = entry;
        }
    }

    /// The raw comparison `screen_write_cell` uses to skip an unchanged cell.
    pub(super) fn same_plain_entry(&self, px: usize, gc: &Cell) -> bool {
        let entry = &self.cells[px];
        if entry.extended {
            return false;
        }
        let stored = &entry.cell;
        let mut raw_flags = stored.flags;
        if stored.fg & COLOUR_FLAG_256 != 0 {
            raw_flags |= FLAG_FG256;
        }
        if stored.bg & COLOUR_FLAG_256 != 0 {
            raw_flags |= FLAG_BG256;
        }
        u32::from(gc.flags) == u32::from(raw_flags)
            && gc.attr == stored.attr
            && gc.fg == stored.fg & 0xff
            && gc.bg == stored.bg & 0xff
            && gc.data.width == 1
            && gc.data.size == 1
            && stored.data.bytes[0] == gc.data.bytes[0]
    }
}

#[derive(Clone, Debug)]
pub(super) struct Grid {
    pub(super) sx: usize,
    pub(super) sy: usize,
    pub(super) hsize: usize,
    pub(super) hscrolled: usize,
    pub(super) hlimit: usize,
    pub(super) history: bool,
    pub(super) lines: Vec<Line>,
}

impl Grid {
    pub(super) fn new(sx: usize, sy: usize, hlimit: usize) -> Self {
        Self {
            sx,
            sy,
            hsize: 0,
            hscrolled: 0,
            hlimit,
            history: hlimit != 0,
            lines: vec![Line::default(); sy],
        }
    }

    fn valid_y(&self, py: usize) -> bool {
        py < self.hsize + self.sy && py < self.lines.len()
    }

    pub(super) fn line(&self, py: usize) -> &Line {
        &self.lines[py]
    }

    pub(super) fn line_mut(&mut self, py: usize) -> &mut Line {
        &mut self.lines[py]
    }

    /// `grid_get_cell`.
    pub(super) fn get_cell(&self, px: usize, py: usize) -> Cell {
        if !self.valid_y(py) {
            return DEFAULT_CELL;
        }
        self.lines[py].cell(px)
    }

    /// `grid_expand_line`.
    pub(super) fn expand_line(&mut self, py: usize, sx: usize, bg: u32) {
        let gsx = self.sx;
        let line = &mut self.lines[py];
        if sx <= line.cellsize() {
            return;
        }
        let size = if sx < gsx / 4 {
            gsx / 4
        } else if sx < gsx / 2 {
            gsx / 2
        } else if gsx > sx {
            gsx
        } else {
            sx
        };
        let old = line.cellsize();
        line.cells.resize(
            size,
            Entry {
                cell: DEFAULT_CELL,
                extended: false,
            },
        );
        for xx in old..size {
            line.clear_cell(xx, bg, false);
        }
    }

    /// `grid_empty_line`.
    pub(super) fn empty_line(&mut self, py: usize, bg: u32) {
        self.lines[py] = Line::default();
        if !colour_is_default(bg) {
            let sx = self.sx;
            self.expand_line(py, sx, bg);
        }
    }

    /// `grid_set_cell`.
    pub(super) fn set_cell(&mut self, px: usize, py: usize, gc: &Cell) {
        if !self.valid_y(py) {
            return;
        }
        self.expand_line(py, px + 1, 8);
        let line = &mut self.lines[py];
        if px + 1 > line.cellused {
            line.cellused = px + 1;
        }
        line.store(px, gc);
    }

    /// `grid_set_padding` (tmux 3.7: a padding cell has no colour).
    pub(super) fn set_padding(&mut self, px: usize, py: usize) {
        self.set_cell(px, py, &PADDING_CELL);
    }

    /// `grid_set_cells`: a run of ASCII characters sharing one style.
    pub(super) fn set_cells(&mut self, px: usize, py: usize, gc: &Cell, text: &[u8]) {
        if !self.valid_y(py) {
            return;
        }
        self.expand_line(py, px + text.len(), 8);
        let line = &mut self.lines[py];
        if px + text.len() > line.cellused {
            line.cellused = px + text.len();
        }
        for (offset, byte) in text.iter().enumerate() {
            let mut cell = *gc;
            cell.data = Glyph::ascii(*byte);
            line.store(px + offset, &cell);
        }
    }

    /// Whether a row index given in tmux's 32-bit unsigned arithmetic exists.
    fn valid_y32(&self, py: u32) -> bool {
        self.valid_y(py as usize)
    }

    /// `grid_clear`. Rows use tmux's 32-bit unsigned arithmetic: callers pass
    /// counts that may have wrapped, as tmux's do.
    pub(super) fn clear(&mut self, px: usize, py: u32, nx: usize, ny: u32, bg: u32) {
        if nx == 0 || ny == 0 {
            return;
        }
        if px == 0 && nx == self.sx {
            self.clear_lines(py, ny, bg);
            return;
        }
        if !self.valid_y32(py) || !self.valid_y32(py.wrapping_add(ny).wrapping_sub(1)) {
            return;
        }
        let end = py.wrapping_add(ny);
        let mut row = py;
        while row < end {
            let yy = row as usize;
            row += 1;
            let sx = self.sx.min(self.lines[yy].cellsize());
            let mut ox = nx;
            if colour_is_default(bg) {
                if px > sx {
                    continue;
                }
                if px + nx > sx {
                    ox = sx - px;
                }
            }
            self.expand_line(yy, px + ox, 8);
            for xx in px..px + ox {
                self.lines[yy].clear_cell(xx, bg, false);
            }
        }
    }

    /// `grid_clear_lines`, in tmux's 32-bit unsigned arithmetic.
    pub(super) fn clear_lines(&mut self, py: u32, ny: u32, bg: u32) {
        if ny == 0 || !self.valid_y32(py) || !self.valid_y32(py.wrapping_add(ny).wrapping_sub(1)) {
            return;
        }
        let end = py.wrapping_add(ny);
        let mut row = py;
        while row < end {
            self.empty_line(row as usize, bg);
            row += 1;
        }
        if py != 0 {
            self.lines[py as usize - 1].flags &= !LINE_WRAPPED;
        }
    }

    /// `grid_move_lines`.
    pub(super) fn move_lines(&mut self, dy: usize, py: usize, ny: usize, bg: u32) {
        if ny == 0 || py == dy {
            return;
        }
        if !self.valid_y(py)
            || !self.valid_y(py + ny - 1)
            || !self.valid_y(dy)
            || !self.valid_y(dy + ny - 1)
        {
            return;
        }
        if dy != 0 {
            self.lines[dy - 1].flags &= !LINE_WRAPPED;
        }
        let moved: Vec<Line> = self.lines[py..py + ny]
            .iter_mut()
            .map(std::mem::take)
            .collect();
        for (offset, line) in moved.into_iter().enumerate() {
            self.lines[dy + offset] = line;
        }
        for yy in py..py + ny {
            if yy < dy || yy >= dy + ny {
                self.empty_line(yy, bg);
            }
        }
        if py != 0 && (py < dy || py >= dy + ny) {
            self.lines[py - 1].flags &= !LINE_WRAPPED;
        }
    }

    /// `grid_move_cells`.
    pub(super) fn move_cells(&mut self, dx: usize, px: usize, py: usize, nx: usize, bg: u32) {
        if nx == 0 || px == dx || !self.valid_y(py) {
            return;
        }
        self.expand_line(py, px + nx, 8);
        self.expand_line(py, dx + nx, 8);
        let line = &mut self.lines[py];
        line.cells.copy_within(px..px + nx, dx);
        if dx + nx > line.cellused {
            line.cellused = dx + nx;
        }
        for xx in px..px + nx {
            if xx >= dx && xx < dx + nx {
                continue;
            }
            line.clear_cell(xx, bg, true);
        }
    }

    /// `grid_collect_history`.
    pub(super) fn collect_history(&mut self) {
        if self.hsize == 0 || self.hsize < self.hlimit {
            return;
        }
        let ny = (self.hlimit / 10).clamp(1, self.hsize);
        self.lines.drain(0..ny);
        self.hsize -= ny;
        if self.hscrolled > self.hsize {
            self.hscrolled = self.hsize;
        }
    }

    /// `grid_scroll_history`: the whole screen scrolls one line into history.
    pub(super) fn scroll_history(&mut self, bg: u32) {
        let yy = self.hsize + self.sy;
        self.lines.truncate(yy);
        self.lines.push(Line::default());
        self.empty_line(yy, bg);
        self.hscrolled += 1;
        self.hsize += 1;
    }

    /// `grid_scroll_history_region`: the top line of a region goes to history.
    pub(super) fn scroll_history_region(&mut self, upper: usize, lower: usize, bg: u32) {
        let line = std::mem::take(&mut self.lines[upper]);
        self.lines.remove(upper);
        self.lines.insert(self.hsize, line);
        // The region below moved up by one; its bottom line is new.
        self.lines.insert(lower + 1, Line::default());
        self.lines.truncate(self.hsize + self.sy + 1);
        self.hscrolled += 1;
        self.hsize += 1;
        self.empty_line(lower + 1, bg);
    }

    /// `grid_clear_history`.
    pub(super) fn clear_history(&mut self) {
        self.lines.drain(0..self.hsize);
        self.hscrolled = 0;
        self.hsize = 0;
    }

    /// `grid_duplicate_lines`: copy lines from another grid.
    pub(super) fn duplicate_lines(&mut self, dy: usize, src: &Grid, sy: usize, ny: usize) {
        let mut ny = ny;
        if dy + ny > self.hsize + self.sy {
            ny = (self.hsize + self.sy).saturating_sub(dy);
        }
        if sy + ny > src.hsize + src.sy {
            ny = (src.hsize + src.sy).saturating_sub(sy);
        }
        for offset in 0..ny {
            self.lines[dy + offset] = src.lines[sy + offset].clone();
        }
    }

    /// The visible line at screen row `y`.
    pub(super) fn view_line(&self, y: usize) -> &Line {
        &self.lines[self.hsize + y]
    }
}
