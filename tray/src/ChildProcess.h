#pragma once
#include <QProcess>
#include <QString>
#include <QStringList>

// Descriptors a QProcess child of the GUI inherits on Unix. Qt forks without
// closing the parent's descriptors, so a child gets every one not yet marked
// close-on-exec. On macOS, which has no pipe2(), ProcessRunner's worker creates
// each request's pipes with pipe() and marks them only afterwards; a child the
// GUI thread starts in between would inherit the write end of that request's
// stdin, and the hgs reading it would wait for end of input until that unrelated
// child exits, which a terminal may never do. On Linux every pipe Qt makes is
// close-on-exec at once, but a runner child forked while the GUI thread runs
// forkpty() for a terminal view would still take both ends of that PTY along
// (glibc marks neither). Every QProcess the GUI starts on Unix, directly or as
// a runner child, therefore goes through inheritStdioOnly() right before start()
// or startDetached(); the runner's posix_spawn children on macOS get the same
// from POSIX_SPAWN_CLOEXEC_DEFAULT.
namespace ChildProcess {

// Whether this Qt build limits children to stdio (tests).
#if defined(Q_OS_UNIX) && QT_VERSION >= QT_VERSION_CHECK(6, 8, 3)
constexpr bool inheritsStdioOnly = true;
#else
constexpr bool inheritsStdioOnly = false;
#endif

// The child keeps stdin, stdout and stderr; Qt marks every higher descriptor
// close-on-exec in the child only, after setting up its channels, and its own
// start report still tells a failed exec apart. Flags set before are kept.
// Qt 6.6 to 6.8.2 closed that report as well, so a failed exec would pass for a
// start there; those builds, like older ones and Windows, leave this to Qt.
inline void inheritStdioOnly(QProcess &process)
{
#if defined(Q_OS_UNIX) && QT_VERSION >= QT_VERSION_CHECK(6, 8, 3)
    auto parameters = process.unixProcessParameters();
    parameters.flags |= QProcess::UnixProcessFlag::CloseFileDescriptors;
    process.setUnixProcessParameters(parameters);
#else
    Q_UNUSED(process);
#endif
}

// The static QProcess::startDetached(program, arguments, workingDirectory), built
// the way Qt implements it, with only stdio inherited.
inline bool startDetached(const QString &program, const QStringList &arguments, const QString &workingDirectory = {})
{
    QProcess process;
    process.setProgram(program);
    process.setArguments(arguments);
    process.setWorkingDirectory(workingDirectory);
    inheritStdioOnly(process);
    return process.startDetached();
}
}
