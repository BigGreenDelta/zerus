PRAGMA foreign_keys=OFF;
BEGIN TRANSACTION;
CREATE TABLE metadata (key TEXT PRIMARY KEY, value TEXT NOT NULL);
INSERT INTO metadata VALUES('relay_binding','27137532e008bf333babcb177122658e7f25e6453a2b8d0f15940a4f699d9a33');
CREATE TABLE requests (
            request_id TEXT PRIMARY KEY, operation TEXT NOT NULL,
            state TEXT NOT NULL, response TEXT, delivered INTEGER NOT NULL DEFAULT 0,
            request_hash TEXT, created_at REAL NOT NULL, updated_at REAL NOT NULL,
            delivered_at REAL, response_expired INTEGER NOT NULL DEFAULT 0
        );
INSERT INTO requests VALUES('c04e1cc8-7a23-5058-b3a6-10359df6153f','send','running',NULL,0,'aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa',1759999940.0,1759999940.0,NULL,0);
COMMIT;
