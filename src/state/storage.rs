use super::*;
use fs2::FileExt;
use sha2::{Digest, Sha256};
use std::fs::{self, File, OpenOptions};
use std::io::Write;
use std::os::unix::fs::{DirBuilderExt, OpenOptionsExt, PermissionsExt};
use std::process::{Command, Output};

pub(super) fn root() -> PathBuf {
    nonempty_env("HGS_STATE_DIR")
        .map(PathBuf::from)
        .unwrap_or_else(|| {
            nonempty_env("XDG_STATE_HOME")
                .map(PathBuf::from)
                .unwrap_or_else(|| home().join(".local/state"))
                .join("hgs/sessions")
        })
}

pub(super) fn private_dir(path: &Path) -> Result<()> {
    fs::DirBuilder::new()
        .recursive(true)
        .mode(0o700)
        .create(path)
        .map_err(|e| e.to_string())
}

pub(super) fn absolute_root() -> Result<PathBuf> {
    private_dir(&root())?;
    root().canonicalize().map_err(|e| e.to_string())
}

pub(super) fn atomic(path: &Path, contents: &str) -> Result<()> {
    let parent = path.parent().ok_or("cannot write file without parent")?;
    private_dir(parent)?;
    let mut temp = tempfile::Builder::new()
        .prefix(".hgs-")
        .tempfile_in(parent)
        .map_err(|e| e.to_string())?;
    temp.write_all(contents.as_bytes())
        .map_err(|e| e.to_string())?;
    temp.as_file().sync_all().map_err(|e| e.to_string())?;
    temp.persist(path).map_err(|e| e.to_string())?;
    File::open(parent)
        .and_then(|file| file.sync_all())
        .map_err(|e| e.to_string())
}

pub(super) struct Guard(File);
impl Drop for Guard {
    fn drop(&mut self) {
        let _ = FileExt::unlock(&self.0);
    }
}

pub(super) fn lock(path: Option<&Path>) -> Result<Guard> {
    let default = root().join(".lock");
    let path = path.unwrap_or(&default);
    private_dir(path.parent().ok_or("invalid lock path")?)?;
    let file = OpenOptions::new()
        .create(true)
        .append(true)
        .mode(0o600)
        .open(path)
        .map_err(|e| e.to_string())?;
    file.lock_exclusive().map_err(|e| e.to_string())?;
    let guard = Guard(file);
    if path == default {
        rename::recover_locked()?;
        rename::sync_legacy_locked()?;
    }
    Ok(guard)
}

/// An exclusive lock on an auxiliary file, or None when another process still
/// holds it at `deadline`. Never use it for the default state lock.
pub(super) fn lock_until(path: &Path, deadline: std::time::Instant) -> Result<Option<Guard>> {
    private_dir(path.parent().ok_or("invalid lock path")?)?;
    let file = OpenOptions::new()
        .create(true)
        .append(true)
        .mode(0o600)
        .open(path)
        .map_err(|e| e.to_string())?;
    loop {
        match file.try_lock_exclusive() {
            Ok(()) => return Ok(Some(Guard(file))),
            Err(error) if error.kind() != io::ErrorKind::WouldBlock => return Err(error.to_string()),
            Err(_) if std::time::Instant::now() >= deadline => return Ok(None),
            Err(_) => std::thread::sleep(std::time::Duration::from_millis(5)),
        }
    }
}

pub(super) fn legacy_record_path(name: &str) -> PathBuf {
    root().join(format!("{:x}.json", Sha256::digest(name.as_bytes())))
}

pub(super) fn route_path(name: &str) -> PathBuf {
    root()
        .join("name_routes")
        .join(legacy_record_path(name).file_name().unwrap())
}

pub(super) fn record_path(name: &str) -> PathBuf {
    let legacy = legacy_record_path(name);
    if route_path(name).exists() {
        root().join("bindings").join(legacy.file_name().unwrap())
    } else {
        legacy
    }
}

pub(super) fn read(name: &str) -> Result<Value> {
    let contents = fs::read_to_string(record_path(name)).map_err(|e| {
        if e.kind() == io::ErrorKind::NotFound {
            format!("{name}: no saved binding; restart through hgs to enable tracking")
        } else {
            e.to_string()
        }
    })?;
    let mut record: Value = serde_json::from_str(&contents)
        .map_err(|e| format!("{name}: invalid saved binding: {e}"))?;
    if !record.is_object()
        || record["version"] != 1
        || string(&record, "name") != name
        || !AGENTS.contains(&string(&record, "agent"))
    {
        return Err(format!("{name}: invalid saved binding"));
    }
    journal::repair_kimi_main(&mut record);
    Ok(record)
}

pub(super) fn write(record: &mut Value) -> Result<()> {
    record["updated"] = json!(now());
    atomic(&record_path(string(record, "name")), &format!("{record}\n"))?;
    rename::mirror_legacy(record)
}

/// Names are mutable; provider hooks and already-running supervisors retain
/// their original environment. The run token is the immutable binding identity.
pub(super) fn read_run(name: &str, run_id: &str) -> Result<Option<Value>> {
    if run_id.is_empty() {
        return Ok(None);
    }
    if record_path(name).exists() {
        let record = read(name)?;
        if string(&record, "run_id") == run_id {
            return Ok(Some(record));
        }
    }
    let mut matching = records()?
        .into_iter()
        .filter(|record| string(record, "run_id") == run_id);
    let record = matching.next();
    if matching.next().is_some() {
        return Err("ambiguous session run identity".into());
    }
    Ok(record)
}

/// Bindings are named by `legacy_record_path`; other JSON files in the state
/// root, such as desktop presence and read marks, are not session records.
fn is_record_path(path: &Path) -> bool {
    let stem = path.file_stem().and_then(|stem| stem.to_str()).unwrap_or_default();
    path.extension().and_then(|extension| extension.to_str()) == Some("json")
        && stem.len() == 64
        && stem.bytes().all(|byte| matches!(byte, b'0'..=b'9' | b'a'..=b'f'))
}

pub(super) fn records() -> Result<Vec<Value>> {
    if !root().exists() {
        return Ok(Vec::new());
    }
    let mut paths: Vec<PathBuf> = fs::read_dir(root())
        .map_err(|e| e.to_string())?
        .filter_map(|entry| entry.ok().map(|entry| entry.path()))
        .filter(|path| is_record_path(path))
        .collect();
    let bindings = root().join("bindings");
    if bindings.exists() {
        paths.extend(
            fs::read_dir(bindings)
                .map_err(|e| e.to_string())?
                .filter_map(|entry| entry.ok().map(|entry| entry.path()))
                .filter(|path| is_record_path(path)),
        );
    }
    paths.sort();
    let mut result = Vec::new();
    for path in paths {
        let loaded = (|| {
            let value: Value =
                serde_json::from_str(&fs::read_to_string(&path).map_err(|e| e.to_string())?)
                    .map_err(|e| e.to_string())?;
            if value["rename_shadow"] == true {
                return Ok(None);
            }
            read(string(&value, "name")).map(Some)
        })();
        match loaded {
            Ok(Some(record)) => result.push(record),
            Ok(None) => {}
            Err(error) => eprintln!("hgs: cannot read {}: {error}", path.display()),
        }
    }
    Ok(result)
}

pub(super) fn tmux(args: &[String], check: bool) -> Result<Output> {
    let output = Command::new("tmux")
        .args(args)
        .output()
        .map_err(|e| format!("tmux: {e}"))?;
    if check && !output.status.success() {
        let error = String::from_utf8_lossy(&output.stderr).trim().to_owned();
        return Err(if error.is_empty() {
            "tmux command failed".into()
        } else {
            error
        });
    }
    Ok(output)
}

pub(super) type Pane = (String, String, String);
pub(super) type Snapshot = BTreeMap<String, Vec<Pane>>;

pub(super) fn live() -> Result<Snapshot> {
    let output = tmux(
        &[
            "list-panes",
            "-a",
            "-F",
            "#{session_name}\t#{pane_id}\t#{pane_dead}\t#{@hgs_run}",
        ]
        .map(str::to_owned),
        false,
    )?;
    let mut result: Snapshot = BTreeMap::new();
    for line in String::from_utf8_lossy(&output.stdout).lines() {
        let fields: Vec<&str> = line.splitn(4, '\t').collect();
        if fields.len() == 4 {
            result.entry(fields[0].to_owned()).or_default().push((
                fields[1].into(),
                fields[2].into(),
                fields[3].into(),
            ));
        }
    }
    Ok(result)
}

pub(super) fn matches(record: &Value, panes: Option<&Vec<Pane>>) -> bool {
    panes
        .map(|panes| {
            panes.len() == 1
                && panes[0].0 == string(record, "pane")
                && panes[0].1 == "0"
                && panes[0].2 == string(record, "run_id")
                && !panes[0].2.is_empty()
        })
        .unwrap_or(false)
}

pub(super) fn process_start(pid: u32) -> String {
    crate::platform::process_start_time(pid).unwrap_or_else(|| ps_process_start(pid).unwrap_or_default())
}

/// ps's own start time: `Some` with its (possibly empty) output when ps ran successfully.
pub(super) fn ps_process_start(pid: u32) -> Option<String> {
    let output = Command::new("ps")
        .args(["-p", &pid.to_string(), "-o", "lstart="])
        .env("LC_ALL", "C")
        .env("TZ", "UTC")
        .output()
        .ok()?;
    output
        .status
        .success()
        .then(|| String::from_utf8_lossy(&output.stdout).trim().to_owned())
}

pub(super) fn process_alive(record: &Value) -> bool {
    let start = string(record, "process_start");
    let pid = record["pid"]
        .as_u64()
        .and_then(|pid| u32::try_from(pid).ok())
        .unwrap_or(0);
    !start.is_empty() && pid > 0 && process_start(pid) == start
}

pub(super) fn pause_active(record: &Value) -> bool {
    let owner = &record["pausing"];
    let pid = owner["pid"]
        .as_u64()
        .and_then(|pid| u32::try_from(pid).ok())
        .unwrap_or(0);
    !string(owner, "start").is_empty() && process_start(pid) == string(owner, "start")
}

pub(super) fn join_command(args: &[String]) -> String {
    args.iter()
        .map(|arg| shell_words::quote(arg).into_owned())
        .collect::<Vec<_>>()
        .join(" ")
}

pub(super) fn runner_executable() -> Result<String> {
    if let Some(executable) = nonempty_env("HGS_EXECUTABLE") {
        let path = Path::new(&executable);
        if path.is_absolute()
            && path
                .metadata()
                .map(|metadata| metadata.is_file() && metadata.permissions().mode() & 0o111 != 0)
                .unwrap_or(false)
        {
            return Ok(executable);
        }
    }
    Ok(std::env::current_exe()
        .map_err(|error| error.to_string())?
        .to_string_lossy()
        .into_owned())
}
