//! Workspace-scoped physical computers and explicitly advertised one-hop routes.
use crate::{
    alerts::recovery_failed,
    args,
    db::{f, i, now, s, Db, Tx},
    error::{Error, Result},
    json::{canonical, digest},
};
use serde_json::{json, Value};
use std::collections::{BTreeMap, HashSet};
use uuid::Uuid;

pub async fn create(t: &mut Tx, w: &str, name: &str, id: Option<&str>) -> Result<String> {
    capacity(
        t,
        "SELECT count(*) AS n FROM computers WHERE workspace_id=?",
        w,
        1,
        2048,
    )
    .await?;
    let id = id
        .map(str::to_owned)
        .unwrap_or_else(|| Uuid::new_v4().to_string());
    t.exec(
        "INSERT INTO computers(id,workspace_id,name) VALUES(?,?,?)",
        args![&id, w, name],
    )
    .await?;
    Ok(id)
}
async fn capacity(t: &mut Tx, sql: &str, w: &str, extra: i64, max: i64) -> Result<()> {
    let r = t.one(sql, args![w]).await?.ok_or(Error::BAD)?;
    if i(&r, "n") + extra > max {
        Err(Error::BUSY)
    } else {
        Ok(())
    }
}
async fn route_capacity(t: &mut Tx, w: &str, extra: i64) -> Result<()> {
    capacity(t,"SELECT count(*) AS n FROM computer_routes r JOIN nodes n ON n.id=r.gateway_id WHERE n.workspace_id=?",w,extra,4096).await
}
async fn alias(t: &mut Tx, w: &str, a: &str, c: &str) -> Result<()> {
    let exists = t
        .one(
            "SELECT computer_id FROM computer_aliases WHERE workspace_id=? AND alias=?",
            args![w, a],
        )
        .await?;
    if exists.is_none() {
        capacity(
            t,
            "SELECT count(*) AS n FROM computer_aliases WHERE workspace_id=?",
            w,
            1,
            4096,
        )
        .await?;
    }
    t.exec("INSERT INTO computer_aliases(workspace_id,alias,computer_id) VALUES(?,?,?) ON CONFLICT(workspace_id,alias) DO UPDATE SET computer_id=excluded.computer_id",args![w,a,c]).await?;
    Ok(())
}
pub async fn enroll(t: &mut Tx, w: &str, n: &str, name: &str) -> Result<()> {
    route_capacity(t, w, 1).await?;
    create(t, w, name, Some(n)).await?;
    alias(t, w, n, n).await?;
    t.exec("UPDATE nodes SET computer_id=? WHERE id=?", args![n, n])
        .await?;
    t.exec("INSERT INTO computer_routes(gateway_id,route_id,computer_id,local,active,online,guarded,name) VALUES(?,?,?,1,1,1,0,?)",args![n,n,n,name]).await?;
    Ok(())
}
async fn native(t: &mut Tx, w: &str, m: &str, name: &str) -> Result<Option<String>> {
    if let Some(r)=t.one("SELECT c.* FROM native_computers m JOIN computers c ON c.id=m.computer_id WHERE m.workspace_id=? AND m.machine_id=?",args![w,m]).await? {return Ok(if i(&r,"revoked")==0{Some(s(&r,"id").into())}else{None});}
    let c = create(t, w, name, None).await?;
    t.exec("UPDATE computers SET machine_id=? WHERE id=?", args![m, &c])
        .await?;
    t.exec(
        "INSERT INTO native_computers(workspace_id,machine_id,computer_id) VALUES(?,?,?)",
        args![w, m, &c],
    )
    .await?;
    Ok(Some(c))
}
pub async fn invalidate(
    db: &Db,
    t: &mut Tx,
    predicate: &str,
    a: &[crate::db::Arg<'_>],
    error: &str,
) -> Result<()> {
    let rows=t.all(&format!("SELECT id,workspace_id,state,result_bytes,reserved_bytes FROM requests WHERE ({predicate}) AND state IN ('queued','claimed') FOR UPDATE"),a).await?;
    for r in rows {
        let (state, message) = if r["state"] == "queued" {
            ("failed", error)
        } else {
            ("uncertain", "gateway route withdrawn after claim")
        };
        t.exec("UPDATE requests SET state=?,error=?,result_bytes=?,reserved_bytes=0,updated=? WHERE id=?",args![state,message,message.len(),now(),&r["id"]]).await?;
        db.bytes(
            t,
            s(&r, "workspace_id"),
            message.len() as i64 - i(&r, "result_bytes") - i(&r, "reserved_bytes"),
        )
        .await?;
        t.exec(
            "UPDATE workspace_usage SET active=active-1 WHERE workspace_id=?",
            args![&r["workspace_id"]],
        )
        .await?;
    }
    Ok(())
}
pub async fn gateway(
    db: &Db,
    t: &mut Tx,
    w: &str,
    n: &str,
    machine: Option<&str>,
    peers: &[Value],
    guarded: bool,
) -> Result<()> {
    let node=t.one("SELECT id,name,computer_id,machine_id FROM nodes WHERE id=? AND workspace_id=? AND revoked=0 FOR UPDATE",args![n,w]).await?.ok_or(Error::UNAUTHORIZED)?;
    if machine.is_some() && !node["machine_id"].is_null() && machine != node["machine_id"].as_str()
    {
        return Err(Error::CONFLICT);
    }
    let machine = machine.or(node["machine_id"].as_str());
    let machine_value = machine.map_or(Value::Null, |m| json!(m));
    let mut computer = s(&node, "computer_id").to_string();
    let own = t
        .one(
            "SELECT revoked,exposed FROM computers WHERE id=?",
            args![&computer],
        )
        .await?
        .ok_or(Error::CONFLICT)?;
    let mut revoked = i(&own, "revoked") != 0;
    if let Some(m) = machine {
        let mapped=t.one("SELECT c.* FROM native_computers m JOIN computers c ON c.id=m.computer_id WHERE m.workspace_id=? AND m.machine_id=?",args![w,m]).await?;
        if let Some(row) = mapped {
            revoked |= i(&row, "revoked") != 0;
            if row["id"] != computer && i(&own, "exposed") == 0 {
                crate::notification_highwater::merge(db, t, w, &computer, s(&row, "id")).await?;
                computer = s(&row, "id").into();
                alias(t, w, n, &computer).await?;
            }
        } else {
            t.exec(
                "INSERT INTO native_computers(workspace_id,machine_id,computer_id) VALUES(?,?,?)",
                args![w, m, &computer],
            )
            .await?;
        }
        t.exec(
            "UPDATE computers SET machine_id=?,revoked=? WHERE id=?",
            args![m, revoked, &computer],
        )
        .await?;
    }
    t.exec(
        "UPDATE nodes SET computer_id=?,machine_id=? WHERE id=?",
        args![&computer, &machine_value, n],
    )
    .await?;
    t.exec("UPDATE computer_routes SET computer_id=?,machine_id=?,guarded=?,active=?,online=? WHERE gateway_id=? AND local=1",args![&computer,&machine_value,guarded&&!revoked,!revoked,!revoked,n]).await?;
    if !guarded {
        invalidate(
            db,
            t,
            "node_id=? AND target_machine_id IS NOT NULL",
            args![n],
            "gateway route withdrawn before delivery",
        )
        .await?;
    }
    let existing=t.all("SELECT route_id,machine_id FROM computer_routes WHERE gateway_id=? AND local=0 FOR UPDATE",args![n]).await?;
    for old in &existing {
        let new = peers.iter().find(|p| p["route_id"] == old["route_id"]);
        if new
            .is_none_or(|p| p["online"] != true || p["machine_id"] != old["machine_id"] || !guarded)
        {
            invalidate(
                db,
                t,
                "node_id=? AND route_id=?",
                args![n, &old["route_id"]],
                "gateway route withdrawn before delivery",
            )
            .await?;
        }
        if new.is_none_or(|p| p["machine_id"] != old["machine_id"]) {
            t.exec(
                "DELETE FROM computer_routes WHERE gateway_id=? AND route_id=?",
                args![n, &old["route_id"]],
            )
            .await?;
        }
    }
    for peer in peers {
        if peer["machine_id"] == machine_value || peer["route_id"] == n {
            return Err(Error::BAD);
        }
        let target = native(t, w, s(peer, "machine_id"), s(peer, "name")).await?;
        let Some(target) = target else {
            invalidate(
                db,
                t,
                "node_id=? AND route_id=?",
                args![n, &peer["route_id"]],
                "computer revoked before delivery",
            )
            .await?;
            t.exec(
                "DELETE FROM computer_routes WHERE gateway_id=? AND route_id=? AND local=0",
                args![n, &peer["route_id"]],
            )
            .await?;
            continue;
        };
        if t.one(
            "SELECT route_id FROM computer_routes WHERE gateway_id=? AND route_id=?",
            args![n, &peer["route_id"]],
        )
        .await?
        .is_none()
        {
            route_capacity(t, w, 1).await?;
        }
        t.exec("INSERT INTO computer_routes(gateway_id,route_id,computer_id,machine_id,local,active,online,guarded,name) VALUES(?,?,?,?,0,1,?,?,?) ON CONFLICT(gateway_id,route_id) DO UPDATE SET online=excluded.online,guarded=excluded.guarded,name=excluded.name",args![n,&peer["route_id"],&target,&peer["machine_id"],&peer["online"],guarded,&peer["name"]]).await?;
        if peer["online"] != true {
            // Keep last_seen: it remains the last accepted peer heartbeat for
            // display. Clearing the snapshot withdraws freshness evidence.
            t.exec("UPDATE computer_routes SET snapshot=NULL,snapshot_hash=NULL WHERE gateway_id=? AND route_id=?",args![n,&peer["route_id"]]).await?;
        }
    }
    Ok(())
}
/// `last_seen` is freshness evidence: a peer route's time counts only while it
/// stores a snapshot accepted since the gateway last marked it online.
/// `last_seen_at` is the display time, which an offline peer route retains.
pub fn available(r: &Value) -> bool {
    if i(r, "gateway_revoked") != 0 || i(r, "revoked") != 0 || i(r, "active") == 0 {
        return false;
    }
    if i(r, "local") != 0 && r["machine_id"].is_null() {
        return true;
    }
    i(r, "online") != 0
        && i(r, "guarded") != 0
        && !r["gateway_seen"].is_null()
        && !r["last_seen"].is_null()
        && now() - f(r, "gateway_seen") < 45.0
        && now() - f(r, "last_seen") < 45.0
}
async fn candidates(t: &mut Tx, w: &str, c: Option<&str>) -> Result<Vec<Value>> {
    let select="SELECT c.id,c.name,c.exposed,c.revoked,r.gateway_id,r.route_id,r.machine_id,r.local,r.active,r.online,r.guarded,CASE WHEN r.local=1 THEN n.last_seen WHEN r.snapshot_hash IS NOT NULL THEN r.last_seen END AS last_seen,CASE WHEN r.local=1 THEN n.last_seen ELSE r.last_seen END AS last_seen_at,n.last_seen AS gateway_seen,n.revoked AS gateway_revoked,n.name AS gateway_name,COALESCE(BYTES(CASE WHEN r.local=1 THEN n.snapshot ELSE r.snapshot END AS_BYTES),0) AS snapshot_bytes FROM computers c JOIN computer_routes r ON r.computer_id=c.id JOIN nodes n ON n.id=r.gateway_id WHERE c.workspace_id=? AND c.revoked=0 AND n.revoked=0 AND r.active=1";
    if let Some(c) = c {
        t.all(&format!("{select} AND c.id=?"), args![w, c]).await
    } else {
        t.all(select, args![w]).await
    }
}
pub async fn resolve(t: &mut Tx, w: &str, a: &str) -> Result<Option<String>> {
    if let Some(r) = t
        .one(
            "SELECT computer_id FROM computer_aliases WHERE workspace_id=? AND alias=?",
            args![w, a],
        )
        .await?
    {
        return Ok(Some(s(&r, "computer_id").into()));
    }
    Ok(t.one(
        "SELECT id FROM computers WHERE workspace_id=? AND id=?",
        args![w, a],
    )
    .await?
    .map(|r| s(&r, "id").into()))
}
fn order(r: &Value) -> (bool, bool, &str, &str) {
    (
        !available(r),
        i(r, "local") == 0,
        s(r, "gateway_id"),
        s(r, "route_id"),
    )
}
async fn snapshot(t: &mut Tx, r: &Value) -> Result<Value> {
    t.one("SELECT CASE WHEN r.local=1 THEN n.snapshot ELSE r.snapshot END AS snapshot FROM computer_routes r JOIN nodes n ON n.id=r.gateway_id WHERE r.gateway_id=? AND r.route_id=?",args![&r["gateway_id"],&r["route_id"]]).await?.map(|r|r["snapshot"].clone()).ok_or(Error::CONFLICT)
}
pub async fn select(t: &mut Tx, w: &str, a: &str) -> Result<Option<Value>> {
    let Some(c) = resolve(t, w, a).await? else {
        return Ok(None);
    };
    let mut rows = candidates(t, w, Some(&c)).await?;
    rows.retain(available);
    rows.sort_by(|a, b| order(a).cmp(&order(b)));
    if rows.is_empty() {
        return Ok(None);
    }
    let mut r = rows.remove(0);
    let stored = snapshot(t, &r).await?;
    if let Some(encoded) = stored.as_str() {
        let mut value = crate::json::parse(encoded.as_bytes(), 32, 50000)?;
        crate::notification_highwater::overlay(t, &c, &mut value).await?;
        r["snapshot"] = json!(canonical(&value)?);
    } else {
        r["snapshot"] = stored;
    }
    Ok(Some(r))
}
pub async fn frozen(t: &mut Tx, r: &Value, allow: bool) -> Result<bool> {
    if !r["target_machine_id"].is_null() && !allow {
        return Ok(false);
    }
    if r["target_computer_id"].is_null() {
        return Ok(true);
    }
    Ok(
        candidates(t, s(r, "workspace_id"), Some(s(r, "target_computer_id")))
            .await?
            .iter()
            .any(|c| {
                c["gateway_id"] == r["node_id"]
                    && (if i(c, "local") != 0 {
                        Value::Null
                    } else {
                        c["route_id"].clone()
                    }) == r["route_id"]
                    && c["machine_id"] == r["target_machine_id"]
                    && available(c)
            }),
    )
}
pub async fn catalog(t: &mut Tx, w: &str, maximum: usize, online_timeout: u64) -> Result<Value> {
    // Caller holds workspace then node/route share locks across size preflight.
    let rows = candidates(t, w, None).await?;
    let mut chosen: BTreeMap<String, Value> = BTreeMap::new();
    for r in rows {
        let key = s(&r, "id").to_string();
        if chosen.get(&key).is_none_or(|old| order(&r) < order(old)) {
            chosen.insert(key, r);
        }
    }
    let aliases = t
        .all(
            "SELECT alias,computer_id FROM computer_aliases WHERE workspace_id=? ORDER BY alias",
            args![w],
        )
        .await?;
    let size: usize = aliases
        .iter()
        .map(|r| s(r, "alias").len() + 4)
        .sum::<usize>()
        + chosen
            .values()
            .map(|r| i(r, "snapshot_bytes") as usize + s(r, "name").len() + 512)
            .sum::<usize>();
    if size > maximum {
        return Err(Error::LARGE);
    }
    let mut selected: Vec<_> = chosen.into_values().collect();
    selected.sort_by(|a, b| (s(a, "name"), s(a, "id")).cmp(&(s(b, "name"), s(b, "id"))));
    if selected.is_empty() {
        return Ok(json!({"computers":[]}));
    }
    let placeholders = std::iter::repeat_n("(?,?)", selected.len())
        .collect::<Vec<_>>()
        .join(",");
    let arguments: Vec<_> = selected
        .iter()
        .flat_map(|r| {
            [
                crate::db::Arg::from(&r["gateway_id"]),
                crate::db::Arg::from(&r["route_id"]),
            ]
        })
        .collect();
    let snapshots=t.all(&format!("WITH selected(gateway_id,route_id) AS (VALUES {placeholders}) SELECT r.gateway_id,r.route_id,CASE WHEN r.local=1 THEN n.snapshot ELSE r.snapshot END AS snapshot FROM computer_routes r JOIN nodes n ON n.id=r.gateway_id JOIN selected s ON s.gateway_id=r.gateway_id AND s.route_id=r.route_id"),&arguments).await?;
    let mut snapshots: BTreeMap<_, _> = snapshots
        .into_iter()
        .map(|mut r| {
            (
                (s(&r, "gateway_id").to_owned(), s(&r, "route_id").to_owned()),
                r["snapshot"].take(),
            )
        })
        .collect();
    let mut aliases_by_computer: BTreeMap<String, Vec<Value>> = BTreeMap::new();
    for mut a in aliases {
        aliases_by_computer
            .entry(s(&a, "computer_id").into())
            .or_default()
            .push(a["alias"].take());
    }
    let mut result = Vec::with_capacity(selected.len());
    let mut projected_bytes = 16usize;
    for r in &selected {
        let snap = snapshots
            .remove(&(s(r, "gateway_id").to_owned(), s(r, "route_id").to_owned()))
            .ok_or(Error::CONFLICT)?;
        let mut parsed = if let Some(s) = snap.as_str() {
            crate::json::parse(s.as_bytes(), 32, 50000)?
        } else {
            Value::Null
        };
        crate::notification_highwater::overlay(t, s(r, "id"), &mut parsed).await?;
        let item = json!({"id":r["id"],"name":r["name"],"online":available(r)&&!r["last_seen"].is_null()&&now()-f(r,"last_seen")<online_timeout as f64,"last_seen_at":r["last_seen_at"],"snapshot":parsed,"machine_id":r["machine_id"],"aliases":aliases_by_computer.remove(s(r,"id")).unwrap_or_default(),"via":if i(r,"local")!=0{Value::Null}else{json!({"gateway_id":r["gateway_id"],"gateway_name":r["gateway_name"]})}});
        projected_bytes += canonical(&item)?.len() + 1;
        if projected_bytes > maximum {
            return Err(Error::LARGE);
        }
        result.push(item);
    }
    let unexposed: Vec<_> = selected
        .iter()
        .filter(|r| i(r, "exposed") == 0)
        .map(|r| crate::db::Arg::from(&r["id"]))
        .collect();
    if !unexposed.is_empty() {
        let placeholders = std::iter::repeat_n("?", unexposed.len())
            .collect::<Vec<_>>()
            .join(",");
        t.exec(
            &format!("UPDATE computers SET exposed=1 WHERE id IN ({placeholders})"),
            &unexposed,
        )
        .await?;
    }
    let result = json!({"computers":result});
    if canonical(&result)?.len() > maximum {
        return Err(Error::LARGE);
    }
    Ok(result)
}
pub async fn revoke_computer(db: &Db, t: &mut Tx, c: &Value) -> Result<()> {
    let rows = if c["machine_id"].is_null() {
        vec![c.clone()]
    } else {
        t.all(
            "SELECT id FROM computers WHERE workspace_id=? AND machine_id=?",
            args![&c["workspace_id"], &c["machine_id"]],
        )
        .await?
    };
    for r in rows {
        invalidate(db,t,"target_computer_id=? OR (target_computer_id IS NULL AND node_id IN (SELECT gateway_id FROM computer_routes WHERE computer_id=? AND local=1))",args![&r["id"],&r["id"]],"computer revoked before delivery").await?;
        t.exec("UPDATE nodes SET manifest_hash=NULL WHERE id IN (SELECT gateway_id FROM computer_routes WHERE computer_id=?)",args![&r["id"]]).await?;
        t.exec("UPDATE computers SET revoked=1 WHERE id=?", args![&r["id"]])
            .await?;
        t.exec("UPDATE computer_routes SET active=0,snapshot=NULL,snapshot_hash=NULL,last_seen=NULL WHERE computer_id=?",args![&r["id"]]).await?;
    }
    Ok(())
}
/// Archived rows never produce events or alerts.
pub fn archived(v: &Value) -> bool {
    v["archived"] == true || v["kind"] == "archive" || v["state"] == "archived"
}
pub(crate) fn explicit_wait(v: &Value) -> bool {
    let w = &v["coordination_wait"];
    w["schema"] == 1
        && w["status"] == "ok"
        && w["count"].as_u64().is_some_and(|n| n > 0 && n <= 1024)
        && v["run_id"].as_str().is_some_and(|v| !v.is_empty())
        && v["conversation_id"].as_str().is_some_and(|v| !v.is_empty())
        && w["run_id"] == v["run_id"]
        && w["conversation_id"] == v["conversation_id"]
}

/// An older listing may finish after a successful preference mutation. Reject
/// that heartbeat so its next fresh listing cannot roll back shared Mute.
pub fn notification_revisions_current(previous: Option<&str>, snapshot: &Value) -> Result<bool> {
    let Some(previous) = previous else {
        return Ok(true);
    };
    let previous = crate::json::parse(previous.as_bytes(), 32, 50000)?;
    let empty = vec![];
    let mut known = BTreeMap::new();
    for before in previous["sessions"].as_array().unwrap_or(&empty) {
        if let Some(preference) = crate::alerts::notification_preferences(before) {
            let key = (
                s(before, "name").split('/').next().unwrap_or_default(),
                s(before, "conversation_id"),
            );
            let entry = known.entry(key).or_insert(preference);
            if preference["revision"].as_i64() > entry["revision"].as_i64() {
                *entry = preference;
            }
        }
    }
    for row in snapshot["sessions"].as_array().unwrap_or(&empty) {
        let key = (
            s(row, "name").split('/').next().unwrap_or_default(),
            s(row, "conversation_id"),
        );
        if let Some(known) = known.get(&key) {
            let Some(incoming) = crate::alerts::notification_preferences(row) else {
                return Ok(false);
            };
            let old_revision = known["revision"].as_i64().unwrap();
            let revision = incoming["revision"].as_i64().unwrap();
            if revision < old_revision || (revision == old_revision && incoming != *known) {
                return Ok(false);
            }
        }
    }
    Ok(true)
}

pub fn event_changes(previous: Option<&str>, snapshot: &Value) -> Result<Vec<(String, String)>> {
    let Some(previous) = previous else {
        return Ok(vec![]);
    };
    let previous = crate::json::parse(previous.as_bytes(), 32, 50000)?;
    fn questions(v: &Value) -> u64 {
        v["pending_question_count"].as_u64().unwrap_or(0)
    }
    fn identity(v: &Value) -> (&Value, &Value) {
        (&v["run_id"], &v["conversation_id"])
    }
    fn pending(v: &Value) -> Result<HashSet<String>> {
        v["mobile_attention"]
            .as_array()
            .unwrap_or(&vec![])
            .iter()
            .filter(|v| v.is_object())
            .map(canonical)
            .collect()
    }
    fn question(v: &Value) -> Result<String> {
        canonical(&json!([
            v["attention_id"],
            v.get("question_request").unwrap_or(&v["question_requests"])
        ]))
    }
    let empty = vec![];
    let old = previous["sessions"].as_array().unwrap_or(&empty);
    let mut events = vec![];
    for v in snapshot["sessions"].as_array().unwrap_or(&empty) {
        if !v["name"].is_string() || archived(v) {
            continue;
        }
        let before = old
            .iter()
            .find(|b| b["name"] == v["name"] && !archived(b))
            .unwrap_or(&Value::Null);
        if v["notification_preferences"] != before["notification_preferences"] {
            // Plain refresh event: withdraw existing notices on every online
            // client, without inventing a new attention/completion edge.
            events.push((s(v, "name").into(), "notification_preferences".into()));
        }
        let p = pending(v)?;
        let b = pending(before)?;
        let phase = s(v, "phase");
        let changed = identity(v) != identity(before) || question(v)? != question(before)?;
        // Every condition a phone notifies on must surface as an event here.
        let kind = if !p.is_subset(&b)
            || (!p.is_empty() && identity(v) != identity(before))
            || (["approval", "input"].contains(&phase)
                && (v["phase"] != before["phase"] || changed))
            || (questions(v) > 0 && (questions(v) > questions(before) || changed))
        {
            "attention"
        } else if (phase == "error" && (before["phase"] != "error" || changed))
            || (recovery_failed(v)
                && (!recovery_failed(before)
                    || v["recovery"]["request_id"] != before["recovery"]["request_id"]
                    || changed))
        {
            "error"
        } else if v["activity"] == "idle"
            && !explicit_wait(v)
            && before["activity"] == "busy"
            && identity(v) == identity(before)
        {
            "completed"
        } else {
            continue;
        };
        events.push((s(v, "name").into(), kind.into()));
    }
    Ok(events)
}

pub fn manifest(machine: &Value, peers: &Value, guarded: bool) -> Result<String> {
    Ok(digest(canonical(
        &json!({"machine_id":machine,"peers":peers,"guarded":guarded}),
    )?))
}

#[cfg(test)]
mod wait_tests {
    use super::*;
    #[test]
    fn independent_wait_is_not_a_completed_turn_and_scope_reuse_does_not_hide_completion() {
        let old=json!({"sessions":[{"name":"codex/example/coordinator","run_id":"run","conversation_id":"conversation","activity":"busy","phase":"tool"}]}).to_string();
        let mut row = json!({"name":"codex/example/coordinator","run_id":"run","conversation_id":"conversation","activity":"idle","phase":"idle",
            "coordination_wait":{"schema":1,"status":"ok","count":1,"run_id":"run","conversation_id":"conversation"}});
        assert!(event_changes(Some(&old), &json!({"sessions":[row]}))
            .unwrap()
            .is_empty());
        row["coordination_wait"]["run_id"] = json!("another-run");
        assert_eq!(
            event_changes(Some(&old), &json!({"sessions":[row]})).unwrap(),
            vec![("codex/example/coordinator".into(), "completed".into())]
        );
        row["coordination_wait"]["run_id"] = json!("run");
        row["coordination_wait"]["count"] = json!(0);
        assert_eq!(
            event_changes(Some(&old), &json!({"sessions":[row]}))
                .unwrap()
                .len(),
            1
        );
    }
}
