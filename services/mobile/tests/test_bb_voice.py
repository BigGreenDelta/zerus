"""Black-box voice messages of a session: submit, status, cancel and profiles.

Each operation reaches its fixed native command once and keeps only an
acknowledgement of exactly this request; a mismatched one is uncertain for a
mutation and failed for a read, and is never repeated. A computer without the
native voice ABI advertises and runs nothing.
"""
import json
import unittest
import uuid

from connector_harness import stopped_journal
from session_harness import CONVERSATION, RUN, SESSION, SessionCase, submission

VOICE_MISMATCH = "native voice acknowledgement did not match"


def voice_message(identity: str = "{{stdin.request_id}}", **changes) -> dict:
    return {"schema": 1, "message": {"id": identity, "run_id": RUN, "conversation_id": CONVERSATION, "revision": 1,
                                     "state": "queued", **changes}}


class VoiceMessageTests(SessionCase):
    markers = ("voice",)

    def script(self, *rules: dict) -> None:
        self.hgs.script(self.rules(*rules))

    async def assert_replays_once_delivered(self, request: dict, receipt: dict, process) -> None:
        """The receipt is replayed unchanged, and nothing runs, once the
        journal has recorded its delivery."""
        await self.until(lambda: (row := self.journal.row(request["request_id"])) and row["delivered"],
                         what="the delivered journal row", process=process)
        await self.assert_replays(request, receipt, process)

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
                expected = voice_message(request["request_id"]) if action == "submit" else answer
                self.assertEqual(result, {"state": "completed", "result": expected, "error": None})
                call = self.native_calls()[-1]
                self.assertEqual(call["argv"], ["voice", SESSION, "--json"])
                self.assertEqual(json.loads(call["stdin"]), {"action": action, **request["payload"]})
                await self.assert_replays_once_delivered(request, result, process)

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
                await self.assert_replays_once_delivered(request, receipt, process)
        for answer in (voice_message(state="invented"), voice_message(state="sent"), voice_message(revision="1"),
                       voice_message(revision=True), {"message": voice_message()["message"]},
                       voice_message(transcript=5)):
            with self.subTest(answer):
                self.script({"argv": ["voice", SESSION, "--json"], "stdout": answer})
                request = self.envelope("voice_submit", {key: value for key, value in
                                                         submission("speech", "valid/voice_submit")["payload"].items()
                                                         if key not in ("request_id", "expected_run_id",
                                                                        "expected_conversation_id")})
                receipt = await self.submit(request, process)
                self.assertEqual(receipt, self.uncertain(VOICE_MISMATCH))
                await self.assert_replays_once_delivered(request, receipt, process)
        # A status read fails on a transcript beyond 64 KiB of UTF-8, one byte more than the bound.
        message_id = str(uuid.uuid4())
        self.script({"argv": ["voice", SESSION, "--json"],
                     "stdout": voice_message(message_id, state="submitted", transcript="x" * 65537)})
        status = self.envelope("voice_status", {"message_id": message_id})
        self.assertEqual(await self.submit(status, process), self.failed(VOICE_MISMATCH))

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
