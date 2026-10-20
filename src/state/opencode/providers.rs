//! OpenCode 2 providers for the account row: identity, a connection kind, the
//! number of credential connections and of listed models. Provider settings,
//! headers, request bodies, base URLs, connection status messages and URLs,
//! credential IDs and values never leave this module.
use super::super::*;
use super::catalog::{label_of, safe_id, MAX_FULL};
use std::collections::{HashMap, HashSet};

pub(super) const MAX_PROVIDERS: usize = 64;
pub(super) const KINDS: [&str; 8] = [
    "free", "key", "oauth", "env", "config", "command", "external", "local",
];
const MAX_INTEGRATIONS: usize = 4096;
const MAX_CONNECTIONS: usize = 64;

fn listed(providers: &Value) -> impl Iterator<Item = (&str, &Value)> {
    providers["data"]
        .as_array()
        .into_iter()
        .flatten()
        .take(MAX_INTEGRATIONS)
        .filter_map(|p| {
            let id = p["id"].as_str().filter(|id| safe_id(id))?;
            Some((id, p))
        })
}

/// Provider ID -> display name (`name`, at most 160 bytes and without control characters, else the ID).
pub(super) fn labels(providers: &Value) -> HashMap<String, String> {
    listed(providers)
        .map(|(id, p)| (id.to_owned(), label_of(&p["name"], id).to_owned()))
        .collect()
}

/// Providers declared in configuration or detected on this computer: OpenCode
/// lists them without an `integrationID`.
pub(super) fn configured(providers: &Value) -> HashSet<String> {
    listed(providers)
        .filter(|(_, p)| p["integrationID"].as_str().is_none())
        .map(|(id, _)| id.to_owned())
        .collect()
}

fn connection_label(value: &Value) -> Option<&str> {
    value.as_str().filter(|l| {
        !l.trim().is_empty() && l.chars().count() <= 80 && !l.chars().any(char::is_control)
    })
}

fn kind(
    provider: &Value,
    integration: Option<&Value>,
    known: bool,
    own: &[&Value],
) -> &'static str {
    let connections: Vec<&Value> = integration
        .and_then(|i| i["connections"].as_array())
        .into_iter()
        .flatten()
        .take(MAX_CONNECTIONS)
        .collect();
    // OpenCode lists the newest credential first.
    if let Some(first) = connections.iter().find(|c| c["type"] == "credential") {
        return match first["method"].as_str() {
            Some("oauth") => "oauth",
            Some("external") => "external",
            Some("command") => "command",
            _ => "key",
        };
    }
    if connections.iter().any(|c| c["type"] == "env") {
        return "env";
    }
    if provider["integrationID"].as_str().is_none() {
        return if known && integration.is_none() {
            "local"
        } else {
            "config"
        };
    }
    if !own.is_empty() && own.iter().all(|m| m["free"] == true) {
        "free"
    } else {
        "config"
    }
}

/// Allowlisted provider rows in service order, at most [`MAX_PROVIDERS`].
/// `integrations` is `/api/integration`; without it kinds are approximate.
pub(super) fn project(
    providers: &Value,
    integrations: Option<&Value>,
    models: &[Value],
) -> (Vec<Value>, bool) {
    // Only a list says which providers have no integration; any other answer
    // leaves kinds approximate, as without one.
    let known = integrations.and_then(|i| i["data"].as_array()).is_some();
    let by_id: HashMap<&str, &Value> = integrations
        .and_then(|i| i["data"].as_array())
        .into_iter()
        .flatten()
        .take(MAX_INTEGRATIONS)
        .filter_map(|i| Some((i["id"].as_str()?, i)))
        .collect();
    let mut rows = Vec::new();
    let mut seen = HashSet::new();
    for (id, provider) in listed(providers) {
        if !seen.insert(id) {
            continue;
        }
        if rows.len() == MAX_PROVIDERS {
            return (rows, true);
        }
        let integration = by_id
            .get(provider["integrationID"].as_str().unwrap_or(id))
            .copied();
        let own: Vec<&Value> = models.iter().filter(|m| m["provider"] == id).collect();
        let credentials: Vec<&Value> = integration
            .and_then(|i| i["connections"].as_array())
            .into_iter()
            .flatten()
            .take(MAX_CONNECTIONS)
            .filter(|c| c["type"] == "credential")
            .collect();
        let kind = kind(provider, integration, known, &own);
        debug_assert!(KINDS.contains(&kind));
        let mut row = json!({"id": id, "label": label_of(&provider["name"], id), "kind": kind,
            "credential_count": credentials.len(), "model_count": own.len()});
        if let [only] = credentials.as_slice() {
            if let Some(label) = connection_label(&only["label"]) {
                row["active_label"] = json!(label);
            }
        }
        rows.push(row);
    }
    (rows, false)
}

/// Provider rows read back from the catalog cache, which is untrusted: only
/// rows [`project`] could have written, with its keys and bounds, at most
/// [`MAX_PROVIDERS`]; `true` when more were listed.
pub(super) fn from_cache(rows: &Value) -> (Vec<Value>, bool) {
    let mut clean = Vec::new();
    let mut seen = HashSet::new();
    for row in rows.as_array().into_iter().flatten() {
        let Some(id) = row["id"].as_str().filter(|id| safe_id(id)) else {
            continue;
        };
        let Some(kind) = KINDS.iter().find(|kind| row["kind"] == **kind) else {
            continue;
        };
        let credentials = row["credential_count"]
            .as_u64()
            .filter(|n| *n <= MAX_CONNECTIONS as u64);
        let models = row["model_count"]
            .as_u64()
            .filter(|n| *n <= MAX_FULL as u64);
        let (Some(credentials), Some(models)) = (credentials, models) else {
            continue;
        };
        if !seen.insert(id) {
            continue;
        }
        if clean.len() == MAX_PROVIDERS {
            return (clean, true);
        }
        let mut provider = json!({"id": id, "label": label_of(&row["label"], id), "kind": kind,
            "credential_count": credentials, "model_count": models});
        if credentials == 1 {
            if let Some(label) = connection_label(&row["active_label"]) {
                provider["active_label"] = json!(label);
            }
        }
        clean.push(provider);
    }
    (clean, false)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn shared() -> Value {
        serde_json::from_slice(include_bytes!(
            "../../../tests/fixtures/opencode-provider-catalog.json"
        ))
        .unwrap()
    }

    #[test]
    fn kinds_counts_and_labels_follow_the_shared_contract() {
        let fixture = shared();
        let service = &fixture["service"];
        let models =
            super::super::catalog::project_all(&service["model"], &labels(&service["provider"])).0;
        let (rows, truncated) =
            project(&service["provider"], Some(&service["integration"]), &models);
        assert!(!truncated);
        assert_eq!(json!(rows), fixture["row"]["providers"]);
        let text = serde_json::to_string(&rows).unwrap();
        for secret in [
            "fixture-secret-key",
            "needs_auth",
            "cred_fixture",
            "GROQ_API_KEY",
            "example.invalid",
            "127.0.0.1",
        ] {
            assert!(!text.contains(secret), "{secret} leaked: {text}");
        }
    }

    #[test]
    fn without_integrations_kinds_are_approximate_but_never_fail() {
        let fixture = shared();
        let service = &fixture["service"];
        let models =
            super::super::catalog::project_all(&service["model"], &labels(&service["provider"])).0;
        let (rows, _) = project(&service["provider"], None, &models);
        let kinds: Vec<&str> = rows.iter().map(|r| r["kind"].as_str().unwrap()).collect();
        assert_eq!(
            kinds,
            ["free", "config", "config", "config", "config", "config"]
        );
        assert!(rows
            .iter()
            .all(|r| r["credential_count"] == 0 && r.get("active_label").is_none()));
    }

    #[test]
    fn providers_are_bounded_and_identities_checked() {
        let mut providers =
            json!({"data": [{"id": "bad id", "name": "X"}, {"id": "a/b", "name": "Y"}]});
        for i in 0..70 {
            providers["data"].as_array_mut().unwrap().push(
                json!({"id": format!("p{i}"), "name": format!("P{i}"), "integrationID": format!("p{i}")}),
            );
        }
        let (rows, truncated) = project(&providers, Some(&json!({"data": []})), &[]);
        assert_eq!((rows.len(), truncated), (MAX_PROVIDERS, true));
        assert_eq!(rows[0]["id"], "p0");
        // A control character in a name falls back to the ID; an overlong label is omitted.
        let one = json!({"data": [{"id": "q", "name": "bad\u{1b}[1m", "integrationID": "q"}]});
        let integrations = json!({"data": [{"id": "q", "name": "Q", "methods": [], "connections": [
            {"type": "credential", "id": "c", "label": "x".repeat(81), "method": "key"}]}]});
        let (rows, _) = project(&one, Some(&integrations), &[]);
        assert_eq!(rows[0]["label"], "q");
        assert_eq!(
            (
                rows[0]["kind"].as_str(),
                rows[0]["credential_count"].as_u64()
            ),
            (Some("key"), Some(1))
        );
        assert!(rows[0].get("active_label").is_none());
    }

    #[test]
    fn only_an_integration_list_makes_a_configured_provider_local() {
        let providers = json!({"data": [{"id": "fixture", "name": "Fixture"}]});
        for junk in [
            json!("not a list"),
            json!({"message": "x"}),
            json!({"data": "x"}),
            json!({"data": {"fixture": {}}}),
            json!([{"id": "other"}]),
        ] {
            let (rows, _) = project(&providers, Some(&junk), &[]);
            assert_eq!(rows[0]["kind"], "config", "{junk}");
        }
        let (rows, _) = project(&providers, Some(&json!({"data": [{"id": "other"}]})), &[]);
        assert_eq!(rows[0]["kind"], "local");
        let (rows, _) = project(&providers, Some(&json!({"data": [{"id": "fixture"}]})), &[]);
        assert_eq!(rows[0]["kind"], "config");
    }

    #[test]
    fn configured_providers_are_those_without_an_integration() {
        let fixture = shared();
        let mut configured: Vec<String> = configured(&fixture["service"]["provider"])
            .into_iter()
            .collect();
        configured.sort();
        assert_eq!(configured, ["fixture", "ollama"]);
    }
}
