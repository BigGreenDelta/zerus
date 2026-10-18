"""Opt-in OpenCode 2 qualification: private homes, a private service port and a keyless model.

ZERUS_TEST_OPENCODE_NATIVE=1 makes a missing or unbuilt hgs, a missing OpenCode, or an
OpenCode other than 2.x at or above the reviewed minimum a failure; without it the suite
is skipped. ZERUS_TEST_OPENCODE_BIN names the OpenCode executable (default: `opencode`
on PATH); its folder leads hgs's PATH, so hgs starts that executable.

Every OpenCode and hgs command runs with private HOME and XDG directories, a private
service port and an offline catalog, so the developer's own OpenCode service, settings
and sessions are never touched. Each test stops its private service; the suite then
verifies that no process still carries its private root. No live agents, credentials or
inferencing services are used.
"""
import base64
import hashlib
import http.server
import json
import os
from pathlib import Path
import random
import re
import shutil
import signal
import socket
import string
import subprocess
import tempfile
import threading
import time
import unittest
import urllib.error
import urllib.request
import uuid

REPO = Path(__file__).resolve().parents[1]
HGS = Path(os.environ.get('HGS_TEST_BIN', REPO / 'target/debug/hgs'))
NATIVE = os.environ.get('ZERUS_TEST_OPENCODE_BIN') or shutil.which('opencode') or ''
MINIMUM = (2, 0, 32)
# OpenCode's default service port; the suite never uses it.
DEFAULT_PORT = 49374
# A fixture key in the private configuration; it must never appear in hgs output.
FIXTURE_KEY = 'fixture-key-never-shown'
NOT_RUNNING = 'OpenCode is not running'
OPENCODE1 = 'This conversation belongs to OpenCode 1, which Zerus no longer supports'
REMOVED = 'Removed from the OpenCode queue before delivery'


def free_port():
    with socket.socket() as probe:
        probe.bind(('127.0.0.1', 0))
        return probe.getsockname()[1]


def native_suffix():
    return ''.join(random.choices(string.ascii_letters + string.digits, k=26))


def owned_processes(marker):
    """(pid, command) of every other process whose command line or environment holds
    `marker`: the private root, which only processes this suite started carry."""
    found, needle = [], marker.encode()
    proc = Path('/proc')
    if proc.is_dir():
        for entry in proc.iterdir():
            if not entry.name.isdigit() or int(entry.name) == os.getpid():
                continue
            try:
                command = (entry / 'cmdline').read_bytes()
                if needle not in command and needle not in (entry / 'environ').read_bytes():
                    continue
            except OSError:
                continue
            found.append((int(entry.name), command.replace(b'\0', b' ').decode(errors='replace').strip()))
        return found
    # macOS: BSD ps shows the environment of the user's own processes with -E.
    listing = subprocess.run(['ps', '-E', '-ww', '-x', '-o', 'pid=,command='], capture_output=True,
                             text=True, errors='replace', timeout=30)
    if listing.returncode:
        raise RuntimeError('Cannot list processes: ' + listing.stderr[-500:])
    for line in listing.stdout.splitlines():
        pid, _, command = line.strip().partition(' ')
        if pid.isdigit() and int(pid) != os.getpid() and marker in command:
            found.append((int(pid), command[:300]))
    return found


class Model(http.server.BaseHTTPRequestHandler):
    """A keyless OpenAI-compatible model. A marker in the newest user message selects
    the reply: HOLD_NATIVE holds the turn open, QUESTION_NATIVE calls OpenCode 2's
    `question` tool, BASH_NATIVE its `shell` tool and ERROR_NATIVE answers HTTP 401."""

    def log_message(self, *_):
        pass

    def do_GET(self):
        body = json.dumps({'object': 'list', 'data': [{'id': 'local', 'object': 'model'},
                                                      {'id': 'alternate', 'object': 'model'}]}).encode()
        self.send_response(200)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
        messages = body.get('messages', [])
        tools = [tool.get('function', {}).get('name') for tool in body.get('tools', [])]
        last_user = next((str(m.get('content', '')) for m in reversed(messages) if m.get('role') == 'user'), '')
        self.server.requests.append({'model': body.get('model'), 'tools': tools, 'last_user': last_user[:200],
                                     'options': {k: v for k, v in body.items() if k not in ('messages', 'tools')}})
        if 'ERROR_NATIVE' in last_user:
            error = json.dumps({'error': {'message': 'Fixture provider rejected', 'code': 'invalid_api_key'}}).encode()
            self.send_response(401)
            self.send_header('Content-Type', 'application/json')
            self.send_header('Content-Length', str(len(error)))
            self.end_headers()
            self.wfile.write(error)
            return
        tool = None
        # Only an agent turn offers tools; a turn that just received a tool result replies.
        if tools and not any(m.get('role') == 'tool' for m in messages[-2:]):
            if 'HOLD_NATIVE' in last_user:
                self.server.holding.set()
                self.server.release.wait(60)
            elif 'QUESTION_NATIVE' in last_user:
                tool = ('question', {'questions': [{'question': 'Pick a color', 'header': 'Color', 'options': [
                    {'label': 'Green', 'description': 'First'}, {'label': 'Blue', 'description': 'Second'}]}]})
            elif 'BASH_NATIVE' in last_user:
                tool = ('shell', {'command': 'printf native-tool-output', 'description': 'Print isolated fixture output'})
        if tool:
            call = {'index': 0, 'id': 'call_' + uuid.uuid4().hex[:12], 'type': 'function',
                    'function': {'name': tool[0], 'arguments': json.dumps(tool[1])}}
            choices = [{'index': 0, 'delta': {'tool_calls': [call]}, 'finish_reason': None},
                       {'index': 0, 'delta': {}, 'finish_reason': 'tool_calls'}]
        else:
            choices = [{'index': 0, 'delta': {'content': 'Native OpenCode reply'}, 'finish_reason': None},
                       {'index': 0, 'delta': {}, 'finish_reason': 'stop'}]
        try:
            self.send_response(200)
            self.send_header('Content-Type', 'text/event-stream')
            self.end_headers()
            for choice in choices:
                chunk = {'id': 'chatcmpl-fixture', 'object': 'chat.completion.chunk', 'created': int(time.time()),
                         'model': body.get('model', 'fixture'), 'choices': [choice]}
                self.wfile.write(('data: ' + json.dumps(chunk) + '\n\n').encode())
                self.wfile.flush()
            self.wfile.write(b'data: [DONE]\n\n')
        except (BrokenPipeError, ConnectionResetError):
            pass


def stub_service(version, pid, openapi=None):
    """A loopback HTTP stub that answers like an OpenCode service of `version`."""
    class Stub(http.server.BaseHTTPRequestHandler):
        seen = []

        def log_message(self, *_):
            pass

        def answer(self, status, value):
            body = json.dumps(value).encode()
            self.send_response(status)
            self.send_header('Content-Type', 'application/json')
            self.send_header('Content-Length', str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def do_GET(self):
            Stub.seen.append(('GET', self.path))
            if self.path == '/api/info':
                self.answer(200, {'version': version, 'pid': pid, 'urls': []})
            elif self.path == '/openapi.json' and openapi is not None:
                self.answer(200, openapi)
            else:
                self.answer(404, {'message': 'Not found'})

        def other(self):
            Stub.seen.append((self.command, self.path))
            self.answer(404, {'message': 'Not found'})
        do_POST = do_PATCH = do_DELETE = do_PUT = other

    server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Stub)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    return server, Stub.seen


@unittest.skipUnless(os.environ.get('ZERUS_TEST_OPENCODE_NATIVE') == '1', 'explicit private native qualification required')
class OpenCodeNative(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not NATIVE or not Path(NATIVE).is_file() or not HGS.is_file():
            raise RuntimeError('The opted-in OpenCode suite requires OpenCode 2 and a built hgs')
        cls.temp = tempfile.TemporaryDirectory(prefix='zerus-opencode-native-')
        cls.root = Path(cls.temp.name)
        cls.model = None
        try:
            cls.prepare()
        except BaseException:
            cls.cleanup()
            raise

    @classmethod
    def prepare(cls):
        root = cls.root
        # Registration files this suite wrote itself, byte for byte.
        cls.fake_registrations = set()
        cls.workspace = root / 'workspace'
        cls.state = root / 'hgs-state'
        for path in ['home', 'xdg-config/opencode', 'xdg-data', 'xdg-cache', 'xdg-state', 'workspace',
                     'hgs-config', 'hgs-state']:
            (root / path).mkdir(parents=True)
        # hgs lists tmux sessions too; an existing private TMUX_TMPDIR keeps it off the
        # user's default server, where a missing directory would fall back to it.
        (root / 'tmux').mkdir(mode=0o700)
        cls.model = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Model)
        cls.model.holding, cls.model.release, cls.model.requests = threading.Event(), threading.Event(), []
        threading.Thread(target=cls.model.serve_forever, daemon=True).start()
        native = Path(NATIVE).absolute()
        cls.env = {'PATH': os.pathsep.join([str(native.parent), os.environ.get('PATH', '')]), 'LANG': 'C.UTF-8',
                   'HOME': str(root / 'home'), 'XDG_CONFIG_HOME': str(root / 'xdg-config'),
                   'XDG_DATA_HOME': str(root / 'xdg-data'), 'XDG_CACHE_HOME': str(root / 'xdg-cache'),
                   'XDG_STATE_HOME': str(root / 'xdg-state'), 'OPENCODE_DISABLE_MODELS_FETCH': 'true',
                   'OPENCODE_DISABLE_AUTOUPDATE': 'true', 'OPENCODE_DISABLE_LSP': 'true',
                   'HGS_CONFIG_DIR': str(root / 'hgs-config'), 'HGS_STATE_DIR': str(cls.state),
                   'HGS_PEERS': '', 'HGS_SELF': 'fixture', 'HGS_TAB': '0', 'TMUX_TMPDIR': str(root / 'tmux')}
        found = shutil.which('opencode', path=cls.env['PATH'])
        if not found or not os.path.samefile(found, native):
            raise RuntimeError('hgs would not start the OpenCode under test: ' + str(found))
        version = cls.opencode('--version').stdout.strip()
        match = re.fullmatch(r'(?:opencode )?v?(\d+)\.(\d+)\.(\d+)', version)
        parts = tuple(int(part) for part in match.groups()) if match else None
        if not parts or parts[0] != MINIMUM[0] or parts < MINIMUM:
            raise RuntimeError('The OpenCode suite requires OpenCode %d.%d.%d or newer 2.x, got %r' % (*MINIMUM, version))
        cls.version = '%d.%d.%d' % parts
        config = {'$schema': 'https://opencode.ai/config.json', 'model': 'fixture/local', 'small_model': 'fixture/local',
                  'permission': {'*': 'ask', 'question': 'allow', 'read': 'allow'},
                  'provider': {'fixture': {'npm': '@ai-sdk/openai-compatible', 'name': 'Private keyless fixture',
                                           'options': {'baseURL': f'http://127.0.0.1:{cls.model.server_port}/v1',
                                                       'apiKey': FIXTURE_KEY},
                                           'models': {'local': {'name': 'Native fixture', 'limit': {'context': 32768, 'output': 2048}},
                                                      'alternate': {'name': 'Alternate fixture',
                                                                    'limit': {'context': 32768, 'output': 2048}}}}}}
        (root / 'xdg-config/opencode/opencode.json').write_text(json.dumps(config))
        # A private service port before anything can start the service.
        cls.port = free_port()
        while cls.port == DEFAULT_PORT:
            cls.port = free_port()
        cls.opencode('service', 'set', 'port', str(cls.port))
        configured = json.loads((root / 'xdg-config/opencode/service.json').read_text())
        if configured.get('port') != cls.port:
            raise RuntimeError('OpenCode did not record the private service port')

    @classmethod
    def opencode(cls, *args):
        """An OpenCode command, always under the private environment."""
        run = subprocess.run([NATIVE, *args], env=cls.env, cwd=cls.workspace, stdin=subprocess.DEVNULL,
                             capture_output=True, text=True, timeout=60)
        if run.returncode:
            raise RuntimeError('opencode %s failed: %s' % (' '.join(args), (run.stderr or run.stdout)[-800:]))
        return run

    @classmethod
    def registration_path(cls):
        return cls.root / 'xdg-state/opencode/service.json'

    @classmethod
    def native_processes(cls):
        return [process for process in owned_processes(str(cls.root)) if 'opencode' in process[1]]

    @classmethod
    def stop_service(cls):
        """Stop the service OpenCode registered privately and wait until no process
        carries the private root. A registration this suite wrote itself (a stub, a hung
        port or an exited process) is deleted, never handed to `opencode service stop`."""
        registration = cls.registration_path()
        try:
            written = registration.read_bytes()
        except OSError:
            written = None
        if written in cls.fake_registrations:
            registration.unlink()
        elif written is not None:
            cls.opencode('service', 'stop')
        until = time.monotonic() + 15
        while owned_processes(str(cls.root)) and time.monotonic() < until:
            time.sleep(.2)

    @classmethod
    def reap(cls):
        """Stop whatever still carries the private root, only this suite's processes;
        returns what was found."""
        leftovers = owned_processes(str(cls.root))
        for sig in (signal.SIGTERM, signal.SIGKILL) if leftovers else ():
            for pid, _ in owned_processes(str(cls.root)):
                try:
                    os.kill(pid, sig)
                except ProcessLookupError:
                    pass
            time.sleep(1)
        return leftovers

    @classmethod
    def cleanup(cls):
        try:
            if cls.model:
                cls.model.release.set()
            if hasattr(cls, 'env'):
                cls.stop_service()
        finally:
            leftovers = cls.reap()
            if cls.model:
                cls.model.shutdown()
                cls.model.server_close()
            cls.temp.cleanup()
        if leftovers:
            raise RuntimeError('Processes outlived the private OpenCode service: ' + json.dumps(leftovers))

    @classmethod
    def tearDownClass(cls):
        cls.cleanup()

    def setUp(self):
        self.model.holding.clear()
        self.model.release.clear()
        self.model.requests.clear()
        self.addCleanup(self.stop_and_check)
        self.addCleanup(self.model.release.set)

    def stop_and_check(self):
        """Each test ends with its private service stopped and nothing left behind, so a
        leak is reported by the test that caused it."""
        self.stop_service()
        self.assertEqual(self.reap(), [], 'processes outlived this test')

    def hgs(self, args, payload=None, success=True):
        run = subprocess.run([str(HGS), *args], env=self.env, cwd=self.workspace, text=True, capture_output=True,
                             input=None if payload is None else json.dumps(payload), timeout=60)
        if success:
            self.assertEqual(run.returncode, 0, run.stderr)
        else:
            self.assertNotEqual(run.returncode, 0, run.stdout)
        return json.loads(run.stdout) if run.returncode == 0 and run.stdout.strip().startswith(('{', '[')) else run

    def launch(self, tag, *options, success=True):
        return self.hgs(['opencode', str(self.workspace), '--new', '-n', tag, '-d', '--launch-id', str(uuid.uuid4()),
                         '--account', 'native-opencode', *options], success=success)

    def create(self, *options):
        tag = uuid.uuid4().hex[:10]
        self.launch(tag, '--model', 'fixture/local', *options)
        return self.hgs(['inspect', f'opencode/workspace/{tag}'])

    def payload(self, binding, **extra):
        return {'request_id': str(uuid.uuid4()), 'expected_run_id': binding['run_id'],
                'expected_conversation_id': binding['conversation_id'], **extra}

    def action(self, binding, action, status='completed', **extra):
        """A scoped session action; its receipt, not the exit status, reports a refusal."""
        receipt = self.hgs(['session-action', binding['name'], '--json'], {**self.payload(binding, action=action), **extra})
        self.assertEqual(receipt['status'], status, receipt)
        return receipt

    def wait(self, name, predicate, timeout=40):
        until = time.monotonic() + timeout
        while True:
            value = self.hgs(['inspect', name])
            if predicate(value):
                return value
            if time.monotonic() > until:
                self.fail('Native state did not settle: ' + json.dumps(value)[-3000:])
            time.sleep(.25)

    def binding_path(self, name):
        return self.state / 'opencode/bindings' / (hashlib.sha256(name.encode()).hexdigest() + '.json')

    def save_binding(self, tag, **fields):
        """A binding written as an earlier hgs would have left it."""
        name = 'opencode/workspace/' + tag
        record = {'backend': 'opencode', 'agent': 'opencode', 'name': name, 'cwd': os.path.realpath(self.workspace),
                  'run_id': str(uuid.uuid4()), 'conversation_id': 'ses_' + native_suffix(), 'created': time.time(),
                  'launch_id': None, 'agent_home': str(self.root / 'xdg-config/opencode'), **fields}
        path = self.binding_path(name)
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(record))
        return record

    def receipt_path(self, request):
        return self.state / 'opencode/receipts' / (request['request_id'] + '.json')

    def registration(self):
        return json.loads(self.registration_path().read_text())

    def write_registration(self, value):
        path = self.registration_path()
        path.parent.mkdir(parents=True, exist_ok=True)
        written = json.dumps(value).encode()
        self.fake_registrations.add(written)
        descriptor = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
        with os.fdopen(descriptor, 'wb') as file:
            file.write(written)
        return written

    def api(self, method, path, body=None):
        """This test's own request to its private service, authenticated as hgs is."""
        registration = self.registration()
        headers = {'Content-Type': 'application/json'}
        if registration.get('password'):
            token = base64.b64encode(('opencode:' + registration['password']).encode()).decode()
            headers['Authorization'] = 'Basic ' + token
        request = urllib.request.Request(registration['url'].rstrip('/') + path, method=method, headers=headers,
                                         data=None if body is None else json.dumps(body).encode())
        try:
            with urllib.request.urlopen(request, timeout=30) as response:
                raw = response.read()
                return response.status, json.loads(raw) if raw else None
        except urllib.error.HTTPError as error:
            with error:
                raw = error.read()
            return error.code, json.loads(raw) if raw else None

    def session(self, conversation_id):
        status, answer = self.api('GET', '/api/session/' + conversation_id)
        self.assertEqual(status, 200, answer)
        return answer['data']

    def user_messages(self, conversation_id):
        status, page = self.api('GET', f'/api/session/{conversation_id}/message?limit=100')
        self.assertEqual(status, 200, page)
        return [(m['id'], m.get('text')) for m in page['data'] if m.get('type') == 'user']

    def inbox(self, conversation_id):
        status, answer = self.api('GET', f'/api/session/{conversation_id}/inbox')
        self.assertEqual(status, 200, answer)
        return answer['data']

    def listing(self):
        rows = self.hgs(['ls', '--local', '--json', '--no-archives'])['sessions']
        return {row['name']: row for row in rows if row.get('backend') == 'opencode'}

    def account(self):
        profiles = self.hgs(['account', 'ls'])['profiles']
        return next(profile for profile in profiles if profile['id'] == 'native-opencode')

    def assert_private(self, *values):
        text = json.dumps(values)
        self.assertNotIn(FIXTURE_KEY, text)
        if self.registration_path().exists():
            registration = self.registration()
            if registration.get('password'):
                self.assertNotIn(registration['password'], text)

    def test_01_not_running_reports_unknown_and_never_starts(self):
        self.stop_service()
        self.assertFalse(self.registration_path().exists())
        binding = self.save_binding('saved-' + uuid.uuid4().hex[:8], service='opencode2')
        name = binding['name']
        row = self.hgs(['inspect', name])
        self.assertTrue(row['inspection_unavailable'])
        self.assertIn(NOT_RUNNING, row['inspection_reason'])
        # Native state stays unknown, never stopped; clients keep the transcript they show.
        self.assertEqual((row['state'], row['phase'], row['activity']), ('unknown', 'unknown', 'unknown'))
        self.assertEqual(row['allowed_actions'], ['forget'])
        self.assertNotIn('events', row)
        self.assertFalse(row['send_supported'])
        listed = self.listing()[name]
        self.assertEqual((listed['state'], listed['phase']), ('unknown', 'unknown'))
        self.assertIn(NOT_RUNNING, listed['inspection_reason'])
        account = self.account()
        self.assertEqual(account['catalog_state'], 'not_running')
        self.assertIn(NOT_RUNNING, account['catalog_reason'])
        # Sending is explicit, but it never starts the service either.
        request = self.payload(binding, text='never sent')
        failure = self.hgs(['send', name, '--json'], request, success=False)
        self.assertIn(NOT_RUNNING, failure.stderr)
        self.assertFalse(self.receipt_path(request).exists())
        self.assertFalse(self.registration_path().exists())
        self.assertEqual(self.native_processes(), [])

    def test_02_launch_starts_service_with_own_ids_and_history(self):
        self.stop_service()
        # A registration left by a service that is gone does not block the start.
        gone = subprocess.Popen(['true'])
        gone.wait()
        self.write_registration({'id': 'svc_gone', 'version': self.version, 'url': f'http://127.0.0.1:{self.port}',
                                 'pid': gone.pid, 'password': 'gone-password'})
        tag, launch_id = uuid.uuid4().hex[:10], str(uuid.uuid4())
        self.hgs(['opencode', str(self.workspace), '--new', '-n', tag, '-d', '--launch-id', launch_id,
                  '--account', 'native-opencode', '--model', 'fixture/local'])
        registration = self.registration()
        self.assertNotEqual(registration['pid'], gone.pid)
        os.kill(registration['pid'], 0)
        self.assertEqual(registration['url'].rstrip('/'), f'http://127.0.0.1:{self.port}')
        self.assertTrue(self.native_processes())
        name = 'opencode/workspace/' + tag
        b = self.hgs(['inspect', name])
        self.assertRegex(b['conversation_id'], r'^ses_[A-Za-z0-9]{26}$')
        self.assertEqual(b['launch_id'], launch_id)
        self.assertFalse(b['terminal_supported'])
        self.assertFalse(b['native_ui_available'])
        self.assertEqual((b['state'], b['phase'], b['settings_model']), ('running', 'idle', 'fixture/local'))
        session = self.session(b['conversation_id'])
        self.assertEqual(session['metadata']['zerus'], {'run_id': b['run_id'], 'launch_id': launch_id})
        self.assertEqual(session['location']['directory'], os.path.realpath(self.workspace))
        self.assertEqual(session['location']['directory'], b['cwd'])
        self.assertEqual((session['model']['providerID'], session['model']['id'], session['title']), ('fixture', 'local', tag))
        request = self.payload(b, text='hello')
        first = self.hgs(['send', name, '--json'], request)
        self.assertEqual(first['status'], 'submitted')
        self.assertRegex(first['native_message_id'], r'^msg_[A-Za-z0-9]{26}$')
        # The same request ID returns the same receipt; other text under it is refused.
        self.assertEqual(self.hgs(['send', name, '--json'], request), first)
        self.hgs(['send', name, '--json'], {**request, 'text': 'changed'}, success=False)
        for field in ['expected_run_id', 'expected_conversation_id']:
            self.hgs(['send', name, '--json'], {**self.payload(b, text='wrong target'), field: 'wrong'}, success=False)
        done = self.wait(name, lambda v: v['phase'] == 'idle' and v.get('reply_id'))
        events = [(e['type'], e['detail']) for e in done['events']]
        self.assertEqual([detail for kind, detail in events if kind == 'UserPromptSubmit'], ['hello'])
        prompt = events.index(('UserPromptSubmit', 'hello'))
        reply = events.index(('AgentMessage', 'Native OpenCode reply'))
        stop = next(i for i, (kind, _) in enumerate(events) if kind == 'Stop')
        self.assertLess(prompt, reply)
        self.assertLess(reply, stop)
        page = self.hgs(['history', name, '--json'], self.payload(b, limit=100))
        self.assertTrue(page['head']['complete'])
        self.assertTrue(any(e.get('detail') == 'Native OpenCode reply' for e in page['events']), page)
        self.assert_private(b, first, done, page)
        # The response to an admitted message is lost: the same request finds it, once.
        receipt = json.loads(self.receipt_path(request).read_text())
        receipt['status'] = 'pending'
        self.receipt_path(request).write_text(json.dumps(receipt))
        self.assertEqual(self.hgs(['send', name, '--json'], request)['status'], 'submitted')
        self.assertEqual(self.user_messages(b['conversation_id']), [(first['native_message_id'], 'hello')])
        # The response to a launch is lost: the journaled session ID is created once.
        lost_tag, lost_id = uuid.uuid4().hex[:10], 'ses_' + native_suffix()
        run_id, cwd = str(uuid.uuid4()), os.path.realpath(self.workspace)
        body = {'id': lost_id, 'location': {'directory': cwd}, 'title': lost_tag,
                'metadata': {'zerus': {'run_id': run_id, 'launch_id': None}}, 'model': {'providerID': 'fixture', 'id': 'local'}}
        self.save_binding(lost_tag, service='opencode2', run_id=run_id, conversation_id=lost_id, creation_pending=True,
                          creation_body=body)
        self.assertEqual(self.api('GET', '/api/session/' + lost_id)[0], 404)
        for _ in range(2):
            self.hgs(['opencode', str(self.workspace), '-n', lost_tag, '-d', '--account', 'native-opencode'])
        created = self.session(lost_id)
        self.assertEqual((created['title'], created['metadata']['zerus']['run_id']), (lost_tag, run_id))
        saved = json.loads(self.binding_path('opencode/workspace/' + lost_tag).read_text())
        self.assertNotIn('creation_pending', saved)
        status, sessions = self.api('GET', '/api/session?limit=100')
        self.assertEqual(status, 200, sessions)
        self.assertEqual([s['id'] for s in sessions['data'] if s.get('title') == lost_tag], [lost_id])
        self.assertEqual(self.hgs(['inspect', 'opencode/workspace/' + lost_tag])['phase'], 'idle')

    def test_03_lost_response_and_queue_never_duplicate(self):
        b = self.create()
        name, conversation = b['name'], b['conversation_id']
        self.hgs(['send', name, '--json'], self.payload(b, text='HOLD_NATIVE first turn'))
        self.assertTrue(self.model.holding.wait(30), 'the model never received the held turn')
        working = self.wait(name, lambda v: v['phase'] == 'working' and v['interrupt_supported'])
        request = self.payload(b, text='Queued while working')
        queued = self.hgs(['send', name, '--json'], request)
        self.assertEqual(queued['status'], 'submitted')
        self.assertTrue(queued.get('queued'))
        message_id = queued['native_message_id']
        # The admission's response is lost: the journal still says pending.
        receipt = json.loads(self.receipt_path(request).read_text())
        receipt['status'] = 'pending'
        self.receipt_path(request).write_text(json.dumps(receipt))
        again = self.hgs(['send', name, '--json'], request)
        self.assertEqual((again['status'], again['native_message_id']), ('submitted', message_id))
        self.assertEqual([item['id'] for item in self.inbox(conversation)], [message_id])
        view = self.wait(name, lambda v: any(e.get('delivery_state') == 'queued' for e in v['events']))
        rows = [e for e in view['events'] if e['detail'] == 'Queued while working']
        self.assertEqual([(e['message_id'], e['delivery_state']) for e in rows], [(message_id, 'queued')])
        self.assertNotIn('remove_queued', view['allowed_actions'])
        # Stop ends the turn; the queued message stays, not delivered.
        stop = {'request_id': str(uuid.uuid4()), 'expected_run_id': b['run_id'], 'expected_conversation_id': conversation,
                'expected_turn_started': working['turn_started']}
        self.hgs(['interrupt', name, '--json'], stop)
        self.model.release.set()
        stopped = self.wait(name, lambda v: v['phase'] == 'idle'
                            and any(e.get('delivery_state') == 'not_delivered' for e in v['events']))
        self.assertIn('remove_queued', stopped['allowed_actions'])
        self.assertEqual([item['id'] for item in self.inbox(conversation)], [message_id])
        self.action(b, 'remove_queued', message_id=message_id)
        self.assertEqual(self.inbox(conversation), [])
        cleared = self.wait(name, lambda v: not any(e.get('delivery_state') for e in v['events']))
        self.assertNotIn('remove_queued', cleared['allowed_actions'])
        refused = self.action(b, 'remove_queued', status='failed', message_id=message_id)
        self.assertIn('unavailable', refused['error'])
        # A confirmed removal is the request's definite non-delivery.
        removed = json.loads(self.receipt_path(request).read_text())
        self.assertEqual((removed['status'], removed['reason']), ('refused', REMOVED))
        self.assertIn(REMOVED, self.hgs(['send', name, '--json'], request, success=False).stderr)
        # A journaled message whose request never reached OpenCode is never posted by a
        # retry: OpenCode would admit an unknown ID, so its delivery stays uncertain.
        lost = self.payload(b, text='Never posted after a lost request')
        lost_id = 'msg_' + native_suffix()
        digest = hashlib.sha256(json.dumps([name, lost], sort_keys=True, separators=(',', ':')).encode()).hexdigest()
        self.receipt_path(lost).write_text(json.dumps({
            'request_id': lost['request_id'], 'name': name, 'run_id': b['run_id'], 'conversation_id': conversation,
            'digest': digest, 'native_message_id': lost_id, 'text': lost['text'], 'submitted_text': lost['text'],
            'submitted_at': time.time(), 'at': time.time(), 'status': 'pending', 'attachments': [],
            'transport': 'native_opencode'}))
        failure = self.hgs(['send', name, '--json'], lost, success=False)
        self.assertIn('Delivery uncertain; this request will not be replayed', failure.stderr)
        self.assertEqual(json.loads(self.receipt_path(lost).read_text())['status'], 'pending')
        self.assertEqual(self.inbox(conversation), [])
        users = self.user_messages(conversation)
        self.assertEqual([text for _, text in users], ['HOLD_NATIVE first turn'])
        self.assertNotIn(message_id, [native_id for native_id, _ in users])
        self.assertEqual(self.hgs(['inspect', name])['phase'], 'idle')

    def test_04_question_and_permission_answers(self):
        b = self.create()
        name = b['name']
        self.hgs(['send', name, '--json'], self.payload(b, text='QUESTION_NATIVE pick a color'))
        data = self.wait(name, lambda v: v['pending_questions'])
        self.assertEqual((data['phase'], data['activity']), ('input', 'attention'))
        self.assertTrue(data.get('attention_id'))
        [card] = data['pending_questions']
        self.assertEqual((card['kind'], card['can_answer'], card['approval']), ('opencode_form', True, False))
        [item] = card['questions']
        self.assertEqual([o['label'] for o in item['options']], ['Green', 'Blue'])
        green = next(o['id'] for o in item['options'] if o['label'] == 'Green')
        # A pending question blocks new messages instead of queueing them behind it.
        self.hgs(['send', name, '--json'], self.payload(b, text='not while asked'), success=False)
        request = self.payload(b, question_id=card['question_id'], expected_question_hash=card['question_hash'],
                               answers=[{'question_id': item['id'], 'selected_option_ids': [green], 'text': ''}])
        self.hgs(['answer', name, '--json'], {**request, 'expected_question_hash': 'changed'}, success=False)
        self.assertEqual(self.hgs(['answer', name, '--json'], request)['status'], 'answered')
        self.hgs(['answer', name, '--json'], request, success=False)
        final = self.wait(name, lambda v: v['phase'] == 'idle' and v.get('reply_id'))
        self.assertTrue(any(e['type'] == 'PreToolUse' and e['tool'] == 'question' for e in final['events']))
        self.assertTrue(any(e['type'] == 'PostToolUse' and e['tool'] == 'question' and '"Green"' in e['detail']
                            for e in final['events']), final['events'])
        self.assertEqual(final['pending_questions'], [])
        tool = self.create()
        self.hgs(['send', tool['name'], '--json'], self.payload(tool, text='BASH_NATIVE run the fixture command'))
        data = self.wait(tool['name'], lambda v: v['pending_questions'])
        [card] = data['pending_questions']
        self.assertEqual((card['kind'], card['can_answer'], card['approval']), ('opencode_permission', True, True))
        [item] = card['questions']
        self.assertEqual([o['id'] for o in item['options']], ['once', 'always', 'reject'])
        self.assertIn('printf native-tool-output', item['prompt'])
        request = self.payload(tool, question_id=card['question_id'], expected_question_hash=card['question_hash'],
                               answers=[{'question_id': 'permission', 'selected_option_ids': ['once'], 'text': ''}])
        self.assertEqual(self.hgs(['answer', tool['name'], '--json'], request)['status'], 'answered')
        final = self.wait(tool['name'], lambda v: v['phase'] == 'idle' and v.get('reply_id'))
        self.assertTrue(any(e['type'] == 'PostToolUse' and e['tool'] == 'shell' and 'native-tool-output' in e['detail']
                            for e in final['events']), final['events'])
        self.assert_private(data, final)

    def test_05_model_effort_and_provider_error(self):
        b = self.create('--effort', 'low')
        name = b['name']
        self.assertEqual((b['settings_model'], b['effort']), ('fixture/local', 'low'))
        self.assertEqual(self.session(b['conversation_id'])['model'], {'providerID': 'fixture', 'id': 'local', 'variant': 'low'})
        options = {row['value']: row for row in b['model_options']}
        self.assertEqual(set(options['fixture/alternate']['effort_options']), {'low', 'medium', 'high'})
        self.hgs(['settings', name, '--json'], self.payload(b, model='fixture/alternate', effort='extreme'), success=False)
        self.hgs(['settings', name, '--json'], self.payload(b, model='fixture/missing', effort=''), success=False)
        applied = self.hgs(['settings', name, '--json'], self.payload(b, model='fixture/alternate', effort='high'))
        self.assertEqual((applied['status'], applied['model'], applied['effort']), ('applied', 'fixture/alternate', 'high'))
        self.assertEqual(self.session(b['conversation_id'])['model'],
                         {'providerID': 'fixture', 'id': 'alternate', 'variant': 'high'})
        changed = self.hgs(['inspect', name])
        self.assertEqual((changed['settings_model'], changed['effort']), ('fixture/alternate', 'high'))
        self.hgs(['send', name, '--json'], self.payload(b, text='ERROR_NATIVE fail'))
        error = self.wait(name, lambda v: v['phase'] == 'error')
        self.assertEqual(error['activity'], 'attention')
        self.assertTrue(any(e['type'] == 'StopFailure' and 'Fixture provider rejected' in e['detail'] for e in error['events']))
        self.assertIn('Fixture provider rejected', json.dumps(error))
        # The failed turn went to the changed model with the changed effort.
        turns = [r for r in self.model.requests if 'ERROR_NATIVE' in r['last_user'] and r['tools']]
        self.assertTrue(turns)
        self.assertEqual({(r['model'], r['options'].get('reasoning_effort')) for r in turns}, {('alternate', 'high')})
        self.assert_private(b, changed, error)

    def test_06_legacy_binding(self):
        current = self.create()
        cwd = os.path.realpath(self.workspace)
        refused = self.save_binding('legacy-' + uuid.uuid4().hex[:8], conversation_id='ses_' + uuid.uuid4().hex,
                                    paused=True, native_data_home=str(self.root / 'opencode1-data'))
        # A session OpenCode 2 migrated keeps its OpenCode 1 ID and directory.
        migrated_id = 'ses_' + uuid.uuid4().hex
        status, created = self.api('POST', '/api/session', {'id': migrated_id, 'location': {'directory': cwd}, 'title': 'migrated'})
        self.assertEqual(status, 200, created)
        migrated = self.save_binding('migrated-' + uuid.uuid4().hex[:8], conversation_id=migrated_id, paused=True)
        rows = self.listing()
        legacy = rows[refused['name']]
        self.assertEqual((legacy['state'], legacy['allowed_actions']), ('stopped', ['forget']))
        self.assertEqual(legacy['inspection_reason'], OPENCODE1)
        self.assertEqual((rows[current['name']]['state'], rows[current['name']]['phase']), ('running', 'idle'))
        self.assertIn('rename', rows[current['name']]['allowed_actions'])
        self.assertEqual((rows[migrated['name']]['state'], rows[migrated['name']]['phase']), ('running', 'idle'))
        adopted = json.loads(self.binding_path(migrated['name']).read_text())
        self.assertEqual(adopted['service'], 'opencode2')
        self.assertNotIn('paused', adopted)
        inspected = self.hgs(['inspect', refused['name']])
        self.assertEqual((inspected['state'], inspected['allowed_actions']), ('stopped', ['forget']))
        self.assertEqual(inspected['inspection_reason'], OPENCODE1)
        full = self.hgs(['inspect', current['name']])
        self.assertEqual(full['phase'], 'idle')
        self.assertIn('fixture/local', [row['value'] for row in full['model_options']])
        self.hgs(['send', refused['name'], '--json'], self.payload(refused, text='never delivered'), success=False)
        # Forget removes only the Zerus binding; OpenCode's sessions stay.
        for record in (refused, migrated):
            self.action(record, 'forget')
            self.assertFalse(self.binding_path(record['name']).exists())
        self.assertTrue(self.binding_path(current['name']).exists())
        self.assertEqual(self.session(migrated_id)['id'], migrated_id)
        self.assertEqual(self.session(current['conversation_id'])['id'], current['conversation_id'])
        self.assertEqual(set(self.listing()) & {refused['name'], migrated['name']}, set())
        self.assertEqual(self.hgs(['inspect', current['name']])['phase'], 'idle')

    def live_stand_in(self):
        """A disposable live process to register as the service: if hgs ever ran OpenCode's
        own start here, OpenCode could stop that process, never this test runner."""
        child = subprocess.Popen(['sleep', '600'], stdin=subprocess.DEVNULL)
        self.addCleanup(child.wait)
        self.addCleanup(child.kill)
        return child

    def assert_never_started(self, registration, stand_in):
        self.assertEqual(self.registration_path().read_bytes(), registration)
        self.assertIsNone(stand_in.poll(), 'the registered process was stopped')
        self.assertEqual(self.native_processes(), [])

    def test_07_incompatible_service(self):
        self.stop_service()
        saved = self.save_binding('incompatible-' + uuid.uuid4().hex[:8], service='opencode2')
        openapi = json.loads((REPO / 'tests/fixtures/opencode2/openapi-2.0.32.json').read_text())
        del openapi['paths']['/api/session/{sessionID}/inbox']
        stand_in = self.live_stand_in()
        for version, reason in [('2.0.31', 'OpenCode 2.0.31 is not supported; install OpenCode 2.0.32 or newer 2.x'),
                                ('2.0.40', 'OpenCode 2.0.40 lacks GET /api/session/{sessionID}/inbox; update OpenCode or Zerus')]:
            with self.subTest(version=version):
                server, seen = stub_service(version, stand_in.pid, openapi)
                try:
                    registration = self.write_registration({
                        'id': 'svc_stub_' + uuid.uuid4().hex[:8], 'version': version, 'pid': stand_in.pid,
                        'url': f'http://127.0.0.1:{server.server_port}', 'password': 'stub-password'})
                    row = self.hgs(['inspect', saved['name']])
                    self.assertTrue(row['inspection_unavailable'])
                    self.assertEqual((row['inspection_reason'], row['state']), (reason, 'unknown'))
                    account = self.account()
                    self.assertEqual((account['catalog_state'], account['catalog_reason']), ('incompatible', reason))
                    tag = 'refused-' + uuid.uuid4().hex[:8]
                    self.assertIn(reason, self.launch(tag, '--model', 'fixture/local', success=False).stderr)
                    self.assertFalse(self.binding_path('opencode/workspace/' + tag).exists())
                    self.assertIn(reason, self.hgs(['account', 'start', 'native-opencode'], success=False).stderr)
                    self.assert_never_started(registration, stand_in)
                    self.assertEqual({method for method, _ in seen}, {'GET'})
                    self.assertTrue({path for _, path in seen} <= {'/api/info', '/openapi.json'}, seen)
                finally:
                    server.shutdown()
                    server.server_close()

    def test_08_never_replaces_a_live_registration(self):
        self.stop_service()
        # A live registered process whose port accepts connections but never answers.
        stand_in = self.live_stand_in()
        with socket.socket() as hung:
            hung.bind(('127.0.0.1', 0))
            hung.listen(16)
            registration = self.write_registration({
                'id': 'svc_hung', 'version': self.version, 'pid': stand_in.pid,
                'url': f'http://127.0.0.1:{hung.getsockname()[1]}', 'password': 'hung-password'})
            tag = 'hung-' + uuid.uuid4().hex[:8]
            failure = self.launch(tag, '--model', 'fixture/local', success=False)
            self.assertRegex(failure.stderr, 'OpenCode is (not responding|starting)')
            self.assertFalse(self.binding_path('opencode/workspace/' + tag).exists())
            failure = self.hgs(['account', 'start', 'native-opencode'], success=False)
            self.assertRegex(failure.stderr, 'OpenCode is (not responding|starting)')
            self.assertEqual(self.account()['catalog_state'], 'unreachable')
            self.assert_never_started(registration, stand_in)

    def test_09_explicit_start_and_account_catalog(self):
        self.stop_service()
        started = self.hgs(['account', 'start', 'native-opencode'])
        self.assertTrue(self.native_processes())
        self.assertEqual(self.registration()['url'].rstrip('/'), f'http://127.0.0.1:{self.port}')
        until = time.monotonic() + 30
        account = self.account()
        while account['catalog_state'] == 'loading' and time.monotonic() < until:
            time.sleep(.5)
            account = self.account()
        self.assertEqual(account['catalog_state'], 'ready', account)
        self.assertEqual(account['settings_model'], 'fixture/local')
        options = {row['value']: row for row in account['model_options']}
        for value in ('fixture/local', 'fixture/alternate'):
            self.assertEqual(set(options[value]['effort_options']), {'low', 'medium', 'high'})
        self.assertEqual(options['fixture/local']['provider_label'], 'Private keyless fixture')
        self.assertNotIn(f'127.0.0.1:{self.model.server_port}', json.dumps(account))
        self.assert_private(started, account)
        self.assertTrue(account['installed'], account.get('install_reason'))
        self.assertNotIn('install_reason', account)

    def test_10_a_message_removed_while_pending_is_never_resurrected(self):
        b = self.create()
        name, conversation = b['name'], b['conversation_id']
        self.hgs(['send', name, '--json'], self.payload(b, text='HOLD_NATIVE first turn'))
        self.assertTrue(self.model.holding.wait(30), 'the model never received the held turn')
        working = self.wait(name, lambda v: v['phase'] == 'working' and v['interrupt_supported'])
        text = 'Removed before delivery'
        request = self.payload(b, text=text)
        queued = self.hgs(['send', name, '--json'], request)
        self.assertTrue(queued.get('queued'))
        message_id = queued['native_message_id']
        # OpenCode admitted the message, but hgs lost the response: its journal says pending.
        receipt = json.loads(self.receipt_path(request).read_text())
        receipt['status'] = 'pending'
        receipt.pop('queued', None)
        self.receipt_path(request).write_text(json.dumps(receipt))
        self.assertEqual([item['id'] for item in self.inbox(conversation)], [message_id])
        stop = {'request_id': str(uuid.uuid4()), 'expected_run_id': b['run_id'], 'expected_conversation_id': conversation,
                'expected_turn_started': working['turn_started']}
        self.hgs(['interrupt', name, '--json'], stop)
        self.model.release.set()
        self.wait(name, lambda v: v['phase'] == 'idle' and any(e.get('delivery_state') == 'not_delivered' for e in v['events']))
        self.action(b, 'remove_queued', message_id=message_id)
        self.assertEqual(self.inbox(conversation), [])
        # The same request again must not admit the removed message a second time.
        retry = subprocess.run([str(HGS), 'send', name, '--json'], env=self.env, cwd=self.workspace, text=True,
                               capture_output=True, input=json.dumps(request), timeout=60)
        until = time.monotonic() + 5
        while time.monotonic() < until and message_id not in [i for i, _ in self.user_messages(conversation)]:
            time.sleep(.25)
        self.assertNotIn(message_id, [i for i, _ in self.user_messages(conversation)], 'the removed message was delivered')
        self.assertNotIn(text, [t for _, t in self.user_messages(conversation)])
        self.assertEqual(self.inbox(conversation), [])
        self.assertEqual([r for r in self.model.requests if text in r['last_user']], [])
        self.assertNotEqual(retry.returncode, 0, retry.stdout)
        self.assertIn(REMOVED, retry.stderr)
        receipt = json.loads(self.receipt_path(request).read_text())
        self.assertEqual((receipt['status'], receipt['reason'], receipt['native_message_id']),
                         ('refused', REMOVED, message_id))
        self.assertIn(REMOVED, self.hgs(['send', name, '--json'], request, success=False).stderr)
        self.assertEqual(self.inbox(conversation), [])
        self.assertEqual(self.hgs(['inspect', name])['phase'], 'idle')

    def test_11_a_message_left_queued_by_stop_runs_first_with_the_next_message(self):
        # OpenCode keeps a message that Stop left in its queue without running it. The
        # next message starts a run that delivers the leftover message first and then
        # the new one, each with its own reply, ending in one idle. Zerus neither
        # reposts nor refuses the leftover message.
        b = self.create()
        name, conversation = b['name'], b['conversation_id']
        first = self.hgs(['send', name, '--json'], self.payload(b, text='HOLD_NATIVE first turn'))
        self.assertTrue(self.model.holding.wait(30), 'the model never received the held turn')
        working = self.wait(name, lambda v: v['phase'] == 'working' and v['interrupt_supported'])
        left_text, next_text = 'Left in the queue by Stop', 'Sent after Stop'
        left = self.payload(b, text=left_text)
        queued = self.hgs(['send', name, '--json'], left)
        self.assertEqual(queued['status'], 'submitted')
        self.assertTrue(queued.get('queued'))
        left_id = queued['native_message_id']
        stop = {'request_id': str(uuid.uuid4()), 'expected_run_id': b['run_id'], 'expected_conversation_id': conversation,
                'expected_turn_started': working['turn_started']}
        self.hgs(['interrupt', name, '--json'], stop)
        self.model.release.set()
        stopped = self.wait(name, lambda v: v['phase'] == 'idle'
                            and any(e.get('delivery_state') == 'not_delivered' for e in v['events']))
        self.assertEqual([(e['detail'], e['delivery_state']) for e in stopped['events'] if e.get('message_id') == left_id],
                         [(left_text, 'not_delivered')])
        self.assertIn('remove_queued', stopped['allowed_actions'])
        # Left alone, OpenCode keeps the message queued and never runs it.
        until = time.monotonic() + 3
        while time.monotonic() < until:
            self.assertEqual([item['id'] for item in self.inbox(conversation)], [left_id])
            self.assertNotIn(left_id, [i for i, _ in self.user_messages(conversation)])
            time.sleep(.5)
        self.assertEqual([r for r in self.model.requests if left_text in r['last_user']], [])
        receipt = self.receipt_path(left).read_bytes()
        requests_before = len(self.model.requests)
        # The next message, sent while the session is idle, starts a run.
        new = self.payload(b, text=next_text)
        sent = self.hgs(['send', name, '--json'], new)
        self.assertEqual(sent['status'], 'submitted')
        self.assertNotIn('queued', sent)
        next_id = sent['native_message_id']

        def history():
            status, page = self.api('GET', f'/api/session/{conversation}/message?limit=100')
            self.assertEqual(status, 200, page)
            return list(reversed(page['data']))
        until = time.monotonic() + 10
        messages = history()
        while not (next_id in [m['id'] for m in messages] and messages[-1]['type'] == 'idle'):
            if time.monotonic() > until:
                self.fail('OpenCode did not settle after the next message: ' + json.dumps(messages)[-3000:])
            time.sleep(.25)
            messages = history()
        # The leftover message ran first, as its own step with its own reply, then the
        # new one; one run, one idle.
        after = messages[next(i for i, m in enumerate(messages) if m['id'] == left_id):]
        self.assertEqual([(m['type'], m['id'] if m['type'] == 'user' else m.get('outcome')) for m in after],
                         [('user', left_id), ('assistant', None), ('user', next_id), ('assistant', None),
                          ('idle', 'succeeded')])
        self.assertEqual([c.get('text') for m in after if m['type'] == 'assistant' for c in m['content']],
                         ['Native OpenCode reply'] * 2)
        self.assertEqual(list(reversed(self.user_messages(conversation))),
                         [(first['native_message_id'], 'HOLD_NATIVE first turn'), (left_id, left_text), (next_id, next_text)])
        self.assertEqual(self.inbox(conversation), [])
        self.assertEqual([r['last_user'] for r in self.model.requests[requests_before:] if r['tools']], [left_text, next_text])
        # Zerus shows both as ordinary delivered prompts, in that order, with nothing to remove.
        view = self.wait(name, lambda v: v['phase'] == 'idle' and not any(e.get('delivery_state') for e in v['events']))
        prompts = [(e['message_id'], e['detail']) for e in view['events'] if e['type'] == 'UserPromptSubmit']
        self.assertEqual(prompts[-2:], [(left_id, left_text), (next_id, next_text)])
        self.assertNotIn('remove_queued', view['allowed_actions'])
        # Zerus never reposted the leftover message and never marked it refused: its
        # receipt is untouched and the same request still answers with its admission.
        self.assertEqual(self.receipt_path(left).read_bytes(), receipt)
        again = self.hgs(['send', name, '--json'], left)
        self.assertEqual((again['status'], again['native_message_id'], again.get('queued')), ('submitted', left_id, True))
        self.assertNotIn('reason', again)
        self.assertEqual(self.receipt_path(left).read_bytes(), receipt)
        self.assertEqual([i for i, _ in self.user_messages(conversation)].count(left_id), 1)
        self.assertEqual([r for r in self.model.requests if r['last_user'] == left_text and r['tools']][1:], [])

if __name__ == '__main__':
    unittest.main()
