//! Reading files back to a phone (`attachment_reads.py`, `file_reads.py`).
//! `attachment_read` names a file of the exact conversation by its delivery
//! receipt and position, never by a path; `file_read` names an explicit path
//! on that conversation's computer. Native hgs reads the whole file once; the
//! connector verifies its answer, keeps the verified bytes briefly in memory
//! and serves one range of at most 512 KiB per request, because one relay
//! result holds at most 1 MiB. Nothing is written to disk.
use crate::pybase64;
use crate::pyvalue::{
    bounded, canonical_uuid, int_literal, int_within, is_str, py_eq, py_len, truthy, PyError,
    PyResult,
};
use serde_json::{json, Map, Value};
use std::collections::VecDeque;
use std::sync::Arc;

/// Files per message, and the size of one file (`attachments.LIMITS`).
pub const MAX_COUNT: usize = 8;
pub const MAX_FILE_BYTES: usize = 10 * 1024 * 1024;
/// One range (`attachments.MAX_READ_BYTES`): its base64 stays below the
/// 1 MiB result limit.
pub const MAX_READ_BYTES: usize = 512 * 1024;
const MAX_ENCODED: usize = MAX_FILE_BYTES.div_ceil(3) * 4;
/// The native answer: one whole file as base64 plus small metadata
/// (`attachment_reads.NATIVE_OUTPUT_BYTES`).
pub const NATIVE_OUTPUT_BYTES: usize = MAX_ENCODED + 64 * 1024;
/// Each cache holds verified files of at most this many bytes in total, so
/// three of the largest files, and keeps each at most this long (seconds).
pub const CACHE_BYTES: usize = 3 * MAX_FILE_BYTES;
pub const CACHE_SECONDS: f64 = 120.0;

pub const ATTACHMENT_MISSING: &str =
    "attachment is not available from this conversation on the computer";
pub const FILE_UNREADABLE: &str = "The file could not be read. Check that it still exists, is readable and is at most 10 MiB, then open it again.";
pub const FILE_CHANGED: &str = "file changed during download; open it again";

const NIL: &str = "00000000-0000-0000-0000-000000000000";
const READ_FIELDS: [&str; 7] = [
    "request_id",
    "expected_run_id",
    "expected_conversation_id",
    "message_id",
    "index",
    "offset",
    "length",
];
const FILE_FIELDS: [&str; 6] = [
    "request_id",
    "expected_run_id",
    "expected_conversation_id",
    "path",
    "offset",
    "length",
];

fn error(message: &str) -> PyError {
    PyError::value(message)
}

/// A verified file: the exact bytes native hgs returned and their digest.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Entry {
    /// The canonical path native hgs read (`file_read` only).
    pub path: String,
    pub name: String,
    pub mime: String,
    pub sha256: String,
    pub data: Vec<u8>,
}

/// A cache identity: every field of the exact read it serves.
pub type Key = Vec<String>;

/// `AttachmentReads`: verified files of one machine, least recently used
/// first. The budget counts bytes, not files, and an entry expires a fixed
/// time after it was read, however often it is used.
#[derive(Debug)]
pub struct Cache {
    max_bytes: usize,
    ttl: f64,
    entries: VecDeque<(Key, f64, Arc<Entry>)>,
    size: usize,
}

impl Default for Cache {
    fn default() -> Self {
        Cache::new(CACHE_BYTES, CACHE_SECONDS)
    }
}

impl Cache {
    pub fn new(max_bytes: usize, ttl: f64) -> Self {
        Cache {
            max_bytes,
            ttl,
            entries: VecDeque::new(),
            size: 0,
        }
    }

    /// The bytes the cache holds.
    pub fn size(&self) -> usize {
        self.size
    }

    /// The entry for `key` at monotonic clock `now`, which becomes the most
    /// recently used; expired entries are dropped first.
    pub fn get(&mut self, key: &Key, now: f64) -> Option<Arc<Entry>> {
        let cutoff = now - self.ttl;
        let mut freed = 0;
        self.entries.retain(|(_, at, entry)| {
            let keep = *at > cutoff;
            if !keep {
                freed += entry.data.len();
            }
            keep
        });
        self.size -= freed;
        let index = self
            .entries
            .iter()
            .position(|(stored, _, _)| stored == key)?;
        let item = self.entries.remove(index)?;
        let entry = Arc::clone(&item.2);
        self.entries.push_back(item);
        Some(entry)
    }

    /// Keeps `entry`, read at `now`, dropping the least recently used ones
    /// beyond the budget. A file larger than the whole budget is not kept.
    pub fn put(&mut self, key: Key, entry: Arc<Entry>, now: f64) {
        if let Some(index) = self
            .entries
            .iter()
            .position(|(stored, _, _)| *stored == key)
        {
            if let Some((_, _, old)) = self.entries.remove(index) {
                self.size -= old.data.len();
            }
        }
        if entry.data.len() > self.max_bytes {
            return;
        }
        self.size += entry.data.len();
        self.entries.push_back((key, now, entry));
        while self.size > self.max_bytes {
            match self.entries.pop_front() {
                Some((_, _, old)) => self.size -= old.data.len(),
                None => break,
            }
        }
    }
}

/// The two caches of a machine context, as Python keeps them.
#[derive(Debug, Default)]
pub struct Reads {
    pub attachments: Cache,
    pub files: Cache,
}

/// `type(x) is int and lower <= x <= upper`, as a number.
fn integer(value: Option<&Value>, lower: usize, upper: usize) -> Option<usize> {
    let digits = int_literal(value?)?;
    if !int_within(&digits, &lower.to_string(), &upper.to_string()) {
        return None;
    }
    digits.parse().ok()
}

/// A validated range: `0 <= offset`, `1 <= length <= 512 KiB` and the range
/// within 10 MiB.
fn range(payload: &Map<String, Value>) -> Option<(usize, usize)> {
    let offset = integer(payload.get("offset"), 0, MAX_FILE_BYTES - 1)?;
    let length = integer(payload.get("length"), 1, MAX_READ_BYTES)?;
    (offset + length <= MAX_FILE_BYTES).then_some((offset, length))
}

fn has_exactly(
    payload: &Map<String, Value>,
    required: &[&str],
    optional: &[&str],
    request_id: &str,
) -> bool {
    required.iter().all(|field| payload.contains_key(*field))
        && payload
            .keys()
            .all(|key| required.contains(&key.as_str()) || optional.contains(&key.as_str()))
        && is_str(payload.get("request_id"), request_id)
}

/// `attachments.validate_read(payload, request_id)`: a receipt, a position
/// and one bounded range, never a path.
pub fn validate_attachment(payload: &Value, request_id: &str) -> PyResult<()> {
    let Some(object) = payload.as_object().filter(|payload| {
        has_exactly(
            payload,
            &READ_FIELDS,
            &["archive_id", "agent_id"],
            request_id,
        )
    }) else {
        return Err(error("invalid attachment read fields"));
    };
    bounded(object.get("expected_run_id"), 128, false)?;
    bounded(object.get("expected_conversation_id"), 256, false)?;
    let message = bounded(object.get("message_id"), 36, false)?;
    if !canonical_uuid(&message) {
        return Err(error("invalid attachment message identity"));
    }
    if integer(object.get("index"), 0, MAX_COUNT - 1).is_none() || range(object).is_none() {
        return Err(error("invalid attachment position or range"));
    }
    if let Some(agent) = object.get("agent_id") {
        if bounded(Some(agent), 160, false)?.contains(['/', '\\']) {
            return Err(error("invalid child identity"));
        }
    }
    if let Some(archive) = object.get("archive_id") {
        let archive = bounded(Some(archive), 36, false)?;
        if !canonical_uuid(&archive) || archive == NIL {
            return Err(error("invalid archive identity"));
        }
    }
    Ok(())
}

/// No C0 control, DEL or C1 control (`127 <= ord(c) <= 159`).
fn printable(text: &str) -> bool {
    !text
        .chars()
        .any(|c| (c as u32) < 32 || (127..=159).contains(&(c as u32)))
}

fn lower_hex(text: &str, length: usize) -> bool {
    text.len() == length
        && text
            .bytes()
            .all(|b| b.is_ascii_digit() || (b'a'..=b'f').contains(&b))
}

/// `file_reads.validate(payload, request_id)`. A later range must name the
/// digest of the first, so it never silently reads a replaced file.
pub fn validate_file(payload: &Value, request_id: &str) -> PyResult<()> {
    let Some(object) = payload.as_object().filter(|payload| {
        has_exactly(
            payload,
            &FILE_FIELDS,
            &["sha256", "archive_id", "agent_id"],
            request_id,
        )
    }) else {
        return Err(error("invalid file read fields"));
    };
    for (key, maximum) in [
        ("expected_run_id", 128),
        ("expected_conversation_id", 256),
        ("path", 8192),
        ("agent_id", 160),
    ] {
        let Some(value) = object.get(key) else {
            continue;
        };
        let valid = value
            .as_str()
            .is_some_and(|text| !text.is_empty() && text.len() <= maximum && printable(text));
        if !valid {
            return Err(error("invalid file read identity or path"));
        }
    }
    let agent = object.get("agent_id").and_then(Value::as_str);
    if agent.is_some_and(|agent| agent.contains(['/', '\\'])) {
        return Err(error("invalid child identity"));
    }
    if let Some(archive) = object.get("archive_id") {
        if !archive
            .as_str()
            .is_some_and(|archive| canonical_uuid(archive) && archive != NIL)
        {
            return Err(error("invalid archive identity"));
        }
    }
    if let Some(digest) = object.get("sha256") {
        if !digest.as_str().is_some_and(|digest| lower_hex(digest, 64)) {
            return Err(error("invalid file digest"));
        }
    }
    let Some((offset, _)) = range(object) else {
        return Err(error("invalid file range"));
    };
    if offset != 0 && !object.contains_key("sha256") {
        return Err(error("later file ranges require the original digest"));
    }
    Ok(())
}

fn text<'a>(payload: &'a Value, key: &str) -> &'a str {
    payload.get(key).and_then(Value::as_str).unwrap_or_default()
}

fn digits(payload: &Value, key: &str) -> String {
    payload.get(key).and_then(int_literal).unwrap_or_default()
}

/// The fresh identity check before the first range of a receipt file:
/// the exact run, conversation and archive, and for a child its parent.
pub fn check_identity(detail: &Value, session: &str, payload: &Value) -> Result<(), String> {
    let empty = json!("");
    let archived = payload.get("archive_id").is_some();
    let same =
        |value: Option<&Value>, key: &str| py_eq(value.unwrap_or(&Value::Null), &payload[key]);
    let matches = detail.as_object().is_some_and(|detail| {
        is_str(detail.get("name"), session)
            && same(detail.get("run_id"), "expected_run_id")
            && same(detail.get("conversation_id"), "expected_conversation_id")
            && py_eq(
                detail.get("archive_id").unwrap_or(&empty),
                payload.get("archive_id").unwrap_or(&empty),
            )
            && (archived
                || !(is_str(detail.get("state"), "archived")
                    || detail.get("archive_id").is_some_and(truthy)))
    });
    if !matches {
        return Err("native attachment conversation identity did not match".into());
    }
    if let Some(agent) = payload.get("agent_id").and_then(Value::as_str) {
        let child = detail["subagents"]
            .as_object()
            .is_some_and(|subagents| subagents.contains_key(agent));
        if !child {
            return Err("child does not belong to the exact selected parent".into());
        }
    }
    Ok(())
}

/// `hgs attachment <session> --request ID --index N --conversation ID`,
/// with the archive and child when the phone names them.
pub fn attachment_argv(session: &str, payload: &Value) -> Vec<String> {
    let mut argv: Vec<String> = [
        "attachment",
        session,
        "--request",
        text(payload, "message_id"),
        "--index",
        &digits(payload, "index"),
        "--conversation",
        text(payload, "expected_conversation_id"),
    ]
    .map(String::from)
    .into();
    for (field, option) in [("archive_id", "--archive"), ("agent_id", "--agent")] {
        if payload.get(field).is_some() {
            argv.extend([option.to_owned(), text(payload, field).to_owned()]);
        }
    }
    argv
}

/// The cache identity of a receipt file.
pub fn attachment_key(session: &str, payload: &Value) -> Key {
    [
        session,
        text(payload, "archive_id"),
        text(payload, "agent_id"),
        text(payload, "expected_run_id"),
        text(payload, "expected_conversation_id"),
        text(payload, "message_id"),
        &digits(payload, "index"),
    ]
    .map(String::from)
    .into()
}

/// The cache identity of a file snapshot, by the digest it was read with.
pub fn file_key(session: &str, payload: &Value, sha256: &str) -> Key {
    [
        "file",
        session,
        text(payload, "expected_run_id"),
        text(payload, "expected_conversation_id"),
        text(payload, "archive_id"),
        text(payload, "agent_id"),
        text(payload, "path"),
        sha256,
    ]
    .map(String::from)
    .into()
}

/// `file_reads.native_payload(payload)`: everything but the range.
pub fn file_native_payload(payload: &Value) -> Value {
    let mut native = payload.clone();
    if let Some(native) = native.as_object_mut() {
        native.remove("offset");
        native.remove("length");
    }
    native
}

/// `attachment_reads.decode(raw, payload)`: the native answer names the
/// requested receipt file, and its bytes are what it declares.
pub fn decode_attachment(raw: &Value, payload: &Value) -> Result<Entry, String> {
    let Some(raw) = raw.as_object().filter(|raw| {
        py_eq(
            raw.get("request_id").unwrap_or(&Value::Null),
            &payload["message_id"],
        ) && raw.get("index").and_then(int_literal) == payload.get("index").and_then(int_literal)
            && raw.get("index").and_then(int_literal).is_some()
    }) else {
        return Err("native attachment identity did not match".into());
    };
    let name = match raw.get("name") {
        Some(Value::String(name)) if !name.is_empty() && py_len(name) <= 1024 => name,
        _ => return Err("native attachment name is invalid".into()),
    };
    let mime = match raw.get("mime") {
        Some(Value::String(mime)) if py_len(mime) <= 255 => mime,
        _ => return Err("native attachment type is invalid".into()),
    };
    let data = match raw.get("data_base64") {
        Some(Value::String(data)) if !data.is_empty() && py_len(data) <= MAX_ENCODED => data,
        _ => return Err("attachment exceeds 10 MiB".into()),
    };
    let decoded =
        pybase64::decode(data).map_err(|_| "native attachment data is invalid".to_owned())?;
    if decoded.is_empty() || decoded.len() > MAX_FILE_BYTES {
        return Err("attachment must be nonempty and at most 10 MiB".into());
    }
    if let Some(declared) = raw.get("bytes").and_then(int_literal) {
        if declared != decoded.len().to_string() {
            return Err("attachment size changed since it was sent".into());
        }
    }
    Ok(Entry {
        path: String::new(),
        name: name.clone(),
        mime: if mime.is_empty() {
            "application/octet-stream".into()
        } else {
            mime.clone()
        },
        sha256: zerus_relay::json::digest(&decoded),
        data: decoded,
    })
}

/// `[a-z0-9!#$&^_.+\-]+/[a-z0-9!#$&^_.+\-]+`.
fn media_type(mime: &str) -> bool {
    let token = |part: &str| {
        !part.is_empty()
            && part
                .bytes()
                .all(|b| b.is_ascii_lowercase() || b.is_ascii_digit() || b"!#$&^_.+-".contains(&b))
    };
    let mut parts = mime.split('/');
    matches!((parts.next(), parts.next(), parts.next()), (Some(kind), Some(subtype), None) if token(kind) && token(subtype))
}

/// `file_reads.decode(raw, session, payload)`: the native snapshot of the
/// requested path for this exact conversation, whose bytes match its size
/// and digest, and the digest of the first range when a later one asks.
pub fn decode_file(raw: &Value, session: &str, payload: &Value) -> Result<Entry, String> {
    let empty = json!("");
    let expected = [
        ("request_id", &payload["request_id"]),
        ("name", &json!(session)),
        ("run_id", &payload["expected_run_id"]),
        ("conversation_id", &payload["expected_conversation_id"]),
        ("archive_id", payload.get("archive_id").unwrap_or(&empty)),
        ("agent_id", payload.get("agent_id").unwrap_or(&empty)),
        ("requested_path", &payload["path"]),
    ];
    let Some(raw) = raw.as_object().filter(|raw| {
        expected
            .iter()
            .all(|(key, value)| py_eq(raw.get(*key).unwrap_or(&empty), value))
    }) else {
        return Err("native file identity did not match".into());
    };
    let string = |key: &str| raw.get(key).and_then(Value::as_str);
    let path = string("path").filter(|path| {
        path.starts_with('/') && !path.starts_with("//") && path.len() <= 8192 && printable(path)
    });
    let name = string("file_name").filter(|name| {
        !name.is_empty()
            && name.len() <= 255
            && path.is_some_and(|path| path.rsplit('/').next() == Some(*name))
            && !name.contains('\\')
            && printable(name)
    });
    let mime = string("mime").filter(|mime| media_type(mime) && py_len(mime) <= 100);
    let size = integer(raw.get("bytes"), 1, MAX_FILE_BYTES);
    let encoded = string("data_base64").filter(|encoded| py_len(encoded) <= MAX_ENCODED);
    let sha256 = string("sha256").filter(|sha256| lower_hex(sha256, 64));
    let (Some(path), Some(name), Some(mime), Some(size), Some(encoded), Some(sha256)) =
        (path, name, mime, size, encoded, sha256)
    else {
        return Err("native file details are invalid".into());
    };
    let data = pybase64::decode(encoded).map_err(|_| "native file data is invalid".to_owned())?;
    let original = payload
        .get("sha256")
        .is_none_or(|original| is_str(Some(original), sha256));
    if data.len() != size || zerus_relay::json::digest(&data) != sha256 || !original {
        return Err(FILE_CHANGED.into());
    }
    Ok(Entry {
        path: path.into(),
        name: name.into(),
        mime: mime.into(),
        sha256: sha256.into(),
        data,
    })
}

/// The validated range of a payload.
fn validated_range(payload: &Value) -> (usize, usize) {
    payload
        .as_object()
        .and_then(range)
        .expect("validated range")
}

/// The fields every range result shares, then the range itself.
fn range_result(
    mut result: Map<String, Value>,
    entry: &Entry,
    payload: &Value,
    offset: usize,
    length: usize,
) -> Value {
    let end = (offset + length).min(entry.data.len());
    let data = &entry.data[offset..end];
    result.insert("file_name".into(), json!(entry.name));
    result.insert("mime".into(), json!(entry.mime));
    result.insert("bytes".into(), json!(entry.data.len()));
    result.insert("sha256".into(), json!(entry.sha256));
    result.insert("offset".into(), json!(offset));
    result.insert("length".into(), json!(data.len()));
    result.insert("data_base64".into(), json!(pybase64::encode(data)));
    for field in ["archive_id", "agent_id"] {
        if let Some(value) = payload.get(field) {
            result.insert(field.into(), value.clone());
        }
    }
    Value::Object(result)
}

fn identity(request_id: &str, session: &str, payload: &Value) -> Map<String, Value> {
    let mut result = Map::new();
    result.insert("request_id".into(), json!(request_id));
    result.insert("name".into(), json!(session));
    result.insert("run_id".into(), payload["expected_run_id"].clone());
    result.insert(
        "conversation_id".into(),
        payload["expected_conversation_id"].clone(),
    );
    result
}

/// `attachment_reads.chunk(entry, session, payload, request_id)`.
pub fn attachment_chunk(
    entry: &Entry,
    session: &str,
    payload: &Value,
    request_id: &str,
) -> Result<Value, String> {
    let (offset, length) = validated_range(payload);
    if offset >= entry.data.len() {
        return Err("attachment range is outside the file".into());
    }
    let index = integer(payload.get("index"), 0, MAX_COUNT - 1).expect("validated index");
    let mut result = identity(request_id, session, payload);
    result.insert("message_id".into(), payload["message_id"].clone());
    result.insert("index".into(), json!(index));
    Ok(range_result(result, entry, payload, offset, length))
}

/// `file_reads.chunk(entry, session, payload, request_id)`.
pub fn file_chunk(
    entry: &Entry,
    session: &str,
    payload: &Value,
    request_id: &str,
) -> Result<Value, String> {
    let (offset, length) = validated_range(payload);
    let original = payload
        .get("sha256")
        .is_none_or(|original| is_str(Some(original), &entry.sha256));
    if offset >= entry.data.len() || !original {
        return Err(FILE_CHANGED.into());
    }
    let mut result = identity(request_id, session, payload);
    result.insert("requested_path".into(), payload["path"].clone());
    result.insert("path".into(), json!(entry.path));
    Ok(range_result(result, entry, payload, offset, length))
}

#[cfg(test)]
mod tests {
    use super::*;

    fn entry(byte: u8, size: usize) -> Arc<Entry> {
        Arc::new(Entry {
            path: String::new(),
            name: "file".into(),
            mime: "text/plain".into(),
            sha256: String::new(),
            data: vec![byte; size],
        })
    }

    fn key(name: &str) -> Key {
        vec![name.to_owned()]
    }

    #[test]
    fn the_cache_keeps_the_most_recently_used_within_its_byte_budget() {
        let mut cache = Cache::default();
        for (index, name) in ["A", "B", "C", "D"].iter().enumerate() {
            assert!(cache.get(&key(name), 1.0).is_none());
            cache.put(key(name), entry(index as u8, MAX_FILE_BYTES), 1.0);
        }
        // D pushed out A, the least recently used.
        assert_eq!(cache.size(), CACHE_BYTES);
        assert!(cache.get(&key("A"), 2.0).is_none());
        assert!(cache.get(&key("C"), 2.0).is_some());
        cache.put(key("A"), entry(0, MAX_FILE_BYTES), 2.0);
        // C was used after B and D, so B went next.
        assert!(cache.get(&key("B"), 3.0).is_none());
        for name in ["C", "D", "A"] {
            assert!(cache.get(&key(name), 3.0).is_some(), "{name}");
        }
        // The budget counts bytes: many small files stay together.
        let mut small = Cache::default();
        for index in 0..1000 {
            small.put(key(&index.to_string()), entry(1, 7), 1.0);
        }
        assert!((0..1000).all(|index| small.get(&key(&index.to_string()), 2.0).is_some()));
        assert_eq!(small.size(), 7000);
        // A file beyond the whole budget is never kept, nor replaces another.
        small.put(key("0"), entry(2, CACHE_BYTES + 1), 2.0);
        assert!(small.get(&key("0"), 2.0).is_none());
        assert_eq!(small.size(), 6993);
    }

    #[test]
    fn cached_files_expire_two_minutes_after_their_read() {
        let mut cache = Cache::default();
        cache.put(key("A"), entry(1, 3), 10.0);
        cache.put(key("B"), entry(2, 5), 70.0);
        // Using an entry does not extend its life.
        assert!(cache.get(&key("A"), 129.9).is_some());
        assert!(cache.get(&key("A"), 130.0).is_none());
        assert_eq!(cache.size(), 5);
        assert!(cache.get(&key("B"), 189.0).is_some());
        assert!(cache.get(&key("B"), 190.0).is_none());
        assert_eq!(cache.size(), 0);
        // Reading again replaces an entry and restarts its clock.
        cache.put(key("A"), entry(1, 3), 200.0);
        cache.put(key("A"), entry(3, 4), 300.0);
        assert_eq!(cache.get(&key("A"), 419.0).unwrap().data, vec![3; 4]);
        assert_eq!(cache.size(), 4);
    }

    #[test]
    fn media_types_follow_the_python_pattern() {
        for mime in ["application/octet-stream", "image/svg+xml", "a!#$&^_.+-/0"] {
            assert!(media_type(mime), "{mime}");
        }
        for mime in [
            "",
            "text",
            "text/",
            "/plain",
            "Text/plain",
            "a/b/c",
            "text/plain; x=1",
            "t\u{e9}xt/plain",
        ] {
            assert!(!media_type(mime), "{mime}");
        }
    }
}
