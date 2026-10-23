//! Real relay + connector control-plane contracts on isolated fixture storage.
#![cfg(unix)]
use serde_json::{json, Value};
use std::path::Path;
use zerus_mobile_connector::{private_state::Store as PrivateStore, workspace, workspace_cli};
use zerus_mobile_crypto::{
    self as crypto, Identity, MembershipAction, MembershipState, MembershipStatement, Role,
    SignedMembership,
};
use zerus_relay::{args, config::Config, db::i};
#[path = "../../relay/tests/common/mod.rs"]
mod common;
#[path = "../../relay/tests/support/mod.rs"]
mod support;
use common::Backend;
use support::Server;

fn config() -> Config {
    Config {
        public_enrollment: true,
        large_payload_slots: 2,
        max_spool_bytes: 164 * 1024 * 1024,
        background: false,
        ..Config::default()
    }
}
fn revision(path: &Path) -> String {
    workspace::public_status(path).unwrap()["config_revision"]
        .as_str()
        .unwrap()
        .into()
}
fn input(path: &Path, server: &str) -> Value {
    json!({"expected_revision":revision(path),"server_url":server,"workspace_name":"Synthetic workspace","computer_name":"Synthetic computer"})
}

fn private_dir(path: &Path) -> std::path::PathBuf {
    path.parent().unwrap().join("workspace-private")
}
fn private_json(directory: &Path, name: &str) -> Value {
    serde_json::from_slice(
        &workspace_cli::read_private(directory, name)
            .unwrap()
            .unwrap(),
    )
    .unwrap()
}
fn self_retirement(profile: &workspace::Profile) -> SignedMembership {
    let identity = Identity::from_record(&profile.identity).unwrap();
    SignedMembership::sign(
        MembershipStatement {
            workspace_id: profile.membership.workspace_id.clone(),
            revision: profile.membership.revision + 1,
            previous_head: profile.membership.head.clone(),
            issuer_device_id: identity.public().device_id.clone(),
            issuer_key_generation: identity.public().key_generation,
            action: MembershipAction::Revoke,
            device: None,
            revoked_device_id: Some(identity.public().device_id.clone()),
        },
        &identity,
    )
    .unwrap()
}
/// Exact durable point after destination admission and retirement reservation,
/// before the first old-workspace DELETE. No native connector is started.
async fn reserved_switch(
    store: &zerus_relay::store::Store,
    path: &Path,
    server: &str,
    old: &workspace::Profile,
) -> Value {
    let identity = Identity::generate(uuid::Uuid::new_v4().to_string(), Role::Computer).unwrap();
    let genesis = SignedMembership::sign(
        MembershipStatement {
            workspace_id: uuid::Uuid::new_v4().to_string(),
            revision: 0,
            previous_head: crypto::encode(&[0; 32]),
            issuer_device_id: identity.public().device_id.clone(),
            issuer_key_generation: 1,
            action: MembershipAction::Add,
            device: Some(identity.public().clone()),
            revoked_device_id: None,
        },
        &identity,
    )
    .unwrap();
    let token = crypto::encode(&[61; 32]);
    store
        .create_workspace(&json!({"membership":genesis,"service_token":token}))
        .await
        .unwrap();
    let membership = MembershipState::from_genesis(&genesis).unwrap();
    let directory = private_dir(path);
    let destination = workspace::Profile {
        server_url: server.into(),
        workspace_name: "Synthetic destination".into(),
        computer_name: "Synthetic computer".into(),
        node_token: token.clone(),
        identity: identity.to_record(),
        historical_heads: std::collections::BTreeMap::from([(0, membership.head.clone())]),
        membership,
        state_dir: directory.join(format!("receipts-{}", identity.public().device_id)),
    };
    let pending = json!({"version":1,"action":"switch-create","expected_revision":revision(path),
        "server_url":server,"workspace_name":destination.workspace_name,"computer_name":destination.computer_name,
        "node_token":token,"identity":identity.to_record(),"genesis":genesis,"candidate":null,"invitation":null,
        "profile":destination,"previous_profile":old,"retirement":self_retirement(old),
        "legacy_retirement_request":null,"legacy_retirement_status":null});
    let state = PrivateStore::lock(&directory, false).unwrap();
    let expected = PrivateStore::revision(state.read("pending.json").unwrap().as_deref());
    state
        .replace(
            "pending.json",
            &expected,
            &serde_json::to_vec(&pending).unwrap(),
        )
        .unwrap();
    pending
}
async fn join_peer(founder: &Path, peer: &Path, server: &str) -> workspace::Profile {
    let invitation = workspace::manage("invite-computer", founder, input(founder, server), true)
        .await
        .unwrap();
    let mut join = input(peer, server);
    join["code"] = invitation["code"].clone();
    assert_eq!(
        workspace::manage("join", peer, join, true).await.unwrap()["state"],
        "pending"
    );
    let old = workspace::load_active(&private_dir(founder)).unwrap();
    workspace::refresh_and_approve(
        &private_dir(founder),
        &workspace::relay(&old, true).unwrap(),
    )
    .await
    .unwrap();
    assert_eq!(
        workspace::manage("check", peer, input(peer, server), true)
            .await
            .unwrap()["state"],
        "active"
    );
    workspace::load_active(&private_dir(peer)).unwrap()
}
async fn assert_destination_preserved(
    store: &zerus_relay::store::Store,
    path: &Path,
    pending: &Value,
) {
    let active = workspace::load_active(&private_dir(path)).unwrap();
    assert_eq!(
        serde_json::to_value(&active.identity).unwrap(),
        pending["identity"]
    );
    assert_eq!(active.node_token, pending["node_token"].as_str().unwrap());
    assert_eq!(
        active.membership.workspace_id,
        pending["profile"]["membership"]["workspace_id"]
            .as_str()
            .unwrap()
    );
    let count = store
        .db
        .one("SELECT count(*) AS count FROM encrypted_workspaces", &[])
        .await
        .unwrap()
        .unwrap();
    assert_eq!(
        i(&count, "count"),
        2,
        "Recovery must not create another destination workspace"
    );
    assert!(
        !active.state_dir.exists(),
        "Workspace management must not start native execution"
    );
    assert!(private_json(&private_dir(path), "pending.json").is_null());
}

#[tokio::test]
async fn switch_rebases_retirement_only_after_verified_competing_revision() {
    let backend = Backend::new(config()).await;
    let store = backend.store().await;
    let server = Server::start(&store).await;
    let local = tempfile::tempdir().unwrap();
    let path = local.path().join("founder/connector.json");
    let peer_path = local.path().join("peer/connector.json");
    workspace::manage("create", &path, input(&path, &server.url), true)
        .await
        .unwrap();
    let peer = join_peer(&path, &peer_path, &server.url).await;
    let old = workspace::load_active(&private_dir(&path)).unwrap();
    let pending = reserved_switch(&store, &path, &server.url, &old).await;
    let proposed: SignedMembership = serde_json::from_value(pending["retirement"].clone()).unwrap();
    // The other active computer wins the same revision only after the exact
    // destination and old retirement proposal have become durable.
    let competing = self_retirement(&peer);
    assert_eq!(competing.statement.revision, proposed.statement.revision);
    let credential = store
        .authenticate(&peer.node_token, zerus_relay::store::Role::Node)
        .await
        .unwrap();
    store
        .revoke_membership(&credential, &json!({"membership":competing}), true)
        .await
        .unwrap();
    let result = workspace::manage("check", &path, input(&path, &server.url), true)
        .await
        .unwrap();
    assert_eq!(result["state"], "active");
    assert_destination_preserved(&store, &path, &pending).await;
    let archived = private_json(
        &private_dir(&path),
        &format!("retired-{}.json", old.identity.device.device_id),
    );
    let archived: workspace::Profile = serde_json::from_value(archived).unwrap();
    assert_eq!(
        archived.historical_heads.get(&competing.statement.revision),
        Some(&competing.head().unwrap())
    );
    assert_ne!(
        archived.historical_heads.get(&proposed.statement.revision),
        Some(&proposed.head().unwrap())
    );
    assert_eq!(
        archived.membership.revision,
        proposed.statement.revision + 1
    );
    assert!(archived
        .membership
        .revoked_device_ids
        .contains(&old.identity.device.device_id));
    server.stop().await;
    store.db.close().await;
}

#[tokio::test]
async fn switch_recovers_lost_self_retirement_response_from_exact_signed_history() {
    let backend = Backend::new(config()).await;
    let store = backend.store().await;
    let server = Server::start(&store).await;
    let local = tempfile::tempdir().unwrap();
    let path = local.path().join("connector.json");
    workspace::manage("create", &path, input(&path, &server.url), true)
        .await
        .unwrap();
    let old = workspace::load_active(&private_dir(&path)).unwrap();
    let pending = reserved_switch(&store, &path, &server.url, &old).await;
    let retirement: SignedMembership =
        serde_json::from_value(pending["retirement"].clone()).unwrap();
    let credential = store
        .authenticate(&old.node_token, zerus_relay::store::Role::Node)
        .await
        .unwrap();
    // Remote commit succeeded, but no retirement acknowledgment or tombstone
    // was saved locally. Recovery has only the existing restricted history API.
    store
        .revoke_membership(&credential, &json!({"membership":retirement}), true)
        .await
        .unwrap();
    assert!(!workspace::is_verified_retired(&private_dir(&path)).unwrap());
    let result = workspace::manage("check", &path, input(&path, &server.url), true)
        .await
        .unwrap();
    assert_eq!(result["state"], "active");
    assert_destination_preserved(&store, &path, &pending).await;
    let archived = private_json(
        &private_dir(&path),
        &format!("retired-{}.json", old.identity.device.device_id),
    );
    let archived: workspace::Profile = serde_json::from_value(archived).unwrap();
    assert_eq!(archived.membership.head, retirement.head().unwrap());
    assert_eq!(archived.membership.revision, retirement.statement.revision);
    assert!(archived
        .membership
        .revoked_device_ids
        .contains(&old.identity.device.device_id));
    server.stop().await;
    store.db.close().await;
}

#[tokio::test]
async fn switch_retirement_rejects_malformed_forked_incomplete_history_and_bearer_refusal() {
    let backend = Backend::new(config()).await;
    let store = backend.store().await;
    let server = Server::start(&store).await;
    let local = tempfile::tempdir().unwrap();
    let path = local.path().join("connector.json");
    workspace::manage("create", &path, input(&path, &server.url), true)
        .await
        .unwrap();
    let old = workspace::load_active(&private_dir(&path)).unwrap();
    let original = workspace_cli::read_config(&path).unwrap();
    let original_active = private_json(&private_dir(&path), "active.json");
    let pending = reserved_switch(&store, &path, &server.url, &old).await;
    let retirement: SignedMembership =
        serde_json::from_value(pending["retirement"].clone()).unwrap();
    let identity = Identity::from_record(&old.identity).unwrap();
    let mut fork = retirement.statement.clone();
    fork.revision += 1;
    fork.previous_head = crypto::encode(&[94; 32]);
    let fork = SignedMembership::sign(fork, &identity).unwrap();
    for change in [&retirement, &fork] {
        let head = change.head().unwrap();
        store.db.exec("INSERT INTO membership_changes(workspace_id,revision,change_id,head,record) VALUES(?,?,?,?,?)",
            args![&old.membership.workspace_id,change.statement.revision as i64,&head,&head,&serde_json::to_string(change).unwrap()]).await.unwrap();
    }
    store.db.exec("UPDATE encrypted_workspaces SET membership_revision=?,membership_head=? WHERE workspace_id=?",
        args![fork.statement.revision as i64,&fork.head().unwrap(),&old.membership.workspace_id]).await.unwrap();
    // A valid-looking self-tombstone prefix must not be committed when another
    // record in that same page is malformed, branches from an unknown head,
    // or is omitted while the relay still advertises the later revision.
    for record in [
        Some(json!({"version":1})),
        Some(serde_json::to_value(&fork).unwrap()),
        None,
    ] {
        if let Some(record) = record {
            store
                .db
                .exec(
                    "UPDATE membership_changes SET record=? WHERE workspace_id=? AND revision=?",
                    args![
                        &serde_json::to_string(&record).unwrap(),
                        &old.membership.workspace_id,
                        fork.statement.revision as i64
                    ],
                )
                .await
                .unwrap();
        } else {
            store
                .db
                .exec(
                    "DELETE FROM membership_changes WHERE workspace_id=? AND revision=?",
                    args![&old.membership.workspace_id, fork.statement.revision as i64],
                )
                .await
                .unwrap();
        }
        assert!(
            workspace::manage("check", &path, input(&path, &server.url), true)
                .await
                .is_err()
        );
        assert_eq!(workspace_cli::read_config(&path).unwrap(), original);
        assert!(!workspace::is_verified_retired(&private_dir(&path)).unwrap());
        assert_eq!(
            workspace::load_active(&private_dir(&path))
                .unwrap()
                .membership
                .head,
            old.membership.head
        );
        let retained = private_json(&private_dir(&path), "pending.json");
        for field in [
            "identity",
            "profile",
            "candidate",
            "node_token",
            "retirement",
        ] {
            assert_eq!(
                retained[field], pending[field],
                "A failed history check must preserve the pending destination and proposal"
            );
        }
    }
    // Restore a complete, valid old history first. Otherwise an error after
    // bearer refusal could still be caused by the preceding incomplete page.
    store
        .db
        .exec(
            "DELETE FROM membership_changes WHERE workspace_id=? AND revision>?",
            args![&old.membership.workspace_id, old.membership.revision as i64],
        )
        .await
        .unwrap();
    store.db.exec("UPDATE encrypted_workspaces SET membership_revision=?,membership_head=? WHERE workspace_id=?",
        args![old.membership.revision as i64,&old.membership.head,&old.membership.workspace_id]).await.unwrap();
    let old_relay = workspace::relay(&old, true).unwrap();
    let history_path = format!(
        "/v2/node/membership?after={}&limit=100",
        old.membership.revision
    );
    let restored = old_relay
        .call("GET".parse().unwrap(), &history_path, None, 2 * 1024 * 1024)
        .await
        .unwrap()
        .unwrap();
    assert_eq!(restored["membership_revision"], old.membership.revision);
    assert_eq!(restored["membership_head"], old.membership.head);
    assert!(restored["changes"].as_array().unwrap().is_empty());
    assert!(restored["next_after"].is_null());
    let before_refusal = private_json(&private_dir(&path), "pending.json");
    assert_eq!(before_refusal, pending);
    store
        .db
        .exec(
            "UPDATE nodes SET revoked=1 WHERE id=?",
            args![&old.identity.device.device_id],
        )
        .await
        .unwrap();
    let refusal = old_relay
        .call("GET".parse().unwrap(), &history_path, None, 2 * 1024 * 1024)
        .await
        .unwrap_err();
    assert_eq!(refusal.status(), Some(401));
    assert!(
        workspace::manage("check", &path, input(&path, &server.url), true)
            .await
            .is_err()
    );
    assert_eq!(workspace_cli::read_config(&path).unwrap(), original);
    assert!(!workspace::is_verified_retired(&private_dir(&path)).unwrap());
    assert_eq!(
        private_json(&private_dir(&path), "active.json"),
        original_active
    );
    assert_eq!(
        private_json(&private_dir(&path), "pending.json"),
        before_refusal
    );
    assert!(!Path::new(pending["profile"]["state_dir"].as_str().unwrap()).exists());
    server.stop().await;
    store.db.close().await;
}

#[tokio::test]
async fn manager_create_join_remove_founder_and_switch_last_computer() {
    let backend = Backend::new(config()).await;
    let store = backend.store().await;
    let server = Server::start(&store).await;
    let local = tempfile::tempdir().unwrap();
    let founder = local.path().join("founder/connector.json");
    let peer = local.path().join("peer/connector.json");
    // Status has no filesystem or registration side effect.
    assert_eq!(
        workspace::public_status(&founder).unwrap()["state"],
        "unconfigured"
    );
    assert!(!founder.parent().unwrap().exists());
    let created = workspace::manage("create", &founder, input(&founder, &server.url), true)
        .await
        .unwrap();
    assert_eq!(created["state"], "active");
    let founder_private = founder.parent().unwrap().join("workspace-private");
    let original = workspace::load_active(&founder_private).unwrap();
    let invitation = workspace::manage(
        "invite-computer",
        &founder,
        input(&founder, &server.url),
        true,
    )
    .await
    .unwrap();
    let mut join = input(&peer, &server.url);
    join["code"] = invitation["code"].clone();
    let pending = workspace::manage("join", &peer, join, true).await.unwrap();
    assert_eq!(pending["state"], "pending");
    assert!(!peer.exists());
    let relay = workspace::relay(&original, true).unwrap();
    workspace::refresh_and_approve(&founder_private, &relay)
        .await
        .unwrap();
    let joined = workspace::manage("check", &peer, input(&peer, &server.url), true)
        .await
        .unwrap();
    assert_eq!(joined["state"], "active");
    let peer_private = peer.parent().unwrap().join("workspace-private");
    let enrolled = workspace::load_active(&peer_private).unwrap();
    assert_eq!(
        original.membership.workspace_id,
        enrolled.membership.workspace_id
    );
    assert_ne!(
        original.identity.device.device_id,
        enrolled.identity.device.device_id
    );
    // Removing the founding computer preserves the independently admitted peer.
    let mut remove = input(&peer, &server.url);
    remove["device_id"] = json!(original.identity.device.device_id);
    let roster = workspace::manage("revoke-device", &peer, remove, true)
        .await
        .unwrap();
    assert_eq!(roster["devices"].as_array().unwrap().len(), 1);
    // An accepted signed self-tombstone exposes a retired status and permits
    // explicit re-enrollment with fresh keys; an HTTP401 alone never does.
    assert!(workspace::refresh_and_approve(&founder_private, &relay)
        .await
        .is_err());
    assert_eq!(
        workspace::public_status(&founder).unwrap()["state"],
        "retired"
    );
    assert!(workspace::is_verified_retired(&founder_private).unwrap());
    assert!(workspace::load_active(&founder_private).is_err());
    let rejoined = workspace::manage(
        "switch-create",
        &founder,
        input(&founder, &server.url),
        true,
    )
    .await
    .unwrap();
    assert_eq!(rejoined["state"], "active");
    assert_ne!(rejoined["node_id"], original.identity.device.device_id);
    let invited = workspace::manage("invite-phone", &peer, input(&peer, &server.url), true)
        .await
        .unwrap();
    assert_eq!(invited["purpose"], "phone");
    let mut cancel = input(&peer, &server.url);
    cancel["invitation_id"] = invited["invitation_id"].clone();
    assert_eq!(
        workspace::manage("cancel-invite", &peer, cancel, true)
            .await
            .unwrap()["cancelled"],
        true
    );
    let switched = workspace::manage("switch-create", &peer, input(&peer, &server.url), true)
        .await
        .unwrap();
    assert_eq!(switched["state"], "active");
    assert_ne!(switched["workspace_id"], joined["workspace_id"]);
    let changed = workspace::load_active(&peer_private).unwrap();
    assert_ne!(changed.state_dir, enrolled.state_dir);
    assert!(peer_private
        .join(format!(
            "retired-{}.json",
            enrolled.identity.device.device_id
        ))
        .exists());
    let old = store
        .db
        .one(
            "SELECT count(*) AS count FROM encrypted_members WHERE workspace_id=? AND revoked=0",
            args![&enrolled.membership.workspace_id],
        )
        .await
        .unwrap()
        .unwrap();
    assert_eq!(i(&old, "count"), 0);
    for path in [&founder, &peer] {
        let (config, _) = workspace_cli::read_config(path).unwrap();
        assert!(config.get("node_token").is_none());
        assert_eq!(config["encrypted_workspace"]["version"], 1);
    }
    assert!(!created.to_string().contains("node_token"));
    assert!(!joined.to_string().contains("private_key"));
    server.stop().await;
    store.db.close().await;
}

#[tokio::test]
async fn create_recovers_lost_response_with_exact_persisted_identity_and_token() {
    let backend = Backend::new(config()).await;
    let store = backend.store().await;
    let server = Server::start(&store).await;
    let local = tempfile::tempdir().unwrap();
    let config = local.path().join("connector.json");
    let private = local.path().join("workspace-private");
    let identity = Identity::generate(uuid::Uuid::new_v4().to_string(), Role::Computer).unwrap();
    let token = zerus_relay::store::token();
    let genesis = SignedMembership::sign(
        MembershipStatement {
            workspace_id: uuid::Uuid::new_v4().to_string(),
            revision: 0,
            previous_head: crypto::encode(&[0; 32]),
            issuer_device_id: identity.public().device_id.clone(),
            issuer_key_generation: 1,
            action: MembershipAction::Add,
            device: Some(identity.public().clone()),
            revoked_device_id: None,
        },
        &identity,
    )
    .unwrap();
    // This is the exact durable state before the first Create request. The
    // remote commit below simulates losing that request's HTTP response.
    let pending = json!({"version":1,"action":"create","expected_revision":"","server_url":server.url,"workspace_name":"Synthetic","computer_name":"Synthetic","node_token":token,"identity":identity.to_record(),"genesis":genesis,"candidate":null,"invitation":null,"profile":null,"previous_profile":null,"retirement":null});
    let state = PrivateStore::lock(&private, true).unwrap();
    state
        .replace(
            "pending.json",
            "",
            &serde_json::to_vec(&Some(pending)).unwrap(),
        )
        .unwrap();
    drop(state);
    store
        .create_workspace(&json!({"membership":genesis,"service_token":token}))
        .await
        .unwrap();
    let recovered = workspace::manage("check", &config, input(&config, &server.url), true)
        .await
        .unwrap();
    assert_eq!(recovered["node_id"], identity.public().device_id);
    let active = workspace::load_active(&private).unwrap();
    assert_eq!(active.node_token, token);
    assert_eq!(active.identity.device, identity.public().clone());
    let count = store
        .db
        .one(
            "SELECT count(*) AS count FROM encrypted_workspaces WHERE workspace_id=?",
            args![&genesis.statement.workspace_id],
        )
        .await
        .unwrap()
        .unwrap();
    assert_eq!(i(&count, "count"), 1);
    server.stop().await;
    store.db.close().await;
}

#[tokio::test]
async fn legacy_switch_waits_for_service_stop_and_replays_exact_retirement() {
    let backend = Backend::new(config()).await;
    let store = backend.store().await;
    let server = Server::start(&store).await;
    let credentials = store
        .provision("Synthetic legacy", "Synthetic computer")
        .await
        .unwrap();
    let local = tempfile::tempdir().unwrap();
    let config = local.path().join("connector.json");
    let old_state = local.path().join("legacy-journal");
    zerus_mobile_connector::fsguard::create_directory(&old_state).unwrap();
    let journal = old_state.join("connector.lock");
    let lock = zerus_mobile_connector::fsguard::private_file(&journal, true)
        .unwrap()
        .unwrap();
    zerus_mobile_connector::fsguard::lock_exclusive(&lock).unwrap();
    let original = json!({"server_url":server.url,"node_token":credentials["node_token"],"state_dir":old_state,"extension":{"preserve":true}});
    workspace_cli::replace_config(&config, "", &original).unwrap();
    let blocked = workspace::manage("switch-create", &config, input(&config, &server.url), true)
        .await
        .err()
        .unwrap();
    assert!(blocked.to_string().contains("service controls"));
    assert_eq!(workspace_cli::read_config(&config).unwrap().0, original);
    let private = local.path().join("workspace-private");
    let pending_bytes = workspace_cli::read_private(&private, "pending.json")
        .unwrap()
        .unwrap();
    let mut pending: Value = serde_json::from_slice(&pending_bytes).unwrap();
    let new_id = pending["identity"]["device"]["device_id"].clone();
    assert!(pending["legacy_retirement_request"].is_null());
    drop(lock);
    // A crash after relay retirement but before reading its response must
    // resume the exact saved receipt using the now-revoked old credential.
    let retirement = uuid::Uuid::new_v4().to_string();
    pending["legacy_retirement_request"] = json!(retirement);
    let state = PrivateStore::lock(&private, false).unwrap();
    state
        .replace(
            "pending.json",
            &PrivateStore::revision(Some(&pending_bytes)),
            &serde_json::to_vec(&pending).unwrap(),
        )
        .unwrap();
    drop(state);
    let legacy_credential = store
        .authenticate(
            credentials["node_token"].as_str().unwrap(),
            zerus_relay::store::Role::Node,
        )
        .await
        .unwrap();
    store
        .retire_legacy_node(&legacy_credential, &json!({"request_id":retirement}))
        .await
        .unwrap();
    let active = workspace::manage("check", &config, input(&config, &server.url), true)
        .await
        .unwrap();
    assert_eq!(active["state"], "active");
    assert_eq!(active["node_id"], new_id);
    let protected = workspace_cli::read_config(&config).unwrap().0;
    assert!(protected.get("node_token").is_none());
    assert_eq!(protected["extension"]["preserve"], true);
    let row = store
        .db
        .one(
            "SELECT revoked FROM nodes WHERE id=?",
            args![credentials["node_id"].as_str().unwrap()],
        )
        .await
        .unwrap()
        .unwrap();
    assert_eq!(i(&row, "revoked"), 1);
    let archive = std::fs::read_dir(&private)
        .unwrap()
        .filter_map(|entry| entry.ok())
        .find(|entry| {
            entry
                .file_name()
                .to_string_lossy()
                .starts_with("legacy-retired-")
        })
        .unwrap();
    let archived: Value = serde_json::from_slice(&std::fs::read(archive.path()).unwrap()).unwrap();
    assert_eq!(archived, original);
    assert!(journal.exists());
    server.stop().await;
    store.db.close().await;
}

#[tokio::test]
async fn invalid_legacy_bearer_requires_explicit_local_disconnect() {
    let backend = Backend::new(config()).await;
    let store = backend.store().await;
    let server = Server::start(&store).await;
    let local = tempfile::tempdir().unwrap();
    let config = local.path().join("connector.json");
    let old_state = local.path().join("legacy-journal");
    let original = json!({"server_url":server.url,"node_token":"synthetic-invalid-legacy-token","state_dir":old_state,"extension":{"preserve":true}});
    workspace_cli::replace_config(&config, "", &original).unwrap();
    let pending = workspace::manage("switch-create", &config, input(&config, &server.url), true)
        .await
        .unwrap();
    assert_eq!(pending["state"], "legacy-retirement-unconfirmed");
    assert_eq!(workspace_cli::read_config(&config).unwrap().0, original);
    assert_eq!(
        workspace::public_status(&config).unwrap()["state"],
        "legacy"
    );
    assert_eq!(
        workspace::public_status(&config).unwrap()["pending"]["state"],
        "legacy-retirement-unconfirmed"
    );
    let repeated = workspace::manage("check", &config, input(&config, &server.url), true)
        .await
        .unwrap();
    assert_eq!(repeated["state"], "legacy-retirement-unconfirmed");
    assert_eq!(workspace_cli::read_config(&config).unwrap().0, original);
    let mut explicit = input(&config, &server.url);
    explicit["allow_unconfirmed_legacy_retirement"] = json!(true);
    zerus_mobile_connector::fsguard::create_directory(&old_state).unwrap();
    let old_lock =
        zerus_mobile_connector::fsguard::private_file(&old_state.join("connector.lock"), true)
            .unwrap()
            .unwrap();
    zerus_mobile_connector::fsguard::lock_exclusive(&old_lock).unwrap();
    assert!(workspace::manage("check", &config, explicit.clone(), true)
        .await
        .is_err());
    assert_eq!(workspace_cli::read_config(&config).unwrap().0, original);
    drop(old_lock);
    let active = workspace::manage("check", &config, explicit, true)
        .await
        .unwrap();
    assert_eq!(active["state"], "active");
    assert_eq!(active["node_id"], pending["node_id"]);
    assert_eq!(active["legacy_retirement"], "unconfirmed");
    let protected = workspace_cli::read_config(&config).unwrap().0;
    assert!(protected.get("node_token").is_none());
    assert_eq!(protected["extension"]["preserve"], true);
    assert!(old_state.join("connector.lock").exists());
    assert!(!active
        .to_string()
        .contains("synthetic-invalid-legacy-token"));
    server.stop().await;
    store.db.close().await;
}

#[tokio::test]
async fn protected_switch_never_uses_local_legacy_disconnect_flag() {
    let backend = Backend::new(config()).await;
    let store = backend.store().await;
    let server = Server::start(&store).await;
    let local = tempfile::tempdir().unwrap();
    let config = local.path().join("connector.json");
    workspace::manage("create", &config, input(&config, &server.url), true)
        .await
        .unwrap();
    let profile = workspace::load_active(&local.path().join("workspace-private")).unwrap();
    let before = workspace_cli::read_config(&config).unwrap().0;
    // An operator-side bearer removal is not a trusted signed membership
    // revocation. It cannot authorize forgetting a protected workspace.
    store
        .db
        .exec(
            "UPDATE nodes SET revoked=1 WHERE id=?",
            args![&profile.identity.device.device_id],
        )
        .await
        .unwrap();
    assert!(
        workspace::manage("switch-create", &config, input(&config, &server.url), true)
            .await
            .is_err()
    );
    let mut bypass = input(&config, &server.url);
    bypass["allow_unconfirmed_legacy_retirement"] = json!(true);
    assert!(workspace::manage("check", &config, bypass, true)
        .await
        .is_err());
    assert_eq!(workspace_cli::read_config(&config).unwrap().0, before);
    assert_eq!(
        workspace::public_status(&config).unwrap()["state"],
        "active"
    );
    assert!(!workspace::is_verified_retired(&local.path().join("workspace-private")).unwrap());
    assert_eq!(
        workspace::load_active(&local.path().join("workspace-private"))
            .unwrap()
            .identity
            .device
            .device_id,
        profile.identity.device.device_id
    );
    server.stop().await;
    store.db.close().await;
}
