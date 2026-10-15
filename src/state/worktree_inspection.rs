//! Explicit, bounded worktree inspection for the cleanup workspace. No fetches,
//! external diff tools, textconv filters or worktree mutations are requested.
use super::worktrees::git;
use super::*;
use std::fs::{self, OpenOptions};
use std::io::Read;
use std::path::Component;
use std::time::{Duration, Instant};

const TEXT_LIMIT: usize = 128 * 1024;

fn output(path: &Path, args: &[&str], deadline: Instant) -> Value {
    match git(path, args, deadline) {
        Ok(bytes) => {
            let truncated = bytes.len() > TEXT_LIMIT;
            json!({"text":String::from_utf8_lossy(&bytes[..bytes.len().min(TEXT_LIMIT)]),"truncated":truncated})
        }
        Err(error) => json!({"error":error.detail,"state":error.state}),
    }
}

fn local_file(root: &Path, relative: &str, offset: usize) -> Result<Value> {
    let name = Path::new(relative);
    if name.as_os_str().is_empty()
        || relative != "."
            && name
                .components()
                .any(|c| !matches!(c, Component::Normal(_)))
    {
        return Err("Select a relative file inside this worktree".into());
    }
    let mut path = root.to_path_buf();
    for part in name.components().filter(|part| *part != Component::CurDir) {
        path.push(part.as_os_str());
        let metadata = fs::symlink_metadata(&path).map_err(|e| e.to_string())?;
        if metadata.file_type().is_symlink() {
            return Err("Symbolic links are not followed by the worktree preview".into());
        }
    }
    let actual = crate::platform::canonical(&path).map_err(|e| e.to_string())?;
    if !actual.starts_with(root) {
        return Err("The file is outside this worktree".into());
    }
    if actual.is_dir() {
        let mut entries = Vec::new();
        let mut truncated = false;
        let deadline = Instant::now() + Duration::from_secs(2);
        for (index, entry) in fs::read_dir(&actual)
            .map_err(|e| e.to_string())?
            .enumerate()
        {
            if Instant::now() >= deadline {
                return Err("Directory listing timed out; inspect a smaller folder".into());
            }
            if index < offset {
                continue;
            }
            let entry = entry.map_err(|e| e.to_string())?;
            if entries.len() == 200 {
                truncated = true;
                break;
            }
            let file_type = entry.file_type().map_err(|e| e.to_string())?;
            let child = if relative == "." {
                PathBuf::from(entry.file_name())
            } else {
                name.join(entry.file_name())
            };
            entries.push(json!({"path":child,"directory":file_type.is_dir(),"symlink":file_type.is_symlink()}));
        }
        entries.sort_by_key(|entry| entry["path"].as_str().unwrap_or("").to_owned());
        return Ok(
            json!({"path":relative,"entries":entries,"truncated":truncated,"offset":offset,"next_offset":if truncated {Some(offset+entries.len())} else {None}}),
        );
    }
    let mut file = crate::platform::no_follow(OpenOptions::new().read(true))
        .open(&actual)
        .map_err(|e| e.to_string())?;
    if !file.metadata().map_err(|e| e.to_string())?.is_file() {
        return Err("Only ordinary files can be previewed".into());
    }
    let mut bytes = Vec::new();
    Read::by_ref(&mut file)
        .take(TEXT_LIMIT as u64 + 1)
        .read_to_end(&mut bytes)
        .map_err(|e| e.to_string())?;
    let truncated = bytes.len() > TEXT_LIMIT;
    bytes.truncate(TEXT_LIMIT);
    let binary = bytes.contains(&0) || std::str::from_utf8(&bytes).is_err() && !truncated;
    Ok(
        json!({"path":relative,"binary":binary,"truncated":truncated,
        "text":if binary { String::new() } else { String::from_utf8_lossy(&bytes).into_owned() }}),
    )
}

pub(super) fn command(args: &[String]) -> Result<Value> {
    let mut options = BTreeMap::new();
    let mut i = 0;
    while i < args.len() {
        match args[i].as_str() {
            "--json" => {},
            key @ ("--path" | "--common-dir" | "--file" | "--offset") => {
                i += 1;
                let value = args.get(i).filter(|v| !v.is_empty() && !v.chars().any(char::is_control))
                    .ok_or_else(|| format!("{key} needs a path"))?;
                if options.insert(key, value.as_str()).is_some() { return Err(format!("Duplicate {key}")); }
            }
            _ => return Err("usage: hgs worktrees inspect --path WORKTREE --common-dir DIR [--file RELATIVE_PATH [--offset N]] [--json]".into()),
        }
        i += 1;
    }
    let path = Path::new(options.get("--path").ok_or("--path is required")?);
    let common = Path::new(
        options
            .get("--common-dir")
            .ok_or("--common-dir is required")?,
    );
    if !path.is_absolute() || !common.is_absolute() {
        return Err("Worktree and repository must be absolute paths".into());
    }
    let path = crate::platform::canonical(path).map_err(|e| e.to_string())?;
    let common = crate::platform::canonical(common).map_err(|e| e.to_string())?;
    let deadline = Instant::now() + Duration::from_secs(25);
    let resolve = |args: &[&str]| -> Result<PathBuf> {
        let bytes = git(&path, args, deadline).map_err(|e| e.detail)?;
        crate::platform::canonical(String::from_utf8_lossy(&bytes).trim())
            .map_err(|e| e.to_string())
    };
    if resolve(&["rev-parse", "--show-toplevel"])? != path
        || resolve(&["rev-parse", "--path-format=absolute", "--git-common-dir"])? != common
    {
        return Err("Repository changed. Refresh the worktree list.".into());
    }
    if let Some(file) = options.get("--file") {
        return Ok(
            json!({"state":"ok","path":path,"common_dir":common,"file":local_file(&path,file, options.get("--offset").map_or(Ok(0), |v| v.parse::<usize>()).map_err(|_|"Invalid directory offset")?)?}),
        );
    }
    let review = super::worktree_cleanup::review(
        &common,
        Some(&path),
        Instant::now() + Duration::from_secs(12),
    );
    let row = review["worktrees"]
        .as_array()
        .and_then(|rows| rows.first())
        .cloned()
        .ok_or("Worktree is no longer listed")?;
    let status = git(
        &path,
        &[
            "status",
            "--porcelain=v1",
            "-z",
            "--untracked-files=all",
            "--ignored=no",
        ],
        deadline,
    )
    .map_err(|e| e.detail)?;
    let mut fields = status.split(|b| *b == 0).filter(|b| !b.is_empty());
    let mut files = Vec::new();
    let mut partial = false;
    while let Some(field) = fields.next() {
        if field.len() < 4 {
            return Err("Invalid Git status".into());
        }
        if files.len() == 200 {
            partial = true;
            break;
        }
        files.push(json!({"status":String::from_utf8_lossy(&field[..2]),"path":String::from_utf8_lossy(&field[3..])}));
        if matches!(field[0], b'R' | b'C') {
            fields.next();
        }
    }
    let mut data = json!({"state":"ok","path":path,"common_dir":common,"sampled_at":now(),
        "review":row,"files":files,"files_partial":partial,
        "changes":output(&path,&["diff","--no-ext-diff","--no-textconv","--no-color","HEAD","--"],deadline)});
    if let Some(base) = row["integration"]["ref"].as_str() {
        let range = format!("{base}..HEAD");
        data["commits"] = output(
            &path,
            &[
                "log",
                "--no-show-signature",
                "--no-decorate",
                "-30",
                "--date=short",
                "--format=%h %ad %s",
                &range,
                "--",
            ],
            deadline,
        );
        data["comparison"] = output(
            &path,
            &[
                "diff",
                "--no-ext-diff",
                "--no-textconv",
                "--no-color",
                base,
                "HEAD",
                "--",
            ],
            deadline,
        );
    }
    Ok(data)
}
