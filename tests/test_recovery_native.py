"""Opt-in real Claude/Codex/Kimi against localhost fixtures, isolated HOME and tmux."""
import http.server,json,os,shutil,subprocess,threading,time,unittest
from pathlib import Path
import test_pause as lifecycle

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
  super().setUp();self.t('new-session','-d','-s','fixture-control','sleep','600');self.t('set-option','-g','remain-on-exit','on')
 def exercise(self,agent):
  global fixture
  fixture=self;self.requests=[];self.failing=True
  binary=shutil.which(agent,path=os.environ['PATH'])
  if not binary and agent=='kimi' and (Path.home()/'.kimi-code/bin/kimi').exists():binary=str(Path.home()/'.kimi-code/bin/kimi')
  if not binary:self.skipTest('Native '+agent+' is not installed on this machine')
  binary=str(Path(binary).resolve())
  (self.bin/agent).unlink();(self.bin/agent).symlink_to(binary)
  server=http.server.ThreadingHTTPServer(('127.0.0.1',0),Handler)
  self.addCleanup(server.server_close);self.addCleanup(server.shutdown)
  threading.Thread(target=server.serve_forever,daemon=True).start();base='http://127.0.0.1:'+str(server.server_port)
  self.env.update(ANTHROPIC_API_KEY='hgs-offline-probe',ANTHROPIC_AUTH_TOKEN='hgs-offline-probe',ANTHROPIC_BASE_URL=base,OPENAI_API_KEY='hgs-offline-probe',OPENAI_BASE_URL=base+'/v1',CLAUDE_CODE_MAX_RETRIES='0',CLAUDE_CODE_DISABLE_NONESSENTIAL_TRAFFIC='1',DISABLE_AUTOUPDATER='1')
  for key in ('ANTHROPIC_API_KEY','ANTHROPIC_AUTH_TOKEN','ANTHROPIC_BASE_URL','OPENAI_API_KEY','OPENAI_BASE_URL','CLAUDE_CODE_MAX_RETRIES','CLAUDE_CODE_DISABLE_NONESSENTIAL_TRAFFIC','DISABLE_AUTOUPDATER'):
   self.t('set-environment','-g',key,self.env[key])
  (self.root/'.claude.json').write_text(json.dumps({'hasCompletedOnboarding':True,'hasSeenAutoDefaultNudge':True,'bypassPermissionsModeAccepted':True,'theme':'dark','customApiKeyResponses':{'approved':['hgs-offline-probe'],'rejected':[]},'projects':{str(self.project.resolve()):{'hasTrustDialogAccepted':True}}}))
  (self.root/'.codex').mkdir()
  (self.root/'.codex/config.toml').write_text('check_for_update_on_startup = false\nmodel_provider = "probe"\n[model_providers.probe]\nname = "probe"\nbase_url = "'+base+'/v1"\nenv_key = "OPENAI_API_KEY"\nwire_api = "responses"\nrequest_max_retries = 0\nstream_max_retries = 0\n[projects.'+json.dumps(str(self.project.resolve()))+']\ntrust_level = "trusted"\n')
  (self.root/'.kimi-code').mkdir()
  (self.root/'.kimi-code/config.toml').write_text('default_model = "probe"\ntelemetry = false\n[providers.probe]\ntype = "openai"\nbase_url = "'+base+'/v1"\napi_key = "hgs-offline-probe"\n[models.probe]\nprovider = "probe"\nmodel = "probe"\nmax_context_size = 32768\ncapabilities = ["tool_use"]\n')
  (self.root/'.kimi-code/tui.toml').write_text('[upgrade]\nauto_install = false\n')
  with (self.root/'.kimi-code/config.toml').open('a') as f:f.write('\n[loop_control]\nmax_attempts_per_step = 1\n')
  name=agent+'/p/recovery';target=name+':'
  args={'codex':['--dangerously-bypass-hook-trust','--dangerously-bypass-approvals-and-sandbox','--no-alt-screen'], 'claude':['--dangerously-skip-permissions'],'kimi':[]}[agent]
  if agent=='codex' and '--dangerously-bypass-hook-trust' not in subprocess.check_output([binary,'--help'],text=True):args.remove('--dangerously-bypass-hook-trust')
  self.hgs(agent,'p','-n','recovery','-d','--',*args)
  def screen():return self.t('capture-pane','-p','-t',target)
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
  finally:worker.terminate();worker.wait(timeout=5);log.close()
 def test_claude(self):self.exercise('claude')
 def test_codex(self):self.exercise('codex')
 def test_kimi(self):self.exercise('kimi')

if __name__=='__main__':unittest.main()
