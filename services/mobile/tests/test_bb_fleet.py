"""Black-box fleet discovery: which direct peers a gateway shares, by which
route IDs and names, and when it opts into one-hop claims.

Only the native inventory (`hgs swarm mobile-peers --json`) names peers. An
invalid inventory shares none of them, and the relay never learns an alias.
"""
import json
import unittest

from connector_harness import HELP_TEXT, SESSION, inspection
from gateway_harness import (DEFAULT_NAME, Fleet, GatewayCase, LOCAL, MAX_PEERS, OMITTED, Peer, VECTOR_ROUTES,
                             help_text, one_hop, query, route_id, synthetic_machine)

# The vectors' second machine, reached through two different aliases.
SHARED_MACHINE = next(machine for via, machine in VECTOR_ROUTES if via == "peer.example.test")


def aliases(*vias: str) -> list[bytes]:
    """Each alias as raw UTF-8 and as JSON \\u escapes."""
    return [form for via in vias for form in (via.encode(), json.dumps(via)[1:-1].encode())]


class FleetTests(GatewayCase):
    def test_route_ids_follow_the_vector_rule(self):
        for (via, machine_id), route in VECTOR_ROUTES.items():
            self.assertEqual(route_id(via, machine_id), route, via)

    async def test_peers_are_shared_by_route_id_without_their_aliases(self):
        peers = [Peer(), Peer(via="peer.example.test", machine=SHARED_MACHINE, name="Example peer"),
                 Peer(via="пир", machine=SHARED_MACHINE, name="Пир")]
        self.use(Fleet(*peers))
        process = await self.launch()
        await self.gateway_ready(process)
        body = self.latest()
        self.assertEqual(body["machine_id"], LOCAL)
        self.assertEqual(body["peers"], sorted([
            {"route_id": VECTOR_ROUTES[(peer.via, peer.machine)], "machine_id": peer.machine,
             "name": peer.name, "online": True} for peer in peers], key=lambda row: row["route_id"]))
        self.assertIn("gateway_one_hop", body["snapshot"]["mobile_capabilities"]["features"])
        for peer in peers:
            [first, *_] = self.relay.peer_heartbeats(VECTOR_ROUTES[(peer.via, peer.machine)])
            self.assertEqual(first.body["machine_id"], peer.machine)
        for exchange in self.relay.exchanges:
            for alias in aliases(*(peer.via for peer in peers)):
                self.assertNotIn(alias, exchange.raw, exchange.path)
        # The opt-in comes with the shared manifest, as a full-length poll.
        guarded = [poll for poll in self.polls() if one_hop(poll)]
        self.assertTrue(guarded)
        self.assertTrue(all(float(query(poll)["wait"]) == 25 for poll in guarded))

    async def test_peer_names_are_projected_within_the_manifest_bounds(self):
        names = ["N" * 256, "ü" * 200, "Lab\x00\x1b[31m Mac\x7f", 7, None, OMITTED, "\x01\x02"]
        # Offline peers with a verified identity are shared but never contacted.
        self.use(Fleet(*(Peer(via=f"bb-name-{index}", machine=synthetic_machine(index), name=name, online=False)
                         for index, name in enumerate(names))))
        process = await self.launch()
        body = await self.manifest(self.fleet.advertised(), process)
        projected = {row["machine_id"]: row["name"] for row in body["peers"]}
        self.assertEqual([projected[synthetic_machine(index)] for index in range(len(names))],
                         ["N" * 128, "ü" * 128, "Lab[31m Mac", DEFAULT_NAME, DEFAULT_NAME, DEFAULT_NAME, DEFAULT_NAME])
        self.assertEqual(self.through(), [])

    async def test_invalid_inventories_fail_closed_and_keep_the_local_identity(self):
        def offline(count: int) -> list[Peer]:
            return [Peer(via=f"bb-peer-{index}", machine=synthetic_machine(index), online=False)
                    for index in range(count)]

        fleet = self.use(Fleet(*offline(MAX_PEERS)))
        process = await self.launch()
        body = await self.manifest(fleet.advertised(), process)
        self.assertEqual(len(body["peers"]), MAX_PEERS)
        valid = fleet.inventory()
        rows = valid["peers"][:2]
        upper = dict(rows[0], machine_id=rows[0]["machine_id"].upper())
        inventories = {
            "more than MAX_PEERS peers": {"stdout": Fleet(*offline(MAX_PEERS + 1)).inventory()},
            "a repeated alias": {"stdout": dict(valid, peers=[rows[0], dict(rows[1], via=rows[0]["via"])])},
            "a non-canonical machine id": {"stdout": dict(valid, peers=[upper, rows[1]])},
            "a non-boolean online flag": {"stdout": dict(valid, peers=[dict(rows[0], online="yes"), rows[1]])},
            "an alias with a control character": {"stdout": dict(valid, peers=[dict(rows[0], via="bb\npeer"), rows[1]])},
            "another schema": {"stdout": dict(valid, schema=2)},
            "a failing exit status": {"stdout": valid, "exit": 1},
            # Over the 128 KiB discovery output bound, padded after valid JSON.
            "output beyond 128 KiB": {"stdout_text": json.dumps(valid).ljust(128 * 1024 + 1)},
        }
        for case, answer in inventories.items():
            fleet.discovery = answer
            body = await self.rediscovered(process)
            self.assertEqual((body.get("machine_id"), body.get("peers")), (LOCAL, []), case)
        fleet.discovery = {}
        self.rescript()
        await self.manifest(fleet.advertised(), process)

    async def test_failed_first_discovery_never_opts_in_and_recovers(self):
        fleet = self.use(Fleet(Peer(), discovery={"exit": 1, "stdout_text": ""}))
        process = await self.launch()
        await self.until(lambda: len([call for call in self.discoveries() if self.hgs.finished(call["pid"])]) >= 2,
                         10, "two failed discoveries", process)
        await self.heartbeats_after(2, process)
        for beat in self.relay.heartbeats():
            self.assertNotIn("machine_id", beat.body)
            self.assertNotIn("gateway_one_hop", beat.body["snapshot"]["mobile_capabilities"]["features"])
        self.assertFalse(any(one_hop(poll) for poll in self.polls()))
        # Local claims without a route keep working meanwhile.
        legacy = self.submission("inspect", peer=False)
        self.assertEqual((await self.submit(legacy, process))["state"], "completed")
        fleet.discovery = {}
        self.rescript()
        await self.gateway_ready(process)

    async def test_native_without_the_peer_abi_is_never_asked_for_peers(self):
        # Without both markers (the inventory line alone is not enough) the
        # gateway stays an ordinary computer, even with an inventory scripted.
        for help in (HELP_TEXT, help_text("peer_inventory")):
            with self.subTest(help=help):
                self.use(Fleet(Peer(), help=help))
                beats, polls, inspections = len(self.relay.heartbeats()), len(self.polls()), len(self.hgs.calls("inspect"))
                process = await self.launch()
                try:
                    await self.heartbeats_after(3, process)
                    request = self.submission("inspect", peer=False)
                    self.assertEqual(await self.submit(request, process),
                                     {"state": "completed", "result": inspection(), "error": None})
                    self.assertEqual(self.discoveries(), [])
                    self.assertTrue(all("machine_id" not in beat.body for beat in self.relay.heartbeats()[beats:]))
                    self.assertFalse(any(one_hop(poll) for poll in self.polls()[polls:]))
                    self.assertEqual([call["argv"] for call in self.hgs.calls("inspect")[inspections:]],
                                     [["inspect", SESSION]])
                    self.assertEqual(self.guarded_calls(), [])
                finally:
                    process.stop()


if __name__ == "__main__":
    unittest.main()
