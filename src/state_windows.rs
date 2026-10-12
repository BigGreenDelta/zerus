//! The local session host's client entry points for builds without one. A
//! Windows build manages sessions on remote machines only: `cli` rejects local
//! session commands with exit code 2 before any of these is reached, and each
//! one fails closed when it is called anyway.
use crate::platform::LOCAL_SESSIONS_UNSUPPORTED as UNSUPPORTED;
use serde_json::{json, Value};
use std::path::Path;

pub(crate) type Result<T> = std::result::Result<T, String>;

pub(crate) fn worktree_catalog(_: &Path) -> Value {
    json!({"state":"unavailable","detail":UNSUPPORTED,"stale":true,"worktrees":[]})
}
pub(crate) fn with_launch_binding<T>(
    _: &str,
    _: impl FnOnce(&Value) -> std::result::Result<T, anyhow::Error>,
) -> anyhow::Result<T> {
    anyhow::bail!(UNSUPPORTED)
}
pub(crate) fn account_context(_: &str) -> Result<Value> {
    Err(UNSUPPORTED.into())
}
pub fn merge_snapshot(_: Value) -> Result<Value> {
    Err(UNSUPPORTED.into())
}
pub fn find_requested_session(_: &str, _: &str) -> Result<Option<String>> {
    Err(UNSUPPORTED.into())
}
pub fn startup_failure(_: &str) -> Result<Option<String>> {
    Err(UNSUPPORTED.into())
}
pub fn check_requested_session(_: &str, _: &str, _: &str) -> Result<bool> {
    Err(UNSUPPORTED.into())
}
pub fn fork_session(
    _: &str,
    _: Option<&str>,
    _: Option<&str>,
    _: Option<&str>,
    _: Option<&str>,
    _: bool,
) -> Result<String> {
    Err(UNSUPPORTED.into())
}
pub(crate) fn reference_details(_: &str, _: &str, _: &str) -> Result<Value> {
    Err(UNSUPPORTED.into())
}
pub(crate) fn inspect_reference(_: &crate::session_reference::Reference) -> Result<Value> {
    Err(UNSUPPORTED.into())
}
pub fn dispatch(_: &[String]) -> Result<i32> {
    Err(UNSUPPORTED.into())
}

pub(crate) mod dsh {
    use super::{Result, UNSUPPORTED};
    use std::path::Path;

    pub(crate) fn exists(_: &str) -> bool {
        false
    }
    pub(crate) fn account_ui_url(_: &Path) -> Result<String> {
        Err(UNSUPPORTED.into())
    }
    pub(crate) fn native_ui_url(_: &str) -> Result<String> {
        Err(UNSUPPORTED.into())
    }
    pub(crate) fn launch(_: &str, _: &str, _: Option<&str>, _: bool, _: bool) -> Result<()> {
        Err(UNSUPPORTED.into())
    }
}
