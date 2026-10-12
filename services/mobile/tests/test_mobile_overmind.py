"""Phone Overmind requests are served by the gateway connector through synthetic native hgs only."""
import base64
import hashlib
import json
import os
from pathlib import Path
import tempfile
import time
import unittest
import uuid

from zerus_mobile import overmind, postgres_schema
from zerus_mobile.connector import Connector, ConnectorError, Journal
from zerus_mobile.context import CAPABILITY_OPERATIONS, GATEWAY_OPERATIONS, OPERATIONS, READ_OPERATIONS, READ_SQL, SHORT_READS

MACHINE = "11111111-1111-4111-8111-111111111111"
TASK = "00000000-0000-4000-8000-0000000000a1"
HELP = "hgs overmind <command> --json (mobile ABI overmind-v1)   JSON in, JSON out; the mobile connector probes this line\n"
ROLES = {"backlog": 1, "ready": 0, "active": 1, "review": 0, "needs_human": 1, "done": 2, "parked": 0}
PROJECTS = {"ok": True, "cursor": 7, "projects": [{"project": "example", "total": 5, "roles": ROLES}]}
DECISION = {"task": TASK, "ref": "om-abcde", "project": "example", "title": "Choose a storage engine", "stage": "Your Call",
            "question": "Private synthetic question", "assignee": {"machine": MACHINE, "name": "codex/example"}, "signature": "a" * 64}
DECISIONS = {"ok": True, "cursor": 7, "decisions": [DECISION]}
DIGEST = {"schema": 1, "rev": 7, "projects": {"example": ROLES},
          "decisions": [{"task": TASK, "project": "example", "title": "Choose a storage engine", "signature": "a" * 64}]}
REFUSAL = {"ok": False, "error": {"code": "agent_busy", "message": "The session is working", "field": "confirm", "state": "missing"}}
# The exact hgs mobile peer allowlist (src/mobile_peers.rs OVERMIND_COMMANDS).
HGS_COMMANDS = {"projects", "board", "show", "decisions", "attachment", "create", "edit", "move", "reorder", "pin", "unpin",
                "label", "link", "comment", "send", "review-send", "rework-send", "answer", "resolve-delivery", "done", "reopen", "delete"}

FAKE_HGS = r'''#!/usr/bin/env python3
"""Synthetic hgs: records each call and answers from config.json; never runs hgs, tmux or an agent."""
import json, sys
from pathlib import Path
root = Path(__file__).parent
args, raw = sys.argv[1:], sys.stdin.read()
payload, local = (json.loads(raw) if raw else None), None
if args[:2] == ["swarm", "__mobile-peer-local"]:
    local, args, payload = payload["target_machine_id"], payload["argv"], payload["payload"]
with (root / "calls.jsonl").open("a") as log:
    log.write(json.dumps({"argv": args, "payload": payload, "local": local}) + "\n")
config = json.loads((root / "config.json").read_text())
if args == ["--help"]:
    print(config["help"])
elif args == ["ls", "--json", "--local"]:
    print(json.dumps({"host": "Synthetic gateway", "sessions": []}))
elif args[:1] == ["overmind"] and len(args) > 1 and args[1] in config["answers"]:
    answer = config["answers"][args[1]]
    print(json.dumps(answer["output"]))
    sys.exit(answer.get("exit", 0))
else:
    sys.exit(3)
'''


def request(operation="overmind_read", action="projects", body=None, **extra):
    request_id = str(uuid.uuid4())
    payload = {"action": action, "request": {} if body is None else body, **extra}
    if operation == "overmind_write":
        payload["request_id"] = request_id
    return {"request_id": request_id, "operation": operation, "session": "", "payload": payload}


class OvermindEnvelopeTests(unittest.TestCase):
    def test_operation_classes_actions_and_commands(self):
        self.assertEqual(GATEWAY_OPERATIONS, {"overmind_read", "overmind_write"})
        self.assertTrue(GATEWAY_OPERATIONS <= OPERATIONS and GATEWAY_OPERATIONS <= CAPABILITY_OPERATIONS)
        self.assertIn("overmind_read", READ_OPERATIONS)
        self.assertNotIn("overmind_write", READ_OPERATIONS)
        self.assertNotIn("overmind_read", READ_SQL)  # Its own short class; the persisted read index text is unchanged.
        self.assertEqual(SHORT_READS, ("attachment_read", "overmind_read"))
        self.assertIn("CREATE INDEX IF NOT EXISTS overmind_history ON requests(updated,id) WHERE operation='overmind_read' AND state IN ('completed','failed','uncertain');",
                      postgres_schema.SCHEMA)
        self.assertEqual(overmind.READ_ACTIONS, ("projects", "board", "task", "decisions", "attachment"))
        self.assertEqual(len(overmind.WRITE_ACTIONS), 17)
        self.assertEqual(set(overmind.COMMANDS.values()), HGS_COMMANDS)
        self.assertEqual(overmind.COMMANDS["task"], "show")
        self.assertTrue(all(command == action for action, command in overmind.COMMANDS.items() if action != "task"))

    def test_envelope_limits_and_identity(self):
        self.assertEqual(overmind.validate("overmind_read", {"action": "projects", "request": {}}, TASK), 64 * 1024)
        self.assertEqual(overmind.validate("overmind_write", {"request_id": TASK, "action": "done", "request": {"task": "om-abcde"}}, TASK),
                         29 * 1024 * 1024)
        for operation, payload in (("overmind_write", {"request_id": TASK.upper(), "action": "done", "request": {}}),
                                   ("overmind_read", {"action": "task", "request": {}, "device": "@phone"}),
                                   ("overmind_read", {"action": "attachment", "request": {"offset": 0, "length": 524289}})):
            with self.subTest(payload=payload), self.assertRaises(ValueError):
                overmind.validate(operation, payload, TASK)

    def test_device_rule_and_phone_label_normalization(self):
        for label in ("x", "Ü" * 64, "Pixel 9 @home - work", "Synthetic phone"):
            self.assertTrue(overmind.valid_device(label), label)
        for label in ("", "x" * 65, "phone\x7f", "phone\u0080", "phone\u009flabel", "tab\there", "-phone", "--json", "@peer", 7, None):
            self.assertFalse(overmind.valid_device(label), label)
        cases = {" Pixel 9 ": "Pixel 9", "\tPixel\x7f 9\u0085": "Pixel 9", "--json": "json", "@peer": "peer", "- @ -phone": "phone",
                 "x" * 70: "x" * 64, "Pixel 9 @home - work": "Pixel 9 @home - work", "Ü" * 64: "Ü" * 64,
                 "a" + " " * 63 + "b": "a", "phone\ud800": "phone", "-@ \x00": None, "": None, None: None, 7: None}
        for value, expected in cases.items():
            with self.subTest(value=value):
                label = overmind.device_label(value)
                self.assertEqual(label, expected)
                self.assertTrue(label is None or overmind.valid_device(label))


class OvermindDigestTests(unittest.TestCase):
    def test_digest_keeps_four_decision_fields_and_role_counts(self):
        self.assertEqual(overmind.digest(PROJECTS, DECISIONS), DIGEST)
        noisy = {**DECISIONS, "decisions": [DECISION, {**DECISION, "title": None}, "x", {**DECISION, "task": "b" * 8, "title": "t" * 300}]}
        result = overmind.digest(PROJECTS, noisy)
        self.assertEqual([entry["task"] for entry in result["decisions"]], [TASK, "b" * 8])
        self.assertEqual(len(result["decisions"][1]["title"]), 120)
        for projects, decisions in ((PROJECTS, {**DECISIONS, "ok": False}), (PROJECTS, {"ok": True, "decisions": {}}),
                                    ({"ok": True}, DECISIONS), (None, DECISIONS), (REFUSAL, DECISIONS)):
            with self.subTest(projects=projects, decisions=decisions), self.assertRaises(ValueError):
                overmind.digest(projects, decisions)

    def test_digest_is_bounded_with_decisions_first(self):
        decisions = {**DECISIONS, "decisions": [{**DECISION, "task": str(uuid.UUID(int=index)), "title": "Ü" * 120} for index in range(60)]}
        projects = {**PROJECTS, "projects": [{"project": "project-%05d-" % index + "p" * 100, "total": 1, "roles": ROLES} for index in range(2000)]}
        result = overmind.digest(projects, decisions)
        self.assertEqual(len(result["decisions"]), 50)
        self.assertLessEqual(len(json.dumps(result, separators=(",", ":"))), 64 * 1024)
        self.assertGreater(len(result["projects"]), 10)
        self.assertEqual(list(result["projects"]), [row["project"] for row in projects["projects"][:len(result["projects"])]])

    def test_decision_changes_need_a_digest_on_both_sides(self):
        snapshot = lambda *entries: {"sessions": [], "mobile_overmind": {"decisions": [{"task": task, "signature": signature} for task, signature in entries]}}
        other = "00000000-0000-4000-8000-0000000000a2"
        self.assertEqual(overmind.decision_changes(None, snapshot((TASK, "a"))), [])
        self.assertEqual(overmind.decision_changes({"sessions": []}, snapshot((TASK, "a"))), [])
        self.assertEqual(overmind.decision_changes(snapshot((TASK, "a")), {"sessions": []}), [])
        self.assertEqual(overmind.decision_changes(snapshot(), snapshot((TASK, "a"), (other, "b"))),
                         [("overmind:" + TASK, "decision"), ("overmind:" + other, "decision")])
        self.assertEqual(overmind.decision_changes(snapshot((TASK, "a"), (other, "b")), snapshot((other, "c"))), [("overmind:" + other, "decision")])


class OvermindJournalTests(unittest.TestCase):
    def test_reads_leave_the_journal_quickly_and_writes_stay_tombstones(self):
        with tempfile.TemporaryDirectory() as directory:
            journal = Journal(Path(directory) / "journal", "example-binding")
            try:
                ids = {}
                for operation in ("overmind_read", "overmind_write"):
                    ids[operation] = str(uuid.uuid4())
                    self.assertTrue(journal.claim(ids[operation], operation, "a" * 64))
                    journal.finish(ids[operation], {"state": "completed", "result": {"ok": True}, "error": None})
                    journal.delivered(ids[operation])
                journal.prune(now=time.time() + 60)
                self.assertEqual(journal.db.execute("SELECT count(*) FROM requests").fetchone()[0], 2)
                journal.prune(now=time.time() + 121)
                self.assertEqual([row[0] for row in journal.db.execute("SELECT operation FROM requests")], ["overmind_write"])
                self.assertFalse(journal.claim(ids["overmind_write"], "overmind_write", "a" * 64))
            finally:
                journal.close()


class OvermindConnectorTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="zerus-overmind-test-")
        self.root = Path(self.temp.name)
        (self.root / "hgs").write_text(FAKE_HGS)
        (self.root / "hgs").chmod(0o700)
        self.configure()
        self.config = {"server_url": "https://relay.example.invalid", "node_token": "fixture-token",
                       "hgs_path": str(self.root / "hgs"), "state_dir": str(self.root / "journal")}
        self.connector = Connector(self.config)

    async def asyncTearDown(self):
        self.connector.journal.close()
        self.temp.cleanup()

    def configure(self, help=HELP, **answers):
        answers = {"projects": {"output": PROJECTS}, "decisions": {"output": DECISIONS}, **answers}
        (self.root / "config.json").write_text(json.dumps({"help": help, "answers": answers}))

    def calls(self, command=None):
        path = self.root / "calls.jsonl"
        rows = [json.loads(line) for line in path.read_text().splitlines()] if path.exists() else []
        return [row for row in rows if command is None or row["argv"][:2] == ["overmind", command]]

    async def test_operations_and_digest_only_on_a_supporting_gateway(self):
        snapshot = await self.connector.snapshot()
        self.assertTrue(GATEWAY_OPERATIONS <= set(snapshot["mobile_capabilities"]["operations"]))
        self.assertEqual(snapshot["mobile_overmind"], DIGEST)
        self.assertNotIn("Private synthetic question", json.dumps(snapshot))
        self.assertEqual([row["payload"] for row in self.calls("projects") + self.calls("decisions")], [{}, {}])
        reads = len(self.calls())
        peer = self.connector.peer_context(MACHINE, self.connector.native)
        try:
            peer_snapshot = await peer.snapshot()
        finally:
            await peer.accounts.close()
        self.assertTrue(peer.overmind_supported)
        self.assertFalse(GATEWAY_OPERATIONS & set(peer_snapshot["mobile_capabilities"]["operations"]))
        self.assertNotIn("mobile_overmind", peer_snapshot)
        self.assertEqual((len(self.calls("projects")), len(self.calls("decisions"))), (1, 1))
        self.assertGreater(len(self.calls()), reads)
        # An hgs without the mobile Overmind ABI hides the operations and withdraws the digest.
        self.configure(help="hgs [@host] overmind <command> [--json]")
        self.connector.capabilities_next_poll = 0
        snapshot = await self.connector.snapshot()
        self.assertFalse(GATEWAY_OPERATIONS & set(snapshot["mobile_capabilities"]["operations"]))
        self.assertNotIn("mobile_overmind", snapshot)
        self.assertEqual((await self.connector.execute(request()))["state"], "failed")
        self.assertEqual(len(self.calls("projects")), 1)

    async def test_digest_is_cached_and_a_failed_refresh_keeps_the_last_one(self):
        first = await self.connector.snapshot()
        self.assertGreater(self.connector.overmind_next_poll - time.monotonic(), 14)
        self.assertEqual((await self.connector.snapshot())["mobile_overmind"], first["mobile_overmind"])
        self.assertEqual(len(self.calls("decisions")), 1)
        for failure in ({"exit": 1, "output": REFUSAL}, {"output": {"ok": True, "cursor": 8, "decisions": None}}, {"exit": 9, "output": "x"}):
            self.configure(decisions=failure)
            self.connector.overmind_next_poll = 0
            self.assertEqual((await self.connector.snapshot())["mobile_overmind"], DIGEST)
        changed = {**DECISIONS, "cursor": 8, "decisions": [{**DECISION, "signature": "b" * 64}]}
        self.configure(decisions={"output": changed})
        self.connector.overmind_next_poll = 0
        self.assertEqual((await self.connector.snapshot())["mobile_overmind"]["decisions"][0]["signature"], "b" * 64)
        self.assertEqual(len(self.calls("decisions")), 5)
        # Before any digest is known, a gateway snapshot carries none rather than an empty list.
        self.connector.journal.close()
        self.configure(decisions={"exit": 1, "output": REFUSAL})
        self.connector = Connector({**self.config, "state_dir": str(self.root / "other-journal")})
        snapshot = await self.connector.snapshot()
        self.assertIn("overmind_read", snapshot["mobile_capabilities"]["operations"])
        self.assertNotIn("mobile_overmind", snapshot)

    async def test_overmind_write_replays_by_request_id(self):
        await self.connector.snapshot()
        write = request("overmind_write", "create", {"project": "example", "prompt": "Hello"}, device="Pixel 9")
        created = {"ok": True, "task": {"uuid": write["request_id"], "ref": "om-abcde", "journal_count": 1}}
        self.configure(create={"output": created})
        first = await self.connector.execute(write)
        self.assertEqual(first, {"state": "completed", "result": created, "error": None})
        self.assertEqual(await self.connector.execute(write), first)
        calls = self.calls("create")
        self.assertEqual(len(calls), 1)
        self.assertEqual(calls[0]["argv"], ["overmind", "create", "--json", "--device", "Pixel 9"])
        # The envelope identity is the Overmind request_id, which also names the created task.
        self.assertEqual(calls[0]["payload"], {"project": "example", "prompt": "Hello", "request_id": write["request_id"]})
        other = json.loads(json.dumps(write))
        other["payload"]["request"]["prompt"] = "Other"
        with self.assertRaises(ConnectorError):
            await self.connector.execute(other)
        # A restarted connector answers from its durable journal, never from a second native write.
        self.connector.journal.close()
        self.connector = Connector(self.config)
        self.assertEqual(await self.connector.execute(write), first)
        self.assertEqual(len(self.calls("create")), 1)
        # A request naming another Overmind identity fails before hgs.
        mismatched = request("overmind_write", "create", {"project": "example", "prompt": "Hello", "request_id": str(uuid.uuid4())})
        await self.connector.snapshot()
        self.assertEqual((await self.connector.execute(mismatched))["state"], "failed")
        self.assertEqual(len(self.calls("create")), 1)

    async def test_refusals_pass_through_as_results(self):
        await self.connector.snapshot()
        refused_board = {"ok": False, "error": {"code": "invalid_field", "message": "unknown project", "field": "project", "state": "invalid"}}
        self.configure(send={"exit": 1, "output": REFUSAL}, board={"exit": 1, "output": refused_board})
        send = request("overmind_write", "send", {"task": "om-abcde", "session": {"name": "codex/example"}})
        self.assertEqual(await self.connector.execute(send), {"state": "completed", "result": REFUSAL, "error": None})
        board = request("overmind_read", "board", {"project": "missing"})
        self.assertEqual(await self.connector.execute(board), {"state": "completed", "result": refused_board, "error": None})
        # Any other native failure is uncertain for a write and failed for a read.
        for answer in ({"exit": 2, "output": "x"}, {"output": {"task": {}}}, {"exit": 1, "output": {"ok": True}}):
            self.configure(send=answer, board=answer)
            with self.subTest(answer=answer):
                write = request("overmind_write", "send", {"task": "om-abcde", "session": {"name": "codex/example"}})
                self.assertEqual((await self.connector.execute(write))["state"], "uncertain")
                self.assertEqual((await self.connector.execute(request("overmind_read", "board", {"project": "x"})))["state"], "failed")

    async def test_actions_map_to_native_commands_with_a_normalized_device(self):
        await self.connector.snapshot()
        answer = {"output": {"ok": True, "cursor": 3}}
        self.configure(board=answer, show=answer, decisions=answer, done=answer, **{"review-send": answer})
        review = request("overmind_write", "review-send", {"task": "om-abcde", "session": {"name": "codex/review"}})
        cases = [(request("overmind_read", "board", {"project": "example", "since": 2}, device=" Pixel 9 "),
                  ["overmind", "board", "--json", "--device", "Pixel 9"], {"project": "example", "since": 2}),
                 (request("overmind_read", "task", {"task": "om-abcde"}), ["overmind", "show", "--json"], {"task": "om-abcde"}),
                 (request("overmind_read", "decisions", {}, device="Pixel\u0085"), ["overmind", "decisions", "--json", "--device", "Pixel"], {}),
                 (review, ["overmind", "review-send", "--json"], {**review["payload"]["request"], "request_id": review["request_id"]}),
                 # The informational label never blocks an action: a label hgs would refuse is normalized or omitted.
                 (request(device="@peer"), ["overmind", "projects", "--json", "--device", "peer"], {}),
                 (request(device="-@ \x00"), ["overmind", "projects", "--json"], {}),
                 (request(device=7), ["overmind", "projects", "--json"], {})]
        for value, argv, payload in cases:
            with self.subTest(action=value["payload"]["action"], device=value["payload"].get("device")):
                self.assertEqual((await self.connector.execute(value))["state"], "completed")
                self.assertEqual({key: self.calls()[-1][key] for key in ("argv", "payload")}, {"argv": argv, "payload": payload})
        before = len(self.calls())
        invalid = [request("overmind_read", "create"), request("overmind_write", "board"), request("overmind_read", "advance"),
                   {**request(), "session": "codex/example"}, request("overmind_read", "attachment", {"offset": 0}),
                   {**request(), "payload": {"action": "projects", "request": {"text": "x" * 65536}}}]
        for value in invalid:
            with self.subTest(payload=str(value["payload"])[:120]):
                self.assertEqual((await self.connector.execute(value))["state"], "failed")
        self.assertEqual(len(self.calls()), before)

    async def test_gateway_requests_never_run_through_a_peer_route(self):
        await self.connector.snapshot()
        self.configure(done={"output": {"ok": True, "task": {"ref": "om-abcde"}}})
        fleet = self.connector.fleet
        fleet.supported, fleet.local_id = True, MACHINE
        route, remote = str(uuid.uuid4()), str(uuid.uuid4())
        fleet.routes[route] = {"route_id": route, "machine_id": remote, "via": "peer.example.invalid", "name": "Peer", "online": True}
        fleet.contexts[route] = peer = self.connector.peer_context(remote, self.connector.native)
        peer.overmind_supported = True
        peer_route = {"schema": 1, "route_id": route, "computer_id": str(uuid.uuid4()), "machine_id": remote}
        before = len(self.calls())
        for value in (request(), request("overmind_write", "done", {"task": "om-abcde"})):
            response = await self.connector.execute({**value, "gateway_route": peer_route})
            self.assertEqual((response["state"], response["result"]), ("failed", None))
            self.assertEqual(await self.connector.execute({**value, "gateway_route": peer_route}), response)
        self.assertEqual(len(self.calls()), before)
        # A peer machine context never serves Overmind, even when handed a request directly.
        direct = request()
        self.assertTrue(self.connector.journal.claim(direct["request_id"], "overmind_read"))
        self.assertEqual((await peer._execute_claimed(direct))["state"], "failed")
        self.assertEqual(len(self.calls()), before)
        await peer.accounts.close()
        # The gateway's own route runs locally through the guarded local path.
        local_route = {"schema": 1, "route_id": None, "computer_id": str(uuid.uuid4()), "machine_id": MACHINE}
        write = request("overmind_write", "done", {"task": "om-abcde"})
        self.assertEqual((await self.connector.execute({**write, "gateway_route": local_route}))["state"], "completed")
        self.assertEqual({key: self.calls()[-1][key] for key in ("argv", "local")}, {"argv": ["overmind", "done", "--json"], "local": MACHINE})

    async def test_attachment_ranges_come_from_the_native_path(self):
        await self.connector.snapshot()
        data = bytes(range(256)) * 4097  # Three ranges, the last one partial.
        sha = hashlib.sha256(data).hexdigest()
        blob = self.root / "blob.png"
        blob.write_bytes(data)
        answer = {"ok": True, "path": str(blob), "name": "screen.png", "mime": "image/png", "bytes": len(data)}
        self.configure(attachment={"output": answer})

        def read(offset, length=524288, **change):
            return request("overmind_read", "attachment", {"task": "om-abcde", "sha256": sha, "offset": offset, "length": length, **change})
        received = b""
        while len(received) < len(data):
            response = await self.connector.execute(read(len(received)))
            self.assertEqual(response["state"], "completed", response)
            result = response["result"]
            chunk = base64.b64decode(result["data_base64"])
            self.assertEqual({key: value for key, value in result.items() if key != "data_base64"},
                             {"ok": True, "task": "om-abcde", "sha256": sha, "name": "screen.png", "mime": "image/png",
                              "size": len(data), "offset": len(received), "length": len(chunk)})
            received += chunk
        self.assertEqual(received, data)
        self.assertEqual([call["payload"] for call in self.calls("attachment")], [{"task": "om-abcde", "sha256": sha}] * 3)
        self.assertEqual((await self.connector.execute(read(len(data) - 10, 64)))["result"]["length"], 10)
        # Ranges past the end, a changed size, links and relative paths fail; they never read other bytes.
        link = self.root / "link.png"
        link.symlink_to(blob)
        for change, offset in (({}, len(data)), ({"bytes": len(data) + 1}, 0), ({"path": str(link)}, 0), ({"path": "blob.png"}, 0),
                               ({"path": str(self.root)}, 0), ({"bytes": "1"}, 0)):
            self.configure(attachment={"output": {**answer, **change}})
            with self.subTest(change=change, offset=offset):
                response = await self.connector.execute(read(offset))
                self.assertEqual((response["state"], response["result"]), ("failed", None))
        refusal = {"ok": False, "error": {"code": "invalid_field", "message": "this task has no such image", "field": "sha256", "state": "invalid"}}
        self.configure(attachment={"exit": 1, "output": refusal})
        self.assertEqual(await self.connector.execute(read(0)), {"state": "completed", "result": refusal, "error": None})
        before = len(self.calls())
        self.assertEqual((await self.connector.execute(read(0, 524289)))["state"], "failed")
        self.assertEqual(len(self.calls()), before)


if __name__ == "__main__":
    unittest.main()
