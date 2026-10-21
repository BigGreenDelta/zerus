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
CREATE INDEX delivered_history ON requests(delivered,delivered_at);
CREATE INDEX project_assignment_delivered_v1 ON requests(delivered_at) WHERE operation='project_assignment' AND delivered=1 AND state!='running';
CREATE INDEX analytics_delivered_reads ON requests(delivered_at) WHERE operation='analytics_read' AND delivered=1 AND state!='running';
CREATE INDEX terminal_delivered_reads ON requests(delivered_at) WHERE operation='terminal_snapshot' AND delivered=1 AND state!='running';
CREATE INDEX archive_delivered_reads ON requests(delivered_at) WHERE operation='archive_list' AND delivered=1 AND state!='running';
CREATE INDEX file_delivered_reads ON requests(delivered_at) WHERE operation='file_read' AND delivered=1 AND state!='running';
CREATE INDEX attachment_delivered_reads ON requests(delivered_at) WHERE operation='attachment_read' AND delivered=1 AND state!='running';
CREATE INDEX journal_operation ON requests(operation);
CREATE INDEX delivered_reads ON requests(delivered_at) WHERE operation='inspect' AND delivered=1 AND state!='running';
CREATE INDEX mutation_retention ON requests(delivered_at) WHERE operation!='inspect' AND delivered=1 AND state!='running' AND response IS NOT NULL AND response_expired=0;
COMMIT;
