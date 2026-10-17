"""Black-box one-hop claims: how a gateway resolves a claim's immutable route
and forwards it to exactly one machine.

A claim with a peer route runs on that peer through `hgs swarm mobile-peer`;
one with the gateway's own route runs through `hgs swarm
__mobile-peer-local`, which pins the gateway's machine identity; one without
a route runs as on any computer. A route that does not match the current
inventory fails before native work, and the route is part of the request's
no-replay identity.
"""
import hashlib
import signal
import unittest

from connector_harness import CONTEXT_KEYS, CONVERSATION_ID, RESTART_ERROR, RUN_ID, SESSION, canonical, inspection
from gateway_harness import (Fleet, GatewayCase, INVALID_ROUTE, LOCAL, LOCAL_CHANGED, NATIVE_FAILED, OTHER,
                             PEER, PEER_CONVERSATION, PEER_RUN, Peer, ROUTE, ROUTE_CHANGED, VIA, gateway_help, guarded,
                             inspected, one_hop, peer_listing, python_length, route_id, submitted, synthetic_machine,
                             through_peer)

PEER_SLOTS = 4  # Native peer commands in flight at once, across all peers.


def completed(result) -> dict:
    return {"state": "completed", "result": result, "error": None}


def delivered(request: dict, route: dict) -> dict:
    """The claim as the relay hands it out, which the request hash covers."""
    return {**{key: value for key, value in request.items() if key != "computer_id"}, "gateway_route": route}


class GatewayTests(GatewayCase):
    async def asyncSetUp(self):
        await super().asyncSetUp()
        self.use(Fleet(Peer()))

    async def test_equal_session_names_reach_only_their_own_machine(self):
        process = await self.launch()
        await self.gateway_ready(process)
        local, remote, legacy = (self.submission("inspect", peer=False), self.submission("inspect"),
                                 self.submission("inspect", peer=False))
        self.assertEqual(await self.submit(local, process, gateway_route=self.local_route()), completed(inspection()))
        self.assertEqual(await self.submit(remote, process, gateway_route=self.route()),
                         completed(inspected(PEER_RUN, PEER_CONVERSATION)))
        self.assertEqual(await self.submit(legacy, process), completed(inspection()))
        [own] = self.guarded_calls(["inspect"])
        self.assertEqual(own["envelope"], {"schema": 1, "target_machine_id": LOCAL, "argv": ["inspect", SESSION],
                                           "payload": None})
        [forwarded] = self.through(argv=["inspect"])
        self.assertEqual(forwarded["envelope"], {"schema": 1, "via": VIA, "target_machine_id": PEER,
                                                 "argv": ["inspect", SESSION], "payload": None})
        self.assertEqual([call["argv"] for call in self.hgs.calls("inspect")], [["inspect", SESSION]])
        # Each machine keeps its own account catalogue.
        accounts = [row["id"] for row in self.latest()["snapshot"]["mobile_accounts"]["accounts"]]
        peer = self.relay.peer_heartbeats(ROUTE)[-1].body["snapshot"]["mobile_accounts"]["accounts"]
        self.assertEqual((accounts, [row["id"] for row in peer]), (["account-local"], ["account-peer"]))

    async def test_mutations_forward_exact_payloads_and_upload_their_results(self):
        process = await self.launch(env={key: "bb-calling-" + key.lower() for key in CONTEXT_KEYS})
        await self.gateway_ready(process)
        text = "literal $(touch ignored); 'quotes'\nЮникод"
        for peer, route, envelope in (
                (True, self.route(), {"schema": 1, "via": VIA, "target_machine_id": PEER}),
                (False, self.local_route(), {"schema": 1, "target_machine_id": LOCAL})):
            request = self.submission("send", peer=peer, text=text)
            result = await self.submit(request, process, gateway_route=route)
            run, conversation = (PEER_RUN, PEER_CONVERSATION) if peer else (RUN_ID, CONVERSATION_ID)
            self.assertEqual(result, completed({"request_id": request["request_id"], "status": "submitted",
                                                "name": SESSION, "run_id": run, "conversation_id": conversation,
                                                "text": text}))
            [call] = self.through(argv=["send"]) if peer else self.guarded_calls(["send"])
            self.assertEqual(call["envelope"], {**envelope, "argv": ["send", SESSION, "--json"],
                                                "payload": request["payload"]})
            self.assertEqual(len(call["stdin"].encode()), python_length(call["envelope"]))
            self.assertFalse(CONTEXT_KEYS & set(call["env"]), "calling agent identity reached native hgs")
            self.assertLessEqual(set(call["fds"]), {0, 1, 2})
            self.assertEqual(self.relay.state(request["request_id"]), "completed")
        self.assertEqual(self.hgs.calls("send"), [])

    async def test_routes_outside_the_current_inventory_fail_before_native_work(self):
        process = await self.launch()
        await self.gateway_ready(process)
        cases = {
            "another machine's route": (self.route(route_id=route_id("bb-third-peer", OTHER), machine_id=OTHER),
                                        ROUTE_CHANGED),
            "a known route to another machine": (self.route(machine_id=OTHER), ROUTE_CHANGED),
            "the gateway under another machine": (self.local_route(machine_id=OTHER), LOCAL_CHANGED),
            "an alias in the route": ({**self.route(), "via": VIA}, INVALID_ROUTE),
            "another schema": (self.route(schema=2), INVALID_ROUTE),
            "a boolean schema": (self.route(schema=True), INVALID_ROUTE),
            "a non-canonical route id": (self.route(route_id=ROUTE.upper()), INVALID_ROUTE),
            "a non-canonical computer id": (self.route(computer_id="not-a-uuid"), INVALID_ROUTE),
            "no fields": ({}, INVALID_ROUTE),
            "not an object": ([], INVALID_ROUTE),
        }
        for case, (route, message) in cases.items():
            request = self.submission("send", text="never forwarded")
            self.assertEqual(await self.submit(request, process, gateway_route=route),
                             {"state": "failed", "result": None, "error": message}, case)
            self.assertEqual(self.journal.row(request["request_id"])["state"], "failed", case)
        self.assertEqual((self.through(argv=["send"]), self.guarded_calls(["send"]), self.hgs.calls("send")),
                         ([], [], []))

    async def test_native_guard_refusal_after_dispatch_is_uncertain_and_never_repeated(self):
        # hgs itself refuses a target whose identity changed after discovery,
        # here after a folder creation's parent preflight has passed.
        folder = self.scoped("directory_create", path="/alias/new")
        self.use(Fleet(Peer(), help=gateway_help("directory_create"),
                       local=[(["dirs", "/alias"], {"stdout": {"path": "/example/gateway", "directories": []}})]))
        refusal = {"exit": 1, "stderr": "mobile target identity changed; refresh computers\n"}
        self.hgs.prepend(through_peer(VIA, PEER, ["send", SESSION, "--json"], **refusal),
                         guarded(["send", SESSION, "--json"], **refusal),
                         guarded(["dirs", "--create", "/alias/new", "--request-id", folder["request_id"]], **refusal))
        process = await self.launch()
        await self.gateway_ready(process)
        uncertain = {"state": "uncertain", "result": None, "error": NATIVE_FAILED}
        for request, route in ((self.submission("send", text="refused by the peer's guard"), self.route()),
                               (self.submission("send", peer=False, text="refused by the local guard"), self.local_route()),
                               (folder, self.local_route())):
            self.assertEqual(await self.submit(request, process, gateway_route=route), uncertain, request["operation"])
            self.assertEqual(await self.submit(request, process, gateway_route=route, redeliver=True), uncertain)
        self.assertEqual((len(self.through(argv=["send"])), len(self.guarded_calls(["send"])),
                          len(self.guarded_calls(["dirs", "--create"]))), (1, 1, 1))
        self.assertEqual(len(self.guarded_calls(["dirs", "/alias"])), 1)

    async def test_route_is_part_of_the_request_identity(self):
        process = await self.launch()
        await self.gateway_ready(process)
        route = self.route()
        request = self.submission("send", text="once through the peer")
        result = await self.submit(request, process, gateway_route=route)
        self.assertEqual(result["state"], "completed", result)
        row = await self.until(lambda: (row := self.journal.row(request["request_id"])) and row["delivered"] and row,
                               what="the delivered journal row")
        self.assertEqual(row["request_hash"], hashlib.sha256(canonical(delivered(request, route)).encode()).hexdigest())
        # A withdrawn peer still replays its preserved receipt.
        self.fleet.peers = []
        self.rescript()
        await self.manifest([], process)
        self.assertEqual(await self.submit(request, process, gateway_route=route, redeliver=True), result)
        # The same identity under another route is a conflict: never executed, never answered.
        mark, uploads = len(self.relay.deliveries), len(self.relay.uploads(request["request_id"]))
        await self.relay.queue(request, gateway_route=self.local_route(), redeliver=True)
        await self.settle(request["request_id"], mark, process)
        self.assertEqual(len(self.relay.uploads(request["request_id"])), uploads)
        self.assertEqual(self.journal.row(request["request_id"])["request_hash"], row["request_hash"])
        self.assertEqual((len(self.through(argv=["send"])), self.guarded_calls(["send"])), (1, []))

    async def test_restart_never_repeats_a_claimed_peer_mutation(self):
        self.hgs.prepend(through_peer(VIA, PEER, ["send", SESSION, "--json"],
                                      stdout=submitted(PEER_RUN, PEER_CONVERSATION), kill_parent=True))
        first = await self.launch()
        await self.gateway_ready(first)
        request = self.submission("send", text="native write before a crash")
        await self.relay.queue(request, gateway_route=self.route())
        self.assertEqual(await first.wait_exit(15), -signal.SIGKILL)
        self.assertEqual(self.journal.row(request["request_id"])["state"], "running")
        self.rescript()
        second = await self.launch()
        recovered = {"state": "uncertain", "result": None, "error": RESTART_ERROR}
        self.assertEqual(await self.until(lambda: self.relay.results(request["request_id"]), 15,
                                          "the recovered upload", second), [recovered])
        self.assertEqual(await self.submit(request, second, gateway_route=self.route(), redeliver=True), recovered)
        self.assertEqual(len(self.through(argv=["send"])), 1)

    async def test_peer_commands_share_four_native_slots(self):
        slow = {"stdout": peer_listing(), "delay": 2.5}
        self.use(Fleet(*(Peer(via=f"bb-slow-{index}", machine=synthetic_machine(index),
                              answers=[(["ls", "--json", "--local"], slow)]) for index in range(PEER_SLOTS + 2))))
        process = await self.launch()
        await self.until(lambda: sum(bool(self.hgs.finished(call["pid"])) for call in self.through(argv=["ls"]))
                         >= PEER_SLOTS + 2, 30, "every slow peer snapshot", process)
        changes = []
        for call in self.through():
            end = self.hgs.finished(call["pid"])
            changes += [(call["at"], 1), (end["at"] if end else float("inf"), -1)]
        running = most = 0
        for _, change in sorted(changes):  # An end sorts before a start at the same instant.
            running += change
            most = max(most, running)
        self.assertEqual(most, PEER_SLOTS)

    async def test_claims_never_wait_for_another_peer(self):
        stuck = Peer(via="bb-stuck-peer", machine=OTHER,
                     answers=[(["ls", "--json", "--local"], {"stdout": peer_listing(), "delay": 30})])
        fleet = self.use(Fleet(Peer(), stuck))
        process = await self.launch()
        await self.manifest(fleet.advertised(), process)
        await self.until(lambda: self.relay.peer_heartbeats(ROUTE) and any(
            one_hop(poll) and poll.status == 0 for poll in self.polls()), 15, "the responsive peer", process)
        [blocked] = self.through(machine=OTHER, argv=["ls"])
        # Waiting for the stuck peer's deadlines would take far longer than this.
        for request in (self.submission("inspect"), self.submission("send", text="not behind another peer")):
            result = await self.submit(request, process, timeout=5, gateway_route=self.route())
            self.assertEqual(result["state"], "completed", result)
        self.assertIsNone(self.hgs.finished(blocked["pid"]))

    async def test_claims_never_wait_for_discovery(self):
        process = await self.launch()
        await self.gateway_ready(process)
        self.fleet.discovery = {"delay": 30}
        rule = self.rescript()
        await self.until(lambda: self.discoveries(rule), 10, "the slow discovery", process)
        # A claim resolves against the current routes; waiting for discovery
        # would hold it until the discovery's own 12 s deadline.
        for request in (self.submission("inspect"), self.submission("send", text="not behind discovery")):
            result = await self.submit(request, process, timeout=5, gateway_route=self.route())
            self.assertEqual(result["state"], "completed", result)


if __name__ == "__main__":
    unittest.main()
