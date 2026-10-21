use serde_json::json;
use zerus_relay::protocol;
#[test]
fn hook_health_is_only_an_exact_scoped_read() {
    let id = "00000000-0000-4000-8000-000000000001";
    let body = json!({"request_id":id,"computer_id":"00000000-0000-4000-8000-000000000002","operation":"hooks_status","session":"claude/example/task","payload":{"request_id":id,"expected_run_id":"run-1","expected_conversation_id":"one"}});
    assert!(protocol::request(&body).is_ok());
    assert!(protocol::READS.contains(&"hooks_status"));
    for key in [
        "repair",
        "account_id",
        "expected_path_id",
        "agent_id",
        "archive_id",
    ] {
        let mut bad = body.clone();
        bad["payload"][key] = json!("foreign");
        assert!(protocol::request(&bad).is_err(), "{key}");
    }
    let mut bad = body.clone();
    bad["payload"]["expected_conversation_id"] = json!("");
    assert!(protocol::request(&bad).is_err());
    for operation in ["hooks_repair", "hooks_install", "hooks_sync"] {
        let mut bad = body.clone();
        bad["operation"] = json!(operation);
        assert!(protocol::request(&bad).is_err());
    }
}
