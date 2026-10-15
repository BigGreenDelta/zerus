"""Black-box SIGTERM, as `systemctl stop` sends it under KillMode=process.

Divergence h (amended): Python has no handler and dies at once; its restart
journals an in-flight mutation as uncertain. Rust stops claiming, lets the
in-flight native call finish within its own timeout, journals its real outcome
and exits 0. Neither ever kills the native mutation child.
"""
import json
import signal
import unittest

from connector_harness import BlackBoxCase, IMPLEMENTATION, RESTART_ERROR, SESSION, acknowledgement


class SigtermTests(BlackBoxCase):
    def assert_stopped(self, code):
        self.assertEqual(code, -signal.SIGTERM if IMPLEMENTATION == "python" else 0)

    async def test_sigterm_stops_claiming_and_lets_the_native_mutation_finish(self):
        self.hgs.prepend({"argv": ["send", SESSION, "--json"], "stdout": acknowledgement(), "delay": 1.5})
        first = await self.launch()
        running = self.request("send", text="in flight at SIGTERM")
        await self.relay.queue(running)
        [call] = await self.until(lambda: self.hgs.calls("send"), what="the native mutation", process=first)
        first.signal(signal.SIGTERM)
        waiting = self.request("send", text="queued after SIGTERM")
        await self.relay.queue(waiting)
        self.assert_stopped(await first.wait_exit(30))
        ended = await self.until(lambda: self.hgs.finished(call["pid"]), what="the native call to finish on its own")
        self.assertEqual(ended["exit"], 0)
        self.assertEqual(len(self.hgs.calls("send")), 1, "a request was claimed after SIGTERM")
        self.assertEqual(self.relay.state(waiting["request_id"]), "queued")
        row = self.journal.row(running["request_id"])
        if IMPLEMENTATION == "python":
            self.assertEqual(row["state"], "running")
            expected = {"state": "uncertain", "result": None, "error": RESTART_ERROR}
        else:
            expected = {"state": "completed", "error": None, "result": {
                **acknowledgement(), "request_id": running["request_id"]}}
            self.assertEqual(json.loads(row["response"]), expected)
        second = await self.launch()
        self.assertEqual((await self.until(lambda: self.relay.results(running["request_id"]),
                                           what="the outcome upload", process=second))[0], expected)
        self.assertEqual((await self.until(lambda: self.relay.results(waiting["request_id"]),
                                           what="the queued request", process=second))[0]["state"], "completed")
        self.assertEqual(len(self.hgs.calls("send")), 2)

    async def test_sigterm_while_idle_leaves_a_reusable_journal(self):
        first = await self.launch()
        first.signal(signal.SIGTERM)
        self.assert_stopped(await first.wait_exit(10))
        second = await self.launch()
        request = self.request("send", text="after an idle stop")
        self.assertEqual((await self.submit(request, second))["state"], "completed")


if __name__ == "__main__":
    unittest.main()
