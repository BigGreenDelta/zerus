"""Finite pure controls of the hint fixture; no native/setup/transport is run.

These are authored controls, not OS isolation or native compatibility evidence.
"""
import copy
import builtins
import hashlib
import json
from pathlib import Path, PurePosixPath
import sys
import tempfile
import types
from types import SimpleNamespace
import unittest
from unittest.mock import patch

import test_agent_hint_native as fixture


def stream(raw):
    import base64
    return dict(base64=base64.b64encode(raw).decode(), bytes=len(raw), forwarded=len(raw),
                sha256=hashlib.sha256(raw).hexdigest(), error=False, overflow=False)


def invocation(output=b''):
    event = dict(hook_event_name='SessionStart', session_id='conversation', source='resume')
    before = dict(argv=['__state', 'hook'], run='run', phase='candidate', source='a' * 40,
                  subject='/fixture/subject/hgs', expected_sha256='b' * 64)
    after = dict(child=dict(pid=100, birth='birth', image='/fixture/subject/hgs'), exit_code=0,
                 signal=None, stdin=stream(json.dumps(event).encode()), stdout=stream(output),
                 stderr=stream(b''), subject_sha256_after='b' * 64, recording_error=False)
    return before, after


def card():
    return dict(source='codex_hooks_trust', question_id='codex-hooks-trust:' + 'c' * 64,
                question_hash='c' * 64, can_answer=True, run_id='run',
                questions=[dict(id='hooks_trust', options=[dict(id='trust')])])


def readonly_hooks():
    return dict(source='startup_terminal_choice', question_id='startup-choice:' + 'd' * 64,
                question_hash='d' * 64, can_answer=False, run_id='run', agent_id='main',
                conversation_id=None, tool_call_id='', answer_transport='',
                answer_unavailable_reason='Answer this in Terminal.', questions=[dict(
                    id='terminal', header='Terminal', question='Hooks need review',
                    body='13 hooks are new or changed.\nHooks can run outside the sandbox after you trust them.',
                    multi_select=False, allow_other=False, options=[dict(id='1', label='Review hooks'),
                    dict(id='2', label='Trust all and continue'),
                    dict(id='3', label="Continue without trusting (hooks won't run)")])])


def state(*cards, ready=False):
    return dict(run_id='run', tracked=True, pending_questions=list(cards), first_message_can_send=ready,
                first_message_reason='' if ready else 'not ready')


def registered_unix_launch(root):
    owned, intent = {}, dict(root=str(root), socket=str(root / 'tmux.sock'), unix_before=None,
                            unix_registered=True, unix_run='run')
    for pid, role in ((10, 'server'), (20, 'supervisor'), (30, 'native')):
        image = fixture.os.path.realpath(root / ('image-' + role))
        owned[(pid, role)] = dict(role=role, image=image)
        intent['unix_' + role] = dict(pid=pid, birth=role, image=image)
    intent['unix_server']['socket'] = intent['socket']
    return owned, intent


class HintFixtureControls(unittest.TestCase):
    def test_macos_image_uses_kernel_path_and_preserves_birth_and_absolute_identity_guards(self):
        import posixpath
        class Identity(fixture.HintContract, unittest.TestCase): pass
        value = Identity()
        value.AGENT, value.bin, value.phase, value.env = 'claude', PurePosixPath('/fixture/owned/bin'), 'candidate', {}
        value.checkpoints, value.remaining = [], lambda maximum: maximum
        record = dict(pid=123, process_start='Wed Oct 21 07:21:07 2026', run_id='run')
        value.binding = lambda name: record
        helper = SimpleNamespace(executable=lambda pid: PurePosixPath('/fixture/owned/bin/claude'))
        frozen_os = SimpleNamespace(name='posix', path=posixpath, PathLike=fixture.os.PathLike, fspath=fixture.os.fspath)
        with patch.object(fixture, 'os', frozen_os), patch.object(fixture.sys, 'platform', 'darwin'), \
                patch.dict(sys.modules, {'test_kimi_native': helper}), \
                patch.object(fixture.subprocess, 'check_output', return_value='Wed Oct 21 07:21:07 2026 claude'):
            value.native_identity('claude/p/test')
            self.assertEqual(value.checkpoints[-1]['image'], '/fixture/owned/bin/claude')
            for image in (PurePosixPath('claude'), PurePosixPath('/fixture/foreign/claude'), None):
                helper.executable = lambda pid, result=image: result
                with self.subTest(image=image), self.assertRaises(AssertionError):
                    value.native_identity('claude/p/test')
            helper.executable = lambda pid: PurePosixPath('/fixture/owned/bin/claude')
            record['process_start'] = 'Wed Oct 21 07:21:08 2026'
            with self.assertRaises(AssertionError): value.native_identity('claude/p/test')
        self.assertEqual(len(value.checkpoints), 1)

    def test_claude_folder_trust_selects_exact_positive_label_and_rejects_refusal_or_changed_labels(self):
        item = dict(card(), source='claude_folder_trust', question_id='claude-trust:' + 'c' * 64,
                    questions=[dict(id='trust', options=[dict(id='trust_0', label='No, exit'),
                                                        dict(id='trust_1', label='Yes, I trust this folder')])])
        selected = fixture.startup_answer(state(item), dict(run_id='run'), set())
        self.assertEqual(selected[2], dict(question_id='trust', selected_option_ids=['trust_1'], text=''))
        for change in ('positive-label', 'refusal-label', 'refusal-id', 'swapped-options'):
            changed = copy.deepcopy(item)
            options = changed['questions'][0]['options']
            if change == 'positive-label': options[1]['label'] = 'No, exit'
            if change == 'refusal-label': options[0]['label'] = 'Yes, I trust this folder'
            if change == 'refusal-id': options[1]['id'] = 'trust_0'
            if change == 'swapped-options': options.reverse()
            with self.subTest(change=change), self.assertRaises(AssertionError):
                fixture.startup_answer(state(changed), dict(run_id='run'), set())
        self.assertEqual(fixture.startup_answer(state(card()), dict(run_id='run'), set())[2]
                         ['selected_option_ids'], ['trust'])
        codex = dict(item, source='codex_folder_trust', question_id='codex-trust:' + 'c' * 64,
                     questions=[dict(id='trust', options=[dict(id='trust_0', label='Trust and continue'),
                                                         dict(id='trust_1', label='Quit')])])
        self.assertEqual(fixture.startup_answer(state(codex), dict(run_id='run'), set())[2]
                         ['selected_option_ids'], ['trust_0'])

    def test_private_codex_config_sets_fixture_tool_mode_without_trusting_hooks_or_projects(self):
        import tomllib
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            home, agent_home = root / 'owned', root / 'codex-config'
            home.mkdir(); agent_home.mkdir()
            value = SimpleNamespace(AGENT='codex', env=dict(HOME=str(home)), agent_home=agent_home,
                                    api_url='http://127.0.0.1:12345')
            fixture.HintContract.configure(value)
            text = (agent_home / 'config.toml').read_text()
            config = tomllib.loads(text)
            self.assertEqual(config['sandbox_mode'], 'danger-full-access')
            self.assertEqual(config['approval_policy'], 'never')
            self.assertLess(text.index('sandbox_mode='), text.index('[features]'))
            self.assertEqual(value.env['HOME'], str(home))
            self.assertEqual(value.env['CODEX_HOME'], str(agent_home))
            self.assertNotIn('projects', config)
            self.assertNotIn('hooks', config)
            self.assertFalse((home / '.codex').exists())
            self.assertNotIn('bypass-hook-trust', text)

    def test_windows_loader_discovers_exact_four_actual_class_methods_without_unix_imports(self):
        # Compile the real fixture module against frozen platform/fixture
        # modules. No Windows API, inherited setup, transport or native runs.
        windows_os = types.ModuleType('os')
        windows_os.__dict__.update(vars(fixture.os))
        windows_os.name, windows_os.environ = 'nt', {}
        windows = types.ModuleType('test_windows_native_agents')
        windows.NativeAgent, windows.CONPTY = type('NativeAgent', (), {}), None
        host = types.ModuleType('test_windows_host')
        host.HostCase = type('HostCase', (unittest.TestCase,), {})
        frozen = {'os': windows_os, 'model_api_fixture': types.ModuleType('model_api_fixture'),
                  'test_windows_native_agents': windows, 'test_windows_host': host}
        imported = []
        def imports(name, globals=None, locals=None, fromlist=(), level=0):
            imported.append(name)
            if name == 'test_pause': raise AssertionError('Windows must not import Unix pty fixtures')
            return frozen.get(name) or builtins.__import__(name, globals, locals, fromlist, level)
        module = types.ModuleType('test_agent_hint_native')
        module.__dict__.update(__file__=fixture.__file__,
                               __builtins__=dict(vars(builtins), __import__=imports))
        exec(compile(Path(fixture.__file__).read_text(), fixture.__file__, 'exec'), module.__dict__)
        loader = unittest.TestLoader()
        with patch.dict(sys.modules, {'test_agent_hint_native': module}):
            for provider in ('Claude', 'Codex'):
                names = ['test_agent_hint_native.NativeWindows' + provider + 'Hint.' + method
                         for method in fixture.CASE_METHODS]
                suite = loader.loadTestsFromNames(names)
                actual = [test.id() for group in suite for test in group]
                self.assertEqual(actual, names)
                self.assertEqual(loader.errors, [])
        self.assertNotIn('test_pause', imported)

    def registration_double(self, root, provider):
        class Registration(fixture.HintContract, unittest.TestCase): pass
        value = Registration()
        value.root, value.AGENT, value.phase = root, provider, 'candidate'
        value.observer_exe, value.real_hook = root / 'observer-hgs', root / 'real-hgs'
        value.real_hook.write_bytes(b'synthetic real hgs')
        value.inputs = {'candidate': dict(sha256=hashlib.sha256(value.real_hook.read_bytes()).hexdigest())}
        value.settings_path = root / 'settings.json'
        return value

    def test_codex_never_fabricates_a_claude_answer_channel_on_either_platform(self):
        with tempfile.TemporaryDirectory() as folder:
            value = self.registration_double(Path(folder), 'codex')
            for platform in ('nt', 'posix'):
                with self.subTest(platform=platform), patch.object(fixture, 'os', SimpleNamespace(name=platform)):
                    value.install_observer_entry()
                    self.assertIsNone(value.real_channel)
                    self.assertNotIn('PermissionRequest', json.loads(value.settings_path.read_text())['hooks'])
                    value.assert_registration()

    def test_claude_requires_its_exact_real_answer_channel_on_either_platform(self):
        with tempfile.TemporaryDirectory() as folder:
            value = self.registration_double(Path(folder), 'claude')
            for platform in ('nt', 'posix'):
                with self.subTest(platform=platform), patch.object(fixture, 'os', SimpleNamespace(name=platform)):
                    value.install_observer_entry()
                    self.assertIsNotNone(value.real_channel)
                    value.assert_registration()
                    settings = json.loads(value.settings_path.read_text())
                    settings['hooks']['PermissionRequest'] = []
                    value.settings_path.write_text(json.dumps(settings))
                    with self.assertRaises(AssertionError): value.assert_registration()

    def test_explicit_claude_config_dir_owns_unchanged_onboarding_payload_without_home_redirect(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            home, agent_home = root / 'owned', root / 'agent-config'
            home.mkdir(); agent_home.mkdir()
            value = SimpleNamespace(AGENT='claude', env=dict(HOME=str(home)), agent_home=agent_home)
            fixture.HintContract.configure(value)
            self.assertEqual(value.env['HOME'], str(home))
            self.assertEqual(value.env['CLAUDE_CONFIG_DIR'], str(agent_home))
            self.assertFalse((home / '.claude.json').exists())
            self.assertEqual(json.loads((agent_home / '.claude.json').read_text()), dict(
                hasCompletedOnboarding=True, hasSeenAutoDefaultNudge=True, theme='dark',
                customApiKeyResponses=dict(approved=['fixture-key'], rejected=[])))
            self.assertEqual(json.loads((agent_home / 'settings.json').read_text()), {})

    def test_exact_static_output_is_separate_from_empty_output(self):
        raw = json.dumps(dict(hookSpecificOutput=dict(hookEventName='SessionStart',
                                                    additionalContext=fixture.HINT))).encode() + b'\n'
        before, after = invocation(raw)
        self.assertTrue(fixture.hint_output(fixture.main_call(before, after)['output']))
        self.assertFalse(fixture.hint_output(b''))
        with self.assertRaises(AssertionError):
            fixture.hint_output(json.dumps(dict(agent_hint='marked')).encode())

    def test_output_without_complete_forwarding_is_unknown(self):
        before, after = invocation(b'actual stdout')
        for field, value in (('forwarded', 0), ('error', True), ('overflow', True), ('sha256', '0' * 64)):
            bad = copy.deepcopy(after)
            bad['stdout'][field] = value
            with self.subTest(field=field), self.assertRaises(AssertionError):
                fixture.main_call(before, bad)

    def test_stdin_and_stderr_have_independent_complete_byte_order(self):
        for raw in (b'\0one\r\ntwo\xff', b''):
            self.assertEqual(fixture.stream_bytes(stream(raw)), raw)
        before, after = invocation()
        after['stderr']['forwarded'] = 1
        with self.assertRaises(AssertionError): fixture.main_call(before, after)

    def test_missing_image_birth_changed_source_and_failed_exit_are_unknown(self):
        before, after = invocation()
        for change in ('identity', 'image', 'birth', 'seal', 'exit', 'signal', 'recorder'):
            bad = copy.deepcopy(after)
            if change == 'identity': bad['child'] = None
            if change == 'image': bad['child']['image'] = '/fixture/foreign/hgs'
            if change == 'birth': bad['child']['birth'] = ''
            if change == 'seal': bad['subject_sha256_after'] = 'd' * 64
            if change == 'exit': bad['exit_code'] = 2
            if change == 'signal': bad['signal'] = 15
            if change == 'recorder': bad['recording_error'] = True
            with self.subTest(change=change), self.assertRaises(AssertionError): fixture.main_call(before, bad)

    def test_child_and_non_start_events_never_establish_main_emission(self):
        before, after = invocation()
        for event in (dict(hook_event_name='SessionStart', session_id='child', agent_id='child'),
                      dict(hook_event_name='Stop', session_id='conversation')):
            changed = copy.deepcopy(after)
            changed['stdin'] = stream(json.dumps(event).encode())
            self.assertIsNone(fixture.main_call(before, changed))

    def test_known_startup_card_selection_does_not_open_public_gate(self):
        selected = fixture.startup_answer(state(card()), dict(run_id='run'), set())
        self.assertEqual(selected[2], dict(question_id='hooks_trust', selected_option_ids=['trust'], text=''))
        self.assertFalse(state(card())['first_message_can_send'])

    def test_unknown_changed_or_repeated_card_never_selects_an_answer(self):
        for change in ('source', 'hash', 'option', 'answerable', 'multiple', 'error', 'run', 'attempted'):
            view, record, attempted = state(card()), dict(run_id='run'), set()
            if change == 'source': view['pending_questions'][0]['source'] = 'update'
            if change == 'hash': view['pending_questions'][0]['question_hash'] = 'd' * 64
            if change == 'option': view['pending_questions'][0]['questions'][0]['options'] = [dict(id='review')]
            if change == 'answerable': view['pending_questions'][0]['can_answer'] = False
            if change == 'multiple': view['pending_questions'].append(card())
            if change == 'error': view['pending_questions_error'] = 'unknown'
            if change == 'run': record['run_id'] = 'foreign'
            if change == 'attempted': attempted.add(('run', card()['question_id'], card()['question_hash']))
            with self.subTest(change=change), self.assertRaises(AssertionError):
                fixture.startup_answer(view, record, attempted)

    def gate_double(self, views):
        class Gate(fixture.HintContract, unittest.TestCase):
            AGENT = 'codex'
            def inspected(self, name):
                value = self.views[min(self.index, len(self.views) - 1)]
                self.index += 1
                return value
            def binding(self, name): return dict(run_id='run', agent='codex')
            def json_hgs(self, *args, **kwargs):
                return dict(sessions=[dict(name='codex/p/test', process_state='running')])
            def native_identity(self, name): self.proven.append(name)
            def execute(self, argv, **options):
                return fixture.subprocess.run(argv, text=True, capture_output=True, **options)
        value = Gate()
        value.views, value.index, value.proven = views, 0, []
        value.end, value.row_end, value.cleaning, value.gate_end = 940., 1000., False, None
        value.startup_ends, value.completed_runs, value.attempted = {}, set(), set()
        value.real, value.phase, value.env, value.checkpoints = {'candidate': Path('/fixture/hgs')}, 'candidate', {}, []
        value.register_unix_run = lambda record: None  # These controls isolate the public gate.
        return value

    def fake_answer(self, calls, name='codex/p/test'):
        def answer(argv, **options):
            calls.append((argv, options))
            payload = json.loads(options['input'])
            receipt = dict(status='answered', name=name, request_id=payload['request_id'],
                           run_id=payload['expected_run_id'], conversation_id=payload['expected_conversation_id'],
                           question_id=payload['question_id'], question_hash=payload['expected_question_hash'],
                           answers=payload['answers'], digest=hashlib.sha256(options['input'].encode()).hexdigest(),
                           submitted_at=1, answered_at=2)
            return SimpleNamespace(returncode=0, stdout=json.dumps(receipt), stderr='')
        return answer

    def test_late_card_is_answered_once_within_same_original_gate(self):
        value, calls, now = self.gate_double([state(), state(card()), state(ready=True)]), [], [0.]
        with patch.object(fixture.time, 'monotonic', side_effect=lambda: now[0]), \
                patch.object(fixture.time, 'sleep', side_effect=lambda n: now.__setitem__(0, now[0] + n)), \
                patch.object(fixture.subprocess, 'run', side_effect=self.fake_answer(calls)):
            value.gate('codex/p/test', 'run')
        self.assertEqual(len(calls), 1)
        self.assertEqual(value.startup_ends['run'], 120)
        self.assertEqual(value.proven, ['codex/p/test'])

    def test_readonly_hooks_waits_for_supported_card_and_only_that_card_is_answered_once(self):
        views = [state(readonly_hooks(), ready=True), state(card()), state(ready=True)]
        value, calls, now = self.gate_double(views), [], [0.]
        with patch.object(fixture.time, 'monotonic', side_effect=lambda: now[0]), \
                patch.object(fixture.time, 'sleep', side_effect=lambda n: now.__setitem__(0, now[0] + n)), \
                patch.object(fixture.subprocess, 'run', side_effect=self.fake_answer(calls)):
            value.gate('codex/p/test', 'run')
        self.assertEqual(len(calls), 1)
        self.assertEqual(json.loads(calls[0][1]['input'])['question_id'], card()['question_id'])
        self.assertEqual(value.checkpoints[0]['consent'], 'codex_hooks_trust')
        self.assertEqual(value.startup_ends['run'], 120.)
        self.assertEqual(value.proven, ['codex/p/test'])

    def test_readonly_hooks_forever_expires_without_answer_prompt_or_new_deadline(self):
        value, calls, now = self.gate_double([state(readonly_hooks(), ready=True)]), [], [119.9]
        value.startup_ends['run'] = 120.
        with patch.object(fixture.time, 'monotonic', side_effect=lambda: now[0]), \
                patch.object(fixture.time, 'sleep', side_effect=lambda n: now.__setitem__(0, now[0] + n)), \
                patch.object(fixture.subprocess, 'run', side_effect=self.fake_answer(calls)), \
                self.assertRaisesRegex(AssertionError, 'gate remained closed'):
            value.gate('codex/p/test', 'run')
        self.assertEqual((calls, value.proven, value.attempted), ([], [], set()))
        self.assertEqual(value.startup_ends['run'], 120.)

    def test_readonly_startup_words_and_options_never_select_input_for_either_agent(self):
        for agent in ('claude', 'codex'):
            for change in ('count', 'title', 'option', 'unnumbered'):
                item = readonly_hooks()
                if change == 'count': item['questions'][0]['body'] = item['questions'][0]['body'].replace('13', '14')
                if change == 'title': item['questions'][0]['question'] = 'Accessing workspace:'
                if change == 'option': item['questions'][0]['options'][1]['label'] = 'Unknown native option'
                if change == 'unnumbered': item['questions'][0]['options'] = []
                with self.subTest(agent=agent, change=change):
                    attempted = set()
                    self.assertIsNone(fixture.startup_answer(state(item), dict(run_id='run', agent=agent), attempted))
                    self.assertEqual(attempted, set())

    def test_changed_readonly_identity_and_schema_are_refused(self):
        for change in ('hash', 'run', 'answerable', 'transport', 'typed-choice', 'agent', 'conversation',
                       'tool', 'source', 'body-bound', 'option-bound', 'main', 'question-id', 'unknown-field'):
            item = readonly_hooks()
            binding = dict(run_id='run', agent='codex')
            if change == 'hash': item['question_hash'] = 'c' * 64
            if change == 'run': item['run_id'] = 'other'
            if change == 'answerable': item['can_answer'] = True
            if change == 'transport': item['answer_transport'] = 'codex_tui'
            if change == 'typed-choice': item['questions'][0]['multi_select'] = 0
            if change == 'agent': binding['agent'] = 'unknown'
            if change == 'conversation': item['conversation_id'] = 'other'
            if change == 'tool': item['tool_call_id'] = 'answer'
            if change == 'source': item['source'] = 'unknown'
            if change == 'body-bound': item['questions'][0]['body'] = 'x' * 32769
            if change == 'option-bound': item['questions'][0]['options'] *= 6
            if change == 'main': item['agent_id'] = 'child'
            if change == 'question-id': item['question_id'] = 'startup-choice:foreign'
            if change == 'unknown-field': item['questions'][0]['unknown'] = {'unbounded': []}
            with self.subTest(change=change), self.assertRaises(AssertionError):
                fixture.startup_answer(state(item), binding, set())

    def test_claude_generic_workspace_waits_for_exact_typed_consent_without_authorizing_a_prompt(self):
        generic = readonly_hooks()
        generic['questions'][0].update(question='Accessing workspace:', body='Full native disclosure', options=[])
        typed = dict(card(), source='claude_folder_trust', question_id='claude-trust:' + 'c' * 64,
                     questions=[dict(id='trust', options=[dict(id='trust_0', label='No, exit'),
                                                         dict(id='trust_1', label='Yes, I trust this folder')])])
        value, calls, now = self.gate_double([state(generic, ready=True), state(typed), state(ready=True)]), [], [0.]
        value.AGENT = 'claude'
        value.binding = lambda name: dict(run_id='run', agent='claude')
        value.json_hgs = lambda *args: dict(sessions=[dict(name='claude/p/test', process_state='running')])
        with patch.object(fixture.time, 'monotonic', side_effect=lambda: now[0]), \
                patch.object(fixture.time, 'sleep', side_effect=lambda n: now.__setitem__(0, now[0] + n)), \
                patch.object(fixture.subprocess, 'run', side_effect=self.fake_answer(calls, 'claude/p/test')):
            value.gate('claude/p/test', 'run')
        self.assertEqual(len(calls), 1)
        self.assertEqual(json.loads(calls[0][1]['input'])['answers'][0]['selected_option_ids'], ['trust_1'])
        self.assertEqual(value.startup_ends['run'], 120.)

    def test_existing_original_deadline_is_never_renewed(self):
        value, calls, now = self.gate_double([state()]), [], [119.9]
        value.startup_ends['run'] = 120
        with patch.object(fixture.time, 'monotonic', side_effect=lambda: now[0]), \
                patch.object(fixture.time, 'sleep', side_effect=lambda n: now.__setitem__(0, now[0] + n)), \
                patch.object(fixture.subprocess, 'run', side_effect=self.fake_answer(calls)):
            with self.assertRaises(AssertionError): value.gate('codex/p/test', 'run')
            with self.assertRaises(AssertionError): value.gate('codex/p/test', 'run')
        self.assertEqual(value.startup_ends['run'], 120)
        self.assertEqual(calls, [])

    def test_gate_expiry_between_reads_prevents_the_next_command_without_renewal(self):
        for stage in ('inspection', 'binding'):
            value, calls, now = self.gate_double([state()]), [], [0.]
            def inspection(name):
                calls.append('inspect')
                if stage == 'inspection': now[0] = 120.
                return state()
            def binding(name):
                calls.append('binding')
                now[0] = 120.
                return dict(run_id='run', agent='codex')
            value.inspected, value.binding = inspection, binding
            value.json_hgs = lambda *args: self.fail('expired gate must not start ls')
            with self.subTest(stage=stage), patch.object(fixture.time, 'monotonic', side_effect=lambda: now[0]), \
                    self.assertRaisesRegex(AssertionError, 'gate remained closed'):
                value.gate('codex/p/test', 'run')
            self.assertEqual(calls, ['inspect'] if stage == 'inspection' else ['inspect', 'binding'])
            self.assertEqual(value.startup_ends['run'], 120.)
            self.assertEqual(value.proven, [])

    def test_startup_diagnostics_keep_unknown_private_pane_and_run_record_with_original_end(self):
        with tempfile.TemporaryDirectory() as folder:
            value = self.gate_double([])
            value.result_dir, value.row_end, value.socket = Path(folder), 10., '/fixture/private/socket'
            value.last_gate = dict(name='codex/p/test', original_end=8.)
            value.maybe_binding = lambda name: dict(run_id='run', exit_code=1)
            calls = []
            def capture(argv, **options):
                calls.append((argv, options['timeout']))
                return SimpleNamespace(returncode=1, stdout='', stderr='private socket missing')
            value.execute = capture
            with patch.object(fixture, 'os', SimpleNamespace(name='posix')), \
                    patch.object(fixture, 'test_pause', SimpleNamespace(TMUX='/fixture/tmux'), create=True), \
                    patch.object(fixture.time, 'monotonic', return_value=9.):
                value.keep_startup_evidence()
            evidence = json.loads((Path(folder) / 'startup-observations.json').read_text())
            self.assertEqual(evidence['diagnostic_end'], 10.)
            self.assertEqual(evidence['run_record'], dict(run_id='run', exit_code=1))
            self.assertEqual(evidence['owned_pane']['returncode'], 1)
            self.assertEqual(evidence['owned_pane']['status'], 'unknown')
            self.assertEqual(calls[0][1], 1.)
            self.assertEqual(calls[0][0][1:3], ['-S', '/fixture/private/socket'])
            self.assertEqual(value.last_gate['original_end'], 8.)

    def test_diagnostic_truncation_is_explicit_and_does_not_change_gate_values(self):
        value = dict(error='x' * 40000)
        observed = fixture.bounded_observation(value)
        self.assertIs(observed['truncated'], True)
        self.assertGreater(observed['bytes'], 32768)
        self.assertEqual(len(value['error']), 40000)

    def test_uncertain_answer_cannot_rearm_same_card(self):
        value, calls = self.gate_double([state(card())]), []
        def uncertain(*args, **options):
            calls.append(options)
            return SimpleNamespace(returncode=1, stdout='', stderr='uncertain')
        with patch.object(fixture.time, 'monotonic', return_value=0), \
                patch.object(fixture.subprocess, 'run', side_effect=uncertain):
            with self.assertRaises(AssertionError): value.gate('codex/p/test', 'run')
            with self.assertRaises(AssertionError): value.gate('codex/p/test', 'run')
        self.assertEqual(len(calls), 1)

    def test_receipt_binds_entire_exact_prepared_request(self):
        payload = dict(request_id='request', expected_run_id='run', expected_conversation_id='',
                       question_id='question', expected_question_hash='hash', answers=[])
        serialized = json.dumps(payload)
        receipt = dict(status='answered', name='name', request_id='request', run_id='run', conversation_id='',
                       question_id='question', question_hash='hash', answers=[],
                       digest=hashlib.sha256(serialized.encode()).hexdigest(), submitted_at=1, answered_at=2)
        fixture.checked_receipt(receipt, payload, serialized, 'name')
        for field in ('name', 'request_id', 'run_id', 'conversation_id', 'question_id', 'question_hash',
                      'answers', 'digest', 'submitted_at', 'answered_at'):
            changed = dict(receipt, **{field: None})
            with self.subTest(field=field), self.assertRaises(AssertionError):
                fixture.checked_receipt(changed, payload, serialized, 'name')

    def test_private_children_do_not_inherit_owner_credentials_or_session_context(self):
        own = fixture.private_environment(dict(PATH='/fixture/bin', HOME='/fixture/home',
                    AWS_SECRET_ACCESS_KEY='untrusted', HGS_SESSION='foreign', CODEX_HOME='/foreign',
                    CLAUDE_CONFIG_DIR='/foreign', OPENAI_ORG_ID='foreign', GITHUB_TOKEN='untrusted'))
        self.assertEqual(own, dict(PATH='/fixture/bin', HOME='/fixture/home'))

    def test_unknown_null_and_truthy_public_forms_never_admit_a_message(self):
        record = dict(run_id='run')
        for key, values in (('tracked', (None, False, 0, 1, 'true')),
                            ('first_message_can_send', (None, False, 0, 1, 'true')),
                            ('first_message_reason', (None, False, 0, 'unknown')),
                            ('pending_questions', (None, False, 0, {}, ''))):
            for value in values:
                view = state(ready=True)
                view[key] = value
                with self.subTest(key=key, value=value): self.assertFalse(fixture.public_ready(view, record))
        for key in ('tracked', 'pending_questions', 'first_message_can_send', 'first_message_reason'):
            view = state(ready=True)
            del view[key]
            self.assertFalse(fixture.public_ready(view, record))
        for value in (None, False, 0, 1, 'true'):
            view = dict(state(ready=True), resume_message_can_send=value, resume_message_reason='')
            self.assertFalse(fixture.public_ready(view, record))
        for value in (None, False, 0, 'unknown'):
            view = dict(state(ready=True), resume_message_can_send=True, resume_message_reason=value)
            self.assertFalse(fixture.public_ready(view, record))

    def test_bound_steady_fallback_requires_exact_conversation_and_idle(self):
        record = dict(run_id='run', conversation_id='conversation')
        view = dict(state(), conversation_id='conversation', activity='idle')
        del view['first_message_reason']  # Legitimate hgs bound-summary shape.
        self.assertTrue(fixture.public_ready(view, record))
        for key, value in (('conversation_id', None), ('conversation_id', 'foreign'), ('activity', None)):
            self.assertFalse(fixture.public_ready(dict(view, **{key: value}), record))

    def test_unknown_cards_and_truthy_answerability_never_select_an_answer(self):
        for value in (None, False, {}, ''):
            view = dict(state(), pending_questions=value)
            with self.assertRaises(AssertionError): fixture.startup_answer(view, dict(run_id='run'), set())
        for key, values in (('tracked', (None, False, 0, 1, 'true')),):
            for value in values:
                view = dict(state(card()), **{key: value})
                with self.assertRaises(AssertionError): fixture.startup_answer(view, dict(run_id='run'), set())
        for key, values in (('can_answer', (None, False, 0, 1, 'true')), ('run_id', (None, 'foreign'))):
            for value in values:
                item = dict(card(), **{key: value})
                with self.assertRaises(AssertionError): fixture.startup_answer(state(item), dict(run_id='run'), set())

    def cleanup_double(self, root, evidence):
        class Cleanup(fixture.HintContract, unittest.TestCase):
            def stop_unix(self): self.steps.append('socket-discovery-reap')
            def stop_private_socket(self): self.steps.append('independent-socket-stop')
            def stop_host(self): self.steps.append('owned-host-handles')
            def close_model(self): self.steps.append('model')
            def final_observer_check(self): self.steps.append('observer')
        value = Cleanup()
        value.root, value.result_dir, value.row_end = root, evidence, 100.
        value.cli_children, value.cleanup_intents, value.steps = [], [], []
        value.cleanup_verified = False
        return value

    def unix_custody_double(self, root):
        class Custody(fixture.HintContract, unittest.TestCase): pass
        value = Custody()
        value.root, value.socket, value.env = root, str(root / 'tmux.sock'), {}
        value.phase, value.real = 'candidate', {'candidate': root / 'subjects/candidate/hgs'}
        value.native_copy = root / 'bin/claude'
        value.unix_owned, value.unix_runs, value.cleanup_intents = {}, set(), []
        value.cli_children, value.cli_observations, value.checkpoints = [], [], []
        value.row_end, value.remaining, value.save_cleanup = 100., lambda maximum: maximum, lambda stage: None
        rows, events = {}, []
        images = {10: '/fixture/tools/tmux', 20: value.real['candidate'], 30: value.native_copy}
        value.unix_table = lambda: dict(rows)
        value.unix_endpoint_absent = lambda: 10 not in rows
        value.unix_image = lambda pid, birth: fixture.os.path.realpath(images[pid])
        def query(argv):
            events.append(argv)
            if 'list-panes' in argv:
                rendered = (argv[-1].replace('#{pid}', '10').replace('#{socket_path}', value.socket)
                            .replace('#{pane_pid}', '20').replace('\t', '_'))
                return SimpleNamespace(returncode=0 if 10 in rows else 1, stderr='',
                    stdout=rendered + '\n' if 10 in rows else '')
            if 'kill-server' in argv:
                rows.clear()
                return SimpleNamespace(returncode=0, stdout='', stderr='')
            raise AssertionError('unexpected query')
        value.unix_query = query
        return value, rows, events

    def test_unix_server_row_uses_printable_framing_and_refuses_ambiguous_owned_paths(self):
        with tempfile.TemporaryDirectory() as folder:
            value, rows, events = self.unix_custody_double(Path(folder))
            rows.update({10: (1, 'S', 'server birth'), 20: (10, 'S', 'supervisor birth')})
            with patch.object(fixture, 'test_pause', SimpleNamespace(TMUX='/fixture/tools/tmux'), create=True):
                _, server, panes = value.unix_live_snapshot()
                self.assertEqual(server['pid'], 10)
                self.assertEqual(panes, {20})
                self.assertEqual(events[-1][-1], '#{pid}|#{socket_path}|#{pane_pid}')
                # The retained local tmux observation replaces control bytes by
                # underscores. The previous TAB format cannot yield three fields.
                old = f'10\t{value.socket}\t20'.replace('\t', '_')
                with patch.object(value, 'unix_query', return_value=SimpleNamespace(
                        returncode=0, stdout=old + '\n', stderr='')):
                    with self.assertRaisesRegex(AssertionError, 'private pane/server identity'):
                        value.unix_live_snapshot()
                for suffix in ('ambiguous|path', 'control\npath'):
                    value.root = Path(folder) / suffix
                    value.socket = str(value.root / 'tmux.sock')
                    before = len(events)
                    with self.assertRaisesRegex(AssertionError, 'cannot be framed'):
                        value.unix_live_snapshot()
                    self.assertEqual(len(events), before)

    def test_every_unix_launch_registers_server_before_spawn_and_new_run_before_absence_can_pass(self):
        with tempfile.TemporaryDirectory() as folder:
            value, rows, events = self.unix_custody_double(Path(folder))
            class Child:
                returncode = 0
                def __init__(self, *args, **kwargs):
                    self.test_before = list(events)
                    rows.update({10: (1, 'S', 'server birth'), 20: (10, 'S', 'supervisor birth'),
                                 30: (20, 'S', 'native birth'), 40: (30, 'S', 'descendant birth')})
                def poll(self): return 0
                def communicate(self, *args, **kwargs): return '', ''
            frozen_os = SimpleNamespace(name='posix', path=fixture.os.path)
            with patch.object(fixture, 'os', frozen_os), \
                    patch.object(fixture, 'test_pause', SimpleNamespace(TMUX='/fixture/tools/tmux'), create=True), \
                    patch.object(fixture, 'private_endpoint_absent', return_value=True), \
                    patch.object(fixture.subprocess, 'Popen', side_effect=Child):
                value.execute([str(value.real['candidate']), 'claude'], env={}, timeout=5)
                self.assertTrue(value.cli_children[0].test_before)
                self.assertFalse(value.cleanup_intents[0]['unix_registered'])
                with self.assertRaises(AssertionError):
                    fixture.unix_absence_proof(value.unix_owned, {}, value.cleanup_intents, True)
                value.register_unix_run(dict(run_id='run', pid=30, process_start='native birth',
                    supervisor=dict(pid=20, start='supervisor birth')))
                self.assertTrue(value.cleanup_intents[0]['unix_registered'])
                self.assertEqual(value.cleanup_intents[0]['unix_server']['socket'], value.socket)
                self.assertIn((40, 'descendant birth'), value.unix_owned)
                value.stop_unix()
            self.assertEqual(len([argv for argv in events if 'kill-server' in argv]), 1)
            self.assertIn('exited', value.checkpoints[-1]['cleanup'])

    def test_stale_unix_endpoint_passes_only_complete_exited_custody_and_never_sends_a_stop(self):
        with tempfile.TemporaryDirectory() as folder:
            value, rows, events = self.unix_custody_double(Path(folder))
            value.unix_owned, intent = registered_unix_launch(Path(folder))
            value.cleanup_intents = [intent]
            with patch.object(fixture, 'test_pause', SimpleNamespace(TMUX='/fixture/tools/tmux'), create=True), \
                    patch.object(fixture, 'private_endpoint_absent', return_value=True):
                value.stop_unix()
                value.stop_private_socket()
            self.assertFalse(any('kill-server' in argv for argv in events))
            self.assertTrue(value.checkpoints)

    def test_unix_absence_refuses_incomplete_custody_live_identity_and_descendant_or_unknown_endpoint(self):
        owned, intent = registered_unix_launch(Path.cwd() / 'synthetic-owned')
        for rows, intents, endpoint in (({}, [dict(unix_registered=False)], True),
                ({10: (1, 'S', 'server')}, [intent], True),
                ({30: (20, 'S', 'native'), 40: (30, 'S', 'child')}, [intent], True),
                ({}, [intent], False), ({}, [intent], None), ({}, [dict(unix_registered=True)], True)):
            with self.subTest(rows=rows, intents=intents, endpoint=endpoint), self.assertRaises(AssertionError):
                fixture.unix_absence_proof(copy.deepcopy(owned), rows, intents, endpoint)
        tracked = copy.deepcopy(owned)
        fixture.observe_unix_descendants(tracked, {30: (20, 'S', 'native'), 40: (30, 'S', 'child')})
        with self.assertRaises(AssertionError):
            fixture.unix_absence_proof(tracked, {40: (1, 'S', 'child')}, [intent], True)
        fixture.unix_absence_proof(tracked, {40: (1, 'S', 'different birth')}, [intent], True)
        self.assertNotIn((99, 'foreign'), tracked)
        for change in ('before', 'server', 'native', 'supervisor', 'socket', 'image'):
            bad = copy.deepcopy(intent)
            if change == 'before': del bad['unix_before']
            if change in ('server', 'native', 'supervisor'): del bad['unix_' + change]
            if change == 'socket': bad['unix_server']['socket'] = str(Path.cwd() / 'foreign.sock')
            if change == 'image': bad['unix_native']['image'] = str(Path.cwd() / 'foreign-image')
            with self.subTest(change=change), self.assertRaises(AssertionError):
                fixture.unix_absence_proof(copy.deepcopy(owned), {}, [bad], True)

    def test_unix_registration_refuses_foreign_server_path_image_birth_parent_and_identity_exhaustion(self):
        with tempfile.TemporaryDirectory() as folder:
            for change in ('socket', 'image', 'birth', 'parent', 'cap'):
                value, rows, _ = self.unix_custody_double(Path(folder))
                rows.update({10: (1, 'S', 'server'), 20: (10, 'S', 'supervisor'), 30: (20, 'S', 'native')})
                value.cleanup_intents = [dict(unix_registered=False, unix_server=dict(pid=10, birth='server',
                    image=fixture.os.path.realpath('/fixture/tools/tmux'), socket=value.socket))]
                record = dict(run_id='run', pid=30, process_start='native', supervisor=dict(pid=20, start='supervisor'))
                if change == 'socket': value.socket = str(Path(folder) / 'foreign.sock')
                if change == 'image': value.unix_image = lambda pid, birth: '/fixture/foreign/image'
                if change == 'birth': record['process_start'] = 'changed'
                if change == 'parent': rows[30] = (99, 'S', 'native')
                if change == 'cap': value.unix_owned = {(100 + n, 'birth'): {} for n in range(2048)}
                with patch.object(fixture, 'test_pause', SimpleNamespace(TMUX='/fixture/tools/tmux'), create=True), \
                        self.subTest(change=change), self.assertRaises(AssertionError):
                    value.register_unix_run(record)
                self.assertFalse(value.cleanup_intents[0]['unix_registered'])

    def test_unix_query_expiry_and_kernel_birth_change_refuse_custody(self):
        with tempfile.TemporaryDirectory() as folder:
            value, _, _ = self.unix_custody_double(Path(folder))
            value.unix_query = fixture.HintContract.unix_query.__get__(value)
            calls = []
            def remaining(maximum):
                calls.append(maximum)
                if len(calls) == 3: raise AssertionError('original end')
                return maximum
            value.remaining = remaining
            with patch.object(fixture.subprocess, 'run', return_value=SimpleNamespace(returncode=0, stdout='', stderr='')), \
                    self.assertRaisesRegex(AssertionError, 'original end'):
                value.unix_query(['/bin/ps', '-p', '10', '-o', 'lstart='])
            births = iter(('known birth', 'different birth'))
            value.remaining = lambda maximum: maximum
            value.unix_query = lambda argv: SimpleNamespace(returncode=0, stdout=next(births))
            with patch.object(fixture.sys, 'platform', 'linux'), \
                    patch.object(fixture.os, 'readlink', return_value='/fixture/owned/image'), \
                    self.assertRaisesRegex(AssertionError, 'changed owned birth'):
                fixture.HintContract.unix_image(value, 10, 'known birth')

    def test_unregistered_unix_run_prevents_startup_consent_action(self):
        value, actions = self.gate_double([state(card())]), []
        def incomplete(record): raise AssertionError('UNKNOWN new run launch custody')
        value.register_unix_run = incomplete
        with patch.object(fixture, 'os', SimpleNamespace(name='posix')), \
                patch.object(fixture.time, 'monotonic', return_value=0), \
                patch.object(fixture.subprocess, 'run', side_effect=self.fake_answer(actions)), \
                self.assertRaisesRegex(AssertionError, 'launch custody'):
            value.gate('codex/p/test', 'run')
        self.assertEqual((actions, value.proven, value.attempted), ([], [], set()))

    def test_private_socket_connect_sends_no_bytes_and_only_two_absence_errors_are_accepted(self):
        import errno
        import stat
        connected = []
        class Endpoint:
            def __enter__(self): return self
            def __exit__(self, *args): return False
            def settimeout(self, timeout): self.timeout = timeout
            def connect(self, path):
                connected.append(path)
                if self.error: raise self.error
            def send(self, *args): raise AssertionError('no endpoint bytes may be sent')
        endpoint = Endpoint()
        metadata = SimpleNamespace(st_mode=stat.S_IFSOCK, st_uid=42)
        for code in (errno.ENOENT, errno.ECONNREFUSED, errno.EACCES, errno.EINTR, errno.ETIMEDOUT, None):
            endpoint.error = OSError(code, 'synthetic') if code is not None else None
            with patch.object(fixture.os, 'lstat', return_value=metadata), \
                    patch.object(fixture.os, 'geteuid', return_value=42, create=True), \
                    patch.object(fixture.socket, 'socket', return_value=endpoint):
                if code in (errno.ENOENT, errno.ECONNREFUSED):
                    self.assertTrue(fixture.private_endpoint_absent('/fixture/owned/tmux.sock', lambda n: n))
                elif code is None:
                    self.assertFalse(fixture.private_endpoint_absent('/fixture/owned/tmux.sock', lambda n: n))
                else:
                    with self.assertRaises(AssertionError):
                        fixture.private_endpoint_absent('/fixture/owned/tmux.sock', lambda n: n)
        self.assertEqual(connected, ['/fixture/owned/tmux.sock'] * 6)
        endpoint.error = OSError(errno.ENOENT, 'synthetic')
        checks = []
        def expired_after_connect(maximum):
            checks.append(maximum)
            if len(checks) == 4: raise AssertionError('original end')
            return maximum
        with patch.object(fixture.os, 'lstat', return_value=metadata), \
                patch.object(fixture.os, 'geteuid', return_value=42, create=True), \
                patch.object(fixture.socket, 'socket', return_value=endpoint), \
                self.assertRaisesRegex(AssertionError, 'original end'):
            fixture.private_endpoint_absent('/fixture/owned/tmux.sock', expired_after_connect)

    def test_owned_host_waits_for_empty_sessions_and_refuses_unknown_or_expired_reads(self):
        for views, failure in (([['session'], []], False), ([[1]] * 4, True), ([None], True), ([list(range(17))], True)):
            now, reads = [0.], []
            def read():
                reads.append(now[0])
                return views[min(len(reads) - 1, len(views) - 1)]
            with patch.object(fixture.time, 'monotonic', side_effect=lambda: now[0]), \
                    patch.object(fixture.time, 'sleep', side_effect=lambda n: now.__setitem__(0, now[0] + n)):
                if failure:
                    with self.assertRaises(AssertionError): fixture.wait_owned_sessions_empty(read, lambda n: n, .1)
                else:
                    fixture.wait_owned_sessions_empty(read, lambda n: n, .1)
                    self.assertEqual(len(reads), 2)
        reads = []
        with patch.object(fixture.time, 'monotonic', return_value=5), self.assertRaises(AssertionError):
            fixture.wait_owned_sessions_empty(lambda: reads.append(1) or [], lambda n: n, 5)
        self.assertEqual(reads, [])
        now = [0.]
        def late_read(): now[0] = 5.; return []
        with patch.object(fixture.time, 'monotonic', side_effect=lambda: now[0]), self.assertRaises(AssertionError):
            fixture.wait_owned_sessions_empty(late_read, lambda n: n, 5)

    def test_windows_cleanup_drains_selected_host_before_stop_and_still_requires_same_handle_exit(self):
        with tempfile.TemporaryDirectory() as folder:
            value, now, events, stopped = fixture.HintContract(), [0.], [], [False]
            # Supply only unittest assertions; no inherited Windows/setup code runs.
            for name in ('assertIn', 'assertLessEqual', 'assertEqual', 'assertLess', 'assertTrue', 'assertIsNotNone'):
                setattr(value, name, getattr(self, name))
            value.root, value.row_end, value.names = Path(folder), 60., []
            value.hosts, value.pinned, value.checkpoints = {7: 123}, {}, []
            value.remaining = lambda maximum: min(maximum, 60. - now[0])
            value.status = lambda: dict(running=True, host=dict(pid=7))
            value.note_cleanup_error = lambda *args, **kwargs: self.fail('cleanup must retain any error')
            lists = [dict(panes=[dict(pane='owned')], sessions=['owned']),
                     dict(panes=[], sessions=['draining']), dict(panes=[], sessions=[])]
            def command(request):
                events.append(request['op'])
                if request['op'] == 'list': return lists.pop(0)
                if request['op'] == 'kill': return {}
                self.assertEqual(request['op'], 'stop_host')
                self.assertEqual(lists, [], 'busy host must not be stopped before sessions drain')
                stopped[0] = True
                return {}
            value.ok = command
            kernel = SimpleNamespace(WaitForSingleObject=lambda handle, timeout: 0 if stopped[0] else 0x102,
                                     TerminateProcess=lambda *args: self.fail('already-exited handle must not be killed'),
                                     CloseHandle=lambda handle: events.append(('close', handle)))
            host = SimpleNamespace(KERNEL32=kernel, WAIT_OBJECT_0=0)
            with patch.dict(sys.modules, {'test_windows_host': host}), \
                    patch.object(fixture.time, 'monotonic', side_effect=lambda: now[0]), \
                    patch.object(fixture.time, 'sleep', side_effect=lambda n: now.__setitem__(0, now[0] + n)):
                value.stop_host()
            self.assertEqual(events, ['list', 'kill', 'list', 'list', 'stop_host', ('close', 123)])
            self.assertEqual(value.hosts, {})

    def test_partial_launch_failure_keeps_ownership_and_attempts_independent_cleanup(self):
        import tempfile
        with tempfile.TemporaryDirectory() as folder:
            root, evidence = Path(folder) / 'runtime', Path(folder) / 'evidence'
            root.mkdir(); evidence.mkdir()
            value = self.cleanup_double(root, evidence)
            value.cleanup_intents = [dict(argv=['/fixture/hgs', 'claude'], root=str(root), phase='candidate')]
            def unknown():
                value.steps.append('failed-discovery')
                raise AssertionError('UNKNOWN launch before binding')
            value.stop_unix = value.stop_host = unknown
            with patch.object(fixture.time, 'monotonic', return_value=0), self.assertRaises(AssertionError):
                value.cleanup_owned()
            self.assertTrue(root.exists())
            self.assertIn('model', value.steps)
            self.assertIn('observer', value.steps)
            self.assertFalse(json.loads((evidence / 'cleanup.json').read_text())['verified'])

    def test_original_cleanup_end_expires_without_stop_or_runtime_deletion(self):
        import tempfile
        with tempfile.TemporaryDirectory() as folder:
            root, evidence = Path(folder) / 'runtime', Path(folder) / 'evidence'
            root.mkdir(); evidence.mkdir()
            value = self.cleanup_double(root, evidence)
            with patch.object(fixture.time, 'monotonic', return_value=100), self.assertRaises(AssertionError):
                value.cleanup_owned()
            self.assertEqual(value.steps, [])
            self.assertTrue(root.exists())
            self.assertEqual(value.row_end, 100)

    def test_missing_foreign_and_expired_handles_never_force_stop(self):
        stopped = []
        def stop(handle, code): stopped.append((handle, code)); return 1
        for handle, image in ((None, '/fixture/owned/hgs.exe'), (123, '/fixture/foreign/hgs.exe')):
            with self.assertRaises(AssertionError):
                fixture.stop_pinned_handle(handle, lambda: image, '/fixture/owned', lambda *args: 0x102,
                                           stop, lambda limit: limit)
        def expired(limit): raise AssertionError('original end')
        with self.assertRaises(AssertionError):
            fixture.stop_pinned_handle(123, lambda: '/fixture/owned/hgs.exe', '/fixture/owned',
                                       lambda *args: 0x102, stop, expired)
        self.assertEqual(stopped, [])

    def test_unavailable_pinned_image_requires_exit_on_that_same_handle_or_remains_unknown(self):
        calls, signals = [], []
        for state_after in (0, 0x102, 0xffffffff):
            states = iter((0x102, state_after))
            def wait(handle, timeout):
                calls.append((handle, timeout))
                return next(states)
            def stop(*args): signals.append(args); return 1
            if state_after == 0:
                fixture.stop_pinned_handle(123, lambda: None, '/fixture/owned', wait, stop, lambda n: n)
            else:
                with self.assertRaisesRegex(AssertionError, 'image unavailable'):
                    fixture.stop_pinned_handle(123, lambda: None, '/fixture/owned', wait, stop, lambda n: n)
        self.assertEqual(calls, [(123, 0), (123, 5000)] * 3)
        with self.assertRaisesRegex(AssertionError, 'absolute image unavailable'):
            fixture.stop_pinned_handle(123, lambda: 'hgs.exe', '/fixture/owned', lambda *args: 0x102,
                                       lambda *args: signals.append(args), lambda n: n)
        self.assertEqual(signals, [])

    def test_missing_image_passive_wait_cannot_pass_before_or_after_the_original_end(self):
        for expire_after_wait in (False, True):
            calls, images, signals, checks = [], [], [], []
            def image(): images.append(123); return None
            def wait(handle, milliseconds):
                calls.append((handle, milliseconds))
                return 0x102 if milliseconds == 0 else 0
            def remaining(maximum):
                checks.append(maximum)
                if not expire_after_wait or len(checks) == 2:
                    raise AssertionError('original end')
                return maximum
            with self.subTest(expire_after_wait=expire_after_wait), self.assertRaisesRegex(AssertionError, 'original end'):
                fixture.stop_pinned_handle(123, image, '/fixture/owned', wait,
                                           lambda *args: signals.append(args), remaining)
            self.assertEqual(images, [123])
            self.assertEqual(calls, [(123, 0), (123, 5000)] if expire_after_wait else [(123, 0)])
            self.assertEqual(checks, [5, 1] if expire_after_wait else [5])
            self.assertEqual(signals, [])

    def test_image_query_uses_only_the_supplied_handle_and_never_opens_a_pid(self):
        import ctypes
        seen = []
        class Query:
            def __call__(self, handle, flags, buffer, size):
                seen.append((handle, flags))
                buffer.value = '/fixture/owned/hgs.exe'
                return 1
        kernel = SimpleNamespace(QueryFullProcessImageNameW=Query())
        with patch.object(ctypes, 'WinDLL', return_value=kernel, create=True):
            image = fixture.pinned_image_query()
            self.assertEqual(image(123), '/fixture/owned/hgs.exe')
        self.assertEqual(seen, [(123, 0)])
        self.assertFalse(hasattr(kernel, 'OpenProcess'))

    def test_cleanup_details_bound_arguments_and_retain_error_source_without_locals(self):
        sentinel = 'private fixture local value must not be captured'
        def failed():
            private_local = sentinel
            if private_local:
                raise TypeError('missing image', None, 'x' * 2048)
        try:
            failed()
        except TypeError as error:
            detail = fixture.cleanup_error('owned handle', error, pid=123)
        self.assertEqual(detail['error'], 'TypeError')
        self.assertEqual(detail['arguments'][0], dict(type='str', text='missing image', truncated=False))
        self.assertEqual(detail['arguments'][1], dict(type='NoneType', text='None', truncated=False))
        self.assertEqual(len(detail['arguments'][2]['text']), 1024)
        self.assertIs(detail['arguments'][2]['truncated'], True)
        self.assertEqual(detail['frames'][-1]['function'], 'failed')
        self.assertEqual(detail['context'], dict(pid=123))
        for frame in detail['frames']:
            self.assertEqual(set(frame), {'file', 'line', 'function'})
            self.assertNotIn('f_locals', frame)
        self.assertNotIn(sentinel, json.dumps(detail))

    def test_cleanup_scalar_rendering_bounds_before_conversion_and_never_calls_custom_repr(self):
        class Untrusted(int):
            def __repr__(self): raise AssertionError('custom repr must not run')
        detail = fixture.cleanup_error('synthetic', AssertionError(b'\xff' * 100_000, 1 << 100_000, Untrusted(1)))
        self.assertEqual([x['truncated'] for x in detail['arguments']], [True, True, False])
        self.assertTrue(all(len(x['text']) <= 1024 for x in detail['arguments']))
        self.assertEqual(detail['arguments'][1]['text'], '<integer exceeds diagnostic bound>')
        self.assertEqual(detail['arguments'][2]['text'], '<non-scalar argument>')

    def test_diagnostic_failure_preserves_unknown_and_does_not_skip_independent_cleanup(self):
        with tempfile.TemporaryDirectory() as folder:
            root, evidence = Path(folder) / 'runtime', Path(folder) / 'evidence'
            root.mkdir(); evidence.mkdir()
            value = self.cleanup_double(root, evidence)
            def fail(): raise AssertionError('unconfirmed owned stop')
            value.stop_unix = value.stop_host = fail
            with patch.object(fixture, 'cleanup_error', side_effect=ValueError('synthetic formatter failure')), \
                    patch.object(fixture.time, 'monotonic', return_value=0), self.assertRaises(AssertionError):
                value.cleanup_owned()
            saved = json.loads((evidence / 'cleanup.json').read_text())
            self.assertFalse(saved['verified'])
            self.assertTrue(saved['errors_overflow'])
            self.assertTrue(root.exists())
            self.assertIn('model', value.steps)
            self.assertIn('observer', value.steps)

    def test_cleanup_detail_overflow_is_explicit_and_never_turns_failed_cleanup_into_success(self):
        with tempfile.TemporaryDirectory() as folder:
            root, evidence = Path(folder) / 'runtime', Path(folder) / 'evidence'
            root.mkdir(); evidence.mkdir()
            value = self.cleanup_double(root, evidence)
            for _ in range(17): value.note_cleanup_error('private stop', AssertionError('unconfirmed stop'))
            def fail(): raise AssertionError('unconfirmed stop')
            value.stop_unix = value.stop_host = fail
            with patch.object(fixture.time, 'monotonic', return_value=0), self.assertRaises(AssertionError):
                value.cleanup_owned()
            saved = json.loads((evidence / 'cleanup.json').read_text())
            self.assertEqual(len(saved['errors']), 16)
            self.assertIs(saved['errors_overflow'], True)
            self.assertIs(saved['verified'], False)
            self.assertIn('model', value.steps)
            self.assertIn('observer', value.steps)
            self.assertTrue(root.exists())

    def test_hanging_model_close_join_is_clamped_to_original_end(self):
        joined = []
        closer = SimpleNamespace(start=lambda: None, join=lambda seconds: joined.append(seconds), is_alive=lambda: True)
        value = self.gate_double([])
        value.cleaning, value.row_end = True, 10
        value.model = SimpleNamespace(shutdown=lambda: None, server_close=lambda: None)
        value.model_thread = SimpleNamespace(is_alive=lambda: True)
        with patch.object(fixture.time, 'monotonic', return_value=9), \
                patch.object(fixture.threading, 'Thread', return_value=closer), self.assertRaises(AssertionError):
            fixture.HintContract.close_model(value)
        self.assertEqual(joined, [1])

    def test_verified_cleanup_deletes_only_after_post_stop_checks(self):
        import tempfile
        with tempfile.TemporaryDirectory() as folder:
            root, evidence = Path(folder) / 'runtime', Path(folder) / 'evidence'
            root.mkdir(); evidence.mkdir()
            value = self.cleanup_double(root, evidence)
            value.native_copy = Path('/fixture/native')
            def images(stage):
                self.assertTrue(root.is_dir())
                self.assertEqual(stage, 'after owned stop')
                value.steps.append('post-stop-images')
            def observer():
                self.assertTrue(root.is_dir())
                value.steps.append('complete-observer')
            value.keep_evidence, value.final_observer_check = images, observer
            with patch.object(fixture.time, 'monotonic', return_value=0): value.cleanup_owned()
            self.assertTrue(value.cleanup_verified)
            self.assertFalse(root.exists())
            self.assertEqual(value.steps[-3:], ['model', 'post-stop-images', 'complete-observer'])

    def test_every_declared_input_private_copy_and_current_stable_image_rejects_change(self):
        import tempfile
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            payload = b'synthetic sealed fixture executable'
            wanted = hashlib.sha256(payload).hexdigest()
            paths = {}
            for label in ('candidate_input', 'pre_feature_input', 'observer_input', 'candidate_private_cli',
                          'pre_feature_private_cli', 'observer_private', 'native_input', 'native_private',
                          'current_stable_cli', 'initial_portable_candidate'):
                paths[label] = root / label / 'image.bin'
                paths[label].parent.mkdir()
                paths[label].write_bytes(payload)
            value = SimpleNamespace(inputs={name: dict(path=str(paths[name + '_input']), sha256=wanted)
                                            for name in ('candidate', 'pre_feature', 'observer')},
                                    real={phase: paths[phase + '_private_cli'] for phase in ('candidate', 'pre_feature')},
                                    observer_exe=paths['observer_private'], native_input=paths['native_input'],
                                    native_copy=paths['native_private'], native_sha256=wanted,
                                    real_hook=paths['current_stable_cli'], phase='candidate', row_end=100,
                                    hgs_exe=paths['initial_portable_candidate'])
            value.conpty_inputs = {}
            for name in ('conpty.dll', 'OpenConsole.exe'):
                source = root / ('conpty-input-' + name)
                source.write_bytes(payload)
                value.conpty_inputs[name] = dict(path=str(source), sha256=wanted)
                paths['conpty_input:' + name] = source
                for phase in ('candidate', 'pre_feature'):
                    path = value.real[phase].parent / name
                    path.write_bytes(payload)
                    paths[phase + '_conpty:' + name] = path
                path = value.hgs_exe.parent / name
                path.write_bytes(payload)
                paths['initial_portable_conpty:' + name] = path
            with patch.object(fixture.time, 'monotonic', return_value=0):
                rows = fixture.HintContract.image_seals(value)
                self.assertEqual({row['label'] for row in rows}, set(paths))
                self.assertTrue(all(row['sha256'] == wanted for row in rows))
                for label, path in paths.items():
                    path.write_bytes(b'changed synthetic fixture executable')
                    with self.subTest(label=label), self.assertRaisesRegex(AssertionError, label):
                        fixture.HintContract.image_seals(value)
                    path.write_bytes(payload)

    def test_initial_portable_refusal_happens_before_first_execute(self):
        import tempfile
        events = []
        class Portable(fixture.HintContract, unittest.TestCase):
            def execute(self, argv, **options):
                events.append('execute')
                return SimpleNamespace(returncode=0, stdout='', stderr='')
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            payload = b'synthetic initial portable input'
            wanted = hashlib.sha256(payload).hexdigest()
            paths = [root / name for name in ('hgs.exe', 'conpty.dll', 'OpenConsole.exe')]
            for path in paths: path.write_bytes(payload)
            value = Portable()
            value.hgs_exe, value.phase, value.real = paths[0], 'candidate', {}
            value.inputs = dict(candidate=dict(path='/fixture/admitted/hgs.exe', sha256=wanted))
            value.conpty_inputs = {path.name: dict(sha256=wanted) for path in paths[1:]}
            value.env, value.project = {}, root
            value.cleaning, value.end, value.row_end, value.gate_end = False, 100, 160, None
            verify = fixture.verify_image_seals
            def sealed(expected, end):
                events.append('seal')
                return verify(expected, end)
            with patch.object(fixture.time, 'monotonic', return_value=0), \
                    patch.object(fixture, 'verify_image_seals', side_effect=sealed):
                value.hgs('session-host', 'install', '--json')
                self.assertEqual(events, ['seal', 'execute'])
                for path in paths:
                    events.clear()
                    path.write_bytes(b'changed synthetic initial portable input')
                    with self.subTest(path=path.name), self.assertRaises(AssertionError):
                        value.hgs('session-host', 'install', '--json')
                    self.assertEqual(events, ['seal'], 'changed initial copy must prevent execute')
                    path.write_bytes(payload)


if __name__ == '__main__':
    unittest.main()
