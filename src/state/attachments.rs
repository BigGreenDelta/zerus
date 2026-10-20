//! Attachment access uses durable delivery receipts, never a caller's file path.
use super::*;
use base64::{engine::general_purpose::STANDARD, Engine};
use std::fs::{self, OpenOptions};
use std::io::{Read, Write};
const MAX_FILE: u64 = 10 * 1024 * 1024;

fn receipt(path: &Path) -> Result<Value> {
    let file = crate::platform::refuse_link(OpenOptions::new().read(true))
        .open(path)
        .map_err(|e| e.to_string())?;
    if !file.metadata().map_err(|e| e.to_string())?.is_file() {
        return Err("Attachment receipt is unavailable".into());
    }
    let mut bytes = Vec::new();
    file.take(256 * 1024 + 1)
        .read_to_end(&mut bytes)
        .map_err(|e| e.to_string())?;
    if bytes.len() > 256 * 1024 {
        return Err("Attachment receipt is too large".into());
    }
    serde_json::from_slice(&bytes).map_err(|e| e.to_string())
}
fn belongs(receipt: &Value, record: &Value, agent: &str) -> bool {
    let conversation = receipt
        .get("resolved_conversation_id")
        .unwrap_or(&receipt["conversation_id"]);
    receipt["status"] == "submitted"
        && string(receipt, "agent_id") == agent
        && ((conversation.as_str().is_some_and(|s| !s.is_empty())
            && *conversation == record["conversation_id"])
            || (conversation.as_str() == Some("")
                && receipt["first_message"] == true
                && receipt["run_id"] == record["run_id"]))
}
fn directory(record: &Value) -> PathBuf {
    root().join(if record["agent"]=="opencode" || record["backend"]=="opencode" {"opencode/receipts"} else if record["agent"] == "dsh" || record["backend"] == "dsh" {
        "dsh/receipts"
    } else {
        "input_receipts"
    })
}
pub(super) fn validate_references(references: &[&str]) -> Result<()> {
    let pattern = regex::Regex::new(r"^\[(Image|File) #[1-9][0-9]{0,8}\]$").unwrap();
    let mut seen = std::collections::HashSet::new();
    for reference in references.iter().filter(|r| !r.is_empty()) {
        if !pattern.is_match(reference) || !seen.insert(*reference) {
            return Err("Invalid or duplicate attachment reference".into());
        }
    }
    Ok(())
}

pub(super) enum MessagePart<'a> {
    Text(&'a str),
    Attachment(usize),
}

// Match only the original message: a filename containing another reference
// must never be expanded recursively. Repeated labels reference the same file;
// its contents are inserted once, at the first occurrence.
pub(super) fn ordered_parts<'a>(text: &'a str, references: &[&str]) -> Vec<MessagePart<'a>> {
    let pattern = regex::Regex::new(r"\[(Image|File) #[1-9][0-9]{0,8}\]").unwrap();
    let mut parts = Vec::new();
    let mut seen = vec![false; references.len()];
    let mut offset = 0;
    for found in pattern.find_iter(text) {
        if let Some(index) = references.iter().position(|r| *r == found.as_str()) {
            if !seen[index] {
                parts.push(MessagePart::Text(&text[offset..found.end()]));
                parts.push(MessagePart::Attachment(index));
                offset = found.end();
                seen[index] = true;
            }
        }
    }
    if offset < text.len() {
        parts.push(MessagePart::Text(&text[offset..]));
    }
    // Legacy callers and a deliberately removed label still send their files.
    for (index, used) in seen.iter().enumerate() {
        if !used {
            parts.push(MessagePart::Attachment(index));
        }
    }
    parts
}

pub(super) fn terminal_text(text: &str, files: &[Value]) -> String {
    let references = files
        .iter()
        .map(|f| string(f, "reference"))
        .collect::<Vec<_>>();
    let mut result = String::new();
    for part in ordered_parts(text, &references) {
        match part {
            MessagePart::Text(value) => result.push_str(value),
            MessagePart::Attachment(index) => {
                let file = &files[index];
                let instruction = format!(
                    "{} {}",
                    if string(file, "mime").starts_with("image/") {
                        "Inspect the attached image at"
                    } else {
                        "Read the attached file at"
                    },
                    serde_json::to_string(&file["path"]).unwrap()
                );
                let reference = references[index];
                if !reference.is_empty() && text.contains(reference) {
                    result.push_str(&format!(" ({instruction})"));
                } else {
                    result.push_str("\n\n");
                    if !reference.is_empty() {
                        result.push_str(reference);
                        result.push(' ');
                    }
                    result.push_str(&instruction);
                }
            }
        }
    }
    result
}

fn public_files(receipt: &Value) -> Vec<Value> {
    receipt["attachments"].as_array().into_iter().flatten().take(8).enumerate()
        .map(|(index,file)|json!({"request_id":receipt["request_id"],"index":index,"name":file["name"],"mime":file["mime"],"bytes":file["bytes"],"reference":file["reference"]})).collect()
}
fn message_text(receipt: &Value) -> String {
    if let Some(text) = receipt["text"].as_str() {
        return text.to_owned();
    }
    // Older receipts predate the user-text field. Strip only the exact suffix
    // HGS itself appended when it submitted these files to the native terminal.
    let mut text = string(receipt, "submitted_text").to_owned();
    for file in receipt["attachments"]
        .as_array()
        .into_iter()
        .flatten()
        .rev()
    {
        let suffix = format!(
            "\n\n{} {}",
            if string(file, "mime").starts_with("image/") {
                "Inspect the attached image at"
            } else {
                "Read the attached file at"
            },
            serde_json::to_string(&file["path"]).unwrap()
        );
        if text.ends_with(&suffix) {
            text.truncate(text.len() - suffix.len());
        }
    }
    text.trim().to_owned()
}
pub(super) fn messages(record: &Value, agent: &str) -> Value {
    let mut paths = fs::read_dir(directory(record))
        .into_iter()
        .flatten()
        .flatten()
        .filter(|p| p.path().extension().is_some_and(|e| e == "json"))
        .filter_map(|p| Some((p.metadata().ok()?.modified().ok()?, p.path())))
        .collect::<Vec<_>>();
    paths.sort_unstable_by_key(|p| std::cmp::Reverse(p.0));
    let mut messages = Vec::new();
    for (_, path) in paths.into_iter().take(512) {
        let Ok(mut receipt) = receipt(&path) else {
            continue;
        };
        if !belongs(&receipt, record, agent)
            || receipt["attachments"].as_array().is_none_or(Vec::is_empty)
        {
            continue;
        }
        if string(&receipt, "conversation_id").is_empty()
            && string(&receipt, "resolved_conversation_id").is_empty()
            && !string(record, "conversation_id").is_empty()
            && receipt["run_id"] == record["run_id"]
        {
            // Pin the first prompt once its provider creates the conversation.
            // Preserve the original acknowledgement for idempotent send retries.
            receipt["resolved_conversation_id"] = record["conversation_id"].clone();
            let _ = atomic(&path, &receipt.to_string());
        }
        messages.push(json!({"type":"UserPromptSubmit","source":"hgs_delivery","message_id":receipt["request_id"],"agent_id":"",
            "at":receipt.get("submitted_at").unwrap_or(&receipt["at"]),"detail":message_text(&receipt),"submitted_text":receipt["submitted_text"],"attachments":public_files(&receipt)}));
        if messages.len() >= 100 {
            break;
        }
    }
    json!(messages)
}

/// Public history projection for one immutable receipt. Raw paths and native
/// terminal-expanded attachment instructions never leave this local helper.
pub(super) fn history_receipt(record:&Value,agent:&str,path:&Path)->Result<Option<(Value,String)>> {
    let receipt=receipt(path)?;
    if !belongs(&receipt,record,agent)||receipt["attachments"].as_array().is_none_or(Vec::is_empty){return Ok(None);}
    let text=message_text(&receipt);
    Ok(Some((json!({"type":"UserPromptSubmit","source":"hgs_delivery","message_id":receipt["request_id"],"agent_id":"",
        "at":receipt.get("submitted_at").unwrap_or(&receipt["at"]),"detail":text,"submitted_text":text,"attachments":public_files(&receipt)}),string(&receipt,"submitted_text").to_owned())))
}
pub(super) fn history_directory(record:&Value)->PathBuf {directory(record)
}

/// Explicitly requested Graph evidence from one exact durable send. It does
/// not enumerate receipts, fetch attachment bytes or acknowledge any message.
pub(super) fn graph_receipt(record: &Value, id: &str) -> Result<Value> {
    let id = uuid::Uuid::parse_str(id).map_err(|_| "Invalid delivery request ID")?;
    if id.is_nil() {
        return Err("Invalid delivery request ID".into());
    }
    let value = receipt(&directory(record).join(format!("{id}.json")))?;
    if !belongs(&value, record, "") || value["request_id"] != id.to_string() {
        return Err("This delivery receipt is unavailable for the exact conversation".into());
    }
    let text = message_text(&value);
    let truncated = text.len() > 32 * 1024;
    let end = text
        .char_indices()
        .map(|(i, _)| i)
        .take_while(|i| *i <= 32 * 1024)
        .last()
        .unwrap_or(0);
    let detail = if truncated { &text[..end] } else { &text };
    Ok(
        json!({"type":"UserPromptSubmit","source":"hgs_delivery","message_id":id.to_string(),"agent_id":"",
        "at":value.get("submitted_at").unwrap_or(&value["at"]),"detail":detail,
        "content_truncated":truncated,"attachments":public_files(&value)}),
    )
}

/// Body-free proof that a result was actually submitted to the exact waiter.
/// A scoped reply ticket declares its meaning; this receipt proves delivery.
pub(super) fn wait_result(waiter: &Value, id: &str, after: f64) -> Result<Value> {
    let request = uuid::Uuid::parse_str(id).map_err(|_| "Invalid result request UUID")?;
    if request.is_nil() || request.to_string()!=id {return Err("Invalid result request UUID".into());}
    let record=json!({"agent":waiter["endpoint"]["provider"],"run_id":waiter["run_id"],"conversation_id":waiter["endpoint"]["conversation"]});
    let value=receipt(&directory(&record).join(format!("{id}.json")))?;
    let at=value.get("submitted_at").or_else(||value.get("at")).and_then(Value::as_f64).filter(|at|at.is_finite() && *at>=after);
    if !belongs(&value,&record,"") || value["request_id"]!=id || value["run_id"]!=record["run_id"] || at.is_none() {
        return Err("Result delivery is not confirmed for this exact wait; preserve the wait and check the original send".into());
    }
    Ok(json!({"request_id":id,"submitted_at":at}))
}

pub(super) fn stage_native(request: &Value) -> Result<Value> {
    if request["attachments"].is_null() {
        return Ok(json!([]));
    }
    let files = request["attachments"]
        .as_array()
        .ok_or("Invalid attachments")?;
    if files.len() > 8 {
        return Err("At most 8 attachments are allowed".into());
    }
    let request_id = uuid::Uuid::parse_str(string(request, "request_id"))
        .map_err(|_| "Invalid attachment request")?;
    let parent = root().join("attachments");
    private_dir(&parent)?;
    let staged_dir = tempfile::Builder::new()
        .prefix(&format!("native-{request_id}-"))
        .tempdir_in(&parent)
        .map_err(|e| e.to_string())?;
    let dir = staged_dir.path();
    let mut total = 0;
    let mut staged = Vec::new();
    for (index, file) in files.iter().enumerate() {
        let data = string(file, "data_base64");
        if data.len() > MAX_FILE as usize * 4 / 3 + 4 {
            return Err("Attachment exceeds 10 MiB".into());
        }
        let bytes = STANDARD
            .decode(data)
            .map_err(|_| "Invalid attachment data")?;
        total += bytes.len();
        if bytes.is_empty() || bytes.len() > MAX_FILE as usize || total > 20 * 1024 * 1024 {
            return Err("Attachments exceed the size limit".into());
        }
        private_dir(dir)?;
        let path = dir.join(input::safe_filename(string(file, "name"), index));
        let mut output = crate::platform::private_open(OpenOptions::new().create_new(true).write(true))
            .open(&path)
            .map_err(|e| e.to_string())?;
        output.write_all(&bytes).map_err(|e| e.to_string())?;
        staged
            .push(json!({"name":file["name"],"mime":file["mime"],"path":path,"bytes":bytes.len(),"reference":file["reference"]}));
    }
    if !staged.is_empty() {
        let _ = staged_dir.keep();
    }
    Ok(json!(staged))
}
/// The creation time a staging request names instead of a run identity: an
/// untracked session, such as a shell from `hgs sh -d`, has none.
fn untracked_request(request: &Value) -> Option<u64> {
    (request["expected_run_id"] == "" && request["expected_conversation_id"] == "")
        .then(|| request["expected_created"].as_u64())
        .flatten()
}

/// Binds staging for an untracked session to the session the caller listed:
/// it still runs, was created at `created`, and no pane carries a run marker.
/// A marked pane belongs to a tracked run whose identity the caller lacks.
fn untracked(
    name: &str,
    created: u64,
    sessions: &[backend::SessionRow],
    panes: Option<&Vec<Pane>>,
) -> Result<()> {
    let listed = sessions.iter().any(|row| row.name == name && row.created == created);
    match panes {
        Some(panes) if listed && panes.iter().all(|pane| pane.2.is_empty()) => Ok(()),
        Some(_) if listed => {
            Err("Wait for the session identity to load, then drop the files again".into())
        }
        _ => Err("Session changed; drop the files again in its Terminal".into()),
    }
}
fn read_file(root: &Path, file: &Value) -> Result<Vec<u8>> {
    let allowed = crate::platform::canonical(root.join("attachments"))
        .map_err(|_| "The attachment is no longer available")?;
    let path = PathBuf::from(string(file, "path"));
    let resolved =
        crate::platform::canonical(&path).map_err(|_| "The attachment is no longer available")?;
    if !resolved.starts_with(&allowed) {
        return Err("Invalid attachment location".into());
    }
    let handle = crate::platform::no_follow(OpenOptions::new().read(true))
        .open(&path)
        .map_err(|_| "The attachment is no longer available")?;
    let meta = handle.metadata().map_err(|e| e.to_string())?;
    if !meta.is_file() || meta.len() > MAX_FILE {
        return Err("Attachment is not a supported file".into());
    }
    let mut bytes = Vec::new();
    handle
        .take(MAX_FILE + 1)
        .read_to_end(&mut bytes)
        .map_err(|e| e.to_string())?;
    if bytes.len() > MAX_FILE as usize {
        return Err("Attachment exceeds 10 MiB".into());
    }
    Ok(bytes)
}
pub(super) fn dispatch(args: &[String]) -> Result<i32> {
    let name = args.first().ok_or("Choose the attachment session")?;
    if args.get(1).map(String::as_str) == Some("--stage") {
        if opencode::exists(name) {return Err("OpenCode file attachments are not yet supported by this adapter".into());}
        if args.len() != 3 || args[2] != "--json" {
            return Err("usage: hgs attachment <session> --stage --json".into());
        }
        let mut bytes = Vec::new();
        io::stdin().take(30 * 1024 * 1024 + 1).read_to_end(&mut bytes).map_err(|e| e.to_string())?;
        if bytes.len() > 30 * 1024 * 1024 { return Err("Attachment request is too large".into()); }
        let request: Value = serde_json::from_slice(&bytes).map_err(|e| e.to_string())?;
        let native = dsh::exists(name);
        if let (false, Some(created)) = (native, untracked_request(&request)) {
            let _guard = lock(None)?;
            let terminal = backend::current();
            untracked(name, created, &terminal.listing()?.sessions, live()?.get(name))?;
            if request["attachments"].as_array().is_none_or(Vec::is_empty) { return Err("No files to transfer".into()); }
            let files = stage_native(&request)?;
            println!("{}",json!({"status":"staged","request_id":request["request_id"],"name":name,
                "run_id":"","conversation_id":"","created":created,"files":files}));
            return Ok(0);
        }
        let _guard = if native { lock(Some(&root().join("dsh/bindings.lock")))? } else { lock(None)? };
        let record = if native { dsh::binding(name)? } else { read(name)? };
        if string(&request,"expected_run_id").is_empty() || request["expected_run_id"] != record["run_id"]
            || request["expected_conversation_id"] != record["conversation_id"] {
            return Err("Session changed; drop the files again in its Terminal".into());
        }
        if !native { kimi_tui_choice::identity(&record)?; }
        if request["attachments"].as_array().is_none_or(Vec::is_empty) { return Err("No files to transfer".into()); }
        let files = stage_native(&request)?;
        println!("{}",json!({"status":"staged","request_id":request["request_id"],"name":name,
            "run_id":record["run_id"],"conversation_id":record["conversation_id"],"files":files}));
        return Ok(0);
    }
    let flag = |key: &str| {
        args.windows(2)
            .find(|p| p[0] == key)
            .map(|p| p[1].as_str())
            .unwrap_or("")
    };
    let id = uuid::Uuid::parse_str(flag("--request")).map_err(|_| "Invalid attachment identity")?;
    let index = flag("--index")
        .parse::<usize>()
        .map_err(|_| "Invalid attachment index")?;
    if index >= 8 {
        return Err("Invalid attachment index".into());
    }
    let record = if !flag("--archive").is_empty() {
        archive::read_archive(name, flag("--archive"))?
    } else if opencode::exists(name) { opencode::binding(name)?
    } else if dsh::exists(name) {
        dsh::binding(name)?
    } else {
        read(name)?
    };
    if flag("--conversation") != string(&record, "conversation_id") {
        return Err("The session conversation changed; nothing was read".into());
    }
    if let Some(output) = voice::read_attachment(&record, Some(flag("--agent")), &id.to_string(), index)? {
        println!("{output}");
        return Ok(0);
    }
    let receipt = receipt(&directory(&record).join(format!("{id}.json")))?;
    if !belongs(&receipt, &record, flag("--agent")) {
        return Err("This attachment belongs to another conversation".into());
    }
    let file = receipt["attachments"]
        .as_array()
        .and_then(|v| v.get(index))
        .ok_or("Attachment not found")?;
    let bytes = read_file(&root(), file)?;
    let mut output = public_files(&receipt)[index].clone();
    output["data_base64"] = json!(STANDARD.encode(bytes));
    println!("{output}");
    Ok(0)
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn inline_references_follow_text_order_without_recursive_expansion() {
        let files = vec![
            json!({"reference":"[Image #1]","mime":"image/png","path":"/private/[Image #2].png"}),
            json!({"reference":"[Image #2]","mime":"image/png","path":"/private/two.png"}),
            json!({"reference":"[File #3]","mime":"text/plain","path":"/private/three.txt"}),
        ];
        let text = "До [Image #2] между [Image #1] после; again [Image #2]";
        let output = terminal_text(text, &files);
        assert!(output.starts_with(
            "До [Image #2] (Inspect the attached image at \"/private/two.png\") между [Image #1]"
        ));
        assert!(output.contains("\"/private/[Image #2].png\") после; again [Image #2]"));
        assert_eq!(output.matches("Inspect the attached image at").count(), 2);
        assert!(output.ends_with("\n\n[File #3] Read the attached file at \"/private/three.txt\""));
        let parts = ordered_parts(text, &["[Image #1]", "[Image #2]", "[File #3]"]);
        let order = parts
            .iter()
            .filter_map(|p| match p {
                MessagePart::Attachment(i) => Some(*i),
                _ => None,
            })
            .collect::<Vec<_>>();
        assert_eq!(order, [1, 0, 2]);
    }

    #[test]
    fn attachment_reference_validation_is_strict_but_legacy_is_supported() {
        assert!(validate_references(&["", "", "[Image #1]", "[File #2]"]).is_ok());
        for refs in [
            vec!["[Image #1]", "[Image #1]"],
            vec!["hello"],
            vec!["[Image #0]"],
            vec!["[Image #1]\n"],
        ] {
            assert!(validate_references(&refs).is_err());
        }
        assert_eq!(
            terminal_text("hello", &[json!({"mime":"image/png","path":"/old.png"})]),
            "hello\n\nInspect the attached image at \"/old.png\""
        );
    }
    #[test]
    fn receipts_are_bound_to_the_conversation_and_child() {
        let record = json!({"run_id":"new-run","conversation_id":"same-conversation"});
        let mut r =
            json!({"status":"submitted","run_id":"old-run","conversation_id":"same-conversation"});
        assert!(belongs(&r, &record, ""));
        r["conversation_id"] = json!("other");
        assert!(!belongs(&r, &record, ""));
        r["run_id"] = json!("new-run");
        r["first_message"] = json!(true);
        assert!(!belongs(&r, &record, ""));
        r["conversation_id"] = json!("");
        assert!(belongs(&r, &record, ""));
        r["agent_id"] = json!("child");
        assert!(!belongs(&r, &record, ""));
        assert!(belongs(&r, &record, "child"));
        r["status"] = json!("pending");
        assert!(!belongs(&r, &record, "child"));
    }
    // Creates a Unix symlink.
    #[cfg(unix)]
    #[test]
    fn attachment_reads_cannot_escape_the_private_store() {
        let temp = tempfile::tempdir().unwrap();
        fs::create_dir(temp.path().join("attachments")).unwrap();
        let path = temp.path().join("attachments/file.txt");
        fs::write(&path, b"file content").unwrap();
        assert_eq!(
            read_file(temp.path(), &json!({"path":path})).unwrap(),
            b"file content"
        );
        let other = temp.path().join("private.txt");
        fs::write(&other, b"private").unwrap();
        assert!(read_file(temp.path(), &json!({"path":other})).is_err());
        let link = temp.path().join("attachments/link");
        std::os::unix::fs::symlink(&other, &link).unwrap();
        assert!(read_file(temp.path(), &json!({"path":link})).is_err());
        fs::remove_file(&path).unwrap();
        assert!(read_file(temp.path(), &json!({"path":path})).is_err());
    }
    #[test]
    fn untracked_staging_is_bound_to_the_listed_live_session() {
        let request = |run: Value, conversation: Value, created: Value| {
            json!({"expected_run_id":run,"expected_conversation_id":conversation,"expected_created":created})
        };
        assert_eq!(untracked_request(&request(json!(""), json!(""), json!(1_700_000_000))), Some(1_700_000_000));
        // Tracked requests and requests without a creation time keep the run identity check.
        for other in [
            request(json!("run"), json!(""), json!(1_700_000_000)),
            request(json!(""), json!("conversation"), json!(1_700_000_000)),
            request(json!(""), json!(""), Value::Null),
            request(json!(""), json!(""), json!("1700000000")),
            request(json!(""), json!(""), json!(-1)),
            request(Value::Null, Value::Null, json!(1_700_000_000)),
        ] {
            assert_eq!(untracked_request(&other), None, "{other}");
        }
        let row = |name: &str, created| backend::SessionRow {
            name: name.into(),
            id: String::new(),
            attached: 1,
            created,
            launch_id: String::new(),
        };
        let sessions = [row("sh/scratch", 1_700_000_000), row("codex/hgs/dashboard", 1_700_000_001)];
        let shell = vec![("%1".to_owned(), "0".to_owned(), String::new()), ("%2".to_owned(), "1".to_owned(), String::new())];
        assert!(untracked("sh/scratch", 1_700_000_000, &sessions, Some(&shell)).is_ok());
        let changed = "Session changed; drop the files again in its Terminal";
        // Recreated under the same name, ended, or never listed.
        assert_eq!(untracked("sh/scratch", 1_699_999_999, &sessions, Some(&shell)).unwrap_err(), changed);
        assert_eq!(untracked("sh/scratch", 1_700_000_000, &sessions, None).unwrap_err(), changed);
        assert_eq!(untracked("sh/other", 1_700_000_000, &sessions, Some(&shell)).unwrap_err(), changed);
        // A supervisor's run marker means a tracked run whose identity the caller lacks.
        let tracked = vec![("%3".to_owned(), "0".to_owned(), "run".to_owned())];
        assert_eq!(
            untracked("codex/hgs/dashboard", 1_700_000_001, &sessions, Some(&tracked)).unwrap_err(),
            "Wait for the session identity to load, then drop the files again"
        );
    }
    #[test]
    fn legacy_image_only_receipt_has_no_synthetic_prompt() {
        let receipt = json!({"submitted_text":"\n\nInspect the attached image at \"/private/image.png\"","attachments":[{"path":"/private/image.png","mime":"image/png"}]});
        assert_eq!(message_text(&receipt), "");
    }
}
