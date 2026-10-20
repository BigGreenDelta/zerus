//! Private state on disk, as `connector.py:108-152` checks it: an owned 0700
//! `state_dir` and owned 0600 regular files with one link, opened without
//! following symlinks and never repaired. Ownership is compared with the
//! effective uid from a system call, so a read-only check creates nothing.
use crate::error::{ConnectorError, Result, IN_USE, PRIVATE_DIRECTORY, PRIVATE_FILE, UNSAFE_FILE};
use rustix::fs::{flock, FlockOperation, OFlags};
use std::fs::{DirBuilder, File, Metadata, OpenOptions};
use std::io;
use std::os::unix::fs::{DirBuilderExt, MetadataExt, OpenOptionsExt};
use std::path::Path;

pub fn euid() -> u32 {
    rustix::process::geteuid().as_raw()
}

fn private_bits(metadata: &Metadata) -> bool {
    metadata.mode() & 0o077 == 0
}

/// Python's `directory.mkdir(parents=True, exist_ok=True, mode=0o700)`:
/// missing parents get 0777 and the directory 0700, both less the umask. An
/// existing directory, or a symlink to one, is accepted here and refused by
/// [`check_private_directory`].
pub fn create_directory(path: &Path) -> io::Result<()> {
    make_directory(path, 0o700)
}

fn make_directory(path: &Path, mode: u32) -> io::Result<()> {
    match DirBuilder::new().mode(mode).create(path) {
        Ok(()) => Ok(()),
        Err(error) if error.kind() == io::ErrorKind::NotFound => {
            let parent = path
                .parent()
                .filter(|parent| !parent.as_os_str().is_empty())
                .ok_or(error)?;
            make_directory(parent, 0o777)?;
            match DirBuilder::new().mode(mode).create(path) {
                Err(error) if !path.is_dir() => Err(error),
                _ => Ok(()),
            }
        }
        // Python does not trust EEXIST alone: any failure is fine for a directory.
        Err(error) => {
            if path.is_dir() {
                Ok(())
            } else {
                Err(error)
            }
        }
    }
}

/// The state directory itself, not a symlink to one, owned and private.
pub fn check_private_directory(path: &Path) -> Result<()> {
    let refused = || ConnectorError::new(PRIVATE_DIRECTORY);
    let metadata = path.symlink_metadata().map_err(|_| refused())?;
    if !metadata.file_type().is_dir() || metadata.uid() != euid() || !private_bits(&metadata) {
        return Err(refused());
    }
    Ok(())
}

fn open_flags(options: &mut OpenOptions) -> &mut OpenOptions {
    options.custom_flags(OFlags::NOFOLLOW.bits() as i32)
}

/// Python's `private_file`: open read-write without following a symlink,
/// creating it 0600 when `create` is set, then require an owned, private
/// regular file with one link. A missing file without `create` is `None`.
pub fn private_file(path: &Path, create: bool) -> Result<Option<File>> {
    let mut options = OpenOptions::new();
    options.read(true).write(true).create(create).mode(0o600);
    let file = match open_flags(&mut options).open(path) {
        Ok(file) => file,
        Err(error) if !create && error.kind() == io::ErrorKind::NotFound => return Ok(None),
        Err(_) => return Err(ConnectorError::new(UNSAFE_FILE)),
    };
    let metadata = file
        .metadata()
        .map_err(|_| ConnectorError::new(UNSAFE_FILE))?;
    if !metadata.file_type().is_file()
        || metadata.uid() != euid()
        || !private_bits(&metadata)
        || metadata.nlink() != 1
    {
        return Err(ConnectorError::new(PRIVATE_FILE));
    }
    Ok(Some(file))
}

/// What [`private_file`] would decide, from `lstat` alone: a read-only check
/// opens nothing for writing and never creates a file.
pub fn inspect_private_file(path: &Path) -> std::result::Result<Option<Metadata>, &'static str> {
    let metadata = match path.symlink_metadata() {
        Ok(metadata) => metadata,
        Err(error) if error.kind() == io::ErrorKind::NotFound => return Ok(None),
        Err(_) => return Err(UNSAFE_FILE),
    };
    inspect_private_metadata(metadata)
}

fn inspect_private_metadata(
    metadata: Metadata,
) -> std::result::Result<Option<Metadata>, &'static str> {
    let kind = metadata.file_type();
    // A symlink fails O_NOFOLLOW and a directory fails O_RDWR: "not safe".
    if kind.is_symlink() || kind.is_dir() || (kind.is_file() && metadata.mode() & 0o600 != 0o600) {
        return Err(UNSAFE_FILE);
    }
    if !kind.is_file() || metadata.uid() != euid() || !private_bits(&metadata) {
        return Err(PRIVATE_FILE);
    }
    match metadata.nlink() {
        // lstat can finish after SQLite unlinks a rollback journal: its inode
        // still exists during the lookup but no directory entry names it.
        // This read-only snapshot is absent, not an unsafe live file. The
        // runtime's descriptor-based private_file check remains stricter.
        0 => Ok(None),
        1 => Ok(Some(metadata)),
        _ => Err(PRIVATE_FILE),
    }
}

/// The exclusive `flock(2)` the Python connector takes on `connector.lock`;
/// `hgs status` probes the same lock with a shared one. It lasts while the
/// file stays open.
pub fn lock_exclusive(file: &File) -> Result<()> {
    flock(file, FlockOperation::NonBlockingLockExclusive).map_err(|_| ConnectorError::new(IN_USE))
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum LockState {
    /// No lock file: no connector has started on this directory.
    Absent,
    /// A connector holds the journal.
    Held,
    Free,
    /// The probe could not open the file (a symlink, or no access).
    Unknown,
}

impl LockState {
    pub fn name(self) -> &'static str {
        match self {
            LockState::Absent => "absent",
            LockState::Held => "held",
            LockState::Free => "free",
            LockState::Unknown => "unknown",
        }
    }
}

/// The shared, non-blocking probe `hgs status` uses, only on an existing file.
pub fn probe_lock(path: &Path) -> LockState {
    let mut options = OpenOptions::new();
    options.read(true);
    let file = match open_flags(&mut options).open(path) {
        Ok(file) => file,
        Err(error) if error.kind() == io::ErrorKind::NotFound => return LockState::Absent,
        Err(_) => return LockState::Unknown,
    };
    match flock(&file, FlockOperation::NonBlockingLockShared) {
        // Closing the file releases the probe at once.
        Ok(()) => LockState::Free,
        Err(error) if error == rustix::io::Errno::WOULDBLOCK => LockState::Held,
        Err(_) => LockState::Unknown,
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::os::unix::fs::{symlink, PermissionsExt};

    #[test]
    fn unlinked_inspection_is_absent_but_live_unsafe_files_are_refused() {
        struct Scratch(std::path::PathBuf);
        impl Drop for Scratch {
            fn drop(&mut self) {
                let _ = std::fs::remove_dir_all(&self.0);
            }
        }
        let nonce = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .unwrap()
            .as_nanos();
        let scratch = Scratch(std::env::temp_dir().join(format!(
            "zerus-journal-inspection-{}-{nonce}",
            std::process::id()
        )));
        DirBuilder::new().mode(0o700).create(&scratch.0).unwrap();
        let path = scratch.0.join("journal.sqlite3-journal");
        let file = OpenOptions::new()
            .read(true)
            .write(true)
            .create_new(true)
            .mode(0o600)
            .open(&path)
            .unwrap();
        assert!(inspect_private_file(&path).unwrap().is_some());

        let hard_link = scratch.0.join("hard-link");
        std::fs::hard_link(&path, &hard_link).unwrap();
        assert_eq!(inspect_private_file(&path).unwrap_err(), PRIVATE_FILE);
        std::fs::remove_file(hard_link).unwrap();
        file.set_permissions(std::fs::Permissions::from_mode(0o644))
            .unwrap();
        assert_eq!(inspect_private_file(&path).unwrap_err(), PRIVATE_FILE);
        file.set_permissions(std::fs::Permissions::from_mode(0o600))
            .unwrap();
        let link = scratch.0.join("symlink");
        symlink(&path, &link).unwrap();
        assert_eq!(inspect_private_file(&link).unwrap_err(), UNSAFE_FILE);

        std::fs::remove_file(&path).unwrap();
        let unlinked = file.metadata().unwrap();
        assert_eq!(unlinked.nlink(), 0);
        assert!(inspect_private_metadata(unlinked).unwrap().is_none());
        assert!(inspect_private_file(&path).unwrap().is_none());
    }
}
