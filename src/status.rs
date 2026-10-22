//! `hgs status` and `hgs version`: what runs on this computer and its peers.
//! Status is read-only. A peer reports its own services only when its hgs
//! lists `status` in its help; an older peer would launch an agent named
//! `status`, so it is asked for its session listing instead.
use crate::{
    cli::{Error, Result},
    config::Config,
    platform,
};
use serde_json::{json, Value};
use std::{path::PathBuf, process::Stdio};

const VERSION: &str = env!("CARGO_PKG_VERSION");
const HELP: &str = "usage: hgs [@host] status [--json] [--local]";
// The USAGE line that tells a peer's help apart from an older hgs.
#[cfg(test)]
const MARKER: &str = "hgs [@host] status";
// `--help` is safe on every hgs; only a peer that lists status runs it.
const PEER_COMMAND: &str = "h=~/.local/bin/hgs; if \"$h\" --help 2>/dev/null | grep -qF 'hgs [@host] status'; \
    then exec \"$h\" status --local --json; else exec \"$h\" ls --local --json; fi";

pub fn version(config: &Config, target: &str, args: &[String]) -> Result<i32> {
    if target.is_empty() || config.is_self(target) {
        println!("hgs {VERSION}");
        return Ok(0);
    }
    if !args.is_empty() {
        return Err(Error::new(1, "usage: hgs [@host] version"));
    }
    // Every hgs answers --version without side effects.
    let output = ssh(config, target, "~/.local/bin/hgs --version")?;
    if !output.status.success() {
        return Err(Error::new(3, format!("{target} unreachable: {}", detail(&output))));
    }
    print!("{}", String::from_utf8_lossy(&output.stdout));
    Ok(0)
}

pub fn run(config: &Config, target: &str, args: &[String], dry: bool) -> Result<i32> {
    let (mut json, mut local) = (false, false);
    for arg in args {
        match arg.as_str() {
            "--json" => json = true,
            "--local" => local = true,
            _ => return Err(Error::new(1, HELP)),
        }
    }
    if dry {
        return Err(Error::new(1, "hgs status only reads; run it without --dry-run"));
    }
    let remote = !target.is_empty() && !config.is_self(target);
    let machines = if remote {
        vec![peer(config, target)]
    } else if local {
        vec![this(config)?]
    } else {
        std::iter::once(this(config))
            .chain(config.peers.iter().map(|p| Ok(peer(config, p))))
            .collect::<Result<_>>()?
    };
    if json {
        println!("{}", if remote || local { machines[0].clone() } else { json!(machines) });
    } else {
        for (index, machine) in machines.iter().enumerate() {
            print!("{}", render(machine, index == 0 && !remote));
        }
    }
    Ok(if remote && machines[0]["ok"] != true { 3 } else { 0 })
}

fn this(config: &Config) -> Result<Value> {
    let listing = crate::cli::local_json(config)?;
    let executable = std::env::current_exe().ok().map(|path| path.display().to_string());
    Ok(json!({"host":config.host(),"ok":true,"hgs_version":VERSION,"executable":executable,
        "local_sessions":listing["local_sessions"] != false,"sessions":counts(&listing["sessions"]),
        "services":{"desktop":desktop(),"swarm_worker":swarm(config),
            "recovery_worker":crate::state::recovery_worker_running().map(running),
            "mobile_connector":connector(config)}}))
}

fn running(value: bool) -> &'static str {
    if value { "running" } else { "stopped" }
}

// Sessions by what they need: the desktop's live states, then saved and archived ones.
fn counts(sessions: &Value) -> Value {
    let (mut working, mut attention, mut ready, mut other, mut stopped, mut archived) = (0, 0, 0, 0, 0, 0);
    for session in sessions.as_array().into_iter().flatten() {
        let field = |key: &str| session[key].as_str().unwrap_or("");
        match (field("state"), field("phase"), field("activity")) {
            ("archived", _, _) => archived += 1,
            ("stopped" | "paused", _, _) => stopped += 1,
            (_, "approval" | "input" | "error", _) => attention += 1,
            (_, _, "busy") => working += 1,
            (_, _, "idle") => ready += 1,
            _ => other += 1,
        }
    }
    json!({"live":working + attention + ready + other,"working":working,"attention":attention,
        "ready":ready,"other":other,"stopped":stopped,"archived":archived})
}

#[cfg(unix)]
fn desktop() -> Option<&'static str> {
    // The desktop app keeps its executable name on every Unix (hgs-tray).
    let uid = unsafe { libc::getuid() }.to_string();
    let status = std::process::Command::new("pgrep").args(["-x", "-u", &uid, "hgs-tray"])
        .stdin(Stdio::null()).stdout(Stdio::null()).stderr(Stdio::null()).status().ok()?;
    match status.code() {
        Some(0) => Some("running"),
        Some(1) => Some("stopped"),
        _ => None,
    }
}

#[cfg(not(unix))]
fn desktop() -> Option<&'static str> {
    None
}

fn swarm(config: &Config) -> &'static str {
    let worker = crate::swarm::worker_status(config);
    match (worker["running"] == true, worker["current"] == true) {
        (true, true) => "running",
        (true, false) => "outdated",
        _ => "stopped",
    }
}

// The connector holds an exclusive lock on its journal while it runs.
fn connector(config: &Config) -> &'static str {
    let Ok(text) = std::fs::read(config.dir.join("mobile/connector.json")) else {
        return "not_configured";
    };
    let settings: Value = serde_json::from_slice(&text).unwrap_or(Value::Null);
    let state = match settings["state_dir"].as_str().filter(|s| !s.is_empty()) {
        Some(path) => match path.strip_prefix("~/") {
            Some(rest) => PathBuf::from(&config.home).join(rest),
            None => PathBuf::from(path),
        },
        None => PathBuf::from(&config.home).join(".local/state/hgs/mobile-connector"),
    };
    running(locked(&state.join("connector.lock")))
}

pub(crate) fn locked(path: &std::path::Path) -> bool {
    let Ok(file) = std::fs::File::open(path) else {
        return false;
    };
    match fs2::FileExt::try_lock_shared(&file) {
        Ok(()) => {
            drop(crate::platform::FileLock::held(file));
            false
        }
        Err(error) => error.raw_os_error() == fs2::lock_contended_error().raw_os_error(),
    }
}

fn ssh(config: &Config, peer: &str, command: &str) -> Result<std::process::Output> {
    if peer.starts_with('-') || peer.chars().any(char::is_whitespace) {
        return Err(Error::new(1, "invalid SSH host alias"));
    }
    platform::ssh_output(
        platform::ssh()
            .args(crate::machines::ssh_options(config, peer))
            .args(["-o", "BatchMode=yes", "-o", "ConnectTimeout=3", peer, command])
            .stdin(Stdio::null())
            .stderr(Stdio::piped()),
    )
    .map_err(|e| Error::new(3, format!("Could not start SSH: {e}")))
}

fn detail(output: &std::process::Output) -> String {
    let text: String = String::from_utf8_lossy(&output.stderr).chars()
        .filter(|c| !c.is_control() || *c == '\n').take(512).collect();
    if text.trim().is_empty() { format!("SSH command failed ({})", output.status) } else { text.trim().to_owned() }
}

fn peer(config: &Config, peer: &str) -> Value {
    let output = match ssh(config, peer, &platform::remote_sh(PEER_COMMAND)) {
        Ok(output) => output,
        Err(error) => return json!({"host":peer,"ok":false,"error":"offline","detail":error.message}),
    };
    if !output.status.success() {
        return json!({"host":peer,"ok":false,"error":"offline","detail":detail(&output)});
    }
    match serde_json::from_slice::<Value>(&output.stdout) {
        Ok(status) if status.get("services").is_some() => status,
        // An older hgs: its listing still has its version and sessions.
        Ok(listing) if listing["sessions"].is_array() => json!({"host":peer,"ok":true,
            "hgs_version":listing["hgs_version"],"local_sessions":listing["local_sessions"] != false,
            "sessions":counts(&listing["sessions"]),"services":null}),
        _ => json!({"host":peer,"ok":false,"error":"bad_response","detail":"Invalid response from hgs on the remote machine"}),
    }
}

fn render(machine: &Value, this: bool) -> String {
    let host = machine["host"].as_str().unwrap_or("?");
    let suffix = if this { " (this computer)" } else { "" };
    if machine["ok"] != true {
        let detail = machine["detail"].as_str().unwrap_or("unreachable").lines().next().unwrap_or("");
        return format!("{host}{suffix}: offline: {detail}\n");
    }
    let mut text = format!("{host}{suffix}: hgs {}", machine["hgs_version"].as_str().unwrap_or("unknown"));
    if let Some(path) = machine["executable"].as_str() {
        text += &format!(" ({path})");
    }
    text += "\n";
    let sessions = &machine["sessions"];
    let n = |key: &str| sessions[key].as_u64().unwrap_or(0);
    let live: Vec<String> = [("working", "working"), ("attention", "need attention"), ("ready", "ready"), ("other", "starting or unknown")]
        .iter().filter(|(key, _)| n(key) > 0).map(|(key, label)| format!("{} {label}", n(key))).collect();
    let mut parts = Vec::new();
    if machine["local_sessions"] == false {
        parts.push("control console".to_owned());
    } else if n("live") > 0 {
        parts.push(format!("{} live ({})", n("live"), live.join(", ")));
    } else {
        parts.push("none live".to_owned());
    }
    for (key, label) in [("stopped", "stopped or paused"), ("archived", "archived")] {
        if n(key) > 0 {
            parts.push(format!("{} {label}", n(key)));
        }
    }
    text += &format!("  Sessions: {}\n", parts.join("; "));
    let services = &machine["services"];
    if services.is_null() {
        text += &format!("  Services: not reported; update hgs on {host} to see them\n");
        return text;
    }
    for (key, label) in [("desktop", "Desktop app"), ("swarm_worker", "Swarm worker"),
        ("recovery_worker", "Recovery worker"), ("mobile_connector", "Mobile connector")] {
        let state = match services[key].as_str() {
            Some("running") => "running",
            Some("stopped") => "not running",
            Some("outdated") => "running an older hgs; it restarts on its next check",
            _ => continue,
        };
        text += &format!("  {label}: {state}\n");
    }
    text
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn sessions_are_counted_by_what_they_need() {
        let sessions = json!([{"activity":"busy","phase":"tool"},{"activity":"busy","phase":"input"},
            {"activity":"idle","phase":"idle"},{"activity":"unknown","phase":"unknown"},
            {"state":"paused","activity":"idle"},{"state":"stopped"},{"state":"archived","phase":"input"},
            {"activity":"idle","phase":"approval"},{"phase":"error"}]);
        assert_eq!(counts(&sessions), json!({"live":6,"working":1,"attention":3,"ready":1,"other":1,
            "stopped":2,"archived":1}));
        assert_eq!(counts(&Value::Null)["live"], 0);
    }

    #[test]
    fn text_names_each_machine_its_sessions_and_services() {
        let machine = json!({"host":"arch","ok":true,"hgs_version":"1.2.3","executable":"/opt/hgs","local_sessions":true,
            "sessions":{"live":3,"working":1,"attention":0,"ready":2,"other":0,"stopped":0,"archived":4},
            "services":{"desktop":"running","swarm_worker":"outdated","recovery_worker":null,"mobile_connector":"not_configured"}});
        assert_eq!(render(&machine, true), "arch (this computer): hgs 1.2.3 (/opt/hgs)\n  \
            Sessions: 3 live (1 working, 2 ready); 4 archived\n  Desktop app: running\n  \
            Swarm worker: running an older hgs; it restarts on its next check\n");
        let older = json!({"host":"mac","ok":true,"hgs_version":"1.0.0","sessions":counts(&json!([])),"services":null});
        assert_eq!(render(&older, false), "mac: hgs 1.0.0\n  Sessions: none live\n  \
            Services: not reported; update hgs on mac to see them\n");
        let offline = json!({"host":"mac","ok":false,"detail":"ssh: connect to host mac: timed out\nmore"});
        assert_eq!(render(&offline, false), "mac: offline: ssh: connect to host mac: timed out\n");
    }

    #[test]
    fn peers_run_status_only_when_their_help_lists_it() {
        assert!(PEER_COMMAND.contains(&format!("grep -qF '{MARKER}'")));
        assert!(crate::cli::usage().contains(MARKER));
        assert!(PEER_COMMAND.contains("else exec \"$h\" ls --local --json"));
    }
}
