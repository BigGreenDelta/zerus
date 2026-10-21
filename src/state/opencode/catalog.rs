//! OpenCode 2 model catalog for New session, model change and the account row.
//! Only allowlisted fields are copied; provider settings, headers, request
//! bodies, URLs and credentials never leave this module.
use super::super::*;
use super::providers;
use super::service::{connect, ensure_started, version_at_least, Service, Unavailable, MINIMUM};
use super::shortlist;
use std::collections::{HashMap, HashSet};
use std::io::Read;
use std::process::Command;
use std::time::{Duration, Instant};

pub(super) const MAX_FULL: usize = 10_000;
const MAX_VALUE: usize = 256;
const MAX_CACHE: u64 = 1_048_576;

pub(super) fn safe_id(value: &str) -> bool {
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

pub(super) fn label_of<'a>(value: &'a Value, fallback: &'a str) -> &'a str {
    value
        .as_str()
        .filter(|l| !l.is_empty() && l.len() <= 160 && !l.chars().any(char::is_control))
        .unwrap_or(fallback)
}

/// A model ID may contain `/`: OpenCode lists Groq's `openai/gpt-oss-20b`.
fn model_id(value: &str) -> bool {
    !value.is_empty()
        && value.len() <= 200
        && !value.chars().any(|c| c.is_whitespace() || c.is_control())
}

/// `provider/model`, split by every reader at the first `/`.
pub(super) fn model_value(provider: &str, id: &str) -> Option<String> {
    (safe_id(provider) && model_id(id))
        .then(|| format!("{provider}/{id}"))
        .filter(|value| value.len() <= MAX_VALUE)
}

/// Whether `model` is a `provider/model` value as [`model_value`] builds it.
pub(super) fn valid_model(model: &str) -> bool {
    model
        .split_once('/')
        .and_then(|(p, m)| model_value(p, m))
        .as_deref()
        == Some(model)
}

/// Effort IDs as a model offers them: safe, at most 40 bytes, at most 16.
fn efforts<'a>(ids: impl Iterator<Item = &'a str>) -> Vec<&'a str> {
    ids.filter(|v| safe_id(v) && v.len() <= 40)
        .take(16)
        .collect()
}

/// A model row: only these keys leave the catalog.
fn model_row(
    value: &str,
    label: &str,
    provider: &str,
    provider_label: &str,
    free: bool,
    efforts: &[&str],
) -> Value {
    json!({"id": value, "value": value, "label": label, "provider": provider,
        "provider_label": provider_label, "free": free,
        "effort_options": efforts, "effort_labels": efforts})
}

fn entry(model: &Value, labels: &HashMap<String, String>) -> Option<Value> {
    let (provider, id) = (model["providerID"].as_str()?, model["id"].as_str()?);
    if model["enabled"] == false {
        return None;
    }
    let value = model_value(provider, id)?;
    let efforts = efforts(
        model["variants"]
            .as_array()
            .into_iter()
            .flatten()
            .filter_map(|v| v["id"].as_str()),
    );
    Some(model_row(
        &value,
        label_of(&model["name"], id),
        provider,
        labels.get(provider).map(String::as_str).unwrap_or(provider),
        free(&model["cost"]),
        &efforts,
    ))
}

/// Every enabled model with a valid identity, allowlisted, in service order.
pub(super) fn project_all(models: &Value, labels: &HashMap<String, String>) -> (Vec<Value>, bool) {
    let mut rows = Vec::new();
    let mut seen = HashSet::new();
    for model in models["data"].as_array().into_iter().flatten() {
        let Some(row) = entry(model, labels) else {
            continue;
        };
        if !seen.insert(row["value"].as_str().unwrap_or_default().to_owned()) {
            continue;
        }
        if rows.len() == MAX_FULL {
            return (rows, true);
        }
        rows.push(row);
    }
    (rows, false)
}

pub(super) struct Snapshot {
    pub models: Vec<Value>,
    pub models_truncated: bool,
    pub default: String,
    pub configured: HashSet<String>,
    pub providers: Vec<Value>,
    pub providers_truncated: bool,
    pub loaded: bool,
}

/// The default model is always offered: past the bound its allowlisted row,
/// built from the default response, replaces the last row. Returns whether a
/// listed row was dropped for it.
fn include_selected(
    rows: &mut Vec<Value>,
    default: &Value,
    selected: &str,
    labels: &HashMap<String, String>,
) -> bool {
    if selected.is_empty() || rows.iter().any(|row| row["value"] == selected) {
        return false;
    }
    let Some(row) = entry(&default["data"], labels) else {
        return false;
    };
    let dropped = rows.len() >= MAX_FULL;
    if dropped {
        rows.pop();
    }
    rows.push(row);
    dropped
}

pub(super) fn snapshot_from(
    providers: &Value,
    models: &Value,
    default: &Value,
    integrations: Option<&Value>,
) -> Snapshot {
    let labels = providers::labels(providers);
    let (mut rows, mut models_truncated) = project_all(models, &labels);
    let selected = match (
        default["data"]["providerID"].as_str(),
        default["data"]["id"].as_str(),
    ) {
        (Some(p), Some(m)) => model_value(p, m).unwrap_or_default(),
        _ => String::new(),
    };
    models_truncated |= include_selected(&mut rows, default, &selected, &labels);
    let (provider_rows, providers_truncated) = providers::project(providers, integrations, &rows);
    Snapshot {
        models: rows,
        models_truncated,
        default: selected,
        configured: providers::configured(providers),
        providers: provider_rows,
        providers_truncated,
        loaded: providers["data"].as_array().is_some_and(|p| !p.is_empty()),
    }
}

/// Reads the catalog without credentials: `/api/credential` is never called.
/// Failures carry fixed texts only (`call_within`), never response fragments.
pub(super) fn read_snapshot(
    service: &Service,
    location: &Path,
    with_providers: bool,
) -> Result<Snapshot> {
    let limit = Duration::from_secs(10);
    // OpenCode lists a folder's own providers only after that folder's models
    // were read, so the models and the default come first.
    let models = service.call_within("GET", &query("/api/model", location), None, limit)?;
    let default =
        service.call_within("GET", &query("/api/model/default", location), None, limit)?;
    let providers = service.call_within("GET", &query("/api/provider", location), None, limit)?;
    // Integrations only name connection kinds; a failure or a slow answer leaves
    // kinds approximate. One second keeps `account ls` within the connector's 3 s.
    let integrations = with_providers
        .then(|| {
            service
                .call_within(
                    "GET",
                    &query("/api/integration", location),
                    None,
                    Duration::from_secs(1),
                )
                .ok()
        })
        .flatten();
    let mut snapshot = snapshot_from(&providers, &models, &default, integrations.as_ref());
    if !with_providers {
        snapshot.providers.clear();
        snapshot.providers_truncated = false;
    }
    Ok(snapshot)
}

/// Percent-encodes every byte outside the RFC 3986 unreserved set.
pub(super) fn encode(text: &str) -> String {
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
        .filter(|(p, m)| safe_id(p) && model_id(m))
        .ok_or("Choose provider/model for OpenCode")?;
    let row = rows.iter().find(|row| row["value"] == value).ok_or(
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
    let snapshot = read_snapshot(service, location, false)?;
    model_ref(&snapshot.models, value, effort)
}

fn cache_path() -> PathBuf {
    root().join("opencode/catalog.json")
}

fn cached() -> Option<Value> {
    cached_for(&home())
}

fn cached_for(home: &Path) -> Option<Value> {
    cached_at(&cache_path(), home)
}

fn cached_at(path: &Path, home: &Path) -> Option<Value> {
    let file = std::fs::File::open(path).ok()?;
    let mut bytes = Vec::new();
    file.take(MAX_CACHE + 1).read_to_end(&mut bytes).ok()?;
    if bytes.len() as u64 > MAX_CACHE {
        return None;
    }
    let mut value: Value = serde_json::from_slice(&bytes).ok()?;
    let listed = value["candidates"].is_array() || value["model_options"].is_array();
    if !listed || value["location"].as_str() != home.to_str() {
        return None;
    }
    value["catalog_at"] = catalog_time(&value["catalog_at"]);
    Some(value)
}

/// The latest cached catalog time shown: the year 2100.
const MAX_TIME: f64 = 4_102_444_800.0;

/// A cached catalog time: a finite number clamped to 0..=MAX_TIME, else null.
fn catalog_time(at: &Value) -> Value {
    match at.as_f64() {
        Some(time) if time.is_finite() => json!(time.clamp(0.0, MAX_TIME)),
        _ => Value::Null,
    }
}

const CACHE_CANDIDATES: usize = 512;

/// What the short list can draw on without the service: the default, every
/// favorite, configured and free model in short-list priority (at most 512),
/// each marked `configured`; `model_options` is the short list at write time,
/// so a rolled-back hgs still shows it. `models_truncated` keeps the live
/// list's cut.
pub(super) fn cache_record(
    location: &Path,
    snapshot: &Snapshot,
    favorites: &[String],
    at: &Value,
) -> Value {
    let (candidates, cut) = shortlist::ordered(
        &snapshot.models,
        &snapshot.default,
        &[],
        favorites,
        &snapshot.configured,
        CACHE_CANDIDATES,
    );
    let candidates: Vec<Value> = candidates
        .into_iter()
        .map(|mut m| {
            let configured = m["provider"]
                .as_str()
                .is_some_and(|p| snapshot.configured.contains(p));
            if let Some(map) = m.as_object_mut() {
                map.remove("favorite");
                map.remove("default");
                map.insert("configured".into(), json!(configured));
            }
            m
        })
        .collect();
    let mut record = json!({"version": 2, "location": location.to_string_lossy(),
        "model_options": row_from(snapshot, favorites, &[])["model_options"],
        "candidates": candidates, "models_truncated": snapshot.models_truncated || cut,
        "providers": snapshot.providers, "providers_truncated": snapshot.providers_truncated,
        "settings_model": snapshot.default, "catalog_at": at});
    if bound_record(&mut record) {
        record["models_truncated"] = json!(true);
    }
    record
}

/// Keeps a cache record within [`MAX_CACHE`] as written: `"` and `\` double
/// in JSON, so bounded entries can still add up. Candidates beyond the short
/// list go first, then both lists from the end alike; the rest of the record
/// is small. Returns whether an entry went.
fn bound_record(record: &mut Value) -> bool {
    let length = |value: &Value| serde_json::to_string(value).map_or(usize::MAX, |t| t.len());
    let mut size = length(record);
    let mut cut = false;
    while size > MAX_CACHE as usize {
        let count = |key: &str| record[key].as_array().map_or(0, Vec::len);
        let key = if count("candidates") >= count("model_options") {
            "candidates"
        } else {
            "model_options"
        };
        let Some(items) = record[key].as_array_mut() else {
            break;
        };
        let Some(last) = items.pop() else {
            break;
        };
        size = size.saturating_sub(length(&last) + usize::from(!items.is_empty()));
        cut = true;
    }
    cut
}

/// A cached short-list entry under the rules of a live one ([`entry`]): an
/// exact `provider/model` value, bounded labels and efforts and only the
/// row's keys; with whether its provider was configured.
fn cached_entry(entry: &Value) -> Option<(Value, bool)> {
    let (value, provider) = (entry["value"].as_str()?, entry["provider"].as_str()?);
    let id = value.strip_prefix(provider)?.strip_prefix('/')?;
    if model_value(provider, id).as_deref() != Some(value) {
        return None;
    }
    let efforts = efforts(
        entry["effort_options"]
            .as_array()
            .into_iter()
            .flatten()
            .filter_map(Value::as_str),
    );
    let row = model_row(
        value,
        label_of(&entry["label"], id),
        provider,
        label_of(&entry["provider_label"], provider),
        entry["free"] == true,
        &efforts,
    );
    Some((row, entry["configured"] == true))
}

/// A cached record as a snapshot. The file is untrusted: its entries,
/// providers and default model pass the rules live data passes, and anything
/// else is dropped. A version 1 record lists only `model_options` and does not
/// say why a model was listed, so every listed model stays offered; an
/// earlier hgs stopped at 256 entries, so a full list may have been cut.
pub(super) fn snapshot_from_cache(cache: &Value) -> Snapshot {
    let version_1 = !cache["candidates"].is_array();
    let entries = cache["candidates"]
        .as_array()
        .or_else(|| cache["model_options"].as_array())
        .map_or(&[][..], Vec::as_slice);
    let mut truncated =
        cache["models_truncated"] == true || (version_1 && entries.len() >= shortlist::MAX_SHORT);
    let mut models = Vec::new();
    let mut seen = HashSet::new();
    let mut configured = HashSet::new();
    for (row, is_configured) in entries.iter().filter_map(cached_entry) {
        if !seen.insert(row["value"].as_str().unwrap_or_default().to_owned()) {
            continue;
        }
        if models.len() == CACHE_CANDIDATES {
            truncated = true;
            break;
        }
        if is_configured || version_1 {
            configured.insert(row["provider"].as_str().unwrap_or_default().to_owned());
        }
        models.push(row);
    }
    let (providers, providers_cut) = providers::from_cache(&cache["providers"]);
    Snapshot {
        models,
        models_truncated: truncated,
        default: cache["settings_model"]
            .as_str()
            .filter(|v| valid_model(v))
            .unwrap_or_default()
            .to_owned(),
        configured,
        providers,
        providers_truncated: providers_cut || cache["providers_truncated"] == true,
        loaded: true,
    }
}

fn favorites() -> Vec<String> {
    crate::platform::home()
        .map(|home| super::favorites::load_favorites(&crate::config::config_dir(&home)))
        .unwrap_or_default()
}

fn empty_row() -> Value {
    json!({"model_options": [], "model_options_truncated": false, "providers": [],
        "providers_truncated": false, "settings_model": ""})
}

/// The account row's model and provider keys: the short list, then providers.
pub(super) fn row_from(snapshot: &Snapshot, favorites: &[String], pinned: &[&str]) -> Value {
    let (options, cut) = shortlist::build(
        &snapshot.models,
        &snapshot.default,
        pinned,
        favorites,
        &snapshot.configured,
    );
    json!({"model_options": options, "model_options_truncated": cut || snapshot.models_truncated,
        "providers": snapshot.providers, "providers_truncated": snapshot.providers_truncated,
        "settings_model": snapshot.default})
}

/// The catalog answer. Callers supply fixed status-only reasons; clipping is
/// only a presentation bound, not permission to forward provider diagnostics.
fn answer(row: &Value, source: &str, state: &str, reason: &str, at: Value) -> Value {
    let mut answer = row.clone();
    answer["model_catalog_source"] = json!(source);
    answer["catalog_state"] = json!(state);
    answer["catalog_reason"] = json!(super::server_reason(reason));
    answer["catalog_at"] = at;
    answer
}

fn unavailable(
    why: &Unavailable,
    home_location: bool,
    favorites: &[String],
    pinned: &[&str],
) -> Value {
    let last = if home_location { cached() } else { None };
    unavailable_from(
        why,
        &super::service::computer(),
        last.as_ref(),
        favorites,
        pinned,
    )
}

/// The stopped answer: the short list rebuilt from the last home catalog
/// with today's favorites, else an empty one.
fn unavailable_from(
    why: &Unavailable,
    computer: &str,
    last: Option<&Value>,
    favorites: &[String],
    pinned: &[&str],
) -> Value {
    let state = match why {
        Unavailable::NotRunning => "not_running",
        Unavailable::Disabled => "disabled",
        Unavailable::Incompatible(_) => "incompatible",
        Unavailable::Unreachable(_) => "unreachable",
    };
    let reason = why.reason(computer);
    match last {
        Some(last) => answer(
            &row_from(&snapshot_from_cache(last), favorites, pinned),
            "cache",
            state,
            &reason,
            catalog_time(&last["catalog_at"]),
        ),
        None => answer(&empty_row(), "unavailable", state, &reason, Value::Null),
    }
}

/// Never starts the service. Falls back to the last known home catalog.
pub(crate) fn catalog(location: Option<&Path>) -> Value {
    catalog_with(location, true, &[])
}

/// The short list of a session's folder for session details, with the
/// session's current and pending model right after the default: no
/// providers, so no `/api/integration` read, and no cache write.
pub(crate) fn session_options(location: &Path, pinned: &[&str]) -> Value {
    let answer = catalog_with(Some(location), false, pinned);
    json!({"model_options": answer["model_options"],
        "model_options_truncated": answer["model_options_truncated"]})
}

/// The short list while the service is stopped, for session details: the
/// cached home list through the same untrusted-cache path, never a service read.
pub(super) fn stopped_options(why: &Unavailable, location: &Path, pinned: &[&str]) -> Value {
    let last = if location == home() { cached() } else { None };
    stopped_options_from(why, last.as_ref(), &favorites(), pinned)
}

fn stopped_options_from(
    why: &Unavailable,
    last: Option<&Value>,
    favorites: &[String],
    pinned: &[&str],
) -> Value {
    let answer = unavailable_from(why, "", last, favorites, pinned);
    json!({"model_options": answer["model_options"],
        "model_options_truncated": answer["model_options_truncated"]})
}

fn catalog_with(location: Option<&Path>, providers: bool, pinned: &[&str]) -> Value {
    let home_dir = home();
    let location = location.unwrap_or(&home_dir);
    let is_home = location == home_dir;
    let favorites = favorites();
    let service = match connect() {
        Ok(service) => service,
        Err(why) => return unavailable(&why, is_home, &favorites, pinned),
    };
    match read_snapshot(&service, location, providers && is_home) {
        Ok(snapshot) => {
            let at = json!(now());
            let row = row_from(&snapshot, &favorites, pinned);
            if !snapshot.loaded {
                return answer(
                    &row,
                    "opencode_service",
                    "loading",
                    "OpenCode is still loading providers",
                    at,
                );
            }
            if providers && is_home {
                if let Ok(text) =
                    serde_json::to_string(&cache_record(location, &snapshot, &favorites, &at))
                {
                    if text.len() as u64 <= MAX_CACHE {
                        let _ = atomic(&cache_path(), &text);
                    }
                }
            }
            answer(&row, "opencode_service", "ready", "", at)
        }
        Err(reason) => unavailable(
            &Unavailable::Unreachable(reason),
            is_home,
            &favorites,
            pinned,
        ),
    }
}

pub(crate) const MAX_FULL_BYTES: usize = 4 * 1024 * 1024;
/// Room for the keys `answer` adds afterwards (state, source, a reason of at most
/// 301 characters, the time).
const ANSWER_RESERVE: usize = 2048;

/// The full catalog of one folder: every offered model with its favorite and
/// default marks, within `MAX_FULL_BYTES`. With the home providers, a provider
/// that the home folder does not list is marked as the folder's own.
pub(super) fn models_from(
    snapshot: &Snapshot,
    favorites: &[String],
    home_providers: Option<&HashSet<String>>,
    location: &Path,
) -> Value {
    let mut answer = row_from(snapshot, favorites, &[]);
    if let (Some(home), Some(providers)) = (home_providers, answer["providers"].as_array_mut()) {
        for provider in providers {
            if provider["id"].as_str().is_some_and(|id| !home.contains(id)) {
                provider["folder"] = json!(true);
            }
        }
    }
    answer["location"] = json!(location.to_string_lossy());
    answer["favorites"] = json!(favorites
        .iter()
        .map(|f| json!({"value": f,
            "available": snapshot.models.iter().any(|m| m["value"] == f.as_str())}))
        .collect::<Vec<_>>());
    answer["models_truncated"] = json!(snapshot.models_truncated);
    answer["models"] = json!([]);
    let mut size = serde_json::to_string(&answer).map_or(usize::MAX, |t| t.len());
    let mut models = Vec::new();
    for model in &snapshot.models {
        let mut row = model.clone();
        let value = row["value"].as_str().unwrap_or_default().to_owned();
        if favorites.contains(&value) {
            row["favorite"] = json!(true);
        }
        if value == snapshot.default {
            row["default"] = json!(true);
        }
        let add = serde_json::to_string(&row).map_or(usize::MAX, |t| t.len())
            + usize::from(!models.is_empty());
        if size.saturating_add(add) > MAX_FULL_BYTES - ANSWER_RESERVE {
            answer["models_truncated"] = json!(true);
            break;
        }
        size += add;
        models.push(row);
    }
    answer["models"] = json!(models);
    answer
}

/// `hgs account models`: the full catalog of one folder for the desktop.
/// Never starts the service.
pub(crate) fn models(location: Option<&Path>) -> Result<Value> {
    const NO_FOLDER: &str = "Choose an existing folder on this computer";
    let home_dir = std::fs::canonicalize(home()).unwrap_or_else(|_| home());
    let location = match location {
        None => home_dir.clone(),
        Some(path) if path.is_absolute() => std::fs::canonicalize(path)
            .ok()
            .filter(|p| p.is_dir())
            .ok_or(NO_FOLDER)?,
        Some(_) => return Err(NO_FOLDER.into()),
    };
    let is_home = location == home_dir;
    let favorites = favorites();
    let stopped = |why: &Unavailable| {
        let mut answer = unavailable(why, is_home, &favorites, &[]);
        answer["location"] = json!(location.to_string_lossy());
        answer["models"] = json!([]);
        answer["models_truncated"] = json!(false);
        answer["favorites"] = json!(favorites
            .iter()
            .map(|f| json!({"value": f}))
            .collect::<Vec<_>>());
        answer
    };
    let service = match connect() {
        Ok(service) => service,
        Err(why) => return Ok(stopped(&why)),
    };
    match read_models(&service, &location, &home_dir) {
        Ok((snapshot, home_providers)) => Ok(models_answer(
            &snapshot,
            &favorites,
            home_providers.as_ref(),
            &location,
            json!(now()),
        )),
        Err(reason) => Ok(stopped(&Unavailable::Unreachable(reason))),
    }
}

/// One folder's snapshot and, for a folder other than home, the providers the
/// home folder lists; `models_from` marks every other provider as the folder's own.
fn read_models(
    service: &Service,
    location: &Path,
    home_dir: &Path,
) -> Result<(Snapshot, Option<HashSet<String>>)> {
    let snapshot = read_snapshot(service, location, true)?;
    if location == home_dir {
        return Ok((snapshot, None));
    }
    let limit = Duration::from_secs(10);
    // As in `read_snapshot`, home lists its providers only after its models and
    // default were read. An empty home list would mark every provider the folder's own.
    service.call_within("GET", &query("/api/model", home_dir), None, limit)?;
    service.call_within("GET", &query("/api/model/default", home_dir), None, limit)?;
    let listed = service.call_within("GET", &query("/api/provider", home_dir), None, limit)?;
    Ok((
        snapshot,
        Some(providers::labels(&listed).into_keys().collect()),
    ))
}

/// The answer of `hgs account models` from a service read made at `at`.
fn models_answer(
    snapshot: &Snapshot,
    favorites: &[String],
    home_providers: Option<&HashSet<String>>,
    location: &Path,
    at: Value,
) -> Value {
    let (state, reason) = if snapshot.loaded {
        ("ready", "")
    } else {
        ("loading", "OpenCode is still loading providers")
    };
    let row = models_from(snapshot, favorites, home_providers, location);
    answer(&row, "opencode_service", state, reason, at)
}

/// The explicit Start OpenCode action behind `hgs account start native-opencode`.
pub(crate) fn start_from_accounts() -> Result<Value> {
    ensure_started()?;
    Ok(catalog(None))
}

/// `opencode --version`, the only subcommand this adapter may run, bounded
/// to five seconds. A hang, failure or odd output means "version unknown".
pub(crate) fn installed_version() -> Option<String> {
    let mut command = Command::new("opencode");
    command.arg("--version");
    let (status, stdout, _) = crate::platform::run_bounded(
        &mut command,
        [4096, 4096],
        Instant::now() + Duration::from_secs(5),
        &|| false,
    )
    .ok()?;
    if !status.success() {
        return None;
    }
    version_from_output(&String::from_utf8_lossy(&stdout))
}

/// The version in `opencode --version` output, which OpenCode 2 prints as
/// `opencode v2.0.32`: the first version-shaped token of the first line, or
/// of a later line only when the first has none (a warning printed first).
fn version_from_output(text: &str) -> Option<String> {
    let mut lines = text.lines();
    let first = lines.next()?;
    version_token(first).or_else(|| lines.find_map(version_token))
}

/// The first whitespace-separated token of `line` that is `MAJOR.MINOR.PATCH`
/// after an optional leading `v`, optionally with a `-prerelease` suffix
/// (which `version_at_least` rejects), without the `v`.
fn version_token(line: &str) -> Option<String> {
    let digits = |part: &str| !part.is_empty() && part.bytes().all(|b| b.is_ascii_digit());
    line.split_whitespace().find_map(|token| {
        let version = token.strip_prefix('v').unwrap_or(token);
        let (core, prerelease) = match version.split_once('-') {
            Some((core, prerelease)) => (core, Some(prerelease)),
            None => (version, None),
        };
        let parts: Vec<&str> = core.split('.').collect();
        let valid = version.len() <= 64
            && parts.len() == 3
            && parts.iter().all(|part| digits(part))
            && prerelease.is_none_or(|p| {
                !p.is_empty()
                    && p.bytes()
                        .all(|b| b.is_ascii_alphanumeric() || b == b'.' || b == b'-')
            });
        valid.then(|| version.to_owned())
    })
}

/// Install row state for the account: `(installed, reason)`.
pub(crate) fn install_state() -> (bool, String) {
    match installed_version() {
        Some(v) if version_at_least(&v, MINIMUM) => (true, String::new()),
        Some(v) => (
            false,
            format!("OpenCode {v} is not supported; install OpenCode 2.0.32 or newer 2.x"),
        ),
        None if crate::platform::available("opencode") => (
            false,
            "The OpenCode version could not be determined; install OpenCode 2.0.32 or newer 2.x"
                .into(),
        ),
        None => (false, "Install OpenCode 2.0.32 or newer 2.x".into()),
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::collections::HashMap;
    fn fixture() -> Value {
        serde_json::from_slice(include_bytes!(
            "../../../tests/fixtures/opencode2/models.json"
        ))
        .unwrap()
    }

    #[test]
    fn the_version_is_read_from_opencode_version_output() {
        for (output, expected) in [
            ("opencode v2.0.32\n", Some("2.0.32")),
            ("v2.0.32", Some("2.0.32")),
            ("2.0.32", Some("2.0.32")),
            ("opencode v2.0.33-beta.1", Some("2.0.33-beta.1")),
            ("garbage", None),
            ("", None),
            // A warning printed before the version line.
            (
                "Warning: could not write the log file\nopencode v2.0.32\n",
                Some("2.0.32"),
            ),
            // The first line's version wins over later version-shaped text.
            (
                "opencode v2.0.32\nlatest 2.0.40 available\n",
                Some("2.0.32"),
            ),
        ] {
            assert_eq!(
                version_from_output(output).as_deref(),
                expected,
                "{output:?}"
            );
        }
        // A prerelease is read, then refused as unsupported.
        assert!(!version_at_least(
            &version_from_output("opencode v2.0.33-beta.1").unwrap(),
            MINIMUM
        ));
        assert!(version_at_least(
            &version_from_output("opencode v2.0.32").unwrap(),
            MINIMUM
        ));
    }

    #[test]
    fn projection_keeps_only_allowlisted_fields() {
        let rows = project_all(&fixture(), &HashMap::new()).0;
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
        let rows = project_all(&fixture(), &HashMap::new()).0;
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
    fn location_is_a_percent_encoded_deep_object() {
        assert_eq!(
            query("/api/model", Path::new("/srv/my project/café&x")),
            "/api/model?location%5Bdirectory%5D=%2Fsrv%2Fmy%20project%2Fcaf%C3%A9%26x"
        );
    }

    #[test]
    fn model_ref_requires_an_exact_offered_model_and_effort() {
        let rows = project_all(&fixture(), &HashMap::new()).0;
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

    #[test]
    fn catalog_reasons_are_bounded_and_sanitized() {
        // Server text that reached a reason travels to phones in the catalog.
        let server = format!("OpenCode HTTP 500: \x1b[1m{}\u{0}\r", "z".repeat(600));
        let why = Unavailable::Unreachable(server);
        let row = answer(
            &empty_row(),
            "unavailable",
            "unreachable",
            &why.reason("fixture"),
            Value::Null,
        );
        let reason = row["catalog_reason"].as_str().unwrap();
        assert_eq!(reason.chars().count(), 301, "{reason}");
        assert!(reason.starts_with("OpenCode HTTP 500: zzz") && reason.ends_with('…'));
        assert!(!reason.chars().any(char::is_control));
        assert_eq!(row["catalog_state"], "unreachable");
        // DEL and C1 controls go too, and do not count toward the bound.
        let c1 = format!("OpenCode HTTP 500: \u{7f}\u{85}\u{9b}{}", "q".repeat(400));
        let row = answer(&empty_row(), "unavailable", "unreachable", &c1, Value::Null);
        let reason = row["catalog_reason"].as_str().unwrap();
        assert_eq!(reason, format!("OpenCode HTTP 500: {}…", "q".repeat(281)));
        let short = answer(
            &empty_row(),
            "unavailable",
            "unreachable",
            "a\u{7f}\tb\u{85}\nc\u{9b}",
            Value::Null,
        );
        assert_eq!(short["catalog_reason"], "a\tb\nc");
        // Zerus's own reasons stay byte for byte.
        for (state, reason) in [
            ("ready", String::new()),
            ("loading", "OpenCode is still loading providers".to_owned()),
            ("not_running", Unavailable::NotRunning.reason("fixture")),
            ("disabled", Unavailable::Disabled.reason("fixture")),
        ] {
            let row = answer(&empty_row(), "opencode_service", state, &reason, json!(1));
            assert_eq!(row["catalog_reason"], json!(reason));
            assert_eq!(row["catalog_state"], state);
        }
    }

    #[test]
    fn cache_is_ignored_for_another_location() {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("catalog.json");
        let record = json!({"location": "/srv/a", "model_options": [], "settings_model": "", "catalog_at": 1});
        std::fs::write(&path, record.to_string()).unwrap();
        assert!(cached_at(&path, Path::new("/srv/a")).is_some());
        assert!(cached_at(&path, Path::new("/srv/b")).is_none());
        let v2 = json!({"version": 2, "location": "/srv/a", "candidates": [], "providers": [], "settings_model": "", "catalog_at": 1});
        std::fs::write(&path, v2.to_string()).unwrap();
        assert!(cached_at(&path, Path::new("/srv/a")).is_some());
        assert!(cached_at(&path, Path::new("/srv/b")).is_none());
        std::fs::write(
            &path,
            json!({"location": "/srv/a", "settings_model": ""}).to_string(),
        )
        .unwrap();
        assert!(cached_at(&path, Path::new("/srv/a")).is_none());
    }

    #[test]
    fn a_stopped_service_rebuilds_the_short_list_from_cached_candidates() {
        let fixture = shared();
        let service = &fixture["service"];
        let snapshot = snapshot_from(
            &service["provider"],
            &service["model"],
            &service["default"],
            Some(&service["integration"]),
        );
        let favorites: Vec<String> = serde_json::from_value(fixture["favorites"].clone()).unwrap();
        let record = cache_record(
            Path::new("/home/example"),
            &snapshot,
            &favorites,
            &json!(1792267200.5),
        );
        let text = record.to_string();
        assert!(
            !text.contains("fixture-secret-key") && !text.contains("solo-model"),
            "{text}"
        );
        for canary in shortlist::SECRETS {
            assert!(!text.contains(canary), "{canary}");
        }
        let mut keys: Vec<&str> = record
            .as_object()
            .unwrap()
            .keys()
            .map(String::as_str)
            .collect();
        keys.sort_unstable();
        assert_eq!(
            keys,
            [
                "candidates",
                "catalog_at",
                "location",
                "model_options",
                "models_truncated",
                "providers",
                "providers_truncated",
                "settings_model",
                "version"
            ]
        );
        let allowed = [
            "id",
            "value",
            "label",
            "provider",
            "provider_label",
            "free",
            "effort_options",
            "effort_labels",
            "configured",
        ];
        for candidate in record["candidates"].as_array().unwrap() {
            assert!(
                candidate
                    .as_object()
                    .unwrap()
                    .keys()
                    .all(|k| allowed.contains(&k.as_str())),
                "{candidate}"
            );
        }
        assert_eq!(record["model_options"], fixture["row"]["model_options"]);
        let cached = snapshot_from_cache(&record);
        assert_eq!(
            row_from(&cached, &favorites, &[])["model_options"],
            fixture["row"]["model_options"]
        );
        assert_eq!(
            row_from(&cached, &favorites, &[])["providers"],
            fixture["row"]["providers"]
        );
        // Unstarring while stopped drops a model that was listed only as a favorite.
        let fewer = row_from(&cached, &favorites[1..], &[]);
        assert!(fewer["model_options"]
            .as_array()
            .unwrap()
            .iter()
            .all(|o| o["value"] != "keyed/paid-model"));
        // A version 1 cache (model_options only) still answers.
        let old = json!({"location": "/home/example", "model_options": fixture["row"]["model_options"], "settings_model": "fixture/local", "catalog_at": 1});
        assert_eq!(
            row_from(&snapshot_from_cache(&old), &[], &[])["model_options"][0]["value"],
            "fixture/local"
        );
    }

    #[test]
    fn the_cache_file_round_trips_through_cached_at() {
        let fixture = shared();
        let service = &fixture["service"];
        let snapshot = snapshot_from(
            &service["provider"],
            &service["model"],
            &service["default"],
            Some(&service["integration"]),
        );
        let favorites: Vec<String> = serde_json::from_value(fixture["favorites"].clone()).unwrap();
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("catalog.json");
        let home = Path::new("/home/example");
        atomic(
            &path,
            &cache_record(home, &snapshot, &favorites, &json!(1792267200.5)).to_string(),
        )
        .unwrap();
        let read = cached_at(&path, home).expect("a version 2 record is read back");
        assert_eq!(
            row_from(&snapshot_from_cache(&read), &favorites, &[])["model_options"],
            fixture["row"]["model_options"]
        );
        assert_eq!(read["catalog_at"], json!(1792267200.5));
        assert!(cached_at(&path, Path::new("/home/example/project")).is_none());
    }

    /// The short list's entries in order, each with its marks.
    fn marked(row: &Value) -> Vec<(String, bool, bool)> {
        row["model_options"]
            .as_array()
            .unwrap()
            .iter()
            .map(|o| {
                (
                    o["value"].as_str().unwrap().to_owned(),
                    o["favorite"] == true,
                    o["default"] == true,
                )
            })
            .collect()
    }

    #[test]
    fn a_version_1_cache_follows_the_current_favorites_and_says_when_it_was_cut() {
        // Written by an earlier hgs: the short list with that day's marks.
        let fixture = shared();
        let old = json!({"location": "/home/example", "model_options": fixture["row"]["model_options"],
            "settings_model": "fixture/local", "catalog_at": 1});
        let cached = snapshot_from_cache(&old);
        let row = row_from(&cached, &["groq/groq/compound".to_owned()], &[]);
        let marks = marked(&row);
        assert_eq!(marks[0], ("fixture/local".to_owned(), false, true));
        assert_eq!(marks[1], ("groq/groq/compound".to_owned(), true, false));
        // Every listed model stays offered; only today's favorite is marked.
        assert_eq!(marks.len(), 8);
        assert_eq!(marks.iter().filter(|m| m.1).count(), 1);
        assert_eq!(row["model_options_truncated"], false);
        // An earlier hgs stopped at 256 entries, so a full list may have been cut.
        let full: Vec<Value> = (0..shortlist::MAX_SHORT)
            .map(|i| {
                let value = format!("p/m{i:03}");
                json!({"id": value, "value": value, "label": "M", "provider": "p",
                    "provider_label": "P", "free": false, "effort_options": [], "effort_labels": []})
            })
            .collect();
        let old = json!({"location": "/home/example", "model_options": full, "settings_model": "p/m000", "catalog_at": 1});
        let row = row_from(&snapshot_from_cache(&old), &[], &[]);
        assert_eq!(
            row["model_options"].as_array().unwrap().len(),
            shortlist::MAX_SHORT
        );
        assert_eq!(row["model_options_truncated"], true);
        // A version 2 record keeps the live list's cut.
        let service = &fixture["service"];
        let mut snapshot = snapshot_from(
            &service["provider"],
            &service["model"],
            &service["default"],
            None,
        );
        snapshot.models_truncated = true;
        let record = cache_record(Path::new("/home/example"), &snapshot, &[], &json!(1));
        assert_eq!(
            row_from(&snapshot_from_cache(&record), &[], &[])["model_options_truncated"],
            true
        );
    }

    /// The keys `accounts.rs` copies from the catalog answer into the row.
    const ROW_KEYS: [&str; 8] = [
        "model_options",
        "model_options_truncated",
        "providers",
        "providers_truncated",
        "settings_model",
        "catalog_state",
        "catalog_reason",
        "catalog_at",
    ];

    #[test]
    fn a_damaged_cache_never_makes_an_oversize_or_unbounded_stopped_row() {
        let huge = "\"\\".repeat(1000);
        let default = json!({"id": "fixture/local", "value": "fixture/local", "label": huge,
            "provider": "fixture", "provider_label": huge, "free": huge, "configured": true,
            "effort_options": (0..400).map(|i| format!("e{i}")).collect::<Vec<_>>(),
            "effort_labels": vec![huge.clone(); 10], "default": true, "settings": {"apiKey": CANARY}});
        let mut entries = vec![default];
        entries.extend(
            (0..15).map(|_| json!({"id": huge, "value": huge, "label": huge, "provider": huge})),
        );
        entries.extend((0..300).map(|i| {
            json!({"value": format!("keyed/m{i:03}"), "provider": "keyed", "label": &huge[..300],
                "provider_label": &huge[..300], "free": true, "baseURL": CANARY, "favorite": "yes"})
        }));
        // A model whose provider disagrees with its value, a value with a space.
        entries.push(json!({"value": "keyed/x", "provider": "other"}));
        entries.push(json!({"value": "keyed/a b", "provider": "keyed"}));
        let mut providers: Vec<Value> = (0..15)
            .map(|i| {
                json!({"id": format!("p{i:03}"), "label": huge, "kind": "key", "credential_count": 1,
                    "model_count": 1, "active_label": huge, "headers": CANARY})
            })
            .collect();
        for odd in [
            json!({"id": "a/b", "label": "L", "kind": "key", "credential_count": 0, "model_count": 0}),
            json!({"id": "q", "label": "Q", "kind": CANARY, "credential_count": 0, "model_count": 0}),
            json!({"id": "r", "label": "R", "kind": "env", "credential_count": -1, "model_count": 0}),
            json!({"id": "s", "label": "S", "kind": "env", "credential_count": 0, "model_count": 1.5}),
            json!({"id": "t", "label": "T", "kind": "env", "credential_count": 0, "model_count": 1e9}),
            json!(CANARY),
        ] {
            providers.push(odd);
        }
        let home = Path::new("/home/example");
        let record = json!({"version": 2, "location": "/home/example", "model_options": &entries[..3],
            "candidates": &entries, "providers": providers, "providers_truncated": {"x": huge},
            "settings_model": "fixture/local", "catalog_at": {"x": huge}});
        let text = record.to_string();
        assert!(
            text.len() > 700_000 && text.len() as u64 <= MAX_CACHE,
            "{}",
            text.len()
        );
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("catalog.json");
        std::fs::write(&path, &text).unwrap();
        let last = cached_at(&path, home).expect("a home record is read");
        assert_eq!(last["catalog_at"], Value::Null);
        let answer = unavailable_from(&Unavailable::NotRunning, "fixture", Some(&last), &[], &[]);
        assert_eq!(answer["model_catalog_source"], "cache");
        // The account row as `accounts.rs` builds it, then fits it.
        let mut row = json!({"id": "native-opencode", "provider": "opencode",
            "label": "Native OpenCode configuration", "native": true, "installed": true});
        for key in ROW_KEYS {
            row[key] = answer[key].clone();
        }
        shortlist::fit_row(&mut row, shortlist::ROW_BUDGET);
        let size = shortlist::ascii_len(&row);
        assert!(size <= shortlist::ROW_BUDGET, "{size}");
        let text = row.to_string();
        assert!(
            !text.contains(CANARY),
            "a dropped cache field reached the row"
        );
        let keys = ["id", "provider", "label", "native", "installed"];
        for key in row.as_object().unwrap().keys() {
            assert!(
                ROW_KEYS.contains(&key.as_str()) || keys.contains(&key.as_str()),
                "{key}"
            );
        }
        assert_eq!(
            (&row["catalog_at"], &row["settings_model"]),
            (&Value::Null, &json!("fixture/local"))
        );
        assert_eq!(
            (&row["model_options_truncated"], &row["providers_truncated"]),
            (&json!(true), &json!(false))
        );
        let options = row["model_options"].as_array().unwrap();
        assert_eq!(
            options[0],
            json!({"id": "fixture/local", "value": "fixture/local", "label": "local", "provider": "fixture",
                "provider_label": "fixture", "free": false, "default": true,
                "effort_options": (0..16).map(|i| format!("e{i}")).collect::<Vec<_>>(),
                "effort_labels": (0..16).map(|i| format!("e{i}")).collect::<Vec<_>>()})
        );
        let allowed = [
            "id",
            "value",
            "label",
            "provider",
            "provider_label",
            "free",
            "effort_options",
            "effort_labels",
            "favorite",
            "default",
        ];
        for option in options {
            assert!(
                option
                    .as_object()
                    .unwrap()
                    .keys()
                    .all(|k| allowed.contains(&k.as_str())),
                "{option}"
            );
            assert!(option.get("favorite").is_none());
            assert_ne!(option["value"], "keyed/x");
        }
        let providers = row["providers"].as_array().unwrap();
        assert_eq!(providers.len(), 15);
        for (i, provider) in providers.iter().enumerate() {
            assert_eq!(
                provider,
                &json!({"id": format!("p{i:03}"), "label": format!("p{i:03}"), "kind": "key",
                    "credential_count": 1, "model_count": 1})
            );
        }
        // An odd-typed default model, time and list read as none.
        let odd = json!({"version": 2, "location": "/home/example", "candidates": [], "providers": CANARY,
            "settings_model": {"x": CANARY}, "catalog_at": "1792267200"});
        std::fs::write(&path, odd.to_string()).unwrap();
        let last = cached_at(&path, home).unwrap();
        let answer = unavailable_from(&Unavailable::NotRunning, "fixture", Some(&last), &[], &[]);
        assert_eq!(
            (&answer["settings_model"], &answer["catalog_at"]),
            (&json!(""), &Value::Null)
        );
        assert_eq!(
            (&answer["model_options"], &answer["providers"]),
            (&json!([]), &json!([]))
        );
        assert!(!answer.to_string().contains(CANARY));
    }

    /// `"` and `\` double in JSON; every identity, label and effort at its bound.
    fn worst(n: usize, tag: usize) -> String {
        let tag = format!("{tag:03}");
        format!("{tag}{}", "\"\\".repeat(n).get(..n - tag.len()).unwrap())
    }

    #[test]
    fn the_cache_writer_bounds_a_worst_case_record() {
        let provider = worst(100, 0);
        let providers = json!({"data": (0..providers::MAX_PROVIDERS).map(|i| {
            let id = if i == 0 { provider.clone() } else { worst(200, i) };
            json!({"id": id, "name": worst(160, i)})
        }).collect::<Vec<_>>()});
        let models = json!({"data": (0..600).map(|i| json!({"id": worst(155, i), "providerID": provider,
            "name": worst(160, i), "enabled": true, "cost": [{"input": 0, "output": 0}],
            "variants": (0..16).map(|j| json!({"id": worst(40, j)})).collect::<Vec<_>>()})).collect::<Vec<_>>()});
        let default = json!({"data": {"id": worst(155, 599), "providerID": provider}});
        let snapshot = snapshot_from(&providers, &models, &default, None);
        assert_eq!(snapshot.models.len(), 600);
        assert_eq!(snapshot.default.len(), 256);
        let favorites: Vec<String> = (0..64)
            .map(|i| format!("{provider}/{}", worst(155, 500 - i)))
            .collect();
        let (all, _) = shortlist::ordered(
            &snapshot.models,
            &snapshot.default,
            &[],
            &favorites,
            &snapshot.configured,
            512,
        );
        assert!(
            json!(all).to_string().len() as u64 > 2 * MAX_CACHE,
            "the case is not worst enough"
        );
        let home = Path::new("/home/example");
        let record = cache_record(home, &snapshot, &favorites, &json!(1792267200.5));
        let text = record.to_string();
        assert!(text.len() as u64 <= MAX_CACHE, "{}", text.len());
        assert_eq!(record["models_truncated"], true);
        let candidates = record["candidates"].as_array().unwrap();
        assert_eq!(candidates[0]["value"], snapshot.default.as_str());
        assert_eq!(
            record["model_options"][0]["value"],
            snapshot.default.as_str()
        );
        assert_eq!(
            record["providers"].as_array().unwrap().len(),
            providers::MAX_PROVIDERS
        );
        // The read back list keeps the default first and every favorite, and says it was cut.
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("catalog.json");
        atomic(&path, &text).unwrap();
        let read = cached_at(&path, home).expect("the bounded record is read back");
        let row = row_from(&snapshot_from_cache(&read), &favorites, &[]);
        let marks = marked(&row);
        assert_eq!(marks[0], (snapshot.default.clone(), false, true));
        assert!(marks[1..65].iter().all(|m| m.1), "{marks:?}");
        assert_eq!(row["model_options_truncated"], true);
    }

    fn shared() -> Value {
        serde_json::from_slice(include_bytes!(
            "../../../tests/fixtures/opencode-provider-catalog.json"
        ))
        .unwrap()
    }

    #[test]
    fn model_ids_may_contain_a_slash_but_providers_may_not() {
        assert_eq!(
            model_value("groq", "openai/gpt-oss-20b").as_deref(),
            Some("groq/openai/gpt-oss-20b")
        );
        assert_eq!(
            model_value("ollama", "qwen3-coder:30b").as_deref(),
            Some("ollama/qwen3-coder:30b")
        );
        let (max_id, long_id, long_provider) = ("m".repeat(200), "m".repeat(201), "p".repeat(60));
        assert_eq!(model_value("p", &max_id).map(|v| v.len()), Some(202));
        for (provider, id) in [
            ("a/b", "c"),
            ("p", ""),
            ("p", "a b"),
            ("p", "a\u{7}"),
            ("", "m"),
            ("p", long_id.as_str()),
            (long_provider.as_str(), max_id.as_str()),
        ] {
            assert!(model_value(provider, id).is_none(), "{provider}/{id}");
        }
        let rows = project_all(&shared()["service"]["model"], &HashMap::new()).0;
        assert_eq!(
            model_ref(&rows, "groq/openai/gpt-oss-20b", "high").unwrap(),
            json!({"providerID": "groq", "id": "openai/gpt-oss-20b", "variant": "high"})
        );
        assert!(model_ref(&rows, "groq/openai/gpt-oss-20b", "max").is_err());
        assert!(model_ref(&rows, "keyed/off", "").is_err());
    }

    #[test]
    fn the_full_catalog_is_allowlisted_and_skips_disabled_models() {
        let service = &shared()["service"];
        let labels = super::super::providers::labels(&service["provider"]);
        let (rows, truncated) = project_all(&service["model"], &labels);
        assert!(!truncated);
        assert_eq!(rows.len(), 9);
        let text = serde_json::to_string(&rows).unwrap();
        for secret in [
            "fixture-secret-key",
            "baseURL",
            "\"settings\"",
            "\"headers\"",
            "Authorization",
            "127.0.0.1",
            "example.invalid",
        ] {
            assert!(!text.contains(secret), "{secret} leaked: {text}");
        }
        let groq = rows
            .iter()
            .find(|r| r["value"] == "groq/openai/gpt-oss-20b")
            .unwrap();
        assert_eq!(groq["id"], groq["value"]);
        assert_eq!(groq["provider_label"], "Groq");
        assert_eq!(groq["effort_options"], json!(["low", "medium", "high"]));
        assert_eq!(
            rows.iter()
                .find(|r| r["value"] == "groq/groq/compound")
                .unwrap()["free"],
            true
        );
        assert_eq!(
            rows.iter()
                .find(|r| r["value"] == "ollama/qwen3-coder:30b")
                .unwrap()["free"],
            false
        );
        assert!(rows.iter().all(|r| r["value"] != "keyed/off"));
    }

    #[test]
    fn the_full_catalog_is_bounded_at_ten_thousand_models() {
        let mut many = json!({"data": []});
        for i in 0..10_050 {
            many["data"].as_array_mut().unwrap().push(
                json!({"id": format!("m{i}"), "providerID": "p", "name": "M", "enabled": true}),
            );
        }
        let (rows, truncated) = project_all(&many, &HashMap::new());
        assert_eq!((rows.len(), truncated), (MAX_FULL, true));
        // The default beyond the bound is still offered, replacing the last row.
        let snapshot = snapshot_from(
            &json!({"data": [{"id": "p", "name": "P"}]}),
            &many,
            &json!({"data": {"id": "m10040", "providerID": "p", "settings": {"apiKey": "fixture-secret-key"}}}),
            None,
        );
        assert_eq!(snapshot.models.len(), MAX_FULL);
        assert_eq!(snapshot.default, "p/m10040");
        assert!(snapshot.models.iter().any(|r| r["value"] == "p/m10040"));
        assert!(model_ref(&snapshot.models, "p/m10040", "").is_ok());
        assert!(!serde_json::to_string(&snapshot.models)
            .unwrap()
            .contains("fixture-secret-key"));
    }

    const CANARY: &str = "canary-secret-7f3a";
    type Script = Vec<(&'static str, Vec<u8>)>;
    type Lines = std::sync::Arc<std::sync::Mutex<Vec<String>>>;

    /// Stops the fake service's thread when the test is done with it.
    struct Stop(std::sync::Arc<std::sync::atomic::AtomicBool>);
    impl Drop for Stop {
        fn drop(&mut self) {
            self.0.store(true, std::sync::atomic::Ordering::SeqCst);
        }
    }

    /// A loopback stand-in for the service: it answers each request with the
    /// raw bytes scripted for its exact path and records every request line.
    fn serve(script: Script) -> (Service, Lines, Stop) {
        serve_by(move |line, _| {
            script
                .iter()
                .find(|(p, _)| *p == path_of(line))
                .map(|(_, raw)| raw.clone())
                .unwrap_or_else(|| http(404, "{}"))
        })
    }

    /// The same stand-in answering each request from its request line and the
    /// request lines recorded before it.
    fn serve_by(
        reply: impl Fn(&str, &[String]) -> Vec<u8> + Send + 'static,
    ) -> (Service, Lines, Stop) {
        let listener = std::net::TcpListener::bind(("127.0.0.1", 0)).unwrap();
        listener.set_nonblocking(true).unwrap();
        let port = listener.local_addr().unwrap().port();
        let seen = Lines::default();
        let log = seen.clone();
        let stop = std::sync::Arc::new(std::sync::atomic::AtomicBool::new(false));
        let flag = stop.clone();
        std::thread::spawn(move || {
            while !flag.load(std::sync::atomic::Ordering::SeqCst) {
                let Ok((mut peer, _)) = listener.accept() else {
                    std::thread::sleep(Duration::from_millis(5));
                    continue;
                };
                let _ = peer.set_nonblocking(false);
                let mut request = [0u8; 4096];
                let n = peer.read(&mut request).unwrap_or(0);
                let text = String::from_utf8_lossy(&request[..n]).into_owned();
                let line = text.lines().next().unwrap_or("").to_owned();
                let earlier = log.lock().unwrap().clone();
                log.lock().unwrap().push(line.clone());
                let answer = reply(line.as_str(), earlier.as_slice());
                let _ = std::io::Write::write_all(&mut peer, &answer);
            }
        });
        let reg = super::super::service::Registration {
            id: String::new(),
            version: String::new(),
            port,
            pid: 0,
            password: None,
        };
        (Service { reg }, seen, Stop(stop))
    }

    fn http(status: u16, body: &str) -> Vec<u8> {
        format!(
            "HTTP/1.1 {status} X\r\nContent-Length: {}\r\nConnection: close\r\n\r\n{body}",
            body.len()
        )
        .into_bytes()
    }

    /// The target (path and query) of a recorded request line.
    fn target_of(line: &str) -> &str {
        line.split(' ').nth(1).unwrap_or("")
    }

    /// The path of a recorded request line.
    fn path_of(line: &str) -> &str {
        target_of(line).split('?').next().unwrap_or("")
    }

    /// OpenCode 2.0.32 lists a folder's own providers only after that folder's
    /// models were read; a snapshot reads the models and the default first.
    #[test]
    fn a_folders_providers_are_read_after_its_models() {
        let providers = json!({"data": [{"id": "folderonly", "name": "Folder only"}]}).to_string();
        let models = json!({"data": [{"id": "proj", "providerID": "folderonly", "name": "Project",
            "enabled": true}]})
        .to_string();
        let default = json!({"data": {"id": "proj", "providerID": "folderonly"}}).to_string();
        let (service, seen, _stop) = serve_by(move |line, earlier| match path_of(line) {
            "/api/provider" if earlier.iter().any(|l| path_of(l) == "/api/model") => {
                http(200, &providers)
            }
            "/api/provider" => http(200, r#"{"data": []}"#),
            "/api/model" => http(200, &models),
            "/api/model/default" => http(200, &default),
            _ => http(404, "{}"),
        });
        let snapshot = read_snapshot(&service, Path::new("/project"), true).unwrap();
        assert!(
            snapshot.providers.iter().any(|p| p["id"] == "folderonly"),
            "{:?}",
            snapshot.providers
        );
        let lines = seen.lock().unwrap().clone();
        let at = |path: &str| lines.iter().position(|l| path_of(l) == path).unwrap();
        assert!(at("/api/model") < at("/api/provider"), "{lines:?}");
        assert!(at("/api/model/default") < at("/api/provider"), "{lines:?}");
    }

    /// Home, too, lists its providers only after its models were read. Those
    /// providers decide which of a folder's providers are its own, so a cold home
    /// must not mark a global provider as the folder's.
    #[test]
    fn the_home_providers_are_read_after_the_home_models() {
        let (home, folder) = (
            Path::new("/home/example"),
            Path::new("/home/example/project"),
        );
        let listed = |ids: &[&str]| {
            let rows: Vec<Value> = ids.iter().map(|id| json!({"id": id, "name": id})).collect();
            json!({"data": rows}).to_string()
        };
        let offered = |ids: &[&str]| {
            let rows: Vec<Value> = ids
                .iter()
                .map(|id| json!({"id": "m", "providerID": id, "name": "M", "enabled": true}))
                .collect();
            json!({"data": rows}).to_string()
        };
        let fake = || {
            let (home_list, home_models) = (listed(&["fixture"]), offered(&["fixture"]));
            let both = ["fixture", "folderonly"];
            let (folder_list, folder_models) = (listed(&both), offered(&both));
            let default = json!({"data": {"id": "m", "providerID": "fixture"}}).to_string();
            let home_model = query("/api/model", home);
            let home_provider = query("/api/provider", home);
            let folder_model = query("/api/model", folder);
            move |line: &str, earlier: &[String]| {
                let read = |target: &str| earlier.iter().any(|l| target_of(l) == target);
                let (target, empty) = (target_of(line), r#"{"data": []}"#);
                match path_of(line) {
                    "/api/provider" if target == home_provider => http(
                        200,
                        if read(&home_model) {
                            home_list.as_str()
                        } else {
                            empty
                        },
                    ),
                    "/api/provider" => http(
                        200,
                        if read(&folder_model) {
                            folder_list.as_str()
                        } else {
                            empty
                        },
                    ),
                    "/api/model" if target == home_model => http(200, &home_models),
                    "/api/model" => http(200, &folder_models),
                    "/api/model/default" => http(200, &default),
                    _ => http(404, "{}"),
                }
            }
        };
        let (service, seen, _stop) = serve_by(fake());
        let (snapshot, home_providers) = read_models(&service, folder, home).unwrap();
        let answer = models_from(&snapshot, &[], home_providers.as_ref(), folder);
        let own = |id: &str| {
            let providers = answer["providers"].as_array().unwrap();
            providers
                .iter()
                .find(|p| p["id"] == id)
                .map(|p| p["folder"] == true)
        };
        assert_eq!(
            (own("fixture"), own("folderonly")),
            (Some(false), Some(true)),
            "{answer}"
        );
        let lines = seen.lock().unwrap().clone();
        let at = |path: &str| {
            let target = query(path, home);
            lines.iter().position(|l| target_of(l) == target).unwrap()
        };
        assert!(at("/api/model") < at("/api/provider"), "{lines:?}");
        assert!(at("/api/model/default") < at("/api/provider"), "{lines:?}");
        assert!(!lines.join("\n").contains("credential"), "{lines:?}");
        // Home itself marks nothing and reads only its own snapshot.
        let (service, seen, _stop) = serve_by(fake());
        let (_, home_providers) = read_models(&service, home, home).unwrap();
        assert!(home_providers.is_none());
        let lines = seen.lock().unwrap().clone();
        let paths: Vec<&str> = lines.iter().map(|l| path_of(l)).collect();
        assert_eq!(
            paths,
            [
                "/api/model",
                "/api/model/default",
                "/api/provider",
                "/api/integration"
            ],
            "{lines:?}"
        );
    }

    /// Nothing a snapshot holds, serialised as the account row and the catalog
    /// cache would, may carry a canary.
    fn assert_clean(snapshot: &Snapshot) {
        let record = json!({"models": snapshot.models, "short": row_from(snapshot, &[], &[]),
            "default": snapshot.default, "providers": snapshot.providers,
            "configured": snapshot.configured.iter().collect::<Vec<_>>()});
        let text = record.to_string();
        assert!(!text.contains(CANARY), "{text}");
    }

    fn good() -> Script {
        let providers = json!({"data": [{"id": "p", "name": "P", "integrationID": "p",
            "settings": {"apiKey": CANARY}, "headers": {"Authorization": CANARY}, "body": CANARY,
            "options": {"x": CANARY}, "baseURL": CANARY}]});
        let models = json!({"data": [{"id": "m", "providerID": "p", "name": "M", "enabled": true,
            "settings": {"apiKey": CANARY}, "options": CANARY, "baseURL": CANARY}]});
        let default = json!({"data": {"id": "m", "providerID": "p", "settings": CANARY}});
        let integrations = json!({"data": [{"id": "p", "name": "P", "methods": [], "connections": [
            {"type": "credential", "id": CANARY, "label": "work", "method": "key", "message": CANARY, "url": CANARY},
            {"type": "env", "message": CANARY}]}]});
        vec![
            ("/api/provider", http(200, &providers.to_string())),
            ("/api/model", http(200, &models.to_string())),
            ("/api/model/default", http(200, &default.to_string())),
            ("/api/integration", http(200, &integrations.to_string())),
        ]
    }

    #[test]
    fn a_good_answer_is_allowlisted_and_never_asks_for_credentials() {
        let (service, seen, _stop) = serve(good());
        let snapshot = read_snapshot(&service, Path::new("/srv/project"), true).unwrap();
        assert_eq!((snapshot.models.len(), snapshot.providers.len()), (1, 1));
        assert_eq!(snapshot.providers[0]["kind"], "key");
        assert_clean(&snapshot);
        let lines = seen.lock().unwrap().join("\n");
        assert!(
            lines.contains("/api/integration") && !lines.contains("credential"),
            "{lines}"
        );
        // Without providers the integration endpoint is not read at all.
        let (service, seen, _stop) = serve(good());
        let snapshot = read_snapshot(&service, Path::new("/srv/project"), false).unwrap();
        assert!(snapshot.providers.is_empty());
        assert!(!seen.lock().unwrap().join("\n").contains("/api/integration"));
    }

    #[test]
    fn invalid_json_and_unexpected_shapes_read_as_an_empty_clean_catalog() {
        let bodies = [
            format!("not json {CANARY}"),
            format!("\"{CANARY}\""),
            format!("[\"{CANARY}\"]"),
            json!({"data": CANARY}).to_string(),
            json!({"data": [CANARY, {"id": 7, "providerID": CANARY}], "message": CANARY})
                .to_string(),
        ];
        for body in bodies {
            let script = [
                "/api/provider",
                "/api/model",
                "/api/model/default",
                "/api/integration",
            ]
            .map(|path| (path, http(200, &body)))
            .to_vec();
            let (service, _, _stop) = serve(script);
            let snapshot = read_snapshot(&service, Path::new("/srv/project"), true).unwrap();
            assert!(snapshot.models.is_empty() && snapshot.providers.is_empty());
            assert_clean(&snapshot);
        }
    }

    #[test]
    fn error_status_and_oversize_answers_fail_with_fixed_texts() {
        let refusal = json!({"message": CANARY, "data": {"message": CANARY}}).to_string();
        let oversize =
            format!("HTTP/1.1 200 OK\r\nContent-Length: 40000000\r\n\r\n{CANARY}").into_bytes();
        for (path, raw) in [
            ("/api/provider", http(500, &refusal)),
            ("/api/model", http(503, &format!("{CANARY} <html>"))),
            ("/api/model/default", http(404, &refusal)),
            ("/api/provider", oversize.clone()),
            ("/api/model", oversize),
        ] {
            let mut script = good();
            script.insert(0, (path, raw));
            let (service, _, _stop) = serve(script);
            let Err(error) = read_snapshot(&service, Path::new("/srv/project"), true) else {
                panic!("{path} should fail");
            };
            assert!(!error.contains(CANARY), "{error}");
            assert!(error.len() < 80, "{error}");
        }
        // An integration failure leaves approximate kinds and never fails the snapshot.
        for raw in [http(500, &refusal), http(200, &format!("junk {CANARY}"))] {
            let mut script = good();
            script.insert(0, ("/api/integration", raw));
            let (service, _, _stop) = serve(script);
            let snapshot = read_snapshot(&service, Path::new("/srv/project"), true).unwrap();
            assert_eq!(snapshot.providers[0]["credential_count"], 0);
            assert_clean(&snapshot);
        }
    }

    /// A 200 that is not an integration list says nothing about connections:
    /// a configured provider stays `config` and is never taken for a local one.
    #[test]
    fn a_junk_integration_answer_keeps_configured_providers_config() {
        let providers = json!({"data": [
            {"id": "p", "name": "P", "integrationID": "p"},
            {"id": "fixture", "name": "Fixture"}]});
        let models =
            json!({"data": [{"id": "m", "providerID": "fixture", "name": "M", "enabled": true}]});
        let script = |integration: Vec<u8>| {
            vec![
                ("/api/provider", http(200, &providers.to_string())),
                ("/api/model", http(200, &models.to_string())),
                ("/api/model/default", http(200, "{}")),
                ("/api/integration", integration),
            ]
        };
        let kind = |integration: Vec<u8>| {
            let (service, _, _stop) = serve(script(integration));
            let snapshot = read_snapshot(&service, Path::new("/srv/project"), true).unwrap();
            assert_clean(&snapshot);
            snapshot.providers[1]["kind"].clone()
        };
        for junk in [
            format!("junk {CANARY}"),
            format!("\"{CANARY}\""),
            json!({"message": CANARY}).to_string(),
            json!({"data": CANARY}).to_string(),
            json!([{"id": "p"}]).to_string(),
        ] {
            assert_eq!(kind(http(200, &junk)), "config", "{junk}");
        }
        // A readable list without the provider's ID: detected locally.
        assert_eq!(kind(http(200, &json!({"data": []}).to_string())), "local");
    }

    #[test]
    fn the_cached_catalog_record_is_allowlisted() {
        let (service, _, _stop) = serve(good());
        let snapshot = read_snapshot(&service, Path::new("/srv/project"), true).unwrap();
        let favorites = ["p/m".to_owned()];
        let record = cache_record(Path::new("/srv/project"), &snapshot, &favorites, &json!(1));
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("catalog.json");
        atomic(&path, &record.to_string()).unwrap();
        assert!(!std::fs::read_to_string(&path).unwrap().contains(CANARY));
        let read = cached_at(&path, Path::new("/srv/project")).unwrap();
        assert_clean(&snapshot_from_cache(&read));
        // The worst case of escaping: the_cache_writer_bounds_a_worst_case_record.
    }

    #[test]
    fn a_default_that_displaces_a_listed_row_marks_the_list_truncated() {
        let listed = |n: usize| {
            json!({"data": (0..n).map(|i| json!({"id": format!("m{i}"), "providerID": "p",
                "name": "M", "enabled": true})).collect::<Vec<_>>()})
        };
        let providers = json!({"data": [{"id": "p", "name": "P"}]});
        let other = json!({"data": {"id": "other", "providerID": "p"}});
        let full = snapshot_from(&providers, &listed(MAX_FULL), &other, None);
        assert_eq!(full.models.len(), MAX_FULL);
        assert!(full.models_truncated);
        assert_eq!(full.models.last().unwrap()["value"], "p/other");
        // Room to spare, or the default already listed: nothing is dropped.
        let room = snapshot_from(&providers, &listed(MAX_FULL - 1), &other, None);
        assert!(!room.models_truncated && room.models.len() == MAX_FULL);
        let listed_default = json!({"data": {"id": "m3", "providerID": "p"}});
        let same = snapshot_from(&providers, &listed(MAX_FULL), &listed_default, None);
        assert!(!same.models_truncated);
    }

    #[test]
    fn a_refused_api_description_never_reaches_the_catalog_reason() {
        let refusal = json!({"message": CANARY, "data": {"message": CANARY}}).to_string();
        for raw in [
            http(500, &refusal),
            http(404, &refusal),
            http(200, &format!("junk {CANARY}")),
        ] {
            let (service, _, _stop) = serve(vec![("/openapi.json", raw)]);
            let dir = tempfile::tempdir().unwrap();
            let reg = super::super::service::Registration {
                id: String::new(),
                version: String::new(),
                port: service.reg.port,
                pid: 0,
                password: None,
            };
            let result = super::super::service::compatibility_in(dir.path(), &reg, "d", || {
                super::super::service::fetch_description(&service)
            });
            let error = result.err().unwrap_or_default();
            let why = Unavailable::Unreachable(error.clone());
            let row = answer(
                &empty_row(),
                "unavailable",
                "unreachable",
                &why.reason("fixture"),
                Value::Null,
            );
            let reason = row["catalog_reason"].as_str().unwrap().to_owned();
            assert!(
                !error.contains(CANARY) && !reason.contains(CANARY),
                "{error} / {reason}"
            );
            assert!(error.len() < 80, "{error}");
            // A failed fetch caches nothing.
            assert_eq!(std::fs::read_dir(dir.path()).unwrap().count(), 0);
        }
    }

    #[test]
    fn stopped_session_details_carry_the_cached_home_short_list() {
        let fixture = shared();
        let service = &fixture["service"];
        let snapshot = snapshot_from(
            &service["provider"],
            &service["model"],
            &service["default"],
            Some(&service["integration"]),
        );
        let favorites: Vec<String> = serde_json::from_value(fixture["favorites"].clone()).unwrap();
        let record = cache_record(
            Path::new("/home/example"),
            &snapshot,
            &favorites,
            &json!(1792267200.5),
        );
        let options =
            stopped_options_from(&Unavailable::NotRunning, Some(&record), &favorites, &[]);
        assert_eq!(options["model_options"], fixture["row"]["model_options"]);
        assert_eq!(options["model_options_truncated"], false);
        assert!(!options.to_string().contains("fixture-secret-key"));
        // Another folder, or no cache: an empty list, but the key is still there,
        // so clients do not take the details for an older hgs.
        let none = stopped_options_from(&Unavailable::NotRunning, None, &favorites, &[]);
        assert_eq!(
            none,
            json!({"model_options": [], "model_options_truncated": false})
        );
        // A cut list stays cut.
        let mut cut = record.clone();
        cut["models_truncated"] = json!(true);
        let options = stopped_options_from(&Unavailable::NotRunning, Some(&cut), &favorites, &[]);
        assert_eq!(options["model_options_truncated"], true);
    }

    #[test]
    fn a_cached_catalog_time_is_clamped_to_a_bounded_non_negative_value() {
        assert_eq!(catalog_time(&json!(-5)), json!(0.0));
        assert_eq!(catalog_time(&json!(1e308)), json!(MAX_TIME));
        assert_eq!(catalog_time(&json!(1792267200.5)), json!(1792267200.5));
        for odd in [json!("1"), json!(null), json!({"x": 1})] {
            assert_eq!(catalog_time(&odd), Value::Null);
        }
    }

    #[test]
    fn the_full_catalog_marks_favorites_folders_and_unavailable_favorites() {
        let fixture = shared();
        let service = &fixture["service"];
        let snapshot = snapshot_from(
            &service["provider"],
            &service["model"],
            &service["default"],
            Some(&service["integration"]),
        );
        let favorites: Vec<String> = serde_json::from_value(fixture["favorites"].clone()).unwrap();
        let home: HashSet<String> = ["opencode", "groq", "fixture", "ollama", "keyed"]
            .map(String::from)
            .into();
        let answer = models_from(
            &snapshot,
            &favorites,
            Some(&home),
            Path::new("/home/example/project"),
        );
        assert_eq!(answer["location"], "/home/example/project");
        assert_eq!(answer["models"].as_array().unwrap().len(), 9);
        assert_eq!(answer["models_truncated"], false);
        assert_eq!(answer["model_options"], fixture["row"]["model_options"]);
        let providers = answer["providers"].as_array().unwrap();
        assert!(providers
            .iter()
            .all(|p| (p["id"] == "solo") == (p["folder"] == true)));
        assert_eq!(
            answer["favorites"],
            json!([
                {"value": "keyed/paid-model", "available": true},
                {"value": "groq/openai/gpt-oss-20b", "available": true},
                {"value": "gone/missing", "available": false}])
        );
        let marked: Vec<&str> = answer["models"]
            .as_array()
            .unwrap()
            .iter()
            .filter(|m| m["favorite"] == true)
            .map(|m| m["value"].as_str().unwrap())
            .collect();
        assert_eq!(marked, ["groq/openai/gpt-oss-20b", "keyed/paid-model"]);
        assert_eq!(
            answer["models"]
                .as_array()
                .unwrap()
                .iter()
                .filter(|m| m["default"] == true)
                .count(),
            1
        );
        assert!(!serde_json::to_string(&answer)
            .unwrap()
            .contains("fixture-secret-key"));
        // The home folder marks no provider.
        let home_answer = models_from(&snapshot, &favorites, None, Path::new("/home/example"));
        assert!(home_answer["providers"]
            .as_array()
            .unwrap()
            .iter()
            .all(|p| p.get("folder").is_none()));
    }

    /// The folder case of the shared contract, as `hgs account models` prints it.
    #[test]
    fn the_folder_catalog_answer_is_the_shared_contract() {
        let fixture = shared();
        let case = &fixture["account_models"];
        let service = &case["service"];
        let snapshot = snapshot_from(
            &service["provider"],
            &service["model"],
            &service["default"],
            Some(&service["integration"]),
        );
        let favorites: Vec<String> = serde_json::from_value(fixture["favorites"].clone()).unwrap();
        let home: HashSet<String> =
            super::super::providers::labels(&fixture["service"]["provider"])
                .into_keys()
                .collect();
        let answer = models_answer(
            &snapshot,
            &favorites,
            Some(&home),
            Path::new(case["location"].as_str().unwrap()),
            case["answer"]["catalog_at"].clone(),
        );
        assert_eq!(answer, case["answer"]);
        assert!(!answer.to_string().contains("fixture-secret-key"));
    }

    /// `hgs account models --location` without a folder passes an empty one.
    #[test]
    fn an_empty_or_relative_folder_is_refused_before_any_read() {
        for location in ["", "relative/folder"] {
            assert_eq!(
                models(Some(Path::new(location))).unwrap_err(),
                "Choose an existing folder on this computer"
            );
        }
    }

    #[test]
    fn the_full_catalog_output_stays_within_four_mebibytes() {
        let mut many = json!({"data": []});
        let variants: Vec<Value> = (0..16)
            .map(|v| json!({"id": format!("variant-{v:02}")}))
            .collect();
        for i in 0..10_000 {
            many["data"]
                .as_array_mut()
                .unwrap()
                .push(json!({"id": format!("m{i:05}"), "providerID": "p",
                "name": "\u{754c}".repeat(160), "enabled": true, "variants": variants}));
        }
        let snapshot = snapshot_from(
            &json!({"data": [{"id": "p", "name": "P", "integrationID": "p"}]}),
            &many,
            &json!({"data": null}),
            None,
        );
        let answer = models_from(&snapshot, &[], None, Path::new("/home/example"));
        assert_eq!(answer["models_truncated"], true);
        // The final keys and a longest reason still fit.
        let full = super::answer(
            &answer,
            "opencode_service",
            "unreachable",
            &"\u{754c}".repeat(400),
            json!(1792267200.5),
        );
        assert!(serde_json::to_string(&full).unwrap().len() <= MAX_FULL_BYTES);
        assert!(answer["models"].as_array().unwrap().len() > 1000);
    }
}
