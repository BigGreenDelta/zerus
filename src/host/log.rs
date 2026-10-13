//! The session host's log, `host.log` in the state directory. The host has no
//! console anyone sees, so starts, refusals, session ends and exits are noted
//! there for diagnosis. Nothing is noted before `open`, as in tests.
use super::session::lock;
use std::{
    fs::{self, File, OpenOptions},
    io::{self, Write},
    path::Path,
    sync::Mutex,
    time::{SystemTime, UNIX_EPOCH},
};

pub(crate) const LOG_FILE: &str = "host.log";
/// A longer log is started afresh when a host starts.
const LIMIT: u64 = 1 << 20;

static LOG: Mutex<Option<File>> = Mutex::new(None);

/// Note from now on into `host.log` in `state_dir`.
pub(crate) fn open(state_dir: &Path) -> io::Result<()> {
    let path = state_dir.join(LOG_FILE);
    let long = fs::metadata(&path).is_ok_and(|metadata| metadata.len() > LIMIT);
    let mut options = OpenOptions::new();
    options.create(true);
    if long {
        options.write(true).truncate(true);
    } else {
        options.append(true);
    }
    let file = crate::platform::private_open(&mut options).open(path)?;
    *lock(&LOG) = Some(file);
    Ok(())
}

/// Append one line: Unix seconds, this process's ID and `message`.
pub(crate) fn note(message: impl AsRef<str>) {
    let mut log = lock(&LOG);
    if let Some(file) = log.as_mut() {
        let seconds = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .map_or(0, |time| time.as_secs());
        let _ = writeln!(
            file,
            "{seconds} {} {}",
            std::process::id(),
            message.as_ref()
        );
    }
}
