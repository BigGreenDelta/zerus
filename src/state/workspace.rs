//! Passive, bounded workspace metadata. Never used to authorize lifecycle actions.
use super::*;
use sha2::{Digest, Sha256};
use std::fs;
use std::process::{Command, Stdio};
use std::time::{Duration, Instant};

pub(super) fn pane_cwds() -> BTreeMap<String, String> {
    cwds(None)
}

/// `pane_cwds` under the names that an earlier pane snapshot gave each session
/// ID (`$N`) in `names`. A rename keeps the ID, so a session renamed since that
/// snapshot keeps its folder under the name the snapshot listed, and one that
/// started since has none (zerus-18u1).
pub(super) fn snapshot_cwds(names: &BTreeMap<String, String>) -> BTreeMap<String, String> {
    cwds(Some(names))
}

fn cwds(names: Option<&BTreeMap<String, String>>) -> BTreeMap<String, String> {
    let Ok(panes) = backend::current().pane_paths() else {
        return BTreeMap::new();
    };
    let mut rows = BTreeMap::<String, (u8, String)>::new();
    for pane in panes {
        if !valid_path(&pane.path) {
            continue;
        }
        // A pane without an ID goes by its name, as in the snapshot.
        let session = match names {
            Some(names) if !pane.session_id.is_empty() => {
                let Some(name) = names.get(&pane.session_id) else {
                    continue;
                };
                name.clone()
            }
            _ => pane.session,
        };
        let priority = u8::from(pane.active_window) * 2 + u8::from(pane.active_pane);
        if rows
            .get(&session)
            .is_none_or(|(score, _)| priority > *score)
        {
            rows.insert(session, (priority, pane.path));
        }
    }
    rows.into_iter()
        .map(|(name, (_, cwd))| (name, cwd))
        .collect()
}

fn valid_path(path: &str) -> bool {
    Path::new(path).is_absolute() && !path.chars().any(char::is_control)
}

fn empty(state: &str) -> Value {
    json!({"git_root":"","git_branch":"","git_worktree":false,
        "git_worktree_name":"","git_detached":false,"git_common_dir":"","git_metadata_state":state})
}

fn git(cwd: &str, args: &[&str], deadline: Instant) -> Option<(bool, String)> {
    let until = (Instant::now() + Duration::from_millis(200)).min(deadline);
    if Instant::now() >= until {
        return None;
    }
    let mut command = Command::new("git");
    command
        .args([
            "--no-optional-locks",
            "-C",
            cwd,
            "-c",
            "core.fsmonitor=false",
        ])
        .args(args)
        .stdin(Stdio::null())
        .stdout(Stdio::piped())
        .stderr(Stdio::null())
        .env("GIT_OPTIONAL_LOCKS", "0")
        .env("LC_ALL", "C");
    for key in [
        "GIT_DIR",
        "GIT_COMMON_DIR",
        "GIT_WORK_TREE",
        "GIT_INDEX_FILE",
        "GIT_OBJECT_DIRECTORY",
        "GIT_ALTERNATE_OBJECT_DIRECTORIES",
        "GIT_CONFIG_COUNT",
    ] {
        command.env_remove(key);
    }
    let mut child = crate::platform::spawn(&mut command).ok()?;
    let status = loop {
        match child.try_wait() {
            Ok(Some(status)) => break status,
            Ok(None) if Instant::now() < until => std::thread::sleep(Duration::from_millis(2)),
            _ => {
                let _ = child.kill();
                let _ = child.wait();
                return None;
            }
        }
    };
    let mut output = String::new();
    child
        .stdout
        .take()?
        .take(16 * 1024)
        .read_to_string(&mut output)
        .ok()?;
    Some((status.success(), output))
}

fn inspect_git(cwd: &str, deadline: Instant) -> Value {
    let Some((ok, output)) = git(
        cwd,
        &[
            "rev-parse",
            "--show-toplevel",
            "--absolute-git-dir",
            "--git-common-dir",
        ],
        deadline,
    ) else {
        return empty("unavailable");
    };
    if !ok {
        return empty("not_repo");
    }
    let lines: Vec<_> = output.lines().collect();
    if lines.len() != 3 {
        return empty("unavailable");
    }
    let absolute = |path: &str| {
        let path = Path::new(path);
        let path = if path.is_absolute() {
            path.to_owned()
        } else {
            Path::new(cwd).join(path)
        };
        crate::platform::canonical(&path).unwrap_or(path)
    };
    let root = absolute(lines[0]);
    let worktree = absolute(lines[1]) != absolute(lines[2]);
    let Some((named, branch)) = git(
        cwd,
        &["symbolic-ref", "--quiet", "--short", "HEAD"],
        deadline,
    ) else {
        return empty("unavailable");
    };
    json!({"git_root":root,"git_branch":if named {branch.trim()} else {""},
        "git_worktree":worktree,"git_worktree_name":if worktree {root.file_name().and_then(|s|s.to_str()).unwrap_or("")} else {""},
        "git_detached":!named,"git_common_dir":absolute(lines[2]),"git_metadata_state":"ok"})
}

fn cached(cwd: &str, deadline: Instant) -> Value {
    if !valid_path(cwd) {
        return empty("unavailable");
    }
    let path = root()
        .join("metadata-cache")
        .join(format!("{:x}.json", Sha256::digest(cwd.as_bytes())));
    let cached = fs::metadata(&path)
        .ok()
        .filter(|m| m.len() < 32768)
        .and_then(|_| fs::read_to_string(&path).ok())
        .and_then(|s| serde_json::from_str::<Value>(&s).ok());
    if let Some(cache) = &cached {
        let age = now() - cache["at"].as_f64().unwrap_or(0.0);
        if cache["cwd"] == cwd && (0.0..15.0).contains(&age) && cache["data"].is_object() {
            return cache["data"].clone();
        }
    }
    if Instant::now() >= deadline {
        return empty("unavailable");
    }
    let data = inspect_git(cwd, deadline);
    let _ = atomic(
        &path,
        &json!({"cwd":cwd,"at":now(),"data":data}).to_string(),
    );
    data
}

pub(super) fn enrich(rows: &mut [Value], live_cwds: &BTreeMap<String, String>) {
    let deadline = Instant::now() + Duration::from_millis(750);
    let mut values = BTreeMap::new();
    for row in rows {
        if string(row, "state") != "archived" {
            if let Some(cwd) = live_cwds.get(string(row, "name")) {
                row["cwd"] = json!(cwd);
                row["cwd_source"] = json!("tmux");
            }
        }
        let cwd = string(row, "cwd").to_owned();
        row["cwd_canonical"] = json!(crate::platform::canonical(&cwd).ok());
        let value = values
            .entry(cwd.clone())
            .or_insert_with(|| cached(&cwd, deadline));
        if let (Some(target), Some(data)) = (row.as_object_mut(), value.as_object()) {
            target.extend(data.clone());
        }
    }
}
