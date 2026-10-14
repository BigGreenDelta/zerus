#include <QtTest>
#include <QDir>
#include <QJsonObject>
#include <QSysInfo>
#include "MachinePlatform.h"
#include "LocalSessionHost.h"

using MachinePlatform::Style;

// Folder paths per machine (MachinePlatform.h). Both styles run on every platform;
// Posix answers are compared with verbatim copies of the expressions the GUI used
// before Windows, so a Linux or macOS GUI behaves byte for byte as it did.
class TestMachinePlatform : public QObject {
    Q_OBJECT
private slots:
    void peersArePosixAndTheLocalMachineFollowsItsPlatform();
    void posixAnswersAreTheFormerExpressions_data();
    void posixAnswersAreTheFormerExpressions();
    void posixPinsForMachinePaths();
    void absolutePaths_data();
    void absolutePaths();
    void windowsShapedPaths_data();
    void windowsShapedPaths();
    void cleanWindowsPaths_data();
    void cleanWindowsPaths();
    void cleanKeepsWindowsFoldersAbsoluteAndIsStable();
    void sameIgnoresCaseAndSeparatorsOnlyOnWindows();
    void withinWindowsFolders_data();
    void withinWindowsFolders();
    void leafOfWindowsPaths_data();
    void leafOfWindowsPaths();
    void nativeSeparatorsOnlyForWindows();
    void otherWindowsSession_data();
    void otherWindowsSession();
    void ownWindowsSessionOnlyOnWindows();
};

void TestMachinePlatform::peersArePosixAndTheLocalMachineFollowsItsPlatform()
{
    const QJsonObject windows{{"platform", "windows"}}, linuxBox{{"platform", "linux"}}, macos{{"platform", "macos"}};
    QCOMPARE(MachinePlatform::of(windows, true), Style::Windows);
    QCOMPARE(MachinePlatform::of(linuxBox, true), Style::Posix);
    QCOMPARE(MachinePlatform::of(macos, true), Style::Posix);
    // Windows peers are not supported in Phase 2: a peer is always POSIX.
    QCOMPARE(MachinePlatform::of(windows, false), Style::Posix);
    QCOMPARE(MachinePlatform::of({}, false), Style::Posix);
    // Before the first poll, or from a CLI that reports no platform: the GUI's own.
    const auto own = QSysInfo::productType() == QLatin1String("windows") ? Style::Windows : Style::Posix;
    QCOMPARE(MachinePlatform::build(), own);
    QCOMPARE(MachinePlatform::of({}, true), own);
    QCOMPARE(MachinePlatform::of(QJsonObject{{"platform", ""}}, true), own);
    QCOMPARE(MachinePlatform::of(QJsonObject{{"platform", 1}}, true), own);
    QCOMPARE(MachinePlatform::of(QJsonObject{{"platform", "Windows"}}, true), Style::Posix);
}

void TestMachinePlatform::posixAnswersAreTheFormerExpressions_data()
{
    QTest::addColumn<QString>("path");
    QTest::addColumn<QString>("folder");
    const QStringList paths{"", "/", "//", "/work", "/work/", "/work/sub", "/workshop", "/Work", "/a/./b/../c/",
        "/../x", "//server/share", "work", "work/..", "./work", ":work", ":/work", "C:/work", "C:\\work",
        "\\\\server\\share", "/work\\sub", "/home/user/project dir"};
    const QStringList folders{"", "/", "/work", "/work/", "/Work", "work", ":work", "C:/work", "//server/share"};
    for (const auto &path : paths)
        for (const auto &folder : folders)
            QTest::addRow("%s in %s", qPrintable(path), qPrintable(folder)) << path << folder;
}

void TestMachinePlatform::posixAnswersAreTheFormerExpressions()
{
    QFETCH(QString, path);
    QFETCH(QString, folder);
    // SessionOrganization, ProjectsDialog, SessionsWindow and SessionCommands gates.
    QCOMPARE(MachinePlatform::absolute(path, Style::Posix), path.startsWith('/'));
    QCOMPARE(MachinePlatform::clean(path, Style::Posix), QDir::cleanPath(path));
    QCOMPARE(MachinePlatform::same(path, folder, Style::Posix), path == folder);
    // ProjectsDialog::suggestName and the folder filter's label.
    QCOMPARE(MachinePlatform::leaf(path, Style::Posix), path.section('/', -1));
    QCOMPARE(MachinePlatform::native(path, Style::Posix), path);
    // NewSessionDialog's withinFolder.
    const bool withinFolder = QDir::isAbsolutePath(folder) && (path == folder || path.startsWith(folder == "/" ? folder : folder + '/'));
    QCOMPARE(MachinePlatform::within(path, folder, Style::Posix), withinFolder);
}

void TestMachinePlatform::posixPinsForMachinePaths()
{
    QVERIFY(MachinePlatform::absolute("/", Style::Posix));
    QVERIFY(MachinePlatform::absolute("/home/user", Style::Posix));
    QVERIFY(!MachinePlatform::absolute("C:/work", Style::Posix));
    QVERIFY(!MachinePlatform::absolute("C:\\work", Style::Posix));
    QVERIFY(!MachinePlatform::absolute("work", Style::Posix));
    QCOMPARE(MachinePlatform::clean("/work/", Style::Posix), QString("/work"));
    QCOMPARE(MachinePlatform::clean("/a/./b/../c/", Style::Posix), QString("/a/c"));
    QCOMPARE(MachinePlatform::clean("/", Style::Posix), QString("/"));
    QVERIFY(MachinePlatform::same("/work", "/work", Style::Posix));
    QVERIFY(!MachinePlatform::same("/work", "/Work", Style::Posix));
    QVERIFY(!MachinePlatform::same("/work", "/work/", Style::Posix));
    QVERIFY(MachinePlatform::within("/work", "/work", Style::Posix));
    QVERIFY(MachinePlatform::within("/work/sub", "/work", Style::Posix));
    QVERIFY(MachinePlatform::within("/work", "/", Style::Posix));
    QVERIFY(!MachinePlatform::within("/workshop", "/work", Style::Posix));
    QVERIFY(!MachinePlatform::within("/Work/sub", "/work", Style::Posix));
    QVERIFY(!MachinePlatform::within("work/sub", "work", Style::Posix));
    QCOMPARE(MachinePlatform::leaf("/home/user/project", Style::Posix), QString("project"));
    QCOMPARE(MachinePlatform::leaf("/", Style::Posix), QString());
    QCOMPARE(MachinePlatform::leaf("/work/", Style::Posix), QString());
    // A backslash is an ordinary character in a POSIX name.
    QCOMPARE(MachinePlatform::leaf("/work/a\\b", Style::Posix), QString("a\\b"));
    QCOMPARE(MachinePlatform::native("/work/sub", Style::Posix), QString("/work/sub"));
}

void TestMachinePlatform::absolutePaths_data()
{
    QTest::addColumn<QString>("path");
    QTest::addColumn<bool>("posix");
    QTest::addColumn<bool>("windows");
    QTest::newRow("posix folder") << "/work" << true << false;
    QTest::newRow("posix root") << "/" << true << false;
    QTest::newRow("drive backslash") << "C:\\work" << false << true;
    QTest::newRow("drive slash") << "C:/work" << false << true;
    QTest::newRow("drive mixed") << "C:\\work/sub\\dir" << false << true;
    QTest::newRow("lower drive root") << "c:/" << false << true;
    QTest::newRow("drive root backslash") << "C:\\" << false << true;
    QTest::newRow("drive without separator") << "C:" << false << false;
    QTest::newRow("drive relative") << "C:work" << false << false;
    QTest::newRow("digit drive") << "1:/work" << false << false;
    QTest::newRow("non-ASCII drive") << QString::fromUtf8("\xc3\x84:/work") << false << false;
    QTest::newRow("unc backslash") << "\\\\server\\share" << false << true;
    QTest::newRow("unc slash") << "//server/share" << true << true;
    QTest::newRow("unc folder") << "\\\\server\\share\\dir" << false << true;
    QTest::newRow("unc mixed") << "\\/server/share\\dir" << false << true;
    QTest::newRow("unc trailing") << "//server/share/" << true << true;
    QTest::newRow("server only") << "\\\\server" << false << false;
    QTest::newRow("server slash only") << "//server" << true << false;
    QTest::newRow("server and separator") << "\\\\server\\" << false << false;
    QTest::newRow("empty share") << "//server//dir" << true << false;
    QTest::newRow("three separators") << "///share" << true << false;
    QTest::newRow("verbatim") << "\\\\?\\C:\\work" << false << false;
    QTest::newRow("device") << "//./pipe/name" << true << false;
    QTest::newRow("rooted without drive") << "\\work" << false << false;
    QTest::newRow("relative") << "work" << false << false;
    QTest::newRow("empty") << "" << false << false;
}

void TestMachinePlatform::absolutePaths()
{
    QFETCH(QString, path);
    QFETCH(bool, posix);
    QFETCH(bool, windows);
    QCOMPARE(MachinePlatform::absolute(path, Style::Posix), posix);
    QCOMPARE(MachinePlatform::absolute(path, Style::Windows), windows);
}

void TestMachinePlatform::windowsShapedPaths_data()
{
    QTest::addColumn<QString>("path");
    QTest::addColumn<bool>("shaped");
    QTest::newRow("drive backslash") << "C:\\work" << true;
    QTest::newRow("drive root") << "c:/" << true;
    QTest::newRow("unc slash") << "//server/share" << true;
    QTest::newRow("unc backslash") << "\\\\server\\share\\dir" << true;
    QTest::newRow("verbatim") << "\\\\?\\C:\\work" << true;
    QTest::newRow("posix folder") << "/home/user/project" << false;
    QTest::newRow("posix root") << "/" << false;
    QTest::newRow("server only") << "//server" << false;
    QTest::newRow("drive without separator") << "C:" << false;
    QTest::newRow("drive relative") << "C:work" << false;
    QTest::newRow("posix colon") << "/work/C:/x" << false;
    QTest::newRow("relative") << "work" << false;
    QTest::newRow("empty") << "" << false;
}

void TestMachinePlatform::windowsShapedPaths()
{
    QFETCH(QString, path);
    QFETCH(bool, shaped);
    QCOMPARE(MachinePlatform::windowsShaped(path), shaped);
}

void TestMachinePlatform::cleanWindowsPaths_data()
{
    QTest::addColumn<QString>("path");
    QTest::addColumn<QString>("clean");
    QTest::newRow("backslashes") << "C:\\work\\project" << "C:/work/project";
    QTest::newRow("trailing separator") << "C:\\work\\" << "C:/work";
    QTest::newRow("trailing slashes") << "C:/work//" << "C:/work";
    QTest::newRow("drive root backslash") << "C:\\" << "C:/";
    QTest::newRow("drive root keeps case") << "c:/" << "c:/";
    QTest::newRow("drive root twice") << "C://" << "C:/";
    QTest::newRow("mixed separators") << "C:\\work/sub\\dir" << "C:/work/sub/dir";
    QTest::newRow("dots") << "C:/work/./sub/../x" << "C:/work/x";
    QTest::newRow("doubled separators") << "C:\\\\work\\\\sub" << "C:/work/sub";
    QTest::newRow("parent of drive root") << "C:/.." << "C:/";
    QTest::newRow("above drive root") << "C:\\..\\work" << "C:/work";
    QTest::newRow("unc backslash") << "\\\\server\\share" << "//server/share";
    QTest::newRow("unc trailing") << "\\\\server\\share\\" << "//server/share";
    QTest::newRow("unc folder") << "//server/share/dir/" << "//server/share/dir";
    QTest::newRow("unc mixed") << "\\/server\\share/dir" << "//server/share/dir";
    QTest::newRow("unc parent") << "\\\\server\\share\\dir\\.." << "//server/share";
    QTest::newRow("above unc root") << "//server/share/../x" << "//server/share/x";
    QTest::newRow("unc keeps case") << "\\\\Server\\Share\\Dir" << "//Server/Share/Dir";
    QTest::newRow("empty") << "" << "";
    QTest::newRow("relative") << "a\\b\\..\\c" << "a/c";
    QTest::newRow("relative up") << "..\\a" << "../a";
    QTest::newRow("relative gone") << "work\\..\\" << ".";
    QTest::newRow("rooted without drive") << "\\work\\" << "/work";
}

void TestMachinePlatform::cleanWindowsPaths()
{
    QFETCH(QString, path);
    QFETCH(QString, clean);
    QCOMPARE(MachinePlatform::clean(path, Style::Windows), clean);
}

void TestMachinePlatform::cleanKeepsWindowsFoldersAbsoluteAndIsStable()
{
    for (const QString path : {"C:\\", "c:/work/", "C:/..", "C:\\work\\..\\..\\x", "\\\\server\\share",
             "//server/share/..", "\\/server/share\\a\\.\\b"}) {
        const auto clean = MachinePlatform::clean(path, Style::Windows);
        QVERIFY2(MachinePlatform::absolute(clean, Style::Windows), qPrintable(path));
        QCOMPARE(MachinePlatform::clean(clean, Style::Windows), clean);
        QVERIFY2(MachinePlatform::same(MachinePlatform::clean(MachinePlatform::native(clean, Style::Windows), Style::Windows),
                     clean, Style::Windows), qPrintable(path));
        QVERIFY2(MachinePlatform::windowsShaped(clean), qPrintable(path));
    }
}

void TestMachinePlatform::sameIgnoresCaseAndSeparatorsOnlyOnWindows()
{
    QVERIFY(MachinePlatform::same("C:/work", "c:\\WORK", Style::Windows));
    QVERIFY(MachinePlatform::same("C:/Users/user/Project", "C:\\users\\USER\\project", Style::Windows));
    QVERIFY(MachinePlatform::same("//server/share/dir", "\\\\SERVER\\Share\\dir", Style::Windows));
    QVERIFY(MachinePlatform::same("C:/", "c:\\", Style::Windows));
    QVERIFY(!MachinePlatform::same("C:/work", "D:/work", Style::Windows));
    QVERIFY(!MachinePlatform::same("C:/work", "C:/workshop", Style::Windows));
    // Callers compare clean forms: a trailing separator is a difference.
    QVERIFY(!MachinePlatform::same("C:/work", "C:/work/", Style::Windows));
    QVERIFY(MachinePlatform::same(MachinePlatform::clean("C:\\work\\", Style::Windows), "c:/work", Style::Windows));
    // POSIX names keep their case, and a backslash is part of a name.
    QVERIFY(!MachinePlatform::same("/work", "/Work", Style::Posix));
    QVERIFY(!MachinePlatform::same("C:/work", "C:\\work", Style::Posix));
    QVERIFY(!MachinePlatform::same("/work/a\\b", "/work/a/b", Style::Posix));
}

void TestMachinePlatform::withinWindowsFolders_data()
{
    QTest::addColumn<QString>("path");
    QTest::addColumn<QString>("folder");
    QTest::addColumn<bool>("within");
    QTest::newRow("same folder") << "C:/work" << "C:/work" << true;
    QTest::newRow("same folder other case") << "c:\\WORK" << "C:/work" << true;
    QTest::newRow("child") << "C:\\work\\sub" << "C:/work" << true;
    QTest::newRow("child other case") << "c:/Work/Sub/deep" << "C:\\WORK" << true;
    QTest::newRow("sibling prefix") << "C:/workshop" << "C:/work" << false;
    QTest::newRow("sibling prefix child") << "C:\\workshop\\sub" << "C:\\work" << false;
    QTest::newRow("parent") << "C:/work" << "C:/work/sub" << false;
    QTest::newRow("drive root") << "C:\\work" << "C:\\" << true;
    QTest::newRow("drive root itself") << "C:/" << "c:\\" << true;
    QTest::newRow("other drive") << "D:/work" << "C:/" << false;
    QTest::newRow("unc child") << "//server/share/dir" << "\\\\SERVER\\share" << true;
    QTest::newRow("unc itself") << "\\\\server\\share" << "//server/share" << true;
    QTest::newRow("unc sibling share") << "//server/shared" << "//server/share" << false;
    QTest::newRow("unc other server") << "//server2/share/dir" << "//server/share" << false;
    QTest::newRow("posix folder") << "C:/work/x" << "/work" << false;
    QTest::newRow("posix root") << "/work" << "/" << false;
    QTest::newRow("drive relative") << "C:work/x" << "C:work" << false;
    QTest::newRow("verbatim") << "\\\\?\\C:\\work\\x" << "\\\\?\\C:\\work" << false;
    QTest::newRow("empty folder") << "C:/work" << "" << false;
}

void TestMachinePlatform::withinWindowsFolders()
{
    QFETCH(QString, path);
    QFETCH(QString, folder);
    QFETCH(bool, within);
    QCOMPARE(MachinePlatform::within(path, folder, Style::Windows), within);
}

void TestMachinePlatform::leafOfWindowsPaths_data()
{
    QTest::addColumn<QString>("path");
    QTest::addColumn<QString>("leaf");
    QTest::newRow("backslashes") << "C:\\work\\project" << "project";
    QTest::newRow("slashes") << "C:/Users/user/Project" << "Project";
    QTest::newRow("mixed") << "C:\\work/sub\\dir" << "dir";
    QTest::newRow("drive root") << "C:\\" << "";
    QTest::newRow("drive root slash") << "c:/" << "";
    QTest::newRow("trailing separator") << "C:\\work\\" << "";
    QTest::newRow("unc root") << "\\\\server\\share" << "";
    QTest::newRow("unc root slash") << "//server/share" << "";
    QTest::newRow("unc root trailing") << "//server/share/" << "";
    QTest::newRow("unc folder") << "\\\\server\\share\\dir" << "dir";
    QTest::newRow("empty") << "" << "";
}

void TestMachinePlatform::leafOfWindowsPaths()
{
    QFETCH(QString, path);
    QFETCH(QString, leaf);
    QCOMPARE(MachinePlatform::leaf(path, Style::Windows), leaf);
}

void TestMachinePlatform::nativeSeparatorsOnlyForWindows()
{
    QCOMPARE(MachinePlatform::native("C:/work/sub", Style::Windows), QString("C:\\work\\sub"));
    QCOMPARE(MachinePlatform::native("C:/", Style::Windows), QString("C:\\"));
    QCOMPARE(MachinePlatform::native("//server/share/dir", Style::Windows), QString("\\\\server\\share\\dir"));
    QCOMPARE(MachinePlatform::native("C:\\already\\native", Style::Windows), QString("C:\\already\\native"));
    QCOMPARE(MachinePlatform::native(MachinePlatform::clean("c:/Work/", Style::Windows), Style::Windows), QString("c:\\Work"));
    QCOMPARE(MachinePlatform::native("/home/user/project", Style::Posix), QString("/home/user/project"));
    QCOMPARE(MachinePlatform::native("C:/work", Style::Posix), QString("C:/work"));
}

// Spec D5: a launch on this computer warns only when its running session host
// reports another Windows session than the GUI's own.
void TestMachinePlatform::otherWindowsSession_data()
{
    QTest::addColumn<QString>("platform");
    QTest::addColumn<QString>("host");
    QTest::addColumn<QJsonObject>("localHost");
    QTest::addColumn<qint64>("own");      // -1: the GUI has no Windows session.
    QTest::addColumn<qint64>("expected"); // -1: no warning.
    const auto running = [](const QJsonValue &session) {
        return QJsonObject{{"state", "running"}, {"pid", 4242}, {"protocol", 1}, {"conpty", "bundled"},
                           {"integrity", "medium"}, {"windows_session", session}};
    };
    const auto idle = [](const QString &state) {
        return QJsonObject{{"state", state}, {"pid", QJsonValue::Null}, {"protocol", QJsonValue::Null},
                           {"conpty", QJsonValue::Null}, {"integrity", QJsonValue::Null}, {"windows_session", QJsonValue::Null}};
    };
    auto elevated = idle("unavailable"); elevated["error"] = "the session host on this computer runs elevated";
    QTest::newRow("other session") << "windows" << QString() << running(2) << qint64(1) << qint64(2);
    QTest::newRow("services session") << "windows" << QString() << running(0) << qint64(1) << qint64(0);
    QTest::newRow("from the services session") << "windows" << QString() << running(3) << qint64(0) << qint64(3);
    QTest::newRow("same session") << "windows" << QString() << running(1) << qint64(1) << qint64(-1);
    QTest::newRow("not installed") << "windows" << QString() << idle("not-installed") << qint64(1) << qint64(-1);
    QTest::newRow("no host runs") << "windows" << QString() << idle("stopped") << qint64(1) << qint64(-1);
    QTest::newRow("unavailable host") << "windows" << QString() << elevated << qint64(1) << qint64(-1);
    QTest::newRow("null session") << "windows" << QString() << running(QJsonValue::Null) << qint64(1) << qint64(-1);
    QTest::newRow("missing session") << "windows" << QString()
        << QJsonObject{{"state", "running"}, {"pid", 4242}} << qint64(1) << qint64(-1);
    QTest::newRow("older CLI") << "windows" << QString() << QJsonObject() << qint64(1) << qint64(-1);
    QTest::newRow("no own session") << "windows" << QString() << running(2) << qint64(-1) << qint64(-1);
    QTest::newRow("remote machine") << "windows" << QString("mac") << running(2) << qint64(1) << qint64(-1);
    QTest::newRow("linux machine") << "linux" << QString() << running(2) << qint64(1) << qint64(-1);
    QTest::newRow("macos machine") << "macos" << QString() << running(2) << qint64(1) << qint64(-1);
}

void TestMachinePlatform::otherWindowsSession()
{
    QFETCH(QString, platform); QFETCH(QString, host); QFETCH(QJsonObject, localHost);
    QFETCH(qint64, own); QFETCH(qint64, expected);
    BoxState local; local.host = "workstation"; local.ok = true;
    local.metrics = QJsonObject{{"platform", platform}}; local.localHost = localHost;
    const auto session = own < 0 ? std::nullopt : std::optional<qint64>(own);
    QCOMPARE(LocalSessionHost::otherWindowsSession(local, host, session).value_or(-1), expected);
    // The injected session of the GUI gives the same answer.
    LocalSessionHost::setOwnWindowsSessionForTests(session);
    QCOMPARE(LocalSessionHost::otherWindowsSession(local, host).value_or(-1), expected);
    LocalSessionHost::resetOwnWindowsSessionForTests();
}

void TestMachinePlatform::ownWindowsSessionOnlyOnWindows()
{
    BoxState local; local.host = "workstation"; local.ok = true; local.metrics = QJsonObject{{"platform", "windows"}};
    local.localHost = QJsonObject{{"state", "running"}, {"pid", 4242}, {"windows_session", 7}};
#ifdef Q_OS_WIN
    const auto own = LocalSessionHost::ownWindowsSession(); QVERIFY(own.has_value());
    QCOMPARE(LocalSessionHost::otherWindowsSession(local, {}).value_or(-1), qint64(*own == 7 ? -1 : 7));
#else
    // Linux and macOS GUIs have no Windows session, so a launch never warns, even
    // when a box reports a Windows host in another session.
    QVERIFY(!LocalSessionHost::ownWindowsSession());
    QVERIFY(!LocalSessionHost::otherWindowsSession(local, {}));
    auto unreported = local; unreported.metrics = QJsonObject();
    QVERIFY(!LocalSessionHost::otherWindowsSession(unreported, {}));
#endif
    LocalSessionHost::setOwnWindowsSessionForTests(1);
    QCOMPARE(LocalSessionHost::ownWindowsSession().value_or(-1), qint64(1));
    QCOMPARE(LocalSessionHost::otherWindowsSession(local, {}).value_or(-1), qint64(7));
    LocalSessionHost::setOwnWindowsSessionForTests(std::nullopt);
    QVERIFY(!LocalSessionHost::ownWindowsSession());
    LocalSessionHost::resetOwnWindowsSessionForTests();
#ifndef Q_OS_WIN
    QVERIFY(!LocalSessionHost::ownWindowsSession());
#endif
}

QTEST_APPLESS_MAIN(TestMachinePlatform)
#include "test_machineplatform.moc"
