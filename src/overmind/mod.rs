//! Overmind: tasks whose content is a ready prompt, moved through a gated,
//! data-driven workflow. See docs/superpowers/specs/2026-10-11-overmind-core-design.md.
mod agent;
mod blobs;
mod cli;
mod commands;
mod fold;
mod identity;
mod model;
mod rank;
mod refs;
mod sessions;
mod store;
mod workflow;

pub use cli::dispatch;
pub use sessions::session_renamed;
