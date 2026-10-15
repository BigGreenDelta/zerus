"""Black-box public history pages and recovery actions.

A history read passes its scoped payload unchanged and keeps only a page of
exactly this request, session, run and conversation; a head read follows
local indexing briefly while it progresses, a cursor read never repeats. A
recovery action acts only on the exact waiting job, once. A computer whose
hgs lacks either ABI runs neither.
"""
import json
import unittest
import uuid

from session_harness import ARCHIVE, CONVERSATION, RUN, SESSION, SessionCase, submission

HISTORY = ["history", SESSION, "--json"]
ENVELOPE = "native public history identity or envelope did not match"
JOB_CHANGED = "native recovery job changed; refresh before action"


def page(**changes) -> dict:
    return {"request_id": "{{stdin.request_id}}", "name": "{{argv.1}}", "run_id": RUN, "conversation_id": CONVERSATION,
            "events": [], "history_epoch": "epoch", "head": {"complete": True, "total_incoming": 0}, **changes}


def answered(value: dict, request: dict) -> dict:
    """`value` as scripted hgs answers it for this request."""
    return {**value, "request_id": request["request_id"], "name": request["session"]}


class HistoryTests(SessionCase):
    markers = ("history",)

    async def test_history_reads_pass_their_scoped_payload_unchanged(self):
        process = await self.launch()
        for name, extra in (("valid/history", {}), ("history/anchor", {}), ("history/archive", {"archive_id": ARCHIVE})):
            with self.subTest(name):
                request = submission("control", name)
                self.hgs.script(self.rules({"argv": HISTORY, "stdout": page(**extra)}))
                before = len(self.native_calls())
                result = await self.submit(request, process)
                self.assertEqual(result, {"state": "completed", "result": answered(page(**extra), request), "error": None})
                [call] = self.native_calls()[before:]
                self.assertEqual((call["argv"], json.loads(call["stdin"])), (HISTORY, request["payload"]))
                await self.assert_replays(request, result, process)
        child = self.envelope("history", {"agent_id": "child"})
        answer = page(agent_id="child", parent_conversation_id=CONVERSATION, conversation_id=CONVERSATION + "/child")
        self.hgs.script(self.rules({"argv": HISTORY, "stdout": answer}))
        self.assertEqual((await self.submit(child, process))["state"], "completed")

    async def test_a_page_of_anything_else_fails_closed(self):
        process = await self.launch()
        for changes in ({"run_id": "replacement-run"}, {"conversation_id": "other"}, {"name": "other"},
                        {"request_id": str(uuid.uuid4())}, {"archive_id": ARCHIVE}, {"agent_id": "child"},
                        {"events": {}}, {"history_epoch": 1}, {"head": []}):
            with self.subTest(changes):
                self.hgs.script(self.rules({"argv": HISTORY, "stdout": page(**changes)}))
                request = self.envelope("history", {})
                receipt = await self.submit(request, process)
                self.assertEqual(receipt, self.failed(ENVELOPE))
                await self.assert_replays(request, receipt, process)
        before = len(self.native_calls())
        unknown = self.envelope("history", {"archive_id": str(uuid.uuid4())})
        self.assertEqual(await self.submit(unknown, process),
                         self.failed("archive must match a fresh exact local snapshot entry"))
        self.assertEqual(len(self.native_calls()), before)

    async def test_a_head_read_follows_local_indexing_while_it_progresses(self):
        process = await self.launch()
        stages = [page(indexing=True, head={"complete": False}, source_status={"progress": {"source_bytes": index}})
                  for index in range(2)]
        complete = page(indexing=False, source_status={"progress": {"source_bytes": 2}})
        self.hgs.script(self.rules(*({"argv": HISTORY, "stdout": stage, "times": 1} for stage in stages),
                                   {"argv": HISTORY, "stdout": complete}))
        request = self.envelope("history", {"limit": 20})
        result = await self.submit(request, process)
        self.assertEqual(result["result"], answered(complete, request))
        calls = self.native_calls("history")
        self.assertEqual([json.loads(call["stdin"]) for call in calls], [request["payload"]] * 3)
        stalled = page(indexing=True, head={"complete": False}, source_status={"provider": "partial"})
        for answer, count in ((stalled, 2), (page(indexing=False, head={"complete": False}), 1)):
            with self.subTest(indexing=answer["indexing"]):
                self.hgs.script(self.rules({"argv": HISTORY, "stdout": answer}))
                before = len(self.native_calls("history"))
                self.assertEqual((await self.submit(self.envelope("history", {}), process))["state"], "completed")
                self.assertEqual(len(self.native_calls("history")) - before, count)

    async def test_a_cursor_read_is_never_repeated_for_indexing(self):
        self.hgs.script(self.rules({"argv": HISTORY, "stdout": page(indexing=True, head={"complete": False})}))
        process = await self.launch()
        for selector in ({"before": "opaque"}, {"after": "opaque"}, {"around": "opaque"},
                         {"around_incoming_seq": 3, "history_epoch": "epoch"}):
            with self.subTest(selector):
                before = len(self.native_calls("history"))
                self.assertEqual((await self.submit(self.envelope("history", selector), process))["state"], "completed")
                self.assertEqual(len(self.native_calls("history")) - before, 1)

    async def test_hgs_without_the_history_abi_runs_nothing(self):
        self.hgs.script(self.rules({"argv": HISTORY, "stdout": page()}, markers=()))
        process = await self.launch()
        self.assertNotIn("history", self.relay.heartbeats()[-1].body["snapshot"]["mobile_capabilities"]["operations"])
        self.assertEqual(await self.submit(submission("control", "valid/history"), process),
                         self.failed("native public history ABI is unavailable"))
        self.assertEqual(self.native_calls(), [])


class RecoveryActionTests(SessionCase):
    markers = ("recovery",)

    def script(self, fresh, acknowledgement=None) -> None:
        rules = [{"argv": ["inspect", SESSION, "--skip-processes"], "stdout": fresh}]
        if acknowledgement is not None:
            rules.append({"argv": ["recovery", "action", "--scoped-json"], "stdout": acknowledgement})
        self.hgs.script(self.rules(*rules))

    @staticmethod
    def job(payload: dict, **changes) -> dict:
        return {"id": payload["job_id"], "state": "waiting",
                "identity": [payload["expected_run_id"], payload["expected_conversation_id"], "model", None, None], **changes}

    @staticmethod
    def fresh(job, **changes) -> dict:
        return {"name": SESSION, "run_id": RUN, "conversation_id": CONVERSATION, "recovery": job, **changes}

    @staticmethod
    def acknowledgement(payload: dict, **changes) -> dict:
        return {"request_id": "{{stdin.request_id}}", "name": "{{stdin.name}}", "run_id": RUN,
                "conversation_id": CONVERSATION, "job_id": payload["job_id"], "action": payload["action"],
                "status": "scheduled", **changes}

    def recovery(self, action="now") -> dict:
        return self.envelope("recovery_action", {"job_id": str(uuid.uuid4()), "action": action})

    async def test_recovery_action_checks_the_waiting_job_then_acts_once(self):
        request = submission("control", "valid/recovery_action")
        payload = request["payload"]
        self.script(self.fresh(self.job(payload)), self.acknowledgement(payload))
        process = await self.launch()
        result = await self.submit(request, process)
        self.assertEqual(result, {"state": "completed", "error": None, "result": {
            "request_id": request["request_id"], "name": SESSION, "run_id": RUN, "conversation_id": CONVERSATION,
            "job_id": payload["job_id"], "action": "now", "status": "scheduled"}})
        self.assertEqual([call["argv"] for call in self.native_calls()],
                         [["inspect", SESSION, "--skip-processes"], ["recovery", "action", "--scoped-json"]])
        self.assertEqual(json.loads(self.native_calls("recovery")[0]["stdin"]), {**payload, "name": SESSION})
        await self.assert_replays(request, result, process)
        for status, state, error in (("cancelled", "completed", None), ("failed", "failed", "Synthetic native refusal"),
                                     ("uncertain", "uncertain", "interrupted")):
            with self.subTest(status):
                request = self.recovery("cancel")
                fields = {"status": status} if error is None else {"status": status, "error": error}
                self.script(self.fresh(self.job(request["payload"])), self.acknowledgement(request["payload"], **fields))
                result = await self.submit(request, process)
                self.assertEqual((result["state"], result["error"], result["result"]["status"]), (state, error, status))
                await self.assert_replays(request, result, process)

    async def test_a_changed_job_or_a_mismatched_acknowledgement_never_repeats(self):
        process = await self.launch()
        changes = [lambda job: self.fresh({**job, "identity": job["identity"][:2]}),
                   lambda job: self.fresh({**job, "identity": ["replacement-run", *job["identity"][1:]]}),
                   lambda job: self.fresh({**job, "state": "running"}),
                   lambda job: self.fresh({**job, "id": str(uuid.uuid4())}),
                   lambda job: self.fresh(None),
                   lambda job: self.fresh(job, run_id="replacement-run"),
                   lambda job: self.fresh(job, conversation_id="other"),
                   lambda job: self.fresh(job, name="other"),
                   lambda job: self.fresh(job, archive_id=str(uuid.uuid4()))]
        for change in changes:
            request = self.recovery()
            fresh = change(self.job(request["payload"]))
            with self.subTest(fresh):
                self.script(fresh, self.acknowledgement(request["payload"]))
                before = len(self.native_calls())
                self.assertEqual(await self.submit(request, process), self.failed(JOB_CHANGED))
                self.assertEqual([call["argv"][0] for call in self.native_calls()[before:]], ["inspect"])
        for fields in ({"job_id": str(uuid.uuid4())}, {"action": "cancel"}, {"request_id": str(uuid.uuid4())},
                       {"name": "other"}, {"run_id": "replacement-run"}, {"status": "done"}):
            request = self.recovery()
            with self.subTest(fields):
                self.script(self.fresh(self.job(request["payload"])), self.acknowledgement(request["payload"], **fields))
                receipt = await self.submit(request, process)
                self.assertEqual(receipt, self.uncertain("native recovery acknowledgement identity did not match"))
                await self.assert_replays(request, receipt, process)
        self.assertEqual(len(self.native_calls("recovery")), 6)

    async def test_hgs_without_the_recovery_abi_runs_nothing(self):
        request = submission("control", "valid/recovery_action")
        self.hgs.script(self.rules({"argv": ["inspect", SESSION, "--skip-processes"],
                                    "stdout": self.fresh(self.job(request["payload"]))}, markers=()))
        process = await self.launch()
        self.assertNotIn("recovery_action",
                         self.relay.heartbeats()[-1].body["snapshot"]["mobile_capabilities"]["operations"])
        self.assertEqual(await self.submit(request, process), self.failed("native scoped recovery ABI is unavailable"))
        self.assertEqual(self.native_calls(), [])


if __name__ == "__main__":
    unittest.main()
