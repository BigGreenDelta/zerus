// The native UI view against a fake browser factory: no Qt WebEngine. The real
// plugin is covered by test_nativeuiplugin on Linux.
#include <QtTest>
#include <QApplication>
#include <QColor>
#include <QJsonDocument>
#include <QLabel>
#include <QPushButton>
#include <QTemporaryDir>
#include <QUrl>
#include "FakeCli.h"
#include "HgsClient.h"
#include "NativeBrowser.h"
#include "NativeBrowserPlugin.h"
#include "NativeUiView.h"

namespace {
struct FakeBrowser final : NativeBrowser {
    explicit FakeBrowser(QString origin) : origin(std::move(origin)) { view.setObjectName("fakeNativeBrowser"); }
    QWidget *widget() override { return &view; }
    void load(const QUrl &url) override { loads << url; }
    QString origin;
    QList<QUrl> loads;
    QWidget view;
};
struct FakeFactory final : NativeBrowserFactory {
    NativeBrowser *create(const QString &origin, const QColor &, QWidget *) override {
        auto *browser = new FakeBrowser(origin); browsers << browser; return browser;
    }
    QList<FakeBrowser *> browsers;
};
}

class TestNativeUi : public QObject {
    Q_OBJECT
private slots:
    void reusesBrowserPerLaunchCredential();
    void reportsLoadFailureAndRendererStop();
    void rejectsNonLoopbackAndLateConnections();
    void missingPluginAsksForTheBrowser();
    void loaderSearchesOnlyFixedPaths();
    void loaderRejectsMissingForeignAndOtherInterfaces();
private:
    // A Python fixture (FakeCli.h) that prints the connection in connection.json.
    QString fakeCli(const QString &dir, const QString &address) {
        if (!FakeCli::replace(dir + "/connection.json", QJsonDocument(QJsonObject{{"url", address}}).toJson())) return {};
        const QString script = dir + "/hgs";
        if (!QFileInfo::exists(script) && !FakeCli::install(script,
                "#!/usr/bin/env python3\nimport pathlib\nprint((pathlib.Path(__file__).parent/'connection.json').read_text())\n")) return {};
        return FakeCli::program(script);
    }
    static QLabel *status(NativeUiView &view) { return view.findChild<QLabel *>("nativeUiStatus"); }
};

void TestNativeUi::reusesBrowserPerLaunchCredential()
{
    QTemporaryDir dir; QVERIFY(dir.isValid());
    const QString address = "http://127.0.0.1:43210/?token=fixture";
    HgsClient client(fakeCli(dir.path(), address)); FakeFactory factory;
    NativeUiView view(&client, [&] { return &factory; }); view.resize(1000, 700);
    view.setSession({}, "dsh/project/first", "run", "arch", true); view.show();
    QTRY_COMPARE_WITH_TIMEOUT(factory.browsers.size(), 1, 15000);
    auto *browser = factory.browsers.first();
    QCOMPARE(browser->origin, QString("http://127.0.0.1:43210")); QCOMPARE(browser->loads, QList<QUrl>{QUrl(address)});
    browser->loadFinished(true); QVERIFY(!status(view)->isVisible());
    // Another session on the same host keeps the page and its native draft.
    QSignalSpy connected(&client, &HgsClient::nativeUiReady);
    view.setSession({}, "dsh/project/second", "run-two", "arch", true); view.activate();
    QTRY_COMPARE(connected.size(), 1);
    QCOMPARE(factory.browsers.size(), 1); QCOMPARE(browser->loads.size(), 1);
    QSignalSpy respond(&view, &NativeUiView::responseRequested);
    view.setResponsePending(true);
    auto *response = view.findChild<QPushButton *>("nativeUiResponse"); QVERIFY(response->isVisible());
    response->click(); QCOMPARE(respond.size(), 1);
    view.setResponsePending(false); QVERIFY(!response->isVisible());
    // Reload reloads the same page; it does not create another profile.
    view.findChild<QPushButton *>("reloadNativeUi")->click(); QTRY_COMPARE(connected.size(), 2);
    QCOMPARE(factory.browsers.size(), 1); QCOMPARE(browser->loads.size(), 2);
    // Reusing a local port for a restarted host must not retain its old cookie/page.
    fakeCli(dir.path(), "http://127.0.0.1:43210/?token=new-generation");
    view.findChild<QPushButton *>("reloadNativeUi")->click(); QTRY_COMPARE(connected.size(), 3);
    QCOMPARE(factory.browsers.size(), 2);
    QCOMPARE(factory.browsers.last()->loads, QList<QUrl>{QUrl("http://127.0.0.1:43210/?token=new-generation")});
}

void TestNativeUi::reportsLoadFailureAndRendererStop()
{
    QTemporaryDir dir; HgsClient client(fakeCli(dir.path(), "http://127.0.0.1:43211/?token=fixture")); FakeFactory factory;
    NativeUiView view(&client, [&] { return &factory; });
    view.setSession({}, "dsh/project/a", "run", "arch", true); view.show();
    QTRY_COMPARE_WITH_TIMEOUT(factory.browsers.size(), 1, 15000);
    factory.browsers.first()->loadFinished(false);
    QVERIFY(status(view)->isVisible()); QVERIFY(status(view)->text().contains("could not load"));
    QVERIFY(!status(view)->text().contains("fixture"));
    factory.browsers.first()->rendererStopped();
    QVERIFY(status(view)->text().contains("renderer stopped"));
}

void TestNativeUi::rejectsNonLoopbackAndLateConnections()
{
    QTemporaryDir dir; HgsClient client(fakeCli(dir.path(), "https://example.invalid/?token=private-value")); FakeFactory factory;
    NativeUiView view(&client, [&] { return &factory; }); view.setSession({}, "dsh/project/a", "run", "arch", true); view.show();
    QTRY_VERIFY_WITH_TIMEOUT(status(view)->text().contains("invalid connection"), 15000);
    QVERIFY(factory.browsers.isEmpty());
    QVERIFY(!status(view)->text().contains("private-value"));
    view.setSession("mac", "dsh/project/b", "run", "mac", false);
    client.nativeUiReady(1, {{"url", "http://127.0.0.1:43210/?token=old-request"}});
    QVERIFY(factory.browsers.isEmpty());
    QVERIFY(status(view)->text().contains("offline"));
}

void TestNativeUi::missingPluginAsksForTheBrowser()
{
    QTemporaryDir dir; HgsClient client(fakeCli(dir.path(), "http://127.0.0.1:43212/?token=fixture"));
    NativeUiView view(&client, [] { return static_cast<NativeBrowserFactory *>(nullptr); });
    view.setSession({}, "dsh/project/a", "run", "arch", true); view.show();
    QTRY_VERIFY_WITH_TIMEOUT(status(view)->text().contains("browser"), 15000);
    QVERIFY(!status(view)->text().contains("fixture"));
}

void TestNativeUi::loaderSearchesOnlyFixedPaths()
{
    const auto paths = NativeBrowserPlugin::candidates();
#ifdef Q_OS_LINUX
    const QDir bin(QCoreApplication::applicationDirPath());
    QCOMPARE(paths, QStringList({QDir::cleanPath(bin.filePath("../lib/hgs-tray/plugins/libhgs-tray-native-ui.so")),
                                 bin.filePath("plugins/libhgs-tray-native-ui.so")}));
#else
    QVERIFY(paths.isEmpty());
#endif
    // This suite's directory has no plugin, so the native UI opens in the browser.
    QString reason; QVERIFY(!NativeBrowserPlugin::factory(&reason)); QVERIFY(!reason.isEmpty());
    FakeFactory fake; NativeBrowserPlugin::setFactoryForTesting(&fake);
    QCOMPARE(NativeBrowserPlugin::factory(), static_cast<NativeBrowserFactory *>(&fake));
    NativeBrowserPlugin::setFactoryForTesting(nullptr); QVERIFY(!NativeBrowserPlugin::factory());
}

void TestNativeUi::loaderRejectsMissingForeignAndOtherInterfaces()
{
    QTemporaryDir dir; QString reason;
    QVERIFY(!NativeBrowserPlugin::load(dir.filePath("absent.so"), &reason)); QVERIFY(reason.contains("not installed"));
    QFile text(dir.filePath("not-a-plugin.so")); QVERIFY(text.open(QIODevice::WriteOnly)); text.write("text"); text.close();
    reason.clear(); QVERIFY(!NativeBrowserPlugin::load(text.fileName(), &reason)); QVERIFY(!reason.isEmpty());
    // A plugin for another interface version is never called through this one.
    reason.clear(); QVERIFY(!NativeBrowserPlugin::load(QStringLiteral(ZERUS_OTHER_PLUGIN), &reason));
    QVERIFY2(reason.contains(NativeBrowserFactory_iid), qPrintable(reason));
}

QTEST_MAIN(TestNativeUi)
#include "test_nativeuiview.moc"
