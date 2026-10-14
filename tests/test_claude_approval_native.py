"""Native Claude permissions, using only a localhost API and temporary files."""
import os, unittest
import test_claude_questions_native as native
import test_questions as questions

@unittest.skipUnless(native.CLAUDE, 'Set HGS_CLAUDE_TEST_BIN for the isolated native approval fixture')
class NativeClaudeApprovals(unittest.TestCase):
    tool_name = 'Bash'
    tool_input = {'command': 'printf approved > approved.txt', 'description': 'Create a fixture file after approval'}
    setUp = native.NativeClaudeQuestions.setUp
    script = native.NativeClaudeQuestions.script
    tmux = native.NativeClaudeQuestions.tmux
    write_record = native.NativeClaudeQuestions.write_record
    wait_for = native.NativeClaudeQuestions.wait_for
    screen = native.NativeClaudeQuestions.screen
    hook = native.NativeClaudeQuestions.hook
    inspect = native.NativeClaudeQuestions.inspect
    answer = questions.Questions.answer
    payload = questions.Questions.payload

    def options(self):
        return self.card['questions'][0]['options']

    def choose(self, label, **updates):
        option = next(o for o in self.options() if o['label'] == label)
        return self.payload(answers=[dict(question_id='approval', selected_option_ids=[option['id']], text='')], **updates)

    def test_native_options_and_approve_once_do_not_change_permission_mode(self):
        self.assertTrue(self.card['approval'])
        self.assertTrue(self.card['can_answer'], self.card['answer_unavailable_reason'])
        self.assertEqual(self.card['tool_call_id'], 'toolu_native_fixture')
        self.assertIn(self.tool_input['command'], self.card['questions'][0]['body'])
        self.assertEqual(self.options()[0], dict(id='1', label='Yes', scope='once'))
        self.assertEqual(self.options()[-1], dict(id=str(len(self.options())), label='No', scope='once'))
        self.assertTrue(all(o['scope'] == 'broader' for o in self.options()[1:-1]), self.options())
        self.answer(self.choose('Yes'))
        self.wait_for(lambda: (self.root/'work/approved.txt').exists())
        self.assertEqual((self.root/'work/approved.txt').read_text(), 'approved')
        self.assertNotIn('bypassPermissions', (self.root/'.claude/settings.json').read_text())

    def test_deny_interrupts_without_running_and_reopens_the_composer(self):
        self.answer(self.choose('No'))
        self.wait_for(lambda: 'What should Claude do instead?' in self.screen())
        self.assertFalse((self.root/'work/approved.txt').exists())
        state = self.inspect()
        self.assertEqual(state['phase'], 'interrupted')
        self.assertEqual(state['pending_questions'], [])

    def test_changed_command_and_hash_send_no_approval(self):
        self.answer(self.choose('Yes', expected_question_hash='0'*64), success=False)
        self.assertIn('Do you want to proceed?', self.screen())
        self.assertFalse((self.root/'work/approved.txt').exists())
        self.hook(dict(hook_event_name='PermissionRequest', tool_name='Bash', tool_input={'command':'rm changed'}))
        changed = self.inspect()['pending_questions'][0]
        self.assertFalse(changed['can_answer'])
        self.assertIn('Terminal', changed['answer_unavailable_reason'])
        self.answer(self.choose('Yes'), success=False)
        self.assertIn('Do you want to proceed?', self.screen())
        self.assertFalse((self.root/'work/approved.txt').exists())


class NativeClaudeReadApproval(NativeClaudeApprovals):
    tool_name = 'Read'
    tool_input = {'file_path': '/outside-fixture/notes.txt'}
    test_native_options_and_approve_once_do_not_change_permission_mode = None
    test_deny_interrupts_without_running_and_reopens_the_composer = None
    test_changed_command_and_hash_send_no_approval = None

    def test_session_grant_is_marked_broader_and_deny_reaches_claude(self):
        self.assertTrue(self.card['can_answer'], self.card['answer_unavailable_reason'])
        self.assertIn('Read(/outside-fixture/notes.txt)', self.card['questions'][0]['body'])
        self.assertEqual([o['scope'] for o in self.options()], ['once', 'broader', 'once'])
        self.answer(self.choose('No'))
        self.wait_for(lambda: 'What should Claude do instead?' in self.screen())
        self.assertEqual(self.inspect()['phase'], 'interrupted')


class NativeClaudeFetchApproval(NativeClaudeReadApproval):
    tool_name = 'WebFetch'
    tool_input = {'url': 'https://example.com', 'prompt': 'title?'}
    panel_question = 'Do you want to allow Claude to fetch this content?'
    test_session_grant_is_marked_broader_and_deny_reaches_claude = None

    def test_feedback_denial_without_footer(self):
        self.assertTrue(self.card['can_answer'], self.card['answer_unavailable_reason'])
        self.assertEqual(self.options()[-1]['scope'], 'once')
        self.assertEqual(self.options()[1]['scope'], 'broader')
        self.answer(self.choose(self.options()[-1]['label']))
        self.wait_for(lambda: 'What should Claude do instead?' in self.screen())

class NativeClaudeInterrupt(NativeClaudeReadApproval):
    tool_name = 'Bash'
    tool_input = {'command': 'sleep 30; printf done > slept.txt', 'description': 'Wait for the fixture'}
    test_session_grant_is_marked_broader_and_deny_reaches_claude = None

    def test_escape_interrupt_is_recorded_without_a_hook(self):
        import json, time, uuid
        self.answer(self.choose('Yes'))
        self.wait_for(lambda: 'Do you want to proceed?' not in self.screen())
        record = json.loads(self.record_path.read_text())
        record.update(run_identity_version=1, supervisor=dict(pid=record['pid'], start=record['process_start']), activity='busy', turn_started=time.time())
        self.record_path.write_text(json.dumps(record))
        import subprocess, test_input
        result = subprocess.run([str(test_input.HGS), 'interrupt', self.name, '--json'], env=self.env, text=True, capture_output=True, timeout=15,
            input=json.dumps(dict(request_id=str(uuid.uuid4()), expected_run_id=self.run_id, expected_conversation_id=self.conversation_id,
                                  expected_turn_started=record['turn_started'])))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue(json.loads(result.stdout)['confirmed'], self.screen())
        self.assertIn('What should Claude do instead?', self.screen())
        self.assertEqual(self.inspect()['phase'], 'interrupted')
        self.assertFalse((self.root/'work/slept.txt').exists())

class NativeClaudeTerminalAnswers(NativeClaudeReadApproval):
    """Answers chosen in Terminal itself: Claude reports none of them with a hook."""
    tool_name = 'Bash'
    tool_input = {'command': 'sleep 20; printf done > slept.txt', 'description': 'Wait for the fixture'}
    test_session_grant_is_marked_broader_and_deny_reaches_claude = None
    key = native.NativeClaudeQuestions.key

    def settled(self, phase):
        return self.wait_for(lambda: (state := self.inspect())['phase'] == phase and state)

    def test_yes_settles_once_the_command_shows_it_runs(self):
        self.key('Enter')
        state = self.settled('tool')
        self.assertEqual(state['pending_questions'], [])
        self.assertNotIn('Do you want to proceed?', self.screen())
        self.assertFalse((self.root/'work/slept.txt').exists())

    def test_no_ends_the_turn_as_an_interruption(self):
        for _ in self.options()[1:]: self.key('Down')
        self.key('Enter')
        self.assertEqual(self.settled('interrupted')['pending_questions'], [])
        self.assertIn('What should Claude do instead?', self.screen())

    def test_escape_ends_the_turn_as_an_interruption(self):
        self.key('Escape')
        self.assertEqual(self.settled('interrupted')['pending_questions'], [])
        self.assertIn('What should Claude do instead?', self.screen())

if __name__ == '__main__': unittest.main()
