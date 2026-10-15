"""Black-box sends: text, inline attachments, child sends and compaction.

A send reaches native hgs once with its payload unchanged, after the fresh
native checks its fields call for: the exact parent and child for a child
send, a confirmed compaction for a continuation. Anything the connector
refuses runs no send, and a send whose outcome is unclear is never repeated.
"""
import base64
import json
import unittest
import uuid

from connector_harness import canonical
from session_harness import (CONVERSATION, DSH_SESSION, OPERATIONS, RUN, SESSION, SessionCase, ack, filled, live_row,
                             session_listing, size_case, submission)

LIMITS = OPERATIONS["constants"]["attachments.LIMITS"]
INVALID = "invalid or oversized inline attachment/message"
NOT_COMPACTED = "compaction is not confirmed for this conversation; message was not sent"
SEND = {"prefix": ["send"], "stdout": ack(status="submitted", text="{{stdin.text}}")}


def attachment(size: int, name: str = "example.bin", **fields) -> dict:
    return {"name": name, "mime": "application/octet-stream",
            "data_base64": base64.b64encode(b"\x00" * size).decode(), **fields}


def context_detail(**changes) -> dict:
    return {"name": SESSION, "run_id": RUN, "conversation_id": CONVERSATION, "phase": "idle", "activity": "idle",
            "compact_context_supported": True, "clear_context_supported": True, **changes}


class SendTests(SessionCase):
    large_bodies = True

    async def test_text_and_attachments_reach_native_unchanged_without_a_preflight(self):
        self.hgs.script(self.rules(SEND))
        process = await self.launch()
        requests = [submission("conversation", "valid/send"), submission("conversation", "send/attachments"),
                    self.envelope("send", {"text": "literal $(touch ignored); 'quotes'\n\tЮникод 😀"}),
                    self.envelope("send", {"text": "", "attachments": [attachment(3, reference="[Image #1]")]})]
        for request in requests:
            with self.subTest(request["payload"].get("text")):
                before = len(self.native_calls())
                result = await self.submit(request, process)
                self.assertEqual(result["state"], "completed")
                self.assertEqual(result["result"]["request_id"], request["request_id"])
                [call] = self.native_calls()[before:]
                self.assertEqual(call["argv"], ["send", SESSION, "--json"])
                self.assertEqual(json.loads(call["stdin"]), request["payload"])
                self.assertEqual(len(call["stdin"].encode()),
                                 len(json.dumps(request["payload"], ensure_ascii=False).encode()))
                await self.assert_replays(request, result, process)

    async def test_attachments_at_the_native_limits_reach_native_unchanged(self):
        self.hgs.script(self.rules(SEND))
        process = await self.launch()
        largest = self.envelope("send", {"text": "", "attachments": [
            attachment(LIMITS["max_file_bytes"], "first.bin", reference="[File #1]"),
            attachment(LIMITS["max_total_bytes"] - LIMITS["max_file_bytes"], "second.bin", reference="[File #2]")]})
        many = self.envelope("send", {"text": "eight files", "attachments": [
            attachment(1, f"file-{index}.txt") for index in range(LIMITS["max_count"])]})
        text = filled(size_case("send-text-64KiB")["value"], size_case("send-text-64KiB")["fill"])["text"]
        longest = self.envelope("send", {"text": text})
        self.assertGreater(len(canonical(largest)), 26 * 1024 * 1024)
        for request in (largest, many, longest):
            with self.subTest(len(canonical(request))):
                before = len(self.native_calls())
                result = await self.submit(request, process, timeout=30)
                self.assertEqual(result["state"], "completed")
                [call] = self.native_calls()[before:]
                self.assertEqual(json.loads(call["stdin"]), request["payload"])

    async def test_attachments_and_text_beyond_the_limits_never_reach_native(self):
        process = await self.launch()
        maximum = LIMITS["max_file_bytes"]
        text = filled(size_case("send-text-64KiB+1")["value"], size_case("send-text-64KiB+1")["fill"])["text"]
        self.assertEqual(size_case("send-text-64KiB+1")["expect"]["receipt_error"], INVALID)
        payloads = [
            {"text": "file too large", "attachments": [attachment(maximum + 1)]},
            {"text": "total too large", "attachments": [attachment(maximum, "a"), attachment(maximum, "b"),
                                                        attachment(1, "c")]},
            {"text": "too many", "attachments": [attachment(1, f"{index}.txt") for index in range(LIMITS["max_count"] + 1)]},
            {"text": text}, {"text": ""}, {"text": "  \n"}, {"text": "bell \x07"}, {"text": "/clear"}, {"text": " !ls"},
            {"text": "x", "path": "/example/private"},
            {"text": "x", "attachments": [attachment(1, "../outside")]},
            {"text": "x", "attachments": [attachment(1, "C:\\outside")]},
            {"text": "x", "attachments": [attachment(1, ".")]},
            {"text": "x", "attachments": [attachment(1, "line\nname")]},
            {"text": "x", "attachments": [attachment(1, path="/etc/passwd")]},
            {"text": "x", "attachments": [attachment(1, url="https://example.invalid/file")]},
            {"text": "x", "attachments": [{**attachment(1), "data_base64": "eA===="}]},
            {"text": "x", "attachments": [{**attachment(1), "data_base64": "not base64"}]},
            {"text": "x", "attachments": [{**attachment(1), "data_base64": ""}]},
            {"text": "x", "attachments": [{**attachment(1), "mime": ""}]},
            {"text": "x", "attachments": [attachment(1, "a", reference="[File #1]"), attachment(1, "b", reference="[File #1]")]},
            {"text": "x", "attachments": [attachment(1, reference="[Folder #1]")]},
            {"text": "x", "attachments": {"name": "not a list"}},
            {"text": "x", "expected_compaction_id": "../../file"},
            {"text": "x", "agent_id": "../child"}, {"text": "x", "agent_id": ""},
            {"text": "x", "agent_id": "child", "expected_conversation_id": ""},
            {"text": "x", "agent_id": "child", "expected_compaction_id": str(uuid.uuid4())},
        ]
        for payload in payloads:
            with self.subTest(sorted(payload), text=payload["text"][:20]):
                request = self.envelope("send", payload)
                self.assertEqual(await self.submit(request, process, timeout=30), self.failed(INVALID))
        self.assertEqual(self.native_calls(), [])

    async def test_child_send_checks_the_exact_parent_and_child_before_one_send(self):
        parent = {"name": DSH_SESSION, "run_id": RUN, "conversation_id": CONVERSATION, "subagents": {"child": {}}}
        child = {"name": DSH_SESSION, "run_id": RUN, "parent_conversation_id": CONVERSATION,
                 "conversation_id": CONVERSATION + "/child", "agent_id": "child", "send_supported": True}
        inspect_parent = ["inspect", DSH_SESSION, "--skip-processes"]
        inspect_child = ["inspect", DSH_SESSION, "--agent", "child", "--skip-processes"]

        def script(parent_value, child_value, receipt=ack(agent_id="child", status="submitted")):
            self.hgs.script(self.rules({"argv": inspect_parent, "stdout": parent_value},
                                       {"argv": inspect_child, "stdout": child_value},
                                       {"prefix": ["send"], "stdout": receipt}))

        script(parent, child)
        process = await self.launch()
        request = submission("conversation", "send/agent")
        result = await self.submit(request, process)
        self.assertEqual(result["state"], "completed")
        self.assertEqual([call["argv"] for call in self.native_calls()],
                         [inspect_parent, inspect_child, ["send", DSH_SESSION, "--json"]])
        self.assertEqual(json.loads(self.native_calls("send")[0]["stdin"]), request["payload"])
        await self.assert_replays(request, result, process)

        refusals = [
            (parent, {**child, "send_supported": False}, "native child cannot accept messages under this exact parent", 2),
            (parent, {**child, "agent_id": "other"}, "native child cannot accept messages under this exact parent", 2),
            (parent, {**child, "run_id": "replacement-run"}, "native child cannot accept messages under this exact parent", 2),
            (parent, {**child, "parent_conversation_id": "other"}, "native child cannot accept messages under this exact parent", 2),
            ({**parent, "subagents": {}}, child, "child does not belong to the exact selected parent", 1),
            ({**parent, "run_id": "replacement-run"}, child, "child does not belong to the exact selected parent", 1),
        ]
        for parent_value, child_value, message, inspections in refusals:
            with self.subTest(message, parent=parent_value, child=child_value):
                script(parent_value, child_value)
                before = len(self.native_calls())
                child_send = self.envelope("send", {"text": "to the child", "agent_id": "child"}, session=DSH_SESSION)
                self.assertEqual(await self.submit(child_send, process), self.failed(message))
                self.assertEqual([call["argv"][0] for call in self.native_calls()[before:]], ["inspect"] * inspections)
        before = len(self.native_calls())
        other_provider = self.envelope("send", {"text": "to a child", "agent_id": "child"})
        self.assertEqual(await self.submit(other_provider, process), self.failed("this native provider has no child-send ABI"))
        self.assertEqual(self.native_calls()[before:], [])

        script(parent, child, ack(agent_id="other", status="submitted"))
        wrong = self.envelope("send", {"text": "to the child", "agent_id": "child"}, session=DSH_SESSION)
        receipt = await self.submit(wrong, process)
        self.assertEqual(receipt, self.uncertain("native child send acknowledgement identity did not match"))
        await self.assert_replays(wrong, receipt, process)
        self.assertEqual(len(self.native_calls("send")), 2)

    async def test_compaction_continuation_sends_only_after_native_completion(self):
        compact = submission("conversation", "valid/compact_context")
        self.hgs.script(self.rules({"argv": ["inspect", SESSION], "stdout": context_detail()},
                                   {"prefix": ["compact-context"], "stdout": ack(status="submitted")}))
        process = await self.launch()
        compacted = await self.submit(compact, process)
        self.assertEqual(compacted["state"], "completed")
        self.assertEqual(compacted["result"]["status"], "submitted")
        self.assertEqual([call["argv"] for call in self.native_calls()],
                         [["inspect", SESSION], ["compact-context", SESSION, "--json"]])
        self.assertEqual(json.loads(self.native_calls("compact-context")[0]["stdin"]), compact["payload"])

        done = {"request_id": compact["request_id"], "run_id": RUN, "conversation_id": CONVERSATION, "status": "completed"}
        for detail in (context_detail(compact_context_request={**done, "status": "compacting"}),
                       context_detail(compact_context_request={**done, "status": "submitted"}),
                       context_detail(compact_context_request={**done, "status": "uncertain"}),
                       context_detail(compact_context_request={**done, "request_id": str(uuid.uuid4())}),
                       context_detail(compact_context_request={**done, "run_id": "replacement-run"}),
                       context_detail(compact_context_request={**done, "conversation_id": "other"}),
                       context_detail(compact_context_request=done, phase="working"),
                       context_detail(compact_context_request=done, activity="working"),
                       context_detail()):
            with self.subTest(detail):
                self.hgs.script(self.rules({"argv": ["inspect", SESSION], "stdout": detail}, SEND))
                before = len(self.native_calls())
                request = self.envelope("send", {"text": "Kept draft", "expected_compaction_id": compact["request_id"]})
                self.assertEqual(await self.submit(request, process), self.failed(NOT_COMPACTED))
                self.assertEqual([call["argv"] for call in self.native_calls()[before:]], [["inspect", SESSION]])

        self.hgs.script(self.rules({"argv": ["inspect", SESSION], "stdout": context_detail(compact_context_request=done)},
                                   SEND))
        request = self.envelope("send", {"text": "Kept draft", "expected_compaction_id": compact["request_id"]})
        before = len(self.native_calls())
        result = await self.submit(request, process)
        self.assertEqual(result["state"], "completed")
        self.assertEqual([call["argv"] for call in self.native_calls()[before:]],
                         [["inspect", SESSION], ["send", SESSION, "--json"]])
        self.assertEqual(json.loads(self.native_calls("send")[0]["stdin"]), request["payload"])
        await self.assert_replays(request, result, process)
        await self.assert_replays(compact, compacted, process)
        self.assertEqual((len(self.native_calls("compact-context")), len(self.native_calls("send"))), (1, 1))

    async def test_context_preflight_identity_and_support_gate_the_command(self):
        process = await self.launch()
        refusals = [
            (context_detail(name="other"), "fresh native context identity did not match"),
            (context_detail(run_id="replacement-run"), "fresh native context identity did not match"),
            (context_detail(conversation_id="other"), "fresh native context identity did not match"),
            (context_detail(archive_id=str(uuid.uuid4())), "fresh native context identity did not match"),
            (context_detail(compact_context_supported=False), "native context operation is unavailable"),
            (context_detail(compact_context_supported=1), "native context operation is unavailable"),
            ([context_detail()], "fresh native context identity did not match"),
        ]
        for detail, message in refusals:
            with self.subTest(detail):
                self.hgs.script(self.rules({"argv": ["inspect", SESSION], "stdout": detail},
                                           {"prefix": ["compact-context"], "stdout": ack(status="submitted")}))
                before = len(self.native_calls())
                request = self.envelope("compact_context", {})
                self.assertEqual(await self.submit(request, process), self.failed(message))
                self.assertEqual([call["argv"] for call in self.native_calls()[before:]], [["inspect", SESSION]])
        self.hgs.script(self.rules({"argv": ["inspect", SESSION], "stdout": {"ok": False}, "exit": 1}))
        self.assertEqual(await self.submit(self.envelope("clear_context", {}), process),
                         self.failed("native command failed"))
        self.assertEqual(self.native_calls("compact-context") + self.native_calls("clear-context"), [])

    async def test_context_acknowledgements_must_match_or_stay_uncertain(self):
        process = await self.launch()
        for operation, status in (("clear_context", "confirmed"), ("clear_context", "submitted"),
                                  ("compact_context", "submitted")):
            with self.subTest(operation, status=status):
                command = operation.replace("_", "-")
                self.hgs.script(self.rules({"argv": ["inspect", SESSION], "stdout": context_detail()},
                                           {"prefix": [command], "stdout": ack(status=status)}))
                request = self.envelope(operation, {})
                result = await self.submit(request, process)
                self.assertEqual((result["state"], result["result"]["status"]), ("completed", status))
                self.assertEqual(json.loads(self.native_calls(command)[-1]["stdin"]), request["payload"])
        mismatched = [("compact_context", {"status": "confirmed"}), ("compact_context", {"status": "completed"}),
                      ("clear_context", {"status": "applied"}), ("compact_context", {"request_id": str(uuid.uuid4())}),
                      ("compact_context", {"name": "other"}), ("clear_context", {"run_id": "replacement-run"}),
                      ("clear_context", {"conversation_id": "other"})]
        for operation, fields in mismatched:
            with self.subTest(operation, fields=fields):
                command = operation.replace("_", "-")
                self.hgs.script(self.rules({"argv": ["inspect", SESSION], "stdout": context_detail()},
                                           {"prefix": [command], "stdout": {**ack(status="submitted"), **fields}}))
                request = self.envelope(operation, {})
                receipt = await self.submit(request, process)
                self.assertEqual(receipt, self.uncertain("native context acknowledgement identity or status did not match"))
                await self.assert_replays(request, receipt, process)

    async def test_an_archived_only_name_never_takes_a_context_command(self):
        archived = "claude/archived-only"
        listing = session_listing(live_row(archived, archive_id=str(uuid.uuid4()), state="archived"))
        self.hgs.script(self.rules({"argv": ["inspect", archived], "stdout": context_detail(name=archived)},
                                   listing_value=listing))
        process = await self.launch()
        for operation in ("compact_context", "clear_context", "send"):
            with self.subTest(operation):
                request = self.envelope(operation, {"text": "x"} if operation == "send" else {}, session=archived)
                self.assertEqual(await self.submit(request, process),
                                 self.failed("session must match an exact nonarchived snapshot entry"))
        self.assertEqual(self.native_calls(), [])


if __name__ == "__main__":
    unittest.main()
