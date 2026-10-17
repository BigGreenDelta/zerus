//! Private relay files. The JSON configuration (and an FCM service account)
//! is read only from an owner-only regular file, never through a symlink. The
//! SQLite store, until the relay is PostgreSQL-only, refuses a shared parent
//! directory instead of loosening or tightening it. `ORACLE.md` maps the
//! Python test these replace.
use std::{
    fs::{self, DirBuilder, Permissions},
    os::unix::fs::{symlink, DirBuilderExt, PermissionsExt},
    path::Path,
};
use zerus_relay::config::{read_private, sqlite_file, Config};

fn mode(path: &Path) -> u32 {
    fs::metadata(path).unwrap().permissions().mode() & 0o777
}

#[test]
fn the_configuration_is_an_owner_only_regular_file() {
    let dir = tempfile::tempdir().unwrap();
    let path = dir.path().join("relay.json");
    fs::write(&path, b"{\"background\":false}").unwrap();
    fs::set_permissions(&path, Permissions::from_mode(0o600)).unwrap();
    assert!(!Config::read(Some(&path)).unwrap().background);
    for shared in [0o640, 0o604, 0o620, 0o602] {
        fs::set_permissions(&path, Permissions::from_mode(shared)).unwrap();
        assert_eq!(
            read_private(&path, 1024).unwrap_err().0.as_u16(),
            400,
            "{shared:o}"
        );
        assert!(Config::read(Some(&path)).is_err());
    }
    fs::set_permissions(&path, Permissions::from_mode(0o600)).unwrap();
    assert!(read_private(&path, 4).is_err());
    // A symlink is never followed, and its target is left as it was.
    let secret = dir.path().join("other-file");
    fs::write(&secret, "do not modify").unwrap();
    fs::set_permissions(&secret, Permissions::from_mode(0o600)).unwrap();
    let link = dir.path().join("link.json");
    symlink(&secret, &link).unwrap();
    assert!(read_private(&link, 1024).is_err());
    assert_eq!(fs::read_to_string(&secret).unwrap(), "do not modify");
    assert!(read_private(dir.path(), 1024).is_err());
}

#[test]
fn the_sqlite_store_is_private_and_never_changes_its_parent() {
    let root = tempfile::tempdir().unwrap();
    let private = root.path().join("private");
    let store = private.join("relay.sqlite3");
    let file = sqlite_file(&store).unwrap();
    assert_eq!(mode(&private), 0o700);
    assert_eq!(mode(&store), 0o600);
    drop(file);
    let public = root.path().join("public");
    DirBuilder::new().mode(0o755).create(&public).unwrap();
    fs::set_permissions(&public, Permissions::from_mode(0o755)).unwrap();
    assert!(sqlite_file(&public.join("relay.sqlite3")).is_err());
    assert_eq!(mode(&public), 0o755);
    assert!(!public.join("relay.sqlite3").exists());
    let secret = root.path().join("other-file");
    fs::write(&secret, "do not modify").unwrap();
    symlink(&secret, private.join("symlink.sqlite3")).unwrap();
    assert!(sqlite_file(&private.join("symlink.sqlite3")).is_err());
    assert_eq!(fs::read_to_string(&secret).unwrap(), "do not modify");
}
