"""End to end: a phone exchanges a project's tasks with a computer through the
Rust relay, the Rust connector and the real hgs
(docs/superpowers/specs/2026-10-18-task-replication-loro-design.md, "Phone").

hgs runs with a private home, configuration and state; its tmux is a stub that
fails, and the connector's session listing is answered without it. A second
hgs state stands in for the phone's replica.
"""
import asyncio
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
import uuid

from aiohttp import ClientSession

from e2e_connector import COMMAND, REQUIRED, running
from relay_fixture import start_relay

NATIVE = "ZERUS_TASK_TEST_HGS"
CATALOG = {"projects": [{"id": "project-1", "name": "orchard", "color": "#123456", "folders": [], "sessions": []}]}
EMPTY = {"project": {}, "tasks": {}}


class TaskExchangeEndToEnd(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        for variable, what in ((NATIVE, "an isolated native CLI"), (COMMAND, "a built Rust connector")):
            if not os.environ.get(variable):
                if os.environ.get(REQUIRED) == "1":
                    self.fail(f"{REQUIRED}=1: this suite needs {variable} naming {what}")
                self.skipTest(f"{variable} names no {what}")
        self.native = str(Path(os.environ[NATIVE]).resolve())
        directory = tempfile.TemporaryDirectory(prefix="zerus-task-exchange-")
        self.addCleanup(directory.cleanup)
        self.root = Path(directory.name)
        self.computer = self.replica("computer")
        self.phone_replica = self.replica("phone")
        relay = await start_relay(rust_only=True)
        self.addAsyncCleanup(relay.cleanup)
        self.relay = relay
        self.phone = await relay.pair("Example phone", relay.invitation["pair_code"])
        self.client = ClientSession(base_url=relay.url)
        self.addAsyncCleanup(self.client.close)

    def replica(self, name: str) -> dict:
        """The environment of an isolated hgs whose catalog has project orchard."""
        home = self.root / name / "home"
        (home / ".local/bin").mkdir(parents=True)
        tmux = home / ".local/bin/tmux"
        tmux.write_text("#!/bin/sh\nexit 1\n")
        tmux.chmod(0o755)
        (self.root / name / "tmux").mkdir(mode=0o700)
        env = {"HOME": str(home), "PATH": "/usr/bin:/bin", "TMUX_TMPDIR": str(self.root / name / "tmux"),
               "HGS_CONFIG_DIR": str(self.root / name / "config"), "HGS_STATE_DIR": str(self.root / name / "state")}
        self.hgs(env, "swarm", "initialize", data=CATALOG)
        return env

    def hgs(self, env: dict, *args: str, data=None, text: str | None = None):
        stdin = text if data is None else json.dumps(data)
        result = subprocess.run([self.native, *args], input=stdin, env=env, text=True, capture_output=True, timeout=60)
        self.assertEqual(result.returncode, 0, result.stderr)
        return json.loads(result.stdout)

    async def call(self, method: str, route: str, body=None, expected: int = 200):
        headers = {"Authorization": "Bearer " + self.phone["device_token"]}
        async with self.client.request(method, route, json=body, headers=headers) as response:
            self.assertEqual(response.status, expected, await response.text())
            return await response.json()

    async def computer_snapshot(self, ready, what: str) -> dict:
        for _ in range(400):
            computers = (await self.call("GET", "/v1/computers"))["computers"]
            snapshot = computers[0].get("snapshot") or {}
            if computers[0]["online"] and ready(snapshot):
                return snapshot
            await asyncio.sleep(0.05)
        self.fail(f"the computer's heartbeat never showed {what}")

    async def exchange(self, payload: dict) -> dict:
        request_id = str(uuid.uuid4())
        body = {"request_id": request_id, "computer_id": self.relay.node["node_id"], "operation": "task_exchange",
                "session": "", "payload": payload}
        receipt = await self.call("POST", "/v1/requests", body, expected=202)
        for _ in range(400):
            if receipt["state"] not in ("queued", "claimed"):
                break
            await asyncio.sleep(0.05)
            receipt = await self.call("GET", "/v1/requests/" + request_id)
        self.assertEqual(receipt["state"], "completed", receipt)
        return receipt["result"]

    async def test_a_phone_exchanges_tasks_through_the_relay_and_the_rust_connector(self):
        self.assertIn("task_exchange", (await self.call("GET", "/v1/capabilities"))["operations"])
        self.hgs(self.computer, "task", "add", "--project", "orchard", text="Add rate limiting")
        listing = self.root / "bin/hgs"
        listing.parent.mkdir()
        # The connector's session listing comes from here; everything else is the real hgs.
        listing.write_text("#!/bin/sh\nif [ \"$1\" = ls ]; then\n"
                           "  echo '{\"host\": \"Synthetic computer\", \"sessions\": []}'\n  exit 0\nfi\n"
                           f"exec '{self.native}' \"$@\"\n")
        listing.chmod(0o755)
        config = {"server_url": self.relay.url, "node_token": self.relay.node["node_token"],
                  "hgs_path": str(listing), "state_dir": str(self.root / "journal"), "poll_interval": 1}
        async with running("rust", config, self.root, self.computer) as connector:
            def ready(snapshot):
                projects = (snapshot.get("mobile_projects") or {}).get("projects") or []
                operations = (snapshot.get("mobile_capabilities") or {}).get("operations") or []
                return "task_exchange" in operations and projects and "tasks_digest" in projects[0]
            snapshot = await self.computer_snapshot(ready, "the task exchange and a tasks digest")
            connector.check()
            here = self.hgs(self.computer, "task", "digests", "--json")["projects"]["project-1"]["digest"]
            self.assertEqual(snapshot["mobile_projects"]["projects"][0]["tasks_digest"], here)
            # An empty phone gets the live task (the project document was never saved).
            [task] = self.hgs(self.computer, "task", "list", "--project", "orchard")["tasks"]
            answer = await self.exchange({"schema": 1, "project": "project-1", "inventory": EMPTY})
            self.assertEqual((answer["more"], answer["digest"], answer["refused"]), (False, here, []))
            self.assertEqual([update["task"] for update in answer["updates"]], [task["uuid"]])
            self.hgs(self.phone_replica, "task", "exchange", "--json",
                     data={"schema": 1, "project": "project-1", "inventory": EMPTY, "updates": answer["updates"]})
            self.assertEqual(self.hgs(self.phone_replica, "task", "digests", "--json")["projects"]["project-1"]["digest"],
                             here)
            # What the phone files reaches the computer.
            self.hgs(self.phone_replica, "task", "add", "--project", "orchard", text="Filed on the phone")
            held = self.hgs(self.phone_replica, "task", "exchange", "--json",
                            data={"schema": 1, "project": "project-1", "inventory": EMPTY})
            answer = await self.exchange({"schema": 1, "project": "project-1", "inventory": EMPTY,
                                          "updates": held["updates"]})
            self.assertGreaterEqual(answer["imported"], 1)
            tasks = self.hgs(self.computer, "task", "list", "--project", "orchard")["tasks"]
            self.assertEqual(sorted(task["text"] for task in tasks), ["Add rate limiting", "Filed on the phone"])
            # The computer's next heartbeat names the new digest.
            changed = self.hgs(self.computer, "task", "digests", "--json")["projects"]["project-1"]["digest"]
            self.assertNotEqual(changed, here)
            await self.computer_snapshot(
                lambda snapshot: snapshot["mobile_projects"]["projects"][0].get("tasks_digest") == changed,
                "the changed tasks digest")
            connector.check()


if __name__ == "__main__":
    unittest.main()
