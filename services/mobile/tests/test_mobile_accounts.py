"""Account privacy, credential invalidation and bounded background scheduling."""
import asyncio
import copy
import json
import os
from pathlib import Path
import tempfile
import unittest

from zerus_mobile.accounts import AccountCache, catalog, usage
from zerus_mobile.connector import Connector, ConnectorError


def profile(identity="native-codex", revision="auth-one"):
    return {"id": identity, "provider": "codex", "label": "Synthetic account", "installed": True,
            "native": True, "is_default": True, "auth_revision": revision, "home": "/private/account",
            "account_status": {"status": "ok", "checked_at": 800, "identity": {"email": "user@example.test"}}}


def inspection(identity="native-codex"):
    return {"id": identity, "provider": "codex", "home": "/private/account", "host": "private-host",
            "access_token": "private-token", "error": "private-error", "source": "Codex App Server",
            "status": "ok", "checked_at": 1000, "identity": {"email": "user@example.test", "plan": "Pro", "secret": "private-secret"},
            "windows": [{"id": "primary", "label": "5 hours", "used_percent": 42.5,
                         "window_minutes": 300, "resets_at": 1500, "url": "https://private.example.test"}],
            "credits": {"balance": "3.25", "unlimited": False, "secret": "private-credit"},
            "balances": [{"kind": "Wallet", "balance": "8.00", "currency": "USD", "token": "private-wallet"}]}


class ProjectionTests(unittest.TestCase):
    def test_native_opencode_account_and_models_keep_configuration_private(self):
        raw={'id':'native-opencode','provider':'opencode','label':'Native OpenCode','installed':True,'native':True,
             'home':'/private/config','model_options':[{'value':'ollama/qwen3-coder:30b','label':'Local model','baseURL':'https://private.example.test','api_key':'private'}],
             'account_status':{'status':'configured','source':'OpenCode','checked_at':1000,'credentials':'private'}}
        entry=catalog({'profiles':[raw]})['native-opencode']
        self.assertEqual(entry['public']['model_options'],[{'value':'ollama/qwen3-coder:30b','label':'Local model','provider':'ollama'}])
        self.assertEqual(entry['usage']['status'],'configured')
        self.assertEqual(entry['usage']['source'],'OpenCode')
        self.assertNotIn('private',json.dumps(entry['public']|{'usage':entry['usage']}))

    def test_only_allowlisted_fields_leave_native_projection(self):
        projected = usage(inspection())
        encoded = json.dumps(projected)
        self.assertNotIn("private", encoded)
        self.assertEqual(projected["identity"], {"email": "user@example.test", "plan": "Pro"})
        self.assertEqual(projected["windows"][0]["used_percent"], 42.5)
        self.assertEqual(projected["credits"], {"balance": "3.25", "unlimited": False})
        self.assertEqual(projected["balances"], [{"kind": "Wallet", "balance": "8.00", "currency": "USD"}])
        self.assertNotIn("removed_profiles", catalog({"profiles": [profile()], "removed_profiles": [profile("hidden")]}))

    def test_invalid_numbers_resets_sources_and_auth_status_are_not_coerced(self):
        raw = inspection()
        raw.update(checked_at=float("nan"), source="https://private.example.test", auth_status="private-token")
        raw["windows"] = [{"used_percent": value, "resets_at": 0} for value in (True, -1, float("inf"), float("nan"), "42")]
        raw["windows"] += [{"used_percent": 23, "resets_at": "2026-10-09T12:34:56Z"},
                           {"used_percent": 24, "resets_at": float("inf"), "window_minutes": "300"}]
        projected = usage(raw)
        self.assertNotIn("checked_at", projected)
        self.assertNotIn("source", projected)
        self.assertNotIn("auth_status", projected)
        self.assertEqual(len(projected["windows"]), 2)
        self.assertEqual(projected["windows"][0]["resets_at"], "2026-10-09T12:34:56Z")
        self.assertNotIn("resets_at", projected["windows"][1])
        self.assertNotIn("window_minutes", projected["windows"][1])

    def test_catalog_bound_identity_and_duplicate_validation(self):
        for raw in ({}, {"profiles": [profile()] * 101}, {"profiles": [profile(), profile()]},
                    {"profiles": [profile("--refresh")]}, {"profiles": [profile("../../private")]}):
            with self.assertRaises(ValueError):
                catalog(raw)
        projected = usage({"identity": {"name": "a" * 5000}, "windows": inspection()["windows"] * 1000})
        self.assertEqual(len(projected["identity"]["name"]), 256)
        self.assertEqual(len(projected["windows"]), 16)

    def test_malformed_nested_values_and_huge_numbers_are_safe(self):
        projected = usage({"status": [], "auth_status": {}, "source": [], "checked_at": 10**1000,
                           "identity": [], "windows": [{"used_percent": 10**1000},
                               {"used_percent": 1, "resets_at": "2026-99-99T12:34:56Z"}],
                           "balances": [{"currency": [], "balance": "1"}]})
        self.assertEqual(projected["status"], "unavailable")
        self.assertNotIn("checked_at", projected)
        self.assertEqual(projected["windows"], [{"used_percent": 1}])
        with self.assertRaises(ValueError):
            catalog({"profiles": [{**profile(), "provider": []}]})

    def test_actual_ascii_expanded_catalog_bytes_are_bounded(self):
        rows = []
        for index in range(100):
            row = profile("account" + str(index))
            row["account_status"] = {"status": "ok", "identity": {"name": "界" * 256},
                "windows": [{"id": "界" * 160, "label": "界" * 160, "used_percent": 42}] * 16}
            rows.append(row)
        with self.assertRaises(ValueError):
            catalog({"profiles": rows})


class CacheTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.now = 0
        self.raw_catalog = {"profiles": [profile()]}
        self.calls = []
        async def native(argv, **kwargs):
            self.calls.append((argv, kwargs))
            return copy.deepcopy(self.raw_catalog) if argv == ["account", "ls"] else inspection(argv[2])
        self.cache = AccountCache(native, clock=lambda: self.now, wall=lambda: 1000 + self.now)

    async def asyncTearDown(self):
        await self.cache.close()

    async def test_refresh_is_single_flight_nonblocking_and_preserves_native_age(self):
        entered, release = asyncio.Event(), asyncio.Event()
        original = self.cache.native
        async def blocked(argv, **kwargs):
            if argv[1] == "inspect":
                entered.set()
                await release.wait()
            return await original(argv, **kwargs)
        self.cache.native = blocked
        first = await asyncio.wait_for(self.cache.update(), .2)
        self.assertEqual(first["checked_at"], 1000)
        self.assertEqual(first["accounts"][0]["usage"]["checked_at"], 800)
        await entered.wait()
        for _ in range(10):
            view = await asyncio.wait_for(self.cache.update(), .2)
            self.assertTrue(view["accounts"][0]["usage"]["refreshing"])
        self.now = 31
        await self.cache.update()  # Local catalog rotation must not lose an in-flight result.
        release.set()
        await self.cache.task
        self.assertEqual(self.cache.view()["accounts"][0]["usage"]["checked_at"], 1000)
        self.assertEqual(sum(argv[1] == "inspect" for argv, _ in self.calls), 1)
        self.now = 301
        self.assertTrue(self.cache.view()["accounts"][0]["usage"]["stale"])

    async def test_credential_change_and_removal_discard_old_usage_and_late_result(self):
        entered, release = asyncio.Event(), asyncio.Event()
        async def native(argv, **kwargs):
            if argv[1] == "ls":
                return copy.deepcopy(self.raw_catalog)
            entered.set()
            await release.wait()
            return inspection()
        self.cache.native = native
        await self.cache.update()
        await entered.wait()
        self.raw_catalog = {"profiles": [{**profile(revision="changed-auth"), "account_status": None}]}
        self.now = 31
        changed = await self.cache.update()
        self.assertEqual(changed["accounts"][0]["usage"]["identity"], {})
        self.assertNotIn("checked_at", changed["accounts"][0]["usage"])
        release.set()
        await self.cache.task
        self.assertEqual(self.cache.view()["accounts"][0]["usage"]["identity"], {})
        self.raw_catalog = {"profiles": []}
        self.now = 62
        self.assertEqual((await self.cache.update())["accounts"], [])

    async def test_fair_rotation_refresh_interval_and_missing_provider(self):
        self.raw_catalog = {"profiles": [profile("one"), profile("two"), {**profile("not-installed"), "installed": False}]}
        for now in (0, 1, 2, 299, 300, 301):
            self.now = now
            await self.cache.update()
            if self.cache.task:
                await self.cache.task
        self.assertEqual([argv[2] for argv, _ in self.calls if argv[1] == "inspect"], ["one", "two", "one", "two"])
        self.assertTrue(all(kwargs["timeout"] == 25 for argv, kwargs in self.calls if argv[1] == "inspect"))

    async def test_dsh_missing_auth_revision_signout_invalidates_usage(self):
        row = {**profile("native-dsh"), "provider": "dsh"}
        row.pop("auth_revision")
        self.raw_catalog = {"profiles": [row]}
        async def native(argv, **kwargs):
            if argv[1] == "ls":
                return copy.deepcopy(self.raw_catalog)
            return {**inspection("native-dsh"), "provider": "dsh"}
        self.cache.native = native
        await self.cache.update()
        await self.cache.task
        self.assertTrue(self.cache.view()["accounts"][0]["usage"]["windows"])
        self.now = 31
        self.raw_catalog["profiles"][0]["account_status"] = None
        view = await self.cache.update()
        self.assertEqual(view["accounts"][0]["usage"]["identity"], {})
        self.assertEqual(view["accounts"][0]["usage"]["windows"], [])
        self.assertNotIn("checked_at", view["accounts"][0]["usage"])

    async def test_failures_retain_stale_values_without_errors_and_shutdown_cancels(self):
        await self.cache.update()
        await self.cache.task
        async def failed(argv, **kwargs):
            raise RuntimeError("private-secret-and-path")
        self.cache.native = failed
        self.now = 31
        view = await self.cache.update()
        self.assertTrue(view["stale"])
        self.assertEqual(view["accounts"][0]["usage"]["checked_at"], 1000)
        self.assertNotIn("private", json.dumps(view))
        self.cache.next_catalog = 1000
        self.cache.stale = False
        self.now = 301
        await self.cache.update()
        await self.cache.task
        self.assertTrue(self.cache.view()["accounts"][0]["usage"]["refresh_error"])
        unavailable = AccountCache(failed)
        self.assertFalse((await unavailable.update())["available"])
        await unavailable.close()
        entered, cancelled = asyncio.Event(), asyncio.Event()
        async def blocking(argv, **kwargs):
            if argv[1] == "ls":
                return self.raw_catalog
            entered.set()
            try:
                await asyncio.Event().wait()
            finally:
                cancelled.set()
        pending = AccountCache(blocking)
        await pending.update()
        await entered.wait()
        await pending.close()
        self.assertTrue(cancelled.is_set())

    async def test_connector_snapshot_exposes_capability_without_waiting_for_provider(self):
        with tempfile.TemporaryDirectory() as directory:
            connector = Connector({"server_url": "https://relay.example.invalid", "node_token": "fixture-token", "state_dir": directory})
            pending = asyncio.Event()
            async def native(argv, **kwargs):
                if argv[0] == "ls":
                    return {"sessions": []}
                if argv == ["account", "ls"]:
                    return self.raw_catalog
                if argv[:2] == ["account", "inspect"]:
                    await pending.wait()
                    return inspection()
                raise ConnectorError("fixture unavailable")
            connector.native = native
            try:
                snapshot = await asyncio.wait_for(connector.snapshot(), .2)
                self.assertIn("accounts_snapshot", snapshot["mobile_capabilities"]["features"])
                self.assertEqual(snapshot["mobile_accounts"]["accounts"][0]["id"], "native-codex")
            finally:
                await connector.accounts.close()
                connector.journal.close()

    async def test_shutdown_reaps_only_the_owned_account_inspection_child(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            hgs = root / "fixture-hgs"
            hgs.write_text("#!/usr/bin/env python3\nimport json,os,sys,time\nfrom pathlib import Path\n"
                "if sys.argv[1:] == ['account','ls']:\n"
                " print(json.dumps({'profiles':[{'id':'native-codex','provider':'codex','label':'Synthetic','installed':True}]}))\n"
                "else:\n"
                " Path(__file__).with_name('owned-pid').write_text(str(os.getpid()))\n"
                " time.sleep(60)\n")
            hgs.chmod(0o700)
            connector = Connector({"server_url": "https://relay.example.invalid", "node_token": "fixture-token",
                                   "state_dir": str(root / "journal"), "hgs_path": str(hgs)})
            try:
                await connector.accounts.update()
                for _ in range(100):
                    if (root / "owned-pid").exists():
                        break
                    await asyncio.sleep(.01)
                pid = int((root / "owned-pid").read_text())
                await connector.accounts.close()
                with self.assertRaises(ProcessLookupError):
                    os.kill(pid, 0)
            finally:
                await connector.accounts.close()
                connector.journal.close()
