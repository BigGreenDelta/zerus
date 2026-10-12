// Windows test helper, built only on Windows (see tests/CMakeLists.txt). Two modes:
//
// Shim: a copy named after a fixture (for example <dir>/hgs.exe next to the
// Python script <dir>/hgs) runs that script with the Python found at configure
// time. The rest of this process's command line is passed through untouched,
// so the fixture sees exactly the argv QProcess or a pseudoconsole produced, as
// hgs.exe would. Python is told to use UTF-8 and LF line ends, like the Unix
// fixtures. Shim and child run in a kill-on-close job: killing the shim, as
// QProcess::kill() does, ends the fixture as well.
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

int runFixture(const std::wstring &program)
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
    return runFixture(program);
}
