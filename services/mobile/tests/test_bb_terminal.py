"""Black-box Terminal screens and input: fixed argv, a short deadline, an
allowlist of keys, exact acknowledgements and short-lived screen receipts."""
import json
import time
import unittest

from bb_operations import (CONVERSATION, RECEIPTS, RUN, SESSION, TERMINAL_KEYS, OperationCase, help_text, listing,
                           size_vector, validation_cases)
from connector_harness import BlackBoxCase, default_rules

TERMINAL = ["terminal", SESSION, "--json"]
BINDING = "d" * 64
MISMATCH = "native Terminal acknowledgement identity or status did not match"
CHANGED = "native Terminal binding or input support changed"
EXPIRED = RECEIPTS["execute:terminal-input-expired"]["error"]
# The relay's claim deadline for terminal input; the connector refuses one more
# than 6 s ahead, so this leaves the most time a busy computer can use.
DEADLINE = 5.5


def screen(**changes) -> dict:
    """The native answer to a snapshot, also the fresh preflight of an input."""
    return {"request_id": "{{stdin.request_id}}", "name": SESSION, "run_id": RUN, "conversation_id": CONVERSATION,
            "status": "snapshot", "terminal_binding_id": BINDING, "input_supported": True,
            "multiline_supported": False, "lines": ["$ synthetic"], **changes}


def submitted(**changes) -> dict:
    return {"request_id": "{{stdin.request_id}}", "name": SESSION, "run_id": RUN, "conversation_id": CONVERSATION,
            "terminal_binding_id": BINDING, "status": "submitted", **changes}


def terminal(fresh=None, ack=None) -> list[dict]:
    return [{"argv": TERMINAL, "stdin_has": {"action": "snapshot"}, "stdout": screen() if fresh is None else fresh},
            {"argv": TERMINAL, "stdin_has": {"action": "input"}, "stdout": submitted() if ack is None else ack}]


class TerminalTests(OperationCase, BlackBoxCase):
    async def asyncSetUp(self):
        await super().asyncSetUp()
        self.hgs.script(terminal() + default_rules(listing(), help_text("terminal")))

    def text(self, text="echo synthetic", enter=True, **changes):
        return self.operation("terminal_input", terminal_binding_id=BINDING, text=text, enter=enter, **changes)

    async def test_vector_screens_and_input_reach_fixed_native_argv(self):
        process = await self.launch()
        await self.replay(process, "terminal-snapshot")
        await self.replay(process, "terminal-input-submitted", expires_in=DEADLINE)
        await self.replay(process, "terminal-input-expired")

    async def test_input_runs_once_after_a_fresh_preflight(self):
        process = await self.launch()
        request = self.text("literal ; $(never run) 'quotes'")
        expires_at = time.time() + DEADLINE
        mark = self.mark()
        result = await self.submit(request, process, expires_at=expires_at)
        self.assertEqual(result, {"state": "completed", "error": None, "result": {
            **submitted(), "request_id": request["request_id"]}})
        preflight, handoff = self.since(mark, *TERMINAL)
        identity = {key: request["payload"][key] for key in ("request_id", "expected_run_id", "expected_conversation_id")}
        self.assertEqual(json.loads(preflight["stdin"]), {**identity, "action": "snapshot"})
        self.assertEqual(json.loads(handoff["stdin"]), {**request["payload"], "action": "input", "expires_at": expires_at})
        await self.delivered(process, request)
        self.assertEqual(await self.submit(request, process, redeliver=True, expires_at=expires_at), result)
        self.assertEqual(len(self.since(mark, *TERMINAL)), 2)

    async def test_every_allowlisted_key_and_no_other(self):
        process = await self.launch()
        for key in TERMINAL_KEYS:
            with self.subTest(key):
                request = self.operation("terminal_input", terminal_binding_id=BINDING, key=key)
                mark = self.mark()
                result = await self.submit(request, process, expires_at=time.time() + DEADLINE)
                self.assertEqual(result["state"], "completed", result)
                handoff = self.since(mark, *TERMINAL)[-1]
                self.assertEqual(json.loads(handoff["stdin"])["key"], key)
        for key in ("C-z", "F1", "enter", "Enter ", "C-c C-c", "Space"):
            with self.subTest(key):
                mark = self.mark()
                result = await self.submit(self.operation("terminal_input", terminal_binding_id=BINDING, key=key),
                                           process, expires_at=time.time() + DEADLINE)
                self.assertEqual(result, {"state": "failed", "result": None, "error": "unsupported terminal key"})
                self.assertEqual(self.since(mark, *TERMINAL), [])

    async def test_literal_text_is_bounded_and_rejects_hidden_controls(self):
        process = await self.launch()
        for identity in ("terminal-text-64KiB", "terminal-text-64KiB+1"):  # 64 KiB of UTF-8, then one byte more.
            with self.subTest(identity):
                payload, expect = size_vector(identity)
                request = self.text(payload["text"], payload["enter"])
                mark = self.mark()
                result = await self.submit(request, process, expires_at=time.time() + DEADLINE)
                self.assertEqual(result["error"], expect["error"])
                self.assertEqual(len(self.since(mark, *TERMINAL)), 0 if expect["error"] else 2)
        for text, enter, state in (("\x1b[31m", True, "failed"), ("\x7f", False, "failed"), ("", False, "failed"),
                                   ("", True, "completed"), ("tab\tseparated", True, "completed")):
            with self.subTest(text=text):
                mark = self.mark()
                result = await self.submit(self.text(text, enter), process, expires_at=time.time() + DEADLINE)
                self.assertEqual(result["state"], state, result)
                if state == "failed":
                    self.assertEqual(result["error"], "invalid literal terminal text")
                    self.assertEqual(self.since(mark, *TERMINAL), [])
        await self.assert_rejections(process, validation_cases("control", {"terminal_snapshot", "terminal_input"}),
                                     [TERMINAL])

    async def test_input_fails_closed_before_handoff(self):
        process = await self.launch()
        for deadline in (-1, 0, 30):
            with self.subTest(deadline=deadline):
                mark = self.mark()
                result = await self.submit(self.text(), process, expires_at=time.time() + deadline)
                self.assertEqual(result, {"state": "failed", "result": None, "error": EXPIRED})
                self.assertEqual(self.since(mark, *TERMINAL), [])
        multiline = self.text("first\nsecond")
        for name, fresh, request in (("binding", screen(terminal_binding_id="e" * 64), self.text()),
                                     ("identity", screen(run_id="replacement-run"), self.text()),
                                     ("input", screen(input_supported=False), self.text()),
                                     ("multiline", screen(), multiline)):
            with self.subTest(name):
                self.hgs.prepend(*terminal(fresh=fresh))
                mark = self.mark()
                result = await self.submit(request, process, expires_at=time.time() + DEADLINE)
                self.assertEqual(result, {"state": "failed", "result": None, "error": CHANGED})
                [preflight] = self.since(mark, *TERMINAL)
                self.assertEqual(json.loads(preflight["stdin"])["action"], "snapshot")
        self.hgs.prepend(*terminal(fresh=screen(multiline_supported=True)))
        self.assertEqual((await self.submit(self.text("first\nsecond"), process,
                                            expires_at=time.time() + DEADLINE))["state"], "completed")

    async def test_unconfirmed_input_is_uncertain_and_never_replays(self):
        process = await self.launch()
        for name, ack, state, error in (
                ("binding", submitted(terminal_binding_id="e" * 64), "uncertain", MISMATCH),
                ("request", submitted(request_id="other"), "uncertain", MISMATCH),
                ("status", submitted(status="snapshot"), "uncertain", MISMATCH),
                ("native uncertain", submitted(status="uncertain", error="Synthetic native doubt"), "uncertain",
                 "Synthetic native doubt"),
                ("native failed", submitted(status="failed", error="Synthetic native refusal"), "failed",
                 "Synthetic native refusal")):
            with self.subTest(name):
                self.hgs.prepend(*terminal(ack=ack))
                request, expires_at = self.text(), time.time() + DEADLINE
                mark = self.mark()
                result = await self.submit(request, process, expires_at=expires_at)
                self.assertEqual((result["state"], result["error"]), (state, error))
                if error != MISMATCH:
                    self.assertEqual(result["result"], {**ack, "request_id": request["request_id"]})
                await self.delivered(process, request)
                self.assertEqual(await self.submit(request, process, redeliver=True, expires_at=expires_at), result)
                self.assertEqual(len(self.since(mark, *TERMINAL)), 2)
        self.hgs.prepend(*terminal(fresh=screen(status="submitted")))
        result = await self.submit(self.operation("terminal_snapshot"), process)
        self.assertEqual(result, {"state": "failed", "result": None, "error": MISMATCH})

    async def test_without_the_terminal_abi_nothing_runs(self):
        self.hgs.script(terminal() + default_rules(listing()))
        process = await self.launch()
        capabilities = self.relay.heartbeats()[-1].body["snapshot"]["mobile_capabilities"]
        self.assertFalse({"terminal_snapshot", "terminal_input"} & set(capabilities["operations"]))
        self.assertEqual(capabilities["reasons"]["terminal"], "Native CLI lacks the scoped Terminal ABI")
        mark = self.mark()
        for request in (self.operation("terminal_snapshot"), self.text()):
            result = await self.submit(request, process, expires_at=time.time() + DEADLINE)
            self.assertEqual(result, {"state": "failed", "result": None,
                                      "error": "native scoped Terminal ABI is unavailable"})
        self.assertEqual(self.since(mark, *TERMINAL), [])

    async def test_screens_leave_the_journal_after_two_minutes_and_inputs_stay(self):
        first = await self.launch()
        kept, expired, typed = self.operation("terminal_snapshot"), self.operation("terminal_snapshot"), self.text()
        results, deadlines = {}, {}
        for request in (kept, expired, typed):
            deadlines[request["request_id"]] = time.time() + DEADLINE
            results[request["request_id"]] = await self.submit(request, first,
                                                               expires_at=deadlines[request["request_id"]])
        self.assertEqual(results[typed["request_id"]]["state"], "completed")
        await self.delivered(first, kept, expired, typed)
        first.stop()
        self.age(60, kept)
        self.age(121, expired)
        self.age(1000, typed)
        second = await self.launch()
        self.assertIsNone(self.journal.row(expired["request_id"]))
        self.assertIsNotNone(self.journal.row(kept["request_id"]))
        mark = self.mark()
        for request in (kept, typed):
            self.assertEqual(await self.submit(request, second, redeliver=True,
                                               expires_at=deadlines[request["request_id"]]),
                             results[request["request_id"]])
        self.assertEqual(self.since(mark, *TERMINAL), [])
        again = await self.submit(expired, second, redeliver=True)
        self.assertEqual(again["state"], "completed", again)
        self.assertEqual(len(self.since(mark, *TERMINAL)), 1)


if __name__ == "__main__":
    unittest.main()
