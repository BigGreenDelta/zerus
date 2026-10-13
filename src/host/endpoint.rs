//! Where the session host listens and how one host per user and state directory
//! is kept (spec D3, D5): the pipe name, the `host.secret` it is derived from, the
//! pipe's security descriptor and the `host.lock` a running host holds.
//!
//! The pipe is `\\.\pipe\hgs-host-<hex>`, `<hex>` being the first 32 hex digits of
//! SHA-256 over the user SID, the case-folded canonical state directory and the
//! secret, separated by NUL bytes. Isolated tests (`HGS_STATE_DIR`) therefore never
//! reach a live host, and another local user cannot compute the name to squat it.
//! The name carries no protocol version.
use super::protocol::Integrity;
use crate::platform;
use rand_core::{OsRng, RngCore};
use sha2::{Digest, Sha256};
use std::{
    fmt, fs,
    io::{self, Read, Write},
    path::{Path, PathBuf},
    thread,
    time::{Duration, Instant},
};

pub const PIPE_PREFIX: &str = r"\\.\pipe\hgs-host-";
/// Below the state directory.
pub const SECRET_FILE: &str = "host.secret";
pub const LOCK_FILE: &str = "host.lock";
/// Random bytes in a secret; the file holds them as lowercase hex.
const SECRET_BYTES: usize = 32;

fn hex(bytes: &[u8]) -> String {
    const DIGITS: &[u8; 16] = b"0123456789abcdef";
    let mut text = String::with_capacity(bytes.len() * 2);
    for byte in bytes {
        text.push(char::from(DIGITS[usize::from(byte >> 4)]));
        text.push(char::from(DIGITS[usize::from(byte & 15)]));
    }
    text
}

/// `path` as Windows compares file names: each character becomes its uppercase
/// form when that is a single character (`é` to `É`, as the NTFS uppercase table
/// maps it), and stays as it is otherwise (`ß`). Only letter case is folded; one
/// directory reached through `HOME`, `USERPROFILE`, a junction or a short name has
/// one canonical spelling only after `platform::canonical`, which is the caller's
/// job.
pub fn fold_case(path: &str) -> String {
    path.chars()
        .map(|character| {
            let mut upper = character.to_uppercase();
            match (upper.next(), upper.next()) {
                (Some(single), None) => single,
                _ => character,
            }
        })
        .collect()
}

/// The host pipe of the user with SID `user_sid` for the canonical `state_dir`.
pub fn pipe_name(user_sid: &str, state_dir: &Path, secret: &str) -> String {
    let mut hash = Sha256::new();
    hash.update(user_sid.as_bytes());
    hash.update([0]);
    hash.update(fold_case(&state_dir.to_string_lossy()).as_bytes());
    hash.update([0]);
    hash.update(secret.as_bytes());
    format!("{PIPE_PREFIX}{}", &hex(&hash.finalize())[..32])
}

/// The host pipe for `state_dir`, which must be canonical, creating its secret
/// on first use.
pub fn resolve(user_sid: &str, state_dir: &Path) -> Result<String, SecretError> {
    Ok(pipe_name(user_sid, state_dir, &secret(state_dir)?))
}

/// Why `host.secret` gave no secret.
#[derive(Debug)]
pub enum SecretError {
    Io(PathBuf, io::Error),
    /// The file holds something other than a secret written here.
    Malformed(PathBuf),
}

impl fmt::Display for SecretError {
    fn fmt(&self, f: &mut fmt::Formatter) -> fmt::Result {
        match self {
            Self::Io(path, error) => write!(f, "{}: {error}", path.display()),
            Self::Malformed(path) => write!(
                f,
                "{} does not hold a session host secret; stop the session host and remove the file",
                path.display()
            ),
        }
    }
}

impl std::error::Error for SecretError {}

/// The secret of `state_dir`'s host. The first caller creates it: 32 random bytes
/// as hex, written privately to a temporary file that is then moved into place
/// without replacing anything, so a reader never sees a partial secret and
/// concurrent first callers all end up with the one that landed first. A file of
/// any other shape is an error, never replaced: a running host may use it.
///
/// Windows keeps no mode bits; there the file inherits the state directory's
/// owner-only ACL, as other private files do (`platform::make_private`).
pub fn secret(state_dir: &Path) -> Result<String, SecretError> {
    let path = state_dir.join(SECRET_FILE);
    match read_secret(&path) {
        Err(SecretError::Io(_, error)) if error.kind() == io::ErrorKind::NotFound => {}
        found => return found,
    }
    let mut bytes = [0; SECRET_BYTES];
    OsRng
        .try_fill_bytes(&mut bytes)
        .map_err(|error| SecretError::Io(path.clone(), io::Error::other(error)))?;
    let fresh = hex(&bytes);
    match create_secret(state_dir, &path, &fresh) {
        Ok(()) => Ok(fresh),
        // Another process created it first.
        Err(error) if error.kind() == io::ErrorKind::AlreadyExists => read_secret(&path),
        Err(error) => Err(SecretError::Io(path, error)),
    }
}

/// The secret of `state_dir`'s host when one was created, without creating it:
/// a host creates its secret before it listens, so without one no host runs
/// there. For readers that must never start or prepare a host (a client that
/// only connects, `session-host status`).
pub fn existing_secret(state_dir: &Path) -> Result<Option<String>, SecretError> {
    match read_secret(&state_dir.join(SECRET_FILE)) {
        Ok(secret) => Ok(Some(secret)),
        Err(SecretError::Io(_, error)) if error.kind() == io::ErrorKind::NotFound => Ok(None),
        Err(error) => Err(error),
    }
}

fn read_secret(path: &Path) -> Result<String, SecretError> {
    let failed = |error| SecretError::Io(path.to_owned(), error);
    let file = platform::refuse_link(fs::OpenOptions::new().read(true))
        .open(path)
        .map_err(failed)?;
    let mut text = Vec::new();
    // One byte more than a secret shows that a file is too long.
    file.take(SECRET_BYTES as u64 * 2 + 1)
        .read_to_end(&mut text)
        .map_err(failed)?;
    if text.len() != SECRET_BYTES * 2
        || !text
            .iter()
            .all(|c| c.is_ascii_hexdigit() && !c.is_ascii_uppercase())
    {
        return Err(SecretError::Malformed(path.to_owned()));
    }
    Ok(String::from_utf8_lossy(&text).into_owned())
}

/// Windows refuses a rename for a moment while a scanner holds the new file
/// (`ERROR_ACCESS_DENIED`, `ERROR_SHARING_VIOLATION`, `ERROR_LOCK_VIOLATION`), as
/// the private `platform::transient_replace_error` knows.
fn transient(error: &io::Error) -> bool {
    cfg!(windows) && matches!(error.raw_os_error(), Some(5 | 32 | 33))
}

fn create_secret(dir: &Path, path: &Path, secret: &str) -> io::Result<()> {
    let mut temporary = tempfile::Builder::new()
        .prefix(".host.secret.")
        .tempfile_in(dir)?;
    platform::make_private(temporary.as_file())?;
    temporary.write_all(secret.as_bytes())?;
    temporary.as_file().sync_all()?;
    let deadline = Instant::now() + Duration::from_secs(1);
    loop {
        match temporary.persist_noclobber(path) {
            Ok(_) => return platform::sync_dir(dir),
            Err(error) if transient(&error.error) && Instant::now() < deadline => {
                temporary = error.file;
                thread::sleep(Duration::from_millis(20));
            }
            Err(error) => return Err(error.error),
        }
    }
}

/// The pipe's security descriptor in SDDL: a protected DACL that grants everything
/// to `user_sid` alone and, for a host at high integrity or above, a mandatory
/// label that keeps lower-integrity processes from reading or writing the pipe.
pub fn pipe_sddl(user_sid: &str, integrity_rid: u32) -> String {
    let mut sddl = format!("D:P(A;;GA;;;{user_sid})");
    if integrity_rid >= Integrity::HIGH {
        sddl.push_str("S:(ML;;NWNR;;;HI)");
    }
    sddl
}

/// The exclusive lock a running host holds on `host.lock` for its whole life.
pub struct HostLock {
    _lock: platform::FileLock,
}

#[derive(Debug)]
pub enum LockError {
    /// Another host holds the lock.
    Held,
    Io(io::Error),
}

impl fmt::Display for LockError {
    fn fmt(&self, f: &mut fmt::Formatter) -> fmt::Result {
        match self {
            Self::Held => f.write_str("another session host runs for this state directory"),
            Self::Io(error) => write!(f, "cannot lock the session host: {error}"),
        }
    }
}

impl std::error::Error for LockError {}

/// Take the host lock of `state_dir` without waiting.
pub fn lock(state_dir: &Path) -> Result<HostLock, LockError> {
    let file = platform::private_open(
        fs::OpenOptions::new()
            .create(true)
            .truncate(false)
            .write(true),
    )
    .open(state_dir.join(LOCK_FILE))
    .map_err(LockError::Io)?;
    match fs2::FileExt::try_lock_exclusive(&file) {
        Ok(()) => Ok(HostLock {
            _lock: platform::FileLock::held(file),
        }),
        Err(error) if platform::lock_contended(&error) => Err(LockError::Held),
        Err(error) => Err(LockError::Io(error)),
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::{Arc, Barrier};

    const SID: &str = "S-1-5-21-1004336348-1177238915-682003330-1001";
    const SECRET: &str = "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff";

    #[test]
    fn pipe_names_hash_user_directory_and_secret() {
        let dir = Path::new(r"C:\Users\Ann\.local\state\hgs");
        let name = pipe_name(SID, dir, SECRET);
        // The scheme is fixed: a host and clients of different builds must agree.
        let mut expected = Sha256::new();
        expected.update(format!("{SID}\0C:\\USERS\\ANN\\.LOCAL\\STATE\\HGS\0{SECRET}").as_bytes());
        assert_eq!(
            name,
            format!(r"\\.\pipe\hgs-host-{}", &hex(&expected.finalize())[..32])
        );
        let digits = name.strip_prefix(PIPE_PREFIX).unwrap();
        assert_eq!(digits.len(), 32);
        assert!(digits
            .bytes()
            .all(|c| c.is_ascii_hexdigit() && !c.is_ascii_uppercase()));

        // Case differences of one canonical directory name one host.
        for spelling in [
            r"c:\users\ann\.local\state\hgs",
            r"C:\USERS\ann\.Local\State\HGS",
        ] {
            assert_eq!(pipe_name(SID, Path::new(spelling), SECRET), name);
        }
        // Everything else names another host.
        for other in [
            pipe_name("S-1-5-21-1004336348-1177238915-682003330-1002", dir, SECRET),
            pipe_name(
                SID,
                Path::new(r"C:\Users\Ann\.local\state\hgs-test"),
                SECRET,
            ),
            pipe_name(SID, dir, &SECRET.replace('0', "1")),
        ] {
            assert_ne!(other, name);
        }
        // Separators keep the parts apart.
        assert_ne!(
            pipe_name("S-1-5-1", Path::new("2"), SECRET),
            pipe_name("S-1-5-", Path::new("12"), SECRET)
        );
    }

    #[test]
    fn case_folding_maps_single_characters_only() {
        assert_eq!(fold_case(r"C:\Users\Ann"), r"C:\USERS\ANN");
        assert_eq!(fold_case("d:/Zoë/Ärger/ﬁle"), "D:/ZOË/ÄRGER/ﬁLE");
        // ß uppercases to two letters; Windows keeps it, and so does the fold.
        assert_eq!(fold_case("Straße"), "STRAßE");
        assert_eq!(fold_case("ǆ"), "Ǆ");
        assert_eq!(fold_case(""), "");
    }

    #[cfg(unix)]
    #[test]
    fn canonical_spellings_of_one_directory_name_one_host() {
        let root = tempfile::tempdir().unwrap();
        let state = root.path().join("state");
        fs::create_dir(&state).unwrap();
        let link = root.path().join("link");
        std::os::unix::fs::symlink(&state, &link).unwrap();
        let name = |dir: &Path| pipe_name(SID, &platform::canonical(dir).unwrap(), SECRET);
        assert_eq!(name(&link), name(&state));
        assert_eq!(name(&state.join("..").join("state")), name(&state));
    }

    #[test]
    fn the_secret_is_created_once_and_privately() {
        let dir = tempfile::tempdir().unwrap();
        let first = secret(dir.path()).unwrap();
        assert_eq!(first.len(), SECRET_BYTES * 2);
        assert!(first
            .bytes()
            .all(|c| c.is_ascii_hexdigit() && !c.is_ascii_uppercase()));
        let path = dir.path().join(SECRET_FILE);
        assert_eq!(fs::read_to_string(&path).unwrap(), first);
        assert_eq!(secret(dir.path()).unwrap(), first);
        #[cfg(unix)]
        assert!(platform::verify_private(&path, platform::Private::File).unwrap());
        // Only the secret is left behind.
        let entries: Vec<_> = fs::read_dir(dir.path())
            .unwrap()
            .map(|entry| entry.unwrap().file_name())
            .collect();
        assert_eq!(entries, [SECRET_FILE]);

        assert_eq!(existing_secret(dir.path()).unwrap(), Some(first.clone()));

        let other = tempfile::tempdir().unwrap();
        // A reader that must not prepare a host finds none and creates nothing.
        assert_eq!(existing_secret(other.path()).unwrap(), None);
        assert_eq!(fs::read_dir(other.path()).unwrap().count(), 0);
        assert_ne!(secret(other.path()).unwrap(), first);
        assert_eq!(
            resolve(SID, dir.path()).unwrap(),
            pipe_name(SID, dir.path(), &first)
        );
    }

    #[test]
    fn an_existing_secret_is_read_and_a_missing_one_left_missing() {
        let dir = tempfile::tempdir().unwrap();
        assert_eq!(existing_secret(dir.path()).unwrap(), None);
        assert!(
            fs::read_dir(dir.path()).unwrap().next().is_none(),
            "nothing written"
        );
        assert_eq!(existing_secret(&dir.path().join("missing")).unwrap(), None);
        let created = secret(dir.path()).unwrap();
        assert_eq!(existing_secret(dir.path()).unwrap(), Some(created));
        fs::write(dir.path().join(SECRET_FILE), "bad").unwrap();
        assert!(matches!(
            existing_secret(dir.path()),
            Err(SecretError::Malformed(_))
        ));
    }

    #[test]
    fn concurrent_first_callers_share_one_secret() {
        let dir = Arc::new(tempfile::tempdir().unwrap());
        let start = Arc::new(Barrier::new(8));
        let callers: Vec<_> = (0..8)
            .map(|_| {
                let (dir, start) = (dir.clone(), start.clone());
                thread::spawn(move || {
                    start.wait();
                    secret(dir.path()).unwrap()
                })
            })
            .collect();
        let secrets: Vec<String> = callers.into_iter().map(|c| c.join().unwrap()).collect();
        assert!(secrets.iter().all(|s| *s == secrets[0]), "{secrets:?}");
        assert_eq!(
            fs::read_to_string(dir.path().join(SECRET_FILE)).unwrap(),
            secrets[0]
        );
    }

    #[test]
    fn a_malformed_secret_is_reported_and_kept() {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join(SECRET_FILE);
        for content in [
            String::new(),
            "abc".into(),
            SECRET.to_uppercase(),
            format!("{SECRET}\n"),
            format!("{SECRET}0"),
            SECRET.replace('a', "g"),
        ] {
            fs::write(&path, &content).unwrap();
            let error = secret(dir.path()).unwrap_err();
            assert!(
                matches!(&error, SecretError::Malformed(p) if *p == path),
                "{content:?}"
            );
            assert!(error.to_string().contains("remove the file"));
            assert!(matches!(
                existing_secret(dir.path()),
                Err(SecretError::Malformed(_))
            ));
            assert_eq!(fs::read_to_string(&path).unwrap(), content);
        }
        fs::write(&path, SECRET).unwrap();
        assert_eq!(secret(dir.path()).unwrap(), SECRET);

        let missing = dir.path().join("missing");
        assert!(matches!(
            secret(&missing),
            Err(SecretError::Io(_, error)) if error.kind() == io::ErrorKind::NotFound
        ));
    }

    #[cfg(unix)]
    #[test]
    fn a_linked_secret_is_refused() {
        let dir = tempfile::tempdir().unwrap();
        let target = dir.path().join("elsewhere");
        fs::write(&target, SECRET).unwrap();
        std::os::unix::fs::symlink(&target, dir.path().join(SECRET_FILE)).unwrap();
        assert!(matches!(secret(dir.path()), Err(SecretError::Io(..))));
        assert_eq!(fs::read_to_string(&target).unwrap(), SECRET);
    }

    #[test]
    fn the_pipe_descriptor_names_only_the_user() {
        assert_eq!(
            pipe_sddl(SID, Integrity::MEDIUM),
            format!("D:P(A;;GA;;;{SID})")
        );
        assert_eq!(
            pipe_sddl(SID, Integrity::HIGH),
            format!("D:P(A;;GA;;;{SID})S:(ML;;NWNR;;;HI)")
        );
        assert!(pipe_sddl(SID, Integrity::SYSTEM).ends_with("HI)"));
    }

    #[test]
    fn one_host_holds_the_lock() {
        let dir = tempfile::tempdir().unwrap();
        let held = lock(dir.path()).unwrap();
        let error = lock(dir.path()).err().unwrap();
        assert!(matches!(error, LockError::Held));
        assert_eq!(
            error.to_string(),
            "another session host runs for this state directory"
        );
        drop(held);
        let again = lock(dir.path()).unwrap();
        assert!(matches!(lock(dir.path()), Err(LockError::Held)));
        drop(again);
        assert!(matches!(
            lock(&dir.path().join("missing")),
            Err(LockError::Io(error)) if error.kind() == io::ErrorKind::NotFound
        ));
    }
}
