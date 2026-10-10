#!/usr/bin/env python3
"""Catalog replication through the real CLI, with isolated nodes and fake SSH."""
import copy
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
import os, pathlib, shlex, sys
root = pathlib.Path(os.environ['SWARM_TEST_ROOT'])
alias = sys.argv[-2]
if (root / ('offline-' + alias)).exists():
    print('fixture offline', file=sys.stderr); sys.exit(255)
env = dict(os.environ, HGS_CONFIG_DIR=str(root / alias), HGS_STATE_DIR=str(root / alias / 'state'))
args = shlex.split(sys.argv[-1])[1:]
with (root / 'calls').open('a') as file: file.write(alias + ' ' + ' '.join(args) + '\\n')
os.execve(os.environ['SWARM_TEST_HGS'], [os.environ['SWARM_TEST_HGS'], *args], env)
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


if __name__ == '__main__':
    unittest.main()
