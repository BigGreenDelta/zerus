//! The phone's task exchange: the computer's answer and the phone's side,
//! played against each other in one process as the relay carries them.
use serde_json::json;
use std::path::Path;
use zerus_replica::phone::{answer, Request};
use zerus_replica::{Actor, Inventory, NewTask, ProjectStore, SessionKey, Stage, Target, TaskDoc};

fn store(root: &Path) -> ProjectStore {
    ProjectStore::open(root, "project-1").unwrap()
}

fn add(store: &ProjectStore, text: &str) -> TaskDoc {
    let mut task = TaskDoc::create(NewTask {
        project: store.project_id(),
        id: &store.unused_short_id().unwrap(),
        text,
        stage: Stage::Todo,
        target: Target::Any,
        author: Actor::Person,
        rank: "V",
        now: 1,
    })
    .unwrap();
    store.save(&mut task).unwrap();
    task
}

/// Sends, reports and accepts `task`: it ends in Done.
fn accept(store: &ProjectStore, task: &mut TaskDoc) {
    let session = SessionKey {
        machine: "m".into(),
        session: "codex/api".into(),
    };
    task.record_send(&session, "request-1", &Actor::Person, 2)
        .unwrap();
    task.record_report("Done.", &Actor::Agent(session.clone()), 3)
        .unwrap();
    task.set_stage(Stage::Done, &Actor::Person, 4).unwrap();
    store.save(task).unwrap();
}

fn request(value: serde_json::Value) -> Request {
    serde_json::from_value(value).unwrap()
}

fn refusal(store: &ProjectStore, value: serde_json::Value) -> String {
    answer(store, request(value)).unwrap_err().to_string()
}

#[test]
fn an_empty_phone_is_answered_the_live_tasks() {
    let root = tempfile::tempdir().unwrap();
    let computer = store(root.path());
    let live = [
        add(&computer, "Add rate limiting"),
        add(&computer, "Measure first"),
    ];
    let mut done = add(&computer, "Document the limits");
    accept(&computer, &mut done);
    let answered = answer(
        &computer,
        request(
            json!({"schema": 1, "project": "project-1", "inventory": {"project": {}, "tasks": {}}}),
        ),
    )
    .unwrap();
    let mut sent: Vec<_> = answered
        .updates
        .iter()
        .map(|u| u.task.clone().unwrap())
        .collect();
    sent.sort();
    let mut expected: Vec<_> = live.iter().map(|t| t.uuid().to_owned()).collect();
    expected.sort();
    assert_eq!(sent, expected);
    assert!(!answered.more);
    assert_eq!(
        answered.digest,
        computer.live_inventory().unwrap().0.digest()
    );
    assert_eq!(answered.inventory, None);
}

#[test]
fn the_answer_s_rules_refuse_what_hgs_refused() {
    let root = tempfile::tempdir().unwrap();
    let computer = store(root.path());
    let base =
        json!({"schema": 1, "project": "project-1", "inventory": {"project": {}, "tasks": {}}});
    let with = |key: &str, value: serde_json::Value| {
        let mut changed = base.clone();
        changed[key] = value;
        changed
    };
    const LOW: &str = "00000000-0000-4000-8000-000000000001";
    const HIGH: &str = "ffffffff-ffff-4fff-bfff-ffffffffffff";
    assert!(refusal(&computer, with("schema", json!(2))).contains("unknown exchange schema 2"));
    assert!(refusal(&computer, with("project", json!("project-2")))
        .contains("the request is for project \"project-2\""));
    assert!(refusal(
        &computer,
        with("range", json!({"after": HIGH, "through": LOW}))
    )
    .contains("an empty range"));
    assert!(
        refusal(&computer, with("range", json!({"after": "orchard"}))).contains("not a task UUID")
    );
    let mut outside = with("range", json!({"after": LOW}));
    outside["inventory"]["tasks"] = json!({LOW: {}});
    assert!(refusal(&computer, outside).contains("lies outside its range"));
    let many: Vec<_> = (0..257)
        .map(|_| json!({"task": uuid::Uuid::new_v4().to_string(), "bytes": ""}))
        .collect();
    assert!(refusal(&computer, with("updates", json!(many))).contains("at most 256 updates"));
    assert!(refusal(
        &computer,
        with("updates", json!([{"task": null, "bytes": "%%%"}]))
    )
    .contains("invalid base64 in an update"));
}

#[test]
fn a_range_answers_its_own_tasks() {
    let root = tempfile::tempdir().unwrap();
    let computer = store(root.path());
    let mut uuids: Vec<String> = (0..6)
        .map(|n| add(&computer, &format!("Task {n}")).uuid().to_owned())
        .collect();
    uuids.sort();
    let split = &uuids[2];
    let ask = |range: serde_json::Value| {
        let answered = answer(
            &computer,
            request(json!({"schema": 1, "project": "project-1",
                           "inventory": {"project": {}, "tasks": {}}, "range": range})),
        )
        .unwrap();
        let mut sent: Vec<_> = answered
            .updates
            .into_iter()
            .map(|u| u.task.unwrap())
            .collect();
        sent.sort();
        sent
    };
    assert_eq!(ask(json!({"through": split})), uuids[..3]);
    assert_eq!(ask(json!({"after": split})), uuids[3..]);
}

#[test]
fn the_wire_form_round_trips() {
    let wire = json!({"schema": 1, "project": "project-1",
        "inventory": {"project": {"12": 3}, "tasks": {}},
        "range": {"after": "00000000-0000-4000-8000-000000000001"},
        "updates": [{"task": null, "bytes": "bG9ybw=="}],
        "continue": {"task": null, "sha256": "ab".repeat(32), "offset": 7}});
    let parsed = request(wire.clone());
    assert_eq!(serde_json::to_value(&parsed).unwrap(), wire);
    // Absent optional fields stay absent.
    let bare =
        json!({"schema": 1, "project": "project-1", "inventory": {"project": {}, "tasks": {}}});
    assert_eq!(serde_json::to_value(request(bare.clone())).unwrap(), bare);
    // An answer with nothing for the phone names its inventory and no part.
    let root = tempfile::tempdir().unwrap();
    let computer = store(root.path());
    let answered = serde_json::to_value(answer(&computer, request(bare)).unwrap()).unwrap();
    assert_eq!(
        answered,
        json!({"schema": 1, "imported": 0, "refused": [], "updates": [], "more": false,
               "digest": Inventory::default().digest(), "inventory": {"project": {}, "tasks": {}}})
    );
}
