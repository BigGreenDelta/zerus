//! Provider-owned shell jobs. A returned tool call may still own background work.
//! Reads never consume a provider's output cursor or infer completion from Stop.
use super::*;
use sha2::{Digest, Sha256};
use std::collections::BTreeMap;
use std::fs::File;
use std::io::{BufRead, BufReader, Seek, SeekFrom};

mod control;
mod history;
mod native;
pub(super) use control::WorkSample;
#[cfg(test)]
mod tests;

const MAX_JOBS: usize = 128;
const OUTPUT_BYTES: usize = 32 * 1024;
type Jobs = BTreeMap<String, Value>;

fn text(v: &Value) -> String {
    if let Some(s) = v.as_str() {
        return s.to_owned();
    }
    if let Some(a) = v.as_array() {
        return a
            .iter()
            .filter_map(|v| v.as_str().or_else(|| v["text"].as_str()))
            .collect::<Vec<_>>()
            .join("\n");
    }
    String::new()
}
fn tail(s: &str, cap: usize) -> String {
    let mut start = s.len().saturating_sub(cap);
    while !s.is_char_boundary(start) {
        start += 1;
    }
    s[start..].to_owned()
}
fn token(s: &str) -> bool {
    !s.is_empty()
        && s.len() <= 200
        && s.bytes()
            .all(|b| b.is_ascii_alphanumeric() || b"-_:.".contains(&b))
}
fn owner(event: &Value) -> &str {
    match string(event, "agent_id") {
        "" | "main" => "main",
        other => other,
    }
}
fn id(record: &Value, owner: &str, call: &str) -> String {
    format!(
        "{:x}",
        Sha256::digest(json!([record["conversation_id"], owner, call]).to_string())
    )[..32]
        .into()
}
fn active(job: &Value) -> bool {
    matches!(
        string(job, "status"),
        "starting" | "approval" | "running" | "stopping" | "unknown"
    )
}
fn new_job(record: &Value, owner: &str, call: &str, command: &str, at: f64) -> Value {
    let provider =
        record["subagents"][owner]
            .get("provider")
            .unwrap_or(if record["backend"] == "dsh" {
                &record["backend"]
            } else {
                &record["agent"]
            });
    json!({"id":id(record,owner,call),"call_id":call,"owner":owner,"provider":provider,
        "run_id":record["run_id"],"conversation_id":record["conversation_id"],
        "command":journal::clipped(&json!(command),16000),"cwd":record["cwd"],
        "started_at":at,"updated_at":at,"status":"starting","source":"hooks",
        "background":false,"observed":true,"output":"","output_truncated":false,
        "capabilities":{"output":true,"stop":false,"stdin":false}})
}
fn call_id(event: &Value) -> &str {
    ["tool_use_id", "tool_call_id", "call_id"]
        .iter()
        .map(|k| string(event, k))
        .find(|s| !s.is_empty())
        .unwrap_or("")
}
fn shell_tool(event: &Value) -> bool {
    matches!(
        string(event, "tool_name").rsplit('.').next().unwrap_or(""),
        "Bash" | "bash" | "Shell" | "shell" | "exec_command" | "shell_command"
    )
}
fn put(jobs: &mut Jobs, job: Value) {
    let key = string(&job, "id").to_owned();
    if jobs.get(&key).is_none_or(|old| {
        old["updated_at"].as_f64().unwrap_or(0.) <= job["updated_at"].as_f64().unwrap_or(0.)
    }) {
        jobs.insert(key, job);
    }
    while jobs.len() > MAX_JOBS {
        let oldest = jobs
            .iter()
            .min_by(|a, b| {
                active(a.1).cmp(&active(b.1)).then_with(|| {
                    a.1["updated_at"]
                        .as_f64()
                        .unwrap_or(0.)
                        .total_cmp(&b.1["updated_at"].as_f64().unwrap_or(0.))
                })
            })
            .map(|(k, _)| k.clone())
            .unwrap();
        jobs.remove(&oldest);
    }
}
fn result(job: &mut Value, response: &Value, failed: bool, at: f64) {
    let response = if response["structuredContent"].is_object() {
        &response["structuredContent"]
    } else {
        response
    };
    let raw = if response.is_string() || response.is_array() {
        text(response)
    } else {
        [
            text(&response["stdout"]),
            text(&response["stderr"]),
            text(&response["output"]),
            text(&response["content"]),
        ]
        .into_iter()
        .filter(|s| !s.is_empty())
        .collect::<Vec<_>>()
        .join("\n")
    };
    let previous = string(job, "output");
    let combined = if previous.is_empty() || raw.starts_with(previous) {
        raw.clone()
    } else if raw.is_empty() {
        previous.into()
    } else {
        format!("{previous}\n{raw}")
    };
    job["output_truncated"] =
        json!(job["output_truncated"] == true || combined.len() > OUTPUT_BYTES);
    job["output"] = json!(tail(&combined, OUTPUT_BYTES));
    if !raw.is_empty() {
        job["output_at"] = json!(at);
    }
    let code = response["exit_code"]
        .as_i64()
        .or_else(|| response["exitCode"].as_i64())
        .or_else(|| response["code"].as_i64());
    let native = [
        "backgroundTaskId",
        "background_task_id",
        "session_id",
        "process_id",
        "task_id",
        "taskId",
    ]
    .iter()
    .find_map(|k| {
        let v = &response[k];
        v.as_str()
            .map(str::to_owned)
            .or_else(|| v.as_u64().map(|n| n.to_string()))
    });
    job["updated_at"] = json!(at);
    for key in ["outputFilePath", "persistedOutputPath"] {
        if response[key].is_string() {
            job["output_path"] = response[key].clone();
        }
    }
    if let Some(native) = native.filter(|s| token(s)) {
        job["native_id"] = json!(native);
        job["background"] = json!(true);
        if code.is_none() && !failed {
            job["status"] = json!("running");
            job["observed"] = json!(false);
            job.as_object_mut().unwrap().remove("ended_at");
            return;
        }
    }
    // Polling an existing background task is not an exit notification.
    if job["background"] == true
        && code.is_none()
        && !failed
        && response["interrupted"] != true
        && response["aborted"] != true
        && response["timed_out"] != true
        && response["timedOut"] != true
        && response["is_error"] != true
        && response["isError"] != true
        && !["completed", "failed", "killed", "stopped"].contains(&string(response, "status"))
    {
        job["observed"] = json!(false);
        return;
    }
    job["status"] = json!(
        if response["interrupted"] == true || response["aborted"] == true {
            "stopped"
        } else if response["timed_out"] == true || response["timedOut"] == true {
            "timed_out"
        } else if failed
            || response["is_error"] == true
            || response["isError"] == true
            || code.is_some_and(|n| n != 0)
        {
            "failed"
        } else {
            "completed"
        }
    );
    job["ended_at"] = json!(at);
    job["exit_code"] = json!(code);
}

pub(super) fn observe(record: &mut Value, event: &Value) {
    let kind = string(event, "hook_event_name");
    let call = call_id(event);
    if call.is_empty() || call.len() > 200 {
        return;
    }
    let mut jobs: Jobs = serde_json::from_value(record["shell_jobs"].clone()).unwrap_or_default();
    let owner = owner(event);
    let key = id(record, owner, call);
    let at = now();
    if shell_tool(event)
        || (jobs.contains_key(&key) && matches!(kind, "PostToolUse" | "PostToolUseFailure"))
    {
        let input = &event["tool_input"];
        let command = ["command", "cmd"]
            .iter()
            .map(|k| text(&input[k]))
            .find(|s| !s.is_empty())
            .unwrap_or_default();
        let mut job = jobs
            .remove(&key)
            .unwrap_or_else(|| new_job(record, owner, call, &command, at));
        if !command.is_empty() {
            job["command"] = json!(journal::clipped(&json!(command), 16000));
        }
        if !string(input, "description").is_empty() {
            job["description"] = json!(journal::clipped(&input["description"], 400));
        }
        for field in ["workdir", "cwd"] {
            if !string(input, field).is_empty() {
                job["cwd"] = json!(journal::clipped(&input[field], 4096));
            }
        }
        job["updated_at"] = json!(at);
        job["run_id"] = record["run_id"].clone();
        job["observed"] = json!(true);
        match kind {
            "PreToolUse" => {
                if !active(&job) {
                    return;
                }
                job["status"] = json!("starting");
            }
            "PermissionRequest" => job["status"] = json!("approval"),
            "PostToolUse" | "PostToolUseFailure" => {
                let response = if job["provider"] == "codex" {
                    native::codex_result(&event["tool_response"])
                } else {
                    event["tool_response"].clone()
                };
                result(&mut job, &response, kind == "PostToolUseFailure", at);
            }
            _ => return,
        }
        if !string(&job, "command").is_empty() {
            put(&mut jobs, job);
        }
    } else if matches!(
        string(event, "tool_name").rsplit('.').next().unwrap_or(""),
        "write_stdin" | "TaskOutput" | "TaskStop"
    ) && matches!(kind, "PostToolUse" | "PostToolUseFailure")
    {
        let input = &event["tool_input"];
        let native = input["session_id"]
            .as_str()
            .map(str::to_owned)
            .or_else(|| input["session_id"].as_u64().map(|n| n.to_string()))
            .or_else(|| input["task_id"].as_str().map(str::to_owned))
            .unwrap_or_default();
        if let Some(job) = jobs.values_mut().find(|j| {
            j["owner"] == owner && j["native_id"] == native && j["run_id"] == record["run_id"]
        }) {
            if string(event, "tool_name").ends_with("TaskStop") {
                if kind == "PostToolUse" {
                    job["status"] = json!("stopping");
                    job["updated_at"] = json!(at);
                }
            } else {
                let response = if job["provider"] == "codex" {
                    native::codex_result(&event["tool_response"])
                } else {
                    event["tool_response"].clone()
                };
                result(job, &response, kind == "PostToolUseFailure", at);
            }
        }
    } else {
        return;
    }
    // Manifests carry short tails only; explicit output reads can recover native logs.
    for job in jobs.values_mut() {
        let s = string(job, "output").to_owned();
        if s.len() > 4096 {
            job["output_truncated"] = json!(true);
            job["output"] = json!(tail(&s, 4096));
        }
    }
    record["shell_jobs"] = json!(jobs);
}
pub(super) fn event_job(record: &Value, event: &Value) -> Option<String> {
    let call = call_id(event);
    if call.is_empty() {
        return None;
    }
    let key = id(record, owner(event), call);
    record["shell_jobs"].get(&key).map(|_| key)
}

fn collect(record: &Value, live: bool) -> (Jobs, Vec<String>) {
    let mut jobs: Jobs = serde_json::from_value(record["shell_jobs"].clone()).unwrap_or_default();
    let mut notes = Vec::new();
    if let Err(e) = native::collect(record, &mut jobs, live) {
        notes.push(e);
    }
    if live {
        if let Err(e) = control::collect(record, &mut jobs) {
            notes.push(e);
        }
    }
    for job in jobs.values_mut() {
        if active(job) {
            let lost_foreground = job["background"] != true
                && job["source"] == "hooks"
                && (record["phase"] == "idle"
                    || (job["owner"] == "main"
                        && record["active_tools"].is_object()
                        && record["active_tools"].get(string(job, "call_id")).is_none()));
            let stale = !live
                || job["run_id"] != record["run_id"]
                || job["observed"] != true
                || lost_foreground;
            job["stale"] = json!(stale);
            if stale {
                job["last_status"] = job["status"].clone();
                job["status"] = json!("unknown");
            }
        }
    }
    (jobs, notes)
}
fn projection(jobs: &Jobs, notes: Vec<String>, complete: bool) -> Value {
    let mut items: Vec<_> = jobs.values().cloned().collect();
    items.sort_by(|a, b| {
        active(b).cmp(&active(a)).then_with(|| {
            b["started_at"]
                .as_f64()
                .unwrap_or(0.)
                .total_cmp(&a["started_at"].as_f64().unwrap_or(0.))
        })
    });
    for job in &mut items {
        job.as_object_mut().unwrap().remove("output");
        job.as_object_mut().unwrap().remove("output_path");
    }
    let running = items
        .iter()
        .filter(|j| {
            j["source"] != "process_tree"
                && matches!(
                    string(j, "status"),
                    "starting" | "approval" | "running" | "stopping"
                )
        })
        .count();
    let unknown = items.iter().filter(|j| j["status"] == "unknown").count();
    let live_count = items
        .iter()
        .filter(|j| j["source"] == "process_tree")
        .count();
    json!({"items":items,"active_count":running,"live_count":live_count,"unknown_count":unknown,"complete":complete,"sampled_at":now(),"notes":notes})
}
pub(super) fn snapshot(record: &Value, live: bool) -> Value {
    let (jobs, notes) = collect(record, live);
    projection(&jobs, notes, false)
}
pub(super) fn dsh_snapshot(record: &Value, inspected: &Value) -> Value {
    let jobs = native::dsh(record, inspected);
    projection(
        &jobs,
        if inspected["jobs"].is_array() {
            vec![]
        } else {
            vec!["Live process registry requires an updated native host; recorded commands remain available.".into()]
        },
        false,
    )
}
pub(super) fn dsh_work(record: &Value, inspected: &Value, live: bool) -> Value {
    if !live || (!inspected["jobs"].is_array() && !inspected["shellCalls"].is_array()) {
        return json!({"count":null,"started_at":null,"command":"","sampled_at":now()});
    }
    let jobs = native::dsh(record, inspected);
    let current: Vec<_> = jobs.values().filter(|j| j["observed"] == true
        && matches!(string(j, "source"), "dsh_jobs" | "dsh_calls")
        && matches!(string(j, "status"), "starting" | "running" | "stopping")).collect();
    let first = current.iter().min_by(|a,b| a["started_at"].as_f64().unwrap_or(0.).total_cmp(&b["started_at"].as_f64().unwrap_or(0.)));
    json!({"count":current.len(),"started_at":first.map(|j|j["started_at"].clone()),
        "command":first.map(|j|journal::clipped(&j["command"],240)).unwrap_or_default(),"sampled_at":now()})
}
fn read_tail(path: &Path) -> Result<(String, bool)> {
    let mut f = open_regular(path)?;
    let len = f.metadata().map_err(|e| e.to_string())?.len();
    let offset = len.saturating_sub(OUTPUT_BYTES as u64);
    f.seek(SeekFrom::Start(offset)).map_err(|e| e.to_string())?;
    let mut bytes = Vec::new();
    f.take(OUTPUT_BYTES as u64)
        .read_to_end(&mut bytes)
        .map_err(|e| e.to_string())?;
    Ok((String::from_utf8_lossy(&bytes).into_owned(), offset > 0))
}
fn open_regular(path: &Path) -> Result<File> {
    use std::os::unix::fs::OpenOptionsExt;
    let f = std::fs::OpenOptions::new()
        .read(true)
        .custom_flags(libc::O_NONBLOCK)
        .open(path)
        .map_err(|e| e.to_string())?;
    if !f.metadata().map_err(|e| e.to_string())?.is_file() {
        return Err("Process data is not a regular file".into());
    }
    Ok(f)
}
pub(super) fn dsh_dispatch(
    record: &Value,
    inspected: &Value,
    output: Option<&str>,
    stop: Option<&str>,
    generation: Option<&str>,
) -> Result<Value> {
    let jobs = native::dsh(record, inspected);
    let Some(id) = output.or(stop) else {
        return Ok(dsh_snapshot(record, inspected));
    };
    let job = jobs
        .get(id)
        .ok_or("This process is no longer in the selected conversation")?;
    let mut value = if job["source"] == "dsh_jobs" {
        if generation != inspected["generation"].as_str() {
            return Err("DeepSeek host changed; refresh Processes".into());
        }
        let owner = if job["owner"] == "main" {
            &record["conversation_id"]
        } else {
            &job["owner"]
        };
        dsh::process_rpc(
            record,
            if stop.is_some() {
                "job-stop"
            } else {
                "job-output"
            },
            json!({"sessionId":record["conversation_id"],"jobId":job["native_id"],"owner":owner,"generation":generation}),
        )?
    } else {
        if stop.is_some() {
            return Err("Native control is unavailable for this recorded command".into());
        }
        json!({"output":job["output"],"truncated":job["output_truncated"],"status":job["status"]})
    };
    value["id"] = json!(id);
    value["run_id"] = record["run_id"].clone();
    value["conversation_id"] = record["conversation_id"].clone();
    Ok(value)
}
pub(super) fn dispatch(args: &[String]) -> Result<i32> {
    let name=args.first().ok_or("usage: hgs processes SESSION [--output ID | --stop ID] [--archive ID] --run ID --conversation ID")?;
    let (mut output, mut stop, mut archive, mut run, mut conversation, mut generation) =
        (None, None, None, None, None, None);
    let mut i = 1;
    while i < args.len() {
        if args[i] == "--json" {
            i += 1;
            continue;
        }
        let value = args.get(i + 1).ok_or("Missing process option value")?;
        match args[i].as_str() {
            "--output" => output = Some(value.as_str()),
            "--stop" => stop = Some(value.as_str()),
            "--archive" => archive = Some(value.as_str()),
            "--run" => run = Some(value.as_str()),
            "--conversation" => conversation = Some(value.as_str()),
            "--generation" => generation = Some(value.as_str()),
            _ => return Err("Unknown process option".into()),
        }
        i += 2;
    }
    if output.is_some() && stop.is_some() {
        return Err("Choose output or stop".into());
    }
    if dsh::exists(name) && archive.is_none() {
        return dsh::processes(name, output, stop, run, conversation, generation);
    }
    let record = {
        let _guard = lock(None)?;
        if let Some(id) = archive {
            archive::read_archive(name, id)?
        } else {
            read(name)?
        }
    };
    if output.is_some() || stop.is_some() {
        if run != Some(string(&record, "run_id"))
            || conversation != Some(string(&record, "conversation_id"))
        {
            return Err("The process belongs to a different session run or conversation".into());
        }
    }
    if let Some(id) = stop {
        if archive.is_some() {
            return Err("Archived processes cannot be stopped".into());
        }
        println!("{}", control::stop(&record, id)?);
        return Ok(0);
    }
    let live = archive.is_none() && matches(&record, live()?.get(name)) && process_alive(&record);
    let (jobs, notes) = collect(&record, live);
    let value = if let Some(id) = output {
        let job = jobs
            .get(id)
            .ok_or("This process is no longer in the selected conversation")?;
        let (output, truncated) = if let Some(path) = native::output_path(&record, job)? {
            read_tail(&path)?
        } else {
            (
                string(job, "output").into(),
                job["output_truncated"] == true,
            )
        };
        json!({"id":id,"run_id":record["run_id"],"conversation_id":record["conversation_id"],"output":output,"truncated":truncated,"status":job["status"]})
    } else {
        projection(&jobs, notes, false)
    };
    println!("{value}");
    Ok(0)
}
