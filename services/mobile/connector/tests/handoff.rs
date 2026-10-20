#![cfg(unix)]
//! The Rust and Python connectors on one journal: each lock excludes the
//! other in both directions, `hgs status` sees the Rust lock, a Rust-written
//! journal reopens under the Python `Journal` unchanged, and `--check` never
//! makes a writing Python journal fail.
//!
//! `ZERUS_CONNECTOR_TEST_PYTHON` names an interpreter with aiohttp (the
//! Python connector's only import outside this tree); `ZERUS_CONNECTOR_TEST_HGS`
//! names a built `hgs`. Without them these tests skip, except in CI.
mod common;

use common::*;
use serde_json::{json, Value};
use std::io::{BufRead, BufReader, Write};
use std::os::unix::fs::PermissionsExt;
use std::path::{Path, PathBuf};
use std::process::{Child, Command, Stdio};
use zerus_mobile_connector::error::IN_USE;
use zerus_mobile_connector::journal::{Journal, JournalError, JOURNAL};

const BINARY: &str = env!("CARGO_BIN_EXE_zerus-mobile-connector");
const RELAY: &str = "https://relay.example.test";
const TOKEN: &str = "fixture-node-token";

/// The Python `Journal`, driven from a test: `open` opens and closes it,
/// `hold` keeps it until stdin closes, `write` claims, finishes and delivers
/// requests until a stop file appears.
const PYTHON: &str = r#"
import os, sys, uuid
from pathlib import Path
from zerus_mobile.connector import ConnectorError, Journal
mode, directory, binding = sys.argv[1:4]
try:
    journal = Journal(Path(directory), binding)
except ConnectorError as error:
    print(error, flush=True)
    sys.exit(3)
written = 0
if mode != "open":
    print("ready", flush=True)
if mode == "hold":
    sys.stdin.read()
elif mode == "write":
    while not os.path.exists(sys.argv[4]):
        request_id = str(uuid.uuid4())
        journal.claim(request_id, "send", "a" * 64)
        journal.finish(request_id, {"state": "completed", "result": {"status": "submitted"}, "error": None})
        journal.delivered(request_id)
        written += 1
journal.close()
print(written, flush=True)
"#;

fn required(variable: &str) -> Option<PathBuf> {
    match std::env::var_os(variable) {
        Some(value) => Some(PathBuf::from(value)),
        None if std::env::var("CI").as_deref() == Ok("true") => {
            panic!("{variable} is required in CI")
        }
        None => {
            eprintln!("skipped: set {variable}");
            None
        }
    }
}

fn binding() -> String {
    zerus_relay::json::digest(format!("{RELAY}\0{TOKEN}"))
}

fn python(interpreter: &Path, args: &[&str]) -> Command {
    let mut command = Command::new(interpreter);
    command
        .arg("-c")
        .arg(PYTHON)
        .args(args)
        .env_clear()
        .env("PATH", "/usr/bin:/bin")
        .env(
            "PYTHONPATH",
            Path::new(env!("CARGO_MANIFEST_DIR")).join(".."),
        )
        .env("PYTHONDONTWRITEBYTECODE", "1");
    command
}

/// Starts a holding or writing Python journal and waits until it is open.
fn start_python(
    interpreter: &Path,
    args: &[&str],
) -> (Child, BufReader<std::process::ChildStdout>) {
    let mut child = spawn(
        python(interpreter, args)
            .stdin(Stdio::piped())
            .stdout(Stdio::piped()),
    );
    let mut stdout = BufReader::new(child.stdout.take().unwrap());
    let mut line = String::new();
    stdout.read_line(&mut line).unwrap();
    assert_eq!(line, "ready\n", "the Python journal did not open");
    (child, stdout)
}

fn scratch(label: &str) -> Scratch {
    umask();
    Scratch::new(label)
}

/// One `--check --json` run against `state`, unasserted.
fn run_check(scratch: &Scratch, state: &Path) -> std::process::Output {
    let path = scratch.path("connector.json");
    let config =
        json!({"server_url": RELAY, "node_token": TOKEN, "state_dir": state.display().to_string()});
    std::fs::write(&path, config.to_string()).unwrap();
    std::fs::set_permissions(&path, std::fs::Permissions::from_mode(0o600)).unwrap();
    captured(
        Command::new(BINARY)
            .args(["--check", "--json", "--config"])
            .arg(&path)
            .env_clear()
            .env("HOME", &scratch.root)
            .env("PATH", "/usr/bin:/bin"),
    )
}

fn check(scratch: &Scratch, state: &Path) -> Value {
    let output = run_check(scratch, state);
    assert_eq!(
        output.status.code(),
        Some(0),
        "{}",
        String::from_utf8_lossy(&output.stdout)
    );
    serde_json::from_slice(&output.stdout).unwrap()
}

#[test]
fn python_refuses_a_journal_rust_holds() {
    let Some(interpreter) = required("ZERUS_CONNECTOR_TEST_PYTHON") else {
        return;
    };
    let scratch = scratch("handoff-rust-holds");
    let state = scratch.path("state");
    let directory = state.display().to_string();
    let held = Journal::open(&state, &binding(), zerus_mobile_connector::now()).unwrap();
    let output = captured(&mut python(&interpreter, &["open", &directory, &binding()]));
    assert_eq!(
        (
            output.status.code(),
            String::from_utf8_lossy(&output.stdout).trim()
        ),
        (Some(3), IN_USE),
        "{}",
        String::from_utf8_lossy(&output.stderr)
    );
    close(held);
    let output = captured(&mut python(&interpreter, &["open", &directory, &binding()]));
    assert_eq!(
        output.status.code(),
        Some(0),
        "{}",
        String::from_utf8_lossy(&output.stderr)
    );
}

#[test]
fn rust_refuses_a_journal_python_holds() {
    let Some(interpreter) = required("ZERUS_CONNECTOR_TEST_PYTHON") else {
        return;
    };
    let scratch = scratch("handoff-python-holds");
    let state = scratch.path("state");
    let (mut child, _stdout) = start_python(
        &interpreter,
        &["hold", &state.display().to_string(), &binding()],
    );
    match Journal::open(&state, &binding(), zerus_mobile_connector::now()) {
        Err(JournalError::Refused(error)) => assert_eq!(error.message(), IN_USE),
        Err(other) => panic!("{other:?}"),
        Ok(_) => panic!("Rust opened a journal that Python holds"),
    }
    let report = check(&scratch, &state);
    assert_eq!(
        (
            report["state"]["lock"].as_str(),
            report["state"]["journal"]["binding"].as_str()
        ),
        (Some("held"), Some("matches"))
    );
    drop(child.stdin.take());
    assert!(child.wait().unwrap().success());
    Journal::open(&state, &binding(), zerus_mobile_connector::now())
        .unwrap()
        .close();
}

#[test]
fn rust_written_journals_reopen_under_python_unchanged() {
    let Some(interpreter) = required("ZERUS_CONNECTOR_TEST_PYTHON") else {
        return;
    };
    for legacy in [false, true] {
        let scratch = scratch("handoff-reopen");
        let state = scratch.path("state");
        if legacy {
            load_fixture(
                &state,
                &std::fs::read_to_string(vectors().join("journal/legacy-5-column.sql")).unwrap(),
            );
        }
        let now = zerus_mobile_connector::now();
        let journal = Journal::open(&state, &binding(), now).unwrap();
        for (index, operation) in ["send", "inspect", "terminal_input"]
            .into_iter()
            .enumerate()
        {
            let request_id = format!("2f4a8091-31d2-5cc3-a524-99590000000{index}");
            assert!(journal
                .claim(&request_id, operation, Some(&"a".repeat(64)), now)
                .unwrap());
            let receipt = json!({"state": "completed", "result": {"text": "caf\u{e9}", "n": index}, "error": null});
            journal.finish(&request_id, &receipt, now).unwrap();
            if index > 0 {
                journal.delivered(&request_id, now).unwrap();
            }
        }
        close(journal);
        let path = state.join(JOURNAL);
        let (schema_before, rows_before) = (schema(&path), rows(&path));
        let output = captured(&mut python(
            &interpreter,
            &["open", &state.display().to_string(), &binding()],
        ));
        assert_eq!(
            output.status.code(),
            Some(0),
            "{}",
            String::from_utf8_lossy(&output.stderr)
        );
        assert_eq!(schema(&path), schema_before, "legacy {legacy}");
        assert_eq!(rows(&path), rows_before, "legacy {legacy}");
    }
}

/// Python's 5 s busy timeout would surface as an OperationalError that
/// stops its connector; `--check` holds its read lock far shorter. Against
/// this tight-loop writer a slow runner may see the journal `busy`, which
/// `--check` reports truthfully; the contract is that the writer never fails.
#[test]
fn check_never_makes_a_writing_python_journal_fail() {
    let Some(interpreter) = required("ZERUS_CONNECTOR_TEST_PYTHON") else {
        return;
    };
    let scratch = scratch("handoff-writer");
    let state = scratch.path("state");
    let stop = scratch.path("stop");
    let (mut child, mut stdout) = start_python(
        &interpreter,
        &[
            "write",
            &state.display().to_string(),
            &binding(),
            &stop.display().to_string(),
        ],
    );
    // Stop the writer before asserting, so a failed check never removes the
    // scratch directory under a running writer.
    let outputs: Vec<_> = (0..25).map(|_| run_check(&scratch, &state)).collect();
    std::fs::write(&stop, "").unwrap();
    assert!(
        child.wait().unwrap().success(),
        "the writing Python journal failed"
    );
    for output in &outputs {
        let report: Value = serde_json::from_slice(&output.stdout).unwrap();
        assert_eq!(report["state"]["lock"], "held", "{report}");
        // A busy journal is the truthful answer and fails the check.
        let expected = match report["state"]["journal"]["status"].as_str() {
            Some("readable") => Some(0),
            Some("busy") => Some(1),
            _ => panic!("{report}"),
        };
        assert_eq!(output.status.code(), expected, "{report}");
    }
    let mut written = String::new();
    stdout.read_line(&mut written).unwrap();
    assert!(written.trim().parse::<u64>().unwrap() > 0);
}

#[test]
fn hgs_status_reports_running_while_rust_holds_the_lock() {
    let Some(hgs) = required("ZERUS_CONNECTOR_TEST_HGS") else {
        return;
    };
    let scratch = scratch("handoff-status");
    let (home, config, hgs_state, bin) = (
        scratch.path("home"),
        scratch.path("config"),
        scratch.path("hgs-state"),
        scratch.path("bin"),
    );
    for directory in [&home, &config.join("mobile"), &hgs_state, &bin] {
        std::fs::create_dir_all(directory).unwrap();
    }
    // No tmux server and no desktop: any other tmux command would be an agent launch.
    for name in ["tmux", "pgrep"] {
        let mut script = std::fs::File::create(bin.join(name)).unwrap();
        script.write_all(b"#!/bin/sh\nexit 1\n").unwrap();
        std::fs::set_permissions(bin.join(name), std::fs::Permissions::from_mode(0o755)).unwrap();
    }
    let state = scratch.path("connector-state");
    std::fs::write(
        config.join("mobile/connector.json"),
        json!({"state_dir": state.display().to_string()}).to_string(),
    )
    .unwrap();
    let status = || -> Value {
        let output = captured(
            Command::new(&hgs)
                .args(["status", "--local", "--json"])
                .env_clear()
                .env("HOME", &home)
                .env("PATH", format!("{}:/usr/bin:/bin", bin.display()))
                .env("HGS_CONFIG_DIR", &config)
                .env("HGS_STATE_DIR", &hgs_state)
                .env("HGS_SELF", "local")
                .env("HGS_PEERS", "")
                .env("HGS_TAB", "0"),
        );
        assert!(
            output.status.success(),
            "{}",
            String::from_utf8_lossy(&output.stderr)
        );
        serde_json::from_slice::<Value>(&output.stdout).unwrap()["services"]["mobile_connector"]
            .clone()
    };
    let journal = Journal::open(&state, &binding(), zerus_mobile_connector::now()).unwrap();
    assert_eq!(status(), "running");
    close(journal);
    assert_eq!(status(), "stopped");
}
