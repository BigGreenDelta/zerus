//! Rebuildable metadata index. Archive JSON files remain authoritative; no journal
//! is opened until its row belongs to a requested page. Callers hold the state lock.
use super::*;
use base64::{engine::general_purpose::URL_SAFE_NO_PAD, Engine};
use rusqlite::{params, Connection};
use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use std::fs;

const MAX_DATE: u64 = 253_402_300_800;
#[derive(Clone, Debug, Deserialize, Serialize, PartialEq)]
#[serde(deny_unknown_fields)]
pub(crate) struct Query {
    #[serde(default)]
    pub from: u64,
    #[serde(default = "end_of_time")]
    pub until: u64,
    #[serde(default = "page_size")]
    pub limit: u64,
    #[serde(default)]
    pub cursor: String,
    #[serde(default)]
    pub query: String,
}
fn query_hash(query: &str) -> String {
    format!("{:x}", Sha256::digest(query.as_bytes()))
}
fn end_of_time() -> u64 {
    MAX_DATE
}
fn page_size() -> u64 {
    50
}
#[derive(Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
struct Cursor {
    version: u8,
    catalog: String,
    from: u64,
    until: u64,
    query: String,
    at: f64,
    id: String,
    #[serde(default)]
    newer: bool,
}

pub(crate) fn validate(value: &Value) -> Result<Query> {
    let q: Query = serde_json::from_value(value.clone()).map_err(|_| "invalid archive query")?;
    if q.from >= q.until
        || q.until > MAX_DATE
        || !(1..=50).contains(&q.limit)
        || q.cursor.len() > 1024
        || q.query.chars().count() > 256
        || q.query.chars().any(char::is_control)
    {
        return Err("invalid archive range, page size or search".into());
    }
    if !q.cursor.is_empty() {
        decode(&q)?;
    }
    Ok(q)
}
fn decode(q: &Query) -> Result<Cursor> {
    let bytes = URL_SAFE_NO_PAD
        .decode(&q.cursor)
        .map_err(|_| "invalid archive cursor")?;
    let c: Cursor = serde_json::from_slice(&bytes).map_err(|_| "invalid archive cursor")?;
    if c.version != 1
        || c.from != q.from
        || c.until != q.until
        || c.query != query_hash(&q.query)
        || !c.at.is_finite()
        || c.at < q.from as f64
        || c.at >= q.until as f64
        || uuid::Uuid::parse_str(&c.id).is_err()
    {
        return Err("archive cursor belongs to another query; refresh the archive".into());
    }
    Ok(c)
}
fn stamp(path: &Path) -> Result<String> {
    let m = fs::metadata(path).map_err(|e| e.to_string())?;
    Ok(format!(
        "{}:{:?}",
        m.len(),
        m.modified().map_err(|e| e.to_string())?
    ))
}
fn sql<T>(r: rusqlite::Result<T>) -> Result<T> {
    r.map_err(|e| format!("archive metadata index: {e}"))
}

pub(super) fn catalog() -> Result<Connection> {
    private_dir(&root())?;
    let mut db = sql(Connection::open(root().join("archive-catalog-v1.sqlite3")))?;
    sql(db.execute_batch("CREATE TABLE IF NOT EXISTS catalog_meta(key TEXT PRIMARY KEY,value TEXT NOT NULL);
        CREATE TABLE IF NOT EXISTS archive_rows(id TEXT PRIMARY KEY,name TEXT NOT NULL,run TEXT NOT NULL,conversation TEXT NOT NULL,at REAL NOT NULL,search TEXT NOT NULL,stamp TEXT NOT NULL,seen TEXT NOT NULL);
        CREATE INDEX IF NOT EXISTS archive_by_date ON archive_rows(at DESC,id DESC);
        CREATE INDEX IF NOT EXISTS archive_identity ON archive_rows(name,run,conversation);"))?;
    sql(db.execute(
        "INSERT OR IGNORE INTO catalog_meta VALUES('id',?)",
        [uuid::Uuid::new_v4().to_string()],
    ))?;
    let directory = root().join("archives");
    let current = if directory.exists() {
        stamp(&directory)?
    } else {
        "absent".into()
    };
    let prior: String = sql(db.query_row(
        "SELECT COALESCE((SELECT value FROM catalog_meta WHERE key='stamp'),'')",
        [],
        |r| r.get(0),
    ))?;
    if current == prior {
        return Ok(db);
    }
    let tx = sql(db.transaction())?;
    let generation = uuid::Uuid::new_v4().to_string();
    if directory.exists() {
        for entry in fs::read_dir(&directory).map_err(|e| e.to_string())? {
            let entry = entry.map_err(|e| e.to_string())?;
            if !entry.file_type().map_err(|e| e.to_string())?.is_file() {
                continue;
            }
            let path = entry.path();
            if path.extension().and_then(|e| e.to_str()) != Some("json") {
                continue;
            }
            let id = path.file_stem().and_then(|e| e.to_str()).unwrap_or("");
            if uuid::Uuid::parse_str(id).is_err() {
                continue;
            }
            let file_stamp = stamp(&path)?;
            let unchanged = sql(tx.execute(
                "UPDATE archive_rows SET seen=? WHERE id=? AND stamp=?",
                params![generation, id, file_stamp],
            ))? > 0;
            if unchanged {
                continue;
            }
            let loaded = (|| -> Result<Value> {
                let value: Value =
                    serde_json::from_str(&fs::read_to_string(&path).map_err(|e| e.to_string())?)
                        .map_err(|e| e.to_string())?;
                archive::read_archive(string(&value, "name"), id)
            })();
            let record = match loaded {
                Ok(r) => r,
                Err(e) => {
                    eprintln!("hgs: archive {id} omitted from catalog: {e}");
                    continue;
                }
            };
            let at = record["archived_at"]
                .as_f64()
                .filter(|n| n.is_finite() && *n >= 0.0 && *n < MAX_DATE as f64)
                .unwrap_or(0.0);
            let search = [
                "name",
                "archive_id",
                "agent",
                "conversation_id",
                "run_id",
                "cwd",
                "launch_dir",
                "title",
            ]
            .iter()
            .map(|k| {
                if *k == "title" {
                    string(&record, k).chars().take(1024).collect::<String>()
                } else {
                    string(&record, k).to_owned()
                }
            })
            .collect::<Vec<_>>()
            .join(" ")
            .to_lowercase();
            sql(tx.execute(
                "INSERT OR REPLACE INTO archive_rows VALUES(?,?,?,?,?,?,?,?)",
                params![
                    id,
                    string(&record, "name"),
                    string(&record, "run_id"),
                    string(&record, "conversation_id"),
                    at,
                    search,
                    file_stamp,
                    generation
                ],
            ))?;
        }
    }
    sql(tx.execute("DELETE FROM archive_rows WHERE seen!=?", [generation]))?;
    sql(tx.execute(
        "INSERT OR REPLACE INTO catalog_meta VALUES('stamp',?)",
        [current],
    ))?;
    sql(tx.commit())?;
    Ok(db)
}
pub(super) fn contains(db: &Connection, record: &Value) -> Result<bool> {
    sql(db.query_row(
        "SELECT EXISTS(SELECT 1 FROM archive_rows WHERE name=? AND run=? AND conversation=?)",
        params![
            string(record, "name"),
            string(record, "run_id"),
            string(record, "conversation_id")
        ],
        |r| r.get(0),
    ))
}
/// Notification admission must neither rebuild the catalog nor reconcile an
/// archive. A stale/missing index is unknown until the next ordinary listing.
pub(super) fn contains_existing(record: &Value) -> Result<bool> {
    let directory = root().join("archives");
    if !directory.exists() { return Ok(false); }
    let db = sql(Connection::open_with_flags(root().join("archive-catalog-v1.sqlite3"), rusqlite::OpenFlags::SQLITE_OPEN_READ_ONLY))?;
    let prior: String = sql(db.query_row("SELECT COALESCE((SELECT value FROM catalog_meta WHERE key='stamp'),'')", [], |row| row.get(0)))?;
    if prior != stamp(&directory)? { return Err("Archive catalog changed; refresh the session list before changing notifications".into()); }
    contains(&db, record)
}
pub(super) fn find(db: &Connection, record: &Value) -> Result<Option<String>> {
    use rusqlite::OptionalExtension;
    sql(db.query_row("SELECT id FROM archive_rows WHERE name=? AND run=? AND conversation=? ORDER BY id LIMIT 1",params![string(record,"name"),string(record,"run_id"),string(record,"conversation_id")],|r|r.get(0)).optional())
}
/// Cheap invalidation for a bounded legacy preview; no archive file is opened.
pub(crate) fn revision() -> Result<String> {
    let dir = root().join("archives");
    Ok(query_hash(&if dir.exists() {
        stamp(&dir)?
    } else {
        "absent".into()
    }))
}
pub(crate) fn read_query() -> Result<Value> {
    use std::io::Read;
    let mut bytes = Vec::new();
    io::stdin()
        .take(8193)
        .read_to_end(&mut bytes)
        .map_err(|e| e.to_string())?;
    if bytes.len() > 8192 {
        return Err("archive query exceeds 8 KiB".into());
    }
    serde_json::from_slice(&bytes).map_err(|_| "invalid archive query JSON".into())
}

pub(crate) fn validate_lookup(value: &Value) -> Result<(&str, &str)> {
    let fields = value.as_object().ok_or("invalid archive lookup")?;
    let name = string(value, "name");
    let id = string(value, "archive_id");
    if fields.len() != 2
        || name.is_empty()
        || name.len() > 2048
        || name.starts_with(['-', '@'])
        || name.contains('@')
        || name.chars().any(char::is_control)
        || uuid::Uuid::parse_str(id).is_err()
    {
        return Err("archive lookup requires an exact session name and archive ID".into());
    }
    Ok((name, id))
}

/// Recover a phone's exact target after a connector restart without listing
/// other archives, reading transcript journals or rebuilding the catalog.
pub(crate) fn lookup(value: &Value) -> Result<Value> { lookup_scoped(value,false) }
pub(crate) fn lookup_scoped(value: &Value, windows_mobile: bool) -> Result<Value> {
    let (name, id) = validate_lookup(value)?;
    let _guard = lock(None)?;
    let path = archive::path(id)?;
    if !fs::symlink_metadata(&path)
        .map_err(|e| e.to_string())?
        .file_type()
        .is_file()
    {
        return Err("archive record is not a regular file".into());
    }
    let record = archive::read_archive(name, id)?;
    let mut result=json!({"version":1,"name":name,"archive_id":id,
        "run_id":string(&record,"run_id"),"conversation_id":string(&record,"conversation_id")});
    if windows_mobile {
        if !supported_provider(&record["agent"]) { return Err("provider is unavailable on a Windows local gateway".into()); }
        result["cmd"]=record["agent"].clone();
    }
    Ok(result)
}
pub(crate) fn page(value: &Value) -> Result<Value> {
    let q = validate(value)?;
    let _guard = lock(None)?;
    let db = catalog()?;
    let catalog: String = sql(db.query_row(
        "SELECT value FROM catalog_meta WHERE key='id'",
        [],
        |r| r.get(0),
    ))?;
    let cursor = if q.cursor.is_empty() {
        None
    } else {
        Some(decode(&q)?)
    };
    if cursor.as_ref().is_some_and(|c| c.catalog != catalog) {
        return Err(
            "archive cursor belongs to another computer or rebuilt catalog; refresh".into(),
        );
    }
    let at = cursor.as_ref().map_or(MAX_DATE as f64, |c| c.at);
    let id = cursor.as_ref().map_or("", |c| c.id.as_str());
    let needle = q.query.trim().to_lowercase();
    let total: u64 = sql(db.query_row(
        "SELECT count(*) FROM archive_rows WHERE at>=? AND at<? AND instr(search,?)>0",
        params![q.from, q.until, needle],
        |r| r.get(0),
    ))?;
    let newer = cursor.as_ref().is_some_and(|c| c.newer);
    let mut stmt=sql(db.prepare(if newer {
        "SELECT id,name,at FROM archive_rows WHERE at>=? AND at<? AND instr(search,?)>0 AND (at>? OR (at=? AND id>?)) ORDER BY at ASC,id ASC LIMIT ?"
    } else {
        "SELECT id,name,at FROM archive_rows WHERE at>=? AND at<? AND instr(search,?)>0 AND (at<? OR (at=? AND id<?)) ORDER BY at DESC,id DESC LIMIT ?"
    }))?;
    let rows = sql(
        stmt.query_map(params![q.from, q.until, needle, at, at, id, q.limit], |r| {
            Ok((
                r.get::<_, String>(0)?,
                r.get::<_, String>(1)?,
                r.get::<_, f64>(2)?,
            ))
        }),
    )?;
    let mut rows = rows
        .collect::<rusqlite::Result<Vec<_>>>()
        .map_err(|e| e.to_string())?;
    if newer {
        rows.reverse();
    }
    let position: u64 = if let Some((first_id, _, first_at)) = rows.first() {
        sql(db.query_row("SELECT count(*) FROM archive_rows WHERE at>=? AND at<? AND instr(search,?)>0 AND (at>? OR (at=? AND id>?))",
            params![q.from,q.until,needle,first_at,first_at,first_id],|r|r.get(0)))?
    } else {
        total
    };
    let records = rows
        .iter()
        .take(q.limit as usize)
        .map(|(id, name, _)| archive::read_archive(name, id))
        .collect::<Result<Vec<_>>>()?;
    drop(_guard); // Journal summaries never block live hooks or lifecycle writers.
    let mut summaries = Vec::new();
    for ((id, name, at), record) in rows.iter().zip(&records) {
        let parts: Vec<_> = name.splitn(3, '/').collect();
        let mut row = journal::summary(record, false);
        row.as_object_mut().ok_or("invalid archive summary")?.extend(json!({"name":name,"cmd":record["agent"],"project":parts.get(1),"tag":parts.get(2),"attached":0,"clients":[],"created":record["created"],"state":"archived","resumable":!string(record,"conversation_id").is_empty(),"archive_id":id,"archived_at":at}).as_object().unwrap().clone());
        if let Some(title) = record["title"].as_str() {
            row["title"] = json!(title.chars().take(1024).collect::<String>());
        }
        summaries.push(row);
    }
    workspace::enrich(&mut summaries, &BTreeMap::new());
    let mut sessions = Vec::new();
    let mut bytes = 0;
    for row in summaries {
        let size = serde_json::to_vec(&row).map_err(|e| e.to_string())?.len();
        if bytes + size > 250_000 {
            if sessions.is_empty() {
                return Err("archived session summary exceeds page limit".into());
            }
            break;
        }
        bytes += size;
        sessions.push(row);
    }
    let encode = |at: f64, id: &str, newer: bool| -> Result<String> {
        Ok(URL_SAFE_NO_PAD.encode(
            serde_json::to_vec(&Cursor {
                version: 1,
                catalog: catalog.clone(),
                from: q.from,
                until: q.until,
                query: query_hash(&q.query),
                at,
                id: id.to_owned(),
                newer,
            })
            .map_err(|e| e.to_string())?,
        ))
    };
    let next = if !sessions.is_empty() && position + (sessions.len() as u64) < total {
        let (id, _, at) = &rows[sessions.len() - 1];
        encode(*at, id, false)?
    } else {
        String::new()
    };
    let previous = if position > 0 {
        if let Some((id, _, at)) = rows.first() {
            encode(*at, id, true)?
        } else if let Some(c) = cursor {
            encode(c.at, &c.id, true)?
        } else {
            String::new()
        }
    } else {
        String::new()
    };
    Ok(
        json!({"version":1,"sessions":sessions,"next_cursor":next,"previous_cursor":previous,"position":position,"total":total,"from":q.from,"until":q.until,"query":q.query}),
    )
}

fn supported_provider(provider: &Value) -> bool {
    matches!(provider.as_str(),Some("codex"|"claude"|"kimi"))
}
/// The authoritative archive reader already rejects providers outside AGENTS
/// before indexing/counting. Keep its bounded totals and cursors unchanged.
/// Refuse an inconsistent page instead of guessing a filtered total.
pub(crate) fn page_scoped(value: &Value, windows_mobile: bool) -> Result<Value> {
    let result=page(value)?;
    if windows_mobile && !result["sessions"].as_array().is_some_and(|rows|
        rows.iter().all(|row|supported_provider(&row["cmd"]))) {
        return Err("archive provider scope is unavailable".into());
    }
    Ok(result)
}
