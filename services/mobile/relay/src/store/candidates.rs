//! Alert candidates: derived events held until the presence rule allows a
//! phone alert. Plans read without locks; deleting the candidate row in the
//! transaction that appends the `alert` event is the single-delivery claim
//! across workers. Rows are applied in id order so evaluators lock alike.
use super::*;
use crate::{
    alerts::{self, Candidate, Decision, Delivery, Presence},
    db::{f, Arg, Tx},
    registry,
};
use std::collections::BTreeSet;

enum Action {
    Drop,
    Deliver,
    Wait(f64),
}
#[derive(Default)]
struct AlertPlan {
    rows: Vec<(Value, Action)>,
}
impl AlertPlan {
    fn deliveries(&self) -> usize {
        self.rows
            .iter()
            .filter(|(_, a)| matches!(a, Action::Deliver))
            .count()
    }
}
/// A snapshot just committed for one computer through one route, keyed as in
/// `computer_routes`.
pub(super) struct Accepted<'a> {
    pub computer: &'a str,
    pub gateway: &'a str,
    pub route: &'a str,
    pub snapshot: &'a Value,
}
fn text(v: &Option<String>) -> Arg<'_> {
    v.as_deref().map_or(Arg::Null, Arg::Text)
}
impl Store {
    /// Records derived events of one computer. The caller holds event locks,
    /// which include this workspace, so the cap is transactional.
    pub(super) async fn record_candidates(
        &self,
        t: &mut Tx,
        w: &str,
        computer: &str,
        candidates: &[Candidate],
    ) -> Result<()> {
        // Lock order: event locks, then every candidate row of this workspace in
        // ascending id, as evaluators do, before replacing any of them.
        let mut count = t
            .all(
                "SELECT id FROM alert_candidates WHERE workspace_id=? ORDER BY id FOR UPDATE",
                args![w],
            )
            .await?
            .len() as i64;
        for c in candidates {
            // A newer event for the same session and kind replaces the held one.
            count -= t.exec("DELETE FROM alert_candidates WHERE workspace_id=? AND computer_id=? AND session=? AND kind=?",args![w,computer,&c.session,&c.kind]).await? as i64;
            if count >= alerts::WORKSPACE_CANDIDATES {
                count -= t.exec("DELETE FROM alert_candidates WHERE id IN (SELECT id FROM alert_candidates WHERE workspace_id=? ORDER BY id LIMIT 1)",args![w]).await? as i64;
            }
            let at = now();
            t.exec("INSERT INTO alert_candidates(workspace_id,computer_id,session,kind,run_id,conversation_id,attention_signature,reply_id,created,next_at) VALUES(?,?,?,?,?,?,?,?,?,?)",args![w,computer,&c.session,&c.kind,text(&c.run_id),text(&c.conversation_id),text(&c.attention_signature),text(&c.reply_id),at,at]).await?;
            count += 1;
        }
        Ok(())
    }
    /// The snapshot of the route the catalog shows: a computer reachable
    /// directly and through a gateway can lag on one route. `None` for a
    /// revoked or unknown computer; `Some(Null)` when that route stores no
    /// readable snapshot, which is missing data.
    async fn computer_snapshot(
        &self,
        t: &mut Tx,
        w: &str,
        computer: &str,
    ) -> Result<Option<Value>> {
        if t.one(
            "SELECT id FROM computers WHERE id=? AND revoked=0",
            args![computer],
        )
        .await?
        .is_none()
        {
            return Ok(None);
        }
        let Some(route) = registry::shown(t, w, computer).await? else {
            return Ok(Some(Value::Null));
        };
        let stored = registry::snapshot(t, &route).await?;
        let mut snapshot = stored
            .as_str()
            .and_then(|v| crate::json::parse(v.as_bytes(), 32, 50000).ok())
            .unwrap_or(Value::Null);
        crate::notification_highwater::overlay(t, computer, &mut snapshot).await?;
        Ok(Some(snapshot))
    }
    /// Plans one workspace. `accepted` is a snapshot just committed for one
    /// computer; `due` limits maintenance to candidates whose recheck came.
    async fn alert_plan(
        &self,
        t: &mut Tx,
        w: &str,
        accepted: Option<Accepted<'_>>,
        due: Option<f64>,
        at: f64,
    ) -> Result<AlertPlan> {
        let rows = t.all("SELECT * FROM alert_candidates WHERE workspace_id=? AND next_at<=? ORDER BY id LIMIT ?",args![w,due.unwrap_or(f64::MAX),alerts::WORKSPACE_CANDIDATES]).await?;
        let mut plan = AlertPlan::default();
        if rows.is_empty() {
            return Ok(plan);
        }
        let delivery = t.one("SELECT mobile_delivery FROM nodes WHERE workspace_id=? AND revoked=0 AND mobile_delivery IS NOT NULL AND last_seen IS NOT NULL ORDER BY last_seen DESC LIMIT 1",args![w]).await?
            .and_then(|r| r["mobile_delivery"].as_str().and_then(Delivery::parse))
            .unwrap_or(Delivery::DEFAULT);
        // Every remembered presence counts, fresh or stale (D2).
        let machines: Vec<Presence> = t.all("SELECT n.presence_at,n.presence_until FROM nodes n JOIN computers c ON c.id=n.computer_id WHERE n.workspace_id=? AND n.revoked=0 AND c.revoked=0 AND n.presence_at IS NOT NULL UNION ALL SELECT r.presence_at,r.presence_until FROM computer_routes r JOIN nodes n ON n.id=r.gateway_id JOIN computers c ON c.id=r.computer_id WHERE n.workspace_id=? AND n.revoked=0 AND c.revoked=0 AND r.local=0 AND r.active=1 AND r.presence_at IS NOT NULL",args![w,w]).await?
            .iter()
            .map(|r| Presence { received: f(r, "presence_at"), away_since: f(r, "presence_until") })
            .collect();
        let decision = alerts::decide(delivery, alerts::away_since(&machines), at);
        let mut resolved = vec![false; rows.len()];
        let mut checked = vec![false; rows.len()];
        if let Some(a) = accepted.filter(|a| rows.iter().any(|r| s(r, "computer_id") == a.computer))
        {
            // Only the displayed route supplies native state. A lagging
            // route still triggers a check of this one physical computer:
            // its confirmed Mute must withdraw candidates even while the
            // owner is present and ordinary deliveries are waiting.
            let shown = registry::shown(t, w, a.computer)
                .await?
                .is_some_and(|r| s(&r, "gateway_id") == a.gateway && s(&r, "route_id") == a.route);
            let snapshot = if shown {
                let mut snapshot = a.snapshot.clone();
                crate::notification_highwater::overlay(t, a.computer, &mut snapshot).await?;
                Some(snapshot)
            } else {
                self.computer_snapshot(t, w, a.computer).await?
            };
            for (k, r) in rows.iter().enumerate() {
                if s(r, "computer_id") != a.computer {
                    continue;
                }
                checked[k] = true;
                match &snapshot {
                    None => resolved[k] = true,
                    Some(Value::Null) => {} // Unknown current state never resolves a request.
                    Some(current) => {
                        resolved[k] =
                            Candidate::from_row(r).resolved(alerts::live(current, s(r, "session")))
                    }
                }
            }
        }
        if decision == Decision::Deliver {
            // Other stored snapshots are parsed only before a delivery, one at a time.
            let computers: BTreeSet<&str> = rows
                .iter()
                .zip(&checked)
                .filter(|(_, done)| !**done)
                .map(|(r, _)| s(r, "computer_id"))
                .collect();
            for computer in computers {
                let snapshot = self.computer_snapshot(t, w, computer).await?;
                for (k, r) in rows.iter().enumerate() {
                    if checked[k] || s(r, "computer_id") != computer {
                        continue;
                    }
                    match &snapshot {
                        // A revoked or unknown computer has no session to alert about.
                        None => resolved[k] = true,
                        // No stored snapshot is missing data: keep the candidate.
                        Some(Value::Null) => {}
                        Some(current) => {
                            resolved[k] = Candidate::from_row(r)
                                .resolved(alerts::live(current, s(r, "session")))
                        }
                    }
                }
            }
        }
        for (k, r) in rows.into_iter().enumerate() {
            let action = if resolved[k] || f(&r, "created") <= at - alerts::CANDIDATE_TTL {
                Action::Drop
            } else {
                match decision {
                    Decision::Deliver => Action::Deliver,
                    Decision::Wait(next) => {
                        // Rewrite only an earlier or elapsed recheck time.
                        let current = f(&r, "next_at");
                        if next >= current && current > at {
                            continue;
                        }
                        Action::Wait(next)
                    }
                }
            };
            plan.rows.push((r, action));
        }
        Ok(plan)
    }
    /// Applies a plan. The caller holds event locks when it contains deliveries.
    /// Plan rows are in ascending id, so candidate rows are locked after any
    /// event locks in the same order as `record_candidates`.
    /// Returns true when an alert was appended.
    async fn apply_alerts(&self, t: &mut Tx, w: &str, plan: AlertPlan) -> Result<bool> {
        let mut latest = None;
        for (r, action) in plan.rows {
            match action {
                Action::Drop => {
                    t.exec("DELETE FROM alert_candidates WHERE id=?", args![&r["id"]])
                        .await?;
                }
                Action::Wait(next) => {
                    t.exec(
                        "UPDATE alert_candidates SET next_at=? WHERE id=?",
                        args![next, &r["id"]],
                    )
                    .await?;
                }
                Action::Deliver => {
                    // Planning preceded the workspace lock. Recheck the current
                    // projected preference after it, before the one delivery claim.
                    match self.computer_snapshot(t, w, s(&r, "computer_id")).await? {
                        Some(Value::Null) => continue, // Missing current data cannot prove an alert is still audible.
                        current => {
                            let resolved = current.as_ref().is_none_or(|snapshot| {
                                Candidate::from_row(&r)
                                    .resolved(alerts::live(snapshot, s(&r, "session")))
                            });
                            if resolved {
                                t.exec("DELETE FROM alert_candidates WHERE id=?", args![&r["id"]])
                                    .await?;
                                continue;
                            }
                        }
                    }
                    // Another worker may already have delivered or replaced this row.
                    if t.exec("DELETE FROM alert_candidates WHERE id=?", args![&r["id"]])
                        .await?
                        == 1
                    {
                        latest = Some(
                            self.append_event(
                                t,
                                w,
                                s(&r, "computer_id"),
                                s(&r, "session"),
                                "alert",
                                Some(s(&r, "kind")),
                            )
                            .await?,
                        );
                    }
                }
            }
        }
        let Some(event) = latest else {
            return Ok(false);
        };
        self.wake_devices(t, w, &event["id"]).await?;
        Ok(true)
    }
    /// Re-evaluates a workspace after a committed snapshot or new presence.
    pub(super) async fn evaluate_workspace(
        &self,
        w: &str,
        accepted: Option<Accepted<'_>>,
    ) -> Result<()> {
        let at = now();
        let mut t = self.db.begin().await?;
        let plan = self.alert_plan(&mut t, w, accepted, None, at).await?;
        if plan.rows.is_empty() {
            return Ok(());
        }
        let deliveries = plan.deliveries();
        if deliveries > 0 {
            self.event_locks(&mut t, &[w], deliveries).await?;
        }
        let topic = format!("workspace:{w}");
        let published = self.apply_alerts(&mut t, w, plan).await?;
        if published {
            t.notify(&topic).await?;
        }
        t.commit().await?;
        if published {
            self.db.signal(topic);
        }
        Ok(())
    }
    /// Maintenance in short transactions, so that no failure or wait stops
    /// the rest: expiry first, then each due workspace in its own elected
    /// transaction. A plan without deliveries takes no event locks; plans
    /// that deliver run last, one workspace per transaction, so the global
    /// event row is held for at most one workspace's candidates. The first
    /// failure is reported after every workspace had its turn.
    pub(super) async fn maintain_alerts(&self, at: f64) -> Result<()> {
        let mut failure = self.expire_alerts(at).await.err();
        // The most overdue workspaces first, so none starves under load.
        let mut t = self.db.begin().await?;
        let due = t.all("SELECT workspace_id FROM (SELECT workspace_id,next_at FROM alert_candidates WHERE next_at<=? ORDER BY next_at,id LIMIT ?) due GROUP BY workspace_id ORDER BY MIN(next_at),workspace_id LIMIT 32",args![at,self.db.cfg.maintenance_batch]).await?;
        t.commit().await?;
        let mut delivering = vec![];
        for r in &due {
            match self
                .maintain_workspace(s(r, "workspace_id"), at, false)
                .await
            {
                Ok(true) => delivering.push(s(r, "workspace_id")),
                Ok(false) => {}
                Err(e) => {
                    failure.get_or_insert(e);
                }
            }
        }
        for w in delivering {
            if let Err(e) = self.maintain_workspace(w, at, true).await {
                failure.get_or_insert(e);
            }
        }
        failure.map_or(Ok(()), Err)
    }
    /// Expires the oldest candidates in an elected transaction of its own, so
    /// a failing plan or delivery never stops the storage bound. Rows are
    /// deleted in ascending id, the order in which evaluators lock them.
    async fn expire_alerts(&self, at: f64) -> Result<()> {
        let mut t = self.db.begin().await?;
        if !Self::elected(&mut t, 735628115).await? {
            return Ok(());
        }
        let mut rows = t
            .all(
                "SELECT id FROM alert_candidates WHERE created<=? ORDER BY created,id LIMIT ?",
                args![at - alerts::CANDIDATE_TTL, self.db.cfg.maintenance_batch],
            )
            .await?;
        rows.sort_by_key(|r| i(r, "id"));
        for r in rows {
            t.exec("DELETE FROM alert_candidates WHERE id=?", args![&r["id"]])
                .await?;
        }
        t.commit().await
    }
    /// Plans and applies one workspace's due candidates while elected for that
    /// workspace. Without `deliver`, a plan that delivers is left unapplied
    /// and reported, for a later transaction that takes the event locks.
    /// Each transaction ends explicitly, which releases the election at once.
    async fn maintain_workspace(&self, w: &str, at: f64, deliver: bool) -> Result<bool> {
        let mut t = self.db.begin().await?;
        if t.postgres
            && !t
                .one(
                    "SELECT pg_try_advisory_xact_lock(735628114,hashtext(?)) AS acquired",
                    args![w],
                )
                .await?
                .is_some_and(|r| r["acquired"] == true)
        {
            return Ok(false);
        }
        let plan = self.alert_plan(&mut t, w, None, Some(at), at).await?;
        let deliveries = plan.deliveries();
        if plan.rows.is_empty() || deliveries > 0 && !deliver {
            t.commit().await?;
            return Ok(deliveries > 0);
        }
        if deliveries > 0 {
            self.event_locks(&mut t, &[w], deliveries).await?;
        }
        let topic = format!("workspace:{w}");
        let published = self.apply_alerts(&mut t, w, plan).await?;
        if published {
            t.notify(&topic).await?;
        }
        t.commit().await?;
        if published {
            self.db.signal(topic);
        }
        Ok(false)
    }
}
