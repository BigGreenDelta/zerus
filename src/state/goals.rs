//! Provider-owned objectives are independent of the current tool or turn.
//! Read-only, versioned adapters, bound to the exact account and conversation.
use super::*;
use rusqlite::{Connection, OpenFlags, OptionalExtension};
use sha2::{Digest, Sha256};
use std::fs::{self, File};
use std::io::{BufRead, BufReader, Seek, SeekFrom};
use std::os::unix::fs::MetadataExt;
use std::time::Duration;

fn codex_goal(path: &Path, conversation: &str) -> rusqlite::Result<Option<Value>> {
    let db = Connection::open_with_flags(
        path,
        OpenFlags::SQLITE_OPEN_READ_ONLY | OpenFlags::SQLITE_OPEN_NO_MUTEX,
    )?;
    db.busy_timeout(Duration::from_millis(15))?;
    db.query_row(
        "SELECT goal_id, objective, status, token_budget, tokens_used, time_used_seconds, created_at_ms, updated_at_ms
         FROM thread_goals WHERE thread_id = ?1",
        [conversation],
        |row| {
            let id: String = row.get(0)?;
            let objective: String = row.get(1)?;
            let status: String = row.get(2)?;
            let budget: Option<i64> = row.get(3)?;
            let tokens: i64 = row.get(4)?;
            let seconds: i64 = row.get(5)?;
            let created: i64 = row.get(6)?;
            let updated: i64 = row.get(7)?;
            if id.is_empty() || objective.trim().is_empty() || tokens < 0 || seconds < 0
                || created <= 0 || updated < created || budget.is_some_and(|v| v <= 0)
                || !["active", "paused", "blocked", "usage_limited", "budget_limited", "complete"].contains(&status.as_str())
            {
                return Err(rusqlite::Error::InvalidQuery);
            }
            Ok(json!({"id":id,"objective":journal::clipped(&json!(objective), 12000),
                "status":status,"token_budget":budget,"tokens_used":tokens,
                "time_used_seconds":seconds,"created_at":created as f64 / 1000.0,
                "updated_at":updated as f64 / 1000.0,"source":"codex_goals_v1"}))
        },
    ).optional()
}

pub(super) fn summary(record: &Value) -> Value {
    if ["claude", "kimi"].contains(&string(record, "agent")) {
        return native_summary(record);
    }
    if string(record, "agent") != "codex" {
        return json!({"goal":null,"goal_source":"unsupported"});
    }
    let conversation = string(record, "conversation_id");
    if uuid::Uuid::parse_str(conversation).is_err() {
        return json!({"goal":null,"goal_source":"unavailable"});
    }
    let native_home = if string(record, "agent_home").is_empty() {
        home().join(".codex")
    } else {
        PathBuf::from(string(record, "agent_home"))
    };
    // Never create a database, mutate native goal state or scrape the TUI.
    match codex_goal(&native_home.join("goals_1.sqlite"), conversation) {
        Ok(goal) => json!({"goal":goal,"goal_source":"codex_goals_v1","goal_observed_at":now()}),
        Err(_) => json!({"goal":null,"goal_source":"unavailable"}),
    }
}

fn native_summary(record: &Value) -> Value {
    let mut kimi_wire = None;
    let result = (|| -> Result<(Value, &'static str)> {
        let (path, source) = match string(record, "agent") {
            "kimi" => {
                let path = questions::wire_paths(record)?
                    .into_iter()
                    .next()
                    .ok_or("No Kimi wire")?;
                kimi_wire = Some(path.clone());
                (path, "kimi_goal_v1")
            }
            "claude" => {
                let id = string(record, "conversation_id");
                if uuid::Uuid::parse_str(id).is_err() {
                    return Err("Unconfirmed conversation".into());
                }
                let path = PathBuf::from(string(record, "transcript"));
                if !path.is_absolute() || path.file_stem().and_then(|s| s.to_str()) != Some(id) {
                    return Err("Unconfirmed transcript".into());
                }
                (path, "claude_goal_status_v1")
            }
            _ => return Err("Unsupported provider".into()),
        };
        let state = replay_goal(&path, source)?;
        Ok((state, source))
    })();
    let mut summary = match result {
        Ok((goal, source)) => json!({"goal":goal,"goal_source":source,"goal_observed_at":now()}),
        Err(_) => json!({"goal":null,"goal_source":"unavailable"}),
    };
    if string(record, "agent") == "kimi" {
        // Recurring provider jobs open future turns without the person; null
        // while unknown. One-shot and expiring jobs are deleted as they fire.
        summary["recurring_schedules"] = json!(kimi_wire
            .and_then(|path| replay_goal(&path, "kimi_cron_v1").ok())
            .map(|jobs| jobs.as_object().map_or(0, |jobs| jobs.len())));
    }
    summary
}

// Persist only reduced goal state and a cursor; do not copy native history.
// Incremental reads keep long running conversations cheap to poll. A partial
// scan never presents an old active goal as the current one.
fn replay_goal(path: &Path, source: &str) -> Result<Value> {
    let key = format!(
        "{:x}",
        Sha256::digest(format!("{source}:{}", path.display()).as_bytes())
    );
    let cache_path = root().join("goal_cache").join(format!("{key}.json"));
    replay_goal_at(path, source, &cache_path)
}
fn replay_goal_at(path: &Path, source: &str, cache_path: &Path) -> Result<Value> {
    let mut reader = BufReader::new(File::open(path).map_err(|e| e.to_string())?);
    let metadata = reader.get_ref().metadata().map_err(|e| e.to_string())?;
    let _lock = lock(Some(&cache_path.with_extension("lock")))?;
    let mut cache: Value = fs::read(&cache_path)
        .ok()
        .and_then(|bytes| serde_json::from_slice(&bytes).ok())
        .unwrap_or(json!({}));
    let identity = json!([metadata.dev(), metadata.ino()]);
    let mut offset = cache["offset"].as_u64().unwrap_or(0);
    let prefix_len = cache["prefix_len"]
        .as_u64()
        .unwrap_or(metadata.len().min(256));
    let mut prefix = Vec::new();
    reader
        .by_ref()
        .take(prefix_len)
        .read_to_end(&mut prefix)
        .map_err(|e| e.to_string())?;
    let prefix_hash = format!("{:x}", Sha256::digest(&prefix));
    if cache["identity"] != identity
        || metadata.len() < offset
        || cache["prefix_hash"] != prefix_hash
    {
        cache = json!({});
        offset = 0;
    }
    let mut goal = cache["goal"].clone();
    let mut skipping = cache["skipping"].as_bool().unwrap_or(false);
    reader
        .seek(SeekFrom::Start(offset))
        .map_err(|e| e.to_string())?;
    let start = offset;
    let mut line = Vec::new();
    while offset < metadata.len() && offset - start < 16 * 1024 * 1024 {
        line.clear();
        let count = reader
            .by_ref()
            .take(2 * 1024 * 1024)
            .read_until(b'\n', &mut line)
            .map_err(|e| e.to_string())?;
        if count == 0 {
            break;
        }
        if !line.ends_with(b"\n") {
            if count == 2 * 1024 * 1024 || skipping {
                offset += count as u64;
                skipping = true;
                continue;
            }
            break;
        }
        offset += count as u64;
        if skipping {
            skipping = false;
            continue;
        }
        let relevant = match source {
            "kimi_goal_v1" => {
                line.windows(5).any(|v| v == b"goal.") || line.windows(6).any(|v| v == b"forked")
            }
            "kimi_cron_v1" => line.windows(6).any(|v| v == b"\"cron."),
            _ => line.windows(11).any(|v| v == b"goal_status"),
        };
        if relevant {
            if let Ok(event) = serde_json::from_slice::<Value>(&line) {
                match source {
                    "kimi_goal_v1" => reduce_kimi(&mut goal, &event),
                    "kimi_cron_v1" => reduce_kimi_cron(&mut goal, &event),
                    _ => reduce_claude(&mut goal, &event),
                }
            }
        }
    }
    if offset != cache["offset"].as_u64().unwrap_or(u64::MAX) {
        atomic(&cache_path, &json!({"identity":identity,"offset":offset,"goal":goal,"skipping":skipping,"prefix_len":prefix_len,"prefix_hash":prefix_hash}).to_string())?;
    }
    if offset < metadata.len() {
        return Err("Reading native goal history".into());
    }
    Ok(goal)
}

fn copy_number(goal: &mut Value, event: &Value, from: &str, to: &str, divisor: f64) {
    if let Some(number) = event[from].as_f64().filter(|n| n.is_finite() && *n >= 0.0) {
        goal[to] = json!(number / divisor);
    }
}

fn reduce_kimi(goal: &mut Value, event: &Value) {
    if !["", "main"].contains(&string(event, "agentId")) {
        return;
    }
    match string(event, "type") {
        "goal.create"
            if !string(event, "goalId").is_empty()
                && !string(event, "objective").trim().is_empty() =>
        {
            *goal = json!({"id":event["goalId"],"objective":journal::clipped(&event["objective"],12000),
                "status":"active","tokens_used":0,"turns_used":0,"time_used_seconds":0,"source":"kimi_goal_v1"});
            if let Some(criterion) = event.get("completionCriterion") {
                goal["completion_criterion"] = criterion.clone();
            }
        }
        "goal.clear" | "forked" => {
            *goal = Value::Null;
            return;
        }
        "goal.update"
            if goal.is_object()
                && (string(event, "goalId").is_empty() || event["goalId"] == goal["id"]) => {}
        _ => return,
    }
    if ["active", "paused", "blocked", "complete"].contains(&string(event, "status")) {
        goal["status"] = event["status"].clone();
        goal["reason"] = if event["status"] == "active" {
            Value::Null
        } else {
            event["reason"].clone()
        };
    }
    for (from, to, divisor) in [
        ("tokensUsed", "tokens_used", 1.0),
        ("turnsUsed", "turns_used", 1.0),
        ("wallClockMs", "time_used_seconds", 1000.0),
    ] {
        copy_number(goal, event, from, to, divisor);
    }
    if event["budgetLimits"].is_object() {
        for (from, to, divisor) in [
            ("tokenBudget", "token_budget", 1.0),
            ("turnBudget", "turn_budget", 1.0),
            ("wallClockBudgetMs", "time_budget_seconds", 1000.0),
        ] {
            goal.as_object_mut().unwrap().remove(to);
            copy_number(goal, &event["budgetLimits"], from, to, divisor);
        }
    }
    copy_number(goal, event, "time", "updated_at", 1000.0);
    // Only recorded usage: adding wall time here would incorrectly count time
    // while Kimi is closed, paused, or disconnected.
}

// Fold the durable cron journal as Kimi does: add, delete, and the wire wins.
fn reduce_kimi_cron(jobs: &mut Value, event: &Value) {
    if !jobs.is_object() {
        *jobs = json!({});
    }
    let jobs = jobs.as_object_mut().unwrap();
    match string(event, "type") {
        "cron.add" => {
            let task = &event["task"];
            if string(task, "id").is_empty() || !task["cron"].is_string()
                || !task["prompt"].is_string() || !task["createdAt"].is_number()
            {
                return;
            }
            if task["recurring"] == false {
                jobs.remove(string(task, "id"));
            } else {
                jobs.insert(string(task, "id").to_owned(), json!(true));
            }
        }
        "cron.delete" => {
            for id in event["ids"].as_array().into_iter().flatten().filter_map(Value::as_str) {
                jobs.remove(id);
            }
        }
        _ => {}
    }
}

fn reduce_claude(goal: &mut Value, event: &Value) {
    let status = &event["attachment"];
    if string(event, "type") != "attachment"
        || string(status, "type") != "goal_status"
        || string(status, "condition").trim().is_empty()
    {
        return;
    }
    let objective = journal::clipped(&status["condition"], 12000);
    if status["sentinel"] == true && status["met"] == true {
        // Claude uses a successful sentinel for /goal clear, not achievement.
        *goal = Value::Null;
        return;
    }
    if goal["objective"] != objective || status["sentinel"] == true {
        *goal = json!({"id":format!("claude-{:x}",Sha256::digest(objective.as_bytes())),"objective":objective,
            "source":"claude_goal_status_v1","status":"active","evaluations":0});
    }
    goal["status"] = json!(if status["failed"] == true {
        "blocked"
    } else if status["met"] == true {
        "complete"
    } else {
        "active"
    });
    goal["reason"] = status["reason"].clone();
    if status["sentinel"] != true {
        goal["evaluations"] = json!(goal["evaluations"].as_u64().unwrap_or(0) + 1);
    }
    copy_number(goal, status, "iterations", "evaluations", 1.0);
    copy_number(goal, status, "tokens", "tokens_used", 1.0);
    copy_number(goal, status, "durationMs", "time_used_seconds", 1000.0);
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn incremental_history_skips_large_rows_and_detects_rewrites() {
        let temp = tempfile::tempdir().unwrap();
        let path = temp.path().join("wire.jsonl");
        let cache = temp.path().join("cache.json");
        let event = |id: &str| {
            json!({"type":"goal.create","goalId":id,"objective":"Complete work"}).to_string() + "\n"
        };
        fs::write(
            &path,
            format!("{}\n{}", "x".repeat(3 * 1024 * 1024), event("one")),
        )
        .unwrap();
        assert_eq!(
            replay_goal_at(&path, "kimi_goal_v1", &cache).unwrap()["id"],
            "one"
        );
        use std::io::Write;
        let mut append = fs::OpenOptions::new().append(true).open(&path).unwrap();
        append.write_all(b"{\"type\":\"goal.clear\"").unwrap();
        assert!(replay_goal_at(&path, "kimi_goal_v1", &cache).is_err());
        append.write_all(b"}\n").unwrap();
        assert!(replay_goal_at(&path, "kimi_goal_v1", &cache)
            .unwrap()
            .is_null());
        fs::write(
            &path,
            format!("{}{}\n", event("two"), "x".repeat(4 * 1024 * 1024)),
        )
        .unwrap();
        assert_eq!(
            replay_goal_at(&path, "kimi_goal_v1", &cache).unwrap()["id"],
            "two"
        );
    }

    #[test]
    fn kimi_recurring_schedules_follow_the_durable_cron_journal() {
        let temp = tempfile::tempdir().unwrap();
        let path = temp.path().join("wire.jsonl");
        let cache = temp.path().join("cache.json");
        let add = |id: &str, recurring: Value| {
            let mut task = json!({"id":id,"cron":"*/15 * * * *","prompt":"Check the backup","createdAt":1});
            if !recurring.is_null() { task["recurring"] = recurring; }
            json!({"type":"cron.add","task":task}).to_string() + "\n"
        };
        let count = || replay_goal_at(&path, "kimi_cron_v1", &cache).unwrap().as_object().map_or(0, |jobs| jobs.len());
        fs::write(&path, add("watch", Value::Null) + &add("once", json!(false))
            + &json!({"type":"cron.add","task":{"id":"invalid","cron":"* * * * *"}}).to_string() + "\n"
            + &json!({"type":"context.append_message","text":"\"cron.add\" in a message"}).to_string() + "\n").unwrap();
        assert_eq!(count(), 1);
        use std::io::Write;
        let mut append = fs::OpenOptions::new().append(true).open(&path).unwrap();
        append.write_all((json!({"type":"cron.cursor","id":"watch","lastFiredAt":2}).to_string() + "\n"
            + &add("hourly", json!(true))).as_bytes()).unwrap();
        assert_eq!(count(), 2);
        append.write_all((json!({"type":"cron.delete","ids":["watch","hourly"]}).to_string() + "\n").as_bytes()).unwrap();
        assert_eq!(count(), 0);
    }

    #[test]
    fn reads_exact_account_and_conversation_without_changing_lifecycle() {
        let root = tempfile::tempdir().unwrap();
        let path = root.path().join("goals_1.sqlite");
        let a = "12345678-1234-1234-1234-123456789abc";
        let b = "12345678-1234-1234-1234-123456789def";
        let db = Connection::open(&path).unwrap();
        db.execute_batch("CREATE TABLE thread_goals(thread_id TEXT PRIMARY KEY, goal_id TEXT, objective TEXT, status TEXT, token_budget INTEGER, tokens_used INTEGER, time_used_seconds INTEGER, created_at_ms INTEGER, updated_at_ms INTEGER);").unwrap();
        db.execute("INSERT INTO thread_goals VALUES (?1, 'goal-a', 'Finish the migration', 'active', 100000, 13000, 120, 1000, 2000)", [a]).unwrap();
        db.execute("INSERT INTO thread_goals VALUES (?1, 'goal-b', 'Other conversation', 'complete', NULL, 1, 1, 1000, 2000)", [b]).unwrap();
        let record = json!({"agent":"codex","agent_home":root.path(),"conversation_id":a,"activity":"idle","phase":"ready"});
        let result = summary(&record);
        assert_eq!(result["goal"]["id"], "goal-a");
        assert_eq!(result["goal"]["status"], "active");
        assert_eq!(result["goal"]["tokens_used"], 13000);
        assert_eq!(record["activity"], "idle");
        for status in [
            "paused",
            "blocked",
            "usage_limited",
            "budget_limited",
            "complete",
        ] {
            db.execute(
                "UPDATE thread_goals SET status=?1 WHERE thread_id=?2",
                [status, a],
            )
            .unwrap();
            assert_eq!(summary(&record)["goal"]["status"], status);
        }
        db.execute("DELETE FROM thread_goals WHERE thread_id=?1", [a])
            .unwrap();
        assert!(summary(&record)["goal"].is_null());
        assert_eq!(summary(&record)["goal_source"], "codex_goals_v1");
        let other = tempfile::tempdir().unwrap();
        let missing = json!({"agent":"codex","agent_home":other.path(),"conversation_id":b});
        assert_eq!(summary(&missing)["goal_source"], "unavailable");
        assert!(!other.path().join("goals_1.sqlite").exists());
        assert_eq!(
            summary(&json!({"agent":"other"}))["goal_source"],
            "unsupported"
        );
    }

    #[test]
    fn kimi_goal_usage_pause_clear_and_fork_are_native_events() {
        let mut goal = Value::Null;
        reduce_kimi(
            &mut goal,
            &json!({"type":"goal.create","agentId":"main","goalId":"g1","objective":"Finish migration","time":1000}),
        );
        reduce_kimi(
            &mut goal,
            &json!({"type":"goal.update","agentId":"child","status":"complete"}),
        );
        assert_eq!(goal["status"], "active");
        reduce_kimi(
            &mut goal,
            &json!({"type":"goal.update","goalId":"other","status":"complete"}),
        );
        assert_eq!(goal["status"], "active");
        reduce_kimi(
            &mut goal,
            &json!({"type":"goal.update","goalId":"g1","status":"paused","tokensUsed":125,"wallClockMs":2300,"turnsUsed":2,"budgetLimits":{"tokenBudget":1000},"reason":"User paused"}),
        );
        assert_eq!(goal["status"], "paused");
        assert_eq!(goal["tokens_used"], 125.0);
        assert_eq!(goal["time_used_seconds"], 2.3);
        assert_eq!(goal["token_budget"], 1000.0);
        reduce_kimi(&mut goal, &json!({"type":"forked","agentId":"main"}));
        assert!(goal.is_null());
        reduce_kimi(&mut goal, &json!({"type":"goal.update","status":"active"}));
        assert!(goal.is_null());
    }

    #[test]
    fn claude_clear_is_not_achievement_and_missing_metrics_are_unknown() {
        let mut goal = Value::Null;
        let event = |a| json!({"type":"attachment","attachment":a});
        reduce_claude(
            &mut goal,
            &event(
                json!({"type":"goal_status","condition":"Tests pass","sentinel":true,"met":false}),
            ),
        );
        assert_eq!(goal["status"], "active");
        assert!(goal.get("tokens_used").is_none());
        reduce_claude(
            &mut goal,
            &event(
                json!({"type":"goal_status","condition":"Tests pass","met":false,"reason":"Still failing"}),
            ),
        );
        assert_eq!(goal["evaluations"], 1);
        reduce_claude(
            &mut goal,
            &event(
                json!({"type":"goal_status","condition":"Tests pass","met":true,"iterations":2,"durationMs":12300,"tokens":430}),
            ),
        );
        assert_eq!(goal["status"], "complete");
        assert_eq!(goal["evaluations"], 2.0);
        reduce_claude(
            &mut goal,
            &event(
                json!({"type":"goal_status","condition":"Tests pass","met":true,"sentinel":true}),
            ),
        );
        assert!(goal.is_null());
    }

    #[test]
    fn incompatible_or_invalid_native_state_is_unavailable() {
        let root = tempfile::tempdir().unwrap();
        let db = Connection::open(root.path().join("goals_1.sqlite")).unwrap();
        db.execute_batch("CREATE TABLE thread_goals(thread_id TEXT);")
            .unwrap();
        let record = json!({"agent":"codex","agent_home":root.path(),"conversation_id":"12345678-1234-1234-1234-123456789abc"});
        assert_eq!(summary(&record)["goal_source"], "unavailable");
        assert_eq!(
            summary(&json!({"agent":"codex","conversation_id":""}))["goal_source"],
            "unavailable"
        );
    }
}
