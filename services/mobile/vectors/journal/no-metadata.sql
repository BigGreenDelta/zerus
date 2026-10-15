PRAGMA foreign_keys=OFF;
BEGIN TRANSACTION;
CREATE TABLE requests (
            request_id TEXT PRIMARY KEY, operation TEXT NOT NULL,
            state TEXT NOT NULL, response TEXT, delivered INTEGER NOT NULL DEFAULT 0,
            request_hash TEXT, created_at REAL NOT NULL, updated_at REAL NOT NULL,
            delivered_at REAL, response_expired INTEGER NOT NULL DEFAULT 0
        );
INSERT INTO requests VALUES('418bfa9b-81d2-5a97-b3d6-9ae94423afea','send','completed','{"state": "completed", "result": {"status": "submitted"}, "error": null}',1,'aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa',1759999940.0,1759999940.0,1759999950.0,0);
COMMIT;
