import base64
import copy
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import AsyncMock, patch
import uuid

from aiohttp import ClientSession
from relay_fixture import start_relay
from zerus_mobile import speech_config
from zerus_mobile.connector import Connector, ConnectorError
from relay_oracle import validate_request

MACHINE = "00000000-0000-4000-8000-000000000001"


def command(operation="speech_profile_upsert", operation_id=None):
    identity = str(uuid.uuid4())
    payload = {"request_id": identity, "expected_machine_id": MACHINE}
    if operation == "speech_config":
        payload = {}
    elif operation == "speech_config_status":
        payload = {"operation_id": operation_id or str(uuid.uuid4()), "expected_machine_id": MACHINE}
    elif operation == "speech_profile_upsert":
        payload.update(expected_revision=0, profile={"id": "example", "name": "Example transcription",
            "adapter": "openai_http", "model": "fixture", "language": "ru",
            "base_url": "https://speech.example.invalid/v1", "allow_insecure_http": False},
            credential={"mode": "replace", "sealed_value": {"key_id": "a" * 64,
                "algorithm": speech_config.ALGORITHM,
                "ephemeral_public_key": base64.b64encode(bytes(91)).decode(),
                "nonce": base64.b64encode(bytes(12)).decode(),
                "ciphertext": base64.b64encode(bytes(40)).decode()}})
    elif operation == "speech_profile_remove":
        payload.update(expected_revision=1, profile_id="example")
    elif operation == "speech_profile_test":
        payload.update(expected_profile_revision=1, profile_id="example")
    return {"request_id": identity, "computer_id": str(uuid.uuid4()), "operation": operation,
            "session": "", "payload": payload}


def receipt(request):
    return {"schema": 1, "machine_id": MACHINE,
            "request_id": request["payload"].get("operation_id", request["request_id"]),
            "state": "completed", "revision": 1, "profile_id": "example", "profile_revision": 1}


def configuration():
    return {"schema": 1, "machine_id": MACHINE, "revision": 0,
            "service": {"available": False, "reason": "Speech service is not running"},
            "adapters": [{"id": "gigaam", "models": ["v3_e2e_rnnt"], "devices": ["cpu", "cuda"]}, {"id": "openai_http"}],
            "profiles": [], "credential_key": {"key_id": "a" * 64,
                "algorithm": speech_config.ALGORITHM, "public_key_spki": base64.b64encode(bytes(91)).decode()}}


class SpeechConfigValidationTests(unittest.TestCase):
    def test_voice_routes_preserve_optional_exact_profile_revision(self):
        identity = str(uuid.uuid4())
        request = {"request_id": identity, "computer_id": str(uuid.uuid4()), "operation": "voice_submit",
                   "session": "codex/example/session", "payload": {"request_id": identity,
                       "expected_run_id": "fixture-run", "expected_conversation_id": "fixture-conversation",
                       "speech": {"machine_id": MACHINE, "profile_id": "example", "profile_revision": 7},
                       "audio": {"format": "pcm_s16le", "sample_rate": 16000, "channels": 1, "data_base64": "AAAAAA=="}}}
        validate_request(request)
        for revision in (-1, True, "7", 2**53):
            request["payload"]["speech"]["profile_revision"] = revision
            with self.subTest(revision=revision), self.assertRaises(ValueError):
                validate_request(request)
        del request["payload"]["speech"]["profile_revision"]
        validate_request(request)

    def test_all_operations_are_computer_scoped_and_exact(self):
        for operation in speech_config.OPERATIONS:
            with self.subTest(operation=operation):
                request = command(operation)
                validate_request(request)
                request["session"] = "codex/example/session"
                with self.assertRaises(ValueError):
                    validate_request(request)

    def test_plaintext_credentials_and_unbounded_envelopes_are_rejected(self):
        changes = [
            lambda p: p.update(credential={"mode": "replace", "value": "fixture-only-key"}),
            lambda p: p.update(credential={"mode": "keep", "value": "fixture-only-key"}),
            lambda p: p["credential"]["sealed_value"].update(value="fixture-only-key"),
            lambda p: p["credential"]["sealed_value"].update(nonce="AAAA"),
            lambda p: p["credential"]["sealed_value"].update(ciphertext=base64.b64encode(bytes(4113)).decode()),
            lambda p: p["credential"]["sealed_value"].update(ephemeral_public_key=base64.b64encode(bytes(129)).decode()),
            lambda p: p["credential"]["sealed_value"].update(key_id="a" * 63),
            lambda p: p["profile"].update(allow_insecure_http="true"),
            lambda p: p["profile"].update(base_url="https://speech.example.invalid/\n"),
            lambda p: p["profile"].update(id="../example"),
            lambda p: p.update(request_id=str(uuid.uuid4())),
            lambda p: p.update(expected_revision=True),
            lambda p: p.update(expected_revision=2**53),
        ]
        for change in changes:
            request = command()
            change(request["payload"])
            with self.subTest(payload=request["payload"]), self.assertRaises(ValueError):
                validate_request(request)

    def test_receipts_reject_unexpected_content_and_wrong_identity(self):
        request = command()
        self.assertTrue(speech_config.result_valid(request["operation"], receipt(request), request["payload"]))
        for field, value in (("credential", {"value": "fixture-only-key"}), ("text", "Unexpected transcript"),
                             ("machine_id", str(uuid.uuid4())), ("request_id", str(uuid.uuid4()))):
            reply = {**receipt(request), field: value}
            self.assertFalse(speech_config.result_valid(request["operation"], reply, request["payload"]))
        config = configuration()
        self.assertTrue(speech_config.result_valid("speech_config", config, {}))
        config["credential_key"]["private_key"] = "fixture-only-key"
        self.assertFalse(speech_config.result_valid("speech_config", config, {}))


class SpeechConfigConnectorTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.connector = Connector({"server_url": "https://relay.example.invalid", "node_token": "fixture-token", "state_dir": self.temp.name})
        self.connector.speech_config_supported = True

    async def asyncTearDown(self):
        self.connector.journal.close()
        self.temp.cleanup()

    async def test_capability_is_independent_and_request_is_not_replayed(self):
        request = command()
        with patch.object(self.connector, "native", AsyncMock(return_value=receipt(request))) as native:
            reply = await self.connector.execute(request)
            self.assertEqual(reply["state"], "completed", reply)
            self.assertEqual(await self.connector.execute(request), reply)
            native.assert_awaited_once_with(["speech", "--json"], {"action": "profile_upsert", **request["payload"]}, timeout=35, failure_json=True)
        self.connector.speech_config_supported = False
        self.connector.speech_supported = True
        with patch.object(self.connector, "native", AsyncMock()) as native:
            self.assertEqual((await self.connector.execute(command()))["state"], "failed")
            native.assert_not_awaited()

    async def test_malformed_result_never_persists_a_key_or_replays(self):
        request = command()
        with patch.object(self.connector, "native", AsyncMock(return_value={**receipt(request), "credential": "fixture-only-key"})) as native:
            reply = await self.connector.execute(request)
            self.assertEqual(reply["state"], "uncertain")
            self.assertEqual(await self.connector.execute(request), reply)
            self.assertEqual(native.await_count, 1)
        stored = self.connector.journal.db.execute("SELECT response FROM requests WHERE request_id=?", (request["request_id"],)).fetchone()[0]
        self.assertNotIn("fixture-only-key", stored)

    async def test_redacted_management_reads_expire(self):
        request = command("speech_config")
        with patch.object(self.connector, "native", AsyncMock(return_value=configuration())):
            self.assertEqual((await self.connector.execute(request))["result"], configuration())
        self.connector.journal.delivered(request["request_id"])
        self.connector.journal.prune(now=10**12)
        self.assertIsNone(self.connector.journal.db.execute("SELECT 1 FROM requests WHERE request_id=?", (request["request_id"],)).fetchone())

    async def test_transport_stderr_does_not_enter_durable_receipts(self):
        request = command()
        with patch.object(self.connector, "native", AsyncMock(side_effect=ConnectorError("fixture-only-key from stderr"))):
            result = await self.connector.execute(request)
        self.assertEqual(result["state"], "uncertain")
        self.assertNotIn("fixture-only-key", json.dumps(result))
        stored = self.connector.journal.db.execute("SELECT response FROM requests WHERE request_id=?", (request["request_id"],)).fetchone()[0]
        self.assertNotIn("fixture-only-key", stored)


class SpeechConfigHttpTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="zerus-speech-config-http-")
        self.root = Path(self.temp.name)
        self.relay = await start_relay(workspace="Provider fixture", computer="Processing computer")
        self.addAsyncCleanup(self.relay.cleanup)
        url, self.node = self.relay.url, self.relay.node
        self.phone = await self.relay.pair("Fixture phone", self.relay.invitation["pair_code"])
        self.client = ClientSession(base_url=url)
        self.headers = {"Authorization": "Bearer " + self.phone["device_token"]}
        self.node_headers = {"Authorization": "Bearer " + self.node["node_token"]}
        await self.call("POST", "/v1/node/heartbeat", {"snapshot": {"sessions": [],
            "mobile_capabilities": {"protocol_version": 1, "operations": sorted(speech_config.OPERATIONS)}}}, node=True)

    async def asyncTearDown(self):
        await self.client.close()
        self.temp.cleanup()

    async def call(self, method, path, body=None, *, node=False, headers=None, status=200):
        async with self.client.request(method, path, json=body,
                headers=headers or (self.node_headers if node else self.headers)) as response:
            result = await response.json()
            self.assertEqual(response.status, status, result)
            return result

    async def save(self):
        request = command()
        request["computer_id"] = self.node["node_id"]
        await self.call("POST", "/v1/requests", request, status=202)
        claimed = (await self.call("GET", "/v1/node/requests", node=True))["requests"]
        self.assertEqual(len(claimed), 1)
        self.assertEqual(claimed[0]["payload"], request["payload"])
        reply = {"state": "completed", "result": receipt(request), "error": None}
        await self.call("POST", f'/v1/node/requests/{request["request_id"]}/result', reply, node=True)
        return request

    async def test_sealed_mutation_is_erased_and_keeps_no_replay_receipt(self):
        request = await self.save()
        replay = await self.call("POST", "/v1/requests", request, status=202)
        self.assertEqual(replay["result"], receipt(request))
        self.assertEqual((await self.call("GET", "/v1/node/requests", node=True))["requests"], [])
        changed = copy.deepcopy(request)
        changed["payload"]["profile"]["name"] = "Changed profile"
        await self.call("POST", "/v1/requests", changed, status=409)
        plaintext = command()
        plaintext["computer_id"] = self.node["node_id"]
        plaintext["payload"]["credential"] = {"mode": "replace", "value": "fixture-only-key"}
        await self.call("POST", "/v1/requests", plaintext, status=400)
        # Rejected before persistence: the phone finds no receipt.
        await self.call("GET", "/v1/requests/" + plaintext["request_id"], status=404)

    async def test_status_is_owned_by_original_phone_and_computer(self):
        request = await self.save()
        status = command("speech_config_status", request["request_id"])
        status["computer_id"] = self.node["node_id"]
        other = await self.relay.pair("Other fixture phone")
        await self.call("POST", "/v1/requests", status,
            headers={"Authorization": "Bearer " + other["device_token"]}, status=404)
        missing = command("speech_config_status")
        missing["computer_id"] = self.node["node_id"]
        await self.call("POST", "/v1/requests", missing, status=404)
        await self.call("POST", "/v1/requests", status, status=202)

    async def test_real_cli_management_roundtrip_without_inference_runtime(self):
        repo = Path(__file__).resolve().parents[3]
        binary = Path(os.environ.get("HGS_TEST_BIN", repo / "target/debug/hgs")).resolve()
        configuration_dir = self.root / "config"
        configuration_dir.mkdir(mode=0o755)
        (configuration_dir / "speech.json").write_text(json.dumps({
            "schema": 1, "profiles": [], "state_dir": str(self.root / "speech-state")}))
        private_env = {key: value for key, value in os.environ.items() if not key.startswith("HGS_")}
        private_env.update(HGS_CONFIG_DIR=str(configuration_dir), HGS_STATE_DIR=str(self.root / "native-state"),
                           HGS_SELF="speech-config-fixture", HGS_PEERS="")
        connector = Connector({"server_url": "https://relay.example.invalid", "node_token": "fixture-token",
                               "hgs_path": str(binary), "state_dir": str(self.root / "connector")})
        connector.speech_config_supported = True
        async def execute(request):
            request["computer_id"] = self.node["node_id"]
            await self.call("POST", "/v1/requests", request, status=202)
            claimed = (await self.call("GET", "/v1/node/requests", node=True))["requests"]
            self.assertEqual(len(claimed), 1)
            with patch("zerus_mobile.connector.native_environment", return_value=private_env):
                result = await connector.execute(claimed[0])
            self.assertEqual(result["state"], "completed", result)
            await self.call("POST", f'/v1/node/requests/{request["request_id"]}/result', result, node=True)
            return (await self.call("GET", "/v1/requests/" + request["request_id"]))["result"]
        try:
            config = await execute(command("speech_config"))
            self.assertFalse(config["service"]["available"])
            change = command()
            change["payload"].update(expected_machine_id=config["machine_id"], expected_revision=config["revision"],
                                      credential={"mode": "keep"})
            accepted = await execute(change)
            self.assertEqual(accepted["state"], "completed", accepted)
            status = command("speech_config_status", change["request_id"])
            status["payload"]["expected_machine_id"] = config["machine_id"]
            self.assertEqual(await execute(status), accepted)
            current = await execute(command("speech_config"))
            self.assertEqual(current["profiles"][0]["id"], "example")
            self.assertFalse(current["profiles"][0]["credential"]["configured"])
            self.assertGreater(current["revision"], config["revision"])
            remove = command("speech_profile_remove")
            remove["payload"].update(expected_machine_id=config["machine_id"], expected_revision=current["revision"])
            self.assertEqual((await execute(remove))["state"], "completed")
            self.assertEqual((await execute(command("speech_config")))["profiles"], [])
            self.assertEqual(configuration_dir.stat().st_mode & 0o777, 0o755)
        finally:
            connector.journal.close()
