//! Locked, bounded, atomic storage for device keys and pending enrollment.
//! Public reports must select fields explicitly; this module never logs bytes.
use crate::fsguard;
use ring::rand::{SecureRandom, SystemRandom};
use std::fs::File;
use std::io::{self, Read, Write};
use std::path::Path;
#[cfg(windows)]
use std::path::PathBuf;

pub const MAX_STATE_BYTES: usize = 2 * 1024 * 1024;
pub type Result<T> = std::result::Result<T, io::Error>;

fn refused(message: &'static str) -> io::Error {
    io::Error::other(message)
}

fn valid_name(name: &str) -> Result<()> {
    if name.is_empty()
        || name.len() > 128
        || name == "."
        || name == ".."
        || !name
            .bytes()
            .all(|b| b.is_ascii_alphanumeric() || b"._-".contains(&b))
    {
        return Err(refused("invalid private state name"));
    }
    Ok(())
}

pub fn random_id() -> Result<String> {
    let mut bytes = [0u8; 16];
    SystemRandom::new()
        .fill(&mut bytes)
        .map_err(|_| refused("secure random identity unavailable"))?;
    Ok(bytes.iter().map(|b| format!("{b:02x}")).collect())
}

/// The open directory anchors Unix operations even if its path is renamed.
/// Windows' directory guard pins the validated path and its ancestors.
pub struct Store {
    #[cfg(windows)]
    path: PathBuf,
    #[cfg(unix)]
    directory: File,
    #[cfg(windows)]
    _directory: zerus_windows_private::DirectoryGuard,
    _lock: File,
}

impl std::fmt::Debug for Store {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("PrivateStateStore").finish_non_exhaustive()
    }
}

impl Store {
    /// Mutation guard. Creating the private lock is intentional; status reads
    /// should inspect an existing atomic snapshot without invoking this method.
    pub fn lock(path: &Path, create: bool) -> Result<Self> {
        #[cfg(unix)]
        let directory = {
            use rustix::fs::{Mode, OFlags};
            use std::os::unix::fs::MetadataExt;
            if create {
                fsguard::create_directory(path)?;
            }
            let directory: File = rustix::fs::open(
                path,
                OFlags::RDONLY | OFlags::DIRECTORY | OFlags::NOFOLLOW | OFlags::CLOEXEC,
                Mode::empty(),
            )?
            .into();
            let metadata = directory.metadata()?;
            if metadata.uid() != fsguard::euid() || metadata.mode() & 0o077 != 0 {
                return Err(refused("private state directory is not private"));
            }
            directory
        };
        #[cfg(windows)]
        let directory = zerus_windows_private::directory(path, create)?;
        #[cfg(unix)]
        let lock = open_at(&directory, "workspace-state.lock", true, false)?;
        #[cfg(windows)]
        let lock =
            zerus_windows_private::open_file(&path.join("workspace-state.lock"), true, true)?;
        fsguard::lock_exclusive(&lock)
            .map_err(|_| refused("another private state operation is in progress"))?;
        Ok(Self {
            #[cfg(windows)]
            path: path.to_owned(),
            #[cfg(unix)]
            directory,
            #[cfg(windows)]
            _directory: directory,
            _lock: lock,
        })
    }

    pub fn read(&self, name: &str) -> Result<Option<Vec<u8>>> {
        valid_name(name)?;
        #[cfg(unix)]
        let opened = open_at(&self.directory, name, false, false);
        #[cfg(windows)]
        let opened = zerus_windows_private::open_file(&self.path.join(name), false, false);
        let file = match opened {
            Ok(file) => file,
            Err(error) if error.kind() == io::ErrorKind::NotFound => return Ok(None),
            Err(error) => return Err(error),
        };
        if file.metadata()?.len() > MAX_STATE_BYTES as u64 {
            return Err(refused("private state exceeds its limit"));
        }
        let mut bytes = Vec::new();
        file.take(MAX_STATE_BYTES as u64 + 1)
            .read_to_end(&mut bytes)?;
        if bytes.len() > MAX_STATE_BYTES {
            return Err(refused("private state exceeds its limit"));
        }
        Ok(Some(bytes))
    }

    pub fn revision(bytes: Option<&[u8]>) -> String {
        bytes.map(zerus_relay::json::digest).unwrap_or_default()
    }

    /// A failed directory sync may follow a committed replacement. Callers must
    /// reload the snapshot after any failure, never infer an automatic retry.
    pub fn replace(&self, name: &str, expected: &str, bytes: &[u8]) -> Result<String> {
        valid_name(name)?;
        if bytes.len() > MAX_STATE_BYTES {
            return Err(refused("private state exceeds its limit"));
        }
        if Self::revision(self.read(name)?.as_deref()) != expected {
            return Err(refused("private state changed; reload before retrying"));
        }
        let temporary = format!(".workspace-{}", random_id()?);
        #[cfg(unix)]
        let mut file = open_at(&self.directory, &temporary, true, true)?;
        #[cfg(windows)]
        let mut file = {
            let file = std::fs::OpenOptions::new()
                .read(true)
                .write(true)
                .create_new(true)
                .open(self.path.join(&temporary))?;
            zerus_windows_private::verify(&file, false)?;
            file
        };
        let result = (|| {
            file.write_all(bytes)?;
            file.sync_all()?;
            drop(file);
            if Self::revision(self.read(name)?.as_deref()) != expected {
                return Err(refused("private state changed; reload before retrying"));
            }
            #[cfg(unix)]
            {
                rustix::fs::renameat(&self.directory, &temporary, &self.directory, name)?;
                self.directory.sync_all()?;
            }
            #[cfg(windows)]
            zerus_windows_private::replace(&self.path.join(&temporary), &self.path.join(name))?;
            Ok(Self::revision(Some(bytes)))
        })();
        if result.is_err() {
            #[cfg(unix)]
            let _ = rustix::fs::unlinkat(&self.directory, &temporary, rustix::fs::AtFlags::empty());
            #[cfg(windows)]
            let _ = std::fs::remove_file(self.path.join(&temporary));
        }
        result
    }
}

#[cfg(unix)]
fn open_at(directory: &File, name: &str, create: bool, exclusive: bool) -> Result<File> {
    use rustix::fs::{Mode, OFlags};
    use std::os::unix::fs::MetadataExt;
    let mut flags = OFlags::NOFOLLOW | OFlags::NONBLOCK | OFlags::CLOEXEC;
    flags |= if create {
        OFlags::RDWR | OFlags::CREATE
    } else {
        OFlags::RDONLY
    };
    if exclusive {
        flags |= OFlags::EXCL;
    }
    let file: File = rustix::fs::openat(directory, name, flags, Mode::RUSR | Mode::WUSR)?.into();
    let metadata = file.metadata()?;
    if !metadata.is_file()
        || metadata.uid() != fsguard::euid()
        || metadata.mode() & 0o077 != 0
        || metadata.nlink() != 1
    {
        return Err(refused("private state file is unsafe"));
    }
    Ok(file)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn durable_replacement_refuses_stale_state_and_concurrent_writers() {
        let directory = tempfile::tempdir().unwrap();
        let path = directory.path().join("private");
        let store = Store::lock(&path, true).unwrap();
        assert!(Store::lock(&path, false).is_err());
        let revision = store.replace("device.json", "", b"first identity").unwrap();
        assert!(store
            .replace("device.json", "", b"different identity")
            .is_err());
        assert_eq!(
            store.read("device.json").unwrap().unwrap(),
            b"first identity"
        );
        let next = store
            .replace("device.json", &revision, b"confirmed identity")
            .unwrap();
        assert_ne!(revision, next);
        drop(store);
        let reopened = Store::lock(&path, false).unwrap();
        assert_eq!(
            reopened.read("device.json").unwrap().unwrap(),
            b"confirmed identity"
        );
    }

    #[test]
    fn names_and_size_are_bounded_before_mutation() {
        let directory = tempfile::tempdir().unwrap();
        let path = directory.path().join("private");
        let store = Store::lock(&path, true).unwrap();
        for name in ["", ".", "..", "../outside", "x/y", "x\\y", "x\n"] {
            assert!(store.replace(name, "", b"secret").is_err());
        }
        assert!(store
            .replace("device.json", "", &vec![0; MAX_STATE_BYTES + 1])
            .is_err());
        assert!(store.read("device.json").unwrap().is_none());
        assert!(!format!("{store:?}").contains("private"));
    }

    #[cfg(unix)]
    #[test]
    fn symlink_and_hardlink_destinations_are_preserved() {
        use std::os::unix::fs::{symlink, OpenOptionsExt};
        let directory = tempfile::tempdir().unwrap();
        let outside = directory.path().join("outside");
        let mut file = std::fs::OpenOptions::new()
            .write(true)
            .create_new(true)
            .mode(0o600)
            .open(&outside)
            .unwrap();
        file.write_all(b"untouched").unwrap();
        drop(file);
        let path = directory.path().join("private");
        let store = Store::lock(&path, true).unwrap();
        symlink(&outside, path.join("link.json")).unwrap();
        std::fs::hard_link(&outside, path.join("hardlink.json")).unwrap();
        for name in ["link.json", "hardlink.json"] {
            assert!(store.read(name).is_err());
            assert!(store.replace(name, "", b"secret").is_err());
        }
        assert_eq!(std::fs::read(outside).unwrap(), b"untouched");
        assert!(path
            .join("link.json")
            .symlink_metadata()
            .unwrap()
            .file_type()
            .is_symlink());
    }

    #[cfg(unix)]
    #[test]
    fn directory_rename_cannot_redirect_a_locked_store() {
        use std::os::unix::fs::DirBuilderExt;
        let directory = tempfile::tempdir().unwrap();
        let path = directory.path().join("private");
        let moved = directory.path().join("retained");
        let store = Store::lock(&path, true).unwrap();
        std::fs::rename(&path, &moved).unwrap();
        std::fs::DirBuilder::new()
            .mode(0o700)
            .create(&path)
            .unwrap();
        store.replace("device.json", "", b"owned identity").unwrap();
        assert!(!path.join("device.json").exists());
        assert_eq!(
            std::fs::read(moved.join("device.json")).unwrap(),
            b"owned identity"
        );
    }
}
