use super::*;
use sha2::{Digest, Sha256};
use std::fs;

const COMMON: &[&str] = &[
    "SessionStart",
    "SessionEnd",
    "UserPromptSubmit",
    "PreToolUse",
    "PostToolUse",
    "PermissionRequest",
    "SubagentStart",
    "SubagentStop",
    "PreCompact",
    "PostCompact",
    "Stop",
];

fn events(agent: &str) -> Vec<&'static str> {
    let mut events = COMMON.to_vec();
    match agent {
        "claude" => events.extend(["PostToolUseFailure", "StopFailure", "Notification"]),
        "codex" => events.push("Interrupt"),
        "kimi" => events.extend([
            "PostToolUseFailure",
            "StopFailure",
            "Notification",
            "Interrupt",
            "TurnStarted",
            "UserPromptQueued",
            "PermissionResult",
            "TaskStarted",
            "SessionHeartbeat",
        ]),
        _ => {}
    }
    events
}

/// The oldest Claude Code whose `PostModelSwitch` hgs has verified: the exact
/// model after each switch (`from_model`, `to_model`, `requested_model`,
/// `source`). The event name first appears in 2.1.251.
const CLAUDE_MODEL_SWITCH: Version = (2, 1, 296);

/// Events of `agent` that only newer versions have, each with the oldest
/// version hgs registers it for. Claude Code (2.1.200 and 2.1.296 alike) stops
/// every start at a settings warning while `settings.json` names a hook event
/// it does not know, so these are added only for a version read from the
/// agent's native CLI, never for a project launcher or a version that cannot
/// be read, and hgs's entry goes again before an older version starts
/// (`prune_json`). `PreModelSwitch` stays
/// unregistered: its allow would also skip Claude's cache-miss confirmation
/// for the user.
fn versioned_events(agent: &str) -> &'static [(&'static str, Version)] {
    match agent {
        "claude" => &[("PostModelSwitch", CLAUDE_MODEL_SWITCH)],
        _ => &[],
    }
}

fn owns_hook(command: &str) -> bool {
    let Ok(argv) = shell_words::split(command) else {
        return false;
    };
    if argv.len() < 3 || argv.last().map(String::as_str) != Some("hook") {
        return false;
    }
    if Path::new(&argv[argv.len() - 2])
        .file_name()
        .and_then(|name| name.to_str())
        == Some("hgs_state.py")
    {
        return true;
    }
    argv[argv.len() - 2] == "__state"
        && Path::new(&argv[argv.len() - 3])
            .file_name()
            .and_then(|name| name.to_str())
            .map(|name| name == "hgs" || name == "hgs.exe")
            .unwrap_or(false)
}

fn resolved_target(path: &Path) -> Result<PathBuf> {
    // Resolve even dangling symlinks, as pathlib.resolve() did, so replacing a
    // dotfile never replaces the user's symlink itself.
    let absolute = if path.is_absolute() {
        path.to_owned()
    } else {
        std::env::current_dir()
            .map_err(|e| e.to_string())?
            .join(path)
    };
    let mut resolved = PathBuf::new();
    let mut remaining: std::collections::VecDeque<std::ffi::OsString> = absolute
        .components()
        .map(|component| component.as_os_str().to_owned())
        .collect();
    let mut links = 0;
    while let Some(component) = remaining.pop_front() {
        if component == "/" {
            resolved.push("/");
            continue;
        }
        if component == "." {
            continue;
        }
        if component == ".." {
            resolved.pop();
            continue;
        }
        resolved.push(&component);
        if let Ok(target) = fs::read_link(&resolved) {
            links += 1;
            if links > 40 {
                return Err(format!("too many symlinks: {}", path.display()));
            }
            resolved.pop();
            if target.is_absolute() {
                resolved.clear();
            }
            let mut pieces: std::collections::VecDeque<_> = target
                .components()
                .map(|c| c.as_os_str().to_owned())
                .collect();
            pieces.append(&mut remaining);
            remaining = pieces;
        }
    }
    Ok(resolved)
}

/// The hook entry hgs adds to a JSON hook configuration (Claude Code's
/// `settings.json`, Codex's `hooks.json`) for each event without one.
///
/// On Unix it is the shell command `'/path/to/hgs' __state hook`. On Windows
/// (spec D12) it names the stable CLI by its absolute path with forward
/// slashes: Claude Code gets its exec form when the installed version has it,
/// else the shell form `"C:/…/hgs.exe" __state hook` that Git Bash, `cmd.exe`
/// and `CreateProcess` read alike; Codex gets the shell form as `command` and
/// its PowerShell form as its Windows override `commandWindows`.
#[derive(Clone, Debug, PartialEq, Eq)]
struct Entry {
    /// The command line a shell runs, or with `args` the executable.
    command: String,
    /// Claude Code's exec form: the executable's arguments, run without a shell.
    args: Option<Vec<String>>,
    /// Codex's command for Windows, which Codex prefers there.
    command_windows: Option<String>,
}

impl Entry {
    fn shell(command: String) -> Self {
        Self {
            command,
            args: None,
            command_windows: None,
        }
    }

    /// A matcher group holding this entry, as hgs writes it for one event.
    fn group(&self) -> Value {
        let mut hook = json!({"type": "command", "command": self.command, "timeout": 5});
        if let Some(args) = &self.args {
            hook["args"] = json!(args);
        }
        if let Some(command) = &self.command_windows {
            hook["commandWindows"] = json!(command);
        }
        json!({"hooks": [hook]})
    }

    /// The hgs entry that `hook`, one item of a matcher group's `hooks`, is:
    /// a command line that runs `hgs __state hook` (or a legacy
    /// `hgs_state.py hook`), or on Windows also Claude Code's exec form that
    /// runs hgs with `__state hook`. A Windows entry keeps the fields hgs
    /// writes there, so a new event repeats it exactly.
    fn owned(hook: &Value, windows: bool) -> Option<Self> {
        let command = hook.get("command").and_then(Value::as_str)?;
        let text = |key: &str| hook.get(key).and_then(Value::as_str).map(str::to_owned);
        if owns_hook(command) {
            return Some(if windows {
                Self {
                    command_windows: text("commandWindows"),
                    ..Self::shell(command.to_owned())
                }
            } else {
                Self::shell(command.to_owned())
            });
        }
        let args: Vec<&str> = hook
            .get("args")?
            .as_array()?
            .iter()
            .map(Value::as_str)
            .collect::<Option<_>>()?;
        (windows && args == HOOK_ARGS && names_hgs(command)).then(|| Self {
            args: Some(HOOK_ARGS.map(str::to_owned).to_vec()),
            ..Self::shell(command.to_owned())
        })
    }
}

/// The arguments with which every hook runs hgs.
const HOOK_ARGS: [&str; 2] = ["__state", "hook"];

/// Whether `program` names hgs: `hgs` or `hgs.exe` (any case) after its last
/// `/` or `\`.
fn names_hgs(program: &str) -> bool {
    let name = program.rsplit(['/', '\\']).next().unwrap_or(program);
    name == "hgs" || name.eq_ignore_ascii_case("hgs.exe")
}

/// The Kimi hook block's markers. On Windows each line may also end with CR LF
/// (a `config.toml` that an editor saved with Windows line ends), so such a
/// file never gets a second block.
const KIMI_BEGIN: &str = "# BEGIN hgs session tracking";
const KIMI_END: &str = "# END hgs session tracking";

/// Where the first `marker` line of `text` starts, and its length with its
/// line end: `\n`, or with `crlf` also `\r\n`.
fn marker_line(text: &str, marker: &str, crlf: bool) -> Option<(usize, usize)> {
    let mut from = 0;
    while let Some(found) = text[from..].find(marker) {
        let start = from + found;
        let rest = &text[start + marker.len()..];
        if rest.starts_with('\n') {
            return Some((start, marker.len() + 1));
        }
        if crlf && rest.starts_with("\r\n") {
            return Some((start, marker.len() + 2));
        }
        from = start + marker.len();
    }
    None
}

/// The Kimi `config.toml` text `original` with hgs's block of hooks, or `None`
/// when its block already has every event: then its bytes are kept. A block
/// that misses events is written again, with the hgs command it holds when it
/// holds one and else the command `fresh` gives.
fn merge_kimi(
    original: &str,
    path: &Path,
    crlf: bool,
    fresh: &mut dyn FnMut() -> Result<String>,
) -> Result<Option<String>> {
    let mut command = None;
    let mut content_without = original.to_owned();
    if let Some((start, begin)) = marker_line(original, KIMI_BEGIN, crlf) {
        let rest = &original[start + begin..];
        let (finish, end) = marker_line(rest, KIMI_END, crlf)
            .ok_or_else(|| format!("incomplete hgs hook block in {}", path.display()))?;
        let block = &rest[..finish];
        let document = block
            .parse::<toml_edit::DocumentMut>()
            .map_err(|e| format!("cannot merge tracking hooks into {}: {e}", path.display()))?;
        if let Some(tables) = document
            .get("hooks")
            .and_then(toml_edit::Item::as_array_of_tables)
        {
            command = tables
                .iter()
                .filter_map(|table| table.get("command").and_then(toml_edit::Item::as_str))
                .find(|value| owns_hook(value))
                .map(str::to_owned);
            if events("kimi").iter().all(|event| {
                tables.iter().any(|table| {
                    table.get("event").and_then(toml_edit::Item::as_str) == Some(event)
                        && table
                            .get("command")
                            .and_then(toml_edit::Item::as_str)
                            .map(owns_hook)
                            .unwrap_or(false)
                })
            }) {
                // Validate the complete document but retain exact trusted
                // hook bytes, comments, ordering and user settings.
                original
                    .parse::<toml_edit::DocumentMut>()
                    .map_err(|e| e.to_string())?;
                return Ok(None);
            }
        }
        content_without = format!("{}{}", &original[..start], &rest[finish + end..]);
    }
    let command = match command {
        Some(command) => command,
        None => fresh()?,
    };
    let mut block = format!("{KIMI_BEGIN}\n");
    for event in events("kimi") {
        block.push_str(&format!(
            "[[hooks]]\nevent = {}\ncommand = {}\ntimeout = 5\n",
            json!(event),
            json!(command)
        ));
    }
    block.push_str(&format!("{KIMI_END}\n"));
    let content = format!("{}\n\n{block}", content_without.trim_end_matches('\n'));
    content
        .parse::<toml_edit::DocumentMut>()
        .map_err(|e| format!("cannot merge tracking hooks into {}: {e}", path.display()))?;
    Ok(Some(content))
}

/// The JSON hook configuration text `original` of `agent` with an hgs entry
/// for each of its events, or `None` when every event has one: then nothing
/// is written. New events repeat the hgs entry already there, exactly;
/// only without one is `fresh` asked for the entry to write. An event of
/// `versioned_events` without an entry asks `version` for the agent's version
/// and is added only when that version has it.
fn merge_json(
    original: &str,
    path: &Path,
    agent: &str,
    windows: bool,
    fresh: &mut dyn FnMut() -> Result<Entry>,
    version: &mut dyn FnMut() -> Option<Version>,
) -> Result<Option<String>> {
    let mut document: Value = if original.trim().is_empty() {
        json!({})
    } else {
        serde_json::from_str(original)
            .map_err(|e| format!("cannot read {}: {e}", path.display()))?
    };
    let object = document
        .as_object_mut()
        .ok_or("hook config must be an object")?;
    let hooks = object
        .entry("hooks")
        .or_insert_with(|| json!({}))
        .as_object_mut()
        .ok_or("hooks must be an object")?;
    let mut entry = hooks
        .values()
        .filter_map(Value::as_array)
        .flatten()
        .filter_map(|group| group.get("hooks").and_then(Value::as_array))
        .flatten()
        .find_map(|hook| Entry::owned(hook, windows));
    let mut changed = false;
    let versioned = versioned_events(agent)
        .iter()
        .map(|&(event, oldest)| (event, Some(oldest)));
    for (event, oldest) in events(agent)
        .into_iter()
        .map(|event| (event, None))
        .chain(versioned)
    {
        let installed = match hooks.get(event) {
            Some(entries) => entries
                .as_array()
                .ok_or("hook event must be an array")?
                .iter()
                .filter_map(|group| group.get("hooks").and_then(Value::as_array))
                .flatten()
                .any(|hook| Entry::owned(hook, windows).is_some()),
            None => false,
        };
        // A version that lacks the event, or cannot be read, gets no key for it.
        if installed || oldest.is_some_and(|oldest| version().is_none_or(|v| v < oldest)) {
            continue;
        }
        let group = match &entry {
            Some(entry) => entry.group(),
            None => {
                let new = fresh()?;
                let group = new.group();
                entry = Some(new);
                group
            }
        };
        hooks
            .entry(event)
            .or_insert_with(|| json!([]))
            .as_array_mut()
            .ok_or("hook event must be an array")?
            .push(group);
        changed = true;
    }
    if !changed {
        return Ok(None);
    }
    Ok(Some(format!(
        "{}\n",
        serde_json::to_string_pretty(&document).map_err(|e| e.to_string())?
    )))
}

/// The JSON hook configuration text `original` of `agent` without hgs's
/// entries for a `versioned_events` event that the agent's version lacks, or
/// `None` when nothing goes: then its bytes are kept. Only where hgs has such
/// an entry is `version` asked, and only a version read as older removes it:
/// one that cannot be read keeps a working entry. The user's hooks stay, a
/// matcher group goes only when hgs's entries were all it held, and the event
/// key only when nothing is left in it. Text that `merge_json` cannot read
/// stays for it to report.
fn prune_json(
    original: &str,
    agent: &str,
    windows: bool,
    version: &mut dyn FnMut() -> Option<Version>,
) -> Option<String> {
    let mut document: Value = serde_json::from_str(original).ok()?;
    let hooks = document.get_mut("hooks")?.as_object_mut()?;
    let owned = |hook: &Value| Entry::owned(hook, windows).is_some();
    let mut changed = false;
    for &(event, oldest) in versioned_events(agent) {
        let Some(groups) = hooks.get_mut(event).and_then(Value::as_array_mut) else {
            continue;
        };
        let installed = groups
            .iter()
            .filter_map(|group| group.get("hooks").and_then(Value::as_array))
            .flatten()
            .any(owned);
        // A version that cannot be read may well have the event: its entry stays.
        if !installed || version().is_none_or(|v| v >= oldest) {
            continue;
        }
        groups.retain_mut(|group| {
            let Some(entries) = group.get_mut("hooks").and_then(Value::as_array_mut) else {
                return true;
            };
            let before = entries.len();
            entries.retain(|hook| !owned(hook));
            entries.len() == before || !entries.is_empty()
        });
        if groups.is_empty() {
            hooks.remove(event);
        }
        changed = true;
    }
    if !changed {
        return None;
    }
    Some(format!(
        "{}\n",
        serde_json::to_string_pretty(&document).ok()?
    ))
}

/// Install hgs's tracking hooks into the configuration of `agent`, whose
/// executable is `program` (spec D12 on Windows).
pub(super) fn install_hooks(agent: &str, program: &str) -> Result<()> {
    let home_key = home_var(agent)?;
    let directory = nonempty_env(home_key)
        .map(PathBuf::from)
        .unwrap_or_else(|| {
            home().join(if agent == "kimi" {
                ".kimi-code".to_owned()
            } else {
                format!(".{agent}")
            })
        });
    let path = resolved_target(&directory.join(match agent {
        "kimi" => "config.toml",
        "claude" => "settings.json",
        _ => "hooks.json",
    }))?;
    let lock_path = path.with_file_name(format!(
        "{}.hgs.lock",
        path.file_name().unwrap().to_string_lossy()
    ));
    let _guard = lock(Some(&lock_path))?;
    let original = match fs::read_to_string(&path) {
        Ok(contents) => contents,
        Err(error) if error.kind() == io::ErrorKind::NotFound => String::new(),
        Err(error) => return Err(error.to_string()),
    };
    let executable = runner_executable()?;
    let installed = Installed {
        program,
        agent: if agent == "claude" { "Claude Code" } else { agent },
        version: std::cell::OnceCell::new(),
        seen: root().join("hook-versions").join(format!(
            "{:x}.json",
            Sha256::digest(path.to_string_lossy().as_bytes())
        )),
    };
    let merged = if agent == "kimi" {
        merge_kimi(&original, &path, cfg!(windows), &mut || {
            fresh_command(&executable)
        })?
    } else {
        // An older version run with this configuration later would stop at
        // hgs's entry for an event it lacks: the entry goes before it starts.
        let pruned = prune_json(&original, agent, cfg!(windows), &mut || {
            installed.registered_version(agent)
        });
        merge_json(
            pruned.as_deref().unwrap_or(&original),
            &path,
            agent,
            cfg!(windows),
            &mut || fresh_entry(agent, &executable, &installed),
            &mut || installed.native_version(agent),
        )?
        .or(pruned)
    };
    let Some(content) = merged else {
        return Ok(());
    };
    if content != original {
        let backup = path.with_file_name(format!(
            "{}.pre-hgs",
            path.file_name().unwrap().to_string_lossy()
        ));
        if !original.is_empty() && !backup.exists() {
            atomic(&backup, &original)?;
        }
        atomic(&path, &content)?;
    }
    Ok(())
}

/// The agent whose hooks an installation writes: its executable `program`, its
/// name for messages, and the version `program --version` reports, read at
/// most once and only when a hook depends on it. `seen` keeps the version last
/// read for this configuration with the executable it was read from.
struct Installed<'a> {
    program: &'a str,
    agent: &'a str,
    version: std::cell::OnceCell<Result<Version>>,
    seen: PathBuf,
}

impl Installed<'_> {
    fn version(&self) -> Result<Version> {
        self.version
            .get_or_init(|| version::agent_version(self.program, self.agent))
            .clone()
    }

    /// The version of the native CLI `cli`, never of a project launcher such
    /// as `run_claude.sh`: a launcher runs only under supervision, since it may
    /// prompt or unlock secrets, so its version stays unknown.
    fn native_version(&self, cli: &str) -> Option<Version> {
        if self.program != cli {
            return None;
        }
        let identity = version::executable_identity(self.program);
        let version = self.version().ok()?;
        if let Some(identity) = identity {
            version::remember(&self.seen, &identity, version);
        }
        Some(version)
    }

    /// `native_version` for an event whose entry is already there: the version
    /// last read while the executable stays the same file, so a current agent
    /// is not asked at every start, else read again. A launcher, an
    /// executable that cannot be found and a version that cannot be read stay
    /// unknown.
    fn registered_version(&self, cli: &str) -> Option<Version> {
        if self.program != cli {
            return None;
        }
        let identity = version::executable_identity(self.program)?;
        if let Some(version) = version::recalled(&self.seen, &identity) {
            return Some(version);
        }
        let version = self.version().ok()?;
        version::remember(&self.seen, &identity, version);
        Some(version)
    }
}

/// The command line a hook runs `executable __state hook` with.
#[cfg(unix)]
fn fresh_command(executable: &str) -> Result<String> {
    Ok(join_command(&[
        executable.to_owned(),
        HOOK_ARGS[0].into(),
        HOOK_ARGS[1].into(),
    ]))
}

#[cfg(unix)]
fn fresh_entry(_agent: &str, executable: &str, _installed: &Installed) -> Result<Entry> {
    fresh_command(executable).map(Entry::shell)
}

#[cfg(windows)]
fn fresh_command(executable: &str) -> Result<String> {
    windows::shell_command(executable)
}

#[cfg(windows)]
fn fresh_entry(agent: &str, executable: &str, installed: &Installed) -> Result<Entry> {
    if agent == "claude" {
        windows::claude_entry(executable, installed.version()?, windows::git_bash().is_some())
    } else {
        windows::codex_entry(executable)
    }
}

/// Agent versions, `MAJOR.MINOR.PATCH`, and reading the installed one.
mod version {
    use super::{atomic, Result};
    use serde_json::{json, Value};
    use sha2::{Digest, Sha256};
    use std::fs;
    use std::io::{Read, Seek, SeekFrom};
    use std::path::{Path, PathBuf};
    use std::process::Stdio;
    use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};

    pub(super) type Version = (u64, u64, u64);

    /// How long `--version` may take: a first start of a large agent under
    /// real-time scanning is slow, and a hook form is decided once.
    const VERSION_TIMEOUT: Duration = Duration::from_secs(20);

    /// The first `MAJOR.MINOR.PATCH` word of `text` (a `--version` answer such
    /// as `2.1.296 (Claude Code)` or `codex-cli 0.162.0`), any suffix after
    /// `-` or `+` ignored.
    pub(super) fn version_in(text: &str) -> Option<Version> {
        text.split_whitespace().find_map(|word| {
            let word = word.strip_prefix('v').unwrap_or(word);
            let core = word.split(['-', '+']).next()?;
            let mut parts = core.split('.').map(|part| part.parse::<u64>().ok());
            let version = (parts.next()??, parts.next()??, parts.next()??);
            parts.next().is_none().then_some(version)
        })
    }

    /// The version that `program --version` reports, without a session's
    /// identity. A version that cannot be read is an error: a form chosen
    /// without it would be kept for ever.
    pub(super) fn agent_version(program: &str, agent: &str) -> Result<Version> {
        let fail = |why: String| {
            format!(
                "cannot read the {agent} version ({program} --version: {why}), which decides \
                 its tracking hook form; set HGS_TRACKING=0 to run it untracked"
            )
        };
        let mut output = tempfile::tempfile().map_err(|e| fail(e.to_string()))?;
        let mut command = crate::platform::agent_command(&[program.to_owned(), "--version".into()])
            .map_err(|e| fail(e.to_string()))?;
        command
            .env_remove("HGS_RUN_ID")
            .env_remove("HGS_SESSION")
            .stdin(Stdio::null())
            .stderr(Stdio::null())
            .stdout(output.try_clone().map_err(|e| fail(e.to_string()))?);
        // On Unix in a process group of its own, so a timeout also stops
        // whatever a wrapper script started.
        #[cfg(unix)]
        let spawned = crate::platform::spawn_group(&mut command);
        #[cfg(windows)]
        let spawned = crate::platform::spawn(&mut command);
        let mut child = spawned.map_err(|e| fail(e.to_string()))?;
        let deadline = Instant::now() + VERSION_TIMEOUT;
        let status = loop {
            if let Some(status) = child.try_wait().map_err(|e| fail(e.to_string()))? {
                break status;
            }
            if Instant::now() >= deadline {
                crate::platform::kill_process_group(&mut child);
                let _ = child.wait();
                return Err(fail("timed out".into()));
            }
            std::thread::sleep(Duration::from_millis(20));
        };
        if !status.success() {
            return Err(fail(format!("exit {status}")));
        }
        let mut text = String::new();
        output
            .seek(SeekFrom::Start(0))
            .and_then(|_| output.take(64 * 1024).read_to_string(&mut text))
            .map_err(|e| fail(e.to_string()))?;
        version_in(&text).ok_or_else(|| fail(format!("no version in {:?}", text.trim())))
    }

    /// What `program` starts, as a few file system calls tell it: each file it
    /// runs (on Windows also the script of an npm shim) by its real path, size
    /// and times, and on Unix by its inode too. Installing another version
    /// changes it; a wrapper script that starts another program does not.
    /// `None` when `program` cannot be found.
    pub(super) fn executable_identity(program: &str) -> Option<String> {
        let command = crate::platform::agent_command(&[program.to_owned()]).ok()?;
        let started = crate::platform::which(command.get_program().to_str()?)?;
        let nanos = |time: std::io::Result<SystemTime>| {
            time.ok()
                .and_then(|time| time.duration_since(UNIX_EPOCH).ok())
                .map_or(0, |since| since.as_nanos())
        };
        let mut identity = String::new();
        for file in std::iter::once(started).chain(command.get_args().map(PathBuf::from)) {
            let file = fs::canonicalize(file).ok()?;
            let meta = fs::metadata(&file).ok()?;
            #[cfg(unix)]
            let changed = {
                use std::os::unix::fs::MetadataExt;
                format!(
                    "{}:{}:{}.{}",
                    meta.dev(),
                    meta.ino(),
                    meta.ctime(),
                    meta.ctime_nsec()
                )
            };
            #[cfg(windows)]
            let changed = nanos(meta.created());
            identity.push_str(&format!(
                "{}\0{}\0{}\0{changed}\n",
                file.display(),
                meta.len(),
                nanos(meta.modified())
            ));
        }
        Some(format!("{:x}", Sha256::digest(identity.as_bytes())))
    }

    /// The version that `remember` kept in `seen` for the executable `identity`.
    pub(super) fn recalled(seen: &Path, identity: &str) -> Option<Version> {
        let record: Value = serde_json::from_slice(&fs::read(seen).ok()?).ok()?;
        if record["executable"] != identity {
            return None;
        }
        version_in(record["version"].as_str()?)
    }

    /// Keep in `seen` that the executable `identity` reported `version`. A
    /// record that cannot be written only means the version is read again.
    pub(super) fn remember(seen: &Path, identity: &str, version: Version) {
        if recalled(seen, identity) != Some(version) {
            let (major, minor, patch) = version;
            let record =
                json!({"executable": identity, "version": format!("{major}.{minor}.{patch}")});
            let _ = atomic(seen, &record.to_string());
        }
    }
}

use self::version::Version;

/// The Windows hook forms of spec D12. The decisions are plain functions
/// tested on every platform; finding Git Bash is Windows-only.
#[cfg_attr(not(windows), allow(dead_code))]
mod windows {
    use super::{Entry, Result, Version, HOOK_ARGS};

    /// The oldest Claude Code whose exec-form hooks (`command` plus `args`, no
    /// shell) hgs has verified: the settings schema of 2.1.296 describes them.
    pub(super) const CLAUDE_EXEC_FORM: Version = (2, 1, 296);

    /// Characters that Git Bash, `cmd.exe`, `CreateProcess` and PowerShell
    /// read differently inside double quotes: `"` ends them, `%` expands a
    /// variable in `cmd.exe` and `!` with its delayed expansion, `$` and `` ` ``
    /// expand in bash and PowerShell.
    const SHELL_SPECIAL: &[char] = &['"', '%', '!', '$', '`'];

    /// What PowerShell also reads as a double quote: a path with one would end
    /// the quoted program of Codex's PowerShell form.
    const POWERSHELL_QUOTES: &[char] = &['\u{201C}', '\u{201D}', '\u{201E}'];

    /// The absolute Windows path `path` of the stable CLI with forward
    /// slashes, as every Windows hook names it: `C:/Users/example/.local/bin/hgs.exe`.
    pub(super) fn path(path: &str) -> Result<String> {
        let path = path.replace('\\', "/");
        let bytes = path.as_bytes();
        let drive = bytes.len() > 3
            && bytes[0].is_ascii_alphabetic()
            && bytes[1] == b':'
            && bytes[2] == b'/';
        let unc = path.starts_with("//") && !path.starts_with("//?/") && !path.starts_with("//./");
        if !drive && !unc {
            return Err(format!(
                "the stable hgs CLI {path} is not an absolute drive or UNC path, so tracking \
                 hooks cannot name it"
            ));
        }
        if path.chars().any(char::is_control) {
            return Err(format!(
                "the stable hgs CLI path {path:?} contains a control character"
            ));
        }
        Ok(path)
    }

    /// The shell form: `"C:/Users/example/.local/bin/hgs.exe" __state hook`, which
    /// Git Bash, `cmd.exe` (as `cmd /c` and `cmd /d /s /c` run it) and a
    /// direct `CreateProcess` all parse into that program and `__state hook`.
    /// PowerShell does not: it reads a quoted program as a string.
    pub(super) fn shell_command(stable: &str) -> Result<String> {
        let path = path(stable)?;
        if let Some(special) = path.chars().find(|c| SHELL_SPECIAL.contains(c)) {
            return Err(format!(
                "the stable hgs CLI path {path} contains {special:?}, which Git Bash, cmd.exe \
                 and CreateProcess read differently in a hook command; tracked sessions need \
                 a profile path without it. Set HGS_TRACKING=0 to run the agent untracked"
            ));
        }
        Ok(format!("\"{path}\" {} {}", HOOK_ARGS[0], HOOK_ARGS[1]))
    }

    /// Claude Code's entry for the installed `version`: its exec form from
    /// `CLAUDE_EXEC_FORM` on, else the shell form, which Claude Code runs with
    /// Git Bash. Without Git Bash it would run that in PowerShell, which cannot
    /// run it, so then nothing is written.
    pub(super) fn claude_entry(stable: &str, version: Version, git_bash: bool) -> Result<Entry> {
        if version >= CLAUDE_EXEC_FORM {
            return Ok(Entry {
                command: path(stable)?,
                args: Some(HOOK_ARGS.map(str::to_owned).to_vec()),
                command_windows: None,
            });
        }
        if !git_bash {
            let (major, minor, patch) = version;
            let (a, b, c) = CLAUDE_EXEC_FORM;
            return Err(format!(
                "Claude Code {major}.{minor}.{patch} runs hook commands in PowerShell when Git \
                 Bash is missing, and PowerShell cannot run hgs's tracking hook: install Git \
                 for Windows or update Claude Code to {a}.{b}.{c} or later. Set HGS_TRACKING=0 \
                 to run it untracked"
            ));
        }
        shell_command(stable).map(Entry::shell)
    }

    /// The PowerShell form, `& "C:/…/hgs.exe" __state hook; exit
    /// $LASTEXITCODE`: PowerShell runs a quoted program only with its call
    /// operator, and its `-Command` turns a native exit code other than 0 or 1
    /// into 1 unless the command exits with it (hgs's 2 blocks a prompt during
    /// a pause). The command never reads `$input`, so PowerShell leaves its
    /// standard input, the hook's payload, to hgs unread.
    pub(super) fn powershell_command(stable: &str) -> Result<String> {
        shell_command(stable)?;
        let path = path(stable)?;
        if let Some(quote) = path.chars().find(|c| POWERSHELL_QUOTES.contains(c)) {
            return Err(format!(
                "the stable hgs CLI path {path} contains {quote:?}, which PowerShell reads as \
                 a double quote in a hook command; tracked sessions need a profile path \
                 without it. Set HGS_TRACKING=0 to run the agent untracked"
            ));
        }
        Ok(format!(
            "& \"{path}\" {} {}; exit $LASTEXITCODE",
            HOOK_ARGS[0], HOOK_ARGS[1]
        ))
    }

    /// Codex's entry: the shell form as `command`, and the PowerShell form as
    /// `commandWindows`, which Codex prefers on Windows and runs with the
    /// session's shell, PowerShell there (`-NoProfile -Command`, Codex 0.140 to
    /// 0.162: `build_hooks_config`, `default_user_shell`). `cmd.exe` runs it
    /// only for a session without one local environment, which hgs never
    /// starts, or on a computer without PowerShell.
    pub(super) fn codex_entry(stable: &str) -> Result<Entry> {
        Ok(Entry {
            command_windows: Some(powershell_command(stable)?),
            ..Entry::shell(shell_command(stable)?)
        })
    }

    #[cfg(windows)]
    pub(super) use self::probe::git_bash;

    #[cfg(windows)]
    mod probe {
        use crate::state::nonempty_env;
        use std::path::PathBuf;

        /// The Git Bash that Claude Code would run a hook's shell form with:
        /// `CLAUDE_CODE_GIT_BASH_PATH`, Git for Windows in Program Files, or
        /// the `bin\bash.exe` of the `git` on PATH (Claude Code 2.1.296).
        pub(in crate::state::hooks) fn git_bash() -> Option<PathBuf> {
            if let Some(path) = nonempty_env("CLAUDE_CODE_GIT_BASH_PATH").map(PathBuf::from) {
                let named = path
                    .file_name()
                    .and_then(|name| name.to_str())
                    .is_some_and(|name| {
                        ["bash.exe", "sh.exe", "bash", "sh"].contains(&name.to_lowercase().as_str())
                    });
                if named && path.is_file() {
                    return Some(path);
                }
            }
            [
                r"C:\Program Files\Git\bin\bash.exe",
                r"C:\Program Files (x86)\Git\bin\bash.exe",
            ]
            .into_iter()
            .map(PathBuf::from)
            .chain(
                crate::platform::which("git")
                    .and_then(|git| Some(git.parent()?.parent()?.join(r"bin\bash.exe"))),
            )
            .find(|path| path.is_file())
        }
    }
}

pub(super) fn hook() -> Result<i32> {
    let (Some(name), Some(token)) = (nonempty_env("HGS_SESSION"), nonempty_env("HGS_RUN_ID"))
    else {
        return Ok(0);
    };
    let mut event = stdin_json()?;
    if nonempty_env("HGS_AGENT").as_deref() == Some("claude") {
        if let std::borrow::Cow::Owned(prompt) = journal::pasted_text(string(&event, "prompt")) {
            event["prompt"] = json!(prompt);
        }
    }
    let sid = event
        .get("session_id")
        .and_then(Value::as_str)
        .filter(|sid| !sid.is_empty() && !sid.chars().any(char::is_whitespace))
        .ok_or("hook did not supply a valid session_id")?;
    let kind = string(&event, "hook_event_name");
    let _guard = lock(None)?;
    let Some(mut record) = read_run(&name, &token)? else {
        return Ok(0);
    };
    if !matches(&record, live()?.get(string(&record, "name"))) {
        return Ok(0);
    }
    if let Some(scoped) = external_agents::route(&mut record, &event) {
        processes::observe(&mut record, &scoped);
        journal::update_activity(&mut record, &scoped);
        if kind == "PermissionRequest" {
            let child = &mut record["subagents"][string(&scoped, "agent_id")];
            child["display_state"] = json!("approval");
            // Each request has its own identity, so acknowledging one never hides the next.
            let tool = ["tool_use_id", "tool_call_id"]
                .iter()
                .map(|key| string(&scoped, key))
                .find(|id| !id.is_empty());
            child["attention_id"] =
                json!(tool.map_or_else(|| format!("request:{}", child["updated"]), str::to_owned));
        }
        journal::log_event(&record, &scoped)?;
        write(&mut record)?;
        return Ok(0);
    }
    if kind == "SessionStart" && string(&event, "agent_id").is_empty()
        && !external_agents::main_start(&record, &event) {
        return Ok(0);
    }
    let agent = string(&record, "agent");
    if !events(agent).contains(&kind)
        && !versioned_events(agent).iter().any(|(event, _)| *event == kind)
    {
        return Ok(0);
    }
    if pause_active(&record) && ["UserPromptSubmit", "PreToolUse", "TurnStarted"].contains(&kind) {
        eprintln!("hgs is pausing this session; retry after resuming");
        return Ok(2);
    }
    let changed_conversation = kind == "SessionStart" && string(&event, "agent_id").is_empty()
        && sid != string(&record, "conversation_id");
    if kind == "SessionStart" && string(&event, "agent_id").is_empty() {
        if !string(&record, "fork_parent_id").is_empty() && sid == string(&record, "fork_parent_id")
        {
            record["error"] =
                json!("provider opened the source conversation instead of a new fork");
            record["activity"] = json!("unknown");
            record["phase"] = json!("unknown");
            write(&mut record)?;
            return Ok(0);
        }
        if sid == string(&record["fresh_from"], "conversation_id") && !sid.is_empty() {
            record["error"] =
                json!("provider opened the previous conversation instead of a fresh one");
            record["activity"] = json!("unknown");
            record["phase"] = json!("unknown");
            write(&mut record)?;
            return Ok(0);
        }
        if !string(&record, "expected_id").is_empty() && sid != string(&record, "expected_id")
            && !clear_context::resumed_start(&record, &event)
        {
            record["error"] = json!("agent resumed a different conversation");
            record["activity"] = json!("unknown");
            write(&mut record)?;
            return Ok(0);
        }
        if sid != string(&record, "conversation_id") {
            clear_context::observe_start(&mut record, &event);
            for key in [
                "clear_context_request",
                "compact_context_request",
                "prompt",
                "last_message",
                "subagents",
                "active_tools",
                "last_error",
                "subagent_groups",
                "subagent_groups_complete",
                "subagents_completed_pruned",
                "kimi_native_roster",
                "task_lists",
                "pending_task_updates",
                "shell_jobs",
                "pending_settings",
                "resume_settings",
                "effort_source_path",
                "effort_source_offset",
                "model_source_path",
                "model_source_offset",
            ] {
                record.as_object_mut().unwrap().remove(key);
            }
        }
        record.as_object_mut().unwrap().remove("expected_id");
        record.as_object_mut().unwrap().remove("session_end");
        record
            .as_object_mut()
            .unwrap()
            .remove("pending_task_updates");
        record["conversation_id"] = json!(sid);
        record["conversation_state"] = json!("active");
        record["activity"] = json!("idle");
        record["phase"] = json!("idle");
        record["main_done"] = json!(true);
        if !string(&event, "cwd").is_empty() {
            record["cwd"] = event["cwd"].clone();
        }
        record["transcript"] = event["transcript_path"].clone();
    } else if sid != string(&record, "conversation_id") {
        return Ok(0);
    }
    if kind == "SessionEnd" && string(&event, "agent_id").is_empty() {
        record["session_end"] = json!({"reason": string(&event, "reason"), "at": now(),
            "run_id": record["run_id"], "conversation_id": sid});
    }
    // Claude's suggested next message is no subagent and no Activity event.
    if prompt_suggestion::observe(&mut record, &event) {
        write(&mut record)?;
        return Ok(0);
    }
    telemetry::observe(&mut record, &event);
    tasks::observe(&mut record, &event);
    processes::observe(&mut record, &event);
    journal::update_activity(&mut record, &event);
    compact_context::observe(&mut record, &event);
    questions::observe(&mut record, &event);
    journal::log_event(&record, &event)?;
    if changed_conversation && record["session_clear"]["journaled"] != true {
        if let Some(mut clear) = clear_context::event(&record) {
            clear["hook_event_name"] = json!("SessionCleared");
            journal::log_event(&record, &clear)?;
        }
    }
    write(&mut record)?;
    if kind == "SessionStart" && string(&event, "agent_id").is_empty() {
        archive::confirm_restore(&mut record)?;
    }
    Ok(0)
}

#[cfg(test)]
mod tests {
    use super::windows::{self as forms, CLAUDE_EXEC_FORM};
    use super::*;

    const UNIX: &str = "'/opt/hgs dir/hgs' __state hook";
    const STABLE: &str = r"C:\Profiles\Иван Петров\.local\bin\hgs.exe";
    const SHELL: &str = r#""C:/Profiles/Иван Петров/.local/bin/hgs.exe" __state hook"#;
    const POWERSHELL: &str =
        r#"& "C:/Profiles/Иван Петров/.local/bin/hgs.exe" __state hook; exit $LASTEXITCODE"#;

    fn path() -> PathBuf {
        PathBuf::from("/config/file")
    }

    /// The JSON merge before spec D12, for the Unix byte-compatibility check.
    fn reference_json(original: &str, agent: &str, fresh: &str) -> Result<Option<String>> {
        let mut command = fresh.to_owned();
        let mut document: Value = if original.trim().is_empty() {
            json!({})
        } else {
            serde_json::from_str(original).map_err(|e| e.to_string())?
        };
        let hooks = document
            .as_object_mut()
            .ok_or("hook config must be an object")?
            .entry("hooks")
            .or_insert_with(|| json!({}))
            .as_object_mut()
            .ok_or("hooks must be an object")?;
        if let Some(existing) = hooks
            .values()
            .filter_map(Value::as_array)
            .flatten()
            .filter_map(|group| group.get("hooks").and_then(Value::as_array))
            .flatten()
            .filter_map(|entry| entry.get("command").and_then(Value::as_str))
            .find(|value| owns_hook(value))
        {
            command = existing.to_owned();
        }
        let mut changed = false;
        for event in events(agent) {
            let entries = hooks
                .entry(event)
                .or_insert_with(|| json!([]))
                .as_array_mut()
                .ok_or("hook event must be an array")?;
            let installed = entries
                .iter()
                .filter_map(|group| group.get("hooks").and_then(Value::as_array))
                .flatten()
                .any(|entry| owns_hook(string(entry, "command")));
            if !installed {
                entries.push(
                    json!({"hooks": [{"type": "command", "command": command, "timeout": 5}]}),
                );
                changed = true;
            }
        }
        Ok(changed.then(|| format!("{}\n", serde_json::to_string_pretty(&document).unwrap())))
    }

    /// The Kimi merge before spec D12.
    fn reference_kimi(original: &str, fresh: &str) -> Result<Option<String>> {
        let mut command = fresh.to_owned();
        let begin = "# BEGIN hgs session tracking\n";
        let end = "# END hgs session tracking\n";
        let mut content_without = original.to_owned();
        if let Some(start) = original.find(begin) {
            let rest = &original[start + begin.len()..];
            let finish = rest.find(end).ok_or("incomplete hgs hook block")?;
            let document = rest[..finish]
                .parse::<toml_edit::DocumentMut>()
                .map_err(|e| e.to_string())?;
            if let Some(tables) = document
                .get("hooks")
                .and_then(toml_edit::Item::as_array_of_tables)
            {
                if let Some(existing) = tables
                    .iter()
                    .filter_map(|table| table.get("command").and_then(toml_edit::Item::as_str))
                    .find(|value| owns_hook(value))
                {
                    command = existing.to_owned();
                }
                if events("kimi").iter().all(|event| {
                    tables.iter().any(|table| {
                        table.get("event").and_then(toml_edit::Item::as_str) == Some(event)
                            && table
                                .get("command")
                                .and_then(toml_edit::Item::as_str)
                                .is_some_and(owns_hook)
                    })
                }) {
                    original
                        .parse::<toml_edit::DocumentMut>()
                        .map_err(|e| e.to_string())?;
                    return Ok(None);
                }
            }
            content_without = format!("{}{}", &original[..start], &rest[finish + end.len()..]);
        }
        let mut block = begin.to_owned();
        for event in events("kimi") {
            block.push_str(&format!(
                "[[hooks]]\nevent = {}\ncommand = {}\ntimeout = 5\n",
                json!(event),
                json!(command)
            ));
        }
        block.push_str(end);
        let content = format!("{}\n\n{block}", content_without.trim_end_matches('\n'));
        content
            .parse::<toml_edit::DocumentMut>()
            .map_err(|e| e.to_string())?;
        Ok(Some(content))
    }

    /// A merge for an agent whose version cannot be read.
    fn json_merge(original: &str, agent: &str, windows: bool, fresh: Entry) -> Option<String> {
        let mut asked = 0;
        let merged = merge_json(
            original,
            &path(),
            agent,
            windows,
            &mut || {
                asked += 1;
                Ok(fresh.clone())
            },
            &mut || None,
        )
        .unwrap();
        assert!(asked <= 1);
        merged
    }

    /// A merge that must reuse the hgs entry already there.
    fn json_reuse(original: &str, agent: &str, windows: bool) -> Option<String> {
        merge_json(
            original,
            &path(),
            agent,
            windows,
            &mut || Err("a fresh entry was asked for".into()),
            &mut || None,
        )
        .unwrap()
    }

    fn kimi_merge(original: &str, crlf: bool, fresh: &str) -> Option<String> {
        merge_kimi(original, &path(), crlf, &mut || Ok(fresh.to_owned())).unwrap()
    }

    fn hook_entries(text: &str, event: &str) -> Vec<Value> {
        let document: Value = serde_json::from_str(text).unwrap();
        document["hooks"][event]
            .as_array()
            .unwrap()
            .iter()
            .flat_map(|group| group["hooks"].as_array().unwrap().clone())
            .collect()
    }

    #[test]
    fn unix_json_hooks_keep_their_bytes() {
        let user = json!({"command": "notify-send done", "type": "command"});
        let legacy = format!("python3 {} hook", "/old/hgs_state.py");
        let cases = [
            String::new(),
            "  \n".into(),
            json!({"model": "opus", "hooks": {"Stop": [{"hooks": [user]}]}}).to_string(),
            json!({"hooks": {"Stop": [{"matcher": "", "hooks": [{"type": "command",
                "command": "'/usr/bin/hgs' __state hook"}]}]}})
            .to_string(),
            json!({"hooks": {"SessionStart": [{"hooks": [{"type": "command", "command": legacy}]}]}})
                .to_string(),
            // An exec-form entry is not an hgs hook on Unix, as before.
            json!({"hooks": {"Stop": [{"hooks": [{"type": "command",
                "command": "/usr/bin/hgs", "args": ["__state", "hook"]}]}]}})
            .to_string(),
        ];
        for agent in ["claude", "codex"] {
            for original in &cases {
                let expected = reference_json(original, agent, UNIX).unwrap();
                assert_eq!(
                    json_merge(original, agent, false, Entry::shell(UNIX.into())),
                    expected,
                    "{agent}: {original}"
                );
                // Merging the result again changes nothing.
                let merged = expected.unwrap();
                assert_eq!(reference_json(&merged, agent, UNIX).unwrap(), None);
                assert_eq!(json_reuse(&merged, agent, false), None);
            }
        }
        // Errors keep their words.
        for (original, error) in [
            ("[]", "hook config must be an object"),
            (r#"{"hooks": []}"#, "hooks must be an object"),
            (r#"{"hooks": {"Stop": {}}}"#, "hook event must be an array"),
        ] {
            let result = merge_json(
                original,
                &path(),
                "claude",
                false,
                &mut || Ok(Entry::shell(UNIX.into())),
                &mut || None,
            );
            assert_eq!(result.unwrap_err(), error);
        }
        assert!(merge_json(
            "{",
            &path(),
            "claude",
            false,
            &mut || Ok(Entry::shell(UNIX.into())),
            &mut || None
        )
        .unwrap_err()
        .starts_with("cannot read /config/file: "));
    }

    #[test]
    fn unix_kimi_hooks_keep_their_bytes() {
        let complete = reference_kimi("default_model = \"probe\"\n", UNIX)
            .unwrap()
            .unwrap();
        let partial = complete.replace("event = \"SessionHeartbeat\"", "event = \"Other\"");
        let crlf = complete.replace('\n', "\r\n");
        let cases = [
            String::new(),
            "default_model = \"probe\"\n\n\n".into(),
            complete.clone(),
            partial,
            format!("{complete}\n[loop_control]\nmax_attempts_per_step = 1\n"),
            // Before D12 a CRLF block was not found, and Unix keeps that.
            crlf,
        ];
        for original in &cases {
            assert_eq!(
                kimi_merge(original, false, UNIX),
                reference_kimi(original, UNIX).unwrap(),
                "{original:?}"
            );
        }
        let error = merge_kimi(
            "# BEGIN hgs session tracking\n[[hooks]]\n",
            &path(),
            false,
            &mut || Ok(UNIX.into()),
        )
        .unwrap_err();
        assert_eq!(error, "incomplete hgs hook block in /config/file");
    }

    #[test]
    fn windows_hook_commands_name_the_stable_cli_with_forward_slashes() {
        assert_eq!(forms::shell_command(STABLE).unwrap(), SHELL);
        assert_eq!(
            forms::shell_command(r"C:\Profiles\a & b (1)\^x;y'z\hgs.exe").unwrap(),
            r#""C:/Profiles/a & b (1)/^x;y'z/hgs.exe" __state hook"#
        );
        assert_eq!(
            forms::shell_command(r"\\server\share\me\.local\bin\hgs.exe").unwrap(),
            r#""//server/share/me/.local/bin/hgs.exe" __state hook"#
        );
        assert_eq!(
            forms::path(STABLE).unwrap(),
            "C:/Profiles/Иван Петров/.local/bin/hgs.exe"
        );
        // A shell form names hgs the way the other hgs hooks do.
        assert!(owns_hook(SHELL));
        for refused in [
            r"hgs.exe",
            r"\Users\me\hgs.exe",
            r"\\?\C:\Users\me\hgs.exe",
            r"\\.\C:\Users\me\hgs.exe",
            "C:\\Users\\me\n\\hgs.exe",
        ] {
            assert!(forms::path(refused).is_err(), "{refused:?}");
            assert!(forms::shell_command(refused).is_err(), "{refused:?}");
        }
        for special in ['%', '$', '`', '!', '"'] {
            let error =
                forms::shell_command(&format!(r"C:\Users\a{special}b\hgs.exe")).unwrap_err();
            assert!(error.contains(&format!("{special:?}")), "{error}");
            assert!(error.contains("HGS_TRACKING=0"), "{error}");
        }
        // The exec form has no shell to misread them.
        assert_eq!(
            forms::claude_entry(r"C:\Profiles\a%b$c\hgs.exe", CLAUDE_EXEC_FORM, false)
                .unwrap()
                .command,
            "C:/Profiles/a%b$c/hgs.exe"
        );
    }

    #[test]
    fn claude_uses_its_exec_form_when_its_version_has_it() {
        let exec = Entry {
            command: forms::path(STABLE).unwrap(),
            args: Some(vec!["__state".into(), "hook".into()]),
            command_windows: None,
        };
        for version in [CLAUDE_EXEC_FORM, (2, 1, 300), (2, 2, 0), (3, 0, 0)] {
            for git_bash in [false, true] {
                assert_eq!(
                    forms::claude_entry(STABLE, version, git_bash).unwrap(),
                    exec
                );
            }
        }
        // Older versions run the shell form with Git Bash; PowerShell, their
        // fallback without it, cannot run it, so nothing is written then.
        assert_eq!(
            forms::claude_entry(STABLE, (2, 1, 295), true).unwrap(),
            Entry::shell(SHELL.into())
        );
        let error = forms::claude_entry(STABLE, (2, 0, 9), false).unwrap_err();
        assert!(error.contains("Claude Code 2.0.9"), "{error}");
        assert!(error.contains("Git for Windows"), "{error}");
        assert!(error.contains("2.1.296"), "{error}");
        assert!(error.contains("HGS_TRACKING=0"), "{error}");

        let codex = forms::codex_entry(STABLE).unwrap();
        assert_eq!(
            codex,
            Entry {
                command: SHELL.into(),
                args: None,
                command_windows: Some(POWERSHELL.into()),
            }
        );
    }

    #[test]
    fn codex_gets_a_powershell_form_on_windows() {
        assert_eq!(forms::powershell_command(STABLE).unwrap(), POWERSHELL);
        assert_eq!(
            forms::powershell_command(r"C:\Profiles\a & b (1)\^x;y'z%\hgs.exe").unwrap_err(),
            forms::shell_command(r"C:\Profiles\a & b (1)\^x;y'z%\hgs.exe").unwrap_err()
        );
        assert_eq!(
            forms::powershell_command(r"C:\Profiles\a & b (1)\^x;y'z\hgs.exe").unwrap(),
            r#"& "C:/Profiles/a & b (1)/^x;y'z/hgs.exe" __state hook; exit $LASTEXITCODE"#
        );
        // PowerShell also ends a double-quoted string at these.
        for quote in ['\u{201C}', '\u{201D}', '\u{201E}'] {
            let stable = format!(r"C:\Users\a{quote}b\hgs.exe");
            assert!(forms::shell_command(&stable).is_ok());
            let error = forms::powershell_command(&stable).unwrap_err();
            assert!(error.contains(&format!("{quote:?}")), "{error}");
            assert!(error.contains("HGS_TRACKING=0"), "{error}");
            assert!(forms::codex_entry(&stable).is_err());
        }
        // The shell form, Codex's `command`, is what names the entry as hgs's.
        let hook = json!({"type": "command", "command": SHELL, "commandWindows": POWERSHELL});
        assert_eq!(
            Entry::owned(&hook, true),
            Some(forms::codex_entry(STABLE).unwrap())
        );
    }

    #[test]
    fn versions_are_read_from_version_answers() {
        assert_eq!(
            version::version_in("2.1.296 (Claude Code)\n"),
            Some((2, 1, 296))
        );
        assert_eq!(version::version_in("codex-cli 0.162.0"), Some((0, 162, 0)));
        assert_eq!(version::version_in("kimi v2.1.1-beta.3"), Some((2, 1, 1)));
        assert_eq!(
            version::version_in("tool 1.2.3+build.7 extra 4.5.6"),
            Some((1, 2, 3))
        );
        for text in ["", "Claude Code", "2.1", "1.2.3.4", "x1.2.3", "2.1.x"] {
            assert_eq!(version::version_in(text), None, "{text:?}");
        }
        assert!((2, 1, 296) > (2, 1, 99));
    }

    #[test]
    fn windows_json_hooks_repeat_the_entry_already_there() {
        let exec = forms::claude_entry(STABLE, CLAUDE_EXEC_FORM, false).unwrap();
        let settings = json_merge(r#"{"model": "opus"}"#, "claude", true, exec.clone()).unwrap();
        for event in events("claude") {
            assert_eq!(
                hook_entries(&settings, event),
                [json!({"type": "command", "command": exec.command,
                    "args": ["__state", "hook"], "timeout": 5})],
                "{event}"
            );
        }
        // The bytes Claude Code reads: its exec form, no shell.
        let stop = "    \"Stop\": [\n      {\n        \"hooks\": [\n          {\n            \
            \"args\": [\n              \"__state\",\n              \"hook\"\n            ],\n            \
            \"command\": \"C:/Profiles/Иван Петров/.local/bin/hgs.exe\",\n            \
            \"timeout\": 5,\n            \"type\": \"command\"\n          }\n        ]\n      }\n    ],\n";
        assert!(settings.contains(stop), "{settings}");
        assert_eq!(json_reuse(&settings, "claude", true), None);
        // A missing event gets the same exec form, never a new decision.
        let mut document: Value = serde_json::from_str(&settings).unwrap();
        document["hooks"].as_object_mut().unwrap().remove("Stop");
        let merged = json_reuse(&document.to_string(), "claude", true).unwrap();
        assert_eq!(
            hook_entries(&merged, "Stop"),
            hook_entries(&settings, "Stop")
        );
        // A shell form written by an older Claude Code stays a shell form.
        let shell = json_merge("", "claude", true, Entry::shell(SHELL.into())).unwrap();
        let mut document: Value = serde_json::from_str(&shell).unwrap();
        document["hooks"]
            .as_object_mut()
            .unwrap()
            .remove("Notification");
        let merged = json_reuse(&document.to_string(), "claude", true).unwrap();
        assert_eq!(
            hook_entries(&merged, "Notification"),
            [json!({"type": "command", "command": SHELL, "timeout": 5})]
        );
        // Unix never reads an exec form as an hgs hook.
        assert_eq!(
            Entry::owned(&hook_entries(&settings, "Stop")[0], false),
            None
        );
        // Another program, or other arguments, is not hgs's hook.
        for (command, args) in [
            ("C:/tools/other.exe", json!(["__state", "hook"])),
            ("C:/tools/hgs.exe", json!(["__state", "hook", "x"])),
            ("C:/tools/hgs.exe", json!(["hook"])),
            ("C:/tools/hgs.exe", json!("__state hook")),
        ] {
            let hook = json!({"type": "command", "command": command, "args": args});
            assert_eq!(Entry::owned(&hook, true), None, "{hook}");
        }
        let hook =
            json!({"type": "command", "command": r"C:\Tools\HGS.EXE", "args": ["__state", "hook"]});
        assert!(Entry::owned(&hook, true).is_some());

        let codex = forms::codex_entry(STABLE).unwrap();
        let hooks = json_merge("", "codex", true, codex).unwrap();
        for event in events("codex") {
            assert_eq!(
                hook_entries(&hooks, event),
                [
                    json!({"type": "command", "command": SHELL, "commandWindows": POWERSHELL,
                    "timeout": 5})
                ]
            );
        }
        let mut document: Value = serde_json::from_str(&hooks).unwrap();
        document["hooks"]
            .as_object_mut()
            .unwrap()
            .remove("Interrupt");
        let merged = json_reuse(&document.to_string(), "codex", true).unwrap();
        assert_eq!(
            hook_entries(&merged, "Interrupt"),
            hook_entries(&hooks, "Stop")
        );
    }

    #[test]
    fn a_crlf_kimi_config_never_gets_a_second_block_on_windows() {
        let lf = kimi_merge("default_model = \"probe\"\n", true, SHELL).unwrap();
        assert!(lf.contains(&format!("command = {}\n", json!(SHELL))));
        assert_eq!(kimi_merge(&lf, true, "unused"), None);
        // An editor saved the whole file with Windows line ends.
        let crlf = lf.replace('\n', "\r\n");
        assert_eq!(kimi_merge(&crlf, true, "unused"), None);
        // A block that misses events is replaced, keeping its command.
        let partial = crlf.replace("event = \"SessionHeartbeat\"", "event = \"Other\"");
        let merged = kimi_merge(&partial, true, "unused").unwrap();
        assert_eq!(merged.matches(KIMI_BEGIN).count(), 1, "{merged}");
        assert_eq!(merged.matches(KIMI_END).count(), 1, "{merged}");
        assert!(
            merged.starts_with("default_model = \"probe\"\r\n"),
            "{merged}"
        );
        assert_eq!(kimi_merge(&merged, true, "unused"), None);
        // Mixed line ends, and text around the block, are kept.
        let mixed = format!(
            "a = 1\r\n{}b = 2\r\n",
            &lf["default_model = \"probe\"\n".len()..]
        );
        let mixed = mixed.replacen(&format!("{KIMI_END}\n"), &format!("{KIMI_END}\r\n"), 1);
        assert_eq!(kimi_merge(&mixed, true, "unused"), None);
        // Only a marker line counts.
        let other = format!("{KIMI_BEGIN}X\r\nx = 1\n");
        let merged = kimi_merge(&other, true, SHELL).unwrap();
        assert!(merged.starts_with(&other), "{merged}");
        assert_eq!(merged.matches(&format!("{KIMI_BEGIN}\n")).count(), 1);
        assert_eq!(marker_line("a\r\nb", "a", false), None);
        assert_eq!(marker_line("a\r\nb", "a", true), Some((0, 3)));
        assert_eq!(marker_line("ab\na\n", "a", false), Some((3, 2)));
    }

    /// A merge for an agent that reports `version`, which says how often it was asked.
    fn versioned_merge(
        original: &str,
        agent: &str,
        windows: bool,
        fresh: Entry,
        version: Option<Version>,
    ) -> (Option<String>, usize) {
        let mut asked = 0;
        let merged = merge_json(
            original,
            &path(),
            agent,
            windows,
            &mut || Ok(fresh.clone()),
            &mut || {
                asked += 1;
                version
            },
        )
        .unwrap();
        (merged, asked)
    }

    #[test]
    fn claude_model_switch_is_registered_only_for_versions_that_have_it() {
        let unix = Entry::shell(UNIX.into());
        let older = json_merge(r#"{"model": "opus"}"#, "claude", false, unix.clone()).unwrap();
        // Older Claude Code stops every start at a warning for an event it does
        // not know, so neither its key nor PreModelSwitch is ever written.
        for version in [None, Some((2, 1, 295)), Some((2, 0, 9))] {
            let (merged, asked) = versioned_merge(
                r#"{"model": "opus"}"#,
                "claude",
                false,
                unix.clone(),
                version,
            );
            assert_eq!(merged.as_ref(), Some(&older), "{version:?}");
            assert_eq!(asked, 1);
            let (merged, asked) = versioned_merge(&older, "claude", false, unix.clone(), version);
            assert_eq!((merged, asked), (None, 1), "{version:?}");
        }
        let hooks: Value = serde_json::from_str(&older).unwrap();
        assert!(hooks["hooks"].get("PostModelSwitch").is_none());
        assert!(hooks["hooks"].get("PreModelSwitch").is_none());
        // A version that has it gets the entry already there, and nothing else changes.
        for version in [CLAUDE_MODEL_SWITCH, (2, 1, 300), (2, 2, 0), (3, 0, 0)] {
            let (merged, asked) = versioned_merge(
                &older,
                "claude",
                false,
                Entry::shell("unused".into()),
                Some(version),
            );
            let merged = merged.unwrap();
            assert_eq!(asked, 1);
            assert_eq!(
                hook_entries(&merged, "PostModelSwitch"),
                hook_entries(&older, "Stop")
            );
            let mut document: Value = serde_json::from_str(&merged).unwrap();
            document["hooks"]
                .as_object_mut()
                .unwrap()
                .remove("PostModelSwitch");
            assert_eq!(document, serde_json::from_str::<Value>(&older).unwrap());
            assert!(document["hooks"].get("PreModelSwitch").is_none());
            // Once it is there, the version is never read again.
            assert_eq!(
                versioned_merge(&merged, "claude", false, unix.clone(), None),
                (None, 0)
            );
        }
        // A fresh configuration gets every event with one entry.
        let (fresh, _) =
            versioned_merge("", "claude", false, unix.clone(), Some(CLAUDE_MODEL_SWITCH));
        let fresh = fresh.unwrap();
        for event in events("claude").into_iter().chain(["PostModelSwitch"]) {
            assert_eq!(
                hook_entries(&fresh, event),
                [json!({"type": "command", "command": UNIX, "timeout": 5})]
            );
        }
        // The user's own PostModelSwitch hook stays, beside hgs's.
        let user = json!({"type": "command", "command": "notify-send switched"});
        let mut document: Value = serde_json::from_str(&older).unwrap();
        document["hooks"]["PostModelSwitch"] = json!([{"matcher": "opus", "hooks": [user]}]);
        let (merged, _) = versioned_merge(
            &document.to_string(),
            "claude",
            false,
            unix.clone(),
            Some(CLAUDE_MODEL_SWITCH),
        );
        let merged = merged.unwrap();
        assert_eq!(
            hook_entries(&merged, "PostModelSwitch"),
            [user, hook_entries(&older, "Stop")[0].clone()]
        );
        let (merged, _) = versioned_merge(
            &document.to_string(),
            "claude",
            false,
            unix.clone(),
            Some((2, 1, 295)),
        );
        assert_eq!(merged, None);
        // Only Claude has versioned events; Codex is never asked.
        let codex = json_merge("", "codex", false, unix.clone()).unwrap();
        assert_eq!(
            versioned_merge("", "codex", false, unix.clone(), Some((9, 9, 9))),
            (Some(codex), 0)
        );
        // Windows repeats the exec form there too.
        let exec = forms::claude_entry(STABLE, CLAUDE_EXEC_FORM, false).unwrap();
        let (settings, _) =
            versioned_merge("", "claude", true, exec.clone(), Some(CLAUDE_MODEL_SWITCH));
        let settings = settings.unwrap();
        assert_eq!(
            hook_entries(&settings, "PostModelSwitch"),
            hook_entries(&settings, "Stop")
        );
        assert_eq!(
            Entry::owned(&hook_entries(&settings, "PostModelSwitch")[0], true),
            Some(exec)
        );
    }

    #[cfg(unix)]
    #[test]
    fn model_switch_version_never_runs_a_project_launcher() {
        use std::os::unix::fs::PermissionsExt;
        let folder = tempfile::tempdir().unwrap();
        let marker = folder.path().join("launcher-ran");
        let launcher = folder.path().join("run_claude.sh");
        let script = format!("#!/bin/sh\n: > '{}'\necho '2.1.296 (Claude Code)'\n", marker.display());
        fs::write(&launcher, script).unwrap();
        fs::set_permissions(&launcher, fs::Permissions::from_mode(0o755)).unwrap();
        let program = launcher.to_string_lossy();
        let installed = Installed {
            program: &program,
            agent: "Claude Code",
            version: std::cell::OnceCell::new(),
            seen: folder.path().join("seen.json"),
        };
        // A launcher may prompt or unlock secrets: its version stays unknown.
        assert_eq!(installed.native_version("claude"), None);
        assert_eq!(installed.registered_version("claude"), None);
        assert!(!marker.exists());
        // The same script read as the agent itself answers.
        assert_eq!(installed.native_version(&program), Some(CLAUDE_MODEL_SWITCH));
        assert!(marker.exists());
    }

    /// A pruning for an agent that reports `version`, which says how often it was asked.
    fn versioned_prune(
        original: &str,
        agent: &str,
        windows: bool,
        version: Option<Version>,
    ) -> (Option<String>, usize) {
        let mut asked = 0;
        let pruned = prune_json(original, agent, windows, &mut || {
            asked += 1;
            version
        });
        (pruned, asked)
    }

    #[test]
    fn claude_model_switch_entry_goes_only_for_an_older_version() {
        let unix = Entry::shell(UNIX.into());
        let older = json_merge(r#"{"model": "opus"}"#, "claude", false, unix.clone()).unwrap();
        let (registered, _) = versioned_merge(
            &older,
            "claude",
            false,
            unix.clone(),
            Some(CLAUDE_MODEL_SWITCH),
        );
        let registered = registered.unwrap();
        // A version that has the event keeps the entry and every byte, and so
        // does one that cannot be read: it may well be current.
        for version in [None, Some(CLAUDE_MODEL_SWITCH), Some((2, 2, 0))] {
            assert_eq!(
                versioned_prune(&registered, "claude", false, version),
                (None, 1),
                "{version:?}"
            );
        }
        // An older version gets back what it had before the entry, and keeps it.
        for version in [(2, 1, 295), (2, 0, 9)] {
            let (pruned, asked) = versioned_prune(&registered, "claude", false, Some(version));
            assert_eq!((pruned.as_deref(), asked), (Some(older.as_str()), 1));
            assert_eq!(
                versioned_merge(&older, "claude", false, unix.clone(), Some(version)),
                (None, 1)
            );
            assert_eq!(
                versioned_prune(&older, "claude", false, Some(version)),
                (None, 0)
            );
        }
        // Only hgs's entries go, as hgs knows them (a legacy one too): the
        // user's hooks stay, and so does a group of theirs beside hgs's.
        let user = json!({"type": "command", "command": "notify-send switched"});
        let entry = hook_entries(&registered, "PostModelSwitch")[0].clone();
        let legacy = json!({"type": "command", "command": "python3 /old/hgs_state.py hook"});
        let mut document: Value = serde_json::from_str(&registered).unwrap();
        document["hooks"]["PostModelSwitch"] = json!([
            {"matcher": "opus", "hooks": [user]},
            {"hooks": [entry]},
            {"matcher": "haiku", "hooks": [entry, user, legacy]},
            {"matcher": "sonnet", "hooks": []},
            {"matcher": "fable"},
        ]);
        let (pruned, _) =
            versioned_prune(&document.to_string(), "claude", false, Some((2, 1, 295)));
        let mut pruned: Value = serde_json::from_str(&pruned.unwrap()).unwrap();
        assert_eq!(
            pruned["hooks"]["PostModelSwitch"],
            json!([
                {"matcher": "opus", "hooks": [user]},
                {"matcher": "haiku", "hooks": [user]},
                {"matcher": "sonnet", "hooks": []},
                {"matcher": "fable"},
            ])
        );
        for document in [&mut document, &mut pruned] {
            document["hooks"]
                .as_object_mut()
                .unwrap()
                .remove("PostModelSwitch");
        }
        assert_eq!(pruned, document);
        // Without an hgs entry there, nothing asks for the version.
        let mut theirs: Value = serde_json::from_str(&older).unwrap();
        theirs["hooks"]["PostModelSwitch"] = json!([{"hooks": [user]}]);
        assert_eq!(
            versioned_prune(&theirs.to_string(), "claude", false, Some((2, 1, 295))),
            (None, 0)
        );
        // Only Claude has versioned events, and what merge_json reports stays for it.
        assert_eq!(
            versioned_prune(&registered, "codex", false, Some((2, 1, 295))),
            (None, 0)
        );
        for original in [
            "",
            "{",
            "[]",
            r#"{"hooks": []}"#,
            r#"{"hooks": {"PostModelSwitch": {}}}"#,
        ] {
            assert_eq!(
                versioned_prune(original, "claude", false, Some((2, 1, 295))),
                (None, 0),
                "{original:?}"
            );
        }
        // Windows knows its exec form as hgs's too.
        let exec = forms::claude_entry(STABLE, CLAUDE_EXEC_FORM, false).unwrap();
        let (settings, _) =
            versioned_merge("", "claude", true, exec.clone(), Some(CLAUDE_MODEL_SWITCH));
        let settings = settings.unwrap();
        let (pruned, _) = versioned_prune(&settings, "claude", true, Some((2, 1, 295)));
        let pruned = pruned.unwrap();
        let hooks: Value = serde_json::from_str(&pruned).unwrap();
        assert!(hooks["hooks"].get("PostModelSwitch").is_none());
        for event in events("claude") {
            assert_eq!(hook_entries(&pruned, event), hook_entries(&settings, event));
        }
    }

    #[cfg(unix)]
    #[test]
    fn registered_version_is_read_again_only_for_another_executable() {
        use std::os::unix::fs::PermissionsExt;
        let folder = tempfile::tempdir().unwrap();
        let reads = folder.path().join("reads");
        let answer = folder.path().join("answer");
        let claude = folder.path().join("claude");
        // A new file in its place, as an installer writes it.
        let install = |rest: &str| {
            let new = folder.path().join("claude.new");
            let script = format!(
                "#!/bin/sh\necho >> '{}'\ncat '{}'\n{rest}",
                reads.display(),
                answer.display()
            );
            fs::write(&new, script).unwrap();
            fs::set_permissions(&new, fs::Permissions::from_mode(0o755)).unwrap();
            fs::rename(&new, &claude).unwrap();
        };
        let asked = || fs::read_to_string(&reads).map_or(0, |text| text.lines().count());
        let program = claude.to_string_lossy().into_owned();
        let seen = folder.path().join("state/hook-versions/settings.json");
        let launch = || Installed {
            program: &program,
            agent: "Claude Code",
            version: std::cell::OnceCell::new(),
            seen: seen.clone(),
        };
        install("");
        fs::write(&answer, "2.1.296 (Claude Code)\n").unwrap();
        // The launch that registers the entry keeps the version it read.
        assert_eq!(launch().native_version(&program), Some(CLAUDE_MODEL_SWITCH));
        assert_eq!(asked(), 1);
        // Later launches of the same file are not asked, whatever it would answer.
        fs::write(&answer, "2.1.295 (Claude Code)\n").unwrap();
        for _ in 0..2 {
            assert_eq!(
                launch().registered_version(&program),
                Some(CLAUDE_MODEL_SWITCH)
            );
        }
        assert_eq!(asked(), 1);
        // Another version installed in its place is, once.
        install("# 2.1.295\n");
        let installed = launch();
        assert_eq!(installed.registered_version(&program), Some((2, 1, 295)));
        assert_eq!(installed.native_version(&program), Some((2, 1, 295)));
        assert_eq!(launch().registered_version(&program), Some((2, 1, 295)));
        assert_eq!(asked(), 2);
        // A version that cannot be read stays unknown and is asked again.
        install("exit 3\n");
        for _ in 0..2 {
            assert_eq!(launch().registered_version(&program), None);
        }
        assert_eq!(asked(), 4);
        // A launcher, and an executable that is gone, are never run.
        assert_eq!(launch().registered_version("claude"), None);
        fs::remove_file(&claude).unwrap();
        assert_eq!(launch().registered_version(&program), None);
        assert_eq!(asked(), 4);
    }
}
