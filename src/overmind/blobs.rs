//! Attachment bytes by SHA-256: immutable, private, written atomically.
use anyhow::{ensure, Result};
use sha2::{Digest, Sha256};
use std::fs;
use std::io::Write;
use std::os::unix::fs::{DirBuilderExt, PermissionsExt};
use std::path::{Path, PathBuf};

pub fn path(dir: &Path, sha256: &str) -> Result<PathBuf> {
    ensure!(
        sha256.len() == 64 && sha256.bytes().all(|b| matches!(b, b'0'..=b'9' | b'a'..=b'f')),
        "invalid attachment digest"
    );
    Ok(dir.join("blobs").join(&sha256[..2]).join(sha256))
}

/// Stores `bytes` and returns their SHA-256. Existing blobs are left as they are.
pub fn put(dir: &Path, bytes: &[u8]) -> Result<String> {
    let sha = format!("{:x}", Sha256::digest(bytes));
    let target = path(dir, &sha)?;
    if target.exists() {
        return Ok(sha);
    }
    let parent = target.parent().expect("blob paths have a parent");
    fs::DirBuilder::new().recursive(true).mode(0o700).create(parent)?;
    let mut temp = tempfile::Builder::new().prefix(".blob-").tempfile_in(parent)?;
    temp.write_all(bytes)?;
    temp.as_file().set_permissions(fs::Permissions::from_mode(0o600))?;
    temp.as_file().sync_all()?;
    temp.persist(&target).map_err(|e| e.error)?;
    fs::File::open(parent)?.sync_all()?;
    Ok(sha)
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::os::unix::fs::PermissionsExt;

    #[test]
    fn blobs_are_private_and_deduplicated() {
        let dir = tempfile::tempdir().unwrap();
        let first = put(dir.path(), b"image").unwrap();
        let second = put(dir.path(), b"image").unwrap();
        assert_eq!(first, second);
        let file = path(dir.path(), &first).unwrap();
        assert_eq!(fs::read(&file).unwrap(), b"image");
        assert_eq!(fs::metadata(&file).unwrap().permissions().mode() & 0o777, 0o600);
        assert!(path(dir.path(), "../etc").is_err());
    }
}
