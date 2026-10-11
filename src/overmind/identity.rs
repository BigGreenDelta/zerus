//! Who is calling: a person, or the agent of a verified Zerus session. This
//! guards against agent mistakes; it is not a security boundary.
use super::model::{Actor, Refusal};
use anyhow::Result;
use std::path::PathBuf;

/// A session environment that does not verify is refused, never a person.
pub fn caller(machine: &str) -> Result<(Actor, Option<PathBuf>)> {
    match crate::state::caller_session().map_err(|e| Refusal::new("session_mismatch", e))? {
        None => Ok((Actor::human(machine), None)),
        Some(session) => Ok((Actor::agent(machine, &session.name), Some(PathBuf::from(session.cwd)))),
    }
}
