//! Host-owned profile configuration. Never return credentials or raw exceptions.
use anyhow::{bail, ensure, Context, Result};
use base64::{engine::general_purpose::STANDARD, Engine};
use serde_json::Value;

const ALGORITHM: &str = "P256-HKDF-SHA256-A256GCM";
const MODELS: &[&str] = &[
    "v3_e2e_rnnt",
    "v3_e2e_ctc",
    "v3_rnnt",
    "v3_ctc",
    "multilingual_ctc",
    "multilingual_large_ctc",
];
const DEVICES: &[&str] = &["cpu", "cuda", "cuda:0", "cuda:1", "mps"];
pub(super) fn is_action(action: &str) -> bool {
    matches!(
        action,
        "config_list" | "profile_upsert" | "profile_remove" | "profile_test" | "config_status"
    )
}
fn fields(value: &Value, required: &[&str], optional: &[&str]) -> Result<()> {
    let map = value.as_object().context("Expected object")?;
    ensure!(
        required.iter().all(|k| map.contains_key(*k))
            && map
                .keys()
                .all(|k| required.contains(&k.as_str()) || optional.contains(&k.as_str())),
        "Invalid speech management fields"
    );
    Ok(())
}
fn string(value: &Value, minimum: usize, maximum: usize) -> Result<&str> {
    let s = value.as_str().context("Expected bounded text")?;
    ensure!(
        (minimum..=maximum).contains(&s.len()) && !s.chars().any(|c| c.is_control()),
        "Invalid speech management text"
    );
    Ok(s)
}
fn identity(value: &Value) -> Result<()> {
    let s = string(value, 36, 36)?;
    let id = uuid::Uuid::parse_str(s)?;
    ensure!(
        !id.is_nil() && id.hyphenated().to_string() == s,
        "Invalid speech management UUID"
    );
    Ok(())
}
fn profile_id(value: &Value) -> Result<&str> {
    let id = string(value, 1, 64)?;
    ensure!(
        id.bytes()
            .all(|b| b.is_ascii_alphanumeric() || b == b'-' || b == b'_'),
        "Invalid speech profile ID"
    );
    Ok(id)
}
fn revision(value: &Value) -> Result<i64> {
    let n = value
        .as_i64()
        .context("Invalid speech configuration revision")?;
    ensure!(
        (0..=9_007_199_254_740_991).contains(&n),
        "Invalid speech configuration revision"
    );
    Ok(n)
}
fn decode(value: &Value, min: usize, max: usize) -> Result<Vec<u8>> {
    let s = value
        .as_str()
        .context("Invalid sealed credential encoding")?;
    ensure!(
        s.len() <= max.div_ceil(3) * 4,
        "Sealed credential too large"
    );
    let bytes = STANDARD
        .decode(s)
        .context("Invalid sealed credential encoding")?;
    ensure!(
        (min..=max).contains(&bytes.len()) && STANDARD.encode(&bytes) == s,
        "Invalid sealed credential encoding"
    );
    Ok(bytes)
}
fn validate_profile(p: &Value) -> Result<()> {
    let adapter = p["adapter"].as_str().unwrap_or("");
    let common = &["id", "name", "adapter", "model", "language"];
    match adapter {
        "gigaam" => {
            let mut required = common.to_vec();
            required.push("device");
            fields(p, &required, &[])?;
        }
        "openai_http" => {
            let mut required = common.to_vec();
            required.extend(["base_url", "allow_insecure_http"]);
            fields(p, &required, &[])?;
        }
        _ => bail!("Unsupported speech adapter"),
    }
    profile_id(&p["id"])?;
    string(&p["name"], 1, 128)?;
    string(&p["model"], 1, 128)?;
    let language = string(&p["language"], 0, 16)?;
    ensure!(
        language.is_empty()
            || (language.len() == 2 && language.bytes().all(|b| b.is_ascii_lowercase())),
        "Use an ISO 639-1 language code"
    );
    if adapter == "gigaam" {
        ensure!(
            MODELS.contains(&p["model"].as_str().unwrap_or("")),
            "Unsupported GigaAM model"
        );
        ensure!(
            DEVICES.contains(&p["device"].as_str().unwrap_or("cpu")),
            "Unsupported inference device"
        );
    } else {
        let url = string(&p["base_url"], 1, 2048)?;
        ensure!(
            !url.contains(['@', '?', '#']) && !url.contains('\\'),
            "Base URL cannot contain credentials, query or fragment"
        );
        let uri: http::Uri = url.parse().context("Invalid speech base URL")?;
        ensure!(
            uri.host().is_some_and(|h| !h.is_empty()) && uri.authority().is_some(),
            "Invalid speech base URL"
        );
        ensure!(p["allow_insecure_http"].is_boolean(), "Invalid HTTP opt-in");
        ensure!(
            uri.scheme_str() == Some("https")
                || (uri.scheme_str() == Some("http") && p["allow_insecure_http"] == true),
            "HTTP requires explicit insecure transport opt-in"
        );
    }
    Ok(())
}
pub(super) fn validate(v: &Value) -> Result<()> {
    let action = v["action"].as_str().unwrap_or("");
    match action {
        "config_list" => fields(v, &["action"], &[])?,
        "config_status" => {
            fields(v, &["action", "operation_id", "expected_machine_id"], &[])?;
            identity(&v["operation_id"])?;
            identity(&v["expected_machine_id"])?;
        }
        "profile_upsert" | "profile_remove" | "profile_test" => {
            let extra: &[&str] = match action {
                "profile_upsert" => &["expected_revision", "profile", "credential"],
                "profile_remove" => &["expected_revision", "profile_id"],
                _ => &["expected_profile_revision", "profile_id"],
            };
            let mut required = vec!["action", "request_id", "expected_machine_id"];
            required.extend(extra);
            fields(v, &required, &[])?;
            identity(&v["request_id"])?;
            identity(&v["expected_machine_id"])?;
            if action == "profile_test" {
                revision(&v["expected_profile_revision"])?;
            } else {
                revision(&v["expected_revision"])?;
            }
            if action != "profile_upsert" {
                profile_id(&v["profile_id"])?;
            } else {
                validate_profile(&v["profile"])?;
                let c = &v["credential"];
                match c["mode"].as_str().unwrap_or("") {
                    "keep" | "remove" => fields(c, &["mode"], &[])?,
                    "replace" => {
                        ensure!(
                            v["profile"]["adapter"] == "openai_http",
                            "Local profiles do not use credentials"
                        );
                        if c.get("value").is_some() {
                            fields(c, &["mode", "value"], &[])?;
                            string(&c["value"], 1, 4096)?;
                        } else {
                            fields(c, &["mode", "sealed_value"], &[])?;
                            let s = &c["sealed_value"];
                            fields(
                                s,
                                &[
                                    "key_id",
                                    "algorithm",
                                    "ephemeral_public_key",
                                    "nonce",
                                    "ciphertext",
                                ],
                                &[],
                            )?;
                            ensure!(
                                s["algorithm"] == ALGORITHM,
                                "Unsupported sealed credential algorithm"
                            );
                            let key = string(&s["key_id"], 64, 64)?;
                            ensure!(
                                key.bytes()
                                    .all(|b| b.is_ascii_digit() || (b'a'..=b'f').contains(&b)),
                                "Invalid credential key identity"
                            );
                            decode(&s["ephemeral_public_key"], 80, 128)?;
                            decode(&s["nonce"], 12, 12)?;
                            decode(&s["ciphertext"], 17, 4112)?;
                        }
                    }
                    _ => bail!("Invalid credential operation"),
                }
            }
        }
        _ => bail!("Unknown speech management action"),
    }
    ensure!(
        serde_json::to_vec(v)?.len() <= 16384,
        "Speech management request too large"
    );
    Ok(())
}

#[cfg(unix)]
mod host {
    use super::*;
    use aes_gcm::{
        aead::{Aead, KeyInit, Payload},
        Aes256Gcm, Nonce,
    };
    use p256::{
        pkcs8::{DecodePrivateKey, DecodePublicKey, EncodePrivateKey, EncodePublicKey},
        SecretKey,
    };
    use rand_core::OsRng;
    use rusqlite::{params, Connection, OpenFlags, OptionalExtension, TransactionBehavior};
    use serde_json::json;
    use sha2::{Digest, Sha256};
    use std::{
        fs::{self, OpenOptions},
        io,
        os::unix::fs::{DirBuilderExt, MetadataExt, OpenOptionsExt},
        path::Path,
        time::Duration,
    };

    fn context(v: &Value) -> Vec<u8> {
        let p = &v["profile"];
        [
            "zerus-speech-credential-v1".to_string(),
            v["expected_machine_id"].as_str().unwrap_or("").into(),
            v["request_id"].as_str().unwrap_or("").into(),
            "profile_upsert".into(),
            v["expected_revision"].to_string(),
            p["id"].as_str().unwrap_or("").into(),
            p["name"].as_str().unwrap_or("").into(),
            p["adapter"].as_str().unwrap_or("").into(),
            p["model"].as_str().unwrap_or("").into(),
            p["language"].as_str().unwrap_or("").into(),
            p["device"].as_str().unwrap_or("").into(),
            p["base_url"].as_str().unwrap_or("").into(),
            if p["allow_insecure_http"] == true {
                "true"
            } else {
                "false"
            }
            .into(),
        ]
        .join("\n")
        .into_bytes()
    }
    fn key_public(private: &[u8]) -> Result<Vec<u8>> {
        Ok(SecretKey::from_pkcs8_der(private)?
            .public_key()
            .to_public_key_der()?
            .as_bytes()
            .to_vec())
    }
    fn key_id(public: &[u8]) -> String {
        format!("{:x}", Sha256::digest(public))
    }
    fn unseal(private: &[u8], v: &Value) -> Result<String> {
        let sealed = &v["credential"]["sealed_value"];
        let key = SecretKey::from_pkcs8_der(private)?;
        let public = key_public(private)?;
        ensure!(
            sealed["key_id"] == key_id(&public),
            "Invalid sealed credential"
        );
        let encoded = decode(&sealed["ephemeral_public_key"], 80, 128)?;
        let peer = p256::PublicKey::from_public_key_der(&encoded)?;
        ensure!(
            peer.to_public_key_der()?.as_bytes() == encoded,
            "Invalid sealed credential"
        );
        let shared = p256::ecdh::diffie_hellman(key.to_nonzero_scalar(), peer.as_affine());
        let ctx = context(v);
        let salt = Sha256::digest(&public);
        let mut aes = [0u8; 32];
        hkdf::Hkdf::<Sha256>::new(Some(&salt), shared.raw_secret_bytes())
            .expand(&ctx, &mut aes)
            .map_err(|_| anyhow::anyhow!("Invalid sealed credential"))?;
        let nonce = decode(&sealed["nonce"], 12, 12)?;
        let ciphertext = decode(&sealed["ciphertext"], 17, 4112)?;
        let plaintext = Aes256Gcm::new_from_slice(&aes)
            .map_err(|_| anyhow::anyhow!("Invalid sealed credential"))?
            .decrypt(
                Nonce::from_slice(&nonce),
                Payload {
                    msg: &ciphertext,
                    aad: &ctx,
                },
            )
            .map_err(|_| anyhow::anyhow!("Invalid sealed credential"))?;
        let result = String::from_utf8(plaintext)?;
        string(&Value::String(result.clone()), 1, 4096)?;
        Ok(result)
    }
    struct Store {
        db: Connection,
    }
    impl Store {
        fn open(dir: &Path) -> Result<Self> {
            let private = dir.join("speech-profiles");
            fs::DirBuilder::new()
                .recursive(true)
                .mode(0o700)
                .create(&private)?;
            let m = fs::symlink_metadata(&private)?;
            ensure!(
                m.is_dir() && m.uid() == unsafe { libc::geteuid() } && m.mode() & 0o077 == 0,
                "Speech configuration directory must be private"
            );
            let path = private.canonicalize()?.join("profiles.sqlite3");
            // Never close an unrelated raw descriptor of a live SQLite inode.
            if !path.try_exists()? {
                match OpenOptions::new()
                    .create_new(true)
                    .write(true)
                    .mode(0o600)
                    .custom_flags(libc::O_NOFOLLOW)
                    .open(&path)
                {
                    Ok(f) => drop(f),
                    Err(e) if e.kind() == io::ErrorKind::AlreadyExists => (),
                    Err(e) => return Err(e.into()),
                }
            }
            let m = fs::symlink_metadata(&path)?;
            ensure!(
                m.is_file()
                    && m.uid() == unsafe { libc::geteuid() }
                    && m.mode() & 0o077 == 0
                    && m.nlink() == 1,
                "Speech configuration store must be private"
            );
            let mut db = Connection::open_with_flags(
                path,
                OpenFlags::SQLITE_OPEN_READ_WRITE
                    | OpenFlags::SQLITE_OPEN_NO_MUTEX
                    | OpenFlags::SQLITE_OPEN_NOFOLLOW,
            )?;
            db.busy_timeout(Duration::from_secs(5))?;
            db.execute_batch("PRAGMA journal_mode=WAL;PRAGMA synchronous=FULL;PRAGMA secure_delete=ON;
                CREATE TABLE IF NOT EXISTS metadata(key TEXT PRIMARY KEY,value TEXT NOT NULL);
                CREATE TABLE IF NOT EXISTS profiles(id TEXT PRIMARY KEY,revision INTEGER NOT NULL,profile TEXT NOT NULL,secret TEXT,deleted INTEGER NOT NULL DEFAULT 0);
                CREATE TABLE IF NOT EXISTS operations(id TEXT PRIMARY KEY,fingerprint TEXT NOT NULL,result TEXT NOT NULL);")?;
            let tx = db.transaction_with_behavior(TransactionBehavior::Immediate)?;
            if tx
                .query_row("SELECT value FROM metadata WHERE key='revision'", [], |r| {
                    r.get::<_, String>(0)
                })
                .optional()?
                .is_none()
            {
                let legacy = match fs::read(dir.join("speech.json")) {
                    Ok(bytes) => {
                        ensure!(
                            bytes.len() <= 65536,
                            "Legacy speech configuration too large"
                        );
                        serde_json::from_slice::<Value>(&bytes)?
                    }
                    Err(e) if e.kind() == io::ErrorKind::NotFound => {
                        json!({"schema":1,"profiles":[]})
                    }
                    Err(e) => return Err(e.into()),
                };
                ensure!(
                    legacy["schema"] == 1,
                    "Unsupported legacy speech configuration"
                );
                let rows = legacy["profiles"]
                    .as_array()
                    .context("Invalid legacy speech profiles")?;
                ensure!(rows.len() <= 32, "Too many legacy profiles");
                for p in rows {
                    profile_id(&p["id"])?;
                    tx.execute(
                        "INSERT INTO profiles VALUES(?,1,?,NULL,0)",
                        params![p["id"].as_str(), p.to_string()],
                    )?;
                }
                let key = SecretKey::random(&mut OsRng).to_pkcs8_der()?;
                tx.execute("INSERT INTO metadata VALUES('revision','1')", [])?;
                tx.execute(
                    "INSERT INTO metadata VALUES('credential_key',?)",
                    params![STANDARD.encode(key.as_bytes())],
                )?;
            }
            tx.commit()?;
            Ok(Self { db })
        }
    }
    fn response(machine: &str, id: &str, state: &str) -> Value {
        json!({"schema":1,"machine_id":machine,"request_id":id,"state":state})
    }
    fn failure(machine: &str, id: &str, code: &str) -> Value {
        let message = match code {
            "revision_conflict" => "Speech configuration changed. Refresh and review your edits.",
            "request_conflict" => "This request identity was already used for different input.",
            "profile_not_found" => "The speech profile is no longer available.",
            "profile_id_retired" => "This profile ID was removed. Choose a new ID.",
            "invalid_credential" => {
                "The credential could not be verified. Refresh the host key and enter it again."
            }
            "operation_not_found" => "The operation was not found. No mutation has been repeated.",
            "service_unavailable" => {
                "Configuration is saved; start the speech service on this computer to test it."
            }
            "capacity" => "Speech configuration storage is full.",
            _ => "Speech profile operation could not be completed.",
        };
        let mut r = response(machine, id, "failed");
        r["error_code"] = json!(code);
        r["error"] = json!(message);
        r
    }
    fn service(config: &crate::config::Config, value: &Value) -> Result<Value> {
        let path = super::super::socket_path(config)
            .map_err(|_| anyhow::anyhow!("Speech service unavailable"))?;
        super::super::exchange(&path, &serde_json::to_vec(value)?)
            .map_err(|_| anyhow::anyhow!("Speech service unavailable"))
    }
    fn current_revision(db: &Connection) -> Result<i64> {
        Ok(db
            .query_row("SELECT value FROM metadata WHERE key='revision'", [], |r| {
                r.get::<_, String>(0)
            })?
            .parse()?)
    }
    pub(super) fn request(config: &crate::config::Config, input: &Value) -> Result<Value> {
        let machine = crate::swarm::local_node_id(config)
            .map_err(|_| anyhow::anyhow!("Machine identity unavailable"))?;
        if input["action"] != "config_list" {
            ensure!(
                input["expected_machine_id"] == machine,
                "Speech computer identity changed"
            );
        }
        let mut store = Store::open(&config.dir)?;
        let action = input["action"].as_str().unwrap_or("");
        if action == "config_list" {
            // Profile fields and CAS revision must come from one snapshot.
            let snapshot = store.db.transaction()?;
            let private: String = snapshot.query_row(
                "SELECT value FROM metadata WHERE key='credential_key'",
                [],
                |r| r.get(0),
            )?;
            let public = key_public(&STANDARD.decode(private)?)?;
            let live = service(config, &json!({"action":"providers"})).ok();
            let mut profiles = Vec::new();
            let mut query=snapshot.prepare("SELECT revision,profile,secret IS NOT NULL FROM profiles WHERE deleted=0 ORDER BY id")?;
            for row in query.query_map([], |r| {
                Ok((
                    r.get::<_, i64>(0)?,
                    r.get::<_, String>(1)?,
                    r.get::<_, bool>(2)?,
                ))
            })? {
                let (revision, encoded, configured) = row?;
                let p: Value = serde_json::from_str(&encoded)?;
                let mut output = json!({"id":p["id"],"name":p["name"],"adapter":p["adapter"],"model":p["model"],"language":p["language"],"revision":revision,"credential":{"configured":configured},"available":false,"reason":"Speech service is unavailable","max_duration_ms":120000});
                for key in ["device", "base_url", "allow_insecure_http"] {
                    if let Some(v) = p.get(key) {
                        output[key] = v.clone();
                    }
                }
                if let Some(ready) = live
                    .as_ref()
                    .and_then(|v| v["profiles"].as_array())
                    .and_then(|rows| {
                        rows.iter()
                            .find(|v| v["id"] == p["id"] && v["profile_revision"] == revision)
                    })
                {
                    output["available"] = ready["available"].clone();
                    output.as_object_mut().unwrap().remove("reason");
                    if let Some(reason) = ready.get("reason") {
                        output["reason"] = reason.clone();
                    }
                }
                profiles.push(output);
            }
            return Ok(
                json!({"schema":1,"machine_id":machine,"revision":current_revision(&snapshot)?,"profiles":profiles,
                "service":{"available":live.is_some(),"reason":if live.is_some(){""}else{"Speech service is unavailable; configuration remains editable"}},
                "adapters":[{"id":"gigaam","models":MODELS,"devices":DEVICES},{"id":"openai_http"}],
                "credential_key":{"key_id":key_id(&public),"algorithm":ALGORITHM,"public_key_spki":STANDARD.encode(public)}}),
            );
        }
        let id = input[if action == "config_status" {
            "operation_id"
        } else {
            "request_id"
        }]
        .as_str()
        .unwrap_or("");
        if action == "config_status" {
            let previous: Option<String> = store
                .db
                .query_row("SELECT result FROM operations WHERE id=?", [id], |r| {
                    r.get(0)
                })
                .optional()?;
            let Some(encoded) = previous else {
                return Ok(failure(&machine, id, "operation_not_found"));
            };
            let mut result: Value = serde_json::from_str(&encoded)?;
            if result["state"] == "running" || result["state"] == "uncertain" {
                if let Ok(job) = service(config, &json!({"action":"status","job_id":id})) {
                    match job["state"].as_str().unwrap_or("") {
                        "queued" | "running" => {
                            result["state"] = json!("running");
                            result.as_object_mut().unwrap().remove("error_code");
                            result.as_object_mut().unwrap().remove("error");
                        }
                        "completed" => {
                            result["state"] = json!("completed");
                            result.as_object_mut().unwrap().remove("error_code");
                            result.as_object_mut().unwrap().remove("error");
                        }
                        "failed" | "uncertain" | "cancelled" => {
                            result["state"] = json!(if job["error_code"] == "not_found" {
                                "uncertain"
                            } else {
                                "failed"
                            });
                            result["error_code"] = job
                                .get("error_code")
                                .cloned()
                                .unwrap_or(json!("test_failed"));
                            result["error"] = json!(
                                "Speech recognition test did not complete. No test was repeated."
                            );
                        }
                        _ => (),
                    }
                    let updated = result.to_string();
                    if updated != encoded {
                        store.db.execute(
                            "UPDATE operations SET result=? WHERE id=? AND result=?",
                            params![updated, id, encoded],
                        )?;
                    }
                }
            }
            return Ok(result);
        }
        let fingerprint = format!("{:x}", Sha256::digest(serde_json::to_vec(input)?));
        let tx = store
            .db
            .transaction_with_behavior(TransactionBehavior::Immediate)?;
        if let Some((hash, result)) = tx
            .query_row(
                "SELECT fingerprint,result FROM operations WHERE id=?",
                [id],
                |r| Ok((r.get::<_, String>(0)?, r.get::<_, String>(1)?)),
            )
            .optional()?
        {
            return Ok(if hash == fingerprint {
                serde_json::from_str(&result)?
            } else {
                failure(&machine, id, "request_conflict")
            });
        }
        if tx.query_row("SELECT count(*) FROM operations", [], |r| {
            r.get::<_, i64>(0)
        })? >= 100000
        {
            return Ok(failure(&machine, id, "capacity"));
        }
        let rev = current_revision(&tx)?;
        if rev >= 9_007_199_254_740_991 {
            return Ok(failure(&machine, id, "capacity"));
        }
        let profile = input["profile"].as_object().cloned().map(Value::Object);
        let pid = profile
            .as_ref()
            .map(|p| p["id"].as_str().unwrap_or(""))
            .unwrap_or_else(|| input["profile_id"].as_str().unwrap_or(""))
            .to_string();
        let existing = tx
            .query_row(
                "SELECT revision,profile,secret,deleted FROM profiles WHERE id=?",
                [&pid],
                |r| {
                    Ok((
                        r.get::<_, i64>(0)?,
                        r.get::<_, String>(1)?,
                        r.get::<_, Option<String>>(2)?,
                        r.get::<_, bool>(3)?,
                    ))
                },
            )
            .optional()?;
        let mut result = response(&machine, id, "completed");
        result["profile_id"] = json!(pid);
        result["revision"] = json!(rev);
        if action != "profile_test" && input["expected_revision"].as_i64() != Some(rev) {
            result = failure(&machine, id, "revision_conflict");
            result["revision"] = json!(rev);
        } else if action == "profile_upsert" {
            let mut p = profile.unwrap();
            if existing.as_ref().is_some_and(|r| r.3) {
                result = failure(&machine, id, "profile_id_retired");
            } else if existing.is_none()
                && (tx.query_row("SELECT count(*) FROM profiles WHERE deleted=0", [], |r| {
                    r.get::<_, i64>(0)
                })? >= 32
                    || tx.query_row("SELECT count(*) FROM profiles", [], |r| r.get::<_, i64>(0))?
                        >= 1024)
            {
                result = failure(&machine, id, "capacity");
            } else {
                let old = existing.as_ref().and_then(|r| r.2.clone());
                let secret = match input["credential"]["mode"].as_str().unwrap_or("") {
                    "keep" => Ok(old),
                    "remove" => Ok(None),
                    _ => {
                        if let Some(s) = input["credential"]["value"].as_str() {
                            Ok(Some(s.to_string()))
                        } else {
                            let key: String = tx.query_row(
                                "SELECT value FROM metadata WHERE key='credential_key'",
                                [],
                                |r| r.get(0),
                            )?;
                            unseal(&STANDARD.decode(key)?, input).map(Some)
                        }
                    }
                };
                match secret {
                    Err(_) => result = failure(&machine, id, "invalid_credential"),
                    Ok(mut secret) => {
                        if p["adapter"] == "gigaam" {
                            secret = None;
                            if p.get("device").is_none() {
                                p["device"] = json!("cpu");
                            }
                        }
                        // Imported cache paths remain private and cannot be changed by clients.
                        if let Some(old) = existing
                            .as_ref()
                            .and_then(|r| serde_json::from_str::<Value>(&r.1).ok())
                        {
                            if let Some(root) = old.get("download_root") {
                                if p["adapter"] == "gigaam" {
                                    p["download_root"] = root.clone();
                                }
                            }
                        }
                        tx.execute("INSERT INTO profiles VALUES(?,?,?,?,0) ON CONFLICT(id) DO UPDATE SET revision=excluded.revision,profile=excluded.profile,secret=excluded.secret",params![pid,rev+1,p.to_string(),secret])?;
                        tx.execute(
                            "UPDATE metadata SET value=? WHERE key='revision'",
                            params![(rev + 1).to_string()],
                        )?;
                        result["revision"] = json!(rev + 1);
                        result["profile_revision"] = json!(rev + 1);
                    }
                }
            }
        } else if existing.as_ref().is_none_or(|r| r.3) {
            result = failure(&machine, id, "profile_not_found");
        } else if action == "profile_remove" {
            tx.execute(
                "UPDATE profiles SET deleted=1,secret=NULL,revision=? WHERE id=?",
                params![rev + 1, pid],
            )?;
            tx.execute(
                "UPDATE metadata SET value=? WHERE key='revision'",
                params![(rev + 1).to_string()],
            )?;
            result["revision"] = json!(rev + 1);
        } else if existing.as_ref().unwrap().0
            != input["expected_profile_revision"].as_i64().unwrap_or(-1)
        {
            result = failure(&machine, id, "revision_conflict");
        } else {
            result["state"] = json!("running");
            result["profile_revision"] = json!(existing.as_ref().unwrap().0);
        }
        tx.execute(
            "INSERT INTO operations VALUES(?,?,?)",
            params![id, fingerprint, result.to_string()],
        )?;
        tx.commit()?;
        if action == "profile_test" && result["state"] == "running" {
            if service(config, &json!({"action":"providers"})).is_err() {
                result = failure(&machine, id, "service_unavailable");
                store.db.execute(
                    "UPDATE operations SET result=? WHERE id=?",
                    params![result.to_string(), id],
                )?;
                return Ok(result);
            }
            let probe = service(
                config,
                &json!({"action":"probe","request_id":id,"profile_id":pid,"profile_revision":input["expected_profile_revision"]}),
            );
            match probe {
                Ok(v)
                    if ["queued", "running", "completed"]
                        .contains(&v["state"].as_str().unwrap_or("")) => {}
                Ok(_) => {
                    result = failure(&machine, id, "test_failed");
                    store.db.execute(
                        "UPDATE operations SET result=? WHERE id=?",
                        params![result.to_string(), id],
                    )?;
                }
                Err(_) => {
                    result["state"] = json!("uncertain");
                    result["error_code"] = json!("service_unavailable");
                    result["error"]=json!("The test is not confirmed. Check its status after reconnecting; it will not be repeated automatically.");
                    store.db.execute(
                        "UPDATE operations SET result=? WHERE id=?",
                        params![result.to_string(), id],
                    )?;
                }
            }
        }
        Ok(result)
    }
    #[cfg(test)]
    mod tests {
        use super::*;
        use std::os::unix::fs::PermissionsExt;
        #[test]
        fn store_migrates_once_preserves_private_cache_and_rejects_symlink() {
            let tmp = tempfile::tempdir().unwrap();
            fs::set_permissions(tmp.path(), fs::Permissions::from_mode(0o700)).unwrap();
            fs::write(tmp.path().join("speech.json"),r#"{"schema":1,"profiles":[{"id":"old","name":"Old","adapter":"gigaam","model":"v3_e2e_rnnt","language":"ru","download_root":"/private/example-models"}]}"#).unwrap();
            let first = Store::open(tmp.path()).unwrap();
            let second = Store::open(tmp.path()).unwrap();
            assert_eq!(current_revision(&first.db).unwrap(), 1);
            assert_eq!(
                second
                    .db
                    .query_row("SELECT count(*) FROM profiles", [], |r| r.get::<_, i64>(0))
                    .unwrap(),
                1
            );
            let payload: String = second
                .db
                .query_row("SELECT profile FROM profiles", [], |r| r.get(0))
                .unwrap();
            assert!(payload.contains("example-models"));
            drop(first);
            drop(second);
            let private = tmp.path().join("speech-profiles");
            fs::rename(private.join("profiles.sqlite3"), private.join("other")).unwrap();
            std::os::unix::fs::symlink("other", private.join("profiles.sqlite3")).unwrap();
            assert!(Store::open(tmp.path()).is_err());
        }
        #[test]
        fn sealed_credential_authenticates_profile_and_request() {
            let host = SecretKey::random(&mut OsRng);
            let host_der = host.to_pkcs8_der().unwrap();
            let public = key_public(host_der.as_bytes()).unwrap();
            let ephemeral = SecretKey::random(&mut OsRng);
            let peer = ephemeral.public_key().to_public_key_der().unwrap();
            let mut v = json!({"action":"profile_upsert","request_id":"11111111-1111-4111-8111-111111111111","expected_machine_id":"22222222-2222-4222-8222-222222222222","expected_revision":1,"profile":{"id":"remote","name":"Remote","adapter":"openai_http","model":"gigaam","language":"ru","base_url":"https://asr.example/v1","allow_insecure_http":false}});
            let shared = p256::ecdh::diffie_hellman(
                ephemeral.to_nonzero_scalar(),
                host.public_key().as_affine(),
            );
            let ctx = context(&v);
            let mut key = [0u8; 32];
            hkdf::Hkdf::<Sha256>::new(Some(&Sha256::digest(&public)), shared.raw_secret_bytes())
                .expand(&ctx, &mut key)
                .unwrap();
            let cipher = Aes256Gcm::new_from_slice(&key)
                .unwrap()
                .encrypt(
                    Nonce::from_slice(&[7u8; 12]),
                    Payload {
                        msg: b"synthetic-secret",
                        aad: &ctx,
                    },
                )
                .unwrap();
            v["credential"] = json!({"mode":"replace","sealed_value":{"key_id":key_id(&public),"algorithm":ALGORITHM,"ephemeral_public_key":STANDARD.encode(peer.as_bytes()),"nonce":STANDARD.encode([7u8;12]),"ciphertext":STANDARD.encode(cipher)}});
            validate(&v).unwrap();
            assert_eq!(unseal(host_der.as_bytes(), &v).unwrap(), "synthetic-secret");
            for (field, replacement) in [
                ("request_id", json!("33333333-3333-4333-8333-333333333333")),
                (
                    "expected_machine_id",
                    json!("44444444-4444-4444-8444-444444444444"),
                ),
                ("expected_revision", json!(2)),
            ] {
                let mut changed = v.clone();
                changed[field] = replacement;
                assert!(unseal(host_der.as_bytes(), &changed).is_err());
            }
            let mut changed = v.clone();
            changed["profile"]["base_url"] = json!("https://other.example/v1");
            assert!(unseal(host_der.as_bytes(), &changed).is_err());
            let other_host = SecretKey::random(&mut OsRng).to_pkcs8_der().unwrap();
            assert!(unseal(other_host.as_bytes(), &v).is_err());
            let mut changed = v.clone();
            changed["credential"]["sealed_value"]["nonce"] = json!(STANDARD.encode([8u8; 12]));
            assert!(unseal(host_der.as_bytes(), &changed).is_err());
        }
        #[test]
        fn independent_credential_known_answer() {
            let v: Value =
                serde_json::from_str(include_str!("../tests/fixtures/speech-credential-v1.json"))
                    .unwrap();
            let private = STANDARD
                .decode(v["host_private_pkcs8"].as_str().unwrap())
                .unwrap();
            assert_eq!(
                STANDARD.encode(key_public(&private).unwrap()),
                v["host_public_spki"]
            );
            assert_eq!(STANDARD.encode(context(&v["request"])), v["context_base64"]);
            validate(&v["request"]).unwrap();
            assert_eq!(unseal(&private, &v["request"]).unwrap(), v["plaintext"]);
        }
    }
}

pub(super) fn request(config: &crate::config::Config, input: &Value) -> Result<Value> {
    #[cfg(unix)]
    {
        host::request(config, input)
    }
    #[cfg(not(unix))]
    {
        let _ = (config, input);
        bail!("Choose a configured remote speech computer")
    }
}
