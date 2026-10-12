#!/usr/bin/env python3
"""hgs status and hgs version against isolated state, services and peers."""
import fcntl
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[1]
HGS = Path(os.environ.get("HGS_TEST_BIN", REPO / "target/debug/hgs")).resolve()

# Peers: "new" runs this hgs, "old" predates status, "down" is unreachable.
FAKE_SSH = r'''#!/usr/bin/env python3
import os, pathlib, subprocess, sys
root = pathlib.Path(os.environ['STATUS_FIXTURE'])
host, command = sys.argv[-2], sys.argv[-1]
if host == 'down':
    print('ssh: connect to host down port 22: Connection refused', file=sys.stderr)
    sys.exit(255)
home = root / ('remote-' + host)
env = dict(os.environ, HOME=str(home), HGS_SELF=host, HGS_PEERS='',
           HGS_CONFIG_DIR=str(home / 'config'), HGS_STATE_DIR=str(home / 'state'))
sys.exit(subprocess.run(['/bin/sh', '-c', command], env=env).returncode)
'''
OLD_HGS = r'''#!/usr/bin/env python3
import json, os, pathlib, sys
pathlib.Path(os.environ['STATUS_FIXTURE'], 'old-calls').open('a').write(' '.join(sys.argv[1:]) + '\n')
if sys.argv[1:] == ['--help']:
    print('usage: hgs [@host] <cmd> [project]\n       hgs [@host] ls                 sessions here and on peers')
elif sys.argv[1:] == ['--version']:
    print('hgs 1.0.0')
elif sys.argv[1:] == ['ls', '--local', '--json']:
    print(json.dumps({'host': 'old', 'ok': True, 'hgs_version': '1.0.0', 'sessions': [
        {'name': 'codex/app/fix', 'activity': 'busy', 'phase': 'tool'},
        {'name': 'claude/app/ask', 'activity': 'idle', 'phase': 'input'},
        {'name': 'claude/app/old', 'state': 'archived'}]}))
else:
    sys.exit(2)
'''


class Status(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='hgs-status-')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.bin = self.root / 'bin'
        self.bin.mkdir()
        self.home = self.root / 'home'
        self.state = self.root / 'state'
        self.config = self.root / 'config'
        for path in (self.home, self.state, self.config):
            path.mkdir()
        self.env = dict(os.environ, HOME=str(self.home), STATUS_FIXTURE=str(self.root),
                        HGS_CONFIG_DIR=str(self.config), HGS_STATE_DIR=str(self.state),
                        HGS_SELF='local', HGS_PEERS='new old down', HGS_TAB='0',
                        PATH=str(self.bin) + ':' + os.environ['PATH'])
        for variable in ('TMUX', 'TMUX_PANE', 'HGS_RUN_ID', 'HGS_SESSION', 'XDG_STATE_HOME'):
            self.env.pop(variable, None)
        # tmux has no server here; any other command would be an agent launch.
        self.script('tmux', '#!/bin/sh\necho "$*" >> "$STATUS_FIXTURE/tmux-calls"\nexit 1\n')
        self.script('pgrep', '#!/bin/sh\necho "$*" > "$STATUS_FIXTURE/pgrep-args"\n'
                             'exit "$(cat "$STATUS_FIXTURE/desktop" 2>/dev/null || echo 1)"\n')
        self.script('ssh', FAKE_SSH)
        new = self.root / 'remote-new/.local/bin'
        new.mkdir(parents=True)
        (new / 'hgs').symlink_to(HGS)
        old = self.root / 'remote-old/.local/bin'
        old.mkdir(parents=True)
        (old / 'hgs').write_text(OLD_HGS)
        (old / 'hgs').chmod(0o755)

    def script(self, name, text):
        path = self.bin / name
        path.write_text(text)
        path.chmod(0o755)

    def hgs(self, *args, code=0):
        result = subprocess.run([str(HGS), *args], env=self.env, capture_output=True, text=True,
                                stdin=subprocess.DEVNULL, timeout=30)
        self.assertEqual(result.returncode, code, result.stderr)
        return result.stdout if code == 0 else result.stdout + result.stderr

    def hold(self, path):
        path.parent.mkdir(parents=True, exist_ok=True)
        lock = path.open('a')
        fcntl.flock(lock, fcntl.LOCK_EX)
        self.addCleanup(lock.close)
        return lock

    def test_version_command_matches_the_flag_and_routes_to_peers(self):
        version = self.hgs('--version')
        self.assertRegex(version, r'^hgs \d+\.\d+\.\d+\n$')
        self.assertEqual(self.hgs('version'), version)
        self.assertEqual(self.hgs('@new', 'version'), version)
        self.assertEqual(self.hgs('@old', '--version'), 'hgs 1.0.0\n')
        self.assertIn('down unreachable', self.hgs('@down', 'version', code=3))

    def test_local_status_reports_sessions_and_services(self):
        (self.root / 'desktop').write_text('0')
        recovery = self.hold(self.state / 'recovery/worker.lock')
        connector_state = self.root / 'connector'
        (self.config / 'mobile').mkdir()
        (self.config / 'mobile/connector.json').write_text(json.dumps({'state_dir': str(connector_state)}))
        connector = self.hold(connector_state / 'connector.lock')
        status = json.loads(self.hgs('status', '--local', '--json'))
        self.assertEqual((status['host'], status['ok']), ('local', True))
        self.assertEqual(status['executable'], str(HGS))
        self.assertEqual(status['sessions']['live'], 0)
        self.assertEqual(status['services'], {'desktop': 'running', 'swarm_worker': 'stopped',
                                              'recovery_worker': 'running', 'mobile_connector': 'running'})
        self.assertEqual((self.root / 'pgrep-args').read_text().split(),
                         ['-x', '-u', str(os.getuid()), 'hgs-tray'])
        (self.root / 'desktop').write_text('1')
        recovery.close()
        connector.close()
        services = json.loads(self.hgs('status', '--local', '--json'))['services']
        self.assertEqual((services['desktop'], services['recovery_worker'], services['mobile_connector']),
                         ('stopped', 'stopped', 'stopped'))
        (self.config / 'mobile/connector.json').unlink()
        self.assertEqual(json.loads(self.hgs('status', '--local', '--json'))['services']['mobile_connector'],
                         'not_configured')

    def test_fleet_status_names_each_peer_and_never_launches_an_agent(self):
        text = self.hgs('status')
        self.assertIn('local (this computer): hgs ', text)
        self.assertIn('  Desktop app: not running\n', text)
        self.assertIn('\nnew: hgs ', text)
        self.assertIn('\nold: hgs 1.0.0\n  Sessions: 2 live (1 working, 1 need attention); 1 archived\n'
                      '  Services: not reported; update hgs on old to see them\n', text)
        self.assertIn('\ndown: offline: ssh: connect to host down port 22: Connection refused\n', text)
        fleet = json.loads(self.hgs('status', '--json'))
        self.assertEqual([m['host'] for m in fleet], ['local', 'new', 'old', 'down'])
        self.assertEqual(fleet[1]['services']['desktop'], 'stopped')
        self.assertIsNone(fleet[2]['services'])
        # The older peer only answered help and its listing.
        calls = (self.root / 'old-calls').read_text().splitlines()
        self.assertEqual(set(calls), {'--help', 'ls --local --json'})
        tmux = (self.root / 'tmux-calls').read_text()
        self.assertNotIn('new-session', tmux)

    def test_one_machine_and_invalid_requests(self):
        status = json.loads(self.hgs('@new', 'status', '--json'))
        self.assertEqual((status['host'], status['ok']), ('new', True))
        self.assertIn('services', status)
        self.assertIn('down: offline', self.hgs('@down', 'status', code=3))
        self.assertIn('usage: hgs [@host] status', self.hgs('status', '--all', code=1))
        self.assertIn('only reads', self.hgs('--dry-run', 'status', code=1))
        self.assertIn('hgs [@host] status [--json] [--local]', self.hgs('--help'))


if __name__ == '__main__':
    unittest.main()
