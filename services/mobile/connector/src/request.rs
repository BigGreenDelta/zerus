//! Request identity and stored receipt text, through the relay codec.
use serde_json::{Map, Value};
use zerus_relay::{error::Error, json};

pub use zerus_relay::json::MAX_INT_DIGITS;

/// Untrusted JSON (relay batches, native output, stored rows) under explicit
/// bounds. Beyond the relay codec, an integer Python could not read is
/// refused, so nothing Python refuses is executed here; the relay refuses the
/// same integers at submission.
pub fn parse(raw: &[u8], depth_limit: usize, value_limit: usize) -> Result<Value, Error> {
    let value = json::parse(raw, depth_limit, value_limit)?;
    if !json::python_integers(&value) {
        return Err(Error::BAD);
    }
    Ok(value)
}

/// `sha256(json.dumps(request, sort_keys=True, separators=(",", ":"),
/// allow_nan=False))` over the request as received, `gateway_route` and
/// `expires_at` included (`connector.py:807`).
pub fn request_hash(request: &Value) -> Result<String, Error> {
    Ok(json::digest(json::canonical(request)?))
}

/// `json.dumps(receipt)` as the Python journal stores it: `state`, `result`
/// and `error` first in that order, Python's default separators and ASCII
/// escapes. Nested objects come out sorted where Python keeps the native
/// order, which changes no length.
pub fn receipt_text(receipt: &Value) -> Result<String, Error> {
    let Value::Object(members) = receipt else {
        return python_text(receipt);
    };
    let mut keys: Vec<&String> = members.keys().collect();
    keys.sort_by_key(|key| {
        (
            ["state", "result", "error"]
                .iter()
                .position(|first| first == key)
                .unwrap_or(3),
            *key,
        )
    });
    let mut out = Vec::new();
    out.push(b'{');
    for (index, key) in keys.into_iter().enumerate() {
        if index > 0 {
            out.extend_from_slice(b", ");
        }
        json::python_default_encode(&Value::String(key.clone()), true, &mut out)?;
        out.extend_from_slice(b": ");
        json::python_default_encode(&members[key], true, &mut out)?;
    }
    out.push(b'}');
    String::from_utf8(out).map_err(|_| Error::BAD)
}

fn python_text(value: &Value) -> Result<String, Error> {
    let mut out = Vec::new();
    json::python_default_encode(value, true, &mut out)?;
    String::from_utf8(out).map_err(|_| Error::BAD)
}

/// `{"state": state, "result": result, "error": error}`.
pub fn receipt(state: &str, result: Value, error: Option<&str>) -> Value {
    let mut members = Map::new();
    members.insert("state".into(), Value::String(state.into()));
    members.insert("result".into(), result);
    members.insert(
        "error".into(),
        error.map_or(Value::Null, |text| Value::String(text.into())),
    );
    Value::Object(members)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn receipts_keep_python_key_order_and_spelling() {
        let stored = receipt_text(&receipt("uncertain", Value::Null, Some("caf\u{e9}"))).unwrap();
        assert_eq!(
            stored,
            "{\"state\": \"uncertain\", \"result\": null, \"error\": \"caf\\u00e9\"}"
        );
        let raw = "{\"error\":null,\"result\":{\"b\":[1,2.50],\"a\":\"\\ud83d\\ude00\"},\"state\":\"completed\",\"x\":1}";
        let nested = parse(raw.as_bytes(), 8, 64).unwrap();
        assert_eq!(receipt_text(&nested).unwrap(),
                   "{\"state\": \"completed\", \"result\": {\"a\": \"\\ud83d\\ude00\", \"b\": [1, 2.5]}, \"error\": null, \"x\": 1}");
    }

    #[test]
    fn integers_python_cannot_read_are_refused() {
        let at = |digits: usize| format!("{{\"x\":[-{}]}}", "7".repeat(digits));
        assert!(parse(at(MAX_INT_DIGITS).as_bytes(), 8, 64).is_ok());
        assert!(parse(at(MAX_INT_DIGITS + 1).as_bytes(), 8, 64).is_err());
        assert!(parse(format!("[{}.5]", "7".repeat(5000)).as_bytes(), 8, 64).is_ok());
    }
}
