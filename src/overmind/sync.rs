//! Replication between machines (spec Part 2). One call pushes and pulls, so a
//! peer reachable in one direction only still converges. The transport is the
//! caller's business: the swarm worker passes its SSH call.
use super::model::now;
use super::store::{PeerState, Store};
use anyhow::{anyhow, ensure, Result};
use serde::{Deserialize, Serialize};
use serde_json::Value;
use std::collections::BTreeMap;

/// Advertised in swarm `hello`; peers without it are skipped.
pub const FEATURE: &str = "overmind:1";
pub const BATCH: usize = 4 * 1024 * 1024;
const ROUNDS: usize = 8;

#[derive(Serialize, Deserialize)]
struct ExchangeRequest {
    v: u32,
    from: String,
    vector: BTreeMap<String, u64>,
    #[serde(default)]
    events: Vec<Value>,
}

#[derive(Serialize, Deserialize)]
struct ExchangeResponse {
    v: u32,
    machine: String,
    vector: BTreeMap<String, u64>,
    #[serde(default)]
    events: Vec<Value>,
    #[serde(default)]
    more: bool,
}

#[derive(Debug, Default, PartialEq)]
pub struct Summary {
    pub rounds: usize,
    pub sent: usize,
    pub inserted: usize,
    pub refused: usize,
    /// Events pushed that the peer still lacks after its answer.
    pub not_accepted: usize,
}

/// The peer's side of one call: store what arrived, answer with what the caller lacks.
pub fn serve(store: &mut Store, request: Value) -> Result<Value> {
    let request: ExchangeRequest =
        serde_json::from_value(request).map_err(|e| anyhow!("invalid Overmind exchange request: {e}"))?;
    ensure!(request.v == 1, "this machine speaks Overmind exchange version 1; update HGS on the newer machine");
    ensure!(
        uuid::Uuid::parse_str(&request.from).is_ok() && request.from != store.machine(),
        "invalid Overmind exchange request"
    );
    store.insert_raw(&request.events)?;
    let (events, more) = store.missing(&request.vector, BATCH)?;
    let response = ExchangeResponse { v: 1, machine: store.machine().into(), vector: store.version_vector()?, events, more };
    Ok(serde_json::to_value(response)?)
}

/// Exchanges with one peer until both sides are current, a round makes no
/// progress or the rounds run out; the outcome is kept for `status`.
pub fn exchange(store: &mut Store, peer: &str, alias: &str, call: &mut dyn FnMut(&Value) -> Result<Value>) -> Result<Summary> {
    let result = rounds(store, peer, call);
    let bookkeeping = || -> Result<()> {
        let mut state = store.peer(peer)?.unwrap_or_else(|| PeerState { machine: peer.into(), ..PeerState::default() });
        state.alias = alias.into();
        state.updated = now();
        match &result {
            Ok(summary) => {
                state.last_ok = state.updated;
                state.last_error = if summary.not_accepted > 0 {
                    format!("the peer did not accept {} events; it may run an older or damaged store", summary.not_accepted)
                } else {
                    String::new()
                };
            }
            Err(error) => state.last_error = format!("{error:#}"),
        }
        store.put_peer(&state)
    };
    match bookkeeping() {
        // A failed call reports its own error, not the bookkeeping's.
        Err(error) if result.is_ok() => Err(error),
        _ => result,
    }
}

fn rounds(store: &mut Store, peer: &str, call: &mut dyn FnMut(&Value) -> Result<Value>) -> Result<Summary> {
    let mut summary = Summary::default();
    while summary.rounds < ROUNDS {
        let known = store.peer(peer)?.and_then(|p| p.vector);
        let (events, push_more) = match &known {
            Some(vector) => store.missing(vector, BATCH)?,
            None => (Vec::new(), true),
        };
        let pushed = events.len();
        let pushed_origins: std::collections::BTreeSet<String> =
            events.iter().filter_map(|e| e["origin"].as_str().map(str::to_owned)).collect();
        summary.sent += pushed;
        let request = ExchangeRequest { v: 1, from: store.machine().into(), vector: store.version_vector()?, events };
        let response: ExchangeResponse = serde_json::from_value(call(&serde_json::to_value(&request)?)?)
            .map_err(|e| anyhow!("the peer's Overmind answer is not valid: {e}"))?;
        ensure!(response.v == 1, "this peer speaks another Overmind exchange version; update HGS on the older machine");
        ensure!(response.machine == peer, "the machine behind this peer changed identity");
        let received = store.insert_raw(&response.events)?;
        summary.inserted += received.inserted;
        summary.refused += received.refused;
        summary.rounds += 1;
        let progress = known.as_ref() != Some(&response.vector) || received.inserted > 0;
        let mut state = store.peer(peer)?.unwrap_or_else(|| PeerState { machine: peer.into(), ..PeerState::default() });
        state.vector = Some(response.vector.clone());
        store.put_peer(&state)?;
        // What the peer still lacks according to its fresh answer: a peer that
        // lost its data is refilled from here on.
        let (lacking, _) = store.missing(&response.vector, BATCH)?;
        // A peer below the vector this round assumed lost data: push from its
        // fresh vector next round. Otherwise events of an origin we pushed to
        // that did not move the peer's seq were refused.
        let lost_data = known
            .as_ref()
            .map_or(false, |k| k.iter().any(|(origin, seq)| response.vector.get(origin).copied().unwrap_or(0) < *seq));
        if pushed > 0 && !push_more && !lost_data {
            let unmoved = |origin: &str| {
                let before = known.as_ref().and_then(|k| k.get(origin)).copied().unwrap_or(0);
                response.vector.get(origin).copied().unwrap_or(0) == before
            };
            let refused = lacking.iter().filter(|e| e["origin"].as_str().map_or(false, |o| pushed_origins.contains(o) && unmoved(o))).count();
            if refused > 0 {
                summary.not_accepted = refused;
                break;
            }
        }
        if !progress || !(push_more || response.more || lost_data || (pushed == 0 && !lacking.is_empty())) {
            break;
        }
    }
    Ok(summary)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::overmind::fold::fold_many;
    use crate::overmind::model::{Actor, Draft};
    use crate::overmind::store::Filter;
    use serde_json::json;

    const A: &str = "11111111-1111-4111-8111-111111111111";
    const B: &str = "22222222-2222-4222-8222-222222222222";
    const C: &str = "33333333-3333-4333-8333-333333333333";

    fn comment(store: &mut Store, task: &str, size: usize) {
        let machine = store.machine().to_owned();
        store
            .write(|tx| {
                tx.append(&Draft {
                    actor: Actor::human(&machine), task: task.into(), project: "p".into(), kind: "journal".into(),
                    base: String::new(), request_id: String::new(),
                    payload: json!({"marker": "[comment]", "text": "x".repeat(size)}),
                })
            })
            .unwrap();
    }
    fn stores() -> (tempfile::TempDir, Store, Store) {
        let dir = tempfile::tempdir().unwrap();
        let a = Store::open(&dir.path().join("a"), A).unwrap();
        let b = Store::open(&dir.path().join("b"), B).unwrap();
        (dir, a, b)
    }

    #[test]
    fn one_exchange_converges_both_ways() {
        let (_dir, mut a, mut b) = stores();
        for _ in 0..3 { comment(&mut a, "t", 10); }
        for _ in 0..2 { comment(&mut b, "u", 10); }
        let mut first = true;
        let summary = exchange(&mut a, B, "b", &mut |request| {
            if first {
                assert!(request["events"].as_array().unwrap().is_empty(), "the first round only pulls");
                first = false;
            }
            serve(&mut b, request.clone())
        })
        .unwrap();
        assert_eq!((summary.inserted, summary.refused), (2, 0));
        assert_eq!(a.version_vector().unwrap(), b.version_vector().unwrap());
        let all = |s: &Store| serde_json::to_value(fold_many(&s.load(&Filter::All).unwrap())).unwrap();
        assert_eq!(all(&a), all(&b));
        let peer = a.peer(B).unwrap().unwrap();
        assert_eq!((peer.alias.as_str(), peer.last_error.as_str(), peer.vector.is_some()), ("b", "", true));
        assert!(peer.last_ok > 0);
    }

    #[test]
    fn large_backlogs_move_in_batches() {
        let (_dir, mut a, mut b) = stores();
        for _ in 0..80 { comment(&mut b, "t", 60 * 1024); }
        let summary = exchange(&mut a, B, "b", &mut |request| serve(&mut b, request.clone())).unwrap();
        assert!(summary.rounds >= 2, "{summary:?}");
        assert_eq!(a.version_vector().unwrap(), b.version_vector().unwrap());
    }

    #[test]
    fn a_failed_call_changes_nothing_and_is_reported() {
        let (_dir, mut a, mut b) = stores();
        comment(&mut b, "t", 10);
        let before = a.cursor().unwrap();
        assert!(exchange(&mut a, B, "b", &mut |_| anyhow::bail!("connection to b timed out")).is_err());
        assert!(exchange(&mut a, B, "b", &mut |_| Ok(json!({"v": 1, "machine": A, "vector": {}, "events": []}))).is_err());
        assert_eq!(a.cursor().unwrap(), before);
        let peer = a.peer(B).unwrap().unwrap();
        assert!(peer.last_error.contains("identity"), "{peer:?}");
        exchange(&mut a, B, "b", &mut |request| serve(&mut b, request.clone())).unwrap();
        assert_eq!(a.peer(B).unwrap().unwrap().last_error, "");
    }

    #[test]
    fn serve_refuses_other_versions_and_itself() {
        let (_dir, _a, mut b) = stores();
        assert!(serve(&mut b, json!({"v": 2, "from": A, "vector": {}})).is_err());
        assert!(serve(&mut b, json!({"v": 1, "from": B, "vector": {}})).is_err());
        assert!(serve(&mut b, json!({"v": 1, "from": A, "vector": {}, "later": true})).is_ok());
    }

    #[test]
    fn a_peer_that_lost_its_data_is_refilled_in_one_exchange() {
        let (dir, mut a, mut b) = stores();
        for _ in 0..3 { comment(&mut a, "t", 10); }
        exchange(&mut a, B, "b", &mut |request| serve(&mut b, request.clone())).unwrap();
        drop(b);
        let mut fresh = Store::open(&dir.path().join("fresh"), B).unwrap();
        exchange(&mut a, B, "b", &mut |request| serve(&mut fresh, request.clone())).unwrap();
        assert_eq!(a.version_vector().unwrap(), fresh.version_vector().unwrap());
    }

    #[test]
    fn a_wiped_peer_is_refilled_even_when_this_machine_has_newer_events() {
        let (dir, mut a, mut b) = stores();
        for _ in 0..3 { comment(&mut a, "t", 10); }
        exchange(&mut a, B, "b", &mut |request| serve(&mut b, request.clone())).unwrap();
        for _ in 0..2 { comment(&mut a, "t", 10); }
        drop(b);
        let mut fresh = Store::open(&dir.path().join("fresh"), B).unwrap();
        exchange(&mut a, B, "b", &mut |request| serve(&mut fresh, request.clone())).unwrap();
        assert_eq!(a.version_vector().unwrap(), fresh.version_vector().unwrap());
        assert_eq!(a.peer(B).unwrap().unwrap().last_error, "");
    }

    #[test]
    fn a_peer_that_refuses_events_stops_the_exchange_and_is_reported() {
        let (_dir, mut a, mut b) = stores();
        for _ in 0..3 { comment(&mut a, "t", 10); }
        comment(&mut b, "u", 10);
        // A also holds events of a third origin, which B accepts.
        let dir_c = tempfile::tempdir().unwrap();
        let mut c = Store::open(dir_c.path(), C).unwrap();
        comment(&mut c, "v", 10);
        a.insert_raw(&c.missing(&BTreeMap::new(), BATCH).unwrap().0).unwrap();
        // B already holds A's first event id at another position, so it refuses A's events.
        let first = a.missing(&BTreeMap::new(), BATCH).unwrap().0.into_iter().find(|e| e["origin"] == A && e["seq"] == 1).unwrap();
        let mut forged = first.clone();
        forged["origin"] = json!("44444444-4444-4444-8444-444444444444");
        b.insert_raw(&[forged]).unwrap();
        let summary = exchange(&mut a, B, "b", &mut |request| serve(&mut b, request.clone())).unwrap();
        assert!(summary.rounds <= 2, "{summary:?}");
        assert!(summary.not_accepted > 0, "{summary:?}");
        let peer = a.peer(B).unwrap().unwrap();
        assert!(peer.last_error.contains("did not accept"), "{peer:?}");
        assert!(a.version_vector().unwrap().get(B).copied().unwrap_or(0) >= 1, "B's own origin reached A");
        assert_eq!(b.version_vector().unwrap().get(C).copied(), Some(1), "a pushed origin B accepted reached B");
    }
}
