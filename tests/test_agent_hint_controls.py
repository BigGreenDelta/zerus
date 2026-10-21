"""Finite pure controls of the hint fixture; no native/setup/transport is run.

These are authored controls, not OS isolation or native compatibility evidence.
"""
import copy
import builtins
import hashlib
import json
from pathlib import Path
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


def state(*cards, ready=False):
    return dict(run_id='run', tracked=True, pending_questions=list(cards), first_message_can_send=ready,
                first_message_reason='' if ready else 'not ready')


class HintFixtureControls(unittest.TestCase):
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
                hasCompletedOnboarding=True, theme='dark',
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
        return value

    def fake_answer(self, calls):
        def answer(argv, **options):
            calls.append((argv, options))
            payload = json.loads(options['input'])
            receipt = dict(status='answered', name='codex/p/test', request_id=payload['request_id'],
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
