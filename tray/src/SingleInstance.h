#pragma once

#include <QByteArray>
#include <QCryptographicHash>
#include <QString>

#include <functional>

class QLocalServer;

// The QLocalServer name a second hgs-tray uses to find the first. On Unix it is a
// socket in the user's own runtime directory. On Windows it is a named pipe in a
// namespace shared by every user of the machine, so the name carries a hash of the
// user's profile directory (case-insensitive, like Windows paths).
namespace SingleInstance {
inline QString key(const QString &userProfile = {})
{
    if (userProfile.isEmpty())
        return QStringLiteral("hgs-tray");
    const QByteArray digest = QCryptographicHash::hash(userProfile.toCaseFolded().toUtf8(),
                                                       QCryptographicHash::Sha256);
    return QStringLiteral("hgs-tray-") + QString::fromLatin1(digest.toHex().left(16));
}

// The GUI's own service, hgs-tray.service under systemd --user or
// com.hgdev.hgs-tray under launchd, as its service manager reports it. Starting
// covers a scheduled restart and a stop that may be part of one; pid is its main
// process, 0 when there is none yet.
struct Service {
    enum class State { Absent, Starting, Running };
    State state = State::Absent;
    qint64 pid = 0;
};
// `systemctl --user show --property=ActiveState,MainPID hgs-tray.service`.
Service systemdService(const QByteArray &properties);
// `launchctl print gui/<uid>/com.hgdev.hgs-tray`.
Service launchdService(const QByteArray &description);
// Asks this computer's service manager. Absent without one, when the service is
// not installed and when the manager does not answer within two seconds.
Service currentService();

// A start that finds no GUI on the socket leaves it to a service that another
// process runs or is about to run.
bool waitsForService(const Service &service, qint64 self);

// The file that serializes concurrent Unix starts, in the user's runtime directory.
QString startLockPath();

// How a Unix hgs-tray becomes the single instance or forwards its activation to it.
//
// Concurrent starts take lockPath in turn, so probing the socket, removing a stale
// one and listening are one step: two starts can no longer both remove the socket
// and leave a GUI that nothing reaches. A start that the service manager does not
// run, such as `hgs-tray --sessions` from the launcher, does not claim the socket
// while the service runs or is about to: it forwards its activation once the
// service listens. It claims the socket itself once the service has stopped, or
// when the service does not listen within serviceWaitMs. Nothing is terminated.
struct Startup {
    QString key;
    QString lockPath;
    QByteArray activation; // "sessions\n" or "show\n"
    qint64 self = 0;       // this process
    std::function<Service()> service;
    std::function<void(const QString &)> report; // one-line diagnostics
    int lockWaitMs = 10000;
    int serviceWaitMs = 30000;
    int pollMs = 200;
};
enum class Outcome { Forwarded, Claimed, Failed };
// Claimed: server listens on key. Failed: server holds the error.
Outcome start(const Startup &startup, QLocalServer &server);
} // namespace SingleInstance
