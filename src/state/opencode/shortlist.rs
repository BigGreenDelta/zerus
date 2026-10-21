//! The OpenCode short list New session and model change show first, and the
//! 32 KiB budget of the account row, measured as the mobile connector
//! measures its projection (`json.dumps(row, ensure_ascii=True)`).
use super::super::*;
use std::collections::{HashMap, HashSet};

pub(crate) const ROW_BUDGET: usize = 32 * 1024;
pub(super) const MAX_SHORT: usize = 256;
pub(super) const KEEP_PROVIDERS: usize = 16;

fn str_len(text: &str) -> usize {
    2 + text
        .chars()
        .map(|c| match c {
            '"' | '\\' | '\n' | '\r' | '\t' | '\u{8}' | '\u{c}' => 2,
            ' '..='~' => 1,
            c => 6 * c.len_utf16(),
        })
        .sum::<usize>()
}

/// A number as Python's `json.dumps` spells it, as the connector's
/// `zerus_relay::json` does: an integer as it is, a float as Python's `repr`,
/// positional from 1e-4 up to 1e16 and else like `1e-07` or `1.5e+300`.
pub(super) fn python_number(number: &serde_json::Number) -> String {
    let raw = number.to_string();
    if !raw.contains(['.', 'e', 'E']) {
        return if raw == "-0" { "0".into() } else { raw };
    }
    // serde_json writes the shortest digits that round-trip, as Python does.
    let (sign, unsigned) = match raw.strip_prefix('-') {
        Some(rest) => ("-", rest),
        None => ("", raw.as_str()),
    };
    let (mantissa, exponent) = match unsigned.split_once(['e', 'E']) {
        Some((m, e)) => (m, e.parse::<i32>().unwrap_or(0)),
        None => (unsigned, 0),
    };
    let point = mantissa.find('.').unwrap_or(mantissa.len()) as i32;
    let all = mantissa.replace('.', "");
    let first = all.find(|c| c != '0').unwrap_or(all.len());
    let digits = all[first..].trim_end_matches('0');
    if digits.is_empty() {
        return format!("{sign}0.0");
    }
    let power = exponent + point - first as i32 - 1;
    if !(-4..16).contains(&power) {
        let (lead, rest) = digits.split_at(1);
        let rest = if rest.is_empty() {
            String::new()
        } else {
            format!(".{rest}")
        };
        let mark = if power < 0 { '-' } else { '+' };
        return format!("{sign}{lead}{rest}e{mark}{:02}", power.abs());
    }
    let position = power + 1;
    if position <= 0 {
        format!(
            "{sign}0.{}{digits}",
            "0".repeat(position.unsigned_abs() as usize)
        )
    } else if position as usize >= digits.len() {
        let zeros = "0".repeat(position as usize - digits.len());
        format!("{sign}{digits}{zeros}.0")
    } else {
        let (whole, fraction) = digits.split_at(position as usize);
        format!("{sign}{whole}.{fraction}")
    }
}

/// `len(json.dumps(value, ensure_ascii=True))`, as the connector's
/// `zerus_relay::json::python_default_len(value, true)`; the shared fixture's
/// `ascii_lengths` pin both.
pub(crate) fn ascii_len(value: &Value) -> usize {
    match value {
        Value::Null | Value::Bool(true) => 4,
        Value::Bool(false) => 5,
        Value::Number(n) => python_number(n).len(),
        Value::String(s) => str_len(s),
        Value::Array(items) => {
            2 + items.iter().map(ascii_len).sum::<usize>() + 2 * items.len().saturating_sub(1)
        }
        Value::Object(map) => {
            2 + map
                .iter()
                .map(|(k, v)| str_len(k) + 2 + ascii_len(v))
                .sum::<usize>()
                + 2 * map.len().saturating_sub(1)
        }
    }
}

/// Default model, pinned models (a session's current and pending model),
/// favorites (stored order), configured models, free models (service order);
/// each once, at most `limit`; `true` when cut.
pub(super) fn ordered(
    models: &[Value],
    default: &str,
    pinned: &[&str],
    favorites: &[String],
    configured: &HashSet<String>,
    limit: usize,
) -> (Vec<Value>, bool) {
    let by_value: HashMap<&str, &Value> = models
        .iter()
        .filter_map(|m| Some((m["value"].as_str()?, m)))
        .collect();
    let candidates = by_value
        .get(default)
        .copied()
        .into_iter()
        .chain(pinned.iter().filter_map(|p| by_value.get(p).copied()))
        .chain(
            favorites
                .iter()
                .filter_map(|f| by_value.get(f.as_str()).copied()),
        )
        .chain(models.iter().filter(|m| {
            m["provider"]
                .as_str()
                .is_some_and(|p| configured.contains(p))
        }))
        .chain(models.iter().filter(|m| m["free"] == true));
    let mut seen = HashSet::new();
    let mut rows = Vec::new();
    for model in candidates {
        let Some(value) = model["value"].as_str() else {
            continue;
        };
        if !seen.insert(value) {
            continue;
        }
        if rows.len() == limit {
            return (rows, true);
        }
        let mut row = model.clone();
        if favorites.iter().any(|f| f == value) {
            row["favorite"] = json!(true);
        }
        if value == default {
            row["default"] = json!(true);
        }
        rows.push(row);
    }
    (rows, false)
}

pub(super) fn build(
    models: &[Value],
    default: &str,
    pinned: &[&str],
    favorites: &[String],
    configured: &HashSet<String>,
) -> (Vec<Value>, bool) {
    ordered(models, default, pinned, favorites, configured, MAX_SHORT)
}

/// Keys a damaged row keeps: its identity, both lists and both flags.
const KEPT: [&str; 6] = [
    "id",
    "provider",
    "model_options",
    "model_options_truncated",
    "providers",
    "providers_truncated",
];

/// Keeps the account `row` within `budget`: short-list entries go from the
/// end (free, then configured, then favorites; a default model first in the
/// list stays), then providers beyond the first [`KEEP_PROVIDERS`], then any
/// other provider. A row still over budget is damaged ([`fit_damaged`]). A
/// row within budget is unchanged; otherwise both flags become booleans, and
/// a list that lost entries sets its flag.
pub(crate) fn fit_row(row: &mut Value, budget: usize) {
    if !row.is_object() || ascii_len(row) <= budget {
        return;
    }
    for flag in ["model_options_truncated", "providers_truncated"] {
        row[flag] = json!(row[flag] == true);
    }
    let mut size = ascii_len(row);
    let keep = usize::from(row["model_options"][0]["default"] == true);
    for (key, flag, floor) in [
        ("model_options", "model_options_truncated", keep),
        ("providers", "providers_truncated", KEEP_PROVIDERS),
        ("providers", "providers_truncated", 0),
    ] {
        while size > budget {
            let Some(items) = row
                .get_mut(key)
                .and_then(Value::as_array_mut)
                .filter(|items| items.len() > floor)
            else {
                break;
            };
            let last = items.pop().unwrap_or(Value::Null);
            size -= ascii_len(&last) + if items.is_empty() { 0 } else { 2 };
            if row[flag] == false {
                // `true` is one byte shorter than `false`.
                row[flag] = json!(true);
                size -= 1;
            }
        }
    }
    if size > budget {
        fit_damaged(row, budget);
    }
}

/// The last resort for a row still over budget, which only damaged input
/// makes: the default entry and other non-scalar fields go, largest first,
/// then other fields. [`KEPT`] stays, so any row whose identity fits ends
/// within budget.
fn fit_damaged(row: &mut Value, budget: usize) {
    for (key, flag) in [
        ("model_options", "model_options_truncated"),
        ("providers", "providers_truncated"),
    ] {
        if !row[key].is_array() {
            row[key] = json!([]);
            row[flag] = json!(true);
        }
    }
    while ascii_len(row) > budget {
        let Some(map) = row.as_object_mut() else {
            return;
        };
        let default = map
            .get("model_options")
            .and_then(Value::as_array)
            .and_then(|options| options.first())
            .map_or(0, ascii_len);
        let largest = |scalar: bool| {
            map.iter()
                .filter(|(key, value)| {
                    !KEPT.contains(&key.as_str())
                        && (value.is_array() || value.is_object()) != scalar
                })
                .map(|(key, value)| (ascii_len(value), key.clone()))
                .max()
        };
        match largest(false) {
            Some((size, key)) if size >= default => {
                map.remove(&key);
            }
            _ if default > 0 => {
                map.insert("model_options".into(), json!([]));
                map.insert("model_options_truncated".into(), json!(true));
            }
            _ => match largest(true) {
                Some((_, key)) => {
                    map.remove(&key);
                }
                None => return,
            },
        }
    }
}

/// Every secret-shaped string of the shared fixture's raw service answers:
/// keys, base URLs, credential IDs, environment variable names, a connection
/// status, the `opencode` provider's key value and the key names around them.
#[cfg(test)]
pub(super) const SECRETS: [&str; 17] = [
    "fixture-secret-key",
    "Bearer",
    "apiKey",
    "Authorization",
    "headers",
    "baseURL",
    "\"settings\"",
    "\"body\"",
    "\"token\"",
    "127.0.0.1",
    "example.invalid",
    "cred_fixture",
    "GROQ_API_KEY",
    "ANTHROPIC_API_KEY",
    "needs_auth",
    "\"public\"",
    "Sign in again",
];

#[cfg(test)]
mod tests {
    use super::super::catalog::{row_from, snapshot_from};
    use super::*;

    fn shared() -> Value {
        serde_json::from_slice(include_bytes!(
            "../../../tests/fixtures/opencode-provider-catalog.json"
        ))
        .unwrap()
    }

    fn model(value: &str, free: bool, label: &str) -> Value {
        let provider = value.split('/').next().unwrap();
        json!({"id": value, "value": value, "label": label, "provider": provider, "provider_label": provider,
               "free": free, "effort_options": ["low"], "effort_labels": ["low"]})
    }

    #[test]
    fn ascii_len_matches_the_connector_measure() {
        for case in shared()["ascii_lengths"].as_array().unwrap() {
            assert_eq!(
                ascii_len(&case["value"]),
                case["length"].as_u64().unwrap() as usize,
                "{}",
                case["value"]
            );
        }
    }

    #[test]
    fn the_shared_row_orders_default_favorites_configured_free() {
        let fixture = shared();
        let service = &fixture["service"];
        let snapshot = snapshot_from(
            &service["provider"],
            &service["model"],
            &service["default"],
            Some(&service["integration"]),
        );
        let favorites: Vec<String> = serde_json::from_value(fixture["favorites"].clone()).unwrap();
        let row = row_from(&snapshot, &favorites, &[]);
        for key in [
            "model_options",
            "providers",
            "settings_model",
            "model_options_truncated",
            "providers_truncated",
        ] {
            assert_eq!(row[key], fixture["row"][key], "{key}");
        }
    }

    #[test]
    fn a_pinned_model_follows_the_default_once_with_its_efforts() {
        let fixture = shared();
        let service = &fixture["service"];
        let snapshot = snapshot_from(
            &service["provider"],
            &service["model"],
            &service["default"],
            Some(&service["integration"]),
        );
        // A paid, unstarred, key-connected model that is a session's current model.
        let row = row_from(
            &snapshot,
            &[],
            &["groq/openai/gpt-oss-20b", "fixture/local", "gone/missing"],
        );
        let options = row["model_options"].as_array().unwrap();
        let values: Vec<&str> = options
            .iter()
            .map(|o| o["value"].as_str().unwrap())
            .collect();
        assert_eq!(&values[..2], ["fixture/local", "groq/openai/gpt-oss-20b"]);
        assert_eq!(
            options[1]["effort_options"],
            json!(["low", "medium", "high"])
        );
        assert!(options[1].get("favorite").is_none());
        assert_eq!(values.iter().filter(|v| **v == "fixture/local").count(), 1);
        assert!(!values.contains(&"gone/missing"));
    }

    #[test]
    fn the_short_list_is_bounded_and_cuts_free_models_first() {
        let mut models = vec![model("cfg/m000", false, "Default")];
        models.extend((0..64).map(|i| model(&format!("paid/f{i:03}"), false, "Favorite")));
        models.extend((1..150).map(|i| model(&format!("cfg/m{i:03}"), false, "Configured")));
        models.extend((0..150).map(|i| model(&format!("zen/z{i:03}"), true, "Free")));
        let favorites: Vec<String> = (0..64).map(|i| format!("paid/f{i:03}")).collect();
        let configured: HashSet<String> = ["cfg".to_owned()].into();
        let (rows, truncated) = build(&models, "cfg/m000", &[], &favorites, &configured);
        assert!(truncated);
        assert_eq!(rows.len(), MAX_SHORT);
        assert_eq!(
            (rows[0]["value"].as_str(), rows[0]["default"].as_bool()),
            (Some("cfg/m000"), Some(true))
        );
        assert!(rows[1..65].iter().all(|r| r["favorite"] == true));
        assert!(rows[65..214]
            .iter()
            .all(|r| r["provider"] == "cfg" && r.get("favorite").is_none()));
        assert_eq!(rows[214..].len(), 42);
        assert!(rows[214..].iter().all(|r| r["free"] == true));
        // A favorite that is not listed is skipped, nothing repeats, and a paid model
        // that is neither default, pinned, favorite nor configured stays out.
        let (rows, cut) = build(
            &models[..3],
            "cfg/m000",
            &[],
            &["gone/missing".to_owned(), "cfg/m000".to_owned()],
            &configured,
        );
        assert_eq!(
            rows.iter()
                .map(|r| r["value"].as_str().unwrap())
                .collect::<Vec<_>>(),
            ["cfg/m000"]
        );
        assert_eq!(
            (&rows[0]["favorite"], &rows[0]["default"], cut),
            (&json!(true), &json!(true), false)
        );
        // The cache uses the same priority with its own limit (Task 4b).
        let (rows, cut) = ordered(&models, "cfg/m000", &[], &favorites, &configured, 512);
        assert_eq!((rows.len(), cut), (364, false));
    }

    fn big_row() -> Value {
        let mut options = vec![{
            let mut d = model("cfg/default", false, &"D".repeat(150));
            d["default"] = json!(true);
            d
        }];
        options.extend((0..30).map(|i| {
            let mut m = model(&format!("paid/f{i:02}"), false, &"F".repeat(150));
            m["favorite"] = json!(true);
            m
        }));
        options.extend((0..30).map(|i| model(&format!("cfg/c{i:02}"), false, &"C".repeat(150))));
        options.extend((0..30).map(|i| model(&format!("zen/r{i:02}"), true, &"R".repeat(150))));
        let providers: Vec<Value> = (0..40)
            .map(|i| {
                json!({"id": format!("p{i:02}"), "label": "P".repeat(60), "kind": "key",
            "credential_count": 1, "model_count": 3, "active_label": "A".repeat(40)})
            })
            .collect();
        json!({"id": "native-opencode", "provider": "opencode", "label": "Native OpenCode configuration",
               "model_options": options, "model_options_truncated": false, "providers": providers, "providers_truncated": false})
    }

    #[test]
    fn the_row_budget_drops_free_then_configured_then_favorites() {
        let mut row = big_row();
        assert!(ascii_len(&row) > ROW_BUDGET);
        fit_row(&mut row, ROW_BUDGET);
        assert!(ascii_len(&row) <= ROW_BUDGET, "{}", ascii_len(&row));
        let options = row["model_options"].as_array().unwrap();
        assert_eq!(options[0]["value"], "cfg/default");
        assert_eq!(options.iter().filter(|o| o["favorite"] == true).count(), 30);
        assert_eq!(
            options.iter().filter(|o| o["provider"] == "cfg").count(),
            31
        );
        let free = options.iter().filter(|o| o["free"] == true).count();
        assert!(free > 0 && free < 30, "{free}");
        assert_eq!(row["model_options_truncated"], true);
        assert_eq!(
            (
                row["providers"].as_array().unwrap().len(),
                &row["providers_truncated"]
            ),
            (40, &json!(false))
        );
    }

    #[test]
    fn providers_beyond_sixteen_go_only_after_every_model_but_the_default() {
        let mut row = big_row();
        let floor = {
            let mut r = row.clone();
            r["model_options"] = json!([row["model_options"][0].clone()]);
            r["providers"]
                .as_array_mut()
                .unwrap()
                .truncate(KEEP_PROVIDERS);
            ascii_len(&r)
        };
        fit_row(&mut row, floor);
        assert_eq!(row["model_options"].as_array().unwrap().len(), 1);
        assert_eq!(row["model_options"][0]["default"], true);
        assert_eq!(row["providers"].as_array().unwrap().len(), KEEP_PROVIDERS);
        assert_eq!(
            (&row["model_options_truncated"], &row["providers_truncated"]),
            (&json!(true), &json!(true))
        );
        // Below that, providers keep going; the default model stays.
        let mut row = big_row();
        fit_row(&mut row, floor - 1500);
        assert!(row["providers"].as_array().unwrap().len() < KEEP_PROVIDERS);
        assert_eq!(row["model_options"][0]["value"], "cfg/default");
        assert!(ascii_len(&row) <= floor - 1500);
    }

    #[test]
    fn a_row_within_the_budget_is_unchanged() {
        let fixture = shared();
        let mut row = fixture["hgs_account"].clone();
        fit_row(&mut row, ROW_BUDGET);
        assert_eq!(row, fixture["hgs_account"]);
    }

    #[test]
    fn the_row_never_carries_dropped_upstream_data() {
        let fixture = shared();
        let service = &fixture["service"];
        let snapshot = snapshot_from(
            &service["provider"],
            &service["model"],
            &service["default"],
            Some(&service["integration"]),
        );
        let mut row = row_from(&snapshot, &[], &["fixture/local"]);
        fit_row(&mut row, ROW_BUDGET);
        let text = row.to_string();
        let raw = fixture["service"].to_string();
        for canary in SECRETS {
            assert!(raw.contains(canary), "{canary} is not in the fixture");
            assert!(!text.contains(canary), "{canary}");
        }
        assert!(!text.contains("\"options\""));
        let mut keys: Vec<&str> = row
            .as_object()
            .unwrap()
            .keys()
            .map(String::as_str)
            .collect();
        keys.sort_unstable();
        assert_eq!(
            keys,
            [
                "model_options",
                "model_options_truncated",
                "providers",
                "providers_truncated",
                "settings_model"
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
            "favorite",
            "default",
        ];
        for option in row["model_options"].as_array().unwrap() {
            assert!(
                option
                    .as_object()
                    .unwrap()
                    .keys()
                    .all(|k| allowed.contains(&k.as_str())),
                "{option}"
            );
        }
        let keys = [
            "id",
            "label",
            "kind",
            "credential_count",
            "model_count",
            "active_label",
        ];
        for provider in row["providers"].as_array().unwrap() {
            assert!(
                provider
                    .as_object()
                    .unwrap()
                    .keys()
                    .all(|k| keys.contains(&k.as_str())),
                "{provider}"
            );
        }
    }

    #[test]
    fn configured_models_go_before_any_favorite() {
        let mut row = big_row();
        // Room for the default, every favorite and half of the configured models.
        let budget = {
            let mut r = row.clone();
            r["model_options"]
                .as_array_mut()
                .unwrap()
                .truncate(1 + 30 + 15);
            r["model_options_truncated"] = json!(true);
            ascii_len(&r)
        };
        fit_row(&mut row, budget);
        assert!(ascii_len(&row) <= budget);
        let options = row["model_options"].as_array().unwrap();
        assert_eq!(options[0]["value"], "cfg/default");
        assert_eq!(options.iter().filter(|o| o["favorite"] == true).count(), 30);
        assert_eq!(options.iter().filter(|o| o["free"] == true).count(), 0);
        assert_eq!(
            options.iter().filter(|o| o["provider"] == "cfg").count(),
            1 + 15
        );
        assert_eq!(
            (
                &row["model_options_truncated"],
                &row["providers_truncated"],
                row["providers"].as_array().unwrap().len()
            ),
            (&json!(true), &json!(false), 40)
        );
    }

    #[test]
    fn a_damaged_row_still_fits_without_its_default_or_odd_fields() {
        let row_with = |label: usize| {
            let mut default = model("cfg/default", false, &"D".repeat(label));
            default["default"] = json!(true);
            json!({"id": "native-opencode", "provider": "opencode", "label": "Native",
                "installed": true, "model_options": [default], "model_options_truncated": false,
                "providers": [], "providers_truncated": false, "catalog_at": 1792267200.5})
        };
        let fits = |row: &Value| assert!(ascii_len(row) <= ROW_BUDGET, "{}", ascii_len(row));
        // An oversize default entry goes; small fields stay.
        let mut row = row_with(40_000);
        fit_row(&mut row, ROW_BUDGET);
        fits(&row);
        assert_eq!(row["model_options"], json!([]));
        assert_eq!(
            (&row["model_options_truncated"], &row["providers_truncated"]),
            (&json!(true), &json!(false))
        );
        assert_eq!(
            (&row["label"], &row["catalog_at"]),
            (&json!("Native"), &json!(1792267200.5))
        );
        // A larger non-scalar field goes first; a default that fits then stays.
        let mut row = row_with(20_000);
        row["usage"] = json!({"windows": ["U".repeat(50_000)]});
        fit_row(&mut row, ROW_BUDGET);
        fits(&row);
        assert!(row.get("usage").is_none());
        assert_eq!(row["model_options"][0]["value"], "cfg/default");
        assert_eq!(row["model_options_truncated"], false);
        // Both go when neither fits alone.
        let mut row = row_with(40_000);
        row["usage"] = json!(["U".repeat(50_000)]);
        fit_row(&mut row, ROW_BUDGET);
        fits(&row);
        assert!(row.get("usage").is_none() && row["model_options"] == json!([]));
        // Providers go first by the budget order, then the odd field.
        let mut row = row_with(1);
        row["usage"] = json!(["U".repeat(50_000)]);
        row["providers"] = json!([{"id": "p", "label": "P", "kind": "key", "credential_count": 0, "model_count": 0}]);
        fit_row(&mut row, ROW_BUDGET);
        fits(&row);
        assert!(row.get("usage").is_none());
        assert_eq!(row["model_options"][0]["value"], "cfg/default");
        assert_eq!(
            (
                &row["providers"],
                &row["model_options_truncated"],
                &row["providers_truncated"]
            ),
            (&json!([]), &json!(false), &json!(true))
        );
        // Oversize scalars, a list that is not one and an odd flag: any row
        // whose identity fits ends within budget.
        let mut row = row_with(40_000);
        row["label"] = json!("L".repeat(50_000));
        row["model_options_truncated"] = json!("T".repeat(50_000));
        row["providers"] = json!("P".repeat(50_000));
        fit_row(&mut row, ROW_BUDGET);
        fits(&row);
        assert_eq!(
            row,
            json!({"id": "native-opencode", "provider": "opencode", "installed": true,
                "model_options": [], "model_options_truncated": true, "providers": [],
                "providers_truncated": true, "catalog_at": 1792267200.5})
        );
    }

    #[test]
    fn ascii_len_spells_numbers_as_python_does() {
        // `json.dumps` writes floats as Python's `repr`: positional from 1e-4 up
        // to 1e16, else a mantissa and a signed exponent of at least two digits.
        for (number, python) in [
            (1e-7, "1e-07"),
            (1e-5, "1e-05"),
            (0.0001, "0.0001"),
            (0.00012, "0.00012"),
            (-1e-4, "-0.0001"),
            (1e16, "1e+16"),
            (1e15, "1000000000000000.0"),
            (1e22, "1e+22"),
            (1e100, "1e+100"),
            (1.5e300, "1.5e+300"),
            (-2.5e-10, "-2.5e-10"),
            (123456789012345678.0, "1.2345678901234568e+17"),
            (5e-324, "5e-324"),
            (1.7976931348623157e308, "1.7976931348623157e+308"),
            (0.0, "0.0"),
            (-0.0, "-0.0"),
            (0.1, "0.1"),
            (100.0, "100.0"),
            (12345.678, "12345.678"),
            (1792267200.5, "1792267200.5"),
        ] {
            let value = json!(number);
            assert_eq!(
                python_number(value.as_number().unwrap()),
                python,
                "{number:e}"
            );
            assert_eq!(ascii_len(&value), python.len(), "{number:e}");
            assert_eq!(
                ascii_len(&json!([number, {"k": number}])),
                2 * python.len() + 11
            );
        }
        for (value, python) in [
            (json!(0), "0"),
            (json!(-12), "-12"),
            (json!(u64::MAX), "18446744073709551615"),
            (json!(i64::MIN), "-9223372036854775808"),
            (serde_json::from_str("1.0E-7").unwrap(), "1e-07"),
            (serde_json::from_str("2e3").unwrap(), "2000.0"),
        ] {
            assert_eq!(python_number(value.as_number().unwrap()), python, "{value}");
            assert_eq!(ascii_len(&value), python.len(), "{value}");
        }
    }
}
