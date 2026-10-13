use super::*;
use std::fs;

fn rows(record: &Value) -> Result<Vec<Value>> {
    let path = Path::new(string(record, "transcript"));
    let sid = string(record, "conversation_id");
    if !path.is_absolute()
        || uuid::Uuid::parse_str(sid).is_err()
        || !path
            .file_name()
            .is_some_and(|n| n.to_string_lossy().contains(sid))
    {
        return Ok(vec![]);
    }
    let mut f =
        open_regular(path).map_err(|_| "Native process history is unavailable".to_owned())?;
    if record["agent"] == "codex" {
        let mut first = String::new();
        BufReader::new((&mut f).take(256 * 1024))
            .read_line(&mut first)
            .map_err(|e| e.to_string())?;
        let first: Value = serde_json::from_str(&first).map_err(|e| e.to_string())?;
        if first["type"] != "session_meta" || first["payload"]["id"] != sid {
            return Err("Native process history identity mismatch".into());
        }
    }
    let offset = f
        .metadata()
        .map_err(|e| e.to_string())?
        .len()
        .saturating_sub(8 * 1024 * 1024);
    f.seek(SeekFrom::Start(offset)).map_err(|e| e.to_string())?;
    let mut bytes = Vec::new();
    f.take(8 * 1024 * 1024)
        .read_to_end(&mut bytes)
        .map_err(|e| e.to_string())?;
    Ok(bytes
        .split_inclusive(|b| *b == b'\n')
        .enumerate()
        .filter(|(i, l)| !(offset > 0 && *i == 0) && l.ends_with(b"\n") && l.len() <= 1024 * 1024)
        .filter_map(|(_, l)| serde_json::from_slice(l).ok())
        .collect())
}
fn native_job(record: &Value, call: &str, command: &str, at: f64) -> Value {
    let mut j = new_job(record, "main", call, command, at);
    j["source"] = json!("native_history");
    j["observed"] = json!(false);
    j
}
pub(super) fn collect(record: &Value, jobs: &mut Jobs, live: bool) -> Result<()> {
    match string(record, "agent") {
        "codex" => {
            codex(record, &rows(record)?, jobs);
            history::recover_codex(record, jobs)?;
        }
        "claude" => claude(record, &rows(record)?, jobs),
        "kimi" => kimi(record, jobs, live)?,
        _ => (),
    }
    Ok(())
}
pub(super) fn codex_result(value: &Value) -> Value {
    if value.is_object() {
        return value.clone();
    }
    let s = text(value);
    if let Ok(v) = serde_json::from_str::<Value>(&s) {
        if v.is_object() {
            return v;
        }
    }
    let mut v = json!({"output":s});
    for (pattern, key) in [
        (r"(?m)^Process exited with code (-?\d+)", "exit_code"),
        (
            r"(?m)^Process running with session ID ([A-Za-z0-9_-]+)",
            "session_id",
        ),
    ] {
        let header = s.split_once("\nOutput:").map(|(h, _)| h).unwrap_or(&s);
        if let Some(c) = regex::Regex::new(pattern).unwrap().captures(header) {
            v[key] = if key == "exit_code" {
                json!(c[1].parse::<i64>().ok())
            } else {
                json!(&c[1])
            };
        }
    }
    v
}
pub(super) fn codex(record: &Value, rows: &[Value], jobs: &mut Jobs) {
    let mut followups: BTreeMap<String, String> = BTreeMap::new();
    for e in rows {
        let p = &e["payload"];
        let at = search::timestamp(&e["timestamp"]);
        // Codex records underlying commands even when the model uses code-mode exec.
        let item = &p["item"];
        if matches!(string(p, "type"), "item_started" | "item_completed")
            && matches!(
                string(item, "type"),
                "CommandExecution" | "commandExecution"
            )
        {
            let call = string(item, "id");
            if call.is_empty() {
                continue;
            }
            let started = p["started_at_ms"].as_f64().map(|n| n / 1000.).unwrap_or(at);
            let ended = p["completed_at_ms"]
                .as_f64()
                .map(|n| n / 1000.)
                .unwrap_or(at);
            let command = if item["command"].is_array() {
                item["command"]
                    .as_array()
                    .unwrap()
                    .iter()
                    .filter_map(Value::as_str)
                    .collect::<Vec<_>>()
                    .join(" ")
            } else {
                text(&item["command"])
            };
            let key = id(record, "main", call);
            let mut job = jobs
                .get(&key)
                .cloned()
                .unwrap_or_else(|| native_job(record, call, &command, started));
            if !command.is_empty() {
                job["command"] = json!(journal::clipped(&json!(command), 16000));
            }
            if item["cwd"].is_string() {
                job["cwd"] = item["cwd"].clone();
            }
            let status = string(item, "status");
            if matches!(status, "completed" | "failed" | "declined" | "interrupted") {
                result(
                    &mut job,
                    &json!({"output":item.get("aggregated_output").or_else(||item.get("aggregatedOutput")).unwrap_or(&Value::Null),
                    "exit_code":item.get("exit_code").or_else(||item.get("exitCode")).unwrap_or(&Value::Null),"interrupted":status=="interrupted","status":status}),
                    status == "failed" || status == "declined",
                    ended,
                );
            } else {
                job["status"] = json!("running");
                job["updated_at"] = json!(at);
            }
            let process = item
                .get("process_id")
                .or_else(|| item.get("processId"))
                .unwrap_or(&Value::Null);
            if process.is_string() {
                job["native_id"] = process.clone();
            }
            if item["hgs_output_truncated"] == true {
                job["output_truncated"] = json!(true);
            }
            put(jobs, job);
            continue;
        }
        if p["type"] == "function_call" {
            let name = string(p, "name").rsplit('.').next().unwrap_or("");
            let call = string(p, "call_id");
            if call.is_empty() {
                continue;
            }
            let args: Value = serde_json::from_str(string(p, "arguments")).unwrap_or(Value::Null);
            if name == "write_stdin" {
                let native = args["session_id"]
                    .as_str()
                    .map(str::to_owned)
                    .or_else(|| args["session_id"].as_u64().map(|n| n.to_string()))
                    .unwrap_or_default();
                if let Some(job) = jobs.values().find(|j| j["native_id"] == native) {
                    followups.insert(call.into(), string(job, "id").into());
                }
                continue;
            }
            if !["exec_command", "shell_command", "shell", "Bash"].contains(&name) {
                continue;
            }
            let command = text(
                args.get("cmd")
                    .or_else(|| args.get("command"))
                    .unwrap_or(&Value::Null),
            );
            if command.is_empty() {
                continue;
            }
            let key = id(record, "main", call);
            if jobs.contains_key(&key) {
                continue;
            }
            let mut job = native_job(record, call, &command, at);
            if let Some(cwd) = args.get("workdir").or_else(|| args.get("cwd")) {
                job["cwd"] = cwd.clone();
            }
            put(jobs, job);
        } else if p["type"] == "function_call_output" {
            let call = string(p, "call_id");
            let key = followups
                .get(call)
                .cloned()
                .unwrap_or_else(|| id(record, "main", call));
            if let Some(job) = jobs.get_mut(&key) {
                if job["updated_at"].as_f64().unwrap_or(0.) <= at {
                    result(job, &codex_result(&p["output"]), false, at);
                }
            }
        }
    }
}
pub(super) fn claude(record: &Value, rows: &[Value], jobs: &mut Jobs) {
    for e in rows {
        if e["sessionId"] != record["conversation_id"] || e["isSidechain"] == true {
            continue;
        }
        let at = search::timestamp(&e["timestamp"]);
        for part in e["message"]["content"].as_array().into_iter().flatten() {
            if part["type"] == "tool_use" && part["name"] == "Bash" {
                let call = string(part, "id");
                if call.is_empty() {
                    continue;
                }
                let key = id(record, "main", call);
                if jobs.contains_key(&key) {
                    continue;
                }
                let mut job = native_job(record, call, string(&part["input"], "command"), at);
                job["description"] = json!(journal::clipped(&part["input"]["description"], 400));
                put(jobs, job);
            } else if part["type"] == "tool_result" {
                let key = id(record, "main", string(part, "tool_use_id"));
                if let Some(job) = jobs.get_mut(&key) {
                    if job["updated_at"].as_f64().unwrap_or(0.) > at {
                        continue;
                    }
                    let response = if e["toolUseResult"].is_object() {
                        e["toolUseResult"].clone()
                    } else {
                        json!({"output":text(&part["content"])})
                    };
                    result(job, &response, part["is_error"] == true, at);
                    for k in ["outputFilePath", "persistedOutputPath"] {
                        if response[k].is_string() {
                            job["output_path"] = response[k].clone();
                        }
                    }
                }
            }
        }
        // Only native attachment envelopes can settle a background task. User
        // text containing task-notification XML is not an authoritative event.
        let a = &e["attachment"];
        if e["type"] == "attachment" && a["type"] == "task_notification" {
            let native = a
                .get("taskId")
                .or_else(|| a.get("task_id"))
                .unwrap_or(&Value::Null);
            if let Some(job) = jobs
                .values_mut()
                .find(|j| j["native_id"] == *native && !native.is_null())
            {
                let status = string(a, "status");
                if ["completed", "failed", "killed", "stopped"].contains(&status) {
                    job["status"] = json!(if status == "killed" {
                        "stopped"
                    } else {
                        status
                    });
                    job["ended_at"] = json!(at);
                    job["updated_at"] = json!(at);
                }
            }
        }
    }
}
fn descendants(record: &Value) -> BTreeMap<u32, f64> {
    let root = record["pid"].as_u64().unwrap_or(0) as u32;
    let mut result = BTreeMap::new();
    if root == 0 {
        return result;
    }
    let Ok(p) = std::process::Command::new("ps")
        .args(["-axo", "pid=,ppid=,lstart="])
        .env("LC_ALL", "C")
        .env("TZ", "UTC")
        .output()
    else {
        return result;
    };
    let parents: BTreeMap<u32, (u32, f64)> = String::from_utf8_lossy(&p.stdout)
        .lines()
        .filter_map(|l| {
            let p: Vec<_> = l.split_whitespace().collect();
            if p.len() != 7 {
                return None;
            }
            let month = [
                "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec",
            ]
            .iter()
            .position(|s| *s == p[3])?
                + 1;
            let date = format!(
                "{}-{month:02}-{:02}T{}Z",
                p[6],
                p[4].parse::<u32>().ok()?,
                p[5]
            );
            Some((
                p[0].parse().ok()?,
                (p[1].parse().ok()?, search::timestamp(&json!(date))),
            ))
        })
        .collect();
    for &pid in parents.keys() {
        let mut parent = pid;
        for _ in 0..64 {
            let Some(&(p, _)) = parents.get(&parent) else {
                break;
            };
            if p == root {
                result.insert(pid, parents[&pid].1);
                break;
            }
            if p == parent {
                break;
            }
            parent = p;
        }
    }
    result
}
fn read_json(path: &Path, root: &Path) -> Option<Value> {
    let path = crate::platform::canonical(path).ok()?;
    if !path.starts_with(root) {
        return None;
    }
    let mut f = open_regular(&path).ok()?;
    if f.metadata().ok()?.len() > 128 * 1024 {
        return None;
    }
    let mut b = String::new();
    f.read_to_string(&mut b).ok()?;
    serde_json::from_str(&b).ok()
}
fn kimi(record: &Value, jobs: &mut Jobs, live: bool) -> Result<()> {
    let paths = questions::wire_paths(record)?;
    let pids = if live {
        descendants(record)
    } else {
        Default::default()
    };
    for path in paths {
        let Some(session) = path.parent().and_then(Path::parent).and_then(Path::parent) else {
            continue;
        };
        let session = crate::platform::canonical(session).map_err(|e| e.to_string())?;
        let mut owners = vec![("main".to_owned(), session.clone())];
        for entry in fs::read_dir(session.join("agents"))
            .into_iter()
            .flatten()
            .take(128)
            .flatten()
        {
            let owner = entry.file_name().to_string_lossy().into_owned();
            if token(&owner) {
                owners.push((owner, entry.path()));
            }
        }
        for (owner, dir) in owners {
            let mut native_tasks = false;
            for entry in fs::read_dir(dir.join("tasks"))
                .into_iter()
                .flatten()
                .take(512)
                .flatten()
            {
                let path = entry.path();
                let native = path.file_stem().and_then(|s| s.to_str()).unwrap_or("");
                if path.extension().and_then(|s| s.to_str()) != Some("json") || !token(native) {
                    continue;
                }
                let Some(task) = read_json(&path, &session) else {
                    continue;
                };
                if !["process", "bash"].contains(&string(&task, "kind")) {
                    continue;
                }
                let sid = task.get("session_id");
                if sid.is_some_and(|s| s != &record["conversation_id"]) {
                    continue;
                }
                let task_id = task
                    .get("taskId")
                    .or_else(|| task.get("id"))
                    .and_then(Value::as_str)
                    .unwrap_or(native);
                if task_id != native {
                    continue;
                }
                native_tasks = true;
                let call = task
                    .get("parentToolCallId")
                    .or_else(|| task.get("parent_tool_call_id"))
                    .and_then(Value::as_str)
                    .filter(|s| !s.is_empty())
                    .unwrap_or(native);
                let at = task["startedAt"]
                    .as_f64()
                    .map(|n| n / 1000.)
                    .unwrap_or_else(|| search::timestamp(&task["started_at"]));
                let mut job = jobs
                    .get(&id(record, &owner, call))
                    .cloned()
                    .unwrap_or_else(|| new_job(record, &owner, call, string(&task, "command"), at));
                job["native_id"] = json!(native);
                job["source"] = json!("kimi_tasks");
                job["description"] = json!(journal::clipped(&task["description"], 400));
                job["background"] = task.get("detached").cloned().unwrap_or(json!(true));
                job["status"] = json!(match string(&task, "status") {
                    "killed" | "cancelled" => "stopped",
                    "lost" => "unknown",
                    "running" => "running",
                    "completed" => "completed",
                    "failed" => "failed",
                    "timed_out" => "timed_out",
                    _ => "unknown",
                });
                let pid = task["pid"].as_u64().unwrap_or(0) as u32;
                // Persisted `running` is only live if the exact native task PID
                // is still a descendant of this verified agent process.
                job["observed"] = json!(
                    live && pids
                        .get(&pid)
                        .is_some_and(|birth| at > 0. && (birth - at).abs() < 5.)
                );
                if job["observed"] == true {
                    job["os_pid"] = json!(pid);
                }
                job["exit_code"] = task
                    .get("exitCode")
                    .or_else(|| task.get("exit_code"))
                    .cloned()
                    .unwrap_or(Value::Null);
                if let Some(end) = task["endedAt"].as_f64().map(|n| n / 1000.).or_else(|| {
                    let n = search::timestamp(&task["ended_at"]);
                    (n > 0.).then_some(n)
                }) {
                    job["ended_at"] = json!(end);
                }
                let recorded_at = task["updatedAt"]
                    .as_f64()
                    .map(|n| n / 1000.)
                    .or_else(|| {
                        fs::metadata(&path)
                            .ok()?
                            .modified()
                            .ok()?
                            .duration_since(std::time::UNIX_EPOCH)
                            .ok()
                            .map(|d| d.as_secs_f64())
                    })
                    .unwrap_or(at);
                job["updated_at"] = json!(if job["observed"] == true {
                    now()
                } else {
                    recorded_at
                });
                job["output_path"] = json!(dir.join("tasks").join(native).join("output.log"));
                put(jobs, job);
            }
            // Kimi's registry contains foreground tasks too. Older task files
            // omit parentToolCallId, so prefer this authoritative owner roster
            // instead of guessing a hook/task match from the command text.
            if native_tasks {
                jobs.retain(|_, job| job["owner"] != owner || job["source"] == "kimi_tasks");
            }
        }
    }
    Ok(())
}
pub(super) fn output_path(record: &Value, job: &Value) -> Result<Option<PathBuf>> {
    let candidate = PathBuf::from(string(job, "output_path"));
    if !candidate.is_absolute() {
        return Ok(None);
    }
    let path = crate::platform::canonical(&candidate)
        .map_err(|_| "The native process log is no longer available".to_owned())?;
    let allowed = match string(record, "agent") {
        "kimi" => questions::wire_paths(record)?
            .iter()
            .filter_map(|p| p.parent().and_then(Path::parent).and_then(Path::parent))
            .any(|root| path.starts_with(root)),
        "claude" => {
            let directory = format!("claude-{}", unsafe { libc::getuid() });
            let mut roots = vec![
                std::env::temp_dir().join(&directory),
                Path::new("/tmp").join(directory),
            ];
            if let Some(parent) = Path::new(string(record, "transcript")).parent() {
                if uuid::Uuid::parse_str(string(record, "conversation_id")).is_ok() {
                    roots.push(
                        parent
                            .join(string(record, "conversation_id"))
                            .join("tool-results"),
                    );
                }
            }
            roots
                .iter()
                .any(|root| crate::platform::canonical(root).is_ok_and(|root| path.starts_with(root)))
        }
        _ => false,
    };
    if !allowed {
        return Err("Native process output path is outside this provider's output storage".into());
    }
    Ok(Some(path))
}
pub(super) fn dsh(record: &Value, inspected: &Value) -> Jobs {
    let mut jobs = Jobs::new();
    for row in inspected["page"]["records"]
        .as_array()
        .into_iter()
        .flatten()
    {
        let e = &row["event"];
        let data = &e["data"];
        let at = e["time"].as_f64().unwrap_or(0.) / 1000.;
        if e["type"] == "tool/call" && data["name"] == "bash" {
            let call = data
                .get("id")
                .or_else(|| data.get("callId"))
                .and_then(Value::as_str)
                .unwrap_or("");
            if call.is_empty() {
                continue;
            }
            let args = if data["arguments"].is_string() {
                serde_json::from_str(string(data, "arguments")).unwrap_or(Value::Null)
            } else {
                data["arguments"].clone()
            };
            let mut job = native_job(record, call, string(&args, "command"), at);
            job["background"] = json!(args["run_in_background"] == true);
            put(&mut jobs, job);
        } else if e["type"] == "tool/result" {
            let m = &data["message"];
            let call = m
                .get("toolCallId")
                .or_else(|| m.get("tool_call_id"))
                .and_then(Value::as_str)
                .unwrap_or("");
            if let Some(job) = jobs.get_mut(&id(record, "main", call)) {
                result(
                    job,
                    &json!({"output":text(&m["content"])}),
                    m["isError"] == true,
                    at,
                );
            }
        }
    }
    for call in inspected["shellCalls"].as_array().into_iter().flatten() {
        let owner = if call["owner"] == record["conversation_id"] {
            "main"
        } else {
            string(call, "owner")
        };
        let call_id = string(call, "callId");
        if call_id.is_empty() {
            continue;
        }
        let key = id(record, owner, call_id);
        let at = call["startedAt"].as_f64().unwrap_or(0.) / 1000.;
        let mut job = jobs
            .get(&key)
            .cloned()
            .unwrap_or_else(|| new_job(record, owner, call_id, string(call, "command"), at));
        job["status"] = call["status"].clone();
        job["observed"] = json!(true);
        job["updated_at"] = json!(now());
        job["source"] = json!("dsh_calls");
        if let Some(end) = call["finishedAt"].as_f64() {
            job["ended_at"] = json!(end / 1000.);
        }
        job["exit_code"] = call["exitCode"].clone();
        if call["exitCode"].as_i64().is_some_and(|n| n != 0) {
            job["status"] = json!("failed");
        }
        put(&mut jobs, job);
    }
    for native in inspected["jobs"].as_array().into_iter().flatten() {
        if native["kind"] != "bash" {
            continue;
        }
        let owner = native.get("owner").and_then(Value::as_str).unwrap_or("");
        if owner.is_empty() {
            continue;
        }
        let owner = if Some(owner) == record["conversation_id"].as_str() {
            "main"
        } else {
            owner
        };
        let native_id = string(native, "id");
        let at = native["startedAt"].as_f64().unwrap_or(0.) / 1000.;
        let mut job = new_job(
            record,
            owner,
            &format!("{}:{native_id}", string(inspected, "generation")),
            string(native, "label"),
            at,
        );
        let call = string(native, "callId");
        if !call.is_empty() {
            if let Some(previous) = jobs.remove(&id(record, owner, call)) {
                job["output"] = previous["output"].clone();
                job["command"] = previous["command"].clone();
                job["cwd"] = previous["cwd"].clone();
            }
            job["call_id"] = json!(call);
        }
        job["native_id"] = json!(native_id);
        job["source"] = json!("dsh_jobs");
        job["background"] = json!(true);
        job["status"] = json!(if native["status"] == "killed" {
            "stopped"
        } else {
            string(native, "status")
        });
        job["generation"] = inspected["generation"].clone();
        job["updated_at"] = json!(now());
        job["progress"] = native["progress"].clone();
        job["detail"] = native["detail"].clone();
        if string(native, "detail").starts_with("exit code: ")
            && string(native, "detail") != "exit code: 0"
            && job["status"] == "completed"
        {
            job["status"] = json!("failed");
        }
        job["capabilities"]["stop"] = json!(native["controllable"] == true && active(&job));
        if let Some(end) = native["finishedAt"].as_f64() {
            job["ended_at"] = json!(end / 1000.);
        }
        put(&mut jobs, job);
    }
    for job in jobs.values_mut() {
        if active(job) && job["observed"] != true {
            job["last_status"] = job["status"].clone();
            job["status"] = json!("unknown");
            job["stale"] = json!(true);
        }
    }
    jobs
}
