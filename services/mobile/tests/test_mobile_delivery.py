"""Shared phone delivery setting through the connector, with synthetic native hgs only."""
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import AsyncMock, patch
import uuid

from zerus_mobile.connector import Connector


def command(value="away_10", **extra):
    return {"request_id": str(uuid.uuid4()), "operation": "set_mobile_delivery", "session": "",
            "payload": {"value": value}, **extra}


class MobileDeliveryConnectorTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.connector = Connector({"server_url": "https://relay.example.invalid", "node_token": "fixture-token",
                                    "state_dir": self.temp.name})
        self.connector.preference_supported = True

    async def asyncTearDown(self):
        self.connector.journal.close()
        self.temp.cleanup()

    async def test_exact_native_command_and_receipt_are_returned_once(self):
        request = command()
        with patch.object(self.connector, "native", AsyncMock(return_value={"ok": True, "mobile_delivery": "away_10"})) as native:
            response = await self.connector.execute(request)
            self.assertEqual(response, {"state": "completed", "result": {"ok": True, "mobile_delivery": "away_10"}, "error": None})
            self.assertEqual(await self.connector.execute(request), response)
            native.assert_awaited_once_with(["swarm", "preference", "set", "mobile_delivery", "away_10", "--json"], timeout=10)

    async def test_mismatched_acknowledgement_is_uncertain_and_never_replayed(self):
        request = command()
        with patch.object(self.connector, "native", AsyncMock(return_value={"ok": True, "mobile_delivery": "away"})) as native:
            self.assertEqual((await self.connector.execute(request))["state"], "uncertain")
            self.assertEqual((await self.connector.execute(request))["state"], "uncertain")
            self.assertEqual(native.await_count, 1)

    async def test_invalid_values_sessions_or_fields_never_spawn(self):
        invalid = [command("later"), command(session="codex/example/mobile"), {**command(), "payload": {}},
                   {**command(), "payload": {"value": "away", "request_id": "fixture"}}, {**command(), "payload": ["away"]}]
        with patch.object(self.connector, "native", AsyncMock()) as native:
            for request in invalid:
                self.assertEqual((await self.connector.execute(request))["state"], "failed")
            native.assert_not_awaited()

    async def test_unsupported_native_cli_fails_before_any_mutation(self):
        self.connector.preference_supported = False
        with patch.object(self.connector, "native", AsyncMock()) as native:
            self.assertEqual((await self.connector.execute(command()))["state"], "failed")
            native.assert_not_awaited()


class MobileDeliveryFixtureTests(unittest.IsolatedAsyncioTestCase):
    async def test_only_the_gateway_advertises_and_its_snapshot_reports_the_value(self):
        with tempfile.TemporaryDirectory() as directory, patch.dict(os.environ, {"ZERUS_MOBILE_FIXTURE_DIR": directory}):
            connector = Connector({"server_url": "https://relay.example.invalid", "node_token": "fixture-token",
                                   "hgs_path": str(Path(__file__).with_name("fixture_hgs.py")),
                                   "state_dir": str(Path(directory) / "journal")})
            try:
                snapshot = await connector.snapshot()
                self.assertIn("set_mobile_delivery", snapshot["mobile_capabilities"]["operations"])
                self.assertEqual(snapshot["preferences"], {"mobile_delivery": "away"})
                self.assertEqual((await connector.execute(command("away_15")))["state"], "completed")
                self.assertEqual((await connector.snapshot())["preferences"], {"mobile_delivery": "away_15"})
                peer = connector.peer_context(str(uuid.uuid4()), connector.native)
                self.assertNotIn("set_mobile_delivery", (await peer.snapshot())["mobile_capabilities"]["operations"])
            finally:
                connector.journal.close()
