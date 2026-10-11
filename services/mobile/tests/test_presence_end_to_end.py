"""Presence-aware alerts across the real connector, the Rust relay and the phone API."""
import asyncio
import importlib.util
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from aiohttp import ClientSession

from zerus_mobile.connector import Connector
from zerus_mobile.server import Config
from zerus_mobile.store import Store
from relay_fixture import start_relay

spec = importlib.util.spec_from_file_location("fixture_presence", Path(__file__).with_name("fixture_hgs.py"))
fixture_hgs = importlib.util.module_from_spec(spec)
spec.loader.exec_module(fixture_hgs)

FIRST, SECOND = "1" * 64, "2" * 64


@unittest.skipUnless(os.environ.get("ZERUS_RELAY_BINARY"), "presence alerts are evaluated by the Rust relay")
class PresenceEndToEndTests(unittest.IsolatedAsyncioTestCase):
    async def test_active_desktop_holds_read_drops_and_lock_delivers(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            fixture = Path(__file__).with_name("fixture_hgs.py")
            fixture.chmod(0o755)
            store = Store(root / "relay" / "state.sqlite3")
            workspace = store.workspace("Integration")
            node = store.node(workspace, "Fixture computer")
            invitation = store.invite(workspace)
            runner, url = await start_relay(store, Config(background=False))

            def presence(idle, locked=None):
                fixture_hgs.persist(root / "fixture-config.json", {"desktop_presence": {
                    "idle_seconds": idle, "locked": locked is not None, "locked_seconds": locked}})

            def session(**fields):
                fixture_hgs.persist(root / "session.json", {**fixture_hgs.initial(), **fields})

            def candidates():
                return store.db.execute("SELECT count(*) FROM alert_candidates").fetchone()[0]

            presence(1.0)
            session()
            connector = Connector({"server_url": url, "node_token": node["node_token"],
                "hgs_path": str(fixture), "state_dir": str(root / "journal"), "poll_interval": 1},
                allow_insecure_localhost=True)
            with patch.dict(os.environ, {"ZERUS_MOBILE_FIXTURE_DIR": directory}):
                task = asyncio.create_task(connector.run())
                try:
                    async with ClientSession() as client:
                        async with client.post(url + "/v1/pair", json={"code": invitation["pair_code"], "device_name": "Test phone"}) as response:
                            self.assertEqual(response.status, 200)
                            headers = {"Authorization": "Bearer " + (await response.json())["device_token"]}

                        async def get(path):
                            async with client.get(url + path, headers=headers) as response:
                                self.assertEqual(response.status, 200)
                                return await response.json()

                        async def kinds(kind):
                            return [e for e in (await get("/v1/events?after=0"))["events"] if e["kind"] == kind]

                        async def until(check):
                            for _ in range(200):
                                if await check():
                                    return
                                await asyncio.sleep(0.05)
                            self.fail("condition was not reached")

                        async def online():
                            computers = (await get("/v1/computers"))["computers"]
                            return bool(computers) and computers[0]["online"]

                        await until(online)
                        # Active at the desktop: the question is an event but not an alert.
                        question = {"phase": "input", "activity": "busy", "attention_id": "q1", "attention_signature": FIRST}
                        session(**question)
                        await until(lambda: kinds("attention"))
                        await asyncio.sleep(2.5)
                        self.assertEqual(await kinds("alert"), [])
                        self.assertEqual(candidates(), 1)
                        # Read on the desktop: the held candidate is dropped and never alerts.
                        read = {"conversation_id": "fixture-conversation", "reply_id": None, "attention_signature": FIRST, "at": 1.0}
                        session(**question, read=read)

                        async def dropped():
                            return candidates() == 0

                        await until(dropped)
                        presence(600.0)
                        await asyncio.sleep(2.5)
                        self.assertEqual(await kinds("alert"), [])
                        # A new question waits while active and alerts once the screen locks.
                        presence(1.0)
                        session(**{**question, "attention_id": "q2", "attention_signature": SECOND}, read=read)

                        async def second_question():
                            return len(await kinds("attention")) == 2

                        await until(second_question)
                        await asyncio.sleep(2.5)
                        self.assertEqual(await kinds("alert"), [])
                        presence(130.0, locked=130.0)
                        await until(lambda: kinds("alert"))
                        alert = (await kinds("alert"))[0]
                        self.assertEqual(set(alert), {"id", "computer_id", "session", "kind", "alert_kind", "created_at"})
                        self.assertEqual((alert["alert_kind"], alert["session"], alert["computer_id"]),
                                         ("attention", fixture_hgs.initial()["name"], node["node_id"]))
                        self.assertEqual(len(await kinds("alert")), 1)
                finally:
                    task.cancel()
                    await asyncio.gather(task, return_exceptions=True)
                    connector.journal.close()
                    await runner.cleanup()
                    store.close()


if __name__ == "__main__":
    unittest.main()
