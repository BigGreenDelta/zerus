//! OpenCode 2's shared per-user background service: discovery through its
//! registration file, the version and API gate, and authenticated loopback
//! HTTP. Zerus never stops, restarts or replaces this service.
use super::super::*;
use base64::{engine::general_purpose::STANDARD, Engine};
use sha2::{Digest, Sha256};
use std::fs;
use std::io::{Read, Write};
use std::net::TcpStream;
use std::process::{Command, Stdio};
use std::time::{Duration, Instant};

pub(crate) const MINIMUM: (u64, u64, u64) = (2, 0, 32);

/// Method (lowercase), path template, request fields the adapter sends.
pub(super) const REQUIRED: &[(&str, &str, &[&str])] = &[
    ("get", "/api/info", &[]),
    ("get", "/api/session/active", &[]),
    (
        "post",
        "/api/session",
        &["id", "location", "model", "title", "metadata"],
    ),
    ("get", "/api/session/{sessionID}", &[]),
    ("patch", "/api/session/{sessionID}", &["title"]),
    (
        "post",
        "/api/session/{sessionID}/prompt",
        &["id", "text", "delivery"],
    ),
    ("get", "/api/session/{sessionID}/message", &[]),
    ("get", "/api/session/{sessionID}/message/{messageID}", &[]),
    ("get", "/api/session/{sessionID}/inbox", &[]),
    ("delete", "/api/session/{sessionID}/inbox/{inboxID}", &[]),
    ("post", "/api/session/{sessionID}/interrupt", &[]),
    ("post", "/api/session/{sessionID}/model", &["model"]),
    ("get", "/api/session/{sessionID}/form", &[]),
    (
        "post",
        "/api/session/{sessionID}/form/{formID}/reply",
        &["answer"],
    ),
    ("get", "/api/session/{sessionID}/permission", &[]),
    (
        "post",
        "/api/session/{sessionID}/permission/{requestID}/reply",
        &["decision"],
    ),
    ("get", "/api/model", &[]),
    ("get", "/api/experimental/migration/v1", &[]),
];

pub(super) struct Registration {
    pub id: String,
    pub version: String,
    pub port: u16,
    pub pid: u32,
    pub password: Option<String>,
}

pub(super) fn registration_path() -> PathBuf {
    registration_path_from(nonempty_env("XDG_STATE_HOME").as_deref(), &home())
}

pub(super) fn registration_path_from(state_home: Option<&str>, home: &Path) -> PathBuf {
    state_home
        .filter(|s| !s.is_empty())
        .map(PathBuf::from)
        .unwrap_or_else(|| home.join(".local/state"))
        .join("opencode/service.json")
}

pub(super) fn parse_registration(bytes: &[u8]) -> Result<Registration> {
    if bytes.len() > 16384 {
        return Err("Invalid OpenCode service registration".into());
    }
    let value: Value =
        serde_json::from_slice(bytes).map_err(|_| "Invalid OpenCode service registration")?;
    let text = |key: &str, max: usize| {
        value[key]
            .as_str()
            .filter(|s| !s.is_empty() && s.len() <= max && !s.chars().any(char::is_control))
    };
    let (Some(id), Some(version), Some(url)) =
        (text("id", 128), text("version", 64), text("url", 64))
    else {
        return Err("Invalid OpenCode service registration".into());
    };
    let rest = url
        .strip_prefix("http://127.0.0.1:")
        .or_else(|| url.strip_prefix("http://localhost:"));
    let port = rest
        .map(|r| r.trim_end_matches('/'))
        .filter(|r| !r.is_empty() && r.bytes().all(|b| b.is_ascii_digit()))
        .and_then(|r| r.parse::<u16>().ok())
        .filter(|p| *p > 0)
        .ok_or("OpenCode service is not on loopback")?;
    let pid = value["pid"]
        .as_u64()
        .filter(|p| *p > 1 && *p <= u32::MAX as u64)
        .ok_or("Invalid OpenCode service registration")? as u32;
    let password = match &value["password"] {
        Value::Null => None,
        Value::String(p) if !p.is_empty() && p.len() <= 512 => Some(p.clone()),
        _ => return Err("Invalid OpenCode service registration".into()),
    };
    Ok(Registration {
        id: id.into(),
        version: version.into(),
        port,
        pid,
        password,
    })
}

pub(super) fn version_at_least(text: &str, minimum: (u64, u64, u64)) -> bool {
    let core = text.trim().trim_start_matches('v');
    if core.contains(['-', '+']) {
        return false;
    }
    let parts: Vec<u64> = core
        .split('.')
        .map(|p| p.parse::<u64>())
        .collect::<std::result::Result<_, _>>()
        .unwrap_or_default();
    parts.len() == 3 && parts[0] == minimum.0 && (parts[1], parts[2]) >= (minimum.1, minimum.2)
}

pub(super) fn missing_operations(openapi: &Value) -> Vec<String> {
    let mut missing = Vec::new();
    for (method, path, fields) in REQUIRED {
        let operation = &openapi["paths"][*path][*method];
        if !operation.is_object() {
            missing.push(format!("{} {path}", method.to_uppercase()));
            continue;
        }
        let schema = &operation["requestBody"]["content"]["application/json"]["schema"];
        for field in *fields {
            if !schema_has_field(openapi, schema, field, 8) {
                missing.push(format!("{} {path} field {field}", method.to_uppercase()));
            }
        }
    }
    missing
}

/// Whether a request schema offers `field`: inline properties, local
/// `#/components/schemas/<Name>` references (bounded depth, so cycles end),
/// `allOf` merged, and any `anyOf`/`oneOf` variant.
fn schema_has_field(openapi: &Value, schema: &Value, field: &str, depth: u8) -> bool {
    if depth == 0 || !schema.is_object() {
        return false;
    }
    if let Some(reference) = schema["$ref"].as_str() {
        let Some(name) = reference.strip_prefix("#/components/schemas/") else {
            return false;
        };
        return schema_has_field(
            openapi,
            &openapi["components"]["schemas"][name],
            field,
            depth - 1,
        );
    }
    schema["properties"].get(field).is_some()
        || ["allOf", "anyOf", "oneOf"].iter().any(|key| {
            schema[*key].as_array().is_some_and(|variants| {
                variants
                    .iter()
                    .any(|v| schema_has_field(openapi, v, field, depth - 1))
            })
        })
}

pub(super) struct Response {
    pub status: u16,
    pub body: Value,
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
pub(super) fn decode_response(bytes: &[u8]) -> Result<Response> {
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
    let value = if body.is_empty() {
        Value::Null
    } else {
        serde_json::from_slice(body).unwrap_or_else(|_| {
            Value::String(String::from_utf8_lossy(&body[..body.len().min(4096)]).into_owned())
        })
    };
    Ok(Response {
        status: framing.status,
        body: value,
    })
}

pub(super) fn request(
    reg: &Registration,
    method: &str,
    path: &str,
    body: Option<&Value>,
    deadline: Instant,
) -> Result<Response> {
    let address = std::net::SocketAddr::from(([127, 0, 0, 1], reg.port));
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
    let auth = reg
        .password
        .as_ref()
        .map(|p| {
            format!(
                "Authorization: Basic {}\r\n",
                STANDARD.encode(format!("opencode:{p}"))
            )
        })
        .unwrap_or_default();
    let headers = format!(
        "{method} {path} HTTP/1.1\r\nHost: 127.0.0.1\r\n{auth}Content-Type: application/json\r\nContent-Length: {}\r\nConnection: close\r\n\r\n",
        body.len()
    );
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

pub(super) struct Service {
    pub reg: Registration,
}

impl Service {
    pub(super) fn raw(&self, method: &str, path: &str, body: Option<&Value>) -> Result<Response> {
        request(
            &self.reg,
            method,
            path,
            body,
            Instant::now() + Duration::from_secs(10),
        )
    }

    pub(super) fn call(&self, method: &str, path: &str, body: Option<&Value>) -> Result<Value> {
        let response = self.raw(method, path, body)?;
        if (200..300).contains(&response.status) {
            return Ok(response.body);
        }
        let reason = response.body["message"]
            .as_str()
            .or(response.body["data"]["message"].as_str())
            .unwrap_or("Native operation refused");
        Err(format!(
            "OpenCode HTTP {}: {}",
            response.status,
            journal::clipped(&json!(reason), 300)
        ))
    }
}

pub(super) enum Unavailable {
    NotRunning,
    Disabled,
    Incompatible(String),
    Unreachable(String),
}

impl Unavailable {
    pub(super) fn reason(&self, computer: &str) -> String {
        match self {
            Unavailable::NotRunning => format!("OpenCode is not running on {computer}"),
            Unavailable::Disabled => {
                format!("The OpenCode background service is disabled on {computer}")
            }
            Unavailable::Incompatible(why) | Unavailable::Unreachable(why) => why.clone(),
        }
    }
}

/// The registration file's bytes, read with the same bound the parser
/// enforces, or None when there is no readable file.
fn read_registration() -> Option<Vec<u8>> {
    let file = fs::File::open(registration_path()).ok()?;
    let mut bytes = Vec::new();
    file.take(16385).read_to_end(&mut bytes).ok()?;
    Some(bytes)
}

pub(super) fn connect() -> std::result::Result<Service, Unavailable> {
    let bytes = read_registration();
    let Some(bytes) = bytes else {
        return Err(if service_disabled() {
            Unavailable::Disabled
        } else {
            Unavailable::NotRunning
        });
    };
    let reg = parse_registration(&bytes).map_err(Unavailable::Unreachable)?;
    if !crate::platform::process_alive(reg.pid) {
        return Err(Unavailable::NotRunning);
    }
    let info = request(
        &reg,
        "GET",
        "/api/info",
        None,
        Instant::now() + Duration::from_secs(2),
    )
    .map_err(|_| Unavailable::Unreachable("OpenCode is not responding".into()))?;
    if info.status != 200 {
        return Err(Unavailable::Unreachable(
            "OpenCode is not responding correctly".into(),
        ));
    }
    let version = clipped_version(info.body["version"].as_str().unwrap_or(""));
    let version = version.as_str();
    if !version_at_least(version, MINIMUM) {
        return Err(Unavailable::Incompatible(format!(
            "OpenCode {version} is not supported; install OpenCode {}.{}.{} or newer 2.x",
            MINIMUM.0, MINIMUM.1, MINIMUM.2
        )));
    }
    let service = Service { reg };
    let missing = compatibility(&service).map_err(Unavailable::Unreachable)?;
    if let Some(first) = missing.first() {
        return Err(Unavailable::Incompatible(format!(
            "OpenCode {version} lacks {first}; update OpenCode or Zerus"
        )));
    }
    Ok(service)
}

/// Digest of the requirement table, so a Zerus with new requirements
/// re-checks a service whose verdict was cached by an older one.
fn requirement_digest() -> String {
    format!(
        "{:x}",
        Sha256::digest(json!(REQUIRED).to_string().as_bytes())
    )
}

fn verdict(openapi: &Value) -> Result<Vec<String>> {
    if !openapi["paths"].is_object() {
        return Err("OpenCode returned an invalid API description".into());
    }
    Ok(missing_operations(openapi))
}

/// The OpenAPI verdict, cached per service identity and requirement digest in
/// private state. The cache write is best effort.
fn compatibility(service: &Service) -> Result<Vec<String>> {
    compatibility_in(
        &root().join("opencode/compat"),
        &service.reg,
        &requirement_digest(),
        || service.call("GET", "/openapi.json", None),
    )
}

fn compatibility_in(
    dir: &Path,
    reg: &Registration,
    digest: &str,
    fetch: impl FnOnce() -> Result<Value>,
) -> Result<Vec<String>> {
    let key = format!(
        "{:x}",
        Sha256::digest(
            json!([reg.id, reg.pid, reg.version, reg.port, digest])
                .to_string()
                .as_bytes()
        )
    );
    let cache = dir.join(format!("{key}.json"));
    if let Ok(bytes) = fs::read(&cache) {
        if let Ok(value) = serde_json::from_slice::<Vec<String>>(&bytes) {
            return Ok(value);
        }
    }
    let missing = verdict(&fetch()?)?;
    let _ = atomic(&cache, &json!(missing).to_string());
    Ok(missing)
}

fn clipped_version(text: &str) -> String {
    text.chars().filter(|c| !c.is_control()).take(64).collect()
}

/// OpenCode 2.0.32 keeps `disabled` in the service configuration file
/// `<config dir>/service.json` (the channel filename for the stable channel;
/// upstream `packages/cli/src/services/service-config.ts`: `filename()`,
/// `paths` and `get`/`set` for the `disabled` key).
fn service_disabled() -> bool {
    let base = nonempty_env("XDG_CONFIG_HOME")
        .map(PathBuf::from)
        .unwrap_or_else(|| home().join(".config"));
    fs::read(base.join("opencode/service.json"))
        .ok()
        .and_then(|b| serde_json::from_slice::<Value>(&b).ok())
        .is_some_and(|v| v["disabled"] == true)
}

pub(super) enum StartDecision {
    AlreadyRunning,
    Start,
    Refuse(String),
}

/// Start runs only when no registration exists or its process is gone; it
/// never runs while a registered process lives, however slow.
pub(super) fn start_decision(
    registration: Option<&Registration>,
    pid_alive: bool,
    disabled: bool,
) -> StartDecision {
    if registration.is_some() && pid_alive {
        return StartDecision::AlreadyRunning;
    }
    if disabled {
        return StartDecision::Refuse(
            "The OpenCode background service is disabled; start it in OpenCode".into(),
        );
    }
    StartDecision::Start
}

/// Variables the shared service must not inherit from `hgs`, the connector
/// or an SSH login.
pub(super) fn start_environment(vars: Vec<(String, String)>) -> Vec<(String, String)> {
    vars.into_iter()
        .filter(|(k, _)| {
            !(k.starts_with("HGS_")
                || k.starts_with("SSH_")
                || k.starts_with("OPENCODE_SERVER_")
                || k.starts_with("ZERUS_MOBILE_")
                || k == "OPENCODE_CONFIG_CONTENT")
        })
        .collect()
}

/// Connect, starting `opencode service start` only when no service runs.
/// A live registered process that does not answer is reported, never replaced.
#[allow(dead_code)]
pub(super) fn ensure_started() -> Result<Service> {
    let lock_path = root().join("opencode/service-start.lock");
    let _guard = lock(Some(&lock_path))?;
    match connect() {
        Ok(service) => return Ok(service),
        Err(Unavailable::NotRunning | Unavailable::Disabled) => {}
        Err(other) => return Err(other.reason(&crate::platform::hostname())),
    }
    let reg = read_registration().and_then(|b| parse_registration(&b).ok());
    let alive = reg
        .as_ref()
        .is_some_and(|r| crate::platform::process_alive(r.pid));
    match start_decision(reg.as_ref(), alive, service_disabled()) {
        StartDecision::AlreadyRunning => {
            return Err("OpenCode is starting; try again shortly".into())
        }
        StartDecision::Refuse(why) => return Err(why),
        StartDecision::Start => {}
    }
    let mut command = Command::new("opencode");
    #[cfg(target_os = "linux")]
    let mut runtime_dir = None;
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
                .args(["--", "opencode"]);
            runtime_dir = Some(runtime);
        }
    }
    command
        .args(["service", "start"])
        .env_clear()
        .envs(start_environment(std::env::vars().collect()))
        .current_dir(home())
        .stdin(Stdio::null())
        .stdout(Stdio::null())
        .stderr(Stdio::null());
    #[cfg(target_os = "linux")]
    if let Some(runtime) = runtime_dir {
        command.env("XDG_RUNTIME_DIR", runtime);
    }
    let mut child = crate::platform::spawn_detached(&mut command)
        .map_err(|_| "OpenCode could not be started; install OpenCode 2".to_owned())?;
    let deadline = Instant::now() + Duration::from_secs(20);
    while Instant::now() < deadline {
        if let Ok(service) = connect() {
            let _ = child.try_wait();
            return Ok(service);
        }
        std::thread::sleep(Duration::from_millis(200));
    }
    Err("OpenCode is still starting; try again shortly".into())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn registration_has_no_state_field_and_requires_loopback_and_pid() {
        let reg = parse_registration(include_bytes!(
            "../../../tests/fixtures/opencode2/registration.json"
        ))
        .unwrap();
        assert_eq!(
            (reg.port, reg.pid, reg.version.as_str()),
            (48951, 424242, "2.0.32")
        );
        assert_eq!(reg.password.as_deref(), Some("fixture-password"));
        for bad in [
            br#"{"id":"a","version":"2.0.32","url":"http://10.0.0.5:4096","pid":5}"#.as_slice(),
            br#"{"id":"a","version":"2.0.32","url":"https://127.0.0.1:4096","pid":5}"#,
            br#"{"id":"a","version":"2.0.32","url":"http://127.0.0.1:4096/x","pid":5}"#,
            br#"{"id":"a","version":"2.0.32","url":"http://127.0.0.1:4096","pid":1}"#,
            br#"{"id":"","version":"2.0.32","url":"http://127.0.0.1:4096","pid":5}"#,
        ] {
            assert!(
                parse_registration(bad).is_err(),
                "{}",
                String::from_utf8_lossy(bad)
            );
        }
        assert!(parse_registration(&vec![b' '; 20000]).is_err());
    }

    #[test]
    fn registration_follows_xdg_state_home() {
        let home = Path::new("/home/example");
        assert_eq!(
            registration_path_from(Some("/private/fixture/state"), home),
            PathBuf::from("/private/fixture/state/opencode/service.json")
        );
        assert_eq!(
            registration_path_from(None, home),
            PathBuf::from("/home/example/.local/state/opencode/service.json")
        );
        assert_eq!(
            registration_path_from(Some(""), home),
            PathBuf::from("/home/example/.local/state/opencode/service.json")
        );
    }

    #[test]
    fn versions_compare_numerically_and_reject_other_majors() {
        assert!(version_at_least("2.0.32", MINIMUM));
        assert!(version_at_least("v2.0.40", MINIMUM));
        assert!(version_at_least("2.1.0", MINIMUM));
        assert!(!version_at_least("2.0.31", MINIMUM));
        assert!(!version_at_least("1.18.35", MINIMUM));
        assert!(!version_at_least("3.0.0", MINIMUM));
        assert!(!version_at_least("2.0.33-beta.1", MINIMUM));
        assert!(!version_at_least("garbage", MINIMUM));
    }

    #[test]
    fn missing_operations_names_each_absent_route_or_field() {
        let mut openapi = json!({"paths":{}});
        for (method, path, fields) in REQUIRED {
            openapi["paths"][*path][*method] = if fields.is_empty() {
                json!({})
            } else {
                let properties: serde_json::Map<String, Value> = fields
                    .iter()
                    .map(|f| ((*f).to_owned(), json!({})))
                    .collect();
                json!({"requestBody":{"content":{"application/json":{"schema":{"properties":properties}}}}})
            };
        }
        assert!(
            missing_operations(&openapi).is_empty(),
            "{:?}",
            missing_operations(&openapi)
        );
        openapi["paths"]
            .as_object_mut()
            .unwrap()
            .remove("/api/session/{sessionID}/inbox");
        openapi["paths"]["/api/session/{sessionID}/prompt"]["post"]["requestBody"]["content"]
            ["application/json"]["schema"]["properties"]
            .as_object_mut()
            .unwrap()
            .remove("delivery");
        assert_eq!(
            missing_operations(&openapi),
            vec![
                "POST /api/session/{sessionID}/prompt field delivery".to_owned(),
                "GET /api/session/{sessionID}/inbox".to_owned(),
            ]
        );
    }

    #[test]
    fn non_success_status_is_returned_not_raised() {
        let bytes = b"HTTP/1.1 404 Not Found\r\nContent-Length: 37\r\n\r\n{\"_tag\":\"MessageNotFoundError\",\"x\":1}";
        let response = decode_response(bytes).unwrap();
        assert_eq!(response.status, 404);
        assert_eq!(response.body["_tag"], "MessageNotFoundError");
    }

    #[test]
    fn schema_refs_allof_and_variants_are_resolved() {
        let openapi = json!({
            "paths":{"/x":{"post":{"requestBody":{"content":{"application/json":{"schema":{"$ref":"#/components/schemas/Form.Reply"}}}}}}},
            "components":{"schemas":{
                "Form.Reply":{"type":"object","properties":{"answer":{}}},
                "Merged":{"allOf":[{"$ref":"#/components/schemas/Form.Reply"},{"properties":{"extra":{}}}]},
                "Either":{"oneOf":[{"properties":{"a":{}}},{"properties":{"b":{}}}]},
                "Loop":{"$ref":"#/components/schemas/Loop"}
            }}
        });
        let schema =
            &openapi["paths"]["/x"]["post"]["requestBody"]["content"]["application/json"]["schema"];
        assert!(schema_has_field(&openapi, schema, "answer", 8));
        assert!(!schema_has_field(&openapi, schema, "decision", 8));
        let merged = json!({"$ref":"#/components/schemas/Merged"});
        assert!(schema_has_field(&openapi, &merged, "answer", 8));
        assert!(schema_has_field(&openapi, &merged, "extra", 8));
        let either = json!({"$ref":"#/components/schemas/Either"});
        assert!(schema_has_field(&openapi, &either, "b", 8));
        assert!(!schema_has_field(
            &openapi,
            &json!({"$ref":"#/components/schemas/Loop"}),
            "a",
            8
        ));
        assert!(!schema_has_field(
            &openapi,
            &json!({"$ref":"https://x/y"}),
            "a",
            8
        ));
    }

    #[test]
    fn missing_field_in_a_referenced_schema_is_reported() {
        let mut openapi = json!({"paths":{}, "components":{"schemas":{
            "Body":{"allOf":[{"properties":{"answer":{}}}]},
            "Thin":{"type":"object","properties":{}}
        }}});
        for (method, path, fields) in REQUIRED {
            openapi["paths"][*path][*method] = if fields.is_empty() {
                json!({})
            } else {
                let name = if *path == "/api/session/{sessionID}/form/{formID}/reply" {
                    "Thin"
                } else {
                    "Full"
                };
                json!({"requestBody":{"content":{"application/json":{"schema":{"$ref":format!("#/components/schemas/{name}")}}}}})
            };
        }
        let properties: serde_json::Map<String, Value> = REQUIRED
            .iter()
            .flat_map(|(_, _, f)| f.iter())
            .map(|f| ((*f).to_owned(), json!({})))
            .collect();
        openapi["components"]["schemas"]["Full"] = json!({"allOf":[{"properties":properties}]});
        assert_eq!(
            missing_operations(&openapi),
            vec!["POST /api/session/{sessionID}/form/{formID}/reply field answer".to_owned()]
        );
    }

    fn temp_dir(tag: &str) -> PathBuf {
        let dir = std::env::temp_dir().join(format!("hgs-compat-{tag}-{}", std::process::id()));
        let _ = fs::remove_dir_all(&dir);
        dir
    }

    fn reg() -> Registration {
        Registration {
            id: "a".into(),
            version: "2.0.32".into(),
            port: 4096,
            pid: 5,
            password: None,
        }
    }

    #[test]
    fn requirement_digest_change_misses_the_cache() {
        let dir = temp_dir("digest");
        let body = || Ok(json!({"paths":{}}));
        let first = compatibility_in(&dir, &reg(), "d1", body).unwrap();
        assert!(!first.is_empty());
        // Same digest hits: the fetch must not run.
        assert_eq!(
            compatibility_in(&dir, &reg(), "d1", || Err("no".into())).unwrap(),
            first
        );
        // A different digest misses and fetches again.
        assert!(compatibility_in(&dir, &reg(), "d2", || Err("fetched".into())).is_err());
        let _ = fs::remove_dir_all(&dir);
    }

    #[test]
    fn invalid_api_description_is_rejected_and_not_cached() {
        let dir = temp_dir("invalid");
        for bad in [json!("html"), json!([]), json!({"paths":[]}), json!({})] {
            let err = compatibility_in(&dir, &reg(), "d", || Ok(bad)).unwrap_err();
            assert_eq!(err, "OpenCode returned an invalid API description");
        }
        assert!(!dir.exists() || fs::read_dir(&dir).unwrap().next().is_none());
        let _ = fs::remove_dir_all(&dir);
    }

    #[test]
    fn cache_write_failure_keeps_the_verified_verdict() {
        let dir = temp_dir("blocked");
        fs::write(&dir, b"a file where the directory should be").unwrap();
        let missing = compatibility_in(&dir, &reg(), "d", || Ok(json!({"paths":{}}))).unwrap();
        assert!(!missing.is_empty());
        let _ = fs::remove_file(&dir);
    }

    #[test]
    fn reported_versions_are_clipped_and_control_free() {
        let long = format!("2.0.\u{7}{}", "9".repeat(200));
        let clipped = clipped_version(&long);
        assert_eq!(clipped.chars().count(), 64);
        assert!(!clipped.chars().any(char::is_control));
    }

    #[test]
    fn real_opencode_2_0_32_description_passes_the_gate() {
        let openapi: Value = serde_json::from_slice(include_bytes!(
            "../../../tests/fixtures/opencode2/openapi-2.0.32.json"
        ))
        .unwrap();
        assert_eq!(missing_operations(&openapi), Vec::<String>::new());
        assert!(verdict(&openapi).unwrap().is_empty());
    }

    #[test]
    fn start_is_refused_while_any_registered_process_lives() {
        let reg = parse_registration(include_bytes!(
            "../../../tests/fixtures/opencode2/registration.json"
        ))
        .unwrap();
        assert!(matches!(
            start_decision(Some(&reg), true, false),
            StartDecision::AlreadyRunning
        ));
        assert!(matches!(
            start_decision(Some(&reg), true, true),
            StartDecision::AlreadyRunning
        ));
        assert!(matches!(
            start_decision(Some(&reg), false, false),
            StartDecision::Start
        ));
        assert!(matches!(
            start_decision(None, false, false),
            StartDecision::Start
        ));
        assert!(matches!(
            start_decision(None, false, true),
            StartDecision::Refuse(_)
        ));
    }

    #[test]
    fn start_environment_drops_session_ssh_and_server_variables() {
        let kept = start_environment(vec![
            ("PATH".into(), "/usr/bin".into()),
            ("HGS_SESSION".into(), "x".into()),
            ("SSH_AUTH_SOCK".into(), "/s".into()),
            ("SSH_CONNECTION".into(), "1".into()),
            ("OPENCODE_SERVER_PASSWORD".into(), "p".into()),
            ("OPENCODE_CONFIG_CONTENT".into(), "{}".into()),
            ("ZERUS_MOBILE_TOKEN".into(), "t".into()),
            ("HOME".into(), "/home/example".into()),
        ]);
        let names: Vec<_> = kept.iter().map(|(k, _)| k.as_str()).collect();
        assert_eq!(names, vec!["PATH", "HOME"]);
    }
}
