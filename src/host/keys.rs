//! The bytes a pane receives for `send_keys`, exactly as tmux's `send-keys`
//! writes them (tmux 3.7 and 3.8 with `extended-keys` off, its default; hgs's
//! panes never turn it on).
//!
//! A named key is looked up as `key_string_lookup_string` (key-string.c) looks
//! it up: `C-`, `M-` and `S-` prefixes in any case and order, a leading `^` for
//! Ctrl, `0x` code points, single characters and tmux's key table, compared
//! without regard to case. It is then written as `input_key` (input-keys.c)
//! writes it: cursor keys follow the pane's application cursor mode (DECCKM)
//! and keypad keys its keypad mode (DECKPAM), keys with modifiers use the
//! xterm `CSI 1;N` forms, and Ctrl and Meta fold into C0 bytes and an ESC
//! prefix. A name tmux does not know, or a key it cannot write (`C-BSpace`,
//! `C-Escape`), is sent as literal text, as `send-keys` does. Mouse key names
//! are not keys here; tmux refuses them without a mouse event and sends the
//! name as text too. `Any` and the user keys (`User0` to `User1000`) are
//! internal key codes and write nothing. Literal text is written as its UTF-8
//! bytes.
//!
//! The expected bytes in the tests were recorded from tmux 3.8 with a raw
//! input recorder in a private server; 1317 names (every key and modifier
//! combination above, in each mode) matched it byte for byte when this was
//! written.
//!
//! One mode is ConPTY's, not tmux's: when the console has asked for
//! win32-input-mode (DECSET 9001) and its program reads key records rather
//! than VT input, a key that tmux writes as a lone C0 byte for Ctrl and a
//! letter is written as key records instead (`win32_ctrl_letter`, D6).

/// The pane modes that change what a key sends.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub(crate) struct Modes {
    /// DECCKM (`CSI ? 1 h`): cursor keys send `ESC O A` instead of `ESC [ A`.
    pub cursor: bool,
    /// DECKPAM (`ESC =`): keypad keys send `ESC O p` and so on.
    pub keypad: bool,
    /// Ctrl and a letter go as win32-input-mode key records: the console
    /// asked for win32-input-mode (`CSI ? 9001 h`) and its program reads key
    /// records (`channel::key_records`).
    pub key_records: bool,
}

/// The bytes `send-keys` writes for the key named `name`.
pub(crate) fn named(name: &str, modes: Modes) -> Vec<u8> {
    match lookup(name) {
        Lookup::Key(key) => match encode(key, modes) {
            Ok(bytes) => match (modes.key_records, bytes.as_slice()) {
                (true, &[byte]) => win32_ctrl_letter(byte).unwrap_or(bytes),
                _ => bytes,
            },
            // tmux has written Meta's ESC before it finds that it cannot
            // write the key; the name follows it as text.
            Err(mut written) => {
                written.extend(literal(name));
                written
            }
        },
        Lookup::Any => Vec::new(),
        Lookup::Unknown => literal(name),
    }
}

/// Whether key records change what the key named `name` sends: tmux writes
/// it as one C0 byte for Ctrl and a letter.
pub(crate) fn ctrl_letter(name: &str, modes: Modes) -> bool {
    let plain = Modes {
        key_records: false,
        ..modes
    };
    named(name, plain)
        != named(
            name,
            Modes {
                key_records: true,
                ..modes
            },
        )
}

/// Set-1 scan codes of the letters A to Z, at their places on a US keyboard.
const LETTER_SCAN_CODES: [u8; 26] = [
    0x1e, 0x30, 0x2e, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17, 0x24, 0x25, 0x26, 0x32, 0x31, 0x18, 0x19,
    0x10, 0x13, 0x1f, 0x14, 0x16, 0x2f, 0x11, 0x2d, 0x15, 0x2c,
];
/// `LEFT_CTRL_PRESSED` in a key record's control key state.
const LEFT_CTRL_PRESSED: u16 = 0x0008;

/// The win32-input-mode records (`CSI Vk ; Sc ; Uc ; Kd ; Cs ; Rc _`) of Ctrl
/// and the letter that the C0 byte `byte` stands for, key down and then key
/// up: the letter's virtual key and scan code, the lower-case Latin letter as
/// its character and `LEFT_CTRL_PRESSED`. None for any other byte, and for
/// backspace, tab, line feed and carriage return (`C-h`, `C-i`, `C-j`,
/// `C-m`), which ConPTY maps to their own keys.
///
/// ConPTY turns a C0 byte into a key record whose character is the control
/// character. A program that reads key records rather than VT input (Codex,
/// through crossterm) then looks the character up in the keyboard layout of
/// the foreground window, so with a Russian layout Ctrl+C arrives as Ctrl and
/// a Cyrillic letter, which Codex ignores. A record with the Latin letter is
/// Ctrl+C in every layout, and ConPTY gives a program that reads VT input
/// (Claude Code, Kimi) the C0 byte again.
pub(crate) fn win32_ctrl_letter(byte: u8) -> Option<Vec<u8>> {
    if !(0x01..=0x1a).contains(&byte) || matches!(byte, 0x08 | 0x09 | 0x0a | 0x0d) {
        return None;
    }
    let virtual_key = b'@' + byte;
    let scan_code = LETTER_SCAN_CODES[usize::from(byte - 1)];
    let character = virtual_key.to_ascii_lowercase();
    let records = [1, 0]
        .map(|down| {
            format!("\x1b[{virtual_key};{scan_code};{character};{down};{LEFT_CTRL_PRESSED};1_")
        })
        .concat();
    Some(records.into_bytes())
}

/// The bytes `send-keys -l` writes for `text`: its UTF-8 bytes.
pub(crate) fn literal(text: &str) -> Vec<u8> {
    text.as_bytes().to_vec()
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum Keypad {
    Slash,
    Star,
    Minus,
    Seven,
    Eight,
    Nine,
    Plus,
    Four,
    Five,
    Six,
    One,
    Two,
    Three,
    Enter,
    Zero,
    Period,
}

impl Keypad {
    /// The byte in normal mode and the final byte of `ESC O x` in keypad mode.
    fn bytes(self) -> (u8, u8) {
        match self {
            Self::Slash => (b'/', b'o'),
            Self::Star => (b'*', b'j'),
            Self::Minus => (b'-', b'm'),
            Self::Seven => (b'7', b'w'),
            Self::Eight => (b'8', b'x'),
            Self::Nine => (b'9', b'y'),
            Self::Plus => (b'+', b'k'),
            Self::Four => (b'4', b't'),
            Self::Five => (b'5', b'u'),
            Self::Six => (b'6', b'v'),
            Self::One => (b'1', b'q'),
            Self::Two => (b'2', b'r'),
            Self::Three => (b'3', b's'),
            Self::Enter => (b'\n', b'M'),
            Self::Zero => (b'0', b'p'),
            Self::Period => (b'.', b'n'),
        }
    }
}

/// A key without modifiers (tmux's `KEYC_MASK_KEY` part).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum Base {
    /// A character, C0 controls included.
    Char(char),
    /// F1 to F12.
    Function(u8),
    Insert,
    Delete,
    Home,
    End,
    PageUp,
    PageDown,
    BackTab,
    Backspace,
    Up,
    Down,
    Left,
    Right,
    Keypad(Keypad),
}

/// A key code: a base key, its modifiers and tmux's flags.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
struct Key {
    base: Base,
    ctrl: bool,
    meta: bool,
    shift: bool,
    /// KEYC_IMPLIED_META: the table entries with Meta already say so, so no
    /// ESC goes before them.
    implied_meta: bool,
    /// KEYC_CURSOR: a cursor key, written by DECCKM.
    cursor: bool,
    /// KEYC_KEYPAD: a keypad key, written by DECKPAM.
    keypad: bool,
}

impl Key {
    fn plain(base: Base) -> Self {
        Self {
            base,
            ctrl: false,
            meta: false,
            shift: false,
            implied_meta: false,
            cursor: false,
            keypad: false,
        }
    }

    fn has_modifiers(self) -> bool {
        self.ctrl || self.meta || self.shift || self.implied_meta
    }
}

enum Lookup {
    Key(Key),
    /// `Any`, an internal key code that writes nothing.
    Any,
    /// KEYC_NONE and KEYC_UNKNOWN: the name goes as text.
    Unknown,
}

/// tmux's key table: names compared without regard to case, and whether a name
/// carries KEYC_IMPLIED_META, KEYC_CURSOR or KEYC_KEYPAD.
fn table(name: &str) -> Option<Key> {
    const NAMED: &[(&str, Base)] = &[
        ("IC", Base::Insert),
        ("Insert", Base::Insert),
        ("DC", Base::Delete),
        ("Delete", Base::Delete),
        ("Home", Base::Home),
        ("End", Base::End),
        ("NPage", Base::PageDown),
        ("PageDown", Base::PageDown),
        ("PgDn", Base::PageDown),
        ("PPage", Base::PageUp),
        ("PageUp", Base::PageUp),
        ("PgUp", Base::PageUp),
        ("Up", Base::Up),
        ("Down", Base::Down),
        ("Left", Base::Left),
        ("Right", Base::Right),
        ("BTab", Base::BackTab),
        ("Space", Base::Char(' ')),
        ("BSpace", Base::Backspace),
        ("Enter", Base::Char('\r')),
        ("Escape", Base::Char('\x1b')),
        ("Tab", Base::Char('\t')),
        ("KP/", Base::Keypad(Keypad::Slash)),
        ("KP*", Base::Keypad(Keypad::Star)),
        ("KP-", Base::Keypad(Keypad::Minus)),
        ("KP7", Base::Keypad(Keypad::Seven)),
        ("KP8", Base::Keypad(Keypad::Eight)),
        ("KP9", Base::Keypad(Keypad::Nine)),
        ("KP+", Base::Keypad(Keypad::Plus)),
        ("KP4", Base::Keypad(Keypad::Four)),
        ("KP5", Base::Keypad(Keypad::Five)),
        ("KP6", Base::Keypad(Keypad::Six)),
        ("KP1", Base::Keypad(Keypad::One)),
        ("KP2", Base::Keypad(Keypad::Two)),
        ("KP3", Base::Keypad(Keypad::Three)),
        ("KPEnter", Base::Keypad(Keypad::Enter)),
        ("KP0", Base::Keypad(Keypad::Zero)),
        ("KP.", Base::Keypad(Keypad::Period)),
    ];
    let base = (1..=12u8)
        .find(|n| name.eq_ignore_ascii_case(&format!("F{n}")))
        .map(Base::Function)
        .or_else(|| {
            NAMED
                .iter()
                .find(|(known, _)| name.eq_ignore_ascii_case(known))
                .map(|(_, base)| *base)
        })?;
    let mut key = Key::plain(base);
    match base {
        Base::Function(_)
        | Base::Insert
        | Base::Delete
        | Base::Home
        | Base::End
        | Base::PageUp
        | Base::PageDown => key.implied_meta = true,
        Base::Up | Base::Down | Base::Left | Base::Right => {
            key.implied_meta = true;
            key.cursor = true;
        }
        Base::Keypad(_) => key.keypad = true,
        Base::Char(_) | Base::BackTab | Base::Backspace => {}
    }
    Some(key)
}

/// `key_string_lookup_string`.
fn lookup(name: &str) -> Lookup {
    if name.eq_ignore_ascii_case("None") {
        return Lookup::Unknown;
    }
    if name.eq_ignore_ascii_case("Any") {
        return Lookup::Any;
    }
    // A code point in hexadecimal, read as sscanf's %x reads it: the hex
    // digits at the start; modifiers are not looked for.
    if let Some(digits) = name.strip_prefix("0x") {
        let end = digits
            .find(|c: char| !c.is_ascii_hexdigit())
            .unwrap_or(digits.len());
        let Ok(code) = u32::from_str_radix(&digits[..end], 16) else {
            return Lookup::Unknown;
        };
        return match char::from_u32(code) {
            Some(c) => Lookup::Key(Key::plain(Base::Char(c))),
            None => Lookup::Unknown,
        };
    }
    let mut rest = name;
    let mut key = Key::plain(Base::Char('\0'));
    // `^x` is Ctrl and the lower-case x; `^` before more text is Ctrl.
    let bytes = rest.as_bytes();
    if bytes.first() == Some(&b'^') && bytes.len() > 1 {
        if bytes.len() == 2 {
            key.base = Base::Char(char::from(bytes[1].to_ascii_lowercase()));
            key.ctrl = true;
            return Lookup::Key(key);
        }
        key.ctrl = true;
        rest = &rest[1..];
    }
    loop {
        let bytes = rest.as_bytes();
        if bytes.len() < 2 || bytes[1] != b'-' {
            break;
        }
        match bytes[0].to_ascii_uppercase() {
            b'C' => key.ctrl = true,
            b'M' => key.meta = true,
            b'S' => key.shift = true,
            _ => return Lookup::Unknown,
        }
        rest = &rest[2..];
    }
    let mut characters = rest.chars();
    let Some(first) = characters.next() else {
        return Lookup::Unknown;
    };
    let single = characters.next().is_none();
    if first.is_ascii() && single {
        if (first as u32) < 32 {
            return Lookup::Unknown;
        }
        key.base = Base::Char(first);
        return Lookup::Key(key);
    }
    if !first.is_ascii() {
        // One UTF-8 character and nothing after it.
        if !single {
            return Lookup::Unknown;
        }
        key.base = Base::Char(first);
        return Lookup::Key(key);
    }
    let Some(found) = table(rest) else {
        return if user_key(rest) {
            Lookup::Any
        } else {
            Lookup::Unknown
        };
    };
    key.base = found.base;
    key.cursor = found.cursor;
    key.keypad = found.keypad;
    key.implied_meta = found.implied_meta && key.meta;
    Lookup::Key(key)
}

/// Whether `name` is one of tmux's user keys, `User0` to `User1000`, which the
/// table search reads with `sscanf(name, "User%u")`: white space and a sign
/// may come first, text after the digits is ignored, and the number wraps
/// around as `strtoul` and an `unsigned int` make it (`User4294967296` is
/// `User0`). They are internal key codes that write nothing, with any
/// modifiers.
fn user_key(name: &str) -> bool {
    let Some(rest) = name.strip_prefix("User") else {
        return false;
    };
    let rest = rest.trim_start_matches([' ', '\t', '\n', '\x0b', '\x0c', '\r']);
    let (negative, digits) = match rest.as_bytes().first() {
        Some(b'+') => (false, &rest[1..]),
        Some(b'-') => (true, &rest[1..]),
        _ => (false, rest),
    };
    let end = digits
        .find(|c: char| !c.is_ascii_digit())
        .unwrap_or(digits.len());
    if end == 0 {
        return false;
    }
    // strtoul: a magnitude past its range is ULONG_MAX, sign or not.
    let value = match digits[..end].parse::<u64>() {
        Ok(magnitude) if negative => magnitude.wrapping_neg(),
        Ok(magnitude) => magnitude,
        Err(_) => u64::MAX,
    };
    // %u stores an unsigned int.
    value as u32 <= 1000
}

/// The xterm modifier parameter (2 to 8) of a key with modifiers.
fn modifier_parameter(key: Key) -> Option<u8> {
    // Entries with Meta carry KEYC_IMPLIED_META; without it, Meta does not
    // match them.
    if key.meta != key.implied_meta {
        return None;
    }
    let value = 1 + u8::from(key.shift) + 2 * u8::from(key.meta) + 4 * u8::from(key.ctrl);
    (value > 1).then_some(value)
}

/// tmux's table of VT10x sequences (`input_key_defaults`): an exact match on
/// the key, its modifiers and its cursor and keypad flags.
fn sequence(key: Key) -> Option<Vec<u8>> {
    let unmodified = !key.has_modifiers();
    let csi = |text: String| {
        let mut bytes = b"\x1b[".to_vec();
        bytes.extend(text.into_bytes());
        bytes
    };
    let ss3 = |last: u8| vec![0x1b, b'O', last];
    match key.base {
        Base::Keypad(pad) if unmodified && !key.cursor => {
            let (plain, application) = pad.bytes();
            Some(if key.keypad {
                ss3(application)
            } else {
                vec![plain]
            })
        }
        Base::Up | Base::Down | Base::Left | Base::Right if !key.keypad => {
            let last = match key.base {
                Base::Up => b'A',
                Base::Down => b'B',
                Base::Right => b'C',
                _ => b'D',
            };
            if unmodified {
                Some(if key.cursor {
                    ss3(last)
                } else {
                    csi((last as char).to_string())
                })
            } else if key.cursor {
                None
            } else {
                modifier_parameter(key).map(|m| csi(format!("1;{m}{}", last as char)))
            }
        }
        _ if key.cursor || key.keypad => None,
        Base::BackTab if unmodified => Some(csi("Z".into())),
        Base::Function(n) => {
            let (number, last) = match n {
                1 => ("1", 'P'),
                2 => ("1", 'Q'),
                3 => ("1", 'R'),
                4 => ("1", 'S'),
                5 => ("15", '~'),
                6 => ("17", '~'),
                7 => ("18", '~'),
                8 => ("19", '~'),
                9 => ("20", '~'),
                10 => ("21", '~'),
                11 => ("23", '~'),
                _ => ("24", '~'),
            };
            if unmodified {
                Some(if last == '~' {
                    csi(format!("{number}~"))
                } else {
                    ss3(last as u8)
                })
            } else {
                modifier_parameter(key).map(|m| csi(format!("{number};{m}{last}")))
            }
        }
        Base::Insert | Base::Delete | Base::Home | Base::End | Base::PageUp | Base::PageDown => {
            let (number, last) = match key.base {
                Base::Insert => ("2", '~'),
                Base::Delete => ("3", '~'),
                Base::Home => ("1", 'H'),
                Base::End => ("4", 'F'),
                Base::PageUp => ("5", '~'),
                _ => ("6", '~'),
            };
            if unmodified {
                // Home and End are `ESC [ 1 ~` and `ESC [ 4 ~` without modifiers.
                Some(csi(format!("{number}~")))
            } else {
                let number = match key.base {
                    Base::Home | Base::End => "1",
                    _ => number,
                };
                modifier_parameter(key).map(|m| csi(format!("{number};{m}{last}")))
            }
        }
        _ => None,
    }
}

/// `input_key` for a pane in `modes`. When tmux cannot write the key,
/// `send-keys` sends its name as text after what was already written.
fn encode(mut key: Key, modes: Modes) -> Result<Vec<u8>, Vec<u8>> {
    if key.base == Base::Backspace {
        // tmux's `backspace` option is C-?.
        key.base = Base::Char('\x7f');
    }
    if key.base == Base::BackTab {
        key.ctrl = false;
        key.meta = false;
        key.shift = false;
        key.implied_meta = false;
    }
    if let Base::Char(c) = key.base {
        if !key.has_modifiers() && !key.cursor && !key.keypad {
            if matches!(c, '\t' | '\r' | '\x1b' | ' '..='\x7f') {
                return Ok(vec![c as u8]);
            }
            if !c.is_ascii() {
                return Ok(c.to_string().into_bytes());
            }
        }
    }
    if !modes.keypad {
        key.keypad = false;
    }
    if !modes.cursor {
        key.cursor = false;
    }
    let found = sequence(key)
        .map(|bytes| (bytes, key.meta && !key.implied_meta))
        .or_else(|| {
            (key.meta && !key.implied_meta)
                .then(|| sequence(Key { meta: false, ..key }))
                .flatten()
                .map(|bytes| (bytes, true))
        })
        .or_else(|| {
            key.cursor
                .then(|| {
                    sequence(Key {
                        cursor: false,
                        ..key
                    })
                })
                .flatten()
                .map(|bytes| (bytes, key.meta && !key.implied_meta))
        })
        .or_else(|| {
            key.keypad
                .then(|| {
                    sequence(Key {
                        keypad: false,
                        ..key
                    })
                })
                .flatten()
                .map(|bytes| (bytes, key.meta && !key.implied_meta))
        });
    if let Some((bytes, escape)) = found {
        let mut out = Vec::with_capacity(bytes.len() + 1);
        if escape {
            out.push(0x1b);
        }
        out.extend(bytes);
        return Ok(out);
    }
    let Base::Char(c) = key.base else {
        // A function, editing or keypad key without a sequence: tmux ignores
        // internal key codes and writes nothing.
        return Ok(Vec::new());
    };
    vt10x(c, key)
}

/// `input_key_vt10x`: a character with modifiers in standard mode. A Ctrl
/// combination without a C0 byte fails after Meta's ESC was written.
fn vt10x(c: char, key: Key) -> Result<Vec<u8>, Vec<u8>> {
    // Which characters Ctrl turns into which bytes, besides `@` to `~`.
    const FROM: &[u8] = b"1!9(0)=+;:'\",<.>/-8? 2";
    const TO: &[u8] = b"119900=+;;'',,..\x1f\x1f\x7f\x7f\0\0";
    let mut out = Vec::new();
    if key.meta {
        out.push(0x1b);
    }
    // Standard mode cannot report modifiers of other characters.
    if !c.is_ascii() {
        out.extend(c.to_string().into_bytes());
        return Ok(out);
    }
    let byte = c as u8;
    let ctrl = key.ctrl && !matches!(byte, b'\r' | b'\n' | b'\t');
    let byte = if ctrl {
        if let Some(at) = FROM.iter().position(|&from| from == byte) {
            TO[at]
        } else if (b'3'..=b'7').contains(&byte) {
            byte - 0x18
        } else if (b'@'..=b'~').contains(&byte) {
            byte & 0x1f
        } else {
            return Err(out);
        }
    } else {
        byte
    };
    out.push(byte & 0x7f);
    Ok(out)
}

#[cfg(test)]
mod tests {
    use super::*;

    const NORMAL: Modes = Modes {
        cursor: false,
        keypad: false,
        key_records: false,
    };
    const CURSOR: Modes = Modes {
        cursor: true,
        keypad: false,
        key_records: false,
    };
    const KEYPAD: Modes = Modes {
        cursor: false,
        keypad: true,
        key_records: false,
    };

    fn hex(bytes: &[u8]) -> String {
        bytes.iter().fold(String::new(), |mut text, byte| {
            text.push_str(&format!("{byte:02x}"));
            text
        })
    }

    /// Recorded from tmux 3.8 `send-keys` into a raw input recorder: the key
    /// name, then the bytes in normal mode, with DECCKM and with DECKPAM.
    const RECORDED: &[(&str, &str, &str, &str)] = &[
        ("Enter", "0d", "0d", "0d"),
        ("Escape", "1b", "1b", "1b"),
        ("Tab", "09", "09", "09"),
        ("BTab", "1b5b5a", "1b5b5a", "1b5b5a"),
        ("BSpace", "7f", "7f", "7f"),
        ("Space", "20", "20", "20"),
        ("Up", "1b5b41", "1b4f41", "1b5b41"),
        ("Down", "1b5b42", "1b4f42", "1b5b42"),
        ("Left", "1b5b44", "1b4f44", "1b5b44"),
        ("Right", "1b5b43", "1b4f43", "1b5b43"),
        ("Home", "1b5b317e", "1b5b317e", "1b5b317e"),
        ("End", "1b5b347e", "1b5b347e", "1b5b347e"),
        ("PPage", "1b5b357e", "1b5b357e", "1b5b357e"),
        ("NPage", "1b5b367e", "1b5b367e", "1b5b367e"),
        ("PageUp", "1b5b357e", "1b5b357e", "1b5b357e"),
        ("PageDown", "1b5b367e", "1b5b367e", "1b5b367e"),
        ("PgUp", "1b5b357e", "1b5b357e", "1b5b357e"),
        ("PgDn", "1b5b367e", "1b5b367e", "1b5b367e"),
        ("DC", "1b5b337e", "1b5b337e", "1b5b337e"),
        ("Delete", "1b5b337e", "1b5b337e", "1b5b337e"),
        ("IC", "1b5b327e", "1b5b327e", "1b5b327e"),
        ("Insert", "1b5b327e", "1b5b327e", "1b5b327e"),
        ("F1", "1b4f50", "1b4f50", "1b4f50"),
        ("F4", "1b4f53", "1b4f53", "1b4f53"),
        ("F5", "1b5b31357e", "1b5b31357e", "1b5b31357e"),
        ("F10", "1b5b32317e", "1b5b32317e", "1b5b32317e"),
        ("F11", "1b5b32337e", "1b5b32337e", "1b5b32337e"),
        ("F12", "1b5b32347e", "1b5b32347e", "1b5b32347e"),
        ("C-a", "01", "01", "01"),
        ("C-c", "03", "03", "03"),
        ("C-d", "04", "04", "04"),
        ("C-e", "05", "05", "05"),
        ("C-l", "0c", "0c", "0c"),
        ("C-s", "13", "13", "13"),
        ("C-u", "15", "15", "15"),
        ("C-w", "17", "17", "17"),
        ("C-x", "18", "18", "18"),
        ("C-z", "1a", "1a", "1a"),
        ("M-s", "1b73", "1b73", "1b73"),
        ("M-Enter", "1b0d", "1b0d", "1b0d"),
        ("C-Up", "1b5b313b3541", "1b5b313b3541", "1b5b313b3541"),
        ("S-Up", "1b5b313b3241", "1b5b313b3241", "1b5b313b3241"),
        ("M-Up", "1b5b313b3341", "1b5b313b3341", "1b5b313b3341"),
        ("C-M-a", "1b01", "1b01", "1b01"),
        ("S-Tab", "09", "09", "09"),
        ("C-Space", "00", "00", "00"),
        ("C-@", "00", "00", "00"),
        ("C-[", "1b", "1b", "1b"),
        ("C-]", "1d", "1d", "1d"),
        ("C-^", "1e", "1e", "1e"),
        ("C-_", "1f", "1f", "1f"),
        ("C-?", "7f", "7f", "7f"),
        ("C-1", "31", "31", "31"),
        ("C-i", "09", "09", "09"),
        ("C-m", "0d", "0d", "0d"),
        ("C-h", "08", "08", "08"),
        ("1", "31", "31", "31"),
        ("hello", "68656c6c6f", "68656c6c6f", "68656c6c6f"),
        ("KP0", "30", "30", "1b4f70"),
        ("KP9", "39", "39", "1b4f79"),
        ("KPEnter", "0a", "0a", "1b4f4d"),
        ("KP*", "2a", "2a", "1b4f6a"),
        ("KP+", "2b", "2b", "1b4f6b"),
        ("KP-", "2d", "2d", "1b4f6d"),
        ("KP.", "2e", "2e", "1b4f6e"),
        ("KP/", "2f", "2f", "1b4f6f"),
        ("C-Home", "1b5b313b3548", "1b5b313b3548", "1b5b313b3548"),
        ("S-F1", "1b5b313b3250", "1b5b313b3250", "1b5b313b3250"),
        ("C-F5", "1b5b31353b357e", "1b5b31353b357e", "1b5b31353b357e"),
        ("M-BSpace", "1b7f", "1b7f", "1b7f"),
        ("M-Escape", "1b1b", "1b1b", "1b1b"),
        (
            "C-BSpace",
            "432d425370616365",
            "432d425370616365",
            "432d425370616365",
        ),
        ("S-Left", "1b5b313b3244", "1b5b313b3244", "1b5b313b3244"),
        ("C-S-Up", "1b5b313b3641", "1b5b313b3641", "1b5b313b3641"),
        ("M-Tab", "1b09", "1b09", "1b09"),
        ("C-Tab", "09", "09", "09"),
        ("S-Enter", "0d", "0d", "0d"),
        ("C-Enter", "0d", "0d", "0d"),
        ("é", "c3a9", "c3a9", "c3a9"),
        ("M-é", "1bc3a9", "1bc3a9", "1bc3a9"),
        ("C-é", "c3a9", "c3a9", "c3a9"),
        // Meta's ESC is written before Ctrl fails; the name follows as text.
        (
            "C-M-Escape",
            "1b432d4d2d457363617065",
            "1b432d4d2d457363617065",
            "1b432d4d2d457363617065",
        ),
        ("^a", "01", "01", "01"),
        ("^C", "03", "03", "03"),
        // User keys are internal and write nothing; others are text.
        ("User0", "", "", ""),
        ("M-User1000", "", "", ""),
        ("User4294967296", "", "", ""),
        (
            "User1001",
            "5573657231303031",
            "5573657231303031",
            "5573657231303031",
        ),
        ("User-1", "557365722d31", "557365722d31", "557365722d31"),
    ];

    #[test]
    fn named_keys_send_what_tmux_sends() {
        for (name, normal, cursor, keypad) in RECORDED {
            for (modes, expected) in [(NORMAL, normal), (CURSOR, cursor), (KEYPAD, keypad)] {
                assert_eq!(hex(&named(name, modes)), *expected, "{name} in {modes:?}");
            }
        }
    }

    #[test]
    fn every_key_hgs_sends_is_known() {
        // The names `state` sends (Terminal's KEYS list, question, effort,
        // interrupt and input queue navigation) must be keys, never text.
        for name in [
            "Enter", "Escape", "Tab", "BTab", "BSpace", "Up", "Down", "Left", "Right", "Home",
            "End", "PPage", "NPage", "DC", "C-c", "C-d", "C-l", "C-a", "C-e", "C-u", "C-w", "C-x",
            "C-s", "M-s",
        ] {
            assert!(matches!(lookup(name), Lookup::Key(_)), "{name}");
            assert_ne!(named(name, NORMAL), name.as_bytes(), "{name}");
        }
    }

    #[test]
    fn literal_text_is_its_utf8_bytes() {
        assert_eq!(literal("/model ä\n\x1b"), "/model ä\n\x1b".as_bytes());
        assert_eq!(literal(""), b"");
    }

    /// The set-1 scan codes of a US keyboard, letter by letter.
    const US_SCAN_CODES: &[(char, u8)] = &[
        ('A', 0x1e),
        ('B', 0x30),
        ('C', 0x2e),
        ('D', 0x20),
        ('E', 0x12),
        ('F', 0x21),
        ('G', 0x22),
        ('H', 0x23),
        ('I', 0x17),
        ('J', 0x24),
        ('K', 0x25),
        ('L', 0x26),
        ('M', 0x32),
        ('N', 0x31),
        ('O', 0x18),
        ('P', 0x19),
        ('Q', 0x10),
        ('R', 0x13),
        ('S', 0x1f),
        ('T', 0x14),
        ('U', 0x16),
        ('V', 0x2f),
        ('W', 0x11),
        ('X', 0x2d),
        ('Y', 0x15),
        ('Z', 0x2c),
    ];

    #[test]
    fn ctrl_letters_have_win32_key_records() {
        // As probed with Codex on a Russian layout (zerus-c8r9).
        assert_eq!(
            win32_ctrl_letter(0x03).unwrap(),
            b"\x1b[67;46;99;1;8;1_\x1b[67;46;99;0;8;1_"
        );
        assert_eq!(
            win32_ctrl_letter(0x15).unwrap(),
            b"\x1b[85;22;117;1;8;1_\x1b[85;22;117;0;8;1_"
        );
        assert_eq!(US_SCAN_CODES.len(), 26);
        for (letter, scan_code) in US_SCAN_CODES {
            let byte = *letter as u8 - b'@';
            let records = win32_ctrl_letter(byte);
            if matches!(letter, 'H' | 'I' | 'J' | 'M') {
                assert_eq!(records, None, "{letter}");
                continue;
            }
            let virtual_key = *letter as u32;
            let character = letter.to_ascii_lowercase() as u32;
            let expected = format!(
                "\x1b[{virtual_key};{scan_code};{character};1;8;1_\
                 \x1b[{virtual_key};{scan_code};{character};0;8;1_"
            );
            assert_eq!(
                String::from_utf8(records.unwrap()).unwrap(),
                expected,
                "{letter}"
            );
        }
        for byte in (0x00..=0xff).filter(|byte| !(0x01..=0x1a).contains(byte)) {
            assert_eq!(win32_ctrl_letter(byte), None, "{byte:#04x}");
        }
    }

    #[test]
    fn keys_for_a_program_that_reads_key_records_send_ctrl_letters_as_records() {
        for (base, win32) in [
            (
                NORMAL,
                Modes {
                    key_records: true,
                    ..NORMAL
                },
            ),
            (
                CURSOR,
                Modes {
                    key_records: true,
                    ..CURSOR
                },
            ),
            (
                KEYPAD,
                Modes {
                    key_records: true,
                    ..KEYPAD
                },
            ),
        ] {
            // Every recorded key is the same, but a lone Ctrl+letter byte.
            for (name, ..) in RECORDED {
                let plain = named(name, base);
                let expected = match plain.as_slice() {
                    &[byte] => win32_ctrl_letter(byte).unwrap_or(plain.clone()),
                    _ => plain.clone(),
                };
                assert_eq!(named(name, win32), expected, "{name} in {win32:?}");
                assert_eq!(ctrl_letter(name, base), expected != plain, "{name}");
            }
            for name in ["C-c", "c-C", "^c", "^C", "C-S-c", "0x3"] {
                assert_eq!(
                    named(name, win32),
                    b"\x1b[67;46;99;1;8;1_\x1b[67;46;99;0;8;1_",
                    "{name}"
                );
                assert!(ctrl_letter(name, base), "{name}");
            }
            assert_eq!(
                named("C-u", win32),
                b"\x1b[85;22;117;1;8;1_\x1b[85;22;117;0;8;1_"
            );
            assert_eq!(named("C-z", win32), win32_ctrl_letter(0x1a).unwrap());
            // Keys ConPTY maps itself, Ctrl with other keys, Meta and text.
            for (name, bytes) in [
                ("C-h", &b"\x08"[..]),
                ("C-i", b"\t"),
                ("Tab", b"\t"),
                ("C-j", b"\n"),
                ("C-m", b"\r"),
                ("Enter", b"\r"),
                ("Escape", b"\x1b"),
                ("C-[", b"\x1b"),
                ("C-Space", b"\0"),
                ("C-]", b"\x1d"),
                ("C-_", b"\x1f"),
                ("C-?", b"\x7f"),
                ("BSpace", b"\x7f"),
                ("C-M-a", b"\x1b\x01"),
                ("M-c", b"\x1bc"),
                ("c", b"c"),
                ("C-Up", b"\x1b[1;5A"),
                ("\x03", b"\x03"),
            ] {
                assert_eq!(named(name, win32), bytes, "{name}");
                assert!(!ctrl_letter(name, base), "{name}");
            }
        }
        assert_eq!(literal("\x03"), b"\x03");
    }
}
