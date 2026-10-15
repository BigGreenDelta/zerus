#!/usr/bin/env python3
"""A scripted stand-in for `hgs` that black-box connector tests put on PATH.

It never runs hgs, tmux or an agent. Every call re-reads `script.json` from the
directory named by ZERUS_SCRIPTED_HGS_DIR, answers with the first matching rule
and appends `start` and `end` events to `calls.jsonl` there.

A rule matches with `argv` (exact) or `prefix` (leading arguments); `times`
limits how many calls it answers. It may set:

- `stdout` (a JSON value) or `stdout_text`, `stderr` (text) and `exit` (status);
- `delay` (seconds to work before answering);
- `kill_parent`: after writing stdout, SIGKILL the parent before exiting, but
  only when the parent's pid is the one in `connector.pid`. This leaves a
  connector that never saw the native outcome.

Strings in `stdout` that are exactly `{{stdin}}`, `{{stdin.FIELD}}` or
`{{argv.N}}` become the parsed stdin, one of its fields or one argument.
An unmatched call exits 2.
"""
import json
import os
from pathlib import Path
import signal
import sys
import time


def record(log, event):
    # One write per line: O_APPEND keeps concurrent callers' lines whole.
    os.write(log, (json.dumps(event, sort_keys=True) + "\n").encode())


def open_descriptors():
    found = []
    for fd in range(256):
        try:
            os.fstat(fd)
        except OSError:
            continue
        found.append(fd)
    return found


def substitute(value, argv, stdin):
    if isinstance(value, dict):
        return {key: substitute(item, argv, stdin) for key, item in value.items()}
    if isinstance(value, list):
        return [substitute(item, argv, stdin) for item in value]
    if isinstance(value, str) and value.startswith("{{") and value.endswith("}}"):
        name = value[2:-2]
        if name == "stdin":
            return stdin
        if name.startswith("stdin."):
            return stdin.get(name[6:]) if isinstance(stdin, dict) else None
        if name.startswith("argv."):
            index = int(name[5:])
            return argv[index] if index < len(argv) else None
    return value


def matches(rule, argv):
    if "argv" in rule:
        return argv == rule["argv"]
    prefix = rule.get("prefix", [])
    return argv[:len(prefix)] == prefix


def main():
    root = Path(os.environ["ZERUS_SCRIPTED_HGS_DIR"])
    argv = sys.argv[1:]
    descriptors = open_descriptors()
    log = os.open(root / "calls.jsonl", os.O_WRONLY | os.O_APPEND | os.O_CREAT, 0o600)
    raw = sys.stdin.buffer.read() if sys.stdin is not None else b""
    text = raw.decode("utf-8", errors="replace")
    try:
        stdin = json.loads(text) if text.strip() else None
    except ValueError:
        stdin = None
    script = json.loads((root / "script.json").read_text())
    previous = {}
    if (root / "calls.jsonl").exists():
        for line in (root / "calls.jsonl").read_text().splitlines():
            event = json.loads(line)
            if event.get("event") == "start" and event.get("rule") is not None:
                previous[event["rule"]] = previous.get(event["rule"], 0) + 1
    chosen = None
    for index, rule in enumerate(script.get("rules", [])):
        if matches(rule, argv) and ("times" not in rule or previous.get(index, 0) < rule["times"]):
            chosen = index
            break
    started = time.time()
    record(log, {"event": "start", "pid": os.getpid(), "ppid": os.getppid(), "argv": argv,
                 "stdin": text, "env": dict(os.environ), "fds": descriptors,
                 "rule": chosen, "at": started})
    if chosen is None:
        sys.stderr.write("scripted hgs: unsupported operation\n")
        record(log, {"event": "end", "pid": os.getpid(), "argv": argv, "exit": 2, "at": time.time()})
        return 2
    rule = script["rules"][chosen]
    if rule.get("delay"):
        time.sleep(float(rule["delay"]))
    if "stdout" in rule:
        output = json.dumps(substitute(rule["stdout"], argv, stdin)) + "\n"
    else:
        output = rule.get("stdout_text", "")
    code = int(rule.get("exit", 0))
    # Recorded before writing: a connector that has already gone cannot read
    # it, and the event still proves this call finished on its own.
    record(log, {"event": "end", "pid": os.getpid(), "argv": argv, "exit": code, "at": time.time()})
    try:
        if rule.get("stderr"):
            sys.stderr.write(rule["stderr"])
            sys.stderr.flush()
        sys.stdout.write(output)
        sys.stdout.flush()
    except (BrokenPipeError, OSError):
        pass
    if rule.get("kill_parent"):
        expected = (root / "connector.pid").read_text().strip() if (root / "connector.pid").exists() else ""
        if expected and int(expected) == os.getppid():
            os.kill(os.getppid(), signal.SIGKILL)
    return code


if __name__ == "__main__":
    try:
        sys.exit(main())
    except BrokenPipeError:
        sys.exit(0)
