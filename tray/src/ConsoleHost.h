#pragma once

#include <QString>
#include <QStringList>

#include <optional>

// The console window of a session on Windows when Windows Terminal is missing or
// would change an argument. hgs-tray.exe started as
//
//   hgs-tray.exe --console-run --title <title> -- <program> <arguments...>
//
// opens a console of its own and starts program with exactly these arguments
// (CreateProcess with the QProcess quoting of WindowsCommandLine.h): no shell
// parses them. When program fails, the window stays open with its exit code
// until Enter is pressed. main.cpp handles the option before anything else.
//
// The argument builders are pure and compiled on every platform for the tests.
namespace ConsoleHost {

inline QString option() { return QStringLiteral("--console-run"); }

struct Request {
    QString title;
    QString program;
    QStringList arguments;
};

// The hgs-tray arguments (after argv[0]) that run program in a console window.
QStringList arguments(const QString &title, const QString &program, const QStringList &arguments);
// The request in hgs-tray's arguments after argv[0], which start with option();
// nothing when they are malformed.
std::optional<Request> parse(const QStringList &arguments);
// The line shown when program has failed.
QString failureMessage(const QString &program, quint32 exitCode);

#ifdef Q_OS_WIN
// Runs the request (arguments after argv[0]) and returns the program's exit code.
int run(const QStringList &arguments);
#endif

} // namespace ConsoleHost
