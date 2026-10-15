PRAGMA foreign_keys=OFF;
BEGIN TRANSACTION;
CREATE TABLE metadata (key TEXT PRIMARY KEY, value TEXT NOT NULL);
INSERT INTO metadata VALUES('relay_binding','12d3fb51d36511f8d27ffa541bd2a4a59e748076a354a075afbe071eb2c5f46c');
CREATE TABLE requests (
            request_id TEXT PRIMARY KEY, operation TEXT NOT NULL,
            state TEXT NOT NULL, response TEXT, delivered INTEGER NOT NULL DEFAULT 0,
            request_hash TEXT, created_at REAL NOT NULL, updated_at REAL NOT NULL,
            delivered_at REAL, response_expired INTEGER NOT NULL DEFAULT 0
        );
WITH RECURSIVE n(i) AS (SELECT 1 UNION ALL SELECT i+1 FROM n WHERE i<5000)
INSERT INTO requests SELECT printf('00000000-0000-4000-8000-%012d',i),'inspect','completed','{"state": "completed", "result": {}, "error": null}',1,NULL,1759999990.0,1759999990.0,1759999990.0,0 FROM n;
WITH RECURSIVE n(i) AS (SELECT 1 UNION ALL SELECT i+1 FROM n WHERE i<99999)
INSERT INTO requests SELECT printf('00000000-0000-4000-9000-%012d',i),'send','completed','{"state": "completed", "result": {}, "error": null}',1,NULL,1759999990.0,1759999990.0,1759999990.0,0 FROM n;
COMMIT;
