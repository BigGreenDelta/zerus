"""A scripted one-hop fleet for the black-box gateway and peer suites.

The gateway's scripted `hgs` answers `swarm mobile-peers --json` with a
synthetic inventory, `swarm mobile-peer --json` for each online peer (matched
on the envelope's `via`, target machine and inner argv) and `swarm
__mobile-peer-local --json` for the gateway's own guarded machine. No SSH,
peer computer or native agent is involved: real hgs refuses a mismatched
target by itself, and here an unscripted call simply fails. Route IDs, limits
and receipt texts come from the golden vectors.
"""
from __future__ import annotations

import dataclasses
import json
from urllib.parse import parse_qsl
import uuid

from connector_harness import (BlackBoxCase, CONVERSATION_ID, Exchange, HELP_TEXT, RUN_ID, SERVICE, SESSION,
                               default_rules, inspection, listing, session_row)


def vector(name: str):
    """One golden vector file (services/mobile/vectors)."""
    return json.loads((SERVICE / "vectors" / name).read_text())


IDENTITIES = vector("identities.json")
OPERATIONS = vector("operations.json")
CATALOGUED = {row["message"] for row in vector("errors.json")["raises"]}
MAX_PEERS = OPERATIONS["constants"]["fleet.MAX_PEERS"]
MAX_PEER_SNAPSHOT = OPERATIONS["constants"]["fleet.MAX_PEER_SNAPSHOT"]
ROUTE_NAMESPACE = uuid.UUID(IDENTITIES["route_namespace"])
VECTOR_ROUTES = {(row["via"], row["machine_id"]): row["route_id"] for row in IDENTITIES["route_ids"]}
HELP_LINES = {row["name"]: row["help_line"] for row in OPERATIONS["capabilities"]["markers"]}


def error(message: str) -> str:
    """A receipt text from the vectors' catalogue of connector errors."""
    if message not in CATALOGUED:
        raise AssertionError(f"not a catalogued connector error: {message!r}")
    return message


ROUTE_CHANGED = error("direct peer route is unavailable or changed")
INVALID_ROUTE = error("invalid immutable gateway route")
LOCAL_CHANGED = error("local native machine identity changed")
NATIVE_FAILED = error("native command failed")

# Synthetic machine identities; PEER behind VIA is a route_ids vector.
LOCAL = "00000000-0000-4000-8000-000000000001"
PEER = "00000000-0000-4000-8000-000000000002"
OTHER = "00000000-0000-4000-8000-000000000003"
PEER_COMPUTER = "00000000-0000-4000-8000-000000000004"
VIA = "configured-mac"
ROUTE = VECTOR_ROUTES[(VIA, PEER)]
PEER_RUN = "bb-peer-run"
PEER_CONVERSATION = "bb-peer-conversation"
DEFAULT_NAME = "Peer computer"
OMITTED = object()  # A peer row without a `name` key.
UNINITIALIZED_SWARM = {"schema": 1, "initialized": False, "organization": {"projects": []}}
LOCAL_ACCOUNTS = [{"id": "account-local", "provider": "claude", "label": "Gateway account", "installed": False}]
PEER_ACCOUNTS = [{"id": "account-peer", "provider": "claude", "label": "Peer account", "installed": False}]


def help_text(*names: str) -> str:
    """`hgs --help` with the mobile ABI markers of these capabilities."""
    return HELP_TEXT + "".join(HELP_LINES[name] + "\n" for name in names)


def gateway_help(*names: str) -> str:
    """Help of a gateway's hgs: the direct-peer ABI plus these capabilities."""
    return help_text("peer_inventory", "peer_command", *names)


GATEWAY_HELP = gateway_help()


def synthetic_machine(number: int) -> str:
    """Another synthetic machine UUID, with hex letters in it."""
    return str(uuid.UUID(int=0xbb000000_0000_4000_8000_000000000000 + number))


def route_id(via: str, machine: str) -> str:
    """The identities.json rule: uuid5(route_namespace, via + NUL + machine_id)."""
    return str(uuid.uuid5(ROUTE_NAMESPACE, via + "\0" + machine))


def advertised_name(name) -> str:
    """The manifest name for a native peer name: control characters dropped,
    at most 128 code points, a fixed name when nothing is left."""
    if not isinstance(name, str):
        return DEFAULT_NAME
    return "".join(c for c in name if ord(c) >= 32 and ord(c) != 127)[:128] or DEFAULT_NAME


def python_length(value) -> int:
    """Python's `json.dumps(value, ensure_ascii=False)` length in UTF-8, the
    native stdin contract (only lengths must match; spelling may differ)."""
    return len(json.dumps(value, ensure_ascii=False).encode())


def inspected(run: str, conversation: str) -> dict:
    return {**inspection(), "run_id": run, "conversation_id": conversation}


def submitted(run: str, conversation: str, **extra) -> dict:
    """A native acknowledgement echoing the forwarded payload's identity."""
    return {"request_id": "{{stdin.payload.request_id}}", "status": "submitted", "name": SESSION,
            "run_id": run, "conversation_id": conversation, **extra}


def through_peer(via: str, machine: str, argv: list[str], **answer) -> dict:
    """A rule for one inner command sent to a peer through `swarm mobile-peer`."""
    return {"argv": ["swarm", "mobile-peer", "--json"],
            "stdin_fields": {"schema": 1, "via": via, "target_machine_id": machine, "argv": argv}, **answer}


def guarded(argv: list[str], machine: str = LOCAL, **answer) -> dict:
    """A rule for one command on the gateway's own guarded machine."""
    return {"argv": ["swarm", "__mobile-peer-local", "--json"],
            "stdin_fields": {"schema": 1, "target_machine_id": machine, "argv": argv}, **answer}


def machine_answers(*, help: str, snapshot: dict, accounts: list, run: str, conversation: str) -> list:
    """(inner argv, answer) for one machine: what a heartbeat and the core
    session operations read or change there."""
    return [
        (["--help"], {"stdout_text": help}),
        (["ls", "--json", "--local"], {"stdout": snapshot}),
        (["swarm", "get"], {"stdout": UNINITIALIZED_SWARM}),
        (["account", "ls"], {"stdout": {"profiles": accounts}}),
        (["inspect", SESSION], {"stdout": inspected(run, conversation)}),
        (["send", SESSION, "--json"], {"stdout": submitted(run, conversation, text="{{stdin.payload.text}}")}),
    ]


def peer_listing(**extra) -> dict:
    """The peer's `hgs ls --json --local`: the gateway's session name, with the
    peer's own run and conversation."""
    sessions = [session_row(run_id=PEER_RUN, conversation_id=PEER_CONVERSATION)]
    return listing(**{"host": "Synthetic peer", "sessions": sessions, **extra})


@dataclasses.dataclass
class Peer:
    """One row of the native inventory and, when online, its scripted hgs."""
    via: str = VIA
    machine: str | None = PEER
    name: object = "Synthetic peer"
    online: bool = True
    help: str = HELP_TEXT
    snapshot: dict = dataclasses.field(default_factory=peer_listing)
    accounts: list = dataclasses.field(default_factory=lambda: list(PEER_ACCOUNTS))
    # Extra (inner argv, answer) pairs, matched before the standard ones.
    answers: list = dataclasses.field(default_factory=list)

    @property
    def route(self) -> str:
        return route_id(self.via, self.machine)

    def row(self) -> dict:
        """The row as `hgs swarm mobile-peers --json` reports it."""
        row = {"via": self.via, "machine_id": self.machine, "name": self.name, "online": self.online,
               "error": None if self.online else "unavailable",
               "identity": None if self.machine is None else "verified" if self.online else "last_verified"}
        if self.name is OMITTED:
            del row["name"]
        return row

    def advertised(self) -> dict:
        return {"route_id": self.route, "machine_id": self.machine, "name": advertised_name(
            None if self.name is OMITTED else self.name), "online": self.online}

    def rules(self) -> list[dict]:
        if not self.online or self.machine is None:
            return []
        standard = machine_answers(help=self.help, snapshot=self.snapshot, accounts=self.accounts,
                                   run=PEER_RUN, conversation=PEER_CONVERSATION)
        return [through_peer(self.via, self.machine, argv, **answer) for argv, answer in [*self.answers, *standard]]


class Fleet:
    """The gateway's scripted hgs: its inventory, its peers and itself."""

    def __init__(self, *peers: Peer, help: str = GATEWAY_HELP, snapshot: dict | None = None,
                 discovery: dict | None = None, local: list | None = None):
        self.peers = list(peers)
        self.help = help
        self.snapshot = listing() if snapshot is None else snapshot
        self.discovery = dict(discovery or {})  # Extra keys of the discovery rule (delay, exit, ...).
        self.local = list(local or [])  # Extra (inner argv, answer) pairs on the guarded local machine.

    def inventory(self) -> dict:
        return {"schema": 1, "local": {"machine_id": LOCAL, "name": "Synthetic gateway"},
                "peers": [peer.row() for peer in self.peers]}

    def advertised(self) -> list[dict]:
        """The manifest a gateway publishes for this inventory."""
        rows = [peer.advertised() for peer in self.peers if peer.machine is not None and peer.machine != LOCAL]
        return sorted(rows, key=lambda row: row["route_id"])

    def rules(self) -> list[dict]:
        answer = self.discovery if {"stdout", "stdout_text"} & set(self.discovery) else {
            "stdout": self.inventory(), **self.discovery}
        rules = [{"argv": ["swarm", "mobile-peers", "--json"], **answer}]
        for peer in self.peers:
            rules.extend(peer.rules())
        own = machine_answers(help=self.help, snapshot=self.snapshot, accounts=LOCAL_ACCOUNTS,
                              run=RUN_ID, conversation=CONVERSATION_ID)
        rules.extend(guarded(argv, **answer) for argv, answer in [*self.local, *own])
        for rule in default_rules(self.snapshot, self.help):
            rules.append({**rule, "stdout": {"profiles": LOCAL_ACCOUNTS}} if rule["argv"] == ["account", "ls"] else rule)
        return rules


def query(exchange: Exchange) -> dict:
    return dict(parse_qsl(exchange.query, keep_blank_values=True))


def one_hop(exchange: Exchange) -> bool:
    return query(exchange).get("gateway_one_hop") == "1"


class GatewayCase(BlackBoxCase):
    """A BlackBoxCase whose scripted hgs is a Fleet."""

    def use(self, fleet: Fleet) -> Fleet:
        self.fleet = fleet
        self.rescript()
        return fleet

    def rescript(self) -> str:
        """Script the current fleet; return the id of its discovery rule."""
        self.hgs.script(self.fleet.rules())
        return self.hgs.rules[0]["id"]

    def polls(self) -> list[Exchange]:
        return [exchange for exchange in self.relay.exchanges
                if exchange.method == "GET" and exchange.path == "/v1/node/requests"]

    def attempts(self, mark: int = 0) -> list[Exchange]:
        """Every heartbeat POST since exchange `mark`, whatever its answer."""
        return [exchange for exchange in self.relay.exchanges[mark:]
                if exchange.method == "POST" and exchange.path == "/v1/node/heartbeat"]

    def peer_attempts(self, route: str | None = None) -> list[Exchange]:
        """Every peer heartbeat POST, for one route or all of them."""
        prefix = "/v1/node/peers/" + (route + "/" if route else "")
        return [exchange for exchange in self.relay.exchanges
                if exchange.method == "POST" and exchange.path.startswith(prefix)]

    def discoveries(self, rule: str | None = None) -> list[dict]:
        return [call for call in self.hgs.calls("swarm", "mobile-peers", "--json")
                if rule is None or call["rule"] == rule]

    def through(self, *, via: str | None = None, machine: str | None = None, argv: list | None = None) -> list[dict]:
        """`swarm mobile-peer` calls, each with its parsed envelope as `envelope`."""
        found = []
        for call in self.hgs.calls("swarm", "mobile-peer", "--json"):
            envelope = json.loads(call["stdin"])
            if ((via is None or envelope.get("via") == via)
                    and (machine is None or envelope.get("target_machine_id") == machine)
                    and (argv is None or envelope.get("argv", [])[:len(argv)] == argv)):
                found.append({**call, "envelope": envelope})
        return found

    def guarded_calls(self, argv: list | None = None) -> list[dict]:
        found = []
        for call in self.hgs.calls("swarm", "__mobile-peer-local", "--json"):
            envelope = json.loads(call["stdin"])
            if argv is None or envelope.get("argv", [])[:len(argv)] == argv:
                found.append({**call, "envelope": envelope})
        return found

    def route(self, peer: Peer | None = None, **changes) -> dict:
        """The immutable gateway_route the relay adds to a claim for this peer."""
        peer = peer or self.fleet.peers[0]
        return {"schema": 1, "route_id": peer.route, "computer_id": PEER_COMPUTER, "machine_id": peer.machine,
                **changes}

    def local_route(self, **changes) -> dict:
        return {"schema": 1, "route_id": None, "computer_id": self.relay.computer_id, "machine_id": LOCAL, **changes}

    def submission(self, operation: str, *, session: str = SESSION, peer: bool = True, **payload) -> dict:
        """A phone submission for the peer's (or the gateway's) session of the
        same name; session operations carry its exact run and conversation."""
        request_id = str(uuid.uuid4())
        if operation != "inspect":
            payload = {"request_id": request_id, "expected_run_id": PEER_RUN if peer else RUN_ID,
                       "expected_conversation_id": PEER_CONVERSATION if peer else CONVERSATION_ID, **payload}
        return {"request_id": request_id, "computer_id": self.relay.computer_id, "operation": operation,
                "session": session, "payload": payload}

    def scoped(self, operation: str, **payload) -> dict:
        """A submission without a session target (catalog, launch, dirs, ...)."""
        request_id = str(uuid.uuid4())
        return {"request_id": request_id, "computer_id": self.relay.computer_id, "operation": operation,
                "session": "", "payload": {"request_id": request_id, **payload}}

    def latest(self) -> dict:
        beats = self.relay.heartbeats()
        return beats[-1].body if beats else {}

    async def manifest(self, rows: list[dict], process, timeout: float = 15) -> dict:
        """Wait until the latest accepted heartbeat shares exactly these peers."""
        await self.until(lambda: self.latest().get("machine_id") == LOCAL and self.latest().get("peers") == rows,
                         timeout, f"a heartbeat manifest of {len(rows)} peers", process)
        return self.latest()

    async def gateway_ready(self, process, timeout: float = 20) -> None:
        """The fleet's manifest is shared, every online peer has heartbeated,
        and a one-hop poll is waiting."""
        await self.manifest(self.fleet.advertised(), process, timeout)
        online = [peer.route for peer in self.fleet.peers if peer.online and peer.machine not in (None, LOCAL)]
        await self.until(lambda: all(self.relay.peer_heartbeats(route) for route in online), timeout,
                         "a heartbeat of every online peer", process)
        await self.until(lambda: any(one_hop(poll) and poll.status == 0 for poll in self.polls()), timeout,
                         "a waiting one-hop poll", process)

    async def heartbeats_after(self, count: int, process, timeout: float = 10) -> dict:
        """Wait for `count` more accepted heartbeats; return the last body."""
        target = len(self.relay.heartbeats()) + count
        await self.until(lambda: len(self.relay.heartbeats()) >= target, timeout,
                         f"{count} more heartbeats", process)
        return self.latest()

    async def rediscovered(self, process) -> dict:
        """Script the current fleet, wait until a discovery answered by it
        has ended and two heartbeats have followed (the first may have read
        the routes before that answer arrived); return the last body."""
        rule = self.rescript()
        await self.until(lambda: any(self.hgs.finished(call["pid"]) for call in self.discoveries(rule)), 10,
                         "a discovery of the changed inventory", process)
        return await self.heartbeats_after(2, process)
