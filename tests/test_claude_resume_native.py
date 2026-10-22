"""Real Claude cold-resume consent in a private home/socket, with synthetic history.

Run with HGS_CLAUDE_TEST_BIN=/absolute/path/to/claude. No real account or model.
"""
import datetime
import hashlib
import json
import os
import shlex
import subprocess
import tempfile
import time
import unittest
import uuid
from pathlib import Path

import test_input as fixtures
import test_questions as questions
import test_claude_questions_native as native


@unittest.skipUnless(native.CLAUDE, 'Set HGS_CLAUDE_TEST_BIN for isolated native resume coverage')
class NativeClaudeResume(unittest.TestCase):
    script = fixtures.InputTransport.script
    tmux = fixtures.InputTransport.tmux
    write_record = fixtures.InputTransport.write_record
    wait_for = native.NativeClaudeQuestions.wait_for
    screen = native.NativeClaudeQuestions.screen
    inspect = questions.Questions.inspect
    answer = questions.Questions.answer

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='hgs-claude-resume-native-')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name).resolve()
        self.bin = self.root / '.local/bin'
        self.bin.mkdir(parents=True)
        self.state = self.root / 'state'
        self.state.mkdir()
        work = self.root / 'work'
        work.mkdir()
        config = self.root / '.claude'
        config.mkdir()
        self.conversation_id = str(uuid.uuid4())
        # The native quota estimator reads subscription metadata and cached
        # feature values. These are invented fixture values, never user settings.
        (self.root / '.claude.json').write_text(json.dumps(dict(
            hasCompletedOnboarding=True, hasSeenAutoDefaultNudge=True, theme='dark',
            projects={str(work): dict(hasTrustDialogAccepted=True)},
            cachedGrowthBookFeatures={'tengu_amber_tally': dict(
                minPercent=1, fiveHourCredits={'default_claude_pro': 100},
                creditsPerMillionTokens={'claude-sonnet-4-6': 100}),
                'tengu_drifting_nova': True, 'tengu_stateful_mitten': False})))
        (config / '.credentials.json').write_text(json.dumps({'claudeAiOauth': dict(
            accessToken='fixture-token', refreshToken='fixture-refresh',
            expiresAt=int((time.time() + 86400) * 1000),
            scopes=['user:inference', 'user:profile'], subscriptionType='pro',
            rateLimitTier='default_claude_pro')}))
        stamp = (datetime.datetime.now(datetime.timezone.utc) - datetime.timedelta(hours=2)).isoformat()
        first = str(uuid.uuid4())
        history = config / 'projects' / str(work).replace('/', '-') / (self.conversation_id + '.jsonl')
        history.parent.mkdir(parents=True)
        common = dict(sessionId=self.conversation_id, cwd=str(work), timestamp=stamp,
                      isSidechain=False, userType='external', version='2.1.296')
        history.write_text('\n'.join(json.dumps(row) for row in [
            dict(common, type='user', uuid=first, parentUuid=None,
                 message=dict(role='user', content='Synthetic earlier request.')),
            dict(common, type='assistant', uuid=str(uuid.uuid4()), parentUuid=first,
                 message=dict(id='fixture-earlier-reply', type='message', role='assistant',
                              model='claude-sonnet-4-5', content=[dict(type='text', text='Synthetic earlier reply.')],
                              stop_reason='end_turn', stop_sequence=None,
                              usage=dict(input_tokens=250000, output_tokens=50,
                                         cache_read_input_tokens=0, cache_creation_input_tokens=0)))]) + '\n')
        self.socket = self.root / 'socket'
        self.script('tmux', '#!/bin/sh\nexec ' + shlex.quote(fixtures.TMUX) + ' -S ' +
                    shlex.quote(str(self.socket)) + ' -f /dev/null "$@"\n')
        self.env = {k: v for k, v in os.environ.items() if not k.startswith('HGS_')}
        self.env.update(HOME=str(self.root), HGS_STATE_DIR=str(self.state),
                        HGS_CONFIG_DIR=str(self.root / 'config'), HGS_SELF='test',
                        PATH=str(self.bin) + ':' + os.environ['PATH'])
        self.env.pop('TMUX', None)
        self.env.pop('TMUX_PANE', None)
        self.name = 'claude/resume-test'
        self.run_id = str(uuid.uuid4())
        self.addCleanup(lambda: fixtures.InputTransport.tearDown(self))
        width, height = ('80', '25') if 'new_conversation' in self._testMethodName else ('100', '35')
        self.tmux('new-session', '-d', '-s', self.name, '-x', width, '-y', height, '-c', str(work),
                  'env', '-i', 'HOME=' + str(self.root), 'PATH=' + os.environ['PATH'],
                  'SHELL=/bin/bash', 'TERM=xterm-256color', 'LANG=C.UTF-8',
                  'CLAUDE_CODE_DISABLE_NONESSENTIAL_TRAFFIC=1', 'DISABLE_AUTOUPDATER=1',
                  'CLAUDE_CODE_GB_DISK_CACHE_WHEN_TELEMETRY_OFF=1',
                  'ANTHROPIC_BASE_URL=http://127.0.0.1:1',
                  native.CLAUDE, '--resume', str(history), '--model', 'claude-sonnet-4-6')
        self.pane = self.tmux('display-message', '-p', '-t', '=' + self.name + ':', '#{pane_id}').strip()
        self.wait_for(lambda: 'Esc to resume' in self.screen())
        pid = int(self.tmux('display-message', '-p', '-t', self.pane, '#{pane_pid}'))
        start = subprocess.check_output(['ps', '-p', str(pid), '-o', 'lstart='],
                                        env=dict(self.env, LC_ALL='C', TZ='UTC'), text=True).strip()
        self.tmux('set-option', '-t', '=' + self.name + ':', '@hgs_run', self.run_id)
        self.record_path = self.state / (hashlib.sha256(self.name.encode()).hexdigest() + '.json')
        self.record = dict(version=1, name=self.name, agent='claude', run_id=self.run_id,
                           run_identity_version=1, supervisor={}, startup_kind='resume',
                           conversation_id=self.conversation_id, expected_id=self.conversation_id,
                           launch_dir=str(work), pane=self.pane, pid=pid, process_start=start,
                           activity='unknown', phase='unknown', active_tools={}, subagents={}, last_event_at=0)
        self.write_record()
        self.card = self.inspect()['pending_questions'][0]
        self.assertTrue(self.card['can_answer'], self.screen())
        # Record subsequent mutations only after native fixture setup finishes.
        logger = self.bin / 'tmux-log.py'
        logger.write_text('import json,pathlib,subprocess,sys\n' +
                          'args=sys.argv[1:]\n' +
                          "if args and args[0]=='send-keys':\n" +
                          ' with pathlib.Path(' + repr(str(self.root / 'sent-keys')) +
                          ").open('a') as log:log.write(json.dumps(args)+'\\n')\n" +
                          'sys.exit(subprocess.call(' + repr([fixtures.TMUX, '-S', str(self.socket), '-f', '/dev/null']) + '+args))\n')
        self.script('tmux', '#!/bin/sh\nexec python3 ' + shlex.quote(str(logger)) + ' "$@"\n')

    def payload(self, choice):
        return dict(request_id=str(uuid.uuid4()), expected_run_id=self.run_id,
                    expected_conversation_id=self.conversation_id,
                    question_id=self.card['question_id'], expected_question_hash=self.card['question_hash'],
                    answers=[dict(question_id='resume', selected_option_ids=[choice], text='')])

    def verify_answer(self, choice):
        self.assertEqual(self.inspect()['phase'], 'approval')
        self.assertFalse((self.root / 'sent-keys').exists())
        payload = self.payload(choice)
        receipt = self.answer(payload)
        self.assertEqual(receipt['status'], 'answered')
        self.assertEqual(self.inspect()['pending_questions'], [])
        sent = (self.root / 'sent-keys').read_bytes()
        self.assertEqual(self.answer(payload), receipt)
        self.assertEqual((self.root / 'sent-keys').read_bytes(), sent)

    def test_explicit_resume_on_real_native_panel(self):
        self.verify_answer('resume')
        self.assertIn('Synthetic earlier reply.', self.screen())

    def test_explicit_new_conversation_on_real_native_panel(self):
        self.verify_answer('new_conversation')
        self.assertNotIn('Synthetic earlier reply.', self.screen())


if __name__ == '__main__':
    unittest.main()
