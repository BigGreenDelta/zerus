"""Scoped existing native queue/settings/process/child ABIs, synthetic only."""
import json
import os
from pathlib import Path
import tempfile
import time
import unittest
from unittest.mock import AsyncMock, patch
import uuid

from zerus_mobile.connector import Connector
from zerus_mobile.context import FEATURES, OPERATIONS
from relay_oracle import validate_request
from zerus_mobile.store import Store

NAME = "codex/example/mobile"
RUN = "fixture-run"
CONVERSATION = "fixture-conversation"
QUEUE = "a" * 64


def command(operation, **payload):
    request_id = str(uuid.uuid4())
    base = {} if operation == "inspect" else {"request_id": request_id, "expected_run_id": RUN, "expected_conversation_id": CONVERSATION}
    return {"request_id": request_id, "operation": operation, "session": NAME, "payload": {**base, **payload}}


class CoreConnectorTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.connector = Connector({"server_url": "https://relay.example.invalid", "node_token": "fixture-token", "state_dir": self.temp.name})
        self.connector.sessions = {NAME}
        self.detail = {"name": NAME, "run_id": RUN, "conversation_id": CONVERSATION, "settings_change_supported": True,
                       "input_queue": {"id": QUEUE, "can_send_now": True}, "subagents": {"child": {"state": "completed"}}}

    async def asyncTearDown(self):
        self.connector.journal.close()
        self.temp.cleanup()

    def receipt(self, request, **extra):
        return {"request_id": request["request_id"], "name": NAME, "run_id": RUN, "conversation_id": CONVERSATION, **extra}

    async def test_queue_exact_fresh_id_and_native_receipt_with_no_duplicate_delivery(self):
        request = command("send_now", queue_id=QUEUE)
        with patch.object(self.connector, "native", AsyncMock(side_effect=[self.detail, self.receipt(request, status="submitted", queue_id=QUEUE)])) as native:
            response = await self.connector.execute(request)
            self.assertEqual(response["state"], "completed")
            self.assertEqual(await self.connector.execute(request), response)
            self.assertEqual(native.await_count, 2)
            self.assertEqual(native.call_args.args, (["send-now", NAME, "--json"], request["payload"]))
        with patch.object(self.connector, "native", AsyncMock(return_value={**self.detail, "input_queue": {"id": "b" * 64, "can_send_now": True}})) as native:
            self.assertEqual((await self.connector.execute(command("send_now", queue_id=QUEUE)))["state"], "failed")
            self.assertEqual(native.await_count, 1)

    async def test_settings_default_effort_and_scheduled_receipt_are_preserved(self):
        for status in ("applied", "scheduled"):
            request = command("settings", model="fixture-model", effort="")
            with patch.object(self.connector, "native", AsyncMock(side_effect=[self.detail, self.receipt(request, status=status, model="fixture-model", effort="", **({"pending_settings_id": request["request_id"]} if status == "scheduled" else {}))])) as native:
                response = await self.connector.execute(request)
                self.assertEqual(response["state"], "completed")
                self.assertEqual(response["result"]["status"], status)
                self.assertEqual(native.call_args.args, (["settings", NAME, "--json"], request["payload"]))
        pending = str(uuid.uuid4())
        with patch.object(self.connector, "native", AsyncMock(return_value={**self.detail, "pending_settings_id": "other"})) as native:
            self.assertEqual((await self.connector.execute(command("settings", model="fixture-model", expected_pending_id=pending)))["state"], "failed")
            self.assertEqual(native.await_count, 1)

    async def test_scheduled_ack_without_native_pending_id_is_uncertain_without_replay(self):
        request = command("settings", model="fixture-model")
        with patch.object(self.connector, "native", AsyncMock(side_effect=[self.detail, self.receipt(request, status="scheduled", model="fixture-model")])) as native:
            result = await self.connector.execute(request)
            self.assertEqual(result["state"], "uncertain")
            self.assertEqual(await self.connector.execute(request), result)
            self.assertEqual(native.await_count, 2)

    async def test_wrong_queue_or_model_ack_is_uncertain_and_never_replayed(self):
        for operation, payload, ack in (("send_now", {"queue_id": QUEUE}, {"status": "submitted", "queue_id": "b" * 64}),
                                        ("settings", {"model": "fixture-model"}, {"status": "applied", "model": "other"})):
            request = command(operation, **payload)
            with patch.object(self.connector, "native", AsyncMock(side_effect=[self.detail, self.receipt(request, **ack)])) as native:
                response = await self.connector.execute(request)
                self.assertEqual(response["state"], "uncertain")
                self.assertEqual(await self.connector.execute(request), response)
                self.assertEqual(native.await_count, 2)

    async def test_process_argv_is_fixed_and_failure_read_vs_stop_is_distinct(self):
        for operation, flag in (("process_output", "--output"), ("process_stop", "--stop")):
            request = command(operation, process_id="job:exact", generation="generation-exact")
            with patch.object(self.connector, "native", AsyncMock(return_value={"id": "job:exact", "run_id": RUN, "conversation_id": CONVERSATION, "status": "requested"})) as native:
                response = await self.connector.execute(request)
                self.assertEqual(response["state"], "completed")
                self.assertEqual(response["result"]["request_id"], request["request_id"])
                self.assertEqual(response["result"]["process_id"], "job:exact")
                self.assertEqual(response["result"]["name"], NAME)
                self.assertEqual(response["result"]["status"], "requested")
                self.assertEqual(native.call_args.args, (["processes", NAME, flag, "job:exact", "--run", RUN, "--conversation", CONVERSATION, "--generation", "generation-exact"],))
            with patch.object(self.connector, "native", AsyncMock(return_value={"id": "other", "run_id": RUN, "conversation_id": CONVERSATION})):
                self.assertEqual((await self.connector.execute(command(operation, process_id="job:exact")))["state"], "failed" if operation == "process_output" else "uncertain")

    async def test_exact_archived_process_read_never_targets_live_or_allows_stop(self):
        archive = str(uuid.uuid4())
        self.connector.archives = {(NAME, archive)}
        self.connector.snapshot_seen_at = time.monotonic()
        with patch.object(self.connector, "native", AsyncMock(return_value={"id": "job", "run_id": RUN, "conversation_id": CONVERSATION})) as native:
            self.assertEqual((await self.connector.execute(command("process_output", process_id="job", archive_id=archive)))["state"], "completed")
            self.assertEqual(native.call_args.args[0][-2:], ["--archive", archive])
        with patch.object(self.connector, "native", AsyncMock()) as native:
            self.assertEqual((await self.connector.execute(command("process_stop", process_id="job", archive_id=archive)))["state"], "failed")
            native.assert_not_awaited()

    async def test_scoped_incremental_and_child_inspection_identity(self):
        request = command("inspect", after=5, expected_run_id=RUN, expected_conversation_id=CONVERSATION)
        with patch.object(self.connector, "native", AsyncMock(return_value=self.detail)) as native:
            self.assertEqual((await self.connector.execute(request))["state"], "completed")
            self.assertEqual(native.call_args.args[0], ["inspect", NAME, "--after", "5"])
        child = {"name": NAME, "run_id": RUN, "agent_id": "child", "parent_conversation_id": CONVERSATION, "conversation_id": CONVERSATION + "/child"}
        request = command("inspect", agent_id="child", expected_run_id=RUN, expected_conversation_id=CONVERSATION)
        with patch.object(self.connector, "native", AsyncMock(side_effect=[self.detail, child])) as native:
            self.assertEqual((await self.connector.execute(request))["state"], "completed")
            self.assertEqual(native.call_args.args[0], ["inspect", NAME, "--agent", "child", "--skip-processes"])
        with patch.object(self.connector, "native", AsyncMock(side_effect=[self.detail, {**child, "parent_conversation_id": "other"}])):
            self.assertEqual((await self.connector.execute(command("inspect", agent_id="child", expected_run_id=RUN, expected_conversation_id=CONVERSATION)))["state"], "failed")
        with patch.object(self.connector, "native", AsyncMock(return_value={**self.detail, "subagents": {}})) as native:
            self.assertEqual((await self.connector.execute(command("inspect", agent_id="child", expected_run_id=RUN, expected_conversation_id=CONVERSATION)))["state"], "failed")
            self.assertEqual(native.await_count, 1)

    async def test_invalid_fields_tokens_or_extensions_never_spawn(self):
        invalid = [command("send_now", queue_id="../path"), command("settings"), command("settings", model=""), command("settings", effort="invented"),
                   command("settings", model="fixture-model", arbitrary="command"), command("process_stop", process_id="../../path"),
                   command("inspect", after=-1, expected_run_id=RUN, expected_conversation_id=CONVERSATION), command("inspect", after=True),
                   command("inspect", agent_id="../path", expected_run_id=RUN, expected_conversation_id=CONVERSATION), command("inspect", after=2)]
        with patch.object(self.connector, "native", AsyncMock()) as native:
            for request in invalid:
                self.assertEqual((await self.connector.execute(request))["state"], "failed")
            native.assert_not_awaited()


class CoreRelayTests(unittest.TestCase):
    def test_extension_caps_old_clients_and_read_retention(self):
        with tempfile.TemporaryDirectory() as directory:
            store = Store(Path(directory) / "relay.sqlite")
            try:
                workspace = store.workspace("Synthetic")
                node = store.node(workspace, "Example")
                phone = store.pair(store.invite(workspace)["pair_code"], "Test")
                device = store.authenticate(phone["device_token"], "devices")
                request = {**command("inspect", after=1, expected_run_id=RUN, expected_conversation_id=CONVERSATION), "computer_id": node["node_id"]}
                validate_request(request)
                self.assertEqual(store.submit(device, request, 200)[0], 409)
                with store.db:
                    store.db.execute("UPDATE nodes SET snapshot=? WHERE id=?", (json.dumps({"mobile_capabilities": {"protocol_version": 1, "operations": sorted(OPERATIONS), "features": sorted(FEATURES)}}), node["node_id"]))
                self.assertEqual(store.submit(device, request, 200)[0], 202)
                read = {**command("process_output", process_id="job"), "computer_id": node["node_id"]}
                validate_request(read)
                self.assertEqual(store.submit(device, read, 200)[0], 202)
                with store.db:
                    store.db.execute("UPDATE requests SET state='completed',updated=? WHERE id=?", (time.time() - 3700, read["request_id"]))
                store.maintain()
                self.assertIsNone(store.db.execute("SELECT id FROM requests WHERE id=?", (read["request_id"],)).fetchone())
            finally:
                store.close()


class CoreFixtureTests(unittest.IsolatedAsyncioTestCase):
    async def test_synthetic_queue_settings_child_process_and_cold_message_window(self):
        with tempfile.TemporaryDirectory() as directory, patch.dict(os.environ, {"ZERUS_MOBILE_FIXTURE_DIR": directory}):
            root = Path(directory)
            (root / "fixture-config.json").write_text(json.dumps({"input_queue": {"id": QUEUE, "text": "Synthetic queued input", "can_send_now": True},
                "subagents": {"child": {"state": "completed"}}, "processes": {"items": [{"id": "fixture-job", "status": "running"}]}, "tool_count": 150}))
            connector = Connector({"server_url": "https://relay.example.invalid", "node_token": "fixture-token",
                "hgs_path": str(Path(__file__).with_name("fixture_hgs.py")), "state_dir": str(root / "journal")})
            try:
                await connector.snapshot()
                for request in (command("send_now", queue_id=QUEUE), command("settings", model="fixture-fast", effort="low"),
                                command("process_output", process_id="fixture-job"), command("process_stop", process_id="fixture-job"),
                                command("inspect", agent_id="child", expected_run_id=RUN, expected_conversation_id=CONVERSATION)):
                    self.assertEqual((await connector.execute(request))["state"], "completed")
                inspected = (await connector.execute(command("inspect")))["result"]
                self.assertEqual(len(inspected["events"]), 100)
                self.assertFalse(any(row["type"] == "UserPromptSubmit" for row in inspected["events"]))
                self.assertTrue(any(row["type"] == "UserPromptSubmit" for row in inspected["message_events"]))
                self.assertEqual(inspected["model"], "fixture-fast")
                self.assertIsNone(inspected["input_queue"])
                self.assertEqual(len((root / "mutations.jsonl").read_text().splitlines()), 3)
            finally:
                connector.journal.close()
