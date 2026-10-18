//! Claude's PermissionRequest answer channel for main-agent tool approvals.
//! Claude names no tool use in that request, so hgs records each active
//! main-agent PreToolUse and binds a request only to the one active tool use
//! with the same tool and input. The waiting hook hands Claude one decision for
//! exactly that tool use: allow it once, deny it and end the turn as Terminal's
//! No does, or deny it with the person's message. A Terminal answer sends the
//! hook no signal, so only the screen proves that the panel still waits.
//! Children stay on the screen path: Claude 2.1.296 runs them in the
//! background and draws their requests outside the main screen.
use super::*;

const USES:&str="claude_tool_uses";
const OVERFLOW:&str="claude_tool_use_overflow";
const LIMIT:usize=64;
/// Claude's message for a denial that ends the turn, as Terminal's No does.
const DENIED:&str="The user denied this tool use.";
pub(super) const LOCKED:&str="A decision was already handed to Claude's permission hook and is not confirmed yet. Check Terminal before answering again.";

fn digest(input:&Value)->String {format!("{:x}",Sha256::digest(input.to_string().as_bytes()))}

/// Record the main agent's tool uses from PreToolUse until they end. A turn
/// boundary or a new conversation forgets them all; more than LIMIT at once
/// makes every binding ambiguous until then.
pub(super) fn observe(record:&mut Value,event:&Value) {
    if record["agent"]!="claude"||!string(event,"agent_id").is_empty() {return;}
    let id=string(event,"tool_use_id");
    let mut uses=record[USES].as_object().cloned().unwrap_or_default();
    match string(event,"hook_event_name") {
        "PreToolUse" if !id.is_empty()&&!question_tool(string(event,"tool_name")) => {
            if uses.len()>=LIMIT&&!uses.contains_key(id) {record[OVERFLOW]=json!(true);}
            else {uses.insert(id.into(),json!({"tool_name":event["tool_name"],"input":digest(&event["tool_input"]),"at":now()}));}
        }
        "PostToolUse"|"PostToolUseFailure" => {uses.remove(id);}
        "Stop"|"StopFailure"|"Interrupt"|"UserPromptSubmit"|"SessionStart"|"SessionEnd" => {
            uses.clear();record.as_object_mut().unwrap().remove(OVERFLOW);
        }
        _ => {}
    }
    if uses.is_empty() {record.as_object_mut().unwrap().remove(USES);} else {record[USES]=json!(uses);}
}

/// Forget the tool use a denial settled: Claude reports no end for it.
pub(super) fn forget(record:&mut Value,tool_use:&str) {
    if let Some(uses)=record[USES].as_object_mut() {uses.remove(tool_use);}
    if record[USES].as_object().is_some_and(|uses|uses.is_empty()) {record.as_object_mut().unwrap().remove(USES);}
}

/// The one active tool use, begun before the request, that the pending
/// main-agent request was made for. None when it is missing or ambiguous.
fn bound_use(record:&Value)->Option<String> {
    let request=&record["pending_approval"];
    let at=record["pending_approval_at"].as_f64()?;
    if record[OVERFLOW]==true {return None;}
    let input=digest(&request["tool_input"]);
    let mut found=record[USES].as_object()?.iter().filter(|(_,used)|used["tool_name"]==request["tool_name"]
        &&used["input"]==input.as_str()&&used["at"].as_f64().is_some_and(|started|started<=at)).map(|(id,_)|id);
    let id=found.next()?.clone();
    found.next().is_none().then_some(id)
}

/// The channel identity of the pending main-agent request: its agent process,
/// conversation, tool use and exact input.
pub(super) fn identity(record:&Value)->Option<Value> {identity_for(record,&bound_use(record)?)}
fn identity_for(record:&Value,tool_use:&str)->Option<Value> {
    let request=request(record)?;
    let tool=string(request,"tool_name");
    if record["agent"]!="claude"||string(record,"run_id").is_empty()||string(record,"conversation_id").is_empty()
        ||!string(request,"agent_id").is_empty()||TERMINAL_ONLY_TOOLS.contains(&tool)
        ||record[USES][tool_use]["input"]!=digest(&request["tool_input"]).as_str() {return None;}
    let key=format!("{:x}",Sha256::digest(json!([record["run_id"],record["conversation_id"],record["pane"],record["pid"],
        record["process_start"],tool_use,tool,request["tool_input"],record["pending_approval_at"]]).to_string()));
    Some(json!({"question_id":format!("claude-approval:{key}"),"question_hash":key,"tool_call_id":tool_use}))
}

/// Claude's PermissionRequest for a main-agent tool approval that hgs can answer.
pub(in crate::state) fn requested(event:&Value)->bool {
    let tool=string(event,"tool_name");
    string(event,"hook_event_name")=="PermissionRequest"&&string(event,"agent_id").is_empty()
        &&!tool.is_empty()&&!question_tool(tool)&&!TERMINAL_ONLY_TOOLS.contains(&tool)
}

/// How recently the journal must have recorded the request a new hook binds
/// to: both hooks start together, and an older request with the same input is
/// not this one.
const FRESH:f64=10.;

/// The identity a waiting hook serves for Claude's PermissionRequest `event`:
/// only from this conversation, for the request hgs just recorded, with one
/// active tool use of the same tool and input.
pub(in crate::state) fn bind(record:&Value,event:&Value)->Option<Value> {
    let conversation=string(record,"conversation_id");
    let request=&record["pending_approval"];
    if !requested(event)||conversation.is_empty()||string(event,"session_id")!=conversation
        ||request["tool_name"]!=event["tool_name"]||request["tool_input"]!=event["tool_input"]
        ||record["pending_approval_at"].as_f64().is_none_or(|at|now()-at>FRESH) {return None;}
    identity(record)
}

/// The one active main-agent tool call with the event's tool and input in
/// `record` that began before the hook did (`started`). Claude journals
/// PreToolUse before it asks, so in the first record a new hook reads, this is
/// the call the event asks about; one that began later is another request.
/// None when there is none or more than one.
pub(in crate::state) fn asked_use(record:&Value,event:&Value,started:f64)->Option<String> {
    if !requested(event)||record[OVERFLOW]==true {return None;}
    let input=digest(&event["tool_input"]);
    let mut found=record[USES].as_object()?.iter()
        .filter(|(_,used)|used["tool_name"]==event["tool_name"]&&used["input"]==input.as_str()
            &&used["at"].as_f64().is_some_and(|at|at<=started)).map(|(id,_)|id);
    let id=found.next()?.clone();
    found.next().is_none().then_some(id)
}

/// `bind`, only to the tool call `tool_use` that the hook captured first: a
/// later call with the same input is another request.
pub(in crate::state) fn bind_to(record:&Value,event:&Value,tool_use:&str)->Option<Value> {
    bind(record,event).filter(|card|card["tool_call_id"]==tool_use)
}

/// Whether `current` still records the exact request that `card` was bound to.
/// Another request with the same input may begin later; it is not this one.
pub(in crate::state) fn pending(current:&Value,card:&Value)->bool {
    identity_for(current,string(card,"tool_call_id")).as_ref()==Some(card)
}

/// Whether the screen still shows Claude waiting for this request: a chooser,
/// and a complete panel only when it is this request and no recorded child
/// request. A clipped panel may be a child's, so it proves nothing while one
/// is recorded. The decision itself goes to the bound tool use alone.
pub(super) fn shown(record:&Value,screen:&str)->bool {
    let Some(request)=request(record) else {return false};
    if !visible(screen)||children::overflow(record) {return false;}
    match parse(screen) {
        Some(panel) => matches(request,&panel)&&!children::entries(record).any(|(_,entry)|matches(&entry["request"],&panel)),
        None => children::entries(record).next().is_none(),
    }
}

/// The same proof for the checked `record`, read from the terminal now
/// through the same identity and capture as `hgs answer`.
pub(in crate::state) fn waits_now(record:&Value)->bool {
    waits(record,kimi_tui_choice::identity(record),kimi_tui_choice::capture)
}
/// A refused identity (another process or pane, a pause, a provider error, a
/// terminal the agent does not own) never lets a decision go. Only a capture
/// that fails after the identity holds proves nothing either way: it never
/// overrides the proof that let `hgs answer` offer, since a retracted offer
/// leaves the request to the screen for good, and Claude ignores a decision
/// that arrives after a Terminal answer.
pub(in crate::state) fn waits(record:&Value,live:Result<Value>,capture:impl FnOnce(&Value)->Result<String>)->bool {
    match live {
        Ok(live) => capture(&live).map_or(true,|screen|shown(record,&screen)),
        Err(_) => false,
    }
}

/// Claude's PermissionRequest decision for an offered approval. Allow carries
/// no updatedPermissions, so it grants this one tool use only.
pub(in crate::state) fn decision(offer:&Value)->Result<Value> {
    let decision=match (string(offer,"decision"),string(offer,"message")) {
        ("allow","") => json!({"behavior":"allow"}),
        ("deny","") => json!({"behavior":"deny","message":DENIED,"interrupt":true}),
        ("feedback",message) if !message.trim().is_empty()&&!message.chars().any(char::is_control) =>
            json!({"behavior":"deny","message":message}),
        _ => return Err("the offered approval is invalid".into()),
    };
    Ok(json!({"hookSpecificOutput":{"hookEventName":"PermissionRequest","decision":decision}}))
}

#[cfg(test)] mod tests {
    use super::*;
    const COMMAND:&str="printf hi > out.txt";
    fn event(kind:&str,extra:Value)->Value {
        let mut event=json!({"hook_event_name":kind,"tool_name":"Bash","tool_input":{"command":COMMAND},"session_id":"conversation"});
        for (key,value) in extra.as_object().unwrap() {event[key]=value.clone();}
        event
    }
    fn waiting()->Value {
        let mut r=json!({"agent":"claude","phase":"working","run_id":"run","conversation_id":"conversation","pane":"%1","pid":1,"process_start":"start"});
        observe(&mut r,&event("PreToolUse",json!({"tool_use_id":"toolu_one"})));
        let request=event("PermissionRequest",json!({}));
        questions::observe(&mut r,&request);r["phase"]=json!("approval");
        r
    }

    #[test] fn binds_only_the_one_active_tool_use_of_this_conversation() {
        let r=waiting();
        let card=bind(&r,&event("PermissionRequest",json!({}))).unwrap();
        assert_eq!(card["tool_call_id"],"toolu_one");
        assert_eq!(card,identity(&r).unwrap());assert!(pending(&r,&card));
        // Another conversation, a child, a question or plan, another input.
        for other in [event("PermissionRequest",json!({"session_id":"other"})),event("PermissionRequest",json!({"session_id":""})),
            event("PermissionRequest",json!({"agent_id":"child"})),event("PermissionRequest",json!({"tool_name":"AskUserQuestion"})),
            event("PermissionRequest",json!({"tool_name":"ExitPlanMode"})),event("PermissionRequest",json!({"tool_input":{"command":"rm"}})),
            event("PreToolUse",json!({}))] {
            assert!(bind(&r,&other).is_none(),"{other}");
        }
        let mut unsaved=r.clone();unsaved["conversation_id"]=json!("");
        assert!(bind(&unsaved,&event("PermissionRequest",json!({"session_id":""}))).is_none());
    }

    #[test] fn a_hook_binds_only_the_call_its_first_record_shows() {
        let request=event("PermissionRequest",json!({}));
        // PreToolUse is journaled; the PermissionRequest is not yet.
        let mut lag=json!({"agent":"claude","phase":"tool","run_id":"run","conversation_id":"conversation","pane":"%1","pid":1,"process_start":"start"});
        observe(&mut lag,&event("PreToolUse",json!({"tool_use_id":"toolu_one"})));
        assert_eq!(asked_use(&lag,&request,now()).as_deref(),Some("toolu_one"));
        assert!(asked_use(&lag,&request,lag[USES]["toolu_one"]["at"].as_f64().unwrap()-1.).is_none(),"a call that began after the hook");
        assert!(bind_to(&lag,&request,"toolu_one").is_none(),"nothing to bind before the journal");
        let journaled=waiting();
        assert_eq!(bind_to(&journaled,&request,"toolu_one"),identity(&journaled),"the same call, once journaled");
        // That call ended and a later identical one is pending now.
        let mut later=journaled.clone();
        observe(&mut later,&event("PostToolUse",json!({"tool_use_id":"toolu_one"})));
        observe(&mut later,&event("PreToolUse",json!({"tool_use_id":"toolu_two"})));
        later[USES]["toolu_two"]["at"]=json!(later["pending_approval_at"].as_f64().unwrap()-1.);
        assert_eq!(bind(&later,&request).unwrap()["tool_call_id"],"toolu_two");
        assert!(bind_to(&later,&request,"toolu_one").is_none(),"never the later call");
        // No call, two identical calls, an overflow, a child or another input.
        let mut none=lag.clone();observe(&mut none,&event("PostToolUse",json!({"tool_use_id":"toolu_one"})));
        assert!(asked_use(&none,&request,now()).is_none());
        let mut twice=lag.clone();observe(&mut twice,&event("PreToolUse",json!({"tool_use_id":"toolu_two"})));
        assert!(asked_use(&twice,&request,now()).is_none());
        let mut overflow=lag.clone();overflow[OVERFLOW]=json!(true);
        assert!(asked_use(&overflow,&request,now()).is_none());
        assert!(asked_use(&lag,&event("PermissionRequest",json!({"agent_id":"child"})),now()).is_none());
        assert!(asked_use(&lag,&event("PermissionRequest",json!({"tool_input":{"command":"rm"}})),now()).is_none());
    }

    #[test] fn missing_stale_ambiguous_or_overflowing_tool_uses_never_bind() {
        let r=waiting();let request=event("PermissionRequest",json!({}));
        let mut missing=r.clone();observe(&mut missing,&event("PostToolUse",json!({"tool_use_id":"toolu_one"})));
        assert!(bind(&missing,&request).is_none());
        let mut ended=r.clone();observe(&mut ended,&json!({"hook_event_name":"Stop"}));
        assert!(bind(&ended,&request).is_none());
        // A tool use that began after the request is not the one it asks for.
        let mut late=r.clone();late[USES]["toolu_one"]["at"]=json!(late["pending_approval_at"].as_f64().unwrap()+1.);
        assert!(bind(&late,&request).is_none());
        // Two active uses with the same input are ambiguous.
        let mut twice=r.clone();observe(&mut twice,&event("PreToolUse",json!({"tool_use_id":"toolu_two"})));
        twice[USES]["toolu_two"]["at"]=twice[USES]["toolu_one"]["at"].clone();
        assert!(bind(&twice,&request).is_none());
        // A child's use of the same input is not recorded for the main agent.
        let mut child=r.clone();observe(&mut child,&event("PreToolUse",json!({"tool_use_id":"toolu_child","agent_id":"child"})));
        assert!(bind(&child,&request).is_some());
        let mut full=json!({"agent":"claude"});
        for index in 0..LIMIT {observe(&mut full,&event("PreToolUse",json!({"tool_use_id":format!("toolu_{index}"),"tool_input":{"command":index.to_string()}})));}
        assert!(full.get(OVERFLOW).is_none());
        observe(&mut full,&event("PreToolUse",json!({"tool_use_id":"toolu_extra"})));
        assert_eq!(full[OVERFLOW],true);assert!(full[USES].get("toolu_extra").is_none());
        let mut overflow=r.clone();overflow[OVERFLOW]=json!(true);
        assert!(bind(&overflow,&request).is_none());
        observe(&mut overflow,&json!({"hook_event_name":"UserPromptSubmit"}));
        assert!(overflow.get(OVERFLOW).is_none()&&overflow.get(USES).is_none());
    }

    #[test] fn a_bound_request_stays_pending_only_with_its_exact_identity() {
        let r=waiting();let card=identity(&r).unwrap();
        // A later identical tool use does not close the bound request.
        let mut duplicate=r.clone();observe(&mut duplicate,&event("PreToolUse",json!({"tool_use_id":"toolu_two"})));
        assert_eq!(identity(&duplicate),Some(card.clone()));
        assert!(pending(&duplicate,&card));
        let mut finished=r.clone();observe(&mut finished,&event("PostToolUse",json!({"tool_use_id":"toolu_one"})));
        assert!(!pending(&finished,&card));
        let mut replaced=r.clone();questions::observe(&mut replaced,&event("PermissionRequest",json!({"tool_input":{"command":"rm"}})));
        assert!(!pending(&replaced,&card));
        let mut again=r.clone();again["pending_approval_at"]=json!(again["pending_approval_at"].as_f64().unwrap()+1.);
        assert!(!pending(&again,&card),"a new request for the same tool use is another identity");
        for (key,value) in [("run_id",json!("other")),("conversation_id",json!("other")),("pid",json!(2)),("process_start",json!("other"))] {
            let mut changed=r.clone();changed[key]=value;assert!(!pending(&changed,&card),"{key}");
        }
        let mut forgotten=r.clone();forget(&mut forgotten,"toolu_one");
        assert!(!pending(&forgotten,&card));assert!(forgotten.get(USES).is_none());
    }

    #[test] fn only_a_freshly_recorded_request_binds() {
        let mut stale=waiting();
        stale["pending_approval_at"]=json!(now()-FRESH-1.);
        stale[USES]["toolu_one"]["at"]=json!(now()-FRESH-2.);
        assert!(identity(&stale).is_some(),"its identity stays for an answer already bound");
        assert!(bind(&stale,&event("PermissionRequest",json!({}))).is_none());
    }

    #[test] fn a_child_request_or_its_panel_proves_nothing_for_the_main_agent() {
        const PANEL:&str="────────────────────────────────────────\n Bash command\n╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌\n printf hi > out.txt\n╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌\n Do you want to proceed?\n ❯ 1. Yes\n   2. No\n\n Esc to cancel · Tab to amend\n";
        let clipped=PANEL.split_once(" Bash command\n").unwrap().1;
        let r=waiting();
        assert!(shown(&r,PANEL)&&shown(&r,clipped));
        assert!(!shown(&r,"────────────────────────────────────────\n❯ \n"));
        assert!(!shown(&r,&PANEL.replace("printf hi","printf ho")),"another request's complete panel");
        let mut child=r.clone();
        questions::observe(&mut child,&event("PermissionRequest",json!({"agent_id":"child"})));
        assert!(!shown(&child,PANEL),"the same command from a child is ambiguous");
        assert!(!shown(&child,clipped),"a clipped panel may be the child's");
        let mut other=r.clone();
        questions::observe(&mut other,&event("PermissionRequest",json!({"agent_id":"child","tool_input":{"command":"ls"}})));
        assert!(shown(&other,PANEL),"a complete panel of this request only");
        assert!(!shown(&other,clipped));
        let mut overflow=r.clone();overflow["child_approval_overflow"]=json!(true);
        assert!(!shown(&overflow,PANEL));
    }

    #[test] fn a_refused_identity_never_waits_but_an_unknown_capture_does() {
        const PANEL:&str="────────────────────────────────────────\n Bash command\n╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌\n printf hi > out.txt\n╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌\n Do you want to proceed?\n ❯ 1. Yes\n   2. No\n\n Esc to cancel · Tab to amend\n";
        let r=waiting();
        // What kimi_tui_choice::identity refuses: a pause, a provider error,
        // a changed process, a terminal this agent does not own.
        for refusal in ["Session identity changed; open Terminal","terminal is not in the expected agent input mode; open Terminal"] {
            assert!(!waits(&r,Err(refusal.into()),|_|Ok(PANEL.into())),"{refusal}");
        }
        assert!(waits(&r,Ok(r.clone()),|_|Ok(PANEL.into())));
        assert!(waits(&r,Ok(r.clone()),|_|Err("Terminal snapshot is too large".into())),"a failed capture proves nothing");
        assert!(!waits(&r,Ok(r.clone()),|_|Ok("────────────────────────────────────────\n❯ \n".into())),"no chooser shown");
        // The panel is matched against the record just checked, not a re-read
        // one that may name another request by now.
        let mut other=r.clone();questions::observe(&mut other,&event("PermissionRequest",json!({"tool_input":{"command":"rm"}})));
        let other_panel=PANEL.replace("printf hi > out.txt","rm");
        assert!(!waits(&r,Ok(other),|_|Ok(other_panel)));
    }

    #[test] fn decisions_grant_once_deny_or_pass_feedback() {
        let decided=|offer:Value|decision(&offer).map(|output|output["hookSpecificOutput"]["decision"].clone());
        assert_eq!(decided(json!({"decision":"allow"})).unwrap(),json!({"behavior":"allow"}));
        assert_eq!(decided(json!({"decision":"deny"})).unwrap(),json!({"behavior":"deny","message":DENIED,"interrupt":true}));
        assert_eq!(decided(json!({"decision":"feedback","message":"Use printf other"})).unwrap(),
            json!({"behavior":"deny","message":"Use printf other"}));
        assert_eq!(decision(&json!({"decision":"allow"})).unwrap()["hookSpecificOutput"]["hookEventName"],"PermissionRequest");
        for invalid in [json!({}),json!({"decision":"allow","message":"x"}),json!({"decision":"deny","message":"x"}),
            json!({"decision":"feedback"}),json!({"decision":"feedback","message":"  "}),json!({"decision":"feedback","message":"a\nb"}),
            json!({"decision":"always"})] {
            assert!(decision(&invalid).is_err(),"{invalid}");
        }
    }
}
