"""Black-box native timeouts, output caps, failing native commands, results
too large for the relay, and the poison pill of divergence b.

A native call that runs out of its budget is killed; its request fails as a
read or stays uncertain as a mutation and never runs again. Output beyond
8 MiB, a failing exit and invalid JSON become fixed receipt texts that carry
nothing the native command printed. A receipt is uploaded when its canonical
form with its request_id is at most 1 MiB, as the relay requires. Python
measures its plain JSON against 1 MiB instead and keeps a native error of any
length, so a receipt just within that measure, or with an error beyond 4,096
code points, is refused by the relay and, retried before every poll, blocks
every later claim; Rust sanitises such a receipt and retires a stored one the
relay keeps refusing (divergence b).

Python's 45 s default native timeout is not run here; the 2 s budget of
`hgs history` and the 10 s budget of the delivery setting cover the same
path, and the default runs against Rust through its test hook.
"""
import json
import os
import unittest

from connector_harness import CLOCK_SCALE_HOOK, IMPLEMENTATION, MIB, NATIVE_TIMEOUT_HOOK, canonical
from session_harness import (CONVERSATION, OPERATIONS, RUN, SESSION, SessionCase, ack, execution, filled, size_case,
                             submission)

CAP = OPERATIONS["constants"]["MAX_BYTES"]
TIMED_OUT = "native command timed out"
TOO_LARGE = "native result exceeds relay size limit"
OUTPUT = "native output exceeds size limit"
SECRET = "bb-private-native-secret"


def exited(pid: int) -> bool:
    """The process is gone or a zombie: it can run no further."""
    try:
        with open(f"/proc/{pid}/stat") as stat:
            return stat.read().rsplit(")", 1)[1].split()[0] == "Z"
    except FileNotFoundError:
        return True
    except OSError:
        try:
            os.kill(pid, 0)
        except ProcessLookupError:
            return True
        return False


def exact_output(size: int) -> dict:
    """A JSON object of exactly `size` bytes as `stdout_text` of a rule."""
    head, tail = '{"request_id": "bb", "padding": "', '"}'
    return {"stdout_text": "@out", "fill": {"@out": [[head, 1], ["a", size - len(head) - len(tail)], [tail, 1]]}}


class NativeTimeoutTests(SessionCase):
    markers = ("history", "preference")

    async def assert_killed(self, call: dict) -> None:
        await self.until(lambda: exited(call["pid"]), 10, "the timed-out native child to be killed")
        self.assertIsNone(self.hgs.finished(call["pid"]), "the timed-out native call ran to its end")

    async def test_a_read_out_of_its_budget_fails_and_its_child_is_killed(self):
        self.hgs.script(self.rules({"prefix": ["history", SESSION, "--json"], "stdout": {}, "delay": 60}))
        process = await self.launch()
        request = submission("control", "valid/history")
        result = await self.submit(request, process, timeout=30)
        self.assertEqual(result, self.failed(TIMED_OUT))
        [call] = self.native_calls("history")
        self.assertEqual(json.loads(call["stdin"]), request["payload"])
        await self.assert_killed(call)
        self.assertIsNone(process.returncode)
        await self.assert_replays(request, result, process)

    async def test_a_mutation_out_of_its_budget_is_uncertain_and_never_retried(self):
        self.hgs.script(self.rules({"prefix": ["swarm", "preference"], "stdout": {"ok": True, "mobile_delivery": "away"},
                                    "delay": 60}))
        process = await self.launch()
        request = submission("launch", "valid/set_mobile_delivery")
        result = await self.submit(request, process, timeout=40)
        self.assertEqual(result, self.uncertain(TIMED_OUT))
        [call] = self.native_calls("swarm")
        await self.assert_killed(call)
        self.hgs.script(self.rules({"prefix": ["swarm", "preference"], "stdout": {"ok": True, "mobile_delivery": "away_5"}}))
        await self.assert_replays(request, result, process)
        self.assertEqual(json.loads(self.journal.row(request["request_id"])["response"]), result)

    async def test_the_default_native_timeout_through_the_rust_test_hook(self):
        if IMPLEMENTATION == "python":
            self.skipTest("Python's default native timeout is 45 s; the 2 s and 10 s budgets above take the same path")
        self.hgs.script(self.rules({"prefix": ["send"], "stdout": ack(status="submitted"), "delay": 60},
                                   {"argv": ["inspect", SESSION], "stdout": {}, "delay": 60}))
        process = await self.launch(env={NATIVE_TIMEOUT_HOOK: "1"})
        sent = self.envelope("send", {"text": "never answered"})
        self.assertEqual(await self.submit(sent, process, timeout=20), self.uncertain(TIMED_OUT))
        inspected = self.envelope("inspect", {}, identity=False)
        self.assertEqual(await self.submit(inspected, process, timeout=20), self.failed(TIMED_OUT))
        for call in self.native_calls():
            await self.assert_killed(call)
        await self.assert_replays(sent, self.uncertain(TIMED_OUT), process)


class NativeOutputTests(SessionCase):
    large_bodies = True

    async def test_native_output_is_capped_at_eight_mebibytes(self):
        process = await self.launch()
        cases = [("send", exact_output(CAP), self.uncertain(TOO_LARGE)),
                 ("send", exact_output(CAP + 1), self.uncertain(OUTPUT)),
                 ("process_output", exact_output(CAP + 1), self.failed(OUTPUT)),
                 ("send", {"stdout": ack(status="submitted"), "stderr": "@err", "fill": {"@err": [["e", CAP + 1]]}},
                  self.uncertain(OUTPUT))]
        for operation, answer, receipt in cases:
            with self.subTest(operation, receipt=receipt["error"]):
                self.hgs.script(self.rules({"prefix": ["send" if operation == "send" else "processes"], **answer}))
                payload = {"text": "flood"} if operation == "send" else {"process_id": "job"}
                self.assertEqual(await self.submit(self.envelope(operation, payload), process, timeout=30), receipt)
        stderr = {"stdout": ack(status="submitted"), "stderr": "@err", "fill": {"@err": [[SECRET + "\n", 4096]]}}
        self.hgs.script(self.rules({"prefix": ["send"], **stderr}))
        result = await self.submit(self.envelope("send", {"text": "noisy but fine"}), process)
        self.assertEqual(result["state"], "completed")
        self.assertNotIn(SECRET, json.dumps(result))

    async def test_failing_or_invalid_native_output_reaches_no_receipt(self):
        answers = [({"stdout_text": SECRET + "\n"}, "native command returned invalid JSON"),
                   ({"stdout_text": ""}, "native command returned invalid JSON"),
                   ({"stdout_text": '{"ok": true'}, "native command returned invalid JSON"),
                   ({"stdout": {"ok": True, "detail": SECRET}, "stderr": SECRET + "\n", "exit": 3}, "native command failed"),
                   ({"stdout": {"ok": False, "error": SECRET}, "exit": 1}, "native command failed"),
                   ({"stdout_text": "", "stderr": SECRET + "\n", "exit": 8}, "native command failed")]
        process = await self.launch()
        for answer, message in answers:
            for operation, state in (("send", "uncertain"), ("process_output", "failed")):
                with self.subTest(operation, answer=answer):
                    self.hgs.script(self.rules({"prefix": ["send" if operation == "send" else "processes"], **answer}))
                    payload = {"text": "x"} if operation == "send" else {"process_id": "job"}
                    request = self.envelope(operation, payload)
                    receipt = await self.submit(request, process)
                    self.assertEqual(receipt, {"state": state, "result": None, "error": message})
                    await self.assert_replays(request, receipt, process)
        self.assertNotIn(SECRET.encode(), (self.state_dir / "journal.sqlite3").read_bytes())
        self.assertNotIn(SECRET, process.output())


class ResultSizeTests(SessionCase):
    large_bodies = True

    def fitting(self, unit: str, request_id: str) -> dict:
        """A send result whose receipt with this request_id is exactly 1 MiB in
        the relay's canonical form (its plain JSON is a little smaller)."""
        def size(output):
            return len(canonical({"request_id": request_id, "state": "completed", "error": None,
                                  "result": {"output": output, "request_id": request_id}}))
        room = MIB - size("")
        units = room // (size(unit) - size(""))
        return {"output": unit * units + "a" * (room - units * (size(unit) - size(""))), "request_id": request_id}

    async def test_a_result_fits_only_within_the_relay_limit(self):
        process = await self.launch()
        for unit in ("a", "\u00e9", "\U0001f600"):
            with self.subTest(unit=unit):
                request = self.envelope("send", {"text": "large result"})
                result = self.fitting(unit, request["request_id"])
                self.hgs.script(self.rules({"prefix": ["send"], "stdout": result}))
                receipt = await self.submit(request, process)
                self.assertEqual(receipt, {"state": "completed", "result": result, "error": None})
                self.assertEqual(len(canonical({"request_id": request["request_id"], **receipt})), MIB)
        for name in ("max-result-ascii-1", "max-result-latin-1", "max-result-non-bmp-1"):
            with self.subTest(name):
                case = size_case(name)
                self.assertTrue(case["expect"]["exceeds"])
                value = filled(case["value"], case["fill"])["result"]
                self.hgs.script(self.rules({"prefix": ["send"], "stdout": {**value, "request_id": "{{stdin.request_id}}"}}))
                request = self.envelope("send", {"text": "too large a result"})
                receipt = await self.submit(request, process)
                self.assertEqual(receipt, self.uncertain(TOO_LARGE))
                await self.assert_replays(request, receipt, process)
        run = execution("process-output-too-large")
        [native] = run["native"]
        self.hgs.script(self.rules({"argv": native["argv"], "stdout": native["answer"], "fill": native["fill"]}))
        self.assertEqual(await self.submit({**run["request"], "computer_id": self.relay.computer_id}, process),
                         json.loads(run["expect"]["stored"]))
        large = {"name": SESSION, "run_id": RUN, "conversation_id": CONVERSATION, "text": "@big"}
        self.hgs.script(self.rules({"argv": ["inspect", SESSION], "stdout": large, "fill": {"@big": [["x", MIB]]}}))
        self.assertEqual(await self.submit(self.envelope("inspect", {}, identity=False), process), self.failed(
            "native inspection metadata exceeds relay size limit; required disclosures were not cropped"))


class RefusedResultTests(SessionCase):
    """Divergence b: a receipt the relay refuses."""

    markers = ("lifecycle",)
    large_bodies = True

    def refused(self, request_id: str) -> list:
        """Upload refusals of this receipt, provoked here and so no violation."""
        path = f"POST /v1/node/requests/{request_id}/result: "
        found = [item for item in self.relay.violations if item.startswith(path)]
        for item in found:
            self.relay.violations.remove(item)
        return found

    def polls(self) -> int:
        return sum(exchange.method == "GET" and exchange.path == "/v1/node/requests" for exchange in self.relay.exchanges)

    async def assert_blocks_python(self, request: dict, status: int, process) -> None:
        """Python retries the refused upload before every poll and claims nothing more."""
        await self.until(lambda: len(self.relay.uploads(request["request_id"])) >= 2, 20, "a retried upload", process)
        later = self.envelope("pause", {})
        await self.relay.queue(later)
        polls = self.polls()
        await self.until(lambda: len(self.relay.uploads(request["request_id"])) >= 3, 30, "another retry", process)
        self.assertEqual((self.polls(), self.relay.state(later["request_id"])), (polls, "queued"))
        self.assertEqual({code for _, code in self.relay.uploads(request["request_id"])}, {status})
        self.assertEqual(self.relay.state(request["request_id"]), "claimed")
        self.assertTrue(self.refused(request["request_id"]))

    async def test_a_native_error_beyond_4096_characters(self):
        long = "\u00e9" * 4097
        for index, (error, kept) in enumerate(((long, long[:4096]), ({"detail": "not text"}, None))):
            with self.subTest(type(error).__name__):
                self.hgs.script(self.rules({"prefix": ["session-action"], "stdout": ack(status="failed", error=error)}))
                # A journal of its own: Python's stays blocked by its refused row.
                process = await self.launch(self.config(state_dir=str(self.scratch / f"state-{index}")))
                request = self.envelope("pause", {})
                await self.relay.queue(request)
                if IMPLEMENTATION == "python":
                    await self.assert_blocks_python(request, 400, process)
                else:
                    [receipt] = await self.until(lambda: self.relay.results(request["request_id"]), 20,
                                                 "the sanitised upload", process)
                    self.assertEqual(receipt["state"], "failed")
                    self.assertIsInstance(receipt["error"], str)
                    self.assertLessEqual(len(receipt["error"]), 4096)
                    if kept is not None:
                        self.assertEqual(receipt["error"], kept)
                process.stop()

    async def test_a_receipt_beyond_the_relay_limit(self):
        # The plain JSON of this receipt is exactly MAX_RESULT, which Python
        # admits; with its request_id the canonical form exceeds 1 MiB.
        case = size_case("max-result-ascii-0")
        self.assertFalse(case["expect"]["exceeds"])
        value = filled(case["value"], case["fill"])["result"]
        self.hgs.script(self.rules({"prefix": ["send"], "stdout": {**value, "request_id": "{{stdin.request_id}}"}}))
        process = await self.launch()
        request = self.envelope("send", {"text": "a receipt at the limit"})
        await self.relay.queue(request)
        if IMPLEMENTATION == "python":
            await self.assert_blocks_python(request, 413, process)
            stored = json.loads(self.journal.row(request["request_id"])["response"])
            self.assertEqual((stored["state"], len(json.dumps(stored))), ("completed", case["expect"]["bytes"]))
        else:
            [receipt] = await self.until(lambda: self.relay.results(request["request_id"]), 20, "the sanitised upload",
                                         process)
            self.assertEqual((receipt["state"], receipt["result"]), ("uncertain", None))
            self.assertIsInstance(receipt["error"], str)
        self.assertEqual(len(self.native_calls("send")), 1)

    async def test_rust_retires_a_stored_result_the_relay_keeps_refusing(self):
        if IMPLEMENTATION == "python":
            self.skipTest("Python blocks instead: test_bb_requests.test_rejected_result_blocks_later_claims_in_python")
        self.hgs.script(self.rules({"prefix": ["session-action"], "stdout": ack(status="completed")}))
        refused = self.envelope("pause", {})
        self.relay.rule("POST", rf"/v1/node/requests/{refused['request_id']}/result", 422)
        # 30 s between attempts become 1 s.
        process = await self.launch(env={CLOCK_SCALE_HOOK: "30"})
        await self.relay.queue(refused)
        later = self.envelope("resume", {})
        self.assertEqual((await self.submit(later, process, timeout=20))["state"], "completed")
        await self.until(lambda: len(self.relay.uploads(refused["request_id"])) >= 3, 30, "three attempts", process)
        await self.until(lambda: self.journal.row(refused["request_id"])["delivered"], 10, "local retirement", process)
        self.assertEqual([status for _, status in self.relay.uploads(refused["request_id"])], [422, 422, 422])
        self.assertEqual(self.relay.state(refused["request_id"]), "claimed")
        self.assertIn("Relay rejected a stored result; preserving local outcome", process.output())
        after = self.envelope("resume", {})
        self.assertEqual((await self.submit(after, process, timeout=20))["state"], "completed")
        self.assertEqual(len(self.relay.uploads(refused["request_id"])), 3)
        self.assertEqual(len(self.native_calls("session-action")), 3)


if __name__ == "__main__":
    unittest.main()
