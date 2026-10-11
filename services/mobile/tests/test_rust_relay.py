"""Released HTTP contracts replayed against the Rust process, when selected.

The Python store is used only as the independent fixture/legacy data writer.
The requests under test always cross the Rust server's real loopback socket.
"""
import os
import uuid
from pathlib import Path
import tempfile
import unittest

from aiohttp import ClientSession
from zerus_mobile.server import Config
from zerus_mobile.store import Store
import test_relay
from relay_fixture import start_relay


class HttpClient:
    def __init__(self, url):
        self.session = ClientSession(base_url=url)

    async def request(self, *args, **kwargs):
        return await self.session.request(*args, **kwargs)

    async def get(self, path, **kwargs):
        return await self.request('GET', path, **kwargs)

    async def post(self, path, **kwargs):
        return await self.request('POST', path, **kwargs)

    async def close(self):
        await self.session.close()


@unittest.skipUnless(os.environ.get('ZERUS_RELAY_BINARY'), 'Rust contract target is explicit')
class RustHttpContracts(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.store = Store(Path(self.temp.name) / 'relay.sqlite3')
        self.workspace = self.store.workspace('Example')
        self.node = self.store.node(self.workspace, 'Example computer')
        self.phone = self.store.pair(self.store.invite(self.workspace)['pair_code'], 'Example phone')
        self.device = self.store.authenticate(self.phone['device_token'], 'devices')
        self.legacy = None
        if self._testMethodName == 'test_existing_python_receipt_identity':
            request_id = str(uuid.uuid4())
            self.legacy = dict(request_id=request_id, computer_id=self.node['node_id'],
                operation='answer', session='synthetic', payload=dict(request_id=request_id,
                    expected_run_id='run', expected_conversation_id='conversation',
                    question_id='q', expected_question_hash='hash',
                    answers={'float': 1e-5, 'large': 2**80, 'text': 'Unicode 😀',
                        'object': {'$serde_json::private::Number': '123'}}))
            self.store.submit(self.device, self.legacy, 500)
            self.store.claim(self.store.authenticate(self.node['node_token'], 'nodes'))
            self.store.result(self.store.authenticate(self.node['node_token'], 'nodes'), request_id,
                {'state': 'completed', 'result': {'value': 2**80, 'text': '😀',
                    'object': {'$serde_json::private::Number': '123'}}, 'error': None})
        self.config = Config(background=False)
        if self._testMethodName == 'test_queue_limit_retention_tombstones':
            self.config.max_queue = 1
        self.runner, url = await start_relay(self.store, self.config)
        self.client = HttpClient(url)

    async def asyncTearDown(self):
        await self.client.close()
        await self.runner.cleanup()
        self.store.close()
        self.temp.cleanup()

    async def test_existing_python_receipt_identity(self):
        before = self.store.db.execute('SELECT body_hash FROM requests WHERE id=?',
            (self.legacy['request_id'],)).fetchone()[0]
        result = await self.call('POST', '/v1/requests', self.legacy, expected=202)
        self.assertEqual(result['state'], 'completed')
        self.assertEqual(result['result'], {'value': 2**80, 'text': '😀',
            'object': {'$serde_json::private::Number': '123'}})
        self.assertEqual((await self.call('GET', '/v1/node/requests', node=True))['requests'], [])
        self.assertEqual(before, self.store.db.execute('SELECT body_hash FROM requests WHERE id=?',
            (self.legacy['request_id'],)).fetchone()[0])

    async def test_presence_alerts_http_contract(self):
        # The database was created by the Python store, so this also covers the in-place upgrade.
        self.assertIn("presence_alerts", (await self.call("GET", "/v1/capabilities"))["features"])
        row = {"name": "codex/project/alert", "run_id": "r", "conversation_id": "c", "phase": "working", "activity": "busy"}

        async def heartbeat(session, idle):
            await self.call("POST", "/v1/node/heartbeat", {"snapshot": {"sessions": [session]},
                "presence": {"idle_seconds": idle, "locked": False, "locked_seconds": None}}, node=True)

        await heartbeat(row, 1.0)
        question = {**row, "phase": "input", "attention_id": "q1", "attention_signature": "a" * 64}
        await heartbeat(question, 1.0)
        self.assertEqual([e["kind"] for e in (await self.call("GET", "/v1/events"))["events"]], ["attention"])
        await heartbeat(question, 600.0)  # Same snapshot; only presence changes.
        events = (await self.call("GET", "/v1/events"))["events"]
        self.assertEqual([e["kind"] for e in events], ["attention", "alert"])
        self.assertNotIn("alert_kind", events[0])
        self.assertEqual(set(events[1]), {"id", "computer_id", "session", "kind", "alert_kind", "created_at"})
        self.assertEqual((events[1]["alert_kind"], events[1]["session"], events[1]["computer_id"]),
                         ("attention", "codex/project/alert", self.node["node_id"]))

    headers = test_relay.RelayTests.headers
    call = test_relay.RelayTests.call
    command = test_relay.RelayTests.command


# These are the existing assertions, not a second implementation of the API.
# Provider-mocked tests are Python-unit tests; Rust provider contracts live in Rust.
for name in (
    'health_capabilities_and_local_only_administration',
    'pair_once_and_expiry_and_hash_storage', 'pair_rate_limit',
    'failed_authentication_rate_limit', 'workspace_and_role_isolation',
    'durable_claim_duplicate_result_and_owner', 'single_claim_and_request_validation',
    'expiry_restart_and_late_result', 'queue_limit_retention_tombstones',
    'claim_timeout_and_revoke', 'device_logout_revokes_token_and_cancels_queue',
    'old_read_history_does_not_consume_mutation_tombstones', 'long_poll_rechecks_revocation',
    'heartbeat_events_baseline_transition_and_privacy', 'body_cap_nonfinite_and_poll_bounds',
    'async_question_fingerprints_add_change_remove', 'push_retired_unifiedpush_and_unavailable_fcm',
    'mark_read_is_exact_and_capability_gated', 'set_mobile_delivery_request_validation', 'mark_read_is_read_class_and_pruned_like_reads',
    'attachment_read_is_exact_gated_and_short_lived',
):
    setattr(RustHttpContracts, 'test_' + name, getattr(test_relay.RelayTests, 'test_' + name))
