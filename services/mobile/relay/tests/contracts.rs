use serde_json::{json, Value};
use std::time::Duration;
use tokio_util::sync::CancellationToken;
use uuid::Uuid;
use zerus_relay::{
    args,
    config::Config,
    db::{i, now, s},
    http::{serve, App},
    json::{canonical, digest},
    protocol,
    store::{Role, Store},
};

mod common;
use common::{Backend, Fixture};

#[test]
fn opencode_launch_model_is_native_account_scoped_and_bounded() {
    let id = Uuid::new_v4().to_string();
    let value = json!({"request_id":id,"computer_id":Uuid::new_v4().to_string(),"operation":"launch","session":"", "payload":{"request_id":id,"agent":"opencode","directory":"/workspace/orbit","tag":"local","account_id":"native-opencode","model":"ollama/qwen3-coder:30b"}});
    protocol::request(&value).unwrap();
    let mut unicode_boundary = value.clone();
    unicode_boundary["payload"]["model"] = json!(format!("ollama/{}", "界".repeat(83)));
    protocol::request(&unicode_boundary).unwrap();
    let mut default = value.clone();
    default["payload"].as_object_mut().unwrap().remove("model");
    protocol::request(&default).unwrap();
    for (key, replacement) in [
        ("agent", json!("codex")),
        ("account_id", json!("native-codex")),
        ("model", json!("local")),
        ("model", json!("/local")),
        ("model", json!("ollama/")),
        ("model", json!("ollama/with space")),
        ("model", json!("ollama/local\n")),
        ("model", json!(format!("ollama/{}", "a".repeat(257)))),
        ("model", json!(format!("ollama/{}", "界".repeat(84)))),
    ] {
        let mut bad = value.clone();
        bad["payload"][key] = replacement;
        assert!(
            protocol::request(&bad).is_err(),
            "accepted invalid model route {key}"
        );
    }
}

fn voice_command(computer: &str, op: &str, message: Option<&str>) -> Value {
    let id = Uuid::new_v4().to_string();
    let mut payload =
        json!({"request_id":id,"expected_run_id":"run","expected_conversation_id":"conversation"});
    if op == "voice_submit" {
        payload["speech"] =
            json!({"machine_id":Uuid::new_v4().to_string(),"profile_id":"gigaam-ru"});
        payload["audio"] =
            json!({"format":"pcm_s16le","sample_rate":16000,"channels":1,"data_base64":"AAAAAA=="});
    } else if ["voice_status", "voice_cancel"].contains(&op) {
        payload["message_id"] = json!(message.unwrap());
    }
    json!({"request_id":id,"computer_id":computer,"operation":op,"session":"codex/example/tag","payload":payload})
}

#[tokio::test]
async fn video_voice_requires_host_capability_and_erases_both_media_after_acceptance() {
    use base64::{engine::general_purpose::STANDARD, Engine};
    let f = Fixture::new().await;
    let mut upload = voice_command(&f.node.id, "voice_submit", None);
    upload["payload"]["audio"]["data_base64"] = json!(STANDARD.encode(vec![0u8; 32000]));
    let mut mp4 = Vec::new();
    for (kind, data) in [
        (b"ftyp", b"isom0000".as_slice()),
        (b"moov", b"".as_slice()),
        (b"mdat", b"video".as_slice()),
    ] {
        mp4.extend_from_slice(&((8 + data.len()) as u32).to_be_bytes());
        mp4.extend_from_slice(kind);
        mp4.extend_from_slice(data);
    }
    upload["payload"]["video"] = json!({"format":"mp4","data_base64":STANDARD.encode(mp4),"duration_ms":1000,"width":480,"height":480});
    protocol::request(&upload).unwrap();
    f.beat(&json!({"sessions":[{"name":"codex/example/tag"}],"mobile_capabilities":{"protocol_version":1,"operations":protocol::OPERATIONS}})).await;
    assert_eq!(f.submit(&upload).await.unwrap_err().0.as_u16(), 409);
    f.beat(&json!({"sessions":[{"name":"codex/example/tag"}],"mobile_capabilities":{"protocol_version":1,"operations":protocol::OPERATIONS,"features":["voice_video"]}})).await;
    f.submit(&upload).await.unwrap();
    assert!(f.store.claim(&f.node, false).await.unwrap().is_some());
    let result = json!({"state":"completed","result":{"schema":1,"message":{"id":upload["request_id"],"state":"queued","revision":1}},"error":null});
    f.store
        .result(
            &f.node,
            s(&upload, "request_id"),
            &result,
            Some(&canonical(&result["result"]).unwrap()),
        )
        .await
        .unwrap();
    let row = f
        .store
        .db
        .one(
            "SELECT body,body_bytes FROM requests WHERE id=?",
            args![&upload["request_id"]],
        )
        .await
        .unwrap()
        .unwrap();
    assert_eq!(row["body"], "");
    assert_eq!(row["body_bytes"], 0);
    assert_eq!(f.submit(&upload).await.unwrap()["result"], result["result"]);
    assert!(f.store.claim(&f.node, false).await.unwrap().is_none());
    f.check_accounting().await;
    f.close().await;
}

#[test]
fn model_settings_match_launch_utf8_byte_bound() {
    let id = Uuid::new_v4().to_string();
    let mut value = json!({"request_id":id,"computer_id":Uuid::new_v4().to_string(),"operation":"settings","session":"opencode/orbit/local", "payload":{"request_id":id,"expected_run_id":"run","expected_conversation_id":"ses_native","model":"ollama/local"}});
    for model in [
        format!("ollama/{}", "a".repeat(249)),
        format!("ollama/{}", "界".repeat(83)),
    ] {
        value["payload"]["model"] = json!(model);
        protocol::request(&value).unwrap();
    }
    for model in [
        format!("ollama/{}", "a".repeat(250)),
        format!("ollama/{}", "界".repeat(84)),
    ] {
        value["payload"]["model"] = json!(model);
        assert!(protocol::request(&value).is_err());
    }
}

#[test]
fn voice_wire_requires_bounded_audio_and_stable_machine_route() {
    let computer = Uuid::new_v4().to_string();
    for op in [
        "voice_submit",
        "voice_profiles",
        "voice_status",
        "voice_cancel",
    ] {
        let value = voice_command(&computer, op, Some(&Uuid::new_v4().to_string()));
        protocol::request(&value).unwrap();
        let mut bad = value.clone();
        bad["session"] = json!("");
        assert!(protocol::request(&bad).is_err());
        let mut bad = value;
        bad["payload"]["request_id"] = json!(Uuid::new_v4().to_string());
        assert!(protocol::request(&bad).is_err());
    }
    for route in [
        json!({"machine_id":"arch","profile_id":"gigaam-ru"}),
        json!({"machine_id":computer,"profile_id":"../model"}),
        json!({"machine_id":computer,"profile_id":"ok","command":"example"}),
    ] {
        let mut value = voice_command(&computer, "voice_submit", None);
        value["payload"]["speech"] = route;
        assert!(protocol::request(&value).is_err());
    }
    for data in ["AA==", "AA==AA==", "AA B", "AAAA"] {
        let mut value = voice_command(&computer, "voice_submit", None);
        value["payload"]["audio"]["data_base64"] = json!(data);
        assert!(protocol::request(&value).is_err());
    }
}

#[tokio::test]
async fn voice_acceptance_erases_relay_audio_and_preserves_shared_conversation_access() {
    let f = Fixture::new().await;
    f.beat(&json!({"sessions":[{"name":"codex/example/tag"}],"mobile_capabilities":{"protocol_version":1,"operations":protocol::OPERATIONS}})).await;
    let upload = voice_command(&f.node.id, "voice_submit", None);
    f.submit(&upload).await.unwrap();
    assert!(f.store.claim(&f.node, false).await.unwrap().is_some());
    let result = json!({"state":"completed","result":{"schema":1,"message":{"id":upload["request_id"],"state":"queued","revision":1}},"error":null});
    f.store
        .result(
            &f.node,
            s(&upload, "request_id"),
            &result,
            Some(&canonical(&result["result"]).unwrap()),
        )
        .await
        .unwrap();
    let row = f
        .store
        .db
        .one(
            "SELECT body,body_bytes FROM requests WHERE id=?",
            args![&upload["request_id"]],
        )
        .await
        .unwrap()
        .unwrap();
    assert_eq!(row["body"], "");
    assert_eq!(row["body_bytes"], 0);
    assert_eq!(f.submit(&upload).await.unwrap()["result"], result["result"]);
    assert!(f.store.claim(&f.node, false).await.unwrap().is_none());
    let invitation = f.store.invite(&f.phone.workspace).await.unwrap();
    let paired = f
        .store
        .pair(s(&invitation, "pair_code"), "Second phone")
        .await
        .unwrap();
    let other = f
        .store
        .authenticate(s(&paired, "device_token"), Role::Device)
        .await
        .unwrap();
    let status = voice_command(&f.node.id, "voice_status", Some(s(&upload, "request_id")));
    f.store
        .submit(
            &other,
            &zerus_relay::store::Submission::new(status).unwrap(),
        )
        .await
        .unwrap();
    f.check_accounting().await;
    f.close().await;
}

#[test]
fn voice_profile_revision_is_optional_bounded_and_exact() {
    let mut request = voice_command(&Uuid::new_v4().to_string(), "voice_submit", None);
    protocol::request(&request).unwrap();
    request["payload"]["speech"]["profile_revision"] = json!(7);
    protocol::request(&request).unwrap();
    for revision in [
        json!(-1),
        json!(true),
        json!("7"),
        json!(9_007_199_254_740_992u64),
    ] {
        request["payload"]["speech"]["profile_revision"] = revision;
        assert!(protocol::request(&request).is_err());
    }
}

fn speech_config_command(computer: &str, op: &str, previous: Option<&str>) -> Value {
    let id = Uuid::new_v4().to_string();
    let owner = "00000000-0000-4000-8000-000000000001";
    let payload = match op {
        "speech_profile_upsert" => {
            json!({"request_id":id,"expected_machine_id":owner,"expected_revision":0,
            "profile":{"id":"fixture","name":"Fixture provider","adapter":"openai_http","model":"fixture",
                "language":"ru","base_url":"https://speech.example.invalid/v1","allow_insecure_http":false},
            "credential":{"mode":"keep"}})
        }
        "speech_config_status" => {
            json!({"operation_id":previous.unwrap(),"expected_machine_id":owner})
        }
        _ => json!({}),
    };
    json!({"request_id":id,"computer_id":computer,"operation":op,"session":"","payload":payload})
}

async fn speech_config_storage_contract(f: &Fixture, writer: &Store) {
    f.beat(&json!({"sessions":[],"mobile_capabilities":{"protocol_version":1,"operations":protocol::SPEECH}})).await;
    let change = speech_config_command(&f.node.id, "speech_profile_upsert", None);
    f.submit(&change).await.unwrap();
    assert!(writer.claim(&f.node, false).await.unwrap().is_some());
    assert!(f.store.claim(&f.node, false).await.unwrap().is_none());
    let result = json!({"schema":1,"machine_id":change["payload"]["expected_machine_id"],
        "request_id":change["request_id"],"state":"completed","revision":1});
    let reply = json!({"state":"completed","result":result,"error":null});
    writer
        .result(
            &f.node,
            s(&change, "request_id"),
            &reply,
            Some(&canonical(&result).unwrap()),
        )
        .await
        .unwrap();
    let row = f
        .store
        .db
        .one(
            "SELECT body,body_bytes FROM requests WHERE id=?",
            args![&change["request_id"]],
        )
        .await
        .unwrap()
        .unwrap();
    assert_eq!(row["body"], "");
    assert_eq!(row["body_bytes"], 0);
    assert_eq!(f.submit(&change).await.unwrap()["result"], result);
    assert!(writer.claim(&f.node, false).await.unwrap().is_none());
    let mut conflict = change.clone();
    conflict["payload"]["profile"]["name"] = json!("Changed");
    assert!(f.submit(&conflict).await.is_err());
    let status = speech_config_command(
        &f.node.id,
        "speech_config_status",
        Some(s(&change, "request_id")),
    );
    f.submit(&status).await.unwrap();
    assert!(writer.claim(&f.node, false).await.unwrap().is_some());
    writer
        .result(
            &f.node,
            s(&status, "request_id"),
            &reply,
            Some(&canonical(&result).unwrap()),
        )
        .await
        .unwrap();
    writer
        .db
        .exec(
            "UPDATE requests SET updated=? WHERE id=?",
            args![now() - 121.0, &status["request_id"]],
        )
        .await
        .unwrap();
    writer.maintain(false).await.unwrap();
    assert!(f
        .store
        .db
        .one(
            "SELECT id FROM requests WHERE id=?",
            args![&status["request_id"]]
        )
        .await
        .unwrap()
        .is_none());
    assert_eq!(f.submit(&change).await.unwrap()["result"], result);
    f.check_accounting().await;
}

#[tokio::test]
async fn speech_config_receipts_erase_bodies_and_expire_reads() {
    let f = Fixture::new().await;
    speech_config_storage_contract(&f, &f.store).await;
    f.close().await;
}

fn speech_command(computer: &str, op: &str, job: Option<&str>) -> Value {
    let id = Uuid::new_v4().to_string();
    let payload = match op {
        "speech_submit" => {
            json!({"request_id":id,"profile_id":"gigaam-ru","audio":{"format":"pcm_s16le","sample_rate":16000,"channels":1,"data_base64":"AAAAAA=="}})
        }
        "speech_status" | "speech_cancel" => json!({"job_id":job.unwrap()}),
        _ => json!({}),
    };
    json!({"request_id":id,"computer_id":computer,"operation":op,"session":"","payload":payload})
}

#[test]
fn speech_wire_rejects_unbounded_or_ambiguous_audio() {
    use base64::Engine;
    let computer = Uuid::new_v4().to_string();
    let b = speech_command(&computer, "speech_submit", None);
    protocol::request(&b).unwrap();
    for data in ["", "AA==", "AAAA", "AA==AA==", "AA B", "AAB="] {
        let mut bad = b.clone();
        bad["payload"]["audio"]["data_base64"] = json!(data);
        assert!(protocol::request(&bad).is_err(), "accepted {data}");
    }
    let mut full = b.clone();
    full["payload"]["audio"]["data_base64"] = json!(
        base64::engine::general_purpose::STANDARD.encode(vec![0; protocol::SPEECH_AUDIO_MAX])
    );
    protocol::request(&full).unwrap();
    full["payload"]["audio"]["data_base64"] =
        json!(base64::engine::general_purpose::STANDARD
            .encode(vec![0; protocol::SPEECH_AUDIO_MAX + 2]));
    assert!(protocol::request(&full).is_err());
    for (field, value) in [("sample_rate", json!(true)), ("channels", json!(2))] {
        let mut bad = b.clone();
        bad["payload"]["audio"][field] = value;
        assert!(protocol::request(&bad).is_err());
    }
    let mut bad = b.clone();
    bad["session"] = json!("codex/example/session");
    assert!(protocol::request(&bad).is_err());
    let mut bad = b;
    bad["payload"]["path"] = json!("/etc/passwd");
    assert!(protocol::request(&bad).is_err());
}

#[tokio::test]
async fn speech_jobs_preserve_owner_identity_and_discard_audio_after_ack() {
    let f = Fixture::new().await;
    f.beat(&json!({"sessions":[],"mobile_capabilities":{"protocol_version":1,"operations":protocol::SPEECH}})).await;
    let b = speech_command(&f.node.id, "speech_submit", None);
    f.submit(&b).await.unwrap();
    assert!(f.store.claim(&f.node, false).await.unwrap().is_some());
    let result = json!({"state":"completed","result":{"schema":1,"job_id":b["request_id"],"state":"queued"},"error":null});
    let encoded = canonical(&result["result"]).unwrap();
    f.store
        .result(&f.node, s(&b, "request_id"), &result, Some(&encoded))
        .await
        .unwrap();
    let row = f
        .store
        .db
        .one(
            "SELECT body,body_bytes FROM requests WHERE id=?",
            args![&b["request_id"]],
        )
        .await
        .unwrap()
        .unwrap();
    assert_eq!(row["body"], "");
    assert_eq!(row["body_bytes"], 0);
    assert_eq!(f.submit(&b).await.unwrap()["result"], result["result"]);
    let status = speech_command(&f.node.id, "speech_status", Some(s(&b, "request_id")));
    f.submit(&status).await.unwrap();
    let invitation = f.store.invite(&f.phone.workspace).await.unwrap();
    let paired = f
        .store
        .pair(s(&invitation, "pair_code"), "Second phone")
        .await
        .unwrap();
    let other = f
        .store
        .authenticate(s(&paired, "device_token"), Role::Device)
        .await
        .unwrap();
    let foreign = speech_command(&f.node.id, "speech_status", Some(s(&b, "request_id")));
    assert_eq!(
        f.store
            .submit(
                &other,
                &zerus_relay::store::Submission::new(foreign).unwrap()
            )
            .await
            .unwrap_err()
            .0
            .as_u16(),
        404
    );
    f.check_accounting().await;
    f.close().await;
}

#[tokio::test]
async fn speech_expired_upload_keeps_tombstone_without_audio() {
    let f = Fixture::new().await;
    f.beat(&json!({"sessions":[],"mobile_capabilities":{"protocol_version":1,"operations":protocol::SPEECH}})).await;
    let b = speech_command(&f.node.id, "speech_submit", None);
    f.submit(&b).await.unwrap();
    f.store
        .db
        .exec(
            "UPDATE requests SET created=? WHERE id=?",
            args![now() - 180.0, &b["request_id"]],
        )
        .await
        .unwrap();
    f.store.maintain(false).await.unwrap();
    let row = f
        .store
        .db
        .one(
            "SELECT body,body_bytes,state FROM requests WHERE id=?",
            args![&b["request_id"]],
        )
        .await
        .unwrap()
        .unwrap();
    assert_eq!(row["state"], "failed");
    assert_eq!(row["body"], "");
    assert_eq!(f.submit(&b).await.unwrap()["state"], "failed");
    f.check_accounting().await;
    f.close().await;
}

#[tokio::test]
async fn speech_status_retention_keeps_owner_receipt_and_exact_accounting() {
    let f = Fixture::new().await;
    f.beat(&json!({"sessions":[],"mobile_capabilities":{"protocol_version":1,"operations":protocol::SPEECH}})).await;
    let upload = speech_command(&f.node.id, "speech_submit", None);
    f.submit(&upload).await.unwrap();
    assert!(f.store.claim(&f.node, false).await.unwrap().is_some());
    let submitted = json!({"state":"completed","result":{"schema":1,"job_id":upload["request_id"],"state":"queued"},"error":null});
    f.store
        .result(
            &f.node,
            s(&upload, "request_id"),
            &submitted,
            Some(&canonical(&submitted["result"]).unwrap()),
        )
        .await
        .unwrap();
    let status = speech_command(&f.node.id, "speech_status", Some(s(&upload, "request_id")));
    f.submit(&status).await.unwrap();
    assert!(f.store.claim(&f.node, false).await.unwrap().is_some());
    let completed = json!({"state":"completed","result":{"schema":1,"job_id":upload["request_id"],"state":"completed","text":"Synthetic private dictation"},"error":null});
    f.store
        .result(
            &f.node,
            s(&status, "request_id"),
            &completed,
            Some(&canonical(&completed["result"]).unwrap()),
        )
        .await
        .unwrap();
    f.store
        .db
        .exec(
            "UPDATE requests SET updated=? WHERE id=?",
            args![now() - 121.0, &status["request_id"]],
        )
        .await
        .unwrap();
    f.store.maintain(false).await.unwrap();
    assert!(f
        .store
        .db
        .one(
            "SELECT id FROM requests WHERE id=?",
            args![&status["request_id"]]
        )
        .await
        .unwrap()
        .is_none());
    assert_eq!(
        f.submit(&upload).await.unwrap()["result"],
        submitted["result"]
    );
    let retry = speech_command(&f.node.id, "speech_status", Some(s(&upload, "request_id")));
    f.submit(&retry).await.unwrap();
    f.check_accounting().await;
    f.close().await;
}
fn presence(idle: f64, locked: Option<f64>) -> Value {
    json!({"idle_seconds":idle,"locked":locked.is_some(),"locked_seconds":locked})
}
fn signature(digit: char) -> String {
    digit.to_string().repeat(64)
}
fn session_row(
    phase: &str,
    activity: &str,
    attention: &str,
    signature: Option<&str>,
    read: Value,
) -> Value {
    json!({"name":"example","run_id":"r","conversation_id":"c","phase":phase,"activity":activity,"attention_id":attention,"attention_signature":signature,"read":read})
}
fn listing(session: Value) -> Value {
    json!({ "sessions": [session] })
}
async fn gateway_beat(f: &Fixture, root: &str, peers: &Value, snapshot: &Value) {
    f.store
        .heartbeat(
            &f.node,
            &json!({"snapshot":snapshot,"machine_id":root,"peers":peers,"presence":null}),
            &canonical(snapshot).unwrap(),
        )
        .await
        .unwrap();
}
async fn peer_beat(f: &Fixture, route: &str, machine: &str, snapshot: &Value, presence: &Value) {
    f.store
        .peer_heartbeat(
            &f.node,
            route,
            &json!({"machine_id":machine,"snapshot":snapshot,"presence":presence}),
            &canonical(snapshot).unwrap(),
        )
        .await
        .unwrap();
}
#[tokio::test]
async fn claims_and_receipts_never_replay() {
    let f = Fixture::new().await;
    let b = f.command("send");
    assert_eq!(f.submit(&b).await.unwrap()["state"], "queued");
    assert_eq!(f.submit(&b).await.unwrap()["state"], "queued");
    let mut changed = b.clone();
    changed["session"] = json!("other");
    assert_eq!(f.submit(&changed).await.unwrap_err().0.as_u16(), 409);
    let (a, c) = tokio::join!(f.store.claim(&f.node, false), f.store.claim(&f.node, false));
    assert_eq!(
        usize::from(a.unwrap().is_some()) + usize::from(c.unwrap().is_some()),
        1
    );
    assert_eq!(f.submit(&b).await.unwrap()["state"], "claimed");
    let result = json!({"state":"completed","result":{"ok":true,"unicode":"😀"},"error":null});
    let e = canonical(&result["result"]).unwrap();
    f.store
        .result(&f.node, s(&b, "request_id"), &result, Some(&e))
        .await
        .unwrap();
    f.store
        .result(&f.node, s(&b, "request_id"), &result, Some(&e))
        .await
        .unwrap();
    assert_eq!(
        f.store
            .receipt(&f.phone, s(&b, "request_id"))
            .await
            .unwrap()["result"],
        result["result"]
    );
    f.check_accounting().await;
    f.store
        .db
        .exec(
            "UPDATE requests SET updated=? WHERE id=?",
            args![now() - 8.0 * 86400.0, &b["request_id"]],
        )
        .await
        .unwrap();
    f.store.maintain(false).await.unwrap();
    assert_eq!(f.submit(&b).await.unwrap()["state"], "completed");
    assert!(f.store.claim(&f.node, false).await.unwrap().is_none());
    f.check_accounting().await;
    f.close().await;
}
#[tokio::test]
async fn mark_read_is_read_class_and_pruned_like_reads() {
    assert!(protocol::READS.contains(&"mark_read"));
    assert!(protocol::READ_SQL.contains("'mark_read'"));
    let f = Fixture::new().await;
    let snapshot = json!({"sessions":[],"mobile_capabilities":{"protocol_version":1,"operations":["mark_read"],"features":[]}});
    f.store
        .heartbeat(
            &f.node,
            &json!({"snapshot":snapshot}),
            &canonical(&snapshot).unwrap(),
        )
        .await
        .unwrap();
    let mut b = f.command("mark_read");
    b["payload"] = json!({"run_id":"run","conversation_id":"conversation","reply_id":"7:100.5","attention_signature":null});
    assert_eq!(f.submit(&b).await.unwrap()["state"], "queued");
    assert!(f.store.claim(&f.node, false).await.unwrap().is_some());
    let result = json!({"state":"completed","result":{"ok":true},"error":null});
    let e = canonical(&result["result"]).unwrap();
    f.store
        .result(&f.node, s(&b, "request_id"), &result, Some(&e))
        .await
        .unwrap();
    f.check_accounting().await;
    f.store
        .db
        .exec(
            "UPDATE requests SET updated=? WHERE id=?",
            args![now() - 3601.0, &b["request_id"]],
        )
        .await
        .unwrap();
    f.store.maintain(false).await.unwrap();
    // A pruned read identity is new again; a mutation would return its completed receipt.
    assert_eq!(f.submit(&b).await.unwrap()["state"], "queued");
    f.check_accounting().await;
    f.close().await;
}
#[tokio::test]
async fn expired_claims_and_revocation_are_terminal() {
    let f = Fixture::new().await;
    let b = f.command("send");
    f.submit(&b).await.unwrap();
    f.store.claim(&f.node, false).await.unwrap();
    f.store
        .db
        .exec(
            "UPDATE requests SET claimed=? WHERE id=?",
            args![now() - 91.0, &b["request_id"]],
        )
        .await
        .unwrap();
    let result = json!({"state":"completed","result":null,"error":null});
    assert_eq!(
        f.store
            .result(&f.node, s(&b, "request_id"), &result, None)
            .await
            .unwrap_err()
            .0
            .as_u16(),
        409
    );
    assert_eq!(
        f.store
            .receipt(&f.phone, s(&b, "request_id"))
            .await
            .unwrap()["state"],
        "uncertain"
    );
    let next = f.command("send");
    f.submit(&next).await.unwrap();
    f.store.revoke(Role::Device, &f.phone.id).await.unwrap();
    assert!(f.store.claim(&f.node, false).await.unwrap().is_none());
    assert!(f
        .store
        .authenticate(s(&f.paired, "device_token"), Role::Device)
        .await
        .is_err());
    f.check_accounting().await;
    f.close().await;
}
/// The sole SQLite owner retires its claims on restart and keeps them uncertain.
/// A PostgreSQL worker never retires another worker's claim before its deadline.
#[tokio::test]
async fn restart_retires_claims_only_on_sqlite() {
    let f = Fixture::new().await;
    let b = f.command("send");
    f.submit(&b).await.unwrap();
    f.store.claim(&f.node, false).await.unwrap();
    f.store.maintain(true).await.unwrap();
    let state = if f.backend.is_postgres() {
        "claimed"
    } else {
        "uncertain"
    };
    assert_eq!(f.submit(&b).await.unwrap()["state"], state);
    f.check_accounting().await;
    f.close().await;
}
#[tokio::test]
async fn startup_defers_alert_pass_failure_but_not_record_maintenance() {
    let f = Fixture::new().await;
    let b = f.command("send");
    f.submit(&b).await.unwrap();
    f.store.claim(&f.node, false).await.unwrap();
    // A failing alert pass (here its table is gone) must not stop a worker from starting.
    f.store
        .db
        .exec("DROP TABLE alert_candidates", &[])
        .await
        .unwrap();
    assert!(f.store.maintain(false).await.is_err());
    // An expired claim, which record maintenance retires on either backend.
    f.store
        .db
        .exec(
            "UPDATE requests SET claimed=? WHERE id=?",
            args![now() - 91.0, &b["request_id"]],
        )
        .await
        .unwrap();
    f.store.maintain_at_startup(true).await.unwrap();
    // The record stages before it still ran. The stored row shows it, since a
    // receipt would also expire the claim itself.
    let row = f
        .store
        .db
        .one(
            "SELECT state FROM requests WHERE id=?",
            args![&b["request_id"]],
        )
        .await
        .unwrap()
        .unwrap();
    assert_eq!(row["state"], "uncertain");
    f.check_accounting().await;
    // Record maintenance failures stay fatal at startup.
    f.store
        .db
        .exec("DROP TABLE rate_limits", &[])
        .await
        .unwrap();
    assert!(f.store.maintain_at_startup(false).await.is_err());
    f.close().await;
}
#[tokio::test]
async fn legacy_oversized_receipt_does_not_change_delivery_identity() {
    let f = Fixture::new().await;
    let b = f.command("send");
    f.submit(&b).await.unwrap();
    f.store.claim(&f.node, false).await.unwrap();
    let large = format!("\"{}\"", "x".repeat(2 * protocol::MIB));
    f.store
        .db
        .exec(
            "UPDATE requests SET state='completed',result=? WHERE id=?",
            args![&large, &b["request_id"]],
        )
        .await
        .unwrap();
    assert_eq!(
        f.store
            .receipt(&f.phone, s(&b, "request_id"))
            .await
            .unwrap_err()
            .0
            .as_u16(),
        413
    );
    assert_eq!(f.submit(&b).await.unwrap_err().0.as_u16(), 413);
    assert!(f.store.claim(&f.node, false).await.unwrap().is_none());
    let row = f
        .store
        .db
        .one(
            "SELECT state,body_hash FROM requests WHERE id=?",
            args![&b["request_id"]],
        )
        .await
        .unwrap()
        .unwrap();
    assert_eq!(row["state"], "completed");
    assert_eq!(row["body_hash"], digest(canonical(&b).unwrap()));
    f.close().await;
}
#[tokio::test]
async fn budgets_reserve_results_and_release_on_expiry() {
    let f = Fixture::config(Config {
        background: false,
        max_queue: 1,
        ..Config::default()
    })
    .await;
    let b = f.command("send");
    f.submit(&b).await.unwrap();
    assert_eq!(
        f.submit(&f.command("send")).await.unwrap_err().0.as_u16(),
        429
    );
    f.store
        .db
        .exec(
            "UPDATE requests SET created=? WHERE id=?",
            args![now() - 121.0, &b["request_id"]],
        )
        .await
        .unwrap();
    assert_eq!(
        f.store
            .receipt(&f.phone, s(&b, "request_id"))
            .await
            .unwrap()["state"],
        "failed"
    );
    f.submit(&f.command("send")).await.unwrap();
    f.check_accounting().await;
    f.close().await;
}
#[tokio::test]
async fn scoped_pairing_and_rates() {
    let f = Fixture::new().await;
    assert!(f
        .store
        .pair(s(&f.credentials, "pair_code"), "Twice")
        .await
        .is_err());
    assert!(f
        .store
        .authenticate(s(&f.credentials, "node_token"), Role::Device)
        .await
        .is_err());
    for _ in 0..10 {
        f.store
            .rate(&json!(["pair", "127.0.0.1"]), 10, true)
            .await
            .unwrap();
    }
    assert!(f
        .store
        .rate(&json!(["pair", "127.0.0.1"]), 10, true)
        .await
        .is_err());
    f.store
        .rate(
            &json!(["devices", f.phone.workspace, f.phone.id]),
            2400,
            false,
        )
        .await
        .unwrap();
    f.close().await;
}
#[tokio::test]
async fn poll_leases_have_separate_capacity_and_idempotent_release() {
    let f = Fixture::new().await;
    let a = f.store.acquire_poll(&f.phone, 35.0).await.unwrap();
    let b = f.store.acquire_poll(&f.phone, 35.0).await.unwrap();
    assert_eq!(
        f.store
            .acquire_poll(&f.phone, 35.0)
            .await
            .unwrap_err()
            .0
            .as_u16(),
        429
    );
    f.store.release_poll(&a).await.unwrap();
    f.store.release_poll(&a).await.unwrap();
    f.store.release_poll(&b).await.unwrap();
    let r = f
        .store
        .db
        .one(
            "SELECT active_polls FROM workspace_usage WHERE workspace_id=?",
            args![&f.phone.workspace],
        )
        .await
        .unwrap()
        .unwrap();
    assert_eq!(r["active_polls"], 0);
    f.close().await;
}
#[tokio::test]
async fn one_hop_routes_freeze_and_withdraw() {
    let f = Fixture::new().await;
    let root = Uuid::new_v4().to_string();
    let machine = Uuid::new_v4().to_string();
    let route = Uuid::new_v4().to_string();
    let snapshot = json!({"sessions":[],"mobile_capabilities":{"protocol_version":1,"operations":protocol::OPERATIONS,"features":["gateway_one_hop"]}});
    let mut heartbeat = json!({"snapshot":snapshot,"machine_id":root,"peers":[{"route_id":route,"machine_id":machine,"name":"Peer","online":true}]});
    f.store
        .heartbeat(&f.node, &heartbeat, &canonical(&snapshot).unwrap())
        .await
        .unwrap();
    let peer = json!({"machine_id":machine,"snapshot":snapshot});
    f.store
        .peer_heartbeat(&f.node, &route, &peer, &canonical(&snapshot).unwrap())
        .await
        .unwrap();
    let catalog = f.store.computers(&f.phone).await.unwrap();
    let peer = catalog["computers"]
        .as_array()
        .unwrap()
        .iter()
        .find(|r| r["machine_id"] == machine)
        .unwrap();
    assert_eq!(peer["via"]["gateway_id"], f.node.id);
    // A peer's appearance never creates another queue credential.
    assert_eq!(f.count("nodes").await, 1);
    let mut b = f.command("send");
    b["computer_id"] = peer["id"].clone();
    f.submit(&b).await.unwrap();
    assert!(f.store.claim(&f.node, false).await.unwrap().is_none());
    let claim = f
        .store
        .claim(&f.node, true)
        .await
        .unwrap()
        .unwrap()
        .decode()
        .unwrap();
    assert_eq!(claim["requests"][0]["gateway_route"]["machine_id"], machine);
    heartbeat["peers"] = json!([]);
    f.store
        .heartbeat(&f.node, &heartbeat, &canonical(&snapshot).unwrap())
        .await
        .unwrap();
    assert_eq!(
        f.store
            .receipt(&f.phone, s(&b, "request_id"))
            .await
            .unwrap()["state"],
        "uncertain"
    );
    f.check_accounting().await;
    f.close().await;
}
#[tokio::test]
async fn offline_peer_route_stays_listed_but_unavailable() {
    let f = Fixture::new().await;
    let root = Uuid::new_v4().to_string();
    let machine = Uuid::new_v4().to_string();
    let route = Uuid::new_v4().to_string();
    let snapshot = json!({"sessions":[],"mobile_capabilities":{"protocol_version":1,"operations":protocol::OPERATIONS,"features":["gateway_one_hop"]}});
    let peers = |online: bool| json!([{"route_id":route,"machine_id":machine,"name":"Peer","online":online}]);
    let listed = |catalog: &Value| {
        catalog["computers"]
            .as_array()
            .unwrap()
            .iter()
            .find(|r| r["machine_id"] == machine)
            .cloned()
            .unwrap()
    };
    gateway_beat(&f, &root, &peers(true), &snapshot).await;
    peer_beat(&f, &route, &machine, &snapshot, &Value::Null).await;
    let online = listed(&f.store.computers(&f.phone).await.unwrap());
    assert_eq!(online["online"], true);
    gateway_beat(&f, &root, &peers(false), &snapshot).await;
    let offline = listed(&f.store.computers(&f.phone).await.unwrap());
    assert_eq!(offline["id"], online["id"]);
    assert_eq!(offline["online"], false);
    assert_eq!(offline["via"]["gateway_id"], f.node.id);
    assert!(offline["snapshot"].is_null());
    let mut b = f.command("send");
    b["computer_id"] = offline["id"].clone();
    assert_eq!(f.submit(&b).await.unwrap_err().0.as_u16(), 409);
    let body = json!({"machine_id":machine,"snapshot":snapshot});
    assert_eq!(
        f.store
            .peer_heartbeat(&f.node, &route, &body, &canonical(&snapshot).unwrap())
            .await
            .unwrap_err()
            .0
            .as_u16(),
        409
    );
    assert!(f.store.claim(&f.node, true).await.unwrap().is_none());
    // Reconnection needs a fresh peer observation before the route is usable.
    gateway_beat(&f, &root, &peers(true), &snapshot).await;
    assert_eq!(
        listed(&f.store.computers(&f.phone).await.unwrap())["online"],
        false
    );
    peer_beat(&f, &route, &machine, &snapshot, &Value::Null).await;
    let restored = listed(&f.store.computers(&f.phone).await.unwrap());
    assert_eq!(
        (restored["id"].clone(), restored["online"].clone()),
        (online["id"].clone(), json!(true))
    );
    f.close().await;
}
/// An offline peer route reports its last accepted peer heartbeat as
/// `last_seen_at`. The retained time is display-only: gateway heartbeats never
/// refresh it, it never makes a reconnected route usable, and a changed native
/// identity on the same route never inherits it. `reader` lists the catalog,
/// which may be another worker sharing the database.
async fn offline_peer_route_keeps_last_seen(f: &Fixture, reader: &Store) {
    let root = Uuid::new_v4().to_string();
    let machine = Uuid::new_v4().to_string();
    let route = Uuid::new_v4().to_string();
    let snapshot = json!({"sessions":[],"mobile_capabilities":{"protocol_version":1,"operations":protocol::OPERATIONS,"features":["gateway_one_hop"]}});
    let changed =
        json!({"sessions":[],"sequence":2,"mobile_capabilities":snapshot["mobile_capabilities"]});
    let peers = |machine: &str, online: bool| json!([{"route_id":route,"machine_id":machine,"name":"Peer","online":online}]);
    let listed = |machine: String| async move {
        reader.computers(&f.phone).await.unwrap()["computers"]
            .as_array()
            .unwrap()
            .iter()
            .find(|r| r["machine_id"] == machine.as_str())
            .cloned()
    };
    // A remembered identity that this relay never observed has no time.
    gateway_beat(f, &root, &peers(&machine, false), &snapshot).await;
    let never = listed(machine.clone()).await.unwrap();
    assert_eq!(never["online"], false);
    assert!(never["last_seen_at"].is_null());
    gateway_beat(f, &root, &peers(&machine, true), &snapshot).await;
    peer_beat(f, &route, &machine, &snapshot, &Value::Null).await;
    let online = listed(machine.clone()).await.unwrap();
    assert_eq!(online["online"], true);
    let seen = online["last_seen_at"].as_f64().unwrap();
    gateway_beat(f, &root, &peers(&machine, false), &snapshot).await;
    // Fresh gateway liveness is not evidence about the peer.
    gateway_beat(f, &root, &peers(&machine, false), &changed).await;
    let offline = listed(machine.clone()).await.unwrap();
    assert_eq!(offline["id"], online["id"]);
    assert_eq!(offline["online"], false);
    assert!(offline["snapshot"].is_null());
    assert_eq!(offline["via"]["gateway_id"], f.node.id);
    assert_eq!(offline["last_seen_at"].as_f64(), Some(seen));
    // Marked online again, the route still needs a fresh peer heartbeat even
    // though the retained time is within the freshness window.
    gateway_beat(f, &root, &peers(&machine, true), &changed).await;
    let marked = listed(machine.clone()).await.unwrap();
    assert_eq!(marked["online"], false);
    assert_eq!(marked["last_seen_at"].as_f64(), Some(seen));
    let mut b = f.command("send");
    b["computer_id"] = marked["id"].clone();
    assert_eq!(f.submit(&b).await.unwrap_err().0.as_u16(), 409);
    assert!(f.store.claim(&f.node, true).await.unwrap().is_none());
    peer_beat(f, &route, &machine, &snapshot, &Value::Null).await;
    let restored = listed(machine.clone()).await.unwrap();
    assert_eq!(restored["online"], true);
    assert!(restored["last_seen_at"].as_f64().unwrap() >= seen);
    // A changed verified identity replaces the route without its history.
    let replacement = Uuid::new_v4().to_string();
    gateway_beat(f, &root, &peers(&replacement, false), &changed).await;
    assert!(listed(machine).await.is_none());
    let replaced = listed(replacement).await.unwrap();
    assert_eq!(replaced["online"], false);
    assert!(replaced["last_seen_at"].is_null());
    f.check_accounting().await;
}
#[tokio::test]
async fn offline_peer_route_reports_last_peer_heartbeat() {
    let f = Fixture::new().await;
    offline_peer_route_keeps_last_seen(&f, &f.store).await;
    f.close().await;
}
#[tokio::test]
async fn heartbeats_store_presence_outside_the_snapshot() {
    let empty = json!({"sessions":[]});
    assert!(protocol::heartbeat(&json!({"snapshot":empty,"presence":null})).is_ok());
    assert!(protocol::heartbeat(&json!({"snapshot":empty,"presence":presence(1.0, None)})).is_ok());
    assert!(protocol::heartbeat(&json!({"snapshot":empty,"presence":"idle"})).is_err());
    let machine = Uuid::new_v4().to_string();
    assert!(protocol::peer_heartbeat(
        &json!({"machine_id":machine,"snapshot":empty,"presence":presence(1.0, None)})
    )
    .is_ok());
    assert!(protocol::peer_heartbeat(
        &json!({"machine_id":machine,"snapshot":empty,"presence":[]})
    )
    .is_err());
    let f = Fixture::new().await;
    let snapshot = json!({"sessions":[],"preferences":{"mobile_delivery":"away_10"}});
    let before = now();
    f.beat_with(&snapshot, &presence(30.0, None)).await;
    let row = f
        .node_row("presence_at,presence_until,mobile_delivery")
        .await;
    let at = row["presence_at"].as_f64().unwrap();
    assert!(at >= before && at <= now());
    assert!((row["presence_until"].as_f64().unwrap() - (at - 30.0 + 120.0)).abs() < 1e-6);
    assert_eq!(row["mobile_delivery"], "away_10");
    // A presence-only change is remembered without rewriting the unchanged snapshot.
    f.store
        .db
        .exec(
            "UPDATE nodes SET snapshot='{}' WHERE id=?",
            args![&f.node.id],
        )
        .await
        .unwrap();
    f.beat_with(&snapshot, &presence(5.0, Some(1.0))).await;
    let row = f.node_row("snapshot,presence_at,presence_until").await;
    assert_eq!(row["snapshot"], "{}");
    let locked = row["presence_at"].as_f64().unwrap();
    assert!(
        locked >= at && (row["presence_until"].as_f64().unwrap() - (locked - 1.0)).abs() < 1e-6
    );
    // Null, absent and malformed presence keep the remembered value.
    let remembered = f.node_row("presence_at,presence_until").await;
    f.beat_with(&snapshot, &Value::Null).await;
    f.beat(&snapshot).await;
    f.beat_with(
        &json!({"sessions":[],"preferences":{"mobile_delivery":"away_45"}}),
        &json!({"idle_seconds":"5","locked":false}),
    )
    .await;
    assert_eq!(f.node_row("presence_at,presence_until").await, remembered);
    assert!(f.node_row("mobile_delivery").await["mobile_delivery"].is_null());
    f.close().await;
}
#[tokio::test]
async fn presence_holds_read_drops_and_lock_delivers_alert() {
    let f = Fixture::new().await;
    let features = protocol::capabilities(vec![])["features"].clone();
    assert!(features
        .as_array()
        .unwrap()
        .iter()
        .any(|v| v == "presence_alerts"));
    f.store
        .register_push(&f.phone, "fcm", "synthetic-token")
        .await
        .unwrap();
    let active = presence(3.0, None);
    let (first, second) = (signature('1'), signature('2'));
    f.beat_with(
        &listing(session_row("working", "busy", "", None, Value::Null)),
        &active,
    )
    .await;
    f.beat_with(
        &listing(session_row(
            "input",
            "busy",
            "q1",
            Some(&first),
            Value::Null,
        )),
        &active,
    )
    .await;
    assert_eq!(f.events_of("attention").await.len(), 1);
    assert_eq!(
        (
            f.count("alert_candidates").await,
            f.alert_count().await,
            f.count("push_jobs").await
        ),
        (1, 0, 0)
    );
    // Reading the question on a desktop drops the held candidate.
    let read = json!({"conversation_id":"c","reply_id":null,"attention_signature":first,"at":1.0});
    f.beat_with(
        &listing(session_row(
            "input",
            "busy",
            "q1",
            Some(&first),
            read.clone(),
        )),
        &active,
    )
    .await;
    assert_eq!(f.count("alert_candidates").await, 0);
    // A replacement question waits while the owner is active.
    let waiting = listing(session_row("input", "busy", "q2", Some(&second), read));
    f.beat_with(&waiting, &active).await;
    assert_eq!(f.count("alert_candidates").await, 1);
    // Identical snapshot content: the lock alone arrives and delivers.
    f.beat_with(&waiting, &presence(4.0, Some(2.0))).await;
    let alerts = f.events_of("alert").await;
    assert_eq!(alerts.len(), 1);
    assert_eq!(
        (
            alerts[0]["alert_kind"].as_str(),
            alerts[0]["session"].as_str(),
            alerts[0]["computer_id"].as_str()
        ),
        (Some("attention"), Some("example"), Some(f.node.id.as_str()))
    );
    assert!(f
        .events_of("attention")
        .await
        .iter()
        .all(|e| e.get("alert_kind").is_none()));
    let jobs = f.store.claim_push_jobs().await.unwrap();
    assert_eq!(jobs.len(), 1);
    assert_eq!(
        serde_json::from_str::<Value>(s(&jobs[0], "payload")).unwrap(),
        json!({"event_id":alerts[0]["id"],"kind":"wake"})
    );
    assert_eq!(f.count("alert_candidates").await, 0);
    f.close().await;
}
#[tokio::test]
async fn unannounced_signature_change_keeps_held_candidate() {
    let f = Fixture::new().await;
    let active = presence(1.0, None);
    let question = session_row("input", "busy", "q1", Some(&signature('1')), Value::Null);
    f.beat_with(
        &listing(session_row("working", "busy", "", None, Value::Null)),
        &active,
    )
    .await;
    f.beat_with(&listing(question.clone()), &active).await;
    // A subagent asks too: hgs changes the signature, the relay emits nothing.
    let mut changed = question;
    changed["subagents"] = json!({"a1":{"state":"approval","attention_id":"sub1"}});
    changed["attention_signature"] = json!(signature('2'));
    f.beat_with(&listing(changed.clone()), &active).await;
    assert_eq!(f.events_of("attention").await.len(), 1);
    assert_eq!(
        (f.count("alert_candidates").await, f.alert_count().await),
        (1, 0)
    );
    f.beat_with(&listing(changed), &presence(2.0, Some(1.0)))
        .await;
    assert_eq!(
        (f.count("alert_candidates").await, f.alert_count().await),
        (0, 1)
    );
    f.close().await;
}
#[tokio::test]
async fn immediate_preference_ignores_active_desktop() {
    let f = Fixture::new().await;
    let snapshot = |phase: &str| json!({"sessions":[session_row(phase,"busy","q1",Some(&signature('1')),Value::Null)],"preferences":{"mobile_delivery":"immediate"}});
    f.beat_with(&snapshot("working"), &presence(1.0, None))
        .await;
    f.beat_with(&snapshot("input"), &presence(1.0, None)).await;
    assert_eq!(
        (f.alert_count().await, f.count("alert_candidates").await),
        (1, 0)
    );
    f.close().await;
}
#[tokio::test]
async fn completed_candidates_follow_reply_read_and_busy_state() {
    let f = Fixture::new().await;
    let active = presence(1.0, None);
    let none = Value::Null;
    let turn = |activity: &str, reply: &str, read: &Value| {
        listing(
            json!({"name":"example","run_id":"r","conversation_id":"c","phase":"idle","activity":activity,"reply_id":reply,"read":read}),
        )
    };
    f.beat_with(&turn("busy", "rp0", &none), &active).await;
    f.beat_with(&turn("idle", "rp1", &none), &active).await;
    assert_eq!(f.count("alert_candidates").await, 1);
    f.beat_with(&turn("busy", "rp1", &none), &active).await; // busy again
    assert_eq!(f.count("alert_candidates").await, 0);
    f.beat_with(&turn("idle", "rp2", &none), &active).await;
    let read = json!({"conversation_id":"c","reply_id":"rp2","attention_signature":null,"at":1.0});
    f.beat_with(&turn("idle", "rp2", &read), &active).await; // read on a desktop
    assert_eq!(f.count("alert_candidates").await, 0);
    f.beat_with(&turn("busy", "rp2", &read), &active).await;
    f.beat_with(&turn("idle", "rp3", &read), &active).await;
    f.beat_with(&turn("idle", "rp4", &read), &active).await; // a newer reply supersedes it
    assert_eq!(f.count("alert_candidates").await, 0);
    f.beat_with(&turn("busy", "rp4", &read), &active).await;
    f.beat_with(&turn("idle", "rp5", &read), &active).await;
    f.beat_with(&turn("idle", "rp5", &read), &presence(1.0, Some(0.0)))
        .await;
    let kinds: Vec<_> = f
        .events_of("alert")
        .await
        .iter()
        .map(|e| e["alert_kind"].as_str().unwrap().to_owned())
        .collect();
    assert_eq!(kinds, ["completed"]);
    f.close().await;
}
#[tokio::test]
async fn peer_desktop_presence_holds_gateway_alerts() {
    let f = Fixture::new().await;
    let (root, machine, route) = (
        Uuid::new_v4().to_string(),
        Uuid::new_v4().to_string(),
        Uuid::new_v4().to_string(),
    );
    let caps = json!({"protocol_version":1,"operations":protocol::OPERATIONS,"features":["gateway_one_hop"]});
    let peers = json!([{"route_id":route,"machine_id":machine,"name":"Peer","online":true}]);
    // The gateway GUI is closed (presence null); the owner works at the peer.
    let gateway = |phase: &str| json!({"sessions":[session_row(phase,"busy","q1",Some(&signature('1')),Value::Null)],"mobile_capabilities":caps});
    let peer = json!({"sessions":[],"mobile_capabilities":caps});
    gateway_beat(&f, &root, &peers, &gateway("working")).await;
    peer_beat(&f, &route, &machine, &peer, &presence(1.0, None)).await;
    gateway_beat(&f, &root, &peers, &gateway("input")).await;
    assert_eq!(
        (f.count("alert_candidates").await, f.alert_count().await),
        (1, 0)
    );
    peer_beat(&f, &route, &machine, &peer, &presence(2.0, Some(1.0))).await;
    let alerts = f.events_of("alert").await;
    assert_eq!(alerts.len(), 1);
    assert_eq!(alerts[0]["computer_id"], f.node.id);
    f.close().await;
}
#[tokio::test]
async fn older_connectors_forward_presence_inside_the_snapshot() {
    let f = Fixture::new().await;
    let with = |p: Value| json!({"sessions":[],"desktop_presence":p});
    let before = now();
    f.beat(&with(presence(30.0, None))).await;
    let row = f.node_row("presence_at,presence_until,snapshot").await;
    let at = row["presence_at"].as_f64().unwrap();
    assert!(at >= before && at <= now());
    assert!((row["presence_until"].as_f64().unwrap() - (at - 30.0 + 120.0)).abs() < 1e-6);
    // The stored snapshot stays verbatim.
    assert!(row["snapshot"]
        .as_str()
        .unwrap()
        .contains("desktop_presence"));
    // Malformed embedded presence keeps the previous values.
    let remembered = f.node_row("presence_at,presence_until").await;
    f.beat(&with(json!({"idle_seconds":"5","locked":false})))
        .await;
    assert_eq!(f.node_row("presence_at,presence_until").await, remembered);
    // A valid heartbeat field wins over the embedded fallback.
    f.beat_with(&with(presence(30.0, None)), &presence(5.0, Some(1.0)))
        .await;
    let row = f.node_row("presence_at,presence_until").await;
    let at = row["presence_at"].as_f64().unwrap();
    assert!((row["presence_until"].as_f64().unwrap() - (at - 1.0)).abs() < 1e-6);
    f.close().await;
}
#[tokio::test]
async fn away_minutes_preference_waits_from_end_of_activity() {
    let f = Fixture::new().await;
    // Input stopped 200 s ago: away for 80 s, so `away_5` waits another 220 s.
    let snapshot = |phase: &str| json!({"sessions":[session_row(phase,"busy","q1",Some(&signature('1')),Value::Null)],"preferences":{"mobile_delivery":"away_5"}});
    f.beat_with(&snapshot("working"), &presence(200.0, None))
        .await;
    f.beat_with(&snapshot("input"), &presence(200.0, None))
        .await;
    f.store.maintain(false).await.unwrap();
    assert_eq!(
        (f.count("alert_candidates").await, f.alert_count().await),
        (1, 0)
    );
    let next = f
        .store
        .db
        .one("SELECT next_at FROM alert_candidates", &[])
        .await
        .unwrap()
        .unwrap();
    assert!(next["next_at"].as_f64().unwrap() <= now() + 15.0);
    // Five away minutes later the maintenance pass delivers it.
    f.store
        .db
        .exec("UPDATE nodes SET presence_until=presence_until-220", &[])
        .await
        .unwrap();
    f.store
        .db
        .exec("UPDATE alert_candidates SET next_at=0", &[])
        .await
        .unwrap();
    f.store.maintain(false).await.unwrap();
    assert_eq!(
        (f.count("alert_candidates").await, f.alert_count().await),
        (0, 1)
    );
    f.close().await;
}
#[tokio::test]
async fn stale_presence_keeps_last_activity_for_away_minutes() {
    let f = Fixture::new().await;
    let snapshot = |phase: &str| json!({"sessions":[session_row(phase,"busy","q1",Some(&signature('1')),Value::Null)],"preferences":{"mobile_delivery":"away_10"}});
    // The lid closes and locks the screen; then heartbeats stop.
    f.beat_with(&snapshot("working"), &presence(0.0, Some(0.0)))
        .await;
    f.beat_with(&snapshot("input"), &presence(0.0, Some(0.0)))
        .await;
    let five_minutes =
        "UPDATE nodes SET presence_at=presence_at-300,presence_until=presence_until-300";
    f.store.db.exec(five_minutes, &[]).await.unwrap();
    f.store
        .db
        .exec("UPDATE alert_candidates SET next_at=0", &[])
        .await
        .unwrap();
    f.store.maintain(false).await.unwrap();
    // Stale presence is not present, but only five of ten away minutes have passed.
    assert_eq!(
        (f.count("alert_candidates").await, f.alert_count().await),
        (1, 0)
    );
    f.store.db.exec(five_minutes, &[]).await.unwrap();
    f.store
        .db
        .exec("UPDATE alert_candidates SET next_at=0", &[])
        .await
        .unwrap();
    f.store.maintain(false).await.unwrap();
    assert_eq!(
        (f.count("alert_candidates").await, f.alert_count().await),
        (0, 1)
    );
    f.close().await;
}
#[tokio::test]
async fn maintenance_recovers_unevaluated_candidates() {
    let f = Fixture::new().await;
    f.beat(&json!({"sessions":[{"name":"example","run_id":"r","conversation_id":"c","phase":"input","activity":"busy","attention_id":"q1"}]})).await;
    // A committed candidate whose post-snapshot evaluation never ran (worker exit or BUSY).
    f.store.db.exec("INSERT INTO alert_candidates(workspace_id,computer_id,session,kind,run_id,conversation_id,created,next_at) VALUES(?,?,'example','attention','r','c',?,?)", args![&f.node.workspace, &f.node.id, now(), now()]).await.unwrap();
    f.store.maintain(false).await.unwrap();
    let alerts = f.events_of("alert").await;
    assert_eq!(alerts.len(), 1);
    assert_eq!(alerts[0]["alert_kind"], "attention");
    assert_eq!(f.count("alert_candidates").await, 0);
    f.close().await;
}
#[tokio::test]
async fn revoked_computer_drops_held_candidate() {
    let f = Fixture::new().await;
    let active = presence(1.0, None);
    f.beat_with(
        &listing(session_row(
            "working",
            "busy",
            "q1",
            Some(&signature('1')),
            Value::Null,
        )),
        &active,
    )
    .await;
    f.beat_with(
        &listing(session_row(
            "input",
            "busy",
            "q1",
            Some(&signature('1')),
            Value::Null,
        )),
        &active,
    )
    .await;
    assert_eq!(f.count("alert_candidates").await, 1);
    f.store.revoke_computer(&f.node.id).await.unwrap();
    f.store
        .db
        .exec("UPDATE alert_candidates SET next_at=0", &[])
        .await
        .unwrap();
    f.store.maintain(false).await.unwrap();
    assert_eq!(
        (f.count("alert_candidates").await, f.alert_count().await),
        (0, 0)
    );
    f.close().await;
}
#[tokio::test]
async fn alert_candidates_are_bounded_and_expire() {
    let f = Fixture::new().await;
    let fleet = |phase: &str| json!({"sessions":(0..1001).map(|k| json!({"name":format!("s{k}"),"run_id":"r","conversation_id":"c","phase":phase})).collect::<Vec<_>>()});
    f.beat_with(&fleet("working"), &presence(1.0, None)).await;
    f.beat_with(&fleet("input"), &presence(1.0, None)).await;
    assert_eq!(f.count("alert_candidates").await, 1000);
    assert_eq!(f.count("alert_candidates WHERE session='s0'").await, 0);
    assert_eq!(f.alert_count().await, 0);
    f.store
        .db
        .exec("UPDATE alert_candidates SET created=created-86401", &[])
        .await
        .unwrap();
    f.store.maintain(false).await.unwrap();
    assert_eq!(f.count("alert_candidates").await, 1000 - 256);
    for _ in 0..3 {
        f.store.maintain(false).await.unwrap();
    }
    assert_eq!(
        (f.count("alert_candidates").await, f.alert_count().await),
        (0, 0)
    );
    f.close().await;
}
#[tokio::test]
async fn maintenance_rechecks_most_overdue_workspaces_first() {
    let f = Fixture::new().await;
    // More waiting workspaces than one pass takes (32): owners at their desks
    // keep 33 of them waiting, so every pass is full.
    let mut spaces = vec![];
    for _ in 0..35 {
        let c = f
            .store
            .provision("Example workspace", "Example computer")
            .await
            .unwrap();
        spaces.push((
            s(&c, "workspace_id").to_owned(),
            s(&c, "node_id").to_owned(),
        ));
    }
    spaces.sort();
    let (expired, asleep) = (&spaces[33], &spaces[34]);
    let at = now();
    let insert = "INSERT INTO alert_candidates(workspace_id,computer_id,session,kind,run_id,conversation_id,created,next_at) VALUES(?,?,'example','attention','r','c',?,?)";
    for (w, node) in &spaces[..33] {
        f.store
            .db
            .exec(
                "UPDATE nodes SET presence_at=?,presence_until=? WHERE id=?",
                args![at, at + 600.0, node],
            )
            .await
            .unwrap();
        f.store
            .db
            .exec(insert, args![w, node, at, at - 15.0])
            .await
            .unwrap();
    }
    // The highest workspace id is the most overdue and its machine sleeps; the
    // next one holds only an expired candidate that is not due.
    f.store
        .db
        .exec(insert, args![&asleep.0, &asleep.1, at, at - 30.0])
        .await
        .unwrap();
    f.store
        .db
        .exec(
            insert,
            args![&expired.0, &expired.1, at - 86401.0, at + 600.0],
        )
        .await
        .unwrap();
    let mut rechecked = std::collections::BTreeSet::new();
    for pass in 0..2 {
        let before = now();
        f.store.maintain(false).await.unwrap();
        if pass == 0 {
            let alert = format!("events WHERE kind='alert' AND workspace_id='{}'", asleep.0);
            assert_eq!((f.count(&alert).await, f.alert_count().await), (1, 1));
            assert_eq!(
                f.count(&format!(
                    "alert_candidates WHERE workspace_id='{}'",
                    expired.0
                ))
                .await,
                0
            );
        }
        let mut t = f.store.db.begin().await.unwrap();
        for r in t
            .all(
                "SELECT workspace_id FROM alert_candidates WHERE next_at>?",
                args![before],
            )
            .await
            .unwrap()
        {
            rechecked.insert(s(&r, "workspace_id").to_owned());
        }
        t.commit().await.unwrap();
        // Fifteen seconds pass for every waiting candidate.
        f.store
            .db
            .exec("UPDATE alert_candidates SET next_at=next_at-15", &[])
            .await
            .unwrap();
    }
    // Every waiting workspace was rechecked within two full passes.
    assert_eq!(rechecked.len(), 33);
    f.close().await;
}
#[tokio::test]
async fn snapshot_events_and_push_claims() {
    let f = Fixture::new().await;
    f.store
        .register_push(&f.phone, "fcm", "synthetic-token")
        .await
        .unwrap();
    for phase in ["idle", "input"] {
        let snapshot = json!({"sessions":[{"name":"example","phase":phase,"run_id":"r","conversation_id":"c"}]});
        f.store
            .heartbeat(
                &f.node,
                &json!({"snapshot":snapshot}),
                &canonical(&snapshot).unwrap(),
            )
            .await
            .unwrap();
    }
    // A connector without presence counts as away: each transition alerts at once.
    let e = f.store.events(&f.phone, 0).await.unwrap();
    let kinds: Vec<_> = e["events"]
        .as_array()
        .unwrap()
        .iter()
        .map(|v| v["kind"].as_str().unwrap())
        .collect();
    assert_eq!(kinds, ["attention", "alert"]);
    assert_eq!(e["events"][1]["alert_kind"], "attention");
    let a = f.store.claim_push_jobs().await.unwrap();
    assert_eq!(a.len(), 1);
    assert!(f.store.claim_push_jobs().await.unwrap().is_empty());
    let reg = f
        .store
        .push_registration(&f.phone.id)
        .await
        .unwrap()
        .unwrap();
    f.store
        .finish_push(&a[0], true, false, Some(&reg))
        .await
        .unwrap();
    f.store
        .finish_push(&a[0], true, false, Some(&reg))
        .await
        .unwrap();
    let count = f
        .store
        .db
        .one("SELECT push_jobs,events FROM global_usage WHERE id=1", &[])
        .await
        .unwrap()
        .unwrap();
    assert_eq!(count["push_jobs"], 0);
    assert_eq!(count["events"], 2);
    f.close().await;
}
#[tokio::test]
async fn alert_during_an_in_flight_wake_wakes_again() {
    let f = Fixture::new().await;
    f.store
        .register_push(&f.phone, "fcm", "synthetic-token")
        .await
        .unwrap();
    let reg = f
        .store
        .push_registration(&f.phone.id)
        .await
        .unwrap()
        .unwrap();
    let question = |attention: &str, digit: char| {
        listing(session_row(
            "input",
            "busy",
            attention,
            Some(&signature(digit)),
            Value::Null,
        ))
    };
    let wake = |job: &Value| serde_json::from_str::<Value>(s(job, "payload")).unwrap();
    // Without presence the owner is away, so every question alerts at once.
    f.beat(&listing(session_row(
        "working",
        "busy",
        "",
        None,
        Value::Null,
    )))
    .await;
    f.beat(&question("q1", '1')).await;
    let first = f.store.claim_push_jobs().await.unwrap();
    assert_eq!(first.len(), 1);
    // The next alert commits while the first wake is being sent.
    f.beat(&question("q2", '2')).await;
    let alerts = f.events_of("alert").await;
    assert_eq!(alerts.len(), 2);
    f.store
        .finish_push(&first[0], true, false, Some(&reg))
        .await
        .unwrap();
    let second = f.store.claim_push_jobs().await.unwrap();
    assert_eq!(second.len(), 1);
    assert_eq!(
        wake(&second[0]),
        json!({"event_id":alerts[1]["id"],"kind":"wake"})
    );
    // A failed send with a newer alert is due again at once, as a new job would be.
    f.beat(&question("q3", '3')).await;
    f.store
        .finish_push(&second[0], false, false, Some(&reg))
        .await
        .unwrap();
    let third = f.store.claim_push_jobs().await.unwrap();
    assert_eq!(third.len(), 1);
    assert_eq!(
        (
            wake(&third[0])["event_id"].clone(),
            i(&third[0], "attempts")
        ),
        (f.events_of("alert").await[2]["id"].clone(), 0)
    );
    // Without a newer alert the delivered job is finished.
    f.store
        .finish_push(&third[0], true, false, Some(&reg))
        .await
        .unwrap();
    assert_eq!(f.count("push_jobs").await, 0);
    let usage = f
        .store
        .db
        .one("SELECT push_jobs FROM global_usage WHERE id=1", &[])
        .await
        .unwrap()
        .unwrap();
    assert_eq!(usage["push_jobs"], 0);
    f.close().await;
}
#[tokio::test]
async fn retired_unifiedpush_registration_is_removed_on_delivery() {
    let f = Fixture::new().await;
    f.store
        .register_push(&f.phone, "unifiedpush", "https://push.example.test/opaque")
        .await
        .unwrap();
    for phase in ["idle", "input"] {
        let snapshot = json!({"sessions":[{"name":"example","phase":phase,"run_id":"r","conversation_id":"c"}]});
        f.store
            .heartbeat(
                &f.node,
                &json!({"snapshot":snapshot}),
                &canonical(&snapshot).unwrap(),
            )
            .await
            .unwrap();
    }
    let push = zerus_relay::push::Push::new(f.store.clone()).await.unwrap();
    assert!(push.providers().is_empty());
    push.once().await.unwrap();
    assert!(f
        .store
        .push_registration(&f.phone.id)
        .await
        .unwrap()
        .is_none());
    f.close().await;
}
#[test]
fn retired_push_hosts_config_still_loads() {
    use std::os::unix::fs::PermissionsExt;
    let dir = tempfile::tempdir().unwrap();
    let path = dir.path().join("relay.json");
    std::fs::write(&path, br#"{"push_hosts":["push.example.test"]}"#).unwrap();
    std::fs::set_permissions(&path, std::fs::Permissions::from_mode(0o600)).unwrap();
    let cfg = Config::read(Some(&path)).unwrap();
    assert!(serde_json::to_value(&cfg)
        .unwrap()
        .get("push_hosts")
        .is_none());
}
#[test]
fn canonical_python_identity() {
    for (raw, expected) in [
        (
            r#"{"z":"😀é\u007f","a":1e20}"#,
            r#"{"a":1e+20,"z":"\ud83d\ude00\u00e9\u007f"}"#,
        ),
        ("1e-5", "1e-05"),
        ("1e-4", "0.0001"),
        ("1.0", "1.0"),
        ("-0.0", "-0.0"),
        ("1e16", "1e+16"),
        ("1e15", "1000000000000000.0"),
        (
            "123456789012345678901234567890",
            "123456789012345678901234567890",
        ),
    ] {
        let v: Value = serde_json::from_str(raw).unwrap();
        assert_eq!(canonical(&v).unwrap(), expected, "{raw}");
    }
    assert_eq!(
        digest("hello"),
        "2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824"
    );
}
#[test]
fn malformed_and_deep_json() {
    assert!(zerus_relay::json::parse(&[b'['; 33], 32, 50000).is_err());
}
#[test]
fn proxy_trust_is_explicit() {
    let mut cfg = Config::default();
    let peer = "127.0.0.1".parse().unwrap();
    assert_eq!(
        zerus_relay::http::client_identity(peer, Some("203.0.113.5"), &cfg).unwrap(),
        "127.0.0.1"
    );
    cfg.trusted_proxy_cidrs = vec!["127.0.0.1/32".parse().unwrap()];
    assert_eq!(
        zerus_relay::http::client_identity(peer, Some("198.51.100.7, 203.0.113.5"), &cfg).unwrap(),
        "203.0.113.5"
    );
    assert!(zerus_relay::http::client_identity(peer, Some("garbage"), &cfg).is_err());
}
#[tokio::test]
async fn http_end_to_end_and_disconnect_cleanup() {
    let f = Fixture::new().await;
    let app = App::new(f.store.clone()).await.unwrap();
    let listener = tokio::net::TcpListener::bind("127.0.0.1:0").await.unwrap();
    let url = format!("http://{}", listener.local_addr().unwrap());
    let stop = CancellationToken::new();
    let server = tokio::spawn(serve(listener, app, stop.clone()));
    let client = reqwest::Client::new();
    assert_eq!(
        client
            .get(format!("{url}/healthz"))
            .send()
            .await
            .unwrap()
            .status(),
        200
    );
    let b = f.command("send");
    let response = client
        .post(format!("{url}/v1/requests"))
        .bearer_auth(s(&f.paired, "device_token"))
        .json(&b)
        .send()
        .await
        .unwrap();
    assert_eq!(response.status(), 202, "{}", response.text().await.unwrap());
    let result: Value = client
        .get(format!("{url}/v1/node/requests"))
        .bearer_auth(s(&f.credentials, "node_token"))
        .send()
        .await
        .unwrap()
        .json()
        .await
        .unwrap();
    assert_eq!(result["requests"][0]["request_id"], b["request_id"]);
    let poll_client = client.clone();
    let endpoint = format!("{url}/v1/events?wait=25");
    let token = s(&f.paired, "device_token").to_string();
    let poll =
        tokio::spawn(async move { poll_client.get(endpoint).bearer_auth(token).send().await });
    for _ in 0..100 {
        let r = f
            .store
            .db
            .one("SELECT count(*) AS n FROM poll_leases", &[])
            .await
            .unwrap()
            .unwrap();
        if i(&r, "n") == 1 {
            break;
        }
        tokio::time::sleep(Duration::from_millis(10)).await;
    }
    poll.abort();
    let _ = poll.await;
    let mut cleaned = false;
    for _ in 0..200 {
        let r = f
            .store
            .db
            .one("SELECT count(*) AS n FROM poll_leases", &[])
            .await
            .unwrap()
            .unwrap();
        if i(&r, "n") == 0 {
            cleaned = true;
            break;
        }
        tokio::time::sleep(Duration::from_millis(10)).await;
    }
    stop.cancel();
    server.await.unwrap().unwrap();
    assert!(
        cleaned,
        "disconnected polls must release durable leases promptly"
    );
    f.close().await;
}

#[tokio::test]
#[ignore = "requires isolated ZERUS_RELAY_TEST_DATABASE_URL; run explicitly in CI"]
async fn postgres_cross_worker_speech() {
    let f = Fixture::provision(
        Backend::postgres(Config {
            background: false,
            ..Config::default()
        })
        .await,
    )
    .await;
    let second = f.backend.store().await;
    f.beat(&json!({"sessions":[],"mobile_capabilities":{"protocol_version":1,"operations":protocol::SPEECH}})).await;
    let upload = speech_command(&f.node.id, "speech_submit", None);
    f.submit(&upload).await.unwrap();
    let (a, b) = tokio::join!(f.store.claim(&f.node, false), second.claim(&f.node, false));
    assert_eq!(a.unwrap().is_some() as u8 + b.unwrap().is_some() as u8, 1);
    let reply = json!({"state":"completed","result":{"schema":1,"job_id":upload["request_id"],"state":"queued"},"error":null});
    second
        .result(
            &f.node,
            s(&upload, "request_id"),
            &reply,
            Some(&canonical(&reply["result"]).unwrap()),
        )
        .await
        .unwrap();
    let row = f
        .store
        .db
        .one(
            "SELECT body,body_bytes FROM requests WHERE id=?",
            args![&upload["request_id"]],
        )
        .await
        .unwrap()
        .unwrap();
    assert_eq!(row["body"], "");
    assert_eq!(row["body_bytes"], 0);
    assert_eq!(f.submit(&upload).await.unwrap()["result"], reply["result"]);
    let status = speech_command(&f.node.id, "speech_status", Some(s(&upload, "request_id")));
    f.submit(&status).await.unwrap();
    assert!(second.claim(&f.node, false).await.unwrap().is_some());
    let result = json!({"state":"completed","result":{"schema":1,"job_id":upload["request_id"],"state":"completed","text":"Synthetic speech"},"error":null});
    second
        .result(
            &f.node,
            s(&status, "request_id"),
            &result,
            Some(&canonical(&result["result"]).unwrap()),
        )
        .await
        .unwrap();
    second
        .db
        .exec(
            "UPDATE requests SET updated=? WHERE id=?",
            args![now() - 121.0, &status["request_id"]],
        )
        .await
        .unwrap();
    second.maintain(false).await.unwrap();
    assert!(f
        .store
        .db
        .one(
            "SELECT id FROM requests WHERE id=?",
            args![&status["request_id"]]
        )
        .await
        .unwrap()
        .is_none());
    f.beat(&json!({"sessions":[{"name":"codex/example/tag"}],"mobile_capabilities":{"protocol_version":1,"operations":protocol::OPERATIONS}})).await;
    let voice = voice_command(&f.node.id, "voice_submit", None);
    f.submit(&voice).await.unwrap();
    let (a, b) = tokio::join!(f.store.claim(&f.node, false), second.claim(&f.node, false));
    assert_eq!(a.unwrap().is_some() as u8 + b.unwrap().is_some() as u8, 1);
    let accepted = json!({"state":"completed","result":{"schema":1,"message":{"id":voice["request_id"],"state":"queued","revision":1}},"error":null});
    second
        .result(
            &f.node,
            s(&voice, "request_id"),
            &accepted,
            Some(&canonical(&accepted["result"]).unwrap()),
        )
        .await
        .unwrap();
    let stored = f
        .store
        .db
        .one(
            "SELECT body,body_bytes FROM requests WHERE id=?",
            args![&voice["request_id"]],
        )
        .await
        .unwrap()
        .unwrap();
    assert_eq!(stored["body"], "");
    assert_eq!(stored["body_bytes"], 0);
    assert_eq!(
        f.submit(&voice).await.unwrap()["result"],
        accepted["result"]
    );
    speech_config_storage_contract(&f, &second).await;
    f.check_accounting().await;
    second.db.close().await;
    f.close().await;
}

#[tokio::test]
#[ignore = "requires isolated ZERUS_RELAY_TEST_DATABASE_URL; run explicitly in CI"]
async fn postgres_cross_worker_contract() {
    let f = Fixture::provision(
        Backend::postgres(Config {
            background: false,
            ..Config::default()
        })
        .await,
    )
    .await;
    let second = f.backend.store().await;
    let b = f.command("send");
    let mut wake = second.db.wake.subscribe();
    f.submit(&b).await.unwrap();
    let notified = tokio::time::timeout(Duration::from_secs(2), wake.recv())
        .await
        .unwrap()
        .unwrap();
    assert_eq!(notified, format!("node:{}", f.node.id));
    let (a, c) = tokio::join!(f.store.claim(&f.node, false), second.claim(&f.node, false));
    assert_eq!(
        usize::from(a.unwrap().is_some()) + usize::from(c.unwrap().is_some()),
        1
    );
    second.maintain(true).await.unwrap();
    assert_eq!(
        second.receipt(&f.phone, s(&b, "request_id")).await.unwrap()["state"],
        "claimed"
    );
    let result = json!({"state":"completed","result":{"ok":true},"error":null});
    let encoded = canonical(&result["result"]).unwrap();
    second
        .result(&f.node, s(&b, "request_id"), &result, Some(&encoded))
        .await
        .unwrap();
    f.check_accounting().await;
    let lease = f.store.acquire_poll(&f.phone, 35.0).await.unwrap();
    let lease2 = second.acquire_poll(&f.phone, 35.0).await.unwrap();
    assert!(second.acquire_poll(&f.phone, 35.0).await.is_err());
    second.release_poll(&lease).await.unwrap();
    f.store.release_poll(&lease2).await.unwrap();
    // Quota/accounting locks do not obstruct unchanged heartbeat liveness.
    let snap = json!({"sessions":[],"mobile_capabilities":{"protocol_version":1,"features":["gateway_one_hop"],"operations":protocol::OPERATIONS}});
    let heartbeat = json!({"snapshot":snap,"machine_id":Uuid::new_v4().to_string(),"peers":[]});
    second
        .heartbeat(&f.node, &heartbeat, &canonical(&snap).unwrap())
        .await
        .unwrap();
    let mut lock = f.store.db.begin().await.unwrap();
    f.store
        .db
        .payload_usage(&mut lock, &f.node.workspace)
        .await
        .unwrap();
    tokio::time::timeout(
        Duration::from_secs(1),
        second.heartbeat(&f.node, &heartbeat, &canonical(&snap).unwrap()),
    )
    .await
    .unwrap()
    .unwrap();
    drop(lock);
    // Changed event-free snapshots also bypass accounting locks.
    let mut lock = f.store.db.begin().await.unwrap();
    f.store
        .db
        .payload_usage(&mut lock, &f.node.workspace)
        .await
        .unwrap();
    let next =
        json!({"sessions":[],"sequence":2,"mobile_capabilities":snap["mobile_capabilities"]});
    let hb = json!({"snapshot":next,"machine_id":heartbeat["machine_id"],"peers":[]});
    tokio::time::timeout(
        Duration::from_secs(1),
        second.heartbeat(&f.node, &hb, &canonical(&next).unwrap()),
    )
    .await
    .unwrap()
    .unwrap();
    drop(lock);
    // Push and event publication share the same durable accounting across workers.
    f.store
        .register_push(&f.phone, "fcm", "synthetic-token")
        .await
        .unwrap();
    for phase in ["idle", "input"] {
        let snap = json!({"sessions":[{"name":"test","phase":phase,"run_id":"r","conversation_id":"c"}],"mobile_capabilities":next["mobile_capabilities"]});
        let hb = json!({"snapshot":snap,"machine_id":heartbeat["machine_id"],"peers":[]});
        second
            .heartbeat(&f.node, &hb, &canonical(&snap).unwrap())
            .await
            .unwrap();
    }
    assert_eq!(
        f.store.events(&f.phone, 0).await.unwrap()["events"]
            .as_array()
            .unwrap()
            .len(),
        2
    );
    let (a, b) = tokio::join!(f.store.claim_push_jobs(), second.claim_push_jobs());
    assert_eq!(a.unwrap().len() + b.unwrap().len(), 1);
    // An active desktop reschedules the held candidate; the lock alone then
    // delivers on the other worker from the stored snapshot.
    let held = json!({"sessions":[session_row("input","busy","q2",Some(&signature('2')),Value::Null)],"mobile_capabilities":next["mobile_capabilities"]});
    let beat = |p: Value| json!({"snapshot":held,"machine_id":heartbeat["machine_id"],"peers":[],"presence":p});
    second
        .heartbeat(
            &f.node,
            &beat(presence(1.0, None)),
            &canonical(&held).unwrap(),
        )
        .await
        .unwrap();
    assert_eq!(
        (f.count("alert_candidates").await, f.alert_count().await),
        (1, 1)
    );
    assert_eq!(f.count("alert_candidates WHERE next_at>created").await, 1);
    f.store
        .heartbeat(
            &f.node,
            &beat(presence(1.0, Some(0.0))),
            &canonical(&held).unwrap(),
        )
        .await
        .unwrap();
    assert_eq!(
        (f.count("alert_candidates").await, f.alert_count().await),
        (0, 2)
    );
    // Simulate losing only this fixture database's LISTEN sockets. Durable
    // commands must remain visible while listeners reconnect and wake waiters.
    let mut wake = second.db.wake.subscribe();
    let mut admin = f.backend.server().await;
    let terminated: Vec<bool> = sqlx::query_scalar(
        "SELECT pg_terminate_backend(pid) FROM pg_stat_activity WHERE datname=$1 AND query LIKE 'LISTEN%'",
    ).bind(f.backend.database()).fetch_all(&mut admin).await.unwrap();
    sqlx::Connection::close(admin).await.unwrap();
    assert_eq!(terminated.len(), 2);
    assert!(terminated.iter().all(|v| *v));
    tokio::time::timeout(Duration::from_secs(3), wake.recv())
        .await
        .unwrap()
        .unwrap();
    let b = f.command("send");
    f.submit(&b).await.unwrap();
    assert!(second.claim(&f.node, true).await.unwrap().is_some());
    second.revoke(Role::Device, &f.phone.id).await.unwrap();
    assert!(f.store.authorized(&f.phone).await.is_err());
    f.check_accounting().await;
    second.db.close().await;
    f.close().await;
}

#[tokio::test]
#[ignore = "requires isolated ZERUS_RELAY_TEST_DATABASE_URL; run explicitly in CI"]
async fn postgres_cross_worker_offline_peer_last_seen() {
    let f = Fixture::provision(
        Backend::postgres(Config {
            background: false,
            ..Config::default()
        })
        .await,
    )
    .await;
    let second = f.backend.store().await;
    // The gateway writes through one worker; the phone lists through another.
    offline_peer_route_keeps_last_seen(&f, &second).await;
    second.db.close().await;
    f.close().await;
}

#[tokio::test]
#[ignore = "requires isolated ZERUS_RELAY_TEST_DATABASE_URL; run explicitly in CI"]
async fn postgres_cross_worker_single_alert() {
    let f = Fixture::provision(
        Backend::postgres(Config {
            background: false,
            ..Config::default()
        })
        .await,
    )
    .await;
    let second = f.backend.store().await;
    f.store
        .register_push(&f.phone, "fcm", "synthetic-token")
        .await
        .unwrap();
    let active = presence(1.0, None);
    let question = |attention: &str, digit: char| {
        listing(session_row(
            "input",
            "busy",
            attention,
            Some(&signature(digit)),
            Value::Null,
        ))
    };
    f.beat_with(
        &listing(session_row("working", "busy", "q0", None, Value::Null)),
        &active,
    )
    .await;
    f.beat_with(&question("q1", '1'), &active).await;
    assert_eq!(f.count("alert_candidates").await, 1);
    // Both workers' maintenance passes race for one due candidate once presence is stale.
    f.store
        .db
        .exec("UPDATE nodes SET presence_at=presence_at-121", &[])
        .await
        .unwrap();
    f.store
        .db
        .exec("UPDATE alert_candidates SET next_at=0", &[])
        .await
        .unwrap();
    let (a, b) = tokio::join!(f.store.maintain(false), second.maintain(false));
    a.unwrap();
    b.unwrap();
    assert_eq!(f.alert_count().await, 1);
    // Presence-only heartbeats on two workers race for the next candidate.
    let waiting = question("q2", '2');
    f.beat_with(&waiting, &active).await;
    assert_eq!(f.count("alert_candidates").await, 1);
    let encoded = canonical(&waiting).unwrap();
    let away = |idle: f64| json!({"snapshot":waiting,"presence":presence(idle, None)});
    let (first, other) = (away(600.0), away(601.0));
    let (a, b) = tokio::join!(
        f.store.heartbeat(&f.node, &first, &encoded),
        second.heartbeat(&f.node, &other, &encoded)
    );
    a.unwrap();
    b.unwrap();
    assert_eq!(
        (f.alert_count().await, f.count("alert_candidates").await),
        (2, 0)
    );
    // Both alerts coalesce into the device's single pending wake.
    assert_eq!(f.count("push_jobs").await, 1);
    // An alert committed on one worker while the other sends the wake is sent again.
    let leased = second.claim_push_jobs().await.unwrap();
    assert_eq!(leased.len(), 1);
    let next = question("q3", '3');
    f.store
        .heartbeat(
            &f.node,
            &json!({"snapshot":next,"presence":presence(600.0, None)}),
            &canonical(&next).unwrap(),
        )
        .await
        .unwrap();
    let alerts = f.events_of("alert").await;
    assert_eq!(alerts.len(), 3);
    let reg = f
        .store
        .push_registration(&f.phone.id)
        .await
        .unwrap()
        .unwrap();
    second
        .finish_push(&leased[0], true, false, Some(&reg))
        .await
        .unwrap();
    let resent = f.store.claim_push_jobs().await.unwrap();
    assert_eq!(resent.len(), 1);
    assert_eq!(
        serde_json::from_str::<Value>(s(&resent[0], "payload")).unwrap(),
        json!({"event_id":alerts[2]["id"],"kind":"wake"})
    );
    second.db.close().await;
    f.close().await;
}

#[test]
fn python_float_and_unicode_differential() {
    for raw in [
        r#"{"$serde_json::private::Number":"123"}"#,
        r#"{"\u0024serde_json::private::Number":"123"}"#,
    ] {
        let v = zerus_relay::json::parse(raw.as_bytes(), 32, 50000).unwrap();
        assert!(v.is_object());
        assert_eq!(v["$serde_json::private::Number"], "123");
        assert_eq!(
            canonical(&v).unwrap(),
            r#"{"$serde_json::private::Number":"123"}"#
        );
    }
    // An independent serializer oracle exercises Python's exponent thresholds,
    // arbitrary integers, non-BMP escaping and randomized IEEE-754 values.
    let output=std::process::Command::new("python3").args(["-c",r#"
import json,random,struct,math
rng=random.Random(7391)
values=[0.0,-0.0,1e-4,1e-5,1e15,1e16,1e20,1e23,5e-324,2**80,-2**80,'😀é\u007f','\\/\n\t']
for _ in range(20000):
    f=struct.unpack('>d',rng.randbytes(8))[0]
    if math.isfinite(f):values.append(f)
for v in values:
    print(json.dumps([v,json.dumps(v,sort_keys=True,separators=(',',':'),allow_nan=False)],ensure_ascii=True))
"#]).output().unwrap();
    assert!(output.status.success());
    for line in String::from_utf8(output.stdout).unwrap().lines() {
        let pair: Value = serde_json::from_str(line).unwrap();
        assert_eq!(
            canonical(&pair[0]).unwrap(),
            pair[1].as_str().unwrap(),
            "{}",
            pair[0]
        );
    }
}

#[tokio::test]
async fn revoked_root_retains_only_authorized_peers() {
    let f = Fixture::new().await;
    let root = Uuid::new_v4().to_string();
    let peer = Uuid::new_v4().to_string();
    let route = Uuid::new_v4().to_string();
    let snap = json!({"sessions":[],"mobile_capabilities":{"protocol_version":1,"operations":protocol::OPERATIONS,"features":["gateway_one_hop"]}});
    let hb = json!({"snapshot":snap,"machine_id":root,"peers":[{"route_id":route,"machine_id":peer,"name":"Remote","online":true}]});
    let encoded = canonical(&snap).unwrap();
    f.store.heartbeat(&f.node, &hb, &encoded).await.unwrap();
    f.store
        .peer_heartbeat(
            &f.node,
            &route,
            &json!({"machine_id":peer,"snapshot":snap}),
            &encoded,
        )
        .await
        .unwrap();
    f.store.revoke_computer(&f.node.id).await.unwrap();
    f.store.heartbeat(&f.node, &hb, &encoded).await.unwrap();
    f.store.authorized(&f.node).await.unwrap();
    let catalog = f.store.computers(&f.phone).await.unwrap();
    assert_eq!(catalog["computers"].as_array().unwrap().len(), 1);
    assert_eq!(catalog["computers"][0]["machine_id"], peer);
    let mut b = f.command("send");
    assert_eq!(f.submit(&b).await.unwrap_err().0.as_u16(), 409);
    b["computer_id"] = catalog["computers"][0]["id"].clone();
    f.submit(&b).await.unwrap();
    assert!(f.store.claim(&f.node, true).await.unwrap().is_some());
    f.close().await;
}

#[tokio::test]
async fn http_rejects_bad_content_before_mutation_and_recovers_from_disconnect() {
    let f = Fixture::new().await;
    let app = App::new(f.store.clone()).await.unwrap();
    let listener = tokio::net::TcpListener::bind("127.0.0.1:0").await.unwrap();
    let address = listener.local_addr().unwrap();
    let url = format!("http://{address}");
    let stop = CancellationToken::new();
    let server = tokio::spawn(serve(listener, app, stop.clone()));
    let client = reqwest::Client::new();
    let auth = s(&f.paired, "device_token");
    let b = f.command("send");
    let endpoint = format!("{url}/v1/requests");
    assert_eq!(
        client
            .post(&endpoint)
            .bearer_auth(auth)
            .header("content-encoding", "gzip")
            .json(&b)
            .send()
            .await
            .unwrap()
            .status(),
        415
    );
    assert_eq!(
        client
            .post(&endpoint)
            .bearer_auth(auth)
            .header("content-type", "application/json")
            .body("{".repeat(100))
            .send()
            .await
            .unwrap()
            .status(),
        413
    );
    // Abort an unknown-length upload while it owns the sole large payload lane.
    use tokio::io::AsyncWriteExt;
    let mut stream = tokio::net::TcpStream::connect(address).await.unwrap();
    stream.write_all(format!("POST /v1/requests HTTP/1.1\r\nHost: localhost\r\nAuthorization: Bearer {auth}\r\nContent-Type: application/json\r\nTransfer-Encoding: chunked\r\n\r\n1\r\n{{\r\n").as_bytes()).await.unwrap();
    tokio::time::sleep(Duration::from_millis(25)).await;
    // A second client is already streaming when admission is refused. It must
    // receive a usable overload response instead of losing it to a TCP reset.
    let overloaded = client
        .post(&endpoint)
        .bearer_auth(auth)
        .header("content-type", "application/json")
        .body(" ".repeat(2 * 1024 * 1024))
        .send()
        .await
        .unwrap();
    assert_eq!(overloaded.status(), 429);
    assert_eq!(overloaded.headers()["retry-after"], "1");
    assert_eq!(
        client
            .get(format!("{url}/healthz"))
            .send()
            .await
            .unwrap()
            .status(),
        200
    );
    drop(stream);
    let mut result = None;
    for _ in 0..20 {
        let r = client
            .post(&endpoint)
            .bearer_auth(auth)
            .json(&b)
            .send()
            .await
            .unwrap();
        if r.status() != 429 {
            result = Some(r.status());
            break;
        }
        tokio::time::sleep(Duration::from_millis(10)).await;
    }
    assert_eq!(result, Some(reqwest::StatusCode::ACCEPTED));
    stop.cancel();
    server.await.unwrap().unwrap();
    f.close().await;
}

#[tokio::test]
async fn http_oversized_declared_body_is_answered_after_bounded_drain() {
    use tokio::io::{AsyncReadExt, AsyncWriteExt};
    let f = Fixture::new().await;
    let app = App::new(f.store.clone()).await.unwrap();
    let listener = tokio::net::TcpListener::bind("127.0.0.1:0").await.unwrap();
    let address = listener.local_addr().unwrap();
    let stop = CancellationToken::new();
    let server = tokio::spawn(serve(listener, app, stop.clone()));
    let auth = s(&f.credentials, "node_token");
    let head = |length: usize, extra: &str| {
        format!("POST /v1/node/heartbeat HTTP/1.1\r\nHost: localhost\r\nAuthorization: Bearer {auth}\r\nContent-Type: application/json\r\nContent-Length: {length}\r\nConnection: close\r\n{extra}\r\n")
    };
    let read = |mut stream: tokio::net::TcpStream| async move {
        let mut response = Vec::new();
        tokio::time::timeout(Duration::from_secs(10), stream.read_to_end(&mut response))
            .await
            .expect("the rejection is bounded in time")
            .expect("the rejection is readable");
        String::from_utf8(response).unwrap()
    };
    let rejected = |response: &str| {
        response.starts_with("HTTP/1.1 413 ")
            && response.ends_with(r#"{"error":"payload exceeds limit"}"#)
    };
    // The heartbeat cap is 1 MiB. A client still uploading when the declared
    // length is refused must read 413 instead of losing it to a TCP reset.
    let mut stream = tokio::net::TcpStream::connect(address).await.unwrap();
    stream
        .write_all(head(protocol::MIB + 1, "").as_bytes())
        .await
        .unwrap();
    let piece = [b' '; 64 * 1024];
    let mut remaining = protocol::MIB + 1;
    while remaining > 0 {
        let n = remaining.min(piece.len());
        stream.write_all(&piece[..n]).await.unwrap();
        remaining -= n;
        tokio::time::sleep(Duration::from_millis(5)).await;
    }
    let response = read(stream).await;
    assert!(rejected(&response), "{response}");
    // A declared body that never arrives cannot hold the rejection until the
    // request deadline: the discard ends at its own short deadline.
    let mut stream = tokio::net::TcpStream::connect(address).await.unwrap();
    stream
        .write_all(head(1 << 30, "").as_bytes())
        .await
        .unwrap();
    let response = read(stream).await;
    assert!(rejected(&response), "{response}");
    // Expect/continue clients are refused before they send any body byte.
    let mut stream = tokio::net::TcpStream::connect(address).await.unwrap();
    stream
        .write_all(head(protocol::MIB + 1, "Expect: 100-continue\r\n").as_bytes())
        .await
        .unwrap();
    let response = read(stream).await;
    assert!(rejected(&response), "{response}");
    stop.cancel();
    server.await.unwrap().unwrap();
    f.close().await;
}

/// Sends a request head, then its body in paced 4 KiB pieces, so the relay
/// decides while the client is still uploading.
async fn upload(address: std::net::SocketAddr, head: &str, body: &[u8]) -> tokio::net::TcpStream {
    use tokio::io::AsyncWriteExt;
    let mut stream = tokio::net::TcpStream::connect(address).await.unwrap();
    stream.write_all(head.as_bytes()).await.unwrap();
    for piece in body.chunks(4096) {
        tokio::time::sleep(Duration::from_millis(2)).await;
        stream
            .write_all(piece)
            .await
            .expect("the relay reads the rest of an upload it refuses");
    }
    stream
}
/// Reads a `Connection: close` response to its end.
async fn answer(mut stream: tokio::net::TcpStream) -> String {
    use tokio::io::AsyncReadExt;
    let mut response = Vec::new();
    tokio::time::timeout(Duration::from_secs(10), stream.read_to_end(&mut response))
        .await
        .expect("the refusal is bounded in time")
        .expect("the refusal is readable");
    String::from_utf8(response).unwrap()
}
fn refused(response: &str, status: &str, error: &str) -> bool {
    response.starts_with(&format!("HTTP/1.1 {status} "))
        && response.ends_with(&format!(r#"{{"error":"{error}"}}"#))
}
/// Waits until a rate counter exists. The relay takes the request lane before
/// it counts the request, so the counter proves that the lane is held.
async fn counted(f: &Fixture, key: Value) {
    let key = digest(canonical(&key).unwrap());
    for _ in 0..500 {
        let row = f
            .store
            .db
            .one("SELECT count FROM rate_limits WHERE key=?", args![&key])
            .await
            .unwrap();
        if row.is_some() {
            return;
        }
        tokio::time::sleep(Duration::from_millis(10)).await;
    }
    panic!("the held request never reached its rate limit");
}

#[tokio::test]
async fn http_early_refusals_are_answered_after_bounded_drain() {
    use tokio::io::{AsyncReadExt, AsyncWriteExt};
    let f = Fixture::new().await;
    let app = App::new(f.store.clone()).await.unwrap();
    let listener = tokio::net::TcpListener::bind("127.0.0.1:0").await.unwrap();
    let address = listener.local_addr().unwrap();
    let stop = CancellationToken::new();
    let server = tokio::spawn(serve(listener, app, stop.clone()));
    let node = s(&f.credentials, "node_token");
    let head = |path: &str, token: &str, extra: &str, length: usize| {
        format!("POST {path} HTTP/1.1\r\nHost: localhost\r\nAuthorization: Bearer {token}\r\nContent-Length: {length}\r\nConnection: close\r\n{extra}\r\n")
    };
    let json = "Content-Type: application/json\r\n";
    // A refusal made before the body is read discards up to 64 KiB, so a
    // client uploading less than that reads the status, not a TCP reset.
    let body = vec![b' '; 48 * 1024];
    let heartbeat = "/v1/node/heartbeat";
    let cases = [
        (
            heartbeat,
            "not-a-credential",
            json,
            "401",
            "credential not authorized",
        ),
        (
            heartbeat,
            node,
            "Content-Type: application/json\r\nContent-Encoding: gzip\r\n",
            "415",
            "compressed requests are unsupported",
        ),
        (
            heartbeat,
            node,
            "Content-Type: text/plain\r\n",
            "415",
            "application/json required",
        ),
        ("/v1/node/unknown", node, json, "404", "not found"),
    ];
    for (path, token, extra, status, error) in cases {
        let stream = upload(address, &head(path, token, extra, body.len()), &body).await;
        let response = answer(stream).await;
        assert!(refused(&response, status, error), "{response}");
    }
    // Expect/continue clients are refused before they send any body byte: the
    // discard never invites the upload with 100 Continue.
    let expect = format!("{json}Expect: 100-continue\r\n");
    let stream = upload(
        address,
        &head(heartbeat, "not-a-credential", &expect, body.len()),
        &[],
    )
    .await;
    let response = answer(stream).await;
    assert!(
        refused(&response, "401", "credential not authorized"),
        "{response}"
    );
    // An unauthenticated client cannot make the relay read more than 64 KiB:
    // the refusal ends at once instead of waiting for the rest of the declared
    // body until the two-second discard deadline.
    let started = std::time::Instant::now();
    let mut stream = tokio::net::TcpStream::connect(address).await.unwrap();
    stream
        .write_all(head(heartbeat, "not-a-credential", json, 1 << 20).as_bytes())
        .await
        .unwrap();
    // The relay may reset the connection while this is still being written.
    let _ = stream.write_all(&vec![b' '; 256 * 1024]).await;
    let mut response = Vec::new();
    let _ = tokio::time::timeout(Duration::from_secs(10), stream.read_to_end(&mut response)).await;
    assert!(
        started.elapsed() < Duration::from_millis(1500),
        "an early refusal read beyond its bound for {:?}",
        started.elapsed()
    );
    stop.cancel();
    server.await.unwrap().unwrap();
    f.close().await;
}

#[tokio::test]
async fn http_rate_and_lane_refusals_are_answered_after_bounded_drain() {
    use tokio::io::AsyncWriteExt;
    let f = Fixture::config(Config {
        background: false,
        max_anonymous_requests: 1,
        max_authenticated_requests: 1,
        ..Config::default()
    })
    .await;
    let app = App::new(f.store.clone()).await.unwrap();
    let listener = tokio::net::TcpListener::bind("127.0.0.1:0").await.unwrap();
    let address = listener.local_addr().unwrap();
    let stop = CancellationToken::new();
    let server = tokio::spawn(serve(listener, app, stop.clone()));
    let node = s(&f.credentials, "node_token");
    let head = |path: &str, token: &str, length: usize| {
        format!("POST {path} HTTP/1.1\r\nHost: localhost\r\nAuthorization: Bearer {token}\r\nContent-Type: application/json\r\nContent-Length: {length}\r\nConnection: close\r\n\r\n")
    };
    let hold = |path: &str, token: &str| {
        format!("POST {path} HTTP/1.1\r\nHost: localhost\r\nAuthorization: Bearer {token}\r\nContent-Type: application/json\r\nTransfer-Encoding: chunked\r\n\r\n1\r\n{{\r\n")
    };
    let body = vec![b' '; 48 * 1024];
    let busy = |response: &str| {
        refused(response, "429", "relay capacity is busy")
            && response.contains("\r\nretry-after: 1\r\n")
    };
    // Hold the sole anonymous lane with an unfinished pairing upload, then
    // refuse a second pairing that is still uploading.
    let mut pairing = tokio::net::TcpStream::connect(address).await.unwrap();
    pairing
        .write_all(hold("/v1/pair", "").as_bytes())
        .await
        .unwrap();
    counted(&f, json!(["pair", "127.0.0.1"])).await;
    let stream = upload(address, &head("/v1/pair", "", body.len()), &body).await;
    let response = answer(stream).await;
    assert!(busy(&response), "{response}");
    drop(pairing);
    // The same for the sole authenticated request lane.
    let mut heartbeat = tokio::net::TcpStream::connect(address).await.unwrap();
    heartbeat
        .write_all(hold("/v1/node/heartbeat", node).as_bytes())
        .await
        .unwrap();
    counted(&f, json!([Role::Node.table(), f.node.workspace, f.node.id])).await;
    let stream = upload(
        address,
        &head("/v1/node/heartbeat", node, body.len()),
        &body,
    )
    .await;
    let response = answer(stream).await;
    assert!(busy(&response), "{response}");
    drop(heartbeat);
    // A client over the failed-login rate limit is refused the same way.
    for _ in 0..20 {
        f.store
            .rate(&json!(["login", "127.0.0.1"]), 20, true)
            .await
            .unwrap();
    }
    let stream = upload(
        address,
        &head("/v1/node/heartbeat", "not-a-credential", body.len()),
        &body,
    )
    .await;
    let response = answer(stream).await;
    assert!(busy(&response), "{response}");
    stop.cancel();
    server.await.unwrap().unwrap();
    f.close().await;
}

#[tokio::test]
async fn http_streamed_body_over_limit_releases_lane_before_bounded_drain() {
    use tokio::io::{AsyncReadExt, AsyncWriteExt};
    let f = Fixture::new().await;
    let app = App::new(f.store.clone()).await.unwrap();
    let listener = tokio::net::TcpListener::bind("127.0.0.1:0").await.unwrap();
    let address = listener.local_addr().unwrap();
    let stop = CancellationToken::new();
    let server = tokio::spawn(serve(listener, app, stop.clone()));
    let node = s(&f.credentials, "node_token");
    let head = format!("POST /v1/node/heartbeat HTTP/1.1\r\nHost: localhost\r\nAuthorization: Bearer {node}\r\nContent-Type: application/json\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n");
    let chunk = |data: &[u8]| {
        let mut framed = format!("{:x}\r\n", data.len()).into_bytes();
        framed.extend_from_slice(data);
        framed.extend_from_slice(b"\r\n");
        framed
    };
    let piece = chunk(&[b' '; 64 * 1024]);
    let too_large = |response: &str| refused(response, "413", "payload exceeds limit");
    // A streamed heartbeat crosses its 1 MiB cap while the client is still
    // sending. The client finishes its upload and reads 413, not a reset.
    let mut stream = tokio::net::TcpStream::connect(address).await.unwrap();
    stream.write_all(head.as_bytes()).await.unwrap();
    for _ in 0..20 {
        stream
            .write_all(&piece)
            .await
            .expect("the relay reads the rest of a stream it refuses");
        tokio::time::sleep(Duration::from_millis(5)).await;
    }
    stream.write_all(b"0\r\n\r\n").await.unwrap();
    let response = answer(stream).await;
    assert!(too_large(&response), "{response}");
    // A stream that stops after crossing the cap is answered at the discard
    // deadline. Its discard no longer holds the sole large payload lane: an
    // unknown-length upload is admitted meanwhile and fails only as JSON.
    // Linux loopback buffers this whole upload before the relay reads it, so
    // finishing the writes does not prove admission and a probe could take
    // the lane first, refusing the stalled stream as busy. The relay sends
    // 100 Continue only when it first reads the body, after admission.
    let mut stalled = tokio::net::TcpStream::connect(address).await.unwrap();
    let expect = head.replace("\r\n\r\n", "\r\nExpect: 100-continue\r\n\r\n");
    stalled.write_all(expect.as_bytes()).await.unwrap();
    let mut interim = [0u8; 25];
    tokio::time::timeout(Duration::from_secs(10), stalled.read_exact(&mut interim))
        .await
        .expect("the relay admits the stalled stream")
        .unwrap();
    assert_eq!(&interim, b"HTTP/1.1 100 Continue\r\n\r\n");
    for _ in 0..17 {
        stalled.write_all(&piece).await.unwrap();
    }
    let mut admitted = None;
    for _ in 0..40 {
        let mut stream = tokio::net::TcpStream::connect(address).await.unwrap();
        stream.write_all(head.as_bytes()).await.unwrap();
        stream.write_all(&chunk(b"{")).await.unwrap();
        stream.write_all(b"0\r\n\r\n").await.unwrap();
        let response = answer(stream).await;
        if !response.starts_with("HTTP/1.1 429 ") {
            admitted = Some(response);
            break;
        }
        tokio::time::sleep(Duration::from_millis(25)).await;
    }
    let admitted = admitted.expect("the refused stream released the large payload lane");
    assert!(refused(&admitted, "400", "invalid request"), "{admitted}");
    let response = answer(stalled).await;
    assert!(too_large(&response), "{response}");
    stop.cancel();
    server.await.unwrap().unwrap();
    f.close().await;
}

#[tokio::test]
async fn mark_read_payload_is_exact_and_capability_gated() {
    let f = Fixture::new().await;
    let id = Uuid::new_v4().to_string();
    let payload = json!({"run_id":"run","conversation_id":"conversation","reply_id":"7:100.5","attention_signature":"a".repeat(64)});
    let body = json!({"request_id":id,"computer_id":f.node.id,"operation":"mark_read","session":"codex/example/tag","payload":payload});
    // A computer that does not advertise the operation keeps rejecting it.
    assert_eq!(f.submit(&body).await.unwrap_err().0.as_u16(), 409);
    let snapshot = json!({"sessions":[],"mobile_capabilities":{"protocol_version":1,"operations":["mark_read"],"features":[]}});
    f.store
        .heartbeat(
            &f.node,
            &json!({"snapshot":snapshot}),
            &canonical(&snapshot).unwrap(),
        )
        .await
        .unwrap();
    assert_eq!(f.submit(&body).await.unwrap()["state"], "queued");
    let changes = [
        json!({"reply_id":7}),
        json!({"attention_signature":"A".repeat(64)}),
        json!({"attention_signature":"a".repeat(63)}),
        json!({"run_id":""}),
        json!({"reply_id":"bad\u{7}"}),
        json!({"request_id":id}),
    ];
    for change in changes {
        let mut invalid = body.clone();
        invalid["request_id"] = json!(Uuid::new_v4().to_string());
        for (key, value) in change.as_object().unwrap() {
            invalid["payload"][key] = value.clone();
        }
        assert!(zerus_relay::store::Submission::new(invalid).is_err());
    }
    let mut missing = body.clone();
    missing["payload"]
        .as_object_mut()
        .unwrap()
        .remove("reply_id");
    assert!(zerus_relay::store::Submission::new(missing).is_err());
    let mut nulls = body.clone();
    nulls["request_id"] = json!(Uuid::new_v4().to_string());
    nulls["payload"]["reply_id"] = Value::Null;
    nulls["payload"]["attention_signature"] = Value::Null;
    assert!(zerus_relay::store::Submission::new(nulls).is_ok());
    assert!(protocol::capabilities(vec![])["operations"]
        .as_array()
        .unwrap()
        .iter()
        .any(|v| v == "mark_read"));
    f.close().await;
}

fn attachment_read(f: &Fixture) -> Value {
    let id = Uuid::new_v4().to_string();
    json!({"request_id":id,"computer_id":f.node.id,"operation":"attachment_read","session":"codex/example/tag",
        "payload":{"request_id":id,"expected_run_id":"run","expected_conversation_id":"conversation",
            "message_id":"00000000-0000-4000-8000-000000000001","index":0,"offset":0,"length":524288}})
}

#[tokio::test]
async fn attachment_read_is_exact_gated_and_short_lived() {
    assert!(protocol::READS.contains(&"attachment_read"));
    assert!(!protocol::READ_SQL.contains("attachment_read"));
    assert!(protocol::capabilities(vec![])["operations"]
        .as_array()
        .unwrap()
        .iter()
        .any(|v| v == "attachment_read"));
    let f = Fixture::new().await;
    let b = attachment_read(&f);
    assert_eq!(f.submit(&b).await.unwrap_err().0.as_u16(), 409);
    f.beat(&json!({"sessions":[],"mobile_capabilities":{"protocol_version":1,"operations":["attachment_read"],"features":[]}}))
        .await;
    for (field, value) in [
        ("message_id", Value::Null),
        ("message_id", json!("00000000-0000-4000-8000-00000000000A")),
        ("message_id", json!("not-a-uuid")),
        ("index", json!(8)),
        ("index", json!(-1)),
        ("index", json!(0.0)),
        ("index", json!("0")),
        ("offset", json!(10 * 1024 * 1024)),
        ("offset", json!(10 * 1024 * 1024 - 10)),
        ("length", json!(0)),
        ("length", json!(524289)),
        ("length", Value::Null),
        ("expected_conversation_id", json!("")),
        ("expected_run_id", json!("")),
        ("archive_id", json!("00000000-0000-0000-0000-000000000000")),
        ("agent_id", json!("../child")),
        ("path", json!("/private/file.png")),
        ("text", json!("hello")),
    ] {
        let mut invalid = b.clone();
        invalid["request_id"] = json!(Uuid::new_v4().to_string());
        invalid["payload"]["request_id"] = invalid["request_id"].clone();
        if value.is_null() {
            invalid["payload"].as_object_mut().unwrap().remove(field);
        } else {
            invalid["payload"][field] = value;
        }
        assert!(
            matches!(zerus_relay::store::Submission::new(invalid), Err(e) if e.0.as_u16() == 400),
            "{field}"
        );
    }
    let mut child = attachment_read(&f);
    child["payload"]["agent_id"] = json!("worker-1");
    child["payload"]["archive_id"] = json!(Uuid::new_v4().to_string());
    assert!(zerus_relay::store::Submission::new(child).is_ok());
    // A completed maximum chunk fits the ordinary result bound; once the phone
    // has read it, the bytes leave the relay after a short grace period.
    assert_eq!(f.submit(&b).await.unwrap()["state"], "queued");
    assert!(f.store.claim(&f.node, false).await.unwrap().is_some());
    let data = "A".repeat(699_052);
    let result = json!({"state":"completed","result":{"request_id":b["request_id"],"name":"codex/example/tag","file_name":"Скриншот 😀.png","data_base64":data},"error":null});
    let e = canonical(&result["result"]).unwrap();
    f.store
        .result(&f.node, s(&b, "request_id"), &result, Some(&e))
        .await
        .unwrap();
    f.check_accounting().await;
    let receipt = f
        .store
        .receipt(&f.phone, s(&b, "request_id"))
        .await
        .unwrap();
    assert_eq!(
        receipt["result"]["data_base64"].as_str().unwrap().len(),
        699_052
    );
    f.store.maintain(false).await.unwrap();
    assert_eq!(
        f.count("requests WHERE operation='attachment_read'").await,
        1
    );
    f.store
        .db
        .exec(
            "UPDATE requests SET result_read=? WHERE id=?",
            args![now() - 6.0, &b["request_id"]],
        )
        .await
        .unwrap();
    f.store.maintain(false).await.unwrap();
    assert_eq!(
        f.count("requests WHERE operation='attachment_read'").await,
        0
    );
    f.check_accounting().await;
    // An unread result is removed after its TTL; a pruned read identity is new again.
    let unread = attachment_read(&f);
    assert_eq!(f.submit(&unread).await.unwrap()["state"], "queued");
    assert!(f.store.claim(&f.node, false).await.unwrap().is_some());
    let failed =
        json!({"state":"failed","result":null,"error":"attachment is no longer available"});
    f.store
        .result(&f.node, s(&unread, "request_id"), &failed, None)
        .await
        .unwrap();
    f.store
        .db
        .exec(
            "UPDATE requests SET updated=? WHERE id=?",
            args![now() - 59.0, &unread["request_id"]],
        )
        .await
        .unwrap();
    f.store.maintain(false).await.unwrap();
    assert_eq!(
        f.count("requests WHERE operation='attachment_read'").await,
        1
    );
    f.store
        .db
        .exec(
            "UPDATE requests SET updated=? WHERE id=?",
            args![now() - 61.0, &unread["request_id"]],
        )
        .await
        .unwrap();
    f.store.maintain(false).await.unwrap();
    assert_eq!(
        f.count("requests WHERE operation='attachment_read'").await,
        0
    );
    f.check_accounting().await;
    assert_eq!(f.submit(&unread).await.unwrap()["state"], "queued");
    f.close().await;
}

#[tokio::test]
async fn mobile_delivery_is_computer_scoped_and_capability_gated() {
    let f = Fixture::new().await;
    let id = Uuid::new_v4().to_string();
    let b = json!({"request_id":id,"computer_id":f.node.id,"operation":"set_mobile_delivery","session":"","payload":{"value":"away_10"}});
    assert_eq!(f.submit(&b).await.unwrap_err().0.as_u16(), 409);
    let snapshot = json!({"sessions":[],"mobile_capabilities":{"protocol_version":1,"operations":["set_mobile_delivery"],"features":[]}});
    f.store
        .heartbeat(
            &f.node,
            &json!({"snapshot":snapshot}),
            &canonical(&snapshot).unwrap(),
        )
        .await
        .unwrap();
    assert_eq!(f.submit(&b).await.unwrap()["state"], "queued");
    assert!(protocol::OPERATIONS.contains(&"set_mobile_delivery"));
    for (field, value) in [
        ("session", json!("codex/example/tag")),
        ("payload", json!({"value":"later"})),
        ("payload", json!({})),
        ("payload", json!({"value":"away","request_id":id})),
    ] {
        let mut invalid = b.clone();
        invalid["request_id"] = json!(Uuid::new_v4().to_string());
        invalid[field] = value;
        assert!(matches!(
            zerus_relay::store::Submission::new(invalid),
            Err(e) if e.0.as_u16() == 400
        ));
    }
    f.close().await;
}

async fn computer_file_read_contract(f: &Fixture, second: Option<&Store>) {
    let worker = second.unwrap_or(&f.store);
    assert!(protocol::READS.contains(&"file_read"));
    assert!(!protocol::READ_SQL.contains("file_read"));
    let id = Uuid::new_v4().to_string();
    let command = json!({"request_id":id,"computer_id":f.node.id,"operation":"file_read","session":"codex/example/tag",
        "payload":{"request_id":id,"expected_run_id":"run","expected_conversation_id":"conversation",
            "path":"/workspace/example/Сводная итог.xlsx","offset":0,"length":524288}});
    protocol::request(&command).unwrap();
    assert_eq!(f.submit(&command).await.unwrap_err().0.as_u16(), 409);
    f.beat(&json!({"sessions":[],"mobile_capabilities":{"protocol_version":1,"operations":["file_read"],"features":[]}})).await;
    for (field, value) in [
        ("path", json!("")),
        ("path", json!("/bad\nfile")),
        ("path", json!("界".repeat(2731))),
        ("offset", json!(1)),
        ("offset", json!(true)),
        ("length", json!(0)),
        ("length", json!(524289)),
        ("sha256", json!("a")),
        ("archive_id", json!("00000000-0000-0000-0000-000000000000")),
        ("agent_id", json!("../child")),
    ] {
        let mut bad = command.clone();
        bad["payload"][field] = value;
        assert!(protocol::request(&bad).is_err(), "accepted {field}");
    }
    let mut later = command.clone();
    later["payload"]["offset"] = json!(3);
    later["payload"]["sha256"] = json!("a".repeat(64));
    protocol::request(&later).unwrap();
    f.submit(&command).await.unwrap();
    worker.claim(&f.node, false).await.unwrap().unwrap();
    let result = json!({"state":"completed","result":{"file_name":"Сводная итог.xlsx","data_base64":"AABQ"},"error":null});
    worker
        .result(
            &f.node,
            &id,
            &result,
            Some(&canonical(&result["result"]).unwrap()),
        )
        .await
        .unwrap();
    f.store.receipt(&f.phone, &id).await.unwrap();
    f.check_accounting().await;
    worker.maintain(false).await.unwrap();
    assert_eq!(f.count("requests WHERE operation='file_read'").await, 1);
    f.store
        .db
        .exec(
            "UPDATE requests SET result_read=? WHERE id=?",
            args![now() - 6., &id],
        )
        .await
        .unwrap();
    worker.maintain(false).await.unwrap();
    assert_eq!(f.count("requests WHERE operation='file_read'").await, 0);
    f.check_accounting().await;
}

#[tokio::test]
async fn computer_file_read_is_scoped_gated_and_short_lived() {
    let f = Fixture::new().await;
    computer_file_read_contract(&f, None).await;
    f.close().await;
}

#[tokio::test]
#[ignore = "requires isolated ZERUS_RELAY_TEST_DATABASE_URL; run explicitly"]
async fn postgres_cross_worker_computer_file_read_is_scoped_gated_and_short_lived() {
    let f = Fixture::provision(
        Backend::postgres(Config {
            background: false,
            ..Config::default()
        })
        .await,
    )
    .await;
    let second = f.backend.store().await;
    computer_file_read_contract(&f, Some(&second)).await;
    second.db.close().await;
    f.close().await;
}

async fn archive_page_contract(f: &Fixture, second: Option<&Store>) {
    let worker = second.unwrap_or(&f.store);
    assert!(protocol::READS.contains(&"archive_list"));
    assert!(!protocol::READ_SQL.contains("archive_list"));
    let id = Uuid::new_v4().to_string();
    let command = json!({"request_id":id,"computer_id":f.node.id,"operation":"archive_list","session":"","payload":{"from":100,"until":200,"limit":50,"cursor":"","query":"Русский"}});
    protocol::request(&command).unwrap();
    assert_eq!(f.submit(&command).await.unwrap_err().0.as_u16(), 409);
    f.beat(&json!({"sessions":[],"mobile_capabilities":{"protocol_version":1,"operations":["archive_list"],"features":[]}})).await;
    for (field, value) in [
        ("limit", json!(51)),
        ("limit", json!(true)),
        ("from", json!(200)),
        ("until", json!(253402300801_u64)),
        ("cursor", json!("#")),
        ("query", json!("bad\nquery")),
        ("offset", json!(5000)),
    ] {
        let mut bad = command.clone();
        bad["payload"][field] = value;
        assert!(protocol::request(&bad).is_err(), "accepted {field}");
    }
    let mut bad = command.clone();
    bad["session"] = json!("codex/example/tag");
    assert!(protocol::request(&bad).is_err());
    f.submit(&command).await.unwrap();
    let claimed = worker
        .claim(&f.node, false)
        .await
        .unwrap()
        .unwrap()
        .decode()
        .unwrap();
    assert_eq!(claimed["requests"][0]["operation"], "archive_list");
    let result = json!({"state":"completed","result":{"version":1,"sessions":[],"next_cursor":"","total":0,"from":100,"until":200,"query":"Русский"},"error":null});
    worker
        .result(
            &f.node,
            &id,
            &result,
            Some(&canonical(&result["result"]).unwrap()),
        )
        .await
        .unwrap();
    assert_eq!(
        f.store.receipt(&f.phone, &id).await.unwrap()["result"]["total"],
        0
    );
    f.check_accounting().await;
    f.store
        .db
        .exec(
            "UPDATE requests SET updated=? WHERE id=?",
            args![now() - 3601., &id],
        )
        .await
        .unwrap();
    worker.maintain(false).await.unwrap();
    assert_eq!(f.count("requests WHERE operation='archive_list'").await, 0);
    f.check_accounting().await;
}
#[tokio::test]
async fn archive_pages_are_gated_bounded_reads() {
    let f = Fixture::new().await;
    archive_page_contract(&f, None).await;
    f.close().await;
}
#[tokio::test]
#[ignore = "requires isolated ZERUS_RELAY_TEST_DATABASE_URL; run explicitly"]
async fn postgres_cross_worker_archive_pages_are_gated_bounded_reads() {
    let f = Fixture::provision(
        Backend::postgres(Config {
            background: false,
            ..Config::default()
        })
        .await,
    )
    .await;
    let second = f.backend.store().await;
    archive_page_contract(&f, Some(&second)).await;
    second.db.close().await;
    f.close().await;
}
