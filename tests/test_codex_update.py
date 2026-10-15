#!/usr/bin/env python3
"""Codex's startup update picker in an isolated native-like TUI; never runs an installer."""
import datetime
import json
import os
import signal
import subprocess
import time
import unittest
import uuid

import test_clear_context_native
import test_input as fixtures
from test_questions import Questions

FAKE = fixtures.SIGNAL_SAFE_READ + r'''
import os,pathlib,select,signal,sys,termios,time,tty
root=pathlib.Path(os.environ['INPUT_FIXTURE']);fd=sys.stdin.fileno();original=termios.tcgetattr(fd);tty.setraw(fd)
os.write(1,b'\x1b[?2004h');selected=0;buf=b''
command="sh -c 'curl -fsSL https://chatgpt.com/codex/install.sh | CODEX_NON_INTERACTIVE=1 sh'"
labels=['Update now (runs `'+command+'`)','Skip','Skip until next version']
def render(*args):
 global selected
 if (root/'selection').exists():selected=int((root/'selection').read_text());(root/'selection').unlink()
 latest=(root/'latest').read_text() if (root/'latest').exists() else '0.160.1'
 lines=['','  Update available · 0.160.0 → '+latest,
        '  Release notes: https://github.com/openai/codex/releases/latest','']
 first=('› ' if selected==0 else '  ')+'1. Update now (runs `'+command+'`)'
 lines += [first[:80],'     '+first[80:].strip()]
 for i in (1,2):lines += [('› ' if selected==i else '  ')+str(i+1)+'. '+labels[i]]
 lines += ['','  enter continue · esc skip']
 os.write(1,b'\x1b[2J\x1b[H'+'\r\n'.join(lines).encode())
signal.signal(signal.SIGUSR1,render);render();(root/'agent-pid').write_text(str(os.getpid()))
try:
 while True:
  value=read_input(fd)
  if not value:break
  with (root/'received').open('ab') as f:f.write(value)
  buf+=value
  while buf:
   if buf.startswith((b'\x1b[A',b'\x1bOA')):selected=(selected-1)%3;buf=buf[3:];render()
   elif buf.startswith((b'\x1b[B',b'\x1bOB')):selected=(selected+1)%3;buf=buf[3:];render()
   elif buf.startswith(b'\r'):
    buf=buf[1:];(root/'submitted').write_text(labels[selected])
    if (root/'hold-dialog').exists():continue
    if selected==0:
     os.write(1,('\x1b[2J\x1b[H\r\nUpdating Codex via `'+command+'`...\r\n').encode())
     if (root/'exit-after-update').exists():sys.exit(0)
     # Like Codex: leave the TUI, run the installer for the given seconds, then exit.
     # Keep recording input meanwhile, so a resent key stays visible to the test.
     if (root/'installer').exists():
      termios.tcsetattr(fd,termios.TCSANOW,original);end=time.monotonic()+float((root/'installer').read_text())
      while (left:=end-time.monotonic())>0:
       if select.select([fd],[],[],left)[0]:
        with (root/'received').open('ab') as f:f.write(os.read(fd,65536))
      sys.exit(0)
    else:
     # Codex keeps announcing the newer version in its main view after a skip.
     os.write(1,'\x1b[2J\x1b[H✨ Update available! 0.160.0 -> 0.160.1\r\nReady for your first message'.encode())
   elif buf[0]==27 and len(buf)<3:break
   else:buf=buf[1:]
finally:termios.tcsetattr(fd,termios.TCSANOW,original)
'''


class CodexUpdate(unittest.TestCase):
    script = fixtures.InputTransport.script
    tmux = fixtures.InputTransport.tmux
    wait_for = fixtures.InputTransport.wait_for
    received = fixtures.InputTransport.received
    write_record = fixtures.InputTransport.write_record
    tearDown = fixtures.InputTransport.tearDown
    inspect = Questions.inspect
    answer = Questions.answer

    def setUp(self):
        original = fixtures.FAKE_AGENT
        try:
            fixtures.FAKE_AGENT = FAKE
            fixtures.InputTransport.setUp(self)
        finally:
            fixtures.FAKE_AGENT = original
        self.conversation_id = ''
        self.record.update(agent='codex', run_identity_version=1, supervisor={},
                           conversation_id=None, launch_dir='/work/example',
                           phase='unknown', activity='unknown', last_event_at=0)
        self.write_record()
        self.card = self.inspect()['pending_questions'][0]

    def payload(self, choice='skip', **updates):
        result = dict(request_id=str(uuid.uuid4()), expected_run_id=self.run_id,
                      expected_conversation_id='', question_id=self.card['question_id'],
                      expected_question_hash=self.card['question_hash'],
                      answers=[dict(question_id='codex_update', selected_option_ids=[choice], text='')])
        result.update(updates)
        return result

    def redraw(self, file, value, expected):
        (self.root / file).write_text(value)
        os.kill(self.record['pid'], signal.SIGUSR1)
        self.wait_for(lambda: expected in self.tmux('capture-pane', '-p', '-t', self.pane))

    def test_startup_card_discloses_command_and_blocks_messages(self):
        state = self.inspect()
        self.assertEqual(state['phase'], 'input')
        self.assertEqual(state['attention_id'], self.card['question_id'])
        self.assertTrue(self.card['question_id'].startswith('codex-update:'))
        self.assertEqual(self.card['source'], 'codex_update')
        self.assertTrue(self.card['can_answer'])
        item = self.card['questions'][0]
        self.assertIn('0.160.0 → 0.160.1', item['question'])
        self.assertEqual([option['id'] for option in item['options']], ['update_now', 'skip', 'skip_version'])
        self.assertIn('install.sh | CODEX_NON_INTERACTIVE=1 sh', item['options'][0]['label'])
        self.assertIn('Codex exits', item['options'][0]['description'])
        self.assertGreater(self.card['created_at'], 0)
        self.assertIn('update choice', fixtures.InputTransport.send(self, fixtures.InputTransport.payload(self), success=False))
        self.assertEqual(self.received(), b'')

    def test_skip_choices_survive_the_native_update_notice(self):
        for choice, label, selection in [('skip', 'Skip', '2'), ('skip_version', 'Skip until next version', '0')]:
            with self.subTest(choice=choice):
                self.redraw('selection', selection, '› ' + str(int(selection) + 1) + '.')
                self.card = self.inspect()['pending_questions'][0]
                request = self.payload(choice)
                receipt = self.answer(request)
                self.assertEqual(receipt['status'], 'answered')
                self.assertFalse(receipt.get('open_terminal', False))
                self.assertEqual((self.root / 'submitted').read_text(), label)
                before = self.received()
                self.assertEqual(self.answer(request), receipt)
                self.assertEqual(self.received(), before)
                self.assertEqual(self.inspect()['pending_questions'], [])

    def test_update_now_opens_terminal_and_accepts_native_exit(self):
        receipt = self.answer(self.payload('update_now'))
        self.assertEqual(receipt['status'], 'answered')
        self.assertTrue(receipt['open_terminal'])
        self.assertEqual(self.received(), b'\r')
        self.assertTrue((self.root / 'submitted').read_text().startswith('Update now'))
        self.redraw('selection', '0', '› 1.')
        (self.root / 'exit-after-update').touch()
        self.card = self.inspect()['pending_questions'][0]
        self.assertEqual(self.answer(self.payload('update_now'))['status'], 'answered')

    def test_update_now_accepts_exit_after_the_installer(self):
        # The terminal stops being an input terminal before Codex exits.
        (self.root / 'installer').write_text('0.3')
        receipt = self.answer(self.payload('update_now'))
        self.assertEqual(receipt['status'], 'answered')
        self.assertTrue(receipt['open_terminal'])
        self.assertEqual(self.received(), b'\r')

    def test_update_now_installer_outliving_acknowledgement_is_never_retried(self):
        (self.root / 'installer').write_text('5')
        request = self.payload('update_now')
        self.assertIn('delivery uncertain', self.answer(request, success=False))
        self.assertEqual(self.received(), b'\r')
        self.assertIn('delivery uncertain', self.answer(request, success=False))
        self.assertEqual(self.received(), b'\r')

    def test_changed_version_or_run_rejects_without_input(self):
        self.answer(self.payload(expected_run_id='other'), success=False)
        self.redraw('latest', '0.161.0', '0.161.0')
        self.assertIn('question', self.answer(self.payload('skip'), success=False))
        self.assertEqual(self.received(), b'')
        for field, value in [('conversation_id', 'started'), ('supervisor', None), ('last_event_at', time.time())]:
            with self.subTest(field=field):
                previous = self.record.get(field)
                self.record[field] = value
                self.write_record()
                self.assertEqual(self.inspect()['pending_questions'], [])
                self.record[field] = previous
                self.write_record()

    def test_no_acknowledgement_is_uncertain_and_never_retried(self):
        (self.root / 'hold-dialog').touch()
        request = self.payload('skip')
        self.assertIn('delivery uncertain', self.answer(request, success=False))
        before = self.received()
        self.assertIn('delivery uncertain', self.answer(request, success=False))
        self.assertEqual(self.received(), before)


@unittest.skipUnless(os.environ.get('HGS_CODEX_TEST_BIN'), 'Set HGS_CODEX_TEST_BIN for the isolated native Codex update test')
class NativeCodexUpdate(unittest.TestCase):
    """The real picker from a cached newer version; only skip choices are answered."""
    script = fixtures.InputTransport.script
    tmux = fixtures.InputTransport.tmux
    write_record = fixtures.InputTransport.write_record
    received = fixtures.InputTransport.received
    tearDown = fixtures.InputTransport.tearDown
    inspect = Questions.inspect
    answer = Questions.answer
    payload = CodexUpdate.payload
    assert_plugin_sync_off = test_clear_context_native.NativeCodexClear.assert_plugin_sync_off

    def wait_for(self, callback, timeout=15):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if callback():
                return
            time.sleep(.05)
        self.fail('native fixture timed out:\n' + self.tmux('capture-pane', '-p', '-t', self.pane, check=False))

    def setUp(self):
        fixtures.InputTransport.setUp(self)
        self.tmux('kill-session', '-t', self.name)
        self.native_home = self.root / '.codex'
        self.native_home.mkdir()
        binary = os.environ['HGS_CODEX_TEST_BIN']
        self.native_version = subprocess.check_output([binary, '--version'], text=True).strip().removeprefix('codex-cli ')
        (self.native_home / 'config.toml').write_text('model="gpt-6.1-sol"\nmodel_provider="fixture"\n'
            '[features]\nplugins=false\n'
            '[model_providers.fixture]\nname="Fixture"\nbase_url="http://127.0.0.1:1/v1"\nwire_api="responses"\n')
        self.assert_plugin_sync_off(self.native_home)
        # A fresh cache keeps Codex from fetching release metadata during the test.
        checked = datetime.datetime.now(datetime.timezone.utc).isoformat().replace('+00:00', 'Z')
        self.version_file = self.native_home / 'version.json'
        self.version_file.write_text(json.dumps(dict(latest_version='999.0.0', last_checked_at=checked, dismissed_version=None)))
        width, height = os.environ.get('HGS_CODEX_TEST_SIZE', '110x35').split('x')
        # The npm install context selects a stable command without a package manager.
        self.tmux('new-session', '-d', '-s', self.name, '-x', width, '-y', height,
                  'env', 'CODEX_HOME=' + str(self.native_home), 'CODEX_MANAGED_BY_NPM=1', binary, '--no-daemon',
                  '-c', 'tui.animations=false', '-C', str(self.root))
        self.pane = self.tmux('display-message', '-p', '-t', '=' + self.name + ':', '#{pane_id}').strip()
        self.tmux('set-option', '-t', '=' + self.name + ':', '@hgs_run', self.run_id)
        self.wait_for(lambda: 'enter continue · esc skip' in self.tmux('capture-pane', '-p', '-t', self.pane))
        pid = int(self.tmux('display-message', '-p', '-t', self.pane, '#{pane_pid}').strip())
        start = subprocess.check_output(['ps', '-p', str(pid), '-o', 'lstart='],
                                        env=dict(self.env, LC_ALL='C', TZ='UTC'), text=True).strip()
        self.conversation_id = ''
        self.record.update(agent='codex', run_identity_version=1, supervisor={}, conversation_id=None,
                           pid=pid, process_start=start, pane=self.pane, launch_dir=str(self.root),
                           agent_home=str(self.native_home), phase='unknown', activity='unknown', last_event_at=0)
        self.write_record()
        self.card = self.inspect()['pending_questions'][0]

    def test_native_picker_identity_and_skip_choices(self):
        item = self.card['questions'][0]
        self.assertEqual(self.card['source'], 'codex_update')
        self.assertEqual(item['question'], 'Update Codex ' + self.native_version + ' → 999.0.0?')
        self.assertEqual(item['options'][0]['label'], 'Update now (runs `npm install -g @openai/codex`)')
        receipt = self.answer(self.payload('skip'))
        self.assertEqual(receipt['status'], 'answered')
        self.assertNotIn('enter continue · esc skip', self.tmux('capture-pane', '-p', '-t', self.pane))
        self.assertIsNone(json.loads(self.version_file.read_text())['dismissed_version'])
        self.assertFalse([q for q in self.inspect()['pending_questions'] if q['source'] == 'codex_update'])

    def test_native_skip_until_next_version_is_recorded_by_codex(self):
        self.tmux('send-keys', '-t', self.pane, 'Down')
        self.wait_for(lambda: '› 2. Skip' in self.tmux('capture-pane', '-p', '-t', self.pane))
        self.card = self.inspect()['pending_questions'][0]
        self.assertEqual(self.answer(self.payload('skip_version'))['status'], 'answered')
        self.wait_for(lambda: json.loads(self.version_file.read_text()).get('dismissed_version') == '999.0.0')


if __name__ == '__main__':
    unittest.main()
