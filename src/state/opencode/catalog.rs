//! OpenCode 2 model catalog for New session, model change and the account row.
//! Only allowlisted fields are copied; provider settings, headers, request
//! bodies, URLs and credentials never leave this module.
use super::super::*;
use super::service::{connect, ensure_started, version_at_least, Service, Unavailable, MINIMUM};
use std::io::Read;
use std::process::{Command, Stdio};

const MAX_MODELS: usize = 256;
const MAX_CACHE: u64 = 1_048_576;

fn safe_id(value: &str) -> bool {
    !value.is_empty()
        && value.len() <= 200
        && !value
            .chars()
            .any(|c| c.is_whitespace() || c.is_control() || c == '/')
}

fn free(cost: &Value) -> bool {
    let zero = |c: &Value| c["input"].as_f64() == Some(0.0) && c["output"].as_f64() == Some(0.0);
    match cost {
        Value::Array(items) => !items.is_empty() && items.iter().all(zero),
        Value::Object(_) => zero(cost),
        _ => false,
    }
}

fn label_of<'a>(value: &'a Value, fallback: &'a str) -> &'a str {
    value
        .as_str()
        .filter(|l| !l.is_empty() && l.len() <= 160 && !l.chars().any(char::is_control))
        .unwrap_or(fallback)
}

pub(super) fn project(models: &Value) -> Vec<Value> {
    let mut rows = Vec::new();
    let mut seen = std::collections::HashSet::new();
    for model in models["data"].as_array().into_iter().flatten() {
        if rows.len() == MAX_MODELS {
            break;
        }
        let (Some(provider), Some(id)) = (model["providerID"].as_str(), model["id"].as_str())
        else {
            continue;
        };
        if model["enabled"] == false || !safe_id(provider) || !safe_id(id) {
            continue;
        }
        let value = format!("{provider}/{id}");
        if !seen.insert(value.clone()) {
            continue;
        }
        let label = label_of(&model["name"], id);
        let efforts: Vec<&str> = model["variants"]
            .as_array()
            .into_iter()
            .flatten()
            .filter_map(|v| v["id"].as_str())
            .filter(|v| safe_id(v) && v.len() <= 40)
            .take(16)
            .collect();
        rows.push(
            json!({"id": value, "value": value, "label": label, "provider": provider,
            "provider_label": provider, "free": free(&model["cost"]),
            "effort_options": efforts, "effort_labels": efforts}),
        );
    }
    rows
}

/// Percent-encodes every byte outside the RFC 3986 unreserved set.
fn encode(text: &str) -> String {
    let mut out = String::new();
    for byte in text.bytes() {
        if byte.is_ascii_alphanumeric() || matches!(byte, b'-' | b'.' | b'_' | b'~') {
            out.push(byte as char);
        } else {
            out.push_str(&format!("%{byte:02X}"));
        }
    }
    out
}

/// `location` is a deepObject in the API description: `location[directory]=`.
fn query(path: &str, location: &Path) -> String {
    format!(
        "{path}?location%5Bdirectory%5D={}",
        encode(&location.to_string_lossy())
    )
}

pub(super) fn model_ref(rows: &[Value], value: &str, effort: &str) -> Result<Value> {
    let (provider, id) = value
        .split_once('/')
        .filter(|(p, m)| safe_id(p) && safe_id(m))
        .ok_or("Choose provider/model for OpenCode")?;
    let row = rows.iter().find(|row| row["id"] == value).ok_or(
        "This OpenCode model is not offered by the running service; refresh its model catalog",
    )?;
    if !effort.is_empty()
        && !row["effort_options"]
            .as_array()
            .into_iter()
            .flatten()
            .any(|e| e == effort)
    {
        return Err("This OpenCode model does not offer that effort".into());
    }
    let mut reference = json!({"providerID": provider, "id": id});
    if !effort.is_empty() {
        reference["variant"] = json!(effort);
    }
    Ok(reference)
}

pub(super) fn offered_model(
    service: &Service,
    location: &Path,
    value: &str,
    effort: &str,
) -> Result<Value> {
    let models = service.call("GET", &query("/api/model", location), None)?;
    model_ref(&project(&models), value, effort)
}

fn cache_path() -> PathBuf {
    root().join("opencode/catalog.json")
}

fn cached() -> Option<Value> {
    let file = std::fs::File::open(cache_path()).ok()?;
    let mut bytes = Vec::new();
    file.take(MAX_CACHE + 1).read_to_end(&mut bytes).ok()?;
    if bytes.len() as u64 > MAX_CACHE {
        return None;
    }
    let value: Value = serde_json::from_slice(&bytes).ok()?;
    value["model_options"].is_array().then_some(value)
}

fn answer(
    options: Value,
    selected: &str,
    source: &str,
    state: &str,
    reason: &str,
    at: Value,
) -> Value {
    json!({"model_options": options, "settings_model": selected, "model_catalog_source": source,
        "catalog_state": state, "catalog_reason": reason, "catalog_at": at})
}

fn unavailable(why: &Unavailable, home_location: bool) -> Value {
    let state = match why {
        Unavailable::NotRunning => "not_running",
        Unavailable::Disabled => "disabled",
        Unavailable::Incompatible(_) => "incompatible",
        Unavailable::Unreachable(_) => "unreachable",
    };
    let reason = why.reason(&crate::platform::hostname());
    match cached().filter(|_| home_location) {
        Some(last) => answer(
            last["model_options"].clone(),
            last["settings_model"].as_str().unwrap_or(""),
            "cache",
            state,
            &reason,
            last["catalog_at"].clone(),
        ),
        None => answer(json!([]), "", "unavailable", state, &reason, Value::Null),
    }
}

fn fetch(service: &Service, location: &Path) -> Result<(Vec<Value>, String, bool)> {
    let models = service.call("GET", &query("/api/model", location), None)?;
    let providers = service.call("GET", &query("/api/provider", location), None)?;
    let default = service.call("GET", &query("/api/model/default", location), None)?;
    let mut rows = project(&models);
    let mut labels = std::collections::HashMap::new();
    for provider in providers["data"].as_array().into_iter().flatten() {
        if let Some(id) = provider["id"].as_str().filter(|id| safe_id(id)) {
            labels.insert(id.to_owned(), label_of(&provider["name"], id).to_owned());
        }
    }
    for row in &mut rows {
        if let Some(label) = row["provider"].as_str().and_then(|p| labels.get(p)) {
            row["provider_label"] = json!(label);
        }
    }
    let selected = match (
        default["data"]["providerID"].as_str(),
        default["data"]["id"].as_str(),
    ) {
        (Some(p), Some(m)) if safe_id(p) && safe_id(m) => format!("{p}/{m}"),
        _ => String::new(),
    };
    let loaded = providers["data"].as_array().is_some_and(|p| !p.is_empty());
    Ok((rows, selected, loaded))
}

/// Never starts the service. Falls back to the last known home catalog.
pub(crate) fn catalog(location: Option<&Path>) -> Value {
    let home_dir = home();
    let location = location.unwrap_or(&home_dir);
    let is_home = location == home_dir;
    let service = match connect() {
        Ok(service) => service,
        Err(why) => return unavailable(&why, is_home),
    };
    match fetch(&service, location) {
        Ok((rows, selected, loaded)) => {
            let at = json!(now());
            let options = json!(rows);
            if !loaded {
                return answer(
                    options,
                    &selected,
                    "opencode_service",
                    "loading",
                    "OpenCode is still loading providers",
                    at,
                );
            }
            if is_home {
                let record = json!({"location": location.to_string_lossy(), "model_options": options,
                    "settings_model": selected, "catalog_at": at});
                if let Ok(text) = serde_json::to_string(&record) {
                    if text.len() as u64 <= MAX_CACHE {
                        let _ = atomic(&cache_path(), &text);
                    }
                }
            }
            answer(options, &selected, "opencode_service", "ready", "", at)
        }
        Err(reason) => unavailable(&Unavailable::Unreachable(reason), is_home),
    }
}

/// The explicit Start OpenCode action behind `hgs account start native-opencode`.
pub(crate) fn start_from_accounts() -> Result<Value> {
    ensure_started()?;
    Ok(catalog(None))
}

/// `opencode --version`, the only subcommand this adapter may run.
pub(crate) fn installed_version() -> Option<String> {
    let output = Command::new("opencode")
        .arg("--version")
        .stdin(Stdio::null())
        .stderr(Stdio::null())
        .output()
        .ok()?;
    if !output.status.success() {
        return None;
    }
    let text = String::from_utf8_lossy(&output.stdout);
    let version = text.lines().next()?.trim().trim_start_matches('v').trim();
    (!version.is_empty() && version.len() <= 64).then(|| version.to_owned())
}

/// Install row state for the account: `(installed, reason)`.
pub(crate) fn install_state() -> (bool, String) {
    match installed_version() {
        Some(v) if version_at_least(&v, MINIMUM) => (true, String::new()),
        Some(v) => (
            false,
            format!("OpenCode {v} is not supported; install OpenCode 2.0.32 or newer 2.x"),
        ),
        None => (false, "Install OpenCode 2.0.32 or newer 2.x".into()),
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    fn fixture() -> Value {
        serde_json::from_slice(include_bytes!(
            "../../../tests/fixtures/opencode2/models.json"
        ))
        .unwrap()
    }

    #[test]
    fn projection_keeps_only_allowlisted_fields() {
        let rows = project(&fixture());
        let text = serde_json::to_string(&rows).unwrap();
        for secret in [
            "fixture-secret-key",
            "baseURL",
            "settings",
            "headers",
            "127.0.0.1",
            "opencode.ai/inference",
        ] {
            assert!(!text.contains(secret), "{secret} leaked: {text}");
        }
        let local = rows.iter().find(|r| r["id"] == "fixture/local").unwrap();
        assert_eq!(local["value"], "fixture/local");
        assert_eq!(local["label"], "Native fixture");
        assert_eq!(local["effort_options"], json!(["low", "medium", "high"]));
    }

    #[test]
    fn free_means_zero_input_and_output_cost() {
        let rows = project(&fixture());
        assert!(rows
            .iter()
            .filter(|r| r["provider"] == "opencode")
            .all(|r| r["free"] == true));
        assert_eq!(
            rows.iter().find(|r| r["id"] == "fixture/local").unwrap()["free"],
            false
        );
    }

    #[test]
    fn projection_is_bounded_and_skips_invalid_ids() {
        let mut many = json!({"data": []});
        for i in 0..400 {
            many["data"].as_array_mut().unwrap().push(
                json!({"id": format!("m{i}"), "providerID": "p", "name": "M", "enabled": true}),
            );
        }
        many["data"].as_array_mut().unwrap().insert(
            0,
            json!({"id": "bad id", "providerID": "p", "enabled": true}),
        );
        many["data"]
            .as_array_mut()
            .unwrap()
            .insert(0, json!({"id": "off", "providerID": "p", "enabled": false}));
        let rows = project(&many);
        assert_eq!(rows.len(), 256);
        assert_eq!(rows[0]["id"], "p/m0");
    }

    #[test]
    fn location_is_a_percent_encoded_deep_object() {
        assert_eq!(
            query("/api/model", Path::new("/srv/my project/café&x")),
            "/api/model?location%5Bdirectory%5D=%2Fsrv%2Fmy%20project%2Fcaf%C3%A9%26x"
        );
    }

    #[test]
    fn model_ref_requires_an_exact_offered_model_and_effort() {
        let rows = project(&fixture());
        assert_eq!(
            model_ref(&rows, "fixture/local", "high").unwrap(),
            json!({"providerID": "fixture", "id": "local", "variant": "high"})
        );
        assert_eq!(
            model_ref(&rows, "fixture/local", "").unwrap(),
            json!({"providerID": "fixture", "id": "local"})
        );
        assert!(model_ref(&rows, "fixture/local", "extreme").is_err());
        assert!(model_ref(&rows, "fixture/missing", "").is_err());
        assert!(model_ref(&rows, "fixture", "").is_err());
        assert!(model_ref(&rows, "fixture/local/x", "").is_err());
        assert!(model_ref(&rows, "ollama/qwen3-coder:30b", "low").is_err());
    }
}
