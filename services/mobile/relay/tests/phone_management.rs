//! Phone management by a computer credential: invitations, the phone list
//! (platform, pairing time, last activity, issuing computer) and revocation.
use reqwest::{Client, StatusCode};
use serde_json::{json, Value};
use zerus_relay::{
    args,
    db::{now, s},
    json::digest,
    store::{Credential, Role},
};

mod common;
mod support;
use common::Fixture;
use support::Server;

fn node(f: &Fixture) -> &str {
    s(&f.credentials, "node_token")
}
fn phone(f: &Fixture) -> &str {
    s(&f.paired, "device_token")
}
async fn call(
    client: &Client,
    server: &Server,
    method: reqwest::Method,
    path: &str,
    token: &str,
    body: Option<Value>,
) -> (StatusCode, Value) {
    let mut request = client.request(method, server.at(path)).bearer_auth(token);
    if let Some(body) = body {
        request = request.json(&body);
    }
    let response = request.send().await.unwrap();
    let status = response.status();
    (status, response.json().await.unwrap_or(Value::Null))
}
async fn invite(client: &Client, server: &Server, token: &str) -> (StatusCode, Value) {
    call(
        client,
        server,
        reqwest::Method::POST,
        "/v1/node/invitations",
        token,
        Some(json!({})),
    )
    .await
}
async fn devices(client: &Client, server: &Server, token: &str) -> Value {
    let (status, body) = call(
        client,
        server,
        reqwest::Method::GET,
        "/v1/node/devices",
        token,
        None,
    )
    .await;
    assert_eq!(status, StatusCode::OK, "{body}");
    body
}
fn device<'a>(list: &'a Value, id: &str) -> Option<&'a Value> {
    list["devices"]
        .as_array()
        .unwrap()
        .iter()
        .find(|d| d["id"] == id)
}

#[tokio::test]
async fn a_computer_invites_a_phone_and_the_list_names_that_computer() {
    let f = Fixture::new().await;
    let server = Server::start(&f.store).await;
    let client = Client::new();
    let (status, invitation) = invite(&client, &server, node(&f)).await;
    assert_eq!(status, StatusCode::OK, "{invitation}");
    assert_eq!(invitation["workspace_id"], f.node.workspace);
    assert_eq!(invitation["workspace_name"], "Example workspace");
    let expires = invitation["expires_at"].as_f64().unwrap();
    assert!((expires - now() - 600.0).abs() < 30.0);
    let paired = client
        .post(server.at("/v1/pair"))
        .json(&json!({"code":invitation["pair_code"],"device_name":"Synthetic phone"}))
        .send()
        .await
        .unwrap();
    assert_eq!(paired.status(), StatusCode::OK);
    let paired: Value = paired.json().await.unwrap();
    let list = devices(&client, &server, node(&f)).await;
    assert_eq!(list["workspace_id"], f.node.workspace);
    assert_eq!(list["workspace_name"], "Example workspace");
    let added = device(&list, s(&paired, "device_id")).unwrap();
    assert_eq!(added["name"], "Synthetic phone");
    assert_eq!(
        added["added_by"],
        json!({"computer_id":f.node.id,"name":"Example computer"})
    );
    assert!((added["paired_at"].as_f64().unwrap() - now()).abs() < 30.0);
    assert_eq!(added["platform"], Value::Null);
    // The administrator's provisioning invitation has no issuing computer.
    let provisioned = device(&list, &f.phone.id).unwrap();
    assert_eq!(provisioned["name"], "Phone");
    assert_eq!(provisioned["added_by"], Value::Null);
    // The invitation is single use.
    let again = client
        .post(server.at("/v1/pair"))
        .json(&json!({"code":invitation["pair_code"],"device_name":"Synthetic phone"}))
        .send()
        .await
        .unwrap();
    assert_eq!(again.status(), StatusCode::UNAUTHORIZED);
    server.stop().await;
    f.close().await;
}

#[tokio::test]
async fn phones_report_their_platform_and_throttled_last_activity() {
    let f = Fixture::new().await;
    let server = Server::start(&f.store).await;
    let client = Client::new();
    // The fixture's own authentication already recorded activity.
    f.store
        .db
        .exec(
            "UPDATE devices SET last_seen=NULL WHERE id=?",
            args![&f.phone.id],
        )
        .await
        .unwrap();
    let before = devices(&client, &server, node(&f)).await;
    assert_eq!(
        device(&before, &f.phone.id).unwrap()["last_seen_at"],
        Value::Null
    );
    f.store
        .register_push(&f.phone, "apns", &"a".repeat(64))
        .await
        .unwrap();
    let (status, _) = call(
        &client,
        &server,
        reqwest::Method::GET,
        "/v1/computers",
        phone(&f),
        None,
    )
    .await;
    assert_eq!(status, StatusCode::OK);
    let seen = devices(&client, &server, node(&f)).await;
    let row = device(&seen, &f.phone.id).unwrap();
    assert_eq!(row["platform"], "ios");
    let first = row["last_seen_at"].as_f64().unwrap();
    assert!((first - now()).abs() < 30.0);
    // A recent activity time is not rewritten by every request.
    f.store
        .db
        .exec(
            "UPDATE devices SET last_seen=? WHERE id=?",
            args![first - 60.0, &f.phone.id],
        )
        .await
        .unwrap();
    call(
        &client,
        &server,
        reqwest::Method::GET,
        "/v1/computers",
        phone(&f),
        None,
    )
    .await;
    let unchanged = devices(&client, &server, node(&f)).await;
    assert_eq!(
        device(&unchanged, &f.phone.id).unwrap()["last_seen_at"]
            .as_f64()
            .unwrap(),
        first - 60.0
    );
    f.store
        .db
        .exec(
            "UPDATE devices SET last_seen=? WHERE id=?",
            args![first - 600.0, &f.phone.id],
        )
        .await
        .unwrap();
    call(
        &client,
        &server,
        reqwest::Method::GET,
        "/v1/computers",
        phone(&f),
        None,
    )
    .await;
    let refreshed = devices(&client, &server, node(&f)).await;
    assert!(
        device(&refreshed, &f.phone.id).unwrap()["last_seen_at"]
            .as_f64()
            .unwrap()
            > first - 30.0
    );
    f.store
        .register_push(&f.phone, "fcm", "synthetic-fcm-token")
        .await
        .unwrap();
    let android = devices(&client, &server, node(&f)).await;
    assert_eq!(
        device(&android, &f.phone.id).unwrap()["platform"],
        "android"
    );
    server.stop().await;
    f.close().await;
}

#[tokio::test]
async fn a_computer_revokes_only_phones_of_its_own_workspace() {
    let f = Fixture::new().await;
    let other = f
        .store
        .provision("Other workspace", "Other computer")
        .await
        .unwrap();
    let other_node = s(&other, "node_token");
    let server = Server::start(&f.store).await;
    let client = Client::new();
    let foreign = devices(&client, &server, other_node).await;
    assert!(device(&foreign, &f.phone.id).is_none());
    let path = format!("/v1/node/devices/{}", f.phone.id);
    let (status, _) = call(
        &client,
        &server,
        reqwest::Method::DELETE,
        &path,
        other_node,
        None,
    )
    .await;
    assert_eq!(status, StatusCode::NOT_FOUND);
    let (status, _) = call(
        &client,
        &server,
        reqwest::Method::GET,
        "/v1/computers",
        phone(&f),
        None,
    )
    .await;
    assert_eq!(status, StatusCode::OK);
    let (status, body) = call(
        &client,
        &server,
        reqwest::Method::DELETE,
        &path,
        node(&f),
        None,
    )
    .await;
    assert_eq!((status, body), (StatusCode::OK, json!({"revoked":true})));
    let (status, _) = call(
        &client,
        &server,
        reqwest::Method::GET,
        "/v1/computers",
        phone(&f),
        None,
    )
    .await;
    assert_eq!(status, StatusCode::UNAUTHORIZED);
    assert!(device(&devices(&client, &server, node(&f)).await, &f.phone.id).is_none());
    let (status, _) = call(
        &client,
        &server,
        reqwest::Method::DELETE,
        &path,
        node(&f),
        None,
    )
    .await;
    assert_eq!(status, StatusCode::NOT_FOUND);
    server.stop().await;
    f.close().await;
}

#[tokio::test]
async fn phone_management_needs_a_computer_credential_and_strict_input() {
    let f = Fixture::new().await;
    let server = Server::start(&f.store).await;
    let client = Client::new();
    let (status, _) = call(
        &client,
        &server,
        reqwest::Method::GET,
        "/v1/node/devices",
        phone(&f),
        None,
    )
    .await;
    assert_eq!(status, StatusCode::UNAUTHORIZED);
    let (status, _) = invite(&client, &server, phone(&f)).await;
    assert_eq!(status, StatusCode::UNAUTHORIZED);
    let (status, _) = call(
        &client,
        &server,
        reqwest::Method::POST,
        "/v1/node/invitations",
        node(&f),
        Some(json!({"workspace_id":"x"})),
    )
    .await;
    assert_eq!(status, StatusCode::BAD_REQUEST);
    let (status, _) = call(
        &client,
        &server,
        reqwest::Method::DELETE,
        "/v1/node/devices/not-a-device",
        node(&f),
        None,
    )
    .await;
    assert_eq!(status, StatusCode::NOT_FOUND);
    server.stop().await;
    f.close().await;
}

#[tokio::test]
async fn invitation_creation_is_rate_limited_per_computer() {
    let f = Fixture::new().await;
    let server = Server::start(&f.store).await;
    let client = Client::new();
    for _ in 0..10 {
        assert_eq!(invite(&client, &server, node(&f)).await.0, StatusCode::OK);
    }
    assert_eq!(
        invite(&client, &server, node(&f)).await.0,
        StatusCode::TOO_MANY_REQUESTS
    );
    let other = f
        .store
        .node(&f.node.workspace, "Second computer")
        .await
        .unwrap();
    assert_eq!(
        invite(&client, &server, s(&other, "node_token")).await.0,
        StatusCode::OK
    );
    server.stop().await;
    f.close().await;
}

#[tokio::test]
async fn phones_paired_before_these_fields_report_them_as_unknown() {
    let f = Fixture::new().await;
    let id = uuid::Uuid::new_v4().to_string();
    f.store
        .db
        .exec(
            "INSERT INTO devices(id,workspace_id,name,token_hash) VALUES(?,?,?,?)",
            args![
                &id,
                &f.node.workspace,
                "Older phone",
                &digest("synthetic-older-token")
            ],
        )
        .await
        .unwrap();
    let older = Credential {
        id: id.clone(),
        workspace: f.node.workspace.clone(),
        role: Role::Device,
    };
    f.store.authorized(&older).await.unwrap();
    let server = Server::start(&f.store).await;
    let list = devices(&Client::new(), &server, node(&f)).await;
    let row = device(&list, &id).unwrap();
    assert_eq!(
        (
            row["paired_at"].clone(),
            row["last_seen_at"].clone(),
            row["added_by"].clone(),
            row["platform"].clone()
        ),
        (Value::Null, Value::Null, Value::Null, Value::Null)
    );
    server.stop().await;
    f.close().await;
}
