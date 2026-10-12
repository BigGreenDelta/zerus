//! Official DeepSeek Harness backend. Native host lifetime is independent of
//! Zerus, tmux, and individual sessions. HGS stores bindings, not credentials.
use super::*;
use sha2::{Digest, Sha256};
use std::fs::{self, OpenOptions};
use std::io::{BufRead, BufReader, Write};
use std::os::unix::{fs::OpenOptionsExt, net::UnixStream, process::CommandExt};
use std::process::{Command, Stdio};
use std::time::{Duration, Instant};

const VERSION: &str = "0.2.0-rc.2";
const BRIDGE: &str = include_str!("../dsh/bridge.mjs");
fn bindings() -> PathBuf {
    root().join("dsh/bindings")
}
fn binding_path(name: &str) -> PathBuf {
    bindings().join(format!("{:x}.json", Sha256::digest(name.as_bytes())))
}
pub(crate) fn exists(name: &str) -> bool {
    binding_path(name).is_file()
}
pub(super) fn binding(name: &str) -> Result<Value> {
    let value: Value =
        serde_json::from_slice(&fs::read(binding_path(name)).map_err(|e| e.to_string())?)
            .map_err(|e| e.to_string())?;
    if value["name"] != name
        || value["backend"] != "dsh"
        || !string(&value, "conversation_id").starts_with("session-")
    {
        return Err("Invalid DeepSeek binding".into());
    }
    Ok(value)
}
fn save(value: &Value) -> Result<()> {
    atomic(&binding_path(string(value, "name")), &value.to_string())
}
fn native_home() -> PathBuf {
    nonempty_env("DSH_HOME")
        .map(PathBuf::from)
        .unwrap_or_else(|| home().join(".dsh"))
}
pub(crate) fn account_context(name: &str) -> Result<Value> {
    let record = binding(name)?;
    Ok(
        json!({"provider":"dsh","id":"native-dsh","home":record["agent_home"],"run_id":record["run_id"]}),
    )
}
pub(crate) fn account_ui_url(home: &Path) -> Result<String> {
    // Reading Accounts must not start or restart a native agent host.
    let record = json!({"agent_home":home});
    let log = fs::read_to_string(host_dir(&record).join("host.log"))
        .map_err(|_| "Open the native DeepSeek UI to connect this account.".to_owned())?;
    regex::Regex::new(r"http://127\.0\.0\.1:[0-9]+/\?token=[A-Za-z0-9_%.-]+")
        .unwrap()
        .find_iter(&log)
        .last()
        .map(|m| m.as_str().to_owned())
        .ok_or_else(|| "Native DeepSeek UI is unavailable.".into())
}
fn host_dir(record: &Value) -> PathBuf {
    root().join("dsh/hosts").join(
        format!(
            "{:x}",
            Sha256::digest(string(record, "agent_home").as_bytes())
        )[..16]
            .to_owned(),
    )
}
fn rpc(record: &Value, method: &str, params: Value) -> Result<Value> {
    let mut socket = UnixStream::connect(host_dir(record).join("host.sock"))
        .map_err(|_| "DeepSeek host is unavailable; resume the session to reconnect".to_owned())?;
    socket
        .set_read_timeout(Some(Duration::from_secs(35)))
        .map_err(|e| e.to_string())?;
    socket
        .set_write_timeout(Some(Duration::from_secs(10)))
        .map_err(|e| e.to_string())?;
    writeln!(socket, "{}", json!({"method":method,"params":params})).map_err(|e| e.to_string())?;
    let mut response = String::new();
    BufReader::new(socket.take(32 * 1024 * 1024))
        .read_line(&mut response)
        .map_err(|e| format!("Native request delivery uncertain: {e}"))?;
    let response: Value = serde_json::from_str(&response)
        .map_err(|e| format!("Invalid native response; delivery uncertain: {e}"))?;
    if response["ok"] != true {
        return Err(string(&response, "error").into());
    }
    Ok(response["value"].clone())
}
fn ensure_host(record: &Value) -> Result<()> {
    let dir = host_dir(record);
    private_dir(&dir)?;
    let _guard = lock(Some(&dir.join("start.lock")))?;
    if let Ok(ping) = rpc(record, "ping", json!({})) {
        if ping["version"] == VERSION && ping["protocol"] == 1 {
            return Ok(());
        }
        return Err("The running DeepSeek host uses an incompatible adapter; close its sessions before upgrading".into());
    }
    match UnixStream::connect(dir.join("host.sock")) {
        Ok(_) => return Err("The running DeepSeek host is not responding; it has been left running. Retry when it recovers".into()),
        Err(e) if matches!(e.kind(), io::ErrorKind::NotFound | io::ErrorKind::ConnectionRefused) => {},
        Err(e) => return Err(format!("Cannot check the native host: {e}")),
    }
    let version = Command::new("dsh").arg("--version").output().map_err(|_| {
        format!("Install the official harness: npm install -g @deepseek-ai/dsh@{VERSION}")
    })?;
    if !version.status.success() || String::from_utf8_lossy(&version.stdout).trim() != VERSION {
        return Err(format!(
            "This adapter requires official @deepseek-ai/dsh@{VERSION}; installed version differs"
        ));
    }
    let script = dir.join("hgs-bridge.mjs");
    atomic(&script, BRIDGE)?;
    let socket = dir.join("host.sock");
    // The startup lock excludes competing HGS starters. Only unlink a stale
    // endpoint after a failed connection, never an active native host.
    if socket.exists() {
        fs::remove_file(&socket).map_err(|e| e.to_string())?;
    }
    let overlay = dir.join("hgs.patch.json");
    atomic(
        &overlay,
        &json!([{"insert":[{"id":"hgs-native-dsh","name":script,"config":{"socket":socket,"recoveryPolicy":recovery::policy_path()}}]}])
            .to_string(),
    )?;
    let log = OpenOptions::new()
        .create(true)
        .truncate(true)
        .write(true)
        .mode(0o600)
        .open(dir.join("host.log"))
        .map_err(|e| e.to_string())?;
    let mut command = Command::new("dsh");
    #[cfg(target_os = "linux")]
    {
        let runtime = PathBuf::from(format!("/run/user/{}", unsafe { libc::getuid() }));
        if runtime.join("systemd/private").exists() && crate::platform::available("systemd-run") {
            // setsid detaches the terminal, but systemd still kills every
            // descendant in the GUI's cgroup on restart. A sibling scope
            // preserves the caller's environment and private log descriptors,
            // while giving the native host an independent lifetime.
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
                .arg(format!("hgs-dsh-{}", uuid::Uuid::new_v4()))
                .args(["--", "dsh"])
                .env("XDG_RUNTIME_DIR", runtime);
        }
    }
    command.args(["--profile", "hgs"]);
    // This flag creates a profile and intentionally rejects existing ones.
    // Reopening the host must retain the native profile and its settings.
    if !Path::new(string(record, "agent_home"))
        .join("profiles/hgs/package.json")
        .exists()
    {
        command.args(["--from-default-profile", "web"]);
    }
    command
        .arg("--patch")
        .arg(overlay)
        .args(["--no-open", "--host", "127.0.0.1", "--port", "0"])
        .env("DSH_HOME", string(record, "agent_home"))
        .current_dir(home())
        .stdin(Stdio::null())
        .stdout(log.try_clone().map_err(|e| e.to_string())?)
        .stderr(log);
    // Detach only the dedicated native host. Closing its launching terminal or
    // HGS process must not stop conversations.
    unsafe {
        command.pre_exec(|| {
            if libc::setsid() == -1 {
                return Err(io::Error::last_os_error());
            }
            Ok(())
        });
    }
    let mut child = command.spawn().map_err(|e| e.to_string())?;
    atomic(&dir.join("host.pid"), &child.id().to_string())?;
    let deadline = Instant::now() + Duration::from_secs(35);
    while Instant::now() < deadline {
        if let Ok(value) = rpc(record, "ping", json!({})) {
            if value["protocol"] == 1 {
                return Ok(());
            }
        }
        if child.try_wait().map_err(|e| e.to_string())?.is_some() {
            return Err(format!(
                "Official DeepSeek host failed to start. Private log: {}",
                dir.join("host.log").display()
            ));
        }
        std::thread::sleep(Duration::from_millis(100));
    }
    Err("DeepSeek host is still starting; retry opening the session".into())
}

pub(crate) fn native_ui_url(name: &str) -> Result<String> {
    let record = if name == "@account" {
        json!({"agent_home":native_home()})
    } else {
        binding(name)?
    };
    ensure_host(&record)?;
    let log = fs::read_to_string(host_dir(&record).join("host.log")).map_err(|e| e.to_string())?;
    let pattern =
        regex::Regex::new(r"http://127\.0\.0\.1:[0-9]+/\?token=[A-Za-z0-9_%.-]+").unwrap();
    let url = pattern
        .find(&log)
        .map(|m| m.as_str().to_owned())
        .ok_or_else(|| "Native UI is still starting; try again in a moment".to_owned())?;
    if name != "@account" {
        match rpc(
            &record,
            "workspace",
            json!({"sessionId":record["conversation_id"]}),
        ) {
            Ok(_) => {}
            Err(error) if error == "Unsupported HGS native request" => {
                // Upgrade the UI of an active legacy host without restarting
                // its agents. Tokens never appear in process arguments.
                let mut child = Command::new("node")
                    .args([
                        "--input-type=module",
                        "-e",
                        include_str!("../dsh/ui-compat.mjs"),
                    ])
                    .stdin(Stdio::piped())
                    .stdout(Stdio::null())
                    .stderr(Stdio::piped())
                    .spawn()
                    .map_err(|e| e.to_string())?;
                child.stdin.take().ok_or("Could not prepare native UI")?
                    .write_all(json!({"url":url,"sessionId":record["conversation_id"],"cwd":record["cwd"]}).to_string().as_bytes())
                    .map_err(|e| e.to_string())?;
                if !child
                    .wait_with_output()
                    .map_err(|e| e.to_string())?
                    .status
                    .success()
                {
                    return Err(
                        "Could not register the session in the native DeepSeek workspace".into(),
                    );
                }
            }
            Err(error) => return Err(error),
        }
    }
    Ok(url)
}

struct PermissionPlan {
    mode: String,
    legacy_url: Option<String>,
    needs_setup: bool,
    generation: Value,
}

fn legacy_permissions(record: &Value, url: &str, operation: &str) -> Result<Value> {
    let mut child = Command::new("node")
        .args([
            "--input-type=module", "-e",
            concat!(include_str!("../dsh/account-client.mjs"), "\n", include_str!("../dsh/permissions-compat.mjs")),
        ])
        .stdin(Stdio::piped()).stdout(Stdio::piped()).stderr(Stdio::null())
        .spawn().map_err(|e| e.to_string())?;
    child.stdin.take().ok_or("Could not prepare native permissions")?
        .write_all(json!({"url":url,"sessionId":record["conversation_id"],"operation":operation,
            "pending":record["permissions_pending"] == true}).to_string().as_bytes())
        .map_err(|e| e.to_string())?;
    let output = child.wait_with_output().map_err(|e| e.to_string())?;
    let result: Value = serde_json::from_slice(&output.stdout).unwrap_or(Value::Null);
    if !output.status.success() || result["ok"] != true {
        return Err(if operation == "check" {
            "Could not verify bypass permissions on the running DeepSeek host. Check its Native UI; no new session was created."
        } else {
            "DeepSeek did not confirm bypass permissions. Resume this session to finish setup; no message was sent."
        }.into());
    }
    Ok(result)
}

// Extend legacy residents through their authenticated native API, without
// reloading their bridge (which would dispose its owned live agents).
fn permission_plan(record: &Value) -> Result<PermissionPlan> {
    let config = crate::config::Config::load().map_err(|e|e.message)?;
    let mode = crate::accounts::permission_mode(&config,"native-dsh","dsh").map_err(|e|e.message)?;
    let ping = rpc(record, "ping", json!({}))?;
    let mut plan = PermissionPlan { mode, legacy_url: None, needs_setup: false, generation: ping["generation"].clone() };
    if plan.mode == "bypass" && (ping["accountPermissions"] != true || record["permissions_pending"] == true) {
        let url = account_ui_url(Path::new(string(record, "agent_home")))?;
        let checked = legacy_permissions(record, &url, "check")?;
        plan.needs_setup = checked["needs_setup"].as_bool().ok_or("Invalid native permissions response")?;
        plan.legacy_url = Some(url);
    }
    Ok(plan)
}

fn create_session(record: &mut Value) -> Result<()> {
    // Deterministic capability/account checks precede persistence; identities
    // still precede native creation so an uncertain reply cannot duplicate it.
    let plan = permission_plan(record)?;
    if rpc(record, "ping", json!({}))?["generation"] != plan.generation {
        return Err("DeepSeek host changed before session creation; retry opening the session".into());
    }
    if plan.needs_setup { record["permissions_pending"] = json!(true); }
    save(record)?;
    rpc(record, "create", json!({"sessionId":record["conversation_id"],"cwd":record["cwd"],
        "title":string(record,"name").rsplit('/').next(),"permissionMode":plan.mode}))?;
    if let Some(url) = plan.legacy_url.filter(|_| plan.needs_setup) {
        if rpc(record, "ping", json!({}))?["generation"] != plan.generation {
            return Err("DeepSeek host changed before permission setup; resume the session".into());
        }
        legacy_permissions(record, &url, "apply")?;
        if rpc(record, "ping", json!({}))?["generation"] != plan.generation {
            return Err("DeepSeek host changed during permission setup; resume the session".into());
        }
    }
    record.as_object_mut().ok_or("Invalid DeepSeek binding")?.remove("permissions_pending");
    save(record)
}

pub(crate) fn launch(
    name: &str,
    cwd: &str,
    launch_id: Option<&str>,
    dry: bool,
    new_only: bool,
) -> Result<()> {
    if dry {
        println!("Create official dsh session {name} in {cwd}");
        return Ok(());
    }
    let _guard = lock(Some(&root().join("dsh/bindings.lock")))?;
    if exists(name) {
        if new_only {
            return Err("This DeepSeek session name already exists".into());
        }
        let mut record = binding(name)?;
        ensure_host(&record)?;
        create_session(&mut record)?;
        record["paused"] = json!(false);
        record["run_id"] = json!(uuid::Uuid::new_v4().to_string());
        save(&record)?;
        return Ok(());
    }
    let cwd = PathBuf::from(cwd)
        .canonicalize()
        .map_err(|e| e.to_string())?;
    let mut record = json!({"backend":"dsh","name":name,"agent":"dsh","agent_home":native_home(),"cwd":cwd,
        "conversation_id":format!("session-{}",uuid::Uuid::new_v4()),"run_id":uuid::Uuid::new_v4().to_string(),
        "created":now(),"launch_id":launch_id,"paused":false});
    ensure_host(&record)?;
    create_session(&mut record)?;
    Ok(())
}

pub(crate) fn snapshots() -> Result<Vec<Value>> {
    let mut out = Vec::new();
    let Ok(files) = fs::read_dir(bindings()) else {
        return Ok(out);
    };
    let mut hosts = BTreeMap::new();
    for file in files.flatten().take(5000) {
        if file.path().extension().is_none_or(|s| s != "json") {
            continue;
        }
        let Ok(bytes) = fs::read(file.path()) else {
            continue;
        };
        let Ok(record) = serde_json::from_slice::<Value>(&bytes) else {
            continue;
        };
        if record["backend"] != "dsh" {
            continue;
        }
        let key = string(&record, "agent_home").to_owned();
        let list = hosts
            .entry(key)
            .or_insert_with(|| rpc(&record, "list", json!({})).unwrap_or(Value::Null));
        let row = list["items"].as_array().and_then(|items| {
            items
                .iter()
                .find(|v| v["sessionId"] == record["conversation_id"])
        });
        let row = row.unwrap_or(&Value::Null);
        let failure = idle_failure(&record, row, &list["generation"]);
        let mut value = normalize(&record, row, &json!({"provider_error":failure}));
        if !list.is_null() {
            value["host_generation"] = list["generation"].clone();
        }
        out.push(value);
    }
    Ok(out)
}

// A native turn can end unsuccessfully while the agent stays alive. Cache only
// that terminal result; polling must not reload conversation pages every time.
fn idle_failure(record: &Value, row: &Value, generation: &Value) -> Value {
    if row["agentAvailable"] != true || row["running"] == true {
        return Value::Null;
    }
    let key = format!(
        "{}:{}",
        string(record, "agent_home"),
        string(record, "conversation_id")
    );
    let path = root()
        .join("dsh/errors")
        .join(format!("{:x}.json", Sha256::digest(key.as_bytes())));
    let stamp = json!([record["run_id"], row["updatedAt"], generation]);
    let cached = fs::read(&path)
        .ok()
        .and_then(|b| serde_json::from_slice::<Value>(&b).ok())
        .unwrap_or(Value::Null);
    if !row["updatedAt"].is_null() && cached["stamp"] == stamp {
        return cached["error"].clone();
    }
    let Ok(inspected) = rpc(
        record,
        "inspect",
        json!({"sessionId":record["conversation_id"]}),
    ) else {
        return Value::Null;
    };
    let error = latest_failure(&inspected["page"]["records"]);
    if private_dir(path.parent().unwrap()).is_ok() {
        let _ = atomic(&path, &json!({"stamp":stamp,"error":error}).to_string());
    }
    error
}
fn turn_failure(event: &Value) -> Value {
    let reason = &event["data"]["reason"];
    let detail = match string(reason, "kind") {
        "error" => {
            let message = string(&reason["error"], "message");
            if message.is_empty() {
                "Provider request failed".to_owned()
            } else {
                message.to_owned()
            }
        }
        "max-tokens" => "Output token limit reached".to_owned(),
        _ => return Value::Null,
    };
    let mut failure = provider_errors::event(
        &format!("dsh-error-{}", event["seq"]),
        event["time"].as_f64().unwrap_or(0.0) / 1000.0,
        &detail,
        "dsh_native",
    );
    let category = provider_errors::category(&format!(
        "{} {}",
        string(reason, "kind"),
        string(&reason["error"], "code")
    ));
    if category != "provider" {
        failure["error_kind"] = json!(category);
    }
    failure
}
fn latest_failure(records: &Value) -> Value {
    for record in records.as_array().into_iter().flatten().rev() {
        let event = &record["event"];
        match string(event, "type") {
            "turn/end" => return turn_failure(event),
            "turn/start" | "user/message" => return Value::Null,
            _ => {}
        }
    }
    Value::Null
}

fn goal(value: &Value) -> Value {
    if !value.is_object() || string(value, "id").is_empty() {
        return Value::Null;
    }
    json!({"id":value["id"],"objective":value["objective"],"status":value["phase"],"revision":value["revision"],
        "rounds_used":value["roundsStarted"],"round_budget":value["maxGoalRounds"],"activation":value["activation"],
        "reason":value["blockedReason"]["message"],"source":"dsh_goal_v1"})
}
fn normalize(record: &Value, row: &Value, inspected: &Value) -> Value {
    let parts = string(record, "name").splitn(3, '/').collect::<Vec<_>>();
    let projections = if inspected["baseline"].is_object() {
        &inspected["baseline"]["values"]
    } else {
        &row["projections"]["values"]
    };
    let alive = inspected["live"]
        .as_bool()
        .unwrap_or(row["agentAvailable"] == true);
    let working = row["running"] == true;
    let selection = if inspected["selection"].is_object() {
        &inspected["selection"]
    } else if row["selection"].is_object() {
        &row["selection"]
    } else if projections["modelSelection"]["next"].is_object() {
        &projections["modelSelection"]["next"]
    } else {
        &projections["modelSelection"]["lastUsed"]
    };
    let last_used = &projections["modelSelection"]["lastUsed"];
    let effort = selection
        .get("reasoningEffort")
        .filter(|v| v.is_string())
        .or_else(|| {
            (last_used["provider"] == selection["provider"]
                && last_used["model"] == selection["model"])
                .then(|| last_used.get("reasoningEffort"))
                .flatten()
        })
        .cloned()
        .unwrap_or(Value::Null);
    let pending = inspected["pendingQuestions"]
        .as_array()
        .or(row["pendingQuestions"].as_array())
        .is_some_and(|v| !v.is_empty());
    let auth_required = alive
        && inspected
            .get("authRequired")
            .unwrap_or(&row["authRequired"])
            == true;
    let phase = if pending || auth_required {
        "input"
    } else if working {
        "working"
    } else {
        "idle"
    };
    let native_goal = if inspected.get("goal").is_some() {
        &inspected["goal"]
    } else if row.get("goal").is_some() {
        &row["goal"]
    } else {
        &projections["goal"]
    };
    let outline = projections["turnOutline"].as_array().and_then(|v| v.last());
    let reply = projections["turnOutline"].as_array().and_then(|rows| {
        rows.iter()
            .rev()
            .find(|v| !string(v, "response").is_empty())
    });
    let mut children = serde_json::Map::new();
    for child in projections["subagentCatalog"]
        .as_array()
        .into_iter()
        .flatten()
    {
        children.insert(string(child,"id").to_owned(),json!({"name":child.get("label").unwrap_or(&child["id"]),"state":"unknown","mode":child["mode"]}));
    }
    let mut value = json!({"name":record["name"],"cmd":"dsh","agent":"dsh","backend":"dsh","project":parts.get(1),"tag":parts.get(2),
        "conversation_id":record["conversation_id"],"run_id":record["run_id"],"launch_id":record["launch_id"],"created":record["created"],
        "tracked":true,"resumable":true,"cwd":record["cwd"],"cwd_source":"native_session","attached":0,"clients":[],
        "state":if alive{"running"}else if record["paused"]==true{"paused"}else{"stopped"},
        "process_state":if alive{"running"}else{"exited"},"conversation_state":"active","runtime_state":if alive{"live"}else{"stopped"},
        "phase":phase,"activity":if pending || auth_required{"attention"}else if working{"busy"}else if alive{"idle"}else{"unknown"},
        "activity_summary":if auth_required{"Sign in required"}else if pending{"Needs attention"}else if working{"Working"}else if alive{"Ready"}else{"Stopped"},
        "model":selection["model"],"effort":effort,"model_provider":selection["provider"],
        "goal":goal(native_goal),"goal_source":"dsh_goal_v1","last_event_at":row["updatedAt"].as_f64().unwrap_or(0.0)/1000.0,
        "subagent_source":"dsh_native","subagent_counts_complete":false,"subagent_total_count":children.len(),"subagents":children,
        "prompt":outline.map(|v|v["prompt"].clone()),"activity_detail":outline.map(|v|v["response"].clone()),
        "reply_id":reply.map(|v|format!("dsh-turn-{}",v["seq"].as_u64().unwrap_or(0))),"reply_at":if reply.is_some(){row["updatedAt"].as_f64().unwrap_or(0.0)/1000.0}else{0.0},
        "native_ui_available":true,"settings_change_supported":alive,"send_available":alive && !auth_required && record["permissions_pending"] != true});
    if pending {
        // Native question IDs distinguish one request from the next for read
        // marks and notifications; their generation-bound hashes do not.
        let ids: Vec<&str> = inspected["pendingQuestions"]
            .as_array()
            .or(row["pendingQuestions"].as_array())
            .into_iter()
            .flatten()
            .filter_map(|q| q["id"].as_str())
            .collect();
        value["attention_id"] = json!(format!("dsh_question:{}", ids.join("\n")));
    }
    if record["permissions_pending"] == true {
        value["phase"] = json!("error");
        value["activity"] = json!("attention");
        value["activity_summary"] = json!("Resume to finish permission setup");
        value["settings_change_supported"] = json!(false);
    }
    value["account_id"] = json!("native-dsh");
    value["account_home"] = record["agent_home"].clone();
    value["auth_required"] = json!(auth_required);
    value["account_status"] = inspected
        .get("accountStatus")
        .unwrap_or(&row["accountStatus"])
        .clone();
    if alive && !working && !pending && !auth_required && record["permissions_pending"] != true {
        let failure = inspected
            .get("provider_error")
            .cloned()
            .unwrap_or_else(|| latest_failure(&inspected["page"]["records"]));
        if failure.is_object() {
            provider_errors::apply(&mut value, &failure);
        }
    }
    if let Some(todos) = projections["todos"].as_array() {
        value["task_lists"] = json!({"main":{"source":"dsh_native","run_id":record["run_id"],"items":todos.iter().enumerate().map(|(index,t)|json!({"id":index.to_string(),"title":t["content"],"status":t["status"]})).collect::<Vec<_>>()}});
    }
    let pending_recovery = inspected.get("nativeRecovery").unwrap_or(&row["nativeRecovery"]);
    if alive && !pending && !auth_required && pending_recovery.is_object() && record["permissions_pending"] != true {
        let mut error = provider_errors::event(string(pending_recovery,"id"),
            pending_recovery["at"].as_f64().unwrap_or(0.0),string(pending_recovery,"detail"),"dsh_request_error");
        error["retry_not_before"] = pending_recovery["retryNotBefore"].clone();
        provider_errors::apply(&mut value,&error);
        value["native_recovery"] = pending_recovery.clone();
    }
    recovery::enrich(record,&mut value);
    value
}

fn native_events(records: &Value) -> Vec<Value> {
    let mut events: Vec<Value> = Vec::new();
    let mut turn_key = String::new();
    let mut last_answer: Option<usize> = None;
    for record in records.as_array().into_iter().flatten() {
        let e = &record["event"];
        let data = &e["data"];
        let kind = string(e, "type");
        if kind == "turn/start" {
            turn_key = format!("dsh-turn-{}", e["seq"].as_u64().unwrap_or(0));
            last_answer = None;
            continue;
        }
        if kind == "turn/end" && !turn_key.is_empty() {
            if let Some(index) = last_answer {
                events[index]["reply_id"] = json!(turn_key);
            }
        }
        let (typ, detail, tool) = match kind {
            "user/message"
                if ["user", "user-question-reply"].contains(&string(&data["source"], "kind")) =>
            {
                ("UserPromptSubmit", message_text(data), String::new())
            }
            "assistant/message" => (
                "AgentMessage",
                message_text(&data["message"]),
                String::new(),
            ),
            "tool/call" => (
                "PreToolUse",
                journal::clipped(&data["arguments"], 1800),
                string(data, "name").to_owned(),
            ),
            "tool/result" => (
                "PostToolUse",
                message_text(&data["message"]),
                string(&data["message"], "name").to_owned(),
            ),
            "turn/end" => {
                let failure = turn_failure(e);
                if failure.is_object() {
                    (
                        "StopFailure",
                        string(&failure, "detail").to_owned(),
                        String::new(),
                    )
                } else {
                    ("Stop", String::new(), String::new())
                }
            }
            _ => continue,
        };
        if detail.is_empty() && typ != "Stop" && typ != "PreToolUse" {
            continue;
        }
        if typ == "AgentMessage" {
            last_answer = Some(events.len());
        }
        events.push(json!({"type":typ,"agent_id":"","seq":e["seq"].as_i64().unwrap_or(0)+1,"at":e["time"].as_f64().unwrap_or(0.0)/1000.0,
            "detail":detail,"tool":tool,"source":"dsh_native","message_id":data["message"]["id"]}));
    }
    events
}
fn message_text(value: &Value) -> String {
    let content = value.get("content").unwrap_or(value);
    if let Some(s) = content.as_str() {
        return journal::clipped(&json!(s), 32000);
    }
    journal::clipped(
        &json!(content
            .as_array()
            .into_iter()
            .flatten()
            .filter(|p| p["type"] == "text")
            .map(|p| string(p, "text"))
            .collect::<Vec<_>>()
            .join("\n")),
        32000,
    )
}

pub(super) fn process_rpc(record: &Value, method: &str, params: Value) -> Result<Value> { rpc(record,method,params) }

pub(super) fn processes(name: &str, output: Option<&str>, stop: Option<&str>, run: Option<&str>, conversation: Option<&str>, generation: Option<&str>) -> Result<i32> {
    let record=binding(name)?;
    if (output.is_some() || stop.is_some()) && (run!=Some(string(&record,"run_id")) || conversation!=Some(string(&record,"conversation_id"))) {
        return Err("The process belongs to a different session run or conversation".into());
    }
    let inspected=rpc(&record,"inspect",json!({"sessionId":record["conversation_id"]}))?;
    println!("{}",processes::dsh_dispatch(&record,&inspected,output,stop,generation)?);Ok(0)
}

pub(crate) fn inspect(name: &str, agent_id: Option<&str>) -> Result<Value> {
    // Internal recovery/input checks never need a process inventory.
    inspect_with_processes(name, agent_id, false)
}
fn inspect_with_processes(name: &str, agent_id: Option<&str>, include_processes: bool) -> Result<Value> {
    let record = binding(name)?;
    let inspected = rpc(
        &record,
        "inspect",
        json!({"sessionId":record["conversation_id"],"agentId":agent_id,"includeProcesses":include_processes}),
    )?;
    let listed = rpc(&record, "list", json!({}))?;
    let row = listed["items"]
        .as_array()
        .and_then(|rows| {
            rows.iter()
                .find(|v| v["sessionId"] == record["conversation_id"])
        })
        .unwrap_or(&Value::Null);
    let mut result = normalize(&record, row, &inspected);
    let mut events = native_events(&inspected["page"]["records"]);
    let process_snapshot=if include_processes { processes::dsh_snapshot(&record,&inspected) } else { Value::Null };
    for event in &mut events {
        let source=inspected["page"]["records"].as_array().into_iter().flatten().map(|v|&v["event"]).find(|e|e["seq"]==event["seq"]);
        if let Some(source)=source {
            let call=source["data"].get("callId").or_else(||source["data"]["message"].get("toolCallId"));
            if let Some(job)=process_snapshot["items"].as_array().into_iter().flatten().find(|j|j["owner"]=="main" && Some(&j["call_id"])==call) {event["process_id"]=job["id"].clone();}
        }
    }
    result["turn_started"] = json!(inspected["page"]["records"].as_array().into_iter().flatten().rev()
        .find(|v|v["event"]["type"]=="turn/start").and_then(|v|v["event"]["time"].as_f64()).unwrap_or(0.0)/1000.0);
    for event in &events {
        if event["type"] == "UserPromptSubmit" {
            result["recovery_user_at"] = event["at"].clone();
            result["recovery_user_text"] = event["detail"].clone();
        }
    }
    for event in inspected["page"]["records"].as_array().into_iter().flatten().map(|v|&v["event"]) {
        if event["type"] == "turn/end" && event["data"]["reason"]["kind"] == "completed" {
            result["recovery_success_at"] = json!(event["time"].as_f64().unwrap_or(0.0) / 1000.0);
        }
        if event["type"] == "turn/end" && ["aborted","interrupted","blocked"].contains(&string(&event["data"]["reason"],"kind")) {
            result["recovery_cancel_at"] = json!(event["time"].as_f64().unwrap_or(0.0) / 1000.0);
        }
    }
    result["cursor"] = json!(0);
    result["events"] = json!(events);
    result["attachment_messages"] = attachments::messages(&record, agent_id.unwrap_or(""));
    result["replace_events"] = json!(true);
    result["history_truncated"] = inspected["page"]["hasMore"].clone();
    result["host_generation"] = inspected["generation"].clone();
    let ping=rpc(&record,"ping",json!({})).unwrap_or(Value::Null);
    result["terminal_supported"]=json!(false);
    result["terminal_reason"]=json!("DeepSeek uses a structured native API without a tmux Terminal");
    result["allowed_actions"]=if agent_id.is_none() && ping["sessionActions"]==true && ping["generation"]==inspected["generation"] {
        inspected["lifecycleActions"].clone()
    } else {json!([])};
    result["action_reasons"]=json!({"archive":"Native DeepSeek has no archive lifecycle API","fork":"Native DeepSeek has no fork lifecycle API",
        "terminate":"Use Pause for this native DeepSeek session","restore":"Native DeepSeek has no archive lifecycle API",
        "pause":"Requires a supported resident adapter and its owned handle","resume":"Requires a supported resident adapter and a stopped session",
        "rename":"Requires the supported resident scoped adapter","forget":"Requires a supported resident adapter and its owned handle"});
    result["pending_questions"] = json!(question_cards(&record, &inspected));
    result["native_questions"] = inspected["pendingQuestions"].clone();
    result["native_projections"] = inspected["baseline"]["values"].clone();
    result["session_usage"] = usage::dsh(&inspected["baseline"]["values"]);
    if include_processes { result["processes"] = process_snapshot; }
    result["compact_context_supported"] = json!(agent_id.is_none() && inspected["compactSupported"] == true && result["phase"] == "idle");
    result["compact_context_request"] = inspected["compactRequest"].clone();
    if result["compact_context_request"].is_object() {
        result["compact_context_request"]["run_id"] = record["run_id"].clone();
        result["compact_context_request"]["conversation_id"] = record["conversation_id"].clone();
        if result["compact_context_request"]["status"] == "compacting" {
            result["phase"] = json!("compacting"); result["activity"] = json!("busy");
            result["compaction_started"] = result["compact_context_request"]["at"].clone();
            result["compact_context_supported"] = json!(false);
        }
    }
    result["interrupt_supported"] = json!(inspected["interruptSupported"] == true &&
        ["working","tool","compacting"].contains(&string(&result,"phase")) && result["process_state"] == "running" && result["turn_started"].as_f64().unwrap_or(0.0)>0.0);
    if let Some(id) = agent_id {
        result["agent_id"] = json!(id);
        result["parent_conversation_id"] = record["conversation_id"].clone();
        result["conversation_id"] = json!(format!("{}/{id}", string(&record, "conversation_id")));
        result["send_supported"] = inspected["childSendSupported"].clone();
        result["read_only"] = json!(inspected["childSendSupported"] != true);
        result["child_mode"] = inspected["childMode"].clone();
        result["history_scope"] = json!("Native DeepSeek subagent history");
    } else if let Ok(catalog) = rpc(&record, "catalog", json!({})) {
        if string(&result, "effort").is_empty() {
            if let Some(model) = catalog["groups"]
                .as_array()
                .into_iter()
                .flatten()
                .find(|g| g["id"] == result["model_provider"])
                .and_then(|g| g["models"].as_array())
                .and_then(|models| models.iter().find(|m| m["id"] == result["model"]))
            {
                result["effort"] = model["reasoning"]["defaultEffort"].clone();
            }
        }
        result["settings_model"] = json!(format!(
            "{}/{}",
            string(&result, "model_provider"),
            string(&result, "model")
        ));
        result["model_options"]=json!(catalog["groups"].as_array().into_iter().flatten().flat_map(|g|g["models"].as_array().into_iter().flatten().map(|m|json!({"id":format!("{}/{}",string(g,"id"),string(m,"id")),"label":format!("{} ({})",string(m,"name"),string(g,"name")),"effort_options":m["reasoning"]["efforts"].as_array().into_iter().flatten().map(|e|e["id"].clone()).collect::<Vec<_>>()}))).collect::<Vec<_>>());
    }
    Ok(result)
}

fn question_cards(record: &Value, inspected: &Value) -> Vec<Value> {
    inspected["pendingQuestions"].as_array().into_iter().flatten().map(|q| {
        let items=q["questions"].as_array().into_iter().flatten().map(|item|json!({
            "id":item["id"],"question":item["question"],"header":item["header"],"body":item["detail"],"multi_select":item["multiSelect"],
            "allow_other":q["approval"]!=true,"required":true,"options":item["options"].as_array().into_iter().flatten().enumerate().map(|(i,o)|json!({"id":if q["approval"]==true {if i==0 {"allow".to_owned()}else{"deny".to_owned()}}else{i.to_string()},"label":o["label"],"description":o["description"]})).collect::<Vec<_>>()
        })).collect::<Vec<_>>();
        let hash=format!("{:x}",Sha256::digest(json!([inspected["generation"],q]).to_string().as_bytes()));
        json!({"question_id":q["id"],"question_hash":hash,"tool_call_id":q["id"],"run_id":record["run_id"],"conversation_id":record["conversation_id"],
            "agent_id":"main","source":"dsh_native","approval":q["approval"]==true,"can_answer":true,"answer_transport":"native_dsh","questions":items})
    }).collect()
}

pub(super) fn recover(snapshot: &Value, request_id: &str) -> Result<Value> {
    let _guard = lock(None)?;
    let record = binding(string(snapshot,"name"))?;
    if record["permissions_pending"] == true {
        return Err("Resume this DeepSeek session to finish permission setup before recovery".into());
    }
    if record["run_id"] != snapshot["run_id"] || record["conversation_id"] != snapshot["conversation_id"] {
        return Err("DeepSeek session changed before recovery".into());
    }
    recovery::check_native_submission(snapshot)?;
    let fresh=inspect(string(snapshot,"name"),None)?;
    for key in ["run_id","conversation_id","host_generation","model","provider_error","recovery_user_at","recovery_cancel_at"] {
        if fresh[key]!=snapshot[key] {return Err("DeepSeek failed request changed before recovery".into());}
    }
    if fresh["phase"]!="error" || fresh["process_state"]!="running" {
        return Err("DeepSeek is no longer waiting after this failure".into());
    }
    if snapshot["native_recovery"].is_object() {
        rpc(&record,"recover",json!({"sessionId":record["conversation_id"],"id":snapshot["native_recovery"]["id"],
            "generation":snapshot["host_generation"],"requestId":request_id}))
    } else {
        let native=rpc(&record,"send",json!({"sessionId":record["conversation_id"],"requestId":request_id,
            "content":[{"type":"text","text":recovery::CONTINUATION}]}))?;
        Ok(json!({"status":"confirmed","request_id":request_id,"native":native}))
    }
}
pub(super) fn dismiss_recovery(snapshot: &Value) -> Result<Value> {
    let record = binding(string(snapshot,"name"))?;
    rpc(&record,"cancel-recovery",json!({"sessionId":record["conversation_id"],"id":snapshot["native_recovery"]["id"]}))
}

pub(super) fn interrupt(snapshot: &Value) -> Result<Value> {
    let record=binding(string(snapshot,"name"))?;
    if record["run_id"]!=snapshot["run_id"] || record["conversation_id"]!=snapshot["conversation_id"] {
        return Err("DeepSeek session changed before interruption".into());
    }
    rpc(&record,"interrupt",json!({"sessionId":record["conversation_id"],"generation":snapshot["host_generation"],"turnStarted":snapshot["turn_started"]}))
}

fn checked_request(record: &Value) -> Result<Value> {
    let mut bytes = Vec::new();
    io::stdin()
        .take(30 * 1024 * 1024 + 1)
        .read_to_end(&mut bytes)
        .map_err(|e| e.to_string())?;
    if bytes.len() > 30 * 1024 * 1024 {
        return Err("Native request is too large".into());
    }
    let request: Value = serde_json::from_slice(&bytes).map_err(|e| e.to_string())?;
    uuid::Uuid::parse_str(string(&request, "request_id")).map_err(|_| "Invalid request ID")?;
    if request["expected_run_id"] != record["run_id"]
        || request["expected_conversation_id"] != record["conversation_id"]
    {
        return Err("Session changed; refresh before trying again".into());
    }
    Ok(request)
}
/// Lifecycle remains confined to a supported resident host. Never start,
/// dispose, reload or replace a host to implement a mobile scoped action.
pub(super) fn session_action(name: &str, action: &str, new_name: Option<&str>, scope: &session_action::Scope) -> Result<()> {
    if !["pause","resume","rename","forget"].contains(&action) {return Err("This native DeepSeek adapter has no archive/fork lifecycle API".into());}
    let _guard=lock(Some(&root().join("dsh/bindings.lock")))?;
    let mut record=binding(name)?;scope.check(&record)?;
    if let Some(new)=new_name {
        let old:Vec<_>=name.split('/').collect();let parts:Vec<_>=new.split('/').collect();
        if parts.len()!=3 || old.len()!=3 || parts[..2]!=old[..2] || parts.iter().any(|p|p.is_empty() || p.trim()!=*p || p.contains([':', '.']) || p.chars().any(char::is_control)) {
            return Err("Rename must preserve the original DeepSeek provider and project".into());
        }
        if new!=name && exists(new) {return Err("The destination session already exists".into());}
    }
    let ping=rpc(&record,"ping",json!({}))?;
    if ping["sessionActions"]!=true {return Err("The resident DeepSeek adapter lacks scoped lifecycle; it was left running".into());}
    let inspected=rpc(&record,"inspect",json!({"sessionId":record["conversation_id"],"includeProcesses":false}))?;
    if ping["generation"]!=inspected["generation"] || !inspected["lifecycleActions"].as_array().is_some_and(|values|values.iter().any(|value|value==action)) {
        return Err("This action is unavailable for the exact native DeepSeek handle".into());
    }
    let mode=crate::accounts::permission_mode(&crate::config::Config::load().map_err(|e|e.message)?,"native-dsh","dsh").map_err(|e|e.message)?;
    scope.changing();
    let answer=rpc(&record,"session-action",json!({"sessionId":record["conversation_id"],"generation":inspected["generation"],
        "action":action,"title":new_name.map(|name|name.rsplit('/').next().unwrap()),"permissionMode":mode}))?;
    if answer["sessionId"]!=record["conversation_id"] || answer["action"]!=action {return Err("DeepSeek lifecycle acknowledgement did not match".into());}
    if answer["generation"]!=inspected["generation"] {return Err("DeepSeek host changed during lifecycle handoff".into());}
    if answer["status"]=="failed" {scope.unchanged();return Err(string(&answer,"error").into());}
    if answer["status"]!="completed" {return Err("DeepSeek lifecycle handoff was not confirmed".into());}
    match action {
        "pause"=>record["paused"]=json!(true),
        "resume"=>{record["paused"]=json!(false);record["run_id"]=json!(uuid::Uuid::new_v4().to_string());record.as_object_mut().unwrap().remove("permissions_pending");},
        "rename"=>{record["name"]=json!(new_name.unwrap());recovery::rename_job(name,new_name.unwrap(),string(&record,"run_id"))?;},
        "forget"=>{archive::remove_synced(&binding_path(name))?;return Ok(());},
        _=>unreachable!(),
    }
    save(&record)?;
    if action=="rename" && new_name!=Some(name) {crate::overmind::session_renamed(name,new_name.unwrap());archive::remove_synced(&binding_path(name))?;}
    scope.result_record(&record);Ok(())
}

pub(crate) fn dispatch(command: &str, args: &[String]) -> Result<i32> {
    let name = args.first().ok_or("Choose a DeepSeek session")?;
    let _guard = if !["inspect", "exists"].contains(&command) {
        Some(lock(Some(&root().join("dsh/bindings.lock")))?)
    } else {
        None
    };
    let mut record = binding(name)?;
    match command {
        "inspect" => {
            let agent = args
                .windows(2)
                .find(|p| p[0] == "--agent")
                .map(|p| p[1].as_str());
            println!("{}", inspect_with_processes(name, agent, !args.iter().any(|s|s=="--skip-processes"))?);
        }
        "compact-context" => {
            let request = checked_request(&record)?;
            let current = inspect(name, None)?;
            if current["compact_context_supported"] != true { return Err("Native compaction is unavailable or the agent is busy".into()); }
            rpc(&record, "compact", json!({"sessionId":record["conversation_id"],"generation":current["host_generation"],"requestId":request["request_id"]}))?;
            println!("{}",json!({"request_id":request["request_id"],"name":name,"run_id":record["run_id"],"conversation_id":record["conversation_id"],"status":"submitted"}));
        }
        "send" | "settings" => {
            if record["permissions_pending"] == true {
                return Err("Resume this DeepSeek session to finish permission setup before sending or changing settings".into());
            }
            let request = checked_request(&record)?;
            let receipt = json!({"request_id":request["request_id"],"name":name,"run_id":record["run_id"],"conversation_id":record["conversation_id"],"at":now()});
            let mut receipt = receipt;
            if command == "send" {
                let agent_id = string(&request, "agent_id");
                let text = string(&request, "text");
                if text.len() > 65536 {
                    return Err("Message exceeds 64 KiB".into());
                }
                let files = request["attachments"]
                    .as_array()
                    .cloned()
                    .unwrap_or_default();
                let references = files
                    .iter()
                    .map(|f| string(f, "reference"))
                    .collect::<Vec<_>>();
                attachments::validate_references(&references)?;
                for file in &files {
                    if !["image/png", "image/jpeg", "image/webp", "image/gif"]
                        .contains(&string(file, "mime"))
                    {
                        return Err("This DeepSeek composer supports image attachments; use the native UI for other files".into());
                    }
                }
                let mut content = Vec::new();
                for part in attachments::ordered_parts(text, &references) {
                    match part {
                        attachments::MessagePart::Text(value) => {
                            if !value.trim().is_empty() {
                                content.push(json!({"type":"text","text":value}));
                            }
                        }
                        attachments::MessagePart::Attachment(index) => {
                            let file = &files[index];
                            if !references[index].is_empty() && !text.contains(references[index]) {
                                content.push(json!({"type":"text","text":references[index]}));
                            }
                            content.push(json!({"type":"image","mediaType":file["mime"],"data":file["data_base64"],"name":file["name"]}));
                        }
                    }
                }
                if content.is_empty() {
                    return Err("Message is empty".into());
                }
                let receipt_path = root()
                    .join("dsh/receipts")
                    .join(format!("{}.json", string(&request, "request_id")));
                let digest = format!(
                    "{:x}",
                    Sha256::digest(json!([name, request]).to_string().as_bytes())
                );
                if receipt_path.exists() {
                    let previous: Value = serde_json::from_slice(
                        &fs::read(&receipt_path).map_err(|e| e.to_string())?,
                    )
                    .map_err(|e| e.to_string())?;
                    if previous["digest"] != digest {
                        return Err("This request ID was already used for another message".into());
                    }
                    if previous["status"] == "submitted" {
                        println!("{previous}");
                        return Ok(0);
                    }
                    return Err("Delivery uncertain: this message may already be in DeepSeek. Check Activity before sending it again".into());
                }
                if !agent_id.is_empty() {
                    let child = inspect(name, Some(agent_id))?;
                    if child["send_supported"] != true {
                        return Err("This child cannot accept messages; it must be continuable with a live parent".into());
                    }
                    receipt["agent_id"] = json!(agent_id);
                }
                // Authentication is a definite preflight failure, not an
                // uncertain delivery. Do not leave a pending receipt for it.
                let readiness = rpc(
                    &record,
                    "inspect",
                    json!({"sessionId":record["conversation_id"]}),
                )?;
                if readiness["authRequired"] == true {
                    return Err(
                        "Sign in to DeepSeek in the native UI before sending a message".into(),
                    );
                }
                receipt["digest"] = json!(digest);
                receipt["attachments"] = attachments::stage_native(&request)?;
                receipt["text"] = json!(text);
                receipt["submitted_text"] = json!(text);
                receipt["submitted_at"] = json!(now());
                receipt["status"] = json!("pending");
                atomic(&receipt_path, &receipt.to_string())?;
                if agent_id.is_empty() {
                    rpc(
                        &record,
                        "send",
                        json!({"sessionId":record["conversation_id"],"requestId":request["request_id"],"content":content,"mode":"steer","expectedCompactionId":request["expected_compaction_id"]}),
                    )?;
                } else {
                    rpc(
                        &record,
                        "send-child",
                        json!({"parentSessionId":record["conversation_id"],"childSessionId":agent_id,
                        "requestId":request["request_id"],"content":content,"mode":"continuable","delivery":"steer"}),
                    )?;
                }
                receipt["status"] = json!("submitted");
                receipt["transport"] = json!("native_dsh");
                atomic(&receipt_path, &receipt.to_string())?;
            } else {
                let current = inspect(name, None)?;
                let selected = if string(&request, "model").is_empty() {
                    string(&current, "settings_model")
                } else {
                    string(&request, "model")
                };
                let (provider, model) = selected
                    .split_once('/')
                    .ok_or("Choose a native provider and model")?;
                let mut params = json!({"sessionId":record["conversation_id"],"provider":provider,"model":model});
                if !string(&request, "effort").is_empty() {
                    params["reasoningEffort"] = request["effort"].clone();
                }
                let applied = rpc(&record, "settings", params)?;
                receipt["status"] = json!("applied");
                receipt["scope"] = json!("session");
                receipt["model"] = json!(selected);
                receipt["effort"] = applied["selected"]["reasoningEffort"].clone();
            }
            println!("{receipt}");
        }
        "answer" => {
            let request = checked_request(&record)?;
            let inspected = rpc(
                &record,
                "inspect",
                json!({"sessionId":record["conversation_id"]}),
            )?;
            let cards = question_cards(&record, &inspected);
            let card = cards
                .iter()
                .find(|q| {
                    q["question_id"] == request["question_id"]
                        && q["question_hash"] == request["expected_question_hash"]
                })
                .ok_or("This question changed or is no longer pending")?;
            let mut answers = Vec::new();
            for answer in request["answers"]
                .as_array()
                .ok_or("Answers are required")?
            {
                let item = card["questions"]
                    .as_array()
                    .unwrap()
                    .iter()
                    .find(|q| q["id"] == answer["question_id"])
                    .ok_or("Unknown question")?;
                let mut labels = Vec::new();
                for id in answer["selected_option_ids"]
                    .as_array()
                    .ok_or("Invalid selected options")?
                {
                    let option = item["options"]
                        .as_array()
                        .unwrap()
                        .iter()
                        .find(|o| o["id"] == *id)
                        .ok_or("Unknown answer option")?;
                    labels.push(option["label"].clone());
                }
                if item["allow_other"] != true && !string(answer, "text").is_empty() {
                    return Err("Choose an approval outcome".into());
                }
                answers.push(json!({"id":answer["question_id"],"selected":labels,"custom":string(answer,"text")}));
            }
            rpc(
                &record,
                "answer",
                json!({"sessionId":record["conversation_id"],"id":request["question_id"],"generation":inspected["generation"],"answers":answers}),
            )?;
            println!(
                "{}",
                json!({"status":"answered","request_id":request["request_id"],"name":name,"question_id":request["question_id"],"question_hash":request["expected_question_hash"],"run_id":record["run_id"],"conversation_id":record["conversation_id"]})
            );
        }
        "pause" | "resume" => {
            if args.iter().any(|v| v == "--dry-run") {
                println!("{command} official dsh {name}");
                return Ok(0);
            }
            if command == "resume" {
                ensure_host(&record)?;
            }
            if command == "resume" {
                create_session(&mut record)?;
            } else {
                rpc(
                    &record,
                    command,
                    json!({"sessionId":record["conversation_id"]}),
                )?;
            }
            record["paused"] = json!(command == "pause");
            if command == "resume" {
                record["run_id"] = json!(uuid::Uuid::new_v4().to_string());
            }
            save(&record)?;
        }
        "rename" => {
            let new = args.get(1).ok_or("Choose a new session name")?;
            if !new.starts_with("dsh/")
                || new.split('/').count() != 3
                || new.contains(['\n', '\r', ':'])
                || new.chars().any(char::is_whitespace)
            {
                return Err("Invalid DeepSeek session name".into());
            }
            if exists(new) {
                return Err("This name is already used".into());
            }
            if args.iter().any(|v| v == "--dry-run") {
                return Ok(0);
            }
            rpc(
                &record,
                "rename",
                json!({"sessionId":record["conversation_id"],"title":new.rsplit('/').next()}),
            )?;
            record["name"] = json!(new);
            recovery::rename_job(name,new,string(&record,"run_id"))?;
            save(&record)?;
            crate::overmind::session_renamed(name, new);
            fs::remove_file(binding_path(name)).map_err(|e| e.to_string())?;
            println!(
                "{}",
                json!({"old_name":name,"name":new,"new_name":new,"run_id":record["run_id"],"conversation_id":record["conversation_id"]})
            );
        }
        "forget" => {
            if args.iter().any(|v| v == "--dry-run") {
                return Ok(0);
            }
            rpc(
                &record,
                "pause",
                json!({"sessionId":record["conversation_id"]}),
            )?;
            fs::remove_file(binding_path(name)).map_err(|e| e.to_string())?;
        }
        "exists" => return Ok(0),
        _ => {
            return Err(format!(
                "{command} is not supported for native DeepSeek sessions yet"
            ))
        }
    }
    Ok(0)
}

#[cfg(test)]
mod provider_failure_tests {
    use super::*;
    #[test]
    fn failed_native_turn_needs_attention_without_confusing_tool_results() {
        let record =
            json!({"name":"dsh/test/session","conversation_id":"session-one","run_id":"run"});
        let row = json!({"agentAvailable":true,"running":false});
        let failed = json!({"event":{"type":"turn/end","seq":12,"time":100000,"data":{"reason":{"kind":"error","error":{"code":"insufficient_quota","message":"Insufficient balance"}}}}});
        let mut records = json!([{"event":{"type":"tool/result","seq":11,"data":{"message":{"content":"model at capacity"}}}},failed]);
        let normalized = normalize(&record, &row, &json!({"page":{"records":records}}));
        assert_eq!(normalized["phase"], "error");
        assert_eq!(normalized["activity_summary"], "Usage limit reached");
        assert_eq!(normalized["attention_id"], "dsh_native:dsh-error-12");
        assert_eq!(
            native_events(&records).last().unwrap()["type"],
            "StopFailure"
        );
        records
            .as_array_mut()
            .unwrap()
            .push(json!({"event":{"type":"turn/start","seq":13}}));
        assert!(latest_failure(&records).is_null());
        assert_eq!(
            normalize(&record, &row, &json!({"page":{"records":records}}))["phase"],
            "idle"
        );
        records.as_array_mut().unwrap().pop();
        let mut working = row.clone();
        working["running"] = json!(true);
        assert_eq!(
            normalize(&record, &working, &json!({"page":{"records":records}}))["phase"],
            "working"
        );
        let mut stopped = row.clone();
        stopped["agentAvailable"] = json!(false);
        assert_ne!(
            normalize(&record, &stopped, &json!({"page":{"records":records}}))["phase"],
            "error"
        );
        records[1]["event"]["data"]["reason"] = json!({"kind":"completed"});
        assert!(latest_failure(&records).is_null());
        assert_eq!(native_events(&records).last().unwrap()["type"], "Stop");
        records[1]["event"]["data"]["reason"] = json!({"kind":"max-tokens"});
        assert_eq!(latest_failure(&records)["error_kind"], "context_limit");
    }
}

#[cfg(test)]
mod question_identity_tests {
    use super::*;
    #[test]
    fn pending_native_questions_have_an_exact_attention_identity() {
        let record = json!({"name":"dsh/test/session","conversation_id":"session-one","run_id":"run"});
        let row = json!({"agentAvailable":true,"running":false});
        let first = normalize(&record, &row, &json!({"pendingQuestions":[{"id":"question-1"}]}));
        assert_eq!(first["phase"], "input");
        assert_eq!(first["attention_id"], "dsh_question:question-1");
        let next = normalize(&record, &row, &json!({"pendingQuestions":[{"id":"question-2"}]}));
        assert_ne!(next["attention_id"], first["attention_id"]);
        assert!(normalize(&record, &row, &json!({}))["attention_id"].is_null());
    }
}
