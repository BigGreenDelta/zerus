//! Explicit reads of a document on the selected conversation's computer.
//! Unlike sent attachments, these are current filesystem files. Nothing is
//! staged or journaled and no native agent is contacted.
use super::*;
use base64::{engine::general_purpose::STANDARD, Engine};
use sha2::{Digest, Sha256};
use std::fs::OpenOptions;

const MAX_FILE: u64 = 10 * 1024 * 1024;

fn bounded(value: &Value, key: &str, max: usize) -> Result<String> {
    let text = value[key]
        .as_str()
        .ok_or_else(|| format!("Missing {key}"))?;
    if text.is_empty() || text.len() > max || text.chars().any(char::is_control) {
        return Err(format!("Invalid {key}"));
    }
    Ok(text.to_owned())
}
fn binding(name: &str, input: &Value) -> Result<Value> {
    let archived = string(input, "archive_id");
    let record = if !archived.is_empty() {
        archive::read_archive(name, archived)?
    } else if opencode::exists(name) {
        opencode::binding(name)?
    } else if dsh::exists(name) {
        dsh::binding(name)?
    } else {
        read(name)?
    };
    if record["run_id"] != input["expected_run_id"]
        || record["conversation_id"] != input["expected_conversation_id"]
    {
        return Err(
            "The conversation changed. Reopen the file from its original conversation".into(),
        );
    }
    let child = string(input, "agent_id");
    if !child.is_empty() && record["subagents"].get(child).is_none() {
        return Err("The child no longer belongs to this conversation".into());
    }
    Ok(record)
}
fn resolve(record: &Value, input: &Value) -> Result<PathBuf> {
    let text = bounded(input, "path", 8192)?;
    let path = PathBuf::from(&text);
    if path.is_absolute() {
        return Ok(path);
    }
    if let Some(relative) = text.strip_prefix("~/") {
        let home = std::env::var_os("HOME")
            .map(PathBuf::from)
            .filter(|p| p.is_absolute())
            .ok_or("The computer's home directory is unavailable")?;
        return Ok(home.join(relative));
    }
    let child = string(input, "agent_id");
    let cwd = if child.is_empty() {
        string(record, "cwd")
    } else {
        string(&record["subagents"][child], "cwd")
    };
    let cwd = Path::new(cwd);
    if !cwd.is_absolute() {
        return Err("Use an absolute path; this conversation's folder is unavailable".into());
    }
    Ok(cwd.join(path))
}
fn snapshot(path: &Path, expected: &str) -> Result<Value> {
    let resolved = crate::platform::canonical(path).map_err(|e| match e.kind() {
        io::ErrorKind::NotFound => "The file no longer exists on this computer".to_owned(),
        io::ErrorKind::PermissionDenied => {
            "This computer cannot read the file with its current permissions".to_owned()
        }
        _ => "The computer could not resolve this file".to_owned(),
    })?;
    let mut handle = crate::platform::no_follow(OpenOptions::new().read(true))
        .open(&resolved)
        .map_err(|_| "The computer could not open this file")?;
    let before = handle
        .metadata()
        .map_err(|_| "The computer could not inspect this file")?;
    if !before.is_file() {
        return Err("This path is not a regular file".into());
    }
    if before.len() == 0 {
        return Err("The file is empty".into());
    }
    if before.len() > MAX_FILE {
        return Err("Files larger than 10 MiB cannot be opened on the phone".into());
    }
    let mut bytes = Vec::new();
    (&mut handle)
        .take(MAX_FILE + 1)
        .read_to_end(&mut bytes)
        .map_err(|_| "The computer could not read this file")?;
    let after = handle
        .metadata()
        .map_err(|_| "The computer could not inspect this file")?;
    if bytes.len() as u64 != before.len()
        || after.len() != before.len()
        || after.modified().ok() != before.modified().ok()
    {
        return Err("The file changed while it was being read. Open it again".into());
    }
    let digest = format!("{:x}", Sha256::digest(&bytes));
    if !expected.is_empty() && digest != expected {
        return Err("The file changed during download. Open it again".into());
    }
    let name = resolved
        .file_name()
        .and_then(|n| n.to_str())
        .filter(|n| !n.is_empty() && !n.chars().any(char::is_control))
        .ok_or("The filename cannot be transferred to the phone")?;
    let resolved_text = resolved
        .to_str()
        .ok_or("This path cannot be transferred to the phone")?;
    Ok(
        json!({"path":resolved_text,"file_name":name,"mime":mime(name),"bytes":bytes.len(),
        "sha256":digest,"data_base64":STANDARD.encode(bytes)}),
    )
}
fn mime(name: &str) -> &'static str {
    match name
        .rsplit('.')
        .next()
        .unwrap_or("")
        .to_ascii_lowercase()
        .as_str()
    {
        "xlsx" => "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet",
        "xls" => "application/vnd.ms-excel",
        "csv" => "text/csv",
        "pdf" => "application/pdf",
        "docx" => "application/vnd.openxmlformats-officedocument.wordprocessingml.document",
        "pptx" => "application/vnd.openxmlformats-officedocument.presentationml.presentation",
        "txt" | "md" | "rs" | "kt" | "py" | "log" => "text/plain",
        "json" => "application/json",
        "png" => "image/png",
        "jpg" | "jpeg" => "image/jpeg",
        "webp" => "image/webp",
        "zip" => "application/zip",
        _ => "application/octet-stream",
    }
}
pub(super) fn dispatch(args: &[String]) -> Result<i32> {
    if args.len() != 2 || args[1] != "--json" {
        return Err("usage: hgs file-read <session> --json".into());
    }
    let mut raw = Vec::new();
    io::stdin()
        .take(32769)
        .read_to_end(&mut raw)
        .map_err(|e| e.to_string())?;
    if raw.len() > 32768 {
        return Err("File request is too large".into());
    }
    let input: Value = serde_json::from_slice(&raw).map_err(|_| "Invalid file request")?;
    let fields = input.as_object().ok_or("Invalid file request")?;
    if fields.keys().any(|k| {
        ![
            "request_id",
            "expected_run_id",
            "expected_conversation_id",
            "path",
            "sha256",
            "archive_id",
            "agent_id",
        ]
        .contains(&k.as_str())
    }) {
        return Err("Unexpected file request fields".into());
    }
    let id = bounded(&input, "request_id", 36)?;
    if uuid::Uuid::parse_str(&id)
        .map_err(|_| "Invalid file request identity")?
        .to_string()
        != id
    {
        return Err("Invalid file request identity".into());
    }
    bounded(&input, "expected_run_id", 128)?;
    bounded(&input, "expected_conversation_id", 256)?;
    let expected = string(&input, "sha256");
    if !expected.is_empty()
        && (expected.len() != 64
            || !expected
                .bytes()
                .all(|b| b.is_ascii_digit() || (b'a'..=b'f').contains(&b)))
    {
        return Err("Invalid expected file digest".into());
    }
    for key in ["archive_id", "agent_id", "sha256"] {
        if input.get(key).is_some() {
            bounded(&input, key, if key == "sha256" { 64 } else { 256 })?;
        }
    }
    let record = binding(&args[0], &input)?;
    let path = resolve(&record, &input)?;
    let mut result = snapshot(&path, expected)?;
    let current = binding(&args[0], &input)?;
    if resolve(&current, &input)? != path {
        return Err("The conversation's folder changed. Reopen this file".into());
    }
    result["request_id"] = json!(id);
    result["name"] = json!(args[0]);
    result["run_id"] = input["expected_run_id"].clone();
    result["conversation_id"] = input["expected_conversation_id"].clone();
    result["requested_path"] = input["path"].clone();
    for key in ["archive_id", "agent_id"] {
        if input.get(key).is_some() {
            result[key] = input[key].clone();
        }
    }
    println!("{result}");
    Ok(0)
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::fs;
    #[test]
    fn exact_unicode_bytes_digest_and_spreadsheet_mime() {
        let root = tempfile::tempdir().unwrap();
        let path = root.path().join("Сводная итог.xlsx");
        fs::write(&path, [0, 255, 80, 75, 10]).unwrap();
        let file = snapshot(&path, "").unwrap();
        assert_eq!(file["file_name"], "Сводная итог.xlsx");
        assert_eq!(file["bytes"], 5);
        assert_eq!(
            file["mime"],
            "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet"
        );
        assert_eq!(
            STANDARD.decode(string(&file, "data_base64")).unwrap(),
            [0, 255, 80, 75, 10]
        );
        fs::write(&path, [0, 255, 80, 75, 11]).unwrap();
        assert!(snapshot(&path, string(&file, "sha256")).is_err());
    }
    #[test]
    fn missing_nonregular_empty_and_oversized_files_are_rejected() {
        let root = tempfile::tempdir().unwrap();
        assert!(snapshot(root.path(), "").is_err());
        let path = root.path().join("file");
        assert!(snapshot(&path, "").is_err());
        fs::write(&path, []).unwrap();
        assert!(snapshot(&path, "").is_err());
        fs::File::create(&path)
            .unwrap()
            .set_len(MAX_FILE + 1)
            .unwrap();
        assert!(snapshot(&path, "").is_err());
    }
    #[test]
    fn relative_paths_use_original_session_not_cli_folder() {
        let root = tempfile::tempdir().unwrap();
        let cwd = root.path().join("project");
        let r = json!({"cwd":cwd});
        assert_eq!(
            resolve(&r, &json!({"path":"docs/report.xlsx"})).unwrap(),
            cwd.join("docs/report.xlsx")
        );
        assert!(resolve(&r, &json!({"path":"report.xlsx","agent_id":"child"})).is_err());
        assert!(resolve(&r, &json!({"path":"bad\nfile"})).is_err());
    }
    #[cfg(unix)]
    #[test]
    fn named_pipe_never_waits_for_a_writer() {
        use std::ffi::CString;
        let root = tempfile::tempdir().unwrap();
        let path = root.path().join("pipe");
        let c = CString::new(path.to_str().unwrap()).unwrap();
        assert_eq!(unsafe { libc::mkfifo(c.as_ptr(), 0o600) }, 0);
        let start = std::time::Instant::now();
        assert!(snapshot(&path, "").is_err());
        assert!(start.elapsed() < std::time::Duration::from_secs(1));
    }
}
