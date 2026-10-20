//! Alert candidates: derived events held until the presence rule allows a
//! phone alert. Plans read without locks; deleting the candidate row in the
//! transaction that appends the `alert` event is the single-delivery claim
//! across workers. Rows are applied in id order so evaluators lock alike.
use super::*;
use crate::{
    alerts::{self, Candidate, Decision, Delivery, Presence},
    db::{f, Arg, Tx},
};
use std::collections::{BTreeMap, BTreeSet};

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
    /// `None` for a revoked or unknown computer; `Some(Null)` when no route
    /// stores a readable snapshot, which is missing data.
    async fn computer_snapshot(&self, t: &mut Tx, computer: &str) -> Result<Option<Value>> {
        if t.one(
            "SELECT id FROM computers WHERE id=? AND revoked=0",
            args![computer],
        )
        .await?
        .is_none()
        {
            return Ok(None);
        }
        let latest=t.one("SELECT CASE WHEN r.local=1 THEN n.snapshot ELSE r.snapshot END AS snapshot FROM computer_routes r JOIN nodes n ON n.id=r.gateway_id WHERE r.computer_id=? AND r.active=1 AND n.revoked=0 AND (CASE WHEN r.local=1 THEN n.snapshot ELSE r.snapshot END) IS NOT NULL ORDER BY COALESCE(CASE WHEN r.local=1 THEN n.last_seen ELSE r.last_seen END,0) DESC,r.gateway_id,r.route_id LIMIT 1",args![computer]).await?;
        let mut snapshot = latest
            .and_then(|r| crate::json::parse(s(&r, "snapshot").as_bytes(), 32, 50000).ok())
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
        accepted: Option<(&str, &Value)>,
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
        if let Some((id, snapshot)) = accepted {
            let mut snapshot = snapshot.clone();
            crate::notification_highwater::overlay(t, id, &mut snapshot).await?;
            for (k, r) in rows.iter().enumerate() {
                if s(r, "computer_id") == id {
                    checked[k] = true;
                    resolved[k] =
                        Candidate::from_row(r).resolved(alerts::live(&snapshot, s(r, "session")));
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
                let snapshot = self.computer_snapshot(t, computer).await?;
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
                    match self.computer_snapshot(t, s(&r, "computer_id")).await? {
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
        accepted: Option<(&str, &Value)>,
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
    /// Elected maintenance: expire old candidates and evaluate due ones in one
    /// transaction, which keeps the election for the whole bounded pass.
    /// Expired rows join their workspace's plan as drops, so candidate rows are
    /// locked only after event locks and in ascending id per workspace.
    pub(super) async fn maintain_alerts(&self, at: f64) -> Result<()> {
        let batch = self.db.cfg.maintenance_batch;
        let mut t = self.db.begin().await?;
        if !Self::elected(&mut t, 735628114).await? {
            return Ok(());
        }
        // The most overdue workspaces first, so none starves under load; the
        // sorted map still applies them in workspace order. Expiry takes the
        // oldest rows of any workspace, outside this cap.
        let due = t.all("SELECT workspace_id FROM (SELECT workspace_id,next_at FROM alert_candidates WHERE next_at<=? ORDER BY next_at,id LIMIT ?) due GROUP BY workspace_id ORDER BY MIN(next_at),workspace_id LIMIT 32",args![at,batch]).await?;
        let mut plans = BTreeMap::new();
        for r in due {
            let w = s(&r, "workspace_id").to_owned();
            let plan = self.alert_plan(&mut t, &w, None, Some(at), at).await?;
            plans.insert(w, plan);
        }
        for r in t.all("SELECT id,workspace_id FROM alert_candidates WHERE created<=? ORDER BY created,id LIMIT ?",args![at-alerts::CANDIDATE_TTL,batch]).await? {
            let w = s(&r, "workspace_id").to_owned();
            plans.entry(w).or_default().rows.push((r, Action::Drop));
        }
        for plan in plans.values_mut() {
            // A row both due and expired is planned as a drop and dropped once.
            plan.rows.sort_by_key(|(r, _)| i(r, "id"));
            plan.rows
                .dedup_by(|later, kept| later.0["id"] == kept.0["id"]);
        }
        let spaces: Vec<&str> = plans
            .iter()
            .filter(|(_, plan)| plan.deliveries() > 0)
            .map(|(w, _)| w.as_str())
            .collect();
        if !spaces.is_empty() {
            let deliveries = plans.values().map(AlertPlan::deliveries).sum();
            self.event_locks(&mut t, &spaces, deliveries).await?;
        }
        let mut topics = vec![];
        for (w, plan) in plans {
            if self.apply_alerts(&mut t, &w, plan).await? {
                let topic = format!("workspace:{w}");
                t.notify(&topic).await?;
                topics.push(topic);
            }
        }
        t.commit().await?;
        for topic in topics {
            self.db.signal(topic);
        }
        Ok(())
    }
}
