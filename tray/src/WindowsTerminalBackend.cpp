#include "WindowsTerminalBackend.h"

#include "ConsoleHost.h"
#include "MachinePlatform.h"
#include "WindowsConsole.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>

#include <utility>

namespace {
QString escapeSubcommands(QString value)
{
    return value.replace(QLatin1Char(';'), QStringLiteral("\\;"));
}

// What Windows Terminal passes on unchanged once it re-joins its command line
// and expands environment references in it (ExpandEnvironmentStrings: any %NAME%
// pair, also across arguments, may be replaced).
bool survivesWindowsTerminal(const QString &value)
{
    if (value.isEmpty() || value.contains(QLatin1Char('"')) || value.contains(QLatin1Char('\t'))
        || value.contains(QLatin1Char('\n')) || value.contains(QStringLiteral("\\;"))
        || value.contains(QLatin1Char('%')))
        return false;
    return !(value.contains(QLatin1Char(' ')) && value.endsWith(QLatin1Char('\\')));
}

QString powerShell()
{
    const QString root = qEnvironmentVariable("SystemRoot", QStringLiteral("C:\\Windows"));
    return QDir::fromNativeSeparators(root) + QStringLiteral("/System32/WindowsPowerShell/v1.0/powershell.exe");
}
} // namespace

WindowsTerminalBackend::WindowsTerminalBackend(OpenMode mode, QString windowsTerminal, QString consoleHost)
    : m_mode(mode), m_terminal(std::move(windowsTerminal)),
      m_consoleHost(consoleHost.isEmpty() ? QCoreApplication::applicationFilePath() : std::move(consoleHost))
{
}

WindowsTerminalBackend::OpenMode WindowsTerminalBackend::modeFromString(const QString &mode)
{
    if (mode == QLatin1String("tab")) return OpenMode::Tab;
    if (mode == QLatin1String("window")) return OpenMode::Window;
    if (mode == QLatin1String("console")) return OpenMode::Console;
    return OpenMode::Auto;
}

QString WindowsTerminalBackend::locateWindowsTerminal()
{
    // wt.exe is an app execution alias under %LOCALAPPDATA%\Microsoft\WindowsApps,
    // which is normally, but not necessarily, on PATH.
    QString found = QStandardPaths::findExecutable(QStringLiteral("wt"));
    if (found.isEmpty()) {
        const QString alias = QDir::fromNativeSeparators(qEnvironmentVariable("LOCALAPPDATA"))
            + QStringLiteral("/Microsoft/WindowsApps/wt.exe");
        if (!qEnvironmentVariable("LOCALAPPDATA").isEmpty() && QFileInfo::exists(alias)) found = alias;
    }
    return found;
}

QString WindowsTerminalBackend::title(const QStringList &arguments)
{
    QString text = QStringLiteral("Zerus: ") + arguments.join(QLatin1Char(' '));
    text.replace(QLatin1Char('\n'), QLatin1Char(' '));
    return text.size() > 100 ? text.left(99) + QChar(0x2026) : text;
}

QStringList WindowsTerminalBackend::terminalArguments(OpenMode mode, const QString &title, const QString &program,
                                                      const QStringList &arguments)
{
    if (!survivesWindowsTerminal(program)) return {};
    for (const auto &argument : arguments)
        if (!survivesWindowsTerminal(argument)) return {};
    QStringList result{QStringLiteral("-w"), mode == OpenMode::Window ? QStringLiteral("new") : QStringLiteral("0"),
                       QStringLiteral("new-tab"), QStringLiteral("--title"), escapeSubcommands(title),
                       QStringLiteral("--"), escapeSubcommands(QString(program).replace(QLatin1Char('/'), QLatin1Char('\\')))};
    for (const auto &argument : arguments) result << escapeSubcommands(argument);
    return result;
}

QStringList WindowsTerminalBackend::shellArguments(OpenMode mode, const QString &directory, const QStringList &shell)
{
    const QString folder = MachinePlatform::native(directory, MachinePlatform::Style::Windows);
    if (shell.isEmpty() || !survivesWindowsTerminal(folder)) return {};
    QStringList result = terminalArguments(mode, title({folder}), shell.first(), shell.mid(1));
    if (result.isEmpty()) return {};
    // After new-tab: the tab's starting folder.
    result.insert(3, QStringLiteral("-d"));
    result.insert(4, escapeSubcommands(folder));
    return result;
}

QStringList WindowsTerminalBackend::shell(const QString &pwsh)
{
    if (!pwsh.isEmpty()) return {pwsh};
    return {powerShell(), QStringLiteral("-NoLogo")};
}

QStringList WindowsTerminalBackend::locateShell()
{
    return shell(QStandardPaths::findExecutable(QStringLiteral("pwsh")));
}

bool WindowsTerminalBackend::open(const QString &command, QString *error)
{
    // A PowerShell command line: run it in a console that stays open. No caller
    // reaches this on Windows (a terminal= template goes to CommandBackend, and
    // local folder shells use openShell).
    return startShell(command, error);
}

bool WindowsTerminalBackend::openProgram(const QString &program, const QStringList &arguments, QString *error)
{
    const QString name = title(arguments);
    if (startTerminal(terminalArguments(m_mode, name, program, arguments), QStringLiteral("these arguments"),
                      QStringLiteral("the session")))
        return true;
    return startConsoleHost(name, program, arguments, error);
}

bool WindowsTerminalBackend::openShell(const QString &directory, QString *error)
{
    if (!MachinePlatform::absolute(directory, MachinePlatform::Style::Windows)) {
        *error = QStringLiteral("not a folder of this computer: %1").arg(directory);
        return false;
    }
    const QString folder = MachinePlatform::native(directory, MachinePlatform::Style::Windows);
    const QStringList command = locateShell();
    if (startTerminal(shellArguments(m_mode, folder, command), QStringLiteral("this folder"),
                      QStringLiteral("the shell")))
        return true;
    return startConsoleHost(title({folder}), command.first(), command.mid(1), error, folder);
}

bool WindowsTerminalBackend::startTerminal(const QStringList &arguments, const QString &refused, const QString &what)
{
    if (m_mode == OpenMode::Console) return false;
    QString reason;
    if (m_terminal.isEmpty())
        reason = QStringLiteral("Windows Terminal (wt.exe) is not installed");
    else if (arguments.isEmpty())
        reason = QStringLiteral("Windows Terminal cannot pass %1 unchanged").arg(refused);
    else if (QProcess::startDetached(m_terminal, arguments, QDir::homePath()))
        return true;
    else
        reason = QStringLiteral("Windows Terminal (%1) did not start").arg(QDir::toNativeSeparators(m_terminal));
    if (m_mode != OpenMode::Auto)
        m_advisory = QStringLiteral("%1, so %2 opened in a console window. Choose Console "
                                    "in \"Open in\" to use console windows deliberately.").arg(reason, what);
    return false;
}

QString WindowsTerminalBackend::takeAdvisory()
{
    return std::exchange(m_advisory, QString());
}

bool WindowsTerminalBackend::startShell(const QString &command, QString *error)
{
#ifdef Q_OS_WIN
    QProcess process;
    process.setProgram(powerShell());
    process.setArguments({QStringLiteral("-NoLogo"), QStringLiteral("-NoProfile"), QStringLiteral("-NoExit"),
                          QStringLiteral("-Command"), command});
    process.setWorkingDirectory(QDir::homePath());
    WindowsConsole::show(process, true);
    if (process.startDetached()) return true;
    *error = QStringLiteral("cannot start %1: %2").arg(QDir::toNativeSeparators(powerShell()), process.errorString());
    return false;
#else
    Q_UNUSED(command);
    *error = QStringLiteral("console windows exist only on Windows");
    return false;
#endif
}

bool WindowsTerminalBackend::startConsoleHost(const QString &title, const QString &program,
                                              const QStringList &arguments, QString *error, const QString &directory)
{
#ifdef Q_OS_WIN
    // hgs-tray.exe --console-run opens the console itself (AllocConsole); the
    // program starts in the host's working folder.
    QProcess process;
    process.setProgram(m_consoleHost);
    process.setArguments(ConsoleHost::arguments(title, program, arguments));
    process.setWorkingDirectory(directory.isEmpty() ? QDir::homePath() : directory);
    WindowsConsole::show(process, false);
    if (process.startDetached()) return true;
    *error = QStringLiteral("cannot start %1: %2").arg(QDir::toNativeSeparators(m_consoleHost), process.errorString());
    return false;
#else
    Q_UNUSED(title); Q_UNUSED(program); Q_UNUSED(arguments); Q_UNUSED(directory);
    *error = QStringLiteral("console windows exist only on Windows");
    return false;
#endif
}
