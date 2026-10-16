"""Differential traces of the black-box suites (C7 parity gate).

With ZERUS_CONNECTOR_TRACE_DIR set, every BlackBoxCase that runs to its end
writes one normalized trace there: the native `hgs` calls (argv, parsed stdin
and environment keys), the relay traffic (poll queries, parsed uploads and
heartbeats with their answers) and the final journal rows. The suites run once
against Python and once against Rust with the same seeded test identities, and

    python3 differential.py compare PYTHON_DIR RUST_DIR

fails on any difference that `differential-allowlist.json` does not name. The
traces are parsed, so key order and whitespace never differ; connector logs are
not traced. Every allowlist entry cites one of the deliberate divergences a-k
(design spec), and an entry that no longer matches anything fails too.

Timing decides how often a connector heartbeats, polls or retries, so periodic
calls and relay exchanges are compared by presence; native mutations and reads
and journal rows must also match in number.
"""
from __future__ import annotations

import argparse
import collections
import contextlib
import fnmatch
import hashlib
import json
from pathlib import Path
import random
import re
import shutil
import sqlite3
import sys
import uuid

TESTS = Path(__file__).resolve().parent
ALLOWLIST = TESTS / "differential-allowlist.json"
DIVERGENCES = set("abcdefghijk")
KINDS = ("native", "relay", "journal")
# The harness adds these for a Python connector run from this source tree.
HARNESS_ENVIRONMENT = {"PYTHONPATH", "PYTHONDONTWRITEBYTECODE"}
# Native commands a connector repeats on its own schedule rather than once per
# request: heartbeat snapshots, capability probes and fleet discovery.
PERIODIC = (
    ["ls", "--json", "--local"],
    ["--help"],
    ["help"],
    ["account", "ls"],
    ["account", "inspect"],
    ["archives", "ls"],
    ["swarm", "get"],
    ["swarm", "mobile-peers", "--json"],
)
# Relay traffic follows the clock (heartbeats, presence, polls), and a result
# whose answer was lost on a busy machine is uploaded again and accepted again,
# so relay exchanges are compared as distinct bodies and answers.
PEER_HEARTBEAT = re.compile(r"/v1/node/peers/[^/]+/heartbeat")
LARGE = 512


REAL_HGS = "runs a real hgs, whose machine ids and paths differ between any two runs"


def untraced(reason: str):
    """Leave a test class or method out of the comparison, for a reason the
    compare report repeats."""
    def mark(item):
        item.__untraced__ = reason
        return item
    return mark


def seeded_uuid4(seed: str):
    """uuid.uuid4 for a traced test: the same identities in both runs."""
    rng = random.Random(seed)

    def uuid4():
        return uuid.UUID(int=rng.getrandbits(128), version=4)
    return uuid4


class Normalizer:
    def __init__(self, case):
        relay = case.relay
        # The relay binding (and the local project id derived from it) hashes
        # the relay URL and node token, which every run draws afresh.
        bindings = {hashlib.sha256((url + "\0" + relay.token).encode()).hexdigest() for url in (relay.url, relay.url + "/")}
        self.journal = stopped_copy(case)
        if self.journal is not None:
            with contextlib.suppress(sqlite3.DatabaseError):
                bindings |= {row[0] for row in self.journal.execute("SELECT value FROM metadata WHERE key='relay_binding'")}
        self.replacements = [(hashlib.sha256(binding.encode()).hexdigest(), "<binding-digest>") for binding in bindings]
        self.replacements += [(binding, "<binding>") for binding in bindings]
        # Python's request_hash of every body the relay handed out, by identity.
        self.delivered = collections.defaultdict(set)
        for exchange in relay.exchanges:
            if exchange.delivered and isinstance(exchange.response, dict):
                for request in exchange.response.get("requests", []):
                    with contextlib.suppress(TypeError, ValueError, AttributeError):
                        encoded = json.dumps(request, sort_keys=True, separators=(",", ":"), allow_nan=False)
                        self.delivered[request.get("request_id")].add(hashlib.sha256(encoded.encode()).hexdigest())
        self.replacements += [(value, name) for value, name in (
            (str(case.scratch), "<scratch>"), (relay.url, "<relay>"), (relay.token, "<token>")) if value]

    def text(self, value: str) -> str:
        for old, new in self.replacements:
            value = value.replace(old, new)
        return value

    def value(self, value):
        if isinstance(value, str):
            value = self.text(value)
            if len(value) > LARGE:
                return digest(value)
            return value
        if isinstance(value, list):
            return [self.value(item) for item in value]
        if isinstance(value, dict):
            return {self.text(key): "<time>" if timestamp(key, item) else
                    "<metrics>" if key == "metrics" and isinstance(item, dict) else self.value(item)
                    for key, item in value.items()}
        return value

    def bounded(self, value):
        """Large containers become a digest, so a trace stays small."""
        value = self.value(value)
        encoded = canonical(value)
        return digest(encoded) if len(encoded) > 8 * LARGE else value


def timestamp(key: str, value) -> bool:
    """A clock reading such as `checked_at`, which no two runs share."""
    return key.endswith("_at") and isinstance(value, (int, float)) and not isinstance(value, bool)


def digest(text: str) -> str:
    return f"<sha256:{hashlib.sha256(text.encode('utf-8', 'surrogatepass')).hexdigest()[:16]} len={len(text)}>"


def canonical(value) -> str:
    return json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=True)


def parsed(text):
    if text is None:
        return None
    try:
        return {"json": json.loads(text)}
    except ValueError:
        return {"text": text}


PEER_ENVELOPES = (["swarm", "mobile-peer", "--json"], ["swarm", "__mobile-peer-local", "--json"])


def periodic(argv: list) -> bool:
    return any(argv[:len(prefix)] == prefix for prefix in PERIODIC)


def native_entry(event: dict, norm: Normalizer) -> str:
    stdin = parsed(event.get("stdin") or None)
    argv = event["argv"]
    repeated = periodic(argv)
    envelope = stdin.get("json") if stdin else None
    if argv in PEER_ENVELOPES and isinstance(envelope, dict) and isinstance(envelope.get("argv"), list):
        repeated = periodic(envelope["argv"])
        if repeated:
            # Which peer a refresh reaches first is scheduling, not contract.
            stdin = {"json": {**envelope, **{key: "<peer>" for key in ("via", "target_machine_id") if key in envelope}}}
    return canonical({"argv": norm.value(argv), "stdin": norm.bounded(stdin), "periodic": repeated,
                      "env": sorted(set(event.get("env", {})) - HARNESS_ENVIRONMENT)})


def relay_entry(exchange, norm: Normalizer) -> str | None:
    path = norm.text(exchange.path)
    if exchange.method == "GET" and exchange.status == 499:
        return None  # A poll still open when the connector stopped.
    if exchange.method == "GET":
        query = dict(part.partition("=")[::2] for part in exchange.query.split("&") if part)
        query.pop("after", None)  # a cursor, whatever the clock made it
        return f"GET {path} {canonical(query)} {exchange.status}"
    body = exchange.body
    if PEER_HEARTBEAT.fullmatch(path) and isinstance(body, dict):
        # Peer refreshes follow the clock in a scheduling order: compare their shape.
        path = "/v1/node/peers/<peer>/heartbeat"
        machine = body.get("machine_id")
        if isinstance(machine, str):
            # A peer's local project id hashes its machine id.
            mark = hashlib.sha256(machine.encode()).hexdigest()
            body = json.loads(canonical({**body, "machine_id": "<peer>"}).replace(mark, "<peer-digest>"))
    body = norm.bounded(body) if body is not None else {"raw": digest(exchange.raw.decode("utf-8", "replace"))}
    return f"{exchange.method} {path} {canonical(body)} {exchange.status}"


def stopped_copy(case) -> sqlite3.Connection | None:
    """The final journal of a stopped connector, opened from a copy: a killed
    one may leave a hot rollback journal or a write-ahead log that only a
    writer can recover."""
    def regular(path: Path) -> bool:
        return path.is_file() and not path.is_symlink()

    source = case.journal.path
    if not regular(source):
        return None  # No journal, or an unsafe one a case planted.
    copy = case.scratch / "trace-journal"
    copy.mkdir(exist_ok=True)
    for suffix in ("", "-journal", "-wal"):
        if regular(Path(str(source) + suffix)):
            shutil.copyfile(str(source) + suffix, copy / (source.name + suffix))
    db = sqlite3.connect(copy / source.name)
    db.row_factory = sqlite3.Row
    return db


def journal_rows(norm: Normalizer) -> list[str]:
    if norm.journal is None:
        return []
    rows = []
    try:
        stored = norm.journal.execute("SELECT * FROM requests ORDER BY request_id").fetchall()
    except sqlite3.DatabaseError as error:
        # Some cases leave a file that is no journal, or one without rows.
        return [f"<{type(error).__name__}>"]
    for row in stored:
        row = dict(row)
        # Whether the last accepted upload was marked delivered races the
        # teardown's SIGKILL; result uploads are compared instead.
        row.pop("delivered", None)
        row.pop("delivered_at", None)
        # Requests may carry the test's clock (a terminal deadline, say), so
        # compare how each implementation hashed them, not the digests.
        if row.get("request_hash") in norm.delivered.get(row.get("request_id"), ()):
            row["request_hash"] = "<hash of a delivered body>"
        row["response"] = norm.bounded(parsed(row.get("response")))
        rows.append(canonical(norm.value(row)))
    return rows


def skipped(case) -> bool:
    outcome = getattr(case, "_outcome", None)
    result = getattr(outcome, "result", None)
    return any(test is case for test, _ in getattr(result, "skipped", ()))


def record(case, directory: str) -> None:
    """Write this case's trace; called from BlackBoxCase.asyncTearDown."""
    if skipped(case):
        return
    target = Path(directory)
    target.mkdir(parents=True, exist_ok=True)
    reason = getattr(getattr(case, case._testMethodName, None), "__untraced__", None) or getattr(case, "__untraced__", None)
    if reason:
        (target / (case.id() + ".json")).write_text(json.dumps({"test": case.id(), "untraced": reason}) + "\n")
        return
    norm = Normalizer(case)
    native = collections.Counter(native_entry(event, norm) for event in case.hgs.events() if event["event"] == "start")
    relay = collections.Counter(entry for entry in (relay_entry(exchange, norm) for exchange in case.relay.exchanges)
                                if entry is not None)
    trace = {"test": case.id(), "native": dict(sorted(native.items())), "relay": dict(sorted(relay.items())),
             "journal": journal_rows(norm)}
    if norm.journal is not None:
        norm.journal.close()
    (target / (case.id() + ".json")).write_text(json.dumps(trace, indent=1, sort_keys=True) + "\n")


def counted(kind: str, entry: str) -> bool:
    """Native work and journal rows must match in number."""
    if kind == "native":
        return not json.loads(entry)["periodic"]
    return kind == "journal"


def differences(python: dict, rust: dict, timing: bool = False) -> list[dict]:
    """With `timing`, a test whose clock-driven traffic differs between any
    two runs compares only native work and journal rows."""
    found = []
    for kind in KINDS:
        left, right = collections.Counter(python[kind]), collections.Counter(rust[kind])
        for entry in sorted(set(left) | set(right)):
            if timing and not counted(kind, entry):
                continue
            if entry not in right:
                found.append({"kind": kind, "side": "python", "entry": entry})
            elif entry not in left:
                found.append({"kind": kind, "side": "rust", "entry": entry})
            elif counted(kind, entry) and left[entry] != right[entry]:
                found.append({"kind": kind, "side": "count", "entry": entry,
                              "python": left[entry], "rust": right[entry]})
    return found


def load_allowlist(path: Path) -> tuple[list[dict], list[dict]]:
    document = json.loads(path.read_text())
    if set(document) != {"allow", "timing"}:
        raise SystemExit("the allowlist holds exactly `allow` and `timing`")
    entries, timing = document["allow"], document["timing"]
    for entry in timing:
        if set(entry) != {"test", "why"} or not entry["why"].strip():
            raise SystemExit(f"timing entry needs exactly test and why: {entry}")
    for entry in entries:
        unknown = set(entry) - {"test", "kind", "side", "match", "divergence", "why"}
        if unknown or not {"test", "kind", "side", "match", "divergence", "why"} <= set(entry):
            raise SystemExit(f"allowlist entry needs exactly test, kind, side, match, divergence and why: {entry}")
        if entry["divergence"] not in DIVERGENCES or entry["kind"] not in KINDS or entry["side"] not in ("python", "rust", "count"):
            raise SystemExit(f"allowlist entry names an unknown divergence, kind or side: {entry}")
        re.compile(entry["match"])
    return entries, timing


def allowed(entries: list[dict], test: str, difference: dict) -> dict | None:
    return next((entry for entry in entries if fnmatch.fnmatchcase(test, entry["test"])
                 and entry["kind"] == difference["kind"] and entry["side"] == difference["side"]
                 and re.search(entry["match"], difference["entry"])), None)


def compare(python_dir: Path, rust_dir: Path, allowlist: Path, out=sys.stdout) -> int:
    entries, timing = load_allowlist(allowlist)
    used = set()
    python = {path.stem: json.loads(path.read_text()) for path in python_dir.glob("*.json")}
    rust = {path.stem: json.loads(path.read_text()) for path in rust_dir.glob("*.json")}
    if not python or not rust:
        print("differential: no traces to compare", file=out)
        return 1
    unexpected = 0
    untraced = collections.Counter()
    for test in sorted(set(python) & set(rust)):
        reason = python[test].get("untraced") or rust[test].get("untraced")
        if reason:
            untraced[reason] += 1
            continue
        clocked = any(fnmatch.fnmatchcase(test, entry["test"]) for entry in timing)
        for difference in differences(python[test], rust[test], clocked):
            entry = allowed(entries, test, difference)
            if entry is not None:
                used.add(id(entry))
                continue
            unexpected += 1
            detail = "" if difference["side"] != "count" else f" (python {difference['python']}, rust {difference['rust']})"
            print(f"{test}: {difference['kind']} only in {difference['side']}{detail}: {difference['entry']}", file=out)
    stale = [entry for entry in entries if id(entry) not in used]
    for entry in stale:
        print(f"differential: allowlist entry matched nothing: {canonical(entry)}", file=out)
    for reason, count in sorted(untraced.items()):
        print(f"differential: {count} untraced: {reason}", file=out)
    print(f"differential: {len(set(python) & set(rust)) - sum(untraced.values())} tests compared, "
          f"{len(set(python) - set(rust))} traced only "
          f"against Python, {len(set(rust) - set(python))} only against Rust, {unexpected} unexpected differences",
          file=out)
    return 1 if unexpected or stale else 0


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    commands = parser.add_subparsers(dest="command", required=True)
    command = commands.add_parser("compare")
    command.add_argument("python", type=Path)
    command.add_argument("rust", type=Path)
    command.add_argument("--allowlist", type=Path, default=ALLOWLIST)
    args = parser.parse_args(argv)
    return compare(args.python, args.rust, args.allowlist)


if __name__ == "__main__":
    sys.exit(main())
