"""Phones read sent attachments by receipt identity, never by a computer path."""
import base64
import hashlib
import json
import tempfile
import time
import unittest
from unittest.mock import AsyncMock, patch
import uuid

from zerus_mobile import attachment_reads
from zerus_mobile.attachments import MAX_FILE_BYTES, MAX_READ_BYTES, validate_read
from zerus_mobile.connector import ConnectorError, Connector
from zerus_mobile.context import OPERATIONS, READ_OPERATIONS, READ_SQL

SESSION = "codex/example/attachments"
MESSAGE = "00000000-0000-4000-8000-000000000001"
ARCHIVE = "55555555-5555-4555-8555-555555555555"


def read_payload(**change):
    request_id = str(uuid.uuid4())
    value = {"request_id": request_id, "expected_run_id": "example-run", "expected_conversation_id": "example-conversation",
             "message_id": MESSAGE, "index": 0, "offset": 0, "length": MAX_READ_BYTES}
    for key, item in change.items():
        if item is None:
            value.pop(key, None)
        else:
            value[key] = item
    return value


def command(**change):
    payload = read_payload(**change)
    return {"request_id": payload["request_id"], "operation": "attachment_read", "session": SESSION, "payload": payload}


def identity(**change):
    return {"name": SESSION, "run_id": "example-run", "conversation_id": "example-conversation", "subagents": {"worker-1": {}}, **change}


def native_file(raw, **change):
    return {"request_id": MESSAGE, "index": 0, "name": "Скриншот 😀.png", "mime": "image/png", "bytes": len(raw),
            "reference": "[Image #1]", "data_base64": base64.b64encode(raw).decode(), **change}


class ReadValidationTests(unittest.TestCase):
    def test_exact_receipt_position_and_bounded_range(self):
        payload = read_payload()
        validate_read(payload, payload["request_id"])
        child = read_payload(agent_id="worker-1", archive_id=ARCHIVE)
        validate_read(child, child["request_id"])
        for change in ({"path": "/private/file.png"}, {"message_id": "../../etc/passwd"}, {"message_id": "00000000-0000-4000-8000-00000000000A"},
                       {"message_id": "--archive"}, {"index": 8}, {"index": -1}, {"index": True}, {"index": 0.0},
                       {"offset": MAX_FILE_BYTES}, {"offset": -1}, {"length": 0}, {"length": MAX_READ_BYTES + 1},
                       {"offset": MAX_FILE_BYTES - 10, "length": 11}, {"expected_conversation_id": ""}, {"expected_run_id": ""},
                       {"agent_id": "../child"}, {"archive_id": "00000000-0000-0000-0000-000000000000"},
                       {"message_id": None}, {"length": None}):
            with self.subTest(change=change):
                invalid = read_payload(**change)
                with self.assertRaises(ValueError):
                    validate_read(invalid, invalid["request_id"])
        with self.assertRaises(ValueError):
            validate_read(payload, str(uuid.uuid4()))

    def test_operation_is_a_short_lived_read_outside_the_indexed_read_text(self):
        self.assertIn("attachment_read", OPERATIONS)
        self.assertIn("attachment_read", READ_OPERATIONS)
        self.assertNotIn("attachment_read", READ_SQL)

    def test_memory_cache_is_bounded_least_recent_and_expires(self):
        now = [100.0]
        cache = attachment_reads.AttachmentReads(max_bytes=10, ttl=120, clock=lambda: now[0])
        entry = lambda size: {"bytes": size, "data": b"x" * size}
        cache.put(("a",), entry(4))
        cache.put(("b",), entry(4))
        self.assertIsNotNone(cache.get(("a",)))
        cache.put(("c",), entry(4))
        self.assertIsNone(cache.get(("b",)))
        self.assertIsNotNone(cache.get(("a",)))
        cache.put(("huge",), entry(11))
        self.assertIsNone(cache.get(("huge",)))
        now[0] += 121
        self.assertIsNone(cache.get(("a",)))
        self.assertEqual(cache.size, 0)


class ConnectorReadTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.connector = Connector({"server_url": "https://relay.example.invalid", "node_token": "fixture-token", "state_dir": self.directory.name})
        self.connector.sessions = {SESSION}
        self.connector.archives = {(SESSION, ARCHIVE)}
        self.connector.snapshot_seen_at = time.monotonic()
        self.connector.attachment_read_supported = True

    async def asyncTearDown(self):
        self.connector.journal.close()
        self.directory.cleanup()

    def native(self, answer, detail=None, calls=None):
        calls = [] if calls is None else calls

        async def run(argv, payload=None, **options):
            calls.append((argv, options))
            if argv[0] == "inspect":
                return identity() if detail is None else detail
            if argv[0] == "attachment":
                if isinstance(answer, Exception):
                    raise answer
                return answer
            raise AssertionError(argv)
        return patch.object(self.connector, "native", AsyncMock(side_effect=run)), calls

    async def test_ranges_return_exact_bytes_digest_and_reuse_one_native_read(self):
        raw = bytes(range(256)) * 4097  # Three ranges, the last one partial.
        mock, calls = self.native(native_file(raw))
        received = b""
        with mock:
            while len(received) < len(raw):
                request = command(offset=len(received), length=min(MAX_READ_BYTES, len(raw) - len(received)))
                response = await self.connector.execute(request)
                self.assertEqual(response["state"], "completed", response)
                result = response["result"]
                self.assertEqual((result["request_id"], result["name"], result["message_id"], result["index"]),
                                 (request["request_id"], SESSION, MESSAGE, 0))
                self.assertEqual((result["bytes"], result["sha256"], result["file_name"], result["mime"], result["offset"]),
                                 (len(raw), hashlib.sha256(raw).hexdigest(), "Скриншот 😀.png", "image/png", len(received)))
                self.assertLess(len(json.dumps({"request_id": request["request_id"], **response}).encode()), 1024 * 1024)
                received += base64.b64decode(result["data_base64"])
        self.assertEqual(received, raw)
        native = [argv for argv, _ in calls if argv[0] == "attachment"]
        self.assertEqual(native, [["attachment", SESSION, "--request", MESSAGE, "--index", "0", "--conversation", "example-conversation"]])
        self.assertEqual([options for argv, options in calls if argv[0] == "attachment"][0]["max_output_bytes"],
                         attachment_reads.NATIVE_OUTPUT_BYTES)
        self.assertEqual([argv for argv, _ in calls if argv[0] == "inspect"], [["inspect", SESSION, "--skip-processes"]])

    async def test_archived_and_child_identity_reach_native_flags(self):
        raw = b"archived child file"
        detail = identity(archive_id=ARCHIVE, state="archived")
        mock, calls = self.native(native_file(raw), detail)
        with mock:
            response = await self.connector.execute(command(archive_id=ARCHIVE, agent_id="worker-1", length=len(raw)))
        self.assertEqual(response["state"], "completed", response)
        self.assertEqual((response["result"]["archive_id"], response["result"]["agent_id"]), (ARCHIVE, "worker-1"))
        self.assertEqual([argv for argv, _ in calls], [["inspect", SESSION, "--archive", ARCHIVE, "--skip-processes"],
            ["attachment", SESSION, "--request", MESSAGE, "--index", "0", "--conversation", "example-conversation", "--archive", ARCHIVE, "--agent", "worker-1"]])

    async def test_foreign_session_identity_and_answers_are_refused(self):
        raw = b"example"
        cases = [
            (dict(session="codex/example/other"), native_file(raw), None),  # Not an exact snapshot entry.
            (dict(session="--help"), native_file(raw), None),
            ({}, native_file(raw), identity(run_id="other-run")),
            ({}, native_file(raw), identity(conversation_id="other-conversation")),
            ({}, native_file(raw), identity(archive_id=ARCHIVE, state="archived")),
            (dict(archive_id=str(uuid.uuid4())), native_file(raw), None),  # Unknown archive.
            (dict(agent_id="worker-2"), native_file(raw), None),
            ({}, native_file(raw, request_id=str(uuid.uuid4())), None),
            ({}, native_file(raw, index=1), None),
            ({}, native_file(raw, bytes=len(raw) + 1), None),
            ({}, native_file(raw, data_base64="not base64"), None),
            ({}, native_file(b""), None),
            ({}, native_file(b"x" * (MAX_FILE_BYTES + 1)), None),  # Oversize.
            ({}, ConnectorError("native command failed"), None),  # Missing file, symlink or traversal refused natively.
            (dict(offset=len(raw)), native_file(raw), None),
        ]
        for change, answer, detail in cases:
            with self.subTest(change=change, answer=str(answer)[:60], detail=detail):
                session = change.pop("session", SESSION)
                request = command(**change)
                request["session"] = session
                mock, calls = self.native(answer, detail)
                self.connector.attachment_reads = attachment_reads.AttachmentReads()
                with mock:
                    response = await self.connector.execute(request)
                self.assertEqual(response["state"], "failed", response)
                self.assertIsNone(response["result"])
                if session != SESSION or "archive_id" in change:
                    self.assertEqual(calls, [])
        mock, _ = self.native(ConnectorError("native command failed"))
        self.connector.attachment_reads = attachment_reads.AttachmentReads()
        with mock:
            response = await self.connector.execute(command())
        self.assertIn("not available", response["error"])

    async def test_unsupported_native_is_not_advertised_or_executed(self):
        self.connector.attachment_read_supported = False
        mock, calls = self.native(native_file(b"x"))
        with mock:
            response = await self.connector.execute(command(length=1))
        self.assertEqual(response["state"], "failed")
        self.assertIn("unavailable", response["error"])
        self.assertEqual(calls, [])
        help_text = "hgs history <session> --json\nhgs [@host] read <session> --json"
        with patch.object(self.connector, "native", AsyncMock(return_value=help_text)):
            self.connector.capabilities_next_poll = 0
            await self.connector.probe_capabilities()
        self.assertFalse(self.connector.attachment_read_supported)
        with patch.object(self.connector, "native", AsyncMock(return_value=help_text + "\nhgs [@host] attachment <session> --request ID --index N --conversation ID")):
            self.connector.capabilities_next_poll = 0
            await self.connector.probe_capabilities()
        self.assertTrue(self.connector.attachment_read_supported)

    async def test_delivered_chunks_leave_the_journal_like_terminal_snapshots(self):
        raw = b"journal retention"
        mock, _ = self.native(native_file(raw))
        request = command(length=len(raw))
        with mock:
            self.assertEqual((await self.connector.execute(request))["state"], "completed")
        journal = self.connector.journal
        journal.delivered(request["request_id"])
        journal.prune(now=time.time() + 60)
        self.assertEqual(journal.db.execute("SELECT count(*) FROM requests WHERE request_id=?", (request["request_id"],)).fetchone()[0], 1)
        journal.prune(now=time.time() + 121)
        self.assertEqual(journal.db.execute("SELECT count(*) FROM requests WHERE request_id=?", (request["request_id"],)).fetchone()[0], 0)


if __name__ == "__main__":
    unittest.main()
