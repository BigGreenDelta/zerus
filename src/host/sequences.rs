//! Control sequences in the session host's byte streams (D6, D13).
//!
//! The host is the only responder to terminal queries: the screen model answers
//! what tmux answers, and nothing else may. So the output relayed to attached
//! clients loses every query (`QueryStripper`), and input from clients loses
//! every answer a terminal sends (`ReplyFilter`). A query then gets exactly one
//! answer whether no client or several are attached, as with tmux, which never
//! relays a query and redraws clients from its own screen.
//!
//! Both filters see the stream through one `Scanner` that holds an incomplete
//! sequence until its last byte arrives, so a sequence split across reads is
//! judged whole. Everything that is not dropped is passed on byte for byte.
//! The output also loses DECSET 9001 (win32-input-mode), which ConPTY asks of
//! the terminal it talks to, and a client terminal must not switch its
//! keyboard encoding. The host is that terminal: `QueryStripper` notes whether
//! the mode is on, and the host writes plain VT input but for Ctrl+letter key
//! sends to a program that reads key records (`keys::win32_ctrl_letter`, D6).

const ESC: u8 = 0x1b;
const BEL: u8 = 0x07;
/// CAN and SUB cancel a sequence.
const CAN: u8 = 0x18;
const SUB: u8 = 0x1a;
/// A longer CSI is not one this module judges; it passes as text.
const CSI_LIMIT: usize = 256;
/// A longer string (OSC, DCS, APC, PM, SOS) is passed on as it arrives.
const STRING_LIMIT: usize = 64 * 1024;
/// ESC, intermediates and a final byte.
const ESCAPE_LIMIT: usize = 8;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum StringKind {
    Osc,
    Dcs,
    /// APC, PM and SOS.
    Other,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum State {
    Ground,
    Escape,
    EscapeIntermediate,
    Csi,
    String(StringKind),
    /// ESC inside a string: `\` ends it.
    StringEscape(StringKind),
    /// A string too long to hold, passed on until it ends.
    Passing {
        kind: StringKind,
        escape: bool,
    },
}

/// A piece of a stream: bytes outside sequences, or one whole sequence.
#[derive(Debug, PartialEq, Eq)]
pub(crate) enum Piece<'a> {
    Text(&'a [u8]),
    Sequence(&'a [u8]),
}

/// How the scanner goes on after a byte of a held sequence.
enum Step {
    More,
    Done,
    /// The byte does not belong to the sequence: the held bytes before it are
    /// text, and the byte is scanned again from the ground state.
    Abort,
    /// The sequence is too long to hold; it passes as text.
    Overflow,
    /// A string ended with ESC and another byte instead of ST: its bytes are
    /// text, and that ESC starts the next sequence, which the byte continues.
    Restart,
}

/// Splits a stream into text and whole control sequences.
#[derive(Debug)]
pub(crate) struct Scanner {
    state: State,
    /// The sequence held so far.
    pending: Vec<u8>,
}

impl Default for Scanner {
    fn default() -> Self {
        Self {
            state: State::Ground,
            pending: Vec::new(),
        }
    }
}

impl Scanner {
    /// Whether an incomplete sequence is held.
    pub(crate) fn holding(&self) -> bool {
        !matches!(self.state, State::Ground | State::Passing { .. })
    }

    /// Give up a held sequence: its bytes become text.
    pub(crate) fn flush(&mut self, emit: &mut dyn FnMut(Piece<'_>)) {
        if self.holding() {
            emit(Piece::Text(&self.pending));
        }
        self.pending.clear();
        self.state = State::Ground;
    }

    /// Scan `bytes`, emitting text and whole sequences in order. An incomplete
    /// sequence at the end is held for the next call.
    pub(crate) fn feed(&mut self, bytes: &[u8], emit: &mut dyn FnMut(Piece<'_>)) {
        // The start of the text not yet emitted.
        let mut run = 0;
        let mut index = 0;
        while index < bytes.len() {
            let byte = bytes[index];
            match self.state {
                State::Ground => {
                    if byte == ESC {
                        if run < index {
                            emit(Piece::Text(&bytes[run..index]));
                        }
                        self.pending.clear();
                        self.pending.push(byte);
                        self.state = State::Escape;
                        run = index + 1;
                    }
                    index += 1;
                }
                State::Passing { kind, escape } => {
                    let ends = matches!(byte, CAN | SUB)
                        || (kind == StringKind::Osc && byte == BEL)
                        || (escape && byte == b'\\');
                    self.state = if ends {
                        State::Ground
                    } else {
                        State::Passing {
                            kind,
                            escape: byte == ESC,
                        }
                    };
                    index += 1;
                }
                _ => {
                    self.pending.push(byte);
                    index += 1;
                    run = index;
                    match self.step(byte) {
                        Step::More => {}
                        Step::Done => {
                            emit(Piece::Sequence(&self.pending));
                            self.pending.clear();
                            self.state = State::Ground;
                        }
                        Step::Abort => {
                            let held = self.pending.len() - 1;
                            emit(Piece::Text(&self.pending[..held]));
                            self.pending.clear();
                            self.state = State::Ground;
                            index -= 1;
                            run = index;
                        }
                        Step::Restart => {
                            let body = self.pending.len() - 2;
                            emit(Piece::Text(&self.pending[..body]));
                            self.pending.clear();
                            self.pending.push(ESC);
                            self.state = State::Escape;
                            index -= 1;
                            run = index;
                        }
                        Step::Overflow => {
                            emit(Piece::Text(&self.pending));
                            self.pending.clear();
                            self.state = match self.state {
                                State::String(kind) => State::Passing {
                                    kind,
                                    escape: false,
                                },
                                _ => State::Ground,
                            };
                        }
                    }
                }
            }
        }
        if !self.holding() && run < bytes.len() {
            emit(Piece::Text(&bytes[run..]));
        }
    }

    /// The next state after `byte`, which `pending` already ends with.
    fn step(&mut self, byte: u8) -> Step {
        let cancel = matches!(byte, CAN | SUB);
        match self.state {
            State::Ground | State::Passing { .. } => unreachable!("not holding a sequence"),
            State::Escape => match byte {
                b'[' => self.go(State::Csi),
                b']' => self.go(State::String(StringKind::Osc)),
                b'P' => self.go(State::String(StringKind::Dcs)),
                b'_' | b'^' | b'X' => self.go(State::String(StringKind::Other)),
                0x20..=0x2f => self.go(State::EscapeIntermediate),
                0x30..=0x7e => Step::Done,
                _ => Step::Abort,
            },
            State::EscapeIntermediate => match byte {
                _ if self.pending.len() > ESCAPE_LIMIT => Step::Overflow,
                0x20..=0x2f => Step::More,
                0x30..=0x7e => Step::Done,
                _ => Step::Abort,
            },
            State::Csi => match byte {
                0x40..=0x7e => Step::Done,
                _ if self.pending.len() > CSI_LIMIT => Step::Overflow,
                0x20..=0x3f => Step::More,
                _ => Step::Abort,
            },
            State::String(_) if cancel => Step::Abort,
            State::String(StringKind::Osc) if byte == BEL => Step::Done,
            State::String(kind) => {
                if byte == ESC {
                    self.go(State::StringEscape(kind))
                } else if self.pending.len() > STRING_LIMIT {
                    Step::Overflow
                } else {
                    Step::More
                }
            }
            State::StringEscape(_) if byte == b'\\' => Step::Done,
            State::StringEscape(_) => Step::Restart,
        }
    }

    fn go(&mut self, state: State) -> Step {
        self.state = state;
        Step::More
    }
}
/// A CSI sequence's parts: `ESC [ prefix params intermediates final`.
struct Csi<'a> {
    /// One of `<`, `=`, `>` and `?`, or 0.
    prefix: u8,
    params: &'a [u8],
    intermediates: &'a [u8],
    last: u8,
}

impl<'a> Csi<'a> {
    fn parse(raw: &'a [u8]) -> Option<Self> {
        let body = raw.strip_prefix(b"\x1b[")?;
        let (&last, body) = body.split_last()?;
        let (prefix, body) = match body.first() {
            Some(&prefix @ (b'<' | b'=' | b'>' | b'?')) => (prefix, &body[1..]),
            _ => (0, body),
        };
        let split = body
            .iter()
            .position(|byte| (0x20..=0x2f).contains(byte))
            .unwrap_or(body.len());
        Some(Self {
            prefix,
            params: &body[..split],
            intermediates: &body[split..],
            last,
        })
    }

    /// The numeric parameters; None when one is not a plain number.
    fn numbers(&self) -> Option<Vec<u32>> {
        if self.params.is_empty() {
            return Some(Vec::new());
        }
        self.params
            .split(|byte| *byte == b';')
            .map(|param| match param {
                [] => Some(0),
                _ if param.iter().all(u8::is_ascii_digit) => {
                    std::str::from_utf8(param).ok()?.parse().ok()
                }
                _ => None,
            })
            .collect()
    }

    fn first(&self) -> u32 {
        self.numbers()
            .and_then(|numbers| numbers.first().copied())
            .unwrap_or(0)
    }
}

/// An OSC's payload, without `ESC ]` and its terminator.
fn osc_payload(raw: &[u8]) -> Option<&[u8]> {
    let body = raw.strip_prefix(b"\x1b]")?;
    Some(
        body.strip_suffix(b"\x1b\\")
            .or_else(|| body.strip_suffix(&[BEL]))
            .unwrap_or(body),
    )
}

/// A DCS's intermediates and final byte, after its numeric parameters.
fn dcs_header(raw: &[u8]) -> Option<(&[u8], u8)> {
    let body = raw.strip_prefix(b"\x1bP")?;
    let start = body
        .iter()
        .position(|byte| !matches!(byte, b'0'..=b'9' | b';'))
        .unwrap_or(body.len());
    let body = &body[start..];
    let split = body.iter().position(|byte| !(0x20..=0x2f).contains(byte))?;
    Some((&body[..split], body[split]))
}

/// Whether `raw`, a whole sequence a program wrote, asks the terminal for an
/// answer: device attributes, status and cursor reports, mode reports, the
/// terminal version, keyboard and graphics settings, size reports, colour and
/// clipboard queries, setting and capability requests.
fn is_query(raw: &[u8]) -> bool {
    if let Some(csi) = Csi::parse(raw) {
        return match (csi.prefix, csi.intermediates, csi.last) {
            (0 | b'>' | b'=', b"", b'c') => true,
            (0 | b'?', b"", b'n') => true,
            (0 | b'?', b"$", b'p') => true,
            (b'>', b"", b'q') | (b'?', b"", b'u') | (b'?', b"", b'S') => true,
            (0, b"", b't') => matches!(csi.first(), 11 | 13 | 14 | 15 | 16 | 18 | 19 | 20 | 21),
            (0, b"", b'x') => csi.first() <= 1,
            _ => false,
        };
    }
    if let Some(payload) = osc_payload(raw) {
        let mut fields = payload.split(|byte| *byte == b';');
        let number = fields.next().unwrap_or_default();
        let known = matches!(
            number,
            b"4" | b"5"
                | b"10"
                | b"11"
                | b"12"
                | b"13"
                | b"14"
                | b"15"
                | b"16"
                | b"17"
                | b"18"
                | b"19"
                | b"52"
        );
        return known && fields.any(|field| field == b"?");
    }
    matches!(dcs_header(raw), Some((b"$" | b"+", b'q')))
}

/// Whether `raw`, a DECSET or DECRST that names mode 9001 (win32-input-mode),
/// sets or resets it, and `raw` without the mode; None for any other sequence.
fn win32_input_mode(raw: &[u8]) -> Option<(bool, Vec<u8>)> {
    let csi = Csi::parse(raw)?;
    if csi.prefix != b'?' || !csi.intermediates.is_empty() || !matches!(csi.last, b'h' | b'l') {
        return None;
    }
    let set = csi.last == b'h';
    let modes: Vec<&[u8]> = csi.params.split(|byte| *byte == b';').collect();
    if !modes.iter().any(|mode| *mode == b"9001") {
        return None;
    }
    let kept: Vec<&[u8]> = modes.into_iter().filter(|mode| *mode != b"9001").collect();
    if kept.is_empty() {
        return Some((set, Vec::new()));
    }
    let mut out = b"\x1b[?".to_vec();
    out.extend(kept.join(&b';'));
    out.push(csi.last);
    Some((set, out))
}

/// Whether `raw`, a whole sequence from a client's terminal, answers a query
/// rather than reports a key, the mouse, focus or a paste. Keys never produce
/// OSC, DCS or APC strings. A cursor position report has the shape of a
/// modified F3 (`CSI 1;N R`), which stays a key.
fn is_reply(raw: &[u8]) -> bool {
    if raw.starts_with(b"\x1b]")
        || raw.starts_with(b"\x1bP")
        || raw.starts_with(b"\x1b_")
        || raw.starts_with(b"\x1b^")
        || raw.starts_with(b"\x1bX")
    {
        return true;
    }
    let Some(csi) = Csi::parse(raw) else {
        return false;
    };
    match (csi.prefix, csi.intermediates, csi.last) {
        (b'?' | b'>', b"", b'c') => true,
        (b'?', b"", b'R') => true,
        (0, b"", b'R') => match csi.numbers().as_deref() {
            Some([1, modifier]) => !(2..=16).contains(modifier),
            Some([_, _]) => true,
            _ => false,
        },
        (0 | b'?', b"", b'n') => true,
        (0 | b'?', b"$", b'y') => true,
        (b'?', b"", b'u') | (b'?', b"", b'S') => true,
        (0, b"", b't' | b'x') => true,
        _ => false,
    }
}

/// The output relayed to attached clients: a session's output without the
/// queries in it, which the host alone answers, and without win32-input-mode,
/// which it notes.
#[derive(Debug, Default)]
pub(crate) struct QueryStripper {
    scanner: Scanner,
    /// The console has set win32-input-mode and not reset it since.
    win32_input: bool,
}

impl QueryStripper {
    /// What to relay for `bytes`. A sequence cut at the end is held until the
    /// rest arrives.
    pub(crate) fn strip(&mut self, bytes: &[u8]) -> Vec<u8> {
        let Self {
            scanner,
            win32_input,
        } = self;
        let mut out = Vec::with_capacity(bytes.len());
        scanner.feed(bytes, &mut |piece| match piece {
            Piece::Text(text) => out.extend_from_slice(text),
            Piece::Sequence(raw) if is_query(raw) => {}
            Piece::Sequence(raw) => match win32_input_mode(raw) {
                Some((set, rewritten)) => {
                    *win32_input = set;
                    out.extend(rewritten);
                }
                None => out.extend_from_slice(raw),
            },
        });
        out
    }

    /// Whether the console wants win32-input-mode (DECSET 9001), as ConPTY
    /// asks at its start; off until it does.
    pub(crate) fn win32_input(&self) -> bool {
        self.win32_input
    }
}

/// Input from one attached client without the answers its terminal sends to
/// queries: the host has answered them already.
#[derive(Debug, Default)]
pub(crate) struct ReplyFilter {
    scanner: Scanner,
}

impl ReplyFilter {
    /// What to pass on for `bytes`. A sequence cut at the end is held until
    /// the rest arrives or `flush` gives it up.
    pub(crate) fn filter(&mut self, bytes: &[u8]) -> Vec<u8> {
        let mut out = Vec::with_capacity(bytes.len());
        self.scanner.feed(bytes, &mut |piece| match piece {
            Piece::Text(text) => out.extend_from_slice(text),
            Piece::Sequence(raw) if is_reply(raw) => {}
            Piece::Sequence(raw) => out.extend_from_slice(raw),
        });
        out
    }

    /// Whether an incomplete sequence is held.
    pub(crate) fn holding(&self) -> bool {
        self.scanner.holding()
    }

    /// The held bytes as typed, once nothing completed them in time (a lone
    /// ESC, or Alt with `[`, `]` or `P`).
    pub(crate) fn flush(&mut self) -> Vec<u8> {
        let mut out = Vec::new();
        self.scanner.flush(&mut |piece| {
            let (Piece::Text(bytes) | Piece::Sequence(bytes)) = piece;
            out.extend_from_slice(bytes);
        });
        out
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// Every way to cut `bytes` in two reads gives `expected`.
    fn strips_whole_and_split(bytes: &[u8], expected: &[u8]) {
        assert_eq!(QueryStripper::default().strip(bytes), expected, "{bytes:?}");
        for cut in 0..=bytes.len() {
            let mut stripper = QueryStripper::default();
            let mut out = stripper.strip(&bytes[..cut]);
            out.extend(stripper.strip(&bytes[cut..]));
            assert_eq!(out, expected, "{bytes:?} cut at {cut}");
        }
        // One byte at a time.
        let mut stripper = QueryStripper::default();
        let out: Vec<u8> = bytes
            .iter()
            .flat_map(|byte| stripper.strip(&[*byte]))
            .collect();
        assert_eq!(out, expected, "{bytes:?} byte by byte");
    }

    fn filters_whole_and_split(bytes: &[u8], expected: &[u8]) {
        let mut filter = ReplyFilter::default();
        let mut out = filter.filter(bytes);
        out.extend(filter.flush());
        assert_eq!(out, expected, "{bytes:?}");
        for cut in 0..=bytes.len() {
            let mut filter = ReplyFilter::default();
            let mut out = filter.filter(&bytes[..cut]);
            out.extend(filter.filter(&bytes[cut..]));
            out.extend(filter.flush());
            assert_eq!(out, expected, "{bytes:?} cut at {cut}");
        }
    }

    #[test]
    fn queries_leave_the_relayed_output_and_everything_else_stays() {
        let queries: &[&[u8]] = &[
            b"\x1b[c",
            b"\x1b[0c",
            b"\x1b[>c",
            b"\x1b[>0c",
            b"\x1b[=c",
            b"\x1b[5n",
            b"\x1b[6n",
            b"\x1b[?6n",
            b"\x1b[?996n",
            b"\x1b[?2004$p",
            b"\x1b[4$p",
            b"\x1b[>0q",
            b"\x1b[>q",
            b"\x1b[?u",
            b"\x1b[?1;1S",
            b"\x1b[14t",
            b"\x1b[16t",
            b"\x1b[18t",
            b"\x1b[21t",
            b"\x1b[x",
            b"\x1b]10;?\x1b\\",
            b"\x1b]11;?\x07",
            b"\x1b]4;1;?\x07",
            b"\x1b]10;?;?\x07",
            b"\x1b]52;c;?\x1b\\",
            b"\x1bP$qm\x1b\\",
            b"\x1bP$q q\x1b\\",
            b"\x1bP+q544e\x1b\\",
        ];
        for query in queries {
            let mut stream = b"a\x1b[1mb".to_vec();
            stream.extend_from_slice(query);
            stream.extend_from_slice(b"c\r\n");
            strips_whole_and_split(&stream, b"a\x1b[1mbc\r\n");
        }
        let kept: &[&[u8]] = &[
            b"plain text \xe2\x94\x80 and UTF-8",
            b"\x1b[0m\x1b[38;5;246m\x1b[48;2;1;2;3m",
            b"\x1b[H\x1b[2J\x1b[10;20H\x1b[K",
            b"\x1b[?2004h\x1b[?1049h\x1b[?1h\x1b=\x1b[?1004h",
            b"\x1b[?25l\x1b[?12l\x1b[2 q",
            b"\x1b]0;title\x07\x1b]2;other\x1b\\",
            b"\x1b]8;;https://example.com\x1b\\link\x1b]8;;\x1b\\",
            b"\x1b]52;c;aGVsbG8=\x07",
            b"\x1b]11;rgb:0000/0000/0000\x07",
            b"\x1b[22;0;0t\x1b[23;0;0t",
            b"\x1b[>4;1m\x1b[>1u\x1b[<u\x1b[s\x1b[u",
            b"\x1b7\x1b8\x1bM\x1b(B\x1b)0\x0e\x0f",
            b"\x1bP1000p\x1b\\",
            b"\x1b_Gf=24;AAAA\x1b\\",
        ];
        for stream in kept {
            strips_whole_and_split(stream, stream);
        }
    }

    #[test]
    fn win32_input_mode_never_reaches_a_client() {
        strips_whole_and_split(b"\x1b[?9001h\x1b[?1004h", b"\x1b[?1004h");
        strips_whole_and_split(b"\x1b[?9001l", b"");
        strips_whole_and_split(b"\x1b[?1004;9001;25h", b"\x1b[?1004;25h");
        strips_whole_and_split(b"\x1b[?90011h", b"\x1b[?90011h");
    }

    /// Every way to cut `bytes` in reads relays `expected` and leaves the
    /// console's win32-input-mode at `mode`.
    fn notes_whole_and_split(bytes: &[u8], mode: bool, expected: &[u8]) {
        let mut stripper = QueryStripper::default();
        assert_eq!(stripper.strip(bytes), expected, "{bytes:?}");
        assert_eq!(stripper.win32_input(), mode, "{bytes:?}");
        for cut in 0..=bytes.len() {
            let mut stripper = QueryStripper::default();
            let mut out = stripper.strip(&bytes[..cut]);
            out.extend(stripper.strip(&bytes[cut..]));
            assert_eq!(out, expected, "{bytes:?} cut at {cut}");
            assert_eq!(stripper.win32_input(), mode, "{bytes:?} cut at {cut}");
        }
        let mut stripper = QueryStripper::default();
        for byte in bytes {
            stripper.strip(&[*byte]);
        }
        assert_eq!(stripper.win32_input(), mode, "{bytes:?} byte by byte");
    }

    #[test]
    fn win32_input_mode_is_noted_from_the_output() {
        assert!(!QueryStripper::default().win32_input());
        notes_whole_and_split(b"\x1b[?9001h\x1b[?1004h", true, b"\x1b[?1004h");
        notes_whole_and_split(b"ab\x1b[?1004;9001;25hcd", true, b"ab\x1b[?1004;25hcd");
        notes_whole_and_split(b"\x1b[?9001h\x1b[?9001l", false, b"");
        notes_whole_and_split(b"\x1b[?9001hx\x1b[?25;9001ly", false, b"x\x1b[?25ly");
        notes_whole_and_split(b"\x1b[?9001l\x1b[?9001h", true, b"");
        // Other modes, ANSI mode 9001, a mode report query and strings.
        for other in [
            &b"\x1b[?90011h"[..],
            b"\x1b[?900h",
            b"\x1b[9001h",
            b"\x1b]0;?9001h\x07",
            b"\x1bP?9001h\x1b\\",
        ] {
            notes_whole_and_split(other, false, other);
        }
        notes_whole_and_split(b"\x1b[?9001$p", false, b"");
        // The mode lasts until the console resets it.
        let mut stripper = QueryStripper::default();
        stripper.strip(b"\x1b[?9001h");
        stripper.strip(b"text\x1b[?1049h\x1b[?9001");
        assert!(stripper.win32_input());
        assert_eq!(stripper.strip(b";1004l"), b"\x1b[?1004l");
        assert!(!stripper.win32_input());
    }

    #[test]
    fn broken_and_oversized_sequences_pass_unchanged() {
        // A sequence cut by CAN, by ESC or by a C0 byte is text up to there.
        strips_whole_and_split(b"\x1b[6\x18n", b"\x1b[6\x18n");
        strips_whole_and_split(b"\x1b[6\x1b[c", b"\x1b[6");
        strips_whole_and_split(b"\x1b[1\r5n", b"\x1b[1\r5n");
        strips_whole_and_split(b"\x1b\x1b[5n", b"\x1b");
        // An OSC ended by ESC and another byte: the ESC starts the next one.
        strips_whole_and_split(b"\x1b]11;?\x1b[c", b"\x1b]11;?");
        // A string too long to judge passes on as it arrives, to its end.
        let mut long = b"\x1b]52;c;".to_vec();
        long.extend(std::iter::repeat_n(b'A', STRING_LIMIT + 10));
        long.extend_from_slice(b"\x07after\x1b[6n");
        let mut expected = long.clone();
        expected.truncate(expected.len() - 4);
        assert_eq!(QueryStripper::default().strip(&long), expected);
        let long_csi: Vec<u8> = [&b"\x1b["[..], &[b'1'; CSI_LIMIT + 5], b"n"].concat();
        assert_eq!(QueryStripper::default().strip(&long_csi), long_csi);
        // A held sequence waits for its end; nothing is lost meanwhile.
        let mut stripper = QueryStripper::default();
        assert_eq!(stripper.strip(b"ab\x1b[3"), b"ab");
        assert_eq!(stripper.strip(b"1m"), b"\x1b[31m");
    }

    #[test]
    fn client_replies_are_dropped_and_keys_stay() {
        let replies: &[&[u8]] = &[
            b"\x1b[?1;2c",
            b"\x1b[?62;22c",
            b"\x1b[>84;0;0c",
            b"\x1b[>0;10;1c",
            b"\x1b[12;40R",
            b"\x1b[?12;40R",
            b"\x1b[0n",
            b"\x1b[3n",
            b"\x1b[?997;1n",
            b"\x1b[?2004;1$y",
            b"\x1b[4;2$y",
            b"\x1b[?0u",
            b"\x1b[?1;0;256S",
            b"\x1b[8;24;80t",
            b"\x1b[4;600;800t",
            b"\x1b[2;1;1;112;112;1;0x",
            b"\x1b]11;rgb:1e1e/1e1e/1e1e\x1b\\",
            b"\x1b]10;rgb:cccc/cccc/cccc\x07",
            b"\x1b]52;c;aGk=\x07",
            b"\x1bP>|WindowsTerminal 1.22\x1b\\",
            b"\x1bP1$r0m\x1b\\",
            b"\x1bP1+r544e=787465726d\x1b\\",
            b"\x1bP!|00000000\x1b\\",
            b"\x1b_Gi=1;OK\x1b\\",
        ];
        for reply in replies {
            let mut stream = b"a".to_vec();
            stream.extend_from_slice(reply);
            stream.extend_from_slice(b"b\r");
            filters_whole_and_split(&stream, b"ab\r");
        }
        let keys: &[&[u8]] = &[
            b"typed text \xc3\xa9\r",
            b"\x1b[A\x1b[B\x1bOC\x1bOD\x1b[1;5A\x1b[3~\x1b[15;2~\x1b[Z",
            b"\x1b[1;2R\x1b[1;5R\x1bOR",
            b"\x1b[<0;10;5M\x1b[<0;10;5m\x1b[M !!",
            b"\x1b[I\x1b[O",
            b"\x1b[200~pasted\nlines\x1b[201~",
            b"\x1ba\x1b\r\x1b\x7f",
            b"\x1b[97;5u\x1b[13u",
            b"\x03\x13\x18\x1a",
        ];
        for stream in keys {
            filters_whole_and_split(stream, stream);
        }
    }

    #[test]
    fn a_held_escape_is_given_up_as_typed() {
        let mut filter = ReplyFilter::default();
        assert_eq!(filter.filter(b"x\x1b"), b"x");
        assert!(filter.holding());
        assert_eq!(filter.flush(), b"\x1b");
        assert!(!filter.holding());
        // Alt+P starts a device string; given up, it is the keys again.
        assert_eq!(filter.filter(b"\x1bPab"), b"");
        assert!(filter.holding());
        assert_eq!(filter.flush(), b"\x1bPab");
        assert_eq!(filter.filter(b"\x1b[1;2"), b"");
        assert_eq!(filter.filter(b"R"), b"\x1b[1;2R");
        assert_eq!(filter.flush(), b"");
    }
}
