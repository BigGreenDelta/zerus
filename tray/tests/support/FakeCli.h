#pragma once

// Fake executables for tests that start hgs and similar programs.
//
// A fixture is a script file. On Unix the script itself is started, so its first
// line chooses the interpreter. Windows cannot start scripts: there the program is
// a copy of the test helper named <script>.exe, which runs the script with Python
// and passes the command line through unchanged (support/TestHelper.cpp). So
// fixtures that must run on Windows are Python scripts, and tests keep the script
// path for sidecar files (__file__ + ".calls") while starting program(script).
#include <QByteArray>
#include <QDeadlineTimer>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QString>
#include <QThread>

#ifndef Q_OS_WIN
#include <csignal>
#include <sys/types.h>
#endif

namespace FakeCli {

inline QString program(const QString &script)
{
#ifdef Q_OS_WIN
    return script + QStringLiteral(".exe");
#else
    return script;
#endif
}

// Never replaces an existing program: a running image cannot be overwritten on
// Windows, and the copy does not depend on the script's contents.
inline bool makeRunnable(const QString &script)
{
#ifdef Q_OS_WIN
    const QString exe = program(script);
    return QFileInfo::exists(exe) || QFile::copy(QStringLiteral(ZERUS_TEST_HELPER), exe);
#else
    return QFile::setPermissions(script, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
#endif
}

// Writes the script as raw bytes (LF line ends) and makes it runnable.
inline bool install(const QString &script, const QByteArray &contents)
{
    QFile file(script);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate) || file.write(contents) != contents.size()) return false;
    file.close();
    return makeRunnable(script);
}

#ifdef Q_OS_WIN
// support/FakeCliWindows.cpp: <windows.h> defines macros (near, small) that
// tests and libvterm use as names.
bool processAlive(qint64 pid);
void terminate(qint64 pid);
#else
inline bool processAlive(qint64 pid)
{
    return pid > 0 && ::kill(pid_t(pid), 0) == 0;
}
#endif

// Atomically replaces a data file that running fixtures read. Windows cannot
// rename over a file another process has open, even for reading, so the
// replacement is retried there while a fixture reads it.
inline bool replace(const QString &path, const QByteArray &bytes)
{
    const QDeadlineTimer deadline(5000);
    for (;;) {
        QSaveFile file(path);
        if (file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit()) return true;
#ifdef Q_OS_WIN
        if (deadline.hasExpired()) return false;
        QThread::msleep(20);
#else
        return false;
#endif
    }
}

// A path as a Python string literal, for embedding into fixture sources.
inline QByteArray pythonString(const QString &value)
{
    QByteArray result = "'";
    for (const char c : value.toUtf8()) {
        if (c == '\\' || c == '\'') result += '\\';
        result += c;
    }
    return result + "'";
}

} // namespace FakeCli
