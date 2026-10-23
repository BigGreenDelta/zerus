//! A legacy gateway can retire only itself and recover one exact receipt.
use super::*;
use crate::protocol;

impl Store {
    pub async fn legacy_retirement_credential(
        &self,
        secret: &str,
    ) -> Result<(Credential, ServiceClass)> {
        let row = self.db.control_one(self.db.auth_class(secret),
            "SELECT n.id,n.workspace_id,w.service_class FROM nodes n JOIN workspaces w ON w.id=n.workspace_id WHERE n.token_hash=? AND n.revoked=1 AND n.retirement_request_id IS NOT NULL AND NOT EXISTS(SELECT 1 FROM encrypted_members m WHERE m.workspace_id=n.workspace_id AND m.device_id=n.id)",
            args![&digest(secret)]).await?.ok_or(Error::UNAUTHORIZED)?;
        Ok((
            Credential {
                id: s(&row, "id").into(),
                workspace: s(&row, "workspace_id").into(),
                role: Role::Node,
            },
            ServiceClass::parse(s(&row, "service_class"))?,
        ))
    }

    pub async fn retire_legacy_node(&self, c: &Credential, body: &Value) -> Result<Value> {
        protocol::fields(body, &["request_id"], &[])?;
        let request = protocol::uuid(&body["request_id"], false)?;
        if c.role != Role::Node {
            return Err(Error::UNAUTHORIZED);
        }
        let mut t = self.db.begin().await?;
        t.one("SELECT id FROM global_usage WHERE id=1 FOR UPDATE", &[])
            .await?;
        self.db.payload_usage(&mut t, &c.workspace).await?;
        let node=t.one("SELECT revoked,retirement_request_id FROM nodes WHERE id=? AND workspace_id=? FOR UPDATE",args![&c.id,&c.workspace]).await?.ok_or(Error::UNAUTHORIZED)?;
        if t.one(
            "SELECT device_id FROM encrypted_members WHERE workspace_id=? AND device_id=?",
            args![&c.workspace, &c.id],
        )
        .await?
        .is_some()
        {
            return Err(Error::CONFLICT);
        }
        if i(&node, "revoked") != 0 {
            if node["retirement_request_id"] != request {
                return Err(Error::UNAUTHORIZED);
            }
        } else {
            t.exec("UPDATE nodes SET revoked=1,retirement_request_id=?,snapshot=NULL,snapshot_hash=NULL,last_seen=NULL WHERE id=?",args![request,&c.id]).await?;
            registry::invalidate(
                &self.db,
                &mut t,
                "node_id=?",
                args![&c.id],
                "gateway retired before delivery",
            )
            .await?;
            t.exec("UPDATE computer_routes SET active=0,online=0,snapshot=NULL,snapshot_hash=NULL,last_seen=NULL WHERE gateway_id=?",args![&c.id]).await?;
            t.notify(&format!("workspace:{}", c.workspace)).await?;
            t.notify(&format!("node:{}", c.id)).await?;
        }
        t.commit().await?;
        self.db.signal(format!("workspace:{}", c.workspace));
        self.db.signal(format!("node:{}", c.id));
        Ok(json!({"retired":true,"request_id":request,"node_id":c.id}))
    }
}
