#!/usr/bin/env python3
"""Hook consent in an isolated native-like TUI; never touches real hook trust."""
import os
import signal
import unittest
import uuid

import test_input as fixtures
from test_questions import Questions

FAKE = fixtures.SIGNAL_SAFE_READ + r'''
import os,pathlib,signal,sys,termios,tty
root=pathlib.Path(os.environ['INPUT_FIXTURE']);fd=sys.stdin.fileno();original=termios.tcgetattr(fd);tty.setraw(fd)
os.write(1,b'\x1b[?2004h');selected=0;buf=b''
labels=['Review hooks','Trust all and continue',"Continue without trusting (hooks won't run)"]
def render(*args):
 global selected
 if (root/'selection').exists():selected=int((root/'selection').read_text());(root/'selection').unlink()
 count=(root/'count').read_text() if (root/'count').exists() else '4'
 lines=['Hooks need review',count+' hooks are new or changed.',
        'Hooks can run outside the sandbox after you trust them.']
 if (root/'error').exists():lines += ['Failed to trust hooks: '+(root/'error').read_text()]
 lines += ['']
 for i,label in enumerate(labels):lines += [('› ' if selected==i else '  ')+str(i+1)+'. '+label]
 lines += ['','enter confirm · esc skip']
 os.write(1,b'\x1b[2J\x1b[H'+'\r\n'.join(lines).encode())
signal.signal(signal.SIGUSR1,render);render();(root/'agent-pid').write_text(str(os.getpid()))
try:
 while True:
  value=read_input(fd)
  if not value:break
  with (root/'received').open('ab') as f:f.write(value)
  buf+=value
  while buf:
   if buf.startswith((b'\x1b[A',b'\x1bOA')):selected=max(0,selected-1);buf=buf[3:];render()
   elif buf.startswith((b'\x1b[B',b'\x1bOB')):selected=min(2,selected+1);buf=buf[3:];render()
   elif buf.startswith(b'\r'):
    buf=buf[1:];(root/'submitted').write_text(labels[selected])
    if not (root/'hold-dialog').exists():
     if (root/'fail-trust').exists() and selected==1:
      (root/'error').write_text('configuration changed');render()
     elif selected==0:
      os.write(1,b'\x1b[2J\x1b[HHooks\r\nLifecycle hooks from config and enabled plugins.\r\n\r\nt trust all \xc2\xb7 enter review \xc2\xb7 esc close')
     else:os.write(1,b'\x1b[2J\x1b[HReady for your first message')
   elif buf[0]==27 and len(buf)<3:break
   else:buf=buf[1:]
finally:termios.tcsetattr(fd,termios.TCSANOW,original)
'''


class HooksTrust(unittest.TestCase):
    script = fixtures.InputTransport.script
    tmux = fixtures.InputTransport.tmux
    wait_for = fixtures.InputTransport.wait_for
    received = fixtures.InputTransport.received
    write_record = fixtures.InputTransport.write_record
    tearDown = fixtures.InputTransport.tearDown
    inspect = Questions.inspect
    answer = Questions.answer

    def setUp(self):
        original = fixtures.FAKE_AGENT
        try:
            fixtures.FAKE_AGENT = FAKE
            fixtures.InputTransport.setUp(self)
        finally:
            fixtures.FAKE_AGENT = original
        self.conversation_id = ''
        self.record.update(agent='codex', run_identity_version=1, supervisor={},
                           conversation_id=None, launch_dir='/work/example',
                           phase='unknown', activity='unknown', last_event_at=0)
        self.write_record()
        self.card = self.inspect()['pending_questions'][0]

    def payload(self, choice='review', **updates):
        result = dict(request_id=str(uuid.uuid4()), expected_run_id=self.run_id,
                      expected_conversation_id='', question_id=self.card['question_id'],
                      expected_question_hash=self.card['question_hash'],
                      answers=[dict(question_id='hooks_trust', selected_option_ids=[choice], text='')])
        result.update(updates)
        return result

    def redraw(self, file, value, expected):
        (self.root / file).write_text(value)
        os.kill(self.record['pid'], signal.SIGUSR1)
        self.wait_for(lambda: expected in self.tmux('capture-pane', '-p', '-t', self.pane))

    def test_startup_approval_disclosure_and_no_automatic_answer(self):
        state = self.inspect()
        self.assertEqual(state['phase'], 'approval')
        self.assertEqual(state['attention_id'], self.card['question_id'])
        self.assertEqual(self.card['source'], 'codex_hooks_trust')
        self.assertTrue(self.card['can_answer'])
        self.assertFalse(self.card.get('optional', False))
        self.assertIn('outside the sandbox', self.card['questions'][0]['body'])
        self.assertIn('4 hooks', self.card['questions'][0]['body'])
        self.assertGreater(self.card['created_at'], 0)
        self.assertIn('hook trust review', fixtures.InputTransport.send(self, fixtures.InputTransport.payload(self), success=False))
        self.assertEqual(self.received(), b'')

    def test_each_choice_is_explicit_and_has_native_acknowledgement(self):
        for choice, label in [('review', 'Review hooks'), ('trust', 'Trust all and continue'),
                              ('continue_without_trusting', "Continue without trusting (hooks won't run)")]:
            with self.subTest(choice=choice):
                self.redraw('selection', '2', '› 3. Continue without trusting')
                self.card = self.inspect()['pending_questions'][0]
                request = self.payload(choice)
                receipt = self.answer(request)
                self.assertEqual(receipt['status'], 'answered')
                self.assertEqual(receipt.get('open_terminal', False), choice == 'review')
                self.assertEqual((self.root / 'submitted').read_text(), label)
                before = self.received()
                self.assertEqual(self.answer(request), receipt)
                self.assertEqual(self.received(), before)
                self.assertEqual(self.inspect()['pending_questions'], [])

    def test_changed_count_error_and_run_reject_without_input(self):
        self.answer(self.payload(expected_run_id='other'), success=False)
        self.redraw('count', '5', '5 hooks are')
        self.answer(self.payload('trust'), success=False)
        self.card = self.inspect()['pending_questions'][0]
        self.redraw('error', 'configuration changed', 'Failed to trust hooks')
        self.answer(self.payload('trust'), success=False)
        self.assertEqual(self.received(), b'')

    def test_resumed_startup_can_answer_but_started_or_unowned_session_cannot(self):
        self.record['expected_id'] = str(uuid.uuid4())
        self.write_record()
        self.card = self.inspect()['pending_questions'][0]
        self.assertEqual(self.answer(self.payload('continue_without_trusting'))['status'], 'answered')
        self.redraw('selection', '0', '› 1. Review hooks')
        for field, value in [('conversation_id', 'started'), ('supervisor', None), ('last_event_at', 1)]:
            with self.subTest(field=field):
                previous = self.record.get(field)
                self.record[field] = value
                self.write_record()
                self.assertEqual(self.inspect()['pending_questions'], [])
                self.record[field] = previous

    def test_no_acknowledgement_is_uncertain_and_never_retried(self):
        (self.root / 'hold-dialog').touch()
        request = self.payload('trust')
        self.assertIn('delivery uncertain', self.answer(request, success=False))
        before = self.received()
        self.assertIn('delivery uncertain', self.answer(request, success=False))
        self.assertEqual(self.received(), before)

    def test_native_trust_error_is_not_reported_as_success(self):
        (self.root / 'fail-trust').touch()
        request = self.payload('trust')
        self.assertIn('delivery uncertain', self.answer(request, success=False))
        current = self.inspect()['pending_questions'][0]
        self.assertNotEqual(current['question_hash'], self.card['question_hash'])
        self.assertIn('configuration changed', current['questions'][0]['body'])


if __name__ == '__main__':
    unittest.main()
