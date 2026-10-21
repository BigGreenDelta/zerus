"""Direct-child custody for the fake desktop update fixture, on POSIX.

Only this owner reaps these retained Popen objects. Every poll/signal/wait is
serialized per child: a child dying between poll and kill remains unreaped,
so its PID cannot be reused; a poll that reaps it records returncode first.
Neither a PID in a log nor an unrelated process can authorize a signal.
"""
import json
import os
from pathlib import Path
import secrets
import signal
import socket
import subprocess
import threading
import time


class Child:
    def __init__(self, process, path, token):
        self.process, self.path, self.token = process, Path(path), token
        self.lock = threading.Lock()


class OwnedChildren:
    def __init__(self, environment):
        if signal.getsignal(signal.SIGCHLD) != signal.SIG_DFL:
            raise RuntimeError('fake GUI custody requires the default child reaper')
        self.environment = environment
        self.children, self.problems = [], []
        self.lock = threading.Lock()
        self.pending, self.closing = 0, False

    def start(self, path):
        with self.lock:
            if self.closing:
                raise RuntimeError('fake GUI custody is closing')
            self.pending += 1
        try:
            token = secrets.token_hex(16)
            child = Child(None, path, token)
            with self.lock:
                self.children.append(child)
            # The retained record exists before spawn; assign the returned Popen
            # before any subsequent timestamp, probe, callback or logging.
            child.process = subprocess.Popen([str(path)], start_new_session=True,
                stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                env=dict(self.environment, TRAY_LAUNCH_TOKEN=token), close_fds=True)
            return child
        except BaseException as error:
            with self.lock:
                self.problems.append('launch: ' + str(error))
            raise
        finally:
            with self.lock:
                self.pending -= 1

    def running(self, path, rows):
        with self.lock:
            children = list(self.children)
        live = set()
        for child in children:
            with child.lock:
                if child.process is not None and child.path == Path(path) and child.process.poll() is None:
                    live.add((child.process.pid, child.token))
        return [(variant, int(pid)) for variant, pid, token in rows if (int(pid), token) in live]

    def stop_all(self, seconds=5):
        clock_problems = []
        try:
            end = time.monotonic() + seconds
        except BaseException as error:
            end = None
            clock_problems.append('cleanup clock: ' + str(error))
        with self.lock:
            self.closing = True
            children = list(self.children)
            problems = list(self.problems) + clock_problems
            if self.pending:
                problems.append('a child launch is still pending')
        # Try every owned child before waiting for any one of them.
        for child in children:
            if child.process is None:
                continue
            try:
                with child.lock:
                    if child.process.poll() is None:
                        child.process.kill()
            except ProcessLookupError:
                pass  # This unreaped direct child may already have exited.
            except BaseException as error:
                problems.append('stop owned child: ' + str(error))
        for child in children:
            if child.process is None:
                continue
            try:
                remaining = 0
                if end is not None:
                    try:
                        remaining = max(0, end - time.monotonic())
                    except BaseException as error:
                        problems.append('cleanup clock: ' + str(error))
                with child.lock:
                    child.process.wait(timeout=remaining)
            except BaseException as error:
                problems.append('reap owned child: ' + str(error))
        return problems

    def snapshot(self):
        with self.lock:
            return dict(pending=self.pending, children=[dict(pid=c.process.pid if c.process else None,
                path=str(c.path), token=c.token, returncode=c.process.returncode if c.process else None)
                for c in self.children])


def receive(connection, length, end):
    result = b''
    while len(result) < length:
        remaining = end - time.monotonic()
        if remaining <= 0:
            raise TimeoutError('fake service request expired')
        connection.settimeout(remaining)
        part = connection.recv(length - len(result))
        if not part:
            raise RuntimeError('incomplete fake service request')
        result += part
    return result


class StartControl:
    """A private fixed start request; the test remains every GUI's parent."""
    def __init__(self, path, start):
        self.path, self.start = Path(path), start
        if len(os.fsencode(self.path)) >= 104:
            raise ValueError('fake service socket path exceeds its POSIX bound')
        self.problems, self.stopping = [], threading.Event()
        self.socket = None
        self.thread = None

    def open(self):
        # The caller owns this controller before any socket/thread allocation.
        self.socket = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.socket.bind(str(self.path))
        self.socket.listen(4)
        self.socket.settimeout(0.1)
        self.thread = threading.Thread(target=self.serve, daemon=True)
        self.thread.start()

    def serve(self):
        while not self.stopping.is_set():
            try:
                connection, _ = self.socket.accept()
            except socket.timeout:
                continue
            except OSError as error:
                if not self.stopping.is_set():
                    self.problems.append('fake service accept: ' + str(error))
                return
            try:
                with connection:
                    end = time.monotonic() + 3
                    if receive(connection, 6, end) != b'start\n':
                        raise RuntimeError('unknown fake service request')
                    remaining = end - time.monotonic()
                    if remaining <= 0:
                        raise TimeoutError('fake service request expired')
                    connection.settimeout(remaining)
                    if connection.recv(1) != b'':
                        raise RuntimeError('extra fake service request bytes')
                    self.start()
                    remaining = end - time.monotonic()
                    if remaining <= 0:
                        raise TimeoutError('fake service start expired')
                    connection.settimeout(remaining)
                    connection.sendall(b'ok\n')
            except BaseException as error:
                self.problems.append('fake service start: ' + str(error))

    def stop(self):
        self.stopping.set()
        problems = list(self.problems)
        if self.socket is not None:
            try:
                self.socket.close()
            except BaseException as error:
                problems.append('fake service close: ' + str(error))
        if self.thread is not None:
            try:
                if self.thread.ident is not None:
                    self.thread.join(timeout=4)
                if self.thread.is_alive():
                    problems.append('fake service thread did not stop')
            except BaseException as error:
                problems.append('fake service join: ' + str(error))
        # Include a refusal recorded while the final accepted request unwound.
        return problems + [p for p in self.problems if p not in problems]


def cleanup(root, control, children, remove):
    problems = []
    for name, action in (('control', control.stop if control else None),
                         ('children', children.stop_all if children else None)):
        if action:
            try:
                problems.extend(action())
            except BaseException as error:
                problems.append(name + ': ' + str(error))
    if not problems:
        try:
            remove(root)
        except BaseException as error:
            problems.append('remove: ' + str(error))
    if problems:
        record = dict(problems=problems, root=str(root))
        try:
            record['owned'] = children.snapshot() if children else None
        except BaseException as error:
            record['snapshot_error'] = str(error)
        try:
            (Path(root) / 'retained-fixture.json').write_text(json.dumps(record, indent=2))
        except BaseException as error:
            record['record_error'] = str(error)
        print('ZERUS_TRAY_FIXTURE_PRESERVED=' + json.dumps(record), flush=True)
        raise AssertionError('fake GUI custody: ' + '; '.join(problems))
