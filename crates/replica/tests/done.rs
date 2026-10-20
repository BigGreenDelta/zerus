//! The Done list a computer serves the phone: newest first, in pages, from the
//! cached stages, loading only the page's documents.
use sha2::{Digest, Sha256};
use std::collections::BTreeSet;
use std::fs;
use std::path::{Path, PathBuf};
use std::time::{Duration, SystemTime};
use zerus_replica::{Actor, NewTask, ProjectStore, SessionKey, Stage, Target, TaskDoc};

fn store(root: &Path) -> ProjectStore {
    ProjectStore::open(root, "project-1").unwrap()
}

/// A task filed, sent, reported and accepted into Done at `now`.
fn done_at(store: &ProjectStore, text: &str, now: i64) -> String {
    done_in(store, text, now, "codex/api")
}

/// A task sent to `session` on computer `m` and accepted into Done at `now`.
fn done_in(store: &ProjectStore, text: &str, now: i64, session: &str) -> String {
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
    let session = SessionKey {
        machine: "m".into(),
        session: session.into(),
    };
    task.record_send(&session, "request-1", &Actor::Person, now - 2)
        .unwrap();
    task.record_report("Done.", &Actor::Agent(session), now - 1)
        .unwrap();
    task.set_stage(Stage::Done, &Actor::Person, now).unwrap();
    store.save(&mut task).unwrap();
    task.uuid().to_owned()
}

fn project_dir(root: &Path) -> PathBuf {
    root.join(format!("{:x}", Sha256::digest(b"project-1")))
}

/// Every file of the project, old enough for the cache to trust.
fn settle_all(root: &Path) {
    fn walk(folder: &Path) {
        for entry in fs::read_dir(folder).unwrap() {
            let path = entry.unwrap().path();
            if path.is_dir() {
                walk(&path);
            } else if path.file_name().unwrap() != "inventory.json" {
                fs::File::options()
                    .write(true)
                    .open(&path)
                    .unwrap()
                    .set_modified(SystemTime::now() - Duration::from_secs(60))
                    .unwrap();
            }
        }
    }
    walk(&project_dir(root));
}

#[test]
fn done_pages_walk_every_task_once() {
    let root = tempfile::tempdir().unwrap();
    let store = store(root.path());
    for n in 0..120 {
        done_at(&store, &format!("Task {n}"), 10 + n);
    }
    // A live task never appears.
    let mut live = TaskDoc::create(NewTask {
        project: "project-1",
        id: &store.unused_short_id().unwrap(),
        text: "Still to do",
        stage: Stage::Todo,
        target: Target::Any,
        author: Actor::Person,
        rank: "V",
        now: 1,
    })
    .unwrap();
    store.save(&mut live).unwrap();
    let mut seen = BTreeSet::new();
    let mut times = Vec::new();
    let mut sizes = Vec::new();
    let mut before = None;
    loop {
        let page = store.done_page(before.clone(), 50, 100, None).unwrap();
        assert_eq!((page.total, page.recent), (120, 30));
        sizes.push(page.tasks.len());
        for row in &page.tasks {
            assert!(seen.insert(row.uuid.clone()), "{} twice", row.uuid);
            times.push(row.accepted_at);
            assert!(row.text.starts_with("Task "));
            assert_eq!(row.session.as_ref().unwrap().session, "codex/api");
        }
        match page.next {
            Some(next) => before = Some(next),
            None => break,
        }
    }
    assert_eq!(sizes, [50, 50, 20]);
    assert_eq!(seen.len(), 120);
    let mut newest_first = times.clone();
    newest_first.sort_unstable_by(|a, b| b.cmp(a));
    assert_eq!(times, newest_first);
}

#[test]
fn an_empty_page_ends_the_list() {
    let root = tempfile::tempdir().unwrap();
    let store = store(root.path());
    done_at(&store, "Task", 10);
    let page = store.done_page(None, 0, 0, None).unwrap();
    assert_eq!((page.tasks.len(), page.next, page.total), (0, None, 1));
}

#[test]
fn a_task_accepted_meanwhile_does_not_shift_the_pages() {
    let root = tempfile::tempdir().unwrap();
    let store = store(root.path());
    let uuids: Vec<_> = (0..5)
        .map(|n| done_at(&store, &format!("Task {n}"), 10 + n))
        .collect();
    let first = store.done_page(None, 2, 0, None).unwrap();
    assert_eq!(
        first
            .tasks
            .iter()
            .map(|r| r.uuid.clone())
            .collect::<Vec<_>>(),
        [uuids[4].clone(), uuids[3].clone()]
    );
    done_at(&store, "Accepted meanwhile", 99);
    let second = store.done_page(first.next, 2, 0, None).unwrap();
    assert_eq!(
        second
            .tasks
            .iter()
            .map(|r| r.uuid.clone())
            .collect::<Vec<_>>(),
        [uuids[2].clone(), uuids[1].clone()]
    );
    assert_eq!(second.total, 6);
}

#[test]
#[cfg(unix)]
fn the_page_loads_only_its_tasks() {
    use std::os::unix::fs::PermissionsExt;
    let root = tempfile::tempdir().unwrap();
    let store = store(root.path());
    let oldest = done_at(&store, "Oldest", 10);
    done_at(&store, "Middle", 20);
    done_at(&store, "Newest", 30);
    settle_all(root.path());
    store.done_page(None, 3, 0, None).unwrap();
    // The cache knows the oldest task's stage and time; its file can no longer be read.
    let file = project_dir(root.path())
        .join("tasks")
        .join(format!("{oldest}.loro"));
    fs::set_permissions(&file, fs::Permissions::from_mode(0o000)).unwrap();
    if fs::read(&file).is_ok() {
        return; // A superuser reads anything: nothing to show here.
    }
    let first = store.done_page(None, 2, 0, None).unwrap();
    assert_eq!(
        first
            .tasks
            .iter()
            .map(|r| r.text.as_str())
            .collect::<Vec<_>>(),
        ["Newest", "Middle"]
    );
    // The page that holds it leaves it out and still ends the list.
    let last = store.done_page(first.next, 2, 0, None).unwrap();
    assert!(last.tasks.is_empty());
    assert_eq!((last.next, last.total, last.damaged), (None, 3, 1));
    assert_eq!(first.damaged, 0);
    fs::set_permissions(&file, fs::Permissions::from_mode(0o600)).unwrap();
}

#[test]
#[cfg(unix)]
fn a_session_filter_counts_and_pages_only_that_sessions_tasks_from_the_cache() {
    use std::os::unix::fs::PermissionsExt;
    let root = tempfile::tempdir().unwrap();
    let store = store(root.path());
    let mut mine = Vec::new();
    for n in 0..5 {
        mine.push(done_in(
            &store,
            &format!("Mine {n}"),
            10 + 2 * n,
            "codex/api",
        ));
        done_in(&store, &format!("Theirs {n}"), 11 + 2 * n, "claude/web");
    }
    let api = SessionKey {
        machine: "m".into(),
        session: "codex/api".into(),
    };
    settle_all(root.path());
    store.done_page(None, 50, 0, None).unwrap();
    // Another session's task no longer reads; the filter needs only the cache.
    let theirs = store.done_page(None, 1, 0, None).unwrap().tasks[0]
        .uuid
        .clone();
    let file = project_dir(root.path())
        .join("tasks")
        .join(format!("{theirs}.loro"));
    fs::set_permissions(&file, fs::Permissions::from_mode(0o000)).unwrap();
    let unreadable = fs::read(&file).is_err();
    let first = store.done_page(None, 3, 14, Some(&api)).unwrap();
    assert_eq!((first.total, first.recent), (5, 3));
    assert_eq!(
        first
            .tasks
            .iter()
            .map(|r| r.uuid.clone())
            .collect::<Vec<_>>(),
        [mine[4].clone(), mine[3].clone(), mine[2].clone()]
    );
    let rest = store.done_page(first.next, 3, 14, Some(&api)).unwrap();
    assert_eq!(
        rest.tasks
            .iter()
            .map(|r| r.uuid.clone())
            .collect::<Vec<_>>(),
        [mine[1].clone(), mine[0].clone()]
    );
    assert_eq!(rest.next, None);
    if unreadable {
        assert_eq!((first.damaged, rest.damaged), (0, 0));
    }
    fs::set_permissions(&file, fs::Permissions::from_mode(0o600)).unwrap();
}

#[test]
fn a_cache_written_before_the_filter_is_read_again() {
    let root = tempfile::tempdir().unwrap();
    let store = store(root.path());
    done_in(&store, "Mine", 10, "codex/api");
    done_in(&store, "Theirs", 11, "claude/web");
    settle_all(root.path());
    store.done_page(None, 50, 0, None).unwrap();
    // An older hgs cached stages without sessions.
    let path = project_dir(root.path()).join("inventory.json");
    let mut cache: serde_json::Value = serde_json::from_slice(&fs::read(&path).unwrap()).unwrap();
    for entry in cache["tasks"].as_object_mut().unwrap().values_mut() {
        entry.as_object_mut().unwrap().remove("sent");
    }
    fs::write(&path, serde_json::to_vec(&cache).unwrap()).unwrap();
    let api = SessionKey {
        machine: "m".into(),
        session: "codex/api".into(),
    };
    let page = store.done_page(None, 50, 0, Some(&api)).unwrap();
    assert_eq!(page.total, 1);
    assert_eq!(page.tasks[0].text, "Mine");
}
