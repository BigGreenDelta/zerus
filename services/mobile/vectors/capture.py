#!/usr/bin/env python3
"""Capture language-neutral golden vectors from the Python mobile connector.

    python3 services/mobile/vectors/capture.py                   rewrite the vector files
    python3 services/mobile/vectors/capture.py --check           recompute and compare byte for byte
    python3 services/mobile/vectors/capture.py --refresh-oracle  also recompute the relay-oracle column

The capture runs offline and is deterministic: a fixed clock, synthetic tokens,
reserved example domains, a scratch state directory, no network connection and
no native process. The relay-oracle column of validation/*.jsonl is frozen data;
only --refresh-oracle recomputes it, with the reference relay's validation as
services/mobile/tests/relay_oracle.py keeps it. The Rust relay's column is
copied; its tests/vectors.rs recomputes it. README.md describes each file.
"""
from __future__ import annotations

import argparse
import ast
import asyncio
import base64
import contextlib
import copy
import errno
import hashlib
import io
import json
import logging
import os
from pathlib import Path
import sqlite3
import stat
import sys
import tempfile
import types
import uuid
from unittest import mock

HERE = Path(__file__).resolve().parent
if __name__ == "__main__":
    sys.path[:0] = [str(HERE.parent), str(HERE.parent / "tests")]

from zerus_mobile import (analytics, accounts, attachment_reads, attachments, connector, context, fleet, file_reads, archives,  # noqa: E402
                          history, launch, managed_relay, projects, speech, speech_config, terminal,
                          voice, worktrees, session_projects)
from zerus_mobile.connector import Connector, ConnectorError, Journal  # noqa: E402

FILE_LIMIT = 64 * 1024
EPOCH = 1760000000.0  # Every clock in the vectors is relative to this fixed instant.
MONOTONIC = 5000.0
NAMESPACE = uuid.uuid5(uuid.NAMESPACE_URL, "https://vectors.example.invalid/zerus-mobile")


def ident(label: str) -> str:
    """A canonical, deterministic UUID for one named vector identity."""
    return str(uuid.uuid5(NAMESPACE, label))


RELAY = "https://relay.example.test"
TOKEN = "fixture-node-token"
BINDING = hashlib.sha256((RELAY + "\0" + TOKEN).encode()).hexdigest()
SESSION = "codex/example/session"
DSH_SESSION = "dsh/example/parent"
RUN = "fixture-run"
CONVERSATION = "fixture-conversation"
COMPUTER = ident("computer")
MACHINE = ident("machine")
ARCHIVE = ident("archive")
NIL = "00000000-0000-0000-0000-000000000000"
HASH_A = "a" * 64
HASH_B = "b" * 64
AUDIO = {"format": "pcm_s16le", "sample_rate": 16000, "channels": 1, "data_base64": "AAAAAA=="}
SEALED = {"key_id": "a" * 64, "algorithm": speech_config.ALGORITHM,
          "ephemeral_public_key": base64.b64encode(bytes(91)).decode(),
          "nonce": base64.b64encode(bytes(12)).decode(),
          "ciphertext": base64.b64encode(bytes(40)).decode()}
CONNECTOR_MODULES = ("connector", "context", "attachments", "attachment_reads", "file_reads", "archives", "history", "launch",
                     "worktrees", "speech", "speech_config", "voice", "recovery", "terminal", "fleet",
                     "projects", "accounts", "managed_relay", "session_projects")
# AttachmentViewCache.failure() shows UPDATE_REQUIRED for a receipt error
# containing either text, and otherwise the first 200 characters of the error.
ANDROID_UPDATE_REQUIRED = ("unavailable", "unsupported operation")
ANDROID_ERROR_PREFIX = 200


def compact(value) -> str:
    return json.dumps(value, ensure_ascii=True, sort_keys=True, separators=(",", ":"))


def jsonl(records) -> bytes:
    return "".join(compact(record) + "\n" for record in records).encode()


def document(value: dict) -> bytes:
    """JSON with one line per top-level member, and per item of a top-level list or object."""
    def block(item, nested):
        if isinstance(item, dict) and item:
            lines = [" " * (nested + 1) + compact(key) + ": " + (compact(member) if nested else block(member, 1))
                     for key, member in sorted(item.items())]
        elif isinstance(item, list) and item:
            lines = [" " * (nested + 1) + compact(member) for member in item]
        else:
            return compact(item)
        brackets = "{}" if isinstance(item, dict) else "[]"
        return brackets[0] + "\n" + ",\n".join(lines) + "\n" + " " * nested + brackets[1]
    return (block(value, 0) + "\n").encode()


def android(message: str) -> dict:
    """Flags only when the phone treats the text specially; absent otherwise."""
    flags = {"update_required": any(text in message for text in ANDROID_UPDATE_REQUIRED),
             "truncated": len(message) > ANDROID_ERROR_PREFIX}
    return {key: True for key, value in flags.items() if value}


def flagged(record: dict, message) -> dict:
    if isinstance(message, str) and android(message):
        record["android"] = android(message)
    return record


def raised(error: BaseException) -> dict:
    if isinstance(error, OSError):  # Its text names local paths; the errno is the contract.
        return {"raised": "OSError", "errno": errno.errorcode.get(error.errno)}
    if isinstance(error, json.JSONDecodeError):  # Parser wording is Python's own.
        return {"raised": "JSONDecodeError"}
    return {"error": str(error), "raised": "ConnectorError" if isinstance(error, ConnectorError) else type(error).__name__}


class CaptureError(Exception):
    """The vectors cannot be produced from the current code and frozen data."""


class Clock:
    """Replaces the time module inside zerus_mobile.connector only."""

    def __init__(self, now: float = EPOCH):
        self.now, self.mono = now, MONOTONIC

    def time(self) -> float:
        return self.now

    def monotonic(self) -> float:
        return self.mono


@contextlib.contextmanager
def fixed_clock(now: float = EPOCH):
    clock = Clock(now)
    with mock.patch.object(connector, "time", clock):
        yield clock


@contextlib.contextmanager
def scratch():
    with tempfile.TemporaryDirectory(prefix="zerus-vectors-") as root:
        yield Path(root).resolve()


def connect_ro(path: Path) -> sqlite3.Connection:
    return sqlite3.connect(path.as_uri() + "?mode=ro", uri=True)


class LogRecords(logging.Handler):
    def __init__(self):
        super().__init__()
        self.messages = []

    def emit(self, record):
        self.messages.append(record.getMessage())


@contextlib.contextmanager
def connector_log(handler: logging.Handler):
    """The connector logs only to handler, independent of logging state left by other tests."""
    log = logging.getLogger("zerus.connector")
    disabled, level, propagate = logging.root.manager.disable, log.level, log.propagate
    log.addHandler(handler)
    logging.disable(logging.NOTSET)
    log.setLevel(logging.INFO)
    log.propagate = False
    try:
        yield
    finally:
        log.removeHandler(handler)
        log.setLevel(level)
        log.propagate = propagate
        logging.disable(disabled)


# ---------------------------------------------------------------------------
# Operation tables and the help-marker capability map.

HELP_LINES = (
    ("analytics", analytics.HELP_MARKER),
    ("lifecycle", "hgs session-action <session> --json"),
    ("terminal", "hgs terminal <session> --json"),
    ("recovery", "hgs recovery action --scoped-json"),
    ("history", "hgs history <session> --json"),
    ("hook_health", "hooks-health-v1"),
    ("session_project_version", session_projects.HELP_MARKER),
    ("session_project_read", "hgs swarm session-project --json"),
    ("session_project_assign", "hgs swarm assign-session --json"),
    ("launch", "hgs <agent> <directory> --new --launch-id ID"),
    ("project_launch", "hgs swarm assign-launch --json"),
    ("worktree", "Mobile worktree ABI: worktrees-v1"),
    ("directory_create", "hgs " + launch.DIRECTORY_CREATE_HELP),
    ("read", "hgs read <session> --json"),
    ("session_notifications", "session-notifications-v1"),
    ("preference", "hgs swarm preference set mobile_delivery"),
    ("attachment_read", "hgs " + attachment_reads.HELP_MARKER),
    ("file_read", "hgs " + file_reads.HELP_MARKER),
    ("archive_list", "hgs " + archives.HELP_MARKER),
    ("speech", "hgs " + speech.HELP_MARKER),
    ("voice", "hgs " + voice.HELP_MARKER),
    ("speech_config", speech_config.HELP_MARKER),
    ("peer_inventory", "hgs swarm mobile-peers --json"),
    ("peer_command", "hgs swarm mobile-peer --json"),
    ("clear_context_dismiss", context.CLEAR_CONTEXT_DISMISS_HELP),
)


NATIVE_DEFAULTS = {"timeout": None, "json_output": True, "max_output_bytes": None, "failure_json": False}


class Native:
    """Scripted answers keyed by exact argv; stands in for Connector._native.

    It records the argv after Connector.native() applied its prefixes, the
    stdin payload and every option that differs from its default.
    """

    def __init__(self, answers=None):
        self.answers = dict(answers or {})
        self.calls = []

    async def __call__(self, argv, payload=None, **options):
        call = {"argv": list(argv)}
        if payload is not None:
            call["payload"] = copy.deepcopy(payload)
        call["options"] = {key: value for key, value in options.items() if value != NATIVE_DEFAULTS[key]}
        self.calls.append(call)
        answer = self.answers.get(tuple(argv), ConnectorError("native command failed"))
        if isinstance(answer, list):
            answer = answer.pop(0) if len(answer) > 1 else answer[0]
        if isinstance(answer, BaseException):
            raise answer
        return copy.deepcopy(answer)


def make_connector(root: Path, **config) -> Connector:
    values = {"server_url": RELAY, "node_token": TOKEN, "state_dir": str(root / "state"),
              "hgs_path": str(root / "missing-hgs")}
    values.update(config)
    return Connector(values)


LISTING = {
    "host": "local-native", "local_host": "local-native", "version": "0.0.0-fixture",
    "metrics": {"sessions": 3}, "preferences": {"mobile_delivery": "away"},
    "desktop_presence": {"state": "active", "observed_at": EPOCH - 10},
    "peers": [{"alias": "private-peer.example.test", "machine_id": ident("peer")}],
    "ended_launches": [{"launch_id": ident("ended-launch"), "screen": ["private failed launch screen"]}],
    "sessions": [
        {"name": SESSION, "cmd": "codex", "tracked": True, "runtime_state": "live",
         "process_state": "running", "state": "idle", "run_id": RUN, "conversation_id": CONVERSATION,
         "process_work": {"active": True, "jobs": 1, "sampled_at": EPOCH - 1}},
        {"name": "codex/example/paused", "cmd": "codex", "tracked": True, "runtime_state": "live",
         "process_state": "running", "state": "paused", "run_id": "paused-run", "conversation_id": "paused-conversation"},
        {"name": "claude/example/archived", "cmd": "claude", "state": "archived", "archive_id": ARCHIVE,
         "run_id": "archived-run"},
        {"name": "codex/@alias", "cmd": "codex", "tracked": True, "runtime_state": "live",
         "process_state": "running", "run_id": "alias-run", "conversation_id": "alias-conversation"},
    ],
}
SWARM = {"schema": 1, "initialized": True, "node_id": "native-local", "swarm_id": "native-shared",
         "machines": [{"id": "native-local", "connection": "local-native"}],
         "peers": {"private-peer": {"private": "must not leave"}},
         "organization": {"default_project": "general", "projects": [
             {"id": "logical", "name": "Logical project", "color": "#123456", "accessible": True,
              "folders": [{"id": "f-local", "name": "Root", "path": "/example/root", "machine_id": "native-local",
                           "machine_name": "Local"},
                          {"id": "f-remote", "name": "Remote", "path": "/private/remote", "machine_id": "native-remote",
                           "machine_name": "Remote"}],
              "sessions": ["local-native\n" + SESSION, "local-native\narchive\n" + ARCHIVE, "remote-native\ncodex/remote/tag"]},
             {"id": "general", "name": "General", "color": "#8d9baa", "accessible": True, "folders": [], "sessions": []}]}}
ACCOUNTS = {"profiles": [
    {"id": "native-codex", "provider": "codex", "label": "Synthetic Codex", "installed": False, "native": True,
     "is_default": True, "home": "/example/private-home", "auth_revision": "rev-1",
     "account_status": {"status": "ok", "auth_status": "signed_in", "checked_at": EPOCH - 30,
                        "identity": {"email": "person@example.test", "plan": "fixture"},
                        "windows": [{"id": "5h", "label": "5 hours", "used_percent": 12.5, "window_minutes": 300,
                                     "resets_at": "2025-10-09T12:00:00Z"}],
                        "error": "private provider error"}},
    {"id": "native-opencode", "provider": "opencode", "label": "OpenCode", "installed": False,
     "model_options": [{"value": "ollama/qwen3-coder:30b", "label": "Local coding model", "api_key": "private"},
                       {"value": "no-provider", "label": "Refused"}]},
]}
# The launch projection skips a provider it does not know; the account cache refuses the catalog.
LAUNCH_ACCOUNTS = {"profiles": ACCOUNTS["profiles"] + [{"id": "native-shell", "provider": "shell", "label": "Skipped provider",
                                                        "installed": True}]}


def inspection():
    """An attention inspection: one open question, one optional Codex async question, one optional
    marker of the wrong type, one already answered and one of another run."""
    return {"name": SESSION, "run_id": RUN, "conversation_id": CONVERSATION, "runtime_state": "live",
            "process_state": "running", "pending_questions": [
                {"run_id": RUN, "conversation_id": CONVERSATION, "question_id": "q-open", "question_hash": "h-open"},
                {"run_id": RUN, "conversation_id": CONVERSATION, "question_id": "codex-async:call-optional:0",
                 "question_hash": "h-optional", "source": "codex_async", "optional": True, "can_skip": True},
                {"run_id": RUN, "conversation_id": CONVERSATION, "question_id": "q-loose", "question_hash": "h-loose",
                 "source": "codex_async", "optional": 1},
                {"run_id": RUN, "conversation_id": CONVERSATION, "question_id": "q-done", "question_hash": "h-done",
                 "answer_delivery": {"status": "submitted"}},
                {"run_id": "other-run", "conversation_id": CONVERSATION, "question_id": "q-other", "question_hash": "h"}]}


def help_text(skip=()) -> str:
    return "\n".join(line for name, line in HELP_LINES if name not in skip) + "\n"


PAGED_LISTING = {**LISTING,"archive_revision":"0"*64,"archive_paging":True,"sessions":[r for r in LISTING["sessions"] if r.get("state")!="archived"]}
EMPTY_ARCHIVES = {"version":1,"sessions":[],"next_cursor":"","total":0,"from":0,"until":253402300800,"query":""}


def snapshot_native(help_answer) -> Native:
    return Native({("ls", "--json", "--local"): LISTING, ("ls", "--json", "--local", "--no-archives"): PAGED_LISTING, ("swarm", "get"): SWARM, ("account", "ls"): ACCOUNTS,
                   ("archives","--json"): EMPTY_ARCHIVES,
                   ("inspect", SESSION): inspection(), ("--help",): help_answer})


async def snapshot_runs(root: Path) -> dict:
    """Run the real Connector.snapshot against scripted native answers."""
    with fixed_clock():
        node = make_connector(root)
    try:
        # One account catalog read, however slowly the runs below progress.
        node.accounts.wall, node.accounts.clock = (lambda: EPOCH), (lambda: MONOTONIC)
        results = {}

        async def run(label, help_answer, *, peers_discovered=False):
            node._native = snapshot_native(help_answer)
            node.capabilities_next_poll = 0
            node.fleet.local_id = MACHINE if peers_discovered else None
            with fixed_clock():
                snapshot = await node.snapshot()
            results[label] = {"snapshot": snapshot, "presence": node.presence,
                              "heartbeat": {**node.fleet.heartbeat(snapshot), "presence": node.presence},
                              "calls": node._native.calls}

        await run("all", help_text(), peers_discovered=True)
        await run("none", "hgs [OPTIONS]\n", peers_discovered=True)
        await run("probe_failed", ConnectorError("native command timed out"), peers_discovered=True)
        for name, _ in HELP_LINES:
            await run("without:" + name, help_text(skip=(name,)), peers_discovered=True)
        await run("all_undiscovered", help_text())
        return results
    finally:
        await node.accounts.close()
        node.journal.close()


def capability_map(runs: dict) -> dict:
    full = runs["all"]["snapshot"]["mobile_capabilities"]
    markers = []
    for name, line in HELP_LINES:
        caps = runs["without:" + name]["snapshot"]["mobile_capabilities"]
        markers.append({"name": name, "help_line": line,
                        "operations": sorted(set(full["operations"]) - set(caps["operations"])),
                        "features": sorted(set(full["features"]) - set(caps["features"])),
                        "reasons": {key: value for key, value in caps["reasons"].items() if key not in full["reasons"]}})
    return {"help_text_all": help_text(), "all": full,
            "none": runs["none"]["snapshot"]["mobile_capabilities"],
            "probe_failed": runs["probe_failed"]["snapshot"]["mobile_capabilities"],
            "without_peer_discovery": runs["all_undiscovered"]["snapshot"]["mobile_capabilities"],
            "markers": markers}


async def legacy_snapshot_vector(root: Path) -> bytes:
    """Current Python's full legacy-native heartbeat, separate from paging."""
    help_answer = help_text(skip=("archive_list",))
    with fixed_clock():
        node = make_connector(root)
    try:
        node.accounts.wall, node.accounts.clock = (lambda: EPOCH), (lambda: MONOTONIC)
        node._native = snapshot_native(help_answer)
        node.fleet.local_id = MACHINE
        with fixed_clock():
            snapshot = await node.snapshot()
        return document({"kind": "snapshot", "id": "heartbeat-legacy", "native": {
            "ls --json --local": LISTING, "swarm get": SWARM, "account ls": ACCOUNTS,
            "inspect " + SESSION: inspection(), "--help": help_answer},
            "peers_discovered": True, "expect": {
                "heartbeat": {**node.fleet.heartbeat(snapshot), "presence": node.presence},
                "calls": sorted(node._native.calls, key=compact)}})
    finally:
        await node.accounts.close()
        node.journal.close()


def operation_tables(runs: dict) -> bytes:
    sets = {
        "OPERATIONS": context.OPERATIONS, "READ_OPERATIONS": context.READ_OPERATIONS,
        "MUTATIONS": connector.MUTATIONS, "LAUNCH_OPERATIONS": context.LAUNCH_OPERATIONS,
        "LIFECYCLE_OPERATIONS": context.LIFECYCLE_OPERATIONS, "TERMINAL_OPERATIONS": context.TERMINAL_OPERATIONS,
        "COMPUTER_OPERATIONS": context.COMPUTER_OPERATIONS, "ATTACHMENT_OPERATIONS": context.ATTACHMENT_OPERATIONS,
        "CAPABILITY_OPERATIONS": context.CAPABILITY_OPERATIONS, "FEATURES": context.FEATURES,
        "CONTEXT_KEYS": connector.CONTEXT_KEYS, "IDENTITY_FIELDS": context.IDENTITY_FIELDS,
        "MARK_READ_FIELDS": context.MARK_READ_FIELDS, "attachments.READ_FIELDS": attachments.READ_FIELDS,
        "speech.OPERATIONS": speech.OPERATIONS, "speech.READS": speech.READS,
        "speech_config.OPERATIONS": speech_config.OPERATIONS, "speech_config.READS": speech_config.READS,
        "speech_config.MUTATIONS": speech_config.MUTATIONS, "voice.OPERATIONS": voice.OPERATIONS,
        "voice.READS": voice.READS, "voice.STATES": voice.STATES, "worktrees.OPERATIONS": worktrees.OPERATIONS,
        "launch.OPERATIONS": launch.OPERATIONS, "launch.AGENTS": launch.AGENTS, "terminal.KEYS": terminal.KEYS,
        "accounts.PROVIDERS": accounts.PROVIDERS, "accounts.STATUSES": accounts.STATUSES,
    }
    return document({
        "provenance": "zerus_mobile Python connector; see README.md",
        "sets": {name: sorted(values) for name, values in sets.items()},
        "counts": {name: len(values) for name, values in sets.items()},
        "sequences": {"MOBILE_DELIVERY": list(context.MOBILE_DELIVERY)},
        "maps": {"CONTEXT_COMMANDS": context.CONTEXT_COMMANDS, "speech.ACTIONS": speech.ACTIONS,
                 "speech_config.ACTIONS": speech_config.ACTIONS, "voice.ACTIONS": voice.ACTIONS},
        "sql": {"READ_SQL": context.READ_SQL, "READ_CLASS_SQL": context.READ_CLASS_SQL,
                "speech.READ_SQL": speech.READ_SQL, "voice.READ_SQL": voice.READ_SQL,
                "speech_config.READ_SQL": speech_config.READ_SQL},
        "constants": {
            "MAX_BYTES": connector.MAX_BYTES, "MAX_PAYLOAD": connector.MAX_PAYLOAD, "MAX_RESULT": connector.MAX_RESULT,
            "ARCHIVE_SNAPSHOT_TTL": connector.ARCHIVE_SNAPSHOT_TTL, "attachments.LIMITS": attachments.LIMITS,
            "attachments.MAX_QUEUE_BYTES": attachments.MAX_QUEUE_BYTES, "attachments.MAX_READ_BYTES": attachments.MAX_READ_BYTES,
            "attachment_reads.NATIVE_OUTPUT_BYTES": attachment_reads.NATIVE_OUTPUT_BYTES,
            "context.ATTACHMENT_RESULT_TTL": context.ATTACHMENT_RESULT_TTL,
            "context.ATTACHMENT_READ_GRACE": context.ATTACHMENT_READ_GRACE,
            "fleet.MAX_PEERS": fleet.MAX_PEERS, "fleet.MAX_PEER_SNAPSHOT": fleet.MAX_PEER_SNAPSHOT,
            "fleet.ROUTE_NAMESPACE": str(fleet.ROUTE_NAMESPACE),
            "accounts.REFRESH_SECONDS": accounts.REFRESH_SECONDS, "accounts.CATALOG_SECONDS": accounts.CATALOG_SECONDS,
            "accounts.MAX_ACCOUNTS": accounts.MAX_ACCOUNTS, "accounts.MAX_CATALOG_BYTES": accounts.MAX_CATALOG_BYTES,
            "speech.MAX_AUDIO_BYTES": speech.MAX_AUDIO_BYTES, "speech_config.ALGORITHM": speech_config.ALGORITHM,
            "speech_config.MAX_REVISION": speech_config.MAX_REVISION,
            "launch.DIRECTORY_CREATE_HELP": launch.DIRECTORY_CREATE_HELP,
            "launch.DIRECTORY_CREATE_FEATURE": launch.DIRECTORY_CREATE_FEATURE,
            "managed_relay.CURRENT": managed_relay.CURRENT, "managed_relay.LEGACY": managed_relay.LEGACY,
        },
        "help_markers": {"speech": speech.HELP_MARKER, "voice": voice.HELP_MARKER,
                         "speech_config": speech_config.HELP_MARKER,
                         "attachment_read": attachment_reads.HELP_MARKER,
                         "file_read": file_reads.HELP_MARKER,
                         "archive_list": archives.HELP_MARKER,
                         "directory_create": launch.DIRECTORY_CREATE_HELP},
        "capabilities": capability_map(runs),
    })


# ---------------------------------------------------------------------------
# Error catalogue.

class _Raises(ast.NodeVisitor):
    def __init__(self, module: str):
        self.module, self.scope, self.found, self.logs = module, [], [], []

    def _visit_scope(self, node):
        self.scope.append(node.name)
        self.generic_visit(node)
        self.scope.pop()

    visit_ClassDef = visit_FunctionDef = visit_AsyncFunctionDef = _visit_scope

    def visit_Raise(self, node):
        call = node.exc
        if isinstance(call, ast.Call) and call.args:
            func = call.func
            name = func.id if isinstance(func, ast.Name) else func.attr if isinstance(func, ast.Attribute) else None
            if name == "error" and self.module == "fleet":
                name = "ConnectorError"  # Fleet(connector, ConnectorError) raises through self.error.
            argument = call.args[0]
            entry = {"module": self.module, "function": ".".join(self.scope) or "<module>", "exception": name}
            if isinstance(argument, ast.Constant) and isinstance(argument.value, str):
                self.found.append({**entry, "message": argument.value})
            elif isinstance(argument, ast.JoinedStr):
                self.found.append({**entry, "template": ast.unparse(argument), "review": "f-string"})
        self.generic_visit(node)

    def visit_Call(self, node):
        func = node.func
        if (isinstance(func, ast.Attribute) and isinstance(func.value, ast.Name) and func.value.id == "LOG"
                and node.args and isinstance(node.args[0], ast.Constant)):
            self.logs.append({"module": self.module, "level": func.attr, "format": node.args[0].value})
        self.generic_visit(node)

    def visit_Dict(self, node):
        # Receipts written directly, such as {"state": ..., "error": "connector stopped ..."}.
        for key, value in zip(node.keys, node.values):
            if (isinstance(key, ast.Constant) and key.value == "error" and isinstance(value, ast.Constant)
                    and isinstance(value.value, str)):
                self.found.append({"module": self.module, "function": ".".join(self.scope) or "<module>",
                                   "exception": None, "message": value.value})
        self.generic_visit(node)


def error_catalogue(observed: set[str]) -> bytes:
    package = Path(connector.__file__).resolve().parent
    entries, logs = [], []
    for module in CONNECTOR_MODULES:
        visitor = _Raises(module)
        visitor.visit(ast.parse((package / (module + ".py")).read_text(), filename=module + ".py"))
        entries += visitor.found
        logs += visitor.logs
    counted = {}
    for entry in entries:
        key = json.dumps(entry, sort_keys=True)
        counted[key] = counted.get(key, 0) + 1
    rows = []
    for key, count in counted.items():
        entry = json.loads(key)
        entry["count"] = count
        rows.append(flagged(entry, entry.get("message")))
    rows.sort(key=lambda row: (row["module"], row["function"], row.get("message", row.get("template", ""))))
    literals = {row["message"] for row in rows if "message" in row}
    library = sorted(observed - literals)
    return document({
        "provenance": "AST scan of raise statements in the zerus_mobile connector modules",
        "modules": list(CONNECTOR_MODULES),
        "android": {"source": "mobile/android/app/src/main/java/app/zerus/mobile/AttachmentViewCache.kt",
                    "update_required_substrings": list(ANDROID_UPDATE_REQUIRED),
                    "shown_prefix_characters": ANDROID_ERROR_PREFIX,
                    "note": "attachment_read failures: a receipt error containing either substring maps to "
                            "UPDATE_REQUIRED; any other error is shown truncated to the prefix"},
        "raises": rows,
        # Phone-visible receipt errors in validation.jsonl that come from a library
        # exception passed through str(error), not from a literal in this package.
        "library_messages": [flagged({"message": message}, message) for message in library],
        "log_formats": sorted(logs, key=lambda row: (row["module"], row["format"])),
    })


# ---------------------------------------------------------------------------
# Identities and configuration.

def identity_vectors(root: Path) -> bytes:
    bindings = []
    for server, explicit, token in ((RELAY, None, TOKEN), (RELAY + "/", None, TOKEN), (RELAY, None, "fixture-other-token"),
                                    ("https://zerus.dev.guthub.dev", None, TOKEN),
                                    (managed_relay.CURRENT, managed_relay.LEGACY, TOKEN)):
        config = {"server_url": server, "node_token": token, "state_dir": str(root / ident(server + str(explicit) + token))}
        if explicit is not None:
            config["identity_url"] = explicit
        with fixed_clock():
            node = Connector(config)
        try:
            stored = node.journal.db.execute("SELECT value FROM metadata WHERE key='relay_binding'").fetchone()[0]
        finally:
            node.journal.close()
        bindings.append({"server_url": server, "identity_url": explicit, "node_token": token,
                         "transport": node.server_url, "binding_input": node.identity_url, "binding": stored,
                         "local_swarm_id": projects.unavailable(stored)["swarm_id"]})
    urls = []
    for value, local in (("https://relay.example.test", False), ("https://relay.example.test///", False),
                         ("HTTPS://Relay.Example.Test/v1/", False), ("https://relay.example.test:0", False),
                         ("https://relay.example.test:65535", False), ("https://relay.example.test:65536", False),
                         ("https://relay.example.test:x", False), ("https://relay.example.test/?", False),
                         ("https://relay.example.test#", False), ("https://relay.example.test?a=b", False),
                         ("https://relay.example.test#part", False), ("https://user@relay.example.test", False),
                         ("https://:secret@relay.example.test", False), ("https:///path", False),
                         ("https://relay.example.test/ path", False), ("https://relay.example.test\t", False),
                         ("https://relay.example.test\x7f", False), ("http://relay.example.test", False),
                         ("http://relay.example.test", True), ("http://localhost:8787", False),
                         ("http://localhost:8787", True), ("http://127.0.0.1:8787/", True), ("http://[::1]:8787", True),
                         ("http://127.0.0.2", True), ("ftp://relay.example.test", False), ("https://[::1", False),
                         ("https://%72elay.example.test", False), ("https://r\u00e9lay.example.test", False), (5, False), (None, False)):
        row = {"value": value, "allow_insecure_localhost": local}
        try:
            row["result"] = connector.checked_url(value, local)
        except ConnectorError as error:
            row["error"] = str(error)
        if isinstance(value, str) and not value.isascii() and "result" in row:
            row["divergence"] = "i: Rust refuses a non-ASCII transport host at startup"
        urls.append(row)
    origins = [{"value": value, "managed_origin": managed_relay.managed_origin(value), "transport": managed_relay.transport(value)}
               for value in ("https://relay.zerus.dev", "https://RELAY.zerus.dev:443/", "https://zerus.dev.guthub.dev",
                             "https://zerus.dev.guthub.dev:443", "https://zerus.dev.guthub.dev/v1", "http://zerus.dev.guthub.dev",
                             "https://zerus.dev.guthub.dev:8443", "https://zerus.dev.guthub.dev.example.test",
                             "https://user:@zerus.dev.guthub.dev", "https://relay.zerus.dev?x=1", "https://[::1",
                             "https://relay.example.test")]
    explicit = []
    for server, value in ((RELAY, None), (RELAY, RELAY), (RELAY, "https://other.example.test"),
                          (managed_relay.CURRENT, managed_relay.LEGACY), (managed_relay.CURRENT, "https://zerus.dev.guthub.dev:443/"),
                          (managed_relay.LEGACY, managed_relay.CURRENT), ("https://RELAY.zerus.dev", managed_relay.LEGACY)):
        row = {"server_url": server, "identity_url": value}
        try:
            row["result"] = managed_relay.identity_url(server, value)
        except ValueError as error:
            row["error"] = str(error)
        explicit.append(row)
    routes = [{"via": via, "machine_id": machine, "route_id": fleet.route_id(via, machine)}
              for via, machine in (("configured-mac", "00000000-0000-4000-8000-000000000002"), ("peer.example.test", MACHINE),
                                   ("\u043f\u0438\u0440", MACHINE), ("", MACHINE))]
    return document({"binding_rule": "sha256_hex(identity_url + NUL + node_token)",
                     "local_swarm_id_rule": "'local:' + sha256_hex(binding)",
                     "route_namespace": str(fleet.ROUTE_NAMESPACE), "route_id_rule": "uuid5(route_namespace, via + NUL + machine_id)",
                     "bindings": bindings, "checked_url": urls, "managed_origin": origins,
                     "identity_url": explicit, "route_ids": routes})


GUI_CONFIG = """{
    "extra": {
        "keep": true
    },
    "hgs_path": "/fixture/hgs",
    "node_token": "synthetic-old-node-token",
    "poll_interval": 7,
    "server_url": "https://relay.example.test",
    "state_dir": "$SCRATCH/gui-state"
}
"""
GUI_MANAGED = """{
    "hgs_path": "/fixture/hgs",
    "identity_url": "https://ZERUS.dev.guthub.dev:443",
    "node_token": "synthetic-old-node-token",
    "poll_interval": 7,
    "server_url": "https://relay.zerus.dev",
    "state_dir": "$SCRATCH/gui-managed"
}
"""


def config_cases():
    base = {"server_url": RELAY, "node_token": TOKEN, "state_dir": "$SCRATCH/state"}

    def case(name, config=None, *, text=None, local=False, setup=(), divergence=None):
        row = {"id": name, "allow_insecure_localhost": local}
        if text is not None:
            row["config_text"] = text
        else:
            row["config"] = config
        if setup:
            row["setup"] = list(setup)
        if divergence:
            row["divergence"] = divergence
        return row

    def changed(**values):
        return {**base, **values}

    def without(*keys):
        return {key: value for key, value in base.items() if key not in keys}

    return [
        case("minimal", base),
        case("defaults", without("state_dir"), divergence=None),
        case("gui-written", text=GUI_CONFIG),
        case("gui-managed-migration", text=GUI_MANAGED),
        case("managed-legacy-transport", changed(server_url="https://zerus.dev.guthub.dev")),
        case("identity-equal", changed(identity_url=RELAY)),
        case("identity-null", changed(identity_url=None)),
        case("identity-foreign", changed(identity_url="https://other.example.test")),
        case("identity-invalid", changed(identity_url="http://other.example.test")),
        case("server-missing", without("server_url")),
        case("server-http", changed(server_url="http://relay.example.test")),
        case("server-loopback-without-flag", changed(server_url="http://127.0.0.1:8787")),
        case("server-loopback-with-flag", changed(server_url="http://127.0.0.1:8787"), local=True),
        case("server-trailing-slashes", changed(server_url=RELAY + "//")),
        case("server-number", changed(server_url=443)),
        case("token-missing", without("node_token")),
        case("token-empty", changed(node_token="")),
        case("token-space", changed(node_token="fixture token")),
        case("token-non-ascii", changed(node_token="fixture-\u00e9")),
        case("token-number", changed(node_token=7)),
        case("hgs-default", base),
        case("hgs-explicit", changed(hgs_path="/fixture/bin/hgs")),
        case("hgs-empty", changed(hgs_path="")),
        case("hgs-null", changed(hgs_path=None)),
        case("hgs-nul", changed(hgs_path="/fixture/h\u0000gs")),
        case("poll-one", changed(poll_interval=1)),
        case("poll-fraction", changed(poll_interval=1.5)),
        case("poll-below-one", changed(poll_interval=0.5)),
        case("poll-string", changed(poll_interval="5")),
        case("poll-null", changed(poll_interval=None)),
        case("poll-true", changed(poll_interval=True), divergence="k: Python accepts a boolean as a number"),
        case("poll-nan", text='{"server_url": "%s", "node_token": "%s", "state_dir": "$SCRATCH/state", "poll_interval": NaN}' % (RELAY, TOKEN)),
        case("poll-infinity", text='{"server_url": "%s", "node_token": "%s", "state_dir": "$SCRATCH/state", "poll_interval": Infinity}' % (RELAY, TOKEN)),
        case("state-empty-string", changed(state_dir="")),
        case("state-null", changed(state_dir=None)),
        case("state-zero", changed(state_dir=0), divergence="k: Python treats a falsy state_dir as absent"),
        case("state-false", changed(state_dir=False), divergence="k: Python treats a falsy state_dir as absent"),
        case("state-number", changed(state_dir=5)),
        case("state-list", changed(state_dir=["x"])),
        case("state-relative", changed(state_dir="relative-state")),
        case("state-home", changed(state_dir="~/home-state")),
        case("state-tilde-user", changed(state_dir="~nosuchuser-vectors/state"),
             divergence="f: Rust refuses ~user; Python expands a known user and raises RuntimeError for an unknown one"),
        case("state-existing-private", base, setup=[{"action": "mkdir", "path": "state", "mode": "0700"}]),
        case("state-group-writable", base, setup=[{"action": "mkdir", "path": "state", "mode": "0770"}]),
        case("state-world-readable", base, setup=[{"action": "mkdir", "path": "state", "mode": "0705"}]),
        case("state-is-file", base, setup=[{"action": "write", "path": "state", "mode": "0600", "text": ""}]),
        case("state-is-symlink", base, setup=[{"action": "mkdir", "path": "private", "mode": "0700"},
                                              {"action": "symlink", "path": "state", "target": "private"}]),
        case("unknown-keys-ignored", changed(extra={"keep": True}, comment="fixture")),
        case("not-an-object", text='["server_url"]'),
        case("invalid-json", text='{"server_url": '),
    ]


def setup_paths(root: Path, steps) -> None:
    for step in steps:
        path = root / step["path"]
        if step["action"] == "mkdir":
            path.mkdir()
            path.chmod(int(step["mode"], 8))
        elif step["action"] == "chmod":
            path.chmod(int(step["mode"], 8))
        elif step["action"] == "write":
            fd = os.open(path, os.O_CREAT | os.O_WRONLY | os.O_EXCL, int(step["mode"], 8))
            with os.fdopen(fd, "w") as handle:
                handle.write(step.get("text", ""))
            path.chmod(int(step["mode"], 8))
        elif step["action"] == "symlink":
            path.symlink_to(step["target"])
        elif step["action"] == "hardlink":
            path.hardlink_to(root / step["target"])
        else:
            raise ValueError("unknown setup action " + step["action"])


@contextlib.contextmanager
def isolated_environment(root: Path):
    """HOME and the working directory point into the scratch tree."""
    home, cwd = root / "home", root / "cwd"
    home.mkdir(mode=0o700)
    cwd.mkdir(mode=0o700)
    previous = os.getcwd()
    with mock.patch.dict(os.environ, {"HOME": str(home)}):
        if Path.home() != home:
            raise RuntimeError("HOME isolation failed")
        os.chdir(cwd)
        try:
            yield
        finally:
            os.chdir(previous)


def placeholders(value, root: Path):
    if isinstance(value, str):
        return value.replace("$SCRATCH", str(root))
    if isinstance(value, list):
        return [placeholders(item, root) for item in value]
    if isinstance(value, dict):
        return {key: placeholders(item, root) for key, item in value.items()}
    return value


def portable(value, root: Path):
    if isinstance(value, str):
        return (value.replace(str(root / "home"), "$HOME").replace(str(root / "cwd"), "$CWD")
                .replace(str(root), "$SCRATCH"))
    return value


def tree(root: Path, skip=("home", "cwd")) -> dict:
    """Every created path below root with its kind and permission bits."""
    found = {}
    for path in sorted(root.rglob("*")):
        relative = path.relative_to(root)
        if relative.parts[0] in skip and len(relative.parts) == 1:
            continue
        info = path.lstat()
        if stat.S_ISLNK(info.st_mode):  # Link permission bits differ between Linux and macOS.
            found[str(relative)] = {"kind": "symlink"}
            continue
        kind = "dir" if stat.S_ISDIR(info.st_mode) else "file"
        found[str(relative)] = {"kind": kind, "mode": "%04o" % stat.S_IMODE(info.st_mode)}
    return found


def config_vectors() -> list[dict]:
    rows = []
    for spec in config_cases():
        with scratch() as root, isolated_environment(root), fixed_clock():
            setup_paths(root, spec.get("setup", ()))
            before = set(tree(root))
            row = copy.deepcopy(spec)
            directories = []

            class Recording(Journal):
                def __init__(self, directory, binding):
                    directories.append(directory)
                    super().__init__(directory, binding)

            try:
                if "config_text" in spec:
                    config = json.loads(placeholders(spec["config_text"], root))
                    if not isinstance(config, dict):
                        raise ConnectorError("configuration must be a JSON object")
                else:
                    config = placeholders(spec["config"], root)
                with mock.patch.object(connector, "Journal", Recording):
                    node = Connector(config, allow_insecure_localhost=spec["allow_insecure_localhost"])
            except Exception as error:
                # main() turns only these into the fixed startup line; anything else escapes it.
                row["expect"] = {"accepted": False, **raised(error),
                                 "startup_line": isinstance(error, (OSError, ValueError, ConnectorError))}
            else:
                try:
                    stored = node.journal.db.execute("SELECT value FROM metadata WHERE key='relay_binding'").fetchone()[0]
                    row["expect"] = {"accepted": True, "server_url": node.server_url, "identity_url": node.identity_url,
                                     "hgs_path": node.hgs, "poll_interval": node.interval, "binding": stored}
                finally:
                    node.journal.close()
            if directories:
                # The directory exactly as Python resolves it: expanduser, then relative to the working directory.
                row["expect"]["state_dir"] = portable(os.path.join(os.getcwd(), directories[0]), root)
            created = tree(root)
            row["expect"]["created"] = {portable(str(root / path), root): info
                                        for path, info in created.items() if path not in before}
        rows.append(row)
    rows += main_vectors()
    return rows


def main_vectors() -> list[dict]:
    """connector.main(): argv handling, exit status and the fixed startup log line."""
    valid = json.dumps({"server_url": RELAY, "node_token": TOKEN, "state_dir": "$SCRATCH/state"})
    loopback = json.dumps({"server_url": "http://127.0.0.1:8787", "node_token": TOKEN, "state_dir": "$SCRATCH/state"})
    cases = [
        ("main-runs", ["--config", "$SCRATCH/connector.json"], valid, "0600", None),
        ("main-equals-form", ["--config=$SCRATCH/connector.json"], valid, "0600", None),
        ("main-prefix-abbreviation", ["--conf", "$SCRATCH/connector.json"], valid, "0600",
         "i-k: Python argparse accepts unambiguous option prefixes; Rust does not"),
        ("main-repeated-config", ["--config", "$SCRATCH/missing.json", "--config", "$SCRATCH/connector.json"], valid, "0600", None),
        ("main-world-writable-config", ["--config", "$SCRATCH/connector.json"], valid, "0666",
         "e: Rust refuses a group- or world-writable connector.json"),
        ("main-loopback-flag", ["--config", "$SCRATCH/connector.json", "--allow-insecure-localhost"], loopback, "0600", None),
        ("main-loopback-without-flag", ["--config", "$SCRATCH/connector.json"], loopback, "0600", None),
        ("main-missing-config-option", [], valid, "0600", None),
        ("main-unknown-option", ["--config", "$SCRATCH/connector.json", "--verbose"], valid, "0600", None),
        ("main-missing-file", ["--config", "$SCRATCH/missing.json"], valid, "0600", None),
        ("main-invalid-json", ["--config", "$SCRATCH/connector.json"], "{", "0600", None),
        ("main-not-object", ["--config", "$SCRATCH/connector.json"], "[]", "0600", None),
        ("main-invalid-token", ["--config", "$SCRATCH/connector.json"],
         json.dumps({"server_url": RELAY, "node_token": "", "state_dir": "$SCRATCH/state"}), "0600", None),
        ("main-unknown-home-user", ["--config", "$SCRATCH/connector.json"],
         json.dumps({"server_url": RELAY, "node_token": TOKEN, "state_dir": "~nosuchuser-vectors/state"}), "0600",
         "f: pathlib raises RuntimeError, which main() does not catch; Rust refuses ~user with a fixed error"),
    ]
    rows = []
    for name, argv, text, mode, divergence in cases:
        with scratch() as root, isolated_environment(root):
            path = root / "connector.json"
            fd = os.open(path, os.O_CREAT | os.O_WRONLY | os.O_EXCL, 0o600)
            with os.fdopen(fd, "w") as handle:
                handle.write(placeholders(text, root))
            path.chmod(int(mode, 8))
            ran, settings = [], []
            stream = io.StringIO()
            handler = logging.StreamHandler(stream)

            def run(coroutine):
                coroutine.close()
                ran.append(True)

            def basic_config(**options):
                # main() configures the root logger; apply its format to a private handler instead.
                settings.append(options)
                handler.setFormatter(logging.Formatter(options["format"]))

            with connector_log(handler), mock.patch.object(connector.asyncio, "run", run), \
                    mock.patch.object(connector.logging, "basicConfig", basic_config), \
                    mock.patch.object(sys, "argv", ["zerus-mobile-connector", *placeholders(argv, root)]), \
                    contextlib.redirect_stderr(io.StringIO()), fixed_clock():
                uncaught = None
                try:
                    connector.main()
                    status = 0
                except SystemExit as exit:
                    status = exit.code
                except Exception as error:  # The interpreter prints a traceback and exits 1.
                    status, uncaught = 1, type(error).__name__
            row = {"id": name, "argv": argv, "config_text": text, "config_mode": mode,
                   "expect": {"exit": status, "ran": bool(ran), "log": stream.getvalue().splitlines(),
                              "logging": settings[0] if settings else None, "uncaught": uncaught}}
            if divergence:
                row["divergence"] = divergence
            rows.append(row)
    return rows


# ---------------------------------------------------------------------------
# request_hash and canonical JSON.

class HashProbe:
    """Stands in for the connector while the real execute() derives request_hash."""

    def __init__(self):
        self.journal, self.claims = self, []

    def claim(self, request_id, operation, request_hash=None):
        self.claims.append({"request_id": request_id, "operation": operation, "request_hash": request_hash})
        return False

    def replay(self, request_id):
        return {"replayed": request_id}


class PollStop(BaseException):
    """Ends the real requests() loop once it has handled one relay poll."""


class RelayPoll:
    """A client whose every request answers 200 with one fixed relay body."""

    def __init__(self, data: bytes):
        self.status, self.content, self.data = 200, self, data

    def request(self, method, url, **options):
        return self

    async def __aenter__(self):
        return self

    async def __aexit__(self, *exc):
        return False

    async def iter_chunked(self, size):
        for start in range(0, len(self.data), size):
            yield self.data[start:start + size]


class PollProbe:
    """Stands in for the connector while the real requests() loop parses and
    dispatches one batch: the real http() reads the body, execute is injected."""

    def __init__(self, execute):
        self.server_url, self.token, self.execute = RELAY, TOKEN, execute
        self.snapshot_ready = asyncio.Event()
        self.snapshot_ready.set()
        self.fleet = types.SimpleNamespace(probe_complete=True, supported=False, relay_supported=False,
                                           local_id=None, available=False)
        self.flushes, self.parsed = 0, False

    async def http(self, client, method, path, body=None, **options):
        batch = await Connector.http(self, client, method, path, body, **options)
        self.parsed = True
        return batch

    async def flush(self, client):
        self.flushes += 1
        if self.flushes > 1:  # The flush after the batch: every request was handled.
            raise PollStop


def hash_cases() -> list[dict]:
    rid = ident("hash/base")
    head = '{"request_id":"%s","operation":"inspect","session":"%s","payload":' % (rid, SESSION)

    def request(payload_text, name, note=None):
        row = {"id": name, "envelope": "request", "raw": head + payload_text + "}"}
        if note:
            row["note"] = note
        return row

    cases = [
        request('{"b":1,"a":2}', "key-order"),
        {"id": "key-order-top-level", "envelope": "request",
         "raw": '{"session":"%s","payload":{},"operation":"inspect","request_id":"%s"}' % (SESSION, rid)},
        request('{"a":1,"a":2}', "duplicate-key-last-wins"),
        request('{"\u00e9":1,"z":2,"Z":3,"\U0001f600":4,"\\uffff":5,"\\ue000":6,"a":7,"\\u00e9x":8}', "unicode-key-order",
                "Python sorts keys by code point, not by UTF-16 unit"),
        request('{"s":"\u00e9 \u754c \U0001f600 \\ud83d\\ude00"}', "non-bmp-and-escaped-pair"),
        request('{"s":"\\"\\\\\\/\\b\\f\\n\\r\\t\\u0000\\u001f\\u007f\\u0080\\u2028\\u2029\\ufeff"}', "every-escape-class"),
        request('{"s":"\\ud800"}', "lone-high-surrogate", "Python accepts; the relay parser cannot produce it"),
        request('{"s":"\\udfff"}', "lone-low-surrogate", "Python accepts; the relay parser cannot produce it"),
        request('{"s":"\\ude00\\ud83d"}', "reversed-surrogates", "Python accepts; the relay parser cannot produce it"),
        request('{"$serde_json::private::Number":"123"}', "serde-magic-key"),
        request('{"$serde_json::private::Number":123}', "serde-magic-key-number"),
        request('{"a":[],"b":{},"c":[{}],"d":null,"e":true,"f":false,"g":""}', "empty-containers"),
        request(' { "a" : [ 1 , 2 ] }\n', "whitespace"),
    ]
    numbers = ["0", "-0", "-0.0", "0.0", "0e0", "-0e0", "1", "1.0", "1.50", "1e2", "1E2", "1.0e+2", "10E-1",
               "1e15", "1e16", "1e17", "9999999999999998.0", "12345678901234567.0", "0.0001", "0.00001", "1e-5",
               "1.5e-5", "1E-7", "0.1", "123456789.123456789", "1792058053.774066", "5e-324", "4.9e-324", "2e-324",
               "2.2250738585072014e-308", "1.7976931348623157e308", "123456789012345678901234567890.0",
               "9007199254740993", "9007199254740993.0", "9223372036854775807", "9223372036854775808",
               "18446744073709551615", "18446744073709551616", str(2 ** 80), str(-2 ** 80), str(2 ** 80 + 1),
               "1" + "0" * 100]
    for text in numbers:
        cases.append(request('{"x":%s}' % text, "number/" + (text if len(text) <= 40 else "1e100-as-integer")))
    for digits, note in ((4300, None), (4301, "Python json refuses an integer of more than 4300 digits")):
        row = {"id": "integer-%d-digits" % digits, "envelope": "request",
               "raw_parts": [[head + '{"x":9', 1], ["8", digits - 2], ["7}}", 1]]}
        if note:
            row["note"] = note
        cases.append(row)
    cases += [
        request('{"x":1e400}', "float-overflow"),
        request('{"x":-1e400}', "float-negative-overflow"),
        request('{"x":1.7976931348623159e308}', "float-rounds-to-infinity"),
        request('{"x":NaN}', "nan-literal", "Python json accepts NaN; the hash refuses it"),
        request('{"x":Infinity}', "infinity-literal", "Python json accepts Infinity; the hash refuses it"),
        request('{"x":-Infinity}', "negative-infinity-literal"),
        {"id": "not-an-object-array", "envelope": "request", "raw": "[]"},
        {"id": "not-an-object-string", "envelope": "request", "raw": '"x"'},
        {"id": "not-an-object-null", "envelope": "request", "raw": "null"},
        {"id": "request-id-missing", "envelope": "request", "raw": '{"operation":"inspect","session":"%s","payload":{}}' % SESSION},
        {"id": "request-id-uppercase", "envelope": "request", "raw": head.replace(rid, rid.upper()) + "{}}"},
        {"id": "request-id-braces", "envelope": "request", "raw": head.replace('"' + rid + '"', '"{' + rid + '}"') + "{}}"},
        {"id": "request-id-number", "envelope": "request", "raw": '{"request_id":7,"operation":"inspect","session":"","payload":{}}'},
        {"id": "request-id-before-nan", "envelope": "request", "raw": '{"request_id":"x","payload":{"x":NaN}}',
         "note": "request_id is checked before the hash"},
        {"id": "operation-not-string", "envelope": "request",
         "raw": '{"request_id":"%s","operation":7,"session":"%s","payload":{}}' % (rid, SESSION),
         "note": "journaled as operation 'invalid'"},
        {"id": "operation-missing", "envelope": "request", "raw": '{"request_id":"%s","session":"","payload":{}}' % rid},
        {"id": "operation-unknown", "envelope": "request",
         "raw": '{"request_id":"%s","operation":"bogus_operation","session":"","payload":{}}' % rid},
    ]
    for operation, entry in envelopes():
        raw = json.dumps(entry, ensure_ascii=False, separators=(",", ":"))
        cases.append({"id": "envelope-" + operation, "envelope": "request", "raw": raw})
    gateway = {"schema": 1, "route_id": ident("route"), "computer_id": COMPUTER, "machine_id": MACHINE}
    for operation, entry in envelopes():
        if operation not in {"inspect", "send", "terminal_input", "directory_create"}:
            continue
        for label, route in (("local", {**gateway, "route_id": None}), ("peer", gateway)):
            claimed = {**entry, "gateway_route": route}
            cases.append({"id": "gateway-%s-%s" % (label, operation), "envelope": "request",
                          "raw": json.dumps(claimed, separators=(",", ":"))})
    batch = [entry for operation, entry in envelopes() if operation in {"inspect", "send"}]
    cases.append({"id": "batch-two-requests", "envelope": "batch",
                  "raw": json.dumps({"requests": batch}, separators=(",", ":"))})
    cases.append({"id": "batch-nan-only-rejects-one", "envelope": "batch",
                  "raw": '{"requests":[%s,{"request_id":"%s","operation":"inspect","session":"","payload":{"x":NaN}}]}'
                         % (json.dumps(batch[0], separators=(",", ":")), ident("hash/nan"))})
    cases.append({"id": "batch-not-list", "envelope": "batch", "raw": '{"requests":{}}'})
    cases.append({"id": "batch-101-requests", "envelope": "batch",
                  "raw_parts": [['{"requests":[', 1], ['{},', 100], ['{}]}', 1]]})
    cases.append({"id": "batch-100-requests", "envelope": "batch",
                  "raw_parts": [['{"requests":[', 1], ['[],', 99], ['[]]}', 1]]})
    cases.append({"id": "batch-invalid-json", "envelope": "batch", "raw": '{"requests":['})
    cases.append(relay_maximum())
    return cases


def relay_maximum() -> dict:
    """An answer submission exactly at the relay limits (depth 32, 50,000 values)
    as the connector receives it: computer_id removed, wrapped in its batch."""
    rid = ident("hash/relay-maximum")
    head = ('{"requests":[{"request_id":"%s","operation":"answer","session":"%s","payload":{"request_id":"%s",'
            '"expected_run_id":"%s","expected_conversation_id":"%s","question_id":"q","expected_question_hash":"h",'
            '"answers":' % (rid, SESSION, rid, RUN, CONVERSATION))
    # Submission values: 1 object + 5 keys + 4 strings, payload 1 object + 6 keys + 5 strings = 22,
    # 30 nested arrays (depth 3..32) = 52, and 49,948 scalars = 50,000. The relay
    # removes computer_id (2 values) and adds the batch object, its key and array.
    return {"id": "relay-maximum-in-batch", "envelope": "batch",
            "note": "submission at max_json_depth 32 and max_json_values 50000; the batch is depth 34 with 50001 values",
            "raw_parts": [[head, 1], ["[", 30], ["0,", 49947], ["0", 1], ["]", 30], ["}}]}", 1]]}


def raw_bytes(case: dict) -> bytes:
    if "raw" in case:
        return case["raw"].encode()
    return "".join(text * count for text, count in case["raw_parts"]).encode()


def hash_vectors() -> dict[str, bytes]:
    rows = []

    async def poll(case) -> dict:
        """One relay poll through the real requests() loop; a lone request is
        delivered as the only member of its batch."""
        raw = raw_bytes(case)
        outcomes = []

        async def execute(request):
            probe = HashProbe()
            try:
                await Connector.execute(probe, request)
            except ConnectorError as error:
                outcomes.append({"rejected": "request", "error": str(error)})
                raise  # requests() logs it and continues with the next request.
            claim = probe.claims[0]
            canonical = json.dumps(request, sort_keys=True, separators=(",", ":"), allow_nan=False)
            if hashlib.sha256(canonical.encode()).hexdigest() != claim["request_hash"]:
                raise RuntimeError("canonical text does not reproduce request_hash")
            result = {"request_hash": claim["request_hash"], "journal_operation": claim["operation"],
                      "canonical_bytes": len(canonical.encode())}
            if len(canonical) <= 2048:
                result["canonical"] = canonical
            outcomes.append(result)

        async def stop(delay):  # The back-off after a failed poll.
            raise PollStop

        node, records = PollProbe(execute), LogRecords()
        body = raw if case["envelope"] == "batch" else b'{"requests":[' + raw + b']}'
        with connector_log(records), mock.patch.object(connector.asyncio, "sleep", stop), \
                contextlib.suppress(PollStop):
            await Connector.requests(node, RelayPoll(body))
        failed = [message.removeprefix("Requests: ") for message in records.messages if message.startswith("Requests: ")]
        if failed:
            return {"rejected": "batch" if node.parsed else "parse", "error": failed[0]}
        if case["envelope"] == "batch":
            return {"requests": outcomes}
        if len(outcomes) != 1:
            raise CaptureError("%s: the requests() loop did not execute the request exactly once" % case["id"])
        return outcomes[0]

    async def run():
        for case in hash_cases():
            rows.append({**case, "expect": await poll(case)})

    asyncio.run(run())
    envelope = ("envelope-", "gateway-", "batch-", "relay-maximum")
    return {"request_hash.jsonl": jsonl(row for row in rows if not row["id"].startswith(envelope)),
            "request_envelopes.jsonl": jsonl(row for row in rows if row["id"].startswith(envelope))}


# ---------------------------------------------------------------------------
# Validation: one valid envelope per operation, test call sites and edges.

ID = "$id"


def identity(**extra):
    return {"request_id": ID, "expected_run_id": RUN, "expected_conversation_id": CONVERSATION, **extra}


def valid_payloads() -> dict:
    upsert_profile = {"id": "example", "name": "Example transcription", "adapter": "openai_http", "model": "fixture",
                      "language": "ru", "base_url": "https://speech.example.invalid/v1", "allow_insecure_http": False}
    return {
        "analytics_read": {"schema": 1, "from": "2026-01-01", "through": "2026-03-31"},
        "inspect": {}, "send": identity(text="Hello from the phone"),
        "answer": identity(question_id="q", expected_question_hash="h", answers=[["Yes"]]),
        "interrupt": identity(expected_turn_started=True), "send_now": identity(queue_id="c" * 64),
        "settings": identity(model="fixture-model"), "process_output": identity(process_id="job"),
        "process_stop": identity(process_id="job"), "compact_context": identity(), "clear_context": identity(),
        "pause": identity(), "resume": identity(), "archive": identity(), "terminate": identity(),
        "rename": identity(new_name="codex/example/renamed"), "fork": identity(tag="fork-tag"),
        "restore": identity(archive_id=ARCHIVE), "forget": identity(archive_id=ARCHIVE),
        "terminal_snapshot": identity(),
        "terminal_input": identity(terminal_binding_id="d" * 64, text="ls", enter=True),
        "hooks_status": identity(),
        "project_assignment": identity(expected_machine_id=MACHINE),
        "project_assign": identity(expected_machine_id=MACHINE,swarm_id=ident("project-swarm"),membership_token="a"*64,catalog_epoch="b"*64,project_id="logical"),
        "history": identity(limit=50), "recovery_action": identity(job_id=ident("job"), action="now"),
        "mark_read": {"run_id": RUN, "conversation_id": CONVERSATION, "reply_id": "7:100.5", "attention_signature": HASH_A},
        "session_notifications": {"run_id": RUN, "conversation_id": CONVERSATION, "muted": True, "expected_revision": 0},
        "attachment_read": identity(message_id=ident("message"), index=0, offset=0, length=1024),
        "archive_list": {"from":0,"until":253402300800,"limit":50,"cursor":"","query":""},
        "file_read": identity(path="/workspace/example/Сводная.xlsx", offset=0, length=1024),
        "voice_profiles": identity(), "voice_submit": identity(speech={"machine_id": MACHINE, "profile_id": "gigaam-ru"}, audio=AUDIO),
        "voice_status": identity(message_id=ident("voice-message")), "voice_cancel": identity(message_id=ident("voice-message")),
        "speech_profiles": {}, "speech_stats": {}, "speech_submit": {"request_id": ID, "profile_id": "gigaam-ru", "audio": AUDIO},
        "speech_status": {"job_id": ident("speech-job")}, "speech_cancel": {"job_id": ident("speech-job")},
        "speech_config": {}, "speech_config_status": {"operation_id": ident("speech-operation"), "expected_machine_id": MACHINE},
        "speech_profile_upsert": {"request_id": ID, "expected_machine_id": MACHINE, "expected_revision": 0,
                                  "profile": upsert_profile, "credential": {"mode": "replace", "sealed_value": SEALED}},
        "speech_profile_remove": {"request_id": ID, "expected_machine_id": MACHINE, "expected_revision": 1, "profile_id": "example"},
        "speech_profile_test": {"request_id": ID, "expected_machine_id": MACHINE, "expected_profile_revision": 1, "profile_id": "example"},
        "catalog": {"request_id": ID}, "dirs": {"request_id": ID, "path": "/example"},
        "directory_create": {"request_id": ID, "path": "/example/new"},
        "launch": {"request_id": ID, "agent": "codex", "directory": "/example/mobile", "tag": "new-session"},
        "worktrees": {"request_id": ID, "path": "/example/repo"},
        "worktree_create": {"request_id": ID, "path": "/example/repo", "common_dir": "/example/repo/.git",
                            "destination": "/example/repo-feature", "branch": "feature", "base": "main"},
        "set_mobile_delivery": {"value": "away_5"},
    }


SESSIONLESS = {"analytics_read", "archive_list"} | context.LAUNCH_OPERATIONS | speech.OPERATIONS | speech_config.OPERATIONS | context.COMPUTER_OPERATIONS


def substitute(value, request_id):
    if value == ID:
        return request_id
    if isinstance(value, list):
        return [substitute(item, request_id) for item in value]
    if isinstance(value, dict):
        return {key: substitute(item, request_id) for key, item in value.items()}
    return value


def envelopes():
    """(operation, claimed request) for every operation, as the relay delivers it."""
    for operation, payload in sorted(valid_payloads().items()):
        rid = ident("envelope/" + operation)
        entry = {"request_id": rid, "operation": operation, "session": "" if operation in SESSIONLESS else SESSION,
                 "payload": substitute(payload, rid)}
        if operation == "terminal_input":
            entry["expires_at"] = EPOCH + 5
        yield operation, entry


DEFAULT_CONTEXT = {"sessions": [DSH_SESSION, SESSION], "archives": [[SESSION, ARCHIVE], [SESSION, NIL]], "snapshot_age": 0}
DROP = object()


def validation_cases() -> list[dict]:
    valid = valid_payloads()
    cases = []

    def case(name, operation, payload=None, *, session=None, source="edge", context_=None, top=None, request_id=None,
             change=None):
        payload = copy.deepcopy(valid[operation] if payload is None else payload)
        if change:
            change(payload)
        rid = request_id or ident("validation/" + name)
        submission = {"request_id": rid, "computer_id": COMPUTER, "operation": operation,
                      "session": ("" if operation in SESSIONLESS else SESSION) if session is None else session,
                      "payload": substitute(payload, rid)}
        for key, value in (top or {}).items():
            if value is DROP:
                submission.pop(key)
            else:
                submission[key] = value
        row = {"kind": "case", "id": name, "source": source, "submission": submission}
        if context_:
            row["context"] = {**DEFAULT_CONTEXT, **context_}
        cases.append(row)

    def put(**values):
        return lambda payload: payload.update(values)

    def drop(*keys):
        return lambda payload: [payload.pop(key) for key in keys]

    for operation in sorted(valid):
        case("valid/" + operation, operation)
    # Additive metadata-only project ABI, including byte bounds and no launch fields.
    for operation in sorted(session_projects.OPERATIONS):
        for field in ["directory","add_folder","project_folder_id","archive_id","agent_id","organization"]:
            case(operation+"/forbidden-"+field,operation,change=put(**{field:"foreign"}))
        case(operation+"/machine-nil",operation,change=put(expected_machine_id=NIL))
        case(operation+"/empty-conversation",operation,change=put(expected_conversation_id=""))
    case("project_assignment/cursor", "project_assignment", change=put(cursor="b"*64+":100"))
    case("project_assignment/mutation-field", "project_assignment", change=put(project_id="logical"))
    case("project_assign/short-proof", "project_assign", change=put(membership_token="a"*63))
    case("project_assign/cursor", "project_assign", change=put(cursor="b"*64+":100"))
    # Session and envelope rules shared by every session operation.
    case("session/unknown", "send", session="codex/example/other")
    case("session/dash", "inspect", session="-help")
    case("session/at", "inspect", session="codex/example@host")
    case("session/control", "inspect", session="codex/example\tsession")
    # The Rust relay's protocol::text refuses DEL (also payload/run-delete); the reference
    # relay's text() refused only C0 controls.
    case("session/delete", "inspect", session="codex/example\x7fsession")
    case("computer-id/delete", "inspect", top={"computer_id": COMPUTER[:35] + "\x7f"})
    case("session/too-long", "inspect", session="c" * 513)
    case("session/not-string", "inspect", session=7)
    case("session/archive-at-top-level", "inspect", top={"archive_id": ARCHIVE})
    case("operation/unknown", "inspect", top={"operation": "bogus_operation"})
    case("operation/not-string", "inspect", top={"operation": 5})
    case("request-id/uppercase", "inspect", request_id=ident("validation/upper").upper())
    case("payload/missing", "inspect", top={"payload": DROP})
    case("payload/null", "send", top={"payload": None})
    case("payload/request-id-mismatch", "send", change=put(request_id=ident("other")))
    case("payload/run-missing", "send", change=drop("expected_run_id"))
    case("payload/run-empty", "send", change=put(expected_run_id=""))
    case("payload/run-delete", "send", change=put(expected_run_id="fixture\x7frun"))
    case("payload/archived-non-inspection", "pause", change=put(archive_id=ARCHIVE),
         source="test_mobile_actions.LifecycleTransportTests.test_old_cli_and_invalid_payload_never_invoke_mutator")
    # inspect
    ids = {"expected_run_id": RUN, "expected_conversation_id": CONVERSATION}
    controls = "test_mobile_controls.CoreConnectorTests.test_invalid_fields_tokens_or_extensions_never_spawn"
    case("inspect/after", "inspect", {"after": 5, **ids})
    case("inspect/after-one", "inspect", {"after": 1, **ids},
         source="test_mobile_controls.CoreRelayTests.test_extension_caps_old_clients_and_read_retention")
    case("inspect/after-negative", "inspect", {"after": -1, **ids}, source=controls)
    case("inspect/after-bool", "inspect", {"after": True}, source=controls)
    case("inspect/after-without-identity", "inspect", {"after": 2}, source=controls)
    case("inspect/after-maximum", "inspect", {"after": 2 ** 63 - 1, **ids})
    case("inspect/after-beyond-maximum", "inspect", {"after": 2 ** 63, **ids})
    case("inspect/agent", "inspect", {"agent_id": "child", **ids})
    case("inspect/agent-path", "inspect", {"agent_id": "../path", **ids}, source=controls)
    case("inspect/agent-and-after", "inspect", {"agent_id": "child", "after": 1, **ids})
    case("inspect/identity-only", "inspect", dict(ids))
    case("inspect/extra-field", "inspect", {"path": "/example"})
    case("inspect/archive", "inspect", {"archive_id": ARCHIVE})
    case("inspect/archive-stale", "inspect", {"archive_id": ARCHIVE}, context_={"snapshot_age": 46})
    case("inspect/archive-unknown", "inspect", {"archive_id": ident("other-archive")})
    case("inspect/archive-not-canonical", "inspect", {"archive_id": ARCHIVE.upper()})
    case("inspect/archived-session-name", "inspect", {"archive_id": ARCHIVE}, session="codex/example/other")
    # send, answer, interrupt
    case("send/attachments", "send", change=put(attachments=[{"name": "note.txt", "mime": "text/plain", "data_base64": "aGk="}]))
    case("send/attachment-path", "send", change=put(attachments=[{"name": "../note.txt", "mime": "text/plain", "data_base64": "aGk="}]))
    case("send/empty", "send", change=put(text=" "))
    case("send/control", "send", change=put(text="a\x1bb"))
    case("send/provider-command", "send", change=put(text="  /help"))
    case("send/extra-field", "send", change=put(path="/example"))
    case("send/text-missing", "send", change=drop("text"))
    case("send/agent", "send", session=DSH_SESSION, change=put(agent_id="child"))
    case("send/agent-compaction", "send", session=DSH_SESSION, change=put(agent_id="child", expected_compaction_id=ident("compaction")))
    case("send/compaction", "send", change=put(expected_compaction_id=ident("compaction")))
    case("send/compaction-not-uuid", "send", change=put(expected_compaction_id="compaction"))
    case("answer/fields-missing", "answer", identity())
    case("answer/extra-field", "answer", change=put(path="/example"))
    case("interrupt/field-missing", "interrupt", identity())
    case("interrupt/empty-conversation", "interrupt", change=put(expected_conversation_id=""))
    # send_now, settings, processes
    case("send_now/queue-path", "send_now", change=put(queue_id="../path"), source=controls)
    case("send_now/empty-conversation", "send_now", change=put(expected_conversation_id=""))
    case("settings/none", "settings", identity(), source=controls)
    case("settings/model-empty", "settings", identity(model=""), source=controls)
    case("settings/effort-invented", "settings", identity(effort="invented"), source=controls)
    case("settings/effort-high", "settings", identity(effort="high"))
    case("settings/extra-field", "settings", identity(model="fixture-model", arbitrary="command"), source=controls)
    case("settings/model-utf8-bytes", "settings", identity(model="\u754c" * 86))
    case("settings/pending", "settings", identity(model="fixture-model", expected_pending_id=ident("pending")))
    case("settings/pending-not-uuid", "settings", identity(model="fixture-model", expected_pending_id="pending"))
    case("settings/pending-uppercase", "settings", identity(model="fixture-model", expected_pending_id=ident("pending").upper()))
    case("process_stop/path", "process_stop", identity(process_id="../../path"), source=controls)
    case("process_output/generation", "process_output", identity(process_id="job", generation="g:1"))
    case("process_output/archive", "process_output", identity(process_id="job", archive_id=ARCHIVE))
    case("process_output/archive-stale", "process_output", identity(process_id="job", archive_id=ARCHIVE), context_={"snapshot_age": 46})
    case("process_stop/archive", "process_stop", identity(process_id="job", archive_id=ARCHIVE))
    case("process_output/archive-nil", "process_output", identity(process_id="job", archive_id=NIL))
    # context commands
    case("compact_context/extra-field", "compact_context", identity(path="/example"))
    case("clear_context/run-empty", "clear_context", identity(expected_run_id=""))
    dismiss_source = "test_bb_dismiss_prompt.DismissPromptTests"
    for label, value in (("true", True), ("false", False), ("null", None),
                         ("integer-zero", 0), ("integer-one", 1), ("string", "true"), ("array", [])):
        case("clear_context/dismiss-" + label, "clear_context", identity(dismiss_prompt=value), source=dismiss_source)
    for value in (True, False):
        case("compact_context/dismiss-" + str(value).lower(), "compact_context", identity(dismiss_prompt=value), source=dismiss_source)
    # lifecycle
    actions = "test_mobile_actions.LifecycleTransportTests.test_old_cli_and_invalid_payload_never_invoke_mutator"
    case("pause/action-field", "pause", identity(action="terminate"), source=actions)
    case("rename/missing-name", "rename", identity(), source=actions)
    case("rename/name-too-long", "rename", identity(new_name="a" * 513), source=actions)
    case("rename/name-padded", "rename", identity(new_name=" padded"))
    case("fork/tag-path", "fork", identity(tag="../invalid"), source=actions)
    case("restore/without-archive", "restore", identity(), source=actions)
    case("restore/archive-stale", "restore", context_={"snapshot_age": 46})
    case("forget/archive-nil", "forget", identity(archive_id=NIL))
    case("rename/archive", "rename", identity(new_name="codex/example/renamed", archive_id=ARCHIVE))
    # terminal
    terminal_source = "test_mobile_terminal.TerminalTransport.test_payload_is_literal_bounded_and_rejects_hidden_control"
    case("terminal_input/key", "terminal_input", identity(terminal_binding_id="d" * 64, key="Enter"))
    case("terminal_input/key-unsupported", "terminal_input", identity(terminal_binding_id="d" * 64, key="C-z"), source=terminal_source)
    case("terminal_input/escape", "terminal_input", identity(terminal_binding_id="d" * 64, text="\x1bsecret", enter=True), source=terminal_source)
    case("terminal_input/binding", "terminal_input", identity(terminal_binding_id="D" * 64, text="ls", enter=True))
    case("terminal_input/empty", "terminal_input", identity(terminal_binding_id="d" * 64, text="", enter=False))
    case("terminal_input/multiline", "terminal_input", identity(terminal_binding_id="d" * 64, text="a\nb\tc", enter=False))
    case("terminal_input/archive", "terminal_input", identity(terminal_binding_id="d" * 64, text="ls", enter=True, archive_id=ARCHIVE),
         source=terminal_source)
    case("terminal_input/action", "terminal_input", identity(terminal_binding_id="d" * 64, text="ls", enter=True, action="input"),
         source=terminal_source)
    case("terminal_snapshot/extra", "terminal_snapshot", identity(text="x"))
    # history
    history_source = "test_mobile_history_recovery.HistoryRecovery.test_history_is_read_only_scoped_and_old_nodes_deny"
    case("history/path", "history", identity(path="/private"), source=history_source)
    case("history/limit-bool", "history", identity(limit=True), source=history_source)
    case("history/limit-101", "history", identity(limit=101), source=history_source)
    case("history/two-selectors", "history", identity(before="cursor", after="cursor"), source=history_source)
    case("history/anchor-without-epoch", "history", identity(around_incoming_seq=1), source=history_source)
    case("history/anchor", "history", identity(around_incoming_seq=1, history_epoch="epoch"))
    case("history/agent-path", "history", identity(agent_id="../other"), source=history_source)
    case("history/archive", "history", identity(archive_id=ARCHIVE))
    # recovery
    case("recovery_action/action", "recovery_action", identity(job_id=ident("job"), action="later"))
    case("recovery_action/action-list", "recovery_action", identity(job_id=ident("job"), action=[]))
    case("recovery_action/job-not-uuid", "recovery_action", identity(job_id="job", action="now"))
    case("recovery_action/job-nil", "recovery_action", identity(job_id=NIL, action="now"))
    # mark_read
    read_source = "test_mobile_read.MarkReadValidation.test_relay_oracle_validates_the_same_envelope"
    case("mark_read/signature-uppercase", "mark_read", change=put(attention_signature="A" * 64), source=read_source)
    case("mark_read/null-marks", "mark_read", change=put(reply_id=None, attention_signature=None))
    case("mark_read/missing-reply", "mark_read", change=drop("reply_id"))
    case("mark_read/unknown-session", "mark_read", session="codex/example/other")
    case("archive_list/nonempty-session", "archive_list", session=SESSION)
    case("archive_list/too-large", "archive_list", change=put(limit=51))
    case("archive_list/invalid-range", "archive_list", change=put(**{"from":10,"until":10}))
    case("archive_list/extra", "archive_list", change=put(offset=1000))
    # file_read
    case("file_read/length-zero", "file_read", change=put(length=0))
    case("file_read/missing-path", "file_read", change=drop("path"))
    case("file_read/later-without-digest", "file_read", change=put(offset=1))
    case("file_read/later-with-digest", "file_read", change=put(offset=1, sha256=HASH_A))
    case("file_read/path-control", "file_read", change=put(path="/workspace/bad\nfile"))
    case("file_read/path-c1-control", "file_read", change=put(path="/workspace/bad\u0085file"))
    case("file_read/identity-byte-cap", "file_read", change=put(expected_run_id="界"*43))
    case("file_read/archive", "file_read", change=put(archive_id=ARCHIVE))
    case("file_read/agent-path", "file_read", change=put(agent_id="a/b"))
    # attachment_read
    case("attachment_read/length-zero", "attachment_read", change=put(length=0))
    case("attachment_read/beyond-file", "attachment_read", change=put(offset=10 * 1024 * 1024 - 1, length=2))
    case("attachment_read/message-uppercase", "attachment_read", change=put(message_id=ident("message").upper()))
    case("attachment_read/agent-path", "attachment_read", change=put(agent_id="a/b"))
    case("attachment_read/archive", "attachment_read", change=put(archive_id=ARCHIVE))
    case("attachment_read/index-bool", "attachment_read", change=put(index=False))
    # voice
    voice_source = "test_mobile_voice.VoiceValidationTests.test_routes_are_machine_identities_not_caller_aliases"
    case("voice_submit/machine-alias", "voice_submit", change=put(speech={"machine_id": "arch", "profile_id": "gigaam-ru"}), source=voice_source)
    case("voice_submit/profile-path", "voice_submit", change=put(speech={"machine_id": MACHINE, "profile_id": "../model"}), source=voice_source)
    case("voice_submit/route-extra", "voice_submit", change=put(speech={"machine_id": MACHINE, "profile_id": "ok", "command": "x"}),
         source=voice_source)
    case("voice_submit/revision", "voice_submit", change=put(speech={"machine_id": MACHINE, "profile_id": "gigaam-ru", "profile_revision": 7}))
    case("voice_submit/revision-bool", "voice_submit", change=put(speech={"machine_id": MACHINE, "profile_id": "gigaam-ru", "profile_revision": True}),
         source="test_mobile_speech_config.SpeechConfigValidationTests.test_voice_routes_preserve_optional_exact_profile_revision")
    case("voice_profiles/empty-conversation", "voice_profiles", change=put(expected_conversation_id=""))
    case("voice_status/extra-path", "voice_status", change=put(path="/example/private"),
         source="test_mobile_voice.VoiceValidationTests.test_operations_keep_session_and_exact_native_identity")
    # speech
    speech_source = "test_mobile_speech.SpeechValidationTests.test_invalid_audio_identity_and_extra_fields_are_rejected"
    case("speech_profiles/session", "speech_profiles", session=SESSION,
         source="test_mobile_speech.SpeechValidationTests.test_all_computer_operations_have_exact_shapes")
    case("speech_profiles/field", "speech_profiles", {"x": 1})
    case("speech_submit/sample-rate-bool", "speech_submit", change=lambda p: p.update(audio={**AUDIO, "sample_rate": True}), source=speech_source)
    case("speech_submit/one-byte", "speech_submit", change=lambda p: p.update(audio={**AUDIO, "data_base64": "AA=="}), source=speech_source)
    case("speech_submit/padding-inside", "speech_submit", change=lambda p: p.update(audio={**AUDIO, "data_base64": "AA==AA=="}), source=speech_source)
    case("speech_submit/space", "speech_submit", change=lambda p: p.update(audio={**AUDIO, "data_base64": "AA B"}), source=speech_source)
    case("speech_submit/odd-size", "speech_submit", change=lambda p: p.update(audio={**AUDIO, "data_base64": "AAAA"}), source=speech_source)
    case("speech_submit/profile-path", "speech_submit", change=put(profile_id="../provider"), source=speech_source)
    case("speech_submit/extra-path", "speech_submit", change=put(path="/etc/passwd"), source=speech_source)
    case("speech_status/job-not-uuid", "speech_status", {"job_id": "job"})
    case("speech_cancel/extra", "speech_cancel", {"job_id": ident("speech-job"), "x": 1})
    # speech configuration
    config_source = "test_mobile_speech_config.SpeechConfigValidationTests.test_plaintext_credentials_and_unbounded_envelopes_are_rejected"
    case("speech_config/session", "speech_config", session=SESSION,
         source="test_mobile_speech_config.SpeechConfigValidationTests.test_all_operations_are_computer_scoped_and_exact")
    case("speech_profile_upsert/plaintext", "speech_profile_upsert", change=put(credential={"mode": "replace", "value": "fixture-only-key"}), source=config_source)
    case("speech_profile_upsert/keep", "speech_profile_upsert", change=put(credential={"mode": "keep"}))
    case("speech_profile_upsert/short-nonce", "speech_profile_upsert",
         change=put(credential={"mode": "replace", "sealed_value": {**SEALED, "nonce": "AAAA"}}), source=config_source)
    case("speech_profile_upsert/insecure-string", "speech_profile_upsert",
         change=lambda p: p["profile"].update(allow_insecure_http="true"), source=config_source)
    case("speech_profile_upsert/profile-path", "speech_profile_upsert", change=lambda p: p["profile"].update(id="../example"), source=config_source)
    case("speech_profile_upsert/request-mismatch", "speech_profile_upsert", change=put(request_id=ident("other")), source=config_source)
    case("speech_profile_upsert/revision-bool", "speech_profile_upsert", change=put(expected_revision=True), source=config_source)
    case("speech_profile_upsert/revision-too-large", "speech_profile_upsert", change=put(expected_revision=2 ** 53), source=config_source)
    case("speech_profile_upsert/language", "speech_profile_upsert", change=lambda p: p["profile"].update(language="rus"))
    case("speech_profile_upsert/gigaam", "speech_profile_upsert", change=put(profile={"id": "local", "name": "Local", "adapter": "gigaam",
                                                                                      "model": "v3", "language": "", "device": "cpu"}))
    case("speech_profile_upsert/gigaam-device-list", "speech_profile_upsert",
         change=put(profile={"id": "local", "name": "Local", "adapter": "gigaam", "model": "v3", "language": "", "device": []}))
    case("speech_config_status/operation-not-uuid", "speech_config_status", {"operation_id": "op", "expected_machine_id": MACHINE})
    case("speech_profile_remove/machine-nil", "speech_profile_remove", change=put(expected_machine_id=NIL))
    # catalog, directories, launch and worktrees
    launch_source = "test_mobile_launch.LaunchTransport.test_preflight_old_cli_invalid_fields_and_wrong_provider_do_not_launch"
    model_source = "test_mobile_launch.LaunchTransport.test_model_route_is_bounded_and_specific_to_native_opencode"
    directory_source = "test_mobile_directory_create.DirectoryCreate.test_invalid_wire_fields_paths_and_uuid_are_rejected_without_native"
    case("catalog/session", "catalog", session=SESSION)
    case("dirs/home", "dirs", {"request_id": ID, "path": "~"})
    case("dirs/relative", "dirs", {"request_id": ID, "path": "relative"})
    for index, path in enumerate(("~", "~/new", "relative", "/", "/new/", "/new/.", "/new/..", "/new\nname", "/new\x7fname", "", None)):
        case("directory_create/path-%02d" % index, "directory_create", {"request_id": ID, "path": path}, source=directory_source)
    case("directory_create/4096-bytes", "directory_create", {"request_id": ID, "path": "/" + "\u754c" * 1365}, source=directory_source)
    case("directory_create/4097-bytes", "directory_create", {"request_id": ID, "path": "/" + "\u754c" * 1365 + "a"})
    case("directory_create/extra-field", "directory_create", {"request_id": ID, "path": "/example/new", "parents": True}, source=directory_source)
    case("directory_create/session", "directory_create", session="codex/example", source=directory_source)
    for index, values in enumerate(({"directory": "../relative"}, {"tag": "x/evil"}, {"tag": "x.y"}, {"agent": "shell"},
                                    {"command": "rm"}, {"account_id": "../secret"})):
        case("launch/invalid-%02d" % index, "launch", change=put(**values), source=launch_source)
    opencode = {"agent": "opencode", "account_id": "native-opencode", "model": "ollama/qwen3-coder:30b"}
    case("launch/opencode-model", "launch", change=put(**opencode), source=model_source)
    case("launch/opencode-model-256-bytes", "launch", change=put(**{**opencode, "model": "ollama/" + "\u754c" * 83}), source=model_source)
    for index, values in enumerate(({"agent": "codex"}, {"account_id": "native-codex"}, {"model": "local"}, {"model": "/local"},
                                    {"model": "ollama/"}, {"model": "ollama/with space"}, {"model": "ollama/" + "\u754c" * 84})):
        case("launch/model-invalid-%02d" % index, "launch", change=put(**{**opencode, **values}), source=model_source)
    case("launch/project", "launch", change=put(swarm_id="swarm", project_id="project", add_folder=True))
    case("launch/project-partial", "launch", change=put(project_id="project"))
    case("launch/project-folder-and-add", "launch", change=put(swarm_id="swarm", project_id="project", project_folder_id="f", add_folder=True))
    case("launch/worktree-placement", "launch", change=put(swarm_id="swarm", project_id="project", add_folder=False,
                                                            worktree_folder_id="anchor", worktree_common_dir="/example/repo/.git"))
    case("worktrees/relative", "worktrees", {"request_id": ID, "path": "repo"})
    case("worktree_create/branch-option", "worktree_create", change=put(branch="-x"))
    case("worktree_create/missing-base", "worktree_create", change=drop("base"))
    # computer-scoped delivery preference
    case("set_mobile_delivery/never", "set_mobile_delivery", {"value": "never"})
    case("set_mobile_delivery/session", "set_mobile_delivery", session=SESSION)
    case("set_mobile_delivery/extra", "set_mobile_delivery", {"value": "away", "x": 1})
    return cases


def claimed(submission: dict) -> dict:
    """The relay removes computer_id before delivering a request to its computer."""
    return {key: value for key, value in submission.items() if key != "computer_id"}


def connector_decision(case: dict) -> dict:
    ctx = case.get("context", DEFAULT_CONTEXT)
    request = claimed(case["submission"])
    with fixed_clock() as clock:
        try:
            connector.checked_uuid(request.get("request_id"))
        except ConnectorError as error:
            return {"decision": "reject", "stage": "before_journal", "error": str(error)}
        node = types.SimpleNamespace(sessions=set(ctx["sessions"]), archives={tuple(row) for row in ctx["archives"]},
                                     snapshot_seen_at=clock.mono - ctx["snapshot_age"])
        try:
            _, _, payload = Connector.validate(node, request)
        except ConnectorError as error:
            return flagged({"decision": "reject", "stage": "receipt", "error": str(error)}, str(error))
        except Exception as error:  # The connector records any other failure with a fixed text.
            message = "connector could not record native outcome"
            return flagged({"decision": "reject", "stage": "receipt", "error": message, "raised": type(error).__name__}, message)
    return {"decision": "accept", "native_payload": "absent" if payload is None else "unchanged" if payload == request.get("payload") else "changed"}


def live_oracle(submission: dict) -> dict:
    """The decision of the Python reference relay's submission validation,
    through its test-only copy in services/mobile/tests/relay_oracle.py."""
    import relay_oracle
    return relay_oracle.decision(submission)


VALIDATION_FAMILIES = {
    "conversation": {"inspect", "send", "answer", "interrupt", "send_now", "settings", "process_output", "process_stop",
                     "compact_context", "clear_context", "mark_read", "attachment_read", "file_read", "hooks_status"},
    "project": set(session_projects.OPERATIONS),
    "control": set(context.LIFECYCLE_OPERATIONS | context.TERMINAL_OPERATIONS | {"history", "recovery_action"}),
    "speech": set(voice.OPERATIONS | speech.OPERATIONS | speech_config.OPERATIONS),
    "launch": set(context.LAUNCH_OPERATIONS | context.COMPUTER_OPERATIONS),
}


def validation_file(case_id: str) -> str:
    parts = case_id.split("/")
    prefix = parts[1] if parts[0] == "valid" else parts[0]
    family = next((name for name, operations in VALIDATION_FAMILIES.items() if prefix in operations), "envelope")
    return "validation/%s.jsonl" % family


def frozen_column(directory: Path, column: str) -> dict:
    """One committed decision column: case id -> (submission text, decision)."""
    decisions = {}
    for path in sorted(directory.glob("validation/*.jsonl")):
        for row in map(json.loads, path.read_text().splitlines()):
            if row.get("kind") == "case" and column in row:
                decisions[row["id"]] = (compact(row["submission"]), row[column])
    return decisions


def frozen_oracle(directory: Path) -> dict:
    """The committed relay-oracle column."""
    return frozen_column(directory, "oracle")


def frozen_relay(directory: Path) -> dict:
    """The committed column of the Rust relay, which relay/tests/vectors.rs computes."""
    return frozen_column(directory, "relay")


def validation_vectors(oracle: dict | None, relay: dict) -> tuple[dict[str, bytes], set[str]]:
    header = {"kind": "context", "name": "default", "context": DEFAULT_CONTEXT,
              "note": "snapshot_age is seconds since the last local snapshot; the connector input is the submission without computer_id"}
    files: dict[str, list] = {}
    observed = set()
    missing = []
    for case in validation_cases():
        case["connector"] = connector_decision(case)
        if "error" in case["connector"]:
            observed.add(case["connector"]["error"])
        if oracle is None:
            case["oracle"] = live_oracle(case["submission"])
        elif oracle.get(case["id"], (None,))[0] == compact(case["submission"]):
            # A decision is reused only for the exact submission it was computed from.
            case["oracle"] = oracle[case["id"]][1]
        else:
            missing.append(case["id"])
        # A new or changed submission has no relay decision until
        # ZERUS_BLESS=1 cargo test --test vectors computes it.
        if relay.get(case["id"], (None,))[0] == compact(case["submission"]):
            case["relay"] = relay[case["id"]][1]
        files.setdefault(validation_file(case["id"]), [header]).append(case)
    if missing:
        raise CaptureError("validation cases without a frozen relay-oracle decision for their submission: "
                           + ", ".join(missing)
                           + "\nRun capture.py --refresh-oracle, or remove them.")
    return {name: jsonl(rows) for name, rows in files.items()}, observed


# ---------------------------------------------------------------------------
# Journal: Python-written fixtures, hand-written legacy fixtures and traces.

LEGACY_FIXTURE = """PRAGMA foreign_keys=OFF;
BEGIN TRANSACTION;
CREATE TABLE requests(request_id TEXT PRIMARY KEY,operation TEXT NOT NULL,state TEXT NOT NULL,response TEXT,delivered INTEGER NOT NULL DEFAULT 0);
INSERT INTO requests VALUES('%s','inspect','completed','{"state": "completed", "result": {"text": "legacy content"}, "error": null}',1);
INSERT INTO requests VALUES('%s','send','running',NULL,0);
INSERT INTO requests VALUES('%s','send','completed','{"state": "completed", "result": {"status": "submitted"}, "error": null}',0);
COMMIT;
""" % (ident("legacy/read"), ident("legacy/running-send"), ident("legacy/pending-send"))
NO_METADATA_FIXTURE = """PRAGMA foreign_keys=OFF;
BEGIN TRANSACTION;
CREATE TABLE requests (
            request_id TEXT PRIMARY KEY, operation TEXT NOT NULL,
            state TEXT NOT NULL, response TEXT, delivered INTEGER NOT NULL DEFAULT 0,
            request_hash TEXT, created_at REAL NOT NULL, updated_at REAL NOT NULL,
            delivered_at REAL, response_expired INTEGER NOT NULL DEFAULT 0
        );
INSERT INTO requests VALUES('%s','send','completed','{"state": "completed", "result": {"status": "submitted"}, "error": null}',1,'%s',%r,%r,%r,0);
COMMIT;
""" % (ident("no-metadata/send"), HASH_A, EPOCH - 60, EPOCH - 60, EPOCH - 50)
FOREIGN_FIXTURE = """PRAGMA foreign_keys=OFF;
BEGIN TRANSACTION;
CREATE TABLE metadata (key TEXT PRIMARY KEY, value TEXT NOT NULL);
INSERT INTO metadata VALUES('relay_binding','%s');
CREATE TABLE requests (
            request_id TEXT PRIMARY KEY, operation TEXT NOT NULL,
            state TEXT NOT NULL, response TEXT, delivered INTEGER NOT NULL DEFAULT 0,
            request_hash TEXT, created_at REAL NOT NULL, updated_at REAL NOT NULL,
            delivered_at REAL, response_expired INTEGER NOT NULL DEFAULT 0
        );
INSERT INTO requests VALUES('%s','send','running',NULL,0,'%s',%r,%r,NULL,0);
COMMIT;
""" % (hashlib.sha256(b"https://other.example.test\0fixture-other-token").hexdigest(), ident("foreign/send"), HASH_A,
       EPOCH - 60, EPOCH - 60)
# 5,000 retained reads and 99,999 retained mutations: one more mutation fits.
CAPS_FIXTURE = """PRAGMA foreign_keys=OFF;
BEGIN TRANSACTION;
CREATE TABLE metadata (key TEXT PRIMARY KEY, value TEXT NOT NULL);
INSERT INTO metadata VALUES('relay_binding','%s');
CREATE TABLE requests (
            request_id TEXT PRIMARY KEY, operation TEXT NOT NULL,
            state TEXT NOT NULL, response TEXT, delivered INTEGER NOT NULL DEFAULT 0,
            request_hash TEXT, created_at REAL NOT NULL, updated_at REAL NOT NULL,
            delivered_at REAL, response_expired INTEGER NOT NULL DEFAULT 0
        );
WITH RECURSIVE n(i) AS (SELECT 1 UNION ALL SELECT i+1 FROM n WHERE i<5000)
INSERT INTO requests SELECT printf('00000000-0000-4000-8000-%%012d',i),'inspect','completed','{"state": "completed", "result": {}, "error": null}',1,NULL,%r,%r,%r,0 FROM n;
WITH RECURSIVE n(i) AS (SELECT 1 UNION ALL SELECT i+1 FROM n WHERE i<99999)
INSERT INTO requests SELECT printf('00000000-0000-4000-9000-%%012d',i),'send','completed','{"state": "completed", "result": {}, "error": null}',1,NULL,%r,%r,%r,0 FROM n;
COMMIT;
""" % (BINDING, EPOCH - 10, EPOCH - 10, EPOCH - 10, EPOCH - 10, EPOCH - 10, EPOCH - 10)
HAND_FIXTURES = {"journal/legacy-5-column.sql": LEGACY_FIXTURE, "journal/no-metadata.sql": NO_METADATA_FIXTURE,
                 "journal/foreign-binding.sql": FOREIGN_FIXTURE, "journal/caps.sql": CAPS_FIXTURE}


def sql_literal(value) -> str:
    if value is None:
        return "NULL"
    if isinstance(value, bool):
        raise TypeError("journal values are never booleans")
    if isinstance(value, int):
        return str(value)
    if isinstance(value, float):
        text = repr(value)
        if not any(char in text for char in ".e") or value != value:
            raise ValueError("unexpected REAL value")
        return text
    if isinstance(value, str) and value.isascii() and value.isprintable():
        return "'" + value.replace("'", "''") + "'"
    raise ValueError("unexpected journal value")


def dump(path: Path) -> str:
    """A deterministic sqlite3 .dump: tables with rows in rowid order, then indexes."""
    db = connect_ro(path)
    try:
        objects = db.execute("SELECT type,name,sql FROM sqlite_master WHERE sql IS NOT NULL ORDER BY rowid").fetchall()
        lines = ["PRAGMA foreign_keys=OFF;", "BEGIN TRANSACTION;"]
        for kind, name, sql in objects:
            if kind == "table":
                lines.append(sql + ";")
                for row in db.execute('SELECT * FROM "%s" ORDER BY rowid' % name):
                    lines.append("INSERT INTO %s VALUES(%s);" % (name, ",".join(map(sql_literal, row))))
        lines += [sql + ";" for kind, name, sql in objects if kind != "table"]
        lines.append("COMMIT;")
        return "\n".join(lines) + "\n"
    finally:
        db.close()


def load_fixture(directory: Path, text: str) -> None:
    """A fixture becomes journal.sqlite3, pre-created 0600 like the connector's own file."""
    directory.mkdir(mode=0o700, exist_ok=True)
    path = directory / "journal.sqlite3"
    os.close(os.open(path, os.O_CREAT | os.O_EXCL | os.O_RDWR, 0o600))
    db = sqlite3.connect(path)
    try:
        db.executescript(text)
    finally:
        db.close()


def schema(path: Path, journal: Journal | None = None) -> dict:
    db = connect_ro(path)
    try:
        master = [{"type": kind, "name": name, "tbl_name": table, "sql": sql} for kind, name, table, sql in
                  db.execute("SELECT type,name,tbl_name,sql FROM sqlite_master ORDER BY rowid")]
        tables = [row["name"] for row in master if row["type"] == "table"]
        result = {
            "sqlite_master": master,
            "table_info": {name: [list(row) for row in db.execute('PRAGMA table_info("%s")' % name)] for name in tables},
            "index_list": {name: [list(row) for row in db.execute('PRAGMA index_list("%s")' % name)] for name in tables},
            "index_xinfo": {row["name"]: [list(item) for item in db.execute('PRAGMA index_xinfo("%s")' % row["name"])]
                            for row in master if row["type"] == "index"},
            "pragmas": {name: db.execute("PRAGMA " + name).fetchone()[0]
                        for name in ("user_version", "application_id", "journal_mode", "auto_vacuum", "encoding", "page_size")},
        }
    finally:
        db.close()
    if journal is not None:
        result["connection_pragmas"] = {name: journal.db.execute("PRAGMA " + name).fetchone()[0]
                                        for name in ("synchronous", "secure_delete", "busy_timeout", "foreign_keys")}
    return result


def table_rows(path: Path) -> list[dict]:
    db = connect_ro(path)
    try:
        cursor = db.execute("SELECT * FROM requests ORDER BY request_id")
        names = [column[0] for column in cursor.description]
        return [dict(zip(names, row)) for row in cursor]
    finally:
        db.close()


class JournalRun:
    """Runs one scenario of real Journal calls under a fixed clock."""

    def __init__(self, root: Path):
        self.root, self.state = root, root / "state"
        self.journals: dict[str, Journal] = {}
        self.now = EPOCH

    def close(self):
        for journal in self.journals.values():
            journal.close()
        self.journals.clear()

    def step(self, step: dict) -> dict:
        self.now = step.get("at", self.now)
        op, name = step["op"], step.get("instance", "a")
        journal = self.journals.get(name)
        with fixed_clock(self.now):
            try:
                if op == "setup":
                    setup_paths(self.root, step["actions"])
                    return {}
                if op == "load":
                    load_fixture(self.state, step["sql"])
                    return {}
                if op == "open":
                    self.journals[name] = Journal(self.state, step["binding"])
                    return {"ok": True}
                if op == "close":
                    self.journals.pop(name).close()
                    return {}
                if op == "claim":
                    return {"returns": journal.claim(step["request_id"], step["operation"], step.get("request_hash"))}
                if op == "finish":
                    journal.finish(step["request_id"], step["response"])
                    return {}
                if op == "replay":
                    return {"returns": journal.replay(step["request_id"])}
                if op == "pending":
                    return {"returns": sorted([list(item) for item in journal.pending()], key=lambda item: item[0]),
                            "order": "unspecified"}
                if op == "delivered":
                    journal.delivered(step["request_id"])
                    return {}
                if op == "prune":
                    journal.prune()
                    return {}
                if op == "rows":
                    return {"returns": table_rows(self.state / "journal.sqlite3")}
                if op == "schema":
                    return {"returns": schema(self.state / "journal.sqlite3", journal)}
                if op == "files":
                    return {"returns": tree(self.root)}
                if op == "sql":
                    db = sqlite3.connect(self.state / "journal.sqlite3", isolation_level=None)
                    try:
                        db.execute(step["sql"])
                    finally:
                        db.close()
                    return {}
            except (ConnectorError, OSError, sqlite3.Error) as error:
                return raised(error)
        raise ValueError("unknown journal step " + op)


def completed(**result):
    return {"state": "completed", "result": result, "error": None}


def lifecycle(request_id, operation, at, *, request_hash=HASH_A, response=None, delivered=True):
    """Claim, finish and (optionally) deliver one request at one instant."""
    steps = [{"op": "claim", "at": at, "request_id": request_id, "operation": operation, "request_hash": request_hash},
             {"op": "finish", "request_id": request_id, "response": response or completed(status="fixture")}]
    if delivered:
        steps.append({"op": "delivered", "request_id": request_id})
    return steps


DAY = 86400.0
RETENTION_ROWS = [  # (label, operation, seconds before EPOCH that the row was delivered)
    ("bogus-7d+1", "bogus_operation", 7 * DAY + 1), ("invalid-7d+1", "invalid", 7 * DAY + 1),
    ("send-7d+0.5", "send", 7 * DAY + 0.5), ("send-7d", "send", 7 * DAY), ("send-7d-0.5", "send", 7 * DAY - 0.5),
    ("pause-7d+0.5", "pause", 7 * DAY + 0.5), ("speech_profile_upsert-7d+0.5", "speech_profile_upsert", 7 * DAY + 0.5),
    ("history-3601", "history", 3601), ("inspect-3601", "inspect", 3601), ("inspect-3600", "inspect", 3600),
    ("inspect-3599", "inspect", 3599), ("mark_read-3599", "mark_read", 3599), ("dirs-3601", "dirs", 3601),
    ("terminal_snapshot-121", "terminal_snapshot", 121), ("terminal_snapshot-120", "terminal_snapshot", 120),
    ("attachment_read-121", "attachment_read", 121), ("speech_status-121", "speech_status", 121),
    ("voice_status-121", "voice_status", 121), ("speech_config_status-121", "speech_config_status", 121),
    ("speech_profiles-119", "speech_profiles", 119), ("terminal_snapshot-119", "terminal_snapshot", 119),
    ("attachment_read-119", "attachment_read", 119),
]


def rid_of(label):
    return ident("journal/" + label)


def journal_scenarios():
    """(trace id, steps, fixture dumped after the steps or None)."""
    fresh = [{"op": "open", "binding": BINDING}, {"op": "schema"}, {"op": "files"}, {"op": "close"}, {"op": "files"}]
    running = [{"op": "open", "at": EPOCH - 100, "binding": BINDING}]
    for label, operation in (("read", "inspect"), ("mutation", "send"), ("unknown", "bogus_operation"), ("invalid", "invalid"),
                             ("speech-config-mutation", "speech_profile_upsert"), ("attachment", "attachment_read")):
        running.append({"op": "claim", "request_id": rid_of("running/" + label), "operation": operation, "request_hash": HASH_A})
    running.append({"op": "close"})
    # The writer's clock only moves forward. Rows stay unpruned until the reopen at EPOCH.
    writes = [(EPOCH - age, lifecycle(rid_of(label), operation, EPOCH - age,
                                      response=completed(text="private native content " + label)))
              for label, operation, age in RETENTION_ROWS]
    writes += [(EPOCH - 9 * DAY + 1, lifecycle(rid_of("pending-send"), "send", EPOCH - 9 * DAY + 1, delivered=False)),
               (EPOCH - 9 * DAY + 2, lifecycle(rid_of("pending-inspect"), "inspect", EPOCH - 9 * DAY + 2, delivered=False)),
               (EPOCH - 200, lifecycle(rid_of("null-hash-send"), "send", EPOCH - 200, request_hash=None))]
    retention = [{"op": "open", "at": EPOCH - 9 * DAY, "binding": BINDING}]
    for _, steps in sorted(writes, key=lambda item: item[0]):
        retention += steps
    retention.append({"op": "close"})
    traces = [("python-fresh", fresh, "journal/python-fresh.sql"),
              ("python-running", running, "journal/python-running.sql"),
              ("python-retention", retention, "journal/python-retention.sql")]
    one = rid_of("trace/one")
    traces.append(("claim-replay-conflict", [
        {"op": "open", "binding": BINDING},
        {"op": "claim", "request_id": one, "operation": "send", "request_hash": HASH_A},
        {"op": "claim", "request_id": one, "operation": "send", "request_hash": HASH_A},
        {"op": "replay", "request_id": one},
        {"op": "claim", "request_id": one, "operation": "send", "request_hash": HASH_B},
        {"op": "claim", "request_id": one, "operation": "pause", "request_hash": HASH_A},
        {"op": "claim", "request_id": one, "operation": "send"},
        {"op": "finish", "at": EPOCH + 1, "request_id": one, "response": completed(status="submitted", text="caf\u00e9 \U0001f600")},
        {"op": "pending"}, {"op": "rows"},
        {"op": "delivered", "at": EPOCH + 2, "request_id": one}, {"op": "pending"},
        {"op": "replay", "at": EPOCH + 3, "request_id": one}, {"op": "rows"}, {"op": "pending"},
        {"op": "delivered", "at": EPOCH + 4, "request_id": one},
        {"op": "finish", "at": EPOCH + 5, "request_id": one, "response": {"state": "uncertain", "result": None, "error": "native command timed out"}},
        {"op": "rows"}, {"op": "close"}, {"op": "open", "at": EPOCH + 6, "binding": BINDING},
        {"op": "claim", "request_id": one, "operation": "send", "request_hash": HASH_A}, {"op": "rows"}, {"op": "close"},
    ], None))
    reads = [{"op": "open", "binding": BINDING}]
    for index in range(3):
        reads.append({"op": "claim", "request_id": rid_of("trace/read-%d" % index), "operation": "terminal_snapshot", "request_hash": HASH_A})
    reads += [{"op": "sql", "sql": "UPDATE requests SET state='completed',response='{}',delivered=1,delivered_at=%r" % (EPOCH - 121)},
              {"op": "prune", "at": EPOCH}, {"op": "rows"},
              {"op": "claim", "request_id": rid_of("trace/read-0"), "operation": "terminal_snapshot", "request_hash": HASH_B},
              {"op": "rows"}, {"op": "close"}]
    traces.append(("pruned-read-executes-again", reads, None))
    traces.append(("second-instance", [
        {"op": "open", "binding": BINDING}, {"op": "open", "instance": "b", "binding": BINDING},
        {"op": "close"}, {"op": "open", "instance": "b", "binding": BINDING}, {"op": "close", "instance": "b"}], None))
    traces.append(("foreign-credentials", [
        {"op": "open", "binding": BINDING}, {"op": "close"}, {"op": "open", "binding": "other-binding"}, {"op": "files"}], None))
    filesystem = [
        ("fs-group-writable-directory", [{"action": "mkdir", "path": "state", "mode": "0770"}]),
        ("fs-world-readable-directory", [{"action": "mkdir", "path": "state", "mode": "0755"}]),
        ("fs-directory-symlink", [{"action": "mkdir", "path": "private", "mode": "0700"},
                                  {"action": "symlink", "path": "state", "target": "private"}]),
        ("fs-journal-symlink", [{"action": "mkdir", "path": "state", "mode": "0700"},
                                {"action": "write", "path": "state/target.sqlite3", "mode": "0600"},
                                {"action": "symlink", "path": "state/journal.sqlite3", "target": "target.sqlite3"}]),
        ("fs-journal-hard-link", [{"action": "mkdir", "path": "state", "mode": "0700"},
                                  {"action": "write", "path": "state/other", "mode": "0600"},
                                  {"action": "hardlink", "path": "state/journal.sqlite3", "target": "state/other"}]),
        ("fs-journal-readable", [{"action": "mkdir", "path": "state", "mode": "0700"},
                                 {"action": "write", "path": "state/journal.sqlite3", "mode": "0644"}]),
        ("fs-lock-symlink", [{"action": "mkdir", "path": "state", "mode": "0700"},
                             {"action": "write", "path": "state/target", "mode": "0600"},
                             {"action": "symlink", "path": "state/connector.lock", "target": "target"}]),
        ("fs-lock-readable", [{"action": "mkdir", "path": "state", "mode": "0700"},
                              {"action": "write", "path": "state/connector.lock", "mode": "0640"}]),
        ("fs-wal-dangling-symlink", [{"action": "mkdir", "path": "state", "mode": "0700"},
                                     {"action": "symlink", "path": "state/journal.sqlite3-wal", "target": "missing"}]),
        ("fs-rollback-journal-readable", [{"action": "mkdir", "path": "state", "mode": "0700"},
                                          {"action": "write", "path": "state/journal.sqlite3-journal", "mode": "0644"}]),
        ("fs-shm-private", [{"action": "mkdir", "path": "state", "mode": "0700"},
                            {"action": "write", "path": "state/journal.sqlite3-shm", "mode": "0600"}]),
    ]
    for name, actions in filesystem:
        traces.append((name, [{"op": "setup", "actions": actions}, {"op": "open", "binding": BINDING}, {"op": "files"}], None))
    return traces


def expectations(fixtures: dict[str, str]):
    """(id, fixture, steps) for reopening each fixture with the Python Journal."""
    retention = [{"op": "open", "binding": BINDING}, {"op": "rows"}, {"op": "pending"}]
    for label, operation, _ in RETENTION_ROWS:
        retention.append({"op": "claim", "request_id": rid_of(label), "operation": operation, "request_hash": HASH_A})
    retention += [{"op": "replay", "request_id": rid_of("send-7d+0.5")}, {"op": "pending"},
                  {"op": "claim", "request_id": rid_of("send-7d+0.5"), "operation": "send", "request_hash": HASH_B},
                  {"op": "claim", "request_id": rid_of("null-hash-send"), "operation": "send", "request_hash": HASH_A},
                  {"op": "claim", "request_id": rid_of("null-hash-send"), "operation": "send"},
                  {"op": "rows"}, {"op": "close"}]
    legacy = [{"op": "schema"}, {"op": "rows"}, {"op": "open", "at": EPOCH, "binding": BINDING}, {"op": "schema"},
              {"op": "rows"}, {"op": "pending"},
              {"op": "claim", "request_id": ident("legacy/read"), "operation": "inspect"},
              {"op": "claim", "request_id": ident("legacy/read"), "operation": "inspect", "request_hash": HASH_A},
              {"op": "claim", "request_id": ident("legacy/running-send"), "operation": "send", "request_hash": HASH_A},
              {"op": "close"}, {"op": "open", "at": EPOCH + 9 * DAY, "binding": BINDING}, {"op": "rows"}, {"op": "close"}]
    return [
        ("python-fresh-reopen", "journal/python-fresh.sql",
         [{"op": "open", "at": EPOCH + 1, "binding": BINDING}, {"op": "schema"}, {"op": "rows"}, {"op": "close"}]),
        ("python-running-recovery", "journal/python-running.sql",
         [{"op": "open", "at": EPOCH + 10, "binding": BINDING}, {"op": "rows"}, {"op": "pending"},
          {"op": "claim", "request_id": rid_of("running/mutation"), "operation": "send", "request_hash": HASH_A},
          {"op": "replay", "request_id": rid_of("running/mutation")}, {"op": "close"}]),
        ("python-retention-prune", "journal/python-retention.sql", retention),
        ("legacy-5-column-upgrade", "journal/legacy-5-column.sql", legacy),
        ("no-metadata-adopts-binding", "journal/no-metadata.sql",
         [{"op": "open", "binding": BINDING}, {"op": "schema"}, {"op": "rows"}, {"op": "close"},
          {"op": "open", "binding": "other-binding"}]),
        ("foreign-binding-refused", "journal/foreign-binding.sql",
         [{"op": "open", "binding": BINDING}, {"op": "schema"}, {"op": "rows"}]),
        ("caps", "journal/caps.sql",
         [{"op": "open", "binding": BINDING},
          {"op": "claim", "request_id": ident("caps/read"), "operation": "inspect", "request_hash": HASH_A},
          {"op": "claim", "request_id": ident("caps/speech-read"), "operation": "speech_status", "request_hash": HASH_A},
          {"op": "claim", "request_id": ident("caps/mutation"), "operation": "send", "request_hash": HASH_A},
          {"op": "claim", "request_id": ident("caps/second-mutation"), "operation": "bogus_operation", "request_hash": HASH_A},
          {"op": "close"}]),
    ]


def run_steps(steps, *, fixture_text: str | None = None, dump_to: str | None = None):
    with scratch() as root:
        run = JournalRun(root)
        recorded = []
        try:
            if fixture_text is not None:
                load_fixture(run.state, fixture_text)
            for step in steps:
                recorded.append({**step, "expect": run.step(step)})
        finally:
            run.close()
        dumped = dump(run.state / "journal.sqlite3") if dump_to else None
    return recorded, dumped


def journal_vectors() -> dict[str, bytes]:
    files: dict[str, bytes] = {}
    fixtures = dict(HAND_FIXTURES)
    traces = []
    for name, steps, dump_to in journal_scenarios():
        recorded, dumped = run_steps(steps, dump_to=dump_to)
        record = {"id": name, "steps": recorded}
        if dump_to:
            record["writes_fixture"] = dump_to
            fixtures[dump_to] = dumped
        traces.append(record)
    expected = []
    for name, fixture, steps in expectations(fixtures):
        recorded, _ = run_steps(steps, fixture_text=fixtures[fixture])
        expected.append({"id": name, "fixture": fixture, "steps": recorded})
    files["journal_traces.jsonl"] = jsonl(traces)
    files["journal_expect.json"] = document({
        "provenance": "Each fixture is loaded into a new 0600 journal.sqlite3 in a private 0700 state directory, "
                      "then the Python Journal runs the steps; see README.md for the step vocabulary",
        "binding": BINDING, "epoch": EPOCH, "scenarios": expected})
    for name, text in fixtures.items():
        files[name] = text.encode()
    return files


# ---------------------------------------------------------------------------
# Receipts and native projections through the real execute().

def session_request(operation, payload, name, *, session=SESSION, **extra):
    rid = ident("execute/" + name)
    return {"request_id": rid, "operation": operation, "session": session, "payload": substitute(payload, rid), **extra}


def capable(node: Connector) -> None:
    for flag in ("lifecycle", "terminal", "recovery", "history", "launch", "project_launch", "worktree", "directory_create",
                 "read", "preference", "attachment_read", "file_read", "speech", "speech_config", "voice", "session_projects"):
        setattr(node, flag + "_supported", True)
    node.sessions = {SESSION, DSH_SESSION}
    node.archives = {(SESSION, ARCHIVE)}
    node.snapshot_seen_at = MONOTONIC


def execute_cases():
    """(id, request, native answers, note) run through Connector.execute."""
    valid = valid_payloads()
    process = {"id": "job", "run_id": RUN, "conversation_id": CONVERSATION, "output": "line\n", "private": "kept"}
    big = {**process, "output": "\u00e9" * 180000}
    cases = [
        ("send-completed", session_request("send", valid["send"], "send-completed"),
         {("send", SESSION, "--json"): {"status": "submitted", "request_id": "native"}}),
        ("send-native-timeout", session_request("send", valid["send"], "send-timeout"),
         {("send", SESSION, "--json"): ConnectorError("native command timed out")}),
        ("send-native-crash", session_request("send", valid["send"], "send-crash"),
         {("send", SESSION, "--json"): RuntimeError("fixture")}),
        ("inspect-native-crash", session_request("inspect", {}, "inspect-crash"),
         {("inspect", SESSION): RuntimeError("fixture")}),
        ("inspect-completed", session_request("inspect", {}, "inspect-completed"),
         {("inspect", SESSION): {"name": SESSION, "run_id": RUN, "conversation_id": CONVERSATION, "events": []}}),
        ("process-output", session_request("process_output", valid["process_output"], "process-output"),
         {("processes", SESSION, "--output", "job", "--run", RUN, "--conversation", CONVERSATION): process}),
        ("process-output-too-large", session_request("process_output", valid["process_output"], "process-output-large"),
         {("processes", SESSION, "--output", "job", "--run", RUN, "--conversation", CONVERSATION): big}),
        ("pause-native-failed", session_request("pause", valid["pause"], "pause-failed"),
         {("session-action", SESSION, "--json"): "$ack:failed"}),
        ("restore-archive", session_request("restore", valid["restore"], "restore"),
         {("session-action", SESSION, "--json"): "$ack:completed"}),
        ("terminal-snapshot", session_request("terminal_snapshot", valid["terminal_snapshot"], "terminal-snapshot"),
         {("terminal", SESSION, "--json"): "$terminal:snapshot"}),
        ("terminal-input-expired", session_request("terminal_input", valid["terminal_input"], "terminal-expired", expires_at=EPOCH),
         {}),
        ("terminal-input-submitted", session_request("terminal_input", valid["terminal_input"], "terminal-input", expires_at=EPOCH + 5),
         {("terminal", SESSION, "--json"): ["$terminal:fresh", "$terminal:submitted"]}),
        ("mark-read-not-found", session_request("mark_read", valid["mark_read"], "mark-read"),
         {("read", SESSION, "--json"): {"ok": False, "error": "not_found"}}),
        ("speech-submit-drops-text", session_request("speech_submit", valid["speech_submit"], "speech-submit", session=""),
         {("speech", "--json"): "$speech"}),
        ("set-mobile-delivery", session_request("set_mobile_delivery", valid["set_mobile_delivery"], "delivery", session=""),
         {("swarm", "preference", "set", "mobile_delivery", "away_5", "--json"): {"ok": True, "mobile_delivery": "away_5"}}),
        ("dirs-projection", session_request("dirs", valid["dirs"], "dirs", session=""),
         {("dirs", "/example"): {"path": "/example", "parent": "/", "home": "/example/home", "truncated": False,
                                 "directories": [{"name": "repo", "path": "/example/repo"}], "private": "dropped"}}),
        ("worktrees-state-prefix", session_request("worktrees", valid["worktrees"], "worktrees", session=""),
         {("__state", "worktrees", "--path", "/example/repo", "--refresh", "--json"):
          {"state": "ok", "path": "/example/repo", "common_dir": "/example/repo/.git", "stale": False, "private": "dropped",
           "worktrees": [{"path": "/example/repo", "kind": "main", "available": True, "branch": "main", "head": "abc"}]}}),
        ("catalog-projection", session_request("catalog", valid["catalog"], "catalog", session=""),
         {("account", "ls"): LAUNCH_ACCOUNTS}),
        ("unsupported-operation", session_request("bogus_operation", {}, "unsupported"), {}),
        ("attachment-read-unavailable", session_request("attachment_read", valid["attachment_read"], "attachment-unavailable"),
         "$no-attachment-abi"),
        ("attachment-read-missing-file", session_request("attachment_read", valid["attachment_read"], "attachment-missing"),
         {("inspect", SESSION, "--skip-processes"): {"name": SESSION, "run_id": RUN, "conversation_id": CONVERSATION},
          ("attachment", SESSION, "--request", ident("message"), "--index", "0", "--conversation", CONVERSATION):
          ConnectorError("native command failed")}),
        ("attachment-read-chunk", session_request("attachment_read", {**valid["attachment_read"], "length": 4}, "attachment-chunk"),
         {("inspect", SESSION, "--skip-processes"): {"name": SESSION, "run_id": RUN, "conversation_id": CONVERSATION},
          ("attachment", SESSION, "--request", ident("message"), "--index", "0", "--conversation", CONVERSATION):
          {"request_id": ident("message"), "index": 0, "name": "note.txt", "mime": "", "bytes": 6,
           "data_base64": base64.b64encode(b"abcdef").decode()}}),
        ("gateway-route-unavailable", session_request("inspect", {}, "gateway-unavailable",
                                                      gateway_route={"schema": 1, "route_id": None, "computer_id": COMPUTER,
                                                                     "machine_id": MACHINE}), {}),
        ("gateway-route-invalid", session_request("inspect", {}, "gateway-invalid", gateway_route={"schema": 2}), {}),
        ("directory-create-route-mismatch", session_request("directory_create", valid["directory_create"], "directory-route", session="",
                                                            computer_id=COMPUTER, gateway_route={"computer_id": ident("other")}), {}),
    ]
    for operation in sorted(session_projects.OPERATIONS):
        cases.append((operation+"-confirmed",session_request(operation,valid[operation],operation+"-confirmed"),
            {("swarm","assign-session" if operation=="project_assign" else "session-project","--json"): "$project"}))
    cases.append(("file-read-snapshot", session_request("file_read", valid["file_read"], "file-read-snapshot"),
                  {("file-read", SESSION, "--json"): "$file:snapshot"}))
    return cases


def resolve_answer(answer, request):
    payload = request["payload"]
    if answer == "$ack:failed":
        return {"request_id": request["request_id"], **{"name": SESSION, "run_id": RUN, "conversation_id": CONVERSATION},
                "status": "failed", "error": "Synthetic native refusal"}
    if answer == "$ack:completed":
        return {"request_id": request["request_id"], "name": SESSION, "run_id": RUN, "conversation_id": CONVERSATION,
                "status": "completed", "result_target": {"name": SESSION, "run_id": "restored-run", "conversation_id": CONVERSATION}}
    if answer == "$terminal:snapshot":
        return {"request_id": request["request_id"], "name": SESSION, "run_id": RUN, "conversation_id": CONVERSATION,
                "status": "snapshot", "lines": ["$ ls"]}
    if answer == "$terminal:fresh":
        return {"name": SESSION, "run_id": RUN, "conversation_id": CONVERSATION, "terminal_binding_id": payload["terminal_binding_id"],
                "input_supported": True, "status": "snapshot"}
    if answer == "$terminal:submitted":
        return {"request_id": request["request_id"], "name": SESSION, "run_id": RUN, "conversation_id": CONVERSATION,
                "terminal_binding_id": payload["terminal_binding_id"], "status": "submitted"}
    if answer == "$file:snapshot":
        data=b"fixture spreadsheet"
        path=payload["path"]
        return {"request_id":request["request_id"],"name":SESSION,"run_id":RUN,"conversation_id":CONVERSATION,
                "requested_path":path,"path":path,"file_name":path.rsplit("/",1)[-1],"mime":"application/octet-stream",
                "bytes":len(data),"sha256":hashlib.sha256(data).hexdigest(),"data_base64":base64.b64encode(data).decode()}
    if answer == "$project":
        mutation=request["operation"]=="project_assign"
        raw={"schema":1,"request_id":request["request_id"],"status":"assigned" if mutation else "ok","name":SESSION,"run_id":RUN,"conversation_id":CONVERSATION,
             "machine_id":payload["expected_machine_id"],"swarm_id":ident("project-swarm"),"membership_token":"a"*64,"catalog_epoch":"b"*64,"current_project_id":"logical"}
        if mutation:raw["project_id"]=payload["project_id"]
        else:raw.update(projects=[{"id":"logical","name":"Logical grouping","color":"#123456"}],next_cursor=None)
        return raw
    if answer == "$speech":
        return {"schema": 1, "job_id": request["request_id"], "state": "completed", "text": "private transcript"}
    return answer


async def execute_runs(root: Path) -> list[dict]:
    with fixed_clock():
        node = make_connector(root)
    rows = []
    try:
        for name, request, answers in execute_cases():
            capable(node)
            if answers == "$no-attachment-abi":
                node.attachment_read_supported = False
                answers = {}
            node._native = Native({key: (([resolve_answer(item, request) for item in value]) if isinstance(value, list)
                                         else resolve_answer(value, request)) for key, value in answers.items()})
            node.attachment_reads = attachment_reads.AttachmentReads(clock=lambda: MONOTONIC)
            row = {"kind": "execute", "id": name, "request": request,
                   "native": [{"argv": list(key), "answer": raised(value) if isinstance(value, BaseException) else copy.deepcopy(value)}
                              for key, value in sorted(node._native.answers.items())]}
            if not node.attachment_read_supported:
                # Every other capability of `capable`, without the attachment marker.
                row["unavailable"] = ["attachment_read"]
            if name == "process-output-too-large":
                row["native"] = [{"argv": item["argv"], "answer": {**item["answer"], "output": "@fill"},
                                  "fill": {"@fill": [["\u00e9", 180000]]}} for item in row["native"]]
            with fixed_clock():
                try:
                    response = await node.execute(copy.deepcopy(request))
                except ConnectorError as error:
                    row["expect"] = {"rejected": "before_journal", "error": str(error)}
                    rows.append(row)
                    continue
            state, stored = node.journal.db.execute("SELECT state,response FROM requests WHERE request_id=?",
                                                    (request["request_id"],)).fetchone()
            if json.loads(stored) != response:
                raise RuntimeError("journal and returned receipt differ")
            row["expect"] = flagged({"state": state, "calls": node._native.calls}, response.get("error"))
            if len(stored) > 4096:
                row["expect"].update(stored_bytes=len(stored.encode()), stored_sha256=hashlib.sha256(stored.encode()).hexdigest())
            else:
                row["expect"]["stored"] = stored
            rows.append(row)
        return rows
    finally:
        await node.accounts.close()
        node.journal.close()


def receipt_vectors(executions: list[dict]) -> bytes:
    """Fixed receipt texts exactly as the Python connector stores and uploads them."""
    texts = {}

    def journal_text(steps, request_id):
        recorded, _ = run_steps(steps + [{"op": "rows"}])
        for row in recorded[-1]["expect"]["returns"]:
            if row["request_id"] == request_id:
                return row["response"]
        raise RuntimeError("missing journal row")

    mutation, read = rid_of("receipt/mutation"), rid_of("receipt/read")
    crash = [{"op": "open", "binding": BINDING},
             {"op": "claim", "request_id": mutation, "operation": "send", "request_hash": HASH_A},
             {"op": "claim", "request_id": read, "operation": "inspect", "request_hash": HASH_A},
             {"op": "close"}, {"op": "open", "at": EPOCH + 1, "binding": BINDING}]
    texts["restart_recovered_mutation"] = journal_text(crash, mutation)
    texts["restart_recovered_read"] = journal_text(crash, read)
    expired = [{"op": "open", "at": EPOCH - 8 * DAY, "binding": BINDING}, *lifecycle(mutation, "send", EPOCH - 8 * DAY),
               {"op": "prune", "at": EPOCH}]
    texts["history_expired_tombstone"] = journal_text(expired, mutation)
    in_progress, _ = run_steps([{"op": "open", "binding": BINDING},
                                {"op": "claim", "request_id": mutation, "operation": "send", "request_hash": HASH_A},
                                {"op": "replay", "request_id": mutation}])
    # Returned to the requests loop for a duplicate claim; never stored or uploaded.
    texts["replay_in_progress_returned"] = json.dumps(in_progress[-1]["expect"]["returns"])
    for row in executions:  # Fixed failure texts; projections.jsonl holds every result shape.
        if "stored" in row["expect"] and json.loads(row["expect"]["stored"])["result"] is None:
            texts["execute:" + row["id"]] = row["expect"]["stored"]

    async def cancelled():
        with scratch() as root:
            with fixed_clock():
                node = make_connector(root)
            try:
                capable(node)
                results = {}
                with fixed_clock():
                    node.fleet.resolve = mock.AsyncMock(side_effect=asyncio.CancelledError())
                    request = session_request("send", valid_payloads()["send"], "cancel-route")
                    with contextlib.suppress(asyncio.CancelledError):
                        await node.execute(request)
                    results["cancelled_during_route_resolution"] = request["request_id"]
                    del node.fleet.resolve
                    for name, operation in (("cancelled_running_mutation", "send"), ("cancelled_running_read", "inspect")):
                        request = session_request(operation, valid_payloads()[operation], name)
                        node._native = Native({("send", SESSION, "--json"): asyncio.CancelledError(),
                                               ("inspect", SESSION): asyncio.CancelledError()})
                        with contextlib.suppress(asyncio.CancelledError):
                            await node.execute(request)
                        results[name] = request["request_id"]
                return {name: node.journal.db.execute("SELECT response FROM requests WHERE request_id=?", (rid,)).fetchone()[0]
                        for name, rid in results.items()}
            finally:
                await node.accounts.close()
                node.journal.close()

    texts.update(asyncio.run(cancelled()))
    receipts = {}
    for name, text in sorted(texts.items()):
        value = json.loads(text)
        receipts[name] = flagged({"stored": text, "state": value.get("state"), "error": value.get("error")}, value.get("error"))
    upload_example = completed(text="caf\u00e9 \U0001f600 \u2028")
    return document({
        "provenance": "texts produced by the Python Journal and Connector.execute; json.dumps default separators, ensure_ascii",
        "receipts": receipts,
        "receipt_keys": ["state", "result", "error"],
        "upload": {"method": "POST", "path": "/v1/node/requests/{request_id}/result", "content_type": "application/json",
                   "rule": "the body is json.dumps(json.loads(stored)): the stored text for ordinary values",
                   "example_stored": json.dumps(upload_example), "example_body": json.dumps(json.loads(json.dumps(upload_example)))},
        "android": {"update_required_substrings": list(ANDROID_UPDATE_REQUIRED), "shown_prefix_characters": ANDROID_ERROR_PREFIX},
    })


# ---------------------------------------------------------------------------
# Projections through pure functions.

def call_cases():
    usage_full = ACCOUNTS["profiles"][0]["account_status"]
    worktree_raw = {"state": "ok", "path": "/example/repo", "common_dir": "/example/repo/.git", "selected_root": "/example/repo",
                    "stale": False, "partial": False, "private": "x",
                    "worktrees": [{"path": "/example/repo", "kind": "main", "available": True, "branch": "main", "head": "abc",
                                   "locked": "yes", "private": 1},
                                  {"path": "/example/repo-feature", "kind": "linked", "available": False, "detached": True, "head": ""}]}
    attachment_raw = {"request_id": ident("message"), "index": 0, "name": "note.txt", "mime": "text/plain", "bytes": 6,
                      "data_base64": base64.b64encode(b"abcdef").decode()}
    attachment_payload = {"request_id": ident("execute/chunk"), "expected_run_id": RUN, "expected_conversation_id": CONVERSATION,
                          "message_id": ident("message"), "index": 0, "offset": 2, "length": 3}
    swarm_snapshot = {"host": "local-native", "sessions": [{"name": SESSION}, {"name": "claude/example/archived", "state": "archived",
                                                                               "archive_id": ARCHIVE}, {"name": "codex/other"}]}
    return [
        ("projects.normalize", "projects-normalize", [SWARM, swarm_snapshot, BINDING]),
        ("projects.normalize", "projects-uninitialized", [{**SWARM, "initialized": False}, swarm_snapshot, BINDING]),
        ("projects.normalize", "projects-duplicate-group", [{**SWARM, "organization": {"projects": [
            {"id": "a", "name": "A"}, {"id": "a", "name": "B"}]}}, swarm_snapshot, BINDING]),
        ("projects.normalize", "projects-schema", [{**SWARM, "schema": 2}, swarm_snapshot, BINDING]),
        ("projects.unavailable", "projects-unavailable", [BINDING]),
        ("projects.apply_memberships", "projects-memberships", [swarm_snapshot, "$normalized"]),
        ("accounts.usage", "usage-full", [usage_full]),
        ("accounts.usage", "usage-hostile", [{"status": "root", "identity": {"name": "a\u0000b", "email": 5},
                                               "windows": [{"used_percent": float("inf")}, {"used_percent": 2 ** 53},
                                                           {"used_percent": 1, "resets_at": "2025-13-40T00:00:00Z"}],
                                               "credits": {"balance": "1e3", "unlimited": "yes"},
                                               "balances": [{"currency": "USD", "balance": "-1.25"}, {"currency": "EUR", "balance": "1"}]}]),
        ("accounts.usage", "usage-not-object", [None]),
        ("accounts.model_options", "model-options", [ACCOUNTS["profiles"][1]]),
        ("launch.catalog", "launch-catalog", [LAUNCH_ACCOUNTS]),
        ("accounts.catalog+view", "account-cache-view", [ACCOUNTS]),
        ("accounts.catalog+view", "account-cache-refuses-unknown-provider", [LAUNCH_ACCOUNTS]),
        ("launch.catalog", "launch-catalog-not-object", [[]]),
        ("launch.canonical_directory", "canonical-directory", ["/example/repo"]),
        ("launch.canonical_directory", "canonical-directory-double-slash", ["//example"]),
        ("launch.canonical_directory", "canonical-directory-dot-dot", ["/example/../repo"]),
        ("launch.canonical_directory", "canonical-directory-trailing", ["/example/"]),
        ("launch.directory_created", "directory-created", [{"status": "created", "request_id": ident("d"), "requested_path": "/example/new",
                                                             "parent": "/example", "path": "/example/new", "private": 1},
                                                            {"request_id": ident("d"), "path": "/example/new"}, ident("d"), "/example"]),
        ("launch.directory_created", "directory-created-other-parent", [{"status": "created", "request_id": ident("d"),
                                                                         "requested_path": "/example/new", "parent": "/real",
                                                                         "path": "/real/new"},
                                                                        {"request_id": ident("d"), "path": "/example/new"}, ident("d"), "/real"]),
        ("worktrees.catalog", "worktrees-catalog", [worktree_raw, "/example/repo"]),
        ("worktrees.catalog", "worktrees-missing-identity", [{**worktree_raw, "common_dir": None}, "/example/repo"]),
        ("worktrees.created", "worktrees-created", [{"request_id": ident("w"), "status": "created", "common_dir": "/example/repo/.git",
                                                     "branch": "feature", "path": "/example/repo-feature", "private": 1},
                                                    {"common_dir": "/example/repo/.git", "branch": "feature"}, ident("w"),
                                                    "/example/repo-feature"]),
        ("context.mark_read_result", "mark-read-applied", [{"ok": True, "applied": {"reply_id": True, "attention_signature": False},
                                                            "read": {"conversation_id": CONVERSATION}}, valid_payloads()["mark_read"]]),
        ("context.mark_read_result", "mark-read-int-flag", [{"ok": True, "applied": {"reply_id": 1, "attention_signature": False},
                                                             "read": None}, valid_payloads()["mark_read"]]),
        ("context.mark_read_result", "mark-read-not-found", [{"ok": False, "error": "not_found"}, valid_payloads()["mark_read"]]),
        ("speech.result_valid", "speech-result", ["speech_status", {"schema": 1, "job_id": ident("speech-job"), "state": "running"},
                                                  {"job_id": ident("speech-job")}]),
        ("speech.result_valid", "speech-result-state", ["speech_status", {"schema": 1, "job_id": ident("speech-job"), "state": "done"},
                                                        {"job_id": ident("speech-job")}]),
        ("voice.result_valid", "voice-result", ["voice_status", {"schema": 1, "message": {"id": ident("voice-message"), "run_id": RUN,
                                                                                          "conversation_id": CONVERSATION, "state": "ready",
                                                                                          "revision": 1}},
                                                substitute(valid_payloads()["voice_status"], ident("v"))]),
        ("voice.result_valid", "voice-result-revision-bool", ["voice_status", {"schema": 1, "message": {
            "id": ident("voice-message"), "run_id": RUN, "conversation_id": CONVERSATION, "state": "ready", "revision": True}},
            substitute(valid_payloads()["voice_status"], ident("v"))]),
        ("speech_config.result_valid", "speech-config-receipt", ["speech_profile_remove", {"schema": 1, "machine_id": MACHINE,
                                                                                          "request_id": ident("s"), "state": "completed",
                                                                                          "revision": 2}, {"request_id": ident("s"),
                                                                                                           "expected_machine_id": MACHINE}]),
        ("speech_config.result_valid", "speech-config-secret-field", ["speech_profile_remove", {"schema": 1, "machine_id": MACHINE,
                                                                                               "request_id": ident("s"), "state": "completed",
                                                                                               "credential": "fixture-secret"},
                                                                      {"request_id": ident("s"), "expected_machine_id": MACHINE}]),
        ("attachment_reads.decode+chunk", "attachment-chunk", [attachment_raw, attachment_payload, SESSION, ident("execute/chunk")]),
        ("attachment_reads.decode+chunk", "attachment-size-changed", [{**attachment_raw, "bytes": 7}, attachment_payload, SESSION,
                                                                      ident("execute/chunk")]),
        ("attachment_reads.decode+chunk", "attachment-range-outside", [attachment_raw, {**attachment_payload, "offset": 6}, SESSION,
                                                                       ident("execute/chunk")]),
        ("fleet.Fleet.heartbeat", "fleet-heartbeat", [{"sessions": []}]),
    ]


def call(function: str, args: list):
    if function == "projects.apply_memberships":
        snapshot = copy.deepcopy(args[0])
        projects.apply_memberships(snapshot, projects.normalize(SWARM, snapshot, BINDING))
        return snapshot
    if function == "attachment_reads.decode+chunk":
        raw, payload, session, request_id = copy.deepcopy(args)
        return attachment_reads.chunk(attachment_reads.decode(raw, payload), session, payload, request_id)
    if function == "accounts.catalog+view":
        cache = accounts.AccountCache(None, clock=lambda: MONOTONIC, wall=lambda: EPOCH)
        cache.entries, cache.available, cache.checked_at = accounts.catalog(copy.deepcopy(args[0])), True, EPOCH
        return cache.view()
    if function == "fleet.Fleet.heartbeat":
        node = types.SimpleNamespace()
        inventory = fleet.Fleet(node, ConnectorError)
        inventory.supported, inventory.local_id = True, MACHINE
        route = fleet.route_id("peer.example.test", ident("peer"))
        inventory.routes = {route: {"route_id": route, "machine_id": ident("peer"), "via": "peer.example.test",
                                    "name": "Peer", "online": True}}
        return inventory.heartbeat(copy.deepcopy(args[0]))
    module, name = function.split(".")
    return getattr(globals()[module], name)(*copy.deepcopy(args))


def projection_vectors(runs: dict, executions: list[dict]) -> bytes:
    rows = []
    for function, name, args in call_cases():
        row = {"kind": "call", "id": name, "function": function, "args": args}
        try:
            row["returns"] = call(function, args)
        except (ValueError, TypeError, KeyError, attachment_reads.AttachmentReadError) as error:
            row["raises"] = {"type": type(error).__name__, "message": str(error)}
        rows.append(row)
    full = runs["all"]
    # Projects and attention run concurrently, so only the set of native calls is a contract.
    calls = sorted(full["calls"], key=compact)
    rows.append({"kind": "snapshot", "id": "heartbeat-snapshot", "native": {
        "ls --json --local": LISTING, "ls --json --local --no-archives": PAGED_LISTING, "archives --json": EMPTY_ARCHIVES,
        "swarm get": SWARM, "account ls": ACCOUNTS, "inspect " + SESSION: inspection(),
        "--help": help_text()}, "peers_discovered": True, "expect": {"heartbeat": full["heartbeat"], "calls": calls}})
    rows += executions
    return jsonl(rows)


# ---------------------------------------------------------------------------
# Sizes: Python's own byte measures, including cases that flip on one byte.

def expand(template, fill: dict):
    if isinstance(template, str) and template in fill:
        return "".join(unit * count for unit, count in fill[template])
    if isinstance(template, list):
        return [expand(item, fill) for item in template]
    if isinstance(template, dict):
        return {key: expand(item, fill) for key, item in template.items()}
    return template


def measures(value) -> dict:
    return {"default_ascii": len(json.dumps(value).encode()),
            "default_utf8": len(json.dumps(value, ensure_ascii=False).encode()),
            "canonical": len(json.dumps(value, sort_keys=True, separators=(",", ":"), allow_nan=False).encode())}


def boundary(template, marker, unit, limit, *, ensure_ascii=True):
    """[[unit, n], ["a", k]] parts so the measured value is exactly limit bytes."""
    empty = len(json.dumps(expand(template, {marker: []}), ensure_ascii=ensure_ascii).encode())
    width = len(json.dumps(unit, ensure_ascii=ensure_ascii).encode()) - 2
    count, pad = divmod(limit - empty, width)
    return [[unit, count], ["a", pad]]


def size_vectors(root: Path) -> bytes:
    rows = []
    for name, value in (("ascii", {"a": "b"}), ("latin", {"a": "\u00e9"}), ("cjk", {"a": "\u754c"}), ("non-bmp", {"a": "\U0001f600"}),
                        ("separators", {"a": "\u2028\u2029", "b": [1, 2.5, None, True], "c": {}}),
                        ("escapes", {"a": "\"\\\n\t\x00\x7f"}), ("key-order", {"b": 1, "a": [{"z": 0, "y": -0.0}]}),
                        ("numbers", {"a": [2 ** 80, 1e16, 1e-5, 5e-324, -0.0]})):
        rows.append({"kind": "measure", "id": "measure-" + name, "value": value, "expect": measures(value)})
    response = {"state": "completed", "result": {"request_id": ident("size"), "output": "@fill"}, "error": None}
    for label, unit in (("ascii", "a"), ("latin", "\u00e9"), ("non-bmp", "\U0001f600")):
        parts = boundary(response, "@fill", unit, connector.MAX_RESULT)
        for delta in (0, 1):
            fill = {"@fill": parts + [["a", delta]]}
            measured = len(json.dumps(expand(response, fill)).encode())
            rows.append({"kind": "limit", "id": "max-result-%s-%d" % (label, delta), "measure": "default_ascii",
                         "limit": connector.MAX_RESULT, "value": response, "fill": fill,
                         "expect": {"bytes": measured, "exceeds": measured > connector.MAX_RESULT}})
    stdin = {"request_id": ident("stdin"), "text": "@fill"}

    async def native_stdin(argv, payload):
        # A path that cannot start: the size check runs before the spawn attempt.
        probe = types.SimpleNamespace(hgs=str(root / "missing-hgs"), max_bytes=connector.MAX_BYTES, timeout=1)
        try:
            await Connector._native(probe, argv, payload)
        except ConnectorError as error:
            return str(error)
        raise RuntimeError("an absent hgs path unexpectedly started")

    for label, argv, extra in (("local", ["send", SESSION, "--json"], 0), ("peer", ["swarm", "mobile-peer", "--json"], 4096)):
        parts = boundary(stdin, "@fill", "\u754c", connector.MAX_PAYLOAD + extra, ensure_ascii=False)
        for delta in (0, 1):
            fill = {"@fill": parts + [["a", delta]]}
            value = expand(stdin, fill)
            rows.append({"kind": "limit", "id": "native-stdin-%s-%d" % (label, delta), "measure": "default_utf8",
                         "limit": connector.MAX_PAYLOAD + extra, "argv": argv, "value": stdin, "fill": fill,
                         "expect": {"bytes": len(json.dumps(value, ensure_ascii=False).encode()),
                                    "error": asyncio.run(native_stdin(argv, value))}})
    inspection_value = {"name": SESSION, "run_id": RUN, "conversation_id": CONVERSATION,
                        "events": [{"type": "AgentMessage", "at": 10 + index, "text": "@row"} for index in range(6)]
                        + [{"type": "UserPromptSubmit", "at": 1, "text": "@row"}],
                        "message_events": [{"type": "UserMessage", "at": 2, "text": "short"}]}
    for label, count in (("fits", 20000), ("trims", 40000)):
        fill = {"@row": [["\u00e9", count]]}
        result = history.bound_inspection(expand(inspection_value, fill), connector.MAX_RESULT - 1024)
        rows.append({"kind": "bound_inspection", "id": "bound-inspection-" + label, "maximum": connector.MAX_RESULT - 1024,
                     "value": inspection_value, "fill": fill,
                     "expect": {"truncated": result.get("mobile_history_truncated", False),
                                "omitted": result.get("mobile_history_omitted"),
                                "kept_events_at": [row["at"] for row in result["events"]],
                                "kept_message_events_at": [row["at"] for row in result["message_events"]],
                                "completed_size": history.completed_size(result),
                                "sha256": hashlib.sha256(json.dumps(result, sort_keys=True, separators=(",", ":")).encode()).hexdigest()}})
    for label in ("64KiB", "64KiB+1"):
        payload = {"request_id": ident("send"), "expected_run_id": RUN, "expected_conversation_id": CONVERSATION, "text": "@fill"}
        fill = {"@fill": [["\u00e9", 32768]] + ([["a", 1]] if label.endswith("+1") else [])}
        try:
            attachments.validate_send(expand(payload, fill))
            message = None
        except ValueError as error:
            message = str(error)
        receipt = connector_decision({"submission": {"request_id": ident("send"), "computer_id": COMPUTER, "operation": "send",
                                                     "session": SESSION, "payload": expand(payload, fill)}})
        rows.append({"kind": "validator", "id": "send-text-" + label, "function": "attachments.validate_send", "value": payload,
                     "fill": fill, "expect": {"error": message, "receipt_error": receipt.get("error")}})
    for label, extra in (("64KiB", 0), ("64KiB+1", 1)):
        payload = {"request_id": ident("t"), "expected_run_id": RUN, "expected_conversation_id": CONVERSATION,
                   "terminal_binding_id": "d" * 64, "text": "@fill", "enter": False}
        fill = {"@fill": [["\u00e9", 32768], ["a", extra]]}
        try:
            terminal.validate("terminal_input", expand(payload, fill), ident("t"))
            message = None
        except ValueError as error:
            message = str(error)
        rows.append({"kind": "validator", "id": "terminal-text-" + label, "function": "terminal.validate", "value": payload,
                     "fill": fill, "expect": {"error": message}})
    for label, extra in (("maximum", 0), ("maximum+4", 4)):
        payload = {"request_id": ident("sp"), "profile_id": "gigaam-ru", "audio": {**AUDIO, "data_base64": "@fill"}}
        fill = {"@fill": [["A", (speech.MAX_AUDIO_BYTES + 2) // 3 * 4 + extra]]}
        try:
            speech.validate("speech_submit", expand(payload, fill), ident("sp"))
            message = None
        except ValueError as error:
            message = str(error)
        rows.append({"kind": "validator", "id": "speech-audio-" + label, "function": "speech.validate", "value": payload,
                     "fill": fill, "expect": {"error": message},
                     "source": "test_mobile_speech.SpeechValidationTests.test_maximum_audio_has_bounded_valid_encoding"})
    return jsonl(rows)


# ---------------------------------------------------------------------------

def render(oracle: dict | None, relay: dict) -> dict[str, bytes]:
    """Every vector file, keyed by its path below services/mobile/vectors."""
    # Parent directories that Journal creates take the process umask.
    previous = os.umask(0o022)
    try:
        return _render(oracle, relay)
    finally:
        os.umask(previous)


def _render(oracle: dict | None, relay: dict) -> dict[str, bytes]:
    files = {}
    with scratch() as root:
        runs = asyncio.run(snapshot_runs(root / "snapshots"))
        files["heartbeat-legacy.json"] = asyncio.run(legacy_snapshot_vector(root / "legacy-snapshot"))
        executions = asyncio.run(execute_runs(root / "executions"))
        files["identities.json"] = identity_vectors(root / "identities")
        files["sizes.jsonl"] = size_vectors(root)
    files["operations.json"] = operation_tables(runs)
    validation, observed = validation_vectors(oracle, relay)
    files.update(validation)
    files["errors.json"] = error_catalogue(observed)
    files.update(hash_vectors())
    files["config.jsonl"] = jsonl(config_vectors())
    files.update(journal_vectors())
    files["receipts.json"] = receipt_vectors(executions)
    files["projections.jsonl"] = projection_vectors(runs, executions)
    return dict(sorted(files.items()))


def existing(directory: Path) -> set[str]:
    return {str(path.relative_to(directory)) for pattern in ("*.json", "*.jsonl", "*/*.jsonl", "journal/*.sql")
            for path in directory.glob(pattern)}


def differences(files: dict[str, bytes], directory: Path = HERE) -> list[str]:
    problems = []
    for name, data in files.items():
        path = directory / name
        if not path.exists():
            problems.append(name + ": missing")
        elif path.read_bytes() != data:
            old, new = path.read_bytes().splitlines(), data.splitlines()
            line = next((index for index, (a, b) in enumerate(zip(old, new)) if a != b), min(len(old), len(new)))
            problems.append("%s: differs from line %d" % (name, line + 1))
    problems += [name + ": not produced by the capture" for name in sorted(existing(directory) - set(files))]
    problems += ["%s: %d bytes exceeds %d" % (name, len(data), FILE_LIMIT) for name, data in files.items() if len(data) > FILE_LIMIT]
    return problems


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--check", action="store_true", help="recompute and compare without writing")
    parser.add_argument("--refresh-oracle", action="store_true", help="recompute the relay-oracle column from tests/relay_oracle.py")
    args = parser.parse_args()
    oracle = None if args.refresh_oracle else frozen_oracle(HERE)
    try:
        files = render(oracle, frozen_relay(HERE))
    except CaptureError as error:
        print(error, file=sys.stderr)
        return 1
    if args.check:
        problems = differences(files)
        for problem in problems:
            print(problem, file=sys.stderr)
        return 1 if problems else 0
    for name, data in files.items():
        path = HERE / name
        path.parent.mkdir(exist_ok=True)
        if not path.exists() or path.read_bytes() != data:
            path.write_bytes(data)
            print("wrote", name)
    for name in sorted(existing(HERE) - set(files)):
        (HERE / name).unlink()
        print("removed", name)
    oversized = [name for name, data in files.items() if len(data) > FILE_LIMIT]
    if oversized:
        print("over %d bytes: %s" % (FILE_LIMIT, ", ".join(oversized)), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
