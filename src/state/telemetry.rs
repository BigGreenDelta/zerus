//! Hook-derived display summaries. Native workspace metadata never enters this model.
use super::*;

pub(super) fn grouped_active(record: &Value) -> u64 {
    if record["kimi_native_roster"] == true { return 0; }
    record["subagent_groups"]
        .as_object()
        .map(|groups| {
            groups
                .values()
                .map(|group| group["active_count"].as_u64().unwrap_or(0))
                .sum()
        })
        .unwrap_or(0)
}

pub(super) fn child_working(record: &Value, child: &Value) -> bool {
    child["state"] == "working" && (record["kimi_native_roster"] != true
        || child["external"] == true || child["native_roster"] == true)
}

/// Whether the listing names this child. A Kimi hook names only a profile,
/// so its entry stands for that profile's group; a native roster lists its own.
pub(super) fn listed(record: &Value, child: &Value) -> bool {
    string(record, "agent") != "kimi" || child["external"] == true
        || (record["kimi_native_roster"] == true && child["native_roster"] == true)
}

/// Work that keeps a session from Ready beside its recorded activity: Kimi
/// profile groups, which can outlast the main turn's Stop, open tools and
/// working children. Idle-only actions such as pause, clear and compaction
/// wait for all of it.
pub(super) fn work_remains(record: &Value) -> bool {
    grouped_active(record) > 0
        || record["active_tools"].as_object().is_some_and(|tools| !tools.is_empty())
        || record["subagents"].as_object().is_some_and(|children| {
            children.values().any(|child| child_working(record, child))
        })
}

/// Kimi external hooks expose a profile name, not the Swarm child's identity.
/// Count observed tasks per profile without assigning a Stop to an invented ID.
pub(super) fn observe(record: &mut Value, event: &Value) {
    let kind = string(event, "hook_event_name");
    if string(record, "agent") != "kimi"
        || !["", "main"].contains(&string(event, "agent_id"))
        || !["SubagentStart", "SubagentStop"].contains(&kind)
    {
        return;
    }
    let profile = journal::clipped(&event["agent_name"], 160);
    if profile.is_empty() {
        return;
    }
    if record.as_object_mut().unwrap().remove("kimi_native_roster") == Some(json!(true)) {
        // Native completions need not have emitted profile Stop hooks, so the
        // old profile totals lose authority when a newer hook beats the catalog.
        record["subagent_groups_complete"] = json!(false);
    }
    if record.get("subagent_groups_complete").is_none() {
        record["subagent_groups_complete"] = json!(record["subagents"]
            .as_object()
            .is_none_or(|children| children.is_empty()));
    }
    let mut groups = record["subagent_groups"]
        .as_object()
        .cloned()
        .unwrap_or_default();
    if !groups.contains_key(&profile) && groups.len() >= 64 {
        record["subagent_groups_complete"] = json!(false);
        return;
    }
    let group = groups.entry(profile.clone()).or_insert_with(|| {
        json!({"label":profile,
        "active_count":0,"completed_count":0,"total_count":0})
    });
    let active = group["active_count"].as_u64().unwrap_or(0);
    if kind == "SubagentStart" {
        group["active_count"] = json!(active + 1);
        group["total_count"] = json!(group["total_count"].as_u64().unwrap_or(0) + 1);
        let detail = journal::clipped(&event["prompt"], 240);
        if !detail.is_empty() {
            group["detail"] = json!(detail);
        }
    } else {
        group["active_count"] = json!(active.saturating_sub(1));
        group["completed_count"] = json!(group["completed_count"].as_u64().unwrap_or(0) + 1);
        if active == 0 {
            record["subagent_groups_complete"] = json!(false);
            group["total_count"] = json!(group["total_count"].as_u64().unwrap_or(0) + 1);
        }
    }
    group["updated"] = json!(now());
    record["subagent_groups"] = json!(groups);
}

pub(super) fn unavailable() -> Value {
    json!({"subagent_source":"unavailable","subagent_counts_complete":false,
        "subagent_active_count":null,"subagent_completed_count":null,"subagent_total_count":null,
        "subagent_previews":[],"activity_summary":"Status unavailable","activity_detail":""})
}

fn children(record: &Value) -> Value {
    let mut result = unavailable();
    let observed = record["last_event_at"].as_f64().unwrap_or(0.0) > 0.0;
    let children = record["subagents"].as_object();
    let groups = record["subagent_groups"].as_object();
    let native_kimi = record["kimi_native_roster"] == true;
    let profiles = string(record, "agent") == "kimi" && !native_kimi
        && (groups.is_some() || children.is_some_and(|children| !children.is_empty()));
    let external = children.is_some_and(|c| c.values().any(|v| v["external"] == true));
    if profiles && !external {
        result["subagent_source"] = json!("hook_profiles");
        result["subagent_counts_complete"] = json!(record["subagent_groups_complete"] == true);
        let Some(groups) = groups else {
            return result;
        };
        let mut previews:Vec<_>=groups.iter().filter(|(_,group)|group["active_count"].as_u64().unwrap_or(0)>0)
            .map(|(profile,group)|json!({"id":format!("profile:{profile}"),"label":profile,
                "state":"working","detail":journal::clipped(&group["detail"],240),"current_tool":"","group":true,
                "active_count":group["active_count"],"completed_count":group["completed_count"],
                "total_count":group["total_count"],"updated":group["updated"]})).collect();
        newest(&mut previews);
        for (key, field) in [
            ("active_count", "subagent_active_count"),
            ("completed_count", "subagent_completed_count"),
            ("total_count", "subagent_total_count"),
        ] {
            result[field] = json!(groups
                .values()
                .map(|g| g[key].as_u64().unwrap_or(0))
                .sum::<u64>());
        }
        result["subagent_previews"] = json!(previews);
        return result;
    }
    if !observed && children.is_none_or(|children| children.is_empty()) {
        return result;
    }
    let mut active = 0;
    let mut completed = if native_kimi { 0 } else { record["subagents_completed_pruned"].as_u64().unwrap_or(0) };
    let mut unknown = 0;
    let mut previews = Vec::new();
    let mut roster = serde_json::Map::new();
    if let Some(children) = children {
        for (id, child) in children {
            if !listed(record, child) {
                continue;
            }
            roster.insert(id.clone(), json!({"name":string(child,"name"),
                "state":string(child,"state"),"display_state":child["display_state"],"attention_id":child["attention_id"],
                "provider":child.get("provider").unwrap_or(&record["agent"]),"external":child["external"],"model":child["model"],"reply_id":child["reply_id"],
                "detail":journal::clipped(&child["detail"],240),
                "current_tool":string(child,"current_tool"),"updated":child["updated"],
                "completion_source":child["completion_source"],"tracking_evidence":child["tracking_evidence"]}));
            let state = if string(child, "display_state") == "unknown" {
                "unknown"
            } else {
                string(child, "state")
            };
            match state {
                "working" => active += 1,
                "finished" => completed += 1,
                _ => unknown += 1,
            }
            if state != "finished" {
                previews.push(json!({"id":id,"label":string(child,"name"),"state":state,
                "detail":journal::clipped(&child["detail"],240),"current_tool":string(child,"current_tool"),"updated":child["updated"]}));
            }
        }
    }
    newest(&mut previews);
    if profiles && external {
        if let Some(groups) = groups {
            active += groups
                .values()
                .map(|g| g["active_count"].as_u64().unwrap_or(0))
                .sum::<u64>();
            completed += groups
                .values()
                .map(|g| g["completed_count"].as_u64().unwrap_or(0))
                .sum::<u64>();
        }
    }
    result["subagent_source"] = json!(if native_kimi {
        "kimi_native"
    } else if profiles && external {
        "mixed_hooks"
    } else {
        "hooks"
    });
    result["subagent_counts_complete"] = json!(
        unknown == 0
            && (!profiles || groups.is_none() || record["subagent_groups_complete"] == true)
    );
    result["subagent_active_count"] = json!(active);
    result["subagent_completed_count"] = json!(completed);
    result["subagent_unknown_count"] = json!(unknown);
    result["subagent_total_count"] = json!(active + completed + unknown);
    result["subagent_previews"] = json!(previews);
    result["subagents"] = json!(roster);
    result
}

fn newest(previews: &mut Vec<Value>) {
    previews.sort_by(|a, b| {
        b["updated"]
            .as_f64()
            .unwrap_or(0.0)
            .total_cmp(&a["updated"].as_f64().unwrap_or(0.0))
            .then_with(|| string(a, "id").cmp(string(b, "id")))
    });
    previews.truncate(6);
    for preview in previews {
        preview.as_object_mut().unwrap().remove("updated");
    }
}

pub(super) fn summary(
    record: &Value,
    activity: &Value,
    current_tool: &str,
    tool_detail: &str,
) -> Value {
    let mut result = children(record);
    let live = string(activity, "runtime_state") == "live";
    let busy = live && string(activity, "activity") == "busy";
    let phase = string(activity, "phase");
    let label = if !live {
        if record["state"] == "archived" {
            "Archived"
        } else if record["paused"] == true {
            "Paused"
        } else {
            "Stopped"
        }
    } else {
        match (string(activity, "activity"), phase) {
            (_, "approval") => "Waiting for approval",
            (_, "input") => "Waiting for input",
            (_, "compacting") => "Compacting context",
            (_, "interrupted") => "Interrupted",
            (_, "error") => "Needs attention",
            ("idle", _) => "Ready",
            ("busy", _) => "Working",
            _ => "Status unavailable",
        }
    };
    result["activity_summary"] = json!(if busy
        && !current_tool.is_empty()
        && !["approval", "input", "compacting"].contains(&phase)
    {
        current_tool
    } else {
        label
    });
    result["activity_detail"] = json!(if busy && !tool_detail.is_empty() {
        tool_detail.to_owned()
    } else if busy {
        journal::clipped(&record["prompt"], 240)
    } else {
        String::new()
    });
    result
}
