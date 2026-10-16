//! The one-hop gateway (`fleet.py`): the direct peers the native inventory
//! names, a stable route per alias and machine, guarded native calls to a
//! peer through `hgs swarm mobile-peer`, and the heartbeat manifest.
//!
//! Only the native inventory decides aliases; relay data never supplies one.
//! Every peer call rechecks its route at dispatch, so a route withdrawn or
//! rebound meanwhile runs nothing. Reviewed difference from Python: a relay
//! that refused the extended heartbeat is tried again after 300 s rather
//! than only after a restart (d).
use crate::native::{Call, Native, NativeError, NativeResult};
use crate::pyvalue::canonical_uuid;
use futures_util::future::LocalBoxFuture;
use serde_json::{json, Map, Value};
use std::cell::{Cell, RefCell};
use std::collections::{BTreeMap, HashSet};
use std::rc::Rc;
use std::time::Duration;
use tokio::sync::Semaphore;

pub const MAX_PEERS: usize = 32;
/// A peer listing, and a peer heartbeat body, stay within this many bytes.
pub const MAX_PEER_SNAPSHOT: usize = 1024 * 1024;
/// `swarm mobile-peers --json` output and time budgets.
pub const INVENTORY_BYTES: usize = 128 * 1024;
pub const INVENTORY_TIMEOUT: f64 = 12.0;
/// Concurrent native calls through peers.
pub const PEER_SLOTS: usize = 4;
/// A refused extended heartbeat is retried after this long (d).
pub const RELAY_RETRY: f64 = 300.0;
/// `fleet.ROUTE_NAMESPACE`. Stable across upgrades: an alias rebound to
/// another machine gets a new route.
pub const ROUTE_NAMESPACE: &str = "fab5cf3c-2c74-5dcc-9a8c-83c5f1f2d7de";

pub const ROUTE_UNAVAILABLE: &str = "direct peer route is unavailable or changed";
pub const PEER_TIMED_OUT: &str = "native peer command timed out";
pub const INVALID_INVENTORY: &str = "native direct peer inventory has invalid format";
pub const INVALID_ROUTE: &str = "invalid immutable gateway route";
pub const ROUTING_UNAVAILABLE: &str = "guarded native routing is unavailable";
pub const LOCAL_CHANGED: &str = "local native machine identity changed";
pub const PEER_TOO_LARGE: &str = "peer snapshot exceeds relay size limit";

/// One direct peer route.
#[derive(Debug, Clone, PartialEq)]
pub struct Route {
    pub route_id: String,
    pub machine_id: String,
    pub via: String,
    pub name: String,
    pub online: bool,
}

impl Route {
    /// The manifest row a heartbeat lists.
    pub fn manifest(&self) -> Value {
        json!({"route_id": self.route_id, "machine_id": self.machine_id, "name": self.name,
               "online": self.online})
    }
}

pub type Routes = Rc<RefCell<BTreeMap<String, Route>>>;

fn uuid_bytes(canonical: &str) -> [u8; 16] {
    let hex: Vec<u8> = canonical.bytes().filter(|byte| *byte != b'-').collect();
    let mut bytes = [0u8; 16];
    for (index, pair) in hex.chunks(2).enumerate().take(16) {
        let text = std::str::from_utf8(pair).unwrap_or("00");
        bytes[index] = u8::from_str_radix(text, 16).unwrap_or(0);
    }
    bytes
}

/// RFC 4122 name-based UUID, version 5: the first 16 bytes of
/// SHA-1(namespace + UTF-8 name) with the version and variant bits set.
fn uuid5(namespace: &[u8; 16], name: &str) -> String {
    let mut input = namespace.to_vec();
    input.extend_from_slice(name.as_bytes());
    let hash = ring::digest::digest(&ring::digest::SHA1_FOR_LEGACY_USE_ONLY, &input);
    let mut bytes = [0u8; 16];
    bytes.copy_from_slice(&hash.as_ref()[..16]);
    bytes[6] = (bytes[6] & 0x0f) | 0x50;
    bytes[8] = (bytes[8] & 0x3f) | 0x80;
    let hex: String = bytes.iter().fold(String::new(), |mut hex, byte| {
        use std::fmt::Write;
        let _ = write!(hex, "{byte:02x}");
        hex
    });
    format!(
        "{}-{}-{}-{}-{}",
        &hex[..8],
        &hex[8..12],
        &hex[12..16],
        &hex[16..20],
        &hex[20..]
    )
}

/// `fleet.route_id(via, machine_id)`: `uuid5(ROUTE_NAMESPACE, via + NUL +
/// machine_id)`.
pub fn route_id(via: &str, machine_id: &str) -> String {
    uuid5(
        &uuid_bytes(ROUTE_NAMESPACE),
        &format!("{via}\0{machine_id}"),
    )
}

fn uuid_text(value: Option<&Value>) -> Option<&str> {
    value
        .and_then(Value::as_str)
        .filter(|text| canonical_uuid(text))
}

/// `Fleet.discover`'s check of `swarm mobile-peers --json`: the local
/// machine and the routes of its direct peers, or an error that withdraws
/// every route.
pub fn inventory(raw: &Value) -> Result<(String, BTreeMap<String, Route>), &'static str> {
    let raw = raw.as_object().ok_or(INVALID_INVENTORY)?;
    let local = raw.get("local").and_then(Value::as_object);
    let peers = raw.get("peers").and_then(Value::as_array);
    let schema_one = crate::pyvalue::py_eq(raw.get("schema").unwrap_or(&Value::Null), &json!(1));
    let (Some(local), Some(peers)) = (local, peers) else {
        return Err(INVALID_INVENTORY);
    };
    let Some(local_id) = uuid_text(local.get("machine_id")) else {
        return Err(INVALID_INVENTORY);
    };
    if !schema_one || peers.len() > MAX_PEERS {
        return Err(INVALID_INVENTORY);
    }
    let mut routes = BTreeMap::new();
    let mut aliases = HashSet::new();
    for row in peers {
        let row = row.as_object().ok_or(INVALID_INVENTORY)?;
        let via = row.get("via").and_then(Value::as_str);
        let online = row.get("online").and_then(Value::as_bool);
        let (Some(via), Some(online)) = (via, online) else {
            return Err(INVALID_INVENTORY);
        };
        if via.is_empty()
            || via.chars().count() > 512
            || via.chars().any(|c| (c as u32) < 32 || c as u32 == 127)
            || !aliases.insert(via.to_owned())
        {
            return Err(INVALID_INVENTORY);
        }
        let machine = row.get("machine_id");
        // An unreachable peer is advertised offline only under the identity
        // this gateway last verified through the same alias.
        if machine.is_none_or(Value::is_null) && !online {
            continue;
        }
        let Some(machine) = uuid_text(machine) else {
            return Err(INVALID_INVENTORY);
        };
        if machine == local_id {
            continue;
        }
        let identity = route_id(via, machine);
        let name = match row.get("name") {
            Some(Value::String(name)) => name
                .chars()
                .filter(|c| (*c as u32) >= 32 && *c as u32 != 127)
                .take(128)
                .collect(),
            _ => "Peer computer".to_owned(),
        };
        let name = if name.is_empty() {
            "Peer computer".to_owned()
        } else {
            name
        };
        routes.insert(
            identity.clone(),
            Route {
                route_id: identity,
                machine_id: machine.to_owned(),
                via: via.to_owned(),
                name,
                online,
            },
        );
    }
    Ok((local_id.to_owned(), routes))
}

/// `Fleet.resolve`'s envelope check: `Ok(None)` for this computer, or the
/// route and machine of a peer.
pub fn route_target(envelope: &Value) -> Result<(Option<String>, String), &'static str> {
    let object = envelope.as_object().ok_or(INVALID_ROUTE)?;
    let mut keys: Vec<&str> = object.keys().map(String::as_str).collect();
    keys.sort_unstable();
    let route = object.get("route_id");
    let valid = keys == ["computer_id", "machine_id", "route_id", "schema"]
        && crate::pyvalue::int_literal(&object["schema"]).as_deref() == Some("1")
        && uuid_text(object.get("computer_id")).is_some()
        && uuid_text(object.get("machine_id")).is_some()
        && (route.is_some_and(Value::is_null) || uuid_text(route).is_some());
    if !valid {
        return Err(INVALID_ROUTE);
    }
    let machine = object["machine_id"].as_str().expect("checked").to_owned();
    Ok((route.and_then(Value::as_str).map(str::to_owned), machine))
}

/// A native seam usable behind a pointer, so a peer context can call the
/// gateway's native layer whatever its type.
pub trait DynNative {
    fn call_dyn(&self, call: Call) -> LocalBoxFuture<'_, NativeResult>;
}

impl<N: Native> DynNative for N {
    fn call_dyn(&self, call: Call) -> LocalBoxFuture<'_, NativeResult> {
        Box::pin(self.call(call))
    }
}

/// `Fleet.callback(identity, machine)`: a peer context's native layer.
pub struct PeerNative {
    pub gateway: Rc<dyn DynNative>,
    pub routes: Routes,
    pub slots: Rc<Semaphore>,
    pub identity: String,
    pub machine: String,
    pub default_timeout: f64,
}

fn listing(argv: &[String]) -> bool {
    argv.len() >= 3
        && argv[..3] == ["ls", "--json", "--local"]
        && (argv.len() == 3 || (argv.len() == 4 && argv[3] == "--no-archives"))
}

impl Native for PeerNative {
    async fn call(&self, call: Call) -> NativeResult {
        let budget = call.timeout.unwrap_or(self.default_timeout);
        let invoke = async {
            let _slot = self
                .slots
                .acquire()
                .await
                .map_err(|_| NativeError::Stopping)?;
            let route = self.routes.borrow().get(&self.identity).cloned();
            let Some(route) =
                route.filter(|route| route.online && route.machine_id == self.machine)
            else {
                return Err(NativeError::Failed(ROUTE_UNAVAILABLE));
            };
            let envelope = json!({"schema": 1, "via": route.via, "target_machine_id": self.machine,
                                  "argv": call.argv, "payload": call.payload});
            let max_output_bytes = if listing(&call.argv) {
                Some(MAX_PEER_SNAPSHOT)
            } else {
                call.max_output_bytes
            };
            let outer = Call {
                argv: ["swarm", "mobile-peer", "--json"].map(String::from).into(),
                payload: Some(envelope),
                timeout: Some(budget),
                json_output: call.json_output,
                failure_json: call.failure_json,
                max_output_bytes,
            };
            self.gateway.call_dyn(outer).await
        };
        let limit = Duration::try_from_secs_f64(budget).unwrap_or(Duration::MAX);
        match tokio::time::timeout(limit, invoke).await {
            Ok(result) => result,
            Err(_) => Err(NativeError::Failed(PEER_TIMED_OUT)),
        }
    }
}

/// The gateway state of the main connector (`Fleet`). Peer contexts are
/// created by [`crate::runtime::Connector::discover`].
pub struct Fleet {
    pub gateway: Rc<dyn DynNative>,
    pub routes: Routes,
    pub contexts: RefCell<BTreeMap<String, Rc<crate::runtime::Connector<PeerNative>>>>,
    pub local_id: RefCell<Option<String>>,
    /// Routes the relay accepted in the last manifest.
    pub published: RefCell<HashSet<(String, String)>>,
    pub relay_supported: Cell<bool>,
    /// When a refused extended heartbeat is tried again (d).
    pub relay_retry_at: Cell<f64>,
    pub slots: Rc<Semaphore>,
    pub discovery: tokio::sync::Mutex<()>,
}

impl Fleet {
    pub fn new(gateway: Rc<dyn DynNative>) -> Self {
        Fleet {
            gateway,
            routes: Rc::default(),
            contexts: RefCell::default(),
            local_id: RefCell::default(),
            published: RefCell::default(),
            relay_supported: Cell::new(true),
            relay_retry_at: Cell::new(0.0),
            slots: Rc::new(Semaphore::new(PEER_SLOTS)),
            discovery: tokio::sync::Mutex::new(()),
        }
    }

    /// The heartbeat body: the snapshot, and with a usable gateway the local
    /// machine and every route's manifest row.
    pub fn heartbeat(&self, snapshot: Value, available: bool) -> Map<String, Value> {
        let mut body = Map::new();
        body.insert("snapshot".into(), snapshot);
        if available {
            body.insert("machine_id".into(), json!(self.local_id.borrow().clone()));
            let peers: Vec<Value> = self.routes.borrow().values().map(Route::manifest).collect();
            body.insert("peers".into(), Value::Array(peers));
        }
        body
    }

    /// `confirm_manifest`: the online routes the relay just accepted.
    pub fn confirm_manifest(&self, body: &Map<String, Value>) {
        let published = body
            .get("peers")
            .and_then(Value::as_array)
            .into_iter()
            .flatten()
            .filter(|row| row["online"] == json!(true))
            .filter_map(|row| {
                Some((
                    row["route_id"].as_str()?.to_owned(),
                    row["machine_id"].as_str()?.to_owned(),
                ))
            })
            .collect();
        *self.published.borrow_mut() = published;
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const LOCAL: &str = "11111111-1111-4111-8111-111111111111";
    const PEER: &str = "22222222-2222-4222-8222-222222222222";

    #[test]
    fn uuid5_matches_rfc_4122() {
        // Python: uuid.uuid5(uuid.NAMESPACE_DNS, "python.org").
        let dns = uuid_bytes("6ba7b810-9dad-11d1-80b4-00c04fd430c8");
        assert_eq!(
            uuid5(&dns, "python.org"),
            "886313e1-3b8a-5372-9b90-0c9aee199e5d"
        );
    }

    #[test]
    fn inventories_fail_closed_and_skip_this_computer() {
        let good = json!({"schema": 1, "local": {"machine_id": LOCAL}, "peers": [
            {"via": "peer", "machine_id": PEER, "online": true, "name": "Lab\u{7f} box"},
            {"via": "self", "machine_id": LOCAL, "online": true},
            {"via": "gone", "machine_id": null, "online": false}]});
        let (local, routes) = inventory(&good).unwrap();
        assert_eq!(local, LOCAL);
        let route = routes.values().next().unwrap();
        assert_eq!(
            (routes.len(), route.name.as_str(), route.route_id.clone()),
            (1, "Lab box", route_id("peer", PEER))
        );
        for bad in [
            json!({"schema": 2, "local": {"machine_id": LOCAL}, "peers": []}),
            json!({"schema": 1, "local": {"machine_id": "x"}, "peers": []}),
            json!({"schema": 1, "local": {"machine_id": LOCAL}, "peers": [{"via": "a", "machine_id": PEER, "online": 1}]}),
            json!({"schema": 1, "local": {"machine_id": LOCAL}, "peers": [
                {"via": "a", "machine_id": PEER, "online": true}, {"via": "a", "machine_id": PEER, "online": true}]}),
            json!({"schema": 1, "local": {"machine_id": LOCAL}, "peers": [{"via": "a", "machine_id": null, "online": true}]}),
        ] {
            assert_eq!(inventory(&bad).map(|_| ()), Err(INVALID_INVENTORY), "{bad}");
        }
    }

    #[test]
    fn routes_name_this_computer_or_an_exact_peer() {
        let local =
            json!({"schema": 1, "route_id": null, "computer_id": LOCAL, "machine_id": LOCAL});
        assert_eq!(route_target(&local), Ok((None, LOCAL.to_owned())));
        let peer = json!({"schema": 1, "route_id": PEER, "computer_id": LOCAL, "machine_id": PEER});
        assert_eq!(
            route_target(&peer),
            Ok((Some(PEER.to_owned()), PEER.to_owned()))
        );
        for bad in [
            json!({"schema": 1, "computer_id": LOCAL, "machine_id": LOCAL}),
            json!({"schema": true, "route_id": null, "computer_id": LOCAL, "machine_id": LOCAL}),
            json!({"schema": 1, "route_id": "x", "computer_id": LOCAL, "machine_id": LOCAL}),
        ] {
            assert_eq!(route_target(&bad), Err(INVALID_ROUTE), "{bad}");
        }
    }
}
