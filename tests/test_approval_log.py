"""Private journal CLI routing and durability; never sends native input."""
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

HGS = Path(os.environ.get('HGS_TEST_BIN', Path(__file__).resolve().parents[1] / 'target/debug/hgs')).resolve()


class ApprovalLog(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='zerus-approval-log-')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.state = self.root / 'state'
        self.env = {k: v for k, v in os.environ.items() if not k.startswith('HGS_')}
        self.env.update(HOME=str(self.root), USERPROFILE=str(self.root),
                        HGS_STATE_DIR=str(self.state), HGS_CONFIG_DIR=str(self.root / 'config'),
                        HGS_SELF='desktop-a', HGS_PEERS='')

    def call(self, operation, payload, success=True, prefix=()):
        result = subprocess.run([str(HGS), *prefix, 'approval-log', operation],
                                input=json.dumps(payload), env=self.env, text=True,
                                capture_output=True, timeout=10)
        if success:
            self.assertEqual(result.returncode, 0, result.stderr)
            return json.loads(result.stdout)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(result.stdout, '')
        return result.stderr

    def test_empty_reads_and_invalid_requests_never_launch_an_agent(self):
        self.assertEqual(self.call('list', {}), dict(entries=[], next=None, previous=None))
        self.assertFalse((self.state / 'approval-audit').exists())
        self.assertIn('JSON object', self.call('list', [], success=False))
        self.assertIn('local', self.call('list', {}, prefix=('@remote-example',), success=False))
        self.assertIn('dry-run', self.call('list', {}, prefix=('--dry-run',), success=False))

    def test_reservation_survives_processes_and_keeps_question_and_outcome(self):
        question = dict(approval=True, can_answer=True, approval_choices=True,
                        source='claude_tool_approval', answer_transport='claude_tui',
                        question_id='fixture-question', question_hash='a' * 64,
                        run_id='fixture-run', conversation_id='fixture-conversation',
                        questions=[dict(id='approval', question='Run this tool?', body='printf fixture',
                                        options=[dict(id='yes', label='Yes', scope='once'),
                                                 dict(id='no', label='No', scope='once')])])
        payload = dict(host='desktop-a', session='claude/sample/review', provider='claude', question=question)
        first = self.call('begin', payload)
        self.assertTrue(first['fresh'])
        again = self.call('begin', payload)
        self.assertFalse(again['fresh'])
        self.assertEqual(first['request_id'], again['request_id'])
        outcome = dict(id=first['id'], request_id=first['request_id'], status='unknown', detail='delivery uncertain')
        self.call('finish', outcome)
        self.call('finish', outcome)
        self.call('finish', dict(outcome, status='approved'), success=False)
        self.assertFalse(self.call('begin', payload)['fresh'])
        detail = self.call('detail', dict(id=first['id']))
        self.assertEqual(detail['question'], question)
        self.assertEqual(detail['answers'], first['answers'])
        self.assertEqual(detail['status'], 'unknown')
        self.assertEqual(len(self.call('list', dict(host='desktop-a', session='claude/sample/review'))['entries']), 1)
        self.assertEqual(self.call('list', dict(session='absent'))['entries'], [])
        for invalid in (dict(before='oops'), dict(after=0), dict(before=1, after=1), dict(host=[])):
            self.call('list', invalid, success=False)


if __name__ == '__main__':
    unittest.main()
