"""Pooled asynchronous PostgreSQL state and transactional relay admission.

Only admission and retention touch the global payload ledger. Claims, polling,
authentication and receipt reads never acquire that global lock. Notifications
contain identities only and are hints: all delivery decisions read committed SQL.
"""

from __future__ import annotations

import asyncio
from concurrent.futures import ThreadPoolExecutor
import functools
import hashlib
import json
import time
import uuid

import asyncpg

from .async_store import Notifications
from .attachments import MAX_QUEUE_BYTES
from .context import (
    ATTACHMENT_READ_GRACE,
    ATTACHMENT_RESULT_TTL,
    CAPABILITY_OPERATIONS,
    READ_OPERATIONS,
    READ_SQL,
    inspect_features,
    supports,
)
from .postgres_schema import SCHEMA
from .projects import is_archived
from .store import Store, canonical, digest, token
from .work import EXECUTOR
from .routes import Registry, RouteError, drive_async, MAX_PEER_SNAPSHOT_BYTES

RESULT_RESERVE = 1024 * 1024
RECEIPT_COLUMNS = "id,device_id,node_id,operation,body_hash,state,result,error,created,claimed,updated,expires_at"
AUTH_COLUMNS = "id,workspace_id,name,revoked"
TERMINAL = ("completed", "failed", "uncertain")


class BoundedAcquire:
    def __init__(self, owner, timeout, cleanup=False):
        self.owner, self.timeout = owner, timeout
        self.slots = owner._cleanup_slots if cleanup else owner._slots
        self.context = None
        self.cleanup = cleanup

    def _release(self):
        self.slots.release()
        if self.cleanup:
            self.owner._cleanup_in_use -= 1

    async def __aenter__(self):
        from .async_store import StoreBusy

        if self.slots.locked():
            raise StoreBusy("PostgreSQL query capacity exhausted")
        await self.slots.acquire()
        if self.cleanup:
            self.owner._cleanup_in_use += 1
        try:
            self.context = self.owner._pool.acquire(timeout=self.timeout)
            return await self.context.__aenter__()
        except BaseException:
            self._release()
            raise

    async def __aexit__(self, *args):
        try:
            return await self.context.__aexit__(*args)
        finally:
            self._release()


class BoundedPool:
    """Bound pending queries and apply deadlines to every pool acquisition."""

    def __init__(self, pool):
        self._pool = pool
        self._slots = asyncio.Semaphore(pool.get_max_size() + 64)
        # Active poll admission bounds disconnect cleanup; reserve another
        # maintenance batch so normal request traffic cannot crowd it out.
        self._cleanup_slots = asyncio.Semaphore(512)
        self._cleanup_in_use = 0

    def configure_cleanup(self, max_pending):
        if type(max_pending) is not int or not 1 <= max_pending <= 65536:
            raise ValueError("cleanup capacity must be an integer between 1 and 65536")
        if self._cleanup_in_use:
            raise RuntimeError(
                "cleanup capacity must be configured before serving requests"
            )
        self._cleanup_slots = asyncio.Semaphore(max_pending)

    def acquire(self, *, timeout=5, cleanup=False):
        return BoundedAcquire(self, timeout, cleanup)

    async def _query(self, method, *args, **kwargs):
        async with self.acquire() as conn:
            return await getattr(conn, method)(*args, **kwargs)

    async def fetch(self, *args, **kwargs):
        return await self._query("fetch", *args, **kwargs)

    async def fetchrow(self, *args, **kwargs):
        return await self._query("fetchrow", *args, **kwargs)

    async def fetchval(self, *args, **kwargs):
        return await self._query("fetchval", *args, **kwargs)

    async def execute(self, *args, **kwargs):
        return await self._query("execute", *args, **kwargs)

    async def close(self):
        await self._pool.close()


class PostgresStore(Notifications):
    def __init__(self, pool, dsn, global_max_bytes, quota_shards, application_name):
        super().__init__()
        self.pool, self.dsn, self.global_max_bytes = (
            BoundedPool(pool),
            dsn,
            global_max_bytes,
        )
        self.application_name = application_name
        self.quota_shards = quota_shards
        self.shard_max_bytes = global_max_bytes // quota_shards
        self.listener = None
        self.listener_task = None
        self._closing = False
        self._cpu_slots = asyncio.Semaphore(16)
        self._cpu_executor = ThreadPoolExecutor(
            max_workers=1, thread_name_prefix="relay-postgres-codec"
        )
        self._listener_ready = asyncio.Event()

    @classmethod
    async def open(
        cls,
        dsn,
        *,
        pool_min=2,
        pool_max=10,
        global_max_bytes=32 * 1024**3,
        quota_shards=64,
        _migration_reallocate_empty=False,
    ):
        if (
            not 1 <= pool_min <= pool_max <= 256
            or not 1 <= quota_shards <= 1024
            or global_max_bytes // quota_shards < RESULT_RESERVE
        ):
            raise ValueError("invalid PostgreSQL pool or storage budget")
        application_name = "zerus-mobile:" + str(uuid.uuid4())
        pool = await asyncpg.create_pool(
            dsn,
            min_size=pool_min,
            max_size=pool_max,
            command_timeout=15,
            server_settings={"application_name": application_name},
            max_inactive_connection_lifetime=60,
        )
        try:
            async with pool.acquire(timeout=5) as conn:
                async with conn.transaction():
                    await conn.execute("SELECT pg_advisory_xact_lock(735628109)")
                    await conn.execute(SCHEMA)
                    if (
                        await conn.fetchval("SELECT max(version) FROM relay_schema")
                        != 1
                    ):
                        raise ValueError("unsupported relay schema version")
                    await conn.execute(
                        "INSERT INTO relay_allocation VALUES(1,$1,$2) ON CONFLICT DO NOTHING",
                        global_max_bytes,
                        quota_shards,
                    )
                    allocation = await conn.fetchrow(
                        "SELECT global_max_bytes,quota_shards FROM relay_allocation WHERE id=1"
                    )
                    if (allocation["global_max_bytes"], allocation["quota_shards"]) != (
                        global_max_bytes,
                        quota_shards,
                    ):
                        if not _migration_reallocate_empty:
                            raise ValueError(
                                "PostgreSQL workers must share identical payload quota allocation"
                            )
                        # Only an offline migration may recover a failed empty
                        # import's allocation. No connected relay worker may keep
                        # using an obsolete allocation after this transaction.
                        tables = (
                            "workspaces",
                            "nodes",
                            "devices",
                            "invitations",
                            "requests",
                            "events",
                            "pushes",
                            "push_jobs",
                            "workspace_usage",
                            "poll_credentials",
                            "poll_leases",
                            "rate_limits",
                            "computers", "native_computers", "computer_aliases", "computer_routes",
                        )
                        await conn.execute(
                            "LOCK TABLE "
                            + ",".join(tables)
                            + " IN ACCESS EXCLUSIVE MODE"
                        )
                        for table in tables:
                            if await conn.fetchval(
                                f"SELECT EXISTS(SELECT 1 FROM {table})"
                            ):
                                raise ValueError(
                                    "allocation recovery requires an empty migration destination"
                                )
                        if await conn.fetchval(
                            "SELECT EXISTS(SELECT 1 FROM pg_stat_activity WHERE datname=current_database() AND application_name LIKE 'zerus-mobile:%' AND application_name!=$1)",
                            application_name,
                        ):
                            raise ValueError(
                                "allocation recovery requires all destination relay workers stopped"
                            )
                        if await conn.fetchval(
                            "SELECT COALESCE(sum(payload_bytes),0) FROM payload_shards"
                        ):
                            raise ValueError(
                                "allocation recovery requires empty payload ledgers"
                            )
                        await conn.execute("DELETE FROM payload_shards")
                        await conn.execute(
                            "UPDATE relay_allocation SET global_max_bytes=$1,quota_shards=$2 WHERE id=1",
                            global_max_bytes,
                            quota_shards,
                        )
                    await conn.execute(
                        "INSERT INTO payload_shards(id) SELECT generate_series(0,$1::integer-1) ON CONFLICT DO NOTHING",
                        quota_shards,
                    )
            store = cls(pool, dsn, global_max_bytes, quota_shards, application_name)
            store.listener_task = asyncio.create_task(store._listen())
            await asyncio.wait_for(store._listener_ready.wait(), 15)
            return store
        except BaseException:
            if "store" in locals():
                await store.close()
            else:
                await pool.close()
            raise

    async def _listen(self):
        while not self._closing:
            conn = None
            try:
                conn = await asyncpg.connect(
                    self.dsn,
                    timeout=10,
                    command_timeout=15,
                    server_settings={"application_name": self.application_name},
                )
                self.listener = conn

                def received(connection, pid, channel, payload):
                    if (
                        payload.startswith(("node:", "workspace:"))
                        and len(payload) <= 128
                    ):
                        self.notify(payload)

                await conn.add_listener("zerus_relay", received)
                self._listener_ready.set()
                for topic in list(self._topics):
                    self.notify(topic)
                while not self._closing and not conn.is_closed():
                    await asyncio.sleep(1)
            except (OSError, asyncpg.PostgresError, asyncio.TimeoutError):
                # Notifications are optional hints; authoritative fallback remains.
                self._listener_ready.set()
                await asyncio.sleep(1)
            finally:
                self.listener = None
                if conn and not conn.is_closed():
                    await conn.close()

    def configure_cleanup(self, max_pending):
        self.pool.configure_cleanup(max_pending)

    async def close(self):
        if self._closing:
            return
        self._closing = True
        if self.listener_task:
            self.listener_task.cancel()
            try:
                await self.listener_task
            except asyncio.CancelledError:
                pass
        await self.pool.close()
        await asyncio.to_thread(self._cpu_executor.shutdown, wait=True)

    async def _cpu(self, function, *args):
        from .async_store import StoreBusy

        if self._cpu_slots.locked():
            raise StoreBusy("database encoding capacity exhausted")
        await self._cpu_slots.acquire()
        task = None
        try:
            task = asyncio.get_running_loop().run_in_executor(
                EXECUTOR.get() or self._cpu_executor, functools.partial(function, *args)
            )
            try:
                return await asyncio.shield(task)
            except asyncio.CancelledError:
                while not task.done():
                    try:
                        await asyncio.shield(task)
                    except asyncio.CancelledError:
                        pass
                    except Exception:
                        break
                if task.done() and not task.cancelled():
                    task.exception()
                raise
        finally:
            self._cpu_slots.release()
            # Failed codec futures hold exception traceback frames; clearing
            # local references prevents maximum bodies waiting for cyclic GC.
            task = None
            function = None
            args = ()

    @staticmethod
    async def _notify(conn, topic):
        await conn.execute("SELECT pg_notify('zerus_relay',$1)", topic)

    async def workspace(self, name):
        value = str(uuid.uuid4())
        async with self.pool.acquire(timeout=5) as conn, conn.transaction():
            await conn.execute("INSERT INTO workspaces VALUES($1,$2)", value, name)
            await conn.execute(
                "INSERT INTO workspace_usage(workspace_id) VALUES($1)", value
            )
        return value

    async def node(self, workspace, name):
        value, secret = str(uuid.uuid4()), token()
        async with self.pool.acquire(timeout=5) as conn, conn.transaction():
            await self._usage(conn, workspace)
            await conn.execute('INSERT INTO nodes(id,workspace_id,name,token_hash) VALUES($1,$2,$3,$4)', value, workspace, name, digest(secret))
            await drive_async(conn, Registry(True,self.shard).enroll({'id':value,'workspace_id':workspace,'name':name}))
        return {"node_id": value, "node_token": secret}

    async def invite(self, workspace):
        code, expires = token(), time.time() + 600
        await self.pool.execute(
            "INSERT INTO invitations VALUES($1,$2,$3)", digest(code), workspace, expires
        )
        return {"pair_code": code, "expires_at": expires}

    async def pair(self, code, name):
        async with self.pool.acquire(timeout=5) as conn, conn.transaction():
            row = await conn.fetchrow(
                "DELETE FROM invitations WHERE code_hash=$1 AND expires>$2 RETURNING workspace_id",
                digest(code),
                time.time(),
            )
            if row is None:
                return None
            value, secret = str(uuid.uuid4()), token()
            await conn.execute(
                "INSERT INTO devices(id,workspace_id,name,token_hash) VALUES($1,$2,$3,$4)",
                value,
                row["workspace_id"],
                name,
                digest(secret),
            )
            workspace_name = await conn.fetchval(
                "SELECT name FROM workspaces WHERE id=$1", row["workspace_id"]
            )
            return {
                "device_id": value,
                "device_token": secret,
                "workspace_id": row["workspace_id"],
                "workspace_name": workspace_name,
            }

    async def authenticate(self, secret, role):
        assert role in ("nodes", "devices")
        row = await self.pool.fetchrow(
            f"SELECT {AUTH_COLUMNS} FROM {role} WHERE token_hash=$1 AND revoked=0",
            digest(secret),
        )
        return dict(row) if row else None

    async def authorized(self, value, role):
        assert role in ("nodes", "devices")
        return bool(
            await self.pool.fetchval(
                f"SELECT true FROM {role} WHERE id=$1 AND revoked=0", value
            )
        )

    async def ready(self):
        return await self.pool.fetchval("SELECT 1") == 1

    async def computers(self, workspace, max_bytes=1024 * 1024):
        async with self.pool.acquire(timeout=5) as conn, conn.transaction():
            # Exposure and root attachment must serialize: a catalog may make
            # a legacy ID visible while the first native binding deduplicates.
            await self._usage(conn,workspace)
            await conn.fetch('SELECT id FROM nodes WHERE workspace_id=$1 ORDER BY id FOR SHARE',workspace)
            # Snapshot CAS paths intentionally avoid workspace locks. Share
            # locks hold their selected bytes stable across preflight/fetch.
            await conn.fetch('SELECT gateway_id,route_id FROM computer_routes WHERE gateway_id IN (SELECT id FROM nodes WHERE workspace_id=$1) ORDER BY gateway_id,route_id FOR SHARE',workspace)
            return await drive_async(conn, Registry(True,self.shard).catalog(workspace,max_bytes))

    @staticmethod
    async def _usage(conn, workspace):
        return await conn.fetchrow(
            "SELECT * FROM workspace_usage WHERE workspace_id=$1 FOR UPDATE", workspace
        )

    @staticmethod
    def _bytes(row):
        return row["body_bytes"] + row["result_bytes"] + row["reserved_bytes"]

    def shard(self, workspace):
        return (
            int.from_bytes(hashlib.sha256(workspace.encode()).digest()[:8], "big")
            % self.quota_shards
        )

    async def _payload_usage(self, conn, workspace):
        await conn.fetchval(
            "SELECT payload_bytes FROM payload_shards WHERE id=$1 FOR UPDATE",
            self.shard(workspace),
        )
        return await self._usage(conn, workspace)

    async def submit(self, device, body, max_queue=200, max_bytes=MAX_QUEUE_BYTES):
        now, encoded = time.time(), await self._cpu(canonical, body)
        byte_count = len(encoded.encode())
        async with self.pool.acquire(timeout=5) as conn, conn.transaction():
            usage = await self._payload_usage(conn, device["workspace_id"])
            if not await conn.fetchval(
                "SELECT true FROM devices WHERE id=$1 AND workspace_id=$2 AND revoked=0 FOR SHARE",
                device["id"],
                device["workspace_id"],
            ):
                return 401, {"error": "credential revoked"}
            existing = await conn.fetchrow(
                f"SELECT {RECEIPT_COLUMNS} FROM requests WHERE id=$1",
                body["request_id"],
            )
            if existing:
                if existing["device_id"] != device["id"] or existing[
                    "body_hash"
                ] != digest(encoded):
                    return 409, {
                        "error": "request_id already used with different content"
                    }
                return 202, Store.envelope(existing)
            # Lock credentials through commit: revoke cannot race admission.
            if not await conn.fetchval(
                "SELECT true FROM devices WHERE id=$1 AND workspace_id=$2 AND revoked=0 FOR SHARE",
                device["id"],
                device["workspace_id"],
            ):
                return 401, {"error": "credential revoked"}
            node = await drive_async(conn, Registry(True,self.shard).select(device['workspace_id'],body['computer_id']))
            if node is None:
                exists=await drive_async(conn, Registry().resolve(device['workspace_id'],body['computer_id']))
                return (409 if exists else 404), {'error':'computer route is unavailable' if exists else 'computer not found'}
            # The workspace lock also serializes manifest admission/withdrawal.
            await conn.fetchval('SELECT id FROM nodes WHERE id=$1 AND revoked=0 FOR SHARE',node['gateway_id'])
            byte_count += 144 if node['machine_id'] else 0
            snapshot = json.loads(node["snapshot"]) if node["snapshot"] else None
            op, payload = body["operation"], body["payload"]
            if op in CAPABILITY_OPERATIONS and not supports(snapshot, op):
                return 409, {
                    "error": "computer does not advertise this operation; update its connector"
                }
            if op == "inspect" and any(
                not supports(snapshot, f, "features") for f in inspect_features(payload)
            ):
                return 409, {
                    "error": "computer does not advertise this inspection feature; update its connector"
                }
            if (
                op == "send"
                and "agent_id" in payload
                and not supports(snapshot, "send_agent", "features")
            ):
                return 409, {
                    "error": "computer does not advertise child message transport; update its connector"
                }
            if op == "terminal_input" and (
                node["last_seen"] is None or now - node["last_seen"] > 45
            ):
                return 409, {
                    "error": "computer is offline; terminal input was not queued"
                }
            reads = op in READ_OPERATIONS
            if usage["active"] >= max_queue:
                return 429, {"error": "workspace queue is full"}
            if usage["reads" if reads else "mutations"] >= (5000 if reads else 100000):
                return 429, {"error": "workspace retained request limit reached"}
            total = byte_count + RESULT_RESERVE
            small = byte_count <= 64 * 1024 and not payload.get("attachments")
            if usage["payload_bytes"] + total > max_bytes + (
                RESULT_RESERVE if small else 0
            ):
                return 429, {"error": "workspace retained payload budget is full"}
            accepted = await conn.fetchval(
                "UPDATE payload_shards SET payload_bytes=payload_bytes+$1 WHERE id=$3 AND payload_bytes+$1<=$2 RETURNING true",
                total,
                self.shard_max_bytes,
                self.shard(device["workspace_id"]),
            )
            if not accepted:
                return 429, {"error": "relay retained payload budget is full"}
            # UUIDs are globally unique. A concurrent different workspace insert
            # cannot consume capacity if it loses this ON CONFLICT race.
            inserted = await conn.fetchval(
                "INSERT INTO requests(id,workspace_id,device_id,node_id,operation,body_hash,body_bytes,body,state,created,updated,expires_at,reserved_bytes,target_computer_id,target_machine_id,route_id) VALUES($1,$2,$3,$4,$5,$6,$7,$8,'queued',$9,$9,$10,$11,$12,$13,$14) ON CONFLICT(id) DO NOTHING RETURNING true",
                body["request_id"],
                device["workspace_id"],
                device["id"],
                node["gateway_id"],
                op,
                digest(encoded),
                byte_count,
                encoded,
                now,
                now + 5 if op == "terminal_input" else None,
                RESULT_RESERVE,
                node["id"],node["machine_id"],None if node["local"] else node["route_id"],
            )
            if not inserted:
                await conn.execute(
                    "UPDATE payload_shards SET payload_bytes=payload_bytes-$1 WHERE id=$2",
                    total,
                    self.shard(device["workspace_id"]),
                )
                existing = await conn.fetchrow(
                    f"SELECT {RECEIPT_COLUMNS} FROM requests WHERE id=$1",
                    body["request_id"],
                )
                if existing["device_id"] == device["id"] and existing[
                    "body_hash"
                ] == digest(encoded):
                    return 202, Store.envelope(existing)
                return 409, {"error": "request_id already used with different content"}
            await conn.execute(
                "UPDATE workspace_usage SET payload_bytes=payload_bytes+$2,active=active+1,reads=reads+$3,mutations=mutations+$4 WHERE workspace_id=$1",
                device["workspace_id"],
                total,
                int(reads),
                int(not reads),
            )
            await self._notify(conn, "node:" + node["gateway_id"])
            return 202, {
                "request_id": body["request_id"],
                "state": "queued",
                "result": None,
                "error": None,
            }

    async def _expire(self, conn, row, queue_ttl, claim_ttl):
        now = time.time()
        state, error = None, None
        if row["state"] == "queued":
            if row["expires_at"] is not None and row["expires_at"] <= now:
                state, error = "failed", "terminal input expired before delivery"
            elif row["created"] <= now - queue_ttl:
                state, error = "failed", "request expired before delivery"
        elif row["state"] == "claimed" and row["claimed"] <= now - claim_ttl:
            state, error = "uncertain", "node result timeout after claim"
        if state:
            size = len(error.encode())
            delta = size - row["reserved_bytes"] - row["result_bytes"]
            await conn.execute(
                "UPDATE requests SET state=$2,error=$3,updated=$4,reserved_bytes=0,result_bytes=$5 WHERE id=$1",
                row["id"],
                state,
                error,
                now,
                size,
            )
            await conn.execute(
                "UPDATE payload_shards SET payload_bytes=payload_bytes+$2 WHERE id=$1",
                self.shard(row["workspace_id"]),
                delta,
            )
            await conn.execute(
                "UPDATE workspace_usage SET payload_bytes=payload_bytes+$2 WHERE workspace_id=$1",
                row["workspace_id"],
                delta,
            )
            await conn.execute(
                "UPDATE workspace_usage SET active=active-1 WHERE workspace_id=$1",
                row["workspace_id"],
            )
            return True
        return False

    async def get_request(self, device, value, queue_ttl=120, claim_ttl=90):
        # Ordinary receipt reads do not lock shard/workspace counters. Only an
        # expired pending receipt enters the byte-changing transaction.
        async with self.pool.acquire(timeout=5) as conn:
            row = await conn.fetchrow(
                f"SELECT {RECEIPT_COLUMNS},workspace_id,reserved_bytes,result_bytes FROM requests WHERE id=$1 AND device_id=$2",
                value,
                device["id"],
            )
            if row is None:
                return None
            now = time.time()
            expired = (
                row["state"] == "queued"
                and (
                    row["created"] <= now - queue_ttl
                    or (row["expires_at"] is not None and row["expires_at"] <= now)
                )
            ) or (row["state"] == "claimed" and row["claimed"] <= now - claim_ttl)
            async with conn.transaction():
                if expired:
                    await self._payload_usage(conn, device["workspace_id"])
                if not await conn.fetchval(
                    "SELECT true FROM devices WHERE id=$1 AND revoked=0 FOR SHARE",
                    device["id"],
                ):
                    return None
                if expired:
                    row = await conn.fetchrow(
                        f"SELECT {RECEIPT_COLUMNS},workspace_id,reserved_bytes,result_bytes FROM requests WHERE id=$1 AND device_id=$2 FOR UPDATE",
                        value,
                        device["id"],
                    )
                    if row is None:
                        return None
                    if await self._expire(conn, row, queue_ttl, claim_ttl):
                        row = await conn.fetchrow(
                            f"SELECT {RECEIPT_COLUMNS} FROM requests WHERE id=$1", value
                        )
                if row["operation"] in ("terminal_snapshot", "attachment_read") and row["state"] in TERMINAL:
                    await conn.execute(
                        "UPDATE requests SET result_read=$2 WHERE id=$1",
                        value,
                        time.time(),
                    )
                return await self._cpu(Store.envelope, row)

    async def has_pending(self, node, queue_ttl=120, allow_gateway=False):
        now = time.time()
        return bool(
            await self.pool.fetchval(
                "SELECT true FROM requests r JOIN nodes n ON n.id=r.node_id WHERE r.node_id=$1 AND n.workspace_id=$2 AND n.revoked=0 AND r.state='queued' AND (r.target_machine_id IS NULL OR $5) AND r.created>$3 AND (r.expires_at IS NULL OR r.expires_at>$4) LIMIT 1",
                node["id"],
                node["workspace_id"],
                now - queue_ttl,
                now,
                allow_gateway,
            )
        )

    async def claim(self, node, queue_ttl=120, claim_ttl=90, allow_gateway=False):
        now = time.time()
        async with self.pool.acquire(timeout=5) as conn, conn.transaction():
            await self._payload_usage(conn, node["workspace_id"])
            if not await conn.fetchval(
                "SELECT true FROM nodes WHERE id=$1 AND revoked=0 FOR SHARE", node["id"]
            ):
                return []
            row = await conn.fetchrow(
                "SELECT * FROM requests WHERE node_id=$1 AND state='queued' AND (target_machine_id IS NULL OR $4) AND created>$2 AND (expires_at IS NULL OR expires_at>$3) ORDER BY created,id LIMIT 1 FOR UPDATE SKIP LOCKED",
                node["id"],
                now - queue_ttl,
                now,
                allow_gateway,
            )
            if row is None:
                return []
            registry=Registry(True,self.shard)
            if not await drive_async(conn, registry.frozen(row,allow_gateway)):
                await drive_async(conn,registry.invalidate('id=?',(row['id'],)))
                return []
            await conn.execute(
                "UPDATE requests SET state='claimed',claimed=$2,updated=$2 WHERE id=$1",
                row["id"],
                now,
            )
            body = await self._cpu(json.loads, row["body"])
            command = {
                k: body[k] for k in ("request_id", "operation", "session", "payload")
            }
            if row["target_machine_id"] is not None:
                command["gateway_route"]={"schema":1,"route_id":row["route_id"],"computer_id":row["target_computer_id"],"machine_id":row["target_machine_id"]}
            if row["expires_at"] is not None:
                command["expires_at"] = row["expires_at"]
            return [command]

    async def result(self, node, value, body, claim_ttl=90):
        encoded = (
            await self._cpu(canonical, body["result"])
            if body["result"] is not None
            else None
        )
        size = len((encoded or "").encode()) + len((body["error"] or "").encode())
        if size > RESULT_RESERVE:
            return 413
        async with self.pool.acquire(timeout=5) as conn, conn.transaction():
            await self._payload_usage(conn, node["workspace_id"])
            if not await conn.fetchval(
                "SELECT true FROM nodes WHERE id=$1 AND revoked=0 FOR SHARE", node["id"]
            ):
                return 401
            row = await conn.fetchrow(
                f"SELECT {RECEIPT_COLUMNS},workspace_id,reserved_bytes,result_bytes FROM requests WHERE id=$1 AND node_id=$2 FOR UPDATE",
                value,
                node["id"],
            )
            if row is None:
                return 404
            if await self._expire(conn, row, 120, claim_ttl):
                return 409
            if row["state"] != "claimed":
                return (
                    200
                    if (row["state"], row["result"], row["error"])
                    == (body["state"], encoded, body["error"])
                    else 409
                )
            delta = size - row["reserved_bytes"] - row["result_bytes"]
            if delta > 0:
                if not await conn.fetchval(
                    "UPDATE payload_shards SET payload_bytes=payload_bytes+$1 WHERE id=$3 AND payload_bytes+$1<=$2 RETURNING true",
                    delta,
                    self.shard_max_bytes,
                    self.shard(node["workspace_id"]),
                ):
                    return 429
            elif delta:
                await conn.execute(
                    "UPDATE payload_shards SET payload_bytes=payload_bytes+$1 WHERE id=$2",
                    delta,
                    self.shard(node["workspace_id"]),
                )
            await conn.execute(
                "UPDATE requests SET state=$2,result=$3,error=$4,updated=$5,result_bytes=$6,reserved_bytes=0 WHERE id=$1",
                value,
                body["state"],
                encoded,
                body["error"],
                time.time(),
                size,
            )
            await conn.execute(
                "UPDATE workspace_usage SET active=active-1,payload_bytes=payload_bytes+$2 WHERE workspace_id=$1",
                node["workspace_id"],
                delta,
            )
            return 200

    async def events(self, workspace, after, max_bytes=1024 * 1024):
        async with self.pool.acquire(timeout=5) as conn, conn.transaction(
            isolation="repeatable_read", readonly=True
        ):
            size = await conn.fetchval(
                "SELECT COALESCE(sum(octet_length(session)+256),0) FROM (SELECT session FROM events WHERE workspace_id=$1 AND id>$2 ORDER BY id LIMIT 100) selected",
                workspace,
                after,
            )
            if size > max_bytes:
                return None
            return [
                dict(r)
                for r in await conn.fetch(
                    "SELECT id,node_id,session,kind,created FROM events WHERE workspace_id=$1 AND id>$2 ORDER BY id LIMIT 100",
                    workspace,
                    after,
                )
            ]

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

    async def _publish_peer_events(self,conn,node,changes,now):
        global_usage = await conn.fetchrow(
            "SELECT * FROM global_usage WHERE id=1 FOR UPDATE"
        )
        registrations = await conn.fetch(
            "SELECT p.device_id,NOT EXISTS(SELECT 1 FROM push_jobs j WHERE j.device_id=p.device_id) AS needs_job FROM pushes p JOIN devices d ON d.id=p.device_id WHERE d.workspace_id=$1 AND d.revoked=0 LIMIT 1000",
            node["workspace_id"],
        )
        event_evictions = max(0, global_usage["events"] + len(changes) - 100000)
        job_evictions = max(
            0,
            global_usage["push_jobs"]
            + sum(r["needs_job"] for r in registrations)
            - 10000,
        )
        victims = []
        job_victims = []
        if event_evictions:
            victims = await conn.fetch(
                "SELECT id,workspace_id FROM events ORDER BY id LIMIT $1",
                event_evictions,
            )
        if job_evictions:
            job_victims = await conn.fetch(
                "SELECT j.id,d.workspace_id FROM push_jobs j JOIN devices d ON d.id=j.device_id ORDER BY j.id LIMIT $1",
                job_evictions,
            )
        workspaces = (
            {node["workspace_id"]}
            | {r["workspace_id"] for r in victims}
            | {r["workspace_id"] for r in job_victims}
        )
        await conn.fetch(
            "SELECT workspace_id FROM workspace_usage WHERE workspace_id=ANY($1::text[]) ORDER BY workspace_id FOR UPDATE",
            sorted(workspaces),
        )
        latest = None
        for session, kind in changes:
            usage = await conn.fetchrow(
                "SELECT events FROM workspace_usage WHERE workspace_id=$1",
                node["workspace_id"],
            )
            fleet = await conn.fetchval(
                "SELECT events FROM global_usage WHERE id=1"
            )
            victim = None
            if usage["events"] >= 10000:
                victim = await conn.fetchrow(
                    "SELECT id,workspace_id FROM events WHERE workspace_id=$1 ORDER BY id LIMIT 1",
                    node["workspace_id"],
                )
            elif fleet >= 100000:
                victim = await conn.fetchrow(
                    "SELECT id,workspace_id FROM events ORDER BY id LIMIT 1"
                )
            if victim:
                await conn.execute(
                    "DELETE FROM events WHERE id=$1", victim["id"]
                )
                await conn.execute(
                    "UPDATE workspace_usage SET events=events-1 WHERE workspace_id=$1",
                    victim["workspace_id"],
                )
                await conn.execute(
                    "UPDATE global_usage SET events=events-1 WHERE id=1"
                )
            latest = await conn.fetchval(
                "INSERT INTO events(workspace_id,node_id,session,kind,created) VALUES($1,$2,$3,$4,$5) RETURNING id",
                node["workspace_id"],
                node["id"],
                session,
                kind,
                now,
            )
            await conn.execute(
                "UPDATE workspace_usage SET events=events+1 WHERE workspace_id=$1",
                node["workspace_id"],
            )
            await conn.execute(
                "UPDATE global_usage SET events=events+1 WHERE id=1"
            )
        if latest is not None:
            payload = canonical({"event_id": latest, "kind": "wake"})
            for registration in registrations:
                existing = await conn.fetchrow(
                    "SELECT id,lease_until FROM push_jobs WHERE device_id=$1 ORDER BY id LIMIT 1",
                    registration["device_id"],
                )
                if existing:
                    if (
                        existing["lease_until"] is None
                        or existing["lease_until"] <= now
                    ):
                        await conn.execute(
                            "UPDATE push_jobs SET event_id=$2,payload=$3,next_at=LEAST(next_at,$4) WHERE id=$1",
                            existing["id"],
                            latest,
                            payload,
                            now,
                        )
                    continue
                usage = await conn.fetchval(
                    "SELECT push_jobs FROM workspace_usage WHERE workspace_id=$1",
                    node["workspace_id"],
                )
                fleet = await conn.fetchval(
                    "SELECT push_jobs FROM global_usage WHERE id=1"
                )
                victim = None
                if usage >= 1000:
                    victim = await conn.fetchrow(
                        "SELECT j.id,d.workspace_id FROM push_jobs j JOIN devices d ON d.id=j.device_id WHERE d.workspace_id=$1 ORDER BY j.id LIMIT 1",
                        node["workspace_id"],
                    )
                elif fleet >= 10000:
                    victim = await conn.fetchrow(
                        "SELECT j.id,d.workspace_id FROM push_jobs j JOIN devices d ON d.id=j.device_id ORDER BY j.id LIMIT 1"
                    )
                if victim:
                    await conn.execute(
                        "DELETE FROM push_jobs WHERE id=$1", victim["id"]
                    )
                    await conn.execute(
                        "UPDATE workspace_usage SET push_jobs=push_jobs-1 WHERE workspace_id=$1",
                        victim["workspace_id"],
                    )
                    await conn.execute(
                        "UPDATE global_usage SET push_jobs=push_jobs-1 WHERE id=1"
                    )
                await conn.execute(
                    "INSERT INTO push_jobs(device_id,event_id,payload,next_at,created) VALUES($1,$2,$3,$4,$4)",
                    registration["device_id"],
                    latest,
                    payload,
                    now,
                )
                await conn.execute(
                    "UPDATE workspace_usage SET push_jobs=push_jobs+1 WHERE workspace_id=$1",
                    node["workspace_id"],
                )
                await conn.execute(
                    "UPDATE global_usage SET push_jobs=push_jobs+1 WHERE id=1"
                )

    async def heartbeat(self, node, snapshot, machine_id=None, peers=None):
        # Legacy unchanged heartbeats retain their inexpensive node-only path.
        needs_registry = machine_id is not None or peers or await self.pool.fetchval('SELECT true FROM nodes n WHERE n.id=$1 AND (n.machine_id IS NOT NULL OR EXISTS(SELECT 1 FROM computer_routes r WHERE r.gateway_id=n.id AND r.local=0))',node['id'])
        manifest=digest(canonical({'machine_id':machine_id,'peers':peers or [],'guarded':supports(snapshot,'gateway_one_hop','features')}))
        unchanged=await self.pool.fetchval('SELECT true FROM nodes WHERE id=$1 AND revoked=0 AND manifest_hash=$2',node['id'],manifest) if needs_registry else True
        if needs_registry and not unchanged:
            async with self.pool.acquire(timeout=5) as conn, conn.transaction():
                await self._payload_usage(conn,node['workspace_id'])
                current=await conn.fetchval('SELECT manifest_hash FROM nodes WHERE id=$1 AND revoked=0',node['id'])
                if current!=manifest:
                    await drive_async(conn,Registry(True,self.shard).gateway(node,machine_id,peers or [],supports(snapshot,'gateway_one_hop','features')))
                    await conn.execute('UPDATE nodes SET manifest_hash=$2 WHERE id=$1',node['id'],manifest)
                await self._notify(conn,'workspace:'+node['workspace_id'])
        return await self._heartbeat_snapshot(node,snapshot)

    async def peer_heartbeat(self,node,route_id,machine_id,snapshot):
        encoded=await self._cpu(canonical,snapshot)
        if len(encoded.encode())>MAX_PEER_SNAPSHOT_BYTES:raise RouteError(413,'peer snapshot is too large')
        for attempt in range(3):
            previous=await self.pool.fetchrow('SELECT snapshot,snapshot_hash,computer_id FROM computer_routes WHERE gateway_id=$1 AND route_id=$2 AND local=0',node['id'],route_id)
            if previous is None:raise RouteError(404,'peer route not found')
            changes=await self._cpu(self._event_changes,previous['snapshot'],snapshot)
            changes=[(name,kind) for name,kind in changes if len(name)<=1024 and not any(ord(c)<32 for c in name)]
            if not changes:
                updated=await self.pool.fetchval("UPDATE computer_routes r SET snapshot=$4,snapshot_hash=$5,last_seen=$6 FROM nodes n,computers c WHERE r.gateway_id=$1 AND r.route_id=$2 AND r.machine_id=$3 AND r.local=0 AND r.active=1 AND r.online=1 AND r.guarded=1 AND n.id=r.gateway_id AND n.workspace_id=$8 AND n.revoked=0 AND c.id=r.computer_id AND c.revoked=0 AND r.snapshot_hash IS NOT DISTINCT FROM $7 RETURNING r.computer_id",node['id'],route_id,machine_id,encoded,digest(encoded),time.time(),previous['snapshot_hash'],node['workspace_id'])
                if updated:
                    return {'id':updated,'previous':previous['snapshot'],'changed':digest(encoded)!=previous['snapshot_hash']}
            async with self.pool.acquire(timeout=5) as conn,conn.transaction():
                if changes:
                    # Event accounting follows the established global→workspace
                    # order; manifest/payload transactions never wait on global.
                    await conn.fetchval('SELECT id FROM global_usage WHERE id=1 FOR UPDATE')
                await self._usage(conn,node['workspace_id'])
                current=await conn.fetchrow('SELECT snapshot_hash FROM computer_routes WHERE gateway_id=$1 AND route_id=$2 FOR UPDATE',node['id'],route_id)
                if current is None:raise RouteError(404,'peer route not found')
                if current['snapshot_hash']!=previous['snapshot_hash']:continue
                result=await drive_async(conn,Registry(True,self.shard).peer_heartbeat(node,route_id,machine_id,encoded))
                if changes:
                    await self._publish_peer_events(conn,{'id':result['id'],'workspace_id':node['workspace_id']},changes,time.time())
                await self._notify(conn,'workspace:'+node['workspace_id'])
                return result
        raise RouteError(409,'peer snapshot changed concurrently')

    async def revoke_computer(self,value):
        async with self.pool.acquire(timeout=5) as conn,conn.transaction():
            workspace=await conn.fetchval('SELECT c.workspace_id FROM computers c LEFT JOIN computer_aliases a ON a.computer_id=c.id WHERE c.id=$1 OR a.alias=$1 LIMIT 1',value)
            if workspace is None:return False
            await self._payload_usage(conn,workspace)
            result=await drive_async(conn,Registry(True,self.shard).revoke_computer(value))
            await self._notify(conn,'workspace:'+workspace)
            return result

    async def _heartbeat_snapshot(self, node, snapshot):
        encoded = await self._cpu(canonical, snapshot)
        snapshot_hash = digest(encoded)
        now = time.time()
        if await self.pool.fetchval(
            "UPDATE nodes SET last_seen=$2 WHERE id=$1 AND workspace_id=$4 AND revoked=0 AND (snapshot_hash=$3 OR EXISTS(SELECT 1 FROM computers c WHERE c.id=nodes.computer_id AND c.revoked=1)) RETURNING true",
            node["id"],
            now,
            snapshot_hash,
            node["workspace_id"],
        ):
            return
        for attempt in range(3):
            previous = await self.pool.fetchrow(
                "SELECT n.snapshot,n.snapshot_hash,c.revoked AS computer_revoked FROM nodes n JOIN computers c ON c.id=n.computer_id WHERE n.id=$1 AND n.workspace_id=$2 AND n.revoked=0",
                node["id"],
                node["workspace_id"],
            )
            if previous is None:
                return
            if previous['computer_revoked']:
                await self.pool.execute('UPDATE nodes SET last_seen=$2 WHERE id=$1 AND revoked=0',node['id'],now)
                return
            changes = await self._cpu(
                self._event_changes, previous["snapshot"], snapshot
            )
            changes = [
                (name, kind)
                for name, kind in changes
                if len(name) <= 1024 and not any(ord(c) < 32 for c in name)
            ]
            if not changes:
                # Progress/timestamp changes are common at fleet scale. Commit
                # them with a node-only compare-and-swap; no fleet quota lock or
                # workspace wake is needed when there is no durable new event.
                async with self.pool.acquire() as conn,conn.transaction():
                    # Revoke invalidates the gateway node before its computer.
                    # Lock that node, then read revocation in a fresh statement;
                    # a pre-lock MVCC snapshot cannot publish after a revoke.
                    await conn.fetchval('SELECT id FROM nodes WHERE id=$1 FOR UPDATE',node['id'])
                    revoked=await conn.fetchval('SELECT c.revoked FROM nodes n JOIN computers c ON c.id=n.computer_id WHERE n.id=$1',node['id'])
                    if revoked:
                        await conn.execute('UPDATE nodes SET last_seen=$2 WHERE id=$1 AND revoked=0',node['id'],now)
                        return
                    changed = await conn.fetchval(
                        "UPDATE nodes SET last_seen=$2,snapshot=$3,snapshot_hash=$4 WHERE id=$1 AND workspace_id=$6 AND revoked=0 AND snapshot_hash IS NOT DISTINCT FROM $5 AND NOT EXISTS(SELECT 1 FROM computers c WHERE c.id=nodes.computer_id AND c.revoked=1) RETURNING true",
                        node["id"],
                        now,
                        encoded,
                        snapshot_hash,
                        previous["snapshot_hash"],
                        node["workspace_id"],
                    )
                if changed:
                    return
                continue
            async with self.pool.acquire() as conn, conn.transaction():
                global_usage = await conn.fetchrow(
                    "SELECT * FROM global_usage WHERE id=1 FOR UPDATE"
                )
                registrations = await conn.fetch(
                    "SELECT p.device_id,NOT EXISTS(SELECT 1 FROM push_jobs j WHERE j.device_id=p.device_id) AS needs_job FROM pushes p JOIN devices d ON d.id=p.device_id WHERE d.workspace_id=$1 AND d.revoked=0 LIMIT 1000",
                    node["workspace_id"],
                )
                event_evictions = max(0, global_usage["events"] + len(changes) - 100000)
                job_evictions = max(
                    0,
                    global_usage["push_jobs"]
                    + sum(r["needs_job"] for r in registrations)
                    - 10000,
                )
                victims = []
                job_victims = []
                if event_evictions:
                    victims = await conn.fetch(
                        "SELECT id,workspace_id FROM events ORDER BY id LIMIT $1",
                        event_evictions,
                    )
                if job_evictions:
                    job_victims = await conn.fetch(
                        "SELECT j.id,d.workspace_id FROM push_jobs j JOIN devices d ON d.id=j.device_id ORDER BY j.id LIMIT $1",
                        job_evictions,
                    )
                workspaces = (
                    {node["workspace_id"]}
                    | {r["workspace_id"] for r in victims}
                    | {r["workspace_id"] for r in job_victims}
                )
                await conn.fetch(
                    "SELECT workspace_id FROM workspace_usage WHERE workspace_id=ANY($1::text[]) ORDER BY workspace_id FOR UPDATE",
                    sorted(workspaces),
                )
                current = await conn.fetchrow(
                    "SELECT n.snapshot_hash,c.revoked AS computer_revoked FROM nodes n JOIN computers c ON c.id=n.computer_id WHERE n.id=$1 AND n.workspace_id=$2 AND n.revoked=0 FOR UPDATE OF n",
                    node["id"],
                    node["workspace_id"],
                )
                if current is None:
                    return
                if current['computer_revoked']:
                    await conn.execute('UPDATE nodes SET last_seen=$2 WHERE id=$1 AND revoked=0',node['id'],now)
                    return
                if current["snapshot_hash"] != previous["snapshot_hash"]:
                    continue
                latest = None
                for session, kind in changes:
                    usage = await conn.fetchrow(
                        "SELECT events FROM workspace_usage WHERE workspace_id=$1",
                        node["workspace_id"],
                    )
                    fleet = await conn.fetchval(
                        "SELECT events FROM global_usage WHERE id=1"
                    )
                    victim = None
                    if usage["events"] >= 10000:
                        victim = await conn.fetchrow(
                            "SELECT id,workspace_id FROM events WHERE workspace_id=$1 ORDER BY id LIMIT 1",
                            node["workspace_id"],
                        )
                    elif fleet >= 100000:
                        victim = await conn.fetchrow(
                            "SELECT id,workspace_id FROM events ORDER BY id LIMIT 1"
                        )
                    if victim:
                        await conn.execute(
                            "DELETE FROM events WHERE id=$1", victim["id"]
                        )
                        await conn.execute(
                            "UPDATE workspace_usage SET events=events-1 WHERE workspace_id=$1",
                            victim["workspace_id"],
                        )
                        await conn.execute(
                            "UPDATE global_usage SET events=events-1 WHERE id=1"
                        )
                    latest = await conn.fetchval(
                        "INSERT INTO events(workspace_id,node_id,session,kind,created) VALUES($1,$2,$3,$4,$5) RETURNING id",
                        node["workspace_id"],
                        await conn.fetchval('SELECT computer_id FROM nodes WHERE id=$1',node['id']),
                        session,
                        kind,
                        now,
                    )
                    await conn.execute(
                        "UPDATE workspace_usage SET events=events+1 WHERE workspace_id=$1",
                        node["workspace_id"],
                    )
                    await conn.execute(
                        "UPDATE global_usage SET events=events+1 WHERE id=1"
                    )
                if latest is not None:
                    payload = canonical({"event_id": latest, "kind": "wake"})
                    for registration in registrations:
                        existing = await conn.fetchrow(
                            "SELECT id,lease_until FROM push_jobs WHERE device_id=$1 ORDER BY id LIMIT 1",
                            registration["device_id"],
                        )
                        if existing:
                            if (
                                existing["lease_until"] is None
                                or existing["lease_until"] <= now
                            ):
                                await conn.execute(
                                    "UPDATE push_jobs SET event_id=$2,payload=$3,next_at=LEAST(next_at,$4) WHERE id=$1",
                                    existing["id"],
                                    latest,
                                    payload,
                                    now,
                                )
                            continue
                        usage = await conn.fetchval(
                            "SELECT push_jobs FROM workspace_usage WHERE workspace_id=$1",
                            node["workspace_id"],
                        )
                        fleet = await conn.fetchval(
                            "SELECT push_jobs FROM global_usage WHERE id=1"
                        )
                        victim = None
                        if usage >= 1000:
                            victim = await conn.fetchrow(
                                "SELECT j.id,d.workspace_id FROM push_jobs j JOIN devices d ON d.id=j.device_id WHERE d.workspace_id=$1 ORDER BY j.id LIMIT 1",
                                node["workspace_id"],
                            )
                        elif fleet >= 10000:
                            victim = await conn.fetchrow(
                                "SELECT j.id,d.workspace_id FROM push_jobs j JOIN devices d ON d.id=j.device_id ORDER BY j.id LIMIT 1"
                            )
                        if victim:
                            await conn.execute(
                                "DELETE FROM push_jobs WHERE id=$1", victim["id"]
                            )
                            await conn.execute(
                                "UPDATE workspace_usage SET push_jobs=push_jobs-1 WHERE workspace_id=$1",
                                victim["workspace_id"],
                            )
                            await conn.execute(
                                "UPDATE global_usage SET push_jobs=push_jobs-1 WHERE id=1"
                            )
                        await conn.execute(
                            "INSERT INTO push_jobs(device_id,event_id,payload,next_at,created) VALUES($1,$2,$3,$4,$4)",
                            registration["device_id"],
                            latest,
                            payload,
                            now,
                        )
                        await conn.execute(
                            "UPDATE workspace_usage SET push_jobs=push_jobs+1 WHERE workspace_id=$1",
                            node["workspace_id"],
                        )
                        await conn.execute(
                            "UPDATE global_usage SET push_jobs=push_jobs+1 WHERE id=1"
                        )
                await conn.execute(
                    "UPDATE nodes SET last_seen=$2,snapshot=$3,snapshot_hash=$4 WHERE id=$1",
                    node["id"],
                    now,
                    encoded,
                    snapshot_hash,
                )
                await self._notify(conn, "workspace:" + node["workspace_id"])

                return
        from .async_store import StoreBusy

        raise StoreBusy("concurrent heartbeat update; retry next heartbeat")

    @staticmethod
    async def _delete_jobs(conn, device):
        count = await conn.fetchval(
            "WITH deleted AS (DELETE FROM push_jobs WHERE device_id=$1 RETURNING id) SELECT count(*) FROM deleted",
            device,
        )
        if count:
            workspace = await conn.fetchval(
                "SELECT workspace_id FROM devices WHERE id=$1", device
            )
            await conn.execute(
                "UPDATE workspace_usage SET push_jobs=push_jobs-$2 WHERE workspace_id=$1",
                workspace,
                count,
            )
            await conn.execute(
                "UPDATE global_usage SET push_jobs=push_jobs-$1 WHERE id=1", count
            )

    async def register_push(self, device, provider, target):
        async with self.pool.acquire(timeout=5) as conn, conn.transaction():
            await conn.fetchrow("SELECT id FROM global_usage WHERE id=1 FOR UPDATE")
            workspace = await conn.fetchval(
                "SELECT workspace_id FROM devices WHERE id=$1", device
            )
            await self._usage(conn, workspace)
            if not await conn.fetchval(
                "SELECT true FROM devices WHERE id=$1 AND revoked=0 FOR SHARE", device
            ):
                return
            await conn.execute(
                "INSERT INTO pushes VALUES($1,$2,$3) ON CONFLICT(device_id) DO UPDATE SET provider=excluded.provider,target=excluded.target",
                device,
                provider,
                target,
            )
            await self._delete_jobs(conn, device)

    async def delete_push(self, device):
        async with self.pool.acquire(timeout=5) as conn, conn.transaction():
            await conn.fetchrow("SELECT id FROM global_usage WHERE id=1 FOR UPDATE")
            workspace = await conn.fetchval(
                "SELECT workspace_id FROM devices WHERE id=$1", device
            )
            await self._usage(conn, workspace)
            await conn.execute("DELETE FROM pushes WHERE device_id=$1", device)
            await self._delete_jobs(conn, device)

    async def revoke(self, role, value):
        assert role in ("nodes", "devices")
        async with self.pool.acquire(timeout=5) as conn, conn.transaction():
            await conn.fetchrow("SELECT id FROM global_usage WHERE id=1 FOR UPDATE")
            workspace = await conn.fetchval(
                f"SELECT workspace_id FROM {role} WHERE id=$1", value
            )
            if workspace is None:
                return False
            await self._payload_usage(conn, workspace)
            await conn.execute(f"UPDATE {role} SET revoked=1 WHERE id=$1", value)
            field = "node_id" if role == "nodes" else "device_id"
            rows = await conn.fetch(
                f"SELECT id,state,reserved_bytes,result_bytes FROM requests WHERE {field}=$1 AND state IN ('queued','claimed') FOR UPDATE",
                value,
            )
            delta = 0
            for row in rows:
                error = (
                    "credential revoked before delivery"
                    if row["state"] == "queued"
                    else "credential revoked after claim"
                )
                size = len(error.encode())
                delta += size - row["reserved_bytes"] - row["result_bytes"]
                await conn.execute(
                    "UPDATE requests SET state=$2,error=$3,updated=$4,reserved_bytes=0,result_bytes=$5 WHERE id=$1",
                    row["id"],
                    "failed" if row["state"] == "queued" else "uncertain",
                    error,
                    time.time(),
                    size,
                )
            await conn.execute(
                "UPDATE workspace_usage SET active=active-$2,payload_bytes=payload_bytes+$3 WHERE workspace_id=$1",
                workspace,
                len(rows),
                delta,
            )
            await conn.execute(
                "UPDATE payload_shards SET payload_bytes=payload_bytes+$2 WHERE id=$1",
                self.shard(workspace),
                delta,
            )
            if role == "nodes":
                await conn.execute("UPDATE computer_routes SET active=0,snapshot=NULL,snapshot_hash=NULL,last_seen=NULL WHERE gateway_id=$1",value)
            if role == "devices":
                await conn.execute("DELETE FROM pushes WHERE device_id=$1", value)
                await self._delete_jobs(conn, value)
            await self._notify(conn, "workspace:" + workspace)
            if role == "nodes":
                await self._notify(conn, "node:" + value)
            return True

    async def claim_push_jobs(self, limit=4, lease_ttl=60):
        now, lease = time.time(), str(uuid.uuid4())
        rows = await self.pool.fetch(
            "UPDATE push_jobs SET lease_token=$3,lease_until=$4 WHERE id IN (SELECT id FROM push_jobs WHERE next_at<=$1 AND attempts<5 AND (lease_until IS NULL OR lease_until<=$1) ORDER BY next_at,id LIMIT $2 FOR UPDATE SKIP LOCKED) RETURNING *",
            now,
            limit,
            lease,
            now + lease_ttl,
        )
        return [dict(r) for r in rows]

    async def push_registration(self, device):
        row = await self.pool.fetchrow(
            "SELECT p.* FROM pushes p JOIN devices d ON d.id=p.device_id WHERE p.device_id=$1 AND d.revoked=0",
            device,
        )
        return dict(row) if row else None

    async def finish_push(self, job, delivered, invalid, registration):
        async with self.pool.acquire(timeout=5) as conn, conn.transaction():
            await conn.fetchrow("SELECT id FROM global_usage WHERE id=1 FOR UPDATE")
            workspace = await conn.fetchval(
                "SELECT workspace_id FROM devices WHERE id=$1", job["device_id"]
            )
            await self._usage(conn, workspace)
            if delivered or invalid or job["attempts"] >= 4 or registration is None:
                deleted = await conn.fetchval(
                    "DELETE FROM push_jobs WHERE id=$1 AND lease_token=$2 RETURNING true",
                    job["id"],
                    job["lease_token"],
                )
                if deleted:
                    await conn.execute(
                        "UPDATE workspace_usage SET push_jobs=push_jobs-1 WHERE workspace_id=$1",
                        workspace,
                    )
                    await conn.execute(
                        "UPDATE global_usage SET push_jobs=push_jobs-1 WHERE id=1"
                    )
                    if invalid and registration:
                        await conn.execute(
                            "DELETE FROM pushes WHERE device_id=$1 AND provider=$2 AND target=$3",
                            job["device_id"],
                            registration["provider"],
                            registration["target"],
                        )
            else:
                await conn.execute(
                    "UPDATE push_jobs SET attempts=attempts+1,next_at=$3,lease_token=NULL,lease_until=NULL WHERE id=$1 AND lease_token=$2",
                    job["id"],
                    job["lease_token"],
                    time.time() + min(3600, 5 * 2 ** job["attempts"]),
                )

    async def acquire_poll(
        self, role, credential_id, workspace_id, ttl, max_credential, max_workspace
    ):
        assert role in ("nodes", "devices")
        now, lease = time.time(), str(uuid.uuid4())
        async with self.pool.acquire(timeout=5) as conn, conn.transaction():
            usage = await self._usage(conn, workspace_id)
            if not await conn.fetchval(
                f"SELECT true FROM {role} WHERE id=$1 AND workspace_id=$2 AND revoked=0 FOR SHARE",
                credential_id,
                workspace_id,
            ):
                return None
            expired = await conn.fetch(
                "DELETE FROM poll_leases WHERE id IN (SELECT id FROM poll_leases WHERE workspace_id=$1 AND expires<=$2 ORDER BY expires LIMIT 256) RETURNING role,credential_id",
                workspace_id,
                now,
            )
            for old in expired:
                await conn.execute(
                    "UPDATE poll_credentials SET active=active-1 WHERE role=$1 AND credential_id=$2",
                    old["role"],
                    old["credential_id"],
                )
            if expired:
                await conn.execute(
                    "UPDATE workspace_usage SET active_polls=active_polls-$2 WHERE workspace_id=$1",
                    workspace_id,
                    len(expired),
                )
                usage = await conn.fetchrow(
                    "SELECT * FROM workspace_usage WHERE workspace_id=$1", workspace_id
                )
            await conn.execute(
                "INSERT INTO poll_credentials(role,credential_id) VALUES($1,$2) ON CONFLICT DO NOTHING",
                role,
                credential_id,
            )
            count = await conn.fetchval(
                "SELECT active FROM poll_credentials WHERE role=$1 AND credential_id=$2 FOR UPDATE",
                role,
                credential_id,
            )
            if count >= max_credential or usage["active_polls"] >= max_workspace:
                return None
            await conn.execute(
                "INSERT INTO poll_leases VALUES($1,$2,$3,$4,$5)",
                lease,
                role,
                credential_id,
                workspace_id,
                now + ttl,
            )
            await conn.execute(
                "UPDATE poll_credentials SET active=active+1 WHERE role=$1 AND credential_id=$2",
                role,
                credential_id,
            )
            await conn.execute(
                "UPDATE workspace_usage SET active_polls=active_polls+1 WHERE workspace_id=$1",
                workspace_id,
            )
            return lease

    async def release_poll(self, lease):
        # A second disconnect/server cancellation must not interrupt SQL cleanup
        # after a lease was admitted. Deletion is idempotent and has dedicated
        # bounded acquisition capacity independent of ordinary request pressure.
        task = asyncio.create_task(self._release_poll(lease))
        try:
            await asyncio.shield(task)
        except asyncio.CancelledError:
            while not task.done():
                try:
                    await asyncio.shield(task)
                except asyncio.CancelledError:
                    pass
                except Exception:
                    break
            if task.done() and not task.cancelled():
                task.exception()
            raise

    async def _release_poll(self, lease):
        async with self.pool.acquire(
            timeout=30, cleanup=True
        ) as conn, conn.transaction():
            row = await conn.fetchrow("SELECT * FROM poll_leases WHERE id=$1", lease)
            if not row:
                return
            await self._usage(conn, row["workspace_id"])
            deleted = await conn.fetchval(
                "DELETE FROM poll_leases WHERE id=$1 RETURNING true", lease
            )
            if deleted:
                await conn.execute(
                    "UPDATE workspace_usage SET active_polls=active_polls-1 WHERE workspace_id=$1",
                    row["workspace_id"],
                )
                await conn.execute(
                    "UPDATE poll_credentials SET active=active-1 WHERE role=$1 AND credential_id=$2",
                    row["role"],
                    row["credential_id"],
                )

    async def rate_limit(self, key, limit, seconds, max_entries=100000):
        bucket = (
            2
            if isinstance(key, (tuple, list)) and key and key[0] in ("devices", "nodes")
            else 1
        )
        key = digest(canonical(key))
        now = time.time()
        async with self.pool.acquire(timeout=5) as conn, conn.transaction():
            count = await conn.fetchval(
                "UPDATE rate_limits SET count=CASE WHEN expires<=$2 THEN 1 ELSE count+1 END,expires=CASE WHEN expires<=$2 THEN $3 ELSE expires END WHERE key=$1 RETURNING count",
                key,
                now,
                now + seconds,
            )
            if count is not None:
                return count <= limit
            # Only creation takes the cardinality gate. Existing authenticated
            # identities update independent rows without a global mutex.
            await conn.fetchval(
                "SELECT entries FROM rate_control WHERE id=$1 FOR UPDATE", bucket
            )
            count = await conn.fetchval(
                "SELECT count FROM rate_limits WHERE key=$1", key
            )
            if count is not None:
                count = await conn.fetchval(
                    "UPDATE rate_limits SET count=count+1 WHERE key=$1 RETURNING count",
                    key,
                )
                return count <= limit
            if not await conn.fetchval(
                "UPDATE rate_control SET entries=entries+1 WHERE id=$2 AND entries<$1 RETURNING true",
                max_entries,
                bucket,
            ):
                return False
            await conn.execute(
                "INSERT INTO rate_limits(key,count,expires,bucket) VALUES($1,1,$2,$3)",
                key,
                now + seconds,
                bucket,
            )
            return True

    async def maintain(
        self,
        *,
        queue_ttl=120,
        claim_ttl=90,
        retention=7 * 86400,
        batch_size=256,
        restart=False,
    ):
        # One short transaction per row group. Advisory leadership is transaction
        # scoped: cancellation or worker loss cannot strand a maintenance leader.
        batch_size = max(1, min(batch_size, 4096))
        now = time.time()
        async with self.pool.acquire(timeout=5) as conn, conn.transaction():
            if not await conn.fetchval("SELECT pg_try_advisory_xact_lock(735628110)"):
                return
            candidates = await conn.fetch(
                "(SELECT id,workspace_id FROM requests WHERE state='queued' AND created<=$1 ORDER BY created LIMIT $4) UNION (SELECT id,workspace_id FROM requests WHERE state='queued' AND expires_at<=$2 ORDER BY expires_at LIMIT $4) UNION (SELECT id,workspace_id FROM requests WHERE state='claimed' AND claimed<=$3 ORDER BY claimed LIMIT $4) LIMIT $4",
                now - queue_ttl,
                now,
                now - claim_ttl,
                batch_size,
            )
            for shard in sorted({self.shard(r["workspace_id"]) for r in candidates}):
                await conn.fetchval(
                    "SELECT payload_bytes FROM payload_shards WHERE id=$1 FOR UPDATE",
                    shard,
                )
            for workspace in sorted({r["workspace_id"] for r in candidates}):
                await self._usage(conn, workspace)
            for candidate in candidates:
                row = await conn.fetchrow(
                    "SELECT id,workspace_id,state,created,claimed,expires_at,reserved_bytes,result_bytes FROM requests WHERE id=$1 FOR UPDATE",
                    candidate["id"],
                )
                if row:
                    await self._expire(conn, row, queue_ttl, claim_ttl)
        async with self.pool.acquire(timeout=5) as conn, conn.transaction():
            if not await conn.fetchval("SELECT pg_try_advisory_xact_lock(735628111)"):
                return
            # Independently indexed bounded retention classes.
            candidates = await conn.fetch(
                f"(SELECT id,workspace_id FROM requests WHERE operation='terminal_snapshot' AND state IN ('completed','failed','uncertain') AND updated<$1 ORDER BY updated LIMIT $4) UNION (SELECT id,workspace_id FROM requests WHERE operation IN {READ_SQL} AND operation!='terminal_snapshot' AND state IN ('completed','failed','uncertain') AND updated<$2 ORDER BY updated LIMIT $4) UNION (SELECT id,workspace_id FROM requests WHERE body!='' AND state IN ('completed','failed','uncertain') AND updated<$3 ORDER BY updated LIMIT $4) UNION (SELECT id,workspace_id FROM requests WHERE operation='attachment_read' AND state IN ('completed','failed','uncertain') AND (updated<$5 OR result_read<$6) ORDER BY updated LIMIT $4) LIMIT $4",
                now - 120,
                now - min(retention, 3600),
                now - retention,
                batch_size,
                now - ATTACHMENT_RESULT_TTL,
                now - ATTACHMENT_READ_GRACE,
            )
            for shard in sorted({self.shard(r["workspace_id"]) for r in candidates}):
                await conn.fetchval(
                    "SELECT payload_bytes FROM payload_shards WHERE id=$1 FOR UPDATE",
                    shard,
                )
            for workspace in sorted({r["workspace_id"] for r in candidates}):
                await self._usage(conn, workspace)
            for candidate in candidates:
                row = await conn.fetchrow(
                    "SELECT id,workspace_id,operation,state,updated,result_read,body_bytes,result_bytes,reserved_bytes,(body!='') AS has_body FROM requests WHERE id=$1 FOR UPDATE",
                    candidate["id"],
                )
                if not row or row["state"] not in TERMINAL:
                    continue
                remove = (
                    row["operation"] == "terminal_snapshot"
                    and row["updated"] < now - 120
                ) or (
                    row["operation"] == "attachment_read"
                    and (row["updated"] < now - ATTACHMENT_RESULT_TTL
                         or (row["result_read"] is not None and row["result_read"] < now - ATTACHMENT_READ_GRACE))
                ) or (
                    row["operation"] in READ_OPERATIONS
                    and row["operation"] != "terminal_snapshot"
                    and row["updated"] < now - min(retention, 3600)
                )
                scrub = row["updated"] < now - retention and row["has_body"]
                if not remove and not scrub:
                    continue
                delta = self._bytes(row)
                if remove:
                    await conn.execute("DELETE FROM requests WHERE id=$1", row["id"])
                    field = (
                        "reads" if row["operation"] in READ_OPERATIONS else "mutations"
                    )
                    await conn.execute(
                        f"UPDATE workspace_usage SET {field}={field}-1,payload_bytes=payload_bytes-$2 WHERE workspace_id=$1",
                        row["workspace_id"],
                        delta,
                    )
                else:
                    marker = "request history expired; delivery must not be retried"
                    delta -= len(marker.encode())
                    await conn.execute(
                        "UPDATE requests SET body='',body_bytes=0,result=NULL,result_bytes=$2,reserved_bytes=0,error=$3 WHERE id=$1",
                        row["id"],
                        len(marker.encode()),
                        marker,
                    )
                    await conn.execute(
                        "UPDATE workspace_usage SET payload_bytes=payload_bytes-$2 WHERE workspace_id=$1",
                        row["workspace_id"],
                        delta,
                    )
                await conn.execute(
                    "UPDATE payload_shards SET payload_bytes=payload_bytes-$1 WHERE id=$2",
                    delta,
                    self.shard(row["workspace_id"]),
                )
        async with self.pool.acquire(timeout=5) as conn, conn.transaction():
            if not await conn.fetchval("SELECT pg_try_advisory_xact_lock(735628112)"):
                return
            await conn.fetchrow("SELECT id FROM global_usage WHERE id=1 FOR UPDATE")
            events = await conn.fetch(
                "SELECT id,workspace_id FROM events WHERE created<$1 ORDER BY created,id LIMIT $2",
                now - retention,
                batch_size,
            )
            for workspace in sorted({r["workspace_id"] for r in events}):
                await self._usage(conn, workspace)
            for event in events:
                if await conn.fetchval(
                    "DELETE FROM events WHERE id=$1 RETURNING true", event["id"]
                ):
                    await conn.execute(
                        "UPDATE workspace_usage SET events=events-1 WHERE workspace_id=$1",
                        event["workspace_id"],
                    )
                    await conn.execute(
                        "UPDATE global_usage SET events=events-1 WHERE id=1"
                    )
        async with self.pool.acquire(timeout=5) as conn, conn.transaction():
            if not await conn.fetchval("SELECT pg_try_advisory_xact_lock(735628113)"):
                return
            await conn.fetchrow("SELECT id FROM global_usage WHERE id=1 FOR UPDATE")
            jobs = await conn.fetch(
                "SELECT j.id,d.workspace_id FROM push_jobs j JOIN devices d ON d.id=j.device_id WHERE j.created<$1 ORDER BY j.created,j.id LIMIT $2",
                now - 86400,
                batch_size,
            )
            for workspace in sorted({r["workspace_id"] for r in jobs}):
                await self._usage(conn, workspace)
            for job in jobs:
                if await conn.fetchval(
                    "DELETE FROM push_jobs WHERE id=$1 RETURNING true", job["id"]
                ):
                    await conn.execute(
                        "UPDATE workspace_usage SET push_jobs=push_jobs-1 WHERE workspace_id=$1",
                        job["workspace_id"],
                    )
                    await conn.execute(
                        "UPDATE global_usage SET push_jobs=push_jobs-1 WHERE id=1"
                    )
            await conn.execute(
                "DELETE FROM invitations WHERE code_hash IN (SELECT code_hash FROM invitations WHERE expires<=$1 ORDER BY expires LIMIT $2)",
                now,
                batch_size,
            )
            await conn.fetch("SELECT entries FROM rate_control ORDER BY id FOR UPDATE")
            deleted_rates = await conn.fetch(
                "WITH deleted AS (DELETE FROM rate_limits WHERE key IN (SELECT key FROM rate_limits WHERE expires<=$1 ORDER BY expires LIMIT $2) RETURNING bucket) SELECT bucket,count(*) AS count FROM deleted GROUP BY bucket",
                now,
                batch_size,
            )
            for row in deleted_rates:
                await conn.execute(
                    "UPDATE rate_control SET entries=entries-$2 WHERE id=$1",
                    row["bucket"],
                    row["count"],
                )
        # Leases released outside a maintenance transaction to preserve the same
        # workspace->credential lock order as ordinary admission and release.
        leases = await self.pool.fetch(
            "SELECT id FROM poll_leases WHERE expires<=$1 ORDER BY expires LIMIT $2",
            now,
            batch_size,
        )
        for lease in leases:
            await self.release_poll(lease["id"])
