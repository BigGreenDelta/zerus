"""Synthetic projection transport controls; no provider or actual hgs is used."""
import copy
import json
from pathlib import Path
import tempfile
import time
import unittest
import uuid
from unittest.mock import AsyncMock, patch

from relay_oracle import validate_request
from zerus_mobile.connector import Connector, ConnectorError, Journal, MUTATIONS
from zerus_mobile.context import READ_OPERATIONS, READ_SQL
from zerus_mobile.fleet import route_id
from zerus_mobile.history import (PROJECTION_BYTES, PROJECTION_RESULT_ERROR,
                                  validate_history_projection, validate_projection_result)

DATA = Path(__file__).resolve().parents[1] / "vectors" / "history_projection"
SESSION = "codex/example/mobile"
ID = "10000000-0000-4000-8000-000000000001"
MACHINE = "40000000-0000-4000-8000-000000000004"
MARKER = "hgs [@host] history-projection <session> --json retired and refreshed history rows"


def document(name):
    return json.loads((DATA / (name + ".json")).read_text())


def request(payload):
    return {"request_id": payload["request_id"], "computer_id": MACHINE,
            "session": SESSION, "operation": "history_projection", "payload": payload}


def changed(value, path, replacement=None, remove=False):
    target = value
    for key in path[:-1]:
        target = target[key]
    if remove:
        del target[path[-1]]
    else:
        target[path[-1]] = copy.deepcopy(replacement)


class HistoryProjectionValidation(unittest.TestCase):
    def test_the_28_request_corpus_has_no_number_coercion_or_unknown_fields(self):
        cases = document("requests")["cases"]
        self.assertEqual(len(cases), 28)
        for case in cases:
            with self.subTest(case["id"]):
                payload = copy.deepcopy(case.get("payload", case.get("base_payload")))
                if "raw_JSON_literal" in case:
                    payload[case["replace_field"]] = json.loads(case["raw_JSON_literal"])
                # The original envelope UUID stays fixed for the mismatch case.
                envelope = {**request(payload), "request_id": ID}
                if case["id"].startswith("valid-"):
                    validate_history_projection(payload, ID)
                    validate_request(envelope)
                else:
                    with self.assertRaises(ValueError):
                        validate_history_projection(payload, ID)
                    with self.assertRaises(ValueError):
                        validate_request(envelope)

    def test_native_identity_bounds_count_utf8_bytes_and_reject_c1_and_null(self):
        base = document("results")["payload"]
        for field, unit, count in (("expected_run_id", "é", 64),
                                   ("expected_conversation_id", "😀", 64),
                                   ("history_epoch", "é", 64), ("agent_id", "é", 80)):
            with self.subTest(field=field):
                validate_history_projection({**base, field: unit * count}, ID)
                for bad in (unit * (count + 1), "bad\u0085identity", "bad\x7fidentity", "\ud800"):
                    with self.assertRaises(ValueError):
                        validate_history_projection({**base, field: bad}, ID)
        for field in ("agent_id", "archive_id"):
            with self.assertRaises(ValueError):
                validate_history_projection({**base, field: None}, ID)
        for identity in ("00000000-0000-0000-0000-000000000000", "ABCDEFAB-0000-4000-8000-000000000001"):
            with self.assertRaises(ValueError):
                validate_history_projection({**base, "request_id": identity}, identity)

    def test_response_grammar_is_disjoint_and_preserves_the_whole_canonical_row(self):
        corpus = document("results")
        self.assertEqual(len(corpus["cases"]), 57)
        for case in corpus["cases"]:
            with self.subTest(case["id"]):
                payload = {**corpus["payload"], **case.get("payload_set", {})}
                result = copy.deepcopy(corpus["result"])
                for path, value in case["set"]:
                    changed(result, path, value)
                for path in case["remove"]:
                    changed(result, path, remove=True)
                original = copy.deepcopy(result)
                if case["accept"]:
                    validate_projection_result(result, payload, SESSION, ID)
                else:
                    with self.assertRaisesRegex(ValueError, PROJECTION_RESULT_ERROR):
                        validate_projection_result(result, payload, SESSION, ID)
                self.assertEqual(result, original, "validation must not edit or acknowledge the result")

    def test_entry_count_and_partial_position_are_not_silently_truncated(self):
        shared = json.loads((Path(__file__).resolve().parents[3] / "mobile/android/app/src/test/resources/history-projection-wire.json").read_text())
        for case in shared["cases"]:
            wire = json.dumps(case["value"], ensure_ascii=True, separators=(",", ":"))
            self.assertEqual(wire, case["ascii"])
            self.assertEqual(len(wire.encode("ascii")), case["bytes"])
        corpus = document("results")
        payload = {**corpus["payload"], "projection_until": 300}
        for count in (256, 257):
            result = {**corpus["result"], "projection_generation": 300, "complete": False,
                      "next_projection_after": count, "projection_changes": [
                          {"generation": n, "history_id": "a" * 64, "change": "retired",
                           "resolution": "resolved", "successor": None} for n in range(1, count + 1)]}
            original = copy.deepcopy(result)
            if count == 256:
                validate_projection_result(result, payload, SESSION, ID)
            else:
                with self.assertRaisesRegex(ValueError, PROJECTION_RESULT_ERROR):
                    validate_projection_result(result, payload, SESSION, ID)
            self.assertEqual(result, original)


class HistoryProjectionConnector(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.c = Connector({"server_url": "https://relay.example.invalid", "node_token": "fixture-projection",
                            "state_dir": self.temp.name})
        self.c.sessions = {SESSION}
        self.c.history_projection_supported = True

    async def asyncTearDown(self):
        for context in self.c.fleet.contexts.values():
            await context.accounts.close()
        await self.c.accounts.close()
        self.c.journal.close()
        self.temp.cleanup()

    def pair(self):
        corpus = document("results")
        payload, result = corpus["payload"], corpus["result"]
        identity = str(uuid.uuid4())
        payload["request_id"] = result["request_id"] = identity
        return request(payload), result

    async def test_one_fixed_range_call_keeps_metadata_and_replays_only_the_receipt(self):
        envelope, result = self.pair()
        original = copy.deepcopy(result)
        with patch.object(self.c, "native", AsyncMock(return_value=result)) as native:
            receipt = await self.c.execute(envelope)
            self.assertEqual(receipt, {"state": "completed", "result": original, "error": None})
            native.assert_awaited_once_with(["history-projection", SESSION, "--json"], envelope["payload"],
                                            timeout=2, max_output_bytes=1024 * 1024)
            self.assertEqual(await self.c.execute(envelope), receipt)
            self.assertEqual(native.await_count, 1)
        self.assertEqual(result, original)
        self.assertIn("history_projection", READ_OPERATIONS)
        self.assertNotIn("history_projection", MUTATIONS)
        self.assertNotIn("history_projection", READ_SQL)

    async def test_projection_marker_is_independent_and_failed_probe_revokes_it(self):
        for help_text, projection, history in ((MARKER, True, False),
                                               ("hgs history <session> --json", False, True),
                                               ("hgs [OPTIONS]", False, False)):
            self.c.capabilities_next_poll = 0
            with patch.object(self.c, "native", AsyncMock(return_value=help_text)):
                await self.c.probe_capabilities()
            self.assertEqual((self.c.history_projection_supported, self.c.history_supported), (projection, history))
        self.c.capabilities_next_poll = 0
        self.c.history_projection_supported = True
        with patch.object(self.c, "native", AsyncMock(side_effect=ConnectorError("native command failed"))):
            await self.c.probe_capabilities()
        self.assertFalse(self.c.history_projection_supported)

    async def test_snapshot_advertises_projection_only_for_the_own_marker(self):
        features = None
        for help_text, projection, history in ((MARKER, True, False),
                                               ("hgs history <session> --json", False, True)):
            async def native(argv, payload=None, **options):
                if argv == ["--help"]:
                    return help_text
                self.assertEqual(argv, ["ls", "--json", "--local"])
                return {"sessions": []}
            self.c.capabilities_next_poll = 0
            with patch.object(self.c, "native", AsyncMock(side_effect=native)), \
                    patch.object(self.c, "enrich_attention", AsyncMock()), \
                    patch.object(self.c, "enrich_projects", AsyncMock()), \
                    patch.object(self.c.accounts, "update", AsyncMock(return_value={})):
                caps = (await self.c.snapshot())["mobile_capabilities"]
            self.assertEqual("history_projection" in caps["operations"], projection)
            self.assertEqual("history" in caps["operations"], history)
            if features is None:
                features = caps["features"]
            self.assertEqual(caps["features"], features)

    async def test_old_hgs_refuses_before_an_archive_lookup_and_bad_payload_never_calls(self):
        envelope, result = self.pair()
        self.c.history_projection_supported = False
        self.c.archive_supported = True
        envelope["payload"]["archive_id"] = "20000000-0000-4000-8000-000000000002"
        with patch.object(self.c, "native", AsyncMock()) as native:
            receipt = await self.c.execute(envelope)
            self.assertEqual(receipt, {"state": "failed", "result": None,
                                       "error": "native history projection ABI is unavailable"})
            native.assert_not_awaited()
        self.c.history_projection_supported = True
        for field, bad in (("projection_after", True), ("projection_until", 1.0), ("before", "cursor")):
            envelope, _ = self.pair()
            envelope["payload"][field] = bad
            with patch.object(self.c, "native", AsyncMock(return_value=result)) as native:
                self.assertEqual((await self.c.execute(envelope))["state"], "failed")
                native.assert_not_awaited()

    async def test_malformed_and_oversize_results_fail_without_a_successful_prefix(self):
        for bad in (None, {"history_epoch": "other"}, {"complete": 1}, {"next_projection_after": 6},
                    {"projection_generation": True}, {"projection_changes": []}):
            envelope, result = self.pair()
            if bad is None:
                result = None
            else:
                result.update(bad)
                if bad == {"projection_changes": []}:
                    result["complete"] = False
            with patch.object(self.c, "native", AsyncMock(return_value=result)) as native:
                receipt = await self.c.execute(envelope)
                self.assertEqual(receipt, {"state": "failed", "result": None, "error": PROJECTION_RESULT_ERROR})
                self.assertEqual(native.await_count, 1)
        for excess in (0, 1):
            envelope, result = self.pair()
            result.update(projection_changes=[], padding="", unicode="😀é")
            base = len(json.dumps(result, ensure_ascii=True, separators=(",", ":")).encode("ascii"))
            result["padding"] = "x" * (PROJECTION_BYTES - base + excess)
            self.assertEqual(len(json.dumps(result, ensure_ascii=True, separators=(",", ":")).encode("ascii")),
                             PROJECTION_BYTES + excess)
            with patch.object(self.c, "native", AsyncMock(return_value=result)):
                receipt = await self.c.execute(envelope)
            if excess:
                self.assertEqual(receipt, {"state": "failed", "result": None, "error": PROJECTION_RESULT_ERROR})
            else:
                self.assertEqual(receipt, {"state": "completed", "result": result, "error": None})
        envelope, result = self.pair()
        result["padding"] = [0] * 400000
        self.assertLessEqual(len(json.dumps(result, separators=(",", ":")).encode()), PROJECTION_BYTES)
        with patch.object(self.c, "native", AsyncMock(return_value=result)):
            self.assertEqual(await self.c.execute(envelope),
                             {"state": "failed", "result": None, "error": "native result exceeds relay size limit"})

    async def test_peer_context_forwards_the_same_bounded_projection_and_rechecks_route(self):
        envelope, result = self.pair()
        fleet = self.c.fleet
        peer_machine = "50000000-0000-4000-8000-000000000005"
        identity = route_id("peer.example.test", peer_machine)
        fleet.supported = True
        fleet.local_id = MACHINE
        fleet.routes[identity] = {"route_id": identity, "machine_id": peer_machine,
                                  "via": "peer.example.test", "name": "Peer", "online": True}
        peer = self.c.peer_context(peer_machine, fleet.callback(identity, peer_machine))
        peer.sessions = {SESSION}
        peer.history_projection_supported = True
        peer.snapshot_ready.set()
        fleet.contexts[identity] = peer
        envelope["gateway_route"] = {"schema": 1, "route_id": identity,
                                     "computer_id": MACHINE, "machine_id": peer_machine}
        with patch.object(self.c, "_native", AsyncMock(return_value=result)) as native:
            self.assertEqual(await self.c.execute(envelope), {"state": "completed", "result": result, "error": None})
            native.assert_awaited_once_with(["swarm", "mobile-peer", "--json"],
                {"schema": 1, "via": "peer.example.test", "target_machine_id": peer_machine,
                 "argv": ["history-projection", SESSION, "--json"], "payload": envelope["payload"]},
                timeout=2, json_output=True, failure_json=False, max_output_bytes=1024 * 1024)
        envelope, _ = self.pair()
        envelope["gateway_route"] = {"schema": 1, "route_id": identity,
                                     "computer_id": MACHINE, "machine_id": peer_machine}
        fleet.routes[identity]["machine_id"] = MACHINE
        with patch.object(self.c, "_native", AsyncMock()) as native:
            self.assertEqual((await self.c.execute(envelope))["state"], "failed")
            native.assert_not_awaited()

    async def test_ordinary_history_keeps_generation_metadata_intact(self):
        envelope, _ = self.pair()
        envelope["operation"] = "history"
        payload = envelope["payload"]
        for key in ("history_epoch", "projection_after", "projection_until"):
            del payload[key]
        result = {"request_id": envelope["request_id"], "name": SESSION, "run_id": payload["expected_run_id"],
                  "conversation_id": payload["expected_conversation_id"], "events": [], "history_epoch": "epoch-one",
                  "head": {"complete": True}, "source_status": {"projection_upgrade": {"state": "complete", "generation": 9}}}
        self.c.history_supported = True
        with patch.object(self.c, "native", AsyncMock(return_value=result)) as native:
            self.assertEqual(await self.c.execute(envelope), {"state": "completed", "result": result, "error": None})
            self.assertEqual(native.await_count, 1)


class HistoryProjectionJournal(unittest.TestCase):
    def test_projection_retention_restart_and_read_cap_keep_mutation_identity(self):
        with tempfile.TemporaryDirectory() as root:
            journal = Journal(Path(root), "fixture-projection")
            now = time.time()
            try:
                for identity, operation in (("delivered", "history_projection"), ("undelivered", "history_projection"),
                                             ("running", "history_projection"), ("mutation", "send")):
                    self.assertTrue(journal.claim(identity, operation))
                    if identity != "running":
                        journal.finish(identity, {"state": "completed", "result": {"sentinel": "synthetic"}, "error": None})
                    if identity in ("delivered", "mutation"):
                        journal.delivered(identity)
                journal.db.execute("UPDATE requests SET delivered_at=? WHERE delivered=1", (now,))
                journal.db.commit()
                ids = lambda: {row[0] for row in journal.db.execute("SELECT request_id FROM requests")}
                journal.prune(now=now + 3599)
                self.assertEqual(ids(), {"delivered", "undelivered", "running", "mutation"})
                journal.prune(now=now + 3601)
                self.assertEqual(ids(), {"undelivered", "running", "mutation"})
                journal.prune(now=now + 8 * 86400)
                self.assertEqual(journal.db.execute("SELECT response_expired FROM requests WHERE request_id='mutation'").fetchone(), (1,))
            finally:
                journal.close()
            restarted = Journal(Path(root), "fixture-projection")
            try:
                self.assertEqual(restarted.replay("running")["state"], "failed")
                with restarted.db:
                    restarted.db.execute("DELETE FROM requests WHERE operation='history_projection'")
                    restarted.db.executemany("INSERT INTO requests(request_id,operation,state,created_at,updated_at) VALUES(?,'history_projection','running',?,?)",
                                            [(str(i), now, now) for i in range(5000)])
                with self.assertRaisesRegex(ConnectorError, "connector retained request limit reached"):
                    restarted.claim("beyond-read-cap", "history_projection")
                self.assertTrue(restarted.claim("independent-mutation-cap", "send"))
            finally:
                restarted.close()
