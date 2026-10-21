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
        // Panel headers begin their row; transcript text quoting them (diffs, source,
        // tool output) must never select a queue or its Escape action.
        let header = |r: &str, h: &str| r.trim_start().starts_with(h);
        const STEER: &str = "• Messages to be submitted after next tool call";
        let start = rows.iter().rposition(|r|header(r,STEER)
            || r.trim() == "• Queued follow-up inputs" || header(r,"• Messages to be submitted at end of turn"))?;
        // Include a pending-steer section only when it directly precedes this one in the
        // same panel (its indented items and blank separators), never an earlier one.
        let mut first = start;
        if !header(rows[start],STEER) {
            for i in (0..start).rev() {
                if header(rows[i],STEER) { first = i; break; }
                if !(rows[i].trim().is_empty() || rows[i].starts_with(' ')) { break; }
            }
        }
        let start = first;
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
/// Stop's Escape returns ordinary Codex follow-ups to its input as one unsent draft
/// (pinned 0.160.1 and 0.162.0). Stop notes the ones this turn's panel shows.
const STOP_FOLLOW_UPS: &str = "codex_stop_follow_ups";
/// The text, without whitespace, of a pinned Codex panel listing ordinary follow-ups
/// alone: its header, `↳` items with indented continuation rows, then its edit hint.
/// A clipped item never equals the full text Codex returns to its input.
fn ordinary_follow_ups(screen: &str) -> Option<String> {
    let queue = parse("codex", screen).filter(|queue| queue.keys.is_empty())?;
    let rows: Vec<&str> = queue.text.lines().map(str::trim_end).collect();
    let [header, items @ .., hint] = &rows[..] else { return None };
    if *header != "• Queued follow-up inputs" || *hint != "    shift+← edit last queued message"
        || !items.first()?.starts_with("  ↳ ") { return None; }
    let mut text = String::new();
    for row in items {
        text += row.strip_prefix("  ↳ ").or_else(|| row.strip_prefix("    ")).or(row.is_empty().then_some(""))?;
    }
    Some(interrupt::squash(&text)).filter(|text| !text.is_empty())
}
fn digest(text: &str) -> String { format!("{:x}", Sha256::digest(text.as_bytes())) }
/// Before Stop's Escape: note the ordinary follow-ups Codex shows for this turn, or
/// forget an earlier note.
pub(super) fn note_stop(record: &mut Value) {
    let noted = kimi_tui_choice::capture(record).ok().and_then(|screen| ordinary_follow_ups(&screen))
        .map(|text| json!({"run_id":record["run_id"],"conversation_id":record["conversation_id"],
            "turn_started":record["turn_started"],"digest":digest(&text)}));
    let Some(fields) = record.as_object_mut() else { return };
    match noted {
        Some(noted) => { fields.insert(STOP_FOLLOW_UPS.into(), noted); }
        None => { fields.remove(STOP_FOLLOW_UPS); }
    }
}
/// The digest of the whole draft in Codex's input once no queue is shown.
fn returned_draft(record: &Value, screen: &str, cursor_y: usize) -> Option<String> {
    if parse("codex", screen).is_some() { return None; }
    input::codex_draft(record, screen, cursor_y).map(|draft| digest(&interrupt::squash(&draft)))
}
/// Whether Codex reported this turn interrupted and its input holds exactly the
/// follow-ups Stop noted for it: nothing added, removed or changed but wrapping.
pub(super) fn returned_by_stop(record: &Value) -> bool {
    let noted = &record[STOP_FOLLOW_UPS];
    record["agent"] == "codex" && record["phase"] == "interrupted" && noted["digest"].is_string()
        && ["run_id", "conversation_id", "turn_started"].iter().all(|key| noted[*key] == record[*key])
        && kimi_tui_choice::capture(record).ok().zip(interrupt::cursor(record))
            .and_then(|(screen, (_, y))| returned_draft(record, &screen, y))
            .is_some_and(|draft| noted["digest"] == draft)
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
        return Err("Session changed or is no longer working; nothing was sent".into());
    }
    let path=absolute_root()?.join("queue_receipts").join(format!("{uuid}.json"));
    if path.exists() {return Err("This send-now request was already attempted; nothing was sent again".into());}
    let current=kimi_tui_choice::identity(&record)?;
    input::checked_terminal(&current,true)?;
    let screen=kimi_tui_choice::capture(&current)?;
    let queue=parse(string(&current,"agent"),&screen).ok_or("The native queue is no longer visible; nothing was sent")?;
    let data=snapshot(&current,&queue);
    if data["id"]!=request.queue_id || data["can_send_now"]!=true {return Err("The queue changed or cannot be sent now; nothing was sent".into());}
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
    // Panel captured from the pinned Codex 0.160.1 and 0.162.0 (110x35).
    const ORDINARY:&str="• Working (1s • esc to interrupt)\n\n• Queued follow-up inputs\n  ↳ Queued follow-up one\n  ↳ Queued follow-up two\n    shift+← edit last queued message\n\n\n› Ask Codex to do anything\n";
    #[test]
    fn codex_ordinary_follow_ups_stay_read_only() {
        // Ordinary (Tab) follow-ups offer only "edit last queued message", which pops the
        // last one into the composer over any draft. Escape interrupts and leaves them all
        // there as one unsent draft, so neither may stand in for a native "send now".
        let ordinary=ORDINARY;
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
    #[test]
    fn codex_queue_headers_in_transcript_never_offer_escape() {
        // Transcript output (here a displayed diff of a queue fixture) can quote a pending
        // steer header, its Escape hint and an item marker on one row. Only the panel
        // itself may decide the action; generic Escape would interrupt the turn.
        let quoted="• Edited src/state/input_queue.rs (+1 -0)\n    113 +        let codex=\"• Messages to be submitted after next tool call (press esc to interrupt and send immediately)\\n  ↳ hello\\n\\n› \\n\";\n\n";
        let ordinary=format!("{quoted}• Working (3s • esc to interrupt)\n\n• Queued follow-up inputs\n  ↳ Queued follow-up one\n    shift+← edit last queued message\n\n\n› Ask Codex to do anything\n");
        let q=parse("codex",&ordinary).unwrap();
        assert!(q.keys.is_empty());assert_eq!(q.hint,"Use Terminal to edit or submit these queued follow-ups");
        assert!(q.text.starts_with("• Queued follow-up inputs")&&!q.text.contains("113 +"));
        assert!(parse("codex",&format!("{quoted}• Working (3s • esc to interrupt)\n\n\n› Ask Codex to do anything\n")).is_none());
        // A real steer panel below the same transcript still offers its displayed Escape.
        let steer=format!("{quoted}• Working (3s • esc to interrupt)\n\n• Messages to be submitted after next tool call\n  (press esc to interrupt and send immediately)\n  ↳ Steer three\n\n• Queued follow-up inputs\n  ↳ Queued follow-up one\n    shift+← edit last queued message\n\n\n› Ask Codex to do anything\n");
        let q=parse("codex",&steer).unwrap();
        assert_eq!(q.keys,vec!["Escape"]);assert!(q.text.starts_with("• Messages to be submitted after next tool call")&&q.text.contains("Queued follow-up one"));
    }
    #[test]
    fn codex_stop_leaves_ordinary_follow_ups_as_an_unsent_draft() {
        // Whole screens of the pinned Codex around its Escape (hgs Stop), with only
        // ordinary follow-ups queued. They hold the panel above, which stays read-only;
        // after Escape no queue is left to show or send: Codex put both follow-ups back
        // into its input as one unsent draft, and its provider received neither.
        let panel=parse("codex",ORDINARY).unwrap();
        for (version,queued,stopped) in [
            ("0.160.1",include_str!("../../tests/fixtures/codex-queued-follow-ups-0.160.1.txt"),
                include_str!("../../tests/fixtures/codex-queued-follow-ups-escape-0.160.1.txt")),
            ("0.162.0",include_str!("../../tests/fixtures/codex-queued-follow-ups.txt"),
                include_str!("../../tests/fixtures/codex-queued-follow-ups-escape.txt"))] {
            let queued=question_terminal::plain(queued).unwrap();
            assert!(queued.contains(&format!("OpenAI Codex (v{version})")));
            let q=parse("codex",&queued).unwrap();
            assert_eq!(q.text,panel.text,"{version}");assert!(q.keys.is_empty(),"{version}");
            assert_eq!(q.hint,"Use Terminal to edit or submit these queued follow-ups");
            let stopped=question_terminal::plain(stopped).unwrap();
            assert!(parse("codex",&stopped).is_none(),"{version}");
            assert!(stopped.contains("■ Conversation interrupted"),"{version}");
            assert!(stopped.contains("\n› Queued follow-up one\n  Queued follow-up two\n"),"{version}");
        }
    }
    #[test]
    fn codex_stop_draft_is_recognized_only_as_the_noted_follow_ups() {
        // Stop notes the panel's ordinary follow-ups before Escape. Afterwards the
        // whole draft in Codex's input must be exactly them, with the cursor in it.
        let record=json!({});
        for (version,queued,stopped) in [
            ("0.160.1",include_str!("../../tests/fixtures/codex-queued-follow-ups-0.160.1.txt"),
                include_str!("../../tests/fixtures/codex-queued-follow-ups-escape-0.160.1.txt")),
            ("0.162.0",include_str!("../../tests/fixtures/codex-queued-follow-ups.txt"),
                include_str!("../../tests/fixtures/codex-queued-follow-ups-escape.txt"))] {
            let queued=question_terminal::plain(queued).unwrap();
            let stopped=question_terminal::plain(stopped).unwrap();
            let noted=ordinary_follow_ups(&queued).unwrap();
            assert_eq!(noted,"Queuedfollow-uponeQueuedfollow-uptwo","{version}");
            assert_eq!(ordinary_follow_ups(ORDINARY).as_deref(),Some(noted.as_str()));
            assert_eq!(input::codex_draft(&record,&stopped,31).as_deref(),Some("Queued follow-up one\nQueued follow-up two"),"{version}");
            for y in [30,31] {assert_eq!(returned_draft(&record,&stopped,y),Some(digest(&noted)),"{version} {y}");}
            // While the panel is shown, or with the cursor outside the draft, nothing is read.
            assert_eq!(returned_draft(&record,&queued,31),None,"{version}");
            for y in [29,32,33] {assert_eq!(returned_draft(&record,&stopped,y),None,"{version} {y}");}
            // Any other draft: edited or shortened follow-ups, or more text after them, such
            // as a draft that was already in the input (Codex appends it on a new line).
            const RETURNED:&str="\n› Queued follow-up one\n  Queued follow-up two\n\n";
            for (other,y) in [("\n› Queued follow-up one\n  Queued follow-up two and more\n\n",31),
                ("\n› Queued follow-up one\n\n\n",30),
                ("› Queued follow-up one\n  Queued follow-up two\n  An earlier draft\n\n",31)] {
                let screen=stopped.replacen(RETURNED,other,1);
                assert_ne!(screen,stopped);assert_eq!(screen.lines().count(),stopped.lines().count());
                assert!(returned_draft(&record,&screen,y).is_some_and(|draft|draft!=digest(&noted)),"{version} {other:?}");
            }
            // Without the blank row and model/folder status below it, the draft's end is unknown.
            for (from,to) in [("  Queued follow-up two\n\n","  Queued follow-up two\nUnindented\n"),
                ("  GPT-6.1-Sol default · ~/work","  Custom status line")] {
                assert_eq!(returned_draft(&record,&stopped.replacen(from,to,1),31),None,"{version} {to}");
            }
        }
        // Pending steers send on Escape, and other panel layouts are not noted.
        let steer="• Messages to be submitted after next tool call (press esc to interrupt and send immediately)\n  ↳ Steer three\n\n• Queued follow-up inputs\n  ↳ Queued follow-up one\n    shift+← edit last queued message\n\n\n› Ask Codex to do anything\n";
        assert_eq!(ordinary_follow_ups(steer),None);
        assert_eq!(ordinary_follow_ups(&ORDINARY.replace("shift+← edit last queued message","alt+↑ edit")),None);
        assert_eq!(ordinary_follow_ups(&ORDINARY.replace("  ↳ Queued follow-up two","Queued follow-up two")),None);
        // Wrapped items continue on indented rows.
        assert_eq!(ordinary_follow_ups(&ORDINARY.replace("  ↳ Queued follow-up two","  ↳ Queued follow-up\n    two")).as_deref(),
            Some("Queuedfollow-uponeQueuedfollow-uptwo"));
    }
}
