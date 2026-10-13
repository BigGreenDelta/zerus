//! Repair hook jobs whose exact native completion fell outside the recent window.
//! Scan incrementally; retain only terminal receipts for existing job identities.
use super::*;
use std::collections::BTreeSet;

const SCAN_BYTES: u64 = 16 * 1024 * 1024;
const LINE_BYTES: u64 = 1024 * 1024;

pub(super) fn recover_codex(record: &Value, jobs: &mut Jobs) -> Result<()> {
    recover(record, jobs, &root().join("process-history"), SCAN_BYTES)
}

fn recover(record: &Value, jobs: &mut Jobs, directory: &Path, budget: u64) -> Result<()> {
    let targets: BTreeSet<String> = jobs
        .values()
        .filter(|j| {
            active(j)
                && j["source"] == "hooks"
                && j["owner"] == "main"
                && record["active_tools"].get(string(j, "call_id")).is_none()
        })
        .map(|j| string(j, "call_id").to_owned())
        .collect();
    if targets.is_empty() {
        return Ok(());
    }
    let path = Path::new(string(record, "transcript"));
    let conversation = string(record, "conversation_id");
    if !path.is_absolute() || uuid::Uuid::parse_str(conversation).is_err() {
        return Ok(());
    }
    let file = open_regular(path)?;
    let (source, meta) =
        crate::platform::FileIdentity::with_metadata(&file).map_err(|e| e.to_string())?;
    let mut reader = BufReader::new(file);
    let mut first = String::new();
    (&mut reader)
        .take(256 * 1024)
        .read_line(&mut first)
        .map_err(|e| e.to_string())?;
    let header: Value = serde_json::from_str(&first).map_err(|e| e.to_string())?;
    if header["type"] != "session_meta" || header["payload"]["id"] != conversation {
        return Err("Native process history identity mismatch".into());
    }
    private_dir(directory)?;
    let key = format!(
        "{:x}",
        Sha256::digest(json!([path, conversation]).to_string())
    );
    let cache_path = directory.join(format!("{key}.json"));
    // Explicitly unlock on drop: a concurrent fork may inherit the descriptor,
    // keeping a close-only flock alive until the child executes or exits.
    let Some(_guard) = lock_until(
        &directory.join(format!("{key}.lock")),
        std::time::Instant::now(),
    )?
    else {
        return Ok(());
    };
    let mut cache: Value = open_regular(&cache_path)
        .ok()
        .and_then(|f| serde_json::from_reader(f.take(8 * 1024 * 1024)).ok())
        .unwrap_or(Value::Null);
    let identity = json!([source.device, source.inode, conversation]);
    if cache["source"] != identity
        || !cache["settled"].is_object()
        || !cache["targets"].is_array()
        || cache["offset"].as_u64().is_none_or(|n| n > meta.len())
    {
        cache = json!({"source":identity,"offset":0,"targets":[],"settled":{}});
    }
    let before = cache.clone();
    for call in &targets {
        if let Some(event) = cache["settled"].get(call) {
            native::codex(record, std::slice::from_ref(event), jobs);
        }
    }
    let missing: BTreeSet<_> = targets
        .into_iter()
        .filter(|call| cache["settled"].get(call).is_none())
        .collect();
    if missing.is_empty() {
        return Ok(());
    }
    let known: BTreeSet<String> =
        serde_json::from_value(cache["targets"].clone()).unwrap_or_default();
    let offset = if missing.is_subset(&known) {
        cache["offset"].as_u64().unwrap_or(0)
    } else {
        0
    };
    reader
        .seek(SeekFrom::Start(offset))
        .map_err(|e| e.to_string())?;
    let mut through = offset;
    let mut discarding = offset > 0 && cache["discarding"] == true;
    while through < meta.len() && through.saturating_sub(offset) < budget {
        let mut line = Vec::new();
        (&mut reader)
            .take(LINE_BYTES + 1)
            .read_until(b'\n', &mut line)
            .map_err(|e| e.to_string())?;
        if line.is_empty()
            || (!discarding && !line.ends_with(b"\n") && line.len() <= LINE_BYTES as usize)
        {
            break;
        }
        if discarding || line.len() > LINE_BYTES as usize {
            // Advance in bounded steps even across an unusually large JSON line.
            discarding = !line.ends_with(b"\n");
            through = reader.stream_position().map_err(|e| e.to_string())?;
            continue;
        }
        through = reader.stream_position().map_err(|e| e.to_string())?;
        let text = String::from_utf8_lossy(&line);
        if !missing.iter().any(|call| text.contains(call)) {
            continue;
        }
        let Ok(event) = serde_json::from_slice::<Value>(&line) else {
            continue;
        };
        let payload = &event["payload"];
        let item = &payload["item"];
        let call = string(item, "id");
        if payload["type"] != "item_completed"
            || !missing.contains(call)
            || !matches!(
                string(item, "type"),
                "CommandExecution" | "commandExecution"
            )
            || !matches!(
                string(item, "status"),
                "completed" | "failed" | "declined" | "interrupted"
            )
        {
            continue;
        }
        // The cache is bounded by manifest identities, with a small plain-text tail.
        let output = text_value(
            item.get("aggregated_output")
                .or_else(|| item.get("aggregatedOutput")),
        );
        let compact = json!({"timestamp":event["timestamp"],"payload":{
            "type":"item_completed","completed_at_ms":payload["completed_at_ms"],
            "item":{"type":"CommandExecution","id":call,"status":item["status"],
                "exit_code":item.get("exit_code").or_else(||item.get("exitCode")).and_then(Value::as_i64),
                "process_id":item.get("process_id").or_else(||item.get("processId")).and_then(Value::as_str).filter(|s|token(s)),
                "aggregated_output":tail(&output,4096),"hgs_output_truncated":output.len()>4096}}});
        native::codex(record, std::slice::from_ref(&compact), jobs);
        cache["settled"][call] = compact;
    }
    cache["offset"] = json!(through);
    cache["discarding"] = json!(discarding);
    cache["targets"] = json!(missing);
    if let Some(settled) = cache["settled"].as_object_mut() {
        settled.retain(|call, _| record["shell_jobs"].get(id(record, "main", call)).is_some());
    }
    if cache != before {
        atomic(&cache_path, &cache.to_string())?;
    }
    Ok(())
}

fn text_value(value: Option<&Value>) -> String {
    value.map(super::text).unwrap_or_default()
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::Write;

    #[test]
    fn incremental_recovery_caches_exact_completion_and_rejects_other_conversation() {
        let tmp = tempfile::tempdir().unwrap();
        let path = tmp.path().join("native.jsonl");
        let directory = tmp.path().join("cache");
        let mut r = super::super::tests::record("codex");
        r["transcript"] = json!(path);
        let mut old = new_job(&r, "main", "old", "echo old", 1.);
        old["updated_at"] = json!(1.);
        let current = new_job(&r, "main", "current", "sleep", 1.);
        let initial = Jobs::from([
            (id(&r, "main", "old"), old),
            (id(&r, "main", "current"), current),
        ]);
        r["shell_jobs"] = json!(initial);
        r["active_tools"] = json!({"current":{}});
        let mut source = std::fs::File::create(&path).unwrap();
        writeln!(
            source,
            "{}",
            json!({"type":"session_meta","payload":{"id":r["conversation_id"]}})
        )
        .unwrap();
        // More than the normal 8 MiB native window separates completion from EOF.
        let done = json!({"timestamp":"2026-10-07T01:00:01Z","payload":{"type":"item_completed","item":{
            "type":"CommandExecution","id":"old","command":"echo old","status":"failed","exit_code":7,"aggregated_output":"x".repeat(6000)}}});
        writeln!(source, "{}", json!({"padding":"p".repeat(4096)})).unwrap();
        let mut jobs = initial.clone();
        recover(&r, &mut jobs, &directory, 1000).unwrap();
        assert_eq!(jobs[&id(&r, "main", "old")]["status"], "starting");
        // Partial native appends must not be consumed until the line is complete.
        write!(source, "{}", done).unwrap();
        source.flush().unwrap();
        recover(&r, &mut jobs, &directory, 64 * 1024).unwrap();
        assert_eq!(jobs[&id(&r, "main", "old")]["status"], "starting");
        writeln!(source).unwrap();
        for _ in 0..9 {
            writeln!(source, "{}", json!({"padding":"p".repeat(1024*1024-100)})).unwrap();
        }
        source.flush().unwrap();
        recover(&r, &mut jobs, &directory, 64 * 1024).unwrap();
        let job = &jobs[&id(&r, "main", "old")];
        assert_eq!(job["status"], "failed");
        assert_eq!(job["command"], "echo old");
        assert_eq!(job["cwd"], "/workspace");
        assert_eq!(job["exit_code"], 7);
        assert_eq!(job["output_truncated"], true);
        assert_eq!(string(job, "output").len(), 4096);
        assert_eq!(jobs[&id(&r, "main", "current")]["status"], "starting");
        let mut again = initial.clone();
        recover(&r, &mut again, &directory, 1).unwrap(); // cached receipt, no history rescan
        assert_eq!(again[&id(&r, "main", "old")]["status"], "failed");
        r["conversation_id"] = json!(uuid::Uuid::new_v4().to_string());
        assert!(recover(&r, &mut initial.clone(), &directory, 1024).is_err());
    }
}
