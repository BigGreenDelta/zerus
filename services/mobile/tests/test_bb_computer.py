"""Black-box computer-scoped heartbeat projections: projects, accounts and attention.

Each projection reads native hgs on its own schedule, carries only reviewed
fields and keeps its last good value, marked stale, when a read fails."""
import copy
import hashlib
import time
import unittest

from bb_operations import OperationCase, projection as call_vector
from connector_harness import BlackBoxCase, default_rules, listing, session_row

PRIVATE = ("must not leave", "/private/remote", "private provider error", "/example/private-home", "rev-1",
           "private-token", "private-secret", "private-error", "private-host", "private-wallet", "never export")


class ComputerCase(OperationCase, BlackBoxCase):
    def snapshots(self) -> list[dict]:
        return [exchange.body["snapshot"] for exchange in self.relay.heartbeats()]

    def assert_private_fields_withheld(self) -> None:
        for exchange in self.relay.heartbeats():
            for text in PRIVATE:
                self.assertNotIn(text.encode(), exchange.raw, text)


class ProjectTests(ComputerCase):
    def vector_listing(self) -> dict:
        normalize = call_vector("projects-normalize")
        snapshot = normalize["args"][1]
        rows = [session_row(**{**row, "cmd": "claude"}) for row in snapshot["sessions"]]
        return listing(host=snapshot["host"], sessions=rows)

    async def test_projects_carry_local_folders_and_exact_memberships(self):
        normalize = call_vector("projects-normalize")
        self.hgs.script([{"argv": ["swarm", "get"], "stdout": normalize["args"][0]}]
                        + default_rules(self.vector_listing()))
        process = await self.launch()
        snapshot = self.snapshots()[-1]
        self.assertEqual(snapshot["mobile_projects"], normalize["returns"])
        memberships = call_vector("projects-memberships")["returns"]["sessions"]
        self.assertEqual([row.get("mobile_project_id") for row in snapshot["sessions"]],
                         [row.get("mobile_project_id") for row in memberships])
        self.assert_private_fields_withheld()
        process.stop()

    async def test_an_uninitialized_catalog_is_a_stable_local_fallback(self):
        process = await self.launch()
        await self.until(lambda: len(self.relay.heartbeats()) >= 2, what="two heartbeats", process=process)
        binding = hashlib.sha256((self.relay.url + "\0" + self.relay.token).encode()).hexdigest()
        expected = {**call_vector("projects-unavailable")["returns"],
                    "swarm_id": "local:" + hashlib.sha256(binding.encode()).hexdigest()}
        for snapshot in self.snapshots():
            self.assertEqual(snapshot["mobile_projects"], expected)
            self.assertNotIn("mobile_project_id", snapshot["sessions"][0])
        self.assertEqual(self.journal.metadata()["relay_binding"], binding)

    async def test_projects_are_read_every_fifteen_seconds_and_kept_when_a_read_fails(self):
        normalize = call_vector("projects-normalize")
        self.hgs.script([{"argv": ["swarm", "get"], "times": 1, "stdout": normalize["args"][0]},
                         {"argv": ["swarm", "get"], "exit": 1, "stderr": "synthetic outage\n"}]
                        + default_rules(self.vector_listing()))
        process = await self.launch()
        [first] = self.hgs.calls("swarm", "get")
        second = (await self.until(lambda: self.hgs.calls("swarm", "get")[1:], 25, "the next project read", process))[0]
        self.assertGreaterEqual(second["at"] - first["at"], 14.5)
        beats = len(self.relay.heartbeats())
        await self.until(lambda: len(self.relay.heartbeats()) > beats + 1, what="heartbeats after the failure",
                         process=process)
        stale = self.snapshots()[-1]
        self.assertEqual(stale["mobile_projects"], {**normalize["returns"], "stale": True})
        self.assertEqual(stale["sessions"][0]["mobile_project_id"], "logical")
        self.assertEqual(len(self.hgs.calls("swarm", "get")), 2)


def profile(identity="native-codex", **changes) -> dict:
    return {"id": identity, "provider": "codex", "label": "Synthetic account", "installed": True, "native": True,
            "is_default": True, "auth_revision": "rev-1", "home": "/example/private-home",
            "account_status": {"status": "ok", "checked_at": 800, "identity": {"email": "user@example.test"}},
            **changes}


def inspection(identity="native-codex", checked_at=None) -> dict:
    return {"id": identity, "provider": "codex", "home": "/example/private-home", "host": "private-host",
            "access_token": "private-token", "error": "private-error", "source": "Codex App Server", "status": "ok",
            "checked_at": time.time() if checked_at is None else checked_at,
            "identity": {"email": "user@example.test", "plan": "Pro", "secret": "private-secret"},
            "windows": [{"id": "primary", "label": "5 hours", "used_percent": 42.5, "window_minutes": 300,
                         "resets_at": 1500, "url": "https://private.example.test"}],
            "credits": {"balance": "3.25", "unlimited": False, "secret": "private-credit"},
            "balances": [{"kind": "Wallet", "balance": "8.00", "currency": "USD", "token": "private-wallet"}]}


class AccountTests(ComputerCase):
    def accounts(self) -> list[dict]:
        return [snapshot["mobile_accounts"] for snapshot in self.snapshots()]

    async def test_account_view_carries_only_public_fields(self):
        vector = copy.deepcopy(call_vector("account-cache-view"))
        # The vector's clock is fixed; a fresh observation keeps its usage current.
        observed = time.time() - 30
        vector["args"][0]["profiles"][0]["account_status"]["checked_at"] = observed
        expected = vector["returns"]
        expected["accounts"][0]["usage"]["checked_at"] = observed
        self.hgs.script([{"argv": ["account", "ls"], "stdout": vector["args"][0]}] + default_rules(listing()))
        before = time.time()
        process = await self.launch()
        view = self.accounts()[-1]
        self.assertGreaterEqual(view.pop("checked_at"), before - 1)
        expected.pop("checked_at")
        self.assertEqual(view, expected)
        self.assertIn("accounts_snapshot", self.snapshots()[-1]["mobile_capabilities"]["features"])
        # Nothing is installed, so nothing is inspected in the background.
        self.assertEqual(self.hgs.calls("account", "inspect"), [])
        self.assert_private_fields_withheld()
        process.stop()

    async def test_usage_is_refreshed_in_the_background_one_account_at_a_time(self):
        self.hgs.script([{"argv": ["account", "ls"], "stdout": {"profiles": [profile("one"), profile("two")]}},
                         {"argv": ["account", "inspect", "one"], "stdout": inspection("one"), "delay": 6},
                         {"argv": ["account", "inspect", "two"], "exit": 1, "stderr": "private-error\n"}]
                        + default_rules(listing()))
        process = await self.launch()
        [first] = await self.until(lambda: self.hgs.calls("account", "inspect"), what="a usage refresh", process=process)
        self.assertEqual(first["argv"], ["account", "inspect", "one"])
        beats = len(self.relay.heartbeats())
        await self.until(lambda: len(self.relay.heartbeats()) >= beats + 2, what="heartbeats during the refresh",
                         process=process)
        # Heartbeats never wait for a provider, and one refresh runs at a time.
        self.assertIsNone(self.hgs.finished(first["pid"]))
        self.assertEqual(len(self.hgs.calls("account", "inspect")), 1)
        self.assertIs(self.accounts()[-1]["accounts"][0]["usage"]["refreshing"], True)
        await self.until(lambda: len(self.hgs.calls("account", "inspect")) >= 2, 15, "the second refresh", process)
        self.assertIsNotNone(self.hgs.finished(first["pid"]))
        self.assertEqual(self.hgs.calls("account", "inspect")[1]["argv"], ["account", "inspect", "two"])
        await self.until(lambda: self.accounts()[-1]["accounts"][1]["usage"]["refresh_error"], 10,
                         "the failed refresh", process)
        one, two = self.accounts()[-1]["accounts"]
        self.assertEqual(one["usage"], {
            "status": "ok", "identity": {"email": "user@example.test", "plan": "Pro"},
            "windows": [{"used_percent": 42.5, "id": "primary", "label": "5 hours", "window_minutes": 300,
                         "resets_at": 1500}],
            "refreshing": False, "refresh_error": False, "source": "Codex App Server",
            "checked_at": one["usage"]["checked_at"], "credits": {"balance": "3.25", "unlimited": False},
            "balances": [{"balance": "8.00", "currency": "USD", "kind": "Wallet"}], "stale": False})
        # A failed refresh keeps the catalog's values and says so, without native text.
        self.assertEqual((two["usage"]["identity"], two["usage"]["checked_at"], two["usage"]["refreshing"],
                          two["usage"]["stale"]), ({"email": "user@example.test"}, 800, False, True))
        self.assert_private_fields_withheld()

    async def test_a_reset_time_that_is_not_an_ascii_timestamp_is_dropped(self):
        # Provider text is untrusted: a multi-byte reset time is dropped as
        # Python's pattern drops it, and the heartbeat carries on.
        windows = [{"id": "primary", "used_percent": 10, "resets_at": "\u20ac\u20ac\u20ac\u20ac-01-01T00:00:00Z"},
                   {"id": "weekly", "used_percent": 20, "resets_at": "2025-10-09T12:00:00Z"}]
        status = {"status": "ok", "checked_at": time.time(), "windows": windows}
        self.hgs.script([{"argv": ["account", "ls"],
                          "stdout": {"profiles": [profile(installed=False, account_status=status)]}}]
                        + default_rules(listing()))
        process = await self.launch()
        await self.until(lambda: len(self.relay.heartbeats()) >= 2, what="two heartbeats", process=process)
        [account] = self.accounts()[-1]["accounts"]
        self.assertEqual(account["usage"]["windows"], [
            {"id": "primary", "used_percent": 10},
            {"id": "weekly", "used_percent": 20, "resets_at": "2025-10-09T12:00:00Z"}])
        self.assertIsNone(process.returncode)

    async def test_an_unavailable_catalog_is_reported_without_native_text(self):
        self.hgs.script([{"argv": ["account", "ls"], "exit": 1, "stderr": "private-error\n"}] + default_rules(listing()))
        process = await self.launch()
        self.assertEqual(self.accounts()[-1], {"schema": 1, "available": False, "stale": True,
                                               "refresh_interval_seconds": 300, "accounts": []})
        self.assertIn("accounts_snapshot", self.snapshots()[-1]["mobile_capabilities"]["features"])
        self.assert_private_fields_withheld()
        process.stop()


def codex(index: int, **changes) -> dict:
    """A live tracked Codex row, eligible for the attention scan unless changed."""
    return session_row(**{"name": f"codex/{index}", "cmd": "codex", "run_id": f"run-{index}",
                          "conversation_id": f"conversation-{index}", **changes})


def questions(index: int, **changes) -> dict:
    row = codex(index)
    return {**row, "pending_questions": [
        {"run_id": row["run_id"], "conversation_id": row["conversation_id"], "question_id": f"question-{index}",
         "question_hash": f"hash-{index}", "answers": {"private": "never export"}, "can_answer": False},
        {"run_id": row["run_id"], "conversation_id": row["conversation_id"], "question_id": "submitted",
         "question_hash": "submitted-hash", "answer_delivery": {"status": "submitted"}}], **changes}


class AttentionTests(ComputerCase):
    async def test_two_live_codex_sessions_are_inspected_per_heartbeat(self):
        skipped = [codex(10, name="codex/stopped", state="stopped"), codex(11, name="codex/archived", state="archived"),
                   codex(12, name="codex/archive", archive_id="aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa"),
                   codex(13, name="codex/untracked", tracked=False), codex(14, name="kimi/example", cmd="kimi"),
                   codex(15, name="codex/paused", state="paused")]
        rules = [{"argv": ["inspect", f"codex/{index}"], "stdout": questions(index)} for index in range(5)]
        self.hgs.script(rules + default_rules(listing(sessions=[codex(index) for index in range(5)] + skipped)))
        process = await self.launch()

        def attention():
            return {row["name"]: row.get("mobile_attention") for row in self.snapshots()[-1]["sessions"]}

        await self.until(lambda: all(attention()[f"codex/{index}"] for index in range(5)), 15,
                         "attention on every eligible session", process)
        for index in range(5):
            self.assertEqual(attention()[f"codex/{index}"],
                             [{"question_id": f"question-{index}", "question_hash": f"hash-{index}"}])
        self.assertFalse(any(attention()[row["name"]] for row in skipped))
        calls = [call["argv"][1] for call in self.hgs.calls("inspect")]
        self.assertFalse(set(calls) - {f"codex/{index}" for index in range(5)}, calls)
        # Two per heartbeat, round robin: {0, 1}, {2, 3}, {4, 0}.
        pairs = [set(calls[index:index + 2]) for index in range(0, 6, 2)]
        self.assertEqual(pairs, [{"codex/0", "codex/1"}, {"codex/2", "codex/3"}, {"codex/4", "codex/0"}])
        self.assert_private_fields_withheld()
        self.assertNotIn(b"can_answer", self.relay.heartbeats()[-1].raw)
        # A changed identity or a slow inspection drops cached attention.
        self.hgs.prepend({"argv": ["inspect", "codex/1"], "stdout": questions(1, run_id="replacement-run")},
                         {"argv": ["inspect", "codex/2"], "stdout": questions(2), "delay": 2})
        await self.until(lambda: not attention()["codex/1"] and not attention()["codex/2"], 20,
                         "dropped attention", process)
        self.assertEqual(attention()["codex/0"], [{"question_id": "question-0", "question_hash": "hash-0"}])


if __name__ == "__main__":
    unittest.main()
