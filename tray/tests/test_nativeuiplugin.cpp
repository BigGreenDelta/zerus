// The embedded native UI plugin (Linux): loaded from the build tree through the
// GUI's loader, exercised against a local fixture server and, optionally, the
// released DeepSeek frontend.
#include <QtTest>
#include <QApplication>
#include <QDesktopServices>
#include <QJsonDocument>
#include <QPointer>
#include <QPushButton>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QWebEnginePage>
#include <QWebEngineProfile>
#include <QWebEngineSettings>
#include <QWebEngineView>
#include <memory>
#include "HgsClient.h"
#include "NativeBrowser.h"
#include "NativeBrowserPlugin.h"
#include "NativeUiView.h"

// Receives the links the plugin hands to the system browser.
class BrowserLinks : public QObject {
    Q_OBJECT
public slots:
    void open(const QUrl &url) { opened << url; }
public:
    QList<QUrl> opened;
};

class TestNativeUiPlugin : public QObject {
    Q_OBJECT
private slots:
    void initTestCase();
    void authenticatesAndKeepsNativeDraft();
    void releasedHarness();
private:
    NativeBrowserFactory *m_factory = nullptr;
    QString fakeCli(const QString &dir, const QString &address) {
        QFile data(dir + "/connection.json"); if (!data.open(QIODevice::WriteOnly)) return {};
        data.write(QJsonDocument(QJsonObject{{"url", address}}).toJson()); data.close();
        QFile script(dir + "/hgs"); if (!script.open(QIODevice::WriteOnly)) return {};
        script.write("#!/bin/sh\ncat '" + QFile::encodeName(data.fileName()) + "'\n"); script.close();
        script.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner); return script.fileName();
    }
    QVariant js(QWebEngineView *view, const QString &code) {
        auto result = std::make_shared<QVariant>(); QEventLoop loop; QTimer deadline; deadline.setSingleShot(true);
        connect(&deadline, &QTimer::timeout, &loop, &QEventLoop::quit); deadline.start(3000);
        view->page()->runJavaScript(code, [result, wait = QPointer<QEventLoop>(&loop)](const QVariant &value) { *result = value; if (wait) wait->quit(); }); loop.exec(); return *result;
    }
};

void TestNativeUiPlugin::initTestCase()
{
    QString reason;
    m_factory = NativeBrowserPlugin::load(QStringLiteral(ZERUS_NATIVE_UI_PLUGIN), &reason);
    QVERIFY2(m_factory, qPrintable(reason));
}

void TestNativeUiPlugin::authenticatesAndKeepsNativeDraft()
{
    QTcpServer server; QVERIFY(server.listen(QHostAddress::LocalHost)); int authenticated = 0;
    connect(&server, &QTcpServer::newConnection, this, [&] {
        auto *socket = server.nextPendingConnection(); connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
        connect(socket, &QTcpSocket::readyRead, socket, [&, socket] {
            auto request = socket->property("request").toByteArray() + socket->readAll(); socket->setProperty("request", request);
            if (!request.contains("\r\n\r\n")) return;
            QByteArray response;
            if (request.startsWith("GET /?token=fixture ")) {
                ++authenticated;
                response = "HTTP/1.1 303 See Other\r\nLocation: /\r\nSet-Cookie: native=fixture; HttpOnly; SameSite=Strict; Path=/\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
            } else {
                const QByteArray html = request.contains("native=fixture")
                    ? "<html><body>Native page authenticated<input id='draft'></body></html>" : "<html><body>Not authenticated</body></html>";
                response = "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nContent-Length: " + QByteArray::number(html.size()) + "\r\nConnection: close\r\n\r\n" + html;
            }
            socket->write(response); socket->disconnectFromHost();
        });
    });
    QTemporaryDir dir; QVERIFY(dir.isValid());
    HgsClient client(fakeCli(dir.path(), QString("http://127.0.0.1:%1/?token=fixture").arg(server.serverPort())));
    NativeUiView view(&client, [this] { return m_factory; }); view.resize(1000, 700);
    view.setSession({}, "dsh/project/first", "run", "arch", true); view.show();
    QTRY_VERIFY_WITH_TIMEOUT(view.findChild<QWebEngineView *>("nativeWebPage"), 15000);
    auto *web = view.findChild<QWebEngineView *>("nativeWebPage");
    QTRY_VERIFY_WITH_TIMEOUT(js(web, "document.body.innerText").toString().contains("Native page authenticated"), 15000);
    QVERIFY(!web->url().hasQuery()); QCOMPARE(authenticated, 1);
    // Cookies and cache stay in memory; local content cannot read files.
    QVERIFY(web->page()->profile()->isOffTheRecord());
    QVERIFY(!web->page()->settings()->testAttribute(QWebEngineSettings::LocalContentCanAccessFileUrls));
    js(web, "document.getElementById('draft').value = 'Keep my native draft'");
    // The main frame never leaves the launch origin; a followed link to
    // another one goes to the system browser (captured here) instead.
    BrowserLinks links; QDesktopServices::setUrlHandler("http", &links, "open");
    const auto unset = qScopeGuard([] { QDesktopServices::unsetUrlHandler("http"); });
    const auto origin = web->url().adjusted(QUrl::RemovePath | QUrl::RemoveQuery);
    const QUrl elsewhere(QString("http://localhost:%1/elsewhere").arg(server.serverPort()));
    js(web, QString("document.body.insertAdjacentHTML('beforeend', \"<a id=away href='%1'>away</a>\");"
                    "document.getElementById('away').click()").arg(elsewhere.toString()));
    QTRY_COMPARE(links.opened, QList<QUrl>{elsewhere});
    QCOMPARE(web->url().adjusted(QUrl::RemovePath | QUrl::RemoveQuery), origin);
    QSignalSpy connected(&client, &HgsClient::nativeUiReady);
    view.setSession({}, "dsh/project/second", "run-two", "arch", true); view.activate();
    QTRY_COMPARE(connected.size(), 1);
    QCOMPARE(view.findChildren<QWebEngineView *>().size(), 1); QCOMPARE(authenticated, 1);
    QCOMPARE(js(web, "document.getElementById('draft').value").toString(), QString("Keep my native draft"));
    // Reusing a local port for a restarted host must not retain its old cookie/page.
    fakeCli(dir.path(), QString("http://127.0.0.1:%1/?token=new-generation").arg(server.serverPort()));
    view.findChild<QPushButton *>("reloadNativeUi")->click(); QTRY_COMPARE(connected.size(), 2);
    QCOMPARE(view.findChildren<QWebEngineView *>().size(), 2);
}

void TestNativeUiPlugin::releasedHarness()
{
    const auto source = qEnvironmentVariable("HGS_NATIVE_UI_URL_FILE");
    if (source.isEmpty()) QSKIP("Set HGS_NATIVE_UI_URL_FILE to exercise the official native frontend");
    QFile input(source); QVERIFY(input.open(QIODevice::ReadOnly));
    const auto address = QJsonDocument::fromJson(input.readAll()).object().value("url").toString(); QVERIFY(!address.isEmpty());
    QTemporaryDir dir; HgsClient client(fakeCli(dir.path(), address));
    NativeUiView view(&client, [this] { return m_factory; }); view.resize(1240, 850);
    view.setSession({}, "dsh/project/native", "run", "test host", true); view.show();
    QTRY_VERIFY_WITH_TIMEOUT(view.findChild<QWebEngineView *>("nativeWebPage"), 15000);
    auto *web = view.findChild<QWebEngineView *>("nativeWebPage");
    QTRY_VERIFY_WITH_TIMEOUT(js(web, "document.body.innerText").toString().contains("DeepSeek", Qt::CaseInsensitive), 30000);
    QVERIFY(!web->url().hasQuery());
    // This is an isolated, keyless fixture. Dismiss its native preview notice
    // and verify that the HGS-created session is present in the real sidebar.
    js(web, "Array.from(document.querySelectorAll('button')).find(b => b.textContent.trim() === 'Continue')?.click()");
    QTRY_VERIFY_WITH_TIMEOUT(js(web, "document.body.innerText").toString().contains("workspace"), 15000);
    QVERIFY(!js(web, "document.body.innerText").toString().contains("No sessions yet"));
    const auto preview = qEnvironmentVariable("HGS_NATIVE_UI_PREVIEW_PATH");
    if (!preview.isEmpty()) { QTest::qWait(1000); QVERIFY(view.grab().save(preview)); }
}

int main(int argc, char **argv)
{
    // As in the GUI: WebEngine starts from a plugin loaded after QApplication.
    QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
    QApplication app(argc, argv); TestNativeUiPlugin test; return QTest::qExec(&test, argc, argv);
}
#include "test_nativeuiplugin.moc"
