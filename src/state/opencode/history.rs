//! OpenCode 2 history: `/message` cursor pages read newest first, queued
//! inbox items, and the Activity events both become.
use super::super::*;
#[cfg(test)]
use super::service::Response;
use super::{catalog::encode, native_id, session_path, Native};
use sha2::{Digest, Sha256};
use std::io::Read;
use std::time::{Duration, Instant};

/// The newest window of a conversation, oldest first.
pub(super) struct Page {
    pub messages: Vec<Value>,
    pub truncated: bool,
}

#[derive(Clone, Copy)]
struct Bounds {
    messages: usize,
    bytes: usize,
}
impl Default for Bounds {
    fn default() -> Self {
        Bounds {
            messages: 10_000,
            bytes: 16 * 1024 * 1024,
        }
    }
}
const PAGE: usize = 100;
const INBOX: usize = 256;
const CURSOR: usize = 4096;
const TEXT: usize = 32000;

/// Messages read earlier, oldest first. Only messages older than the oldest
/// message of a freshly read page are ever taken from it: messages change in
/// place while OpenCode streams them.
struct Cache {
    messages: Vec<Value>,
    truncated: bool,
}

fn cache_path(record: &Value) -> PathBuf {
    root().join("opencode/history").join(format!(
        "{:x}.json",
        Sha256::digest(string(record, "conversation_id").as_bytes())
    ))
}
fn load_at(path: &Path, record: &Value) -> Option<Cache> {
    let file = std::fs::File::open(path).ok()?;
    let limit = Bounds::default().bytes as u64 + 1024 * 1024;
    let mut bytes = Vec::new();
    file.take(limit + 1).read_to_end(&mut bytes).ok()?;
    if bytes.len() as u64 > limit {
        return None;
    }
    let value: Value = serde_json::from_slice(&bytes).ok()?;
    if value["conversation_id"] != record["conversation_id"] {
        return None;
    }
    let messages = value["messages"].as_array()?;
    if messages.iter().any(|m| !valid(m)) {
        return None;
    }
    Some(Cache {
        messages: messages.clone(),
        truncated: value["truncated"] != false,
    })
}
/// Best effort: the cache only saves reading older pages again.
fn save_at(path: &Path, record: &Value, page: &Page) {
    let value = json!({"conversation_id": record["conversation_id"], "truncated": page.truncated, "messages": page.messages});
    let _ = atomic(path, &value.to_string());
}

fn valid(message: &Value) -> bool {
    message["id"]
        .as_str()
        .is_some_and(|id| native_id(id, "msg_"))
}

/// Reads the newest window of `/message` with the cache, within 20 seconds.
pub(super) fn fetch(native: &dyn Native, record: &Value) -> Result<Page> {
    fetch_at(native, record, &cache_path(record))
}
pub(super) fn fetch_at(native: &dyn Native, record: &Value, cache: &Path) -> Result<Page> {
    let cached = load_at(cache, record);
    let deadline = Instant::now() + Duration::from_secs(20);
    let (page, changed) = scan(native, record, cached.as_ref(), Bounds::default(), deadline)?;
    if changed {
        save_at(cache, record, &page);
    }
    Ok(page)
}

/// Pages newest first until the end, a budget or a page that reaches back
/// into the cache. Returns the window and whether the cache should be saved.
fn scan(
    native: &dyn Native,
    record: &Value,
    cached: Option<&Cache>,
    bounds: Bounds,
    deadline: Instant,
) -> Result<(Page, bool)> {
    let mut fresh: Vec<Value> = Vec::new();
    let mut seen = std::collections::HashSet::new();
    let mut bytes = 0usize;
    let mut cursor: Option<String> = None;
    let mut truncated = false;
    let mut older: Option<&Cache> = None;
    let mut splice = 0;
    'pages: loop {
        let query = match &cursor {
            None => format!("/message?limit={PAGE}&order=desc"),
            Some(next) => format!("/message?limit={PAGE}&cursor={}", encode(next)),
        };
        let response = match native.raw_until("GET", &session_path(record, &query), None, deadline)
        {
            Ok(response) => response,
            // A later page that runs out of time leaves the newest window.
            Err(_) if cursor.is_some() && Instant::now() >= deadline => {
                truncated = true;
                break;
            }
            Err(error) => return Err(error),
        };
        if !(200..300).contains(&response.status) {
            return Err(format!(
                "OpenCode history could not be read (HTTP {})",
                response.status
            ));
        }
        let data = response.body["data"]
            .as_array()
            .ok_or("Invalid OpenCode history")?;
        let mut added = false;
        for message in data {
            if !valid(message) || !seen.insert(string(message, "id").to_owned()) {
                continue;
            }
            let size = message.to_string().len();
            if fresh.len() >= bounds.messages || bytes.saturating_add(size) > bounds.bytes {
                truncated = true;
                break 'pages;
            }
            bytes += size;
            fresh.push(message.clone());
            added = true;
        }
        if let (Some(cache), Some(oldest)) = (cached, fresh.last()) {
            if let Some(index) = cache.messages.iter().position(|m| m["id"] == oldest["id"]) {
                older = Some(cache);
                splice = index;
                break;
            }
        }
        let Some(next) = response.body["cursor"]["next"].as_str() else {
            break;
        };
        if data.is_empty() {
            break;
        }
        if !added || next.len() > CURSOR || Instant::now() >= deadline {
            truncated = true;
            break;
        }
        cursor = Some(next.to_owned());
    }
    fresh.reverse();
    let mut messages = match older {
        Some(cache) => {
            truncated |= cache.truncated;
            let mut messages: Vec<Value> = cache.messages[..splice]
                .iter()
                .filter(|m| !seen.contains(string(m, "id")))
                .cloned()
                .collect();
            messages.extend(fresh);
            messages
        }
        None => fresh,
    };
    if older.is_some() && trim(&mut messages, bounds) {
        truncated = true;
    }
    let changed = cached.is_none_or(|cache| {
        cache.truncated != truncated
            || cache.messages.len() != messages.len()
            || cache.messages.last().map(|m| &m["id"]) != messages.last().map(|m| &m["id"])
    });
    Ok((
        Page {
            messages,
            truncated,
        },
        changed,
    ))
}

/// Drops the oldest messages beyond the bounds; true when any were dropped.
fn trim(messages: &mut Vec<Value>, bounds: Bounds) -> bool {
    let mut bytes = 0usize;
    let mut keep = 0;
    for message in messages.iter().rev() {
        let size = message.to_string().len();
        if keep >= bounds.messages || bytes.saturating_add(size) > bounds.bytes {
            break;
        }
        bytes += size;
        keep += 1;
    }
    let drop = messages.len() - keep;
    messages.drain(..drop);
    drop > 0
}

fn seconds(value: &Value) -> f64 {
    value["time"]["created"].as_f64().unwrap_or(0.0) / 1000.0
}
fn event(kind: &str, detail: String, tool: &str, at: f64, id: String, reply: Value) -> Value {
    json!({"type":kind,"detail":detail,"tool":tool,"at":at,"source":"opencode_native","agent_id":"","message_id":id,"reply_id":reply})
}
/// Text of a value that may be a string or a JSON structure, clipped.
fn shown(value: &Value) -> String {
    if value.is_string() {
        journal::clipped(value, TEXT)
    } else if value.is_null() {
        String::new()
    } else {
        journal::clipped(&json!(value.to_string()), TEXT)
    }
}
/// A failure's message; never its raw provider response.
fn error_text(error: &Value) -> String {
    let message = error["message"]
        .as_str()
        .or(error.as_str())
        .or(error["type"].as_str())
        .unwrap_or("OpenCode turn failed");
    journal::clipped(&json!(message), TEXT)
}
fn part_id(part: &Value, index: usize) -> String {
    part["id"]
        .as_str()
        .filter(|id| {
            !id.is_empty()
                && id.len() <= 128
                && id
                    .bytes()
                    .all(|c| c.is_ascii_alphanumeric() || c == b'_' || c == b'-')
        })
        .map_or_else(|| index.to_string(), str::to_owned)
}

/// Activity events for chronological messages. Other message and part types
/// (compaction, reasoning, model switches) are not shown.
pub(super) fn rows(messages: &[Value]) -> Vec<Value> {
    let mut rows = Vec::new();
    for message in messages {
        let id = string(message, "id");
        let at = seconds(message);
        match string(message, "type") {
            "user" => rows.push(event(
                "UserPromptSubmit",
                string(message, "text").to_owned(),
                "",
                at,
                id.to_owned(),
                Value::Null,
            )),
            "assistant" => {
                let first = rows.len();
                let completed = message["time"]["completed"].as_f64();
                let mut reply_at = None;
                let mut texts = Vec::new();
                for (index, part) in message["content"]
                    .as_array()
                    .into_iter()
                    .flatten()
                    .enumerate()
                {
                    match string(part, "type") {
                        "text" => {
                            reply_at.get_or_insert(rows.len());
                            texts.push(string(part, "text"));
                        }
                        "tool" => {
                            let call = format!("{id}:{}", part_id(part, index));
                            let name = string(part, "name");
                            let state = &part["state"];
                            rows.push(event(
                                "PreToolUse",
                                shown(&state["input"]),
                                name,
                                at,
                                call.clone(),
                                Value::Null,
                            ));
                            let result = match string(state, "status") {
                                "completed" => Some(
                                    state["content"]
                                        .as_array()
                                        .into_iter()
                                        .flatten()
                                        .filter_map(|c| c["text"].as_str())
                                        .collect::<Vec<_>>()
                                        .join("\n"),
                                ),
                                "error" => Some(error_text(&state["error"])),
                                _ => None,
                            };
                            if let Some(result) = result {
                                rows.push(event(
                                    "PostToolUse",
                                    journal::clipped(&json!(result), TEXT),
                                    name,
                                    at,
                                    format!("{call}:result"),
                                    Value::Null,
                                ));
                            }
                        }
                        _ => {}
                    }
                }
                let text = texts.join("\n");
                if let Some(index) = reply_at.filter(|_| !text.trim().is_empty()) {
                    let done = completed.map_or(at, |t| t / 1000.0);
                    rows.insert(
                        index,
                        event("AgentMessage", text, "", done, id.to_owned(), json!(id)),
                    );
                }
                // OpenCode still changes this message; history must not keep
                // these rows until it completes.
                if completed.is_none() {
                    for row in &mut rows[first..] {
                        row["streaming"] = json!(true);
                    }
                }
            }
            "idle" => {
                let (kind, detail) = match string(message, "outcome") {
                    "succeeded" => ("Stop", String::new()),
                    "failed" => ("StopFailure", error_text(&message["error"])),
                    other => ("Stop", journal::clipped(&json!(other), 200)),
                };
                rows.push(event(kind, detail, "", at, id.to_owned(), Value::Null));
            }
            _ => {}
        }
    }
    rows
}

/// The latest turn's failure: the newest idle message when it failed and no
/// newer prompt or turn superseded it.
pub(super) fn last_error(messages: &[Value]) -> Option<Value> {
    for message in messages.iter().rev() {
        match string(message, "type") {
            "user" => return None,
            "idle" if message["outcome"] == "failed" => {
                return Some(
                    json!({"id": message["id"], "message": error_text(&message["error"]), "at": seconds(message)}),
                )
            }
            "idle" => return None,
            _ => {}
        }
    }
    None
}

/// Inbox items waiting for this session. After Stop OpenCode keeps them
/// without running them (not delivered); when the session's next message
/// starts a run, the waiting message runs first and that message after it
/// (OpenCode 2.0.32, native `test_11`, one waiting message). Zerus never
/// posts them again.
pub(super) fn queued_rows(inbox: &Value, stopped: bool) -> Vec<Value> {
    inbox["data"]
        .as_array()
        .into_iter()
        .flatten()
        .filter(|item| item["type"] == "user" && valid(item))
        .take(INBOX)
        .map(|item| {
            let mut row = event(
                "UserPromptSubmit",
                string(&item["payload"], "text").to_owned(),
                "",
                seconds(item),
                string(item, "id").to_owned(),
                Value::Null,
            );
            row["delivery_state"] = json!(if stopped { "not_delivered" } else { "queued" });
            row
        })
        .collect()
}

#[cfg(test)]
mod tests {
    use super::*;
    fn load(name: &str) -> Value {
        let path = format!(
            "{}/tests/fixtures/opencode2/{name}.json",
            env!("CARGO_MANIFEST_DIR")
        );
        serde_json::from_slice(&std::fs::read(path).unwrap()).unwrap()
    }
    fn chronological(page: Value) -> Vec<Value> {
        let mut m = page["data"].as_array().unwrap().clone();
        m.reverse();
        m
    }

    #[test]
    fn user_assistant_and_idle_messages_become_activity_events() {
        let rows = rows(&chronological(load("messages")));
        let types: Vec<_> = rows.iter().map(|r| r["type"].as_str().unwrap()).collect();
        assert_eq!(types, vec!["UserPromptSubmit", "AgentMessage", "Stop"]);
        assert_eq!(rows[0]["detail"], "hello");
        assert_eq!(rows[1]["detail"], "Native OpenCode reply");
        assert_eq!(rows[1]["reply_id"], rows[1]["message_id"]);
        assert!(rows.iter().all(|r| r["source"] == "opencode_native"
            && r["message_id"].as_str().unwrap().starts_with("msg_")));
    }

    #[test]
    fn tool_parts_become_pre_and_post_tool_events() {
        let rows = rows(&chronological(load("messages_question")));
        let pre = rows.iter().find(|r| r["type"] == "PreToolUse").unwrap();
        assert_eq!(pre["tool"], "question");
        let post = rows.iter().find(|r| r["type"] == "PostToolUse").unwrap();
        assert!(post["detail"]
            .as_str()
            .unwrap()
            .contains("User has answered"));
    }

    #[test]
    fn a_failed_idle_is_a_provider_error() {
        let messages = chronological(load("messages_error"));
        let error = last_error(&messages).unwrap();
        assert_eq!(error["message"], "Fixture provider rejected");
        assert!(rows(&messages).iter().any(|r| r["type"] == "StopFailure"));
    }

    #[test]
    fn inbox_items_are_queued_and_after_stop_not_delivered() {
        let inbox = load("inbox_busy");
        let queued = queued_rows(&inbox, false);
        assert_eq!(queued[0]["type"], "UserPromptSubmit");
        assert_eq!(queued[0]["delivery_state"], "queued");
        assert_eq!(queued[0]["detail"], "queued later");
        assert_eq!(
            queued_rows(&inbox, true)[0]["delivery_state"],
            "not_delivered"
        );
    }

    #[test]
    fn tool_events_have_unique_ids_and_carry_input_and_result() {
        let rows = rows(&chronological(load("messages_question")));
        let tools: Vec<_> = rows
            .iter()
            .filter(|r| r["type"] == "PreToolUse" || r["type"] == "PostToolUse")
            .collect();
        assert_eq!(tools.len(), 2);
        assert_ne!(tools[0]["message_id"], tools[1]["message_id"]);
        assert_eq!(
            tools[0]["message_id"],
            "msg_14bb2df17001mGPCg2h4EGlMmX:call_mgrimcii"
        );
        assert!(tools[0]["detail"]
            .as_str()
            .unwrap()
            .contains("Pick a color"));
        assert_eq!(tools[1]["tool"], "question");
        assert!(tools.iter().all(|r| r["reply_id"].is_null()));
        // Every event ID is unique, so Activity and history never merge rows.
        let mut ids: Vec<_> = rows.iter().map(|r| r["message_id"].to_string()).collect();
        ids.sort();
        ids.dedup();
        assert_eq!(ids.len(), rows.len());
    }

    #[test]
    fn an_assistant_message_joins_its_text_where_it_starts() {
        let message = json!({"id":"msg_A","type":"assistant","time":{"created":2000},"content":[
            {"type":"text","text":"Checking"},
            {"type":"tool","id":"call_1","name":"bash","state":{"status":"error","input":{"command":"x"},"error":"denied"}},
            {"type":"reasoning","text":"hidden"},
            {"type":"text","text":"Done"}]});
        let rows = rows(&[message]);
        let types: Vec<_> = rows.iter().map(|r| string(r, "type")).collect();
        assert_eq!(types, ["AgentMessage", "PreToolUse", "PostToolUse"]);
        assert_eq!(rows[0]["detail"], "Checking\nDone");
        assert_eq!(rows[0]["reply_id"], "msg_A");
        assert_eq!(rows[0]["at"], 2.0);
        assert_eq!(rows[2]["detail"], "denied");
    }

    #[test]
    fn rows_of_a_streaming_reply_are_marked_until_it_completes() {
        let mut message = json!({"id":"msg_A","type":"assistant","time":{"created":2000},"content":[
            {"type":"text","text":"Partial"},
            {"type":"tool","id":"call_1","name":"bash","state":{"status":"running","input":{}}}]});
        let streaming = rows(std::slice::from_ref(&message));
        assert_eq!(streaming.len(), 2);
        assert!(streaming.iter().all(|r| r["streaming"] == true));
        message["time"]["completed"] = json!(3500);
        let done = rows(&[message]);
        assert!(done.iter().all(|r| r.get("streaming").is_none()));
        // A finished reply is dated when it completed; its tools when it started.
        assert_eq!(done[0]["at"], 3.5);
        assert_eq!(done[1]["at"], 2.0);
        let user = rows(&[json!({"id":"msg_U","type":"user","text":"x","time":{"created":1}})]);
        assert!(user[0].get("streaming").is_none());
    }

    #[test]
    fn other_idle_outcomes_stop_with_their_outcome_and_clear_errors() {
        let idle = |id: &str, outcome: &str| json!({"id":id,"type":"idle","outcome":outcome,"time":{"created":1}});
        let rows = rows(&[idle("msg_A", "interrupted")]);
        assert_eq!(rows[0]["type"], "Stop");
        assert_eq!(rows[0]["detail"], "interrupted");
        let mut failed = chronological(load("messages_error"));
        // A later successful turn or a new prompt supersedes the failure.
        failed.push(idle("msg_B", "succeeded"));
        assert!(last_error(&failed).is_none());
        let mut prompted = chronological(load("messages_error"));
        prompted.push(json!({"id":"msg_C","type":"user","text":"again","time":{"created":2}}));
        assert!(last_error(&prompted).is_none());
        let error = last_error(&chronological(load("messages_error"))).unwrap();
        assert_eq!(error["id"], "msg_14bb546e7001QR5FwtWF0VburA");
    }

    #[test]
    fn queued_rows_skip_foreign_and_invalid_items() {
        let inbox = json!({"data":[
            {"id":"../x","type":"user","payload":{"text":"bad"},"time":{"created":1}},
            {"id":"msg_A","type":"other","payload":{"text":"skip"},"time":{"created":1}},
            {"id":"msg_B","type":"user","payload":{"text":"keep"},"time":{"created":3000}}]});
        let rows = queued_rows(&inbox, false);
        assert_eq!(rows.len(), 1);
        assert_eq!(rows[0]["message_id"], "msg_B");
        assert_eq!(rows[0]["at"], 3.0);
        assert!(queued_rows(&json!({}), true).is_empty());
    }

    /// Serves scripted `/message` pages and records each requested path.
    struct Pages {
        pages: std::cell::RefCell<Vec<Value>>,
        paths: std::cell::RefCell<Vec<String>>,
    }
    impl Native for Pages {
        fn raw(&self, method: &str, path: &str, _: Option<&Value>) -> Result<Response> {
            assert_eq!(method, "GET");
            self.paths.borrow_mut().push(path.to_owned());
            let body = self.pages.borrow_mut().remove(0);
            Ok(Response { status: 200, body })
        }
    }
    fn pages(pages: Vec<Value>) -> Pages {
        Pages {
            pages: std::cell::RefCell::new(pages),
            paths: Default::default(),
        }
    }
    fn message(id: &str, text: &str) -> Value {
        json!({"id":id,"type":"user","text":text,"time":{"created":1}})
    }
    /// A newest-first page of these IDs (given oldest first) and a cursor.
    fn listed(ids: &[&str], next: Option<&str>) -> Value {
        let data: Vec<_> = ids.iter().rev().map(|id| message(id, "x")).collect();
        json!({"data": data, "cursor": {"previous": null, "next": next}})
    }
    fn ids(page: &Page) -> Vec<&str> {
        page.messages.iter().map(|m| string(m, "id")).collect()
    }
    fn record() -> Value {
        json!({"conversation_id": "ses_A"})
    }
    fn later() -> Instant {
        Instant::now() + Duration::from_secs(60)
    }

    #[test]
    fn pages_are_followed_newest_first_and_returned_in_order() {
        let native = pages(vec![
            listed(&["msg_3", "msg_4"], Some("next/+=")),
            listed(&["msg_1", "msg_2"], Some("again")),
            listed(&[], None),
        ]);
        let (page, changed) = scan(&native, &record(), None, Bounds::default(), later()).unwrap();
        assert_eq!(ids(&page), ["msg_1", "msg_2", "msg_3", "msg_4"]);
        assert!(!page.truncated && changed);
        assert_eq!(
            *native.paths.borrow(),
            [
                "/api/session/ses_A/message?limit=100&order=desc",
                "/api/session/ses_A/message?limit=100&cursor=next%2F%2B%3D",
                "/api/session/ses_A/message?limit=100&cursor=again",
            ]
        );
    }

    #[test]
    fn the_cache_supplies_older_messages_and_the_first_page_stays_fresh() {
        let cached = Cache {
            messages: vec![
                message("msg_1", "a"),
                message("msg_2", "b"),
                message("msg_3", "old"),
            ],
            truncated: false,
        };
        // The newest message changed in place (streaming) and one was added;
        // the page reaches back into the cache, so no further page is read.
        let mut first = listed(&["msg_3", "msg_4"], Some("more"));
        first["data"][1] = message("msg_3", "new");
        let native = pages(vec![first]);
        let (page, changed) = scan(
            &native,
            &record(),
            Some(&cached),
            Bounds::default(),
            later(),
        )
        .unwrap();
        assert_eq!(ids(&page), ["msg_1", "msg_2", "msg_3", "msg_4"]);
        assert_eq!(page.messages[2]["text"], "new");
        assert!(changed && !page.truncated);
        assert_eq!(native.paths.borrow().len(), 1);
        // Same newest message: reuse without rewriting the cache.
        let native = pages(vec![page_of(&page.messages[2..])]);
        let cached = Cache {
            messages: page.messages.clone(),
            truncated: false,
        };
        let (again, changed) = scan(
            &native,
            &record(),
            Some(&cached),
            Bounds::default(),
            later(),
        )
        .unwrap();
        assert_eq!(ids(&again), ids(&page));
        assert!(!changed);
    }
    fn page_of(messages: &[Value]) -> Value {
        let data: Vec<_> = messages.iter().rev().cloned().collect();
        json!({"data": data, "cursor": {"previous": null, "next": "more"}})
    }

    #[test]
    fn a_cached_message_read_again_is_never_listed_twice() {
        let cached = Cache {
            messages: vec![
                message("msg_1", "x"),
                message("msg_2", "x"),
                message("msg_3", "x"),
            ],
            truncated: false,
        };
        // The service orders msg_2 after msg_3, unlike the cache.
        let native = pages(vec![listed(&["msg_3", "msg_2", "msg_4"], Some("c"))]);
        let (page, _) = scan(
            &native,
            &record(),
            Some(&cached),
            Bounds::default(),
            later(),
        )
        .unwrap();
        assert_eq!(ids(&page), ["msg_1", "msg_3", "msg_2", "msg_4"]);
    }

    #[test]
    fn a_cache_that_does_not_connect_is_ignored() {
        let cached = Cache {
            messages: vec![message("msg_other", "x")],
            truncated: true,
        };
        let native = pages(vec![listed(&["msg_1"], None)]);
        let (page, _) = scan(
            &native,
            &record(),
            Some(&cached),
            Bounds::default(),
            later(),
        )
        .unwrap();
        assert_eq!(ids(&page), ["msg_1"]);
        assert!(!page.truncated);
    }

    #[test]
    fn bounds_keep_the_newest_window_and_report_truncation() {
        let bounds = Bounds {
            messages: 3,
            bytes: usize::MAX,
        };
        let native = pages(vec![
            listed(&["msg_3", "msg_4"], Some("c1")),
            listed(&["msg_1", "msg_2"], Some("c2")),
        ]);
        let (page, _) = scan(&native, &record(), None, bounds, later()).unwrap();
        assert_eq!(ids(&page), ["msg_2", "msg_3", "msg_4"]);
        assert!(page.truncated);
        // The byte budget applies to the spliced list too.
        let size = message("msg_1", "x").to_string().len();
        let bounds = Bounds {
            messages: 100,
            bytes: size * 2,
        };
        let cached = Cache {
            messages: vec![message("msg_1", "x"), message("msg_2", "x")],
            truncated: false,
        };
        let native = pages(vec![listed(&["msg_2", "msg_3"], Some("c"))]);
        let (page, _) = scan(&native, &record(), Some(&cached), bounds, later()).unwrap();
        assert_eq!(ids(&page), ["msg_2", "msg_3"]);
        assert!(page.truncated);
    }

    #[test]
    fn a_passed_deadline_or_a_repeated_page_stops_with_the_newest_window() {
        let native = pages(vec![listed(&["msg_1", "msg_2"], Some("c"))]);
        let (page, _) = scan(&native, &record(), None, Bounds::default(), Instant::now()).unwrap();
        assert_eq!(ids(&page), ["msg_1", "msg_2"]);
        assert!(page.truncated);
        let native = pages(vec![
            listed(&["msg_1", "msg_2"], Some("c")),
            listed(&["msg_1", "msg_2"], Some("c")),
        ]);
        let (page, _) = scan(&native, &record(), None, Bounds::default(), later()).unwrap();
        assert_eq!(ids(&page), ["msg_1", "msg_2"]);
        assert!(page.truncated);
    }

    #[test]
    fn invalid_pages_and_messages_are_refused_or_skipped() {
        let native = pages(vec![json!({"data": {}})]);
        assert!(scan(&native, &record(), None, Bounds::default(), later()).is_err());
        let mut odd = listed(&["msg_1"], None);
        odd["data"]
            .as_array_mut()
            .unwrap()
            .push(json!({"id":"bad id","type":"user","text":"x"}));
        let native = pages(vec![odd]);
        let (page, _) = scan(&native, &record(), None, Bounds::default(), later()).unwrap();
        assert_eq!(ids(&page), ["msg_1"]);
    }

    #[test]
    fn the_cache_file_round_trips_only_for_its_conversation() {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("history.json");
        let page = Page {
            messages: vec![message("msg_1", "x")],
            truncated: true,
        };
        save_at(&path, &record(), &page);
        let cache = load_at(&path, &record()).unwrap();
        assert_eq!(cache.messages, page.messages);
        assert!(cache.truncated);
        assert!(load_at(&path, &json!({"conversation_id": "ses_B"})).is_none());
        assert_ne!(
            cache_path(&record()),
            cache_path(&json!({"conversation_id": "ses_B"}))
        );
    }
}
