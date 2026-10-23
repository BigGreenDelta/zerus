//! Additive encrypted-transport identities. Native v1 hashes and retention
//! stay unchanged; these tombstones prevent replay even after a native read
//! receipt has expired. Normal admission uses indexed queries and quota
//! counters; upgrading an existing journal aggregates its usage once.
use super::*;

const IDENTITIES: i64 = 1_000_000;
const PAYLOAD_BYTES: i64 = 256 * 1024 * 1024;

impl Journal {
    pub fn encrypted_init(&self, now: f64) -> Result<()> {
        self.db.execute_batch("CREATE TABLE IF NOT EXISTS encrypted_requests (
            request_id TEXT PRIMARY KEY, digest TEXT NOT NULL, header TEXT NOT NULL,
            result TEXT, delivered INTEGER NOT NULL DEFAULT 0,
            created_at REAL NOT NULL, updated_at REAL NOT NULL);
            CREATE INDEX IF NOT EXISTS encrypted_pending ON encrypted_requests(delivered,request_id);
            CREATE INDEX IF NOT EXISTS encrypted_payload_retention ON encrypted_requests(updated_at,request_id) WHERE delivered=1 AND result IS NOT NULL;
            CREATE TABLE IF NOT EXISTS encrypted_snapshots (
            slot TEXT PRIMARY KEY, fingerprint TEXT NOT NULL, envelope TEXT NOT NULL);
            CREATE TABLE IF NOT EXISTS encrypted_usage (
            id INTEGER PRIMARY KEY CHECK(id=1), identities INTEGER NOT NULL,
            result_bytes INTEGER NOT NULL,snapshots INTEGER NOT NULL,snapshot_bytes INTEGER NOT NULL);")?;
        let tx = self.db.unchecked_transaction()?;
        if tx.execute(
            "INSERT OR IGNORE INTO encrypted_usage VALUES(1,0,0,0,0)",
            [],
        )? == 1
        {
            // One-time adoption only. Normal admission never scans history.
            tx.execute("UPDATE encrypted_usage SET identities=(SELECT count(*) FROM encrypted_requests),result_bytes=(SELECT coalesce(sum(length(result)),0) FROM encrypted_requests),snapshots=(SELECT count(*) FROM encrypted_snapshots),snapshot_bytes=(SELECT coalesce(sum(length(envelope)),0) FROM encrypted_snapshots) WHERE id=1",[])?;
        }
        tx.execute_batch("CREATE TRIGGER IF NOT EXISTS encrypted_request_admit AFTER INSERT ON encrypted_requests BEGIN UPDATE encrypted_usage SET identities=identities+1,result_bytes=result_bytes+coalesce(length(new.result),0) WHERE id=1; END;
            CREATE TRIGGER IF NOT EXISTS encrypted_request_result AFTER UPDATE OF result ON encrypted_requests BEGIN UPDATE encrypted_usage SET result_bytes=result_bytes+coalesce(length(new.result),0)-coalesce(length(old.result),0) WHERE id=1; END;
            CREATE TRIGGER IF NOT EXISTS encrypted_snapshot_admit AFTER INSERT ON encrypted_snapshots BEGIN UPDATE encrypted_usage SET snapshots=snapshots+1,snapshot_bytes=snapshot_bytes+length(new.envelope) WHERE id=1; END;
            CREATE TRIGGER IF NOT EXISTS encrypted_snapshot_replace AFTER UPDATE OF envelope ON encrypted_snapshots BEGIN UPDATE encrypted_usage SET snapshot_bytes=snapshot_bytes+length(new.envelope)-length(old.envelope) WHERE id=1; END;
            CREATE TRIGGER IF NOT EXISTS encrypted_snapshot_remove AFTER DELETE ON encrypted_snapshots BEGIN UPDATE encrypted_usage SET snapshots=snapshots-1,snapshot_bytes=snapshot_bytes-length(old.envelope) WHERE id=1; END;")?;
        tx.commit()?;
        self.encrypted_maintenance(now)
    }
    pub fn encrypted_maintenance(&self, now: f64) -> Result<()> {
        self.db.execute(
            "UPDATE encrypted_requests SET result=NULL WHERE request_id IN (
            SELECT request_id FROM encrypted_requests INDEXED BY encrypted_payload_retention
            WHERE delivered=1 AND updated_at<? AND result IS NOT NULL LIMIT 1000)",
            [now - MUTATION_SECONDS],
        )?;
        Ok(())
    }

    /// Commits the signed wire identity before any native admission. An
    /// existing ID never permits a second native execution, including reads.
    pub fn encrypted_reserve(
        &self,
        id: &str,
        digest: &str,
        header: &str,
        now: f64,
    ) -> Result<bool> {
        let tx = self.db.unchecked_transaction()?;
        let prior: Option<String> = tx
            .query_row(
                "SELECT digest FROM encrypted_requests WHERE request_id=?",
                [id],
                |r| r.get(0),
            )
            .optional()?;
        if let Some(prior) = prior {
            if prior != digest {
                return Err(refused(CONFLICT));
            }
            return Ok(false);
        }
        let count: i64 = tx.query_row(
            "SELECT identities FROM encrypted_usage WHERE id=1",
            [],
            |r| r.get(0),
        )?;
        if count >= IDENTITIES {
            return Err(refused(RETAINED_LIMIT));
        }
        tx.execute("INSERT INTO encrypted_requests(request_id,digest,header,created_at,updated_at) VALUES(?,?,?,?,?)",params![id,digest,header,now,now])?;
        tx.commit()?;
        Ok(true)
    }

    pub fn encrypted_pending(&self) -> Result<Vec<(String, String, Option<String>)>> {
        let mut statement=self.db.prepare("SELECT request_id,header,result FROM encrypted_requests INDEXED BY encrypted_pending WHERE delivered=0 ORDER BY request_id LIMIT 4")?;
        let rows = statement.query_map([], |r| Ok((r.get(0)?, r.get(1)?, r.get(2)?)))?;
        Ok(rows.collect::<rusqlite::Result<_>>()?)
    }

    /// Reads without changing the native journal's delivery state.
    pub fn encrypted_native_receipt(&self, id: &str) -> Result<Option<String>> {
        Ok(self
            .db
            .query_row(
                "SELECT response FROM requests WHERE request_id=?",
                [id],
                |r| r.get(0),
            )
            .optional()?
            .flatten())
    }

    pub fn encrypted_result(&self, id: &str, envelope: &str, now: f64) -> Result<()> {
        let tx = self.db.unchecked_transaction()?;
        let (prior, delivered): (Option<String>, bool) = tx.query_row(
            "SELECT result,delivered FROM encrypted_requests WHERE request_id=?",
            [id],
            |r| Ok((r.get(0)?, r.get(1)?)),
        )?;
        if let Some(prior) = prior {
            if prior != envelope {
                return Err(refused(CONFLICT));
            }
            return Ok(());
        }
        if delivered {
            return Err(refused(CONFLICT));
        }
        let bytes: i64 = tx.query_row(
            "SELECT result_bytes FROM encrypted_usage WHERE id=1",
            [],
            |r| r.get(0),
        )?;
        if bytes + envelope.len() as i64 > PAYLOAD_BYTES {
            return Err(refused(RETAINED_LIMIT));
        }
        tx.execute(
            "UPDATE encrypted_requests SET result=?,updated_at=? WHERE request_id=?",
            params![envelope, now, id],
        )?;
        tx.commit()?;
        Ok(())
    }

    pub fn encrypted_delivered(&self, id: &str, now: f64) -> Result<()> {
        let tx = self.db.unchecked_transaction()?;
        tx.execute(
            "UPDATE encrypted_requests SET delivered=1,updated_at=? WHERE request_id=?",
            params![now, id],
        )?;
        tx.execute(
            "UPDATE requests SET delivered=1,delivered_at=?,updated_at=? WHERE request_id=?",
            params![now, now, id],
        )?;
        tx.commit()?;
        Ok(())
    }

    pub fn encrypted_snapshot(&self, slot: &str, fingerprint: &str) -> Result<Option<String>> {
        Ok(self
            .db
            .query_row(
                "SELECT envelope FROM encrypted_snapshots WHERE slot=? AND fingerprint=?",
                params![slot, fingerprint],
                |r| r.get(0),
            )
            .optional()?)
    }
    pub fn encrypted_save_snapshot(
        &self,
        slot: &str,
        fingerprint: &str,
        envelope: &str,
    ) -> Result<()> {
        let tx = self.db.unchecked_transaction()?;
        let (count, bytes): (i64, i64) = tx.query_row(
            "SELECT snapshots,snapshot_bytes FROM encrypted_usage WHERE id=1",
            [],
            |r| Ok((r.get(0)?, r.get(1)?)),
        )?;
        let old: Option<i64> = tx
            .query_row(
                "SELECT length(envelope) FROM encrypted_snapshots WHERE slot=?",
                [slot],
                |r| r.get(0),
            )
            .optional()?;
        if count + if old.is_some() { 0 } else { 1 } > 4096
            || bytes - old.unwrap_or(0) + envelope.len() as i64 > 64 * 1024 * 1024
        {
            return Err(refused(RETAINED_LIMIT));
        }
        tx.execute("INSERT INTO encrypted_snapshots(slot,fingerprint,envelope) VALUES(?,?,?) ON CONFLICT(slot) DO UPDATE SET fingerprint=excluded.fingerprint,envelope=excluded.envelope",params![slot,fingerprint,envelope])?;
        tx.commit()?;
        Ok(())
    }
    pub fn encrypted_drop_snapshots(&self, valid_slots: &HashSet<String>) -> Result<()> {
        let slots: Vec<String> = self
            .db
            .prepare("SELECT slot FROM encrypted_snapshots LIMIT 4097")?
            .query_map([], |r| r.get(0))?
            .collect::<rusqlite::Result<_>>()?;
        if slots.len() > 4096 {
            return Err(refused(RETAINED_LIMIT));
        }
        let tx = self.db.unchecked_transaction()?;
        for slot in slots {
            if !valid_slots.contains(&slot) {
                tx.execute("DELETE FROM encrypted_snapshots WHERE slot=?", [slot])?;
            }
        }
        tx.commit()?;
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn transport_identity_outlives_read_receipt_and_exact_ciphertext_is_immutable() {
        let directory = tempfile::tempdir().unwrap();
        let state = directory.path().join("state");
        let journal = Journal::open(&state, "protected", 1.0).unwrap();
        journal.encrypted_init(1.0).unwrap();
        assert!(journal
            .encrypted_reserve("id", "signed-digest", "header", 1.0)
            .unwrap());
        assert!(journal
            .claim("id", "inspect", Some("native-hash"), 1.0)
            .unwrap());
        journal
            .finish(
                "id",
                &receipt("completed", json!({"private":true}), None),
                2.0,
            )
            .unwrap();
        journal.encrypted_result("id", "ciphertext", 2.0).unwrap();
        assert!(journal
            .encrypted_result("id", "another ciphertext", 2.0)
            .is_err());
        journal.encrypted_delivered("id", 2.0).unwrap();
        journal.prune(READ_SECONDS + 10.0).unwrap();
        assert!(journal.encrypted_native_receipt("id").unwrap().is_none());
        assert!(!journal
            .encrypted_reserve("id", "signed-digest", "header", READ_SECONDS + 10.0)
            .unwrap());
        assert!(journal
            .encrypted_reserve("id", "changed", "header", READ_SECONDS + 10.0)
            .is_err());
        drop(journal);
        let reopened = Journal::open(&state, "protected", MUTATION_SECONDS + 20.0).unwrap();
        reopened.encrypted_init(MUTATION_SECONDS + 20.0).unwrap();
        assert!(!reopened
            .encrypted_reserve("id", "signed-digest", "header", MUTATION_SECONDS + 20.0)
            .unwrap());
        assert!(reopened
            .encrypted_result("id", "resealed after retention", MUTATION_SECONDS + 20.0)
            .is_err());
        assert!(reopened.encrypted_pending().unwrap().is_empty());
    }
}
