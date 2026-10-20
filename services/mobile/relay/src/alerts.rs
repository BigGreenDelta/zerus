//! Presence-aware phone alert rule. Pure functions take the caller's clock.
//! Missing, stale or malformed information means "away": an extra phone
//! alert is preferred over a missed one.
use crate::registry::archived;
use serde_json::Value;

/// Seconds a machine's latest presence keeps it present.
pub const PRESENCE_FRESH: f64 = 120.0;
/// Input idle time after which an unlocked desktop stops counting as active.
pub const IDLE_AWAY: f64 = 120.0;
/// Candidates older than this are discarded without delivery.
pub const CANDIDATE_TTL: f64 = 86400.0;
/// Waiting candidates are evaluated again at least this often.
pub const RECHECK: f64 = 15.0;
/// One candidate per computer, session and kind; this bounds a workspace.
pub const WORKSPACE_CANDIDATES: i64 = 1000;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Delivery {
    Immediate,
    /// Deliver once the owner has been away for this many minutes.
    Away(u32),
}
impl Delivery {
    pub const DEFAULT: Self = Self::Away(0);
    pub fn parse(name: &str) -> Option<Self> {
        Some(match name {
            "immediate" => Self::Immediate,
            "away" => Self::Away(0),
            "away_5" => Self::Away(5),
            "away_10" => Self::Away(10),
            "away_15" => Self::Away(15),
            "away_30" => Self::Away(30),
            _ => return None,
        })
    }
    /// The swarm-wide choice carried by a gateway machine snapshot.
    pub fn from_snapshot(snapshot: &Value) -> Option<Self> {
        snapshot["preferences"]["mobile_delivery"]
            .as_str()
            .and_then(Self::parse)
    }
    pub fn name(self) -> &'static str {
        match self {
            Self::Immediate => "immediate",
            Self::Away(5) => "away_5",
            Self::Away(10) => "away_10",
            Self::Away(15) => "away_15",
            Self::Away(30) => "away_30",
            Self::Away(_) => "away",
        }
    }
}

/// One machine's latest presence, converted with the relay receipt clock so
/// machine clock skew does not matter.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Presence {
    pub received: f64,
    /// End of the last activity: the idle threshold crossing, or the lock time when earlier.
    pub away_since: f64,
}
impl Presence {
    /// Reads the heartbeat `presence` object (the listing's `desktop_presence`).
    pub fn from_value(p: &Value, received: f64) -> Option<Self> {
        let seconds = |v: &Value| v.as_f64().filter(|s| s.is_finite() && *s >= 0.0);
        let idle = seconds(&p["idle_seconds"])?;
        let mut away_since = received - idle + IDLE_AWAY;
        if p["locked"].as_bool()? {
            // An unreported lock time is bounded by the last input.
            let lock = if p["locked_seconds"].is_null() {
                idle
            } else {
                seconds(&p["locked_seconds"])?
            };
            away_since = away_since.min(received - lock);
        }
        Some(Self {
            received,
            away_since,
        })
    }
}

/// T: the end of the owner's last activity over every machine with remembered
/// presence. Presence older than `PRESENCE_FRESH` can no longer make the owner
/// active, but its last activity still counts. `None`: never received.
pub fn away_since(machines: &[Presence]) -> Option<f64> {
    machines
        .iter()
        .map(|m| m.away_since.min(m.received + PRESENCE_FRESH))
        .reduce(f64::max)
}

#[derive(Clone, Copy, Debug, PartialEq)]
pub enum Decision {
    Deliver,
    /// Keep waiting; evaluate again no later than this time.
    Wait(f64),
}
pub fn decide(delivery: Delivery, away_since: Option<f64>, now: f64) -> Decision {
    let due = match (delivery, away_since) {
        (Delivery::Immediate, _) | (_, None) => return Decision::Deliver,
        (Delivery::Away(minutes), Some(since)) => since + f64::from(minutes) * 60.0,
    };
    if now >= due {
        Decision::Deliver
    } else {
        Decision::Wait(due.min(now + RECHECK))
    }
}

/// Recovery states the phone shows as errors.
pub fn recovery_failed(v: &Value) -> bool {
    matches!(
        v["recovery"]["state"].as_str(),
        Some("uncertain" | "blocked" | "exhausted")
    )
}
/// Whether the native session row still shows the need behind this kind,
/// using the phone's input/approval and error definitions.
pub fn native_need(v: &Value, kind: &str) -> bool {
    if kind == "error" {
        (v["phase"] == "error"
            && !matches!(
                v["recovery"]["state"].as_str(),
                Some("waiting" | "dispatching" | "retrying")
            ))
            || recovery_failed(v)
    } else {
        matches!(v["phase"].as_str(), Some("approval" | "input"))
            || v["mobile_attention"]
                .as_array()
                .is_some_and(|a| !a.is_empty())
            || v["pending_question_count"].as_u64().is_some_and(|n| n > 0)
    }
}

/// The live (non-archived) session row with this name.
pub fn live<'a>(snapshot: &'a Value, session: &str) -> Option<&'a Value> {
    snapshot["sessions"]
        .as_array()?
        .iter()
        .find(|v| v["name"] == session && !archived(v))
}
fn text(v: &Value, key: &str) -> Option<String> {
    v[key].as_str().map(str::to_owned)
}

/// Notification suppression never changes the underlying native/read state.
/// A preference for a previous conversation cannot mute a reused session name.
pub fn notification_preferences(v: &Value) -> Option<&Value> {
    let p = &v["notification_preferences"];
    (p["conversation_id"]
        .as_str()
        .is_some_and(|id| !id.is_empty())
        && p["conversation_id"] == v["conversation_id"]
        && p["muted"].is_boolean()
        && p["revision"].as_i64().is_some_and(|r| r >= 0)
        && p["suppressed"].is_object())
    .then_some(p)
}

pub fn notifications_muted(v: &Value) -> bool {
    notification_preferences(v).is_some_and(|p| p["muted"] == true)
}

/// The identity a derived event had when it became a candidate.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Candidate {
    pub session: String,
    pub kind: String,
    pub run_id: Option<String>,
    pub conversation_id: Option<String>,
    pub attention_signature: Option<String>,
    pub reply_id: Option<String>,
}
impl Candidate {
    pub fn capture(snapshot: &Value, session: &str, kind: &str) -> Self {
        let row = live(snapshot, session).unwrap_or(&Value::Null);
        let completed = kind == "completed";
        Self {
            session: session.into(),
            kind: kind.into(),
            run_id: text(row, "run_id"),
            conversation_id: text(row, "conversation_id"),
            attention_signature: if completed {
                None
            } else {
                text(row, "attention_signature")
            },
            reply_id: if completed {
                text(row, "reply_id")
            } else {
                None
            },
        }
    }
    /// Reads a stored `alert_candidates` row.
    pub fn from_row(r: &Value) -> Self {
        Self {
            session: text(r, "session").unwrap_or_default(),
            kind: text(r, "kind").unwrap_or_default(),
            run_id: text(r, "run_id"),
            conversation_id: text(r, "conversation_id"),
            attention_signature: text(r, "attention_signature"),
            reply_id: text(r, "reply_id"),
        }
    }
    /// True when the current live row proves the item was handled or read.
    /// `None` means the snapshot no longer has the live session.
    pub fn resolved(&self, session: Option<&Value>) -> bool {
        let Some(v) = session else {
            return true;
        };
        let current = |key: &str| v[key].as_str();
        if current("run_id") != self.run_id.as_deref()
            || current("conversation_id") != self.conversation_id.as_deref()
        {
            return true;
        }
        if let Some(p) = notification_preferences(v) {
            if p["muted"] == true {
                return true;
            }
            let baseline = &p["suppressed"];
            if self.kind == "completed"
                && self.reply_id.is_some()
                && baseline["reply_id"].as_str() == self.reply_id.as_deref()
            {
                return true;
            }
            if self.kind != "completed"
                && baseline["attention_signature"]
                    .as_str()
                    .is_some_and(|signature| {
                        !signature.is_empty()
                            && (Some(signature) == self.attention_signature.as_deref()
                                || Some(signature) == current("attention_signature"))
                    })
            {
                return true;
            }
        }
        // A mark for another conversation never acknowledges this one.
        let read = if v["read"]["conversation_id"].as_str() == self.conversation_id.as_deref() {
            &v["read"]
        } else {
            &Value::Null
        };
        if self.kind == "completed" {
            return v["activity"] == "busy"
                || crate::registry::explicit_wait(v)
                || current("reply_id") != self.reply_id.as_deref()
                || (self.reply_id.is_some()
                    && read["reply_id"].as_str() == self.reply_id.as_deref());
        }
        let Some(captured) = self.attention_signature.as_deref() else {
            // Without a signature only the native state resolves it; read marks never do.
            return !native_need(v, &self.kind);
        };
        let Some(signature) = current("attention_signature") else {
            // hgs reports nothing that needs the owner any more.
            return true;
        };
        // The signature also changes without a relay event (subagent requests,
        // request start times, unindexed questions), so a change alone proves
        // nothing; a real replacement event replaces the row instead. Reading
        // either the captured or the newer state still acknowledges it.
        let read = read["attention_signature"].as_str();
        read == Some(captured)
            || read == Some(signature)
            || (signature != captured && !native_need(v, &self.kind))
    }
}
