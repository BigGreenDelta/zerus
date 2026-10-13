use serde_json::{json, Value};
use std::{sync::Arc, time::Duration};
use tempfile::TempDir;
use tokio_util::sync::CancellationToken;
use uuid::Uuid;
use zerus_relay::{
    args,
    config::Config,
    db::{i, now, s, Db},
    error::Result,
    http::{serve, App},
    json::{canonical, digest},
    protocol,
    store::{Credential, Role, Store},
};

struct Fixture {
    _dir: TempDir,
    store: Store,
    node: Credential,
    phone: Credential,
    credentials: Value,
    paired: Value,
}

fn voice_command(computer: &str, op: &str, message: Option<&str>) -> Value {
    let id = Uuid::new_v4().to_string();
    let mut payload =
        json!({"request_id":id,"expected_run_id":"run","expected_conversation_id":"conversation"});
    if op == "voice_submit" {
        payload["speech"] =
            json!({"machine_id":Uuid::new_v4().to_string(),"profile_id":"gigaam-ru"});
        payload["audio"] =
            json!({"format":"pcm_s16le","sample_rate":16000,"channels":1,"data_base64":"AAAAAA=="});
    } else if ["voice_status", "voice_cancel"].contains(&op) {
        payload["message_id"] = json!(message.unwrap());
    }
    json!({"request_id":id,"computer_id":computer,"operation":op,"session":"codex/example/tag","payload":payload})
}

#[test]
fn voice_wire_requires_bounded_audio_and_stable_machine_route() {
    let computer = Uuid::new_v4().to_string();
    for op in [
        "voice_submit",
        "voice_profiles",
        "voice_status",
        "voice_cancel",
    ] {
        let value = voice_command(&computer, op, Some(&Uuid::new_v4().to_string()));
        protocol::request(&value).unwrap();
        let mut bad = value.clone();
        bad["session"] = json!("");
        assert!(protocol::request(&bad).is_err());
        let mut bad = value;
        bad["payload"]["request_id"] = json!(Uuid::new_v4().to_string());
        assert!(protocol::request(&bad).is_err());
    }
    for route in [
        json!({"machine_id":"arch","profile_id":"gigaam-ru"}),
        json!({"machine_id":computer,"profile_id":"../model"}),
        json!({"machine_id":computer,"profile_id":"ok","command":"example"}),
    ] {
        let mut value = voice_command(&computer, "voice_submit", None);
        value["payload"]["speech"] = route;
        assert!(protocol::request(&value).is_err());
    }
    for data in ["AA==", "AA==AA==", "AA B", "AAAA"] {
        let mut value = voice_command(&computer, "voice_submit", None);
        value["payload"]["audio"]["data_base64"] = json!(data);
        assert!(protocol::request(&value).is_err());
    }
}

#[tokio::test]
async fn voice_acceptance_erases_relay_audio_and_preserves_shared_conversation_access() {
    let f = Fixture::new().await;
    f.beat(&json!({"sessions":[{"name":"codex/example/tag"}],"mobile_capabilities":{"protocol_version":1,"operations":protocol::OPERATIONS}})).await;
    let upload = voice_command(&f.node.id, "voice_submit", None);
    f.submit(&upload).await.unwrap();
    assert!(f.store.claim(&f.node, false).await.unwrap().is_some());
    let result = json!({"state":"completed","result":{"schema":1,"message":{"id":upload["request_id"],"state":"queued","revision":1}},"error":null});
    f.store
        .result(
            &f.node,
            s(&upload, "request_id"),
            &result,
            Some(&canonical(&result["result"]).unwrap()),
        )
        .await
        .unwrap();
    let row = f
        .store
        .db
        .one(
            "SELECT body,body_bytes FROM requests WHERE id=?",
            args![&upload["request_id"]],
        )
        .await
        .unwrap()
        .unwrap();
    assert_eq!(row["body"], "");
    assert_eq!(row["body_bytes"], 0);
    assert_eq!(f.submit(&upload).await.unwrap()["result"], result["result"]);
    assert!(f.store.claim(&f.node, false).await.unwrap().is_none());
    let invitation = f.store.invite(&f.phone.workspace).await.unwrap();
    let paired = f
        .store
        .pair(s(&invitation, "pair_code"), "Second phone")
        .await
        .unwrap();
    let other = f
        .store
        .authenticate(s(&paired, "device_token"), Role::Device)
        .await
        .unwrap();
    let status = voice_command(&f.node.id, "voice_status", Some(s(&upload, "request_id")));
    f.store
        .submit(
            &other,
            &zerus_relay::store::Submission::new(status).unwrap(),
        )
        .await
        .unwrap();
    f.check_accounting().await;
    f.close().await;
}

#[test]
fn voice_profile_revision_is_optional_bounded_and_exact() {
    let mut request = voice_command(&Uuid::new_v4().to_string(), "voice_submit", None);
    protocol::request(&request).unwrap();
    request["payload"]["speech"]["profile_revision"] = json!(7);
    protocol::request(&request).unwrap();
    for revision in [
        json!(-1),
        json!(true),
        json!("7"),
        json!(9_007_199_254_740_992u64),
    ] {
        request["payload"]["speech"]["profile_revision"] = revision;
        assert!(protocol::request(&request).is_err());
    }
}

fn speech_config_command(computer: &str, op: &str, previous: Option<&str>) -> Value {
    let id = Uuid::new_v4().to_string();
    let owner = "00000000-0000-4000-8000-000000000001";
    let payload = match op {
        "speech_profile_upsert" => {
            json!({"request_id":id,"expected_machine_id":owner,"expected_revision":0,
            "profile":{"id":"fixture","name":"Fixture provider","adapter":"openai_http","model":"fixture",
                "language":"ru","base_url":"https://speech.example.invalid/v1","allow_insecure_http":false},
            "credential":{"mode":"keep"}})
        }
        "speech_config_status" => {
            json!({"operation_id":previous.unwrap(),"expected_machine_id":owner})
        }
        _ => json!({}),
    };
    json!({"request_id":id,"computer_id":computer,"operation":op,"session":"","payload":payload})
}

async fn speech_config_storage_contract(f: &Fixture, writer: &Store) {
    f.beat(&json!({"sessions":[],"mobile_capabilities":{"protocol_version":1,"operations":protocol::SPEECH}})).await;
    let change = speech_config_command(&f.node.id, "speech_profile_upsert", None);
    f.submit(&change).await.unwrap();
    assert!(writer.claim(&f.node, false).await.unwrap().is_some());
    assert!(f.store.claim(&f.node, false).await.unwrap().is_none());
    let result = json!({"schema":1,"machine_id":change["payload"]["expected_machine_id"],
        "request_id":change["request_id"],"state":"completed","revision":1});
    let reply = json!({"state":"completed","result":result,"error":null});
    writer
        .result(
            &f.node,
            s(&change, "request_id"),
            &reply,
            Some(&canonical(&result).unwrap()),
        )
        .await
        .unwrap();
    let row = f
        .store
        .db
        .one(
            "SELECT body,body_bytes FROM requests WHERE id=?",
            args![&change["request_id"]],
        )
        .await
        .unwrap()
        .unwrap();
    assert_eq!(row["body"], "");
    assert_eq!(row["body_bytes"], 0);
    assert_eq!(f.submit(&change).await.unwrap()["result"], result);
    assert!(writer.claim(&f.node, false).await.unwrap().is_none());
    let mut conflict = change.clone();
    conflict["payload"]["profile"]["name"] = json!("Changed");
    assert!(f.submit(&conflict).await.is_err());
    let status = speech_config_command(
        &f.node.id,
        "speech_config_status",
        Some(s(&change, "request_id")),
    );
    f.submit(&status).await.unwrap();
    assert!(writer.claim(&f.node, false).await.unwrap().is_some());
    writer
        .result(
            &f.node,
            s(&status, "request_id"),
            &reply,
            Some(&canonical(&result).unwrap()),
        )
        .await
        .unwrap();
    writer
        .db
        .exec(
            "UPDATE requests SET updated=? WHERE id=?",
            args![now() - 121.0, &status["request_id"]],
        )
        .await
        .unwrap();
    writer.maintain(false).await.unwrap();
    assert!(f
        .store
        .db
        .one(
            "SELECT id FROM requests WHERE id=?",
            args![&status["request_id"]]
        )
        .await
        .unwrap()
        .is_none());
    assert_eq!(f.submit(&change).await.unwrap()["result"], result);
    f.check_accounting().await;
}

#[tokio::test]
async fn speech_config_receipts_erase_bodies_and_expire_reads() {
    let f = Fixture::new().await;
    speech_config_storage_contract(&f, &f.store).await;
    f.close().await;
}

fn speech_command(computer: &str, op: &str, job: Option<&str>) -> Value {
    let id = Uuid::new_v4().to_string();
    let payload = match op {
        "speech_submit" => {
            json!({"request_id":id,"profile_id":"gigaam-ru","audio":{"format":"pcm_s16le","sample_rate":16000,"channels":1,"data_base64":"AAAAAA=="}})
        }
        "speech_status" | "speech_cancel" => json!({"job_id":job.unwrap()}),
        _ => json!({}),
    };
    json!({"request_id":id,"computer_id":computer,"operation":op,"session":"","payload":payload})
}

#[test]
fn speech_wire_rejects_unbounded_or_ambiguous_audio() {
    use base64::Engine;
    let computer = Uuid::new_v4().to_string();
    let b = speech_command(&computer, "speech_submit", None);
    protocol::request(&b).unwrap();
    for data in ["", "AA==", "AAAA", "AA==AA==", "AA B", "AAB="] {
        let mut bad = b.clone();
        bad["payload"]["audio"]["data_base64"] = json!(data);
        assert!(protocol::request(&bad).is_err(), "accepted {data}");
    }
    let mut full = b.clone();
    full["payload"]["audio"]["data_base64"] = json!(
        base64::engine::general_purpose::STANDARD.encode(vec![0; protocol::SPEECH_AUDIO_MAX])
    );
    protocol::request(&full).unwrap();
    full["payload"]["audio"]["data_base64"] =
        json!(base64::engine::general_purpose::STANDARD
            .encode(vec![0; protocol::SPEECH_AUDIO_MAX + 2]));
    assert!(protocol::request(&full).is_err());
    for (field, value) in [("sample_rate", json!(true)), ("channels", json!(2))] {
        let mut bad = b.clone();
        bad["payload"]["audio"][field] = value;
        assert!(protocol::request(&bad).is_err());
    }
    let mut bad = b.clone();
    bad["session"] = json!("codex/example/session");
    assert!(protocol::request(&bad).is_err());
    let mut bad = b;
    bad["payload"]["path"] = json!("/etc/passwd");
    assert!(protocol::request(&bad).is_err());
}

#[tokio::test]
async fn speech_jobs_preserve_owner_identity_and_discard_audio_after_ack() {
    let f = Fixture::new().await;
    f.beat(&json!({"sessions":[],"mobile_capabilities":{"protocol_version":1,"operations":protocol::SPEECH}})).await;
    let b = speech_command(&f.node.id, "speech_submit", None);
    f.submit(&b).await.unwrap();
    assert!(f.store.claim(&f.node, false).await.unwrap().is_some());
    let result = json!({"state":"completed","result":{"schema":1,"job_id":b["request_id"],"state":"queued"},"error":null});
    let encoded = canonical(&result["result"]).unwrap();
    f.store
        .result(&f.node, s(&b, "request_id"), &result, Some(&encoded))
        .await
        .unwrap();
    let row = f
        .store
        .db
        .one(
            "SELECT body,body_bytes FROM requests WHERE id=?",
            args![&b["request_id"]],
        )
        .await
        .unwrap()
        .unwrap();
    assert_eq!(row["body"], "");
    assert_eq!(row["body_bytes"], 0);
    assert_eq!(f.submit(&b).await.unwrap()["result"], result["result"]);
    let status = speech_command(&f.node.id, "speech_status", Some(s(&b, "request_id")));
    f.submit(&status).await.unwrap();
    let invitation = f.store.invite(&f.phone.workspace).await.unwrap();
    let paired = f
        .store
        .pair(s(&invitation, "pair_code"), "Second phone")
        .await
        .unwrap();
    let other = f
        .store
        .authenticate(s(&paired, "device_token"), Role::Device)
        .await
        .unwrap();
    let foreign = speech_command(&f.node.id, "speech_status", Some(s(&b, "request_id")));
    assert_eq!(
        f.store
            .submit(
                &other,
                &zerus_relay::store::Submission::new(foreign).unwrap()
            )
            .await
            .unwrap_err()
            .0
            .as_u16(),
        404
    );
    f.check_accounting().await;
    f.close().await;
}

#[tokio::test]
async fn speech_expired_upload_keeps_tombstone_without_audio() {
    let f = Fixture::new().await;
    f.beat(&json!({"sessions":[],"mobile_capabilities":{"protocol_version":1,"operations":protocol::SPEECH}})).await;
    let b = speech_command(&f.node.id, "speech_submit", None);
    f.submit(&b).await.unwrap();
    f.store
        .db
        .exec(
            "UPDATE requests SET created=? WHERE id=?",
            args![now() - 180.0, &b["request_id"]],
        )
        .await
        .unwrap();
    f.store.maintain(false).await.unwrap();
    let row = f
        .store
        .db
        .one(
            "SELECT body,body_bytes,state FROM requests WHERE id=?",
            args![&b["request_id"]],
        )
        .await
        .unwrap()
        .unwrap();
    assert_eq!(row["state"], "failed");
    assert_eq!(row["body"], "");
    assert_eq!(f.submit(&b).await.unwrap()["state"], "failed");
    f.check_accounting().await;
    f.close().await;
}

#[tokio::test]
async fn speech_status_retention_keeps_owner_receipt_and_exact_accounting() {
    let f = Fixture::new().await;
    f.beat(&json!({"sessions":[],"mobile_capabilities":{"protocol_version":1,"operations":protocol::SPEECH}})).await;
    let upload = speech_command(&f.node.id, "speech_submit", None);
    f.submit(&upload).await.unwrap();
    assert!(f.store.claim(&f.node, false).await.unwrap().is_some());
    let submitted = json!({"state":"completed","result":{"schema":1,"job_id":upload["request_id"],"state":"queued"},"error":null});
    f.store
        .result(
            &f.node,
            s(&upload, "request_id"),
            &submitted,
            Some(&canonical(&submitted["result"]).unwrap()),
        )
        .await
        .unwrap();
    let status = speech_command(&f.node.id, "speech_status", Some(s(&upload, "request_id")));
    f.submit(&status).await.unwrap();
    assert!(f.store.claim(&f.node, false).await.unwrap().is_some());
    let completed = json!({"state":"completed","result":{"schema":1,"job_id":upload["request_id"],"state":"completed","text":"Synthetic private dictation"},"error":null});
    f.store
        .result(
            &f.node,
            s(&status, "request_id"),
            &completed,
            Some(&canonical(&completed["result"]).unwrap()),
        )
        .await
        .unwrap();
    f.store
        .db
        .exec(
            "UPDATE requests SET updated=? WHERE id=?",
            args![now() - 121.0, &status["request_id"]],
        )
        .await
        .unwrap();
    f.store.maintain(false).await.unwrap();
    assert!(f
        .store
        .db
        .one(
            "SELECT id FROM requests WHERE id=?",
            args![&status["request_id"]]
        )
        .await
        .unwrap()
        .is_none());
    assert_eq!(
        f.submit(&upload).await.unwrap()["result"],
        submitted["result"]
    );
    let retry = speech_command(&f.node.id, "speech_status", Some(s(&upload, "request_id")));
    f.submit(&retry).await.unwrap();
    f.check_accounting().await;
    f.close().await;
}
impl Fixture {
    async fn new() -> Self {
        Self::config(Config {
            background: false,
            ..Config::default()
        })
        .await
    }
    async fn config(cfg: Config) -> Self {
        let dir = tempfile::tempdir().unwrap();
        use std::os::unix::fs::PermissionsExt;
        std::fs::set_permissions(dir.path(), std::fs::Permissions::from_mode(0o700)).unwrap();
        let db = Db::open(&dir.path().join("relay.sqlite3"), Arc::new(cfg))
            .await
            .unwrap();
        let store = Store::new(db);
        let c = store
            .provision("Example workspace", "Example computer")
            .await
            .unwrap();
        let p = store.pair(s(&c, "pair_code"), "Phone").await.unwrap();
        let node = store
            .authenticate(s(&c, "node_token"), Role::Node)
            .await
            .unwrap();
        let phone = store
            .authenticate(s(&p, "device_token"), Role::Device)
            .await
            .unwrap();
        Self {
            _dir: dir,
            store,
            node,
            phone,
            credentials: c,
            paired: p,
        }
    }
    fn command(&self, op: &str) -> Value {
        let id = Uuid::new_v4().to_string();
        json!({"request_id":id,"computer_id":self.node.id,"operation":op,"session":"codex/example/tag","payload":if op=="inspect"{json!({})}else{json!({"request_id":id,"expected_run_id":"run","expected_conversation_id":"conversation","text":"hello"})}})
    }
    async fn submit(&self, b: &Value) -> Result<Value> {
        self.store
            .submit(
                &self.phone,
                &zerus_relay::store::Submission::new(b.clone())?,
            )
            .await
            .map(|(_, b)| b)
    }
    async fn check_accounting(&self) {
        let mut t = self.store.db.begin().await.unwrap();
        let row=t.one("SELECT CAST(COALESCE(sum(body_bytes+result_bytes+reserved_bytes),0) AS BIGINT) AS bytes FROM requests WHERE workspace_id=?",args![&self.phone.workspace]).await.unwrap().unwrap();
        let usage = t
            .one(
                "SELECT * FROM workspace_usage WHERE workspace_id=?",
                args![&self.phone.workspace],
            )
            .await
            .unwrap()
            .unwrap();
        assert_eq!(row["bytes"], usage["payload_bytes"]);
        let active=t.one("SELECT count(*) AS n FROM requests WHERE workspace_id=? AND state IN ('queued','claimed')",args![&self.phone.workspace]).await.unwrap().unwrap();
        assert_eq!(active["n"], usage["active"]);
        t.commit().await.unwrap();
    }
    /// A heartbeat from a connector that never sends presence.
    async fn beat(&self, snapshot: &Value) {
        self.store
            .heartbeat(
                &self.node,
                &json!({ "snapshot": snapshot }),
                &canonical(snapshot).unwrap(),
            )
            .await
            .unwrap();
    }
    async fn beat_with(&self, snapshot: &Value, presence: &Value) {
        self.store
            .heartbeat(
                &self.node,
                &json!({"snapshot":snapshot,"presence":presence}),
                &canonical(snapshot).unwrap(),
            )
            .await
            .unwrap();
    }
    async fn node_row(&self, columns: &str) -> Value {
        self.store
            .db
            .one(
                &format!("SELECT {columns} FROM nodes WHERE id=?"),
                args![&self.node.id],
            )
            .await
            .unwrap()
            .unwrap()
    }
    /// Counts the rows of a FROM clause, for example `alert_candidates WHERE session='s0'`.
    async fn count(&self, from: &str) -> i64 {
        i(
            &self
                .store
                .db
                .one(&format!("SELECT count(*) AS n FROM {from}"), &[])
                .await
                .unwrap()
                .unwrap(),
            "n",
        )
    }
    async fn alert_count(&self) -> i64 {
        self.count("events WHERE kind='alert'").await
    }
    async fn events_of(&self, kind: &str) -> Vec<Value> {
        self.store.events(&self.phone, 0).await.unwrap()["events"]
            .as_array()
            .unwrap()
            .iter()
            .filter(|e| e["kind"] == kind)
            .cloned()
            .collect()
    }
    async fn close(self) {
        self.store.db.close().await;
    }
}
fn presence(idle: f64, locked: Option<f64>) -> Value {
    json!({"idle_seconds":idle,"locked":locked.is_some(),"locked_seconds":locked})
}
fn signature(digit: char) -> String {
    digit.to_string().repeat(64)
}
fn session_row(
    phase: &str,
    activity: &str,
    attention: &str,
    signature: Option<&str>,
    read: Value,
) -> Value {
    json!({"name":"example","run_id":"r","conversation_id":"c","phase":phase,"activity":activity,"attention_id":attention,"attention_signature":signature,"read":read})
}
fn listing(session: Value) -> Value {
    json!({ "sessions": [session] })
}
async fn gateway_beat(f: &Fixture, root: &str, peers: &Value, snapshot: &Value) {
    f.store
        .heartbeat(
            &f.node,
            &json!({"snapshot":snapshot,"machine_id":root,"peers":peers,"presence":null}),
            &canonical(snapshot).unwrap(),
        )
        .await
        .unwrap();
}
async fn peer_beat(f: &Fixture, route: &str, machine: &str, snapshot: &Value, presence: &Value) {
    f.store
        .peer_heartbeat(
            &f.node,
            route,
            &json!({"machine_id":machine,"snapshot":snapshot,"presence":presence}),
            &canonical(snapshot).unwrap(),
        )
        .await
        .unwrap();
}
#[tokio::test]
async fn claims_and_receipts_never_replay() {
    let f = Fixture::new().await;
    let b = f.command("send");
    assert_eq!(f.submit(&b).await.unwrap()["state"], "queued");
    assert_eq!(f.submit(&b).await.unwrap()["state"], "queued");
    let mut changed = b.clone();
    changed["session"] = json!("other");
    assert_eq!(f.submit(&changed).await.unwrap_err().0.as_u16(), 409);
    let (a, c) = tokio::join!(f.store.claim(&f.node, false), f.store.claim(&f.node, false));
    assert_eq!(
        usize::from(a.unwrap().is_some()) + usize::from(c.unwrap().is_some()),
        1
    );
    assert_eq!(f.submit(&b).await.unwrap()["state"], "claimed");
    let result = json!({"state":"completed","result":{"ok":true,"unicode":"😀"},"error":null});
    let e = canonical(&result["result"]).unwrap();
    f.store
        .result(&f.node, s(&b, "request_id"), &result, Some(&e))
        .await
        .unwrap();
    f.store
        .result(&f.node, s(&b, "request_id"), &result, Some(&e))
        .await
        .unwrap();
    assert_eq!(
        f.store
            .receipt(&f.phone, s(&b, "request_id"))
            .await
            .unwrap()["result"],
        result["result"]
    );
    f.check_accounting().await;
    f.store
        .db
        .exec(
            "UPDATE requests SET updated=? WHERE id=?",
            args![now() - 8.0 * 86400.0, &b["request_id"]],
        )
        .await
        .unwrap();
    f.store.maintain(false).await.unwrap();
    assert_eq!(f.submit(&b).await.unwrap()["state"], "completed");
    assert!(f.store.claim(&f.node, false).await.unwrap().is_none());
    f.check_accounting().await;
    f.close().await;
}
#[tokio::test]
async fn mark_read_is_read_class_and_pruned_like_reads() {
    assert!(protocol::READS.contains(&"mark_read"));
    assert!(protocol::READ_SQL.contains("'mark_read'"));
    let f = Fixture::new().await;
    let snapshot = json!({"sessions":[],"mobile_capabilities":{"protocol_version":1,"operations":["mark_read"],"features":[]}});
    f.store
        .heartbeat(
            &f.node,
            &json!({"snapshot":snapshot}),
            &canonical(&snapshot).unwrap(),
        )
        .await
        .unwrap();
    let mut b = f.command("mark_read");
    b["payload"] = json!({"run_id":"run","conversation_id":"conversation","reply_id":"7:100.5","attention_signature":null});
    assert_eq!(f.submit(&b).await.unwrap()["state"], "queued");
    assert!(f.store.claim(&f.node, false).await.unwrap().is_some());
    let result = json!({"state":"completed","result":{"ok":true},"error":null});
    let e = canonical(&result["result"]).unwrap();
    f.store
        .result(&f.node, s(&b, "request_id"), &result, Some(&e))
        .await
        .unwrap();
    f.check_accounting().await;
    f.store
        .db
        .exec(
            "UPDATE requests SET updated=? WHERE id=?",
            args![now() - 3601.0, &b["request_id"]],
        )
        .await
        .unwrap();
    f.store.maintain(false).await.unwrap();
    // A pruned read identity is new again; a mutation would return its completed receipt.
    assert_eq!(f.submit(&b).await.unwrap()["state"], "queued");
    f.check_accounting().await;
    f.close().await;
}
#[tokio::test]
async fn expired_claims_and_revocation_are_terminal() {
    let f = Fixture::new().await;
    let b = f.command("send");
    f.submit(&b).await.unwrap();
    f.store.claim(&f.node, false).await.unwrap();
    f.store
        .db
        .exec(
            "UPDATE requests SET claimed=? WHERE id=?",
            args![now() - 91.0, &b["request_id"]],
        )
        .await
        .unwrap();
    let result = json!({"state":"completed","result":null,"error":null});
    assert_eq!(
        f.store
            .result(&f.node, s(&b, "request_id"), &result, None)
            .await
            .unwrap_err()
            .0
            .as_u16(),
        409
    );
    assert_eq!(
        f.store
            .receipt(&f.phone, s(&b, "request_id"))
            .await
            .unwrap()["state"],
        "uncertain"
    );
    let next = f.command("send");
    f.submit(&next).await.unwrap();
    f.store.revoke(Role::Device, &f.phone.id).await.unwrap();
    assert!(f.store.claim(&f.node, false).await.unwrap().is_none());
    assert!(f
        .store
        .authenticate(s(&f.paired, "device_token"), Role::Device)
        .await
        .is_err());
    f.check_accounting().await;
    f.close().await;
}
#[tokio::test]
async fn sqlite_restart_keeps_claim_uncertain() {
    let f = Fixture::new().await;
    let b = f.command("send");
    f.submit(&b).await.unwrap();
    f.store.claim(&f.node, false).await.unwrap();
    f.store.maintain(true).await.unwrap();
    assert_eq!(f.submit(&b).await.unwrap()["state"], "uncertain");
    f.check_accounting().await;
    f.close().await;
}
#[tokio::test]
async fn startup_defers_alert_pass_failure_but_not_record_maintenance() {
    let f = Fixture::new().await;
    let b = f.command("send");
    f.submit(&b).await.unwrap();
    f.store.claim(&f.node, false).await.unwrap();
    // A failing alert pass (here its table is gone) must not stop a worker from starting.
    f.store
        .db
        .exec("DROP TABLE alert_candidates", &[])
        .await
        .unwrap();
    assert!(f.store.maintain(false).await.is_err());
    f.store.maintain_at_startup(true).await.unwrap();
    // The record stages before it still ran: the SQLite restart retired the claim.
    assert_eq!(f.submit(&b).await.unwrap()["state"], "uncertain");
    f.check_accounting().await;
    // Record maintenance failures stay fatal at startup.
    f.store
        .db
        .exec("DROP TABLE rate_limits", &[])
        .await
        .unwrap();
    assert!(f.store.maintain_at_startup(false).await.is_err());
    f.close().await;
}
#[tokio::test]
async fn legacy_oversized_receipt_does_not_change_delivery_identity() {
    let f = Fixture::new().await;
    let b = f.command("send");
    f.submit(&b).await.unwrap();
    f.store.claim(&f.node, false).await.unwrap();
    let large = format!("\"{}\"", "x".repeat(2 * protocol::MIB));
    f.store
        .db
        .exec(
            "UPDATE requests SET state='completed',result=? WHERE id=?",
            args![&large, &b["request_id"]],
        )
        .await
        .unwrap();
    assert_eq!(
        f.store
            .receipt(&f.phone, s(&b, "request_id"))
            .await
            .unwrap_err()
            .0
            .as_u16(),
        413
    );
    assert_eq!(f.submit(&b).await.unwrap_err().0.as_u16(), 413);
    assert!(f.store.claim(&f.node, false).await.unwrap().is_none());
    let row = f
        .store
        .db
        .one(
            "SELECT state,body_hash FROM requests WHERE id=?",
            args![&b["request_id"]],
        )
        .await
        .unwrap()
        .unwrap();
    assert_eq!(row["state"], "completed");
    assert_eq!(row["body_hash"], digest(canonical(&b).unwrap()));
    f.close().await;
}
#[tokio::test]
async fn budgets_reserve_results_and_release_on_expiry() {
    let f = Fixture::config(Config {
        background: false,
        max_queue: 1,
        ..Config::default()
    })
    .await;
    let b = f.command("send");
    f.submit(&b).await.unwrap();
    assert_eq!(
        f.submit(&f.command("send")).await.unwrap_err().0.as_u16(),
        429
    );
    f.store
        .db
        .exec(
            "UPDATE requests SET created=? WHERE id=?",
            args![now() - 121.0, &b["request_id"]],
        )
        .await
        .unwrap();
    assert_eq!(
        f.store
            .receipt(&f.phone, s(&b, "request_id"))
            .await
            .unwrap()["state"],
        "failed"
    );
    f.submit(&f.command("send")).await.unwrap();
    f.check_accounting().await;
    f.close().await;
}
#[tokio::test]
async fn scoped_pairing_and_rates() {
    let f = Fixture::new().await;
    assert!(f
        .store
        .pair(s(&f.credentials, "pair_code"), "Twice")
        .await
        .is_err());
    assert!(f
        .store
        .authenticate(s(&f.credentials, "node_token"), Role::Device)
        .await
        .is_err());
    for _ in 0..10 {
        f.store
            .rate(&json!(["pair", "127.0.0.1"]), 10, true)
            .await
            .unwrap();
    }
    assert!(f
        .store
        .rate(&json!(["pair", "127.0.0.1"]), 10, true)
        .await
        .is_err());
    f.store
        .rate(
            &json!(["devices", f.phone.workspace, f.phone.id]),
            2400,
            false,
        )
        .await
        .unwrap();
    f.close().await;
}
#[tokio::test]
async fn poll_leases_have_separate_capacity_and_idempotent_release() {
    let f = Fixture::new().await;
    let a = f.store.acquire_poll(&f.phone, 35.0).await.unwrap();
    let b = f.store.acquire_poll(&f.phone, 35.0).await.unwrap();
    assert_eq!(
        f.store
            .acquire_poll(&f.phone, 35.0)
            .await
            .unwrap_err()
            .0
            .as_u16(),
        429
    );
    f.store.release_poll(&a).await.unwrap();
    f.store.release_poll(&a).await.unwrap();
    f.store.release_poll(&b).await.unwrap();
    let r = f
        .store
        .db
        .one(
            "SELECT active_polls FROM workspace_usage WHERE workspace_id=?",
            args![&f.phone.workspace],
        )
        .await
        .unwrap()
        .unwrap();
    assert_eq!(r["active_polls"], 0);
    f.close().await;
}
#[tokio::test]
async fn one_hop_routes_freeze_and_withdraw() {
    let f = Fixture::new().await;
    let root = Uuid::new_v4().to_string();
    let machine = Uuid::new_v4().to_string();
    let route = Uuid::new_v4().to_string();
    let snapshot = json!({"sessions":[],"mobile_capabilities":{"protocol_version":1,"operations":protocol::OPERATIONS,"features":["gateway_one_hop"]}});
    let mut heartbeat = json!({"snapshot":snapshot,"machine_id":root,"peers":[{"route_id":route,"machine_id":machine,"name":"Peer","online":true}]});
    f.store
        .heartbeat(&f.node, &heartbeat, &canonical(&snapshot).unwrap())
        .await
        .unwrap();
    let peer = json!({"machine_id":machine,"snapshot":snapshot});
    f.store
        .peer_heartbeat(&f.node, &route, &peer, &canonical(&snapshot).unwrap())
        .await
        .unwrap();
    let catalog = f.store.computers(&f.phone).await.unwrap();
    let peer = catalog["computers"]
        .as_array()
        .unwrap()
        .iter()
        .find(|r| r["machine_id"] == machine)
        .unwrap();
    assert_eq!(peer["via"]["gateway_id"], f.node.id);
    let mut b = f.command("send");
    b["computer_id"] = peer["id"].clone();
    f.submit(&b).await.unwrap();
    assert!(f.store.claim(&f.node, false).await.unwrap().is_none());
    let claim = f
        .store
        .claim(&f.node, true)
        .await
        .unwrap()
        .unwrap()
        .decode()
        .unwrap();
    assert_eq!(claim["requests"][0]["gateway_route"]["machine_id"], machine);
    heartbeat["peers"] = json!([]);
    f.store
        .heartbeat(&f.node, &heartbeat, &canonical(&snapshot).unwrap())
        .await
        .unwrap();
    assert_eq!(
        f.store
            .receipt(&f.phone, s(&b, "request_id"))
            .await
            .unwrap()["state"],
        "uncertain"
    );
    f.check_accounting().await;
    f.close().await;
}
#[tokio::test]
async fn offline_peer_route_stays_listed_but_unavailable() {
    let f = Fixture::new().await;
    let root = Uuid::new_v4().to_string();
    let machine = Uuid::new_v4().to_string();
    let route = Uuid::new_v4().to_string();
    let snapshot = json!({"sessions":[],"mobile_capabilities":{"protocol_version":1,"operations":protocol::OPERATIONS,"features":["gateway_one_hop"]}});
    let peers = |online: bool| json!([{"route_id":route,"machine_id":machine,"name":"Peer","online":online}]);
    let listed = |catalog: &Value| {
        catalog["computers"]
            .as_array()
            .unwrap()
            .iter()
            .find(|r| r["machine_id"] == machine)
            .cloned()
            .unwrap()
    };
    gateway_beat(&f, &root, &peers(true), &snapshot).await;
    peer_beat(&f, &route, &machine, &snapshot, &Value::Null).await;
    let online = listed(&f.store.computers(&f.phone).await.unwrap());
    assert_eq!(online["online"], true);
    gateway_beat(&f, &root, &peers(false), &snapshot).await;
    let offline = listed(&f.store.computers(&f.phone).await.unwrap());
    assert_eq!(offline["id"], online["id"]);
    assert_eq!(offline["online"], false);
    assert_eq!(offline["via"]["gateway_id"], f.node.id);
    assert!(offline["snapshot"].is_null());
    let mut b = f.command("send");
    b["computer_id"] = offline["id"].clone();
    assert_eq!(f.submit(&b).await.unwrap_err().0.as_u16(), 409);
    let body = json!({"machine_id":machine,"snapshot":snapshot});
    assert_eq!(
        f.store
            .peer_heartbeat(&f.node, &route, &body, &canonical(&snapshot).unwrap())
            .await
            .unwrap_err()
            .0
            .as_u16(),
        409
    );
    assert!(f.store.claim(&f.node, true).await.unwrap().is_none());
    // Reconnection needs a fresh peer observation before the route is usable.
    gateway_beat(&f, &root, &peers(true), &snapshot).await;
    assert_eq!(
        listed(&f.store.computers(&f.phone).await.unwrap())["online"],
        false
    );
    peer_beat(&f, &route, &machine, &snapshot, &Value::Null).await;
    let restored = listed(&f.store.computers(&f.phone).await.unwrap());
    assert_eq!(
        (restored["id"].clone(), restored["online"].clone()),
        (online["id"].clone(), json!(true))
    );
    f.close().await;
}
#[tokio::test]
async fn heartbeats_store_presence_outside_the_snapshot() {
    let empty = json!({"sessions":[]});
    assert!(protocol::heartbeat(&json!({"snapshot":empty,"presence":null})).is_ok());
    assert!(protocol::heartbeat(&json!({"snapshot":empty,"presence":presence(1.0, None)})).is_ok());
    assert!(protocol::heartbeat(&json!({"snapshot":empty,"presence":"idle"})).is_err());
    let machine = Uuid::new_v4().to_string();
    assert!(protocol::peer_heartbeat(
        &json!({"machine_id":machine,"snapshot":empty,"presence":presence(1.0, None)})
    )
    .is_ok());
    assert!(protocol::peer_heartbeat(
        &json!({"machine_id":machine,"snapshot":empty,"presence":[]})
    )
    .is_err());
    let f = Fixture::new().await;
    let snapshot = json!({"sessions":[],"preferences":{"mobile_delivery":"away_10"}});
    let before = now();
    f.beat_with(&snapshot, &presence(30.0, None)).await;
    let row = f
        .node_row("presence_at,presence_until,mobile_delivery")
        .await;
    let at = row["presence_at"].as_f64().unwrap();
    assert!(at >= before && at <= now());
    assert!((row["presence_until"].as_f64().unwrap() - (at - 30.0 + 120.0)).abs() < 1e-6);
    assert_eq!(row["mobile_delivery"], "away_10");
    // A presence-only change is remembered without rewriting the unchanged snapshot.
    f.store
        .db
        .exec(
            "UPDATE nodes SET snapshot='{}' WHERE id=?",
            args![&f.node.id],
        )
        .await
        .unwrap();
    f.beat_with(&snapshot, &presence(5.0, Some(1.0))).await;
    let row = f.node_row("snapshot,presence_at,presence_until").await;
    assert_eq!(row["snapshot"], "{}");
    let locked = row["presence_at"].as_f64().unwrap();
    assert!(
        locked >= at && (row["presence_until"].as_f64().unwrap() - (locked - 1.0)).abs() < 1e-6
    );
    // Null, absent and malformed presence keep the remembered value.
    let remembered = f.node_row("presence_at,presence_until").await;
    f.beat_with(&snapshot, &Value::Null).await;
    f.beat(&snapshot).await;
    f.beat_with(
        &json!({"sessions":[],"preferences":{"mobile_delivery":"away_45"}}),
        &json!({"idle_seconds":"5","locked":false}),
    )
    .await;
    assert_eq!(f.node_row("presence_at,presence_until").await, remembered);
    assert!(f.node_row("mobile_delivery").await["mobile_delivery"].is_null());
    f.close().await;
}
#[tokio::test]
async fn presence_holds_read_drops_and_lock_delivers_alert() {
    let f = Fixture::new().await;
    let features = protocol::capabilities(vec![])["features"].clone();
    assert!(features
        .as_array()
        .unwrap()
        .iter()
        .any(|v| v == "presence_alerts"));
    f.store
        .register_push(&f.phone, "fcm", "synthetic-token")
        .await
        .unwrap();
    let active = presence(3.0, None);
    let (first, second) = (signature('1'), signature('2'));
    f.beat_with(
        &listing(session_row("working", "busy", "", None, Value::Null)),
        &active,
    )
    .await;
    f.beat_with(
        &listing(session_row(
            "input",
            "busy",
            "q1",
            Some(&first),
            Value::Null,
        )),
        &active,
    )
    .await;
    assert_eq!(f.events_of("attention").await.len(), 1);
    assert_eq!(
        (
            f.count("alert_candidates").await,
            f.alert_count().await,
            f.count("push_jobs").await
        ),
        (1, 0, 0)
    );
    // Reading the question on a desktop drops the held candidate.
    let read = json!({"conversation_id":"c","reply_id":null,"attention_signature":first,"at":1.0});
    f.beat_with(
        &listing(session_row(
            "input",
            "busy",
            "q1",
            Some(&first),
            read.clone(),
        )),
        &active,
    )
    .await;
    assert_eq!(f.count("alert_candidates").await, 0);
    // A replacement question waits while the owner is active.
    let waiting = listing(session_row("input", "busy", "q2", Some(&second), read));
    f.beat_with(&waiting, &active).await;
    assert_eq!(f.count("alert_candidates").await, 1);
    // Identical snapshot content: the lock alone arrives and delivers.
    f.beat_with(&waiting, &presence(4.0, Some(2.0))).await;
    let alerts = f.events_of("alert").await;
    assert_eq!(alerts.len(), 1);
    assert_eq!(
        (
            alerts[0]["alert_kind"].as_str(),
            alerts[0]["session"].as_str(),
            alerts[0]["computer_id"].as_str()
        ),
        (Some("attention"), Some("example"), Some(f.node.id.as_str()))
    );
    assert!(f
        .events_of("attention")
        .await
        .iter()
        .all(|e| e.get("alert_kind").is_none()));
    let jobs = f.store.claim_push_jobs().await.unwrap();
    assert_eq!(jobs.len(), 1);
    assert_eq!(
        serde_json::from_str::<Value>(s(&jobs[0], "payload")).unwrap(),
        json!({"event_id":alerts[0]["id"],"kind":"wake"})
    );
    assert_eq!(f.count("alert_candidates").await, 0);
    f.close().await;
}
#[tokio::test]
async fn unannounced_signature_change_keeps_held_candidate() {
    let f = Fixture::new().await;
    let active = presence(1.0, None);
    let question = session_row("input", "busy", "q1", Some(&signature('1')), Value::Null);
    f.beat_with(
        &listing(session_row("working", "busy", "", None, Value::Null)),
        &active,
    )
    .await;
    f.beat_with(&listing(question.clone()), &active).await;
    // A subagent asks too: hgs changes the signature, the relay emits nothing.
    let mut changed = question;
    changed["subagents"] = json!({"a1":{"state":"approval","attention_id":"sub1"}});
    changed["attention_signature"] = json!(signature('2'));
    f.beat_with(&listing(changed.clone()), &active).await;
    assert_eq!(f.events_of("attention").await.len(), 1);
    assert_eq!(
        (f.count("alert_candidates").await, f.alert_count().await),
        (1, 0)
    );
    f.beat_with(&listing(changed), &presence(2.0, Some(1.0)))
        .await;
    assert_eq!(
        (f.count("alert_candidates").await, f.alert_count().await),
        (0, 1)
    );
    f.close().await;
}
#[tokio::test]
async fn immediate_preference_ignores_active_desktop() {
    let f = Fixture::new().await;
    let snapshot = |phase: &str| json!({"sessions":[session_row(phase,"busy","q1",Some(&signature('1')),Value::Null)],"preferences":{"mobile_delivery":"immediate"}});
    f.beat_with(&snapshot("working"), &presence(1.0, None))
        .await;
    f.beat_with(&snapshot("input"), &presence(1.0, None)).await;
    assert_eq!(
        (f.alert_count().await, f.count("alert_candidates").await),
        (1, 0)
    );
    f.close().await;
}
#[tokio::test]
async fn completed_candidates_follow_reply_read_and_busy_state() {
    let f = Fixture::new().await;
    let active = presence(1.0, None);
    let none = Value::Null;
    let turn = |activity: &str, reply: &str, read: &Value| {
        listing(
            json!({"name":"example","run_id":"r","conversation_id":"c","phase":"idle","activity":activity,"reply_id":reply,"read":read}),
        )
    };
    f.beat_with(&turn("busy", "rp0", &none), &active).await;
    f.beat_with(&turn("idle", "rp1", &none), &active).await;
    assert_eq!(f.count("alert_candidates").await, 1);
    f.beat_with(&turn("busy", "rp1", &none), &active).await; // busy again
    assert_eq!(f.count("alert_candidates").await, 0);
    f.beat_with(&turn("idle", "rp2", &none), &active).await;
    let read = json!({"conversation_id":"c","reply_id":"rp2","attention_signature":null,"at":1.0});
    f.beat_with(&turn("idle", "rp2", &read), &active).await; // read on a desktop
    assert_eq!(f.count("alert_candidates").await, 0);
    f.beat_with(&turn("busy", "rp2", &read), &active).await;
    f.beat_with(&turn("idle", "rp3", &read), &active).await;
    f.beat_with(&turn("idle", "rp4", &read), &active).await; // a newer reply supersedes it
    assert_eq!(f.count("alert_candidates").await, 0);
    f.beat_with(&turn("busy", "rp4", &read), &active).await;
    f.beat_with(&turn("idle", "rp5", &read), &active).await;
    f.beat_with(&turn("idle", "rp5", &read), &presence(1.0, Some(0.0)))
        .await;
    let kinds: Vec<_> = f
        .events_of("alert")
        .await
        .iter()
        .map(|e| e["alert_kind"].as_str().unwrap().to_owned())
        .collect();
    assert_eq!(kinds, ["completed"]);
    f.close().await;
}
#[tokio::test]
async fn peer_desktop_presence_holds_gateway_alerts() {
    let f = Fixture::new().await;
    let (root, machine, route) = (
        Uuid::new_v4().to_string(),
        Uuid::new_v4().to_string(),
        Uuid::new_v4().to_string(),
    );
    let caps = json!({"protocol_version":1,"operations":protocol::OPERATIONS,"features":["gateway_one_hop"]});
    let peers = json!([{"route_id":route,"machine_id":machine,"name":"Peer","online":true}]);
    // The gateway GUI is closed (presence null); the owner works at the peer.
    let gateway = |phase: &str| json!({"sessions":[session_row(phase,"busy","q1",Some(&signature('1')),Value::Null)],"mobile_capabilities":caps});
    let peer = json!({"sessions":[],"mobile_capabilities":caps});
    gateway_beat(&f, &root, &peers, &gateway("working")).await;
    peer_beat(&f, &route, &machine, &peer, &presence(1.0, None)).await;
    gateway_beat(&f, &root, &peers, &gateway("input")).await;
    assert_eq!(
        (f.count("alert_candidates").await, f.alert_count().await),
        (1, 0)
    );
    peer_beat(&f, &route, &machine, &peer, &presence(2.0, Some(1.0))).await;
    let alerts = f.events_of("alert").await;
    assert_eq!(alerts.len(), 1);
    assert_eq!(alerts[0]["computer_id"], f.node.id);
    f.close().await;
}
#[tokio::test]
async fn older_connectors_forward_presence_inside_the_snapshot() {
    let f = Fixture::new().await;
    let with = |p: Value| json!({"sessions":[],"desktop_presence":p});
    let before = now();
    f.beat(&with(presence(30.0, None))).await;
    let row = f.node_row("presence_at,presence_until,snapshot").await;
    let at = row["presence_at"].as_f64().unwrap();
    assert!(at >= before && at <= now());
    assert!((row["presence_until"].as_f64().unwrap() - (at - 30.0 + 120.0)).abs() < 1e-6);
    // The stored snapshot stays verbatim.
    assert!(row["snapshot"]
        .as_str()
        .unwrap()
        .contains("desktop_presence"));
    // Malformed embedded presence keeps the previous values.
    let remembered = f.node_row("presence_at,presence_until").await;
    f.beat(&with(json!({"idle_seconds":"5","locked":false})))
        .await;
    assert_eq!(f.node_row("presence_at,presence_until").await, remembered);
    // A valid heartbeat field wins over the embedded fallback.
    f.beat_with(&with(presence(30.0, None)), &presence(5.0, Some(1.0)))
        .await;
    let row = f.node_row("presence_at,presence_until").await;
    let at = row["presence_at"].as_f64().unwrap();
    assert!((row["presence_until"].as_f64().unwrap() - (at - 1.0)).abs() < 1e-6);
    f.close().await;
}
#[tokio::test]
async fn away_minutes_preference_waits_from_end_of_activity() {
    let f = Fixture::new().await;
    // Input stopped 200 s ago: away for 80 s, so `away_5` waits another 220 s.
    let snapshot = |phase: &str| json!({"sessions":[session_row(phase,"busy","q1",Some(&signature('1')),Value::Null)],"preferences":{"mobile_delivery":"away_5"}});
    f.beat_with(&snapshot("working"), &presence(200.0, None))
        .await;
    f.beat_with(&snapshot("input"), &presence(200.0, None))
        .await;
    f.store.maintain(false).await.unwrap();
    assert_eq!(
        (f.count("alert_candidates").await, f.alert_count().await),
        (1, 0)
    );
    let next = f
        .store
        .db
        .one("SELECT next_at FROM alert_candidates", &[])
        .await
        .unwrap()
        .unwrap();
    assert!(next["next_at"].as_f64().unwrap() <= now() + 15.0);
    // Five away minutes later the maintenance pass delivers it.
    f.store
        .db
        .exec("UPDATE nodes SET presence_until=presence_until-220", &[])
        .await
        .unwrap();
    f.store
        .db
        .exec("UPDATE alert_candidates SET next_at=0", &[])
        .await
        .unwrap();
    f.store.maintain(false).await.unwrap();
    assert_eq!(
        (f.count("alert_candidates").await, f.alert_count().await),
        (0, 1)
    );
    f.close().await;
}
#[tokio::test]
async fn stale_presence_keeps_last_activity_for_away_minutes() {
    let f = Fixture::new().await;
    let snapshot = |phase: &str| json!({"sessions":[session_row(phase,"busy","q1",Some(&signature('1')),Value::Null)],"preferences":{"mobile_delivery":"away_10"}});
    // The lid closes and locks the screen; then heartbeats stop.
    f.beat_with(&snapshot("working"), &presence(0.0, Some(0.0)))
        .await;
    f.beat_with(&snapshot("input"), &presence(0.0, Some(0.0)))
        .await;
    let five_minutes =
        "UPDATE nodes SET presence_at=presence_at-300,presence_until=presence_until-300";
    f.store.db.exec(five_minutes, &[]).await.unwrap();
    f.store
        .db
        .exec("UPDATE alert_candidates SET next_at=0", &[])
        .await
        .unwrap();
    f.store.maintain(false).await.unwrap();
    // Stale presence is not present, but only five of ten away minutes have passed.
    assert_eq!(
        (f.count("alert_candidates").await, f.alert_count().await),
        (1, 0)
    );
    f.store.db.exec(five_minutes, &[]).await.unwrap();
    f.store
        .db
        .exec("UPDATE alert_candidates SET next_at=0", &[])
        .await
        .unwrap();
    f.store.maintain(false).await.unwrap();
    assert_eq!(
        (f.count("alert_candidates").await, f.alert_count().await),
        (0, 1)
    );
    f.close().await;
}
#[tokio::test]
async fn maintenance_recovers_unevaluated_candidates() {
    let f = Fixture::new().await;
    f.beat(&json!({"sessions":[{"name":"example","run_id":"r","conversation_id":"c","phase":"input","activity":"busy","attention_id":"q1"}]})).await;
    // A committed candidate whose post-snapshot evaluation never ran (worker exit or BUSY).
    f.store.db.exec("INSERT INTO alert_candidates(workspace_id,computer_id,session,kind,run_id,conversation_id,created,next_at) VALUES(?,?,'example','attention','r','c',?,?)", args![&f.node.workspace, &f.node.id, now(), now()]).await.unwrap();
    f.store.maintain(false).await.unwrap();
    let alerts = f.events_of("alert").await;
    assert_eq!(alerts.len(), 1);
    assert_eq!(alerts[0]["alert_kind"], "attention");
    assert_eq!(f.count("alert_candidates").await, 0);
    f.close().await;
}
#[tokio::test]
async fn revoked_computer_drops_held_candidate() {
    let f = Fixture::new().await;
    let active = presence(1.0, None);
    f.beat_with(
        &listing(session_row(
            "working",
            "busy",
            "q1",
            Some(&signature('1')),
            Value::Null,
        )),
        &active,
    )
    .await;
    f.beat_with(
        &listing(session_row(
            "input",
            "busy",
            "q1",
            Some(&signature('1')),
            Value::Null,
        )),
        &active,
    )
    .await;
    assert_eq!(f.count("alert_candidates").await, 1);
    f.store.revoke_computer(&f.node.id).await.unwrap();
    f.store
        .db
        .exec("UPDATE alert_candidates SET next_at=0", &[])
        .await
        .unwrap();
    f.store.maintain(false).await.unwrap();
    assert_eq!(
        (f.count("alert_candidates").await, f.alert_count().await),
        (0, 0)
    );
    f.close().await;
}
#[tokio::test]
async fn alert_candidates_are_bounded_and_expire() {
    let f = Fixture::new().await;
    let fleet = |phase: &str| json!({"sessions":(0..1001).map(|k| json!({"name":format!("s{k}"),"run_id":"r","conversation_id":"c","phase":phase})).collect::<Vec<_>>()});
    f.beat_with(&fleet("working"), &presence(1.0, None)).await;
    f.beat_with(&fleet("input"), &presence(1.0, None)).await;
    assert_eq!(f.count("alert_candidates").await, 1000);
    assert_eq!(f.count("alert_candidates WHERE session='s0'").await, 0);
    assert_eq!(f.alert_count().await, 0);
    f.store
        .db
        .exec("UPDATE alert_candidates SET created=created-86401", &[])
        .await
        .unwrap();
    f.store.maintain(false).await.unwrap();
    assert_eq!(f.count("alert_candidates").await, 1000 - 256);
    for _ in 0..3 {
        f.store.maintain(false).await.unwrap();
    }
    assert_eq!(
        (f.count("alert_candidates").await, f.alert_count().await),
        (0, 0)
    );
    f.close().await;
}
#[tokio::test]
async fn maintenance_rechecks_most_overdue_workspaces_first() {
    let f = Fixture::new().await;
    // More waiting workspaces than one pass takes (32): owners at their desks
    // keep 33 of them waiting, so every pass is full.
    let mut spaces = vec![];
    for _ in 0..35 {
        let c = f
            .store
            .provision("Example workspace", "Example computer")
            .await
            .unwrap();
        spaces.push((
            s(&c, "workspace_id").to_owned(),
            s(&c, "node_id").to_owned(),
        ));
    }
    spaces.sort();
    let (expired, asleep) = (&spaces[33], &spaces[34]);
    let at = now();
    let insert = "INSERT INTO alert_candidates(workspace_id,computer_id,session,kind,run_id,conversation_id,created,next_at) VALUES(?,?,'example','attention','r','c',?,?)";
    for (w, node) in &spaces[..33] {
        f.store
            .db
            .exec(
                "UPDATE nodes SET presence_at=?,presence_until=? WHERE id=?",
                args![at, at + 600.0, node],
            )
            .await
            .unwrap();
        f.store
            .db
            .exec(insert, args![w, node, at, at - 15.0])
            .await
            .unwrap();
    }
    // The highest workspace id is the most overdue and its machine sleeps; the
    // next one holds only an expired candidate that is not due.
    f.store
        .db
        .exec(insert, args![&asleep.0, &asleep.1, at, at - 30.0])
        .await
        .unwrap();
    f.store
        .db
        .exec(
            insert,
            args![&expired.0, &expired.1, at - 86401.0, at + 600.0],
        )
        .await
        .unwrap();
    let mut rechecked = std::collections::BTreeSet::new();
    for pass in 0..2 {
        let before = now();
        f.store.maintain(false).await.unwrap();
        if pass == 0 {
            let alert = format!("events WHERE kind='alert' AND workspace_id='{}'", asleep.0);
            assert_eq!((f.count(&alert).await, f.alert_count().await), (1, 1));
            assert_eq!(
                f.count(&format!(
                    "alert_candidates WHERE workspace_id='{}'",
                    expired.0
                ))
                .await,
                0
            );
        }
        let mut t = f.store.db.begin().await.unwrap();
        for r in t
            .all(
                "SELECT workspace_id FROM alert_candidates WHERE next_at>?",
                args![before],
            )
            .await
            .unwrap()
        {
            rechecked.insert(s(&r, "workspace_id").to_owned());
        }
        t.commit().await.unwrap();
        // Fifteen seconds pass for every waiting candidate.
        f.store
            .db
            .exec("UPDATE alert_candidates SET next_at=next_at-15", &[])
            .await
            .unwrap();
    }
    // Every waiting workspace was rechecked within two full passes.
    assert_eq!(rechecked.len(), 33);
    f.close().await;
}
#[tokio::test]
async fn snapshot_events_and_push_claims() {
    let f = Fixture::new().await;
    f.store
        .register_push(&f.phone, "fcm", "synthetic-token")
        .await
        .unwrap();
    for phase in ["idle", "input"] {
        let snapshot = json!({"sessions":[{"name":"example","phase":phase,"run_id":"r","conversation_id":"c"}]});
        f.store
            .heartbeat(
                &f.node,
                &json!({"snapshot":snapshot}),
                &canonical(&snapshot).unwrap(),
            )
            .await
            .unwrap();
    }
    // A connector without presence counts as away: each transition alerts at once.
    let e = f.store.events(&f.phone, 0).await.unwrap();
    let kinds: Vec<_> = e["events"]
        .as_array()
        .unwrap()
        .iter()
        .map(|v| v["kind"].as_str().unwrap())
        .collect();
    assert_eq!(kinds, ["attention", "alert"]);
    assert_eq!(e["events"][1]["alert_kind"], "attention");
    let a = f.store.claim_push_jobs().await.unwrap();
    assert_eq!(a.len(), 1);
    assert!(f.store.claim_push_jobs().await.unwrap().is_empty());
    let reg = f
        .store
        .push_registration(&f.phone.id)
        .await
        .unwrap()
        .unwrap();
    f.store
        .finish_push(&a[0], true, false, Some(&reg))
        .await
        .unwrap();
    f.store
        .finish_push(&a[0], true, false, Some(&reg))
        .await
        .unwrap();
    let count = f
        .store
        .db
        .one("SELECT push_jobs,events FROM global_usage WHERE id=1", &[])
        .await
        .unwrap()
        .unwrap();
    assert_eq!(count["push_jobs"], 0);
    assert_eq!(count["events"], 2);
    f.close().await;
}
#[tokio::test]
async fn alert_during_an_in_flight_wake_wakes_again() {
    let f = Fixture::new().await;
    f.store
        .register_push(&f.phone, "fcm", "synthetic-token")
        .await
        .unwrap();
    let reg = f
        .store
        .push_registration(&f.phone.id)
        .await
        .unwrap()
        .unwrap();
    let question = |attention: &str, digit: char| {
        listing(session_row(
            "input",
            "busy",
            attention,
            Some(&signature(digit)),
            Value::Null,
        ))
    };
    let wake = |job: &Value| serde_json::from_str::<Value>(s(job, "payload")).unwrap();
    // Without presence the owner is away, so every question alerts at once.
    f.beat(&listing(session_row(
        "working",
        "busy",
        "",
        None,
        Value::Null,
    )))
    .await;
    f.beat(&question("q1", '1')).await;
    let first = f.store.claim_push_jobs().await.unwrap();
    assert_eq!(first.len(), 1);
    // The next alert commits while the first wake is being sent.
    f.beat(&question("q2", '2')).await;
    let alerts = f.events_of("alert").await;
    assert_eq!(alerts.len(), 2);
    f.store
        .finish_push(&first[0], true, false, Some(&reg))
        .await
        .unwrap();
    let second = f.store.claim_push_jobs().await.unwrap();
    assert_eq!(second.len(), 1);
    assert_eq!(
        wake(&second[0]),
        json!({"event_id":alerts[1]["id"],"kind":"wake"})
    );
    // A failed send with a newer alert is due again at once, as a new job would be.
    f.beat(&question("q3", '3')).await;
    f.store
        .finish_push(&second[0], false, false, Some(&reg))
        .await
        .unwrap();
    let third = f.store.claim_push_jobs().await.unwrap();
    assert_eq!(third.len(), 1);
    assert_eq!(
        (
            wake(&third[0])["event_id"].clone(),
            i(&third[0], "attempts")
        ),
        (f.events_of("alert").await[2]["id"].clone(), 0)
    );
    // Without a newer alert the delivered job is finished.
    f.store
        .finish_push(&third[0], true, false, Some(&reg))
        .await
        .unwrap();
    assert_eq!(f.count("push_jobs").await, 0);
    let usage = f
        .store
        .db
        .one("SELECT push_jobs FROM global_usage WHERE id=1", &[])
        .await
        .unwrap()
        .unwrap();
    assert_eq!(usage["push_jobs"], 0);
    f.close().await;
}
#[tokio::test]
async fn retired_unifiedpush_registration_is_removed_on_delivery() {
    let f = Fixture::new().await;
    f.store
        .register_push(&f.phone, "unifiedpush", "https://push.example.test/opaque")
        .await
        .unwrap();
    for phase in ["idle", "input"] {
        let snapshot = json!({"sessions":[{"name":"example","phase":phase,"run_id":"r","conversation_id":"c"}]});
        f.store
            .heartbeat(
                &f.node,
                &json!({"snapshot":snapshot}),
                &canonical(&snapshot).unwrap(),
            )
            .await
            .unwrap();
    }
    let push = zerus_relay::push::Push::new(f.store.clone()).await.unwrap();
    assert!(push.providers().is_empty());
    push.once().await.unwrap();
    assert!(f
        .store
        .push_registration(&f.phone.id)
        .await
        .unwrap()
        .is_none());
    f.close().await;
}
#[test]
fn retired_push_hosts_config_still_loads() {
    use std::os::unix::fs::PermissionsExt;
    let dir = tempfile::tempdir().unwrap();
    let path = dir.path().join("relay.json");
    std::fs::write(&path, br#"{"push_hosts":["push.example.test"]}"#).unwrap();
    std::fs::set_permissions(&path, std::fs::Permissions::from_mode(0o600)).unwrap();
    let cfg = Config::read(Some(&path)).unwrap();
    assert!(serde_json::to_value(&cfg)
        .unwrap()
        .get("push_hosts")
        .is_none());
}
#[test]
fn canonical_python_identity() {
    for (raw, expected) in [
        (
            r#"{"z":"😀é\u007f","a":1e20}"#,
            r#"{"a":1e+20,"z":"\ud83d\ude00\u00e9\u007f"}"#,
        ),
        ("1e-5", "1e-05"),
        ("1e-4", "0.0001"),
        ("1.0", "1.0"),
        ("-0.0", "-0.0"),
        ("1e16", "1e+16"),
        ("1e15", "1000000000000000.0"),
        (
            "123456789012345678901234567890",
            "123456789012345678901234567890",
        ),
    ] {
        let v: Value = serde_json::from_str(raw).unwrap();
        assert_eq!(canonical(&v).unwrap(), expected, "{raw}");
    }
    assert_eq!(
        digest("hello"),
        "2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824"
    );
}
#[test]
fn malformed_and_deep_json() {
    assert!(zerus_relay::json::parse(&[b'['; 33], 32, 50000).is_err());
}
#[test]
fn proxy_trust_is_explicit() {
    let mut cfg = Config::default();
    let peer = "127.0.0.1".parse().unwrap();
    assert_eq!(
        zerus_relay::http::client_identity(peer, Some("203.0.113.5"), &cfg).unwrap(),
        "127.0.0.1"
    );
    cfg.trusted_proxy_cidrs = vec!["127.0.0.1/32".parse().unwrap()];
    assert_eq!(
        zerus_relay::http::client_identity(peer, Some("198.51.100.7, 203.0.113.5"), &cfg).unwrap(),
        "203.0.113.5"
    );
    assert!(zerus_relay::http::client_identity(peer, Some("garbage"), &cfg).is_err());
}
#[tokio::test]
async fn http_end_to_end_and_disconnect_cleanup() {
    let f = Fixture::new().await;
    let app = App::new(f.store.clone()).await.unwrap();
    let listener = tokio::net::TcpListener::bind("127.0.0.1:0").await.unwrap();
    let url = format!("http://{}", listener.local_addr().unwrap());
    let stop = CancellationToken::new();
    let server = tokio::spawn(serve(listener, app, stop.clone()));
    let client = reqwest::Client::new();
    assert_eq!(
        client
            .get(format!("{url}/healthz"))
            .send()
            .await
            .unwrap()
            .status(),
        200
    );
    let b = f.command("send");
    let response = client
        .post(format!("{url}/v1/requests"))
        .bearer_auth(s(&f.paired, "device_token"))
        .json(&b)
        .send()
        .await
        .unwrap();
    assert_eq!(response.status(), 202, "{}", response.text().await.unwrap());
    let result: Value = client
        .get(format!("{url}/v1/node/requests"))
        .bearer_auth(s(&f.credentials, "node_token"))
        .send()
        .await
        .unwrap()
        .json()
        .await
        .unwrap();
    assert_eq!(result["requests"][0]["request_id"], b["request_id"]);
    let poll_client = client.clone();
    let endpoint = format!("{url}/v1/events?wait=25");
    let token = s(&f.paired, "device_token").to_string();
    let poll =
        tokio::spawn(async move { poll_client.get(endpoint).bearer_auth(token).send().await });
    for _ in 0..100 {
        let r = f
            .store
            .db
            .one("SELECT count(*) AS n FROM poll_leases", &[])
            .await
            .unwrap()
            .unwrap();
        if i(&r, "n") == 1 {
            break;
        }
        tokio::time::sleep(Duration::from_millis(10)).await;
    }
    poll.abort();
    let _ = poll.await;
    let mut cleaned = false;
    for _ in 0..200 {
        let r = f
            .store
            .db
            .one("SELECT count(*) AS n FROM poll_leases", &[])
            .await
            .unwrap()
            .unwrap();
        if i(&r, "n") == 0 {
            cleaned = true;
            break;
        }
        tokio::time::sleep(Duration::from_millis(10)).await;
    }
    stop.cancel();
    server.await.unwrap().unwrap();
    assert!(
        cleaned,
        "disconnected polls must release durable leases promptly"
    );
    f.close().await;
}

#[tokio::test]
#[ignore = "requires isolated ZERUS_RELAY_TEST_DATABASE_URL; run explicitly in CI"]
async fn postgres_cross_worker_speech() {
    use sqlx::Connection;
    let base =
        std::env::var("ZERUS_RELAY_TEST_DATABASE_URL").expect("isolated PostgreSQL required");
    let mut admin = sqlx::PgConnection::connect(&base).await.unwrap();
    let name = format!("relay_test_{}", Uuid::new_v4().simple());
    sqlx::query(&format!("CREATE DATABASE {name}"))
        .execute(&mut admin)
        .await
        .unwrap();
    let mut url = url::Url::parse(&base).unwrap();
    url.set_path(&name);
    let cfg = Config {
        database_url: Some(url.to_string()),
        background: false,
        ..Config::default()
    };
    let f = Fixture::config(cfg.clone()).await;
    let second = Store::new(
        Db::open(std::path::Path::new("unused"), Arc::new(cfg))
            .await
            .unwrap(),
    );
    f.beat(&json!({"sessions":[],"mobile_capabilities":{"protocol_version":1,"operations":protocol::SPEECH}})).await;
    let upload = speech_command(&f.node.id, "speech_submit", None);
    f.submit(&upload).await.unwrap();
    let (a, b) = tokio::join!(f.store.claim(&f.node, false), second.claim(&f.node, false));
    assert_eq!(a.unwrap().is_some() as u8 + b.unwrap().is_some() as u8, 1);
    let reply = json!({"state":"completed","result":{"schema":1,"job_id":upload["request_id"],"state":"queued"},"error":null});
    second
        .result(
            &f.node,
            s(&upload, "request_id"),
            &reply,
            Some(&canonical(&reply["result"]).unwrap()),
        )
        .await
        .unwrap();
    let row = f
        .store
        .db
        .one(
            "SELECT body,body_bytes FROM requests WHERE id=?",
            args![&upload["request_id"]],
        )
        .await
        .unwrap()
        .unwrap();
    assert_eq!(row["body"], "");
    assert_eq!(row["body_bytes"], 0);
    assert_eq!(f.submit(&upload).await.unwrap()["result"], reply["result"]);
    let status = speech_command(&f.node.id, "speech_status", Some(s(&upload, "request_id")));
    f.submit(&status).await.unwrap();
    assert!(second.claim(&f.node, false).await.unwrap().is_some());
    let result = json!({"state":"completed","result":{"schema":1,"job_id":upload["request_id"],"state":"completed","text":"Synthetic speech"},"error":null});
    second
        .result(
            &f.node,
            s(&status, "request_id"),
            &result,
            Some(&canonical(&result["result"]).unwrap()),
        )
        .await
        .unwrap();
    second
        .db
        .exec(
            "UPDATE requests SET updated=? WHERE id=?",
            args![now() - 121.0, &status["request_id"]],
        )
        .await
        .unwrap();
    second.maintain(false).await.unwrap();
    assert!(f
        .store
        .db
        .one(
            "SELECT id FROM requests WHERE id=?",
            args![&status["request_id"]]
        )
        .await
        .unwrap()
        .is_none());
    f.beat(&json!({"sessions":[{"name":"codex/example/tag"}],"mobile_capabilities":{"protocol_version":1,"operations":protocol::OPERATIONS}})).await;
    let voice = voice_command(&f.node.id, "voice_submit", None);
    f.submit(&voice).await.unwrap();
    let (a, b) = tokio::join!(f.store.claim(&f.node, false), second.claim(&f.node, false));
    assert_eq!(a.unwrap().is_some() as u8 + b.unwrap().is_some() as u8, 1);
    let accepted = json!({"state":"completed","result":{"schema":1,"message":{"id":voice["request_id"],"state":"queued","revision":1}},"error":null});
    second
        .result(
            &f.node,
            s(&voice, "request_id"),
            &accepted,
            Some(&canonical(&accepted["result"]).unwrap()),
        )
        .await
        .unwrap();
    let stored = f
        .store
        .db
        .one(
            "SELECT body,body_bytes FROM requests WHERE id=?",
            args![&voice["request_id"]],
        )
        .await
        .unwrap()
        .unwrap();
    assert_eq!(stored["body"], "");
    assert_eq!(stored["body_bytes"], 0);
    assert_eq!(
        f.submit(&voice).await.unwrap()["result"],
        accepted["result"]
    );
    speech_config_storage_contract(&f, &second).await;
    f.check_accounting().await;
    second.db.close().await;
    f.close().await;
    sqlx::query(&format!("DROP DATABASE {name}"))
        .execute(&mut admin)
        .await
        .unwrap();
    admin.close().await.unwrap();
}

#[tokio::test]
#[ignore = "requires isolated ZERUS_RELAY_TEST_DATABASE_URL; run explicitly in CI"]
async fn postgres_cross_worker_contract() {
    use sqlx::Connection;
    let base =
        std::env::var("ZERUS_RELAY_TEST_DATABASE_URL").expect("isolated PostgreSQL required");
    let mut admin = sqlx::PgConnection::connect(&base).await.unwrap();
    let name = format!("relay_test_{}", Uuid::new_v4().simple());
    sqlx::query(&format!("CREATE DATABASE {name}"))
        .execute(&mut admin)
        .await
        .unwrap();
    let mut url = url::Url::parse(&base).unwrap();
    url.set_path(&name);
    let cfg = Config {
        database_url: Some(url.to_string()),
        background: false,
        ..Config::default()
    };
    let f = Fixture::config(cfg.clone()).await;
    let second = Store::new(
        Db::open(std::path::Path::new("unused"), Arc::new(cfg.clone()))
            .await
            .unwrap(),
    );
    let b = f.command("send");
    let mut wake = second.db.wake.subscribe();
    f.submit(&b).await.unwrap();
    let notified = tokio::time::timeout(Duration::from_secs(2), wake.recv())
        .await
        .unwrap()
        .unwrap();
    assert_eq!(notified, format!("node:{}", f.node.id));
    let (a, c) = tokio::join!(f.store.claim(&f.node, false), second.claim(&f.node, false));
    assert_eq!(
        usize::from(a.unwrap().is_some()) + usize::from(c.unwrap().is_some()),
        1
    );
    second.maintain(true).await.unwrap();
    assert_eq!(
        second.receipt(&f.phone, s(&b, "request_id")).await.unwrap()["state"],
        "claimed"
    );
    let result = json!({"state":"completed","result":{"ok":true},"error":null});
    let encoded = canonical(&result["result"]).unwrap();
    second
        .result(&f.node, s(&b, "request_id"), &result, Some(&encoded))
        .await
        .unwrap();
    f.check_accounting().await;
    let lease = f.store.acquire_poll(&f.phone, 35.0).await.unwrap();
    let lease2 = second.acquire_poll(&f.phone, 35.0).await.unwrap();
    assert!(second.acquire_poll(&f.phone, 35.0).await.is_err());
    second.release_poll(&lease).await.unwrap();
    f.store.release_poll(&lease2).await.unwrap();
    // Quota/accounting locks do not obstruct unchanged heartbeat liveness.
    let snap = json!({"sessions":[],"mobile_capabilities":{"protocol_version":1,"features":["gateway_one_hop"],"operations":protocol::OPERATIONS}});
    let heartbeat = json!({"snapshot":snap,"machine_id":Uuid::new_v4().to_string(),"peers":[]});
    second
        .heartbeat(&f.node, &heartbeat, &canonical(&snap).unwrap())
        .await
        .unwrap();
    let mut lock = f.store.db.begin().await.unwrap();
    f.store
        .db
        .payload_usage(&mut lock, &f.node.workspace)
        .await
        .unwrap();
    tokio::time::timeout(
        Duration::from_secs(1),
        second.heartbeat(&f.node, &heartbeat, &canonical(&snap).unwrap()),
    )
    .await
    .unwrap()
    .unwrap();
    drop(lock);
    // Changed event-free snapshots also bypass accounting locks.
    let mut lock = f.store.db.begin().await.unwrap();
    f.store
        .db
        .payload_usage(&mut lock, &f.node.workspace)
        .await
        .unwrap();
    let next =
        json!({"sessions":[],"sequence":2,"mobile_capabilities":snap["mobile_capabilities"]});
    let hb = json!({"snapshot":next,"machine_id":heartbeat["machine_id"],"peers":[]});
    tokio::time::timeout(
        Duration::from_secs(1),
        second.heartbeat(&f.node, &hb, &canonical(&next).unwrap()),
    )
    .await
    .unwrap()
    .unwrap();
    drop(lock);
    // Push and event publication share the same durable accounting across workers.
    f.store
        .register_push(&f.phone, "fcm", "synthetic-token")
        .await
        .unwrap();
    for phase in ["idle", "input"] {
        let snap = json!({"sessions":[{"name":"test","phase":phase,"run_id":"r","conversation_id":"c"}],"mobile_capabilities":next["mobile_capabilities"]});
        let hb = json!({"snapshot":snap,"machine_id":heartbeat["machine_id"],"peers":[]});
        second
            .heartbeat(&f.node, &hb, &canonical(&snap).unwrap())
            .await
            .unwrap();
    }
    assert_eq!(
        f.store.events(&f.phone, 0).await.unwrap()["events"]
            .as_array()
            .unwrap()
            .len(),
        2
    );
    let (a, b) = tokio::join!(f.store.claim_push_jobs(), second.claim_push_jobs());
    assert_eq!(a.unwrap().len() + b.unwrap().len(), 1);
    // An active desktop reschedules the held candidate; the lock alone then
    // delivers on the other worker from the stored snapshot.
    let held = json!({"sessions":[session_row("input","busy","q2",Some(&signature('2')),Value::Null)],"mobile_capabilities":next["mobile_capabilities"]});
    let beat = |p: Value| json!({"snapshot":held,"machine_id":heartbeat["machine_id"],"peers":[],"presence":p});
    second
        .heartbeat(
            &f.node,
            &beat(presence(1.0, None)),
            &canonical(&held).unwrap(),
        )
        .await
        .unwrap();
    assert_eq!(
        (f.count("alert_candidates").await, f.alert_count().await),
        (1, 1)
    );
    assert_eq!(f.count("alert_candidates WHERE next_at>created").await, 1);
    f.store
        .heartbeat(
            &f.node,
            &beat(presence(1.0, Some(0.0))),
            &canonical(&held).unwrap(),
        )
        .await
        .unwrap();
    assert_eq!(
        (f.count("alert_candidates").await, f.alert_count().await),
        (0, 2)
    );
    // Gateway Overmind reads have their own short class on every worker.
    assert_eq!(
        f.count("pg_indexes WHERE indexname='overmind_history'")
            .await,
        1
    );
    let read = gateway_op(&f.node.id, "overmind_read", "projects", json!({}));
    f.submit(&read).await.unwrap();
    let claimed = second
        .claim(&f.node, true)
        .await
        .unwrap()
        .unwrap()
        .decode()
        .unwrap();
    assert_eq!(claimed["requests"][0]["operation"], "overmind_read");
    assert!(claimed["requests"][0]["gateway_route"]["route_id"].is_null());
    let projects = json!({"state":"completed","result":{"ok":true,"projects":[]},"error":null});
    let encoded = canonical(&projects["result"]).unwrap();
    second
        .result(&f.node, s(&read, "request_id"), &projects, Some(&encoded))
        .await
        .unwrap();
    f.store
        .receipt(&f.phone, s(&read, "request_id"))
        .await
        .unwrap();
    f.store
        .db
        .exec(
            "UPDATE requests SET result_read=? WHERE id=?",
            args![now() - 6.0, &read["request_id"]],
        )
        .await
        .unwrap();
    second.maintain(false).await.unwrap();
    assert_eq!(f.count("requests WHERE operation='overmind_read'").await, 0);
    f.check_accounting().await;
    // A decision raised on one worker is delivered from the gateway digest.
    let task = Uuid::new_v4().to_string();
    let mut overmind = held.clone();
    overmind["mobile_overmind"] = json!({"schema":1,"rev":"1","projects":{},"decisions":[]});
    let away = presence(1.0, Some(0.0));
    let beat = |snapshot: &Value| json!({"snapshot":snapshot,"machine_id":heartbeat["machine_id"],"peers":[],"presence":away});
    second
        .heartbeat(&f.node, &beat(&overmind), &canonical(&overmind).unwrap())
        .await
        .unwrap();
    overmind["mobile_overmind"]["decisions"] =
        json!([{"task":task,"project":"example","title":"t","signature":signature('3')}]);
    f.store
        .heartbeat(&f.node, &beat(&overmind), &canonical(&overmind).unwrap())
        .await
        .unwrap();
    let alerts = f.events_of("alert").await;
    assert_eq!(
        (
            alerts.len(),
            &alerts[2]["alert_kind"],
            &alerts[2]["session"]
        ),
        (3, &json!("decision"), &json!(format!("overmind:{task}")))
    );
    assert_eq!(f.count("alert_candidates").await, 0);
    // Simulate losing only this fixture database's LISTEN sockets. Durable
    // commands must remain visible while listeners reconnect and wake waiters.
    let mut wake = second.db.wake.subscribe();
    let terminated: Vec<bool> = sqlx::query_scalar(
        "SELECT pg_terminate_backend(pid) FROM pg_stat_activity WHERE datname=$1 AND query LIKE 'LISTEN%'",
    ).bind(&name).fetch_all(&mut admin).await.unwrap();
    assert_eq!(terminated.len(), 2);
    assert!(terminated.iter().all(|v| *v));
    tokio::time::timeout(Duration::from_secs(3), wake.recv())
        .await
        .unwrap()
        .unwrap();
    let b = f.command("send");
    f.submit(&b).await.unwrap();
    assert!(second.claim(&f.node, true).await.unwrap().is_some());
    second.revoke(Role::Device, &f.phone.id).await.unwrap();
    assert!(f.store.authorized(&f.phone).await.is_err());
    f.check_accounting().await;
    second.db.close().await;
    f.close().await;
    sqlx::query(&format!("DROP DATABASE {name}"))
        .execute(&mut admin)
        .await
        .unwrap();
    admin.close().await.unwrap();
}

#[tokio::test]
#[ignore = "requires isolated ZERUS_RELAY_TEST_DATABASE_URL; run explicitly in CI"]
async fn postgres_cross_worker_single_alert() {
    use sqlx::Connection;
    let base =
        std::env::var("ZERUS_RELAY_TEST_DATABASE_URL").expect("isolated PostgreSQL required");
    let mut admin = sqlx::PgConnection::connect(&base).await.unwrap();
    let name = format!("relay_test_{}", Uuid::new_v4().simple());
    sqlx::query(&format!("CREATE DATABASE {name}"))
        .execute(&mut admin)
        .await
        .unwrap();
    let mut url = url::Url::parse(&base).unwrap();
    url.set_path(&name);
    let cfg = Config {
        database_url: Some(url.to_string()),
        background: false,
        ..Config::default()
    };
    let f = Fixture::config(cfg.clone()).await;
    let second = Store::new(
        Db::open(std::path::Path::new("unused"), Arc::new(cfg))
            .await
            .unwrap(),
    );
    f.store
        .register_push(&f.phone, "fcm", "synthetic-token")
        .await
        .unwrap();
    let active = presence(1.0, None);
    let question = |attention: &str, digit: char| {
        listing(session_row(
            "input",
            "busy",
            attention,
            Some(&signature(digit)),
            Value::Null,
        ))
    };
    f.beat_with(
        &listing(session_row("working", "busy", "q0", None, Value::Null)),
        &active,
    )
    .await;
    f.beat_with(&question("q1", '1'), &active).await;
    assert_eq!(f.count("alert_candidates").await, 1);
    // Both workers' maintenance passes race for one due candidate once presence is stale.
    f.store
        .db
        .exec("UPDATE nodes SET presence_at=presence_at-121", &[])
        .await
        .unwrap();
    f.store
        .db
        .exec("UPDATE alert_candidates SET next_at=0", &[])
        .await
        .unwrap();
    let (a, b) = tokio::join!(f.store.maintain(false), second.maintain(false));
    a.unwrap();
    b.unwrap();
    assert_eq!(f.alert_count().await, 1);
    // Presence-only heartbeats on two workers race for the next candidate.
    let waiting = question("q2", '2');
    f.beat_with(&waiting, &active).await;
    assert_eq!(f.count("alert_candidates").await, 1);
    let encoded = canonical(&waiting).unwrap();
    let away = |idle: f64| json!({"snapshot":waiting,"presence":presence(idle, None)});
    let (first, other) = (away(600.0), away(601.0));
    let (a, b) = tokio::join!(
        f.store.heartbeat(&f.node, &first, &encoded),
        second.heartbeat(&f.node, &other, &encoded)
    );
    a.unwrap();
    b.unwrap();
    assert_eq!(
        (f.alert_count().await, f.count("alert_candidates").await),
        (2, 0)
    );
    // Both alerts coalesce into the device's single pending wake.
    assert_eq!(f.count("push_jobs").await, 1);
    // An alert committed on one worker while the other sends the wake is sent again.
    let leased = second.claim_push_jobs().await.unwrap();
    assert_eq!(leased.len(), 1);
    let next = question("q3", '3');
    f.store
        .heartbeat(
            &f.node,
            &json!({"snapshot":next,"presence":presence(600.0, None)}),
            &canonical(&next).unwrap(),
        )
        .await
        .unwrap();
    let alerts = f.events_of("alert").await;
    assert_eq!(alerts.len(), 3);
    let reg = f
        .store
        .push_registration(&f.phone.id)
        .await
        .unwrap()
        .unwrap();
    second
        .finish_push(&leased[0], true, false, Some(&reg))
        .await
        .unwrap();
    let resent = f.store.claim_push_jobs().await.unwrap();
    assert_eq!(resent.len(), 1);
    assert_eq!(
        serde_json::from_str::<Value>(s(&resent[0], "payload")).unwrap(),
        json!({"event_id":alerts[2]["id"],"kind":"wake"})
    );
    second.db.close().await;
    f.close().await;
    sqlx::query(&format!("DROP DATABASE {name}"))
        .execute(&mut admin)
        .await
        .unwrap();
    admin.close().await.unwrap();
}

#[test]
fn python_float_and_unicode_differential() {
    for raw in [
        r#"{"$serde_json::private::Number":"123"}"#,
        r#"{"\u0024serde_json::private::Number":"123"}"#,
    ] {
        let v = zerus_relay::json::parse(raw.as_bytes(), 32, 50000).unwrap();
        assert!(v.is_object());
        assert_eq!(v["$serde_json::private::Number"], "123");
        assert_eq!(
            canonical(&v).unwrap(),
            r#"{"$serde_json::private::Number":"123"}"#
        );
    }
    // An independent serializer oracle exercises Python's exponent thresholds,
    // arbitrary integers, non-BMP escaping and randomized IEEE-754 values.
    let output=std::process::Command::new("python3").args(["-c",r#"
import json,random,struct,math
rng=random.Random(7391)
values=[0.0,-0.0,1e-4,1e-5,1e15,1e16,1e20,1e23,5e-324,2**80,-2**80,'😀é\u007f','\\/\n\t']
for _ in range(20000):
    f=struct.unpack('>d',rng.randbytes(8))[0]
    if math.isfinite(f):values.append(f)
for v in values:
    print(json.dumps([v,json.dumps(v,sort_keys=True,separators=(',',':'),allow_nan=False)],ensure_ascii=True))
"#]).output().unwrap();
    assert!(output.status.success());
    for line in String::from_utf8(output.stdout).unwrap().lines() {
        let pair: Value = serde_json::from_str(line).unwrap();
        assert_eq!(
            canonical(&pair[0]).unwrap(),
            pair[1].as_str().unwrap(),
            "{}",
            pair[0]
        );
    }
}

#[tokio::test]
async fn revoked_root_retains_only_authorized_peers() {
    let f = Fixture::new().await;
    let root = Uuid::new_v4().to_string();
    let peer = Uuid::new_v4().to_string();
    let route = Uuid::new_v4().to_string();
    let snap = json!({"sessions":[],"mobile_capabilities":{"protocol_version":1,"operations":protocol::OPERATIONS,"features":["gateway_one_hop"]}});
    let hb = json!({"snapshot":snap,"machine_id":root,"peers":[{"route_id":route,"machine_id":peer,"name":"Remote","online":true}]});
    let encoded = canonical(&snap).unwrap();
    f.store.heartbeat(&f.node, &hb, &encoded).await.unwrap();
    f.store
        .peer_heartbeat(
            &f.node,
            &route,
            &json!({"machine_id":peer,"snapshot":snap}),
            &encoded,
        )
        .await
        .unwrap();
    f.store.revoke_computer(&f.node.id).await.unwrap();
    f.store.heartbeat(&f.node, &hb, &encoded).await.unwrap();
    f.store.authorized(&f.node).await.unwrap();
    let catalog = f.store.computers(&f.phone).await.unwrap();
    assert_eq!(catalog["computers"].as_array().unwrap().len(), 1);
    assert_eq!(catalog["computers"][0]["machine_id"], peer);
    let mut b = f.command("send");
    assert_eq!(f.submit(&b).await.unwrap_err().0.as_u16(), 409);
    b["computer_id"] = catalog["computers"][0]["id"].clone();
    f.submit(&b).await.unwrap();
    assert!(f.store.claim(&f.node, true).await.unwrap().is_some());
    f.close().await;
}

#[tokio::test]
async fn http_rejects_bad_content_before_mutation_and_recovers_from_disconnect() {
    let f = Fixture::new().await;
    let app = App::new(f.store.clone()).await.unwrap();
    let listener = tokio::net::TcpListener::bind("127.0.0.1:0").await.unwrap();
    let address = listener.local_addr().unwrap();
    let url = format!("http://{address}");
    let stop = CancellationToken::new();
    let server = tokio::spawn(serve(listener, app, stop.clone()));
    let client = reqwest::Client::new();
    let auth = s(&f.paired, "device_token");
    let b = f.command("send");
    let endpoint = format!("{url}/v1/requests");
    assert_eq!(
        client
            .post(&endpoint)
            .bearer_auth(auth)
            .header("content-encoding", "gzip")
            .json(&b)
            .send()
            .await
            .unwrap()
            .status(),
        415
    );
    assert_eq!(
        client
            .post(&endpoint)
            .bearer_auth(auth)
            .header("content-type", "application/json")
            .body("{".repeat(100))
            .send()
            .await
            .unwrap()
            .status(),
        413
    );
    // Abort an unknown-length upload while it owns the sole large payload lane.
    use tokio::io::AsyncWriteExt;
    let mut stream = tokio::net::TcpStream::connect(address).await.unwrap();
    stream.write_all(format!("POST /v1/requests HTTP/1.1\r\nHost: localhost\r\nAuthorization: Bearer {auth}\r\nContent-Type: application/json\r\nTransfer-Encoding: chunked\r\n\r\n1\r\n{{\r\n").as_bytes()).await.unwrap();
    tokio::time::sleep(Duration::from_millis(25)).await;
    // A second client is already streaming when admission is refused. It must
    // receive a usable overload response instead of losing it to a TCP reset.
    let overloaded = client
        .post(&endpoint)
        .bearer_auth(auth)
        .header("content-type", "application/json")
        .body(" ".repeat(2 * 1024 * 1024))
        .send()
        .await
        .unwrap();
    assert_eq!(overloaded.status(), 429);
    assert_eq!(overloaded.headers()["retry-after"], "1");
    assert_eq!(
        client
            .get(format!("{url}/healthz"))
            .send()
            .await
            .unwrap()
            .status(),
        200
    );
    drop(stream);
    let mut result = None;
    for _ in 0..20 {
        let r = client
            .post(&endpoint)
            .bearer_auth(auth)
            .json(&b)
            .send()
            .await
            .unwrap();
        if r.status() != 429 {
            result = Some(r.status());
            break;
        }
        tokio::time::sleep(Duration::from_millis(10)).await;
    }
    assert_eq!(result, Some(reqwest::StatusCode::ACCEPTED));
    stop.cancel();
    server.await.unwrap().unwrap();
    f.close().await;
}

#[tokio::test]
async fn mark_read_payload_is_exact_and_capability_gated() {
    let f = Fixture::new().await;
    let id = Uuid::new_v4().to_string();
    let payload = json!({"run_id":"run","conversation_id":"conversation","reply_id":"7:100.5","attention_signature":"a".repeat(64)});
    let body = json!({"request_id":id,"computer_id":f.node.id,"operation":"mark_read","session":"codex/example/tag","payload":payload});
    // A computer that does not advertise the operation keeps rejecting it.
    assert_eq!(f.submit(&body).await.unwrap_err().0.as_u16(), 409);
    let snapshot = json!({"sessions":[],"mobile_capabilities":{"protocol_version":1,"operations":["mark_read"],"features":[]}});
    f.store
        .heartbeat(
            &f.node,
            &json!({"snapshot":snapshot}),
            &canonical(&snapshot).unwrap(),
        )
        .await
        .unwrap();
    assert_eq!(f.submit(&body).await.unwrap()["state"], "queued");
    let changes = [
        json!({"reply_id":7}),
        json!({"attention_signature":"A".repeat(64)}),
        json!({"attention_signature":"a".repeat(63)}),
        json!({"run_id":""}),
        json!({"reply_id":"bad\u{7}"}),
        json!({"request_id":id}),
    ];
    for change in changes {
        let mut invalid = body.clone();
        invalid["request_id"] = json!(Uuid::new_v4().to_string());
        for (key, value) in change.as_object().unwrap() {
            invalid["payload"][key] = value.clone();
        }
        assert!(zerus_relay::store::Submission::new(invalid).is_err());
    }
    let mut missing = body.clone();
    missing["payload"]
        .as_object_mut()
        .unwrap()
        .remove("reply_id");
    assert!(zerus_relay::store::Submission::new(missing).is_err());
    let mut nulls = body.clone();
    nulls["request_id"] = json!(Uuid::new_v4().to_string());
    nulls["payload"]["reply_id"] = Value::Null;
    nulls["payload"]["attention_signature"] = Value::Null;
    assert!(zerus_relay::store::Submission::new(nulls).is_ok());
    assert!(protocol::capabilities(vec![])["operations"]
        .as_array()
        .unwrap()
        .iter()
        .any(|v| v == "mark_read"));
    f.close().await;
}

fn attachment_read(f: &Fixture) -> Value {
    let id = Uuid::new_v4().to_string();
    json!({"request_id":id,"computer_id":f.node.id,"operation":"attachment_read","session":"codex/example/tag",
        "payload":{"request_id":id,"expected_run_id":"run","expected_conversation_id":"conversation",
            "message_id":"00000000-0000-4000-8000-000000000001","index":0,"offset":0,"length":524288}})
}

#[tokio::test]
async fn attachment_read_is_exact_gated_and_short_lived() {
    assert!(protocol::READS.contains(&"attachment_read"));
    assert!(!protocol::READ_SQL.contains("attachment_read"));
    assert!(protocol::capabilities(vec![])["operations"]
        .as_array()
        .unwrap()
        .iter()
        .any(|v| v == "attachment_read"));
    let f = Fixture::new().await;
    let b = attachment_read(&f);
    assert_eq!(f.submit(&b).await.unwrap_err().0.as_u16(), 409);
    f.beat(&json!({"sessions":[],"mobile_capabilities":{"protocol_version":1,"operations":["attachment_read"],"features":[]}}))
        .await;
    for (field, value) in [
        ("message_id", Value::Null),
        ("message_id", json!("00000000-0000-4000-8000-00000000000A")),
        ("message_id", json!("not-a-uuid")),
        ("index", json!(8)),
        ("index", json!(-1)),
        ("index", json!(0.0)),
        ("index", json!("0")),
        ("offset", json!(10 * 1024 * 1024)),
        ("offset", json!(10 * 1024 * 1024 - 10)),
        ("length", json!(0)),
        ("length", json!(524289)),
        ("length", Value::Null),
        ("expected_conversation_id", json!("")),
        ("expected_run_id", json!("")),
        ("archive_id", json!("00000000-0000-0000-0000-000000000000")),
        ("agent_id", json!("../child")),
        ("path", json!("/private/file.png")),
        ("text", json!("hello")),
    ] {
        let mut invalid = b.clone();
        invalid["request_id"] = json!(Uuid::new_v4().to_string());
        invalid["payload"]["request_id"] = invalid["request_id"].clone();
        if value.is_null() {
            invalid["payload"].as_object_mut().unwrap().remove(field);
        } else {
            invalid["payload"][field] = value;
        }
        assert!(
            matches!(zerus_relay::store::Submission::new(invalid), Err(e) if e.0.as_u16() == 400),
            "{field}"
        );
    }
    let mut child = attachment_read(&f);
    child["payload"]["agent_id"] = json!("worker-1");
    child["payload"]["archive_id"] = json!(Uuid::new_v4().to_string());
    assert!(zerus_relay::store::Submission::new(child).is_ok());
    // A completed maximum chunk fits the ordinary result bound; once the phone
    // has read it, the bytes leave the relay after a short grace period.
    assert_eq!(f.submit(&b).await.unwrap()["state"], "queued");
    assert!(f.store.claim(&f.node, false).await.unwrap().is_some());
    let data = "A".repeat(699_052);
    let result = json!({"state":"completed","result":{"request_id":b["request_id"],"name":"codex/example/tag","file_name":"Скриншот 😀.png","data_base64":data},"error":null});
    let e = canonical(&result["result"]).unwrap();
    f.store
        .result(&f.node, s(&b, "request_id"), &result, Some(&e))
        .await
        .unwrap();
    f.check_accounting().await;
    let receipt = f
        .store
        .receipt(&f.phone, s(&b, "request_id"))
        .await
        .unwrap();
    assert_eq!(
        receipt["result"]["data_base64"].as_str().unwrap().len(),
        699_052
    );
    f.store.maintain(false).await.unwrap();
    assert_eq!(
        f.count("requests WHERE operation='attachment_read'").await,
        1
    );
    f.store
        .db
        .exec(
            "UPDATE requests SET result_read=? WHERE id=?",
            args![now() - 6.0, &b["request_id"]],
        )
        .await
        .unwrap();
    f.store.maintain(false).await.unwrap();
    assert_eq!(
        f.count("requests WHERE operation='attachment_read'").await,
        0
    );
    f.check_accounting().await;
    // An unread result is removed after its TTL; a pruned read identity is new again.
    let unread = attachment_read(&f);
    assert_eq!(f.submit(&unread).await.unwrap()["state"], "queued");
    assert!(f.store.claim(&f.node, false).await.unwrap().is_some());
    let failed =
        json!({"state":"failed","result":null,"error":"attachment is no longer available"});
    f.store
        .result(&f.node, s(&unread, "request_id"), &failed, None)
        .await
        .unwrap();
    f.store
        .db
        .exec(
            "UPDATE requests SET updated=? WHERE id=?",
            args![now() - 59.0, &unread["request_id"]],
        )
        .await
        .unwrap();
    f.store.maintain(false).await.unwrap();
    assert_eq!(
        f.count("requests WHERE operation='attachment_read'").await,
        1
    );
    f.store
        .db
        .exec(
            "UPDATE requests SET updated=? WHERE id=?",
            args![now() - 61.0, &unread["request_id"]],
        )
        .await
        .unwrap();
    f.store.maintain(false).await.unwrap();
    assert_eq!(
        f.count("requests WHERE operation='attachment_read'").await,
        0
    );
    f.check_accounting().await;
    assert_eq!(f.submit(&unread).await.unwrap()["state"], "queued");
    f.close().await;
}

#[tokio::test]
async fn mobile_delivery_is_computer_scoped_and_capability_gated() {
    let f = Fixture::new().await;
    let id = Uuid::new_v4().to_string();
    let b = json!({"request_id":id,"computer_id":f.node.id,"operation":"set_mobile_delivery","session":"","payload":{"value":"away_10"}});
    assert_eq!(f.submit(&b).await.unwrap_err().0.as_u16(), 409);
    let snapshot = json!({"sessions":[],"mobile_capabilities":{"protocol_version":1,"operations":["set_mobile_delivery"],"features":[]}});
    f.store
        .heartbeat(
            &f.node,
            &json!({"snapshot":snapshot}),
            &canonical(&snapshot).unwrap(),
        )
        .await
        .unwrap();
    assert_eq!(f.submit(&b).await.unwrap()["state"], "queued");
    assert!(protocol::OPERATIONS.contains(&"set_mobile_delivery"));
    for (field, value) in [
        ("session", json!("codex/example/tag")),
        ("payload", json!({"value":"later"})),
        ("payload", json!({})),
        ("payload", json!({"value":"away","request_id":id})),
    ] {
        let mut invalid = b.clone();
        invalid["request_id"] = json!(Uuid::new_v4().to_string());
        invalid[field] = value;
        assert!(matches!(
            zerus_relay::store::Submission::new(invalid),
            Err(e) if e.0.as_u16() == 400
        ));
    }
    f.close().await;
}

/// A gateway-scoped Overmind envelope; writes repeat the request identity.
fn gateway_op(computer: &str, op: &str, action: &str, request: Value) -> Value {
    let id = Uuid::new_v4().to_string();
    let mut payload = json!({"action":action,"request":request});
    if op == "overmind_write" {
        payload["request_id"] = json!(id);
    }
    json!({"request_id":id,"computer_id":computer,"operation":op,"session":"","payload":payload})
}
/// 0 when the envelope is accepted, otherwise the refusal status.
fn refusal(body: Value) -> u16 {
    zerus_relay::store::Submission::new(body).map_or_else(|e| e.0.as_u16(), |_| 0)
}
fn advertising(operations: &[&str]) -> Value {
    json!({"sessions":[],"mobile_capabilities":{"protocol_version":1,"operations":operations,"features":[]}})
}

#[test]
fn overmind_envelopes_are_exact() {
    for op in ["overmind_read", "overmind_write"] {
        assert!(protocol::OPERATIONS.contains(&op) && protocol::GATEWAY.contains(&op));
        assert!(protocol::capabilities(vec![])["operations"]
            .as_array()
            .unwrap()
            .iter()
            .any(|v| v == op));
    }
    assert!(protocol::READS.contains(&"overmind_read"));
    assert!(!protocol::READS.contains(&"overmind_write"));
    assert!(!protocol::READ_SQL.contains("overmind"));
    let c = Uuid::new_v4().to_string();
    let read = |action: &str, request: Value| gateway_op(&c, "overmind_read", action, request);
    let write = |action: &str, request: Value| gateway_op(&c, "overmind_write", action, request);
    let range = json!({"task":"OM-1","sha256":"a".repeat(64),"offset":0,"length":524288});
    // Every allowed action with any object request; hgs validates the fields.
    for action in ["projects", "board", "task", "decisions", "attachment"] {
        let request = if action == "attachment" {
            range.clone()
        } else {
            json!({"project":"example","since":"12"})
        };
        assert_eq!(refusal(read(action, request)), 0, "{action}");
    }
    for action in [
        "create",
        "edit",
        "move",
        "reorder",
        "pin",
        "unpin",
        "label",
        "link",
        "comment",
        "send",
        "review-send",
        "rework-send",
        "answer",
        "resolve-delivery",
        "done",
        "reopen",
        "delete",
    ] {
        assert_eq!(refusal(write(action, json!({}))), 0, "{action}");
    }
    // The informational device label follows hgs `--device`: 1–64 characters,
    // any script, no control characters, not starting with `-` or `@`.
    for label in ["Ü".repeat(64), "Pixel 9 @home - work".into(), "x".into()] {
        let mut labelled = write(
            "create",
            json!({"project":"example","title":"Pick a database"}),
        );
        labelled["payload"]["device"] = json!(label);
        assert_eq!(refusal(labelled), 0, "{label}");
    }
    // Writes carry inline images up to the request maximum, beyond the 1 MiB bound of other operations.
    let image = json!({"images":[{"data_base64":"A".repeat(2 * protocol::MIB)}]});
    assert_eq!(refusal(write("create", image)), 0);
    let huge = json!({"text":"A".repeat(protocol::REQUEST_MAX)});
    assert_eq!(refusal(write("create", huge)), 413);
    // Reads stay small.
    assert_eq!(
        refusal(read("board", json!({"since":"1".repeat(65000)}))),
        0
    );
    assert_eq!(
        refusal(read("board", json!({"since":"1".repeat(65536)}))),
        413
    );
    // `None` removes the field at the path.
    let changed = |mut body: Value, path: &str, value: Option<Value>| {
        let keys: Vec<&str> = path.split('/').collect();
        let (last, parents) = keys.split_last().unwrap();
        let mut target = &mut body;
        for key in parents {
            target = &mut target[*key];
        }
        match value {
            Some(v) => target[*last] = v,
            None => {
                target.as_object_mut().unwrap().remove(*last);
            }
        }
        body
    };
    let mut denied = vec![];
    for (base, other) in [
        (read("board", json!({})), "create"),
        (write("create", json!({})), "board"),
    ] {
        for (path, value) in [
            ("session", Some(json!("codex/example/tag"))),
            ("payload/action", Some(json!(other))),
            ("payload/action", Some(json!("sessions"))),
            ("payload/action", Some(json!("show"))),
            ("payload/action", Some(json!("advance"))),
            ("payload/action", Some(json!("call-human"))),
            ("payload/action", Some(json!("rev"))),
            ("payload/action", Some(json!(""))),
            ("payload/action", Some(json!(7))),
            ("payload/action", None),
            ("payload/request", Some(json!([]))),
            ("payload/request", Some(json!("{}"))),
            ("payload/request", Some(Value::Null)),
            ("payload/request", None),
            ("payload/expected_run_id", Some(json!("run"))),
            ("payload/device", Some(json!(""))),
            ("payload/device", Some(json!("d".repeat(65)))),
            ("payload/device", Some(json!("phone\nlabel"))),
            ("payload/device", Some(json!("phone\u{7f}"))),
            ("payload/device", Some(json!("phone\u{80}"))),
            ("payload/device", Some(json!("phone\u{9f}label"))),
            ("payload/device", Some(json!("-phone"))),
            ("payload/device", Some(json!("--json"))),
            ("payload/device", Some(json!("@peer"))),
            ("payload/device", Some(json!(7))),
        ] {
            denied.push((path, value, base.clone()));
        }
    }
    // Only a write repeats the envelope identity, exactly.
    let stray = read("board", json!({}));
    let id = stray["request_id"].clone();
    denied.push(("payload/request_id", Some(id), stray));
    let other = json!(Uuid::new_v4().to_string());
    denied.push((
        "payload/request_id",
        Some(other),
        write("create", json!({})),
    ));
    denied.push(("payload/request_id", None, write("create", json!({}))));
    // Attachment ranges follow `attachment_read`.
    for (path, value) in [
        ("payload/request/offset", None),
        ("payload/request/length", None),
        ("payload/request/length", Some(json!(0))),
        ("payload/request/length", Some(json!(524289))),
        ("payload/request/length", Some(json!(1.5))),
        ("payload/request/length", Some(json!("1"))),
        ("payload/request/offset", Some(json!(-1))),
        ("payload/request/offset", Some(json!(10 * 1024 * 1024))),
        ("payload/request/offset", Some(json!(10 * 1024 * 1024 - 10))),
    ] {
        denied.push((path, value, read("attachment", range.clone())));
    }
    for (path, value, base) in denied {
        let name = format!("{} {path} {value:?}", base["operation"]);
        assert_eq!(refusal(changed(base, path, value)), 400, "{name}");
    }
}

#[tokio::test]
async fn overmind_ops_run_only_on_the_advertising_gateway() {
    let f = Fixture::new().await;
    let read = gateway_op(&f.node.id, "overmind_read", "projects", json!({}));
    let write = gateway_op(
        &f.node.id,
        "overmind_write",
        "create",
        json!({"project":"example","title":"Pick a database"}),
    );
    // A gateway that does not advertise an operation keeps rejecting it.
    assert_eq!(f.submit(&read).await.unwrap_err().0.as_u16(), 409);
    assert_eq!(f.submit(&write).await.unwrap_err().0.as_u16(), 409);
    f.beat(&advertising(&["overmind_read"])).await;
    assert_eq!(f.submit(&read).await.unwrap()["state"], "queued");
    assert_eq!(f.submit(&write).await.unwrap_err().0.as_u16(), 409);
    f.beat(&advertising(&["overmind_read", "overmind_write"]))
        .await;
    assert_eq!(f.submit(&write).await.unwrap()["state"], "queued");
    // The gateway's own connector receives them, with no session and no route.
    for expected in [&read, &write] {
        let claim = f
            .store
            .claim(&f.node, false)
            .await
            .unwrap()
            .unwrap()
            .decode()
            .unwrap();
        let r = &claim["requests"][0];
        assert_eq!(
            (
                &r["request_id"],
                &r["operation"],
                &r["session"],
                &r["payload"]
            ),
            (
                &expected["request_id"],
                &expected["operation"],
                &json!(""),
                &expected["payload"]
            )
        );
        assert!(r.get("gateway_route").is_none());
    }
    // Behind a one-hop gateway a peer is never a target, even when it advertises them.
    let (root, machine, route) = (
        Uuid::new_v4().to_string(),
        Uuid::new_v4().to_string(),
        Uuid::new_v4().to_string(),
    );
    let mut caps = advertising(protocol::OPERATIONS);
    caps["mobile_capabilities"]["features"] = json!(["gateway_one_hop"]);
    let peers = json!([{"route_id":route,"machine_id":machine,"name":"Peer","online":true}]);
    gateway_beat(&f, &root, &peers, &caps).await;
    peer_beat(&f, &route, &machine, &caps, &Value::Null).await;
    let catalog = f.store.computers(&f.phone).await.unwrap();
    let peer = catalog["computers"]
        .as_array()
        .unwrap()
        .iter()
        .find(|r| r["machine_id"] == machine)
        .unwrap()["id"]
        .as_str()
        .unwrap()
        .to_owned();
    for op in ["overmind_read", "overmind_write"] {
        let action = if op == "overmind_read" {
            "projects"
        } else {
            "create"
        };
        let b = gateway_op(&peer, op, action, json!({}));
        assert_eq!(f.submit(&b).await.unwrap_err().0.as_u16(), 409, "{op}");
        // The ordinary session operations still reach the peer.
        let mut send = f.command("send");
        send["computer_id"] = json!(peer);
        assert_eq!(f.submit(&send).await.unwrap()["state"], "queued");
        assert!(f.store.claim(&f.node, true).await.unwrap().is_some());
    }
    // The gateway computer's local route serves them; its claim names no peer route.
    let local = gateway_op(&f.node.id, "overmind_read", "decisions", json!({}));
    assert_eq!(f.submit(&local).await.unwrap()["state"], "queued");
    let claim = f
        .store
        .claim(&f.node, true)
        .await
        .unwrap()
        .unwrap()
        .decode()
        .unwrap();
    let r = &claim["requests"][0];
    assert_eq!(r["request_id"], local["request_id"]);
    assert!(r["gateway_route"]["route_id"].is_null());
    assert_eq!(r["gateway_route"]["machine_id"], root);
    // Peer snapshots never raise decisions; only the gateway publishes the digest.
    let digest = |items: Value| {
        let mut s = caps.clone();
        s["mobile_overmind"] = json!({"schema":1,"rev":"1","projects":{},"decisions":items});
        s
    };
    let item = json!([{"task":Uuid::new_v4().to_string(),"project":"example","title":"t","signature":signature('1')}]);
    peer_beat(&f, &route, &machine, &digest(json!([])), &Value::Null).await;
    peer_beat(&f, &route, &machine, &digest(item), &Value::Null).await;
    assert!(f.events_of("decision").await.is_empty());
    f.check_accounting().await;
    f.close().await;
}

#[tokio::test]
async fn overmind_ops_refuse_a_gateway_reached_only_through_a_peer_route() {
    let f = Fixture::new().await;
    // A second gateway is also a direct peer of the first one.
    let other = f
        .store
        .node(&f.node.workspace, "Other computer")
        .await
        .unwrap();
    let other = f
        .store
        .authenticate(s(&other, "node_token"), Role::Node)
        .await
        .unwrap();
    let (root, machine, route) = (
        Uuid::new_v4().to_string(),
        Uuid::new_v4().to_string(),
        Uuid::new_v4().to_string(),
    );
    let mut caps = advertising(protocol::OPERATIONS);
    caps["mobile_capabilities"]["features"] = json!(["gateway_one_hop"]);
    let own = json!({"snapshot":caps,"machine_id":machine,"peers":[]});
    f.store
        .heartbeat(&other, &own, &canonical(&caps).unwrap())
        .await
        .unwrap();
    let peers = json!([{"route_id":route,"machine_id":machine,"name":"Other","online":true}]);
    gateway_beat(&f, &root, &peers, &caps).await;
    peer_beat(&f, &route, &machine, &caps, &Value::Null).await;
    // While its own route is live, the other gateway's connector serves it.
    let op = || gateway_op(&other.id, "overmind_read", "projects", json!({}));
    let served = op();
    assert_eq!(f.submit(&served).await.unwrap()["state"], "queued");
    let claim = f
        .store
        .claim(&other, true)
        .await
        .unwrap()
        .unwrap()
        .decode()
        .unwrap();
    assert_eq!(claim["requests"][0]["request_id"], served["request_id"]);
    assert!(claim["requests"][0]["gateway_route"]["route_id"].is_null());
    // Its own route goes stale; the first gateway still reaches it one hop away.
    f.store
        .db
        .exec(
            "UPDATE nodes SET last_seen=? WHERE id=?",
            args![now() - 100.0, &other.id],
        )
        .await
        .unwrap();
    let mut send = f.command("send");
    send["computer_id"] = json!(other.id);
    assert_eq!(f.submit(&send).await.unwrap()["state"], "queued");
    let relayed = f
        .store
        .claim(&f.node, true)
        .await
        .unwrap()
        .unwrap()
        .decode()
        .unwrap();
    assert_eq!(relayed["requests"][0]["gateway_route"]["route_id"], route);
    // A gateway operation is refused rather than run on the first gateway's connector.
    for op in [
        op(),
        gateway_op(&other.id, "overmind_write", "create", json!({})),
    ] {
        assert_eq!(f.submit(&op).await.unwrap_err().0.as_u16(), 409);
    }
    assert!(f.store.claim(&f.node, true).await.unwrap().is_none());
    assert!(f.store.claim(&other, true).await.unwrap().is_none());
    f.check_accounting().await;
    f.close().await;
}

#[tokio::test]
async fn overmind_reads_are_short_lived_and_writes_keep_their_identity() {
    let f = Fixture::new().await;
    assert_eq!(
        f.count("sqlite_master WHERE type='index' AND name='overmind_history'")
            .await,
        1
    );
    f.beat(&advertising(&["overmind_read", "overmind_write"]))
        .await;
    let usage = |f: &Fixture| {
        let w = f.phone.workspace.clone();
        let db = f.store.db.clone();
        async move {
            let r = db
                .one(
                    "SELECT reads,mutations FROM workspace_usage WHERE workspace_id=?",
                    args![&w],
                )
                .await
                .unwrap()
                .unwrap();
            (i(&r, "reads"), i(&r, "mutations"))
        }
    };
    let complete = |b: &Value, result: Value| {
        let id = s(b, "request_id").to_owned();
        let store = f.store.clone();
        let node = f.node.clone();
        async move {
            assert!(store.claim(&node, false).await.unwrap().is_some());
            let body = json!({"state":"completed","result":result,"error":null});
            let e = canonical(&body["result"]).unwrap();
            store.result(&node, &id, &body, Some(&e)).await.unwrap();
        }
    };
    let rows = "requests WHERE operation='overmind_read'";
    // A read result near the 1 MiB bound leaves soon after the phone read it.
    let read = gateway_op(
        &f.node.id,
        "overmind_read",
        "board",
        json!({"project":"example"}),
    );
    assert_eq!(f.submit(&read).await.unwrap()["state"], "queued");
    assert_eq!(usage(&f).await, (1, 0));
    complete(
        &read,
        json!({"ok":true,"tasks":[],"page":"A".repeat(1_000_000)}),
    )
    .await;
    f.check_accounting().await;
    let receipt = f
        .store
        .receipt(&f.phone, s(&read, "request_id"))
        .await
        .unwrap();
    assert_eq!(receipt["result"]["page"].as_str().unwrap().len(), 1_000_000);
    assert_eq!(
        f.count(&format!("{rows} AND result_read IS NOT NULL"))
            .await,
        1
    );
    f.store.maintain(false).await.unwrap();
    assert_eq!(f.count(rows).await, 1);
    f.store
        .db
        .exec(
            "UPDATE requests SET result_read=? WHERE id=?",
            args![now() - 6.0, &read["request_id"]],
        )
        .await
        .unwrap();
    f.store.maintain(false).await.unwrap();
    assert_eq!(f.count(rows).await, 0);
    assert_eq!(usage(&f).await, (0, 0));
    f.check_accounting().await;
    // An unread result leaves after its TTL; a pruned read identity is new again.
    let unread = gateway_op(&f.node.id, "overmind_read", "projects", json!({}));
    f.submit(&unread).await.unwrap();
    complete(&unread, json!({"ok":true,"projects":[]})).await;
    for (age, left) in [(59.0, 1), (61.0, 0)] {
        f.store
            .db
            .exec(
                "UPDATE requests SET updated=? WHERE id=?",
                args![now() - age, &unread["request_id"]],
            )
            .await
            .unwrap();
        f.store.maintain(false).await.unwrap();
        assert_eq!(f.count(rows).await, left, "{age}");
    }
    assert_eq!(f.submit(&unread).await.unwrap()["state"], "queued");
    f.check_accounting().await;
    assert!(f.store.claim(&f.node, false).await.unwrap().is_some());
    // A write is a mutation: it outlives the read classes, and a retry after its
    // history expired still returns the completed receipt instead of running again.
    let write = gateway_op(
        &f.node.id,
        "overmind_write",
        "create",
        json!({"project":"example","title":"Pick a database"}),
    );
    assert_eq!(f.submit(&write).await.unwrap()["state"], "queued");
    assert_eq!(usage(&f).await.1, 1);
    // The same request identity never carries another task: a different
    // request, action or device label conflicts while queued, after completion
    // and after the body expired.
    let conflicting = || {
        let mut changed = vec![write.clone(), write.clone(), write.clone()];
        changed[0]["payload"]["request"]["title"] = json!("Pick another database");
        changed[1]["payload"]["device"] = json!("Pixel 9");
        changed[2]["payload"]["action"] = json!("edit");
        changed
    };
    for changed in conflicting() {
        assert_eq!(f.submit(&changed).await.unwrap_err().0.as_u16(), 409);
    }
    complete(
        &write,
        json!({"ok":true,"task":{"uuid":write["request_id"]}}),
    )
    .await;
    f.store
        .receipt(&f.phone, s(&write, "request_id"))
        .await
        .unwrap();
    for age in [3601.0, 8.0 * 86400.0] {
        f.store
            .db
            .exec(
                "UPDATE requests SET updated=? WHERE id=?",
                args![now() - age, &write["request_id"]],
            )
            .await
            .unwrap();
        f.store.maintain(false).await.unwrap();
        assert_eq!(
            f.submit(&write).await.unwrap()["state"],
            "completed",
            "{age}"
        );
        for changed in conflicting() {
            assert_eq!(
                f.submit(&changed).await.unwrap_err().0.as_u16(),
                409,
                "{age}"
            );
        }
    }
    assert_eq!(usage(&f).await.1, 1);
    assert!(f.store.claim(&f.node, false).await.unwrap().is_none());
    f.check_accounting().await;
    f.close().await;
}

#[tokio::test]
async fn decision_alerts_are_held_and_delivered_from_the_gateway_digest() {
    let f = Fixture::new().await;
    f.store
        .register_push(&f.phone, "fcm", "synthetic-token")
        .await
        .unwrap();
    let active = presence(1.0, None);
    let task = Uuid::new_v4().to_string();
    let slot = format!("overmind:{task}");
    let digest = |items: Value| json!({"sessions":[],"mobile_overmind":{"schema":1,"rev":"1","projects":{},"decisions":items}});
    let waiting = |sig: char| {
        digest(
            json!([{"task":task,"project":"example","title":"Pick a database","signature":signature(sig)}]),
        )
    };
    let held = || f.count("alert_candidates WHERE kind='decision'");
    f.beat_with(&digest(json!([])), &active).await;
    // Entering Your Call refreshes phones and holds the alert while the owner is active.
    f.beat_with(&waiting('1'), &active).await;
    let raised = f.events_of("decision").await;
    assert_eq!(raised.len(), 1);
    assert_eq!(
        (
            raised[0]["session"].as_str(),
            raised[0]["computer_id"].as_str()
        ),
        (Some(slot.as_str()), Some(f.node.id.as_str()))
    );
    assert!(raised[0].get("alert_kind").is_none());
    assert_eq!((held().await, f.alert_count().await), (1, 0));
    // The task leaves Your Call: the held alert resolves.
    f.beat_with(&digest(json!([])), &active).await;
    assert_eq!(held().await, 0);
    // It returns, then changes while waiting: each raises it again, one slot.
    f.beat_with(&waiting('1'), &active).await;
    f.beat_with(&waiting('2'), &active).await;
    assert_eq!(f.events_of("decision").await.len(), 3);
    assert_eq!(
        f.count(&format!(
            "alert_candidates WHERE session='{slot}' AND attention_signature='{}'",
            signature('2')
        ))
        .await,
        1
    );
    // A snapshot without the digest is missing data: nothing resolves or repeats.
    f.beat_with(&json!({"sessions":[]}), &active).await;
    f.beat_with(&waiting('2'), &active).await;
    assert_eq!(f.events_of("decision").await.len(), 3);
    assert_eq!((held().await, f.alert_count().await), (1, 0));
    // The lock alone delivers it under the ordinary presence rule.
    f.beat_with(&waiting('2'), &presence(2.0, Some(1.0))).await;
    let alerts = f.events_of("alert").await;
    assert_eq!(alerts.len(), 1);
    assert_eq!(
        (
            alerts[0]["alert_kind"].as_str(),
            alerts[0]["session"].as_str(),
            alerts[0]["computer_id"].as_str()
        ),
        (
            Some("decision"),
            Some(slot.as_str()),
            Some(f.node.id.as_str())
        )
    );
    assert_eq!((held().await, f.count("push_jobs").await), (0, 1));
    // Wakes stay generic: push payloads never carry task content.
    let jobs = f.store.claim_push_jobs().await.unwrap();
    assert_eq!(
        serde_json::from_str::<Value>(s(&jobs[0], "payload")).unwrap(),
        json!({"event_id":alerts[0]["id"],"kind":"wake"})
    );
    f.close().await;
}

#[tokio::test]
async fn decision_candidates_drop_when_the_gateway_stops_serving_overmind() {
    let f = Fixture::new().await;
    let active = presence(1.0, None);
    let snapshot = |operations: &[&str], decisions: Option<Value>| {
        let mut s = advertising(operations);
        if let Some(items) = decisions {
            s["mobile_overmind"] = json!({"schema":1,"rev":"1","projects":{},"decisions":items});
        }
        s
    };
    let serving = ["overmind_read", "overmind_write"];
    let waiting = json!([{"task":Uuid::new_v4().to_string(),"project":"example","title":"t","signature":signature('1')}]);
    f.beat_with(&snapshot(&serving, Some(json!([]))), &active)
        .await;
    f.beat_with(&snapshot(&serving, Some(waiting.clone())), &active)
        .await;
    assert_eq!(f.count("alert_candidates WHERE kind='decision'").await, 1);
    // An older connector (or hgs) no longer serves Overmind and drops the
    // digest: the held decision resolves instead of waiting for a digest.
    f.beat_with(&snapshot(&["send"], None), &active).await;
    assert_eq!(f.count("alert_candidates").await, 0);
    // The lock that follows delivers nothing.
    f.beat_with(&snapshot(&["send"], None), &presence(2.0, Some(1.0)))
        .await;
    assert_eq!(f.alert_count().await, 0);
    f.close().await;
}

#[tokio::test]
async fn a_peer_heartbeat_never_resolves_a_gateway_s_own_decisions() {
    let f = Fixture::new().await;
    // The other computer is a gateway of its own, and the fixture gateway also
    // reaches it as a peer (every enabled hgs peer is a mobile route).
    let other = f
        .store
        .node(&f.node.workspace, "Other computer")
        .await
        .unwrap();
    let other = f
        .store
        .authenticate(s(&other, "node_token"), Role::Node)
        .await
        .unwrap();
    let (root, machine, route) = (
        Uuid::new_v4().to_string(),
        Uuid::new_v4().to_string(),
        Uuid::new_v4().to_string(),
    );
    let mut serving = advertising(&["overmind_read", "overmind_write"]);
    serving["mobile_capabilities"]["features"] = json!(["gateway_one_hop"]);
    let task = Uuid::new_v4().to_string();
    let slot = format!("overmind:{task}");
    let waiting = json!([{"task":task,"project":"example","title":"Pick a database","signature":signature('1')}]);
    for items in [json!([]), waiting] {
        let mut own = serving.clone();
        own["mobile_overmind"] = json!({"schema":1,"rev":"1","projects":{},"decisions":items});
        let body =
            json!({"snapshot":own,"machine_id":machine,"peers":[],"presence":presence(1.0, None)});
        f.store
            .heartbeat(&other, &body, &canonical(&own).unwrap())
            .await
            .unwrap();
    }
    let held = || f.count("alert_candidates WHERE kind='decision'");
    assert_eq!((held().await, f.alert_count().await), (1, 0));
    // The peer view of that computer has valid capabilities without the
    // gateway operations, which only its own gateway serves.
    let peers = json!([{"route_id":route,"machine_id":machine,"name":"Other","online":true}]);
    gateway_beat(&f, &root, &peers, &serving).await;
    peer_beat(&f, &route, &machine, &advertising(&["send"]), &Value::Null).await;
    assert_eq!((held().await, f.alert_count().await), (1, 0));
    // The owner left; delivery reads the computer's own snapshot, not the
    // newer peer view, so the held decision is delivered.
    let away = now() - 1000.0;
    f.store
        .db
        .exec(
            "UPDATE nodes SET presence_at=?,presence_until=? WHERE id=?",
            args![away, away, &other.id],
        )
        .await
        .unwrap();
    f.store
        .db
        .exec("UPDATE alert_candidates SET next_at=0", &[])
        .await
        .unwrap();
    f.store.maintain(false).await.unwrap();
    let alerts = f.events_of("alert").await;
    assert_eq!(alerts.len(), 1);
    assert_eq!(
        (
            alerts[0]["alert_kind"].as_str(),
            alerts[0]["session"].as_str(),
            alerts[0]["computer_id"].as_str()
        ),
        (
            Some("decision"),
            Some(slot.as_str()),
            Some(other.id.as_str())
        )
    );
    assert_eq!(held().await, 0);
    f.check_accounting().await;
    f.close().await;
}
