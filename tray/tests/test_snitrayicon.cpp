#include <QtTest>

#include <KStatusNotifierItem>

#include "SniTrayIcon.h"

class TestSniTrayIcon : public QObject {
    Q_OBJECT
private slots:
    void primaryClickOpensManager();
};

void TestSniTrayIcon::primaryClickOpensManager()
{
    QCoreApplication::setApplicationName("hgs-tray");
    SniTrayIcon icon;
    QGuiApplication::setApplicationDisplayName("Zerus");
    QCOMPARE(icon.item().id(), QString("hgs-tray"));
    QCOMPARE(icon.item().title(), QString("Zerus"));
    QVERIFY(!icon.item().isMenu());
    int activations = 0;
    icon.setActivationHandler([&]() { ++activations; });
    QVERIFY(QMetaObject::invokeMethod(const_cast<KStatusNotifierItem *>(&icon.item()),
        "activateRequested", Q_ARG(bool, true), Q_ARG(QPoint, QPoint(20, 40))));
    QCOMPARE(activations, 1);
    QCOMPARE(icon.item().status(), KStatusNotifierItem::Active);
    QVERIFY(!icon.item().standardActionsEnabled());
}

QTEST_MAIN(TestSniTrayIcon)
#include "test_snitrayicon.moc"
