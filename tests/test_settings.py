#!/usr/bin/env python3
"""Session model settings against a private tmux socket, never real agents."""
import json
import signal
import subprocess
import time
import unittest
import uuid
import test_input
import test_effort

AGENT = test_effort.AGENT.replace("selected = 1\n", "selected = 1\nmodel = 'GPT-6-Astra'; stage = ''; choice = 0; effort_choice = 2\n")
AGENT = AGENT.replace("if agent == 'codex':\n        screen =", "if agent == 'codex':\n        screen =", 1)
AGENT = AGENT.replace("'\\x1b[28;1H   \\x1b[49m\\x1b[29;1H  GPT-6-Astra ' + effort", "'\\x1b[28;1H   \\x1b[49m\\x1b[29;1H  ' + model + ' ' + effort")
AGENT = AGENT.replace("    os.write(1, screen.encode())", r'''
    if stage:
        names = ['GPT-6-Astra', 'GPT-6-Sol'] if stage == 'model' else ['Low', 'Medium', 'High', 'Extra high', 'More reasoning…']
        if stage == 'advanced': names = ['Max', 'Ultra']
        selected = choice if stage == 'model' else effort_choice
        title = 'Select Model and Effort' if stage == 'model' else 'Select Reasoning Level for GPT-6-Sol'
        if stage == 'advanced': title = 'Advanced Reasoning'
        screen = '\x1b[2J\x1b[10;1H  ' + title + '\r\n\r\n'
        for i, n in enumerate(names): screen += ('› ' if i == selected else '  ') + str(i+1) + '. ' + n + '\r\n'
        screen += '\r\n  enter select · esc back' if stage == 'model' else '\r\n  enter default · s session · esc back'
    os.write(1, screen.encode())''')
AGENT = AGENT.replace("        if agent=='codex' and key in", "        if stage and key == b'\\x1b[B':\n          if stage=='model': choice=(choice+1)%2\n          elif stage=='advanced': effort_choice=(effort_choice+1)%2\n          else: effort_choice=(effort_choice+1)%5\n          show()\n        elif agent=='codex' and key in")
AGENT = AGENT.replace("        if agent=='kimi':\n", r'''        if agent=='codex':
          if stage and char==b's' and stage!='model':
            model='GPT-6-Sol';effort=(['max','ultra'][effort_choice] if stage=='advanced' else ['low','medium','high','xhigh'][effort_choice]);stage='';text=''
          elif stage=='model' and char==b'\r':stage='effort';effort_choice=2
          elif stage=='effort' and char==b'\r' and effort_choice==4:stage='advanced';effort_choice=0
          elif not stage and char==b'\r' and text=='/model':stage='model';choice=0;text=''
          elif not stage and char!=b'\r':text+=char.decode('utf8','replace')
          show()
        if agent=='kimi':
''')

AGENT = AGENT.replace("global agent, effort, text, picker", "global agent, effort, text, picker, model")
AGENT = AGENT.replace("agent = 'kimi'; effort = 'high'", "agent = 'kimi'; model = 'K3'; effort = 'high'")
AGENT = AGENT.replace("'\\x1b[28;1H Ask When Needed  K3 thinking: ' + effort", "'\\x1b[28;1H Ask When Needed  ' + model + ' thinking: ' + effort")
AGENT = AGENT.replace("    if stage:\n        names", r"""    if stage == 'kmodel':
        screen='\x1b[2J\x1b[10;1H Select a model\r\n ↑↓ navigate · Enter select · Alt+S session-only · Esc cancel\r\n\r\n'
        for i,n in enumerate(['K3','K4']):screen+=('  ❯ ' if choice==i else '    ')+n+'  kimi\r\n'
        screen+='\r\n Thinking (←→ to switch)\r\n '+ ' '.join('[ '+e.title()+' ]' if i==effort_choice else e.title() for i,e in enumerate(['low','high','max']))
    elif stage:
        names""")
AGENT = AGENT.replace("        if stage and key == b'\\x1b[B':", r"""        if stage=='kmodel':
          if key==b'\x1b[B':choice=(choice+1)%2
          elif key==b'\x1b[C':effort_choice=min(2,effort_choice+1)
          elif key==b'\x1b[D':effort_choice=max(0,effort_choice-1)
          elif key==b'\x1bs':model=['K3','K4'][choice];effort=['low','high','max'][effort_choice];stage='';text=''
          show()
        elif stage and key == b'\x1b[B':""")
AGENT = AGENT.replace("if char==b'\\r' and text=='/effort':", "if char==b'\\r' and text=='/model':stage='kmodel';text='';choice=0;effort_choice=1\n          elif char==b'\\r' and text=='/effort':")

class SessionSettings(test_effort.EffortTransport):
    # Reuse fixture methods, not the legacy command tests.
    def setUp(self):
        original = test_effort.AGENT
        test_effort.AGENT = AGENT
        try: super().setUp()
        finally: test_effort.AGENT = original
        self.cache=self.root/'.codex/models_cache.json'
        models=json.loads(self.cache.read_text())['models']
        models[0]['display_name']='GPT-6-Astra'
        models.append(dict(slug='gpt-6-sol',display_name='GPT-6-Sol',default_reasoning_level='medium',supported_reasoning_levels=[dict(effort=e) for e in ['low','medium','high','xhigh','max','ultra']]))
        self.cache.write_text(json.dumps(dict(models=models)))

    def settings(self, payload=None, success=True):
        p=payload or self.payload()
        result=subprocess.run([str(test_input.HGS),'settings',self.name,'--json'],env=self.env,input=json.dumps(p),text=True,capture_output=True,timeout=20)
        if success:
            self.assertEqual(result.returncode,0,result.stderr)
            return json.loads(result.stdout)
        self.assertNotEqual(result.returncode,0,result.stdout)
        return result.stderr

    def test_model_and_effort_apply_session_only(self):
        p=self.payload('low');p['model']='gpt-6-sol'
        before=self.cache.read_bytes()
        result=self.settings(p)
        self.assertEqual(result['status'],'applied')
        self.assertEqual(result['model'],'gpt-6-sol')
        self.assertEqual(self.inspect()['model'],'gpt-6-sol')
        self.assertEqual(self.inspect()['effort'],'low')
        self.assertEqual(self.received().count(b'\r'),2) # open model and reasoning, never commit defaults
        self.assertTrue(self.received().endswith(b's'))
        sent=self.received();self.settings(p);self.assertEqual(self.received(),sent)
        p['model']='gpt-6-astra';self.assertIn('already used',self.settings(p,False))
        self.assertEqual(self.cache.read_bytes(),before)
        self.assertFalse((self.root/'.codex/config.toml').exists())

    def fresh(self):
        self.conversation_id=''
        self.record.update(conversation_id=None,run_identity_version=1,supervisor={},last_event_at=0,activity='unknown',phase='unknown')
        self.write_record()

    def test_model_and_effort_before_first_message_keep_the_same_run(self):
        self.fresh();info=self.inspect()
        self.assertTrue(info['first_message_can_send']);self.assertTrue(info['settings_change_supported']);self.assertEqual(info['settings_apply_when'],'now')
        p=self.payload('low');p['model']='gpt-6-sol';before=self.cache.read_bytes()
        result=self.settings(p);self.assertEqual(result['status'],'applied')
        info=self.inspect();self.assertEqual(info['model'],'gpt-6-sol');self.assertEqual(info['effort'],'low')
        self.assertTrue(info['first_message_can_send']);self.assertEqual(info['run_id'],self.run_id);self.assertFalse(info.get('conversation_id'))
        self.assertEqual(self.received().count(b'\r'),2);self.assertTrue(self.received().endswith(b's'))
        self.assertNotIn(b'\x1b[200~',self.received());self.assertEqual(self.cache.read_bytes(),before)
        sent=self.received();self.settings(p);self.assertEqual(self.received(),sent)

    def test_fresh_settings_reject_unconfirmed_resume_pending_input_and_stale_identity(self):
        self.fresh();p=self.payload('low');p['model']='gpt-6-sol'
        for updates in [dict(expected_id='requested'),dict(requested_id='requested'),dict(fork_parent_id='parent'),dict(startup_kind='resume'),dict(input_pending_at=1),dict(active_tools={'call':{}}),dict(supervisor=None)]:
            with self.subTest(updates=updates):
                saved=dict(self.record);self.record.update(updates);self.write_record();self.settings(p,success=False)
                self.assertEqual(self.received(),b'');self.record=saved;self.write_record()
        self.settings(dict(p,expected_run_id='old'),success=False);self.assertEqual(self.received(),b'')
        self.record['conversation_id']='new-conversation';self.write_record();self.settings(p,success=False);self.assertEqual(self.received(),b'')

    def test_fresh_settings_preserve_terminal_draft_and_reject_dialog(self):
        self.fresh();p=self.payload('low');p['model']='gpt-6-sol'
        (self.root/'screen').write_text('\x1b[2J\x1b[H› Keep my draft')
        __import__('os').kill(self.record['pid'],signal.SIGUSR1)
        self.wait_for(lambda:'Keep my draft' in self.tmux('capture-pane','-p','-t',self.pane))
        self.assertFalse(self.inspect()['settings_change_supported']);self.settings(p,success=False);self.assertEqual(self.received(),b'')

    def test_advanced_max_uses_session_action(self):
        p=self.payload('max');p['model']='gpt-6-sol'
        self.settings(p)
        self.assertEqual(self.inspect()['effort'],'max')
        self.assertTrue(self.received().endswith(b's'))

    def test_explicit_ultra_uses_advanced_picker(self):
        p=self.payload('ultra');p['model']='gpt-6-sol'
        self.settings(p)
        self.assertEqual(self.inspect()['effort'],'ultra')
        self.assertTrue(self.received().endswith(b's'))
        self.assertNotIn('ultra',self.inspect()['effort_options']) # legacy hotkey API stays guarded

    def test_busy_schedule_then_explicit_ready_apply(self):
        self.record.update(activity='busy',phase='working');self.write_record()
        p=self.payload('low');p['model']='gpt-6-sol'
        result=self.settings(p)
        self.assertEqual(result['status'],'scheduled');self.assertEqual(self.received(),b'')
        info=self.inspect();self.assertEqual(info['model'],'gpt-6-astra')
        self.assertEqual(info['pending_model'],'gpt-6-sol');self.assertEqual(info['settings_apply_when'],'ready')
        self.record=json.loads(self.record_path.read_text());self.record.update(activity='idle',phase='idle');self.write_record()
        p['request_id']=str(uuid.uuid4());self.settings(p)
        self.assertEqual(self.inspect()['pending_model'],'')

    def test_busy_pending_does_not_block_message(self):
        self.record.update(activity='busy',phase='working');self.write_record()
        self.settings(self.payload('low'))
        message=dict(request_id=str(uuid.uuid4()),expected_run_id=self.run_id,expected_conversation_id=self.conversation_id,text='continue')
        result=subprocess.run([str(test_input.HGS),'send',self.name,'--json'],env=self.env,input=json.dumps(message),text=True,capture_output=True,timeout=10)
        self.assertEqual(result.returncode,0,result.stderr)
        # The fixture agent records its input after hgs returns.
        self.wait_for(lambda:self.received().endswith(b'\r'))
        self.assertEqual(self.received(),b'\x1b[200~continue\x1b[201~\r')
        self.assertEqual(self.inspect()['pending_effort'],'low')

    def test_paused_recipe_has_overrides_without_launching(self):
        self.tmux('kill-server')
        # tmux acknowledges before its pane process has finished exiting.
        # Wait for this owned fixture's PID before asserting saved-run behavior.
        self.wait_for(lambda: not subprocess.run(['ps','-p',str(self.record['pid']),'-o','lstart='],
            capture_output=True,text=True,timeout=5).stdout.strip())
        transcript=self.root/('rollout-'+self.conversation_id+'.jsonl')
        transcript.write_text(json.dumps(dict(type='session_meta',payload=dict(id=self.conversation_id)))+'\n')
        self.record.update(paused=True,base=['codex','--model','old','-c','model_reasoning_effort="high"'],launch_dir=str(self.root),transcript=str(transcript),shell='/bin/bash')
        self.write_record()
        p=self.payload('low');p['model']='gpt-6-sol'
        result=self.settings(p);self.assertEqual(result['status'],'scheduled')
        self.assertEqual(self.inspect()['settings_apply_when'],'resume')
        dry=subprocess.run([str(test_input.HGS),'resume',self.name,'--dry-run'],env=self.env,text=True,capture_output=True,timeout=10)
        self.assertEqual(dry.returncode,0,dry.stderr)
        self.assertIn('--model gpt-6-sol',dry.stdout)
        self.assertNotIn('--model old',dry.stdout)
        self.assertIn('model_reasoning_effort=',dry.stdout)
        self.assertIn('low',dry.stdout)
        self.assertEqual(self.received(),b'')

    def test_explicit_old_model_does_not_use_history_as_confirmation(self):
        self.record['effort']='xhigh';self.write_record()
        (self.root/'screen').write_text('\x1b[2J\x1b[27;1H\x1b[48;5;234m› '+ ' '*80 +'\x1b[28;1H   \x1b[49m\x1b[29;1H  GPT-6-Sol xhigh · /fixture\x1b[30;1H  ? for shortcuts\x1b[27;3H')
        __import__('os').kill(self.record['pid'],signal.SIGUSR1)
        self.wait_for(lambda:'GPT-6-Sol xhigh' in self.tmux('capture-pane','-p','-t',self.pane))
        (self.root/'freeze').touch()
        p=self.payload('xhigh');p['model']='gpt-6-astra'
        self.assertIn('not confirmed',self.settings(p,False))
        self.assertEqual(self.received(),b'/model')

    def test_stale_automatic_apply_cannot_replace_newer_intent(self):
        self.record.update(activity='busy',phase='working');self.write_record()
        first=self.payload('low');self.settings(first)
        second=self.payload('high');self.settings(second)
        self.record=json.loads(self.record_path.read_text());self.record.update(activity='idle',phase='idle');self.write_record()
        stale=self.payload('low');stale['expected_pending_id']=first['request_id']
        self.assertIn('pending settings changed',self.settings(stale,False))
        self.assertEqual(self.received(),b'')
        self.assertEqual(self.inspect()['pending_effort'],'high')
        current=self.payload('high');current['expected_pending_id']=second['request_id']
        self.assertEqual(self.settings(current)['status'],'applied')
        self.assertEqual(self.received(),b'\x1b,')
        self.assertEqual(json.loads((self.state/'settings_receipts'/f"{second['request_id']}.json").read_text())['status'],'applied')

    def test_auto_apply_cannot_bypass_uncertain_original_receipt(self):
        self.record.update(activity='busy',phase='working');self.write_record()
        first=self.payload('low');self.settings(first)
        self.record=json.loads(self.record_path.read_text());self.record.update(activity='idle',phase='idle');self.write_record()
        receipt=self.state/'settings_receipts'/f"{first['request_id']}.json"
        value=json.loads(receipt.read_text());value['status']='in_progress';receipt.write_text(json.dumps(value))
        retry=self.payload('low');retry['expected_pending_id']=first['request_id']
        self.assertIn('not safe to retry',self.settings(retry,False))
        self.assertEqual(self.received(),b'')

    def test_pending_settings_survive_multiple_renames(self):
        import hashlib
        self.record.update(activity='busy',phase='working');self.write_record()
        self.settings(self.payload('low'))
        for tag in ['one','two']:
            target='codex/input-test/'+tag
            result=subprocess.run([str(test_input.HGS),'rename',self.name,target],env=self.env,text=True,capture_output=True,timeout=10)
            self.assertEqual(result.returncode,0,result.stderr);self.name=target
        self.record_path=self.state/(hashlib.sha256(self.name.encode()).hexdigest()+'.json')
        self.record=json.loads(self.record_path.read_text());self.record.update(activity='idle',phase='idle');self.write_record()
        message=dict(request_id=str(uuid.uuid4()),expected_run_id=self.run_id,expected_conversation_id=self.conversation_id,text='continue')
        result=subprocess.run([str(test_input.HGS),'send',self.name,'--json'],env=self.env,input=json.dumps(message),text=True,capture_output=True,timeout=10)
        self.assertEqual(result.returncode,0,result.stderr)
        self.assertTrue(self.received().startswith(b'\x1b,'*3))
        self.assertEqual(self.inspect()['pending_effort'],'')

    def test_new_conversation_discards_old_pending_intent(self):
        self.record.update(activity='busy',phase='working');self.write_record()
        self.settings(self.payload('low'))
        self.conversation_id=str(uuid.uuid4())
        event=dict(session_id=self.conversation_id,hook_event_name='SessionStart',
                   transcript_path=str(self.root / '.codex' / ('rollout-'+self.conversation_id+'.jsonl')))
        env=dict(self.env,HGS_SESSION=self.name,HGS_RUN_ID=self.run_id)
        hook=subprocess.run([str(test_input.HGS),'__state','hook'],env=env,input=json.dumps(event),text=True,capture_output=True,timeout=10)
        self.assertEqual(hook.returncode,0,hook.stderr)
        self.assertEqual(self.inspect()['pending_effort'],'')
        self.assertNotIn('resume_settings',json.loads(self.record_path.read_text()))
        message=dict(request_id=str(uuid.uuid4()),expected_run_id=self.run_id,expected_conversation_id=self.conversation_id,text='continue')
        result=subprocess.run([str(test_input.HGS),'send',self.name,'--json'],env=self.env,input=json.dumps(message),text=True,capture_output=True,timeout=10)
        self.assertEqual(result.returncode,0,result.stderr)
        # The fixture agent records its input after hgs returns.
        self.wait_for(lambda:self.received().endswith(b'\r'))
        self.assertEqual(self.received(),b'\x1b[200~continue\x1b[201~\r')

    def test_kimi_model_and_effort_never_write_defaults(self):
        self.record.update(agent='kimi',model='kimi-code/k3');self.write_record()
        config=self.root/'.kimi-code/config.toml';config.parent.mkdir()
        config.write_text('[models."kimi-code/k3"]\ndisplay_name="K3"\ncapabilities=["thinking","always_thinking"]\nsupport_efforts=["low","high","max"]\n[models."kimi-code/k4"]\ndisplay_name="K4"\ncapabilities=["thinking","always_thinking"]\nsupport_efforts=["low","high","max"]\n')
        before=config.read_bytes();(self.root/'set-kimi').touch();__import__('os').kill(self.record['pid'],signal.SIGUSR1)
        self.wait_for(lambda:'K3 thinking: high' in self.tmux('capture-pane','-p','-t',self.pane))
        p=self.payload('max');p['model']='kimi-code/k4'
        self.settings(p)
        self.assertEqual(self.received(),b'/model\r\x1b[B\x1b[C\x1bs')
        self.assertEqual(config.read_bytes(),before)
        self.assertEqual(self.inspect()['model'],'kimi-code/k4')
        self.assertEqual(self.inspect()['effort'],'max')

    def test_kimi_context_choice_schedules_settings_until_it_is_answered(self):
        # Kimi's context choice shows only on the terminal, over a Ready saved
        # record. No key reaches it: inspect schedules the change, as dispatch
        # does, and holds back forks and effort changes until it is answered.
        self.record.update(agent='kimi',model='kimi-code/k3',base=['kimi']);self.write_record()
        config=self.root/'.kimi-code/config.toml';config.parent.mkdir()
        config.write_text('[models."kimi-code/k3"]\ndisplay_name="K3"\ncapabilities=["thinking","always_thinking"]\nsupport_efforts=["low","high","max"]\n[models."kimi-code/k4"]\ndisplay_name="K4"\ncapabilities=["thinking","always_thinking"]\nsupport_efforts=["low","high","max"]\n')
        def redraw(expected):
            __import__('os').kill(self.record['pid'],signal.SIGUSR1)
            self.wait_for(lambda:expected in self.tmux('capture-pane','-p','-t',self.pane))
        (self.root/'set-kimi').touch();redraw('K3 thinking: high');(self.root/'set-kimi').unlink()
        rows=['─'*96,' This session has been idle for 2h and is ~217k tokens.',' ↑↓ navigate · Enter select · Esc cancel','',
              ' Cache expired — the next message re-sends the entire history at full price.']
        rows+=['  '+('❯' if i==0 else ' ')+' '+label for i,label in
               enumerate(['Compact and continue','Start a new session','Continue as-is',"Don't ask me again"])]
        rows+=['','─'*96,' Ask When Needed  K3 thinking: high /fixture',' context: 22% (217k/1M)']
        # The status line keeps the prompt's rows, so the current effort is known.
        (self.root/'screen').write_text('\x1b[2J\x1b[17;1H'+'\r\n'.join(rows));redraw('Cache expired')
        reason="Wait until the agent's question is answered"
        info=self.inspect();self.assertEqual((info['phase'],info['pending_questions'][0]['source']),('input','kimi_cache_hint'))
        self.assertTrue(info['settings_change_supported']);self.assertEqual(info['settings_apply_when'],'ready')
        for action in ('fork','effort_change'):
            self.assertEqual((info[action+'_supported'],info[action+'_reason']),(False,reason),action)
        p=self.payload('max');p['model']='kimi-code/k4'
        result=self.settings(p)
        self.assertEqual((result['status'],result['apply_when'],result['pending_settings_id']),('scheduled','ready',p['request_id']))
        info=self.inspect()
        self.assertEqual((info['model'],info['pending_model'],info['pending_effort']),('kimi-code/k3','kimi-code/k4','max'))
        # An apply of the pending change stays scheduled, and `hgs effort`
        # refuses the chooser; neither sends a key.
        retry=self.payload('max');retry.update(model='kimi-code/k4',expected_pending_id=p['request_id'])
        result=self.settings(retry)
        self.assertEqual((result['status'],result['apply_when'],result['pending_settings_id']),('scheduled','ready',p['request_id']))
        self.assertIn('context choice',self.change(self.payload('low'),success=False))
        self.assertEqual(self.received(),b'')
        # Answered: Kimi's prompt returns, and the pending change applies.
        (self.root/'set-kimi').touch();redraw('K3 thinking: high')
        info=self.inspect();self.assertEqual(info['settings_apply_when'],'now')
        for action in ('fork','effort_change'):
            self.assertEqual((info[action+'_supported'],info[action+'_reason']),(True,''),action)
        apply=self.payload('max');apply.update(model='kimi-code/k4',expected_pending_id=p['request_id'])
        self.assertEqual(self.settings(apply)['status'],'applied')
        self.assertEqual(self.received(),b'/model\r\x1b[B\x1b[C\x1bs')
        info=self.inspect()
        self.assertEqual((info['model'],info['effort'],info['pending_model']),('kimi-code/k4','max',''))

    def test_kimi_model_before_first_message(self):
        self.fresh()
        self.test_kimi_model_and_effort_never_write_defaults()
        self.assertTrue(self.inspect()['first_message_can_send'])
        self.assertFalse(self.inspect().get('conversation_id'))

    def test_unconfirmed_change_never_replays_keys(self):
        (self.root/'freeze').touch();p=self.payload('low');p['model']='gpt-6-sol'
        self.assertIn('not confirmed',self.settings(p,False))
        self.assertEqual(self.received(),b'/model')
        self.assertIn('already',self.settings(p,False))
        self.assertEqual(self.received(),b'/model')
        self.assertEqual(self.inspect()['model'],'gpt-6-astra')

    def test_unread_settings_keys_are_not_a_first_message(self):
        self.fresh();(self.root/'freeze').touch();p=self.payload('low');p['model']='gpt-6-sol'
        self.assertIn('not confirmed',self.settings(p,False))
        record=json.loads(self.record_path.read_text())
        self.assertFalse(record.get('input_pending_at'));self.assertIn('settings_unconfirmed_at',record)
        info=self.inspect();self.assertFalse(info['first_message_can_send'])
        self.assertIn('did not react',info['first_message_reason'])
        self.assertEqual(self.received(),b'/model')

    def test_pending_settings_apply_before_ready_message(self):
        self.record.update(activity='busy',phase='working');self.write_record()
        self.settings(self.payload('low'))
        self.record=json.loads(self.record_path.read_text());self.record.update(activity='idle',phase='idle');self.write_record()
        message=dict(request_id=str(uuid.uuid4()),expected_run_id=self.run_id,expected_conversation_id=self.conversation_id,text='continue')
        result=subprocess.run([str(test_input.HGS),'send',self.name,'--json'],env=self.env,input=json.dumps(message),text=True,capture_output=True,timeout=10)
        self.assertEqual(result.returncode,0,result.stderr)
        # The fixture agent records its input after hgs returns.
        self.wait_for(lambda:self.received().endswith(b'\r'))
        self.assertEqual(self.received(),b'\x1b,'*3+b'\x1b[200~continue\x1b[201~\r')
        self.assertEqual(self.inspect()['pending_effort'],'')

    def test_draft_unknown_model_and_identity_fail_without_input(self):
        p=self.payload();p['model']='not-a-model';self.settings(p,False)
        p=self.payload();p['expected_run_id']='old';self.settings(p,False)
        self.assertEqual(self.received(),b'')
        (self.root/'screen').write_text('\x1b[2J\x1b[H› keep this draft')
        oskill=self.record['pid'];__import__('os').kill(oskill,signal.SIGUSR1)
        self.wait_for(lambda:'draft' in self.tmux('capture-pane','-p','-t',self.pane))
        self.assertIn('draft',self.settings(success=False));self.assertEqual(self.received(),b'')

# Legacy tests are run separately by test_effort.py.
for name in list(test_effort.EffortTransport.__dict__):
    if name.startswith('test_'):setattr(SessionSettings,name,None)
if __name__=='__main__':unittest.main()
