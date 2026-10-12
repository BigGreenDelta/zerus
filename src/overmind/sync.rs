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
    /// How many events of the request this side refused.
    #[serde(default)]
    refused: usize,
}

#[derive(Debug, Default, PartialEq)]
pub struct Summary {
    pub rounds: usize,
    pub sent: usize,
    pub inserted: usize,
    /// Events the peer sent that this store refused: damaged, or another event
    /// at a position it already holds.
    pub refused: usize,
    /// Events pushed that the peer still lacks after its answer.
    pub not_accepted: usize,
    /// Events this store pushed that the peer reported refusing.
    pub peer_refused: usize,
    /// This store was older than the peer's copy and now writes under a fresh origin.
    pub rotated: bool,
}

impl Summary {
    /// What `status` shows for this exchange; empty when it was clean.
    fn problems(&self, peer: &str) -> String {
        let mut problems = Vec::new();
        if self.rotated {
            problems.push(restored_note(peer));
        }
        if self.peer_refused > 0 {
            problems.push(format!(
                "the peer refused {} events from this machine: damaged, or other events at positions it already holds",
                self.peer_refused
            ));
        }
        if self.not_accepted > 0 {
            problems.push(format!("the peer did not accept {} events; it may run an older or damaged store", self.not_accepted));
        }
        if self.refused > 0 {
            problems.push(format!(
                "this machine refused {} events from the peer: damaged, or other events at positions it already holds",
                self.refused
            ));
        }
        problems.join("; ")
    }
}

fn restored_note(peer: &str) -> String {
    format!(
        "this store is older than its copy on {peer}; new events use a fresh origin; \
         tasks changed here before the exchange may differ from the other machines"
    )
}

/// The other side holds more events of this store's own origin than this store
/// does: this is an older copy (restored from a backup) that peers have outgrown,
/// and writing on under the same origin would collide with what they hold.
fn outgrown(own: &BTreeMap<String, u64>, theirs: &BTreeMap<String, u64>, origin: &str) -> bool {
    theirs.get(origin).copied().unwrap_or(0) > own.get(origin).copied().unwrap_or(0)
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
    let (origin, current) = store.current()?;
    if outgrown(&current, &request.vector, &origin) {
        store.rotate_origin()?;
        let mut state = store
            .peer(&request.from)?
            .unwrap_or_else(|| PeerState { machine: request.from.clone(), ..PeerState::default() });
        let name = if state.alias.is_empty() { request.from.clone() } else { state.alias.clone() };
        state.last_error = restored_note(&name);
        state.updated = now();
        store.put_peer(&state)?;
    }
    let received = store.insert_raw(&request.events)?;
    let (events, more) = store.missing(&request.vector, BATCH)?;
    let response = ExchangeResponse {
        v: 1,
        machine: store.machine().into(),
        vector: store.version_vector()?,
        events,
        more,
        refused: received.refused,
    };
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
                state.last_error = summary.problems(if alias.is_empty() { peer } else { alias });
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
        let own = store.version_vector()?;
        let request = ExchangeRequest { v: 1, from: store.machine().into(), vector: own.clone(), events };
        let response: ExchangeResponse = serde_json::from_value(call(&serde_json::to_value(&request)?)?)
            .map_err(|e| anyhow!("the peer's Overmind answer is not valid: {e}"))?;
        ensure!(response.v == 1, "this peer speaks another Overmind exchange version; update HGS on the older machine");
        ensure!(response.machine == peer, "the machine behind this peer changed identity");
        summary.peer_refused += response.refused;
        // Before anything is written here again: peers hold more of this
        // store's origin than this store does. Another process may have written
        // or rotated since the call began, so the store is read again now.
        let (origin, current) = store.current()?;
        if outgrown(&current, &response.vector, &origin) {
            store.rotate_origin()?;
            summary.rotated = true;
        }
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
        // that did not move the peer's seq were refused, whether or not more
        // batches remain: each pushed origin starts right after the peer's seq.
        let lost_data = known
            .as_ref()
            .map_or(false, |k| k.iter().any(|(origin, seq)| response.vector.get(origin).copied().unwrap_or(0) < *seq));
        if pushed > 0 && !lost_data {
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
    fn damaged_events_from_the_peer_are_reported_until_a_clean_exchange() {
        let (_dir, mut a, mut b) = stores();
        comment(&mut b, "t", 10);
        let summary = exchange(&mut a, B, "b", &mut |request| {
            let mut answer = serve(&mut b, request.clone())?;
            answer["events"].as_array_mut().unwrap().push(json!({"id": "damaged"}));
            Ok(answer)
        })
        .unwrap();
        assert!(summary.refused > 0, "{summary:?}");
        assert_eq!(a.version_vector().unwrap(), b.version_vector().unwrap(), "the good events still arrived");
        let peer = a.peer(B).unwrap().unwrap();
        assert!(peer.last_error.contains("damaged"), "{peer:?}");
        exchange(&mut a, B, "b", &mut |request| serve(&mut b, request.clone())).unwrap();
        assert_eq!(a.peer(B).unwrap().unwrap().last_error, "");
    }

    #[test]
    fn refused_pushes_are_reported_when_the_backlog_spans_several_batches() {
        let (_dir, mut a, mut b) = stores();
        for _ in 0..80 { comment(&mut a, "t", 60 * 1024); }
        // B already holds A's first event id at another position, so it refuses A's events.
        let first = a.missing(&BTreeMap::new(), BATCH).unwrap().0.into_iter().find(|e| e["seq"] == 1).unwrap();
        let mut forged = first.clone();
        forged["origin"] = json!("44444444-4444-4444-8444-444444444444");
        b.insert_raw(&[forged]).unwrap();
        let summary = exchange(&mut a, B, "b", &mut |request| serve(&mut b, request.clone())).unwrap();
        assert!(summary.not_accepted > 0, "{summary:?}");
        let peer = a.peer(B).unwrap().unwrap();
        assert!(peer.last_error.contains("did not accept"), "{peer:?}");
    }

    #[test]
    fn a_wiped_store_that_writes_before_it_is_refilled_keeps_every_event() {
        let (dir, mut a, mut b) = stores();
        comment(&mut b, "t", 10);
        exchange(&mut a, B, "b", &mut |request| serve(&mut b, request.clone())).unwrap();
        let old = b.load(&Filter::All).unwrap()[0].id.clone();
        drop(b);
        // The same machine starts over with an empty store and writes at once.
        let mut fresh = Store::open(&dir.path().join("fresh"), B).unwrap();
        comment(&mut fresh, "u", 10);
        let new = fresh.load(&Filter::All).unwrap()[0].id.clone();
        exchange(&mut a, B, "b", &mut |request| serve(&mut fresh, request.clone())).unwrap();
        exchange(&mut fresh, A, "a", &mut |request| serve(&mut a, request.clone())).unwrap();
        let ids = |s: &Store| s.load(&Filter::All).unwrap().into_iter().map(|e| e.id).collect::<std::collections::BTreeSet<_>>();
        for store in [&a, &fresh] {
            assert!(ids(store).contains(&old) && ids(store).contains(&new), "{:?}", ids(store));
        }
        assert_eq!(ids(&a), ids(&fresh));
        assert_eq!(a.version_vector().unwrap(), fresh.version_vector().unwrap());
        assert_eq!(a.peer(B).unwrap().unwrap().last_error, "");
        assert_eq!(fresh.peer(A).unwrap().unwrap().last_error, "");
    }

    /// B holds one event, exchanges with A, is copied aside, writes and exchanges
    /// again, then is replaced by the old copy. Returns the restored B and its old origin.
    fn restored_b(dir: &tempfile::TempDir, a: &mut Store) -> (Store, String) {
        let path = dir.path().join("b");
        let mut b = Store::open(&path, B).unwrap();
        comment(&mut b, "t", 10);
        exchange(a, B, "b", &mut |request| serve(&mut b, request.clone())).unwrap();
        let old = b.origin().to_owned();
        drop(b);
        let aside = dir.path().join("aside.sqlite3");
        std::fs::copy(path.join("overmind.sqlite3"), &aside).unwrap();
        let mut b = Store::open(&path, B).unwrap();
        comment(&mut b, "t", 10);
        comment(&mut b, "t", 10);
        exchange(a, B, "b", &mut |request| serve(&mut b, request.clone())).unwrap();
        drop(b);
        for suffix in ["-wal", "-shm"] {
            let _ = std::fs::remove_file(path.join(format!("overmind.sqlite3{suffix}")));
        }
        std::fs::copy(&aside, path.join("overmind.sqlite3")).unwrap();
        (Store::open(&path, B).unwrap(), old)
    }

    #[test]
    fn a_restored_store_rotates_its_origin() {
        // The restored store answers the exchange.
        let dir = tempfile::tempdir().unwrap();
        let mut a = Store::open(&dir.path().join("a"), A).unwrap();
        let (mut b, old) = restored_b(&dir, &mut a);
        assert_eq!(b.origin(), old);
        exchange(&mut a, B, "b", &mut |request| serve(&mut b, request.clone())).unwrap();
        assert_ne!(b.origin(), old, "the served store rotated");
        let note = b.peer(A).unwrap().unwrap().last_error;
        assert!(note.contains("older than its copy on") && note.contains("fresh origin"), "{note}");
        comment(&mut b, "t", 10);
        let latest = b.load(&Filter::All).unwrap().into_iter().max_by_key(|e| e.lamport).unwrap();
        assert_eq!(latest.origin, b.origin());
        assert_eq!(a.version_vector().unwrap().get(&old), Some(&3));
        exchange(&mut a, B, "b", &mut |request| serve(&mut b, request.clone())).unwrap();
        assert_eq!(a.version_vector().unwrap(), b.version_vector().unwrap());

        // The restored store makes the call.
        let dir = tempfile::tempdir().unwrap();
        let mut a = Store::open(&dir.path().join("a"), A).unwrap();
        let (mut b, old) = restored_b(&dir, &mut a);
        exchange(&mut b, A, "a", &mut |request| serve(&mut a, request.clone())).unwrap();
        assert_ne!(b.origin(), old, "the calling store rotated");
        let note = b.peer(A).unwrap().unwrap().last_error;
        assert!(note.contains("older than its copy on a"), "{note}");
        comment(&mut b, "t", 10);
        assert!(b.version_vector().unwrap().contains_key(b.origin()));
        assert_eq!(b.version_vector().unwrap().get(&old), Some(&3), "the events the old origin lost came back");
    }

    #[test]
    fn a_concurrent_write_during_the_exchange_does_not_rotate_the_origin() {
        let dir = tempfile::tempdir().unwrap();
        let mut a = Store::open(&dir.path().join("a"), A).unwrap();
        let mut other = Store::open(&dir.path().join("a"), A).unwrap();
        let mut b = Store::open(&dir.path().join("b"), B).unwrap();
        comment(&mut a, "t", 10);
        let origin = a.origin().to_owned();
        let summary = exchange(&mut a, B, "b", &mut |request| {
            // A second process writes and passes the event on while this call is under way.
            comment(&mut other, "t", 10);
            exchange(&mut other, B, "b", &mut |inner| serve(&mut b, inner.clone()))?;
            serve(&mut b, request.clone())
        })
        .unwrap();
        assert!(!summary.rotated, "{summary:?}");
        assert_eq!(a.origin(), origin);
        assert_eq!(a.peer(B).unwrap().unwrap().last_error, "");
        // A handle follows an origin another process rotated.
        let fresh = other.rotate_origin().unwrap();
        assert_eq!(a.current().unwrap().0, fresh);
        assert_eq!(a.origin(), fresh);
    }

    #[test]
    fn serve_reports_refused_events() {
        let (_dir, mut a, mut b) = stores();
        comment(&mut a, "t", 10);
        let sent = a.missing(&BTreeMap::new(), BATCH).unwrap().0;
        // B holds A's event id at another position, so it refuses A's event.
        let mut forged = sent[0].clone();
        forged["origin"] = json!("44444444-4444-4444-8444-444444444444");
        b.insert_raw(&[forged]).unwrap();
        let request = json!({"v": 1, "from": A, "vector": a.version_vector().unwrap(), "events": sent});
        assert_eq!(serve(&mut b, request).unwrap()["refused"], 1);
        let summary = exchange(&mut a, B, "b", &mut |request| serve(&mut b, request.clone())).unwrap();
        assert!(summary.peer_refused > 0, "{summary:?}");
        let note = a.peer(B).unwrap().unwrap().last_error;
        assert!(note.contains("peer refused"), "{note}");
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
        let first = a.missing(&BTreeMap::new(), BATCH).unwrap().0.into_iter().find(|e| e["origin"] == a.origin() && e["seq"] == 1).unwrap();
        let mut forged = first.clone();
        forged["origin"] = json!("44444444-4444-4444-8444-444444444444");
        b.insert_raw(&[forged]).unwrap();
        let summary = exchange(&mut a, B, "b", &mut |request| serve(&mut b, request.clone())).unwrap();
        assert!(summary.rounds <= 2, "{summary:?}");
        assert!(summary.not_accepted > 0, "{summary:?}");
        let peer = a.peer(B).unwrap().unwrap();
        assert!(peer.last_error.contains("did not accept"), "{peer:?}");
        assert!(a.version_vector().unwrap().get(b.origin()).copied().unwrap_or(0) >= 1, "B's own origin reached A");
        assert_eq!(b.version_vector().unwrap().get(c.origin()).copied(), Some(1), "a pushed origin B accepted reached B");
    }
}
