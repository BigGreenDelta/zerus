//! Mirror Claude's native tool approval panels in Activity. A pending
//! main-agent PermissionRequest stays visible while Claude shows a chooser. A
//! complete panel that matches the request exposes its native options verbatim;
//! options reaching beyond this one request are marked for a second
//! confirmation in the desktop. Zerus never chooses an option on its own.
use super::*;
use sha2::{Digest,Sha256};
use std::path::Path;

#[derive(Clone,Debug,PartialEq)]
struct Panel { title:String, body:Vec<String>, question:String, labels:Vec<String>, selected:usize }

// Claude also reports AskUserQuestion as a PermissionRequest; claude_question
// owns that card, or native_question when no tool call exists. Plan approval can turn an option into a text field, so its
// card only points to Terminal.
const QUESTION_TOOLS:[&str;3]=["AskUserQuestion","request_user_input","AskUser"];
const TERMINAL_ONLY_TOOLS:[&str;2]=["ExitPlanMode","EnterPlanMode"];
// One-time answers. Every other label, including an unknown one, grants,
// blocks or switches more than the visible request.
const ONCE:[&str;6]=["Yes","No","Yes, but ask again next time","No, and ask again next time",
    "No, and tell Claude what to do differently (esc)","No, and tell Claude what to do differently"];

pub(super) fn question_tool(tool:&str)->bool {QUESTION_TOOLS.contains(&tool)}
fn rule(row:&str,c:char)->bool {row.chars().count()>=20&&row.chars().all(|x|x==c)}
fn numbered(row:&str)->Option<(bool,usize,String)> {
    let (mark,rest)=match row.strip_prefix('❯'){Some(rest)=>(true,rest.trim_start()),None=>(false,row)};
    let (number,label)=rest.split_once(". ")?;
    if number.is_empty()||number.len()>2||!number.chars().all(|c|c.is_ascii_digit())||label.trim().is_empty(){return None;}
    Some((mark,number.parse().ok()?,label.trim().to_owned()))
}
fn label(text:&str)->String {text.split_whitespace().collect::<Vec<_>>().join(" ")}
fn compact(text:&str)->String {text.chars().filter(|c|!c.is_whitespace()).collect()}

/// Any highlighted numbered choice below Claude's last full-width rule. The
/// idle composer sits between two rules, so a resolved request has none.
fn visible(screen:&str)->bool {
    let rows:Vec<_>=screen.lines().map(str::trim).collect();
    let start=rows.iter().rposition(|r|rule(r,'─')).map_or(0,|i|i+1);
    rows[start..].iter().any(|r|numbered(r).is_some_and(|(mark,_,_)|mark))
}

/// The chooser at the bottom of the screen: a question, its consecutive
/// numbered options with exactly one highlighted, wrapped option rows, then
/// only blank rows and an optional separated "Esc to ..." footer.
fn chooser(rows:&[&str])->Option<(usize,Vec<String>,usize)> {
    let mut end=rows.len();
    while end>0&&rows[end-1].is_empty(){end-=1;}
    if end>0&&rows[end-1].starts_with("Esc to ") {
        end-=1;
        if end==0||!rows[end-1].is_empty(){return None;}
        while end>0&&rows[end-1].is_empty(){end-=1;}
    }
    let first=(0..end).rev().find(|&i|numbered(rows[i]).is_some_and(|(_,n,_)|n==1))?;
    let question=first.checked_sub(1)?;
    if !rows[question].ends_with('?')||numbered(rows[question]).is_some(){return None;}
    let mut labels:Vec<String>=Vec::new();let mut selected=None;
    for row in &rows[first..end] {
        match numbered(row) {
            Some((mark,n,text)) if n==labels.len()+1 => {
                if mark&&selected.replace(labels.len()).is_some(){return None;}
                labels.push(text);
            }
            Some(_) => return None,
            None if row.is_empty()||row.contains('❯') => return None,
            None => labels.last_mut()?.push_str(&format!("\n{row}")),
        }
    }
    if labels.len()<2||labels.len()>9 {return None;}
    Some((question,labels,selected?))
}

fn parse(screen:&str)->Option<Panel> {
    let rows:Vec<_>=screen.lines().map(str::trim).collect();
    let (question,labels,selected)=chooser(&rows)?;
    // A panel taller than the terminal loses its top rule and title.
    let top=rows[..question].iter().rposition(|r|rule(r,'─'))?;
    let title=*rows.get(top+1).filter(|r|top+1<question&&!r.is_empty()&&!rule(r,'╌'))?;
    let mut body:Vec<String>=rows[top+2..question].iter().map(|r|r.to_string()).collect();
    while body.last().is_some_and(String::is_empty){body.pop();}
    Some(Panel{title:title.into(),body,question:rows[question].into(),labels,selected})
}

fn eligible(record:&Value)->bool {
    record["agent"]=="claude"&&record["phase"]=="approval"&&!pause_active(record)&&string(record,"error").is_empty()
}
fn request(record:&Value)->Option<&Value> {
    let request=&record["pending_approval"];
    let tool=string(request,"tool_name");
    (request.is_object()&&!tool.is_empty()&&!question_tool(tool)).then_some(request)
}

/// The panel must show the request the agent made. Bash keeps its exact
/// command check; file and fetch panels must name their target.
fn matches(request:&Value,panel:&Panel)->bool {
    let input=&request["tool_input"];
    if request["tool_name"]=="Bash" {
        let command=string(input,"command");
        if command.trim().is_empty()||command.len()>64000 {return false;}
        let dividers:Vec<_>=panel.body.iter().enumerate().filter(|(_,r)|rule(r,'╌')).map(|(i,_)|i).collect();
        return match dividers[..] {
            [first,second] => label(&panel.body[first+1..second].join("\n"))==label(command),
            // Claude 2.1.284 renders the command and its description without
            // code fences. Match the entire body, including the optional tip.
            [] => {
                const TIP:&str="Tip: auto mode handles these prompts for you — choose \"switch to auto mode\" below";
                let content=panel.body.join("\n");let content=content.trim();
                let content=content.strip_prefix(TIP).unwrap_or(content);
                label(content)==label(&format!("{command}\n{}",string(input,"description")))
            }
            _ => false,
        };
    }
    let text=compact(&format!("{}\n{}\n{}",panel.title,panel.body.join("\n"),panel.question));
    let mut targets=Vec::new();
    for key in ["file_path","notebook_path","path"] {
        if let Some(path)=input[key].as_str().filter(|p|!p.is_empty()) {
            targets.push(Path::new(path).file_name().and_then(|n|n.to_str()).unwrap_or(path).to_owned());
        }
    }
    if let Some(url)=input["url"].as_str().filter(|u|!u.is_empty()) {
        targets.push(url.split("://").nth(1).unwrap_or(url).split(['/','?','#']).next().unwrap_or(url).to_owned());
    }
    targets.iter().all(|target|text.contains(&compact(target)))
}

fn summary(request:&Value)->String {
    let input=&request["tool_input"];
    let target=["command","file_path","notebook_path","url","path","pattern"].iter()
        .find_map(|key|input[*key].as_str().filter(|v|!v.trim().is_empty()))
        .map(str::to_owned)
        .unwrap_or_else(||journal::clipped(&json!(input.to_string()),2000));
    format!("{}\n\n{}",string(request,"tool_name"),target)
}

fn tool_call_id(record:&Value,tool:&str)->String {
    let ids:Vec<_>=record["active_tools"].as_object().into_iter().flatten().filter(|(_,v)|v["name"]==tool).map(|(k,_)|k.clone()).collect();
    if ids.len()==1 {ids[0].clone()} else {String::new()}
}

/// The card for the pending request, if Claude still shows a chooser. Without
/// a terminal snapshot the request stays visible but cannot be answered here.
fn build(record:&Value,screen:Option<&str>)->Option<(Value,Option<Panel>)> {
    if !eligible(record) {return None;}
    let request=request(record)?;
    if screen.is_some_and(|screen|!visible(screen)) {return None;}
    let tool=string(request,"tool_name");
    let panel=screen.and_then(parse);
    let problem=match &panel {
        None if screen.is_none() => "Zerus cannot read this terminal. Answer the request in Terminal.",
        None => "Zerus cannot read this approval completely. Enlarge Terminal or answer it there.",
        Some(_) if TERMINAL_ONLY_TOOLS.contains(&tool) => "Review this plan in Terminal.",
        Some(panel) if !matches(request,panel) => "The visible approval does not match the agent's request. Answer it in Terminal.",
        Some(_) => "",
    };
    let native=panel.as_ref().filter(|_|problem.is_empty());
    let hash=format!("{:x}",Sha256::digest(json!([record["run_id"],record["conversation_id"],record["pane"],record["pid"],
        record["process_start"],tool,request["tool_input"],native.map(|p|json!([p.title,p.body,p.question,p.labels]))]).to_string()));
    let (question,body,options)=match native {
        Some(panel) => {
            let content=panel.body.iter().map(|r|if rule(r,'╌'){""}else{r.as_str()}).collect::<Vec<_>>().join("\n");
            let options=panel.labels.iter().enumerate().map(|(i,text)|{
                let text=label(text);
                json!({"id":(i+1).to_string(),"label":text,"scope":if ONCE.contains(&text.as_str()){"once"}else{"broader"}})
            }).collect::<Vec<_>>();
            (panel.question.clone(),format!("{}\n\n{}",panel.title,content.trim()).trim().to_owned(),options)
        }
        None => (format!("Claude asks to use {tool}"),summary(request),Vec::new()),
    };
    let created=record["pending_approval_at"].as_f64().map_or(record["last_event_at"].clone(),|at|json!(at));
    Some((json!({"question_id":format!("claude-approval:{hash}"),"question_hash":hash,"tool_call_id":tool_call_id(record,tool),
        "run_id":record["run_id"],"conversation_id":record["conversation_id"],"source":"claude_tool_approval",
        "can_answer":native.is_some(),"answer_unavailable_reason":problem,"approval":true,"approval_choices":native.is_some(),
        "answer_transport":"claude_tui","created_at":created,
        "questions":[{"id":"approval","question":question,"header":tool,"body":body,"allow_other":false,"options":options}]}),panel))
}

/// Claude Code asks some questions itself, such as a model switch after a
/// safeguard stop. They arrive only as an AskUserQuestion PermissionRequest:
/// without a tool call there is no tool result to acknowledge an answer, so
/// the card mirrors the request and points to Terminal. A model question has
/// an active tool call and keeps claude_question's answerable card.
pub(super) fn native_question(record:&Value)->Option<Value> {
    let request=&record["pending_approval"];
    let tool=string(request,"tool_name");
    if record["agent"]!="claude"||record["phase"]!="input"||pause_active(record)||!string(record,"error").is_empty()
        ||!request.is_object()||!question_tool(tool)
        ||record["active_tools"].as_object().into_iter().flatten().any(|(_,active)|active["name"]==tool) {return None;}
    let mut items=questions::normalize(&request["tool_input"]["questions"]).unwrap_or_else(|_|json!([{"id":"q_0",
        "question":"Claude asks a question in Terminal","header":tool,"body":summary(request),"options":[]}]));
    for item in items.as_array_mut().into_iter().flatten() {item["allow_other"]=json!(false);}
    let hash=format!("{:x}",Sha256::digest(json!([record["run_id"],record["conversation_id"],record["pane"],record["pid"],
        record["process_start"],tool,request["tool_input"]]).to_string()));
    let created=record["pending_approval_at"].as_f64().map_or(record["last_event_at"].clone(),|at|json!(at));
    Some(json!({"question_id":format!("claude-native-question:{hash}"),"question_hash":hash,"tool_call_id":"",
        "run_id":record["run_id"],"conversation_id":record["conversation_id"],"source":"claude_native_question",
        "can_answer":false,"answer_transport":"","answer_unavailable_reason":"Claude Code asks this itself. Answer it in Terminal.",
        "created_at":created,"questions":items}))
}

fn snapshot(record:&Value,screen:&str)->Option<(Value,usize)> {
    let (card,panel)=build(record,Some(screen))?;
    if card["can_answer"]!=true {return None;}
    Some((card["question_hash"].clone(),panel?.selected))
}
fn acknowledged(screen:&str)->bool {!screen.trim().is_empty()&&!visible(screen)}

pub(super) fn current(record:&Value)->Option<Value> {
    if !eligible(record)||request(record).is_none() {return None;}
    let screen=kimi_tui_choice::identity(record).and_then(|record|kimi_tui_choice::capture(&record)).ok();
    build(record,screen.as_deref()).map(|(card,_)|card)
}
pub(super) fn available(record:&Value,question:&Value)->Result<()> {
    let current=current(record).ok_or("The native approval is no longer visible. Open Terminal")?;
    if current["question_hash"]!=question["question_hash"] {return Err("The approval changed. Review its new request".into());}
    if current["can_answer"]!=true {return Err(string(&current,"answer_unavailable_reason").to_owned());}
    Ok(())
}
pub(super) fn answer(record:&Value,question:&Value,answers:&Value)->Result<()> {
    available(record,question)?;
    let panel=parse(&kimi_tui_choice::capture(record)?).ok_or("Approval disappeared")?;
    let chosen=answers[0]["selected_option_ids"][0].as_str().and_then(|id|id.parse::<usize>().ok())
        .filter(|n|(1..=panel.labels.len()).contains(n)).ok_or("Choose one of Claude's options")?-1;
    kimi_tui_choice::answer_queued(record,question,chosen,panel.labels.len(),snapshot,acknowledged)?;
    // A denial ends Claude's turn without another hook. Record it as an
    // interruption so the composer can verify Claude's prompt again.
    let _ = settle(record,&label(&panel.labels[chosen]));
    Ok(())
}
fn settle(original:&Value,chosen:&str)->Result<()> {
    let _guard=lock(None)?;
    let Some(mut record)=read_run(string(original,"name"),string(original,"run_id"))? else {return Ok(())};
    if record["pending_approval"]!=original["pending_approval"]||record["phase"]!="approval" {return Ok(());}
    record.as_object_mut().unwrap().remove("pending_approval");
    record.as_object_mut().unwrap().remove("pending_approval_at");
    if chosen.starts_with("No") {
        record["activity"]=json!("unknown");record["phase"]=json!("interrupted");
        record["main_done"]=json!(false);record["active_tools"]=json!({});
    } else {
        let tools=record["active_tools"].as_object().is_some_and(|tools|!tools.is_empty());
        record["phase"]=json!(if tools {"tool"} else {"working"});
    }
    write(&mut record)
}

#[cfg(test)] mod tests {
    use super::*;
    // Captured from Claude Code 2.1.294 at 120 columns; paths are synthetic.
    const BASH:&str="  ⎿  $ printf hi > out.txt\n\n────────────────────────────────────────\n Bash command\n Tip: auto mode handles these prompts for you — choose \"switch to auto mode\" below\n Write \"hi\" into out.txt without a trailing newline\n╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌\n printf hi > out.txt\n╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌\n Do you want to proceed?\n ❯ 1. Yes\n   2. Yes, and always allow access to\n      /work/project from this\n      project\n   3. Yes, and switch to auto mode · auto mode handles these prompts for you\n   4. No\n\n Esc to cancel · Tab to amend\n\n\n\n";
    const WRITE:&str="● Write(new.txt)\n\n────────────────────────────────────────\n Create file\n new.txt\n╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌\n  1 x\n╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌\n Do you want to create new.txt?\n ❯ 1. Yes\n   2. Yes, and switch to accept edits (auto-approve file edits and common file commands) for this session (shift+tab)\n   3. No\n\n Esc to cancel · Tab to amend\n\n";
    const FETCH:&str="● Fetch(https://example.com)\n\n────────────────────────────────────────\n Fetch\n Claude wants to fetch content from example.com\n╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌\n url: https://example.com/\n prompt: title?\n╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌\n Do you want to allow Claude to fetch this content?\n ❯ 1. Yes\n   2. Yes, and don't ask again for example.com\n   3. No, and tell Claude what to do differently (esc)\n\n\n\n";
    const OUTSIDE:&str="────────────────────────────────────────\n Read outside the working directories\n╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌\n Read(/home/user/.codex/generated_images/0000aaaa-1111-2222-3333-444455556666/exec-77778888-9999-aaaa-bbbb-cc\n ccddddeeee.png)\n╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌\n Auto mode and the sandbox read outside the working directories without asking. Yes or No answers for this read; the\n rest is for later reads. Block: the file tools refuse reads outside the working directories in every project.\n\n Allow this read outside the working directories?\n ❯ 1. Yes, and keep allowing any reads outside the working directories\n   2. No, and block reads outside the working directories from now on\n   3. No, and ask again next time\n   4. Yes, but ask again next time\n\n Esc to cancel · Tab to amend\n";
    const PATH:&str="/home/user/.codex/generated_images/0000aaaa-1111-2222-3333-444455556666/exec-77778888-9999-aaaa-bbbb-ccccddddeeee.png";
    const DENIED:&str="❯ Use the tool now.\n\n● Fetch(https://example.com)\n  ⎿  Interrupted · What should Claude do instead?\n\n────────────────────────────────────────\n❯ \n────────────────────────────────────────\n  ⏸ manual mode on · ? for shortcuts\n";
    fn record(tool:&str,input:Value)->Value {
        json!({"agent":"claude","phase":"approval","run_id":"run","conversation_id":"conversation","pending_approval_at":10.0,
            "active_tools":{"toolu_one":{"name":tool}},"pending_approval":{"hook_event_name":"PermissionRequest","tool_name":tool,"tool_input":input}})
    }
    fn card(record:&Value,screen:&str)->Value {build(record,Some(screen)).unwrap().0}
    fn options(card:&Value)->Vec<(String,String)> {
        card["questions"][0]["options"].as_array().unwrap().iter().map(|o|(string(o,"label").to_owned(),string(o,"scope").to_owned())).collect()
    }

    #[test] fn native_options_are_verbatim_and_broader_ones_are_marked() {
        let bash=card(&record("Bash",json!({"command":"printf hi > out.txt","description":"Write \"hi\" into out.txt without a trailing newline"})),BASH);
        assert_eq!(bash["can_answer"],true);assert_eq!(bash["tool_call_id"],"toolu_one");assert_eq!(bash["created_at"],10.0);
        assert_eq!(bash["questions"][0]["question"],"Do you want to proceed?");
        assert!(string(&bash["questions"][0],"body").starts_with("Bash command\n\n"));
        assert!(string(&bash["questions"][0],"body").ends_with("without a trailing newline\n\nprintf hi > out.txt"));
        assert_eq!(options(&bash),[("Yes","once"),("Yes, and always allow access to /work/project from this project","broader"),
            ("Yes, and switch to auto mode · auto mode handles these prompts for you","broader"),("No","once")].map(|(l,s)|(l.to_owned(),s.to_owned())));
        let outside=card(&record("Read",json!({"file_path":PATH})),OUTSIDE);
        assert_eq!(outside["can_answer"],true);assert_eq!(outside["questions"][0]["question"],"Allow this read outside the working directories?");
        assert_eq!(options(&outside).iter().map(|(_,s)|s.as_str()).collect::<Vec<_>>(),["broader","broader","once","once"]);
        let fetch=card(&record("WebFetch",json!({"url":"https://example.com","prompt":"title?"})),FETCH);
        assert_eq!(fetch["can_answer"],true);assert_eq!(options(&fetch)[2].1,"once");
        let write=card(&record("Write",json!({"file_path":"/work/project/new.txt","content":"x"})),WRITE);
        assert_eq!(write["can_answer"],true);assert_eq!(options(&write)[1].1,"broader");
        assert_eq!(parse(OUTSIDE).unwrap().selected,0);
        assert_eq!(parse(&BASH.replace(" ❯ 1. Yes","   1. Yes").replace("   4. No"," ❯ 4. No")).unwrap().selected,3);
    }

    #[test] fn mismatched_or_unreadable_panels_stay_visible_for_terminal() {
        let other=card(&record("Read",json!({"file_path":"/home/user/other.png"})),OUTSIDE);
        assert_eq!(other["can_answer"],false);assert!(options(&other).is_empty());
        assert_eq!(other["questions"][0]["body"],"Read\n\n/home/user/other.png");
        let bash=record("Bash",json!({"command":"rm changed"}));
        assert_eq!(card(&bash,BASH)["can_answer"],false);
        // The top of a tall panel scrolled away: the chooser remains visible.
        let clipped=OUTSIDE.split_once("Read outside the working directories\n").unwrap().1;
        assert!(parse(clipped).is_none());assert_eq!(card(&record("Read",json!({"file_path":PATH})),clipped)["can_answer"],false);
        let unread=build(&record("Read",json!({"file_path":PATH})),None).unwrap().0;
        assert_eq!(unread["can_answer"],false);assert_eq!(unread["questions"][0]["question"],"Claude asks to use Read");
        let plan=card(&record("ExitPlanMode",json!({"plan":"Ship it"})),FETCH);
        assert_eq!(plan["answer_unavailable_reason"],"Review this plan in Terminal.");
    }

    #[test] fn resolved_question_and_unrelated_choosers_hide_the_card() {
        let read=record("Read",json!({"file_path":PATH}));
        assert!(build(&read,Some(DENIED)).is_none());assert!(acknowledged(DENIED));assert!(!acknowledged(OUTSIDE));
        let mut asked=record("AskUserQuestion",json!({"questions":[]}));assert!(build(&asked,Some(OUTSIDE)).is_none());
        asked["pending_approval"]["tool_name"]=json!("Read");asked["phase"]=json!("input");assert!(build(&asked,Some(OUTSIDE)).is_none());
        // Extra or malformed rows below the options are not this chooser.
        assert!(parse(&OUTSIDE.replace("Esc to cancel · Tab to amend","Status line")).is_none());
        assert!(parse(&OUTSIDE.replace("   4. Yes, but ask again next time","   5. Yes, but ask again next time")).is_none());
        assert!(parse(&OUTSIDE.replace("   3. No, and","❯  3. No, and")).is_none());
        assert!(parse(&OUTSIDE.replace("\n\n Esc to cancel","\n Esc to cancel")).is_none());
    }

    #[test] fn claude_code_questions_without_a_tool_call_point_to_terminal() {
        // Claude Code asks this itself after a safeguard stop: no PreToolUse,
        // no active tool, only the permission prompt of AskUserQuestion.
        let input=json!({"metadata":{"source":"refusal_fallback_preference"},"questions":[{"header":"Model switch",
            "multiSelect":false,"question":"Switch models and keep going whenever this happens?","options":[
            {"label":"Switch automatically","description":"Continue on the other model now"},
            {"label":"Stay","description":"Stop here without switching"}]}]});
        let mut asked=record("AskUserQuestion",input);asked["phase"]=json!("input");asked["active_tools"]=json!({});
        let card=native_question(&asked).unwrap();
        assert_eq!((card["source"].clone(),card["can_answer"].clone(),card["created_at"].clone()),
            (json!("claude_native_question"),json!(false),json!(10.0)));
        assert_eq!(card["answer_unavailable_reason"],"Claude Code asks this itself. Answer it in Terminal.");
        let item=&card["questions"][0];
        assert_eq!((item["header"].clone(),item["allow_other"].clone()),(json!("Model switch"),json!(false)));
        assert_eq!(item["question"],"Switch models and keep going whenever this happens?");
        assert_eq!(item["options"][0]["label"],"Switch automatically");
        assert_eq!(item["options"][1]["description"],"Stop here without switching");
        // A request Zerus cannot normalize still shows that Claude waits.
        let mut odd=asked.clone();odd["pending_approval"]["tool_input"]=json!({"questions":"?"});
        let odd=native_question(&odd).unwrap();
        assert_eq!(odd["questions"][0]["question"],"Claude asks a question in Terminal");
        assert!(odd["questions"][0]["options"].as_array().unwrap().is_empty());
        assert_ne!(odd["question_hash"],card["question_hash"]);
        // The model's own question keeps claude_question's answerable card.
        let mut model=asked.clone();model["active_tools"]=json!({"toolu_ask":{"name":"AskUserQuestion"}});
        assert!(native_question(&model).is_none());
        let mut resolved=asked.clone();resolved["phase"]=json!("working");assert!(native_question(&resolved).is_none());
        let mut tool=asked.clone();tool["pending_approval"]["tool_name"]=json!("Read");assert!(native_question(&tool).is_none());
        assert!(build(&asked,None).is_none());
    }

    #[test] fn hash_ignores_the_highlight_but_not_the_request() {
        let read=record("Read",json!({"file_path":PATH}));
        let moved=OUTSIDE.replace(" ❯ 1. Yes, and keep","   1. Yes, and keep").replace("   2. No, and block"," ❯ 2. No, and block");
        assert_eq!(snapshot(&read,OUTSIDE).unwrap().0,snapshot(&read,&moved).unwrap().0);
        assert_eq!(snapshot(&read,&moved).unwrap().1,1);
        let mut other=read.clone();other["pid"]=json!(2);
        assert_ne!(snapshot(&read,OUTSIDE).unwrap().0,snapshot(&other,OUTSIDE).unwrap().0);
    }
}
