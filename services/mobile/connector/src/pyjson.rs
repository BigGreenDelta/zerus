//! Reads `connector.json` with exactly the acceptance of Python's `json.loads`
//! (the C scanner): `NaN`, `Infinity` and `-Infinity` literals, lone surrogate
//! escapes, repeated keys (the last wins) and unbounded integers up to 4,300
//! digits. A strict parser would refuse files that the Python connector
//! starts with, for example a `NaN` under a key nobody reads.
//!
//! Untrusted protocol JSON never goes through this reader; it uses the relay
//! codec (`crate::request`).

/// Python's JSONDecodeError, or the ValueError of an overlong integer.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct DecodeError;

/// Python's default `sys.int_info.default_max_str_digits`.
const MAX_INT_DIGITS: usize = 4300;
/// Python fails with RecursionError near its default limit of 1,000 frames.
const MAX_DEPTH: usize = 1000;

#[derive(Debug, Clone, PartialEq)]
pub enum Value {
    Null,
    Bool(bool),
    /// An integer literal as written: Python integers are unbounded.
    Int(String),
    /// A float as Python's `float()` reads it, so `1e400` is infinite.
    Float(f64),
    Str(Text),
    Array(Vec<Value>),
    /// Members in document order; lookups take the last repeated key.
    Object(Vec<(Text, Value)>),
}

/// UTF-16 code units, so a lone surrogate survives as Python keeps it.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Text(pub Vec<u16>);

impl Text {
    /// The text, or `None` when it holds a lone surrogate, which no Rust
    /// string (and no UTF-8 file path or URL) can represent.
    pub fn as_str(&self) -> Option<String> {
        String::from_utf16(&self.0).ok()
    }
    pub fn is_empty(&self) -> bool {
        self.0.is_empty()
    }
    fn equals(&self, key: &str) -> bool {
        self.0.iter().copied().eq(key.encode_utf16())
    }
}

impl Value {
    /// `dict.get(key)`.
    pub fn get(&self, key: &str) -> Option<&Value> {
        match self {
            Value::Object(members) => members
                .iter()
                .rev()
                .find(|(k, _)| k.equals(key))
                .map(|(_, v)| v),
            _ => None,
        }
    }
    /// Python truthiness, which `config.get("state_dir") or default` uses.
    pub fn truthy(&self) -> bool {
        match self {
            Value::Null => false,
            Value::Bool(b) => *b,
            Value::Int(digits) => digits.trim_start_matches('-') != "0",
            Value::Float(f) => *f != 0.0,
            Value::Str(text) => !text.is_empty(),
            Value::Array(items) => !items.is_empty(),
            Value::Object(members) => !members.is_empty(),
        }
    }
    /// The Python type name, for `--check` findings.
    pub fn kind(&self) -> &'static str {
        match self {
            Value::Null => "null",
            Value::Bool(_) => "boolean",
            Value::Int(_) | Value::Float(_) => "number",
            Value::Str(_) => "string",
            Value::Array(_) => "array",
            Value::Object(_) => "object",
        }
    }
}

/// `json.loads(text)`; any error stands for Python's JSONDecodeError or
/// ValueError, which `main()` turns into the fixed startup line.
pub fn parse(text: &str) -> Result<Value, DecodeError> {
    let chars: Vec<char> = text.chars().collect();
    let mut reader = Reader {
        chars: &chars,
        at: 0,
    };
    reader.whitespace();
    let value = reader.value(0)?;
    reader.whitespace();
    if reader.at != chars.len() {
        return Err(DecodeError);
    }
    Ok(value)
}

struct Reader<'a> {
    chars: &'a [char],
    at: usize,
}

impl Reader<'_> {
    fn peek(&self) -> Option<char> {
        self.chars.get(self.at).copied()
    }
    fn whitespace(&mut self) {
        while matches!(self.peek(), Some(' ' | '\t' | '\n' | '\r')) {
            self.at += 1;
        }
    }
    fn literal(&mut self, word: &str) -> bool {
        let end = self.at + word.chars().count();
        if end <= self.chars.len() && self.chars[self.at..end].iter().copied().eq(word.chars()) {
            self.at = end;
            return true;
        }
        false
    }
    fn value(&mut self, depth: usize) -> Result<Value, DecodeError> {
        match self.peek().ok_or(DecodeError)? {
            '"' => {
                self.at += 1;
                self.string().map(Value::Str)
            }
            '{' => self.object(depth + 1),
            '[' => self.array(depth + 1),
            'n' if self.literal("null") => Ok(Value::Null),
            't' if self.literal("true") => Ok(Value::Bool(true)),
            'f' if self.literal("false") => Ok(Value::Bool(false)),
            'N' if self.literal("NaN") => Ok(Value::Float(f64::NAN)),
            'I' if self.literal("Infinity") => Ok(Value::Float(f64::INFINITY)),
            '-' if self.literal("-Infinity") => Ok(Value::Float(f64::NEG_INFINITY)),
            '-' | '0'..='9' => self.number(),
            _ => Err(DecodeError),
        }
    }
    fn digits(&mut self) -> usize {
        let start = self.at;
        while matches!(self.peek(), Some('0'..='9')) {
            self.at += 1;
        }
        self.at - start
    }
    /// `(-?(?:0|[1-9]\d*))(\.\d+)?([eE][-+]?\d+)?` with ASCII digits only.
    fn number(&mut self) -> Result<Value, DecodeError> {
        let start = self.at;
        if self.peek() == Some('-') {
            self.at += 1;
        }
        let integer = self.at;
        match self.peek() {
            Some('0') => self.at += 1,
            Some('1'..='9') => {
                self.digits();
            }
            _ => return Err(DecodeError),
        }
        let digits = self.at - integer;
        let mut float = false;
        if self.peek() == Some('.') && matches!(self.chars.get(self.at + 1), Some('0'..='9')) {
            self.at += 1;
            self.digits();
            float = true;
        }
        if matches!(self.peek(), Some('e' | 'E')) {
            let mark = self.at;
            self.at += 1;
            if matches!(self.peek(), Some('-' | '+')) {
                self.at += 1;
            }
            if self.digits() == 0 {
                // The regex leaves an incomplete exponent unmatched.
                self.at = mark;
            } else {
                float = true;
            }
        }
        let literal: String = self.chars[start..self.at].iter().collect();
        if float {
            // Rust's parser rounds like CPython's, overflowing to infinity.
            return literal.parse().map(Value::Float).map_err(|_| DecodeError);
        }
        if digits > MAX_INT_DIGITS {
            return Err(DecodeError);
        }
        Ok(Value::Int(literal))
    }
    fn hex4(&mut self) -> Result<u16, DecodeError> {
        let mut unit = 0u16;
        for _ in 0..4 {
            let digit = self
                .peek()
                .and_then(|c| c.to_digit(16))
                .ok_or(DecodeError)?;
            unit = (unit << 4) | digit as u16;
            self.at += 1;
        }
        Ok(unit)
    }
    fn string(&mut self) -> Result<Text, DecodeError> {
        let mut units = Vec::new();
        loop {
            let c = self.peek().ok_or(DecodeError)?;
            self.at += 1;
            match c {
                '"' => return Ok(Text(units)),
                '\\' => {
                    let escape = self.peek().ok_or(DecodeError)?;
                    self.at += 1;
                    let unit = match escape {
                        '"' => 0x22,
                        '\\' => 0x5c,
                        '/' => 0x2f,
                        'b' => 0x08,
                        'f' => 0x0c,
                        'n' => 0x0a,
                        'r' => 0x0d,
                        't' => 0x09,
                        'u' => {
                            let unit = self.hex4()?;
                            units.push(unit);
                            // A high surrogate joins only an immediately escaped low one.
                            if (0xd800..0xdc00).contains(&unit)
                                && self.at + 6 < self.chars.len()
                                && self.chars[self.at] == '\\'
                                && self.chars[self.at + 1] == 'u'
                            {
                                let mark = self.at;
                                self.at += 2;
                                let low = self.hex4()?;
                                if (0xdc00..0xe000).contains(&low) {
                                    units.push(low);
                                } else {
                                    self.at = mark;
                                }
                            }
                            continue;
                        }
                        _ => return Err(DecodeError),
                    };
                    units.push(unit);
                }
                c if (c as u32) < 0x20 => return Err(DecodeError),
                c => {
                    let mut buffer = [0u16; 2];
                    units.extend_from_slice(c.encode_utf16(&mut buffer));
                }
            }
        }
    }
    fn object(&mut self, depth: usize) -> Result<Value, DecodeError> {
        if depth > MAX_DEPTH {
            return Err(DecodeError);
        }
        self.at += 1;
        let mut members = Vec::new();
        self.whitespace();
        if self.peek() == Some('}') {
            self.at += 1;
            return Ok(Value::Object(members));
        }
        loop {
            if self.peek() != Some('"') {
                return Err(DecodeError);
            }
            self.at += 1;
            let key = self.string()?;
            self.whitespace();
            if self.peek() != Some(':') {
                return Err(DecodeError);
            }
            self.at += 1;
            self.whitespace();
            let value = self.value(depth)?;
            members.push((key, value));
            self.whitespace();
            match self.peek() {
                Some('}') => {
                    self.at += 1;
                    return Ok(Value::Object(members));
                }
                Some(',') => {
                    self.at += 1;
                    self.whitespace();
                }
                _ => return Err(DecodeError),
            }
        }
    }
    fn array(&mut self, depth: usize) -> Result<Value, DecodeError> {
        if depth > MAX_DEPTH {
            return Err(DecodeError);
        }
        self.at += 1;
        let mut items = Vec::new();
        self.whitespace();
        if self.peek() == Some(']') {
            self.at += 1;
            return Ok(Value::Array(items));
        }
        loop {
            items.push(self.value(depth)?);
            self.whitespace();
            match self.peek() {
                Some(']') => {
                    self.at += 1;
                    return Ok(Value::Array(items));
                }
                Some(',') => {
                    self.at += 1;
                    self.whitespace();
                }
                _ => return Err(DecodeError),
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn text(value: &Value) -> Option<String> {
        match value {
            Value::Str(text) => text.as_str(),
            _ => None,
        }
    }

    #[test]
    fn accepts_what_python_json_loads_accepts() {
        let config = parse(
            " {\"a\": NaN, \"b\": -Infinity, \"c\": [1e400, -0, 0.5e-3], \"a\": \"x\\/\\u00e9\"}\n",
        )
        .unwrap();
        assert_eq!(text(config.get("a").unwrap()).as_deref(), Some("x/\u{e9}"));
        assert_eq!(config.get("b"), Some(&Value::Float(f64::NEG_INFINITY)));
        match config.get("c") {
            Some(Value::Array(items)) => {
                assert_eq!(items[0], Value::Float(f64::INFINITY));
                assert_eq!(items[1], Value::Int("-0".into()));
                assert!(!items[1].truthy());
                assert_eq!(items[2], Value::Float(0.0005));
            }
            other => panic!("{other:?}"),
        }
        assert!(
            matches!(parse("{\"k\": \"\\ud800\"}").unwrap().get("k"), Some(Value::Str(t)) if t.as_str().is_none())
        );
        assert_eq!(
            text(&parse("\"\\ud83d\\ude00\"").unwrap()).as_deref(),
            Some("\u{1f600}")
        );
        // A high surrogate is joined only with an escaped low surrogate right after it.
        assert!(
            matches!(parse("\"\\ud83d\\u0041\"").unwrap(), Value::Str(t) if t.0 == [0xd83d, 0x41])
        );
        let digits = format!("[{}]", "9".repeat(4300));
        assert!(parse(&digits).is_ok());
        assert!(parse(&format!("[{}]", "9".repeat(4301))).is_err());
        assert!(parse(&format!("[{}.0]", "9".repeat(5000))).is_ok());
    }

    #[test]
    fn refuses_what_python_json_loads_refuses() {
        for text in [
            "",
            "{",
            "[1,]",
            "{\"a\":1,}",
            "{'a':1}",
            "01",
            "1.",
            ".5",
            "+1",
            "-",
            "1e",
            "\"\t\"",
            "\"\\x\"",
            "\"\\u12g4\"",
            "nul",
            "[1] [2]",
            "\u{feff}{}",
            "{\"a\" 1}",
            "[1 2]",
            "{1:2}",
            "\u{a0}1",
        ] {
            assert!(parse(text).is_err(), "{text:?}");
        }
        assert!(parse(&"[".repeat(1001)).is_err());
        assert!(parse(&format!("{}{}", "[".repeat(1000), "]".repeat(1000))).is_ok());
    }

    #[test]
    fn python_truthiness() {
        for (text, truthy) in [
            ("null", false),
            ("false", false),
            ("true", true),
            ("0", false),
            ("-0", false),
            ("0.0", false),
            ("-0.0", false),
            ("NaN", true),
            ("\"\"", false),
            ("\"x\"", true),
            ("[]", false),
            ("[0]", true),
            ("{}", false),
            ("{\"a\":null}", true),
            ("7", true),
        ] {
            assert_eq!(parse(text).unwrap().truthy(), truthy, "{text}");
        }
    }
}
