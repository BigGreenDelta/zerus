#!/usr/bin/env python3
"""Real Kimi Code against a localhost-only model, never a user's account.
Run with HGS_KIMI_TEST_BIN=/absolute/path/to/kimi (otherwise skipped).

Kimi's updater replaces the executable it runs from, whatever its home. Each
case therefore runs its own copy, with automatic updates off, never the given
file or a shared installation, and fails if either executable's bytes change.
"""
import hashlib
import http.server
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from unittest import mock
import uuid
import test_pause as lifecycle

KIMI = os.environ.get('HGS_KIMI_TEST_BIN')


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


class Model(http.server.BaseHTTPRequestHandler):
    """An OpenAI-compatible chat completion stream that finishes every turn."""

    def log_message(self, *args):
        pass

    def do_GET(self):
        body = json.dumps({'object': 'list', 'data': [{'id': 'fixture', 'object': 'model'}]}).encode()
        self.send_response(200)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_POST(self):
        self.server.requests.append(json.loads(self.rfile.read(int(self.headers.get('Content-Length', 0))) or b'{}'))
        self.send_response(200)
        self.send_header('Content-Type', 'text/event-stream')
        self.end_headers()
        for delta, finish in (({'role': 'assistant', 'content': 'Fixture complete.'}, None), ({}, 'stop')):
            chunk = {'id': 'chat_fixture', 'object': 'chat.completion.chunk', 'model': 'fixture',
                     'choices': [{'index': 0, 'delta': delta, 'finish_reason': finish}]}
            if finish:
                chunk['usage'] = {'prompt_tokens': 10, 'completion_tokens': 3, 'total_tokens': 13}
            self.wfile.write(b'data: ' + json.dumps(chunk).encode() + b'\n\n')
        self.wfile.write(b'data: [DONE]\n\n')
        self.wfile.flush()


@unittest.skipUnless(KIMI, 'Set HGS_KIMI_TEST_BIN for isolated native Kimi coverage')
class NativeKimi(lifecycle.Harness):
    def setUp(self):
        # Kimi's trust panel wraps a folder wider than its frame, and only a
        # path on one row is answered through hgs: keep the fixture's short.
        with mock.patch.object(tempfile, 'tempdir', '/tmp'):
            super().setUp()
        source = Path(KIMI).resolve()
        before = digest(source)
        # The copy lives outside the fixture's home, which tearDown removes, so
        # its bytes are checked once every process of the private socket ended.
        images = Path(tempfile.mkdtemp(prefix='hgs-kimi-image-'))
        self.addCleanup(shutil.rmtree, images, ignore_errors=True)
        image = images / 'kimi'
        shutil.copyfile(source, image)
        image.chmod(0o755)
        (self.bin / 'kimi').unlink()
        (self.bin / 'kimi').symlink_to(image)

        def unchanged():
            self.assertEqual(digest(image), before, 'the fixture changed its own Kimi executable')
            self.assertEqual(digest(source), before, 'the given Kimi executable changed')
        self.addCleanup(unchanged)
        # Only the copy is reachable: hgs puts this home's folders first, and no
        # other folder on the PATH may offer a kimi of its own.
        folders = [folder for folder in os.environ['PATH'].split(os.pathsep)
                   if folder and not (Path(folder) / 'kimi').exists()]
        self.env.update(PATH=os.pathsep.join([str(self.bin), *folders]), KIMI_CODE_NO_AUTO_UPDATE='1',
                        TERM='xterm-256color')
        self.assertEqual(Path(shutil.which('kimi', path=self.env['PATH'])).resolve(), image.resolve())
        server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Model)
        server.requests = []
        self.requests = server.requests
        self.addCleanup(server.server_close)
        self.addCleanup(server.shutdown)
        threading.Thread(target=server.serve_forever, daemon=True).start()
        home = self.root / '.kimi-code'
        home.mkdir()
        (home / 'config.toml').write_text(
            'default_model = "fixture"\ntelemetry = false\n'
            '[providers.fixture]\ntype = "openai"\nbase_url = "http://127.0.0.1:%d/v1"\napi_key = "fixture-key"\n'
            '[models.fixture]\nprovider = "fixture"\nmodel = "fixture"\nmax_context_size = 32768\n'
            '[loop_control]\nmax_attempts_per_step = 1\n' % server.server_port)
        (home / 'tui.toml').write_text('[upgrade]\nauto_install = false\n')
        self.home = home

    def screen(self, name):
        return self.t('capture-pane', '-p', '-J', '-t', '=' + name + ':')

    def until(self, predicate, name, seconds=60):
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            result = predicate()
            if result:
                return result
            time.sleep(0.2)
        self.fail('Native Kimi timed out:\n' + self.screen(name) + '\n' + self.hgs('inspect', name) +
                  '\nLast model messages: ' + json.dumps([request.get('messages', [])[-2:] for request in self.requests[-2:]],
                                                         ensure_ascii=False)[:4000])

    def inspect(self, name):
        return json.loads(self.hgs('inspect', name))

    def answer(self, name, card, choice):
        payload = dict(request_id=str(uuid.uuid4()), expected_run_id=self.binding(name)['run_id'],
                       expected_conversation_id='', question_id=card['question_id'],
                       expected_question_hash=card['question_hash'],
                       answers=[dict(question_id='trust', selected_option_ids=['trust_' + str(choice)], text='')])
        p = subprocess.run([str(lifecycle.HGS), 'answer', name, '--json'], env=self.env, input=json.dumps(payload),
                           capture_output=True, text=True, timeout=60)
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        return json.loads(p.stdout)

    def launch(self):
        """A tracked Kimi start: its folder trust panel is answered once through
        hgs, then the new session takes its first message. Kimi creates its
        conversation only with that message."""
        name = 'kimi/p/native'
        self.hgs('kimi', 'p', '-n', 'native', '-d')
        process = self.until(lambda: self.binding(name).get('pid'), name)
        self.assertIn('KIMI_CODE_NO_AUTO_UPDATE=1', self.environment(process).splitlines())
        answered = []

        def ready():
            state = self.inspect(name)
            cards = [card for card in state.get('pending_questions', []) if card.get('can_answer')]
            if cards and not answered:
                self.assertIn('Trust this folder?', self.screen(name))
                answered.append(self.answer(name, cards[0], 0))
                self.assertEqual(answered[0]['status'], 'answered')
                return None
            return state if state.get('first_message_can_send') else None
        state = self.until(ready, name)
        self.assertEqual(len(answered), 1)
        self.assertFalse(state.get('conversation_id'))
        return name

    def environment(self, pid):
        if sys.platform.startswith('linux'):
            return Path(f'/proc/{pid}/environ').read_bytes().decode(errors='replace').replace('\0', '\n')
        return subprocess.run(['ps', '-E', '-ww', '-o', 'command=', '-p', str(pid)], capture_output=True,
                              text=True, check=True).stdout.replace(' ', '\n')

    def payload(self, name, text):
        state = self.inspect(name)
        return dict(request_id=str(uuid.uuid4()), text=text, expected_run_id=state['run_id'],
                    expected_conversation_id=state.get('conversation_id') or '')

    def send(self, name, payload):
        p = subprocess.run([str(lifecycle.HGS), 'send', name, '--json'], env=self.env, input=json.dumps(payload),
                           capture_output=True, text=True, timeout=60)
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr + '\n' + self.screen(name))
        return json.loads(p.stdout)

    def settled(self, name):
        """The turn's conversation is confirmed and the session is Ready again."""
        state = self.inspect(name)
        return state.get('conversation_id') and state.get('activity') == 'idle' and state

    def held(self, text):
        """How often the newest model request's conversation holds `text` as a
        user message of its own. Kimi adds its reminders as separate messages."""
        found = []
        for message in (self.requests[-1].get('messages', []) if self.requests else []):
            content = message.get('content')
            if isinstance(content, list):
                content = ''.join(part.get('text', '') for part in content if isinstance(part, dict))
            if message.get('role') == 'user':
                found.append(content)
        return found.count(text)

    def test_exact_input_reaches_the_model_once(self):
        name = self.launch()
        conversation = None
        for text in ('Continue the fixture\\', 'Literal paths:\nfixture\\\\', 'Проверка ввода'):
            payload = self.payload(name, text)
            receipt = self.send(name, payload)
            self.assertEqual(receipt['status'], 'submitted')
            self.assertEqual(receipt['text'], text)
            self.until(lambda: self.held(text), name)
            state = self.until(lambda: self.settled(name), name)
            # The first message created the conversation; later ones continue it.
            conversation = conversation or state['conversation_id']
            self.assertEqual(state['conversation_id'], conversation)
            self.assertEqual(self.held(text), 1)
            # The same request identity returns its receipt and never types again.
            requests = len(self.requests)
            self.assertEqual(self.send(name, payload), receipt)
            time.sleep(1)
            self.assertEqual(len(self.requests), requests)
            self.assertEqual(self.held(text), 1)

    def test_pause_and_resume_keep_the_conversation(self):
        name = self.launch()
        self.send(name, self.payload(name, 'Start the fixture conversation'))
        self.until(lambda: self.settled(name), name)
        before = self.binding(name)
        self.hgs('pause', name)
        self.until(lambda: subprocess.run([str(self.bin / 'tmux'), 'has-session', '-t', '=' + name], env=self.env,
                                          capture_output=True).returncode != 0, name)
        self.hgs('resume', name, '-d')
        self.until(lambda: self.binding(name).get('run_id') != before['run_id'] and
                   self.inspect(name).get('activity') == 'idle', name)
        self.assertEqual(self.binding(name)['conversation_id'], before['conversation_id'])


if __name__ == '__main__':
    unittest.main()
