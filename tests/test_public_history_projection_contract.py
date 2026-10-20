"""Production CLI wire on synthetic stopped bindings and private SQLite only.

No agent, tmux, relay or source migration is executed. Synthetic ledger entries
exercise transport identity and resolution after ordinary history settles.
"""
from contextlib import closing
import hashlib
import json
from pathlib import Path
import sqlite3
import subprocess
import unittest

import test_public_history_contract as fixtures


class PublicHistoryProjectionContract(unittest.TestCase):
    setUp = fixtures.PublicHistoryContract.setUp
    write_binding = fixtures.PublicHistoryContract.write_binding
    write_source = fixtures.PublicHistoryContract.write_source
    request = fixtures.PublicHistoryContract.request
    history = fixtures.PublicHistoryContract.history
    complete = fixtures.PublicHistoryContract.complete
    message = staticmethod(fixtures.PublicHistoryContract.message)

    def projection(self, epoch, after=0, until=0, reject=False, **changes):
        payload = self.request(history_epoch=epoch, projection_after=after, projection_until=until)
        payload.pop("limit")
        payload.update(changes)
        result = subprocess.run([str(fixtures.HGS), "history-projection", self.name, "--json"],
                                input=json.dumps(payload), text=True, capture_output=True,
                                env=self.env, timeout=5)
        if reject:
            self.assertNotEqual(result.returncode, 0, result.stdout)
            return result.stderr
        self.assertEqual(result.returncode, 0, result.stderr)
        raw = json.loads(result.stdout)
        for actual, expected in [("request_id", "request_id"), ("run_id", "expected_run_id"),
                                 ("conversation_id", "expected_conversation_id")]:
            self.assertEqual(raw[actual], payload[expected])
        self.assertEqual(raw["name"], self.name)
        self.assertEqual(raw["history_epoch"], epoch)
        self.assertLessEqual(len(json.dumps(raw, ensure_ascii=True, separators=(",", ":")).encode()), 880 * 1024)
        return raw

    def index(self):
        paths = list((self.state / "public_history").glob("*.sqlite3"))
        self.assertEqual(len(paths), 1)
        return paths[0]

    def snapshot(self):
        root = self.state / "public_history"
        return {str(p.relative_to(root)): hashlib.sha256(p.read_bytes()).hexdigest()
                for p in root.iterdir() if p.is_file()} if root.exists() else {}

    def test_missing_index_is_not_created_and_own_help_marker_is_present(self):
        self.assertIn("not ready", self.projection("absent", reject=True))
        self.assertFalse((self.state / "public_history").exists())
        help_result = subprocess.run([str(fixtures.HGS), "--help"], env=self.env, capture_output=True, text=True, timeout=5)
        self.assertEqual(help_result.returncode, 0)
        lines = [line for line in help_result.stdout.splitlines() if "history-projection <session> --json" in line]
        self.assertEqual(len(lines), 1)
        self.assertNotIn("history <session> --json", lines[0])

    def test_settled_tail_retains_epoch_and_zero_generation(self):
        head = self.complete()
        self.assertEqual(head["source_status"]["projection_upgrade"]["generation"], 0)
        raw = self.projection(head["history_epoch"])
        self.assertEqual(raw["projection_changes"], [])
        self.assertEqual(raw["next_projection_after"], 0)
        self.assertIs(raw["complete"], True)
        again = self.complete()
        self.assertEqual(again["history_epoch"], head["history_epoch"])

    def test_resolved_retirement_refresh_and_unresolved_row_keep_the_exact_page_body(self):
        self.write_source([self.message(1, incoming=False), self.message(2)])
        head = self.complete()
        row = next(row for row in head["events"] if row["type"] == "UserPromptSubmit")
        with closing(sqlite3.connect(self.index())) as db, db:
            for identity, change in [("a" * 64, "retired"), (row["history_id"], "refreshed"), ("b" * 64, "refreshed")]:
                db.execute("INSERT INTO projection_changes(epoch,id,change) VALUES(?,?,?)", (head["history_epoch"], identity, change))
        raw = self.projection(head["history_epoch"], until=3)
        self.assertEqual([entry["generation"] for entry in raw["projection_changes"]], [1, 2, 3])
        retired, refreshed, unresolved = raw["projection_changes"]
        self.assertEqual((retired["resolution"], retired["successor"]), ("resolved", None))
        self.assertEqual(refreshed["successor"], row)
        self.assertEqual((unresolved["resolution"], unresolved["successor"], unresolved["reason"]), ("unresolved", None, "no_successor"))
        tail = self.projection(head["history_epoch"], after=3, until=3)
        self.assertEqual(tail["projection_changes"], [])
        self.assertEqual(tail["next_projection_after"], 3)

    def test_refused_ranges_and_identity_change_nothing(self):
        head = self.complete()
        before = self.snapshot()
        for changes in [{"projection_after": 0.0}, {"projection_after": False}, {"projection_until": "0"},
                        {"projection_until": 1}, {"projection_after": -1}, {"expected_run_id": "other"},
                        {"expected_conversation_id": "other"}, {"extra": True}]:
            self.projection(head["history_epoch"], reject=True, **changes)
            self.assertEqual(self.snapshot(), before)
        self.projection("wrong-epoch", reject=True)
        self.assertEqual(self.snapshot(), before)

    def test_unsettled_or_legacy_index_is_refused_without_migration(self):
        head = self.complete()
        path = self.index()
        with closing(sqlite3.connect(path)) as db, db:
            db.execute("UPDATE meta SET value='0' WHERE key='reconciled'")
        before = self.snapshot()
        self.assertIn("still indexing", self.projection(head["history_epoch"], reject=True))
        self.assertEqual(self.snapshot(), before)
        with closing(sqlite3.connect(path)) as db, db:
            db.execute("UPDATE meta SET value='1' WHERE key='schema'")
        before = self.snapshot()
        self.assertIn("not ready", self.projection(head["history_epoch"], reject=True))
        self.assertEqual(self.snapshot(), before)


if __name__ == "__main__":
    unittest.main()
