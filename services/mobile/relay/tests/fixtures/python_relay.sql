-- A PostgreSQL database as the removed Python relay wrote it, for
-- tests/python_receipts.rs. Frozen: its writer no longer exists.
--
-- Written by zerus_mobile.postgres.PostgresStore at 5df9bd9f, the parent of
-- 9a69d936 that removed it, on postgres:17.11-alpine, with sequential UUIDs
-- and fixed synthetic secrets: workspace "Example", node "Example computer",
-- invite and pair "Example phone", then submit, claim and result of one
-- answer request (1e-5, 2**80, a non-BMP character and the object key
-- "$serde_json::private::Number"). body_hash, body, result and the payload
-- accounting are Python's. The schema is that commit's SCHEMA text, verbatim;
-- the rows are its pg_dump --data-only --column-inserts, without the rows the
-- schema text itself inserts and with the 64 payload_shards rows folded into
-- one statement. The node token is synthetic-legacy-node-token and the device
-- token synthetic-legacy-device-token; the pair code was used.

-- Schema
CREATE TABLE IF NOT EXISTS relay_schema(version integer PRIMARY KEY);
INSERT INTO relay_schema VALUES(1) ON CONFLICT DO NOTHING;
CREATE TABLE IF NOT EXISTS workspaces(id text PRIMARY KEY,name text NOT NULL);
CREATE TABLE IF NOT EXISTS nodes(id text PRIMARY KEY,workspace_id text NOT NULL REFERENCES workspaces(id),name text NOT NULL,token_hash text UNIQUE NOT NULL,revoked integer NOT NULL DEFAULT 0,last_seen double precision,snapshot text,snapshot_hash text);
ALTER TABLE nodes ADD COLUMN IF NOT EXISTS snapshot_hash text;
CREATE INDEX IF NOT EXISTS node_workspace ON nodes(workspace_id,name,id) WHERE revoked=0;
CREATE TABLE IF NOT EXISTS devices(id text PRIMARY KEY,workspace_id text NOT NULL REFERENCES workspaces(id),name text NOT NULL,token_hash text UNIQUE NOT NULL,revoked integer NOT NULL DEFAULT 0);
CREATE INDEX IF NOT EXISTS device_workspace ON devices(workspace_id) WHERE revoked=0;
CREATE TABLE IF NOT EXISTS invitations(code_hash text PRIMARY KEY,workspace_id text NOT NULL REFERENCES workspaces(id),expires double precision NOT NULL);
CREATE INDEX IF NOT EXISTS invitation_expiry ON invitations(expires);
CREATE TABLE IF NOT EXISTS requests(id text PRIMARY KEY,workspace_id text NOT NULL REFERENCES workspaces(id),device_id text NOT NULL REFERENCES devices(id),node_id text NOT NULL REFERENCES nodes(id),operation text NOT NULL,body_hash text NOT NULL,body_bytes bigint NOT NULL,body text NOT NULL,state text NOT NULL,result text,error text,created double precision NOT NULL,claimed double precision,updated double precision NOT NULL,expires_at double precision,result_read double precision,result_bytes bigint NOT NULL DEFAULT 0,reserved_bytes bigint NOT NULL DEFAULT 0);
DROP INDEX IF EXISTS read_history_v2;
CREATE INDEX IF NOT EXISTS request_queue ON requests(node_id,created,id) WHERE state='queued';
CREATE INDEX IF NOT EXISTS request_queue_expiry ON requests(created,id) WHERE state='queued';
CREATE INDEX IF NOT EXISTS request_claim_expiry ON requests(claimed,id) WHERE state='claimed';
CREATE INDEX IF NOT EXISTS terminal_expiry ON requests(expires_at,id) WHERE state='queued' AND expires_at IS NOT NULL;
CREATE INDEX IF NOT EXISTS terminal_history ON requests(updated,id) WHERE operation='terminal_snapshot' AND state IN ('completed','failed','uncertain');
CREATE INDEX IF NOT EXISTS attachment_history ON requests(updated,id) WHERE operation='attachment_read' AND state IN ('completed','failed','uncertain');
CREATE INDEX IF NOT EXISTS request_history ON requests(updated,id) WHERE body!='' AND state IN ('completed','failed','uncertain');
CREATE INDEX IF NOT EXISTS read_history_v3 ON requests(updated,id) WHERE operation IN ('inspect','history','process_output','terminal_snapshot','catalog','dirs','worktrees','mark_read') AND state IN ('completed','failed','uncertain');
CREATE INDEX IF NOT EXISTS request_workspace ON requests(workspace_id,state);
CREATE TABLE IF NOT EXISTS workspace_usage(workspace_id text PRIMARY KEY REFERENCES workspaces(id),payload_bytes bigint NOT NULL DEFAULT 0,active bigint NOT NULL DEFAULT 0,reads bigint NOT NULL DEFAULT 0,mutations bigint NOT NULL DEFAULT 0,events bigint NOT NULL DEFAULT 0,push_jobs bigint NOT NULL DEFAULT 0,active_polls bigint NOT NULL DEFAULT 0);
CREATE TABLE IF NOT EXISTS payload_shards(id integer PRIMARY KEY,payload_bytes bigint NOT NULL DEFAULT 0);
CREATE TABLE IF NOT EXISTS relay_allocation(id integer PRIMARY KEY CHECK(id=1),global_max_bytes bigint NOT NULL,quota_shards integer NOT NULL);
CREATE TABLE IF NOT EXISTS rate_control(id integer PRIMARY KEY,entries bigint NOT NULL DEFAULT 0);
ALTER TABLE rate_control DROP CONSTRAINT IF EXISTS rate_control_id_check;
INSERT INTO rate_control(id) VALUES(1),(2) ON CONFLICT DO NOTHING;
CREATE TABLE IF NOT EXISTS global_usage(id integer PRIMARY KEY CHECK(id=1),payload_bytes bigint NOT NULL DEFAULT 0,events bigint NOT NULL DEFAULT 0,push_jobs bigint NOT NULL DEFAULT 0);
INSERT INTO global_usage(id) VALUES(1) ON CONFLICT DO NOTHING;
CREATE TABLE IF NOT EXISTS events(id bigint GENERATED BY DEFAULT AS IDENTITY PRIMARY KEY,workspace_id text NOT NULL REFERENCES workspaces(id),node_id text NOT NULL,session text NOT NULL,kind text NOT NULL,created double precision NOT NULL);
CREATE INDEX IF NOT EXISTS event_workspace ON events(workspace_id,id);
CREATE INDEX IF NOT EXISTS event_expiry ON events(created,id);
CREATE TABLE IF NOT EXISTS pushes(device_id text PRIMARY KEY REFERENCES devices(id),provider text NOT NULL,target text NOT NULL);
CREATE TABLE IF NOT EXISTS push_jobs(id bigint GENERATED BY DEFAULT AS IDENTITY PRIMARY KEY,device_id text NOT NULL REFERENCES devices(id),event_id bigint NOT NULL,payload text NOT NULL,attempts integer NOT NULL DEFAULT 0,next_at double precision NOT NULL,created double precision NOT NULL,lease_token text,lease_until double precision);
CREATE INDEX IF NOT EXISTS push_device ON push_jobs(device_id,id);
CREATE INDEX IF NOT EXISTS push_ready ON push_jobs(next_at,id) WHERE attempts<5;
CREATE INDEX IF NOT EXISTS push_expiry ON push_jobs(created,id);
CREATE TABLE IF NOT EXISTS poll_credentials(role text NOT NULL,credential_id text NOT NULL,active bigint NOT NULL DEFAULT 0,PRIMARY KEY(role,credential_id));
CREATE TABLE IF NOT EXISTS poll_leases(id text PRIMARY KEY,role text NOT NULL,credential_id text NOT NULL,workspace_id text NOT NULL REFERENCES workspaces(id),expires double precision NOT NULL);
CREATE INDEX IF NOT EXISTS poll_expiry ON poll_leases(expires,id);
CREATE TABLE IF NOT EXISTS rate_limits(key text PRIMARY KEY,count bigint NOT NULL,expires double precision NOT NULL,bucket integer NOT NULL DEFAULT 1);
ALTER TABLE rate_limits ADD COLUMN IF NOT EXISTS bucket integer NOT NULL DEFAULT 1;
CREATE INDEX IF NOT EXISTS rate_expiry ON rate_limits(expires,key);

ALTER TABLE nodes ADD COLUMN IF NOT EXISTS computer_id TEXT;
ALTER TABLE nodes ADD COLUMN IF NOT EXISTS machine_id TEXT;
ALTER TABLE nodes ADD COLUMN IF NOT EXISTS manifest_hash TEXT;
ALTER TABLE requests ADD COLUMN IF NOT EXISTS target_computer_id TEXT;
ALTER TABLE requests ADD COLUMN IF NOT EXISTS target_machine_id TEXT;
ALTER TABLE requests ADD COLUMN IF NOT EXISTS route_id TEXT;

CREATE TABLE IF NOT EXISTS computers(id TEXT PRIMARY KEY,workspace_id TEXT NOT NULL REFERENCES workspaces(id),name TEXT NOT NULL,machine_id TEXT,revoked INTEGER NOT NULL DEFAULT 0,exposed INTEGER NOT NULL DEFAULT 0);
CREATE INDEX IF NOT EXISTS computer_workspace ON computers(workspace_id,id);
CREATE TABLE IF NOT EXISTS native_computers(workspace_id TEXT NOT NULL REFERENCES workspaces(id),machine_id TEXT NOT NULL,computer_id TEXT NOT NULL REFERENCES computers(id),PRIMARY KEY(workspace_id,machine_id));
CREATE TABLE IF NOT EXISTS computer_aliases(workspace_id TEXT NOT NULL REFERENCES workspaces(id),alias TEXT NOT NULL,computer_id TEXT NOT NULL REFERENCES computers(id),PRIMARY KEY(workspace_id,alias));
CREATE INDEX IF NOT EXISTS computer_alias_target ON computer_aliases(computer_id);
CREATE TABLE IF NOT EXISTS computer_routes(gateway_id TEXT NOT NULL REFERENCES nodes(id),route_id TEXT NOT NULL,computer_id TEXT NOT NULL REFERENCES computers(id),machine_id TEXT,local INTEGER NOT NULL DEFAULT 0,active INTEGER NOT NULL DEFAULT 1,online INTEGER NOT NULL DEFAULT 0,guarded INTEGER NOT NULL DEFAULT 0,name TEXT NOT NULL,snapshot TEXT,snapshot_hash TEXT,last_seen DOUBLE PRECISION,PRIMARY KEY(gateway_id,route_id));
CREATE INDEX IF NOT EXISTS computer_route_target ON computer_routes(computer_id,gateway_id,route_id);
INSERT INTO computers(id,workspace_id,name,revoked,exposed) SELECT id,workspace_id,name,revoked,1 FROM nodes WHERE computer_id IS NULL ON CONFLICT(id) DO NOTHING;
INSERT INTO computer_aliases(workspace_id,alias,computer_id) SELECT workspace_id,id,id FROM nodes WHERE computer_id IS NULL ON CONFLICT(workspace_id,alias) DO NOTHING;
INSERT INTO computer_routes(gateway_id,route_id,computer_id,local,active,online,name) SELECT id,id,id,1,1,1,name FROM nodes WHERE computer_id IS NULL ON CONFLICT(gateway_id,route_id) DO NOTHING;
UPDATE nodes SET computer_id=id WHERE computer_id IS NULL;

-- Rows
INSERT INTO workspaces (id, name) VALUES ('00000000-0000-4000-8000-000000000001', 'Example');
INSERT INTO computers (id, workspace_id, name, machine_id, revoked, exposed) VALUES ('00000000-0000-4000-8000-000000000002', '00000000-0000-4000-8000-000000000001', 'Example computer', NULL, 0, 0);
INSERT INTO computer_aliases (workspace_id, alias, computer_id) VALUES ('00000000-0000-4000-8000-000000000001', '00000000-0000-4000-8000-000000000002', '00000000-0000-4000-8000-000000000002');
INSERT INTO nodes (id, workspace_id, name, token_hash, revoked, last_seen, snapshot, snapshot_hash, computer_id, machine_id, manifest_hash) VALUES ('00000000-0000-4000-8000-000000000002', '00000000-0000-4000-8000-000000000001', 'Example computer', 'aa58df2c11537161c3b2b3b8739b685e6a21b811ef900329e7249931e2041765', 0, NULL, NULL, NULL, '00000000-0000-4000-8000-000000000002', NULL, NULL);
INSERT INTO computer_routes (gateway_id, route_id, computer_id, machine_id, local, active, online, guarded, name, snapshot, snapshot_hash, last_seen) VALUES ('00000000-0000-4000-8000-000000000002', '00000000-0000-4000-8000-000000000002', '00000000-0000-4000-8000-000000000002', NULL, 1, 1, 1, 0, 'Example computer', NULL, NULL, NULL);
INSERT INTO devices (id, workspace_id, name, token_hash, revoked) VALUES ('00000000-0000-4000-8000-000000000003', '00000000-0000-4000-8000-000000000001', 'Example phone', '291221f640d95a3b13a09c994c7c6b8cef0deac9176eeb3363a6ed2d86b84e11', 0);
INSERT INTO relay_allocation (id, global_max_bytes, quota_shards) VALUES (1, 34359738368, 64);
INSERT INTO payload_shards (id, payload_bytes) SELECT id, CASE id WHEN 16 THEN 572 ELSE 0 END FROM generate_series(0, 63) AS id;
INSERT INTO requests (id, workspace_id, device_id, node_id, operation, body_hash, body_bytes, body, state, result, error, created, claimed, updated, expires_at, result_read, result_bytes, reserved_bytes, target_computer_id, target_machine_id, route_id) VALUES ('00000000-0000-4000-8000-000000000004', '00000000-0000-4000-8000-000000000001', '00000000-0000-4000-8000-000000000003', '00000000-0000-4000-8000-000000000002', 'answer', '73c395492504da58af5621447210f5863c1b71ed3bdd9dc174594ad9aa20eef0', 467, '{"computer_id":"00000000-0000-4000-8000-000000000002","operation":"answer","payload":{"answers":{"float":1e-05,"large":1208925819614629174706176,"object":{"$serde_json::private::Number":"123"},"text":"Unicode \ud83d\ude00"},"expected_conversation_id":"conversation","expected_question_hash":"hash","expected_run_id":"run","question_id":"q","request_id":"00000000-0000-4000-8000-000000000004"},"request_id":"00000000-0000-4000-8000-000000000004","session":"synthetic"}', 'completed', '{"object":{"$serde_json::private::Number":"123"},"text":"\ud83d\ude00","value":1208925819614629174706176}', NULL, 1792089257.270369, 1792089257.2755435, 1792089257.278906, NULL, NULL, 105, 0, '00000000-0000-4000-8000-000000000002', NULL, NULL);
INSERT INTO workspace_usage (workspace_id, payload_bytes, active, reads, mutations, events, push_jobs, active_polls) VALUES ('00000000-0000-4000-8000-000000000001', 572, 0, 0, 1, 0, 0, 0);
