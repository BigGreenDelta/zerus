//! `history.bound_inspection`: an inspection whose completed receipt would
//! exceed the relay's result bound loses whole oldest history rows, the
//! newest own message last, and says so; every other field keeps its exact
//! value. Sizes are Python's plain `json.dumps` with ASCII escapes, as the
//! upload measures them.
use crate::pyvalue::{compare_numbers, hashable, PyError, PyNumber, PyResult};
use serde_json::{json, Map, Value};
use std::cmp::Ordering;
use std::collections::HashSet;
use zerus_relay::json;

pub const STREAMS: [&str; 4] = [
    "events",
    "message_events",
    "provider_messages",
    "attachment_messages",
];
const USER_TYPES: [&str; 5] = [
    "UserPromptSubmit",
    "UserPromptQueued",
    "UserMessage",
    "TurnStarted",
    "QuestionAnswered",
];
pub const METADATA_TOO_LARGE: &str =
    "native inspection metadata exceeds relay size limit; required disclosures were not cropped";

/// `len(json.dumps(value, allow_nan=False).encode())`. A number beyond the
/// double range is Python's ValueError, which reaches no receipt unchanged.
fn encoded(value: &Value) -> PyResult<usize> {
    json::python_default_len(value, true).map_err(|_| PyError::Type)
}

/// `{"state": "completed", "result": ` and `, "error": null}` around a result.
const RECEIPT_FRAME: usize = 49;

/// The size of `{"state": "completed", "result": result, "error": None}`.
pub fn completed_size(result: &Map<String, Value>) -> PyResult<usize> {
    // Measured member by member: a result can be megabytes, never copied.
    let mut size = 2 + 2 * result.len().saturating_sub(1);
    for (key, value) in result {
        size += encoded(&Value::String(key.clone()))? + 2 + encoded(value)?;
    }
    Ok(size + RECEIPT_FRAME)
}

/// A row's `at`, as Python reads it: a finite int or float, otherwise 0.
fn at(row: &Value) -> PyResult<PyNumber> {
    let zero = PyNumber::Int("0".into());
    let Some(Value::Number(number)) = row.as_object().and_then(|row| row.get("at")) else {
        return Ok(zero);
    };
    let number = PyNumber::of(number);
    match &number {
        // math.isfinite() of an int beyond the double range overflows.
        PyNumber::Int(_) if number.as_f64().is_none() => Err(PyError::Type),
        PyNumber::Float(value) if !value.is_finite() => Ok(zero),
        _ => Ok(number),
    }
}

#[derive(Clone)]
struct Item {
    at: PyNumber,
    stream: usize,
    index: usize,
    length: usize,
}

impl Item {
    fn same(&self, other: &Item) -> bool {
        self.stream == other.stream && self.index == other.index
    }
}

/// `(at, stream == "message_events")` comparison for the newest own message.
fn newer(item: &Item, latest: &Item) -> bool {
    match compare_numbers(&item.at, &latest.at) {
        Some(Ordering::Greater) => true,
        Some(Ordering::Equal) => {
            STREAMS[item.stream] == "message_events" && STREAMS[latest.stream] != "message_events"
        }
        _ => false,
    }
}

/// Bounds `result` to `maximum` completed-receipt bytes.
pub fn bound_inspection(
    result: Map<String, Value>,
    maximum: usize,
) -> PyResult<Map<String, Value>> {
    if completed_size(&result)? <= maximum {
        return Ok(result);
    }
    let mut value = result;
    let mut rows: Vec<Item> = Vec::new();
    let mut latest: Option<Item> = None;
    for (stream, name) in STREAMS.iter().enumerate() {
        let Some(Value::Array(items)) = value.get(*name) else {
            continue;
        };
        for (index, row) in items.iter().enumerate() {
            let item = Item {
                at: at(row)?,
                stream,
                index,
                length: encoded(row)?,
            };
            if let Some(object) = row.as_object() {
                // `row.get("type") in USER_TYPES` hashes the value.
                let kind = match object.get("type") {
                    Some(kind) => hashable(kind)?.as_str(),
                    None => None,
                };
                let own = match object.get("agent_id") {
                    None => true,
                    Some(agent) => agent
                        .as_str()
                        .is_some_and(|agent| agent.is_empty() || agent == "main"),
                };
                if kind.is_some_and(|kind| USER_TYPES.contains(&kind))
                    && own
                    && latest.as_ref().is_none_or(|latest| newer(&item, latest))
                {
                    latest = Some(item.clone());
                }
            }
            rows.push(item);
        }
    }
    let mut omitted = [0usize; 4];
    value.insert("mobile_history_truncated".into(), json!(true));
    value.insert("mobile_history_omitted".into(), omitted_value(&omitted));
    let mut size = completed_size(&value)? as i64;
    let mut remaining: [usize; 4] = [0; 4];
    for (stream, name) in STREAMS.iter().enumerate() {
        if let Some(Value::Array(items)) = value.get(*name) {
            remaining[stream] = items.len();
        }
    }
    // Sort key `(item == latest, at, stream, index)`: the newest own message
    // goes last; ties in `at` fall to the stream name, then the position.
    rows.sort_by(|a, b| {
        let a_latest = latest.as_ref().is_some_and(|latest| latest.same(a));
        let b_latest = latest.as_ref().is_some_and(|latest| latest.same(b));
        a_latest
            .cmp(&b_latest)
            .then_with(|| compare_numbers(&a.at, &b.at).unwrap_or(Ordering::Equal))
            .then_with(|| STREAMS[a.stream].cmp(STREAMS[b.stream]))
            .then_with(|| a.index.cmp(&b.index))
    });
    // Sets, as Python's: a large listing loses most of its rows.
    let mut removed: [HashSet<usize>; 4] = Default::default();
    for item in &rows {
        if size <= maximum as i64 {
            break;
        }
        let before = omitted[item.stream].to_string().len() as i64;
        omitted[item.stream] += 1;
        size += omitted[item.stream].to_string().len() as i64 - before;
        size -= item.length as i64 + if remaining[item.stream] > 1 { 2 } else { 0 };
        remaining[item.stream] -= 1;
        removed[item.stream].insert(item.index);
    }
    for (stream, indices) in removed.iter().enumerate() {
        if indices.is_empty() {
            continue;
        }
        if let Some(Value::Array(items)) = value.get_mut(STREAMS[stream]) {
            let kept: Vec<Value> = std::mem::take(items)
                .into_iter()
                .enumerate()
                .filter(|(index, _)| !indices.contains(index))
                .map(|(_, row)| row)
                .collect();
            *items = kept;
        }
    }
    value.insert("mobile_history_omitted".into(), omitted_value(&omitted));
    if completed_size(&value)? > maximum {
        return Err(PyError::value(METADATA_TOO_LARGE));
    }
    Ok(value)
}

fn omitted_value(omitted: &[usize; 4]) -> Value {
    let mut map = Map::new();
    for (stream, name) in STREAMS.iter().enumerate() {
        map.insert((*name).into(), json!(omitted[stream]));
    }
    Value::Object(map)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_receipt_frame_is_python_sized() {
        let result = json!({"name": "s", "events": [1]});
        let receipt = json!({"state": "completed", "result": result, "error": null});
        assert_eq!(
            completed_size(result.as_object().unwrap()).unwrap(),
            json::python_default_len(&receipt, true).unwrap()
        );
    }

    #[test]
    fn opencode_delivery_streaming_and_question_fields_pass_through_unchanged() {
        let queued = json!({"type": "UserPromptSubmit", "delivery_state": "not_delivered",
                            "message_id": "msg_01", "detail": "queued text", "at": 1000});
        let streaming = json!({"type": "AssistantMessage", "streaming": true, "at": 1001,
                               "detail": "partial"});
        let card = json!({"kind": "opencode_form", "can_answer": false, "answerable": false,
            "unavailable_reason": "Answer this form in OpenCode",
            "questions": [{"id": "q1", "input_kind": "text", "min_length": 1, "max_length": 80,
                           "min_items": 1, "max_items": 2, "default_option_ids": ["a"],
                           "options": [{"id": "a", "label": "A", "description": ""}]}]});
        let build = |padding: usize| {
            let mut rows = vec![queued.clone(), streaming.clone()];
            rows.extend((0..padding).map(
                |index| json!({"type": "AssistantMessage", "at": index, "detail": "x".repeat(100)}),
            ));
            json!({"name": "s", "events": rows, "pending_questions": [card.clone()]})
                .as_object()
                .unwrap()
                .clone()
        };
        // Within the bound nothing changes at all.
        let small = build(0);
        assert_eq!(bound_inspection(small.clone(), 1 << 20).unwrap(), small);
        // Over the bound whole oldest rows go; kept rows and cards are exact.
        let bounded = bound_inspection(build(40), 3000).unwrap();
        assert_eq!(bounded["pending_questions"], json!([card]));
        let events = bounded["events"].as_array().unwrap();
        assert!(events.len() < 42);
        assert!(events.contains(&queued) && events.contains(&streaming));
    }
}
