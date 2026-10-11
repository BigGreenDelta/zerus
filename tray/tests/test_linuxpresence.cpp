#include <QtTest>
#include <QDBusConnection>
#include <QDBusContext>
#include <QDBusError>
#include <optional>
#include "LinuxPresenceSource.h"

class ScreenSaver : public QObject, protected QDBusContext {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.ScreenSaver")
public:
    bool active = false, idleSupported = true; uint idle = 0; int idleCalls = 0;
public slots:
    bool GetActive() { return active; }
    uint GetSessionIdleTime() {
        ++idleCalls;
        if (!idleSupported) sendErrorReply(QDBusError::NotSupported, "GetSessionIdleTime is not supported on this platform");
        return idle;
    }
};
class MutterIdle : public QObject {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.gnome.Mutter.IdleMonitor")
public:
    quint64 idle = 0;
public slots:
    quint64 GetIdletime() { return idle; }
};
class SignedMutterIdle : public QObject {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.gnome.Mutter.IdleMonitor")
public slots:
    qint64 GetIdletime() { return -5000; }
};
class GnomeScreenSaver : public QObject {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.gnome.ScreenSaver")
public:
    bool active = false;
public slots:
    bool GetActive() { return active; }
};

class TestLinuxPresence : public QObject {
    Q_OBJECT
    static std::optional<PresenceSample> sample(LinuxPresenceSource &source) {
        std::optional<PresenceSample> result;
        const auto connection = QObject::connect(&source, &PresenceSource::sampled, [&result](const PresenceSample &s) { result = s; });
        source.sample();
        (void)QTest::qWaitFor([&result] { return result.has_value(); }, 5000);
        QObject::disconnect(connection);
        return result;
    }
    static QDBusConnection serve(const QString &name, const QString &service, const QString &path, QObject *object) {
        auto bus = QDBusConnection::connectToBus(QDBusConnection::SessionBus, name);
        if (!bus.registerService(service) || !bus.registerObject(path, object, QDBusConnection::ExportAllSlots)) qFatal("fake D-Bus service failed");
        return bus;
    }
private slots:
    void cleanup() {
        for (const auto *name : {"fake-screensaver", "fake-mutter", "fake-gnome"}) QDBusConnection::disconnectFromBus(name);
    }
    void x11ReadsIdleInProcessBeforeTheScreenSaver() {
        ScreenSaver saver; saver.idle = 7; saver.active = true;
        serve("fake-screensaver", "org.freedesktop.ScreenSaver", "/org/freedesktop/ScreenSaver", &saver);
        LinuxPresenceSource source(QDBusConnection::sessionBus(), {}, [] { return std::optional<qint64>(90500); });
        const auto result = sample(source); QVERIFY(result);
        QVERIFY(result->available); QCOMPARE(result->idleSeconds, 90.5); QVERIFY(result->locked);
        QCOMPARE(saver.idleCalls, 0);
    }
    void screenSaverFallbackUsesSecondsOutsideKde() {
        ScreenSaver saver; saver.idle = 42; saver.active = true;
        serve("fake-screensaver", "org.freedesktop.ScreenSaver", "/org/freedesktop/ScreenSaver", &saver);
        LinuxPresenceSource source(QDBusConnection::sessionBus());
        const auto result = sample(source); QVERIFY(result);
        QVERIFY(result->available); QCOMPARE(result->idleSeconds, 42.0); QVERIFY(result->locked);
    }
    void kdeScreenSaverFallbackUsesMilliseconds() {
        // kscreenlocker answers GetSessionIdleTime with KIdleTime's milliseconds.
        ScreenSaver saver; saver.idle = 42000;
        auto bus = serve("fake-screensaver", "org.freedesktop.ScreenSaver", "/org/freedesktop/ScreenSaver", &saver);
        QVERIFY(bus.registerService("org.kde.screensaver"));
        LinuxPresenceSource source(QDBusConnection::sessionBus(), {}, [] { return std::optional<qint64>(); });
        const auto result = sample(source); QVERIFY(result);
        QVERIFY(result->available); QCOMPARE(result->idleSeconds, 42.0);
    }
    void waylandIdleNeedsVerifiedThresholdEvents() {
        ScreenSaver saver; saver.idleSupported = false;
        serve("fake-screensaver", "org.freedesktop.ScreenSaver", "/org/freedesktop/ScreenSaver", &saver);
        qint64 now = 1000000;
        LinuxPresenceSource source(QDBusConnection::sessionBus(), [&now] { return now; });
        auto result = sample(source); QVERIFY(result);
        QVERIFY(!result->available); QVERIFY(!result->locked); // unverified thresholds never mean "active"
        source.thresholdReached(); now += 20000;
        result = sample(source); QVERIFY(result && result->available); QCOMPARE(result->idleSeconds, 30.0);
        source.resumed();
        result = sample(source); QVERIFY(result && result->available); QCOMPARE(result->idleSeconds, 0.0);
    }
    void earlyThresholdsStillReportIdle() {
        ScreenSaver saver; saver.idleSupported = false;
        serve("fake-screensaver", "org.freedesktop.ScreenSaver", "/org/freedesktop/ScreenSaver", &saver);
        qint64 now = 0; // the default monotonic clock also starts at zero
        LinuxPresenceSource source(QDBusConnection::sessionBus(), [&now] { return now; });
        now = 5000; source.thresholdReached();
        const auto result = sample(source); QVERIFY(result && result->available);
        QCOMPARE(result->idleSeconds, 10.0);
    }
    void gnomeUsesMutterMillisecondsAndItsLock() {
        MutterIdle mutter; mutter.idle = 90500; GnomeScreenSaver gnome; gnome.active = true;
        serve("fake-mutter", "org.gnome.Mutter.IdleMonitor", "/org/gnome/Mutter/IdleMonitor/Core", &mutter);
        serve("fake-gnome", "org.gnome.ScreenSaver", "/org/gnome/ScreenSaver", &gnome);
        LinuxPresenceSource source(QDBusConnection::sessionBus());
        const auto result = sample(source); QVERIFY(result);
        QVERIFY(result->available); QCOMPARE(result->idleSeconds, 90.5); QVERIFY(result->locked);
    }
    void negativeIdleRepliesAreRejected() {
        SignedMutterIdle mutter;
        serve("fake-mutter", "org.gnome.Mutter.IdleMonitor", "/org/gnome/Mutter/IdleMonitor/Core", &mutter);
        LinuxPresenceSource source(QDBusConnection::sessionBus());
        const auto result = sample(source); QVERIFY(result);
        QVERIFY(!result->available);
    }
    void missingServicesReportUnavailable() {
        LinuxPresenceSource source(QDBusConnection::sessionBus());
        const auto result = sample(source); QVERIFY(result);
        QVERIFY(!result->available); QVERIFY(!result->locked);
    }
};

QTEST_GUILESS_MAIN(TestLinuxPresence)
#include "test_linuxpresence.moc"
