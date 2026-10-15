#!/usr/bin/env python3
"""Real tmux/PTY coverage of guarded session-only provider effort controls."""
import json
import os
from pathlib import Path
import subprocess
import time
import unittest

import test_input

AGENT = test_input.SIGNAL_SAFE_READ + r'''
import json, os, pathlib, signal, sys, termios, tty
root = pathlib.Path(os.environ['INPUT_FIXTURE'])
fd = sys.stdin.fileno(); original = termios.tcgetattr(fd); tty.setraw(fd)
agent = 'codex'; effort = 'xhigh'; text = ''; picker = False; selected = 1
levels = ['low', 'medium', 'high', 'xhigh', 'max', 'ultra']
def show():
    if agent == 'codex':
        screen = '\x1b[2J\x1b[27;1H\x1b[48;5;234m› ' + text + ' ' * 80 + '\x1b[28;1H   \x1b[49m\x1b[29;1H  GPT-6-Astra ' + effort + ' · /fixture\x1b[30;1H  ? for shortcuts\x1b[27;' + str(3 + len(text)) + 'H'
    else:
        if picker:
            segments = '  '.join('[ ' + v.title() + ' ]' if i == selected else v.title() for i, v in enumerate(['low','high','max']))
            screen = '\x1b[2J\x1b[20;1H────────────────────────────────\r\n Select thinking effort\r\n ←→ switch · Enter select · Alt+S session-only · Esc cancel\r\n\r\n  ' + segments + '\r\n\r\n────────────────────────────────'
        else:
            screen = '\x1b[2J\x1b[25;1H ╭─────────────────────────────────╮\r\n │ > ' + text + ' ' * (29-len(text)) + '│\r\n ╰─────────────────────────────────╯'
        screen += '\x1b[28;1H Ask When Needed  K3 thinking: ' + effort + ' /fixture\x1b[29;1H context: 2%\x1b[26;' + str(6+len(text)) + 'H'
    os.write(1, screen.encode())
def external(*args):
    global agent, effort, text, picker
    if (root / 'set-kimi').exists(): agent = 'kimi'; effort = 'high'; text = ''; picker = False; show()
    elif (root / 'screen').exists(): os.write(1,(root/'screen').read_bytes())
signal.signal(signal.SIGUSR1,external)
os.write(1,b'\x1b[?2004h'); show(); (root/'agent-pid').write_text(str(os.getpid()))
pending=b''
try:
 while True:
    value=read_input(fd)
    if not value: break
    with (root/'received').open('ab') as f: f.write(value)
    if (root/'freeze').exists(): continue
    pending+=value
    while pending:
      if pending[0]==27:
        if len(pending)<2: break
        n=3 if pending[1:2]==b'[' else 2
        if len(pending)<n: break
        key=pending[:n];pending=pending[n:]
        if agent=='codex' and key in [b'\x1b.',b'\x1b,']:
          i=levels.index(effort);i=max(0,min(4,i+(1 if key==b'\x1b.' else -1)));effort=levels[i];show()
        elif agent=='kimi' and picker:
          if key==b'\x1b[C':selected=min(2,selected+1)
          elif key==b'\x1b[D':selected=max(0,selected-1)
          elif key==b'\x1bs':effort=['low','high','max'][selected];picker=False
          show()
      else:
        char=pending[:1];pending=pending[1:]
        if agent=='kimi':
          if char==b'\r' and text=='/effort':picker=True;text='';selected=['low','high','max'].index(effort)
          elif not picker:text+=char.decode('utf8','replace')
          show()
finally:termios.tcsetattr(fd,termios.TCSANOW,original)
'''

class EffortTransport(unittest.TestCase):
    script = test_input.InputTransport.script
    write_record = test_input.InputTransport.write_record
    tmux = test_input.InputTransport.tmux
    wait_for = test_input.InputTransport.wait_for
    received = test_input.InputTransport.received
    tearDown = test_input.InputTransport.tearDown

    def setUp(self):
        original = test_input.FAKE_AGENT
        test_input.FAKE_AGENT = AGENT
        try: test_input.InputTransport.setUp(self)
        finally: test_input.FAKE_AGENT = original
        self.record['model']='gpt-6-astra'
        self.write_record()
        (self.root/'.codex').mkdir()
        (self.root/'.codex/models_cache.json').write_text(json.dumps({'models':[{'slug':'gpt-6-astra','supported_reasoning_levels':[{'effort':v} for v in ['low','medium','high','xhigh','max','ultra']]}]}))
        self.wait_for(lambda:'GPT-6-Astra xhigh' in self.tmux('capture-pane','-p','-t',self.pane))

    def payload(self, effort='high'):
        import uuid
        return dict(request_id=str(uuid.uuid4()),expected_run_id=self.run_id,
                    expected_conversation_id=self.conversation_id,effort=effort)

    def change(self,payload=None,success=True):
        r=subprocess.run([str(test_input.HGS),'effort',self.name,'--json'],env=self.env,
                         input=json.dumps(payload or self.payload()),text=True,capture_output=True,timeout=15)
        if success:
            self.assertEqual(r.returncode,0,r.stderr)
            return json.loads(r.stdout)
        self.assertNotEqual(r.returncode,0,r.stdout)
        return r.stderr

    def inspect(self):
        r=subprocess.run([str(test_input.HGS),'inspect',self.name],env=self.env,text=True,capture_output=True,timeout=5)
        self.assertEqual(r.returncode,0,r.stderr)
        return json.loads(r.stdout)

    def test_terminal_model_change_overrides_old_history_without_a_new_turn(self):
        import signal
        catalog=self.root/'.codex/models_cache.json'
        catalog.write_text(json.dumps({'models':[
            {'slug':'gpt-6-astra','supported_reasoning_levels':[{'effort':'xhigh'}]},
            {'slug':'gpt-6.1-sol','supported_reasoning_levels':[{'effort':'high'}]}]}))
        (self.root/'screen').write_text('\x1b[2J\x1b[27;1H› \x1b[29;1H  GPT-6.1-Sol high · /fixture\x1b[30;1H  ? for shortcuts')
        os.kill(self.record['pid'],signal.SIGUSR1)
        self.wait_for(lambda:'GPT-6.1-Sol high' in self.tmux('capture-pane','-p','-t',self.pane))
        observed=self.inspect()
        self.assertEqual(observed['model'],'gpt-6.1-sol')
        self.assertEqual(observed['effort'],'high')
        self.assertEqual(self.received(),b'')

    def test_inspection_exposes_native_priority_without_changing_current_settings(self):
        catalog=self.root/'.codex/models_cache.json'
        catalog.write_text(json.dumps({'models':[
            {'slug':'gpt-6-astra','display_name':'Example current model','priority':8,
             'supported_reasoning_levels':[{'effort':'xhigh'}]},
            {'slug':'example-zeta','display_name':'Example Zeta','priority':1},
            {'slug':'example-alpha','display_name':'Example Alpha','priority':1},
            {'slug':'example-hidden','priority':0,'visibility':'hide'},
            {'slug':'example-legacy'}]}))
        before=catalog.read_bytes()
        observed=self.inspect()
        choices=observed['model_options']
        self.assertEqual([row['id'] for row in choices],
                         ['example-zeta','example-alpha','gpt-6-astra','example-legacy'])
        self.assertEqual(choices[2]['label'],'Example current model')
        self.assertEqual(choices[2]['effort_options'],['xhigh'])
        self.assertEqual((observed['model'],observed['effort']),('gpt-6-astra','xhigh'))
        self.assertEqual(catalog.read_bytes(),before)
        self.assertEqual(self.received(),b'')

    def test_codex_actual_steps_confirmed_and_idempotent(self):
        p=self.payload('low');r=self.change(p)
        self.assertEqual(r['status'],'confirmed')
        self.assertEqual(r['scope'],'session')
        self.assertEqual(self.received(),b'\x1b,'*3)
        self.assertEqual(self.inspect()['effort'],'low')
        before=self.received();self.change(p);self.assertEqual(self.received(),before)
        p['effort']='high';self.assertIn('already used',self.change(p,False))
        self.assertEqual(self.received(),before)
        self.assertNotIn('ultra',self.inspect()['effort_options'])
        self.assertFalse((self.root/'.codex/config.toml').exists())

    def test_busy_stale_invalid_and_ultra_do_not_send_input(self):
        self.record['phase']='working';self.write_record();self.change(success=False)
        self.record['phase']='idle';self.write_record()
        p=self.payload();p['expected_run_id']='stale';self.change(p,False)
        p=self.payload('/exit');self.change(p,False)
        self.assertIn('Ultra',self.change(self.payload('ultra'),False))
        self.assertEqual(self.received(),b'')

    def test_no_confirmation_never_retries_or_claims_change(self):
        (self.root/'freeze').touch();p=self.payload();self.change(p,False)
        self.assertEqual(self.received(),b'\x1b,')
        self.assertNotIn('effort',json.loads(self.record_path.read_text()))
        self.assertIn('already',self.change(p,False))
        self.assertEqual(self.received(),b'\x1b,')
        self.assertEqual(self.inspect()['effort'],'xhigh')

    def test_existing_draft_is_preserved(self):
        import signal
        (self.root/'screen').write_text('\x1b[2J\x1b[H› do not change this draft')
        os.kill(self.record['pid'],signal.SIGUSR1)
        self.wait_for(lambda:'draft' in self.tmux('capture-pane','-p','-t',self.pane))
        self.assertIn('draft',self.change(success=False));self.assertEqual(self.received(),b'')

    def test_kimi_native_picker_uses_session_only_selection(self):
        import signal
        self.record['agent']='kimi';self.record['model']='kimi-code/k3';self.write_record()
        (self.root/'.kimi-code').mkdir()
        config=(self.root/'.kimi-code/config.toml')
        config.write_text('[models."kimi-code/k3"]\nmodel="k3"\ndisplay_name="K3"\ncapabilities=["thinking", "always_thinking"]\nsupport_efforts=["low","high","max"]\n')
        before=config.read_bytes();(self.root/'set-kimi').touch();os.kill(self.record['pid'],signal.SIGUSR1)
        self.wait_for(lambda:'thinking: high' in self.tmux('capture-pane','-p','-t',self.pane))
        self.change(self.payload('max'))
        self.assertEqual(self.received(),b'/effort\r\x1b[C\x1bs')
        self.assertEqual(self.inspect()['effort'],'max');self.assertEqual(config.read_bytes(),before)
        self.assertEqual(self.inspect()['effort_options'],['low','high','max'])

if __name__=='__main__': unittest.main()
