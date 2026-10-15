"""SQLite worker cancellation and notification isolation regressions."""

import asyncio
from pathlib import Path
import tempfile
import threading
import time
import unittest
import uuid

from zerus_mobile.async_store import Notifications, StoreBusy, SyncStoreAdapter
from zerus_mobile.store import Store


class NotificationTests(unittest.IsolatedAsyncioTestCase):
    async def test_unrelated_notifications_do_not_spin_idle_topic(self):
        notifications = Notifications()
        generation = notifications.generation("node:idle")
        notifications.notify("workspace:unrelated")
        waiter = asyncio.create_task(notifications.wait("node:idle", generation, 1))
        await asyncio.sleep(0.02)
        self.assertFalse(waiter.done())
        notifications.notify("node:idle")
        await asyncio.wait_for(waiter, 1)

    async def test_new_waiters_do_not_reuse_signalled_event(self):
        notifications = Notifications()
        generation = notifications.generation("node:fixture")
        first = asyncio.create_task(notifications.wait("node:fixture", generation, 1))
        await asyncio.sleep(0)
        notifications.notify("node:fixture")
        second = asyncio.create_task(
            notifications.wait(
                "node:fixture", notifications.generation("node:fixture"), 1
            )
        )
        await first
        await asyncio.sleep(0.02)
        self.assertFalse(second.done())
        notifications.notify("node:fixture")
        await asyncio.wait_for(second, 1)

    async def test_idle_topic_cardinality_is_bounded(self):
        notifications = Notifications()
        for index in range(12000):
            notifications.generation(f"node:{index}")
        self.assertLessEqual(len(notifications._topics), 10000)


class SQLiteResourceTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="zerus-sqlite-resources-")
        self.store = Store(Path(self.directory.name) / "relay.sqlite3")
        self.adapter = SyncStoreAdapter(self.store, max_pending=1)

    async def asyncTearDown(self):
        await self.adapter.close()
        self.store.close()
        self.directory.cleanup()

    async def test_cancellation_drains_worker_and_retains_capacity(self):
        entered, finish = threading.Event(), threading.Event()

        def blocked():
            entered.set()
            finish.wait(2)
            return True

        self.store.ready = blocked
        first = asyncio.create_task(self.adapter.ready())
        await asyncio.to_thread(entered.wait, 1)
        first.cancel()
        try:
            await asyncio.sleep(0.02)
            self.assertFalse(first.done())
            with self.assertRaises(StoreBusy):
                await self.adapter.ready()
        finally:
            finish.set()
        with self.assertRaises(asyncio.CancelledError):
            await first
        self.assertFalse(self.adapter.slots.locked())

    async def test_worker_failures_release_payload_without_cyclic_gc(self):
        import gc
        import weakref

        class Payload:
            def __init__(self):
                self.data = bytearray(1024 * 1024)

        def fail(payload):
            raise RuntimeError("synthetic codec failure")

        self.store.synthetic_failure = fail
        enabled = gc.isenabled()
        gc.disable()
        try:
            for _ in range(10):
                payload = Payload()
                reference = weakref.ref(payload)
                try:
                    await self.adapter._call("synthetic_failure", payload)
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

    async def test_expiry_and_revocation_release_reserve_charge_errors_exactly(self):
        workspace = self.store.workspace("Synthetic workspace")
        node = self.store.node(workspace, "Synthetic node")
        phone = self.store.pair(
            self.store.invite(workspace)["pair_code"], "Synthetic phone"
        )
        device = self.store.authenticate(phone["device_token"], "devices")
        node_row = self.store.authenticate(node["node_token"], "nodes")
        values = []
        for _ in range(2):
            value = {
                "request_id": str(uuid.uuid4()),
                "computer_id": node["node_id"],
                "operation": "send",
                "session": "fixture",
                "payload": {"text": "Synthetic"},
            }
            self.assertEqual(self.store.submit(device, value, 200)[0], 202)
            values.append(value)
        self.store.claim(node_row)
        self.store.revoke("nodes", node["node_id"])
        rows = self.store.db.execute("SELECT * FROM requests").fetchall()
        self.assertEqual({r["state"] for r in rows}, {"failed", "uncertain"})
        self.assertTrue(all(r["reserved_bytes"] == 0 for r in rows))
        expected = sum(
            r["body_bytes"]
            + len((r["result"] or "").encode())
            + len((r["error"] or "").encode())
            for r in rows
        )
        usage = self.store.db.execute(
            "SELECT * FROM workspace_usage WHERE workspace_id=?", (workspace,)
        ).fetchone()
        self.assertEqual(usage["payload_bytes"], expected)
        self.assertEqual(usage["active"], 0)
