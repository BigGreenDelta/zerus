//! Additive, scoped read-only diagnostic. No account/repair command is transported.
use serde_json::Value;
pub const OPERATION: &str = "hooks_status";
pub const HELP_MARKER: &str = "hooks-health-v1";
pub const UNAVAILABLE: &str = "native hook health ABI is unavailable";
fn keys(value: &Value, names: &[&str], exact: bool) -> bool {
    value.as_object().is_some_and(|object| {
        (!exact || object.len() == names.len())
            && object.keys().all(|key| names.contains(&key.as_str()))
    })
}
fn names(value: &Value) -> bool {
    value.as_array().is_some_and(|rows| {
        rows.len() <= 32
            && rows.iter().all(|row| {
                row.as_str().is_some_and(|s| {
                    !s.is_empty() && s.len() <= 64 && s.bytes().all(|b| b.is_ascii_alphanumeric())
                })
            })
    })
}
fn metadata_valid(result: &Value) -> bool {
    let config = &result["configuration"];
    if !keys(
        config,
        &[
            "state",
            "compatibility",
            "sha256",
            "path_id",
            "expected_events",
            "registered_events",
            "missing_events",
            "duplicate_events",
            "conditional_matchers",
            "answer_channel_registered",
        ],
        false,
    ) || ![
        "configured",
        "missing",
        "partial",
        "invalid",
        "unreadable",
        "not_applicable",
    ]
    .iter()
    .any(|state| config["state"] == *state)
    {
        return false;
    }
    let compatibility = &config["compatibility"];
    if !keys(
        compatibility,
        &[
            "state",
            "basis",
            "native_version",
            "unsupported_registered_events",
        ],
        false,
    ) || !["compatible", "incompatible", "unknown", "not_applicable"]
        .iter()
        .any(|state| compatibility["state"] == *state)
    {
        return false;
    }
    for key in ["sha256", "path_id"] {
        if let Some(value) = config.get(key) {
            if !value.as_str().is_some_and(|s| {
                s.len() == 64
                    && s.bytes()
                        .all(|b| b.is_ascii_digit() || (b'a'..=b'f').contains(&b))
            }) {
                return false;
            }
        }
    }
    for key in [
        "expected_events",
        "registered_events",
        "missing_events",
        "duplicate_events",
    ] {
        if config.get(key).is_some_and(|value| !names(value)) {
            return false;
        }
    }
    for key in ["conditional_matchers", "answer_channel_registered"] {
        if config.get(key).is_some_and(|value| !value.is_boolean()) {
            return false;
        }
    }
    if compatibility
        .get("unsupported_registered_events")
        .is_some_and(|value| !names(value))
    {
        return false;
    }
    for key in ["basis", "native_version"] {
        if compatibility.get(key).is_some_and(|value| {
            !value.is_null() && !value.as_str().is_some_and(|s| s.len() <= 128)
        }) {
            return false;
        }
    }
    for key in ["effective", "trust"] {
        let value = &result[key];
        if !keys(value, &["state", "reason"], true)
            || value["state"] != "unknown"
            || !value["reason"].as_str().is_some_and(|s| s.len() <= 512)
        {
            return false;
        }
    }
    let observed = &result["observed"];
    if !keys(observed, &["state", "events"], true)
        || !["unknown", "observed"]
            .iter()
            .any(|state| observed["state"] == *state)
    {
        return false;
    }
    observed["events"].as_array().is_some_and(|events| {
        events.len() <= 32
            && events.iter().all(|event| {
                keys(event, &["event", "count", "last_at"], true)
                    && event["event"].as_str().is_some_and(|s| {
                        !s.is_empty()
                            && s.len() <= 64
                            && s.bytes().all(|b| b.is_ascii_alphanumeric())
                    })
                    && event["count"].as_u64().is_some_and(|count| count > 0)
                    && event["last_at"]
                        .as_f64()
                        .is_some_and(|at| at.is_finite() && at > 0. && at < 1e12)
            })
    })
}
pub fn result_valid(result: &Value, payload: &Value, session: &str) -> bool {
    let fields = [
        "schema",
        "provider",
        "account_id",
        "configuration",
        "effective",
        "trust",
        "observed",
        "running_agents_retrofitted",
        "request_id",
        "name",
        "run_id",
        "conversation_id",
    ];
    result.as_object().is_some_and(|object| {
        object.len() == fields.len() && object.keys().all(|key| fields.contains(&key.as_str()))
    }) && result["schema"] == 1
        && result["request_id"] == payload["request_id"]
        && result["name"] == session
        && result["run_id"] == payload["expected_run_id"]
        && result["conversation_id"] == payload["expected_conversation_id"]
        && result["running_agents_retrofitted"] == false
        && result["provider"].is_string()
        && result["account_id"].is_string()
        && metadata_valid(result)
        && result["configuration"].is_object()
        && result["effective"].is_object()
        && result["effective"]["state"] == "unknown"
        && result["trust"].is_object()
        && result["trust"]["state"] == "unknown"
        && result["observed"].is_object()
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;
    #[test]
    fn diagnostics_are_opt_in_and_never_offer_repair() {
        let old = crate::capabilities::Flags::from_help("inspect answer");
        assert!(!old.python_operations(true).contains(OPERATION));
        let current = crate::capabilities::Flags::from_help(HELP_MARKER);
        assert!(current.advertised(false)["operations"]
            .as_array()
            .unwrap()
            .iter()
            .any(|v| v == OPERATION));
        assert!(!current
            .advertised(false)
            .to_string()
            .contains("hooks_repair"));
    }
    #[test]
    fn exact_read_acknowledgement_cannot_be_retargeted_or_claim_trust() {
        let payload = json!({"request_id":"00000000-0000-4000-8000-000000000001","expected_run_id":"run-1","expected_conversation_id":"one"});
        let result = json!({"schema":1,"provider":"claude","account_id":"native-claude","configuration":{"state":"missing","compatibility":{"state":"unknown"}},"effective":{"state":"unknown","reason":"Not verified"},"trust":{"state":"unknown","reason":"Not verified"},"observed":{"state":"unknown","events":[]},"running_agents_retrofitted":false,"request_id":payload["request_id"],"name":"claude/example/task","run_id":"run-1","conversation_id":"one"});
        assert!(result_valid(&result, &payload, "claude/example/task"));
        for key in ["request_id", "name", "run_id", "conversation_id"] {
            let mut bad = result.clone();
            bad[key] = json!("foreign");
            assert!(!result_valid(&bad, &payload, "claude/example/task"));
        }
        for key in ["effective", "trust"] {
            let mut bad = result.clone();
            bad[key]["state"] = json!("verified");
            assert!(!result_valid(&bad, &payload, "claude/example/task"));
        }
        let mut bad = result.clone();
        bad["private_settings"] = json!({"secret":"not transportable"});
        assert!(!result_valid(&bad, &payload, "claude/example/task"));
        let mut bad = result;
        bad["schema"] = json!(true);
        assert!(!result_valid(&bad, &payload, "claude/example/task"));
    }
}
