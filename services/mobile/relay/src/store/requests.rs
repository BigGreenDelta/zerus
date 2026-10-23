use super::*;
use crate::{
    db::{f, Tx},
    protocol::{self, MIB, READS},
    registry,
};
use axum::http::StatusCode;
// An old database can contain records predating the wire limit. Bound selected
// text at the database boundary, before a driver allocates its result buffer.
const RECEIPT: &str = "id,workspace_id,device_id,node_id,operation,body_hash,state,
CASE WHEN BYTES(result AS_BYTES)<=1048576 THEN result END AS result,
CASE WHEN BYTES(error AS_BYTES)<=1048576 THEN error END AS error,
CASE WHEN COALESCE(BYTES(result AS_BYTES),0)+COALESCE(BYTES(error AS_BYTES),0)>1048576 THEN 1 ELSE 0 END AS oversized,
created,claimed,updated,expires_at,body_bytes,result_bytes,reserved_bytes";
/// A claim leaves storage only after COMMIT. Decoding/HTTP failures cannot requeue it.
pub struct Claim {
    pub body: String,
    pub gateway: Option<Value>,
    pub expires: Value,
}
impl Claim {
    pub fn decode(self) -> Result<Value> {
        let mut b = crate::json::parse(self.body.as_bytes(), 32, 50000)?;
        b.as_object_mut().ok_or(Error::BAD)?.remove("computer_id");
        if let Some(g) = self.gateway {
            b["gateway_route"] = g;
        }
        if !self.expires.is_null() {
            b["expires_at"] = self.expires;
        }
        Ok(Value::Object(serde_json::Map::from_iter([(
            "requests".to_owned(),
            Value::Array(vec![b]),
        )])))
    }
}
/// Validated metadata plus one owned canonical buffer. Construct off the HTTP
/// runtime; the original attachment object tree is dropped before SQL encoding.
pub struct Submission {
    id: String,
    computer: String,
    operation: String,
    encoded: String,
    hash: String,
    after: bool,
    agent: bool,
    attachments: bool,
    video: bool,
    dismiss_prompt_present: bool,
    speech_job: Option<String>,
    speech_config_operation: Option<String>,
}
impl Submission {
    pub fn new(body: Value) -> Result<Self> {
        protocol::request(&body)?;
        let encoded = canonical(&body)?;
        let hash = digest(&encoded);
        Ok(Self {
            id: s(&body, "request_id").into(),
            computer: s(&body, "computer_id").into(),
            operation: s(&body, "operation").into(),
            after: body["payload"].get("after").is_some(),
            agent: body["payload"].get("agent_id").is_some(),
            video: body["operation"] == "voice_submit" && body["payload"].get("video").is_some(),
            dismiss_prompt_present: body["operation"] == "clear_context"
                && body["payload"].get("dismiss_prompt").is_some(),
            attachments: body["payload"]["attachments"]
                .as_array()
                .is_some_and(|v| !v.is_empty()),
            speech_job: if ["speech_status", "speech_cancel"].contains(&s(&body, "operation")) {
                body["payload"]["job_id"].as_str().map(str::to_owned)
            } else {
                None
            },
            speech_config_operation: if s(&body, "operation") == "speech_config_status" {
                body["payload"]["operation_id"].as_str().map(str::to_owned)
            } else {
                None
            },
            encoded,
            hash,
        })
    }
}
impl Store {
    pub async fn submit(&self, c: &Credential, b: &Submission) -> Result<(StatusCode, Value)> {
        let hash = b.hash.as_str();
        let encoded = b.encoded.as_str();
        let id = b.id.as_str();
        let mut t = self.db.begin().await?;
        let usage = self.db.payload_usage(&mut t, &c.workspace).await?;
        self.authorized_tx(&mut t, c).await?;
        if let Some(row) = t
            .one(
                &format!("SELECT {RECEIPT} FROM requests WHERE id=?"),
                args![id],
            )
            .await?
        {
            if row["device_id"] != c.id || row["body_hash"] != hash {
                return Err(Error::CONFLICT);
            }
            let result = envelope(&row)?;
            t.commit().await?;
            return Ok((StatusCode::ACCEPTED, result));
        }
        let Some(node) = registry::select(&mut t, &c.workspace, &b.computer).await? else {
            return Err(
                if registry::resolve(&mut t, &c.workspace, &b.computer)
                    .await?
                    .is_some()
                {
                    Error::CONFLICT
                } else {
                    Error::NOT_FOUND
                },
            );
        };
        t.one(
            "SELECT id FROM nodes WHERE id=? AND revoked=0 FOR SHARE",
            args![&node["gateway_id"]],
        )
        .await?
        .ok_or(Error::UNAUTHORIZED)?;
        let snapshot: Value = match node["snapshot"].as_str() {
            Some(s) => crate::json::parse(s.as_bytes(), 32, 50000)?,
            None => Value::Null,
        };
        let op = b.operation.as_str();
        if let Some(job) = &b.speech_job {
            let owner = t.one("SELECT workspace_id,device_id,operation,target_computer_id,target_machine_id FROM requests WHERE id=?", args![job]).await?.ok_or(Error::NOT_FOUND)?;
            if owner["workspace_id"] != c.workspace
                || owner["device_id"] != c.id
                || owner["operation"] != "speech_submit"
                || owner["target_computer_id"] != node["id"]
                || owner["target_machine_id"] != node["machine_id"]
            {
                return Err(Error::NOT_FOUND);
            }
        }
        if let Some(operation_id) = &b.speech_config_operation {
            let owner = t.one("SELECT workspace_id,device_id,operation,target_computer_id,target_machine_id FROM requests WHERE id=?", args![operation_id]).await?.ok_or(Error::NOT_FOUND)?;
            if owner["workspace_id"] != c.workspace
                || owner["device_id"] != c.id
                || ![
                    "speech_profile_upsert",
                    "speech_profile_remove",
                    "speech_profile_test",
                ]
                .contains(&s(&owner, "operation"))
                || owner["target_computer_id"] != node["id"]
                || owner["target_machine_id"] != node["machine_id"]
            {
                return Err(Error::NOT_FOUND);
            }
        }
        if !["inspect", "send", "answer", "interrupt"].contains(&op)
            && !protocol::supports(&snapshot, op, "operations")
        {
            return Err(Error::CONFLICT);
        }
        for (present, feature) in [(b.after, "inspect_after"), (b.agent, "inspect_agent")] {
            if op == "inspect" && present && !protocol::supports(&snapshot, feature, "features") {
                return Err(Error::CONFLICT);
            }
        }
        if op == "send" && b.agent && !protocol::supports(&snapshot, "send_agent", "features") {
            return Err(Error::CONFLICT);
        }
        if b.video && !protocol::supports(&snapshot, "voice_video", "features") {
            return Err(Error::CONFLICT);
        }
        if b.dismiss_prompt_present
            && !protocol::supports(&snapshot, "clear_context_dismiss", "features")
        {
            return Err(Error::CONFLICT);
        }
        if op == "terminal_input"
            && (node["last_seen"].is_null() || now() - f(&node, "last_seen") > 45.0)
        {
            return Err(Error::CONFLICT);
        }
        let read = READS.contains(&op);
        let bytes = encoded.len() as i64 + if node["machine_id"].is_null() { 0 } else { 144 };
        let total = bytes + MIB as i64;
        let small = bytes <= 65536 && !b.attachments;
        if i(&usage, "active") >= self.db.cfg.max_queue {
            return Err(Error::QUEUE_FULL);
        }
        if i(&usage, if read { "reads" } else { "mutations" }) >= if read { 5000 } else { 100000 } {
            return Err(Error::RETAINED_REQUESTS);
        }
        if i(&usage, "payload_bytes") + total
            > self.db.cfg.max_queue_bytes + if small { MIB as i64 } else { 0 }
        {
            return Err(Error::RETAINED_PAYLOAD);
        }
        let shard_capacity = self.db.payload_capacity(&mut t, &c.workspace).await?;
        if t.exec("UPDATE payload_shards SET payload_bytes=payload_bytes+? WHERE id=? AND payload_bytes+?<=?",args![total,self.db.shard(&c.workspace),total,shard_capacity]).await?==0{return Err(Error::BUSY);}
        let at = now();
        let expires = if op == "terminal_input" {
            json!(at + 5.0)
        } else {
            Value::Null
        };
        let route = if i(&node, "local") != 0 {
            Value::Null
        } else {
            node["route_id"].clone()
        };
        let inserted=t.exec("INSERT INTO requests(id,workspace_id,device_id,node_id,operation,body_hash,body_bytes,body,state,created,updated,expires_at,reserved_bytes,target_computer_id,target_machine_id,route_id) VALUES(?,?,?,?,?,?,?,?,'queued',?,?,CAST(? AS DOUBLE PRECISION),?,?,?,?) ON CONFLICT(id) DO NOTHING",args![id,&c.workspace,&c.id,&node["gateway_id"],op,hash,bytes,encoded,at,at,&expires,MIB,&node["id"],&node["machine_id"],&route]).await?;
        if inserted == 0 {
            return Err(Error::CONFLICT);
        } // Transaction rollback restores reservations.
        t.exec("UPDATE workspace_usage SET payload_bytes=payload_bytes+?,active=active+1,reads=reads+?,mutations=mutations+? WHERE workspace_id=?",args![total,read,!read,&c.workspace]).await?;
        let topic = format!("node:{}", s(&node, "gateway_id"));
        t.notify(&topic).await?;
        t.commit().await?;
        self.db.signal(topic);
        Ok((
            StatusCode::ACCEPTED,
            json!({"request_id":id,"state":"queued","result":null,"error":null}),
        ))
    }
    pub(super) async fn expire(&self, t: &mut Tx, r: &Value, restart: bool) -> Result<bool> {
        let at = now();
        let expired = if r["state"] == "queued" {
            if !r["expires_at"].is_null() && f(r, "expires_at") <= at {
                Some(("failed", "terminal input expired before delivery"))
            } else if f(r, "created") <= at - self.db.cfg.queue_ttl as f64 {
                Some(("failed", "request expired before delivery"))
            } else {
                None
            }
        } else if r["state"] == "claimed"
            && (restart || f(r, "claimed") <= at - self.db.cfg.claim_ttl as f64)
        {
            Some(("uncertain", "node result timeout after claim"))
        } else {
            None
        };
        let Some((state, error)) = expired else {
            return Ok(false);
        };
        t.exec("UPDATE requests SET state=?,error=?,updated=?,reserved_bytes=0,result_bytes=? WHERE id=?",args![state,error,at,error.len(),&r["id"]]).await?;
        self.db
            .bytes(
                t,
                s(r, "workspace_id"),
                error.len() as i64 - i(r, "reserved_bytes") - i(r, "result_bytes"),
            )
            .await?;
        t.exec(
            "UPDATE workspace_usage SET active=active-1 WHERE workspace_id=?",
            args![&r["workspace_id"]],
        )
        .await?;
        Ok(true)
    }
    pub async fn receipt(&self, c: &Credential, id: &str) -> Result<Value> {
        // Only expiring receipts enter byte accounting; ordinary reads avoid it.
        let mut t = self.db.begin().await?;
        let r = t
            .one(
                &format!("SELECT {RECEIPT} FROM requests WHERE id=? AND device_id=?"),
                args![id, &c.id],
            )
            .await?
            .ok_or(Error::NOT_FOUND)?;
        let expired = (r["state"] == "queued"
            && (f(&r, "created") <= now() - self.db.cfg.queue_ttl as f64
                || (!r["expires_at"].is_null() && f(&r, "expires_at") <= now())))
            || (r["state"] == "claimed"
                && f(&r, "claimed") <= now() - self.db.cfg.claim_ttl as f64);
        if expired {
            self.db.payload_usage(&mut t, &c.workspace).await?;
        }
        self.authorized_tx(&mut t, c).await?;
        let row = if expired {
            let r = t
                .one(
                    &format!(
                        "SELECT {RECEIPT} FROM requests WHERE id=? AND device_id=? FOR UPDATE"
                    ),
                    args![id, &c.id],
                )
                .await?
                .ok_or(Error::NOT_FOUND)?;
            self.expire(&mut t, &r, false).await?;
            t.one(
                &format!("SELECT {RECEIPT} FROM requests WHERE id=?"),
                args![id],
            )
            .await?
            .ok_or(Error::NOT_FOUND)?
        } else {
            r
        };
        if ["terminal_snapshot", "attachment_read", "file_read"].contains(&s(&row, "operation"))
            && ["completed", "failed", "uncertain"].contains(&s(&row, "state"))
        {
            t.exec(
                "UPDATE requests SET result_read=? WHERE id=?",
                args![now(), id],
            )
            .await?;
        }
        let result = envelope(&row)?;
        t.commit().await?;
        Ok(result)
    }
    pub async fn pending(&self, c: &Credential, allow: bool) -> Result<bool> {
        Ok(self.db.one("SELECT r.id FROM requests r JOIN nodes n ON n.id=r.node_id WHERE r.node_id=? AND n.workspace_id=? AND n.revoked=0 AND r.state='queued' AND (r.target_machine_id IS NULL OR ?=1) AND r.created>? AND (r.expires_at IS NULL OR r.expires_at>?) LIMIT 1",args![&c.id,&c.workspace,allow,now()-self.db.cfg.queue_ttl as f64,now()]).await?.is_some())
    }
    pub async fn claim(&self, c: &Credential, allow: bool) -> Result<Option<Claim>> {
        let mut t = self.db.begin().await?;
        self.db.payload_usage(&mut t, &c.workspace).await?;
        self.authorized_tx(&mut t, c).await?;
        // Metadata first. Reject legacy oversized rows without materializing them.
        let row=t.one("SELECT id,workspace_id,node_id,device_id,target_computer_id,target_machine_id,route_id,expires_at,BYTES(body AS_BYTES) AS wire_bytes FROM requests WHERE node_id=? AND state='queued' AND (target_machine_id IS NULL OR ?=1) AND created>? AND (expires_at IS NULL OR expires_at>?) ORDER BY created,id LIMIT 1 FOR UPDATE SKIP LOCKED",args![&c.id,allow,now()-self.db.cfg.queue_ttl as f64,now()]).await?;
        let Some(r) = row else {
            t.commit().await?;
            return Ok(None);
        };
        if !registry::frozen(&mut t, &r, allow).await?
            || t.one(
                "SELECT id FROM devices WHERE id=? AND revoked=0 FOR SHARE",
                args![&r["device_id"]],
            )
            .await?
            .is_none()
        {
            registry::invalidate(
                &self.db,
                &mut t,
                "id=?",
                args![&r["id"]],
                "gateway route withdrawn before delivery",
            )
            .await?;
            t.commit().await?;
            return Ok(None);
        }
        if i(&r, "wire_bytes") > 32 * MIB as i64 - 4096 {
            return Err(Error::LARGE);
        }
        let mut body = t
            .one("SELECT body FROM requests WHERE id=?", args![&r["id"]])
            .await?
            .ok_or(Error::NOT_FOUND)?;
        let at = now();
        t.exec(
            "UPDATE requests SET state='claimed',claimed=?,updated=? WHERE id=? AND state='queued'",
            args![at, at, &r["id"]],
        )
        .await?;
        let gateway = if r["target_machine_id"].is_null() {
            None
        } else {
            Some(
                json!({"schema":1,"route_id":r["route_id"],"computer_id":r["target_computer_id"],"machine_id":r["target_machine_id"]}),
            )
        };
        let claim = Claim {
            body: match body["body"].take() {
                Value::String(s) => s,
                _ => return Err(Error::BAD),
            },
            gateway,
            expires: r["expires_at"].clone(),
        };
        t.commit().await?;
        Ok(Some(claim))
    }
    pub async fn result(
        &self,
        c: &Credential,
        id: &str,
        b: &Value,
        encoded: Option<&str>,
    ) -> Result<()> {
        let size = encoded.map_or(0, str::len) + b["error"].as_str().map_or(0, str::len);
        if size > MIB {
            return Err(Error::LARGE);
        }
        let result = encoded.map_or(Value::Null, |s| json!(s));
        let mut t = self.db.begin().await?;
        self.db.payload_usage(&mut t, &c.workspace).await?;
        self.authorized_tx(&mut t, c).await?;
        let row = t
            .one(
                &format!("SELECT {RECEIPT} FROM requests WHERE id=? AND node_id=? FOR UPDATE"),
                args![id, &c.id],
            )
            .await?
            .ok_or(Error::NOT_FOUND)?;
        if self.expire(&mut t, &row, false).await? {
            t.commit().await?;
            return Err(Error::CONFLICT);
        }
        if row["state"] != "claimed" {
            if row["state"] == b["state"] && row["result"] == result && row["error"] == b["error"] {
                t.commit().await?;
                return Ok(());
            }
            return Err(Error::CONFLICT);
        }
        let audio_bytes = if [
            "speech_submit",
            "voice_submit",
            "speech_profile_upsert",
            "speech_profile_remove",
            "speech_profile_test",
        ]
        .contains(&s(&row, "operation"))
        {
            i(&row, "body_bytes")
        } else {
            0
        };
        let delta = size as i64 - i(&row, "reserved_bytes") - i(&row, "result_bytes") - audio_bytes;
        if delta > 0 {
            let shard_capacity = self.db.payload_capacity(&mut t, &c.workspace).await?;
            if t.exec("UPDATE payload_shards SET payload_bytes=payload_bytes+? WHERE id=? AND payload_bytes+?<=?",args![delta,self.db.shard(&c.workspace),delta,shard_capacity]).await?==0{return Err(Error::BUSY);}
        } else {
            t.exec(
                "UPDATE payload_shards SET payload_bytes=payload_bytes+? WHERE id=?",
                args![delta, self.db.shard(&c.workspace)],
            )
            .await?;
        }
        t.exec("UPDATE requests SET state=?,result=?,error=?,updated=?,result_bytes=?,reserved_bytes=0 WHERE id=?",args![&b["state"],&result,&b["error"],now(),size,id]).await?;
        if audio_bytes > 0 {
            t.exec(
                "UPDATE requests SET body='',body_bytes=0 WHERE id=?",
                args![id],
            )
            .await?;
        }
        t.exec("UPDATE workspace_usage SET active=active-1,payload_bytes=payload_bytes+? WHERE workspace_id=?",args![delta,&c.workspace]).await?;
        t.commit().await
    }
}
