"""Optional multi-worker PostgreSQL regressions on an explicitly disposable DB.

Set ZERUS_RELAY_TEST_DSN to a loopback PostgreSQL database named
relay_regressions or zerus_test_*. Its public schema is RESET before each test.
No configured service database or production credentials are read implicitly.
"""

import asyncio
import importlib
import os
from pathlib import Path
import tempfile
import time
import unittest
from urllib.parse import urlsplit
import uuid


TEST_DSN = os.environ.get("ZERUS_RELAY_TEST_DSN", "")
MIB = 1024 * 1024


@unittest.skipUnless(TEST_DSN, "requires an explicitly disposable ZERUS_RELAY_TEST_DSN")
class PostgresResourceTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        parsed = urlsplit(TEST_DSN)
        database = parsed.path.lstrip("/")
        if (
            parsed.scheme not in {"postgres", "postgresql"}
            or parsed.hostname not in {"127.0.0.1", "localhost", "::1"}
            or not (
                database == "relay_regressions" or database.startswith("zerus_test_")
            )
        ):
            raise ValueError(
                "test DSN must name a disposable relay_regressions/zerus_test_* loopback database"
            )
        import asyncpg

        module = importlib.import_module("zerus_mobile.postgres")
        self.store_type = module.PostgresStore
        connection = await asyncpg.connect(TEST_DSN)
        try:
            await connection.execute("DROP SCHEMA public CASCADE; CREATE SCHEMA public")
        finally:
            await connection.close()
        self.stores = []

    async def asyncTearDown(self):
        for store in reversed(self.stores):
            await store.close()

    async def open(self, global_max_bytes=16 * MIB):
        store = await self.store_type.open(
            TEST_DSN,
            pool_min=2,
            pool_max=4,
            global_max_bytes=global_max_bytes,
            quota_shards=1,
        )
        self.stores.append(store)
        return store

    async def identity(self, store, label="Synthetic workspace"):
        workspace = await store.workspace(label)
        node = await store.node(workspace, "Synthetic computer")
        phone = await store.pair(
            (await store.invite(workspace))["pair_code"], "Synthetic phone"
        )
        device = await store.authenticate(phone["device_token"], "devices")
        node_row = await store.authenticate(node["node_token"], "nodes")
        return workspace, node, phone, device, node_row

    def command(self, node, operation="inspect"):
        request_id = str(uuid.uuid4())
        payload = (
            {}
            if operation == "inspect"
            else dict(
                request_id=request_id,
                expected_run_id="synthetic-run",
                expected_conversation_id="synthetic-conversation",
                text="Synthetic text",
            )
        )
        return dict(
            request_id=request_id,
            computer_id=node["node_id"],
            operation=operation,
            session="synthetic-session",
            payload=payload,
        )

    async def test_concurrent_workers_enforce_one_queue_and_one_claim(self):
        first, second = await self.open(), await self.open()
        _, node, _, device, node_row = await self.identity(first)
        values = [self.command(node, "send"), self.command(node, "send")]
        results = await asyncio.gather(
            first.submit(device, values[0], 1, 8 * MIB),
            second.submit(device, values[1], 1, 8 * MIB),
        )
        self.assertEqual(sorted(result[0] for result in results), [202, 429])
        admitted = values[
            next(index for index, result in enumerate(results) if result[0] == 202)
        ]
        claimed = await asyncio.gather(first.claim(node_row), second.claim(node_row))
        self.assertEqual(
            [row["request_id"] for batch in claimed for row in batch],
            [admitted["request_id"]],
        )
        result = dict(state="completed", result={"status": "submitted"}, error=None)
        self.assertEqual(
            await second.result(node_row, admitted["request_id"], result), 200
        )
        self.assertEqual(
            await first.result(node_row, admitted["request_id"], result), 200
        )
        self.assertEqual(
            (await second.submit(device, admitted, 1, 8 * MIB))[1]["state"], "completed"
        )
        self.assertEqual(await first.claim(node_row), [])

    async def test_large_results_do_not_bypass_retained_workspace_budget(self):
        first, second = await self.open(), await self.open()
        _, node, _, device, node_row = await self.identity(first)
        accepted = []
        for _ in range(8):
            command = self.command(node)
            status, _ = await first.submit(device, command, 200, 3 * MIB)
            if status == 429:
                break
            self.assertEqual(status, 202)
            self.assertEqual(
                (await second.claim(node_row))[0]["request_id"], command["request_id"]
            )
            body = dict(
                state="completed", result={"synthetic": "x" * 800000}, error=None
            )
            self.assertEqual(
                await second.result(node_row, command["request_id"], body), 200
            )
            self.assertEqual(
                await first.result(node_row, command["request_id"], body), 200
            )
            accepted.append(command)
        else:
            self.fail("eight 800KiB results bypassed the 3MiB retained budget")
        self.assertGreater(len(accepted), 0)
        other = await self.identity(first, "Other synthetic workspace")
        self.assertEqual(
            (await second.submit(other[3], self.command(other[1]), 200, 3 * MIB))[0],
            202,
        )
        for command in accepted:
            duplicate = await first.submit(device, command, 200, 3 * MIB)
            self.assertEqual(duplicate[0], 202)
            self.assertEqual(duplicate[1]["state"], "completed")
        self.assertEqual(await second.claim(node_row), [])

    async def test_global_reservations_are_atomic_across_workspaces_and_workers(self):
        first, second = await self.open(2 * MIB), await self.open(2 * MIB)
        identities = [
            await self.identity(first, f"Synthetic {index}") for index in range(4)
        ]
        results = await asyncio.gather(
            *(
                store.submit(identity[3], self.command(identity[1]), 200, 8 * MIB)
                for store, identity in zip((first, second, first, second), identities)
            )
        )
        self.assertTrue(any(result[0] == 202 for result in results))
        self.assertTrue(any(result[0] == 429 for result in results), results)
        self.assertTrue(all(result[0] in {202, 429} for result in results))

    async def test_expired_claim_survives_worker_reopen_without_replay(self):
        first = await self.open()
        _, node, _, device, node_row = await self.identity(first)
        command = self.command(node, "send")
        self.assertEqual((await first.submit(device, command, 200, 8 * MIB))[0], 202)
        self.assertEqual(len(await first.claim(node_row)), 1)
        await first.close()
        self.stores.remove(first)
        reopened = await self.open()
        receipt = await reopened.get_request(device, command["request_id"], claim_ttl=0)
        self.assertEqual(receipt["state"], "uncertain")
        self.assertEqual(
            (await reopened.submit(device, command, 200, 8 * MIB))[1]["state"],
            "uncertain",
        )
        self.assertEqual(await reopened.claim(node_row), [])
        self.assertEqual(
            await reopened.result(
                node_row,
                command["request_id"],
                dict(state="completed", result={"status": "submitted"}, error=None),
            ),
            409,
        )

    async def test_revocation_and_submit_race_preserves_no_replay(self):
        first, second = await self.open(), await self.open()
        _, node, phone, device, node_row = await self.identity(first)
        command = self.command(node, "send")
        submitted, revoked = await asyncio.gather(
            first.submit(device, command, 200, 8 * MIB),
            second.revoke("devices", phone["device_id"]),
        )
        self.assertIn(submitted[0], (202, 401))
        self.assertTrue(revoked)
        self.assertEqual(
            (await first.submit(device, self.command(node, "send"), 200, 8 * MIB))[0],
            401,
        )
        self.assertEqual(await second.claim(node_row), [])

    async def test_revocation_and_claim_race_never_replays_mutation(self):
        first, second = await self.open(), await self.open()
        _, node, _, device, node_row = await self.identity(first)
        command = self.command(node, "send")
        self.assertEqual((await first.submit(device, command, 200, 8 * MIB))[0], 202)
        claimed, revoked = await asyncio.gather(
            first.claim(node_row), second.revoke("nodes", node["node_id"])
        )
        self.assertLessEqual(len(claimed), 1)
        self.assertTrue(revoked)
        self.assertEqual(await first.claim(node_row), [])
        receipt = await first.get_request(device, command["request_id"])
        self.assertIn(receipt["state"], ("failed", "uncertain"))
        self.assertEqual(
            await first.result(
                node_row,
                command["request_id"],
                dict(state="completed", result={"status": "submitted"}, error=None),
            ),
            401,
        )

    async def test_revocation_and_result_race_is_atomic(self):
        first, second = await self.open(), await self.open()
        _, node, _, device, node_row = await self.identity(first)
        command = self.command(node, "send")
        self.assertEqual((await first.submit(device, command, 200, 8 * MIB))[0], 202)
        self.assertEqual(len(await first.claim(node_row)), 1)
        body = dict(state="completed", result={"status": "submitted"}, error=None)
        status, revoked = await asyncio.gather(
            first.result(node_row, command["request_id"], body),
            second.revoke("nodes", node["node_id"]),
        )
        self.assertIn(status, (200, 401))
        self.assertTrue(revoked)
        self.assertEqual(await first.result(node_row, command["request_id"], body), 401)
        receipt = await first.get_request(device, command["request_id"])
        self.assertEqual(
            receipt["state"], "completed" if status == 200 else "uncertain"
        )
        self.assertEqual(await second.claim(node_row), [])

    async def test_offline_sqlite_migration_preserves_tokens_receipts_and_tombstones(
        self,
    ):
        from zerus_mobile.store import Store
        from zerus_mobile.migration import migrate_sqlite

        with tempfile.TemporaryDirectory(prefix="zerus-relay-migration-") as directory:
            path = Path(directory) / "relay.sqlite3"
            source = Store(path)
            try:
                workspace = source.workspace("Synthetic migration")
                node = source.node(workspace, "Synthetic computer")
                phone = source.pair(
                    source.invite(workspace)["pair_code"], "Synthetic phone"
                )
                device = source.authenticate(phone["device_token"], "devices")
                node_row = source.authenticate(node["node_token"], "nodes")
                command = self.command(node, "send")
                self.assertEqual(source.submit(device, command, 200)[0], 202)
                source.claim(node_row)
                source.maintain(restart=True)
            finally:
                source.close()
            await migrate_sqlite(
                path, TEST_DSN, global_max_bytes=16 * MIB, quota_shards=1
            )
            destination = await self.open()
            device = await destination.authenticate(phone["device_token"], "devices")
            node_row = await destination.authenticate(node["node_token"], "nodes")
            self.assertIsNotNone(device)
            self.assertIsNotNone(node_row)
            duplicate = await destination.submit(device, command, 200, 8 * MIB)
            self.assertEqual(duplicate[0], 202)
            self.assertEqual(duplicate[1]["state"], "uncertain")
            self.assertEqual(await destination.claim(node_row), [])
            with self.assertRaises((ValueError, RuntimeError)):
                await migrate_sqlite(
                    path, TEST_DSN, global_max_bytes=16 * MIB, quota_shards=1
                )

    async def assert_payload_ledger(self, store):
        actual = await store.pool.fetchval(
            "SELECT COALESCE(sum(body_bytes+result_bytes+reserved_bytes),0) FROM requests"
        )
        self.assertEqual(
            await store.pool.fetchval(
                "SELECT COALESCE(sum(payload_bytes),0) FROM workspace_usage"
            ),
            actual,
        )
        self.assertEqual(
            await store.pool.fetchval(
                "SELECT COALESCE(sum(payload_bytes),0) FROM payload_shards"
            ),
            actual,
        )
        self.assertFalse(
            await store.pool.fetchval(
                "SELECT EXISTS(SELECT 1 FROM workspace_usage WHERE payload_bytes<0 OR active<0)"
            )
        )

    async def test_expiry_revoke_and_tombstone_charge_exact_error_bytes(self):
        first, second = await self.open(), await self.open()
        workspace, node, phone, device, node_row = await self.identity(first)
        command = self.command(node, "send")
        await first.submit(device, command, 200, 8 * MIB)
        await first.claim(node_row)
        receipt = await second.get_request(device, command["request_id"], claim_ttl=0)
        self.assertEqual(receipt["state"], "uncertain")
        row = await first.pool.fetchrow(
            "SELECT * FROM requests WHERE id=$1", command["request_id"]
        )
        self.assertEqual(row["reserved_bytes"], 0)
        self.assertEqual(row["result_bytes"], len(row["error"].encode()))
        await self.assert_payload_ledger(first)
        second_command = self.command(node, "send")
        await second.submit(device, second_command, 200, 8 * MIB)
        await first.revoke("nodes", node["node_id"])
        await self.assert_payload_ledger(first)
        await first.pool.execute(
            "UPDATE requests SET updated=0 WHERE id=$1", command["request_id"]
        )
        await first.maintain(retention=1)
        row = await first.pool.fetchrow(
            "SELECT * FROM requests WHERE id=$1", command["request_id"]
        )
        self.assertEqual(row["body"], "")
        self.assertEqual(row["result_bytes"], len(row["error"].encode()))
        self.assertEqual(
            row["body_hash"],
            __import__("zerus_mobile.store", fromlist=["digest"]).digest(
                __import__("zerus_mobile.store", fromlist=["canonical"]).canonical(
                    command
                )
            ),
        )
        await self.assert_payload_ledger(first)
        self.assertEqual(
            (await second.submit(device, command, 200, 8 * MIB))[1]["state"],
            "uncertain",
        )

    async def test_shared_poll_expiry_recovery_and_idempotent_release(self):
        first, second = await self.open(), await self.open()
        workspace, node, _, _, _ = await self.identity(first)
        # The lease must outlive its committed acquisition and the second
        # worker's check; 10 ms could expire first on a loaded durable database.
        lease = await first.acquire_poll(
            "nodes", node["node_id"], workspace, 0.5, 1, 1
        )
        self.assertIsNotNone(lease)
        self.assertIsNone(
            await second.acquire_poll("nodes", node["node_id"], workspace, 1, 1, 1)
        )
        await asyncio.sleep(0.6)
        replacement = await second.acquire_poll(
            "nodes", node["node_id"], workspace, 1, 1, 1
        )
        self.assertIsNotNone(replacement)
        await first.release_poll(lease)
        await second.release_poll(replacement)
        await second.release_poll(replacement)
        self.assertEqual(
            await first.pool.fetchval(
                "SELECT active_polls FROM workspace_usage WHERE workspace_id=$1",
                workspace,
            ),
            0,
        )
        self.assertEqual(
            await first.pool.fetchval("SELECT sum(active) FROM poll_credentials"), 0
        )

    async def test_anonymous_rate_cardinality_cannot_consume_authenticated_lane(self):
        first, second = await self.open(), await self.open()
        self.assertTrue(await first.rate_limit(("login", "synthetic-one"), 2, 60, 2))
        self.assertTrue(await second.rate_limit(("login", "synthetic-two"), 2, 60, 2))
        self.assertFalse(await first.rate_limit(("login", "synthetic-three"), 2, 60, 2))
        self.assertTrue(
            await second.rate_limit(("devices", "synthetic-device"), 2, 60, 2)
        )
        self.assertTrue(
            await first.rate_limit(("devices", "synthetic-device"), 2, 60, 2)
        )
        self.assertFalse(
            await second.rate_limit(("devices", "synthetic-device"), 2, 60, 2)
        )
        await first.pool.execute("UPDATE rate_limits SET expires=0")
        await second.maintain()
        self.assertEqual(
            await first.pool.fetchval("SELECT sum(entries) FROM rate_control"), 0
        )

    async def test_listener_reconnect_and_unchanged_heartbeat_do_not_wake(self):
        first, second = await self.open(), await self.open()
        workspace, _, _, _, node_row = await self.identity(first)
        topic = "workspace:" + workspace
        await first.heartbeat(node_row, {"sessions": []})
        await asyncio.sleep(0.05)
        generation = second.generation(topic)
        waiter = asyncio.create_task(second.wait(topic, generation, 1))
        await first.heartbeat(node_row, {"sessions": []})
        await asyncio.sleep(0.02)
        self.assertFalse(waiter.done())
        await first.heartbeat(
            node_row, {"sessions": [{"name": "fixture", "phase": "input"}]}
        )
        await asyncio.wait_for(waiter, 1)
        old = second.listener
        old.terminate()
        for _ in range(200):
            if second.listener is not None and second.listener is not old:
                break
            await asyncio.sleep(0.01)
        self.assertIsNot(second.listener, old)
        generation = second.generation(topic)
        waiter = asyncio.create_task(second.wait(topic, generation, 2))
        await first.heartbeat(
            node_row, {"sessions": [{"name": "fixture", "phase": "error"}]}
        )
        await asyncio.wait_for(waiter, 2)

    async def test_push_claim_leases_ignore_stale_finisher(self):
        first, second = await self.open(), await self.open()
        workspace, node, phone, _, node_row = await self.identity(first)
        await first.register_push(
            phone["device_id"], "fcm", "fixture-token"
        )
        await first.heartbeat(node_row, {"sessions": []})
        await first.heartbeat(
            node_row, {"sessions": [{"name": "fixture", "phase": "input"}]}
        )
        batches = await asyncio.gather(
            first.claim_push_jobs(), second.claim_push_jobs()
        )
        jobs = [job for batch in batches for job in batch]
        self.assertEqual(len(jobs), 1)
        old = jobs[0]
        await first.pool.execute("UPDATE push_jobs SET lease_until=0")
        replacement = (await second.claim_push_jobs())[0]
        registration = await first.push_registration(phone["device_id"])
        await first.finish_push(old, True, False, registration)
        self.assertEqual(await first.pool.fetchval("SELECT count(*) FROM push_jobs"), 1)
        await second.finish_push(replacement, True, False, registration)
        self.assertEqual(await first.pool.fetchval("SELECT count(*) FROM push_jobs"), 0)
        self.assertEqual(
            await first.pool.fetchval("SELECT push_jobs FROM global_usage WHERE id=1"),
            0,
        )

    async def test_worker_allocation_mismatch_rejected(self):
        await self.open()
        with self.assertRaises(ValueError):
            await self.store_type.open(
                TEST_DSN, global_max_bytes=32 * MIB, quota_shards=1
            )

    async def test_migration_preserves_deleted_event_sequence_high_water_and_digests(
        self,
    ):
        from zerus_mobile.store import Store
        from zerus_mobile.migration import migrate_sqlite

        with tempfile.TemporaryDirectory(
            prefix="zerus-sequence-migration-"
        ) as directory:
            path = Path(directory) / "relay.sqlite3"
            source = Store(path)
            workspace = source.workspace("Synthetic")
            node = source.node(workspace, "Synthetic")
            phone = source.pair(source.invite(workspace)["pair_code"], "Synthetic")
            node_row = source.authenticate(node["node_token"], "nodes")
            with source.db:
                source.db.execute(
                    "INSERT INTO events(id,workspace_id,node_id,session,kind,created) VALUES(50000,?,?,?,?,?)",
                    (workspace, node["node_id"], "fixture", "attention", 1.0),
                )
                source.db.execute("DELETE FROM events")
            source.close()
            report = await migrate_sqlite(
                path, TEST_DSN, global_max_bytes=16 * MIB, quota_shards=1
            )
            self.assertEqual(report["events"], 0)
            self.assertEqual(len(report["digests"]["events"]), 64)
            destination = await self.open()
            node_row = await destination.authenticate(node["node_token"], "nodes")
            await destination.heartbeat(node_row, {"sessions": []})
            await destination.heartbeat(
                node_row, {"sessions": [{"name": "fixture", "phase": "input"}]}
            )
            events = await destination.events(workspace, 50000)
            self.assertEqual(len(events), 1)
            self.assertEqual(events[0]["id"], 50001)

    async def test_event_caps_recycle_oldest_and_preserve_fresh_attention(self):
        store = await self.open()
        workspace, node, _, _, node_row = await self.identity(store)
        await store.heartbeat(node_row, {"sessions": []})
        await store.pool.execute(
            "INSERT INTO events(workspace_id,node_id,session,kind,created) SELECT $1,$2,'old','attention',0 FROM generate_series(1,10000)",
            workspace,
            node["node_id"],
        )
        await store.pool.execute(
            "UPDATE workspace_usage SET events=10000 WHERE workspace_id=$1", workspace
        )
        await store.pool.execute("UPDATE global_usage SET events=10000 WHERE id=1")
        await store.heartbeat(
            node_row, {"sessions": [{"name": "fresh", "phase": "input"}]}
        )
        self.assertEqual(
            await store.pool.fetchval("SELECT count(*) FROM events"), 10000
        )
        self.assertEqual((await store.events(workspace, 10000))[0]["session"], "fresh")
        self.assertEqual(await store.pool.fetchval("SELECT min(id) FROM events"), 2)
        # Fill the fleet cap in other workspaces, then admit a new tenant event.
        for index in range(9):
            old_workspace, old_node, _, _, _ = await self.identity(
                store, f"Synthetic old {index}"
            )
            await store.pool.execute(
                "INSERT INTO events(workspace_id,node_id,session,kind,created) SELECT $1,$2,'old','attention',0 FROM generate_series(1,10000)",
                old_workspace,
                old_node["node_id"],
            )
            await store.pool.execute(
                "UPDATE workspace_usage SET events=10000 WHERE workspace_id=$1",
                old_workspace,
            )
        await store.pool.execute("UPDATE global_usage SET events=100000 WHERE id=1")
        new_workspace, _, _, _, new_node_row = await self.identity(
            store, "Synthetic fresh tenant"
        )
        await store.heartbeat(new_node_row, {"sessions": []})
        await store.heartbeat(
            new_node_row, {"sessions": [{"name": "fresh tenant", "phase": "input"}]}
        )
        self.assertEqual(
            await store.pool.fetchval("SELECT count(*) FROM events"), 100000
        )
        self.assertEqual(
            await store.pool.fetchval("SELECT sum(events) FROM workspace_usage"), 100000
        )
        self.assertEqual(
            (await store.events(new_workspace, 0))[0]["session"], "fresh tenant"
        )

    async def test_retention_index_skips_large_mutation_tombstone_population(self):
        from zerus_mobile.context import READ_SQL

        store = await self.open()
        workspace, node, phone, _, _ = await self.identity(store)
        await store.pool.execute(
            "INSERT INTO requests(id,workspace_id,device_id,node_id,operation,body_hash,body_bytes,body,state,created,updated) SELECT 'tombstone-'||value,$1,$2,$3,'send','synthetic',0,'','completed',0,0 FROM generate_series(1,100000) value",
            workspace,
            phone["device_id"],
            node["node_id"],
        )
        await store.pool.execute(
            "INSERT INTO requests(id,workspace_id,device_id,node_id,operation,body_hash,body_bytes,body,state,created,updated) VALUES('old-read',$1,$2,$3,'dirs','synthetic',0,'','completed',0,0)",
            workspace,
            phone["device_id"],
            node["node_id"],
        )
        await store.pool.execute("ANALYZE requests")
        plan = await store.pool.fetch(
            f"EXPLAIN SELECT id FROM requests WHERE operation IN {READ_SQL} AND operation!='terminal_snapshot' AND state IN ('completed','failed','uncertain') AND updated<$1 ORDER BY updated LIMIT 256",
            1.0,
        )
        self.assertIn("read_history", " ".join(row[0] for row in plan))

    async def test_migration_overbudget_is_atomic_and_requires_empty_target(self):
        from zerus_mobile.store import Store
        from zerus_mobile.migration import migrate_sqlite

        with tempfile.TemporaryDirectory(prefix="zerus-budget-migration-") as directory:
            path = Path(directory) / "relay.sqlite3"
            source = Store(path)
            workspace = source.workspace("Synthetic")
            node = source.node(workspace, "Synthetic")
            phone = source.pair(source.invite(workspace)["pair_code"], "Synthetic")
            device = source.authenticate(phone["device_token"], "devices")
            for _ in range(3):
                source.submit(device, self.command(node, "send"), 200)
            hashes = [
                tuple(r)
                for r in source.db.execute(
                    "SELECT id,body_hash,state FROM requests ORDER BY id"
                )
            ]
            source.close()
            with self.assertRaisesRegex(ValueError, "shard budget"):
                await migrate_sqlite(
                    path, TEST_DSN, global_max_bytes=2 * MIB, quota_shards=1
                )
            store = await self.open(2 * MIB)
            self.assertEqual(
                await store.pool.fetchval("SELECT count(*) FROM workspaces"), 0
            )
            self.assertEqual(
                await store.pool.fetchval(
                    "SELECT sum(payload_bytes) FROM payload_shards"
                ),
                0,
            )
            source = Store(path)
            try:
                self.assertEqual(
                    [
                        tuple(r)
                        for r in source.db.execute(
                            "SELECT id,body_hash,state FROM requests ORDER BY id"
                        )
                    ],
                    hashes,
                )
            finally:
                source.close()
            await store.close()
            self.stores.remove(store)
            report = await migrate_sqlite(
                path, TEST_DSN, global_max_bytes=16 * MIB, quota_shards=1
            )
            self.assertEqual(report["requests"], 3)
            recovered = await self.open()
            self.assertEqual(
                await recovered.pool.fetchval("SELECT count(*) FROM requests"), 3
            )

    async def test_event_free_changed_heartbeat_bypasses_locked_fleet_counter(self):
        first, second = await self.open(), await self.open()
        workspace, _, _, _, node_row = await self.identity(first)
        await first.heartbeat(
            node_row,
            {"sessions": [{"name": "fixture", "activity": "busy", "progress": 1}]},
        )
        async with second.pool.acquire() as conn, conn.transaction():
            await conn.fetchrow("SELECT id FROM global_usage WHERE id=1 FOR UPDATE")
            await asyncio.wait_for(
                first.heartbeat(
                    node_row,
                    {
                        "sessions": [
                            {"name": "fixture", "activity": "busy", "progress": 2}
                        ]
                    },
                ),
                0.5,
            )
        self.assertEqual(await first.pool.fetchval("SELECT count(*) FROM events"), 0)
        snapshot = await first.pool.fetchval(
            "SELECT snapshot FROM nodes WHERE id=$1", node_row["id"]
        )
        self.assertEqual(
            __import__("json").loads(snapshot)["sessions"][0]["progress"], 2
        )

    async def test_mass_disconnect_cleanup_has_separate_bounded_capacity(self):
        store = await self.open()
        workspace, node, _, _, _ = await self.identity(store)
        leases = [
            await store.acquire_poll("nodes", node["node_id"], workspace, 35, 300, 300)
            for _ in range(250)
        ]
        self.assertTrue(all(leases))
        await asyncio.gather(*(store.release_poll(lease) for lease in leases))
        self.assertEqual(
            await store.pool.fetchval("SELECT count(*) FROM poll_leases"), 0
        )
        self.assertEqual(
            await store.pool.fetchval(
                "SELECT active_polls FROM workspace_usage WHERE workspace_id=$1",
                workspace,
            ),
            0,
        )

    async def test_codec_failures_release_payload_without_cyclic_gc(self):
        import gc
        import weakref

        store = await self.open()

        class Payload:
            def __init__(self):
                self.data = bytearray(1024 * 1024)

        def fail(payload):
            raise RuntimeError("synthetic codec failure")

        enabled = gc.isenabled()
        gc.disable()
        try:
            for _ in range(10):
                payload = Payload()
                reference = weakref.ref(payload)
                try:
                    await store._cpu(fail, payload)
                except RuntimeError:
                    pass
                del payload
                # The executor thread may still hold its work item when this
                # coroutine resumes; a retained payload outlasts the wait.
                deadline = time.monotonic() + 2
                while reference() is not None and time.monotonic() < deadline:
                    await asyncio.sleep(0.001)
                self.assertIsNone(reference())
        finally:
            if enabled:
                gc.enable()

    async def test_custom_poll_cap_sizes_disconnect_cleanup(self):
        store = await self.open()
        store.configure_cleanup(1000 + 256)
        workspace, node, _, _, _ = await self.identity(store)
        leases = [
            await store.acquire_poll(
                "nodes", node["node_id"], workspace, 35, 1000, 1000
            )
            for _ in range(1000)
        ]
        self.assertTrue(all(leases))
        async with store.pool.acquire() as conn, conn.transaction():
            await conn.fetchrow(
                "SELECT workspace_id FROM workspace_usage WHERE workspace_id=$1 FOR UPDATE",
                workspace,
            )
            tasks = [asyncio.create_task(store.release_poll(lease)) for lease in leases]
            await asyncio.sleep(0)
            for task in tasks:
                task.cancel()
            await asyncio.sleep(0)
            for task in tasks:
                task.cancel()
            await asyncio.sleep(0)
            self.assertFalse(any(task.done() for task in tasks))
        results = await asyncio.gather(*tasks, return_exceptions=True)
        self.assertTrue(
            all(isinstance(value, asyncio.CancelledError) for value in results)
        )
        self.assertEqual(
            await store.pool.fetchval("SELECT count(*) FROM poll_leases"), 0
        )
        self.assertEqual(
            await store.pool.fetchval(
                "SELECT active_polls FROM workspace_usage WHERE workspace_id=$1",
                workspace,
            ),
            0,
        )
        for value in (0, -1, 65537, 1.5, True):
            with self.assertRaises(ValueError):
                store.configure_cleanup(value)


if __name__ == "__main__":
    unittest.main()
