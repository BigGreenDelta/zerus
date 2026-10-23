//! Bounded private snapshots and compare-and-swap connector configuration.
use crate::{
    error::{ConnectorError, Result},
    fsguard, private_state,
};
use serde_json::{json, Value};
use std::{
    fs::File,
    io::{Read, Write},
    path::Path,
};

fn refused() -> ConnectorError {
    ConnectorError::new(
        "Workspace configuration is unavailable or changed; reload before continuing",
    )
}
pub fn read_private(directory: &Path, name: &str) -> Result<Option<Vec<u8>>> {
    if name.is_empty()
        || name.len() > 128
        || name == "."
        || name == ".."
        || !name
            .bytes()
            .all(|b| b.is_ascii_alphanumeric() || b"._-".contains(&b))
    {
        return Err(refused());
    }
    fsguard::check_private_directory(directory).map_err(|_| refused())?;
    let Some(file) = fsguard::private_file(&directory.join(name), false).map_err(|_| refused())?
    else {
        return Ok(None);
    };
    read_bounded(file).map(Some)
}

fn read_bounded(file: File) -> Result<Vec<u8>> {
    let mut bytes = Vec::new();
    file.take(private_state::MAX_STATE_BYTES as u64 + 1)
        .read_to_end(&mut bytes)
        .map_err(|_| refused())?;
    if bytes.len() > private_state::MAX_STATE_BYTES {
        return Err(refused());
    }
    Ok(bytes)
}

#[cfg(unix)]
fn directory(path: &Path) -> Result<File> {
    use rustix::fs::{Mode, OFlags};
    use std::os::unix::fs::MetadataExt;
    let file: File = rustix::fs::open(
        path,
        OFlags::RDONLY | OFlags::DIRECTORY | OFlags::NOFOLLOW | OFlags::CLOEXEC,
        Mode::empty(),
    )
    .map_err(|_| refused())?
    .into();
    let metadata = file.metadata().map_err(|_| refused())?;
    if metadata.uid() != fsguard::euid() || metadata.mode() & 0o022 != 0 {
        return Err(refused());
    }
    Ok(file)
}
#[cfg(unix)]
fn read_at(directory: &File, name: &std::ffi::OsStr) -> Result<Option<Vec<u8>>> {
    use rustix::fs::{Mode, OFlags};
    use std::os::unix::fs::MetadataExt;
    let opened = rustix::fs::openat(
        directory,
        name,
        OFlags::RDONLY | OFlags::NOFOLLOW | OFlags::NONBLOCK | OFlags::CLOEXEC,
        Mode::empty(),
    );
    let file: File = match opened {
        Ok(file) => file.into(),
        Err(error) if error == rustix::io::Errno::NOENT => return Ok(None),
        Err(_) => return Err(refused()),
    };
    let metadata = file.metadata().map_err(|_| refused())?;
    if !metadata.is_file()
        || metadata.uid() != fsguard::euid()
        || metadata.mode() & 0o022 != 0
        || metadata.nlink() != 1
    {
        return Err(refused());
    }
    read_bounded(file).map(Some)
}
pub fn read_config(path: &Path) -> Result<(Value, String)> {
    let parent = path.parent().ok_or_else(refused)?;
    if !parent.exists() {
        return Ok((json!({}), String::new()));
    }
    #[cfg(unix)]
    let bytes = read_at(&directory(parent)?, path.file_name().ok_or_else(refused)?)?;
    #[cfg(windows)]
    let bytes = match zerus_windows_private::open_file(path, false, false) {
        Ok(file) => Some(read_bounded(file)?),
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => None,
        Err(_) => return Err(refused()),
    };
    let revision = private_state::Store::revision(bytes.as_deref());
    let value = match bytes {
        Some(bytes) => serde_json::from_slice::<Value>(&bytes).map_err(|_| refused())?,
        None => json!({}),
    };
    if !value.is_object() {
        return Err(refused());
    }
    Ok((value, revision))
}
pub fn replace_config(path: &Path, expected: &str, value: &Value) -> Result<String> {
    let bytes = serde_json::to_vec_pretty(value).map_err(|_| refused())?;
    if bytes.len() > private_state::MAX_STATE_BYTES {
        return Err(refused());
    }
    let parent = path.parent().ok_or_else(refused)?;
    if !parent.exists() {
        fsguard::create_directory(parent).map_err(|_| refused())?;
    }
    #[cfg(unix)]
    {
        use rustix::fs::{Mode, OFlags};
        let dir = directory(parent)?;
        let name = path.file_name().ok_or_else(refused)?;
        let lock: File = rustix::fs::openat(
            &dir,
            ".workspace-config.lock",
            OFlags::RDWR | OFlags::CREATE | OFlags::NOFOLLOW | OFlags::CLOEXEC,
            Mode::from_raw_mode(0o600),
        )
        .map_err(|_| refused())?
        .into();
        use std::os::unix::fs::MetadataExt;
        let meta = lock.metadata().map_err(|_| refused())?;
        if !meta.is_file()
            || meta.uid() != fsguard::euid()
            || meta.mode() & 0o077 != 0
            || meta.nlink() != 1
        {
            return Err(refused());
        }
        fsguard::lock_exclusive(&lock).map_err(|_| refused())?;
        if private_state::Store::revision(read_at(&dir, name)?.as_deref()) != expected {
            return Err(refused());
        }
        let temporary = format!(
            ".workspace-config-{}",
            private_state::random_id().map_err(|_| refused())?
        );
        let mut file: File = rustix::fs::openat(
            &dir,
            temporary.as_str(),
            OFlags::WRONLY | OFlags::CREATE | OFlags::EXCL | OFlags::NOFOLLOW | OFlags::CLOEXEC,
            Mode::from_raw_mode(0o600),
        )
        .map_err(|_| refused())?
        .into();
        let result = (|| {
            file.write_all(&bytes).map_err(|_| refused())?;
            file.sync_all().map_err(|_| refused())?;
            drop(file);
            if private_state::Store::revision(read_at(&dir, name)?.as_deref()) != expected {
                return Err(refused());
            }
            rustix::fs::renameat(&dir, temporary.as_str(), &dir, name).map_err(|_| refused())?;
            dir.sync_all().map_err(|_| refused())?;
            Ok(private_state::Store::revision(Some(&bytes)))
        })();
        if result.is_err() {
            let _ = rustix::fs::unlinkat(&dir, temporary.as_str(), rustix::fs::AtFlags::empty());
        }
        result
    }
    #[cfg(windows)]
    {
        let _guard = zerus_windows_private::directory(parent, false).map_err(|_| refused())?;
        let lock =
            zerus_windows_private::open_file(&parent.join(".workspace-config.lock"), true, true)
                .map_err(|_| refused())?;
        fsguard::lock_exclusive(&lock).map_err(|_| refused())?;
        if read_config(path)?.1 != expected {
            return Err(refused());
        }
        let temporary = parent.join(format!(
            ".workspace-config-{}",
            private_state::random_id().map_err(|_| refused())?
        ));
        let mut file = std::fs::OpenOptions::new()
            .write(true)
            .create_new(true)
            .open(&temporary)
            .map_err(|_| refused())?;
        zerus_windows_private::verify(&file, false).map_err(|_| refused())?;
        file.write_all(&bytes).map_err(|_| refused())?;
        file.sync_all().map_err(|_| refused())?;
        drop(file);
        if read_config(path)?.1 != expected {
            return Err(refused());
        }
        zerus_windows_private::replace(&temporary, path).map_err(|_| refused())?;
        Ok(private_state::Store::revision(Some(&bytes)))
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn config_cas_preserves_unknown_fields_and_never_follows_links() {
        let directory = tempfile::tempdir().unwrap();
        let config = directory.path().join("connector.json");
        let original = json!({"hgs_path":"/opt/example/hgs","extension":{"keep":true}});
        let revision = replace_config(&config, "", &original).unwrap();
        assert_eq!(
            read_config(&config).unwrap(),
            (original.clone(), revision.clone())
        );
        assert!(replace_config(&config, "", &json!({"replace":true})).is_err());
        assert_eq!(read_config(&config).unwrap().0, original);
        #[cfg(unix)]
        {
            use std::os::unix::fs::symlink;
            let link = directory.path().join("linked.json");
            symlink(&config, &link).unwrap();
            assert!(read_config(&link).is_err());
            assert!(replace_config(&link, "", &json!({})).is_err());
            assert_eq!(read_config(&config).unwrap().0, original);
        }
    }
}
