//! Public enrollment/opaque transport is exercised with real device keys on a
//! private backend. PostgreSQL runs also prove shared-worker reservation.
use reqwest::{Client, StatusCode};
use serde_json::{json, Value};
use uuid::Uuid;
use zerus_mobile_crypto::{
    self as crypto, EncryptedEnvelope, EnrollmentCandidate, Header, Identity, Invitation,
    MembershipAction, MembershipState, MembershipStatement, OperationClass, PayloadKind,
    Role as DeviceRole, SignedEnrollment, SignedMembership, SignedMembershipSnapshot,
};
use zerus_relay::{
    args,
    config::Config,
    db::{i, now, s},
    store::{token, Credential, Role, ServiceClass, Store},
};
mod common;
mod support;
use common::Backend;
use support::Server;
fn id() -> String {
    Uuid::new_v4().to_string()
}
fn config() -> Config {
    Config {
        public_enrollment: true,
        large_payload_slots: 2,
        max_spool_bytes: 164 * 1024 * 1024,
        background: false,
        ..Config::default()
    }
}
fn genesis(w: &str, identity: &Identity) -> SignedMembership {
    SignedMembership::sign(
        MembershipStatement {
            workspace_id: w.into(),
            revision: 0,
            previous_head: crypto::encode(&[0; 32]),
            issuer_device_id: identity.public().device_id.clone(),
            issuer_key_generation: 1,
            action: MembershipAction::Add,
            device: Some(identity.public().clone()),
            revoked_device_id: None,
        },
        identity,
    )
    .unwrap()
}
async fn create(store: &Store) -> (Identity, String, MembershipState, Credential) {
    let identity = Identity::generate(id(), DeviceRole::Computer).unwrap();
    let w = id();
    let membership = genesis(&w, &identity);
    let secret = token();
    let body = json!({"membership":membership,"service_token":secret});
    let result = store.create_workspace(&body).await.unwrap();
    assert_eq!(result, store.create_workspace(&body).await.unwrap());
    let credential = store.authenticate(&secret, Role::Node).await.unwrap();
    (
        identity,
        secret,
        MembershipState::from_genesis(&membership).unwrap(),
        credential,
    )
}
async fn retained_bytes(store: &Store, c: &Credential) -> i64 {
    let row = store
        .db
        .one(
            "SELECT payload_bytes FROM workspace_usage WHERE workspace_id=?",
            args![&c.workspace],
        )
        .await
        .unwrap()
        .unwrap();
    i(&row, "payload_bytes")
}
async fn enroll(
    store: &Store,
    issuer: &Identity,
    c: &Credential,
    state: &mut MembershipState,
    role: DeviceRole,
) -> (Identity, String, Credential) {
    let invite = Invitation::generate(
        c.workspace.clone(),
        id(),
        role,
        issuer.public().clone(),
        now() as u64 + 590,
        state.revision,
        state.head.clone(),
    )
    .unwrap();
    let bootstrap = SignedMembershipSnapshot::sign(state.clone(), issuer).unwrap();
    store.computer_invitation(c,&json!({"invitation_id":invite.invitation_id,"purpose":role,"expires_at":invite.expires_at,"bootstrap":bootstrap})).await.unwrap();
    let identity = Identity::generate(id(), role).unwrap();
    let candidate = EnrollmentCandidate::create(&invite, &identity, now() as u64).unwrap();
    candidate.verify_invitation(&invite, now() as u64).unwrap();
    let secret = token();
    let body = json!({"candidate":candidate,"service_token":secret});
    let pending = store.join_workspace(&body).await.unwrap();
    assert_eq!(pending["state"], "pending");
    assert_eq!(pending, store.join_workspace(&body).await.unwrap());
    assert!(store
        .authenticate(
            &secret,
            if role == DeviceRole::Phone {
                Role::Device
            } else {
                Role::Node
            }
        )
        .await
        .is_err());
    let membership = SignedMembership::sign(
        MembershipStatement {
            workspace_id: c.workspace.clone(),
            revision: state.revision + 1,
            previous_head: state.head.clone(),
            issuer_device_id: c.id.clone(),
            issuer_key_generation: 1,
            action: MembershipAction::Add,
            device: Some(identity.public().clone()),
            revoked_device_id: None,
        },
        issuer,
    )
    .unwrap();
    let approval = SignedEnrollment::sign(&candidate, membership.clone(), issuer).unwrap();
    store
        .approve_enrollment(
            c,
            &identity.public().device_id,
            &json!({"enrollment":approval}),
        )
        .await
        .unwrap();
    state.apply_next(&membership).unwrap();
    let status = store
        .enrollment_status(&identity.public().device_id, &secret)
        .await
        .unwrap();
    assert_eq!(status["state"], "active");
    assert_eq!(status["bootstrap"], json!(bootstrap));
    assert_eq!(status["enrollment"], json!(approval));
    let credential = store
        .authenticate(
            &secret,
            if role == DeviceRole::Phone {
                Role::Device
            } else {
                Role::Node
            },
        )
        .await
        .unwrap();
    (identity, secret, credential)
}
fn seal(
    sender: &Identity,
    recipient: &Identity,
    state: &MembershipState,
    gateway: &str,
    request: &str,
    kind: PayloadKind,
    text: &[u8],
) -> EncryptedEnvelope {
    crypto::seal(
        sender,
        recipient.public(),
        Header {
            workspace_id: state.workspace_id.clone(),
            sender_device_id: sender.public().device_id.clone(),
            recipient_device_id: recipient.public().device_id.clone(),
            sender_key_generation: 1,
            recipient_key_generation: 1,
            gateway_id: gateway.into(),
            route_id: gateway.into(),
            request_id: request.into(),
            kind,
            operation_class: OperationClass::Mutation,
            expires_at: now() as u64 + 120,
            membership_revision: state.revision,
            membership_head: state.head.clone(),
        },
        text,
    )
    .unwrap()
}
async fn http(
    client: &Client,
    server: &Server,
    path: &str,
    secret: &str,
    body: &Value,
) -> (StatusCode, Value) {
    let r = client
        .post(server.at(path))
        .bearer_auth(secret)
        .json(body)
        .send()
        .await
        .unwrap();
    let status = r.status();
    (status, r.json().await.unwrap())
}
#[tokio::test]
async fn typed_two_phase_enrollment_and_encrypted_roundtrip_keep_no_private_text() {
    let backend = Backend::new(config()).await;
    let store = backend.store().await;
    let (computer, node_token, mut state, node) = create(&store).await;
    let (phone, device_token, device) =
        enroll(&store, &computer, &node, &mut state, DeviceRole::Phone).await;
    let server = Server::start(&store).await;
    let client = Client::new();
    let plain = b"Synthetic private command /private/example.txt";
    let request_id = id();
    let envelope = seal(
        &phone,
        &computer,
        &state,
        &node.id,
        &request_id,
        PayloadKind::Request,
        plain,
    );
    let submitted = http(
        &client,
        &server,
        "/v2/requests",
        &device_token,
        &json!({"envelope":envelope}),
    )
    .await;
    assert_eq!(submitted.0, StatusCode::ACCEPTED, "{}", submitted.1);
    let claim: Value = client
        .get(server.at("/v2/node/requests"))
        .bearer_auth(&node_token)
        .send()
        .await
        .unwrap()
        .json()
        .await
        .unwrap();
    let claimed: EncryptedEnvelope = serde_json::from_value(claim["requests"][0].clone()).unwrap();
    assert_eq!(
        &*crypto::open(&computer, phone.public(), &envelope.header, &claimed).unwrap(),
        plain
    );
    let empty = store.encrypted_claim(&node).await.unwrap();
    assert!(empty["requests"].as_array().unwrap().is_empty());
    let result = seal(
        &computer,
        &phone,
        &state,
        &node.id,
        &request_id,
        PayloadKind::Result,
        b"Synthetic private native result",
    );
    let path = format!("/v2/node/requests/{request_id}/result");
    assert_eq!(
        http(
            &client,
            &server,
            &path,
            &node_token,
            &json!({"envelope":result})
        )
        .await
        .0,
        StatusCode::OK
    );
    let receipt = store.encrypted_receipt(&device, &request_id).await.unwrap();
    assert_eq!(receipt["state"], "completed");
    assert_eq!(receipt["envelope"], json!(result));
    let stored = store
        .db
        .one(
            "SELECT body,result FROM encrypted_requests WHERE id=?",
            args![&request_id],
        )
        .await
        .unwrap()
        .unwrap();
    assert!(!s(&stored, "body").contains("Synthetic private"));
    assert!(!s(&stored, "result").contains("Synthetic private"));
    // Exact ciphertext retry recovers the completed receipt; changing a byte is
    // a conflict or an invalid signature, never a new queued native operation.
    assert_eq!(
        http(
            &client,
            &server,
            "/v2/requests",
            &device_token,
            &json!({"envelope":envelope})
        )
        .await
        .1["state"],
        "completed"
    );
    let mut changed = envelope.clone();
    changed.header.route_id = id();
    assert_eq!(
        http(
            &client,
            &server,
            "/v2/requests",
            &device_token,
            &json!({"envelope":changed})
        )
        .await
        .0,
        StatusCode::UNAUTHORIZED
    );
    assert_eq!(
        client
            .get(server.at("/v1/computers"))
            .bearer_auth(&device_token)
            .send()
            .await
            .unwrap()
            .status(),
        StatusCode::CONFLICT
    );
    let capabilities: Value = client
        .get(server.at("/v2/capabilities"))
        .send()
        .await
        .unwrap()
        .json()
        .await
        .unwrap();
    assert_eq!(capabilities["encrypted_transport"]["version"], 1);
    server.stop().await;
    store.db.close().await;
}
#[tokio::test]
async fn member_changes_are_cas_ordered_and_revocation_cannot_replay_a_claim() {
    let backend = Backend::new(config()).await;
    let store = backend.store().await;
    let (computer, _secret, mut state, node) = create(&store).await;
    let (phone, secret, device) =
        enroll(&store, &computer, &node, &mut state, DeviceRole::Phone).await;
    let before = state.clone();
    let request_id = id();
    let e = seal(
        &phone,
        &computer,
        &state,
        &node.id,
        &request_id,
        PayloadKind::Request,
        b"private mutation",
    );
    let prepared =
        zerus_relay::store::EncryptedSubmission::new(json!({"envelope":e}), phone.public().clone())
            .unwrap();
    store.encrypted_submit(&device, &prepared).await.unwrap();
    store.encrypted_claim(&node).await.unwrap();
    let revoke = SignedMembership::sign(
        MembershipStatement {
            workspace_id: state.workspace_id.clone(),
            revision: state.revision + 1,
            previous_head: state.head.clone(),
            issuer_device_id: node.id.clone(),
            issuer_key_generation: 1,
            action: MembershipAction::Revoke,
            device: None,
            revoked_device_id: Some(device.id.clone()),
        },
        &computer,
    )
    .unwrap();
    store
        .revoke_membership(&node, &json!({"membership":revoke}), false)
        .await
        .unwrap();
    state.apply_next(&revoke).unwrap();
    assert!(store.authenticate(&secret, Role::Device).await.is_err());
    assert!(store.encrypted_claim(&node).await.unwrap()["requests"]
        .as_array()
        .unwrap()
        .is_empty());
    let row=store.db.one("SELECT state,body_hash,body,body_bytes,reserved_bytes FROM encrypted_requests WHERE id=?",args![&request_id]).await.unwrap().unwrap();
    assert_eq!(row["state"], "uncertain");
    assert!(!s(&row, "body_hash").is_empty());
    assert_eq!(row["body"], "");
    let chunks = store
        .db
        .one(
            "SELECT count(*) AS count FROM encrypted_request_chunks WHERE request_id=?",
            args![&request_id],
        )
        .await
        .unwrap()
        .unwrap();
    assert_eq!(i(&chunks, "count"), 0);
    let stale = SignedMembership::sign(
        MembershipStatement {
            workspace_id: before.workspace_id.clone(),
            revision: before.revision + 1,
            previous_head: before.head.clone(),
            issuer_device_id: node.id.clone(),
            issuer_key_generation: 1,
            action: MembershipAction::Revoke,
            device: None,
            revoked_device_id: Some(node.id.clone()),
        },
        &computer,
    )
    .unwrap();
    assert_eq!(
        store
            .revoke_membership(&node, &json!({"membership":stale}), true)
            .await
            .unwrap_err()
            .0,
        StatusCode::CONFLICT
    );
    let changes = store.membership_changes(&node, -1, 1).await.unwrap();
    assert_eq!(changes["changes"].as_array().unwrap().len(), 1);
    assert_eq!(changes["next_after"], 0);
    // Last computer may explicitly retire, leaving no implicit recovery authority.
    let leave = SignedMembership::sign(
        MembershipStatement {
            workspace_id: state.workspace_id.clone(),
            revision: state.revision + 1,
            previous_head: state.head.clone(),
            issuer_device_id: node.id.clone(),
            issuer_key_generation: 1,
            action: MembershipAction::Revoke,
            device: None,
            revoked_device_id: Some(node.id.clone()),
        },
        &computer,
    )
    .unwrap();
    store
        .revoke_membership(&node, &json!({"membership":leave}), true)
        .await
        .unwrap();
    store.db.close().await;
}
#[tokio::test]
async fn opaque_signals_coalesce_and_keep_private_names_out_of_events() {
    let backend = Backend::new(config()).await;
    let store = backend.store().await;
    let (computer, _secret, mut state, node) = create(&store).await;
    let (_phone, _secret, device) =
        enroll(&store, &computer, &node, &mut state, DeviceRole::Phone).await;
    let scope = crypto::encode(&[1; 32]);
    let change = crypto::encode(&[2; 32]);
    let signal = json!({"presence":{"idle_seconds":0,"locked":false},"delivery":"immediate","signals":[{"scope_id":scope,"kind":"input","change_id":change,"active":true}]});
    store.encrypted_signals(&node, &signal).await.unwrap();
    store.encrypted_signals(&node, &signal).await.unwrap();
    let events = store.encrypted_events(&device, 0).await.unwrap();
    assert_eq!(events["events"].as_array().unwrap().len(), 1);
    assert_eq!(events["events"][0]["scope_id"], scope);
    assert_eq!(events["events"][0]["alert_kind"], "input");
    let mut inactive = signal.clone();
    inactive["signals"][0]["active"] = json!(false);
    store.encrypted_signals(&node, &inactive).await.unwrap();
    store.encrypted_signals(&node, &signal).await.unwrap();
    assert_eq!(
        store.encrypted_events(&device, 0).await.unwrap()["events"]
            .as_array()
            .unwrap()
            .len(),
        2
    );
    store.db.close().await;
}
#[tokio::test]
async fn complete_signals_cancel_held_scopes_and_unknown_presence_delivers_reactivation() {
    let backend = Backend::new(config()).await;
    let store = backend.store().await;
    let (computer, _, mut state, node) = create(&store).await;
    let (_, _, phone) = enroll(&store, &computer, &node, &mut state, DeviceRole::Phone).await;
    let scope = crypto::encode(&[3; 32]);
    let change = crypto::encode(&[4; 32]);
    let active = json!({"complete_route":node.id,"presence":{"idle_seconds":0,"locked":false},"delivery":"away_5","signals":[{"scope_id":scope,"kind":"input","change_id":change,"active":true}]});
    store.encrypted_signals(&node, &active).await.unwrap();
    assert!(store.encrypted_events(&phone, 0).await.unwrap()["events"]
        .as_array()
        .unwrap()
        .is_empty());
    let cleared =
        json!({"complete_route":node.id,"presence":null,"delivery":"away_5","signals":[]});
    store.encrypted_signals(&node, &cleared).await.unwrap();
    assert!(store.encrypted_events(&phone, 0).await.unwrap()["events"]
        .as_array()
        .unwrap()
        .is_empty());
    let mut reactivated = active.clone();
    reactivated["presence"] = Value::Null;
    store.encrypted_signals(&node, &reactivated).await.unwrap();
    assert_eq!(
        store.encrypted_events(&phone, 0).await.unwrap()["events"]
            .as_array()
            .unwrap()
            .len(),
        1
    );
    store.encrypted_signals(&node, &cleared).await.unwrap();
    store.encrypted_signals(&node, &reactivated).await.unwrap();
    assert_eq!(
        store.encrypted_events(&phone, 0).await.unwrap()["events"]
            .as_array()
            .unwrap()
            .len(),
        2
    );
    store.db.close().await;
}
#[tokio::test]
async fn public_mode_requires_capacity_for_standard_and_reserved_heavy_work() {
    let mut c = config();
    c.large_payload_slots = 1;
    assert!(c.validate().is_err());
    c.large_payload_slots = 2;
    c.validate().unwrap();
    c.max_spool_bytes = 64 * 1024 * 1024;
    assert!(c.validate().is_err());
}
#[tokio::test]
async fn standard_capacity_is_shared_and_team_can_use_its_reserved_resources() {
    let backend = Backend::new(config()).await;
    let store = backend.store().await;
    let second = if backend.is_postgres() {
        Some(backend.store().await)
    } else {
        None
    };
    let peer = second.as_ref().unwrap_or(&store);
    for (lane, total) in store.db.cfg.service_lanes() {
        let standard = store.db.cfg.standard_capacity(total);
        let a = store
            .acquire_service(lane, ServiceClass::Standard, standard)
            .await
            .unwrap();
        assert!(
            peer.acquire_service(lane, ServiceClass::Standard, 1)
                .await
                .is_err(),
            "{lane}"
        );
        let b = peer
            .acquire_service(lane, ServiceClass::Team, total - standard)
            .await
            .unwrap();
        assert!(
            store
                .acquire_service(lane, ServiceClass::Team, 1)
                .await
                .is_err(),
            "{lane}"
        );
        drop((a, b));
        assert!(
            support::eventually(|| async {
                store
                    .db
                    .one(
                        "SELECT used_total FROM service_resources WHERE lane=?",
                        args![lane],
                    )
                    .await
                    .unwrap()
                    .is_some_and(|r| i(&r, "used_total") == 0)
            })
            .await
        );
    }
    if let Some(second) = second {
        second.db.close().await;
    }
    store.db.close().await;
}

#[tokio::test]
async fn route_manifests_page_native_targets_and_never_rebind_withdrawn_ids() {
    let backend = Backend::new(config()).await;
    let store = backend.store().await;
    let (computer, _secret, mut state, node) = create(&store).await;
    let (phone, _secret, device) =
        enroll(&store, &computer, &node, &mut state, DeviceRole::Phone).await;
    let local = id();
    let peer = id();
    let peer_route = id();
    store.encrypted_routes_manifest(&node,&json!({"routes":[{"route_id":node.id,"computer_id":local},{"route_id":peer_route,"computer_id":peer}]})).await.unwrap();
    for (route, native) in [(&node.id, &local), (&peer_route, &peer)] {
        let mut header = seal(
            &computer,
            &phone,
            &state,
            &node.id,
            &id(),
            PayloadKind::Snapshot,
            b"private catalog",
        )
        .header;
        header.route_id = route.clone();
        let envelope = crypto::seal(&computer, phone.public(), header, b"private catalog").unwrap();
        let prepared = zerus_relay::store::EncryptedSubmission::new(
            json!({"computer_id":native,"envelope":envelope}),
            computer.public().clone(),
        )
        .unwrap();
        store.encrypted_snapshot(&node, &prepared).await.unwrap();
    }
    let first = store.encrypted_computers(&device, "", 1).await.unwrap();
    let after = s(&first, "next_after");
    assert!(!after.is_empty());
    let second = store.encrypted_computers(&device, after, 1).await.unwrap();
    assert_ne!(
        first["computers"][0]["computer_id"],
        second["computers"][0]["computer_id"]
    );
    assert_eq!(second["next_after"], Value::Null);
    store
        .encrypted_routes_manifest(
            &node,
            &json!({"routes":[{"route_id":node.id,"computer_id":local}]}),
        )
        .await
        .unwrap();
    assert_eq!(
        store.encrypted_computers(&device, "", 100).await.unwrap()["computers"]
            .as_array()
            .unwrap()
            .len(),
        1
    );
    let changed = json!({"routes":[{"route_id":peer_route,"computer_id":id()}]});
    assert_eq!(
        store
            .encrypted_routes_manifest(&node, &changed)
            .await
            .unwrap_err()
            .0,
        StatusCode::CONFLICT
    );
    assert_eq!(
        store.encrypted_routes(&node).await.unwrap()["routes"]
            .as_array()
            .unwrap()
            .len(),
        1
    );
    store.db.close().await;
}
#[tokio::test]
async fn immutable_results_survive_an_unrelated_membership_change_and_expired_reads_stay_receipts()
{
    let backend = Backend::new(config()).await;
    let store = backend.store().await;
    let (computer, _secret, mut state, node) = create(&store).await;
    let (phone, _secret, device) =
        enroll(&store, &computer, &node, &mut state, DeviceRole::Phone).await;
    let request_id = id();
    let e = seal(
        &phone,
        &computer,
        &state,
        &node.id,
        &request_id,
        PayloadKind::Request,
        b"read request",
    );
    let prepared =
        zerus_relay::store::EncryptedSubmission::new(json!({"envelope":e}), phone.public().clone())
            .unwrap();
    store.encrypted_submit(&device, &prepared).await.unwrap();
    store.encrypted_claim(&node).await.unwrap();
    let result = seal(
        &computer,
        &phone,
        &state,
        &node.id,
        &request_id,
        PayloadKind::Result,
        b"retained private result",
    );
    let result = zerus_relay::store::EncryptedSubmission::new(
        json!({"envelope":result}),
        computer.public().clone(),
    )
    .unwrap();
    let (_other, _secret, _device) =
        enroll(&store, &computer, &node, &mut state, DeviceRole::Phone).await;
    store
        .encrypted_result(&node, &request_id, &result)
        .await
        .unwrap();
    store
        .db
        .exec(
            "UPDATE encrypted_requests SET expires=0 WHERE id=?",
            args![&request_id],
        )
        .await
        .unwrap();
    store.maintain_encrypted().await.unwrap();
    let retried = store.encrypted_submit(&device, &prepared).await.unwrap().1;
    assert_eq!(retried["state"], "completed");
    assert_eq!(retried["envelope"], Value::Null);
    assert!(store.encrypted_claim(&node).await.unwrap()["requests"]
        .as_array()
        .unwrap()
        .is_empty());
    store.db.close().await;
}
#[tokio::test]
async fn permanent_metadata_stays_charged_after_ciphertext_cleanup_and_route_withdrawal() {
    let backend = Backend::new(config()).await;
    let store = backend.store().await;
    let (computer, _, mut state, node) = create(&store).await;
    let (phone, _, device) = enroll(&store, &computer, &node, &mut state, DeviceRole::Phone).await;
    let baseline = retained_bytes(&store, &node).await;
    let manifest = json!({"routes":[{"route_id":node.id,"computer_id":id()}]});
    store
        .encrypted_routes_manifest(&node, &manifest)
        .await
        .unwrap();
    store
        .encrypted_routes_manifest(&node, &manifest)
        .await
        .unwrap();
    assert_eq!(retained_bytes(&store, &node).await, baseline + 1024);
    let signal = json!({"presence":null,"delivery":"immediate","signals":[{"scope_id":crypto::encode(&[8;32]),"kind":"input","change_id":crypto::encode(&[9;32]),"active":true}]});
    store.encrypted_signals(&node, &signal).await.unwrap();
    store.encrypted_signals(&node, &signal).await.unwrap();
    assert_eq!(retained_bytes(&store, &node).await, baseline + 2048);
    let request_id = id();
    let request = seal(
        &phone,
        &computer,
        &state,
        &node.id,
        &request_id,
        PayloadKind::Request,
        b"private request",
    );
    let prepared = zerus_relay::store::EncryptedSubmission::new(
        json!({"envelope":request}),
        phone.public().clone(),
    )
    .unwrap();
    store.encrypted_submit(&device, &prepared).await.unwrap();
    let admitted = retained_bytes(&store, &node).await;
    store.encrypted_submit(&device, &prepared).await.unwrap();
    assert_eq!(retained_bytes(&store, &node).await, admitted);
    store
        .db
        .exec(
            "UPDATE encrypted_requests SET expires=0 WHERE id=?",
            args![&request_id],
        )
        .await
        .unwrap();
    store.maintain_encrypted().await.unwrap();
    assert_eq!(retained_bytes(&store, &node).await, baseline + 3072);
    store
        .withdraw_encrypted_route(&node, &node.id)
        .await
        .unwrap();
    store
        .withdraw_encrypted_route(&node, &node.id)
        .await
        .unwrap();
    assert_eq!(retained_bytes(&store, &node).await, baseline + 2048);
    let row = store
        .db
        .one(
            "SELECT metadata_bytes FROM encrypted_requests WHERE id=?",
            args![&request_id],
        )
        .await
        .unwrap()
        .unwrap();
    assert_eq!(i(&row, "metadata_bytes"), 1024);
    let row = store
        .db
        .one(
            "SELECT metadata_bytes FROM encrypted_route_bindings WHERE node_id=? AND route_id=?",
            args![&node.id, &node.id],
        )
        .await
        .unwrap()
        .unwrap();
    assert_eq!(i(&row, "metadata_bytes"), 1024);
    let revoked = SignedMembership::sign(
        MembershipStatement {
            workspace_id: state.workspace_id.clone(),
            revision: state.revision + 1,
            previous_head: state.head.clone(),
            issuer_device_id: node.id.clone(),
            issuer_key_generation: 1,
            action: MembershipAction::Revoke,
            device: None,
            revoked_device_id: Some(device.id.clone()),
        },
        &computer,
    )
    .unwrap();
    store
        .revoke_membership(&node, &json!({"membership":revoked}), false)
        .await
        .unwrap();
    let row = store
        .db
        .one(
            "SELECT metadata_bytes FROM encrypted_requests WHERE id=?",
            args![&request_id],
        )
        .await
        .unwrap()
        .unwrap();
    assert_eq!(i(&row, "metadata_bytes"), 1024);
    store.db.close().await;
}

#[tokio::test]
async fn startup_adopts_retained_metadata_once_without_raising_a_full_workspaces_quota() {
    let backend = Backend::new(config()).await;
    let store = backend.store().await;
    let (computer, _, mut state, node) = create(&store).await;
    let (phone, _, device) = enroll(&store, &computer, &node, &mut state, DeviceRole::Phone).await;
    store
        .encrypted_routes_manifest(
            &node,
            &json!({"routes":[{"route_id":node.id,"computer_id":id()}]}),
        )
        .await
        .unwrap();
    let signal = json!({"presence":null,"delivery":"immediate","signals":[{"scope_id":crypto::encode(&[10;32]),"kind":"input","change_id":crypto::encode(&[11;32]),"active":true}]});
    store.encrypted_signals(&node, &signal).await.unwrap();
    let request_id = id();
    let request = seal(
        &phone,
        &computer,
        &state,
        &node.id,
        &request_id,
        PayloadKind::Request,
        b"private request",
    );
    let prepared = zerus_relay::store::EncryptedSubmission::new(
        json!({"envelope":request}),
        phone.public().clone(),
    )
    .unwrap();
    store.encrypted_submit(&device, &prepared).await.unwrap();
    store
        .db
        .exec(
            "UPDATE encrypted_requests SET expires=0 WHERE id=?",
            args![&request_id],
        )
        .await
        .unwrap();
    store.maintain_encrypted().await.unwrap();
    // Simulate an older public database whose payload quota is almost full
    // and whose three retained metadata records were never billed.
    let previous = store.db.cfg.max_queue_bytes - 1024;
    let mut t = store.db.begin().await.unwrap();
    let usage = store
        .db
        .payload_usage(&mut t, &node.workspace)
        .await
        .unwrap();
    for table in [
        "encrypted_requests",
        "encrypted_route_bindings",
        "opaque_signals",
    ] {
        t.exec(
            &format!("UPDATE {table} SET metadata_bytes=0 WHERE workspace_id=?"),
            args![&node.workspace],
        )
        .await
        .unwrap();
    }
    store
        .db
        .bytes(
            &mut t,
            &node.workspace,
            previous - i(&usage, "payload_bytes"),
        )
        .await
        .unwrap();
    t.commit().await.unwrap();
    store.db.close().await;
    for _ in 0..2 {
        let reopened = backend.store().await;
        assert_eq!(retained_bytes(&reopened, &node).await, previous + 3072);
        for table in [
            "encrypted_requests",
            "encrypted_route_bindings",
            "opaque_signals",
        ] {
            let row=reopened.db.one(&format!("SELECT count(*) AS count FROM {table} WHERE workspace_id=? AND metadata_bytes=1024"),args![&node.workspace]).await.unwrap().unwrap();
            assert_eq!(i(&row, "count"), 1);
        }
        let exact = reopened.encrypted_submit(&device, &prepared).await.unwrap();
        assert_eq!(exact.0, StatusCode::ACCEPTED);
        let fresh = seal(
            &phone,
            &computer,
            &state,
            &node.id,
            &id(),
            PayloadKind::Request,
            b"another private request",
        );
        let fresh = zerus_relay::store::EncryptedSubmission::new(
            json!({"envelope":fresh}),
            phone.public().clone(),
        )
        .unwrap();
        assert_eq!(
            reopened
                .encrypted_submit(&device, &fresh)
                .await
                .unwrap_err()
                .0,
            StatusCode::TOO_MANY_REQUESTS
        );
        assert_eq!(retained_bytes(&reopened, &node).await, previous + 3072);
        reopened.db.close().await;
    }
}

#[tokio::test]
async fn expired_frozen_result_is_terminal_only_after_exact_authenticated_routing() {
    let backend = Backend::new(config()).await;
    let store = backend.store().await;
    let (computer, _, mut state, node) = create(&store).await;
    let (phone, _, device) = enroll(&store, &computer, &node, &mut state, DeviceRole::Phone).await;
    let request_id = id();
    let request = seal(
        &phone,
        &computer,
        &state,
        &node.id,
        &request_id,
        PayloadKind::Request,
        b"private request",
    );
    let prepared = zerus_relay::store::EncryptedSubmission::new(
        json!({"envelope":request}),
        phone.public().clone(),
    )
    .unwrap();
    store.encrypted_submit(&device, &prepared).await.unwrap();
    store.encrypted_claim(&node).await.unwrap();
    let result = seal(
        &computer,
        &phone,
        &state,
        &node.id,
        &request_id,
        PayloadKind::Result,
        b"private outcome",
    );
    let mut header = result.header.clone();
    header.expires_at = now() as u64 - 1;
    let expired = crypto::seal(
        &computer,
        phone.public(),
        header.clone(),
        b"private outcome",
    )
    .unwrap();
    let expired = zerus_relay::store::EncryptedSubmission::new(
        json!({"envelope":expired}),
        computer.public().clone(),
    )
    .unwrap();
    assert_eq!(
        store
            .encrypted_result(&node, &request_id, &expired)
            .await
            .unwrap_err()
            .0,
        StatusCode::GONE
    );
    header.route_id = id();
    let misrouted = crypto::seal(&computer, phone.public(), header, b"private outcome").unwrap();
    let misrouted = zerus_relay::store::EncryptedSubmission::new(
        json!({"envelope":misrouted}),
        computer.public().clone(),
    )
    .unwrap();
    assert_eq!(
        store
            .encrypted_result(&node, &request_id, &misrouted)
            .await
            .unwrap_err()
            .0,
        StatusCode::CONFLICT
    );
    assert_eq!(
        store.encrypted_receipt(&device, &request_id).await.unwrap()["state"],
        "claimed"
    );
    assert!(store.encrypted_claim(&node).await.unwrap()["requests"]
        .as_array()
        .unwrap()
        .is_empty());
    store.db.close().await;
}
#[tokio::test]
async fn self_retirement_http_receipt_is_recoverable_with_only_its_exact_signed_change() {
    let backend = Backend::new(config()).await;
    let store = backend.store().await;
    let (computer, secret, state, node) = create(&store).await;
    let server = Server::start(&store).await;
    let client = Client::new();
    let retirement = SignedMembership::sign(
        MembershipStatement {
            workspace_id: state.workspace_id,
            revision: 1,
            previous_head: state.head,
            issuer_device_id: node.id.clone(),
            issuer_key_generation: 1,
            action: MembershipAction::Revoke,
            device: None,
            revoked_device_id: Some(node.id.clone()),
        },
        &computer,
    )
    .unwrap();
    let body = json!({"membership":retirement});
    for _ in 0..2 {
        let response = client
            .delete(server.at("/v2/node/self"))
            .bearer_auth(&secret)
            .json(&body)
            .send()
            .await
            .unwrap();
        assert_eq!(
            response.status(),
            StatusCode::OK,
            "{}",
            response.text().await.unwrap()
        );
    }
    assert_eq!(
        client
            .get(server.at("/v2/node/routes"))
            .bearer_auth(&secret)
            .send()
            .await
            .unwrap()
            .status(),
        StatusCode::UNAUTHORIZED
    );
    let history: Value = client
        .get(server.at("/v2/node/membership"))
        .bearer_auth(&secret)
        .send()
        .await
        .unwrap()
        .json()
        .await
        .unwrap();
    assert_eq!(history["membership_revision"], 1);
    assert_eq!(history["changes"].as_array().unwrap().len(), 2);
    let mut changed = body;
    changed["membership"]["statement"]["revision"] = json!(2);
    assert_ne!(
        client
            .delete(server.at("/v2/node/self"))
            .bearer_auth(&secret)
            .json(&changed)
            .send()
            .await
            .unwrap()
            .status(),
        StatusCode::OK
    );
    server.stop().await;
    store.db.close().await;
}

#[tokio::test]
async fn legacy_self_retirement_preserves_only_one_exact_receipt() {
    let f = common::Fixture::new().await;
    let server = Server::start(&f.store).await;
    let client = Client::new();
    let secret = s(&f.credentials, "node_token");
    let body = json!({"request_id":id()});
    let first: Value = client
        .delete(server.at("/v1/node/self"))
        .bearer_auth(secret)
        .json(&body)
        .send()
        .await
        .unwrap()
        .json()
        .await
        .unwrap();
    assert_eq!(
        first,
        json!({"retired":true,"request_id":body["request_id"],"node_id":f.node.id})
    );
    let retry: Value = client
        .delete(server.at("/v1/node/self"))
        .bearer_auth(secret)
        .json(&body)
        .send()
        .await
        .unwrap()
        .json()
        .await
        .unwrap();
    assert_eq!(retry, first);
    assert_eq!(
        client
            .delete(server.at("/v1/node/self"))
            .bearer_auth(secret)
            .json(&json!({"request_id":id()}))
            .send()
            .await
            .unwrap()
            .status(),
        StatusCode::UNAUTHORIZED
    );
    assert_eq!(
        client
            .get(server.at("/v1/node/devices"))
            .bearer_auth(secret)
            .send()
            .await
            .unwrap()
            .status(),
        StatusCode::UNAUTHORIZED
    );
    assert!(f.store.authenticate(secret, Role::Node).await.is_err());
    server.stop().await;
    f.close().await;
}
#[tokio::test]
async fn team_authentication_uses_a_separate_bounded_control_pool() {
    if std::env::var_os(common::DATABASE_URL).is_none() {
        return;
    }
    let backend = Backend::postgres(config()).await;
    let store = backend.store().await;
    let (_computer, secret, _state, node) = create(&store).await;
    store
        .set_service_class(&node.workspace, ServiceClass::Team)
        .await
        .unwrap();
    assert_eq!(store.db.auth_class(&secret), ServiceClass::Team);
    let first = store.db.control(ServiceClass::Standard).await.unwrap();
    let second = store.db.control(ServiceClass::Standard).await.unwrap();
    assert!(tokio::time::timeout(
        std::time::Duration::from_millis(100),
        store.authenticate(&token(), Role::Node)
    )
    .await
    .is_err());
    let authenticated = tokio::time::timeout(
        std::time::Duration::from_secs(1),
        store.authenticate(&secret, Role::Node),
    )
    .await
    .unwrap()
    .unwrap();
    assert_eq!(authenticated.id, node.id);
    // A cached priority hash never overrides live revocation.
    store
        .db
        .exec("UPDATE nodes SET revoked=1 WHERE id=?", args![&node.id])
        .await
        .unwrap();
    assert!(store.authenticate(&secret, Role::Node).await.is_err());
    first.commit().await.unwrap();
    second.commit().await.unwrap();
    store.db.close().await;
}

/// Artifact generator for tests/public_load.py. Keys and source bytes remain
/// outside the measured relay cgroup and are removed with its private directory.
#[test]
#[ignore = "private fixture generator; invoked only by disposable public_load.py"]
fn generate_public_load_fixtures() {
    use std::{fs::OpenOptions, os::unix::fs::OpenOptionsExt};
    let directory = std::path::PathBuf::from(
        std::env::var_os("ZERUS_RELAY_PUBLIC_FIXTURE_DIR").expect("private fixture directory"),
    );
    let write = |name: &str, value: &Value| {
        let file = OpenOptions::new()
            .create_new(true)
            .write(true)
            .mode(0o600)
            .open(directory.join(name))
            .unwrap();
        serde_json::to_writer(file, value).unwrap();
    };
    let mut metadata = serde_json::Map::new();
    for lane in ["standard", "team"] {
        let computer = Identity::generate(id(), DeviceRole::Computer).unwrap();
        let phone = Identity::generate(id(), DeviceRole::Phone).unwrap();
        let workspace = id();
        let node_secret = token();
        let device_secret = token();
        let genesis = genesis(&workspace, &computer);
        let mut state = MembershipState::from_genesis(&genesis).unwrap();
        let invitation = Invitation::generate(
            workspace.clone(),
            id(),
            DeviceRole::Phone,
            computer.public().clone(),
            now() as u64 + 590,
            state.revision,
            state.head.clone(),
        )
        .unwrap();
        let bootstrap = SignedMembershipSnapshot::sign(state.clone(), &computer).unwrap();
        let candidate = EnrollmentCandidate::create(&invitation, &phone, now() as u64).unwrap();
        let membership = SignedMembership::sign(
            MembershipStatement {
                workspace_id: workspace.clone(),
                revision: 1,
                previous_head: state.head.clone(),
                issuer_device_id: computer.public().device_id.clone(),
                issuer_key_generation: 1,
                action: MembershipAction::Add,
                device: Some(phone.public().clone()),
                revoked_device_id: None,
            },
            &computer,
        )
        .unwrap();
        let approval = SignedEnrollment::sign(&candidate, membership.clone(), &computer).unwrap();
        state.apply_next(&membership).unwrap();
        write(
            &format!("{lane}-create.json"),
            &json!({"membership":genesis,"service_token":node_secret}),
        );
        write(
            &format!("{lane}-invite.json"),
            &json!({"invitation_id":invitation.invitation_id,"purpose":"phone","expires_at":invitation.expires_at,"bootstrap":bootstrap}),
        );
        write(
            &format!("{lane}-join.json"),
            &json!({"candidate":candidate,"service_token":device_secret}),
        );
        write(
            &format!("{lane}-approve.json"),
            &json!({"enrollment":approval}),
        );
        write(
            &format!("{lane}-routes.json"),
            &json!({"routes":[{"route_id":computer.public().device_id,"computer_id":computer.public().device_id}]}),
        );
        let request_id = id();
        let header = Header {
            workspace_id: workspace.clone(),
            sender_device_id: phone.public().device_id.clone(),
            recipient_device_id: computer.public().device_id.clone(),
            sender_key_generation: 1,
            recipient_key_generation: 1,
            gateway_id: computer.public().device_id.clone(),
            route_id: computer.public().device_id.clone(),
            request_id: request_id.clone(),
            kind: PayloadKind::Request,
            operation_class: OperationClass::Mutation,
            expires_at: now() as u64 + 7200,
            membership_revision: state.revision,
            membership_head: state.head.clone(),
        };
        let envelope = crypto::seal(
            &phone,
            computer.public(),
            header,
            &vec![b'x'; crypto::MAX_PAYLOAD_BYTES],
        )
        .unwrap();
        write(
            &format!("{lane}-request.json"),
            &json!({"envelope":envelope}),
        );
        let mut invalid = envelope;
        invalid.signature = crypto::encode(&[0; 64]);
        write(
            &format!("{lane}-signature.json"),
            &json!({"envelope":invalid}),
        );
        let mut result_header = invalid.header;
        result_header.sender_device_id = computer.public().device_id.clone();
        result_header.recipient_device_id = phone.public().device_id.clone();
        result_header.kind = PayloadKind::Result;
        let result =
            crypto::seal(&computer, phone.public(), result_header, b"{\"ok\":true}").unwrap();
        write(&format!("{lane}-result.json"), &json!({"envelope":result}));
        metadata.insert(lane.into(),json!({"workspace_id":workspace,"node_id":computer.public().device_id,"node_token":node_secret,"device_id":phone.public().device_id,"device_token":device_secret,"request_id":request_id}));
    }
    write("metadata.json", &Value::Object(metadata));
}
