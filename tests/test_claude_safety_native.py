"""Native Claude safety confirmations in private homes against a local API.

The rm operation is fenced to this fixture's generated working directory.
No live session, real account or model service is used.
"""
import json
import shlex
import subprocess
import time
import unittest
import uuid

import test_claude_questions_native as native
import test_input as transport
import test_questions as questions


@unittest.skipUnless(native.CLAUDE, 'Set HGS_CLAUDE_TEST_BIN for private native safety qualification')
class NativeClaudeSafety(unittest.TestCase):
    tool_name = 'Bash'
    native_permission_mode = 'bypassPermissions'
    script = native.NativeClaudeQuestions.script
    tmux = native.NativeClaudeQuestions.tmux
    write_record = native.NativeClaudeQuestions.write_record
    wait_for = native.NativeClaudeQuestions.wait_for
    screen = native.NativeClaudeQuestions.screen
    hook = native.NativeClaudeQuestions.hook
    inspect = native.NativeClaudeQuestions.inspect
    answer = questions.Questions.answer
    payload = questions.Questions.payload

    def native_tool_input(self, work):
        prefix = ':\n' * 90 if 'clipped' in self._testMethodName else ''
        command = ('T=$(pwd); test "$T" = ' + shlex.quote(str(work)) +
                   ' && rm -f $T/*.jsonl $T/*.port && printf approved > approved.txt')
        return dict(command=prefix + command, description='Remove owned fixture files after confirmation')

    def setUp(self):
        native.NativeClaudeQuestions.setUp(self)
        self.assertIn('Dangerous rm operation on possibly-empty variable path:', self.screen())

    def choose(self, label, **updates):
        item = self.card['questions'][0]
        option = next(value for value in item['options'] if value['label'] == label)
        return self.payload(answers=[dict(question_id=item['id'], selected_option_ids=[option['id']], text='')], **updates)

    def without_permission_hook(self, phase='working'):
        record = json.loads(self.record_path.read_text())
        record.pop('pending_approval', None)
        record.pop('pending_approval_at', None)
        record['phase'] = phase
        self.record_path.write_text(json.dumps(record))
        self.card = self.inspect()['pending_questions'][0]

    def test_hook_backed_safety_shows_warning_and_approves_once(self):
        self.assertTrue(self.card['can_answer'], self.card['answer_unavailable_reason'])
        self.assertIn('Dangerous rm operation', self.card['questions'][0]['body'])
        self.assertEqual([option['scope'] for option in self.card['questions'][0]['options']], ['once', 'once'])
        payload = self.choose('Yes')
        self.answer(payload)
        self.wait_for(lambda: (self.root / 'work/approved.txt').exists())
        self.assertEqual((self.root / 'work/approved.txt').read_text(), 'approved')
        self.assertEqual(self.answer(payload)['status'], 'answered')

    def test_deny_does_not_run_the_command(self):
        self.assertTrue(self.card['can_answer'], self.card['answer_unavailable_reason'])
        self.answer(self.choose('No'))
        self.wait_for(lambda: 'What should Claude do instead?' in self.screen())
        self.assertFalse((self.root / 'work/approved.txt').exists())
        self.assertEqual(self.inspect()['pending_questions'], [])

    def test_wrapped_safety_warning_and_denial_at_87_columns(self):
        self.assertTrue(self.card['can_answer'], self.card['answer_unavailable_reason'])
        self.assertIn('Dangerous rm operation', self.card['questions'][0]['body'])
        self.answer(self.choose('No'))
        self.wait_for(lambda: 'What should Claude do instead?' in self.screen())
        self.assertFalse((self.root / 'work/approved.txt').exists())
        self.assertEqual(self.inspect()['pending_questions'], [])

    def test_countdown_does_not_change_the_question_identity(self):
        original = self.card['question_hash']
        time.sleep(1.2)
        self.assertEqual(self.inspect()['pending_questions'][0]['question_hash'], original)

    def test_missing_hook_projects_attention_and_blocks_send_and_answer(self):
        self.without_permission_hook()
        state = self.inspect()
        self.assertEqual(state['phase'], 'approval')
        self.assertEqual(self.card['source'], 'claude_terminal_safety')
        self.assertFalse(self.card['can_answer'])
        self.assertIn('Dangerous rm operation', self.card['questions'][0]['body'])
        self.assertEqual(json.loads(self.record_path.read_text())['phase'], 'working')
        self.answer(self.choose('Yes'), success=False)
        payload = dict(request_id=str(uuid.uuid4()), expected_run_id=self.run_id,
                       expected_conversation_id=self.conversation_id, text='Keep this unsent fixture message')
        result = subprocess.run([str(transport.HGS), 'send', self.name, '--json'], env=self.env,
                                input=json.dumps(payload), text=True, capture_output=True, timeout=15)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('safety approval', result.stderr)
        self.assertIn('Do you want to proceed?', self.screen())
        self.assertFalse((self.root / 'work/approved.txt').exists())

    def test_changed_command_and_hash_never_approve(self):
        self.answer(self.choose('Yes', expected_question_hash='0' * 64), success=False)
        self.hook(dict(hook_event_name='PermissionRequest', tool_name='Bash', tool_input={'command':'printf different'}))
        changed = self.inspect()['pending_questions'][0]
        self.assertFalse(changed['can_answer'])
        self.assertIn('Dangerous rm operation', changed['questions'][0]['body'])
        self.answer(self.choose('Yes'), success=False)
        self.assertIn('Do you want to proceed?', self.screen())
        self.assertFalse((self.root / 'work/approved.txt').exists())

    def test_clipped_command_keeps_warning_with_hook_and_without_hook(self):
        self.assertFalse(self.card['can_answer'])
        self.assertIn('Dangerous rm operation', self.card['questions'][0]['body'])
        self.without_permission_hook('tool')
        self.assertFalse(self.card['can_answer'])
        self.assertEqual(self.inspect()['phase'], 'approval')
        self.assertIn('Dangerous rm operation', self.card['questions'][0]['body'])
        self.assertIn('Terminal', self.card['answer_unavailable_reason'])
        self.answer(self.choose('Yes'), success=False)
        self.assertFalse((self.root / 'work/approved.txt').exists())


if __name__ == '__main__':
    unittest.main()
