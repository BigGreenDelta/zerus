"""Speech transport contracts without microphones, models or live agents."""
import base64
import asyncio
import copy
import json
import os
from pathlib import Path
import sys
import tempfile
import threading
import unittest
from unittest.mock import AsyncMock, patch
import uuid

from zerus_mobile import speech
from zerus_mobile.connector import Connector
from zerus_mobile.server import validate_request
from aiohttp import ClientSession
from relay_fixture import start_relay

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / "services/speech"))
from zerus_speech.config import load as speech_config
from zerus_speech.service import Server as SpeechServer


def command(operation="speech_submit", job=None):
    identity = str(uuid.uuid4())
    payload = ({"request_id": identity, "profile_id": "gigaam-ru", "audio": {
        "format": "pcm_s16le", "sample_rate": 16000, "channels": 1,
        "data_base64": base64.b64encode(b"\0\0" * 160).decode()}}
        if operation == "speech_submit" else {"job_id": job} if job else {})
    return {"request_id": identity, "computer_id": str(uuid.uuid4()),
            "operation": operation, "session": "", "payload": payload}


class SpeechValidationTests(unittest.TestCase):
    def test_all_computer_operations_have_exact_shapes(self):
        for op in speech.OPERATIONS:
            request = command(op, str(uuid.uuid4()) if op in {"speech_status", "speech_cancel"} else None)
            validate_request(request)
            changed = copy.deepcopy(request)
            changed["session"] = "codex/example/session"
            with self.assertRaises(ValueError):
                validate_request(changed)

    def test_invalid_audio_identity_and_extra_fields_are_rejected(self):
        for edit in [lambda p: p.update(request_id=str(uuid.uuid4())),
                     lambda p: p.update(path="/etc/passwd"),
                     lambda p: p["audio"].update(sample_rate=True),
                     lambda p: p["audio"].update(channels=True),
                     lambda p: p["audio"].update(data_base64="AA=="),
                     lambda p: p["audio"].update(data_base64="AA==AA=="),
                     lambda p: p["audio"].update(data_base64="AA B"),
                     lambda p: p["audio"].update(data_base64="AAAA"),
                     lambda p: p.update(profile_id="../provider")]:
            request = command()
            edit(request["payload"])
            with self.assertRaises(ValueError):
                validate_request(request)

    def test_maximum_audio_has_bounded_valid_encoding(self):
        request = command()
        request["payload"]["audio"]["data_base64"] = base64.b64encode(bytes(speech.MAX_AUDIO_BYTES)).decode()
        validate_request(request)
        request["payload"]["audio"]["data_base64"] += "AAAA"
        with self.assertRaises(ValueError):
            validate_request(request)


class SpeechConnectorTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.connector = Connector({"server_url": "https://relay.example.invalid", "node_token": "fixture-token", "state_dir": self.temp.name})
        self.connector.speech_supported = True

    async def asyncTearDown(self):
        self.connector.journal.close()
        self.temp.cleanup()

    async def test_submit_returns_job_without_waiting_for_inference_and_never_replays(self):
        request = command()
        result = {"schema": 1, "job_id": request["request_id"], "state": "queued"}
        with patch.object(self.connector, "native", AsyncMock(return_value=result)) as native:
            response = await self.connector.execute(request)
            self.assertEqual(response["state"], "completed")
            self.assertEqual(response["result"]["state"], "queued")
            self.assertEqual(await self.connector.execute(request), response)
            native.assert_awaited_once_with(["speech", "--json"], {"action": "submit", **request["payload"]}, timeout=10)

    async def test_mismatched_job_is_uncertain_and_old_cli_never_runs_speech(self):
        with patch.object(self.connector, "native", AsyncMock(return_value={"schema": 1, "job_id": str(uuid.uuid4()), "state": "queued"})) as native:
            request = command()
            self.assertEqual((await self.connector.execute(request))["state"], "uncertain")
            self.assertEqual((await self.connector.execute(request))["state"], "uncertain")
            self.assertEqual(native.await_count, 1)
        self.connector.speech_supported = False
        with patch.object(self.connector, "native", AsyncMock()) as native:
            self.assertEqual((await self.connector.execute(command()))["state"], "failed")
            native.assert_not_awaited()

    async def test_status_is_exact_and_read_receipts_are_pruned(self):
        request = command("speech_status", str(uuid.uuid4()))
        result = {"schema": 1, "job_id": request["payload"]["job_id"], "state": "completed", "text": "Example dictation"}
        with patch.object(self.connector, "native", AsyncMock(return_value=result)):
            self.assertEqual((await self.connector.execute(request))["result"], result)
        self.connector.journal.delivered(request["request_id"])
        self.connector.journal.prune(now=10**12)
        self.assertIsNone(self.connector.journal.db.execute("SELECT 1 FROM requests WHERE request_id=?", (request["request_id"],)).fetchone())


class SpeechHttpServiceTests(unittest.IsolatedAsyncioTestCase):
    """Real relay HTTP, connector subprocess and speech socket in private fixtures."""

    async def asyncSetUp(self):
        # First, so a skipped relay leaves no speech server running.
        self.relay = await start_relay(workspace="Speech fixture", computer="Speech computer")
        self.addAsyncCleanup(self.relay.cleanup)
        self.temp = tempfile.TemporaryDirectory(prefix="zerus-speech-http-")
        self.root = Path(self.temp.name)
        config = self.root / "speech.json"
        config.write_text(json.dumps({"schema": 1, "state_dir": str(self.root / "speech"),
            "profiles": [{"id": "gigaam-ru", "name": "Synthetic speech", "adapter": "fixture",
                          "model": "fixture", "language": "ru", "text": "Synthetic dictation"}]}))
        self.speech = SpeechServer(speech_config(config), test_mode=True)
        self.thread = threading.Thread(target=self.speech.serve_forever, daemon=True)
        self.thread.start()
        url, self.node = self.relay.url, self.relay.node
        self.phone = await self.relay.pair("Speech phone", self.relay.invitation["pair_code"])
        self.client = ClientSession(base_url=url)
        self.headers = {"Authorization": "Bearer " + self.phone["device_token"]}
        self.node_headers = {"Authorization": "Bearer " + self.node["node_token"]}
        binary = Path(os.environ.get("HGS_TEST_BIN", REPO / "target/debug/hgs")).resolve()
        self.connector = Connector({"server_url": url, "node_token": self.node["node_token"],
            "hgs_path": str(binary), "state_dir": str(self.root / "journal")}, allow_insecure_localhost=True)
        self.connector.speech_supported = True
        await self.call("POST", "/v1/node/heartbeat", {"snapshot": {"sessions": [],
            "mobile_capabilities": {"protocol_version": 1, "operations": sorted(speech.OPERATIONS)}}}, node=True)

    async def asyncTearDown(self):
        self.connector.journal.close()
        await self.client.close()
        await asyncio.to_thread(self.speech.shutdown)
        self.speech.server_close()
        self.thread.join()
        self.temp.cleanup()

    async def call(self, method, path, body=None, *, node=False, headers=None, status=200):
        async with self.client.request(method, path, json=body,
                headers=headers or (self.node_headers if node else self.headers)) as response:
            result = await response.json()
            self.assertEqual(response.status, status, result)
            return result

    async def roundtrip(self, request):
        request["computer_id"] = self.node["node_id"]
        await self.call("POST", "/v1/requests", request, status=202)
        claimed = (await self.call("GET", "/v1/node/requests", node=True))["requests"]
        self.assertEqual(len(claimed), 1)
        with patch.dict(os.environ, {"HGS_CONFIG_DIR": str(self.root), "HGS_STATE_DIR": str(self.root / "native-state"),
                                     "HOME": str(self.root), "HGS_SELF": "speech-fixture", "HGS_PEERS": ""}):
            result = await self.connector.execute(claimed[0])
        self.assertEqual(result["state"], "completed", result)
        await self.call("POST", f'/v1/node/requests/{request["request_id"]}/result', result, node=True)
        self.connector.journal.delivered(request["request_id"])
        return await self.call("GET", "/v1/requests/" + request["request_id"])

    async def test_maximum_recording_crosses_http_cli_socket_once_and_is_erased(self):
        profile = await self.roundtrip(command("speech_profiles"))
        self.assertTrue(profile["result"]["available"])
        upload = command()
        upload["payload"]["audio"]["data_base64"] = base64.b64encode(bytes(speech.MAX_AUDIO_BYTES)).decode()
        submitted = await self.roundtrip(upload)
        self.assertEqual(submitted["result"]["job_id"], upload["request_id"])
        self.assertNotIn("text", submitted["result"])
        self.assertEqual(await self.call("POST", "/v1/requests", upload, status=202), submitted)
        self.assertEqual((await self.call("GET", "/v1/node/requests", node=True))["requests"], [])
        for _ in range(30):
            status = await self.roundtrip(command("speech_status", upload["request_id"]))
            if status["result"]["state"] == "completed":
                break
            await asyncio.sleep(0.01)
        self.assertEqual(status["result"]["text"], "Synthetic dictation")
        cancelled = await self.roundtrip(command("speech_cancel", upload["request_id"]))
        self.assertNotIn("text", cancelled["result"])
        totals = (await self.roundtrip(command("speech_stats")))["result"]["totals"]
        self.assertEqual((totals["submitted"], totals["completed"], totals["audio_ms"]), (1, 1, 120000))
        self.assertFalse(list((self.root / "speech").glob("*.pcm")))

    async def test_status_and_cancel_are_bound_to_phone_and_computer(self):
        upload = command()
        await self.roundtrip(upload)
        other = await self.relay.pair("Other fixture phone")
        for operation in ("speech_status", "speech_cancel"):
            foreign = command(operation, upload["request_id"])
            foreign["computer_id"] = self.node["node_id"]
            await self.call("POST", "/v1/requests", foreign,
                headers={"Authorization": "Bearer " + other["device_token"]}, status=404)
        second = await self.relay.add_node("Other fixture computer")
        second_headers = {"Authorization": "Bearer " + second["node_token"]}
        await self.call("POST", "/v1/node/heartbeat", {"snapshot": {"sessions": [],
            "mobile_capabilities": {"protocol_version": 1, "operations": sorted(speech.OPERATIONS)}}}, headers=second_headers)
        wrong = command("speech_status", upload["request_id"])
        wrong["computer_id"] = second["node_id"]
        await self.call("POST", "/v1/requests", wrong, status=404)
