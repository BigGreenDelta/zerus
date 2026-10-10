"""Local integration tests use only temporary state and loopback HTTP."""
import asyncio
import json
import io
import os
from pathlib import Path
import stat
import tempfile
import time
import unittest
from unittest.mock import AsyncMock, patch
import uuid

from aiohttp.test_utils import TestClient, TestServer

from zerus_mobile.context import OPERATIONS
from zerus_mobile.server import Config, PUSH_WORKER, create_app
from zerus_mobile.store import Store, digest

try:
    from firebase_admin import messaging as firebase_messaging
except ImportError:
    firebase_messaging = None


class RelayTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.store = Store(Path(self.temp.name) / "relay.sqlite3")
        self.workspace = self.store.workspace("Example")
        self.node = self.store.node(self.workspace, "Example computer")
        self.phone = self.store.pair(self.store.invite(self.workspace)["pair_code"], "Example phone")
        self.device = self.store.authenticate(self.phone["device_token"], "devices")
        self.config = Config(background=False)
        self.app = create_app(self.store, self.config)
        self.client = TestClient(TestServer(self.app))
        await self.client.start_server()

    async def asyncTearDown(self):
        await self.client.close()
        self.store.close()
        self.temp.cleanup()

    def headers(self, node=False):
        return {"Authorization": "Bearer " + (self.node["node_token"] if node else self.phone["device_token"])}

    async def call(self, method, route, body=None, node=False, expected=200, headers=None):
        response = await self.client.request(method, route, json=body, headers=headers or self.headers(node))
        self.assertEqual(response.status, expected, await response.text())
        return await response.json()

    def command(self, operation="inspect"):
        value = {"request_id": str(uuid.uuid4()), "computer_id": self.node["node_id"], "operation": operation, "session": "codex/project/deep/tag", "payload": {}}
        if operation != "inspect":
            value["payload"] = {"request_id": value["request_id"], "expected_run_id": "example-run", "expected_conversation_id": "example-conversation", "text": "Hello"}
        return value

    async def test_health_capabilities_and_local_only_administration(self):
        self.assertEqual((await self.call("GET", "/healthz"))["protocol_version"], 1)
        self.assertEqual((await self.call("GET", "/v1/capabilities"))["push_providers"], [])
        await self.call("POST", "/v1/admin", expected=404)
        await self.call("GET", "/v1/computers", headers={"Authorization": "Bearer wrong"}, expected=401)

    async def test_pair_once_and_expiry_and_hash_storage(self):
        invitation = self.store.invite(self.workspace)
        pair = await self.call("POST", "/v1/pair", {"code": invitation["pair_code"], "device_name": "Other"})
        self.assertEqual(pair["workspace_id"], self.workspace)
        self.assertEqual(pair["workspace_name"], "Example")
        await self.call("POST", "/v1/pair", {"code": invitation["pair_code"], "device_name": "Again"}, expected=401)
        expired = self.store.invite(self.workspace)
        with self.store.db:
            self.store.db.execute("UPDATE invitations SET expires=? WHERE code_hash=?", (time.time() - 1, digest(expired["pair_code"])))
        await self.call("POST", "/v1/pair", {"code": expired["pair_code"], "device_name": "Late"}, expected=401)
        self.assertEqual(len(pair["device_token"]), 43)
        self.assertIsNone(self.store.db.execute("SELECT 1 FROM devices WHERE token_hash=?", (pair["device_token"],)).fetchone())

    async def test_pair_rate_limit(self):
        for _ in range(10):
            await self.call("POST", "/v1/pair", {"code": "bad", "device_name": "Example"}, expected=401)
        await self.call("POST", "/v1/pair", {"code": "bad", "device_name": "Example"}, expected=429)

    async def test_failed_authentication_rate_limit(self):
        for _ in range(20):
            await self.call("GET", "/v1/computers", headers={"Authorization": "Bearer invalid"}, expected=401)
        await self.call("GET", "/v1/computers", headers={"Authorization": "Bearer invalid"}, expected=429)

    async def test_workspace_and_role_isolation(self):
        other = self.store.workspace("Other")
        foreign = self.store.node(other, "Foreign")
        computers = await self.call("GET", "/v1/computers")
        self.assertEqual([row["id"] for row in computers["computers"]], [self.node["node_id"]])
        value = self.command()
        value["computer_id"] = foreign["node_id"]
        await self.call("POST", "/v1/requests", value, expected=404)
        await self.call("GET", "/v1/computers", node=True, expected=401)
        await self.call("GET", "/v1/node/requests", expected=401)

    async def test_durable_claim_duplicate_result_and_owner(self):
        value = self.command("send")
        first = await self.call("POST", "/v1/requests", value, expected=202)
        self.assertEqual(first["state"], "queued")
        self.assertEqual(await self.call("POST", "/v1/requests", value, expected=202), first)
        changed = {**value, "session": "different"}
        await self.call("POST", "/v1/requests", changed, expected=409)
        delivered = await self.call("GET", "/v1/node/requests", node=True)
        self.assertEqual(delivered["requests"][0]["session"], value["session"])
        self.assertEqual((await self.call("GET", "/v1/requests/" + value["request_id"]))["state"], "claimed")
        self.assertEqual((await self.call("GET", "/v1/node/requests", node=True))["requests"], [])
        self.assertEqual((await self.call("POST", "/v1/requests", value, expected=202))["state"], "claimed")
        other_phone = self.store.pair(self.store.invite(self.workspace)["pair_code"], "Other phone")
        await self.call("GET", "/v1/requests/" + value["request_id"], headers={"Authorization": "Bearer " + other_phone["device_token"]}, expected=404)
        other_node = self.store.node(self.workspace, "Other node")
        result = {"state": "completed", "result": {"status": "sent"}, "error": None}
        await self.call("POST", "/v1/node/requests/" + value["request_id"] + "/result", result, headers={"Authorization": "Bearer " + other_node["node_token"]}, expected=404)
        await self.call("POST", "/v1/node/requests/" + value["request_id"] + "/result", result, node=True)
        await self.call("POST", "/v1/node/requests/" + value["request_id"] + "/result", result, node=True)
        self.assertEqual((await self.call("GET", "/v1/requests/" + value["request_id"]))["result"], result["result"])

    async def test_single_claim_and_request_validation(self):
        for _ in range(2):
            await self.call("POST", "/v1/requests", self.command(), expected=202)
        self.assertEqual(len((await self.call("GET", "/v1/node/requests", node=True))["requests"]), 1)
        invalid = self.command("send")
        invalid["payload"]["request_id"] = str(uuid.uuid4())
        await self.call("POST", "/v1/requests", invalid, expected=400)
        invalid = self.command()
        invalid["operation"] = "shell"
        await self.call("POST", "/v1/requests", invalid, expected=400)
        invalid = self.command("send")
        invalid["payload"]["command"] = "whoami"
        await self.call("POST", "/v1/requests", invalid, expected=400)

    async def test_expiry_restart_and_late_result(self):
        queued, claimed = self.command(), self.command("send")
        await self.call("POST", "/v1/requests", claimed, expected=202)
        await self.call("GET", "/v1/node/requests", node=True)
        await self.call("POST", "/v1/requests", queued, expected=202)
        with self.store.db:
            self.store.db.execute("UPDATE requests SET created=? WHERE id=?", (time.time() - 121, queued["request_id"]))
        self.store.maintain(restart=True)
        self.assertEqual((await self.call("GET", "/v1/requests/" + queued["request_id"]))["state"], "failed")
        self.assertEqual((await self.call("GET", "/v1/requests/" + claimed["request_id"]))["state"], "uncertain")
        await self.call("POST", "/v1/node/requests/" + claimed["request_id"] + "/result", {"state": "completed", "result": None, "error": None}, node=True, expected=409)
        self.assertEqual((await self.call("GET", "/v1/node/requests", node=True))["requests"], [])

    async def test_queue_limit_retention_tombstones(self):
        self.config.max_queue = 1
        value = self.command("send")
        await self.call("POST", "/v1/requests", value, expected=202)
        await self.call("POST", "/v1/requests", self.command(), expected=429)
        with self.store.db:
            self.store.db.execute("UPDATE requests SET state='completed',updated=1 WHERE id=?", (value["request_id"],))
        self.store.maintain(retention=1)
        tombstone = await self.call("POST", "/v1/requests", value, expected=202)
        self.assertEqual(tombstone["state"], "completed")
        self.assertIsNone(tombstone["result"])
        await self.call("POST", "/v1/requests", {**value, "session": "changed"}, expected=409)
        self.assertEqual((await self.call("GET", "/v1/node/requests", node=True))["requests"], [])

    async def test_claim_timeout_and_revoke(self):
        value = self.command()
        await self.call("POST", "/v1/requests", value, expected=202)
        await self.call("GET", "/v1/node/requests", node=True)
        with self.store.db:
            self.store.db.execute("UPDATE requests SET claimed=? WHERE id=?", (time.time() - 91, value["request_id"]))
        self.assertEqual((await self.call("GET", "/v1/requests/" + value["request_id"]))["state"], "uncertain")
        queued = self.command()
        await self.call("POST", "/v1/requests", queued, expected=202)
        self.store.revoke("nodes", self.node["node_id"])
        self.assertEqual((await self.call("GET", "/v1/requests/" + queued["request_id"]))["state"], "failed")
        await self.call("GET", "/v1/node/requests", node=True, expected=401)
        self.store.revoke("devices", self.phone["device_id"])
        await self.call("GET", "/v1/computers", expected=401)

    async def test_device_logout_revokes_token_and_cancels_queue(self):
        command = self.command("send")
        await self.call("POST", "/v1/requests", command, expected=202)
        await self.call("DELETE", "/v1/device")
        await self.call("GET", "/v1/computers", expected=401)
        row = self.store.db.execute("SELECT state FROM requests WHERE id=?", (command["request_id"],)).fetchone()
        self.assertEqual(row["state"], "failed")

    async def test_old_read_history_does_not_consume_mutation_tombstones(self):
        command = self.command()
        await self.call("POST", "/v1/requests", command, expected=202)
        with self.store.db:
            self.store.db.execute("UPDATE requests SET state='completed',updated=? WHERE id=?", (time.time() - 3601, command["request_id"]))
        self.store.maintain()
        self.assertIsNone(self.store.db.execute("SELECT 1 FROM requests WHERE id=?", (command["request_id"],)).fetchone())
        # Repeating an expired inspect is safe: it cannot mutate native agents.
        await self.call("POST", "/v1/requests", command, expected=202)

    async def test_long_poll_rechecks_revocation(self):
        task = asyncio.create_task(self.client.get("/v1/node/requests?wait=1", headers=self.headers(node=True)))
        await asyncio.sleep(0.05)
        self.store.revoke("nodes", self.node["node_id"])
        response = await task
        self.assertEqual(response.status, 401)

    async def test_heartbeat_events_baseline_transition_and_privacy(self):
        session = {"name": "codex/project/a/b", "run_id": "r1", "conversation_id": "c1", "phase": "working", "activity": "busy", "prompt": "Private text"}
        async def heartbeat(row):
            await self.call("POST", "/v1/node/heartbeat", {"snapshot": {"sessions": [row]}}, node=True)
        await heartbeat(session)
        self.assertEqual((await self.call("GET", "/v1/events"))["events"], [])
        question = {**session, "phase": "input", "attention_id": "question-one"}
        await heartbeat(question)
        await heartbeat(question)
        await heartbeat({**question, "attention_id": "question-two"})
        await heartbeat({**session, "phase": "idle", "activity": "idle"})
        result = await self.call("GET", "/v1/events")
        self.assertEqual([event["kind"] for event in result["events"]], ["attention", "attention", "completed"])
        self.assertNotIn("Private text", json.dumps(result))
        self.assertEqual((await self.call("GET", "/v1/events?after=" + str(result["cursor"])))["events"], [])
        computers = (await self.call("GET", "/v1/computers"))["computers"]
        self.assertTrue(computers[0]["online"])
        self.assertEqual(computers[0]["snapshot"]["sessions"][0]["prompt"], "Private text")
        other_ws = self.store.workspace("Other")
        other_phone = self.store.pair(self.store.invite(other_ws)["pair_code"], "Other")
        self.assertEqual((await self.call("GET", "/v1/events", headers={"Authorization": "Bearer " + other_phone["device_token"]}))["events"], [])

    async def test_body_cap_nonfinite_and_poll_bounds(self):
        response = await self.client.post("/v1/node/heartbeat", data=b'{"snapshot":NaN}', headers={**self.headers(True), "Content-Type": "application/json"})
        self.assertEqual(response.status, 400)
        response = await self.client.post("/v1/node/heartbeat", data=io.BytesIO(b"x" * (1024 * 1024 + 1)), headers={**self.headers(True), "Content-Type": "application/json"})
        self.assertEqual(response.status, 413)
        await self.call("GET", "/v1/events?wait=26", expected=400)
        await self.call("GET", "/v1/node/requests?wait=NaN", node=True, expected=400)

    async def test_async_question_fingerprints_add_change_remove(self):
        session = {"name": "codex/project/deep/tag", "run_id": "r", "conversation_id": "c", "phase": "working", "activity": "busy"}
        async def send(pending):
            await self.call("POST", "/v1/node/heartbeat", {"snapshot": {"sessions": [{**session, "mobile_attention": pending}]}}, node=True)
        first = {"question_id": "q1", "question_hash": "h1"}
        second = {"question_id": "q1", "question_hash": "h2"}
        await send([first])  # First snapshot is silent even if pending.
        await send([first])
        self.assertEqual((await self.call("GET", "/v1/events"))["events"], [])
        await send([])  # Removals are silent.
        await send([first])
        await send([first])  # An unchanged heartbeat is silent.
        await send([second])
        await send([])
        self.assertEqual([e["kind"] for e in (await self.call("GET", "/v1/events"))["events"]], ["attention", "attention"])

    async def test_push_retired_unifiedpush_and_unavailable_fcm(self):
        self.assertEqual((await self.call("GET", "/v1/capabilities"))["push_providers"], [])
        await self.call("POST", "/v1/push", {"provider": "unifiedpush", "endpoint": "https://push.example.com/token"}, expected=400)
        await self.call("POST", "/v1/push", {"provider": "fcm", "token": "example"}, expected=503)

    async def test_push_generic_payload_retries_and_revoke(self):
        worker = self.app[PUSH_WORKER]
        with patch.object(worker, "fcm", object()):
            await self.call("POST", "/v1/push", {"provider": "fcm", "token": "opaque"})
        node = self.store.authenticate(self.node["node_token"], "nodes")
        with self.store.db:
            self.store.event(node, "private/session", "attention")
        job = self.store.db.execute("SELECT * FROM push_jobs").fetchone()
        self.assertEqual(set(json.loads(job["payload"])), {"event_id", "kind"})
        with patch.object(worker, "deliver", AsyncMock(return_value=(False, False))):
            await worker.once()
        retry = self.store.db.execute("SELECT * FROM push_jobs").fetchone()
        self.assertEqual(retry["attempts"], 1)
        self.assertGreater(retry["next_at"], time.time())
        self.store.revoke("devices", self.phone["device_id"])
        self.assertEqual(self.store.db.execute("SELECT count(*) FROM push_jobs").fetchone()[0], 0)
        self.assertEqual(self.store.db.execute("SELECT count(*) FROM pushes").fetchone()[0], 0)

    async def test_push_retries_are_bounded_and_provider_gone_removes_registration(self):
        worker = self.app[PUSH_WORKER]
        with self.store.db:
            self.store.db.execute("INSERT INTO pushes VALUES(?,?,?)", (self.phone["device_id"], "fcm", "opaque"))
            self.store.event(self.store.authenticate(self.node["node_token"], "nodes"), "session", "attention")
        with patch.object(worker, "deliver", AsyncMock(return_value=(False, False))) as delivery:
            for _ in range(5):
                with self.store.db:
                    self.store.db.execute("UPDATE push_jobs SET next_at=0")
                await worker.once()
            self.assertEqual(delivery.await_count, 5)
            self.assertEqual(self.store.db.execute("SELECT count(*) FROM push_jobs").fetchone()[0], 0)
        with self.store.db:
            self.store.event(self.store.authenticate(self.node["node_token"], "nodes"), "session", "attention")
        with patch.object(worker, "deliver", AsyncMock(return_value=(False, True))):
            await worker.once()
        self.assertEqual(self.store.db.execute("SELECT count(*) FROM pushes").fetchone()[0], 0)

    async def test_retired_unifiedpush_registration_is_removed_on_delivery(self):
        worker = self.app[PUSH_WORKER]
        with self.store.db:
            self.store.db.execute("INSERT INTO pushes VALUES(?,?,?)", (self.phone["device_id"], "unifiedpush", "https://push.example.com/opaque"))
            self.store.event(self.store.authenticate(self.node["node_token"], "nodes"), "session", "attention")
        await worker.once()
        self.assertEqual(self.store.db.execute("SELECT count(*) FROM pushes").fetchone()[0], 0)
        self.assertEqual(self.store.db.execute("SELECT count(*) FROM push_jobs").fetchone()[0], 0)

    @unittest.skipIf(firebase_messaging is None, "optional FCM SDK is not installed")
    async def test_fcm_sdk_message_is_generic_data_only(self):
        worker = self.app[PUSH_WORKER]
        worker.fcm, worker.messaging = object(), firebase_messaging
        try:
            with patch.object(firebase_messaging, "send", return_value="example-message") as send:
                self.assertEqual(await worker.deliver("fcm", "example-token", {"event_id": 1, "kind": "wake"}), (True, False))
                message = send.call_args.args[0]
                self.assertEqual(message.data, {"event_id": "1", "kind": "wake"})
                self.assertIsNone(message.notification)
                self.assertEqual(message.android.priority, "high")
                self.assertIsNone(message.android.notification)
            with patch.object(firebase_messaging, "send", side_effect=firebase_messaging.UnregisteredError("Example expired token")):
                self.assertEqual(await worker.deliver("fcm", "example-token", {"event_id": 1, "kind": "wake"}), (False, True))
        finally:
            worker.fcm = None

    async def test_mark_read_is_exact_and_capability_gated(self):
        value = self.command("mark_read")
        value["payload"] = {"run_id": "example-run", "conversation_id": "example-conversation",
                            "reply_id": "7:100.5", "attention_signature": "a" * 64}
        await self.call("POST", "/v1/requests", value, expected=409)
        snapshot = {"sessions": [], "mobile_capabilities": {"protocol_version": 1, "operations": ["mark_read"], "features": []}}
        await self.call("POST", "/v1/node/heartbeat", {"snapshot": snapshot}, node=True)
        self.assertEqual((await self.call("POST", "/v1/requests", value, expected=202))["state"], "queued")
        for change in ({"reply_id": 7}, {"attention_signature": "A" * 64}, {"attention_signature": "a" * 63},
                       {"run_id": ""}, {"request_id": value["request_id"]}):
            invalid = self.command("mark_read")
            invalid["payload"] = {**value["payload"], **change}
            await self.call("POST", "/v1/requests", invalid, expected=400)
        delivered = await self.call("GET", "/v1/node/requests", node=True)
        self.assertEqual(delivered["requests"][0]["payload"], value["payload"])
        self.assertEqual(set((await self.call("GET", "/v1/capabilities"))["operations"]), OPERATIONS)


class FilesystemTests(unittest.TestCase):
    def test_private_database_and_no_parent_chmod(self):
        with tempfile.TemporaryDirectory() as root:
            directory = Path(root) / "private"
            store = Store(directory / "relay.sqlite3")
            self.assertEqual(stat.S_IMODE(directory.stat().st_mode), 0o700)
            self.assertEqual(stat.S_IMODE((directory / "relay.sqlite3").stat().st_mode), 0o600)
            store.close()
            public = Path(root) / "public"
            public.mkdir(mode=0o755)
            with self.assertRaises(ValueError):
                Store(public / "relay.sqlite3")
            self.assertEqual(stat.S_IMODE(public.stat().st_mode), 0o755)
            secret = Path(root) / "other-file"
            secret.write_text("do not modify")
            (directory / "symlink.sqlite3").symlink_to(secret)
            with self.assertRaises(OSError):
                Store(directory / "symlink.sqlite3")
            self.assertEqual(secret.read_text(), "do not modify")


if __name__ == "__main__":
    unittest.main()
