//! Conversation-owned voice messages. Acceptance commits audio and intent;
//! a separately supervised worker recognizes and delivers without a client.
use super::*;
use base64::{engine::general_purpose::STANDARD, Engine};
use fs2::FileExt;
use rusqlite::{params, Connection, OpenFlags, OptionalExtension};
use sha2::{Digest, Sha256};
use std::{
    fs::{self, File, OpenOptions},
    os::unix::fs::{MetadataExt, OpenOptionsExt, PermissionsExt},
    process::{Command, Stdio},
    time::Duration,
};

const MAX_JOBS: i64 = 10_000;
const MAX_AUDIO: i64 = 2 * 1024 * 1024 * 1024;
const TERMINAL: &[&str] = &["submitted", "failed", "cancelled", "uncertain"];
const MAX_AGE: f64 = 24. * 60. * 60.;

fn err(e: impl std::fmt::Display) -> String {
    e.to_string()
}
fn uuid(value: &Value) -> bool {
    value.as_str().is_some_and(|s| {
        uuid::Uuid::parse_str(s).is_ok_and(|id| !id.is_nil() && id.to_string() == s)
    })
}
fn bounded(value: &Value, max: usize, empty: bool) -> bool {
    value.as_str().is_some_and(|s| {
        (empty || !s.is_empty()) && s.len() <= max && !s.chars().any(char::is_control)
    })
}
pub(crate) fn validate_request(value: &Value) -> Result<()> {
    let object = value.as_object().ok_or("Invalid voice request")?;
    let mut fields = vec![
        "action",
        "request_id",
        "expected_run_id",
        "expected_conversation_id",
    ];
    match string(value, "action") {
        "submit" => fields.extend(["speech", "audio"]),
        "status" | "cancel" => fields.push("message_id"),
        "profiles" => (),
        _ => return Err("Unknown voice action".into()),
    }
    if object.len() != fields.len()
        || fields.iter().any(|key| !object.contains_key(*key))
        || !uuid(&value["request_id"])
        || !bounded(&value["expected_run_id"], 128, false)
        || !bounded(
            &value["expected_conversation_id"],
            256,
            string(value, "action") == "profiles",
        )
    {
        return Err("Invalid voice request fields or exact conversation identity".into());
    }
    if matches!(string(value, "action"), "status" | "cancel") && !uuid(&value["message_id"]) {
        return Err("Invalid voice message UUID".into());
    }
    if string(value, "action") == "submit" {
        if value["speech"].as_object().is_none_or(|o| o.len() != 2)
            || !uuid(&value["speech"]["machine_id"])
        {
            return Err("Choose a speech computer and profile".into());
        }
        crate::speech::validate_request(&json!({"action":"submit","request_id":value["request_id"],"profile_id":value["speech"]["profile_id"],"audio":value["audio"]})).map_err(err)?;
    }
    Ok(())
}

struct Store {
    db: Connection,
}
impl Store {
    fn open_at(directory: &Path) -> Result<Self> {
        private_dir(directory)?;
        let info = fs::symlink_metadata(directory).map_err(err)?;
        if !info.is_dir()
            || info.uid() != unsafe { libc::geteuid() }
            || info.permissions().mode() & 0o077 != 0
        {
            return Err("Voice storage must be private to this user".into());
        }
        // SQLite NOFOLLOW rejects symlinks in every path component, including
        // macOS's ordinary /var -> /private/var parent. Resolve the verified
        // directory only; keep refusing a symlink at the database itself.
        let path = directory
            .canonicalize()
            .map_err(err)?
            .join("messages.sqlite3");
        if !path.try_exists().map_err(err)? {
            match OpenOptions::new()
                .create_new(true)
                .write(true)
                .mode(0o600)
                .custom_flags(libc::O_NOFOLLOW)
                .open(&path)
            {
                Ok(file) => drop(file),
                Err(error) if error.kind() == io::ErrorKind::AlreadyExists => (),
                Err(error) => return Err(err(error)),
            }
        }
        let info = fs::symlink_metadata(&path).map_err(err)?;
        if !info.is_file()
            || info.uid() != unsafe { libc::geteuid() }
            || info.permissions().mode() & 0o077 != 0
            || info.nlink() != 1
        {
            return Err("Voice database must be a private regular file".into());
        }
        // Never open/close a raw descriptor for an existing SQLite inode: that
        // would release POSIX locks owned by another connection in this process.
        let db = Connection::open_with_flags(
            path,
            OpenFlags::SQLITE_OPEN_READ_WRITE
                | OpenFlags::SQLITE_OPEN_NO_MUTEX
                | OpenFlags::SQLITE_OPEN_NOFOLLOW,
        )
        .map_err(err)?;
        db.busy_timeout(Duration::from_secs(3)).map_err(err)?;
        db.execute_batch("PRAGMA journal_mode=WAL; PRAGMA synchronous=FULL; PRAGMA secure_delete=ON;
            CREATE TABLE IF NOT EXISTS messages(id TEXT PRIMARY KEY,digest TEXT NOT NULL,state TEXT NOT NULL,created REAL NOT NULL,revision INTEGER NOT NULL,conversation TEXT NOT NULL,body TEXT NOT NULL,audio BLOB NOT NULL);
            CREATE INDEX IF NOT EXISTS voice_pending ON messages(state,created);
            CREATE INDEX IF NOT EXISTS voice_conversation ON messages(conversation,created);").map_err(err)?;
        let has_sequence = db
            .prepare("PRAGMA table_info(messages)")
            .map_err(err)?
            .query_map([], |r| r.get::<_, String>(1))
            .map_err(err)?
            .filter_map(|r| r.ok())
            .any(|name| name == "change_seq");
        if !has_sequence {
            db.execute_batch(
                "ALTER TABLE messages ADD COLUMN change_seq INTEGER NOT NULL DEFAULT 0;
                UPDATE messages SET change_seq=rowid;",
            )
            .map_err(err)?;
        }
        db.execute_batch("CREATE TABLE IF NOT EXISTS voice_clock(singleton INTEGER PRIMARY KEY CHECK(singleton=1),value INTEGER NOT NULL);
            INSERT OR IGNORE INTO voice_clock SELECT 1,COALESCE(max(change_seq),0) FROM messages;
            CREATE INDEX IF NOT EXISTS voice_changes ON messages(conversation,change_seq);").map_err(err)?;
        Ok(Self { db })
    }
    fn open() -> Result<Self> {
        Self::open_at(&root().join("voice"))
    }
    fn get(&self, id: &str) -> Result<Option<Value>> {
        let body: Option<String> = self
            .db
            .query_row("SELECT body FROM messages WHERE id=?", [id], |r| r.get(0))
            .optional()
            .map_err(err)?;
        body.map(|s| serde_json::from_str(&s).map_err(err))
            .transpose()
    }
    fn save(&self, job: &mut Value) -> Result<bool> {
        let previous = job["revision"].as_i64().ok_or("Invalid voice revision")?;
        job["revision"] = json!(previous + 1);
        let tx = self.db.unchecked_transaction().map_err(err)?;
        let sequence: i64 = tx
            .query_row(
                "UPDATE voice_clock SET value=value+1 WHERE singleton=1 RETURNING value",
                [],
                |r| r.get(0),
            )
            .map_err(err)?;
        let n = tx
            .execute(
                "UPDATE messages SET body=?,state=?,revision=?,change_seq=? WHERE id=? AND revision=?",
                params![
                    job.to_string(),
                    string(job, "state"),
                    previous + 1,
                    sequence,
                    string(job, "id"),
                    previous
                ],
            )
            .map_err(err)?;
        tx.commit().map_err(err)?;
        Ok(n == 1)
    }
    fn insert(&mut self, job: &Value, digest: &str, audio: &[u8]) -> Result<Value> {
        let tx = self
            .db
            .transaction_with_behavior(rusqlite::TransactionBehavior::Immediate)
            .map_err(err)?;
        let previous: Option<(String, String)> = tx
            .query_row(
                "SELECT digest,body FROM messages WHERE id=?",
                [string(job, "id")],
                |r| Ok((r.get(0)?, r.get(1)?)),
            )
            .optional()
            .map_err(err)?;
        if let Some((old, body)) = previous {
            if old != digest {
                return Err("Voice request ID was already used for different content".into());
            }
            return serde_json::from_str(&body).map_err(err);
        }
        let (count, size): (i64, i64) = tx
            .query_row(
                "SELECT count(*),COALESCE(sum(length(audio)),0) FROM messages",
                [],
                |r| Ok((r.get(0)?, r.get(1)?)),
            )
            .map_err(err)?;
        if count >= MAX_JOBS || size + audio.len() as i64 > MAX_AUDIO {
            return Err("Voice storage is full on this conversation computer".into());
        }
        let pending:i64=tx.query_row("SELECT count(*) FROM messages WHERE state NOT IN ('submitted','failed','cancelled','uncertain')",[],|r|r.get(0)).map_err(err)?;
        if pending >= 32 {
            return Err("Voice processing queue is full on this conversation computer".into());
        }
        let sequence: i64 = tx
            .query_row(
                "UPDATE voice_clock SET value=value+1 WHERE singleton=1 RETURNING value",
                [],
                |r| r.get(0),
            )
            .map_err(err)?;
        tx.execute(
            "INSERT INTO messages(id,digest,state,created,revision,conversation,body,audio,change_seq) VALUES(?,?,?,?,?,?,?,?,?)",
            params![
                string(job, "id"),
                digest,
                string(job, "state"),
                job["created_at"].as_f64(),
                1,
                string(job, "conversation_id"),
                job.to_string(),
                audio,
                sequence
            ],
        )
        .map_err(err)?;
        tx.commit().map_err(err)?;
        Ok(job.clone())
    }
    fn audio(&self, id: &str) -> Result<Vec<u8>> {
        self.db
            .query_row("SELECT audio FROM messages WHERE id=?", [id], |r| r.get(0))
            .map_err(err)
    }
    fn jobs(&self, conversation: Option<&str>, pending: bool) -> Result<Vec<Value>> {
        let sql = if pending {
            "SELECT body FROM messages WHERE state NOT IN ('submitted','failed','cancelled','uncertain') OR json_extract(body,'$.asr_cancel_pending')=1 ORDER BY created,id LIMIT 64"
        } else {
            "SELECT body FROM messages WHERE conversation=? ORDER BY created DESC,id DESC LIMIT 100"
        };
        let mut query = self.db.prepare(sql).map_err(err)?;
        let collect = |rows: rusqlite::MappedRows<
            '_,
            fn(&rusqlite::Row<'_>) -> rusqlite::Result<String>,
        >|
         -> Result<Vec<Value>> {
            rows.map(|r| serde_json::from_str(&r.map_err(err)?).map_err(err))
                .collect()
        };
        fn body(row: &rusqlite::Row<'_>) -> rusqlite::Result<String> {
            row.get(0)
        }
        if pending {
            collect(
                query
                    .query_map(
                        [],
                        body as fn(&rusqlite::Row<'_>) -> rusqlite::Result<String>,
                    )
                    .map_err(err)?,
            )
        } else {
            collect(
                query
                    .query_map(
                        [conversation.unwrap_or("")],
                        body as fn(&rusqlite::Row<'_>) -> rusqlite::Result<String>,
                    )
                    .map_err(err)?,
            )
        }
    }
    fn changes(&self, conversation: &str, after: i64) -> Result<(Vec<Value>, i64, bool)> {
        let mut query=self.db.prepare("SELECT body,change_seq FROM messages WHERE conversation=? AND change_seq>? ORDER BY change_seq LIMIT 100").map_err(err)?;
        let rows = query
            .query_map(params![conversation, after], |r| {
                Ok((r.get::<_, String>(0)?, r.get::<_, i64>(1)?))
            })
            .map_err(err)?;
        let mut next = after;
        let mut jobs = Vec::new();
        let mut bytes = 0;
        for row in rows {
            let (body, sequence) = row.map_err(err)?;
            if bytes + body.len() > 512 * 1024 && !jobs.is_empty() {
                break;
            }
            bytes += body.len();
            next = sequence;
            jobs.push(serde_json::from_str(&body).map_err(err)?);
        }
        let more: bool = self
            .db
            .query_row(
                "SELECT EXISTS(SELECT 1 FROM messages WHERE conversation=? AND change_seq>?)",
                params![conversation, next],
                |r| r.get(0),
            )
            .map_err(err)?;
        Ok((jobs, next, !more))
    }
}

fn wav(pcm: &[u8]) -> Vec<u8> {
    let mut bytes = Vec::with_capacity(pcm.len() + 44);
    bytes.extend(b"RIFF");
    bytes.extend((pcm.len() as u32 + 36).to_le_bytes());
    bytes.extend(b"WAVEfmt ");
    bytes.extend(16u32.to_le_bytes());
    bytes.extend(1u16.to_le_bytes());
    bytes.extend(1u16.to_le_bytes());
    bytes.extend(16000u32.to_le_bytes());
    bytes.extend(32000u32.to_le_bytes());
    bytes.extend(2u16.to_le_bytes());
    bytes.extend(16u16.to_le_bytes());
    bytes.extend(b"data");
    bytes.extend((pcm.len() as u32).to_le_bytes());
    bytes.extend(pcm);
    bytes
}
fn public(job: &Value) -> Value {
    let mut value = json!({});
    for key in [
        "id",
        "run_id",
        "conversation_id",
        "created_at",
        "revision",
        "state",
        "audio",
        "transcript",
        "transcript_truncated",
        "error_code",
        "error",
        "native_binding",
    ] {
        if let Some(v) = job.get(key) {
            value[key] = v.clone();
        }
    }
    value
}
fn record(name: &str) -> Result<Value> {
    if dsh::exists(name) {
        dsh::binding(name)
    } else {
        read(name)
    }
}
fn checked(name: &str, request: &Value) -> Result<Value> {
    let value = record(name)?;
    if value["run_id"] != request["expected_run_id"]
        || value["conversation_id"] != request["expected_conversation_id"]
        || value["state"] == "archived"
    {
        return Err("Voice target changed; refresh the exact conversation".into());
    }
    Ok(value)
}
fn belongs(job: &Value, record: &Value) -> bool {
    !string(record, "conversation_id").is_empty()
        && job["conversation_id"] == record["conversation_id"]
        && job["agent"] == record["agent"]
        && job["agent_home"] == record["agent_home"]
        && job["account_id"] == record["account_id"]
}
fn failure(job: &mut Value, code: &str, message: &str) {
    job["state"] = json!("failed");
    job["error_code"] = json!(code);
    job["error"] = json!(message);
}
fn cancel_job(job: &mut Value) -> bool {
    if TERMINAL.contains(&string(job, "state")) || job["state"] == "delivering" {
        return false;
    }
    job["state"] = json!("cancelled");
    job["asr_cancel_pending"] = json!(job["asr_id"].is_string());
    true
}
pub(super) fn dispatch(args: &[String]) -> Result<i32> {
    if args == ["worker"] {
        return worker();
    }
    if args.len() != 2 || args[1] != "--json" {
        return Err("usage: hgs voice <session> --json | hgs voice worker".into());
    }
    let mut bytes = Vec::new();
    io::stdin()
        .take((crate::speech::MAX_REQUEST_BYTES + 1) as u64)
        .read_to_end(&mut bytes)
        .map_err(err)?;
    if bytes.len() > crate::speech::MAX_REQUEST_BYTES {
        return Err("Voice request is too large".into());
    }
    let request: Value = serde_json::from_slice(&bytes).map_err(err)?;
    validate_request(&request)?;
    let target = checked(&args[0], &request)?;
    let config = crate::config::Config::load().map_err(|e| e.message)?;
    if request["action"] == "profiles" {
        println!("{}", crate::speech::voice_computers(&config)?);
        return Ok(0);
    }
    let mut store = Store::open()?;
    let job = if request["action"] == "submit" {
        crate::speech::voice_route(&config, string(&request["speech"], "machine_id"))?;
        let pcm = STANDARD
            .decode(string(&request["audio"], "data_base64"))
            .map_err(err)?;
        let audio = wav(&pcm);
        let digest = format!(
            "{:x}",
            Sha256::digest(
                serde_json::to_vec(&json!([
                    target["agent"],
                    target["account_id"],
                    target["agent_home"],
                    request
                ]))
                .map_err(err)?
            )
        );
        let id = string(&request, "request_id");
        let job = json!({"id":id,"name":args[0],"run_id":target["run_id"],"conversation_id":target["conversation_id"],"agent":target["agent"],"backend":target["backend"],"agent_home":target["agent_home"],"account_id":target["account_id"],
            "created_at":now(),"revision":1,"state":"queued","speech":request["speech"],"attempt":0,
            "audio":{"request_id":id,"index":0,"name":"voice.wav","mime":"audio/wav","bytes":audio.len(),"duration_ms":(pcm.len()*1000/32000).max(1)}});
        let accepted = store.insert(&job, &digest, &audio)?;
        // Failure to start a worker must not hide an accepted durable message.
        if ensure_worker().is_err() {
            eprintln!("hgs: voice message saved; worker startup is pending");
        }
        accepted
    } else {
        let mut job = store
            .get(string(&request, "message_id"))?
            .ok_or("Voice message was not found")?;
        if !belongs(&job, &target) {
            return Err("Voice message belongs to another conversation".into());
        }
        if request["action"] == "cancel" && cancel_job(&mut job) && !store.save(&mut job)? {
            job = store
                .get(string(&request, "message_id"))?
                .ok_or("Voice message was not found")?;
        }
        job
    };
    // Do not expose local target names, account paths, routes or delivery bytes.
    println!("{}", json!({"schema":1,"message":public(&job)}));
    Ok(0)
}

pub(super) fn messages(record: &Value, agent: Option<&str>) -> Vec<Value> {
    if agent.is_some_and(|a| !a.is_empty()) || !root().join("voice/messages.sqlite3").exists() {
        return vec![];
    }
    let jobs = Store::open()
        .and_then(|s| s.jobs(Some(string(record, "conversation_id")), false))
        .unwrap_or_default();
    let mut rows = Vec::new();
    let mut bytes = 0;
    for job in jobs.into_iter().filter(|j| belongs(j, record)) {
        let row = message_row(&job);
        let size = row.to_string().len();
        if bytes + size > 256 * 1024 {
            break;
        }
        bytes += size;
        rows.push(row);
    }
    rows.reverse();
    rows
}
fn message_row(job: &Value) -> Value {
    json!({"source":"hgs_voice","type":"UserMessage","message_id":job["id"],"request_id":job["id"],"at":job["created_at"],"detail":job.get("transcript").cloned().unwrap_or(json!("")),"voice":public(job),"attachments":[job["audio"]]})
}
/// Incremental private history source; bool means the current page is complete.
/// Updates receive a fresh global sequence so an already indexed pending row
/// is revisited when recognition, cancellation or native binding changes it.
pub(super) fn history_page(
    record: &Value,
    agent: Option<&str>,
    after: i64,
) -> Result<(Vec<Value>, i64, bool)> {
    if after < 0 {
        return Err("Invalid voice history sequence".into());
    }
    if agent.is_some_and(|a| !a.is_empty()) || !root().join("voice/messages.sqlite3").exists() {
        return Ok((vec![], after, true));
    }
    let (jobs, next, complete) =
        Store::open()?.changes(string(record, "conversation_id"), after)?;
    Ok((
        jobs.iter()
            .filter(|j| belongs(j, record))
            .map(message_row)
            .collect(),
        next,
        complete,
    ))
}
pub(super) fn read_attachment(
    record: &Value,
    agent: Option<&str>,
    message_id: &str,
    index: usize,
) -> Result<Option<Value>> {
    if agent.is_some_and(|a| !a.is_empty()) || !root().join("voice/messages.sqlite3").exists() {
        return Ok(None);
    }
    let store = Store::open()?;
    let Some(job) = store.get(message_id)? else {
        return Ok(None);
    };
    if index != 0 || !belongs(&job, record) {
        return Err("This voice recording belongs to another conversation".into());
    }
    let mut result = job["audio"].clone();
    result["data_base64"] = json!(STANDARD.encode(store.audio(message_id)?));
    Ok(Some(result))
}
pub(super) fn correlation(record: &Value, message_id: &str) -> Option<Value> {
    let job = Store::open().ok()?.get(message_id).ok()??;
    if !belongs(&job, record) {
        return None;
    }
    Some(
        json!({"native_request_id":job["id"],"native_payload_sha256":job["native_payload_sha256"],"native_submitted_at":job["native_submitted_at"],"boundary":job["boundary"],"native_receipt_status":job["native_receipt_status"],"native_binding":job["native_binding"],"state":job["state"]}),
    )
}

pub(super) fn bind_native(record: &Value, message_id: &str, binding: &Value) -> Result<bool> {
    let object = binding.as_object().ok_or("Invalid native voice binding")?;
    if object.is_empty()
        || object.keys().any(|k| {
            !matches!(
                k.as_str(),
                "provider_message_id"
                    | "provider_source_offset"
                    | "provider_source"
                    | "journal_seq"
            )
        })
        || object.iter().any(|(k, v)| {
            if k == "journal_seq" {
                v.as_i64().is_none_or(|v| v < 1)
            } else {
                !bounded(v, 512, false)
            }
        })
    {
        return Err("Invalid native voice provenance".into());
    }
    let store = Store::open()?;
    let Some(mut job) = store.get(message_id)? else {
        return Ok(false);
    };
    if !belongs(&job, record) || job["state"] != "submitted" {
        return Ok(false);
    }
    if job["native_binding"].is_object() {
        if job["native_binding"] == *binding {
            return Ok(true);
        }
        if job["native_binding"]
            .as_object()
            .unwrap()
            .iter()
            .any(|(key, value)| binding.get(key) != Some(value))
        {
            return Ok(false);
        }
    }
    job["native_binding"] = binding.clone();
    store.save(&mut job)
}

fn current(job: &Value) -> Result<Value> {
    let matches = |r: &Value| belongs(job, r) && r["run_id"] == job["run_id"];
    if let Ok(r) = record(string(job, "name")) {
        if matches(&r) {
            return Ok(r);
        }
    }
    let mut candidates = records()?.into_iter().filter(matches).collect::<Vec<_>>();
    // DSH bindings are separate; inspect only private bounded manifests.
    for entry in fs::read_dir(root().join("dsh/bindings"))
        .into_iter()
        .flatten()
        .flatten()
        .take(10000)
    {
        if let Ok(file) = File::open(entry.path()) {
            let mut bytes = Vec::new();
            if file.take(1_048_577).read_to_end(&mut bytes).is_ok() && bytes.len() <= 1_048_576 {
                if let Ok(r) = serde_json::from_slice::<Value>(&bytes) {
                    if matches(&r) {
                        candidates.push(r);
                    }
                }
            }
        }
    }
    if candidates.len() == 1 {
        Ok(candidates.remove(0))
    } else {
        Err("The original voice conversation is no longer current".into())
    }
}
fn boundary(record: &Value) -> Value {
    let seq = journal::event_db()
        .and_then(|db| {
            db.query_row("SELECT COALESCE(max(seq),0) FROM events", [], |r| {
                r.get::<_, i64>(0)
            })
            .map_err(err)
        })
        .ok();
    let path = string(record, "transcript");
    let metadata = fs::metadata(path).ok();
    json!({"journal_seq":seq,"transcript":path,"source_bytes":metadata.as_ref().map(|m|m.len()),"source_device":metadata.as_ref().map(|m|m.dev()),"source_inode":metadata.as_ref().map(|m|m.ino()),"at":now()})
}
fn receipt(job: &Value) -> Result<Option<Value>> {
    let directory = if job["backend"] == "dsh" || job["agent"] == "dsh" {
        "dsh/receipts"
    } else {
        "input_receipts"
    };
    let path = root()
        .join(directory)
        .join(format!("{}.json", string(job, "id")));
    let file = match OpenOptions::new()
        .read(true)
        .custom_flags(libc::O_NOFOLLOW)
        .open(path)
    {
        Ok(f) => f,
        Err(e) if e.kind() == io::ErrorKind::NotFound => return Ok(None),
        Err(e) => return Err(err(e)),
    };
    let mut bytes = Vec::new();
    file.take(131073).read_to_end(&mut bytes).map_err(err)?;
    if bytes.len() > 131072 {
        return Err("Native voice receipt is oversized".into());
    }
    let value: Value = serde_json::from_slice(&bytes).map_err(err)?;
    if value["request_id"] != job["id"]
        || value["run_id"] != job["run_id"]
        || value["conversation_id"] != job["conversation_id"]
        || value["text"] != job["transcript"]
    {
        return Err("Native request UUID conflicts with this voice message".into());
    }
    Ok(Some(value))
}
fn settle(job: &mut Value, receipt: Option<&Value>, interrupted: bool) {
    if let Some(receipt) = receipt {
        job["native_receipt_status"] = receipt["status"].clone();
        job["native_submitted_at"] = receipt["submitted_at"].clone();
        if receipt["status"] == "submitted" {
            job["state"] = json!("submitted");
            return;
        }
    }
    if receipt.is_some() || interrupted {
        job["state"] = json!("uncertain");
        job["error_code"] = json!("delivery_uncertain");
        job["error"]=json!("Delivery may have reached the agent. Inspect the conversation before sending anything again.");
    } else {
        job["state"] = json!("waiting_agent");
        job["error_code"] = json!("agent_not_ready");
        job["error"]=json!("Waiting for the original agent to accept input. Existing native drafts and permission dialogs are preserved.");
    }
}

fn speech_result(job: &mut Value, value: &Value) -> Result<()> {
    if value["schema"] != 1 || value["job_id"] != job["asr_id"] {
        return Err("Invalid speech result identity".into());
    }
    match string(value, "state") {
        "queued" | "running" => (),
        "completed" => {
            let text = string(value, "text").trim();
            let mut end = text.len().min(32000);
            while !text.is_char_boundary(end) {
                end -= 1;
            }
            job["transcript"] = json!(&text[..end]);
            if end < text.len() {
                job["transcript_truncated"] = json!(true);
            }
            if text.is_empty() {
                failure(
                    job,
                    "no_speech",
                    "No speech was recognized. The original recording is available.",
                );
            } else if text.len() > 32000
                || text.starts_with(['/', '!'])
                || text
                    .chars()
                    .any(|c| c.is_control() && c != '\n' && c != '\t')
            {
                failure(
                    job,
                    "invalid_transcript",
                    "Recognition returned text that cannot be sent safely to the native agent.",
                );
            } else {
                job["transcript"] = json!(text);
                job["state"] = json!("ready");
                job.as_object_mut().unwrap().remove("error");
                job.as_object_mut().unwrap().remove("error_code");
            }
        }
        "failed" | "uncertain"
            if matches!(
                string(value, "error_code"),
                "service_restarted" | "result_expired"
            ) && job["attempt"].as_u64().unwrap_or(0) < 3 =>
        {
            job["state"] = json!("queued");
        }
        "failed" | "uncertain" | "cancelled" => {
            failure(
                job,
                if string(value, "error_code").is_empty() {
                    "recognition_failed"
                } else {
                    string(value, "error_code")
                },
                "Speech recognition failed. The original recording is available.",
            );
        }
        _ => return Err("Invalid speech job state".into()),
    }
    Ok(())
}
fn speech_unavailable(store: &Store, job: &mut Value) -> Result<()> {
    if job["error_code"] != "speech_unavailable" {
        job["error_code"] = json!("speech_unavailable");
        job["error"]=json!("The selected speech computer or service is unavailable. Recognition will retry automatically; the recording is preserved.");
        store.save(job)?;
    }
    Ok(())
}
fn step(store: &Store, config: &crate::config::Config, mut job: Value) -> Result<()> {
    if job["state"] == "cancelled" {
        if job["asr_cancel_pending"] == true {
            let attempts = job["asr_cancel_attempts"].as_u64().unwrap_or(0) + 1;
            let cancelled = crate::speech::voice_request(
                config,
                string(&job["speech"], "machine_id"),
                &json!({"action":"cancel","job_id":job["asr_id"]}),
            )
            .is_ok();
            job["asr_cancel_pending"] = json!(!cancelled && attempts < 3);
            job["asr_cancel_attempts"] = json!(attempts);
            store.save(&mut job)?;
        }
        return Ok(());
    }
    if job["state"] == "delivering" {
        match receipt(&job) {
            Ok(r) => settle(&mut job, r.as_ref(), true),
            Err(_) => failure(
                &mut job,
                "receipt_conflict",
                "Native delivery identity could not be verified.",
            ),
        }
        store.save(&mut job)?;
        return Ok(());
    }
    if now() - job["created_at"].as_f64().unwrap_or(0.) > MAX_AGE {
        failure(&mut job,"processing_expired","Voice processing could not finish within 24 hours. The original recording is available.");
        store.save(&mut job)?;
        return Ok(());
    }
    let target = match current(&job) {
        Ok(r) => r,
        Err(_) => {
            failure(
                &mut job,
                "target_changed",
                "The original conversation or agent run changed. The recording was not sent.",
            );
            store.save(&mut job)?;
            return Ok(());
        }
    };
    if job["state"] == "queued" {
        job["attempt"] = json!(job["attempt"].as_u64().unwrap_or(0) + 1);
        job["asr_id"] = json!(uuid::Uuid::new_v4().to_string());
        job["asr_submitted"] = json!(false);
        job["state"] = json!("transcribing");
        if !store.save(&mut job)? {
            return Ok(());
        }
    }
    if job["state"] == "transcribing" {
        let machine = string(&job["speech"], "machine_id").to_owned();
        // Always recover by status first. Not-found proves no accepted ASR
        // receipt, so resending this same attempt UUID cannot duplicate work.
        let status = crate::speech::voice_request(
            config,
            &machine,
            &json!({"action":"status","job_id":job["asr_id"]}),
        );
        let mut response = match status {
            Ok(v) => v,
            Err(_) => return speech_unavailable(store, &mut job),
        };
        if response["error_code"] == "not_found" {
            let audio = store.audio(string(&job, "id"))?;
            if audio.len() < 44 {
                return Err("Stored voice audio is invalid".into());
            }
            if !store
                .get(string(&job, "id"))?
                .is_some_and(|j| j["revision"] == job["revision"] && j["state"] == "transcribing")
            {
                return Ok(());
            }
            response = match crate::speech::voice_request(
                config,
                &machine,
                &json!({"action":"submit","request_id":job["asr_id"],"profile_id":job["speech"]["profile_id"],"audio":{"format":"pcm_s16le","sample_rate":16000,"channels":1,"data_base64":STANDARD.encode(&audio[44..])}}),
            ) {
                Ok(v) => v,
                Err(_) => return speech_unavailable(store, &mut job),
            };
            // Submit receipts deliberately omit completed text, even on replay.
            if response["state"] == "completed" {
                return Ok(());
            }
        }
        let previous = job.clone();
        if job["error_code"] == "speech_unavailable" {
            job.as_object_mut().unwrap().remove("error_code");
            job.as_object_mut().unwrap().remove("error");
        }
        speech_result(&mut job, &response)?;
        if job != previous {
            store.save(&mut job)?;
        }
        return Ok(());
    }
    if !matches!(string(&job, "state"), "ready" | "waiting_agent") {
        return Ok(());
    }
    job["name"] = target["name"].clone();
    let payload=json!({"request_id":job["id"],"expected_run_id":job["run_id"],"expected_conversation_id":job["conversation_id"],"text":job["transcript"]}).to_string();
    job["native_payload"] = json!(payload);
    job["native_payload_sha256"] = json!(format!("{:x}", Sha256::digest(payload.as_bytes())));
    job["boundary"] = boundary(&target);
    job["state"] = json!("delivering");
    job.as_object_mut().unwrap().remove("error");
    job.as_object_mut().unwrap().remove("error_code");
    if !store.save(&mut job)? {
        return Ok(());
    }
    // A crash after this marker never causes native delivery to replay. The
    // existing send path owns identity, cold-cache consent and PTY guards.
    let result = if target["backend"] == "dsh" || target["agent"] == "dsh" {
        crate::speech::capture(
            &[
                "send".into(),
                string(&target, "name").into(),
                "--json".into(),
            ],
            payload.as_bytes(),
            Duration::from_secs(35),
        )
    } else {
        input::submit(string(&target, "name"), payload.as_bytes(), None)
    };
    let received = receipt(&job);
    match received {
        Ok(r) => {
            settle(&mut job, r.as_ref(), result.is_ok() && r.is_none());
            if job["state"] == "waiting_agent" {
                if let Err(error) = &result {
                    let (code, message) = native_refusal(error);
                    job["error_code"] = json!(code);
                    job["error"] = json!(message);
                }
            }
        }
        Err(_) => failure(
            &mut job,
            "receipt_conflict",
            "Native delivery identity could not be verified.",
        ),
    }
    store.save(&mut job)?;
    Ok(())
}
fn native_refusal(error: &str) -> (&'static str, &'static str) {
    if error.contains("previous input has not been confirmed") {
        ("previous_input_pending","Waiting for the agent to confirm its previous input. Check the conversation or Terminal.")
    } else if error.contains("draft, dialog")
        || error.contains("input mode")
        || error.contains("input terminal")
    {
        ("native_input_blocked","The native Terminal has a draft, dialog or unrecognized prompt. Return it to an empty agent prompt to continue.")
    } else if error.contains("waiting for")
        || error.contains("approval")
        || error.contains("choice")
    {
        ("native_choice_pending","The agent needs a choice or approval in Activity or Terminal before this voice message can continue.")
    } else if error.contains("not running") || error.contains("resume it") {
        (
            "agent_stopped",
            "The original agent is stopped. Voice delivery does not resume agents automatically.",
        )
    } else {
        ("agent_not_ready","The original agent could not accept this message yet. Check Activity or Terminal; the recording is preserved.")
    }
}
fn worker() -> Result<i32> {
    let store = Store::open()?;
    let lock = OpenOptions::new()
        .create(true)
        .truncate(false)
        .append(true)
        .mode(0o600)
        .custom_flags(libc::O_NOFOLLOW)
        .open(root().join("voice/worker.lock"))
        .map_err(err)?;
    if lock.try_lock_exclusive().is_err() {
        return Ok(0);
    }
    let executable = std::env::current_exe().map_err(err)?;
    let original = fs::metadata(&executable).ok().map(|m| (m.dev(), m.ino()));
    loop {
        if fs::metadata(&executable).ok().map(|m| (m.dev(), m.ino())) != original {
            return Ok(0);
        }
        let config = crate::config::Config::load().map_err(|e| e.message)?;
        let mut active = std::collections::HashSet::new();
        for job in store.jobs(None, true)? {
            let key = json!([
                job["agent"],
                job["agent_home"],
                job["account_id"],
                job["conversation_id"]
            ])
            .to_string();
            if !active.insert(key) {
                continue;
            }
            if step(&store, &config, job).is_err() {
                eprintln!("hgs voice: processing postponed; durable message retained");
            }
        }
        std::thread::sleep(Duration::from_secs(2));
    }
}
fn ensure_worker() -> Result<()> {
    #[cfg(target_os = "linux")]
    if home()
        .join(".config/systemd/user/hgs-voice.service")
        .is_file()
        && crate::platform::silent(
            "systemctl",
            &["--user", "start", "--no-block", "hgs-voice.service"],
        )
    {
        return Ok(());
    }
    use std::os::unix::process::CommandExt;
    let mut cmd = Command::new(std::env::current_exe().map_err(err)?);
    cmd.args(["voice", "worker"])
        .stdin(Stdio::null())
        .stdout(Stdio::null())
        .stderr(Stdio::null());
    unsafe {
        cmd.pre_exec(|| {
            if libc::setsid() < 0 {
                Err(io::Error::last_os_error())
            } else {
                Ok(())
            }
        });
    }
    cmd.spawn().map_err(err)?;
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    const ID: &str = "11111111-1111-4111-8111-111111111111";
    fn job() -> Value {
        json!({"id":ID,"name":"codex/fixture","run_id":"run","conversation_id":"conversation","agent":"codex","agent_home":"/private/fixture","account_id":"fixture","created_at":1.,"revision":1,"state":"queued","attempt":1,"asr_id":ID,"speech":{"machine_id":ID,"profile_id":"fixture"},"audio":{"request_id":ID,"index":0,"name":"voice.wav","mime":"audio/wav","bytes":48,"duration_ms":1}})
    }
    fn request() -> Value {
        json!({"action":"submit","request_id":ID,"expected_run_id":"run","expected_conversation_id":"conversation","speech":{"machine_id":ID,"profile_id":"fixture"},"audio":{"format":"pcm_s16le","sample_rate":16000,"channels":1,"data_base64":"AAAAAA=="}})
    }
    #[test]
    fn wav_original_preserves_every_sample() {
        let pcm = [0, 0, 255, 127, 0, 128, 255, 255];
        let value = wav(&pcm);
        assert_eq!(&value[44..], &pcm);
        assert_eq!(&value[..4], b"RIFF");
        assert_eq!(&value[8..16], b"WAVEfmt ");
        assert_eq!(u32::from_le_bytes(value[40..44].try_into().unwrap()), 8);
    }
    #[test]
    fn request_rejects_paths_children_and_unbound_conversation() {
        assert!(validate_request(&request()).is_ok());
        for (key, value) in [
            ("agent_id", json!("child")),
            ("path", json!("/tmp/audio")),
            ("archive_id", json!(ID)),
        ] {
            let mut v = request();
            v[key] = value;
            assert!(validate_request(&v).is_err());
        }
        let mut v = request();
        v["expected_conversation_id"] = json!("");
        assert!(validate_request(&v).is_err());
        v = json!({"action":"profiles","request_id":ID,"expected_run_id":"run","expected_conversation_id":""});
        assert!(validate_request(&v).is_ok());
    }
    #[test]
    fn accept_is_atomic_persistent_and_idempotent() {
        let dir = tempfile::tempdir().unwrap();
        let audio = wav(&[0, 1, 2, 3]);
        {
            let mut store = Store::open_at(&dir.path().join("private")).unwrap();
            store.insert(&job(), "fingerprint", &audio).unwrap();
            assert_eq!(store.insert(&job(), "fingerprint", &audio).unwrap(), job());
            assert!(store.insert(&job(), "different", &audio).is_err());
            assert_eq!(store.jobs(Some("conversation"), false).unwrap().len(), 1);
        }
        let store = Store::open_at(&dir.path().join("private")).unwrap();
        assert_eq!(store.audio(ID).unwrap(), audio);
        assert_eq!(store.get(ID).unwrap().unwrap()["state"], "queued");
    }
    #[test]
    fn cancellation_wins_against_late_asr_result() {
        let dir = tempfile::tempdir().unwrap();
        let mut store = Store::open_at(&dir.path().join("private")).unwrap();
        let mut stale = store.insert(&job(), "fingerprint", &wav(&[0, 0])).unwrap();
        let mut cancelled = stale.clone();
        assert!(cancel_job(&mut cancelled));
        assert!(store.save(&mut cancelled).unwrap());
        speech_result(
            &mut stale,
            &json!({"schema":1,"job_id":ID,"state":"completed","text":"Late transcript"}),
        )
        .unwrap();
        assert!(!store.save(&mut stale).unwrap());
        assert_eq!(store.get(ID).unwrap().unwrap()["state"], "cancelled");
        assert!(!cancel_job(&mut cancelled));
        assert_eq!(store.audio(ID).unwrap(), wav(&[0, 0]));
    }
    #[test]
    fn native_crash_boundary_never_replays() {
        let mut before = job();
        before["state"] = json!("delivering");
        settle(&mut before, None, true);
        assert_eq!(before["state"], "uncertain");
        let mut pending = job();
        settle(
            &mut pending,
            Some(&json!({"status":"in_progress","submitted_at":2.})),
            false,
        );
        assert_eq!(pending["state"], "uncertain");
        let mut accepted = job();
        settle(
            &mut accepted,
            Some(&json!({"status":"submitted","submitted_at":2.})),
            true,
        );
        assert_eq!(accepted["state"], "submitted");
        let mut refused = job();
        settle(&mut refused, None, false);
        assert_eq!(refused["state"], "waiting_agent");
    }
    #[test]
    fn restart_keeps_exact_native_payload_and_audio() {
        let dir = tempfile::tempdir().unwrap();
        let mut interrupted = job();
        interrupted["native_payload"] = json!("{\"text\":\"Exact \\\\ input\"}");
        interrupted["boundary"] = json!({"journal_seq":12,"source_bytes":900});
        interrupted["state"] = json!("delivering");
        {
            let mut store = Store::open_at(&dir.path().join("private")).unwrap();
            store.insert(&interrupted, "hash", &wav(&[1, 2])).unwrap();
        }
        let store = Store::open_at(&dir.path().join("private")).unwrap();
        let mut recovered = store.get(ID).unwrap().unwrap();
        assert_eq!(recovered, interrupted);
        settle(&mut recovered, None, true);
        assert!(store.save(&mut recovered).unwrap());
        assert_eq!(store.get(ID).unwrap().unwrap()["state"], "uncertain");
    }
    #[test]
    fn empty_or_command_transcripts_cannot_submit() {
        for text in ["", "  ", "/clear", "!rm", "bad\u{1b}text"] {
            let mut job = job();
            speech_result(
                &mut job,
                &json!({"schema":1,"job_id":ID,"state":"completed","text":text}),
            )
            .unwrap();
            assert_eq!(job["state"], "failed");
        }
        let mut job = job();
        speech_result(
            &mut job,
            &json!({"schema":1,"job_id":ID,"state":"completed","text":"Привет, мир"}),
        )
        .unwrap();
        assert_eq!(job["state"], "ready");
        assert_eq!(job["transcript"], "Привет, мир");
    }
    #[test]
    fn asr_recovery_has_a_bounded_separate_attempt_budget() {
        let mut v = job();
        speech_result(
            &mut v,
            &json!({"schema":1,"job_id":ID,"state":"uncertain","error_code":"service_restarted"}),
        )
        .unwrap();
        assert_eq!(v["state"], "queued");
        assert_eq!(v["id"], ID);
        v["attempt"] = json!(3);
        speech_result(
            &mut v,
            &json!({"schema":1,"job_id":ID,"state":"uncertain","error_code":"service_restarted"}),
        )
        .unwrap();
        assert_eq!(v["state"], "failed");
        assert!(speech_result(
            &mut job(),
            &json!({"schema":1,"job_id":"different","state":"completed","text":"hello"})
        )
        .is_err());
    }
    #[test]
    fn public_message_never_exposes_private_delivery_material() {
        let mut value = job();
        value["native_payload"] = json!("payload");
        value["boundary"] = json!({"transcript":"/private/path"});
        value["native_binding"] = json!({"journal_seq":2});
        let value = public(&value);
        assert!(value.get("name").is_none());
        assert!(value.get("agent_home").is_none());
        assert!(value.get("native_payload").is_none());
        assert!(value.get("boundary").is_none());
        assert_eq!(value["native_binding"]["journal_seq"], 2);
    }
    #[test]
    fn account_conversation_scope_survives_rename_but_not_rebinding() {
        let job = job();
        let mut target = job.clone();
        target["name"] = json!("codex/renamed");
        target["run_id"] = json!("new-run");
        assert!(belongs(&job, &target));
        target["conversation_id"] = json!("another");
        assert!(!belongs(&job, &target));
        target["conversation_id"] = job["conversation_id"].clone();
        target["account_id"] = json!("another");
        assert!(!belongs(&job, &target));
    }
    #[test]
    fn delivery_started_and_terminal_messages_cannot_be_cancelled() {
        for state in [
            "delivering",
            "submitted",
            "uncertain",
            "failed",
            "cancelled",
        ] {
            let mut v = job();
            v["state"] = json!(state);
            assert!(!cancel_job(&mut v));
            assert_eq!(v["state"], state);
        }
    }
    #[test]
    fn history_changes_are_bounded_and_revisit_revised_rows() {
        let dir = tempfile::tempdir().unwrap();
        let mut store = Store::open_at(&dir.path().join("private")).unwrap();
        for index in 0..125 {
            let mut v = job();
            v["id"] = json!(uuid::Uuid::new_v4().to_string());
            v["created_at"] = json!(index as f64);
            v["state"] = json!("submitted");
            store.insert(&v, "hash", &wav(&[0, 0])).unwrap();
        }
        let (first, next, complete) = store.changes("conversation", 0).unwrap();
        assert_eq!(first.len(), 100);
        assert!(!complete);
        let (last, end, complete) = store.changes("conversation", next).unwrap();
        assert_eq!(last.len(), 25);
        assert!(complete);
        let mut changed = first[0].clone();
        changed["native_binding"] = json!({"journal_seq":3});
        assert!(store.save(&mut changed).unwrap());
        let (revised, sequence, complete) = store.changes("conversation", end).unwrap();
        assert_eq!(revised, vec![changed]);
        assert!(sequence > end);
        assert!(complete);
        assert_eq!(store.jobs(Some("conversation"), false).unwrap().len(), 100);
    }
    #[test]
    fn connection_reopen_preserves_live_wal_visibility() {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("private");
        let worker = Store::open_at(&path).unwrap();
        {
            let mut client = Store::open_at(&path).unwrap();
            client.insert(&job(), "hash", &wav(&[0, 0])).unwrap();
        }
        {
            let inspector = Store::open_at(&path).unwrap();
            assert!(inspector.get(ID).unwrap().is_some());
        }
        let mut job = worker.get(ID).unwrap().unwrap();
        job["state"] = json!("transcribing");
        assert!(worker.save(&mut job).unwrap());
        let reopened = Store::open_at(&path).unwrap();
        assert_eq!(reopened.get(ID).unwrap().unwrap()["state"], "transcribing");
    }

    #[test]
    fn parent_symlink_is_supported_but_database_symlink_is_rejected() {
        use std::os::unix::fs::symlink;
        let dir = tempfile::tempdir().unwrap();
        let target = dir.path().join("real");
        fs::create_dir(&target).unwrap();
        let alias = dir.path().join("alias");
        symlink(&target, &alias).unwrap();
        let path = alias.join("private");
        {
            let mut store = Store::open_at(&path).unwrap();
            store.insert(&job(), "hash", &wav(&[0, 0])).unwrap();
        }
        assert!(Store::open_at(&path).unwrap().get(ID).unwrap().is_some());
        let db = path.join("messages.sqlite3");
        let moved = path.join("saved.sqlite3");
        fs::rename(&db, &moved).unwrap();
        symlink(&moved, &db).unwrap();
        assert!(Store::open_at(&path).is_err());
    }
}
