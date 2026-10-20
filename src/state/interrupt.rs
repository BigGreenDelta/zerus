//! One explicit native interrupt, pinned to the displayed turn. Never kills
//! the session or retries a keystroke whose delivery might have succeeded.
use super::*;
use serde::Deserialize;

fn eligible(record: &Value) -> bool {
    ["codex", "claude", "kimi"].contains(&string(record, "agent"))
        && record["run_identity_version"] == 1 && record["supervisor"].is_object()
        && record["activity"] == "busy"
        && ["working", "tool", "compacting"].contains(&string(record, "phase"))
        && !pause_active(record) && string(record, "error").is_empty()
}
pub(super) fn available(record: &Value, live: bool) -> bool { live && eligible(record) }

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct Request { request_id: String, expected_run_id: String, expected_conversation_id: String, expected_turn_started: f64 }

pub(super) fn dispatch(args: &[String]) -> Result<i32> {
    if args.len()!=2 || args[1]!="--json" { return Err("usage: hgs interrupt <session> --json".into()); }
    let mut bytes=Vec::new(); io::stdin().take(8193).read_to_end(&mut bytes).map_err(|e|e.to_string())?;
    if bytes.len()>8192 {return Err("Interrupt request is too large".into());}
    let request:Request=serde_json::from_slice(&bytes).map_err(|e|e.to_string())?;
    let uuid=uuid::Uuid::parse_str(&request.request_id).map_err(|_|"Invalid request identity")?;
    let guard=lock(None)?;
    let name=&args[0];let open_native=opencode::exists(name);let native=dsh::exists(name) || open_native;
    let record=if open_native {opencode::own_state(name)?} else if native {dsh::inspect(name,None)?} else {read(name)?};
    if request.expected_run_id.is_empty() || record["run_id"]!=request.expected_run_id
        || record["conversation_id"]!=request.expected_conversation_id
        || record["turn_started"].as_f64().unwrap_or(0.0)!=request.expected_turn_started {
        return Err("The session or turn changed; nothing was stopped".into());
    }
    let path=absolute_root()?.join("interrupt_receipts").join(format!("{uuid}.json"));
    if path.exists() {return Err("This interrupt was already attempted. Check Terminal before trying again".into());}
    if native {
        if record["interrupt_supported"]!=true {return Err("This DeepSeek turn cannot be interrupted from Activity. Open the native UI".into());}
    } else {
        if !eligible(&record) {return Err("This turn is no longer working".into());}
        kimi_tui_choice::identity(&record)?;
    }
    recovery::cancel_for_input(name)?;
    atomic(&path,&json!({"status":"in_progress"}).to_string())?;
    let mut seen=None;
    if open_native {opencode::interrupt(&record)?;}
    else if native { dsh::interrupt(&record)?; }
    else {
        let mut current=kimi_tui_choice::identity(&record)?;
        if !eligible(&current) || current["turn_started"]!=record["turn_started"] {return Err("The turn changed before interruption".into());}
        if current["agent"]=="claude" {seen=kimi_tui_choice::capture(&current).ok().map(|screen|interruptions(&screen));}
        // Codex returns ordinary queued follow-ups to its input on Escape.
        if current["agent"]=="codex" {input_queue::note_stop(&mut current);}
        compact_context::observe(&mut current,&json!({"hook_event_name":"Interrupt"}));
        write(&mut current)?;
        backend::current().send(string(&current,"pane"),&[backend::Key::Named("Escape")])
            .map_err(|e|format!("Interrupt delivery uncertain: {e}. Check Terminal before retrying"))?;
    }
    let mut receipt=json!({"request_id":request.request_id,"name":name,"run_id":request.expected_run_id,
        "conversation_id":request.expected_conversation_id,"status":"submitted"});
    atomic(&path,&receipt.to_string())?;
    if let Some(seen)=seen {
        // Hooks must not wait while Claude redraws.
        drop(guard);
        receipt["confirmed"]=json!(settle_claude(&record,seen));
    }
    println!("{receipt}");Ok(0)
}

// Claude prints this line when Escape stops a turn, and sends no hook.
const CLAUDE_INTERRUPTED:&str="Interrupted · What should Claude do instead?";
fn interruptions(screen:&str)->usize {screen.matches(CLAUDE_INTERRUPTED).count()}

/// Record the interruption once Claude shows one more interruption line than
/// before Escape, or once it has rewound the turn, so the session leaves
/// Working and the prompt can return.
fn settle_claude(original:&Value,seen:usize)->bool {
    let deadline=std::time::Instant::now()+std::time::Duration::from_secs(3);
    while std::time::Instant::now()<deadline {
        std::thread::sleep(std::time::Duration::from_millis(60));
        let Ok(screen)=kimi_tui_choice::capture(original) else {continue};
        if interruptions(&screen)<=seen {
            if !rewound(&screen,string(original,"prompt")) {continue;}
            // Activity restores the prompt, so it leaves Terminal; a draft that
            // cannot be taken safely stays there.
            take_rewound_prompt(original);
        }
        let settled=(||->Result<bool>{
            let _guard=lock(None)?;
            let Some(mut record)=read_run(string(original,"name"),string(original,"run_id"))? else {return Ok(false)};
            if record["turn_started"]!=original["turn_started"] || !eligible(&record) {return Ok(false);}
            let event=json!({"hook_event_name":"Interrupt"});
            journal::update_activity(&mut record,&event);
            questions::observe(&mut record,&event);
            journal::log_event(&record,&event)?;
            write(&mut record)?;Ok(true)
        })();
        return settled.unwrap_or(false);
    }
    false
}

/// Claude rewinds a turn stopped before its first reply: the turn leaves the
/// transcript and its prompt returns to the input, with no interruption line.
fn rewound(screen:&str,prompt:&str)->bool {
    returnable(prompt) && claude_input(screen).is_some_and(|(_,rows)|squash(&rows.concat())==squash(prompt))
}

/// Prompts Activity can restore in full: not clipped by the journal, not automatic.
fn returnable(prompt:&str)->bool {
    !squash(prompt).is_empty() && !prompt.ends_with('…') && !prompt.starts_with("[HGS automatic recovery]")
}

/// Wrapping changes only whitespace, so drafts compare without it.
pub(super) fn squash(text:&str)->String {text.chars().filter(|ch|!ch.is_whitespace()).collect()}

/// The rows of Claude's input box, without the ❯ prompt and continuation indent:
/// from the ❯ row under a full-width rule to the next rule.
fn claude_input(screen:&str)->Option<(usize,Vec<String>)> {
    let lines:Vec<&str>=screen.lines().collect();
    let rule=|line:&str|line.chars().count()>=12 && line.chars().all(|ch|"─━╌".contains(ch));
    let start=(1..lines.len()).rev().find(|&i|rule(lines[i-1]) && lines[i].starts_with('❯'))?;
    let end=(start..lines.len()).find(|&i|rule(lines[i]))?;
    Some((start,lines[start..end].iter().map(|row|row.chars().skip(2).collect::<String>().trim_end().to_owned()).collect()))
}

pub(super) fn cursor(record:&Value)->Option<(usize,usize)> {
    let cursor=backend::current().pane_info(backend::Target::Pane(string(record,"pane")),
        &[backend::PaneField::CursorX,backend::PaneField::CursorY]).ok()?;
    let (x,y)=cursor.pair()?;
    Some((x.parse().ok()?,y.parse().ok()?))
}

/// Deletes the rewound prompt from the end, one terminal row at a time: Ctrl+U
/// clears a row, Backspace removes the emptied row, and Ctrl+Y in Terminal
/// brings deleted text back. Each step must leave less of the prompt's start,
/// so nothing typed meanwhile is touched. True once the input is empty.
fn take_rewound_prompt(record:&Value)->bool {
    let target=squash(string(record,"prompt"));
    let pane=string(record,"pane");
    // Text and rows left: Ctrl+U shortens the text, Backspace removes a row.
    let left=|rows:&[String]|squash(&rows.concat()).len()+rows.len();
    let mut last:Option<usize>=None;
    loop {
        let Some((x,y))=cursor(record) else {return false};
        let Ok(styled)=backend::current().capture(pane,backend::Capture::Styled) else {return false};
        if last.is_some() && input::composer_empty("claude",&String::from_utf8_lossy(&styled),x,y) {return true;}
        let Some((start,rows))=kimi_tui_choice::capture(record).ok().and_then(|screen|claude_input(&screen)) else {return false};
        let now=squash(&rows.concat());
        if !target.starts_with(&now) || last.is_some_and(|last|left(&rows)>=last) {return false;}
        if last.is_none() {
            // Start only from the end of the whole prompt; wide characters make
            // the cursor column ambiguous.
            let end=rows.last().map(String::as_str).unwrap_or("");
            if now!=target || y!=start+rows.len()-1 || end.chars().any(|ch|ch>='\u{1100}') || x!=2+end.chars().count() {return false;}
        }
        last=Some(left(&rows));
        let key=if x<=2 {"BSpace"} else {"C-u"};
        if backend::current().send(pane,&[backend::Key::Named(key)]).is_err() {return false;}
        // Wait for Claude to redraw the input before the next step.
        let deadline=std::time::Instant::now()+std::time::Duration::from_millis(600);
        while std::time::Instant::now()<deadline {
            std::thread::sleep(std::time::Duration::from_millis(30));
            let input=kimi_tui_choice::capture(record).ok().and_then(|screen|claude_input(&screen));
            if input.is_none_or(|(_,rows)|Some(left(&rows))!=last) {break;}
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    const RULE:&str="──────────────────────────────";
    fn screen(input:&[&str])->String {format!("❯ earlier prompt\n⏺ Reply\n{RULE}\n{}\n{RULE}\n  ⏵⏵ auto mode on",input.join("\n"))}

    #[test]
    fn a_rewound_turn_is_its_prompt_back_in_the_input() {
        let prompt="first line of the prompt\n\nsecond line";
        assert_eq!(claude_input(&screen(&["❯\u{a0}first","  second"])),Some((3,vec!["first".to_owned(),"second".to_owned()])));
        // Wrapping and the space after ❯ do not matter.
        assert!(rewound(&screen(&["❯\u{a0}first line of the","  prompt","","  second line"]),prompt));
        for input in [&["❯\u{a0}"][..],&["❯\u{a0}first line of the prompt"],&["❯\u{a0}first line of the prompt","  second line and more"]] {
            assert!(!rewound(&screen(input),prompt));
        }
        // Clipped and automatic prompts never leave Terminal.
        for prompt in ["long…","[HGS automatic recovery] continue"," "] {
            assert!(!rewound(&screen(&[&format!("❯\u{a0}{prompt}")]),prompt));
        }
    }
}
