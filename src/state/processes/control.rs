//! Provider-independent control of verified OS process trees. Native call IDs
//! are never interpreted as PIDs or matched to a process by command text.
use super::*;
use std::process::Command;
use std::time::{Duration, Instant};

#[derive(Clone)]
struct Process {
    pid: u32,
    parent: u32,
    group: u32,
    birth: String,
    started: f64,
    command: String,
    executable: String,
}

#[cfg(target_os = "linux")]
fn identity(pid: u32) -> Option<(u32, u32, String, f64)> {
    use std::os::unix::fs::MetadataExt;
    let path = PathBuf::from(format!("/proc/{pid}"));
    if std::fs::metadata(&path).ok()?.uid() != unsafe { libc::geteuid() } {
        return None;
    }
    let stat = std::fs::read_to_string(path.join("stat")).ok()?;
    let (_, tail) = stat.rsplit_once(')')?;
    let fields: Vec<_> = tail.split_whitespace().collect();
    if fields.len() < 20 || matches!(fields[0], "Z" | "X") {
        return None;
    }
    let started = crate::platform::process_started_at(pid)?? as f64;
    Some((
        fields[1].parse().ok()?,
        fields[2].parse().ok()?,
        fields[19].into(),
        started,
    ))
}
#[cfg(target_os = "macos")]
fn identity(pid: u32) -> Option<(u32, u32, String, f64)> {
    let mut info: libc::proc_bsdinfo = unsafe { std::mem::zeroed() };
    let size = std::mem::size_of_val(&info) as i32;
    let read = unsafe {
        libc::proc_pidinfo(
            pid as i32,
            libc::PROC_PIDTBSDINFO,
            0,
            &mut info as *mut _ as *mut libc::c_void,
            size,
        )
    };
    if read != size
        || info.pbi_pid != pid
        || info.pbi_uid != unsafe { libc::geteuid() }
        || info.pbi_status == 5
    {
        return None;
    }
    Some((
        info.pbi_ppid,
        info.pbi_pgid,
        format!("{}:{}", info.pbi_start_tvsec, info.pbi_start_tvusec),
        info.pbi_start_tvsec as f64 + info.pbi_start_tvusec as f64 / 1_000_000.,
    ))
}
#[cfg(not(any(target_os = "linux", target_os = "macos")))]
fn identity(_: u32) -> Option<(u32, u32, String, f64)> {
    None
}

// Read executable/argv from the kernel, not newline-delimited `ps` command text.
// A command containing newlines must not spoof a different process row or helper.
#[cfg(target_os = "linux")]
fn describe(pid: u32) -> Option<(String, String)> {
    let executable = std::fs::read_link(format!("/proc/{pid}/exe"))
        .ok()?
        .to_string_lossy()
        .into_owned();
    let mut bytes = Vec::new();
    File::open(format!("/proc/{pid}/cmdline"))
        .ok()?
        .take(64 * 1024)
        .read_to_end(&mut bytes)
        .ok()?;
    let command = bytes
        .split(|b| *b == 0)
        .filter(|s| !s.is_empty())
        .map(|s| String::from_utf8_lossy(s))
        .collect::<Vec<_>>()
        .join(" ");
    Some((executable, journal::clipped(&json!(command), 16000)))
}
#[cfg(target_os = "macos")]
fn describe(pid: u32) -> Option<(String, String)> {
    let mut path = vec![0u8; 4096];
    let n = unsafe {
        libc::proc_pidpath(
            pid as i32,
            path.as_mut_ptr() as *mut libc::c_void,
            path.len() as u32,
        )
    };
    if n <= 0 {
        return None;
    }
    let executable =
        String::from_utf8_lossy(&path[..path.iter().position(|b| *b == 0).unwrap_or(path.len())])
            .into_owned();
    let mut mib = [libc::CTL_KERN, libc::KERN_PROCARGS2, pid as i32];
    let mut bytes = vec![0u8; 1024 * 1024];
    let mut size = bytes.len();
    let result = unsafe {
        libc::sysctl(
            mib.as_mut_ptr(),
            3,
            bytes.as_mut_ptr() as *mut libc::c_void,
            &mut size,
            std::ptr::null_mut(),
            0,
        )
    };
    if result != 0 || size < 4 {
        return Some((executable.clone(), executable));
    }
    bytes.truncate(size);
    let argc = i32::from_ne_bytes(bytes[..4].try_into().ok()?)
        .max(0)
        .min(4096) as usize;
    let mut rest = &bytes[4..];
    rest = &rest[rest.iter().position(|b| *b == 0)? + 1..];
    while rest.first() == Some(&0) {
        rest = &rest[1..];
    }
    let command = rest
        .split(|b| *b == 0)
        .take(argc)
        .map(|s| String::from_utf8_lossy(s))
        .collect::<Vec<_>>()
        .join(" ");
    Some((executable, journal::clipped(&json!(command), 16000)))
}
#[cfg(not(any(target_os = "linux", target_os = "macos")))]
fn describe(_: u32) -> Option<(String, String)> {
    None
}

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
// The agent's own helpers (Claude Code's `claude daemon` and the sessions it
// hosts) run the agent's native executable. They and their children are not
// commands the agent started. A script runtime proves nothing (an agent
// installed through npm runs under node), so it never excludes a process.
fn own_helper(pid: u32, root: u32, agent: &Process, rows: &BTreeMap<u32, Process>) -> bool {
    let runtime = Path::new(&agent.executable)
        .file_name()
        .and_then(|s| s.to_str())
        .unwrap_or("");
    if runtime.is_empty()
        || ["node", "bun", "deno"].contains(&runtime)
        || runtime.starts_with("python")
    {
        return false;
    }
    let mut at = pid;
    for _ in 0..128 {
        let Some(p) = rows.get(&at) else {
            return false;
        };
        if at == root {
            return false;
        }
        if p.executable == agent.executable {
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
    let output = Command::new("ps")
        .args(["-axo", "pid=,ppid=,pgid="])
        .output()
        .map_err(|e| e.to_string())?;
    if !output.status.success() {
        return Err("Live process inspection is unavailable".into());
    }
    let mut rows = BTreeMap::new();
    for line in String::from_utf8_lossy(&output.stdout).lines() {
        let numbers: Vec<u32> = line
            .split_whitespace()
            .filter_map(|v| v.parse().ok())
            .collect();
        if numbers.len() != 3 {
            continue;
        }
        rows.insert(
            numbers[0],
            Process {
                pid: numbers[0],
                parent: numbers[1],
                group: numbers[2],
                birth: String::new(),
                started: 0.,
                command: String::new(),
                executable: String::new(),
            },
        );
    }
    Ok(rows)
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
        if let Some((parent, group, birth, started)) = identity(*pid) {
            if parent == p.parent && group == p.group {
                p.birth = birth;
                p.started = started;
                if let Some((executable, command)) = describe(*pid) {
                    p.executable = executable;
                    p.command = command;
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
            let executable = Path::new(&p.executable)
                .file_name()
                .and_then(|s| s.to_str())
                .unwrap_or("");
            if [
                "codex-code-mode",
                "codex-code-mode-host",
                "codex-exec-server",
                "hgs",
                "hgs-tray",
            ]
            .contains(&executable)
                || own_helper(p.pid, root, agent, rows)
            {
                return false;
            }
            (p.group == p.pid && p.group != agent.group)
                || (p.parent == root
                    && ["bash", "zsh", "sh", "dash", "fish", "nu", "pwsh"].contains(&executable))
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

struct Target {
    process: Process,
    #[cfg(target_os = "linux")]
    handle: File,
}
impl Target {
    fn pin(process: Process) -> Result<Self> {
        #[cfg(target_os = "linux")]
        {
            use std::os::fd::FromRawFd;
            let fd = unsafe { libc::syscall(libc::SYS_pidfd_open, process.pid, 0) } as i32;
            if fd < 0 {
                return Err("Process changed or this kernel lacks PID handle support".into());
            }
            let handle = unsafe { File::from_raw_fd(fd) };
            if !identity(process.pid).is_some_and(|p| p.2 == process.birth) {
                return Err("Process identity changed".into());
            }
            Ok(Self { process, handle })
        }
        #[cfg(not(target_os = "linux"))]
        {
            Ok(Self { process })
        }
    }
    fn alive(&self) -> bool {
        identity(self.process.pid).is_some_and(|p| p.2 == self.process.birth)
    }
    fn signal(&self, signal: i32) -> Result<()> {
        if !self.alive() {
            return Ok(());
        }
        #[cfg(target_os = "linux")]
        let result = {
            use std::os::fd::AsRawFd;
            (unsafe {
                libc::syscall(
                    libc::SYS_pidfd_send_signal,
                    self.handle.as_raw_fd(),
                    signal,
                    std::ptr::null::<libc::siginfo_t>(),
                    0,
                )
            }) as i32
        };
        #[cfg(not(target_os = "linux"))]
        let result = unsafe { libc::kill(self.process.pid as i32, signal) };
        if result != 0 && std::io::Error::last_os_error().raw_os_error() != Some(libc::ESRCH) {
            return Err(format!(
                "Could not stop PID {}: {}",
                self.process.pid,
                std::io::Error::last_os_error()
            ));
        }
        Ok(())
    }
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
        .map(Target::pin)
        .collect::<Result<_>>()?;
    drop(guard);
    for target in &targets {
        target.signal(libc::SIGTERM)?;
    }
    let deadline = Instant::now() + Duration::from_secs(2);
    while targets.iter().any(Target::alive) && Instant::now() < deadline {
        std::thread::sleep(Duration::from_millis(40));
    }
    for target in &targets {
        target.signal(libc::SIGKILL)?;
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
            group,
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
            // The agent's own daemon and a session it hosts.
            make(70, 10, 70, "/bin/agent"),
            make(71, 70, 71, "/bin/agent"),
            make(72, 71, 72, "/bin/sh"),
        ]
        .into_iter()
        .map(|p| (p.pid, p))
        .collect();
        assert_eq!(roots(&json!({"pid":10}), &rows), vec![20, 30, 41]);
        // Under a script runtime, a process sharing it is still a command.
        let mut scripted = rows.clone();
        for pid in [10, 70, 71] {
            scripted.get_mut(&pid).unwrap().executable = "/usr/bin/node".into();
        }
        assert_eq!(roots(&json!({"pid":10}), &scripted), vec![20, 30, 41, 70]);
        let record = json!({"run_id":"run","conversation_id":"conversation"});
        let original = job(&record, &rows[&20], &rows)["id"].clone();
        let mut reused = rows[&20].clone();
        reused.birth = "new-birth".into();
        assert_ne!(original, job(&record, &reused, &rows)["id"]);
    }
}
