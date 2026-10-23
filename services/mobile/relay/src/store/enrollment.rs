//! Public registration transports public signed membership only. Invitation
//! secrets and device private keys never enter this module or its database.
use super::*;
use crate::{db::Tx, protocol};
use zerus_mobile_crypto::{
    DevicePublic, EnrollmentCandidate, Role as DeviceRole, SignedEnrollment, SignedMembership,
    SignedMembershipSnapshot,
};

pub(super) fn crypto<T>(value: zerus_mobile_crypto::Result<T>) -> Result<T> {
    value.map_err(|_| Error::UNAUTHORIZED)
}
pub(super) fn parse<T: serde::de::DeserializeOwned>(value: &Value) -> Result<T> {
    serde_json::from_value(value.clone()).map_err(|_| Error::BAD)
}
fn service_secret(value: &Value) -> Result<&str> {
    let text = protocol::text(value, 43, false)?;
    crypto(zerus_mobile_crypto::decode_fixed::<32>(text))?;
    Ok(text)
}
fn role_name(role: DeviceRole) -> &'static str {
    match role {
        DeviceRole::Computer => "computer",
        DeviceRole::Phone => "phone",
    }
}
pub(super) async fn member(t: &mut Tx, w: &str, id: &str) -> Result<DevicePublic> {
    let row = t.one("SELECT public_record FROM encrypted_members WHERE workspace_id=? AND device_id=? AND revoked=0",args![w,id]).await?.ok_or(Error::UNAUTHORIZED)?;
    serde_json::from_str(s(&row, "public_record")).map_err(|_| Error::UNAVAILABLE)
}
pub(super) async fn workspace(t: &mut Tx, w: &str) -> Result<Value> {
    t.one(
        "SELECT * FROM encrypted_workspaces WHERE workspace_id=? FOR UPDATE",
        args![w],
    )
    .await?
    .ok_or(Error::UNAUTHORIZED)
}
fn head_result(w: &str, id: &str, state: &Value) -> Value {
    json!({"workspace_id":w,"node_id":id,"state":"active","membership_head":state["membership_head"],"membership_revision":state["membership_revision"]})
}
impl Store {
    pub async fn retirement_credential(&self, secret: &str) -> Result<(Credential, ServiceClass)> {
        let row=self.db.control_one(self.db.auth_class(secret),"SELECT n.id,n.workspace_id,w.service_class FROM nodes n JOIN encrypted_members m ON m.device_id=n.id AND m.workspace_id=n.workspace_id JOIN workspaces w ON w.id=n.workspace_id WHERE n.token_hash=? AND n.revoked=1 AND m.revoked=1",args![&digest(secret)]).await?.ok_or(Error::UNAUTHORIZED)?;
        Ok((
            Credential {
                id: s(&row, "id").into(),
                workspace: s(&row, "workspace_id").into(),
                role: Role::Node,
            },
            ServiceClass::parse(s(&row, "service_class"))?,
        ))
    }
    pub async fn encrypted_credential(&self, c: &Credential) -> Result<bool> {
        Ok(self
            .db
            .one(
                "SELECT device_id FROM encrypted_members WHERE workspace_id=? AND device_id=?",
                args![&c.workspace, &c.id],
            )
            .await?
            .is_some())
    }
    pub async fn create_workspace(&self, body: &Value) -> Result<Value> {
        protocol::fields(body, &["membership", "service_token"], &[])?;
        let membership: SignedMembership = parse(&body["membership"])?;
        crypto(membership.verify_genesis())?;
        let secret = service_secret(&body["service_token"])?;
        let statement = &membership.statement;
        let device = statement.device.as_ref().ok_or(Error::BAD)?;
        protocol::uuid(&json!(statement.workspace_id), false)?;
        protocol::uuid(&json!(device.device_id), false)?;
        let w = &statement.workspace_id;
        let id = &device.device_id;
        let head = crypto(membership.head())?;
        let encoded = canonical(&body["membership"])?;
        let mut t = self.db.begin().await?;
        // Registration serialization also makes lost Create responses exactly
        // recoverable with the original local body and service credential.
        let control = t
            .one(
                "SELECT workspaces FROM public_registration WHERE id=1 FOR UPDATE",
                &[],
            )
            .await?
            .ok_or(Error::BAD)?;
        if let Some(existing) = t
            .one(
                "SELECT * FROM encrypted_workspaces WHERE workspace_id=?",
                args![w],
            )
            .await?
        {
            let genesis = t
                .one(
                    "SELECT head FROM membership_changes WHERE workspace_id=? AND revision=0",
                    args![w],
                )
                .await?
                .ok_or(Error::CONFLICT)?;
            let node = t
                .one(
                    "SELECT token_hash FROM nodes WHERE id=? AND workspace_id=?",
                    args![id, w],
                )
                .await?
                .ok_or(Error::CONFLICT)?;
            if genesis["head"] != head || node["token_hash"] != digest(secret) {
                return Err(Error::CONFLICT);
            }
            t.commit().await?;
            return Ok(head_result(w, id, &existing));
        }
        if i(&control, "workspaces") >= self.db.cfg.max_public_workspaces {
            return Err(Error::BUSY);
        }
        if t.one("SELECT id FROM workspaces WHERE id=?", args![w])
            .await?
            .is_some()
        {
            return Err(Error::CONFLICT);
        }
        t.exec(
            "INSERT INTO workspaces(id,name) VALUES(?,'Workspace')",
            args![w],
        )
        .await?;
        t.exec(
            "INSERT INTO workspace_usage(workspace_id) VALUES(?)",
            args![w],
        )
        .await?;
        t.exec(
            "INSERT INTO nodes(id,workspace_id,name,token_hash) VALUES(?,?,'Computer',?)",
            args![id, w, &digest(secret)],
        )
        .await?;
        registry::enroll(&mut t, w, id, "Computer").await?;
        t.exec("INSERT INTO encrypted_workspaces(workspace_id,membership_head,membership_revision) VALUES(?,?,0)",args![w,&head]).await?;
        t.exec("INSERT INTO encrypted_members(workspace_id,device_id,role,public_record) VALUES(?,?,'computer',?)",args![w,id,&canonical(&json!(device))?]).await?;
        t.exec("INSERT INTO membership_changes(workspace_id,revision,change_id,head,record) VALUES(?,0,?,?,?)",args![w,&head,&head,&encoded]).await?;
        self.enrollment_bytes(
            &mut t,
            w,
            (encoded.len() + canonical(&json!(device))?.len() + 1024) as i64,
        )
        .await?;
        t.exec(
            "UPDATE public_registration SET workspaces=workspaces+1 WHERE id=1",
            &[],
        )
        .await?;
        t.commit().await?;
        Ok(
            json!({"workspace_id":w,"node_id":id,"state":"active","membership_head":head,"membership_revision":0}),
        )
    }
    pub async fn computer_invitation(&self, c: &Credential, body: &Value) -> Result<Value> {
        protocol::fields(
            body,
            &["invitation_id", "purpose", "expires_at", "bootstrap"],
            &[],
        )?;
        let bootstrap: SignedMembershipSnapshot = parse(&body["bootstrap"])?;
        let id = protocol::uuid(&body["invitation_id"], false)?;
        let purpose = protocol::text(&body["purpose"], 16, false)?;
        if !["computer", "phone"].contains(&purpose) {
            return Err(Error::BAD);
        }
        let expires = body["expires_at"].as_u64().ok_or(Error::BAD)?;
        let mut t = self.db.begin().await?;
        let state = workspace(&mut t, &c.workspace).await?;
        self.authorized_tx(&mut t, c).await?;
        let issuer = member(&mut t, &c.workspace, &c.id).await?;
        crypto(bootstrap.verify_signer(&issuer))?;
        if issuer.role != DeviceRole::Computer {
            return Err(Error::UNAUTHORIZED);
        }
        if let Some(existing) = t
            .one(
                "SELECT * FROM enrollment_invitations WHERE invitation_id=?",
                args![id],
            )
            .await?
        {
            if existing["workspace_id"] != c.workspace
                || existing["issuer_device_id"] != c.id
                || existing["purpose"] != purpose
                || existing["expires"].as_f64() != Some(expires as f64)
                || existing["bootstrap"] != canonical(&body["bootstrap"])?
            {
                return Err(Error::CONFLICT);
            }
            t.commit().await?;
            return Ok(invitation_value(&existing));
        }
        if expires as f64 <= now() || expires as f64 > now() + 600.0 {
            return Err(Error::BAD);
        }
        if bootstrap.state.workspace_id != c.workspace
            || bootstrap.state.head != s(&state, "membership_head")
            || bootstrap.state.revision != i(&state, "membership_revision") as u64
        {
            return Err(Error::CONFLICT);
        }
        let roster=t.all("SELECT device_id,public_record,revoked FROM encrypted_members WHERE workspace_id=? ORDER BY device_id LIMIT 4097",args![&c.workspace]).await?;
        if roster.len() != bootstrap.state.members.len() + bootstrap.state.revoked_device_ids.len()
        {
            return Err(Error::CONFLICT);
        }
        for record in roster {
            if i(&record, "revoked") != 0 {
                if !bootstrap
                    .state
                    .revoked_device_ids
                    .contains(s(&record, "device_id"))
                {
                    return Err(Error::CONFLICT);
                }
            } else {
                let expected: DevicePublic = serde_json::from_str(s(&record, "public_record"))
                    .map_err(|_| Error::UNAVAILABLE)?;
                if bootstrap.state.members.get(s(&record, "device_id")) != Some(&expected) {
                    return Err(Error::CONFLICT);
                }
            }
        }
        let count = t.one("SELECT count(*) AS count FROM enrollment_invitations WHERE workspace_id=? AND expires>? AND used=0",args![&c.workspace,now()]).await?.ok_or(Error::BAD)?;
        let retained = t
            .one(
                "SELECT count(*) AS count FROM enrollment_invitations WHERE workspace_id=?",
                args![&c.workspace],
            )
            .await?
            .ok_or(Error::BAD)?;
        if i(&count, "count") >= 64 || i(&retained, "count") >= 4352 {
            return Err(Error::BUSY);
        }
        let bootstrap_encoded = canonical(&body["bootstrap"])?;
        let metadata_bytes = bootstrap_encoded.len() + 512;
        self.enrollment_bytes(&mut t, &c.workspace, metadata_bytes as i64)
            .await?;
        t.exec("INSERT INTO enrollment_invitations(invitation_id,workspace_id,issuer_device_id,purpose,expires,membership_head,membership_revision,bootstrap,metadata_bytes) VALUES(?,?,?,?,?,?,?,?,?)",args![id,&c.workspace,&c.id,purpose,expires as f64,&state["membership_head"],&state["membership_revision"],&bootstrap_encoded,metadata_bytes]).await?;
        t.commit().await?;
        Ok(
            json!({"invitation_id":id,"workspace_id":c.workspace,"issuer_device_id":c.id,"purpose":purpose,"expires_at":expires,"membership_head":state["membership_head"],"membership_revision":state["membership_revision"]}),
        )
    }
    pub async fn join_workspace(&self, body: &Value) -> Result<Value> {
        protocol::fields(body, &["candidate", "service_token"], &[])?;
        let candidate: EnrollmentCandidate = parse(&body["candidate"])?;
        crypto(candidate.verify_signature())?;
        protocol::uuid(&json!(candidate.device.device_id), false)?;
        let secret = service_secret(&body["service_token"])?;
        let hash = crypto(candidate.digest())?;
        let mut t = self.db.begin().await?;
        workspace(&mut t, &candidate.workspace_id).await?;
        let invitation = t
            .one(
                "SELECT * FROM enrollment_invitations WHERE invitation_id=? AND workspace_id=?",
                args![&candidate.invitation_id, &candidate.workspace_id],
            )
            .await?
            .ok_or(Error::UNAUTHORIZED)?;
        if let Some(existing) = t
            .one(
                "SELECT * FROM enrollment_candidates WHERE device_id=?",
                args![&candidate.device.device_id],
            )
            .await?
        {
            if existing["candidate_hash"] != hash || existing["token_hash"] != digest(secret) {
                return Err(Error::CONFLICT);
            }
            t.commit().await?;
            return Ok(
                json!({"enrollment_id":candidate.device.device_id,"state":existing["state"]}),
            );
        }
        if invitation["purpose"] != role_name(candidate.purpose)
            || i(&invitation, "used") != 0
            || invitation["expires"].as_f64().is_none_or(|at| at <= now())
        {
            return Err(Error::UNAUTHORIZED);
        }
        member(
            &mut t,
            &candidate.workspace_id,
            s(&invitation, "issuer_device_id"),
        )
        .await?;
        let count = t.one("SELECT count(*) AS count FROM enrollment_candidates WHERE workspace_id=? AND state='pending'",args![&candidate.workspace_id]).await?.ok_or(Error::BAD)?;
        let invited = t
            .one(
                "SELECT count(*) AS count FROM enrollment_candidates WHERE invitation_id=?",
                args![&candidate.invitation_id],
            )
            .await?
            .ok_or(Error::BAD)?;
        if i(&count, "count") >= 64 || i(&invited, "count") >= 8 {
            return Err(Error::BUSY);
        }
        if t.one(
            "SELECT device_id FROM encrypted_members WHERE device_id=?",
            args![&candidate.device.device_id],
        )
        .await?
        .is_some()
        {
            return Err(Error::CONFLICT);
        }
        let candidate_encoded = canonical(&body["candidate"])?;
        let metadata_bytes = candidate_encoded.len() + 512;
        self.enrollment_bytes(&mut t, &candidate.workspace_id, metadata_bytes as i64)
            .await?;
        t.exec("INSERT INTO enrollment_candidates(device_id,workspace_id,invitation_id,candidate,candidate_hash,token_hash,created,metadata_bytes) VALUES(?,?,?,?,?,?,?,?)",args![&candidate.device.device_id,&candidate.workspace_id,&candidate.invitation_id,&candidate_encoded,&hash,&digest(secret),now(),metadata_bytes]).await?;
        t.commit().await?;
        Ok(json!({"enrollment_id":candidate.device.device_id,"state":"pending"}))
    }
    pub async fn enrollment_status(&self, id: &str, secret: &str) -> Result<Value> {
        let row = self.db.one("SELECT c.*,w.membership_head,w.membership_revision,i.bootstrap FROM enrollment_candidates c JOIN encrypted_workspaces w ON w.workspace_id=c.workspace_id JOIN enrollment_invitations i ON i.invitation_id=c.invitation_id WHERE c.device_id=? AND c.token_hash=?",args![id,&digest(secret)]).await?.ok_or(Error::UNAUTHORIZED)?;
        let candidate: EnrollmentCandidate =
            serde_json::from_str(s(&row, "candidate")).map_err(|_| Error::UNAVAILABLE)?;
        let approval = row["approval"]
            .as_str()
            .map(serde_json::from_str::<Value>)
            .transpose()
            .map_err(|_| Error::UNAVAILABLE)?;
        let mut result = json!({"enrollment_id":id,"workspace_id":row["workspace_id"],"state":row["state"],"enrollment":approval,"bootstrap":serde_json::from_str::<Value>(s(&row,"bootstrap")).map_err(|_|Error::UNAVAILABLE)?,"membership_head":row["membership_head"],"membership_revision":row["membership_revision"]});
        result[if candidate.purpose == DeviceRole::Computer {
            "node_id"
        } else {
            "device_id"
        }] = json!(id);
        Ok(result)
    }
    pub async fn pending_enrollments(
        &self,
        c: &Credential,
        after: &str,
        limit: usize,
    ) -> Result<Value> {
        self.authorized(c).await?;
        let rows = self.db.one("SELECT role FROM encrypted_members WHERE workspace_id=? AND device_id=? AND revoked=0",args![&c.workspace,&c.id]).await?.ok_or(Error::UNAUTHORIZED)?;
        if rows["role"] != "computer" {
            return Err(Error::UNAUTHORIZED);
        }
        let mut t = self.db.begin().await?;
        let rows = t.all("SELECT c.device_id,c.candidate,i.invitation_id,i.workspace_id,i.issuer_device_id,i.purpose,i.expires,i.membership_head,i.membership_revision FROM enrollment_candidates c JOIN enrollment_invitations i ON i.invitation_id=c.invitation_id WHERE c.workspace_id=? AND i.issuer_device_id=? AND c.state='pending' AND i.used=0 AND i.expires>? AND c.device_id>? ORDER BY c.device_id LIMIT ?",args![&c.workspace,&c.id,now(),after,limit+1]).await?;
        t.commit().await?;
        let more = rows.len() > limit;
        let mut values = Vec::new();
        for row in rows.into_iter().take(limit) {
            values.push(json!({"enrollment_id":row["device_id"],"candidate":serde_json::from_str::<Value>(s(&row,"candidate")).map_err(|_|Error::UNAVAILABLE)?,"invitation":invitation_value(&row)}));
        }
        Ok(
            json!({"next_after":if more {values.last().map(|v|v["enrollment_id"].clone())}else{None},"enrollments":values}),
        )
    }
    pub async fn approve_enrollment(
        &self,
        c: &Credential,
        id: &str,
        body: &Value,
    ) -> Result<Value> {
        protocol::fields(body, &["enrollment"], &[])?;
        let approval: SignedEnrollment = parse(&body["enrollment"])?;
        let mut t = self.db.begin().await?;
        let state = workspace(&mut t, &c.workspace).await?;
        self.authorized_tx(&mut t, c).await?;
        let issuer = member(&mut t, &c.workspace, &c.id).await?;
        if issuer.role != DeviceRole::Computer {
            return Err(Error::UNAUTHORIZED);
        }
        let row=t.one("SELECT c.*,i.issuer_device_id,i.expires,i.used FROM enrollment_candidates c JOIN enrollment_invitations i ON i.invitation_id=c.invitation_id WHERE c.device_id=? AND c.workspace_id=?",args![id,&c.workspace]).await?.ok_or(Error::NOT_FOUND)?;
        let candidate: EnrollmentCandidate =
            serde_json::from_str(s(&row, "candidate")).map_err(|_| Error::UNAVAILABLE)?;
        crypto(approval.verify(&candidate, &issuer))?;
        let encoded = canonical(&body["enrollment"])?;
        if row["state"] == "active" {
            if row["approval"] != encoded {
                return Err(Error::CONFLICT);
            }
            t.commit().await?;
            return Ok(head_result(&c.workspace, id, &state));
        }
        if row["issuer_device_id"] != c.id
            || row["state"] != "pending"
            || i(&row, "used") != 0
            || row["expires"].as_f64().is_none_or(|at| at <= now())
        {
            return Err(Error::UNAUTHORIZED);
        }
        crypto(approval.membership.verify_successor(
            &c.workspace,
            i(&state, "membership_revision") as u64,
            s(&state, "membership_head"),
            &issuer,
        ))?;
        let device = &candidate.device;
        if t.one(
            "SELECT device_id FROM encrypted_members WHERE device_id=?",
            args![id],
        )
        .await?
        .is_some()
        {
            return Err(Error::CONFLICT);
        }
        let count=t.one("SELECT count(*) AS count FROM encrypted_members WHERE workspace_id=? AND revoked=0",args![&c.workspace]).await?.ok_or(Error::BAD)?;
        let identities = t
            .one(
                "SELECT count(*) AS count FROM encrypted_members WHERE workspace_id=?",
                args![&c.workspace],
            )
            .await?
            .ok_or(Error::BAD)?;
        if i(&count, "count") >= 200 || i(&identities, "count") >= 4096 {
            return Err(Error::BUSY);
        }
        if device.role == DeviceRole::Computer {
            t.exec(
                "INSERT INTO nodes(id,workspace_id,name,token_hash) VALUES(?,?,'Computer',?)",
                args![id, &c.workspace, &row["token_hash"]],
            )
            .await?;
            registry::enroll(&mut t, &c.workspace, id, "Computer").await?;
        } else {
            t.exec("INSERT INTO devices(id,workspace_id,name,token_hash,paired_at,added_by) VALUES(?,?,'Phone',?,?,?)",args![id,&c.workspace,&row["token_hash"],now(),&c.id]).await?;
        }
        t.exec("INSERT INTO encrypted_members(workspace_id,device_id,role,public_record) VALUES(?,?,?,?)",args![&c.workspace,id,role_name(device.role),&canonical(&json!(device))?]).await?;
        self.enrollment_bytes(
            &mut t,
            &c.workspace,
            (encoded.len()
                + canonical(&json!(device))?.len()
                + canonical(&json!(approval.membership))?.len()
                + 1024) as i64,
        )
        .await?;
        let head = crypto(approval.membership.head())?;
        save_change(&mut t, &approval.membership, &head).await?;
        t.exec(
            "UPDATE enrollment_candidates SET state='active',approval=? WHERE device_id=?",
            args![&encoded, id],
        )
        .await?;
        t.exec(
            "UPDATE enrollment_invitations SET used=1 WHERE invitation_id=?",
            args![&row["invitation_id"]],
        )
        .await?;
        t.exec("UPDATE enrollment_candidates SET state='rejected' WHERE invitation_id=? AND state='pending'",args![&row["invitation_id"]]).await?;
        t.commit().await?;
        Ok(
            json!({"workspace_id":c.workspace,"node_id":id,"state":"active","membership_head":head,"membership_revision":approval.membership.statement.revision}),
        )
    }
    pub async fn membership_changes(
        &self,
        c: &Credential,
        after: i64,
        limit: usize,
    ) -> Result<Value> {
        let mut t = self.db.begin().await?;
        let mut state = workspace(&mut t, &c.workspace).await?;
        let retired=t.one("SELECT revoked,revoked_revision FROM encrypted_members WHERE workspace_id=? AND device_id=?",args![&c.workspace,&c.id]).await?.ok_or(Error::UNAUTHORIZED)?;
        let ceiling = if i(&retired, "revoked") != 0 {
            if c.role != Role::Node {
                return Err(Error::UNAUTHORIZED);
            }
            let revision = retired["revoked_revision"]
                .as_i64()
                .ok_or(Error::UNAUTHORIZED)?;
            let head = t
                .one(
                    "SELECT head FROM membership_changes WHERE workspace_id=? AND revision=?",
                    args![&c.workspace, revision],
                )
                .await?
                .ok_or(Error::UNAUTHORIZED)?;
            state["membership_head"] = head["head"].clone();
            state["membership_revision"] = json!(revision);
            revision
        } else {
            self.authorized_tx(&mut t, c).await?;
            member(&mut t, &c.workspace, &c.id).await?;
            i(&state, "membership_revision")
        };
        if after > ceiling {
            return Err(Error::CONFLICT);
        }
        let rows=t.all("SELECT revision,record FROM membership_changes WHERE workspace_id=? AND revision>? AND revision<=? ORDER BY revision LIMIT ?",args![&c.workspace,after,ceiling,limit+1]).await?;
        t.commit().await?;
        let more = rows.len() > limit;
        let mut changes = Vec::new();
        let mut cursor = after;
        for row in rows.into_iter().take(limit) {
            cursor = i(&row, "revision");
            changes.push(
                serde_json::from_str::<Value>(s(&row, "record")).map_err(|_| Error::UNAVAILABLE)?,
            );
        }
        Ok(
            json!({"changes":changes,"membership_head":state["membership_head"],"membership_revision":state["membership_revision"],"next_after":if more{Some(cursor)}else{None}}),
        )
    }
    async fn enrollment_bytes(&self, t: &mut Tx, w: &str, bytes: i64) -> Result<()> {
        self.enrollment_accounting(t, w, bytes, false).await
    }
    async fn enrollment_control_bytes(&self, t: &mut Tx, w: &str, bytes: i64) -> Result<()> {
        self.enrollment_accounting(t, w, bytes, true).await
    }
    async fn enrollment_accounting(
        &self,
        t: &mut Tx,
        w: &str,
        bytes: i64,
        control: bool,
    ) -> Result<()> {
        let usage = self.db.payload_usage(t, w).await?;
        if bytes > 0 {
            if i(&usage, "payload_bytes")
                .checked_add(bytes)
                .is_none_or(|n| {
                    n > self.db.cfg.max_queue_bytes + if control { 1024 * 1024 } else { 0 }
                })
            {
                return Err(Error::BUSY);
            }
            let capacity = if control {
                self.db.control_payload_capacity(t, w).await?
            } else {
                self.db.payload_capacity(t, w).await?
            };
            if t.exec("UPDATE payload_shards SET payload_bytes=payload_bytes+? WHERE id=? AND payload_bytes+?<=?",args![bytes,self.db.shard(w),bytes,capacity]).await?==0{return Err(Error::BUSY);}
            t.exec(
                "UPDATE workspace_usage SET payload_bytes=payload_bytes+? WHERE workspace_id=?",
                args![bytes, w],
            )
            .await?;
        } else {
            self.db.bytes(t, w, bytes).await?;
        }
        Ok(())
    }
    pub async fn maintain_enrollment(&self) -> Result<()> {
        let mut t = self.db.begin().await?;
        let rows=t.all("SELECT invitation_id,workspace_id FROM enrollment_invitations WHERE expires<? AND used=0 ORDER BY expires,invitation_id LIMIT ?",args![now()-86400.0,self.db.cfg.maintenance_batch]).await?;
        t.commit().await?;
        for row in rows {
            let w = s(&row, "workspace_id");
            let mut t = self.db.begin().await?;
            workspace(&mut t, w).await?;
            let removed=t.all("DELETE FROM enrollment_candidates WHERE invitation_id=? AND state IN ('pending','rejected') RETURNING metadata_bytes",args![&row["invitation_id"]]).await?;
            let mut released = removed.iter().map(|r| i(r, "metadata_bytes")).sum::<i64>();
            if let Some(invite)=t.one("DELETE FROM enrollment_invitations WHERE invitation_id=? AND NOT EXISTS (SELECT 1 FROM enrollment_candidates WHERE invitation_id=?) RETURNING metadata_bytes",args![&row["invitation_id"],&row["invitation_id"]]).await?{released+=i(&invite,"metadata_bytes");}
            if released > 0 {
                self.enrollment_bytes(&mut t, w, -released).await?;
            }
            t.commit().await?;
        }
        Ok(())
    }
    pub async fn revoke_membership(
        &self,
        c: &Credential,
        body: &Value,
        self_only: bool,
    ) -> Result<Value> {
        protocol::fields(body, &["membership"], &[])?;
        let change: SignedMembership = parse(&body["membership"])?;
        let target = change
            .statement
            .revoked_device_id
            .as_deref()
            .ok_or(Error::BAD)?;
        if self_only && target != c.id {
            return Err(Error::BAD);
        }
        let mut t = self.db.begin().await?;
        t.one("SELECT id FROM global_usage WHERE id=1 FOR UPDATE", &[])
            .await?;
        let state = workspace(&mut t, &c.workspace).await?;
        let head = crypto(change.head())?;
        if let Some(old) = t
            .one(
                "SELECT head FROM membership_changes WHERE workspace_id=? AND revision=?",
                args![&c.workspace, change.statement.revision as i64],
            )
            .await?
        {
            if old["head"] != head {
                return Err(Error::CONFLICT);
            }
            t.commit().await?;
            return Ok(
                json!({"revoked":true,"membership_head":head,"membership_revision":change.statement.revision}),
            );
        }
        self.authorized_tx(&mut t, c).await?;
        let issuer = member(&mut t, &c.workspace, &c.id).await?;
        crypto(change.verify_successor(
            &c.workspace,
            i(&state, "membership_revision") as u64,
            s(&state, "membership_head"),
            &issuer,
        ))?;
        let removed = member(&mut t, &c.workspace, target).await?;
        self.db.payload_usage(&mut t, &c.workspace).await?;
        t.exec(
            "UPDATE encrypted_members SET revoked=1,revoked_revision=? WHERE workspace_id=? AND device_id=?",
            args![change.statement.revision as i64,&c.workspace, target],
        )
        .await?;
        t.exec(
            "UPDATE enrollment_candidates SET state='revoked' WHERE device_id=?",
            args![target],
        )
        .await?;
        let table = if removed.role == DeviceRole::Computer {
            "nodes"
        } else {
            "devices"
        };
        t.exec(
            &format!("UPDATE {table} SET revoked=1 WHERE id=? AND workspace_id=?"),
            args![target, &c.workspace],
        )
        .await?;
        self.retire_encrypted_tx(&mut t, &c.workspace, target)
            .await?;
        t.exec(
            "UPDATE enrollment_invitations SET used=1 WHERE workspace_id=? AND issuer_device_id=?",
            args![&c.workspace, target],
        )
        .await?;
        self.enrollment_control_bytes(
            &mut t,
            &c.workspace,
            (canonical(&body["membership"])?.len() + 512) as i64,
        )
        .await?;
        save_change(&mut t, &change, &head).await?;
        if removed.role == DeviceRole::Phone {
            self.delete_push_tx(&mut t, target, &c.workspace).await?;
        }
        self.append_event(&mut t, &c.workspace, &c.id, "", "membership", None)
            .await?;
        t.notify(&format!("workspace:{}", c.workspace)).await?;
        t.notify(&format!("node:{target}")).await?;
        t.commit().await?;
        self.db.signal(format!("workspace:{}", c.workspace));
        self.db.signal(format!("node:{target}"));
        Ok(
            json!({"revoked":true,"membership_head":head,"membership_revision":change.statement.revision}),
        )
    }
}
async fn save_change(t: &mut Tx, change: &SignedMembership, head: &str) -> Result<()> {
    let statement = &change.statement;
    let revision = i64::try_from(statement.revision).map_err(|_| Error::BAD)?;
    t.exec("INSERT INTO membership_changes(workspace_id,revision,change_id,head,record) VALUES(?,?,?,?,?)",args![&statement.workspace_id,revision,head,head,&canonical(&json!(change))?]).await?;
    t.exec("UPDATE encrypted_workspaces SET membership_head=?,membership_revision=? WHERE workspace_id=?",args![head,revision,&statement.workspace_id]).await?;
    Ok(())
}
fn invitation_value(row: &Value) -> Value {
    json!({"invitation_id":row["invitation_id"],"workspace_id":row["workspace_id"],"issuer_device_id":row["issuer_device_id"],"purpose":row["purpose"],"expires_at":row["expires"],"membership_head":row["membership_head"],"membership_revision":row["membership_revision"]})
}
