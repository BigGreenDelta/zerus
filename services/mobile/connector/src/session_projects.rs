//! Metadata-only, exact native session project ABI. Names are payload data,
//! never CLI arguments. Both commands and the version marker are required.
use crate::native::Call;
use serde_json::Value;
use std::collections::HashSet;
pub const OPERATIONS: &[&str] = &["project_assignment", "project_assign"];
pub const FEATURE: &str = "session_project_assignment_v1";
pub const HELP_MARKER: &str = "Mobile project assignment ABI: session-project-v1";
pub const UNAVAILABLE: &str = "native session project assignment ABI is unavailable";
pub const MAX_RESULT: usize = 128 * 1024;
pub fn from_help(help: &str) -> bool {
    help.lines().any(|line| line.trim() == HELP_MARKER)
        && help.contains("swarm session-project --json")
        && help.contains("swarm assign-session --json")
}
pub fn validate(operation: &str, payload: &Value, request_id: &str) -> Result<(), String> {
    zerus_relay::protocol::session_project(payload, request_id, operation == "project_assign")
        .map_err(|_| "invalid scoped session project fields".into())
}
pub fn call(operation: &str, payload: &Value, session: &str) -> Call {
    let mut payload = payload.clone();
    payload["name"] = Value::String(session.into());
    let mut call = Call::new([
        "swarm",
        if operation == "project_assign" {
            "assign-session"
        } else {
            "session-project"
        },
        "--json",
    ])
    .payload(payload)
    .timeout(10.0);
    call.max_output_bytes = Some(MAX_RESULT);
    call
}
fn text(value: &Value, max: usize, empty: bool) -> bool {
    value.as_str().is_some_and(|s| {
        s.len() <= max && (empty || !s.is_empty()) && !s.chars().any(char::is_control)
    })
}
fn hex(value: &Value) -> bool {
    value.as_str().is_some_and(|s| {
        s.len() == 64
            && s.bytes()
                .all(|c| c.is_ascii_digit() || (b'a'..=b'f').contains(&c))
    })
}
fn uuid(value: &Value) -> bool {
    zerus_relay::protocol::uuid(value, true).is_ok()
}
pub fn result_valid(operation: &str, result: &Value, payload: &Value, session: &str) -> bool {
    let Some(object) = result.as_object() else {
        return false;
    };
    let mutation = operation == "project_assign";
    let allowed = [
        "schema",
        "request_id",
        "status",
        "name",
        "run_id",
        "conversation_id",
        "machine_id",
        "swarm_id",
        "membership_token",
        "catalog_epoch",
        "project_id",
        "current_project_id",
        "projects",
        "next_cursor",
        "error",
    ];
    if object.keys().any(|key| !allowed.contains(&key.as_str()))
        || result["schema"] != 1
        || result["request_id"] != payload["request_id"]
        || result["name"] != session
        || result["run_id"] != payload["expected_run_id"]
        || result["conversation_id"] != payload["expected_conversation_id"]
        || result["machine_id"] != payload["expected_machine_id"]
        || serde_json::to_vec(result).map_or(true, |b| b.len() > MAX_RESULT)
        || result
            .get("error")
            .is_some_and(|error| !text(error, 2048, false))
    {
        return false;
    }
    if mutation
        && [
            "swarm_id",
            "membership_token",
            "catalog_epoch",
            "project_id",
        ]
        .iter()
        .any(|key| result[*key] != payload[*key])
    {
        return false;
    }
    if result["status"] == "failed" || (mutation && result["status"] == "uncertain") {
        return result.get("error").is_some()
            && result.get("projects").is_none()
            && result.get("next_cursor").is_none();
    }
    if mutation {
        return result["status"] == "assigned"
            && result["current_project_id"] == payload["project_id"]
            && result.get("projects").is_none()
            && result.get("next_cursor").is_none()
            && result.get("error").is_none();
    }
    if result["status"] != "ok"
        || !uuid(&result["swarm_id"])
        || !hex(&result["membership_token"])
        || !hex(&result["catalog_epoch"])
        || !text(&result["current_project_id"], 128, false)
        || result.get("error").is_some()
        || result.get("project_id").is_some()
    {
        return false;
    }
    let Some(projects) = result["projects"]
        .as_array()
        .filter(|rows| rows.len() <= 100)
    else {
        return false;
    };
    let mut ids = HashSet::new();
    if !projects.iter().all(|p| {
        p.as_object().is_some_and(|o| {
            o.len() == 3
                && o.keys()
                    .all(|key| ["id", "name", "color"].contains(&key.as_str()))
        }) && text(&p["id"], 128, false)
            && ids.insert(p["id"].as_str().unwrap().to_owned())
            && text(&p["name"], 320, false)
            && p["color"].as_str().is_some_and(|s| {
                s.len() == 7 && s.starts_with('#') && s[1..].bytes().all(|b| b.is_ascii_hexdigit())
            })
    }) {
        return false;
    }
    result
        .get("next_cursor")
        .is_some_and(|cursor| cursor.is_null() || text(cursor, 512, false))
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;
    const ID: &str = "11111111-1111-4111-8111-111111111111";
    fn payload(mutation: bool) -> Value {
        let mut result = json!({"request_id":ID,"expected_run_id":"run-one","expected_conversation_id":"one","expected_machine_id":"22222222-2222-4222-8222-222222222222"});
        if mutation {
            result["swarm_id"] = json!("33333333-3333-4333-8333-333333333333");
            result["project_id"] = json!("two");
            result["membership_token"] = json!("a".repeat(64));
            result["catalog_epoch"] = json!("b".repeat(64));
        }
        result
    }
    fn result(mutation: bool) -> Value {
        let payload = payload(mutation);
        let mut result = json!({"schema":1,"request_id":ID,"name":"claude/example/task","run_id":"run-one","conversation_id":"one","machine_id":payload["expected_machine_id"],
            "status":if mutation {"assigned"} else {"ok"},"swarm_id":"33333333-3333-4333-8333-333333333333","membership_token":"a".repeat(64),"catalog_epoch":"b".repeat(64),"current_project_id":"two"});
        if mutation {
            result["project_id"] = payload["project_id"].clone();
        } else {
            result["projects"] = json!([{"id":"two","name":"Два ' $(literal)","color":"#123456"}]);
            result["next_cursor"] = Value::Null;
        }
        result
    }
    #[test]
    fn old_or_partial_help_never_advertises_either_operation() {
        let help = format!(
            "hgs swarm session-project --json\nhgs swarm assign-session --json\n{HELP_MARKER}"
        );
        assert!(from_help(&help));
        for line in help.lines() {
            assert!(!from_help(&help.replace(line, "")));
        }
        let flags = crate::capabilities::Flags::from_help(&help);
        assert!(flags.advertised(false)["features"]
            .as_array()
            .unwrap()
            .iter()
            .any(|v| v == FEATURE));
    }
    #[test]
    fn exact_payload_uses_stdin_and_cannot_smuggle_launch_or_directory_fields() {
        for operation in OPERATIONS {
            let payload = payload(*operation == "project_assign");
            validate(operation, &payload, ID).unwrap();
            let call = call(operation, &payload, "claude/example/task");
            assert_eq!(call.argv.len(), 3);
            assert_eq!(call.argv[0], "swarm");
            assert!(!call.argv.iter().any(|arg| arg.contains("claude/")));
            assert_eq!(call.payload.unwrap()["name"], "claude/example/task");
            for field in [
                "directory",
                "add_folder",
                "organization",
                "archive_id",
                "agent_id",
            ] {
                let mut bad = payload.clone();
                bad[field] = json!("foreign");
                assert!(validate(operation, &bad, ID).is_err());
            }
        }
    }
    #[test]
    fn only_exact_scoped_and_bounded_native_results_are_accepted() {
        for operation in OPERATIONS {
            let mutation = *operation == "project_assign";
            let payload = payload(mutation);
            let result = result(mutation);
            assert!(result_valid(
                operation,
                &result,
                &payload,
                "claude/example/task"
            ));
            for field in [
                "request_id",
                "name",
                "run_id",
                "conversation_id",
                "machine_id",
                "schema",
            ] {
                let mut bad = result.clone();
                bad[field] = json!("foreign");
                assert!(!result_valid(
                    operation,
                    &bad,
                    &payload,
                    "claude/example/task"
                ));
            }
            let mut bad = result.clone();
            bad["private_credentials"] = json!("never");
            assert!(!result_valid(
                operation,
                &bad,
                &payload,
                "claude/example/task"
            ));
        }
        let mut raw = result(false);
        raw["projects"][0]["name"] = json!("x".repeat(321));
        assert!(!result_valid(
            "project_assignment",
            &raw,
            &payload(false),
            "claude/example/task"
        ));
        let mut raw = result(true);
        raw["catalog_epoch"] = json!("c".repeat(64));
        assert!(!result_valid(
            "project_assign",
            &raw,
            &payload(true),
            "claude/example/task"
        ));
    }
}
