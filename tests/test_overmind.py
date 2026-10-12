#!/usr/bin/env python3
"""Overmind tasks through the real CLI with isolated state."""
import hashlib
import json
import os
from pathlib import Path
import sqlite3
import subprocess
import tempfile
import unittest
import uuid

import test_input as fixtures

REPO = Path(__file__).resolve().parents[1]
HGS = Path(os.environ.get('HGS_TEST_BIN', REPO / 'target/debug/hgs')).resolve()


class Overmind(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory(prefix='hgs-overmind-')
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name)
        (self.root / 'bin').mkdir()
        (self.root / 'config').mkdir()
        tmux = self.root / 'bin/tmux'
        tmux.write_text('#!/bin/sh\nif [ "$1" = -V ]; then echo "tmux 3.7"; fi\n')
        tmux.chmod(0o755)
        self.state = self.root / 'state'
        self.env = dict(os.environ, HOME=str(self.root), HGS_CONFIG_DIR=str(self.root / 'config'),
                        HGS_STATE_DIR=str(self.state), HGS_TAB='0',
                        PATH=str(self.root / 'bin') + ':' + os.environ['PATH'])
        for key in ('HGS_PEERS', 'HGS_SELF', 'TMUX', 'TMUX_PANE', 'HGS_RUN_ID', 'HGS_SESSION', 'HGS_EXECUTABLE'):
            self.env.pop(key, None)

    def run_hgs(self, *args, data=None, env=None):
        return subprocess.run([str(HGS), 'overmind', *args], input=None if data is None else json.dumps(data),
                              env=env or self.env, text=True, capture_output=True, timeout=20)

    def ok(self, *args, data=None):
        result = self.run_hgs(*args, data=data)
        self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
        return json.loads(result.stdout)

    def refused(self, *args, data=None, env=None):
        result = self.run_hgs(*args, data=data, env=env)
        self.assertEqual(result.returncode, 1, result.stdout)
        self.assertTrue(result.stderr.startswith('hgs: '), result.stderr)
        body = json.loads(result.stdout)
        self.assertFalse(body['ok'])
        return body['error']

    def test_create_board_show_and_move(self):
        created = self.ok('create', '--json', data={'project': 'demo', 'prompt': 'Fix drafts\nmore', 'stage': 'Queue'})
        ref = created['task']['ref']
        self.assertTrue(ref.startswith('om-'))
        board = self.ok('board', '--json', data={'project': 'demo'})
        self.assertEqual([t['ref'] for t in board['tasks']], [ref])
        self.assertEqual(self.ok('show', ref)['task']['prompt'], 'Fix drafts\nmore')
        self.assertEqual(self.ok('move', '--json', data={'task': ref, 'to': 'Backlog'})['task']['stage'], 'Backlog')
        self.assertEqual(self.refused('done', '--json', data={'task': ref})['code'], 'transition_not_allowed')

    def test_store_is_private(self):
        self.ok('create', '--json', data={'project': 'demo', 'prompt': 'x'})
        directory = self.state / 'overmind'
        self.assertEqual(directory.stat().st_mode & 0o777, 0o700)
        self.assertEqual((directory / 'overmind.sqlite3').stat().st_mode & 0o777, 0o600)
        self.assertEqual(int((directory / 'rev').read_text()), self.ok('rev')['cursor'])

    def test_receipts_replay(self):
        request = {'request_id': '0b9c5c1e-34c4-4d5e-9a64-1d2a3b4c5d6e', 'project': 'demo', 'prompt': 'x'}
        first = self.ok('create', '--json', data=request)
        self.assertEqual(self.ok('create', '--json', data=request), first)
        self.assertEqual(self.refused('create', '--json', data={**request, 'prompt': 'y'})['code'], 'request_reused')

    def test_malformed_json_is_a_refusal(self):
        result = subprocess.run([str(HGS), 'overmind', 'create', '--json'], input='{', env=self.env,
                                text=True, capture_output=True, timeout=20)
        self.assertEqual(result.returncode, 1, result.stdout)
        body = json.loads(result.stdout)
        self.assertFalse(body['ok'])
        self.assertEqual(body['error']['code'], 'invalid_request')
        self.assertTrue(result.stderr.startswith('hgs: '))

    def test_help_creates_no_state(self):
        self.assertEqual(self.run_hgs('--help').returncode, 0)
        self.assertFalse(self.state.exists())

    def test_invalid_requests_and_help(self):
        self.assertEqual(self.refused('create', '--json', data={'project': 'demo', 'prompt': 'x', 'unknown': 1})['code'],
                         'invalid_request')
        self.assertEqual(self.refused('create', '--json', data={'project': 'demo', 'prompt': '   '})['field'], 'prompt')
        result = self.run_hgs('--help')
        self.assertEqual(result.returncode, 0)
        self.assertIn('workflow set', result.stdout)
        unknown = self.refused('frobnicate')
        self.assertEqual(unknown['code'], 'unknown_command')
        self.assertIn('unknown overmind command', unknown['message'])

    def test_json_flag_anywhere_and_show_by_reference(self):
        ref = self.ok('create', '--json', data={'project': 'demo', 'prompt': 'x'})['task']['ref']
        self.assertEqual(self.ok('workflow', '--json', 'get', data={'project': 'demo'})['project'], 'demo')
        self.assertEqual(self.ok('--json', 'board', data={'project': 'demo'})['tasks'][0]['ref'], ref)
        # `show <ref>` never reads stdin, even with --json.
        result = subprocess.run([str(HGS), 'overmind', 'show', ref, '--json'], input='{', env=self.env,
                                text=True, capture_output=True, timeout=20)
        self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
        self.assertEqual(json.loads(result.stdout)['task']['ref'], ref)

    def test_session_environment_must_match_a_live_session(self):
        for extra in ({'HGS_SESSION': 'claude/demo/w1', 'HGS_RUN_ID': 'run-that-does-not-exist'},
                      {'HGS_SESSION': 'claude/demo/w1'}, {'HGS_RUN_ID': 'run-that-does-not-exist'}):
            error = self.refused('mine', env=dict(self.env, **extra))
            self.assertEqual(error['code'], 'session_mismatch', extra)
            self.assertIn('does not match a live Zerus session', error['message'])

    def test_agents_cannot_reach_another_machine(self):
        marker = self.root / 'ssh-called'
        ssh = self.root / 'bin/ssh'
        ssh.write_text(f'#!/bin/sh\ntouch {marker}\nexit 255\n')
        ssh.chmod(0o755)
        env = dict(self.env, HGS_SESSION='claude/demo/w1', HGS_PEERS='example-peer')
        result = subprocess.run([str(HGS), '@example-peer', 'overmind', 'board', '--json'], input='{}', env=env,
                                text=True, capture_output=True, timeout=20)
        self.assertEqual(result.returncode, 1, result.stdout)
        self.assertIn('agents use hgs overmind on their own machine; @host is for people', result.stderr)
        self.assertFalse(marker.exists())

    def test_people_cannot_use_agent_commands(self):
        created = self.ok('create', '--json', data={'project': 'demo', 'prompt': 'x', 'stage': 'Queue'})
        error = self.refused('advance', '--json', data={'task': created['task']['ref'], 'to': 'Build', 'spec': 's'})
        self.assertEqual(error['code'], 'agent_only')

    def rename(self, old, new, archive=False):
        self.state.mkdir(mode=0o700, exist_ok=True)
        binding = {'version': 1, 'name': old, 'agent': 'codex', 'run_id': str(uuid.uuid4())}
        if archive:
            binding['archive_id'] = str(uuid.uuid4())
            (self.state / 'archives').mkdir(mode=0o700, exist_ok=True)
            (self.state / 'archives' / f"{binding['archive_id']}.json").write_text(json.dumps(binding))
        else:
            (self.state / f'{hashlib.sha256(old.encode()).hexdigest()}.json').write_text(json.dumps(binding))
        command = [str(HGS), 'rename', old, new, *(['--archive', binding['archive_id']] if archive else [])]
        result = subprocess.run(command, env=self.env, text=True, capture_output=True, timeout=20)
        self.assertEqual((result.returncode, result.stderr), (0, ''))

    def test_pins_follow_a_renamed_session(self):
        pinned = {'machine': '', 'name': 'codex/demo/old'}
        ref = self.ok('create', '--json', data={'project': 'demo', 'prompt': 'x', 'pinned': pinned})['task']['ref']
        # An archived conversation may share the name of a live session; its rename moves nothing.
        self.rename('codex/demo/old', 'codex/demo/archived', archive=True)
        self.assertEqual(self.ok('show', ref)['task']['pinned']['name'], 'codex/demo/old')
        self.rename('codex/demo/old', 'codex/demo/new')
        self.assertEqual(self.ok('show', ref)['task']['pinned']['name'], 'codex/demo/new')

    def test_renames_create_no_store(self):
        self.rename('codex/demo/old', 'codex/demo/new')
        self.assertFalse((self.state / 'overmind').exists())


class AgentSession(unittest.TestCase):
    """`hgs overmind` inside a live session on an isolated tmux socket."""
    setUp = fixtures.InputTransport.setUp
    tearDown = fixtures.InputTransport.tearDown
    script = fixtures.InputTransport.script
    tmux = fixtures.InputTransport.tmux
    wait_for = fixtures.InputTransport.wait_for
    write_record = fixtures.InputTransport.write_record

    def overmind(self, *args, data=None, agent=True, env=None):
        env = dict(env or self.env)
        if agent:
            env.update(HGS_SESSION=self.name, HGS_RUN_ID=self.run_id)
        result = subprocess.run([str(HGS), 'overmind', *args], input=None if data is None else json.dumps(data),
                                env=env, text=True, capture_output=True, timeout=20)
        return result.returncode, json.loads(result.stdout) if result.stdout.startswith('{') else result.stdout

    def ok(self, *args, **kwargs):
        code, body = self.overmind(*args, **kwargs)
        self.assertEqual(code, 0, body)
        return body

    def claim(self):
        """Claims the only task for this session; plan 1a has no command for it."""
        db = sqlite3.connect(self.state / 'overmind/overmind.sqlite3')
        with db:
            created = json.loads(db.execute("SELECT raw FROM events WHERE kind = 'task_created'").fetchone()[0])
            seq, lamport = db.execute('SELECT MAX(seq) + 1, MAX(lamport) + 1 FROM events').fetchone()
            event = dict(created, id=str(uuid.uuid4()), seq=seq, lamport=lamport, kind='transition',
                         base=created['id'], request_id='',
                         payload={'action': 'claim', 'to': 'Design', 'fields': {},
                                  'session': {'machine': created['origin'], 'name': self.name}})
            db.execute('INSERT INTO events(origin, seq, id, lamport, task, project, kind, raw) VALUES (?, ?, ?, ?, ?, ?, ?, ?)',
                       (event['origin'], seq, event['id'], lamport, event['task'], event['project'], event['kind'],
                        json.dumps(event)))
        db.close()

    def commit(self, repo):
        env = {key: value for key, value in os.environ.items() if not key.startswith('GIT_')}
        env.update(GIT_CONFIG_GLOBAL='/dev/null', GIT_CONFIG_NOSYSTEM='1')
        git = lambda *args: subprocess.check_output(['git', '-C', str(repo), *args], env=env, text=True).strip()
        repo.mkdir()
        git('init', '-q')
        git('-c', 'user.name=Fixture', '-c', 'user.email=fixture@example.test', '-c', 'commit.gpgsign=false',
            'commit', '--allow-empty', '-qm', 'work')
        return git('rev-parse', 'HEAD')

    def test_a_live_session_acts_as_its_agent(self):
        self.assertIn('You can never move a task to Done', self.ok('--help'))
        self.assertEqual(self.ok('mine')['tasks'], [])
        ref = self.ok('create', '--json', data={'project': 'demo', 'prompt': 'x', 'stage': 'Queue'}, agent=False)['task']['ref']
        self.claim()
        self.assertEqual([t['ref'] for t in self.ok('mine')['tasks']], [ref])
        self.run_id = str(uuid.uuid4())
        code, body = self.overmind('mine')
        self.assertEqual((code, body['error']['code']), (1, 'session_mismatch'))

    def test_evidence_is_checked_in_the_session_repository(self):
        repo, other = self.root / 'repo', self.root / 'other'
        sha = self.commit(repo)
        self.commit(other)
        self.record['cwd'] = str(repo)
        self.write_record()
        ref = self.ok('create', '--json', data={'project': 'demo', 'prompt': 'x', 'stage': 'Queue'}, agent=False)['task']['ref']
        self.claim()
        self.ok('advance', '--json', data={'task': ref, 'to': 'Build', 'spec': 'plan'})
        review = {'task': ref, 'to': 'Review', 'worklog': 'done', 'evidence': '0' * 40}
        # The agent's own Git variables neither skip the check nor move it to another repository.
        for env in (self.env, dict(self.env, GIT_DIR=str(other / '.git'), GIT_WORK_TREE=str(other))):
            code, body = self.overmind('advance', '--json', data=review, env=env)
            self.assertEqual((code, body['error']['field']), (1, 'evidence'))
        elsewhere = dict(self.env, GIT_DIR=str(other / '.git'))
        task = self.ok('advance', '--json', data={**review, 'evidence': sha}, env=elsewhere)['task']
        self.assertEqual(task['stage'], 'Review')


if __name__ == '__main__':
    unittest.main()
