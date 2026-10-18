//! The analytics boundary accepts reviewed metadata, never arbitrary JSON bodies.
use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};

pub const HISTOGRAM_BOUNDS: [u64; 5] = [100, 1_000, 10_000, 60_000, 600_000];
pub const MAX_VALUE: u64 = 9_007_199_254_740_991;
pub const MEASURES: [&str; 10] = [
    "turns",
    "work_ms",
    "approval_wait_ms",
    "input_wait_ms",
    "delivery",
    "recovery",
    "failures",
    "tokens",
    "cost",
    "background_jobs",
];

#[derive(Clone, Copy, Debug, Deserialize, Serialize, PartialEq, Eq)]
#[serde(rename_all = "snake_case")]
pub enum Provider {
    Codex,
    Claude,
    Kimi,
    Dsh,
    Opencode,
    Other,
}
impl Provider {
    pub fn parse(value: &str) -> Self {
        match value {
            "codex" => Self::Codex,
            "claude" => Self::Claude,
            "kimi" => Self::Kimi,
            "dsh" => Self::Dsh,
            "opencode" => Self::Opencode,
            _ => Self::Other,
        }
    }
    pub fn name(self) -> &'static str {
        match self {
            Self::Codex => "codex",
            Self::Claude => "claude",
            Self::Kimi => "kimi",
            Self::Dsh => "dsh",
            Self::Opencode => "opencode",
            Self::Other => "other",
        }
    }
}
#[derive(Clone, Copy, Debug, Deserialize, Serialize)]
#[serde(rename_all = "snake_case")]
pub enum Producer {
    Main,
    Child,
}
impl Producer {
    pub fn name(self) -> &'static str {
        match self {
            Self::Main => "main",
            Self::Child => "child",
        }
    }
}

#[derive(Clone, Copy, Debug, Deserialize, Serialize)]
#[serde(rename_all = "snake_case")]
pub enum Failure {
    Authentication,
    RateLimit,
    Quota,
    Policy,
    Network,
    Service,
    Unknown,
}
impl Failure {
    pub fn counter(self) -> &'static str {
        match self {
            Self::Authentication => "failure_authentication",
            Self::RateLimit => "failure_rate_limit",
            Self::Quota => "failure_quota",
            Self::Policy => "failure_policy",
            Self::Network => "failure_network",
            Self::Service => "failure_service",
            Self::Unknown => "failure_unknown",
        }
    }
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Gap {
    Disabled,
    Purged,
    Retention,
    Quota,
    Observation,
    Clock,
    Identity,
    Provider,
}
impl Gap {
    pub fn name(self) -> &'static str {
        match self {
            Self::Disabled => "collection_disabled",
            Self::Purged => "history_purged",
            Self::Retention => "retention_boundary",
            Self::Quota => "quota_exhausted",
            Self::Observation => "observation_gap",
            Self::Clock => "clock_discontinuity",
            Self::Identity => "missing_identity",
            Self::Provider => "unsupported_provider",
        }
    }
}

/// Hash framed identity components before entering analytics storage. Never hash
/// text, paths, commands, prompts or account labels to manufacture an event ID.
#[derive(Clone, Debug)]
pub struct Identity(pub [u8; 32]);
impl Identity {
    pub fn new(parts: &[&str]) -> Self {
        let mut hash = Sha256::new();
        for part in parts {
            hash.update((part.len() as u64).to_be_bytes());
            hash.update(part.as_bytes());
        }
        Self(hash.finalize().into())
    }
}
#[derive(Clone, Copy, Debug)]
pub enum Interval {
    Work,
    Approval,
    Input,
}
impl Interval {
    pub fn metric(self) -> &'static str {
        match self {
            Self::Work => "work_ms",
            Self::Approval => "approval_wait_ms",
            Self::Input => "input_wait_ms",
        }
    }
    pub fn key(self) -> &'static str {
        match self {
            Self::Work => "work",
            Self::Approval => "approval",
            Self::Input => "input",
        }
    }
}
#[derive(Clone, Debug)]
pub struct Clock {
    pub generation: Identity,
    pub monotonic_ms: u64,
}
#[derive(Clone, Debug, Default, Deserialize, Serialize)]
pub struct Tokens {
    #[serde(skip_serializing_if = "Option::is_none")]
    pub input_tokens: Option<u64>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub output_tokens: Option<u64>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub cache_tokens: Option<u64>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub reasoning_tokens: Option<u64>,
}
impl Tokens {
    pub fn values(&self) -> [(&'static str, Option<u64>); 4] {
        [
            ("input_tokens", self.input_tokens),
            ("output_tokens", self.output_tokens),
            ("cache_tokens", self.cache_tokens),
            ("reasoning_tokens", self.reasoning_tokens),
        ]
    }
}
#[derive(Clone, Debug)]
pub enum Kind {
    TurnStarted,
    TurnCompleted,
    TurnInterrupted,
    WaitStarted(Interval),
    WaitResolved(Interval),
    DeliverySubmitted,
    DeliveryFailed,
    DeliveryUncertain,
    RecoveryAttempt,
    RecoverySucceeded,
    RecoveryExhausted,
    Failure(Failure),
    /// The first snapshot in an epoch establishes a baseline; historical totals
    /// are never assigned to today's bucket. Missing components stay unknown.
    Usage {
        epoch: Identity,
        counters: Tokens,
    },
    /// Exact provider request/message accounting. Unlike a lifetime cumulative
    /// gauge, its first value belongs to this one authoritative native event.
    RequestUsage {
        counters: Tokens,
    },
}
impl Kind {
    pub fn counter(&self) -> Option<&'static str> {
        Some(match self {
            Self::TurnStarted => "turn_started",
            Self::TurnCompleted => "turn_completed",
            Self::TurnInterrupted => "turn_interrupted",
            Self::DeliverySubmitted => "delivery_submitted",
            Self::DeliveryFailed => "delivery_failed",
            Self::DeliveryUncertain => "delivery_uncertain",
            Self::RecoveryAttempt => "recovery_attempt",
            Self::RecoverySucceeded => "recovery_succeeded",
            Self::RecoveryExhausted => "recovery_exhausted",
            Self::Failure(f) => f.counter(),
            Self::WaitStarted(Interval::Approval) => "approval_requested",
            Self::WaitStarted(Interval::Input) => "input_requested",
            Self::WaitResolved(Interval::Approval) => "approval_resolved",
            Self::WaitResolved(Interval::Input) => "input_resolved",
            _ => return None,
        })
    }
}
#[derive(Clone, Debug)]
pub struct Event {
    pub id: Identity,
    pub scope: Identity,
    pub provider: Provider,
    pub producer: Producer,
    /// Exact native turn or request identity, independently of event identity.
    pub correlation: Option<Identity>,
    pub observed_ms: i64,
    pub clock: Option<Clock>,
    pub kind: Kind,
}
