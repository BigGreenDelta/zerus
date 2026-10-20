//! OpenCode favorite models, stored on the agent computer beside the hgs
//! account catalog (`opencode-favorites.json`), so every desktop and phone
//! connected to this computer sees one list. A change is an explicit user
//! action with a request identity: a retried identity returns its first
//! receipt and never applies over a newer change.
use super::super::*;
use super::catalog::valid_model;
use std::io::Read;

pub(crate) const MAX_FAVORITES: usize = 64;
const MAX_FILE: u64 = 64 * 1024;
const MAX_RECEIPT: u64 = 4096;
const MAX_RECEIPTS: usize = 256;
pub(super) const INVALID: &str =
    "Invalid opencode-favorites.json; remove it to reset OpenCode favorites";

pub(crate) fn favorites_path(config_dir: &Path) -> PathBuf {
    config_dir.join("opencode-favorites.json")
}

fn bounded(path: &Path, limit: u64) -> std::io::Result<Option<Vec<u8>>> {
    let file = match std::fs::File::open(path) {
        Ok(file) => file,
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => return Ok(None),
        Err(e) => return Err(e),
    };
    let mut bytes = Vec::new();
    file.take(limit + 1).read_to_end(&mut bytes)?;
    Ok(Some(bytes))
}

/// The stored list; `Ok(None)` without a file, `Err` for a damaged one.
fn read(path: &Path) -> Result<Option<Vec<String>>> {
    let Some(bytes) = bounded(path, MAX_FILE).map_err(|_| INVALID)? else {
        return Ok(None);
    };
    if bytes.len() as u64 > MAX_FILE {
        return Err(INVALID.into());
    }
    let value: Value = serde_json::from_slice(&bytes).map_err(|_| INVALID)?;
    let models = value["models"]
        .as_array()
        .filter(|_| value["version"] == 1)
        .ok_or(INVALID)?;
    let mut list: Vec<String> = Vec::new();
    for model in models {
        let model = model.as_str().filter(|m| valid_model(m)).ok_or(INVALID)?;
        if !list.iter().any(|known| known == model) {
            list.push(model.to_owned());
        }
    }
    if list.len() > MAX_FAVORITES {
        return Err(INVALID.into());
    }
    Ok(Some(list))
}

pub(crate) fn set_favorite(
    config_dir: &Path,
    model: &str,
    favorite: bool,
    request_id: &str,
) -> Result<Value> {
    set_favorite_at(
        &favorites_path(config_dir),
        &root().join("opencode/favorite-requests"),
        model,
        favorite,
        request_id,
    )
}

const UNREADABLE: &str = "This request identity has an unreadable record; choose again";

/// `Ok(None)` only for a missing receipt; a receipt that exists but cannot be
/// read as a valid record of this identity fails closed.
fn read_receipt(path: &Path, request: &str) -> Result<Option<Value>> {
    // Only nothing at the path is a new request. A symlink (dangling or not),
    // a directory or any other non-regular entry is unreadable.
    match std::fs::symlink_metadata(path) {
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => return Ok(None),
        Err(_) => return Err(UNREADABLE.into()),
        Ok(meta) if !meta.file_type().is_file() => return Err(UNREADABLE.into()),
        Ok(_) => {}
    }
    // It was there a moment ago; vanishing now is unreadable, never new.
    let Some(bytes) = bounded(path, MAX_RECEIPT).map_err(|_| UNREADABLE)? else {
        return Err(UNREADABLE.into());
    };
    if bytes.len() as u64 > MAX_RECEIPT {
        return Err(UNREADABLE.into());
    }
    let value: Value = serde_json::from_slice(&bytes).map_err(|_| UNREADABLE)?;
    let valid = matches!(value["status"].as_str(), Some("pending" | "applied"))
        && value["request_id"] == request
        && value["model"].is_string()
        && value["favorite"].is_boolean();
    if valid {
        Ok(Some(value))
    } else {
        Err(UNREADABLE.into())
    }
}

/// Keeps the newest receipts; `keep` (the one just written) always stays, even
/// when coarse timestamps make it look as old as others.
fn prune(receipts: &Path, keep: &Path) {
    let Ok(entries) = std::fs::read_dir(receipts) else {
        return;
    };
    let mut files: Vec<(std::time::SystemTime, PathBuf)> = entries
        .filter_map(|e| e.ok())
        .filter(|e| e.path().extension().is_some_and(|x| x == "json") && e.path() != keep)
        .filter_map(|e| Some((e.metadata().ok()?.modified().ok()?, e.path())))
        .collect();
    if files.len() < MAX_RECEIPTS {
        return;
    }
    files.sort();
    for (_, path) in &files[..files.len() + 1 - MAX_RECEIPTS] {
        let _ = std::fs::remove_file(path);
    }
}

/// Checks a change before anything is read or written; returns the normalized
/// request identity. A dry run uses it too.
pub(crate) fn validate(model: &str, request_id: &str) -> Result<String> {
    let request = uuid::Uuid::parse_str(request_id)
        .map_err(|_| "Invalid favorite request identity")?
        .hyphenated()
        .to_string();
    if !valid_model(model) {
        return Err("Choose provider/model for OpenCode".into());
    }
    Ok(request)
}

pub(super) fn set_favorite_at(
    path: &Path,
    receipts: &Path,
    model: &str,
    favorite: bool,
    request_id: &str,
) -> Result<Value> {
    let request = validate(model, request_id)?;
    let receipt_path = receipts.join(format!("{request}.json"));
    let record = |status: &str| json!({"status": status, "request_id": request, "model": model, "favorite": favorite});
    let answer = |list: &[String]| {
        let mut receipt = record("applied");
        receipt["favorites"] = json!(list);
        receipt
    };
    if let Some(previous) = read_receipt(&receipt_path, &request)? {
        if previous["model"] != model || previous["favorite"] != favorite {
            return Err("This request identity belongs to another favorite change".into());
        }
        let current = read(path)?.unwrap_or_default();
        if previous["status"] == "applied" {
            return Ok(answer(&current));
        }
        if current.iter().any(|m| m == model) == favorite {
            atomic(&receipt_path, &record("applied").to_string())?;
            return Ok(answer(&current));
        }
        return Err("This favorite change was interrupted; refresh and choose again".into());
    }
    let mut list = read(path)?.unwrap_or_default();
    let present = list.iter().any(|m| m == model);
    if favorite && !present {
        if list.len() >= MAX_FAVORITES {
            return Err("OpenCode keeps at most 64 favorite models; remove one first".into());
        }
        list.push(model.to_owned());
    } else if !favorite {
        list.retain(|m| m != model);
    }
    atomic(&receipt_path, &record("pending").to_string())?;
    atomic(path, &json!({"version": 1, "models": list}).to_string())?;
    atomic(&receipt_path, &record("applied").to_string())?;
    prune(receipts, &receipt_path);
    Ok(answer(&list))
}

/// The stored favorites for the catalog; a missing or damaged file lists none.
pub(crate) fn load_favorites(config_dir: &Path) -> Vec<String> {
    read(&favorites_path(config_dir))
        .ok()
        .flatten()
        .unwrap_or_default()
}

#[cfg(test)]
mod tests {
    use super::*;

    const R1: &str = "6f1c1a0e-4d8b-4b7e-9a51-3c2d1e0f9a01";
    const R2: &str = "6f1c1a0e-4d8b-4b7e-9a51-3c2d1e0f9a02";
    const R3: &str = "6f1c1a0e-4d8b-4b7e-9a51-3c2d1e0f9a03";

    fn paths() -> (tempfile::TempDir, PathBuf, PathBuf) {
        let dir = tempfile::tempdir().unwrap();
        let (file, receipts) = (
            dir.path().join("opencode-favorites.json"),
            dir.path().join("receipts"),
        );
        (dir, file, receipts)
    }

    fn stored(path: &Path) -> Vec<String> {
        read(path).unwrap().unwrap_or_default()
    }

    #[test]
    fn favorites_keep_their_order_and_never_repeat() {
        let (_dir, file, receipts) = paths();
        set_favorite_at(&file, &receipts, "groq/openai/gpt-oss-20b", true, R1).unwrap();
        let receipt = set_favorite_at(&file, &receipts, "fixture/local", true, R2).unwrap();
        assert_eq!(
            receipt["favorites"],
            json!(["groq/openai/gpt-oss-20b", "fixture/local"])
        );
        assert_eq!(receipt["status"], "applied");
        let again = set_favorite_at(&file, &receipts, "fixture/local", true, R3).unwrap();
        assert_eq!(
            again["favorites"],
            json!(["groq/openai/gpt-oss-20b", "fixture/local"])
        );
        assert_eq!(stored(&file), ["groq/openai/gpt-oss-20b", "fixture/local"]);
    }

    #[test]
    fn a_retried_request_returns_its_receipt_without_reapplying() {
        let (_dir, file, receipts) = paths();
        set_favorite_at(&file, &receipts, "fixture/local", true, R1).unwrap();
        set_favorite_at(&file, &receipts, "fixture/local", false, R2).unwrap();
        let replay = set_favorite_at(&file, &receipts, "fixture/local", true, R1).unwrap();
        assert_eq!(
            (replay["status"].as_str(), replay["favorite"].as_bool()),
            (Some("applied"), Some(true))
        );
        assert_eq!(replay["favorites"], json!([]));
        assert!(stored(&file).is_empty());
    }

    #[test]
    fn an_identity_is_bound_to_one_change() {
        let (_dir, file, receipts) = paths();
        set_favorite_at(&file, &receipts, "fixture/local", true, R1).unwrap();
        let error = set_favorite_at(&file, &receipts, "fixture/other", true, R1).unwrap_err();
        assert_eq!(
            error,
            "This request identity belongs to another favorite change"
        );
        assert!(set_favorite_at(&file, &receipts, "fixture/local", false, R1).is_err());
    }

    #[test]
    fn an_interrupted_change_is_never_replayed_over_a_newer_state() {
        let (_dir, file, receipts) = paths();
        std::fs::create_dir_all(&receipts).unwrap();
        let pending = json!({"status": "pending", "request_id": R3, "model": "fixture/local", "favorite": true});
        std::fs::write(receipts.join(format!("{R3}.json")), pending.to_string()).unwrap();
        let error = set_favorite_at(&file, &receipts, "fixture/local", true, R3).unwrap_err();
        assert_eq!(
            error,
            "This favorite change was interrupted; refresh and choose again"
        );
        assert!(stored(&file).is_empty());
        // When the file already shows the change, the receipt completes.
        set_favorite_at(&file, &receipts, "fixture/local", true, R1).unwrap();
        assert_eq!(
            set_favorite_at(&file, &receipts, "fixture/local", true, R3).unwrap()["status"],
            "applied"
        );
    }

    #[test]
    fn at_most_sixty_four_favorites() {
        let (_dir, file, receipts) = paths();
        for i in 0..MAX_FAVORITES {
            set_favorite_at(
                &file,
                &receipts,
                &format!("p/m{i}"),
                true,
                &uuid::Uuid::new_v4().to_string(),
            )
            .unwrap();
        }
        let error = set_favorite_at(
            &file,
            &receipts,
            "p/one-more",
            true,
            &uuid::Uuid::new_v4().to_string(),
        )
        .unwrap_err();
        assert_eq!(
            error,
            "OpenCode keeps at most 64 favorite models; remove one first"
        );
        // Removing still works at the bound.
        assert!(set_favorite_at(
            &file,
            &receipts,
            "p/m0",
            false,
            &uuid::Uuid::new_v4().to_string()
        )
        .is_ok());
    }

    #[test]
    fn invalid_models_and_identities_are_refused() {
        let (_dir, file, receipts) = paths();
        for model in ["fixture", "a b/c", "p/ x", "/m", "p/"] {
            assert!(
                set_favorite_at(&file, &receipts, model, true, R1).is_err(),
                "{model}"
            );
        }
        assert_eq!(
            set_favorite_at(&file, &receipts, "p/m", true, "not-a-uuid").unwrap_err(),
            "Invalid favorite request identity"
        );
        assert!(!file.exists() && !receipts.exists());
    }

    #[test]
    fn a_damaged_file_is_refused_and_never_overwritten() {
        let (_dir, file, receipts) = paths();
        std::fs::write(&file, b"{not json").unwrap();
        assert_eq!(read(&file).unwrap_err(), INVALID);
        assert_eq!(
            set_favorite_at(&file, &receipts, "p/m", true, R1).unwrap_err(),
            INVALID
        );
        assert_eq!(std::fs::read(&file).unwrap(), b"{not json");
        std::fs::write(&file, vec![b' '; 70_000]).unwrap();
        assert_eq!(read(&file).unwrap_err(), INVALID);
    }

    #[test]
    fn receipts_are_pruned_to_the_newest_256_never_the_one_just_written() {
        let (_dir, file, receipts) = paths();
        let mut last = String::new();
        for i in 0..260 {
            last = uuid::Uuid::new_v4().to_string();
            set_favorite_at(&file, &receipts, "p/m", i % 2 == 0, &last).unwrap();
            assert!(
                receipts.join(format!("{last}.json")).is_file(),
                "receipt {i} was pruned at once"
            );
        }
        assert_eq!(std::fs::read_dir(&receipts).unwrap().count(), 256);
        assert!(receipts.join(format!("{last}.json")).is_file());
    }

    #[test]
    fn g1_an_applied_receipt_refuses_another_intent_and_leaves_the_file() {
        let (_dir, file, receipts) = paths();
        set_favorite_at(&file, &receipts, "fixture/local", true, R1).unwrap();
        let before = std::fs::read(&file).unwrap();
        for (model, favorite) in [
            ("fixture/other", true),
            ("fixture/local", false),
            ("fixture/other", false),
        ] {
            let error = set_favorite_at(&file, &receipts, model, favorite, R1).unwrap_err();
            assert_eq!(
                error,
                "This request identity belongs to another favorite change"
            );
        }
        assert_eq!(std::fs::read(&file).unwrap(), before);
        let kept = read_receipt(&receipts.join(format!("{R1}.json")), R1)
            .unwrap()
            .unwrap();
        assert_eq!(
            (
                kept["status"].as_str(),
                kept["model"].as_str(),
                kept["favorite"].as_bool()
            ),
            (Some("applied"), Some("fixture/local"), Some(true))
        );
    }

    #[test]
    fn g1_a_pending_receipt_refuses_another_intent_and_leaves_the_file() {
        let (_dir, file, receipts) = paths();
        set_favorite_at(&file, &receipts, "fixture/keep", true, R1).unwrap();
        let before = std::fs::read(&file).unwrap();
        std::fs::create_dir_all(&receipts).unwrap();
        let pending = json!({"status": "pending", "request_id": R3, "model": "fixture/local", "favorite": true});
        let pending_path = receipts.join(format!("{R3}.json"));
        std::fs::write(&pending_path, pending.to_string()).unwrap();
        for (model, favorite) in [("fixture/other", true), ("fixture/local", false)] {
            let error = set_favorite_at(&file, &receipts, model, favorite, R3).unwrap_err();
            assert_eq!(
                error,
                "This request identity belongs to another favorite change"
            );
        }
        assert_eq!(std::fs::read(&file).unwrap(), before);
        assert_eq!(read_receipt(&pending_path, R3).unwrap().unwrap(), pending);
    }

    #[test]
    fn identities_must_be_uuids_and_are_normalized_to_one_receipt() {
        let (_dir, file, receipts) = paths();
        for bad in [
            "",
            "../escape",
            "6f1c1a0e",
            "6f1c1a0e-4d8b-4b7e-9a51-3c2d1e0f9a01/x",
            "6f1c1a0e-4d8b-4b7e-9a51-3c2d1e0f9a0g",
        ] {
            assert_eq!(
                set_favorite_at(&file, &receipts, "p/m", true, bad).unwrap_err(),
                "Invalid favorite request identity",
                "{bad}"
            );
        }
        assert!(!file.exists() && !receipts.exists());
        // Any parsable spelling names the same receipt, so a differently cased retry replays.
        set_favorite_at(&file, &receipts, "p/m", true, R1).unwrap();
        set_favorite_at(&file, &receipts, "p/m", false, R2).unwrap();
        let upper = set_favorite_at(&file, &receipts, "p/m", true, &R1.to_uppercase()).unwrap();
        assert_eq!(upper["request_id"], R1);
        assert_eq!(upper["favorites"], json!([]));
        assert_eq!(std::fs::read_dir(&receipts).unwrap().count(), 2);
    }

    #[test]
    fn an_unreadable_receipt_fails_closed_and_leaves_the_file() {
        let (_dir, file, receipts) = paths();
        set_favorite_at(&file, &receipts, "fixture/keep", true, R1).unwrap();
        let before = std::fs::read(&file).unwrap();
        let path = receipts.join(format!("{R3}.json"));
        let oversize = format!("{{\"pad\":\"{}\"}}", "x".repeat(5000));
        for content in ["{not json", oversize.as_str(), "{}", "[]", ""] {
            std::fs::write(&path, content).unwrap();
            let error = set_favorite_at(&file, &receipts, "fixture/other", true, R3).unwrap_err();
            assert_eq!(error, UNREADABLE, "{content:.20}");
            assert_eq!(std::fs::read(&file).unwrap(), before);
            assert_eq!(std::fs::read_to_string(&path).unwrap(), content);
        }
        // A directory in the receipt's place is unreadable too.
        std::fs::remove_file(&path).unwrap();
        std::fs::create_dir(&path).unwrap();
        assert_eq!(
            set_favorite_at(&file, &receipts, "fixture/other", true, R3).unwrap_err(),
            UNREADABLE
        );
        assert_eq!(std::fs::read(&file).unwrap(), before);
    }

    /// A receipt path that holds a symlink is never "no receipt": opening a
    /// dangling one reports NotFound, which must not admit a new request.
    #[cfg(unix)]
    #[test]
    fn a_dangling_receipt_symlink_is_unreadable() {
        let (dir, file, receipts) = paths();
        set_favorite_at(&file, &receipts, "fixture/keep", true, R1).unwrap();
        let before = std::fs::read(&file).unwrap();
        let path = receipts.join(format!("{R3}.json"));
        std::os::unix::fs::symlink(dir.path().join("missing.json"), &path).unwrap();
        let outcome = set_favorite_at(&file, &receipts, "fixture/other", true, R3);
        assert_eq!(outcome.unwrap_err(), UNREADABLE);
        assert_eq!(std::fs::read(&file).unwrap(), before);
        assert!(std::fs::symlink_metadata(&path)
            .unwrap()
            .file_type()
            .is_symlink());
    }

    /// A symlink to a well-formed receipt for this identity is not admitted
    /// either: only a regular file at the receipt path is a receipt.
    #[cfg(unix)]
    #[test]
    fn a_live_receipt_symlink_is_unreadable() {
        let (dir, file, receipts) = paths();
        set_favorite_at(&file, &receipts, "fixture/keep", true, R1).unwrap();
        let before = std::fs::read(&file).unwrap();
        let elsewhere = dir.path().join("elsewhere.json");
        let receipt = json!({"status": "applied", "request_id": R3, "model": "fixture/other", "favorite": true});
        std::fs::write(&elsewhere, receipt.to_string()).unwrap();
        let path = receipts.join(format!("{R3}.json"));
        std::os::unix::fs::symlink(&elsewhere, &path).unwrap();
        let outcome = set_favorite_at(&file, &receipts, "fixture/other", true, R3);
        assert_eq!(outcome.unwrap_err(), UNREADABLE);
        assert_eq!(std::fs::read(&file).unwrap(), before);
        assert_eq!(
            std::fs::read(&elsewhere).unwrap(),
            receipt.to_string().into_bytes()
        );
    }

    #[test]
    fn listing_never_fails_on_a_missing_or_damaged_file() {
        let dir = tempfile::tempdir().unwrap();
        assert!(load_favorites(dir.path()).is_empty());
        std::fs::write(favorites_path(dir.path()), b"{not json").unwrap();
        assert!(load_favorites(dir.path()).is_empty());
        std::fs::write(
            favorites_path(dir.path()),
            json!({"version": 1, "models": ["groq/openai/gpt-oss-20b"]}).to_string(),
        )
        .unwrap();
        assert_eq!(load_favorites(dir.path()), ["groq/openai/gpt-oss-20b"]);
    }
}
