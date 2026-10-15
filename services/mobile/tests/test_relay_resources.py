"""Resource regressions use disposable SQLite state and loopback HTTP only."""
import asyncio
import gc
import weakref
import io
import json
import random
from pathlib import Path
import sqlite3
import tempfile
import threading
import time
import unittest
from unittest.mock import patch
import uuid

from aiohttp.test_utils import TestClient, TestServer

from zerus_mobile.server import Config, create_app
from zerus_mobile.store import Store


class RelayResourceTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="zerus-relay-resources-")
        self.database = Path(self.directory.name) / "relay.sqlite3"
        self.store = Store(self.database)
        self.workspace = self.store.workspace("Synthetic resources")
        self.node = self.store.node(self.workspace, "Synthetic computer")
        self.phone = self.store.pair(self.store.invite(self.workspace)["pair_code"], "Synthetic phone")
        self.config = Config(background=False)
        self.client = None

    async def asyncTearDown(self):
        if self.client:
            await self.client.close()
        self.store.close()
        self.directory.cleanup()

    async def start(self):
        self.client = TestClient(TestServer(create_app(self.store, self.config), handler_cancellation=True))
        await self.client.start_server()

    def headers(self, node=False, **extra):
        token = self.node["node_token"] if node else self.phone["device_token"]
        return {"Authorization": "Bearer " + token, **extra}

    async def request(self, method, route, expected, **kwargs):
        async with self.client.request(method, route, **kwargs) as response:
            value = await response.json()
            self.assertEqual(response.status, expected, value)
            return value

    async def exhaust_pairing(self, forwarded):
        for _ in range(10):
            await self.request("POST", "/v1/pair", 401,
                               json={"code": "synthetic-invalid", "device_name": "Synthetic phone"},
                               headers={"X-Forwarded-For": forwarded})

    async def test_proxy_clients_have_independent_pairing_quotas(self):
        self.config.trusted_proxy_cidrs = ("127.0.0.1/32",)
        invitation = self.store.invite(self.workspace)
        await self.start()
        await self.exhaust_pairing("192.0.2.1")
        await self.request("POST", "/v1/pair", 429,
                           json={"code": "synthetic-invalid", "device_name": "Synthetic phone"},
                           headers={"X-Forwarded-For": "192.0.2.1"})
        await self.request("POST", "/v1/pair", 200,
                           json={"code": invitation["pair_code"], "device_name": "Other synthetic phone"},
                           headers={"X-Forwarded-For": "192.0.2.2"})

    async def test_untrusted_peer_cannot_rotate_forwarding_headers(self):
        await self.start()
        await self.exhaust_pairing("192.0.2.1")
        await self.request("POST", "/v1/pair", 429,
                           json={"code": "synthetic-invalid", "device_name": "Synthetic phone"},
                           headers={"X-Forwarded-For": "192.0.2.2", "Forwarded": "for=198.51.100.2"})

    async def test_trusted_chain_uses_nearest_untrusted_hop(self):
        self.config.trusted_proxy_cidrs = ("127.0.0.1/32", "192.0.2.0/24")
        self.config.max_forwarded_hops = 3
        await self.start()
        await self.exhaust_pairing("203.0.113.1, 198.51.100.1, 192.0.2.10")
        await self.request("POST", "/v1/pair", 429,
                           json={"code": "synthetic-invalid", "device_name": "Synthetic phone"},
                           headers={"X-Forwarded-For": "203.0.113.2, 198.51.100.1, 192.0.2.10"})

    async def test_anonymous_abuse_preserves_authenticated_and_health_access(self):
        await self.start()
        for index in range(40):
            async with self.client.get("/v1/computers", headers={"X-Forwarded-For": f"192.0.2.{index + 1}"}) as response:
                self.assertIn(response.status, (401, 429))
                await response.read()
        await self.request("GET", "/v1/computers", 200, headers=self.headers())
        await self.request("GET", "/healthz", 200)

    async def test_external_sqlite_writer_does_not_stall_health(self):
        await self.start()
        locked, release = threading.Event(), threading.Event()
        failures = []

        def writer():
            connection = None
            try:
                connection = sqlite3.connect(self.database)
                connection.execute("BEGIN IMMEDIATE")
                locked.set()
                release.wait(2)
                connection.rollback()
            except Exception as error:
                failures.append(error)
                locked.set()
            finally:
                if connection:
                    connection.close()

        thread = threading.Thread(target=writer)
        thread.start()
        await asyncio.to_thread(locked.wait, 2)
        self.assertFalse(failures)
        write = asyncio.create_task(self.request("POST", "/v1/node/heartbeat", 200,
                                   headers=self.headers(True), json={"snapshot": {"sessions": []}}))
        try:
            await asyncio.sleep(0.05)
            start = time.perf_counter()
            await self.request("GET", "/healthz", 200)
            self.assertLess(time.perf_counter() - start, 0.3)
            self.assertTrue(thread.is_alive(), "health completed only after the database lock released")
        finally:
            release.set()
            await asyncio.to_thread(thread.join, 3)
            await write
        self.assertFalse(failures)

    async def open_poll(self, route, headers):
        reader, writer = await asyncio.open_connection(self.client.server.host, self.client.server.port)
        lines = [f"GET {route} HTTP/1.1", f"Host: {self.client.server.host}",
                 *(f"{key}: {value}" for key, value in headers.items()), "", ""]
        writer.write("\r\n".join(lines).encode())
        await writer.drain()
        return reader, writer

    async def await_poll_status(self, route, headers, expected):
        deadline = time.monotonic() + 2
        while True:
            async with self.client.get(route, headers=headers) as response:
                value = await response.json()
                if response.status == expected:
                    return value
                self.assertIn(response.status, (200, 429), value)
                if time.monotonic() >= deadline:
                    self.fail(f"poll status did not become {expected}: {response.status} {value}")
            await asyncio.sleep(0.01)

    async def test_long_poll_overload_and_disconnect_release_capacity(self):
        self.config.max_polls_per_credential = 1
        self.config.max_polls_per_workspace = 2
        self.config.max_polls_global = 4
        await self.start()
        _, writer = await self.open_poll("/v1/node/requests?wait=25", self.headers(True))
        try:
            await self.await_poll_status("/v1/node/requests?wait=0.01", self.headers(True), 429)
            await self.request("GET", "/healthz", 200)
        finally:
            writer.close()
            await writer.wait_closed()
        await self.await_poll_status("/v1/node/requests?wait=0.01", self.headers(True), 200)

    async def test_workspace_poll_limit_spans_distinct_credentials(self):
        self.config.max_polls_per_credential = 2
        self.config.max_polls_per_workspace = 1
        self.config.max_polls_global = 4
        await self.start()
        _, writer = await self.open_poll("/v1/node/requests?wait=25", self.headers(True))
        try:
            await self.await_poll_status("/v1/events?wait=0.01", self.headers(), 429)
        finally:
            writer.close()
            await writer.wait_closed()

    async def test_disconnected_large_upload_releases_capacity(self):
        await self.start()
        _, writer = await asyncio.open_connection(self.client.server.host, self.client.server.port)
        headers = ["POST /v1/requests HTTP/1.1", f"Host: {self.client.server.host}",
                   "Content-Type: application/json", "Content-Length: 2097152",
                   "Authorization: " + self.headers()["Authorization"], "", "{"]
        writer.write("\r\n".join(headers).encode())
        await writer.drain()
        await asyncio.sleep(0.05)
        writer.close()
        await writer.wait_closed()
        await asyncio.sleep(0.05)
        async with self.client.post("/v1/requests", data=io.BytesIO(b" " * (1024 * 1024 + 1) + b"{}"),
                                   headers={**self.headers(), "Content-Type": "application/json"}) as response:
            self.assertIn(response.status, (400, 413), await response.text())

    def command(self, operation="inspect"):
        request_id = str(uuid.uuid4())
        payload = {} if operation == "inspect" else dict(request_id=request_id,
                   expected_run_id="synthetic-run", expected_conversation_id="synthetic-conversation", text="Synthetic text")
        return dict(request_id=request_id, computer_id=self.node["node_id"], operation=operation,
                    session="synthetic-session", payload=payload)

    async def test_many_small_requests_with_large_results_reach_workspace_budget(self):
        self.config.max_queue_bytes = 3 * 1024 * 1024
        await self.start()
        admitted = []
        for _ in range(8):
            command = self.command()
            async with self.client.post("/v1/requests", json=command, headers=self.headers()) as response:
                value = await response.json()
                if response.status == 429:
                    break
                self.assertEqual(response.status, 202, value)
            claim = await self.request("GET", "/v1/node/requests", 200, headers=self.headers(True))
            self.assertEqual(claim["requests"][0]["request_id"], command["request_id"])
            body = dict(state="completed", result={"synthetic": "x" * 800000}, error=None)
            await self.request("POST", f'/v1/node/requests/{command["request_id"]}/result', 200,
                               json=body, headers=self.headers(True))
            await self.request("POST", f'/v1/node/requests/{command["request_id"]}/result', 200,
                               json=body, headers=self.headers(True))
            admitted.append(command)
        else:
            self.fail("eight large results bypassed a 3MiB workspace budget")
        self.assertTrue(admitted)
        for command in admitted:
            receipt = await self.request("POST", "/v1/requests", 202, json=command, headers=self.headers())
            self.assertEqual(receipt["state"], "completed")
        self.assertEqual((await self.request("GET", "/v1/node/requests", 200,
                         headers=self.headers(True)))["requests"], [])

    async def test_canonical_result_expansion_is_bounded_and_receipt_remains_safe(self):
        await self.start()
        command = self.command("send")
        await self.request("POST", "/v1/requests", 202, json=command, headers=self.headers())
        await self.request("GET", "/v1/node/requests", 200, headers=self.headers(True))
        result_route = f'/v1/node/requests/{command["request_id"]}/result'
        raw = json.dumps(dict(state="completed", result={"synthetic": "\u0800" * 200000}, error=None),
                         ensure_ascii=False).encode()
        self.assertLess(len(raw), 1024 * 1024)
        await self.request("POST", result_route, 413, data=io.BytesIO(raw),
                           headers={**self.headers(True), "Content-Type": "application/json"})
        receipt = await self.request("GET", f'/v1/requests/{command["request_id"]}', 200,
                                     headers=self.headers())
        self.assertEqual(receipt["state"], "claimed")
        await self.request("POST", result_route, 200, json=dict(state="completed", result={"status": "submitted"}, error=None),
                           headers=self.headers(True))


    async def test_slow_pair_bodies_preserve_authenticated_capacity(self):
        self.config.max_anonymous_requests = 2
        await self.start()
        writers = []
        try:
            for _ in range(2):
                reader, writer = await asyncio.open_connection(self.client.server.host, self.client.server.port)
                writers.append(writer)
                writer.write(b"POST /v1/pair HTTP/1.1\r\nHost: localhost\r\nContent-Type: application/json\r\nContent-Length: 100\r\n\r\n{")
                await writer.drain()
            await asyncio.sleep(0.05)
            await self.request("POST", "/v1/pair", 429, json={"code": "invalid", "device_name": "Synthetic"})
            await self.request("GET", "/v1/computers", 200, headers=self.headers())
            await self.request("GET", "/healthz", 200)
        finally:
            for writer in writers:
                writer.close()
                await writer.wait_closed()

    async def test_oversized_event_cursor_is_rejected(self):
        await self.start()
        await self.request("GET", "/v1/events?after=9223372036854775808", 400, headers=self.headers())

    async def test_catalog_payload_limit_applies_before_snapshot_fetch(self):
        self.config.max_catalog_bytes = 100
        self.store.db.execute("UPDATE nodes SET snapshot=? WHERE id=?", (json.dumps({"sessions": [], "synthetic": "x" * 200}), self.node["node_id"]))
        self.store.db.commit()
        await self.start()
        await self.request("GET", "/v1/computers", 413, headers=self.headers())
        await self.request("GET", "/healthz", 200)

    async def test_dense_and_deep_json_are_rejected_before_loads(self):
        from zerus_mobile.ingress import parse_json
        for raw in (bytearray(b"[" + b"0," * 2000 + b"0]"), bytearray(b"[" * 33 + b"0" + b"]" * 33)):
            with patch("zerus_mobile.ingress.json.loads", side_effect=AssertionError("full parse must not run")):
                with self.assertRaises(ValueError):
                    await asyncio.to_thread(parse_json, raw, 32, 1000)

    async def test_cancelled_thread_keeps_capacity_until_actual_completion(self):
        from zerus_mobile.ingress import blocking, Capacity
        capacity = Capacity(1)
        started, finish = threading.Event(), threading.Event()
        def worker():
            started.set()
            finish.wait(2)
        async def operation():
            capacity.take()
            try:
                await blocking(worker)
            finally:
                capacity.release()
        task = asyncio.create_task(operation())
        await asyncio.to_thread(started.wait, 2)
        task.cancel()
        await asyncio.sleep(0.03)
        self.assertFalse(task.done())
        self.assertEqual(capacity.used, 1)
        finish.set()
        with self.assertRaises(asyncio.CancelledError):
            await task
        self.assertEqual(capacity.used, 0)

    async def test_cancelled_spool_thread_closes_discarded_result(self):
        from zerus_mobile.ingress import blocking
        started, finish = threading.Event(), threading.Event()
        target = io.BytesIO()
        def worker():
            started.set()
            finish.wait(2)
            return target
        task = asyncio.create_task(blocking(worker))
        await asyncio.to_thread(started.wait, 2)
        task.cancel()
        finish.set()
        with self.assertRaises(asyncio.CancelledError):
            await task
        self.assertTrue(target.closed)

    async def test_streaming_encoder_bounds_a_single_large_json_string(self):
        from zerus_mobile.ingress import _encoded, _spool
        value = {"synthetic": "x" * (2 * 1024 * 1024) + "\\\"\n\u0800"}
        chunks = list(_encoded(value))
        self.assertLessEqual(max(map(len, chunks)), 6 * 8192)
        target = _spool(value, 3 * 1024 * 1024)
        try:
            self.assertEqual(json.load(target), value)
        finally:
            target.close()

    async def test_maintenance_and_push_resume_after_transient_failure(self):
        from zerus_mobile.server import STORE, PUSH_WORKER, maintenance, push_background
        await self.start()
        app = self.client.server.app
        count = {"maintain": 0, "push": 0}
        finished = {"maintain": asyncio.Event(), "push": asyncio.Event()}
        async def fail_once(name):
            count[name] += 1
            if count[name] == 1:
                raise RuntimeError("synthetic transient failure")
            finished[name].set()
        async def maintain_stub(**kwargs): await fail_once("maintain")
        async def push_stub(): await fail_once("push")
        self.config.maintenance_interval = 0.01
        with patch.object(app[STORE], "maintain", maintain_stub), patch.object(app[PUSH_WORKER], "once", push_stub):
            tasks = [asyncio.create_task(maintenance(app)), asyncio.create_task(push_background(app))]
            try:
                await asyncio.wait_for(asyncio.gather(*(event.wait() for event in finished.values())), 2)
            finally:
                for task in tasks: task.cancel()
                await asyncio.gather(*tasks, return_exceptions=True)
        self.assertGreaterEqual(count["maintain"], 2)
        self.assertGreaterEqual(count["push"], 2)


    async def test_idle_polls_leave_ordinary_authenticated_admission_free(self):
        from zerus_mobile.server import POLLS
        self.config.max_authenticated_requests = 2
        await self.start()
        polls = [asyncio.create_task(self.client.get("/v1/events?wait=25", headers=self.headers())) for _ in range(2)]
        try:
            for _ in range(100):
                if self.client.server.app[POLLS].used == 2: break
                await asyncio.sleep(0.01)
            self.assertEqual(self.client.server.app[POLLS].used, 2)
            await self.request("POST", "/v1/node/heartbeat", 200, headers=self.headers(True), json={"snapshot": {"sessions": []}})
            await self.request("POST", "/v1/requests", 202, headers=self.headers(), json=self.command())
        finally:
            for task in polls: task.cancel()
            await asyncio.gather(*polls, return_exceptions=True)

    async def test_readiness_flood_coalesces_database_probes(self):
        from zerus_mobile.server import STORE
        await self.start()
        app = self.client.server.app
        started, finish = asyncio.Event(), asyncio.Event()
        count = 0
        async def ready():
            nonlocal count
            count += 1
            started.set()
            await finish.wait()
            return True
        with patch.object(app[STORE], "ready", ready):
            first = asyncio.create_task(self.client.get("/readyz"))
            await asyncio.wait_for(started.wait(), 1)
            try:
                for _ in range(10):
                    await self.request("GET", "/readyz", 503)
                await self.request("GET", "/healthz", 200)
                self.assertEqual(count, 1)
                finish.set()
                response = await first
                await response.read()
                self.assertEqual(response.status, 200)
                await self.request("GET", "/readyz", 200)
                self.assertEqual(count, 1)
            finally:
                finish.set()
                await first

    async def test_cancelled_poll_keeps_slot_until_durable_cleanup_finishes(self):
        from zerus_mobile.server import POLLS, STORE, release_poll
        await self.start()
        app = self.client.server.app
        started, finish = asyncio.Event(), asyncio.Event()
        async def cleanup(lease):
            started.set()
            await finish.wait()
        class Request:
            pass
        request = Request()
        request.app = app
        app[POLLS].take()
        with patch.object(app[STORE], "release_poll", cleanup):
            task = asyncio.create_task(release_poll(request, "synthetic-lease"))
            await started.wait()
            for _ in range(3):
                task.cancel()
                await asyncio.sleep(0)
                self.assertFalse(task.done())
                self.assertEqual(app[POLLS].used, 1)
            finish.set()
            with self.assertRaises(asyncio.CancelledError):
                await task
            self.assertEqual(app[POLLS].used, 0)

    async def test_byte_parser_preserves_stdlib_json_semantics(self):
        from zerus_mobile.ingress import parse_json
        valid = [b'{"key":1,"key":2}', b'{"n":18446744073709551617}', b'"\\ud800"',
                 b'"\\ud83d\\ude00"', b'"\\\\u0800"', b'1.25e-10', b'-0', b'[]', b'{}']
        valid += [json.dumps({"text": "a\n" * 32768}).encode(), b'"' + b'\\/' * 40000 + b'"']
        for raw in valid:
            self.assertEqual(parse_json(bytearray(raw), 32, 50000), json.loads(raw))
        for raw in (b'[1,]', b'{"x":1,}', b'{"x" 1}', b'"raw\ncontrol"', b'01', b'1.', b'NaN', b'Infinity', b'"\\x"', b'"\\uZZZZ"', b'[] true'):
            with self.assertRaises(ValueError, msg=repr(raw)):
                parse_json(bytearray(raw), 32, 50000)
        rng = random.Random(62731)
        def sample(depth=0):
            if depth > 3:
                return rng.choice([None, True, False, rng.randrange(-2**100, 2**100), rng.random(), "x\n\u0800\U0001f600"])
            return rng.choice([lambda: [sample(depth + 1) for _ in range(rng.randrange(4))],
                               lambda: {str(i): sample(depth + 1) for i in range(rng.randrange(4))},
                               lambda: sample(4)])()
        for _ in range(2000):
            value = sample()
            raw = json.dumps(value, ensure_ascii=rng.choice([False, True])).encode()
            self.assertEqual(parse_json(bytearray(raw), 32, 50000), json.loads(raw))

    async def test_wide_json_tokens_and_aggregate_are_bounded_before_decoding(self):
        from zerus_mobile.ingress import parse_json
        raw_cases = [b'"' + b'x' * (1024 * 1024) + "\U0001f600".encode() + b'"',
                     b'"' + b'x' * (1024 * 1024) + b'\\ud83d\\ude00"',
                     b'[' + b','.join(b'"' + b'x' * 100000 + "\U0001f600".encode() + b'"' for _ in range(20)) + b']']
        for raw in raw_cases:
            with patch("zerus_mobile.ingress.json.loads", side_effect=AssertionError("decoder must not run")):
                with self.assertRaises(ValueError):
                    parse_json(bytearray(raw), 32, 50000, 4 * 1024 * 1024)
        self.assertEqual(parse_json(bytearray(b'"' + b'x' * (1024 * 1024) + b'\\\\ud83d"'), 32, 50000)[-6:], "\\ud83d")


    async def test_parser_releases_input_immediately_with_cyclic_gc_disabled(self):
        from zerus_mobile.ingress import parse_json
        class Tracked(bytearray): pass
        gc.disable()
        try:
            for suffix in (b'"}', b'"} trailing'):
                raw = Tracked(b'{"payload":"' + b'x' * 1000000 + suffix)
                reference = weakref.ref(raw)
                try:
                    result = parse_json(raw, 32, 50000)
                    del result
                except ValueError as error:
                    error.__traceback__ = None
                del raw
                self.assertIsNone(reference(), "parser closure retained raw input")
        finally:
            gc.enable()

    async def test_worker_error_releases_input_without_cyclic_collection(self):
        from zerus_mobile.ingress import blocking, parse_json
        class Tracked(bytearray): pass
        gc.disable()
        try:
            for suffix in (b'"} trailing', b'"'):
                raw = Tracked(b'{"payload":"' + b'x' * 1000000 + suffix)
                reference = weakref.ref(raw)
                try:
                    await blocking(parse_json, raw, 32, 50000)
                except ValueError as error:
                    error.__traceback__ = None
                del raw
                # The executor thread drops its work item just after resolving
                # the future, possibly after this coroutine resumes. Cyclic GC
                # stays disabled, so only a reference cycle outlasts the wait.
                deadline = time.monotonic() + 2
                while reference() is not None and time.monotonic() < deadline:
                    await asyncio.sleep(0.001)
                self.assertIsNone(reference(), "worker future retained malformed body")
        finally:
            gc.enable()


    async def test_unicode_event_pages_fit_phone_cap_without_skipping(self):
        self.store.db.executemany("INSERT INTO events(workspace_id,node_id,session,kind,created) VALUES(?,?,?,?,?)", [(self.workspace, self.node["node_id"], "\U0001f600" * 1024, "attention", time.time()) for _ in range(100)])
        self.store.db.commit()
        await self.start()
        after, received = 0, []
        for _ in range(3):
            async with self.client.get(f"/v1/events?after={after}", headers=self.headers()) as response:
                raw = await response.read()
                self.assertEqual(response.status, 200)
                self.assertLessEqual(len(raw), 1024 * 1024)
                page = json.loads(raw)
                received += [row["id"] for row in page["events"]]
                after = page["cursor"]
                if len(received) == 100: break
        self.assertEqual(received, list(range(1, 101)))

    async def test_result_receipt_wire_limit_is_checked_before_acknowledgement(self):
        await self.start()
        command = self.command()
        await self.request("POST", "/v1/requests", 202, json=command, headers=self.headers())
        await self.request("GET", "/v1/node/requests", 200, headers=self.headers(True))
        # Canonical error accounting uses UTF-8, but the response escapes Unicode.
        value = {"state": "completed", "result": {"synthetic": "x" * (1024 * 1024 - 200)}, "error": "\U0001f600" * 20}
        raw = json.dumps(value, ensure_ascii=False).encode()
        self.assertLess(len(raw), 1024 * 1024)
        await self.request("POST", f'/v1/node/requests/{command["request_id"]}/result', 413, data=io.BytesIO(raw), headers={**self.headers(True), "Content-Type": "application/json"})
        receipt = await self.request("GET", f'/v1/requests/{command["request_id"]}', 200, headers=self.headers())
        self.assertEqual(receipt["state"], "claimed")


if __name__ == "__main__":
    unittest.main()
