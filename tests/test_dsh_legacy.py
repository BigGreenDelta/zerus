"""Account permissions on a resident old bridge, against the keyless Harness."""
from contextlib import contextmanager
import hashlib
import http.server
import json
import re
import subprocess
import threading
import time
import unittest
import uuid

import test_dsh_native as native


@unittest.skipUnless(native.fixture.required() or native.DSH and native.HGS.exists(), 'official dsh and built hgs required')
class LegacyPermissions(unittest.TestCase):
    LEGACY_ADAPTER = True
    rpc = native.NativeHarness.__dict__['rpc']
    create = native.NativeHarness.create
    cli = native.NativeHarness.cli
    payload = native.NativeHarness.payload
    wait = native.NativeHarness.wait
    tearDownClass = native.NativeHarness.__dict__['tearDownClass']

    @classmethod
    def setUpClass(cls):
        native.NativeHarness.setUpClass.__func__(cls)
        (cls.host / 'host.log').symlink_to(cls.root / 'host.log')
        until = time.monotonic() + 15
        while time.monotonic() < until:
            if re.search(r'http://127\.0\.0\.1:\d+/\?token=[\w%.-]+', (cls.root / 'host.log').read_text()):
                break
            time.sleep(.05)
        else:
            cls.tearDownClass()
            raise RuntimeError('Isolated native web API did not start')

    def setUp(self):
        self.mode('bypass')

    def tearDown(self):
        self.mode('provider')

    def mode(self, value):
        subprocess.run([str(native.HGS), 'account', 'permissions', 'native-dsh', '--mode', value],
                       env=self.env, check=True, capture_output=True, text=True)

    def binding(self, name):
        path = self.state / 'dsh/bindings' / (hashlib.sha256(name.encode()).hexdigest() + '.json')
        return path, json.loads(path.read_text()) if path.exists() else None

    def launch(self):
        tag = 'legacy-' + str(uuid.uuid4())[:8]
        proc = subprocess.run([str(native.HGS), 'dsh', str(self.workspace), '-n', tag, '-d'],
                              env=self.env, cwd=native.REPO, capture_output=True, text=True, timeout=35)
        matches = list((self.state / 'dsh/bindings').glob('*.json'))
        binding = next((b for p in matches if (b := json.loads(p.read_text()))['name'].endswith('/' + tag)), None)
        return proc, binding

    def preset(self, binding):
        return self.rpc('inspect', {'sessionId': binding['conversation_id']})['baseline']['values']['permissions']['currentValue']

    @contextmanager
    def unavailable_permissions(self, catalog_available):
        """An owned API fixture fails either before or after native creation."""
        class Handler(http.server.BaseHTTPRequestHandler):
            def log_message(self, *_):
                pass

            def do_GET(self):
                self.send_response(303)
                self.send_header('Set-Cookie', 'fixture=local; HttpOnly')
                self.send_header('Location', '/')
                self.end_headers()

            def do_POST(self):
                self.rfile.read(int(self.headers.get('Content-Length', '0')))
                if self.path == '/api/permissionPresets/catalog':
                    value = {'options': [{'value': 'danger-full-access'}] if catalog_available else []}
                elif self.path == '/api/session/list':
                    value = {'items': [{'sessionId': row['sessionId'], 'agentAvailable': row['agentAvailable'],
                                        'running': row['running']} for row in owner.rpc('list')['items']]}
                elif self.path == '/api/session/projections':
                    value = {'values': {'permissions': {'currentValue': 'workspace-write'}}}
                else:
                    value = []
                body = json.dumps({'result': {'ok': True, 'value': value}}).encode()
                self.send_response(200)
                self.send_header('Content-Type', 'application/json')
                self.send_header('Content-Length', str(len(body)))
                self.end_headers()
                self.wfile.write(body)

        owner = self
        server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        log = self.host / 'host.log'
        log.unlink()
        log.write_text(f'http://127.0.0.1:{server.server_port}/?token=fixture-token\n')
        try:
            yield
        finally:
            log.unlink()
            log.symlink_to(self.root / 'host.log')
            server.shutdown()
            server.server_close()
            thread.join(timeout=5)

    def test_new_bypass_session_preserves_host_and_other_agents(self):
        self.assertNotIn('accountPermissions', self.rpc('ping'))
        generation = self.rpc('ping')['generation']
        pid = self.proc.pid
        other = self.create()
        before = self.preset(other)
        proc, binding = self.launch()
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertEqual(self.preset(binding), 'danger-full-access')
        self.assertNotIn('permissions_pending', binding)
        self.assertEqual(self.preset(other), before)
        self.assertEqual(self.rpc('ping')['generation'], generation)
        self.assertEqual(self.proc.pid, pid)
        self.assertIsNone(self.proc.poll())

    def test_resume_applies_bypass_but_attach_preserves_running_mode(self):
        binding = self.create()
        before = self.preset(binding)
        self.cli('send', binding, self.payload(binding, text='fixture message', attachments=[]))
        self.wait(lambda: 'assistant/message' in json.dumps(self.rpc('inspect', {'sessionId': binding['conversation_id']})['page']['records'])
                  and not next(row for row in self.rpc('list')['items'] if row['sessionId'] == binding['conversation_id'])['running'])
        self.cli('resume', binding)
        self.assertEqual(self.preset(binding), before)
        self.cli('pause', binding)
        self.cli('resume', binding)
        self.assertEqual(self.preset(binding), 'danger-full-access')
        self.assertEqual(self.binding(binding['name'])[1]['conversation_id'], binding['conversation_id'])
        self.assertIn('fixture message', json.dumps(self.rpc('inspect', {'sessionId': binding['conversation_id']})['page']['records']))

    def test_provider_mode_needs_no_legacy_web_api(self):
        self.mode('provider')
        with self.unavailable_permissions(False):
            proc, binding = self.launch()
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertNotEqual(self.preset(binding), 'danger-full-access')

    def test_unsupported_preset_creates_no_binding_or_native_session(self):
        before_bindings = set((self.state / 'dsh/bindings').glob('*.json'))
        before_sessions = {item['sessionId'] for item in self.rpc('list')['items']}
        with self.unavailable_permissions(False):
            proc, binding = self.launch()
        self.assertNotEqual(proc.returncode, 0)
        self.assertIn('Could not verify bypass permissions', proc.stderr)
        self.assertIsNone(binding)
        self.assertEqual(set((self.state / 'dsh/bindings').glob('*.json')), before_bindings)
        self.assertEqual({item['sessionId'] for item in self.rpc('list')['items']}, before_sessions)
        self.assertNotIn('fixture-token', proc.stderr + proc.stdout)

    def test_unconfirmed_setup_blocks_send_and_resume_repairs_same_session(self):
        with self.unavailable_permissions(True):
            proc, binding = self.launch()
        self.assertNotEqual(proc.returncode, 0)
        self.assertIn('did not confirm bypass permissions', proc.stderr)
        self.assertTrue(binding['permissions_pending'])
        self.assertNotEqual(self.preset(binding), 'danger-full-access')
        snapshot = self.cli('inspect', binding)
        self.assertFalse(snapshot['send_available'])
        send = subprocess.run([str(native.HGS), 'send', binding['name']],
                              input=json.dumps(self.payload(binding, text='must not be sent', attachments=[])),
                              env=self.env, text=True, capture_output=True, timeout=35)
        self.assertNotEqual(send.returncode, 0)
        self.assertIn('finish permission setup', send.stderr)
        self.cli('resume', binding)
        saved = self.binding(binding['name'])[1]
        self.assertEqual(saved['conversation_id'], binding['conversation_id'])
        self.assertNotIn('permissions_pending', saved)
        self.assertEqual(self.preset(binding), 'danger-full-access')
        self.assertTrue(self.cli('inspect', saved)['send_available'])
        raw = self.rpc('inspect', {'sessionId': binding['conversation_id']})
        self.assertFalse(any(r.get('event', {}).get('type') == 'user/message' for r in raw['page']['records']))

    def test_confirmed_pending_setup_does_not_repeat_permission_command(self):
        proc, binding = self.launch()
        self.assertEqual(proc.returncode, 0, proc.stderr)
        def count():
            rows = self.rpc('inspect', {'sessionId': binding['conversation_id']})['page']['records']
            return sum(row.get('event', {}).get('type') == 'command/run'
                       and row['event']['data'].get('name') == 'permission' for row in rows)
        before = count()
        self.assertEqual(before, 1)
        path, saved = self.binding(binding['name'])
        saved['permissions_pending'] = True
        path.write_text(json.dumps(saved))
        self.cli('resume', saved)
        self.assertNotIn('permissions_pending', self.binding(binding['name'])[1])
        self.assertEqual(count(), before)


if __name__ == '__main__':
    unittest.main()
