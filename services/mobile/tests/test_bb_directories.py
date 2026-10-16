"""Black-box directory catalogs and explicit creation of one folder.

`directory_create` is its own capability. The connector confirms the parent
natively, runs one fixed literal command with the request UUID and accepts only
the exact native receipt; anything less is uncertain and never retried."""
import os
import signal
import unittest

from bb_operations import OperationCase, help_text, listing, real_hgs, validation_cases
from differential import REAL_HGS, untraced
from connector_harness import RESTART_ERROR, BlackBoxCase, default_rules

PATH = "/alias/new project;$(literal)"
CREATED = "/canonical/new project;$(literal)"
UNCONFIRMED = "native directory creation was not confirmed by exact identity"
EXPIRED = {"state": "uncertain", "result": {"status": "history_expired"},
           "error": "Native result history expired; this request will not execute again"}


class DirectoryCase(OperationCase, BlackBoxCase):
    markers = ("directory_create",)

    async def asyncSetUp(self):
        await super().asyncSetUp()
        self.hgs.script(default_rules(listing(), help_text(*self.markers)))

    def create(self, path=PATH, **changes):
        return self.operation("directory_create", session="", identity=False, own=True, path=path, **changes)

    def receipt(self, request, parent="/canonical", **changes) -> dict:
        name = request["payload"]["path"].rsplit("/", 1)[-1]
        return {"status": "created", "request_id": request["request_id"], "requested_path": request["payload"]["path"],
                "path": parent + "/" + name, "parent": parent, **changes}

    def answer(self, request, parent=None, created=None) -> list:
        """Script the parent check and the creation; return the creation argv."""
        argv = ["dirs", "--create", request["payload"]["path"], "--request-id", request["request_id"]]
        self.hgs.prepend({"argv": ["dirs", request["payload"]["path"].rsplit("/", 1)[0] or "/"],
                          **(parent or {"stdout": {"path": "/canonical", "directories": []}})},
                         {"argv": argv, **(created or {"stdout": {**self.receipt(request), "secret": "not projected"}})})
        return argv


class DirectoryTests(DirectoryCase):
    async def test_creation_runs_once_and_returns_only_the_exact_receipt(self):
        request = self.create()
        argv = self.answer(request)
        process = await self.launch()
        mark = self.mark()
        result = await self.submit(request, process)
        self.assertEqual(result, {"state": "completed", "error": None, "result": {
            "status": "created", "request_id": request["request_id"], "requested_path": PATH, "path": CREATED,
            "parent": "/canonical"}})
        self.assertEqual([call["argv"] for call in self.work(mark)], [["dirs", "/alias"], argv])
        self.assertEqual([call["stdin"] for call in self.work(mark)], ["", ""])
        await self.delivered(process, request)
        self.assertEqual(await self.submit(request, process, redeliver=True), result)
        # A different request under the preserved identity never executes.
        changed = {**request, "payload": {**request["payload"], "path": "/other/new"}}
        delivered = len(self.relay.deliveries)
        await self.relay.queue(changed, redeliver=True)
        await self.settle(request["request_id"], delivered, process)
        self.assertEqual(len(self.work(mark)), 2)
        self.assertEqual(self.relay.results(request["request_id"]), [result, result])

    async def test_capability_is_separate_from_launch(self):
        process = await self.launch()
        capabilities = self.relay.heartbeats()[-1].body["snapshot"]["mobile_capabilities"]
        self.assertIn("directory_create", capabilities["operations"])
        self.assertIn("directory_create_v1", capabilities["features"])
        self.assertFalse({"dirs", "catalog", "launch"} & set(capabilities["operations"]))
        process.stop()
        self.hgs.script(default_rules(listing(), help_text("launch")))
        process = await self.launch()
        capabilities = self.relay.heartbeats()[-1].body["snapshot"]["mobile_capabilities"]
        self.assertNotIn("directory_create", capabilities["operations"])
        self.assertNotIn("directory_create_v1", capabilities["features"])
        self.hgs.prepend({"argv": ["account", "ls"], "stdout": {"profiles": []}})
        catalog = await self.submit(self.operation("catalog", session="", identity=False, own=True), process)
        self.assertIs(catalog["result"]["directory_create_supported"], False)
        mark = self.mark()
        self.assertEqual(await self.submit(self.create(), process), {
            "state": "failed", "result": None, "error": "native explicit directory creation ABI is unavailable"})
        self.assertEqual(self.since(mark, "dirs"), [])
        process.stop()
        # Help that cannot be read advertises nothing optional.
        self.hgs.script([{"argv": ["--help"], "exit": 1}] + default_rules(listing(), help_text(*self.markers)))
        process = await self.launch()
        capabilities = self.relay.heartbeats()[-1].body["snapshot"]["mobile_capabilities"]
        self.assertNotIn("directory_create", capabilities["operations"])
        self.assertNotIn("directory_create_v1", capabilities["features"])

    async def test_an_unconfirmed_parent_never_dispatches_creation(self):
        process = await self.launch()
        for name, parent, error in (
                ("failed", {"exit": 1, "stderr": "synthetic\n"}, "native command failed"),
                ("relative", {"stdout": {"path": "canonical"}}, "native directory path is not canonical"),
                ("dot-dot", {"stdout": {"path": "/canonical/../elsewhere"}}, "native directory path is not canonical"),
                ("double slash", {"stdout": {"path": "//canonical"}}, "native directory path is not canonical"),
                ("trailing slash", {"stdout": {"path": "/canonical/"}}, "native directory path is not canonical"),
                ("no path", {"stdout": {}}, "invalid bounded native identity"),
                ("not an object", {"stdout": ["/canonical"]}, "native directory parent catalog is invalid")):
            with self.subTest(name):
                request = self.create()
                self.answer(request, parent=parent)
                mark = self.mark()
                self.assertEqual(await self.submit(request, process), {"state": "failed", "result": None, "error": error})
                self.assertEqual(self.since(mark, "dirs", "--create"), [])
        await self.assert_rejections(process, validation_cases("launch", {"directory_create"}), [["dirs"]])

    async def test_an_unconfirmed_creation_is_uncertain_and_never_retried(self):
        process = await self.launch()
        for name, created, error in (
                ("other parent", lambda request: {"stdout": self.receipt(request, "/elsewhere")}, UNCONFIRMED),
                ("other request", lambda request: {"stdout": {**self.receipt(request), "request_id": "other"}}, UNCONFIRMED),
                ("existing", lambda request: {"stdout": {**self.receipt(request), "status": "exists"}}, UNCONFIRMED),
                ("other folder", lambda request: {"stdout": {**self.receipt(request), "path": "/canonical/other"}},
                 UNCONFIRMED),
                ("other requested path", lambda request: {"stdout": {**self.receipt(request), "requested_path": "/x/y"}},
                 UNCONFIRMED),
                ("not an object", lambda request: {"stdout": [self.receipt(request)]}, UNCONFIRMED),
                ("failed", lambda request: {"exit": 1, "stderr": "File exists\n"}, "native command failed")):
            with self.subTest(name):
                request = self.create()
                argv = self.answer(request, created=created(request))
                mark = self.mark()
                result = await self.submit(request, process)
                self.assertEqual(result, {"state": "uncertain", "result": None, "error": error})
                await self.delivered(process, request)
                self.assertEqual(await self.submit(request, process, redeliver=True), result)
                self.assertEqual([call["argv"] for call in self.since(mark, "dirs", "--create")], [argv])

    async def test_a_crash_after_creation_never_creates_again(self):
        request = self.create()
        argv = self.answer(request, created={"stdout": self.receipt(request), "kill_parent": True})
        first = await self.launch()
        await self.relay.queue(request)
        self.assertEqual(await first.wait_exit(10), -signal.SIGKILL)
        self.assertEqual(self.journal.row(request["request_id"])["state"], "running")
        self.hgs.script([{key: value for key, value in rule.items() if key not in ("id", "kill_parent")}
                         for rule in self.hgs.rules])
        second = await self.launch()
        recovered = {"state": "uncertain", "result": None, "error": RESTART_ERROR}
        self.assertEqual(await self.until(lambda: self.relay.results(request["request_id"]),
                                          what="the recovered upload", process=second), [recovered])
        await self.delivered(second, request)
        second.stop()
        self.age(7 * 86400 + 1, request)
        third = await self.launch()
        self.assertEqual(await self.submit(request, third, redeliver=True), EXPIRED)
        self.assertEqual([call["argv"] for call in self.hgs.calls("dirs", "--create")], [argv])


class DirectoryCatalogTests(DirectoryCase):
    markers = ("launch",)

    async def test_vector_dirs_projection(self):
        process = await self.launch()
        await self.replay(process, "dirs-projection")

    async def test_catalog_fields_are_projected(self):
        process = await self.launch()
        for name, answer, error in (
                ("home", {"path": "/example/account", "parent": "/example", "home": "/example/account", "truncated": True,
                          "directories": [{"name": "repo", "path": "/example/account/repo"}], "private": "dropped"}, None),
                ("no directories", {"path": "/example/account"}, "native directory catalog is invalid"),
                ("no path", {"directories": []}, "native directory catalog is invalid")):
            with self.subTest(name):
                self.hgs.prepend({"argv": ["dirs", "~"], "stdout": answer})
                request = self.operation("dirs", session="", identity=False, own=True, path="~")
                result = await self.submit(request, process)
                if error is None:
                    self.assertEqual(result["result"], {**{key: value for key, value in answer.items() if key != "private"},
                                                        "request_id": request["request_id"]})
                else:
                    self.assertEqual(result, {"state": "failed", "result": None, "error": error})
        await self.assert_rejections(process, validation_cases("launch", {"dirs"}), [["dirs"]])


@untraced(REAL_HGS)
class NativeDirectoryTests(DirectoryCase):
    """The real hgs answers help and directory commands; nothing else."""

    async def test_real_cli_creates_one_literal_folder_through_a_link(self):
        native = real_hgs()
        if native is None:
            if os.environ.get("CI") == "true":
                self.fail("a built hgs is required in CI (ZERUS_DIRECTORY_TEST_HGS or target/debug/hgs)")
            self.skipTest("needs a built hgs")
        root = self.scratch / "native"
        (root / "parent").mkdir(parents=True)
        (root / "alias").symlink_to(root / "parent", target_is_directory=True)
        literal = "new project;$(literal) Русский"
        self.hgs.prepend({"argv": ["--help"], "exec": native}, {"prefix": ["dirs"], "exec": native})
        process = await self.launch()
        self.assertIn("directory_create_v1", self.relay.heartbeats()[-1].body["snapshot"]["mobile_capabilities"]["features"])
        request = self.create(str(root / "alias" / literal))
        mark = self.mark()
        result = await self.submit(request, process)
        self.assertEqual(result, {"state": "completed", "error": None, "result": {
            "status": "created", "request_id": request["request_id"], "requested_path": request["payload"]["path"],
            "path": str(root / "parent" / literal), "parent": str(root / "parent")}})
        await self.delivered(process, request)
        self.assertEqual(await self.submit(request, process, redeliver=True), result)
        self.assertEqual(len(self.since(mark, "dirs")), 2)
        created = root / "parent" / literal
        self.assertEqual(list(created.iterdir()), [])
        (created / "keep.txt").write_text("preserved")
        conflicting = await self.submit(self.create(str(root / "alias" / literal)), process)
        self.assertEqual(conflicting["state"], "uncertain")
        self.assertEqual((created / "keep.txt").read_text(), "preserved")
        missing = await self.submit(self.create(str(root / "missing" / "child")), process)
        self.assertEqual(missing["state"], "failed")
        self.assertFalse((root / "missing").exists())


if __name__ == "__main__":
    unittest.main()
