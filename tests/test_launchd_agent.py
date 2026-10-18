from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
import launchd_agent


RUNNING = '''gui/501/com.example.worker = {
\tactive count = 1
\tpath = /Users/example/Library/LaunchAgents/com.example.worker.plist
\tstate = running
\tprogram = /Users/example/.local/bin/hgs
\truns = 3
\tpid = 4242
\tlast exit code = 78: EX_CONFIG
\tjob state = running
\tproperties = keepalive | runatload | inferred program
}
'''
# An agent whose Login Items launch constraint names an earlier ad-hoc build,
# as launchctl printed it on macOS 27 (paths and counts replaced).
STALLED = '''gui/501/com.example.worker = {
\tactive count = 0
\tpath = /Users/example/Library/LaunchAgents/com.example.worker.plist
\ttype = LaunchAgent
\tstate = spawn scheduled
\tprogram = /Users/example/.local/bin/hgs
\tminimum runtime = 10
\truns = 50726
\tlast exit code = 78: EX_CONFIG
\tresource coalition = {
\t\ttype = resource
\t\tstate = active
\t}
\tspawn type = daemon (3)
\tjob state = spawn failed
\tproperties = keepalive | runatload | inferred program | needs LWCR update | managed LWCR | has LWCR
}
'''


class FakeLaunchd:
    """launchctl as observed on macOS: bootout returns while a worker is still exiting."""

    def __init__(self, loaded=True, removal_polls=0, bootstrap_error='', printed=RUNNING):
        self.loaded, self.removal_polls, self.bootstrap_error = loaded, removal_polls, bootstrap_error
        self.printed = printed
        self.removing = False
        self.calls = []

    def __call__(self, *arguments):
        self.calls.append(arguments[0])
        command = arguments[0]
        code, error = 0, ''
        if command == 'print':
            if self.removing:
                if self.removal_polls:
                    self.removal_polls -= 1
                else:
                    self.removing = self.loaded = False
            code = 0 if self.loaded else 113
            if self.loaded:
                return subprocess.CompletedProcess(['launchctl', *arguments], code, self.printed, '')
        elif command == 'bootout':
            self.removing = True
            code = 36
        elif command == 'bootstrap':
            if self.loaded:
                code, error = 5, 'Bootstrap failed: 5: Input/output error'
            elif self.bootstrap_error:
                code, error = 125, self.bootstrap_error
            else:
                self.loaded = True
        elif command == 'kickstart':
            code = 0 if self.loaded and not self.removing else 113
        return subprocess.CompletedProcess(['launchctl', *arguments], code, '', error)


class LaunchdAgent(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.plist = Path(self.directory.name) / 'com.example.worker.plist'
        self.now = 0.0

    def tearDown(self):
        self.directory.cleanup()

    def install(self, launchd, content=b'new', restart=True):
        def sleep(seconds):
            self.now += seconds
        return launchd_agent.install('com.example.worker', self.plist, content, restart,
                                     run=launchd, clock=lambda: self.now, sleep=sleep)

    def test_changed_definition_waits_for_removal_instead_of_restarting_a_leaving_worker(self):
        self.plist.write_bytes(b'old')
        launchd = FakeLaunchd(removal_polls=3)
        self.assertEqual(self.install(launchd), ('loaded', ''))
        self.assertNotIn('kickstart', launchd.calls)
        self.assertEqual(launchd.calls[-1], 'bootstrap')
        self.assertTrue(launchd.loaded)
        self.assertEqual(self.plist.read_bytes(), b'new')

    def test_unchanged_definition_restarts_only_when_asked(self):
        self.plist.write_bytes(b'new')
        launchd = FakeLaunchd()
        self.assertEqual(self.install(launchd, restart=False), ('unchanged', ''))
        self.assertEqual(launchd.calls, ['print'])
        # A restart loads the agent again, so that launchd reads its definition
        # and launch constraint anew; a kickstart reuses the loaded ones.
        self.assertEqual(self.install(launchd), ('restarted', ''))
        self.assertEqual(launchd.calls[1:3], ['print', 'bootout'])
        self.assertEqual(launchd.calls[-1], 'bootstrap')
        self.assertNotIn('kickstart', launchd.calls)
        self.assertTrue(launchd.loaded)

    def test_agent_launchd_cannot_start_is_loaded_again_without_a_restart(self):
        self.plist.write_bytes(b'new')
        launchd = FakeLaunchd(printed=STALLED)
        self.assertTrue(launchd_agent.stalled(STALLED))
        self.assertEqual(self.install(launchd, restart=False), ('restarted', ''))
        self.assertEqual(launchd.calls[:2], ['print', 'bootout'])
        self.assertEqual(launchd.calls[-1], 'bootstrap')
        self.assertNotIn('kickstart', launchd.calls)
        self.assertEqual(self.plist.read_bytes(), b'new')

    def test_running_agent_is_not_loaded_again_for_an_earlier_failure(self):
        self.plist.write_bytes(b'new')
        self.assertFalse(launchd_agent.stalled(RUNNING))
        self.assertFalse(launchd_agent.stalled(STALLED.replace('\tjob state = spawn failed', '\tjob state = exited')
                                               .replace('78: EX_CONFIG', '0')))
        launchd = FakeLaunchd(printed=RUNNING)
        self.assertEqual(self.install(launchd, restart=False), ('unchanged', ''))
        self.assertEqual(launchd.calls, ['print'])

    def test_stalled_agent_that_cannot_be_loaded_now_is_left_for_next_login(self):
        self.plist.write_bytes(b'new')
        launchd = FakeLaunchd(printed=STALLED, bootstrap_error='Domain does not support specified action')
        self.assertEqual(self.install(launchd, restart=False),
                         ('deferred', 'Domain does not support specified action'))

    def test_unloaded_agent_is_bootstrapped_or_left_for_next_login(self):
        launchd = FakeLaunchd(loaded=False)
        self.assertEqual(self.install(launchd), ('loaded', ''))
        self.assertNotIn('bootout', launchd.calls)
        launchd = FakeLaunchd(loaded=False, bootstrap_error='Domain does not support specified action')
        self.assertEqual(self.install(launchd, content=b'other'),
                         ('deferred', 'Domain does not support specified action'))

    def test_removal_that_never_finishes_is_reported(self):
        self.plist.write_bytes(b'old')
        launchd = FakeLaunchd(removal_polls=10_000)
        with self.assertRaisesRegex(SystemExit, 'still removing'):
            self.install(launchd)
        self.assertNotIn('kickstart', launchd.calls)
        self.assertNotIn('bootstrap', launchd.calls)


if __name__ == '__main__':
    unittest.main()
