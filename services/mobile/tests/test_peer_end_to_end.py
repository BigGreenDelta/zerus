"""Phone HTTP through a running gateway, with isolated native-shaped peers."""
import asyncio
import json
import os
from pathlib import Path
import tempfile
import time
import unittest
import uuid

from aiohttp import ClientSession

from zerus_mobile.connector import Connector
from zerus_mobile.server import Config
from relay_fixture import start_relay
from zerus_mobile.store import Store


LOCAL = '00000000-0000-4000-8000-000000000001'
PEER = '00000000-0000-4000-8000-000000000002'


class PeerEndToEndTests(unittest.IsolatedAsyncioTestCase):
    async def test_phone_reaches_peer_only_through_enrolled_gateway(self):
        with tempfile.TemporaryDirectory(prefix='zerus-peer-http-') as directory:
            root = Path(directory)
            native = root / 'hgs'
            native.write_text(Path(__file__).with_name('fixture_peer_hgs.py').read_text())
            native.chmod(0o700)
            inventory = {'schema': 1, 'local': {'machine_id': LOCAL, 'name': 'Gateway'},
                         'peers': [{'via': 'private-peer-alias', 'machine_id': PEER,
                                    'name': 'Peer', 'online': True, 'error': None}]}
            (root / 'inventory.json').write_text(json.dumps(inventory))
            store = Store(root / 'relay.sqlite3')
            workspace = store.workspace('Example workspace')
            gateway = store.node(workspace, 'Gateway')
            phone = store.pair(store.invite(workspace)['pair_code'], 'Example phone')
            runner, url = await start_relay(store, Config(background=False))
            connector = Connector({'server_url': url, 'node_token': gateway['node_token'],
                                   'hgs_path': str(native), 'state_dir': str(root / 'journal'),
                                   'poll_interval': 1}, allow_insecure_localhost=True)
            task = asyncio.create_task(connector.run())
            try:
                async with ClientSession(headers={'Authorization': 'Bearer ' + phone['device_token']}) as client:
                    async def call(method, path, body=None):
                        async with client.request(method, url + path, json=body) as response:
                            result = await response.json()
                            self.assertLess(response.status, 300, result)
                            return result

                    async def wait_for(path, predicate, seconds=15):
                        async def poll():
                            while True:
                                if task.done():
                                    task.result()
                                result = await call('GET', path)
                                if predicate(result):
                                    return result
                                await asyncio.sleep(.05)
                        return await asyncio.wait_for(poll(), seconds)

                    catalog = await wait_for('/v1/computers', lambda data:
                        len(data['computers']) == 2 and all(row['online'] and row['snapshot']
                            and row['snapshot'].get('sessions') for row in data['computers']))
                    computers = {row['machine_id']: row for row in catalog['computers']}
                    self.assertEqual(set(computers), {LOCAL, PEER})
                    self.assertEqual(computers[LOCAL]['id'], gateway['node_id'])
                    self.assertEqual(computers[PEER]['via']['gateway_id'], gateway['node_id'])
                    self.assertNotIn('private-peer-alias', json.dumps(catalog))
                    self.assertNotIn('never-export-third-peer', json.dumps(catalog))
                    # A peer's appearance never creates another queue credential.
                    self.assertEqual(store.db.execute('SELECT count(*) FROM nodes').fetchone()[0], 1)

                    async def request(machine, operation):
                        identity = str(uuid.uuid4())
                        session = computers[machine]['snapshot']['sessions'][0]
                        payload = {} if operation == 'inspect' else {
                            'request_id': identity, 'expected_run_id': session['run_id'],
                            'expected_conversation_id': session['conversation_id'],
                            'text': 'Literal message for the selected peer'}
                        body = {'request_id': identity, 'computer_id': computers[machine]['id'],
                                'operation': operation, 'session': session['name'], 'payload': payload}
                        await call('POST', '/v1/requests', body)
                        result = await wait_for('/v1/requests/' + identity,
                                                lambda data: data['state'] not in {'queued', 'claimed'})
                        self.assertEqual(result['state'], 'completed', result)
                        self.assertEqual(result['result']['machine'], machine)
                        return body, result

                    # Equal session names must retain separate native identities.
                    await request(LOCAL, 'inspect')
                    await request(PEER, 'inspect')
                    body, receipt = await request(PEER, 'send')
                    self.assertEqual(await call('POST', '/v1/requests', body), receipt)
                    calls = [json.loads(line) for line in (root / 'calls.jsonl').read_text().splitlines()]
                    sends = [row for row in calls if isinstance(row['payload'], dict)
                             and row['payload'].get('argv', [None])[0] == 'send']
                    self.assertEqual(len(sends), 1)
                    self.assertEqual(sends[0]['argv'], ['swarm', 'mobile-peer', '--json'])
                    self.assertEqual(sends[0]['payload']['target_machine_id'], PEER)

                    # Withdrawal reaches the phone, while the durable receipt survives.
                    inventory['peers'] = []
                    (root / 'inventory.json').write_text(json.dumps(inventory))
                    await wait_for('/v1/computers', lambda data:
                        not any(row.get('machine_id') == PEER and row['online'] for row in data['computers']))
                    self.assertEqual(await call('POST', '/v1/requests', body), receipt)
            finally:
                task.cancel()
                await asyncio.gather(task, return_exceptions=True)
                connector.journal.close()
                await runner.cleanup()
                store.close()

    async def test_unreachable_peer_stays_listed_offline_under_its_gateway(self):
        with tempfile.TemporaryDirectory(prefix='zerus-peer-offline-') as directory:
            root = Path(directory)
            native = root / 'hgs'
            native.write_text(Path(__file__).with_name('fixture_peer_hgs.py').read_text())
            native.chmod(0o700)
            inventory = {'schema': 1, 'local': {'machine_id': LOCAL, 'name': 'Gateway'},
                         'peers': [{'via': 'private-peer-alias', 'machine_id': PEER, 'name': 'Peer',
                                    'online': True, 'error': None, 'identity': 'verified'}]}
            (root / 'inventory.json').write_text(json.dumps(inventory))
            store = Store(root / 'relay.sqlite3')
            workspace = store.workspace('Example workspace')
            gateway = store.node(workspace, 'Gateway')
            phone = store.pair(store.invite(workspace)['pair_code'], 'Example phone')
            runner, url = await start_relay(store, Config(background=False))
            connector = Connector({'server_url': url, 'node_token': gateway['node_token'],
                                   'hgs_path': str(native), 'state_dir': str(root / 'journal'),
                                   'poll_interval': 1}, allow_insecure_localhost=True)
            task = asyncio.create_task(connector.run())
            try:
                async with ClientSession(headers={'Authorization': 'Bearer ' + phone['device_token']}) as client:
                    async def wait_for(predicate, seconds=15):
                        async def poll():
                            while True:
                                if task.done():
                                    task.result()
                                async with client.get(url + '/v1/computers') as response:
                                    self.assertEqual(response.status, 200)
                                    data = await response.json()
                                peer = next((row for row in data['computers'] if row.get('machine_id') == PEER), None)
                                if predicate(peer):
                                    return data, peer
                                await asyncio.sleep(.05)
                        return await asyncio.wait_for(poll(), seconds)

                    _, online = await wait_for(lambda row: row and row['online'] and row['snapshot'])
                    inventory['peers'][0].update(online=False, error='unavailable', identity='last_verified')
                    (root / 'inventory.json').write_text(json.dumps(inventory))
                    catalog, offline = await wait_for(lambda row: row and not row['online'])
                    # Same computer (aliases, colours and filters stay stable), still one hop
                    # below the enrolled gateway, without stale sessions or the SSH alias.
                    self.assertEqual(offline['id'], online['id'])
                    self.assertEqual(offline['via']['gateway_id'], gateway['node_id'])
                    self.assertIsNone(offline['snapshot'])
                    self.assertNotIn('private-peer-alias', json.dumps(catalog))
                    rust = bool(os.environ.get('ZERUS_RELAY_BINARY'))
                    if rust:
                        # The Rust relay keeps the last accepted peer heartbeat for
                        # "Offline, last seen"; the retiring Python reference clears it.
                        self.assertLessEqual(online['last_seen_at'], offline['last_seen_at'])
                        self.assertLessEqual(offline['last_seen_at'], time.time())
                    calls = (root / 'calls.jsonl').read_text().count('"mobile-peer"')
                    identity = str(uuid.uuid4())
                    body = {'request_id': identity, 'computer_id': offline['id'], 'operation': 'send',
                            'session': 'codex/example', 'payload': {'request_id': identity,
                            'expected_run_id': 'peer-run', 'expected_conversation_id': 'peer-conversation',
                            'text': 'Literal message for an offline peer'}}
                    async with client.post(url + '/v1/requests', json=body) as response:
                        self.assertEqual(response.status, 409)
                    await asyncio.sleep(1.5)
                    self.assertEqual((root / 'calls.jsonl').read_text().count('"mobile-peer"'), calls)
                    self.assertFalse((root / 'native_effects.jsonl').exists())
                    # Reconnection with the same verified identity restores that computer.
                    inventory['peers'][0].update(online=True, error=None, identity='verified')
                    (root / 'inventory.json').write_text(json.dumps(inventory))
                    _, restored = await wait_for(lambda row: row and row['online'] and row['snapshot'])
                    self.assertEqual(restored['id'], online['id'])
                    if rust:
                        self.assertLess(offline['last_seen_at'], restored['last_seen_at'])
            finally:
                task.cancel()
                await asyncio.gather(task, return_exceptions=True)
                connector.journal.close()
                await runner.cleanup()
                store.close()


if __name__ == '__main__':
    unittest.main()
