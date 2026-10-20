//! The binding surface, called from Rust as the apps call it.
use zerus_replica_ffi::{
    ReplicaError, TaskPosition, TaskSettings, TaskStage, TaskStore, TaskTarget,
};

/// A task sent, reported and accepted into Done, as only hgs records a send.
fn accepted(root: &tempfile::TempDir, text: &str) -> String {
    use zerus_replica::{Actor, NewTask, ProjectStore, SessionKey, Stage, Target, TaskDoc};
    let store = ProjectStore::open(root.path(), "project-1").unwrap();
    let mut task = TaskDoc::create(NewTask {
        project: "project-1",
        id: &store.unused_short_id().unwrap(),
        text,
        stage: Stage::Todo,
        target: Target::Any,
        author: Actor::Person,
        rank: "V",
        now: 1,
    })
    .unwrap();
    let session = SessionKey {
        machine: "m".into(),
        session: "codex/api".into(),
    };
    task.record_send(&session, "request-1", &Actor::Person, 2)
        .unwrap();
    task.record_report("Done.", &Actor::Agent(session), 3)
        .unwrap();
    task.set_stage(Stage::Done, &Actor::Person, 4).unwrap();
    store.save(&mut task).unwrap();
    task.uuid().to_owned()
}

fn store(root: &tempfile::TempDir) -> std::sync::Arc<TaskStore> {
    TaskStore::new(
        root.path().display().to_string(),
        "project-1".into(),
        "orchard".into(),
    )
}

#[test]
fn tasks_are_filed_listed_and_changed() {
    let root = tempfile::tempdir().unwrap();
    let store = store(&root);
    let second = store
        .add(
            "Add rate limiting".into(),
            TaskStage::Todo,
            TaskTarget::AnySession,
            TaskPosition::End,
        )
        .unwrap();
    let first = store
        .add(
            "Measure first".into(),
            TaskStage::Todo,
            TaskTarget::AnySession,
            TaskPosition::Top,
        )
        .unwrap();
    let listed: Vec<_> = store
        .list()
        .unwrap()
        .tasks
        .into_iter()
        .map(|t| t.text)
        .collect();
    assert_eq!(listed, ["Measure first", "Add rate limiting"]);
    assert!(first.id.starts_with("T-"));
    assert_eq!(store.get(second.id.clone()).unwrap().uuid, second.uuid);
    store
        .edit_text(second.uuid.clone(), "Add rate limiting, edited".into())
        .unwrap();
    store
        .move_task(second.uuid.clone(), TaskPosition::Top)
        .unwrap();
    let moved = store
        .set_stage(first.uuid.clone(), TaskStage::Backlog)
        .unwrap();
    assert_eq!(moved.stage, TaskStage::Backlog);
    let commented = store
        .comment(second.uuid.clone(), "Measured: 40 rps.".into())
        .unwrap();
    let entry = commented.log.iter().find(|e| e.kind == "comment").unwrap();
    assert_eq!(entry.text, "Measured: 40 rps.");
    let todo: Vec<_> = store
        .list()
        .unwrap()
        .tasks
        .into_iter()
        .filter(|t| t.stage == TaskStage::Todo)
        .map(|t| t.text)
        .collect();
    assert_eq!(todo, ["Add rate limiting, edited"]);
}

#[test]
fn refusals_and_failures_stay_apart() {
    let root = tempfile::tempdir().unwrap();
    let store = store(&root);
    let task = store
        .add(
            "Add rate limiting".into(),
            TaskStage::Todo,
            TaskTarget::AnySession,
            TaskPosition::End,
        )
        .unwrap();
    match store.edit_text(task.uuid.clone(), "   ".into()) {
        Err(ReplicaError::Refused(message)) => assert!(message.contains("empty"), "{message}"),
        other => panic!("expected a refusal, got {other:?}"),
    }
    match store.add(
        "Nope".into(),
        TaskStage::Done,
        TaskTarget::AnySession,
        TaskPosition::End,
    ) {
        Err(ReplicaError::Refused(message)) => {
            assert!(message.contains("Backlog or To do"), "{message}")
        }
        other => panic!("expected a refusal, got {other:?}"),
    }
    assert!(matches!(
        store.get("T-zzzzzz".into()),
        Err(ReplicaError::Refused(_)) | Err(ReplicaError::Failed(_))
    ));
}

#[test]
fn settings_change_and_come_back() {
    let root = tempfile::tempdir().unwrap();
    let store = store(&root);
    let mut settings: TaskSettings = store.settings().unwrap();
    assert!(settings.agents_other);
    settings.agents_other = false;
    settings.agent_limit = 3;
    let saved = store.set_settings(settings).unwrap();
    assert_eq!((saved.agents_other, saved.agent_limit), (false, 3));
    assert_eq!(store.settings().unwrap().agent_limit, 3);
}

#[test]
fn two_replicas_meet_through_the_api() {
    let (a_root, b_root) = (tempfile::tempdir().unwrap(), tempfile::tempdir().unwrap());
    let (a, b) = (store(&a_root), store(&b_root));
    a.add(
        "Add rate limiting".into(),
        TaskStage::Todo,
        TaskTarget::AnySession,
        TaskPosition::End,
    )
    .unwrap();
    let page = a.updates_for(b.inventory().unwrap(), 1 << 20).unwrap();
    assert!(!page.more);
    let imported = b.import_updates(page.updates).unwrap();
    assert!(imported.changed >= 1 && imported.refused.is_empty());
    assert_eq!(b.list().unwrap().tasks[0].text, "Add rate limiting");
    assert_eq!(a.inventory().unwrap(), b.inventory().unwrap());
}

#[test]
fn a_damaged_inventory_is_refused() {
    let root = tempfile::tempdir().unwrap();
    assert!(matches!(
        store(&root).updates_for("not json".into(), 1 << 20),
        Err(ReplicaError::Refused(_))
    ));
}

#[test]
fn a_held_project_answers_busy_instead_of_waiting() {
    let root = tempfile::tempdir().unwrap();
    let store = store(&root);
    store
        .add(
            "Add rate limiting".into(),
            TaskStage::Todo,
            TaskTarget::AnySession,
            TaskPosition::End,
        )
        .unwrap();
    // Another caller, such as an exchange, holds the project.
    let held = zerus_replica::ProjectStore::open(root.path(), "project-1").unwrap();
    let started = std::time::Instant::now();
    match store.list() {
        Err(ReplicaError::Failed(message)) => assert!(message.contains("busy"), "{message}"),
        other => panic!("expected the busy failure, got {other:?}"),
    }
    assert!(started.elapsed() < std::time::Duration::from_secs(15));
    drop(held);
    assert_eq!(store.list().unwrap().tasks.len(), 1);
}

#[test]
fn two_stores_exchange_through_json() {
    let (a, b) = (tempfile::tempdir().unwrap(), tempfile::tempdir().unwrap());
    let (computer, phone) = (store(&a), store(&b));
    for text in ["Add rate limiting", "Measure first"] {
        computer
            .add(
                text.into(),
                TaskStage::Todo,
                TaskTarget::AnySession,
                TaskPosition::End,
            )
            .unwrap();
    }
    assert_ne!(
        computer.live_digest().unwrap(),
        phone.live_digest().unwrap()
    );
    let exchange = phone.clone().exchange();
    let mut last = None;
    while let Some(request) = exchange.next_request().unwrap() {
        let answer = computer.answer_exchange(request).unwrap();
        last = Some(exchange.accept(answer).unwrap());
    }
    let last = last.unwrap();
    assert!(last.finished);
    assert_eq!(last.digest, computer.live_digest().unwrap());
    assert_eq!(
        computer.live_digest().unwrap(),
        phone.live_digest().unwrap()
    );
    assert_eq!(phone.list().unwrap().tasks.len(), 2);
    // A task the phone does not hold, such as a Done task opened on it, comes
    // when the exchange asks for it.
    let done = accepted(&a, "Document the limits");
    // A plain exchange leaves a Done task on the computer.
    let plain = phone.clone().exchange();
    while let Some(request) = plain.next_request().unwrap() {
        plain
            .accept(computer.answer_exchange(request).unwrap())
            .unwrap();
    }
    assert!(phone.get(done.clone()).is_err());
    let fetching = phone.clone().exchange_fetching(vec![done.clone()]);
    while let Some(request) = fetching.next_request().unwrap() {
        fetching
            .accept(computer.answer_exchange(request).unwrap())
            .unwrap();
    }
    assert_eq!(phone.get(done.clone()).unwrap().text, "Document the limits");
    assert_eq!(phone.get(done.clone()).unwrap().stage, TaskStage::Done);
    // An answer that is not JSON is refused with its reason.
    let again = phone.clone().exchange();
    again.next_request().unwrap().unwrap();
    assert!(matches!(
        again.accept("not json".into()),
        Err(ReplicaError::Refused(message)) if message.contains("invalid exchange answer")
    ));
}
