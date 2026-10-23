//! Operator-owned service classes and cross-worker resource reservations.
//! A lease owns capacity through body ingestion, blocking work and response
//! delivery. Ordinary work cannot borrow the team's reserved portion.
use super::*;

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum ServiceClass {
    #[default]
    Standard,
    Team,
}
impl ServiceClass {
    pub fn name(self) -> &'static str {
        match self {
            Self::Standard => "standard",
            Self::Team => "team",
        }
    }
    pub fn parse(value: &str) -> Result<Self> {
        match value {
            "standard" => Ok(Self::Standard),
            "team" => Ok(Self::Team),
            _ => Err(Error::BAD),
        }
    }
}

/// Release is application-owned, including cancellation of the HTTP future.
pub struct ServiceLease {
    store: Store,
    id: String,
    class: ServiceClass,
}
impl Drop for ServiceLease {
    fn drop(&mut self) {
        let store = self.store.clone();
        let id = self.id.clone();
        let class = self.class;
        self.store.db.tasks.spawn(async move {
            let _ = store.release_service(&id, class).await;
        });
    }
}

impl Store {
    /// Queue retention reserves are enforced before eviction, so a standard
    /// workspace cannot evict a team's event or pending generic push wake.
    pub(super) async fn queue_reservation(
        &self,
        t: &mut crate::db::Tx,
        w: &str,
        queue: &str,
        maximum: i64,
    ) -> Result<(bool, bool)> {
        if !self.db.cfg.public_enrollment {
            return Ok((false, false));
        }
        let class = t
            .one("SELECT service_class FROM workspaces WHERE id=?", args![w])
            .await?
            .ok_or(Error::BAD)?;
        let standard = class["service_class"] != "team";
        let count=match queue {
            "events"=>t.one("SELECT count(*) AS count FROM events e JOIN workspaces w ON w.id=e.workspace_id WHERE w.service_class='standard'",&[]).await?,
            "pushes"=>t.one("SELECT count(*) AS count FROM push_jobs j JOIN devices d ON d.id=j.device_id JOIN workspaces w ON w.id=d.workspace_id WHERE w.service_class='standard'",&[]).await?,
            _=>return Err(Error::BAD),
        }.ok_or(Error::BAD)?;
        Ok((
            standard,
            standard
                && i(&count, "count") >= self.db.cfg.standard_capacity(maximum as usize) as i64,
        ))
    }
    pub async fn service_class(&self, workspace: &str) -> Result<ServiceClass> {
        let row = self
            .db
            .one(
                "SELECT service_class FROM workspaces WHERE id=?",
                args![workspace],
            )
            .await?
            .ok_or(Error::NOT_FOUND)?;
        ServiceClass::parse(s(&row, "service_class"))
    }

    /// This is local operator administration, never an HTTP/client option.
    pub async fn set_service_class(&self, workspace: &str, class: ServiceClass) -> Result<()> {
        if self
            .db
            .exec(
                "UPDATE workspaces SET service_class=? WHERE id=?",
                args![class.name(), workspace],
            )
            .await?
            == 0
        {
            return Err(Error::NOT_FOUND);
        }
        if self.db.cfg.public_enrollment {
            self.db.refresh_team_auth().await?;
        }
        Ok(())
    }

    pub async fn acquire_service(
        &self,
        lane: &str,
        class: ServiceClass,
        units: usize,
    ) -> Result<Option<ServiceLease>> {
        if !self.db.cfg.public_enrollment || units == 0 {
            return Ok(None);
        }
        let mut t = self.db.control(class).await?;
        let row = t
            .one(
                "SELECT * FROM service_resources WHERE lane=? FOR UPDATE",
                args![lane],
            )
            .await?
            .ok_or(Error::BAD)?;
        // A crashed worker's leases expire. Each pass stays bounded even when
        // all workers crash at once; healthy drops release immediately.
        let old = t.all("SELECT id,units,service_class FROM service_leases WHERE lane=? AND expires<=? ORDER BY expires,id LIMIT 256", args![lane, now()]).await?;
        let mut total = i(&row, "used_total");
        let mut standard = i(&row, "used_standard");
        for lease in old {
            if t.exec("DELETE FROM service_leases WHERE id=?", args![&lease["id"]])
                .await?
                != 0
            {
                total -= i(&lease, "units");
                if lease["service_class"] == "standard" {
                    standard -= i(&lease, "units");
                }
            }
        }
        let amount = i64::try_from(units).map_err(|_| Error::BAD)?;
        if total
            .checked_add(amount)
            .is_none_or(|n| n > i(&row, "total"))
            || (class == ServiceClass::Standard
                && standard
                    .checked_add(amount)
                    .is_none_or(|n| n > i(&row, "standard")))
        {
            t.exec(
                "UPDATE service_resources SET used_total=?,used_standard=? WHERE lane=?",
                args![total, standard, lane],
            )
            .await?;
            t.commit().await?;
            return Err(Error::BUSY);
        }
        total += amount;
        if class == ServiceClass::Standard {
            standard += amount;
        }
        let id = Uuid::new_v4().to_string();
        // All request/response deadlines, including the 120s connection
        // ceiling, fit within this conservative crash-recovery interval.
        t.exec(
            "INSERT INTO service_leases(id,lane,service_class,units,expires) VALUES(?,?,?,?,?)",
            args![&id, lane, class.name(), amount, now() + 300.0],
        )
        .await?;
        t.exec(
            "UPDATE service_resources SET used_total=?,used_standard=? WHERE lane=?",
            args![total, standard, lane],
        )
        .await?;
        t.commit().await?;
        Ok(Some(ServiceLease {
            store: self.clone(),
            id,
            class,
        }))
    }

    pub async fn release_service(&self, id: &str, class: ServiceClass) -> Result<()> {
        let mut t = self.db.control(class).await?;
        if let Some(row) = t
            .one("SELECT lane FROM service_leases WHERE id=?", args![id])
            .await?
        {
            // Same resource-before-lease lock order as acquisition.
            t.one(
                "SELECT lane FROM service_resources WHERE lane=? FOR UPDATE",
                args![&row["lane"]],
            )
            .await?;
            if let Some(lease) = t
                .one(
                    "DELETE FROM service_leases WHERE id=? RETURNING lane,units,service_class",
                    args![id],
                )
                .await?
            {
                t.exec("UPDATE service_resources SET used_total=used_total-?,used_standard=used_standard-? WHERE lane=?",args![&lease["units"],if lease["service_class"]=="standard" {i(&lease,"units")}else{0},&lease["lane"]]).await?;
            }
        }
        t.commit().await
    }
}
