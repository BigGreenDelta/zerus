#!/usr/bin/env python3
"""A tmux server that hgs starts from a user unit outlives that unit.

tmux built with systemd support makes each pane `PartOf=` the unit that holds its
server, so stopping the unit that started the shared server ends every agent,
whatever its KillMode. Each test runs one command in a uniquely named transient
service (the launcher, standing in for the desktop GUI, the mobile connector or a
recovery worker) with a clean environment and a private tmux socket, then stops
only that unit. The suite needs a reachable user service manager and is skipped
where none is, as on macOS, in a container or in an SSH login without one. It
never addresses another tmux server or unit and never reloads the manager.
"""
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
import time
import unittest
import uuid

REPO = Path(__file__).resolve().parents[1]
HGS = Path(os.environ.get('HGS_TEST_BIN', REPO / 'target/debug/hgs')).resolve()
TMUX = shutil.which('tmux')
SYSTEMD_RUN = shutil.which('systemd-run')
SYSTEMCTL = shutil.which('systemctl')
RUNTIME = os.environ.get('XDG_RUNTIME_DIR') or f'/run/user/{os.getuid()}'
SERVER_SCOPE = re.compile(r'hgs-tmux-[0-9a-f-]{36}\.scope')


def unavailable():
    if sys.platform != 'linux':
        return 'transient user units are Linux only'
    if not (TMUX and SYSTEMD_RUN and SYSTEMCTL):
        return 'tmux, systemd-run and systemctl are required'
    if not Path(RUNTIME, 'systemd/private').exists():
        return 'no user service manager'
    try:
        state = subprocess.run([SYSTEMCTL, '--user', 'is-system-running'], capture_output=True, text=True,
                               timeout=15).stdout.strip()
    except (OSError, subprocess.TimeoutExpired):
        state = ''
    if state not in ('running', 'degraded'):
        return f'user service manager is {state or "unreachable"}'
    return None


def cgroup(pid):
    """The cgroup v2 path of `pid`, or of systemd's legacy hierarchy."""
    for line in Path(f'/proc/{pid}/cgroup').read_text().splitlines():
        hierarchy, controllers, path = line.split(':', 2)
        if (hierarchy == '0' and not controllers) or controllers == 'name=systemd':
            return path
    raise AssertionError(f'no systemd cgroup for {pid}')


def unit_of(pid):
    return cgroup(pid).rsplit('/', 1)[-1]


def alive(pid):
    try:
        return Path(f'/proc/{pid}/stat').read_text().rsplit(')', 1)[1].split()[0] not in ('Z', 'X')
    except (FileNotFoundError, ProcessLookupError):
        return False


SKIP = unavailable()


@unittest.skipIf(SKIP, SKIP or '')
class ServerScope(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory(prefix='hgs-tmux-scope-')
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name)
        for name in ('bin', 'tmux', 'cfg', 'state', 'home', 'proj'):
            (self.root / name).mkdir(mode=0o700)
        self.socket = self.root / 'tmux.sock'
        # The launcher's plain `tmux` reaches only this test's server.
        self.script('tmux', f'#!/bin/sh\nexec {shlex.quote(TMUX)} -S {shlex.quote(str(self.socket))} -f /dev/null "$@"\n')
        (self.root / 'cfg/config').write_text('HGS_SELF="testhost"\nHGS_PEERS=""\n')
        self.id = uuid.uuid4().hex[:12]
        self.units = []
        # Runs after the private server is gone, so no pane is left in a stopped unit.
        self.addCleanup(self.stop_units)
        self.addCleanup(self.stop_server)

    def script(self, name, text):
        path = self.root / 'bin' / name
        path.write_text(text)
        path.chmod(0o755)
        return path

    def tmux(self, *args):
        env = {key: value for key, value in os.environ.items() if not key.startswith(('TMUX', 'HGS_'))}
        return subprocess.run([TMUX, '-S', str(self.socket), '-f', os.devnull, *args], env=env,
                              capture_output=True, text=True, timeout=10)

    def stop_server(self):
        pid = self.tmux('display-message', '-p', '#{pid}').stdout.strip()
        self.tmux('kill-server')
        deadline = time.monotonic() + 10
        while pid and alive(pid) and time.monotonic() < deadline:
            time.sleep(.05)

    def stop_units(self):
        for unit in self.units:
            subprocess.run([SYSTEMCTL, '--user', 'stop', unit], capture_output=True, timeout=30)

    def launcher(self, *command, kill_mode='process'):
        """Run `command` to completion in a new transient service that stays active."""
        unit = f'hgs-test-launcher-{self.id}-{len(self.units)}.service'
        self.units.append(unit)
        env = dict(PATH=f'{self.root / "bin"}:/usr/bin:/bin', HOME=self.root / 'home', XDG_RUNTIME_DIR=RUNTIME,
                   HGS_CONFIG_DIR=self.root / 'cfg', HGS_STATE_DIR=self.root / 'state',
                   TMUX_TMPDIR=self.root / 'tmux', HGS_TRACKING='0', HGS_TAB='0', SHELL='/bin/sh', LANG='C.UTF-8')
        log = self.root / unit
        result = subprocess.run(
            [SYSTEMD_RUN, '--user', '--quiet', '--collect', '--unit', unit, '--property=Type=oneshot',
             '--property=RemainAfterExit=yes', f'--property=KillMode={kill_mode}',
             f'--property=StandardOutput=file:{log}.out', f'--property=StandardError=file:{log}.err',
             '--', '/usr/bin/env', '-i', *[f'{key}={value}' for key, value in env.items()], *command],
            capture_output=True, text=True, timeout=60)
        output = '\n'.join(path.read_text() for path in (Path(f'{log}.out'), Path(f'{log}.err')) if path.exists())
        self.assertEqual(result.returncode, 0, result.stderr + output)
        return unit, output

    def stop(self, unit):
        subprocess.run([SYSTEMCTL, '--user', 'stop', unit], check=True, capture_output=True, timeout=30)

    def pane(self, session):
        """The server and pane process of `session`."""
        fields = self.tmux('display-message', '-p', '-t', f'={session}:', '#{pid} #{pane_pid}')
        self.assertEqual(fields.returncode, 0, fields.stderr)
        return tuple(int(field) for field in fields.stdout.split())

    def part_of(self, pid):
        """The units the pane scope of `pid` is part of; None without a tmux pane scope."""
        scope = unit_of(pid)
        if not scope.startswith('tmux-spawn-'):
            return None
        return subprocess.run([SYSTEMCTL, '--user', 'show', '-p', 'PartOf', '--value', scope], check=True,
                              capture_output=True, text=True, timeout=15).stdout.split()

    def wait_ended(self, pid, timeout=10):
        deadline = time.monotonic() + timeout
        while alive(pid) and time.monotonic() < deadline:
            time.sleep(.05)
        return not alive(pid)

    def test_a_server_started_in_the_launcher_ends_with_it(self):
        # The fault itself: a server the launcher started holds every pane.
        unit, _ = self.launcher('tmux', 'new-session', '-d', '-s', 'plain', 'sleep 600')
        server, pane = self.pane('plain')
        self.assertEqual(unit_of(server), unit)
        if self.part_of(pane) != [unit]:
            self.skipTest('this tmux does not make panes part of the unit that holds its server')
        self.stop(unit)
        self.assertTrue(self.wait_ended(pane), 'the pane outlived the unit that holds its server')

    def test_a_server_hgs_starts_outlives_the_launcher(self):
        for kill_mode in ('process', 'control-group'):
            with self.subTest(kill_mode=kill_mode):
                # Each launch starts a new server.
                self.stop_server()
                name = f'sh/proj/{kill_mode}'
                unit, output = self.launcher(str(HGS), 'sh', str(self.root / 'proj'), '--new', '-d', '-n', kill_mode,
                                             kill_mode=kill_mode)
                self.assertIn(f'hgs: started {name}', output)
                server, pane = self.pane(name)
                scope, part_of = unit_of(server), self.part_of(pane)
                self.stop(unit)
                time.sleep(1.5)
                self.assertTrue(alive(pane), f'stopping the launcher ended the pane; its server ran in {scope}')
                self.assertTrue(alive(server), 'stopping the launcher ended the tmux server')
                self.assertEqual(self.tmux('has-session', '-t', f'={name}').returncode, 0)
                self.assertRegex(scope, SERVER_SCOPE)
                self.assertNotIn(unit, cgroup(server))
                if part_of is not None:
                    self.assertEqual(part_of, [scope])
                self.stop_server()
                # The empty scope is collected with its server.
                deadline = time.monotonic() + 10
                while subprocess.run([SYSTEMCTL, '--user', 'is-active', scope], capture_output=True, text=True,
                                     timeout=15).stdout.strip() == 'active' and time.monotonic() < deadline:
                    time.sleep(.1)
                self.assertNotEqual(subprocess.run([SYSTEMCTL, '--user', 'is-active', scope], capture_output=True,
                                                   text=True, timeout=15).stdout.strip(), 'active')

    def test_a_running_server_is_left_where_it_is(self):
        started = self.tmux('new-session', '-d', '-s', 'keeper', 'sleep 600')
        self.assertEqual(started.returncode, 0, started.stderr)
        server, keeper = self.pane('keeper')
        before = cgroup(server)
        unit, output = self.launcher(str(HGS), 'sh', str(self.root / 'proj'), '--new', '-d', '-n', 'joined')
        self.assertIn('hgs: started sh/proj/joined', output)
        joined_server, joined = self.pane('sh/proj/joined')
        self.assertEqual(joined_server, server)
        self.assertEqual(cgroup(server), before)
        self.stop(unit)
        time.sleep(1.5)
        self.assertTrue(alive(keeper) and alive(joined) and alive(server))

    def test_a_scope_systemd_refuses_starts_tmux_as_before(self):
        calls = self.root / 'systemd-run.calls'
        self.script('systemd-run', f'#!/bin/sh\necho "$*" >> {shlex.quote(str(calls))}\n'
                    'if [ "$1" = --version ]; then echo "systemd 262 (fixture)"; exit 0; fi\n'
                    'echo "Failed to start transient scope unit: fixture" >&2\nexit 1\n')
        unit, output = self.launcher(str(HGS), 'sh', str(self.root / 'proj'), '--new', '-d', '-n', 'fallback')
        self.assertIn('hgs: started sh/proj/fallback', output)
        server, _ = self.pane('sh/proj/fallback')
        # tmux started as it always has, in the unit that ran hgs.
        self.assertEqual(unit_of(server), unit)
        attempts = calls.read_text().splitlines()
        self.assertEqual(attempts[0], '--version')
        self.assertEqual(len(attempts), 2, attempts)
        self.assertRegex(attempts[1], r'^--user --scope --quiet --collect --expand-environment=no '
                                      r'--description=tmux server for hgs sessions --unit hgs-tmux-[0-9a-f-]{36} -- '
                                      r'/bin/sh -c .* tmux -u new-session -d ')


if __name__ == '__main__':
    unittest.main()
