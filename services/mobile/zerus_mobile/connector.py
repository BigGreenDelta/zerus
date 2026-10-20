"""Outbound-only connector. Native hgs owns agent identity and input validation.

Run with ``python -m zerus_mobile.connector --config /private/connector.json``.
The config contains server_url, node_token, hgs_path and optionally poll_interval
and state_dir (the connector's private journal, separate from HGS_STATE_DIR).
"""
from __future__ import annotations

import argparse
import asyncio
import contextlib
import contextvars
import fcntl
import hashlib
import json
import logging
import math
import os
from pathlib import Path
import sqlite3
import stat
import time
import uuid
from urllib.parse import urlsplit

import aiohttp

from .managed_relay import transport, identity_url
from .accounts import AccountCache
from .attachments import MAX_REQUEST_BYTES, validate_read, validate_send
from . import analytics, speech, speech_config, voice, session_notifications
from . import attachment_reads, file_reads, archives, hook_health
from .recovery import validate_recovery
from .history import validate_history
from .context import CLEAR_CONTEXT_DISMISS_FEATURE, CLEAR_CONTEXT_DISMISS_HELP, validate_clear_context
from .context import ATTACHMENT_OPERATIONS, CONTEXT_COMMANDS, LIFECYCLE_OPERATIONS, TERMINAL_OPERATIONS, LAUNCH_OPERATIONS, COMPUTER_OPERATIONS, OPERATIONS, FEATURES, READ_OPERATIONS, READ_CLASS_SQL, READ_SQL, validate_context, validate_core, validate_inspect, validate_lifecycle, mark_read_result, validate_mark_read, validate_mobile_delivery
from .launch import catalog as project_catalog, validate as validate_launch
from .launch import projects as launch_projects, project_choice
from .launch import DIRECTORY_CREATE_HELP, DIRECTORY_CREATE_FEATURE, canonical_directory, directory_created
from . import worktrees
from .terminal import validate as validate_terminal
from .history import bound_inspection
from .projects import apply_memberships, is_archived, normalize as normalize_projects, unavailable as unavailable_projects

LOG = logging.getLogger("zerus.connector")
MUTATIONS = OPERATIONS - READ_OPERATIONS
CONTEXT_KEYS = frozenset({
    "TMUX", "TMUX_PANE", "HGS_SESSION", "HGS_RUN_ID", "HGS_EXPECTED_ID",
    "HGS_EXECUTABLE", "HGS_AGENT", "HGS_FRESH", "HGS_FRESH_REPLACES", "HGS_REQUESTED_ID",
    "HGS_ARCHIVE_ID", "HGS_LAUNCH_ID", "HGS_FORK_PARENT_ID", "HGS_ACCOUNT_ID",
    "HGS_CLIENT", "CLAUDE_CONFIG_DIR", "CODEX_HOME", "KIMI_CODE_HOME",
    "DSH_HOME", "DSH_CONFIG_DIR", "OPENCODE_CONFIG_DIR", "OPENCODE_CONFIG", "OPENCODE_CONFIG_CONTENT",
})
MAX_BYTES = 8 * 1024 * 1024
MAX_PAYLOAD = MAX_REQUEST_BYTES
MAX_RESULT = 1024 * 1024
ARCHIVE_SNAPSHOT_TTL = 45
LOCAL_MACHINE_GUARD = contextvars.ContextVar("mobile_local_machine_guard", default=None)


class ConnectorError(Exception):
    """A deliberately sanitized error, suitable for transmission and logs."""


class RelayError(ConnectorError):
    def __init__(self, status: int):
        super().__init__("relay request failed")
        self.status = status


def checked_uuid(value: object) -> str:
    if not isinstance(value, str):
        raise ConnectorError("request_id must be a canonical UUID")
    try:
        if str(uuid.UUID(value)) != value:
            raise ValueError
    except ValueError:
        raise ConnectorError("request_id must be a canonical UUID") from None
    return value


def checked_url(value: object, allow_insecure_localhost: bool = False) -> str:
    if not isinstance(value, str) or any(ord(c) < 33 or ord(c) == 127 for c in value):
        raise ConnectorError("invalid server_url")
    try:
        parsed = urlsplit(value)
        parsed.port  # Validate port without displaying the URL.
        valid = bool(parsed.hostname) and not (
            parsed.username is not None or parsed.password is not None
            or parsed.query or parsed.fragment
        )
        secure = parsed.scheme == "https"
        local = (allow_insecure_localhost and parsed.scheme == "http"
                 and parsed.hostname in {"localhost", "127.0.0.1", "::1"})
        if not valid or not (secure or local):
            raise ValueError
    except ValueError:
        raise ConnectorError("server_url requires HTTPS; development HTTP is loopback-only") from None
    return value.rstrip("/")


def native_environment() -> dict[str, str]:
    # Preserve HGS_CONFIG_DIR/HGS_STATE_DIR, HOME, PATH, native API settings and
    # other machine configuration; discard only the calling agent's identity.
    return {key: value for key, value in os.environ.items() if key not in CONTEXT_KEYS}


class Journal:
    def __init__(self, directory: Path, binding: str):
        directory.mkdir(parents=True, exist_ok=True, mode=0o700)
        info = directory.lstat()
        if (not stat.S_ISDIR(info.st_mode) or info.st_uid != os.getuid()
                or info.st_mode & 0o077):
            raise ConnectorError("connector state_dir must be an owned private directory")

        def private_file(path: Path) -> int:
            try:
                fd = os.open(path, os.O_CREAT | os.O_RDWR | os.O_NOFOLLOW, 0o600)
            except OSError:
                raise ConnectorError("connector journal file is not safe") from None
            info = os.fstat(fd)
            if (not stat.S_ISREG(info.st_mode) or info.st_uid != os.getuid()
                    or info.st_mode & 0o077 or info.st_nlink != 1):
                os.close(fd)
                raise ConnectorError("connector journal file must be owned and private")
            return fd

        self.lock = os.fdopen(private_file(directory / "connector.lock"), "r+b")
        try:
            fcntl.flock(self.lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError:
            self.lock.close()
            raise ConnectorError("connector journal is already in use") from None
        try:
            os.close(private_file(directory / "journal.sqlite3"))
            for suffix in ("-journal", "-wal", "-shm"):
                sidecar = directory / ("journal.sqlite3" + suffix)
                if sidecar.exists() or sidecar.is_symlink():
                    os.close(private_file(sidecar))
            self.db = sqlite3.connect(directory / "journal.sqlite3")
            self.db.execute("CREATE TABLE IF NOT EXISTS metadata (key TEXT PRIMARY KEY, value TEXT NOT NULL)")
            with self.db:
                stored = self.db.execute("SELECT value FROM metadata WHERE key='relay_binding'").fetchone()
                if stored and stored[0] != binding:
                    raise ConnectorError("journal belongs to different relay credentials; preserve its existing configuration")
                self.db.execute("INSERT OR IGNORE INTO metadata VALUES('relay_binding',?)", (binding,))
        except Exception:
            if hasattr(self, "db"):
                self.db.close()
            self.lock.close()
            raise
        self.db.execute("PRAGMA synchronous=FULL")
        self.db.execute("PRAGMA secure_delete=ON")
        self.db.execute("""CREATE TABLE IF NOT EXISTS requests (
            request_id TEXT PRIMARY KEY, operation TEXT NOT NULL,
            state TEXT NOT NULL, response TEXT, delivered INTEGER NOT NULL DEFAULT 0,
            request_hash TEXT, created_at REAL NOT NULL, updated_at REAL NOT NULL,
            delivered_at REAL, response_expired INTEGER NOT NULL DEFAULT 0
        )""")
        now = time.time()
        columns = {row[1] for row in self.db.execute("PRAGMA table_info(requests)")}
        with self.db:
            # Legacy records have no reliable age or request fingerprint. Give
            # them the current time rather than pruning old unknown deliveries.
            for name, kind in (("request_hash", "TEXT"), ("created_at", "REAL"),
                               ("updated_at", "REAL"), ("delivered_at", "REAL"),
                               ("response_expired", "INTEGER NOT NULL DEFAULT 0")):
                if name not in columns:
                    self.db.execute(f"ALTER TABLE requests ADD COLUMN {name} {kind}")
            self.db.execute("UPDATE requests SET created_at=? WHERE created_at IS NULL", (now,))
            self.db.execute("UPDATE requests SET updated_at=? WHERE updated_at IS NULL", (now,))
            self.db.execute("UPDATE requests SET delivered_at=? WHERE delivered=1 AND delivered_at IS NULL", (now,))
            rows = self.db.execute(
                "SELECT request_id, operation FROM requests WHERE state='running'").fetchall()
            for request_id, operation in rows:
                state = "uncertain" if operation in MUTATIONS else "failed"
                response = {"state": state, "result": None,
                            "error": "connector stopped before recording the native result"}
                self.db.execute("UPDATE requests SET state=?,response=?,updated_at=? WHERE request_id=?",
                                (state, json.dumps(response), now, request_id))
        self.db.execute("CREATE INDEX IF NOT EXISTS delivered_history ON requests(delivered,delivered_at)")
        self.db.execute("CREATE INDEX IF NOT EXISTS analytics_delivered_reads ON requests(delivered_at) WHERE operation='analytics_read' AND delivered=1 AND state!='running'")
        self.db.execute("CREATE INDEX IF NOT EXISTS terminal_delivered_reads ON requests(delivered_at) WHERE operation='terminal_snapshot' AND delivered=1 AND state!='running'")
        self.db.execute("CREATE INDEX IF NOT EXISTS archive_delivered_reads ON requests(delivered_at) WHERE operation='archive_list' AND delivered=1 AND state!='running'")
        self.db.execute("CREATE INDEX IF NOT EXISTS file_delivered_reads ON requests(delivered_at) WHERE operation='file_read' AND delivered=1 AND state!='running'")
        self.db.execute("CREATE INDEX IF NOT EXISTS attachment_delivered_reads ON requests(delivered_at) WHERE operation='attachment_read' AND delivered=1 AND state!='running'")
        self.db.execute("CREATE INDEX IF NOT EXISTS journal_operation ON requests(operation)")
        self.db.execute("CREATE INDEX IF NOT EXISTS delivered_reads ON requests(delivered_at) WHERE operation='inspect' AND delivered=1 AND state!='running'")
        self.db.execute("CREATE INDEX IF NOT EXISTS mutation_retention ON requests(delivered_at) WHERE operation!='inspect' AND delivered=1 AND state!='running' AND response IS NOT NULL AND response_expired=0")
        self.prune()

    def prune(self, *, now: float | None = None) -> None:
        now = time.time() if now is None else now
        with self.db:
            self.db.execute("DELETE FROM requests WHERE operation='hooks_status' AND delivered=1 AND state!='running' AND delivered_at<?", (now - 120,))
            self.db.execute("DELETE FROM requests WHERE operation='terminal_snapshot' AND delivered=1 AND state!='running' AND delivered_at<?", (now - 120,))
            self.db.execute("DELETE FROM requests WHERE operation='analytics_read' AND delivered=1 AND state!='running' AND delivered_at<?", (now - 120,))
            self.db.execute(f"DELETE FROM requests WHERE operation IN {speech_config.READ_SQL} AND delivered=1 AND state!='running' AND delivered_at<?", (now - 120,))
            self.db.execute(f"DELETE FROM requests WHERE operation IN {speech.READ_SQL} AND delivered=1 AND state!='running' AND delivered_at<?", (now - 120,))
            self.db.execute(f"DELETE FROM requests WHERE operation IN {voice.READ_SQL} AND delivered=1 AND state!='running' AND delivered_at<?", (now - 120,))
            # Delivered attachment chunks are never kept beyond the same short window.
            self.db.execute("DELETE FROM requests WHERE operation='archive_list' AND delivered=1 AND state!='running' AND delivered_at<?", (now - 3600,))
            self.db.execute("DELETE FROM requests WHERE operation='file_read' AND delivered=1 AND state!='running' AND delivered_at<?", (now - 120,))
            self.db.execute("DELETE FROM requests WHERE operation='attachment_read' AND delivered=1 AND state!='running' AND delivered_at<?", (now - 120,))
            self.db.execute(f"DELETE FROM requests WHERE operation IN {READ_SQL} AND operation!='terminal_snapshot' AND delivered=1 AND state!='running' AND delivered_at<?", (now - 3600,))
            rows = self.db.execute(f"SELECT request_id,state FROM requests WHERE operation NOT IN {READ_CLASS_SQL} AND delivered=1 AND state!='running' AND delivered_at<? AND response IS NOT NULL AND response_expired=0", (now - 7 * 86400,)).fetchall()
            for request_id, state in rows:
                expired = {"state": state, "result": {"status": "history_expired"},
                           "error": "Native result history expired; this request will not execute again"}
                encoded = json.dumps(expired)
                # Keep a bounded explicit receipt, never the private native result.
                self.db.execute("UPDATE requests SET response=?,response_expired=1 WHERE request_id=?", (encoded, request_id))

    def claim(self, request_id: str, operation: str, request_hash: str | None = None) -> bool:
        self.prune()
        now = time.time()
        with self.db:
            existing = self.db.execute("SELECT operation,request_hash FROM requests WHERE request_id=?", (request_id,)).fetchone()
            if existing:
                if existing[0] != operation or (request_hash is not None and existing[1] != request_hash):
                    raise ConnectorError("request_id conflicts with preserved native request identity")
                return False
            reads = operation in READ_OPERATIONS
            count = self.db.execute(f"SELECT count(*) FROM requests WHERE (operation IN {READ_CLASS_SQL})=?", (int(reads),)).fetchone()[0]
            if count >= (5000 if reads else 100000):
                raise ConnectorError("connector retained request limit reached")
            cursor = self.db.execute(
                "INSERT OR IGNORE INTO requests(request_id,operation,state,request_hash,created_at,updated_at) VALUES(?,?,'running',?,?,?)",
                (request_id, operation, request_hash, now, now))
        return cursor.rowcount == 1

    def finish(self, request_id: str, response: dict) -> None:
        with self.db:
            self.db.execute("UPDATE requests SET state=?,response=?,delivered=0,delivered_at=NULL,response_expired=0,updated_at=? WHERE request_id=?",
                            (response["state"], json.dumps(response), time.time(), request_id))

    def replay(self, request_id: str) -> dict:
        row = self.db.execute("SELECT response FROM requests WHERE request_id=?",
                              (request_id,)).fetchone()
        if not row or row[0] is None:
            return {"state": "uncertain", "result": None,
                    "error": "request is already in progress"}
        with self.db:
            self.db.execute("UPDATE requests SET delivered=0,delivered_at=NULL WHERE request_id=?", (request_id,))
        return json.loads(row[0])

    def pending(self) -> list[tuple[str, dict]]:
        return [(key, json.loads(body)) for key, body in self.db.execute(
            "SELECT request_id,response FROM requests WHERE delivered=0 AND response IS NOT NULL LIMIT 100")]

    def delivered(self, request_id: str) -> None:
        with self.db:
            self.db.execute("UPDATE requests SET delivered=1,delivered_at=?,updated_at=? WHERE request_id=?", (time.time(), time.time(), request_id))

    def close(self) -> None:
        self.db.close()
        self.lock.close()


class Connector:
    def __init__(self, config: dict, *, allow_insecure_localhost: bool = False,
                 subprocess_timeout: float = 45, max_bytes: int = MAX_BYTES):
        stored_url = checked_url(config.get("server_url"), allow_insecure_localhost)
        explicit_identity = config.get("identity_url")
        if explicit_identity is not None:
            explicit_identity = checked_url(explicit_identity, allow_insecure_localhost)
        try:
            self.identity_url = identity_url(stored_url, explicit_identity)
        except ValueError as exc:
            raise ConnectorError(str(exc)) from None
        self.server_url = transport(stored_url)
        token = config.get("node_token")
        if not isinstance(token, str) or not token or any(ord(c) < 33 or ord(c) > 126 for c in token):
            raise ConnectorError("invalid node_token")
        self.token = token
        hgs = config.get("hgs_path", "hgs")
        if not isinstance(hgs, str) or not hgs or "\0" in hgs:
            raise ConnectorError("invalid hgs_path")
        self.hgs = hgs
        interval = config.get("poll_interval", 5)
        if not isinstance(interval, (int, float)) or not math.isfinite(interval) or interval < 1:
            raise ConnectorError("poll_interval must be at least one second")
        self.interval = interval
        self.timeout = subprocess_timeout
        self.max_bytes = max_bytes
        state = config.get("state_dir") or str(Path.home() / ".local/state/hgs/mobile-connector")
        if not isinstance(state, str):
            raise ConnectorError("invalid state_dir")
        binding = hashlib.sha256((self.identity_url + "\0" + self.token).encode()).hexdigest()
        self.journal = Journal(Path(state).expanduser(), binding)
        self._init_machine_context(binding)
        from .fleet import Fleet
        self.fleet = Fleet(self, ConnectorError)
        self.presence_retry_at = 0.0  # Relays predating `presence` reject it; probe again later.

    def _init_machine_context(self, fallback_id: str) -> None:
        """Initialize only machine caches; never open another durable journal."""
        self.sessions: set[str] = set()
        self.archives: set[tuple[str, str]] = set()
        self.snapshot_seen_at = 0.0
        self.snapshot_ready = asyncio.Event()
        self.attention_cache: dict[tuple[str, str, str], list[dict[str, str | bool]]] = {}
        self.attention_cursor = 0
        self.project_fallback_id = fallback_id
        self.project_cache = None
        self.project_next_poll = 0.0
        self.lifecycle_supported = False
        self.terminal_supported = False
        self.recovery_supported = False
        self.hook_health_supported = False
        self.clear_context_dismiss_supported = False
        self.history_supported = False
        self.launch_supported = False
        self.project_launch_supported = False
        self.worktree_supported = False
        self.directory_create_supported = False
        self.read_supported = False
        self.session_notifications_supported = False
        self.preference_supported = False
        self.attachment_read_supported = False
        self.archive_supported = False
        self.paged_archives = {}
        self.archive_preview = []
        self.archive_preview_revision = None
        self.archive_preview_next = 0.0
        self.archive_preview_meta = {"schema":1,"available":False,"loaded":0,"total":None,"partial":False}
        self.file_read_supported = False
        self.analytics_supported = False
        self.speech_supported = False
        self.speech_config_supported = False
        self.voice_supported = False
        self.video_supported = False
        self.attachment_reads = attachment_reads.AttachmentReads()
        self.file_reads = attachment_reads.AttachmentReads()
        self.capabilities_next_poll = 0.0
        self.presence = None  # Latest listing desktop_presence; sent beside, never inside, the snapshot.
        self.accounts = AccountCache(lambda *args, **kwargs: self.native(*args, **kwargs),
                                     errors=(ConnectorError, ValueError, RuntimeError, UnicodeError))

    def peer_context(self, machine_id: str, native) -> Connector:
        context = Connector.__new__(Connector)
        for field in ("journal", "hgs", "interval", "timeout", "max_bytes"):
            setattr(context, field, getattr(self, field))
        context.native = native
        context._init_machine_context(machine_id)
        return context

    async def native(self, argv: list[str], payload: dict | None = None,
                     *, timeout: float | None = None, json_output: bool = True,
                     failure_json: bool = False, max_output_bytes: int | None = None) -> object:
        machine_id = LOCAL_MACHINE_GUARD.get()
        if machine_id is not None:
            payload = {"schema": 1, "target_machine_id": machine_id,
                       "argv": argv, "payload": payload}
            argv = ["swarm", "__mobile-peer-local", "--json"]
        if argv and argv[0] == "worktrees":
            argv = ["__state", *argv]
        return await self._native(argv, payload, timeout=timeout, json_output=json_output,
                                  failure_json=failure_json, max_output_bytes=max_output_bytes)

    async def _native(self, argv: list[str], payload: dict | None = None,
                      *, timeout: float | None = None, json_output: bool = True,
                      max_output_bytes: int | None = None, failure_json: bool = False) -> object:
        stdin = None if payload is None else json.dumps(payload, ensure_ascii=False).encode()
        if stdin is not None and len(stdin) > MAX_PAYLOAD + (4096 if argv[:2] in (["swarm", "mobile-peer"], ["swarm", "__mobile-peer-local"]) else 0):
            raise ConnectorError("native request exceeds size limit")
        process = None
        inner_argv = payload.get("argv", argv) if isinstance(payload, dict) else argv
        mobile_create = "worktrees" in inner_argv and "create" in inner_argv and "--mobile" in inner_argv
        drain_deadline = time.monotonic() + 35
        oversized = False

        async def read(stream: asyncio.StreamReader) -> bytes:
            nonlocal oversized
            chunks, size = [], 0
            while chunk := await stream.read(65536):
                size += len(chunk)
                if size > (self.max_bytes if max_output_bytes is None else max_output_bytes):
                    if not mobile_create: raise ConnectorError("native output exceeds size limit")
                    oversized = True
                    continue  # Keep draining the owned supervisor without retaining excess.
                chunks.append(chunk)
            return b"".join(chunks)

        async def write() -> None:
            if process.stdin is not None:
                try:
                    if stdin is not None:
                        process.stdin.write(stdin)
                        await process.stdin.drain()
                except (BrokenPipeError, ConnectionResetError):
                    pass
                finally:
                    process.stdin.close()

        tasks = []
        joined = None
        try:
            cancelled_during_spawn = False
            spawning = asyncio.create_task(asyncio.create_subprocess_exec(
                self.hgs, *argv, stdin=asyncio.subprocess.PIPE,
                stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.PIPE,
                env=native_environment()))
            if mobile_create:
                while True:
                    try:
                        process = await asyncio.shield(spawning)
                        break
                    except asyncio.CancelledError:
                        cancelled_during_spawn = True
            else:
                process = await spawning
            tasks = [asyncio.create_task(read(process.stdout)),
                     asyncio.create_task(read(process.stderr)),
                     asyncio.create_task(write()), asyncio.create_task(process.wait())]
            joined = asyncio.gather(*tasks)
            if cancelled_during_spawn: raise asyncio.CancelledError()
            stdout, _, _, code = await asyncio.wait_for(asyncio.shield(joined) if mobile_create else joined, self.timeout if timeout is None else timeout)
            if oversized: raise ConnectorError("native output exceeds size limit")
            # `hgs read` answers an unknown session with JSON and exit status 1.
            if code != 0 and not (failure_json and stdout.strip()):
                raise ConnectorError("native command failed")
            if not json_output:
                return stdout.decode("utf-8", errors="strict")
            try:
                result = json.loads(stdout)
            except (ValueError, UnicodeError):
                raise ConnectorError("native command returned invalid JSON") from None
            # A failing exit carries only a failure answer, never a success receipt.
            if code != 0 and not (isinstance(result, dict) and result.get("ok") is False):
                raise ConnectorError("native command failed")
            return result
        except asyncio.TimeoutError:
            raise ConnectorError("native command timed out") from None
        except OSError:
            raise ConnectorError("native command could not start") from None
        finally:
            if mobile_create and joined is not None and process is not None:
                # Keep only this operation's bounded supervisor alive until it cleans
                # its owned Git group. Repeated cancellation must not cut the drain short.
                while not joined.done() and time.monotonic() < drain_deadline:
                    try:
                        await asyncio.wait_for(asyncio.shield(joined), max(.001, drain_deadline - time.monotonic()))
                    except asyncio.CancelledError:
                        continue
                    except (Exception, asyncio.TimeoutError):
                        break
            if process is not None and process.returncode is None:
                # Only this connector-owned CLI child; never its native agent,
                # its process group, a tmux server, or a DeepSeek host.
                with contextlib.suppress(ProcessLookupError):
                    process.kill()
            for task in tasks:
                if not task.done():
                    task.cancel()
            if tasks:
                await asyncio.gather(*tasks, return_exceptions=True)
            if joined is not None:
                with contextlib.suppress(Exception, asyncio.CancelledError):
                    await joined
            if process is not None:
                # A size-limit reader has stopped before EOF. Drain remaining
                # bytes without retaining them so asyncio can reap the CLI;
                # wait() alone deadlocks when its pipe transport is paused.
                async def discard(stream: asyncio.StreamReader) -> None:
                    while await stream.read(65536):
                        pass
                cleanup = asyncio.gather(discard(process.stdout), discard(process.stderr), process.wait())
                try:
                    await asyncio.wait_for(cleanup, 0.5)
                except asyncio.TimeoutError:
                    # A descendant may still hold inherited pipe descriptors.
                    # Close our pipe transports; do not kill that descendant.
                    process._transport.close()
                finally:
                    with contextlib.suppress(Exception, asyncio.CancelledError):
                        await cleanup

    async def snapshot(self) -> dict:
        await self.probe_capabilities()
        snapshot = await self.native(["ls", "--json", "--local"] + (["--no-archives"] if self.archive_supported else []))
        if not isinstance(snapshot, dict) or not isinstance(snapshot.get("sessions"), list):
            raise ConnectorError("native snapshot has invalid format")
        snapshot["mobile_archive"]={"schema":1,"available":False,"loaded":0,"total":None,"partial":False}
        if self.archive_supported:
            revision=snapshot.get("archive_revision")
            current=time.monotonic()
            if isinstance(revision,str) and (revision!=self.archive_preview_revision or current>=self.archive_preview_next):
                # Invalidate old membership before a failed refresh can make it fresh.
                self.archive_preview=[]
                self.archive_preview_meta={"schema":1,"available":False,"loaded":0,"total":None,"partial":False}
                try:
                    value=await self.native(["archives","--json"],{},timeout=5)
                    self.archive_preview,self.archive_preview_meta=archives.preview(value)
                except (ConnectorError,ValueError):
                    self.archive_preview_meta["error"]="Archive preview is unavailable; refresh Archive on an updated client"
                self.archive_preview_revision=revision
                self.archive_preview_next=current+(300 if self.archive_preview_meta["available"] else 15)
            snapshot["sessions"].extend(self.archive_preview)
            snapshot["mobile_archive"]=self.archive_preview_meta
        snapshot.pop("peers", None)  # Native SSH aliases and peer-of-peer inventory stay local.
        # Ended launches carry the last screen lines of failed starts for the
        # desktop's placeholders; terminal text never leaves this computer.
        snapshot.pop("ended_launches", None)
        # Presence ages on every listing; keeping it out of the snapshot preserves
        # the relay's unchanged-snapshot shortcut.
        presence = snapshot.pop("desktop_presence", None)
        self.presence = presence if isinstance(presence, dict) else None
        # Observation clocks are control freshness evidence, not a change in
        # the background work itself. Preserve semantic snapshot deduplication.
        for row in snapshot["sessions"]:
            if isinstance(row, dict) and isinstance(row.get("process_work"), dict):
                row["process_work"].pop("sampled_at", None)
        self.sessions = {row["name"] for row in snapshot["sessions"]
                         if isinstance(row, dict) and isinstance(row.get("name"), str)
                         and not is_archived(row)}
        self.archives = {(row["name"], row["archive_id"]) for row in snapshot["sessions"]
                         if isinstance(row, dict) and is_archived(row)
                         and isinstance(row.get("name"), str) and isinstance(row.get("archive_id"), str)}
        self.snapshot_seen_at = time.monotonic()
        self.snapshot_ready.set()
        await asyncio.gather(self.enrich_attention(snapshot), self.enrich_projects(snapshot))
        if self.archive_supported:
            # Archive membership is applied to explicit pages; thousands of IDs
            # do not belong in every live-session heartbeat.
            groups=snapshot.get("mobile_projects",{})
            snapshot["mobile_projects"]={**groups,"projects":[{k:v for k,v in p.items() if k!="archives"} for p in groups.get("projects",[]) if isinstance(p,dict)]}
        snapshot["mobile_accounts"] = await self.accounts.update()
        await self.probe_capabilities()
        operations = (OPERATIONS - (set() if self.analytics_supported else analytics.OPERATIONS) - (set() if getattr(self,"hook_health_supported",False) else {"hooks_status"})) - (set() if self.archive_supported else {"archive_list"}) - (set() if self.worktree_supported else worktrees.OPERATIONS) - (set() if self.directory_create_supported else {"directory_create"}) - (set() if self.lifecycle_supported else LIFECYCLE_OPERATIONS) - (set() if self.terminal_supported else TERMINAL_OPERATIONS) - (set() if self.launch_supported else LAUNCH_OPERATIONS - {"directory_create"}) - (set() if self.history_supported else {"history"}) - (set() if self.recovery_supported else {"recovery_action"}) - (set() if self.read_supported else {"mark_read"}) - (set() if getattr(self, "session_notifications_supported", False) else {"session_notifications"}) - (set() if self.attachment_read_supported else ATTACHMENT_OPERATIONS) - (set() if self.file_read_supported else {"file_read"}) - (set() if self.preference_supported and getattr(self, "fleet", None) is not None else COMPUTER_OPERATIONS) - (set() if self.speech_supported else speech.OPERATIONS) - (set() if self.voice_supported else voice.OPERATIONS) - (set() if self.speech_config_supported else speech_config.OPERATIONS)
        snapshot["mobile_capabilities"] = {"protocol_version": 1,
            "operations": sorted(operations), "features": sorted(FEATURES | {"accounts_snapshot"} | ({CLEAR_CONTEXT_DISMISS_FEATURE} if getattr(self, "clear_context_dismiss_supported", False) else set()) | ({voice.video.FEATURE} if self.voice_supported and self.video_supported else set()) | ({DIRECTORY_CREATE_FEATURE} if self.directory_create_supported else set()) | ({"gateway_one_hop"} if getattr(self, "fleet", None) and self.fleet.available else set()) | ({"launch_project"} if self.project_launch_supported else set())),
            "reasons": {**({} if self.lifecycle_supported else {"session_actions": "Native CLI lacks the scoped session-action ABI"}),
                        **({} if self.terminal_supported else {"terminal": "Native CLI lacks the scoped Terminal ABI"})}}

        return snapshot

    async def probe_capabilities(self):
        if time.monotonic() < self.capabilities_next_poll:
            return
        self.capabilities_next_poll = time.monotonic() + 300
        self.archive_supported = False
        self.lifecycle_supported = False
        self.terminal_supported = False
        self.recovery_supported = False
        self.hook_health_supported = False
        self.clear_context_dismiss_supported = False
        self.history_supported = False
        self.launch_supported = False
        self.project_launch_supported = False
        self.worktree_supported = False
        self.directory_create_supported = False
        self.read_supported = False
        self.session_notifications_supported = False
        self.preference_supported = False
        self.attachment_read_supported = False
        self.file_read_supported = False
        self.analytics_supported = False
        self.speech_supported = False
        self.speech_config_supported = False
        self.voice_supported = False
        self.video_supported = False
        if getattr(self, "fleet", None):
            self.fleet.supported = False
        try:
            help_text = await self.native(["--help"], timeout=3, json_output=False)
            self.lifecycle_supported = isinstance(help_text, str) and any(
                "session-action <session> --json" in line for line in help_text.splitlines())
            self.terminal_supported = isinstance(help_text, str) and "terminal <session> --json" in help_text
            self.recovery_supported = isinstance(help_text,str) and "recovery action --scoped-json" in help_text
            self.history_supported = isinstance(help_text,str) and "history <session> --json" in help_text
            self.hook_health_supported = isinstance(help_text,str) and hook_health.HELP_MARKER in help_text
            self.clear_context_dismiss_supported = isinstance(help_text, str) and any(
                line.strip() == CLEAR_CONTEXT_DISMISS_HELP for line in help_text.splitlines())
            self.launch_supported = isinstance(help_text,str) and "--launch-id" in help_text
            self.project_launch_supported = isinstance(help_text,str) and "swarm assign-launch --json" in help_text
            self.worktree_supported = isinstance(help_text,str) and "Mobile worktree ABI: worktrees-v1" in help_text
            self.directory_create_supported = isinstance(help_text, str) and DIRECTORY_CREATE_HELP in help_text
            self.read_supported = isinstance(help_text, str) and "read <session> --json" in help_text
            self.session_notifications_supported = isinstance(help_text, str) and any(line.strip() == session_notifications.HELP_MARKER for line in help_text.splitlines())
            self.preference_supported = isinstance(help_text,str) and "swarm preference set mobile_delivery" in help_text
            self.attachment_read_supported = isinstance(help_text, str) and attachment_reads.HELP_MARKER in help_text
            self.archive_supported = isinstance(help_text,str) and archives.HELP_MARKER in help_text
            self.file_read_supported = isinstance(help_text, str) and file_reads.HELP_MARKER in help_text
            self.analytics_supported = isinstance(help_text, str) and analytics.HELP_MARKER in help_text
            self.speech_supported = isinstance(help_text, str) and speech.HELP_MARKER in help_text
            self.voice_supported = isinstance(help_text, str) and voice.HELP_MARKER in help_text
            self.video_supported = isinstance(help_text, str) and voice.video.HELP_MARKER in help_text
            self.speech_config_supported = isinstance(help_text, str) and speech_config.HELP_MARKER in help_text
            if getattr(self, "fleet", None):
                self.fleet.supported = isinstance(help_text, str) and all(
                    marker in help_text for marker in ("mobile-peers --json", "mobile-peer --json"))
        except (ConnectorError, UnicodeError):
            pass
        finally:
            if getattr(self, "fleet", None):
                self.fleet.probe_complete = True

    async def enrich_projects(self, snapshot: dict) -> None:
        """Poll native logical groups every fifteen seconds; retain stale data."""
        if time.monotonic() >= self.project_next_poll:
            self.project_next_poll = time.monotonic() + 15
            try:
                native = await self.native(["swarm", "get"], timeout=3)
                self.project_cache = normalize_projects(native, snapshot, self.project_fallback_id)
            except (ConnectorError, ValueError, TypeError, KeyError):
                self.project_cache = {**(self.project_cache or unavailable_projects(self.project_fallback_id)), "stale": True}
        snapshot["mobile_projects"] = self.project_cache or unavailable_projects(self.project_fallback_id)
        apply_memberships(snapshot, snapshot["mobile_projects"])

    async def enrich_attention(self, snapshot: dict) -> None:
        """Inspect two live Codex sessions per poll, without submitting input.

        A stable fleet of N eligible sessions is scanned every ceil(N/2) polls.
        The nominal interval is ceil(N/2) * poll_interval, plus CLI/network time.
        Each optional inspection has a one-second timeout; expired/changed
        identities discard cached attention. Only IDs, hashes and an
        `optional: true` marker on optional Codex async questions leave the
        node, never question content.
        """
        eligible = []
        for row in snapshot["sessions"]:
            if (not isinstance(row, dict) or row.get("cmd") != "codex"
                    or row.get("tracked") is not True or row.get("runtime_state") != "live"
                    or row.get("process_state") != "running"
                    or is_archived(row) or row.get("state") in {"paused", "stopped"}):
                continue
            name, run, conversation = row.get("name"), row.get("run_id"), row.get("conversation_id")
            if (not all(isinstance(value, str) and value for value in (name, run, conversation))
                    or name.startswith(("@", "-")) or "@" in name
                    or any(ord(c) < 32 or ord(c) == 127 for c in name)):
                continue
            eligible.append(((name, run, conversation), row))
        active = {key for key, _ in eligible}
        self.attention_cache = {key: cards for key, cards in self.attention_cache.items() if key in active}
        if not eligible:
            self.attention_cursor = 0
            return
        selected = [eligible[(self.attention_cursor + i) % len(eligible)]
                    for i in range(min(2, len(eligible)))]
        self.attention_cursor = (self.attention_cursor + len(selected)) % len(eligible)

        async def inspect(key: tuple[str, str, str]) -> None:
            name, run, conversation = key
            try:
                detail = await self.native(["inspect", name], timeout=min(self.timeout, 1))
                if (not isinstance(detail, dict) or detail.get("run_id") != run
                        or detail.get("conversation_id") != conversation
                        or detail.get("runtime_state") != "live"
                        or detail.get("process_state") != "running"
                        or not isinstance(detail.get("pending_questions"), list)):
                    self.attention_cache.pop(key, None)
                    return
                cards = []
                for card in detail["pending_questions"]:
                    if (not isinstance(card, dict) or card.get("run_id") != run
                            or card.get("conversation_id") != conversation
                            or not all(isinstance(card.get(field), str) and card[field]
                                       for field in ("question_id", "question_hash"))):
                        continue
                    delivery = card.get("answer_delivery")
                    if isinstance(delivery, dict) and delivery.get("status") == "submitted":
                        continue
                    item = {field: card[field] for field in ("question_id", "question_hash")}
                    # Phones decide whether to notify; blocking items keep their exact shape.
                    if card.get("optional") is True and card.get("source") == "codex_async":
                        item["optional"] = True
                    cards.append(item)
                self.attention_cache[key] = sorted(cards, key=lambda card: (card["question_id"], card["question_hash"]))
            except ConnectorError:
                self.attention_cache.pop(key, None)
        await asyncio.gather(*(inspect(key) for key, _ in selected))
        for key, row in eligible:
            if key in self.attention_cache:
                row["mobile_attention"] = self.attention_cache[key]

    def validate(self, request: dict) -> tuple[str, str, dict | None]:
        operation, session = request.get("operation"), request.get("session")
        if not isinstance(operation, str) or operation not in OPERATIONS:
            raise ConnectorError("unsupported operation")
        if operation in analytics.OPERATIONS:
            if session != "": raise ConnectorError("analytics reads use no session target")
            try: analytics.validate(request.get("payload"))
            except ValueError as error: raise ConnectorError(str(error)) from None
            return operation, session, request["payload"]
        if operation == "archive_list":
            if session != "": raise ConnectorError("archive browsing uses no session target")
            try: archives.validate(request.get("payload"))
            except ValueError as error: raise ConnectorError(str(error)) from None
            return operation,session,request["payload"]
        if operation in LAUNCH_OPERATIONS:
            if session != "": raise ConnectorError("catalog/launch use no existing session target")
            try: validate_launch(operation,request.get("payload"),request["request_id"])
            except ValueError as error: raise ConnectorError(str(error)) from None
            return operation,session,request["payload"]
        if operation in speech_config.OPERATIONS:
            if session != "": raise ConnectorError("speech configuration uses no session target")
            try: speech_config.validate(operation, request.get("payload"), request["request_id"])
            except ValueError as error: raise ConnectorError(str(error)) from None
            return operation, session, request["payload"]
        if operation in speech.OPERATIONS:
            if session != "": raise ConnectorError("speech uses no session target")
            try: speech.validate(operation, request.get("payload"), request["request_id"])
            except ValueError as error: raise ConnectorError(str(error)) from None
            return operation, session, request["payload"]
        if operation in COMPUTER_OPERATIONS:
            if session != "": raise ConnectorError("the delivery setting uses no session target")
            try: validate_mobile_delivery(request.get("payload"))
            except ValueError as error: raise ConnectorError(str(error)) from None
            return operation, session, request["payload"]
        if (not isinstance(session, str) or not session or len(session) > 512
                or session.startswith(("-", "@")) or "@" in session
                or any(ord(c) < 32 or ord(c) == 127 for c in session)):
            raise ConnectorError("session must match an exact local snapshot entry")
        if "archive_id" in request:
            raise ConnectorError("archive identity belongs only in inspect payload")
        if operation == session_notifications.OPERATION:
            if session not in self.sessions:
                raise ConnectorError("session must match an exact nonarchived snapshot entry")
            try: session_notifications.validate(request.get("payload"))
            except ValueError as error: raise ConnectorError(str(error)) from None
            return operation, session, request["payload"]
        if operation == "mark_read":
            if session not in self.sessions:
                raise ConnectorError("session must match an exact nonarchived snapshot entry")
            try:
                validate_mark_read(request.get("payload"))
            except ValueError as error:
                raise ConnectorError(str(error)) from None
            return operation, session, request["payload"]
        if operation == "inspect":
            payload = request.get("payload", {})
            try:
                validate_inspect(payload)
            except ValueError as error:
                raise ConnectorError(str(error)) from None
            if "archive_id" in payload:
                archive_id = checked_uuid(payload["archive_id"])
                if not Connector.archive_allowed(self, session, archive_id) and not getattr(self,"archive_supported",False):
                    if time.monotonic()-self.snapshot_seen_at>ARCHIVE_SNAPSHOT_TTL:
                        raise ConnectorError("archive snapshot is stale; refresh before inspection")
                    raise ConnectorError("archive must match an exact local snapshot entry")
                return operation, session, payload
            if session not in self.sessions:
                raise ConnectorError("session must match an exact nonarchived snapshot entry")
            return operation, session, payload or None
        payload = request.get("payload")
        archived_output = operation in {"process_output", "history", "restore", "forget", "rename", "fork", "attachment_read", "file_read"} and isinstance(payload, dict) and "archive_id" in payload
        if isinstance(payload, dict) and "archive_id" in payload and not archived_output:
            raise ConnectorError("archived targets support inspection only")
        if archived_output:
            archive_id = checked_uuid(payload["archive_id"])
            if not Connector.archive_allowed(self, session, archive_id) and not getattr(self,"archive_supported",False):
                raise ConnectorError("archive must match a fresh exact local snapshot entry")
        elif session not in self.sessions:
            raise ConnectorError("session must match an exact nonarchived snapshot entry")
        if not isinstance(payload, dict) or payload.get("request_id") != request["request_id"]:
            raise ConnectorError("payload request_id must match request identity")
        if not isinstance(payload.get("expected_run_id"), str) or not payload["expected_run_id"]:
            raise ConnectorError("expected_run_id is required")
        if operation == "recovery_action":
            try: validate_recovery(payload,request["request_id"])
            except ValueError as error: raise ConnectorError(str(error)) from None
        if operation in voice.OPERATIONS:
            try: voice.validate(operation, payload, request["request_id"])
            except ValueError as error: raise ConnectorError(str(error)) from None
        if operation == "hooks_status":
            try: hook_health.validate(payload,request["request_id"])
            except ValueError as error: raise ConnectorError(str(error)) from None
        if operation == "history":
            try: validate_history(payload,request["request_id"])
            except ValueError as error: raise ConnectorError(str(error)) from None
        if operation == "file_read":
            try: file_reads.validate(payload, request["request_id"])
            except ValueError as error: raise ConnectorError(str(error)) from None
        if operation == "attachment_read":
            try: validate_read(payload,request["request_id"])
            except ValueError as error: raise ConnectorError(str(error)) from None
        if operation in TERMINAL_OPERATIONS:
            try:
                validate_terminal(operation, payload, request["request_id"])
            except ValueError as error:
                raise ConnectorError(str(error)) from None
        if operation in LIFECYCLE_OPERATIONS:
            try:
                validate_lifecycle(operation, payload, request["request_id"])
            except ValueError as error:
                raise ConnectorError(str(error)) from None
        if operation in CONTEXT_COMMANDS:
            try:
                (validate_clear_context if operation == "clear_context" else validate_context)(payload, request["request_id"])
            except ValueError as error:
                raise ConnectorError(str(error)) from None
        if operation in {"send_now", "settings", "process_output", "process_stop"}:
            try:
                validate_core(operation, payload, request["request_id"])
            except ValueError as error:
                raise ConnectorError(str(error)) from None
        if operation == "send":
            try:
                validate_send(payload)
            except (ValueError, TypeError):
                raise ConnectorError("invalid or oversized inline attachment/message") from None
        try:
            if len(json.dumps(payload, ensure_ascii=False).encode()) > MAX_PAYLOAD:
                raise ConnectorError("native request exceeds size limit")
        except (TypeError, ValueError):
            raise ConnectorError("invalid native payload") from None
        return operation, session, payload

    def archive_allowed(self, session, archive_id):
        current=time.monotonic()
        if current-self.snapshot_seen_at<=ARCHIVE_SNAPSHOT_TTL and (session,archive_id) in self.archives:
            return True
        return current-getattr(self,"paged_archives",{}).get((session,archive_id),-float("inf")) <= 3600

    async def archive_preflight(self, session, payload):
        archive_id=payload["archive_id"]
        if self.archive_allowed(session,archive_id):return
        # A bounded recent window cannot contain every phone's retained target.
        # Native JSON, not an arbitrary client identity, authorizes recovery.
        detail=await self.native(["archives","lookup","--json"],{"name":session,"archive_id":archive_id},timeout=5)
        if (not isinstance(detail,dict) or type(detail.get("version")) is not int or detail["version"]!=1
                or detail.get("name")!=session or detail.get("archive_id")!=archive_id
                or any(key in payload and detail.get(field)!=payload[key] for key,field in
                       (("expected_run_id","run_id"),("expected_conversation_id","conversation_id")))):
            raise ConnectorError("fresh native archive identity did not match")
        self.remember_archives([(session,archive_id)])

    def remember_archives(self, targets):
        current=time.monotonic()
        self.paged_archives={key:at for key,at in self.paged_archives.items() if current-at<=3600}
        for key in targets:self.paged_archives[key]=current
        if len(self.paged_archives)>2000:
            self.paged_archives=dict(sorted(self.paged_archives.items(),key=lambda item:item[1],reverse=True)[:2000])

    async def context_preflight(self, operation, session, payload):
        """Read fresh native state before attempting a context mutation.

        Native commands recheck identity and availability atomically themselves.
        This gate also makes old native binaries fail closed without typing.
        """
        detail = await self.native(["inspect", session])
        if (not isinstance(detail, dict) or is_archived(detail) or detail.get("name") != session
                or detail.get("run_id") != payload["expected_run_id"]
                or ("" if detail.get("conversation_id") is None else detail.get("conversation_id")) != payload["expected_conversation_id"]):
            raise ConnectorError("fresh native context identity did not match")
        if operation in CONTEXT_COMMANDS:
            eligibility = "clear_context_dismiss" if operation == "clear_context" and payload.get("dismiss_prompt") is True else operation + "_supported"
            if detail.get(eligibility) is not True:
                raise ConnectorError("native context operation is unavailable")
        elif operation == "settings":
            if detail.get("settings_change_supported") is not True:
                raise ConnectorError("native per-session settings are unavailable")
            if "expected_pending_id" in payload and detail.get("pending_settings_id") != payload["expected_pending_id"]:
                raise ConnectorError("native pending setting changed; refresh before applying")
        elif operation == "send_now":
            queue = detail.get("input_queue")
            if not isinstance(queue, dict) or queue.get("id") != payload["queue_id"] or queue.get("can_send_now") is not True:
                raise ConnectorError("native queue changed or cannot be sent now")
        else:
            compact = detail.get("compact_context_request")
            if (not isinstance(compact, dict) or compact.get("request_id") != payload["expected_compaction_id"]
                    or compact.get("run_id") != payload["expected_run_id"]
                    or compact.get("conversation_id") != payload["expected_conversation_id"]
                    or compact.get("status") != "completed"
                    or detail.get("phase") != "idle" or detail.get("activity") != "idle"):
                raise ConnectorError("compaction is not confirmed for this conversation; message was not sent")

    async def read_attachment(self, session: str, payload: dict, request_id: str) -> dict:
        """One bounded range of a receipt file of this exact conversation.

        The first range verifies identity and reads the whole file through
        native hgs; later ranges of the same exact identity reuse that brief
        in-memory copy instead of re-reading (or re-transferring from a peer).
        """
        key = attachment_reads.AttachmentReads.key(session, payload)
        entry = self.attachment_reads.get(key)
        try:
            if entry is None:
                argv = ["inspect", session]
                if "archive_id" in payload:
                    argv.extend(["--archive", payload["archive_id"]])
                argv.append("--skip-processes")
                attachment_reads.check_identity(await self.native(argv), session, payload)
                try:
                    raw = await self.native(attachment_reads.native_argv(session, payload),
                                            max_output_bytes=attachment_reads.NATIVE_OUTPUT_BYTES)
                except ConnectorError as error:
                    if str(error) != "native command failed":
                        raise
                    raise ConnectorError("attachment is not available from this conversation on the computer") from None
                entry = await asyncio.to_thread(attachment_reads.decode, raw, payload)
                self.attachment_reads.put(key, entry)
            return await asyncio.to_thread(attachment_reads.chunk, entry, session, payload, request_id)
        except attachment_reads.AttachmentReadError as error:
            raise ConnectorError(str(error)) from None

    async def read_computer_file(self, session: str, payload: dict, request_id: str) -> dict:
        # The first range always captures the current file. Following ranges
        # use only that immutable digest, never a silently replaced path.
        expected = payload.get("sha256", "")
        entry = self.file_reads.get(file_reads.key(session, payload, expected)) if expected else None
        try:
            if entry is None:
                try:
                    raw = await self.native(["file-read", session, "--json"], file_reads.native_payload(payload),
                                            max_output_bytes=attachment_reads.NATIVE_OUTPUT_BYTES)
                except ConnectorError as error:
                    if str(error) != "native command failed":
                        raise
                    raise ConnectorError("The file could not be read. Check that it still exists, is readable and is at most 10 MiB, then open it again.") from None
                entry = await asyncio.to_thread(file_reads.decode, raw, session, payload)
                self.file_reads.put(file_reads.key(session, payload, entry["sha256"]), entry)
            return await asyncio.to_thread(file_reads.chunk, entry, session, payload, request_id)
        except attachment_reads.AttachmentReadError as error:
            raise ConnectorError(str(error)) from None

    async def child_send_preflight(self, session, payload):
        if not session.startswith("dsh/"):
            raise ConnectorError("this native provider has no child-send ABI")
        parent = await self.native(["inspect", session, "--skip-processes"])
        if (not isinstance(parent, dict) or is_archived(parent) or parent.get("name") != session
                or parent.get("run_id") != payload["expected_run_id"] or parent.get("conversation_id") != payload["expected_conversation_id"]
                or not isinstance(parent.get("subagents"), dict) or payload["agent_id"] not in parent["subagents"]):
            raise ConnectorError("child does not belong to the exact selected parent")
        child = await self.native(["inspect", session, "--agent", payload["agent_id"], "--skip-processes"])
        if (not isinstance(child, dict) or is_archived(child) or child.get("name") != session
                or child.get("run_id") != payload["expected_run_id"] or child.get("parent_conversation_id") != payload["expected_conversation_id"]
                or child.get("agent_id") != payload["agent_id"] or child.get("conversation_id") != payload["expected_conversation_id"] + "/" + payload["agent_id"]
                or child.get("send_supported") is not True):
            raise ConnectorError("native child cannot accept messages under this exact parent")

    async def execute(self, request: dict) -> dict:
        if not isinstance(request, dict):
            raise ConnectorError("invalid request")
        request_id = checked_uuid(request.get("request_id"))
        operation = request.get("operation")
        journal_operation = operation if isinstance(operation, str) else "invalid"
        try:
            request_hash = hashlib.sha256(json.dumps(request, sort_keys=True, separators=(",", ":"), allow_nan=False).encode()).hexdigest()
        except (TypeError, ValueError, RecursionError):
            raise ConnectorError("invalid native request JSON") from None
        if not self.journal.claim(request_id, journal_operation, request_hash):
            return self.journal.replay(request_id)
        try:
            if operation == "directory_create" and "gateway_route" in request:
                route = request["gateway_route"]
                if (not isinstance(route, dict) or ("computer_id" in request
                        and route.get("computer_id") != request["computer_id"])):
                    raise ConnectorError("directory creation computer does not match its immutable route")
            target, machine_id = await self.fleet.resolve(request.get("gateway_route"),
                                                       present="gateway_route" in request)
        except asyncio.CancelledError:
            self.journal.finish(request_id, {"state": "failed", "result": None,
                "error": "connector stopped before native route resolution"})
            raise
        except ConnectorError as error:
            response = {"state": "failed", "result": None, "error": str(error)}
            self.journal.finish(request_id, response)
            return response
        token = LOCAL_MACHINE_GUARD.set(machine_id)
        try:
            return await target._execute_claimed(request)
        finally:
            LOCAL_MACHINE_GUARD.reset(token)

    async def _execute_claimed(self, request: dict) -> dict:
        request_id = request["request_id"]
        mutation_started = False
        try:
            operation, session, payload = await asyncio.to_thread(self.validate, request)
            if operation == "clear_context" and "dismiss_prompt" in payload and not getattr(self, "clear_context_dismiss_supported", False):
                raise ConnectorError("native clear-context dismiss ABI is unavailable")
            if isinstance(payload,dict) and "archive_id" in payload and getattr(self,"archive_supported",False):
                await self.archive_preflight(session,payload)
            if operation in {*CONTEXT_COMMANDS, "settings", "send_now"} or (operation == "send" and payload.get("expected_compaction_id")):
                await self.context_preflight(operation, session, payload)
            if operation == "send" and "agent_id" in payload:
                await self.child_send_preflight(session, payload)
            if operation == "hooks_status" and not getattr(self,"hook_health_supported",False):
                raise ConnectorError(hook_health.UNAVAILABLE)
            if operation == "history" and not self.history_supported:
                raise ConnectorError("native public history ABI is unavailable")
            if operation in LIFECYCLE_OPERATIONS and not self.lifecycle_supported:
                raise ConnectorError("native scoped lifecycle ABI is unavailable")
            if operation in TERMINAL_OPERATIONS:
                if not self.terminal_supported:
                    raise ConnectorError("native scoped Terminal ABI is unavailable")
                if operation == "terminal_input":
                    deadline = request.get("expires_at")
                    if type(deadline) not in (int, float) or not math.isfinite(deadline) or deadline <= time.time() or deadline > time.time()+6:
                        raise ConnectorError("terminal input expired before native delivery")
                    base = {key: payload[key] for key in ("request_id", "expected_run_id", "expected_conversation_id")}
                    fresh = await self.native(["terminal", session, "--json"], {**base, "action": "snapshot"}, timeout=3)
                    if (not isinstance(fresh, dict) or fresh.get("name") != session or fresh.get("run_id") != payload["expected_run_id"]
                            or fresh.get("conversation_id") != payload["expected_conversation_id"] or fresh.get("terminal_binding_id") != payload["terminal_binding_id"]
                            or fresh.get("input_supported") is not True or ("\n" in payload.get("text", "") and fresh.get("multiline_supported") is not True)):
                        raise ConnectorError("native Terminal binding or input support changed")
                    if deadline <= time.time():
                        raise ConnectorError("terminal input expired before native delivery")
            if operation in analytics.OPERATIONS and not self.analytics_supported:
                raise ConnectorError("native analytics read ABI is unavailable")
            if operation in voice.OPERATIONS and not self.voice_supported:
                raise ConnectorError("native voice message ABI is unavailable")
            if operation == "voice_submit" and "video" in payload and not self.video_supported:
                raise ConnectorError("native video message ABI is unavailable")
            if operation in speech_config.OPERATIONS and not self.speech_config_supported:
                raise ConnectorError("native speech profile management ABI is unavailable")
            if operation in speech.OPERATIONS and not self.speech_supported:
                raise ConnectorError("native speech ABI is unavailable")
            if operation in LAUNCH_OPERATIONS - {"directory_create"} and not self.launch_supported:
                raise ConnectorError("native exact launch-ID ABI is unavailable")
            if operation == "directory_create" and not self.directory_create_supported:
                raise ConnectorError("native explicit directory creation ABI is unavailable")
            if operation in COMPUTER_OPERATIONS and (not self.preference_supported or getattr(self, "fleet", None) is None):
                raise ConnectorError("the shared delivery setting is changed on the gateway computer with a current hgs")
            if operation in worktrees.OPERATIONS and not self.worktree_supported:
                raise ConnectorError("native mobile worktree ABI is unavailable")
            if operation == session_notifications.OPERATION and not getattr(self, "session_notifications_supported", False):
                raise ConnectorError("native shared notification ABI is unavailable")
            if operation == "mark_read" and not self.read_supported:
                raise ConnectorError("native shared read ABI is unavailable")
            if operation == "file_read" and not self.file_read_supported:
                raise ConnectorError("native computer file read ABI is unavailable")
            if operation == "attachment_read" and not self.attachment_read_supported:
                raise ConnectorError("native attachment read ABI is unavailable")
            if operation == "archive_list" and not self.archive_supported:
                raise ConnectorError("Update the computer CLI to browse archive pages")
            if operation == "directory_create":
                from pathlib import PurePosixPath
                parent_result = await self.native(["dirs", str(PurePosixPath(payload["path"]).parent)], timeout=5)
                if not isinstance(parent_result, dict):
                    raise ConnectorError("native directory parent catalog is invalid")
                try: confirmed_parent = canonical_directory(parent_result.get("path"))
                except ValueError as error: raise ConnectorError(str(error)) from None
            if operation == "worktree_create":
                from pathlib import PurePosixPath
                destination = PurePosixPath(payload["destination"])
                if destination.name in ("", ".", ".."):
                    raise ConnectorError("invalid worktree destination")
                parent = await self.native(["dirs", str(destination.parent)], timeout=5)
                confirmed_destination = str(PurePosixPath(worktrees.absolute(parent.get("path"))) / destination.name)
                fresh = worktrees.catalog(await self.native(["worktrees", "--path", payload['path'], "--refresh", "--json"], timeout=5), payload['path'])
                if fresh.get('state') != 'ok' or fresh.get('stale') or fresh.get('common_dir') != payload['common_dir']:
                    raise ConnectorError("repository changed; refresh worktrees before creation")
            launch_directory = None
            if operation == "launch":
                try: accounts = project_catalog(await self.native(["account", "ls"], timeout=5))
                except ValueError as error: raise ConnectorError(str(error)) from None
                if payload["agent"] not in accounts["agents"] or ("account_id" in payload and not any(row["id"] == payload["account_id"] and row["provider"] == payload["agent"] for row in accounts["accounts"])):
                    raise ConnectorError("selected native provider/account is unavailable")
                if "model" in payload:
                    account = next((row for row in accounts["accounts"] if row["id"] == payload["account_id"] and row["provider"] == payload["agent"]), None)
                    if account is None or not any(row["value"] == payload["model"] for row in account.get("model_options", [])):
                        raise ConnectorError("selected native model is no longer offered by this account")
                directory = await self.native(["dirs", payload["directory"]], timeout=5)
                if not isinstance(directory, dict) or not isinstance(directory.get("path"), str) or not Path(directory["path"]).is_absolute():
                    raise ConnectorError("native directory is unavailable")
                launch_directory = directory["path"]
                if "project_id" in payload:
                    if not self.project_launch_supported:
                        raise ConnectorError("native scoped project assignment is unavailable")
                    try:
                        project_data = launch_projects(await self.native(["swarm", "get"], timeout=3))
                        if "project_folder_id" in payload:
                            selected_project = next((row for row in project_data["projects"] if row["id"] == payload["project_id"]), None)
                            selected_folder = next((row for row in selected_project["folders"] if row["id"] == payload["project_folder_id"]), None) if selected_project else None
                            if selected_folder is None:
                                raise ValueError("selected local project folder changed")
                            checked_folder = await self.native(["dirs", selected_folder["path"]], timeout=5)
                            if not isinstance(checked_folder, dict) or checked_folder.get("path") != launch_directory:
                                raise ValueError("selected local project folder changed")
                            selected_folder["path"] = checked_folder["path"]
                        worktree_data = None
                        if "worktree_folder_id" in payload:
                            if not self.worktree_supported: raise ValueError("native verified worktree placement is unavailable")
                            anchor = next((row for project in project_data['projects'] if project['id'] == payload['project_id']
                                           for row in project['folders'] if row['id'] == payload['worktree_folder_id']), None)
                            if anchor is None: raise ValueError("selected local project anchor changed")
                            checked = await self.native(["dirs", anchor['path']], timeout=5)
                            anchor['path'] = worktrees.absolute(checked.get('path'))
                            worktree_data = worktrees.catalog(await self.native(["worktrees", "--path", anchor['path'], "--refresh", "--json"], timeout=5), anchor['path'])
                        project_choice(project_data, payload, launch_directory, worktree_data)
                    except ValueError as error:
                        raise ConnectorError(str(error)) from None
            if operation == "recovery_action":
                if not self.recovery_supported: raise ConnectorError("native scoped recovery ABI is unavailable")
                fresh=await self.native(["inspect",session,"--skip-processes"])
                job=fresh.get("recovery") if isinstance(fresh,dict) else None
                if (not isinstance(fresh,dict) or fresh.get("name") != session or is_archived(fresh)
                        or fresh.get("run_id") != payload["expected_run_id"] or fresh.get("conversation_id") != payload["expected_conversation_id"]
                        or not isinstance(job,dict) or job.get("id") != payload["job_id"] or job.get("state") != "waiting"
                        or not isinstance(job.get("identity"),list) or len(job["identity"]) != 5
                        or job["identity"][:2] != [payload["expected_run_id"],payload["expected_conversation_id"]]):
                    raise ConnectorError("native recovery job changed; refresh before action")
            mutation_started = operation in MUTATIONS
            if operation == "catalog":
                try: result = project_catalog(await self.native(["account", "ls"], timeout=5))
                except ValueError as error: raise ConnectorError(str(error)) from None
                result["project_launch_supported"] = False
                result["worktree_supported"] = self.worktree_supported
                result["directory_create_supported"] = self.directory_create_supported
                if self.project_launch_supported:
                    try:
                        result.update(launch_projects(await self.native(["swarm", "get"], timeout=3)))
                    except (ConnectorError, ValueError):
                        pass
                result["request_id"] = request_id
            elif operation == "archive_list":
                try: result=archives.page(await self.native(["archives","--json"],payload,timeout=30),payload)
                except ValueError as error: raise ConnectorError(str(error)) from None
                groups=self.project_cache or unavailable_projects(self.project_fallback_id)
                apply_memberships(result,groups)
                used={row.get("mobile_project_id") for row in result["sessions"]}
                result["mobile_projects"]={k:v for k,v in groups.items() if k!="projects"}
                result["mobile_projects"]["projects"]=[{k:v for k,v in p.items() if k in {"id","name","color"}} for p in groups.get("projects",[]) if p.get("id") in used]
                result["request_id"]=request_id
            elif operation == "dirs":
                result = await self.native(["dirs", payload["path"]], timeout=5)
                if not isinstance(result, dict) or not isinstance(result.get("path"), str) or not isinstance(result.get("directories"), list):
                    raise ConnectorError("native directory catalog is invalid")
                result = {key: result[key] for key in ("path", "parent", "home", "directories", "truncated") if key in result}
                result["request_id"] = request_id
            elif operation == "worktrees":
                result = worktrees.catalog(await self.native(["worktrees", "--path", payload['path'], "--refresh", "--json"], timeout=5), payload['path'])
                result['request_id'] = request_id
            elif operation == "directory_create":
                argv = ["dirs", "--create", payload["path"], "--request-id", request_id]
                raw = await self.native(argv, timeout=5)
                try: result = directory_created(raw, payload, request_id, confirmed_parent)
                except ValueError as error: raise ConnectorError(str(error)) from None
            elif operation == "worktree_create":
                argv = ["worktrees", "create", "--path", payload['path'], "--common-dir", payload['common_dir'],
                        "--branch", payload['branch'], "--base", payload['base'], "--destination", payload['destination'], "--request-id", request_id, "--json", "--mobile"]
                result = worktrees.created(await self.native(argv, timeout=35), payload, request_id, confirmed_destination)
            elif operation == "launch":
                argv = [payload["agent"], launch_directory, "--new", "-n", payload["tag"], "-d", "--launch-id", request_id]
                if "account_id" in payload: argv.extend(["--account", payload["account_id"]])
                if "model" in payload: argv.extend(["--model", payload["model"]])
                await self.native(argv, timeout=15, json_output=False)
                target = None
                until = time.monotonic()+5
                while time.monotonic() < until:
                    snapshot = await self.native(["ls", "--json", "--local"] + (["--no-archives"] if self.archive_supported else []), timeout=3)
                    matches = [row for row in snapshot.get("sessions", []) if isinstance(row, dict) and row.get("launch_id") == request_id and not is_archived(row)] if isinstance(snapshot, dict) else []
                    if len(matches) == 1 and all(isinstance(matches[0].get(key), str) and matches[0][key] for key in ("name", "run_id")) and (matches[0].get("conversation_id") is None or isinstance(matches[0]["conversation_id"],str)):
                        row = matches[0]
                        target = {"name": row["name"], "run_id": row["run_id"], "conversation_id": row.get("conversation_id") or ""}
                        break
                    await asyncio.sleep(.1)
                if target is None: raise ConnectorError("native launch could not be verified by exact launch UUID; check computers before creating another")
                result = {"request_id": request_id, "status": "created", "result_target": target}
                if "project_id" in payload:
                    assignment_payload = {"request_id": request_id, "name": target["name"],
                        "expected_run_id": target["run_id"], "expected_conversation_id": target["conversation_id"],
                        "swarm_id": payload["swarm_id"], "project_id": payload["project_id"],
                        "directory": launch_directory, "add_folder": payload["add_folder"]}
                    for field in ('worktree_folder_id', 'worktree_common_dir'):
                        if field in payload: assignment_payload[field] = payload[field]
                    if "project_folder_id" in payload:
                        assignment_payload["project_folder_id"] = payload["project_folder_id"]
                    assignment = {"status": "uncertain", "swarm_id": payload["swarm_id"], "project_id": payload["project_id"]}
                    try:
                        raw = await self.native(["swarm", "assign-launch", "--json"], assignment_payload, timeout=5)
                        if (isinstance(raw, dict) and raw.get("request_id") == request_id
                                and raw.get("name") == target["name"] and raw.get("run_id") == target["run_id"]
                                and (raw.get("conversation_id") == target["conversation_id"] or not target["conversation_id"] and isinstance(raw.get("conversation_id"), str))
                                and raw.get("swarm_id") == payload["swarm_id"] and raw.get("project_id") == payload["project_id"]
                                and raw.get("status") in ("assigned", "failed", "uncertain")):
                            assignment["status"] = raw["status"]
                            if raw["status"] == "assigned":
                                self.project_next_poll = 0
                            if raw["status"] == "assigned" and not target["conversation_id"]:
                                target["conversation_id"] = raw["conversation_id"]
                    except (ConnectorError, ValueError):
                        pass
                    result["project_assignment"] = assignment
                    if assignment["status"] != "assigned":
                        result["warning"] = "The session was created, but its project assignment is " + assignment["status"] + ". Review Projects; this launch will not be repeated."
            elif operation == "inspect":
                argv = ["inspect", session]
                if payload is not None and "archive_id" in payload:
                    argv.extend(["--archive", payload["archive_id"]])
                if payload is not None and "agent_id" in payload:
                    # Parent membership and immutable identity are checked
                    # before the native child inspector enforces its roster.
                    parent_argv = ["inspect", session]
                    if "archive_id" in payload:
                        parent_argv.extend(["--archive", payload["archive_id"]])
                    parent = await self.native(parent_argv)
                    if (not isinstance(parent, dict) or parent.get("name") != session
                            or parent.get("run_id") != payload["expected_run_id"]
                            or parent.get("conversation_id") != payload["expected_conversation_id"]
                            or parent.get("archive_id", "") != payload.get("archive_id", "")
                            or not isinstance(parent.get("subagents"), dict) or payload["agent_id"] not in parent["subagents"]):
                        raise ConnectorError("subagent does not belong to the exact selected parent")
                    argv.extend(["--agent", payload["agent_id"], "--skip-processes"])
                if payload is not None and "after" in payload:
                    argv.extend(["--after", str(payload["after"])])
                result = await self.native(argv)
                if payload is not None and "archive_id" in payload and (not isinstance(result, dict) or result.get("archive_id") != payload["archive_id"] or result.get("name") != session):
                    raise ConnectorError("native archive inspection identity did not match")
                if (payload is None or "archive_id" not in payload) and (not isinstance(result, dict) or is_archived(result) or ("name" in result and result["name"] != session)):
                    raise ConnectorError("native ordinary inspection returned a different or archived target")
                if payload and "expected_run_id" in payload:
                    conversation = result.get("parent_conversation_id") if "agent_id" in payload else result.get("conversation_id")
                    if result.get("run_id") != payload["expected_run_id"] or conversation != payload["expected_conversation_id"]:
                        raise ConnectorError("native inspection run/conversation identity did not match")
                if payload and "agent_id" in payload and (result.get("agent_id") != payload["agent_id"] or result.get("conversation_id") != payload["expected_conversation_id"] + "/" + payload["agent_id"]):
                    raise ConnectorError("native subagent inspection identity did not match")
                try:
                    # GET's device envelope also adds request_id; leave room for
                    # that metadata below the ordinary one-MiB response bound.
                    result = await asyncio.to_thread(bound_inspection, result, MAX_RESULT - 1024)
                except ValueError as error:
                    raise ConnectorError(str(error)) from None
            elif operation == "recovery_action":
                result=await self.native(["recovery","action","--scoped-json"],{**payload,"name":session})
                if (not isinstance(result,dict) or result.get("request_id") != request_id or result.get("name") != session
                        or result.get("run_id") != payload["expected_run_id"] or result.get("conversation_id") != payload["expected_conversation_id"]
                        or result.get("job_id") != payload["job_id"] or result.get("action") != payload["action"]
                        or result.get("status") not in {"scheduled","cancelled","failed","uncertain"}):
                    raise ConnectorError("native recovery acknowledgement identity did not match")
            elif operation == "history":
                # Head indexing makes bounded native progress locally rather
                # than requiring one network roundtrip for each two-MiB chunk.
                until=time.monotonic()+2
                result=await self.native(["history",session,"--json"],payload,timeout=2)
                if not {"before","after","around","around_incoming_seq"} & set(payload):
                    for _ in range(31):
                        if not isinstance(result,dict) or result.get("indexing") is not True: break
                        remaining=until-time.monotonic()
                        if remaining<=0: break
                        previous=json.dumps([result.get("source_status"),result.get("head"),result.get("events")],sort_keys=True)
                        try: following=await self.native(["history",session,"--json"],payload,timeout=remaining)
                        except ConnectorError: break
                        following_progress=json.dumps([following.get("source_status"),following.get("head"),following.get("events")],sort_keys=True) if isinstance(following,dict) else None
                        result=following
                        if following_progress==previous: break
                if (not isinstance(result,dict) or result.get("request_id") != request_id or result.get("name") != session
                        or result.get("run_id") != payload["expected_run_id"]
                        or result.get("archive_id","") != payload.get("archive_id","")
                        or result.get("agent_id","") != payload.get("agent_id","")
                        or (result.get("parent_conversation_id") if "agent_id" in payload else result.get("conversation_id")) != payload["expected_conversation_id"]
                        or ("agent_id" in payload and result.get("conversation_id") != payload["expected_conversation_id"]+"/"+payload["agent_id"])
                        or not isinstance(result.get("events"),list) or not isinstance(result.get("history_epoch"),str)
                        or not isinstance(result.get("head"),dict)):
                    raise ConnectorError("native public history identity or envelope did not match")
            elif operation in TERMINAL_OPERATIONS:
                native_payload = {**payload, "action": "snapshot" if operation == "terminal_snapshot" else "input"}
                if operation == "terminal_input":
                    native_payload["expires_at"] = request["expires_at"]
                result = await self.native(["terminal", session, "--json"], native_payload, timeout=3)
                statuses = {"snapshot"} if operation == "terminal_snapshot" else {"submitted", "failed", "uncertain"}
                if (not isinstance(result, dict) or result.get("request_id") != request_id or result.get("name") != session
                        or result.get("run_id") != payload["expected_run_id"] or result.get("conversation_id") != payload["expected_conversation_id"]
                        or result.get("status") not in statuses or (operation == "terminal_input" and result.get("terminal_binding_id") != payload["terminal_binding_id"])):
                    raise ConnectorError("native Terminal acknowledgement identity or status did not match")
            elif operation in LIFECYCLE_OPERATIONS:
                result = await self.native(["session-action", session, "--json"], {**payload, "action": operation})
                if (not isinstance(result, dict) or result.get("request_id") != request_id or result.get("name") != session
                        or result.get("run_id") != payload["expected_run_id"] or result.get("conversation_id") != payload["expected_conversation_id"]
                        or result.get("status") not in {"completed", "failed", "uncertain"}):
                    raise ConnectorError("native lifecycle acknowledgement identity or status did not match")
                if "archive_id" in payload:
                    if result.get("archive_id",payload["archive_id"]) != payload["archive_id"]:
                        raise ConnectorError("native lifecycle archive identity did not match")
                    result["archive_id"] = payload["archive_id"]
                target = result.get("result_target")
                if target is not None and (not isinstance(target, dict) or set(target) - {"name", "run_id", "conversation_id", "archive_id"}
                        or not all(isinstance(target.get(field), str) for field in ("name", "run_id", "conversation_id"))
                        or not target["name"] or not target["run_id"]):
                    raise ConnectorError("native lifecycle result target was invalid")
                if result["status"] == "completed" and target is not None and not target.get("archive_id"):
                    # An authoritative action target can be inspected immediately
                    # while the normal heartbeat catalog catches up. Native
                    # inspection still enforces the requested run/conversation.
                    self.sessions.add(target["name"])
                    if operation == "rename" and target["name"] != session:
                        self.sessions.discard(session)
            elif operation in voice.OPERATIONS:
                result = await self.native(["voice", session, "--json"], {"action": voice.ACTIONS[operation], **payload}, timeout=30)
                if not voice.result_valid(operation, result, payload):
                    raise ConnectorError("native voice acknowledgement did not match")
            elif operation in analytics.OPERATIONS:
                result = await self.native(["analytics", "--json"], {**payload, "action": "summary"},
                                           timeout=10, max_output_bytes=analytics.MAX_OUTPUT_BYTES)
                route = request.get("gateway_route")
                machine = route.get("machine_id") if isinstance(route, dict) else getattr(getattr(self, "fleet", None), "local_id", None)
                if not analytics.result_valid(result, payload, machine):
                    raise ConnectorError("native analytics summary did not match")
            elif operation in speech_config.OPERATIONS:
                try:
                    result = await self.native(["speech", "--json"], {"action": speech_config.ACTIONS[operation], **payload}, timeout=35, failure_json=True)
                except ConnectorError:
                    # CLI/SSH stderr can contain provider configuration. Keep
                    # it out of durable relay/connector receipts and UI errors.
                    raise ConnectorError("speech configuration outcome could not be confirmed; check operation status") from None
                if not speech_config.result_valid(operation, result, payload):
                    raise ConnectorError("native speech configuration acknowledgement did not match")
            elif operation in speech.OPERATIONS:
                result = await self.native(["speech", "--json"], {"action": speech.ACTIONS[operation], **payload}, timeout=10)
                if not speech.result_valid(operation, result, payload):
                    raise ConnectorError("native speech acknowledgement did not match")
                if operation in {"speech_submit", "speech_cancel"}:
                    result = {key: value for key, value in result.items() if key != "text"}
            elif operation == "hooks_status":
                result = await self.native(["hooks",session,"--json"],payload,timeout=5,max_output_bytes=16*1024)
                if not hook_health.result_valid(result,payload,session):
                    raise ConnectorError("native hook health acknowledgement did not match")
            elif operation == "set_mobile_delivery":
                result = await self.native(["swarm", "preference", "set", "mobile_delivery", payload["value"], "--json"], timeout=10)
                if not isinstance(result, dict) or result.get("ok") is not True or result.get("mobile_delivery") != payload["value"]:
                    raise ConnectorError("native delivery setting acknowledgement did not match")
            elif operation == "file_read":
                result = await self.read_computer_file(session, payload, request_id)
            elif operation == "attachment_read":
                result = await self.read_attachment(session, payload, request_id)
            elif operation == session_notifications.OPERATION:
                result = await self.native(["notifications", session, "--json"], payload, failure_json=True)
                if not session_notifications.result_valid(result, payload):
                    raise ConnectorError("native notification preference acknowledgement did not match")
            elif operation == "mark_read":
                result = await self.native(["read", session, "--json"], payload, failure_json=True)
                if not mark_read_result(result, payload):
                    raise ConnectorError("native read acknowledgement did not match")
            elif operation in {"process_output", "process_stop"}:
                argv = ["processes", session, "--output" if operation == "process_output" else "--stop", payload["process_id"],
                        "--run", payload["expected_run_id"], "--conversation", payload["expected_conversation_id"]]
                for field in ("generation", "archive_id"):
                    if field in payload:
                        argv.extend(["--" + field.replace("_id", ""), payload[field]])
                result = await self.native(argv)
                if (not isinstance(result, dict) or result.get("id") != payload["process_id"] or result.get("run_id") != payload["expected_run_id"]
                        or result.get("conversation_id") != payload["expected_conversation_id"]):
                    raise ConnectorError("native process acknowledgement identity did not match")
                result = {**result, "request_id": request_id, "name": session, "process_id": payload["process_id"]}
                if "archive_id" in payload:
                    result["archive_id"] = payload["archive_id"]
            else:
                result = await self.native([CONTEXT_COMMANDS.get(operation, "send-now" if operation == "send_now" else operation), session, "--json"], payload)
                if operation == "send" and "agent_id" in payload:
                    if (not isinstance(result, dict) or result.get("request_id") != request_id or result.get("name") != session
                            or result.get("run_id") != payload["expected_run_id"] or result.get("conversation_id") != payload["expected_conversation_id"]
                            or result.get("agent_id") != payload["agent_id"] or result.get("status") != "submitted"):
                        raise ConnectorError("native child send acknowledgement identity did not match")
                if operation in {*CONTEXT_COMMANDS, "settings", "send_now"}:
                    statuses = {"submitted", "confirmed"} if operation == "clear_context" else {"applied", "scheduled"} if operation == "settings" else {"submitted"}
                    if (not isinstance(result, dict) or result.get("request_id") != request_id
                            or result.get("name") != session or result.get("run_id") != payload["expected_run_id"]
                            or result.get("conversation_id") != payload["expected_conversation_id"]
                            or result.get("status") not in statuses):
                        raise ConnectorError("native context acknowledgement identity or status did not match")
                    if operation == "send_now" and result.get("queue_id") != payload["queue_id"]:
                        raise ConnectorError("native queue acknowledgement identity did not match")
                    if operation == "settings" and any(result.get(field) != payload[field] for field in ("model", "effort") if field in payload):
                        raise ConnectorError("native settings acknowledgement did not match")
                    if operation == "settings" and result["status"] == "scheduled":
                        checked_uuid(result.get("pending_settings_id"))
            native_state = result["status"] if operation in LIFECYCLE_OPERATIONS or operation in {"terminal_input","recovery_action"} else "completed"
            response = {"state": "completed" if native_state in {"submitted","scheduled","cancelled"} else native_state,
                        "result": result, "error": result.get("error") if (operation in LIFECYCLE_OPERATIONS or operation in {"terminal_input","recovery_action"}) and result["status"] in {"failed", "uncertain"} else None}
            if len(json.dumps(response).encode()) > MAX_RESULT:
                raise ConnectorError("native result exceeds relay size limit")
            if isinstance(payload,dict) and "archive_id" in payload and response["state"]=="completed":
                key=(session,payload["archive_id"])
                if operation in {"restore","forget"}: self.paged_archives.pop(key,None)
                elif operation in READ_OPERATIONS and key in self.paged_archives: self.paged_archives[key]=time.monotonic()
            if operation=="archive_list":
                self.remember_archives((row["name"],row["archive_id"]) for row in result["sessions"])
        except ConnectorError as error:
            response = {"state": "uncertain" if mutation_started else "failed",
                        "result": None, "error": str(error)}
        except asyncio.CancelledError:
            self.journal.finish(request_id, {
                "state": "uncertain" if mutation_started else "failed", "result": None,
                "error": "connector stopped before recording the native result"})
            raise
        except Exception:
            response = {"state": "uncertain" if mutation_started else "failed",
                        "result": None, "error": "connector could not record native outcome"}
        self.journal.finish(request_id, response)
        return response

    async def http(self, client: aiohttp.ClientSession, method: str, path: str,
                   body: dict | None = None, *, max_response_bytes: int = MAX_BYTES) -> object:
        try:
            async with client.request(method, self.server_url + path, json=body,
                                      headers={"Authorization": "Bearer " + self.token},
                                      allow_redirects=False) as response:
                if response.status < 200 or response.status >= 300:
                    raise RelayError(response.status)
                data = bytearray()
                async for chunk in response.content.iter_chunked(65536):
                    data.extend(chunk)
                    if len(data) > max_response_bytes:
                        raise ConnectorError("relay response exceeds size limit")
                if not data:
                    return None
                try:
                    return await asyncio.to_thread(json.loads, data) if len(data) > 1024 * 1024 else json.loads(data)
                except (ValueError, UnicodeError):
                    raise ConnectorError("relay returned invalid JSON") from None
        except (aiohttp.ClientError, asyncio.TimeoutError):
            raise ConnectorError("relay is unavailable") from None

    async def flush(self, client: aiohttp.ClientSession) -> None:
        self.journal.prune()
        for request_id, response in self.journal.pending():
            try:
                await self.http(client, "POST", f"/v1/node/requests/{request_id}/result", response)
            except RelayError as error:
                if error.status not in {404, 409}:
                    raise
                # Relay expiry/restart can retire a claim. Preserve the local
                # native outcome and its no-replay tombstone, retire only upload.
                LOG.warning("Relay retired a result claim; preserving local outcome")
            self.journal.delivered(request_id)

    async def heartbeats(self, client: aiohttp.ClientSession) -> None:
        delay = self.interval
        while True:
            try:
                snapshot = await self.snapshot()
                body = self.fleet.heartbeat(snapshot)
                try:
                    await self.post_with_presence(client, "/v1/node/heartbeat", body, self.presence)
                except RelayError as error:
                    if len(body) == 1 or error.status not in {400, 404, 422}:
                        raise
                    self.fleet.relay_supported = False
                    snapshot["mobile_capabilities"]["features"] = [feature for feature in
                        snapshot["mobile_capabilities"]["features"] if feature != "gateway_one_hop"]
                    await self.post_with_presence(client, "/v1/node/heartbeat", {"snapshot": snapshot}, self.presence)
                else:
                    self.fleet.confirm_manifest(body)
                delay = self.interval
            except ConnectorError as error:
                LOG.warning("Heartbeat: %s", error)
                delay = min(60, max(self.interval, delay * 2))
            await asyncio.sleep(delay)

    async def post_with_presence(self, client: aiohttp.ClientSession, path: str, body: dict, presence) -> None:
        """Send presence beside a snapshot. An older relay rejects the additive
        field with 400/422; retry without it and probe again after five minutes."""
        if time.monotonic() < self.presence_retry_at:
            await self.http(client, "POST", path, body)
            return
        try:
            await self.http(client, "POST", path, {**body, "presence": presence})
        except RelayError as error:
            if error.status not in {400, 422}:
                raise
            await self.http(client, "POST", path, body)
            # Only a plain success proves the field, not other body content, was refused.
            self.presence_retry_at = time.monotonic() + 300

    async def requests(self, client: aiohttp.ClientSession) -> None:
        delay = 1
        while True:
            try:
                await self.flush(client)
                await self.snapshot_ready.wait()
                pending_capability = not self.fleet.probe_complete or (
                    self.fleet.supported and self.fleet.relay_supported and self.fleet.local_id is None)
                wait = 1 if pending_capability else 25
                batch = await self.http(client, "GET", f"/v1/node/requests?wait={wait}" + ("&gateway_one_hop=1" if self.fleet.available else ""), max_response_bytes=MAX_PAYLOAD + 4096)
                if not isinstance(batch, dict) or not isinstance(batch.get("requests"), list):
                    raise ConnectorError("relay returned invalid request list")
                if len(batch["requests"]) > 100:
                    raise ConnectorError("relay request list exceeds size limit")
                for request in batch["requests"]:
                    try:
                        await self.execute(request)
                    except ConnectorError as error:
                        LOG.warning("Rejected relay request: %s", error)
                await self.flush(client)
                delay = 1
                if not batch["requests"]:
                    await asyncio.sleep(0.25)
            except ConnectorError as error:
                LOG.warning("Requests: %s", error)
                await asyncio.sleep(delay)
                delay = min(60, delay * 2)

    async def run(self) -> None:
        async with aiohttp.ClientSession(timeout=aiohttp.ClientTimeout(total=35), trust_env=False) as client:
            tasks = [asyncio.create_task(self.heartbeats(client)),
                     asyncio.create_task(self.requests(client)),
                     asyncio.create_task(self.fleet.run(client))]
            try:
                await asyncio.gather(*tasks)
            finally:
                for task in tasks:
                    task.cancel()
                await asyncio.gather(*tasks, return_exceptions=True)
                await self.accounts.close()
                await self.fleet.close()


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", required=True)
    parser.add_argument("--allow-insecure-localhost", action="store_true")
    args = parser.parse_args()
    logging.basicConfig(level=logging.INFO, format="%(levelname)s: %(message)s")
    connector = None
    try:
        config = json.loads(Path(args.config).read_text())
        if not isinstance(config, dict):
            raise ConnectorError("configuration must be a JSON object")
        connector = Connector(config, allow_insecure_localhost=args.allow_insecure_localhost)
        asyncio.run(connector.run())
    except (OSError, ValueError, ConnectorError):
        LOG.error("Connector could not start; verify private configuration and journal availability")
        raise SystemExit(1) from None
    except KeyboardInterrupt:
        pass
    finally:
        if connector is not None:
            connector.journal.close()


if __name__ == "__main__":
    main()
