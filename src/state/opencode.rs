//! OpenCode 2's shared background service, authenticated on loopback.
//! Bindings hold Zerus and session identities, never provider keys.
use super::*;
#[allow(dead_code)] // the OpenCode 2 gate is consumed by later phase 1 tasks
mod catalog;
mod forms;
mod history;
mod service;
pub(crate) use catalog::{catalog, install_state, start_from_accounts};
use sha2::{Digest, Sha256};
use std::fs;
use std::io::Read;
use std::time::{Duration, Instant};

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
pub(super) fn native_id(id: &str, prefix: &str) -> bool {
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
use service::Response;

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
fn verify_session(native: &dyn Native, record: &Value) -> Result<Value> {
    exact_session(record, get(native, &session_path(record, ""))?)
}
/// The session object of a `GET /api/session/{id}` answer, only when it is
/// exactly this binding's session in this binding's directory.
fn exact_session(record: &Value, answer: Value) -> Result<Value> {
    if answer["data"]["id"] != record["conversation_id"]
        || answer["data"]["location"]["directory"] != record["cwd"]
    {
        return Err("The exact native OpenCode session or directory changed".into());
    }
    Ok(answer["data"].clone())
}
/// What one inspection read from the service for one session.
struct Observed {
    /// The session object.
    session: Value,
    active: bool,
    page: history::Page,
    /// The `/inbox` response.
    inbox: Value,
    forms: Value,
    permissions: Value,
}
/// The status of one binding; `None` when the service could not be read,
/// which keeps native state unknown rather than stopped.
fn normalize(record: &Value, observed: Option<&Observed>) -> Value {
    let available = observed.is_some();
    let null = Value::Null;
    let session = observed.map_or(&null, |o| &o.session);
    let active = observed.is_some_and(|o| o.active);
    let messages: &[Value] = observed.map_or(&[], |o| &o.page.messages);
    let pending = observed.map_or_else(Vec::new, |o| forms::cards(record, &o.forms, &o.permissions));
    let failure = history::last_error(messages);
    let phase = if !available {
        "unknown"
    } else if !pending.is_empty() {
        "input"
    } else if active {
        "working"
    } else if failure.is_some() {
        "error"
    } else {
        "idle"
    };
    let (activity, summary) = match phase {
        "input" => ("attention", "Needs attention"),
        "working" => ("busy", "Working"),
        "error" => ("attention", "Provider request failed"),
        "idle" => ("idle", "Ready"),
        _ => ("unknown", "Inspection unavailable"),
    };
    let delivered = history::rows(messages);
    // A message admitted between the inbox and history reads is shown once.
    let ids: std::collections::HashSet<&str> = messages.iter().map(|m| string(m, "id")).collect();
    let queued: Vec<Value> = observed
        .map_or_else(Vec::new, |o| history::queued_rows(&o.inbox, !active))
        .into_iter()
        .filter(|row| !ids.contains(string(row, "message_id")))
        .collect();
    // A reply becomes the session's reply once OpenCode completed it, so a
    // read mark taken while it streams never covers its final text.
    let reply = delivered
        .iter()
        .rev()
        .find(|e| e["type"] == "AgentMessage" && e["streaming"] != true);
    let prompt = delivered
        .iter()
        .rev()
        .find(|e| e["type"] == "UserPromptSubmit");
    let turn_started = prompt.and_then(|r| r["at"].as_f64()).unwrap_or(0.0);
    let mut allowed = if available {
        vec!["rename", "forget"]
    } else {
        vec!["forget"]
    };
    if !active
        && queued
            .iter()
            .any(|row| row["delivery_state"] == "not_delivered")
    {
        allowed.push("remove_queued");
    }
    let model = &session["model"];
    let settings_model = match (string(model, "providerID"), string(model, "id")) {
        ("", _) | (_, "") => Value::Null,
        (provider, id) => json!(format!("{provider}/{id}")),
    };
    let effort = model["variant"]
        .as_str()
        .filter(|variant| !variant.is_empty() && *variant != "default")
        .map_or(Value::Null, |variant| json!(variant));
    let (state, process_state, runtime_state) = if available {
        ("running", "running", "live")
    } else {
        ("unknown", "unknown", "unknown")
    };
    let parts = string(record, "name").splitn(3, '/').collect::<Vec<_>>();
    let idle = available && pending.is_empty();
    let mut events = delivered.clone();
    events.extend(queued);
    let mut value = json!({"name":record["name"],"cmd":"opencode","agent":"opencode","backend":"opencode","project":parts.get(1),"tag":parts.get(2),"run_id":record["run_id"],"conversation_id":record["conversation_id"],"launch_id":record["launch_id"],"created":record["created"],"cwd":record["cwd"],"cwd_source":"native_session","tracked":true,"resumable":true,"attached":0,"clients":[],
        "state":state,"process_state":process_state,"runtime_state":runtime_state,"conversation_state":"active","phase":phase,"activity":activity,"activity_summary":summary,
        "activity_detail":reply.map(|r|r["detail"].clone()),"prompt":prompt.map(|r|r["detail"].clone()),"reply_id":reply.map(|r|r["reply_id"].clone()),"reply_at":reply.map(|r|r["at"].clone()),
        "last_event_at":session["time"]["updated"].as_f64().unwrap_or(0.0)/1000.0,"turn_started":turn_started,"account_id":"native-opencode","account_home":record["agent_home"],
        "model":model["id"],"model_provider":model["providerID"],"settings_model":settings_model,"effort":effort});
    let controls = json!({"send_available":idle,"send_supported":idle,"settings_change_supported":idle && !active,"interrupt_supported":available && active,
        "terminal_supported":false,"terminal_reason":backend::OPENCODE_TERMINAL_REASON.text(),"native_ui_available":false,"bulk_lifecycle_supported":false,"attachments_supported":false,
        "clear_context_supported":false,"clear_context_reason":"OpenCode context reset is not available in this adapter",
        "compact_context_supported":false,"compact_context_reason":"OpenCode compaction arrives in a later update",
        "allowed_actions":allowed,"action_reasons":{"pause":"OpenCode sessions have no separate process to pause","resume":"OpenCode sessions have no separate process to pause",
            "archive":"OpenCode archives are not supported","restore":"OpenCode archives are not supported","fork":"OpenCode fork arrives in a later update","terminate":"Use Stop for this OpenCode session"},
        "pending_questions":pending,"events":events,"replace_events":true,"cursor":0,"history_truncated":observed.is_some_and(|o| o.page.truncated)});
    for (key, item) in controls.as_object().into_iter().flatten() {
        value[key] = item.clone();
    }
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
    if let (Some(failure), "error") = (&failure, phase) {
        let error = provider_errors::event(
            &format!("opencode-error-{}", string(failure, "id")),
            failure["at"].as_f64().unwrap_or(0.0),
            string(failure, "message"),
            "opencode_native",
        );
        provider_errors::apply(&mut value, &error);
    }
    value
}
/// A row without a reading of the session: no events, so clients keep the
/// transcript they already show, and the reason as its detail.
fn unread(record: &Value, reason: &str) -> Value {
    let mut row = normalize(record, None);
    if let Some(map) = row.as_object_mut() {
        map.remove("events");
        map.remove("replace_events");
    }
    row["activity_detail"] = json!(reason);
    row["inspection_reason"] = json!(reason);
    row
}
/// A binding whose service could not be read: unknown, with the reason.
fn unavailable(record: &Value, reason: String) -> Value {
    let mut row = unread(record, &reason);
    row["inspection_unavailable"] = json!(true);
    row
}
/// A binding that OpenCode 2 did not migrate: stopped, and only Forget.
fn legacy_row(record: &Value) -> Value {
    let mut row = unread(
        record,
        "This conversation belongs to OpenCode 1, which Zerus no longer supports",
    );
    row["state"] = json!("stopped");
    row["process_state"] = json!("exited");
    row["runtime_state"] = json!("stopped");
    row["activity_summary"] = json!("OpenCode 1 conversation");
    row
}
/// One GET that must succeed.
fn get(native: &dyn Native, path: &str) -> Result<Value> {
    let response = native.raw("GET", path, None)?;
    if successful(&response) {
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
/// The sessions OpenCode is running now, keyed by session ID.
fn active_sessions(native: &dyn Native) -> Result<Value> {
    let active = get(native, "/api/session/active")?;
    if !active["data"].is_object() {
        return Err("Invalid OpenCode session activity".into());
    }
    Ok(active["data"].clone())
}
fn pending_requests(native: &dyn Native, record: &Value, suffix: &str) -> Result<Value> {
    let answer = get(native, &session_path(record, suffix))?;
    if !answer["data"].is_array() {
        return Err("Invalid OpenCode pending requests".into());
    }
    Ok(answer["data"].clone())
}
/// Everything one session's status needs, given the shared activity map.
fn observe(
    native: &dyn Native,
    record: &Value,
    active: &Value,
    history: impl FnOnce() -> Result<history::Page>,
) -> Result<Observed> {
    let session = exact_session(record, get(native, &session_path(record, ""))?)?;
    let forms = pending_requests(native, record, "/form")?;
    let permissions = pending_requests(native, record, "/permission")?;
    // The inbox first: an item admitted meanwhile is then in the history
    // read; one listed in both is shown once.
    let inbox = get(native, &session_path(record, "/inbox"))?;
    if !inbox["data"].is_array() {
        return Err("Invalid OpenCode inbox".into());
    }
    let page = history()?;
    Ok(Observed {
        session,
        active: active.get(string(record, "conversation_id")).is_some(),
        page,
        inbox,
        forms,
        permissions,
    })
}
#[derive(Clone, Copy, PartialEq)]
enum Detail {
    /// A session list row: status only.
    Listing,
    /// The session's own state for an action.
    Own,
    /// `hgs inspect`: also the model options for its folder.
    Full,
}
fn processes() -> Value {
    json!({"items":[],"complete":false,"source":"opencode_native","reason":"Individual OpenCode shell process controls are not yet qualified"})
}
fn parent_view(
    service: &service::Service,
    record: &Value,
    active: &Value,
    detail: Detail,
) -> Result<Value> {
    let observed = observe(service, record, active, || history::fetch(service, record))?;
    let mut result = normalize(record, Some(&observed));
    if detail != Detail::Listing {
        result["attachment_messages"] = attachments::messages(record, "");
        result["voice_messages"] =
            conversation_history::inspected_voice_messages(record, &result, None);
    }
    if detail == Detail::Full {
        let cwd = PathBuf::from(string(record, "cwd"));
        result["model_options"] = catalog(Some(&cwd))["model_options"].clone();
    }
    let questions = result["pending_questions"]
        .as_array()
        .cloned()
        .unwrap_or_default();
    read_marks::enrich(
        &mut result,
        &read_marks::load(),
        string(record, "name"),
        Some(&questions),
    );
    Ok(result)
}
/// Inspects bindings against one service connection: at most one
/// `/api/session/active` and one migration check per instance.
struct Inspector {
    service: std::result::Result<service::Service, service::Unavailable>,
    computer: String,
    active: Option<Result<Value>>,
    migration: Option<Reply>,
    verdicts: Option<Vec<String>>,
    verdicts_changed: bool,
}
impl Inspector {
    /// Never starts the service.
    fn connect() -> Self {
        Inspector {
            service: service::connect(),
            computer: crate::platform::hostname(),
            active: None,
            migration: None,
            verdicts: None,
            verdicts_changed: false,
        }
    }
    fn row(&mut self, record: &Value, detail: Detail) -> Value {
        let mut row = self.view(record, detail);
        if detail != Detail::Listing && row.get("processes").is_none() {
            row["processes"] = processes();
        }
        row
    }
    fn view(&mut self, record: &Value, detail: Detail) -> Value {
        let service = match &self.service {
            Err(why) => return unavailable(record, why.reason(&self.computer)),
            Ok(service) => service,
        };
        let key = verdict_key(record, &service.reg);
        let known = record["service"] != "opencode2"
            && self
                .verdicts
                .get_or_insert_with(load_verdicts)
                .contains(&key);
        let state = if known {
            Ok((Legacy::Unsupported, false))
        } else {
            legacy_state(service, record, &mut self.migration)
        };
        let mut current = record.clone();
        match state {
            Err(reason) => return unavailable(record, reason),
            Ok((Legacy::Unsupported, settled)) => {
                if settled && !known {
                    let keys = self.verdicts.get_or_insert_with(load_verdicts);
                    if keys.len() >= VERDICT_LIMIT {
                        keys.clear();
                    }
                    keys.push(key);
                    self.verdicts_changed = true;
                }
                return legacy_row(record);
            }
            Ok((Legacy::Adopted, _)) => {
                adopt_binding(string(record, "name"), string(record, "conversation_id"));
                modernize(&mut current);
            }
            Ok((Legacy::Current, _)) => {}
        }
        let active = match self.active.get_or_insert_with(|| active_sessions(service)) {
            Ok(active) => active,
            Err(reason) => return unavailable(record, reason.clone()),
        };
        parent_view(service, &current, active, detail)
            .unwrap_or_else(|reason| unavailable(record, reason))
    }
    fn finish(self) {
        if self.verdicts_changed {
            save_verdicts(self.verdicts.as_deref().unwrap_or(&[]));
        }
    }
}
pub(crate) fn inspect(name: &str, agent_id: Option<&str>) -> Result<Value> {
    let record = binding(name)?;
    if agent_id.is_some() {
        return Err("OpenCode child sessions arrive in a later update".into());
    }
    let mut inspector = Inspector::connect();
    let mut value = inspector.row(&record, Detail::Full);
    inspector.finish();
    waits::decorate(std::slice::from_mut(&mut value));
    Ok(value)
}
/// The session's own state for delivery, answer, interrupt and lifecycle
/// checks; unknown with its reason while the service is unavailable.
pub(crate) fn own_state(name: &str) -> Result<Value> {
    let record = binding(name)?;
    let mut inspector = Inspector::connect();
    let value = inspector.row(&record, Detail::Own);
    inspector.finish();
    Ok(value)
}
pub(crate) fn snapshots() -> Result<Vec<Value>> {
    let Ok(entries) = fs::read_dir(bindings()) else {
        return Ok(Vec::new());
    };
    let records: Vec<Value> = entries
        .flatten()
        .take(5000)
        .filter_map(|entry| fs::read(entry.path()).ok())
        .filter_map(|bytes| serde_json::from_slice::<Value>(&bytes).ok())
        .filter(|record| record["backend"] == "opencode")
        .collect();
    if records.is_empty() {
        return Ok(Vec::new());
    }
    let mut inspector = Inspector::connect();
    let mut rows = Vec::new();
    for record in &records {
        let mut row = inspector.row(record, Detail::Listing);
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
    inspector.finish();
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
    /// The same request, ending no later than `deadline`.
    fn raw_until(
        &self,
        method: &str,
        path: &str,
        body: Option<&Value>,
        _deadline: Instant,
    ) -> Result<Response> {
        self.raw(method, path, body)
    }
}
impl Native for service::Service {
    fn raw(&self, method: &str, path: &str, body: Option<&Value>) -> Result<Response> {
        service::Service::raw(self, method, path, body)
    }
    fn raw_until(
        &self,
        method: &str,
        path: &str,
        body: Option<&Value>,
        deadline: Instant,
    ) -> Result<Response> {
        let deadline = deadline.min(Instant::now() + Duration::from_secs(10));
        service::request(&self.reg, method, path, body, deadline)
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
/// The route and body of one reply, from the pending card, the exact native
/// request it was made from, and the answers.
fn reply_for(card: &Value, native: &Value, answers: &[Value]) -> Result<(String, Value)> {
    let id = string(card, "question_id");
    if card["kind"] == "opencode_permission" {
        let decision = forms::permission_decision(answers)?;
        Ok((
            format!("/permission/{id}/reply"),
            json!({"decision": decision}),
        ))
    } else {
        Ok((
            format!("/form/{id}/reply"),
            forms::form_answer(card, native, answers)?,
        ))
    }
}
const CHANGED: &str = "This question changed or is no longer pending";
/// Server text safe to show: control characters removed, at most 200 chars.
fn clip_text(text: &str) -> String {
    text.chars().filter(|c| !c.is_control()).take(200).collect()
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
        .ok_or(CHANGED)?;
    let path = root()
        .join("opencode/answers")
        .join(format!("{}.json", string(request, "request_id")));
    answer_with(&running_service()?, record, request, card, &path)
}
/// Core of `answer`: the journal, the exact native request and the reply.
fn answer_with(
    native: &dyn Native,
    record: &Value,
    request: &Value,
    card: &Value,
    path: &Path,
) -> Result<Value> {
    if path.exists() {
        return Err("This answer was already attempted; refresh its native acknowledgement".into());
    }
    let raw = request["answers"]
        .as_array()
        .ok_or("Answers are required")?;
    // The exact native request the card was made from, read again now.
    let suffix = if card["kind"] == "opencode_permission" {
        "/permission"
    } else {
        "/form"
    };
    let pending = pending_requests(native, record, suffix)?;
    let found = pending
        .as_array()
        .into_iter()
        .flatten()
        .find(|n| n["id"] == card["question_id"])
        .ok_or(CHANGED)?;
    let none = json!([]);
    let one = json!([found]);
    let again = if suffix == "/form" {
        forms::cards(record, &one, &none)
    } else {
        forms::cards(record, &none, &one)
    };
    if again.first().map(|c| &c["question_hash"]) != Some(&card["question_hash"]) {
        return Err(CHANGED.into());
    }
    let (route, body) = reply_for(card, found, raw)?;
    let mut receipt = json!({"request_id":request["request_id"],"name":record["name"],"run_id":record["run_id"],"conversation_id":record["conversation_id"],"question_id":request["question_id"],"question_hash":request["expected_question_hash"],"status":"pending"});
    atomic(path, &receipt.to_string())?;
    // A 4xx is a definite refusal; anything else leaves the delivery unknown
    // and the pending journal blocks a replay.
    let reply = native.raw("POST", &session_path(record, &route), Some(&body))?;
    if (400..500).contains(&reply.status) {
        receipt["status"] = json!("refused");
        atomic(path, &receipt.to_string())?;
        if reply.body["_tag"] == "FormInvalidAnswerError" {
            let reason = reply.body["message"].as_str().unwrap_or("invalid answer");
            return Err(format!("OpenCode rejected this answer: {}", clip_text(reason)));
        }
        return Err(CHANGED.into());
    }
    if !successful(&reply) {
        let reason = reply.body["message"]
            .as_str()
            .or(reply.body["data"]["message"].as_str())
            .unwrap_or("Native operation failed");
        return Err(format!("OpenCode HTTP {}: {}", reply.status, clip_text(reason)));
    }
    receipt["status"] = json!("answered");
    atomic(path, &receipt.to_string())?;
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
    interrupt_with(&running_service()?, &original)
}
/// Core of `interrupt`: success when OpenCode says it interrupted the turn or
/// the session is no longer running.
fn interrupt_with(native: &dyn Native, record: &Value) -> Result<()> {
    let reply = native.raw("POST", &session_path(record, "/interrupt"), Some(&json!({})))?;
    let said = reply.body["interrupted"] == true || reply.body["data"]["interrupted"] == true;
    if successful(&reply) && said {
        return Ok(());
    }
    // A failed lookup never counts as "not running".
    if let Ok(active) = active_sessions(native) {
        if active.get(string(record, "conversation_id")).is_none() {
            return Ok(());
        }
    }
    Err(format!(
        "OpenCode did not confirm the interruption ({})",
        refusal_reason(&reply)
    ))
}
const NOT_QUEUED: &str = "This message is no longer queued";
/// The exact native message ID of a `remove_queued` request.
fn queued_id(request: &Value) -> Result<String> {
    let id = string(request, "message_id");
    if !native_id(id, "msg_") {
        return Err("Choose the exact queued OpenCode message".into());
    }
    Ok(id.to_owned())
}
/// Core of `remove_queued`: a message can be removed only while it sits in
/// the inbox of a session that is not running; it is then not delivered.
/// `before_delete` runs once, immediately before the DELETE, so a clean
/// refusal is never recorded as a possible change.
fn remove_queued_with(
    native: &dyn Native,
    record: &Value,
    id: &str,
    before_delete: impl FnOnce(),
) -> Result<()> {
    let inbox = get(native, &session_path(record, "/inbox"))?;
    if !inbox["data"]
        .as_array()
        .is_some_and(|items| items.iter().any(|item| item["id"] == id))
    {
        return Err(NOT_QUEUED.into());
    }
    let active = active_sessions(native)?;
    if active.get(string(record, "conversation_id")).is_some() {
        return Err(NOT_QUEUED.into());
    }
    before_delete();
    let reply = native.raw("DELETE", &session_path(record, &format!("/inbox/{id}")), None)?;
    if reply.status == 404 {
        return Err(NOT_QUEUED.into());
    }
    if !successful(&reply) {
        return Err(format!("OpenCode could not remove this message ({})", refusal_reason(&reply)));
    }
    // A 2xx DELETE is accepted without re-reading the inbox; the next
    // inspection reflects it.
    Ok(())
}
/// Core of a model change: set the exact model, then read it back.
fn change_model(native: &dyn Native, record: &Value, model: &Value) -> Result<()> {
    let reply = native.raw(
        "POST",
        &session_path(record, "/model"),
        Some(&json!({"model": model})),
    )?;
    if !successful(&reply) {
        return Err(format!("OpenCode refused this model change ({})", refusal_reason(&reply)));
    }
    let seen = verify_session(native, record)?["model"].clone();
    // A model without an explicit variant reports "default".
    let variant_ok = match model["variant"].as_str() {
        Some(variant) => seen["variant"] == variant,
        None => true,
    };
    if seen["id"] != model["id"] || seen["providerID"] != model["providerID"] || !variant_ok {
        return Err(
            "Native OpenCode model change was not confirmed; inspect before retrying".into(),
        );
    }
    Ok(())
}
/// Core of rename: set only the title, then read it back.
fn change_title(native: &dyn Native, record: &Value, title: &str) -> Result<()> {
    let reply = native.raw(
        "PATCH",
        &session_path(record, ""),
        Some(&json!({"title": title})),
    )?;
    if !successful(&reply) {
        return Err(format!("OpenCode refused this rename ({})", refusal_reason(&reply)));
    }
    if verify_session(native, record)?["title"] != title {
        return Err(
            "Native OpenCode rename acknowledgement did not match; inspect before retrying".into(),
        );
    }
    Ok(())
}
const NO_PROCESS: &str = "OpenCode sessions have no separate process to pause";
fn action(
    record: &mut Value,
    action: &str,
    new_name: Option<&str>,
    message_id: Option<&str>,
    scope: Option<&session_action::Scope>,
) -> Result<()> {
    if ["pause", "resume"].contains(&action) {
        return Err(NO_PROCESS.into());
    }
    if !["rename", "forget", "remove_queued"].contains(&action) {
        return Err("This OpenCode lifecycle action is not supported".into());
    }
    if let Some(scope) = scope {
        scope.check(record)?;
    }
    let current = own_state(string(record, "name"))?;
    if !current["allowed_actions"]
        .as_array()
        .is_some_and(|values| values.iter().any(|v| v == action))
    {
        return Err("This action is unavailable for the exact OpenCode session".into());
    }
    let old = string(record, "name").to_owned();
    if action == "remove_queued" {
        let id = queued_id(&json!({"message_id": message_id}))?;
        remove_queued_with(&running_service()?, record, &id, || {
            if let Some(scope) = scope {
                scope.changing();
            }
        })?;
        if let Some(scope) = scope {
            scope.result_record(record);
        }
        return Ok(());
    }
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
        change_title(&running_service()?, record, b[2])?;
        record["name"] = json!(new);
    } else if let Some(scope) = scope {
        scope.changing();
    }
    if action == "forget" {
        archive::remove_synced(&binding_path(&old))?;
        return Ok(());
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
    message_id: Option<&str>,
    scope: &session_action::Scope,
) -> Result<()> {
    let _guard = lock(Some(&root().join("opencode/bindings.lock")))?;
    let mut record = binding(name)?;
    action(&mut record, kind, new_name, message_id, Some(scope))
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
            let service = running_service()?;
            let model = catalog::offered_model(
                &service,
                Path::new(string(&record, "cwd")),
                string(&request, "model"),
                string(&request, "effort"),
            )?;
            let path = root()
                .join("opencode/settings")
                .join(format!("{}.json", string(&request, "request_id")));
            if path.exists() {
                return Err(
                    "This model change was already attempted; inspect its native state".into(),
                );
            }
            atomic(&path, &json!({"status":"pending"}).to_string())?;
            change_model(&service, &record, &model)?;
            println!(
                "{}",
                json!({"status":"applied","scope":"session","request_id":request["request_id"],"name":name,"run_id":record["run_id"],"conversation_id":record["conversation_id"],"model":request["model"],"effort":request["effort"]})
            );
        }
        "remove_queued" => {
            let id = queued_id(&json!({"message_id": args.get(1)}))?;
            if own_state(name)?["allowed_actions"]
                .as_array()
                .is_none_or(|values| values.iter().all(|v| v != "remove_queued"))
            {
                return Err(NOT_QUEUED.into());
            }
            remove_queued_with(&running_service()?, &record, &id, || {})?;
            println!("{}", own_state(name)?);
        }
        "pause" | "resume" => return Err(NO_PROCESS.into()),
        "rename" | "forget" => {
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
    fn opencode2(name: &str) -> Value {
        let path = format!(
            "{}/tests/fixtures/opencode2/{name}.json",
            env!("CARGO_MANIFEST_DIR")
        );
        serde_json::from_slice(&fs::read(path).unwrap()).unwrap()
    }
    fn observed(messages: &str, active: bool, inbox: Value) -> Observed {
        let mut page = opencode2(messages)["data"].as_array().unwrap().clone();
        page.reverse();
        Observed {
            session: json!({"id":"ses_A","model":{"id":"local","providerID":"fixture","variant":"default"},"time":{"updated":5000}}),
            active,
            page: history::Page {
                messages: page,
                truncated: false,
            },
            inbox,
            forms: json!([]),
            permissions: json!([]),
        }
    }
    fn status_record(id: &str) -> Value {
        json!({"name":"opencode/project/one","run_id":"run","conversation_id":id,"cwd":"/x"})
    }
    fn no_inbox() -> Value {
        json!({"data": []})
    }
    #[test]
    fn an_idle_session_is_ready_for_messages_and_settings() {
        let value = normalize(
            &status_record("ses_A"),
            Some(&observed("messages", false, no_inbox())),
        );
        assert_eq!(value["phase"], "idle");
        assert_eq!(value["activity"], "idle");
        assert_eq!(value["activity_summary"], "Ready");
        assert_eq!(value["state"], "running");
        assert_eq!(value["send_available"], true);
        assert_eq!(value["settings_change_supported"], true);
        assert_eq!(value["interrupt_supported"], false);
        assert_eq!(value["allowed_actions"], json!(["rename", "forget"]));
        assert_eq!(value["settings_model"], "fixture/local");
        assert!(value["effort"].is_null());
        assert_eq!(value["events"].as_array().unwrap().len(), 3);
        assert_eq!(value["reply_id"], "msg_14bb2d63e001NhfRTKy4frROM8");
        assert_eq!(value["activity_detail"], "Native OpenCode reply");
        assert_eq!(value["prompt"], "hello");
        assert_eq!(value["turn_started"], 1792271373.881);
        assert_eq!(value["history_truncated"], false);
        for key in [
            "terminal_supported",
            "attachments_supported",
            "native_ui_available",
            "compact_context_supported",
        ] {
            assert_eq!(value[key], false, "{key}");
        }
        assert_eq!(
            value["action_reasons"]["pause"],
            "OpenCode sessions have no separate process to pause"
        );
        assert_eq!(
            value["action_reasons"]["fork"],
            "OpenCode fork arrives in a later update"
        );
        assert_eq!(
            value["action_reasons"]["terminate"],
            "Use Stop for this OpenCode session"
        );
        assert_eq!(
            value["compact_context_reason"],
            "OpenCode compaction arrives in a later update"
        );
        assert!(value.get("attention_id").is_none());
    }
    #[test]
    fn a_running_session_is_working_and_still_accepts_queued_messages() {
        let inbox = opencode2("inbox_busy");
        let value = normalize(
            &status_record("ses_A"),
            Some(&observed("messages", true, inbox)),
        );
        assert_eq!(value["phase"], "working");
        assert_eq!(value["activity"], "busy");
        assert_eq!(value["activity_summary"], "Working");
        assert_eq!(value["send_available"], true);
        assert_eq!(value["interrupt_supported"], true);
        assert_eq!(value["settings_change_supported"], false);
        let events = value["events"].as_array().unwrap();
        assert_eq!(events.len(), 4);
        assert_eq!(events[3]["delivery_state"], "queued");
        assert_eq!(events[3]["detail"], "queued later");
        assert_eq!(value["allowed_actions"], json!(["rename", "forget"]));
        // Queued text is not the prompt the agent is working on.
        assert_eq!(value["prompt"], "hello");
    }
    #[test]
    fn after_stop_queued_messages_are_not_delivered_and_removable() {
        let value = normalize(
            &status_record("ses_A"),
            Some(&observed("messages", false, opencode2("inbox_busy"))),
        );
        assert_eq!(value["phase"], "idle");
        assert_eq!(value["events"][3]["delivery_state"], "not_delivered");
        assert_eq!(
            value["allowed_actions"],
            json!(["rename", "forget", "remove_queued"])
        );
    }
    #[test]
    fn a_message_admitted_between_reads_is_shown_once() {
        let inbox = json!({"data":[{"id":"msg_6kezDqwZEklnh8eFGVy3UbP6iX","type":"user","payload":{"text":"hello"},"time":{"created":1}}]});
        let value = normalize(
            &status_record("ses_A"),
            Some(&observed("messages", false, inbox)),
        );
        assert_eq!(value["events"].as_array().unwrap().len(), 3);
        assert_eq!(value["allowed_actions"], json!(["rename", "forget"]));
    }
    #[test]
    fn replies_route_to_the_form_or_permission_with_validated_bodies() {
        let record = json!({"run_id":"run","conversation_id":"ses_A"});
        let form = json!({"id":"frm_1","sessionID":"ses_A","title":"Q","fields":[{"key":"q0","title":"C","type":"string"}]});
        let card = forms::cards(&record, &json!([form]), &json!([])).remove(0);
        let ask = [json!({"question_id":"q0","selected_option_ids":[],"text":"hi"})];
        assert_eq!(
            reply_for(&card, &form, &ask).unwrap(),
            (
                "/form/frm_1/reply".to_owned(),
                json!({"answer":{"q0":"hi"}})
            )
        );
        let permission =
            json!({"id":"per_1","sessionID":"ses_A","action":"shell","resources":["x"]});
        let card = forms::cards(&record, &json!([]), &json!([permission])).remove(0);
        let ok = [json!({"question_id":"permission","selected_option_ids":["reject"],"text":""})];
        assert_eq!(
            reply_for(&card, &permission, &ok).unwrap(),
            (
                "/permission/per_1/reply".to_owned(),
                json!({"decision":"reject"})
            )
        );
        assert!(reply_for(&card, &permission, &ask).is_err());
    }
    struct Answering {
        forms: std::cell::RefCell<Value>,
        reply: Result<Response>,
        posted: std::cell::RefCell<Vec<(String, Value)>>,
    }
    impl Native for Answering {
        fn raw(&self, method: &str, path: &str, body: Option<&Value>) -> Result<Response> {
            if method == "GET" {
                assert_eq!(path, "/api/session/ses_A/form");
                return ok(json!({"data": self.forms.borrow().clone()}));
            }
            self.posted.borrow_mut().push((path.to_owned(), body.unwrap().clone()));
            self.reply.clone()
        }
    }
    fn answering(reply: Result<Response>) -> (Answering, Value, Value) {
        let form = json!({"id":"frm_1","sessionID":"ses_A","title":"Q","fields":[{"key":"q0","title":"C","type":"string"}]});
        let card = forms::cards(&session_record(), &json!([form.clone()]), &json!([])).remove(0);
        let request = json!({"request_id":"r1","question_id":"frm_1","expected_question_hash":card["question_hash"],
            "answers":[{"question_id":"q0","selected_option_ids":[],"text":"hi"}]});
        let native = Answering { forms: json!([form]).into(), reply, posted: Default::default() };
        (native, card, request)
    }
    fn journal(path: &Path) -> Value {
        serde_json::from_slice(&fs::read(path).unwrap()).unwrap()
    }
    #[test]
    fn a_4xx_answer_is_refused_once_and_never_retried() {
        for tag in ["FormAlreadySettledError", "FormNotFoundError"] {
            let dir = tempfile::tempdir().unwrap();
            let path = dir.path().join("r1.json");
            let (native, card, request) = answering(status(409, json!({"_tag":tag,"message":"gone"})));
            let record = session_record();
            assert_eq!(answer_with(&native, &record, &request, &card, &path).unwrap_err(), CHANGED);
            assert_eq!(native.posted.borrow().len(), 1);
            assert_eq!(native.posted.borrow()[0].0, "/api/session/ses_A/form/frm_1/reply");
            assert_eq!(journal(&path)["status"], "refused");
            assert!(answer_with(&native, &record, &request, &card, &path).is_err());
            assert_eq!(native.posted.borrow().len(), 1);
        }
    }
    #[test]
    fn an_invalid_answer_shows_the_clipped_server_reason() {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("r1.json");
        let message = format!("bad\u{7}{}", "y".repeat(500));
        let (native, card, request) =
            answering(status(400, json!({"_tag":"FormInvalidAnswerError","message":message})));
        let error = answer_with(&native, &session_record(), &request, &card, &path).unwrap_err();
        assert_eq!(error, format!("OpenCode rejected this answer: bad{}", "y".repeat(197)));
        assert_eq!(journal(&path)["status"], "refused");
    }
    #[test]
    fn a_changed_native_request_is_not_answered() {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("r1.json");
        let (native, card, request) = answering(status(204, json!({})));
        native.forms.borrow_mut()[0]["title"] = json!("Changed");
        assert_eq!(answer_with(&native, &session_record(), &request, &card, &path).unwrap_err(), CHANGED);
        assert!(native.posted.borrow().is_empty());
        assert!(!path.exists());
        *native.forms.borrow_mut() = json!([]);
        assert_eq!(answer_with(&native, &session_record(), &request, &card, &path).unwrap_err(), CHANGED);
        assert!(native.posted.borrow().is_empty());
    }
    #[test]
    fn an_unknown_delivery_stays_pending_and_blocks_a_replay() {
        for reply in [status(503, json!({"message":"down\u{7}"})), Err("reset".to_owned())] {
            let dir = tempfile::tempdir().unwrap();
            let path = dir.path().join("r1.json");
            let (native, card, request) = answering(reply);
            let record = session_record();
            assert!(answer_with(&native, &record, &request, &card, &path).is_err());
            assert_eq!(journal(&path)["status"], "pending");
            assert!(answer_with(&native, &record, &request, &card, &path)
                .unwrap_err()
                .contains("already attempted"));
            assert_eq!(native.posted.borrow().len(), 1);
        }
    }
    #[test]
    fn an_accepted_answer_is_journaled_answered() {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("r1.json");
        let (native, card, request) = answering(status(204, json!({})));
        let receipt = answer_with(&native, &session_record(), &request, &card, &path).unwrap();
        assert_eq!(receipt["status"], "answered");
        assert_eq!(native.posted.borrow()[0].1, json!({"answer":{"q0":"hi"}}));
    }
    #[test]
    fn pending_forms_or_permissions_need_attention_before_work() {
        let forms = opencode2("forms_question");
        let id = forms["data"][0]["sessionID"].as_str().unwrap();
        let mut seen = observed("messages_question", true, no_inbox());
        seen.forms = forms["data"].clone();
        let value = normalize(&status_record(id), Some(&seen));
        assert_eq!(value["phase"], "input");
        assert_eq!(value["activity"], "attention");
        assert_eq!(value["activity_summary"], "Needs attention");
        assert_eq!(value["send_available"], false);
        assert_eq!(value["settings_change_supported"], false);
        assert_eq!(value["interrupt_supported"], true);
        assert_eq!(
            value["attention_id"],
            "opencode_question:frm_14bb2df2500101YAecuyKBa9qA"
        );
        let permissions = opencode2("permissions");
        let id = permissions["data"][0]["sessionID"].as_str().unwrap();
        let mut seen = observed("messages", false, no_inbox());
        seen.permissions = permissions["data"].clone();
        let value = normalize(&status_record(id), Some(&seen));
        assert_eq!(value["phase"], "input");
        assert_eq!(value["pending_questions"][0]["kind"], "opencode_permission");
        // Another session's request never needs this session's attention.
        let value = normalize(&status_record("ses_A"), Some(&seen));
        assert_eq!(value["phase"], "idle");
    }
    #[test]
    fn a_failed_turn_is_a_provider_error_until_work_resumes() {
        let record = status_record("ses_A");
        let value = normalize(
            &record,
            Some(&observed("messages_error", false, no_inbox())),
        );
        assert_eq!(value["phase"], "error");
        assert_eq!(value["activity"], "attention");
        assert_eq!(
            value["provider_error"]["message_id"],
            "opencode-error-msg_14bb546e7001QR5FwtWF0VburA"
        );
        assert_eq!(value["provider_error"]["at"], 1792271533.799);
        assert_eq!(value["last_error"], "Fixture provider rejected");
        let busy = normalize(&record, Some(&observed("messages_error", true, no_inbox())));
        assert_eq!(busy["phase"], "working");
        assert!(busy.get("provider_error").is_none());
    }
    #[test]
    fn an_unreadable_service_is_unknown_and_allows_only_forget() {
        let record = status_record("ses_A");
        let value = unavailable(&record, "OpenCode is not running on fixture".into());
        assert_eq!(value["state"], "unknown");
        assert_eq!(value["phase"], "unknown");
        assert_eq!(value["activity"], "unknown");
        assert_eq!(value["send_available"], false);
        assert_eq!(value["interrupt_supported"], false);
        assert_eq!(value["allowed_actions"], json!(["forget"]));
        assert_eq!(value["inspection_unavailable"], true);
        assert_eq!(
            value["inspection_reason"],
            "OpenCode is not running on fixture"
        );
        assert_eq!(
            value["activity_detail"],
            "OpenCode is not running on fixture"
        );
        let legacy = legacy_row(&record);
        assert_eq!(legacy["state"], "stopped");
        assert_eq!(legacy["allowed_actions"], json!(["forget"]));
        assert!(legacy.get("inspection_unavailable").is_none());
        assert_eq!(legacy["activity_detail"], legacy["inspection_reason"]);
        // Without a reading, clients keep the transcript they already show.
        for row in [&value, &legacy] {
            assert!(row.get("events").is_none());
            assert!(row.get("replace_events").is_none());
        }
    }
    #[test]
    fn a_streaming_reply_is_shown_but_not_yet_the_session_reply() {
        let mut seen = observed("messages", true, no_inbox());
        seen.page.messages.truncate(2);
        seen.page.messages[1]["time"]
            .as_object_mut()
            .unwrap()
            .remove("completed");
        let value = normalize(&status_record("ses_A"), Some(&seen));
        assert_eq!(value["events"][1]["detail"], "Native OpenCode reply");
        assert!(value["reply_id"].is_null());
        assert!(value["activity_detail"].is_null());
    }
    #[test]
    fn effort_is_the_session_variant_and_truncation_is_reported() {
        let mut seen = observed("messages", false, no_inbox());
        seen.session["model"]["variant"] = json!("high");
        seen.page.truncated = true;
        let value = normalize(&status_record("ses_A"), Some(&seen));
        assert_eq!(value["effort"], "high");
        assert_eq!(value["history_truncated"], true);
    }
    #[test]
    fn one_inspection_reads_each_route_once_and_the_activity_map_is_shared() {
        let dir = tempfile::tempdir().unwrap();
        let id = "ses_IycejmWY8RpJq1aeBnnIpbaNAt";
        let record = status_record(id);
        let session = json!({"data":{"id":id,"location":{"directory":"/x"},"model":{"id":"local","providerID":"fixture"}}});
        let messages = opencode2("messages");
        let next = messages["cursor"]["next"].as_str().unwrap().to_owned();
        let native = Scripted(std::cell::RefCell::new(vec![
            (format!("GET /api/session/{id}"), 200, session.clone()),
            (
                format!("GET /api/session/{id}/form"),
                200,
                json!({"data": []}),
            ),
            (
                format!("GET /api/session/{id}/permission"),
                200,
                json!({"data": []}),
            ),
            (
                format!("GET /api/session/{id}/inbox"),
                200,
                opencode2("inbox_busy"),
            ),
            (
                format!("GET /api/session/{id}/message?limit=100&order=desc"),
                200,
                messages,
            ),
            (
                format!("GET /api/session/{id}/message?limit=100&cursor={next}"),
                200,
                json!({"data": [], "cursor": {"next": null}}),
            ),
        ]));
        let active = opencode2("active_busy")["data"].clone();
        let cache = dir.path().join("history.json");
        let seen = observe(&native, &record, &active, || {
            history::fetch_at(&native, &record, &cache)
        })
        .unwrap();
        assert!(native.0.borrow().is_empty());
        assert!(seen.active);
        assert_eq!(seen.page.messages.len(), 3);
        assert_eq!(normalize(&record, Some(&seen))["phase"], "working");
        // The next poll reuses the cached history after one page.
        let native = Scripted(std::cell::RefCell::new(vec![
            (format!("GET /api/session/{id}"), 200, session),
            (
                format!("GET /api/session/{id}/form"),
                200,
                json!({"data": []}),
            ),
            (
                format!("GET /api/session/{id}/permission"),
                200,
                json!({"data": []}),
            ),
            (
                format!("GET /api/session/{id}/inbox"),
                200,
                json!({"data": []}),
            ),
            (
                format!("GET /api/session/{id}/message?limit=100&order=desc"),
                200,
                opencode2("messages"),
            ),
        ]));
        let seen = observe(&native, &record, &json!({}), || {
            history::fetch_at(&native, &record, &cache)
        })
        .unwrap();
        assert!(native.0.borrow().is_empty());
        assert!(!seen.active);
        assert_eq!(seen.page.messages.len(), 3);
    }
    #[test]
    fn a_moved_session_or_invalid_answers_fail_the_inspection() {
        let record = status_record("ses_A");
        let moved = Scripted(std::cell::RefCell::new(vec![(
            "GET /api/session/ses_A".into(),
            200,
            json!({"data":{"id":"ses_A","location":{"directory":"/elsewhere"}}}),
        )]));
        let none = || -> Result<history::Page> { panic!("no history read") };
        assert!(observe(&moved, &record, &json!({}), none).is_err());
        let invalid = Scripted(std::cell::RefCell::new(vec![
            (
                "GET /api/session/ses_A".into(),
                200,
                json!({"data":{"id":"ses_A","location":{"directory":"/x"}}}),
            ),
            (
                "GET /api/session/ses_A/form".into(),
                200,
                json!({"data": {}}),
            ),
        ]));
        assert!(observe(&invalid, &record, &json!({}), none).is_err());
        let refused = Scripted(std::cell::RefCell::new(vec![(
            "GET /api/session/active".into(),
            500,
            json!({"message": "busy"}),
        )]));
        assert_eq!(
            active_sessions(&refused).unwrap_err(),
            "OpenCode HTTP 500: busy"
        );
    }

    /// Scripted answers that also record every request body.
    struct Seen {
        script: std::cell::RefCell<Vec<(String, u16, Value)>>,
        bodies: std::cell::RefCell<Vec<(String, Option<Value>)>>,
    }
    impl Native for Seen {
        fn raw(&self, method: &str, path: &str, body: Option<&Value>) -> Result<Response> {
            let (expected, status, reply) = self.script.borrow_mut().remove(0);
            assert_eq!(expected, format!("{method} {path}"));
            self.bodies
                .borrow_mut()
                .push((expected, body.cloned()));
            Ok(Response { status, body: reply })
        }
    }
    fn seen(steps: Vec<(&str, u16, Value)>) -> Seen {
        Seen {
            script: std::cell::RefCell::new(
                steps.into_iter().map(|(p, s, b)| (p.to_owned(), s, b)).collect(),
            ),
            bodies: Default::default(),
        }
    }
    fn bound() -> Value {
        json!({"name":"opencode/p/t","conversation_id":"ses_A","cwd":"/x"})
    }
    fn session_with(model: Value, title: &str) -> Value {
        json!({"data":{"id":"ses_A","location":{"directory":"/x"},"model":model,"title":title}})
    }
    #[test]
    fn settings_validate_effort_against_the_model_variants() {
        let rows = catalog::project(&fixture(include_bytes!(
            "../../tests/fixtures/opencode2/models.json"
        )));
        assert_eq!(
            catalog::model_ref(&rows, "fixture/local", "high").unwrap(),
            json!({"providerID": "fixture", "id": "local", "variant": "high"})
        );
        assert_eq!(
            catalog::model_ref(&rows, "fixture/local", "").unwrap(),
            json!({"providerID": "fixture", "id": "local"})
        );
        assert!(catalog::model_ref(&rows, "fixture/local", "ultra").is_err());
        assert!(catalog::model_ref(&rows, "fixture/missing", "").is_err());
    }
    #[test]
    fn remove_queued_requires_an_exact_message_id() {
        assert!(queued_id(&json!({"message_id": "msg_gVmstesSuU81H2vTHdmxp65tNI"})).is_ok());
        assert!(queued_id(&json!({"message_id": "../x"})).is_err());
        assert!(queued_id(&json!({})).is_err());
    }
    #[test]
    fn interrupt_succeeds_when_acknowledged_or_already_stopped() {
        let ok = seen(vec![(
            "POST /api/session/ses_A/interrupt",
            200,
            json!({"interrupted": true}),
        )]);
        interrupt_with(&ok, &bound()).unwrap();
        let idle = seen(vec![
            ("POST /api/session/ses_A/interrupt", 200, json!({"interrupted": false})),
            ("GET /api/session/active", 200, json!({"data": {}})),
        ]);
        interrupt_with(&idle, &bound()).unwrap();
        let running = seen(vec![
            ("POST /api/session/ses_A/interrupt", 200, json!({"interrupted": false})),
            ("GET /api/session/active", 200, json!({"data": {"ses_A": {}}})),
        ]);
        assert!(interrupt_with(&running, &bound()).is_err());
        let unknown = seen(vec![
            ("POST /api/session/ses_A/interrupt", 500, json!({"message": "down"})),
            ("GET /api/session/active", 500, json!({"message": "down"})),
        ]);
        assert!(interrupt_with(&unknown, &bound()).is_err());
    }
    #[test]
    fn a_queued_message_is_removed_only_while_stopped_and_still_queued() {
        let inbox = json!({"data":[{"id":"msg_A","type":"user","payload":{"text":"x"}}]});
        let ok = seen(vec![
            ("GET /api/session/ses_A/inbox", 200, inbox.clone()),
            ("GET /api/session/active", 200, json!({"data": {}})),
            ("DELETE /api/session/ses_A/inbox/msg_A", 204, Value::Null),
        ]);
        let calls = std::cell::Cell::new(0);
        remove_queued_with(&ok, &bound(), "msg_A", || calls.set(calls.get() + 1)).unwrap();
        assert_eq!(calls.get(), 1);
        assert!(ok.script.borrow().is_empty());
        let gone = seen(vec![("GET /api/session/ses_A/inbox", 200, json!({"data":[]}))]);
        assert_eq!(
            remove_queued_with(&gone, &bound(), "msg_A", || panic!("no change")).unwrap_err(),
            NOT_QUEUED
        );
        let active = seen(vec![
            ("GET /api/session/ses_A/inbox", 200, inbox),
            ("GET /api/session/active", 200, json!({"data": {"ses_A": {}}})),
        ]);
        assert_eq!(
            remove_queued_with(&active, &bound(), "msg_A", || panic!("no change")).unwrap_err(),
            NOT_QUEUED
        );
    }
    #[test]
    fn a_model_variant_is_sent_and_verified() {
        let model = json!({"providerID":"fixture","id":"local","variant":"high"});
        let ok = seen(vec![
            ("POST /api/session/ses_A/model", 204, Value::Null),
            ("GET /api/session/ses_A", 200, session_with(model.clone(), "t")),
        ]);
        change_model(&ok, &bound(), &model).unwrap();
        assert_eq!(ok.bodies.borrow()[0].1, Some(json!({"model": model})));
        let wrong = seen(vec![
            ("POST /api/session/ses_A/model", 204, Value::Null),
            (
                "GET /api/session/ses_A",
                200,
                session_with(json!({"providerID":"fixture","id":"local","variant":"low"}), "t"),
            ),
        ]);
        assert_eq!(
            change_model(&wrong, &bound(), &model).unwrap_err(),
            "Native OpenCode model change was not confirmed; inspect before retrying"
        );
    }
    #[test]
    fn the_default_effort_sends_no_variant_and_accepts_default() {
        let model = json!({"providerID":"fixture","id":"local"});
        let ok = seen(vec![
            ("POST /api/session/ses_A/model", 204, Value::Null),
            (
                "GET /api/session/ses_A",
                200,
                session_with(json!({"providerID":"fixture","id":"local","variant":"default"}), "t"),
            ),
        ]);
        change_model(&ok, &bound(), &model).unwrap();
        assert_eq!(ok.bodies.borrow()[0].1, Some(json!({"model": model})));
        let other = seen(vec![
            ("POST /api/session/ses_A/model", 204, Value::Null),
            (
                "GET /api/session/ses_A",
                200,
                session_with(json!({"providerID":"fixture","id":"other"}), "t"),
            ),
        ]);
        assert!(change_model(&other, &bound(), &model).is_err());
    }
    #[test]
    fn rename_sends_only_the_title_and_verifies_it() {
        let ok = seen(vec![
            ("PATCH /api/session/ses_A", 204, Value::Null),
            ("GET /api/session/ses_A", 200, session_with(json!({}), "new")),
        ]);
        change_title(&ok, &bound(), "new").unwrap();
        assert_eq!(ok.bodies.borrow()[0].1, Some(json!({"title": "new"})));
        let stale = seen(vec![
            ("PATCH /api/session/ses_A", 204, Value::Null),
            ("GET /api/session/ses_A", 200, session_with(json!({}), "old")),
        ]);
        assert!(change_title(&stale, &bound(), "new").is_err());
    }
    #[test]
    fn pause_and_resume_explain_that_there_is_no_process() {
        let mut record = bound();
        for kind in ["pause", "resume"] {
            assert_eq!(
                action(&mut record, kind, None, None, None).unwrap_err(),
                NO_PROCESS
            );
        }
    }
}
