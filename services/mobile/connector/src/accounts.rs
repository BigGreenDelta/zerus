//! Bounded public account projections (`accounts.py`): the heartbeat's
//! `mobile_accounts` with its 30 s catalog and one single-flight usage
//! refresh at a time (each account at most every 300 s), and the launch
//! catalog of `launch.catalog`. Provider errors, paths, auth fingerprints and
//! unknown fields never leave the computer.
use crate::pyvalue::{
    bounded, bounded_number, is_true, py_eq, py_isspace, text, PyError, PyResult,
};
use serde_json::{json, Map, Value};
use zerus_relay::json;
use zerus_relay::protocol::opencode_effort_id;

pub const REFRESH_SECONDS: f64 = 300.0;
pub const CATALOG_SECONDS: f64 = 30.0;
pub const MAX_ACCOUNTS: usize = 100;
pub const MAX_CATALOG_BYTES: usize = 128 * 1024;
/// `account ls` budget in the heartbeat; `account inspect` gets 25 s.
pub const CATALOG_TIMEOUT: f64 = 3.0;
pub const INSPECT_TIMEOUT: f64 = 25.0;
pub const PROVIDERS: &[&str] = &["codex", "claude", "kimi", "dsh", "opencode"];
pub const STATUSES: &[&str] = &[
    "loading",
    "ok",
    "unavailable",
    "offline",
    "configured",
    "expired",
    "signed_out",
    "credentials_locked",
    "desktop_session_unavailable",
    "credentials_unavailable",
];

fn member<'a>(value: Option<&'a Value>, choices: &[&str]) -> Option<&'a str> {
    value
        .and_then(Value::as_str)
        .filter(|text| choices.contains(text))
}

fn ascii_digits(text: &str) -> bool {
    !text.is_empty() && text.bytes().all(|b| b.is_ascii_digit())
}

/// `\d{n}` at the start of `text`, returning the rest.
fn digits(text: &str, count: usize) -> Option<&str> {
    let head = text.get(..count)?;
    ascii_digits(head).then(|| &text[count..])
}

/// `accounts.iso_reset`: an RFC 3339 timestamp `datetime.fromisoformat`
/// accepts. Python's `\d` also matches other decimal digits, which
/// `fromisoformat` then refuses, so only ASCII digits pass either way; any
/// other non-ASCII text is refused before the fields are sliced by byte.
fn iso_reset(value: &str) -> bool {
    if !value.is_ascii() {
        return false;
    }
    #[allow(clippy::type_complexity)]
    let parse = || -> Option<(u32, u32, u32, u32, u32, u32, Option<(u32, u32)>)> {
        let number = |text: &str| text.parse::<u32>().ok();
        let year = number(&value[..4.min(value.len())])?;
        let rest = digits(value, 4)?.strip_prefix('-')?;
        let month = number(&rest[..2.min(rest.len())])?;
        let rest = digits(rest, 2)?.strip_prefix('-')?;
        let day = number(&rest[..2.min(rest.len())])?;
        let rest = digits(rest, 2)?.strip_prefix('T')?;
        let hour = number(&rest[..2.min(rest.len())])?;
        let rest = digits(rest, 2)?.strip_prefix(':')?;
        let minute = number(&rest[..2.min(rest.len())])?;
        let rest = digits(rest, 2)?.strip_prefix(':')?;
        let second = number(&rest[..2.min(rest.len())])?;
        let mut rest = digits(rest, 2)?;
        if let Some(fraction) = rest.strip_prefix('.') {
            let length = fraction.bytes().take_while(u8::is_ascii_digit).count();
            if !(1..=9).contains(&length) {
                return None;
            }
            rest = &fraction[length..];
        }
        let offset = if rest == "Z" {
            None
        } else {
            let sign = rest.strip_prefix(['+', '-'])?;
            let hours = number(&sign[..2.min(sign.len())])?;
            let tail = digits(sign, 2)?.strip_prefix(':')?;
            let minutes = number(&tail[..2.min(tail.len())])?;
            if !digits(tail, 2)?.is_empty() {
                return None;
            }
            Some((hours, minutes))
        };
        Some((year, month, day, hour, minute, second, offset))
    };
    let Some((year, month, day, hour, minute, second, offset)) = parse() else {
        return false;
    };
    let leap = year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
    let days = match month {
        1 | 3 | 5 | 7 | 8 | 10 | 12 => 31,
        4 | 6 | 9 | 11 => 30,
        2 if leap => 29,
        2 => 28,
        _ => return false,
    };
    // Offset minutes may exceed 59; only the whole offset must stay within a day.
    (1..=9999).contains(&year)
        && (1..=days).contains(&day)
        && hour < 24
        && minute < 60
        && second < 60
        && offset.is_none_or(|(hours, minutes)| hours * 60 + minutes < 24 * 60)
}

/// `-?\d+(?:\.\d+)?` with at most `whole` and `fraction` digits.
fn decimal(text: &str, whole: Option<usize>, fraction: Option<usize>) -> bool {
    let text = text.strip_prefix('-').unwrap_or(text);
    let (int, frac) = match text.split_once('.') {
        Some((int, frac)) => (int, Some(frac)),
        None => (text, None),
    };
    ascii_digits(int)
        && whole.is_none_or(|maximum| int.len() <= maximum)
        && frac.is_none_or(|frac| {
            ascii_digits(frac) && fraction.is_none_or(|maximum| frac.len() <= maximum)
        })
}

/// Removes `key` from every OpenCode model entry of one account row.
fn strip_model_key(row: &mut Value, key: &str) {
    if let Some(Value::Array(options)) = row.get_mut("model_options") {
        for option in options {
            if let Some(option) = option.as_object_mut() {
                option.remove(key);
            }
        }
    }
}

/// One degradation step over every row: 0 drops `effort_labels`, 1
/// `effort_options`, 2 OpenCode `providers` (marking `providers_truncated`).
/// Least valuable first: a projection over the bound loses these in order
/// before it is refused.
const DEGRADE_STEPS: usize = 3;
fn degrade(row: &mut Value, step: usize) {
    match step {
        0 => strip_model_key(row, "effort_labels"),
        1 => strip_model_key(row, "effort_options"),
        _ => {
            if let Some(object) = row.as_object_mut() {
                if object.remove("providers").is_some() {
                    object.insert("providers_truncated".into(), json!(true));
                }
            }
        }
    }
}

const KINDS: [&str; 8] = [
    "free", "key", "oauth", "env", "config", "command", "external", "local",
];
const MAX_PROVIDERS: usize = 64;

/// The provider part of an OpenCode model value and an OpenCode provider ID.
fn provider_identity(provider: &str) -> bool {
    provider
        .bytes()
        .next()
        .is_some_and(|b| b.is_ascii_alphanumeric() || b == b'_')
        && provider.len() <= 80
        && provider
            .bytes()
            .all(|b| b.is_ascii_alphanumeric() || b"_.-".contains(&b))
}

fn count(value: Option<&Value>, maximum: u64) -> Option<u64> {
    value.and_then(Value::as_u64).filter(|n| *n <= maximum)
}

/// OpenCode providers for Accounts: identity, kind and counts only. Heartbeat
/// only; the launch catalog never carries them.
pub fn opencode_providers(profile: &Map<String, Value>, public: &mut Map<String, Value>) {
    let Some(Value::Array(rows)) = profile.get("providers") else {
        return;
    };
    let mut listed = Vec::new();
    let mut truncated = matches!(profile.get("providers_truncated"), Some(Value::Bool(true)));
    for row in rows.iter().take(1024) {
        let Some(row) = row.as_object() else {
            continue;
        };
        let Some(id) = row
            .get("id")
            .and_then(Value::as_str)
            .filter(|id| provider_identity(id))
        else {
            continue;
        };
        if listed.len() == MAX_PROVIDERS {
            truncated = true;
            break;
        }
        let mut entry = Map::new();
        entry.insert("id".into(), json!(id));
        let label = text(row.get("label"), 160)
            .filter(|label| !label.is_empty())
            .unwrap_or_else(|| id.into());
        entry.insert("label".into(), json!(label));
        if let Some(kind) = member(row.get("kind"), &KINDS) {
            entry.insert("kind".into(), json!(kind));
        }
        if let Some(n) = count(row.get("credential_count"), 64) {
            entry.insert("credential_count".into(), json!(n));
        }
        if let Some(n) = count(row.get("model_count"), 10_000) {
            entry.insert("model_count".into(), json!(n));
        }
        if let Some(label) = text(row.get("active_label"), 80).filter(|label| !label.is_empty()) {
            entry.insert("active_label".into(), json!(label));
        }
        listed.push(Value::Object(entry));
    }
    public.insert("providers".into(), Value::Array(listed));
    public.insert("providers_truncated".into(), json!(truncated));
}

/// Drops the optional model details, one kind at a time, until the document
/// built from `rows` fits the bound. Returns whether it fits.
fn fit_rows(rows: &mut [Value], document: impl Fn(&[Value]) -> Value) -> bool {
    let fits = |rows: &[Value]| {
        json::python_default_len(&document(rows), true).unwrap_or(usize::MAX) <= MAX_CATALOG_BYTES
    };
    if fits(rows) {
        return true;
    }
    for step in 0..DEGRADE_STEPS {
        for row in rows.iter_mut() {
            degrade(row, step);
        }
        if fits(rows) {
            return true;
        }
    }
    false
}

/// Variants one model offers: at most 8 native effort ids and, in the same
/// order, their labels. Anything malformed drops the entry, never the model.
fn effort_options(row: &Map<String, Value>) -> Option<(Value, Value)> {
    let efforts = row.get("effort_options")?.as_array()?;
    let labels = row.get("effort_labels").and_then(Value::as_array);
    let mut ids: Vec<Value> = Vec::new();
    let mut shown: Vec<Value> = Vec::new();
    for (index, effort) in efforts.iter().enumerate() {
        let Some(id) = effort.as_str().filter(|id| opencode_effort_id(id)) else {
            continue;
        };
        if ids.iter().any(|known| known == id) {
            continue;
        }
        if ids.len() == 8 {
            break;
        }
        let label = labels
            .and_then(|labels| labels.get(index))
            .and_then(|label| text(Some(label), 40))
            .filter(|label| !label.is_empty())
            .unwrap_or_else(|| id.into());
        ids.push(json!(id));
        shown.push(json!(label));
    }
    (!ids.is_empty()).then_some((Value::Array(ids), Value::Array(shown)))
}

/// OpenCode catalog state and reasons, bounded and only when present.
fn opencode_status(profile: &Map<String, Value>, public: &mut Map<String, Value>) {
    if let Some(state) = member(
        profile.get("catalog_state"),
        &[
            "ready",
            "loading",
            "not_running",
            "disabled",
            "incompatible",
            "unreachable",
        ],
    ) {
        public.insert("catalog_state".into(), json!(state));
    }
    for key in ["catalog_reason", "install_reason"] {
        if let Some(reason) = text(profile.get(key), 200).filter(|reason| !reason.is_empty()) {
            public.insert(key.into(), json!(reason));
        }
    }
    if let Some(at) = profile
        .get("catalog_at")
        .filter(|value| bounded_number(value))
    {
        public.insert("catalog_at".into(), at.clone());
    }
    if let Some(Value::Bool(truncated)) = profile.get("model_options_truncated") {
        public.insert("model_options_truncated".into(), json!(truncated));
    }
}

/// `accounts.model_options(profile)`: configured IDs and labels only.
pub fn model_options(profile: &Map<String, Value>) -> Value {
    let rows = match profile.get("model_options") {
        Some(Value::Array(rows)) => rows.as_slice(),
        _ => &[],
    };
    let mut result = Vec::new();
    let mut seen: Vec<String> = Vec::new();
    for row in rows.iter().take(256) {
        let Some(row) = row.as_object() else {
            continue;
        };
        let Some(value) = row.get("value").and_then(Value::as_str) else {
            continue;
        };
        if value.len() > 256
            || value
                .chars()
                .any(|c| py_isspace(c) || (c as u32) < 32 || c as u32 == 127)
        {
            continue;
        }
        let Some((provider, model)) = value.split_once('/') else {
            continue;
        };
        if model.is_empty()
            || !provider_identity(provider)
            || seen.iter().any(|known| known == value)
        {
            continue;
        }
        seen.push(value.into());
        let label = text(row.get("label"), 160)
            .filter(|label| !label.is_empty())
            .unwrap_or_else(|| value.into());
        let mut option = Map::new();
        option.insert("value".into(), json!(value));
        option.insert("label".into(), json!(label));
        option.insert("provider".into(), json!(provider));
        if let Some(label) = text(row.get("provider_label"), 160).filter(|label| !label.is_empty())
        {
            option.insert("provider_label".into(), json!(label));
        }
        if let Some(Value::Bool(free)) = row.get("free") {
            option.insert("free".into(), json!(free));
        }
        for mark in ["favorite", "default"] {
            if let Some(Value::Bool(true)) = row.get(mark) {
                option.insert(mark.into(), json!(true));
            }
        }
        if let Some((efforts, labels)) = effort_options(row) {
            option.insert("effort_options".into(), efforts);
            option.insert("effort_labels".into(), labels);
        }
        result.push(Value::Object(option));
    }
    Value::Array(result)
}

/// `accounts.usage(raw)`: never native errors, paths or arbitrary fields.
pub fn usage(raw: &Value) -> Value {
    let empty = Map::new();
    let raw = raw.as_object().unwrap_or(&empty);
    let status = member(raw.get("status"), STATUSES).unwrap_or("unavailable");
    let mut identity = Map::new();
    if let Some(Value::Object(source)) = raw.get("identity") {
        for field in [
            "name",
            "email",
            "organization",
            "plan",
            "auth_method",
            "account_id",
        ] {
            if let Some(value) = text(source.get(field), 256).filter(|value| !value.is_empty()) {
                identity.insert(field.into(), json!(value));
            }
        }
    }
    let mut result = Map::new();
    result.insert("status".into(), json!(status));
    result.insert("identity".into(), Value::Object(identity));
    result.insert("windows".into(), json!([]));
    result.insert("refreshing".into(), json!(false));
    result.insert("refresh_error".into(), json!(false));
    if let Some(auth) = member(
        raw.get("auth_status"),
        &["signed_in", "signed_out", "unknown"],
    ) {
        result.insert("auth_status".into(), json!(auth));
    }
    if let Some(source) = member(
        raw.get("source"),
        &[
            "Codex App Server",
            "Claude Code",
            "Kimi Code",
            "DeepSeek Harness",
            "OpenCode",
        ],
    ) {
        result.insert("source".into(), json!(source));
    }
    if let Some(checked) = raw.get("checked_at").filter(|value| bounded_number(value)) {
        result.insert("checked_at".into(), checked.clone());
    }
    if let Some(cached @ Value::Bool(_)) = raw.get("identity_cached") {
        result.insert("identity_cached".into(), cached.clone());
    }
    let mut windows = Vec::new();
    if let Some(Value::Array(rows)) = raw.get("windows") {
        for window in rows.iter().take(16) {
            let Some(window) = window.as_object() else {
                continue;
            };
            let Some(used) = window
                .get("used_percent")
                .filter(|value| bounded_number(value))
            else {
                continue;
            };
            let mut row = Map::new();
            row.insert("used_percent".into(), used.clone());
            for key in ["id", "label"] {
                if let Some(value) = text(window.get(key), 160) {
                    row.insert(key.into(), json!(value));
                }
            }
            if let Some(minutes) = window
                .get("window_minutes")
                .filter(|value| bounded_number(value))
            {
                row.insert("window_minutes".into(), minutes.clone());
            }
            match window.get("resets_at") {
                Some(reset) if bounded_number(reset) => {
                    row.insert("resets_at".into(), reset.clone());
                }
                Some(Value::String(reset)) if iso_reset(reset) => {
                    row.insert("resets_at".into(), json!(reset));
                }
                _ => {}
            }
            windows.push(Value::Object(row));
        }
    }
    result.insert("windows".into(), Value::Array(windows));
    if let Some(Value::Object(credits)) = raw.get("credits") {
        let mut projected = Map::new();
        if let Some(balance) = text(credits.get("balance"), 80) {
            if decimal(&balance, None, None) {
                projected.insert("balance".into(), json!(balance));
            }
        }
        if let Some(unlimited @ Value::Bool(_)) = credits.get("unlimited") {
            projected.insert("unlimited".into(), unlimited.clone());
        }
        if !projected.is_empty() {
            result.insert("credits".into(), Value::Object(projected));
        }
    }
    let mut balances = Vec::new();
    if let Some(Value::Array(rows)) = raw.get("balances") {
        for balance in rows.iter().take(16) {
            let Some(balance) = balance.as_object() else {
                continue;
            };
            let currency = member(balance.get("currency"), &["USD", "CNY"]);
            let amount = balance.get("balance").and_then(Value::as_str);
            if let (Some(currency), Some(amount)) = (currency, amount) {
                if decimal(amount, Some(40), Some(20)) {
                    let kind = text(balance.get("kind"), 80)
                        .filter(|kind| !kind.is_empty())
                        .unwrap_or_else(|| "Wallet".into());
                    balances.push(json!({"balance": amount, "currency": currency, "kind": kind}));
                }
            }
        }
    }
    if !balances.is_empty() {
        result.insert("balances".into(), Value::Array(balances));
    }
    Value::Object(result)
}

fn identity_valid(identity: &str) -> bool {
    let bytes = identity.as_bytes();
    !bytes.is_empty()
        && bytes.len() <= 80
        && (bytes[0].is_ascii_alphanumeric() || bytes[0] == b'_')
        && bytes
            .iter()
            .all(|b| b.is_ascii_alphanumeric() || *b == b'_' || *b == b'-')
}

/// One account of the cache.
#[derive(Debug, Clone)]
pub struct Entry {
    pub public: Map<String, Value>,
    /// Provider, auth revision, home and credential file: a changed one
    /// discards cached usage.
    pub revision: [Value; 4],
    pub usage: Value,
    pub attempted: Option<f64>,
    /// Python compared entries by object identity.
    pub generation: u64,
}

fn same_revision(a: &[Value; 4], b: &[Value; 4]) -> bool {
    a.iter().zip(b).all(|(a, b)| py_eq(a, b))
}

/// `accounts.catalog(raw)`, in Python's dict order.
pub fn catalog(raw: &Value) -> PyResult<Vec<(String, Entry)>> {
    let profiles = match raw.get("profiles") {
        Some(Value::Array(profiles)) if raw.is_object() && profiles.len() <= MAX_ACCOUNTS => {
            profiles
        }
        _ => return Err(PyError::value("native account catalog unavailable")),
    };
    let mut entries: Vec<(String, Entry)> = Vec::new();
    for profile in profiles {
        let Some(profile) = profile.as_object() else {
            return Err(PyError::value("invalid account row"));
        };
        let identity = profile
            .get("id")
            .and_then(Value::as_str)
            .unwrap_or_default();
        let provider = member(profile.get("provider"), PROVIDERS);
        let (true, Some(provider)) = (identity_valid(identity), provider) else {
            return Err(PyError::value("invalid account identity"));
        };
        if entries.iter().any(|(known, _)| known == identity) {
            return Err(PyError::value("invalid account identity"));
        }
        let mut public = Map::new();
        public.insert("id".into(), json!(identity));
        public.insert("provider".into(), json!(provider));
        let label = text(profile.get("label"), 160)
            .filter(|label| !label.is_empty())
            .unwrap_or_else(|| identity.into());
        public.insert("label".into(), json!(label));
        for key in ["native", "installed", "is_default"] {
            public.insert(key.into(), json!(is_true(profile.get(key))));
        }
        if provider == "opencode" {
            public.insert("model_options".into(), model_options(profile));
            opencode_status(profile, &mut public);
            opencode_providers(profile, &mut public);
        }
        let seed = usage(profile.get("account_status").unwrap_or(&Value::Null));
        let auth_revision = match profile.get("auth_revision") {
            Some(Value::String(revision)) if !revision.is_empty() => json!(revision),
            // DSH omits auth_revision: its catalog identity stands in, so a
            // sign-out or replacement never carries old usage across.
            _ => {
                let mut fields = Map::new();
                for key in ["identity", "status", "auth_status"] {
                    fields.insert(key.into(), seed.get(key).cloned().unwrap_or(Value::Null));
                }
                json!(json::canonical(&Value::Object(fields)).unwrap_or_default())
            }
        };
        let revision = [
            json!(provider),
            auth_revision,
            profile.get("home").cloned().unwrap_or(Value::Null),
            profile
                .get("credential_file")
                .cloned()
                .unwrap_or(Value::Null),
        ];
        entries.push((
            identity.into(),
            Entry {
                public,
                revision,
                usage: seed,
                attempted: None,
                generation: 0,
            },
        ));
    }
    let projected = |entries: &[(String, Entry)]| -> Vec<Value> {
        entries
            .iter()
            .map(|(_, entry)| {
                let mut row = entry.public.clone();
                row.insert("usage".into(), entry.usage.clone());
                Value::Object(row)
            })
            .collect()
    };
    let size = |rows: &[Value]| {
        json::python_default_len(&Value::Array(rows.to_vec()), true).unwrap_or(usize::MAX)
    };
    let mut rows = projected(&entries);
    if size(&rows) > MAX_CATALOG_BYTES {
        for step in 0..DEGRADE_STEPS {
            for (_, entry) in &mut entries {
                let mut model_options = Value::Object(std::mem::take(&mut entry.public));
                degrade(&mut model_options, step);
                if let Value::Object(public) = model_options {
                    entry.public = public;
                }
            }
            rows = projected(&entries);
            if size(&rows) <= MAX_CATALOG_BYTES {
                break;
            }
        }
    }
    if size(&rows) > MAX_CATALOG_BYTES {
        return Err(PyError::value(
            "native account projection exceeds size limit",
        ));
    }
    Ok(entries)
}

/// The account cache (`AccountCache`); the runtime drives its native calls.
#[derive(Debug, Default)]
pub struct AccountCache {
    pub entries: Vec<(String, Entry)>,
    pub next_catalog: f64,
    pub checked_at: Option<f64>,
    pub available: bool,
    pub stale: bool,
    pub cursor: usize,
    /// A usage refresh is running.
    pub refreshing: bool,
    generations: u64,
}

/// A usage refresh the cache started: `account inspect <id>`.
#[derive(Debug, Clone, PartialEq)]
pub struct Refresh {
    pub identity: String,
    pub provider: String,
    pub generation: u64,
}

impl AccountCache {
    pub fn catalog_due(&self, clock: f64) -> bool {
        clock >= self.next_catalog
    }

    /// The outcome of `account ls`, read when [`Self::catalog_due`].
    pub fn apply_catalog(&mut self, fresh: PyResult<Vec<(String, Entry)>>, wall: f64) {
        match fresh {
            Ok(fresh) => {
                let mut entries = Vec::new();
                for (identity, mut entry) in fresh {
                    let previous = self
                        .entries
                        .iter()
                        .find(|(known, _)| *known == identity)
                        .map(|(_, previous)| previous);
                    match previous {
                        Some(previous) if same_revision(&previous.revision, &entry.revision) => {
                            let mut kept = previous.clone();
                            kept.public = entry.public;
                            entry = kept;
                        }
                        _ => {
                            self.generations += 1;
                            entry.generation = self.generations;
                        }
                    }
                    entries.push((identity, entry));
                }
                self.entries = entries;
                self.available = true;
                self.stale = false;
                self.checked_at = Some(wall);
            }
            Err(_) => self.stale = true,
        }
    }

    /// The next account whose usage is due, if no refresh runs, marked as
    /// refreshing (`update()` after its catalog step).
    pub fn next_refresh(&mut self, clock: f64) -> Option<Refresh> {
        if !self.available || self.stale || self.refreshing {
            return None;
        }
        let installed: Vec<usize> = (0..self.entries.len())
            .filter(|index| is_true(self.entries[*index].1.public.get("installed")))
            .collect();
        for offset in 0..installed.len() {
            let index = (self.cursor + offset) % installed.len();
            let (identity, entry) = &mut self.entries[installed[index]];
            if entry
                .attempted
                .is_none_or(|attempted| clock - attempted >= REFRESH_SECONDS)
            {
                entry.attempted = Some(clock);
                if let Value::Object(usage) = &mut entry.usage {
                    usage.insert("refreshing".into(), json!(true));
                }
                let refresh = Refresh {
                    identity: identity.clone(),
                    provider: entry.public["provider"].as_str().unwrap_or_default().into(),
                    generation: entry.generation,
                };
                self.cursor = (index + 1) % installed.len();
                self.refreshing = true;
                return Some(refresh);
            }
        }
        None
    }

    /// The outcome of a refresh: `account inspect` output, or `None` when
    /// the native call failed.
    pub fn finish_refresh(&mut self, refresh: &Refresh, raw: Option<&Value>) {
        self.refreshing = false;
        let Some((_, entry)) = self.entries.iter_mut().find(|(identity, entry)| {
            *identity == refresh.identity && entry.generation == refresh.generation
        }) else {
            return;
        };
        let projected = raw.and_then(|raw| {
            let object = raw.as_object()?;
            if object.get("id").and_then(Value::as_str) != Some(refresh.identity.as_str())
                || object.get("provider").and_then(Value::as_str) != Some(refresh.provider.as_str())
            {
                return None;
            }
            let projected = usage(raw);
            // A provider outage preserves the last values, with explicit failure.
            if projected["status"] == "unavailable"
                && entry
                    .usage
                    .get("checked_at")
                    .is_some_and(|checked| !checked.is_null())
            {
                return None;
            }
            Some(projected)
        });
        match projected {
            Some(projected) => entry.usage = projected,
            None => {
                if let Value::Object(usage) = &mut entry.usage {
                    usage.insert("refreshing".into(), json!(false));
                    usage.insert("refresh_error".into(), json!(true));
                }
            }
        }
    }

    /// `AccountCache.view()` at wall clock `wall`.
    pub fn view(&self, wall: f64) -> Value {
        let mut rows = Vec::new();
        for (_, entry) in &self.entries {
            let mut data = entry.usage.clone();
            let observed = data.get("checked_at").and_then(|value| match value {
                Value::Number(number) => crate::pyvalue::PyNumber::of(number).as_f64(),
                _ => None,
            });
            let refresh_error =
                crate::pyvalue::truthy(data.get("refresh_error").unwrap_or(&Value::Null));
            let stale = self.stale
                || refresh_error
                || observed.is_none_or(|observed| {
                    wall - observed >= REFRESH_SECONDS || observed > wall + 60.0
                });
            if let Value::Object(data) = &mut data {
                data.insert("stale".into(), json!(stale));
            }
            let mut row = entry.public.clone();
            row.insert("usage".into(), data);
            rows.push(Value::Object(row));
        }
        let document = |rows: &[Value]| {
            let mut result = json!({"schema": 1, "available": self.available,
                                    "stale": self.stale,
                                    "refresh_interval_seconds": 300, "accounts": rows});
            if let Some(checked) = self.checked_at {
                result["checked_at"] = number(checked);
            }
            result
        };
        if !fit_rows(&mut rows, document) {
            return json!({"schema": 1, "available": false, "stale": true,
                          "refresh_interval_seconds": 300, "accounts": []});
        }
        document(&rows)
    }
}

/// A clock reading as Python's `float` would print it.
pub fn number(value: f64) -> Value {
    serde_json::Number::from_f64(value).map_or(Value::Null, Value::Number)
}

/// `launch.catalog(raw)`: providers and accounts offered for a new session.
pub fn launch_catalog(raw: &Value) -> PyResult<Value> {
    let profiles = match raw.get("profiles") {
        Some(Value::Array(profiles)) if raw.is_object() && profiles.len() <= 1000 => profiles,
        _ => {
            return Err(PyError::value(
                "native account catalog is invalid or too large",
            ))
        }
    };
    let mut accounts = Vec::new();
    let mut agents: Vec<String> = Vec::new();
    for row in profiles {
        let Some(row) = row.as_object() else {
            continue;
        };
        let Some(provider) = member(row.get("provider"), PROVIDERS) else {
            continue;
        };
        let mut account = Map::new();
        for (key, maximum) in [("id", 80), ("provider", 80), ("label", 160)] {
            account.insert(key.into(), json!(bounded(row.get(key), maximum, false)?));
        }
        for key in ["native", "is_default", "installed"] {
            account.insert(key.into(), json!(is_true(row.get(key))));
        }
        let empty = Map::new();
        let cached = match row.get("account_status") {
            Some(Value::Object(cached)) => cached,
            _ => &empty,
        };
        let mut status = cached.get("auth_status");
        if member(status, &["signed_in", "signed_out"]).is_none() {
            status = cached.get("status");
        }
        let status = member(
            status,
            &[
                "signed_in",
                "signed_out",
                "expired",
                "credentials_locked",
                "desktop_session_unavailable",
                "credentials_unavailable",
            ],
        )
        .unwrap_or("unknown");
        account.insert("auth_status".into(), json!(status));
        if provider == "opencode" {
            account.insert("model_options".into(), model_options(row));
            opencode_status(row, &mut account);
        }
        if !cached.is_empty() {
            account.insert("usage".into(), usage(&Value::Object(cached.clone())));
        }
        accounts.push(Value::Object(account));
        if is_true(row.get("installed")) && !agents.iter().any(|known| known == provider) {
            agents.push(provider.into());
        }
    }
    agents.sort();
    let document = |accounts: &[Value]| json!({"accounts": accounts, "agents": agents});
    if !fit_rows(&mut accounts, document) {
        return Err(PyError::value(
            "native launch account projection exceeds size limit",
        ));
    }
    Ok(document(&accounts))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn reset_times_follow_fromisoformat() {
        for valid in [
            "2025-10-09T12:00:00Z",
            "2024-02-29T23:59:59.123456789+05:30",
            "2025-01-01T00:00:00-23:59",
            "2025-01-01T00:00:00+05:99",
        ] {
            assert!(iso_reset(valid), "{valid}");
        }
        for invalid in [
            "2025-02-29T00:00:00Z",
            "2025-10-09T24:00:00Z",
            "2025-10-09 12:00:00Z",
            "2025-10-09T12:00:00",
            "2025-10-09T12:00:00.Z",
            "2025-10-09T12:00:00+24:00",
            "2025-10-09T12:00:00+23:60",
            "0000-01-01T00:00:00Z",
            "2025-10-09T12:00:00.1234567890Z",
            // Multi-byte text where a field is cut by byte offset.
            "\u{20ac}\u{20ac}\u{20ac}\u{20ac}-01-01T00:00:00Z",
            "2025-\u{20ac}0-01T00:00:00Z",
            "2025-01-01T00:00:00+\u{20ac}0:00",
            "\u{663}\u{660}\u{662}\u{665}-10-09T12:00:00Z",
        ] {
            assert!(!iso_reset(invalid), "{invalid}");
        }
    }

    #[test]
    fn decimals_follow_the_python_patterns() {
        assert!(decimal("-1.25", Some(40), Some(20)));
        assert!(!decimal("1e3", None, None));
        assert!(!decimal("1.", None, None));
        assert!(!decimal(".5", None, None));
        assert!(!decimal(&"1".repeat(41), Some(40), Some(20)));
    }
}
