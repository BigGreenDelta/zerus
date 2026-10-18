//! Speech jobs, speech configuration and voice messages (`speech.py`,
//! `speech_config.py`, `voice.py`): the connector's own validation with
//! Python's texts, and the allowlists a native answer must pass before it
//! becomes a durable receipt. Audio never enters a native argument, and a
//! credential reaches the computer only sealed for it.
use crate::pybase64::{self, DecodeError, NOT_ASCII};
use crate::pyvalue::{
    canonical_uuid, hashable, int_literal, int_within, is_str, py_eq, py_len, PyError, PyResult,
};
use serde_json::{json, Map, Value};

/// Two minutes of mono PCM16 at 16 kHz (`speech.MAX_AUDIO_BYTES`).
pub const MAX_AUDIO_BYTES: usize = 16000 * 2 * 120;
const MAX_AUDIO_ENCODED: usize = MAX_AUDIO_BYTES.div_ceil(3) * 4;
/// Recordings are checked in parts of this many characters.
const AUDIO_PART: usize = 65536;
/// The sealing scheme of a mobile credential (`speech_config.ALGORITHM`).
pub const ALGORITHM: &str = "P256-HKDF-SHA256-A256GCM";
/// `speech_config.MAX_REVISION`: 2^53 - 1.
pub const MAX_REVISION: &str = "9007199254740991";
/// A transcript in a native answer, in UTF-8 bytes.
const MAX_TRANSCRIPT: usize = 65536;
const NIL: &str = "00000000-0000-0000-0000-000000000000";

pub const UNCONFIRMED: &str =
    "speech configuration outcome could not be confirmed; check operation status";
pub const JOB_STATES: [&str; 6] = [
    "queued",
    "running",
    "completed",
    "failed",
    "cancelled",
    "uncertain",
];
/// `voice.STATES`.
pub const VOICE_STATES: [&str; 9] = [
    "queued",
    "transcribing",
    "ready",
    "waiting_agent",
    "delivering",
    "submitted",
    "failed",
    "cancelled",
    "uncertain",
];
const DEVICES: [&str; 5] = ["cpu", "cuda", "cuda:0", "cuda:1", "mps"];

/// The `action` native `hgs speech --json` or `hgs voice <session> --json`
/// receives for an operation (`ACTIONS`).
pub fn action(operation: &str) -> &'static str {
    match operation {
        "speech_profiles" => "providers",
        "speech_stats" => "stats",
        "speech_submit" => "submit",
        "speech_status" => "status",
        "speech_cancel" => "cancel",
        "speech_config" => "config_list",
        "speech_profile_upsert" => "profile_upsert",
        "speech_profile_remove" => "profile_remove",
        "speech_profile_test" => "profile_test",
        "speech_config_status" => "config_status",
        "voice_profiles" => "profiles",
        "voice_submit" => "submit",
        "voice_status" => "status",
        "voice_cancel" => "cancel",
        _ => unreachable!("not a speech or voice operation: {operation}"),
    }
}

fn error(message: &str) -> PyError {
    PyError::value(message)
}

fn nonzero_uuid(value: Option<&Value>) -> bool {
    value
        .and_then(Value::as_str)
        .is_some_and(|text| canonical_uuid(text) && text != NIL)
}

/// `speech.identity(value)`: a canonical, nonzero UUID.
fn job_identity(value: Option<&Value>) -> PyResult<()> {
    if nonzero_uuid(value) {
        Ok(())
    } else {
        Err(error("invalid speech job identity"))
    }
}

/// `speech_config.identity(value)`.
fn identity(value: Option<&Value>) -> PyResult<()> {
    if nonzero_uuid(value) {
        Ok(())
    } else {
        Err(error("invalid speech configuration identity"))
    }
}

/// `speech_config.revision(value)`: an integer from 0 to 2^53 - 1.
fn revision(value: Option<&Value>) -> PyResult<()> {
    match value.and_then(int_literal) {
        Some(digits) if int_within(&digits, "0", MAX_REVISION) => Ok(()),
        _ => Err(error("invalid speech configuration revision")),
    }
}

/// `speech_config.fields(value, required, optional)`.
fn fields<'a>(
    value: &'a Value,
    required: &[&str],
    optional: &[&str],
) -> PyResult<&'a Map<String, Value>> {
    value
        .as_object()
        .filter(|object| {
            required.iter().all(|key| object.contains_key(*key))
                && object
                    .keys()
                    .all(|key| required.contains(&key.as_str()) || optional.contains(&key.as_str()))
        })
        .ok_or_else(|| error("invalid speech configuration fields"))
}

/// `speech_config.bounded(value, maximum, empty)`: UTF-8 bytes, no control
/// character.
fn text(value: Option<&Value>, maximum: usize, empty: bool) -> PyResult<()> {
    let valid = value.and_then(Value::as_str).is_some_and(|text| {
        text.len() <= maximum
            && (empty || !text.is_empty())
            && !text.chars().any(|c| (c as u32) < 32 || c as u32 == 127)
    });
    if valid {
        Ok(())
    } else {
        Err(error("invalid speech configuration text"))
    }
}

/// `[A-Za-z0-9_-]{1,64}`.
fn profile_name(value: Option<&Value>) -> bool {
    value.and_then(Value::as_str).is_some_and(|text| {
        (1..=64).contains(&text.len())
            && text
                .bytes()
                .all(|b| b.is_ascii_alphanumeric() || b == b'_' || b == b'-')
    })
}

/// `speech_config.profile_id(value)`.
fn profile_id(value: Option<&Value>) -> PyResult<()> {
    if profile_name(value) {
        Ok(())
    } else {
        Err(error("invalid speech profile identity"))
    }
}

fn lower_hex(value: Option<&Value>) -> bool {
    value.and_then(Value::as_str).is_some_and(|text| {
        text.len() == 64
            && text
                .bytes()
                .all(|b| b.is_ascii_digit() || (b'a'..=b'f').contains(&b))
    })
}

/// `speech_config.encoded(value, minimum, maximum)`: canonical base64 of
/// `minimum..=maximum` bytes.
fn encoded(value: &Value, minimum: usize, maximum: usize) -> PyResult<()> {
    let invalid = || error("invalid sealed speech credential");
    let Some(text) = value
        .as_str()
        .filter(|text| py_len(text) <= maximum.div_ceil(3) * 4)
    else {
        return Err(invalid());
    };
    match pybase64::canonical(text) {
        Ok(Some(raw)) if (minimum..=maximum).contains(&raw.len()) => Ok(()),
        Err(DecodeError::NotAscii) => Err(error(NOT_ASCII)),
        _ => Err(invalid()),
    }
}

/// `value in {...}` of a set of strings: a list or an object raises
/// TypeError (unhashable).
fn one_of(value: Option<&Value>, set: &[&str]) -> PyResult<bool> {
    match value {
        None => Ok(false),
        Some(value) => Ok(hashable(value)?
            .as_str()
            .is_some_and(|text| set.contains(&text))),
    }
}

/// `speech_config.profile(value)`: one of the two adapters, exactly.
fn profile(value: &Value) -> PyResult<()> {
    let adapter = value.as_object().and_then(|value| value.get("adapter"));
    let common = ["id", "name", "adapter", "model", "language"];
    if is_str(adapter, "gigaam") {
        let object = fields(value, &[&common[..], &["device"]].concat(), &[])?;
        if !one_of(object.get("device"), &DEVICES)? {
            return Err(error("invalid speech inference device"));
        }
    } else if is_str(adapter, "openai_http") {
        let required = [&common[..], &["base_url", "allow_insecure_http"]].concat();
        let object = fields(value, &required, &[])?;
        text(object.get("base_url"), 2048, false)?;
        if !object["allow_insecure_http"].is_boolean() {
            return Err(error("invalid speech HTTP transport choice"));
        }
    } else {
        return Err(error("unsupported speech adapter"));
    }
    profile_id(value.get("id"))?;
    text(value.get("name"), 128, false)?;
    text(value.get("model"), 128, false)?;
    text(value.get("language"), 32, true)?;
    let language = value["language"].as_str().unwrap_or_default();
    let iso_639_1 = language.len() == 2 && language.bytes().all(|b| b.is_ascii_lowercase());
    if !(language.is_empty() || iso_639_1) {
        return Err(error("speech language must be an ISO 639-1 code"));
    }
    Ok(())
}

/// `speech_config.credential(value)`: keep, remove, or replace with a value
/// sealed for this computer. A plaintext credential never passes.
fn credential(value: &Value) -> PyResult<()> {
    let mode = value.as_object().and_then(|value| value.get("mode"));
    if one_of(mode, &["keep", "remove"])? {
        fields(value, &["mode"], &[])?;
        return Ok(());
    }
    if !is_str(mode, "replace") {
        return Err(error("invalid speech credential mode"));
    }
    let object = fields(value, &["mode", "sealed_value"], &[])?;
    let sealed = fields(
        &object["sealed_value"],
        &[
            "key_id",
            "algorithm",
            "ephemeral_public_key",
            "nonce",
            "ciphertext",
        ],
        &[],
    )?;
    if !is_str(sealed.get("algorithm"), ALGORITHM) || !lower_hex(sealed.get("key_id")) {
        return Err(error("invalid sealed speech credential"));
    }
    encoded(&sealed["ephemeral_public_key"], 1, 128)?;
    encoded(&sealed["nonce"], 12, 12)?;
    encoded(&sealed["ciphertext"], 17, 4112)
}

/// `speech_config.validate(operation, payload, request_id)`.
pub fn validate_config(operation: &str, payload: &Value, request_id: &str) -> PyResult<()> {
    fn with(extra: &[&'static str]) -> Vec<&'static str> {
        [&["request_id", "expected_machine_id"], extra].concat()
    }
    match operation {
        "speech_config" => {
            fields(payload, &[], &[])?;
            return Ok(());
        }
        "speech_config_status" => {
            let object = fields(payload, &["operation_id", "expected_machine_id"], &[])?;
            identity(object.get("operation_id"))?;
        }
        "speech_profile_upsert" | "speech_profile_remove" | "speech_profile_test" => {
            let object = match operation {
                "speech_profile_upsert" => {
                    let object = fields(
                        payload,
                        &with(&["expected_revision", "profile", "credential"]),
                        &[],
                    )?;
                    revision(object.get("expected_revision"))?;
                    profile(&object["profile"])?;
                    credential(&object["credential"])?;
                    object
                }
                "speech_profile_remove" => {
                    let object = fields(payload, &with(&["expected_revision", "profile_id"]), &[])?;
                    revision(object.get("expected_revision"))?;
                    profile_id(object.get("profile_id"))?;
                    object
                }
                _ => {
                    let required = with(&["expected_profile_revision", "profile_id"]);
                    let object = fields(payload, &required, &[])?;
                    revision(object.get("expected_profile_revision"))?;
                    profile_id(object.get("profile_id"))?;
                    object
                }
            };
            identity(object.get("request_id"))?;
            if !is_str(object.get("request_id"), request_id) {
                return Err(error(
                    "speech configuration request identity does not match",
                ));
            }
        }
        _ => return Err(error("unsupported speech configuration operation")),
    }
    identity(payload.get("expected_machine_id"))
}

/// The parts of `text` of `size` code points each, as Python slices it.
fn parts(text: &str, size: usize) -> Vec<&str> {
    let mut parts = Vec::new();
    let mut start = 0;
    for (count, (index, _)) in text.char_indices().enumerate() {
        if count > 0 && count % size == 0 {
            parts.push(&text[start..index]);
            start = index;
        }
    }
    parts.push(&text[start..]);
    parts
}

/// A recording: mono PCM16 at 16 kHz, canonical base64 in every part of
/// 65,536 characters, at most two minutes and whole samples.
fn audio(value: &Value) -> PyResult<()> {
    let digits = |key: &str| value.get(key).and_then(int_literal);
    let shape = value.as_object().is_some_and(|audio| {
        audio.len() == 4
            && ["format", "sample_rate", "channels", "data_base64"]
                .iter()
                .all(|key| audio.contains_key(*key))
            && is_str(audio.get("format"), "pcm_s16le")
            && digits("sample_rate").as_deref() == Some("16000")
            && digits("channels").as_deref() == Some("1")
    });
    if !shape {
        return Err(error("speech requires mono PCM16 at 16000 Hz"));
    }
    let Some(encoded) = value["data_base64"]
        .as_str()
        .filter(|encoded| (4..=MAX_AUDIO_ENCODED).contains(&py_len(encoded)))
    else {
        return Err(error("speech recording exceeds 120 seconds"));
    };
    let encoding = || error("invalid speech audio encoding");
    let parts = parts(encoded, AUDIO_PART);
    let last = parts.len() - 1;
    let mut size = 0;
    for (index, part) in parts.into_iter().enumerate() {
        match pybase64::canonical(part) {
            Err(DecodeError::NotAscii) => return Err(error(NOT_ASCII)),
            Err(DecodeError::Invalid) | Ok(None) => return Err(encoding()),
            Ok(Some(_)) if part.contains('=') && index != last => return Err(encoding()),
            Ok(Some(decoded)) => size += decoded.len(),
        }
    }
    if !(2..=MAX_AUDIO_BYTES).contains(&size) || size % 2 != 0 {
        return Err(error("invalid speech audio size"));
    }
    Ok(())
}

/// The checks of a submission after its fields: the request's own identity,
/// the optional profile revision, the profile and the recording.
fn submission(
    request_id: &str,
    profile: Option<&Value>,
    profile_revision: Option<&Value>,
    recording: &Value,
) -> PyResult<()> {
    job_identity(Some(&json!(request_id)))?;
    if let Some(value) = profile_revision {
        revision(Some(value))?;
    }
    if !profile_name(profile) {
        return Err(error("invalid speech profile"));
    }
    audio(recording)
}

/// `speech.validate(operation, payload, request_id)`.
pub fn validate_speech(operation: &str, payload: &Value, request_id: &str) -> PyResult<()> {
    let Some(object) = payload.as_object() else {
        return Err(error("invalid speech payload"));
    };
    match operation {
        "speech_profiles" | "speech_stats" if !object.is_empty() => {
            Err(error("speech profiles takes no fields"))
        }
        "speech_profiles" | "speech_stats" => Ok(()),
        "speech_status" | "speech_cancel" => {
            if object.len() != 1 || !object.contains_key("job_id") {
                return Err(error("speech job requires only its identity"));
            }
            job_identity(object.get("job_id"))
        }
        "speech_submit" => {
            let allowed = ["request_id", "profile_id", "profile_revision", "audio"];
            let exact = ["request_id", "profile_id", "audio"]
                .iter()
                .all(|key| object.contains_key(*key))
                && object.keys().all(|key| allowed.contains(&key.as_str()))
                && is_str(object.get("request_id"), request_id);
            if !exact {
                return Err(error("speech request identity does not match"));
            }
            submission(
                request_id,
                object.get("profile_id"),
                object.get("profile_revision"),
                &object["audio"],
            )
        }
        _ => Err(error("unsupported speech operation")),
    }
}

/// `voice.validate(operation, payload, request_id)`: a voice message of one
/// exact conversation, sent to a speech profile of one exact computer.
pub fn validate_voice(operation: &str, payload: &Value, request_id: &str) -> PyResult<()> {
    let mut extra = match operation {
        "voice_submit" => vec!["speech", "audio"],
        "voice_status" | "voice_cancel" => vec!["message_id"],
        _ => vec![],
    };
    if operation=="voice_submit" && payload.get("video").is_some() {extra.push("video");}
    if operation=="voice_submit" && (payload.get("message_id").is_some() || payload.get("rejection").is_some()) {extra.extend(["message_id","rejection"]);}
    let common = ["request_id", "expected_run_id", "expected_conversation_id"];
    let Some(object) = payload.as_object().filter(|payload| {
        crate::capabilities::VOICE.contains(&operation)
            && payload.len() == common.len() + extra.len()
            && common
                .iter()
                .chain(&extra)
                .all(|key| payload.contains_key(*key))
            && is_str(payload.get("request_id"), request_id)
    }) else {
        return Err(error("invalid voice message fields"));
    };
    job_identity(Some(&json!(request_id)))?;
    for (field, limit) in [("expected_run_id", 128), ("expected_conversation_id", 256)] {
        let valid = object[field].as_str().is_some_and(|value| {
            py_len(value) <= limit
                && (!value.is_empty()
                    || (field != "expected_run_id" && operation == "voice_profiles"))
                && !value.chars().any(|c| (c as u32) < 32 || c as u32 == 127)
        });
        if !valid {
            return Err(error("invalid voice conversation identity"));
        }
    }
    if operation == "voice_submit" {
        let allowed = ["machine_id", "profile_id", "profile_revision"];
        let Some(route) = object["speech"].as_object().filter(|route| {
            route.contains_key("machine_id")
                && route.contains_key("profile_id")
                && route.keys().all(|key| allowed.contains(&key.as_str()))
        }) else {
            return Err(error("invalid voice speech route"));
        };
        job_identity(route.get("machine_id"))?;
        submission(
            request_id,
            route.get("profile_id"),
            route.get("profile_revision"),
            &object["audio"],
        )?;
        if let Some(video) = object.get("video") {
            let encoded = object["audio"]["data_base64"].as_str().unwrap_or_default();
            let pcm_bytes =
                encoded.len() / 4 * 3 - (encoded.len() - encoded.trim_end_matches('=').len());
            zerus_relay::validate_voice_video(video, zerus_relay::voice_rejection::message_id(payload).as_str().unwrap_or_default(), pcm_bytes)
                .map_err(|message| error(&message))?;
        }
        zerus_relay::voice_rejection::validate_retry(payload).map_err(|message|error(&message))?;
        return Ok(());
    }
    if extra.contains(&"message_id") {
        job_identity(object.get("message_id"))?;
    }
    Ok(())
}

/// `result.get("schema") != 1` is false.
fn schema_one(result: &Map<String, Value>) -> bool {
    result
        .get("schema")
        .is_some_and(|schema| py_eq(schema, &json!(1)))
}

fn transcript(value: Option<&Value>) -> bool {
    value.is_none_or(|text| {
        text.as_str()
            .is_some_and(|text| text.len() <= MAX_TRANSCRIPT)
    })
}

fn same(value: Option<&Value>, expected: Option<&Value>) -> bool {
    py_eq(
        value.unwrap_or(&Value::Null),
        expected.unwrap_or(&Value::Null),
    )
}

/// `speech.result_valid(operation, result, payload)`. A job state that is
/// a list or an object raises TypeError there: the generic receipt.
pub fn speech_result_valid(operation: &str, result: &Value, payload: &Value) -> PyResult<bool> {
    let Some(result) = result.as_object().filter(|result| schema_one(result)) else {
        return Ok(false);
    };
    match operation {
        "speech_stats" => return Ok(true),
        "speech_profiles" => {
            return Ok(result
                .get("profiles")
                .and_then(Value::as_array)
                .is_some_and(|profiles| profiles.len() <= 32)
                && result.get("available").is_some_and(Value::is_boolean))
        }
        _ => {}
    }
    let expected = if operation == "speech_submit" {
        payload.get("request_id")
    } else {
        payload.get("job_id")
    };
    Ok(same(result.get("job_id"), expected)
        && one_of(result.get("state"), &JOB_STATES)?
        && transcript(result.get("text")))
}

/// `voice.result_valid(operation, result, payload)`, with the same TypeError
/// for an unhashable message state.
pub fn voice_result_valid(operation: &str, result: &Value, payload: &Value) -> PyResult<bool> {
    if operation!="voice_profiles" && result.get("rejection").is_some() {
        return Ok(zerus_relay::voice_rejection::result_valid(result,payload));
    }
    let Some(result) = result.as_object().filter(|result| schema_one(result)) else {
        return Ok(false);
    };
    if operation == "voice_profiles" {
        return Ok(result
            .get("computers")
            .and_then(Value::as_array)
            .is_some_and(|computers| computers.len() <= 128));
    }
    let Some(message) = result.get("message").and_then(Value::as_object) else {
        return Ok(false);
    };
    let expected = if operation == "voice_submit" {
        payload.get("message_id").or_else(||payload.get("request_id"))
    } else {
        payload.get("message_id")
    };
    if operation == "voice_submit" {
        if let Some(video) = payload.get("video") {
            let Some(media) = message.get("video") else {
                return Ok(false);
            };
            let encoded = video["data_base64"].as_str().unwrap_or_default();
            let bytes =
                encoded.len() / 4 * 3 - (encoded.len() - encoded.trim_end_matches('=').len());
            if media.get("request_id") != expected
                || media["index"] != 1
                || media["mime"] != "video/mp4"
                || media["name"] != "video.mp4"
                || media["bytes"].as_u64() != Some(bytes as u64)
                || ["duration_ms", "width", "height"]
                    .iter()
                    .any(|field| media[*field] != video[*field])
            {
                return Ok(false);
            }
        }
    }
    Ok(same(message.get("id"), expected)
        && same(message.get("run_id"), payload.get("expected_run_id"))
        && same(
            message.get("conversation_id"),
            payload.get("expected_conversation_id"),
        )
        && one_of(message.get("state"), &VOICE_STATES)?
        && message.get("revision").and_then(int_literal).is_some()
        && transcript(message.get("transcript")))
}

fn reject() -> PyError {
    PyError::Type
}

fn configuration(result: &Map<String, Value>) -> PyResult<()> {
    let value = Value::Object(result.clone());
    fields(
        &value,
        &[
            "schema",
            "machine_id",
            "revision",
            "service",
            "adapters",
            "profiles",
            "credential_key",
        ],
        &[],
    )?;
    revision(result.get("revision"))?;
    let service = fields(&result["service"], &["available"], &["reason"])?;
    if !service["available"].is_boolean() {
        return Err(reject());
    }
    if let Some(reason) = service.get("reason") {
        text(Some(reason), 512, true)?;
    }
    let profiles = result["profiles"]
        .as_array()
        .filter(|profiles| profiles.len() <= 32)
        .ok_or_else(reject)?;
    for item in profiles {
        let item = fields(
            item,
            &[
                "id",
                "revision",
                "name",
                "adapter",
                "model",
                "language",
                "credential",
                "available",
                "max_duration_ms",
            ],
            &["device", "base_url", "allow_insecure_http", "reason"],
        )?;
        profile_id(item.get("id"))?;
        revision(item.get("revision"))?;
        for (key, maximum) in [
            ("name", 128),
            ("adapter", 32),
            ("model", 128),
            ("language", 32),
        ] {
            text(item.get(key), maximum, key == "language")?;
        }
        let credential = fields(&item["credential"], &["configured"], &[])?;
        if !credential["configured"].is_boolean() || !item["available"].is_boolean() {
            return Err(reject());
        }
        let duration = int_literal(&item["max_duration_ms"]);
        if !duration.is_some_and(|duration| int_within(&duration, "1", "120000")) {
            return Err(reject());
        }
        for (key, maximum) in [("device", 16), ("base_url", 2048), ("reason", 512)] {
            if let Some(value) = item.get(key) {
                text(Some(value), maximum, key == "reason")?;
            }
        }
        if item
            .get("allow_insecure_http")
            .is_some_and(|value| !value.is_boolean())
        {
            return Err(reject());
        }
    }
    let adapters = result["adapters"]
        .as_array()
        .filter(|adapters| adapters.len() <= 8)
        .ok_or_else(reject)?;
    for adapter in adapters {
        let adapter = fields(adapter, &["id"], &["models", "devices"])?;
        text(adapter.get("id"), 32, false)?;
        for key in ["models", "devices"] {
            if let Some(list) = adapter.get(key) {
                let list = list
                    .as_array()
                    .filter(|list| list.len() <= 32)
                    .ok_or_else(reject)?;
                for item in list {
                    text(Some(item), 128, false)?;
                }
            }
        }
    }
    let key = fields(
        &result["credential_key"],
        &["key_id", "algorithm", "public_key_spki"],
        &[],
    )?;
    if !is_str(key.get("algorithm"), ALGORITHM) || !lower_hex(key.get("key_id")) {
        return Err(reject());
    }
    encoded(&key["public_key_spki"], 1, 128)
}

fn outcome(result: &Map<String, Value>, payload: &Value) -> PyResult<()> {
    let value = Value::Object(result.clone());
    fields(
        &value,
        &["schema", "machine_id", "request_id", "state"],
        &[
            "revision",
            "profile_id",
            "profile_revision",
            "error_code",
            "error",
        ],
    )?;
    let operation = payload
        .get("operation_id")
        .or_else(|| payload.get("request_id"));
    if !same(result.get("machine_id"), payload.get("expected_machine_id"))
        || !same(result.get("request_id"), operation)
        || !is_str(result.get("state"), "completed")
            && !is_str(result.get("state"), "failed")
            && !is_str(result.get("state"), "running")
            && !is_str(result.get("state"), "uncertain")
    {
        return Err(reject());
    }
    for key in ["revision", "profile_revision"] {
        if let Some(value) = result.get(key) {
            revision(Some(value))?;
        }
    }
    if let Some(value) = result.get("profile_id") {
        profile_id(Some(value))?;
    }
    for key in ["error_code", "error"] {
        if let Some(value) = result.get(key) {
            text(Some(value), 512, true)?;
        }
    }
    Ok(())
}

/// `speech_config.result_valid(operation, result, payload)`: only
/// allowlisted fields, so no secret or raw provider body becomes a durable
/// receipt. Every failure, a TypeError included, is a refusal.
pub fn config_result_valid(operation: &str, result: &Value, payload: &Value) -> bool {
    let checked = result
        .as_object()
        .filter(|result| result.get("schema").and_then(int_literal).as_deref() == Some("1"))
        .ok_or_else(reject)
        .and_then(|result| {
            identity(result.get("machine_id"))?;
            if operation == "speech_config" {
                configuration(result)
            } else {
                outcome(result, payload)
            }
        });
    checked.is_ok()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn recordings_are_split_by_code_points() {
        assert_eq!(parts("abcde", 2), ["ab", "cd", "e"]);
        assert_eq!(parts("abcd", 2), ["ab", "cd"]);
        assert_eq!(parts("\u{e9}b\u{e9}", 2), ["\u{e9}b", "\u{e9}"]);
        assert_eq!(parts("a", 2), ["a"]);
    }

    #[test]
    fn a_non_ascii_recording_or_credential_keeps_the_python_value_error() {
        let recording = |data: &str| json!({"format": "pcm_s16le", "sample_rate": 16000, "channels": 1, "data_base64": data});
        assert_eq!(audio(&recording("AAAA\u{e9}")), Err(error(NOT_ASCII)));
        // An earlier part is judged first.
        let early = format!("{}A\u{e9}", "A=".repeat(AUDIO_PART / 2));
        assert_eq!(
            audio(&recording(&early)),
            Err(error("invalid speech audio encoding"))
        );
        assert_eq!(
            audio(&recording("AAAA")),
            Err(error("invalid speech audio size"))
        );
        assert_eq!(audio(&recording("AAA=")), Ok(()));
        assert_eq!(
            audio(&recording("AAB=")),
            Err(error("invalid speech audio encoding"))
        );
        assert_eq!(encoded(&json!("\u{e9}AAA"), 1, 3), Err(error(NOT_ASCII)));
        assert_eq!(encoded(&json!("AAAA"), 1, 3), Ok(()));
        assert_eq!(
            encoded(&json!("AAAA"), 4, 4),
            Err(error("invalid sealed speech credential"))
        );
    }

    #[test]
    fn unhashable_values_raise_type_errors_where_python_does() {
        let payload = json!({"job_id": "00000000-0000-4000-8000-000000000001"});
        let answer = |state: Value| json!({"schema": 1, "job_id": "00000000-0000-4000-8000-000000000001", "state": state});
        assert_eq!(
            speech_result_valid("speech_status", &answer(json!(["queued"])), &payload),
            Err(PyError::Type)
        );
        assert_eq!(
            speech_result_valid("speech_status", &answer(json!(5)), &payload),
            Ok(false)
        );
        // Another job is refused before its state is looked at.
        let other = json!({"schema": true, "job_id": "x", "state": []});
        assert_eq!(
            speech_result_valid("speech_status", &other, &payload),
            Ok(false)
        );
        assert_eq!(credential(&json!({"mode": ["keep"]})), Err(PyError::Type));
        // The configuration allowlist refuses instead.
        let status = json!({"schema": 1, "machine_id": "00000000-0000-4000-8000-000000000002",
                            "request_id": "00000000-0000-4000-8000-000000000003", "state": ["completed"]});
        let identity = json!({"operation_id": "00000000-0000-4000-8000-000000000003",
                              "expected_machine_id": "00000000-0000-4000-8000-000000000002"});
        assert!(!config_result_valid(
            "speech_config_status",
            &status,
            &identity
        ));
        let mut fixed = status.clone();
        fixed["state"] = json!("completed");
        assert!(config_result_valid(
            "speech_config_status",
            &fixed,
            &identity
        ));
    }
}
