"""Fixed scoped lifecycle forwarding, exact receipts, rollout compatibility."""
import tempfile
import time
import unittest
from unittest.mock import AsyncMock, patch
import uuid

from zerus_mobile.connector import Connector
from zerus_mobile.context import LIFECYCLE_OPERATIONS
from relay_oracle import validate_request


class LifecycleTransportTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.connector = Connector({"server_url": "https://relay.example.invalid", "node_token": "fixture", "state_dir": self.temp.name})
        self.connector.sessions = {"codex/example/session"}
        self.connector.lifecycle_supported = True

    async def asyncTearDown(self):
        self.connector.journal.close()
        self.temp.cleanup()

    def request(self, operation="pause", **fields):
        identity = str(uuid.uuid4())
        return {"request_id": identity, "computer_id": str(uuid.uuid4()), "operation": operation,
                "session": "codex/example/session", "payload": {"request_id": identity, "expected_run_id": "run", "expected_conversation_id": "conversation", **fields}}

    def receipt(self, request, **fields):
        return {"request_id": request["request_id"], "name": request["session"], "run_id": "run", "conversation_id": "conversation", "status": "completed", **fields}

    async def test_fixed_dispatch_payload_and_identical_retry_never_repeat(self):
        for operation in LIFECYCLE_OPERATIONS - {"restore"}:
            fields = {"new_name": "codex/example/new"} if operation == "rename" else {"tag": "new"} if operation == "fork" else {}
            request = self.request(operation, **fields)
            with patch.object(self.connector, "native", AsyncMock(return_value=self.receipt(request))) as native:
                response = await self.connector.execute(request)
                self.assertEqual(response["state"], "completed")
                self.assertEqual(await self.connector.execute(request), response)
                self.assertEqual(native.await_count, 1)
                self.assertEqual(native.call_args.args, (["session-action", request["session"], "--json"], {**request["payload"], "action": operation}))

    async def test_authoritative_rename_target_can_be_inspected_before_heartbeat(self):
        request = self.request("rename", new_name="codex/example/renamed")
        target = {"name": "codex/example/renamed", "run_id": "run", "conversation_id": "conversation"}
        with patch.object(self.connector,"native",AsyncMock(return_value=self.receipt(request,result_target=target))):
            self.assertEqual((await self.connector.execute(request))["state"],"completed")
        identity = str(uuid.uuid4())
        inspection = {"request_id":identity,"operation":"inspect","session":target["name"],"payload":{}}
        with patch.object(self.connector,"native",AsyncMock(return_value=target)):
            self.assertEqual((await self.connector.execute(inspection))["state"],"completed")
        self.assertNotIn(request["session"],self.connector.sessions)

    async def test_native_definite_failed_and_uncertain_status_are_preserved(self):
        for status in ("failed", "uncertain"):
            request = self.request()
            with patch.object(self.connector, "native", AsyncMock(return_value=self.receipt(request, status=status, error="Synthetic refusal"))) as native:
                response = await self.connector.execute(request)
                self.assertEqual(response["state"], status)
                self.assertEqual(response["error"], "Synthetic refusal")
                self.assertEqual(await self.connector.execute(request), response)
                self.assertEqual(native.await_count, 1)

    async def test_unknown_ack_or_changed_identity_is_uncertain_without_replay(self):
        for fields in ({"status": "started"}, {"run_id": "other"}, {"conversation_id": "other"}, {"name": "other"}):
            request = self.request()
            with patch.object(self.connector, "native", AsyncMock(return_value=self.receipt(request, **fields))) as native:
                response = await self.connector.execute(request)
                self.assertEqual(response["state"], "uncertain")
                self.assertEqual(await self.connector.execute(request), response)
                self.assertEqual(native.await_count, 1)

    async def test_old_cli_and_invalid_payload_never_invoke_mutator(self):
        self.connector.lifecycle_supported = False
        with patch.object(self.connector, "native", AsyncMock()) as native:
            self.assertEqual((await self.connector.execute(self.request()))["state"], "failed")
            native.assert_not_awaited()
        self.connector.lifecycle_supported = True
        for operation, fields in (("pause", {"archive_id": str(uuid.uuid4())}), ("pause", {"action": "terminate"}),
                                  ("rename", {}), ("rename", {"new_name": "a" * 513}), ("fork", {"tag": "../invalid"}), ("restore", {})):
            request = self.request(operation, **fields)
            with self.assertRaises(Exception):
                validate_request(request)
            with patch.object(self.connector, "native", AsyncMock()) as native:
                self.assertEqual((await self.connector.execute(request))["state"], "failed")
                native.assert_not_awaited()

    async def test_archive_restore_selects_exact_pair_with_fresh_cache(self):
        archive = str(uuid.uuid4())
        self.connector.archives = {("codex/example/session", archive)}
        self.connector.snapshot_seen_at = time.monotonic()
        request = self.request("restore", archive_id=archive)
        target = {"name": request["session"], "run_id": "new-run", "conversation_id": "conversation"}
        with patch.object(self.connector, "native", AsyncMock(return_value=self.receipt(request, result_target=target))):
            response = await self.connector.execute(request)
            self.assertEqual(response["result"]["run_id"], "run")
            self.assertEqual(response["result"]["result_target"], target)
        self.connector.snapshot_seen_at -= 60
        with patch.object(self.connector, "native", AsyncMock()) as native:
            self.assertEqual((await self.connector.execute(self.request("restore", archive_id=archive)))["state"], "failed")
            native.assert_not_awaited()

    async def test_bounded_help_probe_does_not_infer_support_from_arbitrary_text(self):
        for text, expected in (("old CLI", False), ("hgs session-action <session> --json scoped lifecycle", True)):
            self.connector.capabilities_next_poll = 0
            with patch.object(self.connector, "native", AsyncMock(return_value=text)) as native:
                await self.connector.probe_capabilities()
                self.assertEqual(self.connector.lifecycle_supported, expected)
                self.assertEqual(native.call_args.args, (["--help"],))
                self.assertEqual(native.call_args.kwargs, {"timeout": 3, "json_output": False})
