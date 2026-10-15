"""Black-box catalog and session launch, with and without a project.

A launch runs one fixed native command carrying the request UUID as its launch
ID, after fresh account and directory checks, and is confirmed only by a
listed session with that launch ID. A project assignment follows a confirmed
launch; its failure never repeats the launch."""
import json
import time
import unittest

from bb_operations import OperationCase, help_text, listing, validation_cases
from connector_harness import BlackBoxCase, default_rules, session_row

DIRECTORY = "/example/mobile"
ACCOUNTS = {"profiles": [
    {"id": "native-codex", "provider": "codex", "label": "Synthetic Codex", "installed": True,
     "home": "/example/private-home", "auth_revision": "bb-private-revision", "secret": "must not escape"},
    {"id": "native-opencode", "provider": "opencode", "label": "Native OpenCode", "installed": True,
     "model_options": [{"value": "ollama/qwen3-coder:30b", "label": "Local coding model", "api_key": "private"}]}]}
UNVERIFIED = "native launch could not be verified by exact launch UUID; check computers before creating another"


def native_projects(**changes) -> dict:
    """`hgs swarm get` with one local and one remote folder."""
    return {"schema": 1, "initialized": True, "node_id": "native-local", "swarm_id": "native-shared",
            "machines": [{"id": "native-local", "connection": "Synthetic computer", "local": True}],
            "peers": {"private-peer": {"private": "must not leave"}},
            "organization": {"version": 2, "default_project": "general", "projects": [
                {"id": "logical", "name": "Logical project", "color": "#123456", "accessible": True,
                 "folders": [{"id": "f-local", "name": "Root", "path": "/example/link", "machine_id": "native-local",
                              "machine_name": "Local"},
                             {"id": "f-anchor", "name": "Repository", "path": "/example/repo",
                              "machine_id": "native-local", "machine_name": "Local"},
                             {"id": "f-remote", "name": "Remote", "path": "/private/remote",
                              "machine_id": "native-remote", "machine_name": "Remote"}],
                 "sessions": []},
                {"id": "general", "name": "General", "color": "#8d9baa", "accessible": True, "folders": [],
                 "sessions": []},
                {"id": "hidden", "name": "Hidden", "color": "#112233", "accessible": False, "folders": [],
                 "sessions": []}]}, **changes}


class LaunchCase(OperationCase, BlackBoxCase):
    markers = ("launch",)

    async def asyncSetUp(self):
        await super().asyncSetUp()
        self.base = [{"argv": ["account", "ls"], "stdout": ACCOUNTS},
                     {"argv": ["dirs", DIRECTORY], "stdout": {"path": DIRECTORY, "directories": []}}]
        self.hgs.script(self.base + default_rules(listing(), help_text(*self.markers)))

    def launch_request(self, *, tag="new-session", **payload):
        payload = {"agent": "codex", "directory": DIRECTORY, "tag": tag, **payload}
        return self.operation("launch", session="", identity=False, own=True, **payload)

    def target(self, request, agent="codex", **changes) -> dict:
        return session_row(name=f"{agent}/example/{request['payload']['tag']}", run_id="new-run",
                           conversation_id="new-conversation", launch_id=request["request_id"], **changes)

    def answer_launch(self, request, *rows, launched=None, assignment=None, projects=None) -> list[str]:
        """Script a launch that lists `rows`; return its expected native argv."""
        payload = request["payload"]
        argv = [payload["agent"], DIRECTORY, "--new", "-n", payload["tag"], "-d", "--launch-id", request["request_id"]]
        argv += ["--account", payload["account_id"]] if "account_id" in payload else []
        argv += ["--model", payload["model"]] if "model" in payload else []
        rules = [{"argv": argv, **({"stdout_text": "Created\n"} if launched is None else launched)}]
        if assignment is not None:
            rules.append({"argv": ["swarm", "assign-launch", "--json"], **assignment})
        if projects is not None:
            rules.append({"argv": ["swarm", "get"], "stdout": projects})
        self.hgs.script(rules + self.base + default_rules(listing(*rows), help_text(*self.markers)))
        return argv


class LaunchTests(LaunchCase):
    markers = ("launch", "project_launch", "worktree", "directory_create")

    async def test_vector_catalog_projection(self):
        process = await self.launch()
        await self.replay(process, "catalog-projection")

    async def test_launch_runs_once_and_returns_its_exact_target(self):
        request = self.launch_request(account_id="native-codex")
        argv = self.answer_launch(request, self.target(request))
        process = await self.launch()
        mark = self.mark()
        result = await self.submit(request, process)
        self.assertEqual(result, {"state": "completed", "error": None, "result": {
            "request_id": request["request_id"], "status": "created", "result_target": {
                "name": "codex/example/new-session", "run_id": "new-run", "conversation_id": "new-conversation"}}})
        self.assertEqual([call["argv"] for call in self.work(mark, keep=[["account", "ls"]])],
                         [["account", "ls"], ["dirs", DIRECTORY], argv])
        self.assertTrue(self.since(mark, "ls", "--json", "--local"), "the launch was not verified by a listing")
        await self.delivered(process, request)
        self.assertEqual(await self.submit(request, process, redeliver=True), result)
        self.assertEqual(len(self.since(mark, "codex")), 1)

    async def test_opencode_models_must_still_be_offered(self):
        offered = self.launch_request(agent="opencode", account_id="native-opencode", model="ollama/qwen3-coder:30b")
        argv = self.answer_launch(offered, self.target(offered, "opencode"))
        process = await self.launch()
        mark = self.mark()
        self.assertEqual((await self.submit(offered, process))["state"], "completed")
        self.assertEqual(self.since(mark, "opencode")[0]["argv"], argv)
        default = self.launch_request(agent="opencode", account_id="native-opencode", tag="default-model")
        argv = self.answer_launch(default, self.target(default, "opencode"))
        self.assertNotIn("--model", argv)
        self.assertEqual((await self.submit(default, process))["state"], "completed")
        mark = self.mark()
        withdrawn = self.launch_request(agent="opencode", account_id="native-opencode", model="other/local", tag="other")
        self.assertEqual(await self.submit(withdrawn, process), {
            "state": "failed", "result": None, "error": "selected native model is no longer offered by this account"})
        self.assertEqual([call["argv"] for call in self.work(mark, keep=[["account", "ls"]])], [["account", "ls"]])

    async def test_failed_preflights_never_launch(self):
        process = await self.launch()
        for name, request, answer, calls, error in (
                ("uninstalled provider", self.launch_request(agent="kimi"), None, [["account", "ls"]],
                 "selected native provider/account is unavailable"),
                ("unknown account", self.launch_request(account_id="native-other"), None, [["account", "ls"]],
                 "selected native provider/account is unavailable"),
                ("relative directory", self.launch_request(), {"path": "example/mobile"},
                 [["account", "ls"], ["dirs", DIRECTORY]], "native directory is unavailable"),
                ("missing directory", self.launch_request(), {"exit": 1},
                 [["account", "ls"], ["dirs", DIRECTORY]], "native command failed")):
            with self.subTest(name):
                if answer is not None:
                    self.hgs.prepend({"argv": ["dirs", DIRECTORY], **({"stdout": answer} if "exit" not in answer else answer)})
                mark = self.mark()
                self.assertEqual(await self.submit(request, process), {"state": "failed", "result": None, "error": error})
                self.assertEqual([call["argv"] for call in self.work(mark, keep=[["account", "ls"]])], calls)
        await self.assert_rejections(process, validation_cases("launch", {"catalog", "launch"}),
                                     [["codex"], ["opencode"], ["dirs"]])

    async def test_an_unconfirmed_launch_is_uncertain_and_never_repeats(self):
        process = await self.launch()
        for name, request, launched, rows in (
                ("native failure", self.launch_request(tag="failed"), {"exit": 1, "stderr": "synthetic\n"}, ()),
                ("never listed", self.launch_request(tag="unlisted"), None, ())):
            with self.subTest(name):
                argv = self.answer_launch(request, *rows, launched=launched)
                mark = self.mark()
                started = time.monotonic()
                result = await self.submit(request, process, timeout=20)
                error = "native command failed" if launched else UNVERIFIED
                self.assertEqual(result, {"state": "uncertain", "result": None, "error": error})
                if launched is None:
                    # The connector waits about five seconds for the listing.
                    self.assertGreaterEqual(time.monotonic() - started, 4.5)
                await self.delivered(process, request)
                self.assertEqual(await self.submit(request, process, redeliver=True), result)
                self.assertEqual([call["argv"] for call in self.since(mark, "codex")], [argv])

    async def test_without_the_launch_abi_nothing_runs(self):
        self.hgs.script(self.base + default_rules(listing()))
        process = await self.launch()
        operations = set(self.relay.heartbeats()[-1].body["snapshot"]["mobile_capabilities"]["operations"])
        self.assertFalse({"catalog", "dirs", "launch", "worktrees", "worktree_create", "directory_create"} & operations)
        mark = self.mark()
        for request in (self.launch_request(), self.operation("catalog", session="", identity=False, own=True)):
            self.assertEqual(await self.submit(request, process), {
                "state": "failed", "result": None, "error": "native exact launch-ID ABI is unavailable"})
        self.assertEqual(self.work(mark), [])


class ProjectLaunchTests(LaunchCase):
    markers = ("launch", "project_launch", "worktree")

    def project_request(self, **payload):
        return self.launch_request(tag="project launch", account_id="native-codex",
                                   **{"swarm_id": "native-shared", "project_id": "logical", "add_folder": True, **payload})

    def ack(self, request, status="assigned", **changes) -> dict:
        return {"request_id": request["request_id"], "name": "codex/example/project launch", "run_id": "new-run",
                "conversation_id": "new-conversation", "swarm_id": "native-shared", "project_id": "logical",
                "status": status, **changes}

    async def test_catalog_lists_accessible_projects_with_local_folders(self):
        self.hgs.prepend({"argv": ["swarm", "get"], "stdout": native_projects()})
        process = await self.launch()
        result = (await self.submit(self.operation("catalog", session="", identity=False, own=True), process))["result"]
        self.assertEqual((result["project_launch_supported"], result["swarm_id"], result["default_project"]),
                         (True, "native-shared", "general"))
        self.assertEqual([row["id"] for row in result["projects"]], ["logical", "general"])
        self.assertEqual([folder["id"] for folder in result["projects"][0]["folders"]], ["f-local", "f-anchor"])
        self.assertEqual(set(result["projects"][0]), {"id", "name", "color", "folders", "accessible"})
        text = json.dumps(result)
        for private in ("/private/remote", "must not leave", "/example/private-home", "bb-private-revision",
                        "must not escape"):
            self.assertNotIn(private, text)

    async def test_assignment_follows_a_confirmed_launch_once(self):
        request = self.project_request()
        argv = self.answer_launch(request, self.target(request), projects=native_projects(),
                                  assignment={"stdout": self.ack(request)})
        process = await self.launch()
        mark = self.mark()
        result = await self.submit(request, process)
        self.assertEqual(result["result"]["project_assignment"],
                         {"status": "assigned", "swarm_id": "native-shared", "project_id": "logical"})
        self.assertNotIn("warning", result["result"])
        work = [call["argv"] for call in self.work(mark, keep=[["account", "ls"], ["swarm", "get"]])]
        self.assertEqual(work[:4], [["account", "ls"], ["dirs", DIRECTORY], ["swarm", "get"], argv])
        [assignment] = self.since(mark, "swarm", "assign-launch")
        self.assertEqual(json.loads(assignment["stdin"]), {
            "request_id": request["request_id"], "name": "codex/example/project launch", "expected_run_id": "new-run",
            "expected_conversation_id": "new-conversation", "swarm_id": "native-shared", "project_id": "logical",
            "directory": DIRECTORY, "add_folder": True})
        # A confirmed assignment refreshes the projects at once, not after 15 s.
        await self.until(lambda: len(self.since(mark, "swarm", "get")) >= 2, 5, "a fresh project catalog", process)
        await self.delivered(process, request)
        self.assertEqual(await self.submit(request, process, redeliver=True), result)
        self.assertEqual((len(self.since(mark, "codex")), len(self.since(mark, "swarm", "assign-launch"))), (1, 1))

    async def test_failed_assignment_keeps_the_created_session(self):
        process = await self.launch()
        for name, assignment, status in (("native failed", "failed", "failed"),
                                         ("native error", {"exit": 1}, "uncertain"),
                                         ("other run", {"run_id": "replacement"}, "uncertain")):
            with self.subTest(name):
                request = self.project_request()
                if isinstance(assignment, str):
                    rule = {"stdout": self.ack(request, assignment)}
                elif "exit" in assignment:
                    rule = assignment
                else:
                    rule = {"stdout": self.ack(request, **assignment)}
                self.answer_launch(request, self.target(request), projects=native_projects(), assignment=rule)
                mark = self.mark()
                result = await self.submit(request, process)
                self.assertEqual(result["state"], "completed", result)
                self.assertEqual(result["result"]["result_target"]["name"], "codex/example/project launch")
                self.assertEqual(result["result"]["project_assignment"]["status"], status)
                self.assertEqual(result["result"]["warning"], "The session was created, but its project assignment is "
                                 + status + ". Review Projects; this launch will not be repeated.")
                await self.delivered(process, request)
                self.assertEqual(await self.submit(request, process, redeliver=True), result)
                self.assertEqual(len(self.since(mark, "codex")), 1)

    async def test_saved_and_related_folders_are_checked_natively(self):
        process = await self.launch()
        anchor = {"state": "ok", "path": "/example/repo", "common_dir": "/example/repo/.git", "stale": False,
                  "worktrees": [{"path": "/example/repo", "kind": "main", "available": True},
                                {"path": DIRECTORY, "kind": "linked", "available": True}]}
        for name, payload, extra, error in (
                ("saved folder through a link", {"project_folder_id": "f-local", "add_folder": False},
                 [{"argv": ["dirs", "/example/link"], "stdout": {"path": DIRECTORY}}], None),
                ("saved folder elsewhere", {"project_folder_id": "f-local", "add_folder": False},
                 [{"argv": ["dirs", "/example/link"], "stdout": {"path": "/example/elsewhere"}}],
                 "selected local project folder changed"),
                ("related worktree", {"add_folder": False, "worktree_folder_id": "f-anchor",
                                      "worktree_common_dir": "/example/repo/.git"},
                 [{"argv": ["dirs", "/example/repo"], "stdout": {"path": "/example/repo"}},
                  {"argv": ["__state", "worktrees", "--path", "/example/repo", "--refresh", "--json"], "stdout": anchor}],
                 None),
                ("unrelated folder", {"add_folder": False, "worktree_folder_id": "f-anchor",
                                      "worktree_common_dir": "/example/repo/.git"},
                 [{"argv": ["dirs", "/example/repo"], "stdout": {"path": "/example/repo"}},
                  {"argv": ["__state", "worktrees", "--path", "/example/repo", "--refresh", "--json"],
                   "stdout": {**anchor, "worktrees": anchor["worktrees"][:1]}}],
                 "choose a current project folder, a verified related worktree, or explicitly add the folder"),
                ("changed swarm", {"swarm_id": "other-swarm"}, [], "project catalog identity changed"),
                ("unknown project", {"project_id": "missing"}, [], "selected native project is unavailable")):
            with self.subTest(name):
                request = self.project_request(**payload)
                self.answer_launch(request, self.target(request), projects=native_projects(),
                                   assignment={"stdout": self.ack(request, project_id=request["payload"]["project_id"])})
                self.hgs.prepend(*extra)
                mark = self.mark()
                result = await self.submit(request, process)
                if error is None:
                    self.assertEqual(result["result"]["project_assignment"]["status"], "assigned", result)
                    self.assertEqual(len(self.since(mark, "codex")), 1)
                else:
                    self.assertEqual(result, {"state": "failed", "result": None, "error": error})
                    self.assertEqual(self.since(mark, "codex"), [])

    async def test_without_the_assignment_abi_a_project_launch_never_starts(self):
        self.markers = ("launch",)
        request = self.project_request()
        self.answer_launch(request, self.target(request), projects=native_projects())
        process = await self.launch()
        self.assertNotIn("launch_project", self.relay.heartbeats()[-1].body["snapshot"]["mobile_capabilities"]["features"])
        mark = self.mark()
        self.assertEqual(await self.submit(request, process), {
            "state": "failed", "result": None, "error": "native scoped project assignment is unavailable"})
        self.assertEqual([call["argv"] for call in self.work(mark, keep=[["account", "ls"]])],
                         [["account", "ls"], ["dirs", DIRECTORY]])


if __name__ == "__main__":
    unittest.main()
