"""Black-box worktree catalogs and creation from a phone.

The connector reads repository evidence only through native hgs, rechecks it
fresh before creating, runs one bounded creation command and accepts only its
exact receipt. A creation in flight is never cut short by the connector: under
SIGTERM Python dies and leaves it to finish (divergence h), Rust waits for it."""
import json
import signal
import time
import unittest

from bb_operations import OPERATIONS, RECEIPTS, OperationCase, help_text, listing, validation_cases
from connector_harness import IMPLEMENTATION, RESTART_ERROR, BlackBoxCase, default_rules

REPOSITORY = "/example/repo"
COMMON = "/example/repo/.git"
CATALOG_ARGV = ["__state", "worktrees", "--path", REPOSITORY, "--refresh", "--json"]
CHANGED = "repository changed; refresh worktrees before creation"
# Python reports a refused worktree projection or receipt through its generic
# text: the specific reasons are ValueErrors outside its ConnectorError path.
GENERIC = RECEIPTS["execute:inspect-native-crash"]["error"]


def catalog(**changes) -> dict:
    return {"state": "ok", "path": REPOSITORY, "common_dir": COMMON, "stale": False, "partial": False,
            "selected_root": REPOSITORY, "private": "dropped",
            "worktrees": [{"path": REPOSITORY, "kind": "main", "available": True, "branch": "main", "head": "abc",
                           "private": "dropped"},
                          {"path": "/example/linked", "kind": "linked", "available": True, "detached": True,
                           "locked": False, "prunable": False}], **changes}


class WorktreeTests(OperationCase, BlackBoxCase):
    markers = ("launch", "worktree")

    async def asyncSetUp(self):
        await super().asyncSetUp()
        self.hgs.script([{"argv": ["dirs", "/alias"], "stdout": {"path": "/canonical", "directories": []}},
                         {"argv": CATALOG_ARGV, "stdout": catalog()}]
                        + default_rules(listing(), help_text(*self.markers)))

    def create(self, **changes):
        payload = {"path": REPOSITORY, "common_dir": COMMON, "destination": "/alias/new", "branch": "feature",
                   "base": "HEAD", **changes}
        return self.operation("worktree_create", session="", identity=False, own=True, **payload)

    def answer(self, request, rule=None) -> list:
        """Script the creation; return its native argv."""
        payload = request["payload"]
        argv = ["__state", "worktrees", "create", "--path", payload["path"], "--common-dir", payload["common_dir"],
                "--branch", payload["branch"], "--base", payload["base"], "--destination", payload["destination"],
                "--request-id", request["request_id"], "--json", "--mobile"]
        self.hgs.prepend({"argv": argv, **(rule or {"stdout": self.receipt(request)})})
        return argv

    def receipt(self, request, **changes) -> dict:
        return {"request_id": request["request_id"], "status": "created", "path": "/canonical/new",
                "common_dir": COMMON, "branch": "feature", "private": "dropped", **changes}

    async def test_vector_catalog_uses_the_state_namespace(self):
        process = await self.launch()
        await self.replay(process, "worktrees-state-prefix")

    async def test_catalog_is_projected_or_refused(self):
        process = await self.launch()
        projected = {"state": "ok", "path": REPOSITORY, "requested_path": REPOSITORY, "stale": False,
                     "partial": False, "common_dir": COMMON, "selected_root": REPOSITORY,
                     "worktrees": [{"path": REPOSITORY, "kind": "main", "available": True, "branch": "main",
                                    "head": "abc", "detached": False, "locked": False, "prunable": False},
                                   {"path": "/example/linked", "kind": "linked", "available": True, "detached": True,
                                    "locked": False, "prunable": False}]}
        for name, answer, expected in (
                ("projection", catalog(), projected),
                ("no repository identity", catalog(common_dir=None), GENERIC),
                ("unknown kind", catalog(worktrees=[{"path": REPOSITORY, "kind": "other"}]),
                 GENERIC),
                ("relative entry", catalog(worktrees=[{"path": "repo", "kind": "main"}]),
                 GENERIC),
                ("not a catalog", {"state": "ok"}, GENERIC)):
            with self.subTest(name):
                self.hgs.prepend({"argv": CATALOG_ARGV, "stdout": answer})
                request = self.operation("worktrees", session="", identity=False, own=True, path=REPOSITORY)
                result = await self.submit(request, process)
                if isinstance(expected, dict):
                    self.assertEqual(result["result"], {**expected, "request_id": request["request_id"]})
                else:
                    self.assertEqual(result, {"state": "failed", "result": None, "error": expected})
        await self.assert_rejections(process, validation_cases("launch", {"worktrees", "worktree_create"}),
                                     [["__state"], ["dirs"]])

    async def test_creation_runs_once_after_fresh_evidence(self):
        request = self.create()
        argv = self.answer(request)
        process = await self.launch()
        mark = self.mark()
        result = await self.submit(request, process)
        self.assertEqual(result, {"state": "completed", "error": None, "result": {
            "request_id": request["request_id"], "status": "created", "path": "/canonical/new", "common_dir": COMMON,
            "branch": "feature"}})
        self.assertEqual([call["argv"] for call in self.work(mark)], [["dirs", "/alias"], CATALOG_ARGV, argv])
        await self.delivered(process, request)
        self.assertEqual(await self.submit(request, process, redeliver=True), result)
        self.assertEqual(len(self.since(mark, "__state", "worktrees", "create")), 1)

    async def test_changed_repository_fails_before_creation(self):
        process = await self.launch()
        for name, fresh in (("other repository", catalog(common_dir="/other/.git")), ("stale", catalog(stale=True)),
                            ("unavailable", catalog(state="unavailable"))):
            with self.subTest(name):
                self.hgs.prepend({"argv": CATALOG_ARGV, "stdout": fresh})
                request = self.create()
                self.answer(request)
                mark = self.mark()
                self.assertEqual(await self.submit(request, process), {"state": "failed", "result": None, "error": CHANGED})
                self.assertEqual(self.since(mark, "__state", "worktrees", "create"), [])
        mark = self.mark()
        self.assertEqual(await self.submit(self.create(destination="/alias/.."), process),
                         {"state": "failed", "result": None, "error": "invalid worktree destination"})
        self.assertEqual(self.work(mark), [])

    async def test_an_unconfirmed_creation_is_uncertain_and_never_repeats(self):
        process = await self.launch()
        oversized = self.scratch / "oversized.json"
        oversized.write_text(json.dumps({"status": "created"}) + " " * OPERATIONS["constants"]["MAX_BYTES"])
        for name, rule, error in (
                ("other folder", lambda request: {"stdout": self.receipt(request, path="/elsewhere/new")}, GENERIC),
                ("other branch", lambda request: {"stdout": self.receipt(request, branch="other")}, GENERIC),
                ("failed", lambda request: {"exit": 1, "stderr": "synthetic\n"}, "native command failed"),
                ("oversized", lambda request: {"stdout_path": str(oversized)}, "native output exceeds size limit")):
            with self.subTest(name):
                request = self.create()
                argv = self.answer(request, rule(request))
                mark = self.mark()
                result = await self.submit(request, process)
                self.assertEqual(result, {"state": "uncertain", "result": None, "error": error})
                await self.delivered(process, request)
                self.assertEqual(await self.submit(request, process, redeliver=True), result)
                self.assertEqual([call["argv"] for call in self.since(mark, "__state", "worktrees", "create")], [argv])

    async def test_sigterm_never_cuts_a_creation_short(self):
        request = self.create()
        argv = self.answer(request, {"stdout": self.receipt(request), "delay": 1.5})
        first = await self.launch()
        await self.relay.queue(request)
        [call] = await self.until(lambda: self.hgs.calls(*argv), what="the native creation", process=first)
        first.signal(signal.SIGTERM)
        code = await first.wait_exit(40)
        exited_at = time.time()
        ended = await self.until(lambda: self.hgs.finished(call["pid"]), what="the creation to finish on its own")
        self.assertEqual(ended["exit"], 0)
        if IMPLEMENTATION == "python":
            self.assertEqual(code, -signal.SIGTERM)
            expected = {"state": "uncertain", "result": None, "error": RESTART_ERROR}
        else:
            self.assertEqual(code, 0)
            self.assertLessEqual(ended["at"], exited_at, "the connector exited before the creation finished")
            expected = {"state": "completed", "error": None, "result": {
                "request_id": request["request_id"], "status": "created", "path": "/canonical/new",
                "common_dir": COMMON, "branch": "feature"}}
            self.assertEqual(json.loads(self.journal.row(request["request_id"])["response"]), expected)
        second = await self.launch()
        self.assertEqual((await self.until(lambda: self.relay.results(request["request_id"]),
                                           what="the outcome upload", process=second))[0], expected)
        self.assertEqual(await self.submit(request, second, redeliver=True), expected)
        self.assertEqual(len(self.hgs.calls(*argv)), 1)

    async def test_without_the_worktree_abi_nothing_runs(self):
        self.hgs.script(default_rules(listing(), help_text("launch")))
        process = await self.launch()
        operations = set(self.relay.heartbeats()[-1].body["snapshot"]["mobile_capabilities"]["operations"])
        self.assertFalse({"worktrees", "worktree_create"} & operations)
        self.assertIn("launch", operations)
        mark = self.mark()
        for request in (self.create(), self.operation("worktrees", session="", identity=False, own=True, path=REPOSITORY)):
            self.assertEqual(await self.submit(request, process), {
                "state": "failed", "result": None, "error": "native mobile worktree ABI is unavailable"})
        self.assertEqual(self.work(mark), [])


if __name__ == "__main__":
    unittest.main()
