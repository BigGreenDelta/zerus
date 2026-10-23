//! Real phone/HTTP relay/connector transport with an isolated native seam.
#![cfg(unix)]
use serde_json::{json, Value};
use std::{
    cell::{Cell, RefCell},
    rc::Rc,
    time::Duration,
};
use zerus_mobile_connector::{
    config, encrypted_runtime,
    http::Relay,
    journal::Journal,
    native::{Call, Native, NativeError, NativeResult, Output},
    runtime::{Connector, Event, Settings},
    workspace,
};
use zerus_mobile_crypto::{
    self as crypto, EncryptedEnvelope, EnrollmentCandidate, Header, Identity, Invitation,
    OperationClass, PayloadKind, Role, SignedEnrollment, SignedMembershipSnapshot,
};
use zerus_relay::config::Config;
#[path = "../../relay/tests/common/mod.rs"]
mod common;
#[path = "../../relay/tests/support/mod.rs"]
mod support;

const MACHINE: &str = "00000000-0000-4000-8000-000000000120";
const SESSION: &str = "codex/fixture/private-session";
const PRIVATE: &str = "Synthetic private native response";
#[derive(Default)]
struct FixtureNative {
    calls: RefCell<Vec<Vec<String>>>,
    block_inspect: bool,
    inspect_entered: Event,
    inspect_release: Event,
    inspect_finished: Cell<bool>,
}
impl Native for FixtureNative {
    async fn call(&self, call: Call) -> NativeResult {
        self.calls.borrow_mut().push(call.argv.clone());
        let mut argv = call.argv.as_slice();
        let guarded = argv.first().is_some_and(|v| v == "--expect-machine");
        if guarded {
            assert_eq!(argv.get(1).map(String::as_str), Some(MACHINE));
            argv = &argv[2..];
        }
        if argv == ["--help"] {
            return Ok(Output::Text(
                "Encrypted workspace ABI: guarded-local-v1".into(),
            ));
        }
        if argv == ["mobile", "identity", "--json"] {
            return Ok(Output::Json(json!({"schema":1,"machine_id":MACHINE})));
        }
        if argv.first().is_some_and(|v| v == "ls") {
            assert!(guarded, "Encrypted snapshots must bind the native machine");
            return Ok(Output::Json(
                json!({"host":"synthetic-host","sessions":[{"name":SESSION,"run_id":"synthetic-run","conversation_id":"synthetic-conversation","phase":"idle","activity":"idle"}]}),
            ));
        }
        if argv == ["inspect", SESSION] {
            assert!(guarded, "Encrypted requests must bind the native machine");
            self.inspect_entered.set();
            if self.block_inspect {
                tokio::time::timeout(Duration::from_secs(5), self.inspect_release.wait())
                    .await
                    .expect("Stop fixture must release its native barrier");
            }
            self.inspect_finished.set(true);
            return Ok(Output::Json(
                json!({"name":SESSION,"run_id":"synthetic-run","conversation_id":"synthetic-conversation","private":PRIVATE}),
            ));
        }
        Err(NativeError::Failed(
            "Synthetic native capability unavailable",
        ))
    }
}
async fn api(relay: &Relay, method: &str, path: &str, body: Option<Value>) -> Value {
    relay
        .call(
            method.parse().unwrap(),
            path,
            body.map(|v| serde_json::to_vec(&v).unwrap()),
            3 * 1024 * 1024,
        )
        .await
        .unwrap()
        .unwrap()
}

#[tokio::test]
async fn phone_enrollment_encrypted_catalog_native_dispatch_and_immutable_receipt() {
    transport_flow(false).await;
}

#[tokio::test]
async fn stop_during_encrypted_native_dispatch_drains_and_retains_identity() {
    transport_flow(true).await;
}

async fn transport_flow(stop_during_native: bool) {
    let backend = common::Backend::new(Config {
        public_enrollment: true,
        large_payload_slots: 2,
        max_spool_bytes: 164 * 1024 * 1024,
        background: false,
        ..Config::default()
    })
    .await;
    let store = backend.store().await;
    let server = support::Server::start(&store).await;
    let local = tempfile::tempdir().unwrap();
    let path = local.path().join("connector.json");
    let revision = workspace::public_status(&path).unwrap()["config_revision"].clone();
    workspace::manage(
        "create",
        &path,
        json!({"server_url":server.url,"expected_revision":revision}),
        true,
    )
    .await
    .unwrap();
    let revision = workspace::public_status(&path).unwrap()["config_revision"].clone();
    let invitation = workspace::manage(
        "invite-phone",
        &path,
        json!({"expected_revision":revision}),
        true,
    )
    .await
    .unwrap();
    let code = invitation["code"]
        .as_str()
        .unwrap()
        .strip_prefix("ze1.")
        .unwrap();
    let invitation: Invitation =
        serde_json::from_slice(&crypto::decode(code, 3072).unwrap()).unwrap();
    let phone = Identity::generate(uuid::Uuid::new_v4().to_string(), Role::Phone).unwrap();
    let candidate =
        EnrollmentCandidate::create(&invitation, &phone, zerus_mobile_connector::now() as u64)
            .unwrap();
    let token = crypto::encode(&[7; 32]);
    let transport = zerus_mobile_connector::url::Transport::new(&server.url).unwrap();
    let mobile = Relay::new(&transport, &token, None);
    api(
        &mobile,
        "POST",
        "/v2/enrollments",
        Some(json!({"candidate":candidate,"service_token":token})),
    )
    .await;
    let environment = config::Environment {
        home: Some(local.path().into()),
        cwd: local.path().into(),
    };
    let started =
        zerus_mobile_connector::start(&path, true, &environment, zerus_mobile_connector::now())
            .unwrap();
    let journal: Journal = started.journal;
    let private_dir = started.config.encrypted_workspace.unwrap();
    let relay = Relay::new(&started.config.transport, &started.config.token, None);
    let connector = Rc::new(Connector::new(
        FixtureNative {
            block_inspect: stop_during_native,
            ..FixtureNative::default()
        },
        journal,
        Settings {
            interval: 0.1,
            binding: started.config.binding,
            default_timeout: 1.0,
            clock_scale: 1.0,
        },
        Box::new(zerus_mobile_connector::now),
    ));
    let local_tasks = tokio::task::LocalSet::new();
    local_tasks.run_until(async {
        let external_stop=Event::default();
        let submitted_identity=RefCell::new(None);
        let run=encrypted_runtime::run(&connector,&relay,&private_dir,async {
            external_stop.wait().await;
            connector.stopping.set();
        });
        let flow=async {
            let status=loop {
                let status=api(&mobile,"GET",&format!("/v2/enrollments/{}",phone.public().device_id),None).await;
                if status["state"]=="active"{break status}
                tokio::time::sleep(Duration::from_millis(25)).await;
            };
            let bootstrap:SignedMembershipSnapshot=serde_json::from_value(status["bootstrap"].clone()).unwrap();let mut membership=bootstrap.verify_bootstrap(&invitation).unwrap();
            let approval:SignedEnrollment=serde_json::from_value(status["enrollment"].clone()).unwrap();approval.verify(&candidate,&invitation.issuer).unwrap();membership.apply_next(&approval.membership).unwrap();
            let catalog=loop {
                let page=api(&mobile,"GET","/v2/computers?limit=100",None).await;
                if page["computers"].as_array().unwrap().first().is_some_and(|row|row["envelope"].is_object()){break page}
                tokio::time::sleep(Duration::from_millis(25)).await;
            };
            let row=&catalog["computers"][0];let envelope:EncryptedEnvelope=serde_json::from_value(row["envelope"].clone()).unwrap();
            let sender=membership.authorized_sender(&envelope).unwrap();let plaintext=crypto::open(&phone,sender,&envelope.header,&envelope).unwrap();let computer:Value=serde_json::from_slice(&plaintext).unwrap();
            assert_eq!(computer["id"],MACHINE);assert!(computer["via"].is_null());assert_eq!(computer["snapshot"]["sessions"][0]["name"],SESSION);
            assert!(!serde_json::to_string(&catalog).unwrap().contains(SESSION));
            let request_id=uuid::Uuid::new_v4().to_string();let gateway=membership.members.get(&envelope.header.gateway_id).unwrap();
            let header=Header{workspace_id:membership.workspace_id.clone(),sender_device_id:phone.public().device_id.clone(),recipient_device_id:gateway.device_id.clone(),sender_key_generation:1,recipient_key_generation:1,gateway_id:gateway.device_id.clone(),route_id:envelope.header.route_id.clone(),request_id:request_id.clone(),kind:PayloadKind::Request,operation_class:OperationClass::Read,expires_at:zerus_mobile_connector::now() as u64+300,membership_revision:membership.revision,membership_head:membership.head.clone()};
            let body=json!({"request_id":request_id,"computer_id":MACHINE,"operation":"inspect","session":SESSION,"encrypted_transport":computer["encrypted_transport"]});
            let encrypted=crypto::seal(&phone,gateway,header,&serde_json::to_vec(&body).unwrap()).unwrap();
            *submitted_identity.borrow_mut()=Some((request_id.clone(),encrypted.digest().unwrap(),serde_json::to_string(&encrypted.header).unwrap()));
            let submitted=json!({"envelope":encrypted});
            assert!(!submitted.to_string().contains(SESSION));api(&mobile,"POST","/v2/requests",Some(submitted.clone())).await;
            if stop_during_native {
                tokio::time::timeout(Duration::from_secs(5),connector.native.inspect_entered.wait()).await.expect("Native dispatch must enter before Stop");
                assert!(!connector.native.inspect_finished.get());
                external_stop.set();connector.stopping.wait().await;
                // Stop monitoring ticks every250ms. Keep native work in flight
                // beyond that tick to expose dropping it when monitors finish.
                tokio::time::sleep(Duration::from_millis(350)).await;
                assert!(!connector.native.inspect_finished.get());
                connector.native.inspect_release.set();
                return;
            }
            let result=loop {
                let receipt=api(&mobile,"GET",&format!("/v2/requests/{request_id}"),None).await;
                if receipt["envelope"].is_object(){break receipt}
                tokio::time::sleep(Duration::from_millis(25)).await;
            };
            assert!(!result.to_string().contains(PRIVATE));let result_envelope:EncryptedEnvelope=serde_json::from_value(result["envelope"].clone()).unwrap();
            let result_header=&result_envelope.header;
            assert_eq!(result_header.kind,PayloadKind::Result);assert_eq!(result_header.operation_class,OperationClass::Read);
            assert_eq!(result_header.request_id,request_id);assert_eq!(result_header.sender_device_id,gateway.device_id);
            assert_eq!(result_header.recipient_device_id,phone.public().device_id);assert_eq!(result_header.gateway_id,envelope.header.gateway_id);assert_eq!(result_header.route_id,envelope.header.route_id);assert!(result_header.expires_at>zerus_mobile_connector::now() as u64);
            let sender=membership.authorized_sender(&result_envelope).unwrap();let plaintext=crypto::open(&phone,sender,&result_envelope.header,&result_envelope).unwrap();let receipt:Value=serde_json::from_slice(&plaintext).unwrap();
            assert_eq!(receipt["request_id"],request_id);assert_eq!(receipt["state"],"completed");assert_eq!(receipt["result"]["private"],PRIVATE);
            assert_eq!(api(&mobile,"POST","/v2/requests",Some(submitted)).await["envelope"],result["envelope"]);
            assert_eq!(connector.native.calls.borrow().iter().filter(|argv|argv.iter().any(|arg|arg=="inspect")).count(),1);
            connector.stopping.set();
        };
        let (code,())=tokio::time::timeout(Duration::from_secs(30),async{tokio::join!(run,flow)}).await.expect("Encrypted transport must settle within its finite fixture deadline");assert_eq!(code,0);
        assert!(connector.native.inspect_finished.get(),"Stop must drain the native call");
        assert_eq!(connector.native.calls.borrow().iter().filter(|argv|argv.iter().any(|arg|arg=="inspect")).count(),1);
        let (id,digest,header)=submitted_identity.into_inner().unwrap();
        let native:Value=serde_json::from_str(&connector.journal.encrypted_native_receipt(&id).unwrap().expect("Native completion must remain durable after Stop")).unwrap();
        assert_eq!(native["state"],"completed");assert_eq!(native["result"]["private"],PRIVATE);
        assert!(!connector.journal.encrypted_reserve(&id,&digest,&header,zerus_mobile_connector::now()).unwrap());
        assert!(connector.journal.encrypted_reserve(&id,"changed digest",&header,zerus_mobile_connector::now()).is_err());
        if stop_during_native {
            let pending=connector.journal.encrypted_pending().unwrap();assert_eq!(pending.len(),1);assert_eq!(pending[0].0,id);assert!(pending[0].2.is_none(),"Stop must retain pending delivery without another result HTTP call");
        }
    }).await;
    drop(connector);
    server.stop().await;
    store.db.close().await;
}
