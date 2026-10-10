#!/usr/bin/env python3
"""Claude cold-conversation consent on a private terminal; no real history."""
import os
import signal
import time
import unittest
import uuid
from pathlib import Path

import test_input as fixtures
import test_questions as questions
import test_claude_trust as trust

FAKE = trust.FAKE.replace("labels=['No, exit','Yes, I trust this folder']",
                          "labels=['Resume','Start a new conversation']")
FAKE = FAKE.replace("'git config'", "'about 5%'")
FAKE = FAKE.replace("screen.replace('❯ No, exit','  No, exit').replace('  Yes, I trust','❯ Yes, I trust')",
                    "screen.replace('❯ 1. Resume','  1. Resume').replace('  2. Start','❯ 2. Start')")
FAKE = FAKE.replace('     if selected==0:sys.exit(0)\n', '')
FAKE = FAKE.replace(" os.write(1,b'\\x1b[2J\\x1b[H'+screen", " if (root/'unfocused').exists():screen=screen.replace('❯ 1. Resume','1. Resume').replace('Enter to confirm','↑/↓ to pick a row first · Enter to confirm')\n os.write(1,b'\\x1b[2J\\x1b[H'+screen")


class ResumeConsent(unittest.TestCase):
    script = fixtures.InputTransport.script
    tmux = fixtures.InputTransport.tmux
    wait_for = fixtures.InputTransport.wait_for
    received = fixtures.InputTransport.received
    write_record = fixtures.InputTransport.write_record
    tearDown = fixtures.InputTransport.tearDown
    inspect = questions.Questions.inspect
    answer = questions.Questions.answer
    redraw = trust.FolderTrust.redraw

    def setUp(self):
        os.environ['CLAUDE_TRUST_FIXTURE'] = str(Path(__file__).parent / 'fixtures/claude-resume.txt')
        self.addCleanup(lambda: os.environ.pop('CLAUDE_TRUST_FIXTURE', None))
        original = fixtures.FAKE_AGENT
        try:
            fixtures.FAKE_AGENT = FAKE
            fixtures.InputTransport.setUp(self)
        finally:
            fixtures.FAKE_AGENT = original
        self.record.update(agent='claude', run_identity_version=1, supervisor={},
                           launch_dir='/work/example', phase='idle', activity='idle',
                           turn_started=time.time() - 7200, main_done=True)
        self.write_record()
        self.card = self.inspect()['pending_questions'][0]

    def payload(self, choice='resume', **updates):
        value = dict(request_id=str(uuid.uuid4()), expected_run_id=self.run_id,
                     expected_conversation_id=self.conversation_id,
                     question_id=self.card['question_id'], expected_question_hash=self.card['question_hash'],
                     answers=[dict(question_id='resume', selected_option_ids=[choice], text='')])
        value.update(updates)
        return value

    def test_resident_session_attention_preserves_warning_and_never_auto_answers(self):
        state = self.inspect()
        self.assertEqual(state['phase'], 'approval')
        self.assertEqual(state['attention_id'], self.card['question_id'])
        self.assertTrue(self.card['can_answer'])
        self.assertIn('293.6k tokens long', self.card['questions'][0]['body'])
        self.assertIn('about 5%', self.card['questions'][0]['body'])
        self.assertEqual([o['label'] for o in self.card['questions'][0]['options']],
                         ['Resume', 'Start a new conversation'])
        self.assertFalse(any(o.get('selected') for o in self.card['questions'][0]['options']))
        self.assertIn('conversation resume choice', fixtures.InputTransport.send(
            self, fixtures.InputTransport.payload(self), success=False))
        self.assertEqual(self.received(), b'')

    def test_explicit_resume_navigates_up_and_repeated_receipt_sends_no_keys(self):
        self.redraw('selection', '1', '❯ 2. Start')
        self.assertEqual(self.inspect()['pending_questions'][0]['question_hash'], self.card['question_hash'])
        payload = self.payload()
        receipt = self.answer(payload)
        self.assertEqual(receipt['status'], 'answered')
        self.assertEqual((self.root / 'submitted').read_text(), 'Resume')
        self.assertEqual(self.received().count(b'\r'), 1)
        before = self.received()
        self.assertEqual(self.answer(payload), receipt)
        self.assertEqual(self.received(), before)
        self.assertEqual(self.inspect()['pending_questions'], [])

    def test_explicit_new_conversation_navigates_down_once(self):
        self.assertEqual(self.answer(self.payload('new_conversation'))['status'], 'answered')
        self.assertEqual((self.root / 'submitted').read_text(), 'Start a new conversation')
        self.assertEqual(self.received().count(b'\r'), 1)

    def test_native_resume_before_session_start_allows_exact_expected_identity(self):
        self.record.update(expected_id=self.conversation_id, phase='unknown', last_event_at=0)
        self.write_record()
        self.card = self.inspect()['pending_questions'][0]
        self.assertEqual(self.answer(self.payload())['status'], 'answered')

    def test_changed_warning_or_identity_rejects_without_terminal_input(self):
        self.answer(self.payload(expected_run_id='other'), success=False)
        self.answer(self.payload(expected_conversation_id='other'), success=False)
        self.redraw('target', 'about 25%', 'about 25%')
        self.assertNotEqual(self.inspect()['pending_questions'][0]['question_hash'], self.card['question_hash'])
        self.answer(self.payload(), success=False)
        self.assertEqual(self.received(), b'')

    def test_native_draft_focus_keeps_card_visible_without_sending_keys(self):
        (self.root / 'unfocused').touch()
        os.kill(self.record['pid'], signal.SIGUSR1)
        self.wait_for(lambda: 'pick a row first' in self.tmux('capture-pane', '-p', '-t', self.pane))
        card = self.inspect()['pending_questions'][0]
        self.assertEqual(card['question_hash'], self.card['question_hash'])
        self.assertFalse(card['can_answer'])
        self.answer(self.payload(), success=False)
        self.assertEqual(self.received(), b'')

    def test_working_session_does_not_offer_a_historical_panel(self):
        self.record['phase'] = 'working'
        self.write_record()
        self.assertEqual(self.inspect()['pending_questions'], [])
        self.answer(self.payload(), success=False)
        self.assertEqual(self.received(), b'')

    def test_unconfirmed_submission_never_replays_the_same_request(self):
        (self.root / 'hold-dialog').touch()
        payload = self.payload()
        self.assertIn('delivery uncertain', self.answer(payload, success=False))
        before = self.received()
        self.assertIn('delivery uncertain', self.answer(payload, success=False))
        self.assertEqual(self.received(), before)


if __name__ == '__main__':
    unittest.main()
