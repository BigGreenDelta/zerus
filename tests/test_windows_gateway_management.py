"""Portable manager capture controls; no connector/native/owner state is used."""
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
from types import SimpleNamespace
import unittest
from unittest import mock

import windows_gateway_management as capture


class Process:
    def __init__(self, waits=(), code=0):
        self.stdout, self.stderr = io.BytesIO(), io.BytesIO()
        self.returncode, self.waits, self.codes = None, [], list(waits)
        self.code, self.kills = code, 0
    def wait(self, timeout):
        self.waits.append(timeout)
        if self.codes:
            raise self.codes.pop(0)
        self.returncode = self.code
        return self.code
    def kill(self):
        self.kills += 1


class Clock:
    def __init__(self):
        self.value = 0
    def now(self):
        return self.value
    def sleep(self, seconds):
        self.value += seconds


class ManagementTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="gateway-capture-control-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)

    def test_exact_stdin_argv_environment_and_real_eof_are_required(self):
        process, observed = Process(), []
        data = {process.stdout: [b'{"running":true}', None], process.stderr: [None]}
        manager = capture.ManagementCapture(self.root, lambda pipe: data[pipe].pop(0))
        argv, env = ["synthetic-manager", "--manage", "start"], {"PRIVATE_FIXTURE": "yes"}
        payload = {"revision": "a" * 64, "path": "Δ folder"}
        def spawn(actual, **kwargs):
            observed.append((actual, kwargs["env"], kwargs["creationflags"], kwargs["stdin"].read()))
            self.assertEqual((kwargs["stdout"], kwargs["stderr"]), (subprocess.PIPE, subprocess.PIPE))
            return process
        with mock.patch.object(capture.subprocess, "Popen", side_effect=spawn):
            result = manager.run(argv, payload, env, 123)
        self.assertEqual(observed, [(argv, env, 123, json.dumps(payload).encode("ascii"))])
        self.assertEqual(json.loads(result.stdout), {"running": True})
        self.assertTrue(manager.pipes_ended)
        self.assertTrue(manager.may_remove_root)
        self.assertEqual((manager.folder / "stdout").read_bytes(), b'{"running":true}')
        self.assertEqual(process.kills, 0)

    def test_exited_manager_with_open_writer_still_times_out_without_signal(self):
        process, clock = Process(), Clock()
        manager = capture.ManagementCapture(self.root, lambda pipe: b"")
        with mock.patch.object(capture.subprocess, "Popen", return_value=process) as spawn, \
                mock.patch.object(capture.time, "monotonic", clock.now), \
                mock.patch.object(capture.time, "sleep", clock.sleep):
            with self.assertRaises(subprocess.TimeoutExpired) as caught:
                manager.run(["memory"], {}, {}, 0)
        self.assertEqual(caught.exception.timeout, 15)
        self.assertEqual(clock.value, 15)
        self.assertEqual(process.kills, 0)
        self.assertFalse(manager.pipes_ended)
        self.assertTrue(manager.exited)
        self.assertFalse(manager.may_remove_root)
        spawn.assert_called_once()

    def test_original_timeout_survives_bounded_direct_manager_reap(self):
        original = subprocess.TimeoutExpired(["memory"], 15)
        process = Process(waits=[original])
        manager = capture.ManagementCapture(self.root, lambda pipe: self.fail("no reads before exit"))
        with mock.patch.object(capture.subprocess, "Popen", return_value=process):
            with self.assertRaises(subprocess.TimeoutExpired) as caught:
                manager.run(["memory"], {}, {}, 0)
        self.assertIs(caught.exception, original)
        self.assertEqual(process.kills, 1)
        self.assertLessEqual(process.waits[0], 15)
        self.assertEqual(process.waits[1], 5)
        self.assertTrue(manager.exited)
        self.assertFalse(manager.may_remove_root)

    def test_signal_error_still_attempts_reap_and_preserves_original_timeout(self):
        original = subprocess.TimeoutExpired(["memory"], 15)
        process = Process(waits=[original, OSError("synthetic wait failure")])
        manager = capture.ManagementCapture(self.root)
        with mock.patch.object(capture.subprocess, "Popen", return_value=process), \
                mock.patch.object(process, "kill", side_effect=OSError("synthetic stop failure")):
            with self.assertRaises(subprocess.TimeoutExpired) as caught:
                manager.run(["memory"], {}, {}, 0)
        self.assertIs(caught.exception, original)
        self.assertEqual(len(process.waits), 2)
        self.assertFalse(manager.exited)
        self.assertFalse(manager.may_remove_root)
        self.assertEqual(len(original.__notes__), 2)

    def test_output_limit_refuses_without_waiting_for_eof(self):
        process = Process()
        manager = capture.ManagementCapture(self.root, lambda pipe: b"x" * 65537)
        with mock.patch.object(capture.subprocess, "Popen", return_value=process):
            with self.assertRaisesRegex(RuntimeError, "output exceeds"):
                manager.run(["memory"], {}, {}, 0)
        self.assertFalse(manager.pipes_ended)
        self.assertFalse(manager.may_remove_root)
        self.assertEqual(process.kills, 0)

    def test_setup_or_spawn_failure_is_not_a_success_or_retried(self):
        manager = capture.ManagementCapture(self.root)
        with mock.patch.object(capture.subprocess, "Popen", side_effect=OSError("synthetic spawn failure")) as spawn:
            with self.assertRaisesRegex(OSError, "synthetic spawn failure"):
                manager.run(["memory"], {}, {}, 0)
        spawn.assert_called_once()
        self.assertFalse(manager.may_remove_root)
        self.assertIsNone(manager.process)

    def test_nonzero_result_and_invalid_utf8_keep_failure_evidence(self):
        for code, body in [(1, b'{"ok":false}'), (0, b'\xff')]:
            process = Process(code=code)
            data = {process.stdout: [body, None], process.stderr: [None]}
            manager = capture.ManagementCapture(self.root, lambda pipe: data[pipe].pop(0))
            with mock.patch.object(capture.subprocess, "Popen", return_value=process):
                if code:
                    result = manager.run(["memory"], {}, {}, 0)
                    self.assertEqual(result.returncode, 1)
                else:
                    with self.assertRaises(UnicodeDecodeError):
                        manager.run(["memory"], {}, {}, 0)
            self.assertFalse(manager.may_remove_root)
            self.assertEqual((manager.folder / "stdout").read_bytes(), body)

    def test_peek_distinguishes_pending_bytes_broken_eof_and_read_error(self):
        pipe = mock.Mock()
        pipe.fileno.return_value = 42
        kernel = SimpleNamespace(PeekNamedPipe=mock.Mock())
        def pending(handle, destination, length, read, count, left):
            count._obj.value = 100000
            return True
        with mock.patch.dict(sys.modules, msvcrt=SimpleNamespace(get_osfhandle=lambda fd: 99)), \
                mock.patch.object(capture.ctypes, "WinDLL", return_value=kernel, create=True), \
                mock.patch.object(capture.ctypes, "get_last_error", return_value=109, create=True), \
                mock.patch.object(capture.os, "read", return_value=b"bytes") as read:
            kernel.PeekNamedPipe.side_effect = pending
            self.assertEqual(capture.available(pipe), b"bytes")
            read.assert_called_once_with(42, 65537)
            read.reset_mock()
            kernel.PeekNamedPipe.side_effect = None
            kernel.PeekNamedPipe.return_value = True
            self.assertEqual(capture.available(pipe), b"")
            read.assert_not_called()
            kernel.PeekNamedPipe.return_value = False
            self.assertIsNone(capture.available(pipe))
            with mock.patch.object(capture.ctypes, "get_last_error", return_value=5):
                with self.assertRaises(OSError):
                    capture.available(pipe)

    @unittest.skipIf(sys.platform == "win32", "POSIX nonblocking adapter only; native Windows uses PeekNamedPipe")
    def test_owned_live_writer_cannot_extend_exited_manager_deadline(self):
        # The writer is deliberately this test's direct child, so cleanup never
        # adopts a descendant's numeric PID. It models another holder of the
        # original manager pipes; actual Windows descendant coverage is native.
        out_read, out_write = os.pipe()
        err_read, err_write = os.pipe()
        writers = [os.fdopen(out_write, "wb"), os.fdopen(err_write, "wb")]
        reads = [os.fdopen(out_read, "rb", buffering=0), os.fdopen(err_read, "rb", buffering=0)]
        keeper = subprocess.Popen([sys.executable, "-I", "-B", "-c", "import sys;sys.stdin.buffer.read()"],
                                  stdin=subprocess.PIPE, stdout=writers[0], stderr=writers[1])
        started = []
        def close_keeper():
            keeper.stdin.close()
            try:
                try:
                    keeper.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    keeper.kill()  # Exact direct Popen handle owned by this test.
                    keeper.wait(timeout=5)
                    raise
            finally:
                for process in started:
                    if process.poll() is None:
                        process.kill()
                        process.wait(timeout=5)
                for item in writers + reads:
                    item.close()
        self.addCleanup(close_keeper)
        real_spawn = subprocess.Popen
        def spawn(argv, **kwargs):
            kwargs.update(stdout=writers[0], stderr=writers[1])
            process = real_spawn(argv, **kwargs)
            process.stdout, process.stderr = reads
            started.append(process)
            for item in writers:
                item.close()
            return process
        def nonblocking(pipe):
            os.set_blocking(pipe.fileno(), False)
            try:
                data = os.read(pipe.fileno(), 65537)
                return data or None
            except BlockingIOError:
                return b""
        manager = capture.ManagementCapture(self.root, nonblocking)
        with mock.patch.object(capture.subprocess, "Popen", side_effect=spawn), mock.patch.object(capture, "SECONDS", 0.2):
            begun = time.monotonic()
            with self.assertRaises(subprocess.TimeoutExpired):
                manager.run([sys.executable, "-I", "-B", "-c", "pass"], {}, dict(os.environ), 0)
            self.assertLess(time.monotonic() - begun, 2)
        self.assertEqual(len(started), 1)
        self.assertEqual(started[0].returncode, 0)
        self.assertIsNone(keeper.poll(), "capture must not signal the other output holder")
        self.assertFalse(manager.may_remove_root)

    def test_late_wait_cannot_read_or_accept_eof(self):
        process, clock = Process(), Clock()
        original_wait = process.wait
        def late_wait(timeout):
            code = original_wait(timeout)
            clock.value = 16
            return code
        read = mock.Mock(return_value=None)
        manager = capture.ManagementCapture(self.root, read)
        with mock.patch.object(capture.subprocess, "Popen", return_value=process), \
                mock.patch.object(process, "wait", side_effect=late_wait), \
                mock.patch.object(capture.time, "monotonic", clock.now):
            with self.assertRaises(subprocess.TimeoutExpired) as caught:
                manager.run(["memory"], {}, {}, 0)
        self.assertEqual(caught.exception.timeout, 15)
        read.assert_not_called()
        self.assertEqual(process.kills, 0)
        self.assertTrue(manager.exited)
        self.assertFalse(manager.pipes_ended)
        self.assertFalse(manager.may_remove_root)

    def test_last_eof_after_deadline_refuses_and_retains_collected_bytes(self):
        process, clock, calls = Process(), Clock(), []
        body = b'{"running":true}'
        def read(pipe):
            calls.append(pipe)
            if len(calls) == 1:
                return body
            if len(calls) == 3:
                clock.value = 16  # The final otherwise-valid EOF arrived late.
            return None
        manager = capture.ManagementCapture(self.root, read)
        with mock.patch.object(capture.subprocess, "Popen", return_value=process), \
                mock.patch.object(capture.time, "monotonic", clock.now), \
                mock.patch.object(capture.time, "sleep", clock.sleep):
            with self.assertRaises(subprocess.TimeoutExpired) as caught:
                manager.run(["memory"], {}, {}, 0)
        self.assertEqual(caught.exception.timeout, 15)
        self.assertEqual(calls, [process.stdout, process.stderr, process.stdout])
        self.assertEqual((manager.folder / "stdout").read_bytes(), body)
        self.assertEqual(process.kills, 0)
        self.assertTrue(manager.exited)
        self.assertFalse(manager.pipes_ended)
        self.assertFalse(manager.may_remove_root)

    def test_retained_output_prefix_is_capped_and_truncation_is_explicit(self):
        process = Process()
        values = {process.stdout: [b"a" * 65530, None], process.stderr: [b"b" * 20, None]}
        manager = capture.ManagementCapture(self.root, lambda pipe: values[pipe].pop(0))
        with mock.patch.object(capture.subprocess, "Popen", return_value=process):
            manager.run(["memory"], {}, {}, 0)
        self.assertEqual((manager.folder / "stdout").read_bytes(), b"a" * 65530)
        self.assertEqual((manager.folder / "stderr").read_bytes(), b"b" * 20)
        self.assertTrue(manager.pipes_ended)
        self.assertTrue(manager.may_remove_root)
        # Overflow in one stream must be covered separately from their combined size.
        process = Process()
        values = {process.stdout: [b"a" * 65530, b"b" * 20], process.stderr: [None]}
        manager = capture.ManagementCapture(self.root, lambda pipe: values[pipe].pop(0))
        with mock.patch.object(capture.subprocess, "Popen", return_value=process):
            with self.assertRaisesRegex(RuntimeError, "retained prefix truncated"):
                manager.run(["memory"], {}, {}, 0)
        self.assertEqual((manager.folder / "stdout").read_bytes(), b"a" * 65530 + b"b" * 6)
        self.assertEqual(json.loads((manager.folder / "truncated.json").read_bytes()),
            {"stream": "stdout", "retained_bytes": 65536, "observed_at_least": 65550, "truncated": True})
        self.assertFalse(manager.may_remove_root)


    def test_both_pipe_close_errors_are_retained_without_publishing_success(self):
        calls = []
        errors = {name: OSError("synthetic " + name + " close") for name in ("stdout", "stderr")}
        class Pipe(io.BytesIO):
            def __init__(self, name):
                super().__init__()
                self.name = name
            def close(self):
                if self.closed:
                    return
                calls.append(self.name)
                super().close()
                raise errors[self.name]
        process = Process()
        process.stdout, process.stderr = Pipe("stdout"), Pipe("stderr")
        manager = capture.ManagementCapture(self.root, lambda pipe: None)
        with mock.patch.object(capture.subprocess, "Popen", return_value=process):
            with self.assertRaises(OSError) as caught:
                manager.run(["memory"], {}, {}, 0)
        self.assertIs(caught.exception, errors["stdout"])
        self.assertEqual(calls, ["stdout", "stderr"])
        self.assertTrue(any("manager stderr close" in note for note in caught.exception.__notes__))
        self.assertTrue(manager.failed)
        self.assertFalse(manager.pipes_ended)
        self.assertFalse(manager.may_remove_root)
        self.assertEqual(process.kills, 0)

    def test_each_private_file_close_error_keeps_failure_and_closes_later_files(self):
        for target in ("stdin.json", "stdout", "stderr", "truncated.json"):
            with self.subTest(target=target):
                calls, files = [], {}
                failure = OSError("synthetic private close")
                class File(io.BytesIO):
                    def __init__(self, name):
                        super().__init__()
                        self.name = name
                    def close(self):
                        if self.closed:
                            return
                        calls.append(self.name)
                        self.retained = self.getvalue()
                        super().close()
                        if self.name == target:
                            raise failure
                def own_file(path):
                    stream = File(path.name)
                    files[path.name] = stream
                    return stream
                process = Process()
                reader = (lambda pipe: b"x" * 65537) if target == "truncated.json" else (lambda pipe: None)
                manager = capture.ManagementCapture(self.root, reader)
                with mock.patch.object(capture.subprocess, "Popen", return_value=process), \
                        mock.patch.object(capture, "exclusive", side_effect=own_file):
                    if target == "truncated.json":
                        with self.assertRaisesRegex(RuntimeError, "retained prefix truncated") as caught:
                            manager.run(["memory"], {}, {}, 0)
                        self.assertEqual(len(files["stdout"].retained), 65536)
                    else:
                        with self.assertRaises(OSError) as caught:
                            manager.run(["memory"], {}, {}, 0)
                        self.assertIs(caught.exception, failure)
                expected = (["truncated.json"] if target == "truncated.json" else []) + ["stderr", "stdout", "stdin.json"]
                self.assertEqual(calls, expected)
                self.assertTrue(all(stream.closed for stream in files.values()))
                self.assertTrue(process.stdout.closed and process.stderr.closed)
                self.assertTrue(any("private " + target + " close" in note for note in caught.exception.__notes__))
                self.assertTrue(manager.failed)
                self.assertFalse(manager.pipes_ended)
                self.assertFalse(manager.may_remove_root)

    def test_original_timeout_survives_pipe_and_private_file_close_errors(self):
        calls = []
        original = subprocess.TimeoutExpired(["memory"], 15)
        class Closing(io.BytesIO):
            def __init__(self, name):
                super().__init__()
                self.name = name
            def close(self):
                if self.closed:
                    return
                calls.append(self.name)
                super().close()
                if self.name in ("pipe-stdout", "file-stderr"):
                    raise OSError("synthetic close failure")
        process = Process(waits=[original])
        process.stdout, process.stderr = Closing("pipe-stdout"), Closing("pipe-stderr")
        manager = capture.ManagementCapture(self.root)
        with mock.patch.object(capture.subprocess, "Popen", return_value=process), \
                mock.patch.object(capture, "exclusive", side_effect=lambda path: Closing("file-" + path.name)):
            with self.assertRaises(subprocess.TimeoutExpired) as caught:
                manager.run(["memory"], {}, {}, 0)
        self.assertIs(caught.exception, original)
        self.assertEqual(calls, ["pipe-stdout", "pipe-stderr", "file-stderr", "file-stdout", "file-stdin.json"])
        self.assertEqual(process.kills, 1)
        self.assertEqual(process.waits[1], 5)
        self.assertTrue(any("manager stdout close" in note for note in original.__notes__))
        self.assertTrue(any("private stderr close" in note for note in original.__notes__))
        self.assertTrue(manager.exited)
        self.assertTrue(manager.failed)
        self.assertFalse(manager.may_remove_root)


if __name__ == "__main__":
    unittest.main()
