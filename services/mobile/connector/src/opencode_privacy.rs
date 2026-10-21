//! OpenCode's service diagnostics stay on the session's computer. This is a
//! projection of named diagnostic fields, never a filter of conversation text.
use serde_json::{json, Map, Value};

pub const INSPECTION: &str =
    "OpenCode session details are unavailable; check OpenCode on the session's computer";
pub const ACTION_FAILED: &str =
    "OpenCode could not complete this action; inspect the session on its computer";
pub const ACTION_UNCERTAIN: &str =
    "OpenCode action outcome is uncertain; inspect the session before creating a new request";

/// `cmd` is the native adapter's provider identity, not a session-name prefix
/// or the contents of an upstream error. Ordinary reply previews are retained.
pub fn row(row: &mut Value) {
    if row.get("cmd").and_then(Value::as_str) != Some("opencode") {
        return;
    }
    if row.get("inspection_reason").is_some() {
        row["inspection_reason"] = json!(INSPECTION);
        row["activity_detail"] = json!(INSPECTION);
    }
}

/// Called only after the exact lifecycle acknowledgement was validated.
/// `known` comes from this machine context's native listing. New hgs receipts
/// also name their owned diagnostic source; upstream response bodies cannot
/// add fields to that envelope. This covers older native CLIs without guessing
/// a provider from arbitrary error text or a user-chosen session name.
pub fn action(result: &mut Map<String, Value>, known: bool) {
    if !known && result.get("error_source").and_then(Value::as_str) != Some("opencode") {
        return;
    }
    let reason = match result.get("status").and_then(Value::as_str) {
        Some("failed") => ACTION_FAILED,
        Some("uncertain") => ACTION_UNCERTAIN,
        _ => return,
    };
    if result.get("error").is_some_and(|error| !error.is_null()) {
        result.insert("error".into(), json!(reason));
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    const PRIVATE: &str =
        "https://provider.example.invalid/v1?key=fixture-YO85-DIAGNOSTIC\u{1b}[31m\u{7}";

    #[test]
    fn inspection_diagnostics_are_fixed_and_conversation_content_is_unchanged() {
        let events = json!([{"type":"AgentMessage","detail":"OpenCode HTTP 400: keep this literal reply","at":1}]);
        let cards = json!([{"question_id":"q1","title":"Keep this question","options":["https://user.example.invalid"]}]);
        for diagnostic in [json!(PRIVATE), Value::Null, json!({"message":PRIVATE})] {
            let mut value = json!({"cmd":"opencode","name":"custom/session",
                "inspection_reason":diagnostic,"activity_detail":PRIVATE,
                "inspection_unavailable":true,"events":events,"pending_questions":cards,
                "phase":"unknown","run_id":"run","conversation_id":"ses_A"});
            row(&mut value);
            assert_eq!(value["inspection_reason"], INSPECTION);
            assert_eq!(value["activity_detail"], INSPECTION);
            assert!(!value.to_string().contains("fixture-YO85-DIAGNOSTIC"));
            assert_eq!(value["events"], events);
            assert_eq!(value["pending_questions"], cards);
            assert_eq!(value["run_id"], "run");
            assert_eq!(value["conversation_id"], "ses_A");
            assert_eq!(value["phase"], "unknown");
        }
    }

    #[test]
    fn ordinary_replies_and_other_providers_are_never_classified_by_error_text_or_name() {
        for original in [
            json!({"cmd":"opencode","activity_detail":PRIVATE,"events":[{"detail":PRIVATE}]}),
            json!({"cmd":"codex","name":"opencode/example/session","inspection_reason":PRIVATE,"activity_detail":PRIVATE}),
            json!({"name":"opencode/example/session","inspection_reason":PRIVATE}),
            json!({"cmd":["opencode"],"inspection_reason":PRIVATE}),
        ] {
            let mut value = original.clone();
            row(&mut value);
            assert_eq!(value, original);
        }
    }

    #[test]
    fn action_diagnostics_use_owned_provider_identity_and_keep_status_and_request_identity() {
        for (status, expected) in [("failed", ACTION_FAILED), ("uncertain", ACTION_UNCERTAIN)] {
            for known in [false, true] {
                let mut value = json!({"request_id":"request","name":"custom/session","run_id":"run",
                    "conversation_id":"ses_A","status":status,"error":PRIVATE});
                if !known {
                    value["error_source"] = json!("opencode");
                }
                let object = value.as_object_mut().unwrap();
                action(object, known);
                assert_eq!(value["error"], expected);
                assert_eq!(value["status"], status);
                assert_eq!(value["request_id"], "request");
                assert_eq!(value["run_id"], "run");
                assert_eq!(value["conversation_id"], "ses_A");
                assert!(!value.to_string().contains("fixture-YO85-DIAGNOSTIC"));
            }
        }
    }

    #[test]
    fn absent_or_malformed_origin_does_not_reclassify_another_provider() {
        for source in [
            Value::Null,
            json!("codex"),
            json!(["opencode"]),
            json!({"name":"opencode"}),
        ] {
            let original = json!({"name":"opencode/example/session","status":"failed","error":PRIVATE,"error_source":source});
            let mut value = original.clone();
            action(value.as_object_mut().unwrap(), false);
            assert_eq!(value, original);
        }
    }

    #[test]
    fn success_and_missing_errors_are_unchanged() {
        for original in [
            json!({"status":"completed","result_target":{"name":"new","run_id":"run","conversation_id":"ses_A"}}),
            json!({"status":"failed","error":null}),
            json!({"status":"uncertain"}),
        ] {
            let mut value = original.clone();
            action(value.as_object_mut().unwrap(), true);
            assert_eq!(value, original);
        }
    }
}
