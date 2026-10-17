//! API v1 identity uses Python json.dumps(sort_keys=True, separators=(',', ':'),
//! ensure_ascii=True). Changing whitespace, escaping, key order or float
//! spelling reopens duplicate requests. The codec is bounded before building an
//! object tree, and sorts keys itself: serde_json's `preserve_order` feature,
//! which any crate in a build can enable, makes `Map` keep insertion order.
use crate::error::{Error, Result};
use serde_json::Value;
use sha2::{Digest, Sha256};
use std::io::Write;
pub fn digest(s: impl AsRef<[u8]>) -> String {
    format!("{:x}", Sha256::digest(s.as_ref()))
}

/// Python's `json` refuses integers of more than 4,300 digits
/// (`sys.int_info.default_max_str_digits`); this codec keeps them.
pub const MAX_INT_DIGITS: usize = 4300;

/// Whether every integer in `value` has at most [`MAX_INT_DIGITS`] digits, so
/// that Python's `json`, and the connector that applies its bound, reads it.
pub fn python_integers(value: &Value) -> bool {
    let mut pending = vec![value];
    while let Some(item) = pending.pop() {
        match item {
            Value::Number(number) => {
                let literal = number.to_string();
                if !literal.contains(['.', 'e', 'E'])
                    && literal.trim_start_matches('-').len() > MAX_INT_DIGITS
                {
                    return false;
                }
            }
            Value::Array(items) => pending.extend(items),
            Value::Object(members) => pending.extend(members.values()),
            _ => {}
        }
    }
    true
}

/// Parses untrusted JSON after a lexical scan that bounds it without building
/// any tree. `depth_limit` bounds container nesting (`[]` is depth 1), and
/// `value_limit` bounds the containers, strings (object keys included) and
/// scalars, so `{"a":[1,true]}` counts 5. Whitespace never counts. Exceeding
/// either limit fails with [`Error::LARGE`], malformed input with [`Error::BAD`].
///
/// Relay configuration admits requests within `max_json_depth` and
/// `max_json_values`, at most 32 and 50,000, and the relay re-reads stored JSON
/// at those maxima. A claim batch, `{"requests":[request]}` with the request's
/// `gateway_route` and `expires_at`, nests a request two levels deeper and adds
/// fewer than 64 values, so parse a whole batch with at least those margins.
pub fn parse(raw: &[u8], depth_limit: usize, value_limit: usize) -> Result<Value> {
    let mut depth = 0usize;
    let mut values = 0usize;
    let mut quoted = false;
    let mut escaped = false;
    let mut scalar = false;
    for &c in raw {
        if quoted {
            if escaped {
                escaped = false;
            } else if c == b'\\' {
                escaped = true;
            } else if c == b'"' {
                quoted = false;
            }
            continue;
        }
        if scalar {
            if c.is_ascii_whitespace() || b",:]}".contains(&c) {
                scalar = false;
            } else {
                continue;
            }
        }
        match c {
            b'"' => {
                quoted = true;
                values += 1;
            }
            b'{' | b'[' => {
                depth += 1;
                values += 1;
                if depth > depth_limit {
                    return Err(Error::LARGE);
                }
            }
            b'}' | b']' => {
                depth = depth.checked_sub(1).ok_or(Error::BAD)?;
            }
            b',' | b':' | b' ' | b'\t' | b'\r' | b'\n' => {}
            _ => {
                scalar = true;
                values += 1;
            }
        }
        if values > value_limit {
            return Err(Error::LARGE);
        }
    }
    if quoted || depth != 0 {
        return Err(Error::BAD);
    }
    // Deserialize containers explicitly: arbitrary_precision's generic Value
    // visitor otherwise interprets a user key named "$serde_json::private::Number"
    // as its internal numeric representation. Borrow raw children so this never
    // copies an attachment once per nesting level.
    fn value(raw: &serde_json::value::RawValue) -> Result<Value> {
        let text = raw.get();
        match text.as_bytes().first() {
            Some(b'{') => {
                let children: std::collections::BTreeMap<String, &serde_json::value::RawValue> =
                    serde_json::from_str(text)?;
                let mut object = serde_json::Map::new();
                for (key, child) in children {
                    object.insert(key, value(child)?);
                }
                Ok(Value::Object(object))
            }
            Some(b'[') => {
                let children: Vec<&serde_json::value::RawValue> = serde_json::from_str(text)?;
                children
                    .into_iter()
                    .map(value)
                    .collect::<Result<Vec<_>>>()
                    .map(Value::Array)
            }
            _ => Ok(serde_json::from_str(text)?),
        }
    }
    value(serde_json::from_slice(raw)?)
}
pub fn canonical(value: &Value) -> Result<String> {
    // Exact allocation avoids doubling a 28 MiB buffer for the final bracket.
    let length = size(value, 32 * 1024 * 1024)?;
    let mut out = Vec::with_capacity(length);
    encode(value, &mut out)?;
    String::from_utf8(out).map_err(|_| Error::BAD)
}
/// Separators and escaping of one Python `json.dumps` form.
#[derive(Clone, Copy)]
struct Style {
    item: &'static [u8],
    key: &'static [u8],
    ascii: bool,
}
const CANONICAL: Style = Style {
    item: b",",
    key: b":",
    ascii: true,
};
/// Writes the API v1 identity form described in the module documentation.
pub fn encode(v: &Value, w: &mut impl Write) -> Result<()> {
    write(v, w, CANONICAL)
}
/// Python's plain `json.dumps(value, ensure_ascii=...)`, separated by `", "` and
/// `": "`, which the connector uses to measure native input and results. Keys
/// are sorted where Python keeps dict order, so only the length is Python's. A
/// number beyond the double range fails, where Python would write `Infinity`.
pub fn python_default_encode(v: &Value, ensure_ascii: bool, w: &mut impl Write) -> Result<()> {
    let style = Style {
        item: b", ",
        key: b": ",
        ascii: ensure_ascii,
    };
    write(v, w, style)
}
/// `len(json.dumps(value, ensure_ascii=...).encode())`, in UTF-8 bytes, which
/// equal Python's character count only when `ensure_ascii` is set.
pub fn python_default_len(v: &Value, ensure_ascii: bool) -> Result<usize> {
    let mut w = LimitWriter {
        inner: std::io::sink(),
        remaining: usize::MAX,
    };
    python_default_encode(v, ensure_ascii, &mut w)?;
    Ok(usize::MAX - w.remaining)
}
fn write(v: &Value, w: &mut impl Write, style: Style) -> Result<()> {
    match v {
        Value::Null => w.write_all(b"null")?,
        Value::Bool(b) => w.write_all(if *b { b"true" } else { b"false" })?,
        Value::Number(n) => w.write_all(number(&n.to_string())?.as_bytes())?,
        Value::String(s) => string(s, w, style.ascii)?,
        Value::Array(a) => {
            w.write_all(b"[")?;
            for (i, x) in a.iter().enumerate() {
                if i > 0 {
                    w.write_all(style.item)?;
                }
                write(x, w, style)?;
            }
            w.write_all(b"]")?;
        }
        Value::Object(o) => {
            // Python orders keys by code point, which is the byte order of UTF-8.
            let mut entries: Vec<_> = o.iter().collect();
            entries.sort_unstable_by(|a, b| a.0.cmp(b.0));
            w.write_all(b"{")?;
            for (i, (k, x)) in entries.into_iter().enumerate() {
                if i > 0 {
                    w.write_all(style.item)?;
                }
                string(k, w, style.ascii)?;
                w.write_all(style.key)?;
                write(x, w, style)?;
            }
            w.write_all(b"}")?;
        }
    }
    Ok(())
}
fn string(s: &str, w: &mut impl Write, ascii: bool) -> Result<()> {
    w.write_all(b"\"")?;
    // Copy long ASCII/base64 runs in one write; do not allocate escaped copies.
    // Without ASCII escaping, Python escapes only the quote, the backslash and
    // C0 controls: DEL and all non-ASCII characters are written as they are.
    let mut start = 0;
    for (i, c) in s.char_indices() {
        if c >= ' ' && (c <= '~' || !ascii) && c != '"' && c != '\\' {
            continue;
        }
        w.write_all(&s.as_bytes()[start..i])?;
        match c {
            '"' => w.write_all(b"\\\"")?,
            '\\' => w.write_all(b"\\\\")?,
            '\n' => w.write_all(b"\\n")?,
            '\r' => w.write_all(b"\\r")?,
            '\t' => w.write_all(b"\\t")?,
            '\u{8}' => w.write_all(b"\\b")?,
            '\u{c}' => w.write_all(b"\\f")?,
            c => {
                let mut units = [0u16; 2];
                for x in c.encode_utf16(&mut units) {
                    write!(w, "\\u{x:04x}")?;
                }
            }
        }
        start = i + c.len_utf8();
    }
    w.write_all(&s.as_bytes()[start..])?;
    w.write_all(b"\"")?;
    Ok(())
}
fn number(raw: &str) -> Result<String> {
    if !raw.contains(['.', 'e', 'E']) {
        return Ok(if raw == "-0" { "0".into() } else { raw.into() });
    }
    let f: f64 = raw.parse().map_err(|_| Error::BAD)?;
    if !f.is_finite() {
        return Err(Error::BAD);
    }
    let s = ryu::Buffer::new().format_finite(f).to_owned();
    let (negative, s) = s
        .strip_prefix('-')
        .map_or((false, s.as_str()), |s| (true, s));
    let (mantissa, exp) = match s.split_once('e') {
        Some((m, e)) => (m, e.parse::<i32>().map_err(|_| Error::BAD)?),
        None => (s, 0),
    };
    let decimal = mantissa.find('.').unwrap_or(mantissa.len()) as i32;
    let digits = mantissa.replace('.', "");
    let first = digits.find(|c| c != '0').unwrap_or(digits.len() - 1);
    let digits = digits[first..].trim_end_matches('0');
    if digits.is_empty() {
        return Ok(if negative { "-0.0" } else { "0.0" }.into());
    }
    let power = exp + decimal - first as i32 - 1;
    let mut out = if negative { "-".into() } else { String::new() };
    if !(-4..16).contains(&power) {
        out.push_str(&digits[..1]);
        if digits.len() > 1 {
            out.push('.');
            out.push_str(&digits[1..]);
        }
        out.push('e');
        out.push(if power < 0 { '-' } else { '+' });
        out.push_str(&format!("{:02}", power.abs()));
    } else {
        let position = power + 1;
        if position <= 0 {
            out.push_str("0.");
            out.extend(std::iter::repeat_n('0', (-position) as usize));
            out.push_str(digits);
        } else if position as usize >= digits.len() {
            out.push_str(digits);
            out.extend(std::iter::repeat_n('0', position as usize - digits.len()));
            out.push_str(".0");
        } else {
            out.push_str(&digits[..position as usize]);
            out.push('.');
            out.push_str(&digits[position as usize..]);
        }
    }
    Ok(out)
}
pub struct LimitWriter<W> {
    pub inner: W,
    pub remaining: usize,
}
impl<W: Write> Write for LimitWriter<W> {
    fn write(&mut self, b: &[u8]) -> std::io::Result<usize> {
        if b.len() > self.remaining {
            return Err(std::io::Error::other("JSON exceeds response limit"));
        }
        let n = self.inner.write(b)?;
        self.remaining -= n;
        Ok(n)
    }
    fn flush(&mut self) -> std::io::Result<()> {
        self.inner.flush()
    }
}
pub fn size(value: &Value, maximum: usize) -> Result<usize> {
    let mut w = LimitWriter {
        inner: std::io::sink(),
        remaining: maximum,
    };
    encode(value, &mut w).map_err(|_| Error::LARGE)?;
    Ok(maximum - w.remaining)
}
