//! Claude AskUserQuestion: verify each native panel before a small input, then
//! acknowledge only the exact tool result in the pinned conversation transcript.
//! Colours compare as xterm-256 indices: on Windows Claude writes truecolour,
//! mapped with chalk's own rule, and ConPTY leaves the hint's spaces uncoloured.
use super::question_terminal::{Native, Transport};
use super::*;
use std::collections::BTreeSet;
use std::fs::File;
use std::io::{Read, Seek, SeekFrom};
use std::time::{Duration, Instant};

fn norm(s: &str) -> String {
    s.split_whitespace().collect::<Vec<_>>().join(" ")
}
/// Claude draws the caret of a focused Other editor itself, as one inverse
/// cell: on a character, or as a space after the last character of a line.
#[derive(Clone, Copy, Debug, PartialEq)]
struct Caret {
    row: usize,
    column: usize,
    line_end: bool,
}
#[derive(Debug, PartialEq)]
enum Panel {
    Question {
        index: usize,
        focused: usize,
        checked: BTreeSet<usize>,
        other: String,
        empty_other: bool,
        caret: Option<Caret>,
    },
    Review {
        answers: Vec<String>,
        focused: usize,
    },
}
impl Panel {
    fn index(&self, count: usize) -> usize {
        match self {
            Self::Question { index, .. } => *index,
            Self::Review { .. } => count,
        }
    }
}
fn numbered(row: &str) -> Option<(usize, bool, &str)> {
    let row = row.trim();
    let focused = row.starts_with('❯');
    let row = row.strip_prefix('❯').unwrap_or(row).trim_start();
    // A hard-newline draft can leave the first editor row empty. Keep the
    // numbered Other row recognizable while later rows still contain text.
    let (number, text) = row
        .split_once(". ")
        .or_else(|| row.strip_suffix('.').map(|number| (number, "")))?;
    Some((number.parse::<usize>().ok()?.checked_sub(1)?, focused, text))
}
fn divider(row: &str) -> bool {
    row.chars().count() > 10 && row.chars().all(|c| c == '─')
}
// Keep the native styling of Other: its empty hint looks identical to a
// literal answer, and Claude decorates committed answers with a green check.
// Unknown styling is retained as text, never treated as an empty input.
#[derive(Clone, Copy, Default)]
struct OtherStyle {
    dim: bool,
    inverse: bool,
    color: Option<u16>,
}
/// The xterm-256 index chalk (ansi-styles `rgbToAnsi256`) writes for a
/// truecolour value. Claude on Windows writes truecolour (spike S1/S3): this
/// rule turns it into the indices it writes under tmux on Linux, which tmux's
/// nearest-colour match does not (`#999999` is 246 here and 247 there). No
/// value is a rounding tie, so the floating-point rounding is exact.
fn ansi256(red: u16, green: u16, blue: u16) -> u16 {
    let round = |value: f64| value.round() as u16;
    if red == green && green == blue {
        return match red {
            0..8 => 16,
            249.. => 231,
            _ => round(f64::from(red - 8) / 247. * 24.) + 232,
        };
    }
    let level = |value: u16| round(f64::from(value) / 255. * 5.);
    16 + 36 * level(red) + 6 * level(green) + level(blue)
}
fn styled_rows(screen: &str) -> Result<Vec<Vec<(char, OtherStyle)>>> {
    let mut rows = vec![Vec::new()];
    let mut style = OtherStyle::default();
    // tmux trims trailing spaces after it has written their styling. A row
    // that ends with inverse switched on lost the caret cell Claude draws
    // after the last character of a line; restore exactly that one cell.
    let mut restyled = false;
    let caret_end = |row: &mut Vec<(char, OtherStyle)>, restyled: bool, style: OtherStyle| {
        if restyled && style.inverse {
            row.push((' ', style));
        }
    };
    let mut chars = screen.chars();
    while let Some(ch) = chars.next() {
        if ch == '\x1b' {
            if chars.next() == Some('[') {
                restyled = true;
                let codes: String = chars.by_ref().take_while(|ch| *ch != 'm').collect();
                let codes: Vec<_> = codes
                    .split(';')
                    .map(|v| {
                        if v.is_empty() {
                            Ok(0)
                        } else {
                            v.parse::<u16>()
                                .map_err(|_| "Unsupported Claude question styling".to_owned())
                        }
                    })
                    .collect::<Result<_>>()?;
                let mut i = 0;
                while i < codes.len() {
                    match codes[i] {
                        0 => style = OtherStyle::default(),
                        2 => style.dim = true,
                        7 => style.inverse = true,
                        22 => style.dim = false,
                        27 => style.inverse = false,
                        39 => style.color = None,
                        30..=37 | 90..=97 => style.color = Some(codes[i]),
                        38 | 48 | 58 => {
                            let length = match codes.get(i + 1) {
                                Some(5) => 2,
                                Some(2) => 4,
                                _ => return Err("Unsupported Claude question color".into()),
                            };
                            if codes.get(i + length).is_none()
                                || codes[i + 2..=i + length].iter().any(|value| *value > 255)
                            {
                                return Err("Incomplete Claude question color".into());
                            }
                            if codes[i] == 38 {
                                style.color = Some(if length == 2 {
                                    codes[i + 2]
                                } else {
                                    ansi256(codes[i + 2], codes[i + 3], codes[i + 4])
                                });
                            }
                            i += length;
                        }
                        _ => {}
                    }
                    i += 1;
                }
            } else {
                // plain() has already validated this bounded OSC 8 hyperlink.
                while let Some(ch) = chars.next() {
                    if ch == '\x07' || (ch == '\x1b' && chars.next() == Some('\\')) {
                        break;
                    }
                }
            }
        } else if ch == '\n' {
            caret_end(rows.last_mut().unwrap(), restyled, style);
            restyled = false;
            rows.push(Vec::new());
        } else {
            restyled = false;
            rows.last_mut().unwrap().push((ch, style));
        }
    }
    caret_end(rows.last_mut().unwrap(), restyled, style);
    let first = rows
        .iter()
        .position(|row| row.iter().any(|(ch, _)| !ch.is_whitespace()))
        .unwrap_or(rows.len());
    let last = rows
        .iter()
        .rposition(|row| row.iter().any(|(ch, _)| !ch.is_whitespace()))
        .map_or(first, |i| i + 1);
    Ok(rows[first..last].to_vec())
}
// Other green text, such as Claude's Edit diff, also maps to 114: only the
// check glyph in that colour is the commit mark.
fn committed_other(row: &[(char, OtherStyle)]) -> bool {
    row.len() >= 2
        && row[row.len() - 2].0 == ' '
        && row
            .last()
            .is_some_and(|(ch, style)| *ch == '✔' && style.color == Some(114))
}
fn empty_hint(rows: &[Vec<(char, OtherStyle)>]) -> bool {
    let [row] = rows else {
        return false;
    };
    // Revisiting a committed single answer retains Claude's green check even
    // after its editor is cleared. It is decoration only in this exact style.
    let row = if committed_other(row) {
        &row[..row.len() - 2]
    } else {
        row.as_slice()
    };
    // ConPTY moves the cursor over blank cells instead of writing them, so
    // the hint's spaces carry no colour on Windows. Only its letters must.
    ["Type something", "Type something."]
        .contains(&row.iter().map(|(ch, _)| ch).collect::<String>().as_str())
        && row.iter().enumerate().all(|(i, (ch, style))| {
            ch.is_whitespace() || style.dim || style.color == Some(246) || (i == 0 && style.inverse)
        })
}
// This checks only text we have just pasted into a proven empty editor. It
// never authorizes reuse of a native draft: wrapping hides boundary spaces.
fn pasted_projection(other: &str, expected: &str) -> bool {
    let mut remaining = expected;
    for (index, row) in other.split('\n').enumerate() {
        if index > 0 {
            remaining = remaining.trim_start();
        }
        let row = row.trim_start();
        let Some(rest) = remaining.strip_prefix(row) else {
            return false;
        };
        remaining = rest;
    }
    remaining.is_empty()
}
fn pasted_editor(panel: &Panel, index: usize, count: usize, text: &str) -> bool {
    matches!(panel,Panel::Question{index:i,focused:f,other,empty_other:false,..}
        if *i==index && *f==count && pasted_projection(other,text))
}
/// Whether wrapped rows show a preset option: its label rows, then its
/// description rows. Some(true) when a single choice also carries Claude's
/// committed-answer mark after the label.
fn option_shown(rows: &[&str], option: &Value, multi: bool) -> Option<bool> {
    let (label, description) = (string(option, "label"), string(option, "description"));
    (1..=rows.len()).find_map(|split| {
        let (head, tail) = rows.split_at(split);
        if !question_terminal::wrapped(tail, description) {
            return None;
        }
        if question_terminal::wrapped(head, label) {
            return Some(false);
        }
        let (last, head) = head.split_last()?;
        let mut head = head.to_vec();
        match last.strip_suffix(" ✔") {
            Some(text) => head.push(text),
            None if *last == "✔" && !head.is_empty() => {}
            None => return None,
        }
        (!multi && question_terminal::wrapped(&head, label)).then_some(true)
    })
}
/// The trimmed plain rows of a Claude screen, as `parse` reads them.
fn panel_rows(screen: &str) -> Option<Vec<String>> {
    let plain = question_terminal::plain(screen).ok()?;
    Some(plain.trim().lines().map(|row| row.trim().to_owned()).collect())
}
/// The top of a question panel: Claude's divider directly above its tab row.
fn panel_top(rows: &[String]) -> bool {
    rows.windows(2).any(|pair| {
        let tab = &pair[1];
        divider(&pair[0])
            && ((tab.starts_with('←') && tab.ends_with('→') && tab.contains("✔ Submit"))
                || tab.starts_with("☐ ")
                || tab.starts_with("☒ "))
    })
}
/// The bottom of a question panel: its key hints, which a narrow terminal may
/// wrap anywhere, or the review's final prompt.
fn panel_bottom(rows: &[String]) -> bool {
    let footer = rows
        .iter()
        .rposition(String::is_empty)
        .map_or(rows, |blank| &rows[blank + 1..]);
    let hints: String = footer.concat().split_whitespace().collect();
    ["Entertoselect", "Esctocancel", "navigate"]
        .iter()
        .all(|hint| hints.contains(hint))
        || rows.iter().any(|row| row == "Ready to submit your answers?")
}
/// A question panel taller than its pane: Claude keeps no scrollback for it, so
/// its bottom shows while its top does not.
pub(super) fn clipped(screen: &str) -> bool {
    panel_rows(screen).is_some_and(|rows| panel_bottom(&rows) && !panel_top(&rows))
}
/// A question panel whose top and bottom both show.
pub(super) fn complete(screen: &str) -> bool {
    panel_rows(screen).is_some_and(|rows| panel_bottom(&rows) && panel_top(&rows))
}
pub(super) fn same_panel(first: &str, second: &str, card: &Value) -> bool {
    matches!((parse(first,card),parse(second,card)),(Ok(a),Ok(b)) if a==b)
}
fn parse(screen: &str, card: &Value) -> Result<Panel> {
    let plain = question_terminal::plain(screen)?;
    let rows: Vec<_> = plain.trim().lines().map(str::trim).collect();
    let styled = styled_rows(screen)?;
    if styled.len() != rows.len() {
        return Err("Unrecognized Claude question styling".into());
    }
    let items = card["questions"]
        .as_array()
        .ok_or("Missing Claude question schema")?;
    if items.is_empty() {
        return Err("Missing Claude questions".into());
    }
    let single_header = if string(&items[0], "header").is_empty() {
        "Q1"
    } else {
        string(&items[0], "header")
    };
    let start = rows
        .iter()
        .rposition(|r| {
            (r.starts_with('←') && r.ends_with('→') && r.contains("✔ Submit"))
                || (items.len() == 1
                    && [format!("☐ {single_header}"), format!("☒ {single_header}")]
                        .contains(&r.to_string()))
        })
        .ok_or("Claude's question is not fully visible. Open Terminal to check it.")?;
    if start == 0 || !divider(rows[start - 1]) {
        return Err("Unrecognized Claude question panel".into());
    }
    let rows = &rows[start + 1..];
    let styled = &styled[start + 1..];
    let title = rows
        .iter()
        .position(|r| !r.is_empty())
        .ok_or("Empty Claude question panel")?;
    if rows[title] == "Review your answers" {
        let end = rows
            .iter()
            .position(|r| *r == "Ready to submit your answers?")
            .ok_or("Claude's answer review is incomplete")?;
        // Keep Claude's wrapped rows: each boundary may be a space or a split
        // word, so only the request's exact text can match them.
        let mut seen: Vec<(Vec<&str>, Vec<&str>)> = Vec::new();
        let mut in_answer = false;
        let mut framed = false;
        for row in &rows[title + 1..end] {
            // Claude frames each question separately: a gutter marks the
            // heading and wrapped rows of a long one only. Answer text is not
            // decorated: a literal bar in a wrapped custom answer must survive
            // the final exact comparison.
            let unframed = row.strip_prefix("│ ").map(str::trim_start);
            if let Some(text) = unframed.unwrap_or(row).strip_prefix("● ") {
                framed = unframed.is_some();
                seen.push((vec![text], Vec::new()));
                in_answer = false;
            } else if let Some(text) = row.strip_prefix("→ ") {
                seen.last_mut().ok_or("Unexpected review answer")?.1 = vec![text];
                in_answer = true;
            } else if !row.is_empty() {
                let item = seen
                    .last_mut()
                    .ok_or("Not all Claude questions have answers")?;
                let row = match unframed {
                    Some(text) if framed && !in_answer => text,
                    _ => row,
                };
                if in_answer { &mut item.1 } else { &mut item.0 }.push(row);
            }
        }
        if seen.len() != items.len()
            || seen
                .iter()
                .zip(items)
                .any(|((q, _), item)| !question_terminal::wrapped(q, string(item, "question")))
        {
            return Err("Claude's review does not match this question request".into());
        }
        let options: Vec<_> = rows[end + 1..].iter().filter(|r| !r.is_empty()).collect();
        if options.len() != 2 {
            return Err("Unrecognized Claude review controls".into());
        }
        let mut focus = None;
        for (index, label) in ["Submit answers", "Cancel"].iter().enumerate() {
            let (number, selected, text) =
                numbered(options[index]).ok_or("Unknown review choice")?;
            if number != index || text != *label || (selected && focus.replace(index).is_some()) {
                return Err("Unknown review choice".into());
            }
        }
        return Ok(Panel::Review {
            answers: seen
                .into_iter()
                .map(|(_, rows)| {
                    rows.iter()
                        .map(|row| norm(row))
                        .collect::<Vec<_>>()
                        .join("\n")
                })
                .collect(),
            focused: focus.ok_or("Missing review selection")?,
        });
    }
    // Ink wraps the key hints like any other text: they are the last block of
    // rows, and a narrow terminal can split them anywhere.
    let footer = rows
        .iter()
        .rposition(|row| row.is_empty())
        .map_or(rows, |blank| &rows[blank + 1..]);
    let hints: String = footer.concat().split_whitespace().collect();
    if !["Entertoselect", "Esctocancel", "navigate"]
        .iter()
        .all(|hint| hints.contains(hint))
    {
        return Err("Claude's question controls are not fully visible. Enlarge Terminal.".into());
    }
    let mut starts = (title..rows.len())
        .filter(|&row| matches!(numbered(rows[row]), Some((0, _, _))))
        .peekable();
    if starts.peek().is_none() {
        return Err("Missing Claude choices".into());
    }
    // Claude also renders prompts with a dim vertical gutter. It is terminal
    // decoration, not part of the tool's question. Strip it only at the start
    // of prompt lines; the question and every option must still match exactly.
    // A wrapped prompt can itself begin a row with "1. ": the choices start
    // at the first such row above which exactly one question is visible.
    let (first, index, item) = starts
        .find_map(|first| {
            let prompt = &rows[title..first];
            let unframed: Vec<_> = prompt
                .iter()
                .map(|row| row.strip_prefix("│ ").unwrap_or(row))
                .collect();
            let matching: Vec<_> = items
                .iter()
                .enumerate()
                .filter(|(_, item)| {
                    let expected = string(item, "question");
                    question_terminal::wrapped(&unframed, expected)
                        || question_terminal::wrapped(prompt, expected)
                })
                .collect();
            (matching.len() == 1).then(|| (first, matching[0].0, matching[0].1))
        })
        .ok_or("The visible Claude question differs from this request")?;
    let multi = item["multi_select"] == true;
    let options = item["options"].as_array().ok_or("Missing options")?;
    let end = rows
        .iter()
        .enumerate()
        .skip(first)
        .find(|(_, r)| divider(r))
        .map(|(i, _)| i)
        .ok_or("Claude choices are clipped")?;
    let mut labels: Vec<Vec<&str>> = Vec::new();
    let mut other_rows = Vec::new();
    let mut in_other = false;
    let mut focused = None;
    let mut checked = BTreeSet::new();
    for (row, styled) in rows[first..end].iter().zip(&styled[first..end]) {
        // Only the next number after a completely shown option starts a
        // choice. A wrapped label or description may also begin "N. ".
        let next = numbered(row).filter(|(number, _, _)| {
            *number == labels.len()
                && *number <= options.len()
                && labels
                    .last()
                    .is_none_or(|shown| option_shown(shown, &options[number - 1], multi).is_some())
        });
        if let Some((number, selected, text)) = next {
            if selected && focused.replace(number).is_some() {
                return Err("Unknown Claude option order".into());
            }
            let text = if multi {
                if let Some(text) = text.strip_prefix("[✔] ") {
                    checked.insert(number);
                    text
                } else {
                    text.strip_prefix("[ ] ")
                        .ok_or("Unknown Claude checkbox state")?
                }
            } else {
                text
            };
            if number == options.len() {
                in_other = true;
                let margin = styled
                    .iter()
                    .position(|(ch, _)| !ch.is_whitespace())
                    .unwrap_or(0);
                let mut offset = margin + row[..row.len() - text.len()].chars().count();
                if text.is_empty() && styled.get(offset).is_some_and(|(ch, _)| *ch == ' ') {
                    offset += 1; // The exact numbered-row separator, not editor text.
                }
                other_rows.push(styled[offset..].to_vec());
            }
            labels.push(vec![text]);
        } else if ["Next", "Submit", "❯ Next", "❯ Submit"].contains(row) {
            in_other = false;
        } else if !row.is_empty() {
            // Labels and descriptions wrap below their first row. Reject
            // missing/clipped labels rather than applying an option index to
            // a partial menu.
            let last = labels.last_mut().ok_or("Unexpected option description")?;
            last.push(row);
            if in_other {
                other_rows.push(styled.clone());
            }
        } else if in_other {
            // Blank editor rows are actual newline evidence, even when the
            // cursor is elsewhere. Do not trim them into an empty hint.
            other_rows.push(styled.clone());
        }
    }
    let shown: Vec<_> = labels
        .iter()
        .zip(options)
        .map(|(rows, option)| option_shown(rows, option, multi))
        .collect();
    if shown.contains(&None) {
        return Err("Claude's visible options differ from this request".into());
    }
    if labels.len() != options.len() + 1 {
        return Err("Claude choices are clipped. Enlarge Terminal.".into());
    }
    for (number, committed) in shown.into_iter().enumerate() {
        if committed == Some(true) {
            checked.insert(number);
        }
    }
    if !multi && checked.len() > 1 {
        return Err("Claude shows multiple selected answers for a single choice".into());
    }
    let empty_other = empty_hint(&other_rows);
    // Strip only the styled native commit marker before locating the cursor.
    // Its state still participates in panel identity through checked.
    if let Some(last) = other_rows.last_mut() {
        if committed_other(last) {
            last.truncate(last.len() - 2);
            checked.insert(options.len());
        }
    }
    if !multi && checked.len() > 1 {
        return Err("Claude shows multiple selected answers for a single choice".into());
    }
    // Locate the caret before its trailing cell is removed. More than one
    // inverse cell is unknown styling and never proves a caret position.
    let mut carets = other_rows.iter().enumerate().flat_map(|(row, cells)| {
        cells
            .iter()
            .enumerate()
            .filter(|(_, (_, style))| style.inverse)
            .map(move |(column, (ch, _))| Caret {
                row,
                column,
                line_end: *ch == ' ' && column + 1 == cells.len(),
            })
    });
    let caret = match (carets.next(), carets.next()) {
        (Some(caret), None) => Some(caret),
        _ => None,
    };
    // Remove exactly the inverse trailing cursor cell, never real spaces.
    if let Some(last) = other_rows.last_mut() {
        if last
            .last()
            .is_some_and(|(ch, style)| *ch == ' ' && style.inverse)
        {
            last.pop();
        }
    }
    let other = other_rows
        .iter()
        .map(|row| row.iter().map(|(ch, _)| ch).collect::<String>())
        .collect::<Vec<_>>()
        .join("\n");
    Ok(Panel::Question {
        index,
        focused: focused.ok_or("Select an answer option in Terminal first")?,
        checked,
        other,
        empty_other,
        caret,
    })
}
fn wait<T: Transport>(
    io: &mut T,
    card: &Value,
    expected: impl Fn(&Panel) -> bool,
) -> Result<Panel> {
    let deadline = Instant::now() + Duration::from_secs(2);
    loop {
        let panel = parse(&io.screen()?, card)?;
        if expected(&panel) {
            return Ok(panel);
        }
        if Instant::now() >= deadline {
            return Err("Claude did not confirm question navigation".into());
        }
        io.pause();
    }
}
fn goto<T: Transport>(io: &mut T, card: &Value, index: usize) -> Result<Panel> {
    let items = card["questions"].as_array().unwrap();
    for _ in 0..=items.len() + 1 {
        let panel = parse(&io.screen()?, card)?;
        let current = panel.index(items.len());
        if current == index {
            return Ok(panel);
        }
        if let Panel::Question { focused, .. } = &panel {
            if *focused == items[current]["options"].as_array().unwrap().len() {
                io.key("Up")?;
                wait(
                    io,
                    card,
                    |p| matches!(p,Panel::Question{index:i,focused:f,..} if *i==current && *f==*focused-1),
                )?;
            }
        }
        // Tab does not wrap and Other owns horizontal arrows while editing.
        let next = if current < index {
            current + 1
        } else {
            current - 1
        };
        io.key(if current < index { "Tab" } else { "Left" })?;
        wait(io, card, |p| p.index(items.len()) == next)?;
    }
    Err("Could not reach the requested Claude question".into())
}
fn focus_other<T: Transport>(io: &mut T, card: &Value, index: usize) -> Result<()> {
    let item = &card["questions"][index];
    let count = item["options"].as_array().unwrap().len();
    let Panel::Question {
        index: current,
        focused,
        ..
    } = parse(&io.screen()?, card)?
    else {
        return Err("Claude left the current question".into());
    };
    if current != index {
        return Err("Claude changed question during selection".into());
    }
    // Numeric single-choice keys activate a committed Other immediately.
    // Arrow navigation enters its editor without submitting the old answer.
    for next in focused + 1..=count {
        io.key("Down")?;
        wait(io, card, |p| {
            matches!(p,Panel::Question{index:i,focused:f,..}
            if *i==index && *f==next)
        })?;
    }
    Ok(())
}
/// Wait until Claude shows the caret move of the key just sent, still in the
/// same non-empty Other editor, and return the editor text of that frame.
/// Every later key is checked against the screen read here: a frame captured
/// before Claude redraws would report Claude's own redraw as a changed
/// terminal, or let a reduction be measured from a stale draft. A caret move
/// changes at most the whitespace Claude shows around the caret and at wraps,
/// so the frame must show the same draft otherwise: text typed meanwhile never
/// becomes the screen the next key is checked against.
fn confirm_caret<T: Transport>(
    io: &mut T,
    card: &Value,
    index: usize,
    draft: &str,
    moved: impl Fn(Caret) -> bool,
    left: &str,
) -> Result<String> {
    let visible = |text: &str| text.chars().filter(|ch| !ch.is_whitespace()).collect::<String>();
    let draft = visible(draft);
    let count = card["questions"][index]["options"]
        .as_array()
        .unwrap()
        .len();
    let deadline = Instant::now() + Duration::from_secs(2);
    loop {
        let Panel::Question {
            index: current,
            focused,
            other,
            empty_other: false,
            caret,
            ..
        } = parse(&io.screen()?, card)?
        else {
            return Err(left.into());
        };
        if current != index || focused != count {
            return Err(left.into());
        }
        let changed = visible(&other) != draft;
        if !changed && caret.is_some_and(&moved) {
            return Ok(other);
        }
        if Instant::now() >= deadline {
            return Err(if changed {
                "Claude's Other draft changed while moving its caret; inspect Terminal"
            } else {
                "Claude did not confirm moving the Other caret; inspect Terminal"
            }
            .into());
        }
        io.pause();
    }
}
fn clear_other<T: Transport>(io: &mut T, card: &Value, index: usize) -> Result<()> {
    clear_other_within(io, card, index, Duration::from_secs(5))
}
/// `window` bounds the time without a verified reduction. The round cap and
/// the strictly shrinking draft bound the whole clear, so a slow terminal that
/// keeps confirming progress is not reported as an uncertain delivery.
fn clear_other_within<T: Transport>(io: &mut T, card: &Value, index: usize, window: Duration) -> Result<()> {
    focus_other(io, card, index)?;
    let count = card["questions"][index]["options"]
        .as_array()
        .unwrap()
        .len();
    let mut deadline = Instant::now() + window;
    let mut right_without_progress = false;
    for _ in 0..128 {
        let Panel::Question {
            index: current,
            focused,
            other,
            empty_other,
            caret,
            ..
        } = parse(&io.screen()?, card)?
        else {
            return Err("Claude left the Other editor".into());
        };
        if current != index || focused != count {
            return Err("Claude left the Other editor".into());
        }
        if empty_other {
            return Ok(());
        }
        if Instant::now() >= deadline {
            break;
        }
        // Native clearing removes the visual line before the caret. Repeat
        // only after a verified reduction; each one renews the shared
        // deadline. Move the caret to the end of its line first unless Claude
        // already shows it there, so no key is sent whose effect is unproven.
        let before = match caret {
            Some(caret) if caret.line_end => other,
            Some(_) => {
                io.key("C-e")?;
                confirm_caret(io, card, index, &other, |caret| caret.line_end, "Claude left the Other editor")?
            }
            None => {
                return Err("Claude's Other caret is not visible; clear the draft in Terminal".into())
            }
        };
        io.key("C-u")?;
        let progress_deadline = deadline.min(Instant::now() + Duration::from_millis(750));
        loop {
            let panel = parse(&io.screen()?, card)?;
            let Panel::Question {
                index: current,
                focused,
                other,
                empty_other,
                caret,
                ..
            } = panel
            else {
                return Err("Claude left the Other editor".into());
            };
            if current != index || focused != count {
                return Err("Claude left the Other editor".into());
            }
            if empty_other {
                return Ok(());
            }
            if other.chars().count() < before.chars().count() {
                right_without_progress = false;
                deadline = Instant::now() + window;
                break;
            }
            if Instant::now() >= progress_deadline {
                // C-u can reach the beginning of a hard line while another
                // newline remains ahead. Traverse one proven blank boundary,
                // then require the normal clear to make progress again.
                if !right_without_progress
                    && Instant::now() < deadline
                    && other
                        .split_once('\n')
                        .is_some_and(|(first, _)| first.trim().is_empty())
                {
                    io.key("Right")?;
                    confirm_caret(
                        io,
                        card,
                        index,
                        &other,
                        |moved| Some(moved) != caret,
                        "Claude left the Other editor during newline traversal",
                    )?;
                    right_without_progress = true;
                    break;
                }
                return Err("Claude did not confirm clearing Other; inspect Terminal".into());
            }
            io.pause();
        }
    }
    Err("Claude Other clearing exceeded its bounded limit; inspect Terminal".into())
}
fn selected_options(item: &Value, answer: &Value) -> BTreeSet<usize> {
    item["options"]
        .as_array()
        .unwrap()
        .iter()
        .enumerate()
        .filter(|(_, option)| {
            answer["selected_option_ids"]
                .as_array()
                .unwrap()
                .contains(&option["id"])
        })
        .map(|(index, _)| index)
        .collect()
}
fn drive<T: Transport>(io: &mut T, card: &Value, answers: &Value) -> Result<()> {
    let items = card["questions"].as_array().ok_or("Missing questions")?;
    let answers = answers.as_array().ok_or("Missing answers")?;
    let wanted: Vec<_> = items
        .iter()
        .zip(answers)
        .map(|(item, answer)| {
            let mut labels: Vec<_> = selected_options(item, answer)
                .iter()
                .map(|index| string(&item["options"][*index], "label").to_owned())
                .collect();
            if !string(answer, "text").is_empty() {
                labels.push(string(answer, "text").into());
            }
            norm(&labels.join(", "))
        })
        .collect();
    // Even a review that looks identical can contain a native custom answer
    // equal to a preset label. Explicit Submit always reconciles every editor.
    for (index, (item, answer)) in items.iter().zip(answers).enumerate() {
        let options = item["options"].as_array().unwrap();
        let count = options.len();
        let selected = selected_options(item, answer);
        let text = string(answer, "text");
        let multi = item["multi_select"] == true;
        goto(io, card, index)?;
        // Explicit Submit makes the GUI form authoritative, including presets.
        clear_other(io, card, index)?;
        if multi {
            for pass in 0..2 {
                for option in 0..=count {
                    let panel = parse(&io.screen()?, card)?;
                    let Panel::Question {
                        index: current,
                        checked,
                        focused,
                        ..
                    } = panel
                    else {
                        return Err("Claude left the current question".into());
                    };
                    if current != index {
                        return Err("Claude changed question during selection".into());
                    }
                    // Leave a previously focused input before sending numeric keys.
                    if focused == count {
                        io.key("Up")?;
                        wait(
                            io,
                            card,
                            |p| matches!(p,Panel::Question{index:i,focused:f,empty_other:true,..} if *i==index && *f==count-1),
                        )?;
                    }
                    let desired = pass == 1 && option < count && selected.contains(&option);
                    if checked.contains(&option) != desired {
                        io.key(&(option + 1).to_string())?;
                        wait(
                            io,
                            card,
                            |p| matches!(p,Panel::Question{index:i,checked:c,..} if *i==index && c.contains(&option)==desired),
                        )?;
                    }
                }
            }
        }
        if !text.is_empty() {
            focus_other(io, card, index)?;
            let panel = parse(&io.screen()?, card)?;
            if !matches!(panel,Panel::Question{index:i,focused:f,empty_other:true,..} if i==index && f==count)
            {
                return Err("Claude Other is no longer empty; inspect Terminal".into());
            }
            io.paste(text)?;
            wait(io, card, |p| pasted_editor(p, index, count, text))?;
            // A concurrent navigation cannot turn a verified paste into a
            // commit of the highlighted preset on another panel.
            if !pasted_editor(&parse(&io.screen()?, card)?, index, count, text) {
                return Err("Claude changed the pasted Other editor".into());
            }
            if multi {
                // Typing selects Other automatically; Enter in this text
                // field is a no-op. Preserve its checked state when leaving.
                io.key("Up")?;
                wait(io, card, |p| {
                    matches!(p,Panel::Question{index:i,focused:f,other,empty_other:false,..}
                    if *i==index && *f==count-1 && pasted_projection(other,text))
                })?;
                let p = parse(&io.screen()?, card)?;
                let Panel::Question {
                    index: current,
                    focused,
                    other,
                    checked,
                    empty_other,
                    ..
                } = p
                else {
                    return Err("Claude left the pasted question".into());
                };
                if current != index
                    || focused != count - 1
                    || empty_other
                    || !pasted_projection(&other, text)
                {
                    return Err("Claude changed the pasted question".into());
                }
                let mut desired = selected.clone();
                desired.insert(count);
                if !checked.contains(&count) {
                    io.key(&(count + 1).to_string())?;
                    wait(io, card, |p| {
                        matches!(p,Panel::Question{index:i,focused:f,other,checked,empty_other:false,..}
                        if *i==index && *f==count-1 && pasted_projection(other,text) && *checked==desired)
                    })?;
                }
                if !matches!(parse(&io.screen()?,card)?,Panel::Question{index:i,focused:f,other,checked,empty_other:false,..}
                    if i==index && f==count-1 && pasted_projection(&other,text) && checked==desired)
                {
                    return Err("Claude changed the selected Other answer".into());
                }
                io.key("Tab")?;
            } else {
                io.key("Enter")?;
                if items.len() == 1 {
                    return Ok(());
                }
            }
        } else if multi {
            if !matches!(parse(&io.screen()?,card)?,Panel::Question{index:i,focused:f,checked,empty_other:true,..}
                if i==index && f<count && checked==selected)
            {
                return Err("Claude changed the selected preset answers".into());
            }
            io.key("Tab")?;
        } else {
            let panel = parse(&io.screen()?, card)?;
            if matches!(panel,Panel::Question{focused:f,..} if f==count) {
                io.key("Up")?;
                wait(
                    io,
                    card,
                    |p| matches!(p,Panel::Question{index:i,focused:f,empty_other:true,..} if *i==index && *f==count-1),
                )?;
            }
            if !matches!(parse(&io.screen()?,card)?,Panel::Question{index:i,focused:f,empty_other:true,..}
                if i==index && f<count)
            {
                return Err("Claude changed the cleared question".into());
            }
            io.key(&(selected.iter().next().ok_or("Missing selected answer")? + 1).to_string())?;
            if items.len() == 1 {
                return Ok(());
            }
        }
        wait(io, card, |p| p.index(items.len()) == index + 1)?;
    }
    goto(io, card, items.len())?;
    submit_review(io, card, &wanted)
}
// A review answer keeps Claude's wrapped rows, joined by newlines.
fn review_shows(actual: &[String], wanted: &[String]) -> bool {
    actual.len() == wanted.len()
        && actual.iter().zip(wanted).all(|(rows, text)| {
            question_terminal::wrapped(&rows.split('\n').collect::<Vec<_>>(), text)
        })
}
fn submit_review<T: Transport>(io: &mut T, card: &Value, wanted: &[String]) -> Result<()> {
    let panel = parse(&io.screen()?, card)?;
    let Panel::Review {
        answers: actual,
        focused,
    } = panel
    else {
        return Err("Claude's final review is missing".into());
    };
    if !review_shows(&actual, wanted) {
        return Err(
            "Claude's final review differs from your answers; nothing was submitted".into(),
        );
    }
    if focused != 0 {
        io.key("Up")?;
        wait(
            io,
            card,
            |p| matches!(p,Panel::Review{answers,focused:0} if review_shows(answers,wanted)),
        )?;
    }
    if !matches!(parse(&io.screen()?,card)?,Panel::Review{answers,focused:0} if review_shows(&answers,wanted))
    {
        return Err("Claude's final review changed".into());
    }
    io.key("Enter")
}

/// Grow the pane no client views until this recorded question's clipped panel
/// shows completely (`question_terminal::fit_unviewed`); the exact match in
/// `available` still decides. Only a question listing calls it.
pub(super) fn fit(record: &Value, card: &Value) {
    if string(card, "source") != "hook"
        || record["pending_questions"]
            .get(string(card, "tool_call_id"))
            .is_none()
    {
        return;
    }
    let Ok(current) = Native::new(record, card).identity() else {
        return;
    };
    if let Ok(screen) = Native::capture(&current) {
        question_terminal::fit_unviewed(&current, screen, Native::capture, clipped, complete);
    }
}
pub(super) fn available(record: &Value, card: &Value) -> Result<()> {
    if string(record, "agent") != "claude" || string(card, "source") != "hook" {
        return Err("Not a Claude question".into());
    }
    let native = Native::new(record, card);
    let current = native.identity()?;
    if response(&current, card)?.is_some() {
        return Err("This Claude question was already answered; nothing was sent".into());
    }
    // Inspection never edits Terminal. Explicit Submit clears/replaces native
    // Other from the authoritative GUI form after per-write identity checks.
    parse(&Native::capture(&current)?, card)?;
    Ok(())
}
pub(super) fn answer(record: &Value, card: &Value, answers: &Value) -> Result<()> {
    available(record, card)?;
    let mut native = Native::new(record, card);
    drive(&mut native, card, answers).map_err(|e| {
        if native.touched() {
            format!("delivery uncertain: {e}; inspect Terminal before retrying")
        } else {
            e
        }
    })
}
pub(super) fn response(record: &Value, card: &Value) -> Result<Option<Value>> {
    let id = string(record, "conversation_id");
    let path = PathBuf::from(string(record, "transcript"));
    if uuid::Uuid::parse_str(id).is_err()
        || !path.is_absolute()
        || !path
            .file_name()
            .is_some_and(|n| n.to_string_lossy() == format!("{id}.jsonl"))
    {
        return Err("Claude transcript identity is unavailable".into());
    }
    let mut file = File::open(path).map_err(|e| e.to_string())?;
    let offset = file
        .metadata()
        .map_err(|e| e.to_string())?
        .len()
        .saturating_sub(8 * 1024 * 1024);
    file.seek(SeekFrom::Start(offset))
        .map_err(|e| e.to_string())?;
    let mut bytes = Vec::new();
    file.take(8 * 1024 * 1024)
        .read_to_end(&mut bytes)
        .map_err(|e| e.to_string())?;
    for (index, line) in bytes.split_inclusive(|b| *b == b'\n').enumerate() {
        if (index == 0 && offset > 0) || !line.ends_with(b"\n") || line.len() > 1024 * 1024 {
            continue;
        }
        let Ok(event) = serde_json::from_slice::<Value>(line) else {
            continue;
        };
        // Claude 2.1.304+ also records each API request as an api-request-blob
        // row with the same tool_result part; only the user row carries the
        // structured toolUseResult acknowledgement.
        if string(&event, "sessionId") != id || event["isSidechain"] == true || event["type"] != "user" {
            continue;
        }
        let Some(parts) = event["message"]["content"].as_array() else {
            continue;
        };
        let Some(result) = parts
            .iter()
            .find(|p| p["type"] == "tool_result" && p["tool_use_id"] == card["tool_call_id"])
        else {
            continue;
        };
        if result["is_error"] == true {
            return Err("Claude rejected the question response".into());
        }
        let result = &event["toolUseResult"];
        if questions::normalize(&result["questions"])? != card["questions"]
            || !result["answers"].is_object()
        {
            return Err(
                "Claude's question acknowledgement differs from the pending request".into(),
            );
        }
        return Ok(Some(
            json!({"type":"interaction.resolved","time":now()*1000.,"response":{"answers":result["answers"]}}),
        ));
    }
    Ok(None)
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::fs;
    fn items() -> Value {
        json!([
            {"question":"Which release should we prepare?","header":"Release","options":[{"label":"Stable","description":"Use stable build"},{"label":"Preview","description":"Use preview build"}]},
            {"question":"Which checks should we run?","header":"Checks","multiSelect":true,"options":[{"label":"Unit","description":"Unit checks"},{"label":"Integration","description":"Integration checks"},{"label":"UI","description":"UI checks"}]},
            {"question":"What else should we know?","header":"Notes","options":[{"label":"Nothing","description":"No extra notes"},{"label":"Later","description":"Discuss later"}]}
        ])
    }
    fn card() -> Value {
        json!({"questions":questions::normalize(&items()).unwrap(),"tool_call_id":"toolu_native_fixture"})
    }
    #[test]
    fn clipped_panels_differ_from_complete_ones() {
        let clipped_panel = include_str!("../../tests/fixtures/claude-question-clipped-80x24.txt");
        assert!(clipped(clipped_panel));
        assert!(!complete(clipped_panel));
        for screen in [
            include_str!("../../tests/fixtures/claude-question-single.txt"),
            include_str!("../../tests/fixtures/claude-question-multi-selected.txt"),
            include_str!("../../tests/fixtures/claude-question-other.txt"),
            include_str!("../../tests/fixtures/claude-question-review.txt"),
            include_str!("../../tests/fixtures/claude-question-single-windows.txt"),
        ] {
            assert!(!clipped(screen), "{screen}");
            assert!(complete(screen), "{screen}");
        }
        // A review whose heading scrolled off is clipped as well.
        let review = include_str!("../../tests/fixtures/claude-question-review.txt");
        let tail: String = review.lines().skip(12).map(|row| format!("{row}\n")).collect();
        assert!(clipped(&tail));
        // Without any panel there is nothing to fit.
        for screen in ["", "❯ Ask questions with the tool now.\n", "Enter to select\n"] {
            assert!(!clipped(screen));
            assert!(!complete(screen));
        }
    }
    #[test]
    fn recognizes_recorded_claude_2_1_289_panels() {
        let card = card();
        assert!(matches!(
            parse(
                include_str!("../../tests/fixtures/claude-question-single.txt"),
                &card
            )
            .unwrap(),
            Panel::Question {
                index: 0,
                focused: 0,
                ..
            }
        ));
        assert!(
            matches!(parse(include_str!("../../tests/fixtures/claude-question-multi-selected.txt"),&card).unwrap(),Panel::Question{index:1,checked,..} if checked==BTreeSet::from([0,2]))
        );
        assert!(
            matches!(parse(include_str!("../../tests/fixtures/claude-question-other.txt"),&card).unwrap(),Panel::Question{index:2,focused:2,other,..} if other=="Type something.")
        );
        assert!(
            matches!(parse(include_str!("../../tests/fixtures/claude-question-other-filled.txt"),&card).unwrap(),Panel::Question{index:2,focused:2,other,..} if other=="Привет from Zerus")
        );
        assert!(
            matches!(parse(include_str!("../../tests/fixtures/claude-question-review.txt"),&card).unwrap(),Panel::Review{focused:0,answers} if answers==["Preview","Unit, UI","Привет from Zerus"])
        );
    }
    #[test]
    fn refuses_changed_clipped_or_unrecognized_questions() {
        let screen = include_str!("../../tests/fixtures/claude-question-single.txt");
        for changed in [
            screen.replace("Which release should we prepare?", "Another question?"),
            screen.replace("Use preview build", "Different description"),
            screen.replace("2. Preview", "2. Other choice"),
            screen.replace("1. Stable", "2. Stable"),
            screen.replace("Esc to cancel", "Esc to leave"),
            screen.replace("❯ 1.", "  1."),
        ] {
            assert!(parse(&changed, &card()).is_err(), "{changed}");
        }
    }
    #[test]
    fn prompt_gutter_preserves_exact_question_and_option_matching() {
        let screen = include_str!("../../tests/fixtures/claude-question-single.txt");
        let card = card();
        for prompt in [
            "│ Which release should we prepare?",
            "\x1b[2m│\x1b[0m \x1b[1mWhich release should we prepare?\x1b[0m",
            "│ Which release should\n│ we prepare?",
            "│ Which release should\n  we prepare?",
        ] {
            let decorated = screen.replace("Which release should we prepare?", prompt);
            assert_eq!(
                parse(&decorated, &card).unwrap(),
                parse(screen, &card).unwrap()
            );
            assert!(same_panel(screen, &decorated, &card));
            for changed in [
                decorated.replace("should", "must"),
                decorated.replace("2. Preview", "2. Other choice"),
                decorated.replace("Use preview build", "Different description"),
                decorated.replace("❯ 1. Stable", "❯ 1. │ Stable"),
            ] {
                assert!(parse(&changed, &card).is_err(), "{changed}");
            }
        }
        // A literal leading bar in the request remains valid too.
        let mut literal = card.clone();
        literal["questions"][0]["question"] = json!("│ Which release should we prepare?");
        let screen = screen.replace(
            "Which release should we prepare?",
            "│ Which release should we prepare?",
        );
        assert!(parse(&screen, &literal).is_ok());
    }
    #[test]
    fn recognizes_gutter_review_and_preserves_literal_answer_text() {
        let card = card();
        let plain = include_str!("../../tests/fixtures/claude-question-review.txt");
        let screen = include_str!("../../tests/fixtures/claude-question-review-gutter.txt");
        assert_eq!(parse(screen, &card).unwrap(), parse(plain, &card).unwrap());
        assert!(same_panel(plain, screen, &card));
        let styled = screen.replace("│", "\x1b[2m│\x1b[0m");
        assert!(same_panel(&styled, screen, &card));
        let literal = screen.replace("Привет from Zerus", "Привет from\n       │ Zerus");
        assert!(
            matches!(parse(&literal, &card).unwrap(), Panel::Review { answers, .. }
            if answers[2] == "Привет from\n│ Zerus"
                && !review_shows(&answers[2..], &["Привет from Zerus".into()]))
        );
        assert!(!same_panel(screen, &literal, &card));
        for changed in [
            screen.replace("we prepare?", "we publish?"),
            screen.replace("│ ● Which checks should we run?\n   → Unit, UI\n", ""),
            screen.replace("Submit answers", "Run command"),
            screen.replace("❯ 1.", "  1."),
        ] {
            assert!(parse(&changed, &card).is_err(), "{changed}");
        }
    }
    #[test]
    fn review_strips_the_gutter_only_from_framed_questions() {
        // Claude 2.1.296 frames each review question separately: at 87
        // columns only a long question may carry the gutter.
        let card = card();
        let plain = include_str!("../../tests/fixtures/claude-question-review.txt");
        let expected = parse(plain, &card).unwrap();
        for framed in [
            plain.replace(" ● What else", " │ ● What else"),
            plain.replace(
                " ● What else should we know?",
                " │ ● What else should\n │   we know?",
            ),
            plain.replace(" ● Which checks", " │ ● Which checks"),
        ] {
            assert_eq!(parse(&framed, &card).unwrap(), expected, "{framed}");
        }
        // An unframed question keeps a literal bar on its wrapped row.
        let literal = plain.replace(
            " ● What else should we know?",
            " ● What else should\n │ we know?",
        );
        assert!(parse(&literal, &card).is_err());
    }
    #[test]
    fn recognizes_committed_single_choices_without_changing_labels() {
        let card = card();
        let screen = include_str!("../../tests/fixtures/claude-question-single.txt");
        let chosen = screen.replace("2. Preview", "2. Preview ✔");
        assert!(
            matches!(parse(&chosen, &card).unwrap(), Panel::Question { checked, .. }
            if checked == BTreeSet::from([1]))
        );
        assert!(!same_panel(screen, &chosen, &card));
        assert!(parse(&chosen.replace("1. Stable", "1. Stable ✔"), &card).is_err());
        assert!(parse(&chosen.replace("2. Preview", "2. Different"), &card).is_err());
        let mut literal = card.clone();
        literal["questions"][0]["options"][1]["label"] = json!("Preview ✔");
        assert!(parse(&chosen, &literal).is_ok());
        assert!(
            matches!(parse(&chosen.replace("Preview ✔", "Preview ✔ ✔"), &literal).unwrap(),
            Panel::Question { checked, .. } if checked == BTreeSet::from([1]))
        );
    }
    // Ink word-wraps each text node to its column. As in Claude 2.1.296, a
    // word longer than a row continues on the current row and is split at its
    // end. Continuation rows keep the node's column and the prompt gutter.
    // Claude truncates its tab row instead of wrapping it.
    fn ink_wrap(text: &str, width: usize) -> Vec<String> {
        let mut rows = Vec::new();
        let mut row = String::new();
        for word in text.split(' ') {
            let used = row.chars().count();
            let mut chars: Vec<char> = word.chars().collect();
            if used > 0 && used + 1 + chars.len() <= width {
                row.push(' ');
                row.push_str(word);
                continue;
            }
            if used > 0 && chars.len() > width {
                row.push(' ');
                row.extend(chars.drain(..width.saturating_sub(used + 1)));
            }
            if used > 0 {
                rows.push(std::mem::take(&mut row));
            }
            while chars.len() > width {
                rows.push(chars.drain(..width).collect());
            }
            row = chars.into_iter().collect();
        }
        rows.push(row);
        rows
    }
    fn rewrap(screen: &str, width: usize) -> String {
        let mut out = Vec::new();
        for row in screen.lines() {
            if divider(row.trim()) {
                out.push("─".repeat(width));
                continue;
            }
            if row.trim_start().starts_with('←') || row.chars().count() <= width {
                out.push(row.to_owned());
                continue;
            }
            let chars: Vec<char> = row.chars().collect();
            let column = chars
                .iter()
                .take_while(|c| c.is_whitespace() || "❯│●→[]✔.0123456789".contains(**c))
                .count();
            let head: String = chars[..column].iter().collect();
            let next: String = head
                .chars()
                .map(|c| if c == '│' { c } else { ' ' })
                .collect();
            let text: String = chars[column..].iter().collect();
            for (i, part) in ink_wrap(text.trim_end(), width - column)
                .into_iter()
                .enumerate()
            {
                out.push(format!("{}{part}", if i == 0 { &head } else { &next }));
            }
        }
        out.join("\n") + "\n"
    }
    #[test]
    fn recognizes_panels_wrapped_at_any_terminal_width() {
        // Long tokens force hard splits; numbered-looking text can begin a row.
        let long = [
            ("Which release should we prepare?", "Which release should we prepare for the contract in /work/example/projects/a-really-long-directory-name/contracts/template.html after step 1. review?"),
            ("Use stable build", "Use stable build from the release branch, then step 2. Verify the signed tag"),
            ("Use preview build", "Use preview build, step 1. Build the package 2. Publish the candidate 0123456789abcdef0123456789abcdef0123456789abcdef"),
            ("Preview", "Preview build from the nightly channel with verification"),
            ("Integration checks", "Integration checks across the Linux, Arch, macOS and Windows matrix 1. first 2. second"),
            ("What else should we know?", "What else should we know before the candidate is verified and published?"),
        ];
        let widen = |text: &str| {
            long.iter().fold(text.to_owned(), |text, (short, long)| {
                text.replace(short, long)
            })
        };
        let short = card();
        let card = json!({"questions":questions::normalize(&serde_json::from_str::<Value>(&widen(&items().to_string())).unwrap()).unwrap(),"tool_call_id":"toolu_native_fixture"});
        let wanted = [
            "Preview build from the nightly channel with verification",
            "Unit, UI",
            "Привет from Zerus",
        ]
        .map(str::to_owned);
        for fixture in [
            include_str!("../../tests/fixtures/claude-question-single.txt"),
            include_str!("../../tests/fixtures/claude-question-multi.txt"),
            include_str!("../../tests/fixtures/claude-question-multi-selected.txt"),
            include_str!("../../tests/fixtures/claude-question-other.txt"),
            include_str!("../../tests/fixtures/claude-question-other-filled.txt"),
            include_str!("../../tests/fixtures/claude-question-review.txt"),
            // Only the long first review question carries Claude's gutter.
            &include_str!("../../tests/fixtures/claude-question-review.txt")
                .replace(" ● Which release", " │ ● Which release"),
        ] {
            let screen = widen(fixture);
            let expected = parse(&screen, &card).unwrap();
            for width in 32..=160 {
                let wrapped = rewrap(&screen, width);
                match parse(&wrapped, &card) {
                    Ok(Panel::Review { answers, focused }) => assert!(
                        focused == 0 && review_shows(&answers, &wanted),
                        "{width}: {answers:?}\n{wrapped}"
                    ),
                    Ok(panel) => assert_eq!(panel, expected, "{width}\n{wrapped}"),
                    Err(error) => panic!("{width}: {error}\n{wrapped}"),
                }
            }
        }
        // As in Claude 2.1.296 at 87 columns: focusing Other adds a key hint.
        let other = rewrap(
            include_str!("../../tests/fixtures/claude-question-other.txt"),
            87,
        );
        assert!(other.ends_with("Esc to\ncancel\n"), "{other}");
        assert!(parse(&other, &short).is_ok());
        // Wrapping never hides a changed option, question or review answer.
        let screen = widen(include_str!(
            "../../tests/fixtures/claude-question-single.txt"
        ));
        for (from, to) in [
            ("template.html", "template.htm"),
            ("2. Publish", "2. Publishes"),
            ("0123456789abcdef0123", "0123456789abcdeX0123"),
            ("1. Stable", "1. Stable build"),
            ("Esc to cancel", "Esc for cancel"),
        ] {
            for width in [40, 48, 87, 160] {
                let changed = rewrap(&screen.replace(from, to), width);
                assert!(parse(&changed, &card).is_err(), "{changed}");
            }
        }
        assert!(!review_shows(
            &["Preview build from\nthe nightly".into()],
            &wanted[..1]
        ));
        assert!(!review_shows(&["Unit,UI".into()], &["Unit, UI".into()]));
    }
    fn other_screen(text: &str) -> String {
        include_str!("../../tests/fixtures/claude-question-other.txt")
            .replace("Type something.", text)
    }
    fn styled_empty() -> String {
        other_screen("\x1b[7mT\x1b[0;2mype something.\x1b[0m")
    }
    #[test]
    fn empty_editor_requires_native_hint_styling() {
        let card = card();
        assert!(matches!(
            parse(&styled_empty(), &card).unwrap(),
            Panel::Question {
                empty_other: true,
                ..
            }
        ));
        assert!(matches!(
            parse(
                &other_screen("\x1b[7mT\x1b[0;2mype something.\x1b[0m \x1b[38;5;114m✔\x1b[39m"),
                &card
            )
            .unwrap(),
            Panel::Question {
                empty_other: true,
                ..
            }
        ));
        for text in [
            "Type something. \x1b[38;5;114m✔\x1b[39m",
            "Type something.",
            "Type something.\x1b[7m \x1b[0m",
            "\x1b[7mT\x1b[0mype something.",
            "literal \x1b[7m \x1b[0m",
        ] {
            assert!(
                matches!(
                    parse(&other_screen(text), &card).unwrap(),
                    Panel::Question {
                        empty_other: false,
                        ..
                    }
                ),
                "{text:?}"
            );
        }
        for escape in [
            "\x1b[38:5:246m",
            "\x1b[38;5m",
            "\x1b[38;2;1;2m",
            "\x1b[48;5;256m",
            "\x1b[58;2;1m",
        ] {
            assert!(parse(
                &other_screen(&format!("{escape}Type something.\x1b[0m")),
                &card
            )
            .is_err());
        }
    }
    #[test]
    fn truecolour_maps_to_the_indices_chalk_writes() {
        // Windows truecolour -> Linux tmux index for every colour of spike S3
        // where chalk's rule reproduces Linux (Claude and Kimi palettes).
        for (rgb, index) in [
            ((153, 153, 153), 246),
            ((136, 136, 136), 244),
            ((55, 55, 55), 237),
            ((80, 80, 80), 239),
            ((0, 0, 0), 16),
            ((255, 255, 255), 231),
            ((248, 248, 242), 231),
            ((78, 186, 101), 114),
            ((177, 185, 249), 153),
            ((215, 119, 87), 174),
            ((255, 193, 7), 220),
            ((72, 150, 140), 73),
            ((255, 107, 128), 211),
            ((2, 40, 0), 22),
            ((61, 1, 0), 52),
            ((79, 168, 255), 111),
            ((78, 200, 126), 114),
            ((107, 107, 107), 242),
            ((224, 224, 224), 253),
            ((232, 168, 56), 215),
            ((232, 84, 84), 210),
            ((245, 245, 245), 255),
            ((255, 203, 107), 222),
            ((91, 192, 190), 116),
            ((90, 90, 90), 240),
        ] {
            assert_eq!(ansi256(rgb.0, rgb.1, rgb.2), index, "{rgb:?}");
        }
        // Grey boundaries of chalk's 24-step ramp.
        for (grey, index) in [(7, 16), (8, 232), (248, 255), (249, 231)] {
            assert_eq!(ansi256(grey, grey, grey), index, "{grey}");
        }
    }
    // Claude Code 2.1.296 under bundled ConPTY (spike S3), `capture-pane -p -e`
    // form in the normal-screen layout: truecolour, and hint spaces skipped by
    // cursor moves are uncoloured.
    const WINDOWS_SINGLE: &str =
        include_str!("../../tests/fixtures/claude-question-single-windows.txt");
    #[test]
    fn recognizes_windows_panels_like_their_linux_captures() {
        let card = card();
        let question = |screen: &str| parse(screen, &card).unwrap();
        let empty = |index, focused, checked: &[usize], other: &str, caret| Panel::Question {
            index,
            focused,
            checked: checked.iter().copied().collect(),
            other: other.into(),
            empty_other: true,
            caret,
        };
        assert_eq!(
            question(WINDOWS_SINGLE),
            empty(0, 0, &[], "Type something.", None)
        );
        assert_eq!(
            question(include_str!(
                "../../tests/fixtures/claude-question-multi-windows.txt"
            )),
            empty(1, 0, &[], "Type something", None)
        );
        assert_eq!(
            question(include_str!(
                "../../tests/fixtures/claude-question-multi-selected-windows.txt"
            )),
            empty(1, 0, &[0, 2], "Type something", None)
        );
        // A focused empty editor shows its caret on the hint's first letter.
        assert_eq!(
            question(include_str!(
                "../../tests/fixtures/claude-question-other-windows.txt"
            )),
            empty(2, 2, &[], "Type something.", Some(Caret { row: 0, column: 0, line_end: false }))
        );
        // The committed answer's truecolour check is decoration, not answer text.
        assert_eq!(
            question(include_str!(
                "../../tests/fixtures/claude-question-revisit-committed-windows.txt"
            )),
            Panel::Question {
                index: 2,
                focused: 0,
                checked: BTreeSet::from([2]),
                other: "Привет from Zerus".into(),
                empty_other: false,
                caret: None,
            }
        );
    }
    #[test]
    fn windows_hint_needs_its_letters_styled() {
        let card = card();
        let hint = "\x1b[38;2;153;153;153mType\x1b[39m \x1b[38;2;153;153;153msomething.\x1b[39m";
        assert!(WINDOWS_SINGLE.contains(hint));
        for literal in [
            "Type something.",
            "Type\x1b[38;2;153;153;153m \x1b[39msomething.",
            "\x1b[38;2;153;153;153mType\x1b[39m something.",
            "\x1b[38;2;153;153;154mType\x1b[39m \x1b[38;2;153;153;154msomething.\x1b[39m",
            "\x1b[38;2;153;153;153mType\x1b[39m \x1b[38;2;153;153;153msomething.\x1b[39m \x1b[38;2;80;200;80m+\x1b[39m",
        ] {
            assert!(
                matches!(parse(&WINDOWS_SINGLE.replace(hint, literal), &card).unwrap(),
                    Panel::Question { empty_other: false, .. }),
                "{literal:?}"
            );
        }
        // Green text other than the check mark stays part of the answer.
        let answer = "Привет from Zerus \x1b[38;2;78;186;101m+\x1b[39m";
        assert!(
            matches!(parse(&WINDOWS_SINGLE.replace(hint, answer), &card).unwrap(),
            Panel::Question { other, checked, .. } if other == "Привет from Zerus +" && checked.is_empty())
        );
    }
    #[test]
    fn other_identity_preserves_visible_spaces_and_wrapped_rows() {
        let card = card();
        let screen = other_screen("draft  text  \x1b[7m \x1b[0m");
        assert!(matches!(parse(&screen, &card).unwrap(),
            Panel::Question { other, .. } if other == "draft  text  "));
        assert!(!same_panel(
            &screen,
            &screen.replace("draft  text", "draft text"),
            &card
        ));
        assert!(!same_panel(
            &screen,
            &screen.replace("text  ", "text "),
            &card
        ));
        let wrapped = other_screen("first\n     second\x1b[7m \x1b[0m");
        assert!(matches!(parse(&wrapped, &card).unwrap(),
            Panel::Question { other, .. } if other == "first\n     second"));
        assert!(!same_panel(
            &wrapped,
            &wrapped.replace("second", "different"),
            &card
        ));
    }
    #[test]
    fn blank_first_other_row_keeps_hard_newline_tail_visible() {
        let card = card();
        let raw = other_screen("\x1b[7m \x1b[0m\n     Third native line");
        assert!(
            matches!(parse(&raw, &card).unwrap(), Panel::Question { focused:2, other, empty_other:false, .. }
            if other.ends_with("\n     Third native line"))
        );
        assert!(parse(&raw.replace("1. Nothing", "1."), &card).is_err());
        let blank = other_screen("\x1b[7m \x1b[0m\n     ");
        assert!(
            matches!(parse(&blank, &card).unwrap(), Panel::Question { other, empty_other:false, .. }
            if other.contains('\n'))
        );
    }
    #[test]
    fn committed_other_decoration_preserves_literal_text_and_native_selection() {
        let card = card();
        let raw = other_screen("answer  ✔\x1b[7m \x1b[0m \x1b[38;5;114m✔\x1b[39m");
        assert!(
            matches!(parse(&raw, &card).unwrap(), Panel::Question { other, checked, empty_other:false, .. }
            if other == "answer  ✔" && checked == BTreeSet::from([2]))
        );
        let literal = other_screen("answer  ✔\x1b[7m \x1b[0m");
        assert!(
            matches!(parse(&literal, &card).unwrap(), Panel::Question { other, checked, .. }
            if other == "answer  ✔" && checked.is_empty())
        );
        assert!(!same_panel(&raw, &literal, &card));
    }
    #[test]
    fn freshly_pasted_projection_requires_complete_text_and_editor_focus() {
        for (visible, text) in [
            ("first\n     second", "first   second"),
            ("longtoken\n     continuation", "longtokencontinuation"),
            ("Привет\n     из Zerus", "Привет из Zerus"),
            ("Type something.", "Type something."),
        ] {
            assert!(pasted_projection(visible, text));
            let panel = Panel::Question {
                index: 2,
                focused: 2,
                checked: BTreeSet::new(),
                other: visible.into(),
                empty_other: false,
                caret: None,
            };
            assert!(pasted_editor(&panel, 2, 2, text));
            assert!(!pasted_editor(&panel, 1, 2, text));
            assert!(!pasted_editor(&panel, 2, 1, text));
        }
        for (visible, text) in [
            ("draft  text", "draft text"),
            ("draft", "draft suffix"),
            ("prefix draft", "draft"),
            ("Type something.", "Type something"),
        ] {
            assert!(!pasted_projection(visible, text));
        }
    }
    struct ClearingFixture {
        screen: String,
        after_keys: Vec<(&'static str, String)>,
        keys: Vec<String>,
        pastes: Vec<String>,
    }
    impl ClearingFixture {
        fn new(screen: String, after_keys: Vec<(&'static str, String)>) -> Self {
            ClearingFixture { screen, after_keys, keys: Vec::new(), pastes: Vec::new() }
        }
    }
    impl Transport for ClearingFixture {
        fn screen(&mut self) -> Result<String> {
            Ok(self.screen.clone())
        }
        fn key(&mut self, key: &str) -> Result<()> {
            self.keys.push(key.into());
            if self.after_keys.first().is_some_and(|(expected, _)| *expected == key) {
                self.screen = self.after_keys.remove(0).1;
            }
            Ok(())
        }
        fn paste(&mut self, text: &str) -> Result<()> {
            self.pastes.push(text.into());
            Ok(())
        }
        fn resolved(&mut self) -> Result<Option<Value>> {
            Ok(None)
        }
    }
    #[test]
    fn clearing_requires_proven_empty_and_stops_after_no_progress_or_navigation() {
        let card = card();
        let at_end = other_screen("draft\x1b[7m \x1b[0m");
        let inside = other_screen("dra\x1b[7mf\x1b[0mt");
        let away = other_screen("draft")
            .replace("❯ 3.", "  3.")
            .replace("  2. Later", "❯ 2. Later");
        for (screen, after_keys, expected_keys, succeeds) in [
            (at_end.clone(), vec![], vec!["C-u"], false),
            (at_end.clone(), vec![("C-u", styled_empty())], vec!["C-u"], true),
            (inside.clone(), vec![("C-e", at_end.clone()), ("C-u", styled_empty())], vec!["C-e", "C-u"], true),
            // C-u acts from the caret: never send it before Claude shows C-e.
            (inside.clone(), vec![], vec!["C-e"], false),
            (inside.clone(), vec![("C-e", away)], vec!["C-e"], false),
            // A caret move never makes text typed meanwhile the new baseline.
            (
                inside.clone(),
                vec![("C-e", other_screen("draftX\x1b[7m \x1b[0m")), ("C-u", styled_empty())],
                vec!["C-e"],
                false,
            ),
            // Without a visible caret no clearing key has a provable effect.
            (other_screen("draft"), vec![], vec![], false),
        ] {
            let mut io = ClearingFixture::new(screen, after_keys);
            assert_eq!(clear_other(&mut io, &card, 2).is_ok(), succeeds);
            assert_eq!(io.keys, expected_keys);
            assert!(io.pastes.is_empty());
            assert!(!io.keys.iter().any(|key| key == "Enter"));
        }
    }
    #[test]
    fn caret_follows_the_single_inverse_editor_cell() {
        let card = card();
        let caret = |text: &str| match parse(&other_screen(text), &card).unwrap() {
            Panel::Question { caret, .. } => caret,
            Panel::Review { .. } => unreachable!(),
        };
        let at = |row, column, line_end| Some(Caret { row, column, line_end });
        assert_eq!(caret("dra\x1b[7mf\x1b[0mt"), at(0, 3, false));
        assert_eq!(caret("draft\x1b[7m \x1b[0m"), at(0, 5, true));
        assert_eq!(caret("draft \x1b[7m \x1b[0m\n     tail"), at(0, 6, true));
        assert_eq!(caret("draft\x1b[7m \x1b[0mtail"), at(0, 5, false));
        assert_eq!(caret("first\n     \x1b[7m \x1b[0m"), at(1, 5, true));
        assert_eq!(caret("\x1b[7mT\x1b[0;2mype something.\x1b[0m"), at(0, 0, false));
        assert_eq!(caret("answer\x1b[7m \x1b[0m \x1b[38;5;114m✔\x1b[39m"), at(0, 6, true));
        // tmux trims the caret space after the last character of a line and
        // resets the style only where the next row starts.
        assert_eq!(caret("draft\x1b[7m"), at(0, 5, true));
        assert_eq!(caret("first\x1b[7m\n\x1b[0m     second"), at(0, 5, true));
        assert_eq!(caret("\x1b[7m\n\x1b[0m     second"), at(0, 0, true));
        assert_eq!(caret("draf\x1b[7mt"), at(0, 4, false));
        assert_eq!(caret("draft\x1b[7m\x1b[0m"), None);
        assert!(matches!(parse(&other_screen("draft\x1b[7m"), &card).unwrap(),
            Panel::Question { other, empty_other: false, .. } if other == "draft"));
        assert_eq!(caret("draft"), None);
        assert_eq!(caret("\x1b[7md\x1b[0mraft\x1b[7m \x1b[0m"), None);
        // The caret is part of the identity each key is checked against.
        assert!(!same_panel(&other_screen("dra\x1b[7mf\x1b[0mt"), &other_screen("dr\x1b[7ma\x1b[0mft"), &card));
    }
    struct SlowClearing {
        remaining: usize,
        keys: Vec<String>,
    }
    impl Transport for SlowClearing {
        fn screen(&mut self) -> Result<String> {
            Ok(if self.remaining == 0 { styled_empty() }
               else { other_screen(&format!("{}\x1b[7m \x1b[0m", "x".repeat(self.remaining))) })
        }
        fn key(&mut self, key: &str) -> Result<()> {
            self.keys.push(key.into());
            if key == "C-u" {
                std::thread::sleep(Duration::from_millis(150));
                self.remaining -= 1;
            }
            Ok(())
        }
        fn paste(&mut self, _: &str) -> Result<()> { Err("unexpected paste".into()) }
        fn resolved(&mut self) -> Result<Option<Value>> { Ok(None) }
    }
    #[test]
    fn slow_verified_clearing_continues_past_the_idle_window() {
        // Eight verified rounds take longer than the window between them.
        // The caret already ends each line, so no round needs C-e.
        let mut io = SlowClearing { remaining: 8, keys: Vec::new() };
        clear_other_within(&mut io, &card(), 2, Duration::from_millis(600)).unwrap();
        assert_eq!(io.keys, ["C-u"].repeat(8));
    }
    #[test]
    fn newline_traversal_without_clear_progress_never_repeats_or_pastes() {
        let mut io = ClearingFixture::new(
            other_screen("\x1b[7m \x1b[0m\n     remaining"),
            vec![
                ("Right", other_screen("\n     \x1b[7mr\x1b[0memaining")),
                ("C-e", other_screen("\n     remaining\x1b[7m \x1b[0m")),
            ],
        );
        assert!(clear_other(&mut io, &card(), 2).is_err());
        assert_eq!(io.keys, ["C-u", "Right", "C-e", "C-u"]);
        assert!(io.pastes.is_empty());
    }
    /// Like `Native`, every key first re-reads the terminal and refuses when it
    /// differs from the screen the driver last read. As under heavy load,
    /// Claude shows the effect of each key, in order, only `delay` captures
    /// after the key was sent.
    struct DelayedRedraw {
        card: Value,
        delay: usize,
        visible: String,
        pending: std::collections::VecDeque<(String, usize)>,
        last: Option<String>,
        after_keys: Vec<(&'static str, String)>,
        keys: Vec<String>,
    }
    impl DelayedRedraw {
        fn capture(&mut self) -> String {
            for (_, captures) in &mut self.pending {
                *captures = captures.saturating_sub(1);
            }
            while self.pending.front().is_some_and(|(_, captures)| *captures == 0) {
                self.visible = self.pending.pop_front().unwrap().0;
            }
            self.visible.clone()
        }
    }
    impl Transport for DelayedRedraw {
        fn screen(&mut self) -> Result<String> {
            let screen = self.capture();
            self.last = Some(screen.clone());
            Ok(screen)
        }
        fn key(&mut self, key: &str) -> Result<()> {
            let actual = self.capture();
            if !self.last.as_ref().is_some_and(|last| same_panel(last, &actual, &self.card)) {
                return Err("terminal changed while preparing the answer".into());
            }
            self.keys.push(key.into());
            if self.after_keys.first().is_some_and(|(expected, _)| *expected == key) {
                self.pending.push_back((self.after_keys.remove(0).1, self.delay));
            }
            Ok(())
        }
        fn paste(&mut self, _: &str) -> Result<()> { Err("unexpected paste".into()) }
        fn resolved(&mut self) -> Result<Option<Value>> { Ok(None) }
        fn pause(&mut self) {
            std::thread::sleep(Duration::from_millis(5));
        }
    }
    #[test]
    fn clearing_waits_for_claude_to_show_each_caret_move() {
        let card = card();
        for (screen, after_keys, expected_keys) in [
            // A wrapped draft: Claude hides the space that ends its last row
            // until the caret follows it, so a frame read before C-e is
            // redrawn differs from the screen C-u is then checked against.
            (
                "native wo\x1b[7mr\x1b[0mds wrapped\n     content",
                vec![
                    ("C-e", "native words wrapped\n     content \x1b[7m \x1b[0m"),
                    ("C-u", "native words wrapped \x1b[7m \x1b[0m"),
                    ("C-u", "\x1b[7mT\x1b[0;2mype something.\x1b[0m"),
                ],
                vec!["C-e", "C-u", "C-u"],
            ),
            // A blank first line: traverse it, then clear the next line.
            (
                "\x1b[7m \x1b[0m\n     remaining",
                vec![
                    ("Right", "\n     \x1b[7mr\x1b[0memaining"),
                    ("C-e", "\n     remaining\x1b[7m \x1b[0m"),
                    ("C-u", "\n     \x1b[7m \x1b[0m"),
                    ("C-u", "\x1b[7mT\x1b[0;2mype something.\x1b[0m"),
                ],
                vec!["C-u", "Right", "C-e", "C-u", "C-u"],
            ),
        ] {
            for delay in 1..=3 {
                let mut io = DelayedRedraw {
                    card: card.clone(),
                    delay,
                    visible: other_screen(screen),
                    pending: Default::default(),
                    last: None,
                    after_keys: after_keys.iter().map(|(key, text)| (*key, other_screen(text))).collect(),
                    keys: Vec::new(),
                };
                clear_other(&mut io, &card, 2).unwrap();
                assert_eq!(io.keys, expected_keys);
                assert!(io.after_keys.is_empty() && io.pending.is_empty());
            }
        }
    }
    #[test]
    fn acknowledgement_requires_exact_tool_conversation_and_schema() {
        let dir = tempfile::tempdir().unwrap();
        let id = uuid::Uuid::new_v4().to_string();
        let path = dir.path().join(format!("{id}.jsonl"));
        let record = json!({"conversation_id":id,"transcript":path});
        let card = card();
        let event = json!({"type":"user","sessionId":id,"message":{"content":[{"type":"tool_result","tool_use_id":"toolu_native_fixture"}]},"toolUseResult":{"questions":items(),"answers":{"Which release should we prepare?":"Preview"}}});
        for field in ["sessionId", "isSidechain", "message", "type"] {
            let mut stale = event.clone();
            stale[field] = if field == "isSidechain" {
                json!(true)
            } else {
                json!("another")
            };
            fs::write(&path, format!("{stale}\n")).unwrap();
            assert!(response(&record, &card).unwrap().is_none());
        }
        fs::write(&path, format!("{event}\n")).unwrap();
        assert!(response(&record, &card).unwrap().is_some());
        // Claude 2.1.304+ request blob: the same tool_result part, no acknowledgement.
        let blob = json!({"type":"api-request-blob","sessionId":id,"hash":"0".repeat(64),
            "message":{"role":"user","content":[{"type":"tool_result","tool_use_id":"toolu_native_fixture",
            "content":"Your questions have been answered"}]}});
        fs::write(&path, format!("{blob}\n")).unwrap();
        assert!(response(&record, &card).unwrap().is_none());
        fs::write(&path, format!("{blob}\n{event}\n")).unwrap();
        assert!(response(&record, &card).unwrap().is_some());
        let mut changed = event;
        changed["toolUseResult"]["questions"][0]["question"] = json!("Changed request");
        fs::write(&path, format!("{changed}\n")).unwrap();
        assert!(response(&record, &card).is_err());
    }
}
