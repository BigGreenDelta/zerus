//! Provider-independent control of verified OS process trees. Native call IDs
//! are never interpreted as PIDs or matched to a process by command text.
use super::*;
use std::time::{Duration, Instant};

#[derive(Clone)]
struct Process {
    pid: u32,
    parent: u32,
    group: Option<u32>,
    birth: String,
    started: f64,
    command: String,
    executable: String,
}

/// Shells an agent starts for its commands, by `platform::program_name`.
/// Windows has no process groups, so its own shells mark background work there.
const SHELLS: &[&str] = if cfg!(windows) {
    &["bash", "zsh", "sh", "dash", "fish", "nu", "pwsh", "powershell", "cmd"]
} else {
    &["bash", "zsh", "sh", "dash", "fish", "nu", "pwsh"]
};

fn descendant(pid: u32, root: u32, rows: &BTreeMap<u32, Process>) -> bool {
    let mut at = pid;
    for _ in 0..128 {
        let Some(p) = rows.get(&at) else {
            return false;
        };
        if p.parent == root {
            return true;
        }
        if p.parent == at || p.parent <= 1 {
            return false;
        }
        at = p.parent;
    }
    false
}
fn process_table() -> Result<BTreeMap<u32, Process>> {
    Ok(crate::platform::process_table()?
        .into_values()
        .map(|row| {
            (
                row.pid,
                Process {
                    pid: row.pid,
                    parent: row.parent,
                    group: row.group,
                    birth: String::new(),
                    started: 0.,
                    command: String::new(),
                    executable: String::new(),
                },
            )
        })
        .collect())
}
fn verified_table(record: &Value, mut rows: BTreeMap<u32, Process>) -> Result<BTreeMap<u32, Process>> {
    if !process_alive(record) {
        return Err("The owning agent process changed".into());
    }
    let root = record["pid"].as_u64().unwrap_or(0) as u32;
    let eligible: Vec<_> = rows
        .keys()
        .copied()
        .filter(|&pid| pid == root || descendant(pid, root, &rows))
        .collect();
    rows.retain(|pid, _| eligible.contains(pid));
    rows.retain(|pid, p| {
        if let Some(identity) = crate::platform::process_identity(*pid) {
            if identity.parent == p.parent && identity.group == p.group {
                p.birth = identity.birth;
                p.started = identity.started;
                // Read executable/argv from the kernel, not newline-delimited `ps` command
                // text. A command containing newlines must not spoof a process row or helper.
                if let Some(image) = crate::platform::describe(*pid) {
                    p.command = match image.argv {
                        Some(argv) => journal::clipped(&json!(argv.join(" ")), 16000),
                        None => image.executable.clone(),
                    };
                    p.executable = image.executable;
                    return true;
                }
            }
        }
        false
    });
    Ok(rows)
}
fn table(record: &Value) -> Result<BTreeMap<u32, Process>> {
    if !process_alive(record) {
        return Err("The owning agent process changed".into());
    }
    verified_table(record, process_table()?)
}

// One lazy OS table per fleet listing, with identities revalidated for each
// owner. This path never scans native transcripts or treats recorded jobs as
// current work. Do not retain this sample across separate listing requests.
#[derive(Default)]
pub(crate) struct WorkSample {
    rows: Option<Result<BTreeMap<u32, Process>>>,
}
impl WorkSample {
    pub(crate) fn summary(&mut self, record: &Value, live: bool) -> Value {
        let unavailable = || json!({"count":null,"started_at":null,"command":"","sampled_at":now()});
        if !live || !process_alive(record) || string(record, "run_id").is_empty()
            || string(record, "conversation_id").is_empty() {
            return unavailable();
        }
        let Ok(rows) = self.rows.get_or_insert_with(process_table).clone()
            .and_then(|rows| verified_table(record, rows)) else { return unavailable(); };
        let current = roots(record, &rows);
        let first = current.iter().map(|pid| &rows[pid])
            .min_by(|a,b| a.started.total_cmp(&b.started));
        json!({"count":current.len(),"started_at":first.map(|p|p.started),
            "command":first.map(|p|journal::clipped(&json!(p.command),240)).unwrap_or_default(),"sampled_at":now()})
    }
}
fn roots(record: &Value, rows: &BTreeMap<u32, Process>) -> Vec<u32> {
    let root = record["pid"].as_u64().unwrap_or(0) as u32;
    let Some(agent) = rows.get(&root) else {
        return vec![];
    };
    let candidates: Vec<_> = rows
        .values()
        .filter(|p| {
            if p.pid == root
                || !descendant(p.pid, root, rows)
                || p.pid == std::process::id()
                || descendant(std::process::id(), p.pid, rows)
            {
                return false;
            }
            let executable =
                crate::platform::program_name(Path::new(&p.executable)).unwrap_or_default();
            let executable = executable.as_str();
            if [
                "codex-code-mode",
                "codex-code-mode-host",
                "codex-exec-server",
                "hgs",
                "hgs-tray",
            ]
            .contains(&executable)
            {
                return false;
            }
            (p.group == Some(p.pid) && p.group != agent.group)
                || (p.parent == root && SHELLS.contains(&executable))
        })
        .map(|p| p.pid)
        .collect();
    candidates
        .iter()
        .copied()
        .filter(|pid| {
            !candidates
                .iter()
                .any(|other| pid != other && descendant(*pid, *other, rows))
        })
        .collect()
}
fn job(record: &Value, process: &Process, rows: &BTreeMap<u32, Process>) -> Value {
    let key = format!(
        "os-{:x}",
        Sha256::digest(
            json!([
                record["run_id"],
                record["conversation_id"],
                process.pid,
                process.birth
            ])
            .to_string()
        )
    );
    let children = rows
        .keys()
        .filter(|&&pid| descendant(pid, process.pid, rows))
        .count();
    json!({"id":key,"source":"process_tree","owner":"main","provider":record["agent"],
        "run_id":record["run_id"],"conversation_id":record["conversation_id"],"command":process.command,
        "os_pid":process.pid,"process_birth":process.birth,"started_at":process.started,"updated_at":now(),
        "status":"running","observed":true,"background":true,"child_count":children,
        "description":format!("PID {} ({} child processes)",process.pid,children),
        "capabilities":{"output":false,"stop":true,"stdin":false}})
}
pub(super) fn collect(record: &Value, jobs: &mut Jobs) -> Result<()> {
    let rows = table(record)?;
    for pid in roots(record, &rows).into_iter().take(MAX_JOBS) {
        put(jobs, job(record, &rows[&pid], &rows));
    }
    Ok(())
}

pub(super) fn stop(record: &Value, id: &str) -> Result<Value> {
    let guard = lock(None)?;
    let current = read(string(record, "name"))?;
    if current["run_id"] != record["run_id"]
        || current["conversation_id"] != record["conversation_id"]
        || current["pid"] != record["pid"]
        || !matches(&current, live()?.get(string(record, "name")))
        || !process_alive(&current)
    {
        return Err("Session changed before stopping the process".into());
    }
    let rows = table(&current)?;
    let pid = roots(&current, &rows)
        .into_iter()
        .find(|pid| job(&current, &rows[pid], &rows)["id"] == id)
        .ok_or("This live process no longer belongs to the selected session")?;
    let mut targets: Vec<_> = rows
        .values()
        .filter(|p| p.pid == pid || descendant(p.pid, pid, &rows))
        .cloned()
        .collect();
    // Leaf processes first. Never signal the agent or a whole reusable process group.
    targets.sort_by_key(|p| {
        std::cmp::Reverse(
            rows.keys()
                .filter(|&&parent| descendant(p.pid, parent, &rows))
                .count(),
        )
    });
    let targets: Vec<_> = targets
        .into_iter()
        .map(|p| crate::platform::PinnedProcess::pin(p.pid, &p.birth))
        .collect::<Result<_>>()?;
    drop(guard);
    for target in &targets {
        target.terminate()?;
    }
    let deadline = Instant::now() + Duration::from_secs(2);
    while targets.iter().any(crate::platform::PinnedProcess::alive) && Instant::now() < deadline {
        std::thread::sleep(Duration::from_millis(40));
    }
    for target in &targets {
        target.kill()?;
    }
    Ok(
        json!({"id":id,"run_id":record["run_id"],"conversation_id":record["conversation_id"],"status":"requested","os_pid":pid}),
    )
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn roots_keep_shell_trees_and_exclude_helpers_and_foreign_processes() {
        let make = |pid, parent, group, exe: &str| Process {
            pid,
            parent,
            group: Some(group),
            birth: "birth".into(),
            started: 1.,
            command: "spoofed\n999 1 999 sh".into(),
            executable: exe.into(),
        };
        let rows: BTreeMap<_, _> = [
            make(10, 1, 10, "/bin/agent"),
            make(20, 10, 20, "/bin/sh"),
            make(21, 20, 21, "/bin/child"),
            make(30, 10, 10, "/bin/bash"),
            make(40, 10, 40, "/opt/codex-code-mode-host"),
            make(41, 40, 41, "/bin/sh"),
            make(50, 10, 10, "/bin/node"),
            make(60, 1, 60, "/bin/sh"),
        ]
        .into_iter()
        .map(|p| (p.pid, p))
        .collect();
        assert_eq!(roots(&json!({"pid":10}), &rows), vec![20, 30, 41]);
        let record = json!({"run_id":"run","conversation_id":"conversation"});
        let original = job(&record, &rows[&20], &rows)["id"].clone();
        let mut reused = rows[&20].clone();
        reused.birth = "new-birth".into();
        assert_ne!(original, job(&record, &reused, &rows)["id"]);
    }
}
