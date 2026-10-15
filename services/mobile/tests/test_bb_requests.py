"""Black-box claim, native execution and result upload of relay requests."""
import hashlib
import json
import unittest

from connector_harness import (BlackBoxCase, CONTEXT_KEYS, CONVERSATION_ID, FakeRelay, IMPLEMENTATION, JSON_DEPTH,
                               JSON_VALUES, RUN_ID, SESSION, canonical, inspection, json_limits)


class RequestTests(BlackBoxCase):
    async def test_read_executes_once_and_uploads_its_result(self):
        process = await self.launch()
        request = self.request("inspect")
        result = await self.submit(request, process)
        self.assertEqual(result, {"state": "completed", "result": inspection(), "error": None})
        calls = self.hgs.calls("inspect")
        self.assertEqual([call["argv"] for call in calls], [["inspect", SESSION]])
        self.assertEqual(calls[0]["stdin"], "")
        self.assertEqual(self.relay.state(request["request_id"]), "completed")
        upload = next(exchange for exchange in self.relay.exchanges
                      if exchange.path == f"/v1/node/requests/{request['request_id']}/result")
        self.assertEqual(upload.headers["authorization"], "Bearer " + self.relay.token)
        row = await self.until(lambda: (row := self.journal.row(request["request_id"])) and row["delivered"] and row,
                               what="the delivered journal row")
        # The no-replay fingerprint covers the request exactly as delivered.
        delivered = {key: value for key, value in request.items() if key != "computer_id"}
        self.assertEqual((row["operation"], row["state"], row["request_hash"]),
                         ("inspect", "completed", hashlib.sha256(canonical(delivered).encode()).hexdigest()))
        self.assertEqual(json.loads(row["response"]), result)

    async def test_mutation_gets_exact_stdin_and_no_calling_agent_identity(self):
        calling = {key: "bb-calling-" + key.lower() for key in CONTEXT_KEYS}
        machine = {"HGS_PEERS": "bb-peer.example.invalid"}
        process = await self.launch(env={**calling, **machine})
        text = "literal $(touch ignored); 'quotes'\nЮникод"
        request = self.request("send", text=text)
        result = await self.submit(request, process)
        self.assertEqual(result, {"state": "completed", "error": None, "result": {
            "request_id": request["request_id"], "status": "submitted", "name": SESSION,
            "run_id": RUN_ID, "conversation_id": CONVERSATION_ID, "text": text}})
        [call] = self.hgs.calls("send")
        self.assertEqual(call["argv"], ["send", SESSION, "--json"])
        self.assertEqual(json.loads(call["stdin"]), request["payload"])
        # Python writes UTF-8 without ASCII escapes; only the length is the contract.
        self.assertEqual(len(call["stdin"].encode()),
                         len(json.dumps(request["payload"], ensure_ascii=False).encode()))
        self.assertFalse(CONTEXT_KEYS & set(call["env"]), "calling agent identity reached native hgs")
        expected = self.environment(machine)
        for key in ("HGS_CONFIG_DIR", "HGS_STATE_DIR", "HGS_PEERS", "HOME", "PATH"):
            self.assertEqual(call["env"].get(key), expected[key], key)
        self.assertLessEqual(set(call["fds"]), {0, 1, 2}, "native hgs inherited connector descriptors")
        self.assertEqual(self.relay.state(request["request_id"]), "completed")

    async def test_unknown_session_fails_without_native_execution(self):
        process = await self.launch()
        before = len(self.hgs.calls())
        request = self.request("send", session="claude/missing", text="never delivered")
        result = await self.submit(request, process)
        self.assertEqual(result, {"state": "failed", "result": None,
                                  "error": "session must match an exact nonarchived snapshot entry"})
        self.assertEqual(self.hgs.calls("send"), [])
        self.assertFalse(any(call["argv"][:1] not in (["ls"], ["swarm"], ["account"], ["--help"])
                             for call in self.hgs.calls()[before:]))

    async def test_request_at_the_relay_parse_limits_executes_from_its_batch(self):
        # The relay parses a submission with depth 32 and 50,000 values, then
        # adds its batch envelope; the connector must not refuse the batch.
        process = await self.launch()
        innermost = []
        value = innermost
        for _ in range(JSON_DEPTH - 3):  # Below the request and payload objects.
            value = [value]
        request = self.request("interrupt", expected_turn_started=value)
        innermost.extend([0] * (JSON_VALUES - json_limits(canonical(request).encode())[1]))
        self.assertEqual(json_limits(canonical(request).encode()), (JSON_DEPTH, JSON_VALUES))
        result = await self.submit(request, process)
        self.assertEqual(result["state"], "completed", result)
        batch = next(exchange for exchange in self.relay.exchanges if request["request_id"] in exchange.delivered)
        depth, values = json_limits(canonical(batch.response).encode())
        self.assertGreater(depth, JSON_DEPTH)
        self.assertGreater(values, JSON_VALUES)
        [call] = self.hgs.calls("interrupt")
        self.assertEqual(json.loads(call["stdin"]), request["payload"])

    async def test_duplicate_identity_replays_without_native_execution(self):
        process = await self.launch()
        sent = self.request("send", text="exactly once")
        first = await self.submit(sent, process)
        read = self.request("inspect")
        inspected = await self.submit(read, process)
        await self.until(lambda: all((row := self.journal.row(request["request_id"])) and row["delivered"]
                                     for request in (sent, read)), what="delivered journal rows")
        # A redelivered identity replays its preserved outcome.
        self.assertEqual(await self.submit(sent, process, redeliver=True), first)
        self.assertEqual(await self.submit(read, process, redeliver=True), inspected)
        self.assertEqual(len(self.hgs.calls("send")), 1)
        self.assertEqual(len(self.hgs.calls("inspect")), 1)
        # A different request under a preserved identity never executes.
        changed = {**sent, "payload": {**sent["payload"], "text": "changed"}}
        mark = len(self.relay.deliveries)
        await self.relay.queue(changed, redeliver=True)
        await self.settle(sent["request_id"], mark, process)
        self.assertEqual(len(self.hgs.calls("send")), 1)
        self.assertEqual(self.relay.results(sent["request_id"]), [first, first])
        self.assertEqual(self.relay.state(sent["request_id"]), "claimed")
        self.assertEqual(json.loads(self.journal.row(sent["request_id"])["response"]), first)

    async def test_result_conflict_is_retired_without_blocking_later_requests(self):
        # The relay retired the claim (expiry or restart): keep the outcome
        # locally, never upload it again and keep claiming.
        retired = self.request("send", text="claim retired by the relay")
        rule = self.relay.rule("POST", rf"/v1/node/requests/{retired['request_id']}/result", 409)
        process = await self.launch()
        await self.relay.queue(retired)
        [(body, status)] = await self.until(lambda: self.relay.uploads(retired["request_id"]),
                                            what="the refused upload", process=process)
        self.assertEqual((status, body["state"]), (409, "completed"))
        await self.until(lambda: self.journal.row(retired["request_id"])["delivered"], what="local retirement",
                         process=process)
        later = self.request("inspect")
        self.assertEqual((await self.submit(later, process))["state"], "completed")
        self.assertEqual(len(self.relay.uploads(retired["request_id"])), 1)
        self.relay.rules.remove(rule)
        self.assertEqual(await self.submit(retired, process, redeliver=True), body)
        self.assertEqual(len(self.hgs.calls("send")), 1)

    async def test_rejected_result_blocks_later_claims_in_python(self):
        # Divergence b: Python re-raises a 400, 413 or 422 upload refusal
        # before it polls, so one result the relay rejects blocks every claim.
        if IMPLEMENTATION != "python":
            self.skipTest("divergence b: the Rust retirement after three spaced attempts is planned for H2")
        rejected = self.request("send", text="result the relay rejects")
        rule = self.relay.rule("POST", rf"/v1/node/requests/{rejected['request_id']}/result", 400)
        process = await self.launch()
        await self.relay.queue(rejected)
        await self.until(lambda: self.relay.uploads(rejected["request_id"]), what="the refused upload", process=process)
        later = self.request("inspect")
        await self.relay.queue(later)

        def polls():
            return sum(exchange.method == "GET" and exchange.path == "/v1/node/requests"
                       for exchange in self.relay.exchanges)

        before = polls()
        await self.until(lambda: len(self.relay.uploads(rejected["request_id"])) >= 2, what="the retried upload",
                         process=process)
        self.assertEqual((polls(), self.relay.state(later["request_id"]), self.hgs.calls("inspect")),
                         (before, "queued", []))
        # Claiming resumes once the relay accepts the stored result.
        self.relay.rules.remove(rule)
        [accepted] = await self.until(lambda: self.relay.results(later["request_id"]), 15, "the later request",
                                      process)
        self.assertEqual(accepted["state"], "completed")
        self.assertEqual(self.relay.state(rejected["request_id"]), "completed")
        self.assertEqual(len(self.hgs.calls("send")), 1)

    async def test_redirects_are_never_followed(self):
        elsewhere = FakeRelay(self.relay.token)
        await elsewhere.start()
        try:
            self.relay.rule("GET", "/v1/node/requests", 302, times=1,
                            headers={"Location": elsewhere.url + "/v1/node/requests?wait=1"})
            self.relay.rule("POST", r"/v1/node/requests/[^/]+/result", 307, times=1,
                            headers={"Location": elsewhere.url + "/v1/node/heartbeat"})
            process = await self.launch()
            request = self.request("send", text="never uploaded elsewhere")
            result = await self.submit(request, process)
            self.assertEqual([status for _, status in self.relay.uploads(request["request_id"])], [307, 200])
            self.assertEqual(self.relay.uploads(request["request_id"])[0][0], result)
            self.assertEqual(elsewhere.exchanges, [], "a redirect carried the node token elsewhere")
            self.assertEqual(len(self.hgs.calls("send")), 1)
        finally:
            await elsewhere.stop()


if __name__ == "__main__":
    unittest.main()
