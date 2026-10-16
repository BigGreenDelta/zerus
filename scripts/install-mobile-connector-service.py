#!/usr/bin/env python3
"""Install, protect or switch the per-user Zerus mobile connector service.

  install   write the user unit (Linux) or the LaunchAgent (macOS) for the Rust
            connector, only when none exists or the existing one is ours
  protect   give the unit KillMode=process when it does not have it yet
  migrate   point a Python connector unit at the Rust connector
  rollback  remove the migration drop-in again

Each action writes or removes one file of its own and, on Linux, reloads the
user manager, which restarts nothing. It never edits, moves or deletes another
file, and never enables, starts, stops, loads or restarts a service: the
printed commands and the cutover runbook (docs/mobile-deployment.md) do that by
hand. protect, migrate and rollback are Linux only.
"""
import argparse
import fcntl
import json
import os
from pathlib import Path
import plistlib
import shutil
import subprocess
import sys

UNIT = "zerus-mobile-connector.service"
PROTECT = "50-zerus-killmode.conf"
MIGRATE = "60-zerus-rust-connector.conf"
MARKER = "# Written by zerus-setup --mobile-connector migrate; rollback removes it."
PROTECTION = "# Written by zerus-setup --mobile-connector protect.\n[Service]\nKillMode=process\n"
BINARIES = (Path("/usr/bin/zerus-mobile-connector"), Path.home() / ".local/bin/zerus-mobile-connector")
# Paths written into a unit verbatim: nothing systemd would split or expand.
UNSAFE = set(" \t\n\"'\\%$;")


INSTALLED = "# Written by zerus-setup --mobile-connector install; it rewrites only this file."
LABEL = "com.hgdev.zerus-mobile-connector"


class Refusal(Exception):
    pass


def systemctl(*args):
    return subprocess.run(["systemctl", "--user", *args], stdin=subprocess.DEVNULL, capture_output=True,
                          text=True, timeout=10)


def reload():
    result = systemctl("daemon-reload")
    if result.returncode:
        raise Refusal("systemctl --user daemon-reload failed: " + result.stderr.strip())


def show():
    """The effective unit as the user manager reports it."""
    result = systemctl("show", UNIT, "-p", "LoadState,KillMode,ExecStart,FragmentPath,DropInPaths", "--no-pager")
    if result.returncode:
        raise Refusal("the user service manager is unavailable: " + result.stderr.strip())
    values = {}
    for line in result.stdout.splitlines():
        key, _, value = line.partition("=")
        values[key] = value
    if values.get("LoadState") != "loaded":
        raise Refusal(f"{UNIT} is not installed for this user; there is nothing to change")
    return values


def command(values):
    """The single ExecStart command line, or None when it has another shape."""
    value = values.get("ExecStart", "")
    if value.count("argv[]=") != 1:
        return None
    argv = value.split("argv[]=", 1)[1].split(" ;", 1)[0]
    if set(argv) & (UNSAFE - {" "}):
        return None
    return argv.split()


def connector(argv):
    return bool(argv) and Path(argv[0]).name == "zerus-mobile-connector"


def drop_ins():
    configuration = Path(os.environ.get("XDG_CONFIG_HOME") or Path.home() / ".config")
    return configuration / "systemd/user" / (UNIT + ".d")


def private(path, text):
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name("." + path.name + ".new")
    temporary.write_text(text)
    temporary.chmod(0o644)
    os.replace(temporary, path)


def held(config):
    """Whether a connector holds the journal lock of this configuration."""
    try:
        state = json.loads(Path(config).read_text()).get("state_dir")
    except (OSError, ValueError, AttributeError) as error:
        raise Refusal(f"cannot read the connector configuration {config}: {error}") from None
    if state in (None, ""):
        state = "~/.local/state/hgs/mobile-connector"
    if not isinstance(state, str):
        raise Refusal("the connector configuration has an invalid state_dir")
    lock = Path(state).expanduser() / "connector.lock"
    try:
        descriptor = os.open(lock, os.O_RDONLY | os.O_NOFOLLOW)
    except FileNotFoundError:
        return False
    try:
        fcntl.flock(descriptor, fcntl.LOCK_SH | fcntl.LOCK_NB)
    except OSError:
        return True
    finally:
        os.close(descriptor)
    return False


def protect(args):
    values = show()
    argv = command(values)
    if not connector(argv):
        raise Refusal(f"{UNIT} runs {values.get('ExecStart', 'nothing')!r}, not a zerus-mobile-connector; "
                      "protect it by hand")
    if values.get("KillMode") == "process":
        print(f"{UNIT} already uses KillMode=process; nothing written.")
        return
    path = drop_ins() / PROTECT
    if path.exists():
        raise Refusal(f"{path} exists but the effective KillMode is {values.get('KillMode')}: a later drop-in "
                      f"overrides it ({values.get('DropInPaths', '')}); review them by hand")
    private(path, PROTECTION)
    reload()
    if show().get("KillMode") != "process":
        path.unlink()
        reload()
        raise Refusal("a later drop-in keeps another KillMode; removed the protection again, review DropInPaths")
    print(f"Wrote {path} and reloaded the user manager; nothing was restarted. KillMode=process now applies.")


def chosen_binary(args):
    if args.binary:
        binary = Path(args.binary)
        if not binary.is_absolute() or binary.name != "zerus-mobile-connector":
            raise Refusal("--binary must be an absolute path to zerus-mobile-connector")
    else:
        present = [binary for binary in BINARIES if binary.is_file()]
        if not present:
            raise Refusal("no Rust connector is installed (/usr/bin or ~/.local/bin); install it first")
        if len(present) > 1:
            raise Refusal("both " + " and ".join(map(str, present)) + " exist; choose one with --binary")
        binary = present[0]
    if not (binary.is_file() and os.access(binary, os.X_OK)):
        raise Refusal(f"{binary} is not an executable file")
    if set(str(binary)) & UNSAFE:
        raise Refusal(f"{binary} contains characters this tool does not write into a unit")
    return binary


def migrate(args):
    values = show()
    argv = command(values)
    binary = chosen_binary(args)
    path = drop_ins() / MIGRATE
    if path.exists() and argv and len(argv) == 3 and argv[:2] == [str(binary), "--config"]:
        print(f"{UNIT} already runs {binary}; nothing written.")
        return
    venv = Path(argv[0]).parent.parent if argv else None
    if not (connector(argv) and len(argv) == 3 and argv[1] == "--config" and Path(argv[0]).parent.name == "bin"
            and (venv / "pyvenv.cfg").is_file()):
        raise Refusal(f"{UNIT} runs {values.get('ExecStart', 'nothing')!r}, not the documented "
                      "<venv>/bin/zerus-mobile-connector --config FILE; migrate it by hand")
    config = argv[2]
    if held(config):
        raise Refusal("a connector still holds this configuration's journal: drain phone actions and stop "
                      f"the unit first (systemctl --user stop {UNIT})")
    if path.exists():
        raise Refusal(f"{path} exists but does not take effect; review DropInPaths ({values.get('DropInPaths', '')})")
    private(path, f"{MARKER}\n[Service]\nExecStart=\nExecStart={binary} --config {config}\nKillMode=process\n")
    reload()
    effective = command(show())
    if effective != [str(binary), "--config", config]:
        path.unlink()
        reload()
        raise Refusal("a later-sorting drop-in, such as a `systemctl --user edit` override.conf, keeps another "
                      "ExecStart; removed the migration again, review DropInPaths")
    print(f"Wrote {path} and reloaded the user manager; nothing was started. When ready: "
          f"systemctl --user start {UNIT}")


def rollback(args):
    path = drop_ins() / MIGRATE
    if not path.exists():
        print(f"No {MIGRATE}; nothing to roll back.")
        return
    if path.read_text().splitlines()[:1] != [MARKER]:
        raise Refusal(f"{path} was not written by this tool; kept")
    path.unlink()
    reload()
    print(f"Removed {path} and reloaded the user manager; nothing was restarted. The unit runs "
          f"{(command(show()) or ['?'])[0]} after its next start.")


def captured_path():
    """PATH for the service: hgs and its native tools as this session finds them."""
    home = Path.home()
    parts = [str(home / ".local/bin"), "/opt/homebrew/bin", "/usr/local/bin",
             *os.environ.get("PATH", "/usr/bin:/bin").split(os.pathsep)]
    return os.pathsep.join(part for part in dict.fromkeys(parts) if part)


def install_binary(args, candidates):
    if args.binary:
        binary = Path(args.binary)
        if not binary.is_absolute() or binary.name != "zerus-mobile-connector":
            raise Refusal("--binary must be an absolute path to zerus-mobile-connector")
    else:
        present = [binary for binary in candidates if binary.is_file()]
        if not present:
            raise Refusal("no Rust connector is installed (" + ", ".join(map(str, candidates)) + "); install it first")
        binary = present[0]
    if not (binary.is_file() and os.access(binary, os.X_OK)):
        raise Refusal(f"{binary} is not an executable file")
    if set(str(binary)) & UNSAFE:
        raise Refusal(f"{binary} contains characters this tool does not write into a service definition")
    return binary


def config_file():
    return Path(os.environ.get("XDG_CONFIG_HOME") or Path.home() / ".config") / "hgs/mobile/connector.json"


def install_unit(args):
    candidates = [Path.home() / ".local/bin/zerus-mobile-connector", Path("/usr/bin/zerus-mobile-connector")]
    if not args.binary and all(candidate.is_file() for candidate in candidates):
        raise Refusal("both " + " and ".join(map(str, candidates)) + " exist; choose one with --binary")
    binary = install_binary(args, candidates)
    configuration = Path(os.environ.get("XDG_CONFIG_HOME") or Path.home() / ".config")
    path = configuration / "systemd/user" / UNIT
    packaged = Path("/usr/lib/systemd/user") / UNIT
    if not path.exists() and binary == Path("/usr/bin/zerus-mobile-connector") and packaged.is_file():
        print(f"The package provides {UNIT} for {binary}; nothing written. "
              f"When ready: systemctl --user enable --now {UNIT}")
        return
    if path.exists() and path.read_text().splitlines()[:1] != [INSTALLED]:
        raise Refusal(f"{path} exists and was not written by this tool; kept (migrate switches a Python connector unit)")
    environment = ("PATH=" + captured_path()).replace("\\", "\\\\").replace('"', '\\"').replace("%", "%%")
    content = (f"{INSTALLED}\n[Unit]\nDescription=Zerus mobile computer connector\nAfter=network-online.target\n"
               "# Without a configuration a manual start is skipped instead of failing repeatedly.\n"
               "ConditionPathExists=%E/hgs/mobile/connector.json\n\n"
               f"[Service]\nType=simple\nExecStart={binary} --config %E/hgs/mobile/connector.json\n"
               f'Environment="{environment}"\nRestart=on-failure\nRestartSec=5\nUMask=0077\nNoNewPrivileges=true\n'
               "# Kill only the connector, never the tmux servers and agents it launched.\nKillMode=process\n\n"
               "[Install]\nWantedBy=default.target\n")
    if path.exists() and path.read_text() == content:
        print(f"{path} is current; nothing written.")
        return
    existed = path.exists()
    private(path, content)
    reload()
    if existed:
        print(f"Updated {path} and reloaded the user manager; a running connector keeps its previous definition "
              f"until it restarts after phone actions drain (systemctl --user restart {UNIT}).")
    else:
        print(f"Wrote {path} and reloaded the user manager; nothing was enabled or started. "
              f"When ready: systemctl --user enable --now {UNIT}")


def launchctl(*args):
    return subprocess.run(["launchctl", *args], stdin=subprocess.DEVNULL, capture_output=True, text=True, timeout=10)


def install_agent(args, bundled):
    binary = install_binary(args, [*bundled, Path.home() / ".local/bin/zerus-mobile-connector"])
    path = Path.home() / "Library/LaunchAgents" / (LABEL + ".plist")
    logs = Path.home() / "Library/Logs/hgs"
    content = plistlib.dumps({
        "Label": LABEL,
        "ProgramArguments": [str(binary), "--config", str(config_file())],
        "RunAtLoad": True,
        # Restart after a failure, not after a stop that exited cleanly.
        "KeepAlive": {"SuccessfulExit": False},
        # Stopping the agent must leave the tmux servers and agents it launched.
        "AbandonProcessGroup": True,
        "ProcessType": "Background",
        "EnvironmentVariables": {"PATH": captured_path()},
        "StandardOutPath": str(logs / "mobile-connector.log"),
        "StandardErrorPath": str(logs / "mobile-connector.log"),
    })
    if path.exists():
        try:
            existing = plistlib.loads(path.read_bytes())
            program = existing.get("ProgramArguments") or [""]
            ours = existing.get("Label") == LABEL and Path(str(program[0])).name == "zerus-mobile-connector"
        except (plistlib.InvalidFileException, ValueError, AttributeError, TypeError):
            ours = False
        if not ours:
            raise Refusal(f"{path} exists and is not a zerus-mobile-connector agent; kept")
        if path.read_bytes() == content:
            print(f"{path} is current; nothing written.")
            return
    logs.mkdir(parents=True, exist_ok=True)
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name("." + path.name + ".new")
    temporary.write_bytes(content)
    temporary.chmod(0o644)
    os.replace(temporary, path)
    domain = f"gui/{os.getuid()}"
    if launchctl("print", f"{domain}/{LABEL}").returncode == 0:
        print(f"Wrote {path}. The loaded agent keeps its previous definition: after phone actions drain, run "
              f"launchctl bootout {domain}/{LABEL} && launchctl bootstrap {domain} {path}")
    else:
        print(f"Wrote {path}; nothing was loaded. When ready: launchctl bootstrap {domain} {path}")


def install(args, platform):
    if platform == "darwin":
        # Zerus.app/Contents/Resources/setup/<this script> ships beside Contents/MacOS.
        here = Path(__file__).resolve()
        install_agent(args, [here.parents[2] / "MacOS/zerus-mobile-connector"] if len(here.parents) > 2 else [])
    else:
        install_unit(args)


def main(argv=None, platform=sys.platform):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("action", choices=("install", "protect", "migrate", "rollback"))
    parser.add_argument("--binary", help="the Rust connector for install, or for migrate when both installed copies exist")
    args = parser.parse_args(argv)
    if args.binary and args.action not in ("install", "migrate"):
        parser.error("--binary applies to install and migrate")
    if args.action == "install" and platform == "darwin":
        pass
    elif not platform.startswith("linux") or not shutil.which("systemctl"):
        print("zerus-setup --mobile-connector manages systemd user units on Linux; on macOS it only installs the agent.",
              file=sys.stderr)
        return 2
    try:
        if args.action == "install":
            install(args, platform)
        else:
            {"protect": protect, "migrate": migrate, "rollback": rollback}[args.action](args)
    except Refusal as refusal:
        print(f"Refused: {refusal}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
