"""Self-tests of the black-box harness; they run no connector."""
import asyncio
import fcntl
import hashlib
import io
import json
import os
from pathlib import Path
import random
import selectors
import shutil
import signal
import socket
import sqlite3
import stat
import subprocess
import sys
import tempfile
import time
import unittest
import uuid
from unittest import mock

from aiohttp import ClientSession

from connector_harness import (BlackBoxCase, JSON_DEPTH, JSON_VALUES, MIB, Journal, canonical,
                               foreign_connections, json_limits, recover_stopped_journal, remote_address)


def reference_limits(raw: bytes) -> tuple[int, int]:
    """A literal port of the counting loop of the relay's json::parse."""
    depth = deepest = values = 0
    quoted = escaped = scalar = False
    for c in raw:
        if quoted:
            if escaped:
                escaped = False
            elif c == 0x5C:
                escaped = True
            elif c == 0x22:
                quoted = False
            continue
        if scalar:
            if c in b" \t\n\x0c\r,:]}":
                scalar = False
            else:
                continue
        if c == 0x22:
            quoted = True
            values += 1
        elif c in b"{[":
            depth += 1
            deepest = max(deepest, depth)
            values += 1
        elif c in b"}]":
            depth -= 1
        elif c not in b", :\t\r\n":
            scalar = True
            values += 1
    return deepest, values


def random_json(rng: random.Random, depth: int = 0):
    kind = rng.randrange(7 if depth < 6 else 4)
    if kind == 0:
        return rng.choice([None, True, False])
    if kind == 1:
        return rng.choice([0, -1, 2**70, 1.5, -0.0, 1e-7, 1e300])
    if kind in (2, 3):
        return "".join(rng.choice('ab"\\{}[],: \né\U0001f600') for _ in range(rng.randrange(6)))
    if kind in (4, 5):
        return [random_json(rng, depth + 1) for _ in range(rng.randrange(4))]
    return {"".join(rng.choice('k"\\{[:, \u00e9') for _ in range(rng.randrange(4))): random_json(rng, depth + 1)
            for _ in range(rng.randrange(4))}


async def chunked(data: bytes):
    yield data


class HarnessTests(BlackBoxCase):
    tests_connector = False

    async def asyncSetUp(self):
        await super().asyncSetUp()
        self.client = ClientSession()

    async def asyncTearDown(self):
        await self.client.close()
        self.relay.violations.clear()  # Provoked on purpose here.
        await super().asyncTearDown()

    async def call(self, method, path, data=None, headers=None, token=None):
        headers = {"Authorization": "Bearer " + (token or self.relay.token), **(headers or {})}
        async with self.client.request(method, self.relay.url + path, data=data, headers=headers) as response:
            return response.status, await response.json()

    async def test_relay_http_rules_are_enforced_and_recorded(self):
        heartbeat = canonical({"snapshot": {"sessions": []}}).encode()
        deep = canonical({"snapshot": {"sessions": [], "deep": json.loads("[" * (JSON_DEPTH - 1) + "]" * (JSON_DEPTH - 1))}})
        typed = {"Content-Type": "application/json"}
        cases = [
            ("accepted", "POST", "/v1/node/heartbeat", lambda: heartbeat, typed, 200, None),
            ("charset parameter", "POST", "/v1/node/heartbeat", lambda: heartbeat, {"Content-Type": "application/json; charset=utf-8"}, 200, None),
            ("not JSON", "POST", "/v1/node/heartbeat", lambda: heartbeat, {"Content-Type": "text/plain"}, 415, "application/json"),
            ("compressed", "POST", "/v1/node/heartbeat", lambda: heartbeat, {**typed, "Content-Encoding": "gzip"}, 415, "Content-Encoding"),
            ("unknown length", "POST", "/v1/node/heartbeat", lambda: chunked(heartbeat), typed, 411, "Content-Length"),
            ("above 1 MiB", "POST", "/v1/node/heartbeat",
             lambda: io.BytesIO(canonical({"snapshot": {"sessions": []}, "pad": "x" * MIB}).encode()), typed, 413, "1 MiB"),
            ("NaN", "POST", "/v1/node/heartbeat", lambda: b'{"snapshot":{"sessions":[NaN]}}', typed, 400, "invalid JSON"),
            ("lone surrogate", "POST", "/v1/node/heartbeat",
             lambda: b'{"snapshot":{"sessions":[],"x":"\\ud800"}}', typed, 400, "invalid JSON"),
            ("too deep", "POST", "/v1/node/heartbeat", lambda: deep.encode(), typed, 413, "JSON limits"),
            ("unknown heartbeat field", "POST", "/v1/node/heartbeat",
             lambda: canonical({"snapshot": {"sessions": []}, "extra": 1}).encode(), typed, 400, "heartbeat fields"),
            ("duplicate query parameter", "GET", "/v1/node/requests?wait=0&wait=0", None, {}, 400, "duplicate query"),
            ("wait above 25 s", "GET", "/v1/node/requests?wait=26", None, {}, 400, "poll parameters"),
            ("unknown endpoint", "GET", "/v1/node/other", None, {}, 404, "unknown endpoint"),
            ("outside /v1/", "GET", "/healthz", None, {}, 404, "outside /v1/"),
        ]
        for name, method, path, data, headers, status, violation in cases:
            with self.subTest(name):
                self.relay.violations.clear()
                self.assertEqual((await self.call(method, path, data and data(), headers))[0], status)
                if violation is None:
                    self.assertEqual(self.relay.violations, [])
                else:
                    self.assertEqual(len(self.relay.violations), 1, self.relay.violations)
                    self.assertIn(violation, self.relay.violations[0])
        self.relay.violations.clear()
        # Another credential is the relay's answer, not a client defect.
        self.assertEqual((await self.call("POST", "/v1/node/heartbeat", heartbeat, typed, token="bb-other"))[0], 401)
        self.assertEqual(self.relay.violations, [])

    async def test_programmed_rules_fire_only_as_asked(self):
        heartbeat = canonical({"snapshot": {"sessions": []}, "presence": None}).encode()
        typed = {"Content-Type": "application/json"}
        rule = self.relay.rule("POST", "/v1/node/heartbeat", 503, times=2)
        presence = self.relay.rule("POST", "/v1/node/heartbeat", 422, when=lambda exchange: "presence" in exchange.body)
        statuses = [(await self.call("POST", "/v1/node/heartbeat", heartbeat, typed))[0] for _ in range(3)]
        self.assertEqual((statuses, rule.used, presence.used), ([503, 503, 422], 2, 1))
        plain = canonical({"snapshot": {"sessions": []}}).encode()
        self.assertEqual((await self.call("POST", "/v1/node/heartbeat", plain, typed))[0], 200)
        self.assertEqual(self.relay.violations, [])

    async def test_long_poll_wakes_within_50_ms_and_carries_the_relay_envelope(self):
        poll = asyncio.create_task(self.call("GET", "/v1/node/requests?wait=5"))
        await self.until(lambda: self.relay.active_polls == 1, what="the poll")
        await asyncio.sleep(0.05)
        request = self.request("send", text="wake")
        queued = time.monotonic()
        await self.relay.queue(request, expires_at=1700000000.5)
        status, batch = await poll
        self.assertLess(time.monotonic() - queued, 0.05)
        delivered = {key: value for key, value in request.items() if key != "computer_id"}
        self.assertEqual((status, batch), (200, {"requests": [{**delivered, "expires_at": 1700000000.5}]}))
        self.assertEqual(self.relay.state(request["request_id"]), "claimed")
        # A routed request goes only to a poll that accepts one-hop requests.
        route = {"schema": 1, "route_id": None, "computer_id": self.relay.computer_id, "machine_id": self.relay.computer_id}
        routed = self.request("inspect")
        await self.relay.queue(routed, gateway_route=route)
        self.assertEqual(await self.call("GET", "/v1/node/requests?wait=0.1"), (200, {"requests": []}))
        status, batch = await self.call("GET", "/v1/node/requests?wait=0.1&gateway_one_hop=1")
        self.assertEqual(batch["requests"][0]["gateway_route"], route)
        self.assertEqual(self.relay.violations, [])

    async def test_results_follow_the_relay_claim_rules(self):
        typed = {"Content-Type": "application/json"}
        receipt = {"state": "completed", "result": {"ok": True}, "error": None}
        request = self.request("inspect")
        path = f"/v1/node/requests/{request['request_id']}/result"
        self.assertEqual((await self.call("POST", path, canonical(receipt), typed))[0], 404)
        await self.relay.queue(request)
        self.assertEqual((await self.call("POST", path, canonical(receipt), typed))[0], 409)
        await self.call("GET", "/v1/node/requests?wait=0")
        for body, status in ((receipt, 200), (receipt, 200), ({**receipt, "state": "failed"}, 409)):
            self.assertEqual((await self.call("POST", path, canonical(body), typed))[0], status)
        self.assertEqual(self.relay.results(request["request_id"]), [receipt, receipt])
        self.assertEqual(self.relay.violations, [])
        invalid = {**receipt, "extra": 1}
        self.assertEqual((await self.call("POST", path, canonical(invalid), typed))[0], 400)
        self.assertIn("invalid result receipt", self.relay.violations[0])

    async def test_peer_heartbeats_follow_the_last_accepted_manifest(self):
        typed = {"Content-Type": "application/json"}
        gateway, peer, other, route, offline = (str(uuid.uuid4()) for _ in range(5))
        snapshot = {"sessions": []}
        guarded = {"sessions": [], "mobile_capabilities": {"protocol_version": 1, "features": ["gateway_one_hop"]}}
        rows = [{"route_id": route, "machine_id": peer, "name": "Peer", "online": True},
                {"route_id": offline, "machine_id": other, "name": "Offline peer", "online": False}]

        async def beat(route_id, machine_id):
            body = canonical({"machine_id": machine_id, "snapshot": snapshot})
            return (await self.call("POST", f"/v1/node/peers/{route_id}/heartbeat", body, typed))[0]

        async def gateway_beat(gateway_snapshot):
            extended = canonical({"snapshot": gateway_snapshot, "machine_id": gateway, "peers": rows})
            self.assertEqual((await self.call("POST", "/v1/node/heartbeat", extended, typed))[0], 200)

        self.assertEqual(await beat(route, peer), 404)
        await gateway_beat(guarded)
        statuses = [await beat(route, peer), await beat(route, other), await beat(offline, other)]
        self.assertEqual(statuses, [200, 409, 409])
        # A gateway snapshot without gateway_one_hop (or with another protocol
        # version) leaves even an online route unguarded.
        for unguarded in (snapshot, {**guarded, "mobile_capabilities": {"protocol_version": True,
                                                                         "features": ["gateway_one_hop"]}}):
            with self.subTest(unguarded=unguarded):
                await gateway_beat(unguarded)
                self.assertEqual(await beat(route, peer), 409)
        await gateway_beat(guarded)
        self.assertEqual(await beat(route, peer), 200)
        # A heartbeat without the gateway fields withdraws every route.
        plain = canonical({"snapshot": snapshot})
        self.assertEqual((await self.call("POST", "/v1/node/heartbeat", plain, typed))[0], 200)
        self.assertEqual(await beat(route, peer), 404)
        self.assertEqual([exchange.path for exchange in self.relay.peer_heartbeats()],
                         [f"/v1/node/peers/{route}/heartbeat"] * 2)
        self.assertEqual(self.relay.violations, [])

    async def test_queue_counts_and_refuses_like_the_relay(self):
        rng = random.Random(20261015)
        samples = [canonical(random_json(rng)).encode() for _ in range(300)]
        samples += [json.dumps(random_json(rng), ensure_ascii=False, indent=1).encode() for _ in range(300)]
        samples += [b'{"a":"[{\\"}","b":[1,-2.5e3,true,null,{}]}', b'"\\\\"', b"[[[]]]"]
        for raw in samples:
            self.assertEqual(json_limits(raw), reference_limits(raw), raw)
        value = []
        for _ in range(JSON_DEPTH - 2):
            value = [value]
        with self.assertRaises(ValueError):
            await self.relay.queue(self.request("interrupt", expected_turn_started=[value]))
        request = self.request("interrupt", expected_turn_started=[])
        request["payload"]["expected_turn_started"] = [0] * (JSON_VALUES + 1 - json_limits(canonical(request).encode())[1])
        with self.assertRaises(ValueError):
            await self.relay.queue(request)
        request["payload"]["expected_turn_started"].pop()
        await self.relay.queue(request)

    async def test_scripted_hgs_answers_records_and_limits_rules(self):
        self.hgs.script([
            {"argv": ["echo", "x"], "times": 1,
             "stdout": {"id": "{{stdin.request_id}}", "argument": "{{argv.1}}", "stdin": "{{stdin}}"}},
            {"prefix": ["fail"], "exit": 3, "stderr": "synthetic native detail\n", "stdout": {"ok": False}},
        ])

        def run(*argv, stdin=b""):
            return subprocess.run([str(self.hgs.bin / "hgs"), *argv], input=stdin, capture_output=True,
                                  env={"PATH": "/usr/bin:/bin"}, timeout=10)

        first = await asyncio.to_thread(run, "echo", "x", stdin=b'{"request_id": "r-1"}')
        self.assertEqual((first.returncode, json.loads(first.stdout)),
                         (0, {"id": "r-1", "argument": "x", "stdin": {"request_id": "r-1"}}))
        again = await asyncio.to_thread(run, "echo", "x")
        self.assertEqual(again.returncode, 2)  # `times` is spent; nothing else matches.
        failed = await asyncio.to_thread(run, "fail", "now")
        self.assertEqual((failed.returncode, failed.stderr, json.loads(failed.stdout)),
                         (3, b"synthetic native detail\n", {"ok": False}))
        starts = self.hgs.calls()
        self.assertEqual([call["argv"] for call in starts], [["echo", "x"], ["echo", "x"], ["fail", "now"]])
        self.assertEqual(starts[0]["stdin"], '{"request_id": "r-1"}')
        self.assertLessEqual(set(starts[0]["fds"]), {0, 1, 2})
        self.assertEqual([self.hgs.finished(call["pid"])["exit"] for call in starts], [0, 2, 3])
        # A rule's uses follow its id, not its position, so a prepended rule
        # neither spends nor refunds another rule's `times`.
        self.hgs.prepend({"argv": ["other"]})
        renewed = [(await asyncio.to_thread(run, "echo", "x")).returncode for _ in range(2)]
        self.assertEqual(renewed, [0, 2])
        # Concurrent calls never both take the last use of a rule.
        self.hgs.script([{"argv": ["once"], "times": 1, "stdout": {"ok": True}}])
        racing = await asyncio.gather(*(asyncio.to_thread(run, "once") for _ in range(8)))
        self.assertEqual(sorted(result.returncode for result in racing), [0] + [2] * 7)

    async def test_scripted_hgs_matches_stdin_fields_by_json_value(self):
        self.hgs.script([{"argv": ["swarm", "mobile-peer", "--json"], "stdin_fields": {"schema": 1, "argv": ["ls"]},
                          "stdout": {"id": "{{stdin.payload.request_id}}", "first": "{{stdin.argv.0}}",
                                     "missing": "{{stdin.payload.absent.deeper}}"}}])

        def run(stdin: bytes):
            return subprocess.run([str(self.hgs.bin / "hgs"), "swarm", "mobile-peer", "--json"], input=stdin,
                                  capture_output=True, env={"PATH": "/usr/bin:/bin"}, timeout=10)

        matched = await asyncio.to_thread(run, b'{"schema": 1, "argv": ["ls"], "payload": {"request_id": "r-1"}}')
        self.assertEqual((matched.returncode, json.loads(matched.stdout)),
                         (0, {"id": "r-1", "first": "ls", "missing": None}))
        # `true` is not 1, a longer argv is not the listed one, and stdin must be an object holding every field.
        for stdin in (b'{"schema": true, "argv": ["ls"]}', b'{"schema": 1, "argv": ["ls", "-a"]}',
                      b'{"argv": ["ls"]}', b'[1, ["ls"]]', b""):
            with self.subTest(stdin=stdin):
                self.assertEqual((await asyncio.to_thread(run, stdin)).returncode, 2)

    async def test_stdin_fields_is_the_one_stdin_key_and_unknown_keys_are_refused(self):
        # The terminal and speech suites pick an answer by its `action`: a call
        # whose stdin lacks or changes a listed field falls through to a later
        # rule, and a listed value never matches a JSON value of another type.
        self.hgs.script([{"argv": ["pick"], "stdin_fields": {"action": "input", "enter": True},
                          "stdout": {"picked": "{{stdin.action}}", "text": "{{stdin.text}}"}},
                         {"argv": ["pick"], "stdin_fields": {"action": "snapshot"}, "stdout": {"picked": "snapshot"}},
                         {"argv": ["pick"], "stdout": {"picked": "other"}}])

        def run(stdin: bytes):
            return subprocess.run([str(self.hgs.bin / "hgs"), "pick"], input=stdin, capture_output=True,
                                  env={"PATH": "/usr/bin:/bin"}, timeout=10)

        for stdin, picked in ((b'{"action": "input", "enter": true, "text": "x"}', {"picked": "input", "text": "x"}),
                              (b'{"action": "input", "enter": 1}', {"picked": "other"}),
                              (b'{"action": "input"}', {"picked": "other"}),
                              (b'{"action": "snapshot", "enter": true}', {"picked": "snapshot"}),
                              (b'{"action": ["snapshot"]}', {"picked": "other"}),
                              (b"not json", {"picked": "other"})):
            with self.subTest(stdin=stdin):
                self.assertEqual(json.loads((await asyncio.to_thread(run, stdin)).stdout), picked)
        # The retired `stdin_has`, or any key the script ignores, would widen a rule.
        for rule in ({"argv": ["pick"], "stdin_has": {"action": "input"}},
                     {"argv": ["pick"], "stdin_fields": ["action"]}):
            with self.subTest(rule=rule), self.assertRaises(ValueError):
                self.hgs.script([rule])

    def test_configurations_may_name_only_loopback_or_reserved_hosts(self):
        for changes in ({"server_url": "https://relay.zerus.dev"}, {"identity_url": "https://zerus.dev.guthub.dev"},
                        {"server_url": "http://192.0.2.1"}):
            with self.subTest(changes=changes), self.assertRaises(AssertionError):
                self.write_config(self.config(**changes))
        self.write_config(self.config(server_url="https://relay.example.invalid"))

    def test_socket_guard_reports_only_non_loopback_peers(self):
        self.assertIsNone(remote_address("0100007F:1F90"))
        self.assertIsNone(remote_address("00000000:0000"))
        self.assertIsNone(remote_address("00000000000000000000000001000000:0050"))
        self.assertIsNone(remote_address("0000000000000000FFFF00000100007F:0050"))
        self.assertEqual(remote_address("010200C0:0009"), "192.0.2.1:9")
        if not sys.platform.startswith("linux"):
            self.skipTest("the socket guard reads /proc")
        # Other modules may share this test process: compare with its sockets now.
        baseline = set(foreign_connections(os.getpid()))
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as udp:
            try:
                udp.connect(("192.0.2.1", 9))  # TEST-NET-1; connecting UDP sends nothing.
            except OSError:
                self.skipTest("no route for the reserved documentation network")
            self.assertEqual(set(foreign_connections(os.getpid())) - baseline, {"udp 192.0.2.1:9"})
        with socket.create_server(("127.0.0.1", 0)) as server, socket.create_connection(server.getsockname()):
            self.assertEqual(set(foreign_connections(os.getpid())) - baseline, set())


class StoppedJournalTests(unittest.TestCase):
    """Real disposable SQLite pager recovery; no connector, relay or hgs."""

    def test_hot_rollback_requires_guarded_recovery_before_readonly_tombstone_checks(self):
        root = Path(tempfile.mkdtemp(prefix="zerus-bb-hot-"))
        database, lock = root / "journal.sqlite3", root / "connector.lock"
        database.touch(mode=0o600, exist_ok=False)
        lock.touch(mode=0o600, exist_ok=False)
        end = time.monotonic() + 20
        child, reaped, complete = None, False, False

        def remaining(maximum, reserve=3):
            available = end - reserve - time.monotonic()
            self.assertGreater(available, 0, f"original fixture end; retained {root}")
            return min(maximum, available)

        try:
            db = sqlite3.connect(database, timeout=2)
            try:
                db.executescript("PRAGMA page_size=1024; PRAGMA journal_mode=DELETE;"
                                 "CREATE TABLE requests(request_id TEXT PRIMARY KEY, state TEXT,"
                                 " response_expired INTEGER, response TEXT, request_hash TEXT);"
                                 "CREATE TABLE padding(body BLOB);")
                with db:
                    db.executemany("INSERT INTO requests VALUES(?,?,?,?,?)", [
                        ("early", "completed", 1, "history_expired", "early-hash"),
                        ("later", "completed", 1, "history_expired", "later-hash")])
                    db.executemany("INSERT INTO padding VALUES(?)", [(b"A" * 8192,)] * 32)
            finally:
                db.close()
            journal = Journal(root)
            before_rows = [dict(row) for row in journal.query(
                "SELECT * FROM requests ORDER BY request_id")]
            before_schema = [tuple(row) for row in journal.query(
                "SELECT type,name,tbl_name,sql FROM sqlite_master ORDER BY type,name")]
            before_bytes = database.read_bytes()
            before_modes = [stat.S_IMODE(path.stat().st_mode) for path in (root, database, lock)]
            writer = """import sqlite3,sys
db=sqlite3.connect(sys.argv[1],isolation_level=None,timeout=2)
db.execute('PRAGMA journal_mode=DELETE')
db.execute('PRAGMA synchronous=FULL')
db.execute('PRAGMA cache_size=1')
db.execute('PRAGMA cache_spill=ON')
db.execute('BEGIN IMMEDIATE')
db.execute("UPDATE requests SET state='running',response_expired=0,response='BB-PRIVATE uncommitted'")
db.execute('UPDATE padding SET body=randomblob(16384)')
sys.stdout.buffer.write(b'uncommitted-spill-ready\\n');sys.stdout.buffer.flush()
sys.stdin.buffer.read(1)
"""
            remaining(1)
            child = subprocess.Popen([sys.executable, "-I", "-S", "-c", writer, str(database)],
                stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                env={"PATH": "/usr/bin:/bin", "HOME": str(root), "TMPDIR": str(root)})
            with selectors.DefaultSelector() as ready:
                ready.register(child.stdout, selectors.EVENT_READ)
                self.assertTrue(ready.select(remaining(10)), f"writer barrier; retained {root}")
                self.assertEqual(os.read(child.stdout.fileno(), 128), b"uncommitted-spill-ready\n")
            with (root / "journal.sqlite3-journal").open("rb") as rollback:
                header = rollback.read(8)
            self.assertEqual(header, b"\xd9\xd5\x05\xf9\x20\xa1\x63\xd7", "real hot rollback header")
            # Only this unreaped direct child is killed; there is no PID lookup,
            # other namespace, connector or native hgs in this regression.
            child.kill()
            stdout, stderr = child.communicate(timeout=remaining(3))
            reaped = True
            self.assertEqual((child.returncode, stdout, stderr), (-signal.SIGKILL, b"", b""))
            with self.assertRaises(sqlite3.OperationalError) as old:
                journal.row("early")
            self.assertEqual(old.exception.sqlite_errorcode, sqlite3.SQLITE_READONLY_ROLLBACK)
            self.assertEqual(old.exception.sqlite_errorname, "SQLITE_READONLY_ROLLBACK")
            remaining(1)
            recover_stopped_journal(root)
            self.assertEqual([dict(row) for row in journal.query(
                "SELECT * FROM requests ORDER BY request_id")], before_rows)
            self.assertEqual([tuple(row) for row in journal.query(
                "SELECT type,name,tbl_name,sql FROM sqlite_master ORDER BY type,name")], before_schema)
            self.assertEqual(database.read_bytes(), before_bytes)
            self.assertNotIn(b"BB-PRIVATE", database.read_bytes())
            self.assertEqual([stat.S_IMODE(path.stat().st_mode) for path in (root, database, lock)], before_modes)
            self.assertFalse((root / "journal.sqlite3-journal").exists())
            remaining(1)
            print("ZERUS_STOPPED_JOURNAL_CONTROL=" + json.dumps({
                "scope": "controlled hot-journal stand-in, not historical CI database evidence",
                "readonly_error_code": old.exception.sqlite_errorcode,
                "readonly_error_name": old.exception.sqlite_errorname,
                "hot_header_hex": header.hex(), "committed_tombstones": 2,
                "database_sha256_before": hashlib.sha256(before_bytes).hexdigest(),
                "database_sha256_after": hashlib.sha256(database.read_bytes()).hexdigest(),
                "schema_and_modes_equal": True, "private_uncommitted_bytes_absent": True,
                "owned_child_reaped": reaped}), flush=True)
            complete = True
        finally:
            unwinding, cleanup_error = sys.exc_info()[0] is not None, None
            if child is not None:
                try:
                    if child.poll() is None:
                        child.kill()
                    if not reaped:
                        child.communicate(timeout=max(.001, min(3, end - time.monotonic())))
                        reaped = True
                except BaseException as error:
                    cleanup_error, complete = error, False
                finally:
                    for stream in (child.stdin, child.stdout, child.stderr):
                        if stream is not None:
                            stream.close()
            if complete and (child is None or reaped):
                shutil.rmtree(root)
            else:
                print(f"Retained stopped-journal fixture: {root}", flush=True)
            if cleanup_error is not None:
                if not unwinding:
                    raise cleanup_error
                print(f"Owned writer cleanup failed: {type(cleanup_error).__name__}", flush=True)

    def test_recovery_refuses_a_held_connector_lock_before_opening_sqlite(self):
        with tempfile.TemporaryDirectory(prefix="zerus-bb-locked-") as folder:
            root = Path(folder)
            (root / "journal.sqlite3").touch(mode=0o600, exist_ok=False)
            descriptor = os.open(root / "connector.lock", os.O_CREAT | os.O_EXCL | os.O_RDWR, 0o600)
            try:
                fcntl.flock(descriptor, fcntl.LOCK_EX | fcntl.LOCK_NB)
                with mock.patch("connector_harness.sqlite3.connect", side_effect=AssertionError("must not open")) as connect:
                    with self.assertRaisesRegex(AssertionError, "only while its connector is stopped"):
                        recover_stopped_journal(root)
                    connect.assert_not_called()
                self.assertEqual((root / "journal.sqlite3").read_bytes(), b"")
            finally:
                os.close(descriptor)

    def test_recovery_never_creates_a_missing_database_or_follows_a_database_link(self):
        with tempfile.TemporaryDirectory(prefix="zerus-bb-missing-") as folder:
            root = Path(folder)
            database = root / "journal.sqlite3"
            with mock.patch("connector_harness.sqlite3.connect", side_effect=AssertionError("must not open")) as connect:
                with self.assertRaises(FileNotFoundError):
                    recover_stopped_journal(root)
                self.assertFalse(database.exists())
                target = root / "other.sqlite3"
                target.write_bytes(b"untouched synthetic target")
                database.symlink_to(target)
                with self.assertRaisesRegex(AssertionError, "existing regular journal"):
                    recover_stopped_journal(root)
                connect.assert_not_called()
                self.assertEqual(target.read_bytes(), b"untouched synthetic target")

    def test_recovery_propagates_an_unrelated_database_error_without_changing_bytes_or_modes(self):
        with tempfile.TemporaryDirectory(prefix="zerus-bb-corrupt-") as folder:
            root = Path(folder)
            database, lock = root / "journal.sqlite3", root / "connector.lock"
            database.touch(mode=0o600, exist_ok=False)
            lock.touch(mode=0o600, exist_ok=False)
            database.write_bytes(b"not a sqlite database\n")
            before, mode = database.read_bytes(), stat.S_IMODE(database.stat().st_mode)
            with self.assertRaises(sqlite3.DatabaseError) as failure:
                recover_stopped_journal(root)
            self.assertEqual(failure.exception.sqlite_errorcode, sqlite3.SQLITE_NOTADB)
            self.assertEqual(database.read_bytes(), before)
            self.assertEqual(stat.S_IMODE(database.stat().st_mode), mode)


if __name__ == "__main__":
    unittest.main()
