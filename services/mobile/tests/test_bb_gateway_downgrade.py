"""Black-box gateway-sharing downgrades against relays that lack it.

A relay that refuses the extended heartbeat (400, 404 or 422) or the peer
heartbeat endpoint (405) turns gateway sharing off: plain heartbeats, no peer
heartbeats, no one-hop polls. A transient failure does not. Divergence d:
Python keeps sharing off until restart and Rust probes again after 300 s;
within the shared tests both stay off, and a restart probes again in both.
A Rust-only test scales the 300 s with the test-hooks clock.
"""
import unittest

from connector_harness import CLOCK_SCALE_HOOK, IMPLEMENTATION
from gateway_harness import Fleet, GatewayCase, LOCAL, Peer, ROUTE, one_hop


def shape(exchange) -> str:
    """A heartbeat attempt: extended (gateway) or plain, with or without presence."""
    body = exchange.body
    return ("gateway" if "machine_id" in body else "plain") + ("+presence" if "presence" in body else "")


class GatewayDowngradeTests(GatewayCase):
    async def asyncSetUp(self):
        await super().asyncSetUp()
        self.use(Fleet(Peer()))

    def refused_from(self, status: int, mark: int = 0) -> list:
        """Heartbeat attempts from the first one refused with `status`: a
        request in flight when a rule is added may still meet it."""
        attempts = self.attempts(mark)
        return attempts[next(index for index, beat in enumerate(attempts) if beat.status == status):]

    async def downgraded(self, process, mark: int) -> None:
        """Wait for a plain heartbeat after `mark`, then wake the waiting
        one-hop poll with an unrouted claim, so that the last poll is current."""
        await self.until(lambda: any(beat.status == 200 and "machine_id" not in beat.body
                                     for beat in self.attempts(mark)), 15, "a plain heartbeat", process)
        request, deliveries = self.submission("inspect", peer=False), len(self.relay.deliveries)
        self.assertEqual((await self.submit(request, process))["state"], "completed")
        await self.settle(request["request_id"], deliveries, process)

    def assert_unshared(self, mark: int) -> None:
        """No heartbeat from the first plain one on shares peers, and the last poll does not opt in."""
        attempts = self.attempts(mark)
        first = next(index for index, beat in enumerate(attempts) if beat.status == 200 and "machine_id" not in beat.body)
        for beat in attempts[first:]:
            self.assertNotIn("machine_id", beat.body)
            self.assertNotIn("gateway_one_hop", beat.body["snapshot"]["mobile_capabilities"]["features"])
        self.assertFalse(one_hop(self.polls()[-1]))

    async def test_an_unsupported_peer_endpoint_stops_gateway_sharing(self):
        self.relay.rule("POST", r"/v1/node/peers/[^/]+/heartbeat", 405)
        process = await self.launch()
        await self.manifest(self.fleet.advertised(), process)
        mark = len(self.relay.exchanges)
        await self.until(lambda: self.peer_attempts(ROUTE), 15, "a refused peer heartbeat", process)
        await self.downgraded(process, mark)
        attempts = len(self.peer_attempts())
        await self.heartbeats_after(3, process)
        self.assertEqual(len(self.peer_attempts()), attempts)
        self.assertEqual({exchange.status for exchange in self.peer_attempts()}, {405})
        self.assert_unshared(mark)

    async def test_a_relay_without_gateway_support_downgrades_once_and_restart_probes_again(self):
        process = await self.launch()
        await self.gateway_ready(process)
        # The relay is rolled back to a version that knows only plain snapshots.
        mark = len(self.relay.exchanges)
        rule = self.relay.rule("POST", "/v1/node/heartbeat", 400, when=lambda exchange: set(exchange.body) != {"snapshot"})
        await self.downgraded(process, mark)
        await self.heartbeats_after(2, process)
        attempts = self.refused_from(400, mark)
        self.assertEqual([(shape(beat), beat.status) for beat in attempts[:4]],
                         [("gateway+presence", 400), ("gateway", 400), ("plain+presence", 400), ("plain", 200)])
        # Presence waits for its own 300 s probe; gateway sharing stays off.
        self.assertEqual({(shape(beat), beat.status) for beat in attempts[4:]}, {("plain", 200)})
        # Even once the relay accepts it again (divergence d, see the module docstring).
        self.relay.rules.remove(rule)
        published = len(self.peer_attempts())
        await self.heartbeats_after(3, process)
        self.assert_unshared(mark)
        self.assertEqual(len(self.peer_attempts()), published)
        process.stop()
        restarted = await self.launch()
        await self.gateway_ready(restarted)

    async def test_rust_shares_the_gateway_again_after_its_retry_time(self):
        # Divergence d: Rust offers the manifest again after 300 s, here
        # scaled to 3 s; Python only after a restart (the test above).
        if IMPLEMENTATION == "python":
            self.skipTest("Python keeps gateway sharing off until restart")
        process = await self.launch(env={CLOCK_SCALE_HOOK: "100"})
        await self.gateway_ready(process)
        mark = len(self.relay.exchanges)
        rule = self.relay.rule("POST", "/v1/node/heartbeat", 400, when=lambda exchange: set(exchange.body) != {"snapshot"})
        await self.downgraded(process, mark)
        self.relay.rules.remove(rule)
        await self.until(lambda: any(beat.status == 200 and "machine_id" in beat.body for beat in self.attempts(mark)),
                         20, "a shared heartbeat again", process)
        # Wake the long poll that started while sharing was off.
        self.assertEqual((await self.submit(self.submission("inspect", peer=False), process))["state"], "completed")
        await self.gateway_ready(process)

    async def test_refused_gateway_fields_downgrade_with_404_and_422(self):
        # The relay accepts presence but not the gateway fields.
        chains = {404: [("gateway+presence", 404), ("plain+presence", 200)],
                  422: [("gateway+presence", 422), ("gateway", 422), ("plain+presence", 200)]}
        for status, chain in chains.items():
            with self.subTest(status=status):
                process = await self.launch()
                try:
                    await self.gateway_ready(process)
                    mark = len(self.relay.exchanges)
                    self.relay.rule("POST", "/v1/node/heartbeat", status, when=lambda exchange: "machine_id" in exchange.body)
                    await self.downgraded(process, mark)
                    attempts = self.refused_from(status, mark)
                    self.assertEqual([(shape(beat), beat.status) for beat in attempts[:len(chain)]], chain)
                    self.assert_unshared(mark)
                finally:
                    process.stop()
                    self.relay.rules.clear()

    async def test_a_transient_heartbeat_failure_keeps_gateway_sharing(self):
        process = await self.launch()
        await self.gateway_ready(process)
        mark = len(self.relay.exchanges)
        self.relay.rule("POST", "/v1/node/heartbeat", 503, times=1, when=lambda exchange: "machine_id" in exchange.body)
        await self.until(lambda: any(beat.status == 503 for beat in self.attempts(mark)), 10, "the refusal", process)
        published = len(self.relay.peer_heartbeats(ROUTE))
        body = await self.heartbeats_after(2, process)
        self.assertEqual((body["machine_id"], body["peers"]), (LOCAL, self.fleet.advertised()))
        self.assertTrue(all("machine_id" in beat.body for beat in self.attempts(mark)))
        await self.until(lambda: len(self.relay.peer_heartbeats(ROUTE)) > published, 10, "a later peer heartbeat",
                         process)
        self.assertTrue(one_hop(self.polls()[-1]))


if __name__ == "__main__":
    unittest.main()
