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


def video_command():
    request = command()
    data = b""
    for kind, content in ((b"ftyp", b"isom0000"), (b"moov", b""), (b"mdat", b"fixture")):
        data += (8 + len(content)).to_bytes(4, "big") + kind + content
    request["payload"]["audio"]["data_base64"] = base64.b64encode(bytes(32_000)).decode()
    request["payload"]["video"] = {"format": "mp4", "data_base64": base64.b64encode(data).decode(),
                                   "duration_ms": 1000, "width": 480, "height": 480}
    return request


class VoiceValidationTests(unittest.TestCase):
    def test_refusal_intent_preserves_nonempty_even_legacy_pcm_api_bounds(self):
        from zerus_mobile import voice_rejection
        for size in (2, 4, 320, 3198, 3200, 3_840_000):
            with self.subTest(pcm_bytes=size):
                request = command()
                payload = request["payload"]
                payload["audio"]["data_base64"] = base64.b64encode(bytes(size)).decode()
                validate_request(request)
                native_intent = voice_rejection.intent(payload)
                self.assertEqual(native_intent["audio"]["bytes"], size + 44)
                proof = {"schema": 1, "state": "rejected_before_acceptance", "request_id": request["request_id"],
                         "message_id": request["request_id"], "run_id": payload["expected_run_id"],
                         "conversation_id": payload["expected_conversation_id"], "intent": native_intent,
                         "payload_sha256": "a" * 64, "generation": 1, "retry_token": str(uuid.uuid4()),
                         "code": "route_unavailable", "detail": "The recording was not accepted."}
                self.assertTrue(voice_rejection.valid_proof(proof))
                retry = dict(payload, request_id=str(uuid.uuid4()), message_id=request["request_id"], rejection=proof)
                voice.validate("voice_submit", retry, retry["request_id"])
                self.assertTrue(voice.result_valid("voice_submit", {"schema": 1, "rejection": proof}, payload))
        for size in (0, 1, 3, 3199, 3_840_002):
            with self.subTest(invalid_pcm_bytes=size):
                payload = command()["payload"]
                payload["audio"]["data_base64"] = base64.b64encode(bytes(size)).decode()
                with self.assertRaises(ValueError):
                    voice_rejection.intent(payload)

    def test_only_new_refusal_proofs_require_exact_integer_types(self):
        from zerus_mobile import voice_rejection
        request = video_command()
        payload = request["payload"]
        proof = {"schema": 1, "state": "rejected_before_acceptance", "request_id": request["request_id"],
                 "message_id": request["request_id"], "run_id": payload["expected_run_id"],
                 "conversation_id": payload["expected_conversation_id"], "intent": voice_rejection.intent(payload),
                 "payload_sha256": "a" * 64, "generation": 1, "retry_token": str(uuid.uuid4()),
                 "code": "route_unavailable", "detail": "The recording was not accepted."}
        self.assertTrue(voice_rejection.valid_proof(proof))
        for value in (True, 1.0, "1"):
            with self.subTest(outer_schema=value):
                self.assertFalse(voice.result_valid("voice_submit", {"schema": value, "rejection": proof}, payload))
        for field, value in (("channels", True), ("channels", 1.0), ("sample_rate", 16000.0)):
            with self.subTest(field=field, value=value):
                wrong = copy.deepcopy(proof); wrong["intent"]["audio"][field] = value
                self.assertFalse(voice_rejection.valid_proof(wrong))
                retry = dict(payload, request_id=str(uuid.uuid4()), message_id=request["request_id"], rejection=wrong)
                with self.assertRaises(ValueError):
                    voice.validate("voice_submit", retry, retry["request_id"])
                self.assertFalse(voice.result_valid("voice_submit", {"schema": 1, "rejection": wrong}, payload))
        # This review tightens the refusal extension only. Legacy response
        # classification and ordinary envelopes retain their existing behavior.
        legacy = command()
        validate_request(legacy)
        for value in (1, True, 1.0):
            result = message(legacy); result["schema"] = value
            self.assertTrue(voice.result_valid("voice_submit", result, legacy["payload"]))

    def test_video_keeps_existing_speech_identity_and_rejects_unbounded_or_incomplete_media(self):
        request = video_command()
        validate_request(request)
        for field, value in (("duration_ms", 60_001), ("duration_ms", True), ("width", 0),
                             ("height", 1921), ("format", "avi"), ("data_base64", "AAAA"),
                             ("poster_base64", "AAAA"), ("path", "/workspace/video.mp4")):
            changed = copy.deepcopy(request)
            changed["payload"]["video"][field] = value
            with self.assertRaises(ValueError, msg=field):
                validate_request(changed)
        changed = copy.deepcopy(request)
        changed["payload"]["audio"]["data_base64"] = base64.b64encode(bytes(96_000)).decode()
        with self.assertRaises(ValueError):
            validate_request(changed)

    def test_native_refusal_is_not_an_error_and_retry_pins_original_identity_and_body(self):
        from zerus_mobile import voice_rejection
        request = video_command()
        payload = request["payload"]
        proof = {"schema": 1, "state": "rejected_before_acceptance", "request_id": request["request_id"],
                 "message_id": request["request_id"], "run_id": payload["expected_run_id"],
                 "conversation_id": payload["expected_conversation_id"], "intent": voice_rejection.intent(payload),
                 "payload_sha256": "a" * 64, "generation": 1, "retry_token": str(uuid.uuid4()),
                 "code": "route_unavailable", "detail": "The recording was not accepted."}
        self.assertTrue(voice.result_valid("voice_submit", {"schema": 1, "rejection": proof}, payload))
        operation = str(uuid.uuid4())
        retry = copy.deepcopy(request)
        retry["request_id"] = retry["payload"]["request_id"] = operation
        retry["payload"].update(message_id=request["request_id"], rejection=proof)
        validate_request(retry)
        for field, value in (("message_id", str(uuid.uuid4())), ("expected_run_id", "other"), ("expected_conversation_id", "other")):
            wrong = copy.deepcopy(retry); wrong["payload"][field] = value
            with self.assertRaises(ValueError): validate_request(wrong)
        for field in ("speech", "audio", "video"):
            wrong = copy.deepcopy(retry)
            if field == "speech": wrong["payload"][field]["profile_id"] = "other"
            elif field == "audio": wrong["payload"][field]["data_base64"] = base64.b64encode(bytes([1]) * 32000).decode()
            else: wrong["payload"][field]["width"] = 640
            with self.assertRaises(ValueError): validate_request(wrong)
        for field, value in (("request_id", str(uuid.uuid4())), ("message_id", str(uuid.uuid4())), ("run_id", "other"),
                             ("generation", True), ("retry_token", "bad"), ("path", "/synthetic")):
            wrong = copy.deepcopy(proof); wrong[field] = value
            self.assertFalse(voice.result_valid("voice_submit", {"schema": 1, "rejection": wrong}, payload))
        self.assertFalse(voice.result_valid("voice_submit", {"schema": 1, "error": "not found"}, payload))

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

    async def test_video_needs_verified_native_capability_and_keeps_the_same_journal_identity(self):
        request = video_command()
        with patch.object(self.connector, "native", AsyncMock()) as native:
            refused = await self.connector.execute(request)
            self.assertEqual(refused["state"], "failed")
            native.assert_not_awaited()
        request = video_command()
        self.connector.video_supported = True
        result = message(request)
        result["message"]["video"] = {"request_id": request["request_id"], "index": 1,
                                      "name": "video.mp4", "mime": "video/mp4", "duration_ms": 1000, "width": 480, "height": 480,
                                      "bytes": len(base64.b64decode(request["payload"]["video"]["data_base64"]))}
        with patch.object(self.connector, "native", AsyncMock(return_value=result)) as native:
            accepted = await self.connector.execute(request)
            self.assertEqual(accepted["state"], "completed", accepted)
            self.assertEqual(await self.connector.execute(request), accepted)
            native.assert_awaited_once_with(["voice", request["session"], "--json"],
                                           {"action": "submit", **request["payload"]}, timeout=30)

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
