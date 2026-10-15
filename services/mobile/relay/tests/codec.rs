//! API v1 codec contracts. They need no server modules, so
//! `cargo test --no-default-features --test codec` also checks the connector's build.
use serde_json::{json, Map, Value};
use zerus_relay::{
    error::Error,
    json::{canonical, parse, python_default_encode, python_default_len},
};

fn value(raw: &str) -> Value {
    parse(raw.as_bytes(), 32, 50000).unwrap()
}
fn dumps(v: &Value, ensure_ascii: bool) -> String {
    let mut out = Vec::new();
    python_default_encode(v, ensure_ascii, &mut out).unwrap();
    String::from_utf8(out).unwrap()
}
fn rejected(raw: &[u8], depth: usize, values: usize) -> Error {
    parse(raw, depth, values).unwrap_err()
}

#[test]
fn goldens_match_python() {
    let escapes = r#""\"\\\/\b\f\n\r\t\u0000\u001f\u007f \u00e9\u2028\ud83d\ude00""#;
    let escaped = r#""\"\\/\b\f\n\r\t\u0000\u001f\u007f \u00e9\u2028\ud83d\ude00""#;
    let unescaped = concat!(
        r#""\"\\/\b\f\n\r\t\u0000\u001f"#,
        "\u{7f} \u{e9}\u{2028}\u{1f600}\""
    );
    // Input, the identity form, and json.dumps(sort_keys=True) with
    // ensure_ascii true and false, all captured from CPython.
    for (raw, identity, ascii, unicode) in [
        (
            r#"{"b":1,"a":2}"#,
            r#"{"a":2,"b":1}"#,
            r#"{"a": 2, "b": 1}"#,
            r#"{"a": 2, "b": 1}"#,
        ),
        (
            "18446744073709551616",
            "18446744073709551616",
            "18446744073709551616",
            "18446744073709551616",
        ),
        ("-0", "0", "0", "0"),
        ("-0.0", "-0.0", "-0.0", "-0.0"),
        (
            r#"{"$serde_json::private::Number":"123"}"#,
            r#"{"$serde_json::private::Number":"123"}"#,
            r#"{"$serde_json::private::Number": "123"}"#,
            r#"{"$serde_json::private::Number": "123"}"#,
        ),
        (escapes, escaped, escaped, unescaped),
        (
            r#"[{"z":[],"y":{}},[1,2.5e-5,1E16,1e15,0.1]]"#,
            r#"[{"y":{},"z":[]},[1,2.5e-05,1e+16,1000000000000000.0,0.1]]"#,
            r#"[{"y": {}, "z": []}, [1, 2.5e-05, 1e+16, 1000000000000000.0, 0.1]]"#,
            r#"[{"y": {}, "z": []}, [1, 2.5e-05, 1e+16, 1000000000000000.0, 0.1]]"#,
        ),
    ] {
        let v = value(raw);
        assert_eq!(canonical(&v).unwrap(), identity, "{raw}");
        assert_eq!(dumps(&v, true), ascii, "{raw}");
        assert_eq!(dumps(&v, false), unicode, "{raw}");
        assert_eq!(python_default_len(&v, true).unwrap(), ascii.len(), "{raw}");
        assert_eq!(
            python_default_len(&v, false).unwrap(),
            unicode.len(),
            "{raw}"
        );
    }
    // The key is user data, never arbitrary_precision's number marker.
    let v = value(r#"{"$serde_json::private::Number":"123"}"#);
    assert_eq!(v["$serde_json::private::Number"], "123");
    // Python reads 1e400 as infinity and would write Infinity, which is not JSON.
    let v = value("[1e400]");
    assert!(canonical(&v).is_err());
    assert_eq!(python_default_len(&v, true).unwrap_err().0, Error::BAD.0);
    assert_eq!(python_default_len(&v, false).unwrap_err().0, Error::BAD.0);
}

#[test]
fn keys_sort_by_code_point_whatever_the_map_order() {
    // Inserted in reverse. With serde_json's preserve_order feature the map
    // iterates in this order, and UTF-16 order would put U+10000 before U+FFFF.
    let mut map = Map::new();
    for (key, n) in [
        ("\u{10000}", 5),
        ("\u{ffff}", 4),
        ("\u{e9}", 3),
        ("z", 2),
        ("a", 1),
    ] {
        map.insert(key.to_owned(), n.into());
    }
    let v = Value::Object(map);
    assert_eq!(
        canonical(&v).unwrap(),
        r#"{"a":1,"z":2,"\u00e9":3,"\uffff":4,"\ud800\udc00":5}"#
    );
    assert_eq!(
        dumps(&v, false),
        "{\"a\": 1, \"z\": 2, \"\u{e9}\": 3, \"\u{ffff}\": 4, \"\u{10000}\": 5}"
    );
}

#[test]
fn parse_limits_count_containers_strings_and_scalars() {
    // One object, one key, one array and two scalars, nested two deep.
    for raw in [&br#"{"a":[1,true]}"#[..], b" {\n\"a\" :\t[ 1 , true ]\r} "] {
        assert!(parse(raw, 2, 5).is_ok());
        assert_eq!(rejected(raw, 1, 5).0, Error::LARGE.0);
        assert_eq!(rejected(raw, 2, 4).0, Error::LARGE.0);
    }
    // Brackets, quotes and escapes inside a string are part of that one value.
    let quoted = br#"["[{\"]}","\\"]"#;
    assert!(parse(quoted, 1, 3).is_ok());
    assert_eq!(rejected(quoted, 1, 2).0, Error::LARGE.0);
    for malformed in [
        &b"[1,2"[..],
        b"]",
        b"[1]]",
        b"\"open",
        b"{\"a\":}",
        b"NaN",
        b"[Infinity]",
    ] {
        assert_eq!(
            rejected(malformed, 32, 50000).0,
            Error::BAD.0,
            "{}",
            String::from_utf8_lossy(malformed)
        );
    }
}

#[test]
fn a_request_at_the_relay_maximum_needs_the_batch_margins() {
    // A request at the relay's maxima: depth 32 and exactly 50,000 values.
    let id = "00000000-0000-4000-8000-000000000001";
    let fixed = 42;
    let raw = format!(
        r#"{{"computer_id":"{id}","operation":"send","payload":{{"deep":{}{},"filler":[{}]}},"request_id":"{id}"}}"#,
        "[".repeat(30),
        "]".repeat(30),
        vec!["0"; 50000 - fixed].join(","),
    );
    let raw = raw.as_bytes();
    assert!(parse(raw, 32, 50000).is_ok());
    assert_eq!(rejected(raw, 31, 50000).0, Error::LARGE.0);
    assert_eq!(rejected(raw, 32, 49999).0, Error::LARGE.0);
    // The claim batch as store/requests.rs Claim::decode builds it: computer_id
    // removed, gateway_route and expires_at added, wrapped in {"requests":[...]}.
    let mut request = parse(raw, 32, 50000).unwrap();
    request.as_object_mut().unwrap().remove("computer_id");
    request["gateway_route"] = json!({"schema":1,"route_id":id,"computer_id":id,"machine_id":id});
    request["expires_at"] = json!(1900000000.5);
    let batch = canonical(&json!({ "requests": [request] })).unwrap();
    let batch = batch.as_bytes();
    let values = 50000 - 2 + 10 + 2 + 3;
    assert!(values < 50000 + 64);
    assert!(parse(batch, 34, values).is_ok());
    assert_eq!(rejected(batch, 32, 50000).0, Error::LARGE.0);
    assert_eq!(rejected(batch, 33, values).0, Error::LARGE.0);
    assert_eq!(rejected(batch, 34, values - 1).0, Error::LARGE.0);
}

/// Seeded random values covering every escape class, astral and non-ASCII
/// keys, the number marker key, integers beyond 64 bits and IEEE-754 doubles.
const GENERATOR: &str = r#"
import json, math, random, struct
rng = random.Random(5308)
pool = [chr(c) for c in range(0x20)] + list(' "\\/~\x7f') + [
    'a', 'Z', '0', '\u00e9', '\u00df', '\u4e2d', '\u2028', '\ufeff', '\uffff',
    '\U0001f600', '\U0010ffff', '$serde_json::private::Number']
def text():
    return ''.join(rng.choice(pool) for _ in range(rng.randrange(6)))
def number():
    kind = rng.randrange(4)
    if kind == 0:
        return rng.randrange(-2**70, 2**70)
    if kind == 1:
        return rng.choice([0.0, -0.0, 1e-5, 1e-4, 1e15, 1e16, 5e-324, 1.7976931348623157e308, 0.1])
    while True:
        f = struct.unpack('>d', rng.randbytes(8))[0]
        if math.isfinite(f):
            return f
def value(depth):
    kind = rng.randrange(8 if depth < 4 else 5)
    if kind == 0:
        return None
    if kind == 1:
        return rng.random() < 0.5
    if kind == 2:
        return number() if rng.random() < 0.75 else rng.randrange(-1000, 1000)
    if kind in (3, 4):
        return text()
    if kind in (5, 6):
        return {text(): value(depth + 1) for _ in range(rng.randrange(5))}
    return [value(depth + 1) for _ in range(rng.randrange(5))]
for _ in range(20000):
    v = value(0)
    print(json.dumps([v,
        json.dumps(v, sort_keys=True, separators=(',', ':'), allow_nan=False),
        json.dumps(v, sort_keys=True, ensure_ascii=True),
        json.dumps(v, sort_keys=True, ensure_ascii=False),
        len(json.dumps(v, ensure_ascii=True).encode()),
        len(json.dumps(v, ensure_ascii=False).encode())]))
"#;

#[test]
fn python_default_form_matches_cpython() {
    let output = std::process::Command::new("python3")
        .args(["-I", "-c", GENERATOR])
        .output()
        .unwrap();
    assert!(
        output.status.success(),
        "{}",
        String::from_utf8_lossy(&output.stderr)
    );
    let mut cases = 0;
    for line in String::from_utf8(output.stdout).unwrap().lines() {
        let row = value(line);
        let v = &row[0];
        assert_eq!(canonical(v).unwrap(), row[1].as_str().unwrap(), "{line}");
        assert_eq!(dumps(v, true), row[2].as_str().unwrap(), "{line}");
        assert_eq!(dumps(v, false), row[3].as_str().unwrap(), "{line}");
        // Python's lengths come from dict order, which never changes them.
        assert_eq!(python_default_len(v, true).unwrap() as u64, row[4]);
        assert_eq!(python_default_len(v, false).unwrap() as u64, row[5]);
        cases += 1;
    }
    assert_eq!(cases, 20000);
}
