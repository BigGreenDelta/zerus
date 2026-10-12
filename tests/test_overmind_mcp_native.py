#!/usr/bin/env python3
"""Keyless Codex contract for the Overmind MCP server that hgs attaches at launch.

hgs starts a scripted Codex in a private tmux; the real Codex then reads the same
overrides with an isolated CODEX_HOME and a localhost model provider."""
import http.server
import json
import os
from pathlib import Path
import subprocess
import threading
import unittest

import test_pause
from test_external_agents_native import Response

test_pause.HGS = Path(os.environ.get('HGS_TEST_BIN', Path(__file__).resolve().parents[1]/'target/debug/hgs')).resolve()

# Records how Codex started it, then answers just enough MCP for Codex to continue.
STUB = r'''#!/usr/bin/env python3
import json, os, sys
with open(%r, 'w') as out:
    json.dump({'argv': sys.argv[1:], 'env': dict(os.environ)}, out)
for line in sys.stdin:
    message = json.loads(line)
    if 'id' not in message:
        continue
    result = {}
    if message.get('method') == 'initialize':
        result = {'protocolVersion': message['params'].get('protocolVersion', '2025-06-18'),
                  'capabilities': {'tools': {}}, 'serverInfo': {'name': 'fixture', 'version': '1'}}
    elif message.get('method') == 'tools/list':
        result = {'tools': []}
    print(json.dumps({'jsonrpc': '2.0', 'id': message['id'], 'result': result}), flush=True)
'''

SESSION_VARIABLES = {'HGS_SESSION', 'HGS_RUN_ID', 'HGS_STATE_DIR', 'TMUX', 'HOME', 'PATH'}


@unittest.skipUnless(os.environ.get('HGS_CODEX_TEST_BIN'), 'Set HGS_CODEX_TEST_BIN for the isolated native Codex MCP test')
class NativeCodexMcp(test_pause.Harness):
    def attached(self):
        """A session hgs launched, and the Overmind overrides its Codex received."""
        name = self.start('codex', 'mcp')
        argv = json.loads((self.root / 'argv.jsonl').read_text().splitlines()[-1])['argv']
        overrides = []
        for at, word in enumerate(argv):
            if word == '--':
                break
            if word == '-c' and argv[at + 1].startswith('mcp_servers.overmind.'):
                overrides += ['-c', argv[at + 1]]
        self.assertEqual(len(overrides), 6, argv)
        return name, overrides

    def native(self, *args, **variables):
        home = self.root / 'native-codex'
        home.mkdir(exist_ok=True)
        env = {key: value for key, value in self.env.items() if not key.startswith(('HGS_', 'OPENAI_', 'CODEX_'))}
        env.update(HOME=str(self.root), CODEX_HOME=str(home), **variables)
        return subprocess.run([os.environ['HGS_CODEX_TEST_BIN'], *args], env=env,
                              capture_output=True, text=True, timeout=60)

    def test_codex_accepts_the_overrides_without_a_configuration_file(self):
        _, overrides = self.attached()
        shown = self.native(*overrides, 'mcp', 'get', 'overmind', '--json')
        self.assertEqual(shown.returncode, 0, shown.stdout + shown.stderr)
        server = json.loads(shown.stdout)
        self.assertTrue(server['enabled'], server)
        transport = server['transport']
        self.assertEqual((transport['type'], transport['command'], transport['args']),
                         ('stdio', str(test_pause.HGS), ['overmind', 'mcp', '--session']))
        self.assertLessEqual(SESSION_VARIABLES, set(transport['env_vars']))
        listed = self.native(*overrides, 'mcp', 'list')
        self.assertEqual(listed.returncode, 0, listed.stdout + listed.stderr)
        self.assertIn('overmind', listed.stdout)
        self.assertFalse((self.root / 'native-codex/config.toml').exists(), 'the agent configuration stays untouched')

    def test_codex_starts_the_server_with_the_session_environment(self):
        name, overrides = self.attached()
        record = self.binding(name)
        server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Response)
        self.addCleanup(server.server_close)
        self.addCleanup(server.shutdown)
        threading.Thread(target=server.serve_forever, daemon=True).start()
        (self.root / 'native-codex').mkdir()
        (self.root / 'native-codex/config.toml').write_text('''model="gpt-6.1-sol"
model_provider="fixture"
check_for_update_on_startup=false
[model_providers.fixture]
name="Fixture"
base_url="http://127.0.0.1:''' + str(server.server_port) + '''/v1"
wire_api="responses"
request_max_retries=0
stream_max_retries=0
''')
        started = self.root / 'mcp-server.json'
        stub = self.root / 'overmind-stub'
        stub.write_text(STUB % str(started))
        stub.chmod(0o755)
        # Only the command is replaced; the arguments and variables are the ones hgs attached.
        at = overrides.index(next(o for o in overrides if o.startswith('mcp_servers.overmind.command=')))
        overrides[at] = 'mcp_servers.overmind.command=' + json.dumps(str(stub))
        session = dict(HGS_SESSION=name, HGS_RUN_ID=record['run_id'], HGS_STATE_DIR=str(self.root / 'state'),
                       TMUX=self.socket + ',0,0')
        run = self.native(*overrides, 'exec', '--ephemeral', '--skip-git-repo-check', '-s', 'read-only',
                          '-C', str(self.root), '--color', 'never', 'Say done.', **session)
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
        self.assertTrue(started.exists(), 'Codex never started the MCP server\n' + run.stdout + run.stderr)
        seen = json.loads(started.read_text())
        self.assertEqual(seen['argv'], ['overmind', 'mcp', '--session'])
        self.assertEqual({key: seen['env'].get(key) for key in session}, session)


if __name__ == '__main__':
    unittest.main()
