//! Bounded speech-management envelopes. Only the owning host decrypts keys.
use crate::{
    error::{Error, Result},
    protocol::{fields, text, uuid},
};
use base64::{engine::general_purpose::STANDARD, Engine};
use serde_json::Value;

pub const OPERATIONS: &[&str] = &[
    "speech_config",
    "speech_profile_upsert",
    "speech_profile_remove",
    "speech_profile_test",
    "speech_config_status",
];
pub const READ_SQL: &str = "('speech_config','speech_config_status')";

fn bounded(value: &Value, maximum: usize, empty: bool) -> Result<&str> {
    let value = text(value, maximum, empty)?;
    if value.len() > maximum {
        return Err(Error::BAD);
    }
    Ok(value)
}

fn revision(value: &Value) -> Result<()> {
    if value.as_u64().is_none_or(|n| n > 9_007_199_254_740_991) {
        return Err(Error::BAD);
    }
    Ok(())
}

fn profile_id(value: &Value) -> Result<()> {
    if !bounded(value, 64, false)?
        .bytes()
        .all(|b| b.is_ascii_alphanumeric() || b"_-".contains(&b))
    {
        return Err(Error::BAD);
    }
    Ok(())
}

fn encoded(value: &Value, minimum: usize, maximum: usize) -> Result<()> {
    let value = bounded(value, maximum.div_ceil(3) * 4, false)?;
    let bytes = STANDARD.decode(value).map_err(|_| Error::BAD)?;
    if !(minimum..=maximum).contains(&bytes.len()) || STANDARD.encode(&bytes) != value {
        return Err(Error::BAD);
    }
    Ok(())
}

fn credential(value: &Value) -> Result<()> {
    match value["mode"].as_str() {
        Some("keep" | "remove") => fields(value, &["mode"], &[]),
        Some("replace") => {
            // A plaintext value must never enter the durable relay queue.
            fields(value, &["mode", "sealed_value"], &[])?;
            let sealed = &value["sealed_value"];
            fields(
                sealed,
                &[
                    "key_id",
                    "algorithm",
                    "ephemeral_public_key",
                    "nonce",
                    "ciphertext",
                ],
                &[],
            )?;
            if sealed["algorithm"] != "P256-HKDF-SHA256-A256GCM" {
                return Err(Error::BAD);
            }
            let id = bounded(&sealed["key_id"], 64, false)?;
            if id.len() != 64
                || !id
                    .bytes()
                    .all(|b| b.is_ascii_digit() || (b'a'..=b'f').contains(&b))
            {
                return Err(Error::BAD);
            }
            encoded(&sealed["ephemeral_public_key"], 1, 128)?;
            encoded(&sealed["nonce"], 12, 12)?;
            encoded(&sealed["ciphertext"], 17, 4112)
        }
        _ => Err(Error::BAD),
    }
}

fn profile(value: &Value) -> Result<()> {
    let required = &["id", "name", "adapter", "model", "language"];
    match value["adapter"].as_str() {
        Some("gigaam") => {
            let mut required = required.to_vec();
            required.push("device");
            fields(value, &required, &[])?;
            if !["cpu", "cuda", "cuda:0", "cuda:1", "mps"]
                .iter()
                .any(|x| value["device"] == *x)
            {
                return Err(Error::BAD);
            }
        }
        Some("openai_http") => {
            let mut required = required.to_vec();
            required.extend(["base_url", "allow_insecure_http"]);
            fields(value, &required, &[])?;
            bounded(&value["base_url"], 2048, false)?;
            if !value["allow_insecure_http"].is_boolean() {
                return Err(Error::BAD);
            }
        }
        _ => return Err(Error::BAD),
    }
    profile_id(&value["id"])?;
    bounded(&value["name"], 128, false)?;
    bounded(&value["model"], 128, false)?;
    bounded(&value["language"], 32, true)?;
    let language = value["language"].as_str().ok_or(Error::BAD)?;
    if !language.is_empty()
        && (language.len() != 2 || !language.bytes().all(|b| b.is_ascii_lowercase()))
    {
        return Err(Error::BAD);
    }
    Ok(())
}

pub fn validate(operation: &str, payload: &Value, request_id: &str) -> Result<()> {
    if operation == "speech_config" {
        return fields(payload, &[], &[]);
    }
    if operation == "speech_config_status" {
        fields(payload, &["operation_id", "expected_machine_id"], &[])?;
        uuid(&payload["operation_id"], true)?;
    } else {
        match operation {
            "speech_profile_upsert" => {
                fields(
                    payload,
                    &[
                        "request_id",
                        "expected_machine_id",
                        "expected_revision",
                        "profile",
                        "credential",
                    ],
                    &[],
                )?;
                revision(&payload["expected_revision"])?;
                profile(&payload["profile"])?;
                credential(&payload["credential"])?;
            }
            "speech_profile_remove" => {
                fields(
                    payload,
                    &[
                        "request_id",
                        "expected_machine_id",
                        "expected_revision",
                        "profile_id",
                    ],
                    &[],
                )?;
                revision(&payload["expected_revision"])?;
                profile_id(&payload["profile_id"])?;
            }
            "speech_profile_test" => {
                fields(
                    payload,
                    &[
                        "request_id",
                        "expected_machine_id",
                        "expected_profile_revision",
                        "profile_id",
                    ],
                    &[],
                )?;
                revision(&payload["expected_profile_revision"])?;
                profile_id(&payload["profile_id"])?;
            }
            _ => return Err(Error::BAD),
        }
        uuid(&payload["request_id"], true)?;
        if payload["request_id"] != request_id {
            return Err(Error::BAD);
        }
    }
    uuid(&payload["expected_machine_id"], true)?;
    Ok(())
}
