"""Shared parts of the black-box suites for computer, read and media operations.

Expected texts come from the golden vectors (`services/mobile/vectors`): help
markers from `operations.json`, rejection messages from `validation/*.jsonl`
and native exchanges with their stored receipts from the `execute` rows of
`projections.jsonl`. The vectors describe one synthetic computer; `listing()`
here is that computer as `hgs ls --json --local` shows it.
"""
from __future__ import annotations

import asyncio
import copy
import json
import os
from pathlib import Path
import time
import uuid

from connector_harness import HELP_TEXT, ROOT, SERVICE, listing as base_listing, session_row, stopped_journal

VECTORS = SERVICE / "vectors"
OPERATIONS = json.loads((VECTORS / "operations.json").read_text())
MARKERS = {row["name"]: row["help_line"] for row in OPERATIONS["capabilities"]["markers"]}
READ_OPERATIONS = frozenset(OPERATIONS["sets"]["READ_OPERATIONS"])
TERMINAL_KEYS = OPERATIONS["sets"]["terminal.KEYS"]
RECEIPTS = json.loads((VECTORS / "receipts.json").read_text())["receipts"]

# The synthetic computer of the vectors (capture.py).
SESSION = "codex/example/session"
RUN = "fixture-run"
CONVERSATION = "fixture-conversation"
_CONTEXT = json.loads((VECTORS / "validation/launch.jsonl").read_text().splitlines()[0])["context"]
ARCHIVES = [tuple(pair) for pair in _CONTEXT["archives"]]
ARCHIVE = ARCHIVES[0][1]


def help_text(*markers: str) -> str:
    """`hgs --help` that advertises exactly these mobile ABI markers."""
    return HELP_TEXT + "".join(MARKERS[name] + "\n" for name in markers)


def listing(*rows: dict, **extra) -> dict:
    """The vectors' computer, two live sessions and two archives of one name,
    then these rows. No row is a live tracked Codex session, so no attention
    scan runs."""
    sessions = [session_row(name=name, run_id=RUN, conversation_id=CONVERSATION) for name in _CONTEXT["sessions"]]
    sessions += [session_row(name=name, run_id="archived-" + archive[:8], conversation_id="archived-" + archive[:8],
                             runtime_state="archived", process_state="stopped", state="archived", archive_id=archive)
                 for name, archive in ARCHIVES]
    return base_listing(sessions=sessions + list(rows), **extra)


def validation_cases(name: str, operations) -> list[dict]:
    """Reviewed rejections of these operations that a running connector can
    reproduce: a relay submission envelope, the default context and a receipt,
    not a refusal before the journal."""
    cases = []
    for line in (VECTORS / "validation" / (name + ".jsonl")).read_text().splitlines():
        row = json.loads(line)
        if (row["kind"] == "case" and row["submission"].get("operation") in operations
                and set(row["submission"]) == {"request_id", "computer_id", "operation", "session", "payload"}
                and row["connector"]["decision"] == "reject" and row["connector"].get("stage") == "receipt"
                and row.get("context", _CONTEXT) == _CONTEXT):
            cases.append(row)
    return cases


def projection(identity: str, kind: str = "call") -> dict:
    """A row of projections.jsonl: a pure projection `call` or an `execute` run."""
    for line in (VECTORS / "projections.jsonl").read_text().splitlines():
        row = json.loads(line)
        if row.get("kind") == kind and row["id"] == identity:
            return row
    raise KeyError(identity)


def expand(template, fill: dict):
    """A vector value with each `fill` marker replaced by its [unit, count] parts."""
    if isinstance(template, str) and template in fill:
        return "".join(unit * count for unit, count in fill[template])
    if isinstance(template, list):
        return [expand(item, fill) for item in template]
    if isinstance(template, dict):
        return {key: expand(item, fill) for key, item in template.items()}
    return template


def size_vector(identity: str) -> tuple[object, dict]:
    """(expanded value, expectation) of a `sizes.jsonl` row."""
    for line in (VECTORS / "sizes.jsonl").read_text().splitlines():
        row = json.loads(line)
        if row["id"] == identity:
            return expand(row["value"], row.get("fill", {})), row["expect"]
    raise KeyError(identity)


def real_hgs() -> str | None:
    """A built hgs: ZERUS_DIRECTORY_TEST_HGS (set by CI), HGS_TEST_BIN or target/debug/hgs."""
    for candidate in (os.environ.get("ZERUS_DIRECTORY_TEST_HGS"), os.environ.get("HGS_TEST_BIN"),
                      str(ROOT / "target/debug/hgs")):
        if candidate and Path(candidate).is_file() and os.access(candidate, os.X_OK):
            return str(Path(candidate).resolve())
    return None


class OperationCase:
    """Mixin for BlackBoxCase: vector replays and calls made after a mark."""

    BACKGROUND = (["--help"], ["ls", "--json", "--local"], ["swarm", "get"], ["account", "ls"], ["account", "inspect"])

    async def asyncSetUp(self):
        await super().asyncSetUp()
        # Building, counting and writing multi-megabyte requests and answers
        # blocks the test loop for a while; that is not a slow callback to report.
        asyncio.get_running_loop().slow_callback_duration = 30

    def mark(self) -> int:
        return len(self.hgs.calls())

    def since(self, mark: int, *prefix: str) -> list[dict]:
        return [call for call in self.hgs.calls()[mark:] if call["argv"][:len(prefix)] == list(prefix)]

    def work(self, mark: int, *, keep=()) -> list[dict]:
        """Native calls since `mark` other than the heartbeat's own listing,
        help, project and account reads (unless listed in `keep`)."""
        return [call for call in self.hgs.calls()[mark:]
                if list(call["argv"]) in map(list, keep)
                or not any(call["argv"][:len(prefix)] == prefix for prefix in self.BACKGROUND)]

    def operation(self, operation: str, *, session: str = SESSION, identity: bool = True,
                  request_id: str | None = None, own: bool = False, **payload) -> dict:
        """A phone submission for the vectors' computer. `identity` adds the
        exact request, run and conversation fields to the payload; `own` adds
        only the request field."""
        request_id = request_id or str(uuid.uuid4())
        if identity:
            payload = {"request_id": request_id, "expected_run_id": RUN, "expected_conversation_id": CONVERSATION,
                       **payload}
        elif own:
            payload = {"request_id": request_id, **payload}
        return {"request_id": request_id, "computer_id": self.relay.computer_id, "operation": operation,
                "session": session, "payload": payload}

    def age(self, seconds: float, *requests: dict) -> None:
        """Move journal rows into the past; only while the connector is stopped."""
        with stopped_journal(self.state_dir) as db:
            for request in requests:
                db.execute("UPDATE requests SET created_at=created_at-?, updated_at=updated_at-?,"
                           " delivered_at=delivered_at-? WHERE request_id=?",
                           (seconds, seconds, seconds, request["request_id"]))

    async def delivered(self, process, *requests: dict) -> None:
        await self.until(lambda: all((row := self.journal.row(request["request_id"])) and row["delivered"]
                                     for request in requests), what="delivered journal rows", process=process)

    def envelope(self, request: dict) -> dict:
        """A vector request (without computer_id) as a phone submits it."""
        return {"request_id": request["request_id"], "computer_id": self.relay.computer_id,
                "operation": request["operation"], "session": request["session"], "payload": request["payload"]}

    async def assert_rejections(self, process, cases: list[dict], operation_argv=()) -> None:
        """Every case fails with its vector message and reaches no native command."""
        for case in cases:
            with self.subTest(case["id"]):
                mark = self.mark()
                submission = {**case["submission"], "computer_id": self.relay.computer_id}
                result = await self.submit(submission, process)
                self.assertEqual(result, {"state": "failed", "result": None, "error": case["connector"]["error"]})
                self.assertEqual(self.work(mark), [], "a rejected request reached native hgs")
                for argv in operation_argv:
                    self.assertEqual(self.since(mark, *argv), [])

    async def replay(self, process, identity: str, *, expires_in: float | None = None) -> dict:
        """Run an `execute` vector: its native answers in, its argv, stdin
        payloads and stored receipt out. `expires_in` moves a terminal input
        deadline from the vector clock to now."""
        vector = copy.deepcopy(projection(identity, "execute"))
        request = vector["request"]
        expires_at = request.pop("expires_at", None)
        if expires_in is not None:
            expires_at = time.time() + expires_in
        rules = []
        for item in vector["native"]:
            answers = item["answer"] if isinstance(item["answer"], list) else [item["answer"]]
            for index, answer in enumerate(answers):
                rule = {"argv": item["argv"]}
                if index < len(answers) - 1:
                    rule["times"] = 1
                if isinstance(answer, dict) and "raised" in answer:
                    # Only a failing exit has a black-box form; a timeout or crash has none.
                    if answer != {"raised": "ConnectorError", "error": "native command failed"}:
                        raise AssertionError(f"{identity}: native {answer} has no black-box form")
                    rule["exit"] = 1
                else:
                    rule["stdout"] = answer
                rules.append(rule)
        # Prepending renews every rule's `times`; restore the script afterwards.
        before = [{key: value for key, value in rule.items() if key != "id"} for rule in self.hgs.rules]
        self.hgs.prepend(*rules)
        mark = self.mark()
        keep = [call["argv"] for call in vector["expect"]["calls"]]
        try:
            result = await self.submit(self.envelope(request), process, expires_at=expires_at)
        finally:
            self.hgs.script(before)
        self.assertEqual(result, json.loads(vector["expect"]["stored"]), identity)
        calls = self.work(mark, keep=keep)
        self.assertEqual([call["argv"] for call in calls], keep, identity)
        for call, expected in zip(calls, vector["expect"]["calls"]):
            payload = expected.get("payload")
            if payload is not None and "expires_at" in payload:
                payload["expires_at"] = expires_at
            self.assertEqual(json.loads(call["stdin"]) if call["stdin"] else None, payload, identity)
        return result
