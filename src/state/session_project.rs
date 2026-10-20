//! Stored identity for logical project placement. Read only the binding fields;
//! no provider history reconciliation, RPC, discovery or process action belongs
//! to this metadata gate.
use super::*;

pub(crate) fn with_session_project_binding<T>(
    name: &str,
    check: impl FnOnce(&Value) -> std::result::Result<T, anyhow::Error>,
) -> anyhow::Result<T> {
    // Keep the launch metadata transaction's established lock order. The
    // provider bindings are immutable for this transaction, including rename.
    let _writer = lock(None).map_err(anyhow::Error::msg)?;
    let _dsh = lock(Some(&root().join("dsh/bindings.lock"))).map_err(anyhow::Error::msg)?;
    let _opencode = opencode::lock_bindings().map_err(anyhow::Error::msg)?;
    let provider = if name.starts_with("opencode/") {
        Some("opencode")
    } else if name.starts_with("dsh/") {
        Some("dsh")
    } else {
        None
    };
    let path = provider.map_or_else(
        || record_path(name),
        |provider| {
            root()
                .join(provider)
                .join("bindings")
                .join(legacy_record_path(name).file_name().unwrap())
        },
    );
    let record = binding_metadata(&path, name, provider)?;
    check(&record)
}
fn binding_metadata(path: &Path, name: &str, provider: Option<&str>) -> anyhow::Result<Value> {
    use std::io::Read;
    let metadata = std::fs::symlink_metadata(path)?;
    anyhow::ensure!(
        metadata.file_type().is_file(),
        "Session binding is not a regular file"
    );
    let mut bytes = Vec::new();
    std::fs::File::open(path)?
        .take(1_048_577)
        .read_to_end(&mut bytes)?;
    anyhow::ensure!(bytes.len() <= 1_048_576, "Session binding is too large");
    let record: Value = serde_json::from_slice(&bytes)?;
    anyhow::ensure!(
        record.is_object() && string(&record, "name") == name,
        "Stored session identity changed"
    );
    let valid = match provider {
        Some("opencode") => {
            record["backend"] == "opencode"
                && opencode::native_id(string(&record, "conversation_id"), "ses_")
        }
        Some("dsh") => {
            record["backend"] == "dsh"
                && string(&record, "conversation_id").starts_with("session-")
                && string(&record, "conversation_id").len() <= 2048
        }
        None => record["version"] == 1 && AGENTS.contains(&string(&record, "agent")),
        _ => false,
    };
    anyhow::ensure!(
        valid && string(&record, "archive_id").is_empty() && record["state"] != "archived",
        "Choose the exact parent session; archived conversations are read-only"
    );
    Ok(record)
}
#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn only_bounded_literal_binding_identity_is_needed_for_paused_metadata() {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("binding.json");
        let name = "claude/example/task";
        let raw = json!({"version":1,"agent":"claude","name":name,"run_id":"run-one","conversation_id":"one","state":"paused","agent_home":"/unread/provider/home"});
        std::fs::write(&path, raw.to_string()).unwrap();
        assert_eq!(binding_metadata(&path, name, None).unwrap(), raw);
        assert!(binding_metadata(&path, "claude/foreign/task", None).is_err());
        assert!(binding_metadata(&path, name, Some("opencode")).is_err());
        std::fs::write(&path, "x".repeat(1_048_577)).unwrap();
        assert!(binding_metadata(&path, name, None).is_err());
        std::fs::write(
            &path,
            json!({"version":1,"agent":"claude","name":name,"archive_id":"archive"}).to_string(),
        )
        .unwrap();
        assert!(binding_metadata(&path, name, None).is_err());
    }
}
