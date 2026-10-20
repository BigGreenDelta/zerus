-- Same durable model in exclusive local mode.

CREATE TABLE IF NOT EXISTS relay_schema(version integer PRIMARY KEY);
INSERT INTO relay_schema VALUES(1) ON CONFLICT DO NOTHING;
CREATE TABLE IF NOT EXISTS workspaces(id text PRIMARY KEY,name text NOT NULL);
CREATE TABLE IF NOT EXISTS nodes(id text PRIMARY KEY,workspace_id text NOT NULL REFERENCES workspaces(id),name text NOT NULL,token_hash text UNIQUE NOT NULL,revoked integer NOT NULL DEFAULT 0,last_seen double precision,snapshot text,snapshot_hash text);
CREATE INDEX IF NOT EXISTS node_workspace ON nodes(workspace_id,name,id) WHERE revoked=0;
CREATE TABLE IF NOT EXISTS devices(id text PRIMARY KEY,workspace_id text NOT NULL REFERENCES workspaces(id),name text NOT NULL,token_hash text UNIQUE NOT NULL,revoked integer NOT NULL DEFAULT 0,paired_at double precision,last_seen double precision,added_by text);
CREATE INDEX IF NOT EXISTS device_workspace ON devices(workspace_id) WHERE revoked=0;
CREATE TABLE IF NOT EXISTS invitations(code_hash text PRIMARY KEY,workspace_id text NOT NULL REFERENCES workspaces(id),expires double precision NOT NULL,added_by text);
CREATE INDEX IF NOT EXISTS invitation_expiry ON invitations(expires);
CREATE TABLE IF NOT EXISTS requests(id text PRIMARY KEY,workspace_id text NOT NULL REFERENCES workspaces(id),device_id text NOT NULL REFERENCES devices(id),node_id text NOT NULL REFERENCES nodes(id),operation text NOT NULL,body_hash text NOT NULL,body_bytes INTEGER NOT NULL,body text NOT NULL,state text NOT NULL,result text,error text,created double precision NOT NULL,claimed double precision,updated double precision NOT NULL,expires_at double precision,result_read double precision,result_bytes INTEGER NOT NULL DEFAULT 0,reserved_bytes INTEGER NOT NULL DEFAULT 0);
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
CREATE TABLE IF NOT EXISTS workspace_usage(workspace_id text PRIMARY KEY REFERENCES workspaces(id),payload_bytes INTEGER NOT NULL DEFAULT 0,active INTEGER NOT NULL DEFAULT 0,reads INTEGER NOT NULL DEFAULT 0,mutations INTEGER NOT NULL DEFAULT 0,events INTEGER NOT NULL DEFAULT 0,push_jobs INTEGER NOT NULL DEFAULT 0,active_polls INTEGER NOT NULL DEFAULT 0);
CREATE TABLE IF NOT EXISTS payload_shards(id integer PRIMARY KEY,payload_bytes INTEGER NOT NULL DEFAULT 0);
CREATE TABLE IF NOT EXISTS relay_allocation(id integer PRIMARY KEY CHECK(id=1),global_max_bytes INTEGER NOT NULL,quota_shards integer NOT NULL);
CREATE TABLE IF NOT EXISTS rate_control(id integer PRIMARY KEY,entries INTEGER NOT NULL DEFAULT 0);
INSERT INTO rate_control(id) VALUES(1),(2) ON CONFLICT DO NOTHING;
CREATE TABLE IF NOT EXISTS global_usage(id integer PRIMARY KEY CHECK(id=1),payload_bytes INTEGER NOT NULL DEFAULT 0,events INTEGER NOT NULL DEFAULT 0,push_jobs INTEGER NOT NULL DEFAULT 0);
INSERT INTO global_usage(id) VALUES(1) ON CONFLICT DO NOTHING;
CREATE TABLE IF NOT EXISTS events(id INTEGER PRIMARY KEY AUTOINCREMENT,workspace_id text NOT NULL REFERENCES workspaces(id),node_id text NOT NULL,session text NOT NULL,kind text NOT NULL,created double precision NOT NULL,alert_kind text);
CREATE INDEX IF NOT EXISTS event_workspace ON events(workspace_id,id);
CREATE INDEX IF NOT EXISTS event_expiry ON events(created,id);
CREATE TABLE IF NOT EXISTS pushes(device_id text PRIMARY KEY REFERENCES devices(id),provider text NOT NULL,target text NOT NULL);
CREATE TABLE IF NOT EXISTS push_jobs(id INTEGER PRIMARY KEY AUTOINCREMENT,device_id text NOT NULL REFERENCES devices(id),event_id INTEGER NOT NULL,payload text NOT NULL,attempts integer NOT NULL DEFAULT 0,next_at double precision NOT NULL,created double precision NOT NULL,lease_token text,lease_until double precision);
CREATE INDEX IF NOT EXISTS push_device ON push_jobs(device_id,id);
CREATE INDEX IF NOT EXISTS push_ready ON push_jobs(next_at,id) WHERE attempts<5;
CREATE INDEX IF NOT EXISTS push_expiry ON push_jobs(created,id);
CREATE TABLE IF NOT EXISTS poll_credentials(role text NOT NULL,credential_id text NOT NULL,active INTEGER NOT NULL DEFAULT 0,PRIMARY KEY(role,credential_id));
CREATE TABLE IF NOT EXISTS poll_leases(id text PRIMARY KEY,role text NOT NULL,credential_id text NOT NULL,workspace_id text NOT NULL REFERENCES workspaces(id),expires double precision NOT NULL);
CREATE INDEX IF NOT EXISTS poll_expiry ON poll_leases(expires,id);
CREATE TABLE IF NOT EXISTS rate_limits(key text PRIMARY KEY,count INTEGER NOT NULL,expires double precision NOT NULL,bucket integer NOT NULL DEFAULT 1);
CREATE INDEX IF NOT EXISTS rate_expiry ON rate_limits(expires,key);


CREATE TABLE IF NOT EXISTS computers(id TEXT PRIMARY KEY,workspace_id TEXT NOT NULL REFERENCES workspaces(id),name TEXT NOT NULL,machine_id TEXT,revoked INTEGER NOT NULL DEFAULT 0,exposed INTEGER NOT NULL DEFAULT 0);
CREATE INDEX IF NOT EXISTS computer_workspace ON computers(workspace_id,id);
CREATE TABLE IF NOT EXISTS native_computers(workspace_id TEXT NOT NULL REFERENCES workspaces(id),machine_id TEXT NOT NULL,computer_id TEXT NOT NULL REFERENCES computers(id),PRIMARY KEY(workspace_id,machine_id));
CREATE TABLE IF NOT EXISTS computer_aliases(workspace_id TEXT NOT NULL REFERENCES workspaces(id),alias TEXT NOT NULL,computer_id TEXT NOT NULL REFERENCES computers(id),PRIMARY KEY(workspace_id,alias));
CREATE INDEX IF NOT EXISTS computer_alias_target ON computer_aliases(computer_id);
CREATE TABLE IF NOT EXISTS computer_routes(gateway_id TEXT NOT NULL REFERENCES nodes(id),route_id TEXT NOT NULL,computer_id TEXT NOT NULL REFERENCES computers(id),machine_id TEXT,local INTEGER NOT NULL DEFAULT 0,active INTEGER NOT NULL DEFAULT 1,online INTEGER NOT NULL DEFAULT 0,guarded INTEGER NOT NULL DEFAULT 0,name TEXT NOT NULL,snapshot TEXT,snapshot_hash TEXT,last_seen DOUBLE PRECISION,presence_at DOUBLE PRECISION,presence_until DOUBLE PRECISION,PRIMARY KEY(gateway_id,route_id));
CREATE INDEX IF NOT EXISTS computer_route_target ON computer_routes(computer_id,gateway_id,route_id);
INSERT INTO computers(id,workspace_id,name,revoked,exposed) SELECT id,workspace_id,name,revoked,1 FROM nodes WHERE computer_id IS NULL ON CONFLICT(id) DO NOTHING;
INSERT INTO computer_aliases(workspace_id,alias,computer_id) SELECT workspace_id,id,id FROM nodes WHERE computer_id IS NULL ON CONFLICT(workspace_id,alias) DO NOTHING;
INSERT INTO computer_routes(gateway_id,route_id,computer_id,local,active,online,name) SELECT id,id,id,1,1,1,name FROM nodes WHERE computer_id IS NULL ON CONFLICT(gateway_id,route_id) DO NOTHING;
UPDATE nodes SET computer_id=id WHERE computer_id IS NULL;

-- Presence-aware phone alerts (additive; relay_schema stays 1).
CREATE TABLE IF NOT EXISTS alert_candidates(id INTEGER PRIMARY KEY AUTOINCREMENT,workspace_id TEXT NOT NULL REFERENCES workspaces(id),computer_id TEXT NOT NULL,session TEXT NOT NULL,kind TEXT NOT NULL,run_id TEXT,conversation_id TEXT,attention_signature TEXT,reply_id TEXT,created DOUBLE PRECISION NOT NULL,next_at DOUBLE PRECISION NOT NULL,UNIQUE(workspace_id,computer_id,session,kind));
CREATE INDEX IF NOT EXISTS alert_candidate_due ON alert_candidates(next_at,id);
CREATE INDEX IF NOT EXISTS alert_candidate_expiry ON alert_candidates(created,id);

CREATE INDEX IF NOT EXISTS speech_reads_history ON requests(updated,id) WHERE operation IN ('speech_profiles','speech_stats','speech_status') AND state IN ('completed','failed','uncertain');
CREATE INDEX IF NOT EXISTS speech_audio_cleanup ON requests(updated,id) WHERE operation='speech_submit' AND body!='' AND state IN ('completed','failed','uncertain');

CREATE INDEX IF NOT EXISTS speech_config_reads_history ON requests(updated,id) WHERE operation IN ('speech_config','speech_config_status') AND state IN ('completed','failed','uncertain');
CREATE INDEX IF NOT EXISTS speech_config_body_cleanup ON requests(updated,id) WHERE operation IN ('speech_profile_upsert','speech_profile_remove','speech_profile_test') AND body!='' AND state IN ('completed','failed','uncertain');

CREATE INDEX IF NOT EXISTS file_history ON requests(updated,id) WHERE operation='file_read' AND state IN ('completed','failed','uncertain');

CREATE INDEX IF NOT EXISTS archive_history ON requests(updated,id) WHERE operation='archive_list' AND state IN ('completed','failed','uncertain');

-- Content-free hook diagnostics expire independently of native mutations.
CREATE INDEX IF NOT EXISTS hook_health_history ON requests(updated,id) WHERE operation='hooks_status' AND state IN ('completed','failed','uncertain');

CREATE INDEX IF NOT EXISTS analytics_reads_history ON requests(updated,id) WHERE operation='analytics_read' AND state IN ('completed','failed','uncertain');

CREATE INDEX IF NOT EXISTS git_status_history_v1 ON requests(updated,id) WHERE operation='git_status' AND state IN ('completed','failed','uncertain');

-- Permanent shared-notification highwater, independent of gateway route lifetime.
CREATE TABLE IF NOT EXISTS computer_notification_preferences(computer_id TEXT PRIMARY KEY REFERENCES computers(id),workspace_id TEXT NOT NULL REFERENCES workspaces(id),preferences TEXT NOT NULL,payload_bytes BIGINT NOT NULL);
CREATE INDEX IF NOT EXISTS computer_notification_workspace ON computer_notification_preferences(workspace_id,computer_id);
