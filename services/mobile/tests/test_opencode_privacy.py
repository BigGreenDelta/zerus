"""OpenCode diagnostic boundaries, using memory native replies and private SQLite."""
import copy
import json
import tempfile
import unittest
import uuid

from zerus_mobile import opencode_privacy as privacy
from zerus_mobile.connector import Connector
from zerus_mobile.fleet import route_id

PRIVATE = "https://provider.example.invalid/v1?key=fixture-YO85-DIAGNOSTIC\x1b[31m\x07"
SESSION = "custom/example/session"
LOCAL = "11111111-1111-4111-8111-111111111111"
PEER = "22222222-2222-4222-8222-222222222222"
COMPUTER = "33333333-3333-4333-8333-333333333333"


class ProjectionTests(unittest.TestCase):
    def test_diagnostic_fields_are_fixed_and_conversation_content_is_unchanged(self):
        events = [{"type": "AgentMessage", "detail": "OpenCode HTTP 400: keep this literal reply", "at": 1}]
        cards = [{"question_id": "q1", "title": "Keep this question", "options": ["https://user.example.invalid"]}]
        for diagnostic in (PRIVATE, None, {"message": PRIVATE}):
            value = dict(cmd="opencode", name=SESSION, inspection_reason=diagnostic,
                         activity_detail=PRIVATE, inspection_unavailable=True,
                         events=events, pending_questions=cards, phase="unknown",
                         run_id="run", conversation_id="ses_A")
            privacy.row(value)
            self.assertEqual(value["inspection_reason"], privacy.INSPECTION)
            self.assertEqual(value["activity_detail"], privacy.INSPECTION)
            self.assertNotIn("fixture-YO85-DIAGNOSTIC", json.dumps(value))
            self.assertEqual(value["events"], events)
            self.assertEqual(value["pending_questions"], cards)
            self.assertEqual((value["phase"], value["run_id"], value["conversation_id"]), ("unknown", "run", "ses_A"))

    def test_replies_and_other_providers_are_not_classified_by_text_or_name(self):
        for original in (
                dict(cmd="opencode", activity_detail=PRIVATE, events=[{"detail": PRIVATE}]),
                dict(cmd="codex", name="opencode/example/session", inspection_reason=PRIVATE, activity_detail=PRIVATE),
                dict(name="opencode/example/session", inspection_reason=PRIVATE),
                dict(cmd=["opencode"], inspection_reason=PRIVATE)):
            value = copy.deepcopy(original)
            privacy.row(value)
            self.assertEqual(value, original)

    def test_action_error_uses_owned_provider_identity_and_keeps_native_identity(self):
        for status, expected in (("failed", privacy.ACTION_FAILED), ("uncertain", privacy.ACTION_UNCERTAIN)):
            for known in (False, True):
                value = dict(request_id="request", name=SESSION, run_id="run", conversation_id="ses_A", status=status, error=PRIVATE)
                if not known:
                    value["error_source"] = "opencode"
                privacy.action(value, known)
                self.assertEqual(value["error"], expected)
                self.assertEqual((value["request_id"], value["name"], value["run_id"], value["conversation_id"], value["status"]),
                                 ("request", SESSION, "run", "ses_A", status))
                self.assertNotIn("fixture-YO85-DIAGNOSTIC", json.dumps(value))

    def test_absent_or_malformed_source_does_not_reclassify_other_providers(self):
        for source in (None, "codex", ["opencode"], {"name": "opencode"}):
            original = dict(name="opencode/example/session", status="failed", error=PRIVATE, error_source=source)
            value = copy.deepcopy(original)
            privacy.action(value, False)
            self.assertEqual(value, original)

    def test_success_and_missing_errors_are_unchanged(self):
        for original in (dict(status="completed", result_target=dict(name="new", run_id="run", conversation_id="ses_A")),
                         dict(status="failed", error=None), dict(status="uncertain")):
            value = copy.deepcopy(original)
            privacy.action(value, True)
            self.assertEqual(value, original)


class ConnectorBoundaryTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="zerus-opencode-privacy-")
        self.connector = Connector(dict(server_url="https://relay.example.invalid", node_token="fixture", state_dir=self.temp.name))
        self.calls = []
        self.status = "failed"
        self.origin = False  # An older hgs without the new owned source field.
        self.providers = {LOCAL: "opencode", PEER: "opencode"}
        self.connector._native = self.native
        self.identity = route_id("fixture-peer", PEER)
        fleet = self.connector.fleet
        fleet.local_id = LOCAL
        fleet.routes[self.identity] = dict(route_id=self.identity, machine_id=PEER, via="fixture-peer", name="Peer", online=True)
        self.peer = self.connector.peer_context(PEER, fleet.callback(self.identity, PEER))
        fleet.contexts[self.identity] = self.peer
        await self.connector.snapshot()
        await self.peer.snapshot()

    async def asyncTearDown(self):
        await self.connector.accounts.close()
        await self.connector.fleet.close()
        self.connector.journal.close()
        self.temp.cleanup()

    def row(self, machine):
        return dict(name=SESSION, cmd=self.providers[machine], run_id="run", conversation_id="ses_A",
                    activity="unknown", phase="unknown", inspection_unavailable=True,
                    inspection_reason=PRIVATE, activity_detail=PRIVATE,
                    events=[dict(type="AgentMessage", detail="literal reply", at=1)],
                    pending_questions=[dict(question_id="q1", title="literal question")])

    async def native(self, argv, payload=None, **options):
        self.calls.append((copy.deepcopy(argv), copy.deepcopy(payload)))
        machine = LOCAL
        if argv[:2] in (["swarm", "mobile-peer"], ["swarm", "__mobile-peer-local"]):
            machine, argv, payload = payload["target_machine_id"], payload["argv"], payload["payload"]
        if argv == ["--help"]:
            return "hgs session-action <session> --json\nhgs mobile-peers --json\nhgs mobile-peer --json"
        if argv[:3] == ["ls", "--json", "--local"]:
            return dict(sessions=[self.row(machine)])
        if argv[:2] == ["inspect", SESSION]:
            return self.row(machine)
        if argv[:2] == ["session-action", SESSION]:
            value = dict(request_id=payload["request_id"], name=SESSION, run_id="run", conversation_id="ses_A", status=self.status)
            if self.status != "completed":
                value["error"] = PRIVATE
                if self.origin:
                    value["error_source"] = "opencode"
            else:
                value["result_target"] = dict(name="custom/example/new", run_id="run", conversation_id="ses_A")
            return value
        if argv == ["account", "ls"]:
            return dict(profiles=[])
        if argv == ["swarm", "get"]:
            return dict(projects=[])
        self.fail(f"Unexpected native call: {argv!r}")

    def request(self, operation, peer):
        identity = str(uuid.uuid4())
        payload = {} if operation == "inspect" else dict(request_id=identity, expected_run_id="run", expected_conversation_id="ses_A", new_name="custom/example/new")
        return dict(request_id=identity, operation=operation, session=SESSION, payload=payload,
                    gateway_route=dict(schema=1, route_id=self.identity if peer else None, computer_id=COMPUTER, machine_id=PEER if peer else LOCAL))

    async def test_local_and_peer_snapshots_and_inspections_do_not_export_diagnostics(self):
        for peer, context in ((False, self.connector), (True, self.peer)):
            snapshot = await context.snapshot()
            row = snapshot["sessions"][0]
            self.assertEqual(row["inspection_reason"], privacy.INSPECTION)
            self.assertEqual(row["activity_detail"], privacy.INSPECTION)
            self.assertEqual(row["events"], self.row(LOCAL)["events"])
            self.assertEqual(row["pending_questions"], self.row(LOCAL)["pending_questions"])
            self.assertNotIn("fixture-YO85-DIAGNOSTIC", json.dumps(snapshot))
            request = self.request("inspect", peer)
            first = await self.connector.execute(request)
            self.assertEqual(first["state"], "completed")
            self.assertEqual(first["result"]["inspection_reason"], privacy.INSPECTION)
            self.assertEqual(first["result"]["pending_questions"], row["pending_questions"])
            count = len(self.calls)
            self.assertEqual(await self.connector.execute(request), first)
            self.assertEqual(len(self.calls), count)
        stored = [body for _, body in self.connector.journal.pending()]
        self.assertNotIn("fixture-YO85-DIAGNOSTIC", json.dumps(stored))
        self.assertTrue(any(argv == ["swarm", "mobile-peer", "--json"] for argv, _ in self.calls))
        self.assertTrue(any(argv == ["swarm", "__mobile-peer-local", "--json"] for argv, _ in self.calls))

    async def test_local_peer_failed_uncertain_and_successful_actions_keep_exact_receipts(self):
        for peer in (False, True):
            for self.status, expected in (("failed", privacy.ACTION_FAILED), ("uncertain", privacy.ACTION_UNCERTAIN), ("completed", None)):
                request = self.request("rename", peer)
                first = await self.connector.execute(request)
                self.assertEqual(first["state"], self.status)
                self.assertEqual(first["error"], expected)
                if expected is not None:
                    self.assertEqual(first["result"]["error"], expected)
                else:
                    self.assertEqual(first["result"]["result_target"]["name"], "custom/example/new")
                count = len(self.calls)
                self.assertEqual(await self.connector.execute(request), first)
                self.assertEqual(len(self.calls), count)
                # Restore admission after the successful synthetic rename.
                await (self.peer if peer else self.connector).snapshot()
        self.assertNotIn("fixture-YO85-DIAGNOSTIC", json.dumps(self.connector.journal.pending()))

    async def test_same_name_on_a_different_machine_never_inherits_provider_classification(self):
        self.providers[LOCAL] = "codex"
        await self.connector.snapshot()
        self.assertNotIn(SESSION, self.connector.opencode_sessions)
        self.assertIn(SESSION, self.peer.opencode_sessions)
        response = await self.connector.execute(self.request("rename", False))
        self.assertEqual(response["error"], PRIVATE)
        response = await self.connector.execute(self.request("rename", True))
        self.assertEqual(response["error"], privacy.ACTION_FAILED)
