"""Black-box --check: the Rust connector's read-only report, next to a running
Python connector on the same journal. --check exists only in the Rust
connector, so this module runs only in Rust mode."""
import asyncio
import hashlib
import json
import os
import subprocess
import sys
import unittest

from connector_harness import (BlackBoxCase, ConnectorProcess, IMPLEMENTATION, SERVICE, STARTUP_FAILURE,
                               connector_command)


class PythonConnector(ConnectorProcess):
    """The Python connector from this source tree, whichever one is under test."""

    def start(self) -> ConnectorProcess:
        with open(self.log_path, "wb") as log:
            self.process = subprocess.Popen(
                [sys.executable, "-m", "zerus_mobile.connector", *self.args], cwd=self.case.scratch, env=self.env,
                stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
        self.case.hgs.parent(self.process.pid)
        if sys.platform.startswith("linux"):
            self.guard = asyncio.get_running_loop().create_task(self.watch_sockets())
        return self


def fingerprint(directory):
    """Bytes, modification time and inode of every file in a state directory."""
    found = {}
    for name in sorted(os.listdir(directory)):
        info = os.lstat(directory / name)
        found[name] = (hashlib.sha256((directory / name).read_bytes()).hexdigest(), info.st_mtime_ns, info.st_ino)
    return found


class CheckTests(BlackBoxCase):
    async def asyncSetUp(self):
        if IMPLEMENTATION != "rust":
            self.skipTest("--check is a command of the Rust connector")
        await super().asyncSetUp()

    async def launch_python(self) -> PythonConnector:
        path = self.write_config(self.config())
        env = self.environment({"PYTHONPATH": str(SERVICE), "PYTHONDONTWRITEBYTECODE": "1"})
        process = PythonConnector(self, ["--config", str(path), "--allow-insecure-localhost"], env)
        self.processes.append(process)
        process.start()
        await self.ready(process)
        return process

    async def check(self, config=None, code=0, mode=0o600) -> dict:
        path = self.write_config(self.config() if config is None else config)
        path.chmod(mode)
        # In a thread: the fake relay shares this event loop with the test.
        result = await asyncio.to_thread(
            subprocess.run, [*connector_command(), "--check", "--json", "--config", str(path), "--allow-insecure-localhost"],
            cwd=self.scratch, env=self.environment(), stdin=subprocess.DEVNULL, capture_output=True, timeout=30)
        self.assertEqual(result.returncode, code, (result.stdout + result.stderr).decode(errors="replace"))
        self.assertEqual(result.stderr, b"")
        self.assertNotIn(self.relay.token.encode(), result.stdout)
        return json.loads(result.stdout)

    async def test_check_reads_a_running_python_connectors_journal_in_place(self):
        python = await self.launch_python()
        request = self.request("send", text="before the check")
        await self.submit(request, python)
        await self.until(lambda: (row := self.journal.row(request["request_id"])) and row["delivered"],
                         what="the delivered journal row", process=python)
        before = fingerprint(self.state_dir)
        report = await self.check()
        self.assertEqual(fingerprint(self.state_dir), before, "--check changed the journal of a running connector")
        state = report["state"]
        self.assertEqual((report["ok"], state["lock"], state["journal"]["binding"], state["journal"]["schema"]),
                         (True, "held", "matches", "current"))
        self.assertEqual(state["journal"]["rows"], {"total": 1, "running": 0, "undelivered": 0,
                                                    "states": {"completed": {"delivered": 1}}})
        beats = len(self.relay.heartbeats())
        await self.until(lambda: len(self.relay.heartbeats()) > beats, what="the next heartbeat", process=python)
        self.assertIsNone(python.returncode)

    async def test_check_never_makes_a_busy_python_connector_fail(self):
        python = await self.launch_python()
        requests = [self.request("send", text=f"busy {index}") for index in range(12)]
        for request in requests:
            await self.relay.queue(request)
        checks, deadline = 0, asyncio.get_running_loop().time() + 60

        def finished():
            return all(self.relay.results(request["request_id"]) for request in requests)
        # Every claim, receipt and delivery is a journal write while the checks read.
        while not finished() or checks < 5:
            report = await self.check()
            self.assertEqual((report["ok"], report["state"]["lock"]), (True, "held"))
            checks += 1
            self.assertLess(asyncio.get_running_loop().time(), deadline, "the requests did not finish:\n" + python.output())
        self.assertIsNone(python.returncode, python.output())
        self.assertEqual({self.relay.results(request["request_id"])[0]["state"] for request in requests}, {"completed"})
        self.assertNotIn("Traceback", python.output())

    async def test_rust_connector_refuses_a_journal_python_holds(self):
        python = await self.launch_python()
        rust = await self.launch(ready=False)
        self.assertEqual(await rust.wait_exit(10), 1, rust.output())
        self.assertIn(STARTUP_FAILURE, rust.output())
        beats = len(self.relay.heartbeats())
        await self.until(lambda: len(self.relay.heartbeats()) > beats, what="the next heartbeat", process=python)
        self.assertIsNone(python.returncode)

    async def test_check_reports_reviewed_refusals_and_creates_nothing(self):
        for name, config, mode, divergence in (
                ("boolean poll interval", self.config(poll_interval=True), 0o600, "k"),
                ("falsy state_dir", self.config(state_dir=[]), 0o600, "k"),
                ("another user's home", self.config(state_dir="~nosuchuser-blackbox/state"), 0o600, "f"),
                ("group-writable configuration", self.config(), 0o660, "e")):
            with self.subTest(name):
                report = await self.check(config, code=1, mode=mode)
                self.assertFalse(report["ok"])
                self.assertIn(divergence, {finding["divergence"] for finding in report["configuration"]["findings"]
                                           if finding["level"] == "refusal"})
                self.assertFalse(self.state_dir.exists())
        self.assertEqual(self.relay.exchanges, [])
        self.assertEqual(self.hgs.events(), [])


if __name__ == "__main__":
    unittest.main()
