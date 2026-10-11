//! Overmind events. Stored events are parsed leniently so that fields added by
//! a newer machine survive on an older one; requests are parsed strictly.
use serde::{Deserialize, Serialize};
use serde_json::Value;

/// The newest event payload version this build understands.
pub const EVENT_VERSION: u32 = 1;

#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum ActorKind {
    Human,
    Agent,
    Autopilot,
    Manager,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct Actor {
    pub kind: ActorKind,
    pub machine: String,
    #[serde(default, skip_serializing_if = "String::is_empty")]
    pub session: String,
    #[serde(default, skip_serializing_if = "String::is_empty")]
    pub device: String,
}

impl Actor {
    pub fn human(machine: &str) -> Self {
        Self { kind: ActorKind::Human, machine: machine.into(), session: String::new(), device: String::new() }
    }
    pub fn agent(machine: &str, session: &str) -> Self {
        Self { kind: ActorKind::Agent, machine: machine.into(), session: session.into(), device: String::new() }
    }
    /// The session an agent or manager acts for.
    pub fn session_ref(&self) -> Option<SessionRef> {
        if self.session.is_empty() {
            return None;
        }
        Some(SessionRef { machine: self.machine.clone(), name: self.session.clone() })
    }
}

#[derive(Clone, Debug, PartialEq, Eq, PartialOrd, Ord, Hash, Serialize, Deserialize)]
pub struct SessionRef {
    pub machine: String,
    pub name: String,
}

#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
pub struct Event {
    pub id: String,
    pub origin: String,
    pub seq: u64,
    pub lamport: u64,
    pub at: u64,
    pub v: u32,
    pub actor: Actor,
    #[serde(default)]
    pub task: String,
    #[serde(default)]
    pub project: String,
    pub kind: String,
    #[serde(default)]
    pub base: String,
    #[serde(default)]
    pub request_id: String,
    #[serde(default)]
    pub payload: Value,
}

impl Event {
    /// The total order in which every machine applies events.
    pub fn order(&self) -> (u64, &str, u64) {
        (self.lamport, self.origin.as_str(), self.seq)
    }
}

/// An event a command wants to append; the store assigns identity and clocks.
#[derive(Clone, Debug)]
pub struct Draft {
    pub actor: Actor,
    pub task: String,
    pub project: String,
    pub kind: String,
    pub base: String,
    pub request_id: String,
    pub payload: Value,
}

pub fn now() -> u64 {
    std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .unwrap_or_default()
        .as_secs()
}

/// A refused action. `state` says how a field arrived: missing, empty or invalid.
#[derive(Clone, Debug, PartialEq, Eq, Serialize)]
pub struct Refusal {
    pub code: String,
    pub message: String,
    #[serde(skip_serializing_if = "String::is_empty")]
    pub field: String,
    #[serde(skip_serializing_if = "String::is_empty")]
    pub state: String,
}

impl Refusal {
    pub fn new(code: &str, message: impl Into<String>) -> Self {
        Self { code: code.into(), message: message.into(), field: String::new(), state: String::new() }
    }
    pub fn field(code: &str, field: &str, state: &str, message: impl Into<String>) -> Self {
        Self { code: code.into(), message: message.into(), field: field.into(), state: state.into() }
    }
}

impl std::fmt::Display for Refusal {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        self.message.fmt(f)
    }
}

impl std::error::Error for Refusal {}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    #[test]
    fn stored_events_tolerate_unknown_fields() {
        let event: Event = serde_json::from_value(json!({
            "id": "e", "origin": "o", "seq": 1, "lamport": 1, "at": 0, "v": 2,
            "actor": {"kind": "human", "machine": "m", "future": true},
            "kind": "future_kind", "payload": {}, "future_field": 1
        }))
        .unwrap();
        assert_eq!(event.v, 2);
        assert_eq!(event.actor.kind, ActorKind::Human);
    }

    #[test]
    fn refusal_serializes_only_present_fields() {
        let plain = serde_json::to_value(Refusal::new("stale", "changed")).unwrap();
        assert_eq!(plain, json!({"code": "stale", "message": "changed"}));
        let field = serde_json::to_value(Refusal::field("missing_field", "spec", "missing", "m")).unwrap();
        assert_eq!(field["field"], "spec");
        assert_eq!(field["state"], "missing");
    }
}
