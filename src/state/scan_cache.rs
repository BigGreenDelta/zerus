//! Results of bounded transcript scans, kept across listings. The desktop lists
//! sessions every two seconds, and stopped or archived conversations keep their
//! transcripts unchanged: re-reading their megabyte tails dominated each listing.
//! An entry is reused only for the same file (device and inode), size and
//! modification time, and the same hgs version. Entries are advisory: anything
//! missing, partial or unexpected only costs a fresh scan.
use super::*;
use sha2::{Digest, Sha256};
use std::fs;

#[cfg(test)]
thread_local! {
    pub(super) static DIR: std::cell::RefCell<Option<PathBuf>> = const { std::cell::RefCell::new(None) };
}

fn dir() -> Option<PathBuf> {
    // Unit tests never touch the real state directory unless they opt in.
    #[cfg(test)]
    return DIR.with(|dir| dir.borrow().clone());
    #[cfg(not(test))]
    Some(root().join("scan-cache"))
}

/// The cached result of `scan` over `path` for `key`, or a fresh scan stored for later.
pub(super) fn cached(kind: &str, path: &Path, key: &str, scan: impl FnOnce() -> Option<Value>) -> Option<Value> {
    let (Some(dir), Ok((identity, meta))) = (dir(), crate::platform::FileIdentity::at(path)) else {
        return scan();
    };
    if !path.is_absolute() || !meta.is_file() {
        return scan();
    }
    let stamp = json!([env!("CARGO_PKG_VERSION"), kind, path, key, identity.device, identity.inode, meta.len(),
        identity.modified, identity.modified_nanos]);
    let entry = dir.join(format!("{:x}.json", Sha256::digest(format!("{kind}\0{}\0{key}", path.display()))));
    let stored = fs::metadata(&entry)
        .ok()
        .filter(|m| m.len() < 1024 * 1024)
        .and_then(|_| fs::read_to_string(&entry).ok())
        .and_then(|text| serde_json::from_str::<Value>(&text).ok())
        .filter(|stored| stored["stamp"] == stamp);
    if let Some(mut stored) = stored {
        return Some(stored["value"].take());
    }
    let value = scan()?;
    // Without fsync: a lost or partial entry is simply scanned again.
    let _ = private_dir(&dir).and_then(|_| {
        let mut temp = tempfile::Builder::new().prefix(".hgs-").tempfile_in(&dir).map_err(|e| e.to_string())?;
        std::io::Write::write_all(&mut temp, json!({"stamp": stamp, "value": value}).to_string().as_bytes())
            .map_err(|e| e.to_string())?;
        crate::platform::persist(temp, &entry).map(|_| ()).map_err(|e| e.to_string())
    });
    Some(value)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn scans_are_reused_until_the_file_changes() {
        let temp = tempfile::tempdir().unwrap();
        DIR.with(|dir| *dir.borrow_mut() = Some(temp.path().join("cache")));
        let path = temp.path().join("transcript.jsonl");
        fs::write(&path, "one\n").unwrap();
        let scans = std::cell::Cell::new(0);
        let scan = || {
            scans.set(scans.get() + 1);
            Some(json!(fs::read_to_string(&path).unwrap().lines().count()))
        };
        assert_eq!(cached("lines", &path, "a", scan), Some(json!(1)));
        assert_eq!(cached("lines", &path, "a", scan), Some(json!(1)));
        assert_eq!(scans.get(), 1);
        // Another key or kind for the same file is its own entry.
        assert_eq!(cached("lines", &path, "b", scan), Some(json!(1)));
        assert_eq!(cached("words", &path, "a", scan), Some(json!(1)));
        assert_eq!(scans.get(), 3);
        fs::write(&path, "one\ntwo\n").unwrap();
        assert_eq!(cached("lines", &path, "a", scan), Some(json!(2)));
        assert_eq!(scans.get(), 4);
        // Failed scans are not stored, and damaged entries are scanned again.
        assert_eq!(cached("none", &path, "a", || None), None);
        assert_eq!(cached("none", &path, "a", scan), Some(json!(2)));
        for entry in fs::read_dir(temp.path().join("cache")).unwrap() {
            fs::write(entry.unwrap().path(), "{").unwrap();
        }
        assert_eq!(cached("lines", &path, "a", scan), Some(json!(2)));
        assert_eq!(scans.get(), 6);
        // Relative paths and missing files are never cached.
        assert_eq!(cached("lines", Path::new("relative.jsonl"), "a", || Some(json!(0))), Some(json!(0)));
        DIR.with(|dir| *dir.borrow_mut() = None);
    }
}
