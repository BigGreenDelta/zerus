//! Per-session reasoning settings. Read provider-owned telemetry and operate the
//! native TUI only after verifying the exact process and an empty idle composer.
//! Neither shared defaults nor the user's draft are changed by this adapter.
use super::*;
use serde::Deserialize;
use sha2::{Digest, Sha256};
use std::fs::{self, File};
use std::io::{BufRead, BufReader, Seek, SeekFrom};
use std::sync::{Mutex, OnceLock};
use std::time::{Duration, Instant, SystemTime};

const MAX_TAIL: u64 = 8 * 1024 * 1024;
const LEVELS: &[&str] = &[
    "off", "none", "minimal", "low", "medium", "high", "xhigh", "max", "ultra", "on",
];

pub(super) fn valid(value: &str) -> bool {
    LEVELS.contains(&value)
}

#[derive(Clone, Default, Debug)]
struct Settings {
    model: String,
    effort: String,
    path: String,
    offset: u64,
}

fn agent_home(record: &Value) -> PathBuf {
    if !string(record, "agent_home").is_empty() {
        return PathBuf::from(string(record, "agent_home"));
    }
    home().join(match string(record, "agent") {
        "codex" => ".codex",
        "kimi" => ".kimi-code",
        _ => ".claude",
    })
}

fn event_settings(agent: &str, event: &Value) -> Option<(String, String)> {
    let value = match (agent, string(event, "type")) {
        ("codex", "turn_context") => &event["payload"],
        ("kimi", "profile.bind" | "llm.request") => event,
        _ => return None,
    };
    let model = if agent == "kimi" {
        string(value, "modelAlias")
    } else {
        string(value, "model")
    };
    let effort = if agent == "kimi" {
        string(value, "thinkingEffort")
    } else {
        value["effort"]
            .as_str()
            .or(value["reasoning_effort"].as_str())
            .unwrap_or("")
    };
    if model.is_empty() || model.len() > 120 || model.chars().any(char::is_control) {
        return None;
    }
    Some((
        model.into(),
        if valid(effort) {
            effort.into()
        } else {
            String::new()
        },
    ))
}

fn history(record: &Value) -> Settings {
    let agent = string(record, "agent");
    let path = if agent == "kimi" {
        questions::wire_paths(record)
            .ok()
            .and_then(|paths| paths.into_iter().next())
    } else if agent == "codex" && !string(record, "conversation_id").is_empty() {
        let path = PathBuf::from(string(record, "transcript"));
        // A manifest points at the exact conversation, never a recent/global file.
        if path.file_name().is_some_and(|name| {
            name.to_string_lossy()
                .contains(string(record, "conversation_id"))
        }) {
            Some(path)
        } else {
            None
        }
    } else {
        None
    };
    let Some(path) = path else {
        return Settings::default();
    };
    let Ok(mut file) = File::open(&path) else {
        return Settings::default();
    };
    let Ok(meta) = file.metadata() else {
        return Settings::default();
    };
    if !meta.is_file() {
        return Settings::default();
    }
    // ls/inspect may include archived bindings to the same conversation. Reuse
    // the bounded metadata scan within this invocation; file changes invalidate it.
    type Cached = (u64, Option<SystemTime>, Settings);
    static CACHE: OnceLock<Mutex<BTreeMap<String, Cached>>> = OnceLock::new();
    let cache = CACHE.get_or_init(|| Mutex::new(BTreeMap::new()));
    let key = format!(
        "{agent}\0{}\0{}",
        string(record, "conversation_id"),
        path.display()
    );
    let modified = meta.modified().ok();
    if let Ok(cached) = cache.lock() {
        if let Some((size, stamp, value)) = cached.get(&key) {
            if *size == meta.len() && *stamp == modified {
                return value.clone();
            }
        }
    }
    // Stopped and archived sessions keep their transcripts: reuse the scan across
    // listings instead of reading up to 8 MiB of each on every poll.
    let scanned = scan_cache::cached("effort-settings", &path, &key, || {
        if agent == "codex" {
            let mut header = Vec::new();
            if BufReader::new((&mut file).take(256 * 1024))
                .read_until(b'\n', &mut header)
                .is_err()
                || !header.ends_with(b"\n")
            {
                return None;
            }
            let Ok(event) = serde_json::from_slice::<Value>(&header) else {
                return None;
            };
            if string(&event, "type") != "session_meta"
                || string(&event["payload"], "id") != string(record, "conversation_id")
            {
                return None;
            }
        }
        let mut window = 64 * 1024u64;
        let found = loop {
            let start = meta.len().saturating_sub(window);
            if file.seek(SeekFrom::Start(start)).is_err() {
                return None;
            }
            let mut bytes = Vec::new();
            if (&mut file).take(window).read_to_end(&mut bytes).is_err() {
                return None;
            }
            let mut offset = start + bytes.len() as u64;
            let mut found = None;
            // Most providers repeat settings per turn; start with 64 KiB and stop
            // at the newest metadata record instead of parsing every event in 8 MiB.
            for line in bytes.split_inclusive(|byte| *byte == b'\n').rev() {
                let end = offset;
                offset -= line.len() as u64;
                if (start > 0 && offset == start) || line.len() > 1024 * 1024 || !line.ends_with(b"\n")
                {
                    continue;
                }
                let marker: &[u8] = if agent == "kimi" {
                    b"thinkingEffort"
                } else {
                    b"turn_context"
                };
                if !line.windows(marker.len()).any(|part| part == marker) {
                    continue;
                }
                let Ok(event) = serde_json::from_slice::<Value>(line) else {
                    continue;
                };
                if let Some((model, effort)) = event_settings(agent, &event) {
                    found = Some(Settings {
                        model,
                        effort,
                        path: path.to_string_lossy().into_owned(),
                        offset: end,
                    });
                    break;
                }
            }
            if let Some(value) = found {
                break Some(value);
            }
            if start == 0 || window == MAX_TAIL {
                // Searched to the limit without settings: keep that answer too,
                // it is the most expensive one to repeat.
                break Some(Settings::default());
            }
            window = (window * 4).min(MAX_TAIL);
        };
        found.map(|value| json!({"model": value.model, "effort": value.effort, "path": value.path, "offset": value.offset}))
    });
    let output = scanned
        .map(|value| Settings {
            model: string(&value, "model").into(),
            effort: string(&value, "effort").into(),
            path: string(&value, "path").into(),
            offset: value["offset"].as_u64().unwrap_or(0),
        })
        .unwrap_or_default();
    if let Ok(mut cached) = cache.lock() {
        cached.insert(key, (meta.len(), modified, output.clone()));
    }
    output
}

fn capture(record: &Value) -> Result<String> {
    let pane = string(record, "pane");
    if !pane
        .strip_prefix('%')
        .is_some_and(|id| !id.is_empty() && id.bytes().all(|b| b.is_ascii_digit()))
    {
        return Err("invalid tracked pane".into());
    }
    let output = tmux(&["capture-pane", "-p", "-t", pane].map(str::to_owned), true)?;
    Ok(String::from_utf8_lossy(&output.stdout).into_owned())
}

fn footer(agent: &str, model: &str, screen: &str) -> Option<String> {
    for line in screen.lines().rev().take(3) {
        let line = line.trim().to_ascii_lowercase();
        if agent == "codex" {
            let prefix = format!("{} ", model.to_ascii_lowercase());
            let Some(tail) = line.strip_prefix(&prefix) else {
                continue;
            };
            let value = tail.split_whitespace().next()?;
            if valid(value) {
                return Some(value.into());
            }
        }
    }
    None
}

const KIMI_MODES: [&str; 4] = ["Ask When Needed", "Never Ask", "Plan", "Always Ask"];

/// The effort Kimi's status line shows for a model displayed as one of `names`.
/// The line joins slots (mode, goal, model, tasks, folder, git) with two spaces.
/// Older releases print it in the last rows, starting with the permission mode;
/// 2.1 prints it on the first row under the boxed composer. The model slot is
/// `<name>`, `<name> thinking` or `<name> thinking: <level>`. A bare name means
/// effort off on the built-in line, but a custom status line command need not
/// show effort, so `custom` never reads a bare name as off.
fn kimi_effort(screen: &str, names: &[&str], custom: bool) -> Option<String> {
    let lines: Vec<_> = screen.lines().collect();
    let below = lines
        .iter()
        .rposition(|line| {
            let line = line.trim();
            line.starts_with('╰') && line.ends_with('╯')
        })
        .and_then(|border| lines[border + 1..].iter().find(|line| !line.trim().is_empty()));
    let legacy = lines.iter().rev().take(3).filter(|line| {
        let first = line.trim().split("  ").next().unwrap_or_default();
        KIMI_MODES.contains(&first)
    });
    for line in below.into_iter().chain(legacy) {
        for slot in line.trim().split("  ").map(str::trim) {
            for name in names.iter().filter(|name| !name.is_empty()) {
                let Some(tail) = slot.strip_prefix(*name) else {
                    continue;
                };
                let value = if tail.is_empty() && !custom {
                    "off".to_owned()
                } else if tail == " thinking" {
                    "on".to_owned()
                } else if let Some(level) = tail.strip_prefix(" thinking: ") {
                    level.to_ascii_lowercase()
                } else {
                    continue;
                };
                if valid(&value) {
                    return Some(value);
                }
            }
        }
    }
    None
}

/// Whether tui.toml replaces Kimi's status line with a command's output.
/// An unreadable configuration counts as custom.
fn kimi_custom_status(record: &Value) -> bool {
    let Ok(file) = File::open(agent_home(record).join("tui.toml")) else {
        return false;
    };
    let mut text = String::new();
    if file.take(256 * 1024).read_to_string(&mut text).is_err() {
        return true;
    }
    text.parse::<toml_edit::DocumentMut>().map_or(true, |config| {
        config
            .get("status_line")
            .and_then(|line| line.get("command"))
            .is_some_and(|command| !command.as_str().is_some_and(|c| c.trim().is_empty()))
    })
}

/// A Kimi model entry's value, as overridden by its `overrides` table.
fn kimi_value<'a>(entry: &'a toml_edit::Item, key: &str) -> Option<&'a toml_edit::Item> {
    entry
        .get("overrides")
        .and_then(|overrides| overrides.get(key))
        .or_else(|| entry.get(key))
}

/// The displayed effort of the session's model (an ID or a display name).
/// Kimi's status line names its model, so it must match that catalog entry.
fn visible_effort(record: &Value, model: &str, screen: &str) -> Option<String> {
    if string(record, "agent") != "kimi" {
        return footer(string(record, "agent"), model, screen);
    }
    let option = catalog(record)
        .into_iter()
        .find(|option| option.id == model || option.label == model)?;
    target_footer(record, &option, screen)
}

#[cfg(test)]
thread_local! { static MODEL_CACHE_PARSES: std::cell::Cell<usize> = const { std::cell::Cell::new(0) }; }

/// Codex's models_cache.json (hundreds of KiB) for this record's account. Every
/// listed Codex session reads its options and catalog from it, so one listing
/// parsed it twice per session. Reuse the parse while the file is unchanged.
fn codex_models(record: &Value) -> Option<std::sync::Arc<Value>> {
    type Cached = (u64, Option<SystemTime>, std::sync::Arc<Value>);
    static CACHE: OnceLock<Mutex<BTreeMap<PathBuf, Cached>>> = OnceLock::new();
    let path = agent_home(record).join("models_cache.json");
    let file = File::open(&path).ok()?;
    let meta = file.metadata().ok()?;
    let modified = meta.modified().ok();
    let cache = CACHE.get_or_init(|| Mutex::new(BTreeMap::new()));
    if let Some((size, stamp, value)) = cache.lock().ok()?.get(&path) {
        if *size == meta.len() && *stamp == modified {
            return Some(value.clone());
        }
    }
    #[cfg(test)]
    MODEL_CACHE_PARSES.with(|count| count.set(count.get() + 1));
    // serde_json's reader requests individual bytes. Buffer the bounded
    // file so every session does not issue hundreds of thousands of reads.
    let value = std::sync::Arc::new(
        serde_json::from_reader::<_, Value>(BufReader::new(file.take(MAX_TAIL))).ok()?,
    );
    if let Ok(mut cached) = cache.lock() {
        cached.insert(path, (meta.len(), modified, value.clone()));
    }
    Some(value)
}

fn options(record: &Value, model: &str) -> Vec<String> {
    match string(record, "agent") {
        "codex" => {
            let Some(cache) = codex_models(record) else {
                return vec![];
            };
            cache["models"]
                .as_array()
                .and_then(|models| {
                    models
                        .iter()
                        .find(|entry| string(entry, "slug").eq_ignore_ascii_case(model))
                })
                .and_then(|entry| entry["supported_reasoning_levels"].as_array())
                .map(|values| {
                    values
                        .iter()
                        .filter_map(|value| value["effort"].as_str())
                        .filter(|value| valid(value))
                        .map(str::to_owned)
                        .collect()
                })
                .unwrap_or_default()
        }
        "kimi" => {
            let Ok(file) = File::open(agent_home(record).join("config.toml")) else {
                return vec![];
            };
            let mut text = String::new();
            if file.take(1024 * 1024).read_to_string(&mut text).is_err() {
                return vec![];
            }
            let Ok(config) = text.parse::<toml_edit::DocumentMut>() else {
                return vec![];
            };
            let Some(entry) = config.get("models").and_then(|models| models.get(model)) else {
                return vec![];
            };
            let mut levels: Vec<String> = kimi_value(entry, "support_efforts")
                .and_then(|value| value.as_array())
                .map(|values| {
                    values
                        .iter()
                        .filter_map(|v| v.as_str())
                        .filter(|v| valid(v))
                        .map(str::to_owned)
                        .collect()
                })
                .unwrap_or_default();
            let always = kimi_value(entry, "capabilities")
                .and_then(|v| v.as_array())
                .is_some_and(|v| v.iter().any(|v| v.as_str() == Some("always_thinking")));
            if levels.is_empty() {
                let thinking = always
                    || kimi_value(entry, "capabilities")
                        .and_then(|v| v.as_array())
                        .is_some_and(|v| v.iter().any(|v| v.as_str() == Some("thinking")))
                    || kimi_value(entry, "adaptive_thinking").and_then(|v| v.as_bool()) == Some(true);
                if thinking {
                    levels.push("on".into());
                }
                if !always {
                    levels.push("off".into());
                }
            } else if !always {
                levels.insert(0, "off".into());
            }
            levels
        }
        _ => vec![],
    }
}

#[derive(Clone, Debug)]
struct ModelOption {
    id: String,
    label: String,
    efforts: Vec<String>,
    default_effort: String,
}

fn safe_model(value: &str) -> bool {
    !value.is_empty()
        && value.len() <= 120
        && !value.chars().any(char::is_control)
        && !value.starts_with('-')
}

fn catalog(record: &Value) -> Vec<ModelOption> {
    let mut result = Vec::new();
    if string(record, "agent") == "codex" {
        let parsed = codex_models(record);
        if let Some(models) = parsed.as_ref().and_then(|v| v["models"].as_array()) {
            for entry in models.iter().take(256) {
                let id = string(entry, "slug");
                if !safe_model(id)
                    || (!string(entry, "visibility").is_empty()
                        && string(entry, "visibility") != "list")
                {
                    continue;
                }
                let label = if safe_model(string(entry, "display_name")) {
                    string(entry, "display_name")
                } else {
                    id
                };
                let mut efforts: Vec<String> = entry["supported_reasoning_levels"]
                    .as_array()
                    .map(|v| {
                        v.iter()
                            .filter_map(|v| v["effort"].as_str())
                            .filter(|v| valid(v))
                            .map(str::to_owned)
                            .collect()
                    })
                    .unwrap_or_default();
                efforts.sort_by_key(|v| match v.as_str() {
                    "max" => 1,
                    "ultra" => 2,
                    _ => 0,
                });
                if id.starts_with("codex-auto-") {
                    efforts.retain(|e| e != "ultra");
                }
                result.push(ModelOption {
                    id: id.into(),
                    label: label.into(),
                    efforts,
                    default_effort: string(entry, "default_reasoning_level").into(),
                });
            }
        }
    } else if string(record, "agent") == "kimi" {
        let mut text = String::new();
        if File::open(agent_home(record).join("config.toml"))
            .and_then(|f| f.take(1024 * 1024).read_to_string(&mut text))
            .is_ok()
        {
            if let Ok(config) = text.parse::<toml_edit::DocumentMut>() {
                if let Some(models) = config.get("models").and_then(|v| v.as_table_like()) {
                    for (id, entry) in models.iter().take(256) {
                        if !safe_model(id) {
                            continue;
                        }
                        // Kimi shows the overridden alias on its status line.
                        let label = kimi_value(entry, "display_name")
                            .or_else(|| kimi_value(entry, "model"))
                            .and_then(|v| v.as_str())
                            .filter(|v| safe_model(v))
                            .unwrap_or(id);
                        let efforts = options(record, id);
                        let default_effort = kimi_value(entry, "default_effort")
                            .and_then(|v| v.as_str())
                            .filter(|v| efforts.iter().any(|e| e == v))
                            .map(str::to_owned)
                            .unwrap_or_else(|| {
                                efforts.get(efforts.len() / 2).cloned().unwrap_or_default()
                            });
                        result.push(ModelOption {
                            id: id.into(),
                            label: label.into(),
                            efforts,
                            default_effort,
                        });
                    }
                }
            }
        }
    }
    result
}

fn settings_reason(record: &Value, models: &[ModelOption]) -> &'static str {
    if record.get("archive_id").is_some() || string(record, "state") == "archived" {
        return "Restore the archived session before changing its settings";
    }
    if string(record, "agent") == "claude" {
        return "Change Claude’s model with /model and effort with /effort in Terminal.";
    }
    if !["codex", "kimi"].contains(&string(record, "agent")) {
        return "Model settings are available in Terminal for this provider.";
    }
    if models.is_empty() {
        return "No model catalog is available for this account; open the agent once to populate it";
    }
    if string(record, "run_id").is_empty()
        || (string(record, "conversation_id").is_empty() && !input::first_message_candidate(record))
        || !string(record, "expected_id").is_empty()
    {
        return "Waiting for the agent to confirm this conversation";
    }
    if string(record, "conversation_id").is_empty()
        && input::checked_settings(string(record,"name"),string(record,"run_id"),"")
            .and_then(|r| input::checked_terminal(&r,true)).is_err()
    {
        return "Wait for the agent's empty prompt before changing model settings";
    }
    if !string(record, "error").is_empty() || pause_active(record) {
        return "Wait for the session transition to finish";
    }
    ""
}

fn ready(record: &Value) -> bool {
    ((string(record, "activity") == "idle" && string(record, "phase") == "idle")
        || input::first_message_candidate(record))
        && telemetry::grouped_active(record) == 0
        && !record["active_tools"]
            .as_object()
            .is_some_and(|v| !v.is_empty())
        && !record["subagents"]
            .as_object()
            .is_some_and(|v| v.values().any(|c| string(c, "state") == "working"))
}

fn settings(record: &Value, live_pane: bool) -> Settings {
    let mut value = history(record);
    let confirmed = string(record, "effort_source_path") == value.path
        && record["effort_source_offset"]
            .as_u64()
            .is_some_and(|offset| offset >= value.offset);
    if value.model.is_empty() || confirmed {
        value.model = string(record, "model").into();
    }
    if value.effort.is_empty() || confirmed {
        value.effort = string(record, "effort").into();
    }
    if live_pane && process_alive(record) {
        if let Ok(screen) = capture(record) {
            {
                // The native status line also reflects changes made in Terminal
                // before the next turn writes fresh history. The account catalog
                // resolves its exact displayed name, never supplies a default.
                let observed: Vec<_> = catalog(record)
                    .into_iter()
                    .filter_map(|model| {
                        target_footer(record, &model, &screen).map(|effort| (model.id, effort))
                    })
                    .collect();
                if let [(model, effort)] = observed.as_slice() {
                    value.model = model.clone();
                    value.effort = effort.clone();
                }
            }
            // Kimi's status line names its model; the catalog match above covers it.
            if string(record, "agent") != "kimi" {
                if let Some(effort) = footer(string(record, "agent"), &value.model, &screen) {
                    value.effort = effort;
                }
            }
        }
    }
    value
}

pub(super) fn summary(record: &Value, live_pane: bool) -> Value {
    let value = settings(record, live_pane);
    let mut choices = options(record, &value.model);
    // Codex deliberately requires its explicit advanced picker for Ultra.
    if string(record, "agent") == "codex" {
        choices.retain(|value| value != "ultra");
    }
    let reason = if !["codex", "kimi"].contains(&string(record, "agent")) {
        "Use the provider's Terminal to change effort"
    } else if value.effort.is_empty() || choices.is_empty() {
        "Waiting for the provider's model and effort settings"
    } else if !live_pane || !process_alive(record) {
        "Resume the session to change effort"
    } else if string(record, "phase") != "idle" || string(record, "activity") != "idle" {
        "Wait for Ready to change effort"
    } else {
        ""
    };
    let models = catalog(record);
    let settings_reason = settings_reason(record, &models);
    let when = if !live_pane || !process_alive(record) {
        "resume"
    } else if ready(record) {
        "now"
    } else {
        "ready"
    };
    let pending = &record["pending_settings"];
    json!({"model":value.model,"effort":value.effort,"effort_options":choices,
        "model_options":models.iter().map(|m| json!({"id":m.id,"label":m.label,"effort_options":m.efforts})).collect::<Vec<_>>(),
        "settings_change_supported":settings_reason.is_empty(),"settings_change_reason":settings_reason,
        "settings_scope":"session","settings_apply_when":when,
        "pending_model":string(pending,"model"),"pending_effort":string(pending,"effort"),
        "pending_settings_id":string(pending,"request_id"),
        "effort_change_supported":reason.is_empty(),"effort_change_reason":reason,
        "effort_scope":"session"})
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct Request {
    request_id: String,
    expected_run_id: String,
    expected_conversation_id: String,
    effort: String,
}

fn checked(name: &str, request: &Request, empty: bool) -> Result<Value> {
    let record = input::checked_settings(
        name,
        &request.expected_run_id,
        &request.expected_conversation_id,
    )?;
    input::checked_terminal(&record, empty)?;
    Ok(record)
}

fn send_key(record: &Value, key: &str) -> Result<()> {
    tmux(
        &["send-keys", "-t", string(record, "pane"), key].map(str::to_owned),
        true,
    )?;
    Ok(())
}

fn wait_until<T>(mut read: impl FnMut() -> Result<Option<T>>) -> Result<T> {
    let deadline = Instant::now() + Duration::from_secs(2);
    loop {
        if let Some(value) = read()? {
            return Ok(value);
        }
        if Instant::now() >= deadline {
            return Err("provider did not confirm the expected change; inspect Terminal".into());
        }
        std::thread::sleep(Duration::from_millis(50));
    }
}

fn codex_change(
    name: &str,
    request: &Request,
    current: &Settings,
    levels: &[String],
) -> Result<()> {
    let mut index = levels
        .iter()
        .position(|value| value == &current.effort)
        .ok_or("current effort is not in the provider model catalog; open Terminal")?;
    let target = levels
        .iter()
        .position(|value| value == &request.effort)
        .ok_or("unsupported effort for this model")?;
    while index != target {
        let record = checked(name, request, true)?;
        if footer("codex", &current.model, &capture(&record)?).as_deref()
            != Some(levels[index].as_str())
        {
            return Err("the provider effort changed; refresh before trying again".into());
        }
        let next = if target > index { index + 1 } else { index - 1 };
        send_key(&record, if target > index { "M-." } else { "M-," })?;
        wait_until(|| {
            let record = checked(name, request, false)?;
            Ok(
                (footer("codex", &current.model, &capture(&record)?).as_deref()
                    == Some(levels[next].as_str()))
                .then_some(()),
            )
        })?;
        input::checked_terminal(&record, true)?;
        index = next;
    }
    Ok(())
}

fn kimi_picker(screen: &str) -> Option<(Vec<String>, usize)> {
    let lines: Vec<_> = screen.lines().collect();
    let title = lines
        .iter()
        .position(|line| line.trim() == "Select thinking effort")?;
    if !lines
        .get(title + 1)?
        .contains("←→ switch · Enter select · Alt+S session-only · Esc cancel")
    {
        return None;
    }
    for line in lines.iter().skip(title + 2) {
        if !line.contains("[ ") || !line.contains(" ]") {
            continue;
        }
        let mut selected = None;
        let mut levels = Vec::new();
        let mut mark = false;
        for word in line.split_whitespace() {
            if word == "[" {
                mark = true;
                continue;
            }
            if word == "]" {
                mark = false;
                continue;
            }
            let word = word.to_ascii_lowercase();
            if !valid(&word) {
                return None;
            }
            if mark {
                if selected.is_some() {
                    return None;
                }
                selected = Some(levels.len());
            }
            if levels.contains(&word) {
                return None;
            }
            levels.push(word);
        }
        return Some((levels, selected?));
    }
    None
}

fn kimi_command_ready(record: &Value) -> Result<bool> {
    let output = tmux(
        &[
            "display-message",
            "-p",
            "-t",
            string(record, "pane"),
            "#{cursor_x}\t#{cursor_y}",
        ]
        .map(str::to_owned),
        true,
    )?;
    let cursor = String::from_utf8_lossy(&output.stdout);
    let Some((x, y)) = cursor.trim().split_once('\t') else {
        return Ok(false);
    };
    let screen = capture(record)?;
    let Some(row) = y.parse::<usize>().ok().and_then(|y| screen.lines().nth(y)) else {
        return Ok(false);
    };
    Ok(x == "12"
        && row.starts_with(" │ > /effort")
        && row.chars().skip(12).collect::<String>().trim() == "│")
}

fn kimi_change(name: &str, request: &Request, current: &Settings) -> Result<()> {
    let record = checked(name, request, true)?;
    if visible_effort(&record, &current.model, &capture(&record)?).as_deref()
        != Some(current.effort.as_str())
    {
        return Err("the current Kimi effort is not visible; open Terminal".into());
    }
    tmux(
        &["send-keys", "-t", string(&record, "pane"), "-l", "/effort"].map(str::to_owned),
        true,
    )?;
    wait_until(|| {
        let record = checked(name, request, false)?;
        Ok(kimi_command_ready(&record)?.then_some(()))
    })?;
    let record = checked(name, request, false)?;
    if !kimi_command_ready(&record)? {
        return Err("terminal changed before opening effort picker".into());
    }
    send_key(&record, "Enter")?;
    let (levels, mut index) = wait_until(|| {
        let record = checked(name, request, false)?;
        Ok(kimi_picker(&capture(&record)?))
    })?;
    let target = levels
        .iter()
        .position(|value| value == &request.effort)
        .ok_or("effort is not offered by the native picker; inspect Terminal")?;
    while index != target {
        let record = checked(name, request, false)?;
        if kimi_picker(&capture(&record)?) != Some((levels.clone(), index)) {
            return Err("effort picker changed; inspect Terminal".into());
        }
        let next = if target > index { index + 1 } else { index - 1 };
        send_key(&record, if target > index { "Right" } else { "Left" })?;
        wait_until(|| {
            let record = checked(name, request, false)?;
            Ok((kimi_picker(&capture(&record)?) == Some((levels.clone(), next))).then_some(()))
        })?;
        index = next;
    }
    let record = checked(name, request, false)?;
    if kimi_picker(&capture(&record)?) != Some((levels, target)) {
        return Err("effort picker changed before applying; inspect Terminal".into());
    }
    send_key(&record, "M-s")?; // Native session-only selection; Enter changes shared defaults.
    wait_until(|| {
        let record = checked(name, request, false)?;
        let screen = capture(&record)?;
        Ok((kimi_picker(&screen).is_none()
            && visible_effort(&record, &current.model, &screen).as_deref()
                == Some(request.effort.as_str()))
        .then_some(()))
    })?;
    input::checked_terminal(&record, true)
}

// Only interact with provider-owned settings dialogs that advertise their
// session-only action. Enter on the final selection would change shared defaults.
fn command_ready(record: &Value, command: &str) -> Result<bool> {
    let output = tmux(
        &[
            "display-message",
            "-p",
            "-t",
            string(record, "pane"),
            "#{cursor_x}\t#{cursor_y}",
        ]
        .map(str::to_owned),
        true,
    )?;
    let cursor = String::from_utf8_lossy(&output.stdout);
    let Some((x, y)) = cursor.trim().split_once('\t') else {
        return Ok(false);
    };
    let screen = capture(record)?;
    let Some(row) = y.parse::<usize>().ok().and_then(|y| screen.lines().nth(y)) else {
        return Ok(false);
    };
    let Some(x) = x.parse::<usize>().ok() else {
        return Ok(false);
    };
    let before = row.chars().take(x).collect::<String>();
    let after = row.chars().skip(x).collect::<String>();
    Ok(if string(record, "agent") == "kimi" {
        before.trim() == format!("│ > {command}") && after.trim() == "│"
    } else {
        before.trim() == format!("› {command}") && after.trim().is_empty()
    })
}

fn open_model(name: &str, request: &Request) -> Result<()> {
    let record = checked(name, request, true)?;
    tmux(
        &["send-keys", "-t", string(&record, "pane"), "-l", "/model"].map(str::to_owned),
        true,
    )?;
    wait_until(|| {
        let r = checked(name, request, false)?;
        Ok(command_ready(&r, "/model")?.then_some(()))
    })?;
    let record = checked(name, request, false)?;
    if !command_ready(&record, "/model")? {
        return Err("terminal changed before opening model settings".into());
    }
    send_key(&record, "Enter")
}

fn selected_codex(screen: &str) -> Option<String> {
    let mut selected = screen.lines().filter_map(|line| {
        let line = line.trim().strip_prefix('›')?.trim();
        let (number, rest) = line.split_once(". ")?;
        number.parse::<usize>().ok()?;
        Some(rest.to_owned())
    });
    let row = selected.next()?;
    selected.next().is_none().then_some(row)
}

fn row_is(row: &str, label: &str) -> bool {
    row.strip_prefix(label).is_some_and(|tail| {
        tail.is_empty()
            || tail.starts_with("  ")
            || tail.starts_with(" (current)")
            || tail.starts_with(" (default)")
    })
}

fn codex_select_row(name: &str, request: &Request, title: &str, label: &str) -> Result<String> {
    let mut visited = Vec::new();
    for _ in 0..256 {
        let record = checked(name, request, false)?;
        let screen = capture(&record)?;
        if !screen.lines().any(|l| l.trim() == title) {
            return Err("model settings dialog changed; inspect Terminal".into());
        }
        let selected =
            selected_codex(&screen).ok_or("model selection is not visible; enlarge Terminal")?;
        if row_is(&selected, label) {
            return Ok(screen);
        }
        if visited.contains(&selected) {
            return Err("requested choice is unavailable in the native model picker".into());
        }
        visited.push(selected.clone());
        send_key(&record, "Down")?;
        wait_until(|| {
            let r = checked(name, request, false)?;
            let s = capture(&r)?;
            Ok((selected_codex(&s).as_deref() != Some(selected.as_str())).then_some(()))
        })?;
    }
    Err("model picker exceeded the supported catalog size".into())
}

fn effort_label(value: &str) -> &str {
    match value {
        "xhigh" => "Extra high",
        "none" => "None",
        "minimal" => "Minimal",
        "low" => "Low",
        "medium" => "Medium",
        "high" => "High",
        "max" => "Max",
        _ => value,
    }
}

fn target_footer(record: &Value, model: &ModelOption, screen: &str) -> Option<String> {
    if string(record, "agent") == "codex" && model.efforts.is_empty() {
        return screen
            .lines()
            .rev()
            .take(3)
            .any(|line| {
                let line = line.trim().to_ascii_lowercase();
                [&model.id, &model.label]
                    .iter()
                    .any(|id| row_is(&line, &id.to_ascii_lowercase()))
            })
            .then(String::new);
    }
    if string(record, "agent") == "codex" {
        footer("codex", &model.id, screen).or_else(|| footer("codex", &model.label, screen))
    } else {
        kimi_effort(screen, &[&model.label, &model.id], kimi_custom_status(record))
    }
}

fn codex_model(name: &str, request: &Request, model: &ModelOption) -> Result<()> {
    open_model(name, request)?;
    let screen = wait_until(|| {
        let r = checked(name, request, false)?;
        let s = capture(&r)?;
        Ok(s.lines()
            .any(|l| ["Select Model", "Select Model and Effort"].contains(&l.trim()))
            .then_some(s))
    })?;
    if screen.lines().any(|l| l.trim() == "Select Model") {
        if model.id.starts_with("codex-auto-") {
            let s = codex_select_row(name, request, "Select Model", &model.label)?;
            if !s.contains("s session") {
                return Err(
                    "model picker does not offer a session-only action; use Terminal".into(),
                );
            }
            send_key(&checked(name, request, false)?, "s")?;
            let initial = wait_until(|| {
                let r = checked(name, request, false)?;
                Ok(target_footer(&r, model, &capture(&r)?))
            })?;
            let current = Settings {
                model: model.label.clone(),
                effort: initial,
                ..Default::default()
            };
            return codex_change(name, request, &current, &model.efforts);
        }
        codex_select_row(name, request, "Select Model", "All models")?;
        send_key(&checked(name, request, false)?, "Enter")?;
        wait_until(|| {
            let r = checked(name, request, false)?;
            Ok(capture(&r)?
                .lines()
                .any(|l| l.trim() == "Select Model and Effort")
                .then_some(()))
        })?;
    }
    let screen = codex_select_row(name, request, "Select Model and Effort", &model.label)?;
    if model.efforts.len() <= 1 && screen.contains("s session") {
        send_key(&checked(name, request, false)?, "s")?;
    } else {
        // This Enter opens the reasoning subdialog, it does not apply defaults.
        if model.efforts.len() <= 1 {
            return Err("model picker cannot safely apply this model session-only".into());
        }
        send_key(&checked(name, request, false)?, "Enter")?;
        let title = format!("Select Reasoning Level for {}", model.label);
        wait_until(|| {
            let r = checked(name, request, false)?;
            Ok(capture(&r)?
                .lines()
                .any(|l| l.trim() == title)
                .then_some(()))
        })?;
        let final_screen = if ["max", "ultra"].contains(&request.effort.as_str()) {
            codex_select_row(name, request, &title, "More reasoning…")?;
            send_key(&checked(name, request, false)?, "Enter")?;
            wait_until(|| {
                let r = checked(name, request, false)?;
                Ok(capture(&r)?
                    .lines()
                    .any(|l| l.trim() == "Advanced Reasoning")
                    .then_some(()))
            })?;
            codex_select_row(
                name,
                request,
                "Advanced Reasoning",
                if request.effort == "ultra" {
                    "Ultra"
                } else {
                    "Max"
                },
            )?
        } else {
            codex_select_row(name, request, &title, effort_label(&request.effort))?
        };
        if !final_screen.contains("s session") {
            return Err(
                "reasoning picker does not offer a session-only action; use Terminal".into(),
            );
        }
        send_key(&checked(name, request, false)?, "s")?;
    }
    wait_until(|| {
        let r = checked(name, request, false)?;
        Ok(
            (target_footer(&r, model, &capture(&r)?).as_deref() == Some(request.effort.as_str()))
                .then_some(()),
        )
    })?;
    input::checked_terminal(&checked(name, request, false)?, true)
}

fn kimi_model_row(screen: &str) -> Option<String> {
    if !screen
        .lines()
        .any(|l| l.trim().starts_with("Select a model"))
        || !screen.contains("Alt+S session-only")
    {
        return None;
    }
    let mut rows = screen
        .lines()
        .filter_map(|l| l.trim().strip_prefix("❯ ").map(str::to_owned));
    let row = rows.next()?;
    rows.next().is_none().then_some(row)
}

/// Whether the selected picker row is visibly the first and the last model row.
/// Model names on unselected rows start where the selected row's name starts.
fn kimi_model_edges(screen: &str) -> (bool, bool) {
    let lines: Vec<_> = screen.lines().collect();
    let Some(selected) = lines.iter().position(|l| l.trim_start().starts_with("❯ ")) else {
        return (false, false);
    };
    let column = lines[selected].chars().position(|c| c == '❯').unwrap_or(0) + 2;
    let row = |index: Option<usize>| {
        index.and_then(|i| lines.get(i)).is_some_and(|line| {
            let indent = line.chars().take_while(|c| *c == ' ').count();
            indent == column && line.chars().count() > column
        })
    };
    (!row(selected.checked_sub(1)), !row(Some(selected + 1)))
}

fn kimi_thinking(screen: &str) -> Option<String> {
    let lines: Vec<_> = screen.lines().collect();
    // The picker is below the conversation, which can quote "Thinking set to …".
    let title = lines
        .iter()
        .rposition(|l| l.trim().starts_with("Thinking"))?;
    let row = lines.get(title + 1)?;
    let value = row
        .split_once("[ ")?
        .1
        .split_once(" ]")?
        .0
        .to_ascii_lowercase();
    valid(&value).then_some(value)
}

fn kimi_model(name: &str, request: &Request, model: &ModelOption) -> Result<()> {
    open_model(name, request)?;
    wait_until(|| {
        let r = checked(name, request, false)?;
        Ok(kimi_model_row(&capture(&r)?))
    })?;
    let mut visited = Vec::new();
    let mut tabs = Vec::new();
    // Older pickers wrap around; Kimi 2.1 stops at either end, so a key that
    // does not move the selection turns the walk around once per provider.
    let mut down = true;
    let mut turned = false;
    for _ in 0..256 {
        let record = checked(name, request, false)?;
        let screen = capture(&record)?;
        let row = kimi_model_row(&screen).ok_or("model picker changed; inspect Terminal")?;
        if row_is(&row, &model.label) {
            break;
        }
        if visited.contains(&row) {
            if !screen.contains("Tab toggle provider") || tabs.contains(&row) {
                return Err("model is not available in the native picker".into());
            }
            tabs.push(row.clone());
            visited.clear();
            (down, turned) = (true, false);
            send_key(&record, "Tab")?;
            wait_until(|| {
                let r = checked(name, request, false)?;
                Ok((kimi_model_row(&capture(&r)?).as_deref() != Some(row.as_str())).then_some(()))
            })?;
            continue;
        }
        send_key(&record, if down { "Down" } else { "Up" })?;
        let deadline = Instant::now() + Duration::from_secs(2);
        let moved = loop {
            let r = checked(name, request, false)?;
            if kimi_model_row(&capture(&r)?).as_deref() != Some(row.as_str()) {
                break true;
            }
            if Instant::now() >= deadline {
                break false;
            }
            std::thread::sleep(Duration::from_millis(50));
        };
        if moved {
            visited.push(row);
            continue;
        }
        // Turn only at a visible end of the list. A key that does not move the
        // selection elsewhere may still be pending, so stop instead of guessing.
        let (top, bottom) = kimi_model_edges(&capture(&checked(name, request, false)?)?);
        if down && !turned && bottom {
            // Walk up; rows passed on the way down are not a loop.
            (down, turned) = (false, true);
            visited.clear();
        } else if !down && top {
            visited.push(row);
        } else {
            return Err("model picker selection did not move; inspect Terminal".into());
        }
    }
    for _ in 0..16 {
        let record = checked(name, request, false)?;
        let screen = capture(&record)?;
        if !kimi_model_row(&screen).is_some_and(|r| row_is(&r, &model.label)) {
            return Err("requested model is not selected".into());
        }
        let effort =
            kimi_thinking(&screen).ok_or("native model thinking selection is not visible")?;
        if effort == request.effort {
            send_key(&record, "M-s")?;
            wait_until(|| {
                let r = checked(name, request, false)?;
                Ok((target_footer(&r, model, &capture(&r)?).as_deref()
                    == Some(request.effort.as_str()))
                .then_some(()))
            })?;
            return input::checked_terminal(&checked(name, request, false)?, true);
        }
        let current = model
            .efforts
            .iter()
            .position(|v| v == &effort)
            .ok_or("unsupported native thinking selection")?;
        let target = model
            .efforts
            .iter()
            .position(|v| v == &request.effort)
            .ok_or("unsupported effort")?;
        send_key(&record, if target > current { "Right" } else { "Left" })?;
        wait_until(|| {
            let r = checked(name, request, false)?;
            Ok((kimi_thinking(&capture(&r)?).as_deref() != Some(effort.as_str())).then_some(()))
        })?;
    }
    Err("native model effort did not settle".into())
}

pub(super) fn dispatch(args: &[String]) -> Result<i32> {
    if args.len() != 2 || args[1] != "--json" {
        return Err("usage: hgs effort <session> --json (JSON from stdin)".into());
    }
    let mut bytes = Vec::new();
    io::stdin()
        .take(4097)
        .read_to_end(&mut bytes)
        .map_err(|e| e.to_string())?;
    if bytes.len() > 4096 {
        return Err("effort payload exceeds 4 KiB".into());
    }
    let request: Request =
        serde_json::from_slice(&bytes).map_err(|e| format!("invalid effort payload: {e}"))?;
    let id = uuid::Uuid::parse_str(&request.request_id)
        .map_err(|_| "request_id must be a UUID")?
        .to_string();
    if request.expected_run_id.is_empty()
        || request.expected_conversation_id.is_empty()
        || !valid(&request.effort)
    {
        return Err("a confirmed session identity and supported effort are required".into());
    }
    let name = &args[0];
    let digest = format!("{:x}", Sha256::digest(&bytes));
    let receipt_path = absolute_root()?
        .join("effort_receipts")
        .join(format!("{id}.json"));
    let _guard = lock(None)?;
    if receipt_path.exists() {
        let text = fs::read_to_string(&receipt_path).map_err(|e| e.to_string())?;
        let receipt: Value = serde_json::from_str(&text).map_err(|e| e.to_string())?;
        if string(&receipt, "name") != name || string(&receipt, "digest") != digest {
            return Err("request ID was already used for different content".into());
        }
        if receipt["status"] != "confirmed" {
            return Err("effort change may already have reached the provider; inspect Terminal before retrying".into());
        }
        println!("{text}");
        return Ok(0);
    }
    let mut record = checked(name, &request, true)?;
    let current = settings(&record, true);
    if visible_effort(&record, &current.model, &capture(&record)?).as_deref()
        != Some(current.effort.as_str())
    {
        return Err("the current provider effort is not visible; open Terminal".into());
    }
    let mut levels = options(&record, &current.model);
    if string(&record, "agent") == "codex" && request.effort == "ultra" {
        return Err("Ultra requires the explicit advanced reasoning picker in Terminal".into());
    }
    if levels.len() > 16 || !levels.contains(&request.effort) || current.effort.is_empty() {
        return Err("effort is unavailable for the active model; refresh or use Terminal".into());
    }
    // Codex walks regular choices followed by advanced choices (Max, Ultra).
    if string(&record, "agent") == "codex" {
        levels.sort_by_key(|v| match v.as_str() {
            "max" => 1,
            "ultra" => 2,
            _ => 0,
        });
    }
    let mut receipt = json!({"request_id":id,"name":name,"digest":digest,"status":"in_progress", "run_id":request.expected_run_id,"conversation_id":request.expected_conversation_id,"effort":request.effort,"model":current.model,"scope":"session","at":now()});
    atomic(&receipt_path, &receipt.to_string())?;
    let result = if current.effort == request.effort {
        Ok(())
    } else {
        match string(&record, "agent") {
            "codex" => codex_change(name, &request, &current, &levels),
            "kimi" => kimi_change(name, &request, &current),
            _ => Err("change this provider's effort in Terminal".into()),
        }
    };
    if let Err(error) = result {
        record["input_pending_at"] = json!(now());
        let _ = write(&mut record);
        return Err(format!("effort change not confirmed: {error}"));
    }
    let latest = history(&record);
    record["effort"] = json!(request.effort);
    record["model"] = json!(current.model);
    record["effort_source_path"] = json!(latest.path);
    record["effort_source_offset"] = json!(latest.offset);
    write(&mut record)?;
    receipt["status"] = json!("confirmed");
    atomic(&receipt_path, &receipt.to_string())?;
    println!("{receipt}");
    Ok(0)
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct SettingsRequest {
    request_id: String,
    expected_run_id: String,
    expected_conversation_id: String,
    #[serde(default)]
    model: Option<String>,
    #[serde(default)]
    effort: Option<String>,
    #[serde(default)]
    expected_pending_id: Option<String>,
}

fn requested_model(record: &Value, request: &SettingsRequest) -> Result<(ModelOption, String)> {
    let models = catalog(record);
    let reason = settings_reason(record, &models);
    if !reason.is_empty() {
        return Err(reason.into());
    }
    let current = settings(record, true);
    let id = request.model.as_deref().unwrap_or_else(|| {
        let pending = string(&record["pending_settings"], "model");
        if pending.is_empty() {
            &current.model
        } else {
            pending
        }
    });
    let model = models
        .iter()
        .find(|m| m.id == id)
        .ok_or("model is not in this account's provider catalog")?
        .clone();
    let effort = request.effort.clone().unwrap_or_else(|| {
        let pending = string(&record["pending_settings"], "effort");
        if string(&record["pending_settings"], "model") == id
            && model.efforts.iter().any(|e| e == pending)
        {
            pending.into()
        } else if model.efforts.contains(&current.effort) {
            current.effort.clone()
        } else if model.efforts.contains(&model.default_effort) {
            model.default_effort.clone()
        } else {
            model.efforts.first().cloned().unwrap_or_default()
        }
    });
    if (!effort.is_empty() && !model.efforts.contains(&effort))
        || (effort.is_empty() && !model.efforts.is_empty())
    {
        return Err("effort is unavailable for the selected model".into());
    }
    if id != current.model && models.iter().filter(|m| m.label == model.label).count() != 1 {
        return Err("provider model names are ambiguous; select this model in Terminal".into());
    }
    Ok((model, effort))
}

fn apply_settings(
    name: &str,
    request: &Request,
    model: &ModelOption,
    receipt_path: &Path,
    receipt: &mut Value,
) -> Result<Value> {
    let _driving = startup_choice::driving(name)?;
    let mut record = checked(name, request, true)?;
    let current = settings(&record, true);
    receipt["status"] = json!("in_progress");
    receipt["run_id"] = json!(request.expected_run_id);
    receipt["name"] = json!(name);
    atomic(receipt_path, &receipt.to_string())?;
    let visible_effort = target_footer(&record, model, &capture(&record)?);
    let result = if visible_effort.as_deref() == Some(request.effort.as_str()) {
        Ok(())
    } else if current.model != model.id
        || visible_effort.is_none()
        || (string(&record, "agent") == "codex" && request.effort == "ultra")
    {
        match string(&record, "agent") {
            "codex" => codex_model(name, request, model),
            "kimi" => kimi_model(name, request, model),
            _ => Err("unsupported model adapter".into()),
        }
    } else {
        match string(&record, "agent") {
            "codex" => {
                let visible = Settings {
                    model: model.label.clone(),
                    effort: visible_effort
                        .clone()
                        .unwrap_or_else(|| current.effort.clone()),
                    ..current.clone()
                };
                codex_change(name, request, &visible, &model.efforts)
            }
            "kimi" => kimi_change(
                name,
                request,
                &Settings {
                    effort: visible_effort
                        .clone()
                        .unwrap_or_else(|| current.effort.clone()),
                    ..current.clone()
                },
            ),
            _ => Err("unsupported effort adapter".into()),
        }
    };
    if let Err(error) = result {
        record["input_pending_at"] = json!(now());
        let _ = write(&mut record);
        return Err(format!("settings change not confirmed: {error}"));
    }
    let latest = history(&record);
    record["model"] = json!(model.id);
    record["effort"] = json!(request.effort);
    record["effort_source_path"] = json!(latest.path);
    record["effort_source_offset"] = json!(latest.offset);
    record["resume_settings"] = json!({"model":model.id,"effort":request.effort});
    record.as_object_mut().unwrap().remove("pending_settings");
    write(&mut record)?;
    receipt["status"] = json!("applied");
    atomic(receipt_path, &receipt.to_string())?;
    Ok(record)
}

pub(super) fn dispatch_settings(args: &[String]) -> Result<i32> {
    if args.len() != 2 || args[1] != "--json" {
        return Err("usage: hgs settings <session> --json (JSON from stdin)".into());
    }
    let mut bytes = Vec::new();
    io::stdin()
        .take(4097)
        .read_to_end(&mut bytes)
        .map_err(|e| e.to_string())?;
    if bytes.len() > 4096 {
        return Err("settings payload exceeds 4 KiB".into());
    }
    let request: SettingsRequest =
        serde_json::from_slice(&bytes).map_err(|e| format!("invalid settings payload: {e}"))?;
    let id = uuid::Uuid::parse_str(&request.request_id)
        .map_err(|_| "request_id must be a UUID")?
        .to_string();
    if request.expected_run_id.is_empty()
        || (request.model.is_none() && request.effort.is_none())
        || request.model.as_deref().is_some_and(|v| !safe_model(v))
        || request
            .effort
            .as_deref()
            .is_some_and(|v| !v.is_empty() && !valid(v))
    {
        return Err(
            "confirmed session identity and a supported model or effort are required".into(),
        );
    }
    let name = &args[0];
    let digest = format!("{:x}", Sha256::digest(&bytes));
    let receipt_path = absolute_root()?
        .join("settings_receipts")
        .join(format!("{id}.json"));
    let _guard = lock(None)?;
    if receipt_path.exists() {
        let text = fs::read_to_string(&receipt_path).map_err(|e| e.to_string())?;
        let receipt: Value = serde_json::from_str(&text).map_err(|e| e.to_string())?;
        if string(&receipt, "name") != name || string(&receipt, "digest") != digest {
            return Err("request ID was already used for different content".into());
        }
        if !["applied", "scheduled"].contains(&string(&receipt, "status")) {
            return Err("settings change may already have reached the provider; inspect Terminal before retrying".into());
        }
        println!("{text}");
        return Ok(0);
    }
    let mut record = read(name)?;
    if string(&record, "run_id") != request.expected_run_id
        || string(&record, "conversation_id") != request.expected_conversation_id
    {
        return Err("session identity changed; refresh before changing settings".into());
    }
    if request
        .expected_pending_id
        .as_deref()
        .is_some_and(|id| id != string(&record["pending_settings"], "request_id"))
    {
        return Err("pending settings changed; refresh before applying".into());
    }
    let (model, effort) = requested_model(&record, &request)?;
    if request.expected_pending_id.is_some()
        && (string(&record["pending_settings"], "model") != model.id
            || string(&record["pending_settings"], "effort") != effort)
    {
        return Err("pending settings target changed; refresh before applying".into());
    }
    let running = process_alive(&record);
    if running && !matches(&record, live()?.get(name)) {
        return Err(
            "the tracked terminal identity changed; refresh before changing settings".into(),
        );
    }
    let mut receipt = json!({"request_id":id,"name":name,"digest":digest,"status":"scheduled","run_id":request.expected_run_id,"conversation_id":request.expected_conversation_id,"model":model.id,"effort":effort,"scope":"session","at":now()});
    if request.expected_pending_id.is_some() {
        let pending_id = string(&record["pending_settings"], "request_id").to_owned();
        if running && ready(&record) {
            receipt["status"] = json!("in_progress");
            atomic(&receipt_path, &receipt.to_string())?;
            apply_pending(name, record)?;
            receipt["status"] = json!("applied");
        } else {
            receipt["pending_settings_id"] = json!(pending_id);
            receipt["apply_when"] = json!(if running { "ready" } else { "resume" });
        }
        atomic(&receipt_path, &receipt.to_string())?;
        println!("{receipt}");
        return Ok(0);
    }
    if running && ready(&record) {
        let native = Request {
            request_id: id,
            expected_run_id: request.expected_run_id,
            expected_conversation_id: request.expected_conversation_id,
            effort,
        };
        apply_settings(name, &native, &model, &receipt_path, &mut receipt)?;
    } else {
        if !running {
            recipes::argv(&record)?;
        }
        // No terminal bytes on a busy or stopped session. The UI applies this
        // explicit request once Ready; send also applies it before the next idle
        // message. A busy message keeps the currently active settings.
        let original = record.clone();
        record["pending_settings"] = json!({"request_id":id,"model":model.id,"effort":effort,"run_id":request.expected_run_id,"conversation_id":request.expected_conversation_id,"receipt_name":name,"receipt_run_id":request.expected_run_id});
        record["resume_settings"] = json!({"model":model.id,"effort":effort});
        write(&mut record)?;
        receipt["pending_settings_id"] = json!(id);
        receipt["apply_when"] = json!(if running { "ready" } else { "resume" });
        if let Err(error) = atomic(&receipt_path, &receipt.to_string()) {
            let mut restore = original;
            write(&mut restore).map_err(|rollback| {
                format!("{error}; could not restore previous settings: {rollback}")
            })?;
            return Err(error);
        }
    }
    println!("{receipt}");
    Ok(0)
}

// Called under the existing input transaction lock; never a mutation from ls or
// inspect. It deliberately does nothing while the agent is still working.
pub(super) fn apply_pending(name: &str, record: Value) -> Result<Value> {
    let pending = &record["pending_settings"];
    if !pending.is_object() || !ready(&record) {
        return Ok(record);
    }
    if string(pending, "run_id") != string(&record, "run_id")
        || string(pending, "conversation_id") != string(&record, "conversation_id")
    {
        return Err("pending settings belong to a different session; refresh settings".into());
    }
    let request = SettingsRequest {
        request_id: string(pending, "request_id").into(),
        expected_run_id: string(&record, "run_id").into(),
        expected_conversation_id: string(&record, "conversation_id").into(),
        model: Some(string(pending, "model").into()),
        effort: Some(string(pending, "effort").into()),
        expected_pending_id: None,
    };
    let (model, effort) = requested_model(&record, &request)?;
    let id = uuid::Uuid::parse_str(&request.request_id)
        .map_err(|_| "invalid pending settings ID")?
        .to_string();
    let path = absolute_root()?
        .join("settings_receipts")
        .join(format!("{id}.json"));
    let mut receipt: Value =
        serde_json::from_str(&fs::read_to_string(&path).map_err(|e| e.to_string())?)
            .map_err(|e| e.to_string())?;
    if string(&receipt, "status") != "scheduled"
        || string(&receipt, "name") != string(pending, "receipt_name")
        || string(&receipt, "run_id") != string(pending, "receipt_run_id")
        || string(&receipt, "conversation_id") != request.expected_conversation_id
    {
        return Err("pending settings are not safe to retry; inspect Terminal".into());
    }
    let native = Request {
        request_id: id,
        expected_run_id: request.expected_run_id,
        expected_conversation_id: request.expected_conversation_id,
        effort,
    };
    apply_settings(name, &native, &model, &path, &mut receipt)
}

// Kimi has a per-invocation model flag but no effort flag. Restore its saved
// session effort through the same guarded adapter once the resumed TUI is Ready.
pub(super) fn restore_pending(record: &mut Value) -> Result<()> {
    if record["resume_settings"].is_object() {
        record["resume_settings"] = effective_resume_settings(record);
    }
    if string(record, "agent") != "kimi"
        || record["pending_settings"].is_object()
        || !safe_model(string(&record["resume_settings"], "model"))
    {
        return Ok(());
    }
    let id = uuid::Uuid::new_v4().to_string();
    let model = string(&record["resume_settings"], "model").to_owned();
    let effort = string(&record["resume_settings"], "effort").to_owned();
    record["pending_settings"] = json!({"request_id":id,"run_id":record["run_id"],"conversation_id":record["conversation_id"],"model":model,"effort":effort,"receipt_name":record["name"],"receipt_run_id":record["run_id"]});
    let receipt = json!({"request_id":id,"name":record["name"],"run_id":record["run_id"],"conversation_id":record["conversation_id"],"model":model,"effort":effort,"scope":"session","status":"scheduled","apply_when":"ready","at":now()});
    atomic(
        &absolute_root()?
            .join("settings_receipts")
            .join(format!("{id}.json")),
        &receipt.to_string(),
    )
}

fn effective_resume_settings(record: &Value) -> Value {
    let native = history(record);
    if !record["pending_settings"].is_object()
        && safe_model(&native.model)
        && (native.path != string(record, "effort_source_path")
            || native.offset > record["effort_source_offset"].as_u64().unwrap_or(0))
    {
        json!({"model":native.model,"effort":native.effort})
    } else {
        record["resume_settings"].clone()
    }
}

pub(super) fn resume_argv(record: &Value, mut args: Vec<String>) -> Vec<String> {
    let effective = effective_resume_settings(record);
    let settings = &effective;
    if let Some(model) = settings["model"].as_str().filter(|m| safe_model(m)) {
        // argparse rejects duplicate scalar options in some providers. Remove
        // only the two owned overrides; keep every unrelated launch option.
        let mut clean = Vec::new();
        let mut i = 0;
        while i < args.len() {
            let key = args[i].split('=').next().unwrap_or("");
            if ["--model", "-m"].contains(&key) {
                i += if args[i].contains('=') { 1 } else { 2 };
                continue;
            }
            if string(record, "agent") == "codex" && ["-c", "--config"].contains(&key) {
                let value = if let Some((_, v)) = args[i].split_once('=') {
                    Some(v)
                } else {
                    args.get(i + 1).map(String::as_str)
                };
                if value.is_some_and(|v| {
                    v.starts_with("model=") || v.starts_with("model_reasoning_effort=")
                }) {
                    i += if args[i].contains('=') { 1 } else { 2 };
                    continue;
                }
            }
            clean.push(args[i].clone());
            i += 1;
        }
        args = clean;
        args.extend(["--model".into(), model.into()]);
        if string(record, "agent") == "codex" {
            if let Some(effort) = settings["effort"].as_str().filter(|v| valid(v)) {
                args.extend(["-c".into(), format!("model_reasoning_effort=\"{effort}\"")]);
            }
        }
    }
    args
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn provider_settings_ignore_text_and_thinking_content() {
        assert_eq!(
            event_settings(
                "codex",
                &json!({"type":"turn_context","payload":{"model":"gpt-6-astra","effort":"xhigh"}})
            ),
            Some(("gpt-6-astra".into(), "xhigh".into()))
        );
        assert_eq!(
            event_settings(
                "kimi",
                &json!({"type":"llm.request","modelAlias":"kimi-code/k3","thinkingEffort":"max"})
            ),
            Some(("kimi-code/k3".into(), "max".into()))
        );
        assert!(event_settings(
            "kimi",
            &json!({"type":"agent.message.appended","modelAlias":"wrong","thinkingEffort":"high"})
        )
        .is_none());
    }
    #[test]
    fn exact_conversation_history_and_confirmed_override() {
        let root = tempfile::tempdir().unwrap();
        let id = "12345678-1234-1234-1234-123456789abc";
        let path = root.path().join(format!("rollout-{id}.jsonl"));
        let context = |effort: &str| {
            json!({"type":"turn_context","payload":{"model":"gpt-6-astra","effort":effort}})
                .to_string()
                + "\n"
        };
        let header = json!({"type":"session_meta","payload":{"id":id}}).to_string() + "\n";
        fs::write(&path, header.clone() + &context("high")).unwrap();
        let mut record =
            json!({"agent":"codex","conversation_id":id,"transcript":path,"model":"gpt-6-astra"});
        let initial = history(&record);
        assert_eq!(initial.effort, "high");
        record["effort"] = json!("low");
        record["effort_source_path"] = json!(initial.path);
        record["effort_source_offset"] = json!(initial.offset);
        assert_eq!(settings(&record, false).effort, "low");
        fs::write(&path, header.clone() + &context("high") + &context("max")).unwrap();
        assert_eq!(settings(&record, false).effort, "max");
        // A matching filename is not enough to trust another conversation.
        fs::write(
            &path,
            json!({"type":"session_meta","payload":{"id":"different"}}).to_string()
                + "\n"
                + &context("max"),
        )
        .unwrap();
        assert!(history(&record).effort.is_empty());
        fs::write(
            &path,
            header + &context("medium") + "{\"type\":\"turn_context\",\"payload\":",
        )
        .unwrap();
        assert_eq!(history(&record).effort, "medium");
    }

    #[test]
    fn native_model_names_require_exact_column_boundaries() {
        assert!(row_is("K3  kimi", "K3"));
        assert!(!row_is("K3 Pro  kimi", "K3"));
        assert!(row_is("GPT-6-Sol (current)  Workhorse", "GPT-6-Sol"));
        assert!(!row_is("GPT-6-Sol Mini  Lightweight", "GPT-6-Sol"));
        let model = ModelOption {
            id: "k3".into(),
            label: "K3".into(),
            efforts: vec!["high".into()],
            default_effort: "high".into(),
        };
        assert_eq!(
            target_footer(
                &json!({"agent":"kimi"}),
                &model,
                "Ask When Needed  K3 thinking: high  /tmp\n context: 2%"
            ),
            Some("high".into())
        );
        assert!(target_footer(
            &json!({"agent":"kimi","model":"k3"}),
            &model,
            "Ask When Needed  K3 Pro thinking: high  /tmp\n context: 2%"
        )
        .is_none());
    }

    #[test]
    fn resume_preserves_newer_native_settings_unless_an_intent_is_pending() {
        let root = tempfile::tempdir().unwrap();
        let id = "native-session";
        let path = root.path().join(format!("rollout-{id}.jsonl"));
        let header = json!({"type":"session_meta","payload":{"id":id}}).to_string() + "\n";
        fs::write(
            &path,
            header.clone()
                + &json!({"type":"turn_context","payload":{"model":"native-model","effort":"high"}})
                    .to_string() + "\n",
        )
        .unwrap();
        let mut record = json!({"agent":"codex","conversation_id":id,"transcript":path,"resume_settings":{"model":"old-override","effort":"low"},"effort_source_path":path,"effort_source_offset":header.len()});
        let args = resume_argv(&record, vec!["codex".into(), "resume".into(), id.into()]);
        assert!(args.contains(&"native-model".into()));
        assert!(args.contains(&"model_reasoning_effort=\"high\"".into()));
        record["pending_settings"] = json!({"model":"old-override","effort":"low"});
        let args = resume_argv(&record, vec!["codex".into(), "resume".into(), id.into()]);
        assert!(args.contains(&"old-override".into()));
        assert!(!args.contains(&"native-model".into()));
        let model = ModelOption {
            id: "k3".into(),
            label: "K3".into(),
            efforts: vec!["high".into()],
            default_effort: "high".into(),
        };
        assert!(target_footer(
            &json!({"agent":"kimi","model":"k3"}),
            &model,
            "Ask When Needed  Pro K3 thinking: high  /tmp\n context: 2%"
        )
        .is_none());
    }

    #[test]
    fn settings_searches_without_a_result_are_kept_across_listings() {
        let temp = tempfile::tempdir().unwrap();
        let cache = temp.path().join("cache");
        scan_cache::DIR.with(|dir| *dir.borrow_mut() = Some(cache.clone()));
        let id = uuid::Uuid::new_v4().to_string();
        let path = temp.path().join(format!("rollout-{id}.jsonl"));
        // A long conversation whose tail has no settings record searches up to 8 MiB.
        let filler = json!({"type":"event_msg","payload":{"type":"agent_message","message":"x".repeat(1000)}}).to_string() + "\n";
        fs::write(&path, json!({"type":"session_meta","payload":{"id":id}}).to_string() + "\n" + &filler.repeat(200)).unwrap();
        let record = json!({"agent":"codex","conversation_id":id,"transcript":path});
        assert!(history(&record).model.is_empty());
        let entries: Vec<_> = fs::read_dir(&cache).unwrap().collect();
        assert_eq!(entries.len(), 1);
        let stored: Value = serde_json::from_str(&fs::read_to_string(entries[0].as_ref().unwrap().path()).unwrap()).unwrap();
        assert_eq!(stored["value"]["model"], "");
        scan_cache::DIR.with(|dir| *dir.borrow_mut() = None);
    }

    #[test]
    fn model_cache_is_parsed_once_per_file_version() {
        let root = tempfile::tempdir().unwrap();
        let path = root.path().join("models_cache.json");
        let models = |levels: &[&str]| json!({"models":[{"slug":"m","supported_reasoning_levels":
            levels.iter().map(|level| json!({"effort":level})).collect::<Vec<_>>()}]}).to_string();
        fs::write(&path, models(&["low"])).unwrap();
        let record = json!({"agent":"codex","agent_home":root.path()});
        let parses = || MODEL_CACHE_PARSES.with(|count| count.get());
        let before = parses();
        // Each listed session asks for its options and catalog: one parse serves them all.
        for _ in 0..5 {
            assert_eq!(options(&record, "m"), vec!["low"]);
            assert_eq!(catalog(&record)[0].efforts, vec!["low"]);
        }
        assert_eq!(parses() - before, 1);
        // A rewritten file is parsed again, even within the same second.
        fs::write(&path, models(&["low", "high"])).unwrap();
        assert_eq!(options(&record, "m"), vec!["low", "high"]);
        assert_eq!(catalog(&record)[0].efforts, vec!["low", "high"]);
        assert_eq!(parses() - before, 2);
        fs::remove_file(&path).unwrap();
        assert!(options(&record, "m").is_empty());
        assert!(catalog(&record).is_empty());
    }

    #[test]
    fn account_catalog_and_resume_override_are_scoped() {
        let root = tempfile::tempdir().unwrap();
        fs::write(root.path().join("models_cache.json"),json!({"models":[
            {"slug":"available","display_name":"Available model","supported_reasoning_levels":[{"effort":"low"},{"effort":"unknown-level"},{"effort":"ultra"}]},
            {"slug":"hidden","visibility":"hide","supported_reasoning_levels":[]}
        ]}).to_string()).unwrap();
        let mut record = json!({"agent":"codex","agent_home":root.path(),"resume_settings":{"model":"available","effort":"low"}});
        let models = catalog(&record);
        assert_eq!(models.len(), 1);
        assert_eq!(models[0].id, "available");
        assert_eq!(models[0].efforts, vec!["low", "ultra"]);
        let args = vec![
            "codex",
            "resume",
            "conversation",
            "--model=old",
            "-c",
            "model_reasoning_effort=high",
            "--profile",
            "work",
            "-c",
            "sandbox_mode=read-only",
        ]
        .into_iter()
        .map(str::to_owned)
        .collect();
        assert_eq!(
            resume_argv(&record, args),
            vec![
                "codex",
                "resume",
                "conversation",
                "--profile",
                "work",
                "-c",
                "sandbox_mode=read-only",
                "--model",
                "available",
                "-c",
                "model_reasoning_effort=\"low\""
            ]
        );
        record["archive_id"] = json!("archived");
        assert!(settings_reason(&record, &models).contains("Restore"));
        assert_eq!(fs::read_dir(root.path()).unwrap().count(), 1);
    }

    #[test]
    fn parses_only_native_footer_and_picker() {
        assert_eq!(
            footer(
                "codex",
                "gpt-6-astra",
                "\n  GPT-6-Astra xhigh · /work\n  ? for shortcuts"
            ),
            Some("xhigh".into())
        );
        let effort = |screen: &str, name: &str| kimi_effort(screen, &[name], false);
        assert_eq!(effort(" Ask When Needed  K3 thinking: max  /work\n context: 22%", "K3").as_deref(), Some("max"));
        assert!(effort("User said thinking: high", "K3").is_none());
        // Kimi 2.1 status rows under its composer, captured from the native TUI or
        // laid out by its footer slots (mode, goal, model, tasks, folder, git).
        let composer = |status: &str| {
            format!(" ● Fixture reply: hello thinking: max\n ╭────╮\n │ >  │\n ╰────╯\n {status}\n                    context: 1% (15/32k)")
        };
        for (status, name, expected) in [
            ("kimi-levels thinking: high  ~/sample-project  main [±]          ctrl+c: cancel", "kimi-levels", "high"),
            ("kimi-think thinking  ~/sample-project  main [±]", "kimi-think", "on"),
            ("kimi-think  ~/sample-project  main [±]", "kimi-think", "off"),
            ("Levels Pro thinking: max  /work", "Levels Pro", "max"),
            ("Never Ask  kimi-think thinking  ~/work", "kimi-think", "on"),
            ("plan  K3 thinking: max  ~/work", "K3", "max"),
            ("Ask When Needed plan  K3 thinking: max  ~/work", "K3", "max"),
            ("swarm  [goal ● paused · 7m · 3 turns]  K3 thinking: high  ~/work", "K3", "high"),
            ("~/work  K3 thinking: low", "K3", "low"),
        ] {
            assert_eq!(effort(&composer(status), name).as_deref(), Some(expected), "{status}");
        }
        assert!(effort(&composer("K3 Pro thinking: max  ~/work"), "K3").is_none());
        assert!(effort(&composer("plan  K3 thinking: unknown  ~/work"), "K3").is_none());
        assert!(effort(&composer("ctrl+c again to exit"), "K3").is_none());
        assert!(effort(" ● Fixture reply: K3  ~/work", "K3").is_none());
        // A custom status line command need not show effort: never read "off".
        assert!(kimi_effort(&composer("K3  ~/work  main"), &["K3"], true).is_none());
        assert_eq!(kimi_effort(&composer("K3 thinking: max  ~/work"), &["K3"], true).as_deref(), Some("max"));
        let model = |id: &str, label: &str, efforts: &[&str]| ModelOption {
            id: id.into(),
            label: label.into(),
            efforts: efforts.iter().map(|v| v.to_string()).collect(),
            default_effort: String::new(),
        };
        let home = tempfile::tempdir().unwrap();
        let kimi = json!({"agent":"kimi","agent_home":home.path()});
        let plain = composer("kimi-plain  ~/sample-project  main [±]");
        assert_eq!(
            target_footer(&kimi, &model("fixture-plain", "kimi-plain", &["off"]), &plain),
            Some("off".into())
        );
        assert!(target_footer(&kimi, &model("fixture-think", "kimi-think", &["on", "off"]), &plain).is_none());
        // A status line command replaces the built-in line: a bare name is not "off".
        fs::write(home.path().join("tui.toml"), "[status_line]\ncommand = \"my-status\"\n").unwrap();
        assert!(target_footer(&kimi, &model("fixture-plain", "kimi-plain", &["off"]), &plain).is_none());
        fs::write(home.path().join("tui.toml"), "[status_line]\nitems = [\"cwd\", \"model\"]\n").unwrap();
        assert_eq!(
            target_footer(&kimi, &model("fixture-plain", "kimi-plain", &["off"]), &plain),
            Some("off".into())
        );
        // The status line shows the alias as overridden, like Kimi's effective model.
        fs::write(
            home.path().join("config.toml"),
            "[models.\"kimi-code/k3\"]\nmodel = \"k3\"\ndisplay_name = \"K3\"\nsupport_efforts = [\"low\", \"high\"]\n[models.\"kimi-code/k3\".overrides]\ndisplay_name = \"K3 Max\"\nsupport_efforts = [\"high\", \"max\"]\n",
        )
        .unwrap();
        let k3 = catalog(&kimi).into_iter().find(|m| m.id == "kimi-code/k3").unwrap();
        assert_eq!((k3.label.as_str(), k3.efforts.clone()), ("K3 Max", vec!["off".to_owned(), "high".into(), "max".into()]));
        assert_eq!(
            visible_effort(&kimi, "kimi-code/k3", &composer("K3 Max thinking: max  ~/work")).as_deref(),
            Some("max")
        );
        let panel = "────\n Select thinking effort\n ←→ switch · Enter select · Alt+S session-only · Esc cancel\n\n Low  [ High ]  Max\n────";
        assert_eq!(
            kimi_picker(panel),
            Some((vec!["low".into(), "high".into(), "max".into()], 1))
        );
        assert!(kimi_picker(&panel.replace("Alt+S session-only · ", "")).is_none());
        // Kimi 2.1 model picker below a conversation that reported an earlier change.
        let models = " Thinking set to max for this session only.\n ────\n  Select a model  (type to search)\n  Tab toggle provider · ↑↓ navigate · Enter select · Alt+S session-only · Es...\n   All   fixture\n     kimi-think   fixture\n   ❯ kimi-plain   fixture\n     kimi-levels  fixture ← current\n  Thinking\n     On (Unsupported)   [ Off ]\n ────";
        assert_eq!(kimi_model_row(models).as_deref(), Some("kimi-plain   fixture"));
        assert_eq!(kimi_thinking(models).as_deref(), Some("off"));
        assert_eq!(
            kimi_thinking(&models.replace("On (Unsupported)   [ Off ]", "[ On ]    Off")).as_deref(),
            Some("on")
        );
        assert_eq!(kimi_model_edges(models), (false, false));
        let top = models.replace("     kimi-think", "   ❯ kimi-think").replace("   ❯ kimi-plain", "     kimi-plain");
        assert_eq!(kimi_model_edges(&top), (true, false));
        let bottom = models.replace("   ❯ kimi-plain", "     kimi-plain").replace("     kimi-levels", "   ❯ kimi-levels");
        assert_eq!(kimi_model_edges(&bottom), (false, true));
        assert!(kimi_picker(&panel.replace("[ High ]", "[ Low ]")).is_none());
    }
}
