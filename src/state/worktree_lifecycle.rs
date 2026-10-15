//! Proven Zerus ownership and conservative cleanup at an explicit lifecycle exit.
//! A private receipt and a nonce in Git's per-checkout metadata must both match;
//! names and folder prefixes never establish ownership.
use super::worktrees::{cache_file, git};
use super::*;
use fs2::FileExt;
use std::fs::{self, OpenOptions};
use std::time::{Duration, Instant};

fn receipt_path(common: &Path, path: &Path) -> PathBuf {
    cache_file(
        "owned",
        &format!("{}\n{}", common.display(), path.display()),
    )
}

pub(super) fn created(common: &Path, path: &Path, branch: &str, head: &str) -> Result<()> {
    let metadata = git(
        path,
        &["rev-parse", "--absolute-git-dir"],
        Instant::now() + Duration::from_secs(3),
    )
    .map_err(|e| e.detail)?;
    let metadata = crate::platform::canonical(String::from_utf8_lossy(&metadata).trim())
        .map_err(|e| e.to_string())?;
    if !metadata.starts_with(common.join("worktrees")) {
        return Err("Cannot verify linked worktree metadata".into());
    }
    let value = json!({"version":1,"id":uuid::Uuid::new_v4().to_string(),"common_dir":common,"path":path,"branch":branch,"head":head,"metadata":metadata,"created_at":now()});
    atomic(&metadata.join("zerus-owner.json"), &format!("{value}\n"))?;
    atomic(&receipt_path(common, path), &format!("{value}\n"))
}

pub(super) fn owned(common: &Path, path: &Path) -> Option<Value> {
    let value: Value = serde_json::from_slice(&fs::read(receipt_path(common, path)).ok()?).ok()?;
    if value["version"] != 1
        || value["common_dir"] != json!(common)
        || value["path"] != json!(path)
        || uuid::Uuid::parse_str(value["id"].as_str()?).is_err()
    {
        return None;
    }
    let metadata = Path::new(value["metadata"].as_str()?);
    if !metadata.starts_with(common.join("worktrees"))
        || crate::platform::canonical(metadata).ok()?.as_path() != metadata
    {
        return None;
    }
    let marker: Value =
        serde_json::from_slice(&fs::read(metadata.join("zerus-owner.json")).ok()?).ok()?;
    if marker != value {
        return None;
    }
    let actual = git(
        path,
        &["rev-parse", "--absolute-git-dir"],
        Instant::now() + Duration::from_secs(3),
    )
    .ok()?;
    (crate::platform::canonical(String::from_utf8_lossy(&actual).trim())
        .ok()?
        .as_path()
        == metadata)
        .then_some(value)
}

/// Called while the repository and state locks are held, before deletion. The
/// durable intent lets explicit Restore recreate the exact retained branch even
/// if the process exits between Git removal and its success receipt.
pub(super) fn prepare_removal(common: &Path, row: &Value) -> Result<()> {
    let path = Path::new(string(row, "path"));
    let ownership = owned(common, path).ok_or("Not a verified Zerus-created worktree")?;
    if ownership["branch"] != row["branch"] {
        return Err("The managed worktree branch changed; review it manually".into());
    }
    let archives = archive::records()?;
    let directory = root().join("archives");
    let count = match fs::read_dir(&directory) {
        Ok(files) => files
            .map(|file| {
                file.map(|file| file.path().extension().is_some_and(|e| e == "json"))
                    .map_err(|e| e.to_string())
            })
            .collect::<Result<Vec<_>>>()?
            .into_iter()
            .filter(|json| *json)
            .count(),
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => 0,
        Err(e) => return Err(e.to_string()),
    };
    if count != archives.len() {
        return Err(
            "Archives could not be fully checked; the worktree was kept for manual review".into(),
        );
    }
    for mut record in archives {
        let refers = ["cwd", "launch_dir"]
            .iter()
            .any(|key| Path::new(string(&record, key)).starts_with(path));
        if !refers {
            continue;
        }
        record["worktree_cleanup"] = json!({"owner":ownership,"head":row["head"],"branch":row["branch"],"status":"removal_pending"});
        atomic(
            &archive::path(string(&record, "archive_id"))?,
            &format!("{record}\n"),
        )?;
    }
    Ok(())
}

pub(super) fn restorable(record: &Value) -> bool {
    let cleanup = &record["worktree_cleanup"];
    let owner = &cleanup["owner"];
    cleanup["status"] == "removal_pending"
        && owner["version"] == 1
        && Path::new(string(owner, "path")).is_absolute()
        && Path::new(string(owner, "common_dir")).is_dir()
        && !string(cleanup, "branch").is_empty()
        && !string(cleanup, "head").is_empty()
        && Path::new(string(record, "launch_dir")).starts_with(Path::new(string(owner, "path")))
}

/// Restore is an explicit launch operation, never triggered by browsing an
/// archive. Refuse branch changes, existing destinations and all uncertain cases.
pub(super) fn restore(record: &Value) -> Result<()> {
    let folder = Path::new(string(record, "launch_dir"));
    if folder.is_dir() {
        return Ok(());
    }
    if !restorable(record) {
        return Err(format!(
            "Working folder is unavailable: {}",
            folder.display()
        ));
    }
    let cleanup = &record["worktree_cleanup"];
    let owner = &cleanup["owner"];
    let common = Path::new(string(owner, "common_dir"));
    let path = Path::new(string(owner, "path"));
    let cache = cache_file("repo", &common.to_string_lossy());
    let guard = crate::platform::private_open(
        OpenOptions::new()
            .create(true)
            .truncate(false)
            .read(true)
            .write(true),
    )
    .open(cache.with_extension("lock"))
    .map_err(|e| e.to_string())?;
    guard
        .try_lock_exclusive()
        .map_err(|_| "Another worktree operation is running; retry Restore after it finishes")?;
    let _repo_lock = crate::platform::FileLock::held(guard);
    let _state_lock = lock(None)?;
    let stored: Value =
        serde_json::from_slice(&fs::read(receipt_path(common, path)).map_err(|e| e.to_string())?)
            .map_err(|e| e.to_string())?;
    if stored != *owner || fs::symlink_metadata(path).is_ok() {
        return Err(
            "Restore destination or ownership changed; inspect the worktree manually".into(),
        );
    }
    if crate::platform::canonical(common).ok().as_deref() != Some(common)
        || path
            .parent()
            .and_then(|parent| crate::platform::canonical(parent).ok())
            .as_deref()
            != path.parent()
    {
        return Err(
            "Restore repository or destination parent changed; inspect its location manually"
                .into(),
        );
    }
    let branch = string(cleanup, "branch");
    let reference = format!("refs/heads/{branch}");
    let head = git(
        common,
        &["rev-parse", "--verify", "--end-of-options", &reference],
        Instant::now() + Duration::from_secs(3),
    )
    .map_err(|e| e.detail)?;
    if String::from_utf8_lossy(&head).trim() != string(cleanup, "head") {
        return Err(
            "Retained worktree branch changed; select its folder explicitly before restoring"
                .into(),
        );
    }
    git(
        common,
        &[
            "worktree",
            "add",
            "--quiet",
            "--",
            &path.to_string_lossy(),
            branch,
        ],
        Instant::now() + Duration::from_secs(30),
    )
    .map_err(|e| e.detail)?;
    let expected = format!("{}\nrefs/heads/{branch}", string(cleanup, "head"));
    let actual = git(
        path,
        &["rev-parse", "HEAD", "--symbolic-full-name", "HEAD"],
        Instant::now() + Duration::from_secs(3),
    )
    .map_err(|e| e.detail)?;
    if String::from_utf8_lossy(&actual).trim() != expected {
        return Err(
            "Restored worktree changed during checkout; inspect it before retrying Restore".into(),
        );
    }
    // Re-establish ownership for the new Git metadata instance. Keep branch/head.
    created(common, path, branch, string(cleanup, "head"))?;
    let _ = fs::remove_file(cache);
    if !folder.is_dir() {
        return Err("Original session subfolder is unavailable in the restored worktree".into());
    }
    Ok(())
}

/// Archive success must never become a failed termination because cleanup was
/// skipped or could not be confirmed. Emit a separate, machine-readable outcome.
pub(super) fn after_archive(record: &Value) {
    let folder = Path::new(string(record, "launch_dir"));
    if !folder.is_absolute() || !folder.is_dir() {
        return;
    }
    let result = (|| -> Result<Value> {
        let deadline = Instant::now() + Duration::from_secs(5);
        let info = git(
            folder,
            &[
                "rev-parse",
                "--show-toplevel",
                "--path-format=absolute",
                "--git-common-dir",
            ],
            deadline,
        )
        .map_err(|e| e.detail)?;
        let info = String::from_utf8(info).map_err(|e| e.to_string())?;
        let mut lines = info.lines();
        let path = crate::platform::canonical(lines.next().ok_or("Missing worktree path")?)
            .map_err(|e| e.to_string())?;
        let common = crate::platform::canonical(lines.next().ok_or("Missing repository")?)
            .map_err(|e| e.to_string())?;
        let outcome = (|| -> Result<Value> {
            if path.join(".git").is_dir() {
                return Ok(Value::Null);
            }
            let args = vec![
                "--path".into(),
                path.to_string_lossy().into_owned(),
                "--common-dir".into(),
                common.to_string_lossy().into_owned(),
                "--fingerprint".into(),
                "automatic".into(),
                "--automatic".into(),
            ];
            super::worktree_cleanup::remove_command(&args)
        })();
        Ok(match outcome {
            Ok(value) if value.is_null() => Value::Null,
            Ok(value) => value,
            Err(error) => {
                json!({"status":if error.starts_with("Removal could not be confirmed") {"unconfirmed"} else {"kept"},"path":path,"common_dir":common,"reason":error})
            }
        })
    })();
    if let Ok(value) = result {
        if !value.is_null() {
            eprintln!("ZERUS_WORKTREE_CLEANUP {value}");
        }
    }
}
