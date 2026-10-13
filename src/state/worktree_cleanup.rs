//! Review and guarded removal of linked worktrees. Removal repeats every check
//! under the repository lock used by creation, compares the fingerprint the
//! person reviewed and runs only `git worktree remove` without force.
use super::worktrees::{cache_file, canonical, error, git, list, GitError};
use super::*;
use fs2::FileExt;
use sha2::{Digest, Sha256};
use std::fs::{self, OpenOptions};
use std::os::unix::fs::OpenOptionsExt;
use std::time::{Duration, Instant, UNIX_EPOCH};

const MAX_REVIEWED: usize = 64;
const RECENT: f64 = 24.0 * 3600.0;
const WALK_ENTRIES: usize = 200_000;
const WALK_TIME: Duration = Duration::from_secs(2);
const MAX_IGNORED: usize = 20;
const MAX_SAMPLES: usize = 5;

/// Folders a Zerus session record refers to.
struct Reference {
    name: String,
    folders: Vec<PathBuf>,
}

struct Context {
    /// (display name, full ref) of the main checkout's branch and origin/HEAD.
    bases: Vec<(String, String)>,
    processes: Vec<(u32, PathBuf, String)>,
    bindings: Vec<Reference>,
    archives: Vec<Reference>,
}

fn reason(code: &str, message: impl Into<String>) -> Value {
    json!({"code":code,"message":message.into()})
}

fn plural(count: usize, word: &str) -> String {
    let suffix = match (count, word.ends_with('s')) {
        (1, _) => "",
        (_, true) => "es",
        _ => "s",
    };
    format!("{count} {word}{suffix}")
}

fn text(bytes: Vec<u8>) -> String {
    String::from_utf8_lossy(&bytes).trim_end_matches('\n').to_owned()
}

fn step(total: Instant, seconds: u64) -> Instant {
    total.min(Instant::now() + Duration::from_secs(seconds))
}

/// Missing folders keep their spelling; an existing parent is resolved so that
/// the same checkout compares equal through symlinked parents.
fn resolved(path: &Path) -> PathBuf {
    if let Ok(path) = fs::canonicalize(path) {
        return path;
    }
    match (path.parent().and_then(|p| fs::canonicalize(p).ok()), path.file_name()) {
        (Some(parent), Some(name)) => parent.join(name),
        _ => path.to_path_buf(),
    }
}

fn inside(folder: &Path, tree: &Path) -> bool {
    folder.starts_with(tree)
}

#[cfg(target_os = "linux")]
fn processes() -> Vec<(u32, PathBuf, String)> {
    let own = std::process::id();
    let Ok(entries) = fs::read_dir("/proc") else {
        return Vec::new();
    };
    entries
        .flatten()
        .filter_map(|entry| {
            let pid: u32 = entry.file_name().to_str()?.parse().ok()?;
            if pid == own {
                return None;
            }
            let cwd = fs::read_link(entry.path().join("cwd")).ok()?;
            let name = fs::read_to_string(entry.path().join("comm"))
                .map(|s| s.trim().to_owned())
                .unwrap_or_default();
            Some((pid, cwd, name))
        })
        .collect()
}

#[cfg(target_os = "macos")]
fn processes() -> Vec<(u32, PathBuf, String)> {
    use std::os::unix::ffi::OsStrExt;
    let own = std::process::id();
    let count = unsafe { libc::proc_listallpids(std::ptr::null_mut(), 0) };
    if count <= 0 {
        return Vec::new();
    }
    let mut pids = vec![0 as libc::c_int; count as usize + 64];
    let size = (pids.len() * std::mem::size_of::<libc::c_int>()) as libc::c_int;
    let found = unsafe { libc::proc_listallpids(pids.as_mut_ptr() as *mut libc::c_void, size) };
    if found <= 0 {
        return Vec::new();
    }
    pids.truncate((found as usize).min(pids.len()));
    pids.into_iter()
        .filter(|pid| *pid > 0 && *pid as u32 != own)
        .filter_map(|pid| {
            let mut info: libc::proc_vnodepathinfo = unsafe { std::mem::zeroed() };
            let size = std::mem::size_of_val(&info) as libc::c_int;
            let read = unsafe {
                libc::proc_pidinfo(
                    pid,
                    libc::PROC_PIDVNODEPATHINFO,
                    0,
                    &mut info as *mut _ as *mut libc::c_void,
                    size,
                )
            };
            if read != size {
                return None;
            }
            let path = &info.pvi_cdir.vip_path;
            let bytes = unsafe {
                std::slice::from_raw_parts(path.as_ptr() as *const u8, std::mem::size_of_val(path))
            };
            let end = bytes.iter().position(|b| *b == 0)?;
            if end == 0 {
                return None;
            }
            let cwd = PathBuf::from(std::ffi::OsStr::from_bytes(&bytes[..end]));
            let mut name = [0u8; 256];
            let length = unsafe {
                libc::proc_name(pid, name.as_mut_ptr() as *mut libc::c_void, name.len() as u32)
            };
            let name = String::from_utf8_lossy(&name[..length.clamp(0, 256) as usize]).into_owned();
            Some((pid as u32, cwd, name))
        })
        .collect()
}

#[cfg(not(any(target_os = "linux", target_os = "macos")))]
fn processes() -> Vec<(u32, PathBuf, String)> {
    Vec::new()
}

fn reference(record: &Value) -> Reference {
    let folders = ["cwd", "launch_dir"]
        .iter()
        .map(|key| string(record, key))
        .filter(|folder| Path::new(folder).is_absolute())
        .map(canonical)
        .collect();
    Reference {
        name: string(record, "name").to_owned(),
        folders,
    }
}

fn deepseek_bindings() -> Vec<Reference> {
    let Ok(files) = fs::read_dir(root().join("dsh/bindings")) else {
        return Vec::new();
    };
    files
        .flatten()
        .take(5000)
        .filter(|file| file.path().extension().is_some_and(|e| e == "json"))
        .filter_map(|file| serde_json::from_slice::<Value>(&fs::read(file.path()).ok()?).ok())
        .filter(|record| record["backend"] == "dsh")
        .map(|record| reference(&record))
        .collect()
}

fn context(common: &Path, trees: &[Value], deadline: Instant) -> Context {
    let mut bases = Vec::new();
    if trees[0]["kind"] == "main" {
        if let Some(branch) = trees[0]["branch"].as_str().filter(|b| !b.is_empty()) {
            bases.push((branch.to_owned(), format!("refs/heads/{branch}")));
        }
    }
    if let Ok(origin) = git(
        common,
        &["symbolic-ref", "--quiet", "refs/remotes/origin/HEAD"],
        deadline,
    ) {
        let origin = text(origin);
        if origin.starts_with("refs/remotes/") {
            let name = origin.trim_start_matches("refs/remotes/").to_owned();
            bases.push((name, origin));
        }
    }
    let mut bindings: Vec<Reference> = storage::records()
        .unwrap_or_default()
        .iter()
        .map(reference)
        .collect();
    bindings.extend(deepseek_bindings());
    Context {
        bases,
        processes: processes(),
        bindings,
        archives: archive::records()
            .unwrap_or_default()
            .iter()
            .map(reference)
            .collect(),
    }
}

/// `Some(true)` when `commit` is reachable from `base`, `None` when Git failed.
fn ancestor(cwd: &Path, commit: &str, base: &str, deadline: Instant) -> Option<bool> {
    match git(cwd, &["merge-base", "--is-ancestor", commit, base], deadline) {
        Ok(_) => Some(true),
        Err(e) if e.state == "metadata_error" && e.detail.is_empty() => Some(false),
        Err(_) => None,
    }
}

#[derive(Default)]
struct Status {
    changes: usize,
    untracked: usize,
    ignored: Vec<String>,
    entries: Vec<String>,
}

fn status(tree: &Path, deadline: Instant) -> std::result::Result<Status, GitError> {
    let bytes = git(
        tree,
        &[
            "status",
            "--porcelain=v1",
            "-z",
            "--ignored",
            "--untracked-files=normal",
            "--ignore-submodules=none",
        ],
        deadline,
    )?;
    let mut result = Status::default();
    let mut fields = bytes.split(|b| *b == 0).filter(|f| !f.is_empty());
    while let Some(field) = fields.next() {
        if field.len() < 4 {
            return Err(error("metadata_error", "Invalid Git status entry"));
        }
        let path = String::from_utf8_lossy(&field[3..]).into_owned();
        match &field[..2] {
            b"!!" => result.ignored.push(path.trim_end_matches('/').to_owned()),
            b"??" => result.untracked += 1,
            code => {
                result.changes += 1;
                // Renames and copies are followed by their original path.
                if matches!(code[0], b'R' | b'C') {
                    fields.next();
                }
            }
        }
        result.entries.push(String::from_utf8_lossy(field).into_owned());
    }
    Ok(result)
}

/// Apparent size of an ignored entry, without following symlinks.
fn size(path: &Path, entries: &mut usize, deadline: Instant) -> (u64, bool) {
    let mut total = 0;
    let mut pending = vec![path.to_path_buf()];
    while let Some(path) = pending.pop() {
        *entries += 1;
        if *entries > WALK_ENTRIES || Instant::now() >= deadline {
            return (total, false);
        }
        let Ok(metadata) = fs::symlink_metadata(&path) else {
            continue;
        };
        if metadata.is_dir() {
            if let Ok(children) = fs::read_dir(&path) {
                pending.extend(children.flatten().map(|child| child.path()));
            }
        } else {
            total += metadata.len();
        }
    }
    (total, true)
}

fn modified(path: &Path) -> f64 {
    fs::metadata(path)
        .and_then(|m| m.modified())
        .ok()
        .and_then(|time| time.duration_since(UNIX_EPOCH).ok())
        .map(|d| d.as_secs_f64())
        .unwrap_or(0.)
}

fn fingerprint(material: Value) -> String {
    format!("{:x}", Sha256::digest(material.to_string().as_bytes()))
}

fn finish(mut row: Value, reasons: Vec<Value>, notes: Vec<Value>) -> Value {
    row["verdict"] = json!(if !reasons.is_empty() {
        "blocked"
    } else if row["available"] == false {
        "missing"
    } else if !notes.is_empty() {
        "review"
    } else {
        "ready"
    });
    row["reasons"] = json!(reasons);
    row["notes"] = json!(notes);
    row
}

fn assess(tree: &Value, trees: &[Value], context: &Context, total: Instant) -> Value {
    let path = PathBuf::from(tree["path"].as_str().unwrap_or(""));
    let head = string(tree, "head").to_owned();
    let branch = string(tree, "branch").to_owned();
    let mut row = tree.clone();
    for key in ["locked", "prunable", "detached"] {
        row[key] = json!(tree[key] == true);
    }
    row["fingerprint"] = json!("");
    match string(tree, "kind") {
        "main" => {
            return finish(
                row,
                vec![reason(
                    "main_checkout",
                    "This is the main checkout of the repository.",
                )],
                vec![],
            )
        }
        "bare" => {
            return finish(
                row,
                vec![reason("bare", "A bare repository has no working folder.")],
                vec![],
            )
        }
        _ => {}
    }
    let mut reasons = Vec::new();
    let mut notes = Vec::new();
    if tree["locked"] == true {
        let why = string(tree, "locked_reason");
        reasons.push(reason(
            "locked",
            if why.is_empty() {
                "Locked by Git.".to_owned()
            } else {
                format!("Locked by Git: {why}")
            },
        ));
    }
    if tree["available"] != true {
        row["fingerprint"] = json!(fingerprint(json!({"path":path,"head":head,"branch":branch,
            "locked":row["locked"],"prunable":row["prunable"],"available":false})));
        return finish(row, reasons, notes);
    }
    if Instant::now() >= total {
        reasons.push(reason(
            "status_unavailable",
            "The review ran out of time before this worktree.",
        ));
        return finish(row, reasons, notes);
    }

    let samples: Vec<_> = context
        .processes
        .iter()
        .filter(|(_, cwd, _)| inside(cwd, &path))
        .collect();
    row["processes"] = json!(samples
        .iter()
        .take(MAX_SAMPLES)
        .map(|(pid, _, name)| json!({"pid":pid,"command":name}))
        .collect::<Vec<_>>());
    if !samples.is_empty() {
        let names: Vec<_> = samples
            .iter()
            .take(MAX_SAMPLES)
            .map(|(pid, _, name)| format!("{name} ({pid})"))
            .collect();
        reasons.push(reason(
            "processes",
            format!(
                "{} use this folder: {}. Close them first.",
                plural(samples.len(), "running process"),
                names.join(", ")
            ),
        ));
    }
    let uses = |references: &[Reference]| -> Vec<String> {
        let mut names: Vec<String> = references
            .iter()
            .filter(|r| r.folders.iter().any(|folder| inside(folder, &path)))
            .map(|r| r.name.clone())
            .collect();
        names.sort();
        names.dedup();
        names
    };
    let sessions = uses(&context.bindings);
    if !sessions.is_empty() {
        reasons.push(reason(
            "sessions",
            format!(
                "Zerus sessions use this folder: {}. Archive them first.",
                sessions.join(", ")
            ),
        ));
    }
    row["sessions"] = json!(sessions);
    let archived = uses(&context.archives).len();
    row["archived_sessions"] = json!(archived);

    let mut nested: Vec<String> = trees
        .iter()
        .filter_map(|other| other["path"].as_str())
        .filter(|other| Path::new(other) != path && inside(Path::new(other), &path))
        .map(str::to_owned)
        .collect();
    nested.sort();
    if !nested.is_empty() {
        reasons.push(reason(
            "nested",
            format!("Contains a nested worktree: {}", nested.join(", ")),
        ));
    }
    row["nested"] = json!(nested);

    let status = match status(&path, step(total, 5)) {
        Ok(status) => status,
        Err(e) => {
            reasons.push(reason(
                "status_unavailable",
                format!("Git status is unavailable: {}", e.detail),
            ));
            return finish(row, reasons, notes);
        }
    };
    row["changes"] = json!(status.changes);
    row["untracked"] = json!(status.untracked);
    if status.changes > 0 {
        reasons.push(reason(
            "changes",
            format!("{} not committed.", plural(status.changes, "changed file")),
        ));
    }
    if status.untracked > 0 {
        reasons.push(reason(
            "untracked",
            format!("{}.", plural(status.untracked, "untracked file")),
        ));
    }

    let metadata = git(
        &path,
        &["rev-parse", "--absolute-git-dir", "--git-path", "modules"],
        step(total, 5),
    )
    .map(text)
    .unwrap_or_default();
    let mut lines = metadata.lines().map(PathBuf::from);
    let gitdir = lines.next();
    let modules = lines.next().map(|m| if m.is_absolute() { m } else { path.join(m) });
    if modules.is_some_and(|m| fs::read_dir(m).is_ok_and(|mut entries| entries.next().is_some())) {
        reasons.push(reason(
            "submodules",
            "Has initialized submodules; Git would require force.",
        ));
    }
    let activity = gitdir
        .map(|dir| {
            ["HEAD", "index", "logs/HEAD"]
                .iter()
                .map(|name| modified(&dir.join(name)))
                .fold(0., f64::max)
        })
        .unwrap_or(0.);
    row["last_activity"] = json!(activity);

    if tree["detached"] == true && !head.is_empty() {
        match git(
            &path,
            &[
                "for-each-ref",
                "--count=1",
                "--format=%(refname)",
                "--contains",
                &head,
                "refs/heads",
                "refs/remotes",
                "refs/tags",
            ],
            step(total, 5),
        ) {
            Ok(found) if found.iter().all(u8::is_ascii_whitespace) => reasons.push(reason(
                "detached_unreachable",
                "The detached commit is not on any branch, tag or remote branch.",
            )),
            Ok(_) => {}
            Err(e) => reasons.push(reason(
                "status_unavailable",
                format!("Cannot check the detached commit: {}", e.detail),
            )),
        }
    }

    let mut merged = None;
    for (_, base) in &context.bases {
        match ancestor(&path, &head, base, step(total, 5)) {
            Some(true) => {
                merged = Some(true);
                break;
            }
            Some(false) => merged = Some(false),
            None => {}
        }
    }
    row["merged"] = json!(merged);
    row["ahead"] = context
        .bases
        .first()
        .and_then(|(_, base)| {
            git(
                &path,
                &["rev-list", "--count", &format!("{base}..{head}")],
                step(total, 5),
            )
            .ok()
        })
        .and_then(|count| text(count).parse::<u64>().ok())
        .map_or(Value::Null, |count| json!(count));
    let upstream = if branch.is_empty() {
        None
    } else {
        git(
            &path,
            &["rev-parse", "--symbolic-full-name", "@{upstream}"],
            step(total, 5),
        )
        .ok()
        .map(text)
        .filter(|u| !u.is_empty())
    };
    row["pushed"] = json!(upstream
        .as_ref()
        .and_then(|u| ancestor(&path, &head, u, step(total, 5))));
    row["upstream"] = json!(upstream.map(|u| u.trim_start_matches("refs/remotes/").to_owned()));

    let deadline = (Instant::now() + WALK_TIME).min(total);
    let mut entries = 0;
    let mut ignored: Vec<(String, u64, bool)> = status
        .ignored
        .iter()
        .map(|name| {
            let (bytes, complete) = size(&path.join(name), &mut entries, deadline);
            (name.clone(), bytes, complete)
        })
        .collect();
    row["ignored_bytes"] = json!(ignored.iter().map(|(_, bytes, _)| bytes).sum::<u64>());
    row["ignored_complete"] = json!(ignored.iter().all(|(_, _, complete)| *complete));
    row["ignored_count"] = json!(ignored.len());
    ignored.sort_by(|a, b| b.1.cmp(&a.1).then_with(|| a.0.cmp(&b.0)));
    row["ignored"] = json!(ignored
        .iter()
        .take(MAX_IGNORED)
        .map(|(name, bytes, complete)| json!({"path":name,"bytes":bytes,"complete":complete}))
        .collect::<Vec<_>>());

    let base = context
        .bases
        .first()
        .map_or("the base branch", |(name, _)| name.as_str());
    match merged {
        Some(false) => notes.push(reason(
            "not_merged",
            match row["ahead"].as_u64() {
                Some(count) => format!(
                    "{} not in {base}. Removing the worktree keeps the branch.",
                    plural(count as usize, "commit")
                ),
                None => format!("Has commits not in {base}. Removing the worktree keeps the branch."),
            },
        )),
        None if context.bases.is_empty() => notes.push(reason(
            "no_base",
            "No base branch was found, so the merge state is unknown.",
        )),
        _ => {}
    }
    if archived > 0 {
        notes.push(reason(
            "archived_sessions",
            format!(
                "{} refer to this folder and could not be restored there.",
                plural(archived, "archived session")
            ),
        ));
    }
    if now() - activity < RECENT {
        notes.push(reason("recent", "Used in the last 24 hours."));
    }
    row["fingerprint"] = json!(fingerprint(json!({"path":path,"head":head,"branch":branch,
        "locked":row["locked"],"prunable":row["prunable"],"available":true,
        "status":status.entries,"nested":row["nested"],"sessions":row["sessions"]})));
    finish(row, reasons, notes)
}

/// Assessment of the checkouts of the repository containing `path`; `only`
/// limits it to one worktree, which may already be missing.
fn review(path: &Path, only: Option<&Path>, total: Instant) -> Value {
    let machine = crate::config::Config::load()
        .map(|c| c.host().to_owned())
        .unwrap_or_default();
    let Ok(path) = fs::canonicalize(path).map_err(|_| ()).and_then(|p| {
        if p.is_dir() {
            Ok(p)
        } else {
            Err(())
        }
    }) else {
        return json!({"path":path,"state":"folder_unavailable","detail":"Folder is missing or inaccessible",
            "worktrees":[],"machine":machine,"sampled_at":now()});
    };
    let result = (|| {
        let common = git(
            &path,
            &["rev-parse", "--path-format=absolute", "--git-common-dir"],
            step(total, 3),
        )?;
        let common = fs::canonicalize(text(common)).map_err(|e| error("metadata_error", e.to_string()))?;
        let (trees, partial) = list(&path, &common, step(total, 3))?;
        let context = context(&common, &trees, step(total, 3));
        let wanted = only.map(|only| (only.to_path_buf(), resolved(only)));
        let mut reviewed = 0;
        let mut cut = false;
        let mut rows = Vec::new();
        for tree in &trees {
            let tree_path = Path::new(tree["path"].as_str().unwrap_or(""));
            if let Some((raw, resolved)) = &wanted {
                if tree_path != raw && tree_path != resolved {
                    continue;
                }
            }
            if tree["kind"] == "linked" {
                reviewed += 1;
                if reviewed > MAX_REVIEWED {
                    cut = true;
                    let mut row = tree.clone();
                    row["fingerprint"] = json!("");
                    rows.push(finish(
                        row,
                        vec![reason(
                            "status_unavailable",
                            "Not reviewed: too many worktrees.",
                        )],
                        vec![],
                    ));
                    continue;
                }
            }
            rows.push(assess(tree, &trees, &context, total));
        }
        let base: Vec<_> = context.bases.iter().map(|(name, _)| name.clone()).collect();
        Ok::<_, GitError>(json!({"state":"ok","path":path,"common_dir":common,"base":base,
            "partial":partial || cut,"worktrees":rows,"machine":machine,"sampled_at":now()}))
    })();
    result.unwrap_or_else(|e| {
        json!({"path":path,"state":e.state,"detail":e.detail,"worktrees":[],"machine":machine,"sampled_at":now()})
    })
}

pub(super) fn review_command(args: &[String]) -> Result<Value> {
    let mut path = None;
    let mut only = None;
    let mut i = 0;
    while i < args.len() {
        match args[i].as_str() {
            "--json" => {}
            key @ ("--path" | "--worktree") => {
                i += 1;
                let value = PathBuf::from(args.get(i).ok_or_else(|| format!("{key} needs a folder"))?);
                if !value.is_absolute() {
                    return Err(format!("{key} must be an absolute folder path"));
                }
                if key == "--path" {
                    path = Some(value);
                } else {
                    only = Some(value);
                }
            }
            _ => {
                return Err(
                    "usage: hgs worktrees review --path PATH [--worktree WORKTREE] [--json]".into(),
                )
            }
        }
        i += 1;
    }
    let path = path.ok_or("--path is required")?;
    Ok(review(
        &path,
        only.as_deref(),
        Instant::now() + Duration::from_secs(20),
    ))
}

pub(super) fn remove_command(args: &[String]) -> Result<Value> {
    const USAGE: &str = "usage: hgs worktrees remove --path WORKTREE --common-dir DIR --fingerprint HASH [--delete-branch] [--request-id ID] [--json]";
    let mut options = BTreeMap::new();
    let mut delete_branch = false;
    let mut i = 0;
    while i < args.len() {
        match args[i].as_str() {
            "--json" => {}
            "--delete-branch" => delete_branch = true,
            "--dry-run" => return Err("worktree removal does not support --dry-run".into()),
            key @ ("--path" | "--common-dir" | "--fingerprint" | "--request-id") => {
                i += 1;
                let value = args.get(i).ok_or_else(|| format!("{key} needs a value"))?;
                if value.is_empty() || value.chars().any(char::is_control) {
                    return Err(format!("{key} cannot be empty or contain control characters"));
                }
                if options.insert(key, value.as_str()).is_some() {
                    return Err(format!("duplicate {key}"));
                }
            }
            _ => return Err(USAGE.into()),
        }
        i += 1;
    }
    let required = |key| options.get(key).copied().ok_or_else(|| format!("{key} is required"));
    let target = Path::new(required("--path")?);
    let common = Path::new(required("--common-dir")?);
    let expected = required("--fingerprint")?;
    if !target.is_absolute() || !common.is_absolute() {
        return Err("Worktree and repository must be absolute paths".into());
    }
    let common = fs::canonicalize(common).map_err(|e| format!("Repository: {e}"))?;
    let changed = "Repository changed or this folder is not one of its worktrees. Review the worktrees again.";
    let actual = git(
        &common,
        &["rev-parse", "--path-format=absolute", "--git-common-dir"],
        Instant::now() + Duration::from_secs(5),
    )
    .map_err(|e| e.detail)?;
    if fs::canonicalize(text(actual)).ok().as_ref() != Some(&common) {
        return Err(changed.into());
    }
    if target.exists() {
        let own = git(
            target,
            &["rev-parse", "--path-format=absolute", "--git-common-dir"],
            Instant::now() + Duration::from_secs(5),
        )
        .map(text)
        .ok()
        .and_then(|own| fs::canonicalize(own).ok());
        if own.as_ref() != Some(&common) {
            return Err(changed.into());
        }
    }

    let cache = cache_file("repo", &common.to_string_lossy());
    private_dir(cache.parent().unwrap())?;
    let guard = OpenOptions::new()
        .create(true)
        .truncate(false)
        .read(true)
        .write(true)
        .mode(0o600)
        .open(cache.with_extension("lock"))
        .map_err(|e| e.to_string())?;
    let waited = Instant::now() + Duration::from_secs(10);
    while guard.try_lock_exclusive().is_err() {
        if Instant::now() >= waited {
            return Err("Another worktree operation is running. Try again after it finishes.".into());
        }
        std::thread::sleep(Duration::from_millis(10));
    }
    let lock = crate::platform::FileLock::held(guard);

    let data = review(
        &common,
        Some(target),
        Instant::now() + Duration::from_secs(20),
    );
    if data["state"] != "ok" {
        return Err(format!(
            "Cannot review the repository: {}",
            string(&data, "detail")
        ));
    }
    let row = data["worktrees"]
        .as_array()
        .and_then(|rows| rows.first())
        .cloned()
        .ok_or(changed)?;
    let path = PathBuf::from(string(&row, "path"));
    if row["kind"] != "linked" {
        return Err("Only linked worktrees can be removed; the main checkout stays.".into());
    }
    if row["verdict"] == "blocked" {
        let reasons: Vec<_> = row["reasons"]
            .as_array()
            .into_iter()
            .flatten()
            .map(|r| string(r, "message").to_owned())
            .collect();
        return Err(format!(
            "Cannot remove {}: {}",
            path.display(),
            reasons.join(" ")
        ));
    }
    if string(&row, "fingerprint") != expected {
        return Err("Worktree changed since the review. Review it again.".into());
    }
    let missing = row["verdict"] == "missing";
    git(
        &common,
        &["worktree", "remove", "--", &path.to_string_lossy()],
        Instant::now() + Duration::from_secs(300),
    )
    .map_err(|e| {
        format!(
            "{}\nNothing was forced. Review the worktree again before retrying.",
            e.detail
        )
    })?;
    let _ = fs::remove_file(&cache);
    let (trees, _) = list(&common, &common, Instant::now() + Duration::from_secs(5))
        .map_err(|e| format!("Removal could not be confirmed: {}", e.detail))?;
    if trees.iter().any(|tree| tree["path"] == json!(path)) || (!missing && path.exists()) {
        return Err("Removal could not be confirmed. Review the worktree again.".into());
    }

    let branch = string(&row, "branch").to_owned();
    let mut deleted = false;
    let mut branch_error = Value::Null;
    if delete_branch && !branch.is_empty() {
        if row["merged"] != true {
            let base = data["base"][0].as_str().unwrap_or("the base branch");
            branch_error = json!(format!(
                "Branch '{branch}' is not merged into {base}; it was kept."
            ));
        } else {
            match git(
                &common,
                &["branch", "-d", "--", &branch],
                Instant::now() + Duration::from_secs(10),
            ) {
                Ok(_) => deleted = true,
                Err(e) => branch_error = json!(e.detail),
            }
        }
    }
    drop(lock);
    Ok(json!({"status":if missing {"forgotten"} else {"removed"},"path":path,"branch":branch,
        "branch_deleted":deleted,"branch_error":branch_error,"common_dir":common,
        "request_id":options.get("--request-id"),"machine":data["machine"]}))
}
