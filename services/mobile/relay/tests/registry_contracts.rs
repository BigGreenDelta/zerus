//! One-hop registry contracts that only the Python reference relay's tests
//! covered: immutable gateway routes, direct enrollments of a peer machine,
//! revocation, event identity and the bounded catalog. `ORACLE.md` maps each
//! Python test to its contract here.
use serde_json::{json, Value};
use uuid::Uuid;
use zerus_relay::{
    args,
    db::{i, now, s},
    error::Result,
    json::{canonical, digest},
    store::{Credential, Role},
};

mod common;
mod support;
use common::Fixture;
use support::Server;

fn snapshot(name: &str, extra: Value) -> Value {
    let mut session = json!({"name":name,"run_id":"run","conversation_id":"conversation"});
    for (k, v) in extra.as_object().unwrap() {
        session[k] = v.clone();
    }
    json!({"sessions":[session],"mobile_capabilities":{"protocol_version":1,"operations":["inspect","send"],"features":["gateway_one_hop"]}})
}
fn plain() -> Value {
    snapshot("session", json!({}))
}
fn question() -> Value {
    snapshot("session", json!({"phase":"input"}))
}
fn inspect(computer: &str) -> Value {
    json!({"request_id":Uuid::new_v4().to_string(),"computer_id":computer,"operation":"inspect","session":"session","payload":{}})
}

/// A gateway with machine `local` that shares one verified peer, `remote`,
/// over `route`; `target` is the peer's computer ID in the catalog.
struct Fleet {
    f: Fixture,
    local: String,
    remote: String,
    route: String,
    peers: Value,
    target: String,
}

impl Fleet {
    async fn new() -> Self {
        Self::with(Fixture::new().await).await
    }
    async fn with(f: Fixture) -> Self {
        let [local, remote, route] = std::array::from_fn(|_| Uuid::new_v4().to_string());
        let peers = json!([{"route_id":route,"machine_id":remote,"name":"Remote","online":true}]);
        let mut fleet = Self {
            f,
            local,
            remote,
            route,
            peers,
            target: String::new(),
        };
        fleet.manifest(&fleet.peers, &plain()).await.unwrap();
        fleet
            .peer(&fleet.route, &fleet.remote, &plain())
            .await
            .unwrap();
        fleet.target = s(&fleet.rows(&fleet.remote).await[0], "id").to_string();
        fleet
    }
    async fn manifest(&self, peers: &Value, snapshot: &Value) -> Result<()> {
        self.f
            .store
            .heartbeat(
                &self.f.node,
                &json!({"snapshot":snapshot,"machine_id":self.local,"peers":peers}),
                &canonical(snapshot).unwrap(),
            )
            .await
    }
    async fn peer(&self, route: &str, machine: &str, snapshot: &Value) -> Result<()> {
        self.f
            .store
            .peer_heartbeat(
                &self.f.node,
                route,
                &json!({"machine_id":machine,"snapshot":snapshot}),
                &canonical(snapshot).unwrap(),
            )
            .await
    }
    /// Another enrollment of the same workspace, as a computer's own connector.
    async fn direct(&self, name: &str) -> (String, Credential) {
        let created = self
            .f
            .store
            .node(&self.f.phone.workspace, name)
            .await
            .unwrap();
        let credential = self
            .f
            .store
            .authenticate(s(&created, "node_token"), Role::Node)
            .await
            .unwrap();
        (s(&created, "node_id").to_string(), credential)
    }
    async fn direct_beat(&self, credential: &Credential, snapshot: &Value) -> Result<()> {
        self.f
            .store
            .heartbeat(
                credential,
                &json!({"snapshot":snapshot,"machine_id":self.remote,"peers":[]}),
                &canonical(snapshot).unwrap(),
            )
            .await
    }
    async fn catalog(&self) -> Vec<Value> {
        self.f.store.computers(&self.f.phone).await.unwrap()["computers"]
            .as_array()
            .unwrap()
            .clone()
    }
    async fn rows(&self, machine: &str) -> Vec<Value> {
        self.catalog()
            .await
            .into_iter()
            .filter(|r| r["machine_id"] == machine)
            .collect()
    }
    async fn online(&self) -> std::collections::BTreeSet<String> {
        self.catalog()
            .await
            .iter()
            .filter(|r| r["online"] == true)
            .map(|r| s(r, "machine_id").to_string())
            .collect()
    }
    async fn submit(&self, b: &Value) -> u16 {
        match self.f.submit(b).await {
            Ok(_) => 202,
            Err(e) => e.0.as_u16(),
        }
    }
    async fn state(&self, b: &Value) -> Value {
        self.f
            .store
            .receipt(&self.f.phone, s(b, "request_id"))
            .await
            .unwrap()["state"]
            .clone()
    }
    async fn claim(&self, node: &Credential, allow: bool) -> Option<Value> {
        self.f
            .store
            .claim(node, allow)
            .await
            .unwrap()
            .map(|c| c.decode().unwrap()["requests"][0].clone())
    }
    /// Ordinary events by computer, without the presence alerts beside them.
    async fn events(&self) -> Vec<(String, String)> {
        self.f.store.events(&self.f.phone, 0).await.unwrap()["events"]
            .as_array()
            .unwrap()
            .iter()
            .filter(|e| e["kind"] != "alert")
            .map(|e| (s(e, "computer_id").into(), s(e, "kind").into()))
            .collect()
    }
    async fn stale_route(&self) {
        self.f
            .store
            .db
            .exec(
                "UPDATE computer_routes SET last_seen=? WHERE gateway_id=? AND route_id=?",
                args![now() - 60.0, &self.f.node.id, &self.route],
            )
            .await
            .unwrap();
    }
    async fn close(self) {
        self.f.close().await;
    }
}

#[tokio::test]
async fn peers_create_no_credential_and_stay_in_their_workspace() {
    let fleet = Fleet::new().await;
    assert_eq!(fleet.f.count("nodes").await, 1);
    let other = fleet
        .f
        .store
        .provision("Other workspace", "Foreign")
        .await
        .unwrap();
    assert_eq!(fleet.submit(&inspect(s(&other, "node_id"))).await, 404);
    assert_eq!(fleet.catalog().await.len(), 2);
    fleet.close().await;
}

#[tokio::test]
async fn gateway_claims_need_opt_in_and_pin_the_native_identity() {
    let fleet = Fleet::new().await;
    let node = &fleet.f.node;
    let body = inspect(&fleet.target);
    assert_eq!(fleet.submit(&body).await, 202);
    assert!(!fleet.f.store.pending(node, false).await.unwrap());
    assert!(fleet.claim(node, false).await.is_none());
    let claimed = fleet.claim(node, true).await.unwrap();
    assert_eq!(
        claimed["gateway_route"],
        json!({"schema":1,"route_id":fleet.route,"computer_id":fleet.target,"machine_id":fleet.remote})
    );
    assert!(fleet.claim(node, true).await.is_none());
    // The gateway's own computer is pinned to its machine too, without a route.
    let local = inspect(&node.id);
    assert_eq!(fleet.submit(&local).await, 202);
    assert!(fleet.claim(node, false).await.is_none());
    let claimed = fleet.claim(node, true).await.unwrap();
    assert!(claimed["gateway_route"]["route_id"].is_null());
    assert_eq!(claimed["gateway_route"]["machine_id"], fleet.local);
    fleet.f.check_accounting().await;
    fleet.close().await;
}

#[tokio::test]
async fn withdrawn_routes_never_revive_old_requests_or_receipts() {
    let fleet = Fleet::new().await;
    let old = inspect(&fleet.target);
    assert_eq!(fleet.submit(&old).await, 202);
    fleet.manifest(&json!([]), &plain()).await.unwrap();
    fleet.manifest(&fleet.peers, &plain()).await.unwrap();
    fleet
        .peer(&fleet.route, &fleet.remote, &plain())
        .await
        .unwrap();
    assert_eq!(fleet.f.submit(&old).await.unwrap()["state"], "failed");
    let new = inspect(&fleet.target);
    assert_eq!(fleet.submit(&new).await, 202);
    assert_eq!(
        fleet.claim(&fleet.f.node, true).await.unwrap()["request_id"],
        new["request_id"]
    );
    let row = fleet
        .f
        .store
        .db
        .one(
            "SELECT body_hash FROM requests WHERE id=?",
            args![&old["request_id"]],
        )
        .await
        .unwrap()
        .unwrap();
    assert_eq!(row["body_hash"], digest(canonical(&old).unwrap()));
    fleet.f.check_accounting().await;
    fleet.close().await;
}

#[tokio::test]
async fn withdrawal_after_claim_is_uncertain_and_results_cannot_resolve_it() {
    let fleet = Fleet::new().await;
    let body = inspect(&fleet.target);
    assert_eq!(fleet.submit(&body).await, 202);
    assert!(fleet.claim(&fleet.f.node, true).await.is_some());
    fleet.manifest(&json!([]), &plain()).await.unwrap();
    assert_eq!(fleet.state(&body).await, "uncertain");
    let result = json!({"state":"completed","result":{},"error":null});
    let refused = fleet
        .f
        .store
        .result(
            &fleet.f.node,
            s(&body, "request_id"),
            &result,
            Some(&canonical(&result["result"]).unwrap()),
        )
        .await
        .unwrap_err();
    assert_eq!(refused.0.as_u16(), 409);
    fleet.f.check_accounting().await;
    fleet.close().await;
}

#[tokio::test]
async fn gateway_liveness_never_refreshes_a_stale_peer() {
    let fleet = Fleet::new().await;
    fleet.stale_route().await;
    fleet.manifest(&fleet.peers, &plain()).await.unwrap();
    assert_eq!(fleet.rows(&fleet.remote).await[0]["online"], false);
    assert_eq!(fleet.submit(&inspect(&fleet.target)).await, 409);
    fleet.close().await;
}

#[tokio::test]
async fn a_stale_frozen_route_never_reroutes_to_a_fresh_direct_enrollment() {
    let fleet = Fleet::new().await;
    let body = inspect(&fleet.target);
    assert_eq!(fleet.submit(&body).await, 202);
    let (_, direct) = fleet.direct("Direct remote").await;
    fleet.direct_beat(&direct, &plain()).await.unwrap();
    fleet.stale_route().await;
    assert!(fleet.claim(&direct, true).await.is_none());
    assert!(fleet.claim(&fleet.f.node, true).await.is_none());
    assert_eq!(fleet.state(&body).await, "failed");
    fleet.f.check_accounting().await;
    fleet.close().await;
}

#[tokio::test]
async fn an_unexposed_direct_enrollment_becomes_an_alias_of_the_peer() {
    let fleet = Fleet::new().await;
    let (alias, direct) = fleet.direct("Direct remote").await;
    fleet.direct_beat(&direct, &plain()).await.unwrap();
    let rows = fleet.rows(&fleet.remote).await;
    assert_eq!(rows.len(), 1);
    assert_eq!(rows[0]["id"], fleet.target);
    assert!(rows[0]["aliases"]
        .as_array()
        .unwrap()
        .contains(&json!(alias)));
    assert!(rows[0]["via"].is_null());
    assert_eq!(fleet.submit(&inspect(&alias)).await, 202);
    let claimed = fleet.claim(&direct, true).await.unwrap();
    assert_eq!(claimed["gateway_route"]["computer_id"], fleet.target);
    fleet.close().await;
}

#[tokio::test]
async fn exposed_legacy_ids_stay_listed_and_revoke_the_physical_machine() {
    let fleet = Fleet::new().await;
    let (legacy, direct) = fleet.direct("Legacy remote").await;
    // A phone has already listed (and saved) this enrollment's own ID.
    fleet.catalog().await;
    fleet.direct_beat(&direct, &plain()).await.unwrap();
    let ids: std::collections::BTreeSet<_> = fleet
        .rows(&fleet.remote)
        .await
        .iter()
        .map(|r| s(r, "id").to_string())
        .collect();
    assert_eq!(ids, [fleet.target.clone(), legacy.clone()].into());
    fleet.f.store.revoke_computer(&legacy).await.unwrap();
    assert!(fleet.rows(&fleet.remote).await.is_empty());
    // Both connectors keep reporting without an error.
    fleet.manifest(&fleet.peers, &plain()).await.unwrap();
    fleet.direct_beat(&direct, &plain()).await.unwrap();
    assert!(fleet.rows(&fleet.remote).await.is_empty());
    fleet.close().await;
}

#[tokio::test]
async fn a_revoked_peer_leaves_the_gateway_and_other_peers_working() {
    let fleet = Fleet::new().await;
    let [route, machine] = std::array::from_fn(|_| Uuid::new_v4().to_string());
    let mut peers = fleet.peers.clone();
    peers
        .as_array_mut()
        .unwrap()
        .push(json!({"route_id":route,"machine_id":machine,"name":"Other","online":true}));
    fleet.manifest(&peers, &plain()).await.unwrap();
    fleet.peer(&route, &machine, &plain()).await.unwrap();
    let queued = inspect(&fleet.target);
    assert_eq!(fleet.submit(&queued).await, 202);
    fleet.f.store.revoke_computer(&fleet.target).await.unwrap();
    // The connector keeps publishing its unchanged manifest.
    for _ in 0..2 {
        fleet.manifest(&peers, &plain()).await.unwrap();
        fleet.peer(&route, &machine, &plain()).await.unwrap();
    }
    assert_eq!(
        fleet.online().await,
        [fleet.local.clone(), machine.clone()].into()
    );
    assert!(fleet.rows(&fleet.remote).await.is_empty());
    assert_eq!(fleet.state(&queued).await, "failed");
    assert_eq!(fleet.submit(&inspect(&fleet.target)).await, 409);
    assert!(fleet
        .peer(&fleet.route, &fleet.remote, &plain())
        .await
        .is_err());
    assert!(fleet.claim(&fleet.f.node, true).await.is_none());
    fleet.f.check_accounting().await;
    fleet.close().await;
}

#[tokio::test]
async fn root_revocation_keeps_the_gateway_credential_and_peer_routes() {
    let fleet = Fleet::new().await;
    let gateway = fleet.f.node.id.clone();
    let local = inspect(&gateway);
    assert_eq!(fleet.submit(&local).await, 202);
    fleet.f.store.revoke_computer(&gateway).await.unwrap();
    for _ in 0..2 {
        fleet.manifest(&fleet.peers, &plain()).await.unwrap();
        fleet
            .peer(&fleet.route, &fleet.remote, &plain())
            .await
            .unwrap();
    }
    assert_eq!(fleet.online().await, [fleet.remote.clone()].into());
    fleet.f.store.authorized(&fleet.f.node).await.unwrap();
    assert_eq!(fleet.state(&local).await, "failed");
    assert_eq!(fleet.submit(&inspect(&gateway)).await, 409);
    let body = inspect(&fleet.target);
    assert_eq!(fleet.submit(&body).await, 202);
    assert_eq!(
        fleet.claim(&fleet.f.node, true).await.unwrap()["request_id"],
        body["request_id"]
    );
    // The gateway's machine binding stays immutable.
    let rebound = fleet
        .f
        .store
        .heartbeat(
            &fleet.f.node,
            &json!({"snapshot":plain(),"machine_id":Uuid::new_v4().to_string(),"peers":fleet.peers}),
            &canonical(&plain()).unwrap(),
        )
        .await;
    assert!(rebound.is_err());
    fleet.close().await;
}

#[tokio::test]
async fn a_revoked_root_publishes_no_local_snapshot_events_or_wakes() {
    let fleet = Fleet::new().await;
    fleet
        .f
        .store
        .register_push(&fleet.f.phone, "fcm", "synthetic-target")
        .await
        .unwrap();
    let before = fleet.f.node_row("snapshot").await;
    fleet
        .f
        .store
        .revoke_computer(&fleet.f.node.id)
        .await
        .unwrap();
    fleet
        .manifest(
            &fleet.peers,
            &snapshot("private-revoked-session", json!({"phase":"input"})),
        )
        .await
        .unwrap();
    assert_eq!(fleet.f.node_row("snapshot").await, before);
    assert!(fleet.events().await.is_empty());
    assert_eq!(fleet.f.count("events").await, 0);
    assert_eq!(fleet.f.count("push_jobs").await, 0);
    fleet
        .peer(&fleet.route, &fleet.remote, &question())
        .await
        .unwrap();
    assert_eq!(
        fleet.events().await,
        [(fleet.target.clone(), "attention".to_string())]
    );
    assert_eq!(fleet.f.count("push_jobs").await, 1);
    fleet.close().await;
}

#[tokio::test]
async fn gateway_revocation_keeps_an_independent_direct_route() {
    let fleet = Fleet::new().await;
    let (_, direct) = fleet.direct("Direct remote").await;
    fleet.direct_beat(&direct, &plain()).await.unwrap();
    fleet
        .f
        .store
        .revoke(Role::Node, &fleet.f.node.id)
        .await
        .unwrap();
    assert_eq!(fleet.submit(&inspect(&fleet.target)).await, 202);
    assert!(fleet.claim(&direct, true).await.is_some());
    fleet.close().await;
}

#[tokio::test]
async fn the_root_binding_is_immutable_and_peer_metadata_must_match() {
    let fleet = Fleet::new().await;
    let other = Uuid::new_v4().to_string();
    let rebound = fleet
        .f
        .store
        .heartbeat(
            &fleet.f.node,
            &json!({"snapshot":plain(),"machine_id":other,"peers":[]}),
            &canonical(&plain()).unwrap(),
        )
        .await;
    assert!(rebound.is_err());
    assert!(fleet.peer(&fleet.route, &other, &plain()).await.is_err());
    assert!(fleet.peer(&other, &fleet.remote, &plain()).await.is_err());
    fleet.manifest(&json!([]), &plain()).await.unwrap();
    assert!(fleet
        .peer(&fleet.route, &fleet.remote, &plain())
        .await
        .is_err());
    fleet.close().await;
}

#[tokio::test]
async fn peer_events_name_the_target_computer_and_keep_event_accounting() {
    let fleet = Fleet::new().await;
    for _ in 0..2 {
        fleet
            .peer(&fleet.route, &fleet.remote, &question())
            .await
            .unwrap();
    }
    assert_eq!(
        fleet.events().await,
        [(fleet.target.clone(), "attention".to_string())]
    );
    let events = fleet.f.count("events").await;
    let usage = fleet
        .f
        .store
        .db
        .one(
            "SELECT events FROM workspace_usage WHERE workspace_id=?",
            args![&fleet.f.phone.workspace],
        )
        .await
        .unwrap()
        .unwrap();
    let global = fleet
        .f
        .store
        .db
        .one("SELECT events FROM global_usage WHERE id=1", &[])
        .await
        .unwrap()
        .unwrap();
    assert_eq!(
        (i(&usage, "events"), i(&global, "events")),
        (events, events)
    );
    fleet.close().await;
}

#[tokio::test]
async fn attached_direct_enrollments_report_events_as_the_visible_computer() {
    let fleet = Fleet::new().await;
    let (_, direct) = fleet.direct("Direct").await;
    fleet.direct_beat(&direct, &plain()).await.unwrap();
    fleet.direct_beat(&direct, &question()).await.unwrap();
    assert_eq!(
        fleet.events().await,
        [(fleet.target.clone(), "attention".to_string())]
    );
    fleet.close().await;
}

#[tokio::test]
async fn revoking_an_attached_alias_blocks_the_peer_machine() {
    let fleet = Fleet::new().await;
    let (alias, direct) = fleet.direct("Direct").await;
    fleet.direct_beat(&direct, &plain()).await.unwrap();
    fleet.f.store.revoke_computer(&alias).await.unwrap();
    assert!(fleet.rows(&fleet.remote).await.is_empty());
    assert_eq!(fleet.submit(&inspect(&alias)).await, 409);
    fleet.close().await;
}

#[tokio::test]
async fn the_catalog_refuses_the_selected_snapshot_aggregate_before_fetching() {
    let fleet = Fleet::new().await;
    // Each snapshot is within its own bound; together they are too large.
    let large = snapshot("session", json!({"padding":"x".repeat(550_000)}));
    fleet.manifest(&fleet.peers, &large).await.unwrap();
    fleet
        .peer(&fleet.route, &fleet.remote, &large)
        .await
        .unwrap();
    let refused = fleet.f.store.computers(&fleet.f.phone).await.unwrap_err();
    assert_eq!(refused.0.as_u16(), 413);
    fleet.close().await;
}

#[tokio::test]
async fn the_catalog_wire_bound_counts_escaped_peer_names() {
    let fleet = Fleet::new().await;
    let peers: Vec<_> = (0..20)
        .map(|_| json!({"route_id":Uuid::new_v4().to_string(),"machine_id":Uuid::new_v4().to_string(),"name":"😀".repeat(128),"online":true}))
        .collect();
    fleet.manifest(&json!(peers), &plain()).await.unwrap();
    let padded = snapshot("session", json!({"padding":"x".repeat(PEER_PADDING)}));
    for peer in &peers {
        fleet
            .peer(s(peer, "route_id"), s(peer, "machine_id"), &padded)
            .await
            .unwrap();
    }
    // Both the projected catalog and HTTP envelope count twelve bytes per
    // escaped emoji and refuse before returning an oversized phone catalog.
    assert_eq!(
        fleet
            .f
            .store
            .computers(&fleet.f.phone)
            .await
            .unwrap_err()
            .0
            .as_u16(),
        413
    );
    let server = Server::start(&fleet.f.store).await;
    let response = reqwest::Client::new()
        .get(server.at("/v1/computers"))
        .bearer_auth(s(&fleet.f.paired, "device_token"))
        .send()
        .await
        .unwrap();
    assert_eq!(response.status(), 413);
    server.stop().await;
    fleet.close().await;
}
/// Padding that keeps 20 peer snapshots below the 1 MiB catalog preflight
/// while their escaped names take the phone's envelope above it.
const PEER_PADDING: usize = 50_800;

#[tokio::test]
async fn the_registry_ceiling_rolls_back_the_whole_manifest() {
    let fleet = Fleet::new().await;
    let workspace = fleet.f.phone.workspace.clone();
    // Fill the workspace to its 2,048 computers, in bounded statements.
    let mut missing = 2048 - fleet.f.count("computers").await;
    while missing > 0 {
        let rows = missing.min(500);
        let values = vec!["(?,?,?)"; rows as usize].join(",");
        let ids: Vec<_> = (0..rows).map(|_| Uuid::new_v4().to_string()).collect();
        let mut arguments = vec![];
        for id in &ids {
            arguments.extend([
                zerus_relay::db::Arg::from(id),
                zerus_relay::db::Arg::from(&workspace),
                zerus_relay::db::Arg::from("Synthetic computer"),
            ]);
        }
        fleet
            .f
            .store
            .db
            .exec(
                &format!("INSERT INTO computers(id,workspace_id,name) VALUES {values}"),
                &arguments,
            )
            .await
            .unwrap();
        missing -= rows;
    }
    let mut peers = fleet.peers.clone();
    peers.as_array_mut().unwrap().push(json!({"route_id":Uuid::new_v4().to_string(),"machine_id":Uuid::new_v4().to_string(),"name":"Extra","online":true}));
    let refused = fleet.manifest(&peers, &plain()).await.unwrap_err();
    assert_eq!(refused.0.as_u16(), 429);
    assert_eq!(fleet.f.count("computers").await, 2048);
    assert_eq!(fleet.f.count("computer_routes WHERE local=0").await, 1);
    // The accepted manifest still stands.
    fleet
        .peer(&fleet.route, &fleet.remote, &plain())
        .await
        .unwrap();
    assert_eq!(fleet.rows(&fleet.remote).await[0]["online"], true);
    fleet.close().await;
}

#[tokio::test]
async fn gateway_metadata_and_its_opt_in_are_strict_over_http() {
    let fleet = Fleet::new().await;
    let server = Server::start(&fleet.f.store).await;
    let client = reqwest::Client::new();
    let token = s(&fleet.f.credentials, "node_token");
    let mut via = fleet.peers.clone();
    via[0]["via"] = json!("ssh-alias");
    let repeated: Vec<_> = (0..33).map(|_| fleet.peers[0].clone()).collect();
    for body in [
        json!({"snapshot":plain(),"machine_id":fleet.local,"peers":via}),
        json!({"snapshot":plain(),"machine_id":fleet.local,"peers":repeated}),
        json!({"snapshot":plain(),"machine_id":"bad","peers":[]}),
    ] {
        let response = client
            .post(server.at("/v1/node/heartbeat"))
            .bearer_auth(token)
            .json(&body)
            .send()
            .await
            .unwrap();
        assert_eq!(response.status(), 400, "{body}");
    }
    let poll = |flag: &str| {
        client
            .get(server.at(&format!("/v1/node/requests?gateway_one_hop={flag}")))
            .bearer_auth(token)
            .send()
    };
    assert_eq!(poll("ssh").await.unwrap().status(), 400);
    assert_eq!(poll("1").await.unwrap().status(), 200);
    let peer = format!("/v1/node/peers/{}/heartbeat", fleet.route);
    let beat = |body: Value| {
        client
            .post(server.at(&peer))
            .bearer_auth(token)
            .json(&body)
            .send()
    };
    let extended = json!({"machine_id":fleet.remote,"snapshot":plain(),"peers":[]});
    assert_eq!(beat(extended).await.unwrap().status(), 400);
    let exact = json!({"machine_id":fleet.remote,"snapshot":plain()});
    assert_eq!(beat(exact).await.unwrap().status(), 200);
    server.stop().await;
    fleet.close().await;
}
