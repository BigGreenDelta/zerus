"""Peer commands keep their meaning in every login shell, including fish.

ssh hands its last argument to the peer's login shell as `<shell> -c <command>`.
POSIX snippets such as `if ...; then ...; fi` and `${SHELL:-/bin/sh}` are syntax
errors in fish, and fish also reads `\\\\` and `\\'` inside single quotes.
"""
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
HGS = Path(os.environ.get('HGS_TEST_BIN', ROOT / 'target/debug/hgs'))

FAKE_SSH = r'''#!/usr/bin/env python3
import json, os, sys
fixture = os.environ['REMOTE_FIXTURE']
with open(os.path.join(fixture, 'ssh-args.json'), 'w') as file:
    json.dump(sys.argv[1:], file)
shell = os.environ['REMOTE_LOGIN_SHELL']
env = dict(os.environ, HOME=os.path.join(fixture, 'peer'), SHELL=os.path.join(fixture, 'peer-shell'))
os.execve(shell, [shell, '-c', sys.argv[-1]], env)
'''

# The peer's hgs lists every capability and records its exact arguments and input.
PEER_HGS = r'''#!/bin/sh
case "$1" in
--help)
  printf '%s\n' 'hgs [@host] wait' '  session-notifications-v1' 'hooks-health-v1' \
    'hgs [@host] analytics --json' 'hgs [@host] guide' 'hgs [@host] status'
  exit 0;;
--version) echo 'hgs 9.9.9'; exit 0;;
esac
python3 -c 'import json, os, sys; json.dump(sys.argv[1:], open(os.path.expanduser("~/argv.json"), "w"))' "$@"
cat > "$HOME/stdin"
printf '{"ok":true,"services":{}}\n'
'''

# What `exec "${SHELL:-/bin/sh}" -l` starts on the peer.
PEER_SHELL = r'''#!/bin/sh
pwd > "$HOME/shell-pwd"
printf '%s\n' "$@" > "$HOME/shell-args"
'''

PEER_TOOL = r'''#!/bin/sh
if [ "$1" = -V ]; then echo 'tmux 3.5'; fi
'''

TRICKY = "it's a\\b \\' \\\\ $HOME \"q\" ; echo bad `x` \\"


def login_shells():
    shells = {'sh': '/bin/sh'}
    for name in ('bash', 'zsh'):
        if path := shutil.which(name):
            shells[name] = path
    if fish := os.environ.get('HGS_TEST_FISH') or shutil.which('fish'):
        shells['fish'] = fish
    return shells


@unittest.skipIf(os.name == 'nt', 'Peers are Linux and macOS computers')
class RemoteLoginShells(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='zerus-remote-shells-')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.peer = self.root / 'peer'
        for path, text in ((self.root / 'fake-ssh', FAKE_SSH), (self.peer / '.local/bin/hgs', PEER_HGS),
                           (self.root / 'peer-shell', PEER_SHELL), (self.peer / '.cargo/bin/tmux', PEER_TOOL)):
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(text)
            path.chmod(0o755)
        home = self.root / 'home'
        (home / '.config/hgs').mkdir(parents=True)
        (home / '.config/hgs/config').write_text('HGS_SELF="test-local"\nHGS_PEERS="peer"\n')
        self.env = {key: value for key, value in os.environ.items()
                    if not key.startswith('HGS_') and key not in ('TMUX', 'TMUX_PANE', 'SSH_AUTH_SOCK')}
        self.env.update(HOME=str(home), HGS_CONFIG_DIR=str(home / '.config/hgs'), HGS_STATE_DIR=str(self.root / 'state'),
                        HGS_SSH=str(self.root / 'fake-ssh'), HGS_TAB='0', REMOTE_FIXTURE=str(self.root))

    def hgs(self, shell, *args, body=''):
        return subprocess.run([str(HGS), *args], input=body, text=True, capture_output=True, timeout=30,
                              env=dict(self.env, REMOTE_LOGIN_SHELL=shell))

    def received(self):
        argv = json.loads((self.peer / 'argv.json').read_text())
        stdin = (self.peer / 'stdin').read_text()
        for name in ('argv.json', 'stdin'):
            (self.peer / name).unlink()
        return argv, stdin

    def test_fish_is_present_where_ci_requires_it(self):
        if os.environ.get('GITHUB_ACTIONS') == 'true' and sys.platform.startswith('linux'):
            self.assertIn('fish', login_shells())
        elif 'fish' not in login_shells():
            self.skipTest('fish is not installed; set HGS_TEST_FISH to run the fish cases')

    def test_capability_probes_run_under_sh_and_keep_json_input(self):
        body = json.dumps(dict(request_id='r-1', text=TRICKY))
        cases = [(('@peer', 'wait', 'ack', '--json'), ['wait', 'ack', '--json']),
                 (('@peer', 'notifications', '--json'), ['notifications', '--json']),
                 (('@peer', 'hooks', 'claude/example/task', '--json'), ['hooks', 'claude/example/task', '--json']),
                 (('@peer', 'analytics', '--json'), ['analytics', '--json', '--read-only']),
                 (('@peer', 'guide'), ['guide'])]
        for name, shell in login_shells().items():
            for args, expected in cases:
                with self.subTest(shell=name, command=args[1]):
                    result = self.hgs(shell, *args, body=body)
                    self.assertEqual(result.returncode, 0, result.stderr)
                    self.assertEqual(self.received(), (expected, body))

    def test_plain_commands_deliver_quotes_and_backslashes_exactly(self):
        for name, shell in login_shells().items():
            with self.subTest(shell=name):
                result = self.hgs(shell, '@peer', 'search', TRICKY)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(self.received()[0], ['search', TRICKY])
                version = self.hgs(shell, '@peer', 'version')
                self.assertEqual((version.returncode, version.stdout), (0, 'hgs 9.9.9\n'), version.stderr)

    def test_peer_status_reads_the_status_command(self):
        for name, shell in login_shells().items():
            with self.subTest(shell=name):
                result = self.hgs(shell, '@peer', 'status', '--json')
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(self.received()[0], ['status', '--local', '--json'])

    def test_machine_check_reads_the_setup_report(self):
        for name, shell in login_shells().items():
            with self.subTest(shell=name):
                result = self.hgs(shell, 'machine', 'check', 'peer')
                self.assertEqual(result.returncode, 0, result.stderr)
                report = json.loads(result.stdout)
                self.assertTrue(report['ok'], report)
                self.assertEqual((report['hgs'], report['tmux']), ('hgs 9.9.9', 'tmux 3.5'))

    def test_machine_shell_opens_the_folder_in_the_login_shell(self):
        folder = self.root / "a folder/it's \\ $(x)"
        folder.mkdir(parents=True)
        for name, shell in login_shells().items():
            with self.subTest(shell=name):
                result = self.hgs(shell, 'machine', 'ssh', 'peer', '--directory', str(folder))
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual((self.peer / 'shell-pwd').read_text(), f'{folder}\n')
                self.assertEqual((self.peer / 'shell-args').read_text(), '-l\n')
                (self.peer / 'shell-pwd').unlink()


if __name__ == '__main__':
    unittest.main()
