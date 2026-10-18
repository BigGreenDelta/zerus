//! The task schema shared by every replica.
use crate::{Error, Result};
use serde::{Deserialize, Serialize};

/// The layout version of task and project documents this build writes.
pub const SCHEMA: i64 = 1;
/// The longest prompt or comment: the input limit of hgs.
pub const MAX_TEXT: usize = 64 * 1024;
/// A task holding this much text (its prompt and history) accepts no further comments.
pub const MAX_DOCUMENT: usize = 4 * 1024 * 1024;

#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum Stage {
    Backlog,
    Todo,
    Active,
    Review,
    Done,
}

impl Stage {
    pub fn as_str(self) -> &'static str {
        match self {
            Self::Backlog => "backlog",
            Self::Todo => "todo",
            Self::Active => "active",
            Self::Review => "review",
            Self::Done => "done",
        }
    }

    pub fn label(self) -> &'static str {
        match self {
            Self::Backlog => "Backlog",
            Self::Todo => "To do",
            Self::Active => "In progress",
            Self::Review => "In review",
            Self::Done => "Done",
        }
    }

    pub fn parse(value: &str) -> Result<Self> {
        Ok(match value {
            "backlog" => Self::Backlog,
            "todo" => Self::Todo,
            "active" => Self::Active,
            "review" => Self::Review,
            "done" => Self::Done,
            _ => return Err(Error::new(format!("unknown stage {value:?}"))),
        })
    }
}

/// A session on a computer: the computer's swarm identity and the session name.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct SessionKey {
    pub machine: String,
    pub session: String,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(tag = "kind", rename_all = "lowercase")]
pub enum Target {
    Any,
    Session(SessionKey),
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(tag = "kind", rename_all = "lowercase")]
pub enum Actor {
    Person,
    Agent(SessionKey),
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct LogEntry {
    pub kind: String,
    pub actor: Actor,
    pub at: i64,
    #[serde(default, skip_serializing_if = "String::is_empty")]
    pub text: String,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub session: Option<SessionKey>,
    /// A send's durable hgs request, for matching it to the paste receipt.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub request_id: Option<String>,
}

/// A task as callers read it.
#[derive(Clone, Debug, PartialEq, Eq, Serialize)]
pub struct TaskView {
    pub uuid: String,
    pub id: String,
    pub project: String,
    pub schema: i64,
    pub writable: bool,
    pub created_at: i64,
    /// When the task entered its current stage (Unix milliseconds).
    pub stage_at: i64,
    pub author: Actor,
    pub stage: Stage,
    pub target: Target,
    pub rank: String,
    pub session: Option<SessionKey>,
    pub summary: String,
    pub cancelled_at: Option<i64>,
    pub cancel_reason: String,
    pub text: String,
    pub log: Vec<LogEntry>,
}

pub(crate) fn check_text(text: &str, what: &str) -> Result<()> {
    if text.trim().is_empty() {
        return Err(Error::new(format!("the {what} is empty")));
    }
    if text.len() > MAX_TEXT {
        return Err(Error::new(format!("the {what} is longer than 64 KiB")));
    }
    check_controls(text, what)
}

/// hgs pastes only text: terminal control characters never reach an agent.
pub(crate) fn check_controls(text: &str, what: &str) -> Result<()> {
    if text
        .chars()
        .any(|c| c.is_control() && c != '\n' && c != '\t')
    {
        return Err(Error::new(format!(
            "the {what} contains terminal control characters"
        )));
    }
    Ok(())
}
