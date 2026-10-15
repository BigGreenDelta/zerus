#!/usr/bin/env python3
"""A scripted stand-in for `hgs` that black-box connector tests put on PATH.

It never runs tmux or an agent, and runs a real hgs only for an `exec` rule.
Every call re-reads `script.json` from the directory named by
ZERUS_SCRIPTED_HGS_DIR, answers with the first matching rule and appends
`start` and `end` events to `calls.jsonl` there.

A rule matches with `argv` (exact) or `prefix` (leading arguments), and with
`stdin_fields` also only when stdin is a JSON object whose listed fields have
exactly these JSON values, so that `true` never matches 1 (a terminal or
speech `action`, or a peer envelope's `via`, `target_machine_id` and inner
`argv`, say); `times` limits how many calls it answers under its `id` (its
index without one). It may set:

- `stdout` (a JSON value) or `stdout_text`, `stderr` (text) and `exit` (status);
- `stdout_path`: write that file's bytes verbatim, for answers too large to
  keep in the script;
- `exec`: run that program (a real hgs) with the same arguments, stdin and
  environment, and answer with its stdout, stderr and exit status;
- `delay` (seconds to work before answering);
- `kill_parent`: after writing stdout, SIGKILL the parent before exiting, but
  only when the parent's pid is the one in `connector.pid`. This leaves a
  connector that never saw the native outcome.

Strings in `stdout` that are exactly `{{stdin}}`, `{{stdin.FIELD}}` or
`{{argv.N}}` become the parsed stdin, one of its fields or one argument; a
dotted FIELD such as `payload.request_id` or `argv.4` descends into objects
and arrays. A rule's `fill` maps placeholders to `[unit, count]` parts, as the
golden vectors do: a string in `stdout`, or the whole `stdout_text` or
`stderr`, equal to a placeholder becomes the concatenation of its parts, so a
rule can answer with megabytes without a large script.
An unmatched call exits 2.
"""
import fcntl
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import time


def append(log, event):
    os.write(log, (json.dumps(event, sort_keys=True) + "\n").encode())


def record(log, event):
    # One write per line under the log's lock, so a reader holding the lock
    # sees whole lines; an unlocked reader drops an unterminated last line.
    fcntl.flock(log, fcntl.LOCK_EX)
    try:
        append(log, event)
    finally:
        fcntl.flock(log, fcntl.LOCK_UN)


def open_descriptors():
    found = []
    for fd in range(256):
        try:
            os.fstat(fd)
        except OSError:
            continue
        found.append(fd)
    return found


def expand(value, fill):
    parts = fill.get(value) if isinstance(value, str) else None
    return value if parts is None else "".join(unit * count for unit, count in parts)


def field(value, path):
    for part in path.split("."):
        if isinstance(value, dict):
            value = value.get(part)
        elif isinstance(value, list) and part.isdigit() and int(part) < len(value):
            value = value[int(part)]
        else:
            return None
    return value


def substitute(value, argv, stdin, fill=None):
    fill = fill or {}
    if isinstance(value, dict):
        return {key: substitute(item, argv, stdin, fill) for key, item in value.items()}
    if isinstance(value, list):
        return [substitute(item, argv, stdin, fill) for item in value]
    if isinstance(value, str) and value in fill:
        return expand(value, fill)
    if isinstance(value, str) and value.startswith("{{") and value.endswith("}}"):
        name = value[2:-2]
        if name == "stdin":
            return stdin
        if name.startswith("stdin."):
            return field(stdin, name[6:])
        if name.startswith("argv."):
            index = int(name[5:])
            return argv[index] if index < len(argv) else None
    return value


def same(left, right):
    # JSON equality, so that `true` never matches 1.
    return json.dumps(left, sort_keys=True) == json.dumps(right, sort_keys=True)


def matches(rule, argv, stdin):
    if "argv" in rule:
        found = argv == rule["argv"]
    else:
        prefix = rule.get("prefix", [])
        found = argv[:len(prefix)] == prefix
    expected = rule.get("stdin_fields")
    if found and expected is not None:
        found = isinstance(stdin, dict) and all(key in stdin and same(stdin[key], value)
                                                for key, value in expected.items())
    return found


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
    rules = script.get("rules", [])
    # Count earlier answers and record this call's start under one lock, so
    # concurrent calls never both take the last use of a `times` rule.
    fcntl.flock(log, fcntl.LOCK_EX)
    try:
        previous = {}
        with open(root / "calls.jsonl", "rb") as earlier:
            for line in earlier.read().splitlines():
                event = json.loads(line)
                if event.get("event") == "start" and event.get("rule") is not None:
                    previous[event["rule"]] = previous.get(event["rule"], 0) + 1
        chosen = rule = None
        for index, candidate in enumerate(rules):
            key = candidate.get("id", index)
            if matches(candidate, argv, stdin) and ("times" not in candidate or previous.get(key, 0) < candidate["times"]):
                chosen, rule = key, candidate
                break
        append(log, {"event": "start", "pid": os.getpid(), "ppid": os.getppid(), "argv": argv,
                     "stdin": text, "env": dict(os.environ), "fds": descriptors,
                     "rule": chosen, "at": time.time()})
    finally:
        fcntl.flock(log, fcntl.LOCK_UN)
    if rule is None:
        sys.stderr.write("scripted hgs: unsupported operation\n")
        record(log, {"event": "end", "pid": os.getpid(), "argv": argv, "exit": 2, "at": time.time()})
        return 2
    if rule.get("delay"):
        time.sleep(float(rule["delay"]))
    fill = rule.get("fill", {})
    errors = expand(rule.get("stderr", ""), fill).encode()
    code = int(rule.get("exit", 0))
    if "exec" in rule:
        native = subprocess.run([rule["exec"], *argv], input=raw, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        output, errors, code = native.stdout, native.stderr, native.returncode
    elif "stdout" in rule:
        output = (json.dumps(substitute(rule["stdout"], argv, stdin, fill)) + "\n").encode()
    elif "stdout_path" in rule:
        output = Path(rule["stdout_path"]).read_bytes()
    else:
        output = expand(rule.get("stdout_text", ""), fill).encode()
    # Recorded before writing: a connector that has already gone cannot read
    # it, and the event still proves this call finished on its own.
    record(log, {"event": "end", "pid": os.getpid(), "argv": argv, "exit": code, "at": time.time()})
    try:
        if errors:
            sys.stderr.buffer.write(errors)
            sys.stderr.flush()
        sys.stdout.buffer.write(output)
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
