#!/usr/bin/env python3
"""Managed profile launches share the user's MCP servers. Private tmux socket and fake agents only."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import time
import tomllib
import unittest

HGS = str(Path(os.environ.get('HGS_TEST_BIN', 'target/debug/hgs')).resolve())
TMUX = shutil.which('tmux')
SERVER = {'url': 'https://memory.example.com/mcp', 'headers': {'Authorization': 'Bearer synthetic-token'}}


@unittest.skipUnless(TMUX, 'tmux is required for launch checks')
class ManagedProfileMcp(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='zerus-mcp-')
        self.root = Path(self.temp.name)
        self.home = self.root / 'home'
        self.config = self.home / '.config/hgs'
        self.config.mkdir(parents=True)
        self.bin = self.home / '.local/bin'
        self.bin.mkdir(parents=True)
        self.project = self.root / 'project'
        self.project.mkdir()
        self.socket = self.root / 'tmux.sock'
        self.env = {key: value for key, value in os.environ.items()
                    if not key.startswith('HGS_') and key not in ('CODEX_HOME', 'CLAUDE_CONFIG_DIR', 'KIMI_CODE_HOME', 'TMUX', 'TMUX_PANE')}
        self.env.update(HOME=str(self.home), HGS_CONFIG_DIR=str(self.config), HGS_STATE_DIR=str(self.root / 'state'),
                        HGS_SELF='test', HGS_PEERS='', HGS_TAB='0', SHELL='/bin/sh', PATH=str(self.bin) + os.pathsep + self.env['PATH'])
        self.script('tmux', f'#!/bin/sh\nexec {TMUX} -S "{self.socket}" -f /dev/null "$@"\n')
        self.addCleanup(self.stop)
        # Each fake agent records the configuration it finds at startup, then exits.
        # A complete copy is renamed into place, so the observed file is never read half written.
        for agent, variable, name in [('codex', 'CODEX_HOME', 'config.toml'), ('claude', 'CLAUDE_CONFIG_DIR', '.claude.json'),
                                      ('kimi', 'KIMI_CODE_HOME', 'mcp.json')]:
            observed = f'{self.root}/{agent}-observed'
            self.script(agent, '#!/bin/sh\ncase "$1" in --help|-h|--version) echo "' + agent + ' 0.0.0"; exit 0;; esac\n'
                        f'cp "${variable}/{name}" "{observed}.tmp" 2>/dev/null && mv "{observed}.tmp" "{observed}" '
                        f'|| : > "{self.root}/{agent}-missing"\nsleep 1\n')

    def stop(self):
        # A detached session outlives its launch command: its tracked supervisor records the
        # agent's exit in the state folder, also after kill-server returns. Remove the fixture
        # only after this private server's pane processes, with the same start identity, ended.
        def alive(pids):
            if not pids:
                return {}
            # One keyword per -o: POSIX ps (macOS) takes the rest of an argument after '=' as its header.
            status = subprocess.run(['ps', '-p', ','.join(pids), '-o', 'pid=', '-o', 'stat=', '-o', 'lstart='],
                                    capture_output=True, text=True, timeout=5, env=dict(self.env, LC_ALL='C', TZ='UTC'))
            rows = [line.split() for line in status.stdout.splitlines()]
            # An unreadable listing must fail here rather than skip the wait.
            self.assertTrue(all(len(row) > 2 and row[0] in pids for row in rows), status.stdout)
            return {row[0]: ' '.join(row[2:]) for row in rows if not row[1].startswith(('Z', 'X'))}
        panes = subprocess.run([TMUX, '-S', str(self.socket), 'list-panes', '-a', '-F', '#{pane_pid}'],
                               capture_output=True, text=True, timeout=5)
        try:
            owned = alive([pid for pid in panes.stdout.split() if pid.isdigit()])
        finally:
            subprocess.run([TMUX, '-S', str(self.socket), 'kill-server'], capture_output=True, timeout=5)
        deadline = time.monotonic() + 10
        while owned and time.monotonic() < deadline:
            time.sleep(0.02)
            owned = {pid: start for pid, start in alive(list(owned)).items() if owned[pid] == start}
        self.assertFalse(owned, 'private tmux panes survived kill-server')
        self.temp.cleanup()

    def script(self, name, text):
        path = self.bin / name
        path.write_text(text)
        path.chmod(0o755)

    def hgs(self, *args):
        result = subprocess.run([HGS, *args], env=self.env, cwd=self.root, capture_output=True, text=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stderr)
        return result.stdout

    def launch(self, agent):
        self.hgs('account', 'add', 'work-' + agent, '--provider', agent, '--label', 'Work')
        self.hgs(agent, str(self.project), '--new', '-n', 'mcp', '-d', '--account', 'work-' + agent)
        observed, missing = self.root / (agent + '-observed'), self.root / (agent + '-missing')
        deadline = time.monotonic() + 15
        while time.monotonic() < deadline and not observed.exists() and not missing.exists():
            time.sleep(0.05)
        self.assertTrue(observed.exists(), agent + ' started without a profile MCP configuration')
        return observed.read_text()

    def test_codex_profile_receives_native_servers_but_not_native_settings(self):
        (self.home / '.codex').mkdir()
        (self.home / '.codex/config.toml').write_text('model = "native-only"\n# BEGIN MANAGED MEMORY\n[mcp_servers.memory]\n'
            'url = "https://memory.example.com/mcp"\n[mcp_servers.memory.http_headers]\nAuthorization = "Bearer synthetic-token"\n')
        config = tomllib.loads(self.launch('codex'))
        self.assertEqual(config['mcp_servers'], {'memory': {'url': SERVER['url'], 'http_headers': SERVER['headers']}})
        self.assertEqual(config['cli_auth_credentials_store'], 'file')
        self.assertNotIn('model', config)

    def test_claude_and_kimi_profiles_receive_native_servers(self):
        (self.home / '.claude.json').write_text(json.dumps({'mcpServers': {'memory': SERVER}, 'oauthAccount': {'email': 'native'}}))
        (self.home / '.kimi-code').mkdir()
        (self.home / '.kimi-code/mcp.json').write_text(json.dumps({'mcpServers': {'memory': SERVER}}))
        for agent in ('claude', 'kimi'):
            with self.subTest(agent=agent):
                observed = json.loads(self.launch(agent))
                self.assertEqual(observed['mcpServers'], {'memory': SERVER})
                self.assertNotIn('oauthAccount', observed)
                baseline = (self.config / 'accounts' / ('work-' + agent) / '.hgs-inherited-mcp.json').read_text()
                self.assertNotIn('synthetic-token', baseline)


if __name__ == '__main__':
    unittest.main()
