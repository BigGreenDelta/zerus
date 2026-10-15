"""Black-box one-hop transport with the real native CLI.

The gateway's hgs is a built binary; its direct peer is a second hgs
configuration behind a synthetic `ssh` that runs that hgs locally. The
connector discovers the peer, reaches it through `hgs swarm mobile-peer`, and
when its own deadline ends a peer command it kills only its hgs child, whose
supervisor must reap the owned SSH transport: the black-box version of the
connector case in tests/test_mobile_peers.py. Agent CLIs and tmux are
stand-ins that exit at once, so no agent, tmux server or network is reached.

The binary is HGS_TEST_BIN, ZERUS_DIRECTORY_TEST_HGS (set by CI) or
target/debug/hgs; without one the tests skip, except in CI, where they fail.
"""
import json
import os
from pathlib import Path
import subprocess
import sys
import unittest

from connector_harness import ROOT
from gateway_harness import GatewayCase, error, route_id

TIMED_OUT = {error("native command timed out"), error("native peer command timed out")}
STAND_INS = ("tmux", "codex", "claude", "kimi", "opencode", "dsh")
SSH = '''#!{python} -I
"""Synthetic SSH: run the alias's hgs here with its own configuration."""
import json, os, shlex, sys, tempfile, time
root, hgs = {root!r}, {hgs!r}
alias, args = sys.argv[-2], shlex.split(sys.argv[-1])[1:]
env =dict(os.environ, HGS_CONFIG_DIR=os.path.join(root, alias), HGS_STATE_DIR=os.path.join(root, alias, "state"))
if args == ["swarm", "__mobile-peer-local", "--json"]:
    data = sys.stdin.buffer.read()
    if json.loads(data)["argv"][0] == "dirs" and os.path.exists(os.path.join(root, "stall-dirs")):
        # Publish whole: the test reads this as soon as it exists.
        receipt = os.path.join(root, "stalled-%d.json" % os.getpid())
        with open(receipt + ".tmp", "w") as staged:
            json.dump({{"pids": [os.getpid(), os.getppid()]}}, staged)
        os.replace(receipt + ".tmp", receipt)
        time.sleep(30)
        sys.exit(0)
    stdin = tempfile.TemporaryFile()
    stdin.write(data)
    stdin.seek(0)
    os.dup2(stdin.fileno(), 0)
os.execve(hgs, [hgs, *args], env)
'''


def native_hgs() -> Path | None:
    for candidate in (os.environ.get("HGS_TEST_BIN"), os.environ.get("ZERUS_DIRECTORY_TEST_HGS"),
                      str(ROOT / "target/debug/hgs")):
        if candidate and os.path.isfile(candidate) and os.access(candidate, os.X_OK):
            return Path(candidate).resolve()
    return None


def running(pid: int) -> bool:
    """Alive and not a zombie (one that its parent has yet to reap is gone too)."""
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    stat = Path(f"/proc/{pid}/stat")
    if stat.exists():
        try:
            return stat.read_text().rsplit(") ", 1)[1][0] not in "ZX"
        except (FileNotFoundError, ProcessLookupError, IndexError):
            return False
    state = subprocess.run(["ps", "-p", str(pid), "-o", "stat="], capture_output=True, text=True)
    return state.returncode == 0 and not state.stdout.strip().startswith(("Z", "X"))


class PeerTransportTests(GatewayCase):
    async def asyncSetUp(self):
        await super().asyncSetUp()
        self.native = native_hgs()
        if self.native is None:
            await self.relay.stop()  # A failed or skipped setup gets no asyncTearDown.
            if os.environ.get("CI") == "true":
                self.fail("a built hgs is required in CI (HGS_TEST_BIN or ZERUS_DIRECTORY_TEST_HGS)")
            self.skipTest("needs a built hgs (cargo build --bin hgs)")
        self.root = self.scratch / "fleet"
        bin_dir = self.root / "bin"
        bin_dir.mkdir(parents=True)
        (bin_dir / "ssh").write_text(SSH.format(python=sys.executable, root=str(self.root), hgs=str(self.native)))
        for name in STAND_INS:
            (bin_dir / name).write_text("#!/bin/sh\nexit 1\n")
        for path in bin_dir.iterdir():
            path.chmod(0o700)
        for node, peers in (("a", "b"), ("b", "")):
            (self.root / node).mkdir()
            (self.root / node / "config").write_text(f'HGS_SELF="{node}"\nHGS_PEERS="{peers}"\n')
        (self.root / "tmux").mkdir()
        self.native_env = {"PATH": os.pathsep.join([str(bin_dir), "/usr/bin", "/bin"]), "HGS_TAB": "0",
                           "TMUX_TMPDIR": str(self.root / "tmux"), "HGS_CONFIG_DIR": str(self.root / "a"),
                           "HGS_STATE_DIR": str(self.root / "a/state")}
        self.machines = {node: self.identity(node) for node in ("a", "b")}

    def identity(self, node: str) -> str:
        env = self.environment({**self.native_env, "HGS_CONFIG_DIR": str(self.root / node),
                                "HGS_STATE_DIR": str(self.root / node / "state")})
        result = subprocess.run([str(self.native), "swarm", "__mobile-peer-identity", "--json"], env=env,
                                capture_output=True, text=True, timeout=30, check=True)
        return json.loads(result.stdout)["machine_id"]

    def stalled(self) -> list[int]:
        return [pid for path in self.root.glob("stalled-*.json") for pid in json.loads(path.read_text())["pids"]]

    async def test_peer_commands_reach_the_peer_and_a_deadline_reaps_the_owned_transport(self):
        route = route_id("b", self.machines["b"])
        process = await self.launch(self.config(hgs_path=str(self.native)), env=self.native_env)
        await self.until(lambda: self.latest().get("machine_id") == self.machines["a"] and self.latest().get("peers") == [
            {"route_id": route, "machine_id": self.machines["b"], "name": "b", "online": True}], 30,
            "the real peer in the manifest", process)
        [snapshot, *_] = await self.until(lambda: self.relay.peer_heartbeats(route), 30, "a real peer heartbeat", process)
        self.assertEqual(snapshot.body["machine_id"], self.machines["b"])
        self.assertIn("dirs", snapshot.body["snapshot"]["mobile_capabilities"]["operations"],
                      "the peer's capability probe did not finish within its 3 s budget")
        listed = self.root / "peer-files"
        (listed / "visible").mkdir(parents=True)
        peer_route = {"schema": 1, "route_id": route, "computer_id": self.relay.computer_id,
                      "machine_id": self.machines["b"]}
        result = await self.submit(self.scoped("dirs", path=str(listed)), process, timeout=30, gateway_route=peer_route)
        self.assertEqual(result["state"], "completed", result)
        self.assertEqual((result["result"]["path"], [row["name"] for row in result["result"]["directories"]]),
                         (str(listed), ["visible"]))
        # The peer stalls far beyond the connector's own 5 s directory deadline.
        (self.root / "stall-dirs").touch()
        result = await self.submit(self.scoped("dirs", path=str(listed)), process, timeout=25, gateway_route=peer_route)
        self.assertEqual((result["state"], result["result"]), ("failed", None))
        self.assertIn(result["error"], TIMED_OUT)
        owned = self.stalled()
        self.assertEqual(len(owned), 2, "the stalled transport left no receipt")
        await self.until(lambda: not any(running(pid) for pid in owned), 10,
                         "the owned SSH and its supervisor to exit", process)
        self.assertIsNone(process.returncode)


if __name__ == "__main__":
    unittest.main()
