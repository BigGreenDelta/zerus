//! Explicit acquisition of a repository on this computer. Receipts reserve a
//! request before mkdir/Git; a repeated or uncertain request never runs again.
use super::*;
use sha2::{Digest, Sha256};
use std::process::Command;
use std::time::{Duration, Instant};

const USAGE: &str = "usage: hgs repository check|clone --url URL --parent PATH --name NAME [--request-id UUID] --json; hgs repository status --request-id UUID --json";

fn valid_url(url: &str) -> bool {
    if url.is_empty()
        || url.len() > 4096
        || url.chars().any(|c| c.is_control() || c.is_whitespace())
    {
        return false;
    }
    if let Some(rest) = url.strip_prefix("https://") {
        let host = rest.split('/').next().unwrap_or("");
        return !host.is_empty()
            && !host.contains('@')
            && !rest.contains(['?', '#'])
            && rest.contains('/');
    }
    if let Some(rest) = url.strip_prefix("ssh://") {
        let host = rest.split('/').next().unwrap_or("");
        return !host.is_empty()
            && !host.rsplit('@').next().unwrap_or("").is_empty()
            && !host
                .split_once('@')
                .is_some_and(|(user, _)| user.contains(':'))
            && !rest.contains(['?', '#'])
            && rest.contains('/');
    }
    // SCP syntax, including configured SSH host aliases. Reject local paths and
    // Git remote helpers: only the reviewed SSH and HTTPS transports are offered.
    if url.contains("://") || url.starts_with('-') || url.contains("::") {
        return false;
    }
    url.split_once(':').is_some_and(|(host, path)| {
        !host.is_empty() && !host.contains(['/', '\\']) && !path.is_empty()
    })
}

fn validate(url: &str, parent: &str, name: &str) -> Result<PathBuf> {
    if !valid_url(url) {
        return Err("Use an SSH or HTTPS repository address without embedded passwords, tokens, query or fragment".into());
    }
    if name.is_empty()
        || name.len() > 240
        || name != name.trim()
        || matches!(name, "." | "..")
        || name
            .chars()
            .any(|c| c.is_control() || "/\\:*?\"<>|".contains(c))
        || name.ends_with('.')
    {
        return Err(
            "Choose a single new folder name without separators or control characters".into(),
        );
    }
    if parent.is_empty() || parent.len() > 4096 || parent.chars().any(char::is_control) {
        return Err("Choose an existing parent folder".into());
    }
    let path = if parent == "~" {
        home()
    } else if let Some(rest) = parent.strip_prefix("~/") {
        if Path::new(rest).has_root() {
            return Err("Invalid home-relative parent folder".into());
        }
        home().join(rest)
    } else {
        PathBuf::from(parent)
    };
    if !path.is_absolute() {
        return Err("Parent folder must be absolute or home-relative".into());
    }
    let path = crate::platform::canonical(path)
        .map_err(|e| format!("Parent folder is unavailable: {e}"))?;
    if !path.is_dir() {
        return Err("Parent is not a folder".into());
    }
    Ok(path)
}

fn git(parent: &Path, args: &[&str], seconds: u64) -> Result<Vec<u8>> {
    let mut cmd = Command::new("git");
    cmd.current_dir(parent)
        .args(["--no-pager", "-c", "core.fsmonitor=false"])
        .args(args)
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
        cmd.env_remove(key);
    }
    crate::platform::git_noninteractive(&mut cmd);
    let (status, out, err) = crate::platform::run_bounded(
        &mut cmd,
        [65536, 65536],
        Instant::now() + Duration::from_secs(seconds),
        &|| false,
    )
    .map_err(|e| match e {
        crate::platform::RunError::Spawn(e) => format!("Cannot start Git: {e}"),
        crate::platform::RunError::Io(e) => format!("Cannot read Git result: {e}"),
        crate::platform::RunError::TooLarge => {
            "Git output exceeded the limit; inspect the destination before continuing".into()
        }
        _ => "Git timed out; inspect the destination before continuing".into(),
    })?;
    if !status.success() {
        return Err(format!(
            "Git failed: {}",
            String::from_utf8_lossy(&err).trim()
        ));
    }
    Ok(out)
}

fn token(id: &str) -> Result<()> {
    if !uuid::Uuid::parse_str(id).is_ok_and(|u| !u.is_nil() && u.to_string() == id) {
        return Err("--request-id must be a canonical nonempty UUID".into());
    }
    Ok(())
}
fn receipt_path(id: &str) -> PathBuf {
    root().join("repository-clones").join(format!("{id}.json"))
}
fn read_receipt(id: &str) -> Result<Value> {
    let p = receipt_path(id);
    let meta =
        std::fs::symlink_metadata(&p).map_err(|e| format!("Clone receipt unavailable: {e}"))?;
    if !meta.is_file() || meta.len() > 16384 {
        return Err("Invalid clone receipt".into());
    }
    serde_json::from_slice(&std::fs::read(p).map_err(|e| e.to_string())?).map_err(|e| e.to_string())
}

pub(super) fn dispatch(args: &[String]) -> Result<i32> {
    let action = args.first().map(String::as_str).ok_or(USAGE)?;
    let mut options = std::collections::HashMap::new();
    let mut i = 1;
    while i < args.len() {
        if args[i] == "--json" {
            i += 1;
            continue;
        }
        if !["--url", "--parent", "--name", "--request-id"].contains(&args[i].as_str())
            || i + 1 >= args.len()
            || options
                .insert(args[i].as_str(), args[i + 1].as_str())
                .is_some()
        {
            return Err(USAGE.into());
        }
        i += 2;
    }
    let get = |key| options.get(key).copied().ok_or_else(|| USAGE.to_string());
    if action == "status" {
        if options.len() != 1 {
            return Err(USAGE.into());
        }
        let id = get("--request-id")?;
        token(id)?;
        println!("{}", read_receipt(id)?);
        return Ok(0);
    }
    if !matches!(action, "check" | "clone")
        || options.len() != if action == "clone" { 4 } else { 3 }
    {
        return Err(USAGE.into());
    }
    let (url, parent, name) = (get("--url")?, get("--parent")?, get("--name")?);
    let fingerprint = format!(
        "{:x}",
        Sha256::digest(serde_json::to_vec(&json!([url, parent, name])).unwrap())
    );
    let id = if action == "clone" {
        let id = get("--request-id")?;
        token(id)?;
        id
    } else {
        ""
    };
    // A per-request lock prevents duplicate clones without serializing unrelated
    // repositories. Status never waits on a long-running clone.
    let _guard = if action == "clone" {
        let lock_path = receipt_path(id).with_extension("lock");
        private_dir(lock_path.parent().ok_or("Invalid receipt directory")?)?;
        let file = lock_file(&lock_path)?;
        match fs2::FileExt::try_lock_exclusive(&file) {
            Ok(()) => Some(crate::platform::FileLock::held(file)),
            Err(e) if e.kind() == std::io::ErrorKind::WouldBlock => {
                return Err(
                    "The original clone is running; read its receipt with repository status".into(),
                )
            }
            Err(e) => return Err(e.to_string()),
        }
    } else {
        None
    };
    if action == "clone" && receipt_path(id).try_exists().map_err(|e| e.to_string())? {
        let saved = read_receipt(id)?;
        if saved["fingerprint"] != fingerprint {
            return Err("Request ID already belongs to a different clone".into());
        }
        println!("{saved}");
        return Ok(0);
    }
    let canonical = validate(url, parent, name)?;
    let destination = canonical.join(name);
    if std::fs::symlink_metadata(&destination).is_ok() {
        return Err(
            "Destination already exists. Choose another name or use the existing folder".into(),
        );
    }
    if action == "check" {
        git(&canonical, &["--version"], 5)?;
        git(
            &canonical,
            &["ls-remote", "--symref", "--", url, "HEAD"],
            20,
        )?;
        println!(
            "{}",
            json!({"status":"ready","parent":canonical,"path":destination,"name":name})
        );
        return Ok(0);
    }
    let mut receipt = json!({"status":"unconfirmed","request_id":id,"fingerprint":fingerprint,
        "parent":canonical,"path":destination,"name":name});
    atomic(&receipt_path(id), &receipt.to_string())?;
    let result: Result<()> = (|| {
        // Reserve this exact previously absent destination atomically. Git may
        // clean up its own failed transfer; Zerus never removes user files.
        std::fs::create_dir(&destination).map_err(|e| format!("Cannot create destination: {e}"))?;
        git(&canonical, &["clone", "--quiet", "--", url, name], 600)?;
        let actual = crate::platform::canonical(&destination).map_err(|e| e.to_string())?;
        if actual != destination {
            return Err("Clone destination changed; inspect the folder".into());
        }
        let top = git(&destination, &["rev-parse", "--show-toplevel"], 5)?;
        let top = crate::platform::canonical(String::from_utf8_lossy(&top).trim())
            .map_err(|e| e.to_string())?;
        if top != destination {
            return Err("Git did not confirm the requested checkout".into());
        }
        Ok(())
    })();
    match result {
        Ok(()) => receipt["status"] = json!("cloned"),
        Err(error) => {
            receipt["status"] = json!("failed");
            receipt["detail"] = json!(error);
        }
    }
    atomic(&receipt_path(id), &receipt.to_string())?;
    println!("{receipt}");
    Ok(0)
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn accepts_only_repository_addresses() {
        for url in [
            "https://example.test/team/repo.git",
            "git@example.test:team/repo.git",
            "ssh://git@example.test/team/repo.git",
            "ssh://example.test:2222/repo.git",
            "work:repo",
        ] {
            assert!(valid_url(url), "{url}");
        }
        for url in [
            "",
            "--upload-pack=bad",
            "ext::bad",
            "/local/repo",
            "file:///repo",
            "https://token@example.test/repo",
            "ssh://user:pass@example.test/repo",
            "https://example.test/repo?token=x",
            "git@example.test:repo\n",
        ] {
            assert!(!valid_url(url), "{url}");
        }
    }
}
