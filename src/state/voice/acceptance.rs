//! Durable admission barriers. An older envelope cannot cross a recorded
//! refusal; only an explicit retry with the current scoped proof can admit the
//! same immutable native message. Native delivery receipts are unchanged.
use super::*;

pub(super) enum Admission {
    Accepted(Value),
    Refused(Value),
}

struct HashWriter(Sha256);
impl io::Write for HashWriter {
    fn write(&mut self, bytes: &[u8]) -> io::Result<usize> {
        self.0.update(bytes);
        Ok(bytes.len())
    }
    fn flush(&mut self) -> io::Result<()> {
        Ok(())
    }
}
fn digest(request: &Value, target: &Value, normalized: bool) -> Result<String> {
    let mut fields: std::collections::BTreeMap<&str, &Value> = request
        .as_object()
        .ok_or("Invalid voice admission")?
        .iter()
        .map(|(key, value)| (key.as_str(), value))
        .collect();
    if normalized {
        fields.remove("message_id");
        fields.remove("rejection");
        fields.insert("request_id", rejection::message_id(request));
    }
    let mut hash = HashWriter(Sha256::new());
    // Serialize the same ordered array/map as the original acceptance digest,
    // without cloning or allocating another complete base64 media envelope.
    serde_json::to_writer(
        &mut hash,
        &(
            &target["agent"],
            &target["account_id"],
            &target["agent_home"],
            fields,
        ),
    )
    .map_err(err)?;
    Ok(format!("{:x}", hash.0.finalize()))
}
fn recorded(
    tx: &rusqlite::Transaction<'_>,
    operation: &str,
    digest: &str,
    id: &str,
    reply: &Value,
) -> Result<()> {
    tx.execute(
        "INSERT OR IGNORE INTO voice_admissions VALUES(?,?,?,?)",
        params![operation, digest, id, reply.to_string()],
    )
    .map_err(err)?;
    Ok(())
}
fn proof_for(proof: &Value, request: &Value) -> Result<Value> {
    let mut value = proof.clone();
    value["request_id"] = request["request_id"].clone();
    if !rejection::result_valid(&json!({"schema":1,"rejection":value}), request) {
        return Err("Voice refusal does not match the exact recording and request".into());
    }
    Ok(value)
}
impl Store {
    pub(super) fn admit(
        &mut self,
        request: &Value,
        target: &Value,
        job: &Value,
        audio: &[u8],
        video: &[u8],
        route_unavailable: bool,
    ) -> Result<Admission> {
        let native_digest = digest(request, target, true)?;
        let operation_digest = digest(request, target, false)?;
        let operation = string(request, "request_id");
        let id = string(job, "id");
        let tx = self
            .db
            .transaction_with_behavior(rusqlite::TransactionBehavior::Immediate)
            .map_err(err)?;
        let old_operation: Option<(String, String)> = tx
            .query_row(
                "SELECT digest,reply FROM voice_admissions WHERE id=?",
                [operation],
                |row| Ok((row.get(0)?, row.get(1)?)),
            )
            .optional()
            .map_err(err)?;
        if old_operation
            .as_ref()
            .is_some_and(|(digest, _)| digest != &operation_digest)
        {
            return Err("Voice operation ID was already used for different content".into());
        }
        let accepted: Option<(String, String)> = tx
            .query_row("SELECT digest,body FROM messages WHERE id=?", [id], |row| {
                Ok((row.get(0)?, row.get(1)?))
            })
            .optional()
            .map_err(err)?;
        if let Some((digest, body)) = accepted {
            if digest != native_digest {
                return Err("Voice request ID was already used for different content".into());
            }
            // A durable accepted message wins over route failures and stale
            // refusal receipts, including an old envelope arriving after retry.
            return Ok(Admission::Accepted(
                serde_json::from_str(&body).map_err(err)?,
            ));
        }
        if let Some((_, reply)) = old_operation {
            let proof: Value = serde_json::from_str(&reply).map_err(err)?;
            return Ok(Admission::Refused(proof_for(&proof, request)?));
        }
        let operations: i64 = tx
            .query_row("SELECT count(*) FROM voice_admissions", [], |row| {
                row.get(0)
            })
            .map_err(err)?;
        if operations >= 40_000 {
            return Err("Voice admission receipt storage is full".into());
        }
        let refused: Option<(String, String)> = tx
            .query_row(
                "SELECT digest,proof FROM voice_refusals WHERE id=?",
                [id],
                |row| Ok((row.get(0)?, row.get(1)?)),
            )
            .optional()
            .map_err(err)?;
        let previous = refused
            .map(|(digest, proof)| -> Result<Value> {
                if digest != native_digest {
                    return Err("Voice request ID was already used for different content".into());
                }
                serde_json::from_str(&proof).map_err(err)
            })
            .transpose()?;
        if let Some(previous) = &previous {
            let retry = &request["rejection"];
            let authorized = retry["retry_token"] == previous["retry_token"]
                && retry["generation"] == previous["generation"]
                && retry["payload_sha256"] == previous["payload_sha256"]
                && retry["intent"] == previous["intent"];
            if !authorized {
                let proof = proof_for(previous, request)?;
                recorded(&tx, operation, &operation_digest, id, &proof)?;
                tx.commit().map_err(err)?;
                return Ok(Admission::Refused(proof));
            }
        } else if request.get("rejection").is_some() {
            return Err("Voice retry refusal is not known on this computer".into());
        }
        let accepted = if route_unavailable {
            Err("Speech route is unavailable".into())
        } else {
            Self::insert_media_in(&tx, job, &native_digest, audio, video)
        };
        let reason = match accepted {
            Ok(accepted) => {
                recorded(
                    &tx,
                    operation,
                    &operation_digest,
                    id,
                    &json!({"accepted":id}),
                )?;
                tx.commit().map_err(err)?;
                return Ok(Admission::Accepted(accepted));
            }
            Err(reason) => reason,
        };
        let (code, detail) = match reason.as_str() {
            "Speech route is unavailable" => (
                "route_unavailable",
                "The selected speech route is unavailable. The recording was not accepted.",
            ),
            "Voice storage is full on this conversation computer" => (
                "storage_full",
                "Voice storage is full. The recording was not accepted.",
            ),
            "Voice processing queue is full on this conversation computer" => (
                "queue_full",
                "The voice queue is full. The recording was not accepted.",
            ),
            _ => return Err(reason),
        };
        if previous.is_none() {
            let refusals: i64 = tx
                .query_row("SELECT count(*) FROM voice_refusals", [], |row| row.get(0))
                .map_err(err)?;
            if refusals >= 10_000 {
                return Err("Voice refusal receipt storage is full".into());
            }
        }
        let generation = previous
            .as_ref()
            .and_then(|proof| proof["generation"].as_u64())
            .unwrap_or(0)
            .checked_add(1)
            .filter(|value| *value <= i64::MAX as u64)
            .ok_or("Voice refusal generation exhausted")?;
        let proof = json!({"schema":1,"state":"rejected_before_acceptance","request_id":operation,"message_id":id,
            "run_id":request["expected_run_id"],"conversation_id":request["expected_conversation_id"],
            "intent":rejection::intent(request)?,"payload_sha256":native_digest,"generation":generation,
            "retry_token":uuid::Uuid::new_v4().to_string(),"code":code,"detail":detail});
        let proof = proof_for(&proof, request)?;
        tx.execute("INSERT INTO voice_refusals VALUES(?,?,?,?) ON CONFLICT(id) DO UPDATE SET proof=excluded.proof",
            params![id,native_digest,job.to_string(),proof.to_string()]).map_err(err)?;
        recorded(&tx, operation, &operation_digest, id, &proof)?;
        tx.commit().map_err(err)?;
        Ok(Admission::Refused(proof))
    }

    pub(super) fn refusal(&self, request: &Value, target: &Value) -> Result<Option<Value>> {
        let row:Option<(String,String)> = self.db.query_row("SELECT owner,proof FROM voice_refusals WHERE id=?1 AND NOT EXISTS(SELECT 1 FROM messages WHERE id=?1)",[string(request,"message_id")],
            |row|Ok((row.get(0)?,row.get(1)?))).optional().map_err(err)?;
        row.map(|(owner, proof)| -> Result<Value> {
            let owner: Value = serde_json::from_str(&owner).map_err(err)?;
            if !belongs(&owner, target) {
                return Err("Voice refusal belongs to another conversation".into());
            }
            proof_for(&serde_json::from_str(&proof).map_err(err)?, request)
        })
        .transpose()
    }
}

#[cfg(all(test, unix))]
mod tests {
    use super::*;

    fn fixture() -> (tempfile::TempDir, Store, Value, Value, Value, Vec<u8>) {
        let directory = tempfile::tempdir().unwrap();
        let store = Store::open_at(&directory.path().join("voice")).unwrap();
        let id = uuid::Uuid::new_v4().to_string();
        let target = json!({"agent":"codex","agent_home":"/synthetic/codex","account_id":"fixture","run_id":"run","conversation_id":"conversation"});
        let request = json!({"action":"submit","request_id":id,"expected_run_id":"run","expected_conversation_id":"conversation",
            "speech":{"machine_id":uuid::Uuid::new_v4().to_string(),"profile_id":"fixture"},
            "audio":{"format":"pcm_s16le","sample_rate":16000,"channels":1,"data_base64":STANDARD.encode(vec![0u8;3200])}});
        let audio = wav(&[0u8; 3200]);
        let job = json!({"id":id,"agent":"codex","agent_home":"/synthetic/codex","account_id":"fixture","run_id":"run","conversation_id":"conversation",
            "state":"queued","created_at":1.,"revision":1,"audio":{"bytes":audio.len(),"waveform":vec![0;64]}});
        (directory, store, target, request, job, audio)
    }
    fn refusal(value: Admission) -> Value {
        match value {
            Admission::Refused(proof) => proof,
            Admission::Accepted(_) => panic!("A refusal barrier admitted an old envelope"),
        }
    }
    fn retry(request: &Value, proof: &Value) -> Value {
        let mut value = request.clone();
        value["message_id"] = request["request_id"].clone();
        value["request_id"] = json!(uuid::Uuid::new_v4().to_string());
        value["rejection"] = proof.clone();
        value
    }

    #[test]
    fn refusal_blocks_old_envelopes_and_explicit_retry_keeps_original_native_identity() {
        let (_directory, mut store, target, request, job, audio) = fixture();
        assert_eq!(
            digest(&request, &target, true).unwrap(),
            format!(
                "{:x}",
                Sha256::digest(
                    serde_json::to_vec(&json!([
                        target["agent"],
                        target["account_id"],
                        target["agent_home"],
                        request
                    ]))
                    .unwrap()
                )
            )
        );
        let proof = refusal(
            store
                .admit(&request, &target, &job, &audio, &[], true)
                .unwrap(),
        );
        assert!(rejection::result_valid(
            &json!({"schema":1,"rejection":proof}),
            &request
        ));
        assert!(store.get(string(&job, "id")).unwrap().is_none());
        // The route recovered, but an old envelope remains durably refused.
        assert_eq!(
            refusal(
                store
                    .admit(&request, &target, &job, &audio, &[], false)
                    .unwrap()
            ),
            proof
        );
        let next = retry(&request, &proof);
        prepare_request(&next).unwrap();
        assert_eq!(
            digest(&next, &target, true).unwrap(),
            digest(&request, &target, true).unwrap()
        );
        for envelope in [&next, &next, &request] {
            let Admission::Accepted(accepted) = store
                .admit(envelope, &target, &job, &audio, &[], false)
                .unwrap()
            else {
                panic!("Exact retry was not admitted")
            };
            assert_eq!(accepted["id"], request["request_id"]);
        }
        assert_eq!(store.audio(string(&job, "id")).unwrap(), audio);
        assert_eq!(
            store
                .db
                .query_row("SELECT count(*) FROM messages", [], |row| row
                    .get::<_, i64>(0))
                .unwrap(),
            1
        );
    }

    #[test]
    fn lost_retry_cannot_use_a_status_copy_of_its_consumed_refusal_as_new_authority() {
        let (_directory, mut store, target, request, job, audio) = fixture();
        let first = refusal(
            store
                .admit(&request, &target, &job, &audio, &[], true)
                .unwrap(),
        );
        let attempted = retry(&request, &first);
        let status = json!({"action":"status","request_id":uuid::Uuid::new_v4().to_string(),"message_id":request["request_id"],
            "expected_run_id":"run","expected_conversation_id":"conversation"});
        let copy = store.refusal(&status, &target).unwrap().unwrap();
        assert_eq!(copy["generation"], attempted["rejection"]["generation"]);
        // Clients require a strictly greater generation. This read cannot
        // revoke the still-in-flight retry's valid token.
        let second = refusal(
            store
                .admit(&attempted, &target, &job, &audio, &[], true)
                .unwrap(),
        );
        assert!(second["generation"].as_u64().unwrap() > first["generation"].as_u64().unwrap());
        assert_ne!(second["retry_token"], first["retry_token"]);
        let stale = retry(&request, &first);
        let stale_result = refusal(
            store
                .admit(&stale, &target, &job, &audio, &[], false)
                .unwrap(),
        );
        assert_eq!(stale_result["generation"], second["generation"]);
        assert!(store.get(string(&job, "id")).unwrap().is_none());
        let authorized = retry(&request, &second);
        assert!(matches!(
            store
                .admit(&authorized, &target, &job, &audio, &[], false)
                .unwrap(),
            Admission::Accepted(_)
        ));
    }

    #[test]
    fn refusal_is_bound_to_operation_target_provider_body_and_original_message() {
        let (_directory, mut store, target, request, job, audio) = fixture();
        let proof = refusal(
            store
                .admit(&request, &target, &job, &audio, &[], true)
                .unwrap(),
        );
        let next = retry(&request, &proof);
        for (field, value) in [
            ("expected_run_id", json!("another-run")),
            ("message_id", json!(uuid::Uuid::new_v4().to_string())),
        ] {
            let mut changed = next.clone();
            changed[field] = value;
            assert!(prepare_request(&changed).is_err());
        }
        let mut changed = next.clone();
        changed["speech"]["profile_id"] = json!("another-profile");
        assert!(prepare_request(&changed).is_err());
        changed = next.clone();
        changed["audio"]["data_base64"] = json!(STANDARD.encode(vec![1u8; 3200]));
        assert!(prepare_request(&changed).is_err());
        changed = next.clone();
        changed["rejection"]["payload_sha256"] = json!("a".repeat(64));
        assert!(matches!(
            store
                .admit(&changed, &target, &job, &audio, &[], false)
                .unwrap(),
            Admission::Refused(_)
        ));
        let mut reused = changed.clone();
        reused["rejection"] = proof;
        assert!(store
            .admit(&reused, &target, &job, &audio, &[], false)
            .is_err());
        assert!(store.get(string(&job, "id")).unwrap().is_none());
        assert!(store.refusal(&json!({"request_id":uuid::Uuid::new_v4().to_string(),"message_id":uuid::Uuid::new_v4().to_string()}),&target).unwrap().is_none());
    }

    #[test]
    fn concurrent_admission_and_refusal_never_disagree_about_the_same_operation() {
        let (directory, store, target, request, job, audio) = fixture();
        drop(store);
        let first = Store::open_at(&directory.path().join("voice")).unwrap();
        let second = Store::open_at(&directory.path().join("voice")).unwrap();
        let barrier = std::sync::Arc::new(std::sync::Barrier::new(2));
        let tasks: Vec<_> = [first, second]
            .into_iter()
            .enumerate()
            .map(|(index, mut store)| {
                let (barrier, target, request, job, audio) = (
                    barrier.clone(),
                    target.clone(),
                    request.clone(),
                    job.clone(),
                    audio.clone(),
                );
                std::thread::spawn(move || {
                    barrier.wait();
                    matches!(
                        store
                            .admit(&request, &target, &job, &audio, &[], index == 0)
                            .unwrap(),
                        Admission::Accepted(_)
                    )
                })
            })
            .collect();
        let results: Vec<_> = tasks.into_iter().map(|task| task.join().unwrap()).collect();
        assert_eq!(results[0], results[1]);
    }

    #[test]
    fn queue_and_storage_refusals_are_atomic_but_unknown_database_errors_are_not_evidence() {
        for (count, state, code) in [
            (32, "queued", "queue_full"),
            (10_000, "submitted", "storage_full"),
        ] {
            let (_directory, mut store, target, request, job, audio) = fixture();
            store.db.execute("WITH RECURSIVE n(i) AS (VALUES(1) UNION ALL SELECT i+1 FROM n WHERE i<?1) INSERT INTO messages(id,digest,state,created,revision,conversation,body,audio) SELECT 'synthetic-'||i,'digest',?2,1,1,'other','{}',x'' FROM n",params![count,state]).unwrap();
            let proof = refusal(
                store
                    .admit(&request, &target, &job, &audio, &[], false)
                    .unwrap(),
            );
            assert_eq!(proof["code"], code);
            assert!(store.get(string(&job, "id")).unwrap().is_none());
            // Freeing the queue/storage never revokes the saved admission barrier.
            store
                .db
                .execute("DELETE FROM messages WHERE conversation='other'", [])
                .unwrap();
            assert_eq!(
                refusal(
                    store
                        .admit(&request, &target, &job, &audio, &[], false)
                        .unwrap()
                ),
                proof
            );
            assert!(matches!(
                store
                    .admit(&retry(&request, &proof), &target, &job, &audio, &[], false)
                    .unwrap(),
                Admission::Accepted(_)
            ));
        }
        let (_directory, mut store, target, request, job, audio) = fixture();
        store.db.execute_batch("CREATE TRIGGER synthetic_failure BEFORE INSERT ON messages BEGIN SELECT RAISE(FAIL,'synthetic database error'); END;").unwrap();
        assert!(store
            .admit(&request, &target, &job, &audio, &[], false)
            .is_err());
        assert_eq!(
            store
                .db
                .query_row("SELECT count(*) FROM voice_refusals", [], |r| r
                    .get::<_, i64>(0))
                .unwrap(),
            0
        );
        assert_eq!(
            store
                .db
                .query_row("SELECT count(*) FROM voice_admissions", [], |r| r
                    .get::<_, i64>(0))
                .unwrap(),
            0
        );
    }
}
