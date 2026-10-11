"""Real HTTP relay + outbound connector + native-shaped subprocess integration."""
import asyncio
import base64
import hashlib
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
import uuid

from aiohttp import ClientSession

from zerus_mobile.connector import Connector
from zerus_mobile.server import Config
from relay_fixture import start_relay
from zerus_mobile.store import Store


class EndToEndTests(unittest.IsolatedAsyncioTestCase):
    async def test_pair_inspect_send_once_question_alert_and_revoke(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            fixture = Path(__file__).with_name("fixture_hgs.py")
            fixture.chmod(0o755)
            store = Store(root / "relay" / "state.sqlite3")
            workspace = store.workspace("Integration")
            node = store.node(workspace, "Fixture computer")
            invitation = store.invite(workspace)
            runner, url = await start_relay(store, Config(background=False))
            connector = Connector({"server_url": url, "node_token": node["node_token"],
                "hgs_path": str(fixture), "state_dir": str(root / "journal"), "poll_interval": 1},
                allow_insecure_localhost=True)
            with patch.dict(os.environ, {"ZERUS_MOBILE_FIXTURE_DIR": directory}):
                task = asyncio.create_task(connector.run())
                try:
                    async with ClientSession() as client:
                        async with client.post(url + "/v1/pair", json={"code": invitation["pair_code"], "device_name": "Test phone"}) as response:
                            self.assertEqual(response.status, 200)
                            phone = await response.json()
                        headers = {"Authorization": "Bearer " + phone["device_token"]}

                        async def call(method, path, body=None):
                            async with client.request(method, url + path, headers=headers, json=body) as response:
                                self.assertLess(response.status, 300)
                                return await response.json()

                        for _ in range(100):
                            computers = (await call("GET", "/v1/computers"))["computers"]
                            if computers[0]["online"]:
                                break
                            await asyncio.sleep(0.03)
                        self.assertTrue(computers[0]["online"])
                        session = computers[0]["snapshot"]["sessions"][0]
                        catalog = computers[0]["snapshot"]["mobile_projects"]
                        self.assertTrue(catalog["available"])
                        self.assertEqual(len(catalog["projects"]), 2)
                        self.assertEqual(session["mobile_project_id"], "33333333-3333-4333-8333-333333333333")

                        async def request(operation, payload):
                            request_id = str(uuid.uuid4())
                            if operation != "inspect":
                                payload = {**payload, "request_id": request_id,
                                           "expected_run_id": session["run_id"],
                                           "expected_conversation_id": session["conversation_id"]}
                            body = {"request_id": request_id, "computer_id": node["node_id"],
                                    "operation": operation, "session": session["name"], "payload": payload}
                            result = await call("POST", "/v1/requests", body)
                            for _ in range(200):
                                if result["state"] not in {"queued", "claimed"}:
                                    break
                                await asyncio.sleep(0.03)
                                result = await call("GET", "/v1/requests/" + request_id)
                            self.assertEqual(result["state"], "completed", result)
                            return body, result

                        _, inspected = await request("inspect", {})
                        self.assertEqual(inspected["result"]["events"][0]["type"], "UserPromptSubmit")
                        body, sent = await request("send", {"text": "Round trip from the phone"})
                        self.assertEqual(sent["result"]["status"], "submitted")
                        self.assertEqual((await call("POST", "/v1/requests", body))["state"], "completed")
                        self.assertEqual(len((root / "mutations.jsonl").read_text().splitlines()), 1)
                        _, inspected = await request("inspect", {})
                        self.assertIn("Round trip from the phone", inspected["result"]["provider_messages"][-1]["detail"])

                        file_bytes = b"synthetic attachment\n" * 60000  # >1MiB request transport.
                        file = {"name": "example.txt", "mime": "text/plain", "data_base64": base64.b64encode(file_bytes).decode(), "reference": "[File #1]"}
                        attachment_body, attachment_sent = await request("send", {"text": "Review [File #1]", "attachments": [file]})
                        self.assertEqual((await call("POST", "/v1/requests", attachment_body))["state"], "completed")
                        self.assertEqual(attachment_sent["result"]["text"], "Review [File #1]")
                        _, inspected = await request("inspect", {})
                        attachments = inspected["result"]["attachment_messages"][-1]
                        self.assertEqual(attachments["message_id"], attachment_body["request_id"])
                        self.assertEqual(attachments["attachments"][0]["bytes"], len(file_bytes))
                        self.assertEqual(len((root / "mutations.jsonl").read_text().splitlines()), 2)
                        # The phone reads the exact receipt file back in bounded ranges.
                        received = b""
                        while len(received) < len(file_bytes):
                            _, chunk = await request("attachment_read", {"message_id": attachment_body["request_id"], "index": 0,
                                "offset": len(received), "length": min(512 * 1024, len(file_bytes) - len(received))})
                            self.assertEqual((chunk["result"]["offset"], chunk["result"]["bytes"], chunk["result"]["file_name"]),
                                             (len(received), len(file_bytes), "example.txt"))
                            self.assertEqual(chunk["result"]["sha256"], hashlib.sha256(file_bytes).hexdigest())
                            received += base64.b64decode(chunk["result"]["data_base64"])
                        self.assertEqual(received, file_bytes)
                        foreign = str(uuid.uuid4())
                        missing = {"request_id": foreign, "computer_id": node["node_id"], "operation": "attachment_read", "session": session["name"],
                                   "payload": {"request_id": foreign, "expected_run_id": session["run_id"], "expected_conversation_id": session["conversation_id"],
                                               "message_id": str(uuid.uuid4()), "index": 0, "offset": 0, "length": 1}}
                        result = await call("POST", "/v1/requests", missing)
                        for _ in range(200):
                            if result["state"] not in {"queued", "claimed"}:
                                break
                            await asyncio.sleep(0.03)
                            result = await call("GET", "/v1/requests/" + foreign)
                        self.assertEqual(result["state"], "failed")
                        self.assertEqual(len((root / "mutations.jsonl").read_text().splitlines()), 2)

                        record = inspected["result"]
                        record["phase"] = "input"
                        record["activity"] = "busy"
                        record["attention_id"] = "fixture-question"
                        record["pending_questions"] = [{"question_id": "fixture-question", "question_hash": "fixture-hash",
                            "run_id": record["run_id"], "conversation_id": record["conversation_id"], "can_answer": True,
                            "questions": [{"id": "q_0", "question": "Proceed with the integration test?", "options": [
                                {"id": "opt_0_0", "label": "Proceed"}, {"id": "opt_0_1", "label": "Wait"}]}]}]
                        (root / "session.json").write_text(json.dumps(record))
                        result = await call("GET", "/v1/events?after=0&wait=5")
                        self.assertTrue(any(event["kind"] == "attention" for event in result["events"]))
                        await request("answer", {"question_id": "fixture-question", "expected_question_hash": "fixture-hash",
                            "answers": [{"question_id": "q_0", "selected_option_ids": ["opt_0_0"], "text": ""}]})
                        self.assertEqual(len((root / "mutations.jsonl").read_text().splitlines()), 3)
                        await call("DELETE", "/v1/device")
                        async with client.get(url + "/v1/computers", headers=headers) as response:
                            self.assertEqual(response.status, 401)
                finally:
                    task.cancel()
                    await asyncio.gather(task, return_exceptions=True)
                    connector.journal.close()
                    await runner.cleanup()
                    store.close()


if __name__ == "__main__":
    unittest.main()
