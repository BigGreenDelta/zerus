"""Explicit one-folder mutation, exact routed receipt and permanent no replay."""
import asyncio
import hashlib
import json
import os
from pathlib import Path
import tempfile
import unittest
import uuid
from unittest.mock import AsyncMock, patch

from aiohttp import ClientSession

from relay_fixture import start_relay
from zerus_mobile import context, launch
from zerus_mobile.connector import Connector, ConnectorError, MUTATIONS
from zerus_mobile.server import validate_request


class DirectoryCreate(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.config = {'server_url': 'https://relay.example.invalid', 'node_token': 'fixture',
                       'state_dir': self.tmp.name}
        self.node = Connector(self.config)
        self.node.directory_create_supported = True

    async def asyncTearDown(self):
        await self.node.accounts.close()
        await self.node.fleet.close()
        self.node.journal.close()
        self.tmp.cleanup()

    def request(self, path='/alias/new project;$(literal)'):
        identity = str(uuid.uuid4())
        return {'request_id': identity, 'computer_id': str(uuid.uuid4()),
                'operation': 'directory_create', 'session': '',
                'payload': {'request_id': identity, 'path': path}}

    def created(self, request, parent='/canonical'):
        return {'status': 'created', 'request_id': request['request_id'],
                'requested_path': request['payload']['path'],
                'path': str(Path(parent) / request['payload']['path'].rsplit('/', 1)[-1]),
                'parent': parent}

    async def test_fixed_literal_argv_and_exact_parent_receipt_replay_once(self):
        request = self.request()
        raw = {**self.created(request), 'secret': 'not projected'}
        with patch.object(self.node, 'native', AsyncMock(side_effect=[{'path': '/canonical'}, raw])) as native:
            result = await self.node.execute(request)
            self.assertEqual(result, {'state': 'completed', 'result': self.created(request), 'error': None})
            self.assertEqual(native.call_args_list[0].args[0], ['dirs', '/alias'])
            self.assertEqual(native.call_args_list[1].args[0],
                             ['dirs', '--create', request['payload']['path'], '--request-id', request['request_id']])
            self.assertEqual(native.call_args_list[1].kwargs['timeout'], 5)
            self.assertEqual(await self.node.execute(request), result)
            self.assertEqual(native.await_count, 2)
            changed = {**request, 'payload': {**request['payload'], 'path': '/other/new'}}
            with self.assertRaisesRegex(ConnectorError, 'conflicts'):
                await self.node.execute(changed)
            self.assertEqual(native.await_count, 2)

    async def test_missing_capability_fails_before_native_or_parent_read(self):
        self.node.directory_create_supported = False
        with patch.object(self.node, 'native', AsyncMock()) as native:
            result = await self.node.execute(self.request())
            self.assertEqual(result['state'], 'failed')
            native.assert_not_awaited()

    async def test_invalid_wire_fields_paths_and_uuid_are_rejected_without_native(self):
        for path in ('~', '~/new', 'relative', '/', '/new/', '/new/.', '/new/..',
                     '/new\nname', '/new\x7fname', '/' + '界' * 1366, '', None):
            request = self.request(path)
            with self.assertRaises(Exception):
                validate_request(request)
            with patch.object(self.node, 'native', AsyncMock()) as native:
                self.assertEqual((await self.node.execute(request))['state'], 'failed')
                native.assert_not_awaited()
        for key, value in [('command', 'mkdir'), ('parents', True), ('shell', True),
                           ('request_id', str(uuid.uuid4()))]:
            request = self.request()
            request['payload'][key] = value
            with self.assertRaises(Exception):
                validate_request(request)
            with patch.object(self.node, 'native', AsyncMock()) as native:
                self.assertEqual((await self.node.execute(request))['state'], 'failed')
                native.assert_not_awaited()
        request = self.request()
        request['session'] = 'codex/example'
        with self.assertRaises(Exception):
            validate_request(request)
        request = self.request()
        request['request_id'] = request['payload']['request_id'] = 'AAAAAAAA-AAAA-4AAA-8AAA-AAAAAAAAAAAA'
        with self.assertRaises(Exception):
            validate_request(request)
        with self.assertRaises(ConnectorError):
            await self.node.execute(request)
        validate_request(self.request('/' + '界' * 1365))  # exactly 4096 UTF-8 bytes

    async def test_parent_failure_or_malformed_parent_does_not_dispatch_mutation(self):
        for parent in (ConnectorError('native command failed'), {}, [], {'path': 'relative'},
                       {'path': '/canonical/..'}, {'path': '//canonical'}, {'path': '/canonical/'}):
            with patch.object(self.node, 'native', AsyncMock(side_effect=[parent])) as native:
                result = await self.node.execute(self.request())
                self.assertEqual(result['state'], 'failed', result)
                self.assertEqual(native.await_count, 1)
                self.assertFalse(any('--create' in call.args[0] for call in native.call_args_list))

    async def test_unconfirmed_native_outcome_is_uncertain_and_duplicate_never_retries(self):
        for delta in ({'request_id': str(uuid.uuid4())}, {'requested_path': '/other/new'},
                      {'parent': '/other'}, {'path': '/other/new'}, {'status': 'failed'}):
            request = self.request()
            with patch.object(self.node, 'native', AsyncMock(side_effect=[{'path': '/canonical'},
                               {**self.created(request), **delta}])) as native:
                result = await self.node.execute(request)
                self.assertEqual(result['state'], 'uncertain', result)
                self.assertEqual(await self.node.execute(request), result)
                self.assertEqual(native.await_count, 2)
        for raw in (None, [], ConnectorError('native command timed out'), ConnectorError('native command failed')):
            request = self.request()
            with patch.object(self.node, 'native', AsyncMock(side_effect=[{'path': '/canonical'}, raw])) as native:
                result = await self.node.execute(request)
                self.assertEqual(result['state'], 'uncertain', result)
                self.assertEqual(await self.node.execute(request), result)
                self.assertEqual(native.await_count, 2)

    async def test_cancellation_after_dispatch_is_uncertain_without_replay(self):
        entered = asyncio.Event()
        async def native(argv, **kwargs):
            if '--create' not in argv:
                return {'path': '/canonical'}
            entered.set()
            await asyncio.Future()
        request = self.request()
        with patch.object(self.node, 'native', native):
            task = asyncio.create_task(self.node.execute(request))
            await entered.wait()
            task.cancel()
            with self.assertRaises(asyncio.CancelledError):
                await task
        with patch.object(self.node, 'native', AsyncMock()) as mock:
            self.assertEqual((await self.node.execute(request))['state'], 'uncertain')
            mock.assert_not_awaited()

    async def test_duplicate_during_native_flight_preserves_one_admission(self):
        request = self.request()
        entered, release = asyncio.Event(), asyncio.Event()
        mutations = []
        async def native(argv, **kwargs):
            if '--create' not in argv:
                return {'path': '/canonical'}
            mutations.append(argv)
            entered.set()
            await release.wait()
            return self.created(request)
        with patch.object(self.node, 'native', native):
            first = asyncio.create_task(self.node.execute(request))
            await entered.wait()
            concurrent = await self.node.execute(request)
            self.assertEqual(concurrent['state'], 'uncertain')
            self.assertIn('already in progress', concurrent['error'])
            self.assertEqual(len(mutations), 1)
            release.set()
            result = await first
            self.assertEqual(result['state'], 'completed')
            self.assertEqual(await self.node.execute(request), result)
            self.assertEqual(len(mutations), 1)

    async def test_restart_recovers_inflight_as_mutation_and_retains_identity_after_prune(self):
        request = self.request()
        digest = hashlib.sha256(json.dumps(request, sort_keys=True, separators=(',', ':'), allow_nan=False).encode()).hexdigest()
        self.assertTrue(self.node.journal.claim(request['request_id'], 'directory_create', digest))
        await self.node.accounts.close()
        self.node.journal.close()
        self.node = Connector(self.config)
        with patch.object(self.node, 'native', AsyncMock()) as native:
            result = await self.node.execute(request)
            self.assertEqual(result['state'], 'uncertain')
            native.assert_not_awaited()
        self.node.journal.delivered(request['request_id'])
        with self.node.journal.db:
            self.node.journal.db.execute('UPDATE requests SET delivered_at=1 WHERE request_id=?', (request['request_id'],))
        self.node.journal.prune(now=7200)
        self.assertEqual(self.node.journal.replay(request['request_id']), result)
        self.node.journal.delivered(request['request_id'])
        with self.node.journal.db:
            self.node.journal.db.execute('UPDATE requests SET delivered_at=1 WHERE request_id=?', (request['request_id'],))
        self.node.journal.prune()
        self.assertEqual(self.node.journal.replay(request['request_id'])['result'], {'status': 'history_expired'})
        self.assertFalse(self.node.journal.claim(request['request_id'], 'directory_create', digest))
        self.assertIn('directory_create', MUTATIONS)
        self.assertNotIn('directory_create', context.READ_OPERATIONS)
        self.assertNotIn('directory_create', context.READ_SQL)
        self.assertNotIn('directory_create', context.READ_CLASS_SQL)

    async def test_help_discovery_is_additive_independent_and_downgrade_safe(self):
        for help_text, expected in [('--launch-id\nMobile worktree ABI: worktrees-v1', False),
                                     (launch.DIRECTORY_CREATE_HELP, True), ('', False),
                                     ('--launch-id\nMobile worktree ABI: worktrees-v1\n' + launch.DIRECTORY_CREATE_HELP, True)]:
            self.node.capabilities_next_poll = 0
            async def native(argv, **kwargs):
                if argv == ['--help']:
                    return help_text
                if argv == ['ls', '--json', '--local']:
                    return {'sessions': []}
                if argv == ['swarm', 'get']:
                    raise ConnectorError('unavailable')
                if argv == ['account', 'ls']:
                    return {'profiles': []}
                raise AssertionError(argv)
            with patch.object(self.node, 'native', native):
                snapshot = await self.node.snapshot()
            caps = snapshot['mobile_capabilities']
            self.assertEqual('directory_create' in caps['operations'], expected)
            self.assertEqual(launch.DIRECTORY_CREATE_FEATURE in caps['features'], expected)
            self.assertEqual(self.node.launch_supported, '--launch-id' in help_text)
            self.assertEqual(self.node.worktree_supported, 'worktrees-v1' in help_text)
        self.node.capabilities_next_poll = 0
        with patch.object(self.node, 'native', AsyncMock(side_effect=ConnectorError('help unavailable'))):
            await self.node.probe_capabilities()
        self.assertFalse(self.node.directory_create_supported)

    async def test_catalog_reports_selected_target_creation_support(self):
        self.node.launch_supported = True
        for supported in (False, True):
            self.node.directory_create_supported = supported
            request = self.request()
            request.update(operation='catalog', payload={'request_id': request['request_id']})
            with patch.object(self.node, 'native', AsyncMock(return_value={'profiles': []})):
                result = await self.node.execute(request)
            self.assertEqual(result['state'], 'completed', result)
            self.assertEqual(result['result']['directory_create_supported'], supported)


class PeerDirectoryCreate(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        import test_connector_peers as fixture
        self.fixture = fixture
        self.write_inventory = fixture.PeerConnectorTests.write_inventory.__get__(self)
        self.calls = fixture.PeerConnectorTests.calls.__get__(self)
        await fixture.PeerConnectorTests.asyncSetUp(self)
        helper = self.root / 'hgs'
        source = helper.read_text().replace('mobile-peers --json\\n',
                                           launch.DIRECTORY_CREATE_HELP + '\\nmobile-peers --json\\n')
        source = source.replace("elif argv[0] == 'dirs':", """elif argv[:2] == ['dirs', '--create']:
    result = {'status': 'created', 'request_id': argv[argv.index('--request-id') + 1],
              'requested_path': argv[2], 'path': '/example/' + run + '/' + argv[2].rsplit('/', 1)[-1],
              'parent': '/example/' + run}
    (root / ('directory-' + machine + '.json')).write_text(json.dumps(result))
    print(json.dumps(result))
elif argv[0] == 'dirs':""")
        helper.write_text(source)
        for node in (self.connector, self.peer):
            node.capabilities_next_poll = 0
            await node.probe_capabilities()

    async def asyncTearDown(self):
        await self.fixture.PeerConnectorTests.asyncTearDown(self)

    def request(self, peer=True):
        request = self.fixture.PeerConnectorTests.request(self, 'catalog', peer=peer)
        request['operation'] = 'directory_create'
        request['computer_id'] = request['gateway_route']['computer_id']
        request['payload']['path'] = '/alias/new'
        return request

    async def test_exact_peer_and_gateway_creation_cannot_cross_machines(self):
        for peer in (True, False):
            request = self.request(peer)
            result = await self.connector.execute(request)
            self.assertEqual(result['state'], 'completed', result)
            run = 'peer-run' if peer else 'local-run'
            self.assertEqual(result['result']['path'], '/example/' + run + '/new')
            before = len(self.calls())
            self.assertEqual(await self.connector.execute(request), result)
            self.assertEqual(len(self.calls()), before)
        for machine in (self.fixture.LOCAL, self.fixture.PEER):
            self.assertTrue((self.root / ('directory-' + machine + '.json')).exists())
        guarded = [row for row in self.calls() if row['argv'] == ['swarm', '__mobile-peer-local', '--json']]
        self.assertEqual(guarded[-1]['payload']['target_machine_id'], self.fixture.LOCAL)
        self.assertEqual(guarded[-1]['payload']['argv'][:2], ['dirs', '--create'])

    async def test_foreign_computer_nested_route_and_unavailable_peer_fail_without_creation(self):
        for delta in ({'computer_id': str(uuid.uuid4())}, {'via': 'foreign'},
                      {'machine_id': self.fixture.OTHER}, {'gateway_route': {}}):
            request = self.request()
            if 'computer_id' in delta:
                request.update(delta)
            else:
                request['gateway_route'].update(delta)
            before = len(self.calls())
            self.assertEqual((await self.connector.execute(request))['state'], 'failed')
            self.assertEqual(len(self.calls()), before)
        self.connector.fleet.routes[self.identity]['online'] = False
        self.assertEqual((await self.connector.execute(self.request()))['state'], 'failed')
        self.assertFalse(any(self.root.glob('directory-*.json')))

    async def test_old_peer_capability_does_not_inherit_new_gateway_support(self):
        self.peer.directory_create_supported = False
        self.assertTrue(self.connector.directory_create_supported)
        before = len(self.calls())
        self.assertEqual((await self.connector.execute(self.request()))['state'], 'failed')
        self.assertEqual(len(self.calls()), before)
        self.assertEqual((await self.connector.execute(self.request(False)))['state'], 'completed')

    async def test_local_native_identity_reset_after_preflight_cannot_create(self):
        original = self.connector._native
        async def native(argv, payload=None, **kwargs):
            value = await original(argv, payload, **kwargs)
            if payload and payload.get('argv', [])[:1] == ['dirs'] and '--create' not in payload['argv']:
                self.inventory['local']['machine_id'] = self.fixture.OTHER
                self.write_inventory()
            return value
        with patch.object(self.connector, '_native', native):
            result = await self.connector.execute(self.request(False))
        self.assertEqual(result['state'], 'uncertain', result)
        self.assertFalse(any(self.root.glob('directory-*.json')))


@unittest.skipUnless(os.environ.get('ZERUS_DIRECTORY_TEST_HGS'), 'Explicit isolated native CLI target required')
class NativeDirectoryCreate(unittest.IsolatedAsyncioTestCase):
    async def test_real_native_symlink_parent_literal_folder_and_durable_duplicate(self):
        with tempfile.TemporaryDirectory(prefix='zerus-directory-native-') as root:
            base = Path(root)
            home = base / 'home'
            home.mkdir()
            parent = base / 'parent'
            parent.mkdir()
            alias = base / 'alias'
            alias.symlink_to(parent, target_is_directory=True)
            literal = 'new project;$(literal) Русский'
            path = str(alias / literal)
            environment = {key: value for key, value in os.environ.items()
                           if not key.startswith(('HGS_', 'OPENCODE_'))}
            environment.update(HOME=str(home), HGS_STATE_DIR=str(base / 'state'))
            native = str(Path(os.environ['ZERUS_DIRECTORY_TEST_HGS']).resolve())
            node = Connector({'server_url': 'https://relay.example.invalid', 'node_token': 'fixture',
                              'state_dir': str(base / 'journal'), 'hgs_path': native})
            request = DirectoryCreate.request(self, path)
            try:
                with patch.dict(os.environ, environment, clear=True):
                    await node.probe_capabilities()
                    self.assertTrue(node.directory_create_supported)
                    with patch.object(node, '_native', wraps=node._native) as calls:
                        result = await node.execute(request)
                        self.assertEqual(result['state'], 'completed', result)
                        self.assertEqual(result['result']['path'], str(parent.resolve() / literal))
                        self.assertEqual(result['result']['requested_path'], path)
                        self.assertEqual(result['result']['parent'], str(parent.resolve()))
                        self.assertEqual(await node.execute(request), result)
                        self.assertEqual(calls.await_count, 2)
                    created = parent / literal
                    self.assertTrue(created.is_dir())
                    self.assertEqual(list(created.iterdir()), [])
                    self.assertFalse((created / '.git').exists())
                    marker = created / 'keep.txt'
                    marker.write_text('preserved')
                    conflicting = DirectoryCreate.request(self, path)
                    result = await node.execute(conflicting)
                    self.assertEqual(result['state'], 'uncertain')
                    self.assertEqual(marker.read_text(), 'preserved')
                    missing = base / 'missing' / 'child'
                    self.assertEqual((await node.execute(DirectoryCreate.request(self, str(missing))))['state'], 'failed')
                    self.assertFalse(missing.parent.exists())
            finally:
                await node.accounts.close()
                await node.fleet.close()
                node.journal.close()


class RustDirectoryHttp(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        relay = await start_relay(rust_only=True)
        self.addAsyncCleanup(relay.cleanup)
        self.node = relay.node
        self.phone = await relay.pair('Example phone', relay.invitation['pair_code'])
        self.client = ClientSession(base_url=relay.url)
        self.addAsyncCleanup(self.client.close)

    def headers(self, node=False):
        return {'Authorization': 'Bearer ' + (self.node['node_token'] if node else self.phone['device_token'])}

    async def call(self, method, route, body=None, node=False, expected=200, headers=None):
        async with self.client.request(method, route, json=body, headers=headers or self.headers(node)) as response:
            self.assertEqual(response.status, expected, await response.text())
            return await response.json()

    def request(self, computer=None):
        request = DirectoryCreate.request(self, '/workspace/new folder')
        request['computer_id'] = computer or self.node['node_id']
        return request

    async def test_actual_http_capability_validation_and_receipt_never_reclaim(self):
        self.assertIn('directory_create', (await self.call('GET', '/v1/capabilities'))['operations'])
        request = self.request()
        await self.call('POST', '/v1/requests', request, expected=409)
        snapshot = {'sessions': [], 'mobile_capabilities': {'protocol_version': 1,
                    'operations': ['directory_create']}}
        await self.call('POST', '/v1/node/heartbeat', {'snapshot': snapshot}, node=True)
        for path in ('~', 'relative', '/', '/folder/', '/folder/..', '/new\nname', '/' + '界' * 1366):
            invalid = self.request()
            invalid['payload']['path'] = path
            await self.call('POST', '/v1/requests', invalid, expected=400)
        await self.call('POST', '/v1/requests', request, expected=202)
        await self.call('POST', '/v1/requests', request, expected=202)
        claim = await self.call('GET', '/v1/node/requests', node=True)
        self.assertEqual(len(claim['requests']), 1)
        self.assertEqual(claim['requests'][0]['payload'], request['payload'])
        self.assertEqual((await self.call('GET', '/v1/node/requests', node=True))['requests'], [])
        receipt = {'state': 'uncertain', 'result': None, 'error': 'native outcome unavailable'}
        await self.call('POST', '/v1/node/requests/' + request['request_id'] + '/result', receipt, node=True)
        self.assertEqual((await self.call('POST', '/v1/requests', request, expected=202))['state'], 'uncertain')
        self.assertEqual((await self.call('GET', '/v1/node/requests', node=True))['requests'], [])

    async def test_actual_http_peer_route_is_exact_and_withdrawal_preserves_uncertainty(self):
        machine, root, route = (str(uuid.uuid4()) for _ in range(3))
        snapshot = {'sessions': [], 'mobile_capabilities': {'protocol_version': 1,
                    'operations': ['directory_create'], 'features': ['gateway_one_hop']}}
        heartbeat = {'snapshot': snapshot, 'machine_id': root, 'peers': [
                    {'route_id': route, 'machine_id': machine, 'name': 'Peer', 'online': True}]}
        await self.call('POST', '/v1/node/heartbeat', heartbeat, node=True)
        await self.call('POST', '/v1/node/peers/' + route + '/heartbeat',
                        {'machine_id': machine, 'snapshot': snapshot}, node=True)
        computers = (await self.call('GET', '/v1/computers'))['computers']
        computer = next(row for row in computers if row.get('machine_id') == machine)
        request = self.request(computer['id'])
        await self.call('POST', '/v1/requests', request, expected=202)
        self.assertEqual((await self.call('GET', '/v1/node/requests', node=True))['requests'], [])
        claim = (await self.call('GET', '/v1/node/requests?gateway_one_hop=1', node=True))['requests'][0]
        self.assertEqual(claim['gateway_route'], {'schema': 1, 'route_id': route,
                         'computer_id': computer['id'], 'machine_id': machine})
        heartbeat['peers'] = []
        await self.call('POST', '/v1/node/heartbeat', heartbeat, node=True)
        result = await self.call('GET', '/v1/requests/' + request['request_id'])
        self.assertEqual(result['state'], 'uncertain')
        self.assertEqual((await self.call('POST', '/v1/requests', request, expected=202))['state'], 'uncertain')
        self.assertEqual((await self.call('GET', '/v1/node/requests?gateway_one_hop=1', node=True))['requests'], [])

    @unittest.skipUnless(os.environ.get('ZERUS_DIRECTORY_TEST_HGS'), 'Explicit isolated native CLI target required')
    async def test_actual_http_connector_and_guarded_native_cli_create_one_folder(self):
        with tempfile.TemporaryDirectory(prefix='zerus-directory-roundtrip-') as root:
            base = Path(root)
            home = base / 'home'
            home.mkdir()
            parent = base / 'parent'
            parent.mkdir()
            alias = base / 'alias'
            alias.symlink_to(parent, target_is_directory=True)
            native = str(Path(os.environ['ZERUS_DIRECTORY_TEST_HGS']).resolve())
            node = Connector({'server_url': 'https://relay.example.invalid', 'node_token': 'fixture',
                              'state_dir': str(base / 'journal'), 'hgs_path': native})
            environment = {key: value for key, value in os.environ.items()
                           if not key.startswith(('HGS_', 'OPENCODE_'))}
            environment.update(HOME=str(home), HGS_STATE_DIR=str(base / 'state'))
            try:
                with patch.dict(os.environ, environment, clear=True):
                    await node.probe_capabilities()
                    await node.fleet.discover()
                    self.assertTrue(node.fleet.available)
                    snapshot = {'sessions': [], 'mobile_capabilities': {'protocol_version': 1,
                                'operations': ['directory_create'], 'features': ['gateway_one_hop']}}
                    await self.call('POST', '/v1/node/heartbeat', {'snapshot': snapshot,
                                    'machine_id': node.fleet.local_id, 'peers': []}, node=True)
                    request = self.request()
                    request['payload']['path'] = str(alias / 'new project')
                    await self.call('POST', '/v1/requests', request, expected=202)
                    claimed = (await self.call('GET', '/v1/node/requests?gateway_one_hop=1', node=True))['requests'][0]
                    self.assertEqual(claimed['gateway_route']['machine_id'], node.fleet.local_id)
                    with patch.object(node, '_native', wraps=node._native) as calls:
                        result = await node.execute(claimed)
                        self.assertEqual(result['state'], 'completed', result)
                        self.assertEqual(result['result']['path'], str(parent.resolve() / 'new project'))
                        await self.call('POST', '/v1/node/requests/' + request['request_id'] + '/result', result, node=True)
                        replay = await self.call('POST', '/v1/requests', request, expected=202)
                        self.assertEqual(replay['result'], result['result'])
                        self.assertEqual(await node.execute(claimed), result)
                        self.assertEqual(calls.await_count, 2)
                        for call in calls.call_args_list:
                            self.assertEqual(call.args[0], ['swarm', '__mobile-peer-local', '--json'])
                            self.assertEqual(call.args[1]['target_machine_id'], node.fleet.local_id)
                    self.assertEqual((await self.call('GET', '/v1/node/requests?gateway_one_hop=1', node=True))['requests'], [])
                    self.assertTrue((parent / 'new project').is_dir())
                    self.assertEqual(list((parent / 'new project').iterdir()), [])
            finally:
                await node.accounts.close()
                await node.fleet.close()
                node.journal.close()
