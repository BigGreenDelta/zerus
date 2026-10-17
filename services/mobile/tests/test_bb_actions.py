"""Black-box session actions: pause, resume, rename, fork, archive, restore,
forget and terminate.

An action reaches `hgs session-action` once with its scoped payload, and its
receipt carries the native status; an acknowledgement of another request,
session, run, conversation or archive, or an unknown status, is uncertain
and never repeated. A computer whose hgs lacks the ABI advertises nothing and
runs nothing.
"""
import json
import unittest
import uuid

from session_harness import (ARCHIVE, CONVERSATION, RUN, SESSION, SessionCase, ack, advertised, reasons,
                             submission)

ACTIONS = ("valid/archive", "valid/forget", "valid/fork", "valid/pause", "valid/rename", "valid/resume",
           "valid/terminate", "valid/restore", "rename/archive")
MISMATCH = "native lifecycle acknowledgement identity or status did not match"
NONARCHIVED = "session must match an exact nonarchived snapshot entry"


def native_result(request: dict, **fields) -> dict:
    """`ack()` as scripted hgs answers it for this request."""
    return {"request_id": request["request_id"], "name": request["session"], "run_id": RUN,
            "conversation_id": CONVERSATION, **fields}


class SessionActionTests(SessionCase):
    markers = ("lifecycle", "recovery")

    def action(self, **fields) -> dict:
        return {"prefix": ["session-action", SESSION, "--json"], "stdout": ack(**fields)}

    async def test_each_action_reaches_native_once_with_its_scoped_payload(self):
        self.hgs.script(self.rules(self.action(status="completed")))
        process = await self.launch()
        capabilities = self.relay.heartbeats()[-1].body["snapshot"]["mobile_capabilities"]
        self.assertEqual(capabilities["operations"], advertised("lifecycle", "recovery"))
        self.assertEqual(capabilities["reasons"], reasons("lifecycle", "recovery"))
        for name in ACTIONS:
            with self.subTest(name):
                request = submission("control", name)
                payload = request["payload"]
                before = len(self.native_calls())
                result = await self.submit(request, process)
                expected = native_result(request, status="completed")
                if "archive_id" in payload:
                    expected["archive_id"] = payload["archive_id"]
                self.assertEqual(result, {"state": "completed", "result": expected, "error": None})
                [call] = self.native_calls()[before:]
                self.assertEqual(call["argv"], ["session-action", SESSION, "--json"])
                self.assertEqual(json.loads(call["stdin"]), {**payload, "action": request["operation"]})
                await self.assert_replays(request, result, process)

    async def test_a_renamed_target_can_be_inspected_before_the_next_heartbeat(self):
        request = submission("control", "valid/rename")
        renamed = request["payload"]["new_name"]
        target = {"name": renamed, "run_id": RUN, "conversation_id": CONVERSATION}
        inspection = {**target, "runtime_state": "live", "process_state": "running", "events": []}
        self.hgs.script(self.rules(self.action(status="completed", result_target=target),
                                   {"argv": ["inspect", renamed], "stdout": inspection}))
        # No heartbeat refreshes the local session list during this test.
        process = await self.launch(self.config(poll_interval=60))
        self.assertEqual(await self.submit(self.envelope("inspect", {}, session=renamed, identity=False), process),
                         self.failed(NONARCHIVED))
        self.assertEqual((await self.submit(request, process))["result"]["result_target"], target)
        found = await self.submit(self.envelope("inspect", {}, session=renamed, identity=False), process)
        self.assertEqual(found, {"state": "completed", "result": inspection, "error": None})
        self.assertEqual(await self.submit(self.envelope("inspect", {}, identity=False), process),
                         self.failed(NONARCHIVED))
        self.assertEqual(len(self.relay.heartbeats()), 1)

    async def test_native_failed_and_uncertain_statuses_are_preserved(self):
        process = await self.launch()
        for status in ("failed", "uncertain"):
            with self.subTest(status):
                self.hgs.script(self.rules(self.action(status=status, error="Synthetic native refusal")))
                request = self.envelope("pause", {})
                result = await self.submit(request, process)
                self.assertEqual(result, {"state": status, "error": "Synthetic native refusal",
                                          "result": native_result(request, status=status, error="Synthetic native refusal")})
                await self.assert_replays(request, result, process)
        self.hgs.script(self.rules(self.action(status="failed")))
        request = self.envelope("resume", {})
        self.assertEqual(await self.submit(request, process),
                         {"state": "failed", "error": None, "result": native_result(request, status="failed")})

    async def test_mismatched_acknowledgements_are_uncertain_and_never_repeated(self):
        process = await self.launch()
        target = {"name": SESSION, "run_id": RUN, "conversation_id": CONVERSATION}
        cases = [({"status": "started"}, MISMATCH), ({"status": "completed", "run_id": "replacement-run"}, MISMATCH),
                 ({"status": "completed", "conversation_id": "other"}, MISMATCH),
                 ({"status": "completed", "name": "other"}, MISMATCH),
                 ({"status": "completed", "request_id": str(uuid.uuid4())}, MISMATCH),
                 ({"status": "completed", "result_target": SESSION}, "native lifecycle result target was invalid"),
                 ({"status": "completed", "result_target": {**target, "name": ""}}, "native lifecycle result target was invalid"),
                 ({"status": "completed", "result_target": {**target, "path": "/example"}},
                  "native lifecycle result target was invalid"),
                 ({"status": "completed", "result_target": {**target, "run_id": None}},
                  "native lifecycle result target was invalid")]
        for fields, message in cases:
            with self.subTest(fields):
                self.hgs.script(self.rules(self.action(**fields)))
                request = self.envelope("pause", {})
                receipt = await self.submit(request, process)
                self.assertEqual(receipt, self.uncertain(message))
                await self.assert_replays(request, receipt, process)
        self.hgs.script(self.rules(self.action(status="completed", archive_id=str(uuid.uuid4()))))
        request = self.envelope("restore", {"archive_id": ARCHIVE})
        self.assertEqual(await self.submit(request, process),
                         self.uncertain("native lifecycle archive identity did not match"))
        self.hgs.script(self.rules({"prefix": ["session-action"], "stdout": [native_result(request)]}))
        self.assertEqual(await self.submit(self.envelope("pause", {}), process), self.uncertain(MISMATCH))

    async def test_restore_targets_only_the_exact_fresh_archive(self):
        target = {"name": SESSION, "run_id": "restored-run", "conversation_id": CONVERSATION}
        self.hgs.script(self.rules(self.action(status="completed", result_target=target)))
        process = await self.launch()
        request = submission("control", "valid/restore")
        result = await self.submit(request, process)
        self.assertEqual(result["result"], {**native_result(request, status="completed", result_target=target),
                                            "archive_id": ARCHIVE})
        unknown = self.envelope("restore", {"archive_id": str(uuid.uuid4())})
        self.assertEqual(await self.submit(unknown, process),
                         self.failed("archive must match a fresh exact local snapshot entry"))
        self.assertEqual(len(self.native_calls()), 1)

    async def test_hgs_without_the_session_action_abi_advertises_and_runs_nothing(self):
        self.hgs.script(self.rules(self.action(status="completed"), markers=()))
        self.hgs.prepend({"argv": ["--help"], "stdout_text": "Usage: hgs session-action and recovery action\n"})
        process = await self.launch()
        capabilities = self.relay.heartbeats()[-1].body["snapshot"]["mobile_capabilities"]
        self.assertEqual((capabilities["operations"], capabilities["reasons"]), (advertised(), reasons()))
        self.assertEqual(await self.submit(self.envelope("pause", {}), process),
                         self.failed("native scoped lifecycle ABI is unavailable"))
        self.assertEqual(await self.submit(submission("control", "valid/recovery_action"), process),
                         self.failed("native scoped recovery ABI is unavailable"))
        self.assertEqual(self.native_calls(), [])
        self.assertEqual({tuple(call["argv"]) for call in self.hgs.calls("--help")}, {("--help",)})


if __name__ == "__main__":
    unittest.main()
