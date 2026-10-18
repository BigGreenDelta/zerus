"""Released HTTP contracts against the Rust relay process on PostgreSQL.

Every test serves its own relay_fixture database, seeded only through the
zerus-relay command line and /v1/pair, and its requests cross the server's real
loopback socket. Expiry and retention are observed over HTTP with second-scale
relay settings. Stored effects that HTTP cannot show, time travel and the
Python-written receipt fixture are Rust contracts; services/mobile/relay/README.md
maps every former case to its test.
"""
import asyncio
import io
import json
from pathlib import Path
import time
import unittest
import uuid

from aiohttp import ClientSession

import relay_fixture

OPERATIONS = set(json.loads((Path(__file__).resolve().parents[1] / 'vectors/operations.json')
                            .read_text())['sets']['OPERATIONS'])
# Operations only the Rust connector executes; the vectors freeze Python's set.
RUST_ONLY = {row['operation'] for row in json.loads((Path(__file__).resolve().parent / 'rust-only-operations.json')
                                                   .read_text())['operations']}
# Background maintenance every 0.1 s, so expired rows leave while a test waits.
MAINTAINED = dict(background=True, maintenance_interval=0.1)
TOMBSTONE = 'request history expired; delivery must not be retried'


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


class RustHttpContracts(unittest.IsolatedAsyncioTestCase):
    # Relay settings by test; seconds where the default would be minutes or days.
    SETTINGS = {
        'test_expiry_and_restart_keep_claims': dict(queue_ttl=1),
        'test_queue_limit_retention_tombstones': dict(max_queue=1, retention=1, **MAINTAINED),
        'test_claim_timeout_late_result_and_revoke': dict(claim_ttl=1),
        'test_old_read_history_does_not_consume_mutation_tombstones': dict(retention=1, **MAINTAINED),
        'test_mark_read_is_read_class_and_pruned_like_reads': dict(retention=1, **MAINTAINED),
        'test_attachment_read_is_exact_gated_and_short_lived': MAINTAINED,
    }

    async def asyncSetUp(self):
        # Also the fixture of test_computer_file_read.NativeFileHttp.
        settings = RustHttpContracts.SETTINGS.get(self._testMethodName, {})
        self.relay = await relay_fixture.start_relay(rust_only=True, workspace='Example', **settings)
        try:
            self.workspace, self.node = self.relay.workspace, self.relay.node
            self.phone = await self.relay.pair('Example phone', self.relay.invitation['pair_code'])
        except BaseException:
            await self.relay.cleanup()
            raise
        self.client = HttpClient(self.relay.url)

    async def asyncTearDown(self):
        try:
            await self.client.close()
        finally:
            await self.relay.cleanup()

    def headers(self, node=False):
        return {'Authorization': 'Bearer ' + (self.node['node_token'] if node else self.phone['device_token'])}

    async def call(self, method, route, body=None, node=False, expected=200, headers=None):
        response = await self.client.request(method, route, json=body, headers=headers or self.headers(node))
        self.assertEqual(response.status, expected, await response.text())
        return await response.json()

    def command(self, operation='inspect'):
        value = {'request_id': str(uuid.uuid4()), 'computer_id': self.node['node_id'], 'operation': operation,
                 'session': 'codex/project/deep/tag', 'payload': {}}
        if operation != 'inspect':
            value['payload'] = {'request_id': value['request_id'], 'expected_run_id': 'example-run',
                                'expected_conversation_id': 'example-conversation', 'text': 'Hello'}
        return value

    async def receipt(self, value):
        return await self.call('GET', '/v1/requests/' + value['request_id'])

    async def complete(self, value, result=None):
        """Deliver the next queued command, which must be this one, and complete it."""
        delivered = (await self.call('GET', '/v1/node/requests', node=True))['requests']
        self.assertEqual([command['request_id'] for command in delivered], [value['request_id']])
        await self.call('POST', '/v1/node/requests/' + value['request_id'] + '/result',
                        {'state': 'completed', 'result': result or {'ok': True}, 'error': None}, node=True)

    async def eventually(self, probe, timeout=20, interval=0.05):
        """The first true value of probe(), which a relay deadline makes true."""
        deadline = time.monotonic() + timeout
        while not (value := await probe()):
            if time.monotonic() > deadline:
                self.fail('the relay did not reach the expected state in time')
            await asyncio.sleep(interval)
        return value

    async def state(self, value, state):
        return (await self.receipt(value))['state'] == state

    async def pruned(self, value):
        response = await self.client.get('/v1/requests/' + value['request_id'], headers=self.headers())
        response.release()
        return response.status == 404

    async def restart(self):
        await self.client.close()
        await self.relay.restart()
        self.client = HttpClient(self.relay.url)

    async def test_health_capabilities_and_local_only_administration(self):
        self.assertEqual((await self.call('GET', '/healthz'))['protocol_version'], 1)
        self.assertEqual((await self.call('GET', '/v1/capabilities'))['push_providers'], [])
        await self.call('POST', '/v1/admin', expected=404)
        await self.call('GET', '/v1/computers', headers={'Authorization': 'Bearer wrong'}, expected=401)

    async def test_pair_once_and_opaque_token(self):
        invitation = await self.relay.invite()
        pair = await self.call('POST', '/v1/pair', {'code': invitation['pair_code'], 'device_name': 'Other'})
        self.assertEqual(pair['workspace_id'], self.workspace)
        self.assertEqual(pair['workspace_name'], 'Example')
        await self.call('POST', '/v1/pair', {'code': invitation['pair_code'], 'device_name': 'Again'}, expected=401)
        self.assertEqual(len(pair['device_token']), 43)
        headers = {'Authorization': 'Bearer ' + pair['device_token']}
        self.assertEqual([row['id'] for row in (await self.call('GET', '/v1/computers', headers=headers))['computers']],
                         [self.node['node_id']])

    async def test_pair_rate_limit(self):
        # Ten attempts a minute from one client; the fixture's pairing was the first.
        for _ in range(9):
            await self.call('POST', '/v1/pair', {'code': 'bad', 'device_name': 'Example'}, expected=401)
        await self.call('POST', '/v1/pair', {'code': 'bad', 'device_name': 'Example'}, expected=429)

    async def test_failed_authentication_rate_limit(self):
        for _ in range(20):
            await self.call('GET', '/v1/computers', headers={'Authorization': 'Bearer invalid'}, expected=401)
        await self.call('GET', '/v1/computers', headers={'Authorization': 'Bearer invalid'}, expected=429)

    async def test_workspace_and_role_isolation(self):
        foreign = await self.relay.cli('provision', '--name', 'Other', '--computer-name', 'Foreign')
        computers = await self.call('GET', '/v1/computers')
        self.assertEqual([row['id'] for row in computers['computers']], [self.node['node_id']])
        value = self.command()
        value['computer_id'] = foreign['node_id']
        await self.call('POST', '/v1/requests', value, expected=404)
        await self.call('GET', '/v1/computers', node=True, expected=401)
        await self.call('GET', '/v1/node/requests', expected=401)

    async def test_durable_claim_duplicate_result_and_owner(self):
        value = self.command('send')
        first = await self.call('POST', '/v1/requests', value, expected=202)
        self.assertEqual(first['state'], 'queued')
        self.assertEqual(await self.call('POST', '/v1/requests', value, expected=202), first)
        changed = {**value, 'session': 'different'}
        await self.call('POST', '/v1/requests', changed, expected=409)
        delivered = await self.call('GET', '/v1/node/requests', node=True)
        self.assertEqual(delivered['requests'][0]['session'], value['session'])
        self.assertEqual((await self.receipt(value))['state'], 'claimed')
        self.assertEqual((await self.call('GET', '/v1/node/requests', node=True))['requests'], [])
        self.assertEqual((await self.call('POST', '/v1/requests', value, expected=202))['state'], 'claimed')
        other_phone = await self.relay.pair('Other phone')
        await self.call('GET', '/v1/requests/' + value['request_id'],
                        headers={'Authorization': 'Bearer ' + other_phone['device_token']}, expected=404)
        other_node = await self.relay.add_node('Other node')
        result = {'state': 'completed', 'result': {'status': 'sent'}, 'error': None}
        await self.call('POST', '/v1/node/requests/' + value['request_id'] + '/result', result,
                        headers={'Authorization': 'Bearer ' + other_node['node_token']}, expected=404)
        await self.call('POST', '/v1/node/requests/' + value['request_id'] + '/result', result, node=True)
        await self.call('POST', '/v1/node/requests/' + value['request_id'] + '/result', result, node=True)
        self.assertEqual((await self.receipt(value))['result'], result['result'])

    async def test_single_claim_and_request_validation(self):
        for _ in range(2):
            await self.call('POST', '/v1/requests', self.command(), expected=202)
        self.assertEqual(len((await self.call('GET', '/v1/node/requests', node=True))['requests']), 1)
        invalid = self.command('send')
        invalid['payload']['request_id'] = str(uuid.uuid4())
        await self.call('POST', '/v1/requests', invalid, expected=400)
        invalid = self.command()
        invalid['operation'] = 'shell'
        await self.call('POST', '/v1/requests', invalid, expected=400)
        invalid = self.command('send')
        invalid['payload']['command'] = 'whoami'
        await self.call('POST', '/v1/requests', invalid, expected=400)

    async def test_expiry_and_restart_keep_claims(self):
        queued, claimed = self.command(), self.command('send')
        await self.call('POST', '/v1/requests', claimed, expected=202)
        delivered = (await self.call('GET', '/v1/node/requests', node=True))['requests']
        self.assertEqual([command['request_id'] for command in delivered], [claimed['request_id']])
        await self.call('POST', '/v1/requests', queued, expected=202)
        await self.restart()
        # A PostgreSQL worker never retires a claim at startup; it keeps its deadline.
        self.assertEqual((await self.receipt(claimed))['state'], 'claimed')
        await self.eventually(lambda: self.state(queued, 'failed'))  # queue_ttl is 1 s
        self.assertEqual((await self.call('GET', '/v1/node/requests', node=True))['requests'], [])
        # The restarted relay never delivers the claim again; its node still reports the outcome.
        result = {'state': 'completed', 'result': {'status': 'sent'}, 'error': None}
        await self.call('POST', '/v1/node/requests/' + claimed['request_id'] + '/result', result, node=True)
        self.assertEqual((await self.receipt(claimed))['result'], result['result'])

    async def test_queue_limit_retention_tombstones(self):
        value = self.command('send')
        await self.call('POST', '/v1/requests', value, expected=202)
        await self.call('POST', '/v1/requests', self.command(), expected=429)
        await self.complete(value, {'status': 'sent'})

        async def tombstone():
            receipt = await self.call('POST', '/v1/requests', value, expected=202)
            return receipt if receipt['result'] is None else None

        # After the retention (1 s) a mutation keeps only its identity and state.
        receipt = await self.eventually(tombstone)
        self.assertEqual((receipt['state'], receipt['error']), ('completed', TOMBSTONE))
        await self.call('POST', '/v1/requests', {**value, 'session': 'changed'}, expected=409)
        self.assertEqual((await self.call('GET', '/v1/node/requests', node=True))['requests'], [])
        # The completed command no longer occupies the one-request queue.
        await self.call('POST', '/v1/requests', self.command(), expected=202)

    async def test_claim_timeout_late_result_and_revoke(self):
        value = self.command('send')
        await self.call('POST', '/v1/requests', value, expected=202)
        await self.call('GET', '/v1/node/requests', node=True)
        await self.eventually(lambda: self.state(value, 'uncertain'))  # claim_ttl is 1 s
        # A late success cannot silently resolve an uncertain delivery.
        await self.call('POST', '/v1/node/requests/' + value['request_id'] + '/result',
                        {'state': 'completed', 'result': None, 'error': None}, node=True, expected=409)
        self.assertEqual((await self.receipt(value))['state'], 'uncertain')
        self.assertEqual((await self.call('GET', '/v1/node/requests', node=True))['requests'], [])
        queued = self.command()
        await self.call('POST', '/v1/requests', queued, expected=202)
        await self.relay.revoke('node', self.node['node_id'])
        self.assertEqual((await self.receipt(queued))['state'], 'failed')
        await self.call('GET', '/v1/node/requests', node=True, expected=401)
        await self.relay.revoke('device', self.phone['device_id'])
        await self.call('GET', '/v1/computers', expected=401)

    async def test_device_logout_revokes_token_and_cancels_queue(self):
        command = self.command('send')
        await self.call('POST', '/v1/requests', command, expected=202)
        await self.call('DELETE', '/v1/device')
        await self.call('GET', '/v1/computers', expected=401)
        # Cancelled with the device: its computer never receives the command.
        self.assertEqual((await self.call('GET', '/v1/node/requests', node=True))['requests'], [])

    async def test_old_read_history_does_not_consume_mutation_tombstones(self):
        read, mutation = self.command(), self.command('send')
        # The mutation completes first, so it is past the retention when the read is.
        for value in (mutation, read):
            await self.call('POST', '/v1/requests', value, expected=202)
            await self.complete(value)
        # Reads leave the relay after the retention (1 s here, at most an hour).
        await self.eventually(lambda: self.pruned(read))
        receipt = await self.call('POST', '/v1/requests', mutation, expected=202)
        self.assertEqual((receipt['state'], receipt['error']), ('completed', TOMBSTONE))
        self.assertEqual((await self.call('GET', '/v1/node/requests', node=True))['requests'], [])
        # Repeating an expired inspect is safe: it cannot mutate native agents.
        self.assertEqual((await self.call('POST', '/v1/requests', read, expected=202))['state'], 'queued')
        delivered = (await self.call('GET', '/v1/node/requests', node=True))['requests']
        self.assertEqual([command['request_id'] for command in delivered], [read['request_id']])

    async def test_mark_read_is_read_class_and_pruned_like_reads(self):
        snapshot = {'sessions': [], 'mobile_capabilities': {'protocol_version': 1, 'operations': ['mark_read'], 'features': []}}
        await self.call('POST', '/v1/node/heartbeat', {'snapshot': snapshot}, node=True)
        command = self.command('mark_read')
        command['payload'] = {'run_id': 'example-run', 'conversation_id': 'example-conversation',
                              'reply_id': '7:100.5', 'attention_signature': None}
        await self.call('POST', '/v1/requests', command, expected=202)
        await self.complete(command)
        await self.eventually(lambda: self.pruned(command))
        # Marks are idempotent and forward-only, so a pruned identity may be repeated.
        self.assertEqual((await self.call('POST', '/v1/requests', command, expected=202))['state'], 'queued')

    async def test_attachment_read_is_exact_gated_and_short_lived(self):
        self.assertIn('attachment_read', (await self.call('GET', '/v1/capabilities'))['operations'])
        command = self.command('attachment_read')
        command['payload'] = {'request_id': command['request_id'], 'expected_run_id': 'example-run',
                              'expected_conversation_id': 'example-conversation',
                              'message_id': '00000000-0000-4000-8000-000000000001', 'index': 0, 'offset': 0, 'length': 524288}
        await self.call('POST', '/v1/requests', command, expected=409)
        snapshot = {'sessions': [], 'mobile_capabilities': {'protocol_version': 1, 'operations': ['attachment_read'], 'features': []}}
        await self.call('POST', '/v1/node/heartbeat', {'snapshot': snapshot}, node=True)
        for change in ({'path': '/private/file.png'}, {'message_id': '../../private/file.png'}, {'index': 8},
                       {'length': 524289}, {'offset': 10 * 1024 * 1024}, {'expected_conversation_id': ''}, {'text': 'Hello'}):
            invalid = self.command('attachment_read')
            invalid['payload'] = {**command['payload'], 'request_id': invalid['request_id'], **change}
            await self.call('POST', '/v1/requests', invalid, expected=400)
        self.assertEqual((await self.call('POST', '/v1/requests', command, expected=202))['state'], 'queued')
        delivered = await self.call('GET', '/v1/node/requests', node=True)
        self.assertEqual(delivered['requests'][0]['payload'], command['payload'])
        data = 'A' * 699052  # A maximum 512 KiB range in base64 fits the ordinary result bound.
        await self.call('POST', '/v1/node/requests/' + command['request_id'] + '/result',
                        {'state': 'completed', 'result': {'request_id': command['request_id'], 'data_base64': data}, 'error': None}, node=True)
        self.assertEqual(len((await self.receipt(command))['result']['data_base64']), 699052)
        read = time.monotonic()

        async def requeued():
            return (await self.call('POST', '/v1/requests', command, expected=202))['state'] == 'queued'

        # Reading the receipt starts a 5 s grace; repeating the request answers
        # from the receipt without renewing it, then, once the bytes have left
        # the relay, queues the identity anew.
        self.assertFalse(await requeued())
        await self.eventually(requeued, interval=0.25)
        self.assertGreater(time.monotonic() - read, 4)

    async def test_long_poll_rechecks_revocation(self):
        started = time.monotonic()
        task = asyncio.create_task(self.client.get('/v1/node/requests?wait=10', headers=self.headers(node=True)))
        await asyncio.sleep(0.25)
        # Still waiting: an answer before the revocation would prove nothing.
        self.assertFalse(task.done())
        await self.relay.revoke('node', self.node['node_id'])
        response = await task
        self.assertEqual(response.status, 401)
        # The revocation woke the poll; it did not wait for its deadline.
        self.assertLess(time.monotonic() - started, 9)

    async def test_heartbeat_events_baseline_transition_and_privacy(self):
        session = {'name': 'codex/project/a/b', 'run_id': 'r1', 'conversation_id': 'c1', 'phase': 'working', 'activity': 'busy', 'prompt': 'Private text'}

        async def heartbeat(row):
            await self.call('POST', '/v1/node/heartbeat', {'snapshot': {'sessions': [row]}}, node=True)
        await heartbeat(session)
        self.assertEqual((await self.call('GET', '/v1/events'))['events'], [])
        question = {**session, 'phase': 'input', 'attention_id': 'question-one'}
        await heartbeat(question)
        await heartbeat(question)
        await heartbeat({**question, 'attention_id': 'question-two'})
        await heartbeat({**session, 'phase': 'idle', 'activity': 'idle'})
        result = await self.call('GET', '/v1/events')
        self.assertEqual([event['kind'] for event in result['events'] if event['kind'] != 'alert'], ['attention', 'attention', 'completed'])
        self.assertNotIn('Private text', json.dumps(result))
        self.assertEqual((await self.call('GET', '/v1/events?after=' + str(result['cursor'])))['events'], [])
        computers = (await self.call('GET', '/v1/computers'))['computers']
        self.assertTrue(computers[0]['online'])
        self.assertEqual(computers[0]['snapshot']['sessions'][0]['prompt'], 'Private text')
        other = await self.relay.cli('provision', '--name', 'Other', '--computer-name', 'Other computer')
        other_phone = await self.relay.pair('Other', other['pair_code'])
        self.assertEqual((await self.call('GET', '/v1/events', headers={'Authorization': 'Bearer ' + other_phone['device_token']}))['events'], [])

    async def test_body_cap_nonfinite_and_poll_bounds(self):
        response = await self.client.post('/v1/node/heartbeat', data=b'{"snapshot":NaN}', headers={**self.headers(True), 'Content-Type': 'application/json'})
        self.assertEqual(response.status, 400)
        # The relay rejects the declared length without reading the body and may
        # close the socket; a client still uploading can then see EPIPE instead of 413.
        # Expect: 100-continue reads that rejection before any body byte is written.
        response = await self.client.post('/v1/node/heartbeat', data=io.BytesIO(b'x' * (1024 * 1024 + 1)), expect100=True, headers={**self.headers(True), 'Content-Type': 'application/json'})
        self.assertEqual(response.status, 413)
        await self.call('GET', '/v1/events?wait=26', expected=400)
        await self.call('GET', '/v1/node/requests?wait=NaN', node=True, expected=400)

    async def test_async_question_fingerprints_add_change_remove(self):
        session = {'name': 'codex/project/deep/tag', 'run_id': 'r', 'conversation_id': 'c', 'phase': 'working', 'activity': 'busy'}

        async def send(pending):
            await self.call('POST', '/v1/node/heartbeat', {'snapshot': {'sessions': [{**session, 'mobile_attention': pending}]}}, node=True)
        first = {'question_id': 'q1', 'question_hash': 'h1'}
        second = {'question_id': 'q1', 'question_hash': 'h2'}
        await send([first])  # First snapshot is silent even if pending.
        await send([first])
        self.assertEqual((await self.call('GET', '/v1/events'))['events'], [])
        await send([])  # Removals are silent.
        await send([first])
        await send([first])  # An unchanged heartbeat is silent.
        await send([second])
        await send([])
        # An optional Codex question is an ordinary item with one more boolean.
        optional = {'question_id': 'codex-async:call:0', 'question_hash': 'h3'}
        await send([optional])
        await send([{**optional, 'optional': True}])  # An upgraded connector marks it once.
        await send([{**optional, 'optional': True}])
        await send([])
        self.assertEqual([e['kind'] for e in (await self.call('GET', '/v1/events'))['events'] if e['kind'] != 'alert'], ['attention'] * 4)

    async def test_push_retired_unifiedpush_and_unavailable_fcm(self):
        self.assertEqual((await self.call('GET', '/v1/capabilities'))['push_providers'], [])
        await self.call('POST', '/v1/push', {'provider': 'unifiedpush', 'endpoint': 'https://push.example.com/token'}, expected=400)
        await self.call('POST', '/v1/push', {'provider': 'fcm', 'token': 'example'}, expected=503)

    async def test_mark_read_is_exact_and_capability_gated(self):
        value = self.command('mark_read')
        value['payload'] = {'run_id': 'example-run', 'conversation_id': 'example-conversation',
                            'reply_id': '7:100.5', 'attention_signature': 'a' * 64}
        await self.call('POST', '/v1/requests', value, expected=409)
        snapshot = {'sessions': [], 'mobile_capabilities': {'protocol_version': 1, 'operations': ['mark_read'], 'features': []}}
        await self.call('POST', '/v1/node/heartbeat', {'snapshot': snapshot}, node=True)
        self.assertEqual((await self.call('POST', '/v1/requests', value, expected=202))['state'], 'queued')
        for change in ({'reply_id': 7}, {'attention_signature': 'A' * 64}, {'attention_signature': 'a' * 63},
                       {'run_id': ''}, {'request_id': value['request_id']}):
            invalid = self.command('mark_read')
            invalid['payload'] = {**value['payload'], **change}
            await self.call('POST', '/v1/requests', invalid, expected=400)
        delivered = await self.call('GET', '/v1/node/requests', node=True)
        self.assertEqual(delivered['requests'][0]['payload'], value['payload'])
        # The released operation set, as the golden vectors froze it from the
        # Python connector, and those only the Rust connector runs.
        self.assertEqual(set((await self.call('GET', '/v1/capabilities'))['operations']), OPERATIONS | RUST_ONLY)

    async def test_set_mobile_delivery_request_validation(self):
        value = {'request_id': str(uuid.uuid4()), 'computer_id': self.node['node_id'],
                 'operation': 'set_mobile_delivery', 'session': '', 'payload': {'value': 'away_10'}}
        self.assertIn('set_mobile_delivery', (await self.call('GET', '/v1/capabilities'))['operations'])
        await self.call('POST', '/v1/requests', value, expected=409)
        snapshot = {'sessions': [], 'mobile_capabilities': {'protocol_version': 1, 'operations': ['set_mobile_delivery'], 'features': []}}
        await self.call('POST', '/v1/node/heartbeat', {'snapshot': snapshot}, node=True)
        self.assertEqual((await self.call('POST', '/v1/requests', value, expected=202))['state'], 'queued')
        delivered = (await self.call('GET', '/v1/node/requests', node=True))['requests'][0]
        self.assertEqual((delivered['operation'], delivered['session'], delivered['payload']),
                         ('set_mobile_delivery', '', {'value': 'away_10'}))
        for change in ({'session': 'codex/project/deep/tag'}, {'payload': {'value': 'later'}}, {'payload': {}},
                       {'payload': {'value': 'away', 'request_id': value['request_id']}}):
            await self.call('POST', '/v1/requests', {**value, 'request_id': str(uuid.uuid4()), **change}, expected=400)

    async def test_presence_alerts_http_contract(self):
        self.assertIn('presence_alerts', (await self.call('GET', '/v1/capabilities'))['features'])
        row = {'name': 'codex/project/alert', 'run_id': 'r', 'conversation_id': 'c', 'phase': 'working', 'activity': 'busy'}

        async def heartbeat(session, idle):
            await self.call('POST', '/v1/node/heartbeat', {'snapshot': {'sessions': [session]},
                'presence': {'idle_seconds': idle, 'locked': False, 'locked_seconds': None}}, node=True)

        await heartbeat(row, 1.0)
        question = {**row, 'phase': 'input', 'attention_id': 'q1', 'attention_signature': 'a' * 64}
        await heartbeat(question, 1.0)
        self.assertEqual([e['kind'] for e in (await self.call('GET', '/v1/events'))['events']], ['attention'])
        await heartbeat(question, 600.0)  # Same snapshot; only presence changes.
        events = (await self.call('GET', '/v1/events'))['events']
        self.assertEqual([e['kind'] for e in events], ['attention', 'alert'])
        self.assertNotIn('alert_kind', events[0])
        self.assertEqual(set(events[1]), {'id', 'computer_id', 'session', 'kind', 'alert_kind', 'created_at'})
        self.assertEqual((events[1]['alert_kind'], events[1]['session'], events[1]['computer_id']),
                         ('attention', 'codex/project/alert', self.node['node_id']))


if __name__ == '__main__':
    unittest.main()
