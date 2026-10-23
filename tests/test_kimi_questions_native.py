#!/usr/bin/env python3
"""Real Kimi TUI against a localhost-only API fixture, never a user's account.
Run with HGS_KIMI_TEST_BIN=/absolute/path/to/a/pinned/kimi and its sha256 in
HGS_KIMI_TEST_SHA256 plus its sealed HGS_KIMI_TEST_RECORD. Kimi installs updates over the
executable it runs from, so the fixture runs a private copy with automatic
updates off and never the user's installed Kimi. HGS_KIMI_GUARD lists
executables, such as that installed Kimi, that must not change during a run;
HGS_KIMI_EVIDENCE_DIR keeps each run's screens, logs and metadata.

Kimi's platform endpoint is the fixture's too, answered as in test_kimi_native,
and the child gets that fixture's private environment. On macOS the pane starts
its exec shim, so the copy's exact environment and image are proven as there;
the shim's parent is this fixture's own private tmux server, not an hgs
supervisor. The root and the copy are removed only after every owned process
ended and the copy was sealed; anything unknown keeps them.
"""
import ast,hashlib,inspect,json,os,shlex,shutil,stat,subprocess,sys,tempfile,textwrap,threading,time,unittest,uuid
import urllib.error,urllib.request
from http.server import ThreadingHTTPServer
from pathlib import Path
from unittest import mock
import test_input as fixtures
import test_kimi_native as guard
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
# test_kimi_native's private child keys and refused prefixes; a control keeps them its own.
KEYS=('HOME','KIMI_CODE_HOME','KIMI_CODE_BASE_URL','KIMI_CODE_NO_AUTO_UPDATE',
      'KIMI_DISABLE_TELEMETRY','XDG_CONFIG_HOME','XDG_DATA_HOME','XDG_CACHE_HOME','XDG_STATE_HOME',
      'USERPROFILE','APPDATA','LOCALAPPDATA','TMPDIR')
FORBIDDEN=('OPENAI_','ANTHROPIC_','CODEX_','CLAUDE','DEEPSEEK_','OPENCODE_',
           'AWS_','AZURE_','GOOGLE_','GCP_','GITHUB_','GH_','NPM_','npm_','SSH_','SOPS_','AGE_')

def gated_project(work):
 """Project MCP servers and instructions that trust would activate. The
 servers' command does nothing, so trusting starts nothing real."""
 servers={f'tracker{i}':dict(command='/bin/true',args=[f'example-mcp-{i}']) for i in range(3)}
 (work/'.mcp.json').write_text(json.dumps(dict(mcpServers=servers)))
 (work/'.kimi-code').mkdir()
 (work/'.kimi-code/mcp.json').write_text(json.dumps(dict(mcpServers={'local':dict(command='/bin/true',args=['server.js'])})))
 (work/'AGENTS.md').write_text('# Example instructions\n')

def child_environment(root,home,platform):
 """test_kimi_native's private allowlist for the Kimi child: only PATH comes from here."""
 return guard.private_environment(dict(PATH=os.environ['PATH']),root,home,platform)

def question_composer(screen,text=''):
 """The complete short draft in Kimi's one boxed composer, with no extra line."""
 if 'Trust this folder?' in screen or 'esc cancel' in screen:return False
 rows=[row.strip() for row in screen.splitlines()]
 prompts=[i for i,row in enumerate(rows) if row.startswith('│ >')]
 if len(prompts)!=1:return False
 i=prompts[0];row=rows[i]
 if not row.endswith('│') or row[1:-1].strip()!=('> '+text).rstrip():return False
 if i==0 or not rows[i-1].startswith('╭') or not rows[i-1].endswith('╮'):return False
 for row in rows[i+1:]:
  if row.startswith('╰') and row.endswith('╯'):return True
  if not row.startswith('│') or not row.endswith('│') or row[1:-1].strip():return False
 return False

def question_start_ready(screen,completed,tmux_present=False):
 """A written platform banner response and an empty canonical composer.
 The env-i direct pane has no TMUX, so Kimi never asks for its tmux warning.
 """
 return (any(row.get('name')=='client_banner' for row in completed) and
         (not tmux_present or guard.TMUX_KEYS in screen) and question_composer(screen))

class Model(guard.Model):
 """Kimi's platform gets test_kimi_native's written 404; the model asks one
 AskUserQuestion until a tool result arrives, then completes the turn."""
 def do_GET(self):
  if self.path.startswith(guard.PLATFORM+'/'):return self.platform({})
  self.send_error(501,"Unsupported method ('GET')")
 def do_POST(self):
  body=json.loads(self.rfile.read(int(self.headers.get('Content-Length',0))) or b'{}')
  if self.path.startswith(guard.PLATFORM+'/'):return self.platform(body)
  fixture=self.server.fixture;fixture.native_requests.append(body)
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

def serve(fixture):
 """The loopback server: platform requests are recorded apart from the model's."""
 server=ThreadingHTTPServer(('127.0.0.1',0),Model)
 server.fixture,server.platform,server.platform_completed=fixture,[],[]
 threading.Thread(target=server.serve_forever,daemon=True).start()
 return server

def processes(env):
 """pid -> (parent, status, start) of every process, starts in UTC as hgs records them."""
 result=subprocess.run(['ps','-axo','pid=,ppid=,stat=,lstart='],env=dict(env,LC_ALL='C',TZ='UTC'),
                       capture_output=True,text=True,check=True,timeout=10)
 table={}
 for line in result.stdout.splitlines():
  fields=line.split(None,3)
  if len(fields)==4:table[int(fields[0])]=(int(fields[1]),fields[2],' '.join(fields[3].split()))
 return table

@unittest.skipUnless(KIMI,'Set HGS_KIMI_TEST_BIN to test a pinned native Kimi against a local fake API')
class NativeKimi(unittest.TestCase):
 """A sealed private copy of the pinned Kimi in a private home and tmux socket."""
 script=fixtures.InputTransport.script
 tmux=fixtures.InputTransport.tmux
 write_record=fixtures.InputTransport.write_record
 inspect=questions.Questions.inspect
 answer=questions.Questions.answer
 # test_kimi_native's unchanged proof: /proc on Linux, the shim capture and the kernel on macOS.
 environment=guard.NativeKimi.environment
 launched_environment=guard.NativeKimi.launched_environment
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
  self.root=Path(tempfile.mkdtemp(prefix='hgs-kimi-native-')).resolve()
  # The copy lives outside the root it may outlast (test_kimi_native's custody).
  self.images=Path(tempfile.mkdtemp(prefix='hgs-kimi-image-')).resolve()
  self.env=self.pane=self.server=self.kimi=self.record_path=self.seen_height=None;self.owned={};self.guarded={};self.clients=[]
  self.native_requests=[];self.addCleanup(self.custody)
  self.bin=self.root/'.local/bin';self.bin.mkdir(parents=True)
  self.state=self.root/'state';self.state.mkdir();self.work=work=self.root/'work';work.mkdir();project(work)
  self.kimi_home=self.root/'.kimi-code';self.kimi_home.mkdir()
  self.stage()
  (self.kimi_home/'tui.toml').write_text('[upgrade]\nauto_install = false\n')
  self.server=server=serve(self)
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
  self.child_env=child_environment(self.root,self.kimi_home,'http://127.0.0.1:%d%s'%(server.server_port,guard.PLATFORM))
  launcher=self.launcher()
  self.socket=self.root/'socket'
  self.script('tmux','#!/bin/sh\nexec '+shlex.quote(fixtures.TMUX)+' -S '+shlex.quote(str(self.socket))+' -f /dev/null "$@"\n')
  self.env=dict(os.environ,HOME=str(self.root),HGS_STATE_DIR=str(self.state),HGS_CONFIG_DIR=str(self.root/'config'),PATH=str(self.bin)+':'+os.environ['PATH'],HGS_SELF='test')
  for v in ('TMUX','TMUX_PANE','HGS_RUN_ID','HGS_SESSION','HGS_EXECUTABLE'):self.env.pop(v,None)
  self.name=name;self.run_id=str(uuid.uuid4())
  # tmux's default size for a session no client has attached to is 80x24.
  self.tmux('new-session','-d','-s',self.name,'-x','80','-y',str(rows),'-c',str(work),'env','-i',
            *[f'{key}={value}' for key,value in sorted(self.child_env.items())],str(launcher))
  self.pane=self.tmux('display-message','-p','-t','='+self.name+':','#{pane_id}').strip()
  self.pid=pid=int(self.tmux('display-message','-p','-t',self.pane,'#{pane_pid}'))
  self.tmux_pid=int(self.tmux('display-message','-p','-t',self.pane,'#{pid}'))
  self.start=subprocess.check_output(['ps','-p',str(pid),'-o','lstart='],env=dict(self.env,LC_ALL='C',TZ='UTC'),text=True).strip()
  table=processes(self.env)
  self.owned={process:table[process][2] for process in (pid,self.tmux_pid) if process in table}
  print('ZERUS_KIMI_FIXTURE='+json.dumps(dict(case=self.id(),root=str(self.root),images=str(self.images),socket=str(self.socket),
        pane_pid=pid,pane_start=self.start,tmux_pid=self.tmux_pid,owned=self.owned)),flush=True)
  # The pane runs the private copy, with exactly the private environment.
  self.prove_launch()
  self.tmux('set-option','-t','='+self.name+':','@hgs_run',self.run_id)
  self.record_path=self.state/(hashlib.sha256(self.name.encode()).hexdigest()+'.json')
 def stage(self):
  """Kimi stages updates beside its executable and swaps them in at the next
  start: run a private copy of exactly the pinned build, keep both update
  switches off, and prove afterwards that nothing outside changed."""
  self.assertRegex(KIMI_SHA256,'^[0-9a-f]{64}$','HGS_KIMI_TEST_SHA256 must pin the executable')
  self.assertEqual(sha256(KIMI),KIMI_SHA256)
  self.version=guard.admitted_version(KIMI,KIMI_SHA256)
  self.guarded={str(path):sha256(path) for path in GUARDED}
  kimi=self.images/'kimi';shutil.copyfile(KIMI,kimi);kimi.chmod(0o755)
  self.assertEqual(sha256(kimi),KIMI_SHA256)
  self.kimi=self.image=kimi.resolve()
 def launcher(self):
  """What the pane starts: on macOS test_kimi_native's compiled exec shim, which
  records its exact argv and envp and becomes the copy; elsewhere the copy."""
  if sys.platform!='darwin':return self.kimi
  self.captures=self.root/'kimi-exec';self.captures.mkdir(mode=0o700)
  source=self.root/'kimi-shim.c';source.write_text(guard.shim_source(self.captures,self.kimi))
  shim=self.root/'kimi-shim'
  build=subprocess.run(['/usr/bin/cc','-std=c11','-O1','-Wall','-Werror','-o',str(shim),str(source)],
                       env=dict(PATH='/usr/bin:/bin',HOME=str(self.root)),capture_output=True,text=True,timeout=120)
  self.assertEqual(build.returncode,0,'the Kimi exec shim did not build: '+build.stderr[-2000:])
  return shim
 def parent_binding(self):
  """This fixture's private tmux server as the parent of the pane it started: a
  guard-only binding for test_kimi_native's oracle, never a product record."""
  if sys.platform!='darwin':return dict(process_start=self.start,supervisor={})
  info=guard.MacProcess.info(self.tmux_pid)
  self.assertEqual(info.uid,os.geteuid(),"the private tmux server is not this user's")
  self.assertEqual(guard.executable(self.tmux_pid),Path(fixtures.TMUX).resolve(),'the private tmux server is not the pinned tmux')
  return dict(process_start=self.start,supervisor=dict(pid=self.tmux_pid,start=guard.lstart_text(info.start_sec)))
 def prove_launch(self):
  """The copy's image and exact exec-time environment: on Linux from /proc, on
  macOS from the shim's capture proven by the kernel's argument area."""
  pid=self.pid
  def running():
   try:return guard.executable(pid)==self.kimi
   except OSError:return False
  self.wait_for(running,10)
  self.assertEqual(sha256(guard.executable(pid)),KIMI_SHA256)
  sample,actual={},None
  try:actual=self.environment(pid,sample,self.parent_binding())
  finally:
   # No environment values enter the retained log.
   print('ZERUS_KIMI_ENVIRONMENT='+json.dumps(dict(case=self.id(),pid=pid,expected_image=str(self.kimi),sample=sample,
         keys=None if actual is None else {key:'missing' if key not in actual else 'match' if actual[key]==self.child_env[key]
                                           else 'different' for key in KEYS},
         forbidden_keys=None if actual is None else [key for key in actual if actual[key] and key.startswith(FORBIDDEN)],
         unexpected_kimi_keys=None if actual is None else [key for key in actual if actual[key] and key.startswith('KIMI_') and key not in KEYS])),flush=True)
  for key in KEYS:self.assertTrue(actual.get(key)==self.child_env[key],'unexpected private child environment key: '+key)
  self.assertFalse([key for key in actual if actual[key] and key.startswith(FORBIDDEN)],'a child inherited credential keys')
  self.assertFalse([key for key in actual if actual[key] and key.startswith('KIMI_') and key not in KEYS],
                   'a child inherited an unexpected Kimi override')
 def alive(self):
  """The pane's Kimi still runs as the process that started."""
  result=subprocess.run(['ps','-p',str(self.pid),'-o','lstart='],env=dict(self.env,LC_ALL='C',TZ='UTC'),capture_output=True,text=True)
  return result.returncode==0 and ' '.join(result.stdout.split())==' '.join(self.start.split())
 def custody(self):
  """Each step is attempted on its own and every failure kept: screens while the pane
  runs, the attached clients, every owned process, the model server, the evidence and
  the seals taken after the owned shutdown. Only when all of them completed are the root
  and then the copy removed; otherwise both stay. A removal that fails part-way is
  reported as it is, never as a kept root."""
  errors=[]
  def step(name,action):
   try:action();return True
   except Exception as error:errors.append(f'{name}: {error}');return False
  step('screens',self.retain_screens)
  clients_done=step('attached clients',self.stop_clients)
  quiet=step('owned shutdown',self.quiesce)
  server_done=step('model server',self.stop_server)
  step('evidence',self.retain_files)
  step('seals',lambda:self.seal(clients_done and quiet and server_done))
  removed=[]
  if not errors:
   for path in (self.root,self.images):
    try:shutil.rmtree(path)
    except Exception as error:
     errors.append(f'removal of {path} failed ({error}); it '+('still exists, possibly in part' if path.exists() else 'no longer exists'))
     break
    removed.append(str(path))
  if errors:raise AssertionError('; '.join(errors)+f'; removed {removed}; kept '+str([str(path) for path in (self.root,self.images) if path.exists()]))
 def stop_clients(self):
  """Direct children this case attached: attempt every independent close/stop/wait
  and every later client. Any failed or unknown cleanup retains both roots."""
  errors=[];remaining=[]
  for client in self.clients:
   def failed(stage,error):errors.append(f'client {client.pid} {stage}: {type(error).__name__}: {error}')
   try:running=client.poll() is None
   except Exception as error:failed('initial poll',error);running=True
   try:client.stdin.close()
   except Exception as error:failed('stdin.close',error)
   if running:
    for name,stop in (('close',None),('terminate',client.terminate),('kill',client.kill)):
     if stop:
      try:stop()
      except Exception as error:failed(name,error)
     try:client.wait(5);break
     except subprocess.TimeoutExpired:pass
     except Exception as error:failed(name+' wait',error)
   try:running=client.poll() is None
   except Exception as error:failed('final poll',error);running=True
   if running:remaining.append(client.pid)
  if remaining:errors.append(f'attached clients did not exit: {remaining}')
  if errors:raise AssertionError('; '.join(errors))
 def stop_server(self):
  if self.server:self.server.shutdown();self.server.server_close()
 def quiesce(self):
  """Every process this case started ends: its private tmux server, the panes and
  the Kimi it recorded, and their descendants, each bound to its start so that a
  reused PID is never waited for or counted."""
  if self.env is None:return
  table=processes(self.env)
  owned={pid:start for pid,start in self.owned.items() if pid in table and table[pid][2]==start}
  for pid in self.tmux('list-panes','-a','-F','#{pane_pid}',check=False).split():
   if pid.isdigit() and int(pid) in table:owned.setdefault(int(pid),table[int(pid)][2])
  def live(table):
   while children:={pid:data[2] for pid,data in table.items() if pid not in owned and data[0] in owned and
                    data[0] in table and table[data[0]][2]==owned[data[0]]}:owned.update(children)
   return {pid:table[pid][1] for pid,start in owned.items()
           if pid in table and table[pid][2]==start and not table[pid][1].startswith(('Z','X'))}
  live(table)
  self.tmux('kill-server',check=False)
  deadline=time.monotonic()+10
  while remaining:=live(processes(self.env)):
   if time.monotonic()>=deadline:raise AssertionError(f'owned processes survived their private tmux server: {remaining}')
   time.sleep(.05)
 def evidence(self):
  out=Path(EVIDENCE)/self.id().rsplit('.',1)[-1];out.mkdir(parents=True,exist_ok=True)
  return out
 def retain_screens(self):
  """The pane's last screens and size, while it still runs."""
  if not EVIDENCE or not self.pane:return
  out=self.evidence()
  (out/'screen.txt').write_text(self.screen())
  (out/'screen-styled.txt').write_text(self.tmux('capture-pane','-p','-e','-t',self.pane,check=False))
  self.seen_height=self.tmux('display-message','-p','-t',self.pane,'#{window_height}',check=False).strip()
 def retain_files(self):
  """Kimi's logs and sessions and the hgs record, once nothing writes them."""
  if not EVIDENCE:return
  out=self.evidence()
  for source in [self.kimi_home/'logs',*self.kimi_home.glob('sessions/*/*/agents/main')]:
   if source.is_dir():shutil.copytree(source,out/source.relative_to(self.kimi_home),dirs_exist_ok=True)
  if self.record_path and self.record_path.exists():shutil.copy2(self.record_path,out/'record.json')
 def seal(self,after_shutdown):
  """The pinned file, its copy and every guard keep their bytes; a seal taken while an
  owned process may still run is recorded but never counts."""
  sealed=dict(pinned=KIMI,pinned_sha256=sha256(KIMI),copy=str(self.kimi),copy_sha256=sha256(self.kimi) if self.kimi else None,
              guarded={str(path):sha256(path) for path in GUARDED},height=self.seen_height,requests=len(self.native_requests),
              platform=list(self.server.platform) if self.server else None,
              platform_completed=list(self.server.platform_completed) if self.server else None,after_owned_shutdown=after_shutdown)
  if EVIDENCE:(self.evidence()/'metadata.json').write_text(json.dumps(sealed,indent=1))
  self.assertTrue(after_shutdown,'the seals were not taken after every owned process ended')
  self.assertEqual(sealed['pinned_sha256'],KIMI_SHA256,'the pinned Kimi changed')
  if self.kimi:self.assertEqual(sealed['copy_sha256'],KIMI_SHA256,'the fixture changed its own Kimi copy')
  self.assertEqual(sealed['guarded'],self.guarded,'a guarded executable changed')
 def attach_client(self):
  """A control-mode client: tmux counts the session as viewed."""
  client=subprocess.Popen([fixtures.TMUX,'-S',str(self.socket),'-f','/dev/null','-C','attach','-t','='+self.name],
                          stdin=subprocess.PIPE,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,env=self.env)
  # Stopped and reaped by custody, before the private server goes.
  self.clients.append(client)
  self.wait_for(lambda:self.tmux('display-message','-p','-t',self.pane,'#{session_attached}').strip()=='1')
 def cards(self):return self.inspect()['pending_questions']
 def card(self):
  cards=self.cards()
  self.assertEqual(len(cards),1,cards)
  return cards[0]

class NativeKimiQuestions(NativeKimi):
 def startup_snapshot(self,stage):
  if not EVIDENCE:return
  snapshot=dict(stage=stage,monotonic=time.monotonic(),screen=self.screen(),
                styled=self.tmux('capture-pane','-p','-e','-t',self.pane,check=False),
                cursor=self.tmux('display-message','-p','-t',self.pane,'#{cursor_x}:#{cursor_y}',check=False).strip(),
                platform_completed=list(self.server.platform_completed),model_requests=len(self.native_requests))
  (self.evidence()/('startup-'+stage+'.json')).write_text(json.dumps(snapshot,indent=1))
 def first_prompt(self):
  self.assertFalse(getattr(self,'initial_prompt_attempted',False),'the initial prompt must never be retried')
  self.initial_prompt_attempted=True
  # One existing startup allowance, shared by readiness and the draft proof.
  end=time.monotonic()+30
  try:
   self.wait_for(lambda:question_start_ready(self.screen(),self.server.platform_completed,
                                           bool(self.child_env.get('TMUX'))),max(0,end-time.monotonic()))
   self.startup_snapshot('before-prompt')
   text='Ask me with the question tool.';buffer='native-question-prompt'
   self.assertLess(time.monotonic(),end,'initial prompt readiness expired; no text was staged')
   loaded=subprocess.run([str(self.bin/'tmux'),'load-buffer','-b',buffer,'-'],env=self.env,
                         input=text,text=True,capture_output=True,timeout=5)
   self.assertEqual(loaded.returncode,0,loaded.stderr)
   self.assertLess(time.monotonic(),end,'initial prompt readiness expired; nothing was pasted')
   # Product-equivalent bracketed paste resets Kimi's plain-key paste burst.
   self.tmux('paste-buffer','-p','-r','-d','-b',buffer,'-t',self.pane)
   self.wait_for(lambda:question_composer(self.screen(),text),max(0,end-time.monotonic()))
   self.startup_snapshot('after-paste')
   self.assertLess(time.monotonic(),end,'initial prompt readiness expired; no Enter was sent')
   self.tmux('send-keys','-t',self.pane,'Enter')
  except BaseException:
   self.startup_snapshot('failure')
   raise
 def setUp(self):
  self.items=SHORT if '_short_' in self._testMethodName else TALL
  self.launch('kimi/question-native')
  def started():
   screen=self.screen()
   return screen if 'Trust this folder?' in screen or '│ >' in screen else None
  if 'Trust this folder?' in self.wait_for(started):self.tmux('send-keys','-t',self.pane,'Enter')
  self.first_prompt()
  def asked():
   for wire in self.kimi_home.glob('sessions/*/*/agents/main/wire.jsonl'):
    if '"interaction.request"' in wire.read_text() and 'esc cancel' in self.screen():return wire
  try:wire=self.wait_for(asked)
  except BaseException:
   self.startup_snapshot('failure')
   raise
  self.conversation_id=wire.parent.parent.parent.name
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
  self.assertTrue(self.alive())

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

class QuestionFixtureControls(unittest.TestCase):
 """The fixture's own guards without a native Kimi: its loopback routes, the
 child's environment, launch refusals, the tmux parent binding and custody."""
 def case(self):return NativeKimi('launch')
 def test_first_prompt_waits_for_ready_and_the_exact_bracketed_draft_then_sends_enter_once(self):
  empty=' ╭────────╮\n │ >      │\n │        │\n ╰────────╯'
  draft=empty.replace('>      ','> Ask me with the question tool. ')
  complete=[dict(name='client_banner')]
  self.assertFalse(question_start_ready(empty,[]),'a drawn composer is not a written banner response')
  for refused in ('Trust this folder?\n'+empty,empty.replace('>      ','> unsent '),empty+'\nesc cancel',
                  empty.replace('│        │','│ extra  │'),empty+empty,empty.replace('╰────────╯','')):
   self.assertFalse(question_start_ready(refused,complete),refused)
  self.assertTrue(question_start_ready(empty,complete))
  self.assertFalse(question_start_ready(empty,complete,True))
  self.assertTrue(question_start_ready(guard.TMUX_KEYS+'\n'+empty,complete,True))
  self.assertTrue(question_composer(draft,'Ask me with the question tool.'))
  self.assertFalse(question_composer(draft.replace('tool.','tool. extra'),'Ask me with the question tool.'))
  for fault in (None,'not-ready','load','late-load','draft'):
   case=NativeKimiQuestions('first_prompt');case.bin=Path('/fixture/bin');case.pane='%1';case.env={};case.child_env={}
   case.server=mock.Mock(platform_completed=[]);case.startup_snapshot=mock.Mock()
   state=dict(screen=empty);case.screen=lambda:state['screen'];commands=[];waits=[]
   def tmux(*args):
    commands.append(args)
    if args[0]=='paste-buffer':state['screen']=draft if fault!='draft' else empty
    return ''
   case.tmux=tmux
   def wait(callback,seconds):
    waits.append(seconds)
    if len(waits)==1:
     self.assertFalse(callback());self.assertEqual(commands,[])
     case.server.platform_completed=complete
    if fault=='not-ready' or not callback():case.fail('not ready')
    return True
   case.wait_for=wait
   with self.subTest(fault=fault),mock.patch.object(subprocess,'run',return_value=mock.Mock(
        returncode=1 if fault=='load' else 0,stderr='load refused')) as load,mock.patch.object(time,'monotonic',
        side_effect=lambda:131 if fault=='late-load' and load.called else 100):
    if fault:
     with self.assertRaises(AssertionError):case.first_prompt()
    else:case.first_prompt()
    self.assertEqual(load.call_count,0 if fault=='not-ready' else 1)
    if load.called:
     self.assertEqual(load.call_args.args[0],['/fixture/bin/tmux','load-buffer','-b','native-question-prompt','-'])
     self.assertEqual(load.call_args.kwargs['input'],'Ask me with the question tool.')
    expected=[] if fault in ('not-ready','load','late-load') else [('paste-buffer','-p','-r','-d','-b','native-question-prompt','-t','%1')]
    if fault is None:expected.append(('send-keys','-t','%1','Enter'))
    self.assertEqual(commands,expected)
    self.assertTrue(all(seconds<=30 for seconds in waits))
    stages=[call.args[0] for call in case.startup_snapshot.call_args_list]
    self.assertEqual(stages,(['failure'] if fault=='not-ready' else ['before-prompt','failure'] if fault in ('load','late-load') else
                             ['before-prompt','failure'] if fault=='draft' else ['before-prompt','after-paste']))
    with self.assertRaisesRegex(AssertionError,'never be retried'):case.first_prompt()
    self.assertEqual(commands,expected)
 def test_kimis_platform_gets_a_written_404_and_never_reaches_the_model(self):
  fixture=mock.Mock(native_requests=[],items=SHORT)
  server=serve(fixture);self.addCleanup(server.server_close);self.addCleanup(server.shutdown)
  post_written,release_post,get_completed=threading.Event(),threading.Event(),threading.Event()
  completion_errors=[]
  class HeldPostCompletion(list):
   def append(self,row):
    # The POST response is already written/flushed. Its handler may be
    # descheduled here while the next request finishes on another thread.
    if row['method']=='POST':
     post_written.set()
     if not release_post.wait(5):completion_errors.append('POST completion was not released')
    super().append(row)
    if row['method']=='GET':get_completed.set()
  server.platform_completed=HeldPostCompletion()
  self.addCleanup(release_post.set)
  opener=urllib.request.build_opener(urllib.request.ProxyHandler({}))
  def call(method,path,body=None):
   request=urllib.request.Request('http://127.0.0.1:%d%s'%(server.server_port,path),method=method,
                                  data=None if body is None else json.dumps(body).encode())
   try:
    with opener.open(request,timeout=10) as response:return response.status,response.read()
   except urllib.error.HTTPError as error:
    with error:return error.code,error.read()
  self.assertEqual(call('POST',guard.PLATFORM+'/resource_configs',dict(name='client_banner'))[0],404)
  self.assertTrue(post_written.wait(3),'The POST response was not written')
  self.assertEqual(call('GET',guard.PLATFORM+'/client_config')[0],404)
  self.assertTrue(get_completed.wait(3),'The GET response was not completed')
  self.assertEqual(server.platform_completed,[dict(method='GET',path='/client_config',name=None)])
  release_post.set()
  self.assertEqual(call('GET','/v1/models')[0],501)
  status,stream=call('POST','/v1/chat/completions',dict(tools=[dict(function=dict(name='AskUserQuestion'))],messages=[]))
  self.assertEqual(status,200);self.assertIn(b'"AskUserQuestion"',stream);self.assertTrue(stream.endswith(b'data: [DONE]\n\n'))
  platform=[dict(method='POST',path='/resource_configs',name='client_banner'),dict(method='GET',path='/client_config',name=None)]
  self.assertEqual(server.platform,platform)
  deadline=time.monotonic()+5
  while len(server.platform_completed)<len(platform) and time.monotonic()<deadline:time.sleep(.02)
  self.assertEqual(completion_errors,[])
  self.assertCountEqual(server.platform_completed,platform)
  self.assertEqual(len(fixture.native_requests),1)
 def test_the_child_gets_only_the_private_environment(self):
  root=Path(tempfile.mkdtemp(prefix='hgs-kimi-control-')).resolve();self.addCleanup(shutil.rmtree,root)
  leaked=dict(OPENAI_API_KEY='leak',KIMI_EXTRA='leak',HTTPS_PROXY='http://proxy.invalid',SSH_AUTH_SOCK='/leak/agent',
              HGS_SESSION='leak',TMUX='/leak/tmux,1,0',DYLD_INSERT_LIBRARIES='/leak/lib.dylib',KIMI_CODE_BASE_URL='https://leak.invalid')
  platform='http://127.0.0.1:9'+guard.PLATFORM
  with mock.patch.dict(os.environ,leaked):env=child_environment(root,root/'.kimi-code',platform)
  self.assertEqual(set(env)-set(KEYS),{'PATH','DISABLE_AUTOUPDATER','TERM','LANG','PYTHONUTF8'})
  self.assertFalse([value for value in env.values() if 'leak' in value])
  self.assertEqual((env['KIMI_CODE_BASE_URL'],env['KIMI_CODE_NO_AUTO_UPDATE'],env['KIMI_DISABLE_TELEMETRY']),(platform,'1','1'))
  for key in set(KEYS)-{'KIMI_CODE_BASE_URL','KIMI_CODE_NO_AUTO_UPDATE','KIMI_DISABLE_TELEMETRY'}:
   self.assertTrue(env[key]==str(root) or env[key].startswith(str(root)+'/'),key)
 def test_the_keys_and_refusals_are_test_kimi_natives(self):
  tree=ast.parse(textwrap.dedent(inspect.getsource(guard.NativeKimi.assert_process)))
  found={node.targets[0].id:ast.literal_eval(node.value) for node in ast.walk(tree) if isinstance(node,ast.Assign) and
         len(node.targets)==1 and isinstance(node.targets[0],ast.Name) and node.targets[0].id in ('keys','forbidden')}
  self.assertEqual(found,dict(keys=KEYS,forbidden=FORBIDDEN))
 def test_a_changed_pin_or_copy_stops_before_any_launch(self):
  root=Path(tempfile.mkdtemp(prefix='hgs-kimi-control-')).resolve();self.addCleanup(shutil.rmtree,root)
  pinned=root/'pinned';pinned.write_bytes(b'pinned bytes');pinned.chmod(0o600)
  digest=hashlib.sha256(b'pinned bytes').hexdigest()
  for pin,refused in (('0'*64,True),('not-a-pin',True),(digest,False)):
   case=self.case();case.images=root/('images-'+pin[:4]);case.images.mkdir()
   with mock.patch.multiple(sys.modules[__name__],KIMI=str(pinned),KIMI_SHA256=pin,GUARDED=[]),mock.patch.object(guard,'admitted_version',return_value='2.3.1'):
    if refused:
     with self.assertRaises(AssertionError):case.stage()
     self.assertFalse((case.images/'kimi').exists(),pin)
    else:
     case.stage()
     self.assertEqual((sha256(case.kimi),stat.S_IMODE(case.kimi.stat().st_mode)),(digest,0o755))
  self.assertEqual(stat.S_IMODE(pinned.stat().st_mode),0o600)
 def test_an_unadmitted_capture_fails_closed_without_reading_the_process(self):
  case=self.case();case.pid=12345;case.kimi=case.image=Path('/fixture/private/kimi')
  case.captures='/fixture/private/root/kimi-exec';case.child_env={key:'/fixture/private/'+key for key in KEYS}
  case.start=guard.lstart_text(1792368000);case.wait_for=lambda callback,seconds=30:self.assertTrue(callback())
  case.parent_binding=lambda:dict(process_start=case.start,supervisor=dict(pid=4242,start='Sun Oct 18 00:00:00 2026'))
  identity=dict(pid=12345,own=True,zombie=False,ppid=4242,birth=(1792368000,123456),image=case.kimi,
                parent_start='Sun Oct 18 00:00:00 2026')
  with mock.patch.object(sys,'platform','darwin'),mock.patch.object(guard,'executable',return_value=case.kimi),\
       mock.patch.multiple(sys.modules[__name__],KIMI_SHA256='a'*64,sha256=mock.Mock(return_value='a'*64)),\
       mock.patch.object(guard.MacProcess,'identity',return_value=identity),\
       mock.patch.object(guard.MacProcess,'procargs',side_effect=AssertionError('the process was read')),\
       mock.patch('builtins.print') as printed:
   with self.assertRaisesRegex(AssertionError,'the Kimi environment cannot be proven: the exec capture cannot be admitted'):
    case.prove_launch()
  logs=[call.args[0] for call in printed.call_args_list if call.args[0].startswith('ZERUS_KIMI_ENVIRONMENT=')]
  self.assertEqual(len(logs),1);self.assertIsNone(json.loads(logs[0].split('=',1)[1])['keys'])
 def test_only_the_identity_checked_private_tmux_server_is_the_panes_parent(self):
  case=self.case();case.tmux_pid=4242;case.start=guard.lstart_text(1792368000)
  def info(uid=None):return mock.Mock(uid=os.geteuid() if uid is None else uid,start_sec=1792281600)
  with mock.patch.object(sys,'platform','darwin'),mock.patch.object(fixtures,'TMUX','/fixture/tmux'):
   for server,image in ((info(os.geteuid()+1),Path('/fixture/tmux')),(info(),Path('/fixture/other-tmux'))):
    with mock.patch.object(guard.MacProcess,'info',return_value=server),mock.patch.object(guard,'executable',return_value=image):
     with self.assertRaises(AssertionError):case.parent_binding()
   with mock.patch.object(guard.MacProcess,'info',return_value=info()),mock.patch.object(guard,'executable',return_value=Path('/fixture/tmux')):
    binding=case.parent_binding()
  self.assertEqual(binding,dict(process_start=case.start,supervisor=dict(pid=4242,start='Sun Oct 18 00:00:00 2026')))
  image=Path('/fixture/private/kimi');record=dict(pid=12345,birth=(1792368000,123456))
  identity=dict(pid=12345,own=True,zombie=False,ppid=4242,birth=record['birth'],image=image,parent_start='Sun Oct 18 00:00:00 2026')
  self.assertEqual(guard.identity_problems(identity,identity,record,image,binding),[])
  for changed in (dict(ppid=999),dict(parent_start='Sun Oct 18 00:00:05 2026')):
   wrong=dict(identity,**changed)
   self.assertIn("the process is not the recorded supervisor's child",guard.identity_problems(wrong,wrong,record,image,binding))
 def test_custody_keeps_the_root_and_copy_unless_each_step_completes(self):
  base=Path(tempfile.mkdtemp(prefix='hgs-kimi-control-')).resolve();self.addCleanup(shutil.rmtree,base)
  steps=('retain_screens','stop_clients','quiesce','stop_server','retain_files','seal')
  remove=shutil.rmtree
  for failing in (None,*steps,'root removal','copy removal'):
   case=self.case();case.root=base/(str(failing)+'-root');case.images=base/(str(failing)+'-images')
   case.root.mkdir();case.images.mkdir();order=[]
   def step(name):
    def action(*arguments):
     order.append((name,*arguments))
     if name==failing:raise AssertionError(name+' failed')
    return action
   for name in steps:setattr(case,name,step(name))
   def rmtree(path):
    if (failing,path)==('root removal',case.root) or (failing,path)==('copy removal',case.images):raise OSError(13,'denied')
    remove(path)
   with mock.patch.object(shutil,'rmtree',side_effect=rmtree):
    if failing:
     with self.assertRaises(AssertionError) as raised:case.custody()
    else:case.custody()
   # Every independent step was attempted; the seal knew whether the shutdown completed.
   self.assertEqual([entry[0] for entry in order],list(steps),failing)
   self.assertEqual(order[-1],('seal',failing not in ('stop_clients','quiesce','stop_server')),failing)
   kept={name for name in ('root','images') if getattr(case,name).exists()}
   self.assertEqual(kept,set() if failing is None else {'images'} if failing=='copy removal' else {'root','images'},failing)
   if failing in ('root removal','copy removal'):
    self.assertIn('removal of '+str(case.root if failing=='root removal' else case.images)+' failed',str(raised.exception))
   if failing=='copy removal':self.assertIn("removed ['"+str(case.root)+"']",str(raised.exception))
  # The attached clients: closed, then terminated, then killed; one that never exits fails custody.
  class Client:
   def __init__(self,exits):self.exits,self.code,self.pid,self.stdin,self.calls=exits,None,4321,mock.Mock(),[]
   def poll(self):return self.code
   def terminate(self):self.calls.append('terminate');self.code=-15 if self.exits=='terminate' else None
   def kill(self):self.calls.append('kill');self.code=-9 if self.exits=='kill' else None
   def wait(self,timeout):
    if self.exits=='close' and self.code is None:self.code=0
    if self.code is None:raise subprocess.TimeoutExpired('tmux',timeout)
    return self.code
  for exits,calls in (('close',[]),('terminate',['terminate']),('kill',['terminate','kill']),('never',['terminate','kill'])):
   case=self.case();case.clients=[client:=Client(exits)]
   if exits=='never':
    with self.assertRaisesRegex(AssertionError,'attached clients did not exit: \\[4321\\]'):case.stop_clients()
   else:case.stop_clients()
   self.assertEqual(client.calls,calls,exits);client.stdin.close.assert_called_once_with()

if __name__=='__main__':unittest.main()
