//! Opaque transport. Every accepted identity is a permanent no-replay receipt;
//! operation hints never decide whether an identity may be forgotten.
use super::enrollment::{crypto, member, workspace};
use super::*;
use crate::{
    db::{f, Tx, ENCRYPTED_METADATA_BYTES},
    protocol::{self, MIB},
};
use axum::http::StatusCode;
use zerus_mobile_crypto::{EncryptedEnvelope, Header, PayloadKind, Role as DeviceRole};

pub const ENCRYPTED_REQUEST_MAX: usize = 41 * MIB;
pub const ENCRYPTED_SMALL_MAX: usize = 2 * MIB;
async fn verify(
    t: &mut Tx,
    c: &Credential,
    h: &Header,
    kind: PayloadKind,
    state: &Value,
) -> Result<()> {
    if h.workspace_id != c.workspace || h.sender_device_id != c.id || h.kind != kind {
        return Err(Error::UNAUTHORIZED);
    }
    protocol::uuid(&json!(h.request_id), false)?;
    protocol::uuid(&json!(h.gateway_id), false)?;
    protocol::uuid(&json!(h.route_id), false)?;
    let sender = member(t, &c.workspace, &c.id).await?;
    let recipient = member(t, &c.workspace, &h.recipient_device_id).await?;
    if recipient.key_generation != h.recipient_key_generation {
        return Err(Error::CONFLICT);
    }
    if h.membership_revision>i(state,"membership_revision") as u64 || t.one("SELECT revision FROM membership_changes WHERE workspace_id=? AND revision=? AND head=?",args![&c.workspace,h.membership_revision as i64,&h.membership_head]).await?.is_none() {return Err(Error::CONFLICT);}
    if (kind != PayloadKind::Result && h.expires_at as f64 <= now())
        || h.expires_at as f64 > now() + 86400.0
    {
        return Err(Error::BAD);
    }
    if (kind == PayloadKind::Request
        && (sender.role != DeviceRole::Phone
            || recipient.role != DeviceRole::Computer
            || h.gateway_id != recipient.device_id))
        || ([PayloadKind::Result, PayloadKind::Snapshot].contains(&kind)
            && (sender.role != DeviceRole::Computer
                || recipient.role != DeviceRole::Phone
                || h.gateway_id != sender.device_id))
    {
        return Err(Error::UNAUTHORIZED);
    }
    Ok(())
}
/// Prepared on the bounded blocking executor before opening a transaction.
/// Public keys are immutable per device ID; the transaction rechecks current
/// membership/revocation before accepting any prepared ciphertext.
pub struct EncryptedSubmission {
    header: Header,
    hash: String,
    encoded: String,
    computer_id: Option<String>,
}
impl EncryptedSubmission {
    pub fn new(mut body: Value, sender: zerus_mobile_crypto::DevicePublic) -> Result<Self> {
        protocol::fields(&body, &["envelope"], &["computer_id"])?;
        let computer_id = body
            .get("computer_id")
            .map(|v| protocol::uuid(v, false).map(str::to_owned))
            .transpose()?;
        let envelope: EncryptedEnvelope =
            serde_json::from_value(body["envelope"].take()).map_err(|_| Error::BAD)?;
        drop(body);
        crypto(envelope.verify_sender(&sender))?;
        let hash = crypto(envelope.digest())?;
        let encoded = serde_json::to_string(&envelope).map_err(|_| Error::BAD)?;
        Ok(Self {
            header: envelope.header,
            hash,
            encoded,
            computer_id,
        })
    }
}
async fn bind_route(
    store: &Store,
    t: &mut Tx,
    c: &Credential,
    route: &str,
    computer: &str,
) -> Result<()> {
    if let Some(existing) = t
        .one(
            "SELECT computer_id FROM encrypted_route_bindings WHERE node_id=? AND route_id=?",
            args![&c.id, route],
        )
        .await?
    {
        if existing["computer_id"] != computer {
            return Err(Error::CONFLICT);
        }
    } else {
        let count = t
            .one(
                "SELECT count(*) AS count FROM encrypted_route_bindings WHERE node_id=?",
                args![&c.id],
            )
            .await?
            .ok_or(Error::BAD)?;
        if i(&count, "count") >= 4096 {
            return Err(Error::BUSY);
        }
        store.db.retain_encrypted_metadata(t, &c.workspace).await?;
        t.exec("INSERT INTO encrypted_route_bindings(node_id,route_id,computer_id,workspace_id,metadata_bytes) VALUES(?,?,?,?,?)",args![&c.id,route,computer,&c.workspace,ENCRYPTED_METADATA_BYTES]).await?;
    }
    Ok(())
}
fn receipt(row: &Value) -> Result<Value> {
    Ok(
        json!({"request_id":row["id"],"state":row["state"],"envelope":row["result"].as_str().filter(|s|!s.is_empty()).map(serde_json::from_str::<Value>).transpose().map_err(|_|Error::UNAVAILABLE)?}),
    )
}
impl Store {
    pub async fn encrypted_sender(
        &self,
        c: &Credential,
    ) -> Result<zerus_mobile_crypto::DevicePublic> {
        let mut t = self.db.begin().await?;
        let sender = member(&mut t, &c.workspace, &c.id).await?;
        t.commit().await?;
        Ok(sender)
    }
    pub async fn encrypted_submit(
        &self,
        c: &Credential,
        body: &EncryptedSubmission,
    ) -> Result<(StatusCode, Value)> {
        let (e, hash, encoded) = (&body.header, &body.hash, &body.encoded);
        if encoded.len() > ENCRYPTED_REQUEST_MAX - 4096 {
            return Err(Error::LARGE);
        }
        let id = &e.request_id;
        let mut t = self.db.begin().await?;
        let state = workspace(&mut t, &c.workspace).await?;
        self.authorized_tx(&mut t, c).await?;
        if let Some(row)=t.one("SELECT id,device_id,workspace_id,body_hash,state,result FROM encrypted_requests WHERE id=?",args![id]).await? {
            if row["workspace_id"]!=c.workspace || row["device_id"]!=c.id || row["body_hash"]!=*hash {return Err(Error::CONFLICT);}
            let result=receipt(&row)?; t.commit().await?; return Ok((StatusCode::ACCEPTED,result));
        }
        verify(&mut t, c, e, PayloadKind::Request, &state).await?;
        let usage = self.db.payload_usage(&mut t, &c.workspace).await?;
        let total = (encoded.len() + ENCRYPTED_SMALL_MAX) as i64 + ENCRYPTED_METADATA_BYTES;
        if i(&usage, "active") >= self.db.cfg.max_queue.min(1000)
            || i(&usage, "mutations") >= 100000
            || i(&usage, "payload_bytes") + total > self.db.cfg.max_queue_bytes
        {
            return Err(Error::BUSY);
        }
        let shard_capacity = self.db.payload_capacity(&mut t, &c.workspace).await?;
        if t.exec("UPDATE payload_shards SET payload_bytes=payload_bytes+? WHERE id=? AND payload_bytes+?<=?",args![total,self.db.shard(&c.workspace),total,shard_capacity]).await?==0 {return Err(Error::BUSY);}
        let at = now();
        let expires = (e.expires_at as f64).min(at + self.db.cfg.queue_ttl as f64);
        t.exec("INSERT INTO encrypted_requests(id,workspace_id,device_id,node_id,route_id,body_hash,body,body_bytes,state,reserved_bytes,created,updated,expires,metadata_bytes) VALUES(?,?,?,?,?,?,'',?,'queued',?,?,?,?,?)",args![id,&c.workspace,&c.id,&e.gateway_id,&e.route_id,hash,encoded.len(),ENCRYPTED_SMALL_MAX,at,at,expires,ENCRYPTED_METADATA_BYTES]).await?;
        for (sequence, chunk) in encoded.as_bytes().chunks(MIB).enumerate() {
            let chunk = std::str::from_utf8(chunk).map_err(|_| Error::BAD)?;
            t.exec(
                "INSERT INTO encrypted_request_chunks(request_id,sequence,body) VALUES(?,?,?)",
                args![id, sequence, chunk],
            )
            .await?;
        }
        t.exec("UPDATE workspace_usage SET payload_bytes=payload_bytes+?,active=active+1,mutations=mutations+1 WHERE workspace_id=?",args![total,&c.workspace]).await?;
        let topic = format!("node:{}", e.gateway_id);
        t.notify(&topic).await?;
        t.commit().await?;
        self.db.signal(topic);
        Ok((
            StatusCode::ACCEPTED,
            json!({"request_id":id,"state":"queued","envelope":null}),
        ))
    }
    pub async fn encrypted_receipt(&self, c: &Credential, id: &str) -> Result<Value> {
        self.authorized(c).await?;
        let row=self.db.one("SELECT id,state,result FROM encrypted_requests WHERE id=? AND workspace_id=? AND device_id=?",args![id,&c.workspace,&c.id]).await?.ok_or(Error::NOT_FOUND)?;
        receipt(&row)
    }
    pub async fn encrypted_pending(&self, c: &Credential) -> Result<bool> {
        Ok(self.db.one("SELECT id FROM encrypted_requests WHERE node_id=? AND workspace_id=? AND state='queued' AND expires>? ORDER BY created,id LIMIT 1",args![&c.id,&c.workspace,now()]).await?.is_some())
    }
    pub async fn encrypted_claim(&self, c: &Credential) -> Result<Value> {
        let body = self.encrypted_claim_body(c).await?;
        Ok(match body {
            Some(body) => json!({"requests":[crate::json::parse(body.as_bytes(),32,50000)?]}),
            None => json!({"requests":[]}),
        })
    }
    pub async fn encrypted_claim_body(&self, c: &Credential) -> Result<Option<String>> {
        let mut t = self.db.begin().await?;
        workspace(&mut t, &c.workspace).await?;
        self.authorized_tx(&mut t, c).await?;
        member(&mut t, &c.workspace, &c.id).await?;
        let Some(row)=t.one("SELECT id,body_bytes FROM encrypted_requests WHERE node_id=? AND workspace_id=? AND state='queued' AND expires>? ORDER BY created,id LIMIT 1 FOR UPDATE SKIP LOCKED",args![&c.id,&c.workspace,now()]).await? else{t.commit().await?;return Ok(None);};
        let size = usize::try_from(i(&row, "body_bytes")).map_err(|_| Error::LARGE)?;
        if size > ENCRYPTED_REQUEST_MAX {
            return Err(Error::LARGE);
        }
        let chunks=t.all("SELECT sequence,body FROM encrypted_request_chunks WHERE request_id=? ORDER BY sequence LIMIT 42",args![&row["id"]]).await?;
        let mut body = String::with_capacity(size);
        for (sequence, chunk) in chunks.into_iter().enumerate() {
            if i(&chunk, "sequence") != sequence as i64
                || s(&chunk, "body").len() > MIB
                || body.len() + s(&chunk, "body").len() > size
            {
                return Err(Error::LARGE);
            }
            body.push_str(s(&chunk, "body"));
        }
        if body.len() != size {
            return Err(Error::UNAVAILABLE);
        }
        t.exec("UPDATE encrypted_requests SET state='claimed',claimed=?,updated=?,expires=? WHERE id=?",args![now(),now(),now()+self.db.cfg.claim_ttl as f64,&row["id"]]).await?;
        t.commit().await?;
        Ok(Some(body))
    }
    pub async fn encrypted_result(
        &self,
        c: &Credential,
        id: &str,
        body: &EncryptedSubmission,
    ) -> Result<()> {
        let (e, hash, encoded) = (&body.header, &body.hash, &body.encoded);
        if encoded.len() > ENCRYPTED_SMALL_MAX - 4096 || e.request_id != id {
            return Err(Error::LARGE);
        }
        let mut t = self.db.begin().await?;
        t.one("SELECT id FROM global_usage WHERE id=1 FOR UPDATE", &[])
            .await?;
        let state = workspace(&mut t, &c.workspace).await?;
        self.authorized_tx(&mut t, c).await?;
        let row = t
            .one(
                "SELECT id,device_id,route_id,state,result_hash,reserved_bytes,result_bytes FROM encrypted_requests WHERE id=? AND workspace_id=? AND node_id=?",
                args![id, &c.workspace, &c.id],
            )
            .await?
            .ok_or(Error::NOT_FOUND)?;
        if row["result_hash"] == *hash {
            t.commit().await?;
            return Ok(());
        }
        if row["state"] != "claimed" {
            return Err(Error::CONFLICT);
        }
        verify(&mut t, c, e, PayloadKind::Result, &state).await?;
        if e.recipient_device_id != s(&row, "device_id") {
            return Err(Error::UNAUTHORIZED);
        }
        if s(&row, "route_id") != e.route_id {
            return Err(Error::CONFLICT);
        }
        // The exact accepted digest above remains recoverable after expiry.
        // A never-accepted frozen result cannot become fresh ciphertext or
        // new native execution authority; let its sender retire delivery.
        if e.expires_at as f64 <= now() {
            return Err(Error(
                StatusCode::GONE,
                "encrypted result expired before delivery",
            ));
        }
        self.db.payload_usage(&mut t, &c.workspace).await?;
        let delta = encoded.len() as i64 - i(&row, "reserved_bytes") - i(&row, "result_bytes");
        if delta > 0 {
            return Err(Error::LARGE);
        }
        self.db.bytes(&mut t, &c.workspace, delta).await?;
        t.exec("UPDATE encrypted_requests SET state='completed',result=?,result_hash=?,result_bytes=?,reserved_bytes=0,updated=?,expires=? WHERE id=?",args![encoded,hash,encoded.len(),now(),now()+self.db.cfg.retention as f64,id]).await?;
        t.exec(
            "UPDATE workspace_usage SET active=active-1 WHERE workspace_id=?",
            args![&c.workspace],
        )
        .await?;
        self.append_event(&mut t, &c.workspace, &c.id, "", "request_result", None)
            .await?;
        let topic = format!("workspace:{}", c.workspace);
        t.notify(&topic).await?;
        t.commit().await?;
        self.db.signal(topic);
        Ok(())
    }
    pub async fn encrypted_snapshot(
        &self,
        c: &Credential,
        body: &EncryptedSubmission,
    ) -> Result<()> {
        let (e, encoded) = (&body.header, &body.encoded);
        if encoded.len() > ENCRYPTED_SMALL_MAX - 4096 {
            return Err(Error::LARGE);
        }
        let mut t = self.db.begin().await?;
        t.one("SELECT id FROM global_usage WHERE id=1 FOR UPDATE", &[])
            .await?;
        let state = workspace(&mut t, &c.workspace).await?;
        self.authorized_tx(&mut t, c).await?;
        verify(&mut t, c, e, PayloadKind::Snapshot, &state).await?;
        let computer_id = body.computer_id.as_deref().unwrap_or(&c.id);
        let route = &e.route_id;
        protocol::uuid(&json!(route), false)?;
        bind_route(self, &mut t, c, route, computer_id).await?;
        if let Some(existing) = t
            .one(
                "SELECT computer_id FROM encrypted_routes WHERE node_id=? AND route_id=?",
                args![&c.id, route],
            )
            .await?
        {
            if existing["computer_id"] != computer_id {
                return Err(Error::CONFLICT);
            }
        } else {
            let count = t
                .one(
                    "SELECT count(*) AS count FROM encrypted_routes WHERE node_id=?",
                    args![&c.id],
                )
                .await?
                .ok_or(Error::BAD)?;
            if i(&count, "count") >= 256 {
                return Err(Error::BUSY);
            }
            t.exec("INSERT INTO encrypted_routes(node_id,route_id,computer_id,workspace_id,last_seen) VALUES(?,?,?,?,?)",args![&c.id,route,computer_id,&c.workspace,now()]).await?;
        }
        let usage = self.db.payload_usage(&mut t, &c.workspace).await?;
        let old = t
            .one(
                "SELECT body_bytes FROM encrypted_snapshots WHERE node_id=? AND route_id=? AND device_id=?",
                args![&c.id, route, &e.recipient_device_id],
            )
            .await?;
        let delta = encoded.len() as i64 - old.as_ref().map_or(0, |r| i(r, "body_bytes"));
        if i(&usage, "payload_bytes") + delta > self.db.cfg.max_queue_bytes {
            return Err(Error::BUSY);
        }
        if delta > 0 {
            let shard_capacity = self.db.payload_capacity(&mut t, &c.workspace).await?;
            if t.exec("UPDATE payload_shards SET payload_bytes=payload_bytes+? WHERE id=? AND payload_bytes+?<=?",args![delta,self.db.shard(&c.workspace),delta,shard_capacity]).await?==0{return Err(Error::BUSY);}
            t.exec(
                "UPDATE workspace_usage SET payload_bytes=payload_bytes+? WHERE workspace_id=?",
                args![delta, &c.workspace],
            )
            .await?;
        } else {
            self.db.bytes(&mut t, &c.workspace, delta).await?;
        }
        t.exec("INSERT INTO encrypted_snapshots(node_id,route_id,device_id,workspace_id,body,body_bytes,updated) VALUES(?,?,?,?,?,?,?) ON CONFLICT(node_id,route_id,device_id) DO UPDATE SET body=excluded.body,body_bytes=excluded.body_bytes,updated=excluded.updated",args![&c.id,route,&e.recipient_device_id,&c.workspace,encoded,encoded.len(),now()]).await?;
        t.exec(
            "UPDATE nodes SET last_seen=? WHERE id=?",
            args![now(), &c.id],
        )
        .await?;
        t.exec(
            "UPDATE encrypted_routes SET last_seen=? WHERE node_id=? AND route_id=?",
            args![now(), &c.id, route],
        )
        .await?;
        self.append_event(&mut t, &c.workspace, computer_id, "", "snapshot", None)
            .await?;
        let topic = format!("workspace:{}", c.workspace);
        t.notify(&topic).await?;
        t.commit().await?;
        self.db.signal(topic);
        Ok(())
    }
    pub async fn encrypted_computers(
        &self,
        c: &Credential,
        after: &str,
        limit: usize,
    ) -> Result<Value> {
        self.authorized(c).await?;
        let cursor: Vec<String> = if after.is_empty() {
            vec![String::new(); 3]
        } else {
            let bytes = crypto(zerus_mobile_crypto::decode(after, 512))?;
            let parsed: Vec<String> = serde_json::from_slice(&bytes).map_err(|_| Error::BAD)?;
            if parsed.len() != 3 {
                return Err(Error::BAD);
            }
            for id in &parsed {
                protocol::uuid(&json!(id), false)?;
            }
            parsed
        };
        let mut t = self.db.begin().await?;
        let rows=t.all("SELECT r.node_id,r.route_id,r.computer_id,r.last_seen,COALESCE(e.body_bytes,0) AS body_bytes FROM encrypted_routes r JOIN nodes n ON n.id=r.node_id LEFT JOIN encrypted_snapshots e ON e.node_id=r.node_id AND e.route_id=r.route_id AND e.device_id=? WHERE r.workspace_id=? AND n.revoked=0 AND (r.computer_id,r.node_id,r.route_id)>(?,?,?) ORDER BY r.computer_id,r.node_id,r.route_id LIMIT ?",args![&c.id,&c.workspace,&cursor[0],&cursor[1],&cursor[2],limit+1]).await?;
        let mut computers = Vec::new();
        let mut bytes = 0;
        let mut more = rows.len() > limit;
        for row in rows.iter().take(limit) {
            if bytes + i(row, "body_bytes") > 2 * MIB as i64 {
                more = true;
                break;
            }
            let encrypted=t.one("SELECT body FROM encrypted_snapshots WHERE node_id=? AND route_id=? AND device_id=?",args![&row["node_id"],&row["route_id"],&c.id]).await?;
            let envelope = encrypted
                .as_ref()
                .map(|r| serde_json::from_str::<Value>(s(r, "body")))
                .transpose()
                .map_err(|_| Error::UNAVAILABLE)?;
            computers.push(json!({"computer_id":row["computer_id"],"gateway_id":row["node_id"],"route_id":row["route_id"],"online":f(row,"last_seen")>now()-self.db.cfg.online_timeout as f64,"envelope":envelope}));
            bytes += i(row, "body_bytes");
        }
        t.commit().await?;
        let next = if more {
            computers.last().map(|r| {
                zerus_mobile_crypto::encode(
                    serde_json::to_string(&[
                        s(r, "computer_id"),
                        s(r, "gateway_id"),
                        s(r, "route_id"),
                    ])
                    .expect("string list")
                    .as_bytes(),
                )
            })
        } else {
            None
        };
        Ok(json!({"next_after":next,"computers":computers}))
    }
    pub async fn encrypted_routes(&self, c: &Credential) -> Result<Value> {
        self.authorized(c).await?;
        let mut t = self.db.begin().await?;
        member(&mut t, &c.workspace, &c.id).await?;
        let routes=t.all("SELECT route_id,computer_id FROM encrypted_routes WHERE node_id=? ORDER BY route_id LIMIT 256",args![&c.id]).await?;
        t.commit().await?;
        Ok(json!({"routes":routes}))
    }
    pub async fn encrypted_routes_manifest(&self, c: &Credential, body: &Value) -> Result<()> {
        protocol::fields(body, &["routes"], &[])?;
        let routes = body["routes"]
            .as_array()
            .filter(|v| v.len() <= 256)
            .ok_or(Error::BAD)?;
        let mut manifest = std::collections::BTreeMap::new();
        for route in routes {
            protocol::fields(route, &["route_id", "computer_id"], &[])?;
            let id = protocol::uuid(&route["route_id"], false)?;
            let computer = protocol::uuid(&route["computer_id"], false)?;
            if manifest.insert(id, computer).is_some() {
                return Err(Error::BAD);
            }
        }
        let mut t = self.db.begin().await?;
        t.one("SELECT id FROM global_usage WHERE id=1 FOR UPDATE", &[])
            .await?;
        workspace(&mut t, &c.workspace).await?;
        self.authorized_tx(&mut t, c).await?;
        member(&mut t, &c.workspace, &c.id).await?;
        self.db.payload_usage(&mut t, &c.workspace).await?;
        let existing=t.all("SELECT route_id,computer_id FROM encrypted_routes WHERE node_id=? ORDER BY route_id LIMIT 256",args![&c.id]).await?;
        let mut changed = existing.len() != manifest.len();
        for row in &existing {
            if manifest.get(s(row, "route_id")) != Some(&s(row, "computer_id")) {
                changed = true;
                let removed=t.all("DELETE FROM encrypted_snapshots WHERE node_id=? AND route_id=? RETURNING body_bytes",args![&c.id,&row["route_id"]]).await?;
                for snapshot in removed {
                    self.db
                        .bytes(&mut t, &c.workspace, -i(&snapshot, "body_bytes"))
                        .await?;
                }
                self.delete_opaque_signals(&mut t, &c.workspace, &c.id, Some(s(row, "route_id")))
                    .await?;
                t.exec(
                    "DELETE FROM encrypted_routes WHERE node_id=? AND route_id=?",
                    args![&c.id, &row["route_id"]],
                )
                .await?;
            }
        }
        for (route, computer) in manifest {
            bind_route(self, &mut t, c, route, computer).await?;
            t.exec("INSERT INTO encrypted_routes(node_id,route_id,computer_id,workspace_id,last_seen) VALUES(?,?,?,?,?) ON CONFLICT(node_id,route_id) DO UPDATE SET last_seen=excluded.last_seen",args![&c.id,route,computer,&c.workspace,now()]).await?;
        }
        t.exec(
            "UPDATE nodes SET last_seen=? WHERE id=?",
            args![now(), &c.id],
        )
        .await?;
        if changed {
            self.append_event(&mut t, &c.workspace, &c.id, "", "routes", None)
                .await?;
            t.notify(&format!("workspace:{}", c.workspace)).await?;
        }
        t.commit().await?;
        if changed {
            self.db.signal(format!("workspace:{}", c.workspace));
        }
        Ok(())
    }
    pub async fn withdraw_encrypted_route(&self, c: &Credential, route: &str) -> Result<()> {
        protocol::uuid(&json!(route), false)?;
        let mut t = self.db.begin().await?;
        workspace(&mut t, &c.workspace).await?;
        self.authorized_tx(&mut t, c).await?;
        member(&mut t, &c.workspace, &c.id).await?;
        self.db.payload_usage(&mut t, &c.workspace).await?;
        let rows=t.all("DELETE FROM encrypted_snapshots WHERE node_id=? AND route_id=? RETURNING body_bytes",args![&c.id,route]).await?;
        for row in rows {
            self.db
                .bytes(&mut t, &c.workspace, -i(&row, "body_bytes"))
                .await?;
        }
        t.exec(
            "DELETE FROM encrypted_routes WHERE node_id=? AND route_id=?",
            args![&c.id, route],
        )
        .await?;
        self.delete_opaque_signals(&mut t, &c.workspace, &c.id, Some(route))
            .await?;
        t.commit().await
    }
    pub(super) async fn retire_encrypted_tx(&self, t: &mut Tx, w: &str, id: &str) -> Result<()> {
        // Revocation only cancels work that was never claimed. A claimed native
        // mutation remains uncertain and can never become a fresh execution.
        let rows=t.all("SELECT id,body_bytes,result_bytes,reserved_bytes,state FROM encrypted_requests WHERE workspace_id=? AND (node_id=? OR device_id=?) AND state IN ('queued','claimed') LIMIT 1000",args![w,id,id]).await?;
        for row in rows {
            t.exec(
                "DELETE FROM encrypted_request_chunks WHERE request_id=?",
                args![&row["id"]],
            )
            .await?;
            self.db
                .bytes(
                    t,
                    w,
                    -i(&row, "body_bytes") - i(&row, "result_bytes") - i(&row, "reserved_bytes"),
                )
                .await?;
            t.exec("UPDATE encrypted_requests SET state='uncertain',body='',body_bytes=0,result=NULL,result_bytes=0,reserved_bytes=0,updated=?,expires=? WHERE id=?",args![now(),now()+self.db.cfg.retention as f64,&row["id"]]).await?;
            t.exec(
                "UPDATE workspace_usage SET active=active-1 WHERE workspace_id=?",
                args![w],
            )
            .await?;
        }
        let snapshots=t.all("DELETE FROM encrypted_snapshots WHERE workspace_id=? AND (node_id=? OR device_id=?) RETURNING body_bytes",args![w,id,id]).await?;
        for row in snapshots {
            self.db.bytes(t, w, -i(&row, "body_bytes")).await?;
        }
        t.exec("DELETE FROM encrypted_routes WHERE node_id=?", args![id])
            .await?;
        self.delete_opaque_signals(t, w, id, None).await?;
        t.exec("UPDATE computer_routes SET active=0,snapshot=NULL,snapshot_hash=NULL,last_seen=NULL WHERE gateway_id=?",args![id]).await?;
        Ok(())
    }
    pub async fn maintain_encrypted(&self) -> Result<()> {
        // Bounded indexed expiry; receipt IDs and hashes are never deleted.
        let mut t = self.db.begin().await?;
        let rows=t.all("SELECT id,workspace_id FROM encrypted_requests WHERE expires<=? AND (body_bytes>0 OR result_bytes>0 OR reserved_bytes>0) ORDER BY expires,id LIMIT ?",args![now(),self.db.cfg.maintenance_batch]).await?;
        t.commit().await?;
        for row in rows {
            let w = s(&row, "workspace_id");
            let mut t = self.db.begin().await?;
            workspace(&mut t, w).await?;
            self.db.payload_usage(&mut t, w).await?;
            if let Some(r) = t
                .one(
                    "SELECT * FROM encrypted_requests WHERE id=? AND expires<=?",
                    args![&row["id"], now()],
                )
                .await?
            {
                t.exec(
                    "DELETE FROM encrypted_request_chunks WHERE request_id=?",
                    args![&r["id"]],
                )
                .await?;
                self.db
                    .bytes(
                        &mut t,
                        w,
                        -i(&r, "body_bytes") - i(&r, "result_bytes") - i(&r, "reserved_bytes"),
                    )
                    .await?;
                let active = ["queued", "claimed"].contains(&s(&r, "state"));
                t.exec("UPDATE encrypted_requests SET state=?,body='',body_bytes=0,result=NULL,result_bytes=0,reserved_bytes=0,updated=? WHERE id=?",args![if active{"uncertain"}else{s(&r,"state")},now(),&row["id"]]).await?;
                if active {
                    t.exec(
                        "UPDATE workspace_usage SET active=active-1 WHERE workspace_id=?",
                        args![w],
                    )
                    .await?;
                }
            }
            t.commit().await?;
        }
        Ok(())
    }
}
