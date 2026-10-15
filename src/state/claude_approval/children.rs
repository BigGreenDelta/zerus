//! Child permission evidence is independent of the parent's turn. Never drop a
//! sibling request on parent progress, or guess which identical request is up.
use super::*;

const LIMIT:usize=128;
const KEY:&str="pending_child_approvals";
const OVERFLOW:&str="child_approval_overflow";

pub(super) fn observe(record:&mut Value,event:&Value) {
    if record["agent"]!="claude" {return;}
    let kind=string(event,"hook_event_name");
    let child=string(event,"agent_id");
    if child.is_empty() {
        if ["SessionStart","SessionEnd"].contains(&kind) {
            record.as_object_mut().unwrap().remove(KEY);
            record.as_object_mut().unwrap().remove(OVERFLOW);
        }
        return;
    }
    let mut pending=record[KEY].as_object().cloned().unwrap_or_default();
    if kind=="PermissionRequest"&&!question_tool(string(event,"tool_name")) {
        // Do not evict evidence: losing a duplicate could make an ambiguous
        // panel appear uniquely answerable. A new native session resets this.
        if pending.len()>=LIMIT {record[OVERFLOW]=json!(true);return;}
        pending.insert(uuid::Uuid::new_v4().to_string(),json!({"request":event,"at":now()}));
    } else if ["Stop","StopFailure","SubagentStop","SessionEnd","Interrupt"].contains(&kind) {
        pending.retain(|_,entry|string(&entry["request"],"agent_id")!=child);
    } else if ["PostToolUse","PostToolUseFailure"].contains(&kind) {
        let matching:Vec<_>=pending.iter().filter(|(_,entry)|{
            let request=&entry["request"];
            string(request,"agent_id")==child&&request["tool_name"]==event["tool_name"]
                &&request["tool_input"].is_object()&&request["tool_input"]==event["tool_input"]
                &&(string(request,"tool_use_id").is_empty()||request["tool_use_id"]==event["tool_use_id"])
        }).map(|(id,_)|id.clone()).collect();
        // Without a unique completion identity, keep the evidence until the
        // child's explicit stop. The visible-panel guard still blocks answers.
        if let [id]=&matching[..] {pending.remove(id);}
    }
    if pending.is_empty() {record.as_object_mut().unwrap().remove(KEY);}
    else {record[KEY]=json!(pending);}
}

pub(super) fn entries(record:&Value)->impl Iterator<Item=(&String,&Value)> {
    record[KEY].as_object().into_iter().flatten()
}
pub(super) fn overflow(record:&Value)->bool {record[OVERFLOW]==true}

pub(super) fn settle(record:&mut Value,original:&Value,id:&str)->bool {
    let Some(entry)=original[KEY].get(id) else {return false};
    if record[KEY].get(id)!=Some(entry) {return false;}
    record[KEY].as_object_mut().unwrap().remove(id);
    // Parent work, tools, sibling attention and the child's native state are
    // owned by their hooks. An answer is not proof that any turn has ended.
    true
}

#[cfg(test)] mod tests {
    use super::*;
    #[test] fn overflow_preserves_every_recorded_request_and_blocks_until_session_start() {
        let mut r=json!({"agent":"claude"});
        let event=json!({"agent_id":"child","hook_event_name":"PermissionRequest","tool_name":"Bash","tool_input":{"command":"same"}});
        for _ in 0..LIMIT {observe(&mut r,&event);}
        let saved=r[KEY].clone();observe(&mut r,&event);
        assert_eq!(r[KEY],saved);assert!(overflow(&r));
        observe(&mut r,&json!({"agent_id":"child","hook_event_name":"PostToolUse","tool_name":"Bash","tool_input":{"command":"same"}}));
        assert_eq!(r[KEY],saved); // one ambiguous completion cannot drop all copies
        observe(&mut r,&json!({"hook_event_name":"SessionStart"}));
        assert!(r.get(KEY).is_none());assert!(!overflow(&r));
    }
}
