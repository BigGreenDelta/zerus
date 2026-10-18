//! Additive logical project ABI; no launch/folder authority, unchanged legacy
//! read index and durable mutation classification.
use serde_json::{json, Value};
use zerus_relay::protocol;
fn request(mutation: bool) -> Value {
    let id = "11111111-1111-4111-8111-111111111111";
    let mut payload = json!({"request_id":id,"expected_run_id":"run-one","expected_conversation_id":"one","expected_machine_id":"22222222-2222-4222-8222-222222222222"});
    if mutation {
        payload["swarm_id"] = json!("33333333-3333-4333-8333-333333333333");
        payload["project_id"] = json!("two");
        payload["membership_token"] = json!("a".repeat(64));
        payload["catalog_epoch"] = json!("b".repeat(64));
    }
    json!({"request_id":id,"computer_id":"44444444-4444-4444-8444-444444444444","session":"claude/example/task","operation":if mutation {"project_assign"} else {"project_assignment"},"payload":payload})
}
#[test]
fn only_full_exact_metadata_fields_are_admitted() {
    for mutation in [false, true] {
        let request = request(mutation);
        protocol::request(&request).unwrap();
        for field in [
            "directory",
            "add_folder",
            "worktree",
            "organization",
            "archive_id",
            "agent_id",
        ] {
            let mut bad = request.clone();
            bad["payload"][field] = json!("foreign");
            assert!(protocol::request(&bad).is_err());
        }
        for field in [
            "request_id",
            "expected_machine_id",
            "expected_run_id",
            "expected_conversation_id",
        ] {
            let mut bad = request.clone();
            bad["payload"].as_object_mut().unwrap().remove(field);
            assert!(protocol::request(&bad).is_err());
        }
        let mut bad = request.clone();
        bad["payload"]["expected_machine_id"] = json!("00000000-0000-0000-0000-000000000000");
        assert!(protocol::request(&bad).is_err());
    }
    assert!(protocol::READS.contains(&"project_assignment"));
    assert!(!protocol::READS.contains(&"project_assign"));
    assert_eq!(protocol::READ_SQL,"('inspect','history','process_output','terminal_snapshot','catalog','dirs','worktrees','mark_read')");
}
