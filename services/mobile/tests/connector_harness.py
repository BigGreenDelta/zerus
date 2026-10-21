"""Black-box harness: the connector runs as its own process, as it is deployed.

Tests drive it only through its command line, its configuration file, a fake
relay on loopback and a scripted `hgs` on PATH, and read its journal only
read-only while it runs. The same suites can therefore run against another
implementation of the connector:

- ZERUS_CONNECTOR_COMMAND is the command line (split like a shell) to run
  instead of `python -m zerus_mobile.connector` from this source tree;
- ZERUS_CONNECTOR_IMPLEMENTATION names it, `python` or `rust` (default:
  `python` without a command, `rust` with one). Assertions on a reviewed
  divergence (BLACKBOX.md) depend on it.

Outside Python mode only the modules listed in `ported.json` run.

Every connector gets a scratch HOME, XDG directories, TMPDIR, HGS_CONFIG_DIR,
HGS_STATE_DIR and state_dir, a PATH of only the scripted hgs and the system
directories, and none of the caller's environment. Its configuration may name
only a loopback relay, or a reserved `.invalid` host that startup must refuse.
"""
from __future__ import annotations

import asyncio
import collections
import contextlib
import dataclasses
import fcntl
import ipaddress
import json
import math
import os
from pathlib import Path
import re
import secrets
import shlex
import shutil
import signal
import sqlite3
import stat
import subprocess
import sys
import tempfile
import time
import unittest
from unittest import mock
from urllib.parse import parse_qsl, urlsplit
import uuid

from aiohttp import web

import differential

TESTS = Path(__file__).resolve().parent
SERVICE = TESTS.parent
ROOT = SERVICE.parent.parent
MIB = 1024 * 1024
# nginx applies its default 1 MiB client_max_body_size to `location ^~ /v1/`,
# and the relay accepts at most that much in every node POST.
WIRE_LIMIT = MIB
# The relay parses submissions with these limits (relay config.rs) and wraps
# each claimed one in {"requests": [...]}, so a batch may exceed them.
JSON_DEPTH = 32
JSON_VALUES = 50000
REQUEST_MAX = 29 * MIB
STARTUP_FAILURE = "Connector could not start; verify private configuration and journal availability"
RESTART_ERROR = "connector stopped before recording the native result"

# The calling agent's identity, which native hgs never receives. The rest of
# the connector's environment, HGS_CONFIG_DIR and HGS_PEERS included, passes on.
CONTEXT_KEYS = frozenset({
    "TMUX", "TMUX_PANE", "HGS_SESSION", "HGS_RUN_ID", "HGS_EXPECTED_ID",
    "HGS_EXECUTABLE", "HGS_AGENT", "HGS_FRESH", "HGS_FRESH_REPLACES", "HGS_REQUESTED_ID",
    "HGS_ARCHIVE_ID", "HGS_LAUNCH_ID", "HGS_FORK_PARENT_ID", "HGS_ACCOUNT_ID",
    "HGS_CLIENT", "CLAUDE_CONFIG_DIR", "CODEX_HOME", "KIMI_CODE_HOME",
    "DSH_HOME", "DSH_CONFIG_DIR", "OPENCODE_CONFIG_DIR", "OPENCODE_CONFIG", "OPENCODE_CONFIG_CONTENT",
})

SESSION = "claude/example"
RUN_ID = "bb-run-1"
CONVERSATION_ID = "bb-conversation-1"
# No mobile ABI markers: every optional operation stays unavailable.
HELP_TEXT = "Usage: hgs [@host] <command> [options]\n"


# The Python connector of this source tree.
PYTHON_COMMAND = [sys.executable, "-m", "zerus_mobile.connector"]


def connector_command() -> list[str]:
    configured = os.environ.get("ZERUS_CONNECTOR_COMMAND")
    if configured:
        return shlex.split(configured)
    return PYTHON_COMMAND


IMPLEMENTATION = os.environ.get("ZERUS_CONNECTOR_IMPLEMENTATION") or (
    "rust" if os.environ.get("ZERUS_CONNECTOR_COMMAND") else "python")


# Settings a Rust build reads only with its non-default `test-hooks` feature,
# which packages never enable: the default native timeout in seconds (Python's
# is 45) and a factor that divides the connector's own retry spacing (30 s
# between attempts under divergence b). Python ignores both, so a test that
# needs one runs only against Rust.
NATIVE_TIMEOUT_HOOK = "ZERUS_CONNECTOR_TEST_NATIVE_TIMEOUT"
CLOCK_SCALE_HOOK = "ZERUS_CONNECTOR_TEST_CLOCK_SCALE"
# Where each case writes its differential trace (differential.py), if anywhere.
TRACE_DIR = os.environ.get("ZERUS_CONNECTOR_TRACE_DIR")

# A shell without job control starts background commands with SIGINT ignored,
# and an ignored signal stays ignored across exec: a Python connector would
# never see Ctrl+C. Connectors start with the default disposition, as under
# systemd or launchd, however these suites were started.
if signal.getsignal(signal.SIGINT) == signal.SIG_IGN:
    signal.signal(signal.SIGINT, signal.default_int_handler)


def ported_modules() -> set[str]:
    return set(json.loads((TESTS / "ported.json").read_text())["rust"])


def implemented(operation) -> bool:
    """Whether the connector under test executes this known operation. A Rust
    build refuses the others as unsupported (`rust_operations` in ported.json,
    checked against the build by its runtime_vectors test)."""
    return IMPLEMENTATION == "python" or operation in json.loads((TESTS / "ported.json").read_text())["rust_operations"]


def canonical(value) -> str:
    """The relay's wire and identity encoding (relay json.rs)."""
    return json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=True, allow_nan=False)


_STRING = re.compile(rb'"(?:[^"\\]|\\.)*"', re.S)
_SCALAR = re.compile(rb'[^\s,:\[\]{}"]+')
_BRACKET = re.compile(rb"[\[\]{}]")


def json_limits(raw: bytes) -> tuple[int, int]:
    """(depth, values) as the relay's json::parse counts them before parsing."""
    strings = len(_STRING.findall(raw))
    stripped = _STRING.sub(b'"', raw)
    values = strings + len(_SCALAR.findall(stripped)) + stripped.count(b"{") + stripped.count(b"[")
    depth = deepest = 0
    for bracket in _BRACKET.findall(stripped):
        depth += 1 if bracket in (b"{", b"[") else -1
        deepest = max(deepest, depth)
    return deepest, values


def strict_json(raw: bytes):
    """Parse as the relay does: UTF-8, no NaN or Infinity, no lone surrogates."""
    def constant(name):
        raise ValueError(name)

    value = json.loads(raw.decode("utf-8"), parse_constant=constant)
    pending = [value]
    while pending:
        item = pending.pop()
        if isinstance(item, dict):
            for key, child in item.items():
                key.encode("utf-8")
                pending.append(child)
        elif isinstance(item, list):
            pending.extend(item)
        elif isinstance(item, str):
            item.encode("utf-8")
        elif isinstance(item, float) and not math.isfinite(item):
            raise ValueError("non-finite number")
    return value


def text_field(value, maximum, empty=False) -> bool:
    return (isinstance(value, str) and (empty or bool(value)) and len(value) <= maximum
            and not any(c < " " or c == "\x7f" for c in value))


def canonical_uuid(value) -> bool:
    try:
        return isinstance(value, str) and str(uuid.UUID(value)) == value
    except ValueError:
        return False


def session_row(**changes) -> dict:
    """An `hgs ls --json --local` row. Not a live tracked Codex row, so the
    connector's background attention scan never inspects it."""
    return {"name": SESSION, "cmd": "claude", "project": "example", "tag": None,
            "attached": False, "clients": [], "created": 1700000000, "launch_id": None,
            "run_id": RUN_ID, "conversation_id": CONVERSATION_ID, "tracked": True,
            "runtime_state": "live", "process_state": "running", "activity": "idle",
            "phase": "idle", **changes}


def listing(**extra) -> dict:
    """The Unix `hgs ls --json --local` top level without optional keys."""
    return {"host": "Synthetic computer", "ok": True, "hgs_version": "0.0.0-blackbox",
            "projects": {}, "sessions": [session_row()],
            "metrics": {"sampled_at": 1.0, "cpu_state": "unavailable"},
            "preferences": {"mobile_delivery": "away"}, **extra}


def inspection() -> dict:
    return {"name": SESSION, "run_id": RUN_ID, "conversation_id": CONVERSATION_ID,
            "runtime_state": "live", "process_state": "running", "activity": "idle",
            "events": [{"seq": 1, "type": "UserPromptSubmit", "agent_id": "", "at": 1.0,
                        "detail": "Synthetic request."}],
            "provider_messages": []}


def acknowledgement(**extra) -> dict:
    return {"request_id": "{{stdin.request_id}}", "status": "submitted", "name": SESSION,
            "run_id": RUN_ID, "conversation_id": CONVERSATION_ID, **extra}


def default_rules(listing_value: dict | None = None, help_text: str = HELP_TEXT) -> list[dict]:
    return [
        {"argv": ["--help"], "stdout_text": help_text},
        {"argv": ["ls", "--json", "--local"], "stdout": listing() if listing_value is None else listing_value},
        {"argv": ["ls", "--json", "--local", "--no-archives"], "stdout": listing() if listing_value is None else listing_value},
        {"argv": ["swarm", "get"], "stdout": {"schema": 1, "initialized": False, "organization": {"projects": []}}},
        {"argv": ["account", "ls"], "stdout": {"profiles": []}},
        {"argv": ["inspect", SESSION], "stdout": inspection()},
        {"argv": ["send", SESSION, "--json"], "stdout": acknowledgement(text="{{stdin.text}}")},
        {"argv": ["interrupt", SESSION, "--json"], "stdout": acknowledgement()},
    ]


# Every rule key scripted_hgs.py reads.
RULE_KEYS = frozenset({"id", "argv", "prefix", "stdin_fields", "times", "stdout", "stdout_text", "stdout_path",
                       "stderr", "exit", "exec", "delay", "kill_parent", "fill"})


class ScriptedHgs:
    """The harness side of scripted_hgs.py: its script, call log and PATH entry."""

    def __init__(self, root: Path):
        self.root = root
        self.bin = root / "bin"
        self.bin.mkdir(parents=True, mode=0o700)
        wrapper = self.bin / "hgs"
        wrapper.write_text("#!/bin/sh\nZERUS_SCRIPTED_HGS_DIR=" + shlex.quote(str(root))
                           + " exec " + shlex.quote(sys.executable) + " -I -S "
                           + shlex.quote(str(TESTS / "scripted_hgs.py")) + ' "$@"\n')
        wrapper.chmod(0o700)
        self.rules: list[dict] = []
        self._ids = 0

    def script(self, rules: list[dict]) -> None:
        """Replace the script; the next call reads it. A rule without an `id`
        gets a fresh one, so its `times` counts only calls answered since. A
        key scripted_hgs.py does not know is refused here, since it would
        otherwise be ignored and widen the rule."""
        for rule in rules:
            unknown = set(rule) - RULE_KEYS
            if unknown:
                raise ValueError(f"scripted hgs rule has unknown keys {sorted(unknown)}: {rule}")
            if not isinstance(rule.get("stdin_fields", {}), dict):
                raise ValueError(f"scripted hgs `stdin_fields` must be an object: {rule}")
        self.rules = []
        for rule in rules:
            self._ids += 1
            self.rules.append({**rule, "id": rule.get("id", f"r{self._ids}")})
        temporary = self.root / "script.json.new"
        temporary.write_text(json.dumps({"rules": self.rules}))
        os.replace(temporary, self.root / "script.json")

    def prepend(self, *rules: dict) -> None:
        self.script([*rules, *[{k: v for k, v in rule.items() if k != "id"} for rule in self.rules]])

    def parent(self, pid: int) -> None:
        (self.root / "connector.pid").write_text(str(pid))

    def events(self) -> list[dict]:
        log = self.root / "calls.jsonl"
        if not log.exists():
            return []
        # A call may be appending its line right now: skip an unterminated end.
        lines = log.read_bytes().split(b"\n")[:-1]
        return [json.loads(line) for line in lines if line.strip()]

    def calls(self, *prefix: str) -> list[dict]:
        return [event for event in self.events() if event["event"] == "start"
                and event["argv"][:len(prefix)] == list(prefix)]

    def finished(self, pid: int) -> dict | None:
        return next((event for event in self.events() if event["event"] == "end"
                     and event["pid"] == pid), None)


@dataclasses.dataclass(eq=False)
class Exchange:
    index: int
    method: str
    path: str
    query: str
    headers: dict
    at: float
    raw: bytes = b""
    body: object = None
    status: int = 0
    response: object = None
    delivered: tuple = ()
    reply_headers: dict = dataclasses.field(default_factory=dict)


@dataclasses.dataclass
class Rule:
    method: str
    path: str
    status: int
    when: object = None
    times: int | None = None
    body: object = None
    headers: dict | None = None
    used: int = 0

    def matches(self, exchange: Exchange) -> bool:
        return (exchange.method == self.method and re.fullmatch(self.path, exchange.path) is not None
                and (self.times is None or self.used < self.times)
                and (self.when is None or self.when(exchange)))


class FakeRelay:
    """A loopback relay for one node credential that enforces the HTTP rules a
    connector must meet in front of the real relay and records all traffic.

    Breaking a rule (a compressed or non-JSON POST, one without Content-Length,
    one above the 1 MiB /v1/ limit, an invalid body, duplicate query
    parameters, an unknown endpoint) is answered as the relay or nginx would and
    also recorded in `violations`, which BlackBoxCase fails on. Requests are
    queued as phones submit them and handed out one per long poll in the
    relay's batch envelope; results follow the relay's claim rules. As the
    relay does, it answers a peer heartbeat with 404 when its route is not in
    the last heartbeat manifest it accepted, and with 409 when that route is
    offline or names another machine, or when that heartbeat's snapshot did
    not offer `gateway_one_hop` (the relay keeps such routes unguarded).
    """

    def __init__(self, token: str | None = None):
        self.token = token or "bb-node-token-" + secrets.token_hex(16)
        self.computer_id = str(uuid.uuid4())
        self.exchanges: list[Exchange] = []
        self.violations: list[str] = []
        self.rules: list[Rule] = []
        self.entries: dict[str, dict] = {}
        self.pending: collections.deque[str] = collections.deque()
        self.result_posts: list[tuple[str, object, int]] = []
        self.deliveries: list[tuple[str, int]] = []  # (request_id, index in exchanges)
        self.manifest: dict[str, dict] = {}  # route_id -> peer row of the last accepted heartbeat
        self.guarded = False  # whether that heartbeat's snapshot offered gateway_one_hop
        self.active_polls = 0
        self.max_polls = 2  # max_polls_per_credential
        self.url = ""
        self.changed: asyncio.Condition | None = None
        self.runner = None

    async def start(self) -> None:
        self.changed = asyncio.Condition()
        app = web.Application(client_max_size=2 * MIB)
        app.router.add_route("*", "/{tail:.*}", self.handle)
        # A poll whose client went away must not claim a request afterwards.
        self.runner = web.AppRunner(app, access_log=None, handler_cancellation=True, auto_decompress=False)
        await self.runner.setup()
        site = web.TCPSite(self.runner, "127.0.0.1", 0)
        await site.start()
        self.url = "http://127.0.0.1:" + str(site._server.sockets[0].getsockname()[1])

    async def stop(self) -> None:
        if self.runner is not None:
            await self.runner.cleanup()

    def rule(self, method: str, path: str, status: int, *, when=None, times=None, body=None, headers=None) -> Rule:
        """Answer matching requests with `status` (and `headers`) before normal handling."""
        rule = Rule(method, path, status, when, times, body, headers)
        self.rules.append(rule)
        return rule

    async def queue(self, request: dict, *, gateway_route=None, expires_at=None,
                    redeliver: bool = False) -> str:
        """Accept a phone submission. The relay would refuse one above its
        parse limits, so the harness refuses to queue it. `redeliver` queues an
        identity again, as no current relay does, to test the journal."""
        encoded = canonical(request)
        depth, values = json_limits(encoded.encode())
        if depth > JSON_DEPTH or values > JSON_VALUES or len(encoded) > REQUEST_MAX:
            raise ValueError("the relay refuses this submission")
        if (not isinstance(request, dict) or set(request) != {"request_id", "computer_id", "operation", "session", "payload"}
                or not canonical_uuid(request["request_id"]) or not isinstance(request["payload"], dict)):
            raise ValueError("not a relay submission envelope")
        request_id = request["request_id"]
        if request_id in self.entries and not redeliver:
            raise ValueError("request_id already submitted")
        delivered = json.loads(encoded)
        delivered.pop("computer_id", None)
        if gateway_route is not None:
            delivered["gateway_route"] = gateway_route
        if expires_at is not None:
            delivered["expires_at"] = expires_at
        self.entries[request_id] = {"state": "queued", "delivered": delivered, "result": None}
        self.pending.append(request_id)
        await self.notify()
        return request_id

    def state(self, request_id: str) -> str | None:
        entry = self.entries.get(request_id)
        return entry["state"] if entry else None

    def results(self, request_id: str) -> list:
        """Every accepted result upload for this identity, in order."""
        return [body for identity, body, status in self.result_posts if identity == request_id and status == 200]

    def uploads(self, request_id: str) -> list[tuple[object, int]]:
        return [(body, status) for identity, body, status in self.result_posts if identity == request_id]

    def heartbeats(self) -> list[Exchange]:
        return [exchange for exchange in self.exchanges if exchange.method == "POST"
                and exchange.path == "/v1/node/heartbeat" and exchange.status == 200]

    def peer_heartbeats(self, route_id: str | None = None) -> list[Exchange]:
        """Accepted peer heartbeats, for one route or all of them."""
        return [exchange for exchange in self.exchanges if exchange.method == "POST" and exchange.status == 200
                and re.fullmatch(r"/v1/node/peers/[^/]+/heartbeat", exchange.path)
                and (route_id is None or exchange.path == f"/v1/node/peers/{route_id}/heartbeat")]

    def delivered_since(self, request_id: str, mark: int) -> int | None:
        """Index in `exchanges` of the poll that handed this identity out in
        delivery `mark` or later (`mark` is a length of `deliveries`)."""
        return next((index for identity, index in self.deliveries[mark:] if identity == request_id), None)

    async def inject(self, delivered) -> str:
        """Hand out `delivered` exactly as given, in its own batch, although no
        relay would queue it (a non-canonical identity, a malformed envelope or
        a non-object). Returns its key in `entries`: its request_id when that
        is a new string, under which a result can then be uploaded."""
        request_id = delivered.get("request_id") if isinstance(delivered, dict) else None
        key = request_id if isinstance(request_id, str) and request_id not in self.entries else f"injected-{len(self.entries)}"
        self.entries[key] = {"state": "queued", "delivered": delivered, "result": None}
        self.pending.append(key)
        await self.notify()
        return key

    async def claimed(self, request: dict) -> None:
        """Register a submission an earlier connector process already claimed."""
        await self.queue(request, redeliver=True)
        self.pending.remove(request["request_id"])
        self.entries[request["request_id"]]["state"] = "claimed"

    async def notify(self) -> None:
        async with self.changed:
            self.changed.notify_all()

    def refuse(self, exchange: Exchange, status: int, message: str, violation: str | None = None):
        if violation:
            self.violations.append(f"{exchange.method} {exchange.path}: {violation}")
        return status, {"error": message}

    async def handle(self, request: web.Request) -> web.Response:
        exchange = Exchange(len(self.exchanges), request.method, request.path, request.query_string,
                            {key.lower(): value for key, value in request.headers.items()}, time.monotonic())
        self.exchanges.append(exchange)
        try:
            status, body = await self.dispatch(request, exchange)
        except asyncio.CancelledError:
            exchange.status = 499  # The client went away; nothing was answered.
            raise
        exchange.status, exchange.response = status, body
        if request.method == "POST" and not request.content.at_eof():
            # Discard a refused body, as the relay does, so that the client reads the refusal.
            with contextlib.suppress(asyncio.TimeoutError, OSError):
                await asyncio.wait_for(request.content.read(), 2)
        parts = exchange.path.strip("/").split("/")
        if exchange.method == "POST" and len(parts) == 5 and parts[:3] == ["v1", "node", "requests"] and parts[4] == "result":
            self.result_posts.append((parts[3], exchange.body, status))
        await self.notify()
        headers = {"Cache-Control": "no-store"}
        if status in (429, 503):
            headers["Retry-After"] = "1"
        headers.update(exchange.reply_headers)
        return web.Response(status=status, body=canonical(body).encode(),
                            content_type="application/json", headers=headers)

    async def dispatch(self, request: web.Request, exchange: Exchange):
        encoding = exchange.headers.get("content-encoding")
        if encoding is not None and encoding != "identity":
            return self.refuse(exchange, 415, "compressed requests are unsupported", "Content-Encoding " + encoding)
        if not exchange.path.startswith("/v1/"):
            return self.refuse(exchange, 404, "not found", "request outside /v1/")
        if exchange.headers.get("authorization") != "Bearer " + self.token:
            return 401, {"error": "credential not authorized"}
        params = parse_qsl(exchange.query, keep_blank_values=True)
        if len({key for key, _ in params}) != len(params):
            return self.refuse(exchange, 400, "invalid request", "duplicate query parameter")
        params = dict(params)
        if exchange.method == "POST":
            refusal = await self.read_body(request, exchange)
            if refusal is not None:
                return refusal
        for rule in self.rules:
            if rule.matches(exchange):
                rule.used += 1
                exchange.reply_headers = dict(rule.headers or {})
                return rule.status, {"error": "scripted relay refusal"} if rule.body is None else rule.body
        parts = exchange.path.strip("/").split("/")
        if exchange.method == "GET" and exchange.path == "/v1/node/requests":
            return await self.poll(request, exchange, params)
        if exchange.method == "POST" and exchange.path == "/v1/node/heartbeat":
            problem = heartbeat_problem(exchange.body)
            if problem:
                return self.refuse(exchange, 400, "invalid request", problem)
            # A heartbeat without a machine_id withdraws every route, and one
            # whose snapshot lacks gateway_one_hop leaves its routes unguarded
            # (relay store/snapshots.rs and registry.rs).
            self.manifest = {row["route_id"]: row for row in exchange.body.get("peers", [])}
            self.guarded = supports(exchange.body["snapshot"], "gateway_one_hop")
            return 200, {"ok": True}
        if exchange.method == "POST" and len(parts) == 5 and parts[:3] == ["v1", "node", "peers"] and parts[4] == "heartbeat":
            problem = peer_heartbeat_problem(parts[3], exchange.body)
            if problem:
                return self.refuse(exchange, 400, "invalid request", problem)
            route = self.manifest.get(parts[3])
            if route is None:
                return 404, {"error": "not found"}
            if not (route["online"] and self.guarded) or route["machine_id"] != exchange.body["machine_id"]:
                return 409, {"error": "request identity or route conflicts"}
            return 200, {"ok": True}
        if exchange.method == "POST" and len(parts) == 5 and parts[:3] == ["v1", "node", "requests"] and parts[4] == "result":
            return self.result(exchange, parts[3])
        return self.refuse(exchange, 404, "not found", "unknown endpoint")

    async def read_body(self, request: web.Request, exchange: Exchange):
        if exchange.headers.get("content-type", "").split(";")[0] != "application/json":
            return self.refuse(exchange, 415, "application/json required", "POST without application/json")
        length = exchange.headers.get("content-length")
        if length is None or "transfer-encoding" in exchange.headers:
            # The relay admits a body of unknown length only through its single
            # large-payload slot, which answers 429 under load.
            return self.refuse(exchange, 411, "length required", "POST without Content-Length")
        if not length.isdigit():
            return self.refuse(exchange, 400, "invalid request", "invalid Content-Length")
        if int(length) > WIRE_LIMIT:
            return self.refuse(exchange, 413, "payload exceeds limit", f"POST body of {length} bytes exceeds the 1 MiB /v1/ limit")
        exchange.raw = await request.read()
        if len(exchange.raw) != int(length):
            return self.refuse(exchange, 400, "invalid request", "body length differs from Content-Length")
        depth, values = json_limits(exchange.raw)
        if depth > JSON_DEPTH or values > JSON_VALUES:
            return self.refuse(exchange, 413, "payload exceeds limit", "body exceeds the relay JSON limits")
        try:
            exchange.body = strict_json(exchange.raw)
        except (ValueError, UnicodeError):
            return self.refuse(exchange, 400, "invalid request", "invalid JSON body")
        return None

    def claimable(self, one_hop: bool) -> str | None:
        for request_id in self.pending:
            entry = self.entries[request_id]
            routed = isinstance(entry["delivered"], dict) and "gateway_route" in entry["delivered"]
            if entry["state"] == "queued" and (one_hop or not routed):
                return request_id
        return None

    async def poll(self, request: web.Request, exchange: Exchange, params: dict):
        try:
            wait = float(params.get("wait", "0"))
            after = int(params.get("after", "0"))
        except ValueError:
            return self.refuse(exchange, 400, "invalid request", "invalid poll parameters")
        one_hop = params.get("gateway_one_hop", "0")
        if not math.isfinite(wait) or not 0 <= wait <= 25 or after < 0 or one_hop not in ("0", "1"):
            return self.refuse(exchange, 400, "invalid request", "invalid poll parameters")
        if self.active_polls >= self.max_polls:
            return 429, {"error": "relay capacity is busy"}
        self.active_polls += 1
        try:
            deadline = time.monotonic() + wait
            async with self.changed:
                while True:
                    request_id = self.claimable(one_hop == "1")
                    if request_id is not None and request.transport is not None and not request.transport.is_closing():
                        self.pending.remove(request_id)
                        entry = self.entries[request_id]
                        entry["state"] = "claimed"
                        exchange.delivered = (request_id,)
                        self.deliveries.append((request_id, exchange.index))
                        return 200, {"requests": [entry["delivered"]]}
                    remaining = deadline - time.monotonic()
                    if remaining <= 0:
                        return 200, {"requests": []}
                    with contextlib.suppress(asyncio.TimeoutError):
                        await asyncio.wait_for(self.changed.wait(), remaining)
        finally:
            self.active_polls -= 1

    def result(self, exchange: Exchange, request_id: str):
        body = exchange.body
        if (not isinstance(body, dict) or set(body) != {"state", "result", "error"}
                or body["state"] not in ("completed", "failed", "uncertain")
                or not (body["error"] is None or isinstance(body["error"], str) and len(body["error"]) <= 4096)):
            return self.refuse(exchange, 400, "invalid request", "invalid result receipt")
        if len(canonical({"request_id": request_id, **body})) > MIB:
            return self.refuse(exchange, 413, "payload exceeds limit", "result receipt exceeds 1 MiB")
        entry = self.entries.get(request_id)
        if entry is None:
            return 404, {"error": "not found"}
        if entry["state"] == "claimed":
            entry["state"], entry["result"] = body["state"], body
            return 200, {"ok": True}
        if entry["result"] is not None and canonical(entry["result"]) == canonical(body):
            return 200, {"ok": True}
        return 409, {"error": "request identity or route conflicts"}


def snapshot_problem(snapshot) -> str | None:
    if not isinstance(snapshot, dict) or not isinstance(snapshot.get("sessions"), list) or len(snapshot["sessions"]) > 5000:
        return "invalid snapshot"
    for row in snapshot["sessions"]:
        if not isinstance(row, dict) or ("name" in row and not text_field(row["name"], 1024)):
            return "invalid snapshot session"
    return None


def supports(snapshot, feature: str) -> bool:
    """The relay's `protocol::supports` for a snapshot's features."""
    capabilities = snapshot.get("mobile_capabilities") if isinstance(snapshot, dict) else None
    return (isinstance(capabilities, dict) and type(capabilities.get("protocol_version")) is int
            and capabilities["protocol_version"] == 1
            and isinstance(capabilities.get("features"), list) and feature in capabilities["features"])


def presence_problem(body) -> str | None:
    if "presence" in body and not (body["presence"] is None or isinstance(body["presence"], dict) and len(body["presence"]) <= 16):
        return "invalid presence"
    return None


def heartbeat_problem(body) -> str | None:
    if not isinstance(body, dict) or "snapshot" not in body or set(body) - {"snapshot", "machine_id", "peers", "presence"}:
        return "invalid heartbeat fields"
    problem = snapshot_problem(body["snapshot"]) or presence_problem(body)
    if problem:
        return problem
    if "machine_id" in body and not canonical_uuid(body["machine_id"]):
        return "invalid heartbeat machine_id"
    if "peers" in body:
        peers, seen = body["peers"], set()
        if not isinstance(peers, list) or len(peers) > 32 or (peers and "machine_id" not in body):
            return "invalid heartbeat peers"
        for row in peers:
            if (not isinstance(row, dict) or set(row) != {"route_id", "machine_id", "name", "online"}
                    or not canonical_uuid(row["route_id"]) or not canonical_uuid(row["machine_id"])
                    or not text_field(row["name"], 128) or type(row["online"]) is not bool or row["route_id"] in seen):
                return "invalid heartbeat peer"
            seen.add(row["route_id"])
    return None


def peer_heartbeat_problem(route_id: str, body) -> str | None:
    if not canonical_uuid(route_id):
        return "invalid peer route"
    if not isinstance(body, dict) or set(body) - {"machine_id", "snapshot", "presence"} or not {"machine_id", "snapshot"} <= set(body):
        return "invalid peer heartbeat fields"
    if not canonical_uuid(body["machine_id"]):
        return "invalid peer machine_id"
    return snapshot_problem(body["snapshot"]) or presence_problem(body)


def foreign_connections(pid: int) -> list[str]:
    """Linux: remote addresses of this process's sockets that are not loopback."""
    try:
        inodes = set()
        for fd in os.listdir(f"/proc/{pid}/fd"):
            with contextlib.suppress(OSError):
                link = os.readlink(f"/proc/{pid}/fd/{fd}")
                if link.startswith("socket:["):
                    inodes.add(link[8:-1])
        found = []
        for table in ("tcp", "tcp6", "udp", "udp6"):
            with open(f"/proc/{pid}/net/{table}") as rows:
                for line in rows.readlines()[1:]:
                    fields = line.split()
                    if len(fields) > 9 and fields[9] in inodes:
                        address = remote_address(fields[2])
                        if address is not None:
                            found.append(f"{table} {address}")
        return found
    except OSError:
        return []  # The process has exited, or this is not Linux.


def remote_address(field: str) -> str | None:
    host, port = field.split(":")
    raw = bytes.fromhex(host)
    # /proc stores each 32-bit word in host (little-endian) order.
    raw = b"".join(raw[i:i + 4][::-1] for i in range(0, len(raw), 4))
    address = ipaddress.ip_address(raw)
    if isinstance(address, ipaddress.IPv6Address) and address.ipv4_mapped:
        address = address.ipv4_mapped
    if address.is_loopback or (address.is_unspecified and int(port, 16) == 0):
        return None
    return f"{address}:{int(port, 16)}"


class Journal:
    """Read-only views of a connector journal, safe while the connector runs."""

    def __init__(self, state_dir: Path):
        self.path = state_dir / "journal.sqlite3"

    def query(self, sql: str, args=()) -> list[sqlite3.Row]:
        db = sqlite3.connect(self.path.as_uri() + "?mode=ro", uri=True, timeout=5)
        try:
            db.row_factory = sqlite3.Row
            return db.execute(sql, args).fetchall()
        finally:
            db.close()

    def rows(self) -> list[dict]:
        return [dict(row) for row in self.query("SELECT * FROM requests ORDER BY created_at, request_id")]

    def row(self, request_id: str) -> dict | None:
        found = self.query("SELECT * FROM requests WHERE request_id=?", (request_id,))
        return dict(found[0]) if found else None

    def metadata(self) -> dict:
        return {row["key"]: row["value"] for row in self.query("SELECT key, value FROM metadata")}

    def columns(self) -> list[str]:
        return [row["name"] for row in self.query("PRAGMA table_info(requests)")]

    def indexes(self) -> set[str]:
        return {row["name"] for row in self.query(
            "SELECT name FROM sqlite_master WHERE type='index' AND tbl_name='requests' AND sql IS NOT NULL")}

    def objects(self) -> set[tuple[str, str]]:
        return {(row["type"], row["name"]) for row in self.query("SELECT type, name FROM sqlite_master")}

    def pragma(self, name: str):
        return self.query(f"PRAGMA {name}")[0][0]


def journal_bytes(state_dir: Path) -> bytes:
    """Every journal file's bytes, for scans for private text. A rollback
    journal exists only during a transaction and may vanish meanwhile."""
    found = []
    for path in sorted(state_dir.iterdir()):
        if path.name.startswith("journal"):
            with contextlib.suppress(FileNotFoundError):
                found.append(path.read_bytes())
    return b"".join(found)


@contextlib.contextmanager
def stopped_journal(state_dir: Path):
    """A writable journal connection, only while no connector holds it. The
    exclusive lock also keeps a connector from starting meanwhile."""
    lock = os.open(state_dir / "connector.lock", os.O_RDWR | os.O_NOFOLLOW)
    try:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError:
            raise AssertionError("seed or age a journal only while its connector is stopped") from None
        db = sqlite3.connect(state_dir / "journal.sqlite3")
        try:
            with db:
                yield db
        finally:
            db.close()
    finally:
        os.close(lock)


def recover_stopped_journal(state_dir: Path) -> None:
    """Allow normal hot-journal recovery only after the connector is stopped.

    A SIGKILL may leave a rollback journal that a mode=ro observer cannot
    recover. The existing exclusive connector lock guards this one writable
    connection; its only SQL is a bounded read, with no migration or pruning.
    """
    if not stat.S_ISREG((state_dir / "journal.sqlite3").lstat().st_mode):
        raise AssertionError("recover only an existing regular journal")
    with stopped_journal(state_dir) as db:
        db.execute("SELECT 1 FROM sqlite_master LIMIT 1").fetchone()


class ConnectorProcess:
    def __init__(self, case: BlackBoxCase, args: list[str], env: dict, command: list[str]):
        self.case, self.args, self.env, self.command = case, args, env, command
        self.index = len(case.processes)
        self.log_path = case.scratch / f"connector-{self.index}.log"
        self.process: subprocess.Popen | None = None
        self.guard: asyncio.Task | None = None

    def start(self) -> ConnectorProcess:
        with open(self.log_path, "wb") as log:
            self.process = subprocess.Popen(
                [*self.command, *self.args], cwd=self.case.scratch, env=self.env,
                stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
        self.case.hgs.parent(self.process.pid)
        if sys.platform.startswith("linux"):
            self.guard = asyncio.get_running_loop().create_task(self.watch_sockets())
        return self

    async def watch_sockets(self) -> None:
        while self.process.poll() is None:
            for finding in foreign_connections(self.process.pid):
                finding = f"connector {self.index} opened a non-loopback socket: {finding}"
                if finding not in self.case.findings:
                    self.case.findings.append(finding)
            await asyncio.sleep(0.1)

    @property
    def pid(self) -> int:
        return self.process.pid

    @property
    def returncode(self) -> int | None:
        return self.process.poll()

    def output(self) -> str:
        return self.log_path.read_text(errors="replace") if self.log_path.exists() else ""

    def signal(self, number: int) -> None:
        """Signal the connector process alone, as systemd's KillMode=process does."""
        os.kill(self.process.pid, number)

    async def wait_exit(self, timeout: float = 10) -> int:
        deadline = time.monotonic() + timeout
        while self.process.poll() is None:
            if time.monotonic() > deadline:
                raise AssertionError(f"connector {self.index} did not exit within {timeout} s:\n{self.output()[-2000:]}")
            await asyncio.sleep(0.02)
        return self.process.returncode

    def stop(self) -> None:
        """SIGKILL the connector and anything it left in its own session."""
        if self.process is None:
            return
        with contextlib.suppress(ProcessLookupError, PermissionError):
            os.killpg(self.process.pid, signal.SIGKILL)
        self.process.wait()
        if self.guard is not None:
            self.guard.cancel()


class BlackBoxCase(unittest.IsolatedAsyncioTestCase):
    """A FakeRelay, a scripted hgs and scratch directories for each test."""

    # Harness self-tests run whichever connector is selected.
    tests_connector = True

    async def asyncSetUp(self):
        if self.tests_connector and IMPLEMENTATION != "python" and type(self).__module__ not in ported_modules():
            self.skipTest(f"{type(self).__module__} is not ported to {IMPLEMENTATION} (ported.json)")
        if TRACE_DIR:
            # Both runs of a traced case then use the same identities.
            identities = mock.patch.object(uuid, "uuid4", differential.seeded_uuid4(self.id()))
            identities.start()
            self.addCleanup(identities.stop)
        self.scratch = Path(tempfile.mkdtemp(prefix="zerus-bb-")).resolve()
        self.addCleanup(shutil.rmtree, self.scratch, ignore_errors=True)
        for name in ("home", "tmp", "hgs-config", "hgs-state"):
            (self.scratch / name).mkdir(mode=0o700)
        self.home = self.scratch / "home"
        self.state_dir = self.scratch / "state"
        self.journal = Journal(self.state_dir)
        self.findings: list[str] = []
        self.processes: list[ConnectorProcess] = []
        self.relay = FakeRelay()
        await self.relay.start()
        self.hgs = ScriptedHgs(self.scratch / "hgs")
        self.hgs.script(default_rules())

    async def asyncTearDown(self):
        for process in self.processes:
            process.stop()
        await self.relay.stop()
        if TRACE_DIR:
            differential.record(self, TRACE_DIR)
        findings = self.findings + self.relay.violations
        for process in self.processes:
            if self.relay.token in process.output():
                findings.append(f"connector {process.index} wrote its node token to its log")
        calls = self.hgs.root / "calls.jsonl"
        if calls.exists() and self.relay.token in calls.read_text(errors="replace"):
            findings.append("native hgs received the node token in its argv, stdin or environment")
        if findings:
            self.fail("Black-box contract findings:\n" + "\n".join(findings))

    def config(self, **changes) -> dict:
        """A valid connector configuration; a None value removes that key."""
        config = {"server_url": self.relay.url, "node_token": self.relay.token,
                  "state_dir": str(self.state_dir), "poll_interval": 1}
        for key, value in changes.items():
            if value is None:
                config.pop(key, None)
            else:
                config[key] = value
        return config

    def environment(self, extra: dict | None = None, command: list[str] | None = None) -> dict:
        home = self.home
        env = {"HOME": str(home), "PATH": os.pathsep.join([str(self.hgs.bin), "/usr/bin", "/bin"]),
               "TMPDIR": str(self.scratch / "tmp"), "XDG_CONFIG_HOME": str(home / ".config"),
               "XDG_STATE_HOME": str(home / ".local/state"), "XDG_DATA_HOME": str(home / ".local/share"),
               "XDG_CACHE_HOME": str(home / ".cache"), "HGS_CONFIG_DIR": str(self.scratch / "hgs-config"),
               "HGS_STATE_DIR": str(self.scratch / "hgs-state")}
        if (command or connector_command()) == PYTHON_COMMAND:
            env.update(PYTHONPATH=str(SERVICE), PYTHONDONTWRITEBYTECODE="1")
        env.update(extra or {})
        return env

    def write_config(self, config: dict | None = None, *, text: str | None = None) -> Path:
        if text is None:
            text = json.dumps(config)
        try:
            parsed = json.loads(text)
        except ValueError:
            parsed = None
        for key in ("server_url", "identity_url") if isinstance(parsed, dict) else ():
            host = urlsplit(parsed[key]).hostname if isinstance(parsed.get(key), str) else None
            if host is not None and host not in ("127.0.0.1", "localhost", "::1") and not host.endswith(".invalid"):
                raise AssertionError("black-box connectors may name only a loopback relay")
        path = self.scratch / f"connector-{len(self.processes)}.json"
        path.write_text(text)
        path.chmod(0o600)
        return path

    async def launch(self, config: dict | None = None, *, text: str | None = None, args=("--allow-insecure-localhost",),
                     env: dict | None = None, ready: bool = True, command: list[str] | None = None) -> ConnectorProcess:
        """Start a connector (by default the one under test); with `ready`,
        wait until it heartbeats and polls."""
        path = self.write_config(self.config() if config is None and text is None else config, text=text)
        process = self.spawn(["--config", str(path), *args], env, command)
        if ready:
            await self.ready(process)
        return process

    def spawn(self, args: list[str], env: dict | None = None, command: list[str] | None = None) -> ConnectorProcess:
        """Start a connector with exactly these arguments."""
        command = command or connector_command()
        process = ConnectorProcess(self, list(args), self.environment(env, command), command)
        self.processes.append(process)
        return process.start()

    async def ready(self, process: ConnectorProcess, timeout: float = 20) -> None:
        beats = len(self.relay.heartbeats())
        await self.until(lambda: len(self.relay.heartbeats()) > beats and self.relay.active_polls > 0,
                         timeout, "the connector to heartbeat and poll", process)

    async def until(self, predicate, timeout: float = 10, what: str = "the condition",
                    process: ConnectorProcess | None = None):
        deadline = time.monotonic() + timeout
        while True:
            value = predicate()
            if value:
                return value
            if time.monotonic() > deadline:
                detail = "" if process is None else "\n" + process.output()[-3000:]
                raise AssertionError(f"timed out after {timeout} s waiting for {what}{detail}")
            await asyncio.sleep(0.02)

    async def settle(self, request_id: str, mark: int, process: ConnectorProcess | None = None) -> None:
        """Wait until the connector has finished the batch that carried this
        identity in delivery `mark` or later: it polls again only after
        executing and flushing a batch."""
        def delivered():
            index = self.relay.delivered_since(request_id, mark)
            return None if index is None else [index]
        index = (await self.until(delivered, 10, "the request's delivery", process))[0]
        await self.until(lambda: any(exchange.method == "GET" and exchange.path == "/v1/node/requests"
                                     for exchange in self.relay.exchanges[index + 1:]),
                         10, "the next poll", process)

    def request(self, operation: str = "send", *, request_id: str | None = None, session: str = SESSION,
                **payload) -> dict:
        """A phone submission; mutations carry the exact request/run/conversation identity."""
        request_id = request_id or str(uuid.uuid4())
        if operation != "inspect":
            payload = {"request_id": request_id, "expected_run_id": RUN_ID,
                       "expected_conversation_id": CONVERSATION_ID, **payload}
        return {"request_id": request_id, "computer_id": self.relay.computer_id,
                "operation": operation, "session": session, "payload": payload}

    async def submit(self, request: dict, process: ConnectorProcess | None = None, *,
                     timeout: float = 10, **options) -> dict:
        """Queue a submission and return the first result the connector uploads."""
        before = len(self.relay.results(request["request_id"]))
        await self.relay.queue(request, **options)
        return (await self.until(lambda: self.relay.results(request["request_id"])[before:],
                                 timeout, "a result upload", process))[0]
