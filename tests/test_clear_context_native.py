"""Verify native clear/input contracts with private homes and localhost providers."""
import http.server, json, os, shlex, subprocess, tempfile, threading, time, unittest, uuid
from pathlib import Path
import test_input
import test_claude_questions_native as native

@unittest.skipUnless(native.CLAUDE, 'Set HGS_CLAUDE_TEST_BIN for the isolated native clear test')
class NativeClearContext(unittest.TestCase):
    setUp = native.NativeClaudeQuestions.setUp
    script = native.NativeClaudeQuestions.script
    write_record = native.NativeClaudeQuestions.write_record
    wait_for = native.NativeClaudeQuestions.wait_for
    screen = native.NativeClaudeQuestions.screen
    hook = native.NativeClaudeQuestions.hook
    inspect = native.NativeClaudeQuestions.inspect
    key = native.NativeClaudeQuestions.key

    def tmux(self, *args, **kwargs):
        if args[0] == 'new-session':
            env = dict(HGS_SESSION=self.name, HGS_RUN_ID=self.run_id,
                       HGS_STATE_DIR=str(self.state), HGS_CONFIG_DIR=str(self.root / 'config'), PATH=self.env['PATH'])
            command = 'env ' + ' '.join(shlex.quote(k+'='+v) for k,v in env.items()) + ' ' + shlex.quote(str(test_input.HGS)) + ' __state hook'
            path = self.root / '.claude/settings.json'
            config = json.loads(path.read_text())
            config['hooks'] = {event: [{'hooks': [{'type':'command', 'command':command}]}] for event in getattr(self,'hook_events',['SessionStart'])}
            path.write_text(json.dumps(config))
        return test_input.InputTransport.tmux(self, *args, **kwargs)

    def test_native_clear_confirms_new_conversation_without_restarting_process(self):
        self.key('Escape'); self.key('Escape'); time.sleep(.5)
        if 'No, keep bypass permissions' in self.screen(): self.key('Enter'); time.sleep(.3)
        self.record.update(phase='idle', activity='idle', active_tools={}); self.write_record()
        request = dict(request_id=str(uuid.uuid4()), expected_run_id=self.run_id,
                       expected_conversation_id=self.conversation_id)
        result = subprocess.run([str(test_input.HGS), 'clear-context', self.name, '--json'],
            input=json.dumps(request), env=self.env, text=True, capture_output=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stderr + '\n' + self.screen())
        self.assertEqual(json.loads(result.stdout)['status'], 'confirmed')
        current = json.loads(self.record_path.read_text())
        self.assertNotEqual(current['conversation_id'], self.conversation_id)
        self.assertEqual(current['pid'], self.record['pid'])
        self.assertEqual(current['run_id'], self.run_id)
        self.assertNotIn('clear_context_request', current)
        self.assertNotIn('Which release should we prepare?', self.screen())
        self.assertNotIn('Ask questions with the tool now.', self.screen())
        fresh = self.inspect()
        self.assertFalse(fresh.get('provider_messages'))
        self.assertFalse(fresh.get('pending_questions'))
        self.assertFalse(fresh.get('cache_hint'))
        self.assertEqual(fresh['phase'], 'idle')

@unittest.skipUnless(os.environ.get('HGS_CODEX_TEST_BIN'), 'Set HGS_CODEX_TEST_BIN for the isolated native Codex clear test')
class NativeCodexClear(unittest.TestCase):
    script = test_input.InputTransport.script
    write_record = test_input.InputTransport.write_record
    tmux = test_input.InputTransport.tmux
    wait_for = test_input.InputTransport.wait_for
    received = test_input.InputTransport.received
    tearDown = test_input.InputTransport.tearDown

    def fail(self, msg=None):
        # Only this credential-free fixture owns the captured terminal. Retain
        # its actual panel and cursor when a hosted contract fails, before the
        # private tmux server and home are cleaned up.
        try:
            screen = self.tmux('capture-pane', '-ep', '-t', self.pane, check=False)
            cursor = self.tmux('display-message', '-p', '-t', self.pane, '#{cursor_x}:#{cursor_y}', check=False).strip()
            record = json.loads(self.record_path.read_text())
            state = {key: record.get(key) for key in ('activity', 'phase', 'session_clear')}
            msg = f'{msg}\nNative fixture cursor: {cursor}; state: {state}\nNative fixture panel: {screen!r}'
        except (AttributeError, OSError, ValueError, subprocess.SubprocessError):
            pass
        # Explicit, so suites reusing this fixture can share its diagnostics.
        unittest.TestCase.fail(self, msg)

    def setUp(self):
        import test_clear_context
        test_input.InputTransport.setUp(self)
        self.tmux('kill-session', '-t', self.name)
        native_home = self.root / '.codex'
        native_home.mkdir()
        version = subprocess.check_output([os.environ['HGS_CODEX_TEST_BIN'], '--version'], text=True).strip()
        self.native_version = version.removeprefix('codex-cli ')
        width, height = os.environ.get('HGS_CODEX_TEST_SIZE', '110x35').split('x')
        self.assertTrue(width.isdigit() and height.isdigit(), 'Expected WIDTHxHEIGHT fixture geometry')
        self.geometry = (int(width), int(height))
        self.animations = os.environ.get('HGS_CODEX_TEST_ANIMATIONS', 'true')
        self.assertIn(self.animations, ('true', 'false'))
        # Curated plugins would be cloned from the network by a git child that
        # can outlive Codex and race this home's cleanup. Suites reusing this
        # fixture may serve a localhost provider and enable more features.
        (native_home / 'config.toml').write_text('''model="gpt-6.1-sol"
model_provider="fixture"
check_for_update_on_startup=false
[features]
plugins=false
''' + getattr(self, 'native_features', '') + '''[model_providers.fixture]
name="Fixture"
base_url="''' + getattr(self, 'provider_url', 'http://127.0.0.1:1/v1') + '''"
wire_api="responses"
''')
        path = native_home / 'sessions/2026/10/09' / ('rollout-fixture-' + self.conversation_id + '.jsonl')
        path.parent.mkdir(parents=True)
        usage_path = test_clear_context.ClearContext.usage_history(self, self.conversation_id, 109000, 100000)
        events = Path(usage_path).read_text().splitlines()
        metadata = json.loads(events[0])
        metadata.update(timestamp='2026-10-09T20:00:00Z')
        metadata['payload'].update(timestamp='2026-10-09T20:00:00Z', cwd=str(self.root), originator='codex_cli_rs',
            cli_version=self.native_version, source='cli', model_provider='fixture', base_instructions={'text':'Fixture.'})
        messages = [dict(type='response_item', timestamp='2026-10-09T20:00:01Z', payload=dict(
            type='message', role=role, content=[dict(type=kind, text=text)]))
            for role, kind, text in [('user', 'input_text', 'Previous fixture request'),
                                     ('assistant', 'output_text', 'Previous fixture answer')]]
        messages.extend([
            dict(type='event_msg', timestamp='2026-10-09T20:00:01Z', payload=dict(
                type='user_message', message='Previous fixture request', images=[], local_images=[])),
            dict(type='event_msg', timestamp='2026-10-09T20:00:02Z', payload=dict(
                type='agent_message', message='Previous fixture answer'))])
        path.write_text('\n'.join([json.dumps(metadata), *map(json.dumps, messages), events[1]]) + '\n')
        self.native_home = native_home
        # Suites reusing this setUp are not subclasses: call this class's launch.
        NativeCodexClear.launch(self, self.root, 'resume', self.conversation_id)
        self.wait_for(lambda: 'Previous fixture answer' in self.tmux('capture-pane', '-p', '-t', self.pane))
        self.record.update(transcript=str(path), last_message='Previous fixture answer')
        self.write_record()

    def launch(self, project, *arguments):
        width, height = map(str, self.geometry)
        self.tmux('new-session', '-d', '-s', self.name, '-x', width, '-y', height,
            'env', 'CODEX_HOME=' + str(self.native_home), os.environ['HGS_CODEX_TEST_BIN'], '--no-daemon',
            '-c', 'tui.animations=' + self.animations,
            '--dangerously-bypass-approvals-and-sandbox', '--dangerously-bypass-hook-trust',
            '-C', str(project), *arguments)
        self.pane = self.tmux('display-message', '-p', '-t', '=' + self.name + ':', '#{pane_id}').strip()
        self.tmux('set-option', '-t', '=' + self.name + ':', '@hgs_run', self.run_id)
        self.wait_for(lambda: '? for shortcuts' in self.tmux('capture-pane', '-p', '-t', self.pane))
        pid = int(self.tmux('display-message', '-p', '-t', self.pane, '#{pane_pid}').strip())
        start = subprocess.check_output(['ps', '-p', str(pid), '-o', 'lstart='],
            env=dict(self.env, LC_ALL='C', TZ='UTC'), text=True).strip()
        self.record.update(pid=pid, process_start=start, pane=self.pane, cwd=str(project), launch_dir=str(project),
            agent_home=str(self.native_home))

    def assert_reset_decoration(self):
        logo = [row for row in (test_input.REPO / 'src/state/data/codex-empty-state-60x21.txt').read_text().splitlines()
                if not row.startswith('#')]
        def visible():
            rows = [row.strip() for row in self.tmux('capture-pane', '-p', '-t', self.pane).splitlines()
                    if row.strip()]
            return any(rows[index:index + len(logo)] == logo for index in range(len(rows)))
        if self.animations == 'true' and self.geometry == (95, 47):
            self.wait_for(visible)
        elif self.animations == 'false':
            self.assertFalse(visible(), 'The reduced-motion fixture must not draw the native logo')

    def test_clear_is_visible_before_the_deferred_native_hook(self):
        import test_clear_context
        before = test_clear_context.ClearContext.inspect(self)
        self.assertEqual(before['session_usage']['context']['used'], 109000)
        self.assertFalse(before.get('session_clear'))
        receipt = test_clear_context.ClearContext.clear(self)
        self.assert_reset_decoration()
        self.assertEqual(receipt['status'], 'confirmed')
        fresh = test_clear_context.ClearContext.inspect(self)
        self.assertEqual(fresh['session_clear']['type'], 'SessionCleared')
        self.assertEqual(fresh['session_usage']['status'], 'unavailable')
        self.assertEqual(fresh['conversation_id'], self.conversation_id)
        current = json.loads(self.record_path.read_text())
        self.assertEqual(current['pid'], self.record['pid'])
        self.assertEqual(current['run_id'], self.run_id)
        self.assertTrue(current['session_clear']['awaiting_session_start'])
        self.assertEqual(sum(e['type']=='SessionCleared' for e in fresh['events']), 1)

    def test_clear_from_terminal_is_observed_once_without_resending(self):
        import test_clear_context
        self.tmux('send-keys', '-t', self.pane, '-l', '/clear')
        self.wait_for(lambda: '/clear' in self.tmux('capture-pane', '-p', '-t', self.pane))
        self.tmux('send-keys', '-t', self.pane, 'Enter')
        self.assert_reset_decoration()
        self.wait_for(lambda: bool(test_clear_context.ClearContext.inspect(self).get('session_clear')))
        first = test_clear_context.ClearContext.inspect(self)
        second = test_clear_context.ClearContext.inspect(self)
        self.assertEqual(first['session_clear'], second['session_clear'])
        self.assertEqual(second['conversation_id'], self.conversation_id)
        self.assertEqual(second['session_usage']['status'], 'unavailable')
        self.assertEqual(sum(e['type']=='SessionCleared' for e in second['events']), 1)
        current = json.loads(self.record_path.read_text())
        self.assertEqual(current['pid'], self.record['pid'])
        self.assertEqual(current['run_id'], self.run_id)
        self.assertNotIn('clear_context_request', current)

    def test_native_draft_is_preserved_and_prevents_clear(self):
        import test_clear_context
        draft = 'Keep this unsent native draft'
        self.tmux('send-keys', '-t', self.pane, '-l', draft)
        self.wait_for(lambda: draft in self.tmux('capture-pane', '-p', '-t', self.pane))
        error = test_clear_context.ClearContext.clear(self, success=False)
        self.assertIn('draft', error.lower())
        screen = self.tmux('capture-pane', '-p', '-t', self.pane)
        self.assertIn(draft, screen)
        self.assertNotIn('/clear', screen)
        fresh = test_clear_context.ClearContext.inspect(self)
        self.assertFalse(fresh.get('session_clear'))
        self.assertEqual(fresh['session_usage']['context']['used'], 109000)

    def test_header_above_an_answered_first_message_is_not_a_clear(self):
        # A new Codex session keeps its header, here with a folder outside the
        # fixture home printed in full, above the first answered message.
        # Only the empty state after an actual /clear is a reset.
        import test_clear_context, test_external_agents_native
        server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), test_external_agents_native.Response)
        self.addCleanup(server.server_close)
        self.addCleanup(server.shutdown)
        threading.Thread(target=server.serve_forever, daemon=True).start()
        outside = tempfile.TemporaryDirectory(prefix='hgs-project-')
        self.addCleanup(outside.cleanup)
        project = Path(outside.name).resolve()
        self.assertFalse(project.is_relative_to(self.root))
        # This session lives through a turn: keep it on the localhost provider
        # without the plugin marketplace sync, whose git checkout into the
        # private home could otherwise still be running at fixture cleanup.
        config = self.native_home / 'config.toml'
        config.write_text(config.read_text().replace('http://127.0.0.1:1/v1', f'http://127.0.0.1:{server.server_port}/v1')
            + 'request_max_retries=0\nstream_max_retries=0\n[projects.' + json.dumps(str(project)) + ']\ntrust_level="trusted"\n')
        previous = self.record['pid']
        self.tmux('kill-session', '-t', '=' + self.name)
        for _ in range(250):
            if subprocess.run(['ps', '-p', str(previous)], capture_output=True).returncode: break
            time.sleep(.02)
        self.launch(project)
        def wait(predicate, message):
            deadline, previous_screen = time.monotonic() + 30, None
            while True:
                screen = self.tmux('capture-pane', '-p', '-t', self.pane)
                if predicate(screen, previous_screen): return screen
                self.assertLess(time.monotonic(), deadline, message)
                previous_screen = screen
                time.sleep(.3)
        header = lambda screen: str(project) in [row.strip() for row in screen.splitlines()[:4]]
        wait(lambda screen, _: header(screen), 'Codex must print the folder outside its home in full')
        message, answer = 'First fixture message', 'Isolated review complete.'
        self.tmux('send-keys', '-t', self.pane, '-l', message)
        self.wait_for(lambda: message in self.tmux('capture-pane', '-p', '-t', self.pane))
        self.tmux('send-keys', '-t', self.pane, 'Enter')
        # The localhost provider answers at once; wait until Codex is idle.
        screen = wait(lambda screen, previous: answer in screen and screen == previous
                      and 'esc to interrupt' not in screen.lower(), 'The first message was not answered')
        self.assertTrue(header(screen), screen)
        self.record.update(prompt=message, last_message=answer)
        self.write_record()
        for _ in range(5):
            fresh = test_clear_context.ClearContext.inspect(self)
            self.assertFalse(fresh.get('session_clear'), screen)
            self.assertEqual(fresh['conversation_id'], self.conversation_id)
            time.sleep(.1)
        self.assertNotIn('session_clear', json.loads(self.record_path.read_text()))
        receipt = test_clear_context.ClearContext.clear(self)
        self.assert_reset_decoration()
        self.assertEqual(receipt['status'], 'confirmed')
        self.assertNotIn(message, self.tmux('capture-pane', '-p', '-t', self.pane))
        fresh = test_clear_context.ClearContext.inspect(self)
        self.assertEqual(sum(e['type']=='SessionCleared' for e in fresh['events']), 1)
        current = json.loads(self.record_path.read_text())
        self.assertTrue(current['session_clear']['awaiting_session_start'])
        self.assertEqual(current['pid'], self.record['pid'])
        self.assertEqual(current['run_id'], self.run_id)

    def test_foreign_directory_does_not_confirm_a_terminal_clear(self):
        import test_clear_context
        self.record['cwd'] = str(self.root / 'other-project')
        self.write_record()
        self.tmux('send-keys', '-t', self.pane, '-l', '/clear')
        self.wait_for(lambda: '/clear' in self.tmux('capture-pane', '-p', '-t', self.pane))
        self.tmux('send-keys', '-t', self.pane, 'Enter')
        time.sleep(.3)
        fresh = test_clear_context.ClearContext.inspect(self)
        self.assertFalse(fresh.get('session_clear'))
        self.assertEqual(fresh['session_usage']['context']['used'], 109000)

if __name__ == '__main__': unittest.main()
