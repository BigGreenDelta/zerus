//! What became of the session a launch ID (`--launch-id`) started: the identity
//! the desktop's launch placeholder waits for. A placeholder ends when a
//! listing shows a session or record that carries its ID. A session that ends
//! before any listing showed it and leaves no record (a program that could not
//! start, a supervisor that failed after `hgs: started`) would leave it waiting
//! for ever, so a launch keeps evidence in `launches/` of the state directory:
//!
//! - `ID.json`, written by a detached launch once its terminal has created the
//!   session (`started`), holds the session's name;
//! - `ID.end.json`, written by the session's `--run` supervisor when it ends
//!   within `STARTUP` without a confirmed conversation (`finished`), holds its
//!   exit code and the last lines of its screen, after them the error that
//!   stopped the supervisor.
//!
//! A supervisor whose run confirmed a conversation removes `ID.json` instead:
//! from then on its record carries the launch ID. `hgs ls --json --local` reads
//! the evidence (`Evidence::read`) before it lists the terminal's sessions and
//! reports as `ended_launches` (`Evidence::ended`) each launch that no session
//! of the listing carries: its session existed before the listing began and is
//! gone. Evidence is listed and kept for `KEPT`, as long as the desktop keeps a
//! placeholder.
use super::*;
use crate::host::last_lines;
use std::collections::BTreeSet;
use std::fs;
use std::time::{Duration, SystemTime};

/// How long evidence is listed and kept: the desktop drops a launch placeholder
/// after a week.
const KEPT: f64 = 7.0 * 24.0 * 60.0 * 60.0;
/// The most ended launches a listing reports, newest first.
const LISTED: usize = 32;
/// How long after it started a supervisor that ends without a confirmed
/// conversation reports how: the desktop's own startup time, after which its
/// placeholder no longer says "Starting…". What the screen of a run that
/// lasted longer showed last is not why it could not start.
const STARTUP: Duration = Duration::from_secs(120);
/// What an ended launch keeps of its screen: its last lines.
const LINES: usize = 8;
const BYTES: usize = 2048;
/// An agent's last output reaches the terminal's screen a moment after the
/// agent has exited (on Windows through its pseudoconsole): the screen is read
/// after this pause and again until it stays the same.
const SETTLE: Duration = Duration::from_millis(100);
const SETTLE_READS: usize = 5;

fn folder() -> PathBuf {
    root().join("launches")
}

/// A launch ID as `--launch-id` takes it: a UUID in its canonical form, which
/// is also a safe file name.
fn valid(id: &str) -> bool {
    uuid::Uuid::parse_str(id).is_ok_and(|uuid| !uuid.is_nil() && uuid.to_string() == id)
}

fn started_path(folder: &Path, id: &str) -> PathBuf {
    folder.join(format!("{id}.json"))
}

fn ended_path(folder: &Path, id: &str) -> PathBuf {
    folder.join(format!("{id}.end.json"))
}

/// Record that a detached launch for `id` created the session `name`; first
/// remove evidence older than `KEPT`.
pub(crate) fn started(id: &str, name: &str) -> Result<()> {
    local_sessions()?;
    if !valid(id) {
        return Err(format!("invalid launch ID: {id}"));
    }
    let folder = folder();
    prune(&folder, SystemTime::now());
    write_started(&folder, id, name, now())
}

fn write_started(folder: &Path, id: &str, name: &str, at: f64) -> Result<()> {
    atomic(
        &started_path(folder, id),
        &format!(
            "{}\n",
            json!({"launch_id": id, "name": name, "started_at": at})
        ),
    )
}

/// Remove the files in `folder` last written more than `KEPT` before `now`.
fn prune(folder: &Path, now: SystemTime) {
    let Ok(entries) = fs::read_dir(folder) else {
        return;
    };
    for entry in entries.flatten() {
        let old = entry
            .metadata()
            .and_then(|metadata| metadata.modified())
            .ok()
            .and_then(|modified| now.duration_since(modified).ok())
            .is_some_and(|age| age.as_secs_f64() > KEPT);
        if old && entry.file_name().to_string_lossy().ends_with(".json") {
            let _ = fs::remove_file(entry.path());
        }
    }
}

/// Called as the `--run` supervisor of a session ends, `elapsed` after it
/// started, with its `outcome`: the exit code, or that and the message of the
/// error that stopped it. Only a session started for a launch ID
/// (`HGS_LAUNCH_ID`) leaves evidence, and nothing here fails the supervisor.
pub(crate) fn finished(elapsed: Duration, outcome: std::result::Result<i32, (i32, &str)>) {
    let Some(id) = nonempty_env("HGS_LAUNCH_ID").filter(|id| valid(id)) else {
        return;
    };
    let folder = folder();
    if confirmed() {
        let _ = fs::remove_file(started_path(&folder, &id));
        return;
    }
    if elapsed > STARTUP {
        return;
    }
    let (code, error) = match outcome {
        Ok(code) => (code, None),
        Err((code, message)) => (code, Some(message)),
    };
    let _ = write_ended(
        &folder,
        &id,
        nonempty_env("HGS_SESSION").as_deref(),
        code,
        &ending(&screen(), error),
        now(),
    );
}

fn write_ended(
    folder: &Path,
    id: &str,
    name: Option<&str>,
    code: i32,
    output: &str,
    at: f64,
) -> Result<()> {
    atomic(
        &ended_path(folder, id),
        &format!(
            "{}\n",
            json!({"launch_id": id, "name": name, "code": code, "output": output, "ended_at": at})
        ),
    )
}

/// Whether the run of this supervisor (`HGS_SESSION`, `HGS_RUN_ID`, which the
/// tracked launch sets once its record exists) confirmed a conversation.
fn confirmed() -> bool {
    let (Some(name), Some(run)) = (nonempty_env("HGS_SESSION"), nonempty_env("HGS_RUN_ID")) else {
        return false;
    };
    let Ok(_guard) = lock(None) else {
        return false;
    };
    read_run(&name, &run)
        .ok()
        .flatten()
        .is_some_and(|record| !string(&record, "conversation_id").is_empty())
}

/// This supervisor's pane as it shows once it has settled, "" when the
/// terminal cannot tell.
fn screen() -> String {
    let terminal = backend::current();
    let Ok(pane) = terminal.own_pane() else {
        return String::new();
    };
    let mut last: Option<Vec<u8>> = None;
    for _ in 0..SETTLE_READS {
        std::thread::sleep(SETTLE);
        let Ok(screen) = terminal.capture(&pane, backend::Capture::Plain) else {
            break;
        };
        if last.as_ref() == Some(&screen) {
            break;
        }
        last = Some(screen);
    }
    last.map(|screen| String::from_utf8_lossy(&screen).into_owned())
        .unwrap_or_default()
}

/// What an ended launch reports of `screen` and the supervisor's `error`, as
/// the supervisor prints it after the screen: their last lines.
fn ending(screen: &str, error: Option<&str>) -> String {
    let mut text = screen.to_owned();
    if let Some(error) = error {
        text.push_str("\nhgs: ");
        text.push_str(error);
    }
    last_lines(&text, LINES, BYTES)
}

#[derive(Default)]
struct Launch {
    name: String,
    /// When the newest of its evidence was written.
    at: f64,
    /// Whether its supervisor reported how it ended.
    reported: bool,
    code: Option<i64>,
    output: String,
}

/// The launch evidence a listing read before it listed the terminal.
pub(crate) struct Evidence {
    folder: PathBuf,
    now: f64,
    launches: BTreeMap<String, Launch>,
}

impl Evidence {
    pub(crate) fn read() -> Self {
        Self::read_in(&folder(), now())
    }

    fn read_in(folder: &Path, now: f64) -> Self {
        let mut launches = BTreeMap::new();
        let evidence = |launches| Self {
            folder: folder.to_owned(),
            now,
            launches,
        };
        let Ok(entries) = fs::read_dir(folder) else {
            return evidence(launches);
        };
        for entry in entries.flatten() {
            let file = entry.file_name().to_string_lossy().into_owned();
            let (id, end) = match file.strip_suffix(".end.json") {
                Some(id) => (id, true),
                None => match file.strip_suffix(".json") {
                    Some(id) => (id, false),
                    None => continue,
                },
            };
            let Some(value) = fs::read_to_string(entry.path())
                .ok()
                .and_then(|text| serde_json::from_str::<Value>(&text).ok())
                .filter(|value| valid(id) && string(value, "launch_id") == id)
            else {
                continue;
            };
            let at = value[if end { "ended_at" } else { "started_at" }]
                .as_f64()
                .unwrap_or(0.0);
            if now - at > KEPT {
                continue;
            }
            let launch: &mut Launch = launches.entry(id.to_owned()).or_default();
            // The name the launch created, else the supervisor's.
            if launch.name.is_empty() || !end {
                let name = string(&value, "name");
                if !name.is_empty() {
                    launch.name = name.to_owned();
                }
            }
            launch.at = launch.at.max(at);
            if end {
                launch.reported = true;
                launch.code = value["code"].as_i64();
                launch.output = string(&value, "output").to_owned();
            }
        }
        evidence(launches)
    }

    /// `ended_launches` of a listing of `sessions`: the launches that none of
    /// them carries, newest first, with the session's name, exit code (null
    /// when unknown) and last lines.
    pub(crate) fn ended(self, sessions: &[Value]) -> Vec<Value> {
        let listed: BTreeSet<&str> = sessions
            .iter()
            .map(|session| string(session, "launch_id"))
            .filter(|id| !id.is_empty())
            .collect();
        let mut ended: Vec<(String, Launch)> = self
            .launches
            .into_iter()
            .filter(|(id, _)| !listed.contains(id.as_str()))
            .collect();
        // A supervisor reports how its session ended before the session leaves
        // the terminal: a launch that ended after this evidence was read and
        // before the terminal listed has its report now.
        if ended.iter().any(|(_, launch)| !launch.reported) {
            let mut later = Self::read_in(&self.folder, self.now).launches;
            for (id, launch) in &mut ended {
                if let Some(report) = later.remove(id.as_str()).filter(|_| !launch.reported) {
                    *launch = report;
                }
            }
        }
        ended.sort_by(|a, b| b.1.at.total_cmp(&a.1.at).then_with(|| a.0.cmp(&b.0)));
        ended.truncate(LISTED);
        ended
            .into_iter()
            .map(|(id, launch)| {
                json!({"launch_id": id, "name": launch.name, "code": launch.code, "output": launch.output})
            })
            .collect()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const ONE: &str = "0b6f4c9e-3c1d-4b8f-9a35-6f2e2d6f6a10";
    const TWO: &str = "4400baad-7d90-480a-8451-5f3e456e2ae9";
    const THREE: &str = "92812fe9-eed8-477c-921a-ef3949434722";

    #[test]
    fn launch_ids_are_canonical_uuids() {
        assert!(valid(ONE));
        for id in [
            "",
            "00000000-0000-0000-0000-000000000000",
            "0B6F4C9E-3C1D-4B8F-9A35-6F2E2D6F6A10",
            "0b6f4c9e3c1d4b8f9a356f2e2d6f6a10",
            "../0b6f4c9e-3c1d-4b8f-9a35-6f2e2d6f6a10",
            "hgs-start-0b6f4c9e-3c1d-4b8f-9a35-6f2e2d6f6a10",
        ] {
            assert!(!valid(id), "{id}");
        }
    }

    #[test]
    fn an_ending_keeps_the_last_lines_and_the_supervisor_error() {
        let screen = "first\n\nsecond   \nthird\n\n\n";
        assert_eq!(ending(screen, None), "first\nsecond\nthird");
        assert_eq!(
            ending(screen, Some("claude: Access is denied. (os error 5)")),
            "first\nsecond\nthird\nhgs: claude: Access is denied. (os error 5)"
        );
        assert_eq!(ending("", Some("cannot start")), "hgs: cannot start");
        let many: Vec<String> = (1..=20).map(|line| format!("line {line}")).collect();
        assert_eq!(ending(&many.join("\n"), None), many[12..].join("\n"));
    }

    #[test]
    fn a_listing_reports_the_launches_that_none_of_its_sessions_carries() {
        let folder = tempfile::tempdir().unwrap();
        let folder = folder.path();
        let now = 1_800_000_000.0;
        // Started, then its supervisor reported how it ended.
        write_started(folder, ONE, "claude/project/one", now - 30.0).unwrap();
        write_ended(
            folder,
            ONE,
            Some("claude/project/one"),
            3,
            "cannot start",
            now - 20.0,
        )
        .unwrap();
        // Started and gone, without a supervisor's word.
        write_started(folder, TWO, "sh/project/two", now - 10.0).unwrap();
        // Ended in a terminal of its own: only its supervisor wrote.
        write_ended(folder, THREE, None, 1, "", now - 5.0).unwrap();
        let ended = |sessions: &[Value]| Evidence::read_in(folder, now).ended(sessions);
        assert_eq!(
            ended(&[]),
            vec![
                json!({"launch_id": THREE, "name": "", "code": 1, "output": ""}),
                json!({"launch_id": TWO, "name": "sh/project/two", "code": null, "output": ""}),
                json!({"launch_id": ONE, "name": "claude/project/one", "code": 3,
                       "output": "cannot start"}),
            ]
        );
        // A listed session or record with the ID, in any state, is not ended.
        let sessions = [
            json!({"name": "sh/project/two", "launch_id": TWO}),
            json!({"name": "claude/project/x", "state": "archived", "launch_id": THREE}),
            json!({"name": "claude/project/y", "launch_id": ""}),
            json!({"name": "claude/project/z"}),
        ];
        assert_eq!(
            ended(&sessions),
            vec![
                json!({"launch_id": ONE, "name": "claude/project/one", "code": 3,
                        "output": "cannot start"})
            ]
        );
        // Evidence older than a week, of an invalid or other ID, or unreadable,
        // is not listed.
        write_started(folder, ONE, "claude/project/one", now - KEPT - 60.0).unwrap();
        write_ended(folder, ONE, None, 3, "", now - KEPT - 1.0).unwrap();
        fs::write(folder.join("not-a-launch.json"), "{}").unwrap();
        fs::write(
            started_path(folder, "00000000-0000-0000-0000-000000000000"),
            json!({"launch_id": "00000000-0000-0000-0000-000000000000", "started_at": now})
                .to_string(),
        )
        .unwrap();
        fs::write(
            started_path(folder, ONE),
            json!({"launch_id": TWO, "started_at": now}).to_string(),
        )
        .unwrap();
        fs::write(ended_path(folder, TWO), "{").unwrap();
        assert_eq!(ended(&sessions), Vec::<Value>::new());
        assert_eq!(
            Evidence::read_in(&folder.join("missing"), now).ended(&[]),
            Vec::<Value>::new()
        );
    }

    #[test]
    fn a_listing_reports_how_a_launch_ended_after_its_evidence_was_read() {
        let folder = tempfile::tempdir().unwrap();
        let now = 1_800_000_000.0;
        write_started(folder.path(), ONE, "claude/project/one", now - 3.0).unwrap();
        let evidence = Evidence::read_in(folder.path(), now);
        // Its supervisor reported and exited before the terminal listed.
        write_ended(
            folder.path(),
            ONE,
            Some("claude/project/one"),
            3,
            "cannot start",
            now - 1.0,
        )
        .unwrap();
        assert_eq!(
            evidence.ended(&[]),
            vec![
                json!({"launch_id": ONE, "name": "claude/project/one", "code": 3,
                        "output": "cannot start"})
            ]
        );
    }

    #[test]
    fn a_listing_reports_the_newest_launches_only() {
        let folder = tempfile::tempdir().unwrap();
        let now = 1_800_000_000.0;
        let ids: Vec<String> = (0..LISTED + 3)
            .map(|index| format!("0b6f4c9e-3c1d-4b8f-9a35-{index:012x}"))
            .collect();
        for (index, id) in ids.iter().enumerate() {
            write_started(
                folder.path(),
                id,
                "sh/project/x",
                now - 100.0 + index as f64,
            )
            .unwrap();
        }
        let ended = Evidence::read_in(folder.path(), now).ended(&[]);
        let listed: Vec<&str> = ended
            .iter()
            .map(|launch| string(launch, "launch_id"))
            .collect();
        let newest: Vec<&str> = ids.iter().rev().take(LISTED).map(String::as_str).collect();
        assert_eq!(listed, newest);
    }

    #[test]
    fn pruning_removes_only_evidence_older_than_a_week() {
        let folder = tempfile::tempdir().unwrap();
        write_started(folder.path(), ONE, "sh/project/one", 0.0).unwrap();
        write_ended(folder.path(), ONE, None, 1, "", 0.0).unwrap();
        fs::write(folder.path().join("notes.txt"), "kept").unwrap();
        let week = Duration::from_secs_f64(KEPT);
        prune(
            folder.path(),
            SystemTime::now() + week - Duration::from_secs(60),
        );
        assert!(started_path(folder.path(), ONE).exists());
        prune(
            folder.path(),
            SystemTime::now() + week + Duration::from_secs(60),
        );
        assert!(!started_path(folder.path(), ONE).exists());
        assert!(!ended_path(folder.path(), ONE).exists());
        assert!(folder.path().join("notes.txt").exists());
        prune(&folder.path().join("missing"), SystemTime::now());
    }
}
