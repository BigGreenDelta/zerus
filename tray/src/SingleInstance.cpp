#include "SingleInstance.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QLocalServer>
#include <QLocalSocket>
#include <QLockFile>
#include <QProcess>
#include <QStandardPaths>
#include <QStringList>
#include <QThread>

#ifdef Q_OS_MACOS
#include <unistd.h>
#endif

namespace SingleInstance {
namespace {
// Hands the activation to a GUI that answers on key.
bool forward(const QString &key, const QByteArray &activation)
{
    QLocalSocket probe;
    probe.connectToServer(key);
    if (!probe.waitForConnected(200))
        return false;
    probe.write(activation);
    probe.waitForBytesWritten(200);
    return true;
}

// QLockFile::tryLock with a timeout backs off to seconds between attempts; the
// lock is held only around a probe, so poll it instead. A start that crashed while
// holding it leaves a stale file, which tryLock removes once its process is gone.
bool takeLock(QLockFile &lock, int waitMs)
{
    QElapsedTimer waited;
    waited.start();
    while (!lock.tryLock(0)) {
        if (lock.error() != QLockFile::LockFailedError || waited.elapsed() >= waitMs)
            return false;
        QThread::msleep(20);
    }
    return true;
}
} // namespace

Service systemdService(const QByteArray &properties)
{
    QByteArray active;
    qint64 pid = 0;
    for (const QByteArray &line : properties.split('\n')) {
        const int equals = line.indexOf('=');
        if (equals < 0)
            continue;
        const QByteArray name = line.left(equals).trimmed(), value = line.mid(equals + 1).trimmed();
        if (name == "ActiveState")
            active = value;
        else if (name == "MainPID")
            pid = value.toLongLong();
    }
    Service service;
    if (active == "active" || active == "reloading" || active == "refreshing")
        service.state = Service::State::Running;
    else if (active == "activating" || active == "deactivating")
        service.state = Service::State::Starting;
    else
        return {};
    service.pid = pid > 0 ? pid : 0;
    return service;
}

Service launchdService(const QByteArray &description)
{
    // Only the job's own properties, one level inside its braces.
    QByteArray state;
    qint64 pid = 0;
    int depth = 0;
    for (const QByteArray &raw : description.split('\n')) {
        const QByteArray line = raw.trimmed();
        if (line == "}") {
            --depth;
            continue;
        }
        if (depth == 1) {
            const int equals = line.indexOf(" = ");
            if (equals > 0 && line.left(equals) == "state")
                state = line.mid(equals + 3);
            else if (equals > 0 && line.left(equals) == "pid")
                pid = line.mid(equals + 3).toLongLong();
        }
        if (line.endsWith('{'))
            ++depth;
    }
    Service service;
    if (state == "running")
        service.state = Service::State::Running;
    else if (state == "spawn scheduled")
        service.state = Service::State::Starting;
    else
        return {};
    service.pid = pid > 0 ? pid : 0;
    return service;
}

Service currentService()
{
#if defined(Q_OS_MACOS)
    const QString program = QStringLiteral("launchctl");
    const QStringList arguments{QStringLiteral("print"),
                                QStringLiteral("gui/%1/com.hgdev.hgs-tray").arg(::getuid())};
    Service (*const parse)(const QByteArray &) = launchdService;
#elif defined(Q_OS_UNIX)
    const QString program = QStringLiteral("systemctl");
    const QStringList arguments{QStringLiteral("--user"), QStringLiteral("show"),
                                QStringLiteral("--property=ActiveState,MainPID"), QStringLiteral("hgs-tray.service")};
    Service (*const parse)(const QByteArray &) = systemdService;
#else
    return {};
#endif
#ifdef Q_OS_UNIX
    QProcess process;
    process.setStandardInputFile(QProcess::nullDevice());
    process.start(program, arguments);
    if (!process.waitForFinished(2000)) {
        process.kill();
        process.waitForFinished(1000);
        return {};
    }
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0)
        return {};
    return parse(process.readAllStandardOutput());
#endif
}

bool waitsForService(const Service &service, qint64 self)
{
    return service.state != Service::State::Absent && service.pid != self;
}

QString startLockPath()
{
    return QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation) + QStringLiteral("/hgs-tray-start.lock");
}

Outcome start(const Startup &startup, QLocalServer &server)
{
    const auto report = [&startup](const QString &text) {
        if (startup.report)
            startup.report(text);
    };
    QDir().mkpath(QFileInfo(startup.lockPath).absolutePath());
    QElapsedTimer started;
    started.start();
    // A lock that cannot be taken once is not waited for again while polling.
    bool lockable = true, waitReported = false;
    for (;;) {
        QLockFile lock(startup.lockPath);
        if (lockable && !takeLock(lock, startup.lockWaitMs)) {
            lockable = false;
            report(lock.error() == QLockFile::LockFailedError
                       ? QStringLiteral("another start keeps %1 locked; starting without it").arg(startup.lockPath)
                       : QStringLiteral("cannot create %1; concurrent starts are not serialized")
                             .arg(startup.lockPath));
        }
        if (forward(startup.key, startup.activation))
            return Outcome::Forwarded;
        const Service service = startup.service ? startup.service() : Service();
        if (waitsForService(service, startup.self)) {
            if (started.elapsed() < startup.serviceWaitMs) {
                if (!waitReported) {
                    waitReported = true;
                    report(service.pid > 0
                               ? QStringLiteral("waiting for the Zerus service (pid %1) to open its socket")
                                     .arg(service.pid)
                               : QStringLiteral("waiting for the Zerus service to start"));
                }
                lock.unlock();
                QThread::msleep(startup.pollMs);
                continue;
            }
            report(QStringLiteral("the Zerus service did not open its socket within %1 s; starting here")
                       .arg(startup.serviceWaitMs / 1000.0));
        }
        // Nothing answers: the socket, if any, is a crashed GUI's.
        QLocalServer::removeServer(startup.key);
        return server.listen(startup.key) ? Outcome::Claimed : Outcome::Failed;
    }
}
} // namespace SingleInstance
