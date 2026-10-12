#!/usr/bin/env python3
"""Overmind replication through the swarm worker, with isolated nodes and fake SSH."""
import json
import os
from pathlib import Path
import subprocess
import tempfile
import time
import unittest

import test_swarm as swarm

# Like the swarm fixture, plus a marker that hides the overmind feature of a peer.
SSH = swarm.SSH.replace(
    "if not (root / ('old-' + alias)).exists():",
    "if args[1:] == ['hello'] and (root / ('no-overmind-' + alias)).exists():\n"
    "    result = subprocess.run([os.environ['SWARM_TEST_HGS'], *args], env=env, text=True, capture_output=True)\n"
    "    output = json.loads(result.stdout); output.pop('features', None)\n"
    "    print(json.dumps(output)); sys.exit(0)\n"
    "if not (root / ('old-' + alias)).exists():", 1)
assert 'no-overmind-' in SSH


class Replication(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory(prefix='hgs-overmind-sync-')
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name)
        (self.root / 'bin').mkdir()
        ssh = self.root / 'bin/ssh'; ssh.write_text(SSH); ssh.chmod(0o755)
        self.env = dict(os.environ, SWARM_TEST_ROOT=str(self.root), SWARM_TEST_HGS=str(swarm.HGS),
                        PATH=str(self.root / 'bin') + ':' + os.environ['PATH'], HGS_TAB='0')
        for key in ['HGS_SELF', 'HGS_PEERS', 'HGS_EXECUTABLE', 'TMUX', 'TMUX_PANE', 'HGS_RUN_ID', 'HGS_SESSION']:
            self.env.pop(key, None)
        for node, peer in [('a', 'b'), ('b', 'a')]:
            (self.root / node).mkdir()
            (self.root / node / 'config').write_text(f'HGS_SELF="{node}"\nHGS_PEERS="{peer}"\n')
            (self.root / node / 'machines.local.json').write_text(json.dumps({'machines': {peer: dict(hostname='host.invalid', user='u', identity_file='/k', enabled=True)}}))
            self.call(node, 'initialize', data={'projects': [swarm.project(node, 'Same name', node, '/' + node)]})
        for node, peer in [('a', 'b'), ('b', 'a')]:
            preview = self.call(node, 'preview', peer)
            self.call(node, 'join', peer, data=dict(node_id=preview['node_id'], swarm_id=preview['swarm_id'], project_map={}, prefer_peer_memberships=False))

    def node_env(self, node):
        return dict(self.env, HGS_CONFIG_DIR=str(self.root / node), HGS_STATE_DIR=str(self.root / node / 'state'))

    def call(self, node, *args, data=None):
        result = subprocess.run([str(swarm.HGS), 'swarm', *args], input=json.dumps(data) if data is not None else None,
                                env=self.node_env(node), text=True, capture_output=True, timeout=40)
        self.assertEqual(result.returncode, 0, result.stderr)
        return json.loads(result.stdout)

    def overmind(self, node, *args, data=None):
        result = subprocess.run([str(swarm.HGS), 'overmind', *args], input=None if data is None else json.dumps(data),
                                env=self.node_env(node), text=True, capture_output=True, timeout=40)
        self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
        return json.loads(result.stdout)

    def refs(self, node):
        return sorted(t['ref'] for t in self.overmind(node, 'board', '--json', data={'project': 'demo'})['tasks'])

    def test_sync_replicates_both_ways_over_one_direction(self):
        made_on_a = self.overmind('a', 'create', '--json', data={'project': 'demo', 'prompt': 'from a'})['task']['ref']
        made_on_b = self.overmind('b', 'create', '--json', data={'project': 'demo', 'prompt': 'from b'})['task']['ref']
        self.call('a', 'sync')  # only a connects
        self.assertEqual(self.refs('a'), self.refs('b'))
        self.assertEqual(set(self.refs('a')), {made_on_a, made_on_b})
        peers = self.overmind('a', 'status')['peers']
        self.assertEqual([(p['alias'], p['last_error'], p['behind']) for p in peers], [('b', '', 0)])

    def test_peers_without_overmind_are_skipped(self):
        before = self.overmind('a', 'status')['peers']  # setUp joins already exchanged once
        (self.root / 'calls').write_text('')
        (self.root / 'no-overmind-b').write_text('')
        self.overmind('a', 'create', '--json', data={'project': 'demo', 'prompt': 'x'})
        self.call('a', 'sync')
        calls = (self.root / 'calls').read_text()
        self.assertNotIn('overmind-exchange', calls)
        after = self.overmind('a', 'status')['peers']  # `behind` grows with local events, the exchange record must not change
        self.assertEqual([(p['alias'], p['last_ok'], p['updated']) for p in after], [(p['alias'], p['last_ok'], p['updated']) for p in before])

    def test_offline_peer_is_reported_and_recovers(self):
        self.overmind('a', 'create', '--json', data={'project': 'demo', 'prompt': 'x'})
        self.call('a', 'sync')
        (self.root / 'offline-b').write_text('')
        self.overmind('a', 'create', '--json', data={'project': 'demo', 'prompt': 'y'})
        self.call('a', 'sync')  # the catalog sync reports b offline; Overmind keeps its last good state
        (self.root / 'offline-b').unlink()
        self.call('a', 'sync')
        self.assertEqual(self.refs('a'), self.refs('b'))
        self.assertEqual(len(self.refs('b')), 2)

    def test_the_worker_exchanges_soon_after_a_local_write(self):
        worker = subprocess.Popen([str(swarm.HGS), 'swarm', 'worker'], env=self.node_env('a'),
                                  stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        try:
            time.sleep(1)
            ref = self.overmind('a', 'create', '--json', data={'project': 'demo', 'prompt': 'x'})['task']['ref']
            deadline = time.monotonic() + 15
            while time.monotonic() < deadline and ref not in self.refs('b'):
                time.sleep(.2)
            self.assertIn(ref, self.refs('b'))
        finally:
            worker.terminate(); worker.communicate(timeout=5)


if __name__ == '__main__':
    unittest.main()
