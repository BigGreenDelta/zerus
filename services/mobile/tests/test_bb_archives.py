"""Black-box archived inspections: an immutable archive UUID, never a reusable name.

A live session and an archive may share a name. Only an exact archive of the
latest snapshot is inspected, with its own native argument, and only an answer
naming that archive is returned; nothing falls back to the live session.

A CLI that pages archives lists live sessions only. Heartbeats then carry a
bounded preview of recent archives, `archive_list` serves checked pages, and an
archive a page listed stays admitted for an hour; any other archive is looked
up natively before anything runs for it."""
import json
import unittest

from bb_operations import OperationCase, help_text, listing, validation_cases
from connector_harness import BlackBoxCase, default_rules, session_row
from connector_harness import listing as base_listing

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


PAGED = "cccccccc-cccc-4ccc-8ccc-cccccccccccc"
PAGE_ARGV = ["archives", "--json"]
LOOKUP_ARGV = ["archives", "lookup", "--json"]
PAGED_ARGV = ["inspect", NAME, "--archive", PAGED]
MAX_DATE = 253402300800
PREVIEW_ERROR = "Archive preview is unavailable; refresh Archive on an updated client"


def archived_row(archive: str, at: float, **changes) -> dict:
    return session_row(name=NAME, run_id="saved-run", conversation_id="saved-conversation", state="archived",
                       archive_id=archive, archived_at=at, runtime_state="archived", process_state="stopped",
                       **changes)


def archive_page(*rows: dict, **changes) -> dict:
    return {"version": 1, "sessions": list(rows), "total": len(rows), "next_cursor": "", "from": 0,
            "until": MAX_DATE, "query": "", **changes}


class ArchivePageTests(OperationCase, BlackBoxCase):
    async def asyncSetUp(self):
        await super().asyncSetUp()
        self.script()

    def script(self, *rules, preview=None, revision="revision-1", markers=("archive_list",)):
        """A paging CLI: live sessions only, `preview` as the recent window."""
        listed = base_listing(sessions=[LIVE], archive_revision=revision, archive_paging=True)
        self.hgs.script(list(rules) + [
            {"argv": PAGE_ARGV, "stdin_fields": {}, "stdout": preview or archive_page()},
            {"argv": PAGED_ARGV, "stdout": saved_detail(archive_id=PAGED)},
            {"argv": ARCHIVED_ARGV, "stdout": saved_detail()},
        ] + default_rules(listed, help_text(*markers)))

    def page(self, **query):
        return self.operation("archive_list", session="", identity=False, **query)

    def inspect(self, archive=PAGED):
        return self.operation("inspect", session=NAME, identity=False, archive_id=archive)

    async def test_a_page_admits_its_archives_for_later_reads(self):
        listed = archive_page(archived_row(PAGED, 20.0), archived_row(ARCHIVE, 10.0), limit=None)
        listed.pop("limit")
        self.script({"argv": PAGE_ARGV, "stdin_fields": {"limit": 2}, "stdout": listed})
        process = await self.launch()
        mark = self.mark()
        response = await self.submit(self.page(limit=2), process)
        self.assertEqual(response["state"], "completed", response)
        result = response["result"]
        self.assertEqual([row["archive_id"] for row in result["sessions"]], [PAGED, ARCHIVE])
        # Only the projects these rows belong to come along, by id, name and colour.
        used = {row.get("mobile_project_id") for row in result["sessions"]}
        projects = result["mobile_projects"]["projects"]
        self.assertLessEqual({project["id"] for project in projects}, used)
        self.assertTrue(all(set(project) <= {"id", "name", "color"} for project in projects))
        call = self.work(mark)[0]
        self.assertEqual((call["argv"], json.loads(call["stdin"])), (PAGE_ARGV, {"limit": 2}))
        # The listed archive is read without a lookup.
        mark = self.mark()
        read = await self.submit(self.inspect(), process)
        self.assertEqual(read, {"state": "completed", "result": saved_detail(archive_id=PAGED), "error": None})
        self.assertEqual([call["argv"] for call in self.work(mark)], [PAGED_ARGV])

    async def test_an_archive_no_page_listed_is_looked_up_once(self):
        found = {"version": 1, "name": NAME, "archive_id": PAGED, "run_id": "saved-run",
                 "conversation_id": "saved-conversation"}
        self.script({"argv": LOOKUP_ARGV, "stdout": found})
        process = await self.launch()
        mark = self.mark()
        for _ in range(2):
            self.assertEqual((await self.submit(self.inspect(), process))["state"], "completed")
        calls = self.work(mark)
        self.assertEqual([call["argv"] for call in calls], [LOOKUP_ARGV, PAGED_ARGV, PAGED_ARGV])
        self.assertEqual(json.loads(calls[0]["stdin"]), {"name": NAME, "archive_id": PAGED})

    async def test_a_lookup_for_another_target_runs_nothing(self):
        for answer in ({"version": 1, "name": NAME, "archive_id": OTHER},
                       {"version": 1, "name": "codex/other/name", "archive_id": PAGED},
                       {"version": 2, "name": NAME, "archive_id": PAGED}):
            with self.subTest(answer=answer):
                self.script({"argv": LOOKUP_ARGV, "stdout": answer})
                process = await self.launch()
                mark = self.mark()
                self.assertEqual(await self.submit(self.inspect(), process),
                                 {"state": "failed", "result": None,
                                  "error": "fresh native archive identity did not match"})
                self.assertEqual([call["argv"] for call in self.work(mark)], [LOOKUP_ARGV])
                process.stop()

    async def test_an_invalid_native_page_fails_as_a_read(self):
        unordered = archive_page(archived_row(ARCHIVE, 10.0), archived_row(PAGED, 20.0))
        self.script({"argv": PAGE_ARGV, "stdin_fields": {"query": "x"}, "stdout": {**unordered, "query": "x"}})
        process = await self.launch()
        request = self.page(query="x")
        self.assertEqual(await self.submit(request, process),
                         {"state": "failed", "result": None, "error": "archive page is out of order"})
        self.assertEqual(self.journal.row(request["request_id"])["state"], "failed")

    async def test_heartbeats_list_live_sessions_and_a_bounded_preview(self):
        self.script(preview=archive_page(archived_row(PAGED, 20.0, prompt="p" * 300, private="kept"), total=5))
        process = await self.launch()
        await self.until(lambda: len(self.relay.heartbeats()) >= 2, what="two heartbeats", process=process)
        snapshot = self.relay.heartbeats()[-1].body["snapshot"]
        self.assertEqual(snapshot["mobile_archive"],
                         {"schema": 1, "available": True, "total": 5, "loaded": 1, "partial": True})
        preview = [row for row in snapshot["sessions"] if row.get("archive_id") == PAGED]
        self.assertEqual(len(preview), 1)
        self.assertEqual(len(preview[0]["prompt"]), 240)
        self.assertNotIn("private", preview[0])
        self.assertIn("archive_list", snapshot["mobile_capabilities"]["operations"])
        listings = self.since(0, "ls")
        self.assertTrue(listings and all(call["argv"] == ["ls", "--json", "--local", "--no-archives"]
                                         for call in listings))
        # One preview per listing revision within five minutes.
        self.assertEqual(len(self.since(0, *PAGE_ARGV)), 1)

    async def test_a_failed_preview_is_reported_and_admits_nothing(self):
        self.script({"argv": PAGE_ARGV, "stdin_fields": {}, "exit": 1})
        process = await self.launch()
        await self.until(lambda: self.relay.heartbeats(), what="a heartbeat", process=process)
        self.assertEqual(self.relay.heartbeats()[-1].body["snapshot"]["mobile_archive"],
                         {"schema": 1, "available": False, "loaded": 0, "total": None, "partial": False,
                          "error": PREVIEW_ERROR})

    async def test_a_cli_without_the_marker_keeps_the_legacy_listing(self):
        self.script(markers=())
        process = await self.launch()
        await self.until(lambda: self.relay.heartbeats(), what="a heartbeat", process=process)
        snapshot = self.relay.heartbeats()[-1].body["snapshot"]
        self.assertNotIn("archive_list", snapshot["mobile_capabilities"]["operations"])
        self.assertEqual(snapshot["mobile_archive"],
                         {"schema": 1, "available": False, "loaded": 0, "total": None, "partial": False})
        mark = self.mark()
        self.assertEqual(await self.submit(self.page(), process),
                         {"state": "failed", "result": None, "error": "Update the computer CLI to browse archive pages"})
        self.assertEqual(self.work(mark), [])
        self.assertTrue(all(call["argv"] == ["ls", "--json", "--local"] for call in self.since(0, "ls")))


if __name__ == "__main__":
    unittest.main()
