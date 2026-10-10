"""Shared read marks through API v1 and the connector, with synthetic native ABIs only."""
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import AsyncMock, patch
import uuid

from zerus_mobile.connector import MUTATIONS, Connector
from zerus_mobile.context import OPERATIONS, READ_OPERATIONS, READ_SQL, mark_read_result, validate_mark_read
from zerus_mobile.server import validate_request

NAME = "codex/example/mobile"
SIGNATURE = "a" * 64


def mark(**changes):
    return {"run_id": "fixture-run", "conversation_id": "fixture-conversation",
            "reply_id": "7:100.5", "attention_signature": SIGNATURE, **changes}


def command(payload=None, session=NAME):
    return {"request_id": str(uuid.uuid4()), "operation": "mark_read", "session": session,
            "payload": mark() if payload is None else payload}


class MarkReadValidation(unittest.TestCase):
    def test_payload_is_exact_identity_with_nullable_marks(self):
        self.assertIn("mark_read", OPERATIONS)
        validate_mark_read(mark())
        validate_mark_read(mark(reply_id=None, attention_signature=None))
        for invalid in (mark(reply_id=7), mark(run_id=""), mark(conversation_id="x" * 257),
                        mark(attention_signature="A" * 64), mark(attention_signature="a" * 63),
                        {**mark(), "request_id": str(uuid.uuid4())},
                        {k: v for k, v in mark().items() if k != "reply_id"}, None):
            with self.assertRaises(ValueError):
                validate_mark_read(invalid)

    def test_relay_oracle_validates_the_same_envelope(self):
        request = {**command(), "computer_id": str(uuid.uuid4())}
        validate_request(request)
        with self.assertRaises(Exception):
            validate_request({**request, "payload": mark(attention_signature="A" * 64)})

    def test_results_must_be_native_read_receipts(self):
        ok = {"ok": True, "applied": {"reply_id": True, "attention_signature": False},
              "read": {"conversation_id": "fixture-conversation", "reply_id": "7:100.5", "attention_signature": None, "at": 1.0}}
        self.assertTrue(mark_read_result(ok, mark()))
        self.assertTrue(mark_read_result({**ok, "applied": {"reply_id": False, "attention_signature": False}, "read": None}, mark()))
        self.assertTrue(mark_read_result({"ok": False, "error": "not_found"}, mark()))
        for invalid in ({**ok, "read": {**ok["read"], "conversation_id": "other"}},
                        {**ok, "applied": {"reply_id": 1, "attention_signature": False}},
                        {"ok": False, "error": "other"}, {"ok": True}, None):
            self.assertFalse(mark_read_result(invalid, mark()))


class MarkReadConnector(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.connector = Connector({"server_url": "https://relay.example.invalid", "node_token": "fixture-token",
                                    "state_dir": self.temp.name})
        self.connector.sessions = {NAME}
        self.connector.read_supported = True

    async def asyncTearDown(self):
        self.connector.journal.close()
        self.temp.cleanup()

    async def test_maps_to_native_read_and_returns_its_stdout_once(self):
        receipt = {"ok": True, "applied": {"reply_id": True, "attention_signature": True},
                   "read": {"conversation_id": "fixture-conversation", "reply_id": "7:100.5",
                            "attention_signature": SIGNATURE, "at": 1.0}}
        request = command()
        with patch.object(self.connector, "native", AsyncMock(return_value=receipt)) as native:
            response = await self.connector.execute(request)
            self.assertEqual(response, {"state": "completed", "result": receipt, "error": None})
            self.assertEqual(native.call_args.args, (["read", NAME, "--json"], request["payload"]))
            self.assertEqual(native.call_args.kwargs, {"failure_json": True})
            self.assertEqual(await self.connector.execute(request), response)
            self.assertEqual(native.await_count, 1)

    async def test_not_found_is_a_completed_result(self):
        with patch.object(self.connector, "native", AsyncMock(return_value={"ok": False, "error": "not_found"})):
            response = await self.connector.execute(command())
        self.assertEqual((response["state"], response["result"]), ("completed", {"ok": False, "error": "not_found"}))

    async def test_invalid_or_unknown_targets_never_spawn(self):
        with patch.object(self.connector, "native", AsyncMock()) as native:
            for request in (command(mark(attention_signature="bad")), command(session="codex/other"),
                            command(session="@peer"), {**command(), "archive_id": str(uuid.uuid4())}):
                self.assertEqual((await self.connector.execute(request))["state"], "failed")
            native.assert_not_awaited()

    async def test_old_native_without_read_abi_is_not_advertised_or_executed(self):
        self.connector.read_supported = False
        with patch.object(self.connector, "native", AsyncMock()) as native:
            response = await self.connector.execute(command())
            native.assert_not_awaited()
        self.assertEqual(response["state"], "failed")
        self.assertIn("read", response["error"])
        old_help = "hgs session-action <session> --json scoped native lifecycle action"
        for help_text, supported in ((old_help, False), (old_help + "\nhgs [@host] read <session> --json   mark", True)):
            self.connector.capabilities_next_poll = 0
            with patch.object(self.connector, "native", AsyncMock(return_value=help_text)):
                await self.connector.probe_capabilities()
            self.assertIs(self.connector.read_supported, supported)

    async def test_old_native_snapshot_omits_mark_read(self):
        old_help = "hgs session-action <session> --json scoped native lifecycle action"
        async def native(argv, *args, **kwargs):
            return old_help if argv == ["--help"] else {"sessions": []}
        with patch.object(self.connector, "native", AsyncMock(side_effect=native)):
            snapshot = await self.connector.snapshot()
        self.assertNotIn("mark_read", snapshot["mobile_capabilities"]["operations"])

    def test_mark_read_is_read_class(self):
        self.assertIn("mark_read", READ_OPERATIONS)
        self.assertNotIn("mark_read", MUTATIONS)
        self.assertIn("'mark_read'", READ_SQL)


class MarkReadFixture(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.env = patch.dict(os.environ, {"ZERUS_MOBILE_FIXTURE_DIR": str(self.root)})
        self.env.start()
        self.connector = Connector({"server_url": "https://relay.example.invalid", "node_token": "fixture-token",
            "hgs_path": str(Path(__file__).with_name("fixture_hgs.py")), "state_dir": str(self.root / "journal")})
        self.snapshot = await self.connector.snapshot()

    async def asyncTearDown(self):
        self.connector.journal.close()
        self.env.stop()
        self.temp.cleanup()

    def config(self, **values):
        (self.root / "fixture-config.json").write_text(json.dumps(values))

    def writes(self):
        rows = [json.loads(line) for line in (self.root / "mutations.jsonl").read_text().splitlines()]
        return [row for row in rows if row["operation"] == "read"]

    async def test_native_exit_status_one_still_returns_not_found(self):
        self.assertIn("mark_read", self.snapshot["mobile_capabilities"]["operations"])
        self.config(read_current={"reply_id": "7:100.5"})
        applied = await self.connector.execute(command(mark(attention_signature=None)))
        self.assertEqual(applied["state"], "completed")
        self.assertTrue(applied["result"]["applied"]["reply_id"])
        stale = await self.connector.execute(command(mark(reply_id="6:90.5", attention_signature=None)))
        self.assertEqual(stale["result"]["applied"], {"reply_id": False, "attention_signature": False})
        self.config(read_not_found=True)
        missing = await self.connector.execute(command())
        self.assertEqual((missing["state"], missing["result"]), ("completed", {"ok": False, "error": "not_found"}))
        self.assertEqual(len(self.writes()), 2)


if __name__ == "__main__":
    unittest.main()
