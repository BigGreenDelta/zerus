//! Incremental metadata extraction at native hook time, never on dashboard reads.
//! Reads at most 68 KiB + one boundary byte / 64 records per invocation, with a
//! 50 ms processing deadline and nonblocking database writes. OS I/O operations
//! can exceed that deadline. Native text is not
//! an identity, classification signal, stored field or exported diagnostic.
use super::{
    adapters,
    model::*,
    store::{is_rejected_metadata, Store},
};
use anyhow::{ensure, Context, Result};
use chrono::{DateTime, Utc};
use serde_json::Value;
use std::{
    fs::OpenOptions,
    io::{Read, Seek, SeekFrom},
    path::Path,
    time::{Duration, Instant},
};

const BYTES: u64 = 64 * 1024;
const HEADER: u64 = 4 * 1024;
const RECORDS: usize = 64;
fn text<'a>(v: &'a Value, key: &str) -> Option<&'a str> {
    v[key]
        .as_str()
        .filter(|s| !s.is_empty() && s.len() <= 256 && !s.chars().any(char::is_control))
}
fn at(row: &Value) -> Option<i64> {
    DateTime::parse_from_rfc3339(text(row, "timestamp")?)
        .ok()
        .map(|t| t.timestamp_millis())
}
// A native timestamp plus exact typed numeric observation distinguishes updates
// even when the provider emits several snapshots within one millisecond. No text
// or source path participates in this composite metadata identity.
fn usage_id(prefix: &str, row: &Value, counters: &Tokens) -> String {
    format!(
        "{}:{}:{:?}",
        prefix,
        text(row, "timestamp").unwrap_or(""),
        counters.values()
    )
}
fn number(v: &Value) -> Option<u64> {
    v.as_u64().filter(|n| *n <= MAX_VALUE)
}
fn sum(a: Option<u64>, b: Option<u64>) -> Option<u64> {
    a?.checked_add(b?).filter(|n| *n <= MAX_VALUE)
}
fn event(
    record: &Value,
    id: &str,
    at: i64,
    kind: Kind,
    correlation: Option<&str>,
    _hook: &str,
) -> Option<Event> {
    let provider = Provider::parse(text(record, "agent")?);
    let conversation = text(record, "conversation_id")?;
    Some(Event {
        id: Identity::new(&["native", provider.name(), conversation, id]),
        scope: Identity::new(&[provider.name(), conversation, ""]),
        provider,
        producer: Producer::Main,
        correlation: correlation.map(|id| Identity::new(&[provider.name(), conversation, "", id])),
        observed_ms: at,
        clock: None,
        kind,
    })
}

fn live_clock(record: &Value, hook: &Value, event: &Event, now: i64) -> Option<Clock> {
    let name = text(hook, "hook_event_name")?;
    let boundary = match event.kind {
        Kind::TurnStarted => matches!(name, "UserPromptSubmit" | "TurnStarted"),
        Kind::TurnCompleted | Kind::TurnInterrupted => matches!(name, "Stop" | "Interrupt"),
        _ => false,
    };
    if !boundary
        || (now - event.observed_ms).abs() > 2000
        || text(hook, "agent_id").is_some_and(|id| id != "main")
    {
        return None;
    }
    let turn = text(hook, "turn_id")?;
    let native = Identity::new(&[
        event.provider.name(),
        text(record, "conversation_id")?,
        "",
        turn,
    ]);
    if event.correlation.as_ref()?.0 != native.0 {
        return None;
    }
    adapters::clock(record)
}

fn rows(record: &Value, row: &Value, hook: &str) -> Vec<Event> {
    let Some(at) = at(row) else { return vec![] };
    let provider = text(record, "agent").unwrap_or("");
    let mut events = Vec::new();
    if provider == "codex" && row["type"] == "event_msg" {
        let p = &row["payload"];
        let Some(kind) = text(p, "type") else {
            return events;
        };
        if let Some(turn) = text(p, "turn_id") {
            let typed = match kind {
                "task_started" | "turn_started" => Some(("started", Kind::TurnStarted)),
                "task_complete" | "turn_complete" if p["error"].is_null() => {
                    Some(("completed", Kind::TurnCompleted))
                }
                "turn_aborted" if p["reason"] == "interrupted" => {
                    Some(("interrupted", Kind::TurnInterrupted))
                }
                "task_complete" | "turn_complete" if p["error"].is_object() => {
                    Some(("failure", Kind::Failure(Failure::Unknown)))
                }
                _ => None,
            };
            if let Some((label, kind)) = typed {
                if let Some(event) = event(
                    record,
                    &format!("turn:{turn}:{label}"),
                    at,
                    kind,
                    Some(turn),
                    hook,
                ) {
                    events.push(event);
                }
            }
        }
        if kind == "token_count" {
            let usage = &p["info"]["total_token_usage"];
            let counters = Tokens {
                input_tokens: number(&usage["input_tokens"]),
                output_tokens: number(&usage["output_tokens"]),
                cache_tokens: number(&usage["cached_input_tokens"]),
                reasoning_tokens: number(&usage["reasoning_output_tokens"]),
            };
            if counters.values().iter().any(|(_, v)| v.is_some()) {
                let epoch = Identity::new(&[
                    "codex-conversation-totals-v1",
                    text(record, "conversation_id").unwrap_or(""),
                ]);
                if let Some(e) = event(
                    record,
                    &usage_id("usage", row, &counters),
                    at,
                    Kind::Usage { epoch, counters },
                    None,
                    hook,
                ) {
                    events.push(e);
                }
            }
        }
    } else if provider == "claude"
        && row["type"] == "assistant"
        && row["sessionId"] == record["conversation_id"]
    {
        let (Some(uuid), Some(id)) = (text(row, "uuid"), text(&row["message"], "id")) else {
            return events;
        };
        let usage = &row["message"]["usage"];
        let cache = sum(
            number(&usage["cache_read_input_tokens"]),
            number(&usage["cache_creation_input_tokens"]),
        );
        let counters = Tokens {
            input_tokens: sum(number(&usage["input_tokens"]), cache),
            output_tokens: number(&usage["output_tokens"]),
            cache_tokens: cache,
            reasoning_tokens: number(&usage["output_tokens_details"]["thinking_tokens"]),
        };
        if counters.values().iter().any(|(_, v)| v.is_some()) {
            if let Some(mut e) = event(
                record,
                &usage_id(&format!("request:{uuid}"), row, &counters),
                at,
                Kind::RequestUsage { counters },
                None,
                hook,
            ) {
                e.scope = Identity::new(&[
                    "claude",
                    text(record, "conversation_id").unwrap_or(""),
                    "request",
                    id,
                ]);
                events.push(e);
            }
        }
        if row["message"]["stop_reason"] == "end_turn" {
            if let Some(e) = event(
                record,
                &format!("completed:{id}"),
                at,
                Kind::TurnCompleted,
                None,
                hook,
            ) {
                events.push(e);
            }
        }
    }
    events
}

fn read(db: &mut Store, record: &Value, hook: &Value) -> Result<()> {
    // Setup and metadata reads consume the same budget as event processing.
    let deadline = Instant::now() + Duration::from_millis(50);
    read_with_budget(db, record, hook, || Instant::now() >= deadline)
}

fn read_with_budget(
    db: &mut Store,
    record: &Value,
    hook: &Value,
    mut exhausted: impl FnMut() -> bool,
) -> Result<()> {
    db.nonblocking_observer()?;
    if !db.enabled()? {
        return Ok(());
    }
    let provider = text(record, "agent").context("Unknown native provider")?;
    if !matches!(provider, "codex" | "claude") {
        return Ok(());
    }
    let conversation = text(record, "conversation_id").context("Missing native conversation")?;
    let id = uuid::Uuid::parse_str(conversation)?;
    ensure!(!id.is_nil(), "Invalid native conversation");
    let path = Path::new(
        record["transcript"]
            .as_str()
            .context("Native metadata unavailable")?,
    );
    ensure!(
        path.is_absolute()
            && path
                .file_name()
                .and_then(|s| s.to_str())
                .is_some_and(|s| s.contains(conversation)),
        "Native metadata identity mismatch"
    );
    let mut file = crate::platform::refuse_link(OpenOptions::new().read(true)).open(path)?;
    let (identity, meta) = crate::platform::FileIdentity::with_metadata(&file)?;
    ensure!(meta.is_file(), "Native metadata must be a file");
    let mut prefix = Vec::new();
    (&mut file)
        .take(HEADER.min(meta.len()))
        .read_to_end(&mut prefix)?;
    let line = prefix.split(|b| *b == b'\n').next().unwrap_or(&[]);
    if provider == "codex" {
        let header: Value = serde_json::from_slice(line)?;
        ensure!(
            header["type"] == "session_meta" && header["payload"]["id"] == conversation,
            "Native metadata identity mismatch"
        );
    }
    let fingerprint = Identity::new(&[&identity.device.to_string(), &identity.inode.to_string()]);
    let key = Identity::new(&[provider, conversation]);
    let now = Utc::now().timestamp_millis();
    let Some(mut db) = db.native_batch(&key, now)? else {
        return Ok(());
    };
    let mut offset = match db.cursor(&key)? {
        Some((old, offset)) if old.0 == fingerprint.0 && offset <= meta.len() => offset,
        _ => {
            db.note_gap(Gap::Observation, now)?;
            meta.len().saturating_sub(BYTES)
        }
    };
    if offset == meta.len() {
        return db.commit();
    }
    // If a previous bounded pass stopped inside an oversized line, skip its
    // remainder. No unbounded allocation or repeated scan from its start.
    let mut boundary = true;
    if offset > 0 {
        file.seek(SeekFrom::Start(offset - 1))?;
        let mut prior = [0];
        file.read_exact(&mut prior)?;
        boundary = prior[0] == b'\n';
    }
    file.seek(SeekFrom::Start(offset))?;
    let mut bytes = Vec::new();
    (&mut file).take(BYTES).read_to_end(&mut bytes)?;
    'lines: for (count, line) in bytes.split_inclusive(|b| *b == b'\n').enumerate() {
        if count >= RECORDS || exhausted() {
            db.note_gap(Gap::Observation, now)?;
            break;
        }
        if !line.ends_with(b"\n") {
            if offset + (line.len() as u64) < meta.len() {
                offset += line.len() as u64;
                db.note_gap(Gap::Observation, now)?;
            }
            break;
        }
        if !boundary {
            offset += line.len() as u64;
            boundary = true;
            db.note_gap(Gap::Observation, now)?;
            continue;
        }
        let Ok(row) = serde_json::from_slice::<Value>(line) else {
            offset += line.len() as u64;
            db.note_gap(Gap::Observation, now)?;
            continue;
        };
        for mut e in rows(record, &row, text(hook, "hook_event_name").unwrap_or("")) {
            e.clock = live_clock(record, hook, &e, now);
            if exhausted() {
                db.note_gap(Gap::Observation, now)?;
                // Replay this line, including any already accepted event IDs.
                break 'lines;
            }
            // A contradictory native metadata row must not pin all future reads.
            // Only a typed metadata rejection is skippable. Storage and unknown
            // failures roll back the pass, including already accepted events, so
            // a later independent pass can replay from its original cursor.
            match db.record(&e, now) {
                Ok(_) => (),
                Err(error) if is_rejected_metadata(&error) => db.note_gap(Gap::Observation, now)?,
                Err(error) => return Err(error),
            }
        }
        offset += line.len() as u64;
    }
    db.save_cursor(&key, &fingerprint, offset, now)?;
    db.commit()
}

pub(crate) fn collect(root: &Path, record: &Value, hook: &Value) {
    // OFF performs no source reads and creates no analytics storage.
    let Ok(Some(mut db)) = Store::open_observer(&root.join("analytics")) else {
        return;
    };
    if read(&mut db, record, hook).is_err() {
        let _ = db.note_gap(Gap::Observation, Utc::now().timestamp_millis());
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;
    const ID: &str = "22222222-2222-4222-8222-222222222222";
    #[test]
    fn only_typed_codex_turns_and_cumulative_values_are_selected() {
        let record = json!({"agent":"codex","conversation_id":ID});
        let row = json!({"type":"event_msg","timestamp":"2026-10-18T12:00:00Z","payload":{"type":"task_complete","turn_id":"turn","last_agent_message":"SECRET_TEXT"}});
        let e = rows(&record, &row, "Stop");
        assert_eq!(e.len(), 1);
        assert!(matches!(e[0].kind, Kind::TurnCompleted));
        assert!(e[0].clock.is_none());
        let missing = json!({"type":"event_msg","timestamp":"2026-10-18T12:00:00Z","payload":{"type":"task_complete","last_agent_message":"completed"}});
        assert!(rows(&record, &missing, "Stop").is_empty());
        let prose = json!({"type":"response_item","timestamp":"2026-10-18T12:00:00Z","payload":{"type":"message","role":"assistant","content":"turn completed 999 tokens"}});
        assert!(rows(&record, &prose, "Stop").is_empty());
    }
    #[test]
    fn claude_request_identity_and_scope_are_required_not_text() {
        let record = json!({"agent":"claude","conversation_id":ID});
        let row = json!({"type":"assistant","timestamp":"2026-10-18T12:00:00Z","sessionId":ID,"uuid":"native-row","message":{"id":"request","content":"SECRET","usage":{"input_tokens":3,"cache_read_input_tokens":5,"cache_creation_input_tokens":2,"output_tokens":7},"stop_reason":"end_turn"}});
        let e = rows(&record, &row, "Stop");
        assert_eq!(e.len(), 2);
        let Kind::RequestUsage { counters } = &e[0].kind else {
            panic!("request usage")
        };
        assert_eq!(counters.input_tokens, Some(10));
        assert_eq!(counters.cache_tokens, Some(7));
        let mut wrong = row;
        wrong["sessionId"] = json!("foreign");
        assert!(rows(&record, &wrong, "Stop").is_empty());
    }

    #[test]
    fn time_proximity_without_exact_main_turn_proof_never_measures_work() {
        let now = Utc::now().timestamp_millis();
        let record = json!({"agent":"codex","conversation_id":ID,"run_id":"run","process_start":"process-generation"});
        let e = event(
            &record,
            "start",
            now,
            Kind::TurnStarted,
            Some("exact-turn"),
            "TurnStarted",
        )
        .unwrap();
        assert!(live_clock(&record, &json!({"hook_event_name":"TurnStarted"}), &e, now).is_none());
        assert!(live_clock(
            &record,
            &json!({"hook_event_name":"TurnStarted","turn_id":"other-turn"}),
            &e,
            now
        )
        .is_none());
        assert!(live_clock(
            &record,
            &json!({"hook_event_name":"TurnStarted","turn_id":"exact-turn","agent_id":"child"}),
            &e,
            now
        )
        .is_none());
        #[cfg(unix)]
        assert!(live_clock(
            &record,
            &json!({"hook_event_name":"TurnStarted","turn_id":"exact-turn"}),
            &e,
            now
        )
        .is_some());
    }
    fn fixture() -> (tempfile::TempDir, Store, Value, i64) {
        let dir = tempfile::tempdir().unwrap();
        let mut db = Store::open(&dir.path().join("analytics"), true, true)
            .unwrap()
            .unwrap();
        let now = Utc::now().timestamp_millis();
        db.configure(super::super::store::NIL_EPOCH, true, 90, now - 10_000)
            .unwrap();
        let path = dir.path().join(format!("rollout-{ID}.jsonl"));
        let record = json!({"agent":"codex","conversation_id":ID,"transcript":path});
        let header = json!({"type":"session_meta","payload":{"id":ID}});
        std::fs::write(path, format!("{header}\n")).unwrap();
        (dir, db, record, now - 5000)
    }
    fn append(record: &Value, row: &Value) {
        use std::io::Write;
        let mut f = OpenOptions::new()
            .append(true)
            .open(record["transcript"].as_str().unwrap())
            .unwrap();
        writeln!(f, "{row}").unwrap();
    }
    fn usage(at: i64, input: u64) -> Value {
        json!({"type":"event_msg","timestamp":DateTime::from_timestamp_millis(at).unwrap().to_rfc3339(),
            "payload":{"type":"token_count","info":{"total_token_usage":{"input_tokens":input,"output_tokens":1}}}})
    }
    fn drain(db: &mut Store, record: &Value) {
        let key = Identity::new(&["codex", ID]);
        let length = std::fs::metadata(record["transcript"].as_str().unwrap())
            .unwrap()
            .len();
        for _ in 0..64 {
            read(db, record, &json!({"hook_event_name":"Stop"})).unwrap();
            if db
                .cursor(&key)
                .unwrap()
                .is_some_and(|(_, offset)| offset == length)
            {
                return;
            }
        }
        panic!("bounded passes did not reach the synthetic stream end");
    }
    fn total(db: &Store, key: &str) -> u64 {
        let today = Utc::now().date_naive();
        let s = db
            .summary(
                "11111111-1111-4111-8111-111111111111",
                &(today - chrono::Duration::days(1)).to_string(),
                &today.to_string(),
                Utc::now().timestamp_millis(),
            )
            .unwrap();
        s["buckets"]
            .as_array()
            .unwrap()
            .iter()
            .filter_map(|b| b["counters"][key].as_u64())
            .sum()
    }
    #[test]
    fn same_millisecond_updates_advance_and_replay_without_duplicate_usage() {
        let (_dir, mut db, record, at) = fixture();
        assert_ne!(
            rows(&record, &usage(at, 10), "Stop")[0].id.0,
            rows(&record, &usage(at, 12), "Stop")[0].id.0
        );
        append(&record, &usage(at, 10));
        append(&record, &usage(at, 12));
        append(&record, &usage(at + 1, 15));
        drain(&mut db, &record);
        assert_eq!(total(&db, "input_tokens"), 5);
        let key = Identity::new(&["codex", ID]);
        let (fingerprint, _) = db.cursor(&key).unwrap().unwrap();
        db.save_cursor(&key, &fingerprint, 0, Utc::now().timestamp_millis())
            .unwrap();
        drain(&mut db, &record);
        assert_eq!(total(&db, "input_tokens"), 5);
    }
    #[test]
    fn conflicting_lifecycle_metadata_cannot_pin_following_rows() {
        let (_dir, mut db, record, at) = fixture();
        let turn = |t| {
            json!({"type":"event_msg","timestamp":DateTime::from_timestamp_millis(t).unwrap().to_rfc3339(),
            "payload":{"type":"task_complete","turn_id":"same-turn"}})
        };
        append(&record, &turn(at));
        append(&record, &turn(at + 1));
        append(&record, &usage(at + 2, 10));
        append(&record, &usage(at + 3, 13));
        drain(&mut db, &record);
        assert_eq!(total(&db, "turn_completed"), 1);
        assert_eq!(total(&db, "input_tokens"), 3);
    }
    #[test]
    fn partial_records_wait_for_newline_and_replaced_sources_replay_safely() {
        use std::io::Write;
        let (_dir, mut db, record, at) = fixture();
        append(&record, &usage(at, 10));
        drain(&mut db, &record);
        let path = Path::new(record["transcript"].as_str().unwrap());
        let row = usage(at + 1, 14).to_string();
        let mut f = OpenOptions::new().append(true).open(path).unwrap();
        write!(f, "{row}").unwrap();
        read(&mut db, &record, &json!({"hook_event_name":"Stop"})).unwrap();
        assert_eq!(total(&db, "input_tokens"), 0);
        writeln!(f).unwrap();
        drop(f);
        drain(&mut db, &record);
        assert_eq!(total(&db, "input_tokens"), 4);
        let replacement = path.with_extension("replacement");
        std::fs::copy(path, &replacement).unwrap();
        std::fs::rename(replacement, path).unwrap();
        drain(&mut db, &record);
        assert_eq!(total(&db, "input_tokens"), 4);
        std::fs::write(
            path,
            format!("{}\n", json!({"type":"session_meta","payload":{"id":ID}})),
        )
        .unwrap();
        append(&record, &usage(at + 2, 16));
        drain(&mut db, &record);
        assert_eq!(total(&db, "input_tokens"), 6);
    }
    #[test]
    fn disabled_collection_never_opens_or_validates_a_native_source() {
        let (_dir, mut db, mut record, _) = fixture();
        let today = Utc::now().date_naive().to_string();
        let now = Utc::now().timestamp_millis();
        let s = db.summary(ID, &today, &today, now).unwrap();
        db.configure(s["source_epoch"].as_str().unwrap(), false, 90, now)
            .unwrap();
        record["transcript"] = json!("/nonexistent/foreign-private-source");
        assert!(read(&mut db, &record, &json!({"hook_event_name":"Stop"})).is_ok());
        assert!(db.cursor(&Identity::new(&["codex", ID])).unwrap().is_none());
    }
    #[test]
    fn deadline_before_event_keeps_line_for_the_next_pass() {
        let (_dir, mut db, record, at) = fixture();
        drain(&mut db, &record);
        let key = Identity::new(&["codex", ID]);
        let before = db.cursor(&key).unwrap().unwrap().1;
        let row = json!({"type":"event_msg","timestamp":DateTime::from_timestamp_millis(at).unwrap().to_rfc3339(),
            "payload":{"type":"task_complete","turn_id":"budget-turn"}});
        append(&record, &row);
        let mut checks = 0;
        read_with_budget(&mut db, &record, &json!({"hook_event_name":"Stop"}), || {
            checks += 1;
            checks >= 2
        })
        .unwrap();
        assert_eq!(total(&db, "turn_completed"), 0);
        assert_eq!(
            db.cursor(&key).unwrap().unwrap().1,
            before,
            "deadline must not consume an unattempted event"
        );
        drain(&mut db, &record);
        assert_eq!(total(&db, "turn_completed"), 1);
    }

    #[test]
    fn deadline_between_events_replays_the_unfinished_line_exactly_once() {
        let (_dir, mut db, mut record, at) = fixture();
        record["agent"] = json!("claude");
        let key = Identity::new(&["claude", ID]);
        read(&mut db, &record, &json!({"hook_event_name":"Stop"})).unwrap();
        let before = db.cursor(&key).unwrap().unwrap().1;
        let row = json!({"type":"assistant","timestamp":DateTime::from_timestamp_millis(at).unwrap().to_rfc3339(),
            "sessionId":ID,"uuid":"row-budget","message":{"id":"request-budget",
            "usage":{"input_tokens":3,"cache_read_input_tokens":0,"cache_creation_input_tokens":0,"output_tokens":7},
            "stop_reason":"end_turn"}});
        append(&record, &row);
        let mut checks = 0;
        read_with_budget(&mut db, &record, &json!({"hook_event_name":"Stop"}), || {
            checks += 1;
            checks >= 3
        })
        .unwrap();
        assert_eq!(total(&db, "input_tokens"), 3);
        assert_eq!(total(&db, "turn_completed"), 0);
        assert_eq!(
            db.cursor(&key).unwrap().unwrap().1,
            before,
            "deadline must leave the partially attempted line replayable"
        );
        read_with_budget(&mut db, &record, &json!({"hook_event_name":"Stop"}), || {
            false
        })
        .unwrap();
        read_with_budget(&mut db, &record, &json!({"hook_event_name":"Stop"}), || {
            false
        })
        .unwrap();
        assert_eq!(total(&db, "input_tokens"), 3);
        assert_eq!(total(&db, "turn_completed"), 1);
    }

    #[test]
    fn expired_budget_and_record_limit_keep_exact_cursor_boundaries() {
        let (_dir, mut db, record, at) = fixture();
        drain(&mut db, &record);
        let key = Identity::new(&["codex", ID]);
        let before = db.cursor(&key).unwrap().unwrap().1;
        for i in 0..100 {
            append(&record, &usage(at + i, 10 + i as u64));
        }
        let hook = json!({"hook_event_name":"Stop"});
        read_with_budget(&mut db, &record, &hook, || true).unwrap();
        assert_eq!(db.cursor(&key).unwrap().unwrap().1, before);
        assert_eq!(total(&db, "input_tokens"), 0);
        read_with_budget(&mut db, &record, &hook, || false).unwrap();
        let expected = before
            + (0..64)
                .map(|i| usage(at + i, 10 + i as u64).to_string().len() as u64 + 1)
                .sum::<u64>();
        assert_eq!(db.cursor(&key).unwrap().unwrap().1, expected);
        assert_eq!(total(&db, "input_tokens"), 63);
        drain(&mut db, &record);
        assert_eq!(total(&db, "input_tokens"), 99);
    }

    #[test]
    fn cursor_write_failure_rolls_back_accepted_events_and_allows_replay() {
        let (dir, mut db, record, at) = fixture();
        drain(&mut db, &record);
        let key = Identity::new(&["codex", ID]);
        let before = db.cursor(&key).unwrap().unwrap().1;
        let raw =
            rusqlite::Connection::open(dir.path().join("analytics/analytics.sqlite3")).unwrap();
        raw.execute_batch("CREATE TRIGGER reject_cursor BEFORE UPDATE ON cursors BEGIN SELECT RAISE(ABORT,'synthetic cursor failure'); END;").unwrap();
        append(
            &record,
            &json!({"type":"event_msg","timestamp":DateTime::from_timestamp_millis(at).unwrap().to_rfc3339(),
            "payload":{"type":"task_complete","turn_id":"cursor-failure-turn"}}),
        );
        assert!(
            read_with_budget(&mut db, &record, &json!({"hook_event_name":"Stop"}), || {
                false
            })
            .is_err()
        );
        assert_eq!(db.cursor(&key).unwrap().unwrap().1, before);
        assert_eq!(
            total(&db, "turn_completed"),
            0,
            "event and cursor must commit atomically"
        );
        raw.execute_batch("DROP TRIGGER reject_cursor;").unwrap();
        drain(&mut db, &record);
        assert_eq!(total(&db, "turn_completed"), 1);
    }

    #[test]
    fn second_event_sql_failure_rolls_back_the_pass_and_replays() {
        let (dir, mut db, record, at) = fixture();
        drain(&mut db, &record);
        let key = Identity::new(&["codex", ID]);
        let before = db.cursor(&key).unwrap().unwrap().1;
        for turn in ["first-valid", "second-valid"] {
            append(
                &record,
                &json!({"type":"event_msg","timestamp":DateTime::from_timestamp_millis(at).unwrap().to_rfc3339(),
                "payload":{"type":"task_complete","turn_id":turn}}),
            );
        }
        let raw =
            rusqlite::Connection::open(dir.path().join("analytics/analytics.sqlite3")).unwrap();
        // The message deliberately resembles a semantic rejection. Classification
        // must use the error type, never text from a SQLite error or native row.
        raw.execute_batch("CREATE TRIGGER reject_second_event BEFORE INSERT ON events WHEN (SELECT count(*) FROM events)=1 BEGIN SELECT RAISE(ABORT,'Conflicting analytics event identity'); END;").unwrap();
        let hook = json!({"hook_event_name":"Stop"});
        let failed = read_with_budget(&mut db, &record, &hook, || false);
        assert_eq!(
            (
                failed.is_err(),
                total(&db, "turn_completed"),
                db.cursor(&key).unwrap().unwrap().1
            ),
            (true, 0, before),
            "SQL failure must roll back the prior accepted event and cursor"
        );
        raw.execute_batch("DROP TRIGGER reject_second_event;")
            .unwrap();
        read_with_budget(&mut db, &record, &hook, || false).unwrap();
        assert_eq!(total(&db, "turn_completed"), 2);
        assert_eq!(
            db.cursor(&key).unwrap().unwrap().1,
            std::fs::metadata(record["transcript"].as_str().unwrap())
                .unwrap()
                .len()
        );
        read_with_budget(&mut db, &record, &hook, || false).unwrap();
        assert_eq!(total(&db, "turn_completed"), 2);
    }

    #[test]
    fn stream_identity_and_byte_record_bounds_are_enforced_without_storing_text() {
        use std::io::Write;
        let (dir, mut db, record, at) = fixture();
        let path = Path::new(record["transcript"].as_str().unwrap());
        let mut f = OpenOptions::new().append(true).open(path).unwrap();
        writeln!(
            f,
            "{}",
            json!({"type":"response_item","text":"PRIVATE_SENTINEL".repeat(8000)})
        )
        .unwrap();
        drop(f);
        for i in 0..100 {
            append(&record, &usage(at + i, 10 + i as u64));
        }
        let (identity, _) =
            crate::platform::FileIdentity::with_metadata(&std::fs::File::open(path).unwrap())
                .unwrap();
        let fingerprint =
            Identity::new(&[&identity.device.to_string(), &identity.inode.to_string()]);
        let key = Identity::new(&["codex", ID]);
        db.save_cursor(&key, &fingerprint, 0, Utc::now().timestamp_millis())
            .unwrap();
        read(&mut db, &record, &json!({"hook_event_name":"Stop"})).unwrap();
        assert!(db.cursor(&key).unwrap().unwrap().1 <= BYTES);
        drain(&mut db, &record);
        assert_eq!(total(&db, "input_tokens"), 99);
        let bytes = std::fs::read(dir.path().join("analytics/analytics.sqlite3")).unwrap();
        assert!(!bytes
            .windows(b"PRIVATE_SENTINEL".len())
            .any(|w| w == b"PRIVATE_SENTINEL"));
        std::fs::write(
            path,
            format!(
                "{}\n",
                json!({"type":"session_meta","payload":{"id":"foreign"}})
            ),
        )
        .unwrap();
        assert!(read(&mut db, &record, &json!({"hook_event_name":"Stop"})).is_err());
    }
}
