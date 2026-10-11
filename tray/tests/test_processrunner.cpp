#include <QtTest>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QThread>

#include <atomic>
#include <memory>
#ifdef Q_OS_UNIX
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "FakeCli.h"
#include "ProcessRunner.h"

// Helper processes start on ProcessRunner's worker: fork/exec of the large GUI
// process and child reaping must never run on the GUI thread, while each
// request still reports exactly once to its owner on the owner's thread.
// Children are /bin/sh scripts on Unix and the test helper's steps on Windows
// (support/TestHelper.cpp), where cases about signals and zombies do not apply.

class TestProcessRunner : public QObject {
    Q_OBJECT
private slots:
    void cleanup() { ProcessRunner::setLaunchObserver({}); }
    void launchesOnWorkerAndReportsOnOwnerThread();
    void capturesOutputErrorAndExitCode();
    void writesInputThenClosesStdin();
    void timeoutKillsChildAndReportsOnce();
    void missingProgramFailsToStart();
    void crashReportsProcessError();
    void destroyedOwnerGetsNoResultAndKillsChild();
    void launchBackendMatchesPlatform();
    void childInheritsOnlyStandardDescriptors();
    void environmentIsInheritedOrReplaced();
    void bareProgramNamesSearchPath();
    void largeInputAndOutputFlowTogether();
    void earlyExitWithPendingInputDoesNotRaiseSigpipe();
    void childSignalsMatchQProcess();
};

namespace {
// The same child on both platforms: a /bin/sh script on Unix, helper steps on Windows.
ProcessRunner::Request child(const QString &script, const QStringList &steps, int timeoutMs = 10000)
{
    ProcessRunner::Request request;
#ifdef Q_OS_WIN
    Q_UNUSED(script);
    request.program = QStringLiteral(ZERUS_TEST_HELPER); request.arguments = steps;
#else
    Q_UNUSED(steps);
    request.program = QStringLiteral("/bin/sh"); request.arguments = {QStringLiteral("-c"), script};
#endif
    request.timeoutMs = timeoutMs; return request;
}
#ifdef Q_OS_UNIX
ProcessRunner::Request shell(const QString &script, int timeoutMs = 10000) { return child(script, {}, timeoutMs); }
#endif
bool alive(qint64 pid) { return FakeCli::processAlive(pid); }
// Reaped children are gone for waitpid; a zombie would still be returned here.
// Windows has no zombies: a process object ends with its last handle.
bool reaped(qint64 pid)
{
#ifdef Q_OS_WIN
    Q_UNUSED(pid); return true;
#else
    int status = 0; return ::waitpid(pid_t(pid), &status, WNOHANG) < 0 && errno == ECHILD;
#endif
}
ProcessRunner::Result runAndWait(const ProcessRunner::Request &request)
{
    QObject owner; int results = 0; ProcessRunner::Result result;
    ProcessRunner::run(request, &owner, [&](const ProcessRunner::Result &value) { ++results; result = value; });
    if (!QTest::qWaitFor([&] { return results == 1; }, 15000)) qWarning("no result");
    return result;
}
qint64 readPid(const QString &path)
{
    QFile file(path); if (!file.open(QIODevice::ReadOnly)) return 0;
    return file.readAll().trimmed().toLongLong();
}
}

void TestProcessRunner::launchesOnWorkerAndReportsOnOwnerThread()
{
    std::atomic<QThread *> launchThread{nullptr};
    ProcessRunner::setLaunchObserver([&launchThread] { launchThread = QThread::currentThread(); });
    QObject owner; QThread *resultThread = nullptr; int results = 0; ProcessRunner::Result result;
    ProcessRunner::run(child(QStringLiteral("printf hello"), {"out", "hello"}), &owner, [&](const ProcessRunner::Result &value) {
        ++results; resultThread = QThread::currentThread(); result = value;
    });
    QTRY_COMPARE(results, 1);
    QVERIFY(launchThread.load());
    QVERIFY(launchThread.load() != QThread::currentThread());
    QCOMPARE(launchThread.load(), ProcessRunner::workerThread());
    QCOMPARE(resultThread, QThread::currentThread());
    QCOMPARE(result.outcome, ProcessRunner::Result::Finished);
    QCOMPARE(result.exitStatus, QProcess::NormalExit);
    QCOMPARE(result.exitCode, 0);
    QCOMPARE(result.standardOutput, QByteArray("hello"));
}

void TestProcessRunner::capturesOutputErrorAndExitCode()
{
    QObject owner; int results = 0; ProcessRunner::Result result;
    // Larger than a pipe buffer: output is drained while the child runs.
    ProcessRunner::run(child(QStringLiteral("head -c 200000 /dev/zero | tr '\\0' x; printf oops >&2; exit 3"),
                             {"fill", "out", "200000", "x", "err", "oops", "exit", "3"}), &owner,
        [&](const ProcessRunner::Result &value) { ++results; result = value; });
    QTRY_COMPARE(results, 1);
    QCOMPARE(result.outcome, ProcessRunner::Result::Finished);
    QCOMPARE(result.exitCode, 3);
    QCOMPARE(result.standardOutput, QByteArray(200000, 'x'));
    QCOMPARE(result.standardError, QByteArray("oops"));
    QCOMPARE(result.error, QProcess::UnknownError);
}

void TestProcessRunner::writesInputThenClosesStdin()
{
    QObject owner; int results = 0; ProcessRunner::Result withInput, empty;
    auto request = child(QStringLiteral("cat"), {"cat"}); request.input = QByteArray("{\"key\":\"value\"}");
    ProcessRunner::run(request, &owner, [&](const ProcessRunner::Result &value) { ++results; withInput = value; });
    // An empty payload still closes stdin, so readers see EOF instead of waiting.
    request.input = QByteArray(""); request.timeoutMs = 5000;
    ProcessRunner::run(request, &owner, [&](const ProcessRunner::Result &value) { ++results; empty = value; });
    QTRY_COMPARE(results, 2);
    QCOMPARE(withInput.standardOutput, QByteArray("{\"key\":\"value\"}"));
    QCOMPARE(empty.outcome, ProcessRunner::Result::Finished);
    QVERIFY(empty.standardOutput.isEmpty());
}

void TestProcessRunner::timeoutKillsChildAndReportsOnce()
{
    QTemporaryDir dir; QVERIFY(dir.isValid()); const auto pidFile = dir.filePath("pid");
    QObject owner; int results = 0; ProcessRunner::Result result;
    ProcessRunner::run(child(QStringLiteral("echo $$ > '%1'; exec sleep 30").arg(pidFile), {"pidfile", pidFile, "sleep", "30"}, 300), &owner,
        [&](const ProcessRunner::Result &value) { ++results; result = value; });
    QTRY_VERIFY(readPid(pidFile) > 0);
    const auto pid = readPid(pidFile);
    QTRY_COMPARE_WITH_TIMEOUT(results, 1, 3000);
    QCOMPARE(result.outcome, ProcessRunner::Result::TimedOut);
    QTRY_VERIFY(!alive(pid));
    QTRY_VERIFY(reaped(pid));
    // The killed child's later exit is reaped without a second report.
    QTest::qWait(200);
    QCOMPARE(results, 1);
}

void TestProcessRunner::missingProgramFailsToStart()
{
    QObject owner; int results = 0; ProcessRunner::Result result;
    ProcessRunner::Request request; request.program = QStringLiteral("/no/such/program-for-zerus"); request.timeoutMs = 5000;
    ProcessRunner::run(request, &owner, [&](const ProcessRunner::Result &value) { ++results; result = value; });
    QTRY_COMPARE(results, 1);
    QCOMPARE(result.outcome, ProcessRunner::Result::FailedToStart);
    QCOMPARE(result.error, QProcess::FailedToStart);
    QVERIFY(!result.errorString.isEmpty());
    QTest::qWait(100);
    QCOMPARE(results, 1);
}

void TestProcessRunner::crashReportsProcessError()
{
    QObject owner; int results = 0; ProcessRunner::Result result;
    ProcessRunner::run(child(QStringLiteral("kill -9 $$"), {"crash"}), &owner, [&](const ProcessRunner::Result &value) { ++results; result = value; });
    QTRY_COMPARE(results, 1);
    QCOMPARE(result.outcome, ProcessRunner::Result::Finished);
    QCOMPARE(result.exitStatus, QProcess::CrashExit);
#ifdef Q_OS_WIN
    QCOMPARE(uint(result.exitCode), 0xC0000005u);   // the NTSTATUS, as QProcess reports it
#else
    QCOMPARE(result.exitCode, SIGKILL);   // the signal number, as QProcess reports it
#endif
    QCOMPARE(result.error, QProcess::Crashed);
    QVERIFY(!result.errorString.isEmpty());
}

void TestProcessRunner::destroyedOwnerGetsNoResultAndKillsChild()
{
    QTemporaryDir dir; QVERIFY(dir.isValid()); const auto pidFile = dir.filePath("pid");
    auto owner = std::make_unique<QObject>(); int results = 0;
    ProcessRunner::run(child(QStringLiteral("echo $$ > '%1'; exec sleep 30").arg(pidFile), {"pidfile", pidFile, "sleep", "30"}), owner.get(),
        [&](const ProcessRunner::Result &) { ++results; });
    QTRY_VERIFY(readPid(pidFile) > 0);
    const auto pid = readPid(pidFile);
    owner.reset();
    // Destroying the owner kills its child before returning, like the old
    // HgsClient destructor, so temporary fixtures are not used afterwards.
    QVERIFY(!alive(pid));
    QVERIFY(reaped(pid));
    QTest::qWait(200);
    QCOMPARE(results, 0);
}

void TestProcessRunner::launchBackendMatchesPlatform()
{
#ifdef Q_OS_MACOS
    // QProcess forks the whole GUI address space on macOS; posix_spawn does not.
    QCOMPARE(QByteArray(ProcessRunner::launchBackend()), QByteArray("posix_spawn"));
#else
    QCOMPARE(QByteArray(ProcessRunner::launchBackend()), QByteArray("QProcess"));
#endif
}

void TestProcessRunner::childInheritsOnlyStandardDescriptors()
{
#ifndef Q_OS_MACOS
    QSKIP("Only the macOS posix_spawn backend closes inherited descriptors");
#else
    // A descriptor opened without O_CLOEXEC elsewhere in the GUI must not leak.
    const int leaked = ::open("/dev/null", O_RDONLY); QVERIFY(leaked > 2);
    const auto result = runAndWait(shell(QStringLiteral("for fd in 0 1 2 %1; do [ -e /dev/fd/$fd ] && printf '%s ' $fd; done; :").arg(leaked)));
    ::close(leaked);
    QCOMPARE(result.outcome, ProcessRunner::Result::Finished);
    QCOMPARE(result.standardOutput, QByteArray("0 1 2 "));
#endif
}

void TestProcessRunner::environmentIsInheritedOrReplaced()
{
    qputenv("ZERUS_RUNNER_INHERITED", "inherited");
    const auto script = QStringLiteral("printf '%s|%s' \"$ZERUS_RUNNER_INHERITED\" \"$ZERUS_RUNNER_SET\"");
    const QStringList steps{"env", "ZERUS_RUNNER_INHERITED", "out", "|", "env", "ZERUS_RUNNER_SET"};
    QCOMPARE(runAndWait(child(script, steps)).standardOutput, QByteArray("inherited|"));
    auto request = child(script, steps);
    QProcessEnvironment environment; environment.insert("ZERUS_RUNNER_SET", QString::fromUtf8("значение"));
    request.environment = environment;
    // A given environment replaces the inherited one; values keep their UTF-8 bytes.
    QCOMPARE(runAndWait(request).standardOutput, QString::fromUtf8("|значение").toUtf8());
    qunsetenv("ZERUS_RUNNER_INHERITED");
}

void TestProcessRunner::bareProgramNamesSearchPath()
{
    ProcessRunner::Request request; request.program = QStringLiteral("sh");
    request.arguments = {QStringLiteral("-c"), QStringLiteral("printf found")}; request.timeoutMs = 10000;
#ifdef Q_OS_WIN
    // The helper's directory on PATH; the name is completed with .exe.
    const QByteArray path = qgetenv("PATH");
    qputenv("PATH", QDir::toNativeSeparators(QFileInfo(QStringLiteral(ZERUS_TEST_HELPER)).absolutePath()).toLocal8Bit()
        + ';' + path);
    request.program = QStringLiteral("zerus-test-helper"); request.arguments = {"out", "found"};
    const auto found = runAndWait(request);
    qputenv("PATH", path);
#else
    const auto found = runAndWait(request);
#endif
    QCOMPARE(found.outcome, ProcessRunner::Result::Finished);
    QCOMPARE(found.standardOutput, QByteArray("found"));
    request.program = QStringLiteral("no-such-program-for-zerus");
    QCOMPARE(runAndWait(request).outcome, ProcessRunner::Result::FailedToStart);
}

void TestProcessRunner::largeInputAndOutputFlowTogether()
{
    // cat echoes while it reads: neither side may block on a full pipe.
    QByteArray payload(1 << 20, '\0');
    for (int i = 0; i < payload.size(); ++i) payload[i] = char('a' + i % 26);
    auto request = child(QStringLiteral("cat; head -c 300000 /dev/zero | tr '\\0' e >&2"), {"cat", "fill", "err", "300000", "e"}, 20000);
    request.input = payload;
    const auto result = runAndWait(request);
    QCOMPARE(result.outcome, ProcessRunner::Result::Finished);
    QCOMPARE(result.exitCode, 0);
    QCOMPARE(result.standardOutput.size(), payload.size());
    QVERIFY(result.standardOutput == payload);
    QCOMPARE(result.standardError, QByteArray(300000, 'e'));
}

void TestProcessRunner::earlyExitWithPendingInputDoesNotRaiseSigpipe()
{
    // The child exits without reading: writing the rest hits a closed pipe,
    // which must fail quietly instead of killing this process with SIGPIPE.
    auto request = child(QStringLiteral("exit 4"), {"exit", "4"}); request.input = QByteArray(8 << 20, 'x');
    const auto result = runAndWait(request);
    QCOMPARE(result.outcome, ProcessRunner::Result::Finished);
    QCOMPARE(result.exitCode, 4);
}

void TestProcessRunner::childSignalsMatchQProcess()
{
#ifdef Q_OS_WIN
    QSKIP("Windows children have no signals");
#else
    // QProcess restores SIGPIPE's default action in the child and leaves other
    // ignored signals ignored; both backends must start children the same way.
    const auto pipe = ::signal(SIGPIPE, SIG_IGN), user = ::signal(SIGUSR1, SIG_IGN);
    const auto piped = runAndWait(shell(QStringLiteral("kill -PIPE $$; printf survived")));
    const auto ignored = runAndWait(shell(QStringLiteral("kill -USR1 $$; printf survived")));
    ::signal(SIGPIPE, pipe); ::signal(SIGUSR1, user);
    QCOMPARE(piped.exitStatus, QProcess::CrashExit);
    QCOMPARE(piped.exitCode, SIGPIPE);
    QCOMPARE(ignored.exitStatus, QProcess::NormalExit);
    QCOMPARE(ignored.standardOutput, QByteArray("survived"));
#endif
}

QTEST_GUILESS_MAIN(TestProcessRunner)
#include "test_processrunner.moc"
