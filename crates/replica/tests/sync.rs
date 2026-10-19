//! Replicating a project between two stores: inventories, digests, paged
//! updates and imports, as computers exchange them.
use sha2::{Digest, Sha256};
use std::fs;
use std::path::{Path, PathBuf};
use std::time::{Duration, Instant, SystemTime};
use zerus_replica::{
    Actor, Document, Inventory, NewTask, ProjectStore, Settings, Stage, Target, TaskDoc, Update,
};

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

/// Everything `from` holds that `to` lacks, page by page.
fn replicate(from: &ProjectStore, to: &ProjectStore, limit: usize) -> usize {
    let mut pages = 0;
    loop {
        let (theirs, _) = to.inventory().unwrap();
        let (updates, more) = from
            .updates_for(&from.inventory().unwrap().0, &theirs, limit)
            .unwrap();
        if updates.is_empty() {
            return pages;
        }
        pages += 1;
        let imported = to.import_updates(updates).unwrap();
        assert!(imported.refused.is_empty(), "{:?}", imported.refused);
        if !more {
            return pages;
        }
    }
}

/// Text that does not compress: snapshots are compressed.
fn noise(seed: &str, length: usize) -> String {
    let mut out = String::new();
    while out.len() < length {
        out += &format!("{:x}", Sha256::digest(format!("{seed}-{}", out.len())));
    }
    out.truncate(length);
    out
}

fn texts(store: &ProjectStore) -> Vec<String> {
    let (tasks, _) = store.listing().unwrap();
    let mut out: Vec<String> = tasks.iter().map(|t| t.view().unwrap().text).collect();
    out.sort();
    out
}

#[test]
fn two_replicas_meet_and_then_agree() {
    let (a, b) = (tempfile::tempdir().unwrap(), tempfile::tempdir().unwrap());
    let (a, b) = (store(a.path()), store(b.path()));
    add(&a, "Add rate limiting");
    add(&a, "Document the setup");
    let mut doc = a.project().unwrap();
    doc.set_settings(&Settings {
        agent_limit: 3,
        ..doc.settings()
    })
    .unwrap();
    a.save_project(&doc).unwrap();
    assert_ne!(
        a.inventory().unwrap().0.digest(),
        b.inventory().unwrap().0.digest()
    );
    replicate(&a, &b, 1 << 20);
    assert_eq!(texts(&b), ["Add rate limiting", "Document the setup"]);
    assert_eq!(b.project().unwrap().settings().agent_limit, 3);
    // Equal versions, equal digests; nothing is left to send either way.
    assert_eq!(a.inventory().unwrap().0, b.inventory().unwrap().0);
    assert_eq!(
        a.inventory().unwrap().0.digest(),
        b.inventory().unwrap().0.digest()
    );
    assert!(a
        .updates_for(
            &a.inventory().unwrap().0,
            &b.inventory().unwrap().0,
            1 << 20
        )
        .unwrap()
        .0
        .is_empty());
    assert!(b
        .updates_for(
            &b.inventory().unwrap().0,
            &a.inventory().unwrap().0,
            1 << 20
        )
        .unwrap()
        .0
        .is_empty());
}

#[test]
fn edits_made_on_both_sides_merge() {
    let (a, b) = (tempfile::tempdir().unwrap(), tempfile::tempdir().unwrap());
    let (a, b) = (store(a.path()), store(b.path()));
    let task = add(&a, "Add rate limiting");
    replicate(&a, &b, 1 << 20);
    let mut here = a.load(task.uuid()).unwrap();
    here.comment("Use the config value.", &Actor::Person, 2)
        .unwrap();
    a.save(&mut here).unwrap();
    let mut there = b.load(task.uuid()).unwrap();
    there.set_stage(Stage::Backlog, &Actor::Person, 3).unwrap();
    b.save(&mut there).unwrap();
    replicate(&a, &b, 1 << 20);
    replicate(&b, &a, 1 << 20);
    for side in [&a, &b] {
        let view = side.load(task.uuid()).unwrap().view().unwrap();
        assert_eq!(view.stage, Stage::Backlog);
        assert!(view
            .log
            .iter()
            .any(|e| e.kind == "comment" && e.text == "Use the config value."));
    }
    assert_eq!(
        a.inventory().unwrap().0.digest(),
        b.inventory().unwrap().0.digest()
    );
}

#[test]
fn updates_come_in_pages_and_a_large_document_alone() {
    let (a, b) = (tempfile::tempdir().unwrap(), tempfile::tempdir().unwrap());
    let (a, b) = (store(a.path()), store(b.path()));
    for n in 0..6 {
        add(&a, &noise(&n.to_string(), 2000));
    }
    // A page stays within its limit (each snapshot here is about 5 KB) unless one
    // document alone exceeds it.
    let (theirs, _) = b.inventory().unwrap();
    let (page, more) = a
        .updates_for(&a.inventory().unwrap().0, &theirs, 12_000)
        .unwrap();
    assert!(more);
    assert!(
        page.len() >= 2 && page.iter().map(|u| u.bytes.len()).sum::<usize>() <= 12_000,
        "{:?}",
        page.iter().map(|u| u.bytes.len()).collect::<Vec<_>>()
    );
    assert!(replicate(&a, &b, 12_000) >= 2);
    assert_eq!(texts(&b).len(), 6);
    let big = add(&a, &noise("big", 20_000));
    let (page, _) = a
        .updates_for(&a.inventory().unwrap().0, &b.inventory().unwrap().0, 1000)
        .unwrap();
    assert_eq!(page.len(), 1);
    assert_eq!(page[0].document, Document::Task(big.uuid().to_owned()));
}

#[test]
fn a_document_that_cannot_be_imported_is_refused_alone() {
    let (here, there, other) = (
        tempfile::tempdir().unwrap(),
        tempfile::tempdir().unwrap(),
        tempfile::tempdir().unwrap(),
    );
    let (a, b) = (store(here.path()), store(there.path()));
    let good = add(&a, "Add rate limiting");
    let foreign = ProjectStore::open(other.path(), "project-2").unwrap();
    let stranger = add(&foreign, "Elsewhere");
    let updates = vec![
        Update {
            document: Document::Task(stranger.uuid().to_owned()),
            bytes: stranger.snapshot().unwrap(),
        },
        Update {
            document: Document::Task("not-a-uuid".into()),
            bytes: good.snapshot().unwrap(),
        },
        Update {
            document: Document::Task("8f14e45f-ceea-4e7a-9b1c-2f6d1a3b5c7d".into()),
            bytes: b"garbage".to_vec(),
        },
        Update {
            document: Document::Task("8f14e45f-ceea-4e7a-9b1c-2f6d1a3b5c7e".into()),
            bytes: good.snapshot().unwrap(),
        },
        Update {
            document: Document::Task(good.uuid().to_owned()),
            bytes: good.snapshot().unwrap(),
        },
    ];
    let imported = b.import_updates(updates).unwrap();
    assert_eq!(imported.changed, 1);
    let refused: Vec<&str> = imported.refused.iter().map(|d| d.error.as_str()).collect();
    assert_eq!(refused.len(), 4, "{refused:?}");
    assert!(refused[0].contains("another project"), "{}", refused[0]);
    assert!(refused[1].contains("not a task UUID"), "{}", refused[1]);
    assert!(refused[3].contains("another task"), "{}", refused[3]);
    assert_eq!(texts(&b), ["Add rate limiting"]);
    // A local task that fails to load is left out of the inventory, named, and
    // never stops the others.
    let broken = add(&b, "Broken here");
    fs::write(task_file(there.path(), broken.uuid()), b"damaged").unwrap();
    let (inventory, damaged) = b.inventory().unwrap();
    assert_eq!(damaged.len(), 1);
    assert!(!inventory.tasks.contains_key(broken.uuid()));
    add(&a, "Still replicated");
    replicate(&a, &b, 1 << 20);
    assert_eq!(texts(&b), ["Add rate limiting", "Still replicated"]);
}

#[test]
fn a_damaged_copy_is_set_aside_for_a_good_one() {
    let (here, there) = (tempfile::tempdir().unwrap(), tempfile::tempdir().unwrap());
    let (a, b) = (store(here.path()), store(there.path()));
    let task = add(&a, "Add rate limiting");
    replicate(&a, &b, 1 << 20);
    let file = task_file(there.path(), task.uuid());
    fs::write(&file, b"damaged").unwrap();
    assert_eq!(b.inventory().unwrap().1.len(), 1);
    // A member's whole copy replaces it; the damaged file stays on disk beside it.
    replicate(&a, &b, 1 << 20);
    assert_eq!(texts(&b), ["Add rate limiting"]);
    assert!(b.inventory().unwrap().1.is_empty());
    assert_eq!(
        a.inventory().unwrap().0.digest(),
        b.inventory().unwrap().0.digest()
    );
    let kept: Vec<_> = fs::read_dir(file.parent().unwrap())
        .unwrap()
        .map(|entry| entry.unwrap().path())
        .filter(|path| path.to_string_lossy().contains(".damaged-"))
        .collect();
    assert_eq!(kept.len(), 1, "{kept:?}");
    assert_eq!(fs::read(&kept[0]).unwrap(), b"damaged");
}

/// Dates a file a minute back, as files written before the last inventory are.
fn settle(path: &Path) {
    fs::File::options()
        .write(true)
        .open(path)
        .unwrap()
        .set_modified(SystemTime::now() - Duration::from_secs(60))
        .unwrap();
}

fn cache_file(root: &Path) -> PathBuf {
    root.join(format!("{:x}", Sha256::digest(b"project-1")))
        .join("inventory.json")
}

/// Changes one task's remembered entry in the inventory cache.
fn remember(root: &Path, uuid: &str, change: impl FnOnce(&mut serde_json::Value)) {
    let mut cache: serde_json::Value =
        serde_json::from_slice(&fs::read(cache_file(root)).unwrap()).unwrap();
    change(&mut cache["tasks"][uuid]);
    fs::write(cache_file(root), serde_json::to_vec(&cache).unwrap()).unwrap();
}

#[test]
fn a_task_that_does_not_load_is_read_again_only_when_its_files_change() {
    let root = tempfile::tempdir().unwrap();
    let a = store(root.path());
    let broken = add(&a, "Broken here");
    let file = task_file(root.path(), broken.uuid());
    fs::write(&file, b"damaged").unwrap();
    settle(&file);
    assert_eq!(a.inventory().unwrap().1.len(), 1);
    // The failure is remembered with the file's stamps and named again as it was.
    remember(root.path(), broken.uuid(), |entry| {
        entry["error"] = "remembered".into()
    });
    assert_eq!(a.inventory().unwrap().1[0].error, "remembered");
}

#[test]
#[cfg(unix)]
fn a_file_written_moments_ago_is_always_read() {
    use std::os::unix::fs::MetadataExt;
    // A rewrite within one timestamp tick can hand a file its old inode back,
    // so a stamp that matches is trusted only once the file is older than that.
    let root = tempfile::tempdir().unwrap();
    let a = store(root.path());
    let task = add(&a, "Add rate limiting");
    let file = task_file(root.path(), task.uuid());
    settle(&file);
    let (first, _) = a.inventory().unwrap();
    fs::File::options()
        .write(true)
        .open(&file)
        .unwrap()
        .set_modified(SystemTime::now())
        .unwrap();
    let meta = fs::metadata(&file).unwrap();
    let modified = meta
        .modified()
        .unwrap()
        .duration_since(SystemTime::UNIX_EPOCH)
        .unwrap();
    remember(root.path(), task.uuid(), |entry| {
        entry["files"][0] = serde_json::json!([
            meta.len(),
            modified.as_secs(),
            modified.subsec_nanos(),
            meta.ino()
        ]);
        entry["version"] = serde_json::json!({"7": 1});
    });
    assert_eq!(a.inventory().unwrap().0, first);
}

#[test]
fn a_held_project_is_skipped_after_a_short_wait() {
    let root = tempfile::tempdir().unwrap();
    let held = store(root.path());
    let started = Instant::now();
    assert!(
        ProjectStore::open_within(root.path(), "project-1", Duration::from_millis(200))
            .unwrap()
            .is_none()
    );
    assert!(started.elapsed() < Duration::from_secs(2));
    drop(held);
    assert!(
        ProjectStore::open_within(root.path(), "project-1", Duration::from_millis(200))
            .unwrap()
            .is_some()
    );
}

#[test]
fn an_inventory_that_claims_too_much_is_refused_and_then_healed() {
    // A replica that names changes it lacks gets updates it cannot apply; they
    // are refused, and the next exchange with its true inventory heals it.
    let (here, there) = (tempfile::tempdir().unwrap(), tempfile::tempdir().unwrap());
    let (a, b) = (store(here.path()), store(there.path()));
    let task = add(&a, "Add rate limiting");
    replicate(&a, &b, 1 << 20);
    let mut changed = a.load(task.uuid()).unwrap();
    changed.comment("First", &Actor::Person, 2).unwrap();
    a.save(&mut changed).unwrap();
    let after_first = a.inventory().unwrap().0;
    changed.comment("Second", &Actor::Person, 3).unwrap();
    a.save(&mut changed).unwrap();
    let (updates, _) = a
        .updates_for(&a.inventory().unwrap().0, &after_first, 1 << 20)
        .unwrap();
    let imported = b.import_updates(updates).unwrap();
    assert_eq!(imported.refused.len(), 1);
    assert!(
        imported.refused[0].error.contains("could not be applied"),
        "{}",
        imported.refused[0].error
    );
    replicate(&a, &b, 1 << 20);
    assert_eq!(a.inventory().unwrap().0, b.inventory().unwrap().0);
}

fn task_file(root: &Path, uuid: &str) -> std::path::PathBuf {
    root.join(format!("{:x}", Sha256::digest(b"project-1")))
        .join("tasks")
        .join(format!("{uuid}.loro"))
}

#[test]
fn the_inventory_reads_a_task_again_only_when_its_files_change() {
    let root = tempfile::tempdir().unwrap();
    let a = store(root.path());
    let task = add(&a, "Add rate limiting");
    settle(&task_file(root.path(), task.uuid()));
    let (first, _) = a.inventory().unwrap();
    let cache = cache_file(root.path());
    // While the task's files stay as they were, the cached version is used as is.
    let text = fs::read_to_string(&cache).unwrap();
    let peer = first.tasks[task.uuid()].keys().next().unwrap().clone();
    fs::write(&cache, text.replace(&format!("\"{peer}\""), "\"7\"")).unwrap();
    assert!(a.inventory().unwrap().0.tasks[task.uuid()].contains_key("7"));
    // A change to them reads the task again.
    let mut task = a.load(task.uuid()).unwrap();
    task.comment("Later", &Actor::Person, 2).unwrap();
    a.save(&mut task).unwrap();
    let (later, _) = a.inventory().unwrap();
    assert!(later.tasks[task.uuid()].contains_key(&peer));
    assert_ne!(later, first);
}

#[test]
fn stored_projects_are_listed_by_id() {
    let root = tempfile::tempdir().unwrap();
    assert!(ProjectStore::stored(root.path()).unwrap().is_empty());
    drop(store(root.path()));
    drop(ProjectStore::open(root.path(), "team/web").unwrap());
    assert_eq!(
        ProjectStore::stored(root.path()).unwrap(),
        ["project-1", "team/web"]
    );
    assert!(ProjectStore::stored(&root.path().join("missing"))
        .unwrap()
        .is_empty());
    let _ = Inventory::default();
}

/// Sends, reports and accepts `task`: it ends in Done.
fn accept(store: &ProjectStore, task: &mut TaskDoc) {
    let session = zerus_replica::SessionKey {
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

/// Every file of the project, old enough for the cache to trust.
fn settle_all(root: &Path) {
    fn walk(folder: &Path) {
        for entry in fs::read_dir(folder).unwrap() {
            let path = entry.unwrap().path();
            if path.is_dir() {
                walk(&path);
            } else if path.file_name().unwrap() != "inventory.json" {
                settle(&path);
            }
        }
    }
    walk(&root.join(format!("{:x}", Sha256::digest(b"project-1"))));
}

#[test]
fn the_live_inventory_leaves_done_tasks_out() {
    let root = tempfile::tempdir().unwrap();
    let a = store(root.path());
    let live = add(&a, "Add rate limiting");
    let mut done = add(&a, "Measure first");
    accept(&a, &mut done);
    let (inventory, _) = a.live_inventory().unwrap();
    assert_eq!(inventory.tasks.keys().collect::<Vec<_>>(), [live.uuid()]);
    assert_eq!(a.inventory().unwrap().0.tasks.len(), 2);
}

#[test]
fn the_phone_view_keeps_what_the_phone_holds() {
    let (a_root, b_root) = (tempfile::tempdir().unwrap(), tempfile::tempdir().unwrap());
    let (a, phone) = (store(a_root.path()), store(b_root.path()));
    let live = add(&a, "Add rate limiting");
    let mut done = add(&a, "Measure first");
    replicate(&a, &phone, 1 << 20);
    accept(&a, &mut done);
    let (theirs, _) = phone.inventory().unwrap();
    let view = a.phone_inventory(&theirs).unwrap();
    let mut held: Vec<_> = view.tasks.keys().map(String::as_str).collect();
    held.sort();
    let mut expected = vec![live.uuid(), done.uuid()];
    expected.sort();
    assert_eq!(held, expected);
    // The view carries this store's newer version of the Done task.
    assert_eq!(
        view.tasks[done.uuid()],
        a.inventory().unwrap().0.tasks[done.uuid()]
    );
    // A phone that never held it is not offered the Done task.
    assert!(!a
        .phone_inventory(&Inventory::default())
        .unwrap()
        .tasks
        .contains_key(done.uuid()));
}

#[test]
fn the_cached_stage_follows_a_change() {
    let root = tempfile::tempdir().unwrap();
    let a = store(root.path());
    let mut task = add(&a, "Add rate limiting");
    settle_all(root.path());
    assert!(a
        .live_inventory()
        .unwrap()
        .0
        .tasks
        .contains_key(task.uuid()));
    accept(&a, &mut task);
    settle_all(root.path());
    assert!(!a
        .live_inventory()
        .unwrap()
        .0
        .tasks
        .contains_key(task.uuid()));
    // A remembered entry without its stage, as an older cache has, is read again.
    remember(root.path(), task.uuid(), |entry| {
        entry.as_object_mut().unwrap().remove("stage");
    });
    assert!(!a
        .live_inventory()
        .unwrap()
        .0
        .tasks
        .contains_key(task.uuid()));
}

#[test]
fn the_view_trusts_the_remembered_stage_of_unchanged_files() {
    let root = tempfile::tempdir().unwrap();
    let a = store(root.path());
    let task = add(&a, "Add rate limiting");
    settle_all(root.path());
    a.live_inventory().unwrap();
    // Unchanged files: the remembered stage answers, the document is not read.
    remember(root.path(), task.uuid(), |entry| {
        entry["stage"] = "done".into()
    });
    assert!(!a
        .live_inventory()
        .unwrap()
        .0
        .tasks
        .contains_key(task.uuid()));
}
