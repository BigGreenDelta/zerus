"""Bounded Windows-x64 test oracle, never a product process reader.

The PEB/RTL_USER_PROCESS_PARAMETERS environment offsets are an internal x64
Windows ABI, not a public compatibility promise. The disposable Windows gate
must qualify this reader against a private known-environment Python child
before launching Kimi. Missing APIs, another ABI, inaccessible/partial memory,
changed identities or inconsistent copies refuse; none imply an empty env.
"""
import ctypes
import os

MAX_ENVIRONMENT = 1024 * 1024
MAX_ROWS = 4096


def decode_environment(data):
    if not isinstance(data, bytes) or not 4 <= len(data) <= MAX_ENVIRONMENT or len(data) % 2:
        raise ValueError('environment size is not bounded UTF-16')
    text = data.decode('utf-16-le', errors='strict')
    if not text.endswith('\0\0') or '\0\0' in text[:-2]:
        raise ValueError('environment has no exact terminal double NUL')
    rows = text[:-2].split('\0') if text[:-2] else []
    if len(rows) > MAX_ROWS:
        raise ValueError('environment row count exceeds its bound')
    result = {}
    for row in rows:
        # Windows may add its private per-drive cwd entries, e.g. =C:=C:\work.
        split = row.find('=', 1 if row.startswith('=') else 0)
        if split <= 0:
            raise ValueError('environment row has no key/value separator')
        key, value = row[:split].upper(), row[split + 1:]
        if key in result or any(ord(char) < 32 for char in key):
            raise ValueError('environment key is repeated or invalid')
        result[key] = value
    return result


def api():
    if os.name != 'nt' or ctypes.sizeof(ctypes.c_void_p) != 8:
        raise OSError('the environment oracle requires native Windows x64 Python')
    from ctypes import wintypes as w
    k, n, a = (ctypes.WinDLL(name, use_last_error=True) for name in ('kernel32', 'ntdll', 'advapi32'))
    k.OpenProcess.argtypes, k.OpenProcess.restype = (w.DWORD, w.BOOL, w.DWORD), w.HANDLE
    k.CloseHandle.argtypes, k.CloseHandle.restype = (w.HANDLE,), w.BOOL
    k.GetCurrentProcess.restype = w.HANDLE
    k.GetProcessTimes.argtypes = (w.HANDLE,) + (ctypes.POINTER(w.FILETIME),) * 4
    k.GetProcessTimes.restype = w.BOOL
    k.QueryFullProcessImageNameW.argtypes = (w.HANDLE, w.DWORD, w.LPWSTR, ctypes.POINTER(w.DWORD))
    k.QueryFullProcessImageNameW.restype = w.BOOL
    k.WaitForSingleObject.argtypes, k.WaitForSingleObject.restype = (w.HANDLE, w.DWORD), w.DWORD
    k.TerminateProcess.argtypes, k.TerminateProcess.restype = (w.HANDLE, w.UINT), w.BOOL
    k.IsWow64Process.argtypes, k.IsWow64Process.restype = (w.HANDLE, ctypes.POINTER(w.BOOL)), w.BOOL
    k.ReadProcessMemory.argtypes = (w.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t,
                                   ctypes.POINTER(ctypes.c_size_t))
    k.ReadProcessMemory.restype = w.BOOL
    k.VirtualQueryEx.argtypes = (w.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t)
    k.VirtualQueryEx.restype = ctypes.c_size_t
    n.NtQueryInformationProcess.argtypes = (w.HANDLE, w.ULONG, ctypes.c_void_p, w.ULONG,
                                           ctypes.POINTER(w.ULONG))
    n.NtQueryInformationProcess.restype = ctypes.c_long
    a.OpenProcessToken.argtypes, a.OpenProcessToken.restype = (w.HANDLE, w.DWORD, ctypes.POINTER(w.HANDLE)), w.BOOL
    a.GetTokenInformation.argtypes = (w.HANDLE, w.DWORD, ctypes.c_void_p, w.DWORD, ctypes.POINTER(w.DWORD))
    a.GetTokenInformation.restype = w.BOOL
    a.GetLengthSid.argtypes, a.GetLengthSid.restype = (ctypes.c_void_p,), w.DWORD
    return k, n, a, w


class Process:
    """A retained process handle, binding full creation ticks, owner and image.

    A handle is never reopened for termination. The caller owns close(), even
    after a failed proof, and retains its fixture while shutdown is unknown.
    """

    def __init__(self, pid, created):
        self.k, self.n, self.a, self.w = api()
        self.pid, self.created, self.handle = pid, str(created), None
        # SYNCHRONIZE | TERMINATE | QUERY_INFORMATION | QUERY_LIMITED | VM_READ.
        self.handle = self.k.OpenProcess(0x00101411, False, pid)
        if not self.handle:
            raise OSError('cannot open owned process for proof')
        try:
            if self.ticks() != self.created:
                raise OSError('the recorded process was replaced')
            wow = self.w.BOOL()
            if not self.k.IsWow64Process(self.handle, ctypes.byref(wow)) or wow.value:
                raise OSError('the environment reader refuses WOW64')
            if self.sid(self.handle) != self.sid(self.k.GetCurrentProcess()):
                raise OSError('the process is not owned by this user')
        except BaseException:
            self.close()
            raise

    def ticks(self):
        times = [self.w.FILETIME() for _ in range(4)]
        if not self.k.GetProcessTimes(self.handle, *(ctypes.byref(item) for item in times)):
            raise OSError('cannot read process creation time')
        return str((times[0].dwHighDateTime << 32) | times[0].dwLowDateTime)

    def sid(self, process):
        token = self.w.HANDLE()
        if not self.a.OpenProcessToken(process, 8, ctypes.byref(token)):
            raise OSError('cannot read process owner')
        try:
            needed = self.w.DWORD()
            self.a.GetTokenInformation(token, 1, None, 0, ctypes.byref(needed))
            if not 16 <= needed.value <= 65536:
                raise OSError('unbounded TokenUser')
            data = ctypes.create_string_buffer(needed.value)
            if not self.a.GetTokenInformation(token, 1, data, len(data), ctypes.byref(needed)):
                raise OSError('cannot read TokenUser')
            if not 16 <= needed.value <= len(data):
                raise OSError('TokenUser returned an invalid size')
            pointer = ctypes.c_void_p.from_buffer(data).value
            first, end = ctypes.addressof(data), ctypes.addressof(data) + needed.value
            # Even GetLengthSid reads its pointer: admit the fixed SID header
            # first, then its declared subauthorities, before any SID API call.
            if not pointer or not first <= pointer <= end - 8:
                raise OSError('TokenUser SID header is outside its buffer')
            header = ctypes.string_at(pointer, 8)
            expected = 8 + 4 * header[1]
            if header[0] != 1 or header[1] > 15 or pointer + expected > end:
                raise OSError('TokenUser SID declaration is outside its buffer')
            length = self.a.GetLengthSid(pointer)
            if length != expected or not 8 <= length <= 68 or pointer + length > end:
                raise OSError('invalid TokenUser SID')
            return ctypes.string_at(pointer, length)
        finally:
            if not self.k.CloseHandle(token):
                raise OSError('owner-token close is unknown')

    def read(self, address, length):
        if not 0 < address < (1 << 47) or not 0 < length <= MAX_ENVIRONMENT:
            raise OSError('remote read is outside bounds')
        data, got = ctypes.create_string_buffer(length), ctypes.c_size_t()
        if not self.k.ReadProcessMemory(self.handle, address, data, length, ctypes.byref(got)) or got.value != length:
            raise OSError('partial remote read')
        return data.raw

    def snapshot(self):
        if self.k.WaitForSingleObject(self.handle, 0) != 258 or self.ticks() != self.created:
            raise OSError('the recorded process is not running')
        if self.sid(self.handle) != self.sid(self.k.GetCurrentProcess()):
            raise OSError('the recorded process owner changed')
        path, size = ctypes.create_unicode_buffer(32768), self.w.DWORD(32768)
        if not self.k.QueryFullProcessImageNameW(self.handle, 0, path, ctypes.byref(size)):
            raise OSError('cannot read the running image')
        # PROCESS_BASIC_INFORMATION is six pointer-sized fields on native x64.
        basic = (ctypes.c_uint64 * 6)()
        returned = self.w.ULONG()
        status = self.n.NtQueryInformationProcess(self.handle, 0, basic, ctypes.sizeof(basic), ctypes.byref(returned))
        if status != 0 or returned.value != ctypes.sizeof(basic) or basic[4] != self.pid:
            raise OSError('PROCESS_BASIC_INFORMATION layout is not admitted')
        parameters = int.from_bytes(self.read(basic[1] + 0x20, 8), 'little')
        environment = int.from_bytes(self.read(parameters + 0x80, 8), 'little')
        return dict(pid=self.pid, created=self.created, image=os.path.normcase(os.path.realpath(path.value)),
                    parent=int(basic[5]), parameters=parameters, environment=environment)

    def block(self, address):
        class Memory(ctypes.Structure):
            _fields_ = [('base', ctypes.c_void_p), ('allocation', ctypes.c_void_p), ('allocation_protect', self.w.DWORD),
                        ('partition', self.w.WORD), ('region', ctypes.c_size_t), ('state', self.w.DWORD),
                        ('protect', self.w.DWORD), ('kind', self.w.DWORD)]
        if ctypes.sizeof(Memory) != 48:
            raise OSError('MEMORY_BASIC_INFORMATION layout is not admitted')
        data = bytearray()
        while len(data) < MAX_ENVIRONMENT:
            cursor, region = address + len(data), Memory()
            if self.k.VirtualQueryEx(self.handle, cursor, ctypes.byref(region), ctypes.sizeof(region)) != 48:
                raise OSError('cannot bound the environment memory region')
            if region.state != 0x1000 or region.protect & (0x100 | 1):
                raise OSError('environment memory is not committed/readable')
            remaining = region.base + region.region - cursor
            count = min(4096, MAX_ENVIRONMENT - len(data), remaining)
            if count < 2 or count % 2:
                raise OSError('unaligned environment memory')
            data.extend(self.read(cursor, count))
            # Search at UTF-16 code-unit boundaries only.
            for end in range(max(0, len(data) - count - 2), len(data) - 3, 2):
                if data[end:end + 4] == b'\0\0\0\0':
                    return bytes(data[:end + 4])
        raise OSError('environment exceeds its bound')

    def environment(self, image, parent):
        before = self.snapshot()
        if before['image'] != os.path.normcase(os.path.realpath(image)) or before['parent'] != parent:
            raise OSError('running image/parent does not match the owned binding')
        first = self.block(before['environment'])
        second = self.block(before['environment'])
        if first != second or self.snapshot() != before:
            raise OSError('process/environment changed during proof')
        return decode_environment(first), {key: value for key, value in before.items()
                                           if key not in ('parameters', 'environment')}

    def wait(self, seconds):
        return self.k.WaitForSingleObject(self.handle, max(0, int(seconds * 1000))) == 0

    def terminate(self):
        if not self.k.TerminateProcess(self.handle, 1):
            raise OSError('owned process-handle termination failed')

    def close(self):
        if self.handle:
            handle, self.handle = self.handle, None
            if not self.k.CloseHandle(handle):
                raise OSError('owned process-handle close is unknown')
