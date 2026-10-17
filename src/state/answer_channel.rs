//! Native answer channel: a waiting Claude `PermissionRequest` hook process and
//! `hgs answer` meet through atomic files in one private directory per pending
//! question, without the terminal. Exactly one of claim or retract wins an offer.

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

pub(super) fn live(record: &Value, card: &Value) -> Option<Waiting> {
    live_in(&directory(record, card).ok()?, record, card)
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

/// Take the offered answer; None when there is none or it was retracted.
fn claim(dir: &Path) -> Result<Option<Value>> {
    match fs::rename(dir.join(ANSWER), dir.join(CLAIMED)) {
        Ok(()) => read(dir, CLAIMED)
            .map(Some)
            .ok_or_else(|| "claimed answer is unreadable".into()),
        Err(error) if error.kind() == io::ErrorKind::NotFound => Ok(None),
        Err(error) => Err(error.to_string()),
    }
}

/// Withdraw the offer; false when the waiting process already claimed it.
fn retract(dir: &Path) -> Result<bool> {
    match fs::rename(dir.join(ANSWER), dir.join(RETRACTED)) {
        Ok(()) => Ok(true),
        Err(error) if error.kind() == io::ErrorKind::NotFound => Ok(false),
        Err(error) => Err(error.to_string()),
    }
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

pub(super) fn closed(dir: &Path) -> Option<Value> {
    read(dir, CLOSED)
}

pub(super) fn emitted(dir: &Path) -> bool {
    dir.join(EMITTED).exists()
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
}
