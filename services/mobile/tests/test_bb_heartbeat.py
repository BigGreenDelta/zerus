"""Black-box heartbeat privacy: what of a native listing reaches the relay.

Every top-level key that `hgs ls --json --local` can emit is classified here
after review. A heartbeat carries only the forwarded ones plus the
connector's own projections, so a key that hgs gains fails these tests until
someone decides whether phones may see it.
"""
import os
import re
import unittest
import uuid

from connector_harness import BlackBoxCase, ROOT, default_rules, listing, session_row

LISTING_KEYS = {
    "host": "forwarded", "ok": "forwarded", "hgs_version": "forwarded", "projects": "forwarded",
    "archive_revision": "forwarded", "archive_paging": "forwarded", "sessions": "forwarded", "metrics": "forwarded", "preferences": "forwarded",
    "local_sessions": "forwarded", "local_host": "forwarded",
    "peers": "withheld",            # SSH aliases and the peer-of-peer inventory stay local.
    "ended_launches": "withheld",   # The last screen lines of failed starts.
    "desktop_presence": "beside",   # The heartbeat's `presence`, outside the snapshot.
}
CONNECTOR_KEYS = {"mobile_archive", "mobile_accounts", "mobile_projects", "mobile_capabilities"}
FORWARDED = {key for key, use in LISTING_KEYS.items() if use == "forwarded"} | CONNECTOR_KEYS
PEER_ALIAS = "bb-private-peer-alias"
SCREEN_TEXT = "BB-SCREEN-SENTINEL last terminal line"
PRESENCE = {"idle_seconds": 4.0, "locked": False, "locked_seconds": None}


def hgs_listing_keys(source: str) -> set[str]:
    """Top-level keys that src/cli.rs local_json can put in a listing."""
    start = source.index("fn local_json_archives(")
    body = source[start:source.index("\nfn ", start)]
    keys = set()
    for literal in re.findall(r"let mut raw = json!\(\{(.*?)\}\);", body):
        keys |= set(re.findall(r'"([a-z_]+)":', literal))
    return keys | set(re.findall(r'\b(?:raw|merged)\["([a-z_]+)"\]\s*=', body))


class HeartbeatPrivacyTests(BlackBoxCase):
    def full_listing(self) -> dict:
        busy = session_row(process_work={"count": 1, "started_at": 100.0, "command": "build --wait", "sampled_at": 200.0})
        return listing(archive_revision="0"*64,archive_paging=True,sessions=[busy], peers=[PEER_ALIAS + ".example.invalid"], local_sessions=True,
                       local_host={"state": "running"}, desktop_presence=PRESENCE,
                       ended_launches=[{"launch_id": str(uuid.uuid4()), "name": "claude/example/failed",
                                        "code": 1, "output": SCREEN_TEXT}])

    async def test_heartbeat_withholds_private_keys_and_carries_only_reviewed_ones(self):
        full = self.full_listing()
        self.assertEqual(set(full), set(LISTING_KEYS))
        self.hgs.script(default_rules(full))
        process = await self.launch()
        await self.until(lambda: len(self.relay.heartbeats()) >= 2, what="two heartbeats", process=process)
        for exchange in self.relay.heartbeats():
            body = exchange.body
            self.assertEqual(set(body), {"snapshot", "presence"})
            self.assertEqual(body["presence"], PRESENCE)
            snapshot = body["snapshot"]
            unreviewed = set(snapshot) - FORWARDED
            self.assertFalse(unreviewed, f"unreviewed heartbeat snapshot keys: {sorted(unreviewed)}")
            self.assertEqual(set(snapshot), FORWARDED, "a reviewed listing key went missing")
            for key in ("host", "hgs_version", "projects", "metrics", "preferences", "local_sessions", "local_host"):
                self.assertEqual(snapshot[key], full[key], key)
            for text in (PEER_ALIAS.encode(), SCREEN_TEXT.encode(), b"ended_launches", b"desktop_presence"):
                self.assertNotIn(text, exchange.raw)
            # Observation clocks would defeat the relay's unchanged-snapshot shortcut.
            self.assertEqual(snapshot["sessions"][0]["process_work"],
                             {"count": 1, "started_at": 100.0, "command": "build --wait"})

    def test_reviewed_listing_keys_match_the_hgs_source(self):
        cli = ROOT / "src/cli.rs"
        if not cli.exists():
            if os.environ.get("CI") == "true":
                self.fail("src/cli.rs is required in CI")
            self.skipTest("needs the hgs source tree")
        emitted = hgs_listing_keys(cli.read_text())
        self.assertIn("sessions", emitted, "the listing scan no longer finds local_json's keys")
        self.assertEqual(emitted - set(LISTING_KEYS), set(),
                         "review these new `hgs ls --json` keys for heartbeats and classify them here")
        self.assertEqual(set(LISTING_KEYS) - emitted, set(), "remove listing keys hgs no longer emits")

    async def test_presence_falls_back_for_a_relay_without_the_field(self):
        self.hgs.script(default_rules(listing(desktop_presence=PRESENCE)))
        self.relay.rule("POST", "/v1/node/heartbeat", 400, times=1, when=lambda exchange: "presence" in exchange.body)
        process = await self.launch()
        await self.until(lambda: len(self.relay.heartbeats()) >= 3, what="three heartbeats", process=process)
        attempts = [exchange for exchange in self.relay.exchanges if exchange.path == "/v1/node/heartbeat"]
        self.assertEqual([("presence" in exchange.body, exchange.status) for exchange in attempts[:4]],
                         [(True, 400), (False, 200), (False, 200), (False, 200)])


if __name__ == "__main__":
    unittest.main()
