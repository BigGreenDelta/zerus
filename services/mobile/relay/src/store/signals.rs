//! Bounded privacy-preserving notification hints. These hints never establish
//! native work, completion or authorization; phones authenticate fresh content.
use super::enrollment::{crypto, member, workspace};
use super::*;
use crate::{
    alerts::{self, Decision, Delivery, Presence},
    db::{f, Tx, ENCRYPTED_METADATA_BYTES},
    protocol,
};
use zerus_mobile_crypto::Role as DeviceRole;

impl Store {
    pub async fn encrypted_signals(&self, c: &Credential, body: &Value) -> Result<()> {
        protocol::fields(
            body,
            &["presence", "delivery", "signals"],
            &["complete_route"],
        )?;
        let complete = body
            .get("complete_route")
            .map(|route| protocol::uuid(route, false))
            .transpose()?;
        let presence = if body["presence"].is_null() {
            None
        } else {
            protocol::fields(
                &body["presence"],
                &["idle_seconds", "locked"],
                &["locked_seconds"],
            )?;
            Some(Presence::from_value(&body["presence"], now()).ok_or(Error::BAD)?)
        };
        let delivery =
            Delivery::parse(protocol::text(&body["delivery"], 16, false)?).ok_or(Error::BAD)?;
        let signals = body["signals"]
            .as_array()
            .filter(|v| v.len() <= 1000)
            .ok_or(Error::BAD)?;
        let mut seen = std::collections::BTreeSet::new();
        for signal in signals {
            protocol::fields(
                signal,
                &["scope_id", "kind", "change_id", "active"],
                &["route_id"],
            )?;
            if let Some(route) = signal.get("route_id") {
                protocol::uuid(route, false)?;
            }
            crypto(zerus_mobile_crypto::decode_fixed::<32>(protocol::text(
                &signal["scope_id"],
                43,
                false,
            )?))?;
            crypto(zerus_mobile_crypto::decode_fixed::<32>(protocol::text(
                &signal["change_id"],
                43,
                false,
            )?))?;
            if complete.is_some_and(|route| {
                signal
                    .get("route_id")
                    .and_then(Value::as_str)
                    .unwrap_or(&c.id)
                    != route
                    || signal["active"] != true
            }) {
                return Err(Error::BAD);
            }
            let kind = protocol::text(&signal["kind"], 16, false)?;
            if !["input", "error", "completed"].contains(&kind)
                || !signal["active"].is_boolean()
                || !seen.insert((s(signal, "scope_id"), kind))
            {
                return Err(Error::BAD);
            }
        }
        let mut t = self.db.begin().await?;
        workspace(&mut t, &c.workspace).await?;
        self.authorized_tx(&mut t, c).await?;
        if member(&mut t, &c.workspace, &c.id).await?.role != DeviceRole::Computer {
            return Err(Error::UNAUTHORIZED);
        }
        self.db.payload_usage(&mut t, &c.workspace).await?;
        if complete.is_none_or(|route| route == c.id) {
            if let Some(presence) = presence {
                t.exec("UPDATE nodes SET last_seen=?,presence_at=?,presence_until=?,mobile_delivery=? WHERE id=?",args![now(),presence.received,presence.away_since,delivery.name(),&c.id]).await?;
            } else {
                t.exec("UPDATE nodes SET last_seen=?,presence_at=NULL,presence_until=NULL,mobile_delivery=? WHERE id=?",args![now(),delivery.name(),&c.id]).await?;
            }
        } else {
            let changed = if let Some(presence) = presence {
                t.exec("UPDATE encrypted_routes SET presence_at=?,presence_until=? WHERE node_id=? AND route_id=?",args![presence.received,presence.away_since,&c.id,complete.unwrap()]).await?
            } else {
                t.exec("UPDATE encrypted_routes SET presence_at=NULL,presence_until=NULL WHERE node_id=? AND route_id=?",args![&c.id,complete.unwrap()]).await?
            };
            if changed == 0 {
                return Err(Error::NOT_FOUND);
            }
            t.exec(
                "UPDATE nodes SET last_seen=?,mobile_delivery=? WHERE id=?",
                args![now(), delivery.name(), &c.id],
            )
            .await?;
        }
        if let Some(route) = complete {
            let previous=t.all("SELECT scope_id,kind FROM opaque_signals WHERE workspace_id=? AND node_id=? AND route_id=? AND active=1 LIMIT 1000",args![&c.workspace,&c.id,route]).await?;
            for row in previous {
                if !seen.contains(&(s(&row, "scope_id"), s(&row, "kind"))) {
                    t.exec("UPDATE opaque_signals SET active=0 WHERE workspace_id=? AND node_id=? AND scope_id=? AND kind=?",args![&c.workspace,&c.id,&row["scope_id"],&row["kind"]]).await?;
                }
            }
        }
        let count = t
            .one(
                "SELECT count(*) AS count FROM opaque_signals WHERE workspace_id=?",
                args![&c.workspace],
            )
            .await?
            .ok_or(Error::BAD)?;
        let mut count = i(&count, "count");
        for signal in signals {
            let route = signal
                .get("route_id")
                .and_then(Value::as_str)
                .unwrap_or(&c.id);
            let computer = t
                .one(
                    "SELECT computer_id FROM encrypted_routes WHERE node_id=? AND route_id=?",
                    args![&c.id, route],
                )
                .await?;
            let computer = computer
                .as_ref()
                .map(|r| s(r, "computer_id"))
                .unwrap_or(&c.id);
            if computer == c.id && route != c.id {
                return Err(Error::NOT_FOUND);
            }
            let old=t.one("SELECT change_id,active FROM opaque_signals WHERE workspace_id=? AND node_id=? AND scope_id=? AND kind=?",args![&c.workspace,&c.id,&signal["scope_id"],&signal["kind"]]).await?;
            if old.is_none() {
                if count >= 1000 {
                    let removed=t.all("DELETE FROM opaque_signals WHERE (workspace_id,node_id,scope_id,kind) IN (SELECT workspace_id,node_id,scope_id,kind FROM opaque_signals WHERE workspace_id=? AND (active=0 OR created<?) ORDER BY created,node_id,scope_id,kind LIMIT 1) RETURNING metadata_bytes",args![&c.workspace,now()-86400.0]).await?;
                    if removed.is_empty() {
                        return Err(Error::BUSY);
                    }
                    self.db
                        .bytes(&mut t, &c.workspace, -i(&removed[0], "metadata_bytes"))
                        .await?;
                    count -= 1;
                }
                self.db
                    .retain_encrypted_metadata(&mut t, &c.workspace)
                    .await?;
                count += 1;
                t.exec("INSERT INTO opaque_signals(workspace_id,node_id,scope_id,kind,route_id,computer_id,change_id,active,created,next_at,metadata_bytes) VALUES(?,?,?,?,?,?,?,?,?,?,?)",args![&c.workspace,&c.id,&signal["scope_id"],&signal["kind"],route,computer,&signal["change_id"],&signal["active"],now(),now(),ENCRYPTED_METADATA_BYTES]).await?;
            } else if old.as_ref().is_some_and(|r| {
                r["change_id"] != signal["change_id"]
                    || (i(r, "active") == 0 && signal["active"] == true)
            }) {
                t.exec("UPDATE opaque_signals SET change_id=?,active=?,delivered=0,created=?,next_at=? WHERE workspace_id=? AND node_id=? AND scope_id=? AND kind=?",args![&signal["change_id"],&signal["active"],now(),now(),&c.workspace,&c.id,&signal["scope_id"],&signal["kind"]]).await?;
            } else {
                t.exec("UPDATE opaque_signals SET active=? WHERE workspace_id=? AND node_id=? AND scope_id=? AND kind=?",args![&signal["active"],&c.workspace,&c.id,&signal["scope_id"],&signal["kind"]]).await?;
            }
        }
        t.commit().await?;
        self.evaluate_opaque_signals(&c.workspace).await
    }
    pub(super) async fn delete_opaque_signals(
        &self,
        t: &mut Tx,
        w: &str,
        node: &str,
        route: Option<&str>,
    ) -> Result<()> {
        let removed = if let Some(route) = route {
            t.all("DELETE FROM opaque_signals WHERE workspace_id=? AND node_id=? AND route_id=? RETURNING metadata_bytes",args![w,node,route]).await?
        } else {
            t.all("DELETE FROM opaque_signals WHERE workspace_id=? AND node_id=? RETURNING metadata_bytes",args![w,node]).await?
        };
        let released = removed
            .iter()
            .map(|row| i(row, "metadata_bytes"))
            .sum::<i64>();
        if released != 0 {
            self.db.bytes(t, w, -released).await?;
        }
        Ok(())
    }
    async fn evaluate_opaque_signals(&self, w: &str) -> Result<()> {
        let mut t = self.db.begin().await?;
        // Same global -> encrypted workspace -> usage order as payload events.
        t.one("SELECT id FROM global_usage WHERE id=1 FOR UPDATE", &[])
            .await?;
        workspace(&mut t, w).await?;
        self.db.usage(&mut t, w).await?;
        let rows=t.all("SELECT s.* FROM opaque_signals s JOIN nodes n ON n.id=s.node_id WHERE s.workspace_id=? AND s.active=1 AND s.delivered=0 AND s.next_at<=? AND n.revoked=0 ORDER BY s.next_at,s.node_id,s.scope_id,s.kind LIMIT 256",args![w,now()]).await?;
        let delivery=t.one("SELECT mobile_delivery FROM nodes WHERE workspace_id=? AND revoked=0 AND mobile_delivery IS NOT NULL ORDER BY last_seen DESC,id LIMIT 1",args![w]).await?.and_then(|r|r["mobile_delivery"].as_str().and_then(Delivery::parse)).unwrap_or(Delivery::DEFAULT);
        // Same max(min(last activity, receipt + freshness)) rule as
        // alerts::away_since, reduced in SQL rather than loading every route.
        let presence=t.one("SELECT MAX(CASE WHEN COALESCE(presence_until,0)<presence_at+? THEN COALESCE(presence_until,0) ELSE presence_at+? END) AS away_since FROM (SELECT presence_at,presence_until FROM nodes WHERE workspace_id=? AND revoked=0 AND presence_at IS NOT NULL UNION ALL SELECT r.presence_at,r.presence_until FROM encrypted_routes r JOIN nodes n ON n.id=r.node_id WHERE r.workspace_id=? AND n.revoked=0 AND r.route_id!=r.node_id AND r.presence_at IS NOT NULL) remembered",args![alerts::PRESENCE_FRESH,alerts::PRESENCE_FRESH,w,w]).await?.ok_or(Error::BAD)?;
        let decision = alerts::decide(delivery, presence["away_since"].as_f64(), now());
        let mut latest = None;
        for row in rows {
            let key = args![w, &row["node_id"], &row["scope_id"], &row["kind"]];
            if f(&row, "created") < now() - alerts::CANDIDATE_TTL {
                t.exec("UPDATE opaque_signals SET active=0 WHERE workspace_id=? AND node_id=? AND scope_id=? AND kind=?",key).await?;
            } else {
                match decision {
                    Decision::Wait(next) => {
                        t.exec("UPDATE opaque_signals SET next_at=? WHERE workspace_id=? AND node_id=? AND scope_id=? AND kind=?",args![next,w,&row["node_id"],&row["scope_id"],&row["kind"]]).await?;
                    }
                    Decision::Deliver => {
                        t.exec("UPDATE opaque_signals SET delivered=1 WHERE workspace_id=? AND node_id=? AND scope_id=? AND kind=?",key).await?;
                        // The session field is an opaque keyed scope, never a name.
                        latest = Some(
                            self.append_event(
                                &mut t,
                                w,
                                s(&row, "computer_id"),
                                s(&row, "scope_id"),
                                "alert",
                                Some(s(&row, "kind")),
                            )
                            .await?,
                        );
                    }
                }
            }
        }
        if let Some(event) = latest {
            self.wake_devices(&mut t, w, &event["id"]).await?;
            t.notify(&format!("workspace:{w}")).await?;
            t.commit().await?;
            self.db.signal(format!("workspace:{w}"));
        } else {
            t.commit().await?;
        }
        Ok(())
    }
    pub async fn maintain_opaque_signals(&self) -> Result<()> {
        let mut t = self.db.begin().await?;
        let rows=t.all("SELECT workspace_id FROM (SELECT workspace_id,next_at FROM opaque_signals WHERE active=1 AND delivered=0 AND next_at<=? ORDER BY next_at,workspace_id LIMIT ?) due GROUP BY workspace_id ORDER BY MIN(next_at),workspace_id LIMIT 32",args![now(),self.db.cfg.maintenance_batch]).await?;
        t.commit().await?;
        for row in rows {
            self.evaluate_opaque_signals(s(&row, "workspace_id"))
                .await?;
        }
        Ok(())
    }
    pub async fn encrypted_events(&self, c: &Credential, after: i64) -> Result<Value> {
        let events = self.events(c, after).await?;
        let values:Vec<Value>=events["events"].as_array().ok_or(Error::UNAVAILABLE)?.iter().map(|e|json!({"id":e["id"],"computer_id":e["computer_id"],"kind":e["kind"],"scope_id":e["session"],"alert_kind":e["alert_kind"],"created_at":e["created_at"]})).collect();
        Ok(json!({"events":values,"next_after":events["cursor"],"cursor":events["cursor"]}))
    }
}
