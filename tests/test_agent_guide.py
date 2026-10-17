"""Agent guide text and verified self references, in private state without real agents."""
import hashlib
import json
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest
from urllib.parse import parse_qs, urlsplit

HGS = Path(os.environ.get('HGS_TEST_BIN', Path(__file__).resolve().parents[1] / 'target/debug/hgs')).resolve()


class AgentGuide(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='zerus-guide-')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name).resolve()
        self.state = self.root / 'state'; self.state.mkdir()
        self.home = self.root / 'home'; self.home.mkdir()
        sockets = self.root / 'sockets'; sockets.mkdir()
        self.env = {k: v for k, v in os.environ.items() if not k.startswith('HGS_')}
        self.env.update(HOME=str(self.home), USERPROFILE=str(self.home), TMUX_TMPDIR=str(sockets),
                        HGS_CONFIG_DIR=str(self.root / 'config'), HGS_STATE_DIR=str(self.state),
                        HGS_SELF='fixture', HGS_PEERS='peer')
        (self.state / 'local-sessions').touch()

    def run_hgs(self, *args):
        return subprocess.run([str(HGS), *args], env=self.env, capture_output=True, text=True, timeout=10)

    def call(self, *args, success=True):
        result = self.run_hgs(*args)
        if success:
            self.assertEqual(result.returncode, 0, result.stderr)
            return result.stdout
        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertEqual(result.stdout, '')
        return result.stderr

    def test_index_and_topics_are_small_and_exact(self):
        index = self.call('guide')
        self.assertIn('hgs self --json', index)
        self.assertLessEqual(len(index.encode()), 2400)
        for topic in ('send', 'wait', 'launch', 'stop'):
            self.assertIn('hgs guide ' + topic, index)
            self.assertIn('(hgs guide %s)' % topic, self.call('guide', topic))
        self.assertIn('unknown guide topic', self.call('guide', 'kill', success=False))
        self.call('guide', 'send', 'wait', success=False)
        self.assertIn('hgs [@host] guide', self.call('--help'))

    @unittest.skipIf(os.name == 'nt', 'POSIX remote-shell compatibility contract')
    def test_peer_guide_is_probed_and_never_launches_on_an_old_peer(self):
        peer = self.home / '.local/bin/hgs'; peer.parent.mkdir(parents=True)
        peer.write_text('#!/bin/sh\nif [ "$1" = --help ]; then echo "hgs dirs [path]"; exit 0; fi\n'
                        'echo unexpected > "$HOME/peer-launched"\nexit 99\n')
        peer.chmod(0o700)
        ssh = self.root / 'fake-ssh'
        ssh.write_text('#!/usr/bin/env python3\nimport os,sys\nos.execv("/bin/sh",["sh","-c",sys.argv[-1]])\n')
        ssh.chmod(0o700); self.env['HGS_SSH'] = str(ssh)
        self.assertIn('no agent guide', self.call('@peer', 'guide', success=False))
        self.assertFalse((self.home / 'peer-launched').exists())
        peer.write_text('#!/bin/sh\nif [ "$1" = --help ]; then echo "hgs [@host] guide [topic]"; exit 0; fi\n'
                        'exec ' + shlex.quote(str(HGS)) + ' "$@"\n')
        self.assertEqual(self.call('@peer', 'guide', 'stop'), self.call('guide', 'stop'))

    def write(self, record):
        path = self.state / (hashlib.sha256(record['name'].encode()).hexdigest() + '.json')
        path.write_text(json.dumps(record))
        return path

    @unittest.skipIf(os.name == 'nt', 'POSIX native-process ancestry fixture')
    def test_self_is_proven_by_ancestry_and_follows_renames(self):
        start = subprocess.check_output(['ps', '-p', str(os.getpid()), '-o', 'lstart='],
                                        env=dict(self.env, LC_ALL='C', TZ='UTC'), text=True).strip()
        record = dict(version=1, name='claude/example/review', agent='claude', run_id='run-one',
                      conversation_id='conversation-one', account_id='fixture', subagents={},
                      pid=os.getpid(), process_start=start)
        old = self.write(record)
        self.assertIn('no live Zerus session', self.call('self', '--json', success=False))
        self.env.update(HGS_SESSION=record['name'], HGS_RUN_ID='run-one')
        own = json.loads(self.call('self', '--json'))
        self.assertEqual((own['name'], own['run_id'], own['conversation_id'], own['provider'], own['host']),
                         (record['name'], 'run-one', 'conversation-one', 'claude', 'fixture'))
        fields = {key: value[0] for key, value in parse_qs(urlsplit(own['uri']).query).items()}
        self.assertEqual((fields['machine'], fields['name'], fields['run'], fields['conversation']),
                         (own['machine_id'], record['name'], 'run-one', 'conversation-one'))
        self.assertEqual(self.call('self').strip(), own['uri'])
        # A rename keeps the agent's environment; the run identity finds the binding.
        old.unlink()
        renamed = dict(record, name='claude/example/deploy')
        self.write(renamed)
        self.assertEqual(json.loads(self.call('self', '--json'))['name'], 'claude/example/deploy')
        # A pre-upgrade runner's shadow under the old name also resolves forward.
        self.write(dict(record, rename_shadow=True, rename_target=renamed['name']))
        self.assertEqual(json.loads(self.call('self', '--json'))['name'], 'claude/example/deploy')
        old.unlink()
        # Environment labels without the owned process prove nothing.
        self.write(dict(renamed, process_start='not the owned process'))
        self.call('self', '--json', success=False)
        self.call('@peer', 'self', '--json', success=False)
        self.call('--dry-run', 'self', success=False)
        self.call('self', '--yaml', success=False)


if __name__ == '__main__':
    unittest.main()
