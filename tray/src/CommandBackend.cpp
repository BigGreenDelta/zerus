#include "CommandBackend.h"

#include "WindowsConsole.h"

#include <QDir>
#include <QProcess>

namespace {
const QLatin1String kPlaceholder("{cmd}");
}

CommandBackend::CommandBackend(QString terminalTemplate)
    : m_template(std::move(terminalTemplate))
{
}

QStringList CommandBackend::buildArgv(const QString &terminalTemplate, const QString &command,
                                      QString *error)
{
    if (terminalTemplate.trimmed().isEmpty()) {
        *error = QStringLiteral(
            "no terminal template: set terminal=<template with {cmd}> in "
            "~/.config/hgs/tray.conf or pass --terminal");
        return {};
    }

    // splitCommand() режет по пробелам, уважая кавычки, -- ровно та же грамматика,
    // которую пользователь ожидает от строчки в конфиге.
    QStringList argv = QProcess::splitCommand(terminalTemplate);
    if (argv.isEmpty()) {
        *error = QStringLiteral("terminal template '%1' does not parse into a command")
                     .arg(terminalTemplate);
        return {};
    }

    bool substituted = false;
    for (QString &arg : argv) {
        if (arg.contains(kPlaceholder)) {
            arg.replace(kPlaceholder, command);
            substituted = true;
        }
    }
    if (!substituted) {
        // Без {cmd} шаблон запустил бы пустой терминал и молча потерял attach --
        // это опечатка в конфиге, а не «терминал по умолчанию».
        *error = QStringLiteral("terminal template '%1' has no {cmd} -- nowhere to put "
                                 "the command").arg(terminalTemplate);
        return {};
    }
    return argv;
}

QString CommandBackend::systemTerminalTemplate(
    const std::function<bool(const QString &)> &isInstalled)
{
    // bash -lc for the same reason as KonsoleBackend::launchArgs: a new terminal window
    // starts without the login profile that puts ~/.local/bin (hgs) on PATH.
    // xdg-terminal-exec (freedesktop default-terminal spec) honours the user's choice;
    // x-terminal-emulator is the Debian/Ubuntu alternative with xterm's -e semantics.
    if (isInstalled(QStringLiteral("xdg-terminal-exec")))
        return QStringLiteral("xdg-terminal-exec bash -lc {cmd}");
    if (isInstalled(QStringLiteral("x-terminal-emulator")))
        return QStringLiteral("x-terminal-emulator -e bash -lc {cmd}");
    return QString();
}

void CommandBackend::prepare(QProcess &process, QStringList argv)
{
    process.setProgram(argv.takeFirst());
    process.setArguments(argv);
    process.setWorkingDirectory(QDir::homePath());
#ifdef Q_OS_WIN
    // A template names a console program or a GUI one (wt.exe), which ignores
    // the new console.
    WindowsConsole::show(process, true);
#endif
}

bool CommandBackend::open(const QString &command, QString *error)
{
    const QStringList argv = buildArgv(m_template, command, error);
    if (argv.isEmpty())
        return false;

    // Детач обязателен: терминал переживает трей, а не висит его дочерним процессом
    // (иначе «Quit» из меню убивал бы открытые вкладки).
    // Outside Windows this is exactly the static QProcess::startDetached(program,
    // arguments, QDir::homePath()), which Qt implements this way.
    QProcess process;
    prepare(process, argv);
    if (!process.startDetached()) {
        *error = QStringLiteral("cannot start '%1'").arg(process.program());
        return false;
    }
    return true;
}
