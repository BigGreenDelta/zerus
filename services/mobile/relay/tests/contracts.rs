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
