//! Golden vectors: documents and updates written by earlier versions of this
//! crate and of Loro. Every build must still read and merge them. Running with
//! `ZERUS_BLESS=1` writes them anew; do that only to add a vector for a new
//! schema or Loro version, never to make a failing build pass.
use std::fs;
use std::path::PathBuf;
use zerus_replica::{
    Actor, NewTask, ProjectDoc, SessionKey, Settings, Stage, Target, TaskDoc, TaskView,
};

const TASK: &str = "task-schema1-loro1.16";

fn path(name: &str) -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR"))
        .join("tests/vectors")
        .join(name)
}

fn read(name: &str) -> Vec<u8> {
    fs::read(path(name)).unwrap_or_else(|e| panic!("{name}: {e}"))
}

fn blessing() -> bool {
    std::env::var("ZERUS_BLESS").as_deref() == Ok("1")
}

fn api() -> SessionKey {
    SessionKey {
        machine: "00000000-0000-4000-8000-000000000001".into(),
        session: "codex/api".into(),
    }
}

/// The base document, then two replicas that change it at the same time.
fn bless_task() {
    let mut base = TaskDoc::create(NewTask {
        project: "project-1",
        id: "T-a1b2c3",
        text: "Add rate limiting to POST /v1/orders.",
        stage: Stage::Todo,
        target: Target::Session(api()),
        author: Actor::Agent(api()),
        rank: "V",
        now: 1_760_000_000_000,
    })
    .unwrap();
    base.comment("Use the config value.", &Actor::Person, 1_760_000_001_000)
        .unwrap();
    let snapshot = base.snapshot().unwrap();
    let version = base.version();
    let mut a = TaskDoc::from_bytes(&snapshot).unwrap();
    a.record_send(
        &api(),
        "00000000-0000-4000-8000-000000000002",
        &Actor::Person,
        1_760_000_002_000,
    )
    .unwrap();
    a.record_report(
        "Token bucket in Redis; 6 tests.",
        &Actor::Agent(api()),
        1_760_000_003_000,
    )
    .unwrap();
    let mut b = TaskDoc::from_bytes(&snapshot).unwrap();
    b.comment("Limit per API key.", &Actor::Person, 1_760_000_002_500)
        .unwrap();
    let (update_a, update_b) = (
        a.export_since(&version).unwrap(),
        b.export_since(&version).unwrap(),
    );
    a.import(&update_b).unwrap();
    for (name, bytes) in [
        (format!("{TASK}.loro"), snapshot),
        (format!("{TASK}.version"), version),
        (format!("{TASK}-a.update"), update_a),
        (format!("{TASK}-b.update"), update_b),
    ] {
        fs::write(path(&name), bytes).unwrap();
    }
    let merged = serde_json::to_string_pretty(&a.view().unwrap()).unwrap() + "\n";
    fs::write(path(&format!("{TASK}-merged.json")), merged).unwrap();
}

/// Every value the stored view has, this build's view has too. Later builds
/// may add fields; they never drop or change the ones a vector recorded.
fn contains(actual: &serde_json::Value, expected: &serde_json::Value, path: &str) {
    use serde_json::Value;
    match (actual, expected) {
        (Value::Object(actual), Value::Object(expected)) => {
            for (key, value) in expected {
                let found = actual
                    .get(key)
                    .unwrap_or_else(|| panic!("{path}.{key} is missing"));
                contains(found, value, &format!("{path}.{key}"));
            }
        }
        (Value::Array(actual), Value::Array(expected)) => {
            assert_eq!(actual.len(), expected.len(), "{path} length");
            for (index, (found, value)) in actual.iter().zip(expected).enumerate() {
                contains(found, value, &format!("{path}[{index}]"));
            }
        }
        _ => assert_eq!(actual, expected, "{path}"),
    }
}

fn merged(first: &str, second: &str) -> TaskView {
    let mut task = TaskDoc::from_bytes(&read(&format!("{TASK}.loro"))).unwrap();
    task.import(&read(&format!("{TASK}-{first}.update")))
        .unwrap();
    task.import(&read(&format!("{TASK}-{second}.update")))
        .unwrap();
    task.view().unwrap()
}

#[test]
fn task_schema1_loro_1_16() {
    if blessing() {
        bless_task();
    }
    let expected: serde_json::Value =
        serde_json::from_slice(&read(&format!("{TASK}-merged.json"))).unwrap();
    // Concurrent updates merge to the same task in either order.
    for (first, second) in [("a", "b"), ("b", "a")] {
        let view = merged(first, second);
        contains(&serde_json::to_value(&view).unwrap(), &expected, "view");
    }
    // The history reads oldest first, whatever order the merge gave its entries.
    let times: Vec<i64> = merged("a", "b").log.iter().map(|e| e.at).collect();
    assert!(times.windows(2).all(|w| w[0] <= w[1]), "{times:?}");
    // An export since the stored version carries both updates to a fresh copy.
    let mut both = TaskDoc::from_bytes(&read(&format!("{TASK}.loro"))).unwrap();
    both.import(&read(&format!("{TASK}-a.update"))).unwrap();
    both.import(&read(&format!("{TASK}-b.update"))).unwrap();
    let delta = both
        .export_since(&read(&format!("{TASK}.version")))
        .unwrap();
    let mut fresh = TaskDoc::from_bytes(&read(&format!("{TASK}.loro"))).unwrap();
    fresh.import(&delta).unwrap();
    contains(
        &serde_json::to_value(fresh.view().unwrap()).unwrap(),
        &expected,
        "view",
    );
    // The stored document still accepts this build's edits.
    both.set_stage(Stage::Done, &Actor::Person, 1_760_000_004_000)
        .unwrap();
    assert_eq!(both.view().unwrap().stage, Stage::Done);
}

#[test]
fn project_schema1_loro_1_16() {
    let (bytes, expected) = (
        "project-schema1-loro1.16.loro",
        "project-schema1-loro1.16.json",
    );
    if blessing() {
        let mut project = ProjectDoc::new();
        project
            .set_settings(&Settings {
                agents_other: false,
                agent_limit: 3,
                ..Settings::default()
            })
            .unwrap();
        fs::write(path(bytes), project.snapshot().unwrap()).unwrap();
        let settings = serde_json::to_string_pretty(&project.settings()).unwrap() + "\n";
        fs::write(path(expected), settings).unwrap();
    }
    let project = ProjectDoc::from_bytes(&read(bytes)).unwrap();
    let expected: Settings = serde_json::from_slice(&read(expected)).unwrap();
    assert_eq!(project.settings(), expected);
}
