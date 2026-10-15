//! Durable queue and accounting. Side effects stop at the committed database.
mod background;
mod candidates;
mod requests;
pub use requests::Submission;
mod snapshots;
use crate::{
    args,
    db::{i, now, s, Db},
    error::{Error, Result},
    json::{canonical, digest},
    registry,
};
use base64::{engine::general_purpose::URL_SAFE_NO_PAD, Engine};
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
    pub async fn pair(&self, code: &str, name: &str) -> Result<Value> {
        let mut t = self.db.begin().await?;
        let row = t
            .one(
                "DELETE FROM invitations WHERE code_hash=? AND expires>? RETURNING workspace_id",
                args![&digest(code), now()],
            )
            .await?
            .ok_or(Error::UNAUTHORIZED)?;
        let w = s(&row, "workspace_id");
        let id = Uuid::new_v4().to_string();
        let secret = token();
        t.exec(
            "INSERT INTO devices(id,workspace_id,name,token_hash) VALUES(?,?,?,?)",
            args![&id, w, name, &digest(&secret)],
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
        let r = self
            .db
            .one(
                &format!(
                    "SELECT id,workspace_id FROM {} WHERE token_hash=? AND revoked=0",
                    role.table()
                ),
                args![&digest(secret)],
            )
            .await?
            .ok_or(Error::UNAUTHORIZED)?;
        Ok(Credential {
            id: s(&r, "id").into(),
            workspace: s(&r, "workspace_id").into(),
            role,
        })
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
