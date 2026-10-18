//! Replicated Zerus documents on Loro. This crate is the only code in Zerus
//! that imports `loro`: hgs and the mobile apps use its typed operations, so
//! a Loro upgrade or a schema change happens here once.
mod error;
pub mod ids;
pub mod rank;

pub use error::{Error, Result};
