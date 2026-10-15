"""Black-box archived inspections: an immutable archive UUID, never a reusable name.

A live session and an archive may share a name. Only an exact archive of the
latest snapshot is inspected, with its own native argument, and only an answer
naming that archive is returned; nothing falls back to the live session."""
import unittest

from bb_operations import OperationCase, listing, validation_cases
from connector_harness import BlackBoxCase, default_rules, session_row

NAME = "codex/project/nested/tag"
ARCHIVE = "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa"
OTHER = "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb"
LIVE = session_row(name=NAME, run_id="live-run", conversation_id="live-conversation")
SAVED = session_row(name=NAME, run_id="saved-run", conversation_id="saved-conversation", state="archived",
                    archive_id=ARCHIVE, runtime_state="archived", process_state="stopped")
ARCHIVED_ARGV = ["inspect", NAME, "--archive", ARCHIVE]
ARCHIVE_MISMATCH = "native archive inspection identity did not match"
ORDINARY_MISMATCH = "native ordinary inspection returned a different or archived target"
UNKNOWN = "archive must match an exact local snapshot entry"
NOT_LIVE = "session must match an exact nonarchived snapshot entry"


def saved_detail(**changes) -> dict:
    return {"name": NAME, "archive_id": ARCHIVE, "run_id": "saved-run", "conversation_id": "saved-conversation",
            "state": "archived", **changes}


def live_detail(**changes) -> dict:
    return {"name": NAME, "run_id": "live-run", "conversation_id": "live-conversation", **changes}


class ArchiveTests(OperationCase, BlackBoxCase):
    async def asyncSetUp(self):
        await super().asyncSetUp()
        self.answer(LIVE, SAVED)

    def answer(self, *rows, saved=None, live=None, projects=None) -> None:
        rules = [{"argv": ARCHIVED_ARGV, "stdout": saved or saved_detail()},
                 {"argv": ["inspect", NAME], "stdout": live or live_detail()}]
        if projects is not None:
            rules.append({"argv": ["swarm", "get"], "stdout": projects})
        self.hgs.script(rules + default_rules(listing(*rows)))

    def inspect(self, archive=ARCHIVE, session=NAME, **payload):
        if archive is not None:
            payload["archive_id"] = archive
        return self.operation("inspect", session=session, identity=False, **payload)

    async def test_a_live_session_and_its_archive_use_distinct_native_arguments(self):
        process = await self.launch()
        mark = self.mark()
        archived = await self.submit(self.inspect(), process)
        live = await self.submit(self.inspect(None), process)
        self.assertEqual(archived, {"state": "completed", "result": saved_detail(), "error": None})
        self.assertEqual(live, {"state": "completed", "result": live_detail(), "error": None})
        self.assertEqual([call["argv"] for call in self.work(mark)], [ARCHIVED_ARGV, ["inspect", NAME]])

    async def test_invalid_unknown_and_renamed_archives_never_spawn(self):
        process = await self.launch()
        mark = self.mark()
        for archive, session, error in (
                ("", NAME, "request_id must be a canonical UUID"),
                ("../../outside", NAME, "request_id must be a canonical UUID"),
                (ARCHIVE.upper(), NAME, "request_id must be a canonical UUID"),
                (7, NAME, "request_id must be a canonical UUID"),
                (OTHER, NAME, UNKNOWN), (ARCHIVE, "codex/other/name", UNKNOWN)):
            with self.subTest(archive=archive, session=session):
                self.assertEqual(await self.submit(self.inspect(archive, session), process),
                                 {"state": "failed", "result": None, "error": error})
        null = self.inspect()
        null["payload"]["archive_id"] = None
        self.assertEqual((await self.submit(null, process))["error"], "request_id must be a canonical UUID")
        self.assertEqual(self.work(mark), [])
        await self.assert_rejections(
            process, [case for case in validation_cases("conversation", {"inspect"}) if "archive" in case["id"]],
            [["inspect"]])

    async def test_an_archived_name_has_no_live_or_mutation_fallback(self):
        self.answer(SAVED)
        process = await self.launch()
        mark = self.mark()
        self.assertEqual(await self.submit(self.inspect(None), process),
                         {"state": "failed", "result": None, "error": NOT_LIVE})
        for operation in ("send", "answer", "interrupt"):
            with self.subTest(operation):
                archived = self.operation(operation, session=NAME, archive_id=ARCHIVE, text="Hello")
                self.assertEqual((await self.submit(archived, process))["error"],
                                 "archived targets support inspection only")
                live = self.operation(operation, session=NAME, text="Hello")
                self.assertEqual((await self.submit(live, process))["error"], NOT_LIVE)
        self.assertEqual(self.work(mark), [])
        self.assertEqual((await self.submit(self.inspect(), process))["state"], "completed")

    async def test_an_archive_id_without_state_is_still_an_archive(self):
        row = session_row(name=NAME, run_id="saved-run", conversation_id="saved-conversation", archive_id=ARCHIVE)
        self.answer(row)
        process = await self.launch()
        mark = self.mark()
        self.assertEqual((await self.submit(self.inspect(None), process))["error"], NOT_LIVE)
        self.assertEqual(self.work(mark), [])
        self.assertEqual((await self.submit(self.inspect(), process))["state"], "completed")

    async def test_answers_for_another_target_fail_without_fallback(self):
        process = await self.launch()
        for name, request, answer, argv, error in (
                ("live answer", self.inspect(), {"saved": live_detail()}, ARCHIVED_ARGV, ARCHIVE_MISMATCH),
                ("other archive", self.inspect(), {"saved": saved_detail(archive_id=OTHER)}, ARCHIVED_ARGV,
                 ARCHIVE_MISMATCH),
                ("other name", self.inspect(), {"saved": saved_detail(name="codex/other/name")}, ARCHIVED_ARGV,
                 ARCHIVE_MISMATCH),
                ("raced archive", self.inspect(None), {"live": live_detail(archive_id=ARCHIVE)}, ["inspect", NAME],
                 ORDINARY_MISMATCH),
                ("archived state", self.inspect(None), {"live": live_detail(state="archived")}, ["inspect", NAME],
                 ORDINARY_MISMATCH),
                ("renamed", self.inspect(None), {"live": live_detail(name="codex/other/name")}, ["inspect", NAME],
                 ORDINARY_MISMATCH)):
            with self.subTest(name):
                self.answer(LIVE, SAVED, **answer)
                mark = self.mark()
                self.assertEqual(await self.submit(request, process), {"state": "failed", "result": None, "error": error})
                self.assertEqual([call["argv"] for call in self.work(mark)], [argv])

    async def test_a_live_session_and_its_archive_belong_to_their_own_projects(self):
        projects = {"schema": 1, "initialized": True, "node_id": "native-local", "swarm_id": "native-shared",
                    "machines": [], "organization": {"default_project": "general", "projects": [
                        {"id": "live-project", "name": "Live", "sessions": ["Synthetic computer\n" + NAME]},
                        {"id": "saved-project", "name": "Saved", "sessions": ["Synthetic computer\narchive\n" + ARCHIVE]},
                        {"id": "general", "name": "General", "sessions": []}]}}
        self.answer(LIVE, SAVED, projects=projects)
        await self.launch()
        rows = [row for row in self.relay.heartbeats()[-1].body["snapshot"]["sessions"] if row["name"] == NAME]
        self.assertEqual([(row.get("archive_id"), row["mobile_project_id"]) for row in rows],
                         [(None, "live-project"), (ARCHIVE, "saved-project")])


if __name__ == "__main__":
    unittest.main()
