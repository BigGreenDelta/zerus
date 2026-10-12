"""Phone Overmind requests are served by the gateway connector through synthetic native hgs only."""
import base64
import hashlib
import json
import os
from pathlib import Path
import tempfile
import time
import unittest
from unittest.mock import AsyncMock, patch
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
    if config["help"] is None:
        sys.exit(3)
    print(config["help"])
elif args == ["ls", "--json", "--local"]:
    print(json.dumps({"host": "Synthetic gateway", "sessions": []}))
elif args[:2] == ["overmind", "show"] and "journal" in config:
    journal, limit = config["journal"], payload.get("journal_limit", 0)
    before = min(payload.get("journal_before", len(journal)), len(journal))
    start = max(0, before - limit)
    print(json.dumps({"ok": True, "task": {"uuid": "task", "prompt": "Synthetic", "journal": journal[start:before],
                                           "journal_total": len(journal), "journal_from": start}}))
elif args[:2] == ["overmind", "board"] and "tasks" in config:
    tasks, start = config["tasks"], int(payload.get("after", "0"))
    end = start + payload.get("limit", len(tasks))
    print(json.dumps({"ok": True, "project": "example", "cursor": 7, "workflow": {"stages": []}, "tasks": tasks[start:end],
                      "next": str(end) if end < len(tasks) else None}))
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

    def test_all_fifty_decisions_fit_with_astral_titles_and_projects_are_trimmed(self):
        astral = "\U0001F600"  # Twelve escaped bytes each.
        decisions = {**DECISIONS, "decisions": [{**DECISION, "task": str(uuid.UUID(int=index)), "project": astral * 32,
                                                 "title": astral * 120, "signature": "f" * 128} for index in range(60)]}
        projects = {**PROJECTS, "projects": [{"project": "project-%05d" % index, "total": 1, "roles": ROLES} for index in range(2000)]}
        result = overmind.digest(projects, decisions)
        self.assertEqual(len(result["decisions"]), 50)
        for entry in result["decisions"]:
            self.assertLessEqual(len(json.dumps(entry["title"])) - 2, 600)
            self.assertEqual(entry["title"], astral * 50)
        self.assertLessEqual(len(json.dumps(result, separators=(",", ":"))), 64 * 1024)
        self.assertEqual(list(result["projects"]), [row["project"] for row in projects["projects"][:len(result["projects"])]])
        # A project id longer than hgs allows (128 UTF-8 bytes) cannot be a decision's project.
        self.assertEqual(overmind.digest(PROJECTS, {**DECISIONS, "decisions": [{**DECISION, "project": astral * 33}, DECISION]})["decisions"],
                         DIGEST["decisions"])

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

    def configure(self, help=HELP, pages=None, **answers):
        answers = {"projects": {"output": PROJECTS}, "decisions": {"output": DECISIONS}, **answers}
        (self.root / "config.json").write_text(json.dumps({"help": help, "answers": answers, **(pages or {})}))

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
                  ["overmind", "board", "--json", "--device", "Pixel 9"], {"project": "example", "since": 2, "limit": 200}),
                 (request("overmind_read", "task", {"task": "om-abcde"}), ["overmind", "show", "--json"], {"task": "om-abcde", "journal_limit": 100}),
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
        blob = self.root / "blobs" / sha[:2] / sha  # Blobs are content-addressed, as hgs stores them.
        blob.parent.mkdir(parents=True)
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
        # Ranges past the end, a changed size, links, relative paths, directories and a path that does not
        # name the requested image fail; they never read other bytes.
        link, directory, other = self.root / "links" / sha, self.root / "directories" / sha, self.root / "screen.png"
        link.parent.mkdir()
        link.symlink_to(blob)
        directory.mkdir(parents=True)
        other.write_bytes(data)
        for change, offset in (({}, len(data)), ({"bytes": len(data) + 1}, 0), ({"path": str(link)}, 0), ({"path": sha}, 0),
                               ({"path": str(directory)}, 0), ({"bytes": "1"}, 0), ({"path": str(other)}, 0),
                               ({"path": str(blob.parent / ("0" * 64))}, 0)):
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

    async def test_an_applied_write_answers_with_a_compact_card(self):
        await self.connector.snapshot()
        card = {"uuid": TASK, "ref": "om-abcde", "project": "example", "title": "Synthetic", "stage": "Build", "priority": 2, "rank": "U",
                "labels": [], "pinned": None, "assignee": None, "reviewer": None, "flags": [], "parent": None, "follows": [],
                "created_at": 1, "updated_at": 2, "read_only": False, "deleted": False, "journal_count": 2, "head": "event-2",
                "pinned_current": "codex/renamed"}
        # The full view escapes past the 1 MiB result bound (Cyrillic costs six bytes a character).
        full = {**card, "prompt": "ж" * 1000, "attachments": [], "journal": [{"marker": "[comment]", "text": "ж" * 180000}],
                "related": [], "resume_stage": "", "created_by": {"kind": "human"}}
        self.configure(comment={"output": {"ok": True, "delivery": "sent", "reason": "", "task": full}},
                       delete={"output": {"ok": True, "deleted": TASK}}, done={"exit": 1, "output": REFUSAL})
        write = request("overmind_write", "comment", {"task": "om-abcde", "text": "ж" * 10})
        first = await self.connector.execute(write)
        self.assertEqual(first, {"state": "completed", "result": {"ok": True, "delivery": "sent", "reason": "", "task": card}, "error": None})
        self.assertEqual(await self.connector.execute(write), first)
        self.assertEqual(len(self.calls("comment")), 1)
        self.assertEqual((await self.connector.execute(request("overmind_write", "delete", {"task": "om-abcde"})))["result"], {"ok": True, "deleted": TASK})
        self.assertEqual((await self.connector.execute(request("overmind_write", "done", {"task": "om-abcde"})))["result"], REFUSAL)

    async def test_task_journal_reads_are_bounded_pages(self):
        await self.connector.snapshot()
        journal = [{"marker": "[comment]", "text": "ж" * 2000 + str(index)} for index in range(300)]  # About 12 KB escaped each.
        self.configure(pages={"journal": journal})
        received, before = [], None
        for _ in range(20):
            if before == 0:
                break
            body = {"task": "om-abcde"} if before is None else {"task": "om-abcde", "journal_before": before}
            response = await self.connector.execute(request("overmind_read", "task", body))
            self.assertEqual(response["state"], "completed")
            self.assertLessEqual(len(json.dumps(response).encode()), 1024 * 1024 - 1024)
            task = response["result"]["task"]
            self.assertEqual(task["journal_total"], 300)
            received, before = task["journal"] + received, task["journal_from"]
        self.assertEqual(received, journal)
        self.assertEqual([call["payload"]["journal_limit"] for call in self.calls("show")][:2], [100, 50])  # Halved until it fits.
        await self.connector.execute(request("overmind_read", "task", {"task": "om-abcde", "journal_limit": 20}))
        self.assertEqual(self.calls("show")[-1]["payload"], {"task": "om-abcde", "journal_limit": 20})
        # One entry that cannot fit is a refusal-like answer, never an uncertain or failed receipt.
        self.configure(pages={"journal": [{"marker": "[comment]", "text": "ж" * 200000}]})
        response = await self.connector.execute(request("overmind_read", "task", {"task": "om-abcde"}))
        self.assertEqual((response["state"], response["result"]["ok"], response["result"]["error"]["code"], response["result"]["error"]["field"]),
                         ("completed", False, "too_large", "journal_limit"))
        self.assertEqual(self.calls("show")[-1]["payload"]["journal_limit"], 1)

    async def test_board_reads_are_bounded_pages(self):
        await self.connector.snapshot()
        tasks = [{"uuid": str(uuid.UUID(int=index)), "title": "ж" * 1000} for index in range(450)]  # About 6 KB escaped each.
        self.configure(pages={"tasks": tasks})
        received, after = [], None
        for _ in range(20):
            body = {"project": "example"} if after is None else {"project": "example", "after": after}
            response = await self.connector.execute(request("overmind_read", "board", body))
            self.assertEqual(response["state"], "completed")
            self.assertLessEqual(len(json.dumps(response).encode()), 1024 * 1024 - 1024)
            received, after = received + response["result"]["tasks"], response["result"]["next"]
            if after is None:
                break
        self.assertEqual(received, tasks)
        self.assertEqual([call["payload"]["limit"] for call in self.calls("board")][:2], [200, 100])
        self.configure(pages={"tasks": [{"uuid": TASK, "title": "ж" * 200000}]})
        result = (await self.connector.execute(request("overmind_read", "board", {"project": "example", "stage": "Queue"})))["result"]
        self.assertEqual((result["ok"], result["error"]["code"], result["error"]["field"]), (False, "too_large", "limit"))
        self.assertEqual(self.calls("board")[-1]["payload"], {"project": "example", "stage": "Queue", "limit": 1})

    async def test_a_failed_help_probe_keeps_overmind_until_a_probe_succeeds(self):
        await self.connector.snapshot()
        self.configure(help=None)  # The probe itself fails, for example by a timeout.
        self.connector.capabilities_next_poll = self.connector.overmind_next_poll = 0
        snapshot = await self.connector.snapshot()
        self.assertTrue(GATEWAY_OPERATIONS <= set(snapshot["mobile_capabilities"]["operations"]))
        self.assertEqual(snapshot["mobile_overmind"], DIGEST)
        self.assertEqual((await self.connector.execute(request()))["state"], "completed")
        self.configure(help="hgs ls")
        self.connector.capabilities_next_poll = 0
        snapshot = await self.connector.snapshot()
        self.assertFalse(GATEWAY_OPERATIONS & set(snapshot["mobile_capabilities"]["operations"]))
        self.assertNotIn("mobile_overmind", snapshot)

    async def test_digest_is_omitted_when_the_heartbeat_would_exceed_the_relay_limit(self):
        class Stop(Exception):
            pass
        sent = []

        async def post(client, path, body, presence):
            sent.append(json.loads(json.dumps(body)))
            raise Stop

        def gateway_snapshot(total):
            # A snapshot carrying the digest whose heartbeat body encodes to exactly `total` bytes.
            value = {"sessions": [{"name": "codex/example", "padding": ""}], "mobile_overmind": DIGEST}
            value["sessions"][0]["padding"] = "x" * (total - len(json.dumps({"snapshot": value, "presence": None}).encode()))
            return value
        for total, kept in ((1024 * 1024, True), (1024 * 1024 + 1, False)):
            with self.subTest(total=total), patch.object(self.connector, "snapshot", AsyncMock(return_value=gateway_snapshot(total))), \
                    patch.object(self.connector, "post_with_presence", post), self.assertRaises(Stop):
                await self.connector.heartbeats(None)
            self.assertEqual("mobile_overmind" in sent[-1]["snapshot"], kept)

    async def test_paged_reads_shrink_when_the_native_answer_exceeds_the_output_cap(self):
        self.connector.journal.close()
        self.connector = Connector(self.config, max_bytes=2 * 1024 * 1024)  # Native output cap of 2 MiB.
        await self.connector.snapshot()
        journal = [{"marker": "[comment]", "text": "ж" * 5000 + str(index)} for index in range(150)]  # About 30 KB escaped each.
        self.configure(pages={"journal": journal})
        response = await self.connector.execute(request("overmind_read", "task", {"task": "om-abcde"}))
        self.assertEqual(response["state"], "completed")
        self.assertEqual((response["result"]["task"]["journal"], response["result"]["task"]["journal_from"]), (journal[125:], 125))
        # 100 entries exceed the output cap, 50 the result bound; 25 fit.
        self.assertEqual([call["payload"]["journal_limit"] for call in self.calls("show")], [100, 50, 25])
        self.configure(pages={"tasks": [{"uuid": str(uuid.UUID(int=index)), "title": "ж" * 5000} for index in range(300)]})
        response = await self.connector.execute(request("overmind_read", "board", {"project": "example"}))
        self.assertEqual((response["state"], len(response["result"]["tasks"])), ("completed", 25))
        self.assertEqual([call["payload"]["limit"] for call in self.calls("board")], [200, 100, 50, 25])

    async def test_a_write_over_the_native_output_cap_still_answers_with_a_card(self):
        await self.connector.snapshot()
        full = {"uuid": TASK, "ref": "om-abcde", "title": "Synthetic", "journal_count": 1, "head": "event-1",
                "journal": [{"marker": "[comment]", "text": "ж" * 1500000}]}  # About 9 MB of native output.
        self.configure(comment={"output": {"ok": True, "task": full}})
        write = request("overmind_write", "comment", {"task": "om-abcde", "text": "Hello"})
        expected = {"state": "completed", "error": None,
                    "result": {"ok": True, "task": {"uuid": TASK, "ref": "om-abcde", "title": "Synthetic", "journal_count": 1, "head": "event-1"}}}
        self.assertEqual(await self.connector.execute(write), expected)
        self.assertEqual(await self.connector.execute(write), expected)
        self.assertEqual(len(self.calls("comment")), 1)

    async def test_null_windows_are_absent_and_a_phone_window_never_exceeds_the_default(self):
        await self.connector.snapshot()
        self.configure(pages={"journal": [{"marker": "[comment]", "text": "note"}], "tasks": [{"uuid": TASK}]})
        cases = [("task", {"task": "om-abcde", "journal_limit": None, "journal_before": None}, {"task": "om-abcde", "journal_limit": 100}),
                 ("task", {"task": "om-abcde", "journal_limit": 400, "journal_before": 3}, {"task": "om-abcde", "journal_limit": 100, "journal_before": 3}),
                 ("task", {"task": "om-abcde", "journal_limit": 20}, {"task": "om-abcde", "journal_limit": 20}),
                 ("board", {"project": "example", "limit": None, "after": None, "stage": None}, {"project": "example", "limit": 200}),
                 ("board", {"project": "example", "limit": 1000, "stage": "Queue"}, {"project": "example", "limit": 200, "stage": "Queue"})]
        for action, body, forwarded in cases:
            with self.subTest(action=action, body=body):
                self.assertEqual((await self.connector.execute(request("overmind_read", action, body)))["state"], "completed")
                self.assertEqual(self.calls("show" if action == "task" else "board")[-1]["payload"], forwarded)

    async def test_fifty_maximal_decisions_fit_one_result(self):
        # hgs clips titles to 120 and questions to 500 characters; stage names have at most 40,
        # session names 256 and project ids 128 UTF-8 bytes. Astral characters escape to 12 bytes.
        await self.connector.snapshot()
        astral = "\U0001F600"
        entry = {"task": TASK, "ref": "om-" + "a" * 32, "project": astral * 32, "title": astral * 120, "stage": astral * 40,
                 "question": astral * 500, "assignee": {"machine": MACHINE, "name": astral * 256}, "signature": "a" * 64}
        self.configure(decisions={"output": {"ok": True, "cursor": 2 ** 62, "decisions": [entry] * 50}})
        response = await self.connector.execute(request("overmind_read", "decisions"))
        self.assertEqual((response["state"], len(response["result"]["decisions"])), ("completed", 50))
        self.assertLessEqual(len(json.dumps(response).encode()), 1024 * 1024 - 4096)


if __name__ == "__main__":
    unittest.main()
