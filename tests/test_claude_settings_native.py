"""Real Claude /model picker in a private home and socket, with synthetic history.

Run with HGS_CLAUDE_TEST_BIN=/absolute/path/to/claude (otherwise skipped). A
fixture API key and an unused loopback endpoint keep every request local.
"""
import datetime
import hashlib
import json
import os
import re
import shlex
import subprocess
import tempfile
import time
import unittest
import uuid
from pathlib import Path

import test_input as fixtures
import test_claude_questions_native as native

EFFORTS = [dict(id=value) for value in ['low', 'medium', 'high', 'xhigh', 'max']]
CATALOG = dict(version=2, fetchedAt=0, staleAt=0, catalog=dict(surface='cc', state={}, config=dict(id='cc', models=[
    dict(id='claude-opus-5-5', name='Opus 5.5', short_name='Opus', section='main', thinking=dict(type='effort', effort_options=EFFORTS)),
    dict(id='claude-fable-5-1', name='Fable 5.1', short_name='Fable', section='main', thinking=dict(type='effort', effort_options=EFFORTS)),
    dict(id='claude-sonnet-5-5', name='Sonnet 5.5', short_name='Sonnet', section='main', thinking=dict(type='effort', effort_options=EFFORTS)),
    dict(id='claude-haiku-5-5', name='Haiku 5.5', short_name='Haiku', section='main', thinking=dict(type='effort', effort_options=EFFORTS))])))


@unittest.skipUnless(native.CLAUDE, 'Set HGS_CLAUDE_TEST_BIN for isolated native model settings coverage')
class NativeClaudeSettings(unittest.TestCase):
    script = fixtures.InputTransport.script
    tmux = fixtures.InputTransport.tmux
    write_record = fixtures.InputTransport.write_record
    wait_for = native.NativeClaudeQuestions.wait_for
    screen = native.NativeClaudeQuestions.screen

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='hgs-claude-settings-native-')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name).resolve()
        self.bin = self.root / '.local/bin'
        self.bin.mkdir(parents=True)
        self.state = self.root / 'state'
        self.state.mkdir()
        work = self.root / 'work'
        work.mkdir()
        self.config = self.root / '.claude'
        (self.config / 'cache/model-catalog').mkdir(parents=True)
        (self.config / 'cache/model-catalog/fixture-000000000000-cc.json').write_text(json.dumps(CATALOG))
        (self.root / '.claude.json').write_text(json.dumps(dict(
            hasCompletedOnboarding=True, hasSeenAutoDefaultNudge=True, theme='dark',
            customApiKeyResponses=dict(approved=['fixture-key'], rejected=[]),
            projects={str(work): dict(hasTrustDialogAccepted=True, hasCompletedProjectOnboarding=True)})))
        self.settings_file = self.config / 'settings.json'
        self.settings_file.write_text('{}\n')
        self.conversation_id = str(uuid.uuid4())
        self.transcript = self.config / 'projects' / re.sub(r'[^A-Za-z0-9]', '-', str(work)) / (self.conversation_id + '.jsonl')
        self.transcript.parent.mkdir(parents=True)
        resumed = 'resumed' in self._testMethodName
        if resumed:
            # One cached reply makes Claude warn before a change re-reads history.
            stamp = datetime.datetime.now(datetime.timezone.utc).isoformat()
            first = str(uuid.uuid4())
            common = dict(sessionId=self.conversation_id, cwd=str(work), timestamp=stamp,
                          isSidechain=False, userType='external', version='2.1.296')
            self.transcript.write_text('\n'.join(json.dumps(row) for row in [
                dict(common, type='user', uuid=first, parentUuid=None,
                     message=dict(role='user', content='Synthetic earlier request.')),
                dict(common, type='assistant', uuid=str(uuid.uuid4()), parentUuid=first,
                     requestedModel='claude-opus-5-5', effort='medium',
                     message=dict(id='fixture-reply', type='message', role='assistant', model='claude-opus-5-5',
                                  content=[dict(type='text', text='Synthetic earlier reply.')],
                                  stop_reason='end_turn', stop_sequence=None,
                                  usage=dict(input_tokens=10, output_tokens=50,
                                             cache_read_input_tokens=0, cache_creation_input_tokens=5000))),
                dict(type='cost-state', sessionId=self.conversation_id, totalCostUSD=0.01, totalAPIDuration=1000,
                     totalAPIDurationWithoutRetries=1000, totalToolDuration=0, totalLinesAdded=0, totalLinesRemoved=0,
                     totalDuration=2000, startTime=int(time.time() * 1000) - 5000,
                     modelUsage={'claude-opus-5-5': dict(inputTokens=10, outputTokens=50, cacheReadInputTokens=0,
                                                         cacheCreationInputTokens=5000, webSearchRequests=0, costUSD=0.01)},
                     hasUnknownModelCost=False)]) + '\n')
        self.socket = self.root / 'socket'
        self.script('tmux', '#!/bin/sh\nexec ' + shlex.quote(fixtures.TMUX) + ' -S ' +
                    shlex.quote(str(self.socket)) + ' -f /dev/null "$@"\n')
        self.env = {k: v for k, v in os.environ.items() if not k.startswith('HGS_')}
        self.env.update(HOME=str(self.root), HGS_STATE_DIR=str(self.state),
                        HGS_CONFIG_DIR=str(self.root / 'config'), HGS_SELF='test',
                        PATH=str(self.bin) + ':' + os.environ['PATH'])
        self.env.pop('TMUX', None)
        self.env.pop('TMUX_PANE', None)
        self.name = 'claude/settings-test'
        self.run_id = str(uuid.uuid4())
        self.addCleanup(lambda: fixtures.InputTransport.tearDown(self))
        if 'hook' in self._testMethodName:
            # The entry hgs registers for Claude Code 2.1.296, run as this run's hook.
            hook = dict(HGS_SESSION=self.name, HGS_RUN_ID=self.run_id, HGS_STATE_DIR=str(self.state),
                        HGS_CONFIG_DIR=str(self.root / 'config'), PATH=self.env['PATH'])
            command = ('env ' + ' '.join(shlex.quote(key + '=' + value) for key, value in hook.items()) + ' '
                       + shlex.quote(str(fixtures.HGS)) + ' __state hook')
            self.settings_file.write_text(json.dumps(dict(hooks=dict(PostModelSwitch=[
                dict(hooks=[dict(type='command', command=command, timeout=5)])]))))
        launch = ['--resume', self.conversation_id] if resumed else ['--session-id', self.conversation_id]
        self.tmux('new-session', '-d', '-s', self.name, '-x', '120', '-y', '36', '-c', str(work),
                  'env', '-i', 'HOME=' + str(self.root), 'PATH=' + os.environ['PATH'],
                  'SHELL=/bin/bash', 'TERM=xterm-256color', 'LANG=C.UTF-8',
                  'CLAUDE_CODE_DISABLE_NONESSENTIAL_TRAFFIC=1', 'DISABLE_AUTOUPDATER=1',
                  'ANTHROPIC_API_KEY=fixture-key', 'ANTHROPIC_BASE_URL=http://127.0.0.1:1',
                  native.CLAUDE, *launch)
        self.pane = self.tmux('display-message', '-p', '-t', '=' + self.name + ':', '#{pane_id}').strip()
        self.wait_for(lambda: '· /effort' in self.screen())
        if resumed:
            self.assertIn('Synthetic earlier reply.', self.screen())
        pid = int(self.tmux('display-message', '-p', '-t', self.pane, '#{pane_pid}'))
        start = subprocess.check_output(['ps', '-p', str(pid), '-o', 'lstart='],
                                        env=dict(self.env, LC_ALL='C', TZ='UTC'), text=True).strip()
        self.tmux('set-option', '-t', '=' + self.name + ':', '@hgs_run', self.run_id)
        self.record_path = self.state / (hashlib.sha256(self.name.encode()).hexdigest() + '.json')
        self.record = dict(version=1, name=self.name, agent='claude', run_id=self.run_id,
                           conversation_id=self.conversation_id, transcript=str(self.transcript),
                           agent_home=str(self.config), model='claude-opus-5-5', effort='medium',
                           launch_dir=str(work), pane=self.pane, pid=pid, process_start=start,
                           activity='idle', phase='idle', active_tools={}, subagents={}, last_event_at=time.time())
        self.write_record()
        # Record only the adapter's input, after native startup.
        logger = self.bin / 'tmux-log.py'
        logger.write_text('import json,pathlib,subprocess,sys\n' +
                          'args=sys.argv[1:]\n' +
                          "if args and args[0]=='send-keys':\n" +
                          ' with pathlib.Path(' + repr(str(self.root / 'sent-keys')) +
                          ").open('a') as log:log.write(json.dumps(args)+'\\n')\n" +
                          'sys.exit(subprocess.call(' + repr([fixtures.TMUX, '-S', str(self.socket), '-f', '/dev/null']) + '+args))\n')
        self.script('tmux', '#!/bin/sh\nexec python3 ' + shlex.quote(str(logger)) + ' "$@"\n')

    def run_hgs(self, *args, payload=None):
        return subprocess.run([str(fixtures.HGS), *args], env=self.env, text=True, capture_output=True, timeout=60,
                              input=None if payload is None else json.dumps(payload))

    def inspect(self):
        result = self.run_hgs('inspect', self.name)
        self.assertEqual(result.returncode, 0, result.stderr)
        return json.loads(result.stdout)

    def change(self, model, effort):
        payload = dict(request_id=str(uuid.uuid4()), expected_run_id=self.run_id,
                       expected_conversation_id=self.conversation_id, model=model, effort=effort)
        result = self.run_hgs('settings', self.name, '--json', payload=payload)
        self.assertEqual(result.returncode, 0, result.stderr + '\n' + self.screen())
        return json.loads(result.stdout)

    def keys(self):
        path = self.root / 'sent-keys'
        return [json.loads(line)[3:] for line in path.read_text().splitlines()] if path.exists() else []

    def receipts(self):
        rows = native.records(self.transcript)
        return [row['message']['content'] for row in rows
                if row.get('type') == 'user' and isinstance(row.get('message', {}).get('content'), str)
                and 'Set model to' in row['message']['content']]

    def test_fresh_session_changes_model_and_effort_without_defaults(self):
        info = self.inspect()
        self.assertTrue(info['settings_change_supported'], info['settings_change_reason'])
        before = self.settings_file.read_bytes()
        self.assertEqual(self.change('claude-sonnet-5-5', 'high')['status'], 'applied')
        self.assertEqual(self.receipts(), ['<local-command-stdout>Set model to `Sonnet 5.5` for this session only with `high` effort</local-command-stdout>'])
        keys = self.keys()
        self.assertEqual(keys[0], ['-l', '/model'])
        self.assertEqual(keys[-1], ['s'])
        self.assertNotIn(['Enter'], keys[2:])
        self.assertIn('● high · /effort', self.screen())
        info = self.inspect()
        self.assertEqual((info['model'], info['effort']), ('claude-sonnet-5-5', 'high'))
        # Effort alone, on the session's own row of the picker.
        self.assertEqual(self.change('claude-sonnet-5-5', 'low')['status'], 'applied')
        self.assertIn('○ low · /effort', self.screen())
        self.assertEqual(self.inspect()['effort'], 'low')
        self.assertEqual(self.settings_file.read_bytes(), before)

    def test_terminal_model_change_reaches_hgs_through_its_hook(self):
        before = self.settings_file.read_bytes()
        # The person changes the model in Terminal, for this session only.
        self.tmux('send-keys', '-t', self.pane, '-l', '/model')
        time.sleep(.3)
        self.tmux('send-keys', '-t', self.pane, 'Enter')
        self.wait_for(lambda: 's to use this session only' in self.screen())
        for _ in range(8):
            row = next((line for line in self.screen().splitlines() if re.match(r'\s*❯ \d\.', line)), '')
            if 'Fable' in row:
                break
            self.tmux('send-keys', '-t', self.pane, 'Down')
            time.sleep(.2)
        self.assertIn('Fable', row, self.screen())
        self.tmux('send-keys', '-t', self.pane, 's')
        # Claude's PostModelSwitch names the exact ID, not the picker's display name.
        record = lambda: json.loads(self.record_path.read_text())
        self.wait_for(lambda: record()['model'] == 'claude-fable-5-1')
        self.assertEqual(record()['model_source_path'], str(self.transcript))
        self.assertEqual(self.inspect()['model'], 'claude-fable-5-1')
        self.assertEqual(self.settings_file.read_bytes(), before)

    def test_resumed_conversation_confirms_claudes_cache_notice(self):
        before = self.settings_file.read_bytes()
        self.assertEqual(self.change('claude-haiku-5-5', 'medium')['status'], 'applied')
        keys = self.keys()
        self.assertEqual(keys[-2:], [['s'], ['Enter']])
        self.assertTrue(self.receipts()[-1].startswith('<local-command-stdout>Set model to `Haiku 5.5` for this session only'))
        self.assertEqual(self.inspect()['model'], 'claude-haiku-5-5')
        self.assertEqual(self.settings_file.read_bytes(), before)


if __name__ == '__main__':
    unittest.main()
