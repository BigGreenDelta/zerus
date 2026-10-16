#!/usr/bin/env python3
"""Exercise an installed system bundle with a disposable home and tmux socket."""
import json
import os
from pathlib import Path
import subprocess
import tempfile


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
