#!/usr/bin/env python3
"""Real Kimi TUI against a localhost-only API fixture, never a user's account.
Run with HGS_KIMI_TEST_BIN=/absolute/path/to/a/pinned/kimi and its sha256 in
HGS_KIMI_TEST_SHA256 (otherwise skipped). Kimi installs updates over the
executable it runs from, so the fixture runs a private copy with automatic
updates off and never the user's installed Kimi. HGS_KIMI_GUARD lists
executables, such as that installed Kimi, that must not change during a run;
HGS_KIMI_EVIDENCE_DIR keeps each run's screens, logs and metadata.
"""
import hashlib,json,os,shlex,shutil,subprocess,tempfile,threading,time,unittest,uuid
from http.server import BaseHTTPRequestHandler,ThreadingHTTPServer
from pathlib import Path
import test_input as fixtures
import test_questions as questions

KIMI=os.environ.get('HGS_KIMI_TEST_BIN')
KIMI_SHA256=os.environ.get('HGS_KIMI_TEST_SHA256','')
GUARDED=[Path(path) for path in os.environ.get('HGS_KIMI_GUARD','').split(os.pathsep) if path]
EVIDENCE=os.environ.get('HGS_KIMI_EVIDENCE_DIR')
def sha256(path):return hashlib.sha256(Path(path).read_bytes()).hexdigest()
LONG=('Любой провайдер из списка (Z.AI, OpenRouter, DeepSeek и другие): ввести, заменить '
      'или удалить ключ на выбранной машине. Дополнительные поля провайдер описывает сам.')
# One question taller than the 24 rows of a pane no client has attached to.
TALL=[dict(question='Which release should we prepare?',header='Release',
           options=[dict(label=f'Option {i}',description=LONG) for i in range(1,5)])]
SHORT=[dict(question='Which release should we prepare?',header='Release',
            options=[dict(label='Stable',description='Use stable build'),dict(label='Preview',description='Use preview build')])]
# Trust choices of Kimi 2.1 and of Kimi 2.2.
TRUST_LABELS=(['Trust this folder',"Don't trust"],['Trust and continue','Exit'])

def gated_project(work):
 """Project MCP servers and instructions that trust would activate. The
 servers' command does nothing, so trusting starts nothing real."""
 servers={f'tracker{i}':dict(command='/bin/true',args=[f'example-mcp-{i}']) for i in range(3)}
 (work/'.mcp.json').write_text(json.dumps(dict(mcpServers=servers)))
 (work/'.kimi-code').mkdir()
 (work/'.kimi-code/mcp.json').write_text(json.dumps(dict(mcpServers={'local':dict(command='/bin/true',args=['server.js'])})))
 (work/'AGENTS.md').write_text('# Example instructions\n')

@unittest.skipUnless(KIMI,'Set HGS_KIMI_TEST_BIN to test a pinned native Kimi against a local fake API')
class NativeKimi(unittest.TestCase):
 """A sealed private copy of the pinned Kimi in a private home and tmux socket."""
 script=fixtures.InputTransport.script
 tmux=fixtures.InputTransport.tmux
 write_record=fixtures.InputTransport.write_record
 inspect=questions.Questions.inspect
 answer=questions.Questions.answer
 items=SHORT
 def wait_for(self,callback,seconds=30):
  deadline=time.monotonic()+seconds
  while time.monotonic()<deadline:
   result=callback()
   if result:return result
   time.sleep(.1)
  self.fail('Native fixture timed out:\n'+self.screen())
 def screen(self):return self.tmux('capture-pane','-p','-t',self.pane,check=False)
 def height(self):return int(self.tmux('display-message','-p','-t',self.pane,'#{window_height}'))
 def launch(self,name,rows=24,project=lambda work:None):
  self.temp=tempfile.TemporaryDirectory(prefix='hgs-kimi-native-');self.addCleanup(self.temp.cleanup)
  self.root=Path(self.temp.name).resolve();self.bin=self.root/'.local/bin';self.bin.mkdir(parents=True)
  self.state=self.root/'state';self.state.mkdir();self.work=work=self.root/'work';work.mkdir();project(work)
  self.kimi_home=self.root/'.kimi-code';self.kimi_home.mkdir()
  # Kimi stages updates beside its executable and swaps them in at the next
  # start: run a private copy of exactly the pinned build, keep both update
  # switches off, and prove afterwards that nothing outside changed.
  self.assertRegex(KIMI_SHA256,'^[0-9a-f]{64}$','HGS_KIMI_TEST_SHA256 must pin the executable')
  self.assertEqual(sha256(KIMI),KIMI_SHA256)
  guarded={str(path):sha256(path) for path in GUARDED}
  self.addCleanup(lambda:self.assertEqual({str(path):sha256(path) for path in GUARDED},guarded,'a guarded executable changed'))
  self.kimi=kimi=self.root/'kimi-bin/kimi';kimi.parent.mkdir();shutil.copy2(KIMI,kimi)
  self.assertEqual(sha256(kimi),KIMI_SHA256)
  (self.kimi_home/'tui.toml').write_text('[upgrade]\nauto_install = false\n')
  fixture=self;self.native_requests=[]
  class Server(BaseHTTPRequestHandler):
   def log_message(self,*args):pass
   def do_POST(self):
    body=json.loads(self.rfile.read(int(self.headers.get('Content-Length',0))) or b'{}')
    fixture.native_requests.append(body)
    tools={t.get('function',{}).get('name') for t in body.get('tools',[])}
    answered=any(m.get('role')=='tool' for m in body.get('messages',[]))
    self.send_response(200);self.send_header('Content-Type','text/event-stream');self.end_headers()
    def chunk(delta,finish=None):
     data=dict(id='chatcmpl-fixture',object='chat.completion.chunk',created=int(time.time()),model='m',
               choices=[dict(index=0,delta=delta,finish_reason=finish)])
     self.wfile.write(('data: '+json.dumps(data)+'\n\n').encode());self.wfile.flush()
    if 'AskUserQuestion' in tools and not answered:
     call=dict(index=0,id='call_fixture',type='function',function=dict(name='AskUserQuestion',arguments=json.dumps(dict(questions=fixture.items))))
     chunk(dict(role='assistant',content=None,tool_calls=[call]));chunk({},'tool_calls')
    else:chunk(dict(role='assistant',content='Fixture complete.'));chunk({},'stop')
    self.wfile.write(b'data: [DONE]\n\n');self.wfile.flush()
  server=ThreadingHTTPServer(('127.0.0.1',0),Server);self.addCleanup(server.server_close);self.addCleanup(server.shutdown)
  threading.Thread(target=server.serve_forever,daemon=True).start()
  (self.kimi_home/'config.toml').write_text(f'''default_model = "fixture/m"
default_permission_mode = "default"
[providers.fixture]
type = "openai"
base_url = "http://127.0.0.1:{server.server_port}/v1"
api_key = "fixture-key"
[models."fixture/m"]
provider = "fixture"
model = "m"
max_context_size = 128000
capabilities = []
display_name = "Fixture"
''')
  self.socket=self.root/'socket'
  self.script('tmux','#!/bin/sh\nexec '+shlex.quote(fixtures.TMUX)+' -S '+shlex.quote(str(self.socket))+' -f /dev/null "$@"\n')
  self.env=dict(os.environ,HOME=str(self.root),HGS_STATE_DIR=str(self.state),HGS_CONFIG_DIR=str(self.root/'config'),PATH=str(self.bin)+':'+os.environ['PATH'],HGS_SELF='test')
  for v in ('TMUX','TMUX_PANE','HGS_RUN_ID','HGS_SESSION','HGS_EXECUTABLE'):self.env.pop(v,None)
  self.name=name;self.run_id=str(uuid.uuid4())
  self.addCleanup(lambda:fixtures.InputTransport.tearDown(self))
  # tmux's default size for a session no client has attached to is 80x24.
  self.tmux('new-session','-d','-s',self.name,'-x','80','-y',str(rows),'-c',str(work),'env','-i','HOME='+str(self.root),
            'PATH='+os.environ['PATH'],'TERM=xterm-256color','LANG=C.UTF-8','KIMI_CODE_HOME='+str(self.kimi_home),
            'KIMI_DISABLE_TELEMETRY=1','KIMI_CODE_NO_AUTO_UPDATE=1',str(kimi))
  self.pane=self.tmux('display-message','-p','-t','='+self.name+':','#{pane_id}').strip()
  self.addCleanup(self.retain)
  self.pid=pid=int(self.tmux('display-message','-p','-t',self.pane,'#{pane_pid}'))
  # The pane runs the private copy, not any other Kimi.
  if Path('/proc').is_dir():self.wait_for(lambda:os.readlink(f'/proc/{pid}/exe')==str(kimi),10)
  self.start=subprocess.check_output(['ps','-p',str(pid),'-o','lstart='],env=dict(self.env,LC_ALL='C',TZ='UTC'),text=True).strip()
  self.tmux('set-option','-t','='+self.name+':','@hgs_run',self.run_id)
  self.record_path=self.state/(hashlib.sha256(self.name.encode()).hexdigest()+'.json')
 def retain(self):
  """Keep this run's screens, logs and metadata before its home is removed."""
  if not EVIDENCE:return
  out=Path(EVIDENCE)/self.id().rsplit('.',1)[-1];out.mkdir(parents=True,exist_ok=True)
  (out/'screen.txt').write_text(self.screen())
  (out/'screen-styled.txt').write_text(self.tmux('capture-pane','-p','-e','-t',self.pane,check=False))
  for source in [self.kimi_home/'logs',*self.kimi_home.glob('sessions/*/*/agents/main')]:
   if source.is_dir():shutil.copytree(source,out/source.relative_to(self.kimi_home),dirs_exist_ok=True)
  if self.record_path.exists():shutil.copy2(self.record_path,out/'record.json')
  (out/'metadata.json').write_text(json.dumps(dict(pinned=KIMI,pinned_sha256=KIMI_SHA256,copy=str(self.kimi),copy_sha256=sha256(self.kimi),
   guarded={str(path):sha256(path) for path in GUARDED},height=self.tmux('display-message','-p','-t',self.pane,'#{window_height}',check=False).strip(),
   requests=len(self.native_requests)),indent=1))
 def attach_client(self):
  """A control-mode client: tmux counts the session as viewed."""
  client=subprocess.Popen([fixtures.TMUX,'-S',str(self.socket),'-f','/dev/null','-C','attach','-t','='+self.name],
                          stdin=subprocess.PIPE,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,env=self.env)
  self.addCleanup(lambda:(client.stdin.close(),client.wait(5)))
  self.wait_for(lambda:self.tmux('display-message','-p','-t',self.pane,'#{session_attached}').strip()=='1')
 def cards(self):return self.inspect()['pending_questions']
 def card(self):
  cards=self.cards()
  self.assertEqual(len(cards),1,cards)
  return cards[0]

class NativeKimiQuestions(NativeKimi):
 def setUp(self):
  self.items=SHORT if '_short_' in self._testMethodName else TALL
  self.launch('kimi/question-native')
  def started():
   screen=self.screen()
   return screen if 'Trust this folder?' in screen or '│ >' in screen else None
  if 'Trust this folder?' in self.wait_for(started):self.tmux('send-keys','-t',self.pane,'Enter')
  self.wait_for(lambda:'│ >' in self.screen())
  self.tmux('send-keys','-t',self.pane,'-l','Ask me with the question tool.');time.sleep(.3)
  self.tmux('send-keys','-t',self.pane,'Enter')
  def asked():
   for wire in self.kimi_home.glob('sessions/*/*/agents/main/wire.jsonl'):
    if '"interaction.request"' in wire.read_text() and 'esc cancel' in self.screen():return wire
  wire=self.wait_for(asked);self.conversation_id=wire.parent.parent.parent.name
  self.record=dict(version=1,name=self.name,agent='kimi',run_id=self.run_id,conversation_id=self.conversation_id,
                   agent_home=str(self.kimi_home),pane=self.pane,pid=self.pid,process_start=self.start,activity='busy',phase='input',
                   active_tools={'call_fixture':{'name':'AskUserQuestion'}},subagents={},last_event_at=time.time(),created=time.time())
  self.write_record();self.wire=wire
 def payload(self,card,option):
  return dict(request_id=str(uuid.uuid4()),expected_run_id=self.run_id,expected_conversation_id=self.conversation_id,
              question_id=card['question_id'],expected_question_hash=card['question_hash'],
              answers=[dict(question_id='q_0',selected_option_ids=[option],text='')])
 def resolved(self):
  rows=[json.loads(line) for line in self.wire.read_text().splitlines() if '"interaction.resolved"' in line]
  return rows[-1]['response'] if rows else None

 def test_tall_question_in_an_unviewed_80x24_pane_grows_until_answerable(self):
  self.assertEqual(self.height(),24)
  self.assertNotIn(' question\n',self.screen())
  card=self.card()
  self.assertEqual(card['source'],'kimi_wire')
  self.assertTrue(card['can_answer'],card['answer_unavailable_reason'])
  self.assertEqual(card['answer_transport'],'kimi_tui')
  # The window grew for the dialog and still follows the next client.
  self.assertEqual(self.height(),48)
  self.assertEqual(self.tmux('show-options','-w','-v','-t',self.pane,'window-size').strip(),'')
  receipt=self.answer(self.payload(card,'opt_0_1'),timeout=60)
  self.assertEqual(receipt['status'],'answered')
  self.assertEqual(self.resolved()['answers'],{'Which release should we prepare?':'Option 2'})
  self.wait_for(lambda:'Fixture complete.' in self.screen())

 def test_tall_question_with_an_attached_client_is_never_resized(self):
  self.attach_client()
  before=self.height()
  card=self.card()
  self.assertFalse(card['can_answer'])
  self.assertEqual(card['answer_unavailable_reason'],'the question dialog is clipped; enlarge Terminal')
  self.assertEqual(self.height(),before)
  self.assertIsNone(self.resolved())

 def test_short_question_answers_without_growth(self):
  card=self.card()
  self.assertTrue(card['can_answer'],card['answer_unavailable_reason'])
  self.assertEqual(self.height(),24)
  receipt=self.answer(self.payload(card,'opt_0_0'),timeout=60)
  self.assertEqual(receipt['status'],'answered')
  self.assertEqual(self.resolved()['answers'],{'Which release should we prepare?':'Stable'})

class NativeKimiTrust(NativeKimi):
 """The startup folder trust prompt: shown whole, answered only as chosen."""
 def setUp(self):
  # A pane a client left at 12 rows: too short for the whole prompt.
  self.launch('kimi/trust-native',rows=12,project=gated_project)
  self.wait_for(lambda:any(label in self.screen() for labels in TRUST_LABELS for label in labels))
  self.record=dict(version=1,name=self.name,agent='kimi',run_id=self.run_id,run_identity_version=1,supervisor={},
                   conversation_id=None,launch_dir=str(self.work),agent_home=str(self.kimi_home),pane=self.pane,pid=self.pid,
                   process_start=self.start,phase='unknown',activity='unknown',last_event_at=0,created=time.time())
  self.write_record()
 def payload(self,card,option):
  return dict(request_id=str(uuid.uuid4()),expected_run_id=self.run_id,expected_conversation_id='',
              question_id=card['question_id'],expected_question_hash=card['question_hash'],
              answers=[dict(question_id='trust',selected_option_ids=[option],text='')])
 def trust_card(self):
  card=self.card()
  self.assertEqual(card['source'],'kimi_folder_trust',card)
  item=card['questions'][0]
  labels=[option['label'] for option in item['options']]
  self.assertIn(labels,[list(labels) for labels in TRUST_LABELS])
  # The whole disclosure: Kimi 2.1 lists every gated server, Kimi 2.2 counts
  # them with their configuration files and adds the instructions.
  shown=[f'{name} (stdio): command=/bin/true' for name in ['local','tracker0','tracker1','tracker2']] \
   if labels==TRUST_LABELS[0] else ['Start 4 MCP servers automatically','.kimi-code/mcp.json, .mcp.json','AGENTS.md']
  for text in [str(self.work),*shown]:self.assertIn(text,item['body'])
  self.assertNotIn('page ·',item['body']);self.assertNotIn('…',item['body'])
  return card

 def test_a_short_unviewed_pane_grows_until_the_whole_prompt_shows_then_trusts_once(self):
  self.assertEqual(self.height(),12)
  card=self.trust_card()
  self.assertGreater(self.height(),12)
  self.assertEqual(self.tmux('show-options','-w','-v','-t',self.pane,'window-size').strip(),'')
  receipt=self.answer(self.payload(card,'trust_0'),timeout=60)
  self.assertEqual(receipt['status'],'answered')
  self.wait_for(lambda:'│ >' in self.screen())
  self.assertTrue(Path(f'/proc/{self.pid}').exists() or not Path('/proc').is_dir())

 def test_declining_trust_exits_kimi(self):
  card=self.trust_card()
  receipt=self.answer(self.payload(card,'trust_1'),timeout=60)
  self.assertEqual(receipt['status'],'answered')
  self.wait_for(lambda:subprocess.run(['ps','-p',str(self.pid)],capture_output=True).returncode!=0)

 def test_a_viewed_short_pane_is_never_resized_or_answered(self):
  self.attach_client()
  before=self.height()
  cards=self.cards()
  self.assertFalse([card for card in cards if card['source']=='kimi_folder_trust'],cards)
  self.assertTrue(all(not card.get('can_answer') for card in cards),cards)
  self.assertEqual(self.height(),before)

if __name__=='__main__':unittest.main()
