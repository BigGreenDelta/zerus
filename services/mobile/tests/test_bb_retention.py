"""Black-box journal retention, with rows aged while the connector is stopped."""
import unittest

from connector_harness import BlackBoxCase, stopped_journal

HOUR, WEEK = 3600, 7 * 86400
EXPIRED = {"state": "completed", "result": {"status": "history_expired"},
           "error": "Native result history expired; this request will not execute again"}


class RetentionTests(BlackBoxCase):
    def age(self, seconds, *requests):
        with stopped_journal(self.state_dir) as db:
            for request in requests:
                db.execute("UPDATE requests SET created_at=created_at-?, updated_at=updated_at-?,"
                           " delivered_at=delivered_at-? WHERE request_id=?",
                           (seconds, seconds, seconds, request["request_id"]))

    async def delivered(self, process, *requests):
        await self.until(lambda: all((row := self.journal.row(request["request_id"])) and row["delivered"]
                                     for request in requests), what="delivered journal rows", process=process)

    async def test_delivered_read_expires_after_an_hour_and_may_execute_again(self):
        first = await self.launch()
        read = self.request("inspect")
        result = await self.submit(read, first)
        await self.delivered(first, read)
        first.stop()
        self.age(HOUR + 1, read)
        second = await self.launch()
        self.assertIsNone(self.journal.row(read["request_id"]))
        self.assertEqual(await self.submit(read, second, redeliver=True), result)
        self.assertEqual(len(self.hgs.calls("inspect")), 2)

    async def test_delivered_mutation_is_scrubbed_after_seven_days_and_never_repeats(self):
        first = await self.launch()
        sent = self.request("send", text="BB-PRIVATE-NATIVE-RESULT")
        await self.submit(sent, first)
        await self.delivered(first, sent)
        first.stop()
        self.age(WEEK + 1, sent)
        second = await self.launch()
        row = await self.until(lambda: (row := self.journal.row(sent["request_id"])) and row["response_expired"] and row,
                               what="the scrubbed row", process=second)
        self.assertEqual(row["state"], "completed")
        self.assertNotIn(b"BB-PRIVATE-NATIVE-RESULT", (self.state_dir / "journal.sqlite3").read_bytes())
        self.assertEqual(await self.submit(sent, second, redeliver=True), EXPIRED)
        changed = {**sent, "payload": {**sent["payload"], "text": "changed"}}
        mark = len(self.relay.deliveries)
        await self.relay.queue(changed, redeliver=True)
        await self.settle(sent["request_id"], mark, second)
        self.assertEqual(self.relay.results(sent["request_id"])[-1], EXPIRED)
        self.assertEqual(len(self.hgs.calls("send")), 1)

    async def test_undelivered_results_never_expire(self):
        # Python flushes before it polls, so a refused upload blocks claiming:
        # leave one row undelivered per run.
        for request in (self.request("inspect"), self.request("send", text="waiting for the relay")):
            with self.subTest(request["operation"]):
                rule = self.relay.rule("POST", rf"/v1/node/requests/{request['request_id']}/result", 503)
                first = await self.launch()
                await self.relay.queue(request)
                [(refused, _)] = await self.until(lambda: self.relay.uploads(request["request_id"])[:1],
                                                  what="a refused upload", process=first)
                first.stop()
                self.age(WEEK + 1, request)
                self.relay.rules.remove(rule)
                second = await self.launch()
                accepted = await self.until(lambda: self.relay.results(request["request_id"]),
                                            what="the retained upload", process=second)
                self.assertEqual(accepted, [refused])
                second.stop()
        self.assertEqual((len(self.hgs.calls("inspect")), len(self.hgs.calls("send"))), (1, 1))


if __name__ == "__main__":
    unittest.main()
