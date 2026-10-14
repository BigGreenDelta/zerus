//! OpenCode's reviewed native API, authenticated on loopback. Native hosts
//! outlive HGS and the GUI; manifests contain bindings, never provider keys.
use super::*;
use base64::{engine::general_purpose::STANDARD, Engine};
use sha2::{Digest, Sha256};
use std::fs::{self, OpenOptions};
use std::io::{Read, Write};
use std::net::{TcpListener, TcpStream};
use std::process::{Command, Stdio};
use std::time::{Duration, Instant};

pub(crate) const VERSION: &str = "1.18.35";
fn bindings() -> PathBuf {
    root().join("opencode/bindings")
}
fn binding_path(name: &str) -> PathBuf {
    bindings().join(format!("{:x}.json", Sha256::digest(name.as_bytes())))
}
pub(crate) fn exists(name: &str) -> bool {
    binding_path(name).is_file()
}
pub(super) fn binding(name: &str) -> Result<Value> {
    let bytes = fs::read(binding_path(name)).map_err(|e| e.to_string())?;
    if bytes.len() > 1_048_576 {
        return Err("Invalid OpenCode binding".into());
    }
    let record: Value = serde_json::from_slice(&bytes).map_err(|e| e.to_string())?;
    if record["name"] != name
        || record["backend"] != "opencode"
        || !native_id(string(&record, "conversation_id"), "ses_")
    {
        return Err("Invalid OpenCode binding".into());
    }
    Ok(record)
}
fn native_id(id: &str, prefix: &str) -> bool {
    id.starts_with(prefix)
        && id.len() <= 128
        && id
            .bytes()
            .all(|c| c.is_ascii_alphanumeric() || c == b'_' || c == b'-')
}
fn save(record: &Value) -> Result<()> {
    atomic(&binding_path(string(record, "name")), &record.to_string())
}
pub(crate) fn config_home() -> PathBuf {
    nonempty_env("OPENCODE_CONFIG_DIR")
        .map(PathBuf::from)
        .unwrap_or_else(|| {
            nonempty_env("XDG_CONFIG_HOME")
                .map(PathBuf::from)
                .unwrap_or_else(|| home().join(".config"))
                .join("opencode")
        })
}
fn data_home() -> PathBuf {
    nonempty_env("XDG_DATA_HOME")
        .map(PathBuf::from)
        .unwrap_or_else(|| home().join(".local/share"))
}
fn identity() -> Value {
    json!({"agent_home":config_home(),"native_data_home":data_home(),"native_config_file":nonempty_env("OPENCODE_CONFIG").unwrap_or_default()})
}
fn host_dir(record: &Value) -> PathBuf {
    root().join("opencode/hosts").join(format!(
        "{:x}",
        Sha256::digest(
            json!([
                record["agent_home"],
                record["native_data_home"],
                record["native_config_file"]
            ])
            .to_string()
            .as_bytes()
        )
    ))
}
fn host(record: &Value) -> Result<Value> {
    let bytes = fs::read(host_dir(record).join("host.json"))
        .map_err(|_| "OpenCode host is unavailable; explicitly resume the session".to_owned())?;
    let value: Value =
        serde_json::from_slice(&bytes).map_err(|_| "Invalid private OpenCode host metadata")?;
    if value["version"] != VERSION
        || value["port"].as_u64().is_none_or(|p| p == 0 || p > 65535)
        || string(&value, "password").len() != 36
        || string(&value, "generation").is_empty()
        || value["pid"]
            .as_u64()
            .is_none_or(|pid| pid <= 1 || pid > u32::MAX as u64)
        || string(&value, "process_start").is_empty()
        || string(&value, "process_start").len() > 512
    {
        return Err("Invalid private OpenCode host metadata".into());
    }
    Ok(value)
}
struct Frame {
    header_end: usize,
    end: usize,
    status: u16,
    chunked: bool,
}
fn frame(bytes: &[u8]) -> Result<Option<Frame>> {
    let Some(split) = bytes.windows(4).position(|p| p == b"\r\n\r\n") else {
        if bytes.len() > 32768 {
            return Err("Native HTTP headers exceed bounds".into());
        }
        return Ok(None);
    };
    if split > 32768 {
        return Err("Native HTTP headers exceed bounds".into());
    }
    let header = std::str::from_utf8(&bytes[..split]).map_err(|_| "Invalid native HTTP headers")?;
    let mut lines = header.split("\r\n");
    let mut status_line = lines
        .next()
        .ok_or("Missing native HTTP status")?
        .split_whitespace();
    if !matches!(status_line.next(), Some("HTTP/1.1" | "HTTP/1.0")) {
        return Err("Invalid native HTTP protocol".into());
    }
    let status = status_line
        .next()
        .and_then(|s| s.parse::<u16>().ok())
        .filter(|s| (100..600).contains(s))
        .ok_or("Invalid native HTTP status")?;
    let mut length = None;
    let mut chunked = false;
    for line in lines {
        let (name, value) = line.split_once(':').ok_or("Invalid native HTTP header")?;
        if name.eq_ignore_ascii_case("content-length") {
            if length.is_some() {
                return Err("Duplicate native Content-Length".into());
            }
            let n = value.trim();
            if n.is_empty() || !n.bytes().all(|b| b.is_ascii_digit()) {
                return Err("Invalid native Content-Length".into());
            }
            length = Some(
                n.parse::<usize>()
                    .map_err(|_| "Native Content-Length exceeds bounds")?,
            );
        }
        if name.eq_ignore_ascii_case("transfer-encoding") {
            if chunked || !value.trim().eq_ignore_ascii_case("chunked") {
                return Err("Unsupported native Transfer-Encoding".into());
            }
            chunked = true;
        }
    }
    if chunked && length.is_some() {
        return Err("Conflicting native HTTP framing".into());
    }
    let body_start = split + 4;
    let end = if chunked {
        let mut offset = body_start;
        loop {
            let rest = &bytes[offset..];
            let Some(n) = rest.windows(2).position(|p| p == b"\r\n") else {
                return Ok(None);
            };
            if n > 1024 {
                return Err("Native HTTP chunk header exceeds bounds".into());
            }
            let raw = std::str::from_utf8(&rest[..n]).map_err(|_| "Invalid native chunk size")?;
            let size_text = raw.split(';').next().unwrap_or("");
            if size_text.is_empty() || !size_text.bytes().all(|b| b.is_ascii_hexdigit()) {
                return Err("Invalid native chunk size".into());
            }
            let size = usize::from_str_radix(size_text, 16)
                .map_err(|_| "Native chunk size exceeds bounds")?;
            offset += n + 2;
            if size == 0 {
                let trailers = &bytes[offset..];
                if trailers.starts_with(b"\r\n") {
                    break offset + 2;
                }
                let Some(trailer_end) = trailers.windows(4).position(|p| p == b"\r\n\r\n") else {
                    return Ok(None);
                };
                if trailer_end > 32768 {
                    return Err("Native HTTP trailers exceed bounds".into());
                }
                let trailer = std::str::from_utf8(&trailers[..trailer_end])
                    .map_err(|_| "Invalid native HTTP trailer")?;
                if trailer.split("\r\n").any(|line| !line.contains(':')) {
                    return Err("Invalid native HTTP trailer".into());
                }
                break offset + trailer_end + 4;
            }
            let next = offset
                .checked_add(size)
                .and_then(|n| n.checked_add(2))
                .ok_or("Native chunk size exceeds bounds")?;
            if next > 32 * 1024 * 1024 {
                return Err("Native chunk body exceeds bounds".into());
            }
            if next > bytes.len() {
                return Ok(None);
            }
            if &bytes[next - 2..next] != b"\r\n" {
                return Err("Invalid native chunk boundary".into());
            }
            offset = next;
        }
    } else if let Some(length) = length {
        let end = body_start
            .checked_add(length)
            .filter(|n| *n <= 32 * 1024 * 1024)
            .ok_or("Native HTTP body exceeds bounds")?;
        if bytes.len() < end {
            return Ok(None);
        }
        end
    } else if status == 204 {
        body_start
    } else {
        return Err("Missing native HTTP body framing".into());
    };
    Ok(Some(Frame {
        header_end: body_start,
        end,
        status,
        chunked,
    }))
}
fn decode_response(bytes: &[u8]) -> Result<Value> {
    let framing = frame(bytes)?.ok_or("Incomplete native HTTP response; delivery uncertain")?;
    if framing.end != bytes.len() {
        return Err("Unexpected bytes after native HTTP response".into());
    }
    let raw = &bytes[framing.header_end..framing.end];
    let mut decoded = Vec::new();
    let body = if framing.chunked {
        let mut rest = raw;
        loop {
            let n = rest
                .windows(2)
                .position(|p| p == b"\r\n")
                .ok_or("Invalid native chunk")?;
            let size = usize::from_str_radix(
                std::str::from_utf8(&rest[..n])
                    .map_err(|_| "Invalid native chunk")?
                    .split(';')
                    .next()
                    .unwrap_or(""),
                16,
            )
            .map_err(|_| "Invalid native chunk")?;
            rest = &rest[n + 2..];
            if size == 0 {
                break;
            }
            decoded.extend_from_slice(&rest[..size]);
            rest = &rest[size + 2..];
        }
        decoded.as_slice()
    } else {
        raw
    };
    if !(200..300).contains(&framing.status) {
        let reason = serde_json::from_slice::<Value>(body)
            .ok()
            .map(|v| {
                v["message"]
                    .as_str()
                    .or(v["data"]["message"].as_str())
                    .unwrap_or("Native operation refused")
                    .to_owned()
            })
            .unwrap_or_else(|| "Native operation refused".into());
        return Err(format!("OpenCode HTTP {}: {reason}", framing.status));
    }
    if body.is_empty() {
        return Ok(Value::Null);
    }
    serde_json::from_slice(body)
        .map_err(|_| "Invalid native JSON response; delivery uncertain".into())
}
fn http(host: &Value, method: &str, path: &str, body: Option<&Value>) -> Result<Value> {
    http_until(
        host,
        method,
        path,
        body,
        Instant::now() + Duration::from_secs(10),
    )
}
fn http_until(
    host: &Value,
    method: &str,
    path: &str,
    body: Option<&Value>,
    deadline: Instant,
) -> Result<Value> {
    let address = std::net::SocketAddr::from((
        [127, 0, 0, 1],
        host["port"].as_u64().ok_or("Missing native endpoint")? as u16,
    ));
    let mut socket = TcpStream::connect_timeout(
        &address,
        deadline
            .checked_duration_since(Instant::now())
            .ok_or("Native HTTP deadline exceeded")?
            .min(Duration::from_secs(2)),
    )
    .map_err(|_| "OpenCode host is unavailable; explicitly resume the session".to_owned())?;
    socket
        .set_read_timeout(Some(Duration::from_secs(10)))
        .map_err(|e| e.to_string())?;
    socket
        .set_write_timeout(Some(
            deadline
                .checked_duration_since(Instant::now())
                .ok_or("Native HTTP deadline exceeded")?
                .min(Duration::from_secs(5)),
        ))
        .map_err(|e| e.to_string())?;
    let body = body.map(Value::to_string).unwrap_or_default();
    let auth = STANDARD.encode(format!("opencode:{}", string(host, "password")));
    let headers=format!("{method} {path} HTTP/1.1\r\nHost: 127.0.0.1\r\nAuthorization: Basic {auth}\r\nContent-Type: application/json\r\nContent-Length: {}\r\nConnection: close\r\n\r\n",body.len());
    socket
        .write_all(headers.as_bytes())
        .and_then(|_| socket.write_all(body.as_bytes()))
        .map_err(|e| format!("Native request delivery uncertain: {e}"))?;
    let mut bytes = Vec::new();
    let mut chunk = [0u8; 8192];
    loop {
        let remaining = deadline
            .checked_duration_since(Instant::now())
            .ok_or("Native HTTP deadline exceeded; delivery uncertain")?;
        socket
            .set_read_timeout(Some(remaining))
            .map_err(|e| e.to_string())?;
        let n = socket
            .read(&mut chunk)
            .map_err(|e| format!("Native response delivery uncertain: {e}"))?;
        if n == 0 {
            break;
        }
        bytes.extend_from_slice(&chunk[..n]);
        if bytes.len() > 32 * 1024 * 1024 {
            return Err("Native response exceeds bounds; delivery uncertain".into());
        }
        if frame(&bytes)?.is_some() {
            break;
        }
    }
    decode_response(&bytes)
}
fn rpc(record: &Value, method: &str, path: &str, body: Option<&Value>) -> Result<Value> {
    http(&host(record)?, method, path, body)
}
fn endpoint(record: &Value, suffix: &str) -> String {
    format!("/api/session/{}{suffix}", string(record, "conversation_id"))
}
fn ensure_host(record: &Value) -> Result<()> {
    file_configuration_only()?;
    let dir = host_dir(record);
    private_dir(&dir)?;
    let _guard = lock(Some(&dir.join("start.lock")))?;
    if dir.join("host.json").exists() {
        let previous = host(record)?;
        if let Ok(health) = http(&previous, "GET", "/global/health", None) {
            if health["healthy"] == true && health["version"] == VERSION {
                return Ok(());
            }
            return Err("The resident OpenCode host is incompatible; it was left running".into());
        }
        // Never replace a hung host; the launcher may be systemd-run rather
        // than OpenCode, so also refuse any listener at its recorded endpoint.
        let pid = previous["pid"].as_u64().unwrap_or(0) as u32;
        if pid > 0
            && !string(&previous, "process_start").is_empty()
            && process_start(pid) == string(&previous, "process_start")
        {
            return Err("The resident OpenCode host is not responding; it was left running".into());
        }
        let address = std::net::SocketAddr::from((
            [127, 0, 0, 1],
            previous["port"].as_u64().unwrap_or(0) as u16,
        ));
        if TcpStream::connect_timeout(&address, Duration::from_millis(200)).is_ok() {
            return Err(
                "The private native endpoint is still occupied; it was left untouched".into(),
            );
        }
    }
    let version = crate::platform::output(Command::new("opencode").arg("--version"))
        .map_err(|_| format!("Install OpenCode {VERSION} on this computer"))?;
    if !version.status.success() || String::from_utf8_lossy(&version.stdout).trim() != VERSION {
        return Err(format!(
            "This native adapter requires reviewed OpenCode {VERSION}"
        ));
    }
    let listener = TcpListener::bind(("127.0.0.1", 0)).map_err(|e| e.to_string())?;
    let port = listener.local_addr().map_err(|e| e.to_string())?.port();
    drop(listener);
    let mut metadata = json!({"version":VERSION,"port":port,"password":uuid::Uuid::new_v4().to_string(),"generation":uuid::Uuid::new_v4().to_string()});
    let log = crate::platform::private_open(OpenOptions::new().create(true).append(true))
        .open(dir.join("host.log"))
        .map_err(|e| e.to_string())?;
    let mut command = Command::new("opencode");
    #[cfg(target_os = "linux")]
    {
        let runtime = PathBuf::from(format!("/run/user/{}", unsafe { libc::getuid() }));
        if runtime.join("systemd/private").exists() && crate::platform::available("systemd-run") {
            command = Command::new("systemd-run");
            command
                .args([
                    "--user",
                    "--scope",
                    "--quiet",
                    "--collect",
                    "--expand-environment=no",
                    "--unit",
                ])
                .arg(format!("hgs-opencode-{}", uuid::Uuid::new_v4()))
                .args(["--", "opencode"])
                .env("XDG_RUNTIME_DIR", runtime);
        }
    }
    command
        .args(["serve", "--hostname", "127.0.0.1", "--port"])
        .arg(port.to_string())
        .env("OPENCODE_SERVER_USERNAME", "opencode")
        .env("OPENCODE_SERVER_PASSWORD", string(&metadata, "password"))
        .env("OPENCODE_CONFIG_DIR", string(record, "agent_home"))
        .env("XDG_DATA_HOME", string(record, "native_data_home"))
        .env_remove("OPENCODE_CONFIG_CONTENT")
        .env_remove("OPENCODE_CONFIG")
        .current_dir(home())
        .stdin(Stdio::null())
        .stdout(log.try_clone().map_err(|e| e.to_string())?)
        .stderr(log);
    for key in [
        "HGS_SESSION",
        "HGS_RUN_ID",
        "HGS_ACCOUNT_ID",
        "HGS_ACCOUNT_HOME",
        "HGS_AGENT",
    ] {
        command.env_remove(key);
    }
    if !string(record, "native_config_file").is_empty() {
        command.env("OPENCODE_CONFIG", string(record, "native_config_file"));
    }
    // Detach only the dedicated native host: its own session outlives the
    // launching terminal and HGS process.
    let mut child = crate::platform::spawn_detached(&mut command).map_err(|e| e.to_string())?;
    metadata["pid"] = json!(child.id());
    metadata["process_start"] = json!(process_start(child.id()));
    atomic(&dir.join("host.json"), &metadata.to_string())?;
    let deadline = Instant::now() + Duration::from_secs(35);
    while Instant::now() < deadline {
        if let Ok(health) = http(&metadata, "GET", "/global/health", None) {
            if health["healthy"] == true && health["version"] == VERSION {
                return Ok(());
            }
        }
        if child.try_wait().map_err(|e| e.to_string())?.is_some() {
            return Err("OpenCode host failed to start; inspect its private host log".into());
        }
        std::thread::sleep(Duration::from_millis(100));
    }
    Err("OpenCode is still starting; explicitly resume when ready".into())
}

// JSONC syntax is accepted by OpenCode configuration. Strip only comments
// and trailing commas outside quoted strings; never change URL or key bytes.
fn configuration_json(bytes: &[u8]) -> Result<Value> {
    let mut clean = bytes.to_vec();
    let mut i = 0;
    let mut quoted = false;
    let mut escaped = false;
    while i < clean.len() {
        let byte = clean[i];
        if quoted {
            if escaped {
                escaped = false;
            } else if byte == b'\\' {
                escaped = true;
            } else if byte == b'"' {
                quoted = false;
            }
            i += 1;
            continue;
        }
        if byte == b'"' {
            quoted = true;
            i += 1;
            continue;
        }
        if byte == b'/' && clean.get(i + 1) == Some(&b'/') {
            while i < clean.len() && !matches!(clean[i], b'\n' | b'\r') {
                clean[i] = b' ';
                i += 1;
            }
            continue;
        }
        if byte == b'/' && clean.get(i + 1) == Some(&b'*') {
            clean[i] = b' ';
            clean[i + 1] = b' ';
            i += 2;
            let mut closed = false;
            while i + 1 < clean.len() {
                if clean[i] == b'*' && clean[i + 1] == b'/' {
                    clean[i] = b' ';
                    clean[i + 1] = b' ';
                    i += 2;
                    closed = true;
                    break;
                }
                if !matches!(clean[i], b'\n' | b'\r') {
                    clean[i] = b' ';
                }
                i += 1;
            }
            if !closed {
                return Err("Unclosed native configuration comment".into());
            }
            continue;
        }
        i += 1;
    }
    quoted = false;
    escaped = false;
    for i in 0..clean.len() {
        let byte = clean[i];
        if quoted {
            if escaped {
                escaped = false;
            } else if byte == b'\\' {
                escaped = true;
            } else if byte == b'"' {
                quoted = false;
            }
            continue;
        }
        if byte == b'"' {
            quoted = true;
        } else if byte == b','
            && clean[i + 1..]
                .iter()
                .find(|c| !c.is_ascii_whitespace())
                .is_some_and(|c| matches!(c, b'}' | b']'))
        {
            clean[i] = b' ';
        }
    }
    serde_json::from_slice(&clean).map_err(|_| "Native OpenCode configuration is invalid".into())
}
fn file_configuration_only() -> Result<()> {
    if nonempty_env("OPENCODE_CONFIG_CONTENT").is_some() {
        return Err("Inline OpenCode configuration is not supported by this adapter; use the host opencode.json file".into());
    }
    Ok(())
}
/// Accounts never start a server or expose provider credentials. Explicitly
/// configured model IDs are enough for a cold local-model launch picker.
pub(crate) fn catalog() -> Value {
    if let Err(reason) = file_configuration_only() {
        return json!({"model_options":[],"settings_model":"","model_catalog_source":"unavailable","model_catalog_reason":reason});
    }
    let mut paths = vec![
        config_home().join("opencode.json"),
        config_home().join("opencode.jsonc"),
    ];
    if let Some(p) = nonempty_env("OPENCODE_CONFIG") {
        paths.push(p.into());
    }
    let mut models = BTreeMap::<String, Value>::new();
    let mut selected = String::new();
    for path in paths {
        let Ok(bytes) = fs::read(path) else { continue };
        if bytes.len() > 1_048_576 {
            continue;
        }
        let Ok(value) = configuration_json(&bytes) else {
            continue;
        };
        if let Some(v) = value["model"].as_str() {
            selected = v.into();
        }
        for (provider, definition) in value["provider"].as_object().into_iter().flatten() {
            for (id, model) in definition["models"].as_object().into_iter().flatten() {
                let key = format!("{provider}/{id}");
                if model_ref(&key).is_err() {
                    continue;
                }
                models.insert(key.clone(),json!({"id":key,"value":key,"effort_options":[],"label":model.get("name").and_then(Value::as_str).unwrap_or(id),"provider":provider}));
            }
        }
    }
    if model_ref(&selected).is_ok() {
        models.entry(selected.clone()).or_insert_with(
            || json!({"id":selected,"value":selected,"effort_options":[],"label":selected}),
        );
    }
    json!({"model_options":models.into_values().collect::<Vec<_>>(),"settings_model":selected,"model_catalog_source":"native_configuration"})
}
fn offered_model(value: &str) -> Result<Value> {
    let model = model_ref(value)?;
    if !catalog()["model_options"]
        .as_array()
        .into_iter()
        .flatten()
        .any(|row| row["id"] == value)
    {
        return Err("This OpenCode model is not in the current native configuration; refresh its model catalog".into());
    }
    Ok(model)
}
fn model_ref(value: &str) -> Result<Value> {
    let (provider, id) = value
        .split_once('/')
        .ok_or("Choose provider/model for OpenCode")?;
    if provider.is_empty()
        || id.is_empty()
        || value.len() > 256
        || value.chars().any(|c| c.is_control() || c.is_whitespace())
    {
        return Err("Invalid OpenCode provider/model".into());
    }
    Ok(json!({"providerID":provider,"id":id}))
}
pub(crate) fn account_context(name: &str) -> Result<Value> {
    let record = binding(name)?;
    Ok(
        json!({"provider":"opencode","id":"native-opencode","home":record["agent_home"],"run_id":record["run_id"]}),
    )
}
pub(crate) fn launch(
    name: &str,
    cwd: &str,
    launch_id: Option<&str>,
    model: Option<&str>,
    dry: bool,
    new_only: bool,
) -> Result<()> {
    file_configuration_only()?;
    let selected = model.map(offered_model).transpose()?;
    if dry {
        println!("Create native OpenCode session {name} in {cwd}");
        return Ok(());
    }
    let _writer = lock(None)?;
    let _guard = lock(Some(&root().join("opencode/bindings.lock")))?;
    if record_path(name).exists() || live()?.contains_key(name) {
        return Err(
            "This name belongs to an existing terminal session; it was left untouched".into(),
        );
    }
    let mode = crate::accounts::permission_mode(
        &crate::config::Config::load().map_err(|e| e.message)?,
        "native-opencode",
        "opencode",
    )
    .map_err(|e| e.message)?;
    if mode != "provider" {
        return Err("OpenCode uses its native permission configuration; select Provider settings for this account".into());
    }
    if exists(name) {
        if new_only {
            return Err("This OpenCode session name already exists".into());
        }
        let mut record = binding(name)?;
        ensure_host(&record)?;
        verify_session(&record)?;
        record["paused"] = json!(false);
        save(&record)?;
        return Ok(());
    }
    let cwd = crate::platform::canonical(cwd).map_err(|e| e.to_string())?;
    if !cwd.is_dir() {
        return Err("OpenCode requires an existing directory".into());
    }
    let mut record = identity();
    for (key,value) in json!({"backend":"opencode","agent":"opencode","name":name,"cwd":cwd,"launch_id":launch_id,"run_id":uuid::Uuid::new_v4().to_string(),"conversation_id":format!("ses_{}",uuid::Uuid::new_v4().simple()),"created":now(),"paused":false,"creation_pending":true}).as_object().unwrap() {record[key]=value.clone();}
    ensure_host(&record)?;
    // Persist the chosen native ID before the first creation attempt. Resume
    // reads this exact ID; it never retries POST after an uncertain handoff.
    save(&record)?;
    let mut body = json!({"id":record["conversation_id"],"location":{"directory":record["cwd"]}});
    if let Some(model) = selected {
        body["model"] = model;
    }
    let result = rpc(&record, "POST", "/api/session", Some(&body))?;
    if result["data"]["id"] != record["conversation_id"]
        || result["data"]["location"]["directory"] != record["cwd"]
    {
        return Err(
            "OpenCode creation acknowledgement did not match; inspect before resuming".into(),
        );
    }
    record.as_object_mut().unwrap().remove("creation_pending");
    save(&record)
}
fn verify_session(record: &Value) -> Result<Value> {
    let result = rpc(record, "GET", &endpoint(record, ""), None)?;
    if result["data"]["id"] != record["conversation_id"]
        || result["data"]["location"]["directory"] != record["cwd"]
    {
        return Err("The exact native OpenCode session or directory changed".into());
    }
    Ok(result["data"].clone())
}
fn history(record: &Value) -> Result<Vec<Value>> {
    let cache = root().join("opencode/history").join(format!(
        "{:x}.json",
        Sha256::digest(
            json!([record["native_data_home"], record["conversation_id"]])
                .to_string()
                .as_bytes()
        )
    ));
    let _guard = lock(Some(&cache.with_extension("lock")))?;
    let generation = host(record)?["generation"].clone();
    let cached = fs::metadata(&cache)
        .ok()
        .filter(|m| m.len() <= 16 * 1024 * 1024)
        .and_then(|_| fs::read(&cache).ok())
        .and_then(|b| serde_json::from_slice::<Value>(&b).ok())
        .unwrap_or(Value::Null);
    let mut rows = if cached["generation"] == generation {
        cached["rows"].as_array().cloned().unwrap_or_default()
    } else {
        Vec::new()
    };
    let mut byte_count = rows.iter().map(|e| e.to_string().len()).sum::<usize>();
    let deadline = Instant::now() + Duration::from_secs(20);
    let mut after = rows
        .last()
        .and_then(|e| e["durable"]["seq"].as_i64())
        .unwrap_or(0);
    for _ in 0..100 {
        if Instant::now() >= deadline {
            return Err("Native history deadline exceeded".into());
        }
        let page = http_until(
            &host(record)?,
            "GET",
            &endpoint(record, &format!("/history?limit=100&after={after}")),
            None,
            deadline.min(Instant::now() + Duration::from_secs(10)),
        )?;
        let batch = page["data"].as_array().ok_or("Invalid OpenCode history")?;
        if batch.is_empty() && page["hasMore"] == true {
            return Err("OpenCode history pagination made no progress".into());
        }
        for event in batch {
            if event["data"]["sessionID"] != record["conversation_id"] {
                return Err("Native history belongs to another conversation".into());
            }
            let seq = event["durable"]["seq"]
                .as_i64()
                .ok_or("Native history lacks a durable sequence")?;
            if seq <= after {
                return Err("Native history sequence is not ordered".into());
            }
            byte_count = byte_count.saturating_add(event.to_string().len());
            if byte_count > 16 * 1024 * 1024 {
                return Err("Native history exceeds its cumulative byte bound".into());
            }
            after = seq;
            rows.push(event.clone());
        }
        if rows.len() > 10000 {
            return Err("OpenCode history exceeds the bounded scan".into());
        }
        if page["hasMore"] == false {
            atomic(
                &cache,
                &json!({"generation":generation,"rows":rows}).to_string(),
            )?;
            return Ok(rows);
        }
    }
    Err("OpenCode history exceeds the bounded scan; no complete-history claim was made".into())
}
fn event_rows(native: &[Value]) -> Vec<Value> {
    let mut rows = Vec::new();
    for event in native {
        let data = &event["data"];
        let kind = string(event, "type");
        let (typ, detail, tool) = match kind {
            "session.next.prompted" => (
                "UserPromptSubmit",
                string(&data["prompt"], "text").to_owned(),
                String::new(),
            ),
            "session.next.text.ended" => (
                "AgentMessage",
                string(data, "text").to_owned(),
                String::new(),
            ),
            "session.next.tool.called" => (
                "PreToolUse",
                journal::clipped(&data["input"], 32000),
                string(data, "tool").to_owned(),
            ),
            "session.next.tool.success" => (
                "PostToolUse",
                data["content"]
                    .as_array()
                    .into_iter()
                    .flatten()
                    .filter_map(|c| c["text"].as_str())
                    .collect::<Vec<_>>()
                    .join("\n"),
                String::new(),
            ),
            "session.next.tool.failed" => (
                "PostToolUse",
                journal::clipped(&data["error"], 32000),
                String::new(),
            ),
            "session.next.step.failed" => {
                ("StopFailure", error_detail(&data["error"]), String::new())
            }
            "session.next.step.ended" => ("Stop", String::new(), String::new()),
            _ => continue,
        };
        rows.push(json!({"type":typ,"detail":detail,"tool":tool,"at":data["timestamp"].as_f64().unwrap_or(0.0)/1000.0,"seq":event["durable"]["seq"],"source":"opencode_native","agent_id":"","message_id":data.get("messageID").unwrap_or(&data["assistantMessageID"]),"reply_id":if typ=="AgentMessage"{data["assistantMessageID"].clone()}else{Value::Null}}));
    }
    rows
}
fn error_detail(value: &Value) -> String {
    value["message"]
        .as_str()
        .or(value["data"]["message"].as_str())
        .map(str::to_owned)
        .unwrap_or_else(|| journal::clipped(value, 32000))
}
fn cards(record: &Value, questions: &Value, permissions: &Value) -> Vec<Value> {
    let mut cards = Vec::new();
    for (kind, pending) in [("question", questions), ("permission", permissions)] {
        for native in pending.as_array().into_iter().flatten() {
            if native["sessionID"] != record["conversation_id"] {
                continue;
            }
            let mut items = Vec::new();
            if kind == "permission" {
                items.push(json!({"id":"permission","header":"Permission","prompt":format!("{}\n{}",string(native,"action"),native["resources"].as_array().into_iter().flatten().filter_map(Value::as_str).collect::<Vec<_>>().join("\n")),"allow_multiple":false,"multi_select":false,"required":true,"question":format!("{}\n{}",string(native,"action"),native["resources"].as_array().into_iter().flatten().filter_map(Value::as_str).collect::<Vec<_>>().join("\n")),"allow_other":false,"options":[{"id":"once","label":"Allow once"},{"id":"always","label":"Always allow"},{"id":"reject","label":"Reject"}]}));
            } else {
                for (i, q) in native["questions"]
                    .as_array()
                    .into_iter()
                    .flatten()
                    .enumerate()
                {
                    items.push(json!({"id":i.to_string(),"header":q["header"],"prompt":q["question"],"question":q["question"],"multi_select":q["multiple"].as_bool().unwrap_or(false),"required":true,"allow_multiple":q["multiple"].as_bool().unwrap_or(false),"allow_other":q["custom"].as_bool().unwrap_or(true),"options":q["options"].as_array().into_iter().flatten().enumerate().map(|(j,o)|json!({"id":j.to_string(),"label":o["label"],"description":o["description"]})).collect::<Vec<_>>() }));
                }
            }
            let hash = format!(
                "{:x}",
                Sha256::digest(
                    json!([record["run_id"], record["conversation_id"], native])
                        .to_string()
                        .as_bytes()
                )
            );
            cards.push(json!({"question_id":native["id"],"question_hash":hash,"run_id":record["run_id"],"conversation_id":record["conversation_id"],"kind":format!("opencode_{kind}"),"status":"pending","answerable":true,"can_answer":true,"approval":kind=="permission","answer_transport":"native_opencode","agent_id":"main","questions":items,"source":"opencode_native"}));
        }
    }
    cards
}
fn normalize(
    record: &Value,
    session: &Value,
    active: bool,
    native: &[Value],
    questions: &Value,
    permissions: &Value,
    available: bool,
) -> Value {
    let pending = cards(record, questions, permissions);
    let alive = available && record["paused"] != true && !session.is_null();
    let error_event = native
        .iter()
        .rev()
        .find_map(|e| match string(e, "type") {
            "session.next.step.failed" => Some(e.clone()),
            "session.next.prompted" | "session.next.step.started" | "session.next.step.ended" => {
                Some(Value::Null)
            }
            _ => None,
        })
        .unwrap_or(Value::Null);
    let error = error_event["data"]["error"].clone();
    let phase = if !available {
        "unknown"
    } else if !pending.is_empty() {
        "input"
    } else if active {
        "working"
    } else if error.is_object() {
        "error"
    } else {
        "idle"
    };
    let parts = string(record, "name").splitn(3, '/').collect::<Vec<_>>();
    let events = event_rows(native);
    let reply = events.iter().rev().find(|e| e["type"] == "AgentMessage");
    let prompt = events
        .iter()
        .rev()
        .find(|e| e["type"] == "UserPromptSubmit");
    let turn_started = native
        .iter()
        .rev()
        .find(|e| {
            matches!(
                string(e, "type"),
                "session.next.step.started" | "session.next.prompted"
            )
        })
        .and_then(|e| e["data"]["timestamp"].as_f64())
        .unwrap_or(0.0)
        / 1000.0;
    let mut allowed = if available {
        vec!["rename"]
    } else {
        vec!["resume"]
    };
    if available && !active {
        allowed.push("forget");
        allowed.push(if alive { "pause" } else { "resume" });
    }
    let mut value = json!({"name":record["name"],"cmd":"opencode","agent":"opencode","backend":"opencode","project":parts.get(1),"tag":parts.get(2),"run_id":record["run_id"],"conversation_id":record["conversation_id"],"launch_id":record["launch_id"],"created":record["created"],"cwd":record["cwd"],"cwd_source":"native_session","tracked":true,"resumable":true,"attached":0,"clients":[],"state":if alive{"running"}else if record["paused"]==true{"paused"}else{"stopped"},"process_state":if alive{"running"}else{"exited"},"runtime_state":if alive{"live"}else{"stopped"},"conversation_state":"active","phase":phase,"activity":if !pending.is_empty() || error.is_object(){"attention"}else if active{"busy"}else if alive{"idle"}else{"unknown"},"activity_summary":if !pending.is_empty(){"Needs attention"}else if active{"Working"}else if error.is_object(){"Provider request failed"}else if alive{"Ready"}else{"Stopped"},"activity_detail":reply.map(|r|r["detail"].clone()),"prompt":prompt.map(|r|r["detail"].clone()),"reply_id":reply.map(|r|r["message_id"].clone()),"reply_at":reply.map(|r|r["at"].clone()),"last_event_at":session["time"]["updated"].as_f64().unwrap_or(0.0)/1000.0,"turn_started":turn_started,"account_id":"native-opencode","account_home":record["agent_home"],"model":session["model"]["id"],"model_provider":session["model"]["providerID"],"settings_model":format!("{}/{}",string(&session["model"],"providerID"),string(&session["model"],"id"))});
    for (key,item) in json!({"send_available":alive && !active && pending.is_empty(),"send_supported":alive && !active && pending.is_empty(),"settings_change_supported":alive && !active && pending.is_empty(),"interrupt_supported":alive && active && turn_started>0.0,"terminal_supported":false,"terminal_reason":backend::OPENCODE_TERMINAL_REASON.text(),"native_ui_available":false,"bulk_lifecycle_supported":false,"attachments_supported":false,"clear_context_supported":false,"clear_context_reason":"OpenCode context reset is not available in this adapter","compact_context_supported":false,"compact_context_reason":"OpenCode compaction is not yet qualified in this adapter","allowed_actions":allowed,"action_reasons":{"archive":"OpenCode archives are not supported by this adapter","fork":"OpenCode forks are not supported by this adapter","terminate":"Use Stop or Pause for this native OpenCode session","restore":"OpenCode archives are not supported by this adapter"},"pending_questions":pending,"events":events,"replace_events":true,"cursor":0,"history_truncated":false}).as_object().unwrap() {value[key]=item.clone();}
    if !pending.is_empty() {
        value["attention_id"] = json!(format!(
            "opencode_question:{}",
            pending
                .iter()
                .filter_map(|q| q["question_id"].as_str())
                .collect::<Vec<_>>()
                .join("\n")
        ));
    }
    if error.is_object() && !active && pending.is_empty() {
        let failure = provider_errors::event(
            &format!(
                "opencode-error-{}",
                error_event["durable"]["seq"].as_i64().unwrap_or(0)
            ),
            session["time"]["updated"].as_f64().unwrap_or(0.0) / 1000.0,
            &error_detail(&error),
            "opencode_native",
        );
        provider_errors::apply(&mut value, &failure);
    }
    value
}
pub(crate) fn inspect(name: &str, agent_id: Option<&str>) -> Result<Value> {
    if agent_id.is_some() {
        return Err("OpenCode child-session controls are not yet qualified".into());
    }
    let record = binding(name)?;
    let session = verify_session(&record)?;
    let active = rpc(&record, "GET", "/api/session/active", None)?;
    let questions = rpc(&record, "GET", &endpoint(&record, "/question"), None)?;
    let permissions = rpc(&record, "GET", &endpoint(&record, "/permission"), None)?;
    let native = history(&record)?;
    let mut result = normalize(
        &record,
        &session,
        active["data"]
            .get(string(&record, "conversation_id"))
            .is_some(),
        &native,
        &questions["data"],
        &permissions["data"],
        true,
    );
    result["host_generation"] = host(&record)?["generation"].clone();
    result["attachment_messages"] = attachments::messages(&record, "");
    result["voice_messages"] =
        conversation_history::inspected_voice_messages(&record, &result, None);
    let configured = catalog();
    result["model_options"] = configured["model_options"].clone();
    result["processes"] = json!({"items":[],"complete":false,"source":"opencode_native","reason":"Individual OpenCode shell process controls are not yet qualified"});
    let questions = result["pending_questions"]
        .as_array()
        .cloned()
        .unwrap_or_default();
    read_marks::enrich(&mut result, &read_marks::load(), name, Some(&questions));
    Ok(result)
}
pub(crate) fn snapshots() -> Result<Vec<Value>> {
    let Ok(entries) = fs::read_dir(bindings()) else {
        return Ok(Vec::new());
    };
    let mut rows = Vec::new();
    for entry in entries.flatten().take(5000) {
        let Ok(bytes) = fs::read(entry.path()) else {
            continue;
        };
        let Ok(record) = serde_json::from_slice::<Value>(&bytes) else {
            continue;
        };
        if record["backend"] != "opencode" {
            continue;
        }
        let row = inspect(string(&record, "name"), None).unwrap_or_else(|error| {
            let mut unavailable = normalize(
                &record,
                &Value::Null,
                false,
                &[],
                &Value::Null,
                &Value::Null,
                false,
            );
            unavailable["state"] = json!(if record["paused"] == true {
                "paused"
            } else {
                "unknown"
            });
            unavailable["process_state"] = json!("unknown");
            unavailable["runtime_state"] = json!("unknown");
            unavailable["activity_summary"] = json!("Inspection unavailable");
            unavailable["inspection_unavailable"] = json!(true);
            unavailable["inspection_reason"] = json!(error);
            unavailable
        });
        let mut row = row;
        if let Some(map) = row.as_object_mut() {
            for key in [
                "events",
                "voice_messages",
                "attachment_messages",
                "processes",
                "model_options",
            ] {
                map.remove(key);
            }
        }
        rows.push(row);
    }
    Ok(rows)
}
fn checked_request(record: &Value) -> Result<Value> {
    let mut bytes = Vec::new();
    io::stdin()
        .take(30 * 1024 * 1024 + 1)
        .read_to_end(&mut bytes)
        .map_err(|e| e.to_string())?;
    if bytes.len() > 30 * 1024 * 1024 {
        return Err("Native request exceeds bounds".into());
    }
    let request: Value = serde_json::from_slice(&bytes).map_err(|e| e.to_string())?;
    let id =
        uuid::Uuid::parse_str(string(&request, "request_id")).map_err(|_| "Invalid request ID")?;
    if id.is_nil()
        || id.to_string() != string(&request, "request_id")
        || request["expected_run_id"] != record["run_id"]
        || request["expected_conversation_id"] != record["conversation_id"]
    {
        return Err("Session changed; refresh before trying again".into());
    }
    Ok(request)
}
pub(super) fn reconcile_receipt(record: &Value, receipt: &mut Value) -> Result<bool> {
    if receipt["status"] == "submitted" {
        return Ok(true);
    }
    verify_session(record)?;
    let id = string(receipt, "native_message_id");
    if !native_id(id, "msg_")
        || receipt["conversation_id"] != record["conversation_id"]
        || receipt["run_id"] != record["run_id"]
    {
        return Err("Native receipt identity changed".into());
    }
    let native = history(record)?;
    let admission = native
        .iter()
        .find(|e| e["type"] == "session.next.prompt.admitted" && e["data"]["messageID"] == id);
    if let Some(event) = admission {
        if event["data"]["prompt"]["text"] != receipt["text"] {
            return Err("Native message ID has conflicting text; delivery uncertain".into());
        }
        receipt["status"] = json!("submitted");
        receipt["native_seq"] = event["durable"]["seq"].clone();
        atomic(
            &root()
                .join("opencode/receipts")
                .join(format!("{}.json", string(receipt, "request_id"))),
            &receipt.to_string(),
        )?;
        return Ok(true);
    }
    Ok(false)
}
fn send(record: &Value, request: &Value) -> Result<Value> {
    if !string(request, "agent_id").is_empty() {
        return Err("OpenCode child delivery is not supported".into());
    }
    let text = string(request, "text");
    if text.len() > 65536 {
        return Err("Message exceeds64KiB".into());
    }
    // A first version cannot silently downgrade attachments or compaction
    // continuation into plain text.
    if request["attachments"]
        .as_array()
        .is_some_and(|v| !v.is_empty())
    {
        return Err("OpenCode attachments are not yet supported by this adapter".into());
    }
    if !string(request, "expected_compaction_id").is_empty() {
        return Err("OpenCode compaction continuation is not supported".into());
    }
    if text.trim().is_empty() {
        return Err("Message is empty".into());
    }
    let path = root()
        .join("opencode/receipts")
        .join(format!("{}.json", string(request, "request_id")));
    let digest = format!(
        "{:x}",
        Sha256::digest(json!([record["name"], request]).to_string().as_bytes())
    );
    let message_id = format!("msg_{}", string(request, "request_id").replace('-', ""));
    if path.exists() {
        let mut prior: Value = serde_json::from_slice(&fs::read(&path).map_err(|e| e.to_string())?)
            .map_err(|e| e.to_string())?;
        if prior["digest"] != digest {
            return Err("This request ID was already used for another message".into());
        }
        if reconcile_receipt(record, &mut prior)? {
            return Ok(prior);
        }
        return Err("Delivery uncertain; this request will not be replayed. Inspect native history before creating a new request".into());
    }
    let current = inspect(string(record, "name"), None)?;
    if current["send_available"] != true {
        return Err(
            "OpenCode is stopped, working or waiting for an exact question; no message was sent"
                .into(),
        );
    }
    let mut receipt = json!({"request_id":request["request_id"],"name":record["name"],"run_id":record["run_id"],"conversation_id":record["conversation_id"],"digest":digest,"native_message_id":message_id,"text":text,"submitted_text":text,"submitted_at":now(),"at":now(),"status":"pending","attachments":[],"transport":"native_opencode"});
    atomic(&path, &receipt.to_string())?;
    let answer = rpc(
        record,
        "POST",
        &endpoint(record, "/prompt"),
        Some(&json!({"id":message_id,"prompt":{"text":text},"delivery":"queue"})),
    )?;
    if answer["data"]["id"] != message_id
        || answer["data"]["sessionID"] != record["conversation_id"]
        || answer["data"]["prompt"]["text"] != text
        || answer["data"]["admittedSeq"].as_u64().is_none()
    {
        return Err("Native message acknowledgement did not match; delivery uncertain".into());
    }
    receipt["status"] = json!("submitted");
    receipt["native_seq"] = answer["data"]["admittedSeq"].clone();
    atomic(&path, &receipt.to_string())?;
    Ok(receipt)
}
fn answer(record: &Value, request: &Value) -> Result<Value> {
    let current = inspect(string(record, "name"), None)?;
    let card = current["pending_questions"]
        .as_array()
        .into_iter()
        .flatten()
        .find(|q| {
            q["question_id"] == request["question_id"]
                && q["question_hash"] == request["expected_question_hash"]
        })
        .ok_or("This question changed or is no longer pending")?;
    let path = root()
        .join("opencode/answers")
        .join(format!("{}.json", string(request, "request_id")));
    if path.exists() {
        return Err("This answer was already attempted; refresh its native acknowledgement".into());
    }
    let raw = request["answers"]
        .as_array()
        .ok_or("Answers are required")?;
    let mut labels = Vec::new();
    let items = card["questions"]
        .as_array()
        .ok_or("Invalid native question schema")?;
    if raw.len() != items.len() {
        return Err("Answer every question exactly once".into());
    }
    for item in items {
        let matches = raw
            .iter()
            .filter(|a| a["question_id"] == item["id"])
            .collect::<Vec<_>>();
        if matches.len() != 1 {
            return Err("Question answers are missing or duplicated".into());
        }
        let answer = matches[0];
        let selected = answer["selected_option_ids"]
            .as_array()
            .ok_or("Invalid selected answers")?;
        if item["allow_multiple"] != true && selected.len() > 1 {
            return Err("This question accepts one answer".into());
        }
        let mut values = Vec::new();
        for id in selected {
            let option = item["options"]
                .as_array()
                .into_iter()
                .flatten()
                .find(|o| o["id"] == *id)
                .ok_or("Unknown answer option")?;
            values.push(option["label"].clone());
        }
        if !string(answer, "text").is_empty() {
            if item["allow_other"] != true {
                return Err("Choose an offered permission outcome".into());
            }
            values.push(answer["text"].clone());
        }
        if values.is_empty() {
            return Err("An answer is required".into());
        }
        labels.push(json!(values));
    }
    let id = string(card, "question_id");
    let (suffix, body) = if card["kind"] == "opencode_permission" {
        let id = raw[0]["selected_option_ids"][0]
            .as_str()
            .ok_or("Choose a permission outcome")?;
        if raw[0]["selected_option_ids"].as_array().unwrap().len() != 1 {
            return Err("Choose one permission outcome".into());
        }
        (
            format!("/permission/{}/reply", string(card, "question_id")),
            json!({"reply":id}),
        )
    } else {
        (format!("/question/{id}/reply"), json!({"answers":labels}))
    };
    let mut receipt = json!({"request_id":request["request_id"],"name":record["name"],"run_id":record["run_id"],"conversation_id":record["conversation_id"],"question_id":request["question_id"],"question_hash":request["expected_question_hash"],"status":"pending"});
    atomic(&path, &receipt.to_string())?;
    rpc(record, "POST", &endpoint(record, &suffix), Some(&body))?;
    receipt["status"] = json!("answered");
    atomic(&path, &receipt.to_string())?;
    Ok(receipt)
}
pub(crate) fn interrupt(record: &Value) -> Result<()> {
    let _guard = lock(Some(&root().join("opencode/bindings.lock")))?;
    let original = binding(string(record, "name"))?;
    if record["run_id"] != original["run_id"]
        || record["conversation_id"] != original["conversation_id"]
    {
        return Err("OpenCode turn identity changed".into());
    }
    let current = inspect(string(record, "name"), None)?;
    if current["interrupt_supported"] != true || current["turn_started"] != record["turn_started"] {
        return Err("OpenCode turn changed before interruption".into());
    }
    rpc(
        &original,
        "POST",
        &endpoint(&original, "/interrupt"),
        Some(&json!({})),
    )?;
    Ok(())
}
fn action(
    record: &mut Value,
    action: &str,
    new_name: Option<&str>,
    scope: Option<&session_action::Scope>,
) -> Result<()> {
    if !["pause", "resume", "rename", "forget"].contains(&action) {
        return Err("This OpenCode lifecycle action is not supported".into());
    }
    if let Some(scope) = scope {
        scope.check(record)?;
    }
    if action == "resume" {
        ensure_host(record)?;
    }
    let current = inspect(string(record, "name"), None)?;
    if !current["allowed_actions"]
        .as_array()
        .is_some_and(|values| values.iter().any(|v| v == action))
    {
        return Err("This action is unavailable for the exact OpenCode session".into());
    }
    let old = string(record, "name").to_owned();
    if action == "rename" {
        let new = new_name.ok_or("Choose a new OpenCode name")?;
        let a = old.split('/').collect::<Vec<_>>();
        let b = new.split('/').collect::<Vec<_>>();
        if a.len() != 3
            || b.len() != 3
            || a[..2] != b[..2]
            || b.iter().any(|p| {
                p.is_empty()
                    || p.trim() != *p
                    || p.contains([':', '.'])
                    || p.chars().any(char::is_control)
            })
        {
            return Err("Rename must preserve provider and project".into());
        }
        if new != old && exists(new) {
            return Err("This name is already used".into());
        }
        if let Some(scope) = scope {
            scope.changing();
        }
        let renamed = rpc(
            record,
            "PATCH",
            &format!("/session/{}", string(record, "conversation_id")),
            Some(&json!({"title":b[2]})),
        )?;
        if renamed["id"] != record["conversation_id"] || renamed["title"] != b[2] {
            return Err(
                "Native OpenCode rename acknowledgement did not match; inspect before retrying"
                    .into(),
            );
        }
        record["name"] = json!(new);
    } else if let Some(scope) = scope {
        scope.changing();
    }
    match action {
        "pause" => record["paused"] = json!(true),
        "resume" => {
            record["paused"] = json!(false);
            record["run_id"] = json!(uuid::Uuid::new_v4().to_string());
            record.as_object_mut().unwrap().remove("creation_pending");
        }
        "forget" => {
            archive::remove_synced(&binding_path(&old))?;
            return Ok(());
        }
        _ => {}
    }
    save(record)?;
    if action == "rename" && new_name != Some(old.as_str()) {
        archive::remove_synced(&binding_path(&old))?;
    }
    if let Some(scope) = scope {
        scope.result_record(record);
    }
    Ok(())
}
pub(super) fn session_action(
    name: &str,
    kind: &str,
    new_name: Option<&str>,
    scope: &session_action::Scope,
) -> Result<()> {
    let _guard = lock(Some(&root().join("opencode/bindings.lock")))?;
    let mut record = binding(name)?;
    action(&mut record, kind, new_name, Some(scope))
}
pub(crate) fn dispatch(command: &str, args: &[String]) -> Result<i32> {
    let name = args.first().ok_or("Choose an OpenCode session")?;
    let _guard = if command != "inspect" && command != "exists" {
        Some(lock(Some(&root().join("opencode/bindings.lock")))?)
    } else {
        None
    };
    let mut record = binding(name)?;
    match command {
        "exists" => {}
        "inspect" => {
            if args
                .windows(2)
                .any(|w| w[0] == "--archive" || w[0] == "--agent")
            {
                return Err("OpenCode archive/child inspection is not supported".into());
            }
            println!("{}", inspect(name, None)?);
        }
        "send" => {
            let request = checked_request(&record)?;
            println!("{}", send(&record, &request)?);
        }
        "answer" => {
            let request = checked_request(&record)?;
            println!("{}", answer(&record, &request)?);
        }
        "settings" => {
            let request = checked_request(&record)?;
            if inspect(name, None)?["settings_change_supported"] != true {
                return Err("OpenCode must be live and idle before changing its model".into());
            }
            if !string(&request, "effort").is_empty() {
                return Err("OpenCode effort controls are not yet qualified".into());
            }
            let model = offered_model(string(&request, "model"))?;
            let path = root()
                .join("opencode/settings")
                .join(format!("{}.json", string(&request, "request_id")));
            if path.exists() {
                return Err(
                    "This model change was already attempted; inspect its native state".into(),
                );
            }
            atomic(&path, &json!({"status":"pending"}).to_string())?;
            rpc(
                &record,
                "POST",
                &endpoint(&record, "/model"),
                Some(&json!({"model":model})),
            )?;
            let verified_model = verify_session(&record)?["model"].clone();
            if verified_model["id"] != model["id"]
                || verified_model["providerID"] != model["providerID"]
            {
                return Err(
                    "Native OpenCode model change was not confirmed; inspect before retrying"
                        .into(),
                );
            }
            println!(
                "{}",
                json!({"status":"applied","scope":"session","request_id":request["request_id"],"name":name,"run_id":record["run_id"],"conversation_id":record["conversation_id"],"model":request["model"]})
            );
        }
        "pause" | "resume" | "rename" | "forget" => {
            if args.iter().any(|v| v == "--dry-run") {
                println!("{command} native OpenCode {name}");
                return Ok(0);
            }
            if args.iter().any(|v| v == "--archive") {
                return Err("OpenCode archives are not supported".into());
            }
            action(
                &mut record,
                command,
                args.get(1)
                    .filter(|v| !v.starts_with('-'))
                    .map(String::as_str),
                None,
            )?;
        }
        _ => {
            return Err(format!(
                "{command} is not supported for native OpenCode sessions"
            ))
        }
    }
    Ok(0)
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn jsonc_preserves_literal_urls_and_handles_comments_and_trailing_commas() {
        let value=configuration_json(br#"{ // visible model config
          "model":"ollama/qwen3-coder:30b", /* comment */
          "provider":{"ollama":{"options":{"baseURL":"http://127.0.0.1:11434/v1"},"models":{"qwen3-coder:30b":{"name":"local",},},},},
        }"#).unwrap();
        assert_eq!(
            value["provider"]["ollama"]["options"]["baseURL"],
            "http://127.0.0.1:11434/v1"
        );
        assert_eq!(value["model"], "ollama/qwen3-coder:30b");
        assert!(configuration_json(b"{ /* unclosed").is_err());
    }
    #[test]
    fn chunked_native_response_is_bounded_and_complete() {
        assert_eq!(
            decode_response(
                b"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n7\r\n{\"a\":1}\r\n0\r\n\r\n"
            )
            .unwrap(),
            json!({"a":1})
        );
        assert!(decode_response(b"HTTP/1.1 200 OK\r\nContent-Length: 20\r\n\r\n{}").is_err());
    }
    #[test]
    fn partial_reads_cannot_extend_the_absolute_native_deadline() {
        let listener = TcpListener::bind(("127.0.0.1", 0)).unwrap();
        let port = listener.local_addr().unwrap().port();
        let worker = std::thread::spawn(move || {
            let (mut peer, _) = listener.accept().unwrap();
            let mut request = [0u8; 2048];
            let _ = peer.read(&mut request);
            peer.write_all(b"HTTP/1.1 200 OK\r\nContent-Length: 4096\r\n\r\n")
                .unwrap();
            for _ in 0..30 {
                if peer.write_all(b"n").is_err() {
                    break;
                }
                std::thread::sleep(Duration::from_millis(20));
            }
        });
        let started = Instant::now();
        let result = http_until(
            &json!({"port":port,"password":"fixture"}),
            "GET",
            "/fixture",
            None,
            started + Duration::from_millis(120),
        );
        let elapsed = started.elapsed();
        assert!(result.is_err());
        assert!(elapsed < Duration::from_millis(500), "{elapsed:?}");
        worker.join().unwrap();
    }
    #[test]
    fn malformed_native_http_never_acknowledges_mutations() {
        for raw in [
            b"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n4\r\nnull\r\n0\r\n".as_slice(),
            b"HTTP/1.1 200 OK\r\nContent-Length: junk\r\n\r\nnull",
            b"HTTP/1.1 200 OK\r\nContent-Length: 4\r\nTransfer-Encoding: chunked\r\n\r\n4\r\nnull\r\n0\r\n\r\n",
            b"HTTP/1.1 200 OK\r\nContent-Length: 4\r\nContent-Length: 4\r\n\r\nnull",
            b"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n4\r\nnullXX0\r\n\r\n",
        ] {assert!(decode_response(raw).is_err(),"{}",String::from_utf8_lossy(raw));}
        assert_eq!(
            decode_response(b"HTTP/1.1 204 No Content\r\n\r\n").unwrap(),
            Value::Null
        );
        assert_eq!(decode_response(b"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n4\r\nnull\r\n0\r\nX-Native: yes\r\n\r\n").unwrap(),Value::Null);
    }
    #[test]
    fn native_states_and_question_identity_do_not_depend_on_terminal() {
        let record = json!({"name":"opencode/test/one","run_id":"run","conversation_id":"ses_one"});
        let session = json!({"model":{"id":"local","providerID":"fixture"}});
        let pending = json!([{"id":"que_one","sessionID":"ses_one","questions":[{"question":"Pick","header":"Pick","options":[{"label":"One"}]}]}]);
        let idle = normalize(
            &record,
            &session,
            false,
            &[],
            &Value::Null,
            &Value::Null,
            true,
        );
        assert_eq!(idle["phase"], "idle");
        assert_eq!(idle["terminal_supported"], false);
        let working = normalize(
            &record,
            &session,
            true,
            &[],
            &Value::Null,
            &Value::Null,
            true,
        );
        assert_eq!(working["phase"], "working");
        assert_eq!(working["send_available"], false);
        let input = normalize(&record, &session, true, &[], &pending, &Value::Null, true);
        assert_eq!(input["phase"], "input");
        assert_eq!(input["pending_questions"][0]["answerable"], true);
        assert_ne!(input["attention_id"], idle["attention_id"]);
    }
    #[test]
    fn exact_native_history_has_stable_ids_and_literal_text() {
        let native = vec![
            json!({"type":"session.next.prompted","durable":{"seq":2},"data":{"timestamp":1000,"messageID":"msg_one","prompt":{"text":"literal\\\ntext"}}}),
            json!({"type":"session.next.text.ended","durable":{"seq":7},"data":{"timestamp":2000,"assistantMessageID":"msg_two","text":"answer"}}),
        ];
        let events = event_rows(&native);
        assert_eq!(events[0]["detail"], "literal\\\ntext");
        assert_eq!(events[1]["reply_id"], "msg_two");
        assert_eq!(events[1]["seq"], 7);
    }
}
