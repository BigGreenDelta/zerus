"""Black-box journal files: creation, reuse in place and unsafe states refused."""
import hashlib
import json
import os
import sqlite3
import stat
import time
import unittest
import uuid

from connector_harness import BlackBoxCase, RESTART_ERROR, STARTUP_FAILURE

COLUMNS = ["request_id", "operation", "state", "response", "delivered", "request_hash",
           "created_at", "updated_at", "delivered_at", "response_expired"]
INDEXES = {"analytics_delivered_reads", "archive_delivered_reads", "delivered_history", "terminal_delivered_reads", "attachment_delivered_reads", "file_delivered_reads",
           "journal_operation", "delivered_reads", "mutation_retention", "project_assignment_delivered_v1"}
LEGACY = ("CREATE TABLE requests(request_id TEXT PRIMARY KEY,operation TEXT NOT NULL,"
          "state TEXT NOT NULL,response TEXT,delivered INTEGER NOT NULL DEFAULT 0)")


def mode(path):
    return stat.S_IMODE(path.lstat().st_mode)


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


class JournalTests(BlackBoxCase):
    def binding(self, url=None, token=None):
        return hashlib.sha256(((url or self.relay.url) + "\0" + (token or self.relay.token)).encode()).hexdigest()

    async def refused(self, config=None):
        process = await self.launch(config, ready=False)
        self.assertEqual(await process.wait_exit(10), 1, process.output())
        self.assertIn(STARTUP_FAILURE, process.output())

    async def test_first_start_creates_a_private_journal_bound_to_its_credentials(self):
        await self.launch()
        self.assertEqual(mode(self.state_dir), 0o700)
        for name in ("journal.sqlite3", "connector.lock"):
            self.assertEqual(mode(self.state_dir / name), 0o600, name)
            self.assertEqual((self.state_dir / name).lstat().st_nlink, 1, name)
        self.assertEqual(self.journal.metadata(), {"relay_binding": self.binding()})
        self.assertEqual(self.journal.columns(), COLUMNS)
        self.assertEqual(self.journal.indexes(), INDEXES)
        # While a Python rollback is supported the schema gains no objects.
        self.assertEqual({name for kind, name in self.journal.objects() if kind == "table"}, {"metadata", "requests"})
        self.assertEqual(self.journal.pragma("user_version"), 0)
        self.assertEqual(self.journal.pragma("journal_mode"), "delete")
        self.assertFalse((self.state_dir / "journal.sqlite3-wal").exists())

    async def test_default_and_home_relative_state_directories(self):
        for state_dir, expected in ((None, self.home / ".local/state/hgs/mobile-connector"),
                                    ("~/bb-journal", self.home / "bb-journal"),
                                    ("relative-journal", self.scratch / "relative-journal")):
            with self.subTest(state_dir=state_dir):
                process = await self.launch(self.config(state_dir=state_dir))
                self.assertEqual(mode(expected), 0o700)
                self.assertEqual(mode(expected / "journal.sqlite3"), 0o600)
                process.stop()
        self.assertFalse(self.state_dir.exists())

    async def test_restart_reuses_the_journal_in_place(self):
        first = await self.launch()
        request = self.request("send", text="before restart")
        result = await self.submit(request, first)
        await self.until(lambda: (row := self.journal.row(request["request_id"])) and row["delivered"],
                         what="the delivered journal row")
        before = self.journal.row(request["request_id"])
        inode = (self.state_dir / "journal.sqlite3").stat().st_ino
        first.stop()
        await self.launch()
        self.assertEqual((self.state_dir / "journal.sqlite3").stat().st_ino, inode)
        self.assertEqual(mode(self.state_dir / "journal.sqlite3"), 0o600)
        self.assertEqual(self.journal.metadata(), {"relay_binding": self.binding()})
        self.assertEqual(self.journal.row(request["request_id"]), before)
        self.assertEqual(json.loads(before["response"]), result)

    async def test_legacy_journal_is_migrated_in_place_and_keeps_its_outbox(self):
        self.state_dir.mkdir(mode=0o700)
        path = self.state_dir / "journal.sqlite3"
        os.close(os.open(path, os.O_CREAT | os.O_WRONLY, 0o600))
        read, write = self.request("inspect"), self.request("send", text="legacy")
        preserved = {"state": "completed", "result": {"text": "legacy result"}, "error": None}
        db = sqlite3.connect(path)
        with db:
            db.execute(LEGACY)
            db.execute("INSERT INTO requests VALUES(?,?,?,?,1)", (read["request_id"], "inspect", "completed", json.dumps(preserved)))
            db.execute("INSERT INTO requests VALUES(?,?,?,NULL,0)", (write["request_id"], "send", "running"))
        db.close()
        inode = path.stat().st_ino
        await self.relay.claimed(write)  # The connector that crashed had claimed it.
        started = time.time()
        process = await self.launch()
        recovered = {"state": "uncertain", "result": None, "error": RESTART_ERROR}
        self.assertEqual(await self.until(lambda: self.relay.results(write["request_id"]), what="the recovered upload",
                                          process=process), [recovered])
        self.assertEqual(path.stat().st_ino, inode)
        self.assertEqual(self.journal.columns(), COLUMNS)
        self.assertEqual(self.journal.indexes(), INDEXES)
        self.assertEqual(self.journal.metadata(), {"relay_binding": self.binding()})
        kept = self.journal.row(read["request_id"])
        # Legacy rows have no reliable age: they count from the upgrade.
        self.assertEqual((kept["state"], json.loads(kept["response"]), kept["delivered"], kept["request_hash"]),
                         ("completed", preserved, 1, None))
        self.assertGreaterEqual(kept["created_at"], started - 1)
        self.assertGreaterEqual(kept["delivered_at"], started - 1)
        # A legacy row has no request fingerprint, so no redelivery can match it.
        mark = len(self.relay.deliveries)
        await self.relay.queue(write, redeliver=True)
        await self.settle(write["request_id"], mark, process)
        self.assertEqual(self.relay.results(write["request_id"]), [recovered])
        self.assertEqual(self.hgs.calls("send"), [])

    async def test_other_credentials_never_take_over_a_journal(self):
        first = await self.launch()
        first.stop()
        before = digest(self.state_dir / "journal.sqlite3")
        for changes in ({"node_token": "bb-other-token"}, {"server_url": self.relay.url + "/other"}):
            with self.subTest(changes=changes):
                await self.refused(self.config(**changes))
                self.assertEqual(digest(self.state_dir / "journal.sqlite3"), before)

    async def test_second_connector_on_the_same_journal_is_refused(self):
        first = await self.launch()
        await self.refused()
        beats = len(self.relay.heartbeats())
        await self.until(lambda: len(self.relay.heartbeats()) > beats, what="the first connector's next heartbeat",
                         process=first)
        self.assertIsNone(first.returncode)

    async def test_unsafe_state_directory_is_refused_without_repair(self):
        shared = self.scratch / "shared"
        shared.mkdir(mode=0o755)
        os.chmod(shared, 0o755)
        await self.refused(self.config(state_dir=str(shared)))
        self.assertEqual(mode(shared), 0o755)
        self.assertEqual(os.listdir(shared), [])
        private = self.scratch / "private"
        private.mkdir(mode=0o700)
        link = self.scratch / "linked"
        link.symlink_to(private)
        await self.refused(self.config(state_dir=str(link)))
        self.assertEqual(os.listdir(private), [])
        regular = self.scratch / "regular"
        regular.write_text("not a directory")
        await self.refused(self.config(state_dir=str(regular)))
        self.assertEqual(regular.read_text(), "not a directory")

    async def test_unsafe_journal_files_are_refused_without_repair(self):
        target = self.scratch / "outside"
        target.write_text("synthetic private file")
        target.chmod(0o600)
        for name in ("connector.lock", "journal.sqlite3", "journal.sqlite3-journal", "journal.sqlite3-wal"):
            with self.subTest(symlink=name):
                directory = self.scratch / ("symlink-" + name)
                directory.mkdir(mode=0o700)
                (directory / name).symlink_to(target)
                await self.refused(self.config(state_dir=str(directory)))
                self.assertEqual(target.read_text(), "synthetic private file")
        hard = self.scratch / "hard-link"
        hard.mkdir(mode=0o700)
        os.link(target, hard / "journal.sqlite3")
        await self.refused(self.config(state_dir=str(hard)))
        readable = self.scratch / "readable"
        readable.mkdir(mode=0o700)
        os.close(os.open(readable / "journal.sqlite3", os.O_CREAT | os.O_WRONLY, 0o600))
        os.chmod(readable / "journal.sqlite3", 0o644)
        await self.refused(self.config(state_dir=str(readable)))
        self.assertEqual(mode(readable / "journal.sqlite3"), 0o644)
        self.assertEqual(target.read_text(), "synthetic private file")


if __name__ == "__main__":
    unittest.main()
