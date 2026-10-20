"""Shared Mute is an exact durable mutation, never a read acknowledgement."""
import copy
import tempfile
import unittest
import uuid
from unittest.mock import AsyncMock, patch

from relay_oracle import validate_request
from zerus_mobile.connector import Connector, MUTATIONS
from zerus_mobile.context import READ_OPERATIONS
from zerus_mobile.session_notifications import HELP_MARKER, result_valid, validate, revisions_current

NAME = "codex/example/notifications"


def payload(**changes):
    return {"run_id": "fixture-run", "conversation_id": "fixture-conversation", "muted": True, "expected_revision": 0, **changes}


def request(value=None):
    return {"request_id": str(uuid.uuid4()), "operation": "session_notifications", "session": NAME,
            "payload": payload() if value is None else value}


def receipt():
    return {"ok": True, "applied": True, "run_id": "fixture-run", "conversation_id": "fixture-conversation",
            "preferences": {"conversation_id": "fixture-conversation", "muted": True, "revision": 1,
                            "suppressed": {"reply_id": None, "attention_signature": None,
                                           "optional_question_ids": [], "main_attention": None, "child_attention": {}}}}


class SharedMuteValidation(unittest.TestCase):
    def test_exact_types_and_mutation_class(self):
        self.assertIn("session_notifications", MUTATIONS)
        self.assertNotIn("session_notifications", READ_OPERATIONS)
        validate(payload())
        validate(payload(muted=False, expected_revision=(1 << 63) - 1))
        invalid = [payload(muted=1), payload(muted="false"), payload(expected_revision=True),
                   payload(expected_revision=-1), payload(expected_revision=1.0), payload(expected_revision=1 << 63),
                   payload(run_id=""), payload(run_id="é" * 65), payload(conversation_id="é" * 129),
                   payload(run_id="line\nbreak"), payload(conversation_id="x" * 257), {**payload(), "reply_id": "old"},
                   {key: value for key, value in payload().items() if key != "muted"}, None]
        for value in invalid:
            with self.subTest(value=value), self.assertRaises(ValueError):
                validate(value)

    def test_reference_relay_accepts_same_finite_payload(self):
        value = {**request(), "computer_id": str(uuid.uuid4())}
        validate_request(value)
        with self.assertRaises(ValueError):
            validate_request({**value, "payload": payload(expected_revision=False)})

    def test_only_exact_host_confirmation_is_accepted(self):
        good = receipt()
        self.assertTrue(result_valid(good, payload()))
        noop = copy.deepcopy(good); noop["applied"] = False; noop["preferences"]["revision"] = 0
        self.assertTrue(result_valid(noop, payload()))
        for field, value in [("run_id", "other"), ("conversation_id", "other"), ("ok", 1), ("applied", 1)]:
            bad = copy.deepcopy(good); bad[field] = value
            self.assertFalse(result_valid(bad, payload()))
        for field, value in [("conversation_id", "other"), ("muted", False), ("muted", 1), ("revision", True), ("revision", 2), ("suppressed", None)]:
            bad = copy.deepcopy(good); bad["preferences"][field] = value
            self.assertFalse(result_valid(bad, payload()))
        self.assertTrue(result_valid({"ok": False, "error": "conflict"}, payload()))
        for applied in (True, False, None):
            self.assertFalse(result_valid({"ok": False, "error": "conflict", "applied": applied}, payload()))
        self.assertFalse(result_valid({"ok": False, "error": "unknown"}, payload()))

    def test_late_catalog_cannot_undo_a_host_confirmation_across_rename_or_resume(self):
        preference = receipt()["preferences"]
        row = {"name": NAME, "conversation_id": "fixture-conversation", "notification_preferences": preference}
        previous = {"sessions": [row]}
        old = copy.deepcopy(row); old["notification_preferences"]["revision"] = 0
        self.assertFalse(revisions_current(previous, {"sessions": [old]}))
        conflict = copy.deepcopy(row); conflict["notification_preferences"]["muted"] = False
        self.assertFalse(revisions_current(previous, {"sessions": [conflict]}))
        for value in (None, {}, {"conversation_id": "other"}):
            unknown = copy.deepcopy(row); unknown["notification_preferences"] = value
            self.assertFalse(revisions_current(previous, {"sessions": [unknown]}))
        resumed = copy.deepcopy(row); resumed["name"] = "codex/renamed"; resumed["run_id"] = "new-run"
        self.assertTrue(revisions_current(previous, {"sessions": [resumed]}))
        fresh = copy.deepcopy(row); fresh["conversation_id"] = "new"; fresh["notification_preferences"]["conversation_id"] = "new"; fresh["notification_preferences"]["revision"] = 0
        self.assertTrue(revisions_current(previous, {"sessions": [fresh]}))


class SharedMuteConnector(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.connector = Connector({"server_url": "https://relay.example.invalid", "node_token": "fixture-token", "state_dir": self.temp.name})
        self.connector.sessions = {NAME}
        self.connector.session_notifications_supported = True

    async def asyncTearDown(self):
        self.connector.journal.close(); self.temp.cleanup()

    async def test_exact_native_call_and_durable_replay_never_write_twice(self):
        value = request()
        with patch.object(self.connector, "native", AsyncMock(return_value=receipt())) as native:
            first = await self.connector.execute(value)
            self.assertEqual(first, {"state": "completed", "result": receipt(), "error": None})
            self.assertEqual(await self.connector.execute(value), first)
            self.assertEqual(native.await_count, 1)
            self.assertEqual(native.call_args.args, (["notifications", NAME, "--json"], value["payload"]))
            self.assertEqual(native.call_args.kwargs, {"failure_json": True})

    async def test_stale_revision_is_a_native_refusal_not_success_or_retry(self):
        conflict = {"ok": False, "error": "conflict", "run_id": "fixture-run", "conversation_id": "fixture-conversation", "preferences": receipt()["preferences"]}
        value = request()
        with patch.object(self.connector, "native", AsyncMock(return_value=conflict)) as native:
            first = await self.connector.execute(value)
            self.assertEqual(first["result"], conflict)
            self.assertEqual(await self.connector.execute(value), first)
            self.assertEqual(native.await_count, 1)

    async def test_missing_marker_invalid_target_and_payload_never_call_native(self):
        for value in [request(payload(muted="true")), {**request(), "session": "codex/unknown"}]:
            with patch.object(self.connector, "native", AsyncMock()) as native:
                self.assertEqual((await self.connector.execute(value))["state"], "failed")
                native.assert_not_awaited()
        self.connector.session_notifications_supported = False
        with patch.object(self.connector, "native", AsyncMock()) as native:
            self.assertEqual((await self.connector.execute(request()))["state"], "failed")
            native.assert_not_awaited()

    async def test_help_marker_is_exact_and_never_implies_shared_read(self):
        for help_text, expected in [("notifications <session> --json", False), (HELP_MARKER + "0", False),
                                    (HELP_MARKER + " suffix", False), ("  " + HELP_MARKER + "  ", True)]:
            self.connector.capabilities_next_poll = 0
            with patch.object(self.connector, "native", AsyncMock(return_value=help_text)):
                await self.connector.probe_capabilities()
            self.assertIs(self.connector.session_notifications_supported, expected)
            self.assertFalse(self.connector.read_supported)

    async def test_wrong_native_receipt_is_uncertain_after_attempted_mutation(self):
        bad = receipt(); bad["preferences"]["revision"] = 7
        with patch.object(self.connector, "native", AsyncMock(return_value=bad)):
            self.assertEqual((await self.connector.execute(request()))["state"], "uncertain")
