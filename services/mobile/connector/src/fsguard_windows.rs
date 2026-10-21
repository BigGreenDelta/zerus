//! Windows equivalent of the private journal contract. File::try_lock uses
//! Windows file locks; the Unix implementation still uses Python's flock.
use crate::error::{ConnectorError, Result, IN_USE, PRIVATE_DIRECTORY, PRIVATE_FILE, UNSAFE_FILE};
use std::fs::{File, Metadata};
use std::path::Path;
pub fn euid() -> u32 {
    0
} // SID checks happen on the actual opened handle.
pub fn create_directory(path: &Path) -> std::io::Result<()> {
    zerus_windows_private::directory(path, true).map(drop)
}
pub fn check_private_directory(path: &Path) -> Result<()> {
    zerus_windows_private::directory(path, false)
        .map(drop)
        .map_err(|_| ConnectorError::new(PRIVATE_DIRECTORY))
}
pub fn private_file(path: &Path, create: bool) -> Result<Option<File>> {
    match zerus_windows_private::open_file(path, true, create) {
        Ok(file) => Ok(Some(file)),
        Err(e) if !create && e.kind() == std::io::ErrorKind::NotFound => Ok(None),
        Err(_) => Err(ConnectorError::new(UNSAFE_FILE)),
    }
}
pub fn inspect_private_file(path: &Path) -> std::result::Result<Option<Metadata>, &'static str> {
    match zerus_windows_private::open_file(path, false, false) {
        Ok(file) => file.metadata().map(Some).map_err(|_| PRIVATE_FILE),
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => Ok(None),
        Err(_) => Err(UNSAFE_FILE),
    }
}
pub fn lock_exclusive(file: &File) -> Result<()> {
    file.try_lock().map_err(|_| ConnectorError::new(IN_USE))
}
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum LockState {
    Absent,
    Held,
    Free,
    Unknown,
}
impl LockState {
    pub fn name(self) -> &'static str {
        match self {
            Self::Absent => "absent",
            Self::Held => "held",
            Self::Free => "free",
            Self::Unknown => "unknown",
        }
    }
}
pub fn probe_lock(path: &Path) -> LockState {
    let file = match zerus_windows_private::open_file(path, false, false) {
        Ok(file) => file,
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => return LockState::Absent,
        Err(_) => return LockState::Unknown,
    };
    match file.try_lock_shared() {
        Ok(()) => LockState::Free,
        Err(std::fs::TryLockError::WouldBlock) => LockState::Held,
        Err(_) => LockState::Unknown,
    }
}
