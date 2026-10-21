//! Host-owned notification preferences. These are neither read marks nor
//! permission to act on an agent. Only this file is written by this command.
use super::*;
use serde::{Deserialize, Serialize};
use std::collections::BTreeSet;
use std::fs;
use std::io::Read;

const MAX_REVISION: u64 = i64::MAX as u64;
const MAX_CONVERSATIONS: usize = 8;
const MAX_FILE: u64 = 16 * 1024 * 1024;
const MAX_OPTIONAL: usize = 64;
const MAX_CHILDREN: usize = 256;
const MAX_PREFERENCE_WIRE_BYTES: usize = 384 * 1024;
const USAGE: &str = "usage: hgs notifications <session> --json (JSON stdin: run_id, conversation_id, muted, expected_revision)";

#[derive(Clone, Debug, Deserialize, Serialize, PartialEq)]
#[serde(deny_unknown_fields)]
struct Attention {
    phase: String,
    request_id: String,
    started_at: Option<f64>,
}

#[derive(Clone, Debug, Deserialize, Serialize, PartialEq)]
#[serde(deny_unknown_fields)]
struct MainAttention {
    run_id: String,
    phase: String,
    request_id: String,
    started_at: Option<f64>,
}

#[derive(Clone, Debug, Default, Deserialize, Serialize, PartialEq)]
#[serde(deny_unknown_fields)]
struct Suppressed {
    reply_id: Option<String>,
    attention_signature: Option<String>,
    optional_question_ids: Vec<String>,
    main_attention: Option<MainAttention>,
    child_attention: BTreeMap<String, Attention>,
}

#[derive(Clone, Debug, Deserialize, Serialize, PartialEq)]
#[serde(deny_unknown_fields)]
struct Preference {
    conversation_id: String,
    muted: bool,
    revision: u64,
    suppressed: Suppressed,
}

impl Preference {
    fn new(conversation: &str) -> Self {
        Self {
            conversation_id: conversation.into(),
            muted: false,
            revision: 0,
            suppressed: Suppressed::default(),
        }
    }
}

#[derive(Clone, Debug, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub(super) struct Store {
    version: u8,
    sessions: BTreeMap<String, BTreeMap<String, Preference>>,
}

impl Default for Store {
    fn default() -> Self {
        Self {
            version: 1,
            sessions: BTreeMap::new(),
        }
    }
}

#[derive(Debug, Deserialize)]
#[serde(deny_unknown_fields)]
struct Request {
    run_id: String,
    conversation_id: String,
    muted: bool,
    expected_revision: u64,
}

fn identity(value: &str) -> bool {
    !value.is_empty() && value.len() <= 256 && !value.chars().any(char::is_control)
}

fn owner_identity(value: &str) -> bool {
    !value.is_empty() && value.len() <= 2048 && !value.chars().any(char::is_control)
}

fn parse(bytes: &[u8]) -> Result<Request> {
    let request: Request =
        serde_json::from_slice(bytes).map_err(|_| "invalid notification preference request")?;
    if !identity(&request.run_id)
        || request.run_id.len() > 128
        || !identity(&request.conversation_id)
        || request.expected_revision > MAX_REVISION
    {
        return Err("invalid notification preference identity or revision".into());
    }
    Ok(request)
}

fn path() -> PathBuf {
    root().join("notifications.json")
}

fn read_store(path: &Path) -> Result<Store> {
    let file = match fs::File::open(path) {
        Ok(file) => file,
        Err(error) if error.kind() == io::ErrorKind::NotFound => return Ok(Store::default()),
        Err(error) => return Err(error.to_string()),
    };
    let mut bytes = Vec::new();
    file.take(MAX_FILE + 1)
        .read_to_end(&mut bytes)
        .map_err(|e| e.to_string())?;
    if bytes.len() as u64 > MAX_FILE {
        return Err("notification preference file exceeds limits".into());
    }
    let store: Store = serde_json::from_slice(&bytes)
        .map_err(|e| format!("invalid notification preferences: {e}"))?;
    if store.version != 1
        || store.sessions.iter().any(|(owner, conversations)| {
            !owner_identity(owner)
                || conversations.len() > MAX_CONVERSATIONS
                || conversations.iter().any(|(conversation, preference)| {
                    !identity(conversation)
                        || preference.conversation_id != *conversation
                        || preference.revision > MAX_REVISION
                        || !valid_baseline(&preference.suppressed)
                        || !preference_fits(preference)
                })
        })
    {
        return Err("invalid notification preference storage".into());
    }
    Ok(store)
}

fn valid_attention(attention: &Attention) -> bool {
    (!attention.request_id.is_empty() || attention.started_at.is_some())
        && attention.phase.len() <= 256
        && !attention.phase.chars().any(char::is_control)
        && attention.request_id.len() <= 65536
        && !attention
            .request_id
            .chars()
            .any(|c| c.is_control() && c != '\n')
        && attention
            .started_at
            .is_none_or(|at| at.is_finite() && at > 0.0)
}

fn valid_baseline(baseline: &Suppressed) -> bool {
    baseline.reply_id.as_deref().is_none_or(identity)
        && baseline.attention_signature.as_deref().is_none_or(|s| {
            s.len() == 64
                && s.bytes()
                    .all(|c| c.is_ascii_digit() || (b'a'..=b'f').contains(&c))
        })
        && baseline.optional_question_ids.len() <= MAX_OPTIONAL
        && baseline.optional_question_ids.iter().all(|id| identity(id))
        && baseline
            .optional_question_ids
            .windows(2)
            .all(|pair| pair[0] < pair[1])
        && baseline.main_attention.as_ref().is_none_or(|main| {
            identity(&main.run_id)
                && main.run_id.len() <= 128
                && valid_attention(&Attention {
                    phase: main.phase.clone(),
                    request_id: main.request_id.clone(),
                    started_at: main.started_at,
                })
        })
        && baseline.child_attention.len() <= MAX_CHILDREN
        && baseline
            .child_attention
            .iter()
            .all(|(id, attention)| identity(id) && valid_attention(attention))
}

/// Upper bound when a transport ASCII-escapes JSON strings. The compact
/// serializer has already escaped ASCII quotes, backslashes and controls.
fn preference_fits(preference: &Preference) -> bool {
    serde_json::to_string(preference).is_ok_and(|serialized| {
        serialized
            .chars()
            .map(|ch| {
                if ch.is_ascii() {
                    1
                } else if u32::from(ch) <= 0xffff {
                    6
                } else {
                    12
                }
            })
            .sum::<usize>()
            <= MAX_PREFERENCE_WIRE_BYTES
    })
}

pub(super) fn load() -> Option<Store> {
    // Corruption is unknown, never a fabricated unmuted default.
    read_store(&path()).ok()
}

fn preference(store: &Store, owner: &str, conversation: &str) -> Preference {
    store
        .sessions
        .get(owner)
        .and_then(|conversations| conversations.get(conversation))
        .cloned()
        .unwrap_or_else(|| Preference::new(conversation))
}

pub(super) fn enrich(session: &mut Value, store: Option<&Store>, owner: &str) {
    let conversation = string(session, "conversation_id");
    let value = store
        .filter(|_| owner_identity(owner) && identity(conversation))
        .map(|store| json!(preference(store, owner, conversation)))
        .unwrap_or(Value::Null);
    session["notification_preferences"] = value;
}

/// Preserve the preference owner when a native provider renames its binding.
pub(super) fn anchor(record: &mut Value) {
    if string(record, "journal_name").is_empty() {
        record["journal_name"] = record["name"].clone();
    }
}

fn positive(value: &Value) -> Option<f64> {
    value.as_f64().filter(|at| at.is_finite() && *at > 0.0)
}

fn attention(session: &Value, phase: &str, child: bool) -> Attention {
    let request = string(session, "attention_id");
    Attention {
        phase: phase.into(),
        request_id: if child && request.is_empty() {
            string(session, "question_id")
        } else {
            request
        }
        .into(),
        started_at: positive(&session["attention_started"])
            .or_else(|| child.then(|| positive(&session["last_event_at"])).flatten()),
    }
}

/// Capture identities, not read acknowledgements. An unknown/over-limit
/// question set is a refusal; dropping identities could replay an old alert.
fn baseline(session: &Value, questions: Option<&[Value]>) -> Result<Suppressed> {
    // The v1 native edge baseline has no recovery-job identity. Refuse an
    // unmute rather than call that independent failed channel fully captured.
    if matches!(
        string(&session["recovery"], "state"),
        "uncertain" | "blocked" | "exhausted"
    ) {
        return Err("baseline_unavailable".into());
    }
    let questions = questions.ok_or("baseline_unavailable")?;
    let mut optional = BTreeSet::new();
    for card in questions.iter().filter(|card| {
        card["source"] == "codex_async"
            && card["optional"] == true
            && card["answer_delivery"]["status"] != "submitted"
    }) {
        let id = string(card, "question_id");
        if !identity(id) {
            return Err("baseline_unavailable".into());
        }
        optional.insert(id.to_owned());
        if optional.len() > MAX_OPTIONAL {
            return Err("baseline_unavailable".into());
        }
    }
    let mut children = BTreeMap::new();
    if string(session, "subagent_source") != "hook_profiles" {
        for (id, child) in session["subagents"].as_object().into_iter().flatten() {
            let phase = child["display_state"]
                .as_str()
                .filter(|s| !s.is_empty())
                .unwrap_or_else(|| string(child, "state"));
            if ["approval", "input", "attention"].contains(&phase) {
                children.insert(id.clone(), attention(child, phase, true));
                if children.len() > MAX_CHILDREN {
                    return Err("baseline_unavailable".into());
                }
            }
        }
    }
    let phase = string(session, "phase");
    let main = ["approval", "input", "error"]
        .contains(&phase)
        .then(|| MainAttention {
            run_id: string(session, "run_id").into(),
            phase: phase.into(),
            request_id: string(session, "attention_id").into(),
            started_at: positive(&session["attention_started"]),
        });
    let result = Suppressed {
        reply_id: session["reply_id"]
            .as_str()
            .filter(|id| !id.is_empty())
            .map(str::to_owned),
        attention_signature: read_marks::signature(session, Some(questions)),
        optional_question_ids: optional.into_iter().collect(),
        main_attention: main,
        child_attention: children,
    };
    if !valid_baseline(&result) {
        return Err("baseline_unavailable".into());
    }
    Ok(result)
}

/// Pure CAS reducer. The baseline closure is never called for mute, no-op,
/// conflict or stale identity. Failure leaves the supplied store untouched.
fn update(
    store: &mut Store,
    owner: &str,
    current: &Value,
    request: &Request,
    capture: impl FnOnce() -> Result<Suppressed>,
) -> (i32, Value, bool) {
    let run = string(current, "run_id");
    let conversation = string(current, "conversation_id");
    let previous = preference(store, owner, conversation);
    let refusal = |error: &str| {
        (
            1,
            json!({"ok":false,"error":error,"run_id":run,"conversation_id":conversation,"preferences":previous}),
            false,
        )
    };
    if request.run_id != run || request.conversation_id != conversation {
        return refusal("stale_identity");
    }
    if request.expected_revision != previous.revision {
        return refusal("conflict");
    }
    if request.muted == previous.muted {
        return (
            0,
            json!({"ok":true,"applied":false,"run_id":run,"conversation_id":conversation,"preferences":previous}),
            false,
        );
    }
    if previous.revision == MAX_REVISION {
        return refusal("revision_exhausted");
    }
    if store.sessions.get(owner).is_some_and(|entries| {
        !entries.contains_key(conversation) && entries.len() >= MAX_CONVERSATIONS
    }) {
        return refusal("capacity");
    }
    let suppressed = if request.muted {
        previous.suppressed.clone()
    } else {
        match capture() {
            Ok(value) => value,
            Err(_) => return refusal("baseline_unavailable"),
        }
    };
    let next = Preference {
        conversation_id: conversation.into(),
        muted: request.muted,
        revision: previous.revision + 1,
        suppressed,
    };
    if !preference_fits(&next) {
        return refusal("baseline_unavailable");
    }
    store
        .sessions
        .entry(owner.into())
        .or_default()
        .insert(conversation.into(), next.clone());
    (
        0,
        json!({"ok":true,"applied":true,"run_id":run,"conversation_id":conversation,"preferences":next}),
        true,
    )
}

/// The raw binding only: never read_marks::current, archive reconciliation,
/// native adoption, approval fitting, input, acknowledgement or lifecycle.
fn current(name: &str) -> Result<Option<Value>> {
    if opencode::exists(name) {
        return opencode::binding(name).map(Some);
    }
    if dsh::exists(name) {
        return dsh::binding(name).map(Some);
    }
    let bytes = match fs::read(record_path(name)) {
        Ok(bytes) => bytes,
        Err(error) if error.kind() == io::ErrorKind::NotFound => return Ok(None),
        Err(error) => return Err(error.to_string()),
    };
    if bytes.len() > 1024 * 1024 {
        return Err("binding exceeds limits".into());
    }
    let record: Value = serde_json::from_slice(&bytes).map_err(|e| e.to_string())?;
    if record["version"] != 1
        || string(&record, "name") != name
        || !AGENTS.contains(&string(&record, "agent"))
    {
        return Err("invalid notification session binding".into());
    }
    if !identity(string(&record, "run_id"))
        || string(&record, "run_id").len() > 128
        || !identity(string(&record, "conversation_id"))
        || !string(&record, "archive_id").is_empty()
        || record["state"] == "archived"
    {
        return Ok(None);
    }
    if archive_catalog::contains_existing(&record)? {
        return Ok(None);
    }
    let panes = live()?;
    if panes.contains_key(name) && !matches(&record, panes.get(name)) {
        return Ok(None);
    }
    Ok(Some(record))
}

fn capture(record: &Value) -> Result<Suppressed> {
    let (view, questions) = match string(record, "backend") {
        "opencode" => opencode::notification_snapshot(record)?,
        "dsh" => dsh::notification_snapshot(record)?,
        _ => {
            let live = process_alive(record) && record["paused"] != true;
            let mut view = journal::notification_summary(record, live)?;
            if !live {
                view["state"] = json!("stopped");
            }
            let questions = if live {
                questions::current_until(record, read_marks::question_budget())?
            } else {
                Some(Vec::new())
            };
            (view, questions)
        }
    };
    baseline(&view, questions.as_deref())
}

fn persist(path: &Path, store: &Store) -> Result<()> {
    if store
        .sessions
        .values()
        .flat_map(|entries| entries.values())
        .any(|preference| !preference_fits(preference))
    {
        return Err("notification preference exceeds wire limits".into());
    }
    let serialized = serde_json::to_string(store).map_err(|e| e.to_string())?;
    if serialized.len() as u64 >= MAX_FILE {
        return Err("notification preference file exceeds limits".into());
    }
    atomic(path, &format!("{serialized}\n"))
}

pub(super) fn dispatch(args: &[String]) -> Result<i32> {
    let [name, flag] = args else {
        return Err(USAGE.into());
    };
    if flag != "--json" || !owner_identity(name) || name.starts_with(['-', '@']) {
        return Err(USAGE.into());
    }
    let mut bytes = Vec::new();
    io::stdin()
        .take(8193)
        .read_to_end(&mut bytes)
        .map_err(|e| e.to_string())?;
    if bytes.len() > 8192 {
        return Err("notification request exceeds limits".into());
    }
    let request = parse(&bytes)?;
    let _preference_guard = lock(Some(&root().join("notifications.lock")))?;
    // Same lock files as their writers, without the default lock's rename
    // recovery. An interrupted rename is refused, never completed here.
    let _native_guard = if name.starts_with("opencode/") {
        Some(opencode::lock_bindings()?)
    } else {
        None
    };
    let binding_lock = if name.starts_with("dsh/") {
        root().join("dsh/bindings.lock")
    } else {
        root().join(".lock")
    };
    let _binding_guard = lock(Some(&binding_lock))?;
    if fs::read_dir(root().join("renames"))
        .ok()
        .is_some_and(|entries| {
            entries
                .flatten()
                .any(|entry| entry.path().extension().is_some_and(|ext| ext == "json"))
        })
    {
        return Err(
            "Session rename recovery is pending; refresh before changing notifications".into(),
        );
    }
    let Some(record) = current(name)? else {
        println!("{}", json!({"ok":false,"error":"not_found"}));
        return Ok(1);
    };
    if !identity(string(&record, "run_id")) || !identity(string(&record, "conversation_id")) {
        return Err("invalid notification identity".into());
    }
    let owner = journal::journal_name(&record);
    if !owner_identity(owner) {
        return Err("invalid notification owner".into());
    }
    let mut store = read_store(&path())?;
    let (code, output, changed) = update(&mut store, owner, &record, &request, || capture(&record));
    if changed {
        persist(&path(), &store)?;
    }
    println!("{output}");
    Ok(code)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn session() -> Value {
        json!({"name":"codex/project/review","run_id":"run-1","conversation_id":"conversation-1",
            "state":"running","process_state":"running","phase":"input","attention_id":"request-1",
            "attention_started":10.5,"reply_id":"31:9.0","subagent_source":"hooks","subagents":{
                "worker":{"state":"working","display_state":"approval","attention_id":"child-1","last_event_at":11.0}}})
    }

    fn request(muted: bool, revision: u64) -> Request {
        parse(json!({"run_id":"run-1","conversation_id":"conversation-1","muted":muted,"expected_revision":revision}).to_string().as_bytes()).unwrap()
    }

    fn unavailable() -> Result<Suppressed> {
        Err("question index unknown".into())
    }

    #[test]
    fn request_schema_rejects_missing_extra_coerced_and_unbounded_values() {
        let valid = json!({"run_id":"run-1","conversation_id":"conversation-1","muted":true,"expected_revision":0});
        assert!(parse(valid.to_string().as_bytes()).is_ok());
        let mut bounded_run = valid.clone();
        bounded_run["run_id"] = json!("r".repeat(128));
        assert!(parse(bounded_run.to_string().as_bytes()).is_ok());
        bounded_run["run_id"] = json!("r".repeat(129));
        assert!(parse(bounded_run.to_string().as_bytes()).is_err());
        assert!(parse(br#"{"run_id":"r","conversation_id":"c","muted":true,"muted":false,"expected_revision":0}"#).is_err());
        for key in ["run_id", "conversation_id", "muted", "expected_revision"] {
            let mut missing = valid.clone();
            missing.as_object_mut().unwrap().remove(key);
            assert!(parse(missing.to_string().as_bytes()).is_err(), "{key}");
        }
        for (key, value) in [
            ("extra", json!(false)),
            ("muted", json!("true")),
            ("muted", Value::Null),
            ("expected_revision", json!(-1)),
            ("expected_revision", json!(0.5)),
            ("expected_revision", json!(MAX_REVISION + 1)),
            ("run_id", json!("")),
            ("conversation_id", json!("c\n")),
            ("conversation_id", json!("x".repeat(257))),
        ] {
            let mut invalid = valid.clone();
            invalid[key] = value;
            assert!(
                parse(invalid.to_string().as_bytes()).is_err(),
                "{key}: {invalid}"
            );
        }
    }

    #[test]
    fn mute_works_without_a_baseline_and_same_value_does_not_capture_or_advance() {
        let mut store = Store::default();
        let (code, first, changed) = update(
            &mut store,
            "owner",
            &session(),
            &request(true, 0),
            unavailable,
        );
        assert_eq!(code, 0);
        assert!(changed);
        assert_eq!(first["preferences"]["revision"], 1);
        assert_eq!(
            first["preferences"]["suppressed"]["optional_question_ids"],
            json!([])
        );
        let before = serde_json::to_value(&store).unwrap();
        let (code, noop, changed) =
            update(&mut store, "owner", &session(), &request(true, 1), || {
                panic!("mute no-op must not inspect native state")
            });
        assert_eq!(code, 0);
        assert!(!changed);
        assert_eq!(noop["applied"], false);
        assert_eq!(serde_json::to_value(&store).unwrap(), before);
        let (code, conflict, changed) =
            update(&mut store, "owner", &session(), &request(true, 0), || {
                panic!("conflict must not inspect")
            });
        assert_eq!(code, 1);
        assert!(!changed);
        assert_eq!(conflict["error"], "conflict");
        assert_eq!(conflict["preferences"], first["preferences"]);
    }

    #[test]
    fn stale_run_and_conversation_never_write_or_call_capture() {
        let mut store = Store::default();
        for key in ["run_id", "conversation_id"] {
            let mut current = session();
            current[key] = json!("new-identity");
            let (code, output, changed) =
                update(&mut store, "owner", &current, &request(true, 0), || {
                    panic!("stale identity must not inspect")
                });
            assert_eq!(code, 1);
            assert!(!changed);
            assert_eq!(output["error"], "stale_identity");
            assert_eq!(output[key], "new-identity");
            assert!(store.sessions.is_empty());
        }
    }

    #[test]
    fn unmute_captures_current_edges_and_cas_rejects_delayed_device_write() {
        let mut store = Store::default();
        update(
            &mut store,
            "owner",
            &session(),
            &request(true, 0),
            unavailable,
        );
        let question = json!({"source":"codex_async","optional":true,"question_id":"q-1","question_hash":"hash-1"});
        let edge = baseline(&session(), Some(&[question])).unwrap();
        let (code, output, changed) =
            update(&mut store, "owner", &session(), &request(false, 1), || {
                Ok(edge.clone())
            });
        assert_eq!(code, 0);
        assert!(changed);
        assert_eq!(output["preferences"]["muted"], false);
        assert_eq!(output["preferences"]["revision"], 2);
        assert_eq!(output["preferences"]["suppressed"]["reply_id"], "31:9.0");
        assert_eq!(
            output["preferences"]["suppressed"]["optional_question_ids"],
            json!(["q-1"])
        );
        assert_eq!(
            output["preferences"]["suppressed"]["main_attention"],
            json!({"run_id":"run-1","phase":"input","request_id":"request-1","started_at":10.5})
        );
        assert_eq!(
            output["preferences"]["suppressed"]["child_attention"]["worker"],
            json!({"phase":"approval","request_id":"child-1","started_at":11.0})
        );
        let before = serde_json::to_value(&store).unwrap();
        let (_, delayed, changed) =
            update(&mut store, "owner", &session(), &request(true, 1), || {
                panic!("old revision")
            });
        assert!(!changed);
        assert_eq!(delayed["error"], "conflict");
        assert_eq!(serde_json::to_value(&store).unwrap(), before);
        // A later completed reply is outside the suppression edge. The real
        // unread identity and read mark have not been acknowledged by unmute.
        let mut later = session();
        later["reply_id"] = json!("32:20.0");
        assert_ne!(baseline(&later, Some(&[])).unwrap().reply_id, edge.reply_id);
        assert_eq!(session()["reply_id"], "31:9.0");
    }

    #[test]
    fn unknown_or_oversized_baseline_refuses_unmute_without_changing_preference() {
        let mut store = Store::default();
        update(
            &mut store,
            "owner",
            &session(),
            &request(true, 0),
            unavailable,
        );
        let before = serde_json::to_value(&store).unwrap();
        let (_, output, changed) =
            update(&mut store, "owner", &session(), &request(false, 1), || {
                baseline(&session(), None)
            });
        assert!(!changed);
        assert_eq!(output["error"], "baseline_unavailable");
        assert_eq!(serde_json::to_value(&store).unwrap(), before);
        let questions: Vec<_> = (0..65).map(|i| json!({"source":"codex_async","optional":true,"question_id":format!("q-{i:03}")})).collect();
        assert!(baseline(&session(), Some(&questions)).is_err());
        let mut too_many = session();
        too_many["subagents"] = json!((0..257)
            .map(|i| (
                format!("child-{i}"),
                json!({"state":"input","attention_id":"request"})
            ))
            .collect::<BTreeMap<_, _>>());
        assert!(baseline(&too_many, Some(&[])).is_err());
    }

    #[test]
    fn baseline_preserves_all_64_optional_ids_and_256_child_edges_without_trimming() {
        let mut current = session();
        current["subagents"] = json!((0..256)
            .map(|i| (
                format!("child-{i}"),
                json!({"state":"input","question_id":format!("request-{i}"),"last_event_at":-1})
            ))
            .collect::<BTreeMap<_, _>>());
        let mut questions: Vec<_> = (0..64).rev().map(|i| json!({"source":"codex_async","optional":true,"question_id":format!("q-{i:03}")})).collect();
        questions.push(questions[0].clone());
        questions.push(json!({"source":"codex_async","optional":true,"question_id":"submitted","answer_delivery":{"status":"submitted"}}));
        let result = baseline(&current, Some(&questions)).unwrap();
        assert_eq!(result.optional_question_ids.len(), 64);
        assert_eq!(result.optional_question_ids.first().unwrap(), "q-000");
        assert_eq!(result.optional_question_ids.last().unwrap(), "q-063");
        assert_eq!(result.child_attention.len(), 256);
        assert_eq!(result.child_attention["child-1"].request_id, "request-1");
        assert_eq!(result.child_attention["child-1"].started_at, None);
        let mut profiles = current;
        profiles["subagent_source"] = json!("hook_profiles");
        assert!(baseline(&profiles, Some(&[]))
            .unwrap()
            .child_attention
            .is_empty());
    }

    #[test]
    fn failed_recovery_is_not_representable_by_the_native_baseline_and_cannot_unmute() {
        for state in ["uncertain", "blocked", "exhausted"] {
            let mut current = session();
            current["recovery"] = json!({"state":state,"request_id":"recovery-B"});
            let mut store = Store::default();
            assert!(
                update(
                    &mut store,
                    "owner",
                    &current,
                    &request(true, 0),
                    unavailable
                )
                .2
            );
            let before = serde_json::to_value(&store).unwrap();
            let (code, result, changed) =
                update(&mut store, "owner", &current, &request(false, 1), || {
                    baseline(&current, Some(&[]))
                });
            assert_eq!(code, 1);
            assert_eq!(result["error"], "baseline_unavailable");
            assert!(!changed);
            assert_eq!(serde_json::to_value(&store).unwrap(), before);
            assert_eq!(result["preferences"]["muted"], true);
        }
    }

    #[test]
    fn unidentified_pending_main_or_child_edge_refuses_instead_of_suppressing_future_requests() {
        let mut current = session();
        current["attention_id"] = json!("");
        current["attention_started"] = Value::Null;
        assert!(baseline(&current, Some(&[])).is_err());
        current["phase"] = json!("idle");
        assert!(baseline(&current, Some(&[])).is_ok());
        current["subagents"]["worker"] =
            json!({"state":"input","attention_id":"","attention_started":0});
        assert!(baseline(&current, Some(&[])).is_err());
        current["subagents"]["worker"]["last_event_at"] = json!(22.0);
        assert_eq!(
            baseline(&current, Some(&[])).unwrap().child_attention["worker"].started_at,
            Some(22.0)
        );
        current["phase"] = json!("input");
        current["attention_started"] = json!(23.0);
        assert_eq!(
            baseline(&current, Some(&[]))
                .unwrap()
                .main_attention
                .unwrap()
                .started_at,
            Some(23.0)
        );
    }

    #[test]
    fn baseline_request_ids_preserve_native_line_feed_separators_but_refuse_other_controls() {
        let mut current = session();
        current["attention_id"] = json!("dsh_question:one\ntwo");
        let edge = baseline(&current, Some(&[])).unwrap();
        assert_eq!(
            edge.main_attention.unwrap().request_id,
            "dsh_question:one\ntwo"
        );
        current["attention_id"] = json!("one\ttwo");
        assert!(baseline(&current, Some(&[])).is_err());
        current["attention_id"] = json!("x".repeat(65537));
        assert!(baseline(&current, Some(&[])).is_err());
        current["attention_id"] = json!("x".repeat(65536));
        assert!(baseline(&current, Some(&[])).is_ok());
    }

    #[test]
    fn ascii_escaped_preference_budget_refuses_unmute_before_store_mutation_or_persist() {
        let mut current = session();
        current["subagents"] = json!((0..4)
            .map(|i| (
                format!("child-{i}"),
                json!({"state":"input","attention_id":"é".repeat(16384)})
            ))
            .collect::<BTreeMap<_, _>>());
        let edge = baseline(&current, Some(&[])).unwrap();
        let mut store = Store::default();
        update(
            &mut store,
            "owner",
            &session(),
            &request(true, 0),
            unavailable,
        );
        let before = serde_json::to_value(&store).unwrap();
        let (_, output, changed) =
            update(&mut store, "owner", &current, &request(false, 1), || {
                Ok(edge.clone())
            });
        assert!(!changed);
        assert_eq!(output["error"], "baseline_unavailable");
        assert_eq!(serde_json::to_value(&store).unwrap(), before);
        let oversized = Preference {
            conversation_id: "conversation-1".into(),
            muted: false,
            revision: 2,
            suppressed: edge,
        };
        assert!(serde_json::to_string(&oversized).unwrap().len() < MAX_PREFERENCE_WIRE_BYTES);
        assert!(!preference_fits(&oversized));
        let mut invalid = Store::default();
        invalid
            .sessions
            .entry("owner".into())
            .or_default()
            .insert("conversation-1".into(), oversized);
        let directory = tempfile::tempdir().unwrap();
        let path = directory.path().join("notifications.json");
        assert!(persist(&path, &invalid).is_err());
        assert!(!path.exists());
        fs::write(&path, serde_json::to_string(&invalid).unwrap()).unwrap();
        assert!(read_store(&path).is_err());
        // Non-BMP characters cost a UTF-16 surrogate pair in ASCII JSON.
        let mut emoji = Preference::new("conversation-1");
        emoji.suppressed.child_attention = (0..3)
            .map(|i| {
                (
                    format!("c-{i}"),
                    Attention {
                        phase: "input".into(),
                        request_id: "😀".repeat(16384),
                        started_at: None,
                    },
                )
            })
            .collect();
        assert!(!preference_fits(&emoji));
    }

    #[test]
    fn revision_exhaustion_and_conversation_bound_are_explicit_and_atomic() {
        let mut store = Store::default();
        let mut pref = Preference::new("conversation-1");
        pref.revision = MAX_REVISION;
        store
            .sessions
            .entry("owner".into())
            .or_default()
            .insert("conversation-1".into(), pref);
        let (_, output, changed) = update(
            &mut store,
            "owner",
            &session(),
            &request(true, MAX_REVISION),
            unavailable,
        );
        assert!(!changed);
        assert_eq!(output["error"], "revision_exhausted");
        store.sessions.clear();
        for i in 0..MAX_CONVERSATIONS {
            let id = format!("retained-{i}");
            store
                .sessions
                .entry("owner".into())
                .or_default()
                .insert(id.clone(), Preference::new(&id));
        }
        let before = serde_json::to_value(&store).unwrap();
        let (_, output, changed) = update(
            &mut store,
            "owner",
            &session(),
            &request(true, 0),
            unavailable,
        );
        assert!(!changed);
        assert_eq!(output["error"], "capacity");
        assert_eq!(serde_json::to_value(&store).unwrap(), before);
    }

    #[test]
    fn rename_and_resume_keep_preference_but_new_conversation_and_fork_do_not() {
        let mut binding = session();
        anchor(&mut binding);
        let owner = journal::journal_name(&binding).to_owned();
        let mut store = Store::default();
        update(&mut store, &owner, &binding, &request(true, 0), unavailable);
        binding["name"] = json!("codex/project/renamed");
        binding["run_id"] = json!("resumed-run");
        anchor(&mut binding);
        assert_eq!(journal::journal_name(&binding), owner);
        enrich(&mut binding, Some(&store), &owner);
        assert_eq!(binding["notification_preferences"]["muted"], true);
        assert_eq!(binding["notification_preferences"]["revision"], 1);
        binding["conversation_id"] = json!("new-conversation");
        enrich(&mut binding, Some(&store), &owner);
        assert_eq!(binding["notification_preferences"]["muted"], false);
        assert_eq!(binding["notification_preferences"]["revision"], 0);
        let mut fork = session();
        enrich(&mut fork, Some(&store), "codex/project/fork");
        assert_eq!(fork["notification_preferences"]["muted"], false);
        enrich(&mut fork, None, "codex/project/fork");
        assert!(fork["notification_preferences"].is_null());
    }

    #[test]
    fn separate_preference_file_roundtrips_and_preserves_binding_read_marks_and_journal() {
        let directory = tempfile::tempdir().unwrap();
        let paths: Vec<_> = [
            "binding.json",
            "read-marks.json",
            "events.sqlite3",
            "native-sentinel",
        ]
        .iter()
        .map(|name| directory.path().join(name))
        .collect();
        for (index, path) in paths.iter().enumerate() {
            fs::write(path, format!("unchanged-{index}")).unwrap();
        }
        let before: Vec<_> = paths.iter().map(|path| fs::read(path).unwrap()).collect();
        let path = directory.path().join("notifications.json");
        assert!(read_store(&path).unwrap().sessions.is_empty());
        let mut store = Store::default();
        update(&mut store, "owner", &session(), &request(true, 0), || {
            panic!("mute must never invoke native observer")
        });
        persist(&path, &store).unwrap();
        let mut reloaded = read_store(&path).unwrap();
        let (_, output, changed) = update(
            &mut reloaded,
            "owner",
            &session(),
            &request(false, 1),
            || baseline(&session(), Some(&[])),
        );
        assert!(changed);
        assert_eq!(output["preferences"]["revision"], 2);
        persist(&path, &reloaded).unwrap();
        assert_eq!(
            preference(&read_store(&path).unwrap(), "owner", "conversation-1").revision,
            2
        );
        for (path, bytes) in paths.iter().zip(before) {
            assert_eq!(fs::read(path).unwrap(), bytes);
        }
        fs::write(&path, "{broken").unwrap();
        assert!(read_store(&path).is_err());
        assert_eq!(fs::read(&path).unwrap(), b"{broken");
    }
}
