#!/usr/bin/env python3
"""Startup trust bridge on a private tmux socket; never starts a real MCP server."""
import json
import os
import signal
import unittest
import uuid

import test_input as fixtures
from test_questions import Questions, terminal_only

FAKE = r'''
import os,pathlib,signal,sys,termios,tty
root=pathlib.Path(os.environ['INPUT_FIXTURE']);fd=sys.stdin.fileno();original=termios.tcgetattr(fd);tty.setraw(fd)
os.write(1,b'\x1b[?2004h');selected=0;buf=b''
labels=['Trust this folder',"Don't trust"]
def render(*args):
 global selected
 if (root/'selection').exists(): selected=int((root/'selection').read_text());(root/'selection').unlink()
 target=(root/'target').read_text() if (root/'target').exists() else 'uvx'
 folder=(root/'folder').read_text() if (root/'folder').exists() else '/work/example'
 prelude=(root/'prelude').read_text().split('\n') if (root/'prelude').exists() else []
 targets=[] if (root/'no-targets').exists() else [' Project MCP targets:',
  '   tracker (stdio): command='+target,'   args=["example-mcp"] cwd=/work/example']
 lines=prelude+['─'*96,' Trust this folder?',' ↑↓ navigate · Enter select · Esc exit','',folder,'',
  ' Project-level MCP servers are disabled until you explicitly choose Trust. Trust starts the',
  ' listed project MCP targets and remembers this folder.']+targets+['']
 for i,label in enumerate(labels):
  lines+=['  '+('❯' if selected==i else ' ')+' '+label,
   '    '+['Enable project MCP servers. Remembered for this folder.','Exit Kimi Code. Asked again next launch.'][i],'']
 lines+=['─'*96]
 os.write(1,b'\x1b[2J\x1b[H'+'\r\n'.join(lines).encode())
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
     if selected==1:sys.exit(0)
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
        self.record.update(agent='kimi', run_identity_version=1, supervisor={},
                           conversation_id=None, launch_dir='/work/example',
                           phase='unknown', activity='unknown', last_event_at=0)
        self.write_record()
        self.card = self.inspect()['pending_questions'][0]

    def payload(self, choice=0, **updates):
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
        self.assertIn('command=uvx', self.card['questions'][0]['body'])
        self.assertIn('/work/example', self.card['questions'][0]['body'])
        p = fixtures.InputTransport.payload(self)
        self.assertIn('trust approval', fixtures.InputTransport.send(self, p, success=False))
        self.assertEqual(self.received(), b'')

    def test_explicit_trust_navigates_then_submits_once(self):
        self.redraw('selection','1',"❯ Don't trust")
        self.assertEqual(self.inspect()['pending_questions'][0]['question_hash'],self.card['question_hash'])
        p=self.payload();receipt=self.answer(p)
        self.assertEqual(receipt['status'],'answered')
        self.assertEqual((self.root/'submitted').read_text(),'Trust this folder')
        before=self.received();self.assertEqual(self.answer(p),receipt);self.assertEqual(self.received(),before)
        self.assertEqual(self.inspect()['pending_questions'],[])

    def test_explicit_decline_can_exit_the_exact_process(self):
        p=self.payload(1);receipt=self.answer(p)
        self.assertEqual(receipt['status'],'answered')
        self.assertEqual((self.root/'submitted').read_text(),"Don't trust")
        before=self.received();self.assertEqual(self.answer(p),receipt);self.assertEqual(self.received(),before)

    def test_changed_target_rejects_stale_approval(self):
        self.redraw('target','different-command','command=different-command')
        self.assertNotEqual(self.inspect()['pending_questions'][0]['question_hash'],self.card['question_hash'])
        self.answer(self.payload(),success=False)
        self.assertEqual(self.received(),b'')

    def test_changed_folder_and_run_reject_without_input(self):
        self.answer(self.payload(expected_run_id='other'),success=False)
        self.redraw('folder','/other/folder','/other/folder')
        terminal_only(self,'Trust this folder?')
        self.answer(self.payload(),success=False)
        self.assertEqual(self.received(),b'')

    def test_launcher_notices_and_panel_without_targets(self):
        (self.root/'no-targets').touch()
        notices = ('? Master password: [hidden]\nunlock: SOPS_AGE_KEY loaded for this session.\n'
                   'run_kimi: secrets unlocked → launching kimi')
        self.redraw('prelude', notices, 'launching kimi')
        self.card = self.inspect()['pending_questions'][0]
        body = self.card['questions'][0]['body']
        self.assertNotIn('Project MCP targets', body)
        self.assertNotIn('launching kimi', body)
        receipt = self.answer(self.payload())
        self.assertEqual(receipt['status'], 'answered')
        self.assertEqual((self.root/'submitted').read_text(), 'Trust this folder')

    def test_unrelated_output_above_panel_is_not_answerable(self):
        self.redraw('prelude', 'agent said:', 'agent said:')
        self.assertNotIn('agent said', terminal_only(self, 'Trust this folder?')['questions'][0]['body'])
        self.answer(self.payload(), success=False)
        self.assertEqual(self.received(), b'')

    def test_missing_acknowledgement_is_not_retried(self):
        (self.root/'hold-dialog').touch();p=self.payload()
        self.assertIn('delivery uncertain',self.answer(p,success=False));before=self.received()
        self.assertIn('delivery uncertain',self.answer(p,success=False));self.assertEqual(self.received(),before)

if __name__ == '__main__': unittest.main()
