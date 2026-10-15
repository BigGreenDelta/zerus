"""Durable voice transport preserves conversation authority and request identity."""
import base64
import copy
import tempfile
import unittest
import uuid
from unittest.mock import AsyncMock, patch

from zerus_mobile import voice
from zerus_mobile.connector import Connector
from relay_oracle import validate_request


def command(operation="voice_submit", message_id=None):
    identity = str(uuid.uuid4())
    payload = {"request_id": identity, "expected_run_id": "fixture-run",
               "expected_conversation_id": "fixture-conversation"}
    if operation == "voice_submit":
        payload.update(speech={"machine_id": str(uuid.uuid4()), "profile_id": "gigaam-ru"},
                       audio={"format": "pcm_s16le", "sample_rate": 16000, "channels": 1,
                              "data_base64": base64.b64encode(bytes(320)).decode()})
    elif operation in {"voice_status", "voice_cancel"}:
        payload["message_id"] = message_id or str(uuid.uuid4())
    return {"request_id": identity, "computer_id": str(uuid.uuid4()), "operation": operation,
            "session": "codex/example/session", "payload": payload}


def message(request, state="queued"):
    p = request["payload"]
    identity = p.get("message_id", p["request_id"])
    return {"schema": 1, "message": {"id": identity,
            "run_id": p["expected_run_id"], "conversation_id": p["expected_conversation_id"],
            "revision": 1, "state": state,
            "audio": {"request_id": identity, "index": 0, "name": "voice.wav",
                      "mime": "audio/wav", "bytes": 364, "duration_ms": 10,
                      "waveform": [0] * 64}}}


class VoiceValidationTests(unittest.TestCase):
    def test_operations_keep_session_and_exact_native_identity(self):
        for operation in voice.OPERATIONS:
            request = command(operation)
            validate_request(request)
            for key, value in (("request_id", str(uuid.uuid4())), ("expected_run_id", ""),
                               ("expected_conversation_id", "bad\nidentity"), ("path", "/example/private")):
                changed = copy.deepcopy(request)
                changed["payload"][key] = value
                with self.assertRaises(ValueError):
                    validate_request(changed)

    def test_routes_are_machine_identities_not_caller_aliases(self):
        for route in ({"machine_id": "arch", "profile_id": "gigaam-ru"},
                      {"machine_id": str(uuid.uuid4()), "profile_id": "../model"},
                      {"machine_id": str(uuid.uuid4()), "profile_id": "ok", "command": "example"}):
            request = command()
            request["payload"]["speech"] = route
            with self.assertRaises(ValueError):
                validate_request(request)


class VoiceConnectorTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.connector = Connector({"server_url": "https://relay.example.invalid",
                                    "node_token": "fixture-token", "state_dir": self.temp.name})
        self.connector.voice_supported = True
        self.connector.sessions.add("codex/example/session")

    async def asyncTearDown(self):
        self.connector.journal.close()
        self.temp.cleanup()

    async def test_acceptance_is_scoped_durable_and_duplicate_never_reinvokes_native(self):
        request = command()
        result = message(request)
        with patch.object(self.connector, "native", AsyncMock(return_value=result)) as native:
            response = await self.connector.execute(request)
            self.assertEqual(response["state"], "completed", response)
            self.assertEqual(response["result"], result)
            self.assertEqual(await self.connector.execute(request), response)
            native.assert_awaited_once_with(["voice", request["session"], "--json"],
                                           {"action": "submit", **request["payload"]}, timeout=30)

    async def test_wrong_conversation_ack_is_uncertain_and_not_replayed(self):
        request = command()
        result = message(request)
        result["message"]["conversation_id"] = "another-conversation"
        with patch.object(self.connector, "native", AsyncMock(return_value=result)) as native:
            self.assertEqual((await self.connector.execute(request))["state"], "uncertain")
            self.assertEqual((await self.connector.execute(request))["state"], "uncertain")
            self.assertEqual(native.await_count, 1)

    async def test_old_host_rejects_without_native_and_status_read_is_short_lived(self):
        self.connector.voice_supported = False
        with patch.object(self.connector, "native", AsyncMock()) as native:
            self.assertEqual((await self.connector.execute(command()))["state"], "failed")
            native.assert_not_awaited()
        self.connector.voice_supported = True
        request = command("voice_status")
        result = message(request, "submitted")
        result["message"]["transcript"] = "Synthetic voice message"
        with patch.object(self.connector, "native", AsyncMock(return_value=result)):
            self.assertEqual((await self.connector.execute(request))["result"], result)
        self.connector.journal.delivered(request["request_id"])
        self.connector.journal.prune(now=10**12)
        self.assertIsNone(self.connector.journal.db.execute(
            "SELECT 1 FROM requests WHERE request_id=?", (request["request_id"],)).fetchone())
