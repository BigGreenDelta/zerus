"""Black-box journal handoffs between implementations (C7 parity gate).

One state directory passes from Python to Rust and back to Python, as a
cutover and its rollback do, while each implementation holds rows the other
wrote: running mutations killed mid-flight, delivered replays, conflicting
identities, scrubbed tombstones, undelivered outbox rows and a foreign binding.

It runs in a Rust run (ZERUS_CONNECTOR_COMMAND), against this tree's Python
connector and every rollback baseline in ZERUS_CONNECTOR_ROLLBACK_COMMANDS
(os.pathsep-separated console scripts of recorded venvs, see
rollback-baselines.json).
"""
import os
from pathlib import Path
import signal
import unittest

from connector_harness import (IMPLEMENTATION, PYTHON_COMMAND, RESTART_ERROR, SESSION, STARTUP_FAILURE,
                               BlackBoxCase, Journal, acknowledgement, connector_command,
                               recover_stopped_journal, stopped_journal)

WEEK = 7 * 86400
EXPIRED = {"state": "completed", "result": {"status": "history_expired"},
           "error": "Native result history expired; this request will not execute again"}
RECOVERED = {"state": "uncertain", "result": None, "error": RESTART_ERROR}


def pythons() -> list[tuple[str, list[str]]]:
    rollbacks = [path for path in os.environ.get("ZERUS_CONNECTOR_ROLLBACK_COMMANDS", "").split(os.pathsep) if path]
    return [("main", PYTHON_COMMAND), *((Path(path).parent.parent.name, [path]) for path in rollbacks)]


class HandoffTests(BlackBoxCase):
    async def asyncSetUp(self):
        if IMPLEMENTATION != "rust":
            self.skipTest("a handoff needs the Rust connector (ZERUS_CONNECTOR_COMMAND)")
        await super().asyncSetUp()

    async def handoffs(self, scenario):
        """Run `scenario(first, second, third)` as Python, Rust, Python, with
        one fresh state directory per Python baseline."""
        for index, (name, python) in enumerate(pythons()):
            with self.subTest(python=name):
                self.state_dir = self.scratch / f"state-{index}"
                self.journal = Journal(self.state_dir)
                await scenario(python, connector_command(), python)

    def sends(self, text: str) -> list[dict]:
        return [call for call in self.hgs.calls("send", SESSION, "--json") if text in call["stdin"]]

    async def delivered(self, process, *requests):
        await self.until(lambda: all((row := self.journal.row(request["request_id"])) and row["delivered"]
                                     for request in requests), what="delivered journal rows", process=process)

    async def replays(self, process, request, expected):
        self.assertEqual(await self.submit(request, process, redeliver=True), expected)

    def age(self, seconds, *requests):
        with stopped_journal(self.state_dir) as db:
            for request in requests:
                db.execute("UPDATE requests SET created_at=created_at-?, updated_at=updated_at-?,"
                           " delivered_at=delivered_at-? WHERE request_id=?",
                           (seconds, seconds, seconds, request["request_id"]))

    def crash_after(self, text: str):
        """Script one send that succeeds natively and then SIGKILLs its connector."""
        self.hgs.prepend({"argv": ["send", SESSION, "--json"], "stdin_fields": {"text": text},
                          "stdout": acknowledgement(), "kill_parent": True})

    async def test_a_mutation_killed_mid_flight_stays_uncertain_in_both_directions(self):
        await self.handoffs(self.a_mutation_killed_mid_flight_stays_uncertain_in_both_directions)

    async def a_mutation_killed_mid_flight_stays_uncertain_in_both_directions(self, first, second, third):
        tag = self.state_dir.name
        killed_first = self.request("send", text=f"{tag} killed under the first connector")
        killed_second = self.request("send", text=f"{tag} killed under the second connector")
        self.crash_after(killed_first["payload"]["text"])
        self.crash_after(killed_second["payload"]["text"])
        process = await self.launch(command=first)
        await self.relay.queue(killed_first)
        self.assertEqual(await process.wait_exit(10), -signal.SIGKILL)
        self.assertEqual(self.journal.row(killed_first["request_id"])["state"], "running")

        process = await self.launch(command=second)
        await self.until(lambda: self.relay.results(killed_first["request_id"]) == [RECOVERED],
                         what="the recovered upload", process=process)
        await self.replays(process, killed_first, RECOVERED)
        await self.relay.queue(killed_second)
        self.assertEqual(await process.wait_exit(10), -signal.SIGKILL)
        self.assertEqual(self.journal.row(killed_second["request_id"])["state"], "running")

        process = await self.launch(command=third)
        await self.until(lambda: self.relay.results(killed_second["request_id"]) == [RECOVERED],
                         what="the recovered upload", process=process)
        await self.replays(process, killed_first, RECOVERED)
        await self.replays(process, killed_second, RECOVERED)
        process.stop()
        for request in (killed_first, killed_second):
            self.assertEqual(len(self.sends(request["payload"]["text"])), 1)

    async def refuses_a_changed_body(self, process, request):
        """The same identity with another body: no native work, no upload, no journal change."""
        # A replay marks its row delivered again only after its upload.
        await self.delivered(process, request)
        before = self.journal.row(request["request_id"])
        uploads = len(self.relay.uploads(request["request_id"]))
        mark = len(self.relay.deliveries)
        await self.relay.queue({**request, "payload": {**request["payload"], "text": "changed"}}, redeliver=True)
        await self.settle(request["request_id"], mark, process)
        self.assertEqual(len(self.relay.uploads(request["request_id"])), uploads)
        self.assertEqual(self.journal.row(request["request_id"]), before)

    async def test_delivered_rows_replay_and_conflicting_identities_never_execute(self):
        await self.handoffs(self.delivered_rows_replay_and_conflicting_identities_never_execute)

    async def delivered_rows_replay_and_conflicting_identities_never_execute(self, first, second, third):
        tag = self.state_dir.name
        early = self.request("send", text=f"{tag} delivered by the first connector")
        later = self.request("send", text=f"{tag} delivered by the second connector")
        results = {}
        for command, new, old in ((first, early, ()), (second, later, (early,)), (third, None, (early, later))):
            process = await self.launch(command=command)
            for request in old:
                await self.replays(process, request, results[request["request_id"]])
                await self.refuses_a_changed_body(process, request)
            if new is not None:
                results[new["request_id"]] = await self.submit(new, process)
                self.assertEqual(results[new["request_id"]]["state"], "completed")
                await self.delivered(process, new)
            process.stop()
        for request in (early, later):
            self.assertEqual(len(self.sends(request["payload"]["text"])), 1)
        self.assertEqual(self.sends("changed"), [])

    async def test_scrubbed_tombstones_never_execute_again(self):
        await self.handoffs(self.scrubbed_tombstones_never_execute_again)

    async def scrubbed_tombstones_never_execute_again(self, first, second, third):
        tag = self.state_dir.name
        early = self.request("send", text=f"{tag} BB-PRIVATE scrubbed by the second connector")
        later = self.request("send", text=f"{tag} BB-PRIVATE scrubbed by the third connector")
        for command, request, scrubbed in ((first, early, ()), (second, later, (early,)), (third, None, (early, later))):
            process = await self.launch(command=command)
            for old in scrubbed:
                await self.until(lambda: self.journal.row(old["request_id"])["response_expired"],
                                 what="the scrubbed row", process=process)
                await self.replays(process, old, EXPIRED)
            if request is not None:
                self.assertEqual((await self.submit(request, process))["state"], "completed")
                await self.delivered(process, request)
            process.stop()
            if request is not None:
                self.age(WEEK + 1, request)
        recover_stopped_journal(self.state_dir)
        self.assertNotIn(b"BB-PRIVATE", (self.state_dir / "journal.sqlite3").read_bytes())
        for request in (early, later):
            self.assertEqual(self.journal.row(request["request_id"])["state"], "completed")
            self.assertEqual(len(self.sends(request["payload"]["text"])), 1)

    async def test_undelivered_results_upload_unchanged_after_a_handoff(self):
        await self.handoffs(self.undelivered_results_upload_unchanged_after_a_handoff)

    async def undelivered_results_upload_unchanged_after_a_handoff(self, first, second, third):
        tag = self.state_dir.name
        early = self.request("send", text=f"{tag} undelivered under the first connector")
        later = self.request("send", text=f"{tag} undelivered under the second connector")
        refused = {}
        for command, request, waiting in ((first, early, None), (second, later, early), (third, None, later)):
            process = await self.launch(command=command)
            if waiting is not None:
                [accepted] = await self.until(lambda: self.relay.results(waiting["request_id"]),
                                              what="the retried upload", process=process)
                self.assertEqual(accepted, refused[waiting["request_id"]])
                await self.delivered(process, waiting)
            if request is not None:
                rule = self.relay.rule("POST", rf"/v1/node/requests/{request['request_id']}/result", 503)
                await self.relay.queue(request)
                [(body, _)] = (await self.until(lambda: self.relay.uploads(request["request_id"]),
                                                what="a refused upload", process=process))[:1]
                self.assertEqual(body["state"], "completed")
                refused[request["request_id"]] = body
            process.stop()
            if request is not None:
                self.relay.rules.remove(rule)
                self.assertEqual(self.journal.row(request["request_id"])["delivered"], 0)
        for request in (early, later):
            self.assertEqual(len(self.sends(request["payload"]["text"])), 1)

    async def test_a_journal_bound_to_other_credentials_is_refused_by_both(self):
        await self.handoffs(self.a_journal_bound_to_other_credentials_is_refused_by_both)

    async def a_journal_bound_to_other_credentials_is_refused_by_both(self, first, second, third):
        (await self.launch(command=first)).stop()
        for command in (second, third):
            before = (self.state_dir / "journal.sqlite3").read_bytes()
            for changes in ({"node_token": "bb-other-token"}, {"server_url": self.relay.url + "/other"}):
                process = await self.launch(self.config(**changes), ready=False, command=command)
                self.assertEqual(await process.wait_exit(10), 1, process.output())
                self.assertIn(STARTUP_FAILURE, process.output())
                self.assertEqual((self.state_dir / "journal.sqlite3").read_bytes(), before)
            (await self.launch(command=command)).stop()


if __name__ == "__main__":
    unittest.main()
