//! Native manual compaction. A submitted command is not a completed compaction.
use super::*;
use serde::Deserialize;
use std::time::{Duration, Instant};

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct Request { request_id: String, expected_run_id: String, expected_conversation_id: String }

pub(super) fn pending(record: &Value) -> bool {
    ["submitted", "compacting", "uncertain"].contains(&string(&record["compact_context_request"], "status"))
}
pub(super) fn check_continuation(record: &Value, id: &str) -> Result<()> {
    if id.is_empty() { return Ok(()); }
    let request = &record["compact_context_request"];
    if request["request_id"] != id || request["status"] != "completed"
        || request["run_id"] != record["run_id"] || request["conversation_id"] != record["conversation_id"]
        || record["activity"] != "idle" || record["phase"] != "idle" {
        return Err("Compaction is not confirmed for this conversation; your message was not sent".into());
    }
    Ok(())
}
pub(super) fn observe(record: &mut Value, event: &Value) {
    if !string(event, "agent_id").is_empty() { return; }
    let mut request = record["compact_context_request"].clone();
    if !request.is_object() || request["run_id"] != record["run_id"]
        || request["conversation_id"] != record["conversation_id"] { return; }
    let status = string(&request, "status").to_owned();
    let kind = string(event, "hook_event_name");
    match kind {
        "PreCompact" if status == "submitted" && event["trigger"] == "manual" => {
            request["status"] = json!("compacting");
            request["turn_id"] = event["turn_id"].clone();
        }
        "PostCompact" if status == "compacting" && event["trigger"] == "manual"
            && request["turn_id"] == event["turn_id"] => {
            request["status"] = json!("completed");
            request["completed_at"] = json!(now());
        }
        "Interrupt" | "StopFailure" | "SessionEnd" | "UserPromptSubmit" | "PermissionRequest" => {
            request["status"] = json!("cancelled");
        }
        _ => return,
    }
    record["compact_context_request"] = request;
}
pub(super) fn dispatch(args: &[String]) -> Result<i32> {
    if args.len() != 2 || args[1] != "--json" { return Err("usage: hgs compact-context <session> --json".into()); }
    if dsh::exists(&args[0]) { return dsh::dispatch("compact-context", args); }
    let mut bytes = Vec::new(); io::stdin().take(8193).read_to_end(&mut bytes).map_err(|e|e.to_string())?;
    if bytes.len() > 8192 { return Err("Compact request is too large".into()); }
    let request: Request = serde_json::from_slice(&bytes).map_err(|e|e.to_string())?;
    let uuid = uuid::Uuid::parse_str(&request.request_id).map_err(|_|"Invalid request identity")?;
    let name = &args[0];
    let guard = lock(None)?;
    let mut record = input::checked_ready(name, &request.expected_run_id, &request.expected_conversation_id)?;
    if !clear_context::available(&record, true) || record["pending_settings"].is_object() {
        return Err("Wait for the agent to become idle before compacting context".into());
    }
    input::checked_terminal(&record, true)?;
    let path = absolute_root()?.join("compact_receipts").join(format!("{uuid}.json"));
    if path.exists() { return Err("This compact was already attempted. Check Terminal before trying again".into()); }
    recovery::cancel_for_input(name)?;
    let mut receipt = json!({"request_id":request.request_id,"name":name,"run_id":request.expected_run_id,
        "conversation_id":request.expected_conversation_id,"status":"submitted","at":now()});
    record["compact_context_request"] = receipt.clone();
    write(&mut record)?;
    atomic(&path, &json!({"status":"in_progress"}).to_string())?;
    let result: Result<()> = (|| {
        tmux(&["send-keys","-t",string(&record,"pane"),"-l","/compact"].map(str::to_owned), true)?;
        let deadline = Instant::now() + Duration::from_secs(3);
        loop {
            let current = kimi_tui_choice::identity(&record)?;
            if clear_context::ready(&current, "/compact")? { break; }
            if Instant::now() >= deadline { return Err("Native /compact input could not be verified".into()); }
            std::thread::sleep(Duration::from_millis(40));
        }
        std::thread::sleep(clear_context::COMMAND_SETTLE);
        let current = input::checked_ready(name, &request.expected_run_id, &request.expected_conversation_id)?;
        kimi_tui_choice::identity(&record)?;
        input::checked_terminal(&current, false)?;
        if !clear_context::ready(&current, "/compact")? { return Err("The native input changed before /compact".into()); }
        tmux(&["send-keys","-t",string(&current,"pane"),"Enter"].map(str::to_owned), true)?;
        Ok(())
    })();
    if let Err(error) = result {
        receipt["status"] = json!("uncertain");
        record["compact_context_request"] = receipt.clone(); write(&mut record)?;
        atomic(&path, &receipt.to_string())?;
        return Err(format!("Compact delivery uncertain: {error}. Check Terminal before retrying"));
    }
    drop(guard); // Native hooks must be free to report start/completion.
    atomic(&path, &receipt.to_string())?;
    println!("{receipt}"); Ok(0)
}

#[cfg(test)] mod tests {
    use super::*;
    #[test] fn only_matching_success_allows_continuation() {
        let r = json!({"run_id":"r","conversation_id":"c","activity":"idle","phase":"idle",
            "compact_context_request":{"request_id":"q","run_id":"r","conversation_id":"c","status":"submitted"}});
        let pre = json!({"hook_event_name":"PreCompact","trigger":"manual","turn_id":"t"});
        let post = json!({"hook_event_name":"PostCompact","trigger":"manual","turn_id":"t"});
        let mut state = r.clone(); observe(&mut state,&post); assert!(check_continuation(&state,"q").is_err());
        observe(&mut state,&pre); observe(&mut state,&post); assert!(check_continuation(&state,"q").is_ok());
        assert!(check_continuation(&state,"other").is_err());
        for interruption in ["Interrupt","StopFailure","SessionEnd","UserPromptSubmit"] {
            let mut state = r.clone(); observe(&mut state,&pre);
            observe(&mut state,&json!({"hook_event_name":interruption})); observe(&mut state,&post);
            assert!(check_continuation(&state,"q").is_err());
        }
        for field in ["trigger","turn_id","agent_id"] {
            let mut state = r.clone(); observe(&mut state,&pre); let mut wrong=post.clone(); wrong[field]=json!("other");
            observe(&mut state,&wrong); assert!(check_continuation(&state,"q").is_err());
        }
    }
}
