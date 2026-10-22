"""Opt-in real hook-output contracts; fixtures, never a private admission executor.

The compatibility runner supplies sealed candidate, real pre-feature ad328 CLI,
observer and reviewed native inputs. This fixture intentionally interposes a
retained owned hook entry at a separate hgs[.exe] path. It does not test that the
fresh registration path originally generated that observer executable. Stable
CLI, staged hosts, supervisor/native argv and real answer-channel stay real.
Only complete actual output AND complete forwarding are accepted observations;
neither proves customer receipt or crash-atomic delivery. Pipes alter fault-path
behavior. Unknown/overflow/missing identity fails, with no retry or mark rollback.
"""
import base64
import errno
import hashlib
import itertools
import json
import math
import os
from pathlib import Path
import shlex
import shutil
import socket
import stat
import subprocess
import sys
import tempfile
import threading
import time
import unittest
import uuid

import model_api_fixture as api
if os.name != 'nt':
    import test_pause

REPO = Path(__file__).resolve().parents[1]
PRE_FEATURE = 'ad3285c19013cf2ff78ec8eaf6ab2fbdd5253dce'
HINT = ('This agent runs in a Zerus session: the hgs CLI reads, messages, launches and stops other agent '
        'sessions on this and other computers. Run hgs guide before using it.')
MAX_CALLS, MAX_RECORD, MAX_REQUESTS = 256, 384 * 1024, 2 * 1024 * 1024
CASE_METHODS = ('test_lifecycle', 'test_lost_mark', 'test_pre_feature_first_resume', 'test_optouts_and_instructions')
INPUTS = 'ZERUS_HINT_INPUTS'
PROJECT_SENTINEL = 'Keep the project instruction fixture sentinel.'
APPEND_SENTINEL = 'Keep the appended fixture sentinel.'
SYSTEM_SENTINEL = 'Keep the system prompt fixture sentinel.'
DEVELOPER_SENTINEL = 'Keep the developer fixture sentinel.'


def private_environment(environment):
    """Only platform basics and the case's own paths/keyless inputs reach children."""
    allowed = {'PATH', 'SHELL', 'SYSTEMROOT', 'WINDIR', 'COMSPEC', 'PATHEXT', 'PROCESSOR_ARCHITECTURE',
               'NUMBER_OF_PROCESSORS', 'HOME', 'USERPROFILE', 'APPDATA', 'LOCALAPPDATA', 'TEMP', 'TMP',
               'TMPDIR', 'TERM', 'LANG', 'LC_ALL', 'LC_CTYPE', 'TZ', 'PYTHONUTF8',
               'HGS_CONFIG_DIR', 'HGS_STATE_DIR', 'HGS_SELF', 'HGS_PEERS', 'HGS_TAB', 'HGS_TRACKING',
               'XDG_CONFIG_HOME', 'XDG_DATA_HOME', 'XDG_STATE_HOME', 'XDG_CACHE_HOME',
               'ANTHROPIC_BASE_URL', 'ANTHROPIC_API_KEY', 'OPENAI_API_KEY',
               'CLAUDE_CODE_DISABLE_NONESSENTIAL_TRAFFIC', 'DISABLE_AUTOUPDATER', 'DISABLE_UPDATES',
               'DISABLE_INSTALLATION_CHECKS', 'CLAUDE_CODE_DISABLE_OFFICIAL_MARKETPLACE_AUTOINSTALL',
               'DISABLE_TELEMETRY', 'DISABLE_ERROR_REPORTING', 'HTTPS_PROXY', 'HTTP_PROXY', 'NO_PROXY'}
    return {key: value for key, value in environment.items() if key.upper() in allowed}


def read_json(path, maximum):
    with Path(path).open('rb') as file:
        raw = file.read(maximum + 1)
    if len(raw) > maximum:
        raise AssertionError('fixture record bound exceeded')
    return json.loads(raw)


def bounded_observation(value, maximum=32768):
    raw = json.dumps(value, ensure_ascii=False).encode()
    if len(raw) <= maximum:
        return value
    return dict(truncated=True, bytes=len(raw), prefix=raw[:maximum].decode(errors='replace'))


def sha256(path, end=None):
    value = hashlib.sha256()
    with Path(path).open('rb') as file:
        for block in iter(lambda: file.read(1024 * 1024), b''):
            if end is not None and time.monotonic() >= end:
                raise AssertionError('original end expired hashing owned image')
            value.update(block)
    return value.hexdigest()


def verify_image_seals(expected, end):
    """Read-only copy/input checks; actual hashes are retained only on success."""
    rows = []
    for label, path, wanted in expected:
        actual = sha256(path, end)
        if actual != wanted:
            raise AssertionError('changed fixture executable: ' + label)
        rows.append(dict(label=label, path=str(path), sha256=actual))
    return rows


def stream_bytes(value):
    """Validate complete captured AND forwarded bytes; never infer the latter."""
    raw = base64.b64decode(value['base64'], validate=True)
    if (value['error'] or value['overflow'] or value['bytes'] != len(raw)
            or value['forwarded'] != len(raw) or len(raw) > 64 * 1024
            or value['sha256'] != hashlib.sha256(raw).hexdigest()):
        raise AssertionError('unknown stream observation')
    return raw


def main_call(begin, result):
    """One actual invocation, separate from model counts and binding metadata."""
    if result['recording_error'] or begin['argv'] != ['__state', 'hook']:
        raise AssertionError('unknown hook observation')
    identity = result['child']
    if not identity or not isinstance(identity['pid'], int) or identity['pid'] <= 0 or not identity['birth']:
        raise AssertionError('missing standalone hgs identity')
    if os.path.normcase(os.path.realpath(identity['image'])) != os.path.normcase(os.path.realpath(begin['subject'])):
        raise AssertionError('standalone hook image differs')
    if result['subject_sha256_after'] != begin['expected_sha256']:
        raise AssertionError('standalone hook image changed')
    incoming, outgoing = stream_bytes(result['stdin']), stream_bytes(result['stdout'])
    stream_bytes(result['stderr'])
    event = json.loads(incoming)
    if event.get('hook_event_name') != 'SessionStart' or event.get('agent_id'):
        return None
    if result['exit_code'] != 0 or result['signal'] is not None:
        raise AssertionError('main hook did not exit successfully')
    return dict(run=begin['run'], phase=begin['phase'], source=begin['source'],
                session=event['session_id'], event_source=event.get('source'), output=outgoing,
                child=identity, subject=begin['subject'])


def hint_output(raw):
    if not raw:
        return False
    document = json.loads(raw)
    if document != {'hookSpecificOutput': {'hookEventName': 'SessionStart', 'additionalContext': HINT}}:
        raise AssertionError('unexpected main hook output')
    return True


def startup_answer(state, binding, attempted):
    """Known native cards only; pure selection, no input action or gate override."""
    if (state.get('pending_questions_error') or state.get('run_id') != binding['run_id']
            or state.get('tracked') is not True or not isinstance(state.get('pending_questions'), list)):
        raise AssertionError('unknown startup identity')
    cards = state['pending_questions']
    if not cards:
        return None
    if len(cards) != 1:
        raise AssertionError('ambiguous startup consent')
    card = cards[0]
    if not isinstance(card, dict): raise AssertionError('unknown startup card')
    if readonly_startup_frame(card, binding):
        # A later public inspection must expose the supported answerable card.
        # This observation grants no answer, prompt, count or fresh deadline.
        return None
    choices = {'claude_folder_trust': ('claude-trust:', 'trust', 'trust_1'),
               'codex_folder_trust': ('codex-trust:', 'trust', 'trust_0'),
               'codex_hooks_trust': ('codex-hooks-trust:', 'hooks_trust', 'trust'),
               'claude_resume': ('claude-resume:', 'resume', 'resume')}
    choice = choices.get(card.get('source'))
    if (not choice or card.get('can_answer') is not True or card.get('run_id') != binding['run_id']
            or not isinstance(card.get('question_hash'), str) or len(card['question_hash']) != 64):
        raise AssertionError('unrecognized startup consent')
    if any(value not in '0123456789abcdef' for value in card['question_hash']):
        raise AssertionError('unknown startup hash')
    prefix, question, option = choice
    items = card.get('questions') or []
    key = (binding['run_id'], card['question_id'], card['question_hash'])
    if (card['question_id'] != prefix + card['question_hash'] or key in attempted or len(items) != 1
            or items[0].get('id') != question
            or option not in {item.get('id') for item in items[0].get('options') or []}):
        raise AssertionError('changed or already attempted startup consent')
    if card['source'] == 'claude_folder_trust' and items[0].get('options') != [
            dict(id='trust_0', label='No, exit'),
            dict(id='trust_1', label='Yes, I trust this folder')]:
        raise AssertionError('changed Claude trust choices; no answer authorized')
    return key, card, dict(question_id=question, selected_option_ids=[option], text='')


def readonly_startup_frame(card, binding):
    """An identity-bound read-only chooser may wait, without selecting anything."""
    value = card.get('question_hash')
    questions = card.get('questions')
    if not (binding.get('agent') in ('claude', 'codex') and binding.get('conversation_id') in (None, '')
            and card.get('source') == 'startup_terminal_choice' and card.get('can_answer') is False
            and isinstance(binding.get('run_id'), str) and 0 < len(binding['run_id']) <= 128
            and card.get('run_id') == binding['run_id'] and card.get('agent_id') == 'main'
            and card.get('conversation_id') in (None, '') and card.get('tool_call_id') == ''
            and card.get('answer_transport') == ''
            and card.get('answer_unavailable_reason') == 'Answer this in Terminal.'
            and isinstance(value, str) and len(value) == 64
            and all(char in '0123456789abcdef' for char in value)
            and card.get('question_id') == 'startup-choice:' + value
            and isinstance(questions, list) and len(questions) == 1 and isinstance(questions[0], dict)
            and questions[0].get('multi_select') is False and questions[0].get('allow_other') is False):
        return False
    question = questions[0]
    options = question.get('options')
    return (set(question) == {'id', 'header', 'question', 'body', 'multi_select', 'allow_other', 'options'}
            and question.get('id') == 'terminal' and question.get('header') == 'Terminal'
            and isinstance(question.get('question'), str) and 0 < len(question['question']) <= 1024
            and isinstance(question.get('body'), str) and len(question['body']) <= 32768
            and isinstance(options, list) and len(options) <= 16
            and all(isinstance(item, dict) and set(item) == {'id', 'label'} and isinstance(item.get('id'), str)
                    and 0 < len(item['id']) <= 64 and isinstance(item.get('label'), str)
                    and 0 < len(item['label']) <= 1024 for item in options))


def observe_unix_descendants(owned, rows):
    """Extend only already-recorded, birth-matched parentage; never signal a PID."""
    while True:
        children = {(pid, row[2]) for pid, row in rows.items()
                    if (pid, row[2]) not in owned and row[0] in rows
                    and (row[0], rows[row[0]][2]) in owned}
        if not children: break
        if len(owned) + len(children) > 2048:
            raise AssertionError('UNKNOWN owned Unix identity bound')
        owned.update({identity: dict(role='descendant') for identity in children})
    return {identity for identity in owned if identity[0] in rows
            and rows[identity[0]][2] == identity[1] and not rows[identity[0]][1].startswith(('Z', 'X'))}


def unix_absence_proof(owned, rows, intents, endpoint_absent):
    if len(intents) > 64 or len(owned) > 2048:
        raise AssertionError('UNKNOWN Unix custody inventory bound')
    runs = set()
    for intent in intents:
        if (not isinstance(intent, dict) or intent.get('unix_registered') is not True
                or not isinstance(intent.get('unix_run'), str) or not intent['unix_run']
                or intent['unix_run'] in runs or 'unix_before' not in intent
                or not isinstance(intent.get('root'), str) or not os.path.isabs(intent['root'])
                or intent.get('socket') != str(Path(intent['root']) / 'tmux.sock')):
            raise AssertionError('UNKNOWN incomplete Unix launch custody')
        runs.add(intent['unix_run'])
        for role, key in (('server', 'unix_before'), ('server', 'unix_server'),
                          ('native', 'unix_native'), ('supervisor', 'unix_supervisor')):
            value = intent.get(key)
            if key == 'unix_before' and value is None: continue
            if (not isinstance(value, dict) or type(value.get('pid')) is not int or value['pid'] <= 0
                    or not isinstance(value.get('birth'), str) or not value['birth']
                    or not isinstance(value.get('image'), str) or not os.path.isabs(value['image'])
                    or owned.get((value['pid'], value['birth'])) != dict(role=role, image=value['image'])
                    or role == 'server' and value.get('socket') != intent['socket']):
                raise AssertionError('UNKNOWN unregistered Unix launch identity')
    if endpoint_absent is not True or observe_unix_descendants(owned, rows):
        raise AssertionError('UNKNOWN private endpoint or incomplete Unix launch custody')


def private_endpoint_absent(path, remaining):
    """Connect only this reserved SOCK_STREAM endpoint; send zero bytes."""
    remaining(1)
    try:
        metadata = os.lstat(path)
    except FileNotFoundError:
        pass
    else:
        if not stat.S_ISSOCK(metadata.st_mode) or metadata.st_uid != os.geteuid():
            raise AssertionError('UNKNOWN private socket type/owner')
    remaining(1)
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as endpoint:
        endpoint.settimeout(remaining(1))
        try:
            endpoint.connect(path)
        except OSError as error:
            remaining(1)
            if error.errno in (errno.ENOENT, errno.ECONNREFUSED): return True
            raise AssertionError('UNKNOWN private endpoint connectivity') from error
    remaining(1)
    return False


def wait_owned_sessions_empty(read, remaining, end):
    """Drain the selected private host within a fixed slice of its original end."""
    while True:
        remaining(1)
        if time.monotonic() >= end: raise AssertionError('UNKNOWN owned sessions did not drain')
        sessions = read()
        remaining(1)
        if time.monotonic() >= end: raise AssertionError('UNKNOWN owned session drain end')
        if not isinstance(sessions, list) or len(sessions) > 16:
            raise AssertionError('UNKNOWN owned session inventory')
        if not sessions: return
        time.sleep(min(.05, remaining(.05), max(0, end - time.monotonic())))


def public_ready(state, record):
    """Typed public admission; unknown/null/truthy values never grant a send."""
    if (state.get('tracked') is not True or state.get('pending_questions') != []
            or not isinstance(state.get('pending_questions'), list) or state.get('pending_questions_error')
            or state.get('run_id') != record['run_id']):
        return False
    if 'resume_message_can_send' in state:
        return state['resume_message_can_send'] is True and state.get('resume_message_reason') == ''
    if not record.get('conversation_id'):
        return state.get('first_message_can_send') is True and state.get('first_message_reason') == ''
    # A confirmed bound conversation deliberately has first_message_can_send
    # false and no first_message_reason. Its exact identity/idle state applies.
    return (isinstance(record['conversation_id'], str) and bool(record['conversation_id'])
            and state.get('conversation_id') == record['conversation_id'] and state.get('activity') == 'idle')


def checked_receipt(receipt, payload, serialized, name):
    wanted = dict(status='answered', name=name, request_id=payload['request_id'],
                  run_id=payload['expected_run_id'], conversation_id=payload['expected_conversation_id'],
                  question_id=payload['question_id'], question_hash=payload['expected_question_hash'],
                  answers=payload['answers'], digest=hashlib.sha256(serialized.encode()).hexdigest())
    if any(receipt.get(key) != value for key, value in wanted.items()) or receipt.get('open_terminal', False):
        raise AssertionError('startup receipt differs from its one prepared request')
    for field in ('submitted_at', 'answered_at'):
        value = receipt.get(field)
        if isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(value) or value <= 0:
            raise AssertionError('startup receipt has unknown acknowledgement time')


def stop_pinned_handle(handle, image, root, wait, terminate, remaining):
    """Finite handle-only decision; callbacks allow pure refusal/deadline controls."""
    if not handle: raise AssertionError('UNKNOWN missing pinned handle')
    state = wait(handle, 0)
    if state == 0: return
    if state != 0x102: raise AssertionError('UNKNOWN pinned handle state')
    actual = image()
    if actual is None:
        # A retiring process can lose image-query availability before its
        # already-pinned handle signals exit. Observe that same handle once;
        # missing image proof never authorizes a forced stop or a PID reopen.
        state = wait(handle, int(remaining(5) * 1000))
        remaining(1)
        if state == 0: return
        raise AssertionError('UNKNOWN pinned handle image unavailable')
    if not isinstance(actual, (str, os.PathLike)) or not os.path.isabs(actual):
        raise AssertionError('UNKNOWN pinned handle absolute image unavailable')
    root, actual = (os.path.normcase(os.path.realpath(value)) for value in (root, actual))
    if os.path.commonpath([root, actual]) != root: raise AssertionError('foreign image: no forced stop')
    remaining(1)
    if not terminate(handle, 1): raise AssertionError('UNKNOWN owned forced stop')
    if wait(handle, int(remaining(5) * 1000)) != 0: raise AssertionError('UNKNOWN owned reap at original end')


def pinned_image_query():
    """Query an already-owned handle; never reopen a numeric process ID."""
    import ctypes
    from ctypes import wintypes
    library = ctypes.WinDLL('kernel32', use_last_error=True)
    query = library.QueryFullProcessImageNameW
    query.argtypes = (wintypes.HANDLE, wintypes.DWORD, wintypes.LPWSTR, ctypes.POINTER(wintypes.DWORD))
    query.restype = wintypes.BOOL
    def image(handle):
        buffer = ctypes.create_unicode_buffer(32768)
        size = wintypes.DWORD(len(buffer))
        if not query(handle, 0, buffer, ctypes.byref(size)):
            return None
        return buffer.value
    return image


def cleanup_error(stage, error, **context):
    """Bounded private exception details, without locals or an extra OS query."""
    arguments = []
    for value in error.args[:8]:
        kind, truncated = type(value), False
        if kind is str:
            text, truncated = value[:1024], len(value) > 1024
        elif kind is bytes:
            # Bound the input before repr: escaped bytes can expand fourfold.
            text, truncated = repr(value[:240]), len(value) > 240
        elif kind is int:
            text = repr(value) if value.bit_length() <= 512 else '<integer exceeds diagnostic bound>'
            truncated = value.bit_length() > 512
        elif value is None or kind in (bool, float):
            text = repr(value)
        else:
            text = '<non-scalar argument>'
        arguments.append(dict(type=kind.__name__[:80], text=text, truncated=truncated))
    frames, frame = [], error.__traceback__
    while frame is not None and len(frames) < 8:
        code = frame.tb_frame.f_code
        frames.append(dict(file=code.co_filename[-512:], line=frame.tb_lineno, function=code.co_name[:128]))
        frame = frame.tb_next
    return dict(stage=stage, error=type(error).__name__, arguments=arguments,
                arguments_truncated=len(error.args) > 8, frames=frames,
                frames_truncated=frame is not None, context=context)


class HintContract:
    """Four named methods shared by real tmux and real session-host transports."""
    AGENT = None

    def setUp(self):
        supplied = json.loads(os.environ[INPUTS])
        self.inputs = supplied
        if supplied['pre_feature']['source'] != PRE_FEATURE:
            raise AssertionError('pre-feature source is not reviewed main ad328')
        for value in supplied.values():
            if not Path(value['path']).is_absolute() or sha256(value['path']) != value['sha256']:
                raise AssertionError('fixture source input changed')
        self.row_end = float(os.environ['ZERUS_NATIVE_DIAGNOSTIC_DEADLINE'])
        self.end, self.gate_end, self.cleaning = self.row_end - 60, None, False
        self.phase, self.attempted, self.checkpoints = 'candidate', set(), []
        self.startup_ends, self.completed_runs = {}, set()
        self.cli_children, self.cleanup_intents, self.cli_observations = [], [], []
        self.unix_owned, self.unix_runs = {}, set()
        self.cleanup_errors, self.cleanup_errors_overflow = [], False
        self.result_dir = Path(os.environ['ZERUS_HINT_RESULTS']) / self.id()
        self.result_dir.mkdir(mode=0o700, parents=True, exist_ok=False)
        self.observations = self.result_dir / 'calls'
        self.observations.mkdir(mode=0o700)
        self.cleanup_verified = False
        # Before inherited setup or any launch can fail. This contract never
        # calls the ordinary fixtures' renewed teardown waits or GC deletion.
        self.addCleanup(self.cleanup_owned)
        super().setUp()
        self.native_input = Path(os.environ['HGS_' + self.AGENT.upper() + '_TEST_BIN'])
        self.native_sha256 = sha256(self.native_input)
        self.names = getattr(self, 'names', [])
        if os.name != 'nt':
            self.state = self.root / 'state'
            self.state.mkdir(exist_ok=True)
            self.agent_home = self.root / ('.claude' if self.AGENT == 'claude' else '.codex')
            self.agent_home.mkdir(exist_ok=True)
            self.api_root = self.root / 'api'
            self.api_root.mkdir()
            self.api_url = 'http://127.0.0.1:' + str(self.start_api())
            self.env = {key: value for key, value in self.env.items()
                        if not key.startswith(('ANTHROPIC_', 'CLAUDE', 'OPENAI_', 'CODEX_', 'XDG_'))}
            for key, name in (('XDG_CONFIG_HOME', 'xc'), ('XDG_DATA_HOME', 'xd'),
                              ('XDG_CACHE_HOME', 'xx'), ('XDG_STATE_HOME', 'xs')):
                (self.root / name).mkdir()
                self.env[key] = str(self.root / name)
            self.env.update(ANTHROPIC_API_KEY='fixture-key', OPENAI_API_KEY='fixture-key',
                            ANTHROPIC_BASE_URL=self.api_url, CLAUDE_CODE_DISABLE_NONESSENTIAL_TRAFFIC='1',
                            DISABLE_AUTOUPDATER='1', DISABLE_UPDATES='1', DISABLE_INSTALLATION_CHECKS='1',
                            CLAUDE_CODE_DISABLE_OFFICIAL_MARKETPLACE_AUTOINSTALL='1', DISABLE_TELEMETRY='1',
                            DISABLE_ERROR_REPORTING='1', HTTPS_PROXY='http://127.0.0.1:9',
                            HTTP_PROXY='http://127.0.0.1:9', NO_PROXY='127.0.0.1,localhost',
                            TERM='xterm-256color', LANG='C.UTF-8')
            temporary = self.root / 't'
            temporary.mkdir()
            self.assertLessEqual(len(str(temporary).encode()), 48, 'use a short private outer TMPDIR')
            self.env['TMPDIR'] = str(temporary)
            self.configure()
            (self.bin / self.AGENT).unlink()
            shutil.copy2(os.environ['HGS_' + self.AGENT.upper() + '_TEST_BIN'], self.bin / self.AGENT)
        self.native_copy = self.bin / (self.AGENT + ('.exe' if os.name == 'nt' else ''))
        self.assertEqual(sha256(self.native_copy), self.native_sha256)
        self.subjects = self.root / 'subjects'
        self.subjects.mkdir()
        self.real = {}
        suffix = '.exe' if os.name == 'nt' else ''
        for phase in ('candidate', 'pre_feature'):
            folder = self.subjects / phase
            folder.mkdir()
            self.real[phase] = folder / ('hgs' + suffix)
            shutil.copy2(self.inputs[phase]['path'], self.real[phase])
            verify_image_seals([(phase + '_private_cli', self.real[phase], self.inputs[phase]['sha256'])], self.end)
            if os.name == 'nt' and getattr(self, 'CONPTY', None):
                for name in ('conpty.dll', 'OpenConsole.exe'):
                    shutil.copy2(Path(self.CONPTY) / name, folder / name)
                    verify_image_seals([(phase + '_conpty:' + name, folder / name,
                                         self.conpty_inputs[name]['sha256'])], self.end)
        self.observer = self.root / 'observer'
        self.observer.mkdir(mode=0o700)
        self.observer_exe = self.observer / ('hgs' + suffix)
        shutil.copy2(self.inputs['observer']['path'], self.observer_exe)
        verify_image_seals([('observer_private', self.observer_exe, self.inputs['observer']['sha256'])], self.end)
        self.settings_path = self.agent_home / ('settings.json' if self.AGENT == 'claude' else 'hooks.json')
        self.set_phase('candidate')
        self.install_observer_entry()

    def tearDown(self):
        self.cleaning, self.gate_end = True, None
        self.keep_startup_evidence()
        self.keep_evidence('before stop')

    def keep_startup_evidence(self):
        # Diagnostics never renew a gate or admit an answer/message. Admit one
        # final two-second private-socket read within five seconds; its existing
        # owned-child reap still shares the original row cleanup end.
        end = min(self.row_end, time.monotonic() + 5)
        document = dict(last_gate=getattr(self, 'last_gate', None),
                        launch=getattr(self, 'last_launch', None),
                        cli=list(getattr(self, 'cli_observations', [])),
                        original_row_end=self.row_end, diagnostic_end=end)
        name = (getattr(self, 'last_gate', None) or {}).get('name')
        if name:
            try:
                self.assertLess(time.monotonic(), end)
                document['run_record'] = bounded_observation(self.maybe_binding(name))
            except Exception as error:
                document['run_record_unknown'] = type(error).__name__ + ': ' + str(error)
        if os.name != 'nt' and name and getattr(self, 'socket', None):
            try:
                remaining = min(2, end - time.monotonic())
                self.assertGreater(remaining, 0)
                result = self.execute([test_pause.TMUX, '-S', self.socket, 'capture-pane', '-p',
                                       '-t', '=' + name + ':', '-S', '-80'], env=self.env,
                                      timeout=remaining)
                document['owned_pane'] = bounded_observation(dict(
                    status='captured' if result.returncode == 0 else 'unknown',
                    returncode=result.returncode, stdout=result.stdout, stderr=result.stderr))
            except Exception as error:
                document['owned_pane_unknown'] = type(error).__name__ + ': ' + str(error)
        encoded = json.dumps(document, indent=2).encode()
        self.assertLessEqual(len(encoded), MAX_RECORD, 'startup diagnostic evidence bound')
        (self.result_dir / 'startup-observations.json').write_bytes(encoded)

    def start_api(self):
        self.model = api.Server(self.api_root, str(self.project), hint=HINT,
                                sentinels=(PROJECT_SENTINEL, APPEND_SENTINEL, SYSTEM_SENTINEL, DEVELOPER_SENTINEL))
        self.model_thread = threading.Thread(target=self.model.serve_forever, daemon=True)
        self.model_thread.start()
        return self.model.server_address[1]

    def place_agent(self):
        # NativeAgent's ordinary fixture may link; this contract needs its own
        # bytes so an updater staging beside the executable cannot alter input.
        shutil.copy2(self.BINARY, self.bin / (self.AGENT + '.exe'))

    def install(self):
        # NativeAgent's same private install; validate its existing ConPTY input
        # pair first, then seal the exact initial portable copies it produced.
        self.assertTrue(getattr(self, 'CONPTY', None), 'reviewed ConPTY input pair is required')
        pin = read_json(REPO / 'packaging/windows/conpty.json', 8192)
        self.conpty_inputs = {name: dict(path=str(Path(self.CONPTY) / name), sha256=pin['files'][name]['sha256'])
                              for name in ('conpty.dll', 'OpenConsole.exe')}
        verify_image_seals([('conpty_input:' + name, Path(value['path']), value['sha256'])
                            for name, value in self.conpty_inputs.items()], self.end)
        super().install()
        self.initial_portable_seals(self.end)

    def initial_portable_seals(self, end):
        expected = [('initial_portable_candidate', self.hgs_exe, self.inputs['candidate']['sha256'])]
        expected += [('initial_portable_conpty:' + name, self.hgs_exe.parent / name, value['sha256'])
                     for name, value in self.conpty_inputs.items()]
        return verify_image_seals(expected, end)

    def close_model(self):
        if not hasattr(self, 'model'):
            return
        problems = []
        def close():
            try:
                if getattr(self, 'model_thread', None) and self.model_thread.is_alive():
                    self.model.shutdown()
                self.model.server_close()
                for handler in self.model.hint_handlers():
                    handler.join(max(0, self.row_end - time.monotonic()))
                if any(handler.is_alive() for handler in self.model.hint_handlers()):
                    raise AssertionError('UNKNOWN owned model handlers')
            except BaseException as error:
                problems.append(type(error).__name__)
        closer = threading.Thread(target=close, daemon=True)
        closer.start()
        closer.join(self.remaining(5))
        self.assertFalse(closer.is_alive() or problems, 'UNKNOWN owned model shutdown; retain runtime')
        if getattr(self, 'model_thread', None):
            self.model_thread.join(self.remaining(1))
            self.assertFalse(self.model_thread.is_alive(), 'UNKNOWN owned model thread; retain runtime')

    def configure(self):
        """Dummy loopback account/onboarding only; no project or hook trust values."""
        self.env = private_environment(self.env)
        if self.AGENT == 'claude':
            (self.agent_home / '.claude.json').write_text(json.dumps(dict(
                hasCompletedOnboarding=True, hasSeenAutoDefaultNudge=True, theme='dark',
                customApiKeyResponses=dict(approved=['fixture-key'], rejected=[]))))
            self.env['CLAUDE_CONFIG_DIR'] = str(self.agent_home)
            (self.agent_home / 'settings.json').write_text('{}\n')
        else:
            self.env['CODEX_HOME'] = str(self.agent_home)
            (self.agent_home / 'config.toml').write_text('\n'.join([
                'model="gpt-6.1-sol"', 'model_provider="fixture"', 'check_for_update_on_startup=false',
                'sandbox_mode="danger-full-access"', 'approval_policy="never"',
                '[features]', 'plugins=false', '[model_providers.fixture]', 'name="Fixture"',
                'base_url=' + json.dumps(self.api_url + '/v1'), 'env_key="OPENAI_API_KEY"',
                'wire_api="responses"', 'request_max_retries=0', 'stream_max_retries=0', '']))

    def remaining(self, maximum=20):
        deadline = self.row_end if self.cleaning else self.end
        if self.gate_end is not None: deadline = min(deadline, self.gate_end)
        remaining = deadline - time.monotonic()
        self.assertGreater(remaining, 0, 'original row work end exhausted')
        return min(maximum, remaining)

    def hgs(self, *args, input=None, rc=0, cwd=None):
        # Per-instance source selection. Never change imported fixture globals.
        phases = getattr(self, 'real', {})
        executable = phases.get(self.phase,
                    getattr(self, 'hgs_exe', Path(self.inputs[self.phase]['path'])))
        if self.phase not in phases and hasattr(self, 'hgs_exe'):
            # NativeAgent.install calls this before it returns. Refuse changed
            # initial portable CLI/ConPTY copies before their first execution.
            self.initial_portable_seals(self.row_end if self.cleaning else self.end)
        env = dict(self.env)
        if os.name != 'nt':
            env['HGS_EXECUTABLE'] = str(executable)
        result = self.execute([str(executable), *args], env=env, cwd=cwd or self.project,
                              input=input, timeout=self.remaining(5 if self.cleaning else 60))
        if rc is not None:
            self.assertEqual(result.returncode, rc, result.stdout + result.stderr)
        return result

    def execute(self, argv, *, env, input=None, cwd=None, timeout):
        # Namespace intent precedes spawn; direct-child custody precedes every
        # fallible communicate/return. Popen owns this child, never a PID lookup.
        launch = len(argv) > 1 and argv[1] in ('claude', 'codex', 'resume', 'fork', 'session-host')
        intent = None
        if os.name != 'nt' and len(argv) > 1 and (launch or argv[1] == 'pause'):
            rows, server, _ = self.unix_live_snapshot()
            if server is None:
                unix_absence_proof(self.unix_owned, rows, self.cleanup_intents, True)
        if launch:
            self.assertLess(len(self.cleanup_intents), 64, 'bounded launch intent inventory')
            if os.name != 'nt':
                self.assertTrue(all(item.get('unix_registered') is True for item in self.cleanup_intents),
                                'UNKNOWN earlier Unix launch custody')
            intent = dict(argv=argv, root=str(getattr(self, 'root', '')),
                          socket=getattr(self, 'socket', None), phase=self.phase)
            if os.name != 'nt': intent.update(unix_before=server, unix_registered=False)
            self.cleanup_intents.append(intent)
            self.save_cleanup('owned intent before spawn')
        timeout = min(timeout, self.remaining(timeout))
        options = dict(env=env, cwd=cwd, stdin=subprocess.PIPE if input is not None else subprocess.DEVNULL,
                       stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, encoding='utf-8')
        if os.name == 'nt':
            import test_windows_host as host
            options['creationflags'] = host.HIDDEN
        child = subprocess.Popen(argv, **options)
        self.cli_children = [known for known in self.cli_children if known.poll() is None]
        self.cli_children.append(child)
        observation = dict(argv=list(argv), started_monotonic=time.monotonic(), status='unknown')
        try:
            stdout, stderr = child.communicate(input, timeout=timeout)
            observation.update(status='exited', returncode=child.returncode,
                               stdout=stdout, stderr=stderr)
        except BaseException as error:
            observation.update(error=type(error).__name__ + ': ' + str(error))
            # An unreaped direct child retains its identity (a Windows process
            # handle, or this parent's unreaped POSIX child). No foreign lookup.
            if child.poll() is None:
                child.kill()
            child.communicate(timeout=max(.001, min(5, self.row_end - time.monotonic())))
            raise
        finally:
            observation['finished_monotonic'] = time.monotonic()
            if len(argv) > 1 and argv[1] in ('claude', 'codex', 'resume', 'fork', 'session-host'):
                self.last_launch = bounded_observation(observation, 4096)
            self.cli_observations = (getattr(self, 'cli_observations', []) +
                                     [bounded_observation(observation, 4096)])[-16:]
        if intent is not None and os.name != 'nt' and child.returncode == 0:
            _, server, _ = self.unix_live_snapshot()
            self.assertIsNotNone(server, 'UNKNOWN new private server identity')
            intent['unix_server'] = server
            self.save_cleanup('private server registered; new run custody pending')
        return subprocess.CompletedProcess(argv, child.returncode, stdout, stderr)

    def run_hgs(self, *args, timeout=60):
        # HostCase helpers use this case's real phase CLI and original end.
        return self.hgs('__host', *args, rc=None)

    def record(self, name):
        return self.binding(name)

    def json_hgs(self, *args, payload=None):
        return json.loads(self.hgs(*args, input=None if payload is None else json.dumps(payload)).stdout)

    def binding_path(self, name):
        return self.state / (hashlib.sha256(name.encode()).hexdigest() + '.json')

    def binding(self, name):
        return read_json(self.binding_path(name), MAX_RECORD)

    def maybe_binding(self, name):
        try:
            return self.binding(name)
        except FileNotFoundError:
            return None

    def inspected(self, name):
        return self.json_hgs('inspect', name)

    def wait(self, predicate, what, seconds=60):
        end = min(self.end, time.monotonic() + seconds)
        while time.monotonic() < end:
            result = predicate()
            if result:
                return result
            time.sleep(min(.1, max(0, end - time.monotonic())))
        self.fail('fixture deadline: ' + what)

    def set_phase(self, phase):
        # Only before any launch or after every owned conversation is paused.
        for name in self.names:
            record = self.binding(name)
            self.assertTrue(record.get('paused') or record.get('session_end'), 'phase change requires quiescence')
        if self.names:
            self.calls()  # Never retarget a helper with an incomplete prior invocation.
        self.phase = phase
        if os.name == 'nt':
            # Real install from real current_exe; observer is outside stable CLI
            # and all staged image folders. A resident old host is recorded below.
            report = self.json_hgs('session-host', 'install', '--json')
            self.real_hook = self.home / '.local/bin/hgs.exe'
            self.assertEqual(sha256(self.real_hook), self.inputs[phase]['sha256'])
            self.checkpoints.append(dict(phase=phase, install=report, resident_host=self.status()))
        else:
            self.real_hook = self.real[phase]
        document = dict(subject=str(self.real_hook), sha256=self.inputs[phase]['sha256'],
                        records=str(self.observations), phase=phase, source=self.inputs[phase]['source'])
        temporary = self.observer / 'observer.tmp'
        temporary.write_text(json.dumps(document))
        os.replace(temporary, self.observer / 'observer.json')

    def install_observer_entry(self):
        program = str(self.observer_exe).replace('\\', '/')
        real = str(self.real_hook).replace('\\', '/')
        if os.name == 'nt' and self.AGENT == 'claude':
            hook = dict(type='command', command=program, args=['__state', 'hook'], timeout=5)
            channel = dict(type='command', command=real, args=['__state', 'answer-channel'], timeout=86400)
        elif os.name == 'nt':
            hook = dict(type='command', command=f'"{program}" __state hook', timeout=5,
                        commandWindows=f'& "{program}" __state hook; exit $LASTEXITCODE')
            channel = None
        else:
            hook = dict(type='command', command=shlex.join([program, '__state', 'hook']), timeout=5)
            channel = (dict(type='command', command=shlex.join([real, '__state', 'answer-channel']), timeout=86400)
                       if self.AGENT == 'claude' else None)
        sentinel = self.root / 'personal-hook'
        code = 'from pathlib import Path; Path(' + repr(str(sentinel)) + ').touch()'
        if os.name == 'nt' and self.AGENT == 'claude':
            personal = dict(type='command', command=sys.executable, args=['-c', code], timeout=5)
        elif os.name == 'nt':
            quoted_code = "'" + code.replace("'", "''") + "'"
            personal = dict(type='command', command=shlex.join([sys.executable, '-c', code]), timeout=5,
                            commandWindows=f'& "{sys.executable}" -c {quoted_code}; exit $LASTEXITCODE')
        else:
            personal = dict(type='command', command=shlex.join([sys.executable, '-c', code]), timeout=5)
        hooks = dict(SessionStart=[dict(hooks=[hook]), dict(hooks=[personal])])
        if self.AGENT == 'claude':
            hooks['PermissionRequest'] = [dict(hooks=[channel])]
        self.seeded_hook, self.personal_hook, self.real_channel = hook, personal, channel
        self.settings_path.write_text(json.dumps(dict(hooks=hooks)))

    def assert_registration(self):
        settings = read_json(self.settings_path, MAX_RECORD)
        groups = settings['hooks']['SessionStart']
        self.assertEqual([h for g in groups for h in g['hooks']], [self.seeded_hook, self.personal_hook])
        if self.real_channel:
            channels = [h for g in settings['hooks']['PermissionRequest'] for h in g['hooks']
                        if h.get('timeout') == 86400]
            self.assertEqual(channels, [self.real_channel])
        self.assertEqual(sha256(self.real_hook), self.inputs[self.phase]['sha256'])

    def calls(self, pending=False):
        paths = list(itertools.islice(self.observations.iterdir(), MAX_CALLS + 1))
        self.assertLessEqual(len(paths), MAX_CALLS, 'observer call bound exceeded')
        found, identities = [], set()
        for path in paths:
            self.assertTrue(path.is_dir(), 'observer overflow or incomplete call')
            try:
                begin = read_json(path / 'begin.json', MAX_RECORD)
                result = read_json(path / 'result.json', MAX_RECORD)
            except (FileNotFoundError, json.JSONDecodeError):
                if pending: return None
                raise AssertionError('incomplete observer invocation remains unknown') from None
            call = main_call(begin, result)
            if call is not None:
                phase = call['phase']
                self.assertEqual(begin['expected_sha256'], self.inputs[phase]['sha256'])
                self.assertEqual(call['source'], self.inputs[phase]['source'])
                identity = (call['child']['pid'], call['child']['birth'])
                self.assertNotIn(identity, identities, 'repeated standalone hook invocation identity')
                identities.add(identity)
                call['invocation'] = path.name
                found.append(call)
        return found

    def emissions(self, run, conversation, emitted, sources):
        calls = self.wait(lambda: [c for c in (self.calls(pending=True) or []) if c['run'] == run and c['session'] == conversation],
                          'completed actual main SessionStart')
        self.assertEqual(len(calls), 1, 'one native start must produce exactly one observed main callback')
        self.assertIn(calls[0]['event_source'], sources)
        self.assertEqual(hint_output(calls[0]['output']), emitted)
        self.checkpoints.append(dict(run=run, conversation=conversation, invocation=calls[0]['invocation'],
                                     emitted=emitted, output_sha256=hashlib.sha256(calls[0]['output']).hexdigest()))
        return calls[0]

    def native_identity(self, name):
        record = self.binding(name)
        pid = record['pid']
        if os.name == 'nt':
            import test_windows_host as host
            from test_windows_host_conpty import image_of
            handle = host.open_host(pid, record['process_start'])
            self.assertTrue(handle, 'native creation identity unavailable')
            try:
                image = image_of(pid)
            finally:
                host.KERNEL32.CloseHandle(handle)
            self.pin_session(name)  # Retain native/supervisor handles while live.
        else:
            out = subprocess.check_output(['/bin/ps', '-ww', '-p', str(pid), '-o', 'lstart=,comm='],
                                          env=dict(self.env, LC_ALL='C', TZ='UTC'), text=True,
                                          timeout=self.remaining(10)).strip().split(None, 5)
            self.assertEqual(' '.join(out[:5]), ' '.join(record['process_start'].split()))
            if sys.platform == 'darwin':
                from test_kimi_native import executable
                image = executable(pid)  # Existing libproc proc_pidpath helper.
            else:
                image = os.readlink('/proc/' + str(pid) + '/exe') if sys.platform.startswith('linux') else out[5]
        self.assertTrue(isinstance(image, (str, os.PathLike)) and os.path.isabs(image),
                        'native absolute image unavailable')
        image = os.fspath(image)
        self.assertIsInstance(image, str, 'native image text unavailable')
        self.assertEqual(os.path.normcase(os.path.realpath(image)),
                         os.path.normcase(os.path.realpath(self.bin / (self.AGENT + ('.exe' if os.name == 'nt' else '')))))
        self.checkpoints.append(dict(native_pid=pid, process_start=record['process_start'], image=image,
                                     run=record['run_id'], phase=self.phase))

    def gate(self, name, run, conversation=None):
        # ONE original 120s end includes late consent, inspection and answers.
        if run in self.completed_runs:
            def steady():
                state, record = self.inspected(name), self.binding(name)
                self.assertEqual(record['run_id'], run)
                self.assertEqual(state.get('run_id'), run)
                self.assertIs(state.get('tracked'), True)
                self.assertFalse(state.get('pending_questions_error'))
                self.assertEqual(state.get('pending_questions'), [])
                ready = public_ready(state, record)
                if ready: self.native_identity(name)
                return ready
            return self.wait(steady, 'public steady conversation gate')
        end = self.startup_ends.setdefault(run, min(self.end, time.monotonic() + 120))
        self.gate_end = end
        try:
            return self._gate(name, run, conversation, end)
        finally:
            self.gate_end = None

    def _gate(self, name, run, conversation, end):
        self.last_gate = dict(name=name, run=run, phase=self.phase, original_end=end)
        while time.monotonic() < end:
            state = self.inspected(name)
            self.last_gate['inspection'] = bounded_observation(state)
            if time.monotonic() >= end: break
            record = self.binding(name)
            self.last_gate['binding'] = bounded_observation(record)
            if time.monotonic() >= end: break
            self.assertEqual(record['run_id'], run)
            self.assertEqual(record['agent'], self.AGENT)
            self.assertIs(state.get('tracked'), True)
            if os.name != 'nt': self.register_unix_run(record)
            if time.monotonic() >= end: break
            selected = startup_answer(state, record, self.attempted)
            if selected:
                key, card, answer = selected
                payload = dict(request_id=str(uuid.uuid4()), expected_run_id=run,
                               expected_conversation_id=record.get('conversation_id') or '',
                               question_id=card['question_id'], expected_question_hash=card['question_hash'],
                               answers=[answer])
                self.attempted.add(key)  # Reserved before action; never rearm uncertainty.
                serialized = json.dumps(payload)
                remaining = min(60, end - time.monotonic())
                self.assertGreater(remaining, 0)
                executable = self.real[self.phase]
                env = dict(self.env)
                if os.name != 'nt': env['HGS_EXECUTABLE'] = str(executable)
                result = self.execute([str(executable), 'answer', name, '--json'], env=env,
                                      input=serialized, timeout=remaining)
                self.assertLess(time.monotonic(), end, 'startup answer exceeded original gate')
                self.assertEqual(result.returncode, 0, result.stderr)
                receipt = json.loads(result.stdout)
                checked_receipt(receipt, payload, serialized, name)
                self.checkpoints.append(dict(consent=card['source'], run=run, receipt=receipt))
                continue
            self.assertFalse(state.get('pending_questions_error'))
            if time.monotonic() >= end: break
            live = next((r for r in self.json_hgs('ls', '--local', '--json')['sessions'] if r['name'] == name), {})
            self.last_gate['live'] = bounded_observation(live)
            public = public_ready(state, record)
            if public and live.get('process_state') == 'running':
                if conversation is not None:
                    self.assertEqual(record.get('expected_id') or record.get('conversation_id'), conversation)
                self.native_identity(name)
                self.assertLess(time.monotonic(), end)
                return
            time.sleep(min(.1, max(0, end - time.monotonic())))
        self.fail('public startup/resume gate remained closed; no prompt or count authorized')

    def launch_hint(self, tag, extra=()):
        project = str(self.project) if os.name == 'nt' else 'p'
        name = f'{self.AGENT}/{"project" if os.name == "nt" else "p"}/{tag}'
        self.names.append(name)
        self.hgs(self.AGENT, project, '--new', '-n', tag, '-d', '--account', 'native-' + self.AGENT,
                 *(['--', *extra] if extra else []))
        if os.name == 'nt': self.status()
        record = self.wait(lambda: self.maybe_binding(name), 'real launch binding')
        self.gate(name, record['run_id'])
        self.assert_registration()
        return name

    def turn(self, name, text, expected_count=1, contexts=()):
        record = self.binding(name)
        self.gate(name, record['run_id'], record.get('expected_id'))
        payload = dict(request_id=str(uuid.uuid4()), expected_run_id=record['run_id'],
                       expected_conversation_id=record.get('conversation_id') or '', text=text, attachments=[])
        receipt = self.json_hgs('send', name, '--json', payload=payload)
        self.assertEqual(receipt['status'], 'submitted')
        def answered():
            path = self.api_root / 'requests.jsonl'
            if not path.exists(): return None
            with path.open('rb') as file: raw = file.read(MAX_REQUESTS + 1)
            self.assertLessEqual(len(raw), MAX_REQUESTS)
            rows = [json.loads(line) for line in raw.splitlines() if line]
            selected = [row for row in rows if row.get('prompt') == text and row.get('route', '').endswith(('/messages', '/responses'))]
            state = self.inspected(name)
            return selected if selected and state.get('activity') == 'idle' and state.get('conversation_id') else None
        rows = self.wait(answered, 'loopback turn completion')
        self.assertTrue(all(row['hint_count'] == expected_count for row in rows), rows)
        self.assertTrue(all(row['fixture_sentinels'].get(text, 0) > 0 for row in rows for text in contexts), rows)
        self.assertNotIn(HINT, json.dumps(self.inspected(name)), 'hint leaked as Activity text')
        self.completed_runs.add(record['run_id'])
        return self.binding(name)

    def pause_hint(self, name):
        self.hgs('pause', name)
        self.wait(lambda: self.binding(name).get('paused'), 'real pause completion')

    def resume_hint(self, name):
        before = self.binding(name)
        self.hgs('resume', name, '-d')
        if os.name == 'nt': self.status()
        record = self.wait(lambda: (r if (r := self.binding(name))['run_id'] != before['run_id'] else None), 'new resume run')
        self.gate(name, record['run_id'], before['conversation_id'])
        return record

    def control(self, command, name):
        record = self.binding(name)
        return self.json_hgs(command, name, '--json', payload=dict(request_id=str(uuid.uuid4()),
                             expected_run_id=record['run_id'], expected_conversation_id=record['conversation_id']))

    def test_lifecycle(self):
        name = self.launch_hint('lifecycle')
        record = self.turn(name, 'Hint fixture startup.')
        self.emissions(record['run_id'], record['conversation_id'], True, {'startup'})
        old_calls = {c['invocation'] for c in self.wait(lambda: self.calls(pending=True), 'complete startup observations')}
        self.control('compact-context', name)
        self.wait(lambda: self.binding(name).get('compact_context_request', {}).get('status') == 'completed', 'real compaction')
        record = self.turn(name, 'Hint fixture after compact.')
        compact = [c for c in self.wait(lambda: self.calls(pending=True), 'complete compact observations')
                   if c['invocation'] not in old_calls and c['event_source'] == 'compact']
        self.assertEqual(len(compact), 1)
        self.assertTrue(hint_output(compact[0]['output']))
        old_conversation = record['conversation_id']
        process_before_clear = (record['run_id'], record['pid'], record['process_start'])
        self.control('clear-context', name)
        record = self.turn(name, 'Hint fixture after clear.')
        self.assertNotEqual(record['conversation_id'], old_conversation)
        self.assertEqual((record['run_id'], record['pid'], record['process_start']), process_before_clear)
        self.emissions(record['run_id'], record['conversation_id'], True, {'clear', 'startup'})
        for number in (1, 2):
            self.pause_hint(name)
            self.resume_hint(name)
            record = self.turn(name, f'Hint fixture marked resume {number}.')
            self.emissions(record['run_id'], record['conversation_id'], False, {'resume'})
        renamed = name.rsplit('/', 1)[0] + '/renamed'
        self.hgs('rename', name, renamed)
        self.names[self.names.index(name)] = renamed
        self.turn(renamed, 'Hint fixture after rename.')
        parent = self.binding(renamed)
        fork = renamed.rsplit('/', 1)[0] + '/forked'
        self.names.append(fork)
        self.hgs('fork', renamed, '-n', 'forked', '-d', '--expected-run-id', parent['run_id'],
                 '--expected-conversation-id', parent['conversation_id'])
        if os.name == 'nt': self.status()
        record = self.wait(lambda: self.maybe_binding(fork), 'real fork binding')
        self.assertEqual(record['fork_parent_id'], parent['conversation_id'])
        self.gate(fork, record['run_id'])
        record = self.turn(fork, 'Hint fixture inherited fork.')
        self.assertNotEqual(record['conversation_id'], parent['conversation_id'])
        self.emissions(record['run_id'], record['conversation_id'], False, {'startup', 'resume', 'fork'})
        self.assert_registration()

    def test_lost_mark(self):
        name = self.launch_hint('lost-mark')
        first = self.turn(name, 'Hint fixture before fault.')
        before = self.emissions(first['run_id'], first['conversation_id'], True, {'startup'})
        self.pause_hint(name)
        path = self.binding_path(name)
        record = read_json(path, MAX_RECORD)
        self.assertEqual((record['run_id'], record['conversation_id']), (first['run_id'], first['conversation_id']))
        self.assertTrue(record['paused'])
        changed = dict(record)
        removed = changed.pop('agent_hint')  # The ONE explicit binding fault.
        self.assertEqual({k: v for k, v in record.items() if k != 'agent_hint'}, changed)
        temporary = path.with_suffix('.hint-fault.tmp')
        temporary.write_text(json.dumps(changed))
        os.replace(temporary, path)
        self.checkpoints.append(dict(fault='remove agent_hint only from paused binding', removed=removed,
                                     run=first['run_id'], conversation=first['conversation_id']))
        self.resume_hint(name)
        second = self.turn(name, 'Hint fixture after fault.', 2 if self.AGENT == 'codex' else 1)
        emitted = self.emissions(second['run_id'], first['conversation_id'], True, {'resume'})
        self.assertNotEqual(emitted['invocation'], before['invocation'])
        self.assertNotEqual(emitted['child'], before['child'])
        self.pause_hint(name)
        self.resume_hint(name)
        third = self.turn(name, 'Hint fixture after repaired mark.', 2 if self.AGENT == 'codex' else 1)
        self.emissions(third['run_id'], first['conversation_id'], False, {'resume'})

    def test_pre_feature_first_resume(self):
        self.set_phase('pre_feature')
        name = self.launch_hint('pre-feature')
        before = self.turn(name, 'Hint fixture real ad328 history.', 0)
        self.emissions(before['run_id'], before['conversation_id'], False, {'startup'})
        self.assertNotIn('agent_hint', before)
        self.pause_hint(name)
        self.set_phase('candidate')
        self.resume_hint(name)
        first = self.turn(name, 'Hint fixture first upgraded resume.')
        self.emissions(first['run_id'], before['conversation_id'], True, {'resume'})
        self.pause_hint(name)
        self.resume_hint(name)
        second = self.turn(name, 'Hint fixture second upgraded resume.')
        self.emissions(second['run_id'], before['conversation_id'], False, {'resume'})

    def test_optouts_and_instructions(self):
        # Native project instructions and user flags are context sentinels. The
        # Unix project launcher is a supported exec wrapper; Windows starts the
        # actual exe directly and makes no unsupported .sh-launcher claim.
        instruction = self.project / ('CLAUDE.md' if self.AGENT == 'claude' else 'AGENTS.md')
        instruction.write_text(PROJECT_SENTINEL + '\n')
        settings = self.root / 'user-settings.json'
        settings.write_text(json.dumps(dict(env=dict(ZERUS_FIXTURE_SETTING='preserved'))))
        flags = (('--append-system-prompt', APPEND_SENTINEL, '--system-prompt', SYSTEM_SENTINEL,
                  '--settings', str(settings)) if self.AGENT == 'claude' else
                 ('-c', 'developer_instructions=' + json.dumps(DEVELOPER_SENTINEL)))
        contexts = (PROJECT_SENTINEL, APPEND_SENTINEL, SYSTEM_SENTINEL) if self.AGENT == 'claude' else (PROJECT_SENTINEL, DEVELOPER_SENTINEL)
        if os.name != 'nt':
            launcher = self.project / ('run_' + self.AGENT + '.sh')
            launcher.write_text('#!/bin/sh\nprintf ran >> ' + shlex.quote(str(self.root / 'launcher-sentinel'))
                                + '\nexec ' + shlex.quote(str(self.bin / self.AGENT)) + ' "$@"\n')
            launcher.chmod(0o700)
        for tag, enabled, off in (('global-off', '0', ''), ('account-off', '1', 'native-' + self.AGENT),
                                   ('unexcluded', '1', 'different-account')):
            self.env.update(HGS_AGENT_HINT=enabled, HGS_AGENT_HINT_OFF=off)
            name = self.launch_hint(tag, flags)
            expected = 1 if tag == 'unexcluded' else 0
            record = self.turn(name, 'Hint fixture ' + tag + '.', expected, contexts)
            self.emissions(record['run_id'], record['conversation_id'], bool(expected), {'startup'})
            if not expected: self.assertNotIn('agent_hint', record)
            saved = list(record['base'])
            self.pause_hint(name)
            self.resume_hint(name)
            resumed = self.turn(name, 'Hint fixture resumed ' + tag + '.', expected, contexts)
            self.assertEqual(resumed['base'], saved)
            self.assertTrue(all(value in saved for value in flags))
            self.emissions(resumed['run_id'], resumed['conversation_id'], False, {'resume'})
            self.assertEqual(instruction.read_text(), PROJECT_SENTINEL + '\n')
            self.assertEqual(json.loads(settings.read_text()), dict(env=dict(ZERUS_FIXTURE_SETTING='preserved')))
            self.assertTrue((self.root / 'personal-hook').is_file())
            self.assert_registration()
            self.pause_hint(name)
        if os.name != 'nt': self.assertTrue((self.root / 'launcher-sentinel').is_file())

    def save_cleanup(self, stage, problems=()):
        document = dict(stage=stage, verified=self.cleanup_verified, original_end=self.row_end,
                        runtime=str(getattr(self, 'root', '')), project_root=str(getattr(self, 'project_parent', '') or ''),
                        intents=self.cleanup_intents, problems=list(problems),
                        errors=getattr(self, 'cleanup_errors', []),
                        errors_overflow=getattr(self, 'cleanup_errors_overflow', False))
        encoded = json.dumps(document, indent=2).encode()
        self.assertLessEqual(len(encoded), MAX_RECORD, 'cleanup evidence bound')
        temporary = self.result_dir / 'cleanup.tmp'
        temporary.write_bytes(encoded)
        os.replace(temporary, self.result_dir / 'cleanup.json')

    def note_cleanup_error(self, stage, error, **context):
        if not hasattr(self, 'cleanup_errors'):
            self.cleanup_errors, self.cleanup_errors_overflow = [], False
        try:
            if len(self.cleanup_errors) < 16:
                self.cleanup_errors.append(cleanup_error(stage, error, **context))
            else:
                self.cleanup_errors_overflow = True
        except BaseException:
            # The original cleanup problem remains failed/UNKNOWN. Diagnostics
            # must not prevent independent stop, reap and seal attempts.
            self.cleanup_errors_overflow = True

    def unix_query(self, argv):
        self.remaining(1)
        result = subprocess.run(argv, env=dict(self.env, LC_ALL='C', TZ='UTC'), capture_output=True,
                                text=True, timeout=self.remaining(3))
        self.remaining(1)
        self.assertLessEqual(len(result.stdout.encode()) + len(result.stderr.encode()), MAX_REQUESTS,
                             'Unix custody query bound')
        return result

    def unix_table(self):
        result = self.unix_query(['/bin/ps', '-axo', 'pid=,ppid=,stat=,lstart='])
        self.assertEqual(result.returncode, 0, 'UNKNOWN process snapshot')
        rows = {}
        for line in result.stdout.splitlines():
            fields = line.split(None, 3)
            self.assertTrue(len(fields) == 4 and fields[0].isdigit() and fields[1].isdigit()
                            and fields[2] and len(fields[3].split()) == 5, 'UNKNOWN process snapshot row')
            pid = int(fields[0])
            self.assertNotIn(pid, rows, 'UNKNOWN duplicate process identity')
            rows[pid] = (int(fields[1]), fields[2], ' '.join(fields[3].split()))
            self.assertLessEqual(len(rows), 16384, 'process snapshot entry bound')
        self.assertTrue(rows, 'UNKNOWN empty process snapshot')
        return rows

    def unix_image(self, pid, birth):
        def check():
            result = self.unix_query(['/bin/ps', '-p', str(pid), '-o', 'lstart='])
            self.assertEqual(result.returncode, 0, 'UNKNOWN owned birth identity')
            self.assertEqual(' '.join(result.stdout.split()), birth, 'UNKNOWN changed owned birth')
        check()
        self.remaining(1)
        if sys.platform == 'darwin':
            from test_kimi_native import executable
            image = executable(pid)
        elif sys.platform.startswith('linux'):
            image = os.readlink('/proc/' + str(pid) + '/exe')
        else:
            raise AssertionError('UNKNOWN Unix kernel image adapter')
        self.remaining(1)
        check()
        self.assertTrue(isinstance(image, (str, os.PathLike)) and os.path.isabs(image),
                        'UNKNOWN owned absolute image')
        image = os.path.realpath(image)
        self.remaining(1)
        return image

    def unix_remember(self, pid, birth, role, expected, rows):
        self.assertTrue(type(pid) is int and pid > 0 and isinstance(birth, str) and birth,
                        'UNKNOWN owned Unix identity')
        birth = ' '.join(birth.split())
        self.assertIn(pid, rows, 'UNKNOWN missing owned process')
        self.assertEqual(rows[pid][2], birth, 'UNKNOWN owned process birth')
        image = self.unix_image(pid, birth)
        self.remaining(1)
        self.assertIn(image, {os.path.realpath(path) for path in expected}, 'UNKNOWN owned process image')
        self.remaining(1)
        self.assertLess(len(self.unix_owned), 2048, 'owned Unix identity bound')
        self.unix_owned[(pid, birth)] = dict(role=role, image=image)
        return dict(pid=pid, birth=birth, image=image)

    def unix_live_snapshot(self):
        self.remaining(1)
        self.assertEqual(Path(self.socket), self.root / 'tmux.sock', 'UNKNOWN private socket scope')
        self.assertTrue(Path(self.socket).is_absolute(), 'UNKNOWN relative private socket')
        self.assertTrue(all(character.isprintable() and character != '|' for character in self.socket),
                        'UNKNOWN private socket cannot be framed unambiguously')
        rows = self.unix_table()
        panes = self.unix_query([test_pause.TMUX, '-S', self.socket, 'list-panes', '-a', '-F',
                                 '#{pid}|#{socket_path}|#{pane_pid}'])
        if panes.returncode == 1 and self.unix_endpoint_absent():
            observe_unix_descendants(self.unix_owned, rows)
            return rows, None, set()
        self.assertEqual(panes.returncode, 0, 'UNKNOWN private socket identity')
        self.assertFalse(self.unix_endpoint_absent(), 'UNKNOWN vanished private server endpoint')
        lines = panes.stdout.splitlines()
        self.assertTrue(0 < len(lines) <= 16, 'UNKNOWN private pane inventory')
        server, pane_ids = None, set()
        for line in lines:
            fields = line.split('|')
            self.assertTrue(len(fields) == 3 and fields[0].isdigit() and fields[2].isdigit(),
                            'UNKNOWN private pane/server identity')
            pid, path, pane = int(fields[0]), fields[1], int(fields[2])
            self.assertEqual(os.path.realpath(path), os.path.realpath(self.socket), 'UNKNOWN server socket scope')
            self.remaining(1)
            self.assertIn(pid, rows, 'UNKNOWN private server process')
            identity = self.unix_remember(pid, rows[pid][2], 'server', [test_pause.TMUX], rows)
            identity['socket'] = str(self.socket)
            self.assertTrue(server is None or server == identity, 'UNKNOWN multiple private servers')
            server = identity
            self.assertIn(pane, rows, 'UNKNOWN private pane process')
            self.unix_remember(pane, rows[pane][2], 'supervisor', list(self.real.values()), rows)
            pane_ids.add(pane)
        observe_unix_descendants(self.unix_owned, rows)
        return rows, server, pane_ids

    def unix_endpoint_absent(self):
        return private_endpoint_absent(self.socket, self.remaining)

    def register_unix_run(self, record):
        run = record.get('run_id')
        if run in self.unix_runs: return
        self.assertTrue(isinstance(run, str) and run, 'UNKNOWN new run identity')
        pending = [item for item in self.cleanup_intents if item.get('unix_registered') is not True]
        self.assertEqual(len(pending), 1, 'UNKNOWN new run launch custody')
        rows, server, panes = self.unix_live_snapshot()
        self.assertIsNotNone(server, 'UNKNOWN new run private server')
        self.assertEqual(server, pending[0].get('unix_server'), 'UNKNOWN new run server changed')
        supervisor = record.get('supervisor') or {}
        self.assertIn(supervisor.get('pid'), panes, 'UNKNOWN new run private pane')
        native = self.unix_remember(record.get('pid'), record.get('process_start'), 'native',
                                    [self.native_copy], rows)
        parent = self.unix_remember(supervisor.get('pid'), supervisor.get('start'), 'supervisor',
                                    list(self.real.values()), rows)
        self.assertEqual(rows[native['pid']][0], parent['pid'], 'UNKNOWN new run supervised child')
        observe_unix_descendants(self.unix_owned, rows)
        pending[0].update(unix_registered=True, unix_run=run, unix_native=native, unix_supervisor=parent)
        self.unix_runs.add(run)
        self.save_cleanup('new run custody complete')

    def stop_unix(self):
        if not hasattr(self, 'socket') or not hasattr(self, 'env'):
            self.assertEqual(self.cleanup_intents, [], 'UNKNOWN partial launch namespace')
            return
        rows, server, _ = self.unix_live_snapshot()
        if server is None:
            unix_absence_proof(self.unix_owned, rows, self.cleanup_intents, True)
        else:
            self.stop_private_socket()
        while observe_unix_descendants(self.unix_owned, self.unix_table()):
            self.assertLess(time.monotonic(), self.row_end - 5, 'UNKNOWN owned descendants at original end')
            time.sleep(min(.05, self.remaining(.05)))
        unix_absence_proof(self.unix_owned, self.unix_table(), self.cleanup_intents,
                           self.unix_endpoint_absent())
        self.checkpoints.append(dict(cleanup='registered private server/run identities and observed descendants exited',
            owned=[dict(pid=pid, birth=birth, **value) for (pid, birth), value in self.unix_owned.items()]))

    def stop_private_socket(self):
        # Also attempted independently if process discovery failed before stop;
        # never retry a previous stop or signal a process by a discovered PID.
        if getattr(self, 'socket_stop_attempted', False): return
        if not getattr(self, 'socket', None): return
        self.socket_stop_attempted = True
        rows, server, _ = self.unix_live_snapshot()
        if server is None:
            unix_absence_proof(self.unix_owned, rows, self.cleanup_intents, True)
            return
        stopped = self.unix_query([test_pause.TMUX, '-S', self.socket, 'kill-server'])
        if stopped.returncode == 1:
            unix_absence_proof(self.unix_owned, self.unix_table(), self.cleanup_intents,
                               self.unix_endpoint_absent())
        else:
            self.assertEqual(stopped.returncode, 0, 'UNKNOWN private socket stop')

    def stop_host(self):
        """Only the hint classes use this; no inherited renewed wait or PID kill."""
        import test_windows_host as host
        problems = []
        query = None
        def image(handle):
            nonlocal query
            if query is None: query = pinned_image_query()
            return query(handle)
        try:
            status = self.status()  # Private endpoint also covers failed launches.
            if status.get('running') is True:
                self.assertIn(status['host']['pid'], self.hosts, 'UNKNOWN live host handle')
            for name in self.names:
                record = self.maybe_binding(name)
                if record:
                    self.pin_session(name)
                    if not record.get('paused') and not record.get('session_end'):
                        self.assertIn((record.get('pid'), record.get('process_start')), self.pinned,
                                      'UNKNOWN live native handle')
            if any(host.KERNEL32.WaitForSingleObject(handle, 0) != host.WAIT_OBJECT_0
                   for handle in self.hosts.values()):
                panes = self.ok({'op': 'list'})['panes']
                self.assertLessEqual(len(panes), 16, 'private host pane bound')
                for pane in panes: self.ok({'op': 'kill', 'target': {'pane': pane['pane']}})
                drain_end = min(self.row_end - 5, time.monotonic() + self.remaining(5))
                wait_owned_sessions_empty(lambda: self.ok({'op': 'list'})['sessions'], self.remaining, drain_end)
                self.ok({'op': 'stop_host'})
        except BaseException as error:
            problems.append('private stop: ' + type(error).__name__)
            self.note_cleanup_error('private stop', error)
        # Host job is no-breakaway and kill-on-close. Pinned handles, not a PID
        # search, are the only forced-stop authority. Missing proof stays unknown.
        handles = [(pid, handle) for pid, handle in self.hosts.items()]
        handles += [(pid, handle) for (pid, _), handle in self.pinned.items()]
        for pid, handle in handles:
            try:
                stop_pinned_handle(handle, lambda: image(handle), self.root,
                                   host.KERNEL32.WaitForSingleObject, host.KERNEL32.TerminateProcess, self.remaining)
            except BaseException as error:
                problems.append('owned handle: ' + type(error).__name__)
                self.note_cleanup_error('owned handle', error, pid=pid)
            finally:
                host.KERNEL32.CloseHandle(handle)
        self.hosts.clear()
        self.pinned.clear()
        files = list(itertools.islice(self.root.rglob('*'), 4097))
        self.assertLessEqual(len(files), 4096, 'owned file-release inventory bound')
        for path in files:
            self.remaining(1)
            if path.suffix.lower() in ('.exe', '.dll'):
                from test_windows_native_agents import in_use
                if in_use(path): problems.append('owned executable still held')
        self.assertEqual(problems, [], 'UNKNOWN Windows owned cleanup; retain runtime')
        self.checkpoints.append(dict(cleanup='pinned owned handles exited; files released'))

    def cleanup_owned(self):
        self.cleaning, self.gate_end = True, None
        problems = []
        # Independent stop/reap/check attempts share ONE end, even after a
        # prior failure. No TemporaryDirectory finalizer can erase unknown work.
        operations = ([self.stop_host] if os.name == 'nt' else [self.stop_unix, self.stop_private_socket])
        def direct_children():
            for child in self.cli_children:
                if child.poll() is None:
                    self.remaining(1)
                    child.kill()  # Only this unreaped direct child's custody.
                child.wait(timeout=self.remaining(3))
        operations += [direct_children, self.close_model]
        if hasattr(self, 'native_copy'):
            operations.append(lambda: self.keep_evidence('after owned stop'))
        operations.append(self.final_observer_check)
        for operation in operations:
            try:
                self.remaining(1)
                operation()
            except BaseException as error:
                problems.append(type(error).__name__ + ': ' + str(error)[:400])
                self.note_cleanup_error(getattr(operation, '__name__', 'cleanup operation'), error)
        self.cleanup_verified = not problems and time.monotonic() < self.row_end
        self.save_cleanup('verified before deletion' if self.cleanup_verified else 'UNKNOWN: runtime retained', problems)
        if self.cleanup_verified:
            try:
                self.remaining(1)
                if getattr(self, 'project_parent', None): shutil.rmtree(self.project_parent)
                if hasattr(self, 'root'): shutil.rmtree(self.root)
                self.remaining(1)
            except BaseException as error:
                self.cleanup_verified = False
                problems.append('runtime removal: ' + type(error).__name__)
                self.save_cleanup('UNKNOWN: runtime removal incomplete', problems)
        self.assertTrue(self.cleanup_verified, 'UNKNOWN owned cleanup; retained namespaces: ' + str(getattr(self, 'root', '')))

    def image_seals(self):
        expected = [(name + '_input', Path(value['path']), value['sha256']) for name, value in self.inputs.items()]
        expected += [(phase + '_private_cli', self.real[phase], self.inputs[phase]['sha256'])
                     for phase in ('candidate', 'pre_feature')]
        expected += [('observer_private', self.observer_exe, self.inputs['observer']['sha256']),
                     ('native_input', self.native_input, self.native_sha256),
                     ('native_private', self.native_copy, self.native_sha256),
                     ('current_stable_cli', self.real_hook, self.inputs[self.phase]['sha256'])]
        if hasattr(self, 'hgs_exe'):
            expected.append(('initial_portable_candidate', self.hgs_exe, self.inputs['candidate']['sha256']))
        if hasattr(self, 'conpty_inputs'):
            for name in ('conpty.dll', 'OpenConsole.exe'):
                value = self.conpty_inputs[name]
                expected.append(('conpty_input:' + name, Path(value['path']), value['sha256']))
                for phase in ('candidate', 'pre_feature'):
                    expected.append((phase + '_conpty:' + name, self.real[phase].parent / name, value['sha256']))
                expected.append(('initial_portable_conpty:' + name, self.hgs_exe.parent / name, value['sha256']))
        return verify_image_seals(expected, self.row_end)

    def keep_evidence(self, stage):
        destination = self.result_dir
        self.checkpoints.append(dict(stage=stage, phase=self.phase, image_seals=self.image_seals()))
        for name, path in (('settings.json', self.settings_path), ('requests.jsonl', self.api_root / 'requests.jsonl')):
            if path.exists(): shutil.copy2(path, destination / name)
        (destination / 'checkpoints.json').write_text(json.dumps(self.checkpoints, indent=2))

    def final_observer_check(self):
        if hasattr(self, 'observations'):
            calls = self.calls()  # No incomplete output/forwarding after owned cleanup.
            (self.result_dir / 'final-calls.json').write_text(json.dumps([
                {key: value for key, value in call.items() if key != 'output'} for call in calls], indent=2))


if os.name != 'nt':
    @unittest.skipUnless(INPUTS in os.environ, 'select the reviewed agent-hint contract')
    class NativeClaudeHint(HintContract, test_pause.Harness):
        AGENT = 'claude'

    @unittest.skipUnless(INPUTS in os.environ, 'select the reviewed agent-hint contract')
    class NativeCodexHint(HintContract, test_pause.Harness):
        AGENT = 'codex'


if os.name == 'nt':
    import test_windows_native_agents as windows
    import test_windows_host as host

    class HintHostCase(host.HostCase):
        """Same private HostCase setup, without its automatic GC/runtime deletion."""
        def setUp(self):
            if not host.HGS.is_file(): raise AssertionError('sealed hgs input is missing')
            self.root = Path(tempfile.mkdtemp(prefix='hgs-hint-host-'))
            self.state, self.home, self.work = (self.root / name for name in ('state', self.PROFILE, 'work'))
            for path in (self.state, self.home, self.work): path.mkdir()
            self.env = {key: value for key, value in os.environ.items()
                        if not key.upper().startswith(('HGS_', 'FAKE_AGENT_', 'TMUX', 'XDG_'))}
            self.env.update(HGS_STATE_DIR=str(self.state), USERPROFILE=str(self.home), HOME=str(self.home),
                            HGS_HOST_IMAGE=str(host.HGS), HGS_HOST_ALLOW_UNSTAGED='1')
            self.unique = f'hgs-hint-host-{os.getpid()}-{time.monotonic_ns()}'
            self.launches, self.hosts = 0, {}

    @unittest.skipUnless(INPUTS in os.environ, 'select the reviewed agent-hint contract')
    class NativeWindowsClaudeHint(HintContract, windows.NativeAgent, HintHostCase):
        AGENT, BINARY, COMPOSER = 'claude', os.environ.get('HGS_CLAUDE_TEST_BIN'), '❯'
        CONPTY = windows.CONPTY

    @unittest.skipUnless(INPUTS in os.environ, 'select the reviewed agent-hint contract')
    class NativeWindowsCodexHint(HintContract, windows.NativeAgent, HintHostCase):
        AGENT, BINARY, COMPOSER = 'codex', os.environ.get('HGS_CODEX_TEST_BIN'), '›'
        CONPTY = windows.CONPTY


if __name__ == '__main__':
    unittest.main()
