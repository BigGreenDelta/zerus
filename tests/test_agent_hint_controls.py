"""Finite pure controls of the hint fixture; no native/setup/transport is run.

These are authored controls, not OS isolation or native compatibility evidence.
"""
import copy
import hashlib
import json
from pathlib import Path
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
                question_hash='c' * 64, can_answer=True,
                questions=[dict(id='hooks_trust', options=[dict(id='trust')])])


def state(*cards, ready=False):
    return dict(run_id='run', tracked=True, pending_questions=list(cards), first_message_can_send=ready,
                first_message_reason=None if ready else 'not ready')


class HintFixtureControls(unittest.TestCase):
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


if __name__ == '__main__':
    unittest.main()
