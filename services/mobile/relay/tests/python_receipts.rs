//! A database the removed Python relay wrote keeps its receipt identities and
//! gains the current schema in place. `fixtures/python_relay.sql` is that
//! database, frozen; it exists only on PostgreSQL, so without
//! `ZERUS_RELAY_TEST_DATABASE_URL` these contracts skip.
use reqwest::Method;
use serde_json::{json, Value};
use tokio_util::sync::CancellationToken;
use zerus_relay::{
    args,
    config::Config,
    db::i,
    http::{serve, App},
    json::{canonical, digest, parse},
};

mod common;
use common::Backend;

const PYTHON_RELAY: &str = include_str!("fixtures/python_relay.sql");
const WORKSPACE: &str = "00000000-0000-4000-8000-000000000001";
const NODE: &str = "00000000-0000-4000-8000-000000000002";
const REQUEST: &str = "00000000-0000-4000-8000-000000000004";
const NODE_TOKEN: &str = "synthetic-legacy-node-token";
const DEVICE_TOKEN: &str = "synthetic-legacy-device-token";
/// Python's SHA-256 of its canonical request text, as the fixture stores it.
const BODY_HASH: &str = "73c395492504da58af5621447210f5863c1b71ed3bdd9dc174594ad9aa20eef0";
/// The completed result in Python's canonical text, as the fixture stores it.
const RESULT: &str = r#"{"object":{"$serde_json::private::Number":"123"},"text":"\ud83d\ude00","value":1208925819614629174706176}"#;
/// The phone's request as Python's `json.dumps` sent it, default separators.
const SENT: &str = r#"{"request_id": "00000000-0000-4000-8000-000000000004", "computer_id": "00000000-0000-4000-8000-000000000002", "operation": "answer", "session": "synthetic", "payload": {"request_id": "00000000-0000-4000-8000-000000000004", "expected_run_id": "run", "expected_conversation_id": "conversation", "question_id": "q", "expected_question_hash": "hash", "answers": {"float": 1e-05, "large": 1208925819614629174706176, "text": "Unicode \ud83d\ude00", "object": {"$serde_json::private::Number": "123"}}}}"#;
/// The same request compact, reordered, in raw UTF-8, with an escaped key and
/// another spelling of the float.
const RESPELLED: &str = r#"{"payload":{"answers":{"object":{"\u0024serde_json::private::Number":"123"},"text":"Unicode 😀","large":1208925819614629174706176,"float":1E-5},"question_id":"q","request_id":"00000000-0000-4000-8000-000000000004","expected_question_hash":"hash","expected_conversation_id":"conversation","expected_run_id":"run"},"session":"synthetic","operation":"answer","computer_id":"00000000-0000-4000-8000-000000000002","request_id":"00000000-0000-4000-8000-000000000004"}"#;

struct Relay {
    client: reqwest::Client,
    url: String,
}

impl Relay {
    /// Status and body, decoded as the relay decodes JSON: serde_json alone
    /// would read the `$serde_json::private::Number` key as a number.
    async fn call(
        &self,
        method: Method,
        route: &str,
        token: &str,
        body: Option<&str>,
    ) -> (u16, Value) {
        let mut request = self
            .client
            .request(method, format!("{}{route}", self.url))
            .bearer_auth(token);
        if let Some(body) = body {
            request = request
                .header("Content-Type", "application/json")
                .body(body.to_owned());
        }
        let response = request.send().await.unwrap();
        let status = response.status().as_u16();
        let body = response.bytes().await.unwrap();
        (status, parse(&body, 32, 50000).unwrap())
    }
    async fn heartbeat(&self, session: &Value, idle: f64) {
        let body = json!({"snapshot":{"sessions":[session]},
            "presence":{"idle_seconds":idle,"locked":false,"locked_seconds":null}});
        let (status, answer) = self
            .call(
                Method::POST,
                "/v1/node/heartbeat",
                NODE_TOKEN,
                Some(&body.to_string()),
            )
            .await;
        assert_eq!(status, 200, "{answer}");
    }
    async fn events(&self) -> Vec<Value> {
        let (status, events) = self
            .call(Method::GET, "/v1/events", DEVICE_TOKEN, None)
            .await;
        assert_eq!(status, 200, "{events}");
        events["events"].as_array().unwrap().clone()
    }
}

#[tokio::test]
async fn python_receipt_identity_and_presence_alerts_survive_the_upgrade() {
    let cfg = Config {
        background: false,
        ..Config::default()
    };
    let Some(backend) = Backend::restored(cfg, PYTHON_RELAY).await else {
        return;
    };
    // Opening the Python database upgrades its schema in place.
    let store = backend.store().await;
    let listener = tokio::net::TcpListener::bind("127.0.0.1:0").await.unwrap();
    let relay = Relay {
        client: reqwest::Client::new(),
        url: format!("http://{}", listener.local_addr().unwrap()),
    };
    let stop = CancellationToken::new();
    let server = tokio::spawn(serve(
        listener,
        App::new(store.clone()).await.unwrap(),
        stop.clone(),
    ));

    // Either spelling of the original request replays Python's receipt.
    for body in [SENT, RESPELLED] {
        let (status, receipt) = relay
            .call(Method::POST, "/v1/requests", DEVICE_TOKEN, Some(body))
            .await;
        assert_eq!(status, 202, "{receipt}");
        assert_eq!(
            (&receipt["request_id"], &receipt["state"]),
            (&json!(REQUEST), &json!("completed"))
        );
        assert_eq!(canonical(&receipt["result"]).unwrap(), RESULT);
        assert!(receipt["error"].is_null());
    }
    let changed = SENT.replace("1e-05", "1e-06");
    let (status, _) = relay
        .call(Method::POST, "/v1/requests", DEVICE_TOKEN, Some(&changed))
        .await;
    assert_eq!(status, 409);
    let (status, receipt) = relay
        .call(
            Method::GET,
            &format!("/v1/requests/{REQUEST}"),
            DEVICE_TOKEN,
            None,
        )
        .await;
    assert_eq!(
        (status, canonical(&receipt["result"]).unwrap()),
        (200, RESULT.into())
    );
    let (status, claims) = relay
        .call(Method::GET, "/v1/node/requests", NODE_TOKEN, None)
        .await;
    assert_eq!((status, claims), (200, json!({"requests":[]})));
    // Rust spells the identity as Python did and never rewrites the stored row.
    let sent = parse(SENT.as_bytes(), 32, 50000).unwrap();
    assert_eq!(digest(canonical(&sent).unwrap()), BODY_HASH);
    let row = store
        .db
        .one(
            "SELECT body_hash,result FROM requests WHERE id=?",
            args![REQUEST],
        )
        .await
        .unwrap()
        .unwrap();
    assert_eq!(
        (&row["body_hash"], &row["result"]),
        (&json!(BODY_HASH), &json!(RESULT))
    );

    // Presence alerts, whose columns and table the upgrade added.
    let (_, capabilities) = relay
        .call(Method::GET, "/v1/capabilities", DEVICE_TOKEN, None)
        .await;
    assert!(capabilities["features"]
        .as_array()
        .unwrap()
        .contains(&json!("presence_alerts")));
    let session = json!({"name":"codex/project/alert","run_id":"r","conversation_id":"c","phase":"working","activity":"busy"});
    relay.heartbeat(&session, 1.0).await;
    let mut question = session.clone();
    question["phase"] = json!("input");
    question["attention_id"] = json!("q1");
    question["attention_signature"] = json!("a".repeat(64));
    relay.heartbeat(&question, 1.0).await;
    let events = relay.events().await;
    assert_eq!(
        events.iter().map(|e| &e["kind"]).collect::<Vec<_>>(),
        [&json!("attention")]
    );
    relay.heartbeat(&question, 600.0).await; // Same snapshot; only presence changes.
    let events = relay.events().await;
    assert_eq!(
        events.iter().map(|e| &e["kind"]).collect::<Vec<_>>(),
        [&json!("attention"), &json!("alert")]
    );
    assert!(events[0].get("alert_kind").is_none());
    assert_eq!(
        (
            &events[1]["alert_kind"],
            &events[1]["session"],
            &events[1]["computer_id"]
        ),
        (
            &json!("attention"),
            &json!("codex/project/alert"),
            &json!(NODE)
        )
    );

    // New admissions charge the payload shard Python charged for this workspace.
    let fresh = json!({"request_id":"00000000-0000-4000-8000-000000000005","computer_id":NODE,
        "operation":"inspect","session":"synthetic","payload":{}});
    let (status, receipt) = relay
        .call(
            Method::POST,
            "/v1/requests",
            DEVICE_TOKEN,
            Some(&fresh.to_string()),
        )
        .await;
    assert_eq!((status, &receipt["state"]), (202, &json!("queued")));
    assert_eq!(store.db.shard(WORKSPACE), 16);
    let bytes = |sql: &'static str| {
        let db = store.db.clone();
        async move { i(&db.one(sql, &[]).await.unwrap().unwrap(), "n") }
    };
    let charged = bytes("SELECT payload_bytes AS n FROM payload_shards WHERE id=16").await;
    assert!(charged > 572);
    assert_eq!(
        bytes("SELECT payload_bytes AS n FROM workspace_usage").await,
        charged
    );
    assert_eq!(
        bytes(
            "SELECT CAST(sum(body_bytes+result_bytes+reserved_bytes) AS BIGINT) AS n FROM requests"
        )
        .await,
        charged
    );
    assert_eq!(
        bytes("SELECT CAST(sum(payload_bytes) AS BIGINT) AS n FROM payload_shards").await,
        charged
    );

    stop.cancel();
    server.await.unwrap().unwrap();
    store.db.close().await;
}
