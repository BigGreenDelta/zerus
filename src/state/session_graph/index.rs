//! Derived, metadata-only projection with transactional revisions. A partial
//! scan never removes unseen sources; an invalid source remains explicit.
use super::model::{Item, Projection};
use anyhow::{ensure, Result};
use base64::{engine::general_purpose::URL_SAFE_NO_PAD, Engine};
use rusqlite::{params, Connection, OptionalExtension, Transaction};
use serde::{Deserialize, Serialize};
use serde_json::{json, Value};
use std::{collections::BTreeSet, path::Path, time::Duration};

pub(super) struct Index {
    db: Connection,
    pub epoch: String,
}
#[derive(Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
struct Cursor {
    epoch: String,
    revision: i64,
    after: String,
    changes: bool,
}

impl Index {
    pub fn open(path: &Path, machine: &str) -> Result<Self> {
        if let Ok(meta) = path.symlink_metadata() {
            ensure!(
                meta.is_file() && !meta.file_type().is_symlink(),
                "Graph cache must be a regular file"
            );
        }
        let db = Connection::open(path)?;
        db.busy_timeout(Duration::from_millis(100))?;
        db.execute_batch("PRAGMA journal_mode=DELETE; PRAGMA max_page_count=8192;
            CREATE TABLE IF NOT EXISTS meta(key TEXT PRIMARY KEY,value TEXT NOT NULL);
            CREATE TABLE IF NOT EXISTS sources(source TEXT PRIMARY KEY,seen INTEGER NOT NULL,stamp TEXT NOT NULL,error TEXT NOT NULL DEFAULT '');
            CREATE TABLE IF NOT EXISTS contributions(source TEXT NOT NULL,id TEXT NOT NULL,payload TEXT NOT NULL,PRIMARY KEY(source,id));
            CREATE INDEX IF NOT EXISTS contributions_id ON contributions(id);
            CREATE TABLE IF NOT EXISTS items(id TEXT PRIMARY KEY,payload TEXT NOT NULL,revision INTEGER NOT NULL,deleted INTEGER NOT NULL DEFAULT 0);
            CREATE INDEX IF NOT EXISTS items_revision ON items(revision,id);")?;
        for (key, value) in [
            ("schema", "1".to_owned()),
            ("machine", machine.to_owned()),
            ("epoch", uuid::Uuid::new_v4().to_string()),
            ("revision", "0".into()),
            ("generation", "1".into()),
            ("scan_after", String::new()),
            ("complete", "false".into()),
        ] {
            db.execute(
                "INSERT OR IGNORE INTO meta(key,value) VALUES(?1,?2)",
                params![key, value],
            )?;
        }
        let get = |key| {
            db.query_row("SELECT value FROM meta WHERE key=?1", [key], |r| {
                r.get::<_, String>(0)
            })
        };
        ensure!(
            get("schema")? == "1" && get("machine")? == machine,
            "Graph cache belongs to another schema or computer"
        );
        let epoch = get("epoch")?;
        Ok(Self { db, epoch })
    }
    pub fn get(&self, key: &str) -> Result<String> {
        Ok(self
            .db
            .query_row("SELECT value FROM meta WHERE key=?1", [key], |r| r.get(0))?)
    }
    pub fn set(&self, key: &str, value: &str) -> Result<()> {
        self.db.execute(
            "INSERT INTO meta VALUES(?1,?2) ON CONFLICT(key) DO UPDATE SET value=excluded.value",
            params![key, value],
        )?;
        Ok(())
    }
    pub fn generation(&self) -> Result<i64> {
        Ok(self.get("generation")?.parse()?)
    }
    pub fn stamp(&self, source: &str) -> Result<Option<String>> {
        Ok(self
            .db
            .query_row(
                "SELECT stamp FROM sources WHERE source=?1 AND error=''",
                [source],
                |r| r.get(0),
            )
            .optional()?)
    }
    pub fn seen(&self, source: &str, generation: i64) -> Result<()> {
        self.db.execute(
            "UPDATE sources SET seen=?2 WHERE source=?1",
            params![source, generation],
        )?;
        Ok(())
    }
    pub fn source_error(&self, source: &str, generation: i64, error: &str) -> Result<()> {
        self.db.execute("INSERT INTO sources VALUES(?1,?2,'',?3) ON CONFLICT(source) DO UPDATE SET seen=excluded.seen,error=excluded.error",params![source,generation,error])?;
        Ok(())
    }
    pub fn publish(
        &mut self,
        source: &str,
        stamp: &str,
        generation: i64,
        projection: &Projection,
        error: &str,
    ) -> Result<()> {
        ensure!(
            projection.len() <= 1024 && serde_json::to_vec(projection)?.len() <= 2 * 1024 * 1024,
            "Graph source exceeds projection budget"
        );
        let tx = self.db.transaction()?;
        let mut affected = source_ids(&tx, source)?;
        tx.execute("DELETE FROM contributions WHERE source=?1", [source])?;
        for (id, item) in projection {
            affected.insert(id.clone());
            tx.execute(
                "INSERT INTO contributions VALUES(?1,?2,?3)",
                params![source, id, serde_json::to_string(item)?],
            )?;
        }
        tx.execute("INSERT INTO sources VALUES(?1,?2,?3,?4) ON CONFLICT(source) DO UPDATE SET seen=excluded.seen,stamp=excluded.stamp,error=excluded.error",
            params![source,generation,stamp,error])?;
        reconcile(&tx, affected)?;
        tx.commit()?;
        Ok(())
    }
    pub fn finish_scan(&mut self, generation: i64, complete: bool) -> Result<()> {
        let tx = self.db.transaction()?;
        // Only a complete directory inventory authorizes removal. Source parse
        // errors are coverage gaps, not permission to infer vanished sessions.
        if complete {
            let affected: BTreeSet<String> = {
                let mut q=tx.prepare("SELECT DISTINCT id FROM contributions JOIN sources USING(source) WHERE seen<>?1")?;
                let rows = q.query_map([generation], |r| r.get(0))?;
                rows.collect::<rusqlite::Result<_>>()?
            };
            tx.execute("DELETE FROM contributions WHERE source IN(SELECT source FROM sources WHERE seen<>?1)",[generation])?;
            tx.execute("DELETE FROM sources WHERE seen<>?1", [generation])?;
            reconcile(&tx, affected)?;
        }
        tx.execute(
            "UPDATE meta SET value=?1 WHERE key='generation'",
            [(generation + 1).to_string()],
        )?;
        tx.execute("UPDATE meta SET value='' WHERE key='scan_after'", [])?;
        tx.execute(
            "UPDATE meta SET value=?1 WHERE key='complete'",
            [if complete { "true" } else { "false" }],
        )?;
        tx.commit()?;
        Ok(())
    }
    pub fn page(&self, token: Option<&str>, changes: bool, limit: usize) -> Result<Value> {
        ensure!(
            (1..=200).contains(&limit),
            "Graph page limit must be 1..200"
        );
        let revision: i64 = self.get("revision")?.parse()?;
        let cursor = if let Some(token) = token {
            ensure!(token.len() <= 2048, "Invalid graph cursor");
            let c: Cursor = serde_json::from_slice(&URL_SAFE_NO_PAD.decode(token)?)?;
            ensure!(
                c.revision >= 0 && c.after.len() <= 160,
                "Invalid graph cursor"
            );
            c
        } else {
            Cursor {
                epoch: self.epoch.clone(),
                revision: if changes { 0 } else { revision },
                after: String::new(),
                changes,
            }
        };
        let coverage = json!({"bindings_complete":self.get("complete")?=="true",
            "source_errors":self.db.query_row("SELECT count(*) FROM sources WHERE error<>''",[],|r|r.get::<_,i64>(0))?,
            "communication":"verified_zerus_sends","historical_communication":"unattributed",
            "native_children":"persisted_rosters","archives":"saved_archive_metadata",
            "communication_retention_days":30});
        if cursor.epoch != self.epoch
            || cursor.revision > revision
            || (!changes && cursor.revision != revision)
            || (token.is_some()
                && cursor.changes != changes
                && !(changes && cursor.after.is_empty()))
        {
            return Ok(
                json!({"schema":1,"epoch":self.epoch,"revision":revision,"reset_required":true,"items":[],"coverage":coverage}),
            );
        }
        // Changes use unique revisions, so the next cursor is the last emitted
        // revision. Snapshots are revision-pinned: concurrent mutation requires
        // a reset rather than silently omitting nodes between pages.
        let sql = if changes {
            "SELECT id,payload,revision,deleted FROM items WHERE revision>?1 ORDER BY revision LIMIT ?3"
        } else {
            "SELECT id,payload,revision,deleted FROM items WHERE id>?2 AND deleted=0 ORDER BY id LIMIT ?3"
        };
        let mut q = self.db.prepare(sql)?;
        let rows = q.query_map(params![cursor.revision, cursor.after, limit + 1], |r| {
            Ok((
                r.get::<_, String>(0)?,
                r.get::<_, String>(1)?,
                r.get::<_, i64>(2)?,
                r.get::<_, bool>(3)?,
            ))
        })?;
        let mut values = Vec::new();
        let mut output_bytes = 0;
        let mut more = false;
        for row in rows {
            let (id, payload, rev, deleted) = row?;
            ensure!(
                payload.len() <= 256 * 1024,
                "Graph item exceeds page budget"
            );
            let value = json!({"id":id,"item":serde_json::from_str::<Value>(&payload)?,"revision":rev,"deleted":deleted});
            let size = serde_json::to_vec(&value)?.len();
            if values.len() == limit
                || (!values.is_empty() && output_bytes + size > 1024 * 1024 - 4096)
            {
                more = true;
                break;
            }
            output_bytes += size;
            values.push(value);
        }
        let next_revision = if changes {
            values
                .last()
                .and_then(|v| v["revision"].as_i64())
                .unwrap_or(revision)
        } else {
            revision
        };
        let after = if !changes && more {
            values.last().unwrap()["id"].as_str().unwrap().to_owned()
        } else {
            String::new()
        };
        let next = URL_SAFE_NO_PAD.encode(serde_json::to_vec(&Cursor {
            epoch: self.epoch.clone(),
            revision: next_revision,
            after,
            changes,
        })?);
        Ok(
            json!({"schema":1,"epoch":self.epoch,"revision":revision,"reset_required":false,"items":values,"has_more":more,"cursor":next,"coverage":coverage}),
        )
    }
}

fn source_ids(tx: &Transaction<'_>, source: &str) -> Result<BTreeSet<String>> {
    let mut q = tx.prepare("SELECT id FROM contributions WHERE source=?1")?;
    let rows = q
        .query_map([source], |r| r.get(0))?
        .collect::<rusqlite::Result<_>>()?;
    Ok(rows)
}
fn reconcile(tx: &Transaction<'_>, ids: BTreeSet<String>) -> Result<()> {
    let mut revision: i64 = tx
        .query_row("SELECT value FROM meta WHERE key='revision'", [], |r| {
            r.get::<_, String>(0)
        })?
        .parse()?;
    for id in ids {
        let contributions: Vec<String> = {
            let mut q = tx.prepare(
                "SELECT DISTINCT payload FROM contributions WHERE id=?1 ORDER BY payload LIMIT 33",
            )?;
            let rows = q.query_map([&id], |r| r.get(0))?;
            rows.collect::<rusqlite::Result<_>>()?
        };
        let deleted = contributions.is_empty();
        let payload = match contributions.as_slice() {
            [] => "null".to_owned(),
            [one] => one.clone(),
            _ => serde_json::to_string(&Item {
                kind: "conflict".into(),
                value: json!({"truncated":contributions.len()>32,"observations":contributions.iter().take(32).map(|p|serde_json::from_str::<Value>(p)).collect::<serde_json::Result<Vec<_>>>()?}),
            })?,
        };
        let prior: Option<(String, bool)> = tx
            .query_row(
                "SELECT payload,deleted FROM items WHERE id=?1",
                [&id],
                |r| Ok((r.get(0)?, r.get(1)?)),
            )
            .optional()?;
        if prior == Some((payload.clone(), deleted)) || (prior.is_none() && deleted) {
            continue;
        }
        revision = revision
            .checked_add(1)
            .ok_or_else(|| anyhow::anyhow!("Graph revision overflow"))?;
        tx.execute("INSERT INTO items VALUES(?1,?2,?3,?4) ON CONFLICT(id) DO UPDATE SET payload=excluded.payload,revision=excluded.revision,deleted=excluded.deleted",params![id,payload,revision,deleted])?;
    }
    tx.execute(
        "UPDATE meta SET value=?1 WHERE key='revision'",
        [revision.to_string()],
    )?;
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    fn projection(value: &str) -> Projection {
        [(
            "node:a".into(),
            Item {
                kind: "node".into(),
                value: json!({"label":value}),
            },
        )]
        .into()
    }
    #[test]
    fn duplicate_observation_is_idempotent_and_partial_scan_cannot_delete() {
        let temp = tempfile::tempdir().unwrap();
        let mut db = Index::open(&temp.path().join("graph.db"), "machine").unwrap();
        db.publish("source", "stamp", 1, &projection("one"), "")
            .unwrap();
        let first = db.get("revision").unwrap();
        db.publish("source", "stamp", 1, &projection("one"), "")
            .unwrap();
        assert_eq!(first, db.get("revision").unwrap());
        db.finish_scan(2, false).unwrap();
        assert_eq!(
            db.page(None, false, 200).unwrap()["items"]
                .as_array()
                .unwrap()
                .len(),
            1
        );
        db.finish_scan(3, true).unwrap();
        let changes = db.page(None, true, 200).unwrap();
        assert_eq!(changes["items"][0]["deleted"], true);
    }
    #[test]
    fn shared_identity_survives_removing_only_one_source() {
        let temp = tempfile::tempdir().unwrap();
        let mut db = Index::open(&temp.path().join("graph.db"), "machine").unwrap();
        db.publish("a", "s", 1, &projection("one"), "").unwrap();
        db.publish("b", "s", 2, &projection("one"), "").unwrap();
        db.finish_scan(2, true).unwrap();
        assert_eq!(
            db.page(None, false, 200).unwrap()["items"][0]["item"]["value"]["label"],
            "one"
        );
        db.publish("c", "s", 3, &projection("different"), "")
            .unwrap();
        assert_eq!(
            db.page(None, false, 200).unwrap()["items"][0]["item"]["kind"],
            "conflict"
        );
    }
    #[test]
    fn stable_pages_changes_and_reset_never_hide_concurrent_mutations() {
        let temp = tempfile::tempdir().unwrap();
        let mut db = Index::open(&temp.path().join("graph.db"), "machine").unwrap();
        let mut p = projection("one");
        p.insert(
            "node:b".into(),
            Item {
                kind: "node".into(),
                value: json!({}),
            },
        );
        db.publish("a", "s", 1, &p, "").unwrap();
        let first = db.page(None, false, 1).unwrap();
        assert_eq!(first["has_more"], true);
        let cursor = first["cursor"].as_str().unwrap();
        let last = db.page(Some(cursor), false, 1).unwrap();
        assert_ne!(first["items"][0]["id"], last["items"][0]["id"]);
        let change_cursor = last["cursor"].as_str().unwrap();
        db.publish("a", "s2", 1, &projection("two"), "").unwrap();
        assert_eq!(
            db.page(Some(cursor), false, 1).unwrap()["reset_required"],
            true
        );
        let changes = db.page(Some(change_cursor), true, 200).unwrap();
        assert_eq!(changes["items"].as_array().unwrap().len(), 2);
        assert!(changes["items"]
            .as_array()
            .unwrap()
            .iter()
            .any(|v| v["deleted"] == true));
        let empty = db
            .page(Some(changes["cursor"].as_str().unwrap()), true, 200)
            .unwrap();
        assert_eq!(empty["items"], json!([]));
    }
    #[test]
    fn foreign_cache_and_malformed_cursor_fail_closed() {
        let temp = tempfile::tempdir().unwrap();
        let path = temp.path().join("graph.db");
        let db = Index::open(&path, "machine").unwrap();
        assert!(Index::open(&path, "other-machine").is_err());
        assert!(db.page(Some("not-a-cursor"), true, 200).is_err());
        assert!(db.page(None, false, 201).is_err());
    }
}
