//! The built binary: `main()` vectors of `config.jsonl`, `--version`, the
//! run-mode startup refusals and the read-only `--check` report.
mod common;

use common::*;
use serde_json::{json, Value};
use std::os::unix::fs::{MetadataExt, PermissionsExt};
use std::path::Path;
use std::process::{Command, Output, Stdio};
use std::time::{Duration, Instant};
use zerus_mobile_connector::journal::{Journal, JOURNAL};
use zerus_mobile_connector::STARTUP_FAILURE;

const BINARY: &str = env!("CARGO_BIN_EXE_zerus-mobile-connector");
const RELAY: &str = "https://relay.example.test";
const TOKEN: &str = "fixture-node-token";

fn startup_line() -> String {
    format!("ERROR: {STARTUP_FAILURE}")
}

/// Runs the connector with only HOME and a system PATH, from `$CWD`. A start
/// that gets past validation is stopped after 10 s, so this never depends on
/// what the runtime does next.
fn connector(scratch: &Scratch, args: &[String]) -> (Option<i32>, String, String) {
    let mut child = Command::new(BINARY)
        .args(args)
        .env_clear()
        .env("HOME", scratch.path("home"))
        .env("PATH", "/usr/bin:/bin")
        .current_dir(scratch.path("cwd"))
        .stdin(Stdio::null())
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .spawn()
        .unwrap();
    let deadline = Instant::now() + Duration::from_secs(10);
    while child.try_wait().unwrap().is_none() && Instant::now() < deadline {
        std::thread::sleep(Duration::from_millis(10));
    }
    let running = child.try_wait().unwrap().is_none();
    if running {
        child.kill().unwrap();
    }
    let Output {
        status,
        stdout,
        stderr,
    } = child.wait_with_output().unwrap();
    let code = if running { None } else { status.code() };
    (
        code,
        String::from_utf8(stdout).unwrap(),
        String::from_utf8(stderr).unwrap(),
    )
}

fn scratch(label: &str) -> Scratch {
    umask();
    let scratch = Scratch::new(label);
    for name in ["home", "cwd"] {
        std::fs::create_dir(scratch.path(name)).unwrap();
        std::fs::set_permissions(scratch.path(name), std::fs::Permissions::from_mode(0o700))
            .unwrap();
    }
    scratch
}

fn write_config(scratch: &Scratch, text: &str, mode: u32) -> String {
    let path = scratch.path("connector.json");
    std::fs::write(&path, scratch.substitute(text)).unwrap();
    std::fs::set_permissions(&path, std::fs::Permissions::from_mode(mode)).unwrap();
    path.display().to_string()
}

fn valid(scratch: &Scratch) -> Value {
    json!({"server_url": RELAY, "node_token": TOKEN, "state_dir": scratch.path("state").display().to_string()})
}

#[test]
fn main_outcomes_match_the_python_connector() {
    let mut checked = 0;
    for record in records("config.jsonl")
        .into_iter()
        .filter(|record| record.get("argv").is_some())
    {
        let id = record["id"].as_str().unwrap();
        let scratch = scratch(&format!("main-{id}"));
        let mode = u32::from_str_radix(record["config_mode"].as_str().unwrap(), 8).unwrap();
        write_config(&scratch, record["config_text"].as_str().unwrap(), mode);
        let args: Vec<String> = record["argv"]
            .as_array()
            .unwrap()
            .iter()
            .map(|arg| scratch.substitute(arg.as_str().unwrap()))
            .collect();
        let (code, _, stderr) = connector(&scratch, &args);
        let expect = &record["expect"];
        let state = scratch.path("state");
        // Rust's reviewed outcome where it differs (README divergences e, f and j).
        let (exit, ran) = match id {
            "main-prefix-abbreviation" => (2, false),
            "main-world-writable-config" | "main-unknown-home-user" => (1, false),
            _ => {
                assert!(
                    record.get("divergence").is_none(),
                    "{id}: unreviewed divergence"
                );
                (
                    expect["exit"].as_i64().unwrap() as i32,
                    expect["ran"] == true,
                )
            }
        };
        if ran {
            // Validation passed and the journal is open; the runtime decides the rest.
            assert!(!stderr.contains(STARTUP_FAILURE), "{id}: {stderr}");
            assert_ne!(code, Some(2), "{id}");
            let stored: String = read_only(&state.join(JOURNAL))
                .query_row(
                    "SELECT value FROM metadata WHERE key='relay_binding'",
                    [],
                    |row| row.get(0),
                )
                .unwrap();
            assert_eq!(stored.len(), 64, "{id}");
        } else {
            assert_eq!(code, Some(exit), "{id}: {stderr}");
            if exit == 1 {
                assert_eq!(stderr.lines().collect::<Vec<_>>(), [startup_line()], "{id}");
            } else {
                assert!(
                    stderr.starts_with("usage: zerus-mobile-connector"),
                    "{id}: {stderr}"
                );
                assert!(!stderr.contains(STARTUP_FAILURE), "{id}");
            }
            assert!(
                !state.exists(),
                "{id}: a refused start created its state directory"
            );
        }
        assert!(!stderr.contains(TOKEN), "{id}: the token reached the log");
        checked += 1;
    }
    assert_eq!(checked, 14);
}

#[test]
fn version_and_help() {
    let scratch = scratch("version");
    let (code, stdout, stderr) = connector(&scratch, &["--version".into()]);
    assert_eq!(
        (code, stdout.as_str(), stderr.as_str()),
        (Some(0), "zerus-mobile-connector 0.1.0\n", "")
    );
    let (code, stdout, _) = connector(&scratch, &["--help".into()]);
    assert_eq!(code, Some(0));
    assert!(
        stdout.starts_with("usage: zerus-mobile-connector"),
        "{stdout}"
    );
}

/// Unsafe state is refused with the fixed line and never repaired.
#[test]
fn run_mode_refuses_unsafe_state_without_repair() {
    let scratch = scratch("unsafe");
    let outside = scratch.path("outside");
    std::fs::write(&outside, "synthetic private file").unwrap();
    std::fs::set_permissions(&outside, std::fs::Permissions::from_mode(0o600)).unwrap();
    let shared = scratch.path("shared");
    std::fs::create_dir(&shared).unwrap();
    std::fs::set_permissions(&shared, std::fs::Permissions::from_mode(0o755)).unwrap();
    let linked = scratch.path("linked");
    std::fs::create_dir(&linked).unwrap();
    std::fs::set_permissions(&linked, std::fs::Permissions::from_mode(0o700)).unwrap();
    std::os::unix::fs::symlink(&outside, linked.join(JOURNAL)).unwrap();
    for directory in [&shared, &linked] {
        let config = json!({"server_url": RELAY, "node_token": TOKEN, "state_dir": directory.display().to_string()});
        let path = write_config(&scratch, &config.to_string(), 0o600);
        let (code, _, stderr) = connector(&scratch, &["--config".into(), path]);
        assert_eq!(
            (code, stderr.trim_end()),
            (Some(1), startup_line().as_str()),
            "{}",
            directory.display()
        );
    }
    assert_eq!(std::fs::metadata(&shared).unwrap().mode() & 0o777, 0o755);
    assert_eq!(std::fs::read_dir(&shared).unwrap().count(), 0);
    assert_eq!(
        std::fs::read_to_string(&outside).unwrap(),
        "synthetic private file"
    );
    // A journal another connector holds is refused the same way.
    let held = Journal::open(&scratch.path("state"), "held-binding", EPOCH).unwrap();
    let path = write_config(&scratch, &valid(&scratch).to_string(), 0o600);
    let (code, _, stderr) = connector(&scratch, &["--config".into(), path]);
    assert_eq!(
        (code, stderr.trim_end()),
        (Some(1), startup_line().as_str())
    );
    held.close();
}

fn check(scratch: &Scratch, config: &str) -> (i32, Value) {
    let (code, stdout, stderr) = connector(
        scratch,
        &[
            "--check".into(),
            "--json".into(),
            "--config".into(),
            config.into(),
        ],
    );
    assert_eq!(stderr, "");
    (
        code.unwrap(),
        serde_json::from_str(&stdout).unwrap_or_else(|_| panic!("{stdout}")),
    )
}

/// Bytes, modification time and inode of every file below a directory.
fn fingerprint(directory: &Path) -> Vec<(String, Vec<u8>, i64, i64, u64)> {
    let mut found: Vec<_> = std::fs::read_dir(directory)
        .unwrap()
        .map(|entry| {
            let path = entry.unwrap().path();
            let metadata = path.symlink_metadata().unwrap();
            (
                path.display().to_string(),
                std::fs::read(&path).unwrap_or_default(),
                metadata.mtime(),
                metadata.mtime_nsec(),
                metadata.ino(),
            )
        })
        .collect();
    found.sort();
    found
}

#[test]
fn check_creates_nothing_and_reports_a_new_state_directory() {
    let scratch = scratch("check-new");
    let config = write_config(&scratch, &valid(&scratch).to_string(), 0o600);
    let before = tree(&scratch.root);
    let (code, report) = check(&scratch, &config);
    assert_eq!(code, 0, "{report}");
    assert_eq!(report["ok"], true);
    assert_eq!(report["configuration"]["valid"], true);
    assert_eq!(report["configuration"]["findings"], json!([]));
    assert_eq!(report["state"]["directory"], "absent");
    assert_eq!(report["state"]["journal"]["binding"], "unbound");
    assert_eq!(tree(&scratch.root), before);
    // The text form says the same.
    let (code, stdout, _) = connector(&scratch, &["--check".into(), "--config".into(), config]);
    assert_eq!(code, Some(0));
    assert!(
        stdout.contains("\nstate directory: absent\n") && stdout.ends_with("result: ok\n"),
        "{stdout}"
    );
}

#[test]
fn check_reports_reviewed_divergences_as_refusals() {
    let scratch = scratch("check-divergences");
    let state = scratch.path("state").display().to_string();
    let base = json!({"server_url": RELAY, "node_token": TOKEN, "state_dir": state});
    let changed = |key: &str, value: Value| {
        let mut config = base.clone();
        config[key] = value;
        config.to_string()
    };
    for (config, mode, divergence) in [
        (changed("poll_interval", json!(true)), 0o600, "k"),
        (changed("state_dir", json!(false)), 0o600, "k"),
        (
            changed("state_dir", json!("~nosuchuser-vectors/state")),
            0o600,
            "f",
        ),
        (
            changed("server_url", json!("https://r\u{e9}lay.example.test")),
            0o600,
            "i",
        ),
        (base.to_string(), 0o620, "e"),
    ] {
        let path = write_config(&scratch, &config, mode);
        let (code, report) = check(&scratch, &path);
        assert_eq!((code, &report["ok"]), (1, &json!(false)), "{report}");
        let findings = report["configuration"]["findings"].as_array().unwrap();
        assert!(findings.iter().any(|finding| finding["divergence"] == divergence && finding["level"] == "refusal"),
                "{report}");
        assert!(!scratch.path("state").exists());
    }
    // A readable file is only a warning.
    let path = write_config(&scratch, &base.to_string(), 0o644);
    let (code, report) = check(&scratch, &path);
    assert_eq!(code, 0, "{report}");
    assert_eq!(report["configuration"]["findings"][0]["level"], "warning");
    // Python refuses these too: no divergence, but the same refusal.
    let path = write_config(&scratch, &changed("node_token", json!("")), 0o600);
    let (code, report) = check(&scratch, &path);
    assert_eq!(
        (code, report["configuration"]["error"].as_str()),
        (1, Some("invalid node_token"))
    );
    assert_eq!(report["configuration"]["findings"], json!([]));
    let (code, report) = check(&scratch, &scratch.path("absent.json").display().to_string());
    assert_eq!(
        (code, report["configuration"]["valid"].as_bool()),
        (1, Some(false))
    );
}

#[test]
fn check_reads_a_legacy_journal_without_touching_it() {
    let scratch = scratch("check-legacy");
    let state = scratch.path("state");
    load_fixture(
        &state,
        &std::fs::read_to_string(vectors().join("journal/legacy-5-column.sql")).unwrap(),
    );
    let config = write_config(&scratch, &valid(&scratch).to_string(), 0o600);
    let before = fingerprint(&state);
    let (code, report) = check(&scratch, &config);
    assert_eq!(fingerprint(&state), before, "--check changed the journal");
    assert_eq!(code, 0, "{report}");
    let journal = &report["state"]["journal"];
    assert_eq!(
        (
            journal["status"].as_str(),
            journal["schema"].as_str(),
            journal["binding"].as_str()
        ),
        (Some("readable"), Some("legacy"), Some("unbound"))
    );
    let pending: Vec<&str> = journal["pending_migration"]
        .as_array()
        .unwrap()
        .iter()
        .map(|step| step.as_str().unwrap())
        .collect();
    assert_eq!(
        &pending[..7],
        [
            "create table metadata",
            "add column request_hash",
            "add column created_at",
            "add column updated_at",
            "add column delivered_at",
            "add column response_expired",
            "create index delivered_history"
        ]
    );
    assert_eq!(
        journal["rows"],
        json!({"total": 3, "running": 1, "undelivered": 2, "states": {
        "completed": {"delivered": 1, "undelivered": 1}, "running": {"undelivered": 1}}})
    );
    assert_eq!(report["state"]["lock"], "absent");
}

#[test]
fn check_refuses_a_foreign_binding_and_sees_a_held_lock() {
    let scratch = scratch("check-foreign");
    load_fixture(
        &scratch.path("state"),
        &std::fs::read_to_string(vectors().join("journal/foreign-binding.sql")).unwrap(),
    );
    let config = write_config(&scratch, &valid(&scratch).to_string(), 0o600);
    let (code, report) = check(&scratch, &config);
    assert_eq!(
        (code, report["state"]["journal"]["binding"].as_str()),
        (1, Some("differs")),
        "{report}"
    );

    let scratch = self::scratch("check-held");
    let binding = zerus_relay::json::digest(format!("{RELAY}\0{TOKEN}"));
    let journal = Journal::open(
        &scratch.path("state"),
        &binding,
        zerus_mobile_connector::now(),
    )
    .unwrap();
    journal
        .claim(
            "2f4a8091-31d2-5cc3-a524-9959d870c5f5",
            "send",
            Some(&"a".repeat(64)),
            zerus_mobile_connector::now(),
        )
        .unwrap();
    let config = write_config(&scratch, &valid(&scratch).to_string(), 0o600);
    let before = fingerprint(&scratch.path("state"));
    let (code, report) = check(&scratch, &config);
    assert_eq!(fingerprint(&scratch.path("state")), before);
    assert_eq!(code, 0, "{report}");
    assert_eq!(report["state"]["lock"], "held");
    let journal_report = &report["state"]["journal"];
    assert_eq!(
        (
            journal_report["binding"].as_str(),
            journal_report["schema"].as_str()
        ),
        (Some("matches"), Some("current"))
    );
    assert_eq!(journal_report["rows"]["running"], 1);
    journal.close();
    let (_, report) = check(&scratch, &config);
    assert_eq!(report["state"]["lock"], "free");
}

/// A rollback journal that no writer holds is hot: the next start recovers
/// it, and `--check` must neither recover nor delete it.
#[test]
fn check_reports_a_hot_journal_without_recovering_it() {
    let scratch = scratch("check-hot");
    let binding = zerus_relay::json::digest(format!("{RELAY}\0{TOKEN}"));
    let live = scratch.path("live");
    Journal::open(&live, &binding, EPOCH).unwrap().close();
    // Copy the database and its rollback journal mid-transaction: the copies
    // have no lock holder, which makes the journal hot.
    let writer = rusqlite::Connection::open(live.join(JOURNAL)).unwrap();
    writer
        .execute_batch("PRAGMA cache_size=1; BEGIN IMMEDIATE")
        .unwrap();
    for index in 0..200 {
        writer
            .execute(
                "INSERT INTO metadata VALUES(?, ?)",
                [format!("k{index}"), "x".repeat(500)],
            )
            .unwrap();
    }
    let state = scratch.path("state");
    std::fs::create_dir(&state).unwrap();
    std::fs::set_permissions(&state, std::fs::Permissions::from_mode(0o700)).unwrap();
    for name in [JOURNAL.to_owned(), format!("{JOURNAL}-journal")] {
        std::fs::copy(live.join(&name), state.join(&name)).unwrap();
        std::fs::set_permissions(state.join(&name), std::fs::Permissions::from_mode(0o600))
            .unwrap();
    }
    drop(writer);
    let config = write_config(&scratch, &valid(&scratch).to_string(), 0o600);
    let before = fingerprint(&state);
    let (code, report) = check(&scratch, &config);
    assert_eq!(
        fingerprint(&state),
        before,
        "--check recovered the hot journal"
    );
    assert_eq!(report["state"]["journal"]["status"], "hot", "{report}");
    assert_eq!(code, 0);
    // While a connector holds the lock, a journal that looks hot belongs to
    // its live write: the check asks to be run again instead.
    let lock = std::fs::OpenOptions::new()
        .read(true)
        .write(true)
        .create_new(true)
        .open(state.join("connector.lock"))
        .unwrap();
    std::fs::set_permissions(
        state.join("connector.lock"),
        std::fs::Permissions::from_mode(0o600),
    )
    .unwrap();
    rustix::fs::flock(&lock, rustix::fs::FlockOperation::NonBlockingLockExclusive).unwrap();
    let (code, report) = check(&scratch, &config);
    assert_eq!(
        (code, report["state"]["journal"]["status"].as_str()),
        (1, Some("busy")),
        "{report}"
    );
    assert_eq!(report["state"]["lock"], "held");
}
