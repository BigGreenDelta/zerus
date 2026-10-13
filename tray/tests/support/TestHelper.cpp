// Windows test helper, built only on Windows (see tests/CMakeLists.txt). Two modes:
//
// Shim: a copy named after a fixture (for example <dir>/hgs.exe next to the
// Python script <dir>/hgs) runs that script with the Python found at configure
// time. The rest of this process's command line is passed through untouched,
// so the fixture sees exactly the argv QProcess or a pseudoconsole produced, as
// hgs.exe would. Python is told to use UTF-8 and LF line ends, like the Unix
// fixtures. Shim and child run in a kill-on-close job: killing the shim, as
// QProcess::kill() does, ends the fixture as well. Outside a console, the shim
// first offers the call to a worker of the test program's pool
// (support/fixture-worker.py), which runs fixtures in a warm interpreter and
// ends a fixture whose shim dies in the same way.
//
// Subcommands: run under its own name, the helper replaces /bin/sh in the
// ProcessRunner and pseudoconsole tests. Arguments are steps run in order, for
// example `fill out 200000 x err oops exit 3`; `read`, `size` and `got` read a
// console line, print the console size and print the line back. This mode joins
// no job: `spawn <pidfile>` leaves a child without a console that writes its
// process ID there and sleeps, so only a job of the caller can end it.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <string>
#include <vector>

#ifndef ZERUS_TEST_PYTHON
#error "ZERUS_TEST_PYTHON must name the Python interpreter for fixtures"
#endif
#ifndef ZERUS_FIXTURE_WORKER
#error "ZERUS_FIXTURE_WORKER must name support/fixture-worker.py"
#endif

namespace {

std::wstring widen(const char *text)
{
    const int size = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
    std::wstring result(size > 0 ? size - 1 : 0, L'\0');
    if (size > 1) MultiByteToWideChar(CP_UTF8, 0, text, -1, result.data(), size);
    return result;
}

std::string narrow(const std::wstring &text)
{
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), int(text.size()), nullptr, 0, nullptr, nullptr);
    std::string result(size, '\0');
    if (size > 0) WideCharToMultiByte(CP_UTF8, 0, text.data(), int(text.size()), result.data(), size, nullptr, nullptr);
    return result;
}

void writeAll(DWORD which, const char *data, size_t size)
{
    HANDLE handle = GetStdHandle(which);
    while (size > 0) {
        DWORD written = 0;
        if (!WriteFile(handle, data, DWORD(size > 65536 ? 65536 : size), &written, nullptr) || written == 0) return;
        data += written; size -= written;
    }
}

void writeText(DWORD which, const std::string &text) { writeAll(which, text.data(), text.size()); }

// MSVC runtime quoting, the rules Python uses to split its command line.
std::wstring quote(const std::wstring &value)
{
    if (!value.empty() && value.find_first_of(L" \t\n\v\"") == std::wstring::npos) return value;
    std::wstring result = L"\"";
    for (size_t i = 0;; ++i) {
        size_t backslashes = 0;
        while (i < value.size() && value[i] == L'\\') { ++i; ++backslashes; }
        if (i == value.size()) { result.append(backslashes * 2, L'\\'); break; }
        if (value[i] == L'"') { result.append(backslashes * 2 + 1, L'\\'); result.push_back(L'"'); }
        else { result.append(backslashes, L'\\'); result.push_back(value[i]); }
    }
    result.push_back(L'"');
    return result;
}

// The command line after the program token, byte for byte.
std::wstring commandTail()
{
    const wchar_t *line = GetCommandLineW();
    if (*line == L'"') {
        ++line;
        while (*line && *line != L'"') ++line;
        if (*line) ++line;
    } else {
        while (*line && *line != L' ' && *line != L'\t') ++line;
    }
    return line;
}

std::wstring modulePath()
{
    std::wstring path(32768, L'\0');
    const DWORD size = GetModuleFileNameW(nullptr, path.data(), DWORD(path.size()));
    path.resize(size);
    return path;
}

std::wstring baseName(const std::wstring &path)
{
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

bool endsWithInsensitive(const std::wstring &value, const std::wstring &suffix)
{
    return value.size() >= suffix.size()
        && _wcsicmp(value.c_str() + value.size() - suffix.size(), suffix.c_str()) == 0;
}

std::string jsonString(const std::wstring &value)
{
    std::string result = "\"";
    for (const char c : narrow(value)) {
        if (c == '"' || c == '\\') {
            result += '\\';
            result += c;
        } else if (static_cast<unsigned char>(c) < 0x20) {
            char escaped[8];
            std::snprintf(escaped, sizeof(escaped), "\\u%04x", static_cast<unsigned char>(c));
            result += escaped;
        } else {
            result += c;
        }
    }
    return result + "\"";
}

// The pool pipe is opened for overlapped I/O, so the input thread can write to it
// while the main thread reads: synchronous I/O on one handle would serialize them.
bool pipeTransfer(HANDLE pipe, char *data, DWORD size, bool write)
{
    OVERLAPPED overlapped{};
    overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    bool ok = overlapped.hEvent != nullptr;
    while (ok && size > 0) {
        DWORD done = 0;
        ResetEvent(overlapped.hEvent);
        const BOOL started = write ? WriteFile(pipe, data, size, nullptr, &overlapped)
                                   : ReadFile(pipe, data, size, nullptr, &overlapped);
        ok = (started || GetLastError() == ERROR_IO_PENDING) && GetOverlappedResult(pipe, &overlapped, &done, TRUE)
            && done > 0;
        data += done; size -= done;
    }
    if (overlapped.hEvent) CloseHandle(overlapped.hEvent);
    return ok;
}

// A frame is a type byte, a little-endian 32-bit length and the data.
bool sendFrame(HANDLE pipe, char kind, const char *data, DWORD size)
{
    std::string frame{kind, char(size), char(size >> 8), char(size >> 16), char(size >> 24)};
    frame.append(data, size);
    return pipeTransfer(pipe, frame.data(), DWORD(frame.size()), true);
}

bool receiveFrame(HANDLE pipe, char &kind, std::string &data)
{
    unsigned char header[5];
    if (!pipeTransfer(pipe, reinterpret_cast<char *>(header), 5, false)) return false;
    kind = char(header[0]);
    data.resize(size_t(header[1]) | size_t(header[2]) << 8 | size_t(header[3]) << 16 | size_t(header[4]) << 24);
    return data.empty() || pipeTransfer(pipe, data.data(), DWORD(data.size()), false);
}

DWORD WINAPI forwardInput(void *pipe)
{
    const HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
    std::vector<char> buffer(65536);
    DWORD count = 0;
    while (input && input != INVALID_HANDLE_VALUE && ReadFile(input, buffer.data(), DWORD(buffer.size()), &count, nullptr)
           && count > 0) {
        if (!sendFrame(pipe, 1, buffer.data(), count)) return 0;
    }
    sendFrame(pipe, 2, nullptr, 0);
    return 0;
}

std::wstring workerPython() { return widen(ZERUS_TEST_PYTHON); }

// Detached, without inherited handles: a worker never holds a test's pipes open.
void startWorker(const std::wstring &pool)
{
    const std::wstring worker = widen(ZERUS_FIXTURE_WORKER);
    const std::wstring directory = worker.substr(0, worker.find_last_of(L"\\/"));
    std::wstring commandLine = quote(workerPython()) + L" -I -X utf8 -B " + quote(worker) + L" " + pool;
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION info{};
    if (CreateProcessW(workerPython().c_str(), commandLine.data(), nullptr, nullptr, FALSE, DETACHED_PROCESS, nullptr,
                       directory.c_str(), &startup, &info)) {
        CloseHandle(info.hThread);
        CloseHandle(info.hProcess);
    }
}

bool console(DWORD which)
{
    DWORD mode = 0;
    const HANDLE handle = GetStdHandle(which);
    return handle && handle != INVALID_HANDLE_VALUE && GetConsoleMode(handle, &mode);
}

// Runs the call on a pool worker, or returns false when none can take it, before
// the fixture has started.
bool runPooled(const std::wstring &script, int argc, wchar_t **argv, int &exitCode)
{
    wchar_t pool[32];
    const DWORD length = GetEnvironmentVariableW(L"ZERUS_FIXTURE_POOL", pool, 32);
    if (length == 0 || length >= 32 || std::wcscmp(pool, L"0") == 0 || console(STD_INPUT_HANDLE)
        || console(STD_OUTPUT_HANDLE) || console(STD_ERROR_HANDLE))
        return false;
    const std::wstring name = std::wstring(L"\\\\.\\pipe\\zerus-fixture-") + pool;
    const HANDLE pipe = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                                    FILE_FLAG_OVERLAPPED, nullptr);
    if (pipe == INVALID_HANDLE_VALUE) {
        if (GetLastError() == ERROR_FILE_NOT_FOUND) startWorker(pool);
        return false;
    }
    char kind = 0;
    std::string data;
    HANDLE worker = nullptr;
    if (receiveFrame(pipe, kind, data) && kind == 4)
        worker = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, DWORD(std::strtoul(data.c_str(), nullptr, 10)));
    std::string call = "{\"pid\":" + std::to_string(GetCurrentProcessId()) + ",\"script\":" + jsonString(script);
    std::wstring directory(32768, L'\0');
    directory.resize(GetCurrentDirectoryW(DWORD(directory.size()), directory.data()));
    call += ",\"directory\":" + jsonString(directory) + ",\"arguments\":[";
    for (int i = 1; i < argc; ++i) call += (i > 1 ? "," : "") + jsonString(argv[i]);
    call += "],\"environment\":{";
    bool first = true;
    if (wchar_t *block = GetEnvironmentStringsW()) {
        for (const wchar_t *entry = block; *entry; entry += std::wcslen(entry) + 1) {
            const std::wstring text = entry;
            const size_t equals = text.find(L'=', 1);
            if (text[0] == L'=' || equals == std::wstring::npos) continue;
            call += (first ? "" : ",") + jsonString(text.substr(0, equals)) + ":" + jsonString(text.substr(equals + 1));
            first = false;
        }
        FreeEnvironmentStringsW(block);
    }
    call += "}}";
    if (!worker || !sendFrame(pipe, 0, call.data(), DWORD(call.size()))) {
        if (worker) CloseHandle(worker);
        CloseHandle(pipe);
        return false;
    }
    if (HANDLE thread = CreateThread(nullptr, 0, forwardInput, pipe, 0, nullptr)) CloseHandle(thread);
    while (receiveFrame(pipe, kind, data)) {
        if (kind == 1 || kind == 2) {
            writeAll(kind == 1 ? STD_OUTPUT_HANDLE : STD_ERROR_HANDLE, data.data(), data.size());
        } else if (kind == 3 && data.size() == 4) {
            exitCode = int(unsigned(static_cast<unsigned char>(data[0])) | unsigned(static_cast<unsigned char>(data[1])) << 8
                | unsigned(static_cast<unsigned char>(data[2])) << 16 | unsigned(static_cast<unsigned char>(data[3])) << 24);
            CloseHandle(worker);
            return true;
        }
    }
    // The worker ended during the call (a fixture may call os._exit).
    DWORD code = 1;
    if (WaitForSingleObject(worker, 5000) != WAIT_OBJECT_0 || !GetExitCodeProcess(worker, &code)) code = 1;
    CloseHandle(worker);
    exitCode = int(code);
    return true;
}

int runFixture(const std::wstring &program, int argc, wchar_t **argv)
{
    std::wstring script = program;
    if (endsWithInsensitive(script, L".exe")) script.resize(script.size() - 4);
    std::string firstLine;
    if (FILE *file = _wfopen(script.c_str(), L"rb")) {
        int c;
        while ((c = std::fgetc(file)) != EOF && c != '\n') firstLine.push_back(char(c));
        std::fclose(file);
    }
    if (!firstLine.empty() && firstLine.back() == '\r') firstLine.pop_back();
    if (firstLine.rfind("#!", 0) != 0 || firstLine.find("python") == std::string::npos) {
        writeText(STD_ERROR_HANDLE, "fixture " + narrow(script) + " must be a Python script on Windows\n");
        return 126;
    }
    // As on Unix: LF line ends for stdio and files opened in text mode, appends
    // that never interleave (concurrent fixtures share log files; a handle with
    // FILE_APPEND_DATA but not FILE_WRITE_DATA appends atomically), and reads that
    // do not stop tests from deleting a file (FILE_SHARE_DELETE; replacing one
    // still fails, see FakeCli::replace). sys.argv and __file__ as if the script
    // had been run directly.
    static const wchar_t bootstrap[] =
        L"import sys,io,os,builtins,runpy,_winapi,msvcrt\n"
        L"[s.reconfigure(newline='\\n') for s in (sys.stdout,sys.stderr)]\n"
        L"o=io.open\n"
        L"def f(file,mode='r',*a,**k):\n"
        L" if 'b' not in mode:\n"
        L"  if len(a)>=4:\n"
        L"   if a[3] is None: a=a[:3]+('',)+a[4:]\n"
        L"  elif k.get('newline') is None: k['newline']=''\n"
        L" if isinstance(file,(str,bytes,os.PathLike)) and '+' not in mode:\n"
        L"  if 'a' in mode:file=msvcrt.open_osfhandle(_winapi.CreateFile(os.fsdecode(file),0x100084,7,0,4,0x80,0),os.O_WRONLY|os.O_APPEND)\n"
        L"  elif not any(m in mode for m in 'wx'):file=msvcrt.open_osfhandle(_winapi.CreateFile(os.fsdecode(file),0x80000000,7,0,3,0x80,0),os.O_RDONLY)\n"
        L" return o(file,mode,*a,**k)\n"
        L"builtins.open=io.open=f\n"
        L"sys.argv=sys.argv[1:]\n"
        L"runpy.run_path(sys.argv[0],run_name='__main__')\n";
    const std::wstring python = widen(ZERUS_TEST_PYTHON);
    std::wstring commandLine = quote(python) + L" -I -X utf8 -B -c " + quote(bootstrap) + L" " + quote(script)
        + commandTail();

    SetEnvironmentVariableW(L"ZERUS_FAKE_PID", std::to_wstring(GetCurrentProcessId()).c_str());
    SetEnvironmentVariableW(L"ZERUS_FAKE_PROGRAM", program.c_str());
    int pooledCode = 0;
    if (runPooled(script, argc, argv, pooledCode)) return pooledCode;

    // The shim joins the job before it starts Python, so Python is in it from the
    // start: a kill between starting Python and assigning it would leave a
    // suspended orphan that keeps inherited pipes (and CTest) open.
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
        AssignProcessToJobObject(job, GetCurrentProcess());
    }
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    for (HANDLE handle : {startup.hStdInput, startup.hStdOutput, startup.hStdError})
        if (handle && handle != INVALID_HANDLE_VALUE) SetHandleInformation(handle, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
    // Without a console of our own (started by QProcess from a GUI test) Python
    // must not open a console window.
    DWORD consoleProcess = 0;
    const DWORD flags = GetConsoleProcessList(&consoleProcess, 1) ? 0 : CREATE_NO_WINDOW;
    PROCESS_INFORMATION info{};
    if (!CreateProcessW(python.c_str(), commandLine.data(), nullptr, nullptr, TRUE, flags, nullptr, nullptr, &startup, &info)) {
        writeText(STD_ERROR_HANDLE, "cannot start " + narrow(python) + "\n");
        return 127;
    }
    CloseHandle(info.hThread);
    WaitForSingleObject(info.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(info.hProcess, &code);
    CloseHandle(info.hProcess);
    return int(code);
}

std::string readConsoleLine()
{
    HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
    std::wstring line;
    wchar_t buffer[512];
    DWORD count = 0;
    while (ReadConsoleW(input, buffer, 512, &count, nullptr) && count > 0) {
        line.append(buffer, count);
        if (line.find(L'\n') != std::wstring::npos) break;
    }
    while (!line.empty() && (line.back() == L'\n' || line.back() == L'\r')) line.pop_back();
    return narrow(line);
}

int runSteps(int argc, wchar_t **argv)
{
    std::string line;
    for (int i = 1; i < argc; ++i) {
        const std::wstring step = argv[i];
        auto next = [&]() -> std::wstring { return i + 1 < argc ? argv[++i] : std::wstring(); };
        if (step == L"out" || step == L"err") {
            writeText(step == L"out" ? STD_OUTPUT_HANDLE : STD_ERROR_HANDLE, narrow(next()));
        } else if (step == L"line") {
            writeText(STD_OUTPUT_HANDLE, narrow(next()) + "\n");
        } else if (step == L"fill") {
            const auto stream = next(); const auto count = _wtoi(next().c_str()); const auto c = next();
            std::string bytes(size_t(count), c.empty() ? 'x' : char(c[0]));
            writeAll(stream == L"err" ? STD_ERROR_HANDLE : STD_OUTPUT_HANDLE, bytes.data(), bytes.size());
        } else if (step == L"cat") {
            HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
            std::vector<char> buffer(65536); DWORD count = 0;
            while (ReadFile(input, buffer.data(), DWORD(buffer.size()), &count, nullptr) && count > 0)
                writeAll(STD_OUTPUT_HANDLE, buffer.data(), count);
        } else if (step == L"env") {
            const auto name = next();
            std::wstring value(32768, L'\0');
            const DWORD size = GetEnvironmentVariableW(name.c_str(), value.data(), DWORD(value.size()));
            value.resize(size);
            writeText(STD_OUTPUT_HANDLE, narrow(value));
        } else if (step == L"pidfile") {
            const auto path = next();
            if (FILE *file = _wfopen(path.c_str(), L"wb")) {
                std::fprintf(file, "%lu\n", GetCurrentProcessId()); std::fclose(file);
            }
        } else if (step == L"spawn") {
            std::wstring commandLine = quote(modulePath()) + L" pidfile " + quote(next()) + L" sleep 120";
            STARTUPINFOW startup{};
            startup.cb = sizeof(startup);
            PROCESS_INFORMATION info{};
            if (!CreateProcessW(modulePath().c_str(), commandLine.data(), nullptr, nullptr, FALSE, DETACHED_PROCESS,
                                nullptr, nullptr, &startup, &info)) {
                writeText(STD_ERROR_HANDLE, "cannot spawn\n");
                return 1;
            }
            CloseHandle(info.hThread);
            CloseHandle(info.hProcess);
        } else if (step == L"sleep") {
            Sleep(DWORD(_wtof(next().c_str()) * 1000));
        } else if (step == L"exit") {
            return _wtoi(next().c_str());
        } else if (step == L"crash") {
            TerminateProcess(GetCurrentProcess(), 0xC0000005);
        } else if (step == L"noecho") {
            HANDLE input = GetStdHandle(STD_INPUT_HANDLE); DWORD mode = 0;
            if (GetConsoleMode(input, &mode)) SetConsoleMode(input, mode & ~ENABLE_ECHO_INPUT);
        } else if (step == L"read") {
            line = readConsoleLine();
        } else if (step == L"got") {
            writeText(STD_OUTPUT_HANDLE, "GOT:" + line + "\n");
        } else if (step == L"size") {
            CONSOLE_SCREEN_BUFFER_INFO info{};
            if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &info))
                writeText(STD_OUTPUT_HANDLE, std::to_string(info.srWindow.Bottom - info.srWindow.Top + 1) + " "
                    + std::to_string(info.srWindow.Right - info.srWindow.Left + 1) + "\n");
        } else {
            writeText(STD_ERROR_HANDLE, "unknown step: " + narrow(step) + "\n");
            return 2;
        }
    }
    return 0;
}

} // namespace

int wmain(int argc, wchar_t **argv)
{
    const std::wstring program = modulePath();
    if (_wcsicmp(baseName(program).c_str(), L"zerus-test-helper.exe") == 0) return runSteps(argc, argv);
    return runFixture(program, argc, argv);
}
