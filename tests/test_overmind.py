#!/usr/bin/env python3
"""Overmind tasks through the real CLI with isolated state."""
import base64
import hashlib
import json
import os
from pathlib import Path
import re
import sqlite3
import subprocess
import tempfile
import time
import unittest
import uuid

import test_dsh_session_actions
import test_input as fixtures
import test_pause

REPO = Path(__file__).resolve().parents[1]
HGS = Path(os.environ.get('HGS_TEST_BIN', REPO / 'target/debug/hgs')).resolve()


def next_position(db, origin):
    """The next seq of `origin`, which numbers only its own events, and the next lamport."""
    seq = db.execute('SELECT COALESCE(MAX(seq), 0) + 1 FROM events WHERE origin = ?', (origin,)).fetchone()[0]
    lamport = db.execute('SELECT COALESCE(MAX(lamport), 0) + 1 FROM events').fetchone()[0]
    return seq, lamport


def claim(state, task, name):
    """Claims a task still in Queue for a session of this machine, as its host does on send."""
    db = sqlite3.connect(Path(state) / 'overmind/overmind.sqlite3')
    with db:
        created = json.loads(db.execute("SELECT raw FROM events WHERE task = ? AND kind = 'task_created'",
                                        (task,)).fetchone()[0])
        seq, lamport = next_position(db, created['origin'])
        event = dict(created, id=str(uuid.uuid4()), seq=seq, lamport=lamport, kind='transition',
                     base=created['id'], request_id='',
                     payload={'action': 'claim', 'to': 'Design', 'fields': {},
                              'session': {'machine': created['actor']['machine'], 'name': name}})
        db.execute('INSERT INTO events(origin, seq, id, lamport, task, project, kind, raw) VALUES (?, ?, ?, ?, ?, ?, ?, ?)',
                   (event['origin'], seq, event['id'], lamport, task, event['project'], 'transition', json.dumps(event)))
    db.close()


def overmind(env, *args, data=None):
    """A person's `hgs overmind` request that succeeds."""
    result = subprocess.run([str(HGS), 'overmind', *args], input=None if data is None else json.dumps(data),
                            env=env, text=True, capture_output=True, timeout=20)
    assert result.returncode == 0, result.stderr + result.stdout
    return json.loads(result.stdout)


class Overmind(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory(prefix='hgs-overmind-')
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name)
        (self.root / 'bin').mkdir()
        (self.root / 'config').mkdir()
        # hgs puts $HOME/.local/bin first in PATH: this stub answers every tmux call and reaches no server.
        tmux = self.root / '.local/bin/tmux'
        tmux.parent.mkdir(parents=True)
        tmux.write_text('#!/bin/sh\nif [ "$1" = -V ]; then echo "tmux 3.7"; fi\n')
        tmux.chmod(0o755)
        self.state = self.root / 'state'
        self.env = dict(os.environ, HOME=str(self.root), HGS_CONFIG_DIR=str(self.root / 'config'),
                        HGS_STATE_DIR=str(self.state), HGS_TAB='0',
                        PATH=str(self.root / 'bin') + ':' + os.environ['PATH'])
        for key in ('HGS_PEERS', 'HGS_SELF', 'TMUX', 'TMUX_PANE', 'HGS_RUN_ID', 'HGS_SESSION', 'HGS_EXECUTABLE', 'HGS_SSH'):
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

    def test_status_on_a_lone_machine(self):
        self.ok('create', '--json', data={'project': 'demo', 'prompt': 'x'})
        status = self.ok('status')
        self.assertEqual((status['peers'], list(status['vector'].values())), ([], [1]))
        # Events are numbered under the store's own origin, not the machine identity.
        self.assertEqual(list(status['vector']), [status['origin']])
        self.assertNotEqual(status['origin'], status['machine'])

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

    def test_swarm_requests_never_come_from_sessions(self):
        def swarm(env):
            return subprocess.run([str(HGS), 'swarm', 'overmind-blobs'], input=json.dumps({'want': []}), env=env,
                                  text=True, capture_output=True, timeout=20)
        self.assertEqual(json.loads(swarm(self.env).stdout), {'missing': []})
        self.assertEqual(swarm(dict(self.env, HGS_SESSION='', HGS_RUN_ID='')).returncode, 0)
        for extra in ({'HGS_SESSION': 'claude/demo/w1'}, {'HGS_RUN_ID': str(uuid.uuid4())}):
            result = swarm(dict(self.env, **extra))
            self.assertEqual(result.returncode, 1, result.stdout)
            self.assertIn('Overmind swarm requests come from other machines, not from sessions', result.stderr)

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

    def test_a_session_archived_after_its_agent_quit_orphans_its_tasks(self):
        task = self.ok('create', '--json', data={'project': 'demo', 'prompt': 'x', 'stage': 'Queue'})['task']
        name, run, conversation = 'claude/demo/quit', str(uuid.uuid4()), str(uuid.uuid4())
        claim(self.state, task['uuid'], name)
        # Saved before the archive: the agent reported that the user quit, and its process is gone.
        binding = {'version': 1, 'name': name, 'agent': 'claude', 'run_id': run, 'conversation_id': conversation,
                   'conversation_state': 'ended',
                   'session_end': {'run_id': run, 'conversation_id': conversation, 'reason': 'prompt_input_exit'}}
        (self.state / f'{hashlib.sha256(name.encode()).hexdigest()}.json').write_text(json.dumps(binding))
        exists = subprocess.run([str(HGS), '__state', 'exists', name], env=self.env, text=True, capture_output=True, timeout=20)
        self.assertEqual((exists.returncode, exists.stderr), (1, ''))  # archived, so no binding is left
        self.assertEqual(len(list((self.state / 'archives').glob('*.json'))), 1)
        self.assertEqual(self.ok('show', task['ref'])['task']['flags'], ['orphaned'])

    def test_pin_without_a_machine_and_unpin(self):
        ref = self.ok('create', '--json', data={'project': 'demo', 'prompt': 'x', 'stage': 'Queue'})['task']['ref']
        pinned = self.ok('pin', '--json', data={'task': ref, 'session': {'name': 'codex/demo/w1'}})['task']['pinned']
        self.assertEqual(pinned, {'machine': self.ok('status')['machine'], 'name': 'codex/demo/w1'})
        self.assertIsNone(self.ok('unpin', '--json', data={'task': ref})['task']['pinned'])
        self.assertIsNone(self.ok('show', ref)['task']['pinned'])

    def test_templates_round_trip(self):
        self.ok('templates', 'set', '--json', data={'project': 'demo', 'templates': {'review': 'Check {task.ref}'}})
        listed = self.ok('templates', 'get', '--json', data={'project': 'demo'})['templates']
        self.assertEqual(listed['review'], {'text': 'Check {task.ref}', 'custom': True})
        self.assertTrue(listed['dispatch']['text'].startswith('[Overmind {task.ref}]'))
        self.assertEqual(self.refused('templates', 'set', '--json', data={'project': 'demo', 'templates': {'review': '!rm'}})['code'], 'invalid_template')


class AgentSession(unittest.TestCase):
    """`hgs overmind` inside a live session on an isolated tmux socket."""
    tearDown = fixtures.InputTransport.tearDown

    def setUp(self):
        # The input fixture's shim in $HOME/.local/bin reaches only its socket; no SSH leaves the test.
        fixtures.InputTransport.setUp(self)
        self.env.pop('HGS_SSH', None)
    script = fixtures.InputTransport.script
    tmux = fixtures.InputTransport.tmux
    wait_for = fixtures.InputTransport.wait_for
    write_record = fixtures.InputTransport.write_record
    received = fixtures.InputTransport.received

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

    def append(self, kind, payload, actor=None, base=None, request_id=''):
        """Appends one event to the only task; plan 1a has no command for some of them."""
        db = sqlite3.connect(self.state / 'overmind/overmind.sqlite3')
        with db:
            created = json.loads(db.execute("SELECT raw FROM events WHERE kind = 'task_created'").fetchone()[0])
            if base is None:
                rows = db.execute("SELECT raw FROM events WHERE kind = 'transition' ORDER BY seq").fetchall()
                base = json.loads(rows[-1][0])['id'] if rows else created['id']
            seq, lamport = next_position(db, created['origin'])
            event = dict(created, id=str(uuid.uuid4()), seq=seq, lamport=lamport, kind=kind,
                         base=base, request_id=request_id, payload=payload)
            if actor is not None:
                event['actor'] = actor
            db.execute('INSERT INTO events(origin, seq, id, lamport, task, project, kind, raw) VALUES (?, ?, ?, ?, ?, ?, ?, ?)',
                       (event['origin'], seq, event['id'], lamport, event['task'], event['project'], event['kind'],
                        json.dumps(event)))
        db.close()

    def claim(self, request_id='', name=None):
        """Claims the only task for a session (this one by default)."""
        machine = json.loads(sqlite3.connect(self.state / 'overmind/overmind.sqlite3').execute(
            "SELECT raw FROM events WHERE kind = 'task_created'").fetchone()[0])['actor']['machine']
        self.append('transition', {'action': 'claim', 'to': 'Design', 'fields': {},
                                   'session': {'machine': machine, 'name': name or self.name}}, request_id=request_id)

    def person(self, *args, data=None):
        return self.ok(*args, data=data, agent=False)

    def task(self, **fields):
        return self.person('create', '--json', data={'project': 'demo', 'prompt': 'Fix the drafts', 'stage': 'Queue', **fields})['task']['ref']

    def send(self, ref, **fields):
        return self.overmind('send', '--json', data={'task': ref, 'session': {'name': self.name}, **fields}, agent=False)

    def test_send_claims_and_types_the_dispatch_text(self):
        image = base64.b64encode(b'\x89PNG fixture').decode()
        ref = self.task(attachments=[{'name': 'shot.png', 'mime': 'image/png', 'data_base64': image}])
        code, body = self.send(ref)
        self.assertEqual((code, body['delivery']), (0, 'sent'), body)
        self.assertEqual((body['task']['stage'], body['task']['assignee']['name']), ('Design', self.name))
        self.assertEqual([j['marker'] for j in body['task']['journal']][-2:], ['[claim]', '[sent]'])
        self.wait_for(lambda: b'\r' in self.received())
        typed = self.received()
        self.assertIn(f'[Overmind {ref}] Fix the drafts'.encode(), typed)
        self.assertIn(b'Inspect the attached image', typed)

    def test_send_retry_with_the_same_request_never_types_twice(self):
        request_id = str(uuid.uuid4())
        ref = self.task()
        first = self.send(ref, request_id=request_id)
        self.wait_for(lambda: b'\r' in self.received())
        self.assertEqual(self.send(ref, request_id=request_id), first)
        self.assertEqual(self.received().count(b'[Overmind '), 1)

    def test_an_interrupted_send_resumes_its_claim(self):
        request_id = str(uuid.uuid4())
        ref = self.task()
        self.claim(request_id=request_id)  # as if hgs stopped right after the claim
        code, body = self.send(ref, request_id=request_id)
        self.assertEqual((code, body['delivery']), (0, 'sent'), body)
        markers = [j['marker'] for j in body['task']['journal']]
        self.assertEqual((markers.count('[claim]'), markers.count('[sent]')), (1, 1))

    def test_a_resumed_claim_is_not_gated_by_the_session_state(self):
        request_id = str(uuid.uuid4())
        ref = self.task()
        self.claim(request_id=request_id)
        typed = subprocess.run(
            [str(HGS), 'send', self.name, '--json'], env=self.env, text=True, capture_output=True, timeout=20,
            input=json.dumps({'request_id': request_id, 'text': 'typed before the crash',
                              'expected_run_id': self.record['run_id'],
                              'expected_conversation_id': self.record['conversation_id']}))
        self.assertEqual(typed.returncode, 0, typed.stderr)
        self.wait_for(lambda: b'\r' in self.received())
        before = self.received()
        self.record['activity'] = 'busy'  # the agent now works on the text
        self.write_record()
        code, body = self.send(ref, request_id=request_id)
        self.assertEqual((code, body['delivery']), (0, 'uncertain'), body)
        markers = [j['marker'] for j in body['task']['journal']]
        self.assertEqual((markers.count('[claim]'), markers.count('[sent?]')), (1, 1))
        self.assertIn('delivery_uncertain', body['task']['flags'])
        self.assertEqual(self.received(), before)

    def test_an_unready_session_writes_nothing(self):
        ref = self.task()
        self.record['expected_id'] = 'pending'
        self.write_record()
        code, body = self.send(ref)
        self.assertEqual((code, body['error']['code']), (1, 'session_unavailable'))
        task = self.person('show', ref)['task']
        self.assertEqual((task['stage'], [j['marker'] for j in task['journal']]), ('Queue', ['[created]']))
        self.assertEqual(self.received(), b'')

    def nothing_written(self, ref, stage='Queue', journal=('[created]',)):
        task = self.person('show', ref)['task']
        self.assertEqual((task['stage'], [j['marker'] for j in task['journal']]), (stage, list(journal)))
        self.assertEqual(self.received(), b'')
        return task

    def test_pending_settings_hgs_send_would_refuse_write_nothing(self):
        ref = self.task()
        # Settings scheduled for another run of the session: `hgs send` refuses before typing.
        self.record['pending_settings'] = {'request_id': str(uuid.uuid4()), 'run_id': str(uuid.uuid4()),
                                           'conversation_id': self.record['conversation_id']}
        self.write_record()
        code, body = self.send(ref)
        self.assertEqual((code, body['error']['code']), (1, 'session_unavailable'), body)
        self.assertIn('pending settings', body['error']['message'])
        self.nothing_written(ref)

    def test_deepseek_and_unknown_sessions_refuse_and_write_nothing(self):
        ref = self.task()
        native = 'dsh/demo/w1'
        bindings = self.state / 'dsh/bindings'
        bindings.mkdir(parents=True)
        (bindings / f'{hashlib.sha256(native.encode()).hexdigest()}.json').write_text('{}')
        for name, expected in [(native, 'session_unavailable'), ('codex/demo/nobody', 'unknown_session')]:
            code, body = self.overmind('send', '--json', data={'task': ref, 'session': {'name': name}}, agent=False)
            self.assertEqual((code, body['error']['code'], body['error']['field']), (1, expected, 'session'), body)
        self.nothing_written(ref)

    def image_not_here(self, ref):
        """Removes the task's only image from this machine; no other machine holds it."""
        sha = self.person('show', ref)['task']['attachments'][0]['sha256']
        (self.state / 'overmind/blobs' / sha[:2] / sha).unlink()

    def image_task(self):
        image = base64.b64encode(b'\x89PNG fixture').decode()
        return self.task(attachments=[{'name': 'shot.png', 'mime': 'image/png', 'data_base64': image}])

    def test_paused_and_stopped_sessions_refuse_before_images_are_fetched(self):
        ref = self.image_task()
        self.image_not_here(ref)
        # A pause in progress, then a session whose agent is gone after the pause.
        self.record['pausing'] = {'pid': self.record['pid'], 'start': self.record['process_start']}
        self.write_record()
        code, body = self.send(ref)
        self.assertEqual((code, body['error']['code']), (1, 'session_unavailable'), body)
        self.nothing_written(ref)
        self.stop_agent()
        self.record.pop('pausing')
        self.record['paused'] = True
        self.write_record()
        code, body = self.send(ref)
        self.assertEqual((code, body['error']['code']), (1, 'session_unavailable'), body)
        self.nothing_written(ref)

    def test_review_send_to_a_stopped_session_fetches_no_images(self):
        ref = self.image_task()
        self.agent_reaches_review(ref, 'codex/other')
        self.image_not_here(ref)
        self.stop_agent()
        code, body = self.overmind('review-send', '--json', data={'task': ref, 'session': {'name': self.name}}, agent=False)
        self.assertEqual((code, body['error']['code']), (1, 'session_unavailable'), body)
        self.assertIsNone(self.person('show', ref)['task']['reviewer'])

    def test_rework_send_to_a_stopped_session_fetches_no_images(self):
        ref = self.image_task()
        self.review_failed(ref)
        self.image_not_here(ref)
        self.stop_agent()
        journal = [j['marker'] for j in self.person('show', ref)['task']['journal']]
        code, body = self.overmind('rework-send', '--json', data={'task': ref}, agent=False)
        self.assertEqual((code, body['error']['code']), (1, 'session_unavailable'), body)
        self.nothing_written(ref, 'Build', journal)

    def test_a_retry_after_typing_records_a_text_that_no_longer_renders_as_uncertain(self):
        request_id = str(uuid.uuid4())
        ref = self.task(prompt='x' * (40 * 1024))
        self.review_failed(ref)
        self.person('templates', 'set', '--json', data={'project': 'demo', 'templates': {'rework': '{task.prompt}{task.prompt}'}})
        code, body = self.overmind('rework-send', '--json', data={'task': ref}, agent=False)
        self.assertEqual((code, body['error']['code']), (1, 'invalid_template'), body)
        # The text of the first attempt may be typed already: the retry never refuses it.
        self.type_directly(request_id)
        before = self.received()
        code, body = self.overmind('rework-send', '--json', data={'task': ref, 'request_id': request_id}, agent=False)
        self.assertEqual((code, body.get('delivery')), (0, 'uncertain'), body)
        self.assertIn('delivery_uncertain', body['task']['flags'])
        self.assertEqual(self.received(), before)

    def test_an_undelivered_answer_names_the_request_that_sends_it_again(self):
        ref = self.task()
        self.asks_a_question(ref)
        shim = (self.bin / 'tmux').read_text()
        # Staging the text fails: nothing reaches the session and no input receipt exists.
        self.script('tmux', '#!/bin/sh\nif [ "$1" = load-buffer ]; then exit 1; fi\n' + shim.split('\n', 1)[1])
        code, body = self.overmind('answer', '--json', data={'task': ref, 'answer': 'REST'}, agent=False)
        self.assertEqual((code, body['error']['code']), (1, 'not_delivered'), body)
        request_id = re.search(r'request_id ([0-9a-f-]{36})', body['error']['message']).group(1)
        self.script('tmux', shim)
        code, body = self.overmind('answer', '--json', data={'task': ref, 'answer': 'REST', 'request_id': request_id}, agent=False)
        self.assertEqual((code, body['delivery'], body['task']['stage']), (0, 'sent', 'Build'), body)
        self.assertEqual([j['marker'] for j in body['task']['journal']].count('[answer]'), 1)
        self.wait_for(lambda: b'REST' in self.received())

    def test_a_pin_that_missed_a_rename_follows_it_on_the_host(self):
        ref = self.task()
        old, new = self.name, self.name + '/renamed'
        renamed = self.hgs('rename', old, new)
        self.assertEqual(renamed.returncode, 0, renamed.stderr)
        self.name = new
        # Written on a machine that had not seen the rename yet: it sorts after the rename and names the old session.
        machine = self.person('show', ref)['task']['created_by']['machine']
        self.append('pin_set', {'session': {'machine': machine, 'name': old}}, actor={'kind': 'human', 'machine': str(uuid.uuid4())})
        task = self.person('show', ref)['task']
        self.assertEqual((task['pinned']['name'], task['pinned_current']), (old, new))
        self.assertEqual(self.person('board', '--json', data={'project': 'demo'})['tasks'][0]['pinned_current'], new)
        code, body = self.send(ref)
        self.assertEqual((code, body.get('delivery')), (0, 'sent'), body)

    def test_a_busy_agent_and_a_second_task_need_confirmation(self):
        first, second = self.task(), self.task()
        self.record['activity'] = 'busy'
        self.write_record()
        code, body = self.send(first)
        self.assertEqual((code, body['error']['code'], body['error']['field']), (1, 'agent_busy', 'confirm'))
        self.assertEqual(self.send(first, confirm=['agent_busy'])[1]['delivery'], 'sent')
        code, body = self.send(second, confirm=['agent_busy'])
        self.assertEqual(body['error']['code'], 'wip_exceeded')

    def test_send_refuses_an_oversized_text_before_claiming(self):
        self.person('templates', 'set', '--json', data={'project': 'demo', 'templates': {'dispatch': '{task.prompt}{task.prompt}'}})
        ref = self.task(prompt='x' * (40 * 1024))
        code, body = self.send(ref)
        self.assertEqual((code, body['error']['code']), (1, 'invalid_template'))
        self.assertEqual(self.person('show', ref)['task']['stage'], 'Queue')

    def agent_reaches_review(self, ref, assignee):
        """The task moves to Review as if `assignee` did the work."""
        machine = self.person('show', ref)['task']['created_by']['machine']
        self.claim(name=assignee)
        agent = {'kind': 'agent', 'machine': machine, 'session': assignee}
        self.append('transition', {'action': 'advance', 'to': 'Build', 'fields': {'spec': 's'}}, actor=agent)
        self.append('transition', {'action': 'advance', 'to': 'Review', 'fields': {'worklog': 'w', 'evidence': 'e'}}, actor=agent)

    def test_review_send_types_the_review_text_to_another_session(self):
        ref = self.task()
        self.agent_reaches_review(ref, 'codex/other')
        code, body = self.overmind('review-send', '--json', data={'task': ref, 'session': {'name': self.name}}, agent=False)
        self.assertEqual((code, body['delivery'], body['task']['reviewer']['name']), (0, 'sent', self.name), body)
        self.wait_for(lambda: b'Review: Fix the drafts' in self.received())
        verdict = self.ok('review', '--json', data={'task': ref, 'verdict': 'needs_work', 'report': 'Add tests'})
        self.assertEqual(verdict['task']['stage'], 'Build')

    def test_rework_and_answer_reach_the_assignee(self):
        ref = self.task()
        self.claim()
        self.ok('advance', '--json', data={'task': ref, 'to': 'Build', 'spec': 's'})
        self.ok('call-human', '--json', data={'task': ref, 'question': 'REST or gRPC?'})
        code, body = self.overmind('answer', '--json', data={'task': ref, 'answer': 'REST'}, agent=False)
        self.assertEqual((code, body['delivery'], body['task']['stage']), (0, 'sent', 'Build'), body)
        self.wait_for(lambda: b'REST' in self.received())
        code, body = self.overmind('rework-send', '--json', data={'task': ref}, agent=False)
        self.assertEqual((code, body['error']['code']), (1, 'not_reviewed'))

    def test_an_answer_retry_replays_and_types_nothing_new(self):
        request_id = str(uuid.uuid4())
        ref = self.task()
        self.claim()
        self.ok('advance', '--json', data={'task': ref, 'to': 'Build', 'spec': 's'})
        self.ok('call-human', '--json', data={'task': ref, 'question': 'REST or gRPC?'})
        data = {'task': ref, 'answer': 'REST', 'request_id': request_id}
        first = self.overmind('answer', '--json', data=data, agent=False)
        self.wait_for(lambda: b'REST' in self.received())
        typed = self.received()
        self.assertEqual(self.overmind('answer', '--json', data=data, agent=False), first)
        self.assertEqual(self.received(), typed)

    def test_a_retry_after_typing_skips_the_gates(self):
        request_id = str(uuid.uuid4())
        ref = self.task()
        self.claim(name='codex/other')  # not in review: the review gate would refuse
        typed = subprocess.run(
            [str(HGS), 'send', self.name, '--json'], env=self.env, text=True, capture_output=True, timeout=20,
            input=json.dumps({'request_id': request_id, 'text': 'typed before the crash',
                              'expected_run_id': self.record['run_id'],
                              'expected_conversation_id': self.record['conversation_id']}))
        self.assertEqual(typed.returncode, 0, typed.stderr)
        self.wait_for(lambda: b'\r' in self.received())
        self.record['activity'] = 'busy'  # agent_busy would refuse a fresh request
        self.write_record()
        code, body = self.overmind('review-send', '--json', agent=False,
                                   data={'task': ref, 'session': {'name': self.name}, 'request_id': request_id})
        self.assertEqual((code, body.get('delivery')), (0, 'uncertain'), body)
        markers = [j['marker'] for j in body['task']['journal']]
        self.assertEqual(markers.count('[sent?]'), 1)
        self.assertIsNone(body['task']['reviewer'])
        self.assertIn('delivery_uncertain', body['task']['flags'])

    def type_directly(self, request_id):
        """Types through `hgs send` as a first, interrupted attempt of a command would."""
        typed = subprocess.run(
            [str(HGS), 'send', self.name, '--json'], env=self.env, text=True, capture_output=True, timeout=20,
            input=json.dumps({'request_id': request_id, 'text': 'typed before the crash',
                              'expected_run_id': self.record['run_id'],
                              'expected_conversation_id': self.record['conversation_id']}))
        self.assertEqual(typed.returncode, 0, typed.stderr)
        self.wait_for(lambda: b'\r' in self.received())

    def asks_a_question(self, ref):
        self.claim()
        self.ok('advance', '--json', data={'task': ref, 'to': 'Build', 'spec': 's'})
        self.ok('call-human', '--json', data={'task': ref, 'question': 'REST or gRPC?'})

    def advance_to_review(self, agent):
        self.append('transition', {'action': 'advance', 'to': 'Review', 'fields': {'worklog': 'w', 'evidence': 'e'}}, actor=agent)

    def review_failed(self, ref):
        """A task in Build assigned to this session after a review asked for changes."""
        self.claim()
        self.ok('advance', '--json', data={'task': ref, 'to': 'Build', 'spec': 's'})
        machine = self.person('show', ref)['task']['created_by']['machine']
        self.advance_to_review({'kind': 'agent', 'machine': machine, 'session': self.name})
        reviewer = {'kind': 'agent', 'machine': machine, 'session': 'codex/other'}
        self.append('reviewer_set', {'session': {'machine': machine, 'name': 'codex/other'}})
        self.append('transition', {'action': 'needs_work', 'to': 'Build', 'fields': {'report': 'tests missing'}}, actor=reviewer)

    def test_rework_send_types_the_review_report(self):
        ref = self.task()
        self.review_failed(ref)
        code, body = self.overmind('rework-send', '--json', data={'task': ref}, agent=False)
        self.assertEqual((code, body.get('delivery')), (0, 'sent'), body)
        self.wait_for(lambda: b'tests missing' in self.received())
        self.assertIn(b'Rework: Fix the drafts', self.received())

    def test_a_rework_retry_after_typing_skips_the_gates(self):
        request_id = str(uuid.uuid4())
        ref = self.task()
        self.review_failed(ref)
        self.type_directly(request_id)
        machine = self.person('show', ref)['task']['created_by']['machine']
        self.advance_to_review({'kind': 'agent', 'machine': machine, 'session': self.name})  # no longer reworkable
        before = self.received()
        code, body = self.overmind('rework-send', '--json', data={'task': ref, 'request_id': request_id}, agent=False)
        self.assertEqual((code, body.get('delivery')), (0, 'uncertain'), body)
        self.assertEqual(self.received(), before)

    def test_an_answer_retry_after_typing_skips_the_gates(self):
        request_id = str(uuid.uuid4())
        ref = self.task()
        self.asks_a_question(ref)
        self.type_directly(request_id)
        before = self.received()
        code, body = self.overmind('answer', '--json', data={'task': ref, 'answer': 'REST', 'request_id': request_id}, agent=False)
        self.assertEqual((code, body.get('delivery')), (0, 'uncertain'), body)
        self.assertEqual(self.received(), before)

    def test_a_recorded_but_unsent_answer_is_sent_by_repeating_the_request(self):
        request_id = str(uuid.uuid4())
        ref = self.task()
        self.asks_a_question(ref)
        self.append('transition', {'action': 'answer', 'to': '@resume', 'fields': {'answer': 'REST'}, 'session': None,
                                   'children': []}, request_id=request_id)  # as if hgs stopped after recording it
        code, body = self.overmind('answer', '--json', data={'task': ref, 'answer': 'REST', 'request_id': request_id}, agent=False)
        self.assertEqual((code, body['delivery'], body['task']['stage']), (0, 'sent', 'Build'), body)
        self.wait_for(lambda: b'REST' in self.received())
        markers = [j['marker'] for j in body['task']['journal']]
        self.assertEqual((markers.count('[answer]'), markers.count('[sent]')), (1, 1), markers)
        self.assertEqual(self.received().count(b'[Overmind '), 1)
        self.assertEqual(self.overmind('answer', '--json', data={'task': ref, 'answer': 'REST', 'request_id': request_id}, agent=False)[1], body)

    def test_a_recorded_answer_is_never_replaced_by_another_text(self):
        request_id = str(uuid.uuid4())
        ref = self.task()
        self.asks_a_question(ref)
        self.append('transition', {'action': 'answer', 'to': '@resume', 'fields': {'answer': 'REST'}, 'session': None,
                                   'children': []}, request_id=request_id)  # recorded, then hgs stopped
        code, body = self.overmind('answer', '--json', data={'task': ref, 'answer': 'gRPC', 'request_id': request_id}, agent=False)
        self.assertEqual((code, body['error']['code']), (1, 'request_reused'), body)
        self.assertEqual(self.received(), b'')
        self.assertNotIn('[sent]', [j['marker'] for j in self.person('show', ref)['task']['journal']])

    def commit(self, repo):
        env = {key: value for key, value in os.environ.items() if not key.startswith('GIT_')}
        env.update(GIT_CONFIG_GLOBAL='/dev/null', GIT_CONFIG_NOSYSTEM='1')
        git = lambda *args: subprocess.check_output(['git', '-C', str(repo), *args], env=env, text=True).strip()
        repo.mkdir()
        git('init', '-q')
        git('-c', 'user.name=Fixture', '-c', 'user.email=fixture@example.test', '-c', 'commit.gpgsign=false',
            'commit', '--allow-empty', '-qm', 'work')
        return git('rev-parse', 'HEAD')

    def hgs(self, *args):
        return subprocess.run([str(HGS), *args], env=self.env, text=True, capture_output=True, timeout=20,
                              stdin=subprocess.DEVNULL)

    def flags(self, ref):
        return self.person('show', ref)['task']['flags']

    def stop_agent(self):
        """Ends the fixture's own session and waits until its agent has exited."""
        self.tmux('kill-session', '-t', '=' + self.name)
        self.wait_for(lambda: not subprocess.run(['ps', '-p', str(self.record['pid']), '-o', 'pid='],
                                                 capture_output=True, text=True).stdout.strip())

    def test_archiving_a_session_orphans_its_tasks(self):
        elsewhere = self.task()
        claim(self.state, self.person('show', elsewhere)['task']['uuid'], 'codex/other')
        ref, waiting = self.task(), self.task()
        self.assertEqual(self.send(ref)[1]['delivery'], 'sent')
        self.stop_agent()
        archived = self.hgs('archive', self.name)
        self.assertEqual((archived.returncode, archived.stderr), (0, ''))
        self.assertIn(f'archived {self.name}', archived.stdout)
        self.assertEqual([self.flags(r) for r in (ref, elsewhere, waiting)], [['orphaned'], [], []])
        task = self.person('show', ref)['task']
        self.assertEqual((task['stage'], task['assignee']['name']), ('Design', self.name))
        # The flag names the session, so it never lands on a task another session took meanwhile.
        db = sqlite3.connect(self.state / 'overmind/overmind.sqlite3')
        flags = [json.loads(raw)['payload'] for (raw,) in db.execute("SELECT raw FROM events WHERE kind = 'flag_set'")]
        db.close()
        self.assertEqual(flags, [{'flag': 'orphaned', 'value': True, 'session': task['assignee']}])
        moved = self.person('move', '--json', data={'task': ref, 'to': 'Queue'})['task']
        self.assertEqual((moved['stage'], moved['assignee'], moved['flags']), ('Queue', None, []))

    def test_terminating_a_session_orphans_its_tasks(self):
        ref = self.task()
        self.send(ref)
        result = self.hgs('terminate', self.name)
        self.assertEqual((result.returncode, result.stderr), (0, ''))
        self.assertIn('terminated and archived', result.stdout)
        self.assertEqual(self.flags(ref), ['orphaned'])

    def test_killing_a_session_orphans_its_tasks(self):
        ref = self.task()
        self.send(ref)
        result = self.hgs('kill', self.name)
        self.assertEqual((result.returncode, result.stderr), (0, ''))
        self.assertFalse(self.record_path.exists())
        self.assertEqual(self.flags(ref), ['orphaned'])

    def test_a_busy_task_store_never_fails_the_archive(self):
        ref = self.task()
        self.send(ref)
        store = sqlite3.connect(self.state / 'overmind/overmind.sqlite3', isolation_level=None)
        store.execute('BEGIN IMMEDIATE')  # another Overmind writer holds the store
        try:
            started = time.monotonic()
            result = self.hgs('terminate', self.name)
            elapsed = time.monotonic() - started
        finally:
            store.execute('ROLLBACK')
            store.close()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('terminated and archived', result.stdout)
        self.assertIn('Overmind', result.stderr)
        self.assertLess(elapsed, 5)
        self.assertEqual(self.flags(ref), [])

    def test_ending_a_session_creates_no_store(self):
        self.assertEqual(self.hgs('swarm', 'hello').returncode, 0)  # the machine has its identity
        result = self.hgs('terminate', self.name)
        self.assertEqual((result.returncode, result.stderr), (0, ''))
        self.assertFalse((self.state / 'overmind').exists())

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


class UserExit(test_pause.Harness):
    """A session whose agent the user quits is archived by its supervisor."""

    def test_quitting_the_agent_orphans_its_tasks(self):
        name = self.start('claude')
        task = overmind(self.env, 'create', '--json', data={'project': 'p', 'prompt': 'x', 'stage': 'Queue'})['task']
        claim(self.root / 'state', task['uuid'], name)
        self.t('send-keys', '-t', '=' + name + ':', '/exit', 'Enter')
        self.wait(lambda: not self.binding(name))
        self.wait(lambda: overmind(self.env, 'show', task['ref'])['task']['flags'] == ['orphaned'])


class DeepSeekForget(unittest.TestCase):
    """Forgetting a native DeepSeek session through its resident adapter."""
    tearDown = test_dsh_session_actions.DeepSeekScopedActions.tearDown
    serve = test_dsh_session_actions.DeepSeekScopedActions.serve
    run_action = test_dsh_session_actions.DeepSeekScopedActions.run_action

    def setUp(self):
        test_dsh_session_actions.DeepSeekScopedActions.setUp(self)
        for key in ('TMUX', 'TMUX_PANE', 'HGS_SSH', 'HGS_SESSION', 'HGS_RUN_ID'):
            self.env.pop(key, None)
        # hgs puts $HOME/.local/bin first in PATH: this stub answers every tmux call and reaches no server.
        tmux = self.root / '.local/bin/tmux'
        tmux.parent.mkdir(parents=True)
        tmux.write_text('#!/bin/sh\nif [ "$1" = -V ]; then echo "tmux 3.7"; fi\n')
        tmux.chmod(0o755)

    def held_task(self):
        task = overmind(self.env, 'create', '--json', data={'project': 'example', 'prompt': 'x', 'stage': 'Queue'})['task']
        claim(self.root / 'state', task['uuid'], self.name)
        return task['ref']

    def test_the_fixture_never_reaches_a_live_tmux_or_ssh(self):
        for key in ('TMUX', 'TMUX_PANE', 'HGS_SSH', 'HGS_SESSION', 'HGS_RUN_ID'):
            self.assertFalse(key in self.env, key)
        self.assertTrue(os.access(self.root / '.local/bin/tmux', os.X_OK))

    def test_forgetting_a_session_orphans_its_tasks(self):
        scoped = self.held_task()
        self.assertEqual(self.run_action('forget')[1]['status'], 'completed')
        self.assertFalse(self.binding.exists())
        self.assertEqual(overmind(self.env, 'show', scoped)['task']['flags'], ['orphaned'])
        # `hgs kill` forgets it too; the tmux stub of setUp reaches no server.
        self.binding.write_text(json.dumps(self.record))
        killed = self.held_task()
        result = subprocess.run([str(HGS), 'kill', self.name], env=self.env, text=True, capture_output=True,
                                timeout=20, stdin=subprocess.DEVNULL)
        self.assertEqual((result.returncode, result.stderr), (0, ''))
        self.assertFalse(self.binding.exists())
        self.assertEqual(overmind(self.env, 'show', killed)['task']['flags'], ['orphaned'])


if __name__ == '__main__':
    unittest.main()
