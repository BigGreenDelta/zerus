"""Black-box: the phone's task exchange, which only the Rust connector offers
(docs/superpowers/specs/2026-10-18-task-replication-loro-design.md, "Phone").

A gateway runs `hgs task exchange --json` with the phone's payload on the
machine the route names, only where that machine's hgs offers it, and every
heartbeat project carries the tasks digest its hgs reports.
"""
import json
import unittest
import uuid

from connector_harness import HELP_TEXT, IMPLEMENTATION, BlackBoxCase
from gateway_harness import OTHER, PEER, ROUTE, Fleet, GatewayCase, Peer, gateway_help, help_text

MARKER = "       hgs task exchange|digests --json                 the phone's task exchange (mobile)\n"
ACTIONS = "       hgs task mobile-send|mobile-cancel|mobile-done --json   the phone's task actions (mobile)\n"
SEND = ["task", "mobile-send", "--json"]
CANCEL = ["task", "mobile-cancel", "--json"]
EXCHANGE = ["task", "exchange", "--json"]
TASK = "0b9d6c55-2f5e-4c39-9a51-0d6f3c2a7e11"
REQUEST = {"schema": 1, "project": "project-1", "inventory": {"project": {}, "tasks": {}},
           "updates": [{"task": TASK, "bytes": "bG9ybw=="}]}
SWARM = {"schema": 1, "initialized": True, "node_id": "native-local", "swarm_id": "native-shared",
         "machines": [{"id": "native-local", "connection": "Synthetic computer", "local": True}],
         "organization": {"projects": [
             {"id": "project-1", "name": "orchard", "color": "#123456", "folders": [], "sessions": []},
             {"id": "project-2", "name": "meadow", "color": "#654321", "folders": [], "sessions": []}]}}


def answer(digest: str) -> dict:
    return {"schema": 1, "imported": 1, "refused": [], "updates": [], "more": False, "digest": digest}


class RustOnly:
    async def asyncSetUp(self):
        await super().asyncSetUp()
        if IMPLEMENTATION == "python":
            self.skipTest("only the Rust connector offers the task exchange")


class TaskExchangeTests(RustOnly, GatewayCase):
    def exchange(self) -> dict:
        return {"request_id": str(uuid.uuid4()), "computer_id": self.relay.computer_id,
                "operation": "task_exchange", "session": "", "payload": REQUEST}

    async def test_the_exchange_runs_on_the_machine_its_route_names(self):
        old = Peer(via="bb-old-peer", machine=OTHER, help=help_text())
        self.use(Fleet(Peer(help=help_text() + MARKER, answers=[(EXCHANGE, {"stdout": answer("ab" * 32)})]), old,
                       help=gateway_help() + MARKER, local=[(EXCHANGE, {"stdout": answer("cd" * 32)})]))
        process = await self.launch()
        await self.gateway_ready(process)
        self.assertIn("task_exchange", self.latest()["snapshot"]["mobile_capabilities"]["operations"])
        peer = self.relay.peer_heartbeats(ROUTE)[-1].body["snapshot"]["mobile_capabilities"]
        self.assertIn("task_exchange", peer["operations"])
        for route, digest, calls in ((self.route(), "ab" * 32, lambda: self.through(machine=PEER, argv=EXCHANGE)),
                                     (self.local_route(), "cd" * 32, lambda: self.guarded_calls(EXCHANGE))):
            result = await self.submit(self.exchange(), process, gateway_route=route)
            self.assertEqual(result, {"state": "completed", "result": answer(digest), "error": None})
            [call] = calls()
            self.assertEqual((call["envelope"]["argv"], call["envelope"]["payload"]), (EXCHANGE, REQUEST))
        # A peer whose hgs predates the exchange never runs it.
        result = await self.submit(self.exchange(), process, gateway_route=self.route(old))
        self.assertEqual((result["state"], result["error"]), ("failed", "native task exchange ABI is unavailable"))
        self.assertEqual(self.through(machine=OTHER, argv=EXCHANGE), [])
        self.assertEqual(self.hgs.calls("task"), [])


class TaskActionTests(RustOnly, GatewayCase):
    def action(self, operation: str, payload: dict) -> dict:
        return {"request_id": str(uuid.uuid4()), "computer_id": self.relay.computer_id,
                "operation": operation, "session": "", "payload": payload}

    async def test_sends_and_cancels_run_on_the_machine_their_route_names(self):
        sent = {"schema": 1, "ok": True, "task": TASK, "stage": "active",
                "session": {"machine": PEER, "session": "codex/api"}, "receipt": {"status": "submitted"}}
        unrecorded = "the prompt was sent, but the task could not record it: disk full"
        old = Peer(via="bb-old-peer", machine=OTHER, help=help_text() + MARKER)
        self.use(Fleet(Peer(help=help_text() + MARKER + ACTIONS, answers=[
            (SEND, {"stdout": sent, "times": 1}),
            (SEND, {"stdout": {"schema": 1, "ok": False, "error": unrecorded, "uncertain": True}, "exit": 1}),
            (CANCEL, {"stdout": {"schema": 1, "ok": False, "error": "only a task In progress can be cancelled"},
                      "exit": 1})]), old, help=gateway_help() + MARKER + ACTIONS))
        process = await self.launch()
        await self.gateway_ready(process)
        send = {"schema": 1, "project": "project-1", "task": TASK, "session": "codex/api"}
        result = await self.submit(self.action("task_send", send), process, gateway_route=self.route())
        self.assertEqual(result, {"state": "completed", "result": sent, "error": None})
        [call] = self.through(machine=PEER, argv=SEND)
        self.assertEqual(call["envelope"]["payload"], send)
        result = await self.submit(self.action("task_send", send), process, gateway_route=self.route())
        self.assertEqual((result["state"], result["error"]), ("uncertain", unrecorded))
        cancel = {"schema": 1, "project": "project-1", "task": TASK}
        result = await self.submit(self.action("task_cancel", cancel), process, gateway_route=self.route())
        self.assertEqual((result["state"], result["error"]), ("failed", "only a task In progress can be cancelled"))
        # A peer whose hgs predates the actions never runs them.
        result = await self.submit(self.action("task_send", send), process, gateway_route=self.route(old))
        self.assertEqual((result["state"], result["error"]), ("failed", "native task actions ABI is unavailable"))
        self.assertEqual(self.through(machine=OTHER, argv=SEND), [])


class TaskDigestTests(RustOnly, BlackBoxCase):
    def digests(self) -> list:
        beats = self.relay.heartbeats()
        if not beats:
            return []
        return [project.get("tasks_digest") for project in beats[-1].body["snapshot"]["mobile_projects"]["projects"]]

    async def test_heartbeat_projects_carry_their_tasks_digest(self):
        self.hgs.prepend({"argv": ["--help"], "stdout_text": HELP_TEXT + MARKER},
                         {"argv": ["swarm", "get"], "stdout": SWARM},
                         {"argv": ["task", "digests", "--json"], "stdout": {
                             "schema": 1, "projects": {"project-1": {"digest": "ab" * 32}}}})
        process = await self.launch()
        await self.until(lambda: self.digests() == ["ab" * 32, None], 20, "a heartbeat with the tasks digest",
                         process)
        self.assertIn("task_exchange", self.relay.heartbeats()[-1].body["snapshot"]["mobile_capabilities"]["operations"])

    async def test_a_refusal_from_hgs_reaches_the_phone_as_its_text(self):
        busy = "the project's tasks are busy; try again"
        self.hgs.prepend({"argv": ["--help"], "stdout_text": HELP_TEXT + MARKER},
                         {"argv": EXCHANGE, "stdout": {"schema": 1, "ok": False, "error": busy}, "exit": 1})
        process = await self.launch()
        request = {"request_id": str(uuid.uuid4()), "computer_id": self.relay.computer_id,
                   "operation": "task_exchange", "session": "", "payload": REQUEST}
        self.assertEqual(await self.submit(request, process), {"state": "failed", "result": None, "error": busy})
        [call] = self.hgs.calls(*EXCHANGE)
        self.assertEqual(json.loads(call["stdin"]), REQUEST)

    async def test_without_the_marker_hgs_is_never_asked(self):
        self.hgs.prepend({"argv": ["swarm", "get"], "stdout": SWARM})
        process = await self.launch()
        await self.until(lambda: self.digests() == [None, None], 20, "a heartbeat with the projects", process)
        self.assertEqual(self.hgs.calls("task"), [])
        self.assertNotIn("task_exchange",
                         self.relay.heartbeats()[-1].body["snapshot"]["mobile_capabilities"]["operations"])


if __name__ == "__main__":
    unittest.main()
