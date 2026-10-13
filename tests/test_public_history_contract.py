"""Public history ABI on synthetic private files; no provider or tmux is started.

Persisted binding changes model a confirmed same-conversation resume. These tests
exercise indexing and wire identity, not lifecycle execution or model traffic.
"""
import hashlib
from contextlib import closing
import json
import os
from pathlib import Path
import sqlite3
import subprocess
import tempfile
import unittest
import uuid


REPO = Path(__file__).resolve().parents[1]
HGS = Path(os.environ.get("HGS_TEST_BIN", REPO / "target/debug/hgs")).resolve()


class PublicHistoryContract(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="hgs-public-history-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.state = self.root / "state"
        self.state.mkdir(mode=0o700)
        socket = self.root / "tmux-private"
        socket.mkdir(mode=0o700)
        self.env = {key: value for key, value in os.environ.items()
                    if not key.startswith("HGS_") and key not in ("TMUX", "TMUX_PANE", "TMUX_TMPDIR")}
        self.env.update(HOME=str(self.root), HGS_STATE_DIR=str(self.state),
                        HGS_CONFIG_DIR=str(self.root / "config"), HGS_SELF="history-fixture",
                        HGS_PEERS="", TMUX_TMPDIR=str(socket))
        self.name = "codex/history-fixture/mobile"
        self.run = str(uuid.uuid4())
        self.conversation = str(uuid.uuid4())
        self.transcript = self.root / f"rollout-{self.conversation}.jsonl"
        self.binding = dict(version=1, name=self.name, agent="codex", run_id=self.run,
                            conversation_id=self.conversation, agent_home=str(self.root / ".codex"),
                            account_id="synthetic", transcript=str(self.transcript), state="stopped")
        self.write_binding()
        self.write_source([])

    def write_binding(self, name=None, **updates):
        self.binding.update(updates)
        if name is not None:
            self.binding["name"] = name
        path = self.state / (hashlib.sha256(self.binding["name"].encode()).hexdigest() + ".json")
        path.write_text(json.dumps(self.binding), encoding="utf-8")
        path.chmod(0o600)

    @staticmethod
    def message(index, incoming=True, at=None, text=None, message_id=None):
        return {"type": "event_msg", "timestamp": at if at is not None else 1_700_000_000 + index,
                "payload": {"type": "agent_message" if incoming else "user_message",
                            "id": message_id or f"public-{index}",
                            "message": text if text is not None else f"literal public message {index}"}}

    def write_source(self, rows):
        header = {"type": "session_meta", "payload": {"id": self.conversation}}
        with self.transcript.open("w", encoding="utf-8") as stream:
            for row in [header, *rows]:
                stream.write(json.dumps(row, ensure_ascii=False) + "\n")
        self.transcript.chmod(0o600)

    def append_source(self, rows):
        with self.transcript.open("a", encoding="utf-8") as stream:
            for row in rows:
                stream.write(json.dumps(row, ensure_ascii=False) + "\n")

    def journal(self, rows):
        path = self.state / "events.sqlite3"
        with closing(sqlite3.connect(path)) as db, db:
            db.execute("CREATE TABLE IF NOT EXISTS events (seq INTEGER PRIMARY KEY, name TEXT, conversation TEXT, payload TEXT)")
            for row in rows:
                db.execute("INSERT INTO events(name,conversation,payload) VALUES(?,?,?)",
                           (self.name, self.conversation, json.dumps(row)))
        path.chmod(0o600)

    def request(self, **updates):
        return {"request_id": str(uuid.uuid4()), "expected_run_id": self.run,
                "expected_conversation_id": self.conversation, "limit": 100, **updates}

    def history(self, payload=None, reject=False, name=None, **updates):
        payload = payload or self.request(**updates)
        result = subprocess.run([str(HGS), "history", name or self.name, "--json"],
                                input=json.dumps(payload), text=True, capture_output=True,
                                env=self.env, timeout=15)
        if reject:
            self.assertNotEqual(result.returncode, 0, result.stdout)
            return result.stderr
        self.assertEqual(result.returncode, 0, result.stderr)
        body = json.loads(result.stdout)
        self.assertEqual(body["request_id"], payload["request_id"])
        self.assertEqual(body["name"], name or self.name)
        self.assertEqual(body["run_id"], payload["expected_run_id"])
        self.assertEqual(body["conversation_id"], payload["expected_conversation_id"])
        self.assertLessEqual(len(body["events"]), payload.get("limit", 100))
        return body

    def complete(self, **updates):
        # Each call has bounded source/reconciliation work. Completion is
        # explicit; a page that happens to have 100 rows is not proof of it.
        for _ in range(100):
            page = self.history(**updates)
            if page["head"]["complete"]:
                self.assertFalse(page["indexing"])
                return page
            self.assertIsNone(page["head"]["incoming_seq"])
            self.assertIsNone(page["head"]["total_incoming"])
        self.fail("synthetic history indexing did not finish within bounded calls")

    def all_older(self, head):
        rows = list(head["events"])
        page = head
        seen = set()
        for _ in range(100):
            if not page["has_more_before"]:
                self.assertIsNone(page["next_before"])
                return rows
            cursor = page["next_before"]
            self.assertNotIn(cursor, seen)
            seen.add(cursor)
            page = self.history(before=cursor)
            self.assertEqual(page["history_epoch"], head["history_epoch"])
            self.assertEqual(page["head"], head["head"])
            rows = page["events"] + rows
        self.fail("older paging failed to make progress")

    def test_long_public_history_behind_tool_noise_and_unread_watermark(self):
        source = []
        for index in range(1240):
            source.extend([
                {"type": "response_item", "payload": {"type": "function_call", "arguments": "PRIVATE TOOL NOISE"}},
                {"type": "event_msg", "payload": {"type": "agent_message", "phase": "analysis", "message": "PRIVATE ANALYSIS"}},
                self.message(index, incoming=index % 2 == 1),
            ])
        self.write_source(source)
        first = self.history()
        self.assertTrue(first["indexing"])
        self.assertFalse(first["head"]["complete"])
        self.assertIsNone(first["head"]["total_incoming"])
        head = self.complete()
        rows = self.all_older(head)
        self.assertEqual([row["detail"] for row in rows], [f"literal public message {i}" for i in range(1240)])
        self.assertEqual(len({row["history_id"] for row in rows}), 1240)
        incoming = [row for row in rows if row["type"] == "AgentMessage"]
        self.assertEqual([row["incoming_seq"] for row in incoming], list(range(1, 621)))
        self.assertTrue(all(row["incoming_seq"] is None for row in rows if row["type"] != "AgentMessage"))
        self.assertEqual(head["head"]["total_incoming"], 620)
        # A durable read-through=100 selects the global first unread reply,
        # rather than the first row of the newest bounded window.
        unread = self.history(around_incoming_seq=100, history_epoch=head["history_epoch"], limit=10)
        self.assertEqual(unread["events"][0]["incoming_seq"], 101)
        self.assertEqual(unread["events"][0]["detail"], "literal public message 201")
        self.assertEqual(unread["head"], head["head"])
        serialized = json.dumps(rows)
        self.assertNotIn("PRIVATE TOOL NOISE", serialized)
        self.assertNotIn("PRIVATE ANALYSIS", serialized)

    def test_equal_timestamp_order_and_both_cursor_directions(self):
        self.write_source([self.message(index, at=100, message_id=f"reverse-{30-index:02}") for index in range(30)])
        head = self.complete(limit=7)
        rows = self.all_older(head)
        self.assertEqual([row["detail"] for row in rows], [f"literal public message {i}" for i in range(30)])
        anchor = rows[9]
        around = self.history(around=anchor["history_cursor"], limit=3)
        self.assertEqual([row["history_id"] for row in around["events"]], [row["history_id"] for row in rows[9:12]])
        before = self.history(before=anchor["history_cursor"], limit=3)
        after = self.history(after=anchor["history_cursor"], limit=3)
        self.assertEqual([row["history_id"] for row in before["events"]], [row["history_id"] for row in rows[6:9]])
        self.assertEqual([row["history_id"] for row in after["events"]], [row["history_id"] for row in rows[10:13]])

    def test_identity_cursor_authentication_and_selector_rejection(self):
        self.write_source([self.message(index) for index in range(4)])
        head = self.complete()
        cursor = head["events"][1]["history_cursor"]
        self.history(self.request(expected_run_id=str(uuid.uuid4())), reject=True)
        self.history(self.request(expected_conversation_id=str(uuid.uuid4())), reject=True)
        self.history(self.request(before=cursor, after=cursor), reject=True)
        self.history(self.request(around_incoming_seq=0), reject=True)
        self.history(self.request(around_incoming_seq=0, history_epoch=str(uuid.uuid4())), reject=True)
        self.history(self.request(before=cursor[:-1] + ("0" if cursor[-1] != "0" else "1")), reject=True)
        # A different provider conversation gets a different cursor secret.
        original = dict(self.binding)
        other = str(uuid.uuid4())
        other_source = self.root / f"rollout-{other}.jsonl"
        other_source.write_text(json.dumps({"type": "session_meta", "payload": {"id": other}}) + "\n")
        other_name = "codex/history-fixture/other"
        self.write_binding(name=other_name, conversation_id=other, transcript=str(other_source))
        self.history(self.request(expected_conversation_id=other, before=cursor), name=other_name, reject=True)
        self.binding = original

    def test_stable_ids_across_repeat_and_same_conversation_resume(self):
        self.write_source([self.message(index) for index in range(8)])
        before = self.complete()
        repeated = self.history()
        self.assertEqual(repeated["events"], before["events"])
        self.run = str(uuid.uuid4())
        self.write_binding(run_id=self.run)
        resumed = self.complete()
        self.assertEqual(resumed["history_epoch"], before["history_epoch"])
        self.assertEqual(resumed["events"], before["events"])
        self.history(self.request(expected_run_id=before["run_id"]), reject=True)
        anchored = self.history(around=before["events"][3]["history_cursor"])
        self.assertEqual(anchored["events"][0]["history_id"], before["events"][3]["history_id"])

    def test_source_reset_rejects_old_epoch_and_cursors(self):
        self.write_source([self.message(index) for index in range(40)])
        before = self.complete()
        old_cursor = before["events"][10]["history_cursor"]
        self.write_source([self.message(999, text="new source after reset")])
        reset = self.history()
        self.assertFalse(reset["head"]["complete"])
        self.assertEqual(reset["source_status"]["provider"], "source_reset")
        self.assertNotEqual(reset["history_epoch"], before["history_epoch"])
        after = self.complete()
        self.assertEqual([row["detail"] for row in after["events"]], ["new source after reset"])
        self.history(self.request(before=old_cursor), reject=True)
        self.history(self.request(around_incoming_seq=0, history_epoch=before["history_epoch"]), reject=True)

    def test_late_public_reply_preserves_stop_identity_and_read_sequence(self):
        self.journal([{"type": "Stop", "at": 100, "detail": "A unique short reply"}])
        before = self.complete()
        hook = before["events"][0]
        self.append_source([self.message(0, at=101, text="A unique short reply with full provider detail")])
        after = self.complete()
        self.assertEqual(len(after["events"]), 1)
        row = after["events"][0]
        self.assertEqual(row["history_id"], hook["history_id"])
        self.assertEqual(row["incoming_seq"], hook["incoming_seq"])
        self.assertEqual(after["history_epoch"], before["history_epoch"])
        self.assertEqual(row["type"], "AgentMessage")
        self.assertEqual(row["detail"], "A unique short reply with full provider detail")
        self.assertTrue(set(hook["original_ids"]).issubset(row["original_ids"]))
        self.assertTrue(any(alias.startswith("provider:codex_transcript:") for alias in row["original_ids"]))

    def test_ambiguous_repeated_replies_are_not_arbitrarily_aliased(self):
        self.journal([{"type": "Stop", "at": 100, "detail": "Repeated reply"},
                      {"type": "Stop", "at": 101, "detail": "Repeated reply"}])
        self.write_source([self.message(0, at=100, text="Repeated reply"), self.message(1, at=101, text="Repeated reply")])
        page = self.complete()
        self.assertEqual(len(page["events"]), 4)
        self.assertEqual(len({row["history_id"] for row in page["events"]}), 4)
        self.assertTrue(all(len(row["original_ids"]) == 1 for row in page["events"]))

    def test_late_stop_does_not_create_phantom_unread_after_provider_reply(self):
        self.write_source([self.message(0, at=100, text="Unique public reply with full details")])
        before = self.complete()
        provider = before["events"][0]
        self.assertEqual(provider["incoming_seq"], 1)
        self.journal([{"type": "Stop", "at": 101, "detail": "Unique public reply"}])
        after = self.complete()
        self.assertEqual(len(after["events"]), 1)
        self.assertEqual(after["head"], before["head"])
        self.assertEqual(after["history_epoch"], before["history_epoch"])
        row = after["events"][0]
        self.assertEqual(row["history_id"], provider["history_id"])
        self.assertEqual(row["incoming_seq"], 1)
        self.assertEqual(row["detail"], provider["detail"])
        self.assertEqual(row["type"], "AgentMessage")
        self.assertTrue(set(provider["original_ids"]).issubset(row["original_ids"]))
        self.assertTrue(any(alias.startswith("journal:") for alias in row["original_ids"]))
        read = self.history(around_incoming_seq=1, history_epoch=before["history_epoch"])
        self.assertEqual(read["head"]["incoming_seq"], 1)

    def test_same_native_message_id_body_revision_preserves_canonical_row(self):
        native_id = "stable-native-message"
        self.write_source([self.message(0, at=100, text="Partial public reply", message_id=native_id)])
        before = self.complete()
        partial = before["events"][0]
        self.append_source([self.message(0, at=100, text="Partial public reply with its final full body", message_id=native_id)])
        after = self.complete()
        self.assertEqual(len(after["events"]), 1)
        self.assertEqual(after["head"], before["head"])
        self.assertEqual(after["history_epoch"], before["history_epoch"])
        full = after["events"][0]
        self.assertEqual(full["history_id"], partial["history_id"])
        self.assertEqual(full["history_cursor"], partial["history_cursor"])
        self.assertEqual(full["incoming_seq"], partial["incoming_seq"])
        self.assertEqual(full["detail"], "Partial public reply with its final full body")
        self.assertEqual(full["message_id"], native_id)
        self.assertEqual(self.history()["events"], after["events"])

    def test_historical_backfill_changes_epoch_before_assigning_old_incoming(self):
        self.write_source([self.message(0, at=100, text="Already indexed first reply"),
                           self.message(1, at=101, text="Already indexed second reply")])
        before = self.complete()
        old_ids = [row["history_id"] for row in before["events"]]
        old_cursor = before["events"][0]["history_cursor"]
        self.assertEqual([row["incoming_seq"] for row in before["events"]], [1, 2])
        # The provider appends an earlier-timestamp public message after the
        # old complete head has already been observed/read. It must not be
        # classified as a newly arriving sequence 3 in that old epoch.
        self.append_source([self.message(2, at=50, text="Historical reply discovered later")])
        after = self.complete()
        self.assertNotEqual(after["history_epoch"], before["history_epoch"])
        self.assertEqual([row["detail"] for row in after["events"]],
                         ["Historical reply discovered later", "Already indexed first reply", "Already indexed second reply"])
        self.assertEqual([row["history_id"] for row in after["events"][1:]], old_ids)
        self.assertEqual([row["incoming_seq"] for row in after["events"]], [1, 2, 3])
        self.assertEqual(after["head"]["total_incoming"], 3)
        self.assertEqual(self.history()["source_status"]["canonical_order_reset"], "historical_backfill")
        self.history(self.request(before=old_cursor), reject=True)
        self.history(self.request(around_incoming_seq=2, history_epoch=before["history_epoch"]), reject=True)

    def test_attachment_only_public_receipt_hides_native_paths_and_bytes(self):
        request_id = str(uuid.uuid4())
        receipts = self.state / "input_receipts"
        receipts.mkdir(mode=0o700)
        private_path = str(self.root / "never-open-this-private-file")
        receipt = {"status": "submitted", "request_id": request_id, "run_id": self.run,
                   "conversation_id": self.conversation, "agent_id": "", "at": 100,
                   "text": "", "submitted_text": f'Read the attached file at "{private_path}"',
                   "attachments": [{"name": "synthetic.txt", "mime": "text/plain", "bytes": 7,
                                    "reference": "[File #1]", "path": private_path, "data_base64": "PRIVATE BYTES"}]}
        path = receipts / f"{request_id}.json"
        path.write_text(json.dumps(receipt));path.chmod(0o600)
        page = self.complete()
        self.assertEqual(len(page["events"]), 1)
        row = page["events"][0]
        self.assertEqual(row["detail"], "")
        self.assertEqual(row["message_id"], request_id)
        self.assertIsNone(row["incoming_seq"])
        self.assertEqual(row["attachments"][0]["name"], "synthetic.txt")
        self.assertNotIn(private_path, json.dumps(page))
        self.assertNotIn("PRIVATE BYTES", json.dumps(page))

    def test_delayed_attachment_acknowledgement_is_one_public_message_in_either_order(self):
        receipts = self.state / "input_receipts"
        receipts.mkdir(mode=0o700)
        for native_first in (False, True):
            request_id = str(uuid.uuid4())
            private_path = str(self.root / "attachments" / request_id / "synthetic.png")
            text = f"Compare synthetic images {native_first}"
            submitted = text + f'\n\nInspect the attached image at "{private_path}"'
            native = self.message(10 + int(native_first), incoming=False, at=1700000202 + int(native_first), text=submitted)
            receipt = {"status": "submitted", "request_id": request_id, "run_id": self.run,
                       "conversation_id": self.conversation, "agent_id": "", "submitted_at": 1700000100,
                       "text": text, "submitted_text": submitted,
                       "attachments": [{"name": "synthetic.png", "mime": "image/png", "bytes": 120, "path": private_path}]}
            if native_first:
                self.append_source([native]); self.complete()
            path = receipts / f"{request_id}.json"
            path.write_text(json.dumps(receipt)); path.chmod(0o600)
            self.complete()
            if not native_first:
                self.append_source([native])
            page = self.complete()
            matches = [row for row in page["events"] if row["detail"] == text]
            self.assertEqual(len(matches), 1)
            self.assertEqual(matches[0]["message_id"], request_id)
            self.assertNotIn(private_path, json.dumps(page))
            self.assertTrue(any(value.startswith("provider:") for value in matches[0]["original_ids"]))
        self.assertEqual(len(self.complete()["events"]), 2)

    def test_cached_delayed_attachment_projection_repairs_without_changing_epoch(self):
        request_id = str(uuid.uuid4())
        private_path = str(self.root / "attachments" / request_id / "synthetic.png")
        submitted = f'Compare images\n\nInspect the attached image at "{private_path}"'
        receipts = self.state / "input_receipts"; receipts.mkdir(mode=0o700)
        receipt = {"status": "submitted", "request_id": request_id, "run_id": self.run,
                   "conversation_id": self.conversation, "agent_id": "", "submitted_at": 100,
                   "text": "Compare images", "submitted_text": submitted,
                   "attachments": [{"name": "synthetic.png", "mime": "image/png", "bytes": 120, "path": private_path}]}
        path = receipts / f"{request_id}.json"; path.write_text(json.dumps(receipt)); path.chmod(0o600)
        before = self.complete()
        # Recreate an already-indexed v2 native row whose delayed hash was absent.
        # Its native source is unchanged: only the bounded upgrade can repair it.
        db_path = next((self.state / "public_history").glob("*.sqlite3"))
        original = "provider:codex_transcript:You:native:cached-native"
        native_id = hashlib.sha256(json.dumps([db_path.stem, original], separators=(",", ":")).encode()).hexdigest()
        native = {"type": "UserMessage", "source": "codex_transcript", "message_id": "cached-native",
                  "detail": submitted, "at": 202, "source_offset": "0:200"}
        with closing(sqlite3.connect(db_path)) as db, db:
            db.execute("INSERT INTO messages(id,at,role,stream,kind,ordinal,body) VALUES(?,?,?,?,?,?,?)",
                       (native_id, 202, "outgoing", "provider", "UserMessage", "1:00000000000000000000:00000000000000000200", json.dumps(native)))
            db.execute("INSERT INTO aliases(original,id) VALUES(?,?)", (original, native_id))
            db.execute("UPDATE meta SET value='2' WHERE key='reconcile_version'")
            db.execute("DELETE FROM reconcile_work")
        page = self.complete()
        self.assertEqual(page["history_epoch"], before["history_epoch"])
        self.assertEqual(len(page["events"]), 1)
        self.assertNotIn(private_path, json.dumps(page))
        old_id = page["events"][0]["history_id"]
        old_epoch = page["history_epoch"]
        with closing(sqlite3.connect(db_path)) as db, db:
            db.execute("UPDATE meta SET value='2' WHERE key='reconcile_version'")
        after = self.complete()
        self.assertEqual(after["history_epoch"], old_epoch)
        self.assertEqual(after["events"][0]["history_id"], old_id)
        self.assertEqual(after["head"]["total_incoming"], 0)

    def test_partial_text_is_disclosed_and_questions_are_outgoing(self):
        self.write_source([self.message(0, text="Ж" * 20000)])
        self.journal([{"type": "QuestionAnswered", "at": 101, "detail": "literal selected answer", "message_id": str(uuid.uuid4())}])
        page = self.complete()
        incoming = next(row for row in page["events"] if row["type"] == "AgentMessage")
        self.assertTrue(incoming["detail_truncated"])
        self.assertLessEqual(len(incoming["detail"].encode()), 32 * 1024)
        self.assertTrue(incoming["detail"].startswith("Ж"))
        answered = next(row for row in page["events"] if row["type"] == "QuestionAnswered")
        self.assertIsNone(answered["incoming_seq"])
        self.assertEqual(page["head"]["total_incoming"], 1)


if __name__ == "__main__":
    unittest.main()
