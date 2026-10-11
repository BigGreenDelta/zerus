#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QIcon>
#include <QLocalServer>
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
#ifdef Q_OS_MACOS
#include "MacAppVisibility.h"
#endif

#include <cstdio>
#include <cstdlib>

#ifdef Q_OS_WIN
#include "ConsoleHost.h"
#include "WindowsCommandLine.h"
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
            out << QStringLiteral("Zerus %1\n").arg(QLatin1String(HGS_TRAY_VERSION));
            return 0;
        }
        errOut << QStringLiteral("hgs-tray: %1\n\n").arg(parseError) << AppConfig::usage();
        return 2;
    }

    if (cfg.selfTest) {
        QCoreApplication app(argc, argv);
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

    QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
    QApplication app(argc, argv);
#ifdef Q_OS_WIN
    // One hgs.exe for every operation: bundled next to hgs-tray.exe, else PATH.
    cfg.hgsPath = AppConfig::resolveHgsPath(cfg.hgsPath, QCoreApplication::applicationDirPath());
#endif
    ScrollbarTheme scrollbarTheme(app);
    WorkspaceFocus::install();
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
            probe.write(cfg.showSessions ? "sessions\n" : "show\n");
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
    const QString singleInstanceKey = SingleInstance::key();
    {
        QLocalSocket probe;
        probe.connectToServer(singleInstanceKey);
        if (probe.waitForConnected(200)) {
            probe.write(cfg.showSessions ? "sessions\n" : "show\n");
            probe.waitForBytesWritten(200);
            errOut << QStringLiteral("hgs-tray: already running\n");
            return 0;
        }
    }
    QLocalServer::removeServer(singleInstanceKey);
    QLocalServer server;
    if (!server.listen(singleInstanceKey)) {
        errOut << QStringLiteral("hgs-tray: cannot claim the socket: %1\n").arg(server.errorString());
        return 3;
    }
#endif

    if (!QSystemTrayIcon::isSystemTrayAvailable()) {
        errOut << QStringLiteral("hgs-tray: no system tray available\n");
        return 4;
    }

    // KSNI derives its persistent tray ID from the initial display name. Rename
    // the UI only after constructing the icon to preserve Plasma's visibility setting.
    TrayAgent agent(cfg);
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
    // from the command line, and abort() must not wait for a message box on the
    // hung desktop. A command line too long to register restarts without options
    // (tray.conf then applies), which beats no restart at all.
    {
        QStringList restart;
        for (const QString &argument : AppConfig::restartArguments(args))
            restart << WindowsCommandLine::argument(argument);
        std::wstring line = restart.join(QLatin1Char(' ')).toStdWString();
        if (line.size() >= RESTART_MAX_CMD_LINE)
            line.clear();
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
    QObject::connect(&server, &QLocalServer::newConnection, &server, [&server, &agent]() {
        QLocalSocket *c = server.nextPendingConnection();
        QObject::connect(c, &QLocalSocket::disconnected, c, &QLocalSocket::deleteLater);
        auto activate = [c, &agent]() {
            if (c->canReadLine() && c->readLine().trimmed() == "sessions") agent.showSessions();
        };
        QObject::connect(c, &QLocalSocket::readyRead, c, activate);
        activate();
    });
    UpdateController::instance()->checkOnStart();
    if (cfg.showSessions) agent.showSessions();
    return app.exec();
}
