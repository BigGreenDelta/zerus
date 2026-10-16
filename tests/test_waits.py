"""Explicit wait protocol in private state, without native input or real agents."""
import hashlib
import json
import os
import shlex
from pathlib import Path
import subprocess
import tempfile
import time
import unittest
from urllib.parse import urlencode
import uuid

HGS = Path(os.environ.get('HGS_TEST_BIN', Path(__file__).resolve().parents[1] / 'target/debug/hgs')).resolve()


class IndependentWaits(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='zerus-waits-')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name).resolve()
        self.state = self.root / 'state'; self.state.mkdir()
        self.home = self.root / 'home'; self.home.mkdir()
        sockets = self.root / 'sockets'; sockets.mkdir()
        self.env = {k: v for k, v in os.environ.items() if not k.startswith('HGS_')}
        self.env.update(HOME=str(self.home), USERPROFILE=str(self.home), TMUX_TMPDIR=str(sockets),
                        HGS_CONFIG_DIR=str(self.root/'config'), HGS_STATE_DIR=str(self.state))
        # Metadata commands need the ordinary explicit local-session opt-in on Windows.
        (self.state/'local-sessions').touch()
        self.machine = self.call('graph', 'snapshot', '--json')['machine_id']
        self.name = 'codex/example/coordinator'
        self.record = dict(version=1, name=self.name, agent='codex', run_id='run-one',
                           conversation_id='conversation-one', account_id='fixture', subagents={})
        (self.state/(hashlib.sha256(self.name.encode()).hexdigest()+'.json')).write_text(json.dumps(self.record))
        self.reference = 'zerus://session?' + urlencode(dict(machine=self.machine, host='fixture', name=self.name,
                                                            conversation='conversation-one', run='run-one'))
        self.producer = 'zerus://session?' + urlencode(dict(machine='22222222-2222-4222-8222-222222222222', host='peer',
            name='claude/example/worker', conversation='conversation-two', run='run-two'))

    def call(self, *args, body=None, success=True):
        result = subprocess.run([str(HGS), *args], env=self.env, input=json.dumps(body) if body is not None else None,
                                capture_output=True, text=True, timeout=10)
        if success:
            self.assertEqual(result.returncode, 0, result.stderr)
            return json.loads(result.stdout)
        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertEqual(result.stdout, '')
        return result.stderr

    def begin(self):
        body = dict(request_id=str(uuid.uuid4()), waiter_reference=self.reference,
                    producer_reference=self.producer, label='Review the patch')
        receipt = self.call('wait', 'begin', '--json', body=body)
        self.assertEqual(self.call('wait', 'begin', '--json', body=body)['reply_ticket'], receipt['reply_ticket'])
        return receipt

    def operation(self, receipt, **extra):
        return dict(receipt['reply_ticket'], request_id=str(uuid.uuid4()), **extra)

    def page(self):
        return self.call('wait', 'list', '--json', body=dict(expected_run_id='run-one', expected_conversation_id='conversation-one'))

    def test_exact_ack_result_delivery_and_persistence(self):
        wait = self.begin()
        self.assertEqual(self.page()['items'][0]['state'], 'awaiting_ack')
        ack = self.operation(wait)
        self.assertEqual(self.call('wait', 'ack', '--json', body=ack)['state'], 'awaiting_result')
        finish = self.operation(wait, result_request_id=str(uuid.uuid4()))
        self.call('wait', 'finish', '--json', body=finish, success=False)
        self.assertEqual(self.page()['items'][0]['state'], 'awaiting_result')
        path = self.state/'input_receipts'/f"{finish['result_request_id']}.json"; path.parent.mkdir()
        result = dict(request_id=finish['result_request_id'], status='in_progress', run_id='run-one',
                      conversation_id='conversation-one', submitted_at=time.time(), text='A private result')
        path.write_text(json.dumps(result))
        self.call('wait', 'finish', '--json', body=finish, success=False)
        result['status'] = 'submitted'; result['run_id'] = 'wrong-run'; path.write_text(json.dumps(result))
        self.call('wait', 'finish', '--json', body=finish, success=False)
        result['run_id'] = 'run-one'; path.write_text(json.dumps(result))
        result['agent_id'] = 'child'; path.write_text(json.dumps(result))
        self.call('wait', 'finish', '--json', body=finish, success=False)
        result.pop('agent_id'); result['submitted_at'] = wait['wait']['created_at'] - 1; path.write_text(json.dumps(result))
        self.call('wait', 'finish', '--json', body=finish, success=False)
        result['submitted_at'] = time.time(); path.write_text(json.dumps(result))
        done = self.call('wait', 'finish', '--json', body=finish)
        self.assertEqual(done['state'], 'completed')
        self.assertEqual(self.call('wait', 'finish', '--json', body=finish), done)
        self.assertEqual(self.page()['items'][0]['result_receipt']['request_id'], finish['result_request_id'])
        self.assertNotIn('A private result', json.dumps(self.page()))
        self.assertNotIn('reply_token', json.dumps(self.page()))
        self.assertEqual(self.call('wait', 'ack', '--json', body=self.operation(wait))['state'], 'completed')

    def test_wrong_computer_and_token_cannot_settle_or_retarget(self):
        wait = self.begin()
        for field in ('waiter_machine', 'expected_run_id', 'expected_conversation_id', 'reply_token'):
            bad = self.operation(wait); bad[field] = 'wrong'
            self.call('wait', 'ack', '--json', body=bad, success=False)
        self.assertEqual(self.page()['items'][0]['state'], 'awaiting_ack')
        cancel = self.operation(wait); cancel.pop('reply_token')
        self.assertEqual(self.call('wait', 'cancel', '--json', body=cancel)['state'], 'cancelled')
        self.call('wait', 'ack', '--json', body=self.operation(wait), success=False)

    @unittest.skipIf(os.name == 'nt', 'POSIX native-process ancestry fixture')
    def test_native_caller_can_declare_its_own_wait_but_environment_labels_are_not_proof(self):
        record=dict(self.record,pid=os.getpid(),process_start=subprocess.check_output(
            ['ps','-p',str(os.getpid()),'-o','lstart='],env=dict(self.env,LC_ALL='C',TZ='UTC'),text=True).strip())
        path=self.state/(hashlib.sha256(self.name.encode()).hexdigest()+'.json')
        path.write_text(json.dumps(record))
        self.env.update(HGS_SESSION=self.name,HGS_RUN_ID='run-one')
        body=dict(request_id=str(uuid.uuid4()),producer_reference=self.producer)
        wait=self.call('wait','begin','--json',body=body)
        self.assertEqual(wait['wait']['waiter']['endpoint']['conversation'],'conversation-one')
        record['process_start']='not the owned process';path.write_text(json.dumps(record))
        body['request_id']=str(uuid.uuid4())
        self.call('wait','begin','--json',body=body,success=False)

    def test_unscoped_declarations_and_old_references_fail(self):
        body = dict(request_id=str(uuid.uuid4()), producer_reference=self.producer)
        self.call('wait', 'begin', '--json', body=body, success=False)
        body['waiter_reference'] = self.reference.replace('conversation-one', 'reused-conversation')
        self.call('wait', 'begin', '--json', body=body, success=False)
        body['waiter_reference'] = self.reference
        body['producer_reference'] = self.reference
        self.call('wait', 'begin', '--json', body=body, success=False)

    @unittest.skipIf(os.name == 'nt', 'POSIX remote-shell compatibility contract')
    def test_old_peer_is_probed_without_mutation_or_agent_launch(self):
        peer = self.home/'.local/bin/hgs'; peer.parent.mkdir(parents=True)
        peer.write_text('#!/bin/sh\nif [ "$1" = --help ]; then echo "hgs dirs [path]"; exit 0; fi\n'
                        'echo unexpected > "$HOME/peer-launched"\nexit 99\n')
        peer.chmod(0o700)
        ssh = self.root/'fake-ssh'
        ssh.write_text('#!/usr/bin/env python3\nimport os,sys\nos.execv("/bin/sh",["sh","-c",sys.argv[-1]])\n')
        ssh.chmod(0o700); self.env['HGS_SSH'] = str(ssh)
        error = self.call('@peer', 'wait', 'ack', '--json', body={}, success=False)
        self.assertIn('update', error)
        self.assertFalse((self.home/'peer-launched').exists())
        # The same probe must preserve JSON stdin for a supported peer.
        peer.write_text('#!/bin/sh\nif [ "$1" = --help ]; then echo "hgs [@host] wait"; exit 0; fi\n'
                        'exec ' + shlex.quote(str(HGS)) + ' "$@"\n')
        wait=self.begin()
        self.assertEqual(self.call('@peer','wait','ack','--json',body=self.operation(wait))['state'],'awaiting_result')


if __name__ == '__main__':
    unittest.main()
