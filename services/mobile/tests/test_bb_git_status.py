"""Black-box Git checkout reads through the actual Rust connector process.

Only an exact native ABI marker offers this computer-scoped read. The native
command receives the literal requested path and no stdin; its bounded result
keeps only checkout evidence, separately from the original request path.
All native answers are synthetic, the relay is loopback and HOME is private.
"""
import json
import signal
import time
import unittest
import uuid

from connector_harness import IMPLEMENTATION, RESTART_ERROR, stopped_journal
from session_harness import SESSION, SessionCase, ack, help_text

PATH = "/example/alias with spaces/$(literal);--flag/Сводная"
CANONICAL = "/example/canonical/worktree"
MARKER = "Mobile Git status ABI: git-status-v1"
MAX_BYTES = 32 * 1024
HOUR = 3600
WEEK = 7 * 86400
UNAVAILABLE = "native Git status ABI is unavailable"
INVALID = "native Git status has invalid format"
FIELDS = "invalid Git status fields or absolute path"
STATES = ("ok", "unavailable", "folder_unavailable", "timeout", "git_unavailable", "metadata_error", "too_large",
          "not_repo", "changed_during_check")
REMOTE_STATES = ("unknown", "detached", "no_upstream", "local_upstream", "unavailable", "verified", "changed", "missing")
STRINGS = ("state", "path", "remote_state", "head", "branch", "upstream", "root", "common_dir", "remote", "remote_ref")
EXPIRED = {"state": "completed", "result": {"status": "history_expired"},
           "error": "Native result history expired; this request will not execute again"}


def argv(path=PATH):
    return ["git-status", "--path", path, "--json"]


def snapshot(**changes):
    return {"state": "ok", "path": CANONICAL, "remote_state": "verified", "head": "a" * 40,
            "branch": "feature/status", "upstream": "origin/main", "root": CANONICAL,
            "common_dir": "/example/canonical/.git", "remote": "origin", "remote_ref": "refs/heads/main",
            "unborn": False, "detached": False, "changed_files": 2, "conflicts": 0, "ahead": 3, "behind": None,
            "sampled_at": 1700000000.5, "remote_checked_at": 1700000000, "remote_age_seconds": 0.5, **changes}


def failed(error):
    # New Rust-only errors deliberately stay outside the frozen Python vectors.
    return {"state": "failed", "result": None, "error": error}


@unittest.skipUnless(IMPLEMENTATION == "rust", "git_status is an additive Rust-only operation")
class GitStatusTests(SessionCase):
    markers = ("git_status",)

    def read(self, path=PATH):
        request = self.envelope("git_status", {"path": path}, session="", identity=False)
        request["payload"]["request_id"] = request["request_id"]
        return request

    def answer(self, native=None, *, path=PATH, **rule):
        if not rule:
            rule = {"stdout": snapshot() if native is None else native}
        self.hgs.prepend({"argv": argv(path), **rule})

    def completed(self, request, native):
        return {"state": "completed", "error": None, "result": {
            "schema": 1, "request_id": request["request_id"], "requested_path": request["payload"]["path"],
            "snapshot": native}}

    async def delivered(self, process, *requests):
        await self.until(lambda: all((row := self.journal.row(request["request_id"])) and row["delivered"]
                                     for request in requests), what="delivered Git read journal rows", process=process)

    def age(self, seconds, *requests):
        with stopped_journal(self.state_dir) as db:
            db.executemany("UPDATE requests SET created_at=created_at-?,updated_at=updated_at-?,"
                           "delivered_at=delivered_at-? WHERE request_id=?",
                           [(seconds, seconds, seconds, request["request_id"]) for request in requests])

    async def test_only_the_exact_native_marker_advertises_and_enables_the_read(self):
        for name, help_value, enabled in (
                ("old CLI", help_text(), False),
                ("command syntax alone", help_text() + "hgs git-status --path PATH --json\n", False),
                ("other version", help_text() + MARKER.replace("-v1", "-v10") + "\n", False),
                ("marker prefix only", help_text() + MARKER + " unavailable\n", False),
                ("exact indented help line", help_text() + "       " + MARKER + "\n", True)):
            with self.subTest(name):
                self.hgs.script(self.rules(markers=()))
                self.hgs.prepend({"argv": ["--help"], "stdout_text": help_value})
                self.answer()
                process = await self.launch()
                capabilities = self.relay.heartbeats()[-1].body["snapshot"]["mobile_capabilities"]
                self.assertEqual("git_status" in capabilities["operations"], enabled)
                self.assertFalse({"catalog", "dirs", "worktrees", "launch"} & set(capabilities["operations"]))
                before = len(self.native_calls())
                request = self.read()
                self.assertEqual(await self.submit(request, process),
                                 self.completed(request, snapshot()) if enabled else failed(UNAVAILABLE))
                self.assertEqual(len(self.native_calls()) - before, int(enabled))
                process.stop()

    async def test_literal_argv_no_stdin_and_only_the_nineteen_snapshot_fields(self):
        native = snapshot(files=["BB-PRIVATE-FILE"], remote_url="https://private.example.test/repository",
                          private={"secret": "BB-PRIVATE-NATIVE"}, requested_path="/example/not-the-request")
        self.answer(native)
        process = await self.launch()
        request = self.read()
        self.assertEqual(set(request["payload"]), {"request_id", "path"})
        result = await self.submit(request, process)
        self.assertEqual(result, self.completed(request, snapshot()))
        self.assertEqual(len(result["result"]["snapshot"]), 19)
        [call] = self.native_calls()
        self.assertEqual((call["argv"], call["stdin"]), (argv(), ""))
        self.assertNotEqual(result["result"]["requested_path"], result["result"]["snapshot"]["path"])
        stored = self.journal.row(request["request_id"])
        self.assertEqual((stored["operation"], stored["state"], json.loads(stored["response"])),
                         ("git_status", "completed", result))
        self.assertNotIn("BB-PRIVATE", stored["response"])

    async def test_partial_evidence_keeps_absent_counts_absent_and_preserves_known_states(self):
        process = await self.launch()
        examples = [{"state": state, "path": CANONICAL, "remote_state": "unknown"} for state in STATES]
        examples += [{"state": "ok", "path": CANONICAL, "remote_state": remote} for remote in REMOTE_STATES]
        examples += [{"state": "ok", "path": CANONICAL, "remote_state": "unknown", "ahead": None, "behind": None},
                     {"state": "ok", "path": CANONICAL, "remote_state": "verified", "changed_files": 0,
                      "conflicts": 0, "ahead": 0, "behind": 0, "sampled_at": 0, "remote_checked_at": 0.0,
                      "remote_age_seconds": 0}]
        for native in examples:
            with self.subTest(native):
                self.answer(native)
                request = self.read()
                self.assertEqual(await self.submit(request, process), self.completed(request, native))
        self.assertEqual(len(self.native_calls("git-status")), len(examples))

    async def test_wrong_targets_extra_fields_and_malformed_payloads_never_call_native(self):
        process = await self.launch()
        cases = []
        for session in (SESSION, "claude/unknown", None, [], 1):
            request = self.read()
            request["session"] = session
            cases.append((f"session {session!r}", request, "Git status uses no session target"))
        for key, value in (("expected_run_id", "example-run"), ("expected_conversation_id", "example-conversation"),
                           ("agent_id", "worker-1"), ("archive_id", str(uuid.uuid4())),
                           ("computer_id", "another-computer"), ("requested_path", PATH), ("refresh", True),
                           ("argv", ["--refresh"]), ("extra", None)):
            request = self.read()
            request["payload"][key] = value
            cases.append((key, request, FIELDS))
        for path in (None, True, 1, [], {}, "", "relative", "~/example", "--help", "C:\\fixture\\repo", "/example/\nrepo",
                     "/example/\x7frepo", "/" + "a" * 4096, "/" + "é" * 2048):
            cases.append((f"path {str(path)[:40]!r}", self.read(path), FIELDS))
        for key in ("request_id", "path"):
            request = self.read()
            del request["payload"][key]
            cases.append((f"missing {key}", request, FIELDS))
        request = self.read()
        request["payload"]["request_id"] = str(uuid.uuid4())
        cases.append(("other payload identity", request, FIELDS))
        for payload in (None, [], "not an object", True):
            request = self.read()
            request["payload"] = payload
            cases.append((f"payload {payload!r}", request, FIELDS))
        for name, request, error in cases:
            with self.subTest(name):
                self.assertEqual(await self.outcome(request, process), failed(error))
        self.assertEqual(self.native_calls(), [])
        # The path limit measures UTF-8 bytes and still accepts its exact edge.
        path = "/" + "é" * 2047 + "a"
        self.assertEqual(len(path.encode()), 4096)
        self.answer(path=path)
        request = self.read(path)
        self.assertEqual(await self.submit(request, process), self.completed(request, snapshot()))
        self.assertEqual([call["argv"] for call in self.native_calls()], [argv(path)])

    async def test_invalid_native_types_and_unknown_states_fail_without_private_output(self):
        cases = [("array", []), ("scalar", "BB-PRIVATE-NATIVE"), ("null", None)]
        for key in ("state", "path", "remote_state"):
            native = snapshot()
            del native[key]
            cases.append((f"missing {key}", native))
        cases += [("unknown state", snapshot(state="success")), ("unknown remote state", snapshot(remote_state="synced"))]
        cases += [(f"nonstring {key}", snapshot(**{key: 1})) for key in STRINGS]
        for key in ("unborn", "detached"):
            cases += [(f"{key} {value!r}", snapshot(**{key: value})) for value in (0, "false", None)]
        for key in ("changed_files", "conflicts", "ahead", "behind"):
            cases += [(f"{key} {value!r}", snapshot(**{key: value})) for value in (-1, True, 0.0, 0.5, "0")]
        cases += [(f"null {key}", snapshot(**{key: None})) for key in ("changed_files", "conflicts")]
        for key in ("sampled_at", "remote_checked_at", "remote_age_seconds"):
            cases += [(f"{key} {value!r}", snapshot(**{key: value})) for value in (-0.5, True, None, "0")]
        process = await self.launch()
        for name, native in cases:
            with self.subTest(name):
                self.answer(stdout=native)
                request = self.read()
                self.assertEqual(await self.submit(request, process), failed(INVALID))
                self.assertNotIn("BB-PRIVATE", self.journal.row(request["request_id"])["response"])
        self.assertEqual(len(self.native_calls()), len(cases))

    async def test_native_output_and_projected_result_have_separate_32_kib_caps(self):
        process = await self.launch()
        native = {"state": "ok", "path": CANONICAL, "remote_state": "unknown", "private": ""}
        encoded = json.dumps(native)
        for extra in (0, 1):
            with self.subTest(extra=extra):
                padded = encoded + " " * (MAX_BYTES - len(encoded.encode()) + extra)
                self.answer(stdout_text=padded)
                request = self.read()
                projected = {key: value for key, value in native.items() if key != "private"}
                self.assertEqual(await self.submit(request, process), self.completed(request, projected) if not extra
                                 else failed("native output exceeds size limit"))
        for name, rule, error in (
                ("stderr", {"stdout": snapshot(), "stderr": "e" * (MAX_BYTES + 1)}, "native output exceeds size limit"),
                ("projected Unicode", {"stdout_text": json.dumps(snapshot(branch="é" * 6000), ensure_ascii=False)},
                 "native Git status exceeds size limit")):
            with self.subTest(name):
                self.answer(**rule)
                self.assertEqual(await self.submit(self.read(), process), failed(error))

    async def test_native_failures_are_failed_reads_with_fixed_errors_and_replay(self):
        process = await self.launch()
        overflow = json.dumps(snapshot(remote_age_seconds="@overflow")).replace('"@overflow"', "1e999")
        for name, rule, error in (
                ("exit", {"stdout": {"secret": "BB-PRIVATE-NATIVE"}, "stderr": "BB-PRIVATE-ERROR", "exit": 4},
                 "native command failed"),
                ("truncated", {"stdout_text": '{"state":"ok"'}, "native command returned invalid JSON"),
                ("empty", {"stdout_text": ""}, "native command returned invalid JSON"),
                ("NaN", {"stdout_text": '{"sampled_at":NaN}'}, "native command returned invalid JSON"),
                ("infinite numeric value", {"stdout_text": overflow}, INVALID)):
            with self.subTest(name):
                self.answer(**rule)
                request = self.read()
                result = await self.submit(request, process)
                self.assertEqual(result, failed(error))
                self.assertEqual(self.journal.row(request["request_id"])["state"], "failed")
                await self.assert_replays(request, result, process)
        self.assertNotIn("BB-PRIVATE", process.output())

    async def test_git_read_has_a_five_second_native_budget_and_never_retries_that_identity(self):
        self.answer(stdout=snapshot(), delay=60)
        process = await self.launch()
        request = self.read()
        started = time.monotonic()
        result = await self.submit(request, process, timeout=15)
        elapsed = time.monotonic() - started
        self.assertEqual(result, failed("native command timed out"))
        self.assertGreaterEqual(elapsed, 4.0)
        self.assertLess(elapsed, 15)
        [call] = self.native_calls()
        self.assertEqual((call["argv"], call["stdin"]), (argv(), ""))
        self.assertIsNone(self.hgs.finished(call["pid"]), "the timed-out fake native command ran to its end")
        await self.assert_replays(request, result, process)

    async def test_duplicate_hash_conflicts_and_restart_preserve_the_original_read_identity(self):
        self.answer()
        first = await self.launch()
        request = self.read()
        result = await self.submit(request, first)
        self.assertEqual(result, self.completed(request, snapshot()))
        await self.delivered(first, request)
        original = self.journal.row(request["request_id"])
        self.assertRegex(original["request_hash"], r"^[0-9a-f]{64}$")
        await self.assert_replays(request, result, first)
        for change in ({"path": "/example/other"}, {"extra": True}):
            changed = {**request, "payload": {**request["payload"], **change}}
            before, deliveries = len(self.relay.results(request["request_id"])), len(self.relay.deliveries)
            await self.relay.queue(changed, redeliver=True)
            await self.settle(request["request_id"], deliveries, first)
            self.assertEqual(len(self.relay.results(request["request_id"])), before)
            self.assertEqual(self.journal.row(request["request_id"])["request_hash"], original["request_hash"])
        first.stop()
        second = await self.launch()
        await self.assert_replays(request, result, second)
        preserved = self.journal.row(request["request_id"])
        # Delivery advances updated_at/delivered_at; neither is request identity.
        for key in ("request_id", "operation", "request_hash", "created_at", "response"):
            self.assertEqual(preserved[key], original[key], key)
        self.assertEqual([call["argv"] for call in self.native_calls()], [argv()])

    async def test_marker_withdrawal_replays_an_existing_read_but_refuses_a_new_identity(self):
        self.answer()
        first = await self.launch()
        request = self.read()
        result = await self.submit(request, first)
        await self.delivered(first, request)
        original = self.journal.row(request["request_id"])
        first.stop()
        # A restart observes withdrawn help without waiting for the normal
        # five-minute capability cache; the persisted identity stays authoritative.
        self.hgs.script(self.rules(markers=()))
        second = await self.launch()
        self.assertNotIn("git_status", self.relay.heartbeats()[-1].body["snapshot"]["mobile_capabilities"]["operations"])
        await self.assert_replays(request, result, second)
        self.assertEqual(await self.submit(self.read(), second), failed(UNAVAILABLE))
        preserved = self.journal.row(request["request_id"])
        self.assertEqual((preserved["request_hash"], preserved["response"]),
                         (original["request_hash"], original["response"]))
        self.assertEqual(len(self.native_calls("git-status")), 1)

    async def test_a_crashed_native_read_recovers_as_failed_and_does_not_execute_again(self):
        self.answer(stdout=snapshot(), kill_parent=True)
        first = await self.launch()
        request = self.read()
        await self.relay.queue(request)
        self.assertEqual(await first.wait_exit(10), -signal.SIGKILL)
        original = self.journal.row(request["request_id"])
        self.assertEqual(original["state"], "running")
        self.hgs.script(self.rules())
        second = await self.launch()
        result = failed(RESTART_ERROR)
        self.assertEqual(await self.until(lambda: self.relay.results(request["request_id"]),
                                          what="the failed interrupted Git read upload", process=second), [result])
        await self.assert_replays(request, result, second)
        self.assertEqual(self.journal.row(request["request_id"])["request_hash"], original["request_hash"])
        self.assertEqual(len(self.native_calls("git-status")), 1)

    async def test_delivered_git_reads_expire_after_an_hour_and_mutation_tombstones_stay(self):
        self.hgs.prepend({"prefix": ["send"], "stdout": ack(status="submitted")})
        self.answer()
        self.answer({}, path="/example/failed")
        first = await self.launch()
        read, refused = self.read(), self.read("/example/failed")
        sent = self.envelope("send", {"text": "synthetic retained mutation"})
        result, mutation = await self.submit(read, first), await self.submit(sent, first)
        self.assertEqual(result, self.completed(read, snapshot()))
        self.assertEqual(mutation["state"], "completed")
        self.assertEqual(await self.submit(refused, first), failed(INVALID))
        await self.delivered(first, read, refused, sent)
        first.stop()
        self.age(121, read, refused, sent)
        second = await self.launch()
        await self.assert_replays(read, result, second)
        await self.assert_replays(refused, failed(INVALID), second)
        await self.delivered(second, read, refused)
        second.stop()
        self.age(HOUR + 1, read, refused)
        self.age(WEEK + 1, sent)
        third = await self.launch()
        self.assertIsNone(self.journal.row(read["request_id"]))
        self.assertIsNone(self.journal.row(refused["request_id"]))
        self.assertTrue(self.journal.row(sent["request_id"])["response_expired"])
        self.assertEqual(await self.submit(read, third, redeliver=True), result)
        self.assertEqual(await self.submit(sent, third, redeliver=True), EXPIRED)
        self.assertEqual((len(self.native_calls("git-status")), len(self.native_calls("send"))), (3, 1))

    async def test_an_undelivered_git_result_survives_age_and_marker_withdrawal(self):
        self.answer()
        request = self.read()
        refusal = self.relay.rule("POST", rf"/v1/node/requests/{request['request_id']}/result", 503)
        first = await self.launch()
        await self.relay.queue(request)
        [(result, _)] = await self.until(lambda: self.relay.uploads(request["request_id"])[:1],
                                         what="the refused Git read upload", process=first)
        self.assertEqual(result, self.completed(request, snapshot()))
        first.stop()
        self.age(WEEK + 1, request)
        self.relay.rules.remove(refusal)
        self.hgs.script(self.rules(markers=()))
        second = await self.launch()
        self.assertEqual(await self.until(lambda: self.relay.results(request["request_id"]),
                                          what="the preserved Git read upload", process=second), [result])
        self.assertFalse(self.journal.row(request["request_id"])["response_expired"])
        self.assertEqual(len(self.native_calls("git-status")), 1)

    async def test_git_status_and_old_reads_share_the_five_thousand_row_quota(self):
        self.hgs.prepend({"prefix": ["send"], "stdout": ack(status="submitted")})
        first = await self.launch()
        first.stop()
        now = time.time()
        receipt = json.dumps({"state": "completed", "result": {}, "error": None})
        # Use the same stopped-journal fixture as the existing read quota
        # coverage. Both classes together reach the shared cap, neither alone does.
        with stopped_journal(self.state_dir) as db:
            db.executemany("INSERT INTO requests(request_id,operation,state,response,delivered,request_hash,created_at,"
                           "updated_at,delivered_at) VALUES(?,?,'completed',?,1,?,?,?,?)",
                           [(str(uuid.uuid4()), "git_status" if index % 2 else "inspect", receipt, "0" * 64,
                             now, now, now) for index in range(5000)])
        second = await self.launch()
        reads = [self.read(), self.envelope("inspect", {}, identity=False)]
        for request in reads:
            await self.relay.queue(request)
        mutation = self.envelope("send", {"text": "mutations retain independent capacity"})
        self.assertEqual((await self.submit(mutation, second))["state"], "completed")
        for request in reads:
            self.assertEqual((self.relay.state(request["request_id"]), self.relay.uploads(request["request_id"])),
                             ("claimed", []))
            self.assertIsNone(self.journal.row(request["request_id"]))
        self.assertEqual((self.native_calls("git-status"), self.native_calls("inspect")), ([], []))
        self.assertEqual(len(self.journal.rows()), 5001)


if __name__ == "__main__":
    unittest.main()
