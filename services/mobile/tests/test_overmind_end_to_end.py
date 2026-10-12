"""Overmind from the phone, end to end: the phone API of a real relay, the gateway
connector and `hgs`.

The real `hgs` (target/debug/hgs) creates, reads and changes tasks. It runs with a
temporary HOME, XDG and state directories and none of the test process's environment
(no TMUX, no HGS_* variable). hgs puts `$HOME/.local/bin` first in PATH, so shims
there answer every tmux, ssh and agent call and reach no server, agent or peer.
Sending to a session and Your Call need a live agent, so fixture_hgs.py serves them.
Every case runs on the Rust relay when it is built, and on the reference relay.
"""
import asyncio
import base64
import contextlib
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch
import uuid

from aiohttp import ClientSession

from zerus_mobile import overmind
from zerus_mobile.connector import Connector
from zerus_mobile.server import Config
from zerus_mobile.store import Store
from relay_fixture import start_relay

spec = importlib.util.spec_from_file_location("fixture_overmind", Path(__file__).with_name("fixture_hgs.py"))
fixture_hgs = importlib.util.module_from_spec(spec)
spec.loader.exec_module(fixture_hgs)

REPO = Path(__file__).resolve().parents[3]
HGS = Path(os.environ.get("HGS_TEST_BIN", REPO / "target/debug/hgs")).resolve()
RUST_RELAY = Path(os.environ.get("ZERUS_RELAY_BINARY") or REPO / "services/mobile/relay/target/debug/zerus-relay")
DEVICE = "Pixel 9 @Küche"  # Inner @ and non-ASCII reach hgs as one argument.
SESSION = fixture_hgs.initial()["name"]

# The connector's hgs: the real binary in the environment of native.json; records each call.
WRAPPER = '''#!{python}
import json, os, sys, tempfile
root = os.path.dirname(os.path.abspath(__file__))
with open(os.path.join(root, "native.json")) as file:
    native = json.load(file)
data = sys.stdin.buffer.read()
with open(os.path.join(root, "calls.jsonl"), "a") as log:
    log.write(json.dumps({{"argv": sys.argv[1:], "stdin": data.decode()}}) + "\\n")
stdin = tempfile.TemporaryFile(dir=root)
stdin.write(data)
stdin.seek(0)
os.dup2(stdin.fileno(), 0)
os.execve(native["hgs"], [native["hgs"], *sys.argv[1:]], native["env"])
'''
# Every tool hgs could start: tmux answers its version only, nothing else succeeds.
SHIM = '''#!/bin/sh
echo "$(basename "$0") $*" >> "{log}"
if [ "$(basename "$0")" = tmux ] && [ "$1" = -V ]; then echo "tmux 3.7"; exit 0; fi
exit 1
'''


def isolated_hgs(root):
    """The real hgs behind a recording wrapper, in an environment built from nothing."""
    home = root / "home"
    shims = home / ".local/bin"
    shims.mkdir(parents=True)
    for tool in ("tmux", "ssh", "scp", "codex", "claude", "kimi", "dsh"):
        (shims / tool).write_text(SHIM.format(log=root / "tools.log"))
        (shims / tool).chmod(0o755)
    for name in ("config", "state", "tmux", "run"):
        (root / name).mkdir(mode=0o700)
    env = {"HOME": str(home), "PATH": f"{shims}:/usr/bin:/bin", "LANG": "C.UTF-8",
           "XDG_CONFIG_HOME": str(home / ".config"), "XDG_STATE_HOME": str(home / ".local/state"),
           "XDG_DATA_HOME": str(home / ".local/share"), "XDG_CACHE_HOME": str(home / ".cache"),
           "XDG_RUNTIME_DIR": str(root / "run"), "TMUX_TMPDIR": str(root / "tmux"),
           "HGS_CONFIG_DIR": str(root / "config"), "HGS_STATE_DIR": str(root / "state"), "HGS_TAB": "0"}
    (root / "native.json").write_text(json.dumps({"hgs": str(HGS), "env": env}))
    wrapper = root / "hgs"
    wrapper.write_text(WRAPPER.format(python=sys.executable))
    wrapper.chmod(0o700)
    return wrapper


class Phone:
    """The phone's side of the relay API, with bounded polling."""

    def __init__(self, test, client, url, token, computer_id, connector):
        self.test, self.client, self.url, self.computer_id, self.connector = test, client, url, computer_id, connector
        self.headers = {"Authorization": "Bearer " + token}

    async def call(self, method, path, body=None, status=None):
        async with self.client.request(method, self.url + path, headers=self.headers, json=body) as response:
            data = await response.json()
            if status is None:
                self.test.assertLess(response.status, 300, data)
            else:
                self.test.assertEqual(response.status, status, data)
            return data

    async def until(self, check, seconds=30):
        deadline = asyncio.get_running_loop().time() + seconds
        while True:
            if self.connector.done():
                self.connector.result()
            value = check()
            if asyncio.iscoroutine(value):
                value = await value
            if value:
                return value
            if asyncio.get_running_loop().time() > deadline:
                self.test.fail("the phone did not see the expected state")
            await asyncio.sleep(0.05)

    async def computer(self):
        computers = (await self.call("GET", "/v1/computers"))["computers"]
        return computers[0] if computers else None

    async def serving(self):
        """The gateway once it is online, advertises Overmind and carries a digest."""
        async def ready():
            row = await self.computer()
            snapshot = (row or {}).get("snapshot") or {}
            operations = set(snapshot.get("mobile_capabilities", {}).get("operations", []))
            return row if row and row["online"] and {"overmind_read", "overmind_write"} <= operations and "mobile_overmind" in snapshot else None
        return await self.until(ready)

    async def digest(self, check):
        async def seen():
            value = (await self.computer())["snapshot"].get("mobile_overmind")
            return value if value is not None and check(value) else None
        return await self.until(seen)

    async def submit(self, body):
        receipt = await self.call("POST", "/v1/requests", body)

        async def settled():
            nonlocal receipt
            if receipt["state"] in {"queued", "claimed"}:
                receipt = await self.call("GET", "/v1/requests/" + body["request_id"])
            return receipt["state"] not in {"queued", "claimed"}
        await self.until(settled)
        return receipt

    async def overmind(self, operation, action, request, *, request_id=None, device=DEVICE):
        """One Overmind request as the phone sends it; returns its body and completed receipt."""
        request_id = request_id or str(uuid.uuid4())
        payload = {"action": action, "request": request, **({"device": device} if device else {})}
        if operation == "overmind_write":
            payload["request_id"] = request_id
        body = {"request_id": request_id, "computer_id": self.computer_id, "operation": operation, "session": "", "payload": payload}
        receipt = await self.submit(body)
        self.test.assertEqual(receipt["state"], "completed", receipt)
        return body, receipt

    async def read(self, action, request):
        result = (await self.overmind("overmind_read", action, request))[1]["result"]
        self.test.assertTrue(result["ok"], result)
        return result

    async def write(self, action, request, **options):
        result = (await self.overmind("overmind_write", action, request, **options))[1]["result"]
        self.test.assertTrue(result["ok"], result)
        return result

    async def events(self, kind):
        return [event for event in (await self.call("GET", "/v1/events?after=0"))["events"] if event["kind"] == kind]


class OvermindEndToEnd:
    """Both cases, on the relay the subclass names (None: the reference relay)."""
    relay = None

    @contextlib.asynccontextmanager
    async def gateway(self, root, native, environment=None):
        store = Store(root / "relay" / "state.sqlite3")
        workspace = store.workspace("Integration")
        node = store.node(workspace, "Gateway computer")
        invitation = store.invite(workspace)
        with patch.dict(os.environ, {"ZERUS_RELAY_BINARY": str(self.relay or "")}):
            runner, url = await start_relay(store, Config(background=False))
        connector = Connector({"server_url": url, "node_token": node["node_token"], "hgs_path": str(native),
                               "state_dir": str(root / "journal"), "poll_interval": 1}, allow_insecure_localhost=True)
        # The digest is refreshed on every heartbeat instead of every fifteen seconds.
        with patch.dict(os.environ, environment or {}), patch.object(overmind, "DIGEST_SECONDS", 0):
            task = asyncio.create_task(connector.run())
            try:
                async with ClientSession() as client:
                    async with client.post(url + "/v1/pair", json={"code": invitation["pair_code"], "device_name": "Test phone"}) as response:
                        self.assertEqual(response.status, 200)
                        token = (await response.json())["device_token"]
                    self.store = store
                    yield Phone(self, client, url, token, node["node_id"], task)
            finally:
                task.cancel()
                await asyncio.gather(task, return_exceptions=True)
                connector.journal.close()
                # The reference relay ends the connector's abandoned long poll once its credential is revoked.
                store.revoke("nodes", node["node_id"])
                await runner.cleanup()
                store.close()

    @unittest.skipUnless(HGS.is_file(), "the real hgs is not built (cargo build --locked)")
    async def test_the_phone_creates_reads_and_changes_tasks_through_the_real_hgs(self):
        with tempfile.TemporaryDirectory(prefix="zerus-overmind-e2e-") as directory:
            root = Path(directory)
            native = isolated_hgs(root)
            identity = subprocess.run([str(native), "swarm", "__mobile-peer-identity", "--json"], input="",
                                      capture_output=True, text=True, timeout=30)
            self.assertEqual(identity.returncode, 0, identity.stderr)
            machine = json.loads(identity.stdout)["machine_id"]
            async with self.gateway(root, native) as phone:
                await phone.serving()

                # Requests reach hgs through its mobile allowlist once the gateway has named its own machine.
                async def routed():
                    return (await phone.computer()).get("machine_id") == machine
                await phone.until(routed)
                self.assertEqual((await phone.computer())["snapshot"]["mobile_overmind"],
                                 {"schema": 1, "rev": 0, "projects": {}, "decisions": []})

                # 900 KiB: the write crosses the 1 MiB envelope that only writes may exceed; two ranges read it back.
                image = bytes(range(256)) * 3600
                sha = hashlib.sha256(image).hexdigest()
                create = {"project": "demo", "prompt": "Review the relay route\nfrom the phone", "stage": "Queue",
                          "attachments": [{"name": "route.png", "mime": "image/png", "data_base64": base64.b64encode(image).decode()}]}
                body, created = await phone.overmind("overmind_write", "create", create)
                card = created["result"]["task"]
                task = body["request_id"]
                self.assertTrue(created["result"]["ok"], created)
                self.assertEqual((card["uuid"], card["project"], card["title"], card["stage"]),
                                 (task, "demo", "Review the relay route", "Queue"))
                self.assertTrue(card["ref"].startswith("om-"))
                self.assertFalse({"prompt", "journal", "attachments"} & set(card), "a write answers with a compact card")
                # A retry after a lost connection replays the receipt; other content under its identity is refused.
                self.assertEqual(await phone.submit(body), created)
                await phone.call("POST", "/v1/requests", {**body, "payload": {**body["payload"], "request": {**create, "stage": "Backlog"}}}, status=409)

                board = await phone.read("board", {"project": "demo"})
                self.assertEqual([(row["uuid"], row["stage"]) for row in board["tasks"]], [(task, "Queue")])
                self.assertIsNone(board["next"])
                self.assertIn("Your Call", [stage["name"] for stage in board["workflow"]["stages"]])
                shown = (await phone.read("task", {"task": card["ref"]}))["task"]
                self.assertEqual((shown["uuid"], shown["prompt"]), (task, create["prompt"]))
                self.assertEqual((shown["journal_total"], shown["journal_from"], [entry["marker"] for entry in shown["journal"]]),
                                 (1, 0, ["[created]"]))
                self.assertEqual(shown["created_by"]["device"], DEVICE)
                self.assertEqual([(row["sha256"], row["bytes"]) for row in shown["attachments"]], [(sha, len(image))])
                received = b""
                while len(received) < len(image):
                    chunk = await phone.read("attachment", {"task": task, "sha256": sha, "offset": len(received),
                                                            "length": min(512 * 1024, len(image) - len(received))})
                    self.assertEqual((chunk["offset"], chunk["size"], chunk["name"], chunk["mime"]),
                                     (len(received), len(image), "route.png", "image/png"))
                    received += base64.b64decode(chunk["data_base64"])
                self.assertEqual(received, image)

                edited = await phone.write("edit", {"task": task, "title": "Check the relay route", "priority": 1})
                self.assertEqual((edited["task"]["title"], edited["task"]["priority"]), ("Check the relay route", 1))
                moved = await phone.write("move", {"task": task, "to": "Backlog"})
                self.assertEqual(moved["task"]["stage"], "Backlog")
                await phone.write("comment", {"task": task, "text": "Checked from the phone"})
                page = (await phone.read("task", {"task": task, "journal_limit": 1}))["task"]
                self.assertEqual([(entry["marker"], entry["text"], entry["actor"]["device"]) for entry in page["journal"]],
                                 [("[comment]", "Checked from the phone", DEVICE)])
                self.assertEqual(page["journal_from"], page["journal_total"] - 1)
                # The phone refreshes a board with the cursor of its last read: only what changed since.
                changed = await phone.read("board", {"project": "demo", "since": board["cursor"]})
                self.assertEqual([(row["uuid"], row["title"], row["stage"]) for row in changed["tasks"]],
                                 [(task, "Check the relay route", "Backlog")])
                self.assertEqual((await phone.read("board", {"project": "demo", "since": changed["cursor"]}))["tasks"], [])
                # A refusal is the answer the phone shows: a completed receipt with ok false.
                refused = (await phone.overmind("overmind_write", "move", {"task": task, "to": "Build"}))[1]["result"]
                self.assertEqual((refused["ok"], refused["error"]["code"]), (False, "use_send"))
                decisions = await phone.read("decisions", {})
                self.assertEqual(decisions["decisions"], [])
                projects = (await phone.read("projects", {}))["projects"]
                self.assertEqual([(row["project"], row["total"], row["roles"]["backlog"]) for row in projects], [("demo", 1, 1)])
                digest = await phone.digest(lambda value: value["projects"].get("demo", {}).get("backlog") == 1)
                self.assertEqual((digest["rev"], digest["decisions"]), (decisions["cursor"], []))

            calls = [json.loads(line) for line in (root / "calls.jsonl").read_text().splitlines()]
            # Every phone request reached hgs as `swarm __mobile-peer-local` for this machine, in an exact shape.
            local = [json.loads(call["stdin"]) for call in calls if call["argv"] == ["swarm", "__mobile-peer-local", "--json"]]
            self.assertEqual({request["target_machine_id"] for request in local}, {machine})
            self.assertEqual({tuple(request["argv"][2:]) for request in local}, {("--json", "--device", DEVICE)})
            self.assertEqual(sorted(request["argv"][1] for request in local),
                             sorted(["create", "board", "show", "attachment", "attachment", "edit", "move", "comment", "show", "board", "board", "move",
                                     "decisions", "projects"]))
            self.assertEqual([request["payload"]["request_id"] for request in local if request["argv"][1] == "create"], [task])
            # The digest is the connector's own read, never a phone request.
            self.assertTrue({tuple(call["argv"]) for call in calls if call["argv"][:1] == ["overmind"]}
                            <= {("overmind", "projects", "--json"), ("overmind", "decisions", "--json")})
            tools = (root / "tools.log").read_text().splitlines() if (root / "tools.log").exists() else []
            # Its tmux calls reached only the shim, and nothing tried to leave the machine.
            self.assertTrue([line for line in tools if line.startswith("tmux ")], tools)
            self.assertFalse([line for line in tools if line.split()[0] in {"ssh", "scp"}], tools)

    async def test_a_task_sent_to_a_session_raises_and_resolves_its_decision_on_the_phone(self):
        with tempfile.TemporaryDirectory(prefix="zerus-overmind-e2e-") as directory:
            root = Path(directory)
            native = Path(__file__).with_name("fixture_hgs.py")
            native.chmod(0o755)

            def configure(**fields):
                fixture_hgs.persist(root / "fixture-config.json", {"overmind": True, **fields})

            def mutations():
                path = root / "mutations.jsonl"
                return [json.loads(line) for line in path.read_text().splitlines()] if path.exists() else []

            def held():
                return self.store.db.execute("SELECT count(*) FROM alert_candidates WHERE kind='decision'").fetchone()[0]

            async def typed(text):
                """Whether the fixture session received this text, as the phone inspects it."""
                inspect = {"request_id": str(uuid.uuid4()), "computer_id": phone.computer_id, "operation": "inspect",
                           "session": SESSION, "payload": {}}
                return any(event.get("detail") == text for event in (await phone.submit(inspect))["result"]["events"])

            async def counted(kind, number):
                return len(await phone.events(kind)) == number

            configure()
            async with self.gateway(root, native, {"ZERUS_MOBILE_FIXTURE_DIR": directory}) as phone:
                await phone.serving()
                await phone.digest(lambda value: value["decisions"] == [])
                body, created = await phone.overmind("overmind_write", "create", {"project": "demo", "prompt": "Pick the storage engine"})
                task, ref = body["request_id"], created["result"]["task"]["ref"]
                self.assertEqual(created["result"]["task"]["stage"], "Backlog")

                send = {"task": task, "session": {"name": SESSION}}
                body, sent = await phone.overmind("overmind_write", "send", send)
                self.assertEqual((sent["result"]["delivery"], sent["result"]["task"]["stage"], sent["result"]["task"]["assignee"]["name"]),
                                 ("sent", "Design", SESSION))
                self.assertEqual(await phone.submit(body), sent)
                self.assertTrue(await typed(f"[Overmind {ref}] Pick the storage engine"))
                self.assertEqual([(row["operation"], row["device"]) for row in mutations()], [("overmind-send", DEVICE)])

                # The agent asks a person: the digest lists the task and the phone gets a decision.
                fixture_hgs.overmind_call_human(root, task, "SQLite or PostgreSQL?")
                slot = "overmind:" + task
                digest = await phone.digest(lambda value: value["decisions"])
                self.assertEqual(digest["decisions"], [{"task": task, "project": "demo", "title": "Pick the storage engine",
                                                        "signature": digest["decisions"][0]["signature"]}])
                raised = await phone.until(lambda: phone.events("decision"))
                self.assertEqual([(event["session"], event["computer_id"], "alert_kind" in event) for event in raised],
                                 [(slot, phone.computer_id, False)])
                if self.relay:
                    # No presence is known, so the Rust relay alerts at once.
                    alerts = await phone.until(lambda: phone.events("alert"))
                    self.assertEqual([(event["alert_kind"], event["session"], event["computer_id"]) for event in alerts],
                                     [("decision", slot, phone.computer_id)])
                question = (await phone.read("decisions", {}))["decisions"]
                self.assertEqual([(row["task"], row["question"]) for row in question], [(task, "SQLite or PostgreSQL?")])

                # Answering leaves Your Call: the digest resolves and nothing is raised again.
                answered = await phone.write("answer", {"task": task, "answer": "SQLite"})
                self.assertEqual((answered["delivery"], answered["task"]["stage"]), ("sent", "Design"))
                self.assertTrue(await typed("SQLite"))
                await phone.digest(lambda value: value["decisions"] == [])

                # At the desk the next question is held, and it resolves without an alert.
                configure(desktop_presence={"idle_seconds": 1.0, "locked": False, "locked_seconds": None})
                await asyncio.sleep(2.5)
                fixture_hgs.overmind_call_human(root, task, "Which index?")
                await phone.until(lambda: counted("decision", 2))
                if self.relay:
                    await phone.until(lambda: held() == 1)
                self.assertTrue(await counted("alert", 1 if self.relay else 0))
                await phone.write("answer", {"task": task, "answer": "By name"})
                await phone.digest(lambda value: value["decisions"] == [])
                if self.relay:
                    await phone.until(lambda: held() == 0)
                # Away from the desk now: a resolved decision is never delivered late.
                configure(desktop_presence={"idle_seconds": 600.0, "locked": False, "locked_seconds": None})
                await asyncio.sleep(2.5)
                self.assertTrue(await counted("decision", 2))
                self.assertTrue(await counted("alert", 1 if self.relay else 0))
                self.assertEqual([row["operation"] for row in mutations()], ["overmind-send", "overmind-answer", "overmind-answer"])


@unittest.skipUnless(RUST_RELAY.is_file(), "the Rust relay is not built")
class RustRelayOvermindEndToEndTests(OvermindEndToEnd, unittest.IsolatedAsyncioTestCase):
    relay = RUST_RELAY


class ReferenceRelayOvermindEndToEndTests(OvermindEndToEnd, unittest.IsolatedAsyncioTestCase):
    relay = None


if __name__ == "__main__":
    unittest.main()
