"""Synthetic process tests for explicit chooser dismissal before context clear.

The additive feature gates the presence of dismiss_prompt, including false.
Only true selects the dedicated fresh native eligibility; all identities and
the original flag reach one fixed native command, and durable outcomes never
grant another attempt. Native hgs, agents and providers are never invoked.
"""
import json
import signal
import unittest
import uuid

from connector_harness import IMPLEMENTATION, NATIVE_TIMEOUT_HOOK, RESTART_ERROR
from session_harness import ARCHIVE, CONVERSATION, RUN, SESSION, SessionCase, ack, help_text, session_listing

FEATURE = "clear_context_dismiss"
MARKER = "Mobile clear-context ABI: dismiss-prompt-v1"
INSPECT = ["inspect", SESSION]
CLEAR = ["clear-context", SESSION, "--json"]
COMPACT = ["compact-context", SESSION, "--json"]
UNAVAILABLE = "native clear-context dismiss ABI is unavailable"
INELIGIBLE = "native context operation is unavailable"
IDENTITY = "fresh native context identity did not match"
MISMATCH = "native context acknowledgement identity or status did not match"
FIELDS = "context control requires exact request/run/conversation identity"
_ABSENT = object()


def detail(**changes):
    return {"name": SESSION, "run_id": RUN, "conversation_id": CONVERSATION,
            "runtime_state": "live", "process_state": "running", "phase": "waiting", "activity": "waiting",
            "clear_context_supported": False, "clear_context_dismiss": True,
            "compact_context_supported": True, **changes}


def receipt(state, error):
    # New feature errors need not be available in the historical receipt vectors.
    return {"state": state, "result": None, "error": error}


class DismissPromptTests(SessionCase):
    markers = (FEATURE,)

    def clear(self, flag=_ABSENT, **fields):
        if flag is not _ABSENT:
            fields = {"dismiss_prompt": flag, **fields}
        return self.envelope("clear_context", fields)

    def script(self, fresh=_ABSENT, *, enabled=True, help_value=None, native_rule=None, listing_value=None):
        self.hgs.script(self.rules(
            {"argv": INSPECT, "stdout": detail() if fresh is _ABSENT else fresh},
            {"argv": CLEAR, **({"stdout": ack(status="submitted")} if native_rule is None else native_rule)},
            {"argv": COMPACT, "stdout": ack(status="submitted")},
            markers=(FEATURE,) if enabled else (), listing_value=listing_value))
        if help_value is not None:
            self.hgs.prepend({"argv": ["--help"], "stdout_text": help_value})

    def completed(self, request, status="submitted"):
        return {"state": "completed", "error": None, "result": {
            "request_id": request["request_id"], "name": SESSION, "run_id": RUN,
            "conversation_id": CONVERSATION, "status": status}}

    async def delivered(self, process, request):
        await self.until(lambda: (row := self.journal.row(request["request_id"])) and row["delivered"],
                         what="the delivered context-clear journal row", process=process)

    async def conflict(self, request, process, **change):
        changed = {**request, "payload": {**request["payload"], **change}}
        if change.get("dismiss_prompt", _ABSENT) is _ABSENT:
            changed["payload"].pop("dismiss_prompt", None)
        before, mark = len(self.relay.results(request["request_id"])), len(self.relay.deliveries)
        await self.relay.queue(changed, redeliver=True)
        await self.settle(request["request_id"], mark, process)
        self.assertEqual(len(self.relay.results(request["request_id"])), before)

    async def test_exact_marker_offers_only_the_feature_and_gates_true_and_false_presence(self):
        operations = None
        for name, help_value, enabled in (
                ("old CLI", help_text(), False),
                ("ordinary command help", help_text() + "hgs clear-context <session> --json\n", False),
                ("v10", help_text() + MARKER.replace("-v1", "-v10") + "\n", False),
                ("suffix", help_text() + MARKER + " explanatory suffix\n", False),
                ("prefix", help_text() + "prefix " + MARKER + "\n", False),
                ("exact indented line", help_text() + "       " + MARKER + "  \n", True)):
            with self.subTest(name):
                self.script(detail(clear_context_supported=True), enabled=False, help_value=help_value)
                process = await self.launch()
                capabilities = self.relay.heartbeats()[-1].body["snapshot"]["mobile_capabilities"]
                self.assertEqual(FEATURE in capabilities["features"], enabled)
                self.assertIn("clear_context", capabilities["operations"])
                self.assertIn("compact_context", capabilities["operations"])
                if operations is None:
                    operations = capabilities["operations"]
                self.assertEqual(capabilities["operations"], operations)
                for flag in (True, False):
                    before = len(self.native_calls())
                    request = self.clear(flag)
                    self.assertEqual(await self.submit(request, process), self.completed(request) if enabled
                                     else receipt("failed", UNAVAILABLE))
                    self.assertEqual([call["argv"] for call in self.native_calls()[before:]],
                                     [INSPECT, CLEAR] if enabled else [])
                process.stop()

    async def test_absent_and_false_use_the_ordinary_fixed_command_and_payload(self):
        for enabled, flag in ((False, _ABSENT), (True, _ABSENT), (True, False)):
            with self.subTest(enabled=enabled, flag="absent" if flag is _ABSENT else flag):
                self.script(detail(clear_context_supported=True, clear_context_dismiss=False,
                                   phase="idle", activity="idle"), enabled=enabled)
                process = await self.launch()
                request = self.clear(flag)
                before = len(self.native_calls())
                self.assertEqual(await self.submit(request, process), self.completed(request))
                [inspected, cleared] = self.native_calls()[before:]
                self.assertEqual((inspected["argv"], inspected["stdin"]), (INSPECT, ""))
                self.assertEqual((cleared["argv"], json.loads(cleared["stdin"])), (CLEAR, request["payload"]))
                self.assertEqual(set(request["payload"]), {"request_id", "expected_run_id", "expected_conversation_id"}
                                 | ({"dismiss_prompt"} if flag is not _ABSENT else set()))
                await self.assert_replays(request, self.completed(request), process)
                process.stop()

    async def test_true_uses_dedicated_boolean_eligibility_in_a_waiting_chooser(self):
        for status in ("submitted", "confirmed"):
            with self.subTest(status):
                self.script(detail(clear_context_supported=False, phase="waiting", activity="waiting"),
                            native_rule={"stdout": ack(status=status)})
                process = await self.launch()
                request = self.clear(True)
                before = len(self.native_calls())
                result = await self.submit(request, process)
                self.assertEqual(result, self.completed(request, status))
                [inspected, cleared] = self.native_calls()[before:]
                self.assertEqual((inspected["argv"], inspected["stdin"]), (INSPECT, ""))
                self.assertEqual((cleared["argv"], json.loads(cleared["stdin"])), (CLEAR, request["payload"]))
                self.assertIs(json.loads(cleared["stdin"])["dismiss_prompt"], True)
                await self.assert_replays(request, result, process)
                process.stop()

    async def test_missing_false_and_nonboolean_eligibility_never_infer_authority_from_a_prompt(self):
        self.script()
        process = await self.launch()
        for value in (_ABSENT, False, "true", 1, 0, None, [], {}):
            with self.subTest(value="absent" if value is _ABSENT else value):
                fresh = detail(clear_context_supported=True, clear_context_dismiss=value,
                               phase="question", activity="waiting",
                               attention={"source": "claude-question-hook", "kind": "approval"})
                if value is _ABSENT:
                    del fresh[FEATURE]
                self.script(fresh)
                before = len(self.native_calls())
                self.assertEqual(await self.submit(self.clear(True), process), receipt("failed", INELIGIBLE))
                self.assertEqual([call["argv"] for call in self.native_calls()[before:]], [INSPECT])
        # A false flag keeps ordinary eligibility even when dismiss eligibility is true.
        self.script(detail(clear_context_supported=False, clear_context_dismiss=True))
        self.assertEqual(await self.submit(self.clear(False), process), receipt("failed", INELIGIBLE))
        self.assertEqual(self.native_calls("clear-context"), [])

    async def test_fresh_inspection_overrides_snapshot_eligibility_hints(self):
        for hint, fresh_allowed in ((True, False), (False, True)):
            with self.subTest(hint=hint, fresh=fresh_allowed):
                listing = session_listing()
                for row in listing["sessions"]:
                    if row["name"] == SESSION:
                        row[FEATURE] = hint
                        row["clear_context_supported"] = hint
                self.script(detail(clear_context_dismiss=fresh_allowed), listing_value=listing)
                process = await self.launch()
                before = len(self.native_calls())
                request = self.clear(True)
                self.assertEqual(await self.submit(request, process), self.completed(request) if fresh_allowed
                                 else receipt("failed", INELIGIBLE))
                self.assertEqual([call["argv"] for call in self.native_calls()[before:]],
                                 [INSPECT, CLEAR] if fresh_allowed else [INSPECT])
                process.stop()

    async def test_exact_identity_and_archive_checks_precede_dismiss_eligibility(self):
        self.script()
        process = await self.launch()
        # Native archived inspection supplies state/archive_id; the existing
        # archive predicate does not derive authority from runtime labels.
        cases = [detail(name="claude/other"), detail(run_id="replacement-run"),
                 detail(conversation_id="replacement-conversation"), detail(archive_id=ARCHIVE),
                 detail(state="archived"),
                 detail(name=None), detail(run_id=None), detail(conversation_id=None), [], None]
        for fresh in cases:
            for eligible in (False, True):
                with self.subTest(fresh=fresh, eligible=eligible):
                    native = {**fresh, FEATURE: eligible} if isinstance(fresh, dict) else fresh
                    self.script(native)
                    before = len(self.native_calls())
                    self.assertEqual(await self.submit(self.clear(True), process), receipt("failed", IDENTITY))
                    self.assertEqual([call["argv"] for call in self.native_calls()[before:]], [INSPECT])
        self.assertEqual(self.native_calls("clear-context"), [])

    async def test_flag_types_other_operations_and_wrong_targets_fail_before_native_work(self):
        self.script()
        process = await self.launch()
        cases = [(self.clear(value), "dismiss_prompt must be a boolean")
                 for value in (None, 0, 1, 0.0, 1.0, "true", "false", [], {})]
        cases += [(self.envelope("compact_context", {"dismiss_prompt": value}), FIELDS) for value in (True, False, None)]
        cases += [(self.clear(True, agent_id="worker-1"), FIELDS),
                  (self.clear(True, extra=False), FIELDS),
                  (self.clear(True, archive_id=ARCHIVE), "archived targets support inspection only"),
                  ({**self.clear(True), "session": "claude/other"}, "session must match an exact nonarchived snapshot entry"),
                  (self.clear(True, expected_conversation_id=""), "context control requires nonempty bounded native identity"),
                  (self.clear(True, expected_run_id=""), "expected_run_id is required")]
        for key, error in (("request_id", "payload request_id must match request identity"),
                           ("expected_run_id", "expected_run_id is required"),
                           ("expected_conversation_id", FIELDS)):
            request = self.clear(True)
            del request["payload"][key]
            cases.append((request, error))
        request = self.clear(True)
        request["payload"]["request_id"] = str(uuid.uuid4())
        cases.append((request, "payload request_id must match request identity"))
        for request, error in cases:
            with self.subTest(operation=request["operation"], payload=request["payload"], session=request["session"]):
                self.assertEqual(await self.submit(request, process), receipt("failed", error))
        self.assertEqual(self.native_calls(), [])

    async def test_duplicate_replays_before_marker_and_eligibility_gates_and_preserves_the_flag_hash(self):
        self.script()
        first = await self.launch()
        request = self.clear(True)
        result = await self.submit(request, first)
        self.assertEqual(result, self.completed(request))
        await self.delivered(first, request)
        original = self.journal.row(request["request_id"])
        first.stop()
        self.script(detail(name="claude/other", clear_context_dismiss=False), enabled=False)
        second = await self.launch()
        before = len(self.native_calls())
        await self.assert_replays(request, result, second)
        for flag in (True, False):
            self.assertEqual(await self.submit(self.clear(flag), second), receipt("failed", UNAVAILABLE))
        self.assertEqual(len(self.native_calls()), before)
        for change in ({"dismiss_prompt": False}, {"dismiss_prompt": _ABSENT},
                       {"dismiss_prompt": True, "expected_run_id": "replacement-run"}):
            await self.conflict(request, second, **change)
        second.stop()
        self.script(detail(clear_context_dismiss=False))
        third = await self.launch()
        await self.assert_replays(request, result, third)
        self.assertEqual(await self.submit(self.clear(True), third), receipt("failed", INELIGIBLE))
        preserved = self.journal.row(request["request_id"])
        for key in ("request_id", "operation", "request_hash", "created_at", "response"):
            self.assertEqual(preserved[key], original[key], key)
        self.assertEqual(len(self.native_calls("clear-context")), 1)
        self.assertIs(json.loads(self.native_calls("clear-context")[0]["stdin"])["dismiss_prompt"], True)

    async def test_unknown_native_outcomes_stay_uncertain_and_replay_after_restart(self):
        self.script()
        first = await self.launch()
        requests = []
        for name, rule, error in (
                ("wrong request", {"stdout": ack(status="submitted", request_id=str(uuid.uuid4()))}, MISMATCH),
                ("wrong conversation", {"stdout": ack(status="confirmed", conversation_id="other")}, MISMATCH),
                ("unknown status", {"stdout": ack(status="completed")}, MISMATCH),
                ("not an object", {"stdout": []}, MISMATCH),
                ("native failure", {"stdout_text": "BB-PRIVATE-UNKNOWN", "stderr": "BB-PRIVATE-ERROR", "exit": 4},
                 "native command failed"),
                ("invalid JSON", {"stdout_text": '{"status":'}, "native command returned invalid JSON")):
            with self.subTest(name):
                self.script(native_rule=rule)
                request = self.clear(True)
                result = await self.submit(request, first)
                self.assertEqual(result, receipt("uncertain", error))
                await self.assert_replays(request, result, first)
                requests.append((request, result))
        first.stop()
        self.script(detail(clear_context_dismiss=False), enabled=False)
        second = await self.launch()
        before = len(self.native_calls())
        for request, result in requests:
            await self.assert_replays(request, result, second)
        self.assertEqual(len(self.native_calls()), before)
        self.assertEqual(len(self.native_calls("clear-context")), len(requests))

    async def test_crash_after_native_clear_recovers_uncertain_without_another_clear(self):
        self.script(native_rule={"stdout": ack(status="submitted"), "kill_parent": True})
        first = await self.launch()
        request = self.clear(True)
        await self.relay.queue(request)
        self.assertEqual(await first.wait_exit(10), -signal.SIGKILL)
        original = self.journal.row(request["request_id"])
        self.assertEqual(original["state"], "running")
        [call] = self.native_calls("clear-context")
        self.assertEqual(json.loads(call["stdin"]), request["payload"])
        self.script(detail(clear_context_dismiss=False), enabled=False)
        second = await self.launch()
        result = receipt("uncertain", RESTART_ERROR)
        self.assertEqual(await self.until(lambda: self.relay.results(request["request_id"]),
                                          what="the recovered context-clear receipt", process=second), [result])
        await self.assert_replays(request, result, second)
        self.assertEqual(self.journal.row(request["request_id"])["request_hash"], original["request_hash"])
        self.assertEqual(len(self.native_calls("clear-context")), 1)

    async def test_an_undelivered_result_survives_restart_and_withdrawn_marker(self):
        self.script()
        request = self.clear(True)
        refusal = self.relay.rule("POST", rf"/v1/node/requests/{request['request_id']}/result", 503)
        first = await self.launch()
        await self.relay.queue(request)
        [(result, _)] = await self.until(lambda: self.relay.uploads(request["request_id"])[:1],
                                         what="the refused context-clear upload", process=first)
        self.assertEqual(result, self.completed(request))
        original = self.journal.row(request["request_id"])
        first.stop()
        self.relay.rules.remove(refusal)
        self.script(detail(clear_context_dismiss=False), enabled=False)
        second = await self.launch()
        self.assertEqual(await self.until(lambda: self.relay.results(request["request_id"]),
                                          what="the unchanged context-clear upload", process=second), [result])
        await self.assert_replays(request, result, second)
        self.assertEqual(self.journal.row(request["request_id"])["request_hash"], original["request_hash"])
        self.assertEqual(len(self.native_calls("clear-context")), 1)

    async def test_rust_native_timeout_is_uncertain_and_never_retries_the_original_flag(self):
        if IMPLEMENTATION != "rust":
            self.skipTest("Rust-only native-timeout hook; Python's unchanged default native budget is 45 seconds")
        self.script(native_rule={"stdout": ack(status="submitted"), "delay": 60})
        first = await self.launch(env={NATIVE_TIMEOUT_HOOK: "1"})
        request = self.clear(True)
        result = await self.submit(request, first, timeout=15)
        self.assertEqual(result, receipt("uncertain", "native command timed out"))
        [call] = self.native_calls("clear-context")
        self.assertEqual(json.loads(call["stdin"]), request["payload"])
        self.assertIsNone(self.hgs.finished(call["pid"]))
        await self.assert_replays(request, result, first)
        first.stop()
        self.script(detail(clear_context_dismiss=False), enabled=False)
        second = await self.launch()
        await self.assert_replays(request, result, second)
        self.assertEqual(len(self.native_calls("clear-context")), 1)


if __name__ == "__main__":
    unittest.main()
