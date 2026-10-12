#!/usr/bin/env python3
"""Overmind replication through the swarm worker, with isolated nodes and fake SSH."""
import base64
import hashlib
import json
import os
from pathlib import Path
import sqlite3
import subprocess
import tempfile
import time
import unittest
import uuid

import test_input as fixtures
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
# A marker that loses the answer of a delivery after the host ran it.
SSH = SSH.replace(
    "if not (root / ('old-' + alias)).exists():",
    "if args[1:] == ['overmind-deliver'] and (root / ('lost-deliver-' + alias)).exists():\n"
    "    subprocess.run([os.environ['SWARM_TEST_HGS'], *args], input=sys.stdin.read(), env=env, text=True, capture_output=True)\n"
    "    print('fixture connection lost', file=sys.stderr); sys.exit(255)\n"
    "if not (root / ('old-' + alias)).exists():", 1)
assert 'lost-deliver-' in SSH
# Markers for a host that rejects a call before running anything: from before forwarded
# deliveries, and one whose exchange fails after its hello.
SSH = SSH.replace(
    "if not (root / ('old-' + alias)).exists():",
    "if args[1:] == ['overmind-deliver'] and (root / ('no-deliver-' + alias)).exists():\n"
    "    print('this machine does not know the Overmind request overmind-deliver; update HGS on it', file=sys.stderr); sys.exit(1)\n"
    "if args[1:] == ['overmind-exchange'] and (root / ('bad-exchange-' + alias)).exists():\n"
    "    print('fixture exchange failure', file=sys.stderr); sys.exit(1)\n"
    "if not (root / ('old-' + alias)).exists():", 1)
assert 'no-deliver-' in SSH and 'bad-exchange-' in SSH


def next_position(db, origin):
    """The next seq of `origin`, which numbers only its own events, and the next lamport."""
    seq = db.execute('SELECT COALESCE(MAX(seq), 0) + 1 FROM events WHERE origin = ?', (origin,)).fetchone()[0]
    lamport = db.execute('SELECT COALESCE(MAX(lamport), 0) + 1 FROM events').fetchone()[0]
    return seq, lamport


def append(state, kind, payload, actor=None, base=None, request_id=''):
    """Appends one event to the only task of a node's store; no command exists for some of them."""
    db = sqlite3.connect(Path(state) / 'overmind/overmind.sqlite3')
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


def tmux_shim(home, socket, tmux=fixtures.TMUX):
    """hgs puts $HOME/.local/bin first in PATH, so every tmux call reaches only this shim:
    the fixture's socket when tmux exists, a failure when it does not."""
    shim = Path(home) / '.local/bin/tmux'
    shim.parent.mkdir(parents=True, exist_ok=True)
    if tmux:
        shim.write_text('#!/bin/sh\nexec ' + tmux + ' -S "' + str(socket) + '" -f /dev/null "$@"\n')
    else:
        shim.write_text('#!/bin/sh\necho "tmux is not available in this test" >&2\nexit 1\n')
    shim.chmod(0o755)


class Nodes(unittest.TestCase):
    """Two swarm nodes `a` and `b` on this machine, joined through the fake SSH."""
    def setUp(self):
        temp = tempfile.TemporaryDirectory(prefix='hgs-overmind-sync-')
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name)
        (self.root / 'bin').mkdir()
        ssh = self.root / 'bin/ssh'; ssh.write_text(SSH); ssh.chmod(0o755)
        # Every tmux call of both nodes, also through the fake SSH, reaches only this socket,
        # and every SSH call only the fake SSH: HOME is this directory.
        tmux_shim(self.root, self.root / 'socket')
        self.env = dict(os.environ, SWARM_TEST_ROOT=str(self.root), SWARM_TEST_HGS=str(swarm.HGS),
                        HOME=str(self.root), PATH=str(self.root / 'bin') + ':' + os.environ['PATH'], HGS_TAB='0')
        for key in ['HGS_SELF', 'HGS_PEERS', 'HGS_EXECUTABLE', 'HGS_SSH', 'TMUX', 'TMUX_PANE', 'HGS_RUN_ID', 'HGS_SESSION']:
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

    def overmind_raw(self, node, *args, data=None):
        return subprocess.run([str(swarm.HGS), 'overmind', *args], input=None if data is None else json.dumps(data),
                              env=self.node_env(node), text=True, capture_output=True, timeout=40)

    def refs(self, node):
        return sorted(t['ref'] for t in self.overmind(node, 'board', '--json', data={'project': 'demo'})['tasks'])

    def image(self, size):
        data = bytes(range(256)) * (size // 256)
        return {'name': 'shot.png', 'mime': 'image/png', 'data_base64': base64.b64encode(data).decode()}, data


class Replication(Nodes):
    def test_the_fixture_never_reaches_a_live_tmux_or_ssh(self):
        shim = self.root / '.local/bin/tmux'
        self.assertTrue(os.access(shim, os.X_OK))
        if fixtures.TMUX:
            self.assertIn('-S "' + str(self.root / 'socket') + '"', shim.read_text())
        self.assertNotIn('HGS_SSH', self.node_env('a'))
        # Without tmux the shim fails instead of letting hgs find a tmux elsewhere on its PATH.
        home = self.root / 'without-tmux'
        tmux_shim(home, self.root / 'socket', None)
        self.assertNotEqual(subprocess.run([str(home / '.local/bin/tmux'), 'list-sessions'], capture_output=True).returncode, 0)

    def test_images_are_fetched_in_ranges_when_needed(self):
        attachment, data = self.image(700 * 1024)
        task = self.overmind('a', 'create', '--json', data={'project': 'demo', 'prompt': 'x', 'attachments': [attachment]})['task']
        self.call('a', 'sync')
        sha = task['attachments'][0]['sha256']
        got = self.overmind('b', 'attachment', '--json', data={'task': task['ref'], 'sha256': sha})
        self.assertEqual(Path(got['path']).read_bytes(), data)
        calls = (self.root / 'calls').read_text()
        self.assertEqual(calls.count('overmind-blob\n'), 2)

    def test_missing_blob_refuses_before_claiming(self):
        attachment, _ = self.image(1024)
        task = self.overmind('a', 'create', '--json', data={'project': 'demo', 'prompt': 'x', 'attachments': [attachment], 'stage': 'Queue'})['task']
        self.call('a', 'sync')
        (self.root / 'offline-a').write_text('')
        result = self.overmind_raw('b', 'attachment', '--json', data={'task': task['ref'], 'sha256': task['attachments'][0]['sha256']})
        self.assertEqual(json.loads(result.stdout)['error']['code'], 'attachment_unavailable')

    def test_refused_sends_fetch_no_images(self):
        attachment, _ = self.image(1024)
        ref = self.overmind('a', 'create', '--json', data={'project': 'demo', 'prompt': 'x', 'attachments': [attachment]})['task']['ref']
        self.call('a', 'sync')
        (self.root / 'calls').write_text('')
        # The task is in Backlog: every gate refuses before b looks for the image.
        for command, request in [('send', {'task': ref, 'session': {'name': 'codex/nobody'}}),
                                 ('review-send', {'task': ref, 'session': {'name': 'codex/nobody'}}),
                                 ('rework-send', {'task': ref})]:
            result = self.overmind_raw('b', command, '--json', data=request)
            self.assertEqual(json.loads(result.stdout)['error']['code'], 'transition_not_allowed', command)
        self.assertNotIn('overmind-blob', (self.root / 'calls').read_text())

    def test_a_lost_delivery_answer_asks_to_check_the_task(self):
        ref = self.overmind('a', 'create', '--json', data={'project': 'demo', 'prompt': 'x', 'stage': 'Queue'})['task']['ref']
        b = self.overmind('b', 'status')['machine']
        (self.root / 'lost-deliver-b').write_text('')
        result = self.overmind_raw('a', 'send', '--json', data={'task': ref, 'session': {'machine': b, 'name': 'codex/nobody'}})
        self.assertEqual(result.returncode, 1, result.stderr)
        error = json.loads(result.stdout)['error']
        self.assertEqual(error['code'], 'delivery_unknown')
        self.assertNotIn('field', error)
        self.assertIn('check the task', error['message'])

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
        self.assertEqual(self.call('a', 'sync'), {'b': {'ok': True}})
        calls = (self.root / 'calls').read_text()
        self.assertIn('b swarm hello', calls)  # b was reached, so the absence below is not vacuous
        self.assertNotIn('overmind-exchange', calls)
        after = self.overmind('a', 'status')['peers']  # `behind` grows with local events, the exchange record must not change
        self.assertEqual([(p['alias'], p['last_ok'], p['updated']) for p in after], [(p['alias'], p['last_ok'], p['updated']) for p in before])

    def test_offline_peer_is_reported_and_recovers(self):
        self.overmind('a', 'create', '--json', data={'project': 'demo', 'prompt': 'x'})
        self.assertEqual(self.call('a', 'sync'), {'b': {'ok': True}})
        self.assertEqual(len(self.refs('b')), 1)
        (self.root / 'offline-b').write_text('')
        self.overmind('a', 'create', '--json', data={'project': 'demo', 'prompt': 'y'})
        # The outage shows in the swarm result; Overmind runs only after a successful hello.
        self.assertFalse(self.call('a', 'sync')['b']['ok'])
        (self.root / 'offline-b').unlink()
        self.assertEqual(len(self.refs('b')), 1)
        self.assertEqual([p['last_error'] for p in self.overmind('a', 'status')['peers']], [''])
        self.assertEqual(self.call('a', 'sync'), {'b': {'ok': True}})
        self.assertEqual(self.refs('a'), self.refs('b'))
        self.assertEqual(len(self.refs('b')), 2)

    def test_the_worker_exchanges_soon_after_a_local_write(self):
        (self.root / 'calls').write_text('')
        worker = subprocess.Popen([str(swarm.HGS), 'swarm', 'worker'], env=self.node_env('a'),
                                  stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
        try:
            # Its first pass exchanges once; later passes exchange only after a change (peers are due every 30 s).
            deadline = time.monotonic() + 15
            while time.monotonic() < deadline and 'overmind-exchange' not in (self.root / 'calls').read_text():
                time.sleep(.1)
            self.assertIn('overmind-exchange', (self.root / 'calls').read_text())
            ref = self.overmind('a', 'create', '--json', data={'project': 'demo', 'prompt': 'x'})['task']['ref']
            deadline = time.monotonic() + 15
            while time.monotonic() < deadline and ref not in self.refs('b'):
                time.sleep(.2)
            held = ref in self.refs('b')
        finally:
            worker.terminate()
            try:
                _, stderr = worker.communicate(timeout=5)
            except subprocess.TimeoutExpired:
                worker.kill()
                _, stderr = worker.communicate()
        self.assertTrue(held, stderr)


@unittest.skipUnless(fixtures.TMUX, 'tmux is required')
class RemoteDelivery(Nodes):
    """A live agent session that only node b tracks, on the nodes' isolated tmux socket."""
    wait_for = fixtures.InputTransport.wait_for
    received = fixtures.InputTransport.received

    def setUp(self):
        super().setUp()
        self.env['INPUT_FIXTURE'] = str(self.root)
        (self.root / 'agent.py').write_text(fixtures.FAKE_AGENT)
        self.name = 'codex/input-test'
        self.addCleanup(self.stop_agent)
        self.tmux('new-session', '-d', '-s', self.name, '-x', '100', '-y', '30', 'python3', str(self.root / 'agent.py'))
        self.wait_for(lambda: (self.root / 'agent-pid').exists() and (self.root / 'agent-pid').read_text().strip())
        pid = int((self.root / 'agent-pid').read_text())
        start = subprocess.check_output(['ps', '-p', str(pid), '-o', 'lstart='],
                                        env=dict(self.env, LC_ALL='C', TZ='UTC'), text=True).strip()
        pane = self.tmux('display-message', '-p', '-t', '=' + self.name + ':', '#{pane_id}').strip()
        run_id = str(uuid.uuid4())
        self.tmux('set-option', '-t', '=' + self.name + ':', '@hgs_run', run_id)
        record = dict(version=1, name=self.name, agent='codex', run_id=run_id, conversation_id=str(uuid.uuid4()),
                      pane=pane, pid=pid, process_start=start, activity='idle', phase='idle', active_tools={},
                      subagents={}, last_event_at=time.time(), created=time.time())
        state = self.root / 'b' / 'state'
        state.mkdir(exist_ok=True)
        self.record = record
        self.record_path = state / (hashlib.sha256(self.name.encode()).hexdigest() + '.json')
        self.record_path.write_text(json.dumps(record))

    def tmux(self, *args, check=True):
        result = subprocess.run([str(self.root / '.local/bin/tmux'), *args], env=self.env,
                                capture_output=True, text=True, timeout=5)
        if check:
            self.assertEqual(result.returncode, 0, result.stderr)
        return result.stdout

    def stop_agent(self):
        # Like InputTransport.tearDown: only this socket's server, then wait for its panes.
        pids = {pid for pid in self.tmux('list-panes', '-a', '-F', '#{pane_pid}', check=False).splitlines() if pid.isdigit()}
        self.tmux('kill-server', check=False)
        deadline = time.monotonic() + 2
        while pids:
            status = subprocess.run(['ps', '-p', ','.join(sorted(pids)), '-o', 'pid=,stat='],
                                    capture_output=True, text=True, timeout=5)
            pids = {parts[0] for line in status.stdout.splitlines()
                    if len(parts := line.split()) >= 2 and not parts[1].startswith(('Z', 'X'))}
            if not pids or time.monotonic() >= deadline:
                break
            time.sleep(.02)
        self.assertFalse(pids, 'owned fixture panes survived tmux shutdown')

    def test_send_reaches_a_session_on_another_machine(self):
        ref = self.overmind('a', 'create', '--json', data={'project': 'demo', 'prompt': 'Fix the drafts', 'stage': 'Queue'})['task']['ref']
        b = self.overmind('b', 'status')['machine']
        result = self.overmind('a', 'send', '--json', data={'task': ref, 'session': {'machine': b, 'name': self.name}})
        self.assertEqual(result['delivery'], 'sent', result)
        self.wait_for(lambda: f'[Overmind {ref}]'.encode() in self.received())
        on_a = self.overmind('a', 'show', ref)['task']
        self.assertEqual((on_a['stage'], on_a['assignee']['machine']), ('Design', b))
        self.assertIn('[sent]', [j['marker'] for j in on_a['journal']])
        claim = next(j for j in on_a['journal'] if j['marker'] == '[claim]')
        self.assertEqual(claim['actor']['machine'], b)  # the host wrote the claim
        self.assertEqual([j['marker'] for j in on_a['journal']].count('[claim]'), 1)
        sent = next(j for j in on_a['journal'] if j['marker'] == '[sent]')
        self.assertEqual(sent['actor']['machine'], b)
        self.assertEqual(result['task']['ref'], ref)

    def made_on_b(self):
        """A task created on b, where `append` sets it up; returns its uuid."""
        return self.overmind('b', 'create', '--json', data={'project': 'demo', 'prompt': 'Fix the drafts', 'stage': 'Queue'})['task']['uuid']

    def b_state(self):
        return self.root / 'b' / 'state'

    def b_machine(self):
        return self.overmind('b', 'status')['machine']

    def agent_on_b(self, session):
        return {'kind': 'agent', 'machine': self.b_machine(), 'session': session}

    def build_on_b(self, session):
        append(self.b_state(), 'transition', {'action': 'claim', 'to': 'Design', 'fields': {},
                                              'session': {'machine': self.b_machine(), 'name': session}})
        append(self.b_state(), 'transition', {'action': 'advance', 'to': 'Build', 'fields': {'spec': 's'}}, actor=self.agent_on_b(session))

    def reach_review_on_b(self, session):
        self.build_on_b(session)
        append(self.b_state(), 'transition', {'action': 'advance', 'to': 'Review', 'fields': {'worklog': 'w', 'evidence': 'e'}},
               actor=self.agent_on_b(session))

    def ref_on_a(self, task):
        self.call('a', 'sync')
        return next(t['ref'] for t in self.overmind('a', 'board', '--json', data={'project': 'demo'})['tasks'] if t['uuid'] == task)

    def test_review_send_reaches_a_session_on_another_machine(self):
        task = self.made_on_b()
        self.reach_review_on_b('codex/other')
        ref = self.ref_on_a(task)
        result = self.overmind('a', 'review-send', '--json', data={'task': ref, 'session': {'machine': self.b_machine(), 'name': self.name}})
        self.assertEqual((result['delivery'], result['task']['reviewer']['name'], result['task']['ref']), ('sent', self.name, ref), result)
        self.wait_for(lambda: b'Review: Fix the drafts' in self.received())
        on_a = self.overmind('a', 'show', ref)['task']
        self.assertEqual((on_a['stage'], on_a['reviewer']['name']), ('Review', self.name))

    def test_rework_send_reaches_the_assignee_on_another_machine(self):
        task = self.made_on_b()
        self.reach_review_on_b(self.name)
        append(self.b_state(), 'reviewer_set', {'session': {'machine': self.b_machine(), 'name': 'codex/other'}})
        append(self.b_state(), 'transition', {'action': 'needs_work', 'to': 'Build', 'fields': {'report': 'tests missing'}},
               actor=self.agent_on_b('codex/other'))
        ref = self.ref_on_a(task)
        result = self.overmind('a', 'rework-send', '--json', data={'task': ref})
        self.assertEqual((result['delivery'], result['task']['ref']), ('sent', ref), result)
        self.wait_for(lambda: b'tests missing' in self.received())
        self.assertIn(b'Rework: Fix the drafts', self.received())

    def test_answer_reaches_the_assignee_on_another_machine(self):
        task = self.made_on_b()
        self.build_on_b(self.name)
        append(self.b_state(), 'transition', {'action': 'call_human', 'to': 'Your Call', 'fields': {'question': 'REST or gRPC?'}},
               actor=self.agent_on_b(self.name))
        ref = self.ref_on_a(task)
        result = self.overmind('a', 'answer', '--json', data={'task': ref, 'answer': 'REST'})
        self.assertEqual((result['delivery'], result['task']['stage'], result['task']['ref']), ('sent', 'Build', ref), result)
        self.wait_for(lambda: b'REST' in self.received())

    def test_a_refusal_about_the_assignee_names_the_task_field(self):
        task = self.made_on_b()
        self.build_on_b(self.name)
        append(self.b_state(), 'transition', {'action': 'call_human', 'to': 'Your Call', 'fields': {'question': 'q'}},
               actor=self.agent_on_b(self.name))
        ref = self.ref_on_a(task)
        self.record_path.unlink()  # b no longer tracks the session
        result = self.overmind_raw('a', 'answer', '--json', data={'task': ref, 'answer': 'REST'})
        error = json.loads(result.stdout)['error']
        self.assertEqual((result.returncode, error['code'], error['field']), (1, 'unknown_session', 'task'), result.stdout)

    def test_a_host_refusal_comes_back_and_a_confirmation_goes_through(self):
        ref = self.overmind('a', 'create', '--json', data={'project': 'demo', 'prompt': 'x', 'stage': 'Queue'})['task']['ref']
        self.record['activity'] = 'busy'
        self.record_path.write_text(json.dumps(self.record))
        request = {'task': ref, 'session': {'machine': self.b_machine(), 'name': self.name}}
        refused = self.overmind_raw('a', 'send', '--json', data=request)
        error = json.loads(refused.stdout)['error']
        self.assertEqual((refused.returncode, error['code'], error['field']), (1, 'agent_busy', 'confirm'), refused.stdout)
        self.assertEqual(self.overmind('a', 'show', ref)['task']['stage'], 'Queue')
        sent = self.overmind('a', 'send', '--json', data=dict(request, confirm=['agent_busy']))
        self.assertEqual(sent['delivery'], 'sent', sent)

    def test_forward_failures_are_refusals(self):
        ref = self.overmind('a', 'create', '--json', data={'project': 'demo', 'prompt': 'x', 'stage': 'Queue'})['task']['ref']
        request = {'task': ref, 'session': {'machine': self.b_machine(), 'name': self.name}}

        def refusal():
            result = self.overmind_raw('a', 'send', '--json', data=request)
            self.assertEqual(result.returncode, 1, result.stderr + result.stdout)
            return json.loads(result.stdout)['error']
        # A host from before forwarded deliveries rejects the call before running anything.
        (self.root / 'no-deliver-b').write_text('')
        error = refusal()
        self.assertEqual((error['code'], error['field']), ('host_outdated', 'session'), error)
        (self.root / 'no-deliver-b').unlink()
        # The exchange fails after hello: nothing was attempted on the host.
        (self.root / 'bad-exchange-b').write_text('')
        error = refusal()
        self.assertEqual(error['code'], 'host_sync_failed', error)
        self.assertIn('nothing was attempted', error['message'])
        (self.root / 'bad-exchange-b').unlink()
        # The machine behind the alias is another one now.
        catalog = self.root / 'b' / 'swarm' / 'catalog.json'
        data = json.loads(catalog.read_text())
        data['node_id'] = str(uuid.uuid4())
        catalog.write_text(json.dumps(data))
        error = refusal()
        self.assertEqual((error['code'], error['field']), ('host_changed', 'session'), error)
        self.assertEqual(self.overmind('a', 'show', ref)['task']['stage'], 'Queue')
        self.assertEqual(self.received(), b'')

    def test_a_host_refuses_a_session_on_another_machine_in_the_callers_words(self):
        ref = self.overmind('a', 'create', '--json', data={'project': 'demo', 'prompt': 'x', 'stage': 'Queue'})['task']['ref']
        self.call('a', 'sync')
        a = self.overmind('a', 'status')['machine']
        answer = self.call('b', 'overmind-deliver', data={'command': 'send', 'request': {'task': ref, 'session': {'machine': a, 'name': self.name}}})
        self.assertEqual(answer['error']['message'], f'the session is not on {self.b_machine()}; no further forwarding')

    def test_a_forwarded_result_is_remembered_on_the_caller(self):
        ref = self.overmind('a', 'create', '--json', data={'project': 'demo', 'prompt': 'x', 'stage': 'Queue'})['task']['ref']
        b = self.overmind('b', 'status')['machine']
        request = {'request_id': str(uuid.uuid4()), 'task': ref, 'session': {'machine': b, 'name': self.name}}
        first = self.overmind('a', 'send', '--json', data=request)
        self.assertEqual(first['delivery'], 'sent', first)
        (self.root / 'calls').write_text('')
        # An identical retry replays here; the same request_id with another request is refused here.
        self.assertEqual(self.overmind('a', 'send', '--json', data=request), first)
        other = dict(request, session={'machine': b, 'name': 'codex/other'})
        refused = self.overmind_raw('a', 'send', '--json', data=other)
        self.assertEqual((refused.returncode, json.loads(refused.stdout)['error']['code']), (1, 'request_reused'))
        self.assertEqual((self.root / 'calls').read_text(), '')  # the host was never contacted
        self.assertEqual(self.received().count(b'[Overmind '), 1)

    def test_a_remote_send_brings_the_task_images_along(self):
        attachment, _ = self.image(1024)
        ref = self.overmind('a', 'create', '--json', data={'project': 'demo', 'prompt': 'Look', 'stage': 'Queue', 'attachments': [attachment]})['task']['ref']
        b = self.overmind('b', 'status')['machine']
        (self.root / 'calls').write_text('')
        result = self.overmind('a', 'send', '--json', data={'task': ref, 'session': {'machine': b, 'name': self.name}})
        self.assertEqual(result['delivery'], 'sent', result)
        self.wait_for(lambda: b'Inspect the attached image' in self.received())
        calls = (self.root / 'calls').read_text()
        self.assertIn('b swarm overmind-blobs\n', calls)  # a pushed the image it holds
        self.assertNotIn('swarm overmind-blob\n', calls)  # and b never had to fetch it

    def test_unreachable_and_outdated_hosts_refuse(self):
        ref = self.overmind('a', 'create', '--json', data={'project': 'demo', 'prompt': 'x', 'stage': 'Queue'})['task']['ref']
        b = self.overmind('b', 'status')['machine']
        request = {'task': ref, 'session': {'machine': b, 'name': self.name}}
        (self.root / 'no-overmind-b').write_text('')
        self.assertEqual(json.loads(self.overmind_raw('a', 'send', '--json', data=request).stdout)['error']['code'], 'host_outdated')
        (self.root / 'no-overmind-b').unlink()
        (self.root / 'offline-b').write_text('')
        offline = self.overmind_raw('a', 'send', '--json', data=request)
        self.assertEqual((offline.returncode, json.loads(offline.stdout)['error']['code']), (1, 'host_unreachable'))
        self.assertEqual(self.overmind('a', 'show', ref)['task']['stage'], 'Queue')
        stranger = dict(request, session={'machine': str(uuid.uuid4()), 'name': self.name})
        refused = json.loads(self.overmind_raw('a', 'send', '--json', data=stranger).stdout)['error']
        self.assertEqual((refused['code'], refused['field']), ('host_unreachable', 'session'))
        self.assertEqual(self.received(), b'')

    def test_a_host_never_forwards_a_delivery_again(self):
        ref = self.overmind('a', 'create', '--json', data={'project': 'demo', 'prompt': 'x', 'stage': 'Queue'})['task']['ref']
        self.call('a', 'sync')
        a = self.overmind('a', 'status')['machine']
        request = {'task': ref, 'session': {'machine': a, 'name': self.name}}
        (self.root / 'calls').write_text('')
        # What a's forward would ask of b, for a session that is not on b.
        answer = self.call('b', 'overmind-deliver', data={'command': 'send', 'request': request})
        self.assertEqual((answer['ok'], answer['error']['code']), (False, 'remote_session'))
        self.assertEqual((self.root / 'calls').read_text(), '')  # b called no other machine
        self.assertEqual(self.overmind('b', 'show', ref)['task']['stage'], 'Queue')

    def test_an_image_no_machine_holds_refuses_the_send_before_claiming(self):
        attachment, _ = self.image(1024)
        ref = self.overmind('a', 'create', '--json', data={'project': 'demo', 'prompt': 'x', 'attachments': [attachment], 'stage': 'Queue'})['task']['ref']
        self.call('a', 'sync')
        (self.root / 'offline-a').write_text('')
        result = self.overmind_raw('b', 'send', '--json', data={'task': ref, 'session': {'name': self.name}})
        self.assertEqual(json.loads(result.stdout)['error']['code'], 'attachment_unavailable', result.stderr)
        task = self.overmind('b', 'show', ref)['task']
        self.assertEqual(task['stage'], 'Queue')
        self.assertNotIn('[claim]', [j['marker'] for j in task['journal']])
        self.assertEqual(self.received(), b'')


if __name__ == '__main__':
    unittest.main()
