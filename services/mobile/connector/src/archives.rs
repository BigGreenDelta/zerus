//! Archive pages (`archives.py` and the archive gates of `connector.py`):
//! the `archive_list` query and page checks, the bounded preview recent
//! archives add to heartbeats for older phones, and the archive identities
//! validated pages admit.
//!
//! An archive is named only by its immutable UUID together with its session
//! name. The local snapshot admits a pair for 45 s; a validated page, an
//! identity-checked read or `hgs archives lookup` admits it for an hour,
//! bounded to the 2,000 most recent pairs, and a confirmed restore or forget
//! revokes it.
use crate::pyvalue::{
    canonical_uuid, compare_numbers, int_literal, int_within, py_eq, py_len, py_uuid, PyError,
    PyNumber,
};
use serde_json::{json, Map, Value};
use std::cmp::Ordering;
use std::collections::HashMap;

/// `archives.MAX_DATE`: 10000-01-01T00:00:00Z.
pub const MAX_DATE: &str = "253402300800";
/// `archives.PREVIEW_BYTES`.
pub const PREVIEW_BYTES: usize = 128 * 1024;
/// How long a paged, read or looked-up archive stays admitted (seconds).
pub const PAGED_SECONDS: f64 = 3600.0;
/// At most this many paged archives stay admitted.
pub const MAX_PAGED: usize = 2000;
pub const PREVIEW_ERROR: &str =
    "Archive preview is unavailable; refresh Archive on an updated client";
pub const UNAVAILABLE: &str = "Update the computer CLI to browse archive pages";
pub const LOOKUP_MISMATCH: &str = "fresh native archive identity did not match";

/// Native archive admission happens before count/position/cursor calculation.
/// Refuse an inconsistent page instead of manufacturing scoped totals by
/// dropping rows from an unscoped native page.
pub fn call(lookup: bool, windows: bool) -> crate::native::Call {
    let mut argv = vec!["archives"];
    if lookup {
        argv.push("lookup");
    }
    argv.push("--json");
    if windows {
        argv.push("--windows-mobile");
    }
    crate::native::Call::new(argv)
}
pub fn supported_page(page: &Value) -> bool {
    page["sessions"].as_array().is_some_and(|rows| {
        rows.iter()
            .all(|row| crate::capabilities::windows_provider(&row["cmd"]))
    })
}

/// The preview keeps these listing fields of a row, and these texts cut to
/// 240 code points.
const PREVIEW_FIELDS: [&str; 26] = [
    "name",
    "cmd",
    "agent",
    "project",
    "tag",
    "run_id",
    "conversation_id",
    "archive_id",
    "archived_at",
    "created",
    "state",
    "tracked",
    "resumable",
    "cwd",
    "cwd_canonical",
    "git_root",
    "git_branch",
    "git_worktree",
    "model",
    "phase",
    "activity",
    "activity_summary",
    "completion_reason",
    "allowed_actions",
    "action_reasons",
    "account_id",
];
const PREVIEW_TEXTS: [&str; 3] = ["prompt", "last_message", "title"];

/// Why a page is refused: Python's ValueError text, or an exception outside
/// its handlers (an integer timestamp beyond the double range, where
/// `math.isfinite` raises OverflowError).
#[derive(Debug, Clone, PartialEq)]
pub enum PageError {
    Value(String),
    Other,
}

fn value(text: &str) -> PageError {
    PageError::Value(text.into())
}

/// The heartbeat's archive descriptor without a preview.
pub fn unavailable() -> Value {
    json!({"schema": 1, "available": false, "loaded": 0, "total": null, "partial": false})
}

fn cursor(text: &Value) -> bool {
    text.as_str().is_some_and(|text| {
        text.len() <= 1024
            && text
                .bytes()
                .all(|b| b.is_ascii_alphanumeric() || b == b'_' || b == b'-')
    })
}

fn int_in(value: Option<&Value>, default: &str, lower: &str, upper: &str) -> Option<String> {
    let digits = match value {
        None => default.to_owned(),
        Some(value) => int_literal(value)?,
    };
    int_within(&digits, lower, upper).then_some(digits)
}

/// `archives.validate(payload)`.
pub fn validate(payload: Option<&Value>) -> Result<(), String> {
    let Some(payload) = payload.and_then(Value::as_object) else {
        return Err("invalid archive query fields".into());
    };
    if payload.keys().any(|key| {
        !matches!(
            key.as_str(),
            "from" | "until" | "limit" | "cursor" | "query"
        )
    }) {
        return Err("invalid archive query fields".into());
    }
    let range = (|| {
        let start = int_in(payload.get("from"), "0", "0", MAX_DATE)?;
        let end = int_in(payload.get("until"), MAX_DATE, "0", MAX_DATE)?;
        int_in(payload.get("limit"), "50", "1", "50")?;
        let order = compare_numbers(&PyNumber::Int(start), &PyNumber::Int(end));
        (order == Some(Ordering::Less)).then_some(())
    })();
    if range.is_none() {
        return Err("invalid archive date range or page size".into());
    }
    if !payload.get("cursor").is_none_or(cursor) {
        return Err("invalid archive cursor".into());
    }
    let search = payload.get("query").map_or(Some(""), Value::as_str);
    if !search.is_some_and(|text| {
        py_len(text) <= 256
            && !text
                .chars()
                .any(|c| (c as u32) < 32 || (127..=159).contains(&(c as u32)))
    }) {
        return Err("invalid archive search".into());
    }
    Ok(())
}

fn number(value: Option<&Value>) -> Option<PyNumber> {
    match value {
        Some(Value::Number(number)) => Some(PyNumber::of(number)),
        _ => None,
    }
}

/// `archives.page(value, payload)`: a native page for this query, checked
/// before anything leaves the computer. Returns the page unchanged.
pub fn page(page: &Value, payload: &Value) -> Result<Value, PageError> {
    validate(Some(payload)).map_err(PageError::Value)?;
    let query = payload.as_object().expect("validated");
    let start = query.get("from").cloned().unwrap_or(json!(0));
    let end = query
        .get("until")
        .cloned()
        .unwrap_or_else(|| Value::Number(MAX_DATE.parse().expect("an integer")));
    let limit: usize = query.get("limit").and_then(Value::as_u64).unwrap_or(50) as usize;
    let search = query.get("query").cloned().unwrap_or(json!(""));
    let invalid = || value("invalid native archive page");
    let page_object = page.as_object().ok_or_else(invalid)?;
    let field = |key: &str| page_object.get(key);
    let int = |key: &str| field(key).and_then(int_literal);
    let sessions = field("sessions").and_then(Value::as_array);
    let total = int("total");
    let next = field("next_cursor");
    let (Some(sessions), Some(total)) = (sessions, total) else {
        return Err(invalid());
    };
    let count = sessions.len().to_string();
    if int("version").as_deref() != Some("1")
        || sessions.len() > limit
        || compare_numbers(&PyNumber::Int(total.clone()), &PyNumber::Int(count.clone()))
            == Some(Ordering::Less)
        || !next.is_some_and(cursor)
        || !field("from").is_some_and(|value| py_eq(value, &start))
        || !field("until").is_some_and(|value| py_eq(value, &end))
        || !field("query").is_some_and(|value| py_eq(value, &search))
        || (sessions.is_empty() && next.and_then(Value::as_str) != Some(""))
    {
        return Err(invalid());
    }
    if page_object.contains_key("previous_cursor") || page_object.contains_key("position") {
        let position = int("position");
        let window = field("previous_cursor").is_some_and(cursor)
            && position.as_ref().is_some_and(|position| {
                int_within(position, "0", &total) && {
                    let end =
                        position.parse::<u128>().unwrap_or(u128::MAX) + sessions.len() as u128;
                    int_within(&end.to_string(), "0", &total)
                }
            });
        if !window {
            return Err(value("invalid native archive window"));
        }
    }
    let (lower, upper) = (
        number(Some(&start)).expect("validated"),
        number(Some(&end)).expect("validated"),
    );
    let mut seen = std::collections::HashSet::new();
    let mut previous: Option<(PyNumber, String)> = None;
    for row in sessions {
        let row = row
            .as_object()
            .filter(|row| row.get("state").and_then(Value::as_str) == Some("archived"))
            .ok_or_else(|| value("invalid archive row"))?;
        let identity = || value("invalid archive identity or date");
        let Some(archive) = row.get("archive_id").and_then(Value::as_str) else {
            return Err(identity());
        };
        py_uuid(archive).map_err(|error| match error {
            PyError::Value(text) => PageError::Value(text),
            _ => PageError::Other,
        })?;
        let name = row.get("name").and_then(Value::as_str);
        let at = match row.get("archived_at") {
            Some(Value::Number(number)) => Some(PyNumber::of(number)),
            _ => None,
        };
        if !canonical_uuid(archive)
            || seen.contains(archive)
            || !name.is_some_and(|name| {
                !name.is_empty()
                    && py_len(name) <= 512
                    && !name.chars().any(|c| (c as u32) < 32 || c as u32 == 127)
            })
        {
            return Err(identity());
        }
        let Some(at) = at else {
            return Err(identity());
        };
        // `math.isfinite` of an integer beyond the double range raises.
        if matches!(at, PyNumber::Int(_)) && at.as_f64().is_none() {
            return Err(PageError::Other);
        }
        if !at.is_finite()
            || compare_numbers(&lower, &at) == Some(Ordering::Greater)
            || compare_numbers(&at, &upper) != Some(Ordering::Less)
        {
            return Err(identity());
        }
        if let Some((before_at, before_archive)) = &previous {
            let order = compare_numbers(&at, before_at)
                .unwrap_or(Ordering::Equal)
                .then_with(|| archive.cmp(before_archive.as_str()));
            if order != Ordering::Less {
                return Err(value("archive page is out of order"));
            }
        }
        previous = Some((at, archive.to_owned()));
        seen.insert(archive.to_owned());
    }
    Ok(page.clone())
}

/// `archives.preview(value)`: the most recent archived rows within 128 KiB
/// and the heartbeat's descriptor of them.
pub fn preview(value: &Value) -> Result<(Vec<Value>, Value), PageError> {
    page(value, &json!({}))?;
    let sessions = value["sessions"].as_array().expect("checked");
    let mut rows = Vec::new();
    let mut size = 2;
    for raw in sessions.iter().filter_map(Value::as_object) {
        let mut row: Map<String, Value> = raw
            .iter()
            .filter(|(key, _)| PREVIEW_FIELDS.contains(&key.as_str()))
            .map(|(key, value)| (key.clone(), value.clone()))
            .collect();
        for key in PREVIEW_TEXTS {
            if let Some(Value::String(text)) = raw.get(key) {
                row.insert(key.into(), Value::String(text.chars().take(240).collect()));
            }
        }
        let row = Value::Object(row);
        let encoded = zerus_relay::json::size(&row, usize::MAX).unwrap_or(usize::MAX - 1) + 1;
        if size + encoded > PREVIEW_BYTES {
            break;
        }
        rows.push(row);
        size += encoded;
    }
    let total = value["total"].clone();
    let partial = int_literal(&total).is_some_and(|total| {
        compare_numbers(
            &PyNumber::Int(rows.len().to_string()),
            &PyNumber::Int(total),
        ) == Some(Ordering::Less)
    });
    let meta = json!({"schema": 1, "available": true, "total": total, "loaded": rows.len(), "partial": partial});
    Ok((rows, meta))
}

/// `remember_archives`: admit these pairs now, dropping those older than an
/// hour and keeping the 2,000 most recent.
pub fn remember(
    paged: &mut HashMap<(String, String), f64>,
    targets: impl IntoIterator<Item = (String, String)>,
    now: f64,
) {
    paged.retain(|_, at| now - *at <= PAGED_SECONDS);
    for target in targets {
        paged.insert(target, now);
    }
    if paged.len() > MAX_PAGED {
        let mut recent: Vec<_> = paged.drain().collect();
        recent.sort_by(|a, b| b.1.total_cmp(&a.1));
        recent.truncate(MAX_PAGED);
        paged.extend(recent);
    }
}

/// The archive pairs of a validated page.
pub fn targets(page: &Value) -> Vec<(String, String)> {
    page["sessions"]
        .as_array()
        .into_iter()
        .flatten()
        .filter_map(|row| {
            Some((
                row.get("name")?.as_str()?.to_owned(),
                row.get("archive_id")?.as_str()?.to_owned(),
            ))
        })
        .collect()
}

/// The `hgs archives lookup` answer for exactly this target.
pub fn lookup_matches(detail: &Value, session: &str, archive: &str, payload: &Value) -> bool {
    let Some(detail) = detail.as_object() else {
        return false;
    };
    int_literal(detail.get("version").unwrap_or(&Value::Null)).as_deref() == Some("1")
        && detail.get("name").and_then(Value::as_str) == Some(session)
        && detail.get("archive_id").and_then(Value::as_str) == Some(archive)
        && [
            ("expected_run_id", "run_id"),
            ("expected_conversation_id", "conversation_id"),
        ]
        .iter()
        .all(|(key, field)| match payload.get(*key) {
            Some(expected) => detail
                .get(*field)
                .is_some_and(|value| py_eq(value, expected)),
            None => true,
        })
}

#[cfg(test)]
mod tests {
    use super::*;

    fn row(archive: &str, at: f64) -> Value {
        json!({"state": "archived", "name": "codex/example/archived", "archive_id": archive, "archived_at": at})
    }
    fn listed(sessions: Vec<Value>, total: u64) -> Value {
        json!({"version": 1, "sessions": sessions, "total": total, "next_cursor": "", "from": 0,
               "until": 253402300800u64, "query": ""})
    }
    const A: &str = "00000000-0000-4000-8000-00000000000a";
    const B: &str = "00000000-0000-4000-8000-00000000000b";

    #[test]
    fn queries_follow_the_python_validator() {
        assert_eq!(validate(Some(&json!({}))), Ok(()));
        assert_eq!(
            validate(Some(
                &json!({"from": 1, "until": 2, "limit": 50, "cursor": "a_-", "query": "x"})
            )),
            Ok(())
        );
        for (payload, error) in [
            (json!([]), "invalid archive query fields"),
            (json!({"extra": 1}), "invalid archive query fields"),
            (
                json!({"from": 2, "until": 2}),
                "invalid archive date range or page size",
            ),
            (
                json!({"limit": 51}),
                "invalid archive date range or page size",
            ),
            (
                json!({"limit": true}),
                "invalid archive date range or page size",
            ),
            (
                json!({"from": 1.0}),
                "invalid archive date range or page size",
            ),
            (json!({"cursor": "a b"}), "invalid archive cursor"),
            (json!({"query": "a\u{85}"}), "invalid archive search"),
        ] {
            assert_eq!(validate(Some(&payload)), Err(error.into()), "{payload}");
        }
    }

    #[test]
    fn pages_must_be_ordered_unique_and_within_the_query() {
        let page = listed(
            vec![
                row(B, 20.0),
                row(A, 20.0),
                row(B.replace('b', "c").as_str(), 10.0),
            ],
            3,
        );
        assert!(super::page(&page, &json!({})).is_ok());
        let repeated = listed(vec![row(A, 20.0), row(A, 10.0)], 2);
        assert_eq!(
            super::page(&repeated, &json!({})),
            Err(value("invalid archive identity or date"))
        );
        let ascending = listed(vec![row(A, 10.0), row(B, 20.0)], 2);
        assert_eq!(
            super::page(&ascending, &json!({})),
            Err(value("archive page is out of order"))
        );
        let badly = listed(vec![row("not-a-uuid", 10.0)], 1);
        assert_eq!(
            super::page(&badly, &json!({})),
            Err(value("badly formed hexadecimal UUID string"))
        );
        let outside = listed(vec![row(A, 10.0)], 1);
        assert_eq!(
            super::page(&outside, &json!({"from": 11})),
            Err(value("invalid native archive page"))
        );
        let huge = listed(
            vec![json!({"state": "archived", "name": "n", "archive_id": A,
                                       "archived_at": Value::Number(format!("1{}", "0".repeat(400)).parse().unwrap())})],
            1,
        );
        assert_eq!(super::page(&huge, &json!({})), Err(PageError::Other));
        let mut window = listed(vec![row(A, 10.0)], 2);
        window["previous_cursor"] = json!("");
        window["position"] = json!(1);
        assert!(super::page(&window, &json!({})).is_ok());
        window["position"] = json!(2);
        assert_eq!(
            super::page(&window, &json!({})),
            Err(value("invalid native archive window"))
        );
    }

    #[test]
    fn previews_keep_listing_fields_within_their_budget() {
        let mut first = row(A, 20.0);
        first["prompt"] = json!("p".repeat(300));
        first["private"] = json!("kept out");
        let (rows, meta) = preview(&listed(vec![first, row(B, 10.0)], 5)).unwrap();
        assert_eq!(rows.len(), 2);
        assert_eq!(rows[0]["prompt"].as_str().unwrap().len(), 240);
        assert!(rows[0].get("private").is_none());
        assert_eq!(
            meta,
            json!({"schema": 1, "available": true, "total": 5, "loaded": 2, "partial": true})
        );
    }

    #[test]
    fn remembered_archives_expire_and_stay_bounded() {
        let mut paged = HashMap::new();
        remember(&mut paged, [("s".into(), A.into())], 0.0);
        remember(&mut paged, [("s".into(), B.into())], 3601.0);
        assert_eq!(paged.len(), 1);
        remember(
            &mut paged,
            (0..2100).map(|i| ("s".to_owned(), format!("{i}"))),
            3602.0,
        );
        assert_eq!(paged.len(), MAX_PAGED);
    }
}
