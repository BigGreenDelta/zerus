"""Black-box restarts: a preserved journal never repeats native work."""
import signal
import unittest

from connector_harness import BlackBoxCase, RESTART_ERROR, SESSION, acknowledgement


class RestartTests(BlackBoxCase):
    async def test_restart_replays_a_delivered_mutation_without_executing_it(self):
        first = await self.launch()
        request = self.request("send", text="exactly once across restarts")
        result = await self.submit(request, first)
        await self.until(lambda: (row := self.journal.row(request["request_id"])) and row["delivered"],
                         what="the delivered journal row")
        first.stop()
        second = await self.launch()
        self.assertEqual(await self.submit(request, second, redeliver=True), result)
        self.assertEqual(len(self.hgs.calls("send")), 1)
        later = self.request("send", text="a new request after restart")
        self.assertEqual((await self.submit(later, second))["state"], "completed")
        self.assertEqual(len(self.hgs.calls("send")), 2)

    async def test_crash_after_native_success_restarts_as_uncertain_without_repeating(self):
        # The native write completes, then the connector dies before journaling it.
        self.hgs.prepend({"argv": ["send", SESSION, "--json"], "stdout": acknowledgement(), "kill_parent": True})
        first = await self.launch()
        request = self.request("send", text="native write before a crash")
        await self.relay.queue(request)
        self.assertEqual(await first.wait_exit(10), -signal.SIGKILL)
        self.assertEqual(self.relay.results(request["request_id"]), [])
        self.assertEqual(self.journal.row(request["request_id"])["state"], "running")
        self.hgs.script([rule for rule in self.hgs.rules if not rule.get("kill_parent")])
        second = await self.launch()
        recovered = {"state": "uncertain", "result": None, "error": RESTART_ERROR}
        self.assertEqual(await self.until(lambda: self.relay.results(request["request_id"]),
                                          what="the recovered upload", process=second), [recovered])
        self.assertEqual(await self.submit(request, second, redeliver=True), recovered)
        self.assertEqual(len(self.hgs.calls("send")), 1)

    async def test_undelivered_result_survives_restart_and_uploads_unchanged(self):
        self.relay.rule("POST", r"/v1/node/requests/[^/]+/result", 503)
        first = await self.launch()
        request = self.request("send", text="outbox across restarts")
        await self.relay.queue(request)
        refused = await self.until(lambda: self.relay.uploads(request["request_id"]),
                                   what="a refused upload", process=first)
        first.stop()
        self.assertEqual(self.journal.row(request["request_id"])["delivered"], 0)
        self.relay.rules.clear()
        second = await self.launch()
        [accepted] = await self.until(lambda: self.relay.results(request["request_id"]),
                                      what="the retried upload", process=second)
        self.assertEqual(accepted, refused[0][0])
        self.assertEqual(accepted["state"], "completed")
        self.assertEqual(len(self.hgs.calls("send")), 1)


if __name__ == "__main__":
    unittest.main()
