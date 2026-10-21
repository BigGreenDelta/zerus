//! Permanent physical-computer preference highwater. Routes never own revisions.
//! Call writers with the payload shard and workspace locked, before route locks.
use crate::{
    alerts, args,
    db::{i, Db, Tx},
    error::{Error, Result},
    json::{canonical, parse},
    protocol::MIB,
};
use serde_json::{json, Value};
use std::collections::BTreeMap;

const MAX_SCOPES: usize = 4096;
const MAX_BYTES: usize = MIB;
type Ledger = BTreeMap<(String, String), Value>;
fn key(row: &Value) -> Option<(String, String)> {
    let provider = row["name"].as_str()?.split('/').next()?;
    let conversation = row["conversation_id"].as_str()?;
    let valid =
        |s: &str| !s.is_empty() && s.len() <= 256 && !s.chars().any(|c| c < ' ' || c == '\u{7f}');
    (valid(provider) && valid(conversation)).then(|| (provider.into(), conversation.into()))
}
fn insert(map: &mut Ledger, key: (String, String), value: Value, reject_older: bool) -> Result<()> {
    if canonical(&value)?.len() > 384 * 1024 {
        return Err(Error::LARGE);
    }
    if let Some(old) = map.get(&key) {
        let revision = value["revision"].as_i64().ok_or(Error::BAD)?;
        let before = old["revision"].as_i64().ok_or(Error::BAD)?;
        if revision == before && value != *old {
            return Err(Error::CONFLICT);
        }
        if revision < before {
            return if reject_older {
                Err(Error::CONFLICT)
            } else {
                Ok(())
            };
        }
    }
    map.insert(key, value);
    if map.len() > MAX_SCOPES {
        return Err(Error::LARGE);
    }
    Ok(())
}
fn encode(map: &Ledger) -> Result<String> {
    let rows: Vec<_> = map
        .iter()
        .map(|((provider, conversation), preferences)| {
            json!({
        "provider":provider,"conversation_id":conversation,"preferences":preferences})
        })
        .collect();
    let encoded = canonical(&json!(rows))?;
    if encoded.len() > MAX_BYTES {
        return Err(Error::LARGE);
    }
    parse(encoded.as_bytes(), 32, 50000)?; // Never persist a map its bounded loader cannot read.
    Ok(encoded)
}
async fn load(t: &mut Tx, computer: &str) -> Result<(Ledger, i64)> {
    let Some(row) = t.one("SELECT CASE WHEN payload_bytes<=1048576+512 THEN preferences END AS preferences,payload_bytes FROM computer_notification_preferences WHERE computer_id=?", args![computer]).await? else { return Ok((Ledger::new(), 0)); };
    let bytes = row["preferences"].as_str().ok_or(Error::LARGE)?;
    if bytes.len() > MAX_BYTES {
        return Err(Error::LARGE);
    }
    let parsed = parse(bytes.as_bytes(), 32, 50000)?;
    let mut map = Ledger::new();
    for entry in parsed.as_array().ok_or(Error::BAD)? {
        let provider = entry["provider"].as_str().ok_or(Error::BAD)?;
        let conversation = entry["conversation_id"].as_str().ok_or(Error::BAD)?;
        let row = json!({"name":provider,"conversation_id":conversation,"notification_preferences":entry["preferences"]});
        let k = key(&row).ok_or(Error::BAD)?;
        let pref = alerts::notification_preferences(&row).ok_or(Error::BAD)?;
        if map.contains_key(&k) {
            return Err(Error::CONFLICT);
        }
        insert(&mut map, k, pref.clone(), true)?;
    }
    Ok((map, i(&row, "payload_bytes")))
}
async fn save(
    db: &Db,
    t: &mut Tx,
    workspace: &str,
    computer: &str,
    map: &Ledger,
    old: i64,
    credit: i64,
) -> Result<()> {
    let encoded = encode(map)?;
    let bytes = encoded.len() as i64 + workspace.len() as i64 + computer.len() as i64;
    let delta = bytes - old - credit;
    if delta > 0 {
        let usage = t
            .one(
                "SELECT payload_bytes FROM workspace_usage WHERE workspace_id=?",
                args![workspace],
            )
            .await?
            .ok_or(Error::BAD)?;
        let shard = t
            .one(
                "SELECT payload_bytes FROM payload_shards WHERE id=?",
                args![db.shard(workspace)],
            )
            .await?
            .ok_or(Error::BAD)?;
        if i(&usage, "payload_bytes") + delta > db.cfg.max_queue_bytes
            || i(&shard, "payload_bytes") + delta > db.cfg.global_max_bytes / db.cfg.quota_shards
        {
            return Err(Error::BUSY);
        }
    }
    t.exec("INSERT INTO computer_notification_preferences(computer_id,workspace_id,preferences,payload_bytes) VALUES(?,?,?,?) ON CONFLICT(computer_id) DO UPDATE SET preferences=excluded.preferences,payload_bytes=excluded.payload_bytes",args![computer,workspace,&encoded,bytes]).await?;
    db.bytes(t, workspace, delta).await
}
/// Empty/old hosts keep the unchanged-snapshot shortcut; preferences must first
/// pass the permanent ledger even if this route already stores identical bytes.
pub fn present(snapshot: &Value) -> bool {
    snapshot["sessions"].as_array().is_some_and(|rows| {
        rows.iter()
            .any(|r| alerts::notification_preferences(r).is_some())
    })
}
pub async fn accept(
    db: &Db,
    t: &mut Tx,
    workspace: &str,
    computer: &str,
    snapshot: &Value,
) -> Result<()> {
    let (mut map, bytes) = load(t, computer).await?;
    let before = map.clone();
    let mut incoming = Ledger::new();
    for row in snapshot["sessions"].as_array().unwrap_or(&vec![]) {
        if let Some(k) = key(row) {
            if let Some(pref) = alerts::notification_preferences(row) {
                insert(&mut incoming, k, pref.clone(), false)?;
            } else if map.contains_key(&k) {
                return Err(Error::CONFLICT);
            }
        }
    }
    for (k, pref) in incoming {
        insert(&mut map, k, pref, true)?;
    }
    if map != before {
        save(db, t, workspace, computer, &map, bytes, 0).await?;
    }
    Ok(())
}
/// Only the full confirmed preference changes. No native/read/route identity is
/// promoted, and absent conversations never gain synthetic session rows.
pub async fn overlay(t: &mut Tx, computer: &str, snapshot: &mut Value) -> Result<()> {
    let (map, _) = load(t, computer).await?;
    if !snapshot.is_object() {
        return Ok(());
    }
    // Repeated scopes are legal in an old snapshot. Preflight replacement
    // costs before cloning even one preference into those rows.
    let mut projected = canonical(snapshot)?.len();
    if projected > MIB {
        return Err(Error::LARGE);
    }
    let sizes = map
        .iter()
        .map(|(scope, pref)| Ok((scope.clone(), canonical(pref)?.len())))
        .collect::<Result<BTreeMap<_, _>>>()?;
    for row in snapshot["sessions"].as_array().unwrap_or(&vec![]) {
        if let Some(replacement) = key(row).and_then(|k| sizes.get(&k)).copied() {
            if let Some(old) = row.get("notification_preferences") {
                projected = projected
                    .checked_sub(canonical(old)?.len())
                    .and_then(|n| n.checked_add(replacement))
                    .ok_or(Error::LARGE)?;
            } else {
                projected = projected
                    .checked_add(r#""notification_preferences":"#.len() + replacement + 1)
                    .ok_or(Error::LARGE)?;
            }
        }
    }
    if projected > MIB {
        return Err(Error::LARGE);
    }
    if let Some(rows) = snapshot["sessions"].as_array_mut() {
        for row in rows {
            if let Some(pref) = key(row).and_then(|k| map.get(&k)) {
                row["notification_preferences"] = pref.clone();
            }
        }
    }
    let encoded = canonical(snapshot)?;
    if encoded.len() > MIB {
        return Err(Error::LARGE);
    }
    parse(encoded.as_bytes(), 32, 50000)?;
    Ok(())
}
/// Consolidating an unexposed enrollment preserves both histories and returns
/// the retired row's exact quota credit in the same registry transaction.
pub async fn merge(db: &Db, t: &mut Tx, workspace: &str, old: &str, target: &str) -> Result<()> {
    if old == target {
        return Ok(());
    }
    let (source, credit) = load(t, old).await?;
    if source.is_empty() {
        return Ok(());
    }
    let (mut map, bytes) = load(t, target).await?;
    for (k, pref) in source {
        insert(&mut map, k, pref, false)?;
    }
    save(db, t, workspace, target, &map, bytes, credit).await?;
    t.exec(
        "DELETE FROM computer_notification_preferences WHERE computer_id=?",
        args![old],
    )
    .await?;
    Ok(())
}
