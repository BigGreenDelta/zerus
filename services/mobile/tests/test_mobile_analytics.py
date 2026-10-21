"""Content-free analytics transport contracts; all state and native calls are fixtures."""
import tempfile
import time
import unittest
import uuid
from unittest.mock import AsyncMock, patch

from zerus_mobile.connector import Connector
from zerus_mobile.fleet import route_id
from zerus_mobile.server import validate_request


MACHINE = "00000000-0000-4000-8000-000000000001"
MARKER = "hgs [@host] analytics --json"


def command():
    return {"request_id": str(uuid.uuid4()), "computer_id": str(uuid.uuid4()),
            "session": "", "operation": "analytics_read",
            "payload": {"schema": 1, "from": "2026-01-01", "through": "2026-03-31"}}


def summary(payload):
    return {"schema": 1, "machine_id": MACHINE, "source": "agent",
            "source_epoch": "00000000-0000-4000-8000-000000000002",
            "revision": 3, "generated_at": 1800000000.25,
            "range": {key: payload[key] for key in ("from", "through")},
            "collection": {"enabled": False}, "coverage": {"complete": False}, "buckets": []}


class AnalyticsValidationTests(unittest.TestCase):
    def test_exact_computer_scoped_ninety_day_read_is_accepted(self):
        validate_request(command())

    def test_calendar_bounds_and_mutation_fields_are_rejected(self):
        for edit in [lambda r: r.update(session="codex/fixture"),
                     lambda r: r["payload"].update(schema=True),
                     lambda r: r["payload"].update(schema=2),
                     lambda r: r["payload"].update(through="2026-04-01"),
                     lambda r: r["payload"].update(through="2025-12-31"),
                     lambda r: r["payload"].update(through="2026-02-29"),
                     lambda r: r["payload"].update(through="2026-3-31"),
                     lambda r: r["payload"].update(through="２０２６-03-31"),
                     lambda r: r["payload"].update(action="purge"),
                     lambda r: r["payload"].update(enabled=True),
                     lambda r: r["payload"].update(path="/fixture/private"),
                     lambda r: r["payload"].pop("schema")]:
            request = command()
            edit(request)
            with self.subTest(request=request), self.assertRaises(Exception):
                validate_request(request)

    def test_leap_calendar_day_is_accepted(self):
        request = command()
        request["payload"].update({"from": "2024-02-29", "through": "2024-02-29"})
        validate_request(request)


class AnalyticsConnectorTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="zerus-analytics-transport-")
        self.connector = Connector({"server_url": "https://relay.example.invalid",
                                    "node_token": "fixture-token", "state_dir": self.temp.name})
        self.connector.analytics_supported = True

    async def asyncTearDown(self):
        await self.connector.accounts.close()
        self.connector.journal.close()
        self.temp.cleanup()

    async def test_summary_is_unchanged_bounded_and_repeated_request_is_not_reexecuted(self):
        request = command()
        result = summary(request["payload"])
        with patch.object(self.connector, "native", AsyncMock(return_value=result)) as native:
            response = await self.connector.execute(request)
            self.assertEqual(response["state"], "completed", response)
            self.assertEqual(response["result"], result)
            self.assertEqual(await self.connector.execute(request), response)
            native.assert_awaited_once_with(["analytics", "--json"],
                {**request["payload"], "action": "summary"}, timeout=10,
                max_output_bytes=1024 * 1024)

    async def test_old_cli_and_remote_mutation_fields_never_dispatch(self):
        self.connector.analytics_supported = False
        with patch.object(self.connector, "native", AsyncMock()) as native:
            response = await self.connector.execute(command())
            self.assertEqual(response["state"], "failed")
            self.assertIn("analytics", response["error"])
            native.assert_not_awaited()
        self.connector.analytics_supported = True
        request = command()
        request["payload"]["action"] = "purge"
        with patch.object(self.connector, "native", AsyncMock()) as native:
            self.assertEqual((await self.connector.execute(request))["state"], "failed")
            native.assert_not_awaited()

    async def test_marker_must_name_exact_selected_abi(self):
        for help_text, enabled in [("analytics --json", False), (MARKER, True), ("", False)]:
            self.connector.capabilities_next_poll = 0
            with patch.object(self.connector, "native", AsyncMock(return_value=help_text)):
                await self.connector.probe_capabilities()
            self.assertEqual(self.connector.analytics_supported, enabled)

    async def test_response_range_schema_and_machine_are_checked(self):
        for edit in [lambda r: r.update(schema=True), lambda r: r.update(machine_id="invalid"),
                     lambda r: r.update(source="unknown"), lambda r: r.update(revision=-1),
                     lambda r: r["range"].update(through="2026-03-30")]:
            request = command()
            result = summary(request["payload"])
            edit(result)
            with patch.object(self.connector, "native", AsyncMock(return_value=result)):
                self.assertEqual((await self.connector.execute(request))["state"], "failed")

    async def test_verified_local_identity_and_oversized_reply_fail_closed(self):
        self.connector.fleet.local_id = "00000000-0000-4000-8000-000000000003"
        request = command()
        with patch.object(self.connector, "native", AsyncMock(return_value=summary(request["payload"]))):
            response = await self.connector.execute(request)
            self.assertEqual(response["state"], "failed")
            self.assertIsNone(response["result"])
        self.connector.fleet.local_id = MACHINE
        request = command()
        result = summary(request["payload"])
        result["buckets"] = [0] * (1024 * 1024)
        with patch.object(self.connector, "native", AsyncMock(return_value=result)):
            response = await self.connector.execute(request)
            self.assertEqual(response["state"], "failed")
            self.assertIsNone(response["result"])

    async def test_one_hop_keeps_peer_identity_capability_and_output_bound(self):
        identity = route_id("fixture-peer", MACHINE)
        fleet = self.connector.fleet
        fleet.routes[identity] = {"via": "fixture-peer", "machine_id": MACHINE, "online": True}
        peer = self.connector.peer_context(MACHINE, fleet.callback(identity, MACHINE))
        self.addAsyncCleanup(peer.accounts.close)
        request = command()
        request["gateway_route"] = {"schema": 1, "route_id": identity,
                                    "computer_id": request["computer_id"], "machine_id": MACHINE}
        result = summary(request["payload"])
        with patch.object(self.connector, "_native", AsyncMock(return_value=result)) as native:
            # Gateway support cannot enable a retained older peer.
            self.assertEqual((await peer._execute_claimed(request))["state"], "failed")
            native.assert_not_awaited()
            peer.analytics_supported = True
            self.assertEqual((await peer._execute_claimed(request))["result"], result)
            args, kwargs = native.call_args
            self.assertEqual(args[0], ["swarm", "mobile-peer", "--json"])
            self.assertEqual(args[1]["target_machine_id"], MACHINE)
            self.assertEqual(args[1]["argv"], ["analytics", "--json"])
            self.assertEqual(kwargs["max_output_bytes"], 1024 * 1024)
            fleet.routes[identity]["machine_id"] = "00000000-0000-4000-8000-000000000003"
            self.assertEqual((await peer._execute_claimed(request))["state"], "failed")
            self.assertEqual(native.await_count, 1)

    async def test_short_read_retention_preserves_pending_upload(self):
        delivered, pending = command(), command()
        for request in (delivered, pending):
            with patch.object(self.connector, "native", AsyncMock(return_value=summary(request["payload"]))):
                self.assertEqual((await self.connector.execute(request))["state"], "completed")
        journal = self.connector.journal
        journal.delivered(delivered["request_id"])
        journal.prune(now=time.time() + 121)
        rows = {row[0] for row in journal.db.execute("SELECT request_id FROM requests")}
        self.assertNotIn(delivered["request_id"], rows)
        self.assertIn(pending["request_id"], rows)
