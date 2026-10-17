//! Python semantics over parsed JSON (`serde_json::Value` with exact number
//! spelling), for the checks the Python connector writes with `==`, `in`,
//! `isinstance`, `len()` and iteration. Native output and phone payloads are
//! untrusted, so a value of an unexpected type must be judged exactly as
//! Python judges it: `True == 1`, iterating a dict yields its keys, `len()`
//! counts code points, and a list cannot be a dict key.
use serde_json::{Map, Number, Value};
use std::cmp::Ordering;

/// What `str()` of a Python exception carries into a receipt: a ValueError
/// with its text, or a TypeError (an unhashable key, a non-iterable value),
/// whose Python text never reaches a receipt.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum PyError {
    Value(String),
    Type,
}

impl PyError {
    pub fn value(message: impl Into<String>) -> Self {
        PyError::Value(message.into())
    }
}

pub type PyResult<T> = Result<T, PyError>;

/// A JSON number as Python's `json.loads` reads it: an `int` when written
/// without a fraction or exponent, otherwise a `float`.
#[derive(Debug, Clone, PartialEq)]
pub enum PyNumber {
    /// Decimal digits with an optional leading `-`, normalized (`-0` is `0`).
    Int(String),
    Float(f64),
}

impl PyNumber {
    pub fn of(number: &Number) -> PyNumber {
        let text = number.to_string();
        if text.contains(['.', 'e', 'E']) {
            // Python's float() reads an overflowing literal as infinity.
            PyNumber::Float(text.parse().unwrap_or(f64::INFINITY))
        } else {
            PyNumber::Int(normal_int(&text))
        }
    }
    pub fn is_finite(&self) -> bool {
        match self {
            PyNumber::Int(_) => true,
            PyNumber::Float(value) => value.is_finite(),
        }
    }
    /// The value as a float, as `float(x)` would give it, or `None` for an
    /// integer beyond the double range (Python's OverflowError).
    pub fn as_f64(&self) -> Option<f64> {
        match self {
            PyNumber::Int(digits) => {
                let value: f64 = digits.parse().ok()?;
                value.is_finite().then_some(value)
            }
            PyNumber::Float(value) => Some(*value),
        }
    }
}

fn normal_int(text: &str) -> String {
    let (negative, digits) = match text.strip_prefix('-') {
        Some(digits) => (true, digits),
        None => (false, text),
    };
    let digits = digits.trim_start_matches('0');
    if digits.is_empty() {
        "0".into()
    } else if negative {
        format!("-{digits}")
    } else {
        digits.into()
    }
}

fn compare_ints(a: &str, b: &str) -> Ordering {
    let (an, ad) = a.strip_prefix('-').map_or((false, a), |d| (true, d));
    let (bn, bd) = b.strip_prefix('-').map_or((false, b), |d| (true, d));
    match (an, bn) {
        (false, true) => Ordering::Greater,
        (true, false) => Ordering::Less,
        _ => {
            let magnitude = ad.len().cmp(&bd.len()).then_with(|| ad.cmp(bd));
            if an {
                magnitude.reverse()
            } else {
                magnitude
            }
        }
    }
}

/// Python's exact comparison of numbers, ints against floats included; NaN
/// cannot come from the strict codec.
pub fn compare_numbers(a: &PyNumber, b: &PyNumber) -> Option<Ordering> {
    match (a, b) {
        (PyNumber::Int(a), PyNumber::Int(b)) => Some(compare_ints(a, b)),
        (PyNumber::Float(a), PyNumber::Float(b)) => a.partial_cmp(b),
        (PyNumber::Int(a), PyNumber::Float(b)) => compare_int_float(a, *b),
        (PyNumber::Float(a), PyNumber::Int(b)) => compare_int_float(b, *a).map(Ordering::reverse),
    }
}

fn compare_int_float(int: &str, float: f64) -> Option<Ordering> {
    if float.is_nan() {
        return None;
    }
    if float.is_infinite() {
        return Some(if float > 0.0 {
            Ordering::Less
        } else {
            Ordering::Greater
        });
    }
    let floor = float.floor();
    // The exact decimal expansion of an integral double.
    let whole = normal_int(&format!("{floor:.0}"));
    match compare_ints(int, &whole) {
        Ordering::Equal if float > floor => Some(Ordering::Less),
        ordering => Some(ordering),
    }
}

/// The number a value stands for in Python arithmetic and comparisons:
/// `bool` is an `int` there.
fn numeric(value: &Value) -> Option<PyNumber> {
    match value {
        Value::Bool(flag) => Some(PyNumber::Int(if *flag { "1" } else { "0" }.into())),
        Value::Number(number) => Some(PyNumber::of(number)),
        _ => None,
    }
}

/// Python's `a == b` for JSON values.
pub fn py_eq(a: &Value, b: &Value) -> bool {
    if let (Some(a), Some(b)) = (numeric(a), numeric(b)) {
        return compare_numbers(&a, &b) == Some(Ordering::Equal);
    }
    match (a, b) {
        (Value::Null, Value::Null) => true,
        (Value::String(a), Value::String(b)) => a == b,
        (Value::Array(a), Value::Array(b)) => {
            a.len() == b.len() && a.iter().zip(b).all(|(a, b)| py_eq(a, b))
        }
        (Value::Object(a), Value::Object(b)) => {
            a.len() == b.len()
                && a.iter()
                    .all(|(key, value)| b.get(key).is_some_and(|other| py_eq(value, other)))
        }
        _ => false,
    }
}

/// `x == "text"`.
pub fn is_str(value: Option<&Value>, text: &str) -> bool {
    value.and_then(Value::as_str) == Some(text)
}

/// `x is True`.
pub fn is_true(value: Option<&Value>) -> bool {
    value == Some(&Value::Bool(true))
}

/// `type(x) is int` for a JSON value: an integer literal, never a bool.
pub fn int_literal(value: &Value) -> Option<String> {
    match value {
        Value::Number(number) => match PyNumber::of(number) {
            PyNumber::Int(digits) => Some(digits),
            PyNumber::Float(_) => None,
        },
        _ => None,
    }
}

/// `int(lower) <= value <= int(upper)` for an integer literal.
pub fn int_within(digits: &str, lower: &str, upper: &str) -> bool {
    compare_ints(digits, lower) != Ordering::Less
        && compare_ints(digits, upper) != Ordering::Greater
}

/// `type(x) in (int, float) and 0 <= x <= 2**53 - 1 and math.isfinite(x)`.
pub fn bounded_number(value: &Value) -> bool {
    let Value::Number(number) = value else {
        return false;
    };
    let number = PyNumber::of(number);
    number.is_finite()
        && compare_numbers(&number, &PyNumber::Int("0".into())) != Some(Ordering::Less)
        && compare_numbers(&number, &PyNumber::Int("9007199254740991".into()))
            != Some(Ordering::Greater)
}

/// Python truthiness of a JSON value.
pub fn truthy(value: &Value) -> bool {
    match value {
        Value::Null => false,
        Value::Bool(flag) => *flag,
        Value::Number(number) => match PyNumber::of(number) {
            PyNumber::Int(digits) => digits != "0",
            PyNumber::Float(value) => value != 0.0,
        },
        Value::String(text) => !text.is_empty(),
        Value::Array(items) => !items.is_empty(),
        Value::Object(members) => !members.is_empty(),
    }
}

/// `for item in value`: a list yields its items, a dict its keys and a
/// string its characters; anything else is a TypeError.
pub fn py_iter(value: &Value) -> PyResult<Vec<Value>> {
    match value {
        Value::Array(items) => Ok(items.clone()),
        Value::Object(members) => Ok(members.keys().cloned().map(Value::String).collect()),
        Value::String(text) => Ok(text.chars().map(|c| Value::String(c.into())).collect()),
        _ => Err(PyError::Type),
    }
}

/// A value usable as a set member or dict key: a list or a dict is not.
pub fn hashable(value: &Value) -> PyResult<&Value> {
    match value {
        Value::Array(_) | Value::Object(_) => Err(PyError::Type),
        _ => Ok(value),
    }
}

/// `str.isspace()` for one character: Unicode whitespace plus the ASCII
/// information separators U+001C-U+001F, which Rust does not count.
pub fn py_isspace(c: char) -> bool {
    c.is_whitespace() || ('\u{1c}'..='\u{1f}').contains(&c)
}

/// `value.strip()`.
pub fn py_strip(value: &str) -> &str {
    value.trim_matches(py_isspace)
}

/// `len(value)` of a Python string: code points.
pub fn py_len(value: &str) -> usize {
    value.chars().count()
}

fn control(c: char) -> bool {
    (c as u32) < 32 || c as u32 == 127
}

/// `context.bounded(value, maximum, empty)`.
pub fn bounded(value: Option<&Value>, maximum: usize, empty: bool) -> PyResult<String> {
    match value {
        Some(Value::String(text))
            if py_len(text) <= maximum
                && (empty || !text.is_empty())
                && !text.chars().any(control) =>
        {
            Ok(text.clone())
        }
        _ => Err(PyError::value("invalid bounded native identity")),
    }
}

/// `context.token(value, maximum)`: `[A-Za-z0-9._:-]{1,maximum}`.
pub fn token(value: Option<&Value>, maximum: usize) -> PyResult<String> {
    match value {
        Some(Value::String(text))
            if !text.is_empty()
                && text.len() <= maximum
                && text
                    .bytes()
                    .all(|b| b.is_ascii_alphanumeric() || b"._:-".contains(&b)) =>
        {
            Ok(text.clone())
        }
        _ => Err(PyError::value("invalid native token")),
    }
}

/// Python's `accounts.text`: control characters removed, then cut to
/// `maximum` code points; `None` for a non-string.
pub fn text(value: Option<&Value>, maximum: usize) -> Option<String> {
    match value {
        Some(Value::String(text)) => Some(
            text.chars()
                .filter(|c| !control(*c))
                .take(maximum)
                .collect(),
        ),
        _ => None,
    }
}

/// `str(uuid.UUID(value)) == value`: the canonical lowercase form.
pub fn canonical_uuid(value: &str) -> bool {
    let bytes = value.as_bytes();
    bytes.len() == 36
        && bytes.iter().enumerate().all(|(index, byte)| match index {
            8 | 13 | 18 | 23 => *byte == b'-',
            _ => byte.is_ascii_digit() || (b'a'..=b'f').contains(byte),
        })
}

/// `uuid.UUID(value)`, as its 128-bit value, with Python's ValueError texts:
/// `urn:` and `uuid:` removed, braces stripped, hyphens dropped, 32 code
/// points left, then `int(hex, 16)` (whitespace, a sign, a `0x` prefix and
/// single underscores between digits allowed).
pub fn py_uuid(value: &str) -> PyResult<u128> {
    let hex = value.replace("urn:", "").replace("uuid:", "");
    let hex = hex.trim_matches(['{', '}']).replace('-', "");
    if py_len(&hex) != 32 {
        return Err(PyError::value("badly formed hexadecimal UUID string"));
    }
    parse_hex(&hex).ok_or_else(|| {
        PyError::Value(format!(
            "invalid literal for int() with base 16: {}",
            py_repr(&hex)
        ))
    })
}

/// `int(text, 16)` for a value known to fit 128 bits.
fn parse_hex(text: &str) -> Option<u128> {
    let text = text.trim_matches(py_isspace);
    let text = text.strip_prefix(['+', '-']).unwrap_or(text);
    let text = text
        .strip_prefix("0x")
        .or_else(|| text.strip_prefix("0X"))
        .map_or(text, |rest| rest.strip_prefix('_').unwrap_or(rest));
    if text.is_empty() || text.starts_with('_') || text.ends_with('_') || text.contains("__") {
        return None;
    }
    let mut value: u128 = 0;
    for c in text.chars().filter(|c| *c != '_') {
        let digit = c.to_digit(16).or_else(|| unicode_digit(c))?;
        value = value.checked_mul(16)?.checked_add(digit as u128)?;
    }
    Some(value)
}

/// A non-ASCII decimal digit (Unicode category Nd), which Python's `int()`
/// also reads. The blocks of ten listed here are the Nd ranges up to the
/// Basic Multilingual Plane, enough for the 32-character values judged here.
fn unicode_digit(c: char) -> Option<u32> {
    const ZEROS: &[u32] = &[
        0x660, 0x6F0, 0x7C0, 0x966, 0x9E6, 0xA66, 0xAE6, 0xB66, 0xBE6, 0xC66, 0xCE6, 0xD66, 0xDE6,
        0xE50, 0xED0, 0xF20, 0x1040, 0x1090, 0x17E0, 0x1810, 0x1946, 0x19D0, 0x1A80, 0x1A90,
        0x1B50, 0x1BB0, 0x1C40, 0x1C50, 0xA620, 0xA8D0, 0xA900, 0xA9D0, 0xA9F0, 0xAA50, 0xABF0,
        0xFF10,
    ];
    let code = c as u32;
    ZEROS
        .iter()
        .find(|zero| (**zero..**zero + 10).contains(&code))
        .map(|zero| code - zero)
}

/// Python's `repr()` of a string, for the one library message that quotes
/// one. Printable non-ASCII characters stay; C0 and C1 controls, DEL, the
/// line and paragraph separators, format characters and non-breaking spaces
/// are escaped as Python escapes them.
pub fn py_repr(value: &str) -> String {
    let quote = if value.contains('\'') && !value.contains('"') {
        '"'
    } else {
        '\''
    };
    let mut out = String::new();
    out.push(quote);
    for c in value.chars() {
        match c {
            '\\' => out.push_str("\\\\"),
            '\n' => out.push_str("\\n"),
            '\r' => out.push_str("\\r"),
            '\t' => out.push_str("\\t"),
            c if c == quote => {
                out.push('\\');
                out.push(c);
            }
            c if printable(c) => out.push(c),
            c if (c as u32) < 0x100 => out.push_str(&format!("\\x{:02x}", c as u32)),
            c if (c as u32) < 0x10000 => out.push_str(&format!("\\u{:04x}", c as u32)),
            c => out.push_str(&format!("\\U{:08x}", c as u32)),
        }
    }
    out.push(quote);
    out
}

fn printable(c: char) -> bool {
    let code = c as u32;
    !(code < 0x20
        || (0x7f..=0xa0).contains(&code)
        || code == 0xad
        || code == 0x1680
        || (0x2000..=0x200f).contains(&code)
        || (0x2028..=0x202f).contains(&code)
        || (0x205f..=0x206f).contains(&code)
        || code == 0x3000
        || (0xd800..=0xf8ff).contains(&code)
        || code == 0xfeff
        || (0xfff0..=0xfffb).contains(&code)
        || code >= 0xf0000)
}

/// A JSON object member that is a string, or `None`.
pub fn get_str<'a>(object: &'a Map<String, Value>, key: &str) -> Option<&'a str> {
    object.get(key).and_then(Value::as_str)
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    fn parse(text: &str) -> Value {
        zerus_relay::json::parse(text.as_bytes(), 16, 1000).unwrap()
    }

    #[test]
    fn equality_follows_python() {
        assert!(py_eq(&json!(true), &parse("1")));
        assert!(py_eq(&parse("1.0"), &parse("1")));
        assert!(py_eq(&json!(false), &parse("0.0")));
        assert!(py_eq(&parse("1.00"), &parse("1.0")));
        assert!(!py_eq(
            &parse("9007199254740993"),
            &parse("9007199254740992.0")
        ));
        assert!(py_eq(&parse("100000000000000000000"), &parse("1e20")));
        assert!(py_eq(&parse("-0"), &parse("0")));
        assert!(py_eq(
            &parse("{\"ok\": 0, \"error\": \"not_found\"}"),
            &json!({"ok": false, "error": "not_found"})
        ));
        assert!(!py_eq(&json!("1"), &parse("1")));
        assert!(!py_eq(&json!(null), &json!(false)));
    }

    #[test]
    fn number_order_is_exact() {
        let n = |text: &str| match parse(text) {
            Value::Number(number) => PyNumber::of(&number),
            _ => unreachable!(),
        };
        assert_eq!(compare_numbers(&n("2"), &n("1.5")), Some(Ordering::Greater));
        assert_eq!(compare_numbers(&n("1"), &n("1.5")), Some(Ordering::Less));
        assert_eq!(compare_numbers(&n("-2"), &n("-1.5")), Some(Ordering::Less));
        assert_eq!(
            compare_numbers(&n("-1"), &n("-1.5")),
            Some(Ordering::Greater)
        );
        assert_eq!(
            compare_numbers(&n("123456789012345678901234567890"), &n("1e300")),
            Some(Ordering::Less)
        );
        assert_eq!(compare_numbers(&n("10"), &n("1e400")), Some(Ordering::Less));
    }

    #[test]
    fn iteration_and_hashing_follow_python() {
        assert_eq!(py_iter(&json!("ab")).unwrap(), [json!("a"), json!("b")]);
        assert_eq!(py_iter(&json!({"k": 1})).unwrap(), [json!("k")]);
        assert_eq!(py_iter(&json!(null)), Err(PyError::Type));
        assert_eq!(py_iter(&json!(1)), Err(PyError::Type));
        assert_eq!(hashable(&json!([1])), Err(PyError::Type));
        assert!(hashable(&json!(1.5)).is_ok());
    }

    #[test]
    fn strings_follow_python() {
        assert_eq!(py_strip("\u{1c} x \u{a0}\u{3000}"), "x");
        assert_eq!(py_len("\u{e9}\u{1f600}"), 2);
        assert_eq!(
            bounded(Some(&json!("\u{e9}\u{e9}")), 2, false).unwrap(),
            "\u{e9}\u{e9}"
        );
        assert!(bounded(Some(&json!("a\u{7f}")), 9, false).is_err());
        assert!(bounded(Some(&json!("")), 9, false).is_err());
        assert!(bounded(Some(&json!("")), 9, true).is_ok());
        assert!(bounded(Some(&json!(1)), 9, true).is_err());
        assert_eq!(text(Some(&json!("a\nb\u{e9}c")), 2).unwrap(), "ab");
        assert!(token(Some(&json!("g:1.x-y_z")), 9).is_ok());
        assert!(token(Some(&json!("a b")), 9).is_err());
    }

    #[test]
    fn uuids_follow_python() {
        assert!(canonical_uuid("81b191c0-bdae-543a-ba1c-0fbcbb2f638d"));
        assert!(!canonical_uuid("81B191C0-BDAE-543A-BA1C-0FBCBB2F638D"));
        assert_eq!(
            py_uuid("{81B191C0BDAE543ABA1C0FBCBB2F638D}").unwrap() >> 96,
            0x81b191c0
        );
        assert_eq!(
            py_uuid("urn:uuid:00000000-0000-0000-0000-000000000000").unwrap(),
            0
        );
        assert_eq!(
            py_uuid("short"),
            Err(PyError::value("badly formed hexadecimal UUID string"))
        );
        assert_eq!(
            py_uuid("g0000000-0000-0000-0000-000000000000"),
            Err(PyError::value(
                "invalid literal for int() with base 16: 'g0000000000000000000000000000000'"
            ))
        );
        assert!(py_uuid("0x00000000000000000000000000000f").is_ok());
        assert!(py_uuid("0000_0000000000000000000000000ff").is_ok());
        assert!(py_uuid("0000__000000000000000000000000ff").is_err());
        assert_eq!(py_repr("a'b"), "\"a'b\"");
        assert_eq!(py_repr("\u{85}\u{e9}"), "'\\x85\u{e9}'");
    }
}
