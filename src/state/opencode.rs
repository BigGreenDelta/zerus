//! OpenCode's reviewed native API, authenticated on loopback. Native hosts
//! outlive HGS and the GUI; manifests contain bindings, never provider keys.
use super::*;
#[allow(dead_code)] // the OpenCode 2 gate is consumed by later phase 1 tasks
mod catalog;
mod service;
pub(crate) use catalog::{catalog, install_state, start_from_accounts};
use sha2::{Digest, Sha256};
use std::fs::{self, OpenOptions};
use std::io::Read;
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
/// A fresh OpenCode identifier: the prefix and 26 random letters or digits.
pub(super) fn new_id(prefix: &str) -> String {
    const ALPHABET: &[u8] = b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
    let mut suffix = String::with_capacity(26);
    // Bytes 6 and 8 of a v4 UUID carry fixed version and variant bits, so
    // only the other 14 are random. 248 = 4 * 62: bytes at or above it are
    // dropped to avoid modulo bias.
    while suffix.len() < 26 {
        for (index, byte) in uuid::Uuid::new_v4().into_bytes().into_iter().enumerate() {
            if index != 6 && index != 8 && byte < 248 && suffix.len() < 26 {
                suffix.push(ALPHABET[byte as usize % ALPHABET.len()] as char);
            }
        }
    }
    format!("{prefix}{suffix}")
}
fn session_path(record: &Value, suffix: &str) -> String {
    format!("/api/session/{}{suffix}", string(record, "conversation_id"))
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
use service::{Registration, Response};
#[cfg(test)]
fn decode_response(bytes: &[u8]) -> Result<Value> {
    legacy_value(service::decode_response(bytes)?)
}
/// OpenCode 1 callers expect failures as errors and strict JSON on success.
fn legacy_value(response: Response) -> Result<Value> {
    if !(200..300).contains(&response.status) {
        let reason = response.body["message"]
            .as_str()
            .or(response.body["data"]["message"].as_str())
            .unwrap_or("Native operation refused");
        return Err(format!("OpenCode HTTP {}: {reason}", response.status));
    }
    if response.body.is_string() {
        return Err("Invalid native JSON response; delivery uncertain".into());
    }
    Ok(response.body)
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
    let registration = Registration {
        id: String::new(),
        version: String::new(),
        port: host["port"]
            .as_u64()
            .and_then(|p| u16::try_from(p).ok())
            .ok_or("Missing native endpoint")?,
        pid: 0,
        password: Some(string(host, "password").to_owned()).filter(|p| !p.is_empty()),
    };
    legacy_value(service::request(
        &registration,
        method,
        path,
        body,
        deadline,
    )?)
}
fn rpc(record: &Value, method: &str, path: &str, body: Option<&Value>) -> Result<Value> {
    http(&host(record)?, method, path, body)
}
fn endpoint(record: &Value, suffix: &str) -> String {
    format!("/api/session/{}{suffix}", string(record, "conversation_id"))
}
fn ensure_host(record: &Value) -> Result<()> {
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

/// OpenCode 1 launch and settings still call this until Task 9 rewrites them.
/// Remove in Task 9: this shim has no place in the OpenCode 2 adapter.
fn legacy_offered_model(location: &Path, value: &str) -> Result<Value> {
    let service = service::connect().map_err(|why| why.reason(&crate::platform::hostname()))?;
    catalog::offered_model(&service, location, value, "")
}
pub(crate) fn account_context(name: &str) -> Result<Value> {
    let record = binding(name)?;
    Ok(
        json!({"provider":"opencode","id":"native-opencode","home":record["agent_home"],"run_id":record["run_id"]}),
    )
}
enum Legacy {
    Current,
    Adopted,
    Unsupported,
}
/// Pure decision for a binding written before the OpenCode 2 adapter: it is
/// adopted only when the migration completed and OpenCode 2 returns exactly
/// this session in exactly this directory.
fn legacy_verdict(record: &Value, migration: &Value, session: Option<&Value>) -> Legacy {
    if record["service"] == "opencode2" {
        return Legacy::Current;
    }
    let (Some(id), Some(cwd)) = (
        record["conversation_id"].as_str().filter(|v| !v.is_empty()),
        record["cwd"].as_str().filter(|v| !v.is_empty()),
    ) else {
        return Legacy::Unsupported;
    };
    let data = session.map(|value| &value["data"]);
    if migration["status"] == "completed"
        && data.is_some_and(|data| {
            data["id"].as_str() == Some(id) && data["location"]["directory"].as_str() == Some(cwd)
        })
    {
        Legacy::Adopted
    } else {
        Legacy::Unsupported
    }
}
type Reply = std::result::Result<Response, String>;
fn refusal_text(response: &Response) -> String {
    let reason = response.body["message"]
        .as_str()
        .or(response.body["data"]["message"].as_str())
        .unwrap_or("Native operation refused");
    format!("OpenCode HTTP {}: {reason}", response.status)
}
fn successful(response: &Response) -> bool {
    (200..300).contains(&response.status)
}
/// Whether the migration finished. Only a successful reply answers; any
/// transport or HTTP failure is an error, never "not migrated".
fn migration_completed(reply: &Reply) -> Result<bool> {
    match reply {
        Err(why) => Err(why.clone()),
        Ok(response) if !successful(response) => Err(refusal_text(response)),
        Ok(response) => {
            let body = if response.body["data"].is_object() {
                &response.body["data"]
            } else {
                &response.body
            };
            Ok(body["status"] == "completed")
        }
    }
}
/// The verdict once the migration completed: a missing session is
/// Unsupported, any other failure is an error.
fn settled_verdict(record: &Value, session: &Reply) -> Result<Legacy> {
    match session {
        Err(why) => Err(why.clone()),
        Ok(response) if response.status == 404 => Ok(Legacy::Unsupported),
        Ok(response) if !successful(response) => Err(refusal_text(response)),
        Ok(response) => Ok(legacy_verdict(
            record,
            &json!({"status": "completed"}),
            Some(&response.body),
        )),
    }
}
/// The mapping from the two native replies to a verdict. The flag says the
/// verdict is settled for this service (the migration finished) and may be
/// cached; the session is requested only after a completed migration.
fn legacy_decision(
    record: &Value,
    migration: &Reply,
    session: impl FnOnce() -> Reply,
) -> Result<(Legacy, bool)> {
    if record["service"] == "opencode2" {
        return Ok((Legacy::Current, false));
    }
    if !migration_completed(migration)? {
        return Ok((Legacy::Unsupported, false));
    }
    settled_verdict(record, &session()).map(|verdict| (verdict, true))
}
/// `migration` is fetched at most once per caller and only when needed.
fn legacy_state(
    service: &service::Service,
    record: &Value,
    migration: &mut Option<Reply>,
) -> Result<(Legacy, bool)> {
    if record["service"] == "opencode2" {
        return Ok((Legacy::Current, false));
    }
    let reply =
        migration.get_or_insert_with(|| service.raw("GET", "/api/experimental/migration/v1", None));
    legacy_decision(record, reply, || {
        service.raw("GET", &session_path(record, ""), None)
    })
}
/// Drop OpenCode 1 fields and mark the binding as an OpenCode 2 binding.
fn modernize(record: &mut Value) {
    let map = record.as_object_mut().unwrap();
    for key in [
        "paused",
        "native_data_home",
        "native_config_file",
        "creation_pending",
        "creation_body",
    ] {
        map.remove(key);
    }
    record["service"] = json!("opencode2");
}
const VERDICT_LIMIT: usize = 5000;
fn verdict_path() -> PathBuf {
    root().join("opencode/legacy-verdicts.json")
}
fn verdict_key(record: &Value, registration: &service::Registration) -> String {
    format!(
        "{}\n{}\n{}\n{}",
        string(record, "name"),
        string(record, "conversation_id"),
        registration.id,
        registration.pid
    )
}
fn load_verdicts() -> Vec<String> {
    let Ok(file) = fs::File::open(verdict_path()) else {
        return Vec::new();
    };
    let mut bytes = Vec::new();
    if file
        .take(4 * 1024 * 1024 + 1)
        .read_to_end(&mut bytes)
        .is_err()
        || bytes.len() > 4 * 1024 * 1024
    {
        return Vec::new();
    }
    serde_json::from_slice::<Value>(&bytes)
        .ok()
        .and_then(|value| {
            value["unsupported"].as_array().map(|items| {
                items
                    .iter()
                    .filter_map(|item| item.as_str().map(str::to_owned))
                    .take(VERDICT_LIMIT)
                    .collect()
            })
        })
        .unwrap_or_default()
}
fn save_verdicts(keys: &[String]) {
    let path = verdict_path();
    if let Some(parent) = path.parent() {
        if private_dir(parent).is_err() {
            return;
        }
    }
    let _ = atomic(&path, &json!({"unsupported": keys}).to_string());
}
/// Rewrite an adopted binding so it is never checked again. Skipped when the
/// binding lock is busy; the next poll tries again.
fn adopt_binding(name: &str, conversation_id: &str) {
    let deadline = Instant::now() + Duration::from_millis(200);
    let Ok(Some(_guard)) = lock_until(&root().join("opencode/bindings.lock"), deadline) else {
        return;
    };
    if let Ok(mut record) = binding(name) {
        if record["conversation_id"] == conversation_id {
            modernize(&mut record);
            let _ = save(&record);
        }
    }
}
enum Refusal {
    /// The service definitely refused the creation (HTTP 4xx).
    Definite(String),
    Other(String),
}
fn creation_reply(
    reply: Reply,
    acknowledged: &dyn Fn(&Value) -> bool,
) -> std::result::Result<(), Refusal> {
    match reply {
        Ok(response) if successful(&response) => {
            if acknowledged(&response.body) {
                Ok(())
            } else {
                Err(Refusal::Other(
                    "OpenCode creation acknowledgement did not match; inspect before resuming"
                        .into(),
                ))
            }
        }
        Ok(response) if (400..500).contains(&response.status) => {
            Err(Refusal::Definite(refusal_text(&response)))
        }
        Ok(response) => Err(Refusal::Other(refusal_text(&response))),
        Err(why) => Err(Refusal::Other(why)),
    }
}
/// After a lost or uncertain creation reply: ask for this exact ID, and only
/// when it does not exist POST the same body once (the ID makes it idempotent).
fn reconcile_creation(
    get: &mut dyn FnMut() -> Reply,
    post: &mut dyn FnMut() -> Reply,
    acknowledged: &dyn Fn(&Value) -> bool,
) -> std::result::Result<(), Refusal> {
    if let Ok(response) = get() {
        if successful(&response) {
            return creation_reply(Ok(response), acknowledged);
        }
    }
    creation_reply(post(), acknowledged)
}
/// The first POST; a definite refusal or acknowledgement ends it, anything
/// uncertain is reconciled.
fn create_session(
    get: &mut dyn FnMut() -> Reply,
    post: &mut dyn FnMut() -> Reply,
    acknowledged: &dyn Fn(&Value) -> bool,
) -> std::result::Result<(), Refusal> {
    match post() {
        Ok(response) if successful(&response) || (400..500).contains(&response.status) => {
            creation_reply(Ok(response), acknowledged)
        }
        _ => reconcile_creation(get, post, acknowledged),
    }
}
pub(crate) fn launch(
    name: &str,
    cwd: &str,
    launch_id: Option<&str>,
    model: Option<&str>,
    effort: Option<&str>,
    dry: bool,
    new_only: bool,
) -> Result<()> {
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
    let present = exists(name);
    if present && new_only {
        return Err("This OpenCode session name already exists".into());
    }
    let cwd = if present {
        None
    } else {
        let cwd = crate::platform::canonical(cwd).map_err(|e| e.to_string())?;
        if !cwd.is_dir() {
            return Err("OpenCode requires an existing directory".into());
        }
        Some(cwd)
    };
    let service = service::ensure_started()?;
    if present {
        let mut record = binding(name)?;
        if resumes_pending(&record) {
            return resume_pending(&service, record);
        }
        let (verdict, _) = legacy_state(&service, &record, &mut None)?;
        let adopted = match verdict {
            Legacy::Current => false,
            Legacy::Adopted => true,
            Legacy::Unsupported => {
                return Err(
                    "This conversation belongs to OpenCode 1, which Zerus no longer supports"
                        .into(),
                )
            }
        };
        verify_session(&service, &record)?;
        if adopted {
            modernize(&mut record);
            save(&record)?;
        }
        return Ok(());
    }
    let cwd = cwd.unwrap();
    let selected = model
        .map(|value| catalog::offered_model(&service, &cwd, value, effort.unwrap_or("")))
        .transpose()?;
    let mut record = json!({"backend":"opencode","service":"opencode2","agent":"opencode","name":name,"cwd":cwd,"launch_id":launch_id,"run_id":uuid::Uuid::new_v4().to_string(),"conversation_id":new_id("ses_"),"created":now(),"agent_home":config_home(),"creation_pending":true});
    let mut body = creation_body(&record);
    if let Some(model) = selected {
        body["model"] = model;
    }
    // Persist the chosen native ID and exact body before the first creation
    // attempt so one launch never has two identities.
    record["creation_body"] = body;
    save(&record)?;
    finish_creation(&service, record, true)
}
fn creation_body(record: &Value) -> Value {
    let name = string(record, "name");
    let tag = name.rsplit('/').next().unwrap_or(name);
    json!({"id":record["conversation_id"],"location":{"directory":record["cwd"]},"title":tag,"metadata":{"zerus":{"run_id":record["run_id"],"launch_id":record["launch_id"]}}})
}
/// Only OpenCode 2 bindings are reconciled against the service; an OpenCode 1
/// binding goes through the legacy verdict and is never POSTed or deleted.
fn resumes_pending(record: &Value) -> bool {
    record["creation_pending"] == true && record["service"] == "opencode2"
}
/// A binding left with creation_pending (crash or lost reply) is reconciled
/// instead of blocking its name.
fn resume_pending(service: &service::Service, record: Value) -> Result<()> {
    finish_creation(service, record, false)
}
fn finish_creation(service: &service::Service, mut record: Value, first: bool) -> Result<()> {
    let body = if record["creation_body"].is_object() {
        record["creation_body"].clone()
    } else {
        creation_body(&record)
    };
    let acknowledged = |value: &Value| {
        value["data"]["id"] == record["conversation_id"]
            && value["data"]["location"]["directory"] == record["cwd"]
    };
    let path = session_path(&record, "");
    let mut get = || service.raw("GET", &path, None);
    let mut post = || service.raw("POST", "/api/session", Some(&body));
    let result = if first {
        create_session(&mut get, &mut post, &acknowledged)
    } else {
        reconcile_creation(&mut get, &mut post, &acknowledged)
    };
    match result {
        Ok(()) => {
            let map = record.as_object_mut().unwrap();
            map.remove("creation_pending");
            map.remove("creation_body");
            save(&record)
        }
        Err(Refusal::Definite(reason)) => {
            let _ = fs::remove_file(binding_path(string(&record, "name")));
            Err(reason)
        }
        Err(Refusal::Other(reason)) => Err(reason),
    }
}
fn verify_session(service: &service::Service, record: &Value) -> Result<Value> {
    let result = service.call("GET", &session_path(record, ""), None)?;
    if result["data"]["id"] != record["conversation_id"]
        || result["data"]["location"]["directory"] != record["cwd"]
    {
        return Err("The exact native OpenCode session or directory changed".into());
    }
    Ok(result["data"].clone())
}
/// Remove with the OpenCode 1 callers in Tasks 5-9.
fn legacy_verify_session(record: &Value) -> Result<Value> {
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
// The reviewed release creates children only with its classic `task` tool,
// for example in the OpenCode TUI or `opencode run` sharing this data home.
// Their own native messages are the only exact turn evidence the API host has;
// it does not own those processes, so it never reports such a child as working.
const CHILD_ROSTER: usize = 64;
const CHILD_HISTORY: usize = 200;

fn child_unknown(reason: &str) -> Value {
    json!({"state":"unknown","reason":reason})
}

/// Exact children by native `parentID`. Any contradictory row makes the whole
/// membership unavailable rather than silently dropping a child.
fn children(record: &Value, deadline: Instant) -> Result<Vec<Value>> {
    let parent = string(record, "conversation_id");
    let listed = http_until(
        &host(record)?,
        "GET",
        &format!("/session/{parent}/children"),
        None,
        deadline,
    )?;
    exact_children(record, &listed)
}
fn exact_children(record: &Value, listed: &Value) -> Result<Vec<Value>> {
    let parent = string(record, "conversation_id");
    let rows = listed.as_array().ok_or("Invalid OpenCode child list")?;
    if rows.len() > 10000 {
        return Err("OpenCode child list exceeds bounds".into());
    }
    let mut seen = std::collections::BTreeSet::new();
    for row in rows {
        let id = string(row, "id");
        if !native_id(id, "ses_")
            || id == parent
            || row["parentID"] != record["conversation_id"]
            || !seen.insert(id)
        {
            return Err("OpenCode child list does not match the exact parent conversation".into());
        }
    }
    let mut rows = rows.clone();
    let created = |row: &Value| row["time"]["created"].as_f64().unwrap_or(0.0);
    rows.sort_by(|a, b| {
        created(b)
            .total_cmp(&created(a))
            .then_with(|| string(a, "id").cmp(string(b, "id")))
    });
    Ok(rows)
}

/// A child's latest turn ends exactly as the reviewed classic loop decides:
/// its newest message is a completed assistant step with a terminal finish and
/// no tool call still awaiting a follow-up step, or an abort/provider error
/// completed that step. Any other shape stays unknown: another OpenCode process
/// may still own the turn, or a crashed one abandoned it.
fn child_turn(child: &str, latest: &Value) -> Value {
    let info = &latest["info"];
    if info["sessionID"] != child || !native_id(string(info, "id"), "msg_") {
        return child_unknown("The child's native message identity did not match");
    }
    let open = "Turn end not recorded; another OpenCode process may still be running it";
    if info["role"] != "assistant" {
        return child_unknown(open);
    }
    let Some(completed) = info["time"]["completed"].as_f64().filter(|t| *t > 0.0) else {
        return child_unknown(open);
    };
    let ended = |kind: &str, detail: String| json!({"state":"finished","kind":kind,"at":completed/1000.0,"native_id":info["id"],"detail":detail});
    if info["error"].is_object() {
        if info["error"]["name"] == "MessageAbortedError" {
            return ended("turn_interrupted", "Native child turn interrupted".into());
        }
        let mut failed = ended(
            "turn_failed",
            journal::clipped(&json!(error_detail(&info["error"])), 240),
        );
        failed["error"] = json!(true);
        return failed;
    }
    let finish = string(info, "finish");
    let follow_up = latest["parts"]
        .as_array()
        .into_iter()
        .flatten()
        .any(|part| {
            part["type"] == "tool"
                && part["metadata"]["providerExecuted"] != true
                && !(part["state"]["status"] == "error"
                    && part["state"]["metadata"]["interrupted"] == true)
        });
    if finish.is_empty() || ["tool-calls", "unknown"].contains(&finish) || follow_up {
        return child_unknown(open);
    }
    ended("turn_completed", "Native child turn completed".into())
}

fn child_model(info: &Value) -> Value {
    let (provider, id) = (
        string(&info["model"], "providerID"),
        string(&info["model"], "id"),
    );
    if provider.is_empty() || id.is_empty() {
        Value::Null
    } else {
        json!(format!("{provider}/{id}"))
    }
}

/// Parent summaries use only bounded per-child native reads. A child beyond the
/// roster bound or the time budget remains a member with unknown state.
fn child_roster(record: &Value, value: &mut Value) {
    let deadline = Instant::now() + Duration::from_secs(4);
    let listed = match children(record, deadline) {
        Ok(rows) => rows,
        Err(error) => {
            value["subagent_source"] = json!("unavailable");
            value["subagent_reason"] = json!(error);
            value["subagent_counts_complete"] = json!(false);
            value["subagent_total_count"] = Value::Null;
            value["subagents"] = json!({});
            return;
        }
    };
    let native = host(record);
    let evidence = listed
        .iter()
        .enumerate()
        .map(|(index, row)| {
            let id = string(row, "id");
            if index >= CHILD_ROSTER {
                return child_unknown("This child is beyond the bounded native roster");
            }
            let page = native.clone().and_then(|native| {
                http_until(
                    &native,
                    "GET",
                    &format!("/session/{id}/message?limit=1"),
                    None,
                    deadline,
                )
            });
            match page {
                Ok(page) => match page.as_array().map(Vec::as_slice) {
                    Some([latest]) => child_turn(id, latest),
                    Some([]) => child_unknown("No native turn has been recorded for this child"),
                    _ => child_unknown("Invalid native child messages"),
                },
                Err(error) => child_unknown(&error),
            }
        })
        .collect::<Vec<_>>();
    summarize_children(value, &listed, &evidence);
}
/// Counts never include an active child: the API host does not own the classic
/// process running one. Unknown states keep the totals incomplete.
fn summarize_children(value: &mut Value, listed: &[Value], evidence: &[Value]) {
    let mut roster = serde_json::Map::new();
    let (mut completed, mut unknown) = (0, 0);
    for (row, evidence) in listed.iter().zip(evidence) {
        let id = string(row, "id");
        let finished = evidence["state"] == "finished";
        let created = row["time"]["created"].as_f64().unwrap_or(0.0) / 1000.0;
        let updated = row["time"]["updated"].as_f64().unwrap_or(0.0) / 1000.0;
        let mut child = json!({"name":journal::clipped(&row["title"],200),"agent_profile":row["agent"],"model":child_model(row),
            "started":created,"updated":if finished {evidence["at"].as_f64().unwrap_or(updated).max(updated)} else {updated},
            "state":if finished {"finished"} else {"unknown"},"current_tool":""});
        if finished {
            completed += 1;
            child["detail"] = evidence["detail"].clone();
            child["completion_source"] = json!("opencode_native_messages");
            child["tracking_evidence"] = json!({"source":"opencode_native_messages","kind":evidence["kind"],"at":evidence["at"],"native_id":evidence["native_id"]});
            if evidence["error"] == true {
                child["display_state"] = json!("error");
            }
        } else {
            unknown += 1;
            child["detail"] = evidence["reason"].clone();
        }
        roster.insert(id.to_owned(), child);
    }
    unknown += listed.len().saturating_sub(evidence.len());
    value["subagent_source"] = json!("opencode_native");
    value["subagents"] = Value::Object(roster);
    value["subagent_active_count"] = json!(0);
    value["subagent_count"] = json!(0);
    value["subagent_completed_count"] = json!(completed);
    value["subagent_unknown_count"] = json!(unknown);
    value["subagent_total_count"] = json!(listed.len());
    value["subagent_counts_complete"] = json!(unknown == 0);
}

fn child_events(child: &str, messages: &[Value]) -> Result<Vec<Value>> {
    let mut rows = Vec::new();
    let row = |typ: &str, detail: String, tool: &str, at: f64, id: &str, reply: Value| json!({"type":typ,"detail":detail,"tool":tool,"at":at,"source":"opencode_native","agent_id":child,"message_id":id,"reply_id":reply});
    for message in messages {
        let info = &message["info"];
        let id = string(info, "id");
        let parts = message["parts"]
            .as_array()
            .ok_or("Invalid native child message")?;
        if info["sessionID"] != child
            || !native_id(id, "msg_")
            || parts.iter().any(|p| {
                p["sessionID"] != child
                    || p["messageID"] != id
                    || !native_id(string(p, "id"), "prt_")
            })
        {
            return Err("Native child history belongs to another conversation".into());
        }
        let created = info["time"]["created"].as_f64().unwrap_or(0.0) / 1000.0;
        let at = |part: &Value| {
            part["time"]["end"]
                .as_f64()
                .or(part["state"]["time"]["end"].as_f64())
                .map(|t| t / 1000.0)
                .unwrap_or(created)
        };
        let text = |part: &Value| {
            part["type"] == "text" && part["synthetic"] != true && part["ignored"] != true
        };
        match string(info, "role") {
            "user" => {
                let prompt = parts
                    .iter()
                    .filter(|p| text(p))
                    .filter_map(|p| p["text"].as_str())
                    .collect::<Vec<_>>()
                    .join("\n");
                rows.push(row(
                    "UserPromptSubmit",
                    prompt,
                    "",
                    created,
                    id,
                    Value::Null,
                ));
            }
            "assistant" => {
                for part in parts {
                    let part_id = string(part, "id");
                    if text(part) {
                        rows.push(row(
                            "AgentMessage",
                            string(part, "text").to_owned(),
                            "",
                            at(part),
                            part_id,
                            json!(id),
                        ));
                    } else if part["type"] == "tool" {
                        let state = &part["state"];
                        let title = journal::clipped(&state["title"], 32000);
                        let call = if title.is_empty() {
                            journal::clipped(&json!(state["input"].to_string()), 32000)
                        } else {
                            title
                        };
                        rows.push(row(
                            "PreToolUse",
                            call,
                            string(part, "tool"),
                            at(part),
                            part_id,
                            Value::Null,
                        ));
                        let result = match string(state, "status") {
                            "completed" => Some(journal::clipped(&state["output"], 32000)),
                            "error" => Some(journal::clipped(&state["error"], 32000)),
                            _ => None,
                        };
                        if let Some(result) = result {
                            rows.push(row(
                                "PostToolUse",
                                result,
                                string(part, "tool"),
                                at(part),
                                &format!("{part_id}:result"),
                                Value::Null,
                            ));
                        }
                    }
                }
                let end = info["time"]["completed"].as_f64().map(|t| t / 1000.0);
                if info["error"].is_object() && info["error"]["name"] != "MessageAbortedError" {
                    rows.push(row(
                        "StopFailure",
                        error_detail(&info["error"]),
                        "",
                        end.unwrap_or(created),
                        &format!("{id}:failure"),
                        Value::Null,
                    ));
                } else if let Some(end) = end {
                    rows.push(row(
                        "Stop",
                        String::new(),
                        "",
                        end,
                        &format!("{id}:stop"),
                        Value::Null,
                    ));
                }
            }
            _ => {}
        }
    }
    for (index, event) in rows.iter_mut().enumerate() {
        event["seq"] = json!(index + 1);
    }
    Ok(rows)
}

/// Read-only child view. Identity is the exact native child whose recorded
/// parent is this binding's conversation; its state is its own evidence only.
fn child(record: &Value, id: &str) -> Result<Value> {
    let parent = string(record, "conversation_id");
    if !native_id(id, "ses_") || id == parent {
        return Err("Invalid OpenCode child identity".into());
    }
    let session = legacy_verify_session(record)?;
    let info = rpc(record, "GET", &format!("/api/session/{id}"), None)?["data"].clone();
    if info["id"] != id || info["parentID"] != record["conversation_id"] {
        return Err("This OpenCode child does not belong to the exact parent conversation".into());
    }
    let page = rpc(
        record,
        "GET",
        &format!("/session/{id}/message?limit={CHILD_HISTORY}"),
        None,
    )?;
    let messages = page.as_array().ok_or("Invalid OpenCode child history")?;
    if messages.len() > CHILD_HISTORY {
        return Err("OpenCode child history exceeds its requested bound".into());
    }
    let events = child_events(id, messages)?;
    let evidence = messages.last().map_or_else(
        || child_unknown("No native turn has been recorded for this child"),
        |latest| child_turn(id, latest),
    );
    let mut value = normalize(
        record,
        &session,
        false,
        &[],
        &Value::Null,
        &Value::Null,
        true,
    );
    let finished = evidence["state"] == "finished";
    let reply = events.iter().rev().find(|e| e["type"] == "AgentMessage");
    let summary = match string(&evidence, "kind") {
        "turn_completed" => "Ready",
        "turn_interrupted" => "Interrupted",
        "turn_failed" => "Turn failed",
        _ => "Status unavailable",
    };
    for (key,item) in json!({"agent_id":id,"parent_conversation_id":parent,"conversation_id":format!("{parent}/{id}"),
        "created":info["time"]["created"].as_f64().unwrap_or(0.0)/1000.0,"cwd":info["location"]["directory"],
        "phase":if finished {"idle"} else {"unknown"},"activity":if finished {"idle"} else {"unknown"},"activity_summary":summary,
        "activity_detail":if finished {reply.map(|r|r["detail"].clone()).unwrap_or(evidence["detail"].clone())} else {evidence["reason"].clone()},
        "prompt":events.iter().rev().find(|e| e["type"]=="UserPromptSubmit").map(|r|r["detail"].clone()),
        "reply_id":reply.map(|r|r["reply_id"].clone()),"reply_at":reply.map(|r|r["at"].clone()),
        "last_event_at":info["time"]["updated"].as_f64().unwrap_or(0.0)/1000.0,"turn_started":0.0,
        "model":info["model"]["id"],"model_provider":info["model"]["providerID"],"settings_model":child_model(&info),"agent_profile":info["agent"],
        "send_available":false,"send_supported":false,"read_only":true,"settings_change_supported":false,"interrupt_supported":false,
        "allowed_actions":[],"pending_questions":[],"events":events,"history_truncated":messages.len()==CHILD_HISTORY,
        "history_scope":"Native OpenCode subagent history","tracking_evidence":if finished {json!({"source":"opencode_native_messages","kind":evidence["kind"],"at":evidence["at"],"native_id":evidence["native_id"]})} else {Value::Null}}).as_object().unwrap() {value[key]=item.clone();}
    if let Some(map) = value.as_object_mut() {
        map.remove("attention_id");
    }
    value["host_generation"] = host(record)?["generation"].clone();
    value["attachment_messages"] = json!([]);
    value["voice_messages"] = json!([]);
    value["processes"] = json!({"items":[],"complete":false,"source":"opencode_native","reason":"OpenCode child process controls are not available"});
    Ok(value)
}

pub(crate) fn inspect(name: &str, agent_id: Option<&str>) -> Result<Value> {
    let record = binding(name)?;
    if let Some(id) = agent_id {
        return child(&record, id);
    }
    let mut value = parent_view(name, &record, true)?;
    waits::decorate(std::slice::from_mut(&mut value));
    Ok(value)
}
// Delivery, answer, interrupt and lifecycle checks need only the parent's
// own state; they never wait for the bounded child roster.
pub(crate) fn own_state(name: &str) -> Result<Value> {
    parent_view(name, &binding(name)?, false)
}
fn parent_view(name: &str, record: &Value, roster: bool) -> Result<Value> {
    let record = record.clone();
    let session = legacy_verify_session(&record)?;
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
    let configured = catalog(None);
    result["model_options"] = configured["model_options"].clone();
    result["processes"] = json!({"items":[],"complete":false,"source":"opencode_native","reason":"Individual OpenCode shell process controls are not yet qualified"});
    if roster {
        child_roster(&record, &mut result);
    }
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
    let service = service::connect();
    let computer = crate::platform::hostname();
    let mut migration: Option<Reply> = None;
    let mut verdicts: Option<Vec<String>> = None;
    let mut verdicts_changed = false;
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
        let unavailable = |reason: String| {
            let mut row = normalize(
                &record,
                &Value::Null,
                false,
                &[],
                &Value::Null,
                &Value::Null,
                false,
            );
            row["state"] = json!("unknown");
            row["process_state"] = json!("unknown");
            row["runtime_state"] = json!("unknown");
            row["activity_summary"] = json!("Inspection unavailable");
            row["inspection_unavailable"] = json!(true);
            row["inspection_reason"] = json!(reason);
            row
        };
        let row = match &service {
            Err(why) => unavailable(why.reason(&computer)),
            Ok(service) => {
                let key = verdict_key(&record, &service.reg);
                let known = record["service"] != "opencode2"
                    && verdicts.get_or_insert_with(load_verdicts).contains(&key);
                let state = if known {
                    Ok((Legacy::Unsupported, false))
                } else {
                    legacy_state(service, &record, &mut migration)
                };
                match state {
                    Err(reason) => unavailable(reason),
                    Ok((Legacy::Unsupported, settled)) => {
                        if settled && !known {
                            let keys = verdicts.get_or_insert_with(load_verdicts);
                            if keys.len() >= VERDICT_LIMIT {
                                keys.clear();
                            }
                            keys.push(key);
                            verdicts_changed = true;
                        }
                        let mut row = unavailable(
                            "This conversation belongs to OpenCode 1, which Zerus no longer supports"
                                .into(),
                        );
                        row.as_object_mut()
                            .unwrap()
                            .remove("inspection_unavailable");
                        row["state"] = json!("stopped");
                        row["process_state"] = json!("exited");
                        row["runtime_state"] = json!("stopped");
                        row["activity_summary"] = json!("OpenCode 1 conversation");
                        row["allowed_actions"] = json!(["forget"]);
                        row
                    }
                    Ok((verdict, _)) => {
                        if matches!(verdict, Legacy::Adopted) {
                            adopt_binding(
                                string(&record, "name"),
                                string(&record, "conversation_id"),
                            );
                        }
                        inspect(string(&record, "name"), None).unwrap_or_else(unavailable)
                    }
                }
            }
        };
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
    if verdicts_changed {
        save_verdicts(verdicts.as_deref().unwrap_or(&[]));
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
/// The native OpenCode 2 operations that delivery needs.
trait Native {
    fn raw(&self, method: &str, path: &str, body: Option<&Value>) -> Result<Response>;
}
impl Native for service::Service {
    fn raw(&self, method: &str, path: &str, body: Option<&Value>) -> Result<Response> {
        service::Service::raw(self, method, path, body)
    }
}
#[derive(Debug)]
enum Delivery {
    Delivered,
    Queued,
    /// The service definitively rejected the message (a 4xx answer).
    Refused(String),
}
fn refusal_reason(response: &Response) -> String {
    let text = response.body["message"]
        .as_str()
        .or(response.body["data"]["message"].as_str())
        .unwrap_or("request rejected");
    let clean: String = text.chars().filter(|c| !c.is_control()).take(160).collect();
    format!("HTTP {}: {clean}", response.status)
}
fn refused(reason: &str) -> String {
    format!("OpenCode refused this message: {reason}")
}
const MISMATCH: &str = "Native message acknowledgement did not match; delivery uncertain";
const UNCERTAIN: &str = "Native message ID has conflicting text; delivery uncertain";
const UNCERTAIN_REPLAY: &str = "Delivery uncertain; this request will not be replayed. Inspect native history before creating a new request";
/// Resolve one message ID exactly: look it up, then the inbox, and only then
/// repost the same ID, which the service admits at most once.
fn reconcile(
    native: &dyn Native,
    record: &Value,
    message_id: &str,
    text: &str,
) -> Result<Delivery> {
    let found = native.raw(
        "GET",
        &session_path(record, &format!("/message/{message_id}")),
        None,
    )?;
    match found.status {
        200 => {
            let data = &found.body["data"];
            if data["type"] == "user" && data["id"] == message_id && data["text"] == text {
                return Ok(Delivery::Delivered);
            }
            return Err(UNCERTAIN.into());
        }
        404 if found.body["_tag"] == "MessageNotFoundError" => {}
        _ => return Err("Native delivery state is uncertain".into()),
    }
    let inbox = native.raw("GET", &session_path(record, "/inbox"), None)?;
    if inbox.status != 200 {
        return Err("Native delivery state is uncertain".into());
    }
    if let Some(item) = inbox.body["data"]
        .as_array()
        .into_iter()
        .flatten()
        .find(|item| item["id"] == message_id)
    {
        if item["payload"]["text"] == text {
            return Ok(Delivery::Queued);
        }
        return Err(UNCERTAIN.into());
    }
    let posted = native.raw(
        "POST",
        &session_path(record, "/prompt"),
        Some(&json!({"id":message_id,"text":text,"delivery":"queue"})),
    )?;
    match posted.status {
        200..=299 => {
            let data = &posted.body["data"];
            if data["id"] == message_id
                && data["sessionID"] == record["conversation_id"]
                && data["payload"]["text"] == text
            {
                Ok(Delivery::Delivered)
            } else {
                Err(MISMATCH.into())
            }
        }
        400..=499 => Ok(Delivery::Refused(refusal_reason(&posted))),
        _ => Err("Native delivery state is uncertain".into()),
    }
}
fn receipt_path(receipt: &Value) -> PathBuf {
    root()
        .join("opencode/receipts")
        .join(format!("{}.json", string(receipt, "request_id")))
}
fn running_service() -> Result<service::Service> {
    // Sending is explicit, but phase 1 does not start the service for it.
    service::connect().map_err(|why| why.reason(&crate::platform::hostname()))
}
fn check_receipt_identity(record: &Value, receipt: &Value) -> Result<()> {
    if !native_id(string(receipt, "native_message_id"), "msg_")
        || receipt["conversation_id"] != record["conversation_id"]
        || receipt["run_id"] != record["run_id"]
    {
        return Err("Native receipt identity changed".into());
    }
    Ok(())
}
/// Reconcile one receipt; a definite refusal is recorded in it.
fn settle(
    native: &dyn Native,
    record: &Value,
    receipt: &mut Value,
    path: &Path,
) -> Result<Delivery> {
    check_receipt_identity(record, receipt)?;
    let id = string(receipt, "native_message_id").to_owned();
    let text = string(receipt, "text").to_owned();
    let outcome = reconcile(native, record, &id, &text)?;
    match &outcome {
        Delivery::Delivered | Delivery::Queued => receipt["status"] = json!("submitted"),
        Delivery::Refused(reason) => {
            receipt["status"] = json!("refused");
            receipt["reason"] = json!(reason);
        }
    }
    atomic(path, &receipt.to_string())?;
    Ok(outcome)
}
pub(super) fn reconcile_receipt(record: &Value, receipt: &mut Value) -> Result<bool> {
    match receipt["status"].as_str() {
        Some("submitted") => return Ok(true),
        Some("refused") => return Err(refused(string(receipt, "reason"))),
        _ => {}
    }
    let service = running_service()?;
    verify_session(&service, record)?;
    match settle(&service, record, receipt, &receipt_path(receipt))? {
        Delivery::Refused(reason) => Err(refused(&reason)),
        _ => Ok(true),
    }
}
/// Whether a form or permission is waiting on the user for this session.
fn waiting_for_user(native: &dyn Native, record: &Value) -> Result<bool> {
    for suffix in ["/form", "/permission"] {
        let answer = native.raw("GET", &session_path(record, suffix), None)?;
        if answer.status != 200 {
            return Err("OpenCode pending requests could not be read; no message was sent".into());
        }
        if answer.body["data"].as_array().is_none_or(|v| !v.is_empty()) {
            return Ok(true);
        }
    }
    Ok(false)
}
fn session_active(native: &dyn Native, record: &Value) -> bool {
    native
        .raw("GET", "/api/session/active", None)
        .ok()
        .filter(|r| r.status == 200)
        .is_some_and(|r| r.body["data"].get(string(record, "conversation_id")).is_some())
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
    let service = running_service()?;
    deliver(
        &service,
        record,
        request,
        &path,
        digest,
        session_graph::target_receipt(record),
    )
}
/// Core of `send`: everything after validation, against any native service.
fn deliver(
    native: &dyn Native,
    record: &Value,
    request: &Value,
    path: &Path,
    digest: String,
    graph_target: Option<Value>,
) -> Result<Value> {
    let text = string(request, "text");
    if path.exists() {
        let mut prior: Value = serde_json::from_slice(&fs::read(path).map_err(|e| e.to_string())?)
            .map_err(|e| e.to_string())?;
        if prior["digest"] != digest {
            return Err("This request ID was already used for another message".into());
        }
        match prior["status"].as_str() {
            Some("submitted") => return Ok(prior),
            Some("refused") => return Err(refused(string(&prior, "reason"))),
            _ => {}
        }
        verify_native(native, record)?;
        return match settle(native, record, &mut prior, path) {
            Ok(Delivery::Refused(reason)) => Err(refused(&reason)),
            Ok(_) => Ok(prior),
            Err(_) => Err(UNCERTAIN_REPLAY.into()),
        };
    }
    verify_native(native, record)?;
    if waiting_for_user(native, record)? {
        return Err(
            "OpenCode is waiting for an exact question or permission; no message was sent".into(),
        );
    }
    let message_id = new_id("msg_");
    let mut receipt = json!({"request_id":request["request_id"],"name":record["name"],"run_id":record["run_id"],"conversation_id":record["conversation_id"],"digest":digest,"native_message_id":message_id,"text":text,"submitted_text":text,"submitted_at":now(),"at":now(),"status":"pending","attachments":[],"transport":"native_opencode"});
    if let Some(target) = graph_target {
        receipt["graph_target"] = target;
    }
    let queued = session_active(native, record);
    atomic(path, &receipt.to_string())?;
    let posted = native.raw(
        "POST",
        &session_path(record, "/prompt"),
        Some(&json!({"id":message_id,"text":text,"delivery":"queue"})),
    );
    match posted {
        Ok(answer) if (200..300).contains(&answer.status) => {
            let data = &answer.body["data"];
            if data["id"] != message_id
                || data["sessionID"] != record["conversation_id"]
                || data["payload"]["text"] != text
            {
                return Err(MISMATCH.into());
            }
        }
        Ok(answer) if (400..500).contains(&answer.status) => {
            let reason = refusal_reason(&answer);
            receipt["status"] = json!("refused");
            receipt["reason"] = json!(reason);
            atomic(path, &receipt.to_string())?;
            return Err(refused(&reason));
        }
        // A transport error or a 5xx leaves the admission unknown.
        _ => match settle(native, record, &mut receipt, path) {
            Ok(Delivery::Refused(reason)) => return Err(refused(&reason)),
            Ok(_) => {}
            Err(_) => return Err(UNCERTAIN_REPLAY.into()),
        },
    }
    receipt["status"] = json!("submitted");
    if queued {
        receipt["queued"] = json!(true);
    }
    atomic(path, &receipt.to_string())?;
    Ok(receipt)
}
fn verify_native(native: &dyn Native, record: &Value) -> Result<()> {
    let result = native.raw("GET", &session_path(record, ""), None)?;
    if result.status != 200
        || result.body["data"]["id"] != record["conversation_id"]
        || result.body["data"]["location"]["directory"] != record["cwd"]
    {
        return Err("The exact native OpenCode session or directory changed".into());
    }
    Ok(())
}
fn answer(record: &Value, request: &Value) -> Result<Value> {
    let current = own_state(string(record, "name"))?;
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
    let current = own_state(string(record, "name"))?;
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
    let current = own_state(string(record, "name"))?;
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
            if args.iter().any(|v| v == "--archive") {
                return Err("OpenCode archive inspection is not supported".into());
            }
            let agent = match args.iter().position(|v| v == "--agent") {
                Some(i) => Some(
                    args.get(i + 1)
                        .map(String::as_str)
                        .ok_or("Choose an OpenCode child session")?,
                ),
                None => None,
            };
            println!("{}", inspect(name, agent)?);
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
            if own_state(name)?["settings_change_supported"] != true {
                return Err("OpenCode must be live and idle before changing its model".into());
            }
            if !string(&request, "effort").is_empty() {
                return Err("OpenCode effort controls are not yet qualified".into());
            }
            let model = legacy_offered_model(&home(), string(&request, "model"))?;
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
            let verified_model = legacy_verify_session(&record)?["model"].clone();
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
    use std::io::Write;
    #[test]
    fn new_ids_match_opencode_and_stored_ids_keep_the_existing_check() {
        for prefix in ["ses_", "msg_"] {
            let id = new_id(prefix);
            assert_eq!(id.len(), 30);
            assert!(id[4..].bytes().all(|b| b.is_ascii_alphanumeric()));
            assert!(native_id(&id, prefix));
        }
        assert_ne!(new_id("ses_"), new_id("ses_"));
        // OpenCode 1 bindings used ses_ plus 32 hex characters; they stay valid.
        assert!(native_id("ses_0123456789abcdef0123456789abcdef", "ses_"));
        assert!(native_id("ses_eb44cc196ffeOcEUMCZm4t66t7", "ses_"));
        assert!(!native_id("ses_bad id", "ses_"));
    }
    #[test]
    fn legacy_bindings_are_adopted_only_for_their_own_migrated_session() {
        let record = json!({"conversation_id": "ses_0123456789abcdef0123456789abcdef", "cwd": "/workspace/fixture", "native_data_home": "/x"});
        let session = json!({"data": {"id": "ses_0123456789abcdef0123456789abcdef", "location": {"directory": "/workspace/fixture"}}});
        assert!(matches!(
            legacy_verdict(&record, &json!({"status": "completed"}), Some(&session)),
            Legacy::Adopted
        ));
        assert!(matches!(
            legacy_verdict(&record, &json!({"status": "running"}), Some(&session)),
            Legacy::Unsupported
        ));
        assert!(matches!(
            legacy_verdict(&record, &json!({"status": "completed"}), None),
            Legacy::Unsupported
        ));
        let moved = json!({"data": {"id": "ses_0123456789abcdef0123456789abcdef", "location": {"directory": "/elsewhere"}}});
        assert!(matches!(
            legacy_verdict(&record, &json!({"status": "completed"}), Some(&moved)),
            Legacy::Unsupported
        ));
        let current = json!({"conversation_id": "ses_x", "cwd": "/w", "service": "opencode2"});
        assert!(matches!(
            legacy_verdict(&current, &json!({}), None),
            Legacy::Current
        ));
    }
    #[test]
    fn legacy_verdict_needs_exact_nonempty_identity() {
        let done = json!({"status": "completed"});
        let record = json!({"conversation_id": "ses_a", "cwd": "/workspace/fixture"});
        let good =
            json!({"data": {"id": "ses_a", "location": {"directory": "/workspace/fixture"}}});
        assert!(matches!(
            legacy_verdict(&record, &done, Some(&good)),
            Legacy::Adopted
        ));
        let other =
            json!({"data": {"id": "ses_b", "location": {"directory": "/workspace/fixture"}}});
        assert!(matches!(
            legacy_verdict(&record, &done, Some(&other)),
            Legacy::Unsupported
        ));
        assert!(matches!(
            legacy_verdict(&record, &done, Some(&json!({}))),
            Legacy::Unsupported
        ));
        for bad in [
            json!({"cwd": "/w"}),
            json!({"conversation_id": "ses_a"}),
            json!({"conversation_id": "", "cwd": ""}),
        ] {
            let blank = json!({"data": {"id": bad["conversation_id"], "location": {"directory": bad["cwd"]}}});
            assert!(matches!(
                legacy_verdict(&bad, &done, Some(&blank)),
                Legacy::Unsupported
            ));
        }
    }
    fn reply(status: u16, body: Value) -> Reply {
        Ok(Response { status, body })
    }
    #[test]
    fn only_definite_answers_make_a_binding_unsupported() {
        let record = json!({"conversation_id": "ses_a", "cwd": "/w"});
        let session = || {
            reply(
                200,
                json!({"data": {"id": "ses_a", "location": {"directory": "/w"}}}),
            )
        };
        let verdict = |m: Reply, s: Reply| legacy_decision(&record, &m, || s);
        let done = || reply(200, json!({"data": {"status": "completed"}}));
        assert!(matches!(
            verdict(done(), session()),
            Ok((Legacy::Adopted, true))
        ));
        assert!(matches!(
            verdict(done(), reply(404, json!({}))),
            Ok((Legacy::Unsupported, true))
        ));
        assert!(matches!(
            verdict(
                done(),
                reply(
                    200,
                    json!({"data": {"id": "ses_z", "location": {"directory": "/w"}}})
                )
            ),
            Ok((Legacy::Unsupported, true))
        ));
        // Not completed: unsupported but not settled, and no session request.
        let pending = legacy_decision(&record, &reply(200, json!({"status": "running"})), || {
            panic!("no session GET")
        });
        assert!(matches!(pending, Ok((Legacy::Unsupported, false))));
        // Errors never become a verdict.
        assert!(verdict(Err("refused".into()), session()).is_err());
        assert!(verdict(reply(500, json!({})), session()).is_err());
        assert!(verdict(done(), Err("timeout".into())).is_err());
        assert!(verdict(done(), reply(503, json!({}))).is_err());
        let current = json!({"service": "opencode2"});
        assert!(matches!(
            legacy_decision(&current, &Err("x".into()), || panic!("none")),
            Ok((Legacy::Current, false))
        ));
    }
    fn ack(value: &Value) -> bool {
        value["data"]["id"] == "ses_a"
    }
    fn counted(
        get: Reply,
        post: Reply,
        first: bool,
    ) -> (std::result::Result<(), Refusal>, usize, usize) {
        let (mut gets, mut posts) = (0, 0);
        let mut g = || {
            gets += 1;
            get.clone()
        };
        let mut p = || {
            posts += 1;
            post.clone()
        };
        let result = if first {
            create_session(&mut g, &mut p, &ack)
        } else {
            reconcile_creation(&mut g, &mut p, &ack)
        };
        (result, gets, posts)
    }
    #[test]
    fn lost_creation_replies_are_reconciled_without_a_second_identity() {
        let found = reply(200, json!({"data": {"id": "ses_a"}}));
        let (result, gets, posts) = counted(found.clone(), Err("lost".into()), false);
        assert!(result.is_ok() && gets == 1 && posts == 0);
        let (result, gets, posts) = counted(reply(404, json!({})), found.clone(), false);
        assert!(result.is_ok() && gets == 1 && posts == 1);
        let wrong = reply(200, json!({"data": {"id": "ses_other"}}));
        let (result, _, posts) = counted(wrong, found.clone(), false);
        assert!(matches!(result, Err(Refusal::Other(_))) && posts == 0);
        let (result, _, posts) = counted(reply(404, json!({})), wrong_ack(), false);
        assert!(matches!(result, Err(Refusal::Other(_))) && posts == 1);
        // First attempt: a definite refusal ends it; a lost reply reconciles.
        let (result, gets, posts) =
            counted(found.clone(), reply(400, json!({"message": "bad"})), true);
        assert!(matches!(result, Err(Refusal::Definite(_))) && gets == 0 && posts == 1);
        let (result, gets, posts) = counted(found.clone(), Err("lost".into()), true);
        assert!(result.is_ok() && gets == 1 && posts == 1);
    }
    fn wrong_ack() -> Reply {
        reply(200, json!({"data": {"id": "ses_other"}}))
    }
    #[test]
    fn session_paths_use_the_conversation_id() {
        let record = json!({"conversation_id": "ses_abc"});
        assert_eq!(session_path(&record, ""), "/api/session/ses_abc");
        assert_eq!(
            session_path(&record, "/prompt"),
            "/api/session/ses_abc/prompt"
        );
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
    // Shapes recorded from the reviewed release's classic task-tool children.
    fn assistant(
        child: &str,
        completed: Option<u64>,
        finish: Option<&str>,
        error: Value,
        parts: Value,
    ) -> Value {
        let mut info = json!({"id":"msg_1406139c9002","sessionID":child,"role":"assistant","time":{"created":1792081476041_u64},"parentID":"msg_1406139c9001","agent":"general"});
        if let Some(t) = completed {
            info["time"]["completed"] = json!(t);
        }
        if let Some(f) = finish {
            info["finish"] = json!(f);
        }
        if !error.is_null() {
            info["error"] = error;
        }
        json!({"info":info,"parts":parts})
    }
    #[test]
    fn child_turn_requires_the_native_loop_exit_and_keeps_open_turns_unknown() {
        let child = "ses_ebf9b81dcffe0BQL0Ct6wWuwaX";
        let text = json!([{"id":"prt_a","sessionID":child,"messageID":"msg_1406139c9002","type":"step-start"},
            {"id":"prt_b","sessionID":child,"messageID":"msg_1406139c9002","type":"text","text":"Child reply"}]);
        let done = child_turn(
            child,
            &assistant(
                child,
                Some(1792081690267),
                Some("stop"),
                Value::Null,
                text.clone(),
            ),
        );
        assert_eq!(done["state"], "finished");
        assert_eq!(done["kind"], "turn_completed");
        assert_eq!(done["native_id"], "msg_1406139c9002");
        // Running in another process, between tool steps, or abandoned.
        for open in [
            assistant(child, None, None, Value::Null, text.clone()),
            assistant(
                child,
                Some(1),
                Some("tool-calls"),
                Value::Null,
                text.clone(),
            ),
            assistant(child, Some(1), Some("unknown"), Value::Null, text.clone()),
            assistant(child, Some(1), None, Value::Null, text.clone()),
            assistant(
                child,
                Some(1),
                Some("stop"),
                Value::Null,
                json!([{"id":"prt_c","type":"tool","tool":"bash","state":{"status":"completed"}}]),
            ),
            json!({"info":{"id":"msg_1406139c9003","sessionID":child,"role":"user","time":{"created":1}},"parts":[]}),
        ] {
            assert_eq!(child_turn(child, &open)["state"], "unknown", "{open}");
        }
        // Provider-executed and interrupted tool calls do not await a follow-up.
        let settled = json!([{"id":"prt_d","type":"tool","metadata":{"providerExecuted":true},"state":{"status":"completed"}},
            {"id":"prt_e","type":"tool","state":{"status":"error","metadata":{"interrupted":true}}}]);
        assert_eq!(
            child_turn(
                child,
                &assistant(child, Some(1), Some("stop"), Value::Null, settled)
            )["state"],
            "finished"
        );
        let aborted = child_turn(
            child,
            &assistant(
                child,
                Some(1792081883704),
                None,
                json!({"name":"MessageAbortedError","data":{"message":"Aborted"}}),
                text.clone(),
            ),
        );
        assert_eq!(aborted["kind"], "turn_interrupted");
        assert!(aborted.get("error").is_none());
        let failed = child_turn(
            child,
            &assistant(
                child,
                Some(2),
                None,
                json!({"name":"APIError","data":{"message":"Fixture provider rejected"}}),
                text.clone(),
            ),
        );
        assert_eq!(failed["kind"], "turn_failed");
        assert_eq!(failed["error"], true);
        assert_eq!(failed["detail"], "Fixture provider rejected");
        assert_eq!(
            child_turn(
                child,
                &assistant(child, None, None, json!({"name":"APIError"}), text.clone())
            )["state"],
            "unknown"
        );
        // Another session's message never settles this child.
        assert_eq!(
            child_turn(
                "ses_other",
                &assistant(child, Some(1), Some("stop"), Value::Null, text)
            )["state"],
            "unknown"
        );
    }
    #[test]
    fn child_membership_is_exact_and_counts_never_claim_active_work() {
        let record = json!({"conversation_id":"ses_parent"});
        let row = |id: &str, parent: &str, created: u64| json!({"id":id,"parentID":parent,"title":format!("{id} (@general subagent)"),"agent":"general","model":{"id":"local","providerID":"fixture"},"time":{"created":created,"updated":created+5}});
        let listed = exact_children(
            &record,
            &json!([
                row("ses_old", "ses_parent", 1000),
                row("ses_new", "ses_parent", 2000)
            ]),
        )
        .unwrap();
        assert_eq!(
            listed.iter().map(|r| string(r, "id")).collect::<Vec<_>>(),
            ["ses_new", "ses_old"]
        );
        for contradictory in [
            json!([row("ses_one", "ses_other", 1)]),
            json!([row("ses_parent", "ses_parent", 1)]),
            json!([
                row("ses_one", "ses_parent", 1),
                row("ses_one", "ses_parent", 2)
            ]),
            json!([row("bad/id", "ses_parent", 1)]),
            json!({"data":[]}),
        ] {
            assert!(
                exact_children(&record, &contradictory).is_err(),
                "{contradictory}"
            );
        }
        let mut parent = json!({"phase":"working","activity":"busy"});
        let finished = json!({"state":"finished","kind":"turn_failed","at":3.0,"native_id":"msg_x","detail":"failed","error":true});
        summarize_children(&mut parent, &listed, &[child_unknown("open"), finished]);
        // A working parent never makes its children working.
        assert_eq!(parent["subagents"]["ses_new"]["state"], "unknown");
        assert_eq!(parent["subagents"]["ses_new"]["detail"], "open");
        assert_eq!(parent["subagents"]["ses_old"]["state"], "finished");
        assert_eq!(parent["subagents"]["ses_old"]["display_state"], "error");
        assert_eq!(
            parent["subagents"]["ses_old"]["tracking_evidence"]["native_id"],
            "msg_x"
        );
        assert_eq!(parent["subagents"]["ses_old"]["model"], "fixture/local");
        assert_eq!(parent["subagent_active_count"], 0);
        assert_eq!(parent["subagent_completed_count"], 1);
        assert_eq!(parent["subagent_unknown_count"], 1);
        assert_eq!(parent["subagent_total_count"], 2);
        assert_eq!(parent["subagent_counts_complete"], false);
        assert_eq!(parent["phase"], "working");
        let mut idle = json!({});
        summarize_children(&mut idle, &[], &[]);
        assert_eq!(idle["subagent_counts_complete"], true);
        assert_eq!(idle["subagent_total_count"], 0);
    }
    #[test]
    fn child_history_is_bound_to_its_own_session_and_messages() {
        let child = "ses_child";
        let user = json!({"info":{"id":"msg_1","sessionID":child,"role":"user","time":{"created":1000}},
            "parts":[{"id":"prt_1","sessionID":child,"messageID":"msg_1","type":"text","text":"literal\\\nwork"},
                     {"id":"prt_2","sessionID":child,"messageID":"msg_1","type":"text","text":"hidden","synthetic":true}]});
        let reply = json!({"info":{"id":"msg_2","sessionID":child,"role":"assistant","time":{"created":2000,"completed":4000},"finish":"stop"},
            "parts":[{"id":"prt_3","sessionID":child,"messageID":"msg_2","type":"tool","tool":"bash","state":{"status":"completed","title":"Print","input":{"command":"printf x"},"output":"x","time":{"end":3000}}},
                     {"id":"prt_4","sessionID":child,"messageID":"msg_2","type":"text","text":"Child reply","time":{"end":3500}}]});
        let events = child_events(child, &[user.clone(), reply.clone()]).unwrap();
        let kinds = events
            .iter()
            .map(|e| string(e, "type").to_owned())
            .collect::<Vec<_>>();
        assert_eq!(
            kinds,
            [
                "UserPromptSubmit",
                "PreToolUse",
                "PostToolUse",
                "AgentMessage",
                "Stop"
            ]
        );
        assert_eq!(events[0]["detail"], "literal\\\nwork");
        assert_eq!(events[2]["detail"], "x");
        assert_eq!(events[3]["reply_id"], "msg_2");
        assert_eq!(events[3]["message_id"], "prt_4");
        assert_eq!(events[3]["at"], 3.5);
        assert!(events
            .iter()
            .all(|e| e["agent_id"] == child && e["source"] == "opencode_native"));
        let mut foreign = reply.clone();
        foreign["parts"][1]["sessionID"] = json!("ses_other");
        assert!(child_events(child, &[user.clone(), foreign]).is_err());
        let mut moved = reply;
        moved["info"]["sessionID"] = json!("ses_other");
        assert!(child_events(child, &[user, moved]).is_err());
    }
    struct Scripted(std::cell::RefCell<Vec<(String, u16, Value)>>);
    impl Native for Scripted {
        fn raw(&self, method: &str, path: &str, _: Option<&Value>) -> Result<Response> {
            let mut script = self.0.borrow_mut();
            let (expected, status, body) = script.remove(0);
            assert_eq!(expected, format!("{method} {path}"));
            Ok(Response { status, body })
        }
    }
    fn fixture(bytes: &[u8]) -> Value {
        serde_json::from_slice(bytes).unwrap()
    }
    #[test]
    fn a_queued_message_is_found_in_the_inbox_after_a_lost_response() {
        let record = json!({"conversation_id": "ses_A"});
        let inbox = fixture(include_bytes!("../../tests/fixtures/opencode2/inbox_busy.json"));
        let queued = inbox["data"][0]["id"].as_str().unwrap().to_owned();
        let native = Scripted(std::cell::RefCell::new(vec![
            (
                format!("GET /api/session/ses_A/message/{queued}"),
                404,
                fixture(include_bytes!("../../tests/fixtures/opencode2/message_not_found.json")),
            ),
            ("GET /api/session/ses_A/inbox".into(), 200, inbox.clone()),
        ]));
        assert!(matches!(
            reconcile(&native, &record, &queued, "queued later").unwrap(),
            Delivery::Queued
        ));
    }
    #[test]
    fn an_unknown_message_is_reposted_with_the_same_id_and_never_twice() {
        let prompt = fixture(include_bytes!("../../tests/fixtures/opencode2/prompt.json"));
        let session = prompt["data"]["sessionID"].as_str().unwrap().to_owned();
        let record = json!({"conversation_id": session});
        let id = prompt["data"]["id"].as_str().unwrap().to_owned();
        let native = Scripted(std::cell::RefCell::new(vec![
            (
                format!("GET /api/session/{session}/message/{id}"),
                404,
                json!({"_tag": "MessageNotFoundError"}),
            ),
            (format!("GET /api/session/{session}/inbox"), 200, json!({"data": []})),
            (format!("POST /api/session/{session}/prompt"), 200, prompt.clone()),
        ]));
        assert!(matches!(
            reconcile(&native, &record, &id, "hello").unwrap(),
            Delivery::Delivered
        ));
        assert!(native.0.borrow().is_empty());
    }
    #[test]
    fn conflicting_text_for_the_same_id_is_uncertain() {
        let record = json!({"conversation_id": "ses_A"});
        let native = Scripted(std::cell::RefCell::new(vec![(
            "GET /api/session/ses_A/message/msg_X".into(),
            200,
            json!({"data": {"id": "msg_X", "type": "user", "text": "other"}}),
        )]));
        assert!(reconcile(&native, &record, "msg_X", "hello").is_err());
    }
    #[test]
    fn pending_forms_or_permissions_block_sending_but_a_busy_session_does_not() {
        let record = json!({"conversation_id": "ses_A"});
        let empty = || json!({"data": []});
        let native = Scripted(std::cell::RefCell::new(vec![
            ("GET /api/session/ses_A/form".into(), 200, empty()),
            ("GET /api/session/ses_A/permission".into(), 200, empty()),
        ]));
        assert!(!waiting_for_user(&native, &record).unwrap());
        let permissions = fixture(include_bytes!("../../tests/fixtures/opencode2/permissions.json"));
        let native = Scripted(std::cell::RefCell::new(vec![
            ("GET /api/session/ses_A/form".into(), 200, empty()),
            ("GET /api/session/ses_A/permission".into(), 200, permissions),
        ]));
        assert!(waiting_for_user(&native, &record).unwrap());
        let busy = fixture(include_bytes!("../../tests/fixtures/opencode2/active_busy.json"));
        let record = json!({"conversation_id": "ses_IycejmWY8RpJq1aeBnnIpbaNAt"});
        let native = Scripted(std::cell::RefCell::new(vec![(
            "GET /api/session/active".into(),
            200,
            busy,
        )]));
        assert!(session_active(&native, &record));
    }
    #[test]
    fn only_opencode2_bindings_resume_a_pending_creation() {
        assert!(resumes_pending(&json!({"service":"opencode2","creation_pending":true})));
        assert!(!resumes_pending(&json!({"creation_pending":true})));
        assert!(!resumes_pending(&json!({"service":"opencode1","creation_pending":true})));
        assert!(!resumes_pending(&json!({"service":"opencode2"})));
    }
    type Step = (String, Result<Response>);
    struct Live {
        script: std::cell::RefCell<Vec<Step>>,
        posted: std::cell::RefCell<Vec<Value>>,
        path: PathBuf,
    }
    impl Native for Live {
        fn raw(&self, method: &str, path: &str, body: Option<&Value>) -> Result<Response> {
            let (expected, reply) = self.script.borrow_mut().remove(0);
            assert_eq!(expected, format!("{method} {path}"));
            if method == "POST" {
                let receipt: Value =
                    serde_json::from_slice(&fs::read(&self.path).unwrap()).unwrap();
                assert_eq!(receipt["status"], "pending");
                assert_eq!(receipt["native_message_id"], body.unwrap()["id"]);
                self.posted.borrow_mut().push(body.unwrap().clone());
            }
            reply
        }
    }
    fn ok(body: Value) -> Result<Response> {
        Ok(Response { status: 200, body })
    }
    fn status(status: u16, body: Value) -> Result<Response> {
        Ok(Response { status, body })
    }
    fn live(dir: &Path, steps: Vec<(&str, Result<Response>)>) -> Live {
        Live {
            script: std::cell::RefCell::new(
                steps.into_iter().map(|(p, r)| (p.to_owned(), r)).collect(),
            ),
            posted: Default::default(),
            path: dir.join("receipt.json"),
        }
    }
    fn session_record() -> Value {
        json!({"name":"w","run_id":"r1","conversation_id":"ses_A","cwd":"/x"})
    }
    fn request() -> Value {
        json!({"request_id":"11111111-1111-4111-8111-111111111111","text":"hello"})
    }
    fn preflight() -> Vec<(&'static str, Result<Response>)> {
        vec![
            ("GET /api/session/ses_A", ok(json!({"data":{"id":"ses_A","location":{"directory":"/x"}}}))),
            ("GET /api/session/ses_A/form", ok(json!({"data":[]}))),
            ("GET /api/session/ses_A/permission", ok(json!({"data":[]}))),
            ("GET /api/session/active", ok(json!({"data":{}}))),
        ]
    }
    fn run(native: &Live, digest: &str) -> Result<Value> {
        deliver(native, &session_record(), &request(), &native.path, digest.into(), None)
    }
    #[test]
    fn a_message_is_journaled_before_the_post_and_marked_queued_when_busy() {
        let dir = tempfile::tempdir().unwrap();
        let mut steps = preflight();
        steps[3] = ("GET /api/session/active", ok(json!({"data":{"ses_A":{"type":"running"}}})));
        steps.push(("POST /api/session/ses_A/prompt", ok(json!(null))));
        let native = live(dir.path(), steps);
        struct Echo<'a>(&'a Live);
        impl Native for Echo<'_> {
            fn raw(&self, m: &str, p: &str, b: Option<&Value>) -> Result<Response> {
                if m == "POST" {
                    let _ = self.0.raw(m, p, b)?;
                    let id = b.unwrap()["id"].clone();
                    return ok(json!({"data":{"id":id,"sessionID":"ses_A","payload":{"text":"hello"}}}));
                }
                self.0.raw(m, p, b)
            }
        }
        let receipt = deliver(&Echo(&native), &session_record(), &request(), &native.path, "d".into(), None).unwrap();
        assert_eq!(receipt["status"], "submitted");
        assert_eq!(receipt["queued"], true);
        assert!(native.script.borrow().is_empty());
        let saved: Value = serde_json::from_slice(&fs::read(&native.path).unwrap()).unwrap();
        assert_eq!(saved["status"], "submitted");
    }
    #[test]
    fn a_refused_post_is_recorded_and_never_reposted() {
        let dir = tempfile::tempdir().unwrap();
        let mut steps = preflight();
        steps.push(("POST /api/session/ses_A/prompt", status(400, json!({"message":"bad"}))));
        let native = live(dir.path(), steps);
        let error = run(&native, "d").unwrap_err();
        assert_eq!(error, "OpenCode refused this message: HTTP 400: bad");
        let saved: Value = serde_json::from_slice(&fs::read(&native.path).unwrap()).unwrap();
        assert_eq!(saved["status"], "refused");
        let retry = live(dir.path(), vec![]);
        assert_eq!(run(&retry, "d").unwrap_err(), error);
        assert!(retry.posted.borrow().is_empty());
        let other = live(dir.path(), vec![]);
        assert!(run(&other, "e").unwrap_err().contains("already used"));
    }
    #[test]
    fn a_mismatched_acknowledgement_is_uncertain_without_a_repost() {
        let dir = tempfile::tempdir().unwrap();
        let mut steps = preflight();
        steps.push((
            "POST /api/session/ses_A/prompt",
            ok(json!({"data":{"id":"msg_other","sessionID":"ses_A","payload":{"text":"hello"}}})),
        ));
        let native = live(dir.path(), steps);
        assert_eq!(run(&native, "d").unwrap_err(), MISMATCH);
        assert_eq!(native.posted.borrow().len(), 1);
        let saved: Value = serde_json::from_slice(&fs::read(&native.path).unwrap()).unwrap();
        assert_eq!(saved["status"], "pending");
    }
    #[test]
    fn a_transport_error_reconciles_and_a_retry_reuses_the_same_id() {
        let dir = tempfile::tempdir().unwrap();
        let mut steps = preflight();
        steps.push(("POST /api/session/ses_A/prompt", Err("reset".into())));
        steps.push(("GET /api/session/ses_A/message/ID", status(500, json!({}))));
        let native = live(dir.path(), steps);
        // The message ID is generated inside, so match the path loosely.
        struct Loose<'a>(&'a Live);
        impl Native for Loose<'_> {
            fn raw(&self, m: &str, p: &str, b: Option<&Value>) -> Result<Response> {
                let p = if p.contains("/message/") { "/api/session/ses_A/message/ID" } else { p };
                self.0.raw(m, p, b)
            }
        }
        let error = deliver(&Loose(&native), &session_record(), &request(), &native.path, "d".into(), None).unwrap_err();
        assert!(error.starts_with("Delivery uncertain"));
        let first = native.posted.borrow()[0]["id"].as_str().unwrap().to_owned();
        let saved: Value = serde_json::from_slice(&fs::read(&native.path).unwrap()).unwrap();
        assert_eq!(saved["status"], "pending");
        assert_eq!(saved["native_message_id"], first);
        // Same request and digest: reconcile only; the message is now found.
        let retry = live(
            dir.path(),
            vec![
                ("GET /api/session/ses_A", ok(json!({"data":{"id":"ses_A","location":{"directory":"/x"}}}))),
                ("GET /api/session/ses_A/message/ID", ok(json!({"data":{"id":first,"type":"user","text":"hello"}}))),
            ],
        );
        let receipt = deliver(&Loose(&retry), &session_record(), &request(), &retry.path, "d".into(), None).unwrap();
        assert_eq!(receipt["status"], "submitted");
        assert_eq!(receipt["native_message_id"], first);
        assert!(retry.posted.borrow().is_empty());
    }
    #[test]
    fn only_a_message_not_found_404_allows_the_inbox_and_repost() {
        let record = json!({"conversation_id": "ses_A"});
        let native = Scripted(std::cell::RefCell::new(vec![(
            "GET /api/session/ses_A/message/msg_X".into(),
            404,
            json!({"_tag": "SessionNotFoundError"}),
        )]));
        assert!(reconcile(&native, &record, "msg_X", "hello").is_err());
        assert!(native.0.borrow().is_empty());
    }
    #[test]
    fn a_refused_repost_is_a_definite_refusal() {
        let record = json!({"conversation_id": "ses_A"});
        let native = Scripted(std::cell::RefCell::new(vec![
            ("GET /api/session/ses_A/message/msg_X".into(), 404, json!({"_tag": "MessageNotFoundError"})),
            ("GET /api/session/ses_A/inbox".into(), 200, json!({"data": []})),
            ("POST /api/session/ses_A/prompt".into(), 422, json!({"message": "no"})),
        ]));
        assert!(matches!(
            reconcile(&native, &record, "msg_X", "hello").unwrap(),
            Delivery::Refused(_)
        ));
    }
}
