#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFileInfo>
#include <QIcon>
#include <QLocalServer>
#include <QJsonDocument>
#include <QLocalSocket>
#include <QSettings>
#include <QStringList>
#include <QSystemTrayIcon>
#include <QTextStream>
#include <QTimer>

#include "AppConfig.h"
#include "UpdateController.h"
#include "HgsClient.h"
#include "MainThreadWatchdog.h"
#include "TrayAgent.h"
#include "WorkspaceStyle.h"
#include "WorkspaceFocus.h"
#include "SingleInstance.h"
#ifndef Q_OS_WIN
#include "TerminationSignals.h"
#endif
#ifdef Q_OS_MACOS
#include "MacAppVisibility.h"
#include "MessageBoxIcons.h"
#include "speech/SpeechRecorder.h"
#endif

#include <cstdio>
#include <cstdlib>

#ifdef Q_OS_WIN
#include "ConsoleHost.h"
#include "StartupNotice.h"
#include "WindowsCommandLine.h"
#include <QCryptographicHash>
#include <QFile>
#include <QJsonObject>
#include <QLockFile>
#include <QStandardPaths>
#include <qt_windows.h>
#include <shellapi.h>
#include <string>
#endif

#ifndef HGS_TRAY_VERSION
#define HGS_TRAY_VERSION "0.0.0"
#endif

#ifdef Q_OS_WIN
namespace {
// The command line as UTF-16: argv is in the ANSI code page, which cannot hold
// every user name or path.
QStringList wideArguments()
{
    QStringList arguments;
    int count = 0;
    if (LPWSTR *values = CommandLineToArgvW(GetCommandLineW(), &count)) {
        for (int i = 0; i < count; ++i)
            arguments << QString::fromWCharArray(values[i]);
        LocalFree(values);
    }
    return arguments;
}

// hgs-tray is a GUI program: started from a console, its standard streams lead
// nowhere. Text answers (--help, --version, --selftest, errors) go to the parent
// console then; redirected streams already work and are kept.
void attachParentConsole()
{
    const auto valid = [](DWORD which) {
        const HANDLE handle = GetStdHandle(which);
        return handle && handle != INVALID_HANDLE_VALUE;
    };
    const bool output = valid(STD_OUTPUT_HANDLE), error = valid(STD_ERROR_HANDLE);
    if ((output && error) || !AttachConsole(ATTACH_PARENT_PROCESS))
        return;
    if (!output)
        std::freopen("CONOUT$", "w", stdout);
    if (!error)
        std::freopen("CONOUT$", "w", stderr);
    // The text is UTF-8; the console shows it in its own code page, which is
    // restored for the shell once hgs-tray exits.
    static const UINT previous = GetConsoleOutputCP();
    if (previous != CP_UTF8 && SetConsoleOutputCP(CP_UTF8))
        std::atexit([] {
            std::fflush(stdout);
            std::fflush(stderr);
            SetConsoleOutputCP(previous);
        });
}

// `hgs session-host <action> --json`, before the tray starts; empty when it fails.
QJsonObject sessionHost(const QString &hgs, const QString &action, StartupNotice &notice)
{
    HgsClient client(hgs);
    QJsonObject answer;
    QEventLoop loop;
    QObject::connect(&client, &HgsClient::sessionHostReady, &loop, [&](quint64, const QJsonObject &data) {
        answer = data;
        loop.quit();
    });
    QObject::connect(&client, &HgsClient::sessionHostFailed, &loop, [&](quint64, const QString &error) {
        QTextStream(stderr) << QStringLiteral("hgs-tray: hgs session-host %1: %2\n").arg(action, error);
        loop.quit();
    });
    notice.step(action);
    client.requestSessionHost(action);
    loop.exec();
    return answer;
}

// The hgs this tray runs until it restarts (spec D2, AppConfig::chooseHgs): on a
// computer with local sessions the stable CLI, staged again from hgs (the bundled
// build) first when that differs and is at least as new.
QString stableHgs(const QString &hgs, const QString &configured)
{
    // Shown while a slow check runs, as there is no tray icon yet.
    StartupNotice notice;
    const QJsonObject status =
        configured.isEmpty() ? sessionHost(hgs, QStringLiteral("status"), notice) : QJsonObject();
    QString sha256;
    // Only an enabled computer compares builds.
    QFile file(hgs);
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (status.value(QStringLiteral("enabled")).toBool() && file.open(QIODevice::ReadOnly) && hash.addData(&file))
        sha256 = QString::fromLatin1(hash.result().toHex());
    const AppConfig::HgsChoice choice = AppConfig::chooseHgs(status, hgs, sha256, configured);
    if (choice.refresh && sessionHost(hgs, QStringLiteral("install"), notice).isEmpty())
        return choice.fallback;
    return choice.path;
}
} // namespace
#endif

class ScrollbarTheme : public QObject {
public:
    explicit ScrollbarTheme(QApplication &app) : QObject(&app), m_app(app) {
        app.installEventFilter(this); update();
    }
protected:
    bool eventFilter(QObject *object, QEvent *event) override {
        if (object == &m_app && event->type() == QEvent::ApplicationPaletteChange)
            QTimer::singleShot(0, this, [this] { update(); });
        return false;
    }
private:
    void update() {
        const auto style = workspaceScrollbars(m_app.palette().color(QPalette::Window).lightness() < 128);
        if (m_app.styleSheet() != style) m_app.setStyleSheet(style);
    }
    QApplication &m_app;
};

// Порог сторожа главного потока (см. MainThreadWatchdog.h). Полминуты -- на порядок
// больше любой честной паузы цикла событий (модальные диалоги его не останавливают) и
// на порядки меньше недели, которую трей однажды простоял повисшим в AppKit.
static const int kMainThreadHangMs = 30000;

int main(int argc, char *argv[])
{
#ifdef Q_OS_WIN
    const QStringList args = wideArguments();
    // A console window for one hgs.exe command (WindowsTerminalBackend): no
    // settings, no single instance and no GUI.
    if (args.size() > 1 && args.at(1) == ConsoleHost::option())
        return ConsoleHost::run(args.mid(1));
    // Before any QSettings: the native format is the registry, which gives
    // ComposerDraftStore no directory. The INI file lives under %APPDATA%.
    QSettings::setDefaultFormat(QSettings::IniFormat);
#else
    QStringList args;
    args.reserve(argc);
    for (int i = 0; i < argc; ++i)
        args << QString::fromLocal8Bit(argv[i]);
#endif

    AppConfig cfg;
    QString parseError;
    const bool parsed = AppConfig::load(args, &cfg, &parseError);
#ifdef Q_OS_WIN
    if (!parsed || cfg.selfTest)
        attachParentConsole();
#endif
    QTextStream out(stdout), errOut(stderr);
    if (!parsed) {
        if (parseError == QLatin1String("__help__")) { out << AppConfig::usage(); return 0; }
        if (parseError == QLatin1String("__version__")) {
            QCoreApplication app(argc, argv);
            QCoreApplication::setApplicationVersion(QLatin1String(HGS_TRAY_VERSION));
            out << QStringLiteral("Zerus %1\n").arg(UpdateController::buildInfo()["versionName"].toString());
            return 0;
        }
        if (parseError == QLatin1String("__build_info__")) {
            QCoreApplication app(argc, argv);
            QCoreApplication::setApplicationVersion(QLatin1String(HGS_TRAY_VERSION));
            out << QJsonDocument(UpdateController::buildInfo()).toJson(QJsonDocument::Compact) << '\n';
            return 0;
        }
        errOut << QStringLiteral("hgs-tray: %1\n\n").arg(parseError) << AppConfig::usage();
        return 2;
    }
#ifndef Q_OS_WIN
    // Windows resolves hgs.exe next to hgs-tray.exe first (resolveHgsPath below).
    if (cfg.hgsPath.isEmpty())
        cfg.hgsPath = AppConfig::bundledHgsPath(QFileInfo(args.value(0)).absoluteFilePath(),
                                                AppConfig::currentPlatform());
    if (cfg.hgsPath.isEmpty())
        cfg.hgsPath = AppConfig::fallbackHgsPath(qEnvironmentVariable("PATH"),
                                                 AppConfig::hgsInstallDirs(QDir::homePath()));
#endif

    if (cfg.selfTest) {
        QCoreApplication app(argc, argv);
#ifdef Q_OS_MACOS
        // Package validation checks registration only: never prompt for consent
        // or open audio hardware from a self-test/startup diagnostic.
        if (!SpeechRecorder::microphonePermissionBackendAvailable()) {
            errOut << "hgs-tray: microphone permission backend is missing; update or reinstall Zerus.\n";
            return 1;
        }
#endif
#ifdef Q_OS_WIN
        cfg.hgsPath = AppConfig::resolveHgsPath(cfg.hgsPath, QCoreApplication::applicationDirPath());
#endif
        HgsClient client(cfg.hgsPath);
        QTextStream o(stdout);
        int rc = 0;
        QEventLoop loop;
        QObject::connect(&client, &HgsClient::localReady, [&](const BoxState &b) {
            o << QStringLiteral("hgs ok: host=%1 sessions=%2 projects=%3\n")
                     .arg(b.host).arg(b.sessions.size()).arg(b.projects.size());
            loop.quit();
        });
        QObject::connect(&client, &HgsClient::failed, [&](const QString &w, const QString &d) {
            QTextStream(stderr) << QStringLiteral("hgs FAILED: %1: %2\n").arg(w, d);
            rc = 1;
            loop.quit();
        });
        client.requestLocal();
        loop.exec();
        return rc;
    }

    // Qt WebEngine can only start from the optional native UI plugin, loaded
    // long after this point, when the attribute was set before QApplication.
    QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
    QApplication app(argc, argv);
#ifdef Q_OS_WIN
    // One hgs.exe for every operation: bundled next to hgs-tray.exe, else PATH;
    // the stable CLI once local sessions are enabled (stableHgs below).
    const bool hgsConfigured = !cfg.hgsPath.isEmpty();
    cfg.hgsPath = AppConfig::resolveHgsPath(cfg.hgsPath, QCoreApplication::applicationDirPath());
#endif
    ScrollbarTheme scrollbarTheme(app);
    WorkspaceFocus::install();
#ifdef Q_OS_MACOS
    MessageBoxIcons::install();
#endif
    QCoreApplication::setOrganizationName(QStringLiteral("hgdev"));
    QCoreApplication::setApplicationName(QStringLiteral("hgs-tray"));
    QCoreApplication::setApplicationVersion(QLatin1String(HGS_TRAY_VERSION));
    QApplication::setQuitOnLastWindowClosed(false);
    QGuiApplication::setDesktopFileName(QStringLiteral("hgs-tray"));
    // Window decorations must always receive the flat silhouette. Do not mix
    // sculpted and symbolic images in a QIcon: a compositor may pick a larger
    // representation and shrink it, bringing the texture back into a tiny glyph.
    // Not on Windows: the white silhouette would vanish on light title bars and
    // taskbars, and windows without an icon get the program's tile (hgs-tray.rc).
#ifndef Q_OS_WIN
    const QIcon appIcon = QIcon::fromTheme(QStringLiteral("hgs-zerus-swarm-symbolic"),
                                          QIcon(QStringLiteral(":/hgs/icons/hgs-zerus-symbolic.svg")));
    QApplication::setWindowIcon(appIcon);
#endif

    // Whether Sessions opens: here once the tray runs, or in a tray already running.
    const bool openSessions = cfg.opensSessions(AppConfig::currentPlatform());
    // Второй запуск не поднимает вторую иконку: стучимся в сокет первого и выходим.
#ifdef Q_OS_WIN
    const QString singleInstanceKey = SingleInstance::key(QDir::homePath());
    // Pipe names are machine-wide: another user could create this one first. A
    // per-user lock therefore decides which tray runs, and only its owner listens;
    // a second instance uses the pipe only to forward the activation.
    const QString lockDirectory = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    QDir().mkpath(lockDirectory);
    const QString lockPath = lockDirectory + QStringLiteral("/hgs-tray.lock");
    QLockFile instanceLock(lockPath);
    const bool locked = instanceLock.tryLock(0);
    if (!locked && instanceLock.error() == QLockFile::LockFailedError) {
        QLocalSocket probe;
        probe.connectToServer(singleInstanceKey);
        if (probe.waitForConnected(200)) {
            // Started by the user, this process may bring a window to the front;
            // without passing that on, the running tray's window opens behind
            // the others and only its taskbar button flashes.
            ULONG owner = 0;
            if (openSessions
                && GetNamedPipeServerProcessId(reinterpret_cast<HANDLE>(probe.socketDescriptor()), &owner))
                AllowSetForegroundWindow(owner);
            probe.write(openSessions ? "sessions\n" : "show\n");
            probe.waitForBytesWritten(200);
        }
        errOut << QStringLiteral("hgs-tray: already running\n");
        return 0;
    }
    if (!locked)
        errOut << QStringLiteral("hgs-tray: cannot create %1; a second tray would not be noticed\n")
                      .arg(QDir::toNativeSeparators(lockPath)) << Qt::flush;
    QLocalServer server;
    // Only this user may connect to the pipe it creates.
    server.setSocketOptions(QLocalServer::UserAccessOption);
    // A pipe of that name held by someone else must not stop this tray; it then
    // runs without second-instance activation.
    if (!server.listen(singleInstanceKey))
        errOut << QStringLiteral("hgs-tray: cannot listen for a second instance: %1\n").arg(server.errorString())
               << Qt::flush;
#else
    // Concurrent starts take turns, and a launcher start leaves the socket to the
    // systemd or launchd service that is starting (SingleInstance::start).
    QLocalServer server;
    {
        SingleInstance::Startup startup;
        startup.key = SingleInstance::key();
        startup.lockPath = SingleInstance::startLockPath();
        startup.activation = openSessions ? "sessions\n" : "show\n";
        startup.self = QCoreApplication::applicationPid();
        startup.service = SingleInstance::currentService;
        startup.report = [&errOut](const QString &text) {
            errOut << QStringLiteral("hgs-tray: %1\n").arg(text) << Qt::flush;
        };
        switch (SingleInstance::start(startup, server)) {
        case SingleInstance::Outcome::Forwarded:
            errOut << QStringLiteral("hgs-tray: already running\n");
            return 0;
        case SingleInstance::Outcome::Failed:
            errOut << QStringLiteral("hgs-tray: cannot claim the socket: %1\n").arg(server.errorString());
            return 3;
        case SingleInstance::Outcome::Claimed:
            break;
        }
    }
#endif

    if (!QSystemTrayIcon::isSystemTrayAvailable()) {
        errOut << QStringLiteral("hgs-tray: no system tray available\n");
        return 4;
    }

#ifdef Q_OS_WIN
    // Once this instance owns the tray: a second one never stages anything.
    cfg.hgsPath = stableHgs(cfg.hgsPath, hgsConfigured ? cfg.hgsPath : QString());
#endif
    // KSNI derives its persistent tray ID from the initial display name. Rename
    // the UI only after constructing the icon to preserve Plasma's visibility setting.
    TrayAgent agent(cfg);
#ifndef Q_OS_WIN
    // A service stop (SIGTERM) quits like the tray menu, so aboutToQuit flushes drafts.
    TerminationSignals termination([](int) { QCoreApplication::quit(); });
#endif
#ifdef Q_OS_MACOS
    MacAppVisibility appVisibility(app);
#endif
    QApplication::setApplicationDisplayName(QStringLiteral("Zerus"));
    // ПОСЛЕ agent: разрушается раньше него, поток сторожа остановлен до разбора трея.
    // abort(), а не exit(): нужен именно крэш -- его перезапускает launchd (KeepAlive
    // Crashed) и systemd (Restart=on-failure), и он оставляет крэш-репорт со стеком
    // главного потока. fprintf, а не QTextStream: зовётся из чужого потока, пока
    // главный стоит неизвестно где.
#ifdef Q_OS_WIN
    // No service manager: Windows Error Reporting restarts the tray after the
    // watchdog's abort (once it has run for a minute), with the same settings
    // from the command line, in the background (AppConfig::restartArguments), and
    // abort() must not wait for a message box on the hung desktop. A command line
    // too long to register restarts with --background alone (tray.conf then
    // applies), which beats no restart at all.
    {
        QStringList restart;
        for (const QString &argument : AppConfig::restartArguments(args))
            restart << WindowsCommandLine::argument(argument);
        std::wstring line = restart.join(QLatin1Char(' ')).toStdWString();
        if (line.size() >= RESTART_MAX_CMD_LINE)
            line = L"--background";
        RegisterApplicationRestart(line.c_str(), RESTART_NO_PATCH | RESTART_NO_REBOOT);
    }
    _set_abort_behavior(0, _WRITE_ABORT_MSG);
#endif
    MainThreadWatchdog watchdog(kMainThreadHangMs, []() {
        std::fprintf(stderr,
                     "hgs-tray: main thread unresponsive for %d s (AppKit deadlock? see "
                     "QuietTrayIcon.h); aborting so the service manager restarts it\n",
                     kMainThreadHangMs / 1000);
        std::fflush(stderr);
        std::abort();
    });
    const auto takeActivation = [&server, &agent]() {
        QLocalSocket *c = server.nextPendingConnection();
        QObject::connect(c, &QLocalSocket::disconnected, c, &QLocalSocket::deleteLater);
        auto activate = [c, &agent]() {
            if (c->canReadLine() && c->readLine().trimmed() == "sessions") agent.showSessions();
        };
        QObject::connect(c, &QLocalSocket::readyRead, c, activate);
        activate();
    };
    QObject::connect(&server, &QLocalServer::newConnection, &server, takeActivation);
#ifdef Q_OS_WIN
    // stableHgs waited for hgs in an event loop: a second instance may have
    // connected then, with no one to take it. Otherwise each later activation
    // would take the one before it.
    while (server.hasPendingConnections())
        takeActivation();
#endif
    UpdateController::instance()->checkOnStart();
    if (openSessions) agent.showSessions();
    return app.exec();
}
