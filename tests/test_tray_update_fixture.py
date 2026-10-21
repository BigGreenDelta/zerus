"""Finite in-memory custody controls; no fake GUI, socket or child is started."""
import contextlib
from pathlib import Path
import signal
import subprocess
import unittest
from unittest import mock

import tray_update_fixture as fixture


class Process:
    def __init__(self, pid, events):
        self.pid, self.events, self.returncode = pid, events, None
        self.lock = None
        self.killed = False
        self.poll_error = self.kill_error = self.wait_error = None

    def event(self, name):
        if self.lock is None or not self.lock.locked():
            raise AssertionError('process operation is outside its ownership lock')
        self.events.append((self.pid, name))

    def poll(self):
        self.event('poll')
        if self.poll_error:
            raise self.poll_error
        return self.returncode

    def kill(self):
        self.event('kill')
        if self.kill_error:
            raise self.kill_error
        self.killed = True

    def wait(self, timeout):
        self.event('wait')
        if self.wait_error:
            raise self.wait_error
        if self.returncode is None:
            self.returncode = -9 if self.killed else 0
        return self.returncode


class Connection:
    def __init__(self, data):
        self.data, self.sent, self.timeouts = data, [], []

    def __enter__(self):
        return self

    def __exit__(self, *args):
        pass

    def settimeout(self, value):
        self.timeouts.append(value)

    def recv(self, count):
        part, self.data = self.data[:count], self.data[count:]
        return part

    def sendall(self, data):
        self.sent.append(data)


class FixtureCustodyControls(unittest.TestCase):
    def setUp(self):
        self.stack = contextlib.ExitStack()
        self.addCleanup(self.stack.close)
        self.events = []
        self.spawn = self.stack.enter_context(mock.patch.object(fixture.subprocess, 'Popen'))
        self.stack.enter_context(mock.patch.object(fixture.socket, 'socket', side_effect=AssertionError('real socket')))
        self.stack.enter_context(mock.patch.object(fixture.threading, 'Thread', side_effect=AssertionError('real thread')))
        self.stack.enter_context(mock.patch.object(fixture.signal, 'getsignal', return_value=signal.SIG_DFL))
        self.stack.enter_context(mock.patch.object(fixture.secrets, 'token_hex', side_effect=['launch-' + str(i) for i in range(32)]))
        self.clock = self.stack.enter_context(mock.patch.object(fixture.time, 'monotonic', return_value=100))
        self.write = self.stack.enter_context(mock.patch.object(Path, 'write_text'))
        self.output = self.stack.enter_context(mock.patch('builtins.print'))
        self.owner = fixture.OwnedChildren({'HOME': '/fixture/home'})

    def child(self, pid=4001, path='/fixture/hgs-tray'):
        process = Process(pid, self.events)
        self.spawn.return_value = process
        child = self.owner.start(Path(path))
        process.lock = child.lock
        return child

    def clean(self, control=None):
        remove = mock.Mock(side_effect=lambda root: self.events.append(('root', 'remove')))
        fixture.cleanup(Path('/fixture'), control, self.owner, remove)
        return remove

    def test_spawn_is_retained_as_one_direct_child_with_closed_stdio(self):
        child = self.child()
        self.assertIs(self.owner.children[0], child)
        args, options = self.spawn.call_args
        self.assertEqual(args, (['/fixture/hgs-tray'],))
        self.assertTrue(options['start_new_session'])
        self.assertTrue(options['close_fds'])
        self.assertEqual([options[n] for n in ('stdin', 'stdout', 'stderr')], [subprocess.DEVNULL] * 3)
        self.assertEqual(options['env'], {'HOME': '/fixture/home', 'TRAY_LAUNCH_TOKEN': child.token})
        self.assertEqual(self.owner.pending, 0)

    def test_pid_log_is_not_signal_or_liveness_authority(self):
        child = self.child()
        rows = [('foreign', '9001', child.token), ('stale', '4001', 'old-launch'), ('current', '4001', child.token)]
        self.assertEqual(self.owner.running('/fixture/hgs-tray', rows), [('current', 4001)])
        self.assertFalse(any(pid == 9001 for pid, _ in self.events))
        self.assertEqual(self.owner.stop_all(), [])
        self.assertEqual([pid for pid, event in self.events if event == 'kill'], [4001])

    def test_exited_child_is_reaped_without_a_signal(self):
        child = self.child()
        child.process.returncode = 0
        self.assertEqual(self.owner.running(child.path, [('done', '4001', child.token)]), [])
        self.assertEqual(self.owner.stop_all(), [])
        self.assertNotIn((4001, 'kill'), self.events)
        self.assertIn((4001, 'wait'), self.events)

    def test_each_process_operation_holds_the_same_per_child_lock(self):
        child = self.child()
        self.owner.running(child.path, [('current', '4001', child.token)])
        self.assertEqual(self.owner.stop_all(), [])
        self.assertEqual([name for _, name in self.events], ['poll', 'poll', 'kill', 'wait'])

    def test_all_children_are_signalled_before_any_wait(self):
        self.child(4001)
        self.child(4002)
        self.assertEqual(self.owner.stop_all(), [])
        self.assertLess(self.events.index((4002, 'kill')), self.events.index((4001, 'wait')))
        self.assertIn((4002, 'wait'), self.events)

    def test_signal_failure_keeps_root_and_still_attempts_other_children(self):
        a, b = self.child(4001), self.child(4002)
        a.process.kill_error = PermissionError('stop refused')
        a.process.wait_error = subprocess.TimeoutExpired('owned', 5)
        with self.assertRaisesRegex(AssertionError, 'stop owned child'):
            self.clean()
        self.assertTrue(b.process.killed)
        self.assertIn((4001, 'wait'), self.events)
        self.assertIn((4002, 'wait'), self.events)
        self.assertNotIn(('root', 'remove'), self.events)
        self.assertIn('stop refused', self.write.call_args.args[0])

    def test_wait_timeout_keeps_root_after_every_child_was_stopped(self):
        a, b = self.child(4001), self.child(4002)
        a.process.wait_error = subprocess.TimeoutExpired('owned', 5)
        with self.assertRaisesRegex(AssertionError, 'reap owned child'):
            self.clean()
        self.assertTrue(a.process.killed and b.process.killed)
        self.assertEqual(b.process.returncode, -9)
        self.assertNotIn(('root', 'remove'), self.events)

    def test_poll_failure_still_attempts_every_reap(self):
        a = self.child(4001)
        self.child(4002)
        a.process.poll_error = RuntimeError('unknown poll')
        self.assertTrue(self.owner.stop_all())
        self.assertIn((4001, 'wait'), self.events)
        self.assertIn((4002, 'kill'), self.events)
        self.assertIn((4002, 'wait'), self.events)

    def test_one_original_stop_deadline_is_shared_by_all_waits(self):
        self.child(4001)
        self.child(4002)
        self.clock.side_effect = [100, 104, 107]
        observed = []
        for child in self.owner.children:
            original = child.process.wait
            child.process.wait = lambda timeout, original=original: (observed.append(timeout), original(timeout))[1]
        self.assertEqual(self.owner.stop_all(seconds=5), [])
        self.assertEqual(observed, [1, 0])

    def test_clock_failure_still_attempts_all_owned_stops_and_zero_budget_waits(self):
        self.child(4001)
        self.child(4002)
        self.clock.side_effect = RuntimeError('clock unknown')
        with self.assertRaisesRegex(AssertionError, 'cleanup clock'):
            self.clean()
        self.assertEqual([pid for pid, event in self.events if event == 'kill'], [4001, 4002])
        self.assertEqual([pid for pid, event in self.events if event == 'wait'], [4001, 4002])
        self.assertNotIn(('root', 'remove'), self.events)

    def test_remove_requires_control_stop_then_confirmed_reaps(self):
        self.child()
        control = mock.Mock()
        control.stop.side_effect = lambda: (self.events.append(('control', 'stop')), [])[1]
        self.clean(control).assert_called_once_with(Path('/fixture'))
        self.assertLess(self.events.index(('control', 'stop')), self.events.index((4001, 'kill')))
        self.assertLess(self.events.index((4001, 'wait')), self.events.index(('root', 'remove')))
        self.write.assert_not_called()

    def test_control_failure_keeps_root_but_still_stops_children(self):
        self.child()
        control = mock.Mock()
        control.stop.side_effect = OSError('controller unknown')
        with self.assertRaisesRegex(AssertionError, 'controller unknown'):
            self.clean(control)
        self.assertIn((4001, 'wait'), self.events)
        self.assertNotIn(('root', 'remove'), self.events)

    def test_pending_launch_cannot_admit_removal_or_another_start(self):
        self.owner.pending = 1
        with self.assertRaisesRegex(AssertionError, 'launch is still pending'):
            self.clean()
        with self.assertRaisesRegex(RuntimeError, 'closing'):
            self.owner.start(Path('/fixture/hgs-tray'))
        self.spawn.assert_not_called()
        self.assertNotIn(('root', 'remove'), self.events)

    def test_spawn_failure_remains_recorded_and_existing_children_are_reaped(self):
        self.child()
        self.spawn.side_effect = OSError('spawn refused')
        with self.assertRaisesRegex(OSError, 'spawn refused'):
            self.owner.start(Path('/fixture/hgs-tray'))
        self.assertEqual(self.owner.pending, 0)
        with self.assertRaisesRegex(AssertionError, 'spawn refused'):
            self.clean()
        self.assertIn((4001, 'wait'), self.events)
        self.assertEqual(self.owner.snapshot()['children'][1]['pid'], None)

    def test_nondefault_reaper_is_refused_before_any_child(self):
        with mock.patch.object(fixture.signal, 'getsignal', return_value=signal.SIG_IGN):
            with self.assertRaisesRegex(RuntimeError, 'default child reaper'):
                fixture.OwnedChildren({})
        self.spawn.assert_not_called()

    def test_failed_root_removal_and_record_write_preserve_primary_diagnostics(self):
        self.child()
        self.write.side_effect = OSError('record unavailable')
        with self.assertRaisesRegex(AssertionError, 'remove: removal refused'):
            fixture.cleanup(Path('/fixture'), None, self.owner, mock.Mock(side_effect=OSError('removal refused')))
        self.assertIn('record unavailable', self.output.call_args.args[0])
        self.assertIn('removal refused', self.output.call_args.args[0])

    def test_control_close_and_live_thread_errors_keep_root(self):
        self.child()
        control = fixture.StartControl('/tmp/ztu-memory/start.sock', mock.Mock())
        control.socket = mock.Mock()
        control.socket.close.side_effect = OSError('close failed')
        control.thread = mock.Mock(ident=123)
        control.thread.is_alive.return_value = True
        with self.assertRaisesRegex(AssertionError, 'thread did not stop'):
            self.clean(control)
        control.thread.join.assert_called_once_with(timeout=4)
        self.assertIn((4001, 'wait'), self.events)
        self.assertNotIn(('root', 'remove'), self.events)

    def test_private_control_accepts_only_the_exact_request_and_orderly_eof(self):
        for wire, accepted in ((b'start\n', True), (b'stop!\n', False), (b'start', False), (b'start\nextra', False)):
            with self.subTest(wire=wire):
                start = mock.Mock()
                control = fixture.StartControl('/tmp/ztu-memory/start.sock', start)
                connection = Connection(wire)
                control.socket = mock.Mock()
                control.socket.accept.return_value = (connection, None)
                control.stopping = mock.Mock()
                control.stopping.is_set.side_effect = [False, True]
                control.serve()
                self.assertEqual(start.call_count, int(accepted))
                self.assertEqual(connection.sent, [b'ok\n'] if accepted else [])
                self.assertEqual(bool(control.problems), not accepted)

    def test_late_start_ack_is_unknown_even_after_owned_launch(self):
        control = fixture.StartControl('/tmp/ztu-memory/start.sock', lambda: setattr(self.clock, 'return_value', 104))
        connection = Connection(b'start\n')
        control.socket = mock.Mock()
        control.socket.accept.return_value = (connection, None)
        control.stopping = mock.Mock()
        control.stopping.is_set.side_effect = [False, True]
        control.serve()
        self.assertEqual(connection.sent, [])
        self.assertIn('fake service start expired', control.problems[0])

    def test_socket_length_refuses_before_allocation_on_both_posix_layouts(self):
        for name in ('/tmp/', '/private/tmp/'):
            with self.subTest(prefix=name):
                control = fixture.StartControl(name + 'ztu-a/start.sock', mock.Mock())
                self.assertIsNone(control.socket)
        with self.assertRaisesRegex(ValueError, 'POSIX bound'):
            fixture.StartControl('/private/tmp/' + 'x' * 104, mock.Mock())


if __name__ == '__main__':
    unittest.main()
