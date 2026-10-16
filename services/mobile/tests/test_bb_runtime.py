"""Black-box runtime contracts, with read operations as the vehicle: TLS
roots, proxy variables, redirects, the outbox, stop signals, the native
layer's descriptors, environment and timeouts, and the reviewed divergences
a, b, g and h (BLACKBOX.md).

Every test runs against both connectors unless it pins a divergence; a
Rust-only test names the Python behaviour it replaces.
"""
import asyncio
import json
import os
from pathlib import Path
import shutil
import signal
import ssl
import subprocess
import time
import unittest
import uuid

from aiohttp import web

from connector_harness import (BlackBoxCase, CLOCK_SCALE_HOOK, CONTEXT_KEYS, CONVERSATION_ID, FakeRelay, HELP_TEXT,
                               IMPLEMENTATION, JSON_DEPTH, JSON_VALUES, MIB, NATIVE_TIMEOUT_HOOK, RESTART_ERROR, RUN_ID,
                               SESSION, canonical, default_rules, inspection, json_limits, stopped_journal)

READ_HELP = HELP_TEXT + "hgs read <session> --json\n"
READ_RECEIPT = {"ok": True, "applied": {"reply_id": False, "attention_signature": False}, "read": None}
STORED_INVALID = "the stored native result could not be uploaded; this request will not execute again"
TLS_CONFIG = """[req]
distinguished_name = name
x509_extensions = ca
prompt = no
[name]
CN = bb-throwaway-ca
[ca]
basicConstraints = critical,CA:TRUE
keyUsage = critical,keyCertSign,cRLSign
subjectKeyIdentifier = hash
[leaf]
basicConstraints = critical,CA:FALSE
keyUsage = critical,digitalSignature
extendedKeyUsage = serverAuth
subjectAltName = IP:127.0.0.1
subjectKeyIdentifier = hash
authorityKeyIdentifier = keyid
"""


def exited(pid: int) -> bool:
    """Gone or a zombie: the process can run no further."""
    try:
        with open(f"/proc/{pid}/stat") as stat:
            return stat.read().rsplit(")", 1)[1].split()[0] == "Z"
    except FileNotFoundError:
        if os.path.isdir("/proc/self"):
            return True
    except OSError:
        pass
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return True
    except PermissionError:
        pass
    return False


def authority(directory: Path) -> tuple[Path, Path]:
    """A throwaway CA (certificate and key) below `directory`."""
    directory.mkdir(mode=0o700)
    config = directory / "openssl.cnf"
    config.write_text(TLS_CONFIG)
    subprocess.run(["openssl", "req", "-x509", "-config", str(config), "-newkey", "ec", "-pkeyopt",
                    "ec_paramgen_curve:prime256v1", "-nodes", "-keyout", str(directory / "ca.key"),
                    "-out", str(directory / "ca.pem"), "-days", "2"], check=True, capture_output=True)
    return directory / "ca.pem", directory / "ca.key"


def server_certificate(directory: Path, ca: Path, ca_key: Path) -> ssl.SSLContext:
    """A context serving a 127.0.0.1 certificate that `ca` signed."""
    config = directory / "openssl.cnf"
    subprocess.run(["openssl", "req", "-new", "-config", str(config), "-newkey", "ec", "-pkeyopt",
                    "ec_paramgen_curve:prime256v1", "-nodes", "-keyout", str(directory / "leaf.key"),
                    "-out", str(directory / "leaf.csr"), "-subj", "/CN=127.0.0.1"], check=True, capture_output=True)
    subprocess.run(["openssl", "x509", "-req", "-in", str(directory / "leaf.csr"), "-CA", str(ca), "-CAkey", str(ca_key),
                    "-CAcreateserial", "-out", str(directory / "leaf.pem"), "-days", "2", "-extfile", str(config),
                    "-extensions", "leaf"], check=True, capture_output=True)
    context = ssl.create_default_context(ssl.Purpose.CLIENT_AUTH)
    context.load_cert_chain(directory / "leaf.pem", directory / "leaf.key")
    return context


class TlsRelay(FakeRelay):
    """The fake relay behind HTTPS on loopback."""

    def __init__(self, context: ssl.SSLContext):
        super().__init__()
        self.context = context

    async def start(self) -> None:
        self.changed = asyncio.Condition()
        app = web.Application(client_max_size=2 * MIB)
        app.router.add_route("*", "/{tail:.*}", self.handle)
        self.runner = web.AppRunner(app, access_log=None, handler_cancellation=True, auto_decompress=False)
        await self.runner.setup()
        site = web.TCPSite(self.runner, "127.0.0.1", 0, ssl_context=self.context)
        await site.start()
        self.url = "https://127.0.0.1:" + str(site._server.sockets[0].getsockname()[1])


class RuntimeCase(BlackBoxCase):
    def mark_read(self) -> dict:
        request_id = str(uuid.uuid4())
        return {"request_id": request_id, "computer_id": self.relay.computer_id, "operation": "mark_read",
                "session": SESSION, "payload": {"run_id": RUN_ID, "conversation_id": CONVERSATION_ID,
                                                "reply_id": None, "attention_signature": None}}

    def process_output(self) -> dict:
        return self.request("process_output", process_id="job")

    def rules(self, *first: dict, help_text: str = HELP_TEXT) -> list[dict]:
        return [*first, *default_rules(help_text=help_text)]

    def polls(self) -> list:
        return [exchange for exchange in self.relay.exchanges
                if exchange.method == "GET" and exchange.path == "/v1/node/requests"]


class ReadTests(RuntimeCase):
    async def test_reads_run_with_fixed_arguments_and_no_calling_identity(self):
        answer = {"id": "job", "run_id": RUN_ID, "conversation_id": CONVERSATION_ID, "output": "line\n"}
        argv = ["processes", SESSION, "--output", "job", "--run", RUN_ID, "--conversation", CONVERSATION_ID]
        self.hgs.script(self.rules({"argv": argv, "stdout": answer}))
        calling = {key: "bb-calling-" + key.lower() for key in CONTEXT_KEYS}
        process = await self.launch(env={**calling, "HGS_PEERS": "bb-peer.example.invalid"})
        request = self.process_output()
        result = await self.submit(request, process)
        self.assertEqual(result, {"state": "completed", "error": None, "result": {
            **answer, "request_id": request["request_id"], "name": SESSION, "process_id": "job"}})
        [call] = self.hgs.calls("processes")
        self.assertEqual((call["argv"], call["stdin"]), (argv, ""))
        self.assertFalse(CONTEXT_KEYS & set(call["env"]), "calling agent identity reached native hgs")
        self.assertEqual(call["env"].get("HGS_PEERS"), "bb-peer.example.invalid")
        self.assertLessEqual(set(call["fds"]), {0, 1, 2}, "native hgs inherited connector descriptors")
        for listing in self.hgs.calls("ls"):
            self.assertLessEqual(set(listing["fds"]), {0, 1, 2})

    async def test_native_children_start_with_the_default_sigpipe_disposition(self):
        # Both connectors ignore SIGPIPE themselves; their hgs children must
        # not inherit that. A shell in front of the scripted hgs records its
        # own ignored-signal mask before Python, which ignores SIGPIPE, starts.
        if not os.path.isfile("/proc/self/status"):
            self.skipTest("needs /proc")
        record = self.scratch / "ignored-signals"
        wrapper = self.scratch / "hgs-wrapper"
        wrapper.write_text("#!/bin/sh\n"
                           "while read -r key value; do\n"
                           f"  if [ \"$key\" = SigIgn: ]; then echo \"$value\" >> '{record}'; fi\n"
                           "done < /proc/$$/status\n"
                           f"exec '{self.hgs.bin / 'hgs'}' \"$@\"\n")
        wrapper.chmod(0o700)
        process = await self.launch(self.config(hgs_path=str(wrapper)))
        self.assertEqual((await self.submit(self.request("inspect"), process))["state"], "completed")
        masks = record.read_text().split()
        self.assertTrue(masks)
        for mask in masks:
            self.assertFalse(int(mask, 16) & 1 << (signal.SIGPIPE - 1), f"a native child ignores SIGPIPE: {mask}")

    async def test_a_request_at_the_relay_parse_limits_is_judged_not_refused(self):
        # The relay admits depth 32 and 50,000 values, then adds its batch
        # envelope: the connector parses the batch and judges the request.
        innermost = []
        value = innermost
        for _ in range(JSON_DEPTH - 3):
            value = [value]
        request = self.request("inspect", deep=value)
        innermost.extend([0] * (JSON_VALUES - json_limits(canonical(request).encode())[1]))
        self.assertEqual(json_limits(canonical(request).encode()), (JSON_DEPTH, JSON_VALUES))
        process = await self.launch()
        self.assertEqual(await self.submit(request, process),
                         {"state": "failed", "result": None, "error": "invalid inspect fields"})
        self.assertEqual(self.hgs.calls("inspect"), [])

    async def test_a_retired_read_claim_never_blocks_later_requests(self):
        first = self.request("inspect")
        rule = self.relay.rule("POST", rf"/v1/node/requests/{first['request_id']}/result", 409)
        process = await self.launch()
        await self.relay.queue(first)
        await self.until(lambda: self.relay.uploads(first["request_id"]), what="the refused upload", process=process)
        await self.until(lambda: self.journal.row(first["request_id"])["delivered"], what="local retirement",
                         process=process)
        later = self.request("inspect")
        self.assertEqual((await self.submit(later, process))["state"], "completed")
        self.assertEqual(len(self.relay.uploads(first["request_id"])), 1)
        self.relay.rules.remove(rule)
        self.assertIn("Relay retired a result claim; preserving local outcome", process.output())

    async def test_redirects_are_never_followed(self):
        elsewhere = FakeRelay(self.relay.token)
        await elsewhere.start()
        try:
            self.relay.rule("GET", "/v1/node/requests", 302, times=1,
                            headers={"Location": elsewhere.url + "/v1/node/requests?wait=1"})
            self.relay.rule("POST", r"/v1/node/requests/[^/]+/result", 307, times=1,
                            headers={"Location": elsewhere.url + "/v1/node/heartbeat"})
            process = await self.launch()
            request = self.request("inspect")
            result = await self.submit(request, process)
            self.assertEqual([status for _, status in self.relay.uploads(request["request_id"])], [307, 200])
            self.assertEqual(result["state"], "completed")
            self.assertEqual(elsewhere.exchanges, [], "a redirect carried the node token elsewhere")
            self.assertEqual(len(self.hgs.calls("inspect")), 1)
        finally:
            await elsewhere.stop()

    async def test_a_poll_interval_beyond_any_timer_keeps_serving_requests(self):
        # Any finite poll_interval of at least one second is valid: after its
        # first heartbeat the connector waits as long as its timers allow.
        process = await self.launch(self.config(poll_interval=1e20))
        self.assertEqual((await self.submit(self.request("inspect"), process))["state"], "completed")
        self.assertEqual(len(self.relay.heartbeats()), 1)
        self.assertIsNone(process.returncode)

    async def test_logs_carry_neither_the_relay_url_nor_the_token(self):
        process = await self.launch()
        await self.relay.stop()
        self.relay.runner = None  # Stopped once; teardown has nothing left to stop.
        await self.until(lambda: "Heartbeat: relay is unavailable" in process.output(), 30, "an unavailable relay",
                         process)
        self.assertNotIn(self.relay.url.split("//", 1)[1], process.output())
        self.assertNotIn(self.relay.token, process.output())


class TransportTests(RuntimeCase):
    async def asyncSetUp(self):
        await super().asyncSetUp()
        if shutil.which("openssl") is None:
            if os.environ.get("CI") == "true":
                self.fail("openssl is required in CI")
            self.skipTest("needs the openssl command")

    async def tls_relay(self) -> tuple[Path, Path]:
        ca, ca_key = await asyncio.to_thread(authority, self.scratch / "tls")
        context = await asyncio.to_thread(server_certificate, self.scratch / "tls", ca, ca_key)
        await self.relay.stop()
        self.relay = TlsRelay(context)
        await self.relay.start()
        return ca, self.scratch / "empty-roots"

    async def test_tls_trusts_the_ca_ssl_cert_file_names(self):
        ca, empty = await self.tls_relay()
        empty.mkdir(mode=0o700)
        process = await self.launch(args=(), env={"SSL_CERT_FILE": str(ca), "SSL_CERT_DIR": str(empty)})
        self.assertEqual((await self.submit(self.request("inspect"), process))["state"], "completed")

    async def test_tls_refuses_a_relay_no_trusted_root_signed(self):
        _, empty = await self.tls_relay()
        empty.mkdir(mode=0o700)
        other, _ = await asyncio.to_thread(authority, self.scratch / "other-ca")
        process = await self.launch(args=(), env={"SSL_CERT_FILE": str(other), "SSL_CERT_DIR": str(empty)},
                                    ready=False)
        await self.until(lambda: "Heartbeat: relay is unavailable" in process.output(), 30,
                         "a refused certificate", process)
        self.assertEqual(self.relay.exchanges, [])
        self.assertIsNone(process.returncode)

    async def test_proxy_variables_are_ignored(self):
        trapped = []

        async def trap(reader, writer):
            trapped.append(writer.get_extra_info("peername"))
            writer.close()

        server = await asyncio.start_server(trap, "127.0.0.1", 0)
        try:
            proxy = "http://127.0.0.1:%d" % server.sockets[0].getsockname()[1]
            names = ("HTTP_PROXY", "HTTPS_PROXY", "ALL_PROXY", "http_proxy", "https_proxy", "all_proxy")
            process = await self.launch(env={name: proxy for name in names})
            self.assertEqual((await self.submit(self.request("inspect"), process))["state"], "completed")
            self.assertEqual(trapped, [], "the connector used a proxy from its environment")
        finally:
            server.close()
            await server.wait_closed()


class StopTests(RuntimeCase):
    """Divergence h: Python dies at SIGTERM; Rust lets the native call in
    progress finish within its own timeout, journals its outcome, exits 0."""

    async def in_flight(self, delay: float):
        self.hgs.script(self.rules({"argv": ["inspect", SESSION], "stdout": inspection(), "delay": delay}))
        process = await self.launch()
        running = self.request("inspect")
        await self.relay.queue(running)
        [call] = await self.until(lambda: self.hgs.calls("inspect"), what="the native read", process=process)
        return process, running, call

    async def test_sigterm_lets_the_read_in_progress_finish(self):
        process, running, call = await self.in_flight(1.5)
        process.signal(signal.SIGTERM)
        waiting = self.request("inspect")
        await self.relay.queue(waiting)
        code = await process.wait_exit(30)
        self.assertEqual(code, -signal.SIGTERM if IMPLEMENTATION == "python" else 0)
        ended = await self.until(lambda: self.hgs.finished(call["pid"]), what="the native read to finish on its own")
        self.assertEqual(ended["exit"], 0)
        self.assertEqual(self.relay.state(waiting["request_id"]), "queued")
        row = self.journal.row(running["request_id"])
        if IMPLEMENTATION == "python":
            self.assertEqual(row["state"], "running")
            expected = {"state": "failed", "result": None, "error": RESTART_ERROR}
        else:
            expected = {"state": "completed", "result": inspection(), "error": None}
            self.assertEqual(json.loads(row["response"]), expected)
        second = await self.launch()
        self.assertEqual((await self.until(lambda: self.relay.results(running["request_id"]),
                                           what="the outcome upload", process=second))[0], expected)
        self.assertEqual((await self.until(lambda: self.relay.results(waiting["request_id"]),
                                           what="the queued request", process=second))[0]["state"], "completed")

    async def test_sigint_stops_like_sigterm(self):
        # Python's KeyboardInterrupt cancels the read in progress and kills
        # its child; Rust treats SIGINT as SIGTERM (divergence h).
        process, running, call = await self.in_flight(1.5)
        process.signal(signal.SIGINT)
        self.assertEqual(await process.wait_exit(30), 0)
        row = self.journal.row(running["request_id"])
        if IMPLEMENTATION == "python":
            await self.until(lambda: exited(call["pid"]), 10, "the cancelled read's child to be killed")
            self.assertIsNone(self.hgs.finished(call["pid"]))
            self.assertEqual(json.loads(row["response"]), {"state": "failed", "result": None, "error": RESTART_ERROR})
        else:
            ended = await self.until(lambda: self.hgs.finished(call["pid"]), what="the read to finish on its own")
            self.assertEqual(ended["exit"], 0)
            self.assertEqual(json.loads(row["response"]), {"state": "completed", "result": inspection(), "error": None})

    async def test_a_second_signal_exits_at_once_and_leaves_the_row_for_recovery(self):
        process, running, call = await self.in_flight(3)
        started = time.monotonic()
        process.signal(signal.SIGTERM)
        await asyncio.sleep(0.2)
        if process.returncode is None:
            process.signal(signal.SIGTERM)
        code = await process.wait_exit(10)
        self.assertLess(time.monotonic() - started, 2.5)
        self.assertEqual(code, -signal.SIGTERM if IMPLEMENTATION == "python" else 128 + signal.SIGTERM)
        self.assertIsNone(self.hgs.finished(call["pid"]))
        self.assertEqual(self.journal.row(running["request_id"])["state"], "running")
        await self.until(lambda: self.hgs.finished(call["pid"]), what="the native read to finish on its own")
        second = await self.launch()
        recovered = {"state": "failed", "result": None, "error": RESTART_ERROR}
        self.assertEqual(await self.until(lambda: self.relay.results(running["request_id"]),
                                          what="the recovered upload", process=second), [recovered])


class NativeOutputTests(RuntimeCase):
    CAP = 8 * MIB
    ARGV = ["processes", SESSION, "--output", "job", "--run", RUN_ID, "--conversation", CONVERSATION_ID]
    ANSWER = {"id": "job", "run_id": RUN_ID, "conversation_id": CONVERSATION_ID, "output": "line\n"}
    SECRET = "bb-private-native-secret"

    def failed(self, error: str) -> dict:
        return {"state": "failed", "result": None, "error": error}

    async def test_stdout_and_stderr_are_each_capped_at_eight_mebibytes(self):
        process = await self.launch()
        head = '{"id": "job", "run_id": "%s", "conversation_id": "%s", "padding": "' % (RUN_ID, CONVERSATION_ID)
        exact = head + "a" * (self.CAP - len(head) - 2) + '"}'
        self.assertEqual(len(exact), self.CAP)
        for rule, error in (({"stdout_text": exact}, "native result exceeds relay size limit"),
                            ({"stdout_text": exact + " "}, "native output exceeds size limit"),
                            ({"stdout": self.ANSWER, "stderr": "e" * (self.CAP + 1)}, "native output exceeds size limit")):
            with self.subTest(error, stdout=len(rule.get("stdout_text", "")), stderr=len(rule.get("stderr", ""))):
                self.hgs.script(self.rules({"argv": self.ARGV, **rule}))
                self.assertEqual(await self.submit(self.process_output(), process, timeout=30), self.failed(error))
        # Stderr within the cap is read and discarded.
        self.hgs.script(self.rules({"argv": self.ARGV, "stdout": self.ANSWER, "stderr": (self.SECRET + "\n") * 4096}))
        result = await self.submit(self.process_output(), process)
        self.assertEqual(result["state"], "completed")
        self.assertNotIn(self.SECRET, json.dumps(result) + process.output())
        self.assertNotIn(self.SECRET.encode(), (self.state_dir / "journal.sqlite3").read_bytes())


class DivergenceTests(RuntimeCase):
    async def test_claims_wait_for_the_first_capability_probe(self):
        # Archive-aware Python probes before its listing; both connectors now
        # wait for the first probe before claiming an optional read.
        self.hgs.script([{"argv": ["--help"], "stdout_text": READ_HELP, "delay": 1.5},
                         {"argv": ["read", SESSION, "--json"], "stdout": READ_RECEIPT},
                         *default_rules(help_text=READ_HELP)[1:]])
        request = self.mark_read()
        await self.relay.queue(request)
        process = await self.launch()
        result = await self.until(lambda: self.relay.results(request["request_id"]), what="the read receipt",
                                  process=process)
        first = self.polls()[0]
        self.assertEqual(result, [{"state": "completed", "result": READ_RECEIPT, "error": None}])
        self.assertEqual(first.query, "wait=25")
        probes = self.hgs.calls("--help")
        self.assertEqual(len(probes), 1)
        finished = self.hgs.finished(probes[0]["pid"])
        self.assertIsNotNone(finished)
        reads = self.hgs.calls("read", SESSION, "--json")
        self.assertEqual(len(reads), 1)
        self.assertGreaterEqual(reads[0]["at"], finished["at"])

    async def test_a_refused_read_result_is_retired_after_three_spaced_attempts(self):
        # Divergence b: Python retries the refused upload before every poll
        # and claims nothing more (test_bb_requests pins it).
        if IMPLEMENTATION == "python":
            self.skipTest("Python blocks: test_bb_requests.test_rejected_result_blocks_later_claims_in_python")
        refused = self.request("inspect")
        self.relay.rule("POST", rf"/v1/node/requests/{refused['request_id']}/result", 422)
        process = await self.launch(env={CLOCK_SCALE_HOOK: "30"})
        await self.relay.queue(refused)
        later = self.request("inspect")
        self.assertEqual((await self.submit(later, process, timeout=20))["state"], "completed")
        await self.until(lambda: len(self.relay.uploads(refused["request_id"])) >= 3, 30, "three attempts", process)
        await self.until(lambda: self.journal.row(refused["request_id"])["delivered"], 10, "local retirement", process)
        attempts = [exchange.at for exchange in self.relay.exchanges
                    if exchange.path == f"/v1/node/requests/{refused['request_id']}/result"]
        self.assertEqual([status for _, status in self.relay.uploads(refused["request_id"])], [422, 422, 422])
        self.assertTrue(all(b - a >= 0.9 for a, b in zip(attempts, attempts[1:])), attempts)
        self.assertIn("Relay rejected a stored result; preserving local outcome", process.output())
        self.assertEqual(self.relay.state(refused["request_id"]), "claimed")

    async def test_an_upload_accepted_on_its_second_attempt_restores_the_long_poll(self):
        # Divergence b in both connectors: once the retried upload is
        # accepted, no attempt is due any more and polls wait 25 s again.
        refused = self.request("inspect")
        path = f"/v1/node/requests/{refused['request_id']}/result"
        self.relay.rule("POST", path, 422, times=1)
        process = await self.launch(env={CLOCK_SCALE_HOOK: "30"})
        await self.relay.queue(refused)
        accepted = (await self.until(lambda: [exchange.index for exchange in self.relay.exchanges
                                              if exchange.path == path and exchange.status == 200], 20,
                                     "the accepted second attempt", process))[0]
        polls = await self.until(lambda: [exchange for exchange in self.polls() if exchange.index > accepted], 10,
                                 "the next poll", process)
        self.assertEqual(polls[0].query, "wait=25")
        self.assertEqual([status for _, status in self.relay.uploads(refused["request_id"])], [422, 200])

    async def test_a_stored_row_the_relay_cannot_accept_uploads_a_substitute(self):
        # Divergence g: Python re-encodes a stored NaN, which the relay
        # refuses, so the row blocks every later claim.
        if IMPLEMENTATION == "python":
            self.skipTest("Python uploads the stored NaN, which the relay refuses")
        first = await self.launch()
        first.stop()
        read, write = self.request("inspect"), self.request("send", text="stored before the change")
        texts = {read["request_id"]: '{"state": "completed", "result": {"x": NaN}, "error": null}',
                 write["request_id"]: '{"state": "failed", "result": null, "error": "' + "e" * 4097 + '"}'}
        now = time.time()
        with stopped_journal(self.state_dir) as db:
            for request in (read, write):
                db.execute("INSERT INTO requests(request_id,operation,state,response,delivered,request_hash,created_at,"
                           "updated_at) VALUES(?,?,?,?,0,?,?,?)",
                           (request["request_id"], request["operation"], "completed", texts[request["request_id"]],
                            "0" * 64, now, now))
        for request in (read, write):
            await self.relay.claimed(request)
        second = await self.launch()
        for request, state in ((read, "failed"), (write, "uncertain")):
            [result] = await self.until(lambda: self.relay.results(request["request_id"]), what="the substitute",
                                        process=second)
            self.assertEqual(result, {"state": state, "result": None, "error": STORED_INVALID})
            await self.until(lambda: self.journal.row(request["request_id"])["delivered"], what="the delivery",
                             process=second)
            self.assertEqual(self.journal.row(request["request_id"])["response"], texts[request["request_id"]])

    async def test_the_native_timeout_hook_kills_a_read_past_its_budget(self):
        if IMPLEMENTATION == "python":
            self.skipTest("Python's default native timeout is 45 s; the hook exists only in Rust test builds")
        argv = ["processes", SESSION, "--output", "job", "--run", RUN_ID, "--conversation", CONVERSATION_ID]
        self.hgs.script(self.rules({"argv": argv, "stdout": {}, "delay": 60},
                                   {"argv": ["inspect", SESSION], "stdout": {}, "delay": 60}))
        process = await self.launch(env={NATIVE_TIMEOUT_HOOK: "1"})
        for request in (self.process_output(), self.request("inspect")):
            result = await self.submit(request, process, timeout=20)
            self.assertEqual(result, {"state": "failed", "result": None, "error": "native command timed out"})
        for call in self.hgs.calls("processes") + self.hgs.calls("inspect"):
            await self.until(lambda: exited(call["pid"]), 10, "the timed-out native child to be killed")
            self.assertIsNone(self.hgs.finished(call["pid"]))
        self.assertIsNone(process.returncode)


if __name__ == "__main__":
    unittest.main()
