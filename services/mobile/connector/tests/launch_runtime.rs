#![cfg(unix)]
//! Launch, folder, worktree, archive and account behaviour that needs an
//! injected clock or a stop in the middle of a native call: the 5 s launch
//! verification and its project assignment, the 45 s archive snapshot rule,
//! a stop during a mutation (divergence h), the account cache's 30 s
//! catalog and 300 s usage clocks with its projections and bounds, the
//! reaping of an abandoned usage refresh.
mod common;

use common::*;
use serde_json::{json, Value};
use std::cell::RefCell;
use std::collections::HashMap;
use std::rc::Rc;
use std::time::Duration;
use tokio::sync::Notify;
use zerus_mobile_connector::accounts::{self, AccountCache};
use zerus_mobile_connector::capabilities::Flags;
use zerus_mobile_connector::execute::Outcome;
use zerus_mobile_connector::journal::{Journal, RESTART_ERROR};
use zerus_mobile_connector::launch;
use zerus_mobile_connector::native::{self, Call, Native, NativeError, NativeResult, Output};
use zerus_mobile_connector::runtime::{Connector, Settings};

const BINDING: &str = "fixture-binding";
const SESSION: &str = "codex/example/session";
const RUN: &str = "fixture-run";
const CONVERSATION: &str = "fixture-conversation";
const ARCHIVE: &str = "81b191c0-bdae-543a-ba1c-0fbcbb2f638d";
const DIRECTORY: &str = "/example/mobile";
const HELP: &str = "session-action <session> --json\n--launch-id\nswarm assign-launch --json\n\
                    Mobile worktree ABI: worktrees-v1\ndirs --create <path>\n";

/// Native answers by argv (the last one repeats), every call recorded, and
/// one argv that can be held until the test releases it.
#[derive(Default)]
struct Scripted {
    answers: RefCell<HashMap<Vec<String>, Vec<Value>>>,
    calls: RefCell<Vec<(Vec<String>, Option<Value>)>>,
    held: RefCell<Option<Vec<String>>>,
    entered: Notify,
    release: Notify,
}

fn argv(words: &[&str]) -> Vec<String> {
    words.iter().map(|word| (*word).to_owned()).collect()
}

impl Scripted {
    fn answer(&self, words: &[&str], answer: Value) {
        self.answers
            .borrow_mut()
            .entry(argv(words))
            .or_default()
            .push(answer);
    }
    fn hold(&self, words: &[&str]) {
        *self.held.borrow_mut() = Some(argv(words));
    }
    fn count(&self, prefix: &[&str]) -> usize {
        self.calls
            .borrow()
            .iter()
            .filter(|(call, _)| call.starts_with(&argv(prefix)))
            .count()
    }
    fn argvs(&self) -> Vec<Vec<String>> {
        self.calls
            .borrow()
            .iter()
            .map(|(call, _)| call.clone())
            .collect()
    }
}

impl Native for Scripted {
    async fn call(&self, call: Call) -> NativeResult {
        self.calls
            .borrow_mut()
            .push((call.argv.clone(), call.payload.clone()));
        if self.held.borrow().as_ref() == Some(&call.argv) {
            self.entered.notify_one();
            self.release.notified().await;
        }
        let answer = {
            let mut answers = self.answers.borrow_mut();
            match answers.get_mut(&call.argv) {
                Some(queue) if queue.len() > 1 => Some(queue.remove(0)),
                Some(queue) => queue.first().cloned(),
                None => None,
            }
        };
        match answer {
            None => Err(NativeError::Failed(native::FAILED)),
            Some(answer) if answer == json!({"exit": 1}) => {
                Err(NativeError::Failed(native::FAILED))
            }
            Some(Value::String(text)) if !call.json_output => Ok(Output::Text(text)),
            Some(answer) => Ok(Output::Json(answer)),
        }
    }
}

struct Harness {
    connector: Rc<Connector<Scripted>>,
    _scratch: Scratch,
}

fn harness(label: &str) -> Harness {
    umask();
    let scratch = Scratch::new(label);
    let journal = Journal::open(&scratch.path("state"), BINDING, EPOCH).unwrap();
    let settings = Settings {
        interval: 5.0,
        binding: BINDING.into(),
        default_timeout: native::DEFAULT_TIMEOUT,
        clock_scale: 1.0,
    };
    let connector = Connector::new(Scripted::default(), journal, settings, Box::new(|| EPOCH));
    connector.flags.set(Flags::from_help(HELP));
    {
        let mut state = connector.state.borrow_mut();
        state.sessions = [SESSION.to_owned()].into();
        state.archives = [(SESSION.to_owned(), ARCHIVE.to_owned())].into();
        state.snapshot_seen_at = Some(connector.clock());
    }
    Harness {
        connector: Rc::new(connector),
        _scratch: scratch,
    }
}

fn request(id: &str, operation: &str, session: &str, payload: Value) -> Value {
    json!({"request_id": id, "operation": operation, "session": session, "payload": payload})
}

fn finished(outcome: Outcome) -> Value {
    match outcome {
        Outcome::Finished(response) => response,
        other => panic!("{other:?}"),
    }
}

fn failed(state: &str, error: &str) -> Value {
    json!({"state": state, "result": null, "error": error})
}

fn restore(id: &str) -> Value {
    request(
        id,
        "restore",
        SESSION,
        json!({"request_id": id, "expected_run_id": RUN,
               "expected_conversation_id": CONVERSATION, "archive_id": ARCHIVE}),
    )
}

#[tokio::test(flavor = "current_thread", start_paused = true)]
async fn an_archive_is_named_only_within_45_s_of_its_snapshot() {
    let harness = harness("archive-clock");
    let connector = &harness.connector;
    let first = "0f4d1e8e-1d8b-4f73-9a3b-2a3c5d6e7f80";
    connector.native.answer(
        &["session-action", SESSION, "--json"],
        json!({"request_id": first, "name": SESSION, "run_id": RUN,
               "conversation_id": CONVERSATION, "status": "completed"}),
    );
    tokio::time::advance(Duration::from_secs(45)).await;
    let response = finished(connector.execute(restore(first)).await.unwrap());
    assert_eq!(response["state"], "completed", "{response}");
    assert_eq!(response["result"]["archive_id"], ARCHIVE);
    tokio::time::advance(Duration::from_secs(1)).await;
    let late = "1f4d1e8e-1d8b-4f73-9a3b-2a3c5d6e7f80";
    assert_eq!(
        finished(connector.execute(restore(late)).await.unwrap()),
        failed(
            "failed",
            "archive must match a fresh exact local snapshot entry"
        )
    );
    let inspect = "2f4d1e8e-1d8b-4f73-9a3b-2a3c5d6e7f80";
    let archived = request(inspect, "inspect", SESSION, json!({"archive_id": ARCHIVE}));
    assert_eq!(
        finished(connector.execute(archived).await.unwrap()),
        failed(
            "failed",
            "archive snapshot is stale; refresh before inspection"
        )
    );
    // Only the restore inside the window reached native hgs.
    assert_eq!(connector.native.argvs().len(), 1);
}

fn accounts_catalog() -> Value {
    json!({"profiles": [{"id": "native-codex", "provider": "codex", "label": "Codex",
                         "installed": true}]})
}

fn script_launch(connector: &Connector<Scripted>, id: &str, tag: &str) -> Vec<String> {
    let native = &connector.native;
    native.answer(&["account", "ls"], accounts_catalog());
    native.answer(
        &["dirs", DIRECTORY],
        json!({"path": DIRECTORY, "directories": []}),
    );
    let launch = [
        "codex",
        DIRECTORY,
        "--new",
        "-n",
        tag,
        "-d",
        "--launch-id",
        id,
    ];
    native.answer(&launch, json!("Created\n"));
    argv(&launch)
}

fn listed(id: &str, conversation: Value) -> Value {
    json!({"sessions": [{"name": "codex/example/new", "run_id": "new-run", "launch_id": id,
                         "conversation_id": conversation}]})
}

#[tokio::test(flavor = "current_thread", start_paused = true)]
async fn a_launch_is_confirmed_only_by_its_listing_within_five_seconds() {
    let harness = harness("launch-verify");
    let connector = &harness.connector;
    let id = "3f4d1e8e-1d8b-4f73-9a3b-2a3c5d6e7f80";
    let launch = script_launch(connector, id, "new");
    let listing = ["ls", "--json", "--local"];
    for _ in 0..2 {
        connector.native.answer(&listing, json!({"sessions": []}));
    }
    connector
        .native
        .answer(&listing, listed(id, json!("new-conversation")));
    let payload = json!({"request_id": id, "agent": "codex", "directory": DIRECTORY, "tag": "new"});
    let started = connector.clock();
    let response = finished(
        connector
            .execute(request(id, "launch", "", payload))
            .await
            .unwrap(),
    );
    assert_eq!(
        response,
        json!({"state": "completed", "error": null, "result": {"request_id": id, "status": "created",
               "result_target": {"name": "codex/example/new", "run_id": "new-run",
                                 "conversation_id": "new-conversation"}}})
    );
    assert_eq!(connector.native.count(&listing), 3);
    assert!((connector.clock() - started - 0.2).abs() < 1e-6);
    assert_eq!(
        connector
            .native
            .count(&launch.iter().map(String::as_str).collect::<Vec<_>>()),
        1
    );

    // Never listed: about five seconds of listings, then uncertain.
    let unlisted = "4f4d1e8e-1d8b-4f73-9a3b-2a3c5d6e7f80";
    let launch = script_launch(connector, unlisted, "unlisted");
    let before = connector.native.count(&listing);
    let payload = json!({"request_id": unlisted, "agent": "codex", "directory": DIRECTORY, "tag": "unlisted"});
    let started = connector.clock();
    let response = finished(
        connector
            .execute(request(unlisted, "launch", "", payload))
            .await
            .unwrap(),
    );
    assert_eq!(response, failed("uncertain", launch::UNVERIFIED));
    assert_eq!(connector.native.count(&listing) - before, 50);
    assert!((connector.clock() - started - launch::VERIFY_SECONDS).abs() < 1e-6);
    let launches = connector
        .native
        .argvs()
        .into_iter()
        .filter(|call| *call == launch)
        .count();
    assert_eq!(launches, 1);
}

fn native_projects() -> Value {
    json!({"schema": 1, "initialized": true, "node_id": "native-local", "swarm_id": "native-shared",
           "machines": [], "organization": {"default_project": "general", "projects": [
               {"id": "logical", "name": "Logical", "folders": [], "sessions": []},
               {"id": "general", "name": "General", "folders": [], "sessions": []}]}})
}

#[tokio::test(flavor = "current_thread", start_paused = true)]
async fn an_assignment_completes_a_launch_and_refreshes_the_projects() {
    let harness = harness("launch-assign");
    let connector = &harness.connector;
    let id = "5f4d1e8e-1d8b-4f73-9a3b-2a3c5d6e7f80";
    script_launch(connector, id, "project");
    connector
        .native
        .answer(&["swarm", "get"], native_projects());
    // The listing has no conversation yet; the assignment supplies it.
    connector
        .native
        .answer(&["ls", "--json", "--local"], listed(id, Value::Null));
    connector.native.answer(
        &["swarm", "assign-launch", "--json"],
        json!({"request_id": id, "name": "codex/example/new", "run_id": "new-run",
               "conversation_id": "assigned-conversation", "swarm_id": "native-shared",
               "project_id": "logical", "status": "assigned"}),
    );
    connector.state.borrow_mut().project_next_poll = 1000.0;
    let payload = json!({"request_id": id, "agent": "codex", "directory": DIRECTORY, "tag": "project",
                         "swarm_id": "native-shared", "project_id": "logical", "add_folder": true});
    let response = finished(
        connector
            .execute(request(id, "launch", "", payload))
            .await
            .unwrap(),
    );
    let result = &response["result"];
    assert_eq!(response["state"], "completed", "{response}");
    assert_eq!(
        result["project_assignment"],
        json!({"status": "assigned", "swarm_id": "native-shared", "project_id": "logical"})
    );
    assert_eq!(
        result["result_target"]["conversation_id"],
        "assigned-conversation"
    );
    assert!(result.get("warning").is_none());
    assert_eq!(connector.state.borrow().project_next_poll, 0.0);
    let stdin = connector
        .native
        .calls
        .borrow()
        .iter()
        .find(|(call, _)| call[..2] == ["swarm", "assign-launch"])
        .and_then(|(_, payload)| payload.clone())
        .unwrap();
    assert_eq!(
        stdin,
        json!({"request_id": id, "name": "codex/example/new", "expected_run_id": "new-run",
               "expected_conversation_id": "", "swarm_id": "native-shared", "project_id": "logical",
               "directory": DIRECTORY, "add_folder": true})
    );
}

/// Runs `request` while `held` is in flight, and sets the connector
/// stopping during that native call.
async fn stop_during(connector: &Rc<Connector<Scripted>>, held: &[&str], request: Value) -> Value {
    connector.native.hold(held);
    let stop = async {
        connector.native.entered.notified().await;
        connector.stopping.set();
        connector.native.release.notify_one();
    };
    let (outcome, ()) = futures_util::future::join(connector.execute(request), stop).await;
    finished(outcome.unwrap())
}

#[tokio::test(flavor = "current_thread")]
async fn a_stop_during_a_folder_creation_journals_its_real_outcome_once() {
    let harness = harness("directory-stop");
    let connector = &harness.connector;
    let id = "6f4d1e8e-1d8b-4f73-9a3b-2a3c5d6e7f80";
    let path = "/alias/new";
    let create = ["dirs", "--create", path, "--request-id", id];
    connector.native.answer(
        &["dirs", "/alias"],
        json!({"path": "/canonical", "directories": []}),
    );
    connector.native.answer(
        &create,
        json!({"status": "created", "request_id": id, "requested_path": path,
               "path": "/canonical/new", "parent": "/canonical"}),
    );
    let submission = request(
        id,
        "directory_create",
        "",
        json!({"request_id": id, "path": path}),
    );
    let response = stop_during(connector, &create, submission.clone()).await;
    let receipt = json!({"status": "created", "request_id": id, "requested_path": path,
                         "path": "/canonical/new", "parent": "/canonical"});
    assert_eq!(
        response,
        json!({"state": "completed", "error": null, "result": receipt})
    );
    // Requests run one at a time: a redelivery replays the one admission,
    // and a changed request under the same identity never runs.
    assert_eq!(
        connector.execute(submission.clone()).await.unwrap(),
        Outcome::Replayed
    );
    let mut changed = submission;
    changed["payload"]["path"] = json!("/alias/other");
    assert_eq!(
        connector.execute(changed).await.unwrap(),
        Outcome::Rejected("request_id conflicts with preserved native request identity".into())
    );
    assert_eq!(connector.native.count(&["dirs", "--create"]), 1);
    assert_eq!(connector.native.argvs().len(), 2);
}

#[tokio::test(flavor = "current_thread")]
async fn a_stop_during_a_worktree_creation_waits_for_its_receipt() {
    let harness = harness("worktree-stop");
    let connector = &harness.connector;
    let id = "7f4d1e8e-1d8b-4f73-9a3b-2a3c5d6e7f80";
    let native = &connector.native;
    native.answer(&["dirs", "/alias"], json!({"path": "/canonical"}));
    native.answer(
        &[
            "__state",
            "worktrees",
            "--path",
            "/r",
            "--refresh",
            "--json",
        ],
        json!({"state": "ok", "path": "/r", "common_dir": "/r/.git", "worktrees": []}),
    );
    let create = [
        "__state",
        "worktrees",
        "create",
        "--path",
        "/r",
        "--common-dir",
        "/r/.git",
        "--branch",
        "feature",
        "--base",
        "HEAD",
        "--destination",
        "/alias/new",
        "--request-id",
        id,
        "--json",
        "--mobile",
    ];
    native.answer(
        &create,
        json!({"request_id": id, "status": "created", "path": "/canonical/new",
               "common_dir": "/r/.git", "branch": "feature"}),
    );
    let payload = json!({"request_id": id, "path": "/r", "common_dir": "/r/.git",
                         "destination": "/alias/new", "branch": "feature", "base": "HEAD"});
    let response = stop_during(
        connector,
        &create,
        request(id, "worktree_create", "", payload),
    )
    .await;
    assert_eq!(response["state"], "completed", "{response}");
    assert_eq!(response["result"]["path"], "/canonical/new");
    assert_eq!(native.count(&["__state", "worktrees", "create"]), 1);
}

#[tokio::test(flavor = "current_thread")]
async fn a_stop_during_a_launch_leaves_it_uncertain_and_never_repeats_it() {
    let harness = harness("launch-stop");
    let connector = &harness.connector;
    let id = "8f4d1e8e-1d8b-4f73-9a3b-2a3c5d6e7f80";
    let launch = script_launch(connector, id, "stopped");
    connector
        .native
        .answer(&["ls", "--json", "--local"], listed(id, json!("c")));
    let payload =
        json!({"request_id": id, "agent": "codex", "directory": DIRECTORY, "tag": "stopped"});
    let held: Vec<&str> = launch.iter().map(String::as_str).collect();
    let submission = request(id, "launch", "", payload);
    let response = stop_during(connector, &held, submission.clone()).await;
    // The launch ran; no verification starts once the connector stops.
    assert_eq!(response, failed("uncertain", RESTART_ERROR));
    assert_eq!(connector.native.count(&["ls"]), 0);
    assert_eq!(
        connector.execute(submission).await.unwrap(),
        Outcome::Replayed
    );
    assert_eq!(connector.native.count(&held), 1);
}

// The account cache (`accounts.AccountCache`), driven as the heartbeat
// drives it: the catalog when due (every 30 s), then at most one usage
// refresh at a time, each account at most every 300 s.

fn profile(identity: &str, revision: &str) -> Value {
    json!({"id": identity, "provider": "codex", "label": "Synthetic account", "installed": true,
           "native": true, "is_default": true, "auth_revision": revision, "home": "/private/account",
           "account_status": {"status": "ok", "checked_at": 800,
                              "identity": {"email": "user@example.test"}}})
}

fn inspection(identity: &str) -> Value {
    json!({"id": identity, "provider": "codex", "home": "/private/account", "host": "private-host",
           "access_token": "private-token", "error": "private-error", "source": "Codex App Server",
           "status": "ok", "checked_at": 1000,
           "identity": {"email": "user@example.test", "plan": "Pro", "secret": "private-secret"},
           "windows": [{"id": "primary", "label": "5 hours", "used_percent": 42.5,
                        "window_minutes": 300, "resets_at": 1500,
                        "url": "https://private.example.test"}],
           "credits": {"balance": "3.25", "unlimited": false, "secret": "private-credit"},
           "balances": [{"kind": "Wallet", "balance": "8.00", "currency": "USD",
                         "token": "private-wallet"}]})
}

/// One heartbeat's `update()` at clock `now` (wall `1000 + now`): the
/// catalog when due, then the refresh it starts, if any.
fn update(cache: &mut AccountCache, now: f64, raw: &Value) -> Option<accounts::Refresh> {
    if cache.catalog_due(now) {
        cache.next_catalog = now + accounts::CATALOG_SECONDS;
        cache.apply_catalog(accounts::catalog(raw), 1000.0 + now);
    }
    cache.next_refresh(now)
}

fn usage(view: &Value, index: usize) -> &Value {
    &view["accounts"][index]["usage"]
}

#[test]
fn usage_refresh_is_single_flight_and_keeps_its_native_age() {
    let raw = json!({"profiles": [profile("native-codex", "auth-one")]});
    let mut cache = AccountCache::default();
    let refresh = update(&mut cache, 0.0, &raw).unwrap();
    assert_eq!(refresh.identity, "native-codex");
    let view = cache.view(1000.0);
    assert_eq!(view["checked_at"], 1000.0);
    assert_eq!(usage(&view, 0)["checked_at"], 800);
    for now in 1..10 {
        assert_eq!(update(&mut cache, f64::from(now), &raw), None);
        assert_eq!(usage(&cache.view(1000.0), 0)["refreshing"], true);
    }
    // A catalog read while the refresh runs keeps the refresh's account.
    assert_eq!(update(&mut cache, 31.0, &raw), None);
    cache.finish_refresh(&refresh, Some(&inspection("native-codex")));
    let view = cache.view(1031.0);
    assert_eq!(usage(&view, 0)["checked_at"], 1000);
    assert_eq!(usage(&view, 0)["stale"], false);
    assert_eq!(usage(&cache.view(1301.0), 0)["stale"], true);
    let text = view.to_string();
    for private in ["private", "/private/account", "auth-one"] {
        assert!(!text.contains(private), "{private}");
    }
}

#[test]
fn changed_or_removed_credentials_discard_usage_and_late_results() {
    let raw = json!({"profiles": [profile("native-codex", "auth-one")]});
    let mut cache = AccountCache::default();
    let refresh = update(&mut cache, 0.0, &raw).unwrap();
    let mut changed = profile("native-codex", "changed-auth");
    changed["account_status"] = Value::Null;
    let raw = json!({"profiles": [changed]});
    assert!(update(&mut cache, 31.0, &raw).is_none());
    let view = cache.view(1031.0);
    assert_eq!(usage(&view, 0)["identity"], json!({}));
    assert!(usage(&view, 0).get("checked_at").is_none());
    // The refresh of the replaced credentials ends late and is ignored.
    cache.finish_refresh(&refresh, Some(&inspection("native-codex")));
    assert_eq!(usage(&cache.view(1031.0), 0)["identity"], json!({}));
    assert_eq!(update(&mut cache, 62.0, &json!({"profiles": []})), None);
    assert_eq!(cache.view(1062.0)["accounts"], json!([]));
}

#[test]
fn refreshes_rotate_fairly_every_300_s_over_installed_accounts() {
    let mut idle = profile("not-installed", "r");
    idle["installed"] = json!(false);
    let raw = json!({"profiles": [profile("one", "r"), profile("two", "r"), idle]});
    let mut cache = AccountCache::default();
    let mut refreshed = Vec::new();
    for now in [0.0, 1.0, 2.0, 299.0, 300.0, 301.0] {
        if let Some(refresh) = update(&mut cache, now, &raw) {
            refreshed.push(refresh.identity.clone());
            cache.finish_refresh(&refresh, Some(&inspection(&refresh.identity)));
        }
    }
    assert_eq!(refreshed, ["one", "two", "one", "two"]);
    assert_eq!(accounts::INSPECT_TIMEOUT, 25.0);
    assert_eq!(accounts::CATALOG_SECONDS, 30.0);
    assert_eq!(accounts::REFRESH_SECONDS, 300.0);
}

#[test]
fn a_dsh_sign_out_without_an_auth_revision_invalidates_usage() {
    let mut row = profile("native-dsh", "unused");
    row["provider"] = json!("dsh");
    row.as_object_mut().unwrap().remove("auth_revision");
    let raw = json!({"profiles": [row.clone()]});
    let mut cache = AccountCache::default();
    let refresh = update(&mut cache, 0.0, &raw).unwrap();
    let mut inspected = inspection("native-dsh");
    inspected["provider"] = json!("dsh");
    cache.finish_refresh(&refresh, Some(&inspected));
    assert_eq!(
        usage(&cache.view(1000.0), 0)["windows"]
            .as_array()
            .unwrap()
            .len(),
        1
    );
    row["account_status"] = Value::Null;
    update(&mut cache, 31.0, &json!({"profiles": [row]}));
    let view = cache.view(1031.0);
    assert_eq!(usage(&view, 0)["identity"], json!({}));
    assert_eq!(usage(&view, 0)["windows"], json!([]));
    assert!(usage(&view, 0).get("checked_at").is_none());
}

#[test]
fn failures_keep_stale_values_without_native_text() {
    let raw = json!({"profiles": [profile("native-codex", "auth-one")]});
    let mut cache = AccountCache::default();
    let refresh = update(&mut cache, 0.0, &raw).unwrap();
    cache.finish_refresh(&refresh, Some(&inspection("native-codex")));
    // The catalog read fails: Python's errors never reach the view.
    cache.next_catalog = 0.0;
    cache.apply_catalog(
        Err(zerus_mobile_connector::pyvalue::PyError::value(
            "private-secret-and-path",
        )),
        1031.0,
    );
    let view = cache.view(1031.0);
    assert_eq!(view["stale"], true);
    assert_eq!(usage(&view, 0)["checked_at"], 1000);
    assert!(!view.to_string().contains("private"));
    // No refresh starts while the catalog is stale; once it is current
    // again, a failed refresh is marked and keeps the last values.
    assert_eq!(cache.next_refresh(301.0), None);
    cache.stale = false;
    let refresh = cache.next_refresh(301.0).unwrap();
    cache.finish_refresh(&refresh, None);
    let view = cache.view(1301.0);
    assert_eq!(usage(&view, 0)["refresh_error"], true);
    assert_eq!(usage(&view, 0)["checked_at"], 1000);
    let mut unavailable = AccountCache::default();
    unavailable.apply_catalog(Err(zerus_mobile_connector::pyvalue::PyError::Type), 1000.0);
    assert_eq!(
        unavailable.view(1000.0),
        json!({"schema": 1, "available": false, "stale": true,
               "refresh_interval_seconds": 300, "accounts": []})
    );
}

#[test]
fn account_projections_are_bounded_and_never_coerced() {
    let duplicate = json!({"profiles": [profile("a", "r"), profile("a", "r")]});
    let many: Vec<Value> = (0..101)
        .map(|index| profile(&format!("a{index}"), "r"))
        .collect();
    for raw in [
        json!({}),
        json!({"profiles": many}),
        duplicate,
        json!({"profiles": [profile("--refresh", "r")]}),
        json!({"profiles": [profile("../../private", "r")]}),
    ] {
        assert!(accounts::catalog(&raw).is_err(), "{raw}");
    }
    let mut wrong = profile("a", "r");
    wrong["provider"] = json!([]);
    assert!(accounts::catalog(&json!({"profiles": [wrong]})).is_err());
    let windows: Vec<Value> = (0..1000)
        .map(|_| inspection("a")["windows"][0].clone())
        .collect();
    let projected =
        accounts::usage(&json!({"identity": {"name": "a".repeat(5000)}, "windows": windows}));
    assert_eq!(projected["identity"]["name"].as_str().unwrap().len(), 256);
    assert_eq!(projected["windows"].as_array().unwrap().len(), 16);
    // Huge, infinite, boolean and text numbers and hostile types are dropped.
    let huge = zerus_relay::json::parse(
        format!(
            "{{\"status\": [], \"auth_status\": {{}}, \"source\": [], \"checked_at\": 1{zeros}, \
              \"identity\": [], \"windows\": [{{\"used_percent\": 1{zeros}}}, \
              {{\"used_percent\": 1, \"resets_at\": \"2026-99-99T12:34:56Z\"}}, \
              {{\"used_percent\": true}}, {{\"used_percent\": -1}}, {{\"used_percent\": 1e400}}, \
              {{\"used_percent\": \"42\"}}, \
              {{\"used_percent\": 24, \"resets_at\": 1e400, \"window_minutes\": \"300\"}}], \
              \"balances\": [{{\"currency\": [], \"balance\": \"1\"}}]}}",
            zeros = "0".repeat(1000)
        )
        .as_bytes(),
        16,
        1000,
    )
    .unwrap();
    let projected = accounts::usage(&huge);
    assert_eq!(projected["status"], "unavailable");
    assert!(projected.get("checked_at").is_none());
    assert!(projected.get("source").is_none());
    assert!(projected.get("auth_status").is_none());
    assert!(projected.get("balances").is_none());
    assert_eq!(
        projected["windows"],
        json!([{"used_percent": 1}, {"used_percent": 24}])
    );
    // The projection is bounded by its ASCII-escaped bytes.
    let wide: Vec<Value> = (0..100)
        .map(|index| {
            let mut row = profile(&format!("account{index}"), "r");
            let window = json!({"id": "\u{754c}".repeat(160), "label": "\u{754c}".repeat(160),
                                "used_percent": 42});
            row["account_status"] = json!({"status": "ok",
                                           "identity": {"name": "\u{754c}".repeat(256)},
                                           "windows": vec![window; 16]});
            row
        })
        .collect();
    assert_eq!(
        accounts::catalog(&json!({"profiles": wide})).err(),
        Some(zerus_mobile_connector::pyvalue::PyError::value(
            "native account projection exceeds size limit"
        ))
    );
}

#[test]
fn launch_catalogs_keep_exact_public_profiles_within_their_bound() {
    let rows = json!({"profiles": [
        {"id": "work-one", "provider": "claude", "label": "Same", "installed": true, "is_default": true,
         "account_status": {"auth_status": "signed_in",
                            "identity": {"email": "account@example.invalid", "token": "private"}},
         "home": "/private"},
        {"id": "work-two", "provider": "claude", "label": "Same", "installed": true,
         "account_status": {"status": "expired"}}]});
    let result = accounts::launch_catalog(&rows).unwrap();
    let ids: Vec<&str> = result["accounts"]
        .as_array()
        .unwrap()
        .iter()
        .map(|row| row["id"].as_str().unwrap())
        .collect();
    assert_eq!(ids, ["work-one", "work-two"]);
    assert_eq!(result["accounts"][0]["is_default"], true);
    assert_eq!(result["accounts"][0]["auth_status"], "signed_in");
    assert_eq!(result["accounts"][1]["auth_status"], "expired");
    assert_eq!(
        result["accounts"][0]["usage"]["identity"],
        json!({"email": "account@example.invalid"})
    );
    assert!(!result.to_string().contains("private"));
    let models: Vec<Value> = (0..256)
        .map(|index| json!({"value": format!("local/model{index}"), "label": "\u{754c}".repeat(160)}))
        .collect();
    let profiles: Vec<Value> = (0..2)
        .map(|index| {
            json!({"id": format!("native-opencode-{index}"), "provider": "opencode",
                   "label": "Native OpenCode", "installed": true, "model_options": models})
        })
        .collect();
    assert_eq!(
        accounts::launch_catalog(&json!({"profiles": profiles})).err(),
        Some(zerus_mobile_connector::pyvalue::PyError::value(
            "native launch account projection exceeds size limit"
        ))
    );
    // Project fields are bounded before any native call.
    let base = json!({"request_id": "r", "agent": "codex", "directory": "/example/mobile",
                      "tag": "mobile launch"});
    assert!(launch::validate("launch", Some(&base), "r").is_ok());
    for fields in [
        json!({"project_id": "logical"}),
        json!({"swarm_id": "swarm", "project_id": "x".repeat(129), "add_folder": true}),
        json!({"swarm_id": "swarm", "project_id": "logical", "add_folder": "true"}),
        json!({"swarm_id": "swarm", "project_id": "\u{754c}".repeat(43), "add_folder": true}),
        json!({"swarm_id": "swarm", "project_id": "logical", "project_folder_id": "f",
               "add_folder": true}),
    ] {
        let mut payload = base.clone();
        for (key, value) in fields.as_object().unwrap() {
            payload[key] = value.clone();
        }
        assert!(
            launch::validate("launch", Some(&payload), "r").is_err(),
            "{fields}"
        );
    }
}

/// A usage refresh still running when the connector stops is dropped with
/// the runtime, which kills its own `account inspect` child and nothing else.
#[cfg(target_os = "linux")]
#[tokio::test(flavor = "current_thread")]
async fn an_abandoned_usage_refresh_reaps_only_its_own_child() {
    use std::os::unix::fs::PermissionsExt;
    let scratch = Scratch::new("refresh-reap");
    let script = scratch.path("fixture-hgs");
    let marker = scratch.path("owned-pid");
    std::fs::write(
        &script,
        format!(
            "#!/bin/sh\necho $$ > '{}'\nexec sleep 60\n",
            marker.display()
        ),
    )
    .unwrap();
    std::fs::set_permissions(&script, std::fs::Permissions::from_mode(0o700)).unwrap();
    let process = native::Process::new(script.display().to_string(), native::DEFAULT_TIMEOUT);
    let call = Call::new(["account", "inspect", "native-codex"]).timeout(accounts::INSPECT_TIMEOUT);
    let written = async {
        while std::fs::read_to_string(&marker).map_or(true, |text| text.trim().is_empty()) {
            tokio::time::sleep(Duration::from_millis(10)).await;
        }
    };
    {
        let refresh = std::pin::pin!(process.call(call));
        let written = std::pin::pin!(written);
        let raced = tokio::time::timeout(
            Duration::from_secs(10),
            futures_util::future::select(refresh, written),
        )
        .await;
        assert!(matches!(raced, Ok(futures_util::future::Either::Right(_))));
    }
    let pid = std::fs::read_to_string(&marker).unwrap().trim().to_owned();
    let gone = || {
        std::fs::read_to_string(format!("/proc/{pid}/stat")).map_or(true, |stat| {
            stat.rsplit(')')
                .next()
                .is_some_and(|rest| rest.trim_start().starts_with('Z'))
        })
    };
    for _ in 0..200 {
        if gone() {
            return;
        }
        tokio::time::sleep(Duration::from_millis(10)).await;
    }
    panic!("the abandoned account inspection {pid} is still running");
}

fn opencode_catalog() -> Value {
    json!({"profiles": [{"id": "native-opencode", "provider": "opencode", "label": "OpenCode",
        "installed": true, "catalog_state": "ready", "catalog_reason": "", "catalog_at": 5,
        "model_options": [
            {"id": "fixture/local", "value": "fixture/local", "label": "Local", "provider": "fixture",
             "provider_label": "Fixture", "free": true,
             "effort_options": ["low", "high"], "effort_labels": ["Low", "High"]},
            {"id": "fixture/plain", "value": "fixture/plain", "label": "Plain", "provider": "fixture"}]}]})
}

#[tokio::test(flavor = "current_thread", start_paused = true)]
async fn an_opencode_launch_passes_only_an_offered_effort() {
    let harness = harness("launch-effort");
    let connector = &harness.connector;
    let native = &connector.native;
    native.answer(&["account", "ls"], opencode_catalog());
    native.answer(
        &["dirs", DIRECTORY],
        json!({"path": DIRECTORY, "directories": []}),
    );
    let id = "5f4d1e8e-1d8b-4f73-9a3b-2a3c5d6e7f80";
    let launch = [
        "opencode",
        DIRECTORY,
        "--new",
        "-n",
        "fx",
        "-d",
        "--launch-id",
        id,
        "--account",
        "native-opencode",
        "--model",
        "fixture/local",
        "--effort",
        "high",
    ];
    native.answer(&launch, json!("Created\n"));
    native.answer(
        &["ls", "--json", "--local"],
        json!({"sessions": [{"name": "opencode/example/fx", "run_id": "run", "launch_id": id,
                             "conversation_id": "conversation"}]}),
    );
    let payload = |id: &str, model: &str, effort: &str| {
        json!({"request_id": id, "agent": "opencode", "directory": DIRECTORY, "tag": "fx",
               "account_id": "native-opencode", "model": model, "effort": effort})
    };
    let sent = finished(
        connector
            .execute(request(
                id,
                "launch",
                "",
                payload(id, "fixture/local", "high"),
            ))
            .await
            .unwrap(),
    );
    assert_eq!(sent["state"], "completed", "{sent}");
    assert_eq!(native.count(&launch), 1);
    // Not offered for this model, or the model offers none: refused before any write.
    for (index, (model, effort)) in [("fixture/local", "ultra"), ("fixture/plain", "high")]
        .into_iter()
        .enumerate()
    {
        let other = format!("6f4d1e8e-1d8b-4f73-9a3b-2a3c5d6e7f8{index}");
        let refused = finished(
            connector
                .execute(request(
                    &other,
                    "launch",
                    "",
                    payload(&other, model, effort),
                ))
                .await
                .unwrap(),
        );
        assert_eq!(
            refused,
            failed("failed", "effort is not offered for this model")
        );
    }
    assert_eq!(native.count(&launch), 1);
    // Other agents keep today's refusal of the unknown field.
    let base = json!({"request_id": "r", "agent": "codex", "directory": "/example/mobile",
                      "tag": "t", "effort": "high"});
    assert!(launch::validate("launch", Some(&base), "r").is_err());
    let mut model_less = payload("r", "fixture/local", "high");
    model_less.as_object_mut().unwrap().remove("model");
    assert!(launch::validate("launch", Some(&model_less), "r").is_err());
    assert!(launch::validate("launch", Some(&payload("r", "fixture/local", "high")), "r").is_ok());
    assert!(launch::validate("launch", Some(&payload("r", "fixture/local", "a b")), "r").is_err());
}

/// The OpenCode effort shape contract every layer shares with the native
/// catalog projection; `effort_options` membership stays authoritative.
const EFFORT_IDS: &str = include_str!("../../../../tests/fixtures/opencode-effort-ids.json");

fn effort_ids(kind: &str) -> Vec<String> {
    let fixture: Value = serde_json::from_str(EFFORT_IDS).unwrap();
    let ids: Vec<String> = fixture[kind]
        .as_array()
        .unwrap()
        .iter()
        .map(|id| id.as_str().unwrap().to_owned())
        .collect();
    assert!(!ids.is_empty(), "{kind}");
    ids
}

/// One OpenCode account whose `fixture/local` model offers `efforts`.
fn opencode_offering(efforts: &[&str]) -> Value {
    json!({"profiles": [{"id": "native-opencode", "provider": "opencode", "label": "OpenCode",
        "installed": true, "catalog_state": "ready",
        "model_options": [{"id": "fixture/local", "value": "fixture/local", "label": "Local",
                           "provider": "fixture", "effort_options": efforts}]}]})
}

fn opencode_launch(id: &str, effort: &str) -> Value {
    json!({"request_id": id, "agent": "opencode", "directory": DIRECTORY, "tag": "fx",
           "account_id": "native-opencode", "model": "fixture/local", "effort": effort})
}

#[test]
fn the_shared_fixture_decides_which_projected_efforts_survive() {
    let projected = |raw: &Value| {
        let launch =
            accounts::launch_catalog(raw).unwrap()["accounts"][0]["model_options"][0].clone();
        let entries = accounts::catalog(raw).unwrap();
        assert_eq!(entries[0].1.public["model_options"][0], launch);
        launch
    };
    for id in effort_ids("accept") {
        let option = projected(&opencode_offering(&[&id]));
        assert_eq!(option["effort_options"], json!([id]), "{id:?}");
        assert_eq!(option["effort_labels"], json!([id]), "{id:?}");
    }
    for id in effort_ids("reject") {
        // A malformed variant drops only that entry, never the model.
        let option = projected(&opencode_offering(&[&id, "high"]));
        assert_eq!(option["value"], "fixture/local", "{id:?}");
        assert_eq!(option["effort_options"], json!(["high"]), "{id:?}");
        let option = projected(&opencode_offering(&[&id]));
        assert!(option.get("effort_options").is_none(), "{id:?}");
    }
}

#[test]
fn the_shared_fixture_decides_every_launch_effort_shape() {
    for id in effort_ids("accept") {
        let payload = opencode_launch("r", &id);
        assert!(
            launch::validate("launch", Some(&payload), "r").is_ok(),
            "{id:?}"
        );
        let mut model_less = payload;
        model_less.as_object_mut().unwrap().remove("model");
        assert!(
            launch::validate("launch", Some(&model_less), "r").is_err(),
            "{id:?}"
        );
    }
    for id in effort_ids("reject") {
        let payload = opencode_launch("r", &id);
        assert!(
            launch::validate("launch", Some(&payload), "r").is_err(),
            "{id:?}"
        );
    }
    assert!(launch::validate("launch", Some(&opencode_launch("r", "")), "r").is_err());
    let mut number = opencode_launch("r", "high");
    number["effort"] = json!(8);
    assert!(launch::validate("launch", Some(&number), "r").is_err());
}

#[tokio::test(flavor = "current_thread", start_paused = true)]
async fn only_a_currently_offered_effort_reaches_its_own_argv_element() {
    let harness = harness("launch-effort-ids");
    let connector = &harness.connector;
    let native = &connector.native;
    // The first projection offers the colon id; every later one has withdrawn it.
    native.answer(
        &["account", "ls"],
        opencode_offering(&["think:deep", "caf\u{e9}"]),
    );
    native.answer(&["account", "ls"], opencode_offering(&["caf\u{e9}"]));
    native.answer(
        &["dirs", DIRECTORY],
        json!({"path": DIRECTORY, "directories": []}),
    );
    let id = "7a4d1e8e-1d8b-4f73-9a3b-2a3c5d6e7f80";
    let launch = [
        "opencode",
        DIRECTORY,
        "--new",
        "-n",
        "fx",
        "-d",
        "--launch-id",
        id,
        "--account",
        "native-opencode",
        "--model",
        "fixture/local",
        "--effort",
        "think:deep",
    ];
    native.answer(&launch, json!("Created\n"));
    native.answer(
        &["ls", "--json", "--local"],
        json!({"sessions": [{"name": "opencode/example/fx", "run_id": "run", "launch_id": id,
                             "conversation_id": "conversation"}]}),
    );
    let sent = finished(
        connector
            .execute(request(id, "launch", "", opencode_launch(id, "think:deep")))
            .await
            .unwrap(),
    );
    assert_eq!(sent["state"], "completed", "{sent}");
    // The id is one argv element after `--effort`, never part of a payload.
    let written: Vec<_> = native
        .calls
        .borrow()
        .iter()
        .filter(|(call, _)| call.first().map(String::as_str) == Some("opencode"))
        .cloned()
        .collect();
    assert_eq!(written, [(argv(&launch), None)]);
    // Withdrawn from the model, or never offered by it although well formed:
    // refused from the fresh projection before any native write.
    for (index, effort) in ["think:deep", "low\u{2014}deep", "thinking-8k_v1.2"]
        .into_iter()
        .enumerate()
    {
        let other = format!("8a4d1e8e-1d8b-4f73-9a3b-2a3c5d6e7f8{index}");
        let before = native.argvs().len();
        let refused = finished(
            connector
                .execute(request(
                    &other,
                    "launch",
                    "",
                    opencode_launch(&other, effort),
                ))
                .await
                .unwrap(),
        );
        let receipt = failed(
            "failed",
            zerus_mobile_connector::mutations::EFFORT_NOT_OFFERED,
        );
        assert_eq!(refused, receipt, "{effort:?}");
        assert_eq!(
            native.argvs()[before..],
            [argv(&["account", "ls"])],
            "{effort:?}"
        );
        // The journal holds only that failed receipt: no write began.
        let stored: Value =
            serde_json::from_str(&connector.journal.replay(&other).unwrap()).unwrap();
        assert_eq!(stored, receipt, "{effort:?}");
    }
    assert_eq!(native.count(&["opencode"]), 1);
}

#[test]
fn opencode_account_rows_keep_effort_free_and_catalog_fields_within_the_bound() {
    let raw = opencode_catalog();
    let view = accounts::launch_catalog(&raw).unwrap();
    let account = &view["accounts"][0];
    assert_eq!(account["catalog_state"], "ready");
    assert!(account.get("catalog_reason").is_none());
    assert_eq!(account["installed"], true);
    assert_eq!(
        account["model_options"][0],
        json!({"value": "fixture/local", "label": "Local", "provider": "fixture",
               "provider_label": "Fixture", "free": true,
               "effort_options": ["low", "high"], "effort_labels": ["Low", "High"]})
    );
    // A model without variants keeps its old three-field shape.
    assert_eq!(
        account["model_options"][1],
        json!({"value": "fixture/plain", "label": "Plain", "provider": "fixture"})
    );
    let entries = accounts::catalog(&raw).unwrap();
    assert_eq!(entries[0].1.public["model_options"][0]["free"], true);
    assert_eq!(entries[0].1.public["catalog_state"], "ready");
    // An unknown state, an uninstalled reason and oversized variants are bounded.
    let mut row = raw["profiles"][0].clone();
    row["catalog_state"] = json!("surprise");
    row["installed"] = json!(false);
    row["install_reason"] = json!("OpenCode 2 is not installed");
    row["model_options"][0]["effort_options"] =
        json!((0..30).map(|i| format!("v{i}")).collect::<Vec<_>>());
    let view = accounts::launch_catalog(&json!({"profiles": [row]})).unwrap();
    let account = &view["accounts"][0];
    assert!(account.get("catalog_state").is_none());
    assert_eq!(account["install_reason"], "OpenCode 2 is not installed");
    assert_eq!(
        account["model_options"][0]["effort_options"]
            .as_array()
            .unwrap()
            .len(),
        8
    );
    assert_eq!(
        account["model_options"][0]["effort_labels"]
            .as_array()
            .unwrap()
            .len(),
        8
    );
    // The widest realistic catalog (256 models, 8 variants each) stays inside 128 KiB.
    let models: Vec<Value> = (0..256)
        .map(|index| {
            json!({"id": format!("provider/model-{index:03}-with-a-long-name"),
                   "value": format!("provider/model-{index:03}-with-a-long-name"),
                   "label": format!("Provider model {index:03} with a long display name"),
                   "provider": "provider", "provider_label": "A Provider", "free": false,
                   "effort_options": ["none", "minimal", "low", "medium", "high", "xhigh", "max", "ultra"],
                   "effort_labels": ["None", "Minimal", "Low", "Medium", "High", "Extra high", "Max", "Ultra"]})
        })
        .collect();
    let wide = json!({"profiles": [{"id": "native-opencode", "provider": "opencode",
        "label": "OpenCode", "installed": true, "catalog_state": "ready", "model_options": models}]});
    let view = accounts::launch_catalog(&wide).unwrap();
    assert_eq!(
        view["accounts"][0]["model_options"]
            .as_array()
            .unwrap()
            .len(),
        256
    );
    assert!(accounts::catalog(&wide).is_ok());
}

#[test]
fn an_oversized_opencode_projection_degrades_before_it_fails() {
    // 160-character labels and 40-byte variants, the native maximum: the full
    // projection is over 128 KiB, the labels-free one is not.
    let efforts: Vec<String> = (0..8).map(|i| format!("{i}{}", "v".repeat(39))).collect();
    let models: Vec<Value> = (0..160)
        .map(|index| {
            json!({"id": format!("p/m{index:03}"), "value": format!("p/m{index:03}"),
                   "label": "l".repeat(160), "provider": "p", "provider_label": "P", "free": false,
                   "effort_options": efforts, "effort_labels": efforts.iter().map(|_| "L".repeat(40)).collect::<Vec<_>>()})
        })
        .collect();
    let raw = json!({"profiles": [{"id": "native-opencode", "provider": "opencode",
        "label": "OpenCode", "installed": true, "catalog_state": "ready", "model_options": models}]});
    let launch = accounts::launch_catalog(&raw).unwrap();
    let option = &launch["accounts"][0]["model_options"][0];
    assert!(option.get("effort_labels").is_none());
    assert_eq!(option["effort_options"].as_array().unwrap().len(), 8);
    let entries = accounts::catalog(&raw).unwrap();
    assert!(entries[0].1.public["model_options"][0]
        .get("effort_labels")
        .is_none());
    assert!(entries[0].1.public["model_options"][0]
        .get("effort_options")
        .is_some());
    let mut cache = AccountCache::default();
    cache.apply_catalog(Ok(entries), 1000.0);
    assert_eq!(cache.view(1000.0)["available"], true);
    // Variants go too when labels alone are not enough.
    let models: Vec<Value> = (0..256)
        .map(|index| {
            json!({"id": format!("p/m{index:03}"), "value": format!("p/m{index:03}"),
                   "label": "l".repeat(160), "provider": "p", "provider_label": "P", "free": false,
                   "effort_options": efforts, "effort_labels": efforts})
        })
        .collect();
    let raw = json!({"profiles": [{"id": "native-opencode", "provider": "opencode",
        "label": "OpenCode", "installed": true, "model_options": models}]});
    let launch = accounts::launch_catalog(&raw).unwrap();
    assert!(launch["accounts"][0]["model_options"][0]
        .get("effort_options")
        .is_none());
    assert_eq!(
        launch["accounts"][0]["model_options"]
            .as_array()
            .unwrap()
            .len(),
        256
    );
}
