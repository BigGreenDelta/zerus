#!/usr/bin/env python3
"""Exercise an installed system bundle with a disposable home and tmux socket."""
import json
import os
from pathlib import Path
import subprocess
import tempfile


# A user manager for the --mobile-connector actions: it reports a Python
# connector unit, applies the drop-ins only on daemon-reload and logs every call.
SYSTEMCTL = """#!/usr/bin/python
import json, os, sys
from pathlib import Path
home = Path(os.environ["HOME"])
with open(home / "systemctl.calls", "a") as log:
    log.write(" ".join(sys.argv[1:]) + "\\n")
drop_ins = home / ".config/systemd/user/zerus-mobile-connector.service.d"
loaded = home / "loaded.json"
if sys.argv[2] == "daemon-reload":
    rust = drop_ins / "60-zerus-rust-connector.conf"
    program = "/usr/bin/zerus-mobile-connector" if rust.exists() else os.environ["FIXTURE_VENV"] + "/bin/zerus-mobile-connector"
    command = program + " --config " + os.environ["FIXTURE_CONFIG"]
    kill = "process" if rust.exists() or (drop_ins / "50-zerus-killmode.conf").exists() else "control-group"
    loaded.write_text(json.dumps({"LoadState": "loaded", "KillMode": kill,
                                  "ExecStart": "{ path=%s ; argv[]=%s ; ignore_errors=no }" % (program, command)}))
elif sys.argv[2] == "show":
    for key, value in json.loads(loaded.read_text()).items():
        print(key + "=" + value)
"""


def connector_actions(home, env):
    """Each --mobile-connector action writes or removes only its drop-in and
    never enables, starts, stops or restarts a unit."""
    fake = home / "fake-bin"
    fake.mkdir()
    (fake / "systemctl").write_text(SYSTEMCTL)
    (fake / "systemctl").chmod(0o755)
    venv = home / "venv"
    (venv / "bin").mkdir(parents=True)
    (venv / "pyvenv.cfg").write_text("home = /usr/bin\n")
    (venv / "bin/zerus-mobile-connector").write_text("#!/bin/sh\n")
    (venv / "bin/zerus-mobile-connector").chmod(0o755)
    config = home / ".config/hgs/mobile/connector.json"
    config.parent.mkdir(parents=True)
    config.write_text(json.dumps({"state_dir": str(home / "connector-state")}))
    actions = dict(env, PATH=f"{fake}:{env['PATH']}", FIXTURE_VENV=str(venv), FIXTURE_CONFIG=str(config))
    subprocess.run([str(fake / "systemctl"), "--user", "daemon-reload"], env=actions, check=True)
    (home / "systemctl.calls").unlink()
    drop_ins = home / ".config/systemd/user/zerus-mobile-connector.service.d"
    for action, present in (("protect", ["50-zerus-killmode.conf"]),
                            ("migrate", ["50-zerus-killmode.conf", "60-zerus-rust-connector.conf"]),
                            ("rollback", ["50-zerus-killmode.conf"])):
        subprocess.run(["/usr/bin/zerus-setup", "--mobile-connector", action], env=actions, check=True,
                       stdin=subprocess.DEVNULL, timeout=30)
        assert sorted(path.name for path in drop_ins.iterdir()) == present, action
        assert sorted(path.name for path in (home / ".config/systemd/user").iterdir()) == [drop_ins.name], action
        calls = (home / "systemctl.calls").read_text().splitlines()
        assert calls and all(call == "--user daemon-reload" or call.startswith("--user show ") for call in calls), calls
        (home / "systemctl.calls").unlink()


def main():
    if os.geteuid() == 0:
        raise SystemExit("Run the installed-package check as an unprivileged user.")
    with tempfile.TemporaryDirectory(prefix="zerus-installed-test-") as temporary:
        home = Path(temporary)
        sockets, project = home / "tmux", home / "project"
        sockets.mkdir(mode=0o700)
        project.mkdir()
        env = {key: value for key, value in os.environ.items()
               if not key.startswith("HGS_") and key not in {"TMUX", "TMUX_PANE", "CODEX_HOME", "CLAUDE_CONFIG_DIR", "KIMI_CODE_HOME"}}
        env.update(HOME=str(home), TMUX_TMPDIR=str(sockets), HGS_STATE_DIR=str(home / "state"),
                   HGS_CONFIG_DIR=str(home / "config"), HGS_SELF="fixture", HGS_PEERS="", HGS_TAB="0")

        def run(*args):
            return subprocess.check_output(args, env=env, text=True, timeout=20)

        run("/usr/bin/zerus-setup")
        links = [home / ".local/bin" / name for name in ("hgs", "hgs-tray", "hgs_state.py")]
        for link in links:
            assert link.is_symlink() and link.resolve() == Path("/usr/bin") / link.name
        times = [link.lstat().st_mtime_ns for link in links]
        run("/usr/bin/zerus-setup")
        assert times == [link.lstat().st_mtime_ns for link in links]
        assert not (home / ".config/systemd").exists(), "setup enabled user services"
        print(run(str(links[0]), "--version").strip())
        print(run(str(links[1]), "--version").strip())
        # The mobile connector is installed but neither linked nor enabled.
        assert not (home / ".local/bin/zerus-mobile-connector").exists()
        print(run("/usr/bin/zerus-mobile-connector", "--version").strip())
        connector_actions(home, env)
        # No native agent is started. The terminal-only fixture uses an ordinary
        # non-Git project and a socket that cannot reach an existing server.
        try:
            run(str(links[0]), "sh", str(project), "-n", "package-fixture", "-d")
            sessions = json.loads(run(str(links[0]), "ls", "--local", "--json"))["sessions"]
            assert len(sessions) == 1 and sessions[0]["name"] == "sh/project/package-fixture"
            assert Path(sessions[0]["cwd"]).resolve() == project
            assert not (project / ".git").exists()
        finally:
            socket = sockets / f"tmux-{os.getuid()}" / "default"
            if socket.exists():
                run("tmux", "-S", str(socket), "kill-server")
    print("Installed system binaries, idempotent user setup and isolated terminal session verified.")


if __name__ == "__main__":
    main()
