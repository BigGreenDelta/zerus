//! Line reflow on a width change (tmux grid.c `grid_reflow` and helpers),
//! including history lines, so a later resize pulls back the same lines.

use super::grid::{Grid, Line, LINE_DEAD, LINE_EXTENDED, LINE_WRAPPED};

fn dead() -> Line {
    Line {
        flags: LINE_DEAD,
        ..Line::default()
    }
}

fn target_grid(sx: usize) -> Grid {
    let mut target = Grid::new(sx, 0, 0);
    target.lines.clear();
    target
}

/// `grid_reflow_add`: append empty lines, returning the first new index.
fn add(target: &mut Grid, n: usize) -> usize {
    let first = target.sy;
    target.lines.truncate(first);
    target.lines.extend((0..n).map(|_| Line::default()));
    target.sy += n;
    first
}

/// `grid_reflow_move`: move a source line across unchanged.
fn move_line(target: &mut Grid, gd: &mut Grid, yy: usize) -> usize {
    let to = add(target, 1);
    target.lines[to] = std::mem::replace(&mut gd.lines[yy], dead());
    to
}

fn width(line: &Line, px: usize) -> usize {
    usize::from(line.cell(px).data.width)
}

/// `grid_reflow_join`: pull following wrapped lines onto this one.
fn join(
    target: &mut Grid,
    gd: &mut Grid,
    sx: usize,
    yy: usize,
    mut width_now: usize,
    already: bool,
) {
    let to = if already {
        target.sy - 1
    } else {
        move_line(target, gd, yy)
    };
    let mut at = target.lines[to].cellused;
    let mut lines = 0;
    let mut wrapped = true;
    let mut from = None;
    let mut want = 0;
    loop {
        if yy + 1 + lines == gd.hsize + gd.sy {
            break;
        }
        let line = yy + 1 + lines;
        if gd.lines[line].flags & LINE_WRAPPED == 0 {
            wrapped = false;
        }
        if gd.lines[line].cellused == 0 {
            if !wrapped {
                break;
            }
            lines += 1;
            continue;
        }
        let gc = gd.lines[line].cell(0);
        if width_now + usize::from(gc.data.width) > sx {
            break;
        }
        width_now += usize::from(gc.data.width);
        target.set_cell(at, to, &gc);
        at += 1;
        from = Some(line);
        want = 1;
        while want < gd.lines[line].cellused {
            let gc = gd.lines[line].cell(want);
            if width_now + usize::from(gc.data.width) > sx {
                break;
            }
            width_now += usize::from(gc.data.width);
            target.set_cell(at, to, &gc);
            at += 1;
            want += 1;
        }
        lines += 1;
        if !wrapped || want != gd.lines[line].cellused || width_now == sx {
            break;
        }
    }
    let Some(from) = from else {
        return;
    };
    if lines == 0 {
        return;
    }
    let left = gd.lines[from].cellused - want;
    if left != 0 {
        gd.move_cells(0, want, yy + lines, left, 8);
        let partial = &mut gd.lines[from];
        partial.cells.truncate(left);
        partial.cellused = left;
        lines -= 1;
    } else if !wrapped {
        target.lines[to].flags &= !LINE_WRAPPED;
    }
    for i in yy + 1..yy + 1 + lines {
        gd.lines[i] = dead();
    }
    if gd.hscrolled > to + lines {
        gd.hscrolled -= lines;
    } else if gd.hscrolled > to {
        gd.hscrolled = to;
    }
}

/// `grid_reflow_split`: break a line that is too wide into several.
fn split(target: &mut Grid, gd: &mut Grid, sx: usize, yy: usize, at: usize) {
    let used = gd.lines[yy].cellused;
    let flags = gd.lines[yy].flags;
    let lines = if flags & LINE_EXTENDED == 0 {
        1 + (used - 1) / sx
    } else {
        let mut lines = 2;
        let mut w = 0;
        for i in at..used {
            let cell = width(&gd.lines[yy], i);
            if w + cell > sx {
                lines += 1;
                w = 0;
            }
            w += cell;
        }
        lines
    };
    let mut line = target.sy + 1;
    let first = add(target, lines);
    let mut w = 0;
    let mut xx = 0;
    for i in at..used {
        let gc = gd.lines[yy].cell(i);
        if w + usize::from(gc.data.width) > sx {
            target.lines[line].flags |= LINE_WRAPPED;
            line += 1;
            w = 0;
            xx = 0;
        }
        w += usize::from(gc.data.width);
        target.set_cell(xx, line, &gc);
        xx += 1;
    }
    if flags & LINE_WRAPPED != 0 {
        target.lines[line].flags |= LINE_WRAPPED;
    }
    let mut original = std::mem::replace(&mut gd.lines[yy], dead());
    original.cells.truncate(at);
    original.cellused = at;
    original.flags |= LINE_WRAPPED;
    target.lines[first] = original;
    if yy <= gd.hscrolled {
        gd.hscrolled += lines - 1;
    }
    if w < sx && flags & LINE_WRAPPED != 0 {
        join(target, gd, sx, yy, w, true);
    }
}

/// `grid_reflow`: rewrap every line, history included, to a new width.
pub(super) fn reflow(gd: &mut Grid, sx: usize) {
    let mut target = target_grid(gd.sx);
    let total = gd.hsize + gd.sy;
    for yy in 0..total {
        let line = &gd.lines[yy];
        if line.flags & LINE_DEAD != 0 {
            continue;
        }
        let wrapped = line.flags & LINE_WRAPPED != 0;
        let (mut at, mut full) = (0, 0);
        if line.flags & LINE_EXTENDED == 0 {
            full = line.cellused;
            at = full.min(sx);
        } else {
            for i in 0..line.cellused {
                let cell = width(line, i);
                if at == 0 && full + cell > sx {
                    at = i;
                }
                full += cell;
            }
        }
        if full == sx {
            move_line(&mut target, gd, yy);
        } else if full > sx {
            split(&mut target, gd, sx, yy, at);
        } else if wrapped {
            join(&mut target, gd, sx, yy, full, false);
        } else {
            move_line(&mut target, gd, yy);
        }
    }
    if target.sy < gd.sy {
        let missing = gd.sy - target.sy;
        add(&mut target, missing);
    }
    gd.hsize = target.sy - gd.sy;
    if gd.hscrolled > gd.hsize {
        gd.hscrolled = gd.hsize;
    }
    gd.lines = target.lines;
}

/// `grid_wrap_position`: the cursor as an offset in its unwrapped line.
/// `None` means the cursor is past the used cells of its line.
pub(super) fn wrap_position(gd: &Grid, px: usize, py: usize) -> (Option<usize>, usize) {
    // A cursor restored below the screen would make tmux read past its lines.
    let py = py.min(gd.lines.len() - 1);
    let (mut ax, mut ay) = (0, 0);
    for line in &gd.lines[..py] {
        if line.flags & LINE_WRAPPED != 0 {
            ax += line.cellused;
        } else {
            ax = 0;
            ay += 1;
        }
    }
    if px >= gd.lines[py].cellused {
        (None, ay)
    } else {
        (Some(ax + px), ay)
    }
}

/// `grid_unwrap_position`.
pub(super) fn unwrap_position(gd: &Grid, wx: Option<usize>, wy: usize) -> (usize, usize) {
    let last = gd.hsize + gd.sy - 1;
    let mut yy = 0;
    let mut ay = 0;
    while yy < last {
        if ay == wy {
            break;
        }
        if gd.lines[yy].flags & LINE_WRAPPED == 0 {
            ay += 1;
        }
        yy += 1;
    }
    match wx {
        None => {
            while yy < last && gd.lines[yy].flags & LINE_WRAPPED != 0 {
                yy += 1;
            }
            (gd.lines[yy].cellused, yy)
        }
        Some(mut wx) => {
            while gd.lines[yy].flags & LINE_WRAPPED != 0 && yy < last {
                if wx < gd.lines[yy].cellused {
                    break;
                }
                wx -= gd.lines[yy].cellused;
                yy += 1;
            }
            (wx, yy)
        }
    }
}
