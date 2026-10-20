//! Replicated Zerus documents on Loro. This crate is the only code in Zerus
//! that imports `loro`: hgs and the mobile apps use its typed operations, so
//! a Loro upgrade or a schema change happens here once.
mod error;
pub mod ids;
mod ops;
pub mod phone;
mod project;
pub mod rank;
mod render;
mod schema;
mod store;
mod sync;
mod task;

pub use error::{Error, ErrorKind, Result};
pub use project::{ProjectDoc, Settings};
pub use render::{cancel_message, task_prompt, Tools};
pub use schema::{
    Actor, LogEntry, SessionKey, Stage, Target, TaskView, MAX_DOCUMENT, MAX_TEXT, SCHEMA,
};
pub use store::{Damaged, Position, ProjectStore};
pub use sync::{Document, DonePage, DoneRow, Imported, Inventory, Update, Version};
pub use task::{NewTask, TaskDoc};
