//! Character widths and joining rules from tmux's utf8.c and utf8-combined.c.

use super::grid::Glyph;
use unicode_width::UnicodeWidthChar;

/// tmux's built-in width overrides (`utf8_default_width_cache`).
#[rustfmt::skip]
const WIDTH_OVERRIDES: &[(u32, u8)] = &[
    (0x261D, 2), (0x26F9, 2), (0x270A, 2), (0x270B, 2), (0x270C, 2), (0x270D, 2),
    (0x1F1E6, 1), (0x1F1E7, 1), (0x1F1E8, 1), (0x1F1E9, 1), (0x1F1EA, 1), (0x1F1EB, 1),
    (0x1F1EC, 1), (0x1F1ED, 1), (0x1F1EE, 1), (0x1F1EF, 1), (0x1F1F0, 1), (0x1F1F1, 1),
    (0x1F1F2, 1), (0x1F1F3, 1), (0x1F1F4, 1), (0x1F1F5, 1), (0x1F1F6, 1), (0x1F1F7, 1),
    (0x1F1F8, 1), (0x1F1F9, 1), (0x1F1FA, 1), (0x1F1FB, 1), (0x1F1FC, 1), (0x1F1FD, 1),
    (0x1F1FE, 1), (0x1F1FF, 1), (0x1F385, 2), (0x1F3C2, 2), (0x1F3C3, 2), (0x1F3C4, 2),
    (0x1F3C7, 2), (0x1F3CA, 2), (0x1F3CB, 2), (0x1F3CC, 2), (0x1F3FB, 2), (0x1F3FC, 2),
    (0x1F3FD, 2), (0x1F3FE, 2), (0x1F3FF, 2), (0x1F442, 2), (0x1F443, 2), (0x1F446, 2),
    (0x1F447, 2), (0x1F448, 2), (0x1F449, 2), (0x1F44A, 2), (0x1F44B, 2), (0x1F44C, 2),
    (0x1F44D, 2), (0x1F44E, 2), (0x1F44F, 2), (0x1F450, 2), (0x1F466, 2), (0x1F467, 2),
    (0x1F468, 2), (0x1F469, 2), (0x1F46B, 2), (0x1F46C, 2), (0x1F46D, 2), (0x1F46E, 2),
    (0x1F470, 2), (0x1F471, 2), (0x1F472, 2), (0x1F473, 2), (0x1F474, 2), (0x1F475, 2),
    (0x1F476, 2), (0x1F477, 2), (0x1F478, 2), (0x1F47C, 2), (0x1F481, 2), (0x1F482, 2),
    (0x1F483, 2), (0x1F485, 2), (0x1F486, 2), (0x1F487, 2), (0x1F48F, 2), (0x1F491, 2),
    (0x1F4AA, 2), (0x1F574, 2), (0x1F575, 2), (0x1F57A, 2), (0x1F590, 2), (0x1F595, 2),
    (0x1F596, 2), (0x1F645, 2), (0x1F646, 2), (0x1F647, 2), (0x1F64B, 2), (0x1F64C, 2),
    (0x1F64D, 2), (0x1F64E, 2), (0x1F64F, 2), (0x1F6A3, 2), (0x1F6B4, 2), (0x1F6B5, 2),
    (0x1F6B6, 2), (0x1F6C0, 2), (0x1F6CC, 2), (0x1F90C, 2), (0x1F90F, 2), (0x1F918, 2),
    (0x1F919, 2), (0x1F91A, 2), (0x1F91B, 2), (0x1F91C, 2), (0x1F91D, 2), (0x1F91E, 2),
    (0x1F91F, 2), (0x1F926, 2), (0x1F930, 2), (0x1F931, 2), (0x1F932, 2), (0x1F933, 2),
    (0x1F934, 2), (0x1F935, 2), (0x1F936, 2), (0x1F937, 2), (0x1F938, 2), (0x1F939, 2),
    (0x1F93D, 2), (0x1F93E, 2), (0x1F977, 2), (0x1F9B5, 2), (0x1F9B6, 2), (0x1F9B8, 2),
    (0x1F9B9, 2), (0x1F9BB, 2), (0x1F9CD, 2), (0x1F9CE, 2), (0x1F9CF, 2), (0x1F9D1, 2),
    (0x1F9D2, 2), (0x1F9D3, 2), (0x1F9D4, 2), (0x1F9D5, 2), (0x1F9D6, 2), (0x1F9D7, 2),
    (0x1F9D8, 2), (0x1F9D9, 2), (0x1F9DA, 2), (0x1F9DB, 2), (0x1F9DC, 2), (0x1F9DD, 2),
    (0x1FAC3, 2), (0x1FAC4, 2), (0x1FAC5, 2), (0x1FAF0, 2), (0x1FAF1, 2), (0x1FAF2, 2),
    (0x1FAF3, 2), (0x1FAF4, 2), (0x1FAF5, 2), (0x1FAF6, 2), (0x1FAF7, 2), (0x1FAF8, 2),
];

/// Where glibc's `wcwidth` (2.44, with tmux's rule for unprintable
/// characters) differs from `unicode-width`, so widths match tmux on Linux.
const GLIBC_WIDTHS: &[(u32, u32, u8)] = &[
    (0xAD, 0xAD, 1),
    (0x605, 0x605, 1),
    (0x70F, 0x70F, 1),
    (0x890, 0x891, 1),
    (0x8E2, 0x8E2, 1),
    (0x9BE, 0x9BE, 1),
    (0x9D7, 0x9D7, 1),
    (0xB3E, 0xB3E, 1),
    (0xB57, 0xB57, 1),
    (0xBBE, 0xBBE, 1),
    (0xBD7, 0xBD7, 1),
    (0xCC0, 0xCC0, 1),
    (0xCC2, 0xCC2, 1),
    (0xCC7, 0xCC8, 1),
    (0xCCA, 0xCCB, 1),
    (0xCD5, 0xCD6, 1),
    (0xD3E, 0xD3E, 1),
    (0xD4E, 0xD4E, 1),
    (0xD57, 0xD57, 1),
    (0xDCF, 0xDCF, 1),
    (0xDDF, 0xDDF, 1),
    (0x1715, 0x1715, 1),
    (0x1734, 0x1734, 1),
    (0x17A4, 0x17A4, 1),
    (0x17D8, 0x17D8, 1),
    (0x1B35, 0x1B35, 1),
    (0x1B3B, 0x1B3B, 1),
    (0x1B3D, 0x1B3D, 1),
    (0x1B43, 0x1B44, 1),
    (0x1BAA, 0x1BAA, 1),
    (0x1BF2, 0x1BF3, 1),
    (0x2065, 0x2065, 1),
    (0x2D7F, 0x2D7F, 0),
    (0x302E, 0x302F, 2),
    (0x3248, 0x324F, 2),
    (0xA8FA, 0xA8FA, 1),
    (0xA953, 0xA953, 1),
    (0xA9C0, 0xA9C0, 1),
    (0xFA6E, 0xFA6F, 1),
    (0xFADA, 0xFAFF, 1),
    (0xFF9E, 0xFF9F, 1),
    (0xFFF0, 0xFFF8, 1),
    (0x111C0, 0x111C0, 1),
    (0x111C2, 0x111C3, 1),
    (0x11235, 0x11235, 1),
    (0x1133E, 0x1133E, 1),
    (0x1134D, 0x1134D, 1),
    (0x11357, 0x11357, 1),
    (0x113B8, 0x113B8, 1),
    (0x113C2, 0x113C2, 1),
    (0x113C5, 0x113C5, 1),
    (0x113C7, 0x113C9, 1),
    (0x113CF, 0x113CF, 1),
    (0x113D1, 0x113D1, 1),
    (0x114B0, 0x114B0, 1),
    (0x114BD, 0x114BD, 1),
    (0x115AF, 0x115AF, 1),
    (0x116B6, 0x116B6, 1),
    (0x11930, 0x11930, 1),
    (0x1193D, 0x1193D, 1),
    (0x1193F, 0x1193F, 1),
    (0x11941, 0x11941, 1),
    (0x11A84, 0x11A89, 1),
    (0x11D46, 0x11D46, 1),
    (0x11F02, 0x11F02, 1),
    (0x11F41, 0x11F41, 1),
    (0x16FF0, 0x16FF1, 2),
    (0x1D165, 0x1D166, 1),
    (0x1D16D, 0x1D172, 1),
    (0x2A6E0, 0x2A6FF, 1),
    (0x2B81E, 0x2B81F, 1),
    (0x2CEAE, 0x2CEAF, 1),
    (0x2EBE1, 0x2EBEF, 1),
    (0x2EE5E, 0x2F7FF, 1),
    (0x2FA1E, 0x2FFFD, 1),
    (0x3134B, 0x3134F, 1),
    (0x3347A, 0x3FFFD, 1),
    (0xE0000, 0xE0000, 1),
    (0xE0002, 0xE001F, 1),
    (0xE0080, 0xE00FF, 1),
    (0xE01F0, 0xE0FFF, 1),
];

/// The first character of UTF-8 data as the C library decodes it
/// (`utf8_towc`): overlong forms and surrogates are invalid, but four-byte
/// sequences up to U+13FFFF are accepted.
pub(super) fn first_char(data: &[u8]) -> Option<u32> {
    let lead = *data.first()?;
    let (len, min, init) = match lead {
        0x00..=0x7f => return Some(u32::from(lead)),
        0xc2..=0xdf => (2, 0x80, u32::from(lead & 0x1f)),
        0xe0..=0xef => (3, 0x800, u32::from(lead & 0x0f)),
        0xf0..=0xf4 => (4, 0x10000, u32::from(lead & 0x07)),
        _ => return None,
    };
    let tail = data.get(1..len)?;
    let mut value = init;
    for byte in tail {
        if byte & 0xc0 != 0x80 {
            return None;
        }
        value = (value << 6) | u32::from(byte & 0x3f);
    }
    if value < min || (0xd800..=0xdfff).contains(&value) {
        return None;
    }
    Some(value)
}

/// The column width tmux gives a character (`utf8_width`): its override
/// table, then the C library's `wcwidth` (here `unicode-width` corrected to
/// glibc), with unprintable characters one column wide and C1 controls none.
pub(super) fn char_width(code: u32) -> u8 {
    if let Ok(index) = WIDTH_OVERRIDES.binary_search_by_key(&code, |(wc, _)| *wc) {
        return WIDTH_OVERRIDES[index].1;
    }
    if let Ok(index) = GLIBC_WIDTHS.binary_search_by(|(start, end, _)| {
        if *end < code {
            std::cmp::Ordering::Less
        } else if *start > code {
            std::cmp::Ordering::Greater
        } else {
            std::cmp::Ordering::Equal
        }
    }) {
        return GLIBC_WIDTHS[index].2;
    }
    match char::from_u32(code).and_then(|c| c.width()) {
        Some(width) => width.min(2) as u8,
        None if (0x80..=0x9f).contains(&code) => 0,
        None => 1,
    }
}

pub(super) fn has_zwj(ud: &Glyph) -> bool {
    ud.as_bytes().ends_with("\u{200d}".as_bytes())
}

pub(super) fn is_zwj(ud: &Glyph) -> bool {
    ud.as_bytes() == "\u{200d}".as_bytes()
}

pub(super) fn is_vs(ud: &Glyph) -> bool {
    ud.as_bytes() == "\u{fe0f}".as_bytes()
}

pub(super) fn is_hangul_filler(ud: &Glyph) -> bool {
    ud.as_bytes() == "\u{3164}".as_bytes()
}

fn regional_count(ud: &Glyph) -> usize {
    ud.as_bytes()
        .windows(4)
        .filter(|w| w[0] == 0xf0 && w[1] == 0x9f && w[2] == 0x87 && (0xa6..=0xbf).contains(&w[3]))
        .count()
}

/// Emoji that take a skin tone modifier (`utf8_should_combine`).
#[rustfmt::skip]
const MODIFIER_BASES: &[u32] = &[
    0x1F44B, 0x1F44C, 0x1F44D, 0x1F44E, 0x1F44F, 0x1F450, 0x1F466, 0x1F467, 0x1F468, 0x1F469,
    0x1F46E, 0x1F470, 0x1F471, 0x1F472, 0x1F473, 0x1F474, 0x1F475, 0x1F476, 0x1F477, 0x1F478,
    0x1F47C, 0x1F481, 0x1F482, 0x1F483, 0x1F485, 0x1F486, 0x1F487, 0x1F4AA, 0x1F575, 0x1F57A,
    0x1F590, 0x1F595, 0x1F596, 0x1F645, 0x1F646, 0x1F647, 0x1F64B, 0x1F64C, 0x1F64D, 0x1F64E,
    0x1F64F, 0x1F6B4, 0x1F6B5, 0x1F6B6, 0x1F926, 0x1F937, 0x1F938, 0x1F939, 0x1F93D, 0x1F93E,
    0x1F9B5, 0x1F9B6, 0x1F9B8, 0x1F9B9, 0x1F9CD, 0x1F9CE, 0x1F9CF, 0x1F9D1, 0x1F9D2, 0x1F9D3,
    0x1F9D4, 0x1F9D5, 0x1F9D6, 0x1F9D7, 0x1F9D8, 0x1F9D9, 0x1F9DA, 0x1F9DB, 0x1F9DC, 0x1F9DD,
    0x1F9DE, 0x1F9DF,
];

/// Whether `add` joins `with` as a regional indicator pair or a skin tone.
pub(super) fn should_combine(with: &Glyph, add: &Glyph) -> bool {
    let (Some(w), Some(a)) = (first_char(with.as_bytes()), first_char(add.as_bytes())) else {
        return false;
    };
    let regional = 0x1F1E6..=0x1F1FF;
    if regional.contains(&a) && regional.contains(&w) {
        return regional_count(with) == 1 && regional_count(add) == 1;
    }
    MODIFIER_BASES.contains(&a) && (0x1F3FB..=0x1F3FF).contains(&w)
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum JamoClass {
    None,
    Choseong,
    Jungseong,
    Jongseong,
}

fn jamo_class(s: &[u8]) -> JamoClass {
    let (b0, b1, b2) = (s[0], s[1], s[2]);
    match (b0, b1) {
        (0xe1, 0x84) => JamoClass::Choseong,
        (0xe1, 0x85) => match b2 {
            0x80..=0x9f => JamoClass::Choseong,
            0xa0..=0xbf => JamoClass::Jungseong,
            _ => JamoClass::None,
        },
        (0xe1, 0x86) => match b2 {
            0x80..=0xa7 => JamoClass::Jungseong,
            0xa8..=0xbf => JamoClass::Jongseong,
            _ => JamoClass::None,
        },
        (0xe1, 0x87) => JamoClass::Jongseong,
        (0xea, 0xa5) if (0xa0..=0xbc).contains(&b2) => JamoClass::Choseong,
        (0xed, 0x9e) if (0xb0..=0xbf).contains(&b2) => JamoClass::Jungseong,
        (0xed, 0x9f) => match b2 {
            0x80..=0x86 => JamoClass::Jungseong,
            0x8b..=0xbb => JamoClass::Jongseong,
            _ => JamoClass::None,
        },
        _ => JamoClass::None,
    }
}

pub(super) enum JamoState {
    NotJamo,
    Choseong,
    Composable,
    NotComposable,
}

/// `hanguljamo_check_state`.
pub(super) fn hangul_jamo_state(previous: &Glyph, ud: &Glyph) -> JamoState {
    if ud.size != 3 {
        return JamoState::NotJamo;
    }
    let tail = |want: JamoClass| {
        let bytes = previous.as_bytes();
        if bytes.len() < 3 {
            return JamoState::NotComposable;
        }
        if jamo_class(&bytes[bytes.len() - 3..]) == want {
            JamoState::Composable
        } else {
            JamoState::NotComposable
        }
    };
    match jamo_class(ud.as_bytes()) {
        JamoClass::Choseong => JamoState::Choseong,
        JamoClass::Jungseong => tail(JamoClass::Choseong),
        JamoClass::Jongseong => tail(JamoClass::Jungseong),
        JamoClass::None => JamoState::NotJamo,
    }
}

/// Whether writing `next` right after the cell holding `previous` would
/// join it to that cell (or drop it) instead of drawing a cell of its own,
/// as `screen_write_combine` decides.
pub(super) fn would_join(previous: &[u8], next: &[u8]) -> bool {
    let Some(code) = first_char(next) else {
        return false;
    };
    let size = match next[0] {
        0x00..=0x7f => 1,
        0xc2..=0xdf => 2,
        0xe0..=0xef => 3,
        _ => 4,
    };
    let mut ud = Glyph::ascii(0);
    ud.bytes[..size].copy_from_slice(&next[..size]);
    ud.size = size as u8;
    ud.width = char_width(code);
    let mut last = Glyph::ascii(0);
    let take = previous.len().min(super::grid::UTF8_SIZE);
    last.bytes[..take].copy_from_slice(&previous[..take]);
    last.size = take as u8;
    if is_hangul_filler(&ud) || is_zwj(&ud) || is_vs(&ud) || ud.width == 0 {
        return ud.size >= 2;
    }
    if ud.size < 2 {
        return false;
    }
    match hangul_jamo_state(&last, &ud) {
        JamoState::NotComposable | JamoState::Composable => true,
        JamoState::Choseong => false,
        JamoState::NotJamo => {
            should_combine(&last, &ud) || should_combine(&ud, &last) || has_zwj(&last)
        }
    }
}

/// tmux's incremental UTF-8 decoder (`utf8_open` and `utf8_append`).
#[derive(Clone, Copy, Debug)]
pub(super) struct Decoder {
    data: [u8; 4],
    have: usize,
    size: usize,
    invalid: bool,
}

pub(super) enum Decoded {
    More,
    Done(Glyph),
    Error,
}

impl Decoder {
    pub(super) fn open(byte: u8) -> Option<Self> {
        let size = match byte {
            0xc2..=0xdf => 2,
            0xe0..=0xef => 3,
            0xf0..=0xf4 => 4,
            _ => return None,
        };
        Some(Self {
            data: [byte, 0, 0, 0],
            have: 1,
            size,
            invalid: false,
        })
    }

    pub(super) fn append(&mut self, byte: u8) -> Decoded {
        if byte & 0xc0 != 0x80 {
            self.invalid = true;
        }
        self.data[self.have] = byte;
        self.have += 1;
        if self.have != self.size {
            return Decoded::More;
        }
        if self.invalid {
            return Decoded::Error;
        }
        let Some(code) = first_char(&self.data[..self.size]) else {
            return Decoded::Error;
        };
        let mut glyph = Glyph::ascii(0);
        glyph.bytes[..self.size].copy_from_slice(&self.data[..self.size]);
        glyph.size = self.size as u8;
        glyph.width = char_width(code);
        Decoded::Done(glyph)
    }
}

/// `utf8_stravis(..., VIS_OCTAL|VIS_CSTYLE)`: how tmux stores hyperlink
/// URIs and ids. Complete UTF-8 characters stay, a backslash doubles, other
/// bytes that are not printable become octal escapes.
pub(super) fn stravis(data: &[u8]) -> Vec<u8> {
    let mut out = Vec::with_capacity(data.len());
    let mut at = 0;
    while at < data.len() {
        let byte = data[at];
        if let Some(mut decoder) = Decoder::open(byte) {
            let mut end = at + 1;
            let mut done = false;
            while end < data.len() {
                match decoder.append(data[end]) {
                    Decoded::More => end += 1,
                    Decoded::Done(_) => {
                        done = true;
                        end += 1;
                        break;
                    }
                    Decoded::Error => break,
                }
            }
            if done {
                out.extend_from_slice(&data[at..end]);
                at = end;
                continue;
            }
        }
        match byte {
            b'\\' => out.extend_from_slice(b"\\\\"),
            b' ' | b'\t' | b'\n' | 0x21..=0x7e => out.push(byte),
            _ => out.extend(format!("\\{byte:03o}").into_bytes()),
        }
        at += 1;
    }
    out
}

/// U+FFFD as tmux writes it for invalid input.
pub(super) fn replacement() -> Glyph {
    let mut glyph = Glyph::ascii(0);
    glyph.bytes[..3].copy_from_slice("\u{fffd}".as_bytes());
    glyph.size = 3;
    glyph.width = 1;
    glyph
}
