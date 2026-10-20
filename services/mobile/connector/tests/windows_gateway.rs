#![cfg(windows)]
//! Native Windows filesystem, lock, configuration and generation controls.
//! Synthetic private state only; this suite neither installs nor contacts a relay.
use serde_json::json;
use std::path::PathBuf;
use zerus_mobile_connector::{
    config::Environment, journal::Journal, start, windows_management::manage,
};
struct Scratch(PathBuf);
impl Scratch {
    fn new() -> Self {
        let path = std::env::temp_dir().join(format!(
            "zerus-mobile-windows-{}-{}",
            std::process::id(),
            std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .unwrap()
                .as_nanos()
        ));
        zerus_windows_private::directory(&path, true).unwrap();
        Self(path)
    }
}
impl Drop for Scratch {
    fn drop(&mut self) {
        let _ = std::fs::remove_dir_all(&self.0);
    }
}
#[test]
fn private_files_reject_hardlinks_and_world_readable_credentials() {
    let scratch = Scratch::new();
    let path = scratch.0.join("credential.json");
    let file = zerus_windows_private::open_file(&path, true, true).unwrap();
    zerus_windows_private::verify(&file, false).unwrap();
    drop(file);
    let alias = scratch.0.join("alias");
    std::fs::hard_link(&path, &alias).unwrap();
    assert!(zerus_windows_private::open_file(&path, false, false).is_err());
    std::fs::remove_file(alias).unwrap();
    let result = std::process::Command::new("icacls")
        .arg(&path)
        .args(["/grant", "*S-1-1-0:(R)"])
        .output()
        .unwrap();
    assert!(result.status.success());
    assert!(zerus_windows_private::open_file(&path, false, false).is_err());
}
#[test]
fn journal_lock_has_one_writer_and_preserves_binding() {
    let scratch = Scratch::new();
    let state = scratch.0.join("receipts");
    let first = Journal::open(&state, "original-binding", 1.0).unwrap();
    assert!(Journal::open(&state, "original-binding", 2.0).is_err());
    assert_eq!(
        zerus_mobile_connector::fsguard::probe_lock(&state.join("connector.lock")).name(),
        "held"
    );
    drop(first);
    assert!(Journal::open(&state, "foreign-binding", 3.0).is_err());
    assert!(Journal::open(&state, "original-binding", 4.0).is_ok());
}
#[test]
fn configuration_is_redacted_revision_guarded_and_stop_is_generation_bound() {
    let scratch = Scratch::new();
    let path = scratch.0.join("mobile/connector.json");
    let missing = manage("status", &path, &json!({})).unwrap();
    assert_eq!(missing["configured"], false);
    assert!(!path.parent().unwrap().exists());
    let saved=manage("save",&path,&json!({"revision":"","server_url":"https://relay.example.test","node_token":"synthetic-windows-node-token"})).unwrap();
    assert_eq!(saved["configured"], true);
    assert!(!saved.to_string().contains("synthetic-windows-node-token"));
    assert!(manage(
        "save",
        &path,
        &json!({"revision":"","server_url":"https://relay.example.test","node_token":""})
    )
    .is_err());
    let environment = Environment::current().unwrap();
    let running = start(&path, false, &environment, 1.0).unwrap();
    let status = manage("status", &path, &json!({})).unwrap();
    assert_eq!(status["running"], true);
    assert!(manage("save",&path,&json!({"revision":status["revision"],"server_url":"https://relay.example.test","node_token":""})).is_err());
    assert!(manage(
        "stop",
        &path,
        &json!({"revision":status["revision"],"generation":"stale"})
    )
    .is_err());
    let stopped = manage(
        "stop",
        &path,
        &json!({"revision":status["revision"],"generation":status["generation"]}),
    )
    .unwrap();
    assert_eq!(stopped["stopping"], true);
    drop(running);
    let restarted = start(&path, false, &environment, 2.0).unwrap();
    let fresh = manage("status", &path, &json!({})).unwrap();
    assert_ne!(fresh["generation"], status["generation"]);
    assert!(manage(
        "stop",
        &path,
        &json!({"revision":status["revision"],"generation":status["generation"]})
    )
    .is_err());
    drop(restarted);
    let rotated=manage("save",&path,&json!({"revision":saved["revision"],"server_url":"https://relay.example.test","node_token":"synthetic-replacement-token"})).unwrap();
    assert_ne!(rotated["state_dir"], saved["state_dir"]);
    assert!(PathBuf::from(saved["state_dir"].as_str().unwrap())
        .join("journal.sqlite3")
        .exists());
}
