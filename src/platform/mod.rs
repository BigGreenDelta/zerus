//! Terminal, clipboard and graphical-session integration. External programs receive argv,
//! never interpolated shell commands (the SSH peer and tmux pane are explicit exceptions).
//!
//! Operating-system specific parts live in `unix` and `windows`. On Windows hgs is a
//! control console for remote machines: the local session host is compiled out.
use crate::config::Config;
use std::{
    env,
    ffi::{OsStr, OsString},
    fs::File,
    io::{self, IsTerminal, Read, Write},
    path::{Path, PathBuf},
    process::{Child, Command, Stdio},
};

#[cfg(unix)]
mod unix;
#[cfg(unix)]
pub use unix::*;
#[cfg(windows)]
mod windows;
#[cfg(windows)]
pub use windows::*;

/// Whether this build runs agent sessions on this machine. Windows builds manage
/// remote machines only; their local session commands exit with code 2.
pub const LOCAL_SESSIONS: bool = cfg!(unix);
pub const LOCAL_SESSIONS_UNSUPPORTED: &str =
    "Local sessions are not supported on Windows yet; choose a remote machine.";

pub fn capture(program: &str, args: &[&str]) -> Option<String> {
    let out = Command::new(program)
        .args(args)
        .stderr(Stdio::null())
        .output()
        .ok()?;
    if out.status.success() {
        Some(String::from_utf8_lossy(&out.stdout).into_owned())
    } else {
        None
    }
}
pub fn silent(program: &str, args: &[&str]) -> bool {
    Command::new(program)
        .args(args)
        .stdout(Stdio::null())
        .stderr(Stdio::null())
        .status()
        .map(|s| s.success())
        .unwrap_or(false)
}
/// The executable `program` names: itself when it is a path, otherwise the first
/// match on PATH (with each PATHEXT extension on Windows).
pub fn which(program: &str) -> Option<PathBuf> {
    which_in(program, &env::var_os("PATH").unwrap_or_default())
}
fn which_in(program: &str, path: &OsStr) -> Option<PathBuf> {
    if is_path(program) {
        return executable(program).then(|| PathBuf::from(program));
    }
    env::split_paths(path)
        .flat_map(|dir| candidates(&dir, program))
        .find(|path| executable(path))
}
pub fn available(program: &str) -> bool {
    which(program).is_some()
}

/// The OpenSSH client for every connection to a peer. `HGS_SSH` overrides it
/// (tests inject a fake client this way); see `default_ssh` for the default.
pub fn ssh_program() -> OsString {
    env::var_os("HGS_SSH")
        .filter(|value| !value.is_empty())
        .unwrap_or_else(default_ssh)
}
pub fn ssh() -> Command {
    Command::new(ssh_program())
}
/// An ssh client started with some standard streams piped to this process.
pub struct SshChild {
    pub child: Child,
    pub stdin: Option<Box<dyn Write + Send>>,
    pub stdout: Option<Box<dyn Read + Send>>,
    pub stderr: Option<Box<dyn Read + Send>>,
}
/// The output of a successful ssh command, without its error text.
pub fn ssh_capture(args: &[String]) -> Option<String> {
    let out = ssh_output(ssh().args(args).stderr(Stdio::null())).ok()?;
    out.status
        .success()
        .then(|| String::from_utf8_lossy(&out.stdout).into_owned())
}

/// Replace `path` with a fully written temporary file. Windows refuses the rename
/// while a scanner or reader briefly holds the destination; retry that for a
/// bounded time. Unix renames at once.
pub fn persist(
    mut temporary: tempfile::NamedTempFile,
    path: impl AsRef<Path>,
) -> Result<File, tempfile::PersistError> {
    let path = path.as_ref();
    let mut delay = std::time::Duration::from_millis(10);
    let deadline = std::time::Instant::now() + std::time::Duration::from_secs(1);
    loop {
        match temporary.persist(path) {
            Ok(file) => return Ok(file),
            Err(error)
                if transient_replace_error(&error.error)
                    && std::time::Instant::now() < deadline =>
            {
                temporary = error.file;
                std::thread::sleep(delay);
                delay = (delay * 2).min(std::time::Duration::from_millis(200));
            }
            Err(error) => return Err(error),
        }
    }
}

/// An fs2 `try_lock_*` failure that only means another process holds the lock.
/// Windows reports ERROR_LOCK_VIOLATION, which is not `ErrorKind::WouldBlock`.
pub fn lock_contended(error: &io::Error) -> bool {
    error.kind() == io::ErrorKind::WouldBlock
        || error.raw_os_error() == fs2::lock_contended_error().raw_os_error()
}

/// An fs2 lock this process holds on a file, released explicitly when dropped.
/// Windows frees a closed handle's lock only when system resources allow, and a
/// forked child can keep a Unix descriptor open until exec, so closing the file
/// alone can leave the next process waiting.
pub struct FileLock(File);

impl FileLock {
    pub fn exclusive(file: File) -> io::Result<Self> {
        fs2::FileExt::lock_exclusive(&file)?;
        Ok(Self(file))
    }

    /// Takes ownership of a file whose lock this process has just acquired.
    pub fn held(file: File) -> Self {
        Self(file)
    }
}

impl Drop for FileLock {
    fn drop(&mut self) {
        // Rust 1.89+ File::unlock would shadow the trait method; name fs2 explicitly.
        let _ = fs2::FileExt::unlock(&self.0);
    }
}

pub fn quote(word: &str) -> String {
    if !word.is_empty()
        && word
            .bytes()
            .all(|c| c.is_ascii_alphanumeric() || b"_./:=@%+,~-".contains(&c))
    {
        word.into()
    } else {
        format!("'{}'", word.replace('\'', "'\\''"))
    }
}
pub fn join<S: AsRef<str>>(args: &[S]) -> String {
    args.iter()
        .map(|s| quote(s.as_ref()))
        .collect::<Vec<_>>()
        .join(" ")
}

fn konsole_args(method: &str, args: &[&str]) -> Option<Vec<String>> {
    let service = env::var("KONSOLE_DBUS_SERVICE")
        .ok()
        .filter(|s| !s.is_empty())?;
    let session = env::var("KONSOLE_DBUS_SESSION")
        .ok()
        .filter(|s| !s.is_empty())?;
    if !available("busctl") {
        return None;
    }
    let mut result = vec![
        "--user".into(),
        "call".into(),
        service,
        session,
        "org.kde.konsole.Session".into(),
        method.into(),
    ];
    result.extend(args.iter().map(|s| s.to_string()));
    Some(result)
}
fn konsole(method: &str, args: &[&str]) {
    if let Some(args) = konsole_args(method, args) {
        let _ = Command::new("busctl")
            .args(args)
            .stdout(Stdio::null())
            .stderr(Stdio::null())
            .status();
    }
}
fn konsole_get(mode: &str) -> String {
    let Some(args) = konsole_args("tabTitleFormat", &["i", mode]) else {
        return String::new();
    };
    Command::new("busctl")
        .args(args)
        .stderr(Stdio::null())
        .output()
        .ok()
        .and_then(|out| {
            let text = String::from_utf8_lossy(&out.stdout);
            text.trim()
                .strip_prefix("s \"")
                .and_then(|s| s.strip_suffix('"'))
                .map(String::from)
        })
        .unwrap_or_default()
}
/// POSIX cksum, retained so existing host colours do not change on migration.
fn cksum(bytes: &[u8]) -> u32 {
    let feed = |mut crc: u32, byte: u8| {
        crc ^= u32::from(byte) << 24;
        for _ in 0..8 {
            crc = if crc & 0x8000_0000 != 0 {
                (crc << 1) ^ 0x04c1_1db7
            } else {
                crc << 1
            };
        }
        crc
    };
    let mut crc = bytes.iter().fold(0, |crc, &byte| feed(crc, byte));
    let mut len = bytes.len();
    while len != 0 {
        crc = feed(crc, len as u8);
        len >>= 8;
    }
    !crc
}
pub fn tab_color(config: &Config, host: &str) -> String {
    for pair in config.tab_colors.split_whitespace() {
        if let Some((key, value)) = pair.split_once('=') {
            if key == host {
                return value.into();
            }
        }
    }
    if host == "local" {
        return String::new();
    }
    [
        "#f5a623", "#7aa2f7", "#00c9a7", "#e06c9f", "#bb9af7", "#9ece6a",
    ][(cksum(host.as_bytes()) % 6) as usize]
        .into()
}

pub struct TabGuard {
    active: bool,
    konsole: bool,
    formats: [String; 2],
}
impl TabGuard {
    pub fn begin(config: &Config, title: &str, color: &str) -> Self {
        let mut guard = Self {
            active: config.tab && io::stdout().is_terminal() && virtual_terminal(),
            konsole: false,
            formats: Default::default(),
        };
        if !guard.active {
            return guard;
        }
        guard.konsole = konsole_args("tabTitleFormat", &[]).is_some();
        if guard.konsole {
            guard.formats = [konsole_get("0"), konsole_get("1")];
            konsole("setTabTitleFormat", &["is", "0", title]);
            konsole("setTabTitleFormat", &["is", "1", title]);
            if !color.is_empty() {
                konsole("setTabColor", &["s", color]);
            }
        }
        print!("\x1b]30;{title}\x07\x1b]0;{title}\x07");
        let _ = io::stdout().flush();
        guard
    }
}
impl Drop for TabGuard {
    fn drop(&mut self) {
        if !self.active {
            return;
        }
        if self.konsole {
            konsole("setTabColor", &["s", ""]);
            for (i, value) in self.formats.iter().enumerate() {
                if !value.is_empty() {
                    konsole("setTabTitleFormat", &["is", &i.to_string(), value]);
                }
            }
        } else {
            print!("\x1b]30;%d : %n\x07\x1b]0;\x07");
            let _ = io::stdout().flush();
        }
    }
}
pub fn restore_terminal() {
    if io::stdout().is_terminal() && virtual_terminal() {
        print!("\x1b[r\x1b(B\x1b[m\x1b[?1l\x1b>\x1b[0 q\x1b[?25h\x1b[?1000l\x1b[?1002l\x1b[?1003l\x1b[?1006l\x1b[?1005l\x1b[?2004l\x1b[?1004l\x1b[>4m\x1b[?69l\x1b[?2031l\x1b[?1049l");
        let _ = io::stdout().flush();
    }
}
pub fn client_done(rc: i32) -> bool {
    if rc != 0 && !silent("tmux", &["list-sessions"]) {
        restore_terminal();
        eprintln!("hgs: the tmux server died; terminal restored, sessions on this box are gone");
        true
    } else {
        false
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn remote_commands_keep_posix_quoting_for_windows_paths() {
        assert_eq!(quote("~/.local/bin/hgs"), "~/.local/bin/hgs");
        assert_eq!(quote(r"C:\Users\a b"), r"'C:\Users\a b'");
        assert_eq!(quote("it's"), r"'it'\''s'");
        assert_eq!(quote(""), "''");
        assert_eq!(
            join(&["inspect", "zerus://session?a=1&b=2", "--local"]),
            "inspect 'zerus://session?a=1&b=2' --local"
        );
    }

    #[test]
    fn executables_are_found_on_path() {
        let dir = tempfile::tempdir().unwrap();
        let name = "hgs-platform-fixture";
        let file = dir.path().join(if cfg!(windows) {
            format!("{name}.exe")
        } else {
            name.into()
        });
        std::fs::write(&file, b"").unwrap();
        #[cfg(unix)]
        {
            use std::os::unix::fs::PermissionsExt;
            std::fs::set_permissions(&file, std::fs::Permissions::from_mode(0o755)).unwrap();
        }
        // Other directories on PATH must not hide or invent the fixture.
        let path = env::join_paths([dir.path().join("missing"), dir.path().to_owned()]).unwrap();
        assert_eq!(which_in(name, &path).as_deref(), Some(file.as_path()));
        assert_eq!(which_in("hgs-platform-absent", &path), None);
        assert_eq!(which_in(name, OsStr::new("")), None);
        assert!(executable(&file));
        assert!(!executable(dir.path()));
        assert_eq!(
            which(&file.to_string_lossy()).as_deref(),
            Some(file.as_path())
        );
    }

    #[test]
    fn replacement_is_atomic_and_contention_is_recognized() {
        let dir = tempfile::tempdir().unwrap();
        let target = dir.path().join("data.json");
        std::fs::write(&target, b"old").unwrap();
        let mut temporary = tempfile::NamedTempFile::new_in(dir.path()).unwrap();
        temporary.write_all(b"new").unwrap();
        persist(temporary, &target).unwrap();
        assert_eq!(std::fs::read(&target).unwrap(), b"new");
        assert_eq!(std::fs::read_dir(dir.path()).unwrap().count(), 1);
        sync_dir(dir.path()).unwrap();

        use fs2::FileExt;
        let lock = dir.path().join("data.lock");
        let holder = File::create(&lock).unwrap();
        holder.lock_exclusive().unwrap();
        let contender = File::open(&lock).unwrap();
        let error = contender.try_lock_exclusive().unwrap_err();
        assert!(lock_contended(&error), "{error:?}");
        assert!(!lock_contended(&io::Error::from(io::ErrorKind::NotFound)));
        drop(holder);

        // Dropping the guard releases the lock even while another handle to the
        // same open file stays alive, as in a child between fork and exec.
        let owner = File::create(&lock).unwrap();
        let still_open = owner.try_clone().unwrap();
        drop(FileLock::exclusive(owner).unwrap());
        contender.try_lock_exclusive().unwrap();
        drop(still_open);
    }
}
