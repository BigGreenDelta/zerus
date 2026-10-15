#!/usr/bin/env python3
"""Kimi cache chooser bridge: private terminal, no provider or paid requests."""
import json, os, signal, subprocess, unittest, uuid
import test_input as fixtures
from test_questions import Questions
FAKE = fixtures.SIGNAL_SAFE_READ + r'''
import json,os,pathlib,signal,sys,termios,tty
root=pathlib.Path(os.environ['INPUT_FIXTURE']);fd=sys.stdin.fileno();original=termios.tcgetattr(fd);tty.setraw(fd)
os.write(1,b'\x1b[?2004h');selected=0;buf=b''
labels=['Compact and continue','Start a new session','Continue as-is',"Don't ask me again"]
def render(*args):
 global selected
 if (root/'selection').exists(): selected=int((root/'selection').read_text());(root/'selection').unlink()
 lines=['─'*96,' This session has been idle for 2h and is ~217k tokens.',' ↑↓ navigate · Enter select · Esc cancel','',' Cache expired — the next message re-sends the entire history at full price.']
 lines+=['  '+('❯' if selected==i else ' ')+' '+label for i,label in enumerate(labels)]
 lines+=['','─'*96,' Ask When Needed K3 thinking: max [1 task running] /work/example main',' context: 22% (217k/1M)']
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
   elif buf.startswith((b'\x1b[B',b'\x1bOB')):selected=min(3,selected+1);buf=buf[3:];render()
   elif buf.startswith(b'\r'):
    buf=buf[1:];(root/'submitted').write_text(labels[selected])
    if not (root/'hold-dialog').exists():os.write(1,b'\x1b[2J\x1b[HAgent working')
   elif buf[0]==27 and len(buf)<3:break
   else:buf=buf[1:]
finally:termios.tcsetattr(fd,termios.TCSANOW,original)
'''
class CacheHint(unittest.TestCase):
    script=fixtures.InputTransport.script
    tmux=fixtures.InputTransport.tmux
    wait_for=fixtures.InputTransport.wait_for
    received=fixtures.InputTransport.received
    write_record=fixtures.InputTransport.write_record
    tearDown=fixtures.InputTransport.tearDown
    inspect=Questions.inspect
    answer=Questions.answer
    def setUp(self):
        original=fixtures.FAKE_AGENT
        try:
            fixtures.FAKE_AGENT=FAKE;fixtures.InputTransport.setUp(self)
        finally:fixtures.FAKE_AGENT=original
        self.record.update(agent='kimi',active_tools={},phase='idle',activity='idle');self.write_record()
        self.card=self.inspect()['pending_questions'][0]
    def payload(self,choice=0,**updates):
        p=dict(request_id=str(uuid.uuid4()),expected_run_id=self.run_id,expected_conversation_id=self.conversation_id,
               question_id=self.card['question_id'],expected_question_hash=self.card['question_hash'],
               answers=[dict(question_id='cache',selected_option_ids=['cache_'+str(choice)],text='')]);p.update(updates);return p
    def test_detects_pending_choice_and_does_not_send_anything(self):
        self.assertTrue(self.card['can_answer']);self.assertEqual(self.card['source'],'kimi_cache_hint')
        self.assertFalse(self.card['questions'][0]['allow_other']);self.assertEqual(self.inspect()['phase'],'input')
        self.assertEqual(self.received(),b'')
        p=fixtures.InputTransport.payload(self)
        e=fixtures.InputTransport.send(self,p,success=False)
        self.assertIn('context choice',e);self.assertEqual(self.received(),b'')
    def test_context_actions_wait_for_the_choice_as_dispatch_does(self):
        # The saved record stays idle; the terminal chooser alone keeps both
        # actions unavailable, with the reason, as dispatch refuses them.
        info=self.inspect();self.assertEqual((info['activity'],info['phase']),('busy','input'))
        for action in ('clear_context','compact_context'):
            self.assertFalse(info[action+'_supported'],action)
            self.assertEqual(info[action+'_reason'],"Wait until the agent's question is answered",action)
        for command in ('clear-context','compact-context'):
            request=dict(request_id=str(uuid.uuid4()),expected_run_id=self.run_id,expected_conversation_id=self.conversation_id)
            r=subprocess.run([str(fixtures.HGS),command,self.name,'--json'],env=self.env,input=json.dumps(request),
                             text=True,capture_output=True,timeout=12)
            self.assertNotEqual(r.returncode,0,command);self.assertIn('context choice',r.stderr,command)
        self.assertEqual(self.received(),b'')
    def test_fork_and_settings_wait_for_the_choice_as_dispatch_does(self):
        # The saved record is Ready; the terminal chooser alone keeps a fork
        # unavailable and schedules a settings change, in inspect and the list.
        self.record.update(base=['kimi']);self.write_record()
        reason="Wait until the agent's question is answered"
        ls=subprocess.run([str(fixtures.HGS),'ls','--local','--json'],env=self.env,text=True,capture_output=True,timeout=12)
        self.assertEqual(ls.returncode,0,ls.stderr)
        row=next(r for r in json.loads(ls.stdout)['sessions'] if r['name']==self.name)
        for info in (self.inspect(),row):
            self.assertEqual((info['fork_supported'],info['fork_reason']),(False,reason))
            self.assertNotIn('fork',info['allowed_actions']);self.assertEqual(info['action_reasons']['fork'],reason)
            self.assertEqual(info['settings_apply_when'],'ready')
        r=subprocess.run([str(fixtures.HGS),'fork',self.name,'-n','branch','-d','--dry-run'],env=self.env,
                         text=True,capture_output=True,timeout=12)
        self.assertNotEqual(r.returncode,0,r.stdout);self.assertIn('context choice',r.stderr)
        self.assertEqual(self.received(),b'')
    def test_all_native_options_and_stable_identity_during_navigation(self):
        for choice in range(4):
            with self.subTest(choice=choice):
                (self.root/'selection').write_text('3');os.kill(self.record['pid'],signal.SIGUSR1)
                self.wait_for(lambda:'❯ Don\'t ask' in self.tmux('capture-pane','-p','-t',self.pane))
                self.assertEqual(self.inspect()['pending_questions'][0]['question_hash'],self.card['question_hash'])
                p=self.payload(choice);r=self.answer(p)
                self.assertEqual(r['status'],'answered')
                self.assertEqual((self.root/'submitted').read_text(),self.card['questions'][0]['options'][choice]['label'])
                before=self.received();self.assertEqual(self.answer(p),r);self.assertEqual(self.received(),before)
                self.assertEqual(self.inspect()['pending_questions'],[])
    def test_stale_question_and_free_text_are_rejected_without_keys(self):
        self.answer(self.payload(expected_question_hash='0'*64),success=False)
        self.answer(self.payload(expected_run_id='other'),success=False)
        self.answer(self.payload(answers=[dict(question_id='cache',selected_option_ids=[],text='anything')]),success=False)
        self.assertEqual(self.received(),b'')
    def test_missing_acknowledgement_is_uncertain_and_never_retried(self):
        (self.root/'hold-dialog').touch();p=self.payload()
        self.assertIn('delivery uncertain',self.answer(p,success=False));before=self.received()
        self.assertIn('delivery uncertain',self.answer(p,success=False));self.assertEqual(self.received(),before)

if __name__=='__main__':unittest.main()
