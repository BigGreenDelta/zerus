//! Explicit native /clear, never a prompt or an automatic cache reaction.
use super::*;
use serde::Deserialize;
use std::time::{Duration, Instant};

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct Request { request_id: String, expected_run_id: String, expected_conversation_id: String,
    #[serde(default)] dismiss_prompt: bool }

pub(super) fn observe_start(record: &mut Value, event: &Value) {
    let previous = string(record, "conversation_id").to_owned();
    let next = string(event, "session_id");
    if previous.is_empty() {
        // A fresh start's first conversation draws the same boundary as /clear.
        let earlier = string(&record["fresh_from"], "conversation_id").to_owned();
        if !earlier.is_empty() && earlier != next {
            record["session_clear"] = json!({"at":record["fresh_from"]["at"],"run_id":record["run_id"],
                "conversation_id":next,"previous_conversation_id":earlier,"source":"fresh_start"});
        }
        record.as_object_mut().unwrap().remove("fresh_from");
        return;
    }
    if previous == next { return; }
    let requested = record["clear_context_request"]["conversation_id"] == previous;
    if string(event, "source") == "clear" || requested {
        // The agent starts over, but the local Activity timeline continues:
        // earlier conversations stay readable above the boundary.
        let mut earlier = earlier_conversations(record).as_array().cloned().unwrap_or_default();
        earlier.retain(|id| id != next);
        earlier.push(json!(previous));
        if earlier.len() > CLEARED_CONVERSATIONS { earlier.drain(..earlier.len() - CLEARED_CONVERSATIONS); }
        record["cleared_conversations"] = json!(earlier);
        // A reset observed in Terminal already journaled its boundary.
        let journaled = awaiting_start(record);
        let at = if journaled { record["session_clear"]["at"].clone() } else { json!(now()) };
        record["session_clear"] = json!({"at":at,"run_id":record["run_id"],
            "conversation_id":next,"previous_conversation_id":previous,"source":"native_hook","journaled":journaled});
    } else {
        for key in ["session_clear", "cleared_conversations"] { record.as_object_mut().unwrap().remove(key); }
    }
}

const CLEARED_CONVERSATIONS: usize = 32;

/// `resume --fresh` replaces a stopped binding; its Activity continues like a
/// clear. The new conversation is unknown until the agent's SessionStart.
pub(super) fn carry_fresh_start(previous: &Value, record: &mut Value) {
    let mut earlier = earlier_conversations(previous).as_array().cloned().unwrap_or_default();
    let id = string(previous, "conversation_id");
    if !id.is_empty() {
        earlier.push(json!(id));
        record["fresh_from"] = json!({"conversation_id":id,"at":now()});
    }
    if earlier.len() > CLEARED_CONVERSATIONS { earlier.drain(..earlier.len() - CLEARED_CONVERSATIONS); }
    if !earlier.is_empty() { record["cleared_conversations"] = json!(earlier); }
    if let Some(anchor) = previous.get("journal_name") { record["journal_name"] = anchor.clone(); }
}

// Earlier conversations of this session's Activity, oldest first. Only
// confirmed clears add to it; another conversation starts a new timeline.
pub(super) fn earlier_conversations(record: &Value) -> Value {
    let current = string(record, "conversation_id");
    // A clear confirmed before this list existed still names its predecessor.
    let ids = record["cleared_conversations"].as_array().cloned().unwrap_or_else(|| {
        let clear = &record["session_clear"];
        if clear["conversation_id"] == current { vec![clear["previous_conversation_id"].clone()] } else { Vec::new() }
    });
    json!(ids.into_iter().filter(|id| id.as_str().is_some_and(|id| !id.is_empty() && id != current)).collect::<Vec<_>>())
}

pub(super) fn awaiting_start(record: &Value) -> bool {
    record["session_clear"]["awaiting_session_start"] == true
        && record["session_clear"]["run_id"] == record["run_id"]
        && record["session_clear"]["conversation_id"] == record["conversation_id"]
}

fn codex_reset_panel(screen: &str, cwd: &str) -> bool {
    if !Path::new(cwd).is_absolute() || !screen.lines().any(|line| line.trim_start().starts_with('›')) { return false; }
    let mut rows: Vec<_> = screen.lines().take_while(|line| !line.trim_start().starts_with('›'))
        .map(str::trim).filter(|line| !line.is_empty()).collect();
    // The released idle screen can include its settled decorative logo. Match
    // the complete known pose; arbitrary braille, prose and partial frames do
    // not confirm a reset. A replay remains unconfirmed until it settles.
    let logo: Vec<_> = include_str!("data/codex-empty-state-60x21.txt").lines()
        .filter(|line| !line.starts_with('#')).collect();
    if rows.ends_with(&logo) { rows.truncate(rows.len() - logo.len()); }
    if !(2..=6).contains(&rows.len()) || !rows[0].starts_with(">_ OpenAI Codex (v")
        || !rows[0].ends_with(')') { return false; }
    let short = Path::new(cwd).strip_prefix(home()).ok().map(|path| {
        if path.as_os_str().is_empty() { "~".to_owned() } else { format!("~/{}",path.display()) }
    });
    if rows[1] != cwd && short.as_deref() != Some(rows[1]) { return false; }
    let tail = if rows.get(2).is_some_and(|row| row.starts_with("permissions: ")) {
        &rows[3..]
    } else { &rows[2..] };
    // Resident Codex 0.160.1 keeps one randomly chosen empty-state greeting.
    // Match the released finite vocabulary, including terminal line wrapping;
    // arbitrary assistant output and unknown startup panels still fail closed.
    let greeting = tail.join(" ");
    tail.is_empty() || include_str!("data/codex-0.160.1-greetings.txt").lines()
        .filter(|line| !line.starts_with('#')).any(|line| line == greeting)
}

// Codex defers its clear SessionStart hook until the next user turn and does
// not materialize the new rollout yet. Observe only the complete reset panel
// in the exact live empty composer; keep the native ID until its real hook.
pub(super) fn observe_terminal(record: &mut Value) -> Result<bool> {
    if string(record,"agent") != "codex" || awaiting_start(record)
        || record["activity"] != "idle" || record["phase"] != "idle"
        || string(record,"conversation_id").is_empty()
        || (string(record,"prompt").is_empty() && string(record,"last_message").is_empty()
            && !record["clear_context_request"].is_object()) { return Ok(false); }
    // Ordinary idle polls only need one read-only capture. Perform the more
    // expensive process and composer checks only for a matching reset panel.
    if !codex_reset_panel(&kimi_tui_choice::capture(record)?,string(record,"cwd")) { return Ok(false); }
    kimi_tui_choice::identity(record)?;
    input::checked_terminal(record,true)?;
    if !codex_reset_panel(&kimi_tui_choice::capture(record)?,string(record,"cwd")) { return Ok(false); }
    kimi_tui_choice::identity(record)?;
    record["session_clear"] = json!({"at":now(),"run_id":record["run_id"],
        "conversation_id":record["conversation_id"],"previous_conversation_id":record["conversation_id"],
        "source":"native_terminal","awaiting_session_start":true});
    for key in ["prompt","last_message","last_error"] { record.as_object_mut().unwrap().remove(key); }
    if let Some(mut clear) = event(record) {
        clear["hook_event_name"] = json!("SessionCleared");
        journal::log_event(record,&clear)?;
    }
    write(record)?;
    Ok(true)
}

pub(super) fn event(record: &Value) -> Option<Value> {
    let clear = &record["session_clear"];
    if clear["run_id"] != record["run_id"] || clear["conversation_id"] != record["conversation_id"]
        || clear["at"].as_f64().is_none_or(|at| at <= 0.0) { return None; }
    Some(json!({"type":"SessionCleared","at":clear["at"],"run_id":record["run_id"],
        "agent_id":"","detail":"","activity_key":format!("clear:{}:{}:{}",
            string(record,"run_id"),string(record,"conversation_id"),clear["at"])}))
}

pub(super) fn available(record: &Value, live: bool) -> bool {
    live && AGENTS.contains(&string(record,"agent"))
        && !string(record,"conversation_id").is_empty()
        && record["activity"]=="idle" && record["phase"]=="idle"
        && !pause_active(record) && string(record,"error").is_empty()
        && string(record,"expected_id").is_empty()
        && !record["clear_context_request"].is_object()
        && !awaiting_start(record)
        && !compact_context::pending(record)
}
/// Claude waits in a chooser that hides its composer. Only an explicit request
/// dismisses it, with one Esc that answers it as in Terminal.
pub(super) fn dismissable(record: &Value, live: bool) -> bool {
    live && record["agent"]=="claude"
        && !string(record,"conversation_id").is_empty()
        && ["input","approval"].contains(&string(record,"phase"))
        && !pause_active(record) && string(record,"error").is_empty()
        && string(record,"expected_id").is_empty()
        && !record["clear_context_request"].is_object()
        && !awaiting_start(record)
        && !compact_context::pending(record)
}
const STARTUP_CHOICES: [&str; 3] = ["claude_resume", "claude_permission_mode", "claude_folder_trust"];
/// Esc can end a launch at a startup chooser; Activity answers those instead.
pub(super) fn startup_choice(cards: &Value) -> bool {
    cards.as_array().into_iter().flatten().any(|card| STARTUP_CHOICES.contains(&string(card,"source")))
}
fn checked_chooser(name: &str, request: &Request) -> Result<Value> {
    let record=read(name)?;
    if string(&record,"run_id")!=request.expected_run_id || string(&record,"conversation_id")!=request.expected_conversation_id {
        return Err("session identity changed; refresh before clearing".into());
    }
    if !matches(&record,live()?.get(name)) || !process_alive(&record) {
        return Err("the exact agent process is no longer running; resume it explicitly".into());
    }
    if !dismissable(&record,true) {return Err("Claude is no longer waiting in a chooser; refresh before clearing".into());}
    if claude_resume::current(&record).is_some() || claude_permission::current(&record).is_some() || claude_trust::current(&record).is_some() {
        return Err("Answer Claude's startup choice in Activity or Terminal before clearing".into());
    }
    input::checked_terminal(&record,false)?;
    if !claude_approval::chooser_visible(&kimi_tui_choice::capture(&record)?) {
        return Err("Claude's chooser is no longer visible; refresh before clearing".into());
    }
    Ok(record)
}
fn command_row(agent: &str, row: &str, x: usize, command: &str) -> bool {
    let before=row.chars().take(x).collect::<String>();
    let after=row.chars().skip(x).collect::<String>();
    match agent {
        "codex" => before.trim()==format!("› {command}") && after.trim().is_empty(),
        "claude" => [format!("❯ {command}"),format!("> {command}")].iter().any(|prompt|prompt==before.replace('\u{a0}'," ").trim()) && after.trim().is_empty(),
        // Kimi 2.1 shows an argument hint such as <instruction> after the command.
        "kimi" => before.trim()==format!("│ > {command}") && after.trim().strip_suffix('│').is_some_and(|hint|{
            let hint=hint.trim();
            hint.is_empty() || (hint.len()<=40 && hint.starts_with('<') && hint.ends_with('>')
                && !hint[1..hint.len()-1].contains(['<','>']))
        }),
        _ => false,
    }
}
/// Kimi 2.1 turns an Enter that follows typed text within about 0.1 s into a
/// newline. Wait this long after a typed command, then recheck it and press Enter.
pub(super) const COMMAND_SETTLE: Duration = Duration::from_millis(250);
pub(super) fn ready(record: &Value, command: &str) -> Result<bool> {
    let cursor=backend::current().pane_info(backend::Target::Pane(string(record,"pane")),
        &[backend::PaneField::CursorX,backend::PaneField::CursorY])?;
    let Some((x,y))=cursor.pair() else {return Ok(false)};
    let (Ok(x),Ok(y))=(x.parse::<usize>(),y.parse::<usize>()) else {return Ok(false)};
    let screen=kimi_tui_choice::capture(record)?;
    Ok(screen.lines().nth(y).is_some_and(|row|command_row(string(record,"agent"),row,x,command)))
}
pub(super) fn dispatch(args: &[String]) -> Result<i32> {
    if args.len()!=2 || args[1]!="--json" {return Err("usage: hgs clear-context <session> --json".into());}
    let mut bytes=Vec::new();io::stdin().take(8193).read_to_end(&mut bytes).map_err(|e|e.to_string())?;
    if bytes.len()>8192 {return Err("Clear request is too large".into());}
    let request:Request=serde_json::from_slice(&bytes).map_err(|e|e.to_string())?;
    let uuid=uuid::Uuid::parse_str(&request.request_id).map_err(|_|"Invalid request identity")?;
    let name=&args[0];
    if dsh::exists(name) {return Err("This DeepSeek harness does not expose a native context reset. Use its native UI.".into());}
    let mut guard=Some(lock(None)?);
    let dismiss=request.dismiss_prompt && !available(&read(name)?,true);
    let mut record=if dismiss {checked_chooser(name,&request)?} else {
        let record=input::checked_ready(name,&request.expected_run_id,&request.expected_conversation_id)?;
        if !available(&record,true) {return Err("Wait for the agent to become idle before clearing context".into());}
        input::checked_terminal(&record,true)?;
        record
    };
    if record["pending_settings"].is_object() {return Err("Wait for the pending model settings before clearing context".into());}
    let path=absolute_root()?.join("clear_receipts").join(format!("{uuid}.json"));
    if path.exists() {return Err("This clear was already attempted. Check Terminal before trying again".into());}
    recovery::cancel_for_input(name)?;
    record["clear_context_request"]=json!({"request_id":request.request_id,"conversation_id":request.expected_conversation_id,"at":now()});
    write(&mut record)?;
    atomic(&path,&json!({"status":"in_progress","run_id":request.expected_run_id,"conversation_id":request.expected_conversation_id}).to_string())?;
    // No Ctrl-U: never discard a native draft. Escape only on explicit request.
    let result=(|| {
        if dismiss {
            // One Esc answers the chooser as in Terminal: an approval is denied,
            // a question stays unanswered. It is never repeated.
            backend::current().send(string(&record,"pane"),&[backend::Key::Named("Escape")])?;
            guard=None; // Hooks reporting the dismissal must be able to commit.
            let deadline=Instant::now()+Duration::from_secs(3);
            loop {
                let current=kimi_tui_choice::identity(&record)?;
                if !claude_approval::chooser_visible(&kimi_tui_choice::capture(&current)?)
                    && input::checked_terminal(&current,true).is_ok() {break;}
                if Instant::now()>=deadline {return Err("Claude's chooser did not close after Esc".into());}
                std::thread::sleep(Duration::from_millis(40));
            }
            guard=Some(lock(None)?);
            input::checked_terminal(&kimi_tui_choice::identity(&record)?,true)?;
        }
        backend::current().send(string(&record,"pane"),&[backend::Key::Literal("/clear")])?;
        let deadline=Instant::now()+Duration::from_secs(3);
        loop {
            let current=kimi_tui_choice::identity(&record)?;
            if ready(&current,"/clear")? {break;}
            if Instant::now()>=deadline {return Err("Native /clear input could not be verified".into());}
            std::thread::sleep(Duration::from_millis(40));
        }
        std::thread::sleep(COMMAND_SETTLE);
        let current=if dismiss {kimi_tui_choice::identity(&record)?}
            else {input::checked_ready(name,&request.expected_run_id,&request.expected_conversation_id)?};
        kimi_tui_choice::identity(&record)?;
        input::checked_terminal(&current,false)?;
        if !ready(&current,"/clear")? {return Err("The native input changed before /clear".into());}
        backend::current().send(string(&current,"pane"),&[backend::Key::Named("Enter")])?;
        Ok(())
    })();
    drop(guard); // SessionStart hooks must be able to commit the new identity.
    result.map_err(|e:String|format!("Clear delivery uncertain: {e}. Check Terminal before retrying"))?;
    let deadline=Instant::now()+Duration::from_secs(4);
    let mut status="submitted";
    loop {
        let mut current=read(name)?;
        if current["run_id"]!=record["run_id"] || current["pid"]!=record["pid"] {break;}
        if !string(&current,"conversation_id").is_empty() && current["conversation_id"]!=record["conversation_id"] {
            status="confirmed";break;
        }
        {
            let _guard=lock(None)?;
            current=read(name)?;
            if current["run_id"]==record["run_id"] && current["pid"]==record["pid"]
                && (observe_terminal(&mut current).unwrap_or(false) || awaiting_start(&current)) {
                status="confirmed";break;
            }
        }
        if Instant::now()>=deadline {break;}
        std::thread::sleep(Duration::from_millis(80));
    }
    let receipt=json!({"request_id":request.request_id,"name":name,"run_id":request.expected_run_id,
        "conversation_id":request.expected_conversation_id,"status":status});
    atomic(&path,&receipt.to_string())?;println!("{receipt}");Ok(0)
}
#[cfg(test)] mod tests {
    use super::*;
    #[test] fn clear_hooks_require_a_changed_conversation_and_keep_the_observed_time() {
        let original=json!({"run_id":"run","conversation_id":"old"});
        let mut record=original.clone();
        observe_start(&mut record,&json!({"session_id":"old","source":"clear"}));
        assert!(record["session_clear"].is_null());
        observe_start(&mut record,&json!({"session_id":"new","source":"resume"}));
        assert!(record["session_clear"].is_null());
        record["clear_context_request"]=json!({"conversation_id":"old"});
        observe_start(&mut record,&json!({"session_id":"new"}));
        record["conversation_id"]=json!("new");
        assert!(event(&record).is_some());
        record["run_id"]=json!("other");assert!(event(&record).is_none());
        record=original;
        record["session_clear"]=json!({"at":123.0,"run_id":"run","conversation_id":"old","awaiting_session_start":true});
        observe_start(&mut record,&json!({"session_id":"new","source":"clear"}));
        record["conversation_id"]=json!("new");
        assert_eq!(event(&record).unwrap()["at"],123.0);
        assert!(!awaiting_start(&record));
    }
    #[test] fn fresh_starts_continue_the_activity_timeline_like_a_clear() {
        let previous=json!({"run_id":"old-run","conversation_id":"second","cleared_conversations":["first"],"journal_name":"anchor"});
        let mut record=json!({"run_id":"run","conversation_id":null});
        carry_fresh_start(&previous,&mut record);
        assert_eq!(earlier_conversations(&record),json!(["first","second"]));
        assert_eq!(record["journal_name"],"anchor");
        observe_start(&mut record,&json!({"session_id":"third","source":"startup"}));
        record["conversation_id"]=json!("third");
        assert!(record["fresh_from"].is_null());
        assert_eq!(record["session_clear"]["previous_conversation_id"],"second");
        assert!(event(&record).is_some());
        assert_eq!(earlier_conversations(&record),json!(["first","second"]));
        // An unconfirmed binding has no conversation of its own to carry.
        let mut unconfirmed=json!({"run_id":"run"});
        carry_fresh_start(&json!({"run_id":"old","conversation_id":null}),&mut unconfirmed);
        assert!(unconfirmed["fresh_from"].is_null());assert!(unconfirmed["cleared_conversations"].is_null());
    }
    #[test] fn confirmed_clears_keep_the_earlier_activity_timeline() {
        let mut record=json!({"run_id":"run","conversation_id":"first"});
        for (next,previous) in [("second","first"),("third","second")] {
            observe_start(&mut record,&json!({"session_id":next,"source":"clear"}));
            record["conversation_id"]=json!(next);
            assert_eq!(record["session_clear"]["previous_conversation_id"],previous);
        }
        assert_eq!(earlier_conversations(&record),json!(["first","second"]));
        assert_eq!(record["session_clear"]["journaled"],false);
        // A clear first confirmed in Terminal was already journaled.
        record["session_clear"]=json!({"at":7.0,"run_id":"run","conversation_id":"third","awaiting_session_start":true});
        observe_start(&mut record,&json!({"session_id":"fourth","source":"clear"}));
        assert_eq!(record["session_clear"]["journaled"],true);
        record["conversation_id"]=json!("fourth");
        assert_eq!(earlier_conversations(&record),json!(["first","second","third"]));
        for index in 0..40 {
            let next=format!("later-{index}");
            observe_start(&mut record,&json!({"session_id":next,"source":"clear"}));
            record["conversation_id"]=json!(next);
        }
        let earlier=earlier_conversations(&record);
        assert_eq!(earlier.as_array().unwrap().len(),CLEARED_CONVERSATIONS);
        assert_eq!(earlier[CLEARED_CONVERSATIONS-1],"later-38");
        // Clears confirmed by an earlier version continue from their predecessor.
        let mut legacy=json!({"run_id":"run","conversation_id":"second",
            "session_clear":{"conversation_id":"second","previous_conversation_id":"first"}});
        assert_eq!(earlier_conversations(&legacy),json!(["first"]));
        observe_start(&mut legacy,&json!({"session_id":"third","source":"clear"}));
        legacy["conversation_id"]=json!("third");
        assert_eq!(earlier_conversations(&legacy),json!(["first","second"]));
        legacy["session_clear"]=json!({"conversation_id":"third","previous_conversation_id":"third","awaiting_session_start":true});
        legacy.as_object_mut().unwrap().remove("cleared_conversations");
        assert_eq!(earlier_conversations(&legacy),json!([]));
        // Another conversation is not a continuation of this timeline.
        observe_start(&mut record,&json!({"session_id":"resumed","source":"resume"}));
        assert!(record["cleared_conversations"].is_null() && record["session_clear"].is_null());
        assert_eq!(earlier_conversations(&record),json!([]));
    }
    // Codex prints a POSIX working directory; Windows forms come with Phase 2 M4.
    #[cfg(unix)]
    #[test] fn only_the_complete_native_empty_reset_header_matches() {
        let panel="\n >_ OpenAI Codex (v0.162.0)\n /fixture\n permissions: YOLO mode\n\n\n› Ask Codex to do anything\n GPT-6.1-Sol default\n ? for shortcuts\n";
        assert!(codex_reset_panel(panel,"/fixture"));
        for screen in [panel.replace("/fixture","/other"),panel.replace("/fixture","/fix…"),
            panel.replace("permissions: YOLO mode","To get started, describe a task"),
            panel.replace("\n\n›","\nPrevious agent response\n›"),
            panel.replace(">_ OpenAI Codex (v0.162.0)","Quoted reset header"),
            panel.replace("\n >_","\nSome user message\n >_"),">_ OpenAI Codex (v0.162.0)\n/fixture\n".to_owned()] {
            assert!(!codex_reset_panel(&screen,"/fixture"),"{screen}");
        }
    }
    // Codex prints a POSIX working directory; Windows forms come with Phase 2 M4.
    #[cfg(unix)]
    #[test] fn resident_native_greetings_match_without_accepting_other_prose() {
        let header = ">_ OpenAI Codex (v0.160.1)\n/fixture\npermissions: YOLO mode\n\n";
        let footer = "\n\n› Ask Codex to do anything\nGPT-6.1-Sol default\n? for shortcuts\n";
        for greeting in include_str!("data/codex-0.160.1-greetings.txt").lines().filter(|line| !line.starts_with('#')) {
            assert!(codex_reset_panel(&format!("{header}{greeting}{footer}"), "/fixture"), "{greeting}");
            if let Some((first, last)) = greeting.rsplit_once(' ') {
                assert!(codex_reset_panel(&format!("{header}{first}\n{last}{footer}"), "/fixture"));
            }
        }
        for prose in ["Previous agent response", "Resuming session…", "Clear failed", "Unknown greeting",
            "What are we cooking up?\nPrevious agent response", "What are we cooking up?\nWhat are we cooking up?"] {
            assert!(!codex_reset_panel(&format!("{header}{prose}{footer}"), "/fixture"), "{prose}");
        }
    }
    // Codex prints a POSIX working directory; Windows forms come with Phase 2 M4.
    #[cfg(unix)]
    #[test] fn only_the_complete_released_idle_logo_is_decoration() {
        let header = ">_ OpenAI Codex (v0.162.0)\n/fixture\npermissions: YOLO mode\n\n";
        let logo = include_str!("data/codex-empty-state-60x21.txt").lines()
            .filter(|line| !line.starts_with('#')).collect::<Vec<_>>().join("\n");
        let footer = "\n\n› Ask Codex to do anything\nGPT-6.1-Sol default\n? for shortcuts\n";
        assert!(codex_reset_panel(&format!("{header}{logo}{footer}"), "/fixture"));
        assert!(codex_reset_panel(&format!("{header}What are we cooking up?\n{logo}{footer}"), "/fixture"));
        for body in [logo.replacen('⣀', "⣁", 1), logo.lines().skip(1).collect::<Vec<_>>().join("\n"),
            format!("{logo}\nPrevious answer"), format!("Previous answer\n{logo}"),
            format!("{logo}\n{logo}"), "⣿⣿⣿\n⣿⣿⣿".to_owned()] {
            assert!(!codex_reset_panel(&format!("{header}{body}{footer}"), "/fixture"), "{body}");
        }
        assert!(!codex_reset_panel(&format!("{header}{logo}{footer}"), "/other"));
    }
    #[test] fn only_a_waiting_claude_chooser_can_be_dismissed_for_a_clear() {
        let waiting=json!({"agent":"claude","conversation_id":"conversation","activity":"busy","phase":"input"});
        assert!(dismissable(&waiting,true));assert!(!dismissable(&waiting,false));
        let mut approval=waiting.clone();approval["phase"]=json!("approval");assert!(dismissable(&approval,true));
        for (key,value) in [("agent",json!("codex")),("phase",json!("idle")),("phase",json!("working")),
            ("conversation_id",json!("")),("error",json!("failed")),("expected_id",json!("next")),
            ("clear_context_request",json!({"request_id":"pending"}))] {
            let mut other=waiting.clone();other[key]=value;assert!(!dismissable(&other,true),"{key}");
        }
        // Esc can end a launch at a startup chooser; only Activity answers those.
        assert!(startup_choice(&json!([{"source":"claude_resume"}])));
        assert!(startup_choice(&json!([{"source":"hook"},{"source":"claude_folder_trust"}])));
        assert!(!startup_choice(&json!([{"source":"claude_terminal_choice"},{"source":"claude_tool_approval"}])));
    }
    #[test] fn dismissal_is_an_explicit_optional_request_field() {
        let base=json!({"request_id":"r","expected_run_id":"run","expected_conversation_id":"c"});
        assert!(!serde_json::from_value::<Request>(base.clone()).unwrap().dismiss_prompt);
        let mut forced=base.clone();forced["dismiss_prompt"]=json!(true);
        assert!(serde_json::from_value::<Request>(forced).unwrap().dismiss_prompt);
        let mut unknown=base;unknown["force"]=json!(true);assert!(serde_json::from_value::<Request>(unknown).is_err());
    }
    #[test] fn exact_command_only() {
        assert!(command_row("codex","› /clear",8,"/clear"));
        assert!(command_row("claude","❯\u{a0}/clear",8,"/clear"));
        assert!(command_row("kimi"," │ > /clear       │",11,"/clear"));
        assert!(command_row("kimi"," │ > /compact  <instruction>          │",13,"/compact"));
        for row in [" │ > /compact extra  │"," │ > /compact  <a> <b> │"," │ > /compact  <instruction"] {assert!(!command_row("kimi",row,13,"/compact"));}
        for row in ["› /clear extra","› draft /clear","shell$ /clear"] {assert!(!command_row("codex",row,8,"/clear"));}
    }
}
