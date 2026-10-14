//! The attach client (D13): `hgs.exe a` on a Windows console, relaying one
//! session's stream from the session host.
//!
//! The host sends a repaint of the pane's screen model and then the console's
//! output without its queries (D6); this client writes both to its own console
//! as they come and sends what is typed back, so the client terminal keeps its
//! own scrollback and selection. It does not draw a screen of its own as the
//! tmux client does: there is no alternate screen around the session and no
//! status line, and a notice the host sends (`hgs paste` on Unix) has nowhere
//! to show, so it is ignored.
//!
//! The portable parts are tested everywhere:
//!
//! - `PrefixKeys`: tmux's default prefix for the three bindings D13 names.
//!   `Ctrl+B d` and `Ctrl+B Ctrl+D` detach and `Ctrl+B Ctrl+B` sends one
//!   Ctrl+B (tmux `send-prefix`). Any other key after Ctrl+B passes through
//!   with the Ctrl+B before it, unchanged, where tmux would run or swallow a
//!   binding: the client has no other bindings, and dropping keys would lose
//!   input the user meant for the agent. As in tmux, keys inside a bracketed
//!   paste are text, and ESC followed by Ctrl+B in one read is Meta+Ctrl+B,
//!   not the prefix. A prefix waits for its next key without a time limit.
//! - `Utf8Text`: the stream's UTF-8 as text for `WriteConsoleW`, a sequence
//!   split across reads kept until its last byte arrives, invalid bytes shown
//!   as U+FFFD; the console's code page never changes.
//! - `ScreenModes` and `relay`: the client loop over any `Terminal`. Input runs
//!   on a thread of its own that also follows the terminal's size, sending a
//!   resize control whenever it changes; the pane follows the most recent
//!   client (host side). When the stream ends (a detach, the session's exit or
//!   a lost host) the terminal is reset as tmux resets it on detach: mouse,
//!   paste, focus and keypad modes off, the cursor shown, the alternate screen
//!   left when the session had entered it, the cursor placed below the last
//!   row; then tmux's own line, `[detached (from session NAME)]` or
//!   `[exited]`.
//!
//! On Windows, `Console` is this process's console (standard input and output
//! must both be one; `hgs a` refuses pipes and files, as tmux refuses a client
//! without a terminal). Its attach modes clear `ENABLE_PROCESSED_INPUT` (Ctrl+C
//! arrives as `\x03`), `ENABLE_LINE_INPUT` and `ENABLE_ECHO_INPUT`, and set
//! `ENABLE_VIRTUAL_TERMINAL_INPUT` and `ENABLE_WINDOW_INPUT` (size changes
//! arrive as `WINDOW_BUFFER_SIZE_EVENT`; the window size is also polled, as a
//! classic console window reports no event when only its height changes);
//! output gets `ENABLE_VIRTUAL_TERMINAL_PROCESSING` and
//! `DISABLE_NEWLINE_AUTO_RETURN`. Ctrl+C and Ctrl+Break events are ignored
//! while attached. `leave` restores every mode it found, after a detach, an
//! error or the host's death. The same client runs inside the GUI's embedded
//! terminal, which starts `hgs.exe a NAME --existing --run-id ID` on a
//! pseudoconsole of its own.
use super::{
    client::{AttachStream, Event},
    protocol::{Control, Size},
    sequences::{Piece, Scanner},
};
use std::{
    io,
    sync::{
        atomic::{AtomicBool, Ordering},
        Mutex, PoisonError,
    },
    thread,
    time::{Duration, Instant},
};

#[cfg(windows)]
pub(crate) use self::console::Console;

/// Ctrl+B, tmux's default prefix key.
pub(crate) const PREFIX: u8 = 0x02;
/// Ctrl+D.
const CONTROL_D: u8 = 0x04;
const ESC: u8 = 0x1b;
/// The markers a terminal sends around a bracketed paste.
const PASTE_START: &[u8] = b"\x1b[200~";
const PASTE_END: &[u8] = b"\x1b[201~";
/// How long one wait for input lasts; the input thread checks between waits
/// whether the stream has ended and whether the terminal's size changed.
const POLL: Duration = Duration::from_millis(100);
/// How long a detach waits for the host to close the stream.
const DETACH_WAIT: Duration = Duration::from_secs(5);

/// What `hgs a` reports when standard input or output is not a console.
pub(crate) const NOT_A_CONSOLE: &str =
    "attaching needs a console: standard input and output are not both a console window";

/// Keys typed in the client terminal, after the prefix bindings.
#[derive(Debug, Default, PartialEq, Eq)]
pub(crate) struct Keys {
    /// What goes to the session.
    pub bytes: Vec<u8>,
    /// A detach binding was typed; nothing typed after it in the same read
    /// goes anywhere.
    pub detach: bool,
}

/// The prefix bindings of the attach client (see the module documentation).
#[derive(Debug, Default)]
pub(crate) struct PrefixKeys {
    /// Ctrl+B was typed and waits for the next key.
    prefix: bool,
    /// The last byte of this read was ESC: a Ctrl+B now is Meta+Ctrl+B.
    escape: bool,
    /// Inside a bracketed paste, whose keys are text.
    pasting: bool,
    /// How much of the next paste marker has been seen.
    marker: usize,
}

impl PrefixKeys {
    /// The keys of one read.
    pub(crate) fn feed(&mut self, input: &[u8]) -> Keys {
        let mut bytes = Vec::with_capacity(input.len() + 1);
        // An ESC from an earlier read was a key of its own (a lone Escape).
        self.escape = false;
        for &byte in input {
            let pasting = self.pasting;
            self.follow_paste(byte);
            if pasting {
                bytes.push(byte);
                continue;
            }
            if self.prefix {
                self.prefix = false;
                match byte {
                    b'd' | CONTROL_D => {
                        return Keys {
                            bytes,
                            detach: true,
                        }
                    }
                    PREFIX => {
                        bytes.push(PREFIX);
                        continue;
                    }
                    _ => bytes.push(PREFIX),
                }
            } else if byte == PREFIX && !self.escape {
                self.prefix = true;
                continue;
            }
            self.escape = byte == ESC;
            bytes.push(byte);
        }
        Keys {
            bytes,
            detach: false,
        }
    }

    /// Whether a Ctrl+B waits for its next key.
    pub(crate) fn pending(&self) -> bool {
        self.prefix
    }

    fn follow_paste(&mut self, byte: u8) {
        let marker = if self.pasting { PASTE_END } else { PASTE_START };
        if byte == marker[self.marker] {
            self.marker += 1;
            if self.marker == marker.len() {
                self.pasting = !self.pasting;
                self.marker = 0;
            }
        } else {
            // Both markers begin with their only ESC.
            self.marker = usize::from(byte == ESC);
        }
    }
}

/// UTF-8 bytes as text, across reads that may split a character.
#[derive(Debug, Default)]
pub(crate) struct Utf8Text {
    /// The start of a character whose other bytes have not arrived (at most 3).
    pending: Vec<u8>,
}

impl Utf8Text {
    /// The text of `bytes` after what an earlier call held: every complete
    /// character, U+FFFD for each invalid sequence; an incomplete character at
    /// the end waits for the next call.
    pub(crate) fn push(&mut self, bytes: &[u8]) -> String {
        let mut input = std::mem::take(&mut self.pending);
        input.extend_from_slice(bytes);
        let mut text = String::with_capacity(input.len());
        let mut rest = input.as_slice();
        loop {
            match std::str::from_utf8(rest) {
                Ok(valid) => {
                    text.push_str(valid);
                    break;
                }
                Err(error) => {
                    let (valid, after) = rest.split_at(error.valid_up_to());
                    text.push_str(std::str::from_utf8(valid).unwrap_or_default());
                    match error.error_len() {
                        Some(length) => {
                            text.push(char::REPLACEMENT_CHARACTER);
                            rest = &after[length..];
                        }
                        None => {
                            self.pending = after.to_vec();
                            break;
                        }
                    }
                }
            }
        }
        text
    }

    /// The end of the stream: a character still incomplete shows as U+FFFD.
    pub(crate) fn finish(&mut self) -> String {
        if self.pending.is_empty() {
            return String::new();
        }
        self.pending.clear();
        char::REPLACEMENT_CHARACTER.to_string()
    }
}

/// The client terminal's modes a session may leave behind, from its output.
#[derive(Debug, Default)]
pub(crate) struct ScreenModes {
    scanner: Scanner,
    /// The session entered the alternate screen (DECSET 1049, 1047 or 47).
    alternate: bool,
}

/// What every reset turns off: the scroll region, the character set and
/// attributes, origin mode, application cursor keys and keypad, the cursor
/// shape, mouse, bracketed paste and focus reporting; the cursor is shown.
const RESET: &str = "\x1b[r\x1b(B\x1b[m\x1b[?6l\x1b[?1l\x1b>\x1b[0 q\x1b[?25h\
                     \x1b[?1000l\x1b[?1002l\x1b[?1003l\x1b[?1006l\x1b[?1005l\x1b[?2004l\x1b[?1004l";

impl ScreenModes {
    /// Follow the session's output.
    pub(crate) fn feed(&mut self, bytes: &[u8]) {
        let Self { scanner, alternate } = self;
        scanner.feed(bytes, &mut |piece| {
            let Piece::Sequence(sequence) = piece else {
                return;
            };
            if let Some((modes, set)) = private_modes(sequence) {
                if modes
                    .iter()
                    .any(|mode| matches!(*mode, b"1049" | b"1047" | b"47"))
                {
                    *alternate = set;
                }
            }
        });
    }

    /// Whether the session left the client terminal in the alternate screen.
    pub(crate) fn alternate(&self) -> bool {
        self.alternate
    }

    /// The sequences that give the client terminal back as tmux leaves it on
    /// detach, with the cursor at the start of a new line below row `rows`.
    pub(crate) fn reset(&self, rows: u16) -> String {
        let mut text = String::new();
        if self.alternate {
            text.push_str("\x1b[?1049l");
        }
        text.push_str(RESET);
        text.push_str(&format!("\x1b[{};1H\r\n", rows.max(1)));
        text
    }
}

/// The modes of a DECSET (`CSI ? … h`) or DECRST (`CSI ? … l`), and whether it
/// sets them.
fn private_modes(sequence: &[u8]) -> Option<(Vec<&[u8]>, bool)> {
    let body = sequence.strip_prefix(b"\x1b[?")?;
    let (last, modes) = body.split_last()?;
    let set = match last {
        b'h' => true,
        b'l' => false,
        _ => return None,
    };
    Some((modes.split(|byte| *byte == b';').collect(), set))
}

/// The client's terminal: this process's console on Windows, a fake in tests.
pub(crate) trait Terminal: Sync {
    /// Its visible size in cells, None when unknown.
    fn size(&self) -> Option<Size>;
    /// Switch to the attach modes (D13). `leave` undoes it.
    fn enter(&self) -> io::Result<()>;
    /// Restore every mode `enter` found; nothing when it did not run.
    fn leave(&self);
    /// What was typed, as UTF-8 and terminal sequences, waiting at most
    /// `timeout`; empty when nothing came. An error means the terminal is gone.
    fn read(&self, timeout: Duration) -> io::Result<Vec<u8>>;
    /// Show `text`.
    fn write(&self, text: &str) -> io::Result<()>;
}

/// How an attached client ended.
#[derive(Debug, PartialEq, Eq)]
pub(crate) enum Outcome {
    /// The user detached; the session runs on.
    Detached,
    /// The session ended, with its supervisor's exit code when known.
    Exited(Option<i32>),
    /// The stream ended without a detach or the session's end: the host died
    /// or the connection broke. The reason, for the user.
    Lost(String),
    /// The client terminal failed; the client detached.
    Failed(String),
}

fn lock<T>(mutex: &Mutex<T>) -> std::sync::MutexGuard<'_, T> {
    mutex.lock().unwrap_or_else(PoisonError::into_inner)
}

/// Relay `stream` of session `name` and `terminal` until the stream ends, then
/// reset the terminal and show tmux's line for a detach or an exit.
/// `terminal` is in its attach modes, and `size` is the size the attach
/// request named.
pub(crate) fn relay(
    stream: &AttachStream,
    terminal: &dyn Terminal,
    name: &str,
    size: Size,
) -> Outcome {
    let stop = AtomicBool::new(false);
    let detaching = AtomicBool::new(false);
    let failed: Mutex<Option<String>> = Mutex::new(None);
    let mut modes = ScreenModes::default();
    let mut text = Utf8Text::default();
    let outcome = thread::scope(|scope| {
        let input = thread::Builder::new()
            .name("hgs-attach-input".into())
            .spawn_scoped(scope, || {
                type_keys(stream, terminal, size, &stop, &detaching, &failed)
            });
        let input = match input {
            Ok(input) => input,
            Err(error) => {
                let _ = stream.detach();
                return Outcome::Failed(error.to_string());
            }
        };
        let mut exited = None;
        // Err: this console failed; Ok: why the stream ended.
        let end = loop {
            match stream.next() {
                Ok(Some(Event::Output(bytes))) => {
                    modes.feed(&bytes);
                    let shown = text.push(&bytes);
                    if !shown.is_empty() {
                        if let Err(error) = terminal.write(&shown) {
                            let _ = stream.detach();
                            break Err(error.to_string());
                        }
                    }
                }
                Ok(Some(Event::Control(Control::Exit { code }))) => exited = Some(code),
                // Resize and detach go to the host, never from it; a notice
                // has no status line to show on.
                Ok(Some(Event::Control(_))) => {}
                Ok(None) => break Ok("the session host closed the connection".to_owned()),
                Err(error) => break Ok(error.to_string()),
            }
        };
        stop.store(true, Ordering::SeqCst);
        // The host closes the stream as soon as it reads a detach, which
        // can be before the input thread has recorded it: only once that
        // thread has ended does `detaching` tell a detach from a lost host.
        let _ = input.join();
        match end {
            Err(error) => Outcome::Failed(error),
            Ok(reason) => ended(&detaching, &failed, exited, reason),
        }
    });
    if matches!(outcome, Outcome::Failed(_)) {
        return outcome;
    }
    let mut shown = text.finish();
    let rows = terminal.size().unwrap_or(size).rows;
    shown.push_str(&modes.reset(rows));
    match &outcome {
        Outcome::Detached => shown.push_str(&format!("[detached (from session {name})]\r\n")),
        Outcome::Exited(_) => shown.push_str("[exited]\r\n"),
        _ => {}
    }
    let _ = terminal.write(&shown);
    outcome
}

/// How a stream that ended came to its end.
fn ended(
    detaching: &AtomicBool,
    failed: &Mutex<Option<String>>,
    exited: Option<Option<i32>>,
    reason: String,
) -> Outcome {
    if let Some(error) = lock(failed).take() {
        return Outcome::Failed(error);
    }
    if let Some(code) = exited {
        return Outcome::Exited(code);
    }
    if detaching.load(Ordering::SeqCst) {
        return Outcome::Detached;
    }
    Outcome::Lost(reason)
}

/// The input thread: keys to the session, size changes as resize controls,
/// until the stream ends or a detach is typed.
fn type_keys(
    stream: &AttachStream,
    terminal: &dyn Terminal,
    mut size: Size,
    stop: &AtomicBool,
    detaching: &AtomicBool,
    failed: &Mutex<Option<String>>,
) {
    let mut keys = PrefixKeys::default();
    let leave = |error: Option<String>| {
        if let Some(error) = error {
            *lock(failed) = Some(error);
        }
        // A detach that cannot be sent finds the stream broken: the output
        // side sees that too.
        if stream.detach().is_err() {
            return;
        }
        detaching.store(true, Ordering::SeqCst);
        // The host closes the stream; one that does not is closed here.
        let deadline = Instant::now() + DETACH_WAIT;
        while !stop.load(Ordering::SeqCst) && Instant::now() < deadline {
            thread::sleep(Duration::from_millis(20));
        }
        if !stop.load(Ordering::SeqCst) {
            stream.close();
        }
    };
    while !stop.load(Ordering::SeqCst) {
        let typed = match terminal.read(POLL) {
            Ok(typed) => typed,
            Err(error) => return leave(Some(error.to_string())),
        };
        if let Some(now) = terminal.size() {
            if now != size && now.cols > 0 && now.rows > 0 {
                if stream.resize(now).is_err() {
                    return;
                }
                size = now;
            }
        }
        if typed.is_empty() {
            continue;
        }
        let Keys { bytes, detach } = keys.feed(&typed);
        if !bytes.is_empty() && stream.send(&bytes).is_err() {
            return;
        }
        if detach {
            return leave(None);
        }
    }
}

#[cfg(windows)]
mod console {
    use super::{lock, Size, Terminal};
    use std::{io, ptr, sync::Mutex, time::Duration};
    use windows_sys::Win32::{
        Foundation::{HANDLE, INVALID_HANDLE_VALUE, WAIT_OBJECT_0, WAIT_TIMEOUT},
        System::{
            Console::{
                GetConsoleMode, GetConsoleScreenBufferInfo, GetNumberOfConsoleInputEvents,
                GetStdHandle, ReadConsoleInputW, SetConsoleCtrlHandler, SetConsoleMode,
                WriteConsoleW, CONSOLE_MODE, CONSOLE_SCREEN_BUFFER_INFO, CTRL_BREAK_EVENT,
                CTRL_C_EVENT, DISABLE_NEWLINE_AUTO_RETURN, ENABLE_ECHO_INPUT, ENABLE_LINE_INPUT,
                ENABLE_PROCESSED_INPUT, ENABLE_PROCESSED_OUTPUT, ENABLE_VIRTUAL_TERMINAL_INPUT,
                ENABLE_VIRTUAL_TERMINAL_PROCESSING, ENABLE_WINDOW_INPUT, INPUT_RECORD, KEY_EVENT,
                STD_INPUT_HANDLE, STD_OUTPUT_HANDLE,
            },
            Threading::WaitForSingleObject,
        },
    };

    /// Input records read at once.
    const RECORDS: usize = 128;
    /// UTF-16 units written at once; a pair is never split.
    const CHUNK: usize = 8192;
    /// The Alt key, on whose release a character composed with Alt and the
    /// numeric keypad arrives.
    const VK_MENU: u16 = 0x12;

    /// A standard handle of this process.
    #[derive(Clone, Copy)]
    struct Handle(HANDLE);

    // SAFETY: a console handle is a kernel handle that every thread of this
    // process may use; the console serializes the calls made on it.
    unsafe impl Send for Handle {}
    // SAFETY: as above; `Handle` only hands the value to console functions.
    unsafe impl Sync for Handle {}

    /// The mode of console handle `handle`, None when it is not a console.
    fn mode(handle: HANDLE) -> Option<CONSOLE_MODE> {
        if handle.is_null() || handle == INVALID_HANDLE_VALUE {
            return None;
        }
        let mut mode = 0;
        // SAFETY: `handle` is one of this process's standard handles, and
        // `mode` is writable; the call fails for anything but a console.
        (unsafe { GetConsoleMode(handle, &mut mode) } != 0).then_some(mode)
    }

    fn set_mode(handle: Handle, mode: CONSOLE_MODE) -> io::Result<()> {
        // SAFETY: `handle` is a console handle that `Console::open` checked.
        if unsafe { SetConsoleMode(handle.0, mode) } == 0 {
            return Err(io::Error::last_os_error());
        }
        Ok(())
    }

    /// Ctrl+C reaches the session as input; neither it nor Ctrl+Break ends
    /// the client. The console's closing still does.
    unsafe extern "system" fn ignore_interrupts(event: u32) -> i32 {
        i32::from(matches!(event, CTRL_C_EVENT | CTRL_BREAK_EVENT))
    }

    /// This process's console as the attach client's terminal.
    pub(crate) struct Console {
        input: Handle,
        output: Handle,
        /// The input and output modes found at start, which `leave` restores.
        found: (CONSOLE_MODE, CONSOLE_MODE),
        entered: Mutex<bool>,
        /// A high surrogate whose low half has not been read yet.
        surrogate: Mutex<Option<u16>>,
    }

    impl Console {
        /// This process's console, when standard input and output are both
        /// one; None for a pipe, a file or no console.
        pub(crate) fn open() -> Option<Self> {
            // SAFETY: GetStdHandle only reads this process's standard handles.
            let (input, output) = unsafe {
                (
                    GetStdHandle(STD_INPUT_HANDLE),
                    GetStdHandle(STD_OUTPUT_HANDLE),
                )
            };
            let found = (mode(input)?, mode(output)?);
            Some(Self {
                input: Handle(input),
                output: Handle(output),
                found,
                entered: Mutex::new(false),
                surrogate: Mutex::new(None),
            })
        }

        /// Append the UTF-8 of UTF-16 unit `unit` to `bytes`, joining
        /// surrogate pairs that arrive in separate records.
        fn push_unit(&self, unit: u16, bytes: &mut Vec<u8>) {
            let mut surrogate = lock(&self.surrogate);
            let mut push = |character: char| {
                let mut buffer = [0; 4];
                bytes.extend_from_slice(character.encode_utf8(&mut buffer).as_bytes());
            };
            match unit {
                0xD800..=0xDBFF => {
                    if surrogate.replace(unit).is_some() {
                        push(char::REPLACEMENT_CHARACTER);
                    }
                }
                0xDC00..=0xDFFF => match surrogate.take() {
                    Some(high) => {
                        let code = 0x10000
                            + ((u32::from(high) - 0xD800) << 10)
                            + (u32::from(unit) - 0xDC00);
                        push(char::from_u32(code).unwrap_or(char::REPLACEMENT_CHARACTER));
                    }
                    None => push(char::REPLACEMENT_CHARACTER),
                },
                _ => {
                    if surrogate.take().is_some() {
                        push(char::REPLACEMENT_CHARACTER);
                    }
                    push(char::from_u32(u32::from(unit)).unwrap_or(char::REPLACEMENT_CHARACTER));
                }
            }
        }
    }

    impl Terminal for Console {
        fn size(&self) -> Option<Size> {
            let mut info = CONSOLE_SCREEN_BUFFER_INFO::default();
            // SAFETY: the output handle is a console screen buffer and `info`
            // is writable.
            if unsafe { GetConsoleScreenBufferInfo(self.output.0, &mut info) } == 0 {
                return None;
            }
            let window = info.srWindow;
            let cells =
                |low: i16, high: i16| u16::try_from(i32::from(high) - i32::from(low) + 1).ok();
            let size = Size {
                cols: cells(window.Left, window.Right)?,
                rows: cells(window.Top, window.Bottom)?,
            };
            (size.cols > 0 && size.rows > 0).then_some(size)
        }

        fn enter(&self) -> io::Result<()> {
            let mut entered = lock(&self.entered);
            if *entered {
                return Ok(());
            }
            let (input, output) = self.found;
            let raw = (input & !(ENABLE_PROCESSED_INPUT | ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT))
                | ENABLE_VIRTUAL_TERMINAL_INPUT
                | ENABLE_WINDOW_INPUT;
            set_mode(self.input, raw).map_err(|error| {
                io::Error::new(
                    error.kind(),
                    format!("the console does not take terminal input sequences: {error}"),
                )
            })?;
            let sequences = output | ENABLE_PROCESSED_OUTPUT | ENABLE_VIRTUAL_TERMINAL_PROCESSING;
            if let Err(error) = set_mode(self.output, sequences | DISABLE_NEWLINE_AUTO_RETURN)
                .or_else(|_| set_mode(self.output, sequences))
            {
                let _ = set_mode(self.input, input);
                return Err(io::Error::new(
                    error.kind(),
                    format!("the console does not show terminal sequences: {error}"),
                ));
            }
            // SAFETY: `ignore_interrupts` is a handler routine that stays valid
            // for the life of the process; `leave` removes it.
            unsafe {
                SetConsoleCtrlHandler(Some(ignore_interrupts), 1);
            }
            *entered = true;
            Ok(())
        }

        fn leave(&self) {
            let mut entered = lock(&self.entered);
            if !*entered {
                return;
            }
            let _ = set_mode(self.input, self.found.0);
            let _ = set_mode(self.output, self.found.1);
            // SAFETY: removes the routine `enter` added.
            unsafe {
                SetConsoleCtrlHandler(Some(ignore_interrupts), 0);
            }
            *entered = false;
        }

        fn read(&self, timeout: Duration) -> io::Result<Vec<u8>> {
            let milliseconds = u32::try_from(timeout.as_millis()).unwrap_or(u32::MAX - 1);
            // SAFETY: the input handle is a console input buffer, which is
            // signaled while it holds records.
            match unsafe { WaitForSingleObject(self.input.0, milliseconds) } {
                WAIT_OBJECT_0 => {}
                WAIT_TIMEOUT => return Ok(Vec::new()),
                _ => return Err(io::Error::last_os_error()),
            }
            let mut waiting = 0;
            // SAFETY: as above; `waiting` is writable.
            if unsafe { GetNumberOfConsoleInputEvents(self.input.0, &mut waiting) } == 0 {
                return Err(io::Error::last_os_error());
            }
            if waiting == 0 {
                return Ok(Vec::new());
            }
            let mut records = [INPUT_RECORD::default(); RECORDS];
            let mut read = 0;
            // SAFETY: `records` holds RECORDS writable records and `read` is
            // writable. Records are waiting, so the call does not block.
            if unsafe {
                ReadConsoleInputW(
                    self.input.0,
                    records.as_mut_ptr(),
                    RECORDS as u32,
                    &mut read,
                )
            } == 0
            {
                return Err(io::Error::last_os_error());
            }
            let mut bytes = Vec::new();
            for record in &records[..(read as usize).min(RECORDS)] {
                if u32::from(record.EventType) != KEY_EVENT {
                    // A WINDOW_BUFFER_SIZE_EVENT only ends the wait: the
                    // caller reads the size after every read.
                    continue;
                }
                // SAFETY: the record is a key event, so `KeyEvent` is the
                // union member the console wrote.
                let key = unsafe { record.Event.KeyEvent };
                // SAFETY: both members of the character union are plain
                // integers of the record the console wrote.
                let unit = unsafe { key.uChar.UnicodeChar };
                // Terminal sequences arrive as key-down records; a character
                // composed with Alt and the keypad comes with Alt's release.
                let typed = key.bKeyDown != 0 || key.wVirtualKeyCode == VK_MENU;
                if !typed || unit == 0 {
                    continue;
                }
                for _ in 0..key.wRepeatCount.max(1) {
                    self.push_unit(unit, &mut bytes);
                }
            }
            Ok(bytes)
        }

        fn write(&self, text: &str) -> io::Result<()> {
            let units: Vec<u16> = text.encode_utf16().collect();
            let mut rest = units.as_slice();
            while !rest.is_empty() {
                let mut count = rest.len().min(CHUNK);
                if count < rest.len() && (0xD800..0xDC00).contains(&rest[count - 1]) {
                    count -= 1;
                }
                let mut written = 0;
                // SAFETY: `rest` holds at least `count` UTF-16 units, the
                // output handle is a console screen buffer and `written` is
                // writable.
                if unsafe {
                    WriteConsoleW(
                        self.output.0,
                        rest.as_ptr(),
                        count as u32,
                        &mut written,
                        ptr::null(),
                    )
                } == 0
                {
                    return Err(io::Error::last_os_error());
                }
                if written == 0 {
                    return Err(io::ErrorKind::WriteZero.into());
                }
                rest = &rest[(written as usize).min(rest.len())..];
            }
            Ok(())
        }
    }

    impl Drop for Console {
        fn drop(&mut self) {
            self.leave();
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn keys(feeds: &[&[u8]]) -> (Vec<u8>, bool) {
        let mut prefix = PrefixKeys::default();
        let mut sent = Vec::new();
        for feed in feeds {
            let keys = prefix.feed(feed);
            sent.extend(keys.bytes);
            if keys.detach {
                return (sent, true);
            }
        }
        (sent, false)
    }

    #[test]
    fn the_prefix_detaches_sends_itself_and_passes_other_keys_through() {
        // Ctrl+B d and Ctrl+B Ctrl+D detach; what came before is sent, what
        // follows in the same read is not.
        assert_eq!(keys(&[b"ab\x02dcd"]), (b"ab".to_vec(), true));
        assert_eq!(keys(&[b"\x02\x04"]), (Vec::new(), true));
        // Split across reads, the prefix waits for its key.
        let mut prefix = PrefixKeys::default();
        assert_eq!(
            prefix.feed(b"x\x02"),
            Keys {
                bytes: b"x".to_vec(),
                detach: false
            }
        );
        assert!(prefix.pending());
        assert_eq!(
            prefix.feed(b"d"),
            Keys {
                bytes: Vec::new(),
                detach: true
            }
        );
        // Ctrl+B Ctrl+B sends one Ctrl+B (send-prefix), and only one.
        assert_eq!(keys(&[b"\x02\x02"]), (b"\x02".to_vec(), false));
        assert_eq!(keys(&[b"\x02", b"\x02", b"d"]), (b"\x02d".to_vec(), false));
        assert_eq!(keys(&[b"\x02\x02\x02\x02"]), (b"\x02\x02".to_vec(), false));
        // Anything else passes with its prefix: D (tmux's choose-client), a
        // control key, an arrow key's sequence, Alt+d.
        assert_eq!(keys(&[b"\x02D"]), (b"\x02D".to_vec(), false));
        assert_eq!(keys(&[b"\x02c"]), (b"\x02c".to_vec(), false));
        assert_eq!(keys(&[b"\x02\x1b[A"]), (b"\x02\x1b[A".to_vec(), false));
        assert_eq!(keys(&[b"\x02\x1bd"]), (b"\x02\x1bd".to_vec(), false));
        assert_eq!(keys(&[b"\x02\x03"]), (b"\x02\x03".to_vec(), false));
        // Meta+Ctrl+B is ESC and Ctrl+B in one read; a lone Escape typed
        // before a Ctrl+B is a key of its own.
        assert_eq!(keys(&[b"\x1b\x02d"]), (b"\x1b\x02d".to_vec(), false));
        assert_eq!(keys(&[b"\x1b", b"\x02d"]), (b"\x1b".to_vec(), true));
        // Ctrl+C and other controls are plain input.
        assert_eq!(keys(&[b"\x03\x04d"]), (b"\x03\x04d".to_vec(), false));
    }

    #[test]
    fn pasted_text_is_never_a_binding() {
        let paste = b"\x1b[200~one\x02dtwo\x02\x02\x1b[201~";
        assert_eq!(keys(&[paste]), (paste.to_vec(), false));
        // Markers split across reads, and a binding right after the paste.
        let (sent, detach) = keys(&[b"\x1b[20", b"0~a\x02d\x1b", b"[201", b"~\x02d"]);
        assert_eq!(sent, b"\x1b[200~a\x02d\x1b[201~");
        assert!(detach);
        // Something that only looks like the start of a marker.
        assert_eq!(
            keys(&[b"\x1b[2\x1b[200~\x02d"]),
            (b"\x1b[2\x1b[200~\x02d".to_vec(), false)
        );
        assert_eq!(keys(&[b"\x1b[20x\x02d"]), (b"\x1b[20x".to_vec(), true));
    }

    #[test]
    fn utf8_split_across_reads_is_kept_until_complete() {
        let text = "a\u{e9}\u{4e2d}\u{1f600}z";
        let bytes = text.as_bytes();
        // Every split point, one byte at a time and in two halves.
        let mut decoder = Utf8Text::default();
        let one_by_one: String = bytes.iter().map(|byte| decoder.push(&[*byte])).collect();
        assert_eq!(one_by_one, text);
        assert_eq!(decoder.finish(), "");
        for split in 0..=bytes.len() {
            let mut decoder = Utf8Text::default();
            let (first, second) = bytes.split_at(split);
            let joined = decoder.push(first) + &decoder.push(second);
            assert_eq!(joined, text, "split at {split}");
        }
        // A held start shows nothing until its rest arrives.
        let mut decoder = Utf8Text::default();
        assert_eq!(decoder.push(b"x\xf0\x9f"), "x");
        assert_eq!(decoder.push(b"\x98\x80"), "\u{1f600}");
        // Invalid bytes become U+FFFD and decoding goes on.
        let mut decoder = Utf8Text::default();
        assert_eq!(decoder.push(b"a\xffb\xc3"), "a\u{fffd}b");
        assert_eq!(decoder.push(b"(c"), "\u{fffd}(c");
        assert_eq!(decoder.push(b"\xe4\xb8"), "");
        assert_eq!(decoder.finish(), "\u{fffd}");
        assert_eq!(decoder.push(b"\x80ok"), "\u{fffd}ok");
    }

    #[test]
    fn the_reset_leaves_the_alternate_screen_only_when_the_session_entered_it() {
        let mut modes = ScreenModes::default();
        modes.feed(b"\x1b[?2004h\x1b[?25l");
        assert!(!modes.alternate());
        let reset = modes.reset(30);
        assert!(!reset.contains("\x1b[?1049l"), "{reset:?}");
        assert!(reset.ends_with("\x1b[30;1H\r\n"), "{reset:?}");
        for off in [
            "\x1b[?2004l",
            "\x1b[?1000l",
            "\x1b[?1006l",
            "\x1b[?1004l",
            "\x1b[?25h",
            "\x1b[?1l\x1b>",
        ] {
            assert!(reset.contains(off), "{off:?} in {reset:?}");
        }
        // Entered with several modes at once, split across reads.
        modes.feed(b"\x1b[?1;10");
        modes.feed(b"49h");
        assert!(modes.alternate());
        assert!(modes.reset(24).starts_with("\x1b[?1049l"));
        modes.feed(b"\x1b[?1049l");
        assert!(!modes.alternate());
        modes.feed(b"\x1b[?47h");
        assert!(modes.alternate());
        modes.feed(b"\x1b[?1047l\x1b[?10490h");
        assert!(!modes.alternate());
    }
}

/// `hgs.exe a` inside a pseudoconsole of this test, as the GUI's embedded
/// terminal runs it, against an isolated host of the same build (spec D13).
/// The session's program and the client's console are this test binary run
/// as a child: `attach_recorder_child` records its raw console input, and
/// `attach_driver_child` runs `hgs.exe` on its console and reports the
/// console modes before, during and after. The build comes from
/// `HGS_TEST_BIN` (run `cargo build --locked` first); without it the test is
/// skipped, as a unit test cannot tell whether a `hgs.exe` it finds is current.
#[cfg(all(test, windows))]
mod console_tests {
    use super::super::{
        conpty::Api,
        console::ConPtyLauncher,
        protocol::Size,
        sequences::{Piece, Scanner},
        session::{Console as SessionConsole, LaunchPlan, Launched, Launcher},
        spawn::Environment,
        staging,
    };
    use super::lock;
    use crate::{platform::PinnedProcess, state::backend::host::HOST_DIED};
    use serde_json::{json, Value};
    use std::{
        env, fs,
        io::{self, Read, Write},
        os::windows::process::CommandExt,
        path::{Path, PathBuf},
        process::{Command, Stdio},
        sync::{
            atomic::{AtomicUsize, Ordering},
            Arc, Mutex,
        },
        thread,
        time::{Duration, Instant},
    };
    use windows_sys::Win32::System::{
        Console::{
            GetConsoleMode, GetStdHandle, ReadConsoleW, SetConsoleMode,
            DISABLE_NEWLINE_AUTO_RETURN, ENABLE_ECHO_INPUT, ENABLE_LINE_INPUT,
            ENABLE_PROCESSED_INPUT, ENABLE_VIRTUAL_TERMINAL_INPUT,
            ENABLE_VIRTUAL_TERMINAL_PROCESSING, ENABLE_WINDOW_INPUT, STD_HANDLE, STD_INPUT_HANDLE,
            STD_OUTPUT_HANDLE,
        },
        Threading::CREATE_NO_WINDOW,
    };

    const MODULE: &str = "host::attach::console_tests::";
    /// Where `attach_recorder_child` appends what it reads.
    const RECORD: &str = "HGS_TEST_ATTACH_RECORD";
    /// Where `attach_driver_child` reports, the hgs.exe it runs and its
    /// arguments as a JSON array.
    const REPORT: &str = "HGS_TEST_ATTACH_REPORT";
    const BIN: &str = "HGS_TEST_ATTACH_BIN";
    const ARGS: &str = "HGS_TEST_ATTACH_ARGS";
    const PROMPT: &str = "recorder>";
    const LIMIT: Duration = Duration::from_secs(60);
    static REPORTS: AtomicUsize = AtomicUsize::new(0);

    /// Poll `done` until it gives a value or `LIMIT` passes.
    fn until<T>(what: &str, mut done: impl FnMut() -> Option<T>) -> T {
        let deadline = Instant::now() + LIMIT;
        loop {
            if let Some(value) = done() {
                return value;
            }
            assert!(Instant::now() < deadline, "timed out waiting for {what}");
            thread::sleep(Duration::from_millis(50));
        }
    }

    /// This test binary running one ignored test of this module.
    fn child_argv(test: &str) -> Vec<String> {
        vec![
            env::current_exe().unwrap().display().to_string(),
            format!("{MODULE}{test}"),
            "--exact".into(),
            "--ignored".into(),
            "--test-threads=1".into(),
        ]
    }

    /// The mode of standard handle `which`; 0 when it is not a console.
    fn mode(which: STD_HANDLE) -> u32 {
        let mut mode = 0;
        // SAFETY: GetStdHandle reads this process's standard handle, which
        // GetConsoleMode only queries; `mode` is writable.
        unsafe {
            GetConsoleMode(GetStdHandle(which), &mut mode);
        }
        mode
    }

    /// Text a pseudoconsole drew, without terminal sequences, blanks or line
    /// breaks: ConPTY may draw blanks as cursor moves.
    fn squeeze(bytes: &[u8]) -> String {
        let mut text = Vec::new();
        Scanner::default().feed(bytes, &mut |piece| {
            if let Piece::Text(bytes) = piece {
                text.extend_from_slice(bytes);
            }
        });
        String::from_utf8_lossy(&text)
            .chars()
            .filter(|character| !character.is_whitespace() && !character.is_control())
            .collect()
    }

    fn record(path: &Path) -> String {
        fs::read(path)
            .map(|bytes| String::from_utf8_lossy(&bytes).into_owned())
            .unwrap_or_default()
    }

    /// The session's program: raw console input, recorded as it comes, until
    /// it ends with `quit`.
    #[test]
    #[ignore = "run by hgs_attaches_inside_a_pseudoconsole"]
    fn attach_recorder_child() {
        let Some(path) = env::var_os(RECORD) else {
            return;
        };
        // SAFETY: GetStdHandle reads this process's standard input handle.
        let input = unsafe { GetStdHandle(STD_INPUT_HANDLE) };
        let found = mode(STD_INPUT_HANDLE);
        let raw = (found & !(ENABLE_PROCESSED_INPUT | ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT))
            | ENABLE_VIRTUAL_TERMINAL_INPUT;
        // SAFETY: `input` is the session's console input buffer.
        assert!(
            unsafe { SetConsoleMode(input, raw) } != 0,
            "no console input"
        );
        let mut stdout = io::stdout().lock();
        stdout.write_all(PROMPT.as_bytes()).unwrap();
        stdout.flush().unwrap();
        let mut file = fs::OpenOptions::new()
            .create(true)
            .append(true)
            .open(path)
            .unwrap();
        let mut seen = String::new();
        let mut buffer = [0u16; 256];
        loop {
            let mut read = 0;
            // SAFETY: `buffer` holds 256 writable UTF-16 units and `read` is
            // writable; no control structure is passed.
            let ok = unsafe {
                ReadConsoleW(
                    input,
                    buffer.as_mut_ptr().cast(),
                    buffer.len() as u32,
                    &mut read,
                    std::ptr::null(),
                )
            };
            if ok == 0 {
                return;
            }
            let text = String::from_utf16_lossy(&buffer[..read as usize]);
            file.write_all(text.as_bytes()).unwrap();
            file.flush().unwrap();
            seen.push_str(&text);
            if seen.ends_with("quit") {
                std::process::exit(3);
            }
        }
    }

    /// The GUI's side: `hgs.exe` with the given arguments on this console,
    /// whose modes it reports before, while and after it runs.
    #[test]
    #[ignore = "run by hgs_attaches_inside_a_pseudoconsole"]
    fn attach_driver_child() {
        let Some(report) = env::var_os(REPORT) else {
            return;
        };
        let modes = || [mode(STD_INPUT_HANDLE), mode(STD_OUTPUT_HANDLE)];
        let args: Vec<String> = serde_json::from_str(&env::var(ARGS).unwrap()).unwrap();
        let before = modes();
        let mut child = Command::new(env::var_os(BIN).unwrap())
            .args(&args)
            .spawn()
            .unwrap();
        let mut seen: Vec<[u32; 2]> = Vec::new();
        let code = loop {
            if let Some(status) = child.try_wait().unwrap() {
                break status.code();
            }
            let now = modes();
            if now != before && !seen.contains(&now) {
                seen.push(now);
            }
            thread::sleep(Duration::from_millis(10));
        };
        let after = modes();
        let text = json!({"before": before, "seen": seen, "after": after, "code": code});
        fs::write(report, text.to_string()).unwrap();
    }

    /// An isolated state directory, profile and host of the build under test.
    struct Case {
        hgs: PathBuf,
        root: tempfile::TempDir,
        work: PathBuf,
        environment: Environment,
        host: Mutex<Option<PinnedProcess>>,
    }

    impl Case {
        fn new(hgs: PathBuf) -> Self {
            let root = tempfile::tempdir().unwrap();
            let [state, home, work] = ["state", "home", "work"].map(|name| {
                let path = root.path().join(name);
                fs::create_dir(&path).unwrap();
                path
            });
            // Local sessions are on in this profile, so that `hgs a` runs;
            // the host is the build under test (src/host/gate.rs).
            staging::enable(&home.join(".config").join("hgs").join(staging::MARKER)).unwrap();
            let mut environment = Environment::current().unwrap();
            for entry in environment.entries() {
                let name = entry.split('=').next().unwrap_or_default().to_owned();
                let upper = name.to_ascii_uppercase();
                if ["HGS_", "TMUX", "XDG_", "FAKE_AGENT_"]
                    .iter()
                    .any(|prefix| upper.starts_with(prefix))
                {
                    environment.remove(&name).unwrap();
                }
            }
            let shown = |path: &Path| path.display().to_string();
            for (name, value) in [
                ("HGS_STATE_DIR", shown(&state)),
                ("USERPROFILE", shown(&home)),
                ("HOME", shown(&home)),
                ("HGS_HOST_IMAGE", shown(&hgs)),
                ("HGS_HOST_ALLOW_UNSTAGED", "1".into()),
                ("HGS_TAB", "0".into()),
            ] {
                environment.set(name, &value).unwrap();
            }
            Self {
                hgs,
                root,
                work,
                environment,
                host: Mutex::new(None),
            }
        }

        /// The environment as variables for `Command`, without the `=C:`
        /// entries of the current directories.
        fn variables(&self) -> Vec<(String, String)> {
            self.environment
                .entries()
                .into_iter()
                .filter_map(|entry| {
                    let (name, value) = entry.split_once('=')?;
                    (!name.is_empty()).then(|| (name.to_owned(), value.to_owned()))
                })
                .collect()
        }

        /// `hgs.exe ARGS`: its exit code, output and error output.
        fn hgs(&self, args: &[&str]) -> (i32, String, String) {
            let output = Command::new(&self.hgs)
                .args(args)
                .env_clear()
                .envs(self.variables())
                .current_dir(&self.work)
                .stdin(Stdio::null())
                .creation_flags(CREATE_NO_WINDOW)
                .output()
                .unwrap();
            (
                output.status.code().unwrap_or(-1),
                String::from_utf8_lossy(&output.stdout).into_owned(),
                String::from_utf8_lossy(&output.stderr).into_owned(),
            )
        }

        fn call(&self, request: &Value, start: bool) -> (i32, Value) {
            let request = request.to_string();
            let mut args = vec!["__host", "call"];
            if start {
                args.push("--start");
            }
            args.push(&request);
            let (code, output, error) = self.hgs(&args);
            let answer = serde_json::from_str(&output).unwrap_or(json!({"stderr": error}));
            (code, answer)
        }

        fn ok(&self, request: Value) -> Value {
            let (code, answer) = self.call(&request, false);
            assert_eq!(code, 0, "{request}: {answer}");
            answer["ok"].clone()
        }

        /// A session of the recorder, recording into `record`; its pane.
        fn create(&self, name: &str, record: &Path) -> String {
            let mut environment = self.environment.clone();
            environment
                .set(RECORD, &record.display().to_string())
                .unwrap();
            let request = json!({
                "op": "create_session",
                "launch_id": format!("hgs-attach-test-{}-{name}", std::process::id()),
                "name": name,
                "cwd": self.work.display().to_string(),
                "env": environment.entries(),
                "argv": child_argv("attach_recorder_child"),
                "size": {"cols": 100, "rows": 30},
            });
            let (code, answer) = self.call(&request, true);
            assert_eq!(code, 0, "{answer}");
            if lock(&self.host).is_none() {
                let (code, status, error) = self.hgs(&["__host", "status"]);
                assert_eq!(code, 0, "{error}");
                let status: Value = serde_json::from_str(&status).unwrap();
                let host = &status["host"];
                let pinned = PinnedProcess::pin(
                    host["pid"].as_u64().unwrap() as u32,
                    host["created"].as_str().unwrap(),
                )
                .unwrap();
                *lock(&self.host) = Some(pinned);
            }
            let pane = answer["ok"]["pane"].as_str().unwrap().to_owned();
            until("the recorder's prompt", || {
                let screen = self.ok(json!({"op": "capture", "target": {"pane": pane}}));
                screen["text"]
                    .as_str()
                    .is_some_and(|text| text.contains(PROMPT))
                    .then_some(())
            });
            pane
        }

        fn session(&self, name: &str) -> Option<Value> {
            let listed = self.ok(json!({"op": "list"}));
            listed["sessions"]
                .as_array()
                .unwrap()
                .iter()
                .find(|session| session["name"] == name)
                .cloned()
        }

        /// `attach_driver_child` running `hgs.exe ARGS` on a pseudoconsole of `size`.
        fn client(&self, args: &[&str], size: Size) -> Client {
            let report = self.root.path().join(format!(
                "report-{}.json",
                REPORTS.fetch_add(1, Ordering::SeqCst)
            ));
            let mut environment = self.environment.clone();
            environment
                .set(REPORT, &report.display().to_string())
                .unwrap();
            environment
                .set(BIN, &self.hgs.display().to_string())
                .unwrap();
            environment.set(ARGS, &json!(args).to_string()).unwrap();
            let argv = child_argv("attach_driver_child");
            let work = self.work.display().to_string();
            let Launched {
                output: mut reader,
                input,
                console,
                ..
            } = ConPtyLauncher::new(Api::get().unwrap())
                .launch(&LaunchPlan {
                    argv: &argv,
                    cwd: &work,
                    environment: &environment,
                    size,
                })
                .unwrap();
            let input = Arc::new(Mutex::new(input));
            let output = Arc::new(Mutex::new(Vec::new()));
            {
                let (input, output) = (input.clone(), output.clone());
                thread::spawn(move || {
                    let mut buffer = [0; 4096];
                    let mut answered = 0;
                    while let Ok(count) = reader.read(&mut buffer) {
                        if count == 0 {
                            return;
                        }
                        let asked = {
                            let mut seen = lock(&output);
                            seen.extend_from_slice(&buffer[..count]);
                            seen.windows(3).filter(|window| window == b"\x1b[c").count()
                        };
                        // A ConPTY that asks who its terminal is (the bundled
                        // pair) gets an answer, as a terminal gives it.
                        while answered < asked {
                            let _ = lock(&input).write_all(b"\x1b[?61c");
                            answered += 1;
                        }
                    }
                });
            }
            Client {
                console,
                input,
                output,
                report,
            }
        }
    }

    impl Drop for Case {
        fn drop(&mut self) {
            let Some(host) = lock(&self.host).take() else {
                return;
            };
            if host.alive() {
                let listed = self.call(&json!({"op": "list"}), false).1;
                for pane in listed["ok"]["panes"].as_array().into_iter().flatten() {
                    self.call(
                        &json!({"op": "kill", "target": {"pane": pane["pane"]}}),
                        false,
                    );
                }
                let deadline = Instant::now() + LIMIT;
                while Instant::now() < deadline
                    && self.call(&json!({"op": "list"}), false).1["ok"]["sessions"]
                        .as_array()
                        .is_some_and(|sessions| !sessions.is_empty())
                {
                    thread::sleep(Duration::from_millis(200));
                }
                self.call(&json!({"op": "stop_host"}), false);
                let deadline = Instant::now() + LIMIT;
                while host.alive() && Instant::now() < deadline {
                    thread::sleep(Duration::from_millis(100));
                }
            }
            let _ = host.kill();
        }
    }

    /// The client's pseudoconsole: what the test types, what it shows.
    struct Client {
        console: Box<dyn SessionConsole>,
        input: Arc<Mutex<Box<dyn Write + Send>>>,
        output: Arc<Mutex<Vec<u8>>>,
        report: PathBuf,
    }

    impl Client {
        fn keys(&self, bytes: &[u8]) {
            let mut input = lock(&self.input);
            input.write_all(bytes).unwrap();
            input.flush().unwrap();
        }

        fn shows(&self, text: &str) -> bool {
            squeeze(&lock(&self.output)).contains(&squeeze(text.as_bytes()))
        }

        fn wait_shows(&self, text: &str) {
            until(&format!("{text:?} on the client"), || {
                self.shows(text).then_some(())
            });
        }

        /// The driver's report once it has ended (the client's exit code and
        /// the console modes) and what the console showed.
        fn finish(self) -> (Value, String) {
            let shown = || squeeze(&lock(&self.output));
            assert!(
                self.console.wait_exit(LIMIT),
                "the client did not end: {}",
                shown()
            );
            let report = until("the report", || {
                fs::read_to_string(&self.report)
                    .ok()
                    .and_then(|text| serde_json::from_str(&text).ok())
            });
            (report, shown())
        }
    }

    impl Drop for Client {
        fn drop(&mut self) {
            self.console.hang_up();
            self.console.end();
        }
    }

    fn mode_pair(value: &Value) -> [u32; 2] {
        [0, 1].map(|index| value[index].as_u64().unwrap() as u32)
    }

    /// The client's exit code; its console modes came back, and while it ran
    /// they were the attach modes (D13) when `attached`.
    fn ended(report: &Value, attached: bool) -> Option<i64> {
        assert_eq!(report["before"], report["after"], "{report}");
        if attached {
            let raw = report["seen"]
                .as_array()
                .unwrap()
                .iter()
                .map(mode_pair)
                .any(|[input, output]| {
                    input & (ENABLE_PROCESSED_INPUT | ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT) == 0
                        && input & ENABLE_VIRTUAL_TERMINAL_INPUT != 0
                        && input & ENABLE_WINDOW_INPUT != 0
                        && output & ENABLE_VIRTUAL_TERMINAL_PROCESSING != 0
                        && output & DISABLE_NEWLINE_AUTO_RETURN != 0
                });
            assert!(raw, "no attach modes seen: {report}");
        }
        report["code"].as_i64()
    }

    #[test]
    fn hgs_attaches_inside_a_pseudoconsole() {
        let Some(hgs) = env::var_os("HGS_TEST_BIN").filter(|value| !value.is_empty()) else {
            eprintln!("skipped: HGS_TEST_BIN names no hgs.exe (cargo build --locked first)");
            return;
        };
        let hgs = std::path::absolute(PathBuf::from(hgs)).unwrap();
        assert!(hgs.is_file(), "{} is missing", hgs.display());
        let case = Case::new(hgs);
        let records = [case.root.path().join("one"), case.root.path().join("two")];
        let pane = case.create("test/attach", &records[0]);
        case.create("test/second", &records[1]);
        let size = Size {
            cols: 100,
            rows: 30,
        };
        let squeezed = |text: &str| squeeze(text.as_bytes());

        // The repaint, keys, the literal prefix, a resize and Ctrl+B d.
        let client = case.client(&["a", "test/attach"], size);
        client.wait_shows(PROMPT);
        client.keys(b"hello");
        until("the keys", || {
            (record(&records[0]) == "hello").then_some(())
        });
        // Listed with its opaque client ID, next to the running host (D2, D13).
        let (code, listing, error) = case.hgs(&["ls", "--json", "--local"]);
        assert_eq!(code, 0, "{error}");
        let listing: Value = serde_json::from_str(&listing).unwrap();
        let row = listing["sessions"]
            .as_array()
            .unwrap()
            .iter()
            .find(|row| row["name"] == "test/attach")
            .unwrap()
            .clone();
        assert_eq!(row["attached"], 1, "{row}");
        let clients = row["clients"].as_array().unwrap();
        assert!(
            clients.len() == 1 && clients[0].as_str().unwrap().starts_with('c'),
            "{row}"
        );
        let host = &listing["local_host"];
        assert_eq!(
            (&host["state"], &host["protocol"]),
            (&json!("running"), &json!(1)),
            "{listing}"
        );
        assert!(["bundled", "inbox"].contains(&host["conpty"].as_str().unwrap()));
        client.keys(b"\x02\x02");
        client.keys(b"\x02x");
        until("the prefix keys", || {
            (record(&records[0]) == "hello\x02\x02x").then_some(())
        });
        let larger = Size {
            cols: 120,
            rows: 40,
        };
        client.console.resize(larger).unwrap();
        until("the pane's new size", || {
            let details = case.ok(json!({"op": "pane_info", "target": {"pane": pane}}));
            (details["width"] == 120 && details["height"] == 40).then_some(())
        });
        client.keys(b"\x02d");
        let (report, shown) = client.finish();
        assert_eq!(ended(&report, true), Some(0));
        assert!(
            shown.contains(&squeezed("[detached (from session test/attach)]")),
            "{shown}"
        );
        thread::sleep(Duration::from_millis(300));
        assert_eq!(record(&records[0]), "hello\x02\x02x");
        assert_eq!(case.session("test/attach").unwrap()["attached"], 0);

        // `--existing --run-id`: a changed run is refused, the current one
        // attaches, and Ctrl+B Ctrl+D detaches.
        case.ok(
            json!({"op": "set_environment", "target": {"name": "test/attach"},
                       "name": "@hgs_run", "value": "run-1"}),
        );
        let client = case.client(
            &["a", "test/attach", "--existing", "--run-id", "run-2"],
            size,
        );
        let (report, shown) = client.finish();
        assert_eq!(ended(&report, false), Some(1));
        assert!(
            shown.contains(&squeezed(crate::state::backend::RUN_CHANGED)),
            "{shown}"
        );
        let client = case.client(
            &["a", "test/attach", "--existing", "--run-id", "run-1"],
            size,
        );
        client.wait_shows(PROMPT);
        client.keys(b"\x02\x04");
        let (report, shown) = client.finish();
        assert_eq!(ended(&report, true), Some(0));
        assert!(shown.contains(&squeezed("[detached")), "{shown}");

        // The session's end ends the client.
        let client = case.client(&["a", "test/attach"], size);
        client.wait_shows(PROMPT);
        client.keys(b"quit");
        let (report, shown) = client.finish();
        assert_eq!(ended(&report, true), Some(0));
        assert!(shown.contains("[exited]"), "{shown}");
        until("the end", || {
            case.session("test/attach").is_none().then_some(())
        });

        // The host's death restores the console and is reported.
        let client = case.client(&["a", "test/second"], size);
        client.wait_shows(PROMPT);
        lock(&case.host).as_ref().unwrap().kill().unwrap();
        let (report, shown) = client.finish();
        assert_eq!(ended(&report, true), Some(1));
        assert!(shown.contains(&squeezed(HOST_DIED)), "{shown}");
    }
}
