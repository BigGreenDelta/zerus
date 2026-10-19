"""Black-box: the phone's task exchange, which only the Rust connector offers
(docs/superpowers/specs/2026-10-18-task-replication-loro-design.md, "Phone").

A gateway runs `hgs task exchange --json` with the phone's payload on the
machine the route names, only where that machine's hgs offers it, and every
heartbeat project carries the tasks digest its hgs reports.
"""
import unittest
import uuid

from connector_harness import HELP_TEXT, IMPLEMENTATION, BlackBoxCase
from gateway_harness import OTHER, PEER, ROUTE, Fleet, GatewayCase, Peer, gateway_help, help_text

MARKER = "       hgs task exchange|digests --json                 the phone's task exchange (mobile)\n"
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

    async def test_without_the_marker_hgs_is_never_asked(self):
        self.hgs.prepend({"argv": ["swarm", "get"], "stdout": SWARM})
        process = await self.launch()
        await self.until(lambda: self.digests() == [None, None], 20, "a heartbeat with the projects", process)
        self.assertEqual(self.hgs.calls("task"), [])
        self.assertNotIn("task_exchange",
                         self.relay.heartbeats()[-1].body["snapshot"]["mobile_capabilities"]["operations"])


if __name__ == "__main__":
    unittest.main()
