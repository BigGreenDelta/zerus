#!/usr/bin/env python3
"""Desktop GUI live updates (tray/update-installed.py, tray/install.sh) with a fake GUI.

The fake GUI runs from the installed path inside an isolated home, so the
updater never matches the developer's own Zerus. systemctl, D-Bus tools and
cmake are fixtures; nothing reaches the real service manager or session bus.
"""
import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import tempfile
import time
import unittest

REPO = Path(__file__).resolve().parents[1]
UPDATER = REPO / 'tray/update-installed.py'

# A GUI build: answers --build-info, holds the single-instance socket, records
# activations, and (unless 'old') saves its queued draft when SIGTERM quits it.
FAKE_GUI = r'''#!{python}
import json, os, pathlib, signal, socket, sys
VARIANT = {variant!r}
root = pathlib.Path(os.environ['TRAY_FIXTURE'])
if sys.argv[1:] == ['--build-info']:
    print(json.dumps({{'version': '0.42.0', 'versionName': VARIANT, 'devBuildNumber': 0,
                      'sourceCommit': VARIANT.ljust(40, '0'), 'sourceDirty': False}}))
    sys.exit(0)
if VARIANT == 'broken':
    sys.exit(1)
drafts = pathlib.Path(os.environ['XDG_CONFIG_HOME']) / 'hgdev/hgs-tray.conf.drafts'
if VARIANT == 'eraser':
    for draft in drafts.rglob('draft.json'):
        draft.unlink()
def flush(*_):
    # The draft write that aboutToQuit flushes before the process ends.
    queued = drafts / 'queued'
    queued.mkdir(parents=True, exist_ok=True)
    (queued / 'draft.json').write_text(json.dumps({{'key': 'queued', 'text': 'saved on quit'}}))
    sys.exit(0)
if VARIANT != 'old':
    signal.signal(signal.SIGTERM, flush)
path = os.path.join(os.environ['TMPDIR'], 'hgs-tray')
server = socket.socket(socket.AF_UNIX)
try:
    os.unlink(path)
except FileNotFoundError:
    pass
server.bind(path)
server.listen()
with (root / 'started').open('a') as log:
    log.write(f'{{VARIANT}} {{os.getpid()}}\n')
while True:
    connection, _ = server.accept()
    with (root / 'activations').open('a') as log:
        log.write(connection.recv(64).decode())
    connection.close()
'''
SYSTEMCTL = r'''#!/bin/sh
echo "$*" >> "$TRAY_FIXTURE/systemctl"
case "$*" in
  "--user start hgs-tray.service") setsid "$HOME/.local/bin/hgs-tray" </dev/null >/dev/null 2>&1 & ;;
  # The installer test runs an enabled, active unit.
  "--user is-active hgs-tray.service") echo active ;;
  "--user is-enabled hgs-tray.service") echo enabled ;;
esac
exit 0
'''


@unittest.skipUnless(sys.platform.startswith('linux'), 'the fake GUI is identified through /proc')
class TrayUpdate(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='zerus-tray-update-')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.home = self.root / 'home'
        self.bin = self.root / 'bin'
        self.builds = self.root / 'builds'
        for path in (self.home / '.local/bin', self.bin, self.builds, self.root / 'tmp'):
            path.mkdir(parents=True)
        self.installed = self.home / '.local/bin/hgs-tray'
        self.drafts = self.home / '.config/hgdev/hgs-tray.conf.drafts'
        self.script(self.bin / 'systemctl', SYSTEMCTL)
        for tool in ('qdbus6', 'busctl'):
            self.script(self.bin / tool, '#!/bin/sh\nexit 1\n')
        self.env = dict(os.environ, HOME=str(self.home), TMPDIR=str(self.root / 'tmp'),
                        XDG_CONFIG_HOME=str(self.home / '.config'), TRAY_FIXTURE=str(self.root),
                        PATH=f'{self.bin}{os.pathsep}{os.environ["PATH"]}',
                        HGS_TRAY_QUIT_TIMEOUT='10', HGS_TRAY_START_TIMEOUT='4')
        self.addCleanup(self.stop_all)

    def script(self, path, text):
        path.write_text(text)
        path.chmod(0o755)

    def build(self, variant, path=None):
        path = path or self.builds / variant / 'hgs-tray'
        path.parent.mkdir(parents=True, exist_ok=True)
        self.script(path, FAKE_GUI.format(python=sys.executable, variant=variant))
        return path

    def started(self):
        log = self.root / 'started'
        return [line.split() for line in log.read_text().splitlines()] if log.exists() else []

    def running(self):
        """The variant and pid of the live fake GUI, if any."""
        alive = []
        for variant, pid in self.started():
            try:
                os.kill(int(pid), 0)
                cmdline = Path(f'/proc/{pid}/cmdline').read_bytes()
            except (ProcessLookupError, FileNotFoundError):
                continue
            if str(self.installed).encode() in cmdline:
                alive.append((variant, int(pid)))
        return alive

    def launch(self, variant):
        self.build(variant, self.installed)
        # Detached like a service: init, not this test, reaps it when it quits.
        subprocess.run(['sh', '-c', 'setsid "$0" </dev/null >/dev/null 2>&1 &', str(self.installed)],
                       env=self.env, check=True)
        self.wait(lambda: self.running() and (Path(self.env['TMPDIR']) / 'hgs-tray').exists())
        return self.running()[0][1]

    def wait(self, condition, seconds=10):
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            if condition():
                return
            time.sleep(0.1)
        self.fail('condition not reached')

    def stop_all(self):
        for _, pid in self.started():
            try:
                os.kill(int(pid), signal.SIGKILL)
            except ProcessLookupError:
                pass

    def draft(self, name, text):
        folder = self.drafts / name
        folder.mkdir(parents=True)
        (folder / 'draft.json').write_text(json.dumps({'key': name, 'text': text}))

    def update(self, built):
        return subprocess.run([sys.executable, str(UPDATER), str(built), str(self.installed)], env=self.env,
                              capture_output=True, text=True, timeout=60)

    def leftovers(self):
        return [p.name for p in self.installed.parent.iterdir() if p.name.startswith('.hgs-tray-update-')]

    def test_running_gui_quits_like_its_menu_and_comes_back(self):
        old = self.launch('first')
        self.draft('kept', 'unsent text')
        result = self.update(self.build('second'))
        self.assertEqual(result.returncode, 0, result.stderr)
        # SIGTERM reached the handler: the queued draft was written before replacement.
        self.assertEqual(json.loads((self.drafts / 'queued/draft.json').read_text())['text'], 'saved on quit')
        self.assertEqual(json.loads((self.drafts / 'kept/draft.json').read_text())['text'], 'unsent text')
        [(variant, pid)] = self.running()
        self.assertEqual(variant, 'second')
        self.assertNotEqual(pid, old)
        self.assertIn('--user start hgs-tray.service', (self.root / 'systemctl').read_text())
        self.assertEqual((self.root / 'activations').read_text(), 'sessions\n')
        self.assertIn("'second'", self.installed.read_text())
        previous = self.home / '.local/share/hgs/desktop-previous/hgs-tray'
        self.assertIn("'first'", previous.read_text())
        record = json.loads((self.home / '.local/share/hgs/build-provenance.json').read_text())
        self.assertEqual((record['guiVersionName'], record['preservedDrafts']), ('second', 2))
        self.assertEqual(self.leftovers(), [])
        self.assertEqual(list((self.home / '.local/state/hgs/desktop-update').iterdir()), [])
        self.assertIn('2 nonempty drafts kept', result.stdout)
        # The next update keeps exactly one rollback copy: the GUI it replaced.
        self.assertEqual(self.update(self.build('third')).returncode, 0)
        self.assertEqual([p.name for p in previous.parent.iterdir()], ['hgs-tray'])
        self.assertIn("'second'", previous.read_text())
        self.assertEqual([variant for variant, _ in self.running()], ['third'])

    def test_gui_without_a_signal_handler_is_never_killed(self):
        old = self.launch('old')
        result = self.update(self.build('second'))
        self.assertEqual(result.returncode, 1)
        self.assertIn('predates graceful service stops', result.stderr)
        self.assertEqual(self.running(), [('old', old)])
        self.assertIn("'old'", self.installed.read_text())
        self.assertFalse((self.root / 'systemctl').exists())
        self.assertEqual(self.leftovers(), [])

    def test_new_gui_that_does_not_start_is_rolled_back(self):
        self.launch('first')
        result = self.update(self.build('broken'))
        self.assertEqual(result.returncode, 1)
        self.assertIn('previous GUI was restored', result.stderr)
        self.assertIn("'first'", self.installed.read_text())
        self.wait(lambda: [variant for variant, _ in self.running()] == ['first'])
        self.assertEqual(self.leftovers(), [])

    def test_stopped_gui_is_replaced_without_starting_it(self):
        self.build('first', self.installed)
        result = self.update(self.build('second'))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("'second'", self.installed.read_text())
        self.assertIn("'first'", (self.home / '.local/share/hgs/desktop-previous/hgs-tray').read_text())
        self.assertFalse((self.root / 'systemctl').exists())
        self.assertEqual(self.running(), [])

    def test_update_removes_only_old_unused_leftover_folders(self):
        folder = self.installed.parent

        def leftover(name, files=(), age=7200):
            path = folder / name
            path.mkdir()
            for file in files:
                (path / file).parent.mkdir(parents=True, exist_ok=True)
                (path / file).write_text('copy')
            then = time.time() - age
            for item in [*path.rglob('*'), path]:
                os.utime(item, (then, then))
            return path

        leftover('.zerus-main-500e966')
        leftover('.zerus-previous-500e966', ['hgs-tray'])
        leftover('.zerus-0.41.Abm', ['previous/hgs-tray', 'previous/hgs_state.py'])
        leftover('.hgs-tray-update-interrupted', ['hgs-tray'])
        kept = [leftover('.zerus-notes', ['notes.txt']), leftover('.zerus-recent', age=0),
                leftover('.zerus-deep', ['a/b/hgs-tray']), leftover('.other-stage')]
        running = leftover('.zerus-running', ['hgs-tray'])
        self.script(running / 'hgs-tray', '#!/bin/sh\nsleep 60\n')
        os.utime(running, (time.time() - 7200,) * 2)
        process = subprocess.Popen([str(running / 'hgs-tray')], start_new_session=True)
        self.addCleanup(process.wait)
        self.addCleanup(process.kill)
        (folder / '.zerus-link').symlink_to(self.root)
        self.build('first', self.installed)
        result = self.update(self.build('second'))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(sorted(p.name for p in folder.iterdir() if p.name.startswith('.')),
                         sorted([p.name for p in kept] + ['.zerus-running', '.zerus-link']))
        self.assertIn('removed 4 leftover folders', result.stdout)
        self.assertIn('.zerus-notes: it holds files other than installer copies', result.stdout)
        self.assertIn('.zerus-running: a process still uses it', result.stdout)
        self.assertTrue((self.root / 'builds').exists())

    def test_lost_drafts_are_reported_with_their_snapshot(self):
        self.launch('first')
        self.draft('kept', 'unsent text')
        result = self.update(self.build('eraser'))
        self.assertEqual(result.returncode, 1)
        self.assertIn('kept: missing', result.stderr)
        [snapshot] = (self.home / '.local/state/hgs/desktop-update').iterdir()
        self.assertIn(str(snapshot), result.stdout)
        self.assertEqual(json.loads((snapshot / 'kept/draft.json').read_text())['text'], 'unsent text')
        self.assertEqual(snapshot.stat().st_mode & 0o777, 0o700)

    def test_installer_replaces_a_running_gui_through_the_updater(self):
        tray = self.root / 'tray'
        for name in ('install.sh', 'update-installed.py', 'packaging/hgs-tray.service', 'packaging/hgs-tray.desktop'):
            (tray / name).parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(REPO / 'tray' / name, tray / name)
        shutil.copytree(REPO / 'tray/resources/icons', tray / 'resources/icons')
        build = self.build('second')
        self.script(self.bin / 'cmake', f'#!/bin/sh\nif [ "$1" = --build ]; then mkdir -p "$2" && cp "{build}" "$2/hgs-tray"; fi\n')
        self.script(self.bin / 'uname', '#!/bin/sh\necho Linux\n')
        for tool in ('gtk-update-icon-cache', 'kbuildsycoca6'):
            self.script(self.bin / tool, '#!/bin/sh\nexit 0\n')
        self.launch('first')
        result = subprocess.run(['bash', str(tray / 'install.sh')], env=self.env, capture_output=True,
                                text=True, timeout=90)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('Installed hgs-tray.', result.stdout)
        self.assertEqual([variant for variant, _ in self.running()], ['second'])
        self.assertEqual(json.loads((self.drafts / 'queued/draft.json').read_text())['text'], 'saved on quit')
        calls = (self.root / 'systemctl').read_text()
        self.assertNotIn('restart', calls)
        self.assertIn('--user enable --now hgs-tray.service', calls)


if __name__ == '__main__':
    unittest.main()
