//! Durable queue and accounting. Side effects stop at the committed database.
mod background;
mod candidates;
mod requests;
mod retirement;
pub use requests::Submission;
mod service;
mod snapshots;
pub use service::{ServiceClass, ServiceLease};
mod encrypted;
mod enrollment;
mod signals;
use crate::{
    args,
    db::{i, now, s, Db},
    error::{Error, Result},
    json::{canonical, digest},
    registry,
};
use base64::{engine::general_purpose::URL_SAFE_NO_PAD, Engine};
pub use encrypted::{EncryptedSubmission, ENCRYPTED_REQUEST_MAX, ENCRYPTED_SMALL_MAX};
use rand::RngCore;
use serde_json::{json, Value};
use uuid::Uuid;
#[derive(Clone)]
pub struct Store {
    pub db: Db,
}
#[derive(Clone)]
pub struct Credential {
    pub id: String,
    pub workspace: String,
    pub role: Role,
}
#[derive(Clone, Copy, PartialEq, Eq)]
pub enum Role {
    Node,
    Device,
}
impl Role {
    pub fn table(self) -> &'static str {
        match self {
            Self::Node => "nodes",
            Self::Device => "devices",
        }
    }
}
/// Seconds between recorded phone activity times.
pub const LAST_SEEN_INTERVAL: f64 = 300.0;
/// The phone list bound per workspace.
pub const MAX_LISTED_DEVICES: usize = 200;

pub fn token() -> String {
    let mut bytes = [0u8; 32];
    rand::rngs::OsRng.fill_bytes(&mut bytes);
    URL_SAFE_NO_PAD.encode(bytes)
}
pub fn envelope(r: &Value) -> Result<Value> {
    if i(r, "oversized") != 0 {
        return Err(Error::LARGE);
    }
    Ok(
        json!({"request_id":r["id"],"state":r["state"],"result":match r["result"].as_str(){Some(s)=>crate::json::parse(s.as_bytes(),32,50000)?,None=>Value::Null},"error":r["error"]}),
    )
}
impl Store {
    pub fn new(db: Db) -> Self {
        Self { db }
    }
    pub async fn provision(&self, name: &str, computer: &str) -> Result<Value> {
        let w = Uuid::new_v4().to_string();
        let n = Uuid::new_v4().to_string();
        let secret = token();
        let code = token();
        let expires = now() + 600.0;
        let mut t = self.db.begin().await?;
        t.exec(
            "INSERT INTO workspaces(id,name) VALUES(?,?)",
            args![&w, name],
        )
        .await?;
        t.exec(
            "INSERT INTO workspace_usage(workspace_id) VALUES(?)",
            args![&w],
        )
        .await?;
        t.exec(
            "INSERT INTO nodes(id,workspace_id,name,token_hash) VALUES(?,?,?,?)",
            args![&n, &w, computer, &digest(&secret)],
        )
        .await?;
        registry::enroll(&mut t, &w, &n, computer).await?;
        t.exec(
            "INSERT INTO invitations(code_hash,workspace_id,expires) VALUES(?,?,?)",
            args![&digest(&code), &w, expires],
        )
        .await?;
        t.commit().await?;
        Ok(
            json!({"workspace_id":w,"node_id":n,"node_token":secret,"pair_code":code,"expires_at":expires}),
        )
    }
    pub async fn node(&self, w: &str, name: &str) -> Result<Value> {
        let mut t = self.db.begin().await?;
        self.db.usage(&mut t, w).await?;
        let n = Uuid::new_v4().to_string();
        let secret = token();
        t.exec(
            "INSERT INTO nodes(id,workspace_id,name,token_hash) VALUES(?,?,?,?)",
            args![&n, w, name, &digest(&secret)],
        )
        .await?;
        registry::enroll(&mut t, w, &n, name).await?;
        t.commit().await?;
        Ok(json!({"workspace_id":w,"node_id":n,"node_token":secret}))
    }
    pub async fn invite(&self, w: &str) -> Result<Value> {
        let code = token();
        let expires = now() + 600.0;
        self.db
            .exec(
                "INSERT INTO invitations(code_hash,workspace_id,expires) VALUES(?,?,?)",
                args![&digest(&code), w, expires],
            )
            .await?;
        Ok(json!({"workspace_id":w,"pair_code":code,"expires_at":expires}))
    }
    /// A phone invitation issued by a computer of the workspace, which the phone
    /// list later names. Same lifetime and single use as the administrator's.
    pub async fn invite_from(&self, c: &Credential) -> Result<Value> {
        let mut t = self.db.begin().await?;
        self.authorized_tx(&mut t, c).await?;
        let code = token();
        let expires = now() + 600.0;
        t.exec(
            "INSERT INTO invitations(code_hash,workspace_id,expires,added_by) VALUES(?,?,?,?)",
            args![&digest(&code), &c.workspace, expires, &c.id],
        )
        .await?;
        let workspace = t
            .one(
                "SELECT name FROM workspaces WHERE id=?",
                args![&c.workspace],
            )
            .await?
            .ok_or(Error::UNAUTHORIZED)?;
        t.commit().await?;
        Ok(
            json!({"workspace_id":c.workspace,"workspace_name":workspace["name"],"pair_code":code,"expires_at":expires}),
        )
    }
    pub async fn pair(&self, code: &str, name: &str) -> Result<Value> {
        let mut t = self.db.begin().await?;
        let row = t
            .one(
                "DELETE FROM invitations WHERE code_hash=? AND expires>? RETURNING workspace_id,added_by",
                args![&digest(code), now()],
            )
            .await?
            .ok_or(Error::UNAUTHORIZED)?;
        let w = s(&row, "workspace_id");
        let id = Uuid::new_v4().to_string();
        let secret = token();
        t.exec(
            "INSERT INTO devices(id,workspace_id,name,token_hash,paired_at,added_by) VALUES(?,?,?,?,?,?)",
            args![&id, w, name, &digest(&secret), now(), &row["added_by"]],
        )
        .await?;
        let workspace = t
            .one("SELECT name FROM workspaces WHERE id=?", args![w])
            .await?
            .ok_or(Error::UNAUTHORIZED)?;
        t.commit().await?;
        Ok(
            json!({"device_id":id,"device_token":secret,"workspace_id":w,"workspace_name":workspace["name"]}),
        )
    }
    pub async fn authenticate(&self, secret: &str, role: Role) -> Result<Credential> {
        self.authenticate_class(secret, role)
            .await
            .map(|(credential, _)| credential)
    }
    pub async fn authenticate_class(
        &self,
        secret: &str,
        role: Role,
    ) -> Result<(Credential, ServiceClass)> {
        let class = self.db.auth_class(secret);
        let device = role == Role::Device;
        let mut t = self.db.control(class).await?;
        let row=t.one(&format!("SELECT c.id,c.workspace_id,w.service_class{} FROM {} c JOIN workspaces w ON w.id=c.workspace_id WHERE c.token_hash=? AND c.revoked=0",if device{",c.last_seen"}else{""},role.table()),args![&digest(secret)]).await?.ok_or(Error::UNAUTHORIZED)?;
        let actual = ServiceClass::parse(s(&row, "service_class"))?;
        let at = now();
        if device
            && row["last_seen"]
                .as_f64()
                .is_none_or(|seen| seen < at - LAST_SEEN_INTERVAL)
        {
            t.exec(
                "UPDATE devices SET last_seen=? WHERE id=?",
                args![at, s(&row, "id")],
            )
            .await?;
        }
        t.commit().await?;
        Ok((
            Credential {
                id: s(&row, "id").into(),
                workspace: s(&row, "workspace_id").into(),
                role,
            },
            actual,
        ))
    }
    pub async fn authorized(&self, c: &Credential) -> Result<()> {
        self.db
            .one(
                &format!(
                    "SELECT id FROM {} WHERE id=? AND workspace_id=? AND revoked=0",
                    c.role.table()
                ),
                args![&c.id, &c.workspace],
            )
            .await?
            .ok_or(Error::UNAUTHORIZED)?;
        Ok(())
    }
    pub(super) async fn authorized_tx(&self, t: &mut crate::db::Tx, c: &Credential) -> Result<()> {
        t.one(
            &format!(
                "SELECT id FROM {} WHERE id=? AND workspace_id=? AND revoked=0 FOR SHARE",
                c.role.table()
            ),
            args![&c.id, &c.workspace],
        )
        .await?
        .ok_or(Error::UNAUTHORIZED)?;
        Ok(())
    }
    pub async fn computers(&self, c: &Credential) -> Result<Value> {
        let mut t = self.db.begin().await?;
        self.db.usage(&mut t, &c.workspace).await?;
        self.authorized_tx(&mut t, c).await?;
        t.all(
            "SELECT id FROM nodes WHERE workspace_id=? ORDER BY id FOR SHARE",
            args![&c.workspace],
        )
        .await?;
        t.all("SELECT gateway_id,route_id FROM computer_routes WHERE gateway_id IN (SELECT id FROM nodes WHERE workspace_id=?) ORDER BY gateway_id,route_id FOR SHARE",args![&c.workspace]).await?;
        let v = registry::catalog(
            &mut t,
            &c.workspace,
            self.db.cfg.max_catalog_bytes,
            self.db.cfg.online_timeout,
        )
        .await?;
        t.commit().await?;
        Ok(v)
    }
    /// The active phones of the computer's workspace, newest pairing first.
    pub async fn devices(&self, c: &Credential) -> Result<Value> {
        let mut t = self.db.begin().await?;
        self.authorized_tx(&mut t, c).await?;
        let workspace = t
            .one(
                "SELECT name FROM workspaces WHERE id=?",
                args![&c.workspace],
            )
            .await?
            .ok_or(Error::UNAUTHORIZED)?;
        let rows = t
            .all(
                &format!("SELECT d.id,d.name,d.paired_at,d.last_seen,d.added_by,COALESCE(n.computer_id,n.id) AS added_computer,n.name AS added_name,p.provider FROM devices d LEFT JOIN nodes n ON n.id=d.added_by AND n.workspace_id=d.workspace_id LEFT JOIN pushes p ON p.device_id=d.id WHERE d.workspace_id=? AND d.revoked=0 ORDER BY CASE WHEN d.paired_at IS NULL THEN 1 ELSE 0 END,d.paired_at DESC,d.id LIMIT {MAX_LISTED_DEVICES}"),
                args![&c.workspace],
            )
            .await?;
        t.commit().await?;
        let devices: Vec<Value> = rows
            .iter()
            .map(|r| {
                let platform = match r["provider"].as_str() {
                    Some("apns" | "apns_sandbox") => json!("ios"),
                    Some("fcm") => json!("android"),
                    _ => Value::Null,
                };
                let added_by = if r["added_name"].is_string() {
                    json!({"computer_id":r["added_computer"],"name":r["added_name"]})
                } else {
                    Value::Null
                };
                json!({"id":r["id"],"name":r["name"],"platform":platform,"paired_at":r["paired_at"],"last_seen_at":r["last_seen"],"added_by":added_by})
            })
            .collect();
        Ok(json!({"workspace_id":c.workspace,"workspace_name":workspace["name"],"devices":devices}))
    }
    /// A computer revokes an active phone of its own workspace.
    pub async fn revoke_device(&self, c: &Credential, id: &str) -> Result<()> {
        self.authorized(c).await?;
        self.db
            .one(
                "SELECT id FROM devices WHERE id=? AND workspace_id=? AND revoked=0",
                args![id, &c.workspace],
            )
            .await?
            .ok_or(Error::NOT_FOUND)?;
        self.revoke(Role::Device, id).await
    }
    pub async fn revoke(&self, role: Role, id: &str) -> Result<()> {
        let mut t = self.db.begin().await?;
        let table = role.table();
        let r = t
            .one(
                &format!("SELECT workspace_id FROM {table} WHERE id=?"),
                args![id],
            )
            .await?
            .ok_or(Error::NOT_FOUND)?;
        let w = s(&r, "workspace_id");
        // Global event/job accounting precedes payload shard, workspace, credentials.
        t.one("SELECT id FROM global_usage WHERE id=1 FOR UPDATE", &[])
            .await?;
        self.db.payload_usage(&mut t, w).await?;
        t.exec(
            &format!("UPDATE {table} SET revoked=1 WHERE id=?"),
            args![id],
        )
        .await?;
        registry::invalidate(
            &self.db,
            &mut t,
            if role == Role::Device {
                "device_id=?"
            } else {
                "node_id=?"
            },
            args![id],
            "credential revoked before delivery",
        )
        .await?;
        if role == Role::Device {
            self.delete_push_tx(&mut t, id, w).await?;
        } else {
            t.exec("UPDATE computer_routes SET active=0,snapshot=NULL,snapshot_hash=NULL,last_seen=NULL WHERE gateway_id=?",args![id]).await?;
        }
        let topic = format!("workspace:{w}");
        t.notify(&topic).await?;
        t.notify(&format!("node:{id}")).await?;
        t.commit().await?;
        self.db.signal(topic);
        self.db.signal(format!("node:{id}"));
        Ok(())
    }
    pub async fn revoke_computer(&self, id: &str) -> Result<()> {
        let mut t = self.db.begin().await?;
        // An alias wins, as in routing: a bound direct enrollment keeps its
        // superseded computer row under the same ID.
        let alias=t.one("SELECT c.* FROM computer_aliases a JOIN computers c ON c.id=a.computer_id WHERE a.alias=? LIMIT 1",args![id]).await?;
        let c = match alias {
            Some(c) => c,
            None => t
                .one("SELECT * FROM computers WHERE id=?", args![id])
                .await?
                .ok_or(Error::NOT_FOUND)?,
        };
        self.db.payload_usage(&mut t, s(&c, "workspace_id")).await?;
        registry::revoke_computer(&self.db, &mut t, &c).await?;
        let topic = format!("workspace:{}", s(&c, "workspace_id"));
        t.notify(&topic).await?;
        t.commit().await?;
        self.db.signal(topic);
        Ok(())
    }
    pub async fn rate(&self, key: &Value, limit: i64, anonymous: bool) -> Result<()> {
        let key = digest(canonical(key)?);
        let mut t = self.db.begin().await?;
        let at = now();
        if let Some(r)=t.one("UPDATE rate_limits SET count=CASE WHEN expires<=? THEN 1 ELSE count+1 END,expires=CASE WHEN expires<=? THEN ? ELSE expires END WHERE key=? RETURNING count",args![at,at,at+60.0,&key]).await? {t.commit().await?;return if i(&r,"count")<=limit{Ok(())}else{Err(Error::BUSY)};}
        let bucket = if anonymous { 1i64 } else { 2 };
        let max = if anonymous {
            self.db.cfg.max_rate_entries_anonymous
        } else {
            self.db.cfg.max_rate_entries_authenticated
        };
        t.one(
            "SELECT entries FROM rate_control WHERE id=? FOR UPDATE",
            args![bucket],
        )
        .await?;
        if let Some(r) = t
            .one(
                "UPDATE rate_limits SET count=count+1 WHERE key=? RETURNING count",
                args![&key],
            )
            .await?
        {
            t.commit().await?;
            return if i(&r, "count") <= limit {
                Ok(())
            } else {
                Err(Error::BUSY)
            };
        }
        if t.exec(
            "UPDATE rate_control SET entries=entries+1 WHERE id=? AND entries<?",
            args![bucket, max],
        )
        .await?
            == 0
        {
            return Err(Error::BUSY);
        }
        t.exec(
            "INSERT INTO rate_limits(key,count,expires,bucket) VALUES(?,1,?,?)",
            args![&key, at + 60.0, bucket],
        )
        .await?;
        t.commit().await
    }
}
