//! Fixed, bounded JSON ABI to the optional local speech service. No model runtime
//! or client-supplied commands enter the CLI process.
use crate::{
    cli::{Error, Result},
    config::Config,
};
use serde_json::{json, Value};
use std::{
    fs,
    io::{Read, Write},
    path::PathBuf,
    time::Duration,
};

pub const MAX_REQUEST_BYTES: usize = 5_130_000;
const MAX_RESPONSE_BYTES: usize = 1_048_576;

pub fn dispatch(config: &Config, args: &[String], dry: bool) -> Result<i32> {
    if dry || args != ["--json"] {
        return Err(Error::new(
            1,
            "usage: hgs [@host] speech --json (JSON stdin)",
        ));
    }
    let mut input = Vec::new();
    std::io::stdin()
        .take((MAX_REQUEST_BYTES + 1) as u64)
        .read_to_end(&mut input)?;
    if input.len() > MAX_REQUEST_BYTES {
        return Err(Error::new(1, "Speech request is too large"));
    }
    let value: Value =
        serde_json::from_slice(&input).map_err(|_| Error::new(1, "Invalid speech JSON request"))?;
    println!("{}", request(config, &value)?);
    Ok(0)
}

fn socket_path(config: &Config) -> Result<PathBuf> {
    let settings = match fs::read(config.dir.join("speech.json")) {
        Ok(bytes) if bytes.len() <= 65_536 => serde_json::from_slice::<Value>(&bytes)
            .map_err(|_| Error::new(1, "Invalid speech host configuration"))?,
        Ok(_) => return Err(Error::new(1, "Speech configuration is too large")),
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => Value::Null,
        Err(e) => return Err(e.into()),
    };
    let default_state = PathBuf::from(
        std::env::var("XDG_STATE_HOME").unwrap_or_else(|_| format!("{}/.local/state", config.home)),
    )
    .join("hgs/speech");
    let state = settings
        .get("state_dir")
        .and_then(Value::as_str)
        .map(PathBuf::from)
        .unwrap_or(default_state);
    let path = settings
        .get("socket_path")
        .and_then(Value::as_str)
        .map(PathBuf::from)
        .unwrap_or_else(|| state.join("service.sock"));
    if !path.is_absolute() {
        return Err(Error::new(1, "Speech socket path must be absolute"));
    }
    Ok(path)
}

pub fn request(config: &Config, input: &Value) -> Result<Value> {
    validate_request(input).map_err(|e| Error::new(1, e.to_string()))?;
    let action = input.get("action").and_then(Value::as_str).unwrap_or("");
    if !matches!(
        action,
        "providers" | "submit" | "status" | "cancel" | "stats"
    ) {
        return Err(Error::new(1, "Unknown speech action"));
    }
    let bytes = serde_json::to_vec(input).map_err(|e| Error::new(1, e.to_string()))?;
    if bytes.len() > MAX_REQUEST_BYTES {
        return Err(Error::new(1, "Speech request is too large"));
    }
    let path = socket_path(config)?;
    match exchange(&path, &bytes) {
        Ok(value) => Ok(value),
        Err(error) if action == "providers" => Ok(json!({
            "schema":1,"profiles":[],"available":false,"reason":error.message
        })),
        Err(error) => Err(error),
    }
}

pub(crate) fn validate_request(input: &Value) -> anyhow::Result<()> {
    use base64::Engine;
    let object = input
        .as_object()
        .ok_or_else(|| anyhow::anyhow!("Speech request must be an object"))?;
    let action = input.get("action").and_then(Value::as_str).unwrap_or("");
    let fields: &[&str] = match action {
        "providers" | "stats" => &["action"],
        "submit" => &["action", "request_id", "profile_id", "audio"],
        "status" | "cancel" => &["action", "job_id"],
        _ => anyhow::bail!("Unknown speech action"),
    };
    anyhow::ensure!(
        object.len() == fields.len() && fields.iter().all(|field| object.contains_key(*field)),
        "Invalid speech request fields"
    );
    if action == "submit" || action == "status" || action == "cancel" {
        let key = if action == "submit" {
            "request_id"
        } else {
            "job_id"
        };
        let id = input[key].as_str().unwrap_or("");
        let uuid = uuid::Uuid::parse_str(id)
            .map_err(|_| anyhow::anyhow!("Speech job identity must be a UUID"))?;
        anyhow::ensure!(!uuid.is_nil(), "Speech job UUID must not be nil");
        anyhow::ensure!(
            uuid.hyphenated().to_string() == id,
            "Speech job UUID must be canonical lowercase"
        );
    }
    if action == "submit" {
        let profile = input["profile_id"].as_str().unwrap_or("");
        anyhow::ensure!(
            !profile.is_empty()
                && profile.len() <= 64
                && profile
                    .bytes()
                    .all(|c| c.is_ascii_alphanumeric() || c == b'-' || c == b'_'),
            "Invalid speech profile ID"
        );
        let audio = input["audio"]
            .as_object()
            .ok_or_else(|| anyhow::anyhow!("Invalid speech audio"))?;
        anyhow::ensure!(
            audio.len() == 4
                && input["audio"]["format"] == "pcm_s16le"
                && input["audio"]["sample_rate"].as_u64() == Some(16000)
                && input["audio"]["channels"].as_u64() == Some(1),
            "Audio must be mono PCM s16le at 16000 Hz"
        );
        let encoded = input["audio"]["data_base64"]
            .as_str()
            .ok_or_else(|| anyhow::anyhow!("Audio must be base64"))?;
        anyhow::ensure!(
            encoded.len() <= 5_120_000,
            "Audio exceeds the 120 second limit"
        );
        let decoded = base64::engine::general_purpose::STANDARD
            .decode(encoded)
            .map_err(|_| anyhow::anyhow!("Invalid audio base64"))?;
        anyhow::ensure!(
            !decoded.is_empty() && decoded.len() <= 3_840_000 && decoded.len() % 2 == 0,
            "Invalid PCM sample count"
        );
    }
    Ok(())
}

#[cfg(unix)]
fn exchange(path: &std::path::Path, bytes: &[u8]) -> Result<Value> {
    use std::os::unix::{
        fs::{FileTypeExt, MetadataExt},
        net::UnixStream,
    };
    let metadata = fs::symlink_metadata(path)
        .map_err(|_| Error::new(1, "Speech service is unavailable on this computer"))?;
    if !metadata.file_type().is_socket()
        || metadata.uid() != unsafe { libc::geteuid() }
        || metadata.mode() & 0o077 != 0
    {
        return Err(Error::new(
            1,
            "Speech service socket is not private to this user",
        ));
    }
    let mut stream = UnixStream::connect(path)
        .map_err(|_| Error::new(1, "Speech service is unavailable on this computer"))?;
    stream.set_read_timeout(Some(Duration::from_secs(10)))?;
    stream.set_write_timeout(Some(Duration::from_secs(10)))?;
    stream.write_all(bytes)?;
    stream.write_all(b"\n")?;
    stream.shutdown(std::net::Shutdown::Write)?;
    let mut response = Vec::new();
    stream
        .take((MAX_RESPONSE_BYTES + 1) as u64)
        .read_to_end(&mut response)?;
    if response.len() > MAX_RESPONSE_BYTES {
        return Err(Error::new(1, "Speech service response is too large"));
    }
    let value: Value = serde_json::from_slice(&response)
        .map_err(|_| Error::new(1, "Invalid speech service response"))?;
    if value.get("schema").and_then(Value::as_u64) != Some(1) {
        return Err(Error::new(1, "Unsupported speech service schema"));
    }
    Ok(value)
}

#[cfg(not(unix))]
fn exchange(_path: &std::path::Path, _bytes: &[u8]) -> Result<Value> {
    Err(Error::new(
        1,
        "Use a configured remote computer for speech recognition",
    ))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn rejects_unscoped_fields_bad_ids_and_audio() {
        assert!(validate_request(&json!({"action":"providers"})).is_ok());
        assert!(validate_request(&json!({"action":"stats"})).is_ok());
        assert!(validate_request(&json!({"action":"providers","command":"exec"})).is_err());
        assert!(validate_request(&json!({"action":"status","job_id":"../recording"})).is_err());
        let mut input = json!({"action":"submit","request_id":"f2934a64-a321-4c65-9a1f-54a42386ac9d","profile_id":"gigaam-ru","audio":{"format":"pcm_s16le","sample_rate":16000,"channels":1,"data_base64":"AAA="}});
        assert!(validate_request(&input).is_ok());
        input["audio"]["data_base64"] = json!("AA==");
        assert!(validate_request(&input).is_err());
        input["audio"]["data_base64"] = json!("!!!!");
        assert!(validate_request(&input).is_err());
        input["audio"]["data_base64"] = json!("AAA=");
        input["audio"]["sample_rate"] = json!(16000.0);
        assert!(validate_request(&input).is_err());
    }

    #[test]
    #[cfg(unix)]
    fn socket_requires_private_permissions_and_schema() {
        use std::os::unix::{fs::PermissionsExt, net::UnixListener};
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("service.sock");
        let listener = UnixListener::bind(&path).unwrap();
        fs::set_permissions(&path, fs::Permissions::from_mode(0o666)).unwrap();
        assert!(exchange(&path, b"{}")
            .unwrap_err()
            .message
            .contains("private"));
        fs::set_permissions(&path, fs::Permissions::from_mode(0o600)).unwrap();
        let worker = std::thread::spawn(move || {
            let (mut connection, _) = listener.accept().unwrap();
            let mut input = String::new();
            connection.read_to_string(&mut input).unwrap();
            assert_eq!(input, "{\"action\":\"providers\"}\n");
            connection.write_all(b"{\"schema\":2}\n").unwrap();
        });
        assert!(exchange(&path, b"{\"action\":\"providers\"}")
            .unwrap_err()
            .message
            .contains("schema"));
        worker.join().unwrap();
    }
}
