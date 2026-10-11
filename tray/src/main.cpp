#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QIcon>
#include <QLocalServer>
#include <QLocalSocket>
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
#ifdef Q_OS_MACOS
#include "MacAppVisibility.h"
#endif

#include <cstdio>
#include <cstdlib>

#ifndef HGS_TRAY_VERSION
#define HGS_TRAY_VERSION "0.0.0"
#endif

static const char *kSingleInstanceKey = "hgs-tray";

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
    QStringList args;
    args.reserve(argc);
    for (int i = 0; i < argc; ++i)
        args << QString::fromLocal8Bit(argv[i]);

    QTextStream out(stdout), errOut(stderr);

    AppConfig cfg;
    QString parseError;
    if (!AppConfig::load(args, &cfg, &parseError)) {
        if (parseError == QLatin1String("__help__")) { out << AppConfig::usage(); return 0; }
        if (parseError == QLatin1String("__version__")) {
            out << QStringLiteral("Zerus %1\n").arg(QLatin1String(HGS_TRAY_VERSION));
            return 0;
        }
        errOut << QStringLiteral("hgs-tray: %1\n\n").arg(parseError) << AppConfig::usage();
        return 2;
    }
    if (cfg.hgsPath.isEmpty())
        cfg.hgsPath = AppConfig::fallbackHgsPath(qEnvironmentVariable("PATH"),
                                                 AppConfig::hgsInstallDirs(QDir::homePath()));

    if (cfg.selfTest) {
        QCoreApplication app(argc, argv);
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
    const QIcon appIcon = QIcon::fromTheme(QStringLiteral("hgs-zerus-swarm-symbolic"),
                                          QIcon(QStringLiteral(":/hgs/icons/hgs-zerus-symbolic.svg")));
    QApplication::setWindowIcon(appIcon);

    // Второй запуск не поднимает вторую иконку: стучимся в сокет первого и выходим.
    {
        QLocalSocket probe;
        probe.connectToServer(QLatin1String(kSingleInstanceKey));
        if (probe.waitForConnected(200)) {
            probe.write(cfg.showSessions ? "sessions\n" : "show\n");
            probe.waitForBytesWritten(200);
            errOut << QStringLiteral("hgs-tray: already running\n");
            return 0;
        }
    }
    QLocalServer::removeServer(QLatin1String(kSingleInstanceKey));
    QLocalServer server;
    if (!server.listen(QLatin1String(kSingleInstanceKey))) {
        errOut << QStringLiteral("hgs-tray: cannot claim the socket: %1\n").arg(server.errorString());
        return 3;
    }

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
