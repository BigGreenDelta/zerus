//! Overmind: tasks whose content is a ready prompt, moved through a gated,
//! data-driven workflow. See docs/superpowers/specs/2026-10-11-overmind-core-design.md.
#![allow(dead_code)] // Removed in Task 7 once the CLI uses every part.
mod blobs;
mod fold;
mod model;
mod store;
mod workflow;
