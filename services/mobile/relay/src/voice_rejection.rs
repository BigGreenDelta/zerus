//! Additive, conversation-bound pre-acceptance refusal evidence. Generic errors
//! and missing status are never evidence that a delayed submission cannot run.
use base64::{engine::general_purpose::STANDARD, Engine};
use serde_json::{json, Value};
use sha2::{Digest, Sha256};

type Result<T> = std::result::Result<T, String>;
pub const MAX_PROOF_BYTES: usize = 4096;

fn uuid(value: &Value) -> bool {
    value.as_str().is_some_and(|text| {
        uuid::Uuid::parse_str(text).is_ok_and(|id| !id.is_nil() && id.to_string() == text)
    })
}
fn text(value: &Value, max: usize) -> bool {
    value.as_str().is_some_and(|text| {
        !text.is_empty() && text.len() <= max && !text.chars().any(char::is_control)
    })
}
fn hex(value: &Value) -> bool {
    value.as_str().is_some_and(|text| {
        text.len() == 64
            && text
                .bytes()
                .all(|c| c.is_ascii_digit() || (b'a'..=b'f').contains(&c))
    })
}
fn fields(value: &Value, required: &[&str], optional: &[&str]) -> bool {
    value.as_object().is_some_and(|object| {
        required.iter().all(|key| object.contains_key(*key))
            && object
                .keys()
                .all(|key| required.contains(&key.as_str()) || optional.contains(&key.as_str()))
    })
}
fn decoded(value: &Value, max: usize) -> Result<Vec<u8>> {
    let encoded = value
        .as_str()
        .filter(|value| value.len() <= max.div_ceil(3) * 4)
        .ok_or("Invalid voice intent encoding")?;
    let bytes = STANDARD
        .decode(encoded)
        .map_err(|_| "Invalid voice intent encoding")?;
    if bytes.len() > max {
        return Err("Voice intent exceeds bounds".into());
    }
    Ok(bytes)
}

/// Every immutable request field is represented, with binary data replaced by
/// byte counts and SHA-256. Hash the canonical retained WAV, as clients do.
pub fn intent(payload: &Value) -> Result<Value> {
    let pcm = decoded(&payload["audio"]["data_base64"], 3_840_000)?;
    if !(3200..=3_840_000).contains(&pcm.len()) || pcm.len() % 2 != 0 {
        return Err("Invalid voice PCM intent".into());
    }
    let mut hash = Sha256::new();
    hash.update(b"RIFF");
    hash.update((36 + pcm.len() as u32).to_le_bytes());
    hash.update(b"WAVEfmt ");
    hash.update(16u32.to_le_bytes());
    hash.update(1u16.to_le_bytes());
    hash.update(1u16.to_le_bytes());
    hash.update(16000u32.to_le_bytes());
    hash.update(32000u32.to_le_bytes());
    hash.update(2u16.to_le_bytes());
    hash.update(16u16.to_le_bytes());
    hash.update(b"data");
    hash.update((pcm.len() as u32).to_le_bytes());
    hash.update(&pcm);
    let mut value = json!({"speech":payload["speech"],"audio":{"format":payload["audio"]["format"],
        "sample_rate":payload["audio"]["sample_rate"],"channels":payload["audio"]["channels"],
        "bytes":pcm.len()+44,"sha256":format!("{:x}",hash.finalize())}});
    drop(pcm);
    if let Some(video) = payload.get("video") {
        let data = decoded(&video["data_base64"], 10 * 1024 * 1024)?;
        value["video"] = json!({"format":video["format"],"duration_ms":video["duration_ms"],"width":video["width"],"height":video["height"],
            "bytes":data.len(),"sha256":format!("{:x}",Sha256::digest(&data))});
        drop(data);
        if let Some(poster) = video.get("poster_base64") {
            value["video"]["poster_sha256"] =
                json!(format!("{:x}", Sha256::digest(decoded(poster, 8192)?)));
        }
    }
    Ok(value)
}

pub fn message_id(payload: &Value) -> &Value {
    payload.get("message_id").unwrap_or(&payload["request_id"])
}

pub fn valid_intent(value: &Value) -> bool {
    if !fields(value, &["speech", "audio"], &["video"])
        || !fields(
            &value["speech"],
            &["machine_id", "profile_id"],
            &["profile_revision"],
        )
        || !uuid(&value["speech"]["machine_id"])
        || !text(&value["speech"]["profile_id"], 64)
        || value["speech"]
            .get("profile_revision")
            .is_some_and(|revision| {
                revision
                    .as_u64()
                    .is_none_or(|n| n == 0 || n > 9_007_199_254_740_991)
            })
        || !fields(
            &value["audio"],
            &["format", "sample_rate", "channels", "bytes", "sha256"],
            &[],
        )
        || value["audio"]["format"] != "pcm_s16le"
        || value["audio"]["sample_rate"] != 16000
        || value["audio"]["channels"] != 1
        || !hex(&value["audio"]["sha256"])
        || value["audio"]["bytes"]
            .as_u64()
            .is_none_or(|n| !(3244..=3_840_044).contains(&n) || n % 2 != 0)
    {
        return false;
    }
    if let Some(video) = value.get("video") {
        if !fields(
            video,
            &[
                "format",
                "duration_ms",
                "width",
                "height",
                "bytes",
                "sha256",
            ],
            &["poster_sha256"],
        ) || video["format"] != "mp4"
            || !hex(&video["sha256"])
            || video["bytes"]
                .as_u64()
                .is_none_or(|n| n == 0 || n > 10 * 1024 * 1024)
            || video["duration_ms"]
                .as_u64()
                .is_none_or(|n| !(100..=60_000).contains(&n))
            || ["width", "height"].iter().any(|field| {
                video[*field]
                    .as_u64()
                    .is_none_or(|n| !(16..=1920).contains(&n))
            })
            || video.get("poster_sha256").is_some_and(|value| !hex(value))
        {
            return false;
        }
    }
    true
}

pub fn valid_proof(value: &Value) -> bool {
    value.to_string().len() <= MAX_PROOF_BYTES
        && fields(
            value,
            &[
                "schema",
                "state",
                "request_id",
                "message_id",
                "run_id",
                "conversation_id",
                "intent",
                "payload_sha256",
                "generation",
                "retry_token",
                "code",
                "detail",
            ],
            &[],
        )
        && value["schema"] == 1
        && value["state"] == "rejected_before_acceptance"
        && uuid(&value["request_id"])
        && uuid(&value["message_id"])
        && uuid(&value["retry_token"])
        && text(&value["run_id"], 128)
        && text(&value["conversation_id"], 256)
        && hex(&value["payload_sha256"])
        && valid_intent(&value["intent"])
        && value["generation"]
            .as_u64()
            .is_some_and(|n| n > 0 && n <= i64::MAX as u64)
        && ["route_unavailable", "storage_full", "queue_full"]
            .iter()
            .any(|code| value["code"] == *code)
        && text(&value["detail"], 2000)
}

pub fn validate_retry(payload: &Value) -> Result<()> {
    if payload.get("rejection").is_none() && payload.get("message_id").is_none() {
        return Ok(());
    }
    let proof = &payload["rejection"];
    if !uuid(&payload["message_id"])
        || payload["message_id"] == payload["request_id"]
        || !valid_proof(proof)
        || proof["message_id"] != payload["message_id"]
        || proof["run_id"] != payload["expected_run_id"]
        || proof["conversation_id"] != payload["expected_conversation_id"]
        || proof["intent"] != intent(payload)?
    {
        return Err(
            "Voice retry requires exact durable refusal evidence and unchanged recording".into(),
        );
    }
    Ok(())
}

pub fn result_valid(result: &Value, payload: &Value) -> bool {
    let proof = &result["rejection"];
    result["schema"] == 1
        && valid_proof(proof)
        && proof["request_id"] == payload["request_id"]
        && proof["message_id"] == *message_id(payload)
        && proof["run_id"] == payload["expected_run_id"]
        && proof["conversation_id"] == payload["expected_conversation_id"]
        && (payload.get("audio").is_none()
            || intent(payload).is_ok_and(|intent| proof["intent"] == intent))
}
