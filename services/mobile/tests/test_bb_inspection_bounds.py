"""Black-box inspection bounding.

An inspection above the relay's result limit loses whole oldest history rows
and says so; its control disclosures are never cropped. (Public history pages
are in `test_bb_history_recovery`, which took this module's page tests.)"""
import copy
import hashlib
import json
import unittest

from bb_operations import CONVERSATION, RUN, SESSION, OperationCase, listing, size_vector
from connector_harness import MIB, BlackBoxCase, canonical, default_rules

STREAMS = ("events", "message_events", "provider_messages", "attachment_messages")
# The connector leaves 1 KiB of the relay's 1 MiB result limit for the
# phone's envelope, measured as Python's json.dumps encodes the receipt.
INSPECTION_LIMIT = MIB - 1024


def receipt_size(result) -> int:
    return len(json.dumps({"state": "completed", "result": result, "error": None}, allow_nan=False).encode())


class InspectionBoundTests(OperationCase, BlackBoxCase):
    async def asyncSetUp(self):
        await super().asyncSetUp()
        self.hgs.script(default_rules(listing()))

    def inspected(self, raw):
        self.hgs.prepend({"argv": ["inspect", SESSION], "stdout": raw})

    async def test_large_inspection_loses_only_whole_oldest_rows_and_says_so(self):
        own = {"seq": 1, "type": "UserPromptSubmit", "agent_id": "", "at": 1, "detail": "Newest own message"}
        metadata = {"name": SESSION, "run_id": RUN, "conversation_id": CONVERSATION,
                    "pending_questions": [{"question_id": "bb-question", "question_hash": "bb-hash",
                                           "text": "\U0001f980" * 20000}],
                    "input_queue": {"id": "bb-queue", "text": "Literal full queue"},
                    "session_usage": {"context": {"used": 2000, "limit": 100000}}, "compact_context_supported": True}
        raw = {**metadata, "events": [own], "message_events": [own],
               "provider_messages": [{"message_id": str(index), "at": index + 2, "detail": "\U0001f980" * 32000}
                                     for index in range(8)]}
        self.inspected(raw)
        process = await self.launch()
        result = (await self.submit(self.operation("inspect", identity=False), process))["result"]
        for key, value in metadata.items():
            self.assertEqual(result[key], value, key)
        self.assertIs(result["mobile_history_truncated"], True)
        self.assertLessEqual(receipt_size(result), INSPECTION_LIMIT)
        removed = []
        for stream in STREAMS:
            original, kept = raw.get(stream, []), result.get(stream, [])
            # Kept rows are whole, unchanged and in their original order.
            self.assertEqual(kept, [row for row in original if row in kept], stream)
            self.assertEqual(result["mobile_history_omitted"][stream], len(original) - len(kept), stream)
            removed += [(row["at"], stream, index, row) for index, row in enumerate(original) if row not in kept]
        # The newest own message stays; every dropped row is older than every
        # kept history row, and one fewer dropped row would not fit.
        self.assertEqual(result["message_events"], [own])
        kept_order = [(row["at"], stream, index) for stream in STREAMS
                      for index, row in enumerate(raw.get(stream, []))
                      if row in result.get(stream, []) and not (stream == "message_events" and row == own)]
        self.assertLess(max(item[:3] for item in removed), min(kept_order))
        newest = max(removed, key=lambda item: item[:3])
        restored = copy.deepcopy(result)
        stream = newest[1]
        restored[stream] = [row for row in raw[stream] if row in result.get(stream, []) or row is newest[3]]
        restored["mobile_history_omitted"][stream] -= 1
        self.assertGreater(receipt_size(restored), INSPECTION_LIMIT)

    async def test_vector_inspections_keep_their_recorded_rows(self):
        process = await self.launch()
        for identity in ("bound-inspection-fits", "bound-inspection-trims"):
            with self.subTest(identity):
                raw, expect = size_vector(identity)
                self.inspected(raw)
                result = (await self.submit(self.operation("inspect", identity=False), process))["result"]
                self.assertEqual(result.get("mobile_history_truncated", False), expect["truncated"])
                self.assertEqual(result.get("mobile_history_omitted"), expect["omitted"])
                self.assertEqual([row["at"] for row in result["events"]], expect["kept_events_at"])
                self.assertEqual([row["at"] for row in result["message_events"]], expect["kept_message_events_at"])
                self.assertEqual(receipt_size(result), expect["completed_size"])
                self.assertEqual(hashlib.sha256(canonical(result).encode()).hexdigest(), expect["sha256"])

    async def test_small_inspection_is_unchanged(self):
        raw = {"name": SESSION, "run_id": RUN, "conversation_id": CONVERSATION,
               "events": [{"seq": 1, "at": 1, "detail": "Literal \U0001f980"}]}
        self.inspected(raw)
        process = await self.launch()
        self.assertEqual(await self.submit(self.operation("inspect", identity=False), process),
                         {"state": "completed", "result": raw, "error": None})

    async def test_oversized_disclosures_fail_without_cropping(self):
        self.inspected({"name": SESSION, "pending_questions": [{"text": "\U0001f980" * 100000}],
                        "events": [{"at": 1, "detail": "Synthetic"}]})
        process = await self.launch()
        self.assertEqual(await self.submit(self.operation("inspect", identity=False), process), {
            "state": "failed", "result": None,
            "error": "native inspection metadata exceeds relay size limit; required disclosures were not cropped"})


if __name__ == "__main__":
    unittest.main()
