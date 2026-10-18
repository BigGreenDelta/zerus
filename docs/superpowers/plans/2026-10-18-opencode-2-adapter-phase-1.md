# OpenCode 2 Adapter Phase 1 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the OpenCode 1.18.35 adapter with one that drives OpenCode 2's shared background service, so desktop, Android, iOS and `hgs opencode` can start and run native OpenCode conversations (including the keyless `opencode/glm-5.3-flash-free`) with history, status, Stop, queued messages, model and effort, questions and permissions.

**Architecture:** `hgs` reads OpenCode's service registration file, checks `/api/info` and the service's `/openapi.json` against a required-operation table, and talks to the service over authenticated loopback HTTP. The adapter module `src/state/opencode.rs` becomes a module root with focused submodules (`service`, `catalog`, `history`, `forms`). Bindings keep the existing name, run ID and conversation ID contract; clients keep one question-card type with additive fields.

**Tech Stack:** Rust (`hgs`, `serde_json`, existing blocking HTTP framing), Python `unittest` native qualification, Qt 6 desktop (`tray/src`), Rust mobile connector (`services/mobile/connector`), Kotlin/Compose Android, SwiftUI/`ZerusKit` iOS, GitHub Actions.

**Spec:** `docs/superpowers/specs/2026-10-17-opencode-2-adapter-design.md` (phase 1 sections). Read it before starting any task.

## Global Constraints

- Minimum OpenCode version: `2.0.32`; accept any `2.x` at or above it that passes the required-operation table.
- Never stop, restart, replace or upgrade the OpenCode service. Start it only when no registration file exists or its `pid` is dead, only via `opencode service start`, only for explicit actions and launch.
- Never run `opencode models`, `opencode auth`, `opencode run`, other `opencode service` subcommands, or any `--standalone` command against the user's data. `opencode --version` is allowed.
- Never log, journal, return or forward the service password, URL, registration file contents, provider `settings`, `headers`, `body`, `options`, URLs or credential values.
- New session IDs: `ses_` + 26 random ASCII letters and digits; new message IDs: `msg_` + 26 random ASCII letters and digits. Stored, adopted, child and fork IDs keep the existing check: expected prefix, ≤128 chars of ASCII letters, digits, `_`, `-`.
- Always send `delivery: "queue"`; never `steer`.
- Forget never calls `DELETE /api/session`.
- Keep API v1, durable request identities and the connector journal compatible. Additive fields only.
- Bounded reads: history ≤10,000 rows and ≤16 MiB per scan; responses ≤32 MiB; forms ≤32 fields, ≤64 options per field, ≤16 KiB text.
- Native tests use private `XDG_*` directories, a private service port and an offline catalog; never the developer's real OpenCode service or the Mac.
- Repository text is English. Every commit message ends with the Co-Authored-By trailer given in the implementer's instructions (omitted from the examples below).
- The repository owner explicitly authorized local commits (and push/merge by the orchestrator) for this work on 2026-10-13.

## Review Focus

- The service registration exists but the service is mid-restart (pid alive, `/api/info` times out): Zerus must report unknown state and must not start or replace anything. Test owned by Task 2.
- A message whose HTTP response is lost while the session is busy (queued in `/inbox`): resending the same request must return the original admission and never create a second message. Test owned by Task 5.
- OpenCode returns a catalog row that carries `apiKey`, `headers` or a base URL: none of it may appear in `hgs` output, caches or errors. Test owned by Task 3.
- A pre-upgrade OpenCode 1 binding (`ses_` + 32 hex characters) whose session OpenCode 2 has not migrated: it must stay listed, stopped, with only Forget allowed, and must not break listing of other sessions. Test owned by Task 4.
- An OpenCode question form with a `custom` option answered with free text on an older phone that does not know `input_kind`: `hgs` must accept the text answer for option fields with `allow_other` and reject malformed values with an explicit error. Test owned by Task 7.

---

### Task 1: Service registration, HTTP with status, and the compatibility gate

**Files:**
- Create: `src/state/opencode/service.rs`
- Modify: `src/state/opencode.rs` (declare `mod service;`, move `frame`/`decode_response`/`http_until` into `service.rs`, make `decode_response` return the status)
- Test: unit tests inside `src/state/opencode/service.rs`
- Fixtures: `tests/fixtures/opencode2/registration.json`, `tests/fixtures/opencode2/info.json` (already committed)

**Interfaces:**
- Produces:
  - `pub(crate) const MINIMUM: (u64, u64, u64) = (2, 0, 32);`
  - `pub(super) struct Registration { pub id: String, pub version: String, pub port: u16, pub pid: u32, pub password: Option<String> }`
  - `pub(super) fn registration_path() -> PathBuf`
  - `pub(super) fn parse_registration(bytes: &[u8]) -> Result<Registration>`
  - `pub(super) fn version_at_least(text: &str, minimum: (u64, u64, u64)) -> bool`
  - `pub(super) struct Response { pub status: u16, pub body: Value }`
  - `pub(super) fn request(reg: &Registration, method: &str, path: &str, body: Option<&Value>, deadline: Instant) -> Result<Response>` (never errors on non-2xx; errors only on transport/framing)
  - `pub(super) fn missing_operations(openapi: &Value) -> Vec<String>`
  - `pub(super) struct Service { pub reg: Registration }` with `pub(super) fn call(&self, method: &str, path: &str, body: Option<&Value>) -> Result<Value>` (2xx body or `Err("OpenCode HTTP <status>: <message>")`) and `pub(super) fn raw(&self, method, path, body) -> Result<Response>`
  - `pub(super) enum Unavailable { NotRunning, Disabled, Incompatible(String), Unreachable(String) }` with `fn reason(&self, computer: &str) -> String`
  - `pub(super) fn connect() -> std::result::Result<Service, Unavailable>`

- [ ] **Step 1: Write the failing tests**

Add to `src/state/opencode/service.rs`:

```rust
#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn registration_has_no_state_field_and_requires_loopback_and_pid() {
        let reg = parse_registration(include_bytes!("../../../tests/fixtures/opencode2/registration.json")).unwrap();
        assert_eq!((reg.port, reg.pid, reg.version.as_str()), (48951, 424242, "2.0.32"));
        assert_eq!(reg.password.as_deref(), Some("fixture-password"));
        for bad in [
            br#"{"id":"a","version":"2.0.32","url":"http://10.0.0.5:4096","pid":5}"#.as_slice(),
            br#"{"id":"a","version":"2.0.32","url":"https://127.0.0.1:4096","pid":5}"#,
            br#"{"id":"a","version":"2.0.32","url":"http://127.0.0.1:4096/x","pid":5}"#,
            br#"{"id":"a","version":"2.0.32","url":"http://127.0.0.1:4096","pid":1}"#,
            br#"{"id":"","version":"2.0.32","url":"http://127.0.0.1:4096","pid":5}"#,
        ] {
            assert!(parse_registration(bad).is_err(), "{}", String::from_utf8_lossy(bad));
        }
        assert!(parse_registration(&vec![b' '; 20000]).is_err());
    }

    #[test]
    fn registration_follows_xdg_state_home() {
        let _env = crate::test_env::lock();
        std::env::set_var("XDG_STATE_HOME", "/private/fixture/state");
        assert_eq!(registration_path(), PathBuf::from("/private/fixture/state/opencode/service.json"));
        std::env::remove_var("XDG_STATE_HOME");
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
        for (method, path, _) in REQUIRED {
            openapi["paths"][*path][*method] = json!({});
        }
        openapi["paths"]["/api/session"]["post"] = json!({"requestBody":{"content":{"application/json":{"schema":{"properties":{"id":{},"location":{},"model":{},"title":{},"metadata":{}}}}}}});
        openapi["paths"]["/api/session/{sessionID}/prompt"]["post"] = json!({"requestBody":{"content":{"application/json":{"schema":{"properties":{"id":{},"text":{},"delivery":{}}}}}}});
        assert!(missing_operations(&openapi).is_empty(), "{:?}", missing_operations(&openapi));
        openapi["paths"].as_object_mut().unwrap().remove("/api/session/{sessionID}/inbox");
        openapi["paths"]["/api/session/{sessionID}/prompt"]["post"]["requestBody"]["content"]["application/json"]["schema"]["properties"]
            .as_object_mut().unwrap().remove("delivery");
        assert_eq!(missing_operations(&openapi), vec![
            "GET /api/session/{sessionID}/inbox".to_owned(),
            "POST /api/session/{sessionID}/prompt field delivery".to_owned(),
        ]);
    }

    #[test]
    fn non_success_status_is_returned_not_raised() {
        let bytes = b"HTTP/1.1 404 Not Found\r\nContent-Length: 39\r\n\r\n{\"_tag\":\"MessageNotFoundError\",\"x\":1}  ";
        let response = decode_response(&bytes[..bytes.len() - 2]).unwrap();
        assert_eq!(response.status, 404);
        assert_eq!(response.body["_tag"], "MessageNotFoundError");
    }
}
```

If `crate::test_env::lock` does not exist, use the existing environment-test lock in this crate (search `fn env_lock` / `ENV_LOCK` in `src/`) and use it with the same name in every task.

- [ ] **Step 2: Run tests to verify they fail**

Run: `cargo test --locked opencode::service -- --nocapture`
Expected: compile errors (`parse_registration`, `REQUIRED`, `missing_operations` not defined).

- [ ] **Step 3: Implement**

`src/state/opencode/service.rs` (move the existing `Frame`, `frame`, chunk decoding and socket code from `opencode.rs` here and adapt):

```rust
//! OpenCode 2's shared per-user background service: discovery through its
//! registration file, the version and API gate, and authenticated loopback
//! HTTP. Zerus never stops, restarts or replaces this service.
use super::super::*;
use base64::{engine::general_purpose::STANDARD, Engine};
use sha2::{Digest, Sha256};
use std::io::{Read, Write};
use std::net::TcpStream;
use std::time::{Duration, Instant};

pub(crate) const MINIMUM: (u64, u64, u64) = (2, 0, 32);

/// Method (lowercase), path template, request fields the adapter sends.
pub(super) const REQUIRED: &[(&str, &str, &[&str])] = &[
    ("get", "/api/info", &[]),
    ("get", "/api/session/active", &[]),
    ("post", "/api/session", &["id", "location", "model", "title", "metadata"]),
    ("get", "/api/session/{sessionID}", &[]),
    ("patch", "/api/session/{sessionID}", &["title"]),
    ("post", "/api/session/{sessionID}/prompt", &["id", "text", "delivery"]),
    ("get", "/api/session/{sessionID}/message", &[]),
    ("get", "/api/session/{sessionID}/message/{messageID}", &[]),
    ("get", "/api/session/{sessionID}/inbox", &[]),
    ("delete", "/api/session/{sessionID}/inbox/{inboxID}", &[]),
    ("post", "/api/session/{sessionID}/interrupt", &[]),
    ("post", "/api/session/{sessionID}/model", &["model"]),
    ("get", "/api/session/{sessionID}/form", &[]),
    ("post", "/api/session/{sessionID}/form/{formID}/reply", &["answer"]),
    ("get", "/api/session/{sessionID}/permission", &[]),
    ("post", "/api/session/{sessionID}/permission/{requestID}/reply", &["decision"]),
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
    nonempty_env("XDG_STATE_HOME")
        .map(PathBuf::from)
        .unwrap_or_else(|| home().join(".local/state"))
        .join("opencode/service.json")
}

pub(super) fn parse_registration(bytes: &[u8]) -> Result<Registration> {
    if bytes.len() > 16384 {
        return Err("Invalid OpenCode service registration".into());
    }
    let value: Value = serde_json::from_slice(bytes).map_err(|_| "Invalid OpenCode service registration")?;
    let text = |key: &str, max: usize| {
        value[key].as_str().filter(|s| !s.is_empty() && s.len() <= max && !s.chars().any(char::is_control))
    };
    let (Some(id), Some(version), Some(url)) = (text("id", 128), text("version", 64), text("url", 64)) else {
        return Err("Invalid OpenCode service registration".into());
    };
    let rest = url.strip_prefix("http://127.0.0.1:").or_else(|| url.strip_prefix("http://localhost:"));
    let port = rest
        .map(|r| r.trim_end_matches('/'))
        .filter(|r| !r.is_empty() && r.bytes().all(|b| b.is_ascii_digit()))
        .and_then(|r| r.parse::<u16>().ok())
        .filter(|p| *p > 0)
        .ok_or("OpenCode service is not on loopback")?;
    let pid = value["pid"].as_u64().filter(|p| *p > 1 && *p <= u32::MAX as u64).ok_or("Invalid OpenCode service registration")? as u32;
    let password = match &value["password"] {
        Value::Null => None,
        Value::String(p) if !p.is_empty() && p.len() <= 512 => Some(p.clone()),
        _ => return Err("Invalid OpenCode service registration".into()),
    };
    Ok(Registration { id: id.into(), version: version.into(), port, pid, password })
}

pub(super) fn version_at_least(text: &str, minimum: (u64, u64, u64)) -> bool {
    let core = text.trim().trim_start_matches('v');
    if core.contains(['-', '+']) {
        return false;
    }
    let parts: Vec<u64> = core.split('.').map(|p| p.parse::<u64>()).collect::<std::result::Result<_, _>>().unwrap_or_default();
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
        let properties = &operation["requestBody"]["content"]["application/json"]["schema"]["properties"];
        for field in *fields {
            if properties.get(*field).is_none() {
                missing.push(format!("{} {path} field {field}", method.to_uppercase()));
            }
        }
    }
    missing
}

pub(super) struct Response {
    pub status: u16,
    pub body: Value,
}
```

Then move `Frame`, `frame` and the chunk decoder from `opencode.rs`, and change `decode_response` to `pub(super) fn decode_response(bytes: &[u8]) -> Result<Response>`: keep every framing/size check, return `Response { status, body }` for any status, parse an empty body as `Value::Null` and a non-JSON body as `Value::String` (at most 4 KiB). Add:

```rust
pub(super) fn request(reg: &Registration, method: &str, path: &str, body: Option<&Value>, deadline: Instant) -> Result<Response> {
    // Same socket loop as the former `http_until`, connecting to
    // 127.0.0.1:<reg.port>; send `Authorization: Basic base64("opencode:<password>")`
    // only when `reg.password` is Some. Transport errors keep the existing
    // "delivery uncertain" wording; they never include the URL or password.
}

pub(super) struct Service {
    pub reg: Registration,
}

impl Service {
    pub(super) fn raw(&self, method: &str, path: &str, body: Option<&Value>) -> Result<Response> {
        request(&self.reg, method, path, body, Instant::now() + Duration::from_secs(10))
    }
    pub(super) fn call(&self, method: &str, path: &str, body: Option<&Value>) -> Result<Value> {
        let response = self.raw(method, path, body)?;
        if (200..300).contains(&response.status) {
            return Ok(response.body);
        }
        let reason = response.body["message"].as_str().or(response.body["data"]["message"].as_str()).unwrap_or("Native operation refused");
        Err(format!("OpenCode HTTP {}: {}", response.status, journal::clipped(&json!(reason), 300)))
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
            Unavailable::Disabled => format!("The OpenCode background service is disabled on {computer}"),
            Unavailable::Incompatible(why) => why.clone(),
            Unavailable::Unreachable(why) => why.clone(),
        }
    }
}

fn pid_alive(pid: u32) -> bool {
    crate::platform::process_alive(pid)
}

pub(super) fn connect() -> std::result::Result<Service, Unavailable> {
    let path = registration_path();
    let Ok(bytes) = fs::read(&path) else {
        return Err(if service_disabled() { Unavailable::Disabled } else { Unavailable::NotRunning });
    };
    let reg = parse_registration(&bytes).map_err(Unavailable::Unreachable)?;
    if !pid_alive(reg.pid) {
        return Err(Unavailable::NotRunning);
    }
    let info = request(&reg, "GET", "/api/info", None, Instant::now() + Duration::from_secs(2))
        .map_err(|_| Unavailable::Unreachable("OpenCode is not responding".into()))?;
    let version = info.body["version"].as_str().unwrap_or("");
    if info.status != 200 || !version_at_least(version, MINIMUM) {
        return Err(Unavailable::Incompatible(format!(
            "OpenCode {version} is not supported; install OpenCode {}.{}.{} or newer 2.x", MINIMUM.0, MINIMUM.1, MINIMUM.2)));
    }
    let service = Service { reg };
    let missing = compatibility(&service).map_err(Unavailable::Unreachable)?;
    if let Some(first) = missing.first() {
        return Err(Unavailable::Incompatible(format!("OpenCode {version} lacks {first}; update OpenCode or Zerus")));
    }
    Ok(service)
}

/// The OpenAPI verdict, cached per service identity in private state.
fn compatibility(service: &Service) -> Result<Vec<String>> {
    let key = format!("{:x}", Sha256::digest(json!([service.reg.id, service.reg.pid, service.reg.version, service.reg.port]).to_string().as_bytes()));
    let cache = root().join("opencode/compat").join(format!("{key}.json"));
    if let Ok(bytes) = fs::read(&cache) {
        if let Ok(value) = serde_json::from_slice::<Vec<String>>(&bytes) {
            return Ok(value);
        }
    }
    let openapi = service.call("GET", "/openapi.json", None)?;
    let missing = missing_operations(&openapi);
    atomic(&cache, &json!(missing).to_string())?;
    Ok(missing)
}

/// `opencode service` keeps `disabled: true` in its private configuration.
fn service_disabled() -> bool {
    let path = nonempty_env("XDG_CONFIG_HOME").map(PathBuf::from).unwrap_or_else(|| home().join(".config")).join("opencode/service.json");
    fs::read(path).ok().and_then(|b| serde_json::from_slice::<Value>(&b).ok()).is_some_and(|v| v["disabled"] == true)
}
```

Before relying on `service_disabled`, confirm where OpenCode 2.0.32 stores service configuration: `gh api "repos/anomalyco/opencode/contents/packages/cli/src/services/service-config.ts?ref=v2.0.32" --jq .content | base64 -d`. Use exactly the path and key it reads; if it is not a JSON file with `disabled`, adapt `service_disabled` to that format and record the source line in a comment.

Add `pub fn process_alive(pid: u32) -> bool` to `src/platform/unix.rs` (`kill(pid, 0) == 0 || errno == EPERM`) and a Windows stub returning `false` in `src/platform/windows.rs`, re-exported from `src/platform/mod.rs`, unless an equivalent helper already exists (search `kill(` and `process_start` in `src/platform/`); reuse it if it does.

- [ ] **Step 4: Run tests to verify they pass**

Run: `cargo test --locked opencode::service`
Expected: 5 passed.

- [ ] **Step 5: Commit**

```bash
git add src/state/opencode.rs src/state/opencode/service.rs src/platform/
git commit -m "Discover the OpenCode 2 service and gate its API"
```

---

### Task 2: Guarded service start

**Files:**
- Modify: `src/state/opencode/service.rs`
- Test: unit tests in the same file

**Interfaces:**
- Consumes: `registration_path`, `parse_registration`, `connect`, `Unavailable` (Task 1)
- Produces:
  - `pub(super) enum StartDecision { AlreadyRunning, Start, Refuse(String) }`
  - `pub(super) fn start_decision(registration: Option<&Registration>, pid_alive: bool, disabled: bool) -> StartDecision`
  - `pub(super) fn ensure_started() -> Result<Service>` (explicit actions and launch only)

- [ ] **Step 1: Write the failing tests**

```rust
#[test]
fn start_is_refused_while_any_registered_process_lives() {
    let reg = parse_registration(include_bytes!("../../../tests/fixtures/opencode2/registration.json")).unwrap();
    assert!(matches!(start_decision(Some(&reg), true, false), StartDecision::AlreadyRunning));
    assert!(matches!(start_decision(Some(&reg), false, false), StartDecision::Start));
    assert!(matches!(start_decision(None, false, false), StartDecision::Start));
    assert!(matches!(start_decision(None, false, true), StartDecision::Refuse(_)));
}

#[test]
fn start_environment_drops_session_ssh_and_server_variables() {
    let kept = start_environment(vec![
        ("PATH".into(), "/usr/bin".into()), ("HGS_SESSION".into(), "x".into()), ("SSH_AUTH_SOCK".into(), "/s".into()),
        ("SSH_CONNECTION".into(), "1".into()), ("OPENCODE_SERVER_PASSWORD".into(), "p".into()),
        ("ZERUS_MOBILE_TOKEN".into(), "t".into()), ("HOME".into(), "/home/example".into()),
    ]);
    let names: Vec<_> = kept.iter().map(|(k, _)| k.as_str()).collect();
    assert_eq!(names, vec!["PATH", "HOME"]);
}
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `cargo test --locked opencode::service::tests::start`
Expected: compile errors.

- [ ] **Step 3: Implement**

```rust
pub(super) enum StartDecision {
    AlreadyRunning,
    Start,
    Refuse(String),
}

pub(super) fn start_decision(registration: Option<&Registration>, pid_alive: bool, disabled: bool) -> StartDecision {
    if registration.is_some() && pid_alive {
        return StartDecision::AlreadyRunning;
    }
    if disabled {
        return StartDecision::Refuse("The OpenCode background service is disabled; start it in OpenCode".into());
    }
    StartDecision::Start
}

/// Variables the shared service must not inherit from `hgs`, the connector
/// or an SSH login.
pub(super) fn start_environment(vars: Vec<(String, String)>) -> Vec<(String, String)> {
    vars.into_iter()
        .filter(|(k, _)| !(k.starts_with("HGS_") || k.starts_with("SSH_") || k.starts_with("OPENCODE_SERVER_") || k.starts_with("ZERUS_MOBILE_") || k == "OPENCODE_CONFIG_CONTENT"))
        .collect()
}

pub(super) fn ensure_started() -> Result<Service> {
    let lock_path = root().join("opencode/service-start.lock");
    let _guard = lock(Some(&lock_path))?;
    match connect() {
        Ok(service) => return Ok(service),
        Err(Unavailable::NotRunning) => {}
        Err(other) => return Err(other.reason(&crate::platform::hostname())),
    }
    let reg = fs::read(registration_path()).ok().and_then(|b| parse_registration(&b).ok());
    let alive = reg.as_ref().is_some_and(|r| pid_alive(r.pid));
    match start_decision(reg.as_ref(), alive, service_disabled()) {
        StartDecision::AlreadyRunning => return Err("OpenCode is starting; try again shortly".into()),
        StartDecision::Refuse(why) => return Err(why),
        StartDecision::Start => {}
    }
    let mut command = Command::new("opencode");
    #[cfg(target_os = "linux")]
    {
        let runtime = PathBuf::from(format!("/run/user/{}", unsafe { libc::getuid() }));
        if runtime.join("systemd/private").exists() && crate::platform::available("systemd-run") {
            command = Command::new("systemd-run");
            command
                .args(["--user", "--scope", "--quiet", "--collect", "--expand-environment=no", "--unit"])
                .arg(format!("hgs-opencode-{}", uuid::Uuid::new_v4()))
                .args(["--", "opencode"])
                .env("XDG_RUNTIME_DIR", runtime);
        }
    }
    command.args(["service", "start"]).env_clear().envs(start_environment(std::env::vars().collect()))
        .current_dir(home()).stdin(Stdio::null()).stdout(Stdio::null()).stderr(Stdio::null());
    let mut child = crate::platform::spawn_detached(&mut command).map_err(|_| "OpenCode could not be started; install OpenCode 2".to_owned())?;
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
```

`crate::platform::hostname()` is the existing local machine label helper (search `fn hostname` in `src/platform/`); use whichever function `hgs` already uses for "this computer" labels.

- [ ] **Step 4: Run tests to verify they pass**

Run: `cargo test --locked opencode::service`
Expected: all service tests pass.

- [ ] **Step 5: Commit**

```bash
git add src/state/opencode/service.rs
git commit -m "Start the shared OpenCode service only when none is running"
```

---

### Task 3: Model catalog through the allowlist

**Files:**
- Create: `src/state/opencode/catalog.rs`
- Modify: `src/state/opencode.rs` (replace the OpenCode 1 `catalog()`, `configuration_json`, `file_configuration_only`, `offered_model`), `src/accounts.rs:303-315`
- Test: unit tests in `catalog.rs`; `tests/test_accounts.py` (account row shape)
- Fixture: `tests/fixtures/opencode2/models.json` (contains `fixture-secret-key` and base URLs on purpose)

**Interfaces:**
- Consumes: `connect`, `Service::call` (Task 1)
- Produces:
  - `pub(super) fn project(models: &Value) -> Vec<Value>`: allowlisted model entries `{"id","value","label","provider","provider_label","free","effort_options","effort_labels"}` with `id == value == "<providerID>/<modelID>"`, ≤256, service order
  - `pub(crate) fn catalog(location: Option<&Path>) -> Value`: `{"model_options", "settings_model", "model_catalog_source", "catalog_state", "catalog_reason", "catalog_at"}`; never starts the service; on `Unavailable` returns the last known list from `root()/opencode/catalog.json` with `catalog_state` `not_running`/`incompatible`/`unreachable`
  - `pub(super) fn model_ref(rows: &[Value], value: &str, effort: &str) -> Result<Value>` (pure): a `Model.Ref` JSON (`{"providerID","id"}` plus `"variant"` when effort is non-empty) for an exact projected entry whose `effort_options` contain a non-empty effort
  - `pub(super) fn offered_model(service: &Service, location: &Path, value: &str, effort: &str) -> Result<Value>`: fetches `/api/model` for `location`, projects, calls `model_ref`
  - `pub(crate) fn start_from_accounts() -> Result<Value>`: explicit Start OpenCode for `hgs account start native-opencode`; calls `service::ensure_started()` and returns `catalog(None)`
  - `pub(crate) fn installed_version() -> Option<String>` (`opencode --version` output, trimmed, `v` prefix removed)

- [ ] **Step 1: Write the failing tests**

```rust
#[cfg(test)]
mod tests {
    use super::*;
    fn fixture() -> Value {
        serde_json::from_slice(include_bytes!("../../../tests/fixtures/opencode2/models.json")).unwrap()
    }

    #[test]
    fn projection_keeps_only_allowlisted_fields() {
        let rows = project(&fixture());
        let text = serde_json::to_string(&rows).unwrap();
        for secret in ["fixture-secret-key", "baseURL", "settings", "headers", "127.0.0.1", "opencode.ai/inference"] {
            assert!(!text.contains(secret), "{secret} leaked: {text}");
        }
        let local = rows.iter().find(|r| r["id"] == "fixture/local").unwrap();
        assert_eq!(local["value"], "fixture/local");
        assert_eq!(local["label"], "Native fixture");
        assert_eq!(local["effort_options"], json!(["low", "medium", "high"]));
    }

    #[test]
    fn free_means_zero_input_and_output_cost() {
        let rows = project(&fixture());
        assert!(rows.iter().filter(|r| r["provider"] == "opencode").all(|r| r["free"] == true));
        assert_eq!(rows.iter().find(|r| r["id"] == "fixture/local").unwrap()["free"], false);
    }

    #[test]
    fn projection_is_bounded_and_skips_invalid_ids() {
        let mut many = json!({"data": []});
        for i in 0..400 {
            many["data"].as_array_mut().unwrap().push(json!({"id": format!("m{i}"), "providerID": "p", "name": "M", "enabled": true}));
        }
        many["data"].as_array_mut().unwrap().insert(0, json!({"id": "bad id", "providerID": "p", "enabled": true}));
        many["data"].as_array_mut().unwrap().insert(0, json!({"id": "off", "providerID": "p", "enabled": false}));
        let rows = project(&many);
        assert_eq!(rows.len(), 256);
        assert_eq!(rows[0]["id"], "p/m0");
    }
}
```

Free rule: an entry is free when `cost` is a non-empty array and every element has `input == 0` and `output == 0`, or `cost` is an object with both zero. Check `tests/fixtures/opencode2/models.json`: OpenCode free models carry zero costs; the fixture provider has `"cost": []` (not free).

- [ ] **Step 2: Run tests to verify they fail**

Run: `cargo test --locked opencode::catalog`
Expected: compile errors.

- [ ] **Step 3: Implement**

```rust
//! OpenCode 2 model catalog for New session, model change and the account row.
//! Only allowlisted fields are copied; provider settings, headers, request
//! bodies, URLs and credentials never leave this function.
use super::super::*;
use super::service::{connect, Service, Unavailable};

const MAX_MODELS: usize = 256;

fn safe_id(value: &str) -> bool {
    !value.is_empty() && value.len() <= 200 && !value.chars().any(|c| c.is_whitespace() || c.is_control() || c == '/')
}

fn free(cost: &Value) -> bool {
    let zero = |c: &Value| c["input"].as_f64() == Some(0.0) && c["output"].as_f64() == Some(0.0);
    match cost {
        Value::Array(items) => !items.is_empty() && items.iter().all(zero),
        Value::Object(_) => zero(cost),
        _ => false,
    }
}

pub(super) fn project(models: &Value) -> Vec<Value> {
    let mut rows = Vec::new();
    let mut seen = std::collections::HashSet::new();
    for model in models["data"].as_array().into_iter().flatten() {
        if rows.len() == MAX_MODELS {
            break;
        }
        let (Some(provider), Some(id)) = (model["providerID"].as_str(), model["id"].as_str()) else { continue };
        if model["enabled"] == false || !safe_id(provider) || !safe_id(id) {
            continue;
        }
        let value = format!("{provider}/{id}");
        if !seen.insert(value.clone()) {
            continue;
        }
        let label = model["name"].as_str().filter(|l| l.len() <= 160 && !l.chars().any(char::is_control)).unwrap_or(id);
        let efforts: Vec<&str> = model["variants"].as_array().into_iter().flatten()
            .filter_map(|v| v["id"].as_str()).filter(|v| safe_id(v) && v.len() <= 40).take(16).collect();
        rows.push(json!({"id": value, "value": value, "label": label, "provider": provider,
            "free": free(&model["cost"]), "effort_options": efforts, "effort_labels": efforts}));
    }
    rows
}
```

`provider_label` comes from `GET /api/provider` (`data[].id` → `data[].name`), allowlisted the same way; add it in `catalog()` after projecting.

`catalog(location)`:
1. `match connect()`; on `Err(u)` read `root()/opencode/catalog.json` (`{"location":…,"model_options":[…],"settings_model":…,"catalog_at":…}`), return it with `catalog_state` from `u` (`not_running`, `disabled`, `incompatible`, `unreachable`) and `catalog_reason = u.reason(<this computer>)`; when no cache exists return empty `model_options`.
2. On `Ok(service)`: `GET /api/model?location[directory]=<dir>` (URL-encode the directory; `location` defaults to `home()`), `GET /api/provider?location[directory]=<dir>`, `GET /api/model/default?location[directory]=<dir>`; project; `settings_model` = `"<providerID>/<id>"` of the default model (only `providerID` and `id` are read); an empty `/api/provider` right after start yields `catalog_state: "loading"`.
3. Write the cache for the home location only (atomic, ≤1 MiB) and return `catalog_state: "ready"`.

Check the exact `location` query encoding against `tests/fixtures/opencode2` probe notes: OpenAPI declares `location` as a `deepObject` (`location[directory]=...`). Verify once against a running isolated service in Task 10 and keep the test that pins it.

`model_ref(rows, value, effort)`: split `value` at the first `/`, require both halves `safe_id`, require an exact projected entry, and require `effort` to be empty or one of that entry's `effort_options`. Return `json!({"providerID": p, "id": m})`, adding `"variant": effort` when non-empty. `offered_model(service, location, value, effort)` fetches `/api/model` for `location`, projects it and calls `model_ref`.

Add `start` to `hgs account` (`src/accounts.rs` operation match): `hgs account start native-opencode [--json]` calls `crate::state::opencode::start_from_accounts()` and prints its catalog; any other account ID is rejected with `Only the native OpenCode account has a service to start`. Inspection paths never call it.

In `src/accounts.rs` replace the OpenCode row block (`platform::available("opencode") || …`) so that:
- `installed` is true only when `installed_version()` satisfies `version_at_least(.., MINIMUM)`;
- otherwise `installed: false`, `install_reason`: `"Install OpenCode 2.0.32 or newer 2.x"` (or `"OpenCode <v> is not supported; install OpenCode 2.0.32 or newer 2.x"` when an older version is present);
- `model_options`, `settings_model`, `catalog_state`, `catalog_reason`, `catalog_at` come from `crate::state::opencode::catalog(None)`;
- `home` stays `native_home(config, "opencode")`.

- [ ] **Step 4: Run tests to verify they pass**

Run: `cargo test --locked opencode::catalog && PYTHONPATH=tests python3 -m unittest test_accounts -v`
Expected: PASS. Update `tests/test_accounts.py` expectations that asserted the OpenCode 1 JSONC parser (search `opencode.json` in that file) to the new behavior: with no service running, `catalog_state == "not_running"` and `model_options == []`.

- [ ] **Step 5: Commit**

```bash
git add src/state/opencode.rs src/state/opencode/catalog.rs src/accounts.rs tests/test_accounts.py
git commit -m "List OpenCode 2 models through an allowlisted catalog"
```

---

### Task 4: Bindings, launch and OpenCode 1 adoption

**Files:**
- Modify: `src/state/opencode.rs` (`binding`, `launch`, `verify_session`, `account_context`, `snapshots`; delete `identity`, `host_dir`, `host`, `ensure_host`, `rpc`, `endpoint` host coupling)
- Modify: `src/cli.rs:1803-1810` (pass `--effort` for OpenCode)
- Test: unit tests in `src/state/opencode.rs`

**Interfaces:**
- Consumes: `connect`, `ensure_started`, `Service` (Tasks 1-2), `offered_model` (Task 3)
- Produces:
  - `pub(super) fn new_id(prefix: &str) -> String` (`prefix` + 26 random ASCII letters/digits)
  - `fn native_id(id: &str, prefix: &str) -> bool` (unchanged contract)
  - `pub(crate) fn launch(name: &str, cwd: &str, launch_id: Option<&str>, model: Option<&str>, effort: Option<&str>, dry: bool, new_only: bool) -> Result<()>`
  - `fn session_path(record: &Value, suffix: &str) -> String` → `/api/session/<conversation_id><suffix>`
  - `enum Legacy { Current, Adopted, Unsupported }`
  - `fn legacy_verdict(record: &Value, migration: &Value, session: Option<&Value>) -> Legacy` (pure)
  - `fn legacy_state(service: &Service, record: &Value) -> Legacy` (fetches `/api/experimental/migration/v1` and `GET /api/session/<id>`, then calls `legacy_verdict`)

- [ ] **Step 1: Write the failing tests**

```rust
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
    assert!(matches!(legacy_verdict(&record, &json!({"status": "completed"}), Some(&session)), Legacy::Adopted));
    assert!(matches!(legacy_verdict(&record, &json!({"status": "running"}), Some(&session)), Legacy::Unsupported));
    assert!(matches!(legacy_verdict(&record, &json!({"status": "completed"}), None), Legacy::Unsupported));
    let moved = json!({"data": {"id": "ses_0123456789abcdef0123456789abcdef", "location": {"directory": "/elsewhere"}}});
    assert!(matches!(legacy_verdict(&record, &json!({"status": "completed"}), Some(&moved)), Legacy::Unsupported));
    let current = json!({"conversation_id": "ses_x", "cwd": "/w", "service": "opencode2"});
    assert!(matches!(legacy_verdict(&current, &json!({}), None), Legacy::Current));
}
```

`legacy_verdict(record, migration, session)` is the pure decision used by `legacy_state`: a record with `"service": "opencode2"` is `Current`; otherwise it is `Adopted` only when `migration.status == "completed"` and `session.data.id == record.conversation_id` and `session.data.location.directory == record.cwd`; else `Unsupported`.

- [ ] **Step 2: Run tests to verify they fail**

Run: `cargo test --locked opencode::tests::new_ids opencode::tests::legacy`
Expected: compile errors.

- [ ] **Step 3: Implement**

```rust
pub(super) fn new_id(prefix: &str) -> String {
    use rand::Rng;
    const ALPHABET: &[u8] = b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
    let mut rng = rand::thread_rng();
    let suffix: String = (0..26).map(|_| ALPHABET[rng.gen_range(0..ALPHABET.len())] as char).collect();
    format!("{prefix}{suffix}")
}
```

If `rand` is not already a dependency, derive 26 characters from two `uuid::Uuid::new_v4()` byte arrays modulo 62 instead; do not add a crate.

Binding records written by this adapter contain `{"backend":"opencode","service":"opencode2","agent":"opencode","name","cwd","launch_id","run_id","conversation_id","created","agent_home"}` (`agent_home` = `config_home()` for `account_context` compatibility). Remove `paused`, `native_data_home`, `native_config_file`, `creation_pending` from new records; ignore them on old records (`paused` is ignored).

`launch(name, cwd, launch_id, model, effort, dry, new_only)`:
1. Keep the existing name/lock/collision checks and the permission-mode check (only `provider` mode in phase 1).
2. `let service = service::ensure_started()?;`
3. If the binding exists: `new_only` → error as today; otherwise `legacy_state` must be `Current` or `Adopted` (adopted bindings are rewritten with `"service":"opencode2"`), then `verify_session` and return.
4. Canonicalize `cwd` (must be a directory).
5. `let selected = model.map(|m| catalog::offered_model(&service, &cwd, m, effort.unwrap_or(""))).transpose()?;`
6. Build the record with `conversation_id: new_id("ses_")`, `run_id: uuid v4`, `"creation_pending": true`; `save(&record)` before the POST.
7. `POST /api/session` with `{"id", "location": {"directory": cwd}, "title": <tag>, "metadata": {"zerus": {"run_id", "launch_id"}}, "model": selected?}`.
8. On a transport error: `GET /api/session/<id>`; if it returns the same `id` and directory, continue; otherwise re-POST the same body once (OpenCode returns the existing session for a known ID) and require a matching acknowledgement. Never generate a second ID for one launch.
9. Verify `data.id == conversation_id` and `data.location.directory == cwd`, remove `creation_pending`, save.

`verify_session(service, record)`: `GET /api/session/<id>`, require the same ID and directory.

`snapshots()`: for each binding, `connect()` once per call (not per binding); on `Unavailable`, emit the existing unavailable row with `inspection_reason` = `reason`, `state: "unknown"`; for `Legacy::Unsupported`, emit `state: "stopped"`, `activity_summary: "OpenCode 1 conversation"`, `inspection_reason: "This conversation belongs to OpenCode 1, which Zerus no longer supports"`, `allowed_actions: ["forget"]`.

`src/cli.rs`: accept `--effort <variant>` for `opencode` launches (`LaunchOptions` already parses `--model`; add `effort` the same way if absent) and pass it to `state::opencode::launch`. Reject `--effort` without `--model`.

- [ ] **Step 4: Run tests to verify they pass**

Run: `cargo test --locked opencode`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add src/state/opencode.rs src/cli.rs
git commit -m "Launch OpenCode 2 sessions with Zerus IDs and adopt migrated bindings"
```

---

### Task 5: Send, queue and reconciliation

**Files:**
- Modify: `src/state/opencode.rs` (`send`, `reconcile_receipt`, `checked_request`)
- Test: unit tests with a scripted responder

**Interfaces:**
- Consumes: `Service::raw`, `new_id`, `session_path` (Tasks 1, 4)
- Produces:
  - `pub(super) trait Native { fn raw(&self, method: &str, path: &str, body: Option<&Value>) -> Result<Response>; }` implemented by `Service` and by a test double
  - `fn reconcile(native: &dyn Native, record: &Value, message_id: &str, text: &str) -> Result<Delivery>` where `enum Delivery { Delivered, Queued, Unknown }`
  - `pub(super) fn reconcile_receipt(record: &Value, receipt: &mut Value) -> Result<bool>` (same signature as today; used by `src/state/voice.rs`)

- [ ] **Step 1: Write the failing tests**

```rust
struct Scripted(std::cell::RefCell<Vec<(String, u16, Value)>>);
impl Native for Scripted {
    fn raw(&self, method: &str, path: &str, _: Option<&Value>) -> Result<Response> {
        let mut script = self.0.borrow_mut();
        let (expected, status, body) = script.remove(0);
        assert_eq!(expected, format!("{method} {path}"));
        Ok(Response { status, body })
    }
}

#[test]
fn a_queued_message_is_found_in_the_inbox_after_a_lost_response() {
    let record = json!({"conversation_id": "ses_A"});
    let inbox: Value = serde_json::from_slice(include_bytes!("../../tests/fixtures/opencode2/inbox_busy.json")).unwrap();
    let queued = inbox["data"][0]["id"].as_str().unwrap().to_owned();
    let native = Scripted(std::cell::RefCell::new(vec![
        (format!("GET /api/session/ses_A/message/{queued}"), 404, serde_json::from_slice(include_bytes!("../../tests/fixtures/opencode2/message_not_found.json")).unwrap()),
        ("GET /api/session/ses_A/inbox".into(), 200, inbox.clone()),
    ]));
    assert!(matches!(reconcile(&native, &record, &queued, "queued later").unwrap(), Delivery::Queued));
}

#[test]
fn an_unknown_message_is_reposted_with_the_same_id_and_never_twice() {
    let record = json!({"conversation_id": "ses_A"});
    let prompt: Value = serde_json::from_slice(include_bytes!("../../tests/fixtures/opencode2/prompt.json")).unwrap();
    let id = prompt["data"]["id"].as_str().unwrap().to_owned();
    let native = Scripted(std::cell::RefCell::new(vec![
        (format!("GET /api/session/ses_A/message/{id}"), 404, json!({"_tag": "MessageNotFoundError"})),
        ("GET /api/session/ses_A/inbox".into(), 200, json!({"data": []})),
        ("POST /api/session/ses_A/prompt".into(), 200, prompt.clone()),
    ]));
    assert!(matches!(reconcile(&native, &record, &id, "hello").unwrap(), Delivery::Delivered));
    assert!(native.0.borrow().is_empty());
}

#[test]
fn conflicting_text_for_the_same_id_is_uncertain() {
    let record = json!({"conversation_id": "ses_A"});
    let native = Scripted(std::cell::RefCell::new(vec![
        ("GET /api/session/ses_A/message/msg_X".into(), 200, json!({"data": {"id": "msg_X", "type": "user", "text": "other"}})),
    ]));
    assert!(reconcile(&native, &record, "msg_X", "hello").is_err());
}
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `cargo test --locked opencode::tests::a_queued opencode::tests::an_unknown opencode::tests::conflicting`
Expected: compile errors.

- [ ] **Step 3: Implement**

`reconcile`:
1. `GET /message/<id>`: 200 with `type == "user"` → compare `text` (exact) → `Delivered`, mismatch → `Err("Native message ID has conflicting text; delivery uncertain")`. 404 → step 2. Other status → `Err` uncertain.
2. `GET /inbox`: an item with this `id` → compare `payload.text` → `Queued`. Absent → step 3.
3. `POST /prompt` with `{"id","text","delivery":"queue"}`: 200 and `data.id == id` and `data.payload.text == text` → `Delivered` (a repost of a known ID returns the original admission; an unknown ID is admitted now exactly once). Otherwise `Err` uncertain.

> **Superseded 2026-10-18:** step 3 is removed. OpenCode 2.0.32 forgets an ID once `DELETE /inbox/{id}` removed its queued message, so the repost could deliver a removed message. A message absent from history and inbox stays uncertain, and a removal record consulted first makes a confirmed removal a definite non-delivery. The current rule is in the spec's "Identity, launch and delivery".

`send(record, request)`:
- keep existing validations (64 KiB, empty text, no attachments, no compaction continuation, no child target);
- `message_id = new_id("msg_")` stored in the receipt before the POST; the receipt keeps `request_id`, `digest`, `native_message_id`, `text`, `status: "pending"`, `transport: "native_opencode"`;
- a repeated request ID with the same digest calls `reconcile`;
- the session must be connected and not waiting for a question or permission; a busy session is allowed (the message queues);
- `POST /prompt` `{"id": message_id, "text": text, "delivery": "queue"}`; on success require `data.id == message_id`, `data.sessionID == conversation_id`, `data.payload.text == text`; `status` becomes `"submitted"`, and `"queued": true` when `/api/session/active` contains the session;
- on a transport error run `reconcile`; `Unknown`/`Err` leaves the receipt `pending` and returns `"Delivery uncertain; this request will not be replayed. Inspect native history before creating a new request"`.

`reconcile_receipt` keeps its signature and calls `reconcile` with `connect()`; `Delivered` and `Queued` mark the receipt `submitted`.

- [ ] **Step 4: Run tests to verify they pass**

Run: `cargo test --locked opencode`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add src/state/opencode.rs
git commit -m "Queue OpenCode messages and reconcile lost admissions exactly"
```

---

### Task 6: History, inbox rows and status

**Files:**
- Create: `src/state/opencode/history.rs`
- Modify: `src/state/opencode.rs` (`normalize`, `parent_view`, `own_state`, `inspect`; delete the OpenCode 1 `history`, `event_rows`, classic child roster `child_*`, `CHILD_*` constants and their tests)
- Test: unit tests in `history.rs` using `messages.json`, `messages_question.json`, `messages_error.json`, `inbox_busy.json`, `active_busy.json`

**Interfaces:**
- Consumes: `Service::call`, `session_path`
- Produces:
  - `pub(super) fn fetch(service: &Service, record: &Value) -> Result<Page>` with `struct Page { messages: Vec<Value>, truncated: bool }` in chronological order
  - `pub(super) fn rows(messages: &[Value]) -> Vec<Value>`: Activity events
  - `pub(super) fn queued_rows(inbox: &Value, stopped: bool) -> Vec<Value>`
  - `pub(super) fn last_error(messages: &[Value]) -> Option<Value>`

- [ ] **Step 1: Write the failing tests**

```rust
#[cfg(test)]
mod tests {
    use super::*;
    fn load(name: &str) -> Value {
        let path = format!("{}/tests/fixtures/opencode2/{name}.json", env!("CARGO_MANIFEST_DIR"));
        serde_json::from_slice(&std::fs::read(path).unwrap()).unwrap()
    }
    fn chronological(page: Value) -> Vec<Value> {
        let mut m = page["data"].as_array().unwrap().clone();
        m.reverse();
        m
    }

    #[test]
    fn user_assistant_and_idle_messages_become_activity_events() {
        let rows = rows(&chronological(load("messages")));
        let types: Vec<_> = rows.iter().map(|r| r["type"].as_str().unwrap()).collect();
        assert_eq!(types, vec!["UserPromptSubmit", "AgentMessage", "Stop"]);
        assert_eq!(rows[0]["detail"], "hello");
        assert_eq!(rows[1]["detail"], "Native OpenCode reply");
        assert_eq!(rows[1]["reply_id"], rows[1]["message_id"]);
        assert!(rows.iter().all(|r| r["source"] == "opencode_native" && r["message_id"].as_str().unwrap().starts_with("msg_")));
    }

    #[test]
    fn tool_parts_become_pre_and_post_tool_events() {
        let rows = rows(&chronological(load("messages_question")));
        let pre = rows.iter().find(|r| r["type"] == "PreToolUse").unwrap();
        assert_eq!(pre["tool"], "question");
        let post = rows.iter().find(|r| r["type"] == "PostToolUse").unwrap();
        assert!(post["detail"].as_str().unwrap().contains("User has answered"));
    }

    #[test]
    fn a_failed_idle_is_a_provider_error() {
        let messages = chronological(load("messages_error"));
        let error = last_error(&messages).unwrap();
        assert_eq!(error["message"], "Fixture provider rejected");
        assert!(rows(&messages).iter().any(|r| r["type"] == "StopFailure"));
    }

    #[test]
    fn inbox_items_are_queued_and_after_stop_not_delivered() {
        let inbox = load("inbox_busy");
        let queued = queued_rows(&inbox, false);
        assert_eq!(queued[0]["type"], "UserPromptSubmit");
        assert_eq!(queued[0]["delivery_state"], "queued");
        assert_eq!(queued[0]["detail"], "queued later");
        assert_eq!(queued_rows(&inbox, true)[0]["delivery_state"], "not_delivered");
    }
}
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `cargo test --locked opencode::history`
Expected: compile errors.

- [ ] **Step 3: Implement**

Message mapping (v2 `GET /message` returns newest first with `cursor.next`):

| v2 message | Activity event |
| --- | --- |
| `type: "user"` | `UserPromptSubmit`, `detail` = `text` |
| `type: "assistant"`, each `content[]` `type: "text"` | `AgentMessage`, `detail` = joined text, `reply_id` = message ID |
| `content[]` `type: "tool"` | `PreToolUse` (`tool` = `name`, `detail` = `journal::clipped(state.input, 32000)`), then when `state.status` is `completed`/`error`: `PostToolUse` (`detail` = joined `state.content[].text` or `state.error`) |
| `type: "idle"`, `outcome: "succeeded"` | `Stop` |
| `type: "idle"`, `outcome: "failed"` | `StopFailure`, `detail` = `error.message` |
| `type: "idle"`, other outcome | `Stop`, `detail` = outcome |
| other types (`compaction`, reasoning parts) | skipped |

Every event: `{"type","detail","tool","at": time.created/1000,"source":"opencode_native","agent_id":"","message_id": id,"reply_id": …}`. Tool events use `message_id` = `<message id>:<part id>` so they stay unique.

`fetch`: page with `limit=100`, follow `cursor.next` (append `&cursor=<value>` URL-encoded) newest-first until `cursor.next` is null, stopping at 10,000 messages or 16 MiB cumulative (`truncated: true`); deadline 20 s; reverse to chronological order. Cache the chronological list in `root()/opencode/history/<sha256(conversation_id)>.json` keyed by the newest message ID; when the first page's newest ID equals the cached newest ID, reuse the cache without further pages.

`normalize(record, session, active, messages, inbox, forms, permissions, available)` keeps the existing output keys (`state`, `phase`, `activity`, `activity_summary`, `events`, `pending_questions`, `send_available`, …) with:
- `phase`: `unknown` if unavailable; `input` if forms or permissions pending; `working` if active; `error` if `last_error` is newer than the newest user message; else `idle`;
- `alive` = available (no paused state);
- `events` = `rows(messages)` + `queued_rows(inbox, !active)`;
- `send_available` = available and no pending cards (busy is allowed);
- `interrupt_supported` = available and active;
- `settings_change_supported` = available and not active and no pending cards;
- `effort` = `session.model.variant` unless it is `"default"`;
- `settings_model` = `"<providerID>/<id>"`;
- `history_truncated` from `Page`;
- `allowed_actions`: `["rename","forget"]` plus `"remove_queued"` when an inbox item is `not_delivered`; unavailable bindings get `["forget"]`;
- `action_reasons`: `pause`/`resume` → `"OpenCode sessions have no separate process to pause"`, `archive`/`restore` → `"OpenCode archives are not supported"`, `fork` → `"OpenCode fork arrives in a later update"`, `terminate` → `"Use Stop for this OpenCode session"`, `compact_context_reason` → `"OpenCode compaction arrives in a later update"`;
- `terminal_supported: false`, `attachments_supported: false`, `native_ui_available: false`;
- provider errors keep `provider_errors::event`/`apply` with ID `opencode-error-<idle message id>`.

`parent_view`: `connect()`; `verify_session`; `GET /api/session/active`; `GET /form` and `GET /permission` for the session; `fetch`; `GET /inbox`; then `normalize`, plus `model_options` from `catalog(Some(cwd))`, attachments/voice/read-marks decoration as today. Remove `child_roster` (phase 2 re-adds children); `processes` stays the existing "not qualified" placeholder object.

- [ ] **Step 4: Run tests to verify they pass**

Run: `cargo test --locked opencode`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add src/state/opencode.rs src/state/opencode/history.rs
git commit -m "Read OpenCode 2 history, queue and status"
```

---

### Task 7: Question forms and permissions

**Files:**
- Create: `src/state/opencode/forms.rs`, `tests/fixtures/opencode-form-contract.json`
- Modify: `src/state/opencode.rs` (`cards`, `answer`)
- Test: unit tests in `forms.rs` driven by the contract file

**Interfaces:**
- Produces:
  - `pub(super) fn cards(record: &Value, forms: &Value, permissions: &Value) -> Vec<Value>` (question card objects in the existing shape)
  - `pub(super) fn form_answer(card: &Value, form: &Value, answers: &[Value]) -> Result<Value>` → `{"answer": {<key>: value}}`
  - `pub(super) fn permission_decision(answers: &[Value]) -> Result<&'static str>` → `"once" | "always" | "reject"`

- [ ] **Step 1: Write the contract and failing tests**

`tests/fixtures/opencode-form-contract.json` lists cases `{"name","form","card","answers","reply"|"error"}`. Include at least:
1. `question-single` — the probe form (`string` with `options`, `custom: true`) → card item `{"id":"q0","header":"Color","prompt":"Pick a color","options":[{"id":"Green","label":"Green","description":"First"},{"id":"Blue","label":"Blue","description":"Second"}],"multi_select":false,"allow_other":true,"required":true}`; answer `selected_option_ids:["Green"]` → `{"answer":{"q0":"Green"}}`.
2. `question-custom-text` — same form, answer `text:"Purple"` → `{"answer":{"q0":"Purple"}}`.
3. `question-custom-forbidden` — `custom:false`, answer `text:"Purple"` → error `Choose one of the offered answers`.
4. `multiselect` — `type:"multiselect"`, `maxItems:2`, options a/b/c → `multi_select:true`, `max_items:2`; answer `["a","c"]` → `{"answer":{"k":["a","c"]}}`; three answers → error `Choose at most 2 answers`.
5. `boolean` — `{"type":"boolean","default":true}` → options `Yes`/`No` with `"default_option_ids":["yes"]`; answer `["no"]` → `{"answer":{"k":false}}`.
6. `plain-string` — `{"type":"string","maxLength":5}` → `"input_kind":"text","max_length":5,"options":[]`, `allow_other:true`; answer text `"toolong"` → error `Answer is longer than 5 characters`.
7. `unsupported-number` — a `number` field → card `can_answer:false`, `answerable:false`, `unavailable_reason:"Answer this form in OpenCode"`.
8. `oversized` — 33 fields → `can_answer:false` with the same reason.
9. `conditional` — a field with `when` → phase 1 marks the card `can_answer:false` with the same reason.
10. `permission` — the probe permission → item `{"id":"permission","header":"Permission","prompt":"shell\nprintf native-tool-output","options":[once/always/reject]}` with labels `Allow once`, `Always allow`, `Reject` and the reject option `description: "Rejects every pending request in this session"`; answer `["always"]` → `{"decision":"always"}`.

```rust
#[test]
fn contract_cases() {
    let contract: Value = serde_json::from_slice(include_bytes!("../../../tests/fixtures/opencode-form-contract.json")).unwrap();
    for case in contract["cases"].as_array().unwrap() {
        let record = json!({"run_id": "run", "conversation_id": "ses_A"});
        let name = case["name"].as_str().unwrap();
        let (forms, permissions) = if name == "permission" { (json!([]), json!([case["form"]])) } else { (json!([case["form"]]), json!([])) };
        let cards = cards(&record, &forms, &permissions);
        assert_eq!(cards.len(), 1, "{name}");
        for (key, expected) in case["card"].as_object().unwrap() {
            let actual = if key == "item" { &cards[0]["questions"][0] } else { &cards[0][key.as_str()] };
            if key == "item" {
                for (k, v) in expected.as_object().unwrap() { assert_eq!(&actual[k.as_str()], v, "{name}.{k}"); }
            } else { assert_eq!(actual, expected, "{name}.{key}"); }
        }
        if case.get("answers").is_none() { continue; }
        let answers = case["answers"].as_array().unwrap();
        let result = if name == "permission" { permission_decision(answers).map(|d| json!({"decision": d})) }
                     else { form_answer(&cards[0], &case["form"], answers) };
        match (case.get("reply"), case.get("error")) {
            (Some(reply), _) => assert_eq!(&result.unwrap(), reply, "{name}"),
            (_, Some(error)) => assert_eq!(result.unwrap_err(), error.as_str().unwrap(), "{name}"),
            _ => panic!("{name} needs reply or error"),
        }
    }
}
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `cargo test --locked opencode::forms`
Expected: compile errors.

- [ ] **Step 3: Implement**

Card shape stays the existing one (`question_id` = form or permission ID, `question_hash` = SHA-256 of `[run_id, conversation_id, native]`, `kind` = `opencode_form` or `opencode_permission`, `status: "pending"`, `answer_transport: "native_opencode"`, `agent_id: "main"`, `source: "opencode_native"`, `approval` true for permissions). Option IDs are the option `value` (≤200 chars, otherwise the index as a string). Bounds: >32 fields, >64 options, or >16 KiB of text → `can_answer:false`. `hidden` fields are omitted and their `default` submitted. Any field with `when`, or of type `number`, `integer` or `external`, makes the whole card `can_answer:false` in phase 1.

`answer(record, request)` keeps the existing answer journal and hash check, re-reads the pending card, and posts either `POST /form/<id>/reply` with `form_answer(...)` or `POST /permission/<id>/reply` with `{"decision": …}`. A 4xx from OpenCode (`FormAlreadySettled`, `FormNotFound`, `FormInvalidAnswer`) returns `"This question changed or is no longer pending"` without retry.

- [ ] **Step 4: Run tests to verify they pass**

Run: `cargo test --locked opencode`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add src/state/opencode.rs src/state/opencode/forms.rs tests/fixtures/opencode-form-contract.json
git commit -m "Answer OpenCode question forms and permissions"
```

---

### Task 8: Stop, queued removal, model and effort, rename, forget

**Files:**
- Modify: `src/state/opencode.rs` (`interrupt`, `action`, `session_action`, `dispatch` `settings`), `src/state/session_action.rs` (route `remove_queued` for OpenCode), `src/state/interrupt.rs` only if the OpenCode branch needs the new turn identity
- Test: unit tests for request validation; native coverage in Task 10

**Interfaces:**
- Consumes: Tasks 1-7
- Produces:
  - session action kind `"remove_queued"` with request field `message_id`
  - `settings` accepts `{"model": "<provider>/<id>", "effort": "<variant>"|""}`

- [ ] **Step 1: Write the failing tests**

```rust
#[test]
fn settings_validate_effort_against_the_model_variants() {
    let models: Value = serde_json::from_slice(include_bytes!("../../tests/fixtures/opencode2/models.json")).unwrap();
    let rows = catalog::project(&models);
    assert_eq!(catalog::model_ref(&rows, "fixture/local", "high").unwrap(), json!({"providerID": "fixture", "id": "local", "variant": "high"}));
    assert_eq!(catalog::model_ref(&rows, "fixture/local", "").unwrap(), json!({"providerID": "fixture", "id": "local"}));
    assert!(catalog::model_ref(&rows, "fixture/local", "ultra").is_err());
    assert!(catalog::model_ref(&rows, "fixture/missing", "").is_err());
}

#[test]
fn remove_queued_requires_an_exact_message_id() {
    assert!(queued_id(&json!({"message_id": "msg_gVmstesSuU81H2vTHdmxp65tNI"})).is_ok());
    assert!(queued_id(&json!({"message_id": "../x"})).is_err());
    assert!(queued_id(&json!({})).is_err());
}
```

`catalog::model_ref` and `catalog::offered_model` come from Task 3; `settings` uses `offered_model` with the session folder.

- [ ] **Step 2: Run tests to verify they fail**

Run: `cargo test --locked opencode::tests::settings opencode::tests::remove_queued`
Expected: compile errors.

- [ ] **Step 3: Implement**

- `interrupt(record)`: identity checks as today; require `interrupt_supported`; `POST /interrupt`; success when the body has `interrupted: true` or the session is no longer in `/api/session/active`.
- `remove_queued`: validate `message_id` with `native_id(.., "msg_")`, require it in `/inbox`, `DELETE /inbox/<id>`, return the updated state. Clients must confirm before calling it.
- `settings`: require `settings_change_supported`; `model_ref` against `catalog(Some(cwd))`'s full list for the session folder; `POST /model` with `{"model": ref}`; verify `GET /api/session/<id>` returns the same `providerID`, `id` and (when given) `variant`; keep the settings journal file.
- `rename`: keep the name rules; `PATCH /api/session/<id>` with only `{"title": new_tag}`; verify the title with a `GET`.
- `forget`: remove the binding file only.
- `dispatch`: drop `pause`/`resume`; their CLI calls return `OpenCode sessions have no separate process to pause`.

- [ ] **Step 4: Run tests to verify they pass**

Run: `cargo test --locked opencode && cargo clippy --locked --all-targets -- -D warnings`
Expected: PASS, no warnings.

- [ ] **Step 5: Commit**

```bash
git add src/state/
git commit -m "Stop, unqueue, rename and change model and effort for OpenCode 2"
```

---

### Task 9: Remove OpenCode 1 leftovers and update documentation

**Files:**
- Modify: `src/state/opencode.rs` (delete dead OpenCode 1 code and tests: host metadata, `ensure_host`, `history` events, JSONC parser, `child_*`), `src/state/backend/mod.rs` (`OPENCODE_TERMINAL_REASON` text if it mentions the API host), any `opencode::` caller that used removed fields (`account_context`)
- Modify: `docs/opencode.md`, `docs/reference.md` (OpenCode sections), `docs/local-models.md` (OpenCode part), `CONTRIBUTING.md` (native suite command), `AGENTS.md`/`CLAUDE.md` only if they mention 1.18.35
- Test: `cargo test --locked`, `python3 scripts/check-source-privacy.py`

- [ ] **Step 1: Find leftovers**

Run: `grep -rn -E '1\.18\.35|ensure_host|host\.json|/history\?|session\.next\.|OPENCODE_CONFIG_CONTENT|classic' src/ docs/ tests/ CONTRIBUTING.md`
Expected: every hit is either removed in this task or intentionally historical (spec/plan files under `docs/superpowers/`).

- [ ] **Step 2: Delete dead code and fix callers**

Remove each hit in `src/`; `account_context` returns `{"provider":"opencode","id":"native-opencode","home": record.agent_home or config_home(),"run_id"}`.

- [ ] **Step 3: Rewrite `docs/opencode.md`**

Required sections (English, concise, same tone as today): Supported versions (`2.0.32` or newer 2.x, API gate, what an incompatibility looks like); The shared service (Zerus uses OpenCode's background service, never stops it, starts it only when not running, how to start it yourself with `opencode service start`); Models (keyless free models such as `opencode/glm-5.3-flash-free`, effort variants, configured providers); Start a conversation (desktop, phones, CLI examples with `--model` and `--effort`); Controls (send, queued messages and Remove, Stop, questions, permissions, model and effort, rename, Forget; unavailable: pause/resume, Terminal, attachments, compaction, fork, children until a later update); OpenCode 1 conversations (adopted after OpenCode 2 migrates them, otherwise Forget only); Privacy (password and URL stay local, catalog allowlist); Qualification (the native suite command from Task 10). Update the local model guide's OpenCode configuration example to OpenCode 2 (`provider` entries are unchanged; remove `OPENCODE_CONFIG_CONTENT` text).

- [ ] **Step 4: Verify**

Run: `cargo test --locked && python3 scripts/check-source-privacy.py`
Expected: PASS, 0 findings.

- [ ] **Step 5: Commit**

```bash
git add -A src docs CONTRIBUTING.md
git commit -m "Remove OpenCode 1 support and document the OpenCode 2 adapter"
```

---

### Task 10: Native qualification and CI

**Files:**
- Rewrite: `tests/test_opencode_native.py`
- Modify: `scripts/ci/agent-compatibility.json`, `scripts/ci/agent-compatibility.py`, `.github/workflows/agent-upstream.yml`, `docs/ci-and-aur.md`

**Interfaces:**
- Consumes: the `hgs` commands from Tasks 4-8

- [ ] **Step 1: Rewrite the native suite**

Keep the module docstring contract (opt-in with `ZERUS_TEST_OPENCODE_NATIVE=1`; missing binary is a failure when opted in). Setup:
- version check: `opencode --version` must satisfy ≥2.0.32 and major 2;
- private root with `home`, `xdg-config/opencode/opencode.json` (fixture provider as today, `permission: {"*":"ask","question":"allow","read":"allow"}`), `xdg-data`, `xdg-cache`, `xdg-state`, `workspace`, `hgs-config`, `hgs-state`;
- env: `HOME`, `XDG_CONFIG_HOME`, `XDG_DATA_HOME`, `XDG_CACHE_HOME`, `XDG_STATE_HOME`, `OPENCODE_DISABLE_MODELS_FETCH=true`, `OPENCODE_DISABLE_AUTOUPDATE=true`, `OPENCODE_DISABLE_LSP=true`, `HGS_CONFIG_DIR`, `HGS_STATE_DIR`, `HGS_PEERS=''`, `HGS_SELF=fixture`, `HGS_TAB=0`, `PATH`, `LANG`;
- a free private port: `opencode service set port <port>` under that env before any test;
- teardown: `opencode service stop` under the private env, then verify no process contains the private root (`pgrep -f`), then the model server and temp dir.

Fixture model: tools are `question`, `shell` and `subagent` in OpenCode 2 (`QUESTION_NATIVE` → `question`, `BASH_NATIVE` → `shell` with `{"command": ...}`, `ERROR_NATIVE` → HTTP 401).

Tests (each creates its own session through `hgs opencode <workspace> --new -n <tag> -d --launch-id <uuid> --account native-opencode --model fixture/local`):
1. `test_01_not_running_reports_unknown_and_never_starts`: before launch, `hgs inspect` of a saved binding reports `inspection_unavailable` with `OpenCode is not running`, and no registration file appears.
2. `test_02_launch_starts_service_with_own_ids_and_history`: launch starts the service; `conversation_id` matches `^ses_[A-Za-z0-9]{26}$`; `GET /api/session/<id>` (via the registration file) has `metadata.zerus.run_id`; sending `hello` yields `UserPromptSubmit`/`AgentMessage`/`Stop` events.
3. `test_03_lost_response_and_queue_never_duplicate`: hold the model (`HOLD_NATIVE`), send a second message, simulate a lost response by calling `hgs send` again with the same request ID → receipt `submitted`, one inbox item; Stop leaves it `not_delivered`; `remove_queued` deletes it; no duplicate user messages in `/message`.
4. `test_04_question_and_permission_answers`: question form answered with `Green`; permission answered `once`; tool output appears.
5. `test_05_model_effort_and_provider_error`: `settings` to `fixture/alternate` with effort `high` verified via the session; `ERROR_NATIVE` yields `phase: "error"` with `Fixture provider rejected`.
6. `test_06_legacy_binding`: write an OpenCode 1 binding file (`ses_` + 32 hex, no `service` key) → listed stopped with only `forget`, while a current session in the same listing stays fully inspectable; forget removes only the binding.
7. `test_07_incompatible_service`: a fake registration file pointing to a local HTTP stub that returns `{"version":"2.0.31"}` for `/api/info` → `Incompatible` reason, and a stub with version `2.0.40` but an `/openapi.json` missing `/api/session/{sessionID}/inbox` → reason names that route; neither starts a service.
8. `test_08_never_replaces_a_live_registration`: a registration whose `pid` is this test process (alive) and whose port answers nothing → launch fails with `OpenCode is not responding`/`starting`, the registration file is unchanged and no `opencode` process was started.

- [ ] **Step 2: Run the suite locally**

Run:
```bash
cargo build --locked
ZERUS_TEST_OPENCODE_NATIVE=1 HGS_TEST_BIN="$PWD/target/debug/hgs" PYTHONPATH=tests python3 -m unittest test_opencode_native -v
```
Expected: 8 passed against the installed OpenCode 2.0.32. Afterwards `pgrep -af zerus-opencode-native` prints nothing.

- [ ] **Step 3: Add OpenCode to the pinned native matrix**

In `scripts/ci/agent-compatibility.json` add:

```json
"opencode": [
  {"version": "2.0.32", "target": "linux-x64-baseline",
   "url": "https://registry.npmjs.org/@opencode/cli-linux-x64-baseline/-/cli-linux-x64-baseline-2.0.32.tgz",
   "integrity": "<from npm>"},
  {"version": "2.0.32", "target": "darwin-arm64",
   "url": "https://registry.npmjs.org/@opencode/cli-darwin-arm64/-/cli-darwin-arm64-2.0.32.tgz",
   "integrity": "<from npm>"}
]
```

Fill each `integrity` with `npm view @opencode/cli-<target>@2.0.32 dist.integrity` (the darwin value was verified on 2026-10-17; the linux-x64-baseline tarball's SHA-256 is `2ee2de875e01ae1164850f2909e4abb7c43f21f065cb07ec9c6eeac1041adafa`). In `agent-compatibility.py` add an `'opencode'` entry beside `codex`/`claude`: `variable='ZERUS_TEST_OPENCODE_BIN'`, `version_output='opencode v{}'`, tarball/metadata templates for `@opencode/cli-{target}`, `members={'linux-x64-baseline':'package/bin/opencode','darwin-arm64':'package/bin/opencode'}`, `suites=['test_opencode_native']`, environment `ZERUS_TEST_OPENCODE_NATIVE=1`; map the Linux host target to `linux-x64-baseline`. Extend upstream discovery in `agent-upstream.yml` / `discover_upstream` to report the newest `@opencode/cli` release and run the suite against it without changing the pin (the OpenCode minimum policy).

- [ ] **Step 4: Validate CI locally**

Run: `python3 scripts/ci/agent-compatibility.py --help` and the repository's workflow validation from `docs/ci-and-aur.md` (actionlint or the documented script).
Expected: no errors.

- [ ] **Step 5: Document and commit**

Add OpenCode to the native compatibility section of `docs/ci-and-aur.md` (pinned minimum, daily newest-release canary, private service port).

```bash
git add tests/test_opencode_native.py scripts/ci/ .github/workflows/agent-upstream.yml docs/ci-and-aur.md
git commit -m "Qualify the OpenCode 2 adapter natively in CI"
```

---

### Task 11: Desktop

**Files:**
- Modify: `tray/src/NewSessionDialog.cpp`/`.h` (effort combo for OpenCode), `tray/src/HgsClient.cpp` (pass `--effort`, `remove_queued`), `tray/src/ActivityView.cpp` (queued and not delivered labels, Remove), `tray/src/NativeSessionCapabilities.h` (OpenCode defaults no longer list pause/resume), `tray/src/AccountsPage.cpp` (install reason and `catalog_reason` text)
- Test: the Qt test files that cover these classes (search `tray/tests` for `NewSessionDialog`, `ActivityView`, `NativeSessionCapabilities`)

- [ ] **Step 1: Write failing Qt tests**

1. New session with agent `opencode`, account `native-opencode` whose `model_options` contains `fixture/local` with `effort_options ["low","medium","high"]`: the effort combo shows `Default`, `low`, `medium`, `high`; choosing `high` emits the launch with effort `high`; switching to a model without effort options hides the combo and clears the value.
2. Activity with an event `{"type":"UserPromptSubmit","delivery_state":"queued"}` shows `Queued`; with `not_delivered` shows `Not delivered` and a `Remove` button visible only when `allowed_actions` contains `remove_queued`; clicking Remove asks for confirmation and calls `session-action … remove_queued` with that `message_id`; Cancel sends nothing.
3. `NativeSessionCapabilities::allowed` for an OpenCode session without reported actions allows only `rename` and `forget`.
4. Accounts shows `install_reason` when `installed` is false and `catalog_reason` when `catalog_state` is not `ready`; when `catalog_state` is `not_running` it shows `Start OpenCode on <computer>`, which runs `hgs [@host] account start native-opencode --json` and refreshes the account on success. New session with OpenCode selected shows the same button when the selected computer reports `not_running`.

- [ ] **Step 2: Run them to verify they fail**

Run: `bash scripts/ci/gui.sh build && bash scripts/ci/gui.sh test` (or the narrower ctest target documented in `CONTRIBUTING.md`).
Expected: the new tests fail.

- [ ] **Step 3: Implement**

Follow the existing model combo code in `NewSessionDialog::updateModels()`; add `m_effort` populated from the selected option's `effort_options` (bounded to 16, validated like model IDs), hidden for other agents. `launchRequested` gains an effort argument passed to `hgs opencode … --effort <value>` only when non-empty. Activity labels reuse the existing small status label style used for voice states (`ActivityView.cpp` labels near line 902). Remove uses `HgsClient::runSessionAction(host, name, "remove_queued", {{"expected_run_id",…},{"expected_conversation_id",…},{"message_id",id}})` behind a `QMessageBox` confirmation stating `Remove this queued message? It has not been delivered to OpenCode.`

- [ ] **Step 4: Run tests to verify they pass**

Run: `bash scripts/ci/gui.sh test`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add tray/
git commit -m "Offer OpenCode effort and queued message removal on the desktop"
```

---

### Task 12: Mobile connector

**Files:**
- Modify: `services/mobile/connector/src/launch.rs` (accept `effort` for OpenCode), `services/mobile/connector/src/mutations.rs` (OpenCode effort validated against `effort_options` instead of `EFFORTS`), `services/mobile/connector/src/capabilities.rs` (advertise `opencode_launch_effort`), `services/mobile/connector/src/accounts.rs` (forward `effort_options`, `free`, `catalog_state`, `catalog_reason`, `install_reason` within existing bounds), `services/mobile/connector/src/execute.rs` or the session-action path (allow `remove_queued` with `message_id`)
- Modify: `docs/mobile-architecture.md` (launch effort, queued delivery state, OpenCode question fields)
- Test: connector unit tests beside each change; run `cargo test --locked` in `services/mobile/connector`

- [ ] **Step 1: Write failing tests**

1. A launch payload `{"agent":"opencode","account_id":"native-opencode","model":"fixture/local","effort":"high", …}` is accepted when the account's `model_options` lists `fixture/local` with `effort_options` containing `high`, and passes `--effort high` to `hgs`; `effort:"ultra"` is rejected with `effort is not offered for this model`; `effort` for another agent is rejected exactly as today.
2. The capability list includes `opencode_launch_effort`.
3. An account projection containing OpenCode entries with `effort_options`, `free` and `catalog_state` keeps them and stays within the 128 KiB account limit.
4. `remove_queued` requires a `msg_` ID and exact run/conversation identity, and is journaled like other session actions.
5. Question cards with `input_kind`, `max_length`, `default_option_ids`, `can_answer:false` and `unavailable_reason` pass through unchanged.

- [ ] **Step 2: Run them to verify they fail**

Run: `cd services/mobile/connector && cargo test --locked`
Expected: new tests fail.

- [ ] **Step 3: Implement**

Keep API v1 compatibility: all fields are additive; the old Python connector stays untouched (it never advertises `opencode_launch_effort`, so apps do not send effort to it).

- [ ] **Step 4: Run tests to verify they pass**

Run: `cd services/mobile/connector && cargo test --locked && cargo clippy --locked --all-targets -- -D warnings`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add services/mobile/connector docs/mobile-architecture.md
git commit -m "Carry OpenCode effort and queued removal through the Rust connector"
```

---

### Task 13: Android

**Files:**
- Modify: OpenCode launch and model sheet code (search `mobile/android/app/src/main` for `opencode`, `effort_options`, `model_options`), Activity message rendering for `delivery_state`, question rendering for `input_kind`/`max_length`/`default_option_ids`/`can_answer`
- Test: unit tests under `mobile/android/app/src/test` using `tests/fixtures/opencode-form-contract.json` (copy into test resources the way `optional-question-contract.json` is shared)

- [ ] **Step 1: Write failing tests**

1. Launch form for OpenCode shows an effort row when the chosen model has `effort_options` and the connector advertises `opencode_launch_effort`; the request contains `effort` only then.
2. Model sheet for an OpenCode session offers that model's `effort_options` (not the fixed list).
3. A `UserPromptSubmit` event with `delivery_state` `queued`/`not_delivered` renders `Queued`/`Not delivered` (no Remove on phones in phase 1).
4. Contract cases render: `plain-string` shows a text field with a 5-character limit; `boolean` preselects Yes; `unsupported-number` shows `Answer this form in OpenCode` and no Submit.

- [ ] **Step 2: Run to verify failure**

Run: `cd mobile/android && ./gradlew testDebugUnitTest`
Expected: new tests fail.

- [ ] **Step 3: Implement** following the existing effort sheet (`android-model-effort` behavior) and question card code.

- [ ] **Step 4: Run to verify pass**

Run: `cd mobile/android && ./gradlew testDebugUnitTest lintDebug`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add mobile/android
git commit -m "Show OpenCode effort, queued messages and form fields on Android"
```

---

### Task 14: iOS

**Files:**
- Modify: `mobile/ios/ZerusKit/Sources/ZerusKit/Launch.swift`, `SessionControls.swift`, `Events.swift`, `SessionDetails.swift`; SwiftUI views under `mobile/ios/Zerus/Launch/` and the conversation/question views
- Test: `mobile/ios/ZerusKit/Tests/ZerusKitTests/` (add the form contract file as a test resource)

- [ ] **Step 1: Write failing tests** (same four behaviors as Task 13, in `ZerusKitTests`).
- [ ] **Step 2: Run to verify failure**

Run: `cd mobile/ios/ZerusKit && swift test` (on a macOS runner; on Linux use the package's documented test command from `mobile/ios/README.md`).
Expected: new tests fail.

- [ ] **Step 3: Implement** in `ZerusKit` first (parsing and validation), then the SwiftUI views.
- [ ] **Step 4: Run to verify pass**

Run: `cd mobile/ios/ZerusKit && swift test`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add mobile/ios
git commit -m "Show OpenCode effort, queued messages and form fields on iOS"
```

---

### Task 15: Final verification

- [ ] **Step 1:** `cargo fmt --check && cargo clippy --locked --all-targets -- -D warnings && cargo test --locked`
- [ ] **Step 2:** `PYTHONPATH=tests python3 -m unittest discover -s tests` (the ordinary CLI suite, as `scripts/ci/cli.sh` runs it)
- [ ] **Step 3:** The native suite from Task 10 against the installed OpenCode 2.0.32; confirm no leftover `opencode` processes under private roots.
- [ ] **Step 4:** `python3 scripts/check-source-privacy.py`
- [ ] **Step 5:** Manual check on arch with the real OpenCode service (owner's machine, after the PR is merged and installed): New session → OpenCode → `opencode/glm-5.3-flash-free` → send a message → reply appears.
- [ ] **Step 6:** Update Beads: close phase-1 tasks under `zerus-pl2u`, file phase 2, `bd export -o .beads/issues.jsonl`, privacy check, commit.
