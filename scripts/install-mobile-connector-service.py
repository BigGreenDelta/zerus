#!/usr/bin/env python3
"""Protect or switch the per-user Zerus mobile connector service.

  protect   give the unit KillMode=process when it does not have it yet
  migrate   point a Python connector unit at the Rust connector
  rollback  remove the migration drop-in again

Each action writes or removes one drop-in of its own in the user's systemd
configuration and reloads the user manager, which restarts nothing. It never
edits, moves or deletes another file, and never enables, starts, stops or
restarts a unit: the cutover runbook (docs/mobile-deployment.md) drains and
restarts by hand. Linux only.
"""
import argparse
import fcntl
import json
import os
from pathlib import Path
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


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("action", choices=("protect", "migrate", "rollback"))
    parser.add_argument("--binary", help="the Rust connector for migrate when both installed copies exist")
    args = parser.parse_args(argv)
    if not sys.platform.startswith("linux") or not shutil.which("systemctl"):
        print("zerus-setup --mobile-connector manages systemd user units on Linux only.", file=sys.stderr)
        return 2
    if args.binary and args.action != "migrate":
        parser.error("--binary applies to migrate")
    try:
        {"protect": protect, "migrate": migrate, "rollback": rollback}[args.action](args)
    except Refusal as refusal:
        print(f"Refused: {refusal}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
