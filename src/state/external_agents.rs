//! Agent CLIs invoked by another provider inherit the HGS run token. Their
//! SessionStart must never rebind the parent conversation.
use super::*;
use std::io::{BufRead, BufReader};

#[derive(Clone, Copy)]
struct Caller {
    provider: &'static str,
    headless: bool,
}

fn native_caller(argv: &[String]) -> Option<Caller> {
    let executable = crate::platform::program_name(Path::new(argv.first()?))?;
    // macOS framework builds run python3 scripts as .../Python.app/Contents/MacOS/Python.
    let offset = if executable == "node" || executable.to_ascii_lowercase().starts_with("python") { 1 } else { 0 };
    let name = crate::platform::program_name(Path::new(argv.get(offset)?))?;
    let provider = match name.as_str() {
        "codex" | "codex.exe" | "codex.js" => "codex",
        "claude" | "claude.exe" => "claude",
        "kimi" => "kimi",
        _ => return None,
    };
    let mut args = argv.iter().skip(offset + 1);
    let headless = provider == "codex" && loop {
        let Some(arg) = args.next() else { break false; };
        if arg == "--" { break false; }
        if ["-c", "--config", "-C", "--cd", "-m", "--model", "-p", "--profile",
            "-s", "--sandbox", "-a", "--ask-for-approval", "--enable", "--disable",
            "--add-dir", "--local-provider"].contains(&arg.as_str()) {
            args.next();
        } else if arg.starts_with('-') {
            // Known switches and --option=value cannot consume the next word.
            if !arg.contains('=') && !["--no-daemon", "--no-alt-screen", "--oss", "--search",
                "--dangerously-bypass-hook-trust", "--dangerously-bypass-approvals-and-sandbox",
                "--full-auto"].contains(&arg.as_str()) { break false; }
        } else { break ["exec", "e", "review", "app-server"].contains(&arg.as_str()); }
    };
    Some(Caller { provider, headless })
}

fn ancestry(root: u32, mut pid: u32, mut read: impl FnMut(u32) -> Option<(u32, Vec<String>)>) -> Option<Caller> {
    let mut caller = None;
    for _ in 0..64 {
        let (parent, argv) = read(pid)?;
        if caller.is_none() { caller = native_caller(&argv); }
        if pid == root { return caller; }
        if parent == pid || parent <= 1 { return None; }
        pid = parent;
    }
    None
}

fn caller(record: &Value) -> Option<Caller> {
    if !process_alive(record) { return None; }
    ancestry(record["pid"].as_u64()? as u32, crate::platform::parent_process_id(), crate::platform::parent_and_argv)
}

// An inherited HGS token is not proof that a new same-provider conversation
// belongs to the interactive parent. Codex records its headless entry point in
// the rollout header; /new and /clear still use the interactive source.
fn codex_exec(event: &Value) -> bool {
    let path = Path::new(string(event, "transcript_path"));
    if !path.is_absolute() { return false; }
    let Ok(file) = std::fs::File::open(path) else { return false; };
    let mut line = String::new();
    if BufReader::new(file.take(256 * 1024)).read_line(&mut line).is_err() { return false; }
    let Ok(meta) = serde_json::from_str::<Value>(&line) else { return false; };
    meta["type"] == "session_meta" && meta["payload"]["id"] == event["session_id"]
        && meta["payload"]["source"] == "exec"
}

fn provider(event: &Value) -> Option<&'static str> {
    let id = string(event, "session_id");
    if id
        .strip_prefix("session_")
        .is_some_and(|v| uuid::Uuid::parse_str(v).is_ok())
    {
        return Some("kimi");
    }
    let path = Path::new(string(event, "transcript_path"));
    if path
        .file_name()
        .is_some_and(|name| name.to_string_lossy().starts_with("rollout-"))
    {
        return Some("codex");
    }
    let claude = nonempty_env("CLAUDE_CONFIG_DIR")
        .map(PathBuf::from)
        .unwrap_or_else(|| home().join(".claude"));
    if path.is_absolute() && path.starts_with(claude) && uuid::Uuid::parse_str(id).is_ok() {
        return Some("claude");
    }
    None
}

/// An unidentified inherited SessionStart cannot replace an established parent.
/// Keep legacy startup hooks and verified interactive /new and /clear events.
pub(super) fn main_start(record: &Value, event: &Value) -> bool {
    if event["session_id"] == record["conversation_id"] { return true; }
    match provider(event).or_else(|| caller(record).map(|source| source.provider)) {
        Some(provider) => provider == string(record, "agent"),
        None => string(record, "conversation_id").is_empty(),
    }
}

/// Return a child-scoped event, without touching parent identity, model, or
/// native transcript. Unknown foreign conversations cannot claim a child.
pub(super) fn route(record: &mut Value, event: &Value) -> Option<Value> {
    let sid = string(event, "session_id");
    if sid.is_empty()
        || sid.len() > 100
        || sid.contains(['/', '\\'])
        || sid.chars().any(char::is_control)
        || string(record, "conversation_id").is_empty()
        || sid == string(record, "conversation_id")
    {
        return None;
    }
    let known = record["subagents"].as_object().and_then(|children| {
        children
            .iter()
            .find(|(_, child)| child["external"] == true && child["conversation_id"] == sid)
            .map(|(id, _)| id.clone())
    });
    let id = if let Some(id) = known {
        id
    } else {
        if string(event, "hook_event_name") != "SessionStart" {
            return None;
        }
        let source = caller(record);
        let agent = provider(event).or_else(|| source.map(|source| source.provider))?;
        let headless = agent == "codex" && (codex_exec(event)
            || source.is_some_and(|source| source.provider == "codex" && source.headless));
        if agent == string(record, "agent") && !headless {
            return None;
        }
        let id = format!("external:{agent}:{sid}");
        let agent_home = nonempty_env(home_var(agent).ok()?)
            .map(PathBuf::from)
            .unwrap_or_else(|| {
                home().join(if agent == "kimi" {
                    ".kimi-code"
                } else if agent == "codex" {
                    ".codex"
                } else {
                    ".claude"
                })
            });
        if !record["subagents"].is_object() {
            record["subagents"] = json!({});
        }
        record["subagents"][&id] = json!({"external":true,"provider":agent,"conversation_id":sid,
            "name":format!("{} {}",agent,sid.trim_start_matches("session_").chars().take(8).collect::<String>()),
            "started":now(),"transcript":event["transcript_path"],"cwd":event["cwd"],"agent_home":agent_home});
        id
    };
    let child = &mut record["subagents"][&id];
    for (from, to) in [
        ("cwd", "cwd"),
        ("transcript_path", "transcript"),
        ("model", "model"),
        ("reasoning_effort", "effort"),
        ("effort", "effort"),
    ] {
        if !string(event, from).is_empty() {
            child[to] = event[from].clone();
        }
    }
    let mut scoped = event.clone();
    scoped["agent_id"] = json!(id);
    // Keep the registered label: a native agent_type such as "default" belongs
    // to the foreign harness, not to the parent provider's profile registry.
    scoped["agent_type"] = child["name"].clone();
    Some(scoped)
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn caller_requires_ancestry_to_the_owned_parent() {
        let rows = [
            (50, (40, vec!["sh", "-c", "hgs __state hook"])),
            (40, (30, vec!["/opt/codex", "exec", "--ephemeral"])),
            (30, (20, vec!["bash", "-c", "review"])),
            (20, (1, vec!["/opt/claude"])),
        ];
        let read = |pid| rows.iter().find(|(id, _)| *id == pid).map(|(_, (parent, argv))|
            (*parent, argv.iter().map(|arg| arg.to_string()).collect()));
        let child = ancestry(20, 50, read).unwrap();
        assert_eq!(child.provider, "codex");
        assert!(child.headless);
        assert!(ancestry(21, 50, read).is_none());
        assert!(ancestry(20, 51, read).is_none());
        let parent = ancestry(20, 20, read).unwrap();
        assert_eq!(parent.provider, "claude");
        assert!(!parent.headless);
        let wrapper = |pid| Some((if pid == 20 { 1 } else { 20 },
            vec!["node".into(), "/opt/bin/codex.js".into(), "--".into(), "exec".into()]));
        assert!(!ancestry(20, 30, wrapper).unwrap().headless);
        assert!(ancestry(20, 30, |pid| Some((pid, vec![]))).is_none());
        for (argv, headless) in [
            (vec!["codex", "-m", "exec"], false),
            (vec!["codex", "--no-daemon", "-c", "model=fixture", "exec", "--ephemeral"], true),
            (vec!["node", "/opt/codex.js", "resume", "exec"], false),
        ] {
            assert_eq!(native_caller(&argv.into_iter().map(str::to_owned).collect::<Vec<_>>()).unwrap().headless, headless);
        }
    }

    #[test]
    fn macos_framework_python_hosts_agents() {
        // macOS ps reports a python3 shebang script under the framework executable.
        let argv = ["/Library/Frameworks/Python.framework/Versions/3.13/Resources/Python.app/Contents/MacOS/Python",
            "/opt/bin/claude"].map(str::to_owned);
        assert_eq!(native_caller(&argv).unwrap().provider, "claude");
    }

    #[test]
    fn unidentified_start_preserves_established_parent_but_allows_legacy_bootstrap() {
        let parent = json!({"agent":"claude","conversation_id":"parent"});
        let unknown = json!({"session_id":"foreign"});
        assert!(!main_start(&parent, &unknown));
        assert!(main_start(&json!({"agent":"claude"}), &unknown));
        assert!(main_start(&parent, &json!({"session_id":"parent"})));
        assert!(!main_start(&parent, &json!({"session_id":"foreign", "transcript_path":"/tmp/rollout-foreign.jsonl"})));
    }
    #[test]
    fn nested_codex_exec_cannot_replace_the_interactive_conversation() {
        let dir = tempfile::tempdir().unwrap();
        let sid = uuid::Uuid::new_v4().to_string();
        let path = dir.path().join(format!("rollout-{sid}.jsonl"));
        let mut meta = json!({"type":"session_meta","payload":{"id":sid,"source":"exec"}});
        std::fs::write(&path,meta.to_string()+"\n").unwrap();
        let parent = json!({"agent":"codex","conversation_id":"parent","transcript":"parent-rollout","model":"parent-model","subagents":{}});
        let event = json!({"hook_event_name":"SessionStart","session_id":sid,"transcript_path":path,"cwd":"/child","model":"child-model"});
        let mut record = parent.clone();
        let scoped = route(&mut record,&event).unwrap();
        let child = string(&scoped,"agent_id");
        assert_eq!(record["conversation_id"],"parent");
        assert_eq!(record["transcript"],"parent-rollout");
        assert_eq!(record["model"],"parent-model");
        assert_eq!(record["subagents"][child]["provider"],"codex");
        let stop = json!({"hook_event_name":"Stop","session_id":sid,"last_assistant_message":"child finished"});
        let scoped = route(&mut record,&stop).unwrap();
        journal::update_activity(&mut record,&scoped);
        assert_eq!(record["subagents"][child]["state"],"finished");
        assert_eq!(record["conversation_id"],"parent");
        // A real /new still replaces the parent's conversation. Neither an
        // unrelated header nor a first headless launch can claim a child.
        for source in ["cli","vscode"] {
            meta["payload"]["source"]=json!(source);
            std::fs::write(&path,meta.to_string()+"\n").unwrap();
            assert!(route(&mut parent.clone(),&event).is_none());
        }
        meta["payload"]["source"]=json!("exec");
        meta["payload"]["id"]=json!("wrong");
        std::fs::write(&path,meta.to_string()+"\n").unwrap();
        assert!(route(&mut parent.clone(),&event).is_none());
        meta["payload"]["id"]=json!(sid);
        std::fs::write(&path,meta.to_string()+"\n").unwrap();
        let mut fresh=parent.clone();fresh["conversation_id"]=Value::Null;
        assert!(route(&mut fresh,&event).is_none());
    }
    #[test]
    fn mixed_children_keep_parent_binding_model_and_history() {
        for (parent, child, sid, path) in [
            (
                "kimi",
                "codex",
                "11111111-1111-4111-8111-111111111111",
                "/tmp/rollout-11111111-1111-4111-8111-111111111111.jsonl",
            ),
            (
                "codex",
                "kimi",
                "session_22222222-2222-4222-8222-222222222222",
                "/tmp/kimi-session",
            ),
        ] {
            let mut record = json!({"agent":parent,"conversation_id":"parent-id","model":"parent-model","transcript":"parent-path","main_done":false,"subagents":{}});
            let mut event = json!({"hook_event_name":"SessionStart","session_id":sid,"transcript_path":path,"cwd":"/tmp/test","model":"child-model"});
            let scoped = route(&mut record, &event).unwrap();
            let id = string(&scoped, "agent_id").to_owned();
            journal::update_activity(&mut record, &scoped);
            assert_eq!(record["conversation_id"], "parent-id");
            assert_eq!(record["transcript"], "parent-path");
            assert_eq!(record["model"], "parent-model");
            assert_eq!(record["subagents"][&id]["provider"], child);
            event["hook_event_name"] = json!("Stop");
            event["last_assistant_message"] = json!("child done");
            let scoped = route(&mut record, &event).unwrap();
            journal::update_activity(&mut record, &scoped);
            assert_eq!(record["subagents"][&id]["state"], "finished");
            assert_eq!(record["subagents"][&id]["detail"], "child done");
            assert_eq!(record["model"], "parent-model");
            assert_eq!(record["main_done"], false);
        }
    }
    #[test]
    fn same_provider_new_conversation_and_unknown_child_are_not_routed() {
        let mut record = json!({"agent":"codex","conversation_id":"parent","subagents":{}});
        assert!(route(&mut record,&json!({"hook_event_name":"SessionStart","session_id":"new","transcript_path":"/tmp/rollout-new.jsonl"})).is_none());
        assert!(route(
            &mut record,
            &json!({"hook_event_name":"Stop","session_id":"unknown"})
        )
        .is_none());
        assert!(route(
            &mut record,
            &json!({"hook_event_name":"SessionStart","session_id":"session_../../other"})
        )
        .is_none());
        assert_eq!(record["subagents"], json!({}));
    }
}
