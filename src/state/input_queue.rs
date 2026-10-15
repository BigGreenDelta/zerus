//! Native TUI queue snapshots. Sending now invokes the provider's displayed
//! action, never pastes the message again. Unknown key bindings stay read-only.
use super::*;
use serde::Deserialize;
use sha2::{Digest, Sha256};

#[derive(Debug)]
struct Queue { text: String, keys: Vec<&'static str>, hint: String }
fn parse(agent: &str, screen: &str) -> Option<Queue> {
    let rows: Vec<_> = screen.lines().collect();
    if agent == "codex" {
        let start = rows.iter().rposition(|r|r.contains("Messages to be submitted after next tool call")
            || r.trim() == "• Queued follow-up inputs" || r.contains("Messages to be submitted at end of turn"))?;
        // Include preceding pending-steer sections when multiple queue types coexist.
        let start = rows[..=start].iter().rposition(|r|r.contains("Messages to be submitted after next tool call")).unwrap_or(start);
        let end = rows[start+1..].iter().position(|r|r.trim_start().starts_with('›') || r.trim_start().starts_with('╭')).map(|i|start+1+i).unwrap_or(rows.len());
        let block = rows[start..end].join("\n");
        if !block.contains('↳') { return None; }
        let normalized = block.split_whitespace().collect::<Vec<_>>().join(" ");
        let can = normalized.contains("press esc to interrupt and send immediately");
        return Some(Queue { text: block.trim().into(), keys: if can {vec!["Escape"]} else {vec![]},
            hint: if can {"Interrupt the current turn and send pending messages now"} else {"Use Terminal to edit or submit these queued follow-ups"}.into() });
    }
    let end = rows.iter().rposition(|row| {
        let row = row.trim();
        if agent == "claude" { row == "ctrl+x ctrl+s to send now" }
        else if agent == "kimi" { row.starts_with("↑ to edit") && (row.contains("ctrl-s to steer immediately") || row.contains("will send after")) }
        else { false }
    })?;
    let mut start = end;
    for i in (end.saturating_sub(80)..end).rev() {
        let row = rows[i];
        if row.trim().is_empty() || row.trim().chars().all(|c|c=='─') { break; }
        if row.trim_start().starts_with('❯') { start = i; }
        else if !row.starts_with(' ') { break; }
    }
    if start == end { return None; }
    let text = rows[start..end].join("\n").trim().to_owned();
    let keys = if agent == "claude" { vec!["C-x", "C-s"] }
        else if rows[end].contains("ctrl-s to steer immediately") {vec!["C-s"]} else {vec![]};
    Some(Queue { text, keys, hint: if agent == "claude" {"Send the queued messages now"}
        else {"Steer the agent with the queued messages now"}.into() })
}
fn eligible(record: &Value) -> bool {
    record["activity"] == "busy" && ["working","tool","compacting","idle"].contains(&string(record,"phase"))
        && record["run_identity_version"] == 1 && record["supervisor"].is_object()
        && !string(record,"conversation_id").is_empty() && string(record,"expected_id").is_empty()
        && string(record,"error").is_empty() && !pause_active(record)
        && ["codex","claude","kimi"].contains(&string(record,"agent"))
}
fn snapshot(record: &Value, queue: &Queue) -> Value {
    let id = format!("{:x}",Sha256::digest(json!([record["run_id"],record["conversation_id"],record["pane"],record["pid"],record["process_start"],queue.text,queue.keys]).to_string()));
    json!({"id":id,"text":queue.text,"can_send_now":!queue.keys.is_empty() && record["phase"]!="compacting", "hint":queue.hint})
}
pub(super) fn inspection(record: &Value, live: bool) -> Option<Value> {
    if !live || !eligible(record) {return None;}
    let queue = parse(string(record,"agent"),&kimi_tui_choice::capture(record).ok()?)?;
    kimi_tui_choice::identity(record).ok()?;
    let mut data = snapshot(record,&queue);
    // Never submit an unrelated draft together with the queue.
    if input::checked_terminal(record,true).is_err() {
        data["can_send_now"] = json!(false);
        data["hint"] = json!("Return Terminal to an empty input before sending the queue now");
    }
    Some(data)
}
#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct Request { request_id: String, expected_run_id: String, expected_conversation_id: String, queue_id: String }
pub(super) fn dispatch(args: &[String]) -> Result<i32> {
    if args.len()!=2 || args[1]!="--json" {return Err("usage: hgs send-now <session> --json".into());}
    let mut bytes=Vec::new();io::stdin().take(8193).read_to_end(&mut bytes).map_err(|e|e.to_string())?;
    if bytes.len()>8192 {return Err("queue request is too large".into());}
    let request:Request=serde_json::from_slice(&bytes).map_err(|e|e.to_string())?;
    let uuid=uuid::Uuid::parse_str(&request.request_id).map_err(|_|"request_id must be a UUID")?;
    let _guard=lock(None)?;
    let record=read(&args[0])?;
    if record["run_id"]!=request.expected_run_id || record["conversation_id"]!=request.expected_conversation_id || !eligible(&record) {
        return Err("Session changed or is no longer working; refresh Activity".into());
    }
    let path=absolute_root()?.join("queue_receipts").join(format!("{uuid}.json"));
    if path.exists() {return Err("This send-now request was already attempted. Refresh Activity before trying again".into());}
    let current=kimi_tui_choice::identity(&record)?;
    input::checked_terminal(&current,true)?;
    let screen=kimi_tui_choice::capture(&current)?;
    let queue=parse(string(&current,"agent"),&screen).ok_or("The native queue is no longer visible; refresh Activity")?;
    let data=snapshot(&current,&queue);
    if data["id"]!=request.queue_id || data["can_send_now"]!=true {return Err("The queue changed or cannot be sent now; refresh Activity".into());}
    let receipt=json!({"request_id":request.request_id,"name":args[0],"run_id":request.expected_run_id,
        "conversation_id":request.expected_conversation_id,"queue_id":request.queue_id,"status":"submitted"});
    atomic(&path,&json!({"status":"in_progress"}).to_string())?;
    // Recheck right before the native shortcut; no generic Enter, Escape retry,
    // or synthetic re-submission if the provider has already consumed the queue.
    kimi_tui_choice::identity(&current)?;
    let latest=parse(string(&current,"agent"),&kimi_tui_choice::capture(&current)?).ok_or("Queue disappeared before sending")?;
    if snapshot(&current,&latest)["id"]!=request.queue_id {return Err("Queue changed before sending".into());}
    let keys:Vec<_>=queue.keys.iter().map(|key|backend::Key::Named(key)).collect();
    backend::current().send(string(&current,"pane"),&keys).map_err(|e|format!("Delivery uncertain: {e}. Check Terminal before trying again"))?;
    atomic(&path,&receipt.to_string())?;println!("{receipt}");Ok(0)
}
#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn recognizes_only_provider_queue_controls_and_keeps_wrapped_text() {
        let c="old output\n\n❯ first queued message\n  second line\nctrl+x ctrl+s to send now\n\n❯ \n";
        let q=parse("claude",c).unwrap();assert!(q.text.contains("second line"));assert_eq!(q.keys,vec!["C-x","C-s"]);
        assert!(parse("claude",&c.replace("ctrl+x ctrl+s","ctrl+z")).is_none());
        assert!(parse("kimi",c).is_none());
        let k="─────────────────────────\n  ❯ queued…\n  ↑ to edit · ctrl-s to steer immediately\n──────────────────────\n > \n";
        assert_eq!(parse("kimi",k).unwrap().keys,vec!["C-s"]);
        assert!(parse("kimi",&k.replace("ctrl-s to steer immediately","will send after current task")).unwrap().keys.is_empty());
        let codex="• Messages to be submitted after next tool call (press esc to interrupt and send immediately)\n  ↳ hello\n\n› \n";
        assert_eq!(parse("codex",codex).unwrap().keys,vec!["Escape"]);
        assert!(parse("codex",&codex.replace("press esc","press ctrl+c")).unwrap().keys.is_empty());
        let record=json!({"run_id":"a","conversation_id":"b","pane":"%1","pid":1,"process_start":"start","phase":"tool"});
        assert_ne!(snapshot(&record,&q)["id"],snapshot(&record,&parse("claude",&c.replace("first","changed")).unwrap())["id"]);
        let mut other=record.clone();other["run_id"]=json!("new");assert_ne!(snapshot(&record,&q)["id"],snapshot(&other,&q)["id"]);
    }
    #[test]
    fn codex_ordinary_follow_ups_stay_read_only() {
        // Panels captured from the pinned Codex 0.160.1 and 0.162.0 (110x35). Ordinary
        // (Tab) follow-ups offer only "edit last queued message", which pops the last one
        // into the composer over any draft. Escape interrupts and leaves them all there
        // as one unsent draft, so neither may stand in for a native "send now".
        let ordinary="• Working (1s • esc to interrupt)\n\n• Queued follow-up inputs\n  ↳ Queued follow-up one\n  ↳ Queued follow-up two\n    shift+← edit last queued message\n\n\n› Ask Codex to do anything\n";
        let q=parse("codex",ordinary).unwrap();
        assert!(q.keys.is_empty());assert!(q.text.contains("Queued follow-up one")&&q.text.contains("Queued follow-up two"));
        assert_eq!(q.hint,"Use Terminal to edit or submit these queued follow-ups");
        let record=json!({"phase":"working"});
        assert_eq!(snapshot(&record,&q)["can_send_now"],false);
        // Steers listed above them keep the displayed Escape: 0.160.1 then submits the
        // steer at once and the ordinary follow-up after that turn, in order.
        let mixed="• Working (2s • esc to interrupt)\n\n• Messages to be submitted after next tool call (press esc to interrupt and send immediately)\n  ↳ Steer three\n\n• Queued follow-up inputs\n  ↳ Queued follow-up one\n    shift+← edit last queued message\n\n\n› Ask Codex to do anything\n";
        let q=parse("codex",mixed).unwrap();
        assert_eq!(q.keys,vec!["Escape"]);assert!(q.text.contains("Steer three")&&q.text.contains("Queued follow-up one"));
    }
}
