//! Explicit, provider-neutral dependencies. A declaration is authored by an
//! owned native process or scoped operator; a reply ticket authorizes acknowledgement
//! or completion. Neither prose, a submitted message nor peer idleness settles it.
use super::*;
use anyhow::{ensure, Context};
use rusqlite::{params, Connection, OpenFlags, OptionalExtension};
use sha2::{Digest, Sha256};
use std::io::Read;
use std::{fs, time::Duration};

const ACTIVE_LIMIT: i64 = 1024;
const PREVIEW_LIMIT: i64 = 4;

fn path() -> PathBuf {
    root().join("coordination/waits.sqlite3")
}
fn open_at(path: &Path, write: bool) -> anyhow::Result<Connection> {
    if let Ok(meta) = path.symlink_metadata() {
        ensure!(
            meta.is_file() && !meta.file_type().is_symlink(),
            "Nonregular wait journal"
        );
    }
    if write {
        private_dir(path.parent().context("Missing journal parent")?)
            .map_err(anyhow::Error::msg)?;
        if !path.exists() {
            crate::platform::private_open(fs::OpenOptions::new().write(true).create_new(true))
                .open(path)?;
        }
    }
    let db = Connection::open_with_flags(
        path,
        if write {
            OpenFlags::SQLITE_OPEN_READ_WRITE
        } else {
            OpenFlags::SQLITE_OPEN_READ_ONLY
        },
    )?;
    db.busy_timeout(Duration::from_millis(250))?;
    if write {
        db.execute_batch("PRAGMA journal_mode=DELETE; PRAGMA max_page_count=262144;
            CREATE TABLE IF NOT EXISTS waits(seq INTEGER PRIMARY KEY AUTOINCREMENT,id TEXT UNIQUE NOT NULL,
                waiter_run TEXT NOT NULL,waiter_conversation TEXT NOT NULL,state TEXT NOT NULL,
                created REAL NOT NULL,updated REAL NOT NULL,token TEXT NOT NULL,payload TEXT NOT NULL);
            CREATE INDEX IF NOT EXISTS waits_owner ON waits(waiter_run,waiter_conversation,state,seq);
            CREATE INDEX IF NOT EXISTS waits_pages ON waits(waiter_run,waiter_conversation,seq);
            CREATE INDEX IF NOT EXISTS waits_retention ON waits(state,updated,seq);
            CREATE TABLE IF NOT EXISTS operations(id TEXT PRIMARY KEY,digest TEXT NOT NULL,receipt TEXT NOT NULL,at REAL NOT NULL);
            CREATE INDEX IF NOT EXISTS operations_at ON operations(at);")?;
        db.execute_batch(
            "CREATE TABLE IF NOT EXISTS owners(run TEXT NOT NULL,conversation TEXT NOT NULL,revision INTEGER NOT NULL,PRIMARY KEY(run,conversation));
            CREATE TABLE IF NOT EXISTS declarations(id TEXT PRIMARY KEY,digest TEXT NOT NULL);",
        )?;
    }
    Ok(db)
}
fn uuid(value: &str) -> bool {
    uuid::Uuid::parse_str(value).is_ok_and(|id| !id.is_nil() && id.to_string() == value)
}
fn text<'a>(value: &'a Value, key: &str) -> &'a str {
    value[key].as_str().unwrap_or_default()
}
fn clean(value: &str, max: usize) -> bool {
    value.len() <= max && !value.chars().any(char::is_control)
}
fn public(value: &Value, state: &str) -> Value {
    let mut result = value.clone();
    result["state"] = json!(state);
    result
}
fn ticket(value: &Value, token: &str) -> Value {
    json!({"wait_id":value["id"],"waiter_machine":value["waiter"]["endpoint"]["machine"],
        "expected_run_id":value["waiter"]["run_id"],"expected_conversation_id":value["waiter"]["endpoint"]["conversation"],
        "reply_token":token})
}
fn begin(db: &mut Connection, input: &Value, owner: &Value) -> anyhow::Result<Value> {
    let id = text(input, "request_id");
    ensure!(uuid(id), "request_id must be a canonical UUID");
    let label = text(input, "label");
    ensure!(clean(label, 240), "Invalid wait label");
    let producer = crate::session_reference::Reference::parse(text(input, "producer_reference"))
        .map_err(|e| anyhow::anyhow!(e.message))?;
    ensure!(
        producer.archive.is_empty()
            && producer.agent.is_empty()
            && !producer.conversation.is_empty()
            && ["claude", "codex", "kimi", "dsh", "opencode"]
                .contains(&producer.name.split('/').next().unwrap_or("")),
        "Choose a confirmed independent agent session reference"
    );
    ensure!(
        !text(&owner["endpoint"], "conversation").is_empty(),
        "The waiting conversation is not confirmed"
    );
    ensure!(
        producer.machine != text(&owner["endpoint"], "machine")
            || producer.conversation != text(&owner["endpoint"], "conversation")
            || producer.name.split('/').next() != owner["endpoint"]["provider"].as_str(),
        "A session cannot wait for itself"
    );
    let declaration_digest = format!(
        "{:x}",
        Sha256::digest(json!([owner, input["producer_reference"], label]).to_string())
    );
    let created = now();
    let payload = json!({"schema":1,"id":id,"waiter":owner,"producer":{
        "machine":producer.machine,"host":producer.host,"name":producer.name,
        "provider":producer.name.split('/').next().unwrap_or(""),"run_id":producer.run,"conversation_id":producer.conversation},
        "producer_reference":input["producer_reference"],"label":label,"created_at":created});
    let tx = db.transaction_with_behavior(rusqlite::TransactionBehavior::Immediate)?;
    if let Some((old, state, token)) = tx
        .query_row(
            "SELECT payload,state,token FROM waits WHERE id=?1",
            [id],
            |r| {
                Ok((
                    r.get::<_, String>(0)?,
                    r.get::<_, String>(1)?,
                    r.get::<_, String>(2)?,
                ))
            },
        )
        .optional()?
    {
        let old: Value = serde_json::from_str(&old)?;
        ensure!(
            old["waiter"]["endpoint"] == owner["endpoint"]
                && old["waiter"]["run_id"] == owner["run_id"]
                && old["producer"] == payload["producer"]
                && old["label"] == payload["label"],
            "Wait request identity changed"
        );
        return Ok(
            json!({"wait":public(&old,&state),"reply_ticket":ticket(&old,&token),"replayed":true}),
        );
    }
    ensure!(
        tx.query_row("SELECT 1 FROM declarations WHERE id=?1", [id], |r| r
            .get::<_, i64>(0))
            .optional()?
            .is_none(),
        "This wait request has expired; it cannot be replayed"
    );
    let run = text(owner, "run_id");
    let conversation = text(&owner["endpoint"], "conversation");
    let count: i64 = tx.query_row("SELECT count(*) FROM waits WHERE waiter_run=?1 AND waiter_conversation=?2 AND state IN ('awaiting_ack','awaiting_result')",params![run,conversation],|r|r.get(0))?;
    ensure!(
        count < ACTIVE_LIMIT,
        "This session already has 1024 open waits; settle or cancel one first"
    );
    let token = format!(
        "{}{}",
        uuid::Uuid::new_v4().simple(),
        uuid::Uuid::new_v4().simple()
    );
    tx.execute("INSERT INTO waits(id,waiter_run,waiter_conversation,state,created,updated,token,payload) VALUES(?1,?2,?3,'awaiting_ack',?4,?4,?5,?6)",params![id,run,conversation,created,token,payload.to_string()])?;
    tx.execute("INSERT INTO owners VALUES(?1,?2,1) ON CONFLICT(run,conversation) DO UPDATE SET revision=revision+1",params![run,conversation])?;
    tx.execute(
        "INSERT INTO declarations VALUES(?1,?2)",
        params![id, declaration_digest],
    )?;
    // Retain open dependencies indefinitely; bounded maintenance touches only
    // settled rows. Their immutable seq is the administrative keyset cursor.
    tx.execute("DELETE FROM waits WHERE seq IN (SELECT seq FROM waits WHERE state IN ('completed','cancelled') AND updated<?1 LIMIT 256)",[created-30.0*86400.0])?;
    // Request identities and small operation receipts are permanent tombstones;
    // expiring journal details never permits an old declaration to reopen.
    tx.commit()?;
    Ok(
        json!({"wait":public(&payload,"awaiting_ack"),"reply_ticket":ticket(&payload,&token),"replayed":false}),
    )
}
fn settle(
    db: &mut Connection,
    action: &str,
    input: &Value,
    machine: &str,
    validate_result: fn(&Value, &Value) -> anyhow::Result<Value>,
) -> anyhow::Result<Value> {
    let id = text(input, "request_id");
    let wait = text(input, "wait_id");
    ensure!(
        uuid(id) && uuid(wait),
        "Canonical operation and wait UUIDs are required"
    );
    ensure!(
        text(input, "waiter_machine") == machine,
        "Wait belongs to another computer; choose its configured @host"
    );
    let digest = format!("{:x}", Sha256::digest(json!([action, input]).to_string()));
    let tx = db.transaction_with_behavior(rusqlite::TransactionBehavior::Immediate)?;
    if let Some((old, receipt)) = tx
        .query_row(
            "SELECT digest,receipt FROM operations WHERE id=?1",
            [id],
            |r| Ok((r.get::<_, String>(0)?, r.get::<_, String>(1)?)),
        )
        .optional()?
    {
        ensure!(old == digest, "Operation request identity changed");
        return Ok(serde_json::from_str(&receipt)?);
    }
    let (payload, state, token): (String, String, String) = tx.query_row(
        "SELECT payload,state,token FROM waits WHERE id=?1",
        [wait],
        |r| Ok((r.get(0)?, r.get(1)?, r.get(2)?)),
    )?;
    let mut value: Value = serde_json::from_str(&payload)?;
    ensure!(
        value["waiter"]["endpoint"]["machine"] == machine,
        "Wait journal belongs to another computer"
    );
    ensure!(
        input["expected_run_id"] == value["waiter"]["run_id"]
            && input["expected_conversation_id"] == value["waiter"]["endpoint"]["conversation"],
        "Waiting conversation identity changed"
    );
    if action != "cancel" {
        let supplied = text(input, "reply_token").as_bytes();
        ensure!(
            supplied.len() == token.len()
                && supplied
                    .iter()
                    .zip(token.as_bytes())
                    .fold(0u8, |n, (a, b)| n | (a ^ b))
                    == 0,
            "Invalid scoped reply ticket"
        );
    }
    if action == "finish" && matches!(state.as_str(), "awaiting_ack" | "awaiting_result") {
        value["result_receipt"] = validate_result(&value, input)?;
    }
    let next = match (state.as_str(), action) {
        ("cancelled", "cancel") => "cancelled",
        ("cancelled", _) => anyhow::bail!("This wait was cancelled"),
        ("completed", _) => "completed",
        (_, "cancel") => "cancelled",
        (_, "finish") => "completed",
        (_, "ack") => "awaiting_result",
        _ => anyhow::bail!("Invalid wait operation"),
    };
    let receipt = json!({"schema":1,"request_id":id,"wait_id":wait,"state":next});
    tx.execute(
        "UPDATE waits SET state=?2,updated=max(updated+0.001,?3),payload=?4 WHERE id=?1",
        params![wait, next, now(), value.to_string()],
    )?;
    tx.execute(
        "INSERT INTO operations VALUES(?1,?2,?3,?4)",
        params![id, digest, receipt.to_string(), now()],
    )?;
    tx.execute("INSERT INTO owners VALUES(?1,?2,1) ON CONFLICT(run,conversation) DO UPDATE SET revision=revision+1",
        params![text(&value["waiter"],"run_id"),text(&value["waiter"]["endpoint"],"conversation")])?;
    tx.commit()?;
    Ok(receipt)
}
fn page(db: &Connection, run: &str, conversation: &str, before: i64) -> anyhow::Result<Value> {
    let mut q = db.prepare("SELECT seq,payload,state FROM waits WHERE waiter_run=?1 AND waiter_conversation=?2 AND seq<?3 ORDER BY seq DESC LIMIT 51")?;
    let rows = q
        .query_map(params![run, conversation, before], |r| {
            Ok((
                r.get::<_, i64>(0)?,
                r.get::<_, String>(1)?,
                r.get::<_, String>(2)?,
            ))
        })?
        .collect::<rusqlite::Result<Vec<_>>>()?;
    let more = rows.len() > 50;
    let cursor = if more {
        rows.get(49).map(|r| r.0.to_string())
    } else {
        None
    };
    let items = rows
        .iter()
        .take(50)
        .map(|(_, v, s)| serde_json::from_str(v).map(|v| public(&v, s)))
        .collect::<serde_json::Result<Vec<_>>>()?;
    Ok(json!({"schema":1,"items":items,"next_cursor":cursor}))
}
pub(crate) fn dispatch(args: &[String]) -> Result<i32> {
    if args.len() != 2 || args[1] != "--json" {
        return Err(
            "usage: hgs [@host] wait begin|ack|finish|cancel|list --json (JSON stdin)".into(),
        );
    }
    let mut bytes = Vec::new();
    io::stdin()
        .take(16_385)
        .read_to_end(&mut bytes)
        .map_err(|e| e.to_string())?;
    if bytes.len() > 16_384 {
        return Err("Wait request exceeds 16 KiB".into());
    }
    let input: Value = serde_json::from_slice(&bytes).map_err(|e| e.to_string())?;
    if !input.is_object() {
        return Err("A JSON object is required".into());
    }
    let config = crate::config::Config::load().map_err(|e| e.to_string())?;
    let machine = crate::swarm::local_node_id(&config).map_err(|e| e.to_string())?;
    let mut result = (|| -> anyhow::Result<Value> {
        match args[0].as_str() {
            "begin" => {
                let owner = if let Some(owner) =
                    session_graph::send_origin().filter(|_| input.get("waiter_reference").is_none())
                {
                    owner
                } else {
                    // Deliberate operator/API-host declarations use the same
                    // exact local reference checks as opening a conversation.
                    // This is an authored dependency, never native work proof.
                    let reference = crate::session_reference::Reference::parse(text(
                        &input,
                        "waiter_reference",
                    ))
                    .map_err(|e| anyhow::anyhow!(e.message))?;
                    ensure!(
                        reference.machine == machine
                            && reference.archive.is_empty()
                            && reference.agent.is_empty(),
                        "Choose the waiting computer and its independent conversation"
                    );
                    let name = reference.name.as_str();
                    let direct = if dsh::exists(name) {
                        dsh::binding(name).ok()
                    } else if opencode::exists(name) {
                        opencode::binding(name).ok()
                    } else if reference.run.is_empty() {
                        read(name).ok()
                    } else {
                        read_run(name, &reference.run).map_err(anyhow::Error::msg)?
                    };
                    let record = if let Some(record) = direct {
                        record
                    } else {
                        // The ordinary reference resolver also follows hosted
                        // backend renames; only this exceptional path needs it.
                        let details = crate::session_reference::inspect(&config, &reference)
                            .map_err(|e| anyhow::anyhow!(e.message))?;
                        let name = text(&details, "name");
                        if dsh::exists(name) {
                            dsh::binding(name)
                        } else if opencode::exists(name) {
                            opencode::binding(name)
                        } else {
                            read(name)
                        }
                        .map_err(anyhow::Error::msg)?
                    };
                    ensure!(
                        reference.name.split('/').next() == record["agent"].as_str()
                            && (reference.run.is_empty()
                                || reference.run == text(&record, "run_id"))
                            && reference.conversation == text(&record, "conversation_id"),
                        "Waiting conversation changed; refresh its reference"
                    );
                    session_graph::target_receipt(&record)
                        .context("Waiting identity unavailable")?
                };
                begin(&mut open_at(&path(), true)?, &input, &owner)
            }
            "ack" | "finish" | "cancel" => settle(
                &mut open_at(&path(), true)?,
                &args[0],
                &input,
                &machine,
                |wait, input| {
                    attachments::wait_result(
                        &wait["waiter"],
                        text(input, "result_request_id"),
                        wait["created_at"].as_f64().unwrap_or(f64::MAX),
                    )
                    .map_err(anyhow::Error::msg)
                },
            ),
            "list" => {
                let run = text(&input, "expected_run_id");
                let conversation = text(&input, "expected_conversation_id");
                ensure!(
                    !run.is_empty()
                        && clean(run, 128)
                        && !conversation.is_empty()
                        && clean(conversation, 512),
                    "Exact waiting run and conversation are required"
                );
                let before = if input.get("before").is_some_and(|v| !v.is_null()) {
                    text(&input, "before").parse::<i64>()?
                } else {
                    i64::MAX
                };
                ensure!(before > 0, "Invalid wait page cursor");
                if !path().exists() {
                    return Ok(json!({"schema":1,"items":[],"next_cursor":null}));
                }
                page(&open_at(&path(), false)?, run, conversation, before)
            }
            _ => anyhow::bail!("Unknown wait operation"),
        }
    })()
    .map_err(|e| e.to_string())?;
    if args[0] == "begin" {
        result["reply_host"] = json!(config.host());
    }
    println!("{result}");
    Ok(0)
}

fn summary(db: &Connection, session: &Value) -> anyhow::Result<Value> {
    let run = text(session, "run_id");
    let conversation = text(session, "conversation_id");
    let revision: Option<i64> = db
        .query_row(
            "SELECT revision FROM owners WHERE run=?1 AND conversation=?2",
            params![run, conversation],
            |r| r.get(0),
        )
        .optional()?;
    let Some(revision) = revision else {
        return Ok(Value::Null);
    };
    let count:i64=db.query_row("SELECT count(*) FROM waits WHERE waiter_run=?1 AND waiter_conversation=?2 AND state IN ('awaiting_ack','awaiting_result')",params![run,conversation],|r|r.get(0))?;
    let mut q=db.prepare("SELECT payload,state FROM waits WHERE waiter_run=?1 AND waiter_conversation=?2 AND state IN ('awaiting_ack','awaiting_result') ORDER BY seq LIMIT ?3")?;
    let mut items = Vec::new();
    for row in q.query_map(params![run, conversation, PREVIEW_LIMIT], |r| {
        Ok((r.get::<_, String>(0)?, r.get::<_, String>(1)?))
    })? {
        let (v, s) = row?;
        items.push(public(&serde_json::from_str(&v)?, &s));
    }
    Ok(
        json!({"schema":1,"status":"ok","revision":revision,"run_id":run,"conversation_id":conversation,"count":count,"items":items,"truncated":count>PREVIEW_LIMIT}),
    )
}
pub(super) fn decorate(sessions: &mut [Value]) {
    // One read-only connection per listing, indexed lookups per returned session.
    // No full-journal read, per-session connection, native poll or unbounded cache.
    if !path().exists() {
        return;
    }
    let mut db = open_at(&path(), false);
    let snapshot = db.as_mut().ok().and_then(|db| db.transaction().ok());
    for session in sessions {
        let value = snapshot
            .as_ref()
            .and_then(|db| summary(db, session).ok())
            .unwrap_or_else(|| json!({"schema":1,"status":"unavailable"}));
        if value.is_null() {
            session.as_object_mut().unwrap().remove("coordination_wait");
        } else {
            session["coordination_wait"] = value;
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    fn fixture() -> (tempfile::TempDir, Connection, Value, Value) {
        let dir = tempfile::tempdir().unwrap();
        let db = open_at(&dir.path().join("waits.sqlite3"), true).unwrap();
        let owner = json!({"name":"codex/example/coordinator","run_id":"run-one","endpoint":{"machine":"11111111-1111-4111-8111-111111111111","provider":"codex","conversation":"conversation-one"}});
        let input = json!({"request_id":"33333333-3333-4333-8333-333333333333","label":"Review patch",
            "producer_reference":"zerus://session?machine=22222222-2222-4222-8222-222222222222&host=peer&name=claude%2Fexample%2Fworker&conversation=conversation-two&run=run-two"});
        (dir, db, owner, input)
    }
    fn transition(
        db: &mut Connection,
        action: &str,
        input: &Value,
        machine: &str,
    ) -> anyhow::Result<Value> {
        super::settle(db, action, input, machine, |_, _| {
            Ok(json!({"request_id":"55555555-5555-4555-8555-555555555555"}))
        })
    }
    fn operation(ticket: &Value) -> Value {
        let mut value = ticket.clone();
        value["request_id"] = json!(uuid::Uuid::new_v4().to_string());
        value
    }
    #[test]
    fn explicit_ack_result_and_replays() {
        let (_dir, mut db, owner, input) = fixture();
        let receipt = begin(&mut db, &input, &owner).unwrap();
        assert_eq!(receipt["wait"]["state"], "awaiting_ack");
        assert_eq!(
            begin(&mut db, &input, &owner).unwrap()["reply_ticket"],
            receipt["reply_ticket"]
        );
        let ack = operation(&receipt["reply_ticket"]);
        let machine = text(&owner["endpoint"], "machine");
        assert_eq!(
            transition(&mut db, "ack", &ack, machine).unwrap()["state"],
            "awaiting_result"
        );
        assert_eq!(
            transition(
                &mut db,
                "finish",
                &operation(&receipt["reply_ticket"]),
                machine
            )
            .unwrap()["state"],
            "completed"
        );
        assert_eq!(
            transition(&mut db, "ack", &ack, machine).unwrap()["state"],
            "awaiting_result"
        );
        assert_eq!(
            transition(
                &mut db,
                "ack",
                &operation(&receipt["reply_ticket"]),
                machine
            )
            .unwrap()["state"],
            "completed"
        );
        assert_eq!(
            summary(
                &db,
                &json!({"run_id":"run-one","conversation_id":"conversation-one"})
            )
            .unwrap()["count"],
            0
        );
    }
    #[test]
    fn mismatched_tickets_operations_and_conversations_fail() {
        let (_dir, mut db, owner, input) = fixture();
        let receipt = begin(&mut db, &input, &owner).unwrap();
        let machine = text(&owner["endpoint"], "machine");
        for field in [
            "reply_token",
            "expected_run_id",
            "expected_conversation_id",
            "waiter_machine",
            "wait_id",
        ] {
            let mut bad = operation(&receipt["reply_ticket"]);
            bad[field] = json!("wrong");
            assert!(
                transition(&mut db, "ack", &bad, machine).is_err(),
                "{field}"
            );
        }
        let ack = operation(&receipt["reply_ticket"]);
        transition(&mut db, "ack", &ack, machine).unwrap();
        assert!(transition(&mut db, "finish", &ack, machine).is_err());
        let mut changed = input.clone();
        changed["label"] = json!("Changed task");
        assert!(begin(&mut db, &changed, &owner).is_err());
        assert_eq!(
            summary(
                &db,
                &json!({"run_id":"new-run","conversation_id":"conversation-one"})
            )
            .unwrap(),
            Value::Null
        );
        assert_eq!(
            summary(
                &db,
                &json!({"run_id":"run-one","conversation_id":"new-conversation"})
            )
            .unwrap(),
            Value::Null
        );
        let mut cancel = operation(&receipt["reply_ticket"]);
        cancel.as_object_mut().unwrap().remove("reply_token");
        assert_eq!(
            transition(&mut db, "cancel", &cancel, machine).unwrap()["state"],
            "cancelled"
        );
        assert!(transition(
            &mut db,
            "finish",
            &operation(&receipt["reply_ticket"]),
            machine
        )
        .is_err());
    }
    #[test]
    fn bounded_previews_and_stable_journal_pages() {
        let (_dir, mut db, owner, mut input) = fixture();
        let machine = text(&owner["endpoint"], "machine");
        for _ in 0..80 {
            input["request_id"] = json!(uuid::Uuid::new_v4().to_string());
            let receipt = begin(&mut db, &input, &owner).unwrap();
            transition(
                &mut db,
                "finish",
                &operation(&receipt["reply_ticket"]),
                machine,
            )
            .unwrap();
        }
        let first = page(&db, "run-one", "conversation-one", i64::MAX).unwrap();
        assert_eq!(first["items"].as_array().unwrap().len(), 50);
        input["request_id"] = json!(uuid::Uuid::new_v4().to_string());
        begin(&mut db, &input, &owner).unwrap();
        let second = page(
            &db,
            "run-one",
            "conversation-one",
            text(&first, "next_cursor").parse().unwrap(),
        )
        .unwrap();
        assert_eq!(second["items"].as_array().unwrap().len(), 30);
        for _ in 1..ACTIVE_LIMIT {
            input["request_id"] = json!(uuid::Uuid::new_v4().to_string());
            begin(&mut db, &input, &owner).unwrap();
        }
        input["request_id"] = json!(uuid::Uuid::new_v4().to_string());
        assert!(begin(&mut db, &input, &owner).is_err());
        let view = summary(
            &db,
            &json!({"run_id":"run-one","conversation_id":"conversation-one"}),
        )
        .unwrap();
        assert_eq!(view["count"], ACTIVE_LIMIT);
        assert_eq!(view["items"].as_array().unwrap().len(), 4);
        assert_eq!(view["truncated"], true);
        assert!(!view.to_string().contains("reply_token"));
        assert!(!first.to_string().contains("reply_token"));
    }
    #[test]
    fn expiring_details_never_reopens_declarations_or_loses_operation_identity() {
        let (_dir, mut db, owner, input) = fixture();
        let receipt = begin(&mut db, &input, &owner).unwrap();
        let finish = operation(&receipt["reply_ticket"]);
        let machine = text(&owner["endpoint"], "machine");
        let done = transition(&mut db, "finish", &finish, machine).unwrap();
        db.execute(
            "DELETE FROM waits WHERE id=?1",
            [text(&input, "request_id")],
        )
        .unwrap();
        assert!(begin(&mut db, &input, &owner).is_err());
        assert_eq!(
            transition(&mut db, "finish", &finish, machine).unwrap(),
            done
        );
        assert!(transition(
            &mut db,
            "ack",
            &operation(&receipt["reply_ticket"]),
            machine
        )
        .is_err());
    }
}
