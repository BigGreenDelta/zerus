use super::{candidates::Accepted, *};
use crate::{
    alerts::{Candidate, Delivery, Presence},
    db::{Arg, Tx},
    protocol::{self, MIB},
    registry,
};
use std::collections::BTreeSet;
impl Store {
    pub async fn heartbeat(&self, c: &Credential, b: &Value, encoded: &str) -> Result<()> {
        if encoded.len() > MIB {
            return Err(Error::LARGE);
        }
        let machine = b.get("machine_id").unwrap_or(&Value::Null);
        let empty = json!([]);
        let peers = b.get("peers").unwrap_or(&empty);
        let guarded = protocol::supports(&b["snapshot"], "gateway_one_hop", "features");
        let manifest = registry::manifest(machine, peers, guarded)?;
        let old = self
            .db
            .one(
                "SELECT machine_id,manifest_hash FROM nodes WHERE id=? AND revoked=0",
                args![&c.id],
            )
            .await?
            .ok_or(Error::UNAUTHORIZED)?;
        if old["manifest_hash"] != manifest {
            let mut t = self.db.begin().await?;
            self.db.payload_usage(&mut t, &c.workspace).await?;
            registry::gateway(
                &self.db,
                &mut t,
                &c.workspace,
                &c.id,
                machine.as_str(),
                peers.as_array().ok_or(Error::BAD)?,
                guarded,
            )
            .await?;
            t.exec(
                "UPDATE nodes SET manifest_hash=? WHERE id=?",
                args![&manifest, &c.id],
            )
            .await?;
            let topic = format!("workspace:{}", c.workspace);
            t.notify(&topic).await?;
            t.commit().await?;
            self.db.signal(topic);
        }
        self.snapshot(c, None, &b["snapshot"], &b["presence"], encoded)
            .await
    }
    pub async fn peer_heartbeat(
        &self,
        c: &Credential,
        route: &str,
        b: &Value,
        encoded: &str,
    ) -> Result<()> {
        self.snapshot(
            c,
            Some((route, s(b, "machine_id"))),
            &b["snapshot"],
            &b["presence"],
            encoded,
        )
        .await
    }
    async fn snapshot(
        &self,
        c: &Credential,
        peer: Option<(&str, &str)>,
        snapshot: &Value,
        presence: &Value,
        encoded: &str,
    ) -> Result<()> {
        if encoded.len() > MIB {
            return Err(Error::LARGE);
        }
        let hash = digest(encoded);
        // Only valid incoming preferences can grow the permanent highwater.
        // Missing/old-host fields still pass accept()'s read-only stale check.
        let needs_payload = crate::notification_highwater::present(snapshot);
        // Older connectors forward the desktop's presence inside the snapshot.
        let received = now();
        let presence = Presence::from_value(presence, received)
            .or_else(|| Presence::from_value(&snapshot["desktop_presence"], received));
        if peer.is_none() && !needs_payload {
            let mut t = self.db.begin().await?;
            if t.exec("UPDATE nodes SET last_seen=? WHERE id=? AND workspace_id=? AND revoked=0 AND (snapshot_hash=? OR EXISTS(SELECT 1 FROM computers c WHERE c.id=nodes.computer_id AND c.revoked=1))",args![now(),&c.id,&c.workspace,&hash]).await? != 0 {
                // Unchanged content: remember presence without rewriting the snapshot.
                let mut held = false;
                if let Some(p) = presence {
                    Self::remember_presence(&mut t, None, &c.id, p).await?;
                    held = t.one("SELECT id FROM alert_candidates WHERE workspace_id=? LIMIT 1",args![&c.workspace]).await?.is_some();
                }
                t.commit().await?;
                if held && self.evaluate_workspace(&c.workspace, None).await.is_err() {
                    eprintln!("relay alert evaluation deferred");
                }
                return Ok(());
            }
            // SQLite has one connection; release it before the full path.
            drop(t);
        }
        for _ in 0..3 {
            let previous=if let Some((route,_))=peer {
                self.db.one("SELECT snapshot,snapshot_hash,computer_id FROM computer_routes WHERE gateway_id=? AND route_id=? AND local=0",args![&c.id,route]).await?
            }else{self.db.one("SELECT snapshot,snapshot_hash,computer_id FROM nodes WHERE id=? AND workspace_id=? AND revoked=0",args![&c.id,&c.workspace]).await?}.ok_or(Error::NOT_FOUND)?;
            if !registry::notification_revisions_current(previous["snapshot"].as_str(), snapshot)? {
                return Err(Error::CONFLICT);
            }
            let changes = registry::event_changes(previous["snapshot"].as_str(), snapshot)?;
            let mut t = self.db.begin().await?;
            if !changes.is_empty() {
                self.event_locks_with_payload(
                    &mut t,
                    &[c.workspace.as_str()],
                    changes.len(),
                    needs_payload.then_some(c.workspace.as_str()),
                )
                .await?;
            } else if needs_payload {
                self.db.payload_usage(&mut t, &c.workspace).await?;
            }
            // Global events (when needed), payload shard, sorted workspaces, then route.
            let current = if let Some((route, machine)) = peer {
                self.authorized_tx(&mut t, c).await?;
                let r=t.one("SELECT snapshot_hash,computer_id,machine_id,active,online,guarded FROM computer_routes WHERE gateway_id=? AND route_id=? AND local=0 FOR UPDATE",args![&c.id,route]).await?.ok_or(Error::NOT_FOUND)?;
                let target = t
                    .one(
                        "SELECT revoked FROM computers WHERE id=?",
                        args![&r["computer_id"]],
                    )
                    .await?
                    .ok_or(Error::NOT_FOUND)?;
                if r["machine_id"] != machine
                    || i(&r, "active") == 0
                    || i(&r, "online") == 0
                    || i(&r, "guarded") == 0
                    || i(&target, "revoked") != 0
                {
                    return Err(Error::CONFLICT);
                }
                r
            } else {
                let r=t.one("SELECT snapshot_hash,computer_id FROM nodes WHERE id=? AND workspace_id=? AND revoked=0 FOR UPDATE",args![&c.id,&c.workspace]).await?.ok_or(Error::UNAUTHORIZED)?;
                let target = t
                    .one(
                        "SELECT revoked FROM computers WHERE id=?",
                        args![&r["computer_id"]],
                    )
                    .await?
                    .ok_or(Error::NOT_FOUND)?;
                if i(&target, "revoked") != 0 {
                    t.exec(
                        "UPDATE nodes SET last_seen=? WHERE id=?",
                        args![now(), &c.id],
                    )
                    .await?;
                    t.commit().await?;
                    return Ok(());
                }
                r
            };
            if current["snapshot_hash"] != previous["snapshot_hash"] {
                continue;
            }
            crate::notification_highwater::accept(
                &self.db,
                &mut t,
                &c.workspace,
                s(&current, "computer_id"),
                snapshot,
            )
            .await?;
            if let Some((route, _)) = peer {
                t.exec("UPDATE computer_routes SET snapshot=?,snapshot_hash=?,last_seen=? WHERE gateway_id=? AND route_id=?",args![encoded,&hash,now(),&c.id,route]).await?;
            } else {
                let delivery =
                    Delivery::from_snapshot(snapshot).map_or(Arg::Null, |d| Arg::Text(d.name()));
                t.exec("UPDATE nodes SET snapshot=?,snapshot_hash=?,last_seen=?,mobile_delivery=? WHERE id=?",args![encoded,&hash,now(),delivery,&c.id]).await?;
            }
            if let Some(p) = presence {
                Self::remember_presence(&mut t, peer.map(|(route, _)| route), &c.id, p).await?;
            }
            let computer = s(&current, "computer_id").to_owned();
            // Held candidates are re-evaluated with the presence and read state just stored.
            let held = !changes.is_empty()
                || t.one(
                    "SELECT id FROM alert_candidates WHERE workspace_id=? LIMIT 1",
                    args![&c.workspace],
                )
                .await?
                .is_some();
            if !changes.is_empty() {
                let candidates: Vec<_> = changes
                    .iter()
                    .map(|(session, kind)| Candidate::capture(snapshot, session, kind))
                    .collect();
                self.publish(&mut t, &c.workspace, &computer, &candidates)
                    .await?;
                let topic = format!("workspace:{}", c.workspace);
                t.notify(&topic).await?;
                t.commit().await?;
                self.db.signal(topic);
            } else {
                t.commit().await?;
            }
            // The snapshot is accepted; maintenance retries a failed evaluation.
            let accepted = Accepted {
                computer: &computer,
                gateway: &c.id,
                // A local route is keyed by its gateway.
                route: peer.map_or(c.id.as_str(), |(route, _)| route),
                snapshot,
            };
            if held
                && self
                    .evaluate_workspace(&c.workspace, Some(accepted))
                    .await
                    .is_err()
            {
                eprintln!("relay alert evaluation deferred");
            }
            return Ok(());
        }
        Err(Error::CONFLICT)
    }
    /// Remembers a machine's latest presence with its receipt time. A heartbeat
    /// without valid presence keeps the stored value, which then goes stale.
    async fn remember_presence(
        t: &mut Tx,
        route: Option<&str>,
        gateway: &str,
        p: Presence,
    ) -> Result<()> {
        if let Some(route) = route {
            t.exec("UPDATE computer_routes SET presence_at=?,presence_until=? WHERE gateway_id=? AND route_id=?",args![p.received,p.away_since,gateway,route]).await?;
        } else {
            t.exec(
                "UPDATE nodes SET presence_at=?,presence_until=? WHERE id=?",
                args![p.received, p.away_since, gateway],
            )
            .await?;
        }
        Ok(())
    }
    /// Locks global event accounting, then every workspace that may gain an
    /// event or wake job or lose its oldest one to a cap, in sorted order.
    pub(super) async fn event_locks(
        &self,
        t: &mut Tx,
        spaces: &[&str],
        count: usize,
    ) -> Result<()> {
        self.event_locks_with_payload(t, spaces, count, None).await
    }
    async fn event_locks_with_payload(
        &self,
        t: &mut Tx,
        spaces: &[&str],
        count: usize,
        payload: Option<&str>,
    ) -> Result<()> {
        let fleet = t
            .one(
                "SELECT events,push_jobs FROM global_usage WHERE id=1 FOR UPDATE",
                &[],
            )
            .await?
            .ok_or(Error::BAD)?;
        let mut workspaces: BTreeSet<String> = spaces.iter().map(|w| (*w).to_string()).collect();
        for r in t
            .all(
                "SELECT workspace_id FROM events ORDER BY id LIMIT ?",
                args![(i(&fleet, "events") + count as i64 - 100000).max(0)],
            )
            .await?
        {
            workspaces.insert(s(&r, "workspace_id").into());
        }
        // At most 1000 registrations per workspace can create jobs during one publication.
        let jobs = i(&fleet, "push_jobs") + 1000 * spaces.len() as i64 - 10000;
        for r in t.all("SELECT d.workspace_id FROM push_jobs j JOIN devices d ON d.id=j.device_id ORDER BY j.id LIMIT ?",args![jobs.max(0)]).await?{workspaces.insert(s(&r,"workspace_id").into());}
        if let Some(w) = payload {
            t.one(
                "SELECT payload_bytes FROM payload_shards WHERE id=? FOR UPDATE",
                args![self.db.shard(w)],
            )
            .await?
            .ok_or(Error::BAD)?;
        }
        for w in workspaces {
            self.db.usage(t, &w).await?;
        }
        Ok(())
    }
    /// Ordinary events refresh phone state; their candidates decide phone wakes.
    async fn publish(
        &self,
        t: &mut Tx,
        w: &str,
        computer: &str,
        candidates: &[Candidate],
    ) -> Result<()> {
        for c in candidates {
            let event = self
                .append_event(t, w, computer, &c.session, &c.kind, None)
                .await?;
            if c.kind == "notification_preferences" {
                // Data-only wakes let sleeping phones withdraw an old card;
                // they never create a user-visible alert candidate.
                self.wake_devices(t, w, &event["id"]).await?;
            }
        }
        let alerts: Vec<_> = candidates
            .iter()
            .filter(|c| matches!(c.kind.as_str(), "attention" | "error" | "completed"))
            .cloned()
            .collect();
        self.record_candidates(t, w, computer, &alerts).await
    }
    /// Appends one event inside the workspace and fleet caps. The caller holds event locks.
    pub(super) async fn append_event(
        &self,
        t: &mut Tx,
        w: &str,
        computer: &str,
        session: &str,
        kind: &str,
        alert_kind: Option<&str>,
    ) -> Result<Value> {
        let usage = t
            .one(
                "SELECT events FROM workspace_usage WHERE workspace_id=?",
                args![w],
            )
            .await?
            .ok_or(Error::BAD)?;
        let fleet = t
            .one("SELECT events FROM global_usage WHERE id=1", &[])
            .await?
            .ok_or(Error::BAD)?;
        let victim = if i(&usage, "events") >= 10000 {
            t.one(
                "SELECT id,workspace_id FROM events WHERE workspace_id=? ORDER BY id LIMIT 1",
                args![w],
            )
            .await?
        } else if i(&fleet, "events") >= 100000 {
            t.one(
                "SELECT id,workspace_id FROM events ORDER BY id LIMIT 1",
                &[],
            )
            .await?
        } else {
            None
        };
        if let Some(v) = victim {
            self.delete_event(t, &v).await?;
        }
        let event=t.one("INSERT INTO events(workspace_id,node_id,session,kind,alert_kind,created) VALUES(?,?,?,?,?,?) RETURNING id",args![w,computer,session,kind,alert_kind.map_or(Arg::Null,Arg::Text),now()]).await?.ok_or(Error::BAD)?;
        t.exec(
            "UPDATE workspace_usage SET events=events+1 WHERE workspace_id=?",
            args![w],
        )
        .await?;
        t.exec("UPDATE global_usage SET events=events+1 WHERE id=1", &[])
            .await?;
        Ok(event)
    }
    /// Coalesces one generic wake per registered device toward the newest alert.
    /// A job whose send is in flight records the newer event too, and
    /// `finish_push` then sends it again instead of deleting it.
    pub(super) async fn wake_devices(&self, t: &mut Tx, w: &str, event_id: &Value) -> Result<()> {
        let payload = canonical(&json!({"event_id":event_id,"kind":"wake"}))?;
        let registrations=t.all("SELECT p.device_id FROM pushes p JOIN devices d ON d.id=p.device_id WHERE d.workspace_id=? AND d.revoked=0 ORDER BY p.device_id LIMIT 1000",args![w]).await?;
        for reg in registrations {
            if let Some(j) = t
                .one(
                    "SELECT id FROM push_jobs WHERE device_id=? ORDER BY id LIMIT 1",
                    args![&reg["device_id"]],
                )
                .await?
            {
                t.exec("UPDATE push_jobs SET event_id=?,payload=?,next_at=CASE WHEN next_at<? THEN next_at ELSE ? END WHERE id=?",args![event_id,&payload,now(),now(),&j["id"]]).await?;
                continue;
            }
            let usage = t
                .one(
                    "SELECT push_jobs FROM workspace_usage WHERE workspace_id=?",
                    args![w],
                )
                .await?
                .ok_or(Error::BAD)?;
            let fleet = t
                .one("SELECT push_jobs FROM global_usage WHERE id=1", &[])
                .await?
                .ok_or(Error::BAD)?;
            let victim = if i(&usage, "push_jobs") >= 1000 {
                t.one("SELECT j.id,d.workspace_id FROM push_jobs j JOIN devices d ON d.id=j.device_id WHERE d.workspace_id=? ORDER BY j.id LIMIT 1",args![w]).await?
            } else if i(&fleet, "push_jobs") >= 10000 {
                t.one("SELECT j.id,d.workspace_id FROM push_jobs j JOIN devices d ON d.id=j.device_id ORDER BY j.id LIMIT 1",&[]).await?
            } else {
                None
            };
            if let Some(j) = victim {
                self.delete_job(t, &j).await?;
            }
            t.exec("INSERT INTO push_jobs(device_id,event_id,payload,next_at,created) VALUES(?,?,?,?,?)",args![&reg["device_id"],event_id,&payload,now(),now()]).await?;
            t.exec(
                "UPDATE workspace_usage SET push_jobs=push_jobs+1 WHERE workspace_id=?",
                args![w],
            )
            .await?;
            t.exec(
                "UPDATE global_usage SET push_jobs=push_jobs+1 WHERE id=1",
                &[],
            )
            .await?;
        }
        Ok(())
    }
    pub(super) async fn delete_event(&self, t: &mut Tx, r: &Value) -> Result<()> {
        if t.exec("DELETE FROM events WHERE id=?", args![&r["id"]])
            .await?
            != 0
        {
            t.exec(
                "UPDATE workspace_usage SET events=events-1 WHERE workspace_id=?",
                args![&r["workspace_id"]],
            )
            .await?;
            t.exec("UPDATE global_usage SET events=events-1 WHERE id=1", &[])
                .await?;
        }
        Ok(())
    }
    pub(super) async fn delete_job(&self, t: &mut Tx, r: &Value) -> Result<()> {
        if t.exec("DELETE FROM push_jobs WHERE id=?", args![&r["id"]])
            .await?
            != 0
        {
            t.exec(
                "UPDATE workspace_usage SET push_jobs=push_jobs-1 WHERE workspace_id=?",
                args![&r["workspace_id"]],
            )
            .await?;
            t.exec(
                "UPDATE global_usage SET push_jobs=push_jobs-1 WHERE id=1",
                &[],
            )
            .await?;
        }
        Ok(())
    }
    pub async fn events(&self, c: &Credential, after: i64) -> Result<Value> {
        let mut t = self.db.begin().await?;
        self.authorized_tx(&mut t, c).await?;
        let rows=t.all("SELECT id,node_id,session,kind,alert_kind,created FROM events WHERE workspace_id=? AND id>? ORDER BY id LIMIT 100",args![&c.workspace,after]).await?;
        let mut used = crate::json::size(&json!({"events":[],"cursor":i64::MAX}), MIB)?;
        let mut events = vec![];
        let mut cursor = after;
        for r in rows {
            let mut e = json!({"id":r["id"],"computer_id":r["node_id"],"session":r["session"],"kind":r["kind"],"created_at":r["created"]});
            // Ordinary events keep their released shape.
            if r["kind"] == "alert" {
                e["alert_kind"] = r["alert_kind"].clone();
            }
            let size = crate::json::size(&e, MIB)? + 1;
            if used + size > MIB {
                if events.is_empty() {
                    return Err(Error::LARGE);
                }
                break;
            }
            used += size;
            cursor = i(&r, "id");
            events.push(e);
        }
        t.commit().await?;
        Ok(json!({"events":events,"cursor":cursor}))
    }
}
