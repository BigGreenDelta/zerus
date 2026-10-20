#![cfg(windows)]
//! Native Windows filesystem, lock, configuration and generation controls.
//! Synthetic private state only; this suite neither installs nor contacts a relay.
use serde_json::json;
use std::path::PathBuf;
use zerus_mobile_connector::{
    config::Environment, journal::Journal, start, windows_management::manage,
};
struct Scratch(PathBuf, tempfile::TempDir);
impl Scratch {
    fn new() -> Self {
        Self::with_prefix("zerus-mobile-windows-")
    }
    fn with_prefix(prefix: &str) -> Self {
        let owner = tempfile::Builder::new().prefix(prefix).tempdir().unwrap();
        let path = owner.path().join("private");
        zerus_windows_private::directory(&path, true).unwrap();
        Self(path, owner)
    }
}
#[test]
fn equal_scratch_prefixes_keep_independent_owned_roots() {
    let first = Scratch::with_prefix("zerus-mobile-same-prefix-");
    let second = Scratch::with_prefix("zerus-mobile-same-prefix-");
    let first_root = first.1.path().to_owned();
    let second_root = second.1.path().to_owned();
    assert_ne!(first_root, second_root);
    let sentinel = second.0.join("sentinel");
    std::fs::write(&sentinel, b"second-owner").unwrap();
    drop(first);
    assert!(!first_root.exists());
    assert!(second_root.exists());
    assert_eq!(std::fs::read(&sentinel).unwrap(), b"second-owner");
    drop(second);
    assert!(!second_root.exists());
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
fn inheritable_world_allow_refuses_existing_database_before_sidecar_creation() {
    let scratch = Scratch::new();
    let state = scratch.0.join("receipts");
    drop(Journal::open(&state, "fixture", 1.0).unwrap());
    let db = state.join("journal.sqlite3");
    let lock = state.join("connector.lock");
    for file in [&db, &lock] {
        let protected = std::process::Command::new("icacls")
            .arg(file)
            .args(["/inheritance:d"])
            .output()
            .unwrap();
        assert!(protected.status.success());
        zerus_windows_private::open_file(file, false, false).unwrap();
    }
    let before = std::fs::read(&db).unwrap();
    for suffix in ["-wal", "-shm", "-journal"] {
        assert!(!state.join(format!("journal.sqlite3{suffix}")).exists());
    }
    let result = std::process::Command::new("icacls")
        .arg(&state)
        .args(["/grant", "*S-1-1-0:(OI)(CI)(IO)(R)"])
        .output()
        .unwrap();
    assert!(result.status.success());
    // Both existing files remain private. Refusal cannot accidentally come
    // from the lock inheriting the unsafe ACE instead of directory admission.
    for file in [&db, &lock] {
        zerus_windows_private::open_file(file, false, false).unwrap();
    }
    assert!(Journal::open(&state, "fixture", 2.0).is_err());
    assert_eq!(std::fs::read(&db).unwrap(), before);
    for suffix in ["-wal", "-shm", "-journal"] {
        assert!(!state.join(format!("journal.sqlite3{suffix}")).exists());
    }
}
#[test]
fn saved_revision_cannot_win_a_held_child_admission() {
    let scratch = Scratch::new();
    let path = scratch.0.join("mobile/connector.json");
    let first=manage("save", &path, &json!({"revision":"","server_url":"https://relay.example.test","node_token":"synthetic-token-a"})).unwrap();
    let revision = first["revision"].as_str().unwrap().to_owned();
    let (release, held) = std::sync::mpsc::channel();
    let child_path = path.clone();
    // The child is held just before the exact admission routine called by
    // --admit-revision. No runtime/native/network object is constructed here.
    let child = std::thread::spawn(move || {
        held.recv_timeout(std::time::Duration::from_secs(5))
            .unwrap();
        zerus_mobile_connector::start_bound(
            &child_path,
            false,
            &Environment::current().unwrap(),
            1.0,
            Some(&revision),
        )
        .is_err()
    });
    let second=manage("save", &path, &json!({"revision":first["revision"],"server_url":"https://relay.example.test","node_token":"synthetic-token-b"})).unwrap();
    release.send(()).unwrap();
    assert!(child.join().unwrap());
    assert_eq!(
        manage("status", &path, &json!({})).unwrap()["running"],
        false
    );
    for state in [&first["state_dir"], &second["state_dir"]] {
        let state = PathBuf::from(state.as_str().unwrap());
        for file in [
            "connector-control.json",
            "connector.lock",
            "journal.sqlite3",
        ] {
            assert!(!state.join(file).exists());
        }
    }
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
    assert_eq!(
        status["running"],
        true,
        "redacted status={status}; Started state={:?}; config={path:?} present={}; lock={:?} present={}; post-failure probe={:?}; state path matches={}",
        running.config.state_dir,
        path.exists(),
        running.config.state_dir.join("connector.lock"),
        running.config.state_dir.join("connector.lock").exists(),
        zerus_mobile_connector::fsguard::probe_lock(&running.config.state_dir.join("connector.lock")),
        status["state_dir"].as_str().map(PathBuf::from).as_ref() == Some(&running.config.state_dir),
    );
    assert_eq!(
        manage("start", &path, &json!({"revision":status["revision"]})).unwrap(),
        status
    );
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
