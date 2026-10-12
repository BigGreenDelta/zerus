#!/usr/bin/env python3
"""Startup trust bridge on a private tmux socket; never trusts a real workspace."""
import json
import os
import signal
import unittest
import uuid
from pathlib import Path

import test_input as fixtures
import test_questions as question_fixtures

FAKE = r'''
import os,pathlib,signal,sys,termios,time,tty
root=pathlib.Path(os.environ['INPUT_FIXTURE']);fd=sys.stdin.fileno();original=termios.tcgetattr(fd);tty.setraw(fd)
os.write(1,b'\x1b[?2004h');selected=0;buf=b''
labels=['No, exit','Yes, I trust this folder']
def render(*args):
 global selected
 if (root/'selection').exists(): selected=int((root/'selection').read_text());(root/'selection').unlink()
 target=(root/'target').read_text() if (root/'target').exists() else 'git config'
 folder=(root/'folder').read_text() if (root/'folder').exists() else '/work/example'
 screen=pathlib.Path(os.environ['CLAUDE_TRUST_FIXTURE']).read_text().strip().replace('/work/example',folder).replace('git config',target)
 if (root/'prelude').exists():screen=(root/'prelude').read_text()+screen
 if selected==1: screen=screen.replace('❯ No, exit','  No, exit').replace('  Yes, I trust','❯ Yes, I trust')
 os.write(1,b'\x1b[2J\x1b[H'+screen.replace('\n','\r\n').encode())
signal.signal(signal.SIGUSR1,render);render();(root/'agent-pid').write_text(str(os.getpid()))
try:
 while True:
  value=os.read(fd,65536)
  if not value:break
  with (root/'received').open('ab') as f:f.write(value)
  buf+=value
  while buf:
   if buf.startswith((b'\x1b[A',b'\x1bOA')):selected=max(0,selected-1);buf=buf[3:];render()
   elif buf.startswith((b'\x1b[B',b'\x1bOB')):selected=min(1,selected+1);buf=buf[3:];render()
   elif buf.startswith(b'\r'):
    buf=buf[1:];(root/'submitted').write_text(labels[selected])
    if not (root/'hold-dialog').exists():
     if selected==0:sys.exit(0)
     if (root/'transition-input').exists():
      os.write(1,b'\x1b[?2004l\x1b[2J\x1b[HStarting the native input screen');time.sleep(.2)
      if not (root/'hold-input-mode').exists():os.write(1,b'\x1b[?2004h')
     os.write(1,b'\x1b[2J\x1b[HReady for your first message')
   elif buf[0]==27 and len(buf)<3:break
   else:buf=buf[1:]
finally:termios.tcsetattr(fd,termios.TCSANOW,original)
'''

class FolderTrust(unittest.TestCase):
    script = fixtures.InputTransport.script
    tmux = fixtures.InputTransport.tmux
    wait_for = fixtures.InputTransport.wait_for
    received = fixtures.InputTransport.received
    write_record = fixtures.InputTransport.write_record
    tearDown = fixtures.InputTransport.tearDown
    inspect = question_fixtures.Questions.inspect
    answer = question_fixtures.Questions.answer

    def setUp(self):
        os.environ["CLAUDE_TRUST_FIXTURE"] = str(Path(__file__).parent / "fixtures/claude-folder-trust.txt")
        self.addCleanup(lambda: os.environ.pop("CLAUDE_TRUST_FIXTURE", None))
        original = fixtures.FAKE_AGENT
        try:
            fixtures.FAKE_AGENT = FAKE
            fixtures.InputTransport.setUp(self)
        finally:
            fixtures.FAKE_AGENT = original
        self.conversation_id = ''
        self.record.update(agent='claude', run_identity_version=1, supervisor={},
                           conversation_id=None, launch_dir='/work/example',
                           phase='unknown', activity='unknown', last_event_at=0)
        self.write_record()
        self.card = self.inspect()['pending_questions'][0]

    def payload(self, choice=1, **updates):
        p = dict(request_id=str(uuid.uuid4()), expected_run_id=self.run_id,
                 expected_conversation_id='', question_id=self.card['question_id'],
                 expected_question_hash=self.card['question_hash'],
                 answers=[dict(question_id='trust', selected_option_ids=['trust_'+str(choice)], text='')])
        p.update(updates)
        return p

    def redraw(self, file, value, expected):
        (self.root/file).write_text(value)
        os.kill(self.record['pid'], signal.SIGUSR1)
        self.wait_for(lambda: expected in self.tmux('capture-pane', '-p', '-t', self.pane))

    def test_attention_before_conversation_without_automatic_input(self):
        state = self.inspect()
        self.assertEqual(state['phase'], 'approval')
        self.assertEqual(state['attention_id'], self.card['question_id'])
        self.assertTrue(self.card['can_answer'])
        self.assertIn('17 tool permissions', self.card['questions'][0]['body'])
        self.assertIn('/work/example', self.card['questions'][0]['body'])
        p = fixtures.InputTransport.payload(self)
        self.assertIn('trust approval', fixtures.InputTransport.send(self, p, success=False))
        self.assertEqual(self.received(), b'')

    def test_legacy_keychain_prelude_preserves_disclosure_and_explicit_answer(self):
        prefix='hgs: Claude token is in the locked login keychain (ssh context); unlocking it\npassword to unlock /Users/test/Library/Keychains/login.keychain-db: \n\n'
        self.tmux('resize-window','-t','='+self.name+':','-x','120','-y','50')
        self.redraw('prelude',prefix,'password to unlock')
        card=self.inspect()['pending_questions'][0]
        self.assertEqual(card['question_hash'],self.card['question_hash'])
        self.assertIn('17 tool permissions',card['questions'][0]['body'])
        self.assertNotIn('password to unlock',card['questions'][0]['body'])
        self.assertEqual(self.received(),b'')
        self.assertEqual(self.answer(self.payload())['status'],'answered')
        self.assertEqual((self.root/'submitted').read_text(),'Yes, I trust this folder')

    def test_explicit_trust_navigates_then_submits_once(self):
        self.redraw('selection','0','❯ No, exit')
        self.assertEqual(self.inspect()['pending_questions'][0]['question_hash'],self.card['question_hash'])
        p=self.payload();receipt=self.answer(p)
        self.assertEqual(receipt['status'],'answered')
        self.assertEqual((self.root/'submitted').read_text(),'Yes, I trust this folder')
        before=self.received();self.assertEqual(self.answer(p),receipt);self.assertEqual(self.received(),before)
        self.assertEqual(self.inspect()['pending_questions'],[])

    def test_bitwarden_prelude_preserves_disclosure_and_explicit_answer(self):
        prefixes=[
            'Bitwarden master password:\nunlock: SOPS_AGE_KEY loaded for this session.\nrun_claude: secrets unlocked → launching claude\n\n',
            'run_claude: SOPS_AGE_KEY already set — reusing it.\nrun_claude: secrets unlocked → launching claude\n\n',
        ]
        for width,height in [(95,47),(110,35)]:
            self.tmux('resize-window','-t','='+self.name+':','-x',str(width),'-y',str(height))
            for prefix in prefixes:
                self.redraw('prelude',prefix,'launching claude')
                state=self.inspect();card=state['pending_questions'][0]
                self.assertEqual(state['phase'],'approval')
                self.assertEqual(card['question_hash'],self.card['question_hash'])
                self.assertIn('17 tool permissions',card['questions'][0]['body'])
                self.assertIn('and 9 more',card['questions'][0]['body'])
                self.assertNotIn('SOPS_AGE_KEY',card['questions'][0]['body'])
                self.assertEqual(self.received(),b'')
        payload=self.payload();receipt=self.answer(payload)
        self.assertEqual(receipt['status'],'answered')
        self.assertEqual((self.root/'submitted').read_text(),'Yes, I trust this folder')
        before=self.received();self.assertEqual(self.answer(payload),receipt)
        self.assertEqual(self.received(),before)

    def test_unknown_output_after_bitwarden_is_not_answerable(self):
        self.tmux('resize-window','-t','='+self.name+':','-x','120','-y','50')
        prefix='Bitwarden master password:\nunlock: SOPS_AGE_KEY loaded for this session.\nrun_claude: secrets unlocked → launching claude\nUnrelated command output\n'
        self.redraw('prelude',prefix,'Unrelated command output')
        self.assertNotIn('Unrelated',question_fixtures.terminal_only(self,'Accessing workspace:')['questions'][0]['body'])
        self.answer(self.payload(),success=False)
        self.assertEqual(self.received(),b'')

    def test_trust_waits_for_verified_input_after_native_startup_transition(self):
        (self.root/'transition-input').touch()
        payload=self.payload();receipt=self.answer(payload)
        self.assertEqual(receipt['status'],'answered')
        before=self.received();self.assertEqual(self.answer(payload),receipt)
        self.assertEqual(self.received(),before)

    def test_unverified_input_after_trust_is_uncertain_and_never_retried(self):
        (self.root/'transition-input').touch();(self.root/'hold-input-mode').touch()
        payload=self.payload()
        self.assertIn('delivery uncertain',self.answer(payload,success=False))
        before=self.received()
        self.assertIn('delivery uncertain',self.answer(payload,success=False))
        self.assertEqual(self.received(),before)

    def test_explicit_decline_can_exit_the_exact_process(self):
        p=self.payload(0);receipt=self.answer(p)
        self.assertEqual(receipt['status'],'answered')
        self.assertEqual((self.root/'submitted').read_text(),'No, exit')
        before=self.received();self.assertEqual(self.answer(p),receipt);self.assertEqual(self.received(),before)

    def test_changed_target_rejects_stale_approval(self):
        self.redraw('target','different-command','different-command')
        self.assertNotEqual(self.inspect()['pending_questions'][0]['question_hash'],self.card['question_hash'])
        self.answer(self.payload(),success=False)
        self.assertEqual(self.received(),b'')

    def test_changed_folder_and_run_reject_without_input(self):
        self.answer(self.payload(expected_run_id='other'),success=False)
        self.redraw('folder','/other/folder','/other/folder')
        question_fixtures.terminal_only(self,'Accessing workspace:')
        self.answer(self.payload(),success=False)
        self.assertEqual(self.received(),b'')

    def test_missing_acknowledgement_is_not_retried(self):
        (self.root/'hold-dialog').touch();p=self.payload()
        self.assertIn('delivery uncertain',self.answer(p,success=False));before=self.received()
        self.assertIn('delivery uncertain',self.answer(p,success=False));self.assertEqual(self.received(),before)

if __name__ == '__main__': unittest.main()
