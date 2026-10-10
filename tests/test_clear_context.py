"""Native clear commands over a private tmux socket; never real user sessions."""
import json, subprocess, time, unittest, uuid
import test_input

AGENT = r'''
import os, pathlib, sys, termios, tty, json, uuid, signal
root=pathlib.Path(os.environ['INPUT_FIXTURE']); fd=sys.stdin.fileno(); old=termios.tcgetattr(fd);tty.setraw(fd)
text='';agent='codex'
def show(*args):
 global agent
 agent=(root/'provider').read_text() if (root/'provider').exists() else 'codex'
 if (root/'screen').exists():os.write(1,(root/'screen').read_bytes());return
 row=(' │ > '+text+' '*(29-len(text))+'│') if agent=='kimi' else ('❯ ' if agent=='claude' else '› ')+text
 screen='\x1b[2J\x1b[25;1H'+(' ╭─────────────────────────────────╮' if agent=='kimi' else '──────────────────────────────────')+'\x1b[26;1H'+row+'\x1b[27;1H'+(' ╰─────────────────────────────────╯' if agent=='kimi' else '──────────────────────────────────')+'\x1b[28;1H'+(' Ask When Needed  K3 /fixture' if agent=='kimi' else '')+'\x1b[26;'+str((6 if agent=='kimi' else 3)+len(text))+'H'
 if agent=='codex':
  screen='\x1b[2J\x1b[27;1H\x1b[48;5;234m› '+text+' '*80+'\x1b[28;1H   \x1b[49m\x1b[29;1H  GPT-6-Astra high · /fixture\x1b[30;1H  ? for shortcuts\x1b[27;'+str(3+len(text))+'H'
  if (root/'native-cleared').exists():screen=screen.replace('\x1b[2J','\x1b[2J\x1b[2;1H >_ OpenAI Codex (v0.162.0)\x1b[3;1H /fixture\x1b[4;1H permissions: default')
 os.write(1,screen.encode())
signal.signal(signal.SIGUSR1,show);os.write(1,b'\x1b[?2004h');show();(root/'agent-pid').write_text(str(os.getpid()))
try:
 while True:
  value=os.read(fd,65536)
  with (root/'received').open('ab') as f:f.write(value)
  if (root/'freeze').exists():continue
  for c in value:
   if c==13:
    if text=='/clear':
     p=next((root/'state').glob('*.json'));r=json.loads(p.read_text())
     if (root/'defer-clear-hook').exists():(root/'native-cleared').touch()
     else:r['conversation_id']=str(uuid.uuid4())
     # Match real hooks: readers must see a complete binding during confirmation.
     staged=p.with_suffix('.next');staged.write_text(json.dumps(r));staged.replace(p);text=''
   else:text+=chr(c)
  show()
finally:termios.tcsetattr(fd,termios.TCSANOW,old)
'''
class ClearContext(unittest.TestCase):
 script=test_input.InputTransport.script
 write_record=test_input.InputTransport.write_record
 tmux=test_input.InputTransport.tmux
 wait_for=test_input.InputTransport.wait_for
 received=test_input.InputTransport.received
 tearDown=test_input.InputTransport.tearDown
 def setUp(self):
  original=test_input.FAKE_AGENT;test_input.FAKE_AGENT=AGENT
  try:test_input.InputTransport.setUp(self)
  finally:test_input.FAKE_AGENT=original
 def clear(self,payload=None,success=True,remote=False):
  payload=payload or dict(request_id=str(uuid.uuid4()),expected_run_id=self.run_id,expected_conversation_id=self.conversation_id)
  r=subprocess.run([str(test_input.HGS),*(['@remote'] if remote else []),'clear-context',self.name,'--json'],env=self.env,input=json.dumps(payload),text=True,capture_output=True,timeout=12)
  if success:self.assertEqual(r.returncode,0,r.stderr);return json.loads(r.stdout)
  self.assertNotEqual(r.returncode,0);return r.stderr
 def inspect(self,*args):
  result=subprocess.run([str(test_input.HGS),'inspect',self.name,*args],env=self.env,text=True,capture_output=True,timeout=12)
  self.assertEqual(result.returncode,0,result.stderr);return json.loads(result.stdout)
 def hook(self,event):
  env=dict(self.env,HGS_SESSION=self.name,HGS_RUN_ID=self.run_id)
  result=subprocess.run([str(test_input.HGS),'__state','hook'],env=env,input=json.dumps(event),text=True,capture_output=True,timeout=12)
  self.assertEqual(result.returncode,0,result.stderr)
 def usage_history(self,sid,tokens,cache):
  path=self.root/('rollout-'+sid+'.jsonl')
  path.write_text(json.dumps(dict(type='session_meta',payload=dict(id=sid)))+'\n'+json.dumps(dict(type='event_msg',payload=dict(type='token_count',info=dict(model_context_window=258400,total_token_usage=dict(input_tokens=tokens,cached_input_tokens=cache,output_tokens=0,total_tokens=tokens),last_token_usage=dict(input_tokens=tokens,cached_input_tokens=cache,output_tokens=0,total_tokens=tokens)))))+'\n')
  return str(path)
 def test_deferred_codex_clear_resets_display_without_inventing_identity(self):
  self.record.update(cwd='/fixture',prompt='Previous request',last_message='Previous answer',
                     transcript=self.usage_history(self.conversation_id,109000,100000));self.write_record()
  self.assertEqual(self.inspect()['session_usage']['context']['used'],109000)
  (self.root/'defer-clear-hook').touch()
  self.assertEqual(self.clear()['status'],'confirmed');self.assertEqual(self.received(),b'/clear\r')
  fresh=self.inspect();self.assertEqual(fresh['conversation_id'],self.conversation_id)
  self.assertEqual(fresh['session_clear']['type'],'SessionCleared');self.assertEqual(fresh['session_usage']['status'],'unavailable')
  self.assertFalse(fresh.get('cache_hint'));self.assertFalse(fresh.get('provider_messages'));self.assertFalse(fresh.get('prompt'))
  self.assertEqual(sum(e['type']=='SessionCleared' for e in fresh['events']),1)
  self.assertEqual(self.inspect()['session_clear'],fresh['session_clear']);self.assertEqual(self.received(),b'/clear\r')
  new=str(uuid.uuid4());path=self.usage_history(new,32,8)
  self.hook(dict(hook_event_name='SessionStart',session_id=new,source='clear',cwd='/fixture',transcript_path=path))
  started=self.inspect();self.assertEqual(started['conversation_id'],new)
  self.assertEqual(started['session_clear']['at'],fresh['session_clear']['at'])
  self.assertEqual(started['session_usage']['context']['used'],32);self.assertEqual(started['session_usage']['totals']['cache_read'],8)
  self.assertEqual(sum(e['type']=='SessionCleared' for e in started['events']),1)
 def test_terminal_clear_is_observed_once_without_a_gui_request(self):
  self.record.update(cwd='/fixture',last_message='Previous answer',transcript=self.usage_history(self.conversation_id,109000,100000));self.write_record()
  (self.root/'defer-clear-hook').touch();self.tmux('send-keys','-t',self.pane,'-l','/clear');self.tmux('send-keys','-t',self.pane,'Enter')
  self.wait_for(lambda:(self.root/'native-cleared').exists());time.sleep(.1)
  fresh=self.inspect();self.assertEqual(fresh['session_clear']['type'],'SessionCleared')
  self.assertEqual(fresh['session_usage']['status'],'unavailable');self.assertEqual(fresh['conversation_id'],self.conversation_id)
  self.assertEqual(sum(e['type']=='SessionCleared' for e in self.inspect()['events']),1)
 def test_native_hook_records_clear_and_counters_belong_to_the_new_conversation(self):
  self.record.update(transcript=self.usage_history(self.conversation_id,109000,100000));self.write_record()
  new=str(uuid.uuid4());event=dict(hook_event_name='SessionStart',session_id=new,source='clear',transcript_path=self.usage_history(new,25,5))
  self.hook(event);self.hook(event)
  fresh=self.inspect();self.assertEqual(sum(e['type']=='SessionCleared' for e in fresh['events']),1)
  self.assertEqual(fresh['session_usage']['context']['used'],25);self.assertEqual(fresh['session_usage']['prompt_cache']['cache_read'],5)
 def test_activity_keeps_the_earlier_conversation_after_a_clear(self):
  old=self.conversation_id;new=str(uuid.uuid4())
  self.hook(dict(hook_event_name='UserPromptSubmit',session_id=old,prompt='Earlier request'))
  self.hook(dict(hook_event_name='Stop',session_id=old,last_assistant_message='Earlier answer'))
  cursor=self.inspect()['cursor']
  self.hook(dict(hook_event_name='SessionStart',session_id=new,source='clear',transcript_path=self.usage_history(new,25,5)))
  self.hook(dict(hook_event_name='UserPromptSubmit',session_id=new,prompt='Fresh request'))
  fresh=self.inspect();self.assertEqual(fresh['conversation_id'],new);self.assertEqual(fresh['cleared_conversations'],[old])
  timeline=[e['detail'] or e['type'] for e in fresh['events'] if e['type'] in ('UserPromptSubmit','Stop','SessionCleared')]
  self.assertEqual(timeline,['Earlier request','Earlier answer','SessionCleared','Fresh request'])
  self.assertEqual([e['detail'] or e['type'] for e in fresh['message_events']],timeline)
  # Sequence numbers are global: a reader's cursor continues across the clear.
  later=self.inspect('--after',str(cursor))
  self.assertEqual([e['detail'] or e['type'] for e in later['events'] if e['type']!='SessionStart'],['SessionCleared','Fresh request'])
  # Another conversation starts its own timeline.
  other=str(uuid.uuid4());self.hook(dict(hook_event_name='SessionStart',session_id=other,source='resume',transcript_path=self.usage_history(other,25,5)))
  resumed=self.inspect();self.assertEqual(resumed['cleared_conversations'],[])
  self.assertFalse(any(e['detail'] in ('Earlier request','Fresh request') for e in resumed['events']))
 def test_unconfirmed_clear_keeps_reported_usage_and_has_no_success_notice(self):
  self.record.update(transcript=self.usage_history(self.conversation_id,109000,100000),
                     clear_context_request=dict(request_id=str(uuid.uuid4()),conversation_id=self.conversation_id));self.write_record()
  fresh=self.inspect();self.assertFalse(fresh.get('session_clear'));self.assertEqual(fresh['session_usage']['context']['used'],109000)
  self.assertFalse(any(e['type']=='SessionCleared' for e in fresh['events']))
 def test_reset_observation_rejects_stale_identity_foreign_directory_and_native_draft(self):
  self.record.update(cwd='/fixture',last_message='Previous answer',transcript=self.usage_history(self.conversation_id,109000,100000))
  original=self.record.copy()
  panel='\x1b[2J\x1b[2;1H >_ OpenAI Codex (v0.162.0)\x1b[3;1H /fixture\x1b[4;1H permissions: default\x1b[27;1H\x1b[48;5;234m› '+' '*80+'\x1b[28;1H   \x1b[49m\x1b[29;1H  GPT-6-Astra high · /fixture\x1b[30;1H  ? for shortcuts\x1b[27;3H'
  cases=[(dict(process_start='stale'),panel,'2:26'),(dict(cwd='/other'),panel,'2:26'),
         (dict(activity='busy',phase='working'),panel,'2:26'),({},panel.replace('› '+' '*80,'› unsent native draft'+' '*61).replace('\x1b[27;3H','\x1b[27;22H'),'21:26')]
  for patch,screen,cursor in cases:
   with self.subTest(patch=patch,cursor=cursor):
    self.record=dict(original,**patch);self.write_record();test_input.InputTransport.screen(self,screen,cursor)
    fresh=self.inspect();self.assertFalse(fresh.get('session_clear'))
    self.assertEqual(fresh['session_usage']['context']['used'],109000)
 def test_exact_native_command_and_confirmed_identity(self):
  for agent in ['codex','claude','kimi']:
   with self.subTest(agent=agent):
    import signal
    self.record['agent']=agent;self.write_record();(self.root/'provider').write_text(agent);os=__import__('os');os.kill(self.record['pid'],signal.SIGUSR1);time.sleep(.12)
    before=self.received();self.assertEqual(self.clear()['status'],'confirmed');self.assertEqual(self.received()[len(before):],b'/clear\r')
 def test_stale_busy_draft_and_reused_request_do_not_submit(self):
  p=dict(request_id=str(uuid.uuid4()),expected_run_id='old',expected_conversation_id=self.conversation_id)
  self.clear(p,False);self.assertEqual(self.received(),b'')
  self.record['activity']='busy';self.record['phase']='working';self.write_record();self.clear(success=False);self.assertEqual(self.received(),b'')
  self.record['activity']='idle';self.record['phase']='idle';self.write_record()
  test_input.InputTransport.screen(self,'\x1b[2J\x1b[H› unsent draft', '14:0');self.clear(success=False);self.assertEqual(self.received(),b'')
 def test_uncertain_input_does_not_press_enter_or_retry(self):
  (self.root/'freeze').touch();p=dict(request_id=str(uuid.uuid4()),expected_run_id=self.run_id,expected_conversation_id=self.conversation_id)
  self.assertIn('uncertain',self.clear(p,False));self.assertEqual(self.received(),b'/clear')
  self.clear(p,False);self.assertEqual(self.received(),b'/clear')
if __name__=='__main__':unittest.main()
