"""Opt-in released OpenCode contract, private homes and a keyless model.

ZERUS_TEST_OPENCODE_NATIVE=1 makes missing/wrong native binaries failures.
No live agents, credentials or inferencing services are used.
"""
import base64
import hashlib
import http.server
import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import tempfile
import threading
import time
import unittest
import urllib.request
import uuid

REPO = Path(__file__).resolve().parents[1]
HGS = Path(os.environ.get('HGS_TEST_BIN', REPO / 'target/debug/hgs'))
NATIVE = os.environ.get('ZERUS_TEST_OPENCODE_BIN', shutil.which('opencode') or '')


class Model(http.server.BaseHTTPRequestHandler):
    requests = []
    def log_message(self, *_): pass
    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
        self.requests.append(body)
        text = '\n'.join(str(m.get('content', '')) for m in body.get('messages', []) if m.get('role') == 'user')
        if 'HOLD_NATIVE' in text:
            self.server.holding.set()
            self.server.release.wait(20)
        last_user = next((str(m.get('content','')) for m in reversed(body.get('messages',[])) if m.get('role')=='user'), '')
        if 'ERROR_NATIVE' in last_user:
            self.send_response(401);self.send_header('Content-Type','application/json');self.end_headers()
            self.wfile.write(json.dumps({'error':{'message':'Fixture provider rejected','code':'invalid_api_key'}}).encode());return
        tool = None
        if not any(m.get('role')=='tool' for m in body.get('messages',[])):
            if 'QUESTION_NATIVE' in last_user: tool = ('question', {'questions':[{'question':'Pick a color','header':'Color','options':[{'label':'Green','description':'First'},{'label':'Blue','description':'Second'}]}]})
            elif 'BASH_NATIVE' in last_user: tool = ('bash',{'command':'printf native-tool-output','description':'Print isolated fixture output'})
        if tool:
            choices=[{'index':0,'delta':{'tool_calls':[{'index':0,'id':'call_fixture','type':'function','function':{'name':tool[0],'arguments':json.dumps(tool[1])}}]},'finish_reason':None}, {'index':0,'delta':{},'finish_reason':'tool_calls'}]
        else:
            choices = [{'index': 0, 'delta': {'content': 'Native OpenCode reply'}, 'finish_reason': None},
                   {'index': 0, 'delta': {}, 'finish_reason': 'stop'}]
        try:
            self.send_response(200)
            self.send_header('Content-Type', 'text/event-stream')
            self.end_headers()
            for choice in choices:
                v = {'id': 'chatcmpl-fixture', 'object': 'chat.completion.chunk', 'created': int(time.time()), 'model': 'fixture', 'choices': [choice]}
                self.wfile.write(('data: ' + json.dumps(v) + '\n\n').encode())
                self.wfile.flush()
            self.wfile.write(b'data: [DONE]\n\n')
        except (BrokenPipeError, ConnectionResetError): pass


@unittest.skipUnless(os.environ.get('ZERUS_TEST_OPENCODE_NATIVE') == '1', 'explicit private native qualification required')
class OpenCodeNative(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not NATIVE or not HGS.is_file(): raise RuntimeError('Configured native suite requires OpenCode and built hgs')
        version = subprocess.check_output([NATIVE, '--version'], text=True).strip()
        if version != '1.18.35': raise RuntimeError('Native suite requires reviewed OpenCode1.18.35, got ' + version)
        cls.temp = tempfile.TemporaryDirectory(prefix='zerus-opencode-native-')
        cls.root = Path(cls.temp.name)
        cls.workspace = cls.root / 'workspace'; cls.workspace.mkdir()
        cls.home = cls.root / 'home'; cls.home.mkdir()
        cls.config = cls.root / 'config'; cls.config.mkdir()
        cls.native_config = cls.root / 'native-config'; cls.native_config.mkdir()
        cls.state = cls.root / 'state'
        cls.model = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Model)
        cls.model.holding = threading.Event(); cls.model.release = threading.Event()
        cls.thread = threading.Thread(target=cls.model.serve_forever, daemon=True); cls.thread.start()
        native = {'model': 'fixture/local', 'small_model': 'fixture/local', 'share': 'disabled',
                  'permission': {'*': 'ask', 'question':'allow'}, 'provider': {'fixture': {'npm': '@ai-sdk/openai-compatible',
                  'name': 'Private keyless fixture', 'options': {'baseURL': f'http://127.0.0.1:{cls.model.server_port}/v1'},
                  'models': {'local': {'name': 'Native fixture', 'limit': {'context': 32768, 'output': 2048}}, 'alternate': {'name':'Alternate fixture','limit':{'context':32768,'output':2048}}}}}}
        (cls.native_config / 'opencode.json').write_text(json.dumps(native))
        cls.env = {'PATH': os.environ['PATH'], 'LANG': 'C.UTF-8', 'HOME': str(cls.home),
                   'XDG_DATA_HOME': str(cls.root / 'data'), 'XDG_CACHE_HOME': str(cls.root / 'cache'),
                   'XDG_CONFIG_HOME': str(cls.root / 'xdg-config'), 'OPENCODE_CONFIG_DIR': str(cls.native_config),
                   'OPENCODE_DISABLE_AUTOUPDATE': 'true', 'OPENCODE_DISABLE_MODELS_FETCH': 'true',
                   'OPENCODE_DISABLE_LSP': 'true', 'HGS_CONFIG_DIR': str(cls.config),
                   'HGS_STATE_DIR': str(cls.state), 'HGS_PEERS': '', 'HGS_SELF': 'fixture', 'HGS_TAB': '0'}
        cls.receipt = {'version': version, 'fixture': 'owned native OpenCode + keyless local model'}

    @classmethod
    def tearDownClass(cls):
        cls.model.release.set(); cls.model.shutdown(); cls.model.server_close(); cls.thread.join(5)
        # Stop only the native host created inside this unique private state.
        for file in cls.state.glob('opencode/hosts/*/host.json'):
            data = json.loads(file.read_text()); pid = data['pid']
            try:
                os.kill(pid, signal.SIGTERM)
            except ProcessLookupError: pass
        cls.temp.cleanup()

    def command(self, args, payload=None, success=True):
        run = subprocess.run([str(HGS), *args], env=self.env, cwd=self.workspace,
                             input=None if payload is None else json.dumps(payload), text=True, capture_output=True, timeout=50)
        if success: self.assertEqual(run.returncode, 0, run.stderr)
        else: self.assertNotEqual(run.returncode, 0, run.stdout)
        return json.loads(run.stdout) if run.returncode == 0 and run.stdout.strip().startswith(('{', '[')) else run

    def create(self):
        tag = uuid.uuid4().hex[:10]
        self.command(['opencode', str(self.workspace), '--new', '-n', tag, '-d', '--launch-id', str(uuid.uuid4()), '--account', 'native-opencode', '--model', 'fixture/local'])
        return self.command(['inspect', f'opencode/workspace/{tag}'])

    def payload(self, binding, **extra):
        return {'request_id': str(uuid.uuid4()), 'expected_run_id': binding['run_id'],
                'expected_conversation_id': binding['conversation_id'], **extra}

    def wait(self, name, predicate, timeout=20):
        until = time.monotonic() + timeout
        while time.monotonic() < until:
            value = self.command(['inspect', name])
            if predicate(value): return value
            time.sleep(.15)
        self.fail('Native state did not settle: ' + json.dumps(value))

    def api(self, binding, suffix, body=None, method=None):
        data = json.loads(next(self.state.glob('opencode/hosts/*/host.json')).read_text())
        url = f"http://127.0.0.1:{data['port']}/api/session/{binding['conversation_id']}{suffix}"
        headers = {'Authorization': 'Basic ' + base64.b64encode(('opencode:' + data['password']).encode()).decode(), 'Content-Type': 'application/json'}
        req = urllib.request.Request(url, headers=headers, data=None if body is None else json.dumps(body).encode(), method=method)
        with urllib.request.urlopen(req, timeout=30) as response:
            raw = response.read(); return json.loads(raw) if raw else None

    def test_01_catalog_launch_history_exact_identity_and_no_replay(self):
        profiles = self.command(['account', 'ls'])['profiles']
        native = next(p for p in profiles if p['id'] == 'native-opencode')
        self.assertIn('fixture/local', [m['value'] for m in native['model_options']])
        self.assertFalse(self.state.joinpath('opencode/hosts').exists())
        legacy='opencode/workspace/legacy-collision'
        self.state.mkdir(exist_ok=True)
        legacy_file=self.state/(hashlib.sha256(legacy.encode()).hexdigest()+'.json')
        legacy_bytes=json.dumps({'name':legacy,'cmd':'opencode','agent':'opencode','run_id':'retained-terminal','cwd':str(self.workspace)}).encode()
        legacy_file.write_bytes(legacy_bytes)
        collision=self.command(['opencode',str(self.workspace),'--new','-n','legacy-collision','-d','--account','native-opencode','--model','fixture/local'],success=False)
        self.assertIn('existing terminal session',collision.stderr)
        self.assertEqual(legacy_file.read_bytes(),legacy_bytes)
        self.assertFalse(self.state.joinpath('opencode/hosts').exists())
        b = self.create(); name = b['name']
        self.assertFalse(b['terminal_supported']); self.assertFalse(b['compact_context_supported'])
        for field in ['expected_run_id', 'expected_conversation_id']:
            self.command(['send', name, '--json'], {**self.payload(b, text='wrong-target'), field: 'wrong'}, False)
        request = self.payload(b, text='Literal\\\nmessage')
        first = self.command(['send', name, '--json'], request)
        self.assertEqual(first['status'], 'submitted')
        self.assertEqual(self.command(['send', name, '--json'], request), first)
        self.command(['send', name, '--json'], {**request, 'text': 'changed'}, False)
        done = self.wait(name, lambda v: v['phase'] == 'idle' and v.get('reply_id'))
        self.assertEqual([e['detail'] for e in done['events'] if e['type'] == 'UserPromptSubmit'], ['Literal\\\nmessage'])
        self.assertTrue(any(e['detail'] == 'Native OpenCode reply' for e in done['events']))
        page = self.command(['history', name, '--json'], self.payload(b, limit=100))
        self.assertTrue(page['head']['complete'])
        self.assertFalse(done['native_ui_available'])
        self.assertNotIn('password', json.dumps(done))
        # Simulate loss of the native response after the real durable admission.
        receipt = self.state / 'opencode/receipts' / (request['request_id'] + '.json')
        pending = json.loads(receipt.read_text()); pending['status'] = 'pending'; receipt.write_text(json.dumps(pending))
        reconciled = self.command(['send', name, '--json'], request)
        self.assertEqual(reconciled['status'], 'submitted')
        final = self.command(['inspect', name])
        self.assertEqual(len([e for e in final['events'] if e['type'] == 'UserPromptSubmit']), 1)

    def test_02_authoritative_working_interrupt_pause_resume_and_rename(self):
        b = self.create(); name = b['name']; self.model.holding.clear(); self.model.release.clear()
        self.command(['send', name, '--json'], self.payload(b, text='HOLD_NATIVE'))
        self.assertTrue(self.model.holding.wait(10))
        working = self.wait(name, lambda v: v['phase'] == 'working' and v['interrupt_supported'])
        self.command(['pause', name], success=False)
        self.command(['send', name, '--json'], self.payload(b, text='busy-refused'), False)
        self.command(['interrupt', name, '--json'], self.payload(b, expected_turn_started=working['turn_started'] + 1), False)
        self.command(['interrupt', name, '--json'], self.payload(b, expected_turn_started=working['turn_started']))
        self.model.release.set()
        self.wait(name, lambda v: v['phase'] != 'working')
        self.command(['session-action', name, '--json'], self.payload(b, action='pause'))
        self.assertEqual(self.command(['inspect', name])['state'], 'paused')
        self.command(['session-action', name, '--json'], self.payload(b, action='resume'))
        resumed = self.command(['inspect', name]); self.assertEqual(resumed['conversation_id'], b['conversation_id']); self.assertNotEqual(resumed['run_id'], b['run_id'])
        self.command(['send', name, '--json'], self.payload(b, text='old-run'), False)
        new_name = name + '-renamed'
        self.command(['session-action', name, '--json'], self.payload(resumed, action='rename', new_name=new_name))
        self.assertEqual(self.command(['inspect', new_name])['conversation_id'], b['conversation_id'])

    def test_03_native_permission_exact_answer(self):
        b = self.create(); name = b['name']; result = []
        def ask(): result.append(self.api(b, '/permission', {'id': 'per_' + uuid.uuid4().hex, 'action': 'bash', 'resources': ['fixture-command']}))
        thread = threading.Thread(target=ask); thread.start()
        data = self.wait(name, lambda v: v['pending_questions'])
        card = data['pending_questions'][0]
        self.assertTrue(card['can_answer']); self.assertTrue(card['approval'])
        request = self.payload(b, question_id=card['question_id'], expected_question_hash=card['question_hash'],
                               answers=[{'question_id': 'permission', 'selected_option_ids': ['once'], 'text': ''}])
        self.command(['answer', name, '--json'], {**request, 'expected_question_hash': 'changed'}, False)
        self.assertEqual(self.command(['answer', name, '--json'], request)['status'], 'answered')
        thread.join(10); self.assertFalse(thread.is_alive()); self.assertTrue(result)
        self.command(['answer', name, '--json'], request, False)

    def test_04_native_question_and_real_tool_state(self):
        b=self.create();name=b['name'];self.command(['send',name,'--json'],self.payload(b,text='QUESTION_NATIVE'))
        data=self.wait(name,lambda v:any(not q['approval'] for q in v['pending_questions']))
        card=next(q for q in data['pending_questions'] if not q['approval'])
        request=self.payload(b,question_id=card['question_id'],expected_question_hash=card['question_hash'],answers=[{'question_id':'0','selected_option_ids':['0'],'text':''}])
        self.command(['answer',name,'--json'],request)
        final=self.wait(name,lambda v:v['phase']=='idle' and v.get('reply_id'))
        self.assertTrue(any(e['type']=='PreToolUse' and e['tool']=='question' for e in final['events']))
        self.assertTrue(any(e['type']=='PostToolUse' for e in final['events']))
        tool=self.create();self.command(['send',tool['name'],'--json'],self.payload(tool,text='BASH_NATIVE'))
        data=self.wait(tool['name'],lambda v:any(q['approval'] for q in v['pending_questions']))
        card=data['pending_questions'][0]
        self.command(['answer',tool['name'],'--json'],self.payload(tool,question_id=card['question_id'],expected_question_hash=card['question_hash'],answers=[{'question_id':'permission','selected_option_ids':['once'],'text':''}]))
        final=self.wait(tool['name'],lambda v:v['phase']=='idle' and v.get('reply_id'))
        self.assertTrue(any(e['type']=='PostToolUse' and 'native-tool-output' in e['detail'] for e in final['events']))

    def test_05_model_change_native_error_and_read_marks(self):
        b=self.create();name=b['name']
        self.command(['settings',name,'--json'],self.payload(b,model='fixture/alternate'))
        self.assertEqual(self.command(['inspect',name])['settings_model'],'fixture/alternate')
        self.command(['send',name,'--json'],self.payload(b,text='ERROR_NATIVE'))
        error=self.wait(name,lambda v:v['phase']=='error')
        self.assertEqual(error['activity'],'attention')
        self.assertIn('Fixture provider rejected',json.dumps(error))
        self.assertTrue(error.get('attention_id'))
        self.assertNotIn('password',json.dumps(error))

    def test_06_voice_transcript_delivers_once_and_audio_stays_in_history(self):
        from test_speech import Server, load
        import fcntl
        b=self.create();name=b['name']
        settings=self.config/'speech.json'
        settings.write_text(json.dumps({'schema':1,'state_dir':str(self.root/'asr'),'profiles':[{'id':'fixture','name':'Fixture','adapter':'fixture','model':'fixture','language':'ru','text':'Voice native fixture transcript'}]}))
        server=Server(load(settings),test_mode=True);thread=threading.Thread(target=server.serve_forever,daemon=True);thread.start()
        log=(self.root/'voice-worker.log').open('ab')
        worker=subprocess.Popen([str(HGS),'voice','worker'],env=self.env,stdin=subprocess.DEVNULL,stdout=subprocess.DEVNULL,stderr=log)
        try:
            for _ in range(60):
                path=self.state/'voice/worker.lock'
                if path.exists():
                    with path.open('rb') as stream:
                        try:fcntl.flock(stream,fcntl.LOCK_EX|fcntl.LOCK_NB)
                        except BlockingIOError:break
                time.sleep(.05)
            else:self.fail('Owned voice worker did not obtain its private lock')
            machine=self.command(['swarm','__mobile-peer-identity','--json'])['machine_id']
            payload=self.payload(b,action='submit',speech={'machine_id':machine,'profile_id':'fixture'},audio={'format':'pcm_s16le','sample_rate':16000,'channels':1,'data_base64':base64.b64encode(b'\x10\x01'*16000).decode()})
            accepted=self.command(['voice',name,'--json'],payload)['message']
            until=time.monotonic()+20
            while time.monotonic()<until:
                result=self.command(['voice',name,'--json'],self.payload(b,action='status',message_id=accepted['id']))['message']
                if result['state']=='submitted':break
                time.sleep(.2)
            else:self.fail('Native voice failed: '+json.dumps(result)+(self.root/'voice-worker.log').read_text())
            self.assertEqual(result['transcript'],'Voice native fixture transcript')
            self.assertEqual(self.command(['voice',name,'--json'],payload)['message']['id'],accepted['id'])
            final=self.wait(name,lambda v:v['phase']=='idle' and v.get('reply_id'))
            self.assertEqual(len([e for e in final['events'] if e['type']=='UserPromptSubmit' and e['detail']=='Voice native fixture transcript']),1)
            self.assertTrue(final['voice_messages'])
            page=self.command(['history',name,'--json'],self.payload(b,limit=100))
            self.assertTrue(page['head']['complete'])
            self.assertTrue(any(e.get('voice') or e.get('type')=='VoiceMessage' for e in page['events']),page)
            audio=self.command(['attachment',name,'--request',accepted['id'],'--index','0','--conversation',b['conversation_id']])
            self.assertTrue(audio['data_base64'])
        finally:
            worker.terminate();worker.wait(10);log.close();server.shutdown();server.server_close();thread.join(5)

    def test_07_corrupt_or_hung_metadata_never_replaces_a_resident_host(self):
        b=self.create();file=next(self.state.glob('opencode/hosts/*/host.json'));original=file.read_text();metadata=json.loads(original)
        try:
            bad={**metadata,'version':'unreviewed'};file.write_text(json.dumps(bad))
            failure=self.command(['resume',b['name'],'-d'],success=False)
            self.assertIn('Invalid private OpenCode host metadata',failure.stderr)
            self.assertEqual(json.loads(file.read_text()),bad)
            file.write_text(original)
            for missing in ['pid','process_start']:
                bad={k:v for k,v in metadata.items() if k!=missing};file.write_text(json.dumps(bad))
                failure=self.command(['resume',b['name'],'-d'],success=False)
                self.assertIn('Invalid private OpenCode host metadata',failure.stderr)
                self.assertEqual(json.loads(file.read_text()),bad)
            file.write_text(original)
            with __import__('socket').socket() as sock:
                sock.bind(('127.0.0.1',0));port=sock.getsockname()[1]
            bad={**metadata,'port':port};file.write_text(json.dumps(bad))
            failure=self.command(['resume',b['name'],'-d'],success=False)
            self.assertIn('left running',failure.stderr)
            self.assertEqual(json.loads(file.read_text()),bad)
            file.write_text(original)
            self.assertEqual(self.command(['inspect',b['name']])['conversation_id'],b['conversation_id'])
        finally:file.write_text(original)

    def test_08_lost_http_response_reconciles_by_get_and_never_reposts(self):
        b=self.create();name=b['name'];file=next(self.state.glob('opencode/hosts/*/host.json'));original=file.read_text();native=json.loads(original)
        class Proxy(http.server.BaseHTTPRequestHandler):
            submissions=0
            def log_message(self,*_):pass
            def do_GET(self):self.forward()
            def do_POST(self):self.forward()
            def forward(self):
                body=self.rfile.read(int(self.headers.get('Content-Length',0)))
                req=urllib.request.Request(f"http://127.0.0.1:{native['port']}"+self.path,headers={'Authorization':self.headers['Authorization'],'Content-Type':'application/json'},data=body if self.command=='POST' else None,method=self.command)
                with urllib.request.urlopen(req,timeout=15) as result:
                    data=result.read();status=result.status
                if self.command=='POST' and self.path.endswith('/prompt'):
                    Proxy.submissions+=1;self.close_connection=True;return
                self.send_response(status);self.send_header('Content-Type','application/json');self.send_header('Content-Length',str(len(data)));self.end_headers();self.wfile.write(data)
        proxy=http.server.ThreadingHTTPServer(('127.0.0.1',0),Proxy);thread=threading.Thread(target=proxy.serve_forever,daemon=True);thread.start()
        try:
            file.write_text(json.dumps({**native,'port':proxy.server_port}))
            request=self.payload(b,text='Lost response exactly once')
            self.command(['send',name,'--json'],request,False)
            receipt=self.state/'opencode/receipts'/(request['request_id']+'.json')
            self.assertEqual(json.loads(receipt.read_text())['status'],'pending')
            self.assertEqual(self.command(['send',name,'--json'],request)['status'],'submitted')
            self.assertEqual(Proxy.submissions,1)
            final=self.wait(name,lambda v:v['phase']=='idle' and v.get('reply_id'))
            self.assertEqual(len([e for e in final['events'] if e['type']=='UserPromptSubmit']),1)
            # Unknown admission remains uncertain; no attempt is authorized by
            # merely supplying the old UUID again.
            unknown=self.payload(b,text='Never admitted')
            pending={'request_id':unknown['request_id'],'name':name,'run_id':b['run_id'],'conversation_id':b['conversation_id'],'text':unknown['text'],'status':'pending','native_message_id':'msg_'+unknown['request_id'].replace('-',''),'digest':hashlib.sha256(json.dumps([name,unknown],sort_keys=True,separators=(',',':')).encode()).hexdigest()}
            (self.state/'opencode/receipts'/(unknown['request_id']+'.json')).write_text(json.dumps(pending))
            self.command(['send',name,'--json'],unknown,False)
            self.assertEqual(Proxy.submissions,1)
        finally:
            file.write_text(original);proxy.shutdown();proxy.server_close();thread.join(5)
