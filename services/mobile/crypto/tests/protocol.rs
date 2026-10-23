//! Synthetic fixture keys only. Never use these deterministic keys in service.
use hpke::{Deserializable, Kem, OpModeR, Serializable};
use serde_json::{json, Value};
use sha2::Digest;
use std::collections::BTreeSet;
use zerus_mobile_crypto::*;

fn identity(id: &str, role: Role, seed: u8) -> Identity {
    let signing = ed25519_dalek::SigningKey::from_bytes(&[seed; 32]);
    let kem =
        <hpke::kem::X25519HkdfSha256 as Kem>::PrivateKey::from_bytes(&[seed + 64; 32]).unwrap();
    Identity::from_record(&IdentityRecord {
        version: VERSION,
        device: DevicePublic {
            device_id: id.into(),
            role,
            key_generation: 1,
            signing_public_key: encode(&signing.verifying_key().to_bytes()),
            encryption_public_key: encode(&hpke::kem::X25519HkdfSha256::sk_to_pk(&kem).to_bytes()),
        },
        signing_private_key: encode(&[seed; 32]),
        encryption_private_key: encode(&[seed + 64; 32]),
    })
    .unwrap()
}
fn genesis(issuer: &Identity) -> SignedMembership {
    SignedMembership::sign(
        MembershipStatement {
            workspace_id: "workspace-fixture".into(),
            revision: 0,
            previous_head: encode(&[0; 32]),
            issuer_device_id: issuer.public().device_id.clone(),
            issuer_key_generation: 1,
            action: MembershipAction::Add,
            device: Some(issuer.public().clone()),
            revoked_device_id: None,
        },
        issuer,
    )
    .unwrap()
}
fn add(state: &MembershipState, issuer: &Identity, member: &Identity) -> SignedMembership {
    SignedMembership::sign(
        MembershipStatement {
            workspace_id: state.workspace_id.clone(),
            revision: state.revision + 1,
            previous_head: state.head.clone(),
            issuer_device_id: issuer.public().device_id.clone(),
            issuer_key_generation: 1,
            action: MembershipAction::Add,
            device: Some(member.public().clone()),
            revoked_device_id: None,
        },
        issuer,
    )
    .unwrap()
}
fn revoke(state: &MembershipState, issuer: &Identity, member: &Identity) -> SignedMembership {
    SignedMembership::sign(
        MembershipStatement {
            workspace_id: state.workspace_id.clone(),
            revision: state.revision + 1,
            previous_head: state.head.clone(),
            issuer_device_id: issuer.public().device_id.clone(),
            issuer_key_generation: 1,
            action: MembershipAction::Revoke,
            device: None,
            revoked_device_id: Some(member.public().device_id.clone()),
        },
        issuer,
    )
    .unwrap()
}
fn header(state: &MembershipState, sender: &Identity, recipient: &Identity) -> Header {
    Header {
        workspace_id: state.workspace_id.clone(),
        sender_device_id: sender.public().device_id.clone(),
        recipient_device_id: recipient.public().device_id.clone(),
        sender_key_generation: 1,
        recipient_key_generation: 1,
        gateway_id: recipient.public().device_id.clone(),
        route_id: "opaque-route".into(),
        request_id: "request-fixture".into(),
        kind: PayloadKind::Request,
        operation_class: OperationClass::Mutation,
        expires_at: 2_000_000_000,
        membership_revision: state.revision,
        membership_head: state.head.clone(),
    }
}

#[test]
fn published_rfc9180_x25519_aes256_known_answer() {
    let v: Value = serde_json::from_str(include_str!("vectors/rfc9180.json")).unwrap();
    let bytes = |key: &str| hex::decode(v[key].as_str().unwrap()).unwrap();
    type K = hpke::kem::X25519HkdfSha256;
    let sk = <K as Kem>::PrivateKey::from_bytes(&bytes("skRm")).unwrap();
    assert_eq!(K::sk_to_pk(&sk).to_bytes().as_slice(), bytes("pkRm"));
    let enc = <K as Kem>::EncappedKey::from_bytes(&bytes("enc")).unwrap();
    let mut context = hpke::setup_receiver::<hpke::aead::AesGcm256, hpke::kdf::HkdfSha256, K>(
        &OpModeR::Base,
        &sk,
        &enc,
        &bytes("info"),
    )
    .unwrap();
    let e = &v["encryption"];
    let decode = |key: &str| hex::decode(e[key].as_str().unwrap()).unwrap();
    assert_eq!(
        context.open(&decode("ct"), &decode("aad")).unwrap(),
        decode("pt")
    );
}

#[test]
fn ciphertext_tampering_and_every_header_binding_fail_closed() {
    let gateway = identity("gateway", Role::Computer, 1);
    let phone = identity("phone", Role::Phone, 2);
    let mut state = MembershipState::from_genesis(&genesis(&gateway)).unwrap();
    state.apply_next(&add(&state, &gateway, &phone)).unwrap();
    let expected = header(&state, &phone, &gateway);
    let envelope = seal(
        &phone,
        gateway.public(),
        expected.clone(),
        br#"{"operation":"send","text":"private"}"#,
    )
    .unwrap();
    assert_eq!(
        &**open(&gateway, phone.public(), &expected, &envelope).unwrap(),
        br#"{"operation":"send","text":"private"}"#
    );
    let value = serde_json::to_value(&envelope).unwrap();
    for (field, replacement) in [
        ("workspace_id", json!("other-workspace")),
        ("sender_device_id", json!("other-sender")),
        ("recipient_device_id", json!("other-recipient")),
        ("sender_key_generation", json!(2)),
        ("recipient_key_generation", json!(2)),
        ("gateway_id", json!("other-gateway")),
        ("route_id", json!("other-route")),
        ("request_id", json!("other-request")),
        ("kind", json!("result")),
        ("operation_class", json!("read")),
        ("expires_at", json!(2_000_000_001u64)),
        ("membership_revision", json!(2)),
        ("membership_head", json!(encode(&[7; 32]))),
    ] {
        let mut altered = value.clone();
        altered["header"][field] = replacement;
        let altered: EncryptedEnvelope = serde_json::from_value(altered).unwrap();
        // Even when the transport caller derives its expectation from the
        // altered routing fields, authenticated bytes reject the alteration.
        assert!(
            open(&gateway, phone.public(), &altered.header, &altered).is_err(),
            "{field}"
        );
    }
    for field in ["enc", "ciphertext", "signature"] {
        let mut altered = envelope.clone();
        let target = match field {
            "enc" => &mut altered.enc,
            "ciphertext" => &mut altered.ciphertext,
            _ => &mut altered.signature,
        };
        let mut bytes = decode(target, MAX_PAYLOAD_BYTES + 16).unwrap();
        bytes[0] ^= 1;
        *target = encode(&bytes);
        assert!(
            open(&gateway, phone.public(), &expected, &altered).is_err(),
            "{field}"
        );
    }
    let relay = identity("relay-attacker", Role::Computer, 3);
    let mut forged = envelope.clone();
    forged.signature = relay.sign(&forged.signing_bytes().unwrap());
    assert!(open(&gateway, phone.public(), &expected, &forged).is_err());
    assert!(open(&gateway, relay.public(), &expected, &forged).is_err());
    let mut substituted = phone.public().clone();
    substituted.signing_public_key = relay.public().signing_public_key.clone();
    assert!(envelope.verify_sender(&substituted).is_err());
}

#[test]
fn fresh_contexts_do_not_reuse_ciphertext_and_retry_uses_exact_envelope() {
    let gateway = identity("gateway", Role::Computer, 1);
    let phone = identity("phone", Role::Phone, 2);
    let state = MembershipState::from_genesis(&genesis(&gateway)).unwrap();
    let h = header(&state, &phone, &gateway);
    let first = seal(&phone, gateway.public(), h.clone(), b"same operation").unwrap();
    let second = seal(&phone, gateway.public(), h.clone(), b"same operation").unwrap();
    assert_ne!(first.enc, second.enc);
    assert_ne!(first.digest().unwrap(), second.digest().unwrap());
    let persisted = serde_json::to_vec(&first).unwrap();
    let restored: EncryptedEnvelope = serde_json::from_slice(&persisted).unwrap();
    assert_eq!(first.digest().unwrap(), restored.digest().unwrap());
    assert_eq!(
        &**open(&gateway, phone.public(), &h, &restored).unwrap(),
        b"same operation"
    );
    // Cryptography alone deliberately accepts a replay: integration MUST
    // consult its permanent request journal before invoking any native action.
    assert!(open(&gateway, phone.public(), &h, &restored).is_ok());
}

#[test]
fn founding_computer_can_retire_without_invalidating_prior_admissions() {
    let founder = identity("founder", Role::Computer, 1);
    let peer = identity("peer", Role::Computer, 2);
    let phone = identity("phone", Role::Phone, 3);
    let newcomer = identity("newcomer", Role::Phone, 4);
    let mut state = MembershipState::from_genesis(&genesis(&founder)).unwrap();
    let mut retired = state.clone();
    retired
        .apply_next(&revoke(&state, &founder, &founder))
        .unwrap();
    retired.validate().unwrap();
    assert!(retired.members.is_empty());
    state.apply_next(&add(&state, &founder, &peer)).unwrap();
    state.apply_next(&add(&state, &founder, &phone)).unwrap();
    let before = state.clone();
    let retire = revoke(&state, &peer, &founder);
    state.apply_next(&retire).unwrap();
    assert!(state.members.contains_key("phone"));
    assert!(state.members.contains_key("peer"));
    let unchanged = state.clone();
    assert!(state.apply_next(&add(&state, &founder, &newcomer)).is_err());
    assert_eq!(state, unchanged);
    assert!(state.apply_next(&add(&state, &peer, &founder)).is_err());
    assert_eq!(state, unchanged);
    state.apply_next(&add(&state, &peer, &newcomer)).unwrap();
    let mut replayed = before.clone();
    replayed.apply_next(&retire).unwrap();
    assert_eq!(replayed, unchanged);
    assert!(replayed.apply_next(&retire).is_err());
}

#[test]
fn forks_rollback_role_escalation_and_key_replacement_are_rejected() {
    let founder = identity("founder", Role::Computer, 1);
    let peer = identity("peer", Role::Computer, 2);
    let phone = identity("phone", Role::Phone, 3);
    let other = identity("other", Role::Phone, 4);
    let initial = MembershipState::from_genesis(&genesis(&founder)).unwrap();
    let branch_a = add(&initial, &founder, &peer);
    let branch_b = add(&initial, &founder, &phone);
    let mut state = initial.clone();
    state.apply_next(&branch_a).unwrap();
    assert!(state.apply_next(&branch_b).is_err());
    assert!(state.apply_next(&genesis(&founder)).is_err());
    state.apply_next(&add(&state, &founder, &phone)).unwrap();
    let mut escalation = add(&state, &founder, &other);
    escalation.statement.issuer_device_id = "phone".into();
    escalation.signature = phone.sign(&escalation.statement.canonical_bytes().unwrap());
    assert!(state.apply_next(&escalation).is_err());
    let replacement = identity("phone", Role::Computer, 5);
    assert!(state
        .apply_next(&add(&state, &founder, &replacement))
        .is_err());
    let persisted = serde_json::to_vec(&state).unwrap();
    let restored: MembershipState = serde_json::from_slice(&persisted).unwrap();
    assert_eq!(state, restored);
    assert!(restored.clone().apply_next(&branch_b).is_err());
}

#[test]
fn delayed_verified_history_can_arrive_but_revoked_senders_cannot() {
    let gateway = identity("gateway", Role::Computer, 1);
    let phone = identity("phone", Role::Phone, 2);
    let other = identity("other", Role::Phone, 3);
    let mut state = MembershipState::from_genesis(&genesis(&gateway)).unwrap();
    state.apply_next(&add(&state, &gateway, &phone)).unwrap();
    let past = state.clone();
    let encrypted = seal(
        &phone,
        gateway.public(),
        header(&state, &phone, &gateway),
        b"private",
    )
    .unwrap();
    state.apply_next(&add(&state, &gateway, &other)).unwrap();
    assert!(state.authorized_sender(&encrypted).is_err());
    assert_eq!(
        state
            .authorized_sender_at_known_head(&encrypted, &past.head)
            .unwrap(),
        phone.public()
    );
    state.apply_next(&revoke(&state, &gateway, &phone)).unwrap();
    assert!(state
        .authorized_sender_at_known_head(&encrypted, &past.head)
        .is_err());
}

#[test]
fn malicious_relay_cannot_substitute_join_keys_or_approve_an_invitation() {
    let gateway = identity("gateway", Role::Computer, 1);
    let phone = identity("phone", Role::Phone, 2);
    let attacker = identity("attacker", Role::Phone, 3);
    let state = MembershipState::from_genesis(&genesis(&gateway)).unwrap();
    let invite = Invitation::generate(
        state.workspace_id.clone(),
        "invitation-fixture".into(),
        Role::Phone,
        gateway.public().clone(),
        2_000_000_000,
        state.revision,
        state.head.clone(),
    )
    .unwrap();
    let candidate = EnrollmentCandidate::create(&invite, &phone, 1_000).unwrap();
    candidate.verify_invitation(&invite, 1_000).unwrap();
    let mut substituted = candidate.clone();
    substituted.device = attacker.public().clone();
    substituted.signature = attacker.sign(&substituted.signing_bytes().unwrap());
    assert!(substituted.verify_signature().is_ok());
    assert!(substituted.verify_invitation(&invite, 1_000).is_err());
    assert!(candidate.verify_invitation(&invite, 2_000_000_000).is_err());
    let mut wrong_purpose = candidate.clone();
    wrong_purpose.purpose = Role::Computer;
    assert!(wrong_purpose.verify_invitation(&invite, 1_000).is_err());
    let approval =
        SignedEnrollment::sign(&candidate, add(&state, &gateway, &phone), &gateway).unwrap();
    approval.verify(&candidate, gateway.public()).unwrap();
    assert!(approval.verify(&substituted, gateway.public()).is_err());
    let snapshot = SignedMembershipSnapshot::sign(state.clone(), &gateway).unwrap();
    assert_eq!(snapshot.verify_bootstrap(&invite).unwrap(), state);
    let mut altered = snapshot.clone();
    altered.state.head = encode(&[4; 32]);
    assert!(altered.verify_bootstrap(&invite).is_err());
    let mut altered = snapshot;
    altered
        .state
        .members
        .insert("attacker".into(), attacker.public().clone());
    assert!(altered.verify_bootstrap(&invite).is_err());
    // Possession of a service bearer or public candidate gives neither secret
    // proof-generation capability nor the inviter's signing capability.
    let mut forged = approval;
    forged.signature = attacker.sign(&forged.signing_bytes().unwrap());
    assert!(forged.verify(&candidate, gateway.public()).is_err());
}

#[test]
fn malformed_keys_lengths_versions_and_extra_json_fields_fail_closed() {
    let device = identity("device", Role::Computer, 1);
    let mut bad = device.public().clone();
    bad.signing_public_key = encode(&[0; 32]);
    assert!(bad.validate().is_err());
    let mut bad = device.public().clone();
    bad.encryption_public_key = encode(&[0; 32]);
    assert!(bad.validate().is_err());
    let mut record = device.to_record();
    record.signing_private_key = encode(&[9; 32]);
    assert!(Identity::from_record(&record).is_err());
    assert!(decode_fixed::<32>(&format!("{}=", encode(&[1; 32]))).is_err());
    assert!(decode_fixed::<32>(&encode(&[1; 31])).is_err());
    let mut value = serde_json::to_value(device.public()).unwrap();
    value["unexpected"] = json!(true);
    assert!(serde_json::from_value::<DevicePublic>(value).is_err());
    let mut bad = device.public().clone();
    bad.key_generation = u64::MAX;
    assert!(bad.validate().is_err());
    let mut bad = device.public().clone();
    bad.device_id = "private/path".into();
    assert!(bad.validate().is_err());
    assert!(decode("_x", 1).is_err()); // nonzero unused base64 bits
}

#[test]
fn streaming_verification_and_digest_preserve_frozen_frames_at_chunk_boundaries() {
    let gateway = identity("gateway", Role::Computer, 1);
    let sender = identity("sender", Role::Phone, 2);
    let state = MembershipState::from_genesis(&genesis(&gateway)).unwrap();
    for length in [16, 17, 49_151, 49_152, 49_153, 98_305] {
        let mut envelope = EncryptedEnvelope {
            version: VERSION,
            header: header(&state, &sender, &gateway),
            enc: encode(&[7; 32]),
            ciphertext: encode(&vec![11; length]),
            signature: String::new(),
        };
        envelope.signature = sender.sign(&envelope.signing_bytes().unwrap());
        envelope.verify_sender(sender.public()).unwrap();
        let expected = sha2::Sha256::digest(
            frame(
                "zerus-e2ee-envelope-digest-v1",
                &[
                    &envelope.signing_bytes().unwrap(),
                    &decode_fixed::<64>(&envelope.signature).unwrap(),
                ],
            )
            .unwrap(),
        );
        assert_eq!(envelope.digest().unwrap(), encode(&expected));
        let mut weak_r = [0; 64];
        weak_r[0] = 1;
        envelope.signature = encode(&weak_r);
        assert!(envelope.verify_sender(sender.public()).is_err());
    }
}

fn protocol_vector() -> Value {
    let gateway = identity("00000000-0000-4000-8000-000000000001", Role::Computer, 1);
    let phone = identity("00000000-0000-4000-8000-000000000002", Role::Phone, 2);
    let genesis = genesis(&gateway);
    let state = MembershipState::from_genesis(&genesis).unwrap();
    let invitation = Invitation {
        version: VERSION,
        workspace_id: state.workspace_id.clone(),
        invitation_id: "00000000-0000-4000-8000-000000000003".into(),
        purpose: Role::Phone,
        issuer: gateway.public().clone(),
        expires_at: 2_000_000_000,
        membership_revision: state.revision,
        membership_head: state.head.clone(),
        secret: encode(&[99; 32]),
    };
    let candidate = EnrollmentCandidate::create(&invitation, &phone, 1_000).unwrap();
    let membership = add(&state, &gateway, &phone);
    let approval = SignedEnrollment::sign(&candidate, membership.clone(), &gateway).unwrap();
    let snapshot = SignedMembershipSnapshot::sign(state.clone(), &gateway).unwrap();
    let mut next = state;
    next.apply_next(&membership).unwrap();
    let h = header(&next, &phone, &gateway);
    let plaintext = br#"{"operation":"send","text":"private fixture"}"#;
    let envelope = seal(&phone, gateway.public(), h.clone(), plaintext).unwrap();
    json!({"warning":"PUBLIC SYNTHETIC TEST KEYS; NEVER USE IN SERVICE", "gateway_identity":gateway.to_record(),"phone_identity":phone.to_record(),"genesis":genesis,"invitation":invitation,"candidate":candidate,"approval":approval,"bootstrap_snapshot":snapshot,"final_state":next,"envelope":envelope,"plaintext_b64url":encode(plaintext),"header_hex":hex::encode(h.canonical_bytes().unwrap()),"device_hex":hex::encode(phone.public().canonical_bytes().unwrap()),"candidate_proof_hex":hex::encode(candidate.proof_bytes().unwrap()),"candidate_signing_hex":hex::encode(candidate.signing_bytes().unwrap()),"approval_signing_hex":hex::encode(approval.signing_bytes().unwrap()),"snapshot_signing_hex":hex::encode(snapshot.signing_bytes().unwrap()),"envelope_signing_hex":hex::encode(envelope.signing_bytes().unwrap())})
}

#[test]
#[ignore = "Explicit maintainer tool: generates a new random HPKE fixture"]
fn regenerate_public_protocol_vector() {
    std::fs::write(
        concat!(
            env!("CARGO_MANIFEST_DIR"),
            "/tests/vectors/protocol-v1.json"
        ),
        format!(
            "{}\n",
            serde_json::to_string_pretty(&protocol_vector()).unwrap()
        ),
    )
    .unwrap();
}

#[test]
#[ignore = "Explicit maintainer tool: exports standard Dalek torsion encodings"]
fn regenerate_rejected_key_vectors() {
    let mut rejected = BTreeSet::new();
    for point in &curve25519_dalek::constants::EIGHT_TORSION {
        let bytes = point.compress().to_bytes();
        rejected.insert(encode(&bytes));
        let mut flipped = bytes;
        flipped[31] ^= 0x80;
        rejected.insert(encode(&flipped));
    }
    for n in 0..19u8 {
        let mut y = [0xff; 32];
        y[0] = 0xed + n;
        y[31] = 0x7f;
        rejected.insert(encode(&y));
        y[31] |= 0x80;
        rejected.insert(encode(&y));
    }
    let vector = json!({"source":"curve25519-dalek 5.0.0 constants::EIGHT_TORSION; RFC8032 canonical y<p", "ed25519_rejected_public_keys":rejected});
    std::fs::write(
        concat!(
            env!("CARGO_MANIFEST_DIR"),
            "/tests/vectors/rejected-keys-v1.json"
        ),
        format!("{}\n", serde_json::to_string_pretty(&vector).unwrap()),
    )
    .unwrap();
}

#[test]
fn shared_weak_and_noncanonical_ed25519_key_vectors_are_rejected() {
    let value: Value = serde_json::from_str(
        &std::fs::read_to_string(concat!(
            env!("CARGO_MANIFEST_DIR"),
            "/tests/vectors/rejected-keys-v1.json"
        ))
        .unwrap(),
    )
    .unwrap();
    let good = identity("device", Role::Computer, 1);
    for key in value["ed25519_rejected_public_keys"].as_array().unwrap() {
        let mut bad = good.public().clone();
        bad.signing_public_key = key.as_str().unwrap().to_string();
        assert!(bad.validate().is_err(), "{key}");
    }
}

#[test]
fn public_cross_platform_vector_decodes_verifies_and_decrypts() {
    let value: Value = serde_json::from_str(
        &std::fs::read_to_string(concat!(
            env!("CARGO_MANIFEST_DIR"),
            "/tests/vectors/protocol-v1.json"
        ))
        .unwrap(),
    )
    .unwrap();
    let gateway = Identity::from_record(
        &serde_json::from_value::<IdentityRecord>(value["gateway_identity"].clone()).unwrap(),
    )
    .unwrap();
    let phone = Identity::from_record(
        &serde_json::from_value::<IdentityRecord>(value["phone_identity"].clone()).unwrap(),
    )
    .unwrap();
    let envelope: EncryptedEnvelope = serde_json::from_value(value["envelope"].clone()).unwrap();
    assert_eq!(
        hex::encode(envelope.header.canonical_bytes().unwrap()),
        value["header_hex"]
    );
    assert_eq!(
        hex::encode(envelope.signing_bytes().unwrap()),
        value["envelope_signing_hex"]
    );
    assert_eq!(
        &**open(&gateway, phone.public(), &envelope.header, &envelope).unwrap(),
        decode(
            value["plaintext_b64url"].as_str().unwrap(),
            MAX_PAYLOAD_BYTES
        )
        .unwrap()
    );
    let invite: Invitation = serde_json::from_value(value["invitation"].clone()).unwrap();
    let candidate: EnrollmentCandidate =
        serde_json::from_value(value["candidate"].clone()).unwrap();
    candidate.verify_invitation(&invite, 1000).unwrap();
    assert_eq!(
        hex::encode(candidate.proof_bytes().unwrap()),
        value["candidate_proof_hex"]
    );
    assert_eq!(
        hex::encode(candidate.signing_bytes().unwrap()),
        value["candidate_signing_hex"]
    );
    let approval: SignedEnrollment = serde_json::from_value(value["approval"].clone()).unwrap();
    approval.verify(&candidate, gateway.public()).unwrap();
    assert_eq!(
        hex::encode(approval.signing_bytes().unwrap()),
        value["approval_signing_hex"]
    );
    let snapshot: SignedMembershipSnapshot =
        serde_json::from_value(value["bootstrap_snapshot"].clone()).unwrap();
    assert_eq!(
        hex::encode(snapshot.signing_bytes().unwrap()),
        value["snapshot_signing_hex"]
    );
    let mut state = snapshot.verify_bootstrap(&invite).unwrap();
    state.apply_next(&approval.membership).unwrap();
    assert_eq!(serde_json::to_value(&state).unwrap(), value["final_state"]);
    state.authorized_sender(&envelope).unwrap();
}
