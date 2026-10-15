"""Shared pieces of the session-operation black-box suites (`test_bb_*`).

Identities, help lines and receipt texts come from the golden vectors in
services/mobile/vectors, so these suites cannot drift from them: the computer
holds the sessions and archives of the vectors' validation context,
`help_text()` prints the vectors' capability help lines, and `text()` accepts
only an error the vectors catalogue.
"""
from __future__ import annotations

import asyncio
import json
import uuid

from connector_harness import SERVICE, BlackBoxCase, HELP_TEXT, canonical_uuid, default_rules, listing, session_row

VECTORS = SERVICE / "vectors"
OPERATIONS = json.loads((VECTORS / "operations.json").read_text())
_ERRORS = json.loads((VECTORS / "errors.json").read_text())
_RECEIPTS = json.loads((VECTORS / "receipts.json").read_text())
_MARKERS = {marker["name"]: marker for marker in OPERATIONS["capabilities"]["markers"]}
_TEXTS = ({row["message"] for row in _ERRORS["raises"]} | {row["message"] for row in _ERRORS["library_messages"]}
          | {row["error"] for row in _RECEIPTS["receipts"].values()})
# The connector's own listing, help probe and catalog reads.
BACKGROUND = (["--help"], ["ls", "--json", "--local"], ["swarm", "get"], ["account", "ls"])


def validation(family: str) -> tuple[dict, list[dict]]:
    """The context and the cases of validation/<family>.jsonl."""
    rows = [json.loads(line) for line in (VECTORS / "validation" / f"{family}.jsonl").read_text().splitlines()]
    [context] = [row["context"] for row in rows if row["kind"] == "context"]
    return context, [row for row in rows if row["kind"] == "case"]


def submission(family: str, name: str) -> dict:
    """A copy of one vector submission."""
    return json.loads(json.dumps(next(row for row in validation(family)[1] if row["id"] == name)["submission"]))


def size_case(name: str) -> dict:
    return next(row for row in map(json.loads, (VECTORS / "sizes.jsonl").read_text().splitlines()) if row["id"] == name)


def execution(name: str) -> dict:
    """One `Connector.execute` run of projections.jsonl."""
    return next(row for row in map(json.loads, (VECTORS / "projections.jsonl").read_text().splitlines())
                if row["kind"] == "execute" and row["id"] == name)


def filled(value, fill: dict):
    """A vector value with each `fill` placeholder replaced by its parts."""
    if isinstance(value, dict):
        return {key: filled(item, fill) for key, item in value.items()}
    if isinstance(value, list):
        return [filled(item, fill) for item in value]
    if isinstance(value, str) and value in fill:
        return "".join(unit * count for unit, count in fill[value])
    return value


def text(message: str) -> str:
    """A receipt error the phone sees; it must be one the vectors catalogue."""
    if message not in _TEXTS:
        raise AssertionError(f"{message!r} is not a receipt text of the golden vectors")
    return message


def help_text(*markers: str) -> str:
    """`hgs --help` with the help lines of exactly these capability markers."""
    return HELP_TEXT + "".join(_MARKERS[name]["help_line"] + "\n" for name in markers)


def advertised(*markers: str) -> list[str]:
    """The operations a connector advertises with exactly these markers: an
    optional operation needs every marker that lists it."""
    needed: dict[str, set[str]] = {}
    for marker in _MARKERS.values():
        for operation in marker["operations"]:
            needed.setdefault(operation, set()).add(marker["name"])
    return sorted(set(OPERATIONS["capabilities"]["none"]["operations"])
                  | {operation for operation, names in needed.items() if names <= set(markers)})


def reasons(*markers: str) -> dict:
    """The unsupported-feature reasons a connector reports with these markers."""
    found = dict(OPERATIONS["capabilities"]["none"]["reasons"])
    for name in markers:
        for key in _MARKERS[name]["reasons"]:
            found.pop(key, None)
    return found


CONTEXT = validation("envelope")[0]
SESSION = next(name for name in CONTEXT["sessions"] if not name.startswith("dsh/"))
DSH_SESSION = next(name for name in CONTEXT["sessions"] if name.startswith("dsh/"))
ARCHIVES = [tuple(pair) for pair in CONTEXT["archives"]]
ARCHIVE = ARCHIVES[0][1]
RUN = submission("conversation", "valid/send")["payload"]["expected_run_id"]
CONVERSATION = submission("conversation", "valid/send")["payload"]["expected_conversation_id"]


def live_row(name: str, **changes) -> dict:
    # Not a tracked Codex row, so the connector's attention scan never inspects it.
    return session_row(name=name, cmd="dsh" if name.startswith("dsh/") else "claude", run_id=RUN,
                       conversation_id=CONVERSATION, **changes)


def session_listing(*rows: dict) -> dict:
    """`hgs ls --json --local` holding the live sessions and the archives of
    the vectors' validation context, followed by `rows`."""
    archived = [live_row(name, archive_id=archive, state="archived", runtime_state="archived",
                         process_state="stopped") for name, archive in ARCHIVES]
    return listing(sessions=[*(live_row(name) for name in CONTEXT["sessions"]), *archived, *rows])


def ack(**extra) -> dict:
    """A native acknowledgement of the exact request, session and conversation."""
    return {"request_id": "{{stdin.request_id}}", "name": "{{argv.1}}", "run_id": RUN,
            "conversation_id": CONVERSATION, **extra}


class SessionCase(BlackBoxCase):
    """A computer with the vectors' sessions whose hgs prints the help lines
    of `markers`."""

    markers: tuple[str, ...] = ()
    # Relaying megabytes blocks the test's own event loop on purpose, which
    # asyncio's debug mode would otherwise report for every such callback.
    large_bodies = False

    async def asyncSetUp(self):
        await super().asyncSetUp()
        if self.large_bodies:
            asyncio.get_running_loop().slow_callback_duration = 60
        self.hgs.script(self.rules())

    def rules(self, *rules: dict, markers: tuple[str, ...] | None = None, listing_value: dict | None = None) -> list[dict]:
        """`rules` in front of the background answers of this computer."""
        help_value = help_text(*(self.markers if markers is None else markers))
        return [*rules, *default_rules(session_listing() if listing_value is None else listing_value, help_value)]

    def envelope(self, operation: str, payload, *, session: str = SESSION, identity: bool = True) -> dict:
        """A phone submission; with `identity` the payload starts with the exact
        request, run and conversation identity."""
        request_id = str(uuid.uuid4())
        if identity:
            payload = {"request_id": request_id, "expected_run_id": RUN, "expected_conversation_id": CONVERSATION,
                       **payload}
        return {"request_id": request_id, "computer_id": self.relay.computer_id, "operation": operation,
                "session": session, "payload": payload}

    async def deliver(self, request: dict) -> None:
        """Queue a submission as a phone would; one that is not a relay
        envelope is handed out as it stands, without its computer_id."""
        if (set(request) == {"request_id", "computer_id", "operation", "session", "payload"}
                and canonical_uuid(request["request_id"]) and isinstance(request["payload"], dict)):
            await self.relay.queue(request)
        else:
            await self.relay.inject({key: value for key, value in request.items() if key != "computer_id"})

    async def outcome(self, request: dict, process, timeout: float = 10) -> dict:
        """Deliver a submission and return the first result uploaded for it."""
        before = len(self.relay.results(request["request_id"]))
        await self.deliver(request)
        return (await self.until(lambda: self.relay.results(request["request_id"])[before:],
                                 timeout, "a result upload", process))[0]

    def native_calls(self, *prefix: str) -> list[dict]:
        """Native calls other than the connector's background reads."""
        return [call for call in self.hgs.calls(*prefix) if call["argv"] not in BACKGROUND]

    def failed(self, message: str) -> dict:
        return {"state": "failed", "result": None, "error": text(message)}

    def uncertain(self, message: str) -> dict:
        return {"state": "uncertain", "result": None, "error": text(message)}

    async def assert_replays(self, request: dict, receipt: dict, process) -> None:
        """A redelivered identity uploads its preserved receipt and runs nothing."""
        before = len(self.native_calls())
        self.assertEqual(await self.submit(request, process, redeliver=True), receipt)
        self.assertEqual(len(self.native_calls()), before, "a redelivered request ran native hgs again")
