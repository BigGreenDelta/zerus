"""Black-box session-operation vectors and rejections before any native call.

Every validation vector of a session operation, the delivery setting or a
voice message that the connector rejects ends in exactly its failed receipt
and runs nothing, and the execution vectors of these operations run exactly
their native calls and store and upload exactly their receipt. Identities
that no relay queues are dropped before the journal and block nothing.
Shell syntax, aliases, options and control characters never become native
arguments.

The vectors whose context is a snapshot older than the 45 s archive window
(`*/archive-stale`) need a stale snapshot and are left to Rust unit tests with
an injected clock; execution vectors whose native answer is a raised Python
exception have no black-box equivalent.
"""
import json
import time
import unittest
import uuid

from connector_harness import canonical_uuid, implemented, stopped_journal
from session_harness import (CONTEXT, SESSION, SessionCase, ack, execution, live_row, session_listing, submission,
                             text, validation)

LOCAL = "session must match an exact local snapshot entry"
NONARCHIVED = "session must match an exact nonarchived snapshot entry"


def rejected_vectors() -> list[dict]:
    """Rejected cases of the session families, and the voice and delivery
    setting cases of the speech and launch families."""
    cases = []
    for family in ("envelope", "conversation", "control", "speech", "launch"):
        for row in validation(family)[1]:
            operation = row["submission"].get("operation")
            if family in ("speech", "launch") and not (
                    isinstance(operation, str) and (operation.startswith("voice_") or operation == "set_mobile_delivery")):
                continue
            if row["connector"]["decision"] == "reject" and row.get("context", CONTEXT) == CONTEXT:
                cases.append(row)
    return cases


class SessionValidationTests(SessionCase):
    async def test_rejected_vectors_fail_with_their_receipt_and_run_nothing(self):
        process = await self.launch()
        cases = rejected_vectors()
        receipts = [row for row in cases if row["connector"]["stage"] == "receipt"]
        self.assertGreater(len(receipts), 80)
        for row in cases:
            await self.deliver(row["submission"])
        await self.until(lambda: all(self.relay.results(row["submission"]["request_id"]) for row in receipts),
                         60, "every rejection receipt", process)
        for row in receipts:
            with self.subTest(row["id"]):
                request_id = row["submission"]["request_id"]
                # A build that does not implement a known operation refuses it before validating.
                error = (text(row["connector"]["error"]) if implemented(row["submission"].get("operation"))
                         else "unsupported operation")
                receipt = {"state": "failed", "result": None, "error": error}
                self.assertEqual(self.relay.results(request_id), [receipt])
                stored = self.journal.row(request_id)
                self.assertEqual((stored["state"], json.loads(stored["response"])), ("failed", receipt))
        for row in cases:
            if row["connector"]["stage"] == "before_journal":
                with self.subTest(row["id"]):
                    self.assertEqual(self.relay.uploads(row["submission"]["request_id"]), [])
                    self.assertIsNone(self.journal.row(row["submission"]["request_id"]))
        self.assertEqual(self.native_calls(), [])

    async def test_execution_vectors_store_and_upload_their_receipt(self):
        names = ("send-completed", "inspect-completed", "process-output", "process-output-too-large",
                 "pause-native-failed", "restore-archive", "mark-read-not-found", "set-mobile-delivery",
                 "unsupported-operation")
        markers = ("lifecycle", "read", "preference")
        self.hgs.script(self.rules(markers=markers))
        process = await self.launch()
        for run in map(execution, names):
            with self.subTest(run["id"]):
                self.hgs.script(self.rules(*({"argv": native["argv"], "stdout": native["answer"],
                                              "fill": native.get("fill", {})} for native in run["native"]),
                                           markers=markers))
                request = {**run["request"], "computer_id": self.relay.computer_id}
                before = len(self.native_calls())
                result = await self.submit(request, process)
                self.assertEqual(result, json.loads(run["expect"]["stored"]))
                self.assertEqual(json.loads(self.journal.row(request["request_id"])["response"]), result)
                calls = self.native_calls()[before:]
                self.assertEqual([call["argv"] for call in calls], [call["argv"] for call in run["expect"]["calls"]])
                self.assertEqual([json.loads(call["stdin"]) if call["stdin"] else None for call in calls],
                                 [call.get("payload") for call in run["expect"]["calls"]])

    async def test_identities_no_relay_queues_are_dropped_without_blocking(self):
        self.hgs.script(self.rules({"prefix": ["send"], "stdout": ack(status="submitted")}))
        process = await self.launch()
        identity = str(uuid.uuid4())
        dropped = []
        for request_id in ("../../inspect", "not-a-uuid", 42, None, identity.upper(), "{" + identity + "}",
                           "urn:uuid:" + identity, identity.replace("-", ""), " " + identity):
            request = self.envelope("send", {"text": "never delivered"})
            del request["computer_id"]
            request["request_id"] = request["payload"]["request_id"] = request_id
            dropped.append(request)
        missing = {key: value for key, value in dropped[0].items() if key != "request_id"}
        keys = []
        for value in (*dropped, missing, 42, "send", None, [missing]):
            self.assertFalse(isinstance(value, dict) and canonical_uuid(value.get("request_id")))
            keys.append(await self.relay.inject(value))
        later = self.envelope("send", {"text": "after the dropped requests"})
        result = await self.submit(later, process)
        self.assertEqual(result["state"], "completed")
        self.assertEqual([self.relay.state(key) for key in keys], ["claimed"] * len(keys))
        self.assertEqual([row["request_id"] for row in self.journal.rows()], [later["request_id"]])
        self.assertEqual({identity for identity, _, _ in self.relay.result_posts}, {later["request_id"]})
        self.assertEqual([call["argv"][0] for call in self.native_calls()], ["send"])
        self.assertIsNone(process.returncode)

    async def test_retained_reads_at_their_cap_never_take_mutation_capacity(self):
        # 5,000 read-class rows is the cap; a read beyond it is dropped before
        # the journal (the relay's claim expires), a mutation still runs.
        self.hgs.script(self.rules({"prefix": ["send"], "stdout": ack(status="submitted")},
                                   {"argv": ["inspect", SESSION], "stdout": {"name": SESSION}}))
        first = await self.launch()
        first.stop()
        now, receipt = time.time(), json.dumps({"state": "completed", "result": {}, "error": None})
        with stopped_journal(self.state_dir) as db:
            db.executemany("INSERT INTO requests(request_id,operation,state,response,delivered,request_hash,created_at,"
                           "updated_at,delivered_at) VALUES(?,'inspect','completed',?,1,?,?,?,?)",
                           [(str(uuid.uuid4()), receipt, "0" * 64, now, now, now) for _ in range(5000)])
        process = await self.launch()
        read = self.envelope("inspect", {}, identity=False)
        await self.relay.queue(read)
        later = self.envelope("send", {"text": "mutations keep their own capacity"})
        self.assertEqual((await self.submit(later, process))["state"], "completed")
        self.assertEqual((self.relay.state(read["request_id"]), self.relay.uploads(read["request_id"])), ("claimed", []))
        self.assertIsNone(self.journal.row(read["request_id"]))
        self.assertEqual(self.native_calls("inspect"), [])
        self.assertEqual(len(self.journal.rows()), 5001)

    async def test_payload_identity_must_match_before_native_execution(self):
        process = await self.launch()
        mismatched = self.envelope("send", {"text": "wrong identity"})
        mismatched["payload"]["request_id"] = str(uuid.uuid4())
        cases = [(mismatched, "payload request_id must match request identity")]
        for operation, fields in (("send", {"text": "x"}), ("answer", {}), ("interrupt", {}), ("pause", {})):
            for run in ("", None, 5, ["fixture-run"]):
                request = self.envelope(operation, fields)
                request["payload"]["expected_run_id"] = run
                cases.append((request, "expected_run_id is required"))
            request = self.envelope(operation, fields)
            del request["payload"]["expected_run_id"]
            cases.append((request, "expected_run_id is required"))
        for request, message in cases:
            with self.subTest(request["operation"], payload=request["payload"]):
                self.assertEqual(await self.submit(request, process), self.failed(message))
        self.assertEqual(self.native_calls(), [])

    async def test_aliases_options_and_control_characters_never_become_arguments(self):
        process = await self.launch()
        sessions = [("@peer", LOCAL), ("-n", LOCAL), ("--json", LOCAL), ("@" + SESSION, LOCAL),
                    (SESSION + "@peer", LOCAL), (SESSION + "\n", LOCAL), (SESSION + "\x00", LOCAL),
                    (SESSION + "\x1b[2J", LOCAL), (SESSION + "\x7f", LOCAL), ("", LOCAL), ("x" * 513, LOCAL),
                    ("x" * 512, NONARCHIVED), ("codex/missing", NONARCHIVED), (SESSION.upper(), NONARCHIVED),
                    (SESSION + " ", NONARCHIVED), (SESSION + "​", NONARCHIVED)]
        fields = {"send": {"text": "never delivered"}, "answer": {"question_id": "q", "answers": [["Yes"]]},
                  "interrupt": {}}
        for session, message in sessions:
            for operation, payload in fields.items():
                with self.subTest(operation, session=session):
                    request = self.envelope(operation, payload, session=session)
                    self.assertEqual(await self.submit(request, process), self.failed(message))
        for operation in ("kill", "sh", "__state", "send;exit", "Send", "send ", "send-now", "compact-context", ""):
            with self.subTest(operation=operation):
                request = self.envelope(operation, {"text": "never delivered"})
                self.assertEqual(await self.submit(request, process), self.failed("unsupported operation"))
        self.assertEqual(self.native_calls(), [])

    async def test_shell_syntax_in_an_exact_known_name_stays_one_argument(self):
        names = ["claude/a;echo pwned", "claude/$(touch bb-shell-ran)", "claude/`touch bb-shell-ran`",
                 "claude/'quoted' \"name\" | cat", "claude/ünïcode 界 😀"]
        self.hgs.script(self.rules({"prefix": ["send"], "stdout": ack(status="submitted")},
                                   listing_value=session_listing(*map(live_row, names))))
        process = await self.launch()
        for name in names:
            with self.subTest(name):
                request = self.envelope("send", {"text": "literal $(touch bb-shell-ran)"}, session=name)
                result = await self.submit(request, process)
                self.assertEqual(result["state"], "completed")
                self.assertEqual(result["result"]["name"], name)
        self.assertEqual([call["argv"] for call in self.native_calls()], [["send", name, "--json"] for name in names])
        self.assertEqual([path for path in self.scratch.rglob("bb-shell-ran")], [])

    async def test_laxer_answer_and_interrupt_payloads_reach_native_unchanged(self):
        # The connector checks only the shared identity of these operations
        # and leaves their fields to native hgs (the relay is stricter).
        self.hgs.script(self.rules({"prefix": ["answer"], "stdout": ack(status="submitted")},
                                   {"prefix": ["interrupt"], "stdout": ack(status="submitted")}))
        process = await self.launch()
        for name in ("valid/answer", "answer/fields-missing", "answer/extra-field", "valid/interrupt",
                     "interrupt/field-missing", "interrupt/empty-conversation"):
            with self.subTest(name):
                request = submission("conversation", name)
                before = len(self.native_calls())
                result = await self.submit(request, process)
                self.assertEqual(result["state"], "completed")
                [call] = self.native_calls()[before:]
                self.assertEqual(call["argv"], [request["operation"], SESSION, "--json"])
                self.assertEqual(json.loads(call["stdin"]), request["payload"])
                self.assertEqual(result["result"]["request_id"], request["request_id"])


if __name__ == "__main__":
    unittest.main()
