"""Keyless nested Codex exec contracts with private HOME, state and tmux."""
import http.server
import json
import os
import shlex
import signal
import subprocess
import threading
import time
import unittest

import test_clear_context_native
import test_input as fixtures


class Response(http.server.BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def do_POST(self):
        self.rfile.read(int(self.headers.get('Content-Length', 0)))
        self.send_response(200)
        self.send_header('Content-Type', 'text/event-stream')
        self.end_headers()
        item = dict(type='message', id='msg_review', role='assistant', status='completed',
                    content=[dict(type='output_text', text='Isolated review complete.', annotations=[])])
        response = dict(id='resp_review', object='response', status='completed', output=[item],
                        usage=dict(input_tokens=1, output_tokens=5, total_tokens=6))
        part = item['content'][0]
        for kind, value in [
            ('response.created', dict(response=dict(response, status='in_progress', output=[]))),
            ('response.output_item.added', dict(output_index=0, item=dict(item, status='in_progress', content=[]))),
            ('response.content_part.added', dict(output_index=0, content_index=0, item_id=item['id'], part=dict(part, text=''))),
            ('response.output_text.delta', dict(output_index=0, content_index=0, item_id=item['id'], delta=part['text'])),
            ('response.output_text.done', dict(output_index=0, content_index=0, item_id=item['id'], text=part['text'])),
            ('response.content_part.done', dict(output_index=0, content_index=0, item_id=item['id'], part=part)),
            ('response.output_item.done', dict(output_index=0, item=item)),
            ('response.completed', dict(response=response))]:
            self.wfile.write(('event: ' + kind + '\ndata: ' + json.dumps(dict(type=kind, **value)) + '\n\n').encode())
            self.wfile.flush()


@unittest.skipUnless(os.environ.get('HGS_CODEX_TEST_BIN'), 'Set HGS_CODEX_TEST_BIN for isolated native external agents')
class NativeExternalAgents(unittest.TestCase):
    setUp = fixtures.InputTransport.setUp
    tearDown = fixtures.InputTransport.tearDown
    script = fixtures.InputTransport.script
    tmux = fixtures.InputTransport.tmux
    write_record = fixtures.InputTransport.write_record
    wait_for = fixtures.InputTransport.wait_for
    received = fixtures.InputTransport.received
    assert_plugin_sync_off = test_clear_context_native.NativeCodexClear.assert_plugin_sync_off

    def test_ephemeral_review_is_a_child_of_claude_and_codex(self):
        server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Response)
        self.addCleanup(server.server_close)
        self.addCleanup(server.shutdown)
        threading.Thread(target=server.serve_forever, daemon=True).start()
        native_home = self.root / '.codex'
        native_home.mkdir()
        (native_home / 'config.toml').write_text('''model="gpt-6.1-sol"
model_provider="fixture"
check_for_update_on_startup=false
[features]
plugins=false
[model_providers.fixture]
name="Fixture"
base_url="http://127.0.0.1:''' + str(server.server_port) + '''/v1"
wire_api="responses"
request_max_retries=0
stream_max_retries=0
''')
        # `codex exec` would sync plugins too, leaving its git child running after it exits.
        self.assert_plugin_sync_off(native_home)
        command = shlex.quote(str(fixtures.HGS)) + ' __state hook'
        # Keep the provider-neutral command installed in resident accounts.
        (native_home / 'hooks.json').write_text(json.dumps(dict(hooks={kind: [dict(hooks=[
            dict(type='command', command=command)])] for kind in ('SessionStart', 'UserPromptSubmit', 'Stop', 'SessionEnd')})))
        self.script('review-launcher', '''#!/usr/bin/env python3
import json, os, pathlib, subprocess
root = pathlib.Path(os.environ['INPUT_FIXTURE'])
run = subprocess.run(json.loads(os.environ['REVIEW_ARGV']), capture_output=True, text=True, timeout=30)
(root / 'review-output').write_text(run.stdout + run.stderr)
(root / 'review-exit').write_text(str(run.returncode))
''')
        native = os.environ['HGS_CODEX_TEST_BIN']
        for parent in ('claude', 'codex'):
            with self.subTest(parent=parent):
                self.record.update(agent=parent, model='parent-model', transcript='parent-history',
                    prompt='Parent request', main_done=False, subagents={}, cwd=str(self.root))
                self.write_record()
                (self.root / 'review-exit').unlink(missing_ok=True)
                env = dict(self.env, HGS_SESSION=self.name, HGS_RUN_ID=self.run_id,
                           CODEX_HOME=str(native_home), REVIEW_ARGV=json.dumps([native, 'exec',
                           '--ephemeral', '--dangerously-bypass-hook-trust', '--skip-git-repo-check',
                           '-s', 'read-only', '-C', str(self.root), '--color', 'never', 'Review the isolated fixture.']))
                for key in tuple(env):
                    if key.startswith(('OPENAI_', 'ANTHROPIC_', 'CLAUDE_', 'CODEX_')) and key != 'CODEX_HOME':
                        env.pop(key)
                (self.root / 'nested-launch.json').write_text(json.dumps(dict(
                    argv=[str(self.bin / 'review-launcher')], env=env)))
                os.kill(self.record['pid'], signal.SIGUSR2)
                deadline = time.monotonic() + 40
                while not (self.root / 'review-exit').exists() and time.monotonic() < deadline:
                    time.sleep(.05)
                self.assertTrue((self.root / 'review-exit').exists(), 'Native review timed out')
                output = (self.root / 'review-output').read_text()
                self.assertEqual((self.root / 'review-exit').read_text(), '0', output)
                self.assertIn('Isolated review complete.', output)
                raw = json.loads(self.record_path.read_text())
                for key in ('conversation_id', 'model', 'transcript', 'pid', 'run_id', 'prompt', 'main_done'):
                    self.assertEqual(raw[key], self.record[key], key + '\n' + output)
                self.assertEqual(len(raw['subagents']), 1, output)
                child_id, child = next(iter(raw['subagents'].items()))
                self.assertEqual(child['provider'], 'codex')
                self.assertEqual(child['state'], 'finished')
                self.assertFalse(child.get('transcript'))
                view = subprocess.run([str(fixtures.HGS), 'inspect', self.name, '--agent', child_id],
                    env=self.env, capture_output=True, text=True, timeout=10)
                self.assertEqual(view.returncode, 0, view.stderr)
                events = json.loads(view.stdout)['events']
                self.assertTrue(any(e.get('type') == 'Stop' and 'Isolated review complete.' in e.get('detail', '')
                                    for e in events), events)
                self.assertEqual(self.received(), b'')


if __name__ == '__main__':
    unittest.main()
