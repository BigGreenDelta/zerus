//! Codex can defer SessionStart until the first turn after native resume or fork.
//! Require an owned rollout and an empty native composer before that turn.
//! This never fabricates a hook or confirms a run.
use super::*;
use std::fs;
use std::io::{BufRead, BufReader};

pub(super) fn fork_pending(record: &Value) -> bool {
    record["agent"] == "codex"
        && record["startup_kind"] == "new"
        && uuid::Uuid::parse_str(string(record, "fork_parent_id")).is_ok()
        && recipes::argv(record).is_ok_and(|args| {
            let program = crate::project_launcher::program_words(&args);
            args.get(program).is_some_and(|arg| arg == "fork")
                && args.get(program + 1).map(|id| json!(id)).as_ref() == record.get("fork_parent_id")
        })
        && string(record, "conversation_id").is_empty()
        && string(record, "expected_id").is_empty()
        && string(record, "requested_id").is_empty()
}

/// The answer a Codex launch check gives when the open files cannot be listed.
fn open_files_error(error: crate::platform::RunError) -> String {
    use crate::platform::RunError;
    match error {
        RunError::Spawn(error) | RunError::Io(error) => error.to_string(),
        RunError::Stopped => "timed out verifying agent history".into(),
        RunError::TooLarge | RunError::Failed | RunError::Unsupported => {
            "Cannot verify the Codex process's open history files".into()
        }
    }
}

fn agent_pid(pid: u64) -> Result<u32> {
    u32::try_from(pid).map_err(|_| "The tracked agent process is not the supervised Codex".into())
}

fn history_meta(path: &Path) -> Result<Value> {
    let mut line = String::new();
    BufReader::new(fs::File::open(path).map_err(|e| e.to_string())?)
        .take(256 * 1024)
        .read_line(&mut line)
        .map_err(|e| e.to_string())?;
    serde_json::from_str(&line).map_err(|_| "Cannot verify native conversation history".into())
}

fn fork_directory(record: &Value) -> Result<PathBuf> {
    let launch = Path::new(string(record, "launch_dir"));
    let mut cwd = launch.to_path_buf();
    let args = recipes::argv(record)?;
    // After the program and `fork <parent>`.
    let mut index = crate::project_launcher::program_words(&args) + 2;
    while index < args.len() && args[index] != "--" {
        let (key, value) = args[index]
            .split_once('=')
            .map_or((args[index].as_str(), None), |(key, value)| {
                (key, Some(value))
            });
        if ["-C", "--cd"].contains(&key) {
            let folder = if let Some(value) = value {
                value
            } else {
                index += 1;
                args.get(index)
                    .ok_or("The fork's directory option is missing its value")?
            };
            cwd = launch.join(folder);
        }
        index += 1;
    }
    crate::platform::canonical(cwd)
        .map_err(|_| "The fork's working directory is unavailable".into())
}

pub(super) fn pending(record: &Value) -> bool {
    record["agent"] == "codex"
        && record["run_identity_version"] == 1
        && record["startup_kind"] == "resume"
        && record["supervisor"].is_object()
        && !string(record, "conversation_id").is_empty()
        && record["expected_id"] == record["conversation_id"]
        && string(record, "requested_id").is_empty()
        && string(record, "error").is_empty()
        && record.get("session_end").is_none()
        && record["last_event_at"].as_f64().unwrap_or(0.0) == 0.0
        && ["", "unknown"].contains(&string(record, "activity"))
        && ["", "unknown"].contains(&string(record, "phase"))
}

fn owns_rollout(record: &Value) -> Result<()> {
    let path = crate::platform::canonical(string(record, "transcript"))
        .map_err(|_| "Saved conversation history is unavailable")?;
    if !path.is_file() {
        return Err("Saved conversation history is not a file".into());
    }
    let meta = history_meta(&path)?;
    if meta["type"] != "session_meta" || meta["payload"]["id"] != record["expected_id"] {
        return Err("Saved conversation history has a different identity".into());
    }
    let pid = record["pid"].as_u64().ok_or("Missing agent process")?;
    if !crate::platform::holds_open(agent_pid(pid)?, &path).map_err(open_files_error)? {
        return Err("Codex has not opened the saved conversation yet".into());
    }
    Ok(())
}

fn checked_process(record: &Value) -> Result<u64> {
    if !process_alive(record) || !matches(record, live()?.get(string(record, "name"))) {
        return Err("The tracked agent is no longer running".into());
    }
    let supervisor = &record["supervisor"];
    let parent = supervisor["pid"]
        .as_u64()
        .ok_or("Missing session supervisor")?;
    if parent > u32::MAX as u64
        || string(supervisor, "start").is_empty()
        || process_start(parent as u32) != string(supervisor, "start")
    {
        return Err("The tracked session supervisor has changed".into());
    }
    let pid = record["pid"].as_u64().ok_or("Missing agent process")?;
    let (actual_parent, name) =
        crate::platform::parent_and_name(agent_pid(pid)?).map_err(|e| e.to_string())?;
    if actual_parent.map(u64::from) != Some(parent)
        || name
            .and_then(|name| crate::platform::program_name(Path::new(&name)))
            .is_none_or(|name| name != "codex")
    {
        return Err("The tracked agent process is not the supervised Codex".into());
    }
    Ok(pid)
}

#[cfg(any(windows, test))]
#[derive(Clone, Copy)]
struct ForkScanLimits {
    entries: usize,
    depth: usize,
    bytes: usize,
    elapsed: std::time::Duration,
}

#[cfg(any(windows, test))]
const FORK_SCAN_LIMITS: ForkScanLimits = ForkScanLimits {
    entries: 16_384,
    depth: 8,
    bytes: 16 * 1024 * 1024,
    elapsed: std::time::Duration::from_secs(2),
};

#[cfg(any(windows, test))]
fn history_link(metadata: &fs::Metadata) -> bool {
    #[cfg(windows)]
    {
        use std::os::windows::fs::MetadataExt;
        use windows_sys::Win32::Storage::FileSystem::FILE_ATTRIBUTE_REPARSE_POINT;
        if metadata.file_attributes() & FILE_ATTRIBUTE_REPARSE_POINT != 0 {
            return true;
        }
    }
    metadata.file_type().is_symlink()
}

#[cfg(any(windows, test))]
fn history_directory(path: &Path) -> Result<fs::File> {
    let before = fs::symlink_metadata(path).map_err(|_| "Cannot inspect fork history directory")?;
    if history_link(&before) || !before.is_dir() {
        return Err("Fork history directory is not a plain directory".into());
    }
    let mut options = fs::OpenOptions::new();
    options.read(true);
    crate::platform::refuse_link(&mut options);
    #[cfg(windows)]
    {
        use std::os::windows::fs::OpenOptionsExt;
        use windows_sys::Win32::Storage::FileSystem::{
            FILE_FLAG_BACKUP_SEMANTICS, FILE_FLAG_OPEN_REPARSE_POINT, FILE_SHARE_READ,
            FILE_SHARE_WRITE,
        };
        // Keep each account/ancestor directory open without delete sharing while
        // walking it. A final link/junction is opened itself and then refused.
        options
            .custom_flags(FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT)
            .share_mode(FILE_SHARE_READ | FILE_SHARE_WRITE);
    }
    let file = options
        .open(path)
        .map_err(|_| "Cannot open fork history directory")?;
    let metadata = file
        .metadata()
        .map_err(|_| "Cannot verify fork history directory")?;
    if history_link(&metadata) || !metadata.is_dir() {
        return Err("Fork history directory changed or is a link".into());
    }
    Ok(file)
}

#[cfg(any(windows, test))]
struct ForkScan<'a, H, E> {
    record: &'a Value,
    root: PathBuf,
    cwd: &'a Path,
    pid: u32,
    limits: ForkScanLimits,
    entries: usize,
    bytes: usize,
    children: std::collections::BTreeSet<(PathBuf, String)>,
    holds: H,
    elapsed: E,
}

#[cfg(any(windows, test))]
impl<H, E> ForkScan<'_, H, E>
where
    H: FnMut(u32, &Path) -> std::result::Result<bool, crate::platform::RunError>,
    E: FnMut() -> std::time::Duration,
{
    fn within_end(&mut self) -> Result<()> {
        if (self.elapsed)() >= self.limits.elapsed {
            return Err("Fork history verification time limit exhausted".into());
        }
        Ok(())
    }

    fn visit(&mut self, folder: &Path, depth: usize) -> Result<()> {
        self.within_end()?;
        if depth > self.limits.depth {
            return Err("Fork history directory depth limit exhausted".into());
        }
        let _directory = history_directory(folder)?;
        let entries =
            fs::read_dir(folder).map_err(|_| "Cannot enumerate fork history directory")?;
        self.within_end()?;
        for entry in entries {
            self.within_end()?;
            self.entries += 1;
            if self.entries > self.limits.entries {
                return Err("Fork history entry limit exhausted".into());
            }
            let entry = entry.map_err(|_| "Incomplete fork history directory enumeration")?;
            let path = entry.path();
            let metadata =
                fs::symlink_metadata(&path).map_err(|_| "Cannot inspect fork history entry")?;
            if history_link(&metadata) {
                return Err("Fork history contains a link or reparse point".into());
            }
            let path = crate::platform::canonical(&path)
                .map_err(|_| "Cannot resolve fork history entry")?;
            if !path.starts_with(&self.root) {
                return Err("Fork history entry is outside the selected account".into());
            }
            if metadata.is_dir() {
                self.visit(&path, depth + 1)?;
            } else if metadata.is_file() {
                if path
                    .file_name()
                    .is_some_and(|name| name.to_string_lossy().starts_with("rollout-"))
                    && path
                        .extension()
                        .is_some_and(|extension| extension == "jsonl")
                {
                    self.rollout(&path)?;
                }
            } else {
                return Err("Fork history contains an unknown entry type".into());
            }
        }
        self.within_end()
    }

    fn rollout(&mut self, path: &Path) -> Result<()> {
        self.within_end()?;
        if !path.starts_with(&self.root) {
            return Err("Fork history candidate is outside the selected account".into());
        }
        let entry =
            fs::symlink_metadata(path).map_err(|_| "Cannot inspect fork history candidate")?;
        if history_link(&entry) || !entry.is_file() {
            return Err("Fork history candidate is not a plain file".into());
        }
        let canonical = crate::platform::canonical(path)
            .map_err(|_| "Cannot resolve fork history candidate")?;
        if !canonical.starts_with(&self.root) {
            return Err("Fork history candidate escaped the selected account".into());
        }
        let path = canonical.as_path();
        let mut options = fs::OpenOptions::new();
        options.read(true);
        crate::platform::refuse_link(&mut options);
        crate::platform::never_block(&mut options);
        #[cfg(windows)]
        {
            use std::os::windows::fs::OpenOptionsExt;
            use windows_sys::Win32::Storage::FileSystem::{FILE_SHARE_READ, FILE_SHARE_WRITE};
            options.share_mode(FILE_SHARE_READ | FILE_SHARE_WRITE);
        }
        let file = options
            .open(path)
            .map_err(|_| "Cannot read fork history header")?;
        let metadata = file
            .metadata()
            .map_err(|_| "Cannot verify fork history file")?;
        if !metadata.is_file() || history_link(&metadata) {
            return Err("Fork history changed or is not a plain file".into());
        }
        let identity = crate::platform::FileIdentity::of(&file)
            .map_err(|_| "Cannot identify fork history file")?;
        let same_file = |path: &Path| {
            crate::platform::file_identity(path).is_some_and(|other| {
                (other.device, other.inode) == (identity.device, identity.inode)
            })
        };
        if !same_file(path) {
            return Err("Fork history file changed before verification".into());
        }
        let remaining = self.limits.bytes.saturating_sub(self.bytes);
        if remaining == 0 {
            return Err("Fork history header byte limit exhausted".into());
        }
        let maximum = remaining.min(256 * 1024);
        let mut line = Vec::new();
        BufReader::new(&file)
            .take((maximum + 1) as u64)
            .read_until(b'\n', &mut line)
            .map_err(|_| "Cannot read fork history header")?;
        if line.len() > maximum {
            return Err("Fork history header byte limit exhausted".into());
        }
        self.bytes += line.len();
        self.within_end()?;
        let meta: Value =
            serde_json::from_slice(&line).map_err(|_| "Cannot verify fork history header")?;
        let id = string(&meta["payload"], "id");
        if meta["type"] == "session_meta"
            && uuid::Uuid::parse_str(id).is_ok()
            && id != string(self.record, "fork_parent_id")
            && meta["payload"]["forked_from_id"] == self.record["fork_parent_id"]
            && meta["payload"]["source"] == "cli"
            && crate::platform::canonical(string(&meta["payload"], "cwd"))
                .is_ok_and(|path| path == self.cwd)
        {
            self.within_end()?;
            let owned = (self.holds)(self.pid, path).map_err(open_files_error)?;
            self.within_end()?;
            if !same_file(path) {
                return Err("Fork history file changed during ownership verification".into());
            }
            if owned {
                self.children.insert((path.to_owned(), id.to_owned()));
                if self.children.len() > 1 {
                    return Err("Codex has opened multiple verified fork histories".into());
                }
            }
        }
        Ok(())
    }
}

/// Windows can verify a known file's holder, but cannot list the process's
/// files. A complete bounded account-only scan supplies candidates, never proof.
#[cfg(any(windows, test))]
fn windows_fork_children<H, E>(
    record: &Value,
    pid: u32,
    sessions: &Path,
    cwd: &Path,
    holds: H,
    elapsed: E,
    limits: ForkScanLimits,
) -> Result<std::collections::BTreeSet<(PathBuf, String)>>
where
    H: FnMut(u32, &Path) -> std::result::Result<bool, crate::platform::RunError>,
    E: FnMut() -> std::time::Duration,
{
    let _account = history_directory(sessions.parent().ok_or("Missing fork history account")?)?;
    let _sessions = history_directory(sessions)?;
    let root = crate::platform::canonical(sessions)
        .map_err(|_| "Forked conversation history is unavailable")?;
    let mut scan = ForkScan {
        record,
        root: root.clone(),
        cwd,
        pid,
        limits,
        entries: 0,
        bytes: 0,
        children: std::collections::BTreeSet::new(),
        holds,
        elapsed,
    };
    complete_fork_scan(&mut scan, |scan| scan.visit(&root, 0))
}

#[cfg(any(windows, test))]
fn complete_fork_scan<'a, H, E>(
    scan: &mut ForkScan<'a, H, E>,
    discover: impl FnOnce(&mut ForkScan<'a, H, E>) -> Result<()>,
) -> Result<std::collections::BTreeSet<(PathBuf, String)>>
where
    H: FnMut(u32, &Path) -> std::result::Result<bool, crate::platform::RunError>,
    E: FnMut() -> std::time::Duration,
{
    // A matching held candidate never turns a partial/error enumeration into
    // proof that this process owns exactly one eligible child.
    discover(scan)?;
    scan.within_end()?;
    if scan.children.len() != 1 {
        return Err("Codex has not opened a single verified fork of this conversation yet".into());
    }
    Ok(std::mem::take(&mut scan.children))
}

/// A fork has no requested child ID yet. The native process must own exactly
/// one interactive rollout whose header links a distinct ID to the saved parent.
pub(super) fn fork_ready(record: &Value) -> Result<()> {
    if !fork_pending(record) || !input::first_message_candidate(record) || pause_active(record) {
        return Err("Waiting for the agent to open the forked conversation".into());
    }
    let pid = checked_process(record)?;
    let agent_home = if string(record, "agent_home").is_empty() {
        home().join(".codex")
    } else {
        PathBuf::from(string(record, "agent_home"))
    };
    #[cfg(windows)]
    let sessions = agent_home.join("sessions");
    #[cfg(not(windows))]
    let sessions = crate::platform::canonical(agent_home.join("sessions"))
        .map_err(|_| "Forked conversation history is unavailable")?;
    let cwd = fork_directory(record)?;
    #[cfg(windows)]
    let children = {
        let started = std::time::Instant::now();
        windows_fork_children(
            record,
            agent_pid(pid)?,
            &sessions,
            &cwd,
            crate::platform::holds_open,
            || started.elapsed(),
            FORK_SCAN_LIMITS,
        )?
    };
    #[cfg(not(windows))]
    let mut children = std::collections::BTreeSet::new();
    #[cfg(not(windows))]
    for path in crate::platform::open_files(agent_pid(pid)?).map_err(open_files_error)? {
        if !path
            .file_name()
            .is_some_and(|name| name.to_string_lossy().starts_with("rollout-"))
            || path
                .extension()
                .is_none_or(|extension| extension != "jsonl")
        {
            continue;
        }
        let Ok(path) = crate::platform::canonical(&path) else {
            continue;
        };
        if !path.starts_with(&sessions) {
            continue;
        }
        let Ok(meta) = history_meta(&path) else {
            continue;
        };
        let id = string(&meta["payload"], "id");
        if meta["type"] == "session_meta"
            && uuid::Uuid::parse_str(id).is_ok()
            && id != string(record, "fork_parent_id")
            && meta["payload"]["forked_from_id"] == record["fork_parent_id"]
            && meta["payload"]["source"] == "cli"
            && crate::platform::canonical(string(&meta["payload"], "cwd"))
                .is_ok_and(|path| path == cwd)
        {
            children.insert((path, id.to_owned()));
        }
    }
    if children.len() != 1 {
        return Err("Codex has not opened a single verified fork of this conversation yet".into());
    }
    input::checked_terminal(record, true)?;
    if !process_alive(record) {
        return Err("The forked agent process has changed".into());
    }
    Ok(())
}

/// The exact supervised Codex holds the expected conversation's rollout open.
pub(super) fn owned(record: &Value) -> Result<()> {
    checked_process(record)?;
    owns_rollout(record)
}

fn verified(record: &Value) -> Result<()> {
    owned(record)?;
    input::checked_terminal(record, true)?;
    if !process_alive(record) {
        return Err("The resumed agent process has changed".into());
    }
    Ok(())
}

pub(super) fn ready(record: &Value) -> Result<()> {
    if !pending(record) || pause_active(record) {
        return Err("Waiting for the agent to confirm this conversation".into());
    }
    if record["input_pending_at"].as_f64().unwrap_or(0.0) > 0.0 {
        return Err("Message sent. Waiting for the agent to confirm this conversation.".into());
    }
    verified(record)
}

/// A pending resume that may also clear or pause before its deferred
/// SessionStart once `idle` verifies it. A sent message may already have
/// started a turn, and an archive restore commits only on the hook that names
/// its exact conversation (archive::confirm_restore).
pub(super) fn clearable(record: &Value) -> bool {
    pending(record)
        && string(record, "restore_archive_id").is_empty()
        && record["input_pending_at"].as_f64().unwrap_or(0.0) == 0.0
}

/// A clearable resume whose exact process waits at the restored conversation's
/// empty native composer, as an idle confirmed session does.
pub(super) fn idle(record: &Value) -> bool {
    clearable(record) && verified(record).is_ok()
}

/// One summary's verification of a live pending resume, or None. Its message,
/// pause and clear availability all reuse it, so they agree and the process,
/// history and composer are each checked once.
pub(super) fn summary_check(record: &Value, live: bool) -> Option<Result<()>> {
    (live && pending(record)).then(|| ready(record))
}

/// `idle` for a record whose `summary_check` passed, without verifying again.
pub(super) fn idle_checked(record: &Value, verified: bool) -> bool {
    verified && clearable(record)
}

pub(super) fn enrich(output: &mut Value, check: &Option<Result<()>>) {
    let Some(check) = check else {
        return;
    };
    let reason = check.clone().err().unwrap_or_default();
    output["resume_message_can_send"] = json!(reason.is_empty());
    output["resume_message_reason"] = json!(reason);
    if reason.is_empty() {
        output["activity"] = json!("idle");
        output["phase"] = json!("idle");
        output["activity_summary"] = json!("Waiting for your next message");
    } else {
        output["phase"] = json!("starting");
        output["activity_summary"] = json!("Restoring conversation");
        output["activity_detail"] = json!(reason);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn resumed() -> Value {
        json!({"agent":"codex","run_identity_version":1,"startup_kind":"resume",
            "supervisor":{"pid":1,"start":"supervisor"},"run_id":"run","conversation_id":"saved",
            "expected_id":"saved","activity":"unknown","phase":"unknown","last_event_at":0})
    }

    #[test]
    fn only_a_resume_without_a_sent_message_or_archive_restore_may_clear_or_pause() {
        assert!(clearable(&resumed()));
        for (key, value) in [("input_pending_at", json!(1.0)), ("restore_archive_id", json!("archive")),
            ("expected_id", json!("other")), ("requested_id", json!("saved")), ("startup_kind", json!("new")),
            ("error", json!("failed")), ("last_event_at", json!(1.0)), ("activity", json!("idle")),
            ("agent", json!("claude"))] {
            let mut other = resumed();
            other[key] = value;
            assert!(!clearable(&other), "{key}");
        }
        // Without the exact live process nothing is verified, and no other
        // state makes a resume idle.
        assert!(!idle(&resumed()));
        assert!(!lifecycle::safe_to_pause(&resumed()));
        let mut confirmed = resumed();
        confirmed.as_object_mut().unwrap().remove("expected_id");
        confirmed["activity"] = json!("idle");
        assert!(lifecycle::safe_to_pause(&confirmed));
        confirmed["active_tools"] = json!({"tool": {}});
        assert!(!lifecycle::safe_to_pause(&confirmed));
    }

    const FORK_PARENT: &str = "11111111-1111-4111-8111-111111111111";
    const FORK_CHILD: &str = "22222222-2222-4222-8222-222222222222";

    /// A fork's `fork <parent>` follows its program, also when a `.ps1`
    /// project launcher spreads that over a PowerShell command line.
    #[test]
    fn a_pending_codex_fork_is_recognised_after_any_project_launcher() {
        let temporary = tempfile::tempdir().unwrap();
        let launch = crate::platform::canonical(temporary.path()).unwrap();
        fs::create_dir(launch.join("sub")).unwrap();
        let pwsh = ["pwsh", "-NoLogo", "-NoProfile", "-File", "/p/run_codex.ps1"];
        for program in [&["codex"][..], &["/p/run_codex.sh"][..], &pwsh[..]] {
            let base = |args: &[&'static str]| [program, args].concat();
            let record = |args: &[&'static str]| json!({"agent":"codex","startup_kind":"new",
                "fork_parent_id":FORK_PARENT,"base":base(args),"launch_dir":launch});
            assert!(fork_pending(&record(&["fork", FORK_PARENT, "--no-daemon"])), "{program:?}");
            assert!(!fork_pending(&record(&["resume", FORK_PARENT])), "{program:?}");
            assert!(!fork_pending(&record(&["fork", FORK_CHILD])), "{program:?}");
            assert_eq!(fork_directory(&record(&["fork", FORK_PARENT, "-C", "sub"])).unwrap(),
                launch.join("sub"), "{program:?}");
        }
    }

    struct ForkHistory {
        temporary: tempfile::TempDir,
        record: Value,
        sessions: PathBuf,
        cwd: PathBuf,
        rollout: PathBuf,
    }

    impl ForkHistory {
        fn new() -> Self {
            let temporary = tempfile::tempdir().unwrap();
            let sessions = temporary.path().join("account/sessions");
            let cwd = temporary.path().join("workspace");
            fs::create_dir_all(&sessions).unwrap();
            fs::create_dir(&cwd).unwrap();
            let sessions = crate::platform::canonical(sessions).unwrap();
            let rollout = sessions.join("rollout-child.jsonl");
            let value = Self {
                temporary,
                record: json!({"fork_parent_id":FORK_PARENT}),
                sessions,
                cwd: crate::platform::canonical(cwd).unwrap(),
                rollout,
            };
            value.write_header(&value.rollout, &value.header());
            value
        }

        fn header(&self) -> Value {
            json!({"type":"session_meta","payload":{"id":FORK_CHILD,
                "forked_from_id":FORK_PARENT,"source":"cli","cwd":self.cwd}})
        }

        fn write_header(&self, path: &Path, header: &Value) {
            fs::write(path, format!("{header}\n")).unwrap();
        }

        fn prove<H>(
            &self,
            holds: H,
            limits: ForkScanLimits,
        ) -> Result<std::collections::BTreeSet<(PathBuf, String)>>
        where
            H: FnMut(u32, &Path) -> std::result::Result<bool, crate::platform::RunError>,
        {
            windows_fork_children(
                &self.record,
                42,
                &self.sessions,
                &self.cwd,
                holds,
                || std::time::Duration::ZERO,
                limits,
            )
        }
    }

    #[test]
    fn windows_fork_unsupported_enumeration_needs_positive_exact_file_ownership() {
        let fixture = ForkHistory::new();
        // This is the old production Windows discovery operation's result and
        // its actual error mapper, not a metadata-only substitute for ownership.
        let old: std::result::Result<Vec<PathBuf>, crate::platform::RunError> =
            Err(crate::platform::RunError::Unsupported);
        assert_eq!(
            old.map_err(open_files_error).unwrap_err(),
            "Cannot verify the Codex process's open history files"
        );
        let mut observed = Vec::new();
        let child = fixture
            .prove(
                |pid, path| {
                    observed.push((pid, path.to_owned()));
                    Ok(true)
                },
                FORK_SCAN_LIMITS,
            )
            .unwrap();
        assert_eq!(
            observed,
            [(42, crate::platform::canonical(&fixture.rollout).unwrap())]
        );
        assert_eq!(
            child,
            std::collections::BTreeSet::from([(
                crate::platform::canonical(&fixture.rollout).unwrap(),
                FORK_CHILD.into()
            )])
        );
        assert!(fixture.prove(|_, _| Ok(false), FORK_SCAN_LIMITS).is_err());
        assert!(fixture
            .prove(
                |_, _| Err(crate::platform::RunError::Unsupported),
                FORK_SCAN_LIMITS
            )
            .is_err());
        assert!(fixture
            .prove(
                |_, _| Err(crate::platform::RunError::Io(io::Error::other(
                    "synthetic ownership error"
                ))),
                FORK_SCAN_LIMITS
            )
            .is_err());
    }

    #[test]
    fn windows_fork_never_uses_an_unopened_neighbor_or_ambiguous_held_children() {
        let fixture = ForkHistory::new();
        let neighbor = fixture.sessions.join("rollout-neighbor.jsonl");
        let mut second = fixture.header();
        second["payload"]["id"] = json!("33333333-3333-4333-8333-333333333333");
        fixture.write_header(&neighbor, &second);
        assert!(fixture.prove(|_, _| Ok(true), FORK_SCAN_LIMITS).is_err());
        let child = fixture
            .prove(
                |_, path| Ok(path == crate::platform::canonical(&fixture.rollout).unwrap()),
                FORK_SCAN_LIMITS,
            )
            .unwrap();
        assert_eq!(child.len(), 1);
        assert_eq!(child.first().unwrap().1, FORK_CHILD);
        // Neither a newer neighbor nor the saved parent is ownership evidence.
        let mut parent = fixture.header();
        parent["payload"]["id"] = json!(FORK_PARENT);
        fixture.write_header(&fixture.rollout, &parent);
        assert!(fixture
            .prove(
                |_, path| Ok(path != crate::platform::canonical(&neighbor).unwrap()),
                FORK_SCAN_LIMITS
            )
            .is_err());
    }

    #[test]
    fn windows_fork_rejects_wrong_header_parent_source_cwd_and_non_child_ids() {
        let fixture = ForkHistory::new();
        for (key, value) in [
            ("id", json!(FORK_PARENT)),
            ("id", json!("not-a-uuid")),
            ("forked_from_id", json!(FORK_CHILD)),
            ("source", json!("exec")),
            ("cwd", json!(fixture.sessions)),
        ] {
            let mut header = fixture.header();
            header["payload"][key] = value;
            fixture.write_header(&fixture.rollout, &header);
            let mut queried = false;
            assert!(fixture
                .prove(
                    |_, _| {
                        queried = true;
                        Ok(true)
                    },
                    FORK_SCAN_LIMITS
                )
                .is_err());
            assert!(!queried, "invalid header reached ownership lookup: {key}");
        }
    }

    #[test]
    fn windows_fork_complete_discovery_is_required_even_after_a_held_candidate() {
        let fixture = ForkHistory::new();
        let mut scan = ForkScan {
            record: &fixture.record,
            root: crate::platform::canonical(&fixture.sessions).unwrap(),
            cwd: &fixture.cwd,
            pid: 42,
            limits: FORK_SCAN_LIMITS,
            entries: 0,
            bytes: 0,
            children: std::collections::BTreeSet::new(),
            holds: |_, _: &Path| Ok(true),
            elapsed: || std::time::Duration::ZERO,
        };
        let result = complete_fork_scan(&mut scan, |scan| {
            scan.rollout(&fixture.rollout)?;
            assert_eq!(scan.children.len(), 1);
            Err("synthetic partial enumeration".into())
        });
        assert_eq!(result.unwrap_err(), "synthetic partial enumeration");
        // The production completion seam also refuses a genuine missing/read
        // error after a first candidate has already passed ownership.
        assert!(complete_fork_scan(&mut scan, |scan| {
            scan.rollout(&fixture.sessions.join("rollout-missing.jsonl"))
        })
        .is_err());
    }

    #[test]
    fn windows_fork_injected_foreign_account_candidates_never_reach_ownership() {
        let fixture = ForkHistory::new();
        let foreign = fixture
            .temporary
            .path()
            .join("other-account/rollout-foreign.jsonl");
        fs::create_dir(foreign.parent().unwrap()).unwrap();
        fixture.write_header(&foreign, &fixture.header());
        let mut queried = false;
        let mut scan = ForkScan {
            record: &fixture.record,
            root: crate::platform::canonical(&fixture.sessions).unwrap(),
            cwd: &fixture.cwd,
            pid: 42,
            limits: FORK_SCAN_LIMITS,
            entries: 0,
            bytes: 0,
            children: std::collections::BTreeSet::new(),
            holds: |_, _: &Path| {
                queried = true;
                Ok(true)
            },
            elapsed: || std::time::Duration::ZERO,
        };
        assert!(complete_fork_scan(&mut scan, |scan| scan.rollout(&foreign)).is_err());
        assert!(!queried);
    }

    #[test]
    fn windows_fork_refuses_entry_depth_and_total_or_individual_header_exhaustion() {
        let fixture = ForkHistory::new();
        let exact = fs::read(&fixture.rollout).unwrap().len();
        assert!(fixture
            .prove(
                |_, _| Ok(true),
                ForkScanLimits {
                    entries: 0,
                    ..FORK_SCAN_LIMITS
                }
            )
            .is_err());
        assert!(fixture
            .prove(
                |_, _| Ok(true),
                ForkScanLimits {
                    bytes: exact - 1,
                    ..FORK_SCAN_LIMITS
                }
            )
            .is_err());
        assert!(fixture
            .prove(
                |_, _| Ok(true),
                ForkScanLimits {
                    bytes: exact,
                    ..FORK_SCAN_LIMITS
                }
            )
            .is_ok());
        let neighbor = fixture.sessions.join("rollout-neighbor.jsonl");
        fixture.write_header(&neighbor, &fixture.header());
        assert!(fixture
            .prove(
                |_, path| Ok(path == crate::platform::canonical(&fixture.rollout).unwrap()),
                ForkScanLimits {
                    bytes: exact,
                    ..FORK_SCAN_LIMITS
                }
            )
            .is_err());
        assert!(fixture
            .prove(
                |_, path| Ok(path == crate::platform::canonical(&fixture.rollout).unwrap()),
                ForkScanLimits {
                    bytes: exact * 2,
                    ..FORK_SCAN_LIMITS
                }
            )
            .is_ok());
        fs::remove_file(neighbor).unwrap();
        let nested = fixture.sessions.join("nested");
        fs::create_dir(&nested).unwrap();
        assert!(fixture
            .prove(
                |_, _| Ok(true),
                ForkScanLimits {
                    depth: 0,
                    ..FORK_SCAN_LIMITS
                }
            )
            .is_err());
        fs::remove_dir(nested).unwrap();
        let mut oversized = fixture.header();
        oversized["payload"]["padding"] = json!("x".repeat(256 * 1024));
        fixture.write_header(&fixture.rollout, &oversized);
        assert!(fixture.prove(|_, _| Ok(true), FORK_SCAN_LIMITS).is_err());
    }

    #[test]
    fn windows_fork_unknown_headers_and_missing_account_root_never_admit() {
        let fixture = ForkHistory::new();
        for raw in [
            b"".as_slice(),
            b"not json\n".as_slice(),
            b"{\"type\":\"session_meta\"}\n".as_slice(),
        ] {
            fs::write(&fixture.rollout, raw).unwrap();
            assert!(fixture.prove(|_, _| Ok(true), FORK_SCAN_LIMITS).is_err());
        }
        fs::remove_file(&fixture.rollout).unwrap();
        fs::remove_dir(&fixture.sessions).unwrap();
        assert!(fixture.prove(|_, _| Ok(true), FORK_SCAN_LIMITS).is_err());
    }

    #[test]
    fn windows_fork_original_time_limit_cannot_accept_a_late_ownership_result() {
        let fixture = ForkHistory::new();
        let late = std::cell::Cell::new(false);
        let result = windows_fork_children(
            &fixture.record,
            42,
            &fixture.sessions,
            &fixture.cwd,
            |_, _| {
                late.set(true);
                Ok(true)
            },
            || {
                if late.get() {
                    FORK_SCAN_LIMITS.elapsed
                } else {
                    std::time::Duration::ZERO
                }
            },
            FORK_SCAN_LIMITS,
        );
        assert!(result.is_err());
        let mut queried = false;
        let result = windows_fork_children(
            &fixture.record,
            42,
            &fixture.sessions,
            &fixture.cwd,
            |_, _| {
                queried = true;
                Ok(true)
            },
            || FORK_SCAN_LIMITS.elapsed,
            FORK_SCAN_LIMITS,
        );
        assert!(result.is_err());
        assert!(!queried);
    }

    #[cfg(unix)]
    #[test]
    fn windows_fork_candidate_scan_refuses_symlinks_including_an_unrelated_entry() {
        use std::os::unix::fs::symlink;
        let fixture = ForkHistory::new();
        let alias = fixture.sessions.join("unrelated-link");
        symlink(&fixture.cwd, &alias).unwrap();
        assert!(fixture.prove(|_, _| Ok(true), FORK_SCAN_LIMITS).is_err());
        fs::remove_file(alias).unwrap();
        fs::remove_file(&fixture.rollout).unwrap();
        fs::remove_dir(&fixture.sessions).unwrap();
        symlink(&fixture.cwd, &fixture.sessions).unwrap();
        assert!(fixture.prove(|_, _| Ok(true), FORK_SCAN_LIMITS).is_err());
    }
}
