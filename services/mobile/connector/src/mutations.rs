//! Session mutations and context operations, as `_execute_claimed`
//! (`connector.py:863-1235`) runs them: send (text and inline attachments,
//! a DeepSeek child send, a compaction continuation), answer, interrupt,
//! send_now, settings, the context commands, process_stop, the session
//! actions, recovery_action and set_mobile_delivery, with the validators of
//! `context.py`, `recovery.py` and `attachments.validate_send` and Python's
//! phone-visible texts.
//!
//! Each operation reads fresh native state where Python does, checks its
//! capability, and only then starts its single native write. From that
//! point any failure leaves the receipt uncertain; the journal row was
//! claimed `running` before any native call, so the same identity never
//! runs again. A write is never started once the connector is stopping
//! (divergence h), which leaves that receipt failed. Python's checks are
//! reproduced with its value semantics: `dict.get` yields None for a
//! missing key, `==` follows Python's numbers, a missing payload key is a
//! KeyError and an unhashable status a TypeError, both of which become the
//! generic receipt.
use crate::capabilities::LIFECYCLE;
use crate::execute::{Failure, GENERIC};
use crate::native::{Call, Native};
use crate::projects::is_archived;
use crate::pyvalue::{
    bounded, canonical_uuid, is_str, is_true, py_eq, py_isspace, py_len, py_strip, py_uuid, token,
    truthy, PyError,
};
use crate::runtime::Connector;
use crate::validate::{checked_uuid, Validated};
use serde_json::{json, Map, Value};
use std::cell::Cell;
use zerus_relay::protocol::opencode_effort_id;

/// The operations of this module. Every one but `answer`, `interrupt` and
/// `send` needs a native capability or a fresh native preflight.
pub const OPERATIONS: &[&str] = &[
    "answer",
    "archive",
    "clear_context",
    "compact_context",
    "forget",
    "fork",
    "interrupt",
    "pause",
    "process_stop",
    "recovery_action",
    "remove_queued",
    "rename",
    "restore",
    "resume",
    "send",
    "send_now",
    "set_mobile_delivery",
    "settings",
    "terminate",
];

pub fn handles(operation: &str) -> bool {
    OPERATIONS.contains(&operation)
}

const IDENTITY_FIELDS: [&str; 3] = ["request_id", "expected_run_id", "expected_conversation_id"];
/// `context.MOBILE_DELIVERY`.
pub const MOBILE_DELIVERY: [&str; 6] = [
    "immediate",
    "away",
    "away_5",
    "away_10",
    "away_15",
    "away_30",
];
const EFFORTS: [&str; 11] = [
    "", "off", "none", "minimal", "low", "medium", "high", "xhigh", "max", "ultra", "on",
];

/// `attachments.py` limits.
pub const MAX_COUNT: usize = 8;
pub const MAX_FILE_BYTES: usize = 10 * 1024 * 1024;
pub const MAX_TOTAL_BYTES: usize = 20 * 1024 * 1024;
pub const MAX_TEXT_BYTES: usize = 64 * 1024;
/// Base64 is checked in aligned chunks of this many characters, as Python
/// decodes it, so no decoded copy of a file is ever held.
const BASE64_CHUNK: usize = 65536;

pub const INVALID_SEND: &str = "invalid or oversized inline attachment/message";
pub const DELIVERY_UNAVAILABLE: &str =
    "the shared delivery setting is changed on the gateway computer with a current hgs";
pub const EFFORT_NOT_OFFERED: &str = "effort is not offered for this model";
const CONTEXT_IDENTITY: &str = "fresh native context identity did not match";
const NOT_COMPACTED: &str =
    "compaction is not confirmed for this conversation; message was not sent";
const NOT_A_CHILD: &str = "child does not belong to the exact selected parent";
const CHILD_REFUSED: &str = "native child cannot accept messages under this exact parent";
const JOB_CHANGED: &str = "native recovery job changed; refresh before action";
const LIFECYCLE_MISMATCH: &str =
    "native lifecycle acknowledgement identity or status did not match";
const CONTEXT_MISMATCH: &str = "native context acknowledgement identity or status did not match";

static NULL: Value = Value::Null;
static EMPTY: Value = Value::String(String::new());

/// `obj.get(key)`: None (JSON null) for a missing key.
fn get<'a>(object: &'a Map<String, Value>, key: &str) -> &'a Value {
    object.get(key).unwrap_or(&NULL)
}

/// `payload[key]`: a missing key raises KeyError, the generic receipt.
fn key<'a>(payload: &'a Value, key: &str) -> Result<&'a Value, Failure> {
    payload.get(key).ok_or(Failure::Generic)
}

/// `value in {...}` for a set of strings: an unhashable value raises
/// TypeError, the generic receipt.
fn in_set(value: &Value, set: &[&str]) -> Result<bool, Failure> {
    match value {
        Value::Array(_) | Value::Object(_) => Err(Failure::Generic),
        Value::String(text) => Ok(set.contains(&text.as_str())),
        _ => Ok(false),
    }
}

fn text(message: &str) -> String {
    message.into()
}

fn value_error(error: PyError) -> String {
    match error {
        PyError::Value(message) => message,
        PyError::Type => GENERIC.into(),
    }
}

/// Exactly `IDENTITY_FIELDS` plus any of `extras` (`required` names more
/// fields that must be present), with the payload's request_id.
fn scoped(
    payload: &Value,
    request_id: &str,
    required: &[&str],
    extras: &[&str],
    exact: bool,
) -> bool {
    payload.as_object().is_some_and(|payload| {
        let allowed = |key: &str| IDENTITY_FIELDS.contains(&key) || extras.contains(&key);
        IDENTITY_FIELDS
            .iter()
            .chain(required)
            .all(|field| payload.contains_key(*field))
            && payload.keys().all(|key| allowed(key))
            && (!exact || payload.len() == IDENTITY_FIELDS.len() + required.len())
            && payload.get("request_id").and_then(Value::as_str) == Some(request_id)
    })
}

/// `str(uuid.UUID(value)) != value` (and, with `nonzero`, the nil UUID),
/// with uuid's own ValueError text for a value it cannot read.
fn uuid_identity(value: &str, nonzero: bool, message: &str) -> Result<(), String> {
    let parsed = py_uuid(value).map_err(value_error)?;
    if !canonical_uuid(value) || (nonzero && parsed == 0) {
        return Err(text(message));
    }
    Ok(())
}

/// `context.validate_mobile_delivery(payload)`.
pub fn validate_delivery(payload: Option<&Value>) -> Result<(), String> {
    let valid = payload.and_then(Value::as_object).is_some_and(|payload| {
        payload.len() == 1
            && payload
                .get("value")
                .and_then(Value::as_str)
                .is_some_and(|value| MOBILE_DELIVERY.contains(&value))
    });
    if valid {
        Ok(())
    } else {
        Err(text("invalid phone notification timing"))
    }
}

/// The operation's own validator on the scoped session path of
/// `Connector.validate`, after the shared identity checks. Answer and
/// interrupt have none: native hgs checks their fields (the relay is
/// stricter).
pub fn validate(operation: &str, payload: &Value, request_id: &str) -> Result<(), String> {
    match operation {
        "recovery_action" => validate_recovery(payload, request_id),
        "compact_context" => validate_context(payload, request_id),
        "clear_context" => validate_clear_context(payload, request_id),
        "send_now" | "settings" | "process_stop" => validate_core(operation, payload, request_id),
        "send" => validate_send(payload),
        "remove_queued" => validate_remove_queued(payload, request_id),
        _ if LIFECYCLE.contains(&operation) => validate_lifecycle(operation, payload, request_id),
        _ => Ok(()),
    }
}

/// `settings` for an OpenCode session: the model's own `effort_options` decide
/// the effort, so the fixed list does not apply. Any other session keeps the
/// fixed list.
pub fn validate_for_session(
    operation: &str,
    session: &str,
    payload: &Value,
    request_id: &str,
) -> Result<(), String> {
    if operation == "settings" && session.starts_with("opencode/") {
        return validate_core_with(operation, payload, request_id, true);
    }
    validate(operation, payload, request_id)
}

/// An OpenCode effort must be one of the target model's `effort_options` in
/// the session's own model catalog. An empty effort selects the default.
pub fn opencode_effort_offered(
    detail: &Map<String, Value>,
    payload: &Value,
) -> Result<(), Failure> {
    let Some(effort) = payload
        .get("effort")
        .and_then(Value::as_str)
        .filter(|effort| !effort.is_empty())
    else {
        return Ok(());
    };
    let model = payload
        .get("model")
        .or_else(|| detail.get("settings_model"))
        .and_then(Value::as_str)
        .unwrap_or_default();
    let offered = !model.is_empty()
        && detail
            .get("model_options")
            .and_then(Value::as_array)
            .into_iter()
            .flatten()
            .filter(|option| {
                ["id", "value"]
                    .iter()
                    .any(|key| option.get(*key).and_then(Value::as_str) == Some(model))
            })
            .filter_map(|option| option.get("effort_options").and_then(Value::as_array))
            .any(|efforts| {
                efforts
                    .iter()
                    .any(|offered| offered.as_str() == Some(effort))
            });
    if offered {
        Ok(())
    } else {
        Err(Failure::text(EFFORT_NOT_OFFERED))
    }
}

/// `remove_queued`: scoped identity plus one exact `msg_` message identity,
/// `msg_` and 1-124 ASCII letters, digits, `_` or `-` as hgs and the relay
/// check it (`tests/fixtures/opencode-queued-message-ids.json`).
pub fn validate_remove_queued(payload: &Value, request_id: &str) -> Result<(), String> {
    if !scoped(payload, request_id, &["message_id"], &["message_id"], true) {
        return Err(text("invalid scoped native operation fields"));
    }
    bounded(payload.get("expected_run_id"), 128, false).map_err(value_error)?;
    bounded(payload.get("expected_conversation_id"), 256, false).map_err(value_error)?;
    let valid = payload
        .get("message_id")
        .and_then(Value::as_str)
        .is_some_and(|id| {
            id.len() <= 128
                && id.len() > "msg_".len()
                && id.starts_with("msg_")
                && id
                    .bytes()
                    .all(|b| b.is_ascii_alphanumeric() || b == b'_' || b == b'-')
        });
    if valid {
        Ok(())
    } else {
        Err(text("invalid native message identity"))
    }
}

/// `recovery.validate_recovery(payload, request_id)`.
pub fn validate_recovery(payload: &Value, request_id: &str) -> Result<(), String> {
    if !scoped(
        payload,
        request_id,
        &["job_id", "action"],
        &["job_id", "action"],
        true,
    ) {
        return Err(text("invalid scoped recovery fields"));
    }
    bounded(payload.get("expected_run_id"), 128, false).map_err(value_error)?;
    bounded(payload.get("expected_conversation_id"), 256, false).map_err(value_error)?;
    let job = bounded(payload.get("job_id"), 36, false).map_err(value_error)?;
    uuid_identity(&job, true, "invalid recovery job UUID")?;
    if !in_set(&payload["action"], &["now", "cancel"]).map_err(|_| text(GENERIC))? {
        return Err(text("invalid recovery job action"));
    }
    Ok(())
}

/// Only the optional clear field is new; the ordinary identity validator stays unchanged.
pub fn validate_clear_context(payload: &Value, request_id: &str) -> Result<(), String> {
    if let Some(value) = payload.get("dismiss_prompt") {
        if !value.is_boolean() {
            return Err(text("dismiss_prompt must be a boolean"));
        }
        let mut ordinary = payload.clone();
        ordinary
            .as_object_mut()
            .expect("field exists")
            .remove("dismiss_prompt");
        validate_context(&ordinary, request_id)
    } else {
        validate_context(payload, request_id)
    }
}

/// `context.validate_context(payload, request_id)`.
pub fn validate_context(payload: &Value, request_id: &str) -> Result<(), String> {
    if !scoped(payload, request_id, &[], &[], true) {
        return Err(text(
            "context control requires exact request/run/conversation identity",
        ));
    }
    for (field, maximum) in [("expected_run_id", 128), ("expected_conversation_id", 256)] {
        let valid = payload[field].as_str().is_some_and(|value| {
            !value.is_empty()
                && py_len(value) <= maximum
                && !value.chars().any(|c| (c as u32) < 32 || c as u32 == 127)
        });
        if !valid {
            return Err(text(
                "context control requires nonempty bounded native identity",
            ));
        }
    }
    Ok(())
}

/// `context.validate_core` for send_now, settings and process_stop.
pub fn validate_core(operation: &str, payload: &Value, request_id: &str) -> Result<(), String> {
    validate_core_with(operation, payload, request_id, false)
}

/// An OpenCode effort is empty (the default) or a native effort id, checked
/// against the model's offered `effort_options` before the write.
fn opencode_effort(effort: &str) -> bool {
    effort.is_empty() || opencode_effort_id(effort)
}

fn validate_core_with(
    operation: &str,
    payload: &Value,
    request_id: &str,
    opencode: bool,
) -> Result<(), String> {
    let extras: &[&str] = match operation {
        "send_now" => &["queue_id"],
        "settings" => &["model", "effort", "expected_pending_id"],
        _ => &["process_id", "generation"],
    };
    if !scoped(payload, request_id, &[], extras, false) {
        return Err(text("invalid scoped native operation fields"));
    }
    bounded(payload.get("expected_run_id"), 128, false).map_err(value_error)?;
    bounded(
        payload.get("expected_conversation_id"),
        256,
        operation != "send_now",
    )
    .map_err(value_error)?;
    match operation {
        "send_now" => {
            let valid = payload
                .get("queue_id")
                .and_then(Value::as_str)
                .is_some_and(|queue| {
                    queue.len() == 64
                        && queue
                            .bytes()
                            .all(|b| b.is_ascii_digit() || (b'a'..=b'f').contains(&b))
                });
            if !valid {
                return Err(text("invalid native queue identity"));
            }
        }
        "settings" => {
            if payload.get("model").is_none() && payload.get("effort").is_none() {
                return Err(text("select a model or effort"));
            }
            if let Some(model) = payload.get("model") {
                let model = bounded(Some(model), 256, false).map_err(value_error)?;
                if model.len() > 256 {
                    return Err(text("native model identifier exceeds UTF-8 byte limit"));
                }
            }
            if let Some(effort) = payload.get("effort") {
                let allowed = |effort: &str| {
                    if opencode {
                        opencode_effort(effort)
                    } else {
                        EFFORTS.contains(&effort)
                    }
                };
                if !effort.as_str().is_some_and(allowed) {
                    return Err(text("invalid native effort"));
                }
            }
            if let Some(pending) = payload.get("expected_pending_id") {
                let pending = bounded(Some(pending), 36, false).map_err(value_error)?;
                uuid_identity(&pending, false, "invalid pending setting identity")?;
            }
        }
        _ => {
            token(payload.get("process_id"), 160).map_err(value_error)?;
            if let Some(generation) = payload.get("generation") {
                token(Some(generation), 256).map_err(value_error)?;
            }
        }
    }
    Ok(())
}

/// `context.validate_lifecycle(operation, payload, request_id)`.
pub fn validate_lifecycle(
    operation: &str,
    payload: &Value,
    request_id: &str,
) -> Result<(), String> {
    let mut extras = match operation {
        "rename" => vec!["new_name"],
        "fork" => vec!["tag"],
        _ => vec![],
    };
    if matches!(operation, "restore" | "forget" | "rename" | "fork") {
        extras.push("archive_id");
    }
    if !scoped(payload, request_id, &[], &extras, false) {
        return Err(text("invalid scoped lifecycle fields"));
    }
    bounded(payload.get("expected_run_id"), 128, false).map_err(value_error)?;
    bounded(payload.get("expected_conversation_id"), 256, true).map_err(value_error)?;
    if operation == "restore" && payload.get("archive_id").is_none() {
        return Err(text("restore requires an immutable archive identity"));
    }
    if let Some(archive) = payload.get("archive_id") {
        let archive = bounded(Some(archive), 36, false).map_err(value_error)?;
        uuid_identity(&archive, true, "invalid archive identity")?;
    }
    if operation == "rename" {
        let name = bounded(payload.get("new_name"), 512, false).map_err(value_error)?;
        if name.len() > 512 || py_strip(&name) != name {
            return Err(text("invalid native session name"));
        }
    }
    if let Some(tag) = payload.get("tag") {
        let tag = bounded(Some(tag), 120, false).map_err(value_error)?;
        if tag.len() > 120 || py_strip(&tag) != tag || tag.contains(['/', ':', '.']) {
            return Err(text("invalid native fork tag"));
        }
    }
    Ok(())
}

/// `attachments.REFERENCE.fullmatch(value)`: `[Image #N]` or `[File #N]`.
fn reference(value: &str) -> bool {
    let Some(rest) = value
        .strip_prefix("[Image #")
        .or_else(|| value.strip_prefix("[File #"))
    else {
        return false;
    };
    let Some(digits) = rest.strip_suffix(']') else {
        return false;
    };
    (1..=9).contains(&digits.len())
        && digits.bytes().all(|b| b.is_ascii_digit())
        && !digits.starts_with('0')
}

/// The bytes `chunk` decodes to when it is exactly what `b64encode` would
/// write: `b64decode(chunk, validate=True)` succeeds and re-encodes to it.
fn canonical_base64(chunk: &[u8]) -> Option<usize> {
    if !chunk.len().is_multiple_of(4) {
        return None;
    }
    let digit = |b: u8| match b {
        b'A'..=b'Z' => Some(u32::from(b - b'A')),
        b'a'..=b'z' => Some(u32::from(b - b'a') + 26),
        b'0'..=b'9' => Some(u32::from(b - b'0') + 52),
        b'+' => Some(62),
        b'/' => Some(63),
        _ => None,
    };
    let groups = chunk.len() / 4;
    let mut size = 0;
    for (index, group) in chunk.chunks(4).enumerate() {
        let padding = group.iter().rev().take_while(|b| **b == b'=').count();
        if padding > 2 || (padding > 0 && index + 1 != groups) {
            return None;
        }
        let mut bits = 0u32;
        for b in &group[..4 - padding] {
            bits = (bits << 6) | digit(*b)?;
        }
        // Bits the padding leaves unused must be zero to re-encode the same.
        let unused = [0, 0b11, 0b1111][padding];
        if bits & unused != 0 {
            return None;
        }
        size += 3 - padding;
    }
    Some(size)
}

/// The decoded size of one attachment's `data_base64`, checked in aligned
/// chunks as Python checks it, or `None` when it is not canonical base64.
fn decoded_size(data: &str) -> Option<usize> {
    // `b64decode` of a str accepts only ASCII.
    if !data.is_ascii() || data.len() > MAX_FILE_BYTES.div_ceil(3) * 4 {
        return None;
    }
    let bytes = data.as_bytes();
    let mut size = 0;
    for (index, chunk) in bytes.chunks(BASE64_CHUNK).enumerate() {
        let last = (index + 1) * BASE64_CHUNK >= bytes.len();
        if !last && chunk.contains(&b'=') {
            return None;
        }
        size += canonical_base64(chunk)?;
    }
    Some(size)
}

/// `attachments.validate_send(payload)`: any ValueError or TypeError there
/// becomes one fixed receipt text.
pub fn validate_send(payload: &Value) -> Result<(), String> {
    checked_send(payload).ok_or_else(|| text(INVALID_SEND))
}

/// Whether `validate_send` accepts the payload.
fn checked_send(payload: &Value) -> Option<()> {
    const FIELDS: [&str; 7] = [
        "request_id",
        "text",
        "expected_run_id",
        "expected_conversation_id",
        "attachments",
        "expected_compaction_id",
        "agent_id",
    ];
    let payload = payload.as_object()?;
    if !payload.keys().all(|key| FIELDS.contains(&key.as_str())) {
        return None;
    }
    // `validate_compaction_id`: the native ABI defaults to "".
    let compaction = match payload.get("expected_compaction_id") {
        None => "",
        Some(Value::String(value)) => value,
        Some(_) => return None,
    };
    if !compaction.is_empty() && !canonical_uuid(compaction) {
        return None;
    }
    if let Some(agent) = payload.get("agent_id") {
        let agent = bounded(Some(agent), 160, false).ok()?;
        bounded(payload.get("expected_conversation_id"), 256, false).ok()?;
        if agent.contains(['/', '\\']) || !compaction.is_empty() {
            return None;
        }
    }
    let text = payload.get("text").and_then(Value::as_str)?;
    if text.len() > MAX_TEXT_BYTES
        || text
            .chars()
            .any(|c| c.is_control() && c != '\n' && c != '\t')
        || text.trim_start_matches(py_isspace).starts_with(['/', '!'])
    {
        return None;
    }
    let files = match payload.get("attachments") {
        None => &[][..],
        Some(Value::Array(files)) if files.len() <= MAX_COUNT => files.as_slice(),
        Some(_) => return None,
    };
    if py_strip(text).is_empty() && files.is_empty() {
        return None;
    }
    let control = |value: &str| value.chars().any(char::is_control);
    let mut total = 0;
    let mut references: Vec<&str> = Vec::new();
    for file in files {
        let file = file.as_object()?;
        let known = ["name", "mime", "data_base64", "reference"];
        if !["name", "mime", "data_base64"]
            .iter()
            .all(|field| file.contains_key(*field))
            || !file.keys().all(|key| known.contains(&key.as_str()))
        {
            return None;
        }
        let name = file["name"].as_str()?;
        if name.is_empty()
            || name == "."
            || name == ".."
            || name.len() > 255
            || control(name)
            || name.contains(['/', '\\'])
        {
            return None;
        }
        let mime = file["mime"].as_str()?;
        if mime.is_empty() || mime.len() > 100 || control(mime) {
            return None;
        }
        let label = match file.get("reference") {
            None => "",
            Some(Value::String(label)) => label,
            Some(_) => return None,
        };
        if !label.is_empty() {
            if !reference(label) || references.contains(&label) {
                return None;
            }
            references.push(label);
        }
        let data = file["data_base64"].as_str()?;
        let size = decoded_size(data)?;
        if size == 0 || size > MAX_FILE_BYTES {
            return None;
        }
        total += size;
        if total > MAX_TOTAL_BYTES {
            return None;
        }
    }
    Some(())
}

impl<N: Native + 'static> Connector<N> {
    /// Runs one validated operation of this module. `started` turns true
    /// once its native write may have begun.
    pub(crate) async fn mutate(
        &self,
        validated: &Validated,
        request_id: &str,
        started: &Cell<bool>,
    ) -> Result<Value, Failure> {
        let operation = validated.operation.as_str();
        let session = validated.session.as_str();
        let payload = validated.payload.as_ref().ok_or(Failure::Generic)?;
        let flags = self.flags.get();
        // Fresh native state first, as Python checks it before the gates.
        let continuation =
            operation == "send" && payload.get("expected_compaction_id").is_some_and(truthy);
        if matches!(
            operation,
            "compact_context" | "clear_context" | "settings" | "send_now"
        ) || continuation
        {
            self.context_preflight(operation, session, payload).await?;
        }
        if operation == "send" && payload.get("agent_id").is_some() {
            self.child_send_preflight(session, payload).await?;
        }
        if LIFECYCLE.contains(&operation) && !flags.lifecycle {
            return Err(Failure::text("native scoped lifecycle ABI is unavailable"));
        }
        // Only the gateway's own preference-capable hgs changes the shared
        // setting; a peer context has no fleet.
        if operation == "set_mobile_delivery" && (!flags.preference || self.fleet.is_none()) {
            return Err(Failure::text(DELIVERY_UNAVAILABLE));
        }
        if operation == "recovery_action" {
            if !flags.recovery {
                return Err(Failure::text("native scoped recovery ABI is unavailable"));
            }
            self.recovery_preflight(session, payload).await?;
        }
        match operation {
            "recovery_action" => self.recovery(session, payload, request_id, started).await,
            "set_mobile_delivery" => self.delivery(payload, started).await,
            "process_stop" => {
                self.begin(started)?;
                self.processes("--stop", session, payload, request_id).await
            }
            _ if LIFECYCLE.contains(&operation) => {
                self.lifecycle(operation, session, payload, request_id, started)
                    .await
            }
            _ => {
                self.command(operation, session, payload, request_id, started)
                    .await
            }
        }
    }

    /// The native write may start from here: a failure is uncertain from
    /// now on. Once the connector is stopping no write starts (divergence
    /// h), and the receipt stays failed.
    fn begin(&self, started: &Cell<bool>) -> Result<(), Failure> {
        if self.stopping.is_set() {
            return Err(Failure::Stopped);
        }
        started.set(true);
        Ok(())
    }

    async fn write(&self, call: Call, started: &Cell<bool>) -> Result<Value, Failure> {
        self.begin(started)?;
        Ok(self.native(call).await?.json())
    }

    /// `context_preflight`: fresh native state before a context command,
    /// a settings change, a queued send or a compaction continuation.
    /// Native commands recheck identity atomically themselves; this gate also
    /// makes an old native binary fail closed without typing.
    async fn context_preflight(
        &self,
        operation: &str,
        session: &str,
        payload: &Value,
    ) -> Result<(), Failure> {
        let detail = self.native(Call::new(["inspect", session])).await?.json();
        let Some(detail) = detail.as_object() else {
            return Err(Failure::text(CONTEXT_IDENTITY));
        };
        if is_archived(detail)
            || !is_str(detail.get("name"), session)
            || !py_eq(get(detail, "run_id"), key(payload, "expected_run_id")?)
        {
            return Err(Failure::text(CONTEXT_IDENTITY));
        }
        let conversation = match get(detail, "conversation_id") {
            Value::Null => &EMPTY,
            other => other,
        };
        if !py_eq(conversation, key(payload, "expected_conversation_id")?) {
            return Err(Failure::text(CONTEXT_IDENTITY));
        }
        match operation {
            "clear_context" if is_true(payload.get("dismiss_prompt")) => {
                if !is_true(detail.get("clear_context_dismiss")) {
                    return Err(Failure::text("native context operation is unavailable"));
                }
            }
            "compact_context" | "clear_context" => {
                if !is_true(detail.get(&format!("{operation}_supported"))) {
                    return Err(Failure::text("native context operation is unavailable"));
                }
            }
            "settings" => {
                if !is_true(detail.get("settings_change_supported")) {
                    return Err(Failure::text("native per-session settings are unavailable"));
                }
                if session.starts_with("opencode/") {
                    opencode_effort_offered(detail, payload)?;
                }
                if let Some(expected) = payload.get("expected_pending_id") {
                    if !py_eq(get(detail, "pending_settings_id"), expected) {
                        return Err(Failure::text(
                            "native pending setting changed; refresh before applying",
                        ));
                    }
                }
            }
            "send_now" => {
                let current = detail
                    .get("input_queue")
                    .and_then(Value::as_object)
                    .is_some_and(|queue| {
                        py_eq(get(queue, "id"), &payload["queue_id"])
                            && is_true(queue.get("can_send_now"))
                    });
                if !current {
                    return Err(Failure::text("native queue changed or cannot be sent now"));
                }
            }
            _ => {
                let confirmed = detail
                    .get("compact_context_request")
                    .and_then(Value::as_object)
                    .is_some_and(|compact| {
                        py_eq(
                            get(compact, "request_id"),
                            &payload["expected_compaction_id"],
                        ) && py_eq(get(compact, "run_id"), &payload["expected_run_id"])
                            && py_eq(
                                get(compact, "conversation_id"),
                                &payload["expected_conversation_id"],
                            )
                            && is_str(compact.get("status"), "completed")
                    });
                if !confirmed
                    || !is_str(detail.get("phase"), "idle")
                    || !is_str(detail.get("activity"), "idle")
                {
                    return Err(Failure::text(NOT_COMPACTED));
                }
            }
        }
        Ok(())
    }

    /// `child_send_preflight`: a DeepSeek child of the exact selected
    /// parent that accepts messages.
    async fn child_send_preflight(&self, session: &str, payload: &Value) -> Result<(), Failure> {
        if !session.starts_with("dsh/") {
            return Err(Failure::text("this native provider has no child-send ABI"));
        }
        let run = &payload["expected_run_id"];
        let conversation = &payload["expected_conversation_id"];
        let agent = payload["agent_id"].as_str().ok_or(Failure::Generic)?;
        let parent = self
            .native(Call::new(["inspect", session, "--skip-processes"]))
            .await?
            .json();
        let belongs = parent.as_object().is_some_and(|parent| {
            !is_archived(parent)
                && is_str(parent.get("name"), session)
                && py_eq(get(parent, "run_id"), run)
                && py_eq(get(parent, "conversation_id"), conversation)
                && parent
                    .get("subagents")
                    .and_then(Value::as_object)
                    .is_some_and(|subagents| subagents.contains_key(agent))
        });
        if !belongs {
            return Err(Failure::text(NOT_A_CHILD));
        }
        let child = self
            .native(Call::new([
                "inspect",
                session,
                "--agent",
                agent,
                "--skip-processes",
            ]))
            .await?
            .json();
        let expected = format!("{}/{agent}", conversation.as_str().unwrap_or_default());
        let accepts = child.as_object().is_some_and(|child| {
            !is_archived(child)
                && is_str(child.get("name"), session)
                && py_eq(get(child, "run_id"), run)
                && py_eq(get(child, "parent_conversation_id"), conversation)
                && is_str(child.get("agent_id"), agent)
                && is_str(child.get("conversation_id"), &expected)
                && is_true(child.get("send_supported"))
        });
        if !accepts {
            return Err(Failure::text(CHILD_REFUSED));
        }
        Ok(())
    }

    /// The exact waiting recovery job of this run and conversation.
    async fn recovery_preflight(&self, session: &str, payload: &Value) -> Result<(), Failure> {
        let fresh = self
            .native(Call::new(["inspect", session, "--skip-processes"]))
            .await?
            .json();
        let run = &payload["expected_run_id"];
        let conversation = &payload["expected_conversation_id"];
        let waiting = fresh.as_object().is_some_and(|fresh| {
            is_str(fresh.get("name"), session)
                && !is_archived(fresh)
                && py_eq(get(fresh, "run_id"), run)
                && py_eq(get(fresh, "conversation_id"), conversation)
                && fresh
                    .get("recovery")
                    .and_then(Value::as_object)
                    .is_some_and(|job| {
                        py_eq(get(job, "id"), &payload["job_id"])
                            && is_str(job.get("state"), "waiting")
                            && job.get("identity").and_then(Value::as_array).is_some_and(
                                |identity| {
                                    identity.len() == 5
                                        && py_eq(&identity[0], run)
                                        && py_eq(&identity[1], conversation)
                                },
                            )
                    })
        });
        if !waiting {
            return Err(Failure::text(JOB_CHANGED));
        }
        Ok(())
    }

    async fn recovery(
        &self,
        session: &str,
        payload: &Value,
        request_id: &str,
        started: &Cell<bool>,
    ) -> Result<Value, Failure> {
        let mut stdin = payload.clone();
        stdin["name"] = json!(session);
        let result = self
            .write(
                Call::new(["recovery", "action", "--scoped-json"]).payload(stdin),
                started,
            )
            .await?;
        let mismatch = || Failure::text("native recovery acknowledgement identity did not match");
        let Some(object) = result.as_object() else {
            return Err(mismatch());
        };
        let same = is_str(object.get("request_id"), request_id)
            && is_str(object.get("name"), session)
            && ["run_id", "conversation_id", "job_id", "action"]
                .iter()
                .zip([
                    "expected_run_id",
                    "expected_conversation_id",
                    "job_id",
                    "action",
                ])
                .all(|(field, expected)| py_eq(get(object, field), &payload[expected]));
        if !same
            || !in_set(
                get(object, "status"),
                &["scheduled", "cancelled", "failed", "uncertain"],
            )?
        {
            return Err(mismatch());
        }
        Ok(result)
    }

    async fn lifecycle(
        &self,
        operation: &str,
        session: &str,
        payload: &Value,
        request_id: &str,
        started: &Cell<bool>,
    ) -> Result<Value, Failure> {
        let mut stdin = payload.clone();
        stdin["action"] = json!(operation);
        let result = self
            .write(
                Call::new(["session-action", session, "--json"]).payload(stdin),
                started,
            )
            .await?;
        let Value::Object(mut result) = result else {
            return Err(Failure::text(LIFECYCLE_MISMATCH));
        };
        let same = is_str(result.get("request_id"), request_id)
            && is_str(result.get("name"), session)
            && py_eq(get(&result, "run_id"), &payload["expected_run_id"])
            && py_eq(
                get(&result, "conversation_id"),
                &payload["expected_conversation_id"],
            );
        if !same
            || !in_set(
                get(&result, "status"),
                &["completed", "failed", "uncertain"],
            )?
        {
            return Err(Failure::text(LIFECYCLE_MISMATCH));
        }
        // An acknowledgement without `message_id` is accepted: hgs echoes it
        // only for actions that carry one, and the request, name, run and
        // conversation identity above already bind the receipt. A present
        // but different `message_id` is refused.
        if let Some(message) = payload.get("message_id") {
            if !py_eq(result.get("message_id").unwrap_or(message), message) {
                return Err(Failure::text(
                    "native lifecycle message identity did not match",
                ));
            }
        }
        if let Some(archive) = payload.get("archive_id") {
            if !py_eq(result.get("archive_id").unwrap_or(archive), archive) {
                return Err(Failure::text(
                    "native lifecycle archive identity did not match",
                ));
            }
            result.insert("archive_id".into(), archive.clone());
        }
        let opencode = self.state.borrow().opencode_sessions.contains(session);
        crate::opencode_privacy::action(&mut result, opencode);
        let target = get(&result, "result_target");
        if !target.is_null() {
            let valid = target.as_object().is_some_and(|target| {
                target.keys().all(|key| {
                    ["name", "run_id", "conversation_id", "archive_id"].contains(&key.as_str())
                }) && ["name", "run_id", "conversation_id"]
                    .iter()
                    .all(|field| target.get(*field).is_some_and(Value::is_string))
                    && !target["name"].as_str().unwrap_or_default().is_empty()
                    && !target["run_id"].as_str().unwrap_or_default().is_empty()
            });
            if !valid {
                return Err(Failure::text("native lifecycle result target was invalid"));
            }
            let target = target.as_object().expect("checked");
            if is_str(result.get("status"), "completed")
                && !target.get("archive_id").is_some_and(truthy)
            {
                // An authoritative target can be inspected at once, while the
                // next listing catches up; native inspection still enforces
                // the requested run and conversation.
                let name = target["name"].as_str().expect("checked").to_owned();
                let mut state = self.state.borrow_mut();
                if operation == "rename" && name != session {
                    state.sessions.remove(session);
                    state.opencode_sessions.remove(session);
                }
                if opencode {
                    state.opencode_sessions.insert(name.clone());
                }
                state.sessions.insert(name);
            }
        }
        Ok(Value::Object(result))
    }

    async fn delivery(&self, payload: &Value, started: &Cell<bool>) -> Result<Value, Failure> {
        let value = payload["value"].as_str().ok_or(Failure::Generic)?;
        let call = Call::new([
            "swarm",
            "preference",
            "set",
            "mobile_delivery",
            value,
            "--json",
        ])
        .timeout(10.0);
        let result = self.write(call, started).await?;
        let applied = result.as_object().is_some_and(|result| {
            is_true(result.get("ok")) && is_str(result.get("mobile_delivery"), value)
        });
        if !applied {
            return Err(Failure::text(
                "native delivery setting acknowledgement did not match",
            ));
        }
        Ok(result)
    }

    /// send, answer, interrupt, send_now, settings and the context
    /// commands: the payload reaches the fixed native command unchanged.
    async fn command(
        &self,
        operation: &str,
        session: &str,
        payload: &Value,
        request_id: &str,
        started: &Cell<bool>,
    ) -> Result<Value, Failure> {
        let command = match operation {
            "compact_context" => "compact-context",
            "clear_context" => "clear-context",
            "send_now" => "send-now",
            other => other,
        };
        let call = Call::new([command, session, "--json"]).payload(payload.clone());
        let result = self.write(call, started).await?;
        let object = result.as_object();
        let identity = |object: &Map<String, Value>| {
            is_str(object.get("request_id"), request_id)
                && is_str(object.get("name"), session)
                && py_eq(get(object, "run_id"), &payload["expected_run_id"])
                && py_eq(
                    get(object, "conversation_id"),
                    &payload["expected_conversation_id"],
                )
        };
        if operation == "send" {
            if let Some(agent) = payload.get("agent_id") {
                let delivered = object.is_some_and(|object| {
                    identity(object)
                        && py_eq(get(object, "agent_id"), agent)
                        && is_str(object.get("status"), "submitted")
                });
                if !delivered {
                    return Err(Failure::text(
                        "native child send acknowledgement identity did not match",
                    ));
                }
            }
            return Ok(result);
        }
        if matches!(operation, "answer" | "interrupt") {
            return Ok(result);
        }
        let statuses: &[&str] = match operation {
            "clear_context" => &["submitted", "confirmed"],
            "settings" => &["applied", "scheduled"],
            _ => &["submitted"],
        };
        let Some(object) = object.filter(|object| identity(object)) else {
            return Err(Failure::text(CONTEXT_MISMATCH));
        };
        if !in_set(get(object, "status"), statuses)? {
            return Err(Failure::text(CONTEXT_MISMATCH));
        }
        if operation == "send_now" && !py_eq(get(object, "queue_id"), &payload["queue_id"]) {
            return Err(Failure::text(
                "native queue acknowledgement identity did not match",
            ));
        }
        if operation == "settings" {
            let changed = ["model", "effort"].iter().any(|field| {
                payload
                    .get(*field)
                    .is_some_and(|expected| !py_eq(get(object, field), expected))
            });
            if changed {
                return Err(Failure::text(
                    "native settings acknowledgement did not match",
                ));
            }
            if is_str(object.get("status"), "scheduled") {
                checked_uuid(object.get("pending_settings_id")).map_err(Failure::Connector)?;
            }
        }
        Ok(result)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn send(payload: Value) -> Result<(), ()> {
        checked_send(&payload).ok_or(())
    }

    fn file(data: &str) -> Value {
        json!({"name": "a.bin", "mime": "application/octet-stream", "data_base64": data})
    }

    #[test]
    fn base64_must_be_canonical_in_aligned_chunks() {
        assert_eq!(canonical_base64(b"AAAA"), Some(3));
        assert_eq!(canonical_base64(b"AA=="), Some(1));
        assert_eq!(canonical_base64(b"AAE="), Some(2));
        // Unused bits set: b64decode reads it, re-encoding differs.
        assert_eq!(canonical_base64(b"AB=="), None);
        assert_eq!(canonical_base64(b"AAF="), None);
        for bad in [
            &b"AA="[..],
            b"A===",
            b"====",
            b"AA==AAAA",
            b"A=A=",
            b"AA-_",
            b"AA\nA",
        ] {
            assert_eq!(canonical_base64(bad), None, "{bad:?}");
        }
        let chunk = "A".repeat(BASE64_CHUNK);
        assert_eq!(
            decoded_size(&format!("{chunk}AA==")),
            Some(BASE64_CHUNK / 4 * 3 + 1)
        );
        // Padding inside a chunk that is not the last one.
        let padded = format!("{}AA==", "A".repeat(BASE64_CHUNK - 4));
        assert_eq!(decoded_size(&format!("{padded}AAAA")), None);
        assert_eq!(decoded_size(&padded), Some((BASE64_CHUNK - 4) / 4 * 3 + 1));
        assert_eq!(decoded_size("\u{e9}AAA"), None);
    }

    #[test]
    fn sends_follow_the_python_validator() {
        assert_eq!(send(json!({"text": "hello"})), Ok(()));
        assert_eq!(
            send(json!({"text": "", "attachments": [file("AAAA")]})),
            Ok(())
        );
        assert_eq!(send(json!({"text": "  \u{1c}"})), Err(()));
        assert_eq!(send(json!({"text": "\u{85}x"})), Err(()));
        assert_eq!(send(json!({"text": "\u{a0}/clear"})), Err(()));
        assert_eq!(send(json!({"text": "tab\tand\nline"})), Ok(()));
        assert_eq!(send(json!({"text": "x", "attachments": null})), Err(()));
        assert_eq!(
            send(json!({"text": "x", "expected_compaction_id": ""})),
            Ok(())
        );
        assert_eq!(
            send(
                json!({"text": "x", "expected_compaction_id": "81B191C0-BDAE-543A-BA1C-0FBCBB2F638D"})
            ),
            Err(())
        );
        let mut name = file("AAAA");
        name["name"] = json!("\u{e9}".repeat(128));
        assert_eq!(send(json!({"text": "x", "attachments": [name]})), Err(()));
        for label in ["[Image #1]", "[File #123456789]"] {
            let mut item = file("AAAA");
            item["reference"] = json!(label);
            assert_eq!(
                send(json!({"text": "x", "attachments": [item]})),
                Ok(()),
                "{label}"
            );
        }
        for label in [
            "[Image #0]",
            "[File #1234567890]",
            "[Image #\u{661}]",
            "[Folder #1]",
            " [File #1]",
        ] {
            let mut item = file("AAAA");
            item["reference"] = json!(label);
            assert_eq!(
                send(json!({"text": "x", "attachments": [item]})),
                Err(()),
                "{label}"
            );
        }
        assert_eq!(
            send(json!({"text": "x", "agent_id": "child", "expected_conversation_id": "c"})),
            Ok(())
        );
        assert_eq!(send(json!({"text": "x", "agent_id": "child"})), Err(()));
    }

    #[test]
    fn statuses_judge_python_membership() {
        assert_eq!(in_set(&json!("now"), &["now"]), Ok(true));
        assert_eq!(in_set(&json!(1), &["now"]), Ok(false));
        assert_eq!(in_set(&Value::Null, &["now"]), Ok(false));
        assert_eq!(in_set(&json!(["now"]), &["now"]), Err(Failure::Generic));
        assert_eq!(in_set(&json!({}), &["now"]), Err(Failure::Generic));
    }

    #[test]
    fn delivery_values_are_exact() {
        assert_eq!(
            validate_delivery(Some(&json!({"value": "away_30"}))),
            Ok(())
        );
        for payload in [
            json!({}),
            json!(["away"]),
            json!({"value": "AWAY"}),
            json!({"value": 5}),
            json!({"value": "away", "request_id": "fixture"}),
        ] {
            assert!(validate_delivery(Some(&payload)).is_err(), "{payload}");
        }
        assert!(validate_delivery(None).is_err());
    }

    fn scoped_payload(extra: Value) -> Value {
        let mut payload = json!({"request_id": "r", "expected_run_id": "run",
                                 "expected_conversation_id": "conversation"});
        for (key, value) in extra.as_object().unwrap() {
            payload[key] = value.clone();
        }
        payload
    }

    #[test]
    fn remove_queued_needs_one_exact_message_identity() {
        let check = |extra: Value| validate("remove_queued", &scoped_payload(extra), "r");
        assert_eq!(check(json!({"message_id": "msg_abc123"})), Ok(()));
        assert_eq!(check(json!({"message_id": "msg_a-b_c"})), Ok(()));
        assert!(check(json!({"message_id": "msg_".to_owned() + &"a".repeat(124)})).is_ok());
        for bad in [
            json!({}),
            json!({"message_id": ""}),
            json!({"message_id": "msg_"}),
            json!({"message_id": "abc123"}),
            json!({"message_id": "msg_a b"}),
            json!({"message_id": "msg_/x"}),
            json!({"message_id": 7}),
            json!({"message_id": "msg_".to_owned() + &"a".repeat(125)}),
            json!({"message_id": "msg_a", "text": "x"}),
        ] {
            assert!(check(bad.clone()).is_err(), "{bad}");
        }
        // Run and conversation identity are exact and required.
        let mut payload = scoped_payload(json!({"message_id": "msg_a"}));
        payload
            .as_object_mut()
            .unwrap()
            .remove("expected_conversation_id");
        assert!(validate("remove_queued", &payload, "r").is_err());
        payload["expected_conversation_id"] = json!("");
        assert!(validate("remove_queued", &payload, "r").is_err());
        assert!(LIFECYCLE.contains(&"remove_queued") && handles("remove_queued"));
    }

    #[test]
    fn an_opencode_effort_is_checked_against_the_model_not_the_fixed_list() {
        let settings = |effort: &str| scoped_payload(json!({"effort": effort}));
        let accepts = |session: &str, effort: &str| {
            validate_for_session("settings", session, &settings(effort), "r").is_ok()
        };
        // A variant outside the fixed list is fine for OpenCode only.
        assert!(accepts("opencode/p/t", "thinking-8k"));
        assert!(accepts("opencode/p/t", ""));
        assert!(!accepts("codex/p/t", "thinking-8k"));
        assert!(accepts("codex/p/t", "high"));
        for bad in ["a b", "x/y", &"a".repeat(41)] {
            assert!(!accepts("opencode/p/t", bad), "{bad}");
        }
        let detail = json!({"settings_model": "fixture/local", "model_options": [
            {"id": "fixture/local", "effort_options": ["low", "high"]},
            {"id": "fixture/other", "effort_options": []}]});
        let detail = detail.as_object().unwrap();
        let offered = |extra: Value| opencode_effort_offered(detail, &scoped_payload(extra));
        assert!(offered(json!({"effort": "high"})).is_ok());
        assert!(offered(json!({"model": "fixture/local", "effort": "low"})).is_ok());
        assert!(offered(json!({"effort": ""})).is_ok());
        assert!(offered(json!({"model": "fixture/local"})).is_ok());
        // No current model and no model in the request: nothing is offered.
        let bare = json!({"settings_model": "", "model_options": [
            {"id": "", "effort_options": ["high"]}]});
        assert!(opencode_effort_offered(
            bare.as_object().unwrap(),
            &scoped_payload(json!({"effort": "high"}))
        )
        .is_err());
        let missing =
            json!({"model_options": [{"id": "fixture/local", "effort_options": ["high"]}]});
        assert!(opencode_effort_offered(
            missing.as_object().unwrap(),
            &scoped_payload(json!({"effort": "high"}))
        )
        .is_err());
        for bad in [
            json!({"effort": "ultra"}),
            json!({"model": "fixture/other", "effort": "low"}),
            json!({"model": "fixture/missing", "effort": "low"}),
        ] {
            assert!(offered(bad.clone()).is_err(), "{bad}");
        }
    }
}
