#pragma once

#include "MachinePlatform.h"
#include "WindowsCommandLine.h"

#include <QRegularExpression>
#include <QString>
#include <QStringList>

#include <algorithm>

// Terminal and clipboard actions must address the same CLI and session.
namespace SessionCommands {
// The shell a displayed or copied command is written for: POSIX sh on Unix,
// PowerShell (the Windows Terminal default) on Windows. A parameter rather than an
// #ifdef in the formatting, so both forms are tested on every platform.
enum class Shell { Posix, PowerShell };

inline Shell currentShell()
{
#ifdef Q_OS_WIN
    return Shell::PowerShell;
#else
    return Shell::Posix;
#endif
}

inline QString shellQuote(const QString &value)
{
    static const QRegularExpression safe(QStringLiteral("^[A-Za-z0-9_@%+=:,./-]+$"));
    if (!value.isEmpty() && safe.match(value).hasMatch()) return value;
    QString escaped = value;
    escaped.replace(QLatin1String("'"), QLatin1String("'\\''"));
    return QLatin1Char('\'') + escaped + QLatin1Char('\'');
}

// PowerShell literal: single quotes, with every quote PowerShell accepts as a
// single quote doubled. Bare words exclude @ (splatting), commas (arrays), $,
// backticks, parentheses and braces.
inline QString powerShellQuote(const QString &value)
{
    static const QRegularExpression safe(QStringLiteral("^[A-Za-z0-9_./\\\\:=+-]+$"));
    if (!value.isEmpty() && safe.match(value).hasMatch()) return value;
    QString escaped;
    for (const QChar c : value) {
        escaped += c;
        if (c == QLatin1Char('\'') || c == QChar(0x2018) || c == QChar(0x2019) || c == QChar(0x201a)
            || c == QChar(0x201b))
            escaped += c;
    }
    return QLatin1Char('\'') + escaped + QLatin1Char('\'');
}

// Windows PowerShell 5.1 (the Windows Terminal default without PowerShell 7)
// puts a native argument with a space in double quotes without escaping quotes
// or a final backslash in it, and drops an empty one; PowerShell 7.3 and later
// pass such arguments correctly, so no quoting suits both.
inline bool powerShellPassesUnchanged(const QString &value)
{
    if (value.isEmpty() || value.contains(QLatin1Char('"'))) return false;
    const bool spaced = value.contains(QLatin1Char(' ')) || value.contains(QLatin1Char('\t'));
    return !(spaced && value.endsWith(QLatin1Char('\\')));
}

inline QString command(const QString &hgsPath, const QStringList &arguments, Shell shell)
{
    if (shell == Shell::PowerShell) {
        QString program = hgsPath.isEmpty() ? QStringLiteral("hgs") : hgsPath;
        program.replace(QLatin1Char('/'), QLatin1Char('\\'));
        const QString quoted = powerShellQuote(program);
        // Start-Process hands the exact Windows command line to the program in
        // every PowerShell edition.
        if (!std::all_of(arguments.begin(), arguments.end(), powerShellPassesUnchanged))
            return QStringLiteral("Start-Process -FilePath %1 -ArgumentList %2 -NoNewWindow -Wait")
                .arg(quoted, powerShellQuote(WindowsCommandLine::build({}, arguments).mid(1)));
        // A quoted program needs the call operator to run.
        QString result = quoted.startsWith(QLatin1Char('\'')) ? QStringLiteral("& ") + quoted : quoted;
        for (const auto &argument : arguments) result += QLatin1Char(' ') + powerShellQuote(argument);
        return result;
    }
    QString result = shellQuote(hgsPath.isEmpty() ? QStringLiteral("hgs") : hgsPath);
    for (const auto &argument : arguments) result += QLatin1Char(' ') + shellQuote(argument);
    return result;
}

inline QString command(const QString &hgsPath, const QStringList &arguments)
{
    return command(hgsPath, arguments, currentShell());
}

// Arguments of `hgs [@host] a <name>` or of resuming an archived conversation.
inline QStringList openArguments(const QString &host, const QString &name, const QString &archiveId = {})
{
    QStringList arguments;
    if (!host.isEmpty()) arguments << QLatin1Char('@') + host;
    if (archiveId.isEmpty()) arguments << QStringLiteral("a") << name;
    else arguments << QStringLiteral("resume") << name << QStringLiteral("--archive") << archiveId;
    return arguments;
}

inline QString open(const QString &hgsPath, const QString &host, const QString &name,
                    const QString &archiveId = {}, Shell shell = currentShell())
{
    return command(hgsPath, openArguments(host, name, archiveId), shell);
}

inline QString folderShell(const QString &hgsPath, const QString &host, const QString &directory,
                           Shell shell = currentShell())
{
    // Peers are POSIX; this computer has the platform of its shell.
    const bool windows = host.isEmpty() && shell == Shell::PowerShell;
    const auto style = windows ? MachinePlatform::Style::Windows : MachinePlatform::Style::Posix;
    if (!MachinePlatform::absolute(directory, style) || directory.contains(QChar::Null)) return {};
    if (windows)
        return QStringLiteral("Set-Location -LiteralPath ") + powerShellQuote(MachinePlatform::native(directory, style));
    const auto local = QStringLiteral("cd -- ") + shellQuote(directory)
        + QStringLiteral(" && exec \"${SHELL:-/bin/sh}\" -l");
    return host.isEmpty() ? local : command(hgsPath, {"machine", "ssh", host, "--directory", directory}, shell);
}
} // namespace SessionCommands
