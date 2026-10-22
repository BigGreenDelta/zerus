"""Real Claude's auto-mode offer under a finished reply, answered from Activity.

Run with HGS_CLAUDE_TEST_BIN=/absolute/path/to/claude. A private home, socket and
localhost API; the fixture account names a manual default permission mode.
"""
import json
import time
import unittest
import uuid

import test_input as fixtures
import test_questions as questions
import test_claude_questions_native as native

TITLE = 'Make auto mode your default permission mode?'


@unittest.skipUnless(native.CLAUDE, 'Set HGS_CLAUDE_TEST_BIN for isolated native auto-mode offer coverage')
class NativeClaudeAutoModeOffer(unittest.TestCase):
    message_fixture = True
    auto_mode_offer = True
    script = fixtures.InputTransport.script
    tmux = fixtures.InputTransport.tmux
    write_record = fixtures.InputTransport.write_record
    wait_for = native.NativeClaudeQuestions.wait_for
    screen = native.NativeClaudeQuestions.screen
    hook = native.NativeClaudeQuestions.hook
    inspect = questions.Questions.inspect
    answer = questions.Questions.answer

    def setUp(self):
        native.NativeClaudeQuestions.setUp(self)
        # The launch prompt's turn has ended; Claude makes its offer below the reply.
        self.record.update(run_identity_version=1, supervisor={}, launch_dir=str(self.root / 'work'))
        self.write_record()
        self.hook(dict(hook_event_name='UserPromptSubmit', prompt='Ask questions with the tool now.'))
        self.hook(dict(hook_event_name='Stop'))
        self.wait_for(lambda: TITLE in self.screen())
        self.card = self.wait_for(lambda: next((q for q in self.inspect()['pending_questions']
            if q['source'] == 'claude_permission_mode'), None))

    def payload(self, prefix):
        option = next(o for o in self.card['questions'][0]['options'] if o['label'].startswith(prefix))
        return dict(request_id=str(uuid.uuid4()), expected_run_id=self.run_id,
            expected_conversation_id=self.conversation_id, question_id=self.card['question_id'],
            expected_question_hash=self.card['question_hash'], answers=[dict(
                question_id='permission_mode', selected_option_ids=[option['id']], text='')])

    def account(self):
        claude = json.loads((self.root / '.claude.json').read_text())
        settings = json.loads((self.root / '.claude/settings.json').read_text())
        return claude.get('hasSeenAutoDefaultNudge'), settings['permissions']['defaultMode']

    def test_offer_waits_for_an_explicit_choice(self):
        state = self.inspect()
        self.assertEqual(state['attention_id'], self.card['question_id'])
        self.assertTrue(self.card['can_answer'], self.screen())
        self.assertEqual(self.card['conversation_id'], self.conversation_id)
        labels = [o['label'] for o in self.card['questions'][0]['options']]
        self.assertEqual(labels[0], 'Yes, set auto mode as my default permission mode')
        self.assertTrue(labels[1].startswith('No, keep '), labels)
        self.assertIn('blocks the rest', self.card['questions'][0]['body'])
        message = dict(request_id=str(uuid.uuid4()), expected_run_id=self.run_id,
                       expected_conversation_id=self.conversation_id, text='Another message')
        self.assertIn('permission mode choice', fixtures.InputTransport.send(self, message, success=False))
        time.sleep(1)
        self.assertIn(TITLE, self.screen())
        self.assertEqual(self.account(), (None, 'default'))

    def test_keep_answers_once_and_records_the_native_decline(self):
        payload = self.payload('No, keep ')
        receipt = self.answer(payload)
        self.assertEqual(receipt['status'], 'answered', self.screen())
        self.wait_for(lambda: self.account()[0] is True)
        self.assertEqual(self.account(), (True, 'default'))
        self.assertNotIn(TITLE, self.screen())
        self.assertEqual(self.inspect()['pending_questions'], [])
        self.assertEqual(self.answer(payload), receipt)

    def test_accept_sets_auto_mode_as_the_account_default(self):
        receipt = self.answer(self.payload('Yes, set auto mode'))
        self.assertEqual(receipt['status'], 'answered', self.screen())
        self.wait_for(lambda: self.account() == (True, 'auto'))
        self.wait_for(lambda: 'auto mode on' in self.screen())
        self.assertEqual(self.inspect()['pending_questions'], [])


if __name__ == '__main__':
    unittest.main()
