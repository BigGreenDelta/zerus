//! OpenCode providers phase 1: the connector forwards only allowlisted
//! provider and model fields, pinned by tests/fixtures/opencode-provider-catalog.json.
use serde_json::{json, Value};
use zerus_mobile_connector::accounts;
use zerus_relay::json as pyjson;

const CONTRACT: &str = include_str!("../../../../tests/fixtures/opencode-provider-catalog.json");
const NOW: f64 = 1_792_267_200.0;

fn contract() -> Value {
    serde_json::from_str(CONTRACT).unwrap()
}

fn view(profiles: Value) -> Value {
    let mut cache = accounts::AccountCache::default();
    cache.apply_catalog(accounts::catalog(&json!({"profiles": profiles})), NOW);
    cache.view(NOW)
}

#[test]
fn the_ascii_measure_is_the_one_hgs_budgets_with() {
    for case in contract()["ascii_lengths"].as_array().unwrap() {
        assert_eq!(
            pyjson::python_default_len(&case["value"], true).unwrap() as u64,
            case["length"].as_u64().unwrap()
        );
    }
}

#[test]
fn the_heartbeat_forwards_allowlisted_providers_and_marks() {
    let c = contract();
    assert_eq!(
        view(json!([c["hgs_account"]]))["accounts"][0],
        c["heartbeat_account"]
    );
}

#[test]
fn a_stopped_row_keeps_its_cut_short_list_providers_and_reason() {
    let c = contract();
    assert_eq!(
        view(json!([c["stopped_hgs_account"]]))["accounts"][0],
        c["stopped_account"]
    );
}

#[test]
fn the_launch_catalog_forwards_marks_but_not_providers() {
    let c = contract();
    let launch = accounts::launch_catalog(&json!({"profiles": [c["hgs_account"]]})).unwrap();
    assert_eq!(launch["accounts"][0], c["launch_account"]);
    assert_eq!(launch["agents"], json!(["opencode"]));
}

#[test]
fn hostile_provider_entries_are_dropped_or_bounded() {
    let mut row = contract()["hgs_account"].clone();
    let mut providers = vec![
        json!({"id": "bad id", "label": "X", "kind": "key"}),
        json!("not an object"),
        json!({"id": "ok", "label": "\u{1b}[31m", "kind": "magic", "credential_count": -1, "model_count": 10_001,
               "active_label": "", "settings": {"apiKey": "fixture-secret-key"}, "url": "https://example.invalid"}),
    ];
    providers.extend((0..70).map(|i| {
        json!({"id": format!("p{i}"), "label": format!("P{i}"), "kind": "env",
        "credential_count": 0, "model_count": 1})
    }));
    row["providers"] = json!(providers);
    let shown = view(json!([row]));
    let listed = shown["accounts"][0]["providers"].as_array().unwrap();
    assert_eq!(listed.len(), 64);
    assert_eq!(listed[0], json!({"id": "ok", "label": "[31m"}));
    assert_eq!(
        listed[1],
        json!({"id": "p0", "label": "P0", "kind": "env", "credential_count": 0, "model_count": 1})
    );
    assert_eq!(shown["accounts"][0]["providers_truncated"], true);
    assert!(
        !shown.to_string().contains("fixture-secret-key")
            && !shown.to_string().contains("example.invalid")
    );
}

fn others() -> Vec<Value> {
    (0..99)
        .map(|i| json!({"id": format!("work-{i:02}"), "provider": "codex", "label": format!("Work {i:02} {}", "w".repeat(150)),
            "native": false, "installed": true, "is_default": false, "home": format!("/home/example/.config/hgs/accounts/work-{i:02}"),
            "auth_revision": format!("rev-{i}"), "credential_file": true,
            "account_status": {"status": "ok", "identity": {"email": format!("work-{i:02}@example.invalid"), "plan": "Example plan"},
                "windows": [{"id": "five_hour", "label": "Session", "used_percent": 42.5, "window_minutes": 300, "resets_at": 1792270800}],
                "checked_at": 1792267200}}))
        .collect()
}

#[test]
fn an_opencode_row_at_its_budget_fits_beside_ninety_nine_accounts() {
    let c = contract();
    let mut row = c["hgs_account"].clone();
    let template = row["model_options"][5].clone();
    for index in 0.. {
        let mut entry = template.clone();
        let value = format!("opencode/budget-{index:04}");
        entry["id"] = json!(value);
        entry["value"] = json!(value);
        entry["label"] = json!(format!("Budget {index:04} {}", "\u{754c}".repeat(8)));
        let mut next = row.clone();
        next["model_options"].as_array_mut().unwrap().push(entry);
        if pyjson::python_default_len(&next, true).unwrap() > 32 * 1024 {
            break;
        }
        row = next;
    }
    assert!(pyjson::python_default_len(&row, true).unwrap() > 31 * 1024);
    let mut profiles = vec![row.clone()];
    profiles.extend(others());
    let shown = view(json!(profiles));
    assert_eq!(shown["available"], true);
    assert!(pyjson::python_default_len(&shown, true).unwrap() <= accounts::MAX_CATALOG_BYTES);
    let opencode = &shown["accounts"][0];
    assert_eq!(
        opencode["model_options"].as_array().unwrap().len(),
        row["model_options"].as_array().unwrap().len()
    );
    assert_eq!(opencode["providers"], c["heartbeat_account"]["providers"]);
    assert!(
        opencode["model_options"][5]["effort_options"].is_array(),
        "nothing was degraded"
    );
}

#[test]
fn an_oversized_projection_drops_providers_after_efforts_and_keeps_every_account() {
    let c = contract();
    let mut row = c["hgs_account"].clone();
    row["providers"] = json!((0..64).map(|i| json!({"id": format!("p{i:02}"), "label": "\u{754c}".repeat(160),
        "kind": "key", "credential_count": 1, "model_count": 1, "active_label": "\u{754c}".repeat(80)})).collect::<Vec<_>>());
    let mut profiles = vec![row];
    profiles.extend(others());
    let shown = view(json!(profiles));
    assert_eq!(shown["available"], true);
    assert_eq!(shown["accounts"].as_array().unwrap().len(), 100);
    let opencode = &shown["accounts"][0];
    assert!(opencode.get("providers").is_none());
    assert_eq!(opencode["providers_truncated"], true);
    assert!(opencode["model_options"][0].get("effort_options").is_none());
    assert_eq!(opencode["model_options"][0]["value"], "fixture/local");
}

/// G2: secret-shaped fields riding on the account row, a provider or a model
/// never reach the heartbeat, the launch catalog or a refused projection.
#[test]
fn every_secret_canary_is_absent_from_forwarded_documents() {
    let c = contract();
    let canaries = [
        "fixture-secret-key",
        "canary-credential-id",
        "CANARY_ENV_NAME",
        "canary.example.invalid",
        "127.0.0.1",
    ];
    let hostile = || {
        json!({
            "settings": {"apiKey": "fixture-secret-key", "baseURL": "http://127.0.0.1:9/v1"},
            "headers": {"Authorization": "Bearer fixture-secret-key"},
            "body": {"token": "fixture-secret-key"},
            "options": {"apiKey": "fixture-secret-key"},
            "baseURL": "https://canary.example.invalid/v1",
            "url": "https://canary.example.invalid/device?code=fixture-secret-key",
            "status": {"message": "Sign in again with fixture-secret-key", "url": "https://canary.example.invalid"},
            "credential_id": "canary-credential-id",
            "credentials": [{"id": "canary-credential-id", "value": "fixture-secret-key"}],
            "env": ["CANARY_ENV_NAME"],
        })
    };
    let mut row = c["hgs_account"].clone();
    let extras = hostile();
    for (key, value) in extras.as_object().unwrap() {
        row[key] = value.clone();
        for list in ["providers", "model_options"] {
            for entry in row[list].as_array_mut().unwrap() {
                entry[key] = value.clone();
            }
        }
    }
    let shown = view(json!([row.clone()]));
    let launch = accounts::launch_catalog(&json!({"profiles": [row.clone()]})).unwrap();
    // The same hostile fields over the bound still cannot surface in the degraded form.
    let mut big = row;
    big["providers"] = json!((0..64)
        .map(|i| {
            let mut p = json!({"id": format!("p{i:02}"), "label": "\u{754c}".repeat(160), "kind": "key",
                "active_label": "\u{754c}".repeat(80)});
            p["settings"] = hostile()["settings"].clone();
            p
        })
        .collect::<Vec<_>>());
    let mut profiles = vec![big];
    profiles.extend(others());
    let degraded = view(json!(profiles));
    for document in [shown.to_string(), launch.to_string(), degraded.to_string()] {
        for canary in canaries {
            assert!(!document.contains(canary), "{canary} leaked");
        }
    }
    assert_eq!(shown["accounts"][0], c["heartbeat_account"]);
    assert_eq!(launch["accounts"][0], c["launch_account"]);
    assert!(degraded["accounts"][0].get("providers").is_none());
}

fn measure(value: &Value) -> usize {
    pyjson::python_default_len(value, true).unwrap()
}

/// The heartbeat with the OpenCode account degraded through `step`: 0 drops
/// `effort_labels`, 1 also `effort_options`, 2 also `providers`. Written out
/// here so the connector's own order is checked, not reused.
fn degraded(heartbeat: &Value, step: usize) -> Value {
    let mut heartbeat = heartbeat.clone();
    let opencode = &mut heartbeat["accounts"][0];
    for option in opencode["model_options"].as_array_mut().unwrap() {
        let option = option.as_object_mut().unwrap();
        option.remove("effort_labels");
        if step >= 1 {
            option.remove("effort_options");
        }
    }
    if step >= 2 {
        opencode.as_object_mut().unwrap().remove("providers");
        opencode["providers_truncated"] = json!(true);
    }
    heartbeat
}

/// A Codex account whose projection grows by exactly `extra` bytes (at most
/// 1689) as its label and identity fields grow.
fn filler(index: usize, extra: usize) -> Value {
    let mut left = extra;
    let mut grow = |maximum: usize| {
        let n = left.min(maximum);
        left -= n;
        "x".repeat(n + 1)
    };
    let label = grow(159);
    let mut identity = serde_json::Map::new();
    for field in [
        "name",
        "email",
        "organization",
        "plan",
        "auth_method",
        "account_id",
    ] {
        identity.insert(field.into(), json!(grow(255)));
    }
    assert_eq!(left, 0, "one filler grows by at most 1689 bytes");
    json!({"id": format!("pad-{index:02}"), "provider": "codex", "label": label, "native": false,
        "installed": true, "is_default": false, "auth_revision": format!("rev-{index:02}"),
        "account_status": {"status": "ok", "identity": identity, "checked_at": NOW}})
}

/// The heartbeat of `row` and `fillers` with nothing degraded, built from two
/// projections that each fit: every account is projected on its own.
fn undegraded(row: &Value, fillers: &[Value]) -> Value {
    let mut heartbeat = view(json!([row]));
    let rest = view(json!(fillers));
    assert_eq!(
        (heartbeat["available"].clone(), rest["available"].clone()),
        (json!(true), json!(true))
    );
    let accounts = rest["accounts"].as_array().unwrap().clone();
    heartbeat["accounts"]
        .as_array_mut()
        .unwrap()
        .extend(accounts);
    heartbeat
}

/// 99 Codex accounts beside `row` that make the undegraded heartbeat exactly
/// `target` bytes long.
fn padded(row: &Value, target: usize) -> Vec<Value> {
    let least: Vec<Value> = (0..99).map(|i| filler(i, 0)).collect();
    let mut missing = target
        .checked_sub(measure(&undegraded(row, &least)))
        .expect("the smallest fillers already exceed the target");
    let fillers: Vec<Value> = (0..99)
        .map(|i| {
            let extra = missing.min(1689);
            missing -= extra;
            filler(i, extra)
        })
        .collect();
    assert_eq!(missing, 0, "the target is beyond what 99 fillers reach");
    assert_eq!(measure(&undegraded(row, &fillers)), target);
    fillers
}

/// Task 6 order at the exact bound: a heartbeat of exactly 128 KiB is sent
/// whole; one byte more drops effort labels; each step is taken only when the
/// one before it does not suffice, and past the last one the projection is
/// refused.
#[test]
fn each_degrade_step_alone_suffices_at_its_exact_threshold() {
    let row = contract()["hgs_account"].clone();
    let alone = view(json!([row.clone()]));
    let full = measure(&alone);
    // What each step saves, counted on the OpenCode account alone.
    let saved: Vec<usize> = (0..3)
        .map(|step| full - measure(&degraded(&alone, step)))
        .collect();
    assert!(
        saved[0] > 0 && saved[0] < saved[1] && saved[1] < saved[2],
        "{saved:?}"
    );
    let max = accounts::MAX_CATALOG_BYTES;
    let cases = [
        (0, None),
        (1, Some(0)),
        (saved[0], Some(0)),
        (saved[0] + 1, Some(1)),
        (saved[1], Some(1)),
        (saved[1] + 1, Some(2)),
        (saved[2], Some(2)),
    ];
    for (over, step) in cases {
        let fillers = padded(&row, max + over);
        let whole = undegraded(&row, &fillers);
        let mut profiles = vec![row.clone()];
        profiles.extend(fillers);
        let shown = view(json!(profiles));
        let expected = step.map_or(whole.clone(), |step| degraded(&whole, step));
        assert_eq!(measure(&shown), measure(&expected), "{over} bytes over");
        assert_eq!(shown, expected, "{over} bytes over");
        assert!(measure(&shown) <= max);
    }
    // One byte more than every step can save: refused, never cut further.
    let fillers = padded(&row, max + saved[2] + 1);
    let mut profiles = vec![row];
    profiles.extend(fillers);
    let shown = view(json!(profiles));
    assert_eq!(shown["available"], false);
    assert_eq!(shown["accounts"], json!([]));
}
