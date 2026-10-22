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
if (root / ('preference-' + alias)).exists():
    # A member from before the shared project order: it lists the kinds it
    # accepts, without "order", and rejects that kind.
    request = sys.stdin.read()
    if args[1:] == ['exchange'] and any(op['key']['kind'] == 'order' for op in json.loads(request)['operations']):
        print('unknown variant from a newer member', file=sys.stderr); sys.exit(1)
    result = subprocess.run([os.environ['SWARM_TEST_HGS'], *args], input=request, env=env, text=True, capture_output=True)
    sys.stderr.write(result.stderr)
    if result.returncode: sys.exit(result.returncode)
    output = json.loads(result.stdout)
    if 'kinds' in output: output['kinds'] = [kind for kind in output['kinds'] if kind != 'order']
    print(json.dumps(output)); sys.exit(0)
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

    def test_observation_and_concurrent_inbox_cannot_undo_concrete_projects(self):
        before = self.call('a', 'get'); desired = copy.deepcopy(before['organization'])
        desired['projects'].append(dict(id='ungrouped', name='Inbox', color='#123456', folders=[], sessions=[]))
        self.patch('a', before, desired)
        self.join('a', 'b'); self.join('b', 'a')

        def sync():
            for node in ('a', 'b', 'a'):
                self.call(node, 'sync')
            snapshots = [self.call(node, 'get') for node in ('a', 'b')]
            self.assertEqual(snapshots[0]['digest'], snapshots[1]['digest'])
            return snapshots

        def assignment(snapshot, session, project_id):
            desired = copy.deepcopy(snapshot['organization'])
            desired['session_assignments_explicit'] = True
            for project in desired['projects']:
                project['sessions'] = [s for s in project['sessions'] if s != session]
                if project['id'] == project_id:
                    project['sessions'].append(session)
            return desired

        # Qt's firstObservationDoesNotRaceAnotherDesktopAssignment verifies
        # that real GUI initialize/apply payloads omit passive memberships.
        # Replay that contract through isolated CLI nodes in both write orders.
        for observer_first in (True, False):
            a, b = sync()
            session = f'a\ncodex/example/launch-{int(observer_first)}'
            observed = copy.deepcopy(a['organization'])
            observed['session_assignments_explicit'] = True
            observed['runs'] = {'a\nrun-one': session}  # Local presentation only.
            launched = assignment(b, session, 'b')
            for node, base, desired in ([('a', a, observed), ('b', b, launched)] if observer_first
                                        else [('b', b, launched), ('a', a, observed)]):
                result = self.patch(node, base, desired)
                if node == 'a':
                    self.assertEqual(result['written'], {})
            for snapshot in sync():
                self.assertEqual(snapshot['conflicts'], [])
                self.assertIn(session, next(p for p in snapshot['organization']['projects'] if p['id'] == 'b')['sessions'])

        # A concurrent Inbox choice loses to one known concrete project. The
        # observed versions still include it, so a later explicit Inbox move wins.
        a, b = sync(); session = 'a\ncodex/example/explicit-choice'
        self.patch('b', b, assignment(b, session, 'b'))
        sync()
        self.patch('a', a, assignment(a, session, 'ungrouped'))
        a, b = sync()
        self.assertEqual(a['conflicts'], [])
        self.assertIn(session, next(p for p in a['organization']['projects'] if p['id'] == 'b')['sessions'])
        self.patch('a', a, assignment(a, session, 'ungrouped'))
        for snapshot in sync():
            self.assertEqual(snapshot['conflicts'], [])
            self.assertIn(session, next(p for p in snapshot['organization']['projects'] if p['id'] == 'ungrouped')['sessions'])
        # Neither the local fallback marker nor the additive intent hint is a
        # new shared operation or stored catalog field for resident peers.
        exported = json.dumps(self.call('a', 'export'))
        self.assertNotIn('unassigned_sessions', exported)
        self.assertNotIn('session_assignments_explicit', exported)

    def test_additive_assignment_hint_keeps_legacy_catalog_operations(self):
        # Also run this contract against a retained pre-fix hgs binary: the hint
        # is organization metadata, not a new Patch field or operation kind.
        before = self.call('a', 'get'); desired = copy.deepcopy(before['organization'])
        desired['session_assignments_explicit'] = True
        desired['projects'].append(dict(id='ungrouped', name='Inbox', color='#123456', folders=[], sessions=[]))
        desired['projects'][0]['sessions'] = ['a\ncodex/example/review']
        saved = self.patch('a', before, desired)
        desired = copy.deepcopy(saved['organization']); desired['session_assignments_explicit'] = True
        for project in desired['projects']:
            project['sessions'] = ['a\ncodex/example/review'] if project['id'] == 'ungrouped' else []
        saved = self.patch('a', saved, desired)
        self.assertEqual(saved['conflicts'], [])
        self.assertEqual(next(p for p in saved['organization']['projects'] if p['id'] == 'ungrouped')['sessions'], ['a\ncodex/example/review'])
        exported = json.dumps(self.call('a', 'export'))
        self.assertNotIn('session_assignments_explicit', exported)
        self.assertNotIn('unassigned_sessions', exported)

    def test_explicit_initial_mapping_preserves_both_folder_locations_and_backup(self):
        self.join('a', 'b', {'a': 'b'})
        projects = self.call('a', 'get')['organization']['projects']
        self.assertEqual(len(projects), 1)
        self.assertEqual({f['path'] for f in projects[0]['folders']}, {'/a', '/b'})
        self.assertTrue(list((self.root / 'a/swarm').glob('backup-before-join-*.json')))

    def test_local_windows_folders_never_reach_the_catalog(self):
        # A desktop keeps its local Windows folders (D15); an older or newer GUI
        # that sends one still applies, and neither node's catalog shows it.
        self.join('a', 'b')
        before = self.call('a', 'get'); desired = copy.deepcopy(before['organization'])
        project_a = next(p for p in desired['projects'] if p['id'] == 'a')
        project_a['name'] = 'Renamed beside Windows folders'
        # This machine by its alias, or by its ID once a snapshot named it.
        project_a['folders'] += [dict(id='drive', machine='a', path='C:\\work', name='Drive'),
                                 dict(id='unc', machine='a', path='//server/share/dir', name='Share'),
                                 dict(id='by-id', machine='desktop', machine_id=before['node_id'], path='c:/Users/user/project', name='')]
        windows = copy.deepcopy(desired)
        self.patch('a', before, desired); self.call('a', 'sync')
        for node in ('a', 'b'):
            projects = {p['id']: p for p in self.call(node, 'get')['organization']['projects']}
            self.assertEqual(projects['a']['name'], 'Renamed beside Windows folders')
            self.assertEqual([f['path'] for f in projects['a']['folders']], ['/a'])
        exported = json.dumps(self.call('a', 'export'))
        for private in ['C:\\\\work', '//server/share', 'Users/user']:
            self.assertNotIn(private, exported)
        # Removing them again is no change; other invalid paths, a peer's drive
        # path among them, still fail.
        saved = self.call('a', 'get'); desired = copy.deepcopy(saved['organization'])
        self.patch('a', dict(saved, organization=windows), desired)
        self.assertEqual(saved['digest'], self.call('a', 'get')['digest'])
        for folder in [dict(id='relative', machine='a', path='work', name=''),
                       dict(id='peer-drive', machine='b', path='C:\\work', name=''),
                       dict(id='unknown', machine='windows-desktop', path='c:/Users/user/project', name='')]:
            failing = copy.deepcopy(desired)
            next(p for p in failing['projects'] if p['id'] == 'a')['folders'].append(folder)
            self.call('a', 'apply', data=dict(base=saved['organization'], desired=failing, versions=saved['versions']), ok=False)
        self.assertEqual(saved['digest'], self.call('a', 'get')['digest'])
        self.assertNotIn('windows-desktop', json.dumps(self.call('a', 'export')))

    def test_a_peers_unc_shaped_folder_syncs_as_a_posix_path(self):
        # A Windows GUI can store "//server/share" for a POSIX peer. The catalog
        # holds it like any folder, so renaming and removing it sync.
        self.join('a', 'b')
        def folders(node):
            return [f for p in self.call(node, 'get')['organization']['projects'] if p['id'] == 'a' for f in p['folders']]
        def edit(change):
            before = self.call('a', 'get'); desired = copy.deepcopy(before['organization'])
            change(next(p for p in desired['projects'] if p['id'] == 'a')['folders'])
            self.patch('a', before, desired); self.call('a', 'sync')
        edit(lambda items: items.append(dict(id='unc', machine='b', path='//server/share/dir', name='')))
        node_b = self.call('b', 'get')['node_id']
        for node in ('a', 'b'):
            unc = next(f for f in folders(node) if f['id'] == 'unc')
            self.assertEqual((unc['path'], unc['machine_id']), ('//server/share/dir', node_b))
        edit(lambda items: next(f for f in items if f['id'] == 'unc').update(name='Share'))
        self.assertEqual(next(f for f in folders('b') if f['id'] == 'unc')['name'], 'Share')
        edit(lambda items: items.remove(next(f for f in items if f['id'] == 'unc')))
        for node in ('a', 'b'):
            self.assertEqual([f['path'] for f in folders(node)], ['/a'])

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

    def test_join_maps_membership_before_comparing_and_concrete_beats_peer_inbox(self):
        session = 'b\ncodex/shared/mapped'
        for node in ['a', 'b']:
            before = self.call(node, 'get'); desired = copy.deepcopy(before['organization'])
            desired['projects'][0]['sessions'] = [session]
            self.patch(node, before, desired)
        self.join('a', 'b', mapping={'a': 'b'}, prefer_peer=False)
        result = self.call('a', 'get')
        self.assertEqual(result['conflicts'], [])
        self.assertIn(session, next(p for p in result['organization']['projects'] if p['id'] == 'b')['sessions'])
        exported = self.call('a', 'export')['catalog']['operations'].values()
        heads = [o for o in exported if o['key'].get('session') == session.split('\n', 1)[1]]
        self.assertTrue(all(o['value'] == 'b' for o in heads))

    def test_concrete_local_project_beats_peer_inbox_even_when_keeping_peer_placement(self):
        session = 'b\ncodex/shared/concrete'
        for node in ['a', 'b']:
            before = self.call(node, 'get'); desired = copy.deepcopy(before['organization'])
            if node == 'b':
                desired['projects'].append(dict(id='ungrouped',name='Inbox',color='#123456',folders=[],sessions=[session]))
            else:
                desired['projects'][0]['sessions'] = [session]
            self.patch(node, before, desired)
        self.join('a', 'b', prefer_peer=True)
        result = self.call('a', 'get')
        self.assertEqual(result['conflicts'], [])
        self.assertIn(session, next(p for p in result['organization']['projects'] if p['id'] == 'a')['sessions'])

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


    def order(self, node, ids):
        before = self.call(node, 'get')
        desired = copy.deepcopy(before['organization'])
        projects = {p['id']: p for p in desired['projects']}
        desired['projects'] = [projects[id] for id in ids] + [p for p in desired['projects'] if p['id'] not in ids]
        desired['project_order'] = [p['id'] for p in desired['projects']]
        return self.patch(node, before, desired)

    def shown(self, node):
        organization = self.call(node, 'get')['organization']
        return [p['id'] for p in organization['projects']], organization.get('project_order')

    def test_the_project_order_is_shared_by_every_member(self):
        self.join('a', 'b'); self.join('b', 'a'); self.join('c', 'b'); self.join('b', 'c')
        self.converge()
        self.assertEqual(self.shown('a'), (['a', 'b', 'c'], None))
        self.order('c', ['c', 'a', 'b'])
        snapshots = self.converge()
        for node in 'abc':
            self.assertEqual(self.shown(node), (['c', 'a', 'b'], ['c', 'a', 'b']), node)
        self.assertEqual({len(s['conflicts']) for s in snapshots}, {0})
        # A desktop without the shared order applies its own order as before;
        # the swarm's order stays.
        before = self.call('a', 'get')
        desired = copy.deepcopy(before['organization'])
        desired.pop('project_order')
        desired['projects'].reverse()
        next(p for p in desired['projects'] if p['id'] == 'a')['name'] = 'Renamed by an older desktop'
        self.patch('a', before, desired)
        self.converge()
        self.assertEqual(self.shown('b'), (['c', 'a', 'b'], ['c', 'a', 'b']))

    def test_joining_keeps_the_swarm_project_order(self):
        self.order('b', ['b'])
        self.order('a', ['a'])
        self.join('a', 'b')
        self.assertEqual(self.shown('a'), (['b', 'a'], ['b', 'a']))

    def test_a_joiner_brings_its_mapped_order_to_a_swarm_without_one(self):
        before = self.call('c', 'get')
        desired = copy.deepcopy(before['organization'])
        desired['projects'].append(project('d', 'Other', 'c', '/d'))
        desired['project_order'] = ['c', 'd']
        self.patch('c', before, desired)
        # c's project c becomes b's project b; unmapped, d would come first.
        self.join('c', 'b', mapping={'c': 'b'})
        self.assertEqual(self.shown('c'), (['b', 'd'], ['b', 'd']))

    def test_members_from_before_the_shared_order_keep_syncing_projects(self):
        self.join('a', 'b'); self.join('b', 'a'); self.join('c', 'b'); self.join('b', 'c')
        self.converge()
        (self.root / 'preference-c').touch()  # c answers b like a release without the shared order
        self.order('a', ['b', 'c', 'a'])
        self.call('a', 'preference', 'set', 'mobile_delivery', 'away_10', '--json')
        for node in ('a', 'b', 'b'):
            self.assertTrue(all(row['ok'] for row in self.call(node, 'sync').values()), node)
        self.assertEqual(self.shown('b'), (['b', 'c', 'a'], ['b', 'c', 'a']))
        self.assertEqual(self.shown('c'), (['a', 'b', 'c'], None))
        self.assertEqual(self.call('c', 'preference', 'get', '--json'), {'mobile_delivery': 'away_10'})
        self.assertNotIn('catalog_outdated', self.call('b', 'get')['peers']['c'])
        (self.root / 'preference-c').unlink()
        self.assertTrue(self.call('b', 'sync')['c']['ok'])
        self.assertEqual(self.shown('c'), (['b', 'c', 'a'], ['b', 'c', 'a']))


if __name__ == '__main__':
    unittest.main()
