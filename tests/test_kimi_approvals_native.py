#!/usr/bin/env python3
"""Kimi's own tool approval panel against a localhost-only model, never a
user's account. Linux only for now: /proc proves the running image, its start
and its environment. Run with HGS_KIMI_TEST_BIN=/absolute/path/to/a/pinned/kimi
and HGS_KIMI_TEST_SHA256 set to one reviewed pin (otherwise skipped).

Kimi's updater replaces the executable it runs from, so each case runs its own
copy with automatic updates off, never the given file or a shared install, and
asserts afterwards that neither, nor any HGS_KIMI_GUARD executable, changed.
Every helper (hgs, tmux, ps) and Kimi itself get an explicit private
environment; Kimi's platform endpoint is the fixture's and answers 404.

Activity state comes from hgs's own launch and hooks in the private home:
nothing is synthesized. The fixture answers Kimi's startup folder trust only on
an exactly matching chooser. The clipped cases resize the private pane
themselves. Not covered: an older unresolved request, repeated request IDs or
an unfinished wire in a real Kimi; their refusals are unit-tested.
"""
import hashlib, http.server, json, os, shutil, subprocess, sys, tempfile, threading, time, unittest, uuid
from pathlib import Path
from unittest import mock
import test_pause as lifecycle

KIMI = os.environ.get('HGS_KIMI_TEST_BIN')
KIMI_SHA256 = os.environ.get('HGS_KIMI_TEST_SHA256', '')
GUARDED = [Path(path) for path in os.environ.get('HGS_KIMI_GUARD', '').split(os.pathsep) if path]
EVIDENCE = os.environ.get('HGS_KIMI_EVIDENCE_DIR')
# The reviewed builds: the maintained Kimi and the one installed for new sessions.
REVIEWED = {'66f47536e40b02bb1d577cdd324768f73d39b8b9472e0ca140e73d8e225cd7de': '2.1.1',
            '8ee7405d82958f0a50a3aab0773aba9e01057978bad93ba379eaf89e5b6111af': '2.2.0'}
PLATFORM = '/kimi-platform'
# Exactly these private values reach Kimi; no other KIMI_ key and no credential prefix.
REQUIRED = ('HOME', 'KIMI_CODE_HOME', 'KIMI_CODE_BASE_URL', 'KIMI_CODE_NO_AUTO_UPDATE', 'KIMI_DISABLE_TELEMETRY',
            'XDG_CONFIG_HOME', 'XDG_DATA_HOME', 'XDG_CACHE_HOME', 'XDG_STATE_HOME',
            'USERPROFILE', 'APPDATA', 'LOCALAPPDATA', 'TMPDIR')
FORBIDDEN = ('OPENAI_', 'ANTHROPIC_', 'CODEX_', 'CLAUDE', 'DEEPSEEK_', 'OPENCODE_', 'AWS_', 'AZURE_', 'GOOGLE_',
             'GCP_', 'GITHUB_', 'GH_', 'NPM_', 'npm_', 'SSH_', 'SOPS_', 'AGE_')
SYSTEM_PATH = ('/usr/local/bin', '/usr/bin', '/bin')
CALL = 'call_fixture'
MARKER = 'printf approved > approved-marker'
DANGER = 'rm -rf scratch-fixture'
FEEDBACK = 'Use a dry run instead'


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def private_environment(root, home, bin_folder, platform, hgs_keys):
    """Built from nothing: the fixture's own folders, hgs's private routing and
    a PATH of the private bin plus system folders that offer no kimi."""
    path = [str(bin_folder), *(folder for folder in SYSTEM_PATH if not (Path(folder) / 'kimi').exists())]
    env = dict(hgs_keys, PATH=os.pathsep.join(path), HOME=str(root), KIMI_CODE_HOME=str(home),
               KIMI_CODE_BASE_URL=platform, KIMI_CODE_NO_AUTO_UPDATE='1', KIMI_DISABLE_TELEMETRY='1',
               DISABLE_AUTOUPDATER='1', TERM='xterm-256color', LANG='C.UTF-8', PYTHONUTF8='1')
    for key, folder in (('XDG_CONFIG_HOME', '.config'), ('XDG_DATA_HOME', '.local/share'),
                        ('XDG_CACHE_HOME', '.cache'), ('XDG_STATE_HOME', '.local/state'),
                        ('USERPROFILE', '.'), ('APPDATA', 'AppData/Roaming'),
                        ('LOCALAPPDATA', 'AppData/Local'), ('TMPDIR', 'tmp')):
        (root / folder).mkdir(parents=True, exist_ok=True)
        env[key] = str(root / folder)
    return env


def environment_findings(actual, expected):
    """Key-level verdicts only: values never leave this function."""
    return dict(keys={key: 'missing' if key not in actual else 'match' if actual[key] == expected[key] else 'different'
                      for key in REQUIRED},
                forbidden=sorted(key for key in actual if actual[key] and key.startswith(FORBIDDEN)),
                unexpected_kimi=sorted(key for key in actual if actual[key] and key.startswith('KIMI_') and key not in REQUIRED))


def trust_chooser_2_2(screen, folder):
    """Whether `screen` is exactly Kimi 2.2.0's startup trust chooser for
    `folder`, with nothing to activate and Trust and continue selected."""
    rows = [row.rstrip() for row in screen.splitlines()]
    rule = next((index for index, row in enumerate(rows) if row.strip() and set(row.strip()) == {'─'}), None)
    if rule is None:
        return False
    margin = len(rows[rule]) - len(rows[rule].lstrip())
    width = len(rows[rule].strip())
    expected = ['─' * width, ' Trust this folder?', ' ↑↓ navigate · Enter select · Esc exit', '', ' ' + str(folder), '',
                ' No project integrations or instructions to activate.', '',
                ' Trust is remembered for this folder, including future project config changes.',
                ' Tool approvals follow your permission settings.', '', '  ❯ Trust and continue', '    Exit', '─' * width]
    shown = rows[rule:rule + len(expected)]
    return width >= 20 and len(shown) == len(expected) and all(
        (row == '' and line == '') or row == ' ' * margin + line for row, line in zip(shown, expected)) and \
        not any(row.strip() for row in rows[rule + len(expected):])


class Model(http.server.BaseHTTPRequestHandler):
    """An OpenAI-compatible stream asking for one Bash call until a tool result
    arrives, and Kimi's platform, which has nothing to offer."""

    def log_message(self, *args):
        pass

    def platform(self, body):
        observed = dict(method=self.command, path=self.path[len(PLATFORM):],
                        name=body.get('name') if isinstance(body, dict) else None)
        self.server.platform.append(observed)
        self.send_error(404)
        self.wfile.flush()
        # The request log alone does not prove that its response was written.
        self.server.platform_completed.append(dict(observed))

    def do_GET(self):
        if self.path.startswith(PLATFORM + '/'):
            return self.platform({})
        body = json.dumps({'object': 'list', 'data': [{'id': 'fixture', 'object': 'model'}]}).encode()
        self.send_response(200)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers.get('Content-Length', 0))) or b'{}')
        if self.path.startswith(PLATFORM + '/'):
            return self.platform(body)
        self.server.requests.append(body)
        tools = {tool.get('function', {}).get('name') for tool in body.get('tools', [])}
        answered = any(message.get('role') == 'tool' for message in body.get('messages', []))
        self.send_response(200)
        self.send_header('Content-Type', 'text/event-stream')
        self.end_headers()

        def chunk(delta, finish=None):
            data = dict(id='chat_fixture', object='chat.completion.chunk', model='fixture',
                        choices=[dict(index=0, delta=delta, finish_reason=finish)])
            self.wfile.write(b'data: ' + json.dumps(data).encode() + b'\n\n')
        if 'Bash' in tools and not answered:
            arguments = json.dumps(dict(command=self.server.command, description='Fixture command'))
            chunk(dict(role='assistant', content=None, tool_calls=[
                dict(index=0, id=CALL, type='function', function=dict(name='Bash', arguments=arguments))]))
            chunk({}, 'tool_calls')
        else:
            chunk(dict(role='assistant', content='Fixture complete.'))
            chunk({}, 'stop')
        self.wfile.write(b'data: [DONE]\n\n')
        self.wfile.flush()


def serve(command):
    server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Model)
    server.requests, server.platform, server.platform_completed, server.command = [], [], [], command
    threading.Thread(target=server.serve_forever, daemon=True).start()
    return server


@unittest.skipUnless(KIMI and sys.platform.startswith('linux'),
                     'Set HGS_KIMI_TEST_BIN on Linux to test a pinned native Kimi against a local fake API')
class NativeKimiApprovals(lifecycle.Harness):
    command = MARKER
    rows = None

    def setUp(self):
        if '_danger' in self._testMethodName:
            self.command = DANGER
        if '_clipped_' in self._testMethodName:
            self.rows = 12
        self.assertIn(KIMI_SHA256, REVIEWED, 'HGS_KIMI_TEST_SHA256 must be a reviewed pin')
        self.version = REVIEWED[KIMI_SHA256]
        source = Path(KIMI).resolve()
        self.assertEqual(digest(source), KIMI_SHA256)
        self.guards = {str(path): digest(path) for path in [source, *GUARDED]}
        self.source = source
        # A short private root keeps the trust chooser's folder on one row.
        with mock.patch.object(tempfile, 'tempdir', '/tmp'):
            super().setUp()
        try:
            self.prepare()
        except BaseException:
            self.tearDown()
            raise

    def prepare(self):
        # The copy lives outside the home tearDown removes: its bytes are
        # checked after every owned process stopped, and kept on any doubt.
        self.images = Path(tempfile.mkdtemp(prefix='hgs-kimi-image-'))
        self.image = (self.images / 'kimi').resolve()
        shutil.copyfile(self.source, self.image)
        self.image.chmod(0o755)
        self.assertEqual(digest(self.image), KIMI_SHA256)
        (self.bin / 'kimi').unlink()
        (self.bin / 'kimi').symlink_to(self.image)
        (self.project / 'scratch-fixture').mkdir()
        self.server = server = serve(self.command)
        self.addCleanup(server.server_close)
        self.addCleanup(server.shutdown)
        self.home = home = self.root / '.kimi-code'
        home.mkdir()
        (home / 'config.toml').write_text(
            'default_model = "fixture"\ntelemetry = false\ndefault_permission_mode = "default"\n'
            '[providers.fixture]\ntype = "openai"\nbase_url = "http://127.0.0.1:%d/v1"\napi_key = "fixture-key"\n'
            '[models.fixture]\nprovider = "fixture"\nmodel = "fixture"\nmax_context_size = 32768\n'
            '[loop_control]\nmax_attempts_per_step = 1\n' % server.server_port)
        (home / 'tui.toml').write_text('[upgrade]\nauto_install = false\n')
        hgs_keys = {key: self.env[key] for key in ('SHELL', 'HGS_CONFIG_DIR', 'HGS_STATE_DIR', 'HGS_SELF', 'HGS_PEERS',
                                                    'HGS_TAB', 'HGS_TRACKING')}
        self.env = private_environment(self.root, home, self.bin, 'http://127.0.0.1:%d%s' % (server.server_port, PLATFORM),
                                       hgs_keys)
        self.assertEqual(Path(shutil.which('kimi', path=self.env['PATH'])).resolve(), self.image)
        self.name = 'kimi/p/approval'
        self.launch()

    # -- process proof ---------------------------------------------------
    def stat_start(self, pid):
        stat = Path(f'/proc/{pid}/stat').read_text()
        return stat.rsplit(')', 1)[1].split()[19]

    def assert_process(self, stage):
        pid = self.binding(self.name)['pid']

        def running():
            try:
                return Path(os.readlink(f'/proc/{pid}/exe')).resolve() == self.image
            except OSError:
                return False  # The supervisor may not have exec'd its image yet.
        self.until(running)
        start = self.stat_start(pid)
        data = Path(f'/proc/{pid}/environ').read_bytes()
        same = self.stat_start(pid) == start and Path(os.readlink(f'/proc/{pid}/exe')).resolve() == self.image
        actual = dict(row.split('=', 1) for row in data.decode(errors='replace').split('\0') if '=' in row)
        findings = environment_findings(actual, self.env)
        print('ZERUS_KIMI_PROCESS=' + json.dumps(dict(case=self.id(), stage=stage, pid=pid, start_ticks=start,
              same_process=same, image=str(self.image), sha256=KIMI_SHA256, version=self.version, **findings)), flush=True)
        self.assertTrue(same, 'the process changed while its environment was read')
        self.assertEqual(digest(f'/proc/{pid}/exe'), KIMI_SHA256)
        self.assertEqual(findings['keys'], {key: 'match' for key in REQUIRED})
        self.assertEqual(findings['forbidden'], [])
        self.assertEqual(findings['unexpected_kimi'], [])

    def assert_images(self):
        """The given pin, every guarded executable and, once made, the private
        copy: unchanged, whether or not evidence is kept."""
        after = {str(path): digest(path) for path in [self.source, *GUARDED]}
        image = getattr(self, 'image', None)
        copy = digest(image) if image and image.exists() else None
        print('ZERUS_KIMI_IMAGE_GUARDS=' + json.dumps(dict(case=self.id(), before=self.guards, after=after, copy=copy)),
              flush=True)
        self.assertEqual(after, self.guards, 'a given or guarded Kimi executable changed')
        if image:
            self.assertEqual(copy, KIMI_SHA256, 'the fixture changed its own Kimi executable')

    def tearDown(self):
        self.retain()
        try:
            super().tearDown()
        except BaseException:
            # Unknown survivors: keep the private home and image for inspection.
            print('ZERUS_KIMI_PRESERVED=' + json.dumps(dict(case=self.id(), home=str(self.root),
                  image=str(getattr(self, 'images', '')))), flush=True)
            self.assert_images()
            raise
        # Every owned process stopped: only now may the copy go.
        self.assert_images()
        if hasattr(self, 'images'):
            shutil.rmtree(self.images)

    def retain(self):
        """Keep this run's screens, logs, wire and record before cleanup."""
        if not EVIDENCE or not hasattr(self, 'name'):
            return
        out = Path(EVIDENCE) / self.id().rsplit('.', 1)[-1]
        out.mkdir(parents=True, exist_ok=True)
        target = '=' + self.name + ':'
        for file, args in (('screen.txt', ('capture-pane', '-p', '-t', target)),
                           ('screen-styled.txt', ('capture-pane', '-p', '-e', '-t', target))):
            result = subprocess.run([str(self.bin / 'tmux'), *args], env=self.env, capture_output=True, text=True, timeout=5)
            (out / file).write_text(result.stdout + result.stderr)
        for source in [self.home / 'logs', *self.home.glob('sessions/*/*/agents')]:
            if source.is_dir():
                shutil.copytree(source, out / source.relative_to(self.home), dirs_exist_ok=True)
        (out / 'record.json').write_text(json.dumps(self.binding(self.name)))
        (out / 'metadata.json').write_text(json.dumps(dict(version=self.version, pinned=KIMI, sha256=KIMI_SHA256,
            image=str(self.image), model_requests=len(self.server.requests), platform=self.server.platform,
            platform_completed=self.server.platform_completed), indent=1))

    # -- driving ---------------------------------------------------------
    def screen(self):
        return self.t('capture-pane', '-p', '-t', '=' + self.name + ':')

    def height(self):
        return int(self.t('display-message', '-p', '-t', '=' + self.name + ':', '#{window_height}'))

    def until(self, predicate, seconds=60):
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            result = predicate()
            if result:
                return result
            time.sleep(0.2)
        self.fail('Native Kimi timed out:\n' + self.screen())

    def inspect(self):
        return json.loads(self.hgs('inspect', self.name))

    def call(self, command, payload, success=True):
        result = subprocess.run([str(lifecycle.HGS), command, self.name, '--json'], env=self.env,
                                input=json.dumps(payload), capture_output=True, text=True, timeout=60)
        if not success:
            self.assertNotEqual(result.returncode, 0, result.stdout)
            return result.stderr
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr + '\n' + self.screen())
        return json.loads(result.stdout)

    def trust(self):
        """Kimi asks whether to trust the private project before its session
        starts. Answer only that exact chooser: through hgs's own Kimi 2.1
        trust card, or, for Kimi 2.2, which hgs on this base cannot read yet,
        with Enter on the exactly matching chooser with Trust and continue
        selected. The screen answered is printed as proof."""
        def asked():
            screen = self.screen()
            return screen if 'Trust this folder?' in screen else None
        screen = self.until(asked)
        if self.version == '2.1.1':
            cards = [card for card in self.inspect().get('pending_questions', [])
                     if card.get('source') == 'kimi_folder_trust' and card.get('can_answer')]
            self.assertEqual(len(cards), 1, screen)
            card = cards[0]
            payload = dict(request_id=str(uuid.uuid4()), expected_run_id=self.binding(self.name)['run_id'],
                           expected_conversation_id='', question_id=card['question_id'],
                           expected_question_hash=card['question_hash'],
                           answers=[dict(question_id='trust', selected_option_ids=['trust_0'], text='')])
            self.assertEqual(self.call('answer', payload)['status'], 'answered')
        else:
            self.assertTrue(trust_chooser_2_2(screen, self.project), screen)
            print('ZERUS_KIMI_TRUST=' + json.dumps(dict(case=self.id(), version=self.version, screen=screen)), flush=True)
            self.t('send-keys', '-t', '=' + self.name + ':', 'Enter')

    def launch(self):
        self.hgs('kimi', 'p', '-n', 'approval', '-d')
        self.until(lambda: self.binding(self.name).get('pid'))
        self.assert_process('initial')
        self.trust()

        def started():
            # Every platform request Kimi made got its 404 written (Kimi 2.1.1
            # asks for its banner), and hgs admits the first message.
            platform = self.server.platform_completed
            if len(platform) != len(self.server.platform) or (
                    self.version == '2.1.1' and not any(row['name'] == 'client_banner' for row in platform)):
                return None
            state = self.inspect()
            return state if state.get('first_message_can_send') else None
        state = self.until(started)
        payload = dict(request_id=str(uuid.uuid4()), text='Run the fixture command.', expected_run_id=state['run_id'],
                       expected_conversation_id=state.get('conversation_id') or '')
        self.assertEqual(self.call('send', payload)['status'], 'submitted')

        def asked():
            wires = list(self.home.glob('sessions/*/*/agents/main/wire.jsonl'))
            if len(wires) == 1 and '"kind":"approval"' in wires[0].read_text() and '1. Approve once' in self.screen():
                return wires[0]
        self.wire = self.until(asked)
        self.assert_process('approval')
        if self.rows:
            # Back to a short pane no client views; the window follows clients again.
            self.t('resize-window', '-t', '=' + self.name + ':', '-y', str(self.rows))
            self.t('set-option', '-w', '-u', '-t', '=' + self.name + ':', 'window-size')

    def attach_client(self):
        """A control-mode client at the same short height: tmux counts the pane as viewed."""
        client = subprocess.Popen([str(self.bin / 'tmux'), '-C', 'attach', '-t', '=' + self.name], env=self.env,
                                  stdin=subprocess.PIPE, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        self.addCleanup(lambda: (client.stdin.close(), client.wait(5)))
        client.stdin.write(f'refresh-client -C 80x{self.rows}\n'.encode())
        client.stdin.flush()
        self.until(lambda: self.t('display-message', '-p', '-t', '=' + self.name + ':', '#{session_attached}').strip() == '1')

    def card(self):
        cards = [card for card in self.inspect()['pending_questions'] if card['source'] == 'kimi_tool_approval']
        self.assertEqual(len(cards), 1, cards)
        return cards[0]

    def payload(self, card, option=None, text=''):
        return dict(request_id=str(uuid.uuid4()), expected_run_id=card['run_id'],
                    expected_conversation_id=card['conversation_id'], question_id=card['question_id'],
                    expected_question_hash=card['question_hash'],
                    answers=[dict(question_id='approval', selected_option_ids=[option] if option else [], text=text)])

    def resolved(self):
        rows = [json.loads(line) for line in self.wire.read_text().splitlines() if '"interaction.resolved"' in line]
        return rows[-1]['response'] if rows else None

    def answerable(self):
        card = self.card()
        self.assertTrue(card['can_answer'], card['answer_unavailable_reason'])
        self.assertEqual(card['answer_transport'], 'kimi_tui')
        item = card['questions'][0]
        self.assertEqual((item['question'], item['header']), ('Run this command?', 'Bash'))
        self.assertIn('cwd: ' + str(self.project), item['body'])
        self.assertIn('$ ' + self.command, item['body'])
        self.assertEqual([option['label'] for option in item['options']],
                         ['Approve once', 'Approve for this session', 'Reject'])
        return card

    def decide(self, card, **choice):
        receipt = self.call('answer', self.payload(card, **choice))
        self.assertEqual(receipt['status'], 'answered')
        self.until(lambda: 'Fixture complete.' in self.screen())
        return self.resolved()

    def tool_results(self):
        return [message.get('content') for body in self.server.requests for message in body.get('messages', [])
                if message.get('role') == 'tool']

    # -- cases -----------------------------------------------------------
    def test_approve_once_runs_the_command(self):
        self.assertEqual(self.decide(self.answerable(), option='1'), {'decision': 'approved'})
        self.assertEqual((self.project / 'approved-marker').read_text(), 'approved')

    def test_approve_for_this_session_records_its_scope(self):
        self.assertEqual(self.decide(self.answerable(), option='2'), {'decision': 'approved', 'scope': 'session'})
        self.assertTrue((self.project / 'approved-marker').exists())

    def test_reject_never_runs_the_command(self):
        self.assertEqual(self.decide(self.answerable(), option='3'), {'decision': 'rejected'})
        self.assertFalse((self.project / 'approved-marker').exists())

    def test_reject_with_feedback_reaches_the_model(self):
        self.assertEqual(self.decide(self.answerable(), text=FEEDBACK), {'decision': 'rejected', 'feedback': FEEDBACK})
        self.assertFalse((self.project / 'approved-marker').exists())
        self.assertTrue(any('Reason: ' + FEEDBACK in json.dumps(result) for result in self.tool_results()),
                        self.tool_results())

    def test_a_danger_warning_is_part_of_the_reviewed_request(self):
        card = self.answerable()
        self.assertIn('Dangerous: recursive delete', card['questions'][0]['body'])
        self.assertEqual(self.decide(card, option='3'), {'decision': 'rejected'})
        self.assertTrue((self.project / 'scratch-fixture').is_dir())

    def test_an_unviewed_clipped_panel_grows_until_answerable(self):
        self.assertEqual(self.height(), 12)
        card = self.answerable()
        self.assertGreater(self.height(), 12)
        self.assertEqual(self.t('show-options', '-w', '-v', '-t', '=' + self.name + ':', 'window-size').strip(), '')
        self.assertEqual(self.decide(card, option='3'), {'decision': 'rejected'})

    def test_a_viewed_clipped_panel_is_never_resized_or_answered(self):
        self.attach_client()
        before = self.height()
        card = self.card()
        self.assertFalse(card['can_answer'])
        self.assertIn('enlarge Terminal', card['answer_unavailable_reason'])
        self.assertEqual(self.height(), before)
        self.assertIn('enlarge Terminal', self.call('answer', self.payload(card, option='1'), success=False))
        self.assertIsNone(self.resolved())
        self.assertFalse((self.project / 'approved-marker').exists())


class FixtureControls(unittest.TestCase):
    """The fixture's own guards, without hgs, tmux or Kimi."""

    def test_the_private_environment_inherits_nothing(self):
        with tempfile.TemporaryDirectory() as folder, mock.patch.dict(
                os.environ, {'OPENAI_API_KEY': 'synthetic', 'KIMI_EXTRA': 'synthetic', 'SSH_AUTH_SOCK': '/x'}):
            root = Path(folder)
            env = private_environment(root, root / '.kimi-code', root / 'bin', 'http://127.0.0.1:9/kimi-platform',
                                      dict(HGS_STATE_DIR=str(root / 'state')))
            findings = environment_findings(env, env)
            self.assertEqual(findings, dict(keys={key: 'match' for key in REQUIRED}, forbidden=[], unexpected_kimi=[]))
            self.assertFalse({'OPENAI_API_KEY', 'KIMI_EXTRA', 'SSH_AUTH_SOCK'} & set(env))
            self.assertEqual(env['PATH'].split(os.pathsep)[0], str(root / 'bin'))
            self.assertTrue(all(Path(env[key]).is_relative_to(root) for key in REQUIRED if key.endswith(('HOME', 'TMPDIR',
                            'USERPROFILE', 'APPDATA', 'LOCALAPPDATA'))))

    def test_environment_findings_name_keys_never_values(self):
        expected = {key: '/private/' + key for key in REQUIRED}
        actual = dict(expected, HOME='/owner-home', OPENAI_API_KEY='synthetic-secret', KIMI_EXTRA='x')
        del actual['TMPDIR']
        findings = environment_findings(actual, expected)
        self.assertEqual((findings['keys']['HOME'], findings['keys']['TMPDIR']), ('different', 'missing'))
        self.assertEqual((findings['forbidden'], findings['unexpected_kimi']), (['OPENAI_API_KEY'], ['KIMI_EXTRA']))
        self.assertNotIn('synthetic-secret', json.dumps(findings))
        self.assertNotIn('/owner-home', json.dumps(findings))

    # Kimi 2.2.0's chooser as tmux showed it in an 80-column pane.
    CHOOSER = '\n'.join([' ' + '─' * 78, '  Trust this folder?', '  ↑↓ navigate · Enter select · Esc exit', '',
                         '  /tmp/hgs-pause-x/project with spaces', '',
                         '  No project integrations or instructions to activate.', '',
                         '  Trust is remembered for this folder, including future project config changes.',
                         '  Tool approvals follow your permission settings.', '', '   ❯ Trust and continue',
                         '     Exit', ' ' + '─' * 78, ''])

    def test_only_the_exact_kimi_2_2_trust_chooser_is_answered(self):
        folder = '/tmp/hgs-pause-x/project with spaces'
        self.assertTrue(trust_chooser_2_2(self.CHOOSER, folder))
        for changed in (self.CHOOSER.replace('❯ Trust and continue', '  Trust and continue').replace('     Exit', '   ❯ Exit'),
                        self.CHOOSER.replace('No project integrations or instructions to activate.', 'MCP servers: 2'),
                        self.CHOOSER.replace('project with spaces', 'other'),
                        self.CHOOSER.replace('Tool approvals follow', 'Tool approvals do not follow'),
                        self.CHOOSER + '\n Esc exit later',
                        self.CHOOSER.replace('Trust this folder?', 'Trust this folder? (2)')):
            self.assertFalse(trust_chooser_2_2(changed, folder), changed)
        self.assertFalse(trust_chooser_2_2(self.CHOOSER, '/tmp/hgs-pause-x/project'))

    def test_kimis_platform_answers_404_and_never_reaches_the_model(self):
        server = serve(MARKER)
        self.addCleanup(server.server_close)
        self.addCleanup(server.shutdown)
        import urllib.error, urllib.request
        request = urllib.request.Request('http://127.0.0.1:%d%s/resource_configs' % (server.server_port, PLATFORM),
                                         data=json.dumps({'name': 'client_banner'}).encode(), method='POST')
        with self.assertRaises(urllib.error.HTTPError) as error:
            urllib.request.urlopen(request, timeout=5)
        self.assertEqual(error.exception.code, 404)
        deadline = time.monotonic() + 5
        while not server.platform_completed and time.monotonic() < deadline:
            time.sleep(0.02)
        self.assertEqual(server.platform_completed, [dict(method='POST', path='/resource_configs', name='client_banner')])
        self.assertEqual(server.requests, [])

    def test_the_model_asks_for_bash_once_then_finishes(self):
        server = serve(MARKER)
        self.addCleanup(server.server_close)
        self.addCleanup(server.shutdown)
        import urllib.request

        def post(messages):
            body = dict(tools=[dict(type='function', function=dict(name='Bash'))], messages=messages)
            request = urllib.request.Request('http://127.0.0.1:%d/v1/chat/completions' % server.server_port,
                                             data=json.dumps(body).encode(), method='POST')
            return urllib.request.urlopen(request, timeout=5).read().decode()
        first = post([dict(role='user', content='go')])
        self.assertIn('"name": "Bash"', first)
        self.assertIn(json.dumps(json.dumps(dict(command=MARKER, description='Fixture command')))[1:-1], first)
        self.assertIn('Fixture complete.', post([dict(role='tool', content='done')]))


if __name__ == '__main__':
    unittest.main()
