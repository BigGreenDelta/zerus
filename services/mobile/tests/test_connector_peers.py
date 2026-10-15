"""Synthetic local/peer identities and connector journal/routing isolation."""
import asyncio
import json
from pathlib import Path
import tempfile
import time
import unittest
from unittest.mock import AsyncMock, patch
import uuid

from zerus_mobile.connector import Connector, ConnectorError, RelayError
from zerus_mobile.fleet import route_id

LOCAL = '00000000-0000-4000-8000-000000000001'
PEER = '00000000-0000-4000-8000-000000000002'
OTHER = '00000000-0000-4000-8000-000000000003'
COMPUTER = '00000000-0000-4000-8000-000000000004'


class PeerConnectorTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='zerus-peer-connector-')
        self.root = Path(self.temp.name)
        fixture = self.root / 'hgs'
        fixture.write_text(Path(__file__).with_name('fixture_peer_hgs.py').read_text())
        fixture.chmod(0o700)
        self.inventory = {'schema': 1, 'local': {'machine_id': LOCAL, 'name': 'Arch'},
            'peers': [{'via': 'configured-mac', 'machine_id': PEER, 'name': 'Mac', 'online': True, 'error': None}]}
        self.write_inventory()
        self.config = {'server_url': 'https://relay.example.invalid', 'node_token': 'fixture-token',
                       'hgs_path': str(fixture), 'state_dir': str(self.root / 'journal'), 'poll_interval': 1}
        self.connector = Connector(self.config)
        self.identity = route_id('configured-mac', PEER)
        await self.connector.snapshot()
        await self.connector.fleet.discover()
        self.peer = self.connector.fleet.contexts[self.identity]
        await self.peer.snapshot()
        # Each first snapshot starts one background account usage refresh. Let
        # them finish, so a test's native call log contains only its own calls.
        await asyncio.gather(*(cache.task for cache in (self.connector.accounts, self.peer.accounts)
                               if cache.task is not None))

    async def asyncTearDown(self):
        await self.connector.accounts.close()
        await self.connector.fleet.close()
        self.connector.journal.close()
        self.temp.cleanup()

    def write_inventory(self):
        (self.root / 'inventory.json').write_text(json.dumps(self.inventory))

    def calls(self):
        return [json.loads(row) for row in (self.root / 'calls.jsonl').read_text().splitlines()]

    def request(self, operation='inspect', *, peer=True):
        identity = str(uuid.uuid4())
        run = 'peer-run' if peer else 'local-run'
        envelope = {'schema': 1, 'route_id': self.identity if peer else None,
                    'computer_id': COMPUTER, 'machine_id': PEER if peer else LOCAL}
        payload = {} if operation == 'inspect' else {'request_id': identity, 'expected_run_id': run,
            'expected_conversation_id': 'peer-conversation' if peer else 'local-conversation'}
        if operation == 'send': payload['text'] = 'Literal fixture message'
        if operation == 'history': payload.update(limit=20)
        if operation in {'catalog', 'launch'}:
            payload = {'request_id': identity}
            if operation == 'launch': payload.update(agent='codex', directory='/example/fixture', tag='new', account_id='account-' + run)
        return {'request_id': identity, 'operation': operation, 'session': '' if operation in {'catalog', 'launch'} else 'codex/example',
                'payload': payload, 'gateway_route': envelope}

    async def test_same_session_names_have_separate_guarded_contexts(self):
        local = await self.connector.execute(self.request(peer=False))
        peer = await self.connector.execute(self.request())
        self.assertEqual(local['result']['machine'], LOCAL)
        self.assertEqual(peer['result']['machine'], PEER)
        self.assertIs(self.peer.journal, self.connector.journal)
        self.assertIsNot(self.peer.accounts, self.connector.accounts)
        self.assertEqual(self.connector.accounts.view()['accounts'][0]['id'], 'account-local-run')
        self.assertEqual(self.peer.accounts.view()['accounts'][0]['id'], 'account-peer-run')
        guards = [call for call in self.calls() if call['argv'] == ['swarm', '__mobile-peer-local', '--json']]
        self.assertTrue(guards)
        self.assertEqual(guards[-1]['payload']['target_machine_id'], LOCAL)

    async def test_publication_strips_aliases_and_peer_of_peer_inventory(self):
        snapshot = await self.connector.snapshot()
        body = self.connector.fleet.heartbeat(snapshot)
        self.connector.fleet.confirm_manifest(body)
        with patch.object(self.connector, 'http', AsyncMock(return_value={})) as http:
            await self.connector.fleet.publish(None)
        published = json.dumps([body, http.call_args.args[3]])
        self.assertNotIn('configured-mac', published)
        self.assertNotIn('never-export-third-peer', published)
        self.assertEqual(body['peers'][0]['route_id'], self.identity)
        self.assertEqual(http.call_args.args[2], f'/v1/node/peers/{self.identity}/heartbeat')
        forged = self.request()
        forged['gateway_route']['route_id'] = route_id('third-peer', OTHER)
        forged['gateway_route']['machine_id'] = OTHER
        self.assertEqual((await self.connector.execute(forged))['state'], 'failed')

    async def test_removed_rebound_and_forged_alias_fail_before_native_mutation(self):
        self.inventory['peers'][0]['machine_id'] = OTHER
        self.write_inventory()
        request = self.request('send')
        response = await self.connector.execute(request)
        self.assertIn(response['state'], {'failed', 'uncertain'})
        self.assertFalse((self.root / 'native_effects.jsonl').exists())
        forged = self.request('send')
        forged['gateway_route']['via'] = 'configured-mac'
        self.assertEqual((await self.connector.execute(forged))['state'], 'failed')
        self.inventory['peers'] = []
        self.write_inventory()
        self.assertIn((await self.connector.execute(self.request('send')))['state'], {'failed', 'uncertain'})
        self.assertFalse((self.root / 'native_effects.jsonl').exists())

    async def test_bound_local_uuid_guard_prevents_execution_after_identity_reset(self):
        self.inventory['local']['machine_id'] = OTHER
        self.write_inventory()
        result = await self.connector.execute(self.request('send', peer=False))
        self.assertEqual(result['state'], 'uncertain')
        self.assertIsNone(result['result'])

    async def test_route_is_in_journal_identity_and_duplicate_survives_withdrawal(self):
        request = self.request('send')
        first = await self.connector.execute(request)
        self.assertEqual(first['state'], 'completed')
        self.inventory['peers'] = []
        self.write_inventory()
        self.assertEqual(await self.connector.execute(request), first)
        changed = json.loads(json.dumps(request))
        changed['gateway_route']['route_id'] = None
        with self.assertRaisesRegex(ConnectorError, 'conflicts'):
            await self.connector.execute(changed)

    def go_offline(self, machine=PEER):
        self.inventory['peers'][0].update(machine_id=machine, online=False, error='unavailable',
                                          identity='last_verified')
        self.write_inventory()

    async def test_last_verified_offline_peer_is_advertised_but_never_dispatched(self):
        context = self.peer
        self.go_offline()
        await self.connector.fleet.discover()
        # The same alias/UUID keeps its route and context; only availability changes.
        self.assertIs(self.connector.fleet.contexts[self.identity], context)
        body = self.connector.fleet.heartbeat(await self.connector.snapshot())
        self.assertEqual(body['peers'], [{'route_id': self.identity, 'machine_id': PEER, 'name': 'Mac', 'online': False}])
        self.assertNotIn('configured-mac', json.dumps(body))
        self.connector.fleet.confirm_manifest(body)
        self.assertEqual(self.connector.fleet.published, set())
        before = len(self.calls())
        with patch.object(self.connector, 'http', AsyncMock(return_value={})) as http:
            await self.connector.fleet.publish(None)
            http.assert_not_awaited()
        for operation in ('inspect', 'send', 'history', 'catalog', 'launch'):
            self.assertEqual((await self.connector.execute(self.request(operation)))['state'], 'failed', operation)
        with self.assertRaisesRegex(ConnectorError, 'unavailable or changed'):
            await context.native(['ls', '--json', '--local'])
        self.assertEqual(len(self.calls()), before)
        self.assertFalse((self.root / 'native_effects.jsonl').exists())
        # Reconnection under the same verified identity restores the same route.
        self.inventory['peers'][0].update(online=True, error=None, identity='verified')
        self.write_inventory()
        await self.connector.fleet.discover()
        self.assertIs(self.connector.fleet.contexts[self.identity], context)
        self.assertEqual((await self.connector.execute(self.request()))['state'], 'completed')

    async def test_changed_verified_identity_replaces_the_offline_route(self):
        self.go_offline()
        await self.connector.fleet.discover()
        self.inventory['peers'][0].update(machine_id=OTHER, online=True, error=None, identity='verified')
        self.write_inventory()
        await self.connector.fleet.discover()
        replacement = route_id('configured-mac', OTHER)
        self.assertEqual(set(self.connector.fleet.routes), {replacement})
        self.assertEqual(set(self.connector.fleet.contexts), {replacement})
        body = self.connector.fleet.heartbeat(await self.connector.snapshot())
        self.assertEqual([row['route_id'] for row in body['peers']], [replacement])
        self.assertEqual((await self.connector.execute(self.request()))['state'], 'failed')
        # An identity-less unreachable alias is still never advertised.
        self.inventory['peers'][0].update(machine_id=None, online=False, error='unavailable', identity=None)
        self.write_inventory()
        await self.connector.fleet.discover()
        self.assertEqual(self.connector.fleet.routes, {})
        self.assertEqual(self.connector.fleet.heartbeat({})['peers'], [])

    async def test_peer_presence_travels_beside_its_snapshot(self):
        presence = {'idle_seconds': 7.0, 'locked': True, 'locked_seconds': 3.0}
        self.inventory['presence'] = {PEER: presence}
        self.write_inventory()
        self.connector.fleet.confirm_manifest(self.connector.fleet.heartbeat(await self.connector.snapshot()))
        with patch.object(self.connector, 'http', AsyncMock(return_value={})) as http:
            await self.connector.fleet.publish(None)
        path, body = http.call_args.args[2], http.call_args.args[3]
        self.assertEqual(path, f'/v1/node/peers/{self.identity}/heartbeat')
        self.assertNotIn('desktop_presence', body['snapshot'])
        self.assertEqual(body['presence'], presence)
        self.assertIsNone(self.connector.presence)

    async def test_restart_claimed_mutation_does_not_replay_peer(self):
        request = self.request('send')
        import hashlib
        digest = hashlib.sha256(json.dumps(request, sort_keys=True, separators=(',', ':'), allow_nan=False).encode()).hexdigest()
        self.connector.journal.claim(request['request_id'], 'send', digest)
        await self.connector.accounts.close()
        await self.connector.fleet.close()
        self.connector.journal.close()
        self.connector = Connector(self.config)
        before = len(self.calls())
        result = await self.connector.execute(request)
        self.assertEqual(result['state'], 'uncertain')
        self.assertEqual(len(self.calls()), before)

    async def test_accounts_history_launch_and_followup_inspection_target_peer(self):
        for operation in ('catalog', 'history', 'launch'):
            result = await self.connector.execute(self.request(operation))
            self.assertEqual(result['state'], 'completed', result)
            if operation == 'catalog': self.assertEqual(result['result']['accounts'][0]['id'], 'account-peer-run')
            if operation == 'history': self.assertEqual(result['result']['machine'], PEER)
            if operation == 'launch': self.assertEqual(result['result']['result_target']['run_id'], 'peer-run')
        self.assertFalse((self.root / ('launch-' + LOCAL + '.json')).exists())
        self.assertTrue((self.root / ('launch-' + PEER + '.json')).exists())

    async def test_project_launch_preflight_and_assignment_use_selected_peer(self):
        request = self.request('launch')
        request['payload'].update(swarm_id='fixture-swarm', project_id='project-peer-run',
            project_folder_id='folder-peer-run', add_folder=False)
        result = await self.connector.execute(request)
        self.assertEqual(result['state'], 'completed', result)
        self.assertEqual(result['result']['project_assignment']['status'], 'assigned')
        assignment = json.loads((self.root / ('assigned-' + PEER + '.json')).read_text())
        self.assertEqual(assignment['project_id'], 'project-peer-run')
        self.assertEqual(assignment['expected_run_id'], 'peer-run')
        self.assertFalse((self.root / ('assigned-' + LOCAL + '.json')).exists())
        wrong = self.request('launch')
        wrong['payload'].update(swarm_id='fixture-swarm', project_id='project-local-run', add_folder=True)
        self.assertEqual((await self.connector.execute(wrong))['state'], 'failed')

    async def test_slow_discovery_and_peer_snapshot_do_not_delay_local_heartbeats(self):
        self.inventory['discovery_delay'] = 5
        self.inventory['peer_delay'] = 5
        self.write_inventory()
        published = asyncio.Event()
        async def http(*args, **kwargs):
            if args[2] == '/v1/node/heartbeat': published.set()
            return {}
        with patch.object(self.connector, 'http', http):
            background = asyncio.create_task(self.connector.fleet.run(None))
            heartbeat = asyncio.create_task(self.connector.heartbeats(None))
            try:
                await asyncio.wait_for(published.wait(), 1)
            finally:
                background.cancel(); heartbeat.cancel()
                await asyncio.gather(background, heartbeat, return_exceptions=True)

    async def test_shared_remote_semaphore_and_deadlines_are_bounded(self):
        active = maximum = 0
        async def native(*args, **kwargs):
            nonlocal active, maximum
            active += 1; maximum = max(maximum, active)
            try:
                await asyncio.sleep(.05)
                return {}
            finally:
                active -= 1
        with patch.object(self.connector, '_native', native):
            await asyncio.gather(*(self.peer.native(['inspect', 'codex/example'], timeout=1) for _ in range(12)))
        self.assertEqual(maximum, 4)


    async def test_long_native_name_is_projected_with_relay_manifest_bound(self):
        self.inventory['peers'][0]['name'] = 'N' * 256
        self.write_inventory()
        await self.connector.fleet.discover()
        body = self.connector.fleet.heartbeat(await self.connector.snapshot())
        self.assertEqual(len(body['peers'][0]['name']), 128)

    async def test_peer_snapshot_size_and_inventory_count_are_independently_bounded(self):
        self.connector.fleet.confirm_manifest(self.connector.fleet.heartbeat(await self.connector.snapshot()))
        with patch.object(self.peer, 'snapshot', AsyncMock(return_value={'sessions': [], 'large': 'x' * (1024 * 1024)})):
            with patch.object(self.connector, 'http', AsyncMock()) as http:
                await self.connector.fleet.publish(None)
                http.assert_not_awaited()
        inventory = {**self.inventory, 'peers': self.inventory['peers'] * 33}
        with patch.object(self.connector, '_native', AsyncMock(return_value=inventory)):
            await self.connector.fleet.discover()
        self.assertFalse(self.connector.fleet.routes)
        self.assertFalse(self.connector.fleet.contexts)

    async def test_claim_poll_opt_in_tracks_guarded_routing_capability(self):
        for available in (True, False):
            self.connector.fleet.supported = available
            self.connector.snapshot_ready.set()
            observed = asyncio.Event()
            paths = []
            async def http(*args, **kwargs):
                paths.append(args[2]); observed.set()
                return {'requests': []}
            with patch.object(self.connector, 'http', http):
                task = asyncio.create_task(self.connector.requests(None))
                try: await asyncio.wait_for(observed.wait(), 1)
                finally:
                    task.cancel(); await asyncio.gather(task, return_exceptions=True)
            self.assertEqual('&gateway_one_hop=1' in paths[0], available)

    async def test_cancellation_during_target_snapshot_records_failed_receipt_without_native_replay(self):
        request = self.request('send')
        entered = asyncio.Event()
        self.peer.snapshot_ready.clear()
        async def snapshot():
            entered.set()
            await asyncio.Future()
        with patch.object(self.peer, 'snapshot', snapshot):
            task = asyncio.create_task(self.connector.execute(request))
            await entered.wait()
            task.cancel()
            with self.assertRaises(asyncio.CancelledError): await task
        before = len(self.calls())
        self.assertEqual((await self.connector.execute(request))['state'], 'failed')
        self.assertEqual(len(self.calls()), before)

    async def test_each_claim_dispatches_only_target_without_full_peer_discovery(self):
        with patch.object(self.connector.fleet, 'discover', AsyncMock(side_effect=AssertionError('full inventory scan'))):
            for operation in ('inspect', 'send', 'history'):
                self.assertEqual((await self.connector.execute(self.request(operation)))['state'], 'completed')

    async def test_startup_poll_transitions_from_short_legacy_wait_to_guarded_wait(self):
        self.connector.fleet.local_id = None
        paths = []
        reached_guarded = asyncio.Event()
        async def http(*args, **kwargs):
            paths.append(args[2])
            if len(paths) == 1:
                self.connector.fleet.local_id = LOCAL
            else:
                reached_guarded.set()
            return {'requests': []}
        with patch.object(self.connector, 'http', http):
            task = asyncio.create_task(self.connector.requests(None))
            try: await asyncio.wait_for(reached_guarded.wait(), 1)
            finally:
                task.cancel(); await asyncio.gather(task, return_exceptions=True)
        self.assertEqual(paths[0], '/v1/node/requests?wait=1')
        self.assertEqual(paths[1], '/v1/node/requests?wait=25&gateway_one_hop=1')

    async def test_missing_peer_route_keeps_other_peer_and_local_guarded_routing(self):
        self.inventory['peers'].append({'via': 'configured-other', 'machine_id': OTHER,
            'name': 'Other peer', 'online': True, 'error': None})
        self.write_inventory()
        await self.connector.fleet.discover()
        snapshot = await self.connector.snapshot()
        body = self.connector.fleet.heartbeat(snapshot)
        self.connector.fleet.confirm_manifest(body)
        published = []
        async def http(client, method, path, payload=None, **kwargs):
            if path == f'/v1/node/peers/{self.identity}/heartbeat':
                raise RelayError(404)
            published.append(path)
            return {'requests': []} if method == 'GET' else {}
        with patch.object(self.connector, 'http', http):
            await self.connector.fleet.publish(None)
            self.assertIn(f'/v1/node/peers/{route_id("configured-other", OTHER)}/heartbeat', published)
            self.assertTrue(self.connector.fleet.available)
            self.assertTrue(self.connector.fleet.relay_supported)
            self.assertEqual(self.connector.fleet.heartbeat(snapshot), body)
            self.assertIn('gateway_one_hop', snapshot['mobile_capabilities']['features'])
            task = asyncio.create_task(self.connector.requests(None))
            try:
                for _ in range(20):
                    if any(path.startswith('/v1/node/requests') for path in published):
                        break
                    await asyncio.sleep(.01)
            finally:
                task.cancel(); await asyncio.gather(task, return_exceptions=True)
            self.assertIn('/v1/node/requests?wait=25&gateway_one_hop=1', published)
            local = await self.connector.execute(self.request(peer=False))
            self.assertEqual(local['state'], 'completed')
            self.assertEqual(local['result']['machine'], LOCAL)

    async def test_pre_gateway_relay_fallback_chain_and_presence_probe(self):
        self.connector.fleet.supported = True
        self.connector.presence = {'idle_seconds': 4.0, 'locked': False, 'locked_seconds': None}
        calls = []

        async def http(*args, **kwargs):
            body = args[3]; calls.append(body)
            # A real pre-gateway relay (and the strict Python reference) accepts only the plain shape.
            if set(body) != {'snapshot'}: raise RelayError(400)
            return {}

        class Stop(Exception): pass

        async def beat():
            async def sleep(_): raise Stop
            with patch.object(self.connector, 'http', http), patch('zerus_mobile.connector.asyncio.sleep', sleep):
                with self.assertRaises(Stop):
                    await self.connector.heartbeats(None)

        def shape(body): return ('gateway' if 'machine_id' in body else 'snapshot') + ('+presence' if 'presence' in body else '')
        await beat()
        self.assertEqual([shape(body) for body in calls],
                         ['gateway+presence', 'gateway', 'snapshot+presence', 'snapshot'])
        self.assertNotIn('gateway_one_hop', calls[-1]['snapshot']['mobile_capabilities']['features'])
        self.assertGreater(self.connector.presence_retry_at, time.monotonic() + 290)
        calls.clear()
        await beat()  # Within 300 s: no failing attempt repeats and no presence is sent.
        self.assertEqual([shape(body) for body in calls], ['snapshot'])
        calls.clear()
        self.connector.presence_retry_at = time.monotonic() - 1  # The 300 s interval elapsed.
        await beat()
        self.assertEqual([shape(body) for body in calls], ['snapshot+presence', 'snapshot'])
        self.assertGreater(self.connector.presence_retry_at, time.monotonic() + 290)

    async def test_old_native_never_probes_dispatch_and_old_relay_falls_back(self):
        self.connector.fleet.supported = False
        before = len(self.calls())
        await self.connector.fleet.discover()
        self.assertEqual(len(self.calls()), before)
        legacy = self.request(peer=False); legacy.pop('gateway_route')
        result = await self.connector.execute(legacy)
        self.assertEqual(result['state'], 'completed')
        self.assertEqual(self.calls()[-1]['argv'], ['inspect', 'codex/example'])

    async def test_mark_read_reaches_only_the_selected_machine(self):
        for peer in (True, False):
            run = 'peer-run' if peer else 'local-run'
            request = self.request('mark_read', peer=peer)
            request['payload'] = {'run_id': run, 'conversation_id': 'peer-conversation' if peer else 'local-conversation',
                                  'reply_id': 'reply-' + run, 'attention_signature': None}
            result = await self.connector.execute(request)
            self.assertEqual(result['state'], 'completed', result)
            self.assertTrue(result['result']['applied']['reply_id'])
        effects = [json.loads(row) for row in (self.root / 'native_effects.jsonl').read_text().splitlines()]
        self.assertEqual([(row['argv'], row['machine']) for row in effects if row['argv'][0] == 'read'],
                         [(['read', 'codex/example', '--json'], PEER), (['read', 'codex/example', '--json'], LOCAL)])
