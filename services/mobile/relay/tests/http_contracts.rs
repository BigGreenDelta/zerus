//! HTTP resource contracts that only the Python reference relay's tests
//! covered: client identity behind proxies, anonymous and authenticated
//! admission, long-poll capacity, strict query parameters, response bounds,
//! readiness, health under a held database and background recovery.
//! `ORACLE.md` maps each Python test to its contract here.
use base64::{engine::general_purpose::STANDARD, Engine};
use reqwest::{Client, StatusCode};
use serde_json::{json, Value};
use std::time::{Duration, Instant};
use uuid::Uuid;
use zerus_relay::{
    args,
    config::Config,
    db::{i, now, s},
    http::App,
    json::digest,
    protocol::MIB,
    store::Role,
};

mod common;
mod support;
use common::Fixture;
use support::{eventually, Server};

fn config(change: impl FnOnce(&mut Config)) -> Config {
    let mut cfg = Config {
        background: false,
        ..Config::default()
    };
    change(&mut cfg);
    cfg
}
fn phone(f: &Fixture) -> &str {
    s(&f.paired, "device_token")
}
fn node(f: &Fixture) -> &str {
    s(&f.credentials, "node_token")
}
async fn pair(client: &Client, server: &Server, code: &str, forwarded: &str) -> u16 {
    client
        .post(server.at("/v1/pair"))
        .header("x-forwarded-for", forwarded)
        .json(&json!({"code":code,"device_name":"Synthetic phone"}))
        .send()
        .await
        .unwrap()
        .status()
        .as_u16()
}
async fn exhaust_pairing(client: &Client, server: &Server, forwarded: &str) {
    for _ in 0..10 {
        assert_eq!(
            pair(client, server, "synthetic-invalid", forwarded).await,
            401
        );
    }
}
async fn get(client: &Client, server: &Server, path: &str, token: &str) -> StatusCode {
    client
        .get(server.at(path))
        .bearer_auth(token)
        .send()
        .await
        .unwrap()
        .status()
}
async fn leases(f: &Fixture) -> i64 {
    f.count("poll_leases").await
}
async fn devices_with(f: &Fixture, hash: &str) -> i64 {
    let row = f
        .store
        .db
        .one(
            "SELECT count(*) AS n FROM devices WHERE token_hash=?",
            args![hash],
        )
        .await
        .unwrap()
        .unwrap();
    i(&row, "n")
}

#[tokio::test]
async fn trusted_proxy_clients_have_independent_pairing_quotas() {
    let f = Fixture::config(config(|c| {
        c.trusted_proxy_cidrs = vec!["127.0.0.1/32".parse().unwrap()]
    }))
    .await;
    let invitation = f.store.invite(&f.phone.workspace).await.unwrap();
    let server = Server::start(&f.store).await;
    let client = Client::new();
    exhaust_pairing(&client, &server, "192.0.2.1").await;
    assert_eq!(
        pair(&client, &server, "synthetic-invalid", "192.0.2.1").await,
        429
    );
    assert_eq!(
        pair(&client, &server, s(&invitation, "pair_code"), "192.0.2.2").await,
        200
    );
    server.stop().await;
    f.close().await;
}

#[tokio::test]
async fn an_untrusted_peer_cannot_rotate_forwarding_headers() {
    let f = Fixture::new().await;
    let server = Server::start(&f.store).await;
    let client = Client::new();
    exhaust_pairing(&client, &server, "192.0.2.1").await;
    let response = client
        .post(server.at("/v1/pair"))
        .header("x-forwarded-for", "192.0.2.2")
        .header("forwarded", "for=198.51.100.2")
        .json(&json!({"code":"synthetic-invalid","device_name":"Synthetic phone"}))
        .send()
        .await
        .unwrap();
    assert_eq!(response.status(), 429);
    server.stop().await;
    f.close().await;
}

#[tokio::test]
async fn a_trusted_chain_names_the_nearest_untrusted_hop() {
    let f = Fixture::config(config(|c| {
        c.trusted_proxy_cidrs = vec![
            "127.0.0.1/32".parse().unwrap(),
            "192.0.2.0/24".parse().unwrap(),
        ];
        c.max_forwarded_hops = 3;
    }))
    .await;
    let server = Server::start(&f.store).await;
    let client = Client::new();
    exhaust_pairing(&client, &server, "203.0.113.1, 198.51.100.1, 192.0.2.10").await;
    assert_eq!(
        pair(
            &client,
            &server,
            "synthetic-invalid",
            "203.0.113.2, 198.51.100.1, 192.0.2.10"
        )
        .await,
        429
    );
    // A longer chain than the configured hops is refused outright.
    assert_eq!(
        pair(
            &client,
            &server,
            "synthetic-invalid",
            "203.0.113.3, 203.0.113.2, 198.51.100.2, 192.0.2.10"
        )
        .await,
        400
    );
    server.stop().await;
    f.close().await;
}

#[tokio::test]
async fn anonymous_abuse_keeps_authenticated_and_health_access() {
    let f = Fixture::config(config(|c| c.max_anonymous_requests = 2)).await;
    let server = Server::start(&f.store).await;
    let client = Client::new();
    for index in 0..40 {
        let status = client
            .get(server.at("/v1/computers"))
            .header("x-forwarded-for", format!("192.0.2.{}", index + 1))
            .send()
            .await
            .unwrap()
            .status();
        assert!([401, 429].contains(&status.as_u16()), "{index}: {status}");
    }
    assert_eq!(get(&client, &server, "/v1/computers", phone(&f)).await, 200);
    assert_eq!(get(&client, &server, "/healthz", "").await, 200);
    // Slow pairing bodies hold both anonymous lanes.
    let head = "POST /v1/pair HTTP/1.1\r\nHost: localhost\r\nContent-Type: application/json\r\nContent-Length: 100\r\n\r\n{";
    let held = [server.hold(head).await, server.hold(head).await];
    let pairs = json!(["pair", "127.0.0.1"]);
    let key = zerus_relay::json::digest(zerus_relay::json::canonical(&pairs).unwrap());
    assert!(
        eventually(|| async {
            f.store
                .db
                .one("SELECT count FROM rate_limits WHERE key=?", args![&key])
                .await
                .unwrap()
                .is_some_and(|r| r["count"] == 2)
        })
        .await
    );
    assert_eq!(
        pair(&client, &server, "synthetic-invalid", "192.0.2.1").await,
        429
    );
    assert_eq!(get(&client, &server, "/v1/computers", phone(&f)).await, 200);
    assert_eq!(get(&client, &server, "/healthz", "").await, 200);
    drop(held);
    server.stop().await;
    f.close().await;
}

#[tokio::test]
async fn poll_capacity_spans_credentials_and_returns_after_a_disconnect() {
    let f = Fixture::config(config(|c| {
        c.max_polls_per_credential = 1;
        c.max_polls_per_workspace = 2;
        c.max_polls_global = 4;
    }))
    .await;
    let invitation = f.store.invite(&f.phone.workspace).await.unwrap();
    let second = f
        .store
        .pair(s(&invitation, "pair_code"), "Second phone")
        .await
        .unwrap();
    let server = Server::start(&f.store).await;
    let client = Client::new();
    let quick = "/v1/node/requests?wait=0.01";
    let held_node = server.poll("/v1/node/requests?wait=25", node(&f)).await;
    assert!(eventually(|| async { leases(&f).await == 1 }).await);
    // The credential's single poll is taken; health is unaffected.
    assert_eq!(get(&client, &server, quick, node(&f)).await, 429);
    assert_eq!(get(&client, &server, "/healthz", "").await, 200);
    // A phone takes the workspace's second poll; another phone finds none.
    let held_phone = server.poll("/v1/events?wait=25", phone(&f)).await;
    assert!(eventually(|| async { leases(&f).await == 2 }).await);
    assert_eq!(
        get(
            &client,
            &server,
            "/v1/events?wait=0.01",
            s(&second, "device_token")
        )
        .await,
        429
    );
    // Disconnected polls give their capacity back.
    drop((held_node, held_phone));
    assert!(eventually(|| async { leases(&f).await == 0 }).await);
    assert_eq!(get(&client, &server, quick, node(&f)).await, 200);
    server.stop().await;
    f.close().await;
}

#[tokio::test]
async fn idle_polls_leave_ordinary_authenticated_admission_free() {
    let f = Fixture::config(config(|c| c.max_authenticated_requests = 2)).await;
    let server = Server::start(&f.store).await;
    let client = Client::new();
    let polls = [
        server.poll("/v1/events?wait=25", phone(&f)).await,
        server.poll("/v1/events?wait=25", phone(&f)).await,
    ];
    assert!(eventually(|| async { leases(&f).await == 2 }).await);
    let heartbeat = client
        .post(server.at("/v1/node/heartbeat"))
        .bearer_auth(node(&f))
        .json(&json!({"snapshot":{"sessions":[]}}))
        .send()
        .await
        .unwrap();
    assert_eq!(heartbeat.status(), 200);
    let submitted = client
        .post(server.at("/v1/requests"))
        .bearer_auth(phone(&f))
        .json(&f.command("inspect"))
        .send()
        .await
        .unwrap();
    assert_eq!(submitted.status(), 202);
    drop(polls);
    server.stop().await;
    f.close().await;
}

#[tokio::test]
async fn cursors_waits_and_routing_flags_are_strict() {
    let f = Fixture::new().await;
    let server = Server::start(&f.store).await;
    let client = Client::new();
    for path in [
        "/v1/events?after=9223372036854775808",
        "/v1/events?after=-1",
        "/v1/events?wait=26",
        "/v1/events?wait=NaN",
        "/v1/events?wait=inf",
        "/v1/events?wait=1&wait=1",
    ] {
        assert_eq!(get(&client, &server, path, phone(&f)).await, 400, "{path}");
    }
    for path in [
        "/v1/node/requests?wait=NaN",
        "/v1/node/requests?gateway_one_hop=ssh",
    ] {
        assert_eq!(get(&client, &server, path, node(&f)).await, 400, "{path}");
    }
    assert_eq!(
        get(
            &client,
            &server,
            "/v1/events?after=9223372036854775807",
            phone(&f)
        )
        .await,
        200
    );
    // Non-finite numbers are not JSON.
    let nan = client
        .post(server.at("/v1/node/heartbeat"))
        .bearer_auth(node(&f))
        .header("content-type", "application/json")
        .body(r#"{"snapshot":NaN}"#)
        .send()
        .await
        .unwrap();
    assert_eq!(nan.status(), 400);
    server.stop().await;
    f.close().await;
}

#[tokio::test]
async fn unicode_event_pages_fit_the_phone_bound_without_skipping() {
    let f = Fixture::new().await;
    let session = "😀".repeat(1024);
    for _ in 0..100 {
        f.store
            .db
            .exec(
                "INSERT INTO events(workspace_id,node_id,session,kind,created) VALUES(?,?,?,?,?)",
                args![&f.phone.workspace, &f.node.id, &session, "attention", now()],
            )
            .await
            .unwrap();
    }
    let mut t = f.store.db.begin().await.unwrap();
    let expected: Vec<Value> = t
        .all("SELECT id FROM events ORDER BY id", &[])
        .await
        .unwrap()
        .into_iter()
        .map(|r| r["id"].clone())
        .collect();
    t.commit().await.unwrap();
    let server = Server::start(&f.store).await;
    let client = Client::new();
    let (mut after, mut received, mut pages) = (Value::from(0), vec![], 0);
    while received.len() < 100 && pages < 3 {
        let raw = client
            .get(server.at(&format!("/v1/events?after={after}")))
            .bearer_auth(phone(&f))
            .send()
            .await
            .unwrap()
            .bytes()
            .await
            .unwrap();
        assert!(raw.len() <= MIB, "{} bytes", raw.len());
        let page: Value = serde_json::from_slice(&raw).unwrap();
        received.extend(
            page["events"]
                .as_array()
                .unwrap()
                .iter()
                .map(|e| e["id"].clone()),
        );
        after = page["cursor"].clone();
        pages += 1;
    }
    assert!(pages > 1);
    assert_eq!(received, expected);
    server.stop().await;
    f.close().await;
}

#[tokio::test]
async fn results_are_bounded_in_their_escaped_form_before_acknowledgement() {
    let f = Fixture::new().await;
    let server = Server::start(&f.store).await;
    let client = Client::new();
    let b = f.command("send");
    f.submit(&b).await.unwrap();
    assert!(f.store.claim(&f.node, false).await.unwrap().is_some());
    let route = server.at(&format!("/v1/node/requests/{}/result", s(&b, "request_id")));
    let post = |raw: Vec<u8>| {
        client
            .post(&route)
            .bearer_auth(node(&f))
            .header("content-type", "application/json")
            .body(raw)
            .send()
    };
    // Each body is below 1 MiB as UTF-8 and above it once escaped.
    for value in [
        json!({"state":"completed","result":{"synthetic":"\u{0800}".repeat(200_000)},"error":null}),
        json!({"state":"completed","result":{"synthetic":"x".repeat(MIB - 200)},"error":"😀".repeat(20)}),
    ] {
        let raw = serde_json::to_vec(&value).unwrap();
        assert!(raw.len() < MIB);
        assert_eq!(post(raw).await.unwrap().status(), 413);
        let receipt = f
            .store
            .receipt(&f.phone, s(&b, "request_id"))
            .await
            .unwrap();
        assert_eq!(receipt["state"], "claimed");
    }
    let accepted = json!({"state":"completed","result":{"status":"submitted"},"error":null});
    assert_eq!(
        post(serde_json::to_vec(&accepted).unwrap())
            .await
            .unwrap()
            .status(),
        200
    );
    server.stop().await;
    f.check_accounting().await;
    f.close().await;
}

#[tokio::test]
async fn the_catalog_size_is_checked_before_snapshots_are_read() {
    let f = Fixture::config(config(|c| c.max_catalog_bytes = 100)).await;
    f.beat(&json!({"sessions":[],"synthetic":"x".repeat(200)}))
        .await;
    assert_eq!(
        f.store.computers(&f.phone).await.unwrap_err().0.as_u16(),
        413
    );
    let server = Server::start(&f.store).await;
    let client = Client::new();
    assert_eq!(get(&client, &server, "/v1/computers", phone(&f)).await, 413);
    assert_eq!(get(&client, &server, "/healthz", "").await, 200);
    server.stop().await;
    f.close().await;
}

#[tokio::test]
async fn readiness_probes_coalesce_while_the_database_is_held() {
    let f = Fixture::config(config(|c| {
        c.pool_min = 1;
        c.pool_max = 1;
    }))
    .await;
    let server = Server::start(&f.store).await;
    let client = Client::new();
    // The only pooled connection is busy, so a probe waits for it.
    let held = f.store.db.begin().await.unwrap();
    let first = tokio::spawn(client.get(server.at("/readyz")).send());
    tokio::time::sleep(Duration::from_millis(100)).await;
    for _ in 0..10 {
        let started = Instant::now();
        assert_eq!(get(&client, &server, "/readyz", "").await, 503);
        assert!(started.elapsed() < Duration::from_millis(500));
    }
    assert_eq!(get(&client, &server, "/healthz", "").await, 200);
    drop(held);
    assert_eq!(first.await.unwrap().unwrap().status(), 200);
    assert_eq!(get(&client, &server, "/readyz", "").await, 200);
    server.stop().await;
    f.close().await;
}

#[tokio::test]
async fn health_answers_while_a_writer_holds_the_database() {
    let f = Fixture::new().await;
    let server = Server::start(&f.store).await;
    let client = Client::new();
    let mut writer = f.store.db.begin().await.unwrap();
    if f.backend.is_postgres() {
        writer
            .exec("LOCK TABLE nodes IN ACCESS EXCLUSIVE MODE", &[])
            .await
            .unwrap();
    }
    let heartbeat = tokio::spawn(
        client
            .post(server.at("/v1/node/heartbeat"))
            .bearer_auth(node(&f).to_string())
            .json(&json!({"snapshot":{"sessions":[]}}))
            .send(),
    );
    tokio::time::sleep(Duration::from_millis(50)).await;
    let started = Instant::now();
    assert_eq!(get(&client, &server, "/healthz", "").await, 200);
    assert!(started.elapsed() < Duration::from_millis(300));
    assert!(
        !heartbeat.is_finished(),
        "the heartbeat waits for the writer"
    );
    writer.commit().await.unwrap();
    assert_eq!(heartbeat.await.unwrap().unwrap().status(), 200);
    server.stop().await;
    f.close().await;
}

#[tokio::test]
async fn background_maintenance_and_wakes_resume_after_failures() {
    let f = Fixture::config(Config {
        background: true,
        maintenance_interval: 0.1,
        claim_ttl: 1,
        ..Config::default()
    })
    .await;
    f.store
        .register_push(
            &f.phone,
            "unifiedpush",
            "https://push.example.invalid/opaque",
        )
        .await
        .unwrap();
    // Without presence the owner is away, so a question queues a wake at once.
    for phase in ["idle", "input"] {
        f.beat(&json!({"sessions":[{"name":"example","phase":phase,"run_id":"r","conversation_id":"c"}]}))
            .await;
    }
    assert_eq!(f.count("push_jobs").await, 1);
    let b = f.command("send");
    f.submit(&b).await.unwrap();
    assert!(f.store.claim(&f.node, false).await.unwrap().is_some());
    // Both loops fail while their tables are away.
    for (from, to) in [
        ("requests", "requests_hidden"),
        ("push_jobs", "push_jobs_hidden"),
    ] {
        f.store
            .db
            .exec(&format!("ALTER TABLE {from} RENAME TO {to}"), &[])
            .await
            .unwrap();
    }
    let app = App::new(f.store.clone()).await.unwrap();
    app.start_background();
    tokio::time::sleep(Duration::from_millis(1500)).await;
    let hidden = f
        .store
        .db
        .one(
            "SELECT state FROM requests_hidden WHERE id=?",
            args![&b["request_id"]],
        )
        .await
        .unwrap()
        .unwrap();
    assert_eq!(hidden["state"], "claimed");
    assert!(f
        .store
        .push_registration(&f.phone.id)
        .await
        .unwrap()
        .is_some());
    for (from, to) in [
        ("requests_hidden", "requests"),
        ("push_jobs_hidden", "push_jobs"),
    ] {
        f.store
            .db
            .exec(&format!("ALTER TABLE {from} RENAME TO {to}"), &[])
            .await
            .unwrap();
    }
    // Later passes expire the claim and deliver (here: retire) the wake.
    let mut recovered = false;
    for _ in 0..50 {
        // Read the row itself: a receipt would expire the claim on its own.
        let state = f
            .store
            .db
            .one(
                "SELECT state FROM requests WHERE id=?",
                args![&b["request_id"]],
            )
            .await
            .unwrap()
            .unwrap()["state"]
            .clone();
        let registered = f.store.push_registration(&f.phone.id).await.unwrap();
        if state == "uncertain" && registered.is_none() && f.count("push_jobs").await == 0 {
            recovered = true;
            break;
        }
        tokio::time::sleep(Duration::from_millis(100)).await;
    }
    assert!(recovered, "background loops did not resume");
    app.stop.cancel();
    f.check_accounting().await;
    f.close().await;
}

#[tokio::test]
async fn a_large_inline_upload_crosses_http_and_reaches_the_node_once() {
    let f = Fixture::new().await;
    let server = Server::start(&f.store).await;
    let client = Client::new();
    let id = Uuid::new_v4().to_string();
    let file = json!({"name":"example.bin","mime":"application/octet-stream","data_base64":STANDARD.encode(vec![b'x'; 2 * MIB])});
    let request = json!({"request_id":id,"computer_id":f.node.id,"operation":"send","session":"codex/example/nested/tag",
        "payload":{"request_id":id,"text":"Read these files","expected_run_id":"run","expected_conversation_id":"conversation","attachments":[file]}});
    let submit = || {
        client
            .post(server.at("/v1/requests"))
            .bearer_auth(phone(&f))
            .json(&request)
            .send()
    };
    let first = submit().await.unwrap();
    assert_eq!(first.status(), 202);
    assert_eq!(first.json::<Value>().await.unwrap()["state"], "queued");
    let claim = |client: &Client| {
        client
            .get(server.at("/v1/node/requests"))
            .bearer_auth(node(&f))
            .send()
    };
    let batch: Value = claim(&client).await.unwrap().json().await.unwrap();
    assert_eq!(batch["requests"].as_array().unwrap().len(), 1);
    assert_eq!(batch["requests"][0]["payload"], request["payload"]);
    let again = submit().await.unwrap();
    assert_eq!(again.status(), 202);
    assert_eq!(again.json::<Value>().await.unwrap()["state"], "claimed");
    let empty: Value = claim(&client).await.unwrap().json().await.unwrap();
    assert_eq!(empty["requests"], json!([]));
    server.stop().await;
    f.close().await;
}

#[tokio::test]
async fn health_capabilities_and_administration_stay_off_the_api() {
    let f = Fixture::new().await;
    let server = Server::start(&f.store).await;
    let client = Client::new();
    let health: Value = client
        .get(server.at("/healthz"))
        .send()
        .await
        .unwrap()
        .json()
        .await
        .unwrap();
    assert_eq!(health["protocol_version"], 1);
    let capabilities: Value = client
        .get(server.at("/v1/capabilities"))
        .bearer_auth(phone(&f))
        .send()
        .await
        .unwrap()
        .json()
        .await
        .unwrap();
    assert_eq!(capabilities["push_providers"], json!([]));
    let admin = client
        .post(server.at("/v1/admin"))
        .bearer_auth(phone(&f))
        .json(&json!({}))
        .send()
        .await
        .unwrap();
    assert_eq!(admin.status(), 404);
    assert_eq!(get(&client, &server, "/v1/computers", "wrong").await, 401);
    server.stop().await;
    f.close().await;
}

#[tokio::test]
async fn pairing_is_single_use_expires_and_keeps_only_hashes() {
    let f = Fixture::new().await;
    let server = Server::start(&f.store).await;
    let client = Client::new();
    let pair = |code: Value| {
        client
            .post(server.at("/v1/pair"))
            .json(&json!({"code":code,"device_name":"Other"}))
            .send()
    };
    let invitation = f.store.invite(&f.phone.workspace).await.unwrap();
    let paired: Value = pair(invitation["pair_code"].clone())
        .await
        .unwrap()
        .json()
        .await
        .unwrap();
    assert_eq!(paired["workspace_id"], f.phone.workspace);
    assert_eq!(paired["workspace_name"], "Example workspace");
    let token = s(&paired, "device_token");
    assert_eq!(token.len(), 43);
    assert_eq!(
        pair(invitation["pair_code"].clone())
            .await
            .unwrap()
            .status(),
        401
    );
    let expired = f.store.invite(&f.phone.workspace).await.unwrap();
    f.store
        .db
        .exec(
            "UPDATE invitations SET expires=? WHERE code_hash=?",
            args![now() - 1.0, &digest(s(&expired, "pair_code"))],
        )
        .await
        .unwrap();
    assert_eq!(
        pair(expired["pair_code"].clone()).await.unwrap().status(),
        401
    );
    // Only the token's digest is stored.
    assert_eq!(devices_with(&f, token).await, 0);
    assert_eq!(devices_with(&f, &digest(token)).await, 1);
    server.stop().await;
    f.close().await;
}

#[tokio::test]
async fn credentials_are_scoped_to_their_role_and_workspace() {
    let f = Fixture::new().await;
    let foreign = f
        .store
        .provision("Other workspace", "Foreign")
        .await
        .unwrap();
    let server = Server::start(&f.store).await;
    let client = Client::new();
    let computers: Value = client
        .get(server.at("/v1/computers"))
        .bearer_auth(phone(&f))
        .send()
        .await
        .unwrap()
        .json()
        .await
        .unwrap();
    let ids: Vec<_> = computers["computers"]
        .as_array()
        .unwrap()
        .iter()
        .map(|c| c["id"].clone())
        .collect();
    assert_eq!(ids, [json!(f.node.id)]);
    let submit = |b: &Value| {
        client
            .post(server.at("/v1/requests"))
            .bearer_auth(phone(&f))
            .json(b)
            .send()
    };
    let mut elsewhere = f.command("inspect");
    elsewhere["computer_id"] = foreign["node_id"].clone();
    assert_eq!(submit(&elsewhere).await.unwrap().status(), 404);
    assert_eq!(get(&client, &server, "/v1/computers", node(&f)).await, 401);
    assert_eq!(
        get(&client, &server, "/v1/node/requests", phone(&f)).await,
        401
    );
    // A phone that logs out loses its token and its queued requests.
    let queued = f.command("send");
    assert_eq!(submit(&queued).await.unwrap().status(), 202);
    let logout = client
        .delete(server.at("/v1/device"))
        .bearer_auth(phone(&f))
        .send()
        .await
        .unwrap();
    assert_eq!(logout.status(), 200);
    assert_eq!(get(&client, &server, "/v1/computers", phone(&f)).await, 401);
    let row = f
        .store
        .db
        .one(
            "SELECT state FROM requests WHERE id=?",
            args![&queued["request_id"]],
        )
        .await
        .unwrap()
        .unwrap();
    assert_eq!(row["state"], "failed");
    server.stop().await;
    f.check_accounting().await;
    f.close().await;
}

#[tokio::test]
async fn long_polls_recheck_revocation() {
    let f = Fixture::new().await;
    let server = Server::start(&f.store).await;
    let client = Client::new();
    let poll = tokio::spawn(
        client
            .get(server.at("/v1/node/requests?wait=1"))
            .bearer_auth(node(&f).to_string())
            .send(),
    );
    assert!(eventually(|| async { leases(&f).await == 1 }).await);
    f.store.revoke(Role::Node, &f.node.id).await.unwrap();
    assert_eq!(poll.await.unwrap().unwrap().status(), 401);
    server.stop().await;
    f.close().await;
}

#[tokio::test]
async fn heartbeat_events_follow_transitions_and_carry_no_content() {
    let f = Fixture::new().await;
    let server = Server::start(&f.store).await;
    let client = Client::new();
    let session = json!({"name":"codex/project/a/b","run_id":"r1","conversation_id":"c1","phase":"working","activity":"busy","prompt":"Private text"});
    let with = |changes: Value| {
        let mut row = session.clone();
        for (k, v) in changes.as_object().unwrap() {
            row[k] = v.clone();
        }
        row
    };
    let question = with(json!({"phase":"input","attention_id":"question-one"}));
    for row in [
        session.clone(),
        question.clone(),
        question.clone(),
        with(json!({"phase":"input","attention_id":"question-two"})),
        with(json!({"phase":"idle","activity":"idle"})),
    ] {
        let beat = client
            .post(server.at("/v1/node/heartbeat"))
            .bearer_auth(node(&f))
            .json(&json!({"snapshot":{"sessions":[row]}}))
            .send()
            .await
            .unwrap();
        assert_eq!(beat.status(), 200);
    }
    let events = |token: &str, path: &str| {
        client
            .get(server.at(path))
            .bearer_auth(token.to_string())
            .send()
    };
    let raw = events(phone(&f), "/v1/events")
        .await
        .unwrap()
        .text()
        .await
        .unwrap();
    assert!(!raw.contains("Private text"));
    let page: Value = serde_json::from_str(&raw).unwrap();
    let kinds: Vec<_> = page["events"]
        .as_array()
        .unwrap()
        .iter()
        .filter(|e| e["kind"] != "alert")
        .map(|e| e["kind"].as_str().unwrap())
        .collect();
    assert_eq!(kinds, ["attention", "attention", "completed"]);
    let later: Value = events(phone(&f), &format!("/v1/events?after={}", page["cursor"]))
        .await
        .unwrap()
        .json()
        .await
        .unwrap();
    assert_eq!(later["events"], json!([]));
    let computers: Value = events(phone(&f), "/v1/computers")
        .await
        .unwrap()
        .json()
        .await
        .unwrap();
    assert_eq!(computers["computers"][0]["online"], true);
    assert_eq!(
        computers["computers"][0]["snapshot"]["sessions"][0]["prompt"],
        "Private text"
    );
    // Another workspace's phone sees none of them.
    let other = f.store.provision("Other workspace", "Other").await.unwrap();
    let paired = f
        .store
        .pair(s(&other, "pair_code"), "Other phone")
        .await
        .unwrap();
    let foreign: Value = events(s(&paired, "device_token"), "/v1/events")
        .await
        .unwrap()
        .json()
        .await
        .unwrap();
    assert_eq!(foreign["events"], json!([]));
    server.stop().await;
    f.close().await;
}

#[tokio::test]
async fn pending_question_fingerprints_announce_additions_and_changes_only() {
    let f = Fixture::new().await;
    let send = |pending: Value| json!({"sessions":[{"name":"codex/project/deep/tag","run_id":"r","conversation_id":"c","phase":"working","activity":"busy","mobile_attention":pending}]});
    let first = json!([{"question_id":"q1","question_hash":"h1"}]);
    let changed = json!([{"question_id":"q1","question_hash":"h2"}]);
    // The first snapshot is silent even with a pending question.
    f.beat(&send(first.clone())).await;
    f.beat(&send(first.clone())).await;
    assert!(f.events_of("attention").await.is_empty());
    // Removals and repeats are silent; additions and changes are not.
    for pending in [json!([]), first.clone(), first, changed, json!([])] {
        f.beat(&send(pending)).await;
    }
    assert_eq!(f.events_of("attention").await.len(), 2);
    f.close().await;
}

#[tokio::test]
async fn push_registration_needs_a_configured_provider() {
    let f = Fixture::new().await;
    let server = Server::start(&f.store).await;
    let client = Client::new();
    let register = |body: Value| {
        client
            .post(server.at("/v1/push"))
            .bearer_auth(phone(&f))
            .json(&body)
            .send()
    };
    let retired = json!({"provider":"unifiedpush","endpoint":"https://push.example.invalid/token"});
    assert_eq!(register(retired).await.unwrap().status(), 400);
    let fcm = json!({"provider":"fcm","token":"example"});
    assert_eq!(register(fcm).await.unwrap().status(), 503);
    assert_eq!(f.count("pushes").await, 0);
    server.stop().await;
    f.close().await;
}

#[tokio::test]
async fn a_cancelled_poll_keeps_its_slot_until_durable_cleanup_finishes() {
    let f = Fixture::config(config(|c| c.max_polls_global = 1)).await;
    // The cleanup must wait on a table while other queries run, which needs
    // a second database connection.
    if !f.backend.is_postgres() {
        f.close().await;
        return;
    }
    let server = Server::start(&f.store).await;
    let client = Client::new();
    let held = server.poll("/v1/node/requests?wait=25", node(&f)).await;
    assert!(eventually(|| async { leases(&f).await == 1 }).await);
    let mut lock = f.store.db.begin().await.unwrap();
    lock.exec("LOCK TABLE poll_leases IN ACCESS EXCLUSIVE MODE", &[])
        .await
        .unwrap();
    drop(held);
    // The disconnected poll's cleanup now waits for the table.
    let waiting = "pg_stat_activity WHERE datname=current_database() AND wait_event_type='Lock' AND query LIKE '%poll_leases%' AND pid<>pg_backend_pid()";
    assert!(eventually(|| async { f.count(waiting).await == 1 }).await);
    // Until it finishes, the worker's only poll slot stays taken.
    let quick = "/v1/events?wait=0.01";
    assert_eq!(get(&client, &server, quick, phone(&f)).await, 429);
    lock.commit().await.unwrap();
    assert!(eventually(|| async { leases(&f).await == 0 }).await);
    assert!(
        eventually(|| async { get(&client, &server, quick, phone(&f)).await == StatusCode::OK })
            .await
    );
    server.stop().await;
    f.close().await;
}
