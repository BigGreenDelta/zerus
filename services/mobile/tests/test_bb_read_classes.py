"""Black-box journal classes of computer, read and media operations.

Screens, attachment ranges, transcripts and speech settings leave the journal
two minutes after delivery; catalogs, history and inspections after an hour;
mutations stay to prevent a repeat. A restart fails an interrupted read and
leaves an interrupted mutation uncertain. Rows are aged only while the
connector is stopped, and the journal file is scanned for what it held."""
import base64
import signal
import time
import unittest
import uuid

from bb_operations import (CONVERSATION, READ_OPERATIONS, RECEIPTS, RUN, SESSION, OperationCase, help_text,
                           listing)
from connector_harness import RESTART_ERROR, BlackBoxCase, default_rules

SCREEN = "BB-SCREEN-SENTINEL"
FILE = b"BB-FILE-SENTINEL " * 4
TRANSCRIPT = "BB-TRANSCRIPT-SENTINEL"
VOICE = "BB-VOICE-SENTINEL"
HISTORY = "BB-HISTORY-SENTINEL"
INSPECTED = "BB-INSPECT-SENTINEL"
MACHINE = "00000000-0000-4000-8000-000000000001"
MESSAGE = "00000000-0000-4000-8000-0000000000aa"
TERMINAL = ["terminal", SESSION, "--json"]
SPEECH = ["speech", "--json"]
VOICE_ARGV = ["voice", SESSION, "--json"]
WORKTREES = ["__state", "worktrees", "--path", "/example/repo", "--refresh", "--json"]
SHORT = {"terminal_snapshot", "attachment_read", "speech_profiles", "speech_stats", "speech_status", "speech_config",
         "speech_config_status", "voice_profiles", "voice_status"}
HOUR = {"inspect", "history", "catalog", "dirs", "worktrees"}
DEADLINE = 5.5  # The terminal input claim deadline; the connector refuses one more than 6 s ahead.
KEY = {"key_id": "a" * 64, "algorithm": "P256-HKDF-SHA256-A256GCM",
       "public_key_spki": base64.b64encode(bytes(91)).decode()}


def identity(**changes) -> dict:
    return {"request_id": "{{stdin.request_id}}", "name": SESSION, "run_id": RUN, "conversation_id": CONVERSATION,
            **changes}


def rules() -> list[dict]:
    voice = {"schema": 1, "message": {"id": "{{stdin.message_id}}", "run_id": RUN, "conversation_id": CONVERSATION,
                                      "revision": 1, "state": "submitted", "transcript": VOICE}}
    return [
        {"argv": TERMINAL, "stdin_has": {"action": "snapshot"},
         "stdout": identity(status="snapshot", terminal_binding_id="d" * 64, input_supported=True, lines=[SCREEN])},
        {"argv": TERMINAL, "stdin_has": {"action": "input"}, "stdout": identity(status="submitted",
                                                                                terminal_binding_id="d" * 64)},
        {"argv": ["inspect", SESSION, "--skip-processes"], "stdout": {"name": SESSION, "run_id": RUN,
                                                                      "conversation_id": CONVERSATION}},
        {"argv": ["attachment", SESSION, "--request", MESSAGE, "--index", "0", "--conversation", CONVERSATION],
         "stdout": {"request_id": MESSAGE, "index": 0, "name": "note.txt", "mime": "text/plain", "bytes": len(FILE),
                    "data_base64": base64.b64encode(FILE).decode()}},
        {"argv": SPEECH, "stdin_has": {"action": "status"},
         "stdout": {"schema": 1, "job_id": "{{stdin.job_id}}", "state": "completed", "text": TRANSCRIPT}},
        {"argv": SPEECH, "stdin_has": {"action": "providers"}, "stdout": {"schema": 1, "available": True, "profiles": []}},
        {"argv": SPEECH, "stdin_has": {"action": "stats"}, "stdout": {"schema": 1}},
        {"argv": SPEECH, "stdin_has": {"action": "submit"},
         "stdout": {"schema": 1, "job_id": "{{stdin.request_id}}", "state": "queued"}},
        {"argv": SPEECH, "stdin_has": {"action": "config_list"},
         "stdout": {"schema": 1, "machine_id": MACHINE, "revision": 0, "service": {"available": False},
                    "adapters": [], "profiles": [], "credential_key": KEY}},
        {"argv": SPEECH, "stdin_has": {"action": "config_status"},
         "stdout": {"schema": 1, "machine_id": MACHINE, "request_id": "{{stdin.operation_id}}", "state": "completed"}},
        {"argv": VOICE_ARGV, "stdin_has": {"action": "status"}, "stdout": voice},
        {"argv": VOICE_ARGV, "stdin_has": {"action": "profiles"}, "stdout": {"schema": 1, "computers": []}},
        {"argv": ["history", SESSION, "--json"],
         "stdout": identity(history_epoch="bb-epoch", head={"complete": True}, events=[{"detail": HISTORY}])},
        {"argv": ["inspect", SESSION], "stdout": {"name": SESSION, "events": [{"detail": INSPECTED}]}},
        {"argv": ["dirs", "/example"], "stdout": {"path": "/example", "directories": []}},
        {"argv": WORKTREES, "stdout": {"state": "ok", "path": "/example/repo", "common_dir": "/example/repo/.git",
                                       "worktrees": []}},
        {"argv": ["dirs", "/canonical"], "stdout": {"path": "/canonical", "directories": []}},
        {"prefix": ["dirs", "--create", "/canonical/new"],
         "stdout": {"status": "created", "request_id": "{{argv.4}}", "requested_path": "/canonical/new",
                    "path": "/canonical/new", "parent": "/canonical"}},
    ]


class ReadClassTests(OperationCase, BlackBoxCase):
    async def asyncSetUp(self):
        await super().asyncSetUp()
        self.hgs.script(rules() + default_rules(listing(), help_text(
            "terminal", "history", "launch", "worktree", "directory_create", "attachment_read", "speech", "voice",
            "speech_config")))

    def computer(self, operation, own=False, **payload):
        return self.operation(operation, session="", identity=False, own=own, **payload)

    def requests(self) -> list[dict]:
        audio = {"format": "pcm_s16le", "sample_rate": 16000, "channels": 1,
                 "data_base64": base64.b64encode(bytes(320)).decode()}
        return [
            self.operation("terminal_snapshot"),
            self.operation("attachment_read", message_id=MESSAGE, index=0, offset=0, length=len(FILE)),
            self.computer("speech_status", job_id=str(uuid.uuid4())), self.computer("speech_profiles"),
            self.computer("speech_stats"), self.computer("speech_config"),
            self.computer("speech_config_status", operation_id=str(uuid.uuid4()), expected_machine_id=MACHINE),
            self.operation("voice_status", message_id=str(uuid.uuid4())), self.operation("voice_profiles"),
            self.operation("inspect", identity=False), self.operation("history"),
            self.computer("catalog", own=True), self.computer("dirs", own=True, path="/example"),
            self.computer("worktrees", own=True, path="/example/repo"),
            self.operation("terminal_input", terminal_binding_id="d" * 64, text="echo synthetic", enter=True),
            self.computer("directory_create", own=True, path="/canonical/new"),
            self.computer("speech_submit", own=True, profile_id="gigaam-ru", audio=audio),
        ]

    async def test_reads_leave_the_journal_by_class_and_mutations_stay(self):
        requests = self.requests()
        operations = {request["operation"] for request in requests}
        self.assertEqual(operations & READ_OPERATIONS, SHORT | HOUR)
        first = await self.launch()
        results, deadlines = {}, {}
        for request in requests:
            # A redelivery repeats the relay's claim deadline, which the request identity covers.
            deadlines[request["request_id"]] = time.time() + DEADLINE
            result = await self.submit(request, first, expires_at=deadlines[request["request_id"]])
            self.assertEqual(result["state"], "completed", (request["operation"], result))
            results[request["request_id"]] = result
        await self.delivered(first, *requests)
        held = self.journal_bytes()
        for sentinel in (SCREEN, base64.b64encode(FILE).decode(), TRANSCRIPT, VOICE, HISTORY, INSPECTED):
            self.assertIn(sentinel.encode(), held, "the scan would not see this sentinel")
        first.stop()

        self.age(121, *requests)
        second = await self.launch()
        kept = {row["operation"] for row in self.journal.rows()}
        self.assertEqual(kept, operations - SHORT)
        held = self.journal_bytes()
        for sentinel in (SCREEN, base64.b64encode(FILE).decode(), TRANSCRIPT, VOICE):
            self.assertNotIn(sentinel.encode(), held, "a short-lived read stayed in the journal file")
        mark = self.mark()
        for request in requests:
            if request["operation"] in HOUR:
                self.assertEqual(await self.submit(request, second, redeliver=True,
                                                   expires_at=deadlines[request["request_id"]]),
                                 results[request["request_id"]])
        # A catalog reads accounts and projects, which the heartbeat read only
        # just now and next reads 15 s later; count them as work here.
        self.assertEqual(self.work(mark, keep=[["account", "ls"], ["swarm", "get"]]), [],
                         "a retained read executed again")
        await self.delivered(second, *[request for request in requests if request["operation"] in HOUR])
        second.stop()

        self.age(3601, *requests)
        third = await self.launch()
        rows = self.journal.rows()
        self.assertEqual({row["operation"] for row in rows}, operations - SHORT - HOUR)
        self.assertFalse(any(row["response_expired"] for row in rows))
        held = self.journal_bytes()
        for sentinel in (HISTORY, INSPECTED):
            self.assertNotIn(sentinel.encode(), held, "an hour-long read stayed in the journal file")
        mark = self.mark()
        for request in requests:
            if request["operation"] not in READ_OPERATIONS:
                self.assertEqual(await self.submit(request, third, redeliver=True,
                                                   expires_at=deadlines[request["request_id"]]),
                                 results[request["request_id"]])
        self.assertEqual(self.work(mark), [], "a mutation executed again")

    def journal_bytes(self) -> bytes:
        return b"".join(path.read_bytes() for path in self.state_dir.iterdir() if path.name.startswith("journal"))

    async def test_a_restart_fails_an_interrupted_read_and_doubts_an_interrupted_mutation(self):
        for request, argv, state in (
                (self.computer("speech_status", job_id=str(uuid.uuid4())), SPEECH, "failed"),
                (self.computer("speech_submit", own=True, profile_id="gigaam-ru", audio={
                    "format": "pcm_s16le", "sample_rate": 16000, "channels": 1,
                    "data_base64": base64.b64encode(bytes(320)).decode()}), SPEECH, "uncertain")):
            with self.subTest(request["operation"]):
                self.hgs.prepend({"argv": argv, "stdout": {"schema": 1}, "kill_parent": True})
                first = await self.launch()
                await self.relay.queue(request)
                self.assertEqual(await first.wait_exit(10), -signal.SIGKILL)
                self.hgs.script([{key: value for key, value in rule.items() if key not in ("id", "kill_parent")}
                                 for rule in self.hgs.rules[1:]])
                second = await self.launch()
                expected = {"state": state, "result": None, "error": RESTART_ERROR}
                self.assertEqual(expected["error"], RECEIPTS["restart_recovered_read"]["error"])
                self.assertEqual(await self.until(lambda: self.relay.results(request["request_id"]),
                                                  what="the recovered upload", process=second), [expected])
                second.stop()


if __name__ == "__main__":
    unittest.main()
