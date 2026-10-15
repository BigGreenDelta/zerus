"""Black-box peer route lifecycle: offline, reconnected, rebound and withdrawn
peers, and discovery that is slow.

A route is the stable identity of one alias and one verified machine. An
unreachable peer keeps its route only under its last verified identity and is
never contacted; any other change replaces or withdraws the route, and a
claim for an old route fails before native work.
"""
import unittest

from gateway_harness import (Fleet, GatewayCase, LOCAL, OTHER, PEER, PEER_CONVERSATION, PEER_RUN, Peer, ROUTE,
                             ROUTE_CHANGED, VIA, inspected, one_hop, peer_listing, query, route_id)

FAILED = {"state": "failed", "result": None, "error": ROUTE_CHANGED}


class PeerRouteTests(GatewayCase):
    async def test_unreachable_verified_peer_is_shared_offline_and_never_contacted(self):
        peer = Peer()
        fleet = self.use(Fleet(peer))
        process = await self.launch()
        await self.gateway_ready(process)
        peer.online = False
        self.rescript()
        body = await self.manifest(fleet.advertised(), process)
        self.assertEqual(body["peers"], [{"route_id": ROUTE, "machine_id": PEER, "name": "Synthetic peer",
                                          "online": False}])
        # A peer call that started before the change may still log its start.
        await self.heartbeats_after(1, process)
        contacted, published = len(self.through(via=VIA)), len(self.relay.peer_heartbeats(ROUTE))
        for request in (self.submission("inspect"), self.submission("send", text="never sent offline"),
                        self.scoped("catalog")):
            self.assertEqual(await self.submit(request, process, gateway_route=self.route(peer)), FAILED,
                             request["operation"])
        await self.heartbeats_after(2, process)
        self.assertEqual(len(self.through(via=VIA)), contacted)
        self.assertEqual(len(self.relay.peer_heartbeats(ROUTE)), published)
        # Reconnecting under the same verified identity restores the same route.
        peer.online = True
        self.rescript()
        await self.manifest(fleet.advertised(), process)
        await self.until(lambda: len(self.relay.peer_heartbeats(ROUTE)) > published, 15,
                         "a peer heartbeat after reconnection", process)
        result = await self.submit(self.submission("inspect"), process, gateway_route=self.route(peer))
        self.assertEqual(result, {"state": "completed", "result": inspected(PEER_RUN, PEER_CONVERSATION), "error": None})

    async def test_changed_identity_replaces_the_route_and_unverified_aliases_are_withdrawn(self):
        peer = Peer()
        fleet = self.use(Fleet(peer))
        process = await self.launch()
        await self.gateway_ready(process)
        old = self.route(peer)
        peer.online = False
        self.rescript()
        await self.manifest(fleet.advertised(), process)
        # The alias answers again, as another machine.
        peer.machine, peer.online = OTHER, True
        self.rescript()
        body = await self.manifest(fleet.advertised(), process)
        replacement = route_id(VIA, OTHER)
        self.assertEqual([(row["route_id"], row["machine_id"]) for row in body["peers"]], [(replacement, OTHER)])
        [first, *_] = await self.until(lambda: self.relay.peer_heartbeats(replacement), 15,
                                       "a heartbeat of the replacement route", process)
        self.assertEqual(first.body["machine_id"], OTHER)
        reached = len(self.through(machine=PEER))
        self.assertEqual(await self.submit(self.submission("send", text="never to the old machine"), process,
                                           gateway_route=old), FAILED)
        self.assertEqual(len(self.through(machine=PEER)), reached)
        self.assertEqual(self.through(argv=["send"]), [])
        result = await self.submit(self.submission("inspect"), process, gateway_route=self.route(peer))
        self.assertEqual(result["state"], "completed", result)
        # An unreachable alias without a verified identity is never shared.
        peer.machine, peer.online = None, False
        body = await self.rediscovered(process)
        self.assertEqual((body["machine_id"], body["peers"]), (LOCAL, []))

    async def test_rebound_and_withdrawn_routes_fail_before_native_work(self):
        peer = Peer()
        fleet = self.use(Fleet(peer))
        process = await self.launch()
        await self.gateway_ready(process)
        old = self.route(peer)
        peer.machine = OTHER
        self.rescript()
        await self.manifest(fleet.advertised(), process)
        rebound = self.route(peer)
        worktree = self.scoped("worktree_create", path="/example/peer", common_dir="/example/peer/.git",
                               destination="/example/peer/new", branch="feature", base="HEAD")
        for request in (self.submission("send", text="never after a rebind"), self.submission("inspect"),
                        self.scoped("directory_create", path="/example/new"), worktree):
            self.assertEqual(await self.submit(request, process, gateway_route=old), FAILED, request["operation"])
        fleet.peers = []
        self.rescript()
        await self.manifest([], process)
        self.assertEqual(await self.submit(self.submission("send", text="never after withdrawal"), process,
                                           gateway_route=rebound), FAILED)
        for argv in (["send"], ["inspect"], ["dirs"], ["worktrees"]):
            self.assertEqual(self.through(argv=argv), [], argv)

    async def test_slow_discovery_and_peer_snapshots_never_hold_local_heartbeats(self):
        slow = {"stdout": peer_listing(), "delay": 5}
        fleet = self.use(Fleet(Peer(answers=[(["ls", "--json", "--local"], slow)])))
        self.hgs.prepend({"argv": ["swarm", "mobile-peers", "--json"], "stdout": fleet.inventory(), "delay": 5,
                          "times": 1})
        process = await self.launch()

        def discovering():
            calls = self.discoveries()
            return calls and self.hgs.finished(calls[0]["pid"]) is None and calls[0]

        first = await self.until(discovering, 10, "the first discovery", process)
        await self.heartbeats_after(2, process)
        # One-hop polls can only follow a finished discovery: read them first.
        polls, beats = list(self.polls()), list(self.relay.heartbeats())
        self.assertIsNone(self.hgs.finished(first["pid"]), "local heartbeats waited for discovery")
        self.assertTrue(polls)
        self.assertEqual([(float(query(poll)["wait"]), one_hop(poll)) for poll in polls], [(1, False)] * len(polls))
        for beat in beats:
            self.assertNotIn("machine_id", beat.body)
            self.assertNotIn("gateway_one_hop", beat.body["snapshot"]["mobile_capabilities"]["features"])
        await self.manifest(fleet.advertised(), process)
        # The opt-in follows at once, as a full-length one-hop poll.
        guarded = await self.until(lambda: [poll for poll in self.polls() if one_hop(poll)], 10,
                                   "a one-hop poll", process)
        self.assertEqual(float(query(guarded[0])["wait"]), 25)
        [listing] = await self.until(lambda: self.through(argv=["ls"]), 10, "the peer snapshot", process)
        await self.heartbeats_after(2, process)
        self.assertIsNone(self.hgs.finished(listing["pid"]), "local heartbeats waited for a peer snapshot")
        await self.until(lambda: self.relay.peer_heartbeats(ROUTE), 15, "the slow peer heartbeat", process)


if __name__ == "__main__":
    unittest.main()
