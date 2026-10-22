"""Real Claude Code started by hgs reports its context window through its status line.

Run with HGS_CLAUDE_TEST_BIN=/absolute/path/to/claude (otherwise skipped). The agent runs
in a private home, state folder and tmux socket against the keyless loopback API of
tests/model_api_fixture.py; no account, model or live session is reached.
"""
import json
import os
from pathlib import Path
import shlex
import subprocess
import sys
import time
import unittest
import uuid

import test_pause
from test_claude_settings_native import CATALOG

CLAUDE = os.environ.get('HGS_CLAUDE_TEST_BIN')
FIXTURE = Path(__file__).resolve().parent / 'model_api_fixture.py'
USER_LINE = 'cat > "$HOME/status-input.json"; printf "Fixture status line"'


@unittest.skipUnless(CLAUDE and test_pause.TMUX, 'Set HGS_CLAUDE_TEST_BIN for the isolated native status line test')
class NativeClaudeStatusLine(test_pause.Harness):
    def setUp(self):
        super().setUp()
        for key in [key for key in self.env if key.startswith(('ANTHROPIC_', 'CLAUDE'))]:
            del self.env[key]
        # hgs reads the version of `claude`, the native CLI, as it does for a user.
        self.script('claude', '#!/bin/sh\nexec ' + shlex.quote(CLAUDE) + ' "$@"\n')
        api = self.root / 'api'
        api.mkdir()
        server = subprocess.Popen([sys.executable, '-I', str(FIXTURE), '--root', str(api), '--project', str(self.project)],
                                  stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        # Cleanups run after tearDown: stop the API only once the agent has exited.
        self.addCleanup(lambda: (server.terminate(), server.wait(10)))
        self.wait_until(lambda: (api / 'port').exists(), 'the fixture API')
        project = str(self.project)
        trusted = dict(hasTrustDialogAccepted=True, hasCompletedProjectOnboarding=True)
        (self.root / '.claude.json').write_text(json.dumps(dict(
            hasCompletedOnboarding=True, hasSeenAutoDefaultNudge=True, theme='dark', customApiKeyResponses=dict(approved=['fixture-key'], rejected=[]),
            projects={project: trusted, os.path.realpath(project): trusted})))
        catalog = self.root / '.claude/cache/model-catalog'
        catalog.mkdir(parents=True)
        (catalog / 'fixture-000000000000-cc.json').write_text(json.dumps(CATALOG))
        self.settings = self.root / '.claude/settings.json'
        self.user_line = {'type': 'command', 'command': USER_LINE, 'padding': 0}
        self.settings.write_text(json.dumps({'statusLine': self.user_line}))
        self.env.update(ANTHROPIC_API_KEY='fixture-key', ANTHROPIC_BASE_URL='http://127.0.0.1:' + (api / 'port').read_text(),
                        CLAUDE_CODE_DISABLE_NONESSENTIAL_TRAFFIC='1', DISABLE_AUTOUPDATER='1',
                        TERM='xterm-256color', LANG='C.UTF-8')

    def wait_until(self, predicate, what, timeout=45):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            value = predicate()
            if value:
                return value
            time.sleep(.1)
        self.fail('timed out waiting for ' + what + '\n' + self.screen() + '\n' + json.dumps(getattr(self, 'last', None)))

    def screen(self):
        pane = self.binding(self.name).get('pane') if hasattr(self, 'name') else None
        if not pane:
            return ''
        return subprocess.run([str(self.bin / 'tmux'), 'capture-pane', '-p', '-t', pane], env=self.env,
                              capture_output=True, text=True).stdout

    def inspect(self):
        return json.loads(self.hgs('inspect', self.name))

    def json_command(self, *args, payload):
        result = subprocess.run([str(test_pause.HGS), *args, '--json'], env=self.env, input=json.dumps(payload),
                                capture_output=True, text=True, timeout=60)
        self.assertEqual(result.returncode, 0, result.stderr + '\n' + self.screen())
        return json.loads(result.stdout)

    def request(self, **fields):
        binding = self.binding(self.name)
        return dict(request_id=str(uuid.uuid4()), expected_run_id=binding['run_id'],
                    expected_conversation_id=binding['conversation_id'], **fields)

    def reported(self):
        """The status line input Claude passed last, which hgs passed to the user's command."""
        path = self.root / 'status-input.json'
        try:
            return json.loads(path.read_text())
        except (FileNotFoundError, ValueError):
            return {}

    def capacity(self, conversation, model):
        """The inspected context once it reports the window that Claude's status line gave
        for `conversation` and `model`."""
        reported = self.reported()
        info = self.inspect()
        context = info['session_usage'].get('context', {})
        self.last = dict(reported=dict(session_id=reported.get('session_id'), model=reported.get('model'),
                                       context_window=reported.get('context_window')),
                         inspected=dict(model=info.get('model'), conversation_id=info.get('conversation_id'),
                                        session_usage=info['session_usage']))
        if reported.get('session_id') != conversation or reported.get('model', {}).get('id') != model:
            return None
        window = reported['context_window']['context_window_size']
        if (context.get('limit'), context.get('limit_model'), context.get('limit_source')) != \
                (window, model, 'native_statusline'):
            return None
        return context

    def test_tracked_claude_reports_its_window_and_keeps_the_users_status_line(self):
        self.name = 'claude/p/status'
        self.hgs('claude', 'p', '-n', 'status', '-d')
        self.wait_until(lambda: self.binding(self.name).get('conversation_id'), 'the native SessionStart')
        conversation = self.binding(self.name)['conversation_id']
        # Claude shows the user's own status line, from hgs's command.
        self.wait_until(lambda: 'Fixture status line' in self.screen(), "the user's status line")
        self.assertEqual(json.loads(self.settings.read_text())['statusLine'], self.user_line)
        self.assertNotIn('--settings', self.binding(self.name)['base'])
        context = self.wait_until(lambda: self.capacity(conversation, 'claude-opus-5-5'), 'the reported window')
        self.assertGreater(context['limit'], 0)
        # The input comes from the Claude Code under test: the pin in the reviewed matrix,
        # the latest release in the upstream canary.
        tested = subprocess.check_output([CLAUDE, '--version'], env=self.env, text=True).split()[0]
        self.assertEqual(self.reported()['version'], tested)
        # A reply adds the used context; the window stays the model's.
        self.json_command('send', self.name, payload=self.request(text='Fixture request.', attachments=[]))
        self.wait_until(lambda: 'Fixture reply.' in self.screen(), 'the reply')
        self.wait_until(lambda: (context := self.capacity(conversation, 'claude-opus-5-5')) and context.get('used'),
                        'the used context')
        # A session-only model change from Zerus: the window follows Claude's new report.
        before = self.settings.read_bytes()
        result = self.json_command('settings', self.name, payload=self.request(model='claude-haiku-5-5', effort='medium'))
        self.assertEqual(result['status'], 'applied')
        self.wait_until(lambda: self.capacity(conversation, 'claude-haiku-5-5'), "the new model's window")
        self.assertEqual(self.settings.read_bytes(), before)
        # Compaction replaces the used context, not the window.
        self.json_command('compact-context', self.name, payload=self.request())
        self.wait_until(lambda: self.binding(self.name).get('compact_context_request', {}).get('status') == 'completed',
                        'the compaction')
        self.wait_until(lambda: self.capacity(conversation, 'claude-haiku-5-5'), 'the window after compaction')
        # Clear starts another conversation in the same run, with its own report.
        result = self.json_command('clear-context', self.name, payload=self.request())
        self.assertEqual(result['status'], 'confirmed')
        cleared = self.binding(self.name)['conversation_id']
        self.assertNotEqual(cleared, conversation)
        # Claude 2.1.296 reports again at its next update; until then the earlier
        # conversation's sample is not applied to the new one.
        limit = self.inspect()['session_usage'].get('context', {}).get('limit')
        sample, = (self.root / 'state/context-telemetry').glob('*.json')
        if json.loads(sample.read_text())['conversation_id'] == conversation:
            self.assertIsNone(limit)
        self.json_command('send', self.name, payload=self.request(text='Fixture request after clear.', attachments=[]))
        self.wait_until(lambda: self.capacity(cleared, 'claude-haiku-5-5'), "the new conversation's window")
        # A resumed run registers the status line again and reports from its start.
        run = self.binding(self.name)['run_id']
        self.hgs('pause', self.name)
        self.hgs('resume', self.name, '-d')
        self.wait_until(lambda: self.binding(self.name).get('run_id') != run
                        and self.binding(self.name).get('activity') == 'idle', 'the resumed run')
        self.assertEqual(self.binding(self.name)['conversation_id'], cleared)
        self.wait_until(lambda: self.capacity(cleared, self.inspect()['model']), "the resumed run's window")
        self.assertEqual(json.loads(self.settings.read_text())['statusLine'], self.user_line)

    def test_linked_worktree_keeps_the_status_line_of_its_repository(self):
        """Claude 2.1.296 reads a linked worktree's local settings from its repository, by
        ownership checks hgs does not repeat; a status line there stays Claude's own."""
        git = ['git', '-c', 'user.name=HGS fixture', '-c', 'user.email=hgs@example.invalid', '-c', 'commit.gpgSign=false',
               '-c', 'core.hooksPath=' + os.devnull]
        env = {key: value for key, value in self.env.items() if not key.startswith('GIT_')}
        env.update(GIT_CONFIG_NOSYSTEM='1', GIT_CONFIG_GLOBAL=os.devnull, GIT_TERMINAL_PROMPT='0')
        linked = self.root / 'linked'
        for args in (['init', '-q', '-b', 'main'], ['commit', '-q', '--allow-empty', '-m', 'Fixture'],
                     ['worktree', 'add', '-q', '-b', 'linked', str(linked)]):
            subprocess.run(git + ['-C', str(self.project), *args], env=env, check=True, capture_output=True, timeout=20)
        (self.project / '.claude').mkdir()
        (self.project / '.claude/settings.local.json').write_text(json.dumps(
            {'statusLine': {'type': 'command', 'command': 'printf "Repository status line"'}}))
        config = json.loads((self.root / '.claude.json').read_text())
        config['projects'][str(linked)] = config['projects'][str(self.project)]
        (self.root / '.claude.json').write_text(json.dumps(config))
        self.name = 'claude/linked/status'
        self.hgs('claude', str(linked), '-n', 'status', '-d')
        self.wait_until(lambda: self.binding(self.name).get('conversation_id'), 'the native SessionStart')
        self.wait_until(lambda: 'Repository status line' in self.screen(), "the repository's status line")
        self.assertNotIn('Fixture status line', self.screen())
        self.assertIsNone(self.inspect()['session_usage'].get('context', {}).get('limit'))
        self.assertFalse((self.root / 'state/context-telemetry').exists())


if __name__ == '__main__':
    unittest.main()
