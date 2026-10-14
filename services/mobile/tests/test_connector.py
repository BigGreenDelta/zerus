"""Connector safety tests use synthetic subprocesses, never resident hgs agents."""
import asyncio
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch
import uuid

from aiohttp import web, ClientSession, ClientTimeout

from zerus_mobile.connector import Connector, ConnectorError, RelayError, checked_url


FAKE_HGS = r'''#!/usr/bin/env python3
import json, os, sys, time
from pathlib import Path
root = Path(__file__).parent
payload = sys.stdin.read()
with (root / 'calls.jsonl').open('a') as log:
    keys = ('HGS_RUN_ID', 'HGS_ACCOUNT_ID', 'HGS_SESSION', 'TMUX', 'CODEX_HOME',
            'HGS_CONFIG_DIR', 'HGS_STATE_DIR', 'HGS_PEERS')
    log.write(json.dumps({'argv': sys.argv[1:], 'stdin': payload,
                         'env': {k: os.environ[k] for k in keys if k in os.environ}}) + '\n')
mode = (root / 'mode').read_text() if (root / 'mode').exists() else ''
if mode == 'sleep': time.sleep(10)
if mode == 'flood':
    sys.stdout.write('x' * 200000)
    sys.stdout.flush()
    time.sleep(10)
if mode == 'stderr-flood':
    sys.stderr.write('x' * 200000)
    sys.stderr.flush()
    time.sleep(10)
if mode == 'secret-failure':
    print('private-native-secret', file=sys.stderr)
    sys.exit(8)
if mode == 'bad-json':
    print('private-native-secret')
    sys.exit(0)
if sys.argv[1] == 'ls':
    print(json.dumps({'host': 'synthetic', 'hgs_version': '9.8.7', 'sessions': [{'name': 'codex/example'},
                                                     {'name': 'codex/a;echo pwned'}]}))
else:
    print(json.dumps({'native': True, 'argv': sys.argv[1:],
                      'payload': json.loads(payload) if payload else None}))
'''


class ConnectorTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="zerus-connector-test-")
        self.root = Path(self.temp.name)
        self.hgs = self.root / "hgs"
        self.hgs.write_text(FAKE_HGS)
        self.hgs.chmod(0o700)
        self.config = {"server_url": "https://relay.example.invalid", "node_token": "fixture-token",
                       "hgs_path": str(self.hgs), "state_dir": str(self.root / "journal")}
        self.connector = Connector(self.config, subprocess_timeout=0.3)
        await self.connector.snapshot()

    async def asyncTearDown(self):
        self.connector.journal.close()
        self.temp.cleanup()

    async def test_process_observation_clock_does_not_change_snapshot_work(self):
        expected = dict(count=1, started_at=100, command='build --wait')
        for clock in [200, 201]:
            row = dict(name='codex/example', process_work={**expected, 'sampled_at': clock})
            self.hgs.write_text(FAKE_HGS.replace("{'name': 'codex/example'}", repr(row)))
            snapshot = await self.connector.snapshot()
            self.assertEqual(snapshot['sessions'][0]['process_work'], expected)

    async def test_ended_launches_never_leave_the_computer(self):
        ended = [{'launch_id': '00000000-0000-4000-8000-000000000001', 'name': 'claude/example',
                  'code': 1, 'output': 'hgs: claude: Access is denied.'}]
        self.hgs.write_text(FAKE_HGS.replace("'hgs_version': '9.8.7', ", "'hgs_version': '9.8.7', 'ended_launches': %r, " % ended))
        snapshot = await self.connector.snapshot()
        self.assertNotIn('ended_launches', snapshot)
        self.assertNotIn('Access is denied', json.dumps(snapshot))

    async def test_desktop_presence_travels_beside_the_snapshot_with_old_relay_fallback(self):
        presence = {'idle_seconds': 4.0, 'locked': False, 'locked_seconds': None}
        self.hgs.write_text(FAKE_HGS.replace("'hgs_version': '9.8.7', ", "'hgs_version': '9.8.7', 'desktop_presence': %r, " % presence))
        snapshot = await self.connector.snapshot()
        self.assertNotIn('desktop_presence', snapshot)
        self.assertEqual(self.connector.presence, presence)
        sent = []

        async def http(client, method, path, body=None):
            sent.append(body)
            if 'presence' in body and len(sent) == 1:
                raise RelayError(400)  # A relay predating the additive field.
            return {'ok': True}

        self.connector.http = http
        for _ in range(2):
            await self.connector.post_with_presence(None, '/v1/node/heartbeat', {'snapshot': snapshot}, self.connector.presence)
        self.assertEqual([sorted(body) for body in sent], [['presence', 'snapshot'], ['snapshot'], ['snapshot']])
        self.connector.presence_retry_at = 0.0  # The periodic probe found a newer relay.
        await self.connector.post_with_presence(None, '/v1/node/heartbeat', {'snapshot': snapshot}, None)
        self.assertEqual(sent[-1], {'snapshot': snapshot, 'presence': None})
        self.hgs.write_text(FAKE_HGS)  # The desktop GUI stopped reporting presence.
        await self.connector.snapshot()
        self.assertIsNone(self.connector.presence)

    async def test_cli_version_survives_enrichment_and_legacy_snapshots_remain_supported(self):
        snapshot = await self.connector.snapshot()
        self.assertEqual(snapshot['hgs_version'], '9.8.7')
        self.hgs.write_text(FAKE_HGS.replace("'hgs_version': '9.8.7', ", ""))
        snapshot = await self.connector.snapshot()
        self.assertNotIn('hgs_version', snapshot)
        self.assertEqual(len(snapshot['sessions']), 2)

    def request(self, operation="send", session="codex/example", **payload):
        request_id = str(uuid.uuid4())
        return {"request_id": request_id, "operation": operation, "session": session,
                "payload": payload if operation == "inspect" else {"request_id": request_id, "expected_run_id": "exact-run",
                            "expected_conversation_id": "exact-conversation",
                            "text": "literal $(touch ignored); 'quotes'\nЮникод", **payload}}

    def calls(self):
        return [json.loads(line) for line in (self.root / "calls.jsonl").read_text().splitlines()]

    async def test_fixed_argv_and_exact_stdin_for_all_allowlisted_operations(self):
        self.assertEqual(self.calls()[0]["argv"], ["ls", "--json", "--local"])
        for operation in ("inspect", "send", "answer", "interrupt"):
            request = self.request(operation)
            response = await self.connector.execute(request)
            self.assertEqual(response["state"], "completed")
            self.assertTrue(response["result"]["native"])
            call = self.calls()[-1]
            self.assertEqual(call["argv"], [operation, "codex/example"]
                             + ([] if operation == "inspect" else ["--json"]))
            self.assertEqual(call["stdin"], "" if operation == "inspect" else
                             json.dumps(request["payload"], ensure_ascii=False))

    async def test_scrubs_native_identity_but_preserves_machine_paths(self):
        environment = {"HGS_RUN_ID": "calling-run", "HGS_ACCOUNT_ID": "calling-account",
                       "HGS_SESSION": "calling-session", "TMUX": "calling-socket",
                       "HGS_CONFIG_DIR": "/synthetic/config", "HGS_STATE_DIR": "/synthetic/state",
                       "HGS_PEERS": "synthetic-peer", "CODEX_HOME": "/calling/account-home"}
        with patch.dict(os.environ, environment):
            await self.connector.execute(self.request("inspect"))
        env = self.calls()[-1]["env"]
        for key in ("HGS_RUN_ID", "HGS_ACCOUNT_ID", "HGS_SESSION", "TMUX", "CODEX_HOME"):
            self.assertNotIn(key, env)
        for key in ("HGS_CONFIG_DIR", "HGS_STATE_DIR", "HGS_PEERS"):
            self.assertEqual(env[key], environment[key])

    async def test_rejects_remote_alias_options_controls_and_unknown_session(self):
        before = len(self.calls())
        for session in ("@peer", "-n", "--json", "codex/example@peer", "codex/example\n",
                        "codex/example\0", "codex/missing"):
            result = await self.connector.execute(self.request(session=session))
            self.assertEqual(result["state"], "failed")
        for operation in ("kill", "pause", "sh", "__state", "send;exit"):
            result = await self.connector.execute(self.request(operation))
            self.assertEqual(result["state"], "failed")
        self.assertEqual(len(self.calls()), before)

    async def test_shell_syntax_in_exact_known_name_is_literal_argv(self):
        result = await self.connector.execute(self.request(session="codex/a;echo pwned"))
        self.assertEqual(result["state"], "completed")
        self.assertEqual(self.calls()[-1]["argv"], ["send", "codex/a;echo pwned", "--json"])

    async def test_rejects_invalid_request_and_payload_identity_before_spawn(self):
        before = len(self.calls())
        for request_id in ("../../inspect", "not-a-uuid", 42, str(uuid.uuid4()).upper()):
            request = self.request()
            request["request_id"] = request_id
            with self.assertRaises(ConnectorError):
                await self.connector.execute(request)
        for changes in ({"request_id": str(uuid.uuid4())}, {"expected_run_id": ""},
                        {"expected_run_id": None}):
            result = await self.connector.execute(self.request(**changes))
            self.assertEqual(result["state"], "failed")
        self.assertEqual(len(self.calls()), before)

    async def test_completed_duplicate_replays_native_result_without_execution(self):
        request = self.request()
        result = await self.connector.execute(request)
        count = len(self.calls())
        self.connector.journal.delivered(request["request_id"])
        self.assertEqual(await self.connector.execute(request), result)
        self.assertEqual(len(self.calls()), count)
        self.assertEqual(self.connector.journal.pending(), [(request["request_id"], result)])
        changed = dict(request, operation="interrupt")
        with self.assertRaises(ConnectorError):
            await self.connector.execute(changed)
        self.assertEqual(len(self.calls()), count)

    async def test_timeout_is_uncertain_for_mutation_and_never_retried(self):
        (self.root / "mode").write_text("sleep")
        request = self.request()
        result = await asyncio.wait_for(self.connector.execute(request), 2)
        self.assertEqual(result["state"], "uncertain")
        self.assertEqual(result["error"], "native command timed out")
        count = len(self.calls())
        (self.root / "mode").unlink()
        self.assertEqual(await self.connector.execute(request), result)
        self.assertEqual(len(self.calls()), count)

    async def test_timeout_read_fails_without_marking_mutation(self):
        (self.root / "mode").write_text("sleep")
        result = await self.connector.execute(self.request("inspect"))
        self.assertEqual(result["state"], "failed")

    async def test_output_bounds_and_safe_native_errors(self):
        self.connector.max_bytes = 1024
        for mode in ("flood", "stderr-flood", "secret-failure", "bad-json"):
            (self.root / "mode").write_text(mode)
            result = await asyncio.wait_for(self.connector.execute(self.request()), 2)
            self.assertEqual(result["state"], "uncertain")
            self.assertNotIn("private-native-secret", json.dumps(result))
        (self.root / "mode").unlink()

    async def test_cancelled_mutation_is_durable_and_uncertain(self):
        (self.root / "mode").write_text("sleep")
        request = self.request()
        count = len(self.calls())
        task = asyncio.create_task(self.connector.execute(request))
        for _ in range(100):
            if len(self.calls()) > count:
                break
            await asyncio.sleep(0.01)
        task.cancel()
        with self.assertRaises(asyncio.CancelledError):
            await task
        response = self.connector.journal.replay(request["request_id"])
        self.assertEqual(response["state"], "uncertain")
        self.assertEqual(len(self.connector.journal.pending()), 1)

    async def test_abrupt_process_exit_after_native_execution_cannot_repeat_write(self):
        request = self.request()
        config = dict(self.config, state_dir=str(self.root / "crash-journal"))
        code = """import asyncio,json,os,sys
from zerus_mobile.connector import Connector
c=Connector(json.loads(sys.argv[1]))
c.sessions={'codex/example'}
c.journal.finish=lambda *a: os._exit(23)
asyncio.run(c.execute(json.loads(sys.argv[2])))
"""
        completed = await asyncio.to_thread(subprocess.run,
            [sys.executable, "-c", code, json.dumps(config), json.dumps(request)],
            capture_output=True, timeout=5)
        self.assertEqual(completed.returncode, 23, completed.stderr)
        count = len(self.calls())
        recovered = Connector(config)
        try:
            recovered.sessions = {"codex/example"}
            result = await recovered.execute(request)
            self.assertEqual(result["state"], "uncertain")
            self.assertEqual(len(self.calls()), count)
            self.assertEqual(recovered.journal.pending(), [(request["request_id"], result)])
        finally:
            recovered.journal.close()

    async def test_journal_excludes_second_connector(self):
        with self.assertRaises(ConnectorError):
            Connector(self.config)
        self.assertEqual((self.root / "journal").stat().st_mode & 0o777, 0o700)
        self.assertEqual((self.root / "journal/journal.sqlite3").stat().st_mode & 0o777, 0o600)

    async def test_outbox_survives_restart_and_retries_identical_result(self):
        request = self.request()
        result = await self.connector.execute(request)
        bodies = []

        async def receive(req):
            bodies.append(await req.json())
            self.assertEqual(req.headers["Authorization"], "Bearer fixture-token")
            return web.json_response({"ok": True}, status=503 if len(bodies) == 1 else 200)

        app = web.Application()
        app.router.add_post("/v1/node/requests/{id}/result", receive)
        runner = web.AppRunner(app)
        await runner.setup()
        site = web.TCPSite(runner, "127.0.0.1", 0)
        await site.start()
        url = "http://127.0.0.1:" + str(site._server.sockets[0].getsockname()[1])
        self.connector.server_url = url
        async with ClientSession(timeout=ClientTimeout(total=2)) as client:
            with self.assertRaises(ConnectorError):
                await self.connector.flush(client)
            self.connector.journal.close()
            self.connector = Connector(self.config)
            self.connector.server_url = url
            await self.connector.flush(client)
        await runner.cleanup()
        self.assertEqual(bodies, [result, result])
        self.assertEqual(self.connector.journal.pending(), [])
        self.assertEqual(sum(call["argv"][0] == "send" for call in self.calls()), 1)

    async def test_redirect_does_not_forward_token_or_repeat_result_elsewhere(self):
        seen = []

        async def destination(req):
            seen.append(req.headers.get("Authorization"))
            return web.json_response({})

        async def redirect(req):
            raise web.HTTPFound("/destination")

        app = web.Application()
        app.router.add_get("/redirect", redirect)
        app.router.add_get("/destination", destination)
        runner = web.AppRunner(app)
        await runner.setup()
        site = web.TCPSite(runner, "127.0.0.1", 0)
        await site.start()
        self.connector.server_url = "http://127.0.0.1:" + str(site._server.sockets[0].getsockname()[1])
        async with ClientSession(timeout=ClientTimeout(total=2)) as client:
            with self.assertRaises(ConnectorError):
                await self.connector.http(client, "GET", "/redirect")
        await runner.cleanup()
        self.assertEqual(seen, [])

    async def test_result_conflict_is_retired_and_does_not_block_subsequent_commands(self):
        request = self.request()
        first = await self.connector.execute(request)
        uploads = []

        async def http(client, method, path, body=None):
            uploads.append(body)
            if len(uploads) == 1:
                raise RelayError(409)
            return {"ok": True}

        self.connector.http = http
        await self.connector.flush(None)
        self.assertEqual(self.connector.journal.pending(), [])
        second = await self.connector.execute(self.request("inspect"))
        await self.connector.flush(None)
        self.assertEqual(uploads, [first, second])
        count = len(self.calls())
        self.assertEqual(await self.connector.execute(request), first)
        self.assertEqual(len(self.calls()), count)

    async def test_result_too_large_is_failed_read_or_uncertain_write(self):
        async def large(*args, **kwargs):
            return {"text": "x" * (1024 * 1024)}

        self.connector.native = large
        for operation, state in (("inspect", "failed"), ("send", "uncertain")):
            result = await self.connector.execute(self.request(operation))
            self.assertEqual(result["state"], state)
            self.assertEqual(result["result"], None)
            self.assertIn("relay size limit", result["error"])

    async def test_journal_refuses_config_switch_and_unsafe_files_without_chmod(self):
        self.connector.journal.close()
        for changed in ({"server_url": "https://other.example.invalid"}, {"node_token": "other-token"}):
            with self.assertRaises(ConnectorError):
                Connector(dict(self.config, **changed))
        self.connector = Connector(self.config)
        public = self.root / "shared"
        public.mkdir(mode=0o755)
        with self.assertRaises(ConnectorError):
            Connector(dict(self.config, state_dir=str(public)))
        self.assertEqual(public.stat().st_mode & 0o777, 0o755)
        target = self.root / "outside"
        target.write_text("fixture private file")
        target.chmod(0o600)
        for name in ("connector.lock", "journal.sqlite3", "journal.sqlite3-journal"):
            directory = self.root / ("unsafe-" + name)
            directory.mkdir(mode=0o700)
            (directory / name).symlink_to(target)
            with self.assertRaises(ConnectorError):
                Connector(dict(self.config, state_dir=str(directory)))
            self.assertEqual(target.read_text(), "fixture private file")

    async def test_attention_round_robin_budget_and_exact_identity_cache(self):
        rows = [{"name": f"codex/{i}", "cmd": "codex", "tracked": True,
                 "runtime_state": "live", "process_state": "running",
                 "run_id": f"run-{i}", "conversation_id": f"conversation-{i}"} for i in range(5)]
        skipped = [dict(rows[0], name="codex/stopped", state="stopped"),
                   dict(rows[0], name="codex/archived", state="archived"),
                   dict(rows[0], name="codex/archive-compatibility", archive_id="aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa"),
                   dict(rows[0], name="codex/untracked", tracked=False),
                   dict(rows[0], name="kimi/example", cmd="kimi")]
        calls = []
        mismatch = set()

        async def native(argv, **kwargs):
            calls.append(argv)
            i = int(argv[1].split("/")[1])
            detail = dict(rows[i])
            detail["pending_questions"] = [{"run_id": rows[i]["run_id"],
                "conversation_id": rows[i]["conversation_id"], "question_id": f"question-{i}",
                "question_hash": f"hash-{i}", "answers": {"private": "never export"},
                "can_answer": False},
                {"run_id": rows[i]["run_id"], "conversation_id": rows[i]["conversation_id"],
                 "question_id": "submitted", "question_hash": "submitted-hash",
                 "answer_delivery": {"status": "submitted"}}]
            if i in mismatch:
                detail["run_id"] = "other-run"
            return detail

        self.connector.native = native
        for expected in ([0, 1], [2, 3], [4, 0]):
            start = len(calls)
            snapshot = {"sessions": [dict(row) for row in rows + skipped]}
            await self.connector.enrich_attention(snapshot)
            self.assertEqual(calls[start:], [["inspect", f"codex/{i}"] for i in expected])
            for row in snapshot["sessions"]:
                if "mobile_attention" in row:
                    i = int(row["name"].split("/")[1])
                    self.assertEqual(row["mobile_attention"], [{"question_id": f"question-{i}",
                                                              "question_hash": f"hash-{i}"}])
                self.assertNotIn("can_answer", row)
        self.assertEqual(len(self.connector.attention_cache), 5)
        # Reused/disappeared names invalidate before another inspection is due.
        rows[4]["run_id"] = "new-run"
        mismatch.add(0)
        self.connector.attention_cursor = 0
        snapshot = {"sessions": [dict(rows[0]), dict(rows[4])]}
        await self.connector.enrich_attention(snapshot)
        self.assertNotIn("mobile_attention", snapshot["sessions"][0])
        self.assertNotIn(("codex/4", "run-4", "conversation-4"), self.connector.attention_cache)
        self.assertLessEqual(len(self.connector.attention_cache), 1)

    def test_https_and_explicit_loopback_only_http(self):
        self.assertEqual(checked_url("https://relay.example.invalid/"), "https://relay.example.invalid")
        for url in ("http://127.0.0.1", "http://localhost", "http://[::1]"):
            with self.assertRaises(ConnectorError):
                checked_url(url)
            self.assertEqual(checked_url(url, True), url)
        for url in ("http://example.invalid", "http://127.0.0.2", "https://user:password@example.invalid",
                    "https://example.invalid?token=x", "https://example.invalid/#x",
                    "https://example.invalid\n", "ftp://localhost", "https://example.invalid:bad"):
            with self.assertRaises(ConnectorError):
                checked_url(url, True)


if __name__ == "__main__":
    unittest.main()
