//! Read-only exact launch binding gate. Hold native writer locks through the
//! caller's metadata transaction; this never starts or resumes an agent.
use super::*;

pub(crate) fn with_launch_binding<T>(
    name: &str,
    check: impl FnOnce(&Value) -> std::result::Result<T, anyhow::Error>,
) -> anyhow::Result<T> {
    local_sessions().map_err(anyhow::Error::msg)?;
    let _writer = lock(None).map_err(anyhow::Error::msg)?;
    let _dsh = lock(Some(&root().join("dsh/bindings.lock"))).map_err(anyhow::Error::msg)?;
    let _opencode=lock(Some(&root().join("opencode/bindings.lock"))).map_err(anyhow::Error::msg)?;
    let record = if opencode::exists(name) {
        let record=opencode::binding(name).map_err(anyhow::Error::msg)?;
        let inspected=opencode::inspect(name,None).map_err(anyhow::Error::msg)?;
        anyhow::ensure!(record["paused"]!=true && inspected["state"]=="running","Native OpenCode launch is no longer live");record
    } else if dsh::exists(name) {
        let record = dsh::binding(name).map_err(anyhow::Error::msg)?;
        let inspected = dsh::process_rpc(
            &record,
            "inspect",
            json!({
                "sessionId":record["conversation_id"],"includeProcesses":false
            }),
        )
        .map_err(anyhow::Error::msg)?;
        // The bridge inspection alone can return a saved, stopped conversation.
        let listed = dsh::process_rpc(&record, "list", json!({})).map_err(anyhow::Error::msg)?;
        let row = listed["items"]
            .as_array()
            .and_then(|rows| {
                rows.iter()
                    .find(|row| row["sessionId"] == record["conversation_id"])
            })
            .ok_or_else(|| anyhow::anyhow!("Native launch is no longer present"))?;
        anyhow::ensure!(
            record["paused"] != true
                && inspected["live"]
                    .as_bool()
                    .unwrap_or(row["agentAvailable"] == true),
            "Native launch is no longer live"
        );
        anyhow::ensure!(!inspected.is_null(), "Native launch cannot be inspected");
        record
    } else {
        let record = read(name).map_err(anyhow::Error::msg)?;
        anyhow::ensure!(
            matches(&record, live().map_err(anyhow::Error::msg)?.get(name)),
            "Native launch is no longer live"
        );
        record
    };
    anyhow::ensure!(
        string(&record, "archive_id").is_empty() && record["state"] != "archived",
        "Archived launches cannot be assigned"
    );
    check(&record)
}
