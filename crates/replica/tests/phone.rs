//! The phone's task exchange: the computer's answer and the phone's side,
//! played against each other in one process as the relay carries them.
use serde_json::{json, Value};
use sha2::{Digest, Sha256};
use std::collections::BTreeSet;
use std::fs;
use std::path::{Path, PathBuf};
use zerus_replica::phone::{answer, Answer, Budget, Exchange, Progress, Request};
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

/// Text that does not compress.
fn noise(seed: &str, length: usize) -> String {
    let mut out = String::new();
    while out.len() < length {
        out += &format!("{:x}", Sha256::digest(format!("{seed}-{}", out.len())));
    }
    out.truncate(length);
    out
}

fn comment(store: &ProjectStore, uuid: &str, text: &str) {
    let mut task = store.load(uuid).unwrap();
    task.comment(text, &Actor::Person, 5).unwrap();
    store.save(&mut task).unwrap();
}

fn comments(store: &ProjectStore, uuid: &str) -> usize {
    let task = store.load(uuid).unwrap();
    task.view()
        .unwrap()
        .log
        .iter()
        .filter(|e| e.kind == "comment")
        .count()
}

fn live_digest(store: &ProjectStore) -> String {
    store.live_inventory().unwrap().0.digest()
}

fn task_file(root: &Path, uuid: &str) -> PathBuf {
    root.join(format!("{:x}", Sha256::digest(b"project-1")))
        .join("tasks")
        .join(format!("{uuid}.loro"))
}

/// Every key, scalar, object and array, as the relay counts toward 50,000.
fn values(value: &Value) -> usize {
    match value {
        Value::Object(map) => 1 + map.values().map(|v| 1 + values(v)).sum::<usize>(),
        Value::Array(items) => 1 + items.iter().map(values).sum::<usize>(),
        _ => 1,
    }
}

/// A request or an answer as the relay carries it, within its bounds with
/// room for the envelope.
fn wire<T: serde::Serialize + serde::de::DeserializeOwned>(value: &T) -> T {
    let json = serde_json::to_value(value).unwrap();
    let size = serde_json::to_vec(&json).unwrap().len();
    assert!(size <= (1 << 20) - 4096, "{size} bytes");
    assert!(values(&json) <= 49_000, "{} values", values(&json));
    serde_json::from_value(json).unwrap()
}

#[derive(Default)]
struct Run {
    requests: usize,
    ranges: BTreeSet<String>,
    parts: usize,
    progress: Vec<Progress>,
    error: Option<String>,
}

/// One request and its answer; false once the exchange is done.
fn step(
    exchange: &mut Exchange,
    phone: &ProjectStore,
    computer: &ProjectStore,
    run: &mut Run,
) -> bool {
    let request = match exchange.next_request(phone) {
        Ok(Some(request)) => wire(&request),
        Ok(None) => return false,
        Err(error) => {
            run.error = Some(error.to_string());
            return false;
        }
    };
    run.requests += 1;
    run.ranges
        .insert(serde_json::to_string(&request.range).unwrap());
    let answered = wire(&answer(computer, request).unwrap());
    run.parts += usize::from(answered.part.is_some());
    match exchange.accept(phone, answered) {
        Ok(progress) => run.progress.push(progress),
        Err(error) => {
            run.error = Some(error.to_string());
            return false;
        }
    }
    true
}

fn run(phone: &ProjectStore, computer: &ProjectStore, budget: Budget) -> Run {
    let mut exchange = Exchange::with_budget(budget);
    let mut run = Run::default();
    while step(&mut exchange, phone, computer, &mut run) {}
    run
}

fn settled(run: Run) -> Run {
    assert_eq!(run.error, None);
    assert!(run.progress.last().is_some_and(|p| p.finished));
    run
}

fn pair() -> (
    tempfile::TempDir,
    tempfile::TempDir,
    ProjectStore,
    ProjectStore,
) {
    let (a, b) = (tempfile::tempdir().unwrap(), tempfile::tempdir().unwrap());
    let (phone, computer) = (store(a.path()), store(b.path()));
    (a, b, phone, computer)
}

#[test]
fn a_fresh_phone_gets_every_live_task() {
    let (_a, _b, phone, computer) = pair();
    for n in 0..30 {
        add(&computer, &format!("Task {n}"));
    }
    let mut done = add(&computer, "Document the limits");
    accept(&computer, &mut done);
    let pulled = settled(run(&phone, &computer, Budget::default()));
    assert_eq!(live_digest(&phone), live_digest(&computer));
    assert_eq!(
        pulled.progress.last().unwrap().digest,
        live_digest(&computer)
    );
    assert_eq!(phone.inventory().unwrap().0.tasks.len(), 30);
}

#[test]
fn the_phone_s_edits_reach_the_computer() {
    let (_a, _b, phone, computer) = pair();
    let task = add(&computer, "Add rate limiting");
    settled(run(&phone, &computer, Budget::default()));
    comment(&phone, task.uuid(), "From the phone");
    settled(run(&phone, &computer, Budget::default()));
    assert_eq!(comments(&computer, task.uuid()), 1);
    assert_eq!(live_digest(&phone), live_digest(&computer));
}

#[test]
fn both_sides_changed() {
    let (_a, _b, phone, computer) = pair();
    let task = add(&computer, "Add rate limiting");
    settled(run(&phone, &computer, Budget::default()));
    comment(&phone, task.uuid(), "From the phone");
    comment(&computer, task.uuid(), "From the computer");
    settled(run(&phone, &computer, Budget::default()));
    assert_eq!(comments(&phone, task.uuid()), 2);
    assert_eq!(comments(&computer, task.uuid()), 2);
    assert_eq!(live_digest(&phone), live_digest(&computer));
}

#[test]
fn a_held_task_follows_into_done() {
    let (_a, _b, phone, computer) = pair();
    let mut task = add(&computer, "Add rate limiting");
    settled(run(&phone, &computer, Budget::default()));
    accept(&computer, &mut task);
    settled(run(&phone, &computer, Budget::default()));
    assert_eq!(
        phone.load(task.uuid()).unwrap().stage().unwrap(),
        Stage::Done
    );
    assert_eq!(live_digest(&phone), live_digest(&computer));
}

#[test]
fn every_request_fits_the_relay() {
    let (_a, _b, phone, computer) = pair();
    // Every writer adds a peer to a task's version: 300 tasks of 40 writers
    // each pass both the inventory's bytes and its values budget.
    for n in 0..300 {
        let mut task = add(&computer, &format!("Task {n}"));
        for c in 0..39 {
            task = TaskDoc::from_bytes(&task.snapshot().unwrap()).unwrap();
            task.comment(&format!("Note {c}"), &Actor::Person, 5)
                .unwrap();
        }
        computer.save(&mut task).unwrap();
    }
    let peers = computer
        .inventory()
        .unwrap()
        .0
        .tasks
        .values()
        .next()
        .unwrap()
        .len();
    assert_eq!(peers, 40);
    let first = settled(run(&phone, &computer, Budget::default()));
    assert!(first.ranges.len() > 1, "{:?}", first.ranges);
    assert_eq!(live_digest(&phone), live_digest(&computer));
    // Nothing left: one request per range, and nothing changes.
    let again = settled(run(&phone, &computer, Budget::default()));
    assert_eq!(again.requests, again.ranges.len());
    assert!(again.progress.iter().all(|p| p.changed == 0));
}

#[test]
fn small_budgets_split_ranges_and_still_converge() {
    let (_a, _b, phone, computer) = pair();
    for n in 0..200 {
        add(&computer, &format!("Task {n}"));
    }
    let budget = Budget {
        inventory_bytes: 4096,
        ..Budget::default()
    };
    let first = settled(run(&phone, &computer, budget));
    assert!(first.ranges.len() > 3, "{:?}", first.ranges);
    assert_eq!(live_digest(&phone), live_digest(&computer));
    let task = phone
        .inventory()
        .unwrap()
        .0
        .tasks
        .keys()
        .nth(150)
        .unwrap()
        .clone();
    comment(&phone, &task, "From the phone");
    settled(run(&phone, &computer, budget));
    assert_eq!(comments(&computer, &task), 1);
}

#[test]
fn a_large_document_arrives_in_parts() {
    let (_a, _b, phone, computer) = pair();
    let task = add(&computer, "Add rate limiting");
    for n in 0..5 {
        comment(
            &computer,
            task.uuid(),
            &noise(&format!("comment-{n}"), 60_000),
        );
    }
    let done = settled(run(&phone, &computer, Budget::default()));
    assert!(done.parts >= 2, "{} parts", done.parts);
    assert_eq!(comments(&phone, task.uuid()), 5);
}

#[test]
fn a_part_out_of_step_starts_again() {
    let (_a, _b, phone, computer) = pair();
    let task = add(&computer, "Add rate limiting");
    for n in 0..5 {
        comment(
            &computer,
            task.uuid(),
            &noise(&format!("comment-{n}"), 60_000),
        );
    }
    let mut exchange = Exchange::new();
    let first = exchange.next_request(&phone).unwrap().unwrap();
    let answered = answer(&computer, first).unwrap();
    let mut forged = answered.part.clone().unwrap();
    exchange.accept(&phone, answered).unwrap();
    let asked = exchange.next_request(&phone).unwrap().unwrap();
    assert!(asked.resume.is_some());
    // A part that does not continue what arrived drops the document.
    forged.offset = 999;
    let out_of_step = Answer {
        schema: 1,
        imported: 0,
        refused: vec![],
        updates: vec![],
        part: Some(forged),
        more: true,
        digest: live_digest(&computer),
        inventory: None,
    };
    exchange.accept(&phone, out_of_step).unwrap();
    let afresh = exchange.next_request(&phone).unwrap().unwrap();
    assert_eq!(afresh.resume, None);
    // A complete document whose SHA-256 does not match is never imported, even
    // when its bytes would apply.
    let small = add(&computer, "Measure first");
    let bytes = computer.load(small.uuid()).unwrap().snapshot().unwrap();
    let mismatched = Answer {
        part: Some(zerus_replica::phone::Part {
            task: Some(small.uuid().to_owned()),
            size: bytes.len(),
            sha256: "00".repeat(32),
            offset: 0,
            bytes: base64::Engine::encode(&base64::engine::general_purpose::STANDARD, &bytes),
        }),
        updates: vec![],
        ..answer(&computer, afresh).unwrap()
    };
    let progress = exchange.accept(&phone, mismatched).unwrap();
    assert_eq!(progress.changed, 0);
    assert!(phone.inventory().unwrap().0.tasks.is_empty());
    // The rest of the exchange brings it whole.
    let mut rest = Run::default();
    while step(&mut exchange, &phone, &computer, &mut rest) {}
    settled(rest);
    assert_eq!(comments(&phone, task.uuid()), 5);
    assert!(phone.load(small.uuid()).is_ok());
}

/// A task of another project, saved into `store`'s folder as a misplaced file would be.
fn misplaced(store: &ProjectStore) -> String {
    let mut task = TaskDoc::create(NewTask {
        project: "project-2",
        id: &store.unused_short_id().unwrap(),
        text: "Filed elsewhere",
        stage: Stage::Todo,
        target: Target::Any,
        author: Actor::Person,
        rank: "V",
        now: 1,
    })
    .unwrap();
    store.save(&mut task).unwrap();
    task.uuid().to_owned()
}

#[test]
fn refused_documents_are_not_sent_again() {
    let (_a, b, phone, computer) = pair();
    let good = add(&computer, "Add rate limiting");
    settled(run(&phone, &computer, Budget::default()));
    // The computer's copy no longer loads: the phone's whole document replaces it.
    fs::write(task_file(b.path(), good.uuid()), b"damaged").unwrap();
    // The computer refuses a task of another project once, and it is not sent again.
    let stray = misplaced(&phone);
    let done = settled(run(&phone, &computer, Budget::default()));
    assert_eq!(comments(&computer, good.uuid()), 0);
    assert!(computer.load(good.uuid()).is_ok());
    let refused: Vec<_> = done
        .progress
        .iter()
        .flat_map(|p| p.refused_there.iter().map(|d| d.uuid.clone()))
        .collect();
    assert_eq!(refused, [stray]);
}

#[test]
fn an_answer_that_only_repeats_a_refusal_ends_the_exchange() {
    let (_a, _b, phone, computer) = pair();
    add(&computer, "Add rate limiting");
    misplaced(&computer);
    let ended = run(&phone, &computer, Budget::default());
    let error = ended.error.unwrap();
    assert!(error.contains("do not apply here"), "{error}");
    assert!(ended.requests < 6, "{} requests", ended.requests);
}

#[test]
fn a_document_too_large_to_send_is_named_and_skipped() {
    let (_a, _b, phone, computer) = pair();
    let task = add(&computer, "Add rate limiting");
    let small = add(&computer, "Measure first");
    settled(run(&phone, &computer, Budget::default()));
    comment(&phone, task.uuid(), &noise("a", 50_000));
    comment(&phone, task.uuid(), &noise("b", 50_000));
    comment(&phone, small.uuid(), "Short");
    let budget = Budget {
        push_bytes: 64 * 1024,
        ..Budget::default()
    };
    let done = settled(run(&phone, &computer, budget));
    let unsent: BTreeSet<_> = done
        .progress
        .iter()
        .flat_map(|p| p.unsent.clone())
        .collect();
    assert_eq!(unsent, BTreeSet::from([task.uuid().to_owned()]));
    assert_eq!(comments(&computer, small.uuid()), 1);
    assert_eq!(comments(&computer, task.uuid()), 0);
}

#[test]
fn a_document_that_leaves_while_in_parts_ends_the_exchange() {
    let (_a, _b, phone, computer) = pair();
    let task = add(&computer, "Add rate limiting");
    for n in 0..5 {
        comment(
            &computer,
            task.uuid(),
            &noise(&format!("comment-{n}"), 60_000),
        );
    }
    let mut exchange = Exchange::new();
    let mut run = Run::default();
    assert!(step(&mut exchange, &phone, &computer, &mut run));
    assert_eq!(run.parts, 1);
    // Accepted into Done before its last part: the phone never held it, so the
    // computer no longer offers it.
    let mut fresh = computer.load(task.uuid()).unwrap();
    accept(&computer, &mut fresh);
    while step(&mut exchange, &phone, &computer, &mut run) {}
    let run = settled(run);
    assert!(run.requests < 5, "{} requests", run.requests);
    assert!(phone.inventory().unwrap().0.tasks.is_empty());
}

fn empty_answer(computer: &ProjectStore) -> Answer {
    Answer {
        schema: 1,
        imported: 0,
        refused: vec![],
        updates: vec![],
        part: None,
        more: false,
        digest: live_digest(computer),
        inventory: None,
    }
}

#[test]
fn a_part_of_a_document_larger_than_any_task_is_refused() {
    let (_a, _b, phone, computer) = pair();
    let mut exchange = Exchange::new();
    exchange.next_request(&phone).unwrap().unwrap();
    let huge = Answer {
        part: Some(zerus_replica::phone::Part {
            task: Some("0b9d6c55-2f5e-4c39-9a51-0d6f3c2a7e11".into()),
            size: 9 << 20,
            sha256: "00".repeat(32),
            offset: 0,
            bytes: "bG9ybw==".into(),
        }),
        more: true,
        ..empty_answer(&computer)
    };
    let error = exchange.accept(&phone, huge).unwrap_err().to_string();
    assert!(error.contains("too large"), "{error}");
}

#[test]
fn an_answer_that_promises_more_and_brings_nothing_ends_the_exchange() {
    let (_a, _b, phone, computer) = pair();
    add(&computer, "Add rate limiting");
    let mut exchange = Exchange::new();
    exchange.next_request(&phone).unwrap().unwrap();
    let empty = Answer {
        more: true,
        ..empty_answer(&computer)
    };
    let error = exchange.accept(&phone, empty).unwrap_err().to_string();
    assert!(error.contains("brought nothing"), "{error}");
    assert!(exchange.finished());
}

#[test]
fn a_done_task_the_phone_asks_for_arrives_and_stays() {
    let (_a, _b, phone, computer) = pair();
    add(&computer, "Add rate limiting");
    let mut done = add(&computer, "Document the limits");
    accept(&computer, &mut done);
    settled(run(&phone, &computer, Budget::default()));
    assert!(phone.load(done.uuid()).is_err());
    // Opening it on the phone fetches it whole; later exchanges keep it.
    let mut exchange = Exchange::with_budget(Budget::default());
    exchange.fetch([done.uuid().to_owned()]);
    let mut fetched = Run::default();
    while step(&mut exchange, &phone, &computer, &mut fetched) {}
    settled(fetched);
    assert_eq!(
        phone.load(done.uuid()).unwrap().stage().unwrap(),
        Stage::Done
    );
    comment(&computer, done.uuid(), "Later on the computer");
    settled(run(&phone, &computer, Budget::default()));
    assert_eq!(comments(&phone, done.uuid()), 1);
    // A task the computer does not have ends the exchange all the same.
    let mut unknown = Exchange::new();
    unknown.fetch(["00000000-0000-4000-8000-000000000009".to_owned()]);
    let mut nothing = Run::default();
    while step(&mut unknown, &phone, &computer, &mut nothing) {}
    settled(nothing);
}
