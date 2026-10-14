//! Read-only checkout/publication evidence, separate from fleet and agent polling.
use super::*;
use sha2::{Digest, Sha256};
use std::fs;
use std::time::{Duration, Instant};
use worktrees::git;

fn text(cwd: &Path, args: &[&str], deadline: Instant) -> Option<String> {
    String::from_utf8(git(cwd, args, deadline).ok()?).ok().map(|s| s.trim().to_owned())
}

fn oid(value: &str) -> bool {
    matches!(value.len(), 40 | 64) && value.bytes().all(|b| b.is_ascii_hexdigit())
}

fn parse_status(bytes: &[u8]) -> Option<Value> {
    let mut fields = bytes.split(|b| *b == 0);
    let mut data = json!({"changed_files":0,"conflicts":0,"ahead":null,"behind":null});
    let mut changed = 0;
    let mut conflicts = 0;
    while let Some(field) = fields.next() {
        if field.is_empty() { continue; }
        if field.starts_with(b"# ") {
            let line = std::str::from_utf8(field).ok()?;
            if let Some(head) = line.strip_prefix("# branch.oid ") {
                if head != "(initial)" && !oid(head) { return None; }
                data["head"] = json!(head);
                data["unborn"] = json!(head == "(initial)");
            } else if let Some(branch) = line.strip_prefix("# branch.head ") {
                data["detached"] = json!(branch == "(detached)");
                data["branch"] = json!(if branch == "(detached)" { "" } else { branch });
            } else if let Some(upstream) = line.strip_prefix("# branch.upstream ") {
                data["upstream"] = json!(upstream);
            } else if let Some(ab) = line.strip_prefix("# branch.ab ") {
                let (ahead, behind) = ab.split_once(' ')?;
                data["ahead"] = json!(ahead.strip_prefix('+')?.parse::<u64>().ok()?);
                data["behind"] = json!(behind.strip_prefix('-')?.parse::<u64>().ok()?);
            }
        } else {
            match field.first()? {
                b'1' | b'?' => changed += 1,
                b'2' => { changed += 1; fields.next()?; }, // Rename: one path, with a second NUL field.
                b'u' => { changed += 1; conflicts += 1; },
                _ => return None,
            }
        }
    }
    if !data["head"].is_string() || !data["branch"].is_string() { return None; }
    data["changed_files"] = json!(changed);
    data["conflicts"] = json!(conflicts);
    Some(data)
}

fn remote_check(cwd: &Path, common: &Path, remote: &str, reference: &str, tracking: &str, deadline: Instant) -> Value {
    // Include the configured endpoint and observed tracking OID in the identity.
    // A push/fetch or remote reconfiguration invalidates old verification.
    let Some(endpoint) = text(cwd, &["remote", "get-url", "--", remote], deadline) else {
        return json!({"state":"unavailable","at":now()});
    };
    let identity = format!("{}\0{remote}\0{reference}\0{tracking}\0{endpoint}", common.display());
    let cache = root().join("git-status-cache").join(format!("{:x}.json", Sha256::digest(identity.as_bytes())));
    if let Some(cached) = fs::metadata(&cache).ok().filter(|m| m.len() < 4096)
        .and_then(|_| fs::read(&cache).ok()).and_then(|b| serde_json::from_slice::<Value>(&b).ok()) {
        let age = now() - cached["at"].as_f64().unwrap_or(0.0);
        if (0.0..60.0).contains(&age) { return cached; }
    }
    let reported = text(cwd, &["-c", "credential.interactive=false", "ls-remote", "--refs", "--", remote, reference], deadline);
    let mut result = json!({"state":"unavailable","at":now()});
    if let Some(reported) = reported {
        // ls-remote patterns can match more than one ref. Accept only the exact one.
        let matches: Vec<_> = reported.lines().filter_map(|line| line.split_once('\t'))
            .filter(|(hash, name)| *name == reference && oid(hash)).collect();
        result["state"] = json!(match matches.as_slice() {
            [(hash, _)] if *hash == tracking => "verified",
            [(_, _)] => "changed",
            [] if reported.is_empty() => "missing",
            _ => "unavailable",
        });
    }
    let _ = atomic(&cache, &result.to_string());
    result
}

pub(super) fn inspect(path: &Path) -> Value {
    let deadline = Instant::now() + Duration::from_secs(3);
    let local_deadline = Instant::now() + Duration::from_millis(700);
    let mut data = json!({"state":"unavailable","path":path,"sampled_at":now(),"remote_state":"unknown"});
    let Some(path) = crate::platform::canonical(path).ok().filter(|p| p.is_dir()) else {
        data["state"] = json!("folder_unavailable"); return data;
    };
    data["path"] = json!(path);
    let bytes = match git(&path, &["status", "--porcelain=v2", "--branch", "-z", "--untracked-files=all"], local_deadline) {
        Ok(bytes) => bytes,
        Err(error) => { data["state"] = json!(error.state); return data; }
    };
    let Some(status) = parse_status(&bytes) else { return data; };
    data.as_object_mut().unwrap().extend(status.as_object().unwrap().clone());
    let Some(root_path) = text(&path, &["rev-parse", "--show-toplevel"], local_deadline) else { return data; };
    let Some(common) = text(&path, &["rev-parse", "--path-format=absolute", "--git-common-dir"], local_deadline) else { return data; };
    data["root"] = json!(root_path);
    data["common_dir"] = json!(common);
    data["state"] = json!("ok");
    if data["detached"] == true {
        data["remote_state"] = json!("detached"); return data;
    }
    let branch = format!("refs/heads/{}", string(&data, "branch"));
    // A failed or timed-out lookup is no evidence that no upstream is
    // configured: keep remote_state unknown.
    let Some(upstream) = text(&path, &["for-each-ref", "--format=%(upstream:remotename)%00%(upstream:remoteref)", &branch], local_deadline) else {
        return data;
    };
    let Some((remote, reference)) = upstream.split_once('\0') else {
        data["remote_state"] = json!("no_upstream"); return data;
    };
    if remote.is_empty() || reference.is_empty() || remote == "." {
        data["remote_state"] = json!(if remote == "." { "local_upstream" } else { "no_upstream" }); return data;
    }
    data["remote"] = json!(remote);
    data["remote_ref"] = json!(reference);
    let tracking = text(&path, &["rev-parse", "--verify", "@{upstream}"], local_deadline).filter(|s| oid(s)).unwrap_or_default();
    let checked = remote_check(&path, Path::new(&common), remote, reference, &tracking, deadline);
    data["remote_state"] = checked["state"].clone();
    data["remote_checked_at"] = checked["at"].clone();
    data["remote_age_seconds"] = json!((now() - checked["at"].as_f64().unwrap_or(0.0)).max(0.0));
    // Recheck after network IO: do not certify a checkout that switched branches
    // or changed while verification was in flight.
    match git(&path, &["status", "--porcelain=v2", "--branch", "-z", "--untracked-files=all"], deadline)
        .ok().and_then(|b| parse_status(&b)) {
        Some(current) if current == status => {},
        Some(_) => { data["state"] = json!("changed_during_check"); },
        None => { data["state"] = json!("unavailable"); },
    }
    data
}

pub(super) fn dispatch(args: &[String]) -> Result<i32> {
    let mut path = None;
    let mut i = 0;
    while i < args.len() {
        match args[i].as_str() {
            "--json" => {},
            "--path" if path.is_none() => { i += 1; path = args.get(i); },
            _ => return Err("usage: hgs git-status --path PATH --json".into()),
        }
        i += 1;
    }
    let path = path.ok_or("usage: hgs git-status --path PATH --json")?;
    if !Path::new(path).is_absolute() || path.chars().any(char::is_control) {
        return Err("Git status requires an absolute folder path".into());
    }
    println!("{}", inspect(Path::new(path)));
    Ok(0)
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn nul_paths_and_renames_are_counted_once() {
        let bytes = b"# branch.oid aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\0# branch.head feature/status\0# branch.upstream origin/main\0# branch.ab +2 -3\x001 MM ignored metadata file\x002 R. ignored metadata new\nname\0old\nname\0? non-utf8-\xff\0u UU conflicted\0";
        let status = parse_status(bytes).unwrap();
        assert_eq!(status["changed_files"], 4);
        assert_eq!(status["conflicts"], 1);
        assert_eq!((status["ahead"].as_u64(), status["behind"].as_u64()), (Some(2), Some(3)));
        assert!(parse_status(b"# branch.head main\0").is_none());
        assert!(parse_status(b"# branch.oid bad\0# branch.head main\0").is_none());
    }
}
