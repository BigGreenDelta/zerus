"""Black-box session controls: the input queue, per-session settings,
background processes, scoped inspection, shared read marks, the phone
delivery setting and voice messages.

Each control checks fresh native state where Python does, reaches its fixed
native command once, and keeps only an acknowledgement of exactly this
request; a mismatched one is uncertain for a mutation and failed for a read,
and is never repeated. A computer without the native ABI advertises and runs
nothing.
"""
import json
import unittest
import uuid

from connector_harness import stopped_journal
from session_harness import (ARCHIVE, CONVERSATION, OPERATIONS, RUN, SESSION, SessionCase, ack, session_listing,
                             submission)

QUEUE = submission("conversation", "valid/send_now")["payload"]["queue_id"]
SIGNATURE = submission("conversation", "valid/mark_read")["payload"]["attention_signature"]
CONTEXT_MISMATCH = "native context acknowledgement identity or status did not match"
READ_MISMATCH = "native read acknowledgement did not match"
VOICE_MISMATCH = "native voice acknowledgement did not match"


def detail(**changes) -> dict:
    return {"name": SESSION, "run_id": RUN, "conversation_id": CONVERSATION, "settings_change_supported": True,
            "input_queue": {"id": QUEUE, "can_send_now": True}, "subagents": {"child": {"state": "completed"}},
            "runtime_state": "live", "process_state": "running", "events": [], **changes}


def read_receipt(**changes) -> dict:
    return {"ok": True, "applied": {"reply_id": True, "attention_signature": True},
            "read": {"conversation_id": CONVERSATION, "reply_id": "7:100.5", "attention_signature": SIGNATURE, "at": 1.0},
            **changes}


def voice_message(identity: str = "{{stdin.request_id}}", **changes) -> dict:
    return {"schema": 1, "message": {"id": identity, "run_id": RUN, "conversation_id": CONVERSATION, "revision": 1,
                                     "state": "queued", **changes}}


class QueueAndSettingsTests(SessionCase):
    def script(self, fresh: dict, *rules: dict) -> None:
        self.hgs.script(self.rules({"argv": ["inspect", SESSION], "stdout": fresh}, *rules))

    async def test_send_now_sends_only_the_exact_fresh_queue(self):
        request = submission("conversation", "valid/send_now")
        self.script(detail(), {"prefix": ["send-now"], "stdout": ack(status="submitted", queue_id=QUEUE)})
        process = await self.launch()
        result = await self.submit(request, process)
        self.assertEqual(result["state"], "completed")
        self.assertEqual([call["argv"] for call in self.native_calls()],
                         [["inspect", SESSION], ["send-now", SESSION, "--json"]])
        self.assertEqual(json.loads(self.native_calls("send-now")[0]["stdin"]), request["payload"])
        await self.assert_replays(request, result, process)
        for fresh in (detail(input_queue={"id": "b" * 64, "can_send_now": True}),
                      detail(input_queue={"id": QUEUE, "can_send_now": False}), detail(input_queue=None)):
            with self.subTest(fresh["input_queue"]):
                self.script(fresh, {"prefix": ["send-now"], "stdout": ack(status="submitted", queue_id=QUEUE)})
                self.assertEqual(await self.submit(self.envelope("send_now", {"queue_id": QUEUE}), process),
                                 self.failed("native queue changed or cannot be sent now"))
        for fields, message in (({"status": "submitted", "queue_id": "b" * 64}, "native queue acknowledgement identity did not match"),
                                ({"status": "applied", "queue_id": QUEUE}, CONTEXT_MISMATCH)):
            with self.subTest(fields):
                self.script(detail(), {"prefix": ["send-now"], "stdout": ack(**fields)})
                request = self.envelope("send_now", {"queue_id": QUEUE})
                receipt = await self.submit(request, process)
                self.assertEqual(receipt, self.uncertain(message))
                await self.assert_replays(request, receipt, process)
        self.assertEqual(len(self.native_calls("send-now")), 3)

    async def test_settings_apply_or_schedule_only_the_exact_change(self):
        process = await self.launch()
        pending = str(uuid.uuid4())
        accepted = [(submission("conversation", "valid/settings"), {"status": "applied", "model": "fixture-model"}),
                    (submission("conversation", "settings/effort-high"), {"status": "applied", "effort": "high"}),
                    (self.envelope("settings", {"model": "fixture-model", "effort": ""}),
                     {"status": "scheduled", "model": "fixture-model", "effort": "", "pending_settings_id": pending})]
        for request, fields in accepted:
            with self.subTest(fields):
                self.script(detail(), {"prefix": ["settings"], "stdout": ack(**fields)})
                result = await self.submit(request, process)
                self.assertEqual((result["state"], result["result"]["status"]), ("completed", fields["status"]))
                self.assertEqual(self.native_calls()[-1]["argv"], ["settings", SESSION, "--json"])
                self.assertEqual(json.loads(self.native_calls()[-1]["stdin"]), request["payload"])
        uncertain = [({"status": "scheduled", "model": "fixture-model"}, "request_id must be a canonical UUID"),
                     ({"status": "scheduled", "model": "fixture-model", "pending_settings_id": pending.upper()},
                      "request_id must be a canonical UUID"),
                     ({"status": "applied", "model": "other"}, "native settings acknowledgement did not match"),
                     ({"status": "submitted", "model": "fixture-model"}, CONTEXT_MISMATCH)]
        for fields, message in uncertain:
            with self.subTest(fields):
                self.script(detail(), {"prefix": ["settings"], "stdout": ack(**fields)})
                request = self.envelope("settings", {"model": "fixture-model"})
                receipt = await self.submit(request, process)
                self.assertEqual(receipt, self.uncertain(message))
                await self.assert_replays(request, receipt, process)
        before = len(self.native_calls("settings"))
        pending_request = submission("conversation", "settings/pending")
        for fresh, message in ((detail(pending_settings_id="other"), "native pending setting changed; refresh before applying"),
                               (detail(settings_change_supported=False), "native per-session settings are unavailable"),
                               (detail(settings_change_supported="true"), "native per-session settings are unavailable")):
            with self.subTest(message, fresh=fresh):
                self.script(fresh, {"prefix": ["settings"], "stdout": ack(status="applied", model="fixture-model")})
                request = pending_request if "pending_settings_id" in fresh else self.envelope("settings", {"effort": "low"})
                self.assertEqual(await self.submit(request, process), self.failed(message))
        self.assertEqual(len(self.native_calls("settings")), before)

    async def test_a_model_name_is_bounded_by_its_utf8_bytes_like_a_native_launch(self):
        self.script(detail(), {"prefix": ["settings"], "stdout": ack(status="applied", model="{{stdin.model}}")})
        process = await self.launch()
        for model in ("ollama/" + "a" * 249, "ollama/" + "界" * 83):
            with self.subTest(len(model.encode())):
                result = await self.submit(self.envelope("settings", {"model": model}), process)
                self.assertEqual((result["state"], result["result"]["model"]), ("completed", model))
        for model, message in (("ollama/" + "a" * 250, "invalid bounded native identity"),
                               ("ollama/" + "界" * 84, "native model identifier exceeds UTF-8 byte limit")):
            with self.subTest(len(model.encode())):
                self.assertEqual(await self.submit(self.envelope("settings", {"model": model}), process),
                                 self.failed(message))
        self.assertEqual(len(self.native_calls("settings")), 2)


class ProcessAndInspectionTests(SessionCase):
    async def test_process_commands_use_fixed_arguments_and_exact_acknowledgements(self):
        native = {"id": "job", "run_id": RUN, "conversation_id": CONVERSATION, "status": "requested", "output": "line\n"}
        self.hgs.script(self.rules({"prefix": ["processes"], "stdout": native}))
        process = await self.launch()
        fixed = ["--run", RUN, "--conversation", CONVERSATION]
        for name, argv in (("process_output/generation", ["--output", "job", *fixed, "--generation", "g:1"]),
                           ("valid/process_stop", ["--stop", "job", *fixed]),
                           ("process_output/archive", ["--output", "job", *fixed, "--archive", ARCHIVE])):
            with self.subTest(name):
                request = submission("conversation", name)
                result = await self.submit(request, process)
                expected = {**native, "request_id": request["request_id"], "name": SESSION, "process_id": "job"}
                if "archive_id" in request["payload"]:
                    expected["archive_id"] = ARCHIVE
                self.assertEqual(result, {"state": "completed", "result": expected, "error": None})
                call = self.native_calls()[-1]
                self.assertEqual((call["argv"], call["stdin"]), (["processes", SESSION, *argv], ""))
                await self.assert_replays(request, result, process)
        for changes in ({"id": "other"}, {"run_id": "replacement-run"}, {"conversation_id": "other"}):
            self.hgs.script(self.rules({"prefix": ["processes"], "stdout": {**native, **changes}}))
            for operation, state in (("process_output", "failed"), ("process_stop", "uncertain")):
                with self.subTest(operation, changes=changes):
                    request = self.envelope(operation, {"process_id": "job"})
                    receipt = await self.submit(request, process)
                    self.assertEqual(receipt, {"state": state, "result": None,
                                               "error": "native process acknowledgement identity did not match"})
                    await self.assert_replays(request, receipt, process)

    async def test_scoped_and_child_inspection_check_the_exact_identity(self):
        child = {"name": SESSION, "run_id": RUN, "agent_id": "child", "parent_conversation_id": CONVERSATION,
                 "conversation_id": CONVERSATION + "/child"}
        child_argv = ["inspect", SESSION, "--agent", "child", "--skip-processes"]

        def script(parent, child_value=child, after=detail()):
            self.hgs.script(self.rules({"argv": ["inspect", SESSION], "stdout": parent},
                                       {"argv": child_argv, "stdout": child_value},
                                       {"argv": ["inspect", SESSION, "--after", "5"], "stdout": after}))

        script(detail())
        process = await self.launch()
        incremental = await self.submit(submission("conversation", "inspect/after"), process)
        self.assertEqual(incremental, {"state": "completed", "result": detail(), "error": None})
        self.assertEqual(self.native_calls()[-1]["argv"], ["inspect", SESSION, "--after", "5"])
        found = await self.submit(submission("conversation", "inspect/agent"), process)
        self.assertEqual(found, {"state": "completed", "result": child, "error": None})
        self.assertEqual([call["argv"] for call in self.native_calls()[-2:]], [["inspect", SESSION], child_argv])
        refusals = [(detail(subagents={}), child, "subagent does not belong to the exact selected parent", 1),
                    (detail(run_id="replacement-run"), child, "subagent does not belong to the exact selected parent", 1),
                    (detail(), {**child, "parent_conversation_id": "other"},
                     "native inspection run/conversation identity did not match", 2),
                    (detail(), {**child, "conversation_id": "other"}, "native subagent inspection identity did not match", 2),
                    (detail(), {**child, "agent_id": "other"}, "native subagent inspection identity did not match", 2)]
        for parent, child_value, message, calls in refusals:
            with self.subTest(message, parent=parent, child=child_value):
                script(parent, child_value)
                before = len(self.native_calls())
                request = self.envelope("inspect", {"agent_id": "child", "expected_run_id": RUN,
                                                    "expected_conversation_id": CONVERSATION}, identity=False)
                self.assertEqual(await self.submit(request, process), self.failed(message))
                self.assertEqual(len(self.native_calls()) - before, calls)
        for parent, message in ((detail(run_id="replacement-run"), "native inspection run/conversation identity did not match"),
                                (detail(archive_id=str(uuid.uuid4())),
                                 "native ordinary inspection returned a different or archived target"),
                                (detail(name="other"), "native ordinary inspection returned a different or archived target")):
            with self.subTest(message, parent=parent):
                script(parent)
                self.assertEqual(await self.submit(submission("conversation", "inspect/identity-only")
                                                   | {"request_id": str(uuid.uuid4())}, process), self.failed(message))


class MarkReadTests(SessionCase):
    markers = ("read",)

    def script(self, receipt, code: int = 0) -> None:
        self.hgs.script(self.rules({"argv": ["read", SESSION, "--json"], "stdout": receipt, "exit": code}))

    def mark(self, **changes) -> dict:
        payload = {**submission("conversation", "valid/mark_read")["payload"], **changes}
        return self.envelope("mark_read", payload, identity=False)

    async def test_mark_read_returns_the_native_receipt_once(self):
        self.script(read_receipt())
        process = await self.launch()
        self.assertIn("mark_read", self.relay.heartbeats()[-1].body["snapshot"]["mobile_capabilities"]["operations"])
        request = submission("conversation", "valid/mark_read")
        result = await self.submit(request, process)
        self.assertEqual(result, {"state": "completed", "result": read_receipt(), "error": None})
        [call] = self.native_calls()
        self.assertEqual(call["argv"], ["read", SESSION, "--json"])
        self.assertEqual(json.loads(call["stdin"]), request["payload"])
        await self.assert_replays(request, result, process)
        stale = read_receipt(applied={"reply_id": False, "attention_signature": False}, read=None)
        for receipt, code in ((stale, 0), ({"ok": False, "error": "not_found"}, 0), ({"ok": False, "error": "not_found"}, 1)):
            with self.subTest(receipt, exit=code):
                self.script(receipt, code)
                self.assertEqual(await self.submit(self.mark(reply_id="6:90.5", attention_signature=None), process),
                                 {"state": "completed", "result": receipt, "error": None})

    async def test_only_a_native_read_receipt_of_this_conversation_completes(self):
        process = await self.launch()
        failures = [(read_receipt(), 1, "native command failed"), ({"ok": False}, 0, READ_MISMATCH),
                    (read_receipt(read={**read_receipt()["read"], "conversation_id": "other"}), 0, READ_MISMATCH),
                    (read_receipt(applied={"reply_id": 1, "attention_signature": False}), 0, READ_MISMATCH),
                    (read_receipt(applied={"reply_id": True}), 0, READ_MISMATCH),
                    ({"ok": False, "error": "other"}, 0, READ_MISMATCH), ({"ok": False, "error": "other"}, 1, READ_MISMATCH),
                    ({"ok": True}, 0, READ_MISMATCH), (["ok"], 0, READ_MISMATCH)]
        for receipt, code, message in failures:
            with self.subTest(receipt, exit=code):
                self.script(receipt, code)
                request = self.mark()
                receipt_value = await self.submit(request, process)
                self.assertEqual(receipt_value, self.failed(message))
                self.assertEqual(self.journal.row(request["request_id"])["state"], "failed")
        self.hgs.script(self.rules({"argv": ["read", SESSION, "--json"], "stdout_text": "", "exit": 1}))
        self.assertEqual(await self.submit(self.mark(), process), self.failed("native command failed"))

    async def test_invalid_marks_or_targets_never_reach_native(self):
        self.script(read_receipt())
        process = await self.launch()
        payload = submission("conversation", "valid/mark_read")["payload"]
        exact = "mark_read requires exactly run, conversation, reply and attention fields"
        bounded, signature = "invalid bounded native identity", "invalid attention signature"
        cases = [(self.mark(reply_id=7), bounded), (self.mark(run_id=""), bounded),
                 (self.mark(conversation_id="x" * 257), bounded), (self.mark(conversation_id="bad\nidentity"), bounded),
                 (self.mark(attention_signature="A" * 64), signature), (self.mark(attention_signature="a" * 63), signature),
                 (self.mark(attention_signature=5), signature), (self.mark(request_id=str(uuid.uuid4())), exact),
                 (self.envelope("mark_read", {k: v for k, v in payload.items() if k != "reply_id"}, identity=False), exact),
                 (self.envelope("mark_read", None, identity=False), exact),
                 (self.envelope("mark_read", payload, session="codex/other", identity=False),
                  "session must match an exact nonarchived snapshot entry"),
                 (self.envelope("mark_read", payload, session="@peer", identity=False),
                  "session must match an exact local snapshot entry"),
                 ({**self.mark(), "archive_id": ARCHIVE}, "archive identity belongs only in inspect payload")]
        for request, message in cases:
            with self.subTest(message, payload=request["payload"], session=request["session"]):
                self.assertEqual(await self.outcome(request, process), self.failed(message))
        self.assertEqual(self.native_calls(), [])

    async def test_hgs_without_the_read_abi_advertises_and_runs_nothing(self):
        self.hgs.script(self.rules({"argv": ["read", SESSION, "--json"], "stdout": read_receipt()}, markers=()))
        process = await self.launch()
        self.assertNotIn("mark_read", self.relay.heartbeats()[-1].body["snapshot"]["mobile_capabilities"]["operations"])
        self.assertEqual(await self.submit(self.mark(), process), self.failed("native shared read ABI is unavailable"))
        self.assertEqual(self.native_calls(), [])


class DeliverySettingTests(SessionCase):
    markers = ("preference",)

    def setting(self, value: str) -> dict:
        return self.envelope("set_mobile_delivery", {"value": value}, session="", identity=False)

    async def test_each_value_runs_its_fixed_command_once(self):
        self.hgs.script(self.rules({"prefix": ["swarm", "preference", "set", "mobile_delivery"],
                                    "stdout": {"ok": True, "mobile_delivery": "{{argv.4}}"}}))
        process = await self.launch()
        self.assertIn("set_mobile_delivery", self.relay.heartbeats()[-1].body["snapshot"]["mobile_capabilities"]["operations"])
        requests = [submission("launch", "valid/set_mobile_delivery"),
                    *map(self.setting, OPERATIONS["sequences"]["MOBILE_DELIVERY"])]
        for request in requests:
            value = request["payload"]["value"]
            with self.subTest(value):
                result = await self.submit(request, process)
                self.assertEqual(result, {"state": "completed", "result": {"ok": True, "mobile_delivery": value},
                                          "error": None})
                call = self.native_calls()[-1]
                self.assertEqual((call["argv"], call["stdin"]),
                                 (["swarm", "preference", "set", "mobile_delivery", value, "--json"], ""))
                await self.assert_replays(request, result, process)

    async def test_a_mismatched_acknowledgement_is_uncertain_and_never_repeated(self):
        process = await self.launch()
        for answer in ({"ok": True, "mobile_delivery": "away"}, {"ok": "true", "mobile_delivery": "away_10"},
                       {"mobile_delivery": "away_10"}, ["away_10"]):
            with self.subTest(answer):
                self.hgs.script(self.rules({"prefix": ["swarm", "preference"], "stdout": answer}))
                request = self.setting("away_10")
                receipt = await self.submit(request, process)
                self.assertEqual(receipt, self.uncertain("native delivery setting acknowledgement did not match"))
                await self.assert_replays(request, receipt, process)

    async def test_invalid_values_or_fields_never_reach_native(self):
        self.hgs.script(self.rules({"prefix": ["swarm", "preference"], "stdout": {"ok": True, "mobile_delivery": "away"}}))
        process = await self.launch()
        for payload in ({}, None, ["away"], {"value": "AWAY"}, {"value": 5}, {"value": "away", "request_id": "fixture"}):
            with self.subTest(payload=payload):
                request = self.envelope("set_mobile_delivery", payload, session="", identity=False)
                self.assertEqual(await self.outcome(request, process), self.failed("invalid phone notification timing"))
        self.assertEqual(self.native_calls(), [])

    async def test_hgs_without_the_preference_abi_advertises_and_runs_nothing(self):
        self.hgs.script(self.rules({"prefix": ["swarm", "preference"], "stdout": {"ok": True}}, markers=()))
        process = await self.launch()
        self.assertNotIn("set_mobile_delivery",
                         self.relay.heartbeats()[-1].body["snapshot"]["mobile_capabilities"]["operations"])
        self.assertEqual(await self.submit(self.setting("away"), process),
                         self.failed("the shared delivery setting is changed on the gateway computer with a current hgs"))
        self.assertEqual(self.native_calls(), [])

    async def test_the_snapshot_reports_the_setting_with_or_without_an_hgs_version(self):
        value = session_listing()
        value["preferences"] = {"mobile_delivery": "away_15"}
        legacy = {key: item for key, item in value.items() if key != "hgs_version"}
        for listing_value in (value, legacy):
            with self.subTest("hgs_version" in listing_value):
                self.hgs.script(self.rules(listing_value=listing_value))
                process = await self.launch()
                snapshot = self.relay.heartbeats()[-1].body["snapshot"]
                self.assertEqual(snapshot["preferences"], {"mobile_delivery": "away_15"})
                self.assertEqual(snapshot.get("hgs_version"), listing_value.get("hgs_version"))
                self.assertEqual([row["name"] for row in snapshot["sessions"]],
                                 [row["name"] for row in listing_value["sessions"]])
                process.stop()


class VoiceMessageTests(SessionCase):
    markers = ("voice",)

    def script(self, *rules: dict) -> None:
        self.hgs.script(self.rules(*rules))

    async def test_voice_operations_reach_native_with_their_action_once(self):
        status = submission("speech", "valid/voice_status")
        message_id = status["payload"]["message_id"]
        self.script({"argv": ["voice", SESSION, "--json"], "stdout": voice_message()})
        process = await self.launch()
        for name, action, answer in (("valid/voice_submit", "submit", voice_message()),
                                     ("valid/voice_status", "status",
                                      voice_message(message_id, state="submitted", transcript="Synthetic voice message")),
                                     ("valid/voice_cancel", "cancel", voice_message(message_id, state="cancelled")),
                                     ("valid/voice_profiles", "profiles", {"schema": 1, "computers": []})):
            with self.subTest(name):
                self.script({"argv": ["voice", SESSION, "--json"], "stdout": answer})
                request = submission("speech", name)
                result = await self.submit(request, process)
                self.assertEqual(result["state"], "completed")
                call = self.native_calls()[-1]
                self.assertEqual(call["argv"], ["voice", SESSION, "--json"])
                self.assertEqual(json.loads(call["stdin"]), {"action": action, **request["payload"]})
                await self.assert_replays(request, result, process)

    async def test_a_mismatched_voice_acknowledgement_never_repeats(self):
        process = await self.launch()
        for name, state in (("valid/voice_submit", "uncertain"), ("valid/voice_cancel", "uncertain"),
                            ("valid/voice_status", "failed")):
            with self.subTest(name):
                request = submission("speech", name)
                identity = request["payload"].get("message_id", request["request_id"])
                self.script({"argv": ["voice", SESSION, "--json"],
                             "stdout": voice_message(identity, conversation_id="another-conversation")})
                receipt = await self.submit(request, process)
                self.assertEqual(receipt, {"state": state, "result": None, "error": VOICE_MISMATCH})
                await self.assert_replays(request, receipt, process)
        for answer in (voice_message(state="invented"), voice_message(revision="1"), {"message": voice_message()["message"]},
                       voice_message(transcript=5)):
            with self.subTest(answer):
                self.script({"argv": ["voice", SESSION, "--json"], "stdout": answer})
                request = self.envelope("voice_submit", {key: value for key, value in
                                                         submission("speech", "valid/voice_submit")["payload"].items()
                                                         if key not in ("request_id", "expected_run_id",
                                                                        "expected_conversation_id")})
                self.assertEqual(await self.submit(request, process), self.uncertain(VOICE_MISMATCH))

    async def test_hgs_without_the_voice_abi_advertises_and_runs_nothing(self):
        self.hgs.script(self.rules({"argv": ["voice", SESSION, "--json"], "stdout": voice_message()}, markers=()))
        process = await self.launch()
        operations = self.relay.heartbeats()[-1].body["snapshot"]["mobile_capabilities"]["operations"]
        self.assertFalse({"voice_submit", "voice_status", "voice_cancel", "voice_profiles"} & set(operations))
        self.assertEqual(await self.submit(submission("speech", "valid/voice_submit"), process),
                         self.failed("native voice message ABI is unavailable"))
        self.assertEqual(self.native_calls(), [])

    async def test_a_delivered_status_read_is_kept_only_two_minutes(self):
        status = submission("speech", "valid/voice_status")
        submit = submission("speech", "valid/voice_submit")
        self.script({"argv": ["voice", SESSION, "--json"], "stdout": voice_message(status["payload"]["message_id"])})
        first = await self.launch()
        await self.submit(status, first)
        self.script({"argv": ["voice", SESSION, "--json"], "stdout": voice_message()})
        await self.submit(submit, first)
        await self.until(lambda: all((row := self.journal.row(request["request_id"])) and row["delivered"]
                                     for request in (status, submit)), what="delivered journal rows", process=first)
        first.stop()
        with stopped_journal(self.state_dir) as db:
            db.execute("UPDATE requests SET delivered_at=delivered_at-121")
        second = await self.launch()
        self.assertIsNone(self.journal.row(status["request_id"]))
        self.assertEqual(self.journal.row(submit["request_id"])["state"], "completed")
        self.script({"argv": ["voice", SESSION, "--json"], "stdout": voice_message(status["payload"]["message_id"])})
        await self.submit(status, second, redeliver=True)
        self.assertEqual(len(self.native_calls("voice")), 3)


if __name__ == "__main__":
    unittest.main()
