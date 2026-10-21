//! Optional Codex questions are asynchronous messages, not blocking tool calls.
//! Preserve them independently of tool completion and acknowledge exact native
//! questionItemIds. Skip is a local dismissal and never fabricates a user answer.
use super::question_terminal::{Native, Transport};
use super::*;
use sha2::{Digest, Sha256};
use std::fs::{self, File};
use std::io::{BufRead, BufReader, Seek, SeekFrom};
use std::time::{Duration, Instant};

fn cache_path(record: &Value) -> Result<PathBuf> {
    Ok(absolute_root()?.join("codex_questions").join(format!(
        "{:x}.json",
        Sha256::digest(json!([record["agent_home"], record["conversation_id"]]).to_string())
    )))
}
fn parse_reply(text: &str) -> Vec<Value> {
    let text = text.trim();
    let text = if text.starts_with("# Context from my IDE setup:\n") {
        let Some((_, request)) = text.rsplit_once("\n## My request for Codex:\n") else {
            return vec![];
        };
        request.trim()
    } else {
        text
    };
    let Some(body) = text
        .strip_prefix("<send_user_message_question_reply>")
        .and_then(|s| s.strip_suffix("</send_user_message_question_reply>"))
    else {
        return vec![];
    };
    match serde_json::from_str::<Value>(body.trim()) {
        Ok(Value::Array(a)) => a,
        Ok(v @ Value::Object(_)) => vec![v],
        _ => vec![],
    }
}
fn observe(record: &Value, cache: &mut Value, event: &Value) {
    let payload = &event["payload"];
    if event["type"] == "response_item"
        && payload["type"] == "function_call"
        && matches!(
            string(payload, "name"),
            "request_user_input_async" | "functions.request_user_input_async"
        )
    {
        let call = string(payload, "call_id");
        if call.is_empty() || call.len() > 200 {
            return;
        }
        let Ok(args) = serde_json::from_str::<Value>(string(payload, "arguments")) else {
            return;
        };
        let Some(items) = args["questions"]
            .as_array()
            .filter(|a| !a.is_empty() && a.len() <= 16)
        else {
            return;
        };
        for (index, item) in items.iter().enumerate() {
            let title = string(item, "title");
            if title.trim().is_empty()
                || title.len() > 8192
                || title
                    .chars()
                    .any(|c| c.is_control() && c != '\n' && c != '\t')
            {
                continue;
            }
            let options = match item.get("options") {
                None | Some(Value::Null) => vec![],
                Some(Value::Array(a)) if a.len() <= 20 => a.clone(),
                _ => continue,
            };
            if options.iter().any(|o| {
                o.as_str().is_none_or(|s| {
                    s.trim().is_empty() || s.len() > 1024 || s.chars().any(char::is_control)
                })
            }) {
                continue;
            }
            let native_id = json!(["request_user_input_async", call, index]).to_string();
            let id = format!("codex-async:{call}:{index}");
            if !cache["items"][&id].is_null() {
                continue;
            }
            let choices: Vec<_> = options
                .iter()
                .enumerate()
                .map(|(i, o)| json!({"id":format!("opt_{i}"),"label":o,"description":""}))
                .collect();
            let mut card = json!({"question_id":id,"tool_call_id":call,"native_question_id":native_id,
                "run_id":record["run_id"],"conversation_id":record["conversation_id"],"source":"codex_async",
                "created_at":search::timestamp(&event["timestamp"]),"optional":true,"can_skip":true,"can_answer":false,
                "questions":[{"id":"q_0","question":title,"options":choices,"allow_other":true,"required":false,"multi_select":false}]});
            card["question_hash"] = json!(questions::fingerprint(&card));
            cache["items"][id] = card;
        }
    }
    if event["type"] == "response_item" && payload["type"] == "function_call_output" {
        if let Some(items) = cache["items"].as_object_mut() {
            let accepted = serde_json::from_str::<Value>(string(payload, "output"))
                .ok()
                .is_some_and(|v| v["accepted"] == true);
            for card in items
                .values_mut()
                .filter(|c| c["tool_call_id"] == payload["call_id"])
            {
                card["accepted"] = json!(accepted);
            }
        }
    }
    let texts: Vec<&str> = if event["type"] == "event_msg" && payload["type"] == "user_message" {
        vec![string(payload, "message")]
    } else if event["type"] == "response_item"
        && payload["type"] == "message"
        && payload["role"] == "user"
    {
        payload["content"]
            .as_array()
            .into_iter()
            .flatten()
            .filter_map(|p| p["text"].as_str())
            .collect()
    } else {
        vec![]
    };
    for reply in texts.into_iter().flat_map(parse_reply) {
        if let Some(items) = cache["items"].as_object_mut() {
            for card in items.values_mut() {
                if reply["questionItemId"] == card["native_question_id"]
                    && reply["question"] == card["questions"][0]["question"]
                    && reply["answer"].is_string()
                {
                    card["response"] = json!({"type":"interaction.resolved","time":search::timestamp(&event["timestamp"])*1000.,
                    "response":{"answers":{string(&reply,"question"):reply["answer"]}}});
                }
            }
        }
    }
}
fn read_cache(record: &Value) -> Result<Value> {
    index(record, &cache_path(record)?, None)?.ok_or_else(|| "question index incomplete".into())
}
/// Bring the question cache up to date with the transcript. With a deadline,
/// indexing stops once it passes (after at least one line) and saves its
/// progress, so the next call resumes; the cache is returned only when it is
/// complete. None also when another process holds the cache past the deadline.
fn index(record: &Value, cache_path: &Path, deadline: Option<Instant>) -> Result<Option<Value>> {
    let path = PathBuf::from(string(record, "transcript"));
    let id = string(record, "conversation_id");
    if uuid::Uuid::parse_str(id).is_err()
        || string(record, "run_id").is_empty()
        || !path.is_absolute()
        || !path
            .file_name()
            .is_some_and(|n| n.to_string_lossy().contains(id))
    {
        return Ok(Some(json!({"items":{}})));
    }
    let lock_path = cache_path.with_extension("lock");
    let _guard = match deadline {
        None => lock(Some(&lock_path))?,
        Some(deadline) => match lock_until(&lock_path, deadline)? {
            Some(guard) => guard,
            None => return Ok(None),
        },
    };
    let file = File::open(&path).map_err(|e| e.to_string())?;
    let (identity, meta) =
        crate::platform::FileIdentity::with_metadata(&file).map_err(|e| e.to_string())?;
    let mut reader = BufReader::new(file);
    let mut line = String::new();
    reader
        .by_ref()
        .take(256 * 1024)
        .read_line(&mut line)
        .map_err(|e| e.to_string())?;
    let header: Value = serde_json::from_str(&line).map_err(|e| e.to_string())?;
    if header["type"] != "session_meta" || header["payload"]["id"] != id {
        return Err("Conversation transcript identity mismatch".into());
    }
    let mut cache: Value = fs::read_to_string(cache_path)
        .ok()
        .and_then(|s| serde_json::from_str(&s).ok())
        .unwrap_or(json!({"items":{}}));
    let inode = json!([identity.device, identity.inode]);
    let offset = cache["offset"].as_u64().unwrap_or(0);
    if cache["inode"] != inode || offset > meta.len() {
        cache = json!({"items":{},"offset":0,"inode":inode});
    }
    // Questions can stay unanswered long after they leave the recent-message
    // tail. Index the entire exact conversation once, then only appended bytes.
    // Reindex old tail-only caches while retaining explicit local dismissals.
    if cache["scan_version"] != 2 {
        cache["offset"] = json!(0);
        cache["scan_version"] = json!(2);
    }
    let offset = cache["offset"].as_u64().unwrap_or(0);
    reader
        .seek(SeekFrom::Start(offset))
        .map_err(|e| e.to_string())?;
    // Initial tail can begin inside a JSON line; never interpret that fragment.
    if cache.get("initialized").is_none() && offset > 0 {
        line.clear();
        reader.read_line(&mut line).map_err(|e| e.to_string())?;
    }
    let mut complete = true;
    loop {
        let before = reader.stream_position().map_err(|e| e.to_string())?;
        line.clear();
        let count = reader
            .by_ref()
            .take(1024 * 1024 + 1)
            .read_line(&mut line)
            .map_err(|e| e.to_string())?;
        if count == 0 {
            break;
        }
        if !line.ends_with('\n') {
            if count <= 1024 * 1024 {
                cache["offset"] = json!(before);
                break;
            }
            // An oversized line is skipped unparsed; it still counts against the deadline.
            reader.skip_until(b'\n').map_err(|e| e.to_string())?;
        }
        cache["offset"] = json!(reader.stream_position().map_err(|e| e.to_string())?);
        if count <= 1024 * 1024
            && (line.contains("request_user_input_async")
                || line.contains("function_call_output")
                || line.contains("send_user_message_question_reply"))
        {
            if let Ok(event) = serde_json::from_str(&line) {
                observe(record, &mut cache, &event);
            }
        }
        // Lines appended while indexing do not count against the deadline.
        if deadline.is_some_and(|deadline| Instant::now() >= deadline)
            && cache["offset"].as_u64().unwrap_or(0) < meta.len()
        {
            complete = false;
            break;
        }
    }
    if complete {
        recover_submissions(record, &mut cache)?;
    }
    // The offset is at a line boundary, also after a partial scan.
    cache["initialized"] = json!(true);
    let serialized = cache.to_string();
    if fs::read_to_string(cache_path).ok().as_deref() != Some(&serialized) {
        atomic(cache_path, &serialized)?;
    }
    Ok(complete.then_some(cache))
}
pub(super) fn current(record: &Value) -> Result<Vec<Value>> {
    if string(record, "agent") != "codex" {
        return Ok(vec![]);
    }
    Ok(pending(record, &read_cache(record)?))
}
/// Pending questions, or None when the index is not complete by `deadline`.
pub(super) fn current_until(record: &Value, deadline: Instant) -> Result<Option<Vec<Value>>> {
    if string(record, "agent") != "codex" {
        return Ok(Some(vec![]));
    }
    Ok(index(record, &cache_path(record)?, Some(deadline))?.map(|cache| pending(record, &cache)))
}
fn pending(record: &Value, cache: &Value) -> Vec<Value> {
    cache["items"]
        .as_object()
        .into_iter()
        .flat_map(|o| o.values())
        .filter(|c| c["accepted"] == true && c["response"].is_null() && c["skipped"] != true)
        .cloned()
        .map(|mut card| {
            card["run_id"] = record["run_id"].clone();
            card["question_hash"] = json!(questions::fingerprint(&card));
            if matching_submission(record, &card, &card["delivery"]) {
                card["answer_delivery"] = card["delivery"].clone();
            }
            card.as_object_mut().unwrap().remove("delivery");
            card
        })
        .collect()
}
pub(super) fn messages(record: &Value) -> Result<Vec<Value>> {
    if string(record, "agent") != "codex" {
        return Ok(vec![]);
    }
    let cache = read_cache(record)?;
    Ok(cache["items"].as_object().into_iter().flat_map(|m|m.values()).filter(|c|c["accepted"]==true).map(|card|{
        let question=&card["questions"][0];
        let choices=question["options"].as_array().unwrap().iter().flat_map(|o|["\n- ",string(o,"label")]).collect::<String>();
        let status=if card["skipped"]==true {"Skipped"}else if !card["response"].is_null(){"Answered"}else{"Optional"};
        json!({"type":"AgentMessage","source":"codex_transcript","message_id":card["question_id"],"agent_id":"",
            "at":card["created_at"],"message_phase":"commentary","detail":format!("{}\n{}{}",status,string(question,"question"),choices)})
    }).collect())
}
fn unambiguous(record: &Value, card: &Value) -> Result<bool> {
    Ok(current(record)?
        .iter()
        .filter(|c| c["questions"] == card["questions"])
        .count()
        == 1)
}
pub(super) fn response(record: &Value, card: &Value) -> Result<Option<Value>> {
    let cache = read_cache(record)?;
    let response = &cache["items"][string(card, "question_id")]["response"];
    Ok((!response.is_null()).then(|| response.clone()))
}
fn panel_body(screen: &str, card: &Value) -> Result<Option<String>> {
    Ok(panel(screen, card)?.map(|(_, body)| body))
}
/// Codex animates its working status above a waiting question. Before input,
/// only this card's complete panel must be unchanged.
pub(super) fn same_panel(first: &str, second: &str, card: &Value) -> bool {
    matches!((panel(first, card), panel(second, card)), (Ok(Some((a, _))), Ok(Some((b, _)))) if a == b)
}
/// This card's visible panel rows, from its progress line or title through the
/// key hints, and the answer rows within them.
fn panel(screen: &str, card: &Value) -> Result<Option<(String, String)>> {
    let plain = question_terminal::plain(screen)?;
    let lines: Vec<_> = plain.lines().collect();
    let Some(footer) = lines.iter().rposition(|line| {
        // Codex 0.162 abbreviates the skip key as "^]".
        line.contains("enter submit") && (line.contains("ctrl+] skip") || line.contains("^] skip"))
    }) else {
        return Ok(None);
    };
    let Some(body_end) = (0..footer).rev().find(|i| !lines[*i].trim().is_empty()) else {
        return Ok(None);
    };
    let Some(separator) = (0..body_end).rev().find(|i| lines[*i].trim().is_empty()) else {
        return Ok(None);
    };
    let Some(title_end) = (0..separator).rev().find(|i| !lines[*i].trim().is_empty()) else {
        return Ok(None);
    };
    let mut title_start = (0..title_end)
        .rev()
        .find(|i| lines[*i].trim().is_empty())
        .map_or(0, |i| i + 1);
    let start = title_start;
    // Codex 0.162 numbers the questions of one call above the title ("1 of 2").
    // Only this card's own position may stand there.
    if let Some(shown) = progress(lines[title_start]) {
        if title_start == title_end || Some(shown) != position(card) {
            return Ok(None);
        }
        title_start += 1;
    }
    let compact = |s: &str| s.split_whitespace().collect::<String>();
    if compact(&lines[title_start..=title_end].join("\n"))
        != compact(string(&card["questions"][0], "question"))
    {
        return Ok(None);
    }
    let body = lines[separator + 1..=body_end].join("\n");
    let options = card["questions"][0]["options"]
        .as_array()
        .ok_or("missing options")?;
    if !options
        .iter()
        .all(|o| compact(&body).contains(&compact(string(o, "label"))))
    {
        return Ok(None);
    }
    Ok(Some((lines[start..=footer].join("\n"), body)))
}
fn progress(line: &str) -> Option<usize> {
    let (shown, total) = line.trim().split_once(" of ")?;
    let shown: usize = shown.parse().ok()?;
    (total.parse::<usize>().ok()? >= shown).then_some(shown)
}
fn position(card: &Value) -> Option<usize> {
    let index: usize = string(card, "question_id")
        .rsplit(':')
        .next()?
        .parse()
        .ok()?;
    Some(index + 1)
}
fn visible_panel(screen: &str, card: &Value) -> Result<bool> {
    Ok(panel_body(screen, card)?.is_some())
}
fn panel_empty(screen: &str, card: &Value) -> Result<bool> {
    let Some(body) = panel_body(screen, card)? else {
        return Ok(false);
    };
    let options = card["questions"][0]["options"].as_array().unwrap();
    if options.is_empty() {
        return Ok(body.trim() == "Type your answer");
    }
    let index = options.len() + 1;
    // An unselected Other row displays its saved draft instead of the label.
    // Never overwrite either a visible draft or one stored behind a selection.
    Ok(body.lines().last().is_some_and(|line| {
        let row = line.trim().trim_start_matches('›').trim();
        row == format!("{index}. Other") || row == format!("{index}. Other (write an answer)")
    }))
}
/// What a paste left in this card's panel, wrapped rows joined: the selected
/// Other row, or a free-form question's answer field. None while another row
/// is selected; an option label with the same text is not a draft.
fn other_draft(screen: &str, card: &Value) -> Result<Option<String>> {
    let Some(body) = panel_body(screen, card)? else {
        return Ok(None);
    };
    let words = |text: &str| text.split_whitespace().collect::<Vec<_>>().join(" ");
    let options = card["questions"][0]["options"].as_array().map_or(0, Vec::len);
    if options == 0 {
        return Ok(Some(words(&body)));
    }
    let label = format!("{}. ", options + 1);
    let rows: Vec<_> = body.lines().collect();
    let Some(start) = rows.iter().rposition(|row| {
        row.trim_start()
            .strip_prefix('›')
            .is_some_and(|rest| rest.trim_start().starts_with(&label))
    }) else {
        return Ok(None);
    };
    let draft = words(&rows[start..].join("\n"));
    Ok(draft.strip_prefix('›').map(str::trim_start).and_then(|row| row.strip_prefix(&label)).map(str::to_owned))
}
/// The key hints below an open question panel: its last row offering submit and skip.
fn hints(screen: &str) -> Option<String> {
    let plain = question_terminal::plain(screen).ok()?;
    plain
        .lines()
        .rev()
        .find(|line| {
            line.contains("enter submit") && (line.contains("ctrl+] skip") || line.contains("^] skip"))
        })
        .map(str::to_owned)
}
/// Whether key hints name the visible row of a scrolled answer list ("option 1/6").
fn scrolled(hints: &str) -> bool {
    let words: Vec<_> = hints.split_whitespace().collect();
    words.windows(2).any(|pair| {
        pair[0] == "option"
            && pair[1].split_once('/').is_some_and(|(row, rows)| {
                !row.is_empty()
                    && !rows.is_empty()
                    && row.chars().chain(rows.chars()).all(|c| c.is_ascii_digit())
            })
    })
}
/// Codex 0.160.1 and 0.162.0 fit an open question panel taller than the pane
/// by scrolling its answer list below the whole title, and name the visible
/// row in the key hints. Such a panel is clipped; once the pane holds every
/// answer, the hints drop that name and the panel is complete.
fn clipped(screen: &str) -> bool {
    hints(screen).is_some_and(|row| scrolled(&row))
}
fn complete(screen: &str) -> bool {
    hints(screen).is_some_and(|row| !scrolled(&row))
}
/// Grow the pane no client views until this question's scrolled panel shows
/// every answer (`question_terminal::fit_unviewed`); the exact match in
/// `available` still decides. Only a question listing calls it. A title taller
/// than the pane pushes the key hints off screen, so that panel stays in Terminal.
pub(super) fn fit(record: &Value, card: &Value) {
    if card["answer_delivery"]["status"] == "submitted" {
        return;
    }
    let Ok(current) = Native::new(record, card).identity() else {
        return;
    };
    if let Ok(screen) = Native::capture(&current) {
        question_terminal::fit_unviewed(&current, screen, Native::capture, clipped, complete);
    }
}
pub(super) fn available(record: &Value, card: &Value) -> Result<()> {
    if card["answer_delivery"]["status"] == "submitted" {
        return Err("An answer was already submitted. Wait for Codex to record it.".into());
    }
    let mut terminal = Native::new(record, card);
    let screen = terminal.screen()?;
    if visible_panel(&screen, card)? {
        if !unambiguous(record, card)? {
            return Err(
                "Several pending questions have the same text. Choose one in Terminal.".into(),
            );
        }
        if !panel_empty(&screen, card)? {
            return Err("This question has a draft in Terminal. Finish or clear it there.".into());
        }
        return Ok(());
    }
    input::checked_terminal(record, true)
}
fn answer_text(card: &Value, answer: &Value) -> Result<String> {
    if let Some(id) = answer["selected_option_ids"]
        .as_array()
        .and_then(|a| a.first())
    {
        card["questions"][0]["options"]
            .as_array()
            .unwrap()
            .iter()
            .find(|o| o["id"] == *id)
            .map(|o| string(o, "label").to_owned())
            .ok_or("unknown answer option".into())
    } else {
        Ok(string(answer, "text").to_owned())
    }
}
fn envelope(card: &Value, text: &str) -> String {
    format!(
        "<send_user_message_question_reply>\n{}\n</send_user_message_question_reply>",
        json!([{"questionItemId":card["native_question_id"],"question":card["questions"][0]["question"],"answer":text}])
    )
}

fn matching_submission(record: &Value, card: &Value, delivery: &Value) -> bool {
    delivery["status"] == "submitted"
        && delivery["run_id"] == record["run_id"]
        && delivery["conversation_id"] == record["conversation_id"]
        && delivery["question_id"] == card["question_id"]
        && delivery["question_hash"] == questions::fingerprint(card)
}

fn submission(record: &Value, card: &Value, answers: &Value, request_id: &str) -> Value {
    json!({"status":"submitted","request_id":request_id,"run_id":record["run_id"],
        "conversation_id":record["conversation_id"],"question_id":card["question_id"],
        "question_hash":questions::fingerprint(card),"answers":answers,"submitted_at":now()})
}

fn save_submission(record: &Value, card: &Value, answers: &Value, request_id: &str) -> Result<()> {
    read_cache(record)?;
    let path = cache_path(record)?;
    let _guard = lock(Some(&path.with_extension("lock")))?;
    let mut cache: Value =
        serde_json::from_str(&fs::read_to_string(&path).map_err(|e| e.to_string())?)
            .map_err(|e| e.to_string())?;
    let saved = &mut cache["items"][string(card, "question_id")];
    if saved["questions"] != card["questions"]
        || saved["native_question_id"] != card["native_question_id"]
    {
        return Err("question changed after submission".into());
    }
    saved["delivery"] = submission(record, card, answers, request_id);
    atomic(&path, &cache.to_string())
}

// Older adapters left an in-progress question receipt after successfully
// entering its envelope. Recover only a confirmed question submission or the
// exact durable terminal acknowledgement, once per run; a timeout or a queue
// preview alone never proves submission.
fn recover_submissions(record: &Value, cache: &mut Value) -> Result<()> {
    if cache["submission_scan_run"] == record["run_id"] {
        return Ok(());
    }
    let root = absolute_root()?;
    let entries = match fs::read_dir(root.join("question_receipts")) {
        Ok(entries) => Some(entries),
        Err(e) if e.kind() == io::ErrorKind::NotFound => None,
        Err(e) => return Err(e.to_string()),
    };
    for entry in entries.into_iter().flatten().flatten() {
        if !entry
            .metadata()
            .is_ok_and(|m| m.is_file() && m.len() <= 256 * 1024)
        {
            continue;
        }
        let Some(receipt): Option<Value> = fs::read_to_string(entry.path())
            .ok()
            .and_then(|s| serde_json::from_str(&s).ok())
        else {
            continue;
        };
        let request_id = string(&receipt, "request_id");
        if uuid::Uuid::parse_str(request_id).is_err()
            || receipt["run_id"] != record["run_id"]
            || receipt["conversation_id"] != record["conversation_id"]
            || receipt["name"] != record["name"]
            || !["in_progress", "submitted"].contains(&string(&receipt, "status"))
        {
            continue;
        }
        let mut card = cache["items"][string(&receipt, "question_id")].clone();
        card["run_id"] = record["run_id"].clone();
        if card["source"] != "codex_async"
            || receipt["question_hash"] != questions::fingerprint(&card)
            || receipt["answers"].as_array().is_none_or(|a| a.len() != 1)
            || receipt["answers"][0]["skip"] == true
            || receipt["answers"][0]["question_id"] != card["questions"][0]["id"]
        {
            continue;
        }
        if receipt["status"] == "submitted" {
            if !matching_submission(record, &card, &card["delivery"]) {
                cache["items"][string(&receipt, "question_id")]["delivery"] =
                    submission(record, &card, &receipt["answers"], request_id);
            }
            continue;
        }
        let input_path = root
            .join("input_receipts")
            .join(format!("{request_id}.json"));
        if !fs::metadata(&input_path).is_ok_and(|m| m.is_file() && m.len() <= 256 * 1024) {
            continue;
        }
        let Some(input): Option<Value> = fs::read_to_string(input_path)
            .ok()
            .and_then(|s| serde_json::from_str(&s).ok())
        else {
            continue;
        };
        let Ok(text) = answer_text(&card, &receipt["answers"][0]) else {
            continue;
        };
        if input["status"] == "submitted"
            && input["request_id"] == request_id
            && input["name"] == record["name"]
            && input["run_id"] == record["run_id"]
            && input["conversation_id"] == record["conversation_id"]
            && input["text"] == envelope(&card, &text)
            && !matching_submission(record, &card, &card["delivery"])
        {
            cache["items"][string(&receipt, "question_id")]["delivery"] =
                submission(record, &card, &receipt["answers"], request_id);
        }
    }
    cache["submission_scan_run"] = record["run_id"].clone();
    Ok(())
}

pub(super) fn answer(
    record: &Value,
    card: &Value,
    answers: &Value,
    request_id: &str,
) -> Result<&'static str> {
    let mut terminal = Native::new(record, card);
    let screen = terminal.screen()?;
    if answers[0]["skip"] == true {
        if visible_panel(&screen, card)? && unambiguous(record, card)? {
            terminal.key("C-]")?;
        }
        let path = cache_path(record)?;
        let _guard = lock(Some(&path.with_extension("lock")))?;
        let mut cache: Value =
            serde_json::from_str(&fs::read_to_string(&path).map_err(|e| e.to_string())?)
                .map_err(|e| e.to_string())?;
        cache["items"][string(card, "question_id")]["skipped"] = json!(true);
        atomic(&path, &cache.to_string()).map_err(|e| {
            if terminal.touched() {
                format!("delivery uncertain: {e}")
            } else {
                e
            }
        })?;
        return Ok("answered");
    }
    available(record, card)?;
    let text = answer_text(card, &answers[0])?;
    if visible_panel(&screen, card)? {
        // Native paste selects Other and submits its own exact question envelope.
        let result: Result<()> = (|| {
            terminal.paste(&text)?;
            std::thread::sleep(Duration::from_millis(180));
            let screen = terminal.screen()?;
            // Codex wraps a long answer onto indented rows of its Other field.
            let pasted = text.split_whitespace().collect::<Vec<_>>().join(" ");
            if other_draft(&screen, card)? != Some(pasted) {
                return Err("question or answer draft changed before submission".into());
            }
            terminal.key("Enter")
        })();
        if let Err(error) = result {
            return Err(if terminal.touched() {
                format!("delivery uncertain: {error}")
            } else {
                error
            });
        }
    } else {
        let payload = json!({"request_id":request_id,"text":envelope(card,&text),"expected_run_id":record["run_id"],"expected_conversation_id":record["conversation_id"]});
        input::submit(string(record, "name"), payload.to_string().as_bytes(), None)?;
    }
    save_submission(record, card, answers, request_id)
        .map_err(|e| format!("delivery uncertain: {e}"))?;
    let deadline = Instant::now() + Duration::from_secs(6);
    loop {
        if let Some(response) =
            response(record, card).map_err(|e| format!("delivery uncertain: {e}"))?
        {
            return if response["response"]["answers"][string(&card["questions"][0], "question")]
                == text
            {
                Ok("answered")
            } else {
                Err("delivery uncertain: question received a different answer".into())
            };
        }
        if Instant::now() >= deadline {
            return Ok("submitted");
        }
        std::thread::sleep(Duration::from_millis(80));
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    fn request() -> Value {
        json!({"type":"response_item","timestamp":"2026-10-07T15:00:00Z","payload":{"type":"function_call","name":"request_user_input_async","call_id":"call_one","arguments":json!({"questions":[{"title":"Which option?","options":["One","Two"]},{"title":"Any notes?"}]}).to_string()}})
    }
    #[test]
    fn accepted_is_not_answered_and_each_optional_question_has_an_exact_identity() {
        let record = json!({"run_id":"run","conversation_id":"conv"});
        let mut cache = json!({"items":{}});
        observe(&record, &mut cache, &request());
        assert_eq!(cache["items"].as_object().unwrap().len(), 2);
        observe(
            &record,
            &mut cache,
            &json!({"type":"response_item","payload":{"type":"function_call_output","call_id":"call_one","output":"{\"accepted\":true}"}}),
        );
        let card = cache["items"]["codex-async:call_one:0"].clone();
        assert!(card["response"].is_null());
        assert_eq!(card["optional"], true);
        let reply = envelope(&card, "One");
        assert_eq!(
            parse_reply(&reply)[0]["questionItemId"],
            "[\"request_user_input_async\",\"call_one\",0]"
        );
        let wrong = reply.replace("call_one", "call_other");
        observe(
            &record,
            &mut cache,
            &json!({"type":"event_msg","payload":{"type":"user_message","message":wrong}}),
        );
        assert!(cache["items"]["codex-async:call_one:0"]["response"].is_null());
        observe(
            &record,
            &mut cache,
            &json!({"type":"event_msg","payload":{"type":"user_message","message":reply}}),
        );
        assert!(!cache["items"]["codex-async:call_one:0"]["response"].is_null());
        assert!(cache["items"]["codex-async:call_one:1"]["response"].is_null());
        observe(&record, &mut cache, &request());
        assert!(!cache["items"]["codex-async:call_one:0"]["response"].is_null());
        assert_eq!(
            cache["items"]["codex-async:call_one:1"]["questions"][0]["options"],
            json!([])
        );
    }
    #[test]
    fn bounded_index_saves_its_progress_and_resumes() {
        let dir = tempfile::tempdir().unwrap();
        let id = uuid::Uuid::new_v4().to_string();
        let transcript = dir.path().join(format!("rollout-{id}.jsonl"));
        let accepted = json!({"type":"response_item","payload":{"type":"function_call_output","call_id":"call_one","output":"{\"accepted\":true}"}});
        let mut lines = vec![json!({"type":"session_meta","payload":{"id":id}}), request(), accepted];
        lines.extend((0..5).map(|i| json!({"type":"response_item","payload":{"type":"function_call_output","call_id":format!("call-{i}"),"output":"x"}})));
        fs::write(&transcript, lines.iter().map(|line| line.to_string() + "\n").collect::<String>()).unwrap();
        let record = json!({"agent":"codex","run_id":"run","conversation_id":id,"transcript":transcript});
        let cache = dir.path().join("cache.json");
        let meta = fs::metadata(&transcript).unwrap();
        let source = crate::platform::file_identity(&transcript).unwrap();
        // Skip the one-time submission recovery, which reads the hgs state directory.
        fs::write(&cache, json!({"items":{},"offset":0,"inode":[source.device,source.inode],"scan_version":2,
            "initialized":true,"submission_scan_run":"run"}).to_string()).unwrap();
        // An expired deadline still indexes one line per call and keeps it.
        let expired = Instant::now();
        let mut offsets = Vec::new();
        let complete = loop {
            assert!(offsets.len() < lines.len(), "indexing made no progress");
            if let Some(cache) = index(&record, &cache, Some(expired)).unwrap() {
                break cache;
            }
            let saved: Value = serde_json::from_str(&fs::read_to_string(&cache).unwrap()).unwrap();
            offsets.push(saved["offset"].as_u64().unwrap());
        };
        // Every line but the last one ended a call, the header included.
        assert_eq!(offsets.len(), lines.len() - 1);
        assert!(offsets.windows(2).all(|pair| pair[0] < pair[1]));
        assert_eq!(complete["offset"], json!(meta.len()));
        assert_eq!(pending(&record, &complete).len(), 2);
        assert_eq!(index(&record, &cache, None).unwrap(), Some(complete));
        // A cache held by another process past the deadline is unknown, not empty.
        let _held = lock(Some(&cache.with_extension("lock"))).unwrap();
        assert_eq!(index(&record, &cache, Some(Instant::now() + Duration::from_millis(30))).unwrap(), None);
    }
    #[test]
    fn an_expired_deadline_stops_after_each_oversized_line() {
        let dir = tempfile::tempdir().unwrap();
        let id = uuid::Uuid::new_v4().to_string();
        let transcript = dir.path().join(format!("rollout-{id}.jsonl"));
        // Lines over 1 MiB are skipped unparsed, but each still counts against the deadline.
        let oversized = json!({"type":"event_msg","payload":{"type":"agent_message","message":"x".repeat(1024 * 1024)}});
        let accepted = json!({"type":"response_item","payload":{"type":"function_call_output","call_id":"call_one","output":"{\"accepted\":true}"}});
        let mut lines = vec![json!({"type":"session_meta","payload":{"id":id}})];
        lines.extend(std::iter::repeat_n(oversized, 3));
        lines.extend([request(), accepted]);
        fs::write(&transcript, lines.iter().map(|line| line.to_string() + "\n").collect::<String>()).unwrap();
        let record = json!({"agent":"codex","run_id":"run","conversation_id":id,"transcript":transcript});
        let cache = dir.path().join("cache.json");
        let source = crate::platform::file_identity(&transcript).unwrap();
        fs::write(&cache, json!({"items":{},"offset":0,"inode":[source.device,source.inode],"scan_version":2,
            "initialized":true,"submission_scan_run":"run"}).to_string()).unwrap();
        let expired = Instant::now();
        let mut calls = 0;
        let complete = loop {
            calls += 1;
            assert!(calls <= lines.len(), "indexing made no progress");
            if let Some(cache) = index(&record, &cache, Some(expired)).unwrap() {
                break cache;
            }
        };
        // Every line but the last one ended a call, each oversized line included.
        assert_eq!(calls, lines.len());
        assert_eq!(pending(&record, &complete).len(), 2);
    }
    #[test]
    fn only_complete_native_question_panels_accept_input() {
        let mut cache = json!({"items":{}});
        observe(&json!({}), &mut cache, &request());
        let card = &cache["items"]["codex-async:call_one:0"];
        let screen="Which option?\n\n› 1. One\n  2. Two\n  3. Other (write an answer)\nenter submit   ctrl+] skip\n";
        assert!(visible_panel(screen, card).unwrap());
        assert!(panel_empty(screen, card).unwrap());
        assert!(!panel_empty(
            &screen.replace("Other (write an answer)", "existing draft"),
            card
        )
        .unwrap());
        let freeform = &cache["items"]["codex-async:call_one:1"];
        assert!(panel_empty(
            "Any notes?\n\nType your answer\n\nenter submit   ctrl+] skip",
            freeform
        )
        .unwrap());
        assert!(!panel_empty(
            "Any notes?\n\nMy existing draft\n\nenter submit   ctrl+] skip",
            freeform
        )
        .unwrap());
        for wrong in [
            screen.replace("Which option?", "Different?"),
            screen.replace("Two", "Hidden"),
            screen.replace("ctrl+] skip", "esc cancel"),
        ] {
            assert!(!visible_panel(&wrong, card).unwrap());
        }
    }
    #[test]
    fn codex_0_162_panels_with_progress_and_short_skip_key_accept_input() {
        let mut cache = json!({"items":{}});
        observe(&json!({}), &mut cache, &request());
        let card = &cache["items"]["codex-async:call_one:0"];
        // Captured from Codex 0.162.1 (trailing spaces trimmed).
        let screen = "• Queued follow-up inputs\n\n\n  1 of 2\n  Which option?\n\n  › 1. One\n    2. Two\n    3. Other\n\n  enter submit   ^] skip   shift+→ main prompt   shift+← next question\n\n";
        assert!(visible_panel(screen, card).unwrap());
        assert!(panel_empty(screen, card).unwrap());
        let freeform = &cache["items"]["codex-async:call_one:1"];
        let notes = "• Queued follow-up inputs\n\n\n  Any notes?\n\n  Type your answer\n\n  enter submit   ^] skip   shift+→ main prompt\n\n";
        assert!(panel_empty(notes, freeform).unwrap());
        assert!(!panel_empty(&notes.replace("Type your answer", "Free notes"), freeform).unwrap());
        // Another question's position, or a progress line alone, is never this card.
        for wrong in [
            screen.replace("1 of 2", "2 of 2"),
            screen.replace("  Which option?\n", ""),
            screen.replace("^] skip", "esc cancel"),
        ] {
            assert!(!visible_panel(&wrong, card).unwrap());
        }
        assert!(visible_panel(&notes.replace("  Any notes?", "  2 of 2\n  Any notes?"), freeform).unwrap());
    }
    #[test]
    fn a_scrolled_answer_list_grows_until_the_whole_panel_shows() {
        // Captured from Codex 0.160.1 and 0.162.0 at 80x24, then grown to 80x48.
        let scrolled = [
            include_str!("../../tests/fixtures/codex-question-scrolled-0.160.1.txt"),
            include_str!("../../tests/fixtures/codex-question-scrolled-0.162.0.txt"),
        ];
        let grown = [
            include_str!("../../tests/fixtures/codex-question-complete-0.160.1.txt"),
            include_str!("../../tests/fixtures/codex-question-complete-0.162.0.txt"),
        ];
        let title = ["Before I continue with the release preparation, which distribution channel should this build target, considering that the stable channel requires the full CI matrix, signed artifacts and the owner-triggered publication step, while nightly and dev channels publish automatically after verification?"; 3].join(" ");
        let options: Vec<_> = (1..=5).map(|i| format!("Option {i}: publish to this channel after the complete matrix, package checks and signed asset verification succeed on the exact candidate")).collect();
        let mut cache = json!({"items":{}});
        let arguments = json!({"questions":[{"title":title,"options":options}]}).to_string();
        observe(&json!({}), &mut cache, &json!({"type":"response_item","payload":{"type":"function_call","name":"request_user_input_async","call_id":"call_tall","arguments":arguments}}));
        let card = &cache["items"]["codex-async:call_tall:0"];
        for screen in scrolled {
            assert!(clipped(screen) && !complete(screen));
            assert!(!visible_panel(screen, card).unwrap());
        }
        for screen in grown {
            assert!(complete(screen) && !clipped(screen));
            assert!(visible_panel(screen, card).unwrap());
        }
        // A pasted answer is the selected Other row, its wrapped rows joined;
        // the same text as an option label elsewhere in the panel is not.
        let answer = &options[1];
        for screen in [
            include_str!("../../tests/fixtures/codex-question-other-draft-0.160.1.txt"),
            include_str!("../../tests/fixtures/codex-question-other-draft-0.162.0.txt"),
        ] {
            assert!(!question_terminal::plain(screen).unwrap().contains(answer.as_str()));
            assert_eq!(other_draft(screen, card).unwrap().as_deref(), Some(answer.as_str()));
        }
        for screen in grown {
            assert_eq!(other_draft(screen, card).unwrap(), None);
        }
        // Only Codex's key hints name the scrolled row; elsewhere it is text.
        for other in [
            "› Ask Codex to do anything\n",
            "• Working (3s • esc to interrupt)\n  option 1/6\n",
            "  Press enter to confirm or esc to cancel   option 1/6\n",
        ] {
            assert!(!clipped(other) && !complete(other), "{other}");
        }
    }
    #[test]
    fn working_status_above_a_codex_panel_does_not_change_it() {
        let mut cache = json!({"items":{}});
        observe(&json!({}), &mut cache, &request());
        let card = &cache["items"]["codex-async:call_one:0"];
        // Captured from Codex 0.162.0 with animations, one second apart.
        let panel = "• Queued follow-up inputs\n\n\n  1 of 2\n  Which option?\n\n  › 1. One\n    2. Two\n    3. Other\n\n  enter submit   ^] skip   shift+→ main prompt   shift+← next question\n\n";
        let first = format!("• \x1b[2mWorking\x1b[0m \x1b[2m(2s • \x1b[0;1mesc\x1b[0;2m to interrupt)\x1b[0m\n\n{panel}");
        let second = format!("\x1b[2m◦\x1b[0m \x1b[2mWorking\x1b[0m \x1b[2m(3s • \x1b[0;1mesc\x1b[0;2m to interrupt)\x1b[0m\n\n{panel}");
        assert_ne!(first, second);
        assert!(same_panel(&first, &second, card));
        for changed in [
            second.replace("  › 1. One\n    2. Two", "    1. One\n  › 2. Two"),
            second.replace("3. Other", "3. Draft"),
            second.replace("1 of 2", "2 of 2"),
            second.replace("^] skip", "esc cancel"),
        ] {
            assert!(!same_panel(&first, &changed, card));
        }
    }
}
