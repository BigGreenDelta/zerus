"""SQLite state. No network call or await happens inside a transaction."""
from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import secrets
import sqlite3
import stat
import time
import threading
import functools
import uuid

from .attachments import MAX_QUEUE_BYTES
from . import speech, speech_config, voice, session_notifications, notification_highwater
from .context import CLEAR_CONTEXT_DISMISS_FEATURE
from .context import CAPABILITY_OPERATIONS, ATTACHMENT_READ_GRACE, ATTACHMENT_RESULT_TTL, READ_OPERATIONS, READ_CLASS_SQL, READ_SQL, inspect_features, supports
from .projects import is_archived
from .routes import Registry, RouteError, drive_sync, SCHEMA as ROUTE_SCHEMA, MAX_PEER_SNAPSHOT_BYTES


def token() -> str:
    return secrets.token_urlsafe(32)


def digest(value: str) -> str:
    return hashlib.sha256(value.encode()).hexdigest()


def canonical(value) -> str:
    return json.dumps(value, sort_keys=True, separators=(",", ":"), allow_nan=False)


def serialized(method):
    @functools.wraps(method)
    def guarded(self, *args, **kwargs):
        with self._lock:
            return method(self, *args, **kwargs)
    return guarded


class Store:
    def __init__(self, path: str | Path):
        self._lock = threading.RLock()
        path = Path(path)
        path.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
        # Do not chmod a caller's home, cwd or shared directory.
        parent_info = path.parent.stat()
        if parent_info.st_uid != os.geteuid() or stat.S_IMODE(parent_info.st_mode) & 0o077:
            raise ValueError("database requires a dedicated private directory (mode 0700)")
        fd = os.open(path, os.O_CREAT | os.O_RDWR | os.O_NOFOLLOW, 0o600)
        try:
            info = os.fstat(fd)
            if not stat.S_ISREG(info.st_mode) or info.st_uid != os.geteuid():
                raise ValueError("database must be a regular file owned by this user")
            os.fchmod(fd, 0o600)
        finally:
            os.close(fd)
        # Prepared statements can retain their last maximum-size body binding.
        # This development backend favors a bounded native lifetime over SQL
        # parse caching; the compatibility worker serializes the connection.
        self.db = sqlite3.connect(path, timeout=5, check_same_thread=False, cached_statements=0)
        self.db.row_factory = sqlite3.Row
        self.db.execute("PRAGMA foreign_keys=ON")
        self.db.execute("PRAGMA journal_mode=DELETE")
        self.db.execute("PRAGMA synchronous=FULL")
        self.db.execute("PRAGMA secure_delete=ON")
        self.db.executescript("""
        CREATE TABLE IF NOT EXISTS workspaces(id TEXT PRIMARY KEY,name TEXT NOT NULL);
        CREATE TABLE IF NOT EXISTS nodes(id TEXT PRIMARY KEY,workspace_id TEXT NOT NULL REFERENCES workspaces(id),name TEXT NOT NULL,token_hash TEXT UNIQUE NOT NULL,revoked INTEGER NOT NULL DEFAULT 0,last_seen REAL,snapshot TEXT);
        CREATE TABLE IF NOT EXISTS devices(id TEXT PRIMARY KEY,workspace_id TEXT NOT NULL REFERENCES workspaces(id),name TEXT NOT NULL,token_hash TEXT UNIQUE NOT NULL,revoked INTEGER NOT NULL DEFAULT 0);
        CREATE TABLE IF NOT EXISTS invitations(code_hash TEXT PRIMARY KEY,workspace_id TEXT NOT NULL REFERENCES workspaces(id),expires REAL NOT NULL);
        CREATE TABLE IF NOT EXISTS requests(id TEXT PRIMARY KEY,workspace_id TEXT NOT NULL,device_id TEXT NOT NULL REFERENCES devices(id),node_id TEXT NOT NULL REFERENCES nodes(id),operation TEXT NOT NULL,body_hash TEXT NOT NULL,body_bytes INTEGER NOT NULL,body TEXT NOT NULL,state TEXT NOT NULL,result TEXT,error TEXT,created REAL NOT NULL,claimed REAL,updated REAL NOT NULL);
        CREATE INDEX IF NOT EXISTS request_queue ON requests(node_id,state,created);
        CREATE INDEX IF NOT EXISTS request_expiry ON requests(state,created,claimed);
        CREATE INDEX IF NOT EXISTS request_history ON requests(updated) WHERE body!='' AND state IN ('completed','failed','uncertain');
        CREATE INDEX IF NOT EXISTS hook_health_history ON requests(updated,id) WHERE operation='hooks_status' AND state IN ('completed','failed','uncertain');
        CREATE TABLE IF NOT EXISTS events(id INTEGER PRIMARY KEY AUTOINCREMENT,workspace_id TEXT NOT NULL,node_id TEXT NOT NULL,session TEXT NOT NULL,kind TEXT NOT NULL,created REAL NOT NULL);
        CREATE INDEX IF NOT EXISTS event_workspace ON events(workspace_id,id);
        CREATE TABLE IF NOT EXISTS pushes(device_id TEXT PRIMARY KEY REFERENCES devices(id),provider TEXT NOT NULL,target TEXT NOT NULL);
        CREATE TABLE IF NOT EXISTS push_jobs(id INTEGER PRIMARY KEY AUTOINCREMENT,device_id TEXT NOT NULL REFERENCES devices(id),event_id INTEGER NOT NULL,payload TEXT NOT NULL,attempts INTEGER NOT NULL DEFAULT 0,next_at REAL NOT NULL,created REAL NOT NULL);
        """)
        if "operation" not in {r[1] for r in self.db.execute("PRAGMA table_info(requests)")}:
            with self.db:
                self.db.execute("ALTER TABLE requests ADD COLUMN operation TEXT NOT NULL DEFAULT 'unknown'")
                self.db.execute("UPDATE requests SET operation=json_extract(body,'$.operation') WHERE body!=''")
        if "body_hash" not in {r[1] for r in self.db.execute("PRAGMA table_info(requests)")}:
            with self.db:
                self.db.execute("ALTER TABLE requests ADD COLUMN body_hash TEXT NOT NULL DEFAULT ''")
                for row in self.db.execute("SELECT id,body FROM requests WHERE body!=''").fetchall():
                    self.db.execute("UPDATE requests SET body_hash=? WHERE id=?", (digest(row["body"]), row["id"]))
        if "body_bytes" not in {r[1] for r in self.db.execute("PRAGMA table_info(requests)")}:
            with self.db:
                self.db.execute("ALTER TABLE requests ADD COLUMN body_bytes INTEGER NOT NULL DEFAULT 0")
                self.db.execute("UPDATE requests SET body_bytes=length(CAST(body AS BLOB))")
        columns={r[1] for r in self.db.execute("PRAGMA table_info(requests)")}
        with self.db:
            for column in ("expires_at","result_read"):
                if column not in columns:
                    self.db.execute(f"ALTER TABLE requests ADD COLUMN {column} REAL")
        if "snapshot_hash" not in {r[1] for r in self.db.execute("PRAGMA table_info(nodes)")}:
            self.db.execute("ALTER TABLE nodes ADD COLUMN snapshot_hash TEXT")
        self.db.executescript("""
        CREATE TABLE IF NOT EXISTS relay_usage(id INTEGER PRIMARY KEY,event_count INTEGER NOT NULL DEFAULT 0,push_count INTEGER NOT NULL DEFAULT 0);
        INSERT OR IGNORE INTO relay_usage(id) VALUES(1);
        UPDATE relay_usage SET event_count=(SELECT count(*) FROM events),push_count=(SELECT count(*) FROM push_jobs) WHERE id=1;
        CREATE TRIGGER IF NOT EXISTS event_count_insert AFTER INSERT ON events BEGIN UPDATE relay_usage SET event_count=event_count+1 WHERE id=1; END;
        CREATE TRIGGER IF NOT EXISTS event_count_delete AFTER DELETE ON events BEGIN UPDATE relay_usage SET event_count=event_count-1 WHERE id=1; END;
        CREATE TRIGGER IF NOT EXISTS push_count_insert AFTER INSERT ON push_jobs BEGIN UPDATE relay_usage SET push_count=push_count+1 WHERE id=1; END;
        CREATE TRIGGER IF NOT EXISTS push_count_delete AFTER DELETE ON push_jobs BEGIN UPDATE relay_usage SET push_count=push_count-1 WHERE id=1; END;
        CREATE INDEX IF NOT EXISTS push_device ON push_jobs(device_id,id);
        """)
        self.db.execute("DROP INDEX IF EXISTS read_history_v2")
        self.db.execute("CREATE INDEX IF NOT EXISTS terminal_history_v2 ON requests(updated) WHERE operation='terminal_snapshot' AND state IN ('completed','failed','uncertain')")
        self.db.execute("CREATE INDEX IF NOT EXISTS archive_history ON requests(updated,id) WHERE operation='archive_list' AND state IN ('completed','failed','uncertain')")
        self.db.execute("CREATE INDEX IF NOT EXISTS history_projection_reads_v1 ON requests(updated,id) WHERE operation='history_projection' AND state IN ('completed','failed','uncertain')")
        self.db.execute("CREATE INDEX IF NOT EXISTS file_history ON requests(updated,id) WHERE operation='file_read' AND state IN ('completed','failed','uncertain')")
        self.db.execute("CREATE INDEX IF NOT EXISTS attachment_history ON requests(updated,id) WHERE operation='attachment_read' AND state IN ('completed','failed','uncertain')")
        self.db.execute("CREATE INDEX IF NOT EXISTS terminal_expiry ON requests(expires_at) WHERE state='queued' AND expires_at IS NOT NULL")
        self.db.execute("CREATE INDEX IF NOT EXISTS request_workspace ON requests(workspace_id,operation,state)")
        self.db.execute("CREATE INDEX IF NOT EXISTS event_expiry ON events(created)")
        self.db.execute("CREATE INDEX IF NOT EXISTS push_expiry ON push_jobs(created)")
        self.db.execute("CREATE INDEX IF NOT EXISTS invitation_expiry ON invitations(expires)")
        self.db.execute("CREATE INDEX IF NOT EXISTS analytics_reads_history ON requests(updated,id) WHERE operation='analytics_read' AND state IN ('completed','failed','uncertain')")
        self.db.execute("CREATE INDEX IF NOT EXISTS project_assignment_history_v1 ON requests(updated,id) WHERE operation='project_assignment' AND state IN ('completed','failed','uncertain')")
        self.db.execute(f"CREATE INDEX IF NOT EXISTS read_history_v3 ON requests(updated) WHERE operation IN {READ_SQL} AND state IN ('completed','failed','uncertain')")
        request_columns = {r[1] for r in self.db.execute("PRAGMA table_info(requests)")}
        with self.db:
            if "reserved_bytes" not in request_columns:
                self.db.execute("ALTER TABLE requests ADD COLUMN reserved_bytes INTEGER NOT NULL DEFAULT 0")
                self.db.execute("UPDATE requests SET reserved_bytes=1048576 WHERE state IN ('queued','claimed')")
            if "result_bytes" not in request_columns:
                self.db.execute("ALTER TABLE requests ADD COLUMN result_bytes INTEGER NOT NULL DEFAULT 0")
                self.db.execute("UPDATE requests SET result_bytes=COALESCE(length(CAST(result AS BLOB)),0)+COALESCE(length(CAST(error AS BLOB)),0)")
            self.db.executescript(f"""
            CREATE TABLE IF NOT EXISTS workspace_usage(workspace_id TEXT PRIMARY KEY,payload_bytes INTEGER NOT NULL DEFAULT 0,active INTEGER NOT NULL DEFAULT 0,reads INTEGER NOT NULL DEFAULT 0,mutations INTEGER NOT NULL DEFAULT 0,budget_bytes INTEGER NOT NULL DEFAULT {MAX_QUEUE_BYTES});
            INSERT OR IGNORE INTO workspace_usage(workspace_id) SELECT id FROM workspaces;
            UPDATE workspace_usage SET payload_bytes=(SELECT COALESCE(sum(body_bytes+result_bytes+reserved_bytes),0) FROM requests WHERE workspace_id=workspace_usage.workspace_id),active=(SELECT count(*) FROM requests WHERE workspace_id=workspace_usage.workspace_id AND state IN ('queued','claimed')),reads=(SELECT count(*) FROM requests WHERE workspace_id=workspace_usage.workspace_id AND operation IN {READ_CLASS_SQL}),mutations=(SELECT count(*) FROM requests WHERE workspace_id=workspace_usage.workspace_id AND operation NOT IN {READ_CLASS_SQL});
            CREATE TRIGGER IF NOT EXISTS workspace_usage_create AFTER INSERT ON workspaces BEGIN INSERT INTO workspace_usage(workspace_id) VALUES(NEW.id); END;
            DROP TRIGGER IF EXISTS request_usage_insert;
            DROP TRIGGER IF EXISTS request_usage_delete;
            DROP TRIGGER IF EXISTS request_usage_update;
            DROP TRIGGER IF EXISTS request_result_accounting;
            CREATE TRIGGER request_usage_insert AFTER INSERT ON requests BEGIN
                UPDATE workspace_usage SET payload_bytes=payload_bytes+NEW.body_bytes+NEW.result_bytes+NEW.reserved_bytes,active=active+(NEW.state IN ('queued','claimed')),reads=reads+(NEW.operation IN {READ_CLASS_SQL}),mutations=mutations+(NEW.operation NOT IN {READ_CLASS_SQL}) WHERE workspace_id=NEW.workspace_id;
            END;
            CREATE TRIGGER request_usage_delete AFTER DELETE ON requests BEGIN
                UPDATE workspace_usage SET payload_bytes=payload_bytes-OLD.body_bytes-OLD.result_bytes-OLD.reserved_bytes,active=active-(OLD.state IN ('queued','claimed')),reads=reads-(OLD.operation IN {READ_CLASS_SQL}),mutations=mutations-(OLD.operation NOT IN {READ_CLASS_SQL}) WHERE workspace_id=OLD.workspace_id;
            END;
            CREATE TRIGGER request_usage_update AFTER UPDATE OF body_bytes,result_bytes,reserved_bytes,state ON requests BEGIN
                UPDATE workspace_usage SET payload_bytes=payload_bytes+NEW.body_bytes+NEW.result_bytes+NEW.reserved_bytes-OLD.body_bytes-OLD.result_bytes-OLD.reserved_bytes,active=active+(NEW.state IN ('queued','claimed'))-(OLD.state IN ('queued','claimed')) WHERE workspace_id=NEW.workspace_id;
            END;
            """)

        for table, columns in (("nodes", ("computer_id", "machine_id", "manifest_hash")), ("requests", ("target_computer_id", "target_machine_id", "route_id"))):
            existing = {r[1] for r in self.db.execute(f"PRAGMA table_info({table})")}
            for column in columns:
                if column not in existing:
                    self.db.execute(f"ALTER TABLE {table} ADD COLUMN {column} TEXT")
        self.db.executescript(ROUTE_SCHEMA)
        with self.db:
            self.db.execute("UPDATE workspace_usage SET payload_bytes=(SELECT COALESCE(sum(body_bytes+result_bytes+reserved_bytes),0) FROM requests WHERE workspace_id=workspace_usage.workspace_id)+(SELECT COALESCE(sum(payload_bytes),0) FROM computer_notification_preferences WHERE workspace_id=workspace_usage.workspace_id)")
        self.registry = Registry()


    @serialized
    def close(self):
        self.db.close()

    @serialized
    def workspace(self, name: str) -> str:
        value = str(uuid.uuid4())
        with self.db:
            self.db.execute("INSERT INTO workspaces VALUES(?,?)", (value, name))
        return value

    @serialized
    def node(self, workspace: str, name: str) -> dict:
        value, secret = str(uuid.uuid4()), token()
        with self.db:
            self.db.execute("INSERT INTO nodes(id,workspace_id,name,token_hash) VALUES(?,?,?,?)", (value, workspace, name, digest(secret)))
            drive_sync(self.db, self.registry.enroll({"id": value, "workspace_id": workspace, "name": name}))
        return {"node_id": value, "node_token": secret}

    @serialized
    def invite(self, workspace: str) -> dict:
        code, expires = token(), time.time() + 600
        with self.db:
            self.db.execute("DELETE FROM invitations WHERE code_hash IN (SELECT code_hash FROM invitations WHERE expires<=? ORDER BY expires LIMIT 256)", (time.time(),))
            self.db.execute("INSERT INTO invitations VALUES(?,?,?)", (digest(code), workspace, expires))
        return {"pair_code": code, "expires_at": expires}

    @serialized
    def pair(self, code: str, name: str) -> dict | None:
        with self.db:
            self.db.execute("BEGIN IMMEDIATE")
            row = self.db.execute("SELECT invitations.*,workspaces.name AS workspace_name FROM invitations JOIN workspaces ON workspaces.id=invitations.workspace_id WHERE code_hash=? AND expires>?", (digest(code), time.time())).fetchone()
            if not row:
                return None
            value, secret = str(uuid.uuid4()), token()
            self.db.execute("DELETE FROM invitations WHERE code_hash=?", (digest(code),))
            self.db.execute("INSERT INTO devices(id,workspace_id,name,token_hash) VALUES(?,?,?,?)", (value, row["workspace_id"], name, digest(secret)))
        return {"device_id": value, "device_token": secret, "workspace_id": row["workspace_id"], "workspace_name": row["workspace_name"]}

    @serialized
    def authenticate(self, secret: str, role: str):
        assert role in ("nodes", "devices")
        return self.db.execute(f"SELECT id,workspace_id,name,revoked FROM {role} WHERE token_hash=? AND revoked=0", (digest(secret),)).fetchone()

    @serialized
    def revoke(self, role: str, value: str) -> bool:
        assert role in ("nodes", "devices")
        now = time.time()
        with self.db:
            found = self.db.execute(f"UPDATE {role} SET revoked=1 WHERE id=?", (value,)).rowcount
            field = "node_id" if role == "nodes" else "device_id"
            self.db.execute(f"UPDATE requests SET state='failed',error='credential revoked before delivery',result_bytes=length('credential revoked before delivery'),reserved_bytes=0,updated=? WHERE {field}=? AND state='queued'", (now, value))
            self.db.execute(f"UPDATE requests SET state='uncertain',error='credential revoked after claim',result_bytes=length('credential revoked after claim'),reserved_bytes=0,updated=? WHERE {field}=? AND state='claimed'", (now, value))
            if role == "nodes":
                self.db.execute("UPDATE computer_routes SET active=0,snapshot=NULL,snapshot_hash=NULL,last_seen=NULL WHERE gateway_id=?", (value,))
            if role == "devices":
                self.db.execute("DELETE FROM pushes WHERE device_id=?", (value,))
                self.db.execute("DELETE FROM push_jobs WHERE device_id=?", (value,))
        return bool(found)

    @staticmethod
    def envelope(row) -> dict:
        return {"request_id": row["id"], "state": row["state"], "result": json.loads(row["result"]) if row["result"] is not None else None, "error": row["error"]}

    @serialized
    def maintain(self, *, restart=False, queue_ttl=120, claim_ttl=90, retention=7 * 86400, batch_size=256):
        now = time.time()
        batch_size=max(1,min(batch_size,4096))
        with self.db:
            self.db.execute(f"UPDATE requests SET state='failed',error='terminal input expired before delivery',result_bytes=length('terminal input expired before delivery'),reserved_bytes=0,updated=? WHERE id IN (SELECT id FROM requests WHERE state='queued' AND expires_at<=? ORDER BY expires_at LIMIT {batch_size})", (now,now))
            self.db.execute(f"DELETE FROM requests WHERE id IN (SELECT id FROM requests WHERE operation='terminal_snapshot' AND state IN ('completed','failed','uncertain') AND updated<? ORDER BY updated LIMIT {batch_size})", (now-120,))
            # Attachment bytes are carried, never kept beyond a short read window.
            self.db.execute(f"DELETE FROM requests WHERE id IN (SELECT id FROM requests WHERE operation='archive_list' AND state IN ('completed','failed','uncertain') AND updated<? ORDER BY updated,id LIMIT {batch_size})", (now-3600,))
            self.db.execute(f"DELETE FROM requests WHERE id IN (SELECT id FROM requests WHERE operation='file_read' AND state IN ('completed','failed','uncertain') AND (updated<? OR result_read<?) ORDER BY updated LIMIT {batch_size})", (now - ATTACHMENT_RESULT_TTL, now - ATTACHMENT_READ_GRACE))
            self.db.execute(f"DELETE FROM requests WHERE id IN (SELECT id FROM requests WHERE operation='attachment_read' AND state IN ('completed','failed','uncertain') AND (updated<? OR result_read<?) ORDER BY updated LIMIT {batch_size})", (now - ATTACHMENT_RESULT_TTL, now - ATTACHMENT_READ_GRACE))
            self.db.execute(f"UPDATE requests SET state='failed',error='request expired before delivery',result_bytes=length('request expired before delivery'),reserved_bytes=0,updated=? WHERE id IN (SELECT id FROM requests WHERE state='queued' AND created<=? ORDER BY created LIMIT {batch_size})", (now, now - queue_ttl))
            if restart:
                self.db.execute(f"UPDATE requests SET state='uncertain',error='relay restarted after claim',result_bytes=length('relay restarted after claim'),reserved_bytes=0,updated=? WHERE state='claimed'", (now,))
            else:
                self.db.execute(f"UPDATE requests SET state='uncertain',error='node result timeout after claim',result_bytes=length('node result timeout after claim'),reserved_bytes=0,updated=? WHERE id IN (SELECT id FROM requests WHERE state='claimed' AND claimed<=? ORDER BY claimed LIMIT {batch_size})", (now, now - claim_ttl))
            # Read-only inspections can expire; mutations keep indefinite UUID
            # tombstones so an old command can never execute a second time.
            self.db.execute(f"DELETE FROM requests WHERE id IN (SELECT id FROM requests INDEXED BY project_assignment_history_v1 WHERE operation='project_assignment' AND state IN ('completed','failed','uncertain') AND updated<? ORDER BY updated,id LIMIT {batch_size})", (now-120,))
            self.db.execute(f"DELETE FROM requests WHERE id IN (SELECT id FROM requests INDEXED BY hook_health_history WHERE operation='hooks_status' AND state IN ('completed','failed','uncertain') AND updated<? ORDER BY updated LIMIT {batch_size})", (now-120,))
            self.db.execute(f"DELETE FROM requests WHERE id IN (SELECT id FROM requests WHERE operation IN {READ_SQL} AND operation!='terminal_snapshot' AND state IN ('completed','failed','uncertain') AND updated<? ORDER BY updated LIMIT {batch_size})", (now - min(retention, 3600),))
            self.db.execute(f"DELETE FROM requests WHERE id IN (SELECT id FROM requests INDEXED BY history_projection_reads_v1 WHERE operation='history_projection' AND state IN ('completed','failed','uncertain') AND updated<? ORDER BY updated,id LIMIT {batch_size})", (now - min(retention, 3600),))
            self.db.execute(f"UPDATE requests SET body='',body_bytes=0,result=NULL,result_bytes=length('request history expired; delivery must not be retried'),reserved_bytes=0,error='request history expired; delivery must not be retried' WHERE id IN (SELECT id FROM requests WHERE state IN ('completed','failed','uncertain') AND updated<? AND body!='' ORDER BY updated LIMIT {batch_size})", (now - retention,))
            self.db.execute(f"DELETE FROM requests WHERE id IN (SELECT id FROM requests WHERE operation='analytics_read' AND state IN ('completed','failed','uncertain') AND updated<? ORDER BY updated,id LIMIT {batch_size})", (now-120,))
            self.db.execute(f"DELETE FROM requests WHERE id IN (SELECT id FROM requests WHERE operation IN {voice.READ_SQL} AND state IN ('completed','failed','uncertain') AND updated<? ORDER BY updated LIMIT {batch_size})", (now-120,))
            self.db.execute(f"DELETE FROM requests WHERE id IN (SELECT id FROM requests WHERE operation IN {speech_config.READ_SQL} AND state IN ('completed','failed','uncertain') AND updated<? ORDER BY updated LIMIT {batch_size})", (now-120,))
            self.db.execute(f"DELETE FROM requests WHERE id IN (SELECT id FROM requests WHERE operation IN {speech.READ_SQL} AND state IN ('completed','failed','uncertain') AND updated<? ORDER BY updated LIMIT {batch_size})", (now-120,))
            self.db.execute(f"UPDATE requests SET body='',body_bytes=0 WHERE id IN (SELECT id FROM requests WHERE operation IN ('speech_submit','voice_submit','speech_profile_upsert','speech_profile_remove','speech_profile_test') AND body!='' AND state IN ('completed','failed','uncertain') ORDER BY updated LIMIT {batch_size})")
            self.db.execute(f"DELETE FROM events WHERE id IN (SELECT id FROM events WHERE created<? ORDER BY created LIMIT {batch_size})", (now - retention,))
            self.db.execute(f"DELETE FROM push_jobs WHERE id IN (SELECT id FROM push_jobs WHERE created<? OR attempts>=5 ORDER BY created LIMIT {batch_size})", (now - 86400,))
            self.db.execute(f"DELETE FROM invitations WHERE code_hash IN (SELECT code_hash FROM invitations WHERE expires<=? ORDER BY expires LIMIT {batch_size})", (now,))

    @serialized
    def submit(self, device, body: dict, max_queue: int, max_bytes: int = MAX_QUEUE_BYTES):
        now, encoded = time.time(), canonical(body)
        with self.db:
            self.db.execute("BEGIN IMMEDIATE")
            if not self.authorized(device["id"], "devices"):
                return 401, {"error": "credential revoked"}
            existing = self.db.execute("SELECT id,device_id,body_hash,state,result,error FROM requests WHERE id=?", (body["request_id"],)).fetchone()
            if existing:
                if existing["device_id"] != device["id"] or existing["body_hash"] != digest(encoded):
                    return 409, {"error": "request_id already used with different content"}
                return 202, self.envelope(existing)
            node = drive_sync(self.db, self.registry.select(device["workspace_id"], body["computer_id"]))
            if not node:
                exists = drive_sync(self.db, self.registry.resolve(device["workspace_id"], body["computer_id"]))
                return (409 if exists else 404), {"error": "computer route is unavailable" if exists else "computer not found"}
            snapshot = json.loads(node["snapshot"]) if node["snapshot"] else None
            if body["operation"] in {"speech_status", "speech_cancel"}:
                owner = self.db.execute("SELECT workspace_id,device_id,operation,target_computer_id,target_machine_id FROM requests WHERE id=?", (body["payload"]["job_id"],)).fetchone()
                if (not owner or owner["workspace_id"] != device["workspace_id"] or owner["device_id"] != device["id"]
                        or owner["operation"] != "speech_submit" or owner["target_computer_id"] != node["id"]
                        or owner["target_machine_id"] != node["machine_id"]):
                    return 404, {"error": "speech job not found"}
            if body["operation"] == "speech_config_status":
                owner = self.db.execute("SELECT workspace_id,device_id,operation,target_computer_id,target_machine_id FROM requests WHERE id=?", (body["payload"]["operation_id"],)).fetchone()
                if (not owner or owner["workspace_id"] != device["workspace_id"] or owner["device_id"] != device["id"]
                        or owner["operation"] not in speech_config.MUTATIONS or owner["target_computer_id"] != node["id"]
                        or owner["target_machine_id"] != node["machine_id"]):
                    return 404, {"error": "speech configuration operation not found"}
            if body["operation"] in CAPABILITY_OPERATIONS and not supports(snapshot, body["operation"]):
                return 409, {"error": "computer does not advertise this operation; update its connector"}
            if body["operation"] == "clear_context" and "dismiss_prompt" in body["payload"] and not supports(snapshot, CLEAR_CONTEXT_DISMISS_FEATURE, "features"):
                return 409, {"error": "computer does not advertise dismiss-and-clear; update its connector"}
            if body["operation"] == "inspect" and any(not supports(snapshot, feature, "features") for feature in inspect_features(body["payload"])):
                return 409, {"error": "computer does not advertise this inspection feature; update its connector"}
            if body["operation"] == "send" and "agent_id" in body["payload"] and not supports(snapshot, "send_agent", "features"):
                return 409, {"error": "computer does not advertise child message transport; update its connector"}
            if body["operation"] == "voice_submit" and "video" in body["payload"] and not supports(snapshot, "voice_video", "features"):
                return 409, {"error": "computer does not advertise video messages; update its connector"}
            if body["operation"] == "terminal_input" and (node["last_seen"] is None or now-node["last_seen"]>45):
                return 409, {"error": "computer is offline; terminal input was not queued"}
            usage = self.db.execute("SELECT * FROM workspace_usage WHERE workspace_id=?", (device["workspace_id"],)).fetchone()
            count = usage["active"]
            self.db.execute("UPDATE workspace_usage SET budget_bytes=? WHERE workspace_id=?", (max_bytes,device["workspace_id"]))
            if count >= max_queue:
                return 429, {"error": "workspace queue is full"}
            reads = body["operation"] in READ_OPERATIONS
            total = usage["reads" if reads else "mutations"]
            if total >= (5000 if reads else 100000):
                return 429, {"error": "workspace retained request limit reached"}
            byte_count = len(encoded.encode()) + (144 if node["machine_id"] else 0)
            retained = usage["payload_bytes"]
            # Preserve a small control/text allowance when attachment traffic
            # occupies the main body budget. No uncertain request is evicted.
            small = byte_count <= 64 * 1024 and not body.get("payload", {}).get("attachments")
            if retained + byte_count + 1024*1024 > max_bytes + (1024 * 1024 if small else 0):
                return 429, {"error": "workspace retained payload budget is full"}
            self.db.execute("INSERT INTO requests(id,workspace_id,device_id,node_id,operation,body_hash,body_bytes,body,state,created,updated,reserved_bytes,target_computer_id,target_machine_id,route_id) VALUES(?,?,?,?,?,?,?,?,'queued',?,?,1048576,?,?,?)", (body["request_id"], device["workspace_id"], device["id"], node["gateway_id"], body["operation"], digest(encoded), byte_count, encoded, now, now, node["id"], node["machine_id"], None if node["local"] else node["route_id"]))
            if body["operation"] == "terminal_input":
                self.db.execute("UPDATE requests SET expires_at=? WHERE id=?", (now+5,body["request_id"]))
            row = self.db.execute("SELECT id,state,result,error FROM requests WHERE id=?", (body["request_id"],)).fetchone()
        return 202, self.envelope(row)

    @serialized
    def has_pending(self,node,queue_ttl=120,allow_gateway=False):
        now=time.time()
        return self.db.execute("SELECT 1 FROM requests r JOIN nodes n ON n.id=r.node_id WHERE r.node_id=? AND n.workspace_id=? AND n.revoked=0 AND r.state='queued' AND (r.target_machine_id IS NULL OR ?) AND r.created>? AND (r.expires_at IS NULL OR r.expires_at>?) LIMIT 1",(node["id"],node["workspace_id"],int(allow_gateway),now-queue_ttl,now)).fetchone() is not None

    @serialized
    def claim(self, node, queue_ttl=120, claim_ttl=90, allow_gateway=False) -> list:
        now = time.time()
        with self.db:
            self.db.execute("BEGIN IMMEDIATE")
            live = self.db.execute("SELECT 1 FROM nodes WHERE id=? AND revoked=0", (node["id"],)).fetchone()
            if not live:
                return []
            row = self.db.execute("SELECT * FROM requests WHERE node_id=? AND state='queued' AND (target_machine_id IS NULL OR ?) AND created>? AND (expires_at IS NULL OR expires_at>?) ORDER BY created LIMIT 1", (node["id"],int(allow_gateway),now-queue_ttl,now)).fetchone()
            if not row:
                return []
            if not drive_sync(self.db, self.registry.frozen(row, allow_gateway)):
                drive_sync(self.db, self.registry.invalidate('id=?', (row['id'],)))
                return []
            self.db.execute("UPDATE requests SET state='claimed',claimed=?,updated=? WHERE id=? AND state='queued'", (now, now, row["id"]))
            body = json.loads(row["body"])
        command={key: body[key] for key in ("request_id", "operation", "session", "payload")}
        if row["target_machine_id"] is not None:
            command["gateway_route"] = {"schema": 1, "route_id": row["route_id"], "computer_id": row["target_computer_id"], "machine_id": row["target_machine_id"]}
        if row["expires_at"] is not None:
            command["expires_at"]=row["expires_at"]
        return [command]

    @serialized
    def result(self, node, request_id: str, body: dict, claim_ttl=90) -> int:
        with self.db:
            if not self.authorized(node["id"], "nodes"): return 401
            row = self.db.execute("SELECT id,workspace_id,state,result,error,claimed,result_bytes,reserved_bytes FROM requests WHERE id=? AND node_id=?", (request_id, node["id"])).fetchone()
            if not row:
                return 404
            encoded = canonical(body["result"]) if body["result"] is not None else None
            if len((encoded or "").encode()) + len((body["error"] or "").encode()) > 1024 * 1024:
                return 413
            if row["state"] == "claimed" and row["claimed"] <= time.time()-claim_ttl:
                self.db.execute("UPDATE requests SET state='uncertain',error='node result timeout after claim',result_bytes=length('node result timeout after claim'),reserved_bytes=0,updated=? WHERE id=?", (time.time(),request_id))
                return 409
            if row["state"] != "claimed":
                # A late success cannot silently resolve an uncertain delivery.
                return 200 if (row["state"], row["result"], row["error"]) == (body["state"], encoded, body["error"]) else 409
            size = len((encoded or "").encode()) + len((body["error"] or "").encode())
            usage = self.db.execute("SELECT * FROM workspace_usage WHERE workspace_id=?", (row["workspace_id"],)).fetchone()
            if size > row["result_bytes"] + row["reserved_bytes"] and usage["payload_bytes"] - row["result_bytes"] - row["reserved_bytes"] + size > usage["budget_bytes"] + 1024*1024:
                return 429
            self.db.execute("UPDATE requests SET state=?,result=?,error=?,updated=?,result_bytes=?,reserved_bytes=0 WHERE id=?", (body["state"], encoded, body["error"], time.time(), size, request_id))
            self.db.execute("UPDATE requests SET body='',body_bytes=0 WHERE id=? AND operation IN ('speech_submit','voice_submit','speech_profile_upsert','speech_profile_remove','speech_profile_test')", (request_id,))
        return 200

    @serialized
    def event(self, node, session: str, kind: str, wake=True):
        now = time.time()
        cur = self.db.execute("INSERT INTO events(workspace_id,node_id,session,kind,created) VALUES(?,?,?,?,?)", (node["workspace_id"], node["id"], session, kind, now))
        if not wake:
            return
        payload = canonical({"event_id": cur.lastrowid, "kind": "wake"})
        self.db.execute("INSERT INTO push_jobs(device_id,event_id,payload,next_at,created) SELECT p.device_id,?,?,?,? FROM pushes p JOIN devices d ON d.id=p.device_id WHERE d.workspace_id=? AND d.revoked=0 AND NOT EXISTS(SELECT 1 FROM push_jobs j WHERE j.device_id=p.device_id) LIMIT max(0,10000-(SELECT push_count FROM relay_usage WHERE id=1))", (cur.lastrowid, payload, now, now, node["workspace_id"]))

    @serialized
    def heartbeat(self, node, snapshot: dict, machine_id=None, peers=None):
        with self.db:
            self.db.execute("BEGIN IMMEDIATE")
            manifest=digest(canonical({'machine_id':machine_id,'peers':peers or [],'guarded':supports(snapshot,'gateway_one_hop','features')}))
            current=self.db.execute('SELECT manifest_hash FROM nodes WHERE id=? AND revoked=0',(node['id'],)).fetchone()
            if current is None:return False
            if current['manifest_hash']!=manifest:
                drive_sync(self.db, self.registry.gateway(node, machine_id, peers or [], supports(snapshot, 'gateway_one_hop', 'features')))
                self.db.execute('UPDATE nodes SET manifest_hash=? WHERE id=?',(manifest,node['id']))
            return self._heartbeat_snapshot(node, snapshot)

    def _heartbeat_snapshot(self, node, snapshot: dict):
        now = time.time()
        encoded=canonical(snapshot)
        snapshot_hash=digest(encoded)
        with self.db:
            row = self.db.execute("SELECT n.snapshot_hash,c.revoked AS computer_revoked FROM nodes n JOIN computers c ON c.id=n.computer_id WHERE n.id=? AND n.revoked=0", (node["id"],)).fetchone()
            if row is None: return False
            if row['computer_revoked'] or (row[0]==snapshot_hash and not any(notification_highwater.preference(s) is not None for s in snapshot.get('sessions', []))):
                self.db.execute("UPDATE nodes SET last_seen=? WHERE id=?",(now,node["id"]))
                return False
            computer=self.db.execute('SELECT computer_id FROM nodes WHERE id=?',(node['id'],)).fetchone()[0]
            event_node={'id':computer,'workspace_id':node['workspace_id']}
            drive_sync(self.db, notification_highwater.accept(node['workspace_id'],computer,snapshot))
            previous = self.db.execute("SELECT snapshot FROM nodes WHERE id=?",(node["id"],)).fetchone()[0]
            if previous is not None:
                if not session_notifications.revisions_current(json.loads(previous), snapshot):
                    raise RouteError(409, "stale shared notification revision")
                old = {s["name"]: s for s in json.loads(previous).get("sessions", []) if isinstance(s, dict) and isinstance(s.get("name"), str) and not is_archived(s)}
                for session in snapshot.get("sessions", []):
                    if not isinstance(session, dict) or not isinstance(session.get("name"), str) or len(session["name"])>1024 or any(ord(c)<32 for c in session["name"]) or is_archived(session):
                        continue
                    before = old.get(session["name"])
                    if before is None:
                        before = {}
                    identity = lambda s: (s.get("run_id"), s.get("conversation_id"))
                    question = lambda s: canonical([s.get("attention_id"), s.get("question_request", s.get("question_requests", None))])
                    if session.get("notification_preferences") != before.get("notification_preferences"):
                        self.event(event_node, session["name"], "notification_preferences")
                    preference = session.get("notification_preferences", {})
                    muted = isinstance(preference, dict) and preference.get("conversation_id") == session.get("conversation_id") and preference.get("muted") is True
                    phase = session.get("phase")
                    pending = lambda s: {canonical(q) for q in s.get("mobile_attention", []) if isinstance(q, dict)} if isinstance(s.get("mobile_attention", []), list) else set()
                    new_async_question = bool(pending(session) - pending(before)) or (bool(pending(session)) and identity(before) != identity(session))
                    if new_async_question or (phase in ("approval", "input") and (before.get("phase") != phase or identity(before) != identity(session) or question(before) != question(session))):
                        self.event(event_node, session["name"], "attention", wake=not muted)
                    elif phase == "error" and (before.get("phase") != "error" or identity(before) != identity(session) or question(before) != question(session)):
                        self.event(event_node, session["name"], "error", wake=not muted)
                    elif session.get("activity") == "idle" and before.get("activity") == "busy" and identity(before) == identity(session):
                        self.event(event_node, session["name"], "completed", wake=not muted)
            self.db.execute("UPDATE nodes SET last_seen=?,snapshot=?,snapshot_hash=? WHERE id=?", (now, encoded,snapshot_hash,node["id"]))
            # Bound event and push storage even for a noisy computer.
            excess=self.db.execute("SELECT max(0,event_count-100000) FROM relay_usage WHERE id=1").fetchone()[0]
            if excess:
                self.db.execute("DELETE FROM events WHERE id IN (SELECT id FROM events ORDER BY id LIMIT ?)",(min(excess,5000),))
            return True


    @serialized
    def authorized(self, value, role):
        assert role in ("nodes", "devices")
        return self.db.execute(f"SELECT 1 FROM {role} WHERE id=? AND revoked=0", (value,)).fetchone() is not None

    @serialized
    def ready(self):
        return self.db.execute("SELECT 1").fetchone()[0] == 1

    @serialized
    def computers(self, workspace, max_bytes=1024*1024):
        with self.db:
            return drive_sync(self.db, self.registry.catalog(workspace, max_bytes))

    @serialized
    def request_gateway(self, request_id):
        row = self.db.execute('SELECT node_id FROM requests WHERE id=?', (request_id,)).fetchone()
        return row[0] if row else None

    @staticmethod
    def _event_changes(previous, snapshot):
        if previous is None:
            return []
        old = {
            s["name"]: s
            for s in json.loads(previous).get("sessions", [])
            if isinstance(s, dict)
            and isinstance(s.get("name"), str)
            and not is_archived(s)
        }
        events = []
        for session in snapshot.get("sessions", []):
            if (
                not isinstance(session, dict)
                or not isinstance(session.get("name"), str)
                or is_archived(session)
            ):
                continue
            before = old.get(session["name"], {})
            identity = lambda s: (s.get("run_id"), s.get("conversation_id"))
            question = lambda s: canonical(
                [
                    s.get("attention_id"),
                    s.get("question_request", s.get("question_requests", None)),
                ]
            )
            pending = lambda s: (
                {
                    canonical(q)
                    for q in s.get("mobile_attention", [])
                    if isinstance(q, dict)
                }
                if isinstance(s.get("mobile_attention", []), list)
                else set()
            )
            if session.get("notification_preferences") != before.get("notification_preferences"):
                events.append((session["name"], "notification_preferences"))
            phase = session.get("phase")
            if (
                bool(pending(session) - pending(before))
                or (bool(pending(session)) and identity(before) != identity(session))
                or (
                    phase in ("approval", "input")
                    and (
                        before.get("phase") != phase
                        or identity(before) != identity(session)
                        or question(before) != question(session)
                    )
                )
            ):
                kind = "attention"
            elif phase == "error" and (
                before.get("phase") != "error"
                or identity(before) != identity(session)
                or question(before) != question(session)
            ):
                kind = "error"
            elif (
                session.get("activity") == "idle"
                and before.get("activity") == "busy"
                and identity(before) == identity(session)
            ):
                kind = "completed"
            else:
                continue
            events.append((session["name"], kind))
        return events


    @serialized
    def peer_heartbeat(self, node, route_id, machine_id, snapshot):
        encoded = canonical(snapshot)
        if len(encoded.encode()) > MAX_PEER_SNAPSHOT_BYTES:
            raise RouteError(413, 'peer snapshot is too large')
        with self.db:
            self.db.execute('BEGIN IMMEDIATE')
            result=drive_sync(self.db, self.registry.peer_heartbeat(node, route_id, machine_id, encoded))
            drive_sync(self.db, notification_highwater.accept(node['workspace_id'],result['id'],snapshot))
            if result["previous"] is not None and not session_notifications.revisions_current(json.loads(result["previous"]), snapshot):
                raise RouteError(409, "stale shared notification revision")
            for name,kind in self._event_changes(result['previous'],snapshot):
                if len(name)<=1024 and not any(ord(c)<32 for c in name):
                    row = next((s for s in snapshot.get('sessions', []) if isinstance(s, dict) and s.get('name') == name), {})
                    preference = row.get('notification_preferences', {})
                    muted = isinstance(preference, dict) and preference.get('conversation_id') == row.get('conversation_id') and preference.get('muted') is True
                    self.event({'id':result['id'],'workspace_id':node['workspace_id']},name,kind,wake=kind == 'notification_preferences' or not muted)
            excess=self.db.execute('SELECT max(0,event_count-100000) FROM relay_usage WHERE id=1').fetchone()[0]
            if excess:self.db.execute('DELETE FROM events WHERE id IN (SELECT id FROM events ORDER BY id LIMIT ?)',(min(excess,5000),))
            return result

    @serialized
    def revoke_computer(self, value):
        with self.db:
            self.db.execute('BEGIN IMMEDIATE')
            return drive_sync(self.db, self.registry.revoke_computer(value))

    @serialized
    def get_request(self, device, value, queue_ttl=120, claim_ttl=90):
        now = time.time()
        with self.db:
            if not self.authorized(device["id"],"devices"): return None
            row = self.db.execute("SELECT id,operation,state,result,error,created,claimed,expires_at FROM requests WHERE id=? AND device_id=?", (value,device["id"])).fetchone()
            if row is None: return None
            if row["state"] == "queued" and ((row["expires_at"] is not None and row["expires_at"] <= now) or row["created"] <= now-queue_ttl):
                error = "terminal input expired before delivery" if row["expires_at"] is not None and row["expires_at"] <= now else "request expired before delivery"
                self.db.execute("UPDATE requests SET state='failed',error=?,result_bytes=?,reserved_bytes=0,updated=? WHERE id=?", (error,len(error.encode()),now,value))
            elif row["state"] == "claimed" and row["claimed"] <= now-claim_ttl:
                self.db.execute("UPDATE requests SET state='uncertain',error='node result timeout after claim',result_bytes=length('node result timeout after claim'),reserved_bytes=0,updated=? WHERE id=?", (now,value))
            row = self.db.execute("SELECT id,operation,state,result,error FROM requests WHERE id=?", (value,)).fetchone()
            if row["operation"] in ("terminal_snapshot", "attachment_read", "file_read") and row["state"] not in ("queued","claimed"):
                self.db.execute("UPDATE requests SET result_read=? WHERE id=?", (now,value))
            return self.envelope(row)

    @serialized
    def events(self, workspace, after, max_bytes=1024*1024):
        size=self.db.execute("SELECT COALESCE(sum(length(CAST(session AS BLOB))+256),0) FROM (SELECT session FROM events WHERE workspace_id=? AND id>? ORDER BY id LIMIT 100)",(workspace,after)).fetchone()[0]
        if size>max_bytes: return None
        return [dict(row) for row in self.db.execute("SELECT * FROM events WHERE workspace_id=? AND id>? ORDER BY id LIMIT 100", (workspace,after))]

    @serialized
    def register_push(self, device, provider, target):
        with self.db:
            if not self.authorized(device, "devices"): return
            self.db.execute("INSERT INTO pushes VALUES(?,?,?) ON CONFLICT(device_id) DO UPDATE SET provider=excluded.provider,target=excluded.target", (device,provider,target))
            self.db.execute("DELETE FROM push_jobs WHERE device_id=?", (device,))

    @serialized
    def delete_push(self, device):
        with self.db:
            self.db.execute("DELETE FROM pushes WHERE device_id=?", (device,))
            self.db.execute("DELETE FROM push_jobs WHERE device_id=?", (device,))

    @serialized
    def claim_push_jobs(self, limit=4, lease_ttl=60):
        now = time.time()
        with self.db:
            self.db.execute("BEGIN IMMEDIATE")
            jobs = [dict(row) for row in self.db.execute("SELECT * FROM push_jobs WHERE next_at<=? AND attempts<5 ORDER BY id LIMIT ?", (now,limit))]
            for job in jobs:
                self.db.execute("UPDATE push_jobs SET next_at=? WHERE id=?", (now+lease_ttl,job["id"]))
        return jobs

    @serialized
    def push_registration(self, device):
        row = self.db.execute("SELECT p.* FROM pushes p JOIN devices d ON d.id=p.device_id WHERE p.device_id=? AND d.revoked=0", (device,)).fetchone()
        return dict(row) if row else None

    @serialized
    def finish_push(self, job, delivered, invalid, registration):
        with self.db:
            if delivered or invalid or job["attempts"] >= 4 or registration is None:
                self.db.execute("DELETE FROM push_jobs WHERE id=?", (job["id"],))
                if invalid and registration:
                    self.db.execute("DELETE FROM pushes WHERE device_id=? AND provider=? AND target=?", (job["device_id"],registration["provider"],registration["target"]))
            else:
                self.db.execute("UPDATE push_jobs SET attempts=attempts+1,next_at=? WHERE id=?", (time.time()+min(3600,5*2**job["attempts"]),job["id"]))
