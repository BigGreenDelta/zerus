"""Windows fixture worker for the test helper shim (support/TestHelper.cpp).

Starting Python and importing json or pathlib takes most of a fixture call on
Windows. A worker keeps one interpreter, with those modules loaded, and runs the
fixture scripts of one test program, one call at a time, each on its own instance
of the named pipe \\\\.\\pipe\\zerus-fixture-<test program PID>. The shim sends
the call (script, arguments, working directory, environment and its own PID) and
then its standard input; the worker answers with the fixture's standard output,
standard error and exit code. Fixtures see what they would see in a process of
their own, started like the shim's slow path: the same argv, __file__,
environment, UTF-8 streams with LF line ends and file semantics (see the
bootstrap in TestHelper.cpp). If the shim dies during a call, as with
QProcess::kill(), the worker ends at once, which ends that fixture. A worker
also ends with its test program.

Frames are a type byte, a little-endian 32-bit length and the data. From the
shim: 0 the call (JSON), 1 standard input, 2 end of standard input. To the shim:
4 the worker PID, 1 standard output, 2 standard error, 3 the exit code (32 bits).
"""
import _winapi
import builtins
import gc
import io
import json
import msvcrt
import os
import pathlib  # noqa: F401  (loaded once for the fixtures)
import struct
import subprocess
import sys
import threading
import time  # noqa: F401
import traceback
import types

POOL = sys.argv[1]
NAME = r'\\.\pipe\zerus-fixture-' + POOL
WORKERS = 6
# A process keeps its working directory from being deleted, so between calls a
# worker stays here instead of in a test's temporary directory.
HOME = os.path.dirname(os.path.abspath(__file__))
INFINITE = 0xFFFFFFFF
PIPE_REJECT_REMOTE_CLIENTS = 0x8
ERROR_PIPE_CONNECTED = 535


# As the shim's bootstrap: LF for text files, appends that never interleave and
# reads that do not stop tests from deleting a file.
def _patch_open():
    original = io.open

    def patched(file, mode='r', *args, **kwargs):
        if 'b' not in mode:
            if len(args) >= 4:
                if args[3] is None:
                    args = args[:3] + ('',) + args[4:]
            elif kwargs.get('newline') is None:
                kwargs['newline'] = ''
        if isinstance(file, (str, bytes, os.PathLike)) and '+' not in mode:
            if 'a' in mode:
                file = msvcrt.open_osfhandle(_winapi.CreateFile(os.fsdecode(file), 0x100084, 7, 0, 4, 0x80, 0),
                                             os.O_WRONLY | os.O_APPEND)
            elif not any(m in mode for m in 'wx'):
                file = msvcrt.open_osfhandle(_winapi.CreateFile(os.fsdecode(file), 0x80000000, 7, 0, 3, 0x80, 0),
                                             os.O_RDONLY)
        return original(file, mode, *args, **kwargs)

    builtins.open = io.open = patched
    return original


raw_open = _patch_open()


class Pipe:
    def __init__(self, handle):
        self.handle = handle

    def write(self, kind, data=b''):
        data = struct.pack('<BI', kind, len(data)) + bytes(data)
        while data:
            written, _ = _winapi.WriteFile(self.handle, data)
            data = data[written:]

    def read_exact(self, size):
        chunks = []
        while size:
            data, _ = _winapi.ReadFile(self.handle, size)
            if not data:
                raise BrokenPipeError
            chunks.append(data)
            size -= len(data)
        return b''.join(chunks)

    def read(self):
        kind, size = struct.unpack('<BI', self.read_exact(5))
        return kind, self.read_exact(size)


class Output(io.RawIOBase):
    def __init__(self, pipe, kind):
        self.pipe, self.kind = pipe, kind

    def writable(self):
        return True

    def write(self, data):
        self.pipe.write(self.kind, data)
        return len(data)


class Input(io.RawIOBase):
    def __init__(self, pipe):
        self.pipe, self.pending, self.ended = pipe, b'', False

    def readable(self):
        return True

    def readinto(self, buffer):
        while not self.pending and not self.ended:
            kind, data = self.pipe.read()
            if kind == 2:
                self.ended = True
            else:
                self.pending = data
        count = min(len(buffer), len(self.pending))
        buffer[:count] = self.pending[:count]
        self.pending = self.pending[count:]
        return count


def exit_code(error):
    code = error.code
    if code is None:
        return 0
    if isinstance(code, int):
        return code
    print(code, file=sys.stderr)
    return 1


compiled = {}


def run(pipe, call):
    finished = threading.Event()
    shim = _winapi.OpenProcess(_winapi.SYNCHRONIZE, False, call['pid'])

    def watch():
        _winapi.WaitForSingleObject(shim, INFINITE)
        if not finished.is_set():
            os._exit(1)
        _winapi.CloseHandle(shim)

    threading.Thread(target=watch, daemon=True).start()
    script = call['script']
    sys.argv = [script] + call['arguments']
    sys.stdin = io.TextIOWrapper(io.BufferedReader(Input(pipe)), encoding='utf-8', errors='surrogateescape')
    sys.stdout = io.TextIOWrapper(io.BufferedWriter(Output(pipe, 1)), encoding='utf-8', errors='surrogateescape',
                                  newline='\n')
    sys.stderr = io.TextIOWrapper(io.BufferedWriter(Output(pipe, 2)), encoding='utf-8', errors='backslashreplace',
                                  newline='\n', line_buffering=True)
    main = types.ModuleType('__main__')
    main.__file__ = script
    main.__builtins__ = builtins
    saved_main = sys.modules['__main__']
    sys.modules['__main__'] = main
    code = 0
    try:
        os.environ.clear()
        os.environ.update(call['environment'])
        os.chdir(call['directory'])
        with raw_open(script, 'rb') as file:
            source = file.read()
        if source not in compiled:
            compiled[source] = compile(source, script, 'exec')
        exec(compiled[source], main.__dict__)
    except SystemExit as error:
        code = exit_code(error)
    except BaseException:
        traceback.print_exc()
        code = 1
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.flush()
        except Exception:
            pass
    sys.modules['__main__'] = saved_main
    sys.stdin = sys.stdout = sys.stderr = None
    # A process would close the fixture's files when it ended; tests then replace
    # or delete them.
    main.__dict__.clear()
    gc.collect()
    os.chdir(HOME)
    finished.set()
    pipe.write(3, struct.pack('<I', code & 0xFFFFFFFF))
    # The shim closes the pipe once it has the exit code; input it still sends
    # until then belongs to this call.
    try:
        while True:
            pipe.read()
    except OSError:
        pass


def start_worker():
    line = subprocess.list2cmdline([sys.executable, '-I', '-X', 'utf8', '-B', os.path.abspath(__file__), POOL])
    process, thread, _, _ = _winapi.CreateProcess(sys.executable, line, None, None, False, _winapi.DETACHED_PROCESS,
                                                  None, HOME, subprocess.STARTUPINFO())
    _winapi.CloseHandle(thread)
    _winapi.CloseHandle(process)


def idle_worker_exists():
    try:
        _winapi.WaitNamedPipe(NAME, 1)
        return True
    except OSError:
        return False


def serve():
    os.chdir(HOME)
    test = _winapi.OpenProcess(_winapi.SYNCHRONIZE, False, int(POOL))
    threading.Thread(target=lambda: (_winapi.WaitForSingleObject(test, INFINITE), os._exit(0)), daemon=True).start()
    while True:
        try:
            handle = _winapi.CreateNamedPipe(
                NAME, _winapi.PIPE_ACCESS_DUPLEX, _winapi.PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                WORKERS, 65536, 65536, 0, _winapi.NULL)
        except OSError:
            return  # As many workers as the pool allows.
        try:
            _winapi.ConnectNamedPipe(handle)
        except OSError as error:
            if error.winerror != ERROR_PIPE_CONNECTED:
                _winapi.CloseHandle(handle)
                continue
        if not idle_worker_exists():
            start_worker()
        pipe = Pipe(handle)
        try:
            pipe.write(4, str(os.getpid()).encode())
            kind, data = pipe.read()
            if kind == 0:
                run(pipe, json.loads(data))
        except OSError:
            pass
        _winapi.CloseHandle(handle)


if __name__ == '__main__':
    serve()
