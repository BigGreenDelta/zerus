#include <QtTest>
#include <QDBusConnection>
#include <QDBusContext>
#include <QSettings>
#include <QTemporaryDir>
#include "LinuxAttentionNotifier.h"

class NotificationServer : public QObject, protected QDBusContext {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.Notifications")
public:
    struct Call { uint id; QString body; QStringList actions; QVariantMap hints; };
    QList<Call> calls;
    QList<uint> closed;
    bool failNext = false;
public slots:
    uint Notify(const QString &, uint, const QString &, const QString &, const QString &body,
                const QStringList &actions, const QVariantMap &hints, int) {
        if (failNext) { failNext=false; sendErrorReply("org.hgs.NotificationFailed", "Test delivery failure"); return 0; }
        const uint id = 100 + calls.size(); calls << Call{id, body, actions, hints}; return id;
    }
    void CloseNotification(uint id) { closed << id; emit NotificationClosed(id, 3); }
signals:
    void ActionInvoked(uint id, const QString &action);
    void NotificationClosed(uint id, uint reason);
    void ActivationToken(uint id, const QString &token);
};
class TestLinuxAttention : public QObject {
    Q_OBJECT
private slots:
    void exactTargetsPersistenceAndCancellation();
};
void TestLinuxAttention::exactTargetsPersistenceAndCancellation()
{
    QTemporaryDir dir; QVERIFY(dir.isValid());
    QCoreApplication::setOrganizationName("hgs-tests"); QCoreApplication::setApplicationName("attention");
    QSettings::setDefaultFormat(QSettings::IniFormat); QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, dir.path());
    auto serverBus = QDBusConnection::connectToBus(QDBusConnection::SessionBus, "fake-notifications");
    QVERIFY(serverBus.registerService("org.freedesktop.Notifications"));
    NotificationServer server;
    QVERIFY(serverBus.registerObject("/org/freedesktop/Notifications", &server,
        QDBusConnection::ExportAllSlots | QDBusConnection::ExportAllSignals));
    auto notifier = std::make_unique<LinuxAttentionNotifier>();
    QSignalSpy clicked(notifier.get(), &AttentionNotifier::activated);
    notifier->post("first", "Attention", "<b>Literal & session</b>");
    notifier->post("second", "Attention", "Another session");
    QTRY_COMPARE(server.calls.size(), 2); QTest::qWait(30);
    QCOMPARE(server.calls[0].body, QString("&lt;b&gt;Literal &amp; session&lt;/b&gt;"));
    QCOMPARE(server.calls[0].actions, QStringList({"default", "Open session", "open-session", "Open session"}));
    QCOMPARE(server.calls[0].hints.value("desktop-entry").toString(), QString("hgs-tray"));
    emit server.ActivationToken(101, "wayland-click-token"); emit server.ActionInvoked(101, "open-session");
    emit server.ActionInvoked(100, "default");
    QTRY_COMPARE(clicked.size(), 2);
    QCOMPARE(clicked[0][0].toString(), QString("second")); QCOMPARE(clicked[0][1].toString(), QString("wayland-click-token"));
    QCOMPARE(clicked[1][0].toString(), QString("first"));
    emit server.ActionInvoked(999, "default"); emit server.ActionInvoked(100, "unknown");
    QTest::qWait(20); QCOMPARE(clicked.size(), 2);
    notifier.reset(); notifier = std::make_unique<LinuxAttentionNotifier>();
    QSignalSpy afterRestart(notifier.get(), &AttentionNotifier::activated);
    emit server.NotificationClosed(100, 1); emit server.ActionInvoked(100, "default");
    QTRY_COMPARE(afterRestart.size(), 1); QCOMPARE(afterRestart[0][0].toString(), QString("first"));
    notifier->withdraw("first"); QTRY_VERIFY(server.closed.contains(100));
    emit server.ActionInvoked(100, "default"); QTest::qWait(20); QCOMPARE(afterRestart.size(), 1);
    notifier->post("cancelled-before-reply", "Attention", "Gone already");
    notifier->withdraw("cancelled-before-reply");
    QTRY_COMPARE(server.calls.size(), 3); QTRY_VERIFY(server.closed.contains(102));
    emit server.ActionInvoked(102, "default"); QTest::qWait(20); QCOMPARE(afterRestart.size(), 1);
    QSignalSpy failed(notifier.get(), &AttentionNotifier::failed); server.failNext=true;
    notifier->post("failed-delivery", "Attention", "Retry later");
    QTRY_COMPARE(failed.size(), 1); QCOMPARE(failed[0][0].toString(), QString("failed-delivery"));
    QVERIFY(failed[0][1].toString().contains("Test delivery failure"));
    notifier->post("failed-delivery", "Attention", "Retry later");
    QTRY_COMPARE(server.calls.size(), 4);
    // A notice may name its action: a Your Call task opens a task, not a session.
    notifier->post("decision", "Zerus — Your Call", "om-k3f9x: Pick a store", "Open task");
    QTRY_COMPARE(server.calls.size(), 5); QTest::qWait(30);
    QCOMPARE(server.calls[4].actions, QStringList({"default", "Open task", "open-session", "Open task"}));
    emit server.ActionInvoked(server.calls[4].id, "open-session");
    QTRY_COMPARE(afterRestart.size(), 2); QCOMPARE(afterRestart[1][0].toString(), QString("decision"));
}
QTEST_GUILESS_MAIN(TestLinuxAttention)
#include "test_linuxattention.moc"
