"""Black-box speech jobs and speech configuration.

Speech and its configuration are computer-scoped and carry no session.
Credentials arrive only sealed for the computer, and only allowlisted native
fields become durable receipts. Voice messages, which belong to one exact
conversation, are in `test_bb_controls.VoiceMessageTests`, which took this
module's voice message cases; their vectors and capability stay here."""
import base64
import json
import os
import unittest
import uuid

from bb_operations import OperationCase, help_text, listing, real_hgs, size_vector, validation_cases
from connector_harness import BlackBoxCase, default_rules

SPEECH = ["speech", "--json"]
MACHINE = "00000000-0000-4000-8000-000000000001"
ALGORITHM = "P256-HKDF-SHA256-A256GCM"
SECRET = "BB-FIXTURE-ONLY-KEY"
CONFIG_MISMATCH = "native speech configuration acknowledgement did not match"
UNCONFIRMED = "speech configuration outcome could not be confirmed; check operation status"


def audio(samples: int = 160) -> dict:
    return {"format": "pcm_s16le", "sample_rate": 16000, "channels": 1,
            "data_base64": base64.b64encode(b"\0\0" * samples).decode()}


def job(**changes) -> dict:
    return {"schema": 1, "job_id": "{{stdin.request_id}}", "state": "queued", **changes}


def sealed() -> dict:
    return {"key_id": "a" * 64, "algorithm": ALGORITHM, "ephemeral_public_key": base64.b64encode(bytes(91)).decode(),
            "nonce": base64.b64encode(bytes(12)).decode(), "ciphertext": base64.b64encode(b"\x17" * 40).decode()}


def profile() -> dict:
    return {"id": "example", "name": "Example transcription", "adapter": "openai_http", "model": "fixture",
            "language": "ru", "base_url": "https://speech.example.invalid/v1", "allow_insecure_http": False}


def receipt(**changes) -> dict:
    return {"schema": 1, "machine_id": MACHINE, "request_id": "{{stdin.request_id}}", "state": "completed",
            "revision": 1, "profile_id": "example", "profile_revision": 1, **changes}


def configuration(**changes) -> dict:
    return {"schema": 1, "machine_id": MACHINE, "revision": 0,
            "service": {"available": False, "reason": "Speech service is not running"},
            "adapters": [{"id": "gigaam", "models": ["v3_e2e_rnnt"], "devices": ["cpu", "cuda"]}, {"id": "openai_http"}],
            "profiles": [], "credential_key": {"key_id": "a" * 64, "algorithm": ALGORITHM,
                                               "public_key_spki": base64.b64encode(bytes(91)).decode()}, **changes}


class SpeechCase(OperationCase, BlackBoxCase):
    markers = ("speech", "speech_config", "voice")

    async def asyncSetUp(self):
        await super().asyncSetUp()
        self.hgs.script(default_rules(listing(), help_text(*self.markers)))

    def answer(self, argv, action, stdout=None, **rule) -> None:
        self.hgs.prepend({"argv": argv, "stdin_fields": {"action": action},
                          **({} if stdout is None else {"stdout": stdout}), **rule})

    def computer(self, operation: str, *, own: bool = False, **payload) -> dict:
        """A computer-scoped submission; `own` puts its request_id in the payload."""
        return self.operation(operation, session="", identity=False, own=own, **payload)

    def upsert(self, credential=None) -> dict:
        return self.computer("speech_profile_upsert", own=True, expected_machine_id=MACHINE, expected_revision=0,
                             profile=profile(), credential=credential or {"mode": "replace", "sealed_value": sealed()})

    def journal_text(self) -> bytes:
        return b"".join(path.read_bytes() for path in self.state_dir.iterdir() if path.name.startswith("journal"))


class SpeechJobTests(SpeechCase):
    async def test_vector_submit_keeps_no_transcript(self):
        process = await self.launch()
        await self.replay(process, "speech-submit-drops-text")

    async def test_submit_returns_its_job_once(self):
        self.answer(SPEECH, "submit", job())
        process = await self.launch()
        request = self.computer("speech_submit", own=True, profile_id="gigaam-ru", audio=audio())
        mark = self.mark()
        result = await self.submit(request, process)
        self.assertEqual(result, {"state": "completed", "error": None, "result": {
            "schema": 1, "job_id": request["request_id"], "state": "queued"}})
        [call] = self.since(mark, *SPEECH)
        self.assertEqual(json.loads(call["stdin"]), {"action": "submit", **request["payload"]})
        await self.delivered(process, request)
        self.assertEqual(await self.submit(request, process, redeliver=True), result)
        self.assertEqual(len(self.since(mark, *SPEECH)), 1)

    async def test_unconfirmed_jobs_are_uncertain_and_never_replay(self):
        process = await self.launch()
        for name, rule in (("other job", {"stdout": job(job_id=str(uuid.uuid4()))}),
                           ("unknown state", {"stdout": job(state="done")}),
                           ("failed native", {"exit": 1, "stderr": "synthetic\n"})):
            with self.subTest(name):
                self.answer(SPEECH, "submit", **rule)
                request = self.computer("speech_submit", own=True, profile_id="gigaam-ru", audio=audio())
                mark = self.mark()
                result = await self.submit(request, process)
                self.assertEqual(result["state"], "uncertain")
                self.assertEqual(result["error"], "native command failed" if "exit" in rule
                                 else "native speech acknowledgement did not match")
                await self.delivered(process, request)
                self.assertEqual(await self.submit(request, process, redeliver=True), result)
                self.assertEqual(len(self.since(mark, *SPEECH)), 1)

    async def test_reads_and_cancellation(self):
        process = await self.launch()
        identity = str(uuid.uuid4())
        for operation, action, payload, answer, expected in (
                ("speech_status", "status", {"job_id": identity},
                 {"schema": 1, "job_id": identity, "state": "completed", "text": "Example dictation"}, "completed"),
                ("speech_status", "status", {"job_id": identity},
                 {"schema": 1, "job_id": str(uuid.uuid4()), "state": "completed"}, "failed"),
                ("speech_profiles", "providers", {}, {"schema": 1, "available": True, "profiles": [{"id": "gigaam-ru"}]},
                 "completed"),
                ("speech_profiles", "providers", {}, {"schema": 1, "available": True, "profiles": [{}] * 33}, "failed"),
                ("speech_stats", "stats", {}, {"schema": 1, "jobs": {"queued": 0}}, "completed"),
                ("speech_cancel", "cancel", {"job_id": identity},
                 {"schema": 1, "job_id": identity, "state": "cancelled", "text": "partial"}, "completed")):
            with self.subTest(operation, expected=expected):
                self.answer(SPEECH, action, answer)
                request = self.computer(operation, **payload)
                mark = self.mark()
                result = await self.submit(request, process)
                self.assertEqual(result["state"], expected, result)
                [call] = self.since(mark, *SPEECH)
                self.assertEqual(json.loads(call["stdin"]), {"action": action, **payload})
                if expected == "completed":
                    # A transcript leaves only through a status read.
                    kept = answer if operation != "speech_cancel" else {k: v for k, v in answer.items() if k != "text"}
                    self.assertEqual(result["result"], kept)
                else:
                    self.assertEqual(result["error"], "native speech acknowledgement did not match")

    async def test_recordings_are_bounded_to_two_minutes(self):
        self.answer(SPEECH, "submit", job())
        process = await self.launch()
        for identity in ("speech-audio-maximum", "speech-audio-maximum+4"):
            with self.subTest(identity):
                payload, expect = size_vector(identity)
                request = self.computer("speech_submit", own=True,
                                        **{key: value for key, value in payload.items() if key != "request_id"})
                mark = self.mark()
                result = await self.submit(request, process, timeout=20)
                self.assertEqual(result["error"], expect["error"])
                self.assertEqual(len(self.since(mark, *SPEECH)), 0 if expect["error"] else 1)

    async def test_vector_rejections_never_reach_native(self):
        process = await self.launch()
        await self.assert_rejections(process, validation_cases("speech", {
            "speech_profiles", "speech_stats", "speech_submit", "speech_status", "speech_cancel", "speech_config",
            "speech_config_status", "speech_profile_upsert", "speech_profile_remove", "speech_profile_test",
            "voice_profiles", "voice_submit", "voice_status", "voice_cancel"}), [SPEECH, ["voice"]])


class SpeechConfigurationTests(SpeechCase):
    async def test_sealed_change_runs_once_and_keeps_only_its_receipt(self):
        self.answer(SPEECH, "profile_upsert", receipt())
        process = await self.launch()
        request = self.upsert()
        mark = self.mark()
        result = await self.submit(request, process)
        self.assertEqual(result, {"state": "completed", "error": None,
                                  "result": {**receipt(), "request_id": request["request_id"]}})
        [call] = self.since(mark, *SPEECH)
        self.assertEqual(json.loads(call["stdin"]), {"action": "profile_upsert", **request["payload"]})
        await self.delivered(process, request)
        self.assertEqual(await self.submit(request, process, redeliver=True), result)
        self.assertEqual(len(self.since(mark, *SPEECH)), 1)
        self.assertNotIn(sealed()["ciphertext"].encode(), self.journal_text())

    async def test_unexpected_native_content_never_becomes_a_receipt(self):
        process = await self.launch()
        for name, rule, error in (
                ("credential field", {"stdout": receipt(credential=SECRET)}, CONFIG_MISMATCH),
                ("other computer", {"stdout": receipt(machine_id="00000000-0000-4000-8000-000000000002")}, CONFIG_MISMATCH),
                ("failure answer", {"stdout": {"ok": False, "error": SECRET}, "exit": 1}, CONFIG_MISMATCH),
                ("stderr only", {"exit": 1, "stderr": SECRET + " from provider configuration\n"}, UNCONFIRMED)):
            with self.subTest(name):
                self.answer(SPEECH, "profile_upsert", **rule)
                request = self.upsert()
                result = await self.submit(request, process)
                self.assertEqual(result, {"state": "uncertain", "result": None, "error": error})
                await self.delivered(process, request)
                self.assertEqual(await self.submit(request, process, redeliver=True), result)
        self.assertNotIn(SECRET.encode(), self.journal_text())
        self.assertNotIn(SECRET, process.output())

    async def test_reads_are_allowlisted_and_bound_to_the_computer(self):
        process = await self.launch()
        operation_id = str(uuid.uuid4())
        status = {"operation_id": operation_id, "expected_machine_id": MACHINE}
        for name, operation, action, payload, answer, state in (
                ("configuration", "speech_config", "config_list", {}, configuration(), "completed"),
                ("extra field", "speech_config", "config_list", {}, configuration(api_key=SECRET), "failed"),
                ("plaintext profile secret", "speech_config", "config_list", {},
                 configuration(profiles=[{"id": "x", "revision": 1, "name": "X", "adapter": "openai_http", "model": "m",
                                          "language": "", "credential": {"configured": True, "value": SECRET},
                                          "available": True, "max_duration_ms": 1000}]), "failed"),
                ("status", "speech_config_status", "config_status", status, receipt(request_id=operation_id), "completed"),
                ("status of another operation", "speech_config_status", "config_status", status, receipt(), "failed")):
            with self.subTest(name):
                self.answer(SPEECH, action, answer)
                mark = self.mark()
                result = await self.submit(self.computer(operation, **payload), process)
                self.assertEqual(result["state"], state, result)
                [call] = self.since(mark, *SPEECH)
                self.assertEqual(json.loads(call["stdin"]), {"action": action, **payload})
                if state == "failed":
                    self.assertEqual(result["error"], CONFIG_MISMATCH)
                else:
                    self.assertEqual(result["result"], answer)
        self.assertNotIn(SECRET.encode(), self.journal_text())

    async def test_speech_and_its_configuration_are_separate_capabilities(self):
        for markers, unavailable, error in (
                (("speech",), self.upsert(), "native speech profile management ABI is unavailable"),
                (("speech_config",), self.computer("speech_profiles"), "native speech ABI is unavailable"),
                (("speech_config",), self.operation("voice_profiles"), "native voice message ABI is unavailable")):
            with self.subTest(markers=markers, operation=unavailable["operation"]):
                self.hgs.script(default_rules(listing(), help_text(*markers)))
                process = await self.launch()
                mark = self.mark()
                self.assertEqual(await self.submit(unavailable, process),
                                 {"state": "failed", "result": None, "error": error})
                self.assertEqual(self.since(mark, *SPEECH) + self.since(mark, "voice"), [])
                process.stop()

    async def test_real_cli_manages_a_profile_without_inference(self):
        native = real_hgs()
        if native is None:
            if os.environ.get("CI") == "true":
                self.fail("a built hgs is required in CI (ZERUS_DIRECTORY_TEST_HGS or target/debug/hgs)")
            self.skipTest("needs a built hgs")
        config = self.scratch / "hgs-config"
        (config / "speech.json").write_text(json.dumps({"schema": 1, "profiles": [],
                                                        "state_dir": str(self.scratch / "speech-state")}))
        self.hgs.prepend({"argv": SPEECH, "exec": native})
        process = await self.launch()

        async def execute(request):
            result = await self.submit(request, process, timeout=30)
            self.assertEqual(result["state"], "completed", result)
            return result["result"]

        current = await execute(self.computer("speech_config"))
        self.assertFalse(current["service"]["available"])
        change = self.upsert({"mode": "keep"})
        change["payload"].update(expected_machine_id=current["machine_id"], expected_revision=current["revision"])
        accepted = await execute(change)
        self.assertEqual(accepted["state"], "completed", accepted)
        status = self.computer("speech_config_status", operation_id=change["request_id"],
                               expected_machine_id=current["machine_id"])
        self.assertEqual(await execute(status), accepted)
        changed = await execute(self.computer("speech_config"))
        self.assertEqual([row["id"] for row in changed["profiles"]], ["example"])
        self.assertFalse(changed["profiles"][0]["credential"]["configured"])
        self.assertGreater(changed["revision"], current["revision"])
        removal = self.computer("speech_profile_remove", own=True, expected_machine_id=current["machine_id"],
                                expected_revision=changed["revision"], profile_id="example")
        self.assertEqual((await execute(removal))["state"], "completed")
        self.assertEqual((await execute(self.computer("speech_config")))["profiles"], [])


if __name__ == "__main__":
    unittest.main()
