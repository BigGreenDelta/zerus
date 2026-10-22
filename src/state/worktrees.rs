//! On-demand checkout catalog and explicit creation of a new branch/worktree.
use super::*;
use fs2::FileExt;
use sha2::{Digest, Sha256};
use std::fs::{self, OpenOptions};
use std::process::Command;
use std::time::{Duration, Instant};

const MAX_OUTPUT: usize = 1024 * 1024;
const MAX_TREES: usize = 512;
const TTL: f64 = 30.0;
static MOBILE_STOP: std::sync::atomic::AtomicBool = std::sync::atomic::AtomicBool::new(false);
#[cfg(unix)]
extern "C" fn stop_mobile(_: libc::c_int) { MOBILE_STOP.store(true, std::sync::atomic::Ordering::Relaxed); }
#[cfg(unix)]
struct MobileSignals(Vec<(libc::c_int, libc::sighandler_t)>);
#[cfg(unix)]
impl MobileSignals {
    fn install() -> Self {
        MOBILE_STOP.store(false, std::sync::atomic::Ordering::Relaxed);
        Self([libc::SIGHUP, libc::SIGTERM].iter().map(|signal| (*signal, unsafe {
            libc::signal(*signal, stop_mobile as *const () as libc::sighandler_t)
        })).collect())
    }
}
#[cfg(unix)]
impl Drop for MobileSignals {
    fn drop(&mut self) { for (signal, old) in &self.0 { unsafe { libc::signal(*signal, *old); } } }
}
/// Windows is no mobile gateway in Phase 2 (the mobile connector is Unix-only),
/// so nothing cancels a mobile creation there; every Git call keeps its deadline.
#[cfg(not(unix))]
struct MobileSignals;
#[cfg(not(unix))]
impl MobileSignals {
    fn install() -> Self {
        MOBILE_STOP.store(false, std::sync::atomic::Ordering::Relaxed);
        Self
    }
}
fn mobile_stopped() -> bool { MOBILE_STOP.load(std::sync::atomic::Ordering::Relaxed) }


pub(super) struct GitError {
    pub(super) state: &'static str,
    pub(super) detail: String,
    /// Git's exit code when it completed with a failure.
    pub(super) code: Option<i32>,
}
pub(super) fn error(state: &'static str, detail: impl Into<String>) -> GitError {
    GitError {
        state,
        detail: detail.into(),
        code: None,
    }
}

pub(super) fn git(cwd: &Path, args: &[&str], deadline: Instant) -> std::result::Result<Vec<u8>, GitError> {
    if mobile_stopped() || Instant::now() >= deadline { return Err(error("timeout", "Git operation cancelled or timed out")); }
    let mut command = Command::new("git");
    command
        .args(["--no-optional-locks", "-c", "core.fsmonitor=false", "-C"])
        .arg(cwd)
        .args(args)
        .env("LC_ALL", "C")
        .env("GIT_OPTIONAL_LOCKS", "0");
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
    if args.contains(&"ls-remote") {
        // Read-only publication checks must never open a credential prompt.
        crate::platform::git_noninteractive(&mut command);
    }
    // Both pipes are drained while Git runs, including large catalogs. A
    // checkout hook or filter can outlive Git: on timeout the whole operation
    // stops instead of writing on after an error was reported.
    let (status, out, err) =
        crate::platform::run_bounded(&mut command, [MAX_OUTPUT, 8192], deadline, &mobile_stopped)
            .map_err(|failure| match failure {
                crate::platform::RunError::Spawn(e) => error(
                    if e.kind() == io::ErrorKind::NotFound {
                        "git_unavailable"
                    } else {
                        "metadata_error"
                    },
                    e.to_string(),
                ),
                crate::platform::RunError::TooLarge => {
                    error("too_large", "Git operation exceeded the output limit")
                }
                crate::platform::RunError::Io(e) => error("metadata_error", e.to_string()),
                _ => error("timeout", "Git operation cancelled or timed out"),
            })?;
    if status.success() {
        Ok(out)
    } else {
        let detail = String::from_utf8_lossy(&err).trim().to_owned();
        let mut failure = error(
            if detail.contains("not a git repository") {
                "not_repo"
            } else {
                "metadata_error"
            },
            detail,
        );
        failure.code = status.code();
        Err(failure)
    }
}

pub(super) fn cache_file(kind: &str, identity: &str) -> PathBuf {
    root().join("worktree-cache").join(format!(
        "{}-{:x}.json",
        kind,
        Sha256::digest(identity.as_bytes())
    ))
}
fn load(path: &Path) -> Option<Value> {
    fs::metadata(path)
        .ok()
        .filter(|m| m.len() <= 2 * MAX_OUTPUT as u64)?;
    serde_json::from_slice(&fs::read(path).ok()?).ok()
}
/// Existing recent exact-path evidence only. Directory browsing never runs Git
/// merely to suggest a project; absent/stale snapshots do not relate worktrees.
pub(super) fn cached_catalog(path: &Path) -> Option<Value> {
    let path = crate::platform::canonical(path).ok()?;
    let value = load(&cache_file("path", &path.to_string_lossy()))?;
    let age = now() - value["sampled_at"].as_f64()?;
    (value["path"].as_str() == path.to_str() && value["state"] == "ok"
        && value["stale"] == false && value["partial"] == false && (0.0..TTL).contains(&age)).then_some(value)
}
pub(super) fn canonical(path: &str) -> PathBuf {
    crate::platform::canonical(path).unwrap_or_else(|_| PathBuf::from(path))
}

fn parse(bytes: &[u8]) -> std::result::Result<(Vec<Value>, bool), GitError> {
    let mut trees = Vec::new();
    let mut row = serde_json::Map::new();
    let mut partial = false;
    for field in bytes.split(|b| *b == 0) {
        if field.is_empty() {
            if row.is_empty() {
                continue;
            }
            if !row.contains_key("path") {
                return Err(error("metadata_error", "Invalid worktree record"));
            }
            if trees.len() == MAX_TREES {
                partial = true;
                row.clear();
                break;
            }
            let path = row["path"].as_str().unwrap();
            let root = canonical(path);
            row.insert("available".into(), json!(root.is_dir()));
            row.insert("path".into(), json!(root));
            row.insert(
                "kind".into(),
                json!(if row.get("bare") == Some(&json!(true)) {
                    "bare"
                } else if trees.is_empty() {
                    "main"
                } else {
                    "linked"
                }),
            );
            trees.push(Value::Object(std::mem::take(&mut row)));
            continue;
        }
        let value = std::str::from_utf8(field)
            .map_err(|_| error("metadata_error", "Git returned a path that is not UTF-8"))?;
        let (key, value) = value.split_once(' ').unwrap_or((value, ""));
        match key {
            "worktree" => {
                if !Path::new(value).is_absolute() {
                    return Err(error(
                        "metadata_error",
                        "Git returned a relative worktree path",
                    ));
                }
                row.insert("path".into(), json!(value));
            }
            "HEAD" => {
                row.insert("head".into(), json!(value));
            }
            "branch" => {
                row.insert(
                    "branch".into(),
                    json!(value.strip_prefix("refs/heads/").unwrap_or(value)),
                );
            }
            "bare" | "detached" => {
                row.insert(key.into(), json!(true));
            }
            "locked" | "prunable" => {
                row.insert(key.into(), json!(true));
                row.insert(format!("{key}_reason"), json!(value));
            }
            _ => {}
        }
    }
    if !row.is_empty() {
        return Err(error("metadata_error", "Incomplete worktree record"));
    }
    if trees.is_empty() {
        return Err(error(
            "metadata_error",
            "Git returned an empty worktree catalog",
        ));
    }
    Ok((trees, partial))
}

/// Current checkouts of the repository at `path`, whose common directory is `common`.
pub(super) fn list(
    path: &Path,
    common: &Path,
    deadline: Instant,
) -> std::result::Result<(Vec<Value>, bool), GitError> {
    let bytes = git(path, &["worktree", "list", "--porcelain", "-z"], deadline)?;
    let (mut trees, partial) = parse(&bytes)?;
    // Git's porcelain reports the gitdir for a submodule's main
    // checkout. Resolve core.worktree through Git; never offer the
    // metadata directory itself as a working folder.
    if trees[0]["kind"] == "main" && trees[0]["path"] == json!(common) {
        let working = git(common, &["rev-parse", "--show-toplevel"], deadline)
            .ok()
            .and_then(|b| String::from_utf8(b).ok())
            .map(|s| canonical(s.trim_end_matches('\n')));
        if let Some(working) = working.filter(|p| p.is_absolute() && p != common) {
            trees[0]["available"] = json!(working.is_dir());
            trees[0]["path"] = json!(working);
        } else {
            trees[0]["available"] = json!(false);
        }
    }
    Ok((trees, partial))
}

pub(super) fn catalog(path: &Path, refresh: bool) -> Value {
    catalog_until(path, refresh, Instant::now() + Duration::from_secs(3))
}
pub(super) fn catalog_until(path: &Path, refresh: bool, deadline: Instant) -> Value {
    let requested = path.to_string_lossy();
    let path_cache = cache_file("path", &requested);
    let previous = load(&path_cache);
    let path = match crate::platform::canonical(path) {
        Ok(p) if p.is_dir() => p,
        _ => {
            return json!({"path":requested,"state":"folder_unavailable","detail":"Folder is missing or inaccessible","worktrees":[],"sampled_at":now()})
        }
    };
    let result = (|| {
        let common = git(
            &path,
            &["rev-parse", "--path-format=absolute", "--git-common-dir"],
            deadline,
        )?;
        let common = std::str::from_utf8(&common)
            .map_err(|_| error("metadata_error", "Invalid repository path"))?
            .trim_end_matches('\n');
        let common =
            crate::platform::canonical(common).map_err(|e| error("metadata_error", e.to_string()))?;
        let identity = common.to_string_lossy();
        let cache = cache_file("repo", &identity);
        let mut snapshot = load(&cache);
        let fresh = |v: &Value| {
            let age = now() - v["sampled_at"].as_f64().unwrap_or(0.);
            (0.0..TTL).contains(&age) && v["state"] == "ok"
        };
        if refresh || !snapshot.as_ref().is_some_and(fresh) {
            let _ = private_dir(cache.parent().unwrap());
            let guard = lock_file(&cache.with_extension("lock")).ok();
            let _lock = match guard {
                Some(guard) => {
                    while guard.try_lock_exclusive().is_err() {
                        if Instant::now() >= deadline {
                            return Err(error("timeout", "Catalog is being refreshed"));
                        }
                        std::thread::sleep(Duration::from_millis(10));
                    }
                    if !refresh {
                        snapshot = load(&cache);
                    }
                    Some(crate::platform::FileLock::held(guard))
                }
                None => None,
            };
            if refresh || !snapshot.as_ref().is_some_and(fresh) {
                let (trees, partial) = list(&path, &common, deadline)?;
                let value = json!({"state":"ok","common_dir":common,"worktrees":trees,"partial":partial,"sampled_at":now()});
                let _ = atomic(&cache, &value.to_string());
                snapshot = Some(value);
            }
        }
        let mut snapshot =
            snapshot.ok_or_else(|| error("metadata_error", "No worktree snapshot"))?;
        // Discovery is on the agent's machine and identifies nested cwd and symlinks.
        snapshot["selected_root"] = git(&path, &["rev-parse", "--show-toplevel"], deadline)
            .ok()
            .and_then(|b| String::from_utf8(b).ok())
            .map(|p| json!(canonical(p.trim_end_matches('\n'))))
            .unwrap_or(Value::Null);
        snapshot["path"] = json!(path);
        snapshot["stale"] = json!(false);
        Ok::<_, GitError>(snapshot)
    })();
    match result {
        Ok(snapshot) => {
            let _ = atomic(&path_cache, &snapshot.to_string());
            snapshot
        }
        Err(e) => {
            let mut value = if e.state != "not_repo" {
                previous
                    .filter(|v| v["state"] == "ok")
                    .unwrap_or(json!({"worktrees":[],"sampled_at":now()}))
            } else {
                json!({"worktrees":[],"sampled_at":now()})
            };
            value["path"] = json!(path);
            value["state"] = json!(e.state);
            value["detail"] = json!(e.detail);
            value["stale"] = json!(!value["worktrees"].as_array().is_none_or(|a| a.is_empty()));
            value
        }
    }
}

pub(super) fn dispatch(args: &[String]) -> Result<i32> {
    let action = args.first().map(String::as_str);
    if let Some(action @ ("create" | "review" | "inspect" | "remove")) = action {
        let value = match action {
            "create" => create(&args[1..])?,
            "review" => super::worktree_cleanup::review_command(&args[1..])?,
            "inspect" => super::worktree_inspection::command(&args[1..])?,
            _ => super::worktree_cleanup::remove_command(&args[1..])?,
        };
        println!("{value}");
        return Ok(0);
    }
    let mut path = None;
    let mut refresh = false;
    let mut i = 0;
    while i < args.len() {
        match args[i].as_str() {
            "--path" => {
                i += 1;
                path = Some(PathBuf::from(args.get(i).ok_or("--path needs a folder")?));
            }
            "--refresh" => refresh = true,
            "--json" => {}
            _ => return Err("usage: hgs worktrees --path PATH [--refresh] [--json]".into()),
        }
        i += 1;
    }
    let path = path.ok_or("--path is required")?;
    if !path.is_absolute() {
        return Err("worktree catalog requires an absolute folder path".into());
    }
    let mut value = catalog(&path, refresh);
    value["machine"] = json!(crate::config::Config::load()
        .map_err(|e| e.to_string())?
        .host());
    println!("{value}");
    Ok(0)
}

/// The remote upstream of the local branch a new worktree starts from, as Git's
/// `--track=inherit` would copy it, so the new branch's publication can be
/// verified. Commits, tags, detached HEAD and local upstreams have none. Under
/// push.default=upstream or tracking a bare `git push` from the new branch
/// would update the base branch, so it keeps no upstream there either.
fn inherited_upstream(read: &dyn Fn(&[&str]) -> Result<String>, base: &str) -> Option<(String, String)> {
    let full = read(&["rev-parse", "--verify", "--symbolic-full-name", "--end-of-options", base]).ok()?;
    let name = full.strip_prefix("refs/heads/")?;
    let remote = read(&["config", "--get", &format!("branch.{name}.remote")]).ok()?;
    let merge = read(&["config", "--get", &format!("branch.{name}.merge")]).ok()?;
    let push = read(&["config", "--get", "push.default"]).unwrap_or_default();
    if remote.is_empty() || remote == "." || !merge.starts_with("refs/heads/") || ["upstream", "tracking"].contains(&push.as_str()) {
        return None;
    }
    Some((remote, merge))
}

fn create(args: &[String]) -> Result<Value> {
    let mobile = args.iter().any(|value| value == "--mobile");
    let _signals = mobile.then(MobileSignals::install);
    let total_deadline = Instant::now() + Duration::from_secs(if mobile { 25 } else { 315 });
    let mut options = BTreeMap::new();
    let mut i = 0;
    while i < args.len() {
        match args[i].as_str() {
            "--json" | "--mobile" => {}
            key @ ("--path" | "--destination" | "--branch" | "--base" | "--common-dir" | "--request-id") => {
                i += 1;
                let value = args.get(i).ok_or_else(|| format!("{key} needs a value"))?;
                if value.is_empty() || value.chars().any(char::is_control) {
                    return Err(format!("{key} cannot be empty or contain control characters"));
                }
                if options.insert(key, value.as_str()).is_some() { return Err(format!("duplicate {key}")); }
            }
            _ => return Err("usage: hgs worktrees create --path REPO --destination FOLDER --branch NEW_BRANCH [--base HEAD] [--json]".into()),
        }
        i += 1;
    }
    let required = |key| {
        options
            .get(key)
            .copied()
            .ok_or_else(|| format!("{key} is required"))
    };
    let source = Path::new(required("--path")?);
    let destination = Path::new(required("--destination")?);
    if !source.is_absolute() || !destination.is_absolute() {
        return Err("Repository and destination must be absolute folder paths".into());
    }
    let source = crate::platform::canonical(source).map_err(|e| format!("Repository: {e}"))?;
    let parent = crate::platform::canonical(destination.parent().ok_or("Choose a destination folder")?)
        .map_err(|e| format!("Destination parent must already exist: {e}"))?;
    let destination = parent.join(
        destination
            .file_name()
            .ok_or("Choose a destination folder")?,
    );
    if fs::symlink_metadata(&destination).is_ok() {
        return Err("Destination already exists. Choose a new folder.".into());
    }
    let deadline = total_deadline.min(Instant::now() + Duration::from_secs(5));
    let read = |args: &[&str]| -> Result<String> {
        String::from_utf8(git(&source, args, deadline).map_err(|e| e.detail)?)
            .map(|s| s.trim_end_matches('\n').to_owned())
            .map_err(|e| e.to_string())
    };
    let common = crate::platform::canonical(read(&[
        "rev-parse",
        "--path-format=absolute",
        "--git-common-dir",
    ])?)
    .map_err(|e| e.to_string())?;
    if let Some(expected) = options.get("--common-dir") {
        if crate::platform::canonical(expected).ok().as_ref() != Some(&common) {
            return Err("Repository changed. Refresh its worktrees before creating one.".into());
        }
    }
    if destination.starts_with(&common) {
        return Err("Destination cannot be inside Git metadata".into());
    }
    let branch = required("--branch")?;
    if branch.starts_with('-') {
        return Err("Branch name cannot start with '-'".into());
    }
    let reference = format!("refs/heads/{branch}");
    read(&["check-ref-format", &reference]).map_err(|_| "Invalid new branch name".to_string())?;
    read(&["check-ref-format", "--branch", branch])
        .map_err(|_| "Invalid new branch name".to_string())?;
    if read(&["for-each-ref", "--format=%(refname)", "refs/heads"])?
        .lines()
        .any(|r| {
            r == reference
                || r.starts_with(&format!("{reference}/"))
                || reference.starts_with(&format!("{r}/"))
        })
    {
        return Err(format!(
            "Branch '{branch}' already exists or conflicts with an existing branch. Choose a new branch name."
        ));
    }
    let base = options.get("--base").copied().unwrap_or("HEAD");
    let head = read(&[
        "rev-parse",
        "--verify",
        "--end-of-options",
        &format!("{base}^{{commit}}"),
    ])
    .map_err(|e| format!("Cannot resolve starting revision '{base}': {e}"))?;
    let upstream = inherited_upstream(&read, base);
    let cache = cache_file("repo", &common.to_string_lossy());
    fs::create_dir_all(cache.parent().unwrap()).map_err(|e| e.to_string())?;
    let guard = crate::platform::private_open(
        OpenOptions::new().create(true).truncate(false).read(true).write(true),
    )
    .open(cache.with_extension("lock"))
    .map_err(|e| e.to_string())?;
    while guard.try_lock_exclusive().is_err() {
        if Instant::now() >= deadline {
            return Err(
                "Another worktree operation is running. Try again after it finishes.".into(),
            );
        }
        std::thread::sleep(Duration::from_millis(10));
    }
    let _lock = crate::platform::FileLock::held(guard);
    // Atomic reservation rejects simultaneous creators and existing/symlinked
    // destinations. Git accepts this empty directory but never overwrites files.
    if mobile_stopped() || Instant::now() >= total_deadline { return Err("Worktree creation cancelled before checkout".into()); }
    fs::create_dir(&destination).map_err(|e| format!("Cannot reserve destination: {e}"))?;
    let outcome = git(
        &source,
        &[
            "worktree",
            "add",
            "--quiet",
            "-b",
            branch,
            "--",
            &destination.to_string_lossy(),
            &head,
        ],
        total_deadline.min(Instant::now() + Duration::from_secs(if mobile { 15 } else { 300 })),
    );
    let _ = fs::remove_file(cache);
    outcome.map_err(|e| format!("{}\nCheck {} and branch '{branch}' before retrying. Any created files and branch have been kept.", e.detail, destination.display()))?;
    // Confirm the resulting checkout, not merely the exit code (hooks may fail
    // after creation, or another process may have changed the destination).
    let deadline = total_deadline.min(Instant::now() + Duration::from_secs(5));
    let actual = git(
        &destination,
        &["rev-parse", "--path-format=absolute", "--git-common-dir"],
        deadline,
    )
    .map_err(|e| e.detail)?;
    let actual = crate::platform::canonical(String::from_utf8_lossy(&actual).trim_end_matches('\n'))
        .map_err(|e| e.to_string())?;
    let actual_branch =
        git(&destination, &["symbolic-ref", "--quiet", "HEAD"], deadline).map_err(|e| e.detail)?;
    if actual != common
        || String::from_utf8_lossy(&actual_branch).trim_end_matches('\n') != reference
    {
        return Err(
            "Worktree creation could not be confirmed. Inspect the destination before retrying."
                .into(),
        );
    }
    // Best effort: the checkout exists either way, and a missing upstream only
    // leaves publication unverified. Never keep half of the tracking pair.
    let upstream = upstream.filter(|(remote, merge)| {
        let set = |key: &str, value: &str| {
            git(&destination, &["config", &format!("branch.{branch}.{key}"), value], deadline).is_ok()
        };
        let complete = set("remote", remote) && set("merge", merge);
        if !complete {
            let _ = git(&destination, &["config", "--unset", &format!("branch.{branch}.remote")], deadline);
        }
        complete
    });
    let upstream = upstream.map(|(remote, merge)| format!("{remote}/{}", merge.strip_prefix("refs/heads/").unwrap_or(&merge)));
    let managed = super::worktree_lifecycle::created(&common, &destination, branch, &head);
    Ok(
        json!({"status":"created", "path":destination, "branch":branch, "base":base, "head":head,
        "upstream":upstream, "common_dir":common, "request_id":options.get("--request-id"),
        "managed_cleanup":managed.is_ok(), "cleanup_note":managed.err(),
        "machine":crate::config::Config::load().map_err(|e|e.to_string())?.host()}),
    )
}
