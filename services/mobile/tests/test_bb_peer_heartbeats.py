"""Black-box peer heartbeats: what of a peer's listing reaches the relay.

A gateway posts each online peer's snapshot under its route with the same
privacy projection as its own, and the peer's desktop presence beside it. An
oversized or refused peer snapshot affects only that route.
"""
import json
import unittest
import uuid

from connector_harness import session_row
from gateway_harness import (Fleet, GatewayCase, LOCAL, MAX_PEER_SNAPSHOT, OTHER, PEER, PEER_CONVERSATION, PEER_RUN,
                             Peer, ROUTE, VIA, one_hop, peer_listing, synthetic_machine)
from test_bb_heartbeat import FORWARDED

PEER_PRESENCE = {"idle_seconds": 7.0, "locked": True, "locked_seconds": 3.0}
THIRD_PEER = "bb-third-peer.example.invalid"
SCREEN_TEXT = "BB-PEER-SCREEN-SENTINEL last terminal line"


def padded(size: int) -> dict:
    """A peer listing whose `hgs` output is exactly `size` bytes."""
    base = len(json.dumps(peer_listing(padding=""))) + 1
    return peer_listing(padding="x" * (size - base))


class PeerHeartbeatTests(GatewayCase):
    async def test_peer_heartbeats_carry_only_the_peer_projection(self):
        busy = session_row(run_id=PEER_RUN, conversation_id=PEER_CONVERSATION,
                           process_work={"count": 1, "started_at": 100.0, "command": "build --wait", "sampled_at": 200.0})
        failed_launch = {"launch_id": str(uuid.uuid4()), "name": "claude/example/failed", "code": 1, "output": SCREEN_TEXT}
        self.use(Fleet(Peer(snapshot=peer_listing(sessions=[busy], peers=[THIRD_PEER], ended_launches=[failed_launch],
                                                  desktop_presence=PEER_PRESENCE, local_sessions=True,
                                                  local_host={"state": "running"}))))
        process = await self.launch()
        await self.gateway_ready(process)
        await self.until(lambda: len(self.relay.peer_heartbeats(ROUTE)) >= 2, 15, "two peer heartbeats", process)
        self.assertEqual({exchange.status for exchange in self.peer_attempts()}, {200})
        for exchange in self.relay.peer_heartbeats(ROUTE):
            body = exchange.body
            self.assertEqual(set(body), {"machine_id", "snapshot", "presence"})
            self.assertEqual((body["machine_id"], body["presence"]), (PEER, PEER_PRESENCE))
            self.assertEqual(set(body["snapshot"]), FORWARDED)
            self.assertEqual(body["snapshot"]["sessions"][0]["process_work"],
                             {"count": 1, "started_at": 100.0, "command": "build --wait"})
            self.assertNotIn("gateway_one_hop", body["snapshot"]["mobile_capabilities"]["features"])
            for text in (THIRD_PEER, SCREEN_TEXT, "ended_launches", "desktop_presence", VIA):
                self.assertNotIn(text.encode(), exchange.raw)
        # The gateway's own heartbeat carries its own presence (none here), never a peer's.
        for beat in self.relay.heartbeats():
            self.assertIsNone(beat.body["presence"])
            self.assertNotIn(THIRD_PEER.encode(), beat.raw)

    async def test_oversized_peer_snapshots_are_withheld_from_their_route_only(self):
        # Under the 1 MiB output bound but over 1 MiB once projected; and over the bound itself.
        large = Peer(via="bb-large-peer", machine=synthetic_machine(1), snapshot=padded(MAX_PEER_SNAPSHOT - 64))
        huge = Peer(via="bb-huge-peer", machine=synthetic_machine(2), snapshot=padded(MAX_PEER_SNAPSHOT + 1))
        fleet = self.use(Fleet(Peer(), large, huge))
        process = await self.launch()
        await self.manifest(fleet.advertised(), process)
        await self.until(lambda: len(self.relay.peer_heartbeats(ROUTE)) >= 2 and all(
            sum(bool(self.hgs.finished(call["pid"])) for call in self.through(machine=peer.machine, argv=["ls"])) >= 2
            for peer in (large, huge)), 20, "two snapshots of every peer", process)
        self.assertEqual([(peer.via, self.peer_attempts(peer.route)) for peer in (large, huge)],
                         [(peer.via, []) for peer in (large, huge)])
        body = await self.heartbeats_after(1, process)
        self.assertEqual(body["peers"], fleet.advertised())
        self.assertTrue(all(row["online"] for row in body["peers"]))
        self.assertIn("gateway_one_hop", body["snapshot"]["mobile_capabilities"]["features"])

    async def test_a_missing_peer_route_never_downgrades_gateway_sharing(self):
        other = Peer(via="bb-other-peer", machine=OTHER, name="Other peer")
        fleet = self.use(Fleet(Peer(), other))
        self.relay.rule("POST", f"/v1/node/peers/{ROUTE}/heartbeat", 404)
        process = await self.launch()
        await self.manifest(fleet.advertised(), process)
        await self.until(lambda: len(self.peer_attempts(ROUTE)) >= 2 and len(self.relay.peer_heartbeats(other.route)) >= 2,
                         15, "refused and accepted peer heartbeats", process)
        self.assertEqual({exchange.status for exchange in self.peer_attempts(ROUTE)}, {404})
        body = await self.heartbeats_after(1, process)
        self.assertEqual((body["machine_id"], body["peers"]), (LOCAL, fleet.advertised()))
        self.assertIn("gateway_one_hop", body["snapshot"]["mobile_capabilities"]["features"])
        request = self.submission("inspect", peer=False)
        self.assertEqual((await self.submit(request, process, gateway_route=self.local_route()))["state"], "completed")
        self.assertTrue(one_hop(self.polls()[-1]))


if __name__ == "__main__":
    unittest.main()
