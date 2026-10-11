//! Who is calling: a person, or the agent of a verified Zerus session. This
//! guards against agent mistakes; it is not a security boundary.
use super::model::Actor;
use anyhow::{anyhow, Result};
use std::path::PathBuf;

pub fn caller(machine: &str) -> Result<(Actor, Option<PathBuf>)> {
    match crate::state::caller_session().map_err(|e| anyhow!(e))? {
        None => Ok((Actor::human(machine), None)),
        Some(session) => Ok((Actor::agent(machine, &session.name), Some(PathBuf::from(session.cwd)))),
    }
}
