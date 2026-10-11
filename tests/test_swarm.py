#!/usr/bin/env python3
"""Catalog replication through the real CLI, with isolated nodes and fake SSH."""
import copy
import fcntl
import json
import os
from pathlib import Path
import subprocess
import tempfile
import time
import unittest

REPO = Path(__file__).resolve().parents[1]
HGS = Path(os.environ.get('HGS_TEST_BIN', REPO / 'target/debug/hgs')).resolve()
SSH = '''#!/usr/bin/env python3
import json, os, pathlib, shlex, subprocess, sys
root = pathlib.Path(os.environ['SWARM_TEST_ROOT'])
alias = sys.argv[-2]
if (root / ('offline-' + alias)).exists():
    print('fixture offline', file=sys.stderr); sys.exit(255)
env = dict(os.environ, HGS_CONFIG_DIR=str(root / alias), HGS_STATE_DIR=str(root / alias / 'state'))
args = shlex.split(sys.argv[-1])[1:]
with (root / 'calls').open('a') as file: file.write(alias + ' ' + ' '.join(args) + '\\n')
if not (root / ('old-' + alias)).exists():
    os.execve(os.environ['SWARM_TEST_HGS'], [os.environ['SWARM_TEST_HGS'], *args], env)
# A member from before shared preferences: it rejects unknown fields and key
# kinds and never advertises the kinds it accepts.
request = sys.stdin.read()
if args[1:] == ['exchange']:
    data = json.loads(request)
    if 'kinds' in data or any(op['key']['kind'] not in ('project', 'folder', 'session', 'machine') for op in data['operations']):
        print('unknown field or variant from a newer member', file=sys.stderr); sys.exit(1)
result = subprocess.run([os.environ['SWARM_TEST_HGS'], *args], input=request, env=env, text=True, capture_output=True)
sys.stderr.write(result.stderr)
if result.returncode: sys.exit(result.returncode)
output = json.loads(result.stdout)
output.pop('kinds', None)
print(json.dumps(output))
'''


def project(id, name, machine, path):
    return dict(id=id, name=name, color='#123456', vivid=False,
                folders=[dict(id='folder-' + id, machine=machine, path=path, name='')], sessions=[])


class Swarm(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory(prefix='hgs-swarm-')
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name)
        (self.root / 'bin').mkdir()
        ssh = self.root / 'bin/ssh'; ssh.write_text(SSH); ssh.chmod(0o755)
        self.env = dict(os.environ, SWARM_TEST_ROOT=str(self.root), SWARM_TEST_HGS=str(HGS),
                        PATH=str(self.root / 'bin') + ':' + os.environ['PATH'], HGS_TAB='0')
        for key in ['HGS_SELF', 'HGS_PEERS', 'HGS_EXECUTABLE']:
            self.env.pop(key, None)
        for node, peers in [('a', 'b'), ('b', 'a c'), ('c', 'b')]:
            (self.root / node).mkdir()
            (self.root / node / 'config').write_text(f'HGS_SELF="{node}"\nHGS_PEERS="{peers}"\n')
            (self.root / node / 'machines.local.json').write_text(json.dumps({'machines': {peers.split()[0]: dict(hostname='host-secret.invalid', user='private-user', identity_file='/private/key', enabled=True)}}))
            self.call(node, 'initialize', data={'projects': [project(node, 'Same name', node, '/' + node)]})

    def call(self, node, *args, data=None, ok=True):
        env = dict(self.env, HGS_CONFIG_DIR=str(self.root / node), HGS_STATE_DIR=str(self.root / node / 'state'))
        result = subprocess.run([str(HGS), 'swarm', *args], input=json.dumps(data) if data is not None else None,
                                env=env, text=True, capture_output=True, timeout=40)
        if ok:
            self.assertEqual(result.returncode, 0, result.stderr)
            return json.loads(result.stdout)
        self.assertNotEqual(result.returncode, 0)
        return result.stderr

    def join(self, node, peer, mapping=None, prefer_peer=False):
        preview = self.call(node, 'preview', peer)
        return self.call(node, 'join', peer, data=dict(node_id=preview['node_id'], swarm_id=preview['swarm_id'], project_map=mapping or {}, prefer_peer_memberships=prefer_peer))

    def patch(self, node, before, desired):
        return self.call(node, 'apply', data=dict(base=before['organization'], desired=desired, versions=before['versions']))

    def converge(self):
        for node in ['a', 'b', 'c', 'b', 'a', 'b']:
            self.call(node, 'sync')
        snapshots = [self.call(node, 'get') for node in ['a', 'b', 'c']]
        self.assertEqual(len({s['digest'] for s in snapshots}), 1)
        return snapshots

    def test_third_node_transitive_sync_access_filter_and_private_configuration(self):
        self.join('a', 'b'); self.join('b', 'a'); self.join('c', 'b'); self.join('b', 'c')
        a, b, c = self.converge()
        self.assertEqual({p['id'] for p in c['organization']['projects']}, {'a', 'b', 'c'})
        # C only configured B. Receiving A's identity cannot grant access to A.
        projects = {p['id']: p for p in c['organization']['projects']}
        self.assertFalse(projects['a']['accessible'])
        self.assertTrue(projects['b']['accessible'])
        self.assertTrue(projects['c']['accessible'])
        exported = json.dumps(self.call('c', 'export'))
        for secret in ['host-secret', 'private-user', '/private/key', 'identity_file', 'hostname']:
            self.assertNotIn(secret, exported)
        # A folder on a configured computer remains visible while offline.
        (self.root / 'offline-b').touch()
        self.assertFalse(self.call('c', 'sync')['b']['ok'])
        self.assertTrue(next(p for p in self.call('c', 'get')['organization']['projects'] if p['id'] == 'b')['accessible'])

    def test_offline_conflicts_deletion_resolution_and_repeated_sync(self):
        self.join('a', 'b'); self.join('b', 'a'); self.join('c', 'b'); self.join('b', 'c')
        a, b, c = self.converge()
        first, second = copy.deepcopy(a['organization']), copy.deepcopy(b['organization'])
        next(p for p in first['projects'] if p['id'] == 'a')['name'] = 'First edit'
        next(p for p in second['projects'] if p['id'] == 'a')['name'] = 'Second edit'
        first['projects'] = [p for p in first['projects'] if p['id'] != 'c']
        self.patch('a', a, first); self.patch('b', b, second)
        third = copy.deepcopy(c['organization'])
        next(p for p in third['projects'] if p['id'] == 'c')['name'] = 'Late offline edit'
        self.patch('c', c, third)
        a, b, c = self.converge()
        self.assertNotIn('c', [p['id'] for p in c['organization']['projects']])
        conflict = next(c for c in a['conflicts'] if c['field'].get('field') == 'name' and c['field'].get('id') == 'a')
        self.assertEqual({v['value'] for v in conflict['variants']}, {'First edit', 'Second edit'})
        self.call('a', 'resolve', data=dict(key=conflict['key'], versions=[v['id'] for v in conflict['variants']], value='Resolved'))
        a, b, c = self.converge()
        self.assertEqual(a['conflicts'], [])
        self.assertEqual(next(p for p in c['organization']['projects'] if p['id'] == 'a')['name'], 'Resolved')
        self.assertEqual(a['digest'], self.converge()[0]['digest'])

    def test_join_does_not_merge_names_without_explicit_mapping(self):
        self.join('a', 'b')
        self.assertEqual(len(self.call('a', 'get')['organization']['projects']), 2)

    def test_explicit_initial_mapping_preserves_both_folder_locations_and_backup(self):
        self.join('a', 'b', {'a': 'b'})
        projects = self.call('a', 'get')['organization']['projects']
        self.assertEqual(len(projects), 1)
        self.assertEqual({f['path'] for f in projects[0]['folders']}, {'/a', '/b'})
        self.assertTrue(list((self.root / 'a/swarm').glob('backup-before-join-*.json')))

    def test_remote_payload_validation_keeps_store_unchanged(self):
        before = self.call('a', 'get')
        self.call('a', 'exchange', data=dict(swarm_id=before['swarm_id'], known=[], operations=[dict(id='bad')]), ok=False)
        self.assertEqual(before['digest'], self.call('a', 'get')['digest'])
        self.call('a', 'exchange', data=dict(swarm_id=self.call('b', 'get')['swarm_id'], known=[], operations=[]), ok=False)

    def test_removing_a_coalesced_folder_removes_observed_duplicates(self):
        before = self.call('a', 'get'); desired = copy.deepcopy(before['organization'])
        desired['projects'][0]['folders'][0].update(machine='b', machine_id='', path='/b')
        self.patch('a', before, desired)
        self.join('a', 'b', {'a': 'b'})
        before = self.call('a', 'get')
        self.assertEqual(len(before['organization']['projects'][0]['folders']), 1)
        desired = copy.deepcopy(before['organization']); desired['projects'][0]['folders'] = []
        self.patch('a', before, desired); self.call('a', 'sync')
        self.assertEqual(self.call('a', 'get')['organization']['projects'][0]['folders'], [])
        self.assertEqual(self.call('b', 'get')['organization']['projects'][0]['folders'], [])

    def test_move_and_copy_folder_links_replicate_without_moving_sessions(self):
        self.join('a', 'b')
        before = self.call('a', 'get'); desired = copy.deepcopy(before['organization'])
        source = next(p for p in desired['projects'] if p['id'] == 'a')
        target = next(p for p in desired['projects'] if p['id'] == 'b')
        source['sessions'] = ['a\ncodex/example/session']
        original = copy.deepcopy(source['folders'][0])
        session_addresses = {'a\ncodex/example/session', original['machine_id'] + '\ncodex/example/session'}
        duplicate = copy.deepcopy(original); duplicate['id'] = 'copied-folder'
        target['folders'].append(duplicate)
        destination = project('destination', 'Destination', 'a', '/unused')
        destination['folders'] = []; desired['projects'].append(destination)
        self.patch('a', before, desired); self.call('a', 'sync')
        for node in ('a', 'b'):
            groups = {p['id']: p for p in self.call(node, 'get')['organization']['projects']}
            self.assertEqual([f['id'] for f in groups['a']['folders']], [original['id']])
            copied = next(f for f in groups['b']['folders'] if f['id'] == 'copied-folder')
            self.assertEqual((copied['machine_id'], copied['path'], copied['name']),
                             (original['machine_id'], original['path'], original['name']))
            self.assertEqual(len(groups['a']['sessions']), 1)
            self.assertIn(groups['a']['sessions'][0], session_addresses)
        before = self.call('a', 'get'); desired = copy.deepcopy(before['organization'])
        groups = {p['id']: p for p in desired['projects']}
        groups['destination']['folders'] = groups['a']['folders']; groups['a']['folders'] = []
        self.patch('a', before, desired); self.call('a', 'sync')
        for node in ('a', 'b'):
            groups = {p['id']: p for p in self.call(node, 'get')['organization']['projects']}
            self.assertEqual(groups['a']['folders'], [])
            self.assertEqual([f['id'] for f in groups['destination']['folders']], [original['id']])
            self.assertEqual(len(groups['a']['sessions']), 1)
            self.assertIn(groups['a']['sessions'][0], session_addresses)
            self.assertEqual(len(groups['b']['folders']), 2)
        # Removing the moved link must leave its copied sibling intact on both nodes.
        before = self.call('a', 'get'); desired = copy.deepcopy(before['organization'])
        next(p for p in desired['projects'] if p['id'] == 'destination')['folders'] = []
        self.patch('a', before, desired); self.call('a', 'sync')
        for node in ('a', 'b'):
            groups = {p['id']: p for p in self.call(node, 'get')['organization']['projects']}
            self.assertEqual(groups['destination']['folders'], [])
            self.assertEqual({f['id'] for f in groups['b']['folders']}, {'folder-b', 'copied-folder'})

    def test_worker_syncs_without_a_gui_or_reverse_connection(self):
        self.join('a', 'b')
        self.assertEqual(self.call('b', 'get')['peers'], {})
        env = dict(self.env, HGS_CONFIG_DIR=str(self.root / 'a'), HGS_STATE_DIR=str(self.root / 'a/state'))
        worker = subprocess.Popen([str(HGS), 'swarm', 'worker'], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        try:
            before = self.call('a', 'get'); desired = copy.deepcopy(before['organization'])
            next(p for p in desired['projects'] if p['id'] == 'a')['name'] = 'Changed with GUI closed'
            self.patch('a', before, desired)
            deadline = time.monotonic() + 10
            while time.monotonic() < deadline:
                actual = self.call('b', 'get')
                if next(p for p in actual['organization']['projects'] if p['id'] == 'a')['name'] == 'Changed with GUI closed':
                    break
                time.sleep(.1)
            else:
                self.fail('headless worker did not replicate the change')
        finally:
            worker.terminate(); worker.communicate(timeout=5)

    def test_worker_is_reported_and_isolated_directories_never_restart_services(self):
        # Service-manager fakes record any attempt; hgs puts HOME/.local/bin first on PATH.
        home = self.root / 'home'
        (home / '.local/bin').mkdir(parents=True)
        for directory in (home / '.local/bin', self.root / 'bin'):
            for name in ('systemctl', 'launchctl'):
                fake = directory / name
                fake.write_text(f'#!/bin/sh\necho "{name} $*" >> "{self.root}/service-calls"\n'); fake.chmod(0o755)
        self.env['HOME'] = str(home)
        swarm = self.root / 'a/swarm'
        self.assertEqual(self.call('a', 'get')['worker'], {'running': False, 'current': False})
        with (swarm / 'worker.lock').open('a') as lock:
            fcntl.flock(lock, fcntl.LOCK_EX)
            # A released worker holds the lock without recording its version.
            self.assertEqual(self.call('a', 'get')['worker'], {'running': True, 'current': False})
            self.call('a', 'preference', 'set', 'mobile_delivery', 'away_5', '--json')
            inventory = self.call('a', 'inventory')
            self.call('a', 'exchange', data={'swarm_id': inventory['swarm_id'], 'known': [], 'operations': []})
            (swarm / 'worker.json').write_text(json.dumps({'version': '0.0.1', 'pid': os.getpid(), 'started_at': 1}))
            self.assertEqual(self.call('a', 'get')['worker'], {'running': True, 'current': False})
        # HGS_CONFIG_DIR is not the directory the service uses.
        self.assertFalse((self.root / 'service-calls').exists())
        self.assertFalse((swarm / 'worker-restart.json').exists())
        env = dict(self.env, HGS_CONFIG_DIR=str(self.root / 'a'), HGS_STATE_DIR=str(self.root / 'a/state'))
        worker = subprocess.Popen([str(HGS), 'swarm', 'worker'], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        try:
            deadline = time.monotonic() + 10
            while self.call('a', 'get')['worker'] != {'running': True, 'current': True} and time.monotonic() < deadline:
                time.sleep(.1)
            self.assertEqual(self.call('a', 'get')['worker'], {'running': True, 'current': True})
            marker = json.loads((swarm / 'worker.json').read_text())
            version = subprocess.run([str(HGS), '--version'], env=env, text=True, capture_output=True, check=True).stdout.split()[1]
            self.assertEqual((marker['version'], marker['pid']), (version, worker.pid))
            self.assertLessEqual(abs(marker['started_at'] - time.time()), 60)
        finally:
            worker.terminate(); worker.communicate(timeout=5)
        self.assertEqual(self.call('a', 'get')['worker'], {'running': False, 'current': False})
        self.assertFalse((self.root / 'service-calls').exists())

    def test_initial_join_can_keep_existing_swarm_session_placement(self):
        for node in ['a', 'b']:
            before = self.call(node, 'get'); desired = copy.deepcopy(before['organization'])
            desired['projects'][0]['sessions'] = ['b\ncodex/shared/session']
            self.patch(node, before, desired)
        self.join('a', 'b', prefer_peer=True)
        result = self.call('a', 'get')
        self.assertEqual(result['conflicts'], [])
        owner = next(p['id'] for p in result['organization']['projects'] if 'b\ncodex/shared/session' in p['sessions'])
        self.assertEqual(owner, 'b')

    def test_concurrent_delivery_choices_converge_without_conflict_review(self):
        self.join('a', 'b'); self.join('b', 'a'); self.join('c', 'b'); self.join('b', 'c')
        self.converge()
        self.call('a', 'preference', 'set', 'mobile_delivery', 'away_5', '--json')
        self.call('c', 'preference', 'set', 'mobile_delivery', 'immediate', '--json')
        snapshots = self.converge()
        values = {self.call(node, 'preference', 'get', '--json')['mobile_delivery'] for node in 'abc'}
        self.assertEqual(len(values), 1)
        self.assertIn(values.pop(), {'away_5', 'immediate'})
        for snapshot in snapshots:
            self.assertEqual(snapshot['conflicts'], [])
            self.assertEqual(snapshot['preferences'], snapshots[0]['preferences'])
        self.call('b', 'preference', 'set', 'mobile_delivery', 'away_30', '--json')
        self.converge()
        self.assertEqual({self.call(node, 'preference', 'get', '--json')['mobile_delivery'] for node in 'abc'}, {'away_30'})

    def test_joining_keeps_the_swarm_delivery_choice(self):
        self.call('b', 'preference', 'set', 'mobile_delivery', 'immediate', '--json')
        self.call('a', 'preference', 'set', 'mobile_delivery', 'away_5', '--json')
        self.join('a', 'b')
        self.assertEqual(self.call('a', 'preference', 'get', '--json'), {'mobile_delivery': 'immediate'})
        self.assertEqual(self.call('a', 'get')['conflicts'], [])
        self.call('c', 'preference', 'set', 'mobile_delivery', 'away_30', '--json')
        self.join('c', 'b')
        self.assertEqual(self.call('c', 'preference', 'get', '--json'), {'mobile_delivery': 'immediate'})

    def test_older_members_keep_project_sync_without_the_delivery_choice(self):
        self.join('a', 'b'); self.join('b', 'a'); self.join('c', 'b'); self.join('b', 'c')
        self.converge()
        (self.root / 'old-c').touch()  # c now answers b like a release without shared preferences
        self.call('a', 'preference', 'set', 'mobile_delivery', 'away_10', '--json')
        for node in ('a', 'b', 'b'):
            self.assertTrue(all(row['ok'] for row in self.call(node, 'sync').values()), node)
        self.assertEqual(self.call('b', 'preference', 'get', '--json'), {'mobile_delivery': 'away_10'})
        self.assertEqual(self.call('c', 'preference', 'get', '--json'), {'mobile_delivery': 'away'})
        before = self.call('b', 'get')
        desired = copy.deepcopy(before['organization'])
        next(p for p in desired['projects'] if p['id'] == 'b')['name'] = 'Renamed after the preference'
        self.patch('b', before, desired)
        self.assertTrue(self.call('b', 'sync')['c']['ok'])
        projects = self.call('c', 'get')['organization']['projects']
        self.assertEqual(next(p for p in projects if p['id'] == 'b')['name'], 'Renamed after the preference')
        peers = self.call('b', 'get')['peers']
        self.assertTrue(peers['c']['catalog_outdated'])
        self.assertNotIn('catalog_outdated', peers['a'])
        # An older hgs on b itself still reads b's catalog: peers keep their released fields.
        stored = json.loads((self.root / 'b/swarm/catalog.json').read_text())
        self.assertEqual({field for peer in stored['peers'].values() for field in peer}, {'node', 'last_sync', 'error'})
        # A legacy requester omits accepted kinds and receives only legacy kinds.
        inventory = self.call('b', 'inventory')
        response = self.call('b', 'exchange', data={'swarm_id': inventory['swarm_id'], 'known': [], 'operations': []})
        self.assertTrue(response['operations'])
        self.assertNotIn('preference', {op['key']['kind'] for op in response['operations']})
        # After the update the member receives the choice and the notice clears.
        (self.root / 'old-c').unlink()
        self.assertTrue(self.call('b', 'sync')['c']['ok'])
        self.assertEqual(self.call('c', 'preference', 'get', '--json'), {'mobile_delivery': 'away_10'})
        self.assertNotIn('catalog_outdated', self.call('b', 'get')['peers']['c'])


if __name__ == '__main__':
    unittest.main()
