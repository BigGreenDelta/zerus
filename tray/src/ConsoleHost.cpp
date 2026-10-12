#include "ConsoleHost.h"

#include <algorithm>

#ifdef Q_OS_WIN
#include "WindowsCommandLine.h"

#include <QDir>
#include <QFileInfo>
#include <qt_windows.h>

#include <iterator>
#include <string>
#include <vector>
#endif

namespace ConsoleHost {

QStringList arguments(const QString &title, const QString &program, const QStringList &arguments)
{
    return QStringList{option(), QStringLiteral("--title"), title, QStringLiteral("--"), program} + arguments;
}

std::optional<Request> parse(const QStringList &arguments)
{
    if (arguments.size() < 5 || arguments.at(0) != option() || arguments.at(1) != QLatin1String("--title")
        || arguments.at(3) != QLatin1String("--") || arguments.at(4).isEmpty())
        return std::nullopt;
    return Request{arguments.at(2), arguments.at(4), arguments.mid(5)};
}

QString failureMessage(const QString &program, quint32 exitCode)
{
    QString name = program.mid(std::max(program.lastIndexOf(QLatin1Char('/')), program.lastIndexOf(QLatin1Char('\\'))) + 1);
    if (name.endsWith(QLatin1String(".exe"), Qt::CaseInsensitive))
        name.chop(4);
    // Status codes such as 0xC000013A (console closed) read better in hex.
    const QString code = exitCode > 0xffff ? QStringLiteral("0x") + QString::number(exitCode, 16).toUpper()
                                           : QString::number(exitCode);
    return QStringLiteral("%1 exited with code %2. Press Enter to close this window.")
        .arg(name.isEmpty() ? program : name, code);
}

#ifdef Q_OS_WIN
namespace {
// Ctrl+C and Ctrl+Break belong to the program, which shares this console; closing
// the window ends both.
BOOL WINAPI keepWaiting(DWORD event)
{
    return event == CTRL_C_EVENT || event == CTRL_BREAK_EVENT;
}

QString systemError(DWORD error)
{
    wchar_t *text = nullptr;
    const DWORD length = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM
                                            | FORMAT_MESSAGE_IGNORE_INSERTS,
                                        nullptr, error, MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US),
                                        reinterpret_cast<wchar_t *>(&text), 0, nullptr);
    const QString message = length ? QString::fromWCharArray(text, int(length)).trimmed()
                                   : QStringLiteral("error %1").arg(error);
    if (text)
        LocalFree(text);
    return message;
}

void write(HANDLE output, const QString &text)
{
    const std::wstring wide = text.toStdWString();
    DWORD written = 0;
    WriteConsoleW(output, wide.data(), DWORD(wide.size()), &written, nullptr);
}

void waitForEnter(HANDLE input)
{
    // The program may have left the console in raw mode.
    SetConsoleMode(input, ENABLE_PROCESSED_INPUT | ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT);
    FlushConsoleInputBuffer(input);
    wchar_t buffer[256];
    for (;;) {
        DWORD count = 0;
        if (!ReadConsoleW(input, buffer, DWORD(std::size(buffer)), &count, nullptr)) {
            // Ctrl+C ends a read without ending the wait.
            if (GetLastError() == ERROR_OPERATION_ABORTED)
                continue;
            return;
        }
        if (std::any_of(buffer, buffer + count, [](wchar_t c) { return c == L'\r' || c == L'\n'; }))
            return;
        if (count == 0)
            Sleep(10);
    }
}

HANDLE openConsole(const wchar_t *name)
{
    // Inheritable: the program receives exactly these as its standard handles.
    SECURITY_ATTRIBUTES inheritable{sizeof(inheritable), nullptr, TRUE};
    return CreateFileW(name, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &inheritable,
                       OPEN_EXISTING, 0, nullptr);
}
} // namespace

int run(const QStringList &arguments)
{
    const auto request = parse(arguments);
    if (!request)
        return 2;
    // hgs-tray is a GUI program without a console; this one is the window. It
    // fails only when a console is attached already, which serves as well.
    AllocConsole();
    SetConsoleTitleW(reinterpret_cast<const wchar_t *>(request->title.utf16()));
    SetConsoleCtrlHandler(keepWaiting, TRUE);
    const HANDLE input = openConsole(L"CONIN$"), output = openConsole(L"CONOUT$");
    if (input == INVALID_HANDLE_VALUE || output == INVALID_HANDLE_VALUE)
        return 1;

    // Only the console handles are inherited, whatever else this process holds.
    SIZE_T size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
    std::vector<char> storage(size);
    auto *attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
    HANDLE inherited[] = {input, output};
    const bool listed = InitializeProcThreadAttributeList(attributes, 1, 0, &size);
    const bool limited = listed
        && UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited, sizeof(inherited),
                                     nullptr, nullptr);
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = input;
    startup.StartupInfo.hStdOutput = output;
    startup.StartupInfo.hStdError = output;
    startup.lpAttributeList = limited ? attributes : nullptr;

    // The argument vector reaches the program exactly as QProcess would pass it:
    // nothing between this process and the program parses or expands it.
    const QString program = QDir::toNativeSeparators(request->program);
    const std::wstring application = program.toStdWString();
    std::wstring commandLine = WindowsCommandLine::build(request->program, request->arguments).toStdWString();
    PROCESS_INFORMATION info{};
    const BOOL started = CreateProcessW(QFileInfo(request->program).isAbsolute() ? application.c_str() : nullptr,
                                        commandLine.data(), nullptr, nullptr, TRUE,
                                        limited ? EXTENDED_STARTUPINFO_PRESENT : 0, nullptr, nullptr,
                                        &startup.StartupInfo, &info);
    const DWORD startError = GetLastError();
    if (listed)
        DeleteProcThreadAttributeList(attributes);
    if (!started) {
        write(output, QStringLiteral("Cannot start %1: %2\r\n\r\nPress Enter to close this window.")
                          .arg(program, systemError(startError)));
        waitForEnter(input);
        return 127;
    }
    CloseHandle(info.hThread);
    WaitForSingleObject(info.hProcess, INFINITE);
    DWORD exitCode = 1;
    GetExitCodeProcess(info.hProcess, &exitCode);
    CloseHandle(info.hProcess);
    if (exitCode != 0) {
        write(output, QStringLiteral("\r\n") + failureMessage(request->program, quint32(exitCode)));
        waitForEnter(input);
    }
    return int(exitCode);
}
#endif

} // namespace ConsoleHost
