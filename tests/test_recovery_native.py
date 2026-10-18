"""Opt-in real Claude/Codex/Kimi against localhost fixtures, isolated HOME and tmux.

An agent's updater replaces the executable it runs from, whatever its home, so a
case never runs an installation or a link to one. It runs a verified copy of one
native executable image in a folder of its own, checks that the initial and the
resumed agent process run that copy, and fails if the copy or the installation
changed. Agents see only an explicit environment with private roots."""
import ctypes,hashlib,http.server,json,os,shutil,subprocess,sys,tempfile,threading,time,unittest
from pathlib import Path
import test_pause as lifecycle

# Mach-O (thin and universal) and ELF: a script or launcher shim cannot be copied alone.
NATIVE_MAGIC=(b'\xcf\xfa\xed\xfe',b'\xce\xfa\xed\xfe',b'\xca\xfe\xba\xbe',b'\xbe\xba\xfe\xca',b'\x7fELF')
# Private roots of the fixture home: no agent finds a profile, cache or state of the caller's.
PRIVATE_ROOTS={'TMPDIR':'tmp','XDG_CONFIG_HOME':'.config','XDG_DATA_HOME':'.local/share','XDG_CACHE_HOME':'.cache',
 'XDG_STATE_HOME':'.local/state','CLAUDE_CONFIG_DIR':'.claude','CODEX_HOME':'.codex','KIMI_CODE_HOME':'.kimi-code'}
HGS_SETTINGS=('HGS_CONFIG_DIR','HGS_STATE_DIR','HGS_SELF','HGS_PEERS','HGS_TAB','HGS_TRACKING')
# Every variable a native probe may see: no inherited proxy, SSH agent, cloud,
# npm, Git, profile or provider setting is among them.
ALLOWED={'HOME','USER','LOGNAME','PATH','SHELL','TERM','LANG','NO_PROXY','no_proxy','DISABLE_AUTOUPDATER',
 'KIMI_CODE_NO_AUTO_UPDATE','CLAUDE_CODE_DISABLE_NONESSENTIAL_TRAFFIC',*PRIVATE_ROOTS,*HGS_SETTINGS,
 'ANTHROPIC_API_KEY','ANTHROPIC_AUTH_TOKEN','ANTHROPIC_BASE_URL','OPENAI_API_KEY','OPENAI_BASE_URL','CLAUDE_CODE_MAX_RETRIES'}

def digest(path):
 h=hashlib.sha256()
 with open(path,'rb') as f:
  for chunk in iter(lambda:f.read(1<<20),b''):h.update(chunk)
 return h.hexdigest()

def native_copy(source,images):
 """The resolved installation, its verified copy in images and its digest, or a
 failure: anything but one regular native executable image is never run."""
 real=Path(os.path.realpath(source))
 if not real.is_file():raise AssertionError('Not a regular native executable: '+str(real))
 with real.open('rb') as f:magic=f.read(4)
 if magic not in NATIVE_MAGIC:raise AssertionError('Refusing '+str(real)+': not a native executable image, so no safe copy of it runs')
 before=digest(real);image=images/real.name
 shutil.copyfile(real,image);image.chmod(0o755)
 if digest(image)!=before:raise AssertionError('The copy of '+str(real)+' differs from it')
 return real,image,before

def executable(pid):
 """The image a process actually runs: libproc's proc_pidpath on macOS, /proc on Linux."""
 if sys.platform=='darwin':
  libproc=ctypes.CDLL('/usr/lib/libproc.dylib')
  libproc.proc_pidpath.argtypes=[ctypes.c_int,ctypes.c_char_p,ctypes.c_uint32];libproc.proc_pidpath.restype=ctypes.c_int
  buf=ctypes.create_string_buffer(4096)
  if libproc.proc_pidpath(int(pid),buf,len(buf))<=0:raise AssertionError('No executable image for process '+str(pid))
  return os.path.realpath(buf.value.decode())
 if sys.platform.startswith('linux'):return os.readlink(f'/proc/{int(pid)}/exe')
 raise AssertionError('The executable image of a process cannot be proven on '+sys.platform)

def fixture_environment(root,bin,settings,caller):
 """Only what a native probe needs. `settings` are the fixture's own hgs
 settings; of the caller's environment only the user name passes."""
 env={key:caller[key] for key in ('USER','LOGNAME') if key in caller}
 env.update({key:settings[key] for key in HGS_SETTINGS})
 for key,folder in PRIVATE_ROOTS.items():env[key]=str(root/folder)
 for key in ('TMPDIR','XDG_CONFIG_HOME','XDG_DATA_HOME','XDG_CACHE_HOME','XDG_STATE_HOME'):Path(env[key]).mkdir(parents=True,exist_ok=True)
 env.update(HOME=str(root),SHELL=str(bin/'shell'),PATH=os.pathsep.join([str(bin),'/usr/bin','/bin','/usr/sbin','/sbin']),
  TERM='xterm-256color',LANG='en_US.UTF-8' if sys.platform=='darwin' else 'C.UTF-8',NO_PROXY='127.0.0.1,localhost',
  no_proxy='127.0.0.1,localhost',DISABLE_AUTOUPDATER='1',KIMI_CODE_NO_AUTO_UPDATE='1',CLAUDE_CODE_DISABLE_NONESSENTIAL_TRAFFIC='1')
 return env

fixture=None
class Handler(http.server.BaseHTTPRequestHandler):
 def log_message(self,*args):pass
 def do_POST(self):
  req=json.loads(self.rfile.read(int(self.headers.get('content-length',0))))
  fixture.requests.append(req)
  if fixture.failing:
   self.send_response(503);self.send_header('Content-Type','application/json');self.send_header('x-should-retry','false');self.end_headers();self.wfile.write(json.dumps({'type':'error','error':{'type':'overloaded_error','code':'server_error','message':'503 Service unavailable fixture'}}).encode());return
  self.send_response(200);self.send_header('Content-Type','text/event-stream');self.end_headers()
  def send(typ,data):
   self.wfile.write(('event: '+typ+'\ndata: '+json.dumps(data)+'\n\n').encode());self.wfile.flush()
  if '/messages' in self.path:
   msg={'id':'msg_probe','type':'message','role':'assistant','model':req.get('model','probe'),'content':[],'stop_reason':None,'stop_sequence':None,'usage':{'input_tokens':1,'output_tokens':1}}
   events=[('message_start',{'message':msg}),('content_block_start',{'index':0,'content_block':{'type':'text','text':''}}),('content_block_delta',{'index':0,'delta':{'type':'text_delta','text':'Offline probe complete.'}}),('content_block_stop',{'index':0}),('message_delta',{'delta':{'stop_reason':'end_turn','stop_sequence':None},'usage':{'output_tokens':5}}),('message_stop',{})]
   for typ,d in events:send(typ,dict(type=typ,**d))
  elif '/responses' in self.path:
   part={'type':'output_text','text':'Offline probe complete.','annotations':[]}
   item={'type':'message','id':'msg_probe','role':'assistant','status':'completed','content':[part]}
   response={'id':'resp_probe','object':'response','status':'completed','output':[item],'usage':{'input_tokens':1,'output_tokens':5,'total_tokens':6}}
   for typ,d in [('response.created',{'response':dict(response,status='in_progress',output=[])}),('response.output_item.added',{'output_index':0,'item':dict(item,status='in_progress',content=[])}),('response.content_part.added',{'output_index':0,'content_index':0,'item_id':'msg_probe','part':dict(part,text='')}),('response.output_text.delta',{'output_index':0,'content_index':0,'item_id':'msg_probe','delta':part['text']}),('response.output_text.done',{'output_index':0,'content_index':0,'item_id':'msg_probe','text':part['text']}),('response.content_part.done',{'output_index':0,'content_index':0,'item_id':'msg_probe','part':part}),('response.output_item.done',{'output_index':0,'item':item}),('response.completed',{'response':response})]:send(typ,dict(type=typ,**d))
  else:
   for delta,finish in [({'role':'assistant','content':'Offline probe complete.'},None),({},'stop')]:
    self.wfile.write(('data: '+json.dumps({'id':'chat_probe','object':'chat.completion.chunk','model':'probe','choices':[{'index':0,'delta':delta,'finish_reason':finish}]})+'\n\n').encode())
   self.wfile.write(b'data: [DONE]\n\n');self.wfile.flush()

@unittest.skipUnless(os.environ.get('HGS_NATIVE_RECOVERY')=='1','Set HGS_NATIVE_RECOVERY=1 for installed native terminal clients')
class NativeRecovery(lifecycle.Harness):
 def setUp(self):
  super().setUp()
  # Before the private tmux server starts: it and every agent inherit this environment.
  self.env=fixture_environment(self.root,self.bin,self.env,os.environ)
  self.t('new-session','-d','-s','fixture-control','sleep','600');self.t('set-option','-g','remain-on-exit','on')
 def install(self,agent,binary):
  """Run a verified copy of `binary` as `agent`: the fixture's PATH names only
  that copy, which lives outside the home tearDown removes, so once every
  process of the private socket has ended, its bytes and the installation's
  are checked."""
  images=Path(tempfile.mkdtemp(prefix='hgs-recovery-image-'));self.addCleanup(shutil.rmtree,images,ignore_errors=True)
  source,image,before=native_copy(binary,images)
  (self.bin/agent).unlink();(self.bin/agent).symlink_to(image)
  self.image,self.image_digest=image,before
  def unchanged():
   self.assertEqual(digest(image),before,'the fixture changed its own copy of '+agent)
   self.assertEqual(digest(source),before,'the installed '+agent+' changed')
  self.unchanged=unchanged;self.addCleanup(unchanged)
  return image
 def assert_runs_copy(self,pid):
  """The process runs the fixture's copy, which still has the verified bytes."""
  self.assertEqual(executable(pid),os.path.realpath(self.image),'process '+str(pid)+' does not run the fixture copy')
  self.assertEqual(digest(self.image),self.image_digest,'the fixture copy changed while its agent ran')
 def exercise(self,agent):
  global fixture
  fixture=self;self.requests=[];self.failing=True
  binary=shutil.which(agent,path=os.environ['PATH'])
  if not binary and agent=='kimi' and (Path.home()/'.kimi-code/bin/kimi').exists():binary=str(Path.home()/'.kimi-code/bin/kimi')
  if not binary:self.skipTest('Native '+agent+' is not installed on this machine')
  binary=str(self.install(agent,binary))
  server=http.server.ThreadingHTTPServer(('127.0.0.1',0),Handler)
  self.addCleanup(server.server_close);self.addCleanup(server.shutdown)
  threading.Thread(target=server.serve_forever,daemon=True).start();base='http://127.0.0.1:'+str(server.server_port)
  self.env.update(ANTHROPIC_API_KEY='hgs-offline-probe',ANTHROPIC_AUTH_TOKEN='hgs-offline-probe',ANTHROPIC_BASE_URL=base,OPENAI_API_KEY='hgs-offline-probe',OPENAI_BASE_URL=base+'/v1',CLAUDE_CODE_MAX_RETRIES='0',CLAUDE_CODE_DISABLE_NONESSENTIAL_TRAFFIC='1',DISABLE_AUTOUPDATER='1',KIMI_CODE_NO_AUTO_UPDATE='1')
  self.assertLessEqual(set(self.env),ALLOWED)
  for key in ('ANTHROPIC_API_KEY','ANTHROPIC_AUTH_TOKEN','ANTHROPIC_BASE_URL','OPENAI_API_KEY','OPENAI_BASE_URL','CLAUDE_CODE_MAX_RETRIES','CLAUDE_CODE_DISABLE_NONESSENTIAL_TRAFFIC','DISABLE_AUTOUPDATER','KIMI_CODE_NO_AUTO_UPDATE'):
   self.t('set-environment','-g',key,self.env[key])
  # Claude reads this file from CLAUDE_CONFIG_DIR, which the fixture names.
  claude=json.dumps({'hasCompletedOnboarding':True,'bypassPermissionsModeAccepted':True,'theme':'dark','customApiKeyResponses':{'approved':['hgs-offline-probe'],'rejected':[]},'projects':{str(self.project.resolve()):{'hasTrustDialogAccepted':True}}})
  (self.root/'.claude.json').write_text(claude);(self.root/'.claude').mkdir(exist_ok=True);(self.root/'.claude/.claude.json').write_text(claude)
  (self.root/'.codex').mkdir()
  (self.root/'.codex/config.toml').write_text('check_for_update_on_startup = false\nmodel_provider = "probe"\n[model_providers.probe]\nname = "probe"\nbase_url = "'+base+'/v1"\nenv_key = "OPENAI_API_KEY"\nwire_api = "responses"\nrequest_max_retries = 0\nstream_max_retries = 0\n[projects.'+json.dumps(str(self.project.resolve()))+']\ntrust_level = "trusted"\n')
  (self.root/'.kimi-code').mkdir()
  (self.root/'.kimi-code/config.toml').write_text('default_model = "probe"\ntelemetry = false\n[providers.probe]\ntype = "openai"\nbase_url = "'+base+'/v1"\napi_key = "hgs-offline-probe"\n[models.probe]\nprovider = "probe"\nmodel = "probe"\nmax_context_size = 32768\ncapabilities = ["tool_use"]\n')
  (self.root/'.kimi-code/tui.toml').write_text('[upgrade]\nauto_install = false\n')
  with (self.root/'.kimi-code/config.toml').open('a') as f:f.write('\n[loop_control]\nmax_attempts_per_step = 1\n')
  name=agent+'/p/recovery';target=name+':'
  args={'codex':['--dangerously-bypass-hook-trust','--dangerously-bypass-approvals-and-sandbox','--no-alt-screen'], 'claude':['--dangerously-skip-permissions'],'kimi':[]}[agent]
  if agent=='codex' and '--dangerously-bypass-hook-trust' not in subprocess.check_output([binary,'--help'],text=True,env=self.env,stdin=subprocess.DEVNULL):args.remove('--dangerously-bypass-hook-trust')
  self.hgs(agent,'p','-n','recovery','-d','--',*args)
  def screen():return self.t('capture-pane','-p','-t',target)
  self.wait(lambda:self.binding(name).get('pid'));self.assert_runs_copy(self.binding(name)['pid'])
  time.sleep(2)
  if agent=='kimi' and ('trust' in screen().lower() or 'Yes' in screen()):self.t('send-keys','-t',target,'Enter')
  if 'Make auto mode' in screen():self.t('send-keys','-t',target,'Down','Enter')
  time.sleep(.5);self.t('send-keys','-t',target,'-l','Offline recovery fixture');time.sleep(.2);self.t('send-keys','-t',target,'Enter')
  def inspect():return json.loads(self.hgs('inspect',name))
  def wait_for(check,seconds=45):
   until=time.monotonic()+seconds
   while time.monotonic()<until:
    if check():return
    time.sleep(.2)
   self.fail('Native timeout '+agent+'\n'+screen()+'\nStyled: '+repr(self.t('capture-pane','-p','-e','-t',target))+'\nCursor: '+self.t('display-message','-p','-t',target,'#{cursor_x}:#{cursor_y}')+'\n'+json.dumps(inspect(),ensure_ascii=False))
  wait_for(lambda:inspect().get('phase')=='error')
  if 'Make auto mode' in screen():self.t('send-keys','-t',target,'Down','Enter');time.sleep(.5)
  # The API never succeeds until the native client has stopped its own retries.
  self.failing=False
  directory=self.root/'state/recovery';directory.mkdir(exist_ok=True)
  (directory/'policy.json').write_text(json.dumps(dict(version=1,revision=1,enabled=True,enabled_at=1,service=True,network=True,rate_limit=True,delays=[1,1])))
  log=(self.root/'recovery.log').open('w');worker=subprocess.Popen([str(lifecycle.HGS),'recovery','worker'],env=self.env,stdout=log,stderr=log)
  try:
   wait_for(lambda:inspect().get('recovery',{}).get('state') in ('succeeded','uncertain','cancelled'))
   result=inspect();self.assertEqual(result['recovery']['state'],'succeeded',json.dumps(result,ensure_ascii=False)+'\n'+screen())
   self.assertEqual(result['recovery']['attempt'],1);self.assertTrue(result['recovery']['acknowledged'])
   self.assertTrue(any('[HGS automatic recovery]' in json.dumps(r) for r in self.requests))
   # Whichever process recovery left running, a resumed one included, runs the copy.
   self.assert_runs_copy(self.binding(name)['pid'])
  finally:worker.terminate();worker.wait(timeout=5);log.close()
 def test_claude(self):self.exercise('claude')
 def test_codex(self):self.exercise('codex')
 def test_kimi(self):self.exercise('kimi')

@unittest.skipUnless(lifecycle.HGS.exists(),'built hgs required as a stand-in native executable')
class RecoveryFixtureIsolation(lifecycle.Harness):
 """NativeRecovery's isolation without a native agent: a copy of the built hgs
 stands in for one, an updater is a rename over the realpath of the executable
 it started from, and the caller's environment holds hazardous settings."""
 install=NativeRecovery.install
 assert_runs_copy=NativeRecovery.assert_runs_copy
 HAZARDS=('HTTP_PROXY','HTTPS_PROXY','ALL_PROXY','http_proxy','https_proxy','SSH_AUTH_SOCK','SSH_AGENT_PID','AWS_PROFILE',
  'AWS_ACCESS_KEY_ID','GOOGLE_APPLICATION_CREDENTIALS','AZURE_CONFIG_DIR','NPM_CONFIG_USERCONFIG','npm_config__authToken',
  'GIT_CONFIG_GLOBAL','GIT_SSH_COMMAND','GH_TOKEN','ANTHROPIC_API_KEY','OPENAI_API_KEY','DEEPSEEK_API_KEY','CLAUDE_CONFIG_DIR',
  'CODEX_HOME','KIMI_CODE_HOME','OPENCODE_CONFIG','XDG_CONFIG_HOME','XDG_CACHE_HOME','TMPDIR','NODE_OPTIONS','BUN_INSTALL',
  'DYLD_INSERT_LIBRARIES','LD_PRELOAD','PYTHONPATH','TMUX','TMUX_PANE','HGS_ACCOUNT','HGS_SESSION','HGS_RUN_ID')
 def setUp(self):
  super().setUp()
  # The installation the fixture copies: a native image the control owns, outside
  # the home tearDown removes, so the cleanup's own check can still read it.
  installs=Path(tempfile.mkdtemp(prefix='hgs-recovery-installed-'));self.addCleanup(shutil.rmtree,installs,ignore_errors=True)
  self.installed=installs/'kimi';shutil.copyfile(lifecycle.HGS,self.installed);self.installed.chmod(0o755)
  self.original=digest(self.installed);self.processes=[]
  def stop():
   for process,stdin in self.processes:
    process.kill();process.wait(timeout=5);os.close(stdin)
  self.addCleanup(stop)
 def update(self,path):
  """What a self-updater does: replace the realpath of its own executable."""
  target=os.path.realpath(path);new=target+'.update'
  Path(new).write_bytes(Path(target).read_bytes()+b'\0updated');os.chmod(new,0o755);os.replace(new,target)
 def start(self,image):
  """A long-lived process of a native image: hgs waiting for its request on stdin."""
  read,write=os.pipe()
  process=subprocess.Popen([str(image),'wait','ack','--json'],stdin=read,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,
   env=fixture_environment(self.root,self.bin,self.env,{}))
  os.close(read);self.processes.append((process,write))
  for _ in range(100):
   try:
    if executable(process.pid)==os.path.realpath(image):return process
   except AssertionError:pass
   time.sleep(.05)
  self.fail('the stand-in did not start')
 def restore(self,image):
  # The verified bytes again, so the cleanup's own check passes once a guard has been shown to fail.
  shutil.copyfile(self.installed,image)

 def test_only_one_regular_native_image_is_copied(self):
  link=self.root/'shim/kimi';link.parent.mkdir();link.symlink_to(self.installed)
  images=self.root/'images';images.mkdir()
  source,image,before=native_copy(link,images)
  self.assertEqual((source,before),(self.installed.resolve(),self.original))
  self.assertFalse(image.is_symlink());self.assertTrue(image.is_file());self.assertEqual(digest(image),self.original)
  # A script or launcher shim, a folder or a missing file is refused, and nothing is copied.
  script=self.root/'shim/codex';script.write_text('#!/usr/bin/env node\nrequire("./codex.js")\n');script.chmod(0o755)
  for refused in (script,self.root/'shim',self.root/'missing'):
   with self.subTest(refused.name),self.assertRaises(AssertionError):native_copy(refused,images)
  self.assertEqual(sorted(path.name for path in images.iterdir()),['kimi'])

 def test_an_updater_replaces_only_the_fixture_copy(self):
  image=self.install('kimi',self.installed)
  self.update(self.bin/'kimi')
  self.assertEqual(digest(self.installed),self.original,'the agent replaced the installation through the fixture')
  with self.assertRaisesRegex(AssertionError,'changed its own copy'):self.unchanged()
  self.restore(image)

 def test_a_changed_installation_fails_the_check(self):
  image=self.install('kimi',self.installed)
  self.update(self.installed)
  with self.assertRaisesRegex(AssertionError,'installed kimi changed'):self.unchanged()
  shutil.copyfile(image,self.installed)

 def test_the_agent_process_must_run_the_unchanged_copy(self):
  image=self.install('kimi',self.installed)
  self.assert_runs_copy(self.start(self.bin/'kimi').pid)
  # The installation itself is never accepted in its place.
  with self.assertRaisesRegex(AssertionError,'does not run the fixture copy'):self.assert_runs_copy(self.start(self.installed).pid)
  running=self.start(self.bin/'kimi')
  self.update(self.bin/'kimi')
  with self.assertRaises(AssertionError):self.assert_runs_copy(running.pid)
  self.restore(image)

 def test_only_allowlisted_variables_reach_agents_and_their_tmux_server(self):
  marker='zerus-fixture-hazard'
  caller={key:marker+'-'+key for key in self.HAZARDS}
  caller.update(USER='fixture-user',PATH=str(self.installed.parent)+os.pathsep+'/usr/bin')
  env=fixture_environment(self.root,self.bin,self.env,caller)
  # Names only: no value of the caller's is ever printed.
  self.assertLessEqual(set(env),ALLOWED)
  leaked=sorted(key for key in caller if key!='USER' and any(caller[key] in value for value in env.values()))
  self.assertFalse(leaked,'caller settings reached the fixture: '+', '.join(leaked))
  self.assertEqual((env['HOME'],env['USER']),(str(self.root),'fixture-user'))
  self.assertEqual(env['PATH'].split(os.pathsep),[str(self.bin),'/usr/bin','/bin','/usr/sbin','/sbin'])
  for key in PRIVATE_ROOTS:self.assertTrue(Path(env[key]).is_relative_to(self.root),key)
  self.assertEqual((env['DISABLE_AUTOUPDATER'],env['KIMI_CODE_NO_AUTO_UPDATE']),('1','1'))
  # The private tmux server, whose global environment every agent inherits.
  self.env=env;self.t('new-session','-d','-s','fixture-control','sleep','600')
  names={line.lstrip('-').split('=',1)[0] for line in self.t('show-environment','-g').splitlines() if line}
  # The /bin/sh of the private tmux wrapper sets its own PWD and SHLVL.
  own=ALLOWED|{'PWD','SHLVL'}
  self.assertLessEqual(names,own,'tmux server variables outside the allowlist: '+', '.join(sorted(names-own)))
  self.assertFalse([line.split('=',1)[0] for line in self.t('show-environment','-g').splitlines() if marker in line])

if __name__=='__main__':unittest.main()
