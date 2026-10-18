//! Native answer channel: a waiting Claude `PermissionRequest` hook process and
//! `hgs answer` meet through atomic files in one private directory per pending
//! question or tool approval, without the terminal. Exactly one of claim or
//! retract wins an offer.

use super::*;
use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use std::fs;
use std::io;
use std::time::{Duration, Instant};

/// The file protocol a waiting process speaks; `hgs answer` offers only to its own.
pub(super) const PROTOCOL: u64 = 1;
/// The waiting process and its identity; present while it accepts an offer.
const WAITING: &str = "waiting.json";
/// An offered answer, until the waiting process claims or `hgs answer` retracts it.
const ANSWER: &str = "answer.json";
const CLAIMED: &str = "claimed.json";
const RETRACTED: &str = "retracted.json";
/// Written before the decision is printed: without it nothing reached the agent.
const EMITTED: &str = "emitted.json";
const CLOSED: &str = "closed.json";

/// The channel directory of `card` under the state root `root`.
fn directory_in(root: &Path, record: &Value, card: &Value) -> PathBuf {
    let digest = |value: Value| format!("{:x}", Sha256::digest(value.to_string().as_bytes()));
    root.join("native_answers")
        .join(digest(json!(string(record, "run_id"))))
        .join(digest(json!([
            string(record, "run_id"),
            string(record, "conversation_id"),
            string(card, "question_id"),
            string(card, "question_hash"),
        ])))
}

pub(super) fn directory(record: &Value, card: &Value) -> Result<PathBuf> {
    Ok(directory_in(&absolute_root()?, record, card))
}

#[derive(Serialize, Deserialize, Clone, Debug, PartialEq)]
pub(super) struct Waiting {
    pub(super) protocol: u64,
    pub(super) pid: u32,
    pub(super) process_start: String,
    pub(super) run_id: String,
    pub(super) conversation_id: String,
    pub(super) question_id: String,
    pub(super) question_hash: String,
    pub(super) started_at: f64,
    pub(super) deadline: f64,
}

impl Waiting {
    pub(super) fn new(record: &Value, card: &Value, pid: u32, lifetime: f64) -> Self {
        let started_at = now();
        Self {
            protocol: PROTOCOL,
            pid,
            process_start: process_start(pid),
            run_id: string(record, "run_id").into(),
            conversation_id: string(record, "conversation_id").into(),
            question_id: string(card, "question_id").into(),
            question_hash: string(card, "question_hash").into(),
            started_at,
            deadline: started_at + lifetime,
        }
    }

    fn serves(&self, record: &Value, card: &Value) -> bool {
        self.protocol == PROTOCOL
            && self.run_id == string(record, "run_id")
            && self.conversation_id == string(record, "conversation_id")
            && self.question_id == string(card, "question_id")
            && self.question_hash == string(card, "question_hash")
            && !self.process_start.is_empty()
            && process_start(self.pid) == self.process_start
            && now() < self.deadline
    }
}

fn write(dir: &Path, name: &str, value: &Value) -> Result<()> {
    atomic(&dir.join(name), &value.to_string())
}

fn read(dir: &Path, name: &str) -> Option<Value> {
    serde_json::from_str(&fs::read_to_string(dir.join(name)).ok()?).ok()
}

/// The waiting process of `card` in `dir`, while it can take a new offer.
fn live_in(dir: &Path, record: &Value, card: &Value) -> Option<Waiting> {
    if [ANSWER, CLAIMED, RETRACTED, EMITTED, CLOSED]
        .iter()
        .any(|stage| dir.join(stage).exists())
    {
        return None;
    }
    let waiting: Waiting = serde_json::from_value(read(dir, WAITING)?).ok()?;
    waiting.serves(record, card).then_some(waiting)
}

/// Offer an answer. Only one offer ever exists per channel.
pub(super) fn offer(dir: &Path, offer: &Value) -> Result<()> {
    if [ANSWER, CLAIMED, RETRACTED, EMITTED]
        .iter()
        .any(|stage| dir.join(stage).exists())
    {
        return Err("an answer was already offered to this question".into());
    }
    write(dir, ANSWER, offer)
}

/// Move the offered answer to `stage` if it is still offered. A rename alone
/// is not exclusive on Windows, where std renames through an open handle: two
/// renames that both opened answer.json would both succeed. Both sides
/// therefore take the channel's handover lock first. A scanner or reader
/// without delete sharing can hold answer.json for a moment there, and the
/// refused rename moved nothing, so it is tried again for a bounded time.
fn take(dir: &Path, stage: &str) -> Result<bool> {
    let _handover = lock(Some(&dir.join("handover.lock")))?;
    let deadline = Instant::now() + Duration::from_secs(1);
    loop {
        match fs::rename(dir.join(ANSWER), dir.join(stage)) {
            Ok(()) => return Ok(true),
            Err(error) if error.kind() == io::ErrorKind::NotFound => return Ok(false),
            Err(error)
                if crate::platform::transient_replace_error(&error) && Instant::now() < deadline =>
            {
                std::thread::sleep(Duration::from_millis(20));
            }
            Err(error) => return Err(error.to_string()),
        }
    }
}

/// Take the offered answer; None when there is none or it was retracted.
fn claim(dir: &Path) -> Result<Option<Value>> {
    if !take(dir, CLAIMED)? {
        return Ok(None);
    }
    read(dir, CLAIMED)
        .map(Some)
        .ok_or_else(|| "claimed answer is unreadable".into())
}

/// Why a claimed answer must not go out: its question changed or was answered
/// in Terminal, or an approval is no longer shown waiting by an agent whose
/// identity holds (`waits`). Nothing is printed then; the answer stays the
/// channel's, so `hgs answer` reports it uncertain and never sends it again.
fn withheld(
    kind: Kind,
    current: Option<&Value>,
    bound: &Value,
    card: &Value,
    event: &Value,
    answered: bool,
    waits: impl FnOnce(&Value) -> bool,
) -> Option<&'static str> {
    let changed = should_close(kind, current, bound, card).is_some()
        || answered
        || kind == Kind::Question && current.and_then(|current| bind(current, event)).as_ref() != Some(card);
    if changed {
        Some("question changed before delivery")
    } else if kind == Kind::Approval && !current.is_some_and(waits) {
        Some("approval not shown before delivery")
    } else {
        None
    }
}

/// Take an offered answer to `kind` only when `ready`. An approval waits for
/// the screen to show its request; until then the offer stays unclaimed, and
/// `hgs answer` retracts it with nothing sent.
fn take_offer(kind: Kind, dir: &Path, ready: impl FnOnce() -> bool) -> Result<Option<Value>> {
    // An offer that appears after this look waits for the next poll.
    if kind == Kind::Approval && (!dir.join(ANSWER).exists() || !ready()) {
        return Ok(None);
    }
    claim(dir)
}

/// Withdraw the offer; false when the waiting process already claimed it.
fn retract(dir: &Path) -> Result<bool> {
    take(dir, RETRACTED)
}

#[derive(Debug, PartialEq)]
pub(super) enum Taken {
    Claimed,
    Retracted,
}

/// Wait up to `within` for the offer to be claimed, else retract it.
pub(super) fn await_claim(dir: &Path, within: Duration) -> Result<Taken> {
    let deadline = Instant::now() + within;
    while Instant::now() < deadline {
        if dir.join(CLAIMED).exists() {
            return Ok(Taken::Claimed);
        }
        std::thread::sleep(Duration::from_millis(50));
    }
    if retract(dir)? {
        return Ok(Taken::Retracted);
    }
    if dir.join(CLAIMED).exists() {
        return Ok(Taken::Claimed);
    }
    Err("the offered answer disappeared".into())
}

/// Record why the waiting process ended, and whether it printed a decision.
fn close(dir: &Path, reason: &str) {
    let _ = write(
        dir,
        CLOSED,
        &json!({"reason":reason,"emitted":emitted(dir),"at":now()}),
    );
}

#[cfg(test)]
fn closed(dir: &Path) -> Option<Value> {
    read(dir, CLOSED)
}

pub(super) fn emitted(dir: &Path) -> bool {
    dir.join(EMITTED).exists()
}

/// The answers object Claude's AskUserQuestion takes, for validated `answers`
/// to `card`: each exact question text with the text Claude records.
pub(super) fn native_answers(card: &Value, answers: &Value) -> Value {
    let mut result = serde_json::Map::new();
    for (question, answer) in card["questions"]
        .as_array()
        .into_iter()
        .flatten()
        .zip(answers.as_array().into_iter().flatten())
    {
        result.insert(
            string(question, "question").into(),
            json!(questions::answer_text(question, answer)),
        );
    }
    json!(result)
}

/// Whether the waiting process recorded in `dir` still runs.
/// Deliver `offer` through the live channel in `dir`. Returns the transport
/// that carried it. Only an offer the channel never claimed (retracted after
/// `claim_wait`) goes to `screen`. Once claimed, the answer belongs to the
/// channel: emitted.json within `handover` means it was handed to Claude, and
/// anything else is uncertain. A claimed answer is never resent, and no other
/// worker's close or a dead claimant changes that.
pub(super) fn deliver(
    dir: &Path,
    offer: &Value,
    claim_wait: Duration,
    handover: Duration,
    screen: impl FnOnce() -> Result<()>,
) -> Result<&'static str> {
    self::offer(dir, offer)?;
    match await_claim(dir, claim_wait) {
        Ok(Taken::Retracted) => return screen().map(|()| "claude_tui"),
        Ok(Taken::Claimed) => {}
        Err(error) => return Err(format!("delivery uncertain: {error}")),
    }
    let deadline = Instant::now() + handover;
    loop {
        if emitted(dir) {
            return Ok("claude_hook");
        }
        if Instant::now() >= deadline {
            return Err("delivery uncertain: Claude's answer channel took the answer but did not report handing it over; inspect Terminal before retrying".into());
        }
        std::thread::sleep(Duration::from_millis(25));
    }
}

/// Where an answer to `card` may go.
#[derive(Debug, PartialEq)]
pub(super) enum Route {
    /// A live channel waits for an offer in this directory.
    Channel(PathBuf),
    /// An answer was offered to, claimed by or handed over through the channel:
    /// it must never be sent again, through any transport.
    Locked,
    /// No answer reached the channel; the verified screen may deliver.
    Screen,
}

fn route_in(dir: &Path, record: &Value, card: &Value) -> Route {
    if [ANSWER, CLAIMED, EMITTED].iter().any(|stage| dir.join(stage).exists()) {
        Route::Locked
    } else if live_in(dir, record, card).is_some() {
        Route::Channel(dir.to_owned())
    } else {
        Route::Screen
    }
}

pub(super) fn route(record: &Value, card: &Value) -> Result<Route> {
    Ok(route_in(&directory(record, card)?, record, card))
}

/// Why a locked answer cannot be sent again.
pub(super) const LOCKED: &str = "An answer was already handed to Claude's answer channel and is not confirmed yet. Check Terminal before answering again.";

/// Take sole ownership of the channel in `dir` for this worker's lifetime: the
/// lock ends with the process. None when another worker owns it, or when an
/// earlier worker already used it; that channel is never taken over.
fn own(dir: &Path) -> Result<Option<Guard>> {
    let Some(owner) = lock_until(&dir.join("owner.lock"), Instant::now())? else {
        return Ok(None);
    };
    let used = [ANSWER, CLAIMED, RETRACTED, EMITTED, CLOSED]
        .iter()
        .any(|stage| dir.join(stage).exists());
    Ok((!used).then_some(owner))
}

/// How long a waiting process accepts an answer. Shorter than the hook timeout
/// hgs registers, so it closes itself where no SIGTERM arrives (Windows).
const LIFETIME: f64 = 86_000.;
/// The PreToolUse hook records the card just before Claude asks for permission.
const BIND: Duration = Duration::from_secs(5);
const POLL: Duration = Duration::from_millis(150);

/// What a waiting process answers: an AskUserQuestion card, or a main-agent
/// tool approval (`claude_approval::channel`).
#[derive(Clone, Copy, Debug, PartialEq)]
enum Kind {
    Question,
    Approval,
}

fn kind(event: &Value) -> Option<Kind> {
    if asks(event) {
        Some(Kind::Question)
    } else if claude_approval::channel::requested(event) {
        Some(Kind::Approval)
    } else {
        None
    }
}

/// Claude's decision for the claimed `offer`.
fn decision_kind(kind: Kind, event: &Value, card: &Value, offer: &Value) -> Result<Value> {
    match kind {
        Kind::Question => decision(event, card, offer),
        Kind::Approval => claude_approval::channel::decision(offer),
    }
}

/// The main agent's `PermissionRequest` for AskUserQuestion.
fn asks(event: &Value) -> bool {
    string(event, "hook_event_name") == "PermissionRequest"
        && string(event, "tool_name") == "AskUserQuestion"
        && string(event, "agent_id").is_empty()
}

/// The one pending main-agent question card that a Claude `PermissionRequest`
/// for AskUserQuestion asks about. It has no tool_use_id: identical requests
/// waiting at once are ambiguous and stay with the terminal.
fn bind(record: &Value, event: &Value) -> Option<Value> {
    // Claude names its conversation in every hook event. A Claude of another
    // conversation that inherited this session's variables, or an event
    // without one, never binds to this session's question.
    let conversation = string(record, "conversation_id");
    if !asks(event) || conversation.is_empty() || string(event, "session_id") != conversation {
        return None;
    }
    let asked = questions::normalize(&event["tool_input"]["questions"]).ok()?;
    let mut cards = record["pending_questions"]
        .as_object()?
        .values()
        .filter(|card| {
            string(card, "source") == "hook"
                && string(card, "agent_id") == "main"
                && card["questions"] == asked
        });
    let card = cards.next()?.clone();
    cards.next().is_none().then_some(card)
}

/// Claude's allow for the pending AskUserQuestion with the offered answers,
/// keyed by each exact question text of `card`.
fn decision(event: &Value, card: &Value, offer: &Value) -> Result<Value> {
    let answers = offer["answers"]
        .as_object()
        .ok_or("the offered answer has no answers")?;
    let texts: Vec<&str> = card["questions"]
        .as_array()
        .ok_or("invalid pending question")?
        .iter()
        .map(|question| string(question, "question"))
        .collect();
    if answers.len() != texts.len()
        || !texts
            .iter()
            .all(|text| answers.get(*text).is_some_and(Value::is_string))
    {
        return Err("the offered answer does not match the pending question".into());
    }
    let mut input = event["tool_input"].clone();
    input["answers"] = json!(answers);
    Ok(json!({"hookSpecificOutput":{"hookEventName":"PermissionRequest",
        "decision":{"behavior":"allow","updatedInput":input}}}))
}

/// Why a waiting process must stop, given the current record of its session.
/// An approval stays bound to its exact request: a later identical one is not it.
fn should_close(
    kind: Kind,
    current: Option<&Value>,
    bound: &Value,
    card: &Value,
) -> Option<&'static str> {
    let Some(current) = current else {
        return Some("session ended");
    };
    if string(current, "run_id") != string(bound, "run_id") {
        return Some("run changed");
    }
    if string(current, "conversation_id") != string(bound, "conversation_id") {
        return Some("conversation changed");
    }
    match kind {
        Kind::Question => {
            let pending = &current["pending_questions"][string(card, "tool_call_id")];
            if pending["question_hash"] != card["question_hash"] {
                return Some("question resolved");
            }
        }
        Kind::Approval if !claude_approval::channel::pending(current, card) => {
            return Some("approval resolved");
        }
        Kind::Approval => {}
    }
    if !process_alive(current) {
        return Some("agent exited");
    }
    None
}

/// Notices a transcript `tool_result` for the bound tool use: an answer, a
/// decline or a cancellation in Terminal. It reads only bytes added since its
/// last look, so a long wait stays cheap.
#[derive(Default)]
struct TranscriptWatch {
    offset: u64,
}

impl TranscriptWatch {
    fn answered(&mut self, record: &Value, tool_use_id: &str) -> bool {
        use std::io::{Read, Seek, SeekFrom};
        let conversation = string(record, "conversation_id");
        let path = PathBuf::from(string(record, "transcript"));
        if conversation.is_empty()
            || !path.is_absolute()
            || path.file_name().is_none_or(|name| {
                name.to_string_lossy() != format!("{conversation}.jsonl")
            })
        {
            return false;
        }
        let Ok(mut file) = fs::File::open(&path) else {
            return false;
        };
        let length = file.metadata().map(|meta| meta.len()).unwrap_or(0);
        if length < self.offset {
            self.offset = 0;
        }
        // Start at most 8 MiB back, as the acknowledgement reader does.
        let start = self.offset.max(length.saturating_sub(8 * 1024 * 1024));
        let mut bytes = Vec::new();
        if file.seek(SeekFrom::Start(start)).is_err()
            || file.take(length - start).read_to_end(&mut bytes).is_err()
        {
            return false;
        }
        // Only complete lines count; a partial last line is read again later.
        let complete = bytes.iter().rposition(|byte| *byte == b'\n').map_or(0, |end| end + 1);
        self.offset = start + complete as u64;
        bytes[..complete]
            .split(|byte| *byte == b'\n')
            .filter_map(|line| serde_json::from_slice::<Value>(line).ok())
            .filter(|event| string(event, "sessionId") == conversation && event["isSidechain"] != true)
            .any(|event| {
                event["message"]["content"].as_array().is_some_and(|parts| {
                    parts.iter().any(|part| {
                        part["type"] == "tool_result" && part["tool_use_id"] == tool_use_id
                    })
                })
            })
    }
}

/// Remove channel directories of runs untouched for `age`, looking at no more
/// than `limit` entries per call.
fn prune(root: &Path, age: Duration, limit: usize) {
    let Ok(entries) = fs::read_dir(root) else {
        return;
    };
    let cutoff = std::time::SystemTime::now() - age;
    for entry in entries.flatten().take(limit) {
        let old = entry
            .metadata()
            .and_then(|meta| meta.modified())
            .is_ok_and(|modified| modified < cutoff);
        if old && entry.file_type().is_ok_and(|kind| kind.is_dir()) {
            let _ = fs::remove_dir_all(entry.path());
        }
    }
}

static TERMINATED: std::sync::atomic::AtomicBool = std::sync::atomic::AtomicBool::new(false);

#[cfg(unix)]
extern "C" fn terminate(_: libc::c_int) {
    TERMINATED.store(true, std::sync::atomic::Ordering::SeqCst);
}

/// `hgs __state answer-channel`: Claude's `PermissionRequest` hook for
/// AskUserQuestion and main-agent tool approvals. It waits beside the native
/// panel for an answer offered through `hgs answer` and prints Claude's
/// decision: allow with those answers, or the one-time approval. Anything
/// else (another session, no exact card, an answer in Terminal, a timeout)
/// ends it without output, and the panel stays. So does any error: nothing is
/// printed before emitted.json. An offer this worker had not claimed lets
/// `hgs answer` use the screen; a claimed one stays with the channel and is
/// reported uncertain, never sent again.
pub(super) fn run() -> Result<i32> {
    let _ = wait();
    Ok(0)
}

fn wait() -> Result<()> {
    let (Some(name), Some(token)) = (nonempty_env("HGS_SESSION"), nonempty_env("HGS_RUN_ID"))
    else {
        return Ok(());
    };
    let started = now();
    let event = stdin_json()?;
    let Some(kind) = kind(&event) else {
        return Ok(());
    };
    // One proof that an approval still waits, before and after its claim.
    let waits = claude_approval::channel::waits_now;
    #[cfg(unix)]
    unsafe {
        libc::signal(libc::SIGTERM, terminate as *const () as libc::sighandler_t);
    }
    let until = Instant::now() + BIND;
    // An approval stays with the one active tool call that the first record
    // shows for it; without one it never binds, and a later call with the same
    // input is another request.
    let mut asked: Option<Option<String>> = None;
    let (record, card) = loop {
        if let Some(record) = read_run(&name, &token)? {
            let card = match kind {
                Kind::Question => bind(&record, &event),
                Kind::Approval => match asked.get_or_insert_with(|| claude_approval::channel::asked_use(&record, &event, started)) {
                    Some(tool_use) => claude_approval::channel::bind_to(&record, &event, tool_use),
                    None => return Ok(()),
                },
            };
            if let Some(card) = card {
                break (record, card);
            }
        }
        if Instant::now() >= until || TERMINATED.load(std::sync::atomic::Ordering::SeqCst) {
            return Ok(());
        }
        std::thread::sleep(Duration::from_millis(100));
    };
    let root = absolute_root()?;
    prune(&root.join("native_answers"), Duration::from_secs(30 * 86_400), 64);
    let dir = directory_in(&root, &record, &card);
    // One worker per question: a second PermissionRequest worker leaves it.
    let Some(_owner) = own(&dir)? else {
        return Ok(());
    };
    let lifetime = nonempty_env("HGS_ANSWER_CHANNEL_SECONDS")
        .and_then(|seconds| seconds.parse::<f64>().ok())
        .filter(|seconds| *seconds > 0. && *seconds <= LIFETIME)
        .unwrap_or(LIFETIME);
    let waiting = Waiting::new(&record, &card, std::process::id(), lifetime);
    write(&dir, WAITING, &json!(waiting))?;
    let tool = string(&card, "tool_call_id").to_owned();
    let mut transcript = TranscriptWatch::default();
    let mut looked = Instant::now() - Duration::from_secs(1);
    loop {
        if TERMINATED.load(std::sync::atomic::Ordering::SeqCst) {
            close(&dir, "terminated");
            return Ok(());
        }
        if now() >= waiting.deadline {
            close(&dir, "deadline");
            return Ok(());
        }
        let current = read_run(&name, &token)?;
        if let Some(reason) = should_close(kind, current.as_ref(), &record, &card) {
            close(&dir, reason);
            return Ok(());
        }
        if looked.elapsed() >= Duration::from_secs(1) {
            looked = Instant::now();
            if transcript.answered(current.as_ref().unwrap_or(&record), &tool) {
                close(&dir, "answered in Terminal");
                return Ok(());
            }
        }
        let ready = || current.as_ref().is_some_and(waits);
        if let Some(offer) = take_offer(kind, &dir, ready)? {
            // The question must still be the bound one when its answer goes out.
            // Claude tells the hook nothing of a Terminal approval: only the
            // screen still showing the request proves it waits.
            let current = read_run(&name, &token)?;
            let answered = transcript.answered(current.as_ref().unwrap_or(&record), &tool);
            let withheld = withheld(kind, current.as_ref(), &record, &card, &event, answered, waits);
            let output = decision_kind(kind, &event, &card, &offer);
            if withheld.is_some() || output.is_err() || TERMINATED.load(std::sync::atomic::Ordering::SeqCst) {
                close(&dir, withheld.unwrap_or("invalid answer"));
                return Ok(());
            }
            write(&dir, EMITTED, &json!({"request_id":offer["request_id"],"at":now()}))?;
            println!("{}", output?);
            close(&dir, "emitted");
            return Ok(());
        }
        std::thread::sleep(POLL);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn record() -> Value {
        json!({"run_id":"run-1","conversation_id":"conversation-1"})
    }
    fn card() -> Value {
        json!({"question_id":"tool:toolu_1","question_hash":"hash-1","tool_call_id":"toolu_1"})
    }

    #[test]
    fn directory_is_private_to_run_and_question() {
        let root = Path::new("/state");
        let first = directory_in(root, &record(), &card());
        assert!(first.starts_with("/state/native_answers"));
        let mut changed = card();
        changed["question_hash"] = json!("hash-2");
        assert_ne!(first, directory_in(root, &record(), &changed));
        let mut run = record();
        run["run_id"] = json!("run-2");
        assert_ne!(first.parent(), directory_in(root, &run, &card()).parent());
    }

    #[test]
    fn live_requires_matching_identity_and_process() {
        let temp = tempfile::tempdir().unwrap();
        let dir = temp.path();
        assert!(live_in(dir, &record(), &card()).is_none());
        let waiting = Waiting::new(&record(), &card(), std::process::id(), 60.);
        write(dir, WAITING, &json!(waiting)).unwrap();
        assert_eq!(live_in(dir, &record(), &card()), Some(waiting.clone()));
        let mut changed = card();
        changed["question_hash"] = json!("changed");
        assert!(live_in(dir, &record(), &changed).is_none());
        let mut conversation = record();
        conversation["conversation_id"] = json!("other");
        assert!(live_in(dir, &conversation, &card()).is_none());
        // Another start time means the waiting process is gone.
        let mut gone = waiting.clone();
        gone.process_start = "not this process".into();
        write(dir, WAITING, &json!(gone)).unwrap();
        assert!(live_in(dir, &record(), &card()).is_none());
        let mut expired = waiting.clone();
        expired.deadline = now() - 1.;
        write(dir, WAITING, &json!(expired)).unwrap();
        assert!(live_in(dir, &record(), &card()).is_none());
        let mut newer = waiting.clone();
        newer.protocol = PROTOCOL + 1;
        write(dir, WAITING, &json!(newer)).unwrap();
        assert!(live_in(dir, &record(), &card()).is_none());
        // Any later stage ends the window for a new offer.
        write(dir, WAITING, &json!(waiting)).unwrap();
        for stage in [ANSWER, CLAIMED, RETRACTED, EMITTED, CLOSED] {
            write(dir, stage, &json!({})).unwrap();
            assert!(live_in(dir, &record(), &card()).is_none(), "{stage}");
            fs::remove_file(dir.join(stage)).unwrap();
        }
        assert!(live_in(dir, &record(), &card()).is_some());
    }

    #[test]
    fn two_offers_claim_once() {
        for _ in 0..200 {
            let temp = tempfile::tempdir().unwrap();
            let dir = temp.path().to_owned();
            offer(&dir, &json!({"request_id":"one"})).unwrap();
            assert!(offer(&dir, &json!({"request_id":"two"})).is_err());
            let claimer = {
                let dir = dir.clone();
                std::thread::spawn(move || claim(&dir).unwrap())
            };
            let retractor = {
                let dir = dir.clone();
                std::thread::spawn(move || retract(&dir).unwrap())
            };
            let claimed = claimer.join().unwrap();
            let retracted = retractor.join().unwrap();
            assert!(claimed.is_some() != retracted, "exactly one wins");
            if let Some(answer) = claimed {
                assert_eq!(answer["request_id"], "one");
            }
        }
    }

    #[test]
    fn retract_after_timeout_without_claim() {
        let temp = tempfile::tempdir().unwrap();
        let dir = temp.path();
        offer(dir, &json!({"request_id":"one"})).unwrap();
        assert_eq!(await_claim(dir, Duration::from_millis(120)).unwrap(), Taken::Retracted);
        assert!(!dir.join(ANSWER).exists());
        assert!(dir.join(RETRACTED).exists());
        assert!(claim(dir).unwrap().is_none());
    }

    #[test]
    fn claim_within_the_wait_is_reported() {
        let temp = tempfile::tempdir().unwrap();
        let dir = temp.path().to_owned();
        offer(&dir, &json!({"request_id":"one"})).unwrap();
        let claimer = {
            let dir = dir.clone();
            std::thread::spawn(move || {
                std::thread::sleep(Duration::from_millis(60));
                claim(&dir).unwrap()
            })
        };
        assert_eq!(await_claim(&dir, Duration::from_secs(3)).unwrap(), Taken::Claimed);
        assert!(claimer.join().unwrap().is_some());
        assert!(!dir.join(RETRACTED).exists());
    }

    #[test]
    fn close_records_whether_the_decision_was_emitted() {
        let temp = tempfile::tempdir().unwrap();
        let dir = temp.path();
        close(dir, "terminated");
        assert_eq!(closed(dir).unwrap()["reason"], "terminated");
        assert_eq!(closed(dir).unwrap()["emitted"], false);
        assert!(!emitted(dir));
        write(dir, EMITTED, &json!({"request_id":"one"})).unwrap();
        close(dir, "terminated");
        assert_eq!(closed(dir).unwrap()["emitted"], true);
        assert!(emitted(dir));
    }

    fn items() -> Value {
        json!([{"question":"Which release should we prepare?","header":"Release","options":[
            {"label":"Stable","description":"Use stable build"},{"label":"Preview","description":"Use preview build"}]}])
    }
    fn hook_card(tool: &str) -> Value {
        json!({"question_id":format!("tool:{tool}"),"tool_call_id":tool,"agent_id":"main","source":"hook",
            "question_hash":"hash-1","questions":questions::normalize(&items()).unwrap()})
    }
    fn event() -> Value {
        json!({"hook_event_name":"PermissionRequest","tool_name":"AskUserQuestion","session_id":"conversation-1",
            "tool_input":{"questions":items(),"metadata":{"source":"model"}}})
    }
    fn live_record() -> Value {
        let pid = std::process::id();
        json!({"run_id":"run-1","conversation_id":"conversation-1","pid":pid,"process_start":process_start(pid),
            "pending_questions":{"toolu_1":hook_card("toolu_1")}})
    }

    #[test]
    fn bind_needs_exactly_one_matching_card() {
        let mut record = live_record();
        assert_eq!(bind(&record, &event()).unwrap()["tool_call_id"], "toolu_1");
        let mut child = event();
        child["agent_id"] = json!("agent-7");
        assert!(bind(&record, &child).is_none());
        let mut other_tool = event();
        other_tool["tool_name"] = json!("Bash");
        assert!(bind(&record, &other_tool).is_none());
        let mut other_event = event();
        other_event["hook_event_name"] = json!("PreToolUse");
        assert!(bind(&record, &other_event).is_none());
        let mut changed = event();
        changed["tool_input"]["questions"][0]["question"] = json!("Something else?");
        assert!(bind(&record, &changed).is_none());
        let mut terminal_only = hook_card("toolu_1");
        terminal_only["source"] = json!("claude_native_question");
        record["pending_questions"]["toolu_1"] = terminal_only;
        assert!(bind(&record, &event()).is_none());
        record["pending_questions"] = json!({"toolu_1":hook_card("toolu_1"),"toolu_2":hook_card("toolu_2")});
        assert!(bind(&record, &event()).is_none(), "identical requests are ambiguous");
        record["pending_questions"] = json!({});
        assert!(bind(&record, &event()).is_none());
    }

    #[test]
    fn bind_requires_the_saved_conversation() {
        let record = live_record();
        assert!(bind(&record, &event()).is_some());
        // A Claude of another conversation that inherited this session's
        // variables must never bind to its question.
        let mut foreign = event();
        foreign["session_id"] = json!("another-conversation");
        assert!(bind(&record, &foreign).is_none());
        let mut missing = event();
        missing.as_object_mut().unwrap().remove("session_id");
        assert!(bind(&record, &missing).is_none());
        let mut empty = event();
        empty["session_id"] = json!("");
        assert!(bind(&record, &empty).is_none());
        let mut unsaved = live_record();
        unsaved["conversation_id"] = json!("");
        assert!(bind(&unsaved, &empty).is_none());
    }

    #[test]
    fn decision_passes_the_original_input_with_answers() {
        let card = hook_card("toolu_1");
        let offer = json!({"request_id":"one","answers":{"Which release should we prepare?":"Preview"}});
        let output = decision(&event(), &card, &offer).unwrap();
        let allowed = &output["hookSpecificOutput"]["decision"];
        assert_eq!(output["hookSpecificOutput"]["hookEventName"], "PermissionRequest");
        assert_eq!(allowed["behavior"], "allow");
        let mut expected = event()["tool_input"].clone();
        expected["answers"] = offer["answers"].clone();
        assert_eq!(allowed["updatedInput"], expected);
        assert!(allowed.get("updatedPermissions").is_none());
        for answers in [json!({}), json!({"Another question?":"Preview"}), json!({"Which release should we prepare?":7}),
            json!({"Which release should we prepare?":"Preview","Extra?":"x"})] {
            assert!(decision(&event(), &card, &json!({"request_id":"one","answers":answers})).is_err());
        }
    }

    #[test]
    fn channel_exits_when_card_or_run_changes() {
        let bound = live_record();
        let card = hook_card("toolu_1");
        assert_eq!(should_close(Kind::Question, Some(&bound), &bound, &card), None);
        assert_eq!(should_close(Kind::Question, None, &bound, &card), Some("session ended"));
        let mut run = bound.clone();
        run["run_id"] = json!("run-2");
        assert_eq!(should_close(Kind::Question, Some(&run), &bound, &card), Some("run changed"));
        let mut conversation = bound.clone();
        conversation["conversation_id"] = json!("other");
        assert_eq!(should_close(Kind::Question, Some(&conversation), &bound, &card), Some("conversation changed"));
        let mut resolved = bound.clone();
        resolved["pending_questions"] = json!({});
        assert_eq!(should_close(Kind::Question, Some(&resolved), &bound, &card), Some("question resolved"));
        let mut replaced = bound.clone();
        replaced["pending_questions"]["toolu_1"]["question_hash"] = json!("hash-2");
        assert_eq!(should_close(Kind::Question, Some(&replaced), &bound, &card), Some("question resolved"));
        let mut exited = bound.clone();
        exited["process_start"] = json!("not this process");
        assert_eq!(should_close(Kind::Question, Some(&exited), &bound, &card), Some("agent exited"));
    }

    #[test]
    fn transcript_watch_sees_only_results_for_the_bound_tool() {
        let temp = tempfile::tempdir().unwrap();
        let conversation = "6f1f6a3e-6c55-4c55-9a55-1c2b3d4e5f60";
        let path = temp.path().join(format!("{conversation}.jsonl"));
        let line = |tool: &str, error: bool| {
            format!("{}\n", json!({"sessionId":conversation,"message":{"content":[
                {"type":"tool_result","tool_use_id":tool,"is_error":error,"content":"x"}]}}))
        };
        fs::write(&path, line("toolu_other", false)).unwrap();
        let record = json!({"conversation_id":conversation,"transcript":path});
        let mut watch = TranscriptWatch::default();
        assert!(!watch.answered(&record, "toolu_1"));
        assert!(!watch.answered(&record, "toolu_1"), "unchanged file");
        let mut file = fs::OpenOptions::new().append(true).open(&path).unwrap();
        std::io::Write::write_all(&mut file, line("toolu_1", true).as_bytes()).unwrap();
        assert!(watch.answered(&record, "toolu_1"), "a Terminal decline also resolves it");
        let mut wrong = record.clone();
        wrong["transcript"] = json!(temp.path().join("other.jsonl"));
        assert!(!TranscriptWatch::default().answered(&wrong, "toolu_1"));
    }

    #[test]
    fn prune_removes_only_old_run_directories() {
        let temp = tempfile::tempdir().unwrap();
        let root = temp.path().join("native_answers");
        for name in ["old", "new"] {
            fs::create_dir_all(root.join(name).join("question")).unwrap();
        }
        let old = std::time::SystemTime::now() - Duration::from_secs(40 * 86400);
        // Windows opens a directory for its times only with backup semantics.
        #[cfg(windows)]
        let folder = {
            use std::os::windows::fs::OpenOptionsExt;
            fs::OpenOptions::new().write(true).custom_flags(0x0200_0000).open(root.join("old")).unwrap()
        };
        #[cfg(not(windows))]
        let folder = fs::File::open(root.join("old")).unwrap();
        folder.set_modified(old).unwrap();
        drop(folder);
        prune(&root, Duration::from_secs(30 * 86400), 64);
        assert!(!root.join("old").exists());
        assert!(root.join("new").exists());
    }

    /// A scanner or reader that opens answer.json without delete sharing makes
    /// Windows refuse its rename for a moment: the claim or the retraction
    /// waits it out, and still only one of them wins.
    #[cfg(windows)]
    #[test]
    fn a_briefly_held_offer_is_still_taken_once() {
        use std::os::windows::fs::OpenOptionsExt;
        // FILE_SHARE_READ only, as some readers and scanners open files.
        let hold = |dir: &Path| fs::OpenOptions::new().read(true).share_mode(1).open(dir.join(ANSWER)).unwrap();
        for (stage, other) in [(CLAIMED, RETRACTED), (RETRACTED, CLAIMED)] {
            let temp = tempfile::tempdir().unwrap();
            let dir = temp.path();
            offer(dir, &json!({"request_id": "r", "answers": {}})).unwrap();
            let reader = hold(dir);
            let release = std::thread::spawn(move || {
                std::thread::sleep(Duration::from_millis(150));
                drop(reader);
            });
            assert!(take(dir, stage).unwrap(), "{stage}");
            release.join().unwrap();
            assert!(dir.join(stage).exists() && !dir.join(ANSWER).exists(), "{stage}");
            assert!(!take(dir, other).unwrap(), "{other} after {stage}");
        }
        // A holder that never lets go leaves the offer in place, as an error.
        let temp = tempfile::tempdir().unwrap();
        let dir = temp.path();
        offer(dir, &json!({"request_id": "r", "answers": {}})).unwrap();
        let _reader = hold(dir);
        let started = Instant::now();
        assert!(take(dir, CLAIMED).is_err());
        assert!(started.elapsed() < Duration::from_secs(3));
        assert!(dir.join(ANSWER).exists() && !dir.join(CLAIMED).exists());
    }

    /// A stand-in waiting process: waiting.json for this test process, then `act`.
    fn channel(act: impl FnOnce(&Path) + Send + 'static) -> (tempfile::TempDir, std::thread::JoinHandle<()>) {
        let temp = tempfile::tempdir().unwrap();
        let dir = temp.path().to_owned();
        write(&dir, WAITING, &json!(Waiting::new(&record(), &card(), std::process::id(), 60.))).unwrap();
        (temp, std::thread::spawn(move || act(&dir)))
    }
    // Fsync on hosted runners can take most of a second: a stand-in claims
    // well within `claim_wait`, and a handover meant to arrive gets 5 s.
    fn delivered(dir: &Path, screen: &std::cell::Cell<u32>) -> Result<&'static str> {
        delivered_within(dir, screen, Duration::from_millis(300))
    }
    fn delivered_within(dir: &Path, screen: &std::cell::Cell<u32>, handover: Duration) -> Result<&'static str> {
        deliver(dir, &json!({"request_id":"one","answers":{}}), Duration::from_secs(3),
            handover, || { screen.set(screen.get() + 1); Ok(()) })
    }

    #[test]
    fn answer_uses_claimed_channel() {
        let (temp, waiting) = channel(|dir| {
            while claim(dir).unwrap().is_none() { std::thread::sleep(Duration::from_millis(10)); }
            write(dir, EMITTED, &json!({"request_id":"one"})).unwrap();
        });
        let screen = std::cell::Cell::new(0);
        assert_eq!(delivered_within(temp.path(), &screen, Duration::from_secs(5)).unwrap(), "claude_hook");
        assert_eq!(screen.get(), 0);
        waiting.join().unwrap();
    }

    #[test]
    fn unclaimed_offer_retracts_then_uses_screen() {
        let (temp, waiting) = channel(|_| {});
        let screen = std::cell::Cell::new(0);
        assert_eq!(delivered(temp.path(), &screen).unwrap(), "claude_tui");
        assert_eq!(screen.get(), 1);
        assert!(temp.path().join(RETRACTED).exists());
        waiting.join().unwrap();
    }

    #[test]
    fn channel_closed_before_emitting_is_uncertain_without_screen() {
        let (temp, waiting) = channel(|dir| {
            while claim(dir).unwrap().is_none() { std::thread::sleep(Duration::from_millis(10)); }
            close(dir, "question changed before delivery");
        });
        let screen = std::cell::Cell::new(0);
        let error = delivered(temp.path(), &screen).unwrap_err();
        assert!(error.contains("delivery uncertain"), "{error}");
        assert_eq!(screen.get(), 0, "a claimed answer never goes to the screen");
        waiting.join().unwrap();
    }

    #[test]
    fn dead_channel_after_claim_is_uncertain_without_screen() {
        let (temp, waiting) = channel(|dir| {
            while claim(dir).unwrap().is_none() { std::thread::sleep(Duration::from_millis(10)); }
            let mut gone = Waiting::new(&record(), &card(), std::process::id(), 60.);
            gone.process_start = "not this process".into();
            write(dir, WAITING, &json!(gone)).unwrap();
        });
        let screen = std::cell::Cell::new(0);
        assert!(delivered(temp.path(), &screen).unwrap_err().contains("delivery uncertain"));
        assert_eq!(screen.get(), 0);
        waiting.join().unwrap();
    }

    #[test]
    fn old_worker_close_after_claim_waits_for_the_claimant() {
        // Another worker's close must not decide for the one that claimed.
        let (temp, waiting) = channel(|dir| {
            while claim(dir).unwrap().is_none() { std::thread::sleep(Duration::from_millis(10)); }
            close(dir, "deadline");
            std::thread::sleep(Duration::from_millis(100));
            write(dir, EMITTED, &json!({"request_id":"one"})).unwrap();
        });
        let screen = std::cell::Cell::new(0);
        assert_eq!(delivered_within(temp.path(), &screen, Duration::from_secs(5)).unwrap(), "claude_hook");
        assert_eq!(screen.get(), 0);
        waiting.join().unwrap();
    }

    #[test]
    fn delayed_emit_beyond_the_handover_is_uncertain_without_screen() {
        let (temp, waiting) = channel(|dir| {
            while claim(dir).unwrap().is_none() { std::thread::sleep(Duration::from_millis(10)); }
            close(dir, "deadline");
            std::thread::sleep(Duration::from_millis(600));
            write(dir, EMITTED, &json!({"request_id":"one"})).unwrap();
        });
        let screen = std::cell::Cell::new(0);
        assert!(delivered(temp.path(), &screen).unwrap_err().contains("delivery uncertain"));
        assert_eq!(screen.get(), 0);
        waiting.join().unwrap();
        assert!(temp.path().join(EMITTED).exists());
    }

    #[test]
    fn only_one_worker_owns_a_channel() {
        let temp = tempfile::tempdir().unwrap();
        let dir = temp.path();
        let first = own(dir).unwrap();
        assert!(first.is_some());
        assert!(own(dir).unwrap().is_none(), "a second worker leaves the channel to the first");
        drop(first);
        assert!(own(dir).unwrap().is_some(), "a channel nobody used is free again");
        // A channel an earlier worker used is never taken over.
        for stage in [ANSWER, CLAIMED, RETRACTED, EMITTED, CLOSED] {
            write(dir, stage, &json!({})).unwrap();
            assert!(own(dir).unwrap().is_none(), "{stage}");
            fs::remove_file(dir.join(stage)).unwrap();
        }
    }

    #[test]
    fn routes_keep_a_handed_over_answer_away_from_the_screen() {
        let temp = tempfile::tempdir().unwrap();
        let dir = temp.path();
        assert_eq!(route_in(dir, &record(), &card()), Route::Screen);
        write(dir, WAITING, &json!(Waiting::new(&record(), &card(), std::process::id(), 60.))).unwrap();
        assert_eq!(route_in(dir, &record(), &card()), Route::Channel(dir.to_owned()));
        for stage in [ANSWER, CLAIMED, EMITTED] {
            write(dir, stage, &json!({})).unwrap();
            assert_eq!(route_in(dir, &record(), &card()), Route::Locked, "{stage}");
            fs::remove_file(dir.join(stage)).unwrap();
        }
        // Unclaimed outcomes leave the screen path available.
        for stage in [RETRACTED, CLOSED] {
            write(dir, stage, &json!({})).unwrap();
            assert_eq!(route_in(dir, &record(), &card()), Route::Screen, "{stage}");
            fs::remove_file(dir.join(stage)).unwrap();
        }
        let mut gone = Waiting::new(&record(), &card(), std::process::id(), 60.);
        gone.process_start = "not this process".into();
        write(dir, WAITING, &json!(gone)).unwrap();
        assert_eq!(route_in(dir, &record(), &card()), Route::Screen);
    }

    /// A main-agent approval bound to its one active tool call, in this process.
    fn approval() -> (Value, Value, Value) {
        let input = json!({"command":"printf hi > out.txt"});
        let pid = std::process::id();
        let record = json!({"agent":"claude","run_id":"run-1","conversation_id":"conversation-1","pid":pid,
            "process_start":process_start(pid),"phase":"approval","pending_approval_at":now(),
            "pending_approval":{"hook_event_name":"PermissionRequest","tool_name":"Bash","tool_input":input},
            "claude_tool_uses":{"toolu_1":{"tool_name":"Bash","input":format!("{:x}", Sha256::digest(input.to_string().as_bytes())),"at":now()-1.}}});
        let event = json!({"hook_event_name":"PermissionRequest","tool_name":"Bash","tool_input":input,"session_id":"conversation-1"});
        let card = claude_approval::channel::bind(&record, &event).unwrap();
        (record, event, card)
    }

    #[test]
    fn a_claimed_approval_goes_out_only_while_its_agent_still_waits() {
        let (record, event, card) = approval();
        assert_eq!(withheld(Kind::Approval, Some(&record), &record, &card, &event, false, |_| true), None);
        // What a refused identity gives after the claim: nothing printed.
        let refused = |checked: &Value| claude_approval::channel::waits(checked, Err("Session identity changed; open Terminal".into()), |_| unreachable!());
        assert_eq!(withheld(Kind::Approval, Some(&record), &record, &card, &event, false, refused),
            Some("approval not shown before delivery"));
        assert_eq!(withheld(Kind::Approval, Some(&record), &record, &card, &event, true, |_| true),
            Some("question changed before delivery"), "answered in Terminal");
        assert_eq!(withheld(Kind::Approval, None, &record, &card, &event, false, |_| true),
            Some("question changed before delivery"));
        let mut finished = record.clone();
        finished["claude_tool_uses"] = json!({});
        assert_eq!(withheld(Kind::Approval, Some(&finished), &record, &card, &event, false, |_| true),
            Some("question changed before delivery"));
        // A question never asks the screen, and still needs its exact card.
        let question = live_record();
        let card = hook_card("toolu_1");
        assert_eq!(withheld(Kind::Question, Some(&question), &question, &card, &self::event(), false, |_| unreachable!()), None);
        let mut changed = question.clone();
        changed["pending_questions"]["toolu_2"] = hook_card("toolu_2");
        assert_eq!(withheld(Kind::Question, Some(&changed), &question, &card, &self::event(), false, |_| unreachable!()),
            Some("question changed before delivery"), "identical questions are ambiguous");
        assert_eq!(withheld(Kind::Question, Some(&question), &question, &card, &self::event(), true, |_| unreachable!()),
            Some("question changed before delivery"));
    }

    #[test]
    fn a_refused_identity_leaves_an_offer_unclaimed() {
        let (temp, waiting) = channel(|dir| {
            for _ in 0..40 {
                let ready = || claude_approval::channel::waits(&json!({}), Err("Session identity changed; open Terminal".into()), |_| unreachable!());
                assert!(take_offer(Kind::Approval, dir, ready).unwrap().is_none());
                std::thread::sleep(Duration::from_millis(10));
            }
        });
        let screen = std::cell::Cell::new(0);
        let error = deliver(temp.path(), &json!({"request_id":"one","decision":"allow"}), Duration::from_millis(200),
            Duration::from_millis(200), || { screen.set(screen.get() + 1); Err("Nothing was sent".into()) }).unwrap_err();
        assert_eq!((error.as_str(), screen.get()), ("Nothing was sent", 1));
        assert!(!temp.path().join(CLAIMED).exists() && !temp.path().join(EMITTED).exists());
        waiting.join().unwrap();
    }

    #[test]
    fn an_approval_offer_stays_unclaimed_until_its_screen_shows_it() {
        let (temp, waiting) = channel(|dir| {
            for _ in 0..40 {
                assert!(take_offer(Kind::Approval, dir, || false).unwrap().is_none());
                std::thread::sleep(Duration::from_millis(10));
            }
        });
        let screen = std::cell::Cell::new(0);
        let error = deliver(temp.path(), &json!({"request_id":"one","decision":"allow"}), Duration::from_millis(200),
            Duration::from_millis(200), || { screen.set(screen.get() + 1); Err("Nothing was sent".into()) }).unwrap_err();
        assert_eq!(error, "Nothing was sent", "a retracted offer is no uncertain delivery");
        assert_eq!(screen.get(), 1);
        assert!(!temp.path().join(CLAIMED).exists());
        assert!(temp.path().join(RETRACTED).exists());
        waiting.join().unwrap();
        // Once shown, the approval is taken; a question never waits for it.
        for (kind, ready) in [(Kind::Approval, true), (Kind::Question, false)] {
            let temp = tempfile::tempdir().unwrap();
            offer(temp.path(), &json!({"request_id":"two"})).unwrap();
            assert_eq!(take_offer(kind, temp.path(), || ready).unwrap().unwrap()["request_id"], "two", "{kind:?}");
        }
        let empty = tempfile::tempdir().unwrap();
        assert!(take_offer(Kind::Approval, empty.path(), || panic!("no offer, no capture")).unwrap().is_none());
    }

    #[test]
    fn claimed_without_handover_is_uncertain() {
        let (temp, waiting) = channel(|dir| {
            while claim(dir).unwrap().is_none() { std::thread::sleep(Duration::from_millis(10)); }
        });
        let screen = std::cell::Cell::new(0);
        let error = delivered(temp.path(), &screen).unwrap_err();
        assert!(error.contains("delivery uncertain"), "{error}");
        assert_eq!(screen.get(), 0);
        waiting.join().unwrap();
    }

    #[test]
    fn emitted_after_closing_is_never_resent() {
        let (temp, waiting) = channel(|dir| {
            while claim(dir).unwrap().is_none() { std::thread::sleep(Duration::from_millis(10)); }
            write(dir, EMITTED, &json!({"request_id":"one"})).unwrap();
            close(dir, "terminated");
        });
        let screen = std::cell::Cell::new(0);
        assert_eq!(delivered_within(temp.path(), &screen, Duration::from_secs(5)).unwrap(), "claude_hook");
        assert_eq!(screen.get(), 0);
        waiting.join().unwrap();
    }

    #[test]
    fn native_answers_key_each_question_text() {
        let card = json!({"questions": questions::normalize(&json!([
            {"question":"Which release?","header":"Release","options":[{"label":"Stable"},{"label":"Preview"}]},
            {"question":"Which checks?","header":"Checks","multiSelect":true,"options":[{"label":"Unit"},{"label":"UI"}]}])).unwrap()});
        let answers = json!([
            {"question_id":"q_0","selected_option_ids":["opt_0_1"],"text":""},
            {"question_id":"q_1","selected_option_ids":["opt_1_0","opt_1_1"],"text":"Lint"}]);
        assert_eq!(native_answers(&card, &answers), json!({"Which release?":"Preview","Which checks?":"Unit, UI, Lint"}));
    }
}
