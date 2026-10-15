"""Black-box operations through one-hop routes: each reads and changes only
the selected machine, with that machine's own capabilities.

The peer context of a gateway has its own snapshot, accounts, projects and
help markers; every native call of an operation, preflights included, goes to
the machine its route names.
"""
import base64
import hashlib
import unittest
import uuid

from connector_harness import CONVERSATION_ID, IMPLEMENTATION, RUN_ID, SESSION, session_row
from gateway_harness import (Fleet, GatewayCase, LOCAL, OTHER, PEER, PEER_CONVERSATION, PEER_RUN,
                             Peer, ROUTE, error, gateway_help, help_text, inspected, peer_listing)

PROJECT_SWARM = {"schema": 1, "initialized": True, "node_id": PEER, "swarm_id": "bb-swarm", "organization": {
    "projects": [{"id": "project-peer", "name": "Peer project", "folders": [
        {"id": "folder-peer", "name": "peer", "path": "/example/peer", "machine_id": PEER}]}]}}
INSTALLED = [{"id": "account-peer", "provider": "claude", "label": "Peer account", "installed": True}]


def completed(result) -> dict:
    return {"state": "completed", "result": result, "error": None}


def failed(message: str) -> dict:
    return {"state": "failed", "result": None, "error": error(message)}


def target(call: dict) -> str:
    return call["envelope"]["target_machine_id"]


class PeerOperationTests(GatewayCase):
    async def test_catalog_history_and_project_launch_use_the_peer_context(self):
        launch = self.scoped("launch", agent="claude", directory="/example/peer", tag="new", account_id="account-peer",
                             swarm_id="bb-swarm", project_id="project-peer", project_folder_id="folder-peer",
                             add_folder=False)
        identity = launch["request_id"]
        launched = session_row(name="claude/example/new", run_id="bb-launched-run",
                               conversation_id="bb-launched-conversation", launch_id=identity)
        argv = ["claude", "/example/peer", "--new", "-n", "new", "-d", "--launch-id", identity, "--account", "account-peer"]
        peer = Peer(help=help_text("launch", "history", "project_launch"), accounts=INSTALLED,
                    snapshot=peer_listing(sessions=[session_row(run_id=PEER_RUN, conversation_id=PEER_CONVERSATION),
                                                    launched]),
                    answers=[(["swarm", "get"], {"stdout": PROJECT_SWARM}),
                             (["account", "inspect", "account-peer"], {"stdout": {
                                 "id": "account-peer", "provider": "claude", "status": "ok"}}),
                             (["dirs", "/example/peer"], {"stdout": {"path": "/example/peer", "directories": []}}),
                             (argv, {"stdout_text": "Created\n"}),
                             (["swarm", "assign-launch", "--json"], {"stdout": {
                                 "request_id": "{{stdin.payload.request_id}}", "name": "{{stdin.payload.name}}",
                                 "run_id": "{{stdin.payload.expected_run_id}}",
                                 "conversation_id": "{{stdin.payload.expected_conversation_id}}",
                                 "swarm_id": "{{stdin.payload.swarm_id}}", "project_id": "{{stdin.payload.project_id}}",
                                 "status": "assigned"}}),
                             (["history", SESSION, "--json"], {"stdout": {
                                 "request_id": "{{stdin.payload.request_id}}", "name": SESSION, "run_id": PEER_RUN,
                                 "conversation_id": PEER_CONVERSATION, "events": [], "history_epoch": "bb-peer-epoch",
                                 "head": {}}})])
        self.use(Fleet(peer))
        process = await self.launch()
        await self.gateway_ready(process)
        catalog = await self.submit(self.scoped("catalog"), process, gateway_route=self.route())
        self.assertEqual(catalog["state"], "completed", catalog)
        self.assertEqual(([row["id"] for row in catalog["result"]["accounts"]], catalog["result"]["agents"]),
                         (["account-peer"], ["claude"]))
        self.assertEqual((catalog["result"]["swarm_id"], [row["id"] for row in catalog["result"]["projects"]]),
                         ("bb-swarm", ["project-peer"]))
        history = self.submission("history", limit=20)
        self.assertEqual(await self.submit(history, process, gateway_route=self.route()), completed({
            "request_id": history["request_id"], "name": SESSION, "run_id": PEER_RUN,
            "conversation_id": PEER_CONVERSATION, "events": [], "history_epoch": "bb-peer-epoch", "head": {}}))
        [read] = self.through(argv=["history"])
        self.assertEqual((read["envelope"]["argv"], read["envelope"]["payload"]),
                         (["history", SESSION, "--json"], history["payload"]))
        self.assertEqual(await self.submit(launch, process, gateway_route=self.route()), completed({
            "request_id": identity, "status": "created",
            "result_target": {"name": "claude/example/new", "run_id": "bb-launched-run",
                              "conversation_id": "bb-launched-conversation"},
            "project_assignment": {"status": "assigned", "swarm_id": "bb-swarm", "project_id": "project-peer"}}))
        [started] = self.through(argv=["claude"])
        self.assertEqual((started["envelope"]["argv"], started["envelope"]["payload"]), (argv, None))
        [assigned] = self.through(argv=["swarm", "assign-launch"])
        self.assertEqual(assigned["envelope"]["payload"], {
            "request_id": identity, "name": "claude/example/new", "expected_run_id": "bb-launched-run",
            "expected_conversation_id": "bb-launched-conversation", "swarm_id": "bb-swarm",
            "project_id": "project-peer", "directory": "/example/peer", "add_folder": False,
            "project_folder_id": "folder-peer"})
        # A project the peer does not have is never launched there.
        elsewhere = self.scoped("launch", agent="claude", directory="/example/peer", tag="other",
                                account_id="account-peer", swarm_id="bb-swarm", project_id="project-gateway",
                                add_folder=True)
        self.assertEqual(await self.submit(elsewhere, process, gateway_route=self.route()),
                         failed("selected native project is unavailable"))
        self.assertEqual(len(self.through(argv=["claude"])), 1)
        self.assertEqual((self.guarded_calls(), self.hgs.calls("claude"), self.hgs.calls("history")), ([], [], []))

    async def test_mark_read_reaches_only_the_selected_machine(self):
        def answer(conversation: str) -> dict:
            return {"stdout": {"ok": True, "applied": {"reply_id": True, "attention_signature": False},
                               "read": {"conversation_id": conversation, "reply_id": "{{stdin.payload.reply_id}}",
                                        "attention_signature": None, "at": 1.0}}}

        self.use(Fleet(Peer(help=help_text("read"), answers=[(["read", SESSION, "--json"], answer(PEER_CONVERSATION))]),
                       help=gateway_help("read"), local=[(["read", SESSION, "--json"], answer(CONVERSATION_ID))]))
        process = await self.launch()
        await self.gateway_ready(process)
        for route, run, conversation, machine in ((self.route(), PEER_RUN, PEER_CONVERSATION, PEER),
                                                  (self.local_route(), RUN_ID, CONVERSATION_ID, LOCAL)):
            request = {**self.submission("inspect"), "operation": "mark_read", "payload": {
                "run_id": run, "conversation_id": conversation, "reply_id": "reply-" + run, "attention_signature": None}}
            result = await self.submit(request, process, gateway_route=route)
            self.assertEqual(result["state"], "completed", result)
            self.assertEqual(result["result"]["read"]["conversation_id"], conversation)
            [call] = (self.through if machine == PEER else self.guarded_calls)(argv=["read"])
            self.assertEqual((target(call), call["envelope"]["payload"]), (machine, request["payload"]))
        self.assertEqual(self.hgs.calls("read"), [])

    async def test_folders_and_worktrees_are_created_only_on_the_selected_machine(self):
        abi = ("launch", "worktree", "directory_create")
        requests = {}
        answers = {}
        for machine, root in ((PEER, "/example/peer"), (LOCAL, "/example/gateway")):
            folder = self.scoped("directory_create", path="/alias/new")
            worktree = self.scoped("worktree_create", path=root, common_dir=root + "/.git", destination=root + "/new",
                                   branch="feature", base="HEAD")
            requests[machine] = (folder, worktree)
            answers[machine] = [
                (["dirs", "/alias"], {"stdout": {"path": root, "directories": []}}),
                (["dirs", "--create", "/alias/new", "--request-id", folder["request_id"]], {"stdout": {
                    "status": "created", "request_id": folder["request_id"], "requested_path": "/alias/new",
                    "path": root + "/new", "parent": root}}),
                (["dirs", root], {"stdout": {"path": root, "directories": []}}),
                (["worktrees", "--path", root, "--refresh", "--json"], {"stdout": {
                    "path": root, "state": "ok", "stale": False, "common_dir": root + "/.git",
                    "worktrees": [{"path": root, "kind": "main", "available": True}]}}),
                (["worktrees", "create", "--path", root, "--common-dir", root + "/.git", "--branch", "feature",
                  "--base", "HEAD", "--destination", root + "/new", "--request-id", worktree["request_id"], "--json",
                  "--mobile"], {"stdout": {"request_id": worktree["request_id"], "status": "created",
                                           "path": root + "/new", "common_dir": root + "/.git", "branch": "feature"}}),
            ]
        # A second peer whose hgs predates explicit folder creation.
        old = Peer(via="bb-old-peer", machine=OTHER, help=help_text("launch", "worktree"))
        self.use(Fleet(Peer(help=help_text(*abi), answers=answers[PEER]), old, help=gateway_help(*abi),
                       local=answers[LOCAL]))
        process = await self.launch()
        await self.gateway_ready(process)
        for machine, route, root in ((PEER, self.route(), "/example/peer"),
                                     (LOCAL, self.local_route(), "/example/gateway")):
            folder, worktree = requests[machine]
            created = completed({"status": "created", "request_id": folder["request_id"],
                                 "requested_path": "/alias/new", "path": root + "/new", "parent": root})
            self.assertEqual(await self.submit(folder, process, gateway_route=route), created)
            self.assertEqual(await self.submit(folder, process, gateway_route=route, redeliver=True), created)
            self.assertEqual(await self.submit(worktree, process, gateway_route=route), completed({
                "request_id": worktree["request_id"], "status": "created", "path": root + "/new",
                "common_dir": root + "/.git", "branch": "feature"}))
            calls = self.through(machine=PEER) if machine == PEER else self.guarded_calls()
            self.assertEqual(len([call for call in calls if call["envelope"]["argv"][:2] == ["dirs", "--create"]]), 1)
            [create] = [call for call in calls if call["envelope"]["argv"][:2] == ["worktrees", "create"]]
            self.assertEqual(create["envelope"]["argv"][-2:], ["--json", "--mobile"])
        folder = self.scoped("directory_create", path="/alias/new")
        self.assertEqual(await self.submit(folder, process, gateway_route=self.route(old)),
                         failed("native explicit directory creation ABI is unavailable"))
        self.assertEqual(self.through(machine=OTHER, argv=["dirs"]), [])
        self.assertEqual((self.hgs.calls("dirs"), self.hgs.calls("worktrees"), self.hgs.calls("__state")), ([], [], []))

    async def test_the_delivery_setting_belongs_to_the_gateway(self):
        preference = ["swarm", "preference", "set", "mobile_delivery", "away_10", "--json"]
        self.use(Fleet(Peer(help=help_text("preference")), help=gateway_help("preference"),
                       local=[(preference, {"stdout": {"ok": True, "mobile_delivery": "away_10"}})]))
        process = await self.launch()
        await self.gateway_ready(process)
        self.assertIn("set_mobile_delivery", self.latest()["snapshot"]["mobile_capabilities"]["operations"])
        peer = self.relay.peer_heartbeats(ROUTE)[-1].body["snapshot"]["mobile_capabilities"]
        self.assertNotIn("set_mobile_delivery", peer["operations"])

        def setting() -> dict:
            return {"request_id": str(uuid.uuid4()), "computer_id": self.relay.computer_id,
                    "operation": "set_mobile_delivery", "session": "", "payload": {"value": "away_10"}}

        self.assertEqual(await self.submit(setting(), process, gateway_route=self.route()), failed(
            "the shared delivery setting is changed on the gateway computer with a current hgs"))
        self.assertEqual(self.through(argv=["swarm", "preference"]), [])
        self.assertEqual(await self.submit(setting(), process, gateway_route=self.local_route()),
                         completed({"ok": True, "mobile_delivery": "away_10"}))
        [call] = self.guarded_calls(["swarm", "preference"])
        self.assertEqual((call["envelope"]["argv"], call["envelope"]["payload"]), (preference, None))

    async def test_peer_attachment_read_follows_divergence_c(self):
        message = str(uuid.uuid4())
        data = b"peer attachment bytes"
        argv = ["attachment", SESSION, "--request", message, "--index", "0", "--conversation", PEER_CONVERSATION]
        self.use(Fleet(Peer(help=help_text("attachment_read"), answers=[
            (["inspect", SESSION, "--skip-processes"], {"stdout": inspected(PEER_RUN, PEER_CONVERSATION)}),
            (argv, {"stdout": {"request_id": message, "index": 0, "name": "note.txt", "mime": "text/plain",
                               "bytes": len(data), "data_base64": base64.b64encode(data).decode()}})])))
        process = await self.launch()
        await self.gateway_ready(process)
        request = self.submission("attachment_read", message_id=message, index=0, offset=5, length=10)
        result = await self.submit(request, process, gateway_route=self.route())
        if IMPLEMENTATION == "python":
            # The peer callback takes no output bound, so Python fails with
            # its generic text before it asks the peer for the file.
            self.assertEqual(result, failed("connector could not record native outcome"))
            self.assertEqual(self.through(argv=["attachment"]), [])
            return
        self.assertEqual(result, completed({
            "request_id": request["request_id"], "name": SESSION, "run_id": PEER_RUN,
            "conversation_id": PEER_CONVERSATION, "message_id": message, "index": 0, "file_name": "note.txt",
            "mime": "text/plain", "bytes": len(data), "sha256": hashlib.sha256(data).hexdigest(), "offset": 5,
            "length": 10, "data_base64": base64.b64encode(data[5:15]).decode()}))
        [call] = self.through(argv=["attachment"])
        self.assertEqual((target(call), call["envelope"]["argv"]), (PEER, argv))


if __name__ == "__main__":
    unittest.main()
