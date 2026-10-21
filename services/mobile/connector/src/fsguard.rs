//! Platform filesystem boundary; Unix retains the Python-compatible guards.
#[cfg(unix)]
#[path = "fsguard_unix.rs"]
mod implementation;
#[cfg(windows)]
#[path = "fsguard_windows.rs"]
mod implementation;
pub use implementation::*;
