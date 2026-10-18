//! Replicated Zerus documents on Loro. This crate is the only code in Zerus
//! that imports `loro`: hgs and the mobile apps use its typed operations, so
//! a Loro upgrade or a schema change happens here once.
mod error;
pub mod ids;
pub mod rank;
mod schema;
mod task;

pub use error::{Error, Result};
pub use schema::{
    Actor, LogEntry, SessionKey, Stage, Target, TaskView, MAX_DOCUMENT, MAX_TEXT, SCHEMA,
};
pub use task::{NewTask, TaskDoc};
