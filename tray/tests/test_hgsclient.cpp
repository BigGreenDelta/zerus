#include <QtTest>
#include <QFile>
#include <QFileDevice>
#include <QTemporaryDir>
#include <QTextStream>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QThread>
#include <QPointer>
#include <QUuid>
#include <atomic>
#include <memory>
#ifndef Q_OS_WIN
#include <fcntl.h>
#include <unistd.h>
#endif
#include "ChildProcess.h"
#include "FakeCli.h"
#include "FleetState.h"
#include "HgsClient.h"
#include "DownloadCache.h"
#include "ProcessRunner.h"
#include "SessionPresentation.h"
#include "NativeSessionCapabilities.h"

class TestHgsClient : public QObject {
    Q_OBJECT
    std::unique_ptr<QTemporaryDir> m_settings;
private slots:
    void initTestCase() {
        m_settings = std::make_unique<QTemporaryDir>(); QVERIFY(m_settings->isValid());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, m_settings->path());
    }
    void parsesBox();
    void parsesSharedReadState();
    void readMarksUseStdinAndAcceptOnlyHostAnswers();
    void cliVersionUsesConfiguredExecutableAndRefreshes();
    void parsesSavedSessions();
    void parsesActivityMetadata();
    void providerFailuresRequireAttentionAndPreserveSpecificStatus();
    void scheduledAgentsShowTheirNextRunInsteadOfReady();
    void checkoutLinesNameTheFolderBesideTheBranch();
    void questionReplyPreviews_data();
    void questionReplyPreviews();
    void unrecognizedQuestionPreviewsStayLiteral();
    void archiveIdentityAndCommands();
    void archiveInspectionKeepsDistinctIdentities();
    void renameCommands();
    void forkCommandsPinSourceIdentity();
    void preservesArgumentBytes();
    void parsesOfflineBox();
    void parsesControlOnlyComputer();
    void parsesLocalSessionHost();
    void parsesEndedLaunches();
    void sessionHostActionsUseJsonAndReportFailures();
    void parsesFleetArray();
    void rejectsGarbage();
    void tagAndProjectMayBeNull();
    void missingBinaryEmitsFailed();
    void duplicatePeerRequestIsIgnoredWhileInFlight();
    void peerAnswerIsKeyedByThePolledAlias();
    void localPollingReportsStartAndRecoversFromFailure();
    void presenceWritesAreCoalescedAndReportFailures();
    void sessionCommands();
    void inspectionCursorAndFailures();
    void remoteFoldersAndProjects();
    void parsesProjects();
    void rejectsGarbageProjects();
    void messagesUseStdinAndIdentityReceipts();
    void messageFailuresAreExplicitAndNeverRetried();
    void questionAnswersUsePinnedIdentityAndStdin();
    void submittedQuestionReceiptsRequireOptionalCodexAndExactIdentity();
    void questionAnswerFailures_data();
    void questionAnswerFailures();
    void unconfirmedQuestionCannotStartTransport();
    void effortUsesStdinAndPinnedIdentityReceipt();
    void settingsUseStdinAndPinnedIdentityReceipt();
    void settingsRejectMismatchedReceipt_data();
    void settingsRejectMismatchedReceipt();
    void openCodeSettingsErrorsDoNotNameTerminal();
    void effortFailures_data();
    void effortFailures();
    void effortLateResponsesKeepRequestIdentity();
    void terminationPinsRun();
    void nativeLaunchReportsProcessResult();
    void nativeApiSessionsRetainExactCapabilities();
    void openCodeQueuedRemovalUsesScopedSessionAction();
    void openCodeQueuedMessageIdsFollowSharedRule();
    void attachmentsUseCapturedIdentityAndValidateResponses();
    void downloadsRequireExactSavedFileReceipts();
    void cachedDownloadsPreserveRequestIdentityAndCleanFailures();
    void repeatedCachedDownloadUsesOnePersistentSnapshot();
    void cachedDownloadReuseCoalescesAndForceKeepsPriorCopy();
    void cachedDownloadFanoutStopsWhenOwnerIsDestroyed();
    void gitStatusReadsAreSharedAndScopedToMachineAndFolder();
    void pollsLaunchOffTheGuiThreadAndReportOnIt();
    void destroyedClientKillsItsPolls();
    void directChildrenSkipPipesTheRunnerIsCreating();
};

void TestHgsClient::downloadsRequireExactSavedFileReceipts()
{
    QTemporaryDir fixture; QVERIFY(fixture.isValid());
    const auto program = fixture.filePath("hgs"), destination = fixture.filePath("saved copy.txt");
    QFile saved(destination); QVERIFY(saved.open(QIODevice::WriteOnly)); saved.write("sample"); saved.close();
    QVERIFY(FakeCli::install(program, "#!/usr/bin/env python3\nimport sys\nprint(open(__file__+'.receipt').read())\n"));
    HgsClient client(FakeCli::program(program));
    QSignalSpy ready(&client, &HgsClient::fileDownloaded), failed(&client, &HgsClient::fileDownloadFailed);
    const QJsonObject exact{{"host", "remote"}, {"path", "/remote/file.txt"}, {"local_path", destination}, {"bytes", 6}};
    QList<QJsonObject> invalid{{}, exact, exact, exact, exact};
    invalid[1]["host"] = "other"; invalid[2]["path"] = "/other.txt";
    invalid[3]["local_path"] = fixture.filePath("other.txt"); invalid[4]["bytes"] = 0;
    auto reply = [&](const QJsonObject &value) { QFile response(program + ".receipt"); QVERIFY(response.open(QIODevice::WriteOnly)); response.write(QJsonDocument(value).toJson()); };
    for (const auto &value : invalid) {
        reply(value); const auto request = client.downloadFile("remote", "/remote/file.txt", destination);
        QTRY_COMPARE(failed.size(), 1); QCOMPARE(failed.takeFirst()[0].toULongLong(), request); QVERIFY(ready.isEmpty());
    }
    reply(exact); const auto request = client.downloadFile("remote", "/remote/file.txt", destination);
    QTRY_COMPARE(ready.size(), 1); QCOMPARE(ready.first()[0].toULongLong(), request); QCOMPARE(ready.first()[1].toString(), destination);
    saved.remove(); ready.clear();
    client.downloadFile("remote", "/remote/file.txt", destination); QTRY_COMPARE(failed.size(), 1); QVERIFY(ready.isEmpty());
#ifdef Q_OS_WIN
    // A downloaded shortcut is confirmed by its own size, even when its target is
    // missing here: Qt otherwise describes a .lnk by the file it points to.
    failed.clear(); const auto shortcut = fixture.filePath("remote.lnk"), target = fixture.filePath("elsewhere.txt");
    { QFile file(target); QVERIFY(file.open(QIODevice::WriteOnly)); }
    QVERIFY(QFile::link(target, shortcut)); QVERIFY(QFile::remove(target));
    QFile link(shortcut); QVERIFY(link.open(QIODevice::ReadOnly)); const auto size = link.size(); link.close(); QVERIFY(size > 0);
    reply({{"host", "remote"}, {"path", "/remote/remote.lnk"}, {"local_path", shortcut}, {"bytes", size}});
    const auto shortcutRequest = client.downloadFile("remote", "/remote/remote.lnk", shortcut);
    QTRY_COMPARE(ready.size(), 1); QCOMPARE(ready.first()[0].toULongLong(), shortcutRequest); QVERIFY(failed.isEmpty());
#endif
}

void TestHgsClient::cachedDownloadsPreserveRequestIdentityAndCleanFailures()
{
    QTemporaryDir dir; QVERIFY(dir.isValid());
    const auto root = dir.filePath("Zerus Files"), fixture = dir.filePath("hgs");
    QSettings settings; const auto oldFolder = settings.value("downloads/folder"), oldLimit = settings.value("downloads/cacheBytes");
    const auto restore = qScopeGuard([&] {
        if (oldFolder.isValid()) settings.setValue("downloads/folder", oldFolder); else settings.remove("downloads/folder");
        if (oldLimit.isValid()) settings.setValue("downloads/cacheBytes", oldLimit); else settings.remove("downloads/cacheBytes");
    });
    settings.setValue("downloads/folder", root); settings.setValue("downloads/cacheBytes", 0);
    QVERIFY(FakeCli::install(fixture, R"PY(#!/usr/bin/env python3
import sys,json,pathlib
a=sys.argv[1:];host=a[0][1:];path=a[a.index('--path')+1];target=a[a.index('--output')+1]
pathlib.Path(target).write_bytes(host.encode())
if path.endswith('fail.txt'): sys.exit(1)
print(json.dumps(dict(host=host,path=path,local_path=target,bytes=999 if path.endswith('bad.txt') else len(host))))
)PY"));
    HgsClient client(FakeCli::program(fixture));
    QSignalSpy ready(&client, &HgsClient::fileDownloaded), failed(&client, &HgsClient::fileDownloadFailed);
    const auto one = client.downloadFileToCache("one", "/remote/same.txt"), two = client.downloadFileToCache("two", "/other/same.txt");
    QTRY_COMPARE(ready.size(), 2); QVERIFY(failed.isEmpty());
    QSet<QString> names;
    for (const auto &receipt : ready) {
        const auto id = receipt[0].toULongLong(); const auto path = receipt[1].toString();
        QVERIFY(id == one || id == two); QCOMPARE(QFileInfo(path).absolutePath(), root);
        QFile file(path); QVERIFY(file.open(QIODevice::ReadOnly)); QCOMPARE(file.readAll(), id == one ? QByteArray("one") : QByteArray("two"));
        names.insert(path);
    }
    QCOMPARE(names.size(), 2); ready.clear();
    const auto broken = client.downloadFileToCache("one", "/remote/bad.txt"), failure = client.downloadFileToCache("one", "/remote/fail.txt");
    QTRY_COMPARE(failed.size(), 2); QVERIFY(ready.isEmpty());
    QSet<quint64> ids; for (const auto &receipt : failed) ids.insert(receipt[0].toULongLong());
    QCOMPARE(ids, (QSet<quint64>{broken, failure}));
    QTRY_VERIFY(QDir(root).entryList({".zerus-part-*"}, QDir::Files | QDir::Hidden).isEmpty());
    QCOMPARE(DownloadCache::usage(root).files, 2);
    // An unusable cache reports against the same ID before any transfer starts.
    const auto blocked = dir.filePath("not a folder"); QFile file(blocked); QVERIFY(file.open(QIODevice::WriteOnly)); file.close();
    settings.setValue("downloads/folder", blocked);
    const auto prepFailed = client.downloadFileToCache("one", "/remote/same.txt");
    QTRY_COMPARE(failed.size(), 3); QCOMPARE(failed.last()[0].toULongLong(), prepFailed); QVERIFY(ready.isEmpty());
}

void TestHgsClient::repeatedCachedDownloadUsesOnePersistentSnapshot()
{
    QTemporaryDir dir; QVERIFY(dir.isValid());
    const auto fixture = dir.filePath("hgs"), root = dir.filePath("cache");
    QSettings settings; const auto oldFolder = settings.value("downloads/folder"), oldLimit = settings.value("downloads/cacheBytes");
    const auto restore = qScopeGuard([&] {
        if (oldFolder.isValid()) settings.setValue("downloads/folder", oldFolder); else settings.remove("downloads/folder");
        if (oldLimit.isValid()) settings.setValue("downloads/cacheBytes", oldLimit); else settings.remove("downloads/cacheBytes");
    });
    settings.setValue("downloads/folder", root); settings.setValue("downloads/cacheBytes", 0);
    QVERIFY(FakeCli::install(fixture, R"PY(#!/usr/bin/env python3
import sys,json,pathlib
a=sys.argv[1:];host=a[0][1:];path=a[a.index('--path')+1];target=a[a.index('--output')+1]
with open(__file__+'.calls','a') as f:f.write('transfer\n')
pathlib.Path(target).write_bytes(b'snapshot')
print(json.dumps(dict(host=host,path=path,local_path=target,bytes=8)))
)PY"));
    QString local;
    for (int attempt = 0; attempt < 2; ++attempt) {
        HgsClient client(FakeCli::program(fixture));
        QSignalSpy ready(&client, &HgsClient::fileDownloaded), failed(&client, &HgsClient::fileDownloadFailed);
        const auto id = client.downloadFileToCache("remote", "/remote/same.txt");
        QTRY_COMPARE(ready.size(), 1); QVERIFY(failed.isEmpty()); QCOMPARE(ready[0][0].toULongLong(), id);
        QFile calls(fixture + ".calls"); QVERIFY(calls.open(QIODevice::ReadOnly));
        QCOMPARE(calls.readAll().count('\n'), 1);
        if (attempt == 0) local = ready[0][1].toString();
        else QCOMPARE(ready[0][1].toString(), local);
    }
}

void TestHgsClient::cachedDownloadReuseCoalescesAndForceKeepsPriorCopy()
{
    QTemporaryDir dir; QVERIFY(dir.isValid());
    const auto root = dir.filePath("cache"), fixture = dir.filePath("hgs");
    QSettings settings; const auto oldFolder = settings.value("downloads/folder"), oldLimit = settings.value("downloads/cacheBytes");
    const auto restore = qScopeGuard([&] {
        if (oldFolder.isValid()) settings.setValue("downloads/folder", oldFolder); else settings.remove("downloads/folder");
        if (oldLimit.isValid()) settings.setValue("downloads/cacheBytes", oldLimit); else settings.remove("downloads/cacheBytes");
    });
    settings.setValue("downloads/folder", root); settings.setValue("downloads/cacheBytes", 0);
    const auto write = [](const QString &path, const QByteArray &bytes) { QFile f(path); return f.open(QIODevice::WriteOnly) && f.write(bytes) == bytes.size(); };
    const auto read = [](const QString &path) { QFile f(path); return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray(); };
    QVERIFY(write(fixture + ".payload", "first snapshot"));
    QVERIFY(FakeCli::install(fixture, R"PY(#!/usr/bin/env python3
import sys,json,pathlib,time
a=sys.argv[1:];host=a[0][1:];path=a[a.index('--path')+1];target=a[a.index('--output')+1]
with open(__file__+'.calls','a') as f:f.write(json.dumps([host,path,target])+'\n')
pathlib.Path(__file__+'.started').touch()
deadline=time.monotonic()+10
while not pathlib.Path(__file__+'.release').exists():
 if time.monotonic()>=deadline:sys.exit(8)
 time.sleep(.01)
data=pathlib.Path(__file__+'.payload').read_bytes();pathlib.Path(target).write_bytes(data)
print(json.dumps(dict(host=host,path=path,local_path=target,bytes=len(data))))
)PY"));
    HgsClient client(FakeCli::program(fixture));
    const auto release = qScopeGuard([&] { write(fixture + ".release", "release"); });
    QSignalSpy ready(&client, &HgsClient::fileDownloaded), failed(&client, &HgsClient::fileDownloadFailed), checked(&client, &HgsClient::fileCacheChecked);
    const auto missing = client.findCachedFile("remote", "/remote/same.txt");
    QVERIFY(checked.isEmpty()); // Never signal inline, before callers can save the ID.
    QTRY_COMPARE(checked.size(), 1); QCOMPARE(checked[0][0].toULongLong(), missing);
    QVERIFY(checked[0][1].toString().isEmpty()); QVERIFY(checked[0][2].toString().isEmpty());
    const auto one = client.downloadFileToCache("remote", "/remote/same.txt");
    QTRY_VERIFY(QFileInfo::exists(fixture + ".started"));
    const auto two = client.downloadFileToCache("remote", "/remote/same.txt"); QVERIFY(one != two);
    QVERIFY(write(fixture + ".release", "release"));
    QTRY_COMPARE(ready.size(), 2); QVERIFY(failed.isEmpty());
    QCOMPARE((QSet<quint64>{ready[0][0].toULongLong(), ready[1][0].toULongLong()}), (QSet<quint64>{one, two}));
    const auto first = ready[0][1].toString(); QCOMPARE(ready[1][1].toString(), first);
    QCOMPARE(read(first), QByteArray("first snapshot"));
    QCOMPARE(read(fixture + ".calls").count('\n'), 1);
    ready.clear();
    const auto again = client.downloadFileToCache("remote", "/remote/same.txt");
    QTRY_COMPARE(ready.size(), 1); QCOMPARE(ready[0][0].toULongLong(), again); QCOMPARE(ready[0][1].toString(), first);
    QCOMPARE(read(fixture + ".calls").count('\n'), 1);
    checked.clear(); const auto local = client.findCachedFile("remote", "/remote/same.txt");
    QTRY_COMPARE(checked.size(), 1); QCOMPARE(checked[0][0].toULongLong(), local); QCOMPARE(checked[0][1].toString(), first);
    QVERIFY(checked[0][2].toString().isEmpty()); QCOMPARE(read(fixture + ".calls").count('\n'), 1);
    // A new client has no process-local request/cache state to reuse.
    { HgsClient restarted(FakeCli::program(fixture)); QSignalSpy restored(&restarted, &HgsClient::fileDownloaded);
      const auto id = restarted.downloadFileToCache("remote", "/remote/same.txt"); QTRY_COMPARE(restored.size(), 1);
      QCOMPARE(restored[0][0].toULongLong(), id); QCOMPARE(restored[0][1].toString(), first); }
    QCOMPARE(read(fixture + ".calls").count('\n'), 1);
    QVERIFY(write(fixture + ".payload", "new remote snapshot")); ready.clear();
    const auto cachedThenRefresh = client.downloadFileToCache("remote", "/remote/same.txt");
    const auto fresh = client.downloadFileToCache("remote", "/remote/same.txt", true);
    QTRY_COMPARE(ready.size(), 2);
    QCOMPARE((QSet<quint64>{ready[0][0].toULongLong(), ready[1][0].toULongLong()}), (QSet<quint64>{cachedThenRefresh, fresh}));
    const auto second = ready[0][1].toString(); QVERIFY(first != second);
    QCOMPARE(ready[1][1].toString(), second);
    QCOMPARE(read(first), QByteArray("first snapshot")); QCOMPARE(read(second), QByteArray("new remote snapshot"));
    QCOMPARE(read(fixture + ".calls").count('\n'), 2);
    // Save As always transfers, even with a valid cached snapshot.
    ready.clear(); const auto explicitPath = dir.filePath("explicit.txt");
    const auto explicitId = client.downloadFile("remote", "/remote/same.txt", explicitPath);
    QTRY_COMPARE(ready.size(), 1); QCOMPARE(ready[0][0].toULongLong(), explicitId); QCOMPARE(ready[0][1].toString(), explicitPath);
    QCOMPARE(read(fixture + ".calls").count('\n'), 3); QCOMPARE(read(explicitPath), QByteArray("new remote snapshot"));
    // An unreadable index never starts another payload, including explicit refresh.
    QVERIFY(write(QDir(root).filePath(".zerus-files.json"), "invalid index")); checked.clear();
    const auto corrupt = client.findCachedFile("remote", "/remote/same.txt");
    QTRY_COMPARE(checked.size(), 1); QCOMPARE(checked[0][0].toULongLong(), corrupt); QVERIFY(!checked[0][2].toString().isEmpty());
    ready.clear(); const auto blocked = client.downloadFileToCache("remote", "/remote/same.txt");
    const auto forced = client.downloadFileToCache("remote", "/remote/same.txt", true);
    QTRY_COMPARE(failed.size(), 2); QVERIFY(ready.isEmpty());
    QCOMPARE((QSet<quint64>{failed[0][0].toULongLong(), failed[1][0].toULongLong()}), (QSet<quint64>{blocked, forced}));
    QCOMPARE(read(fixture + ".calls").count('\n'), 3);
    QCOMPARE(read(first), QByteArray("first snapshot")); QCOMPARE(read(second), QByteArray("new remote snapshot"));
    failed.clear(); const auto forcedAlone = client.downloadFileToCache("remote", "/remote/same.txt", true);
    QTRY_COMPARE(failed.size(), 1); QCOMPARE(failed[0][0].toULongLong(), forcedAlone);
    QCOMPARE(read(fixture + ".calls").count('\n'), 3);
}

void TestHgsClient::cachedDownloadFanoutStopsWhenOwnerIsDestroyed()
{
    QTemporaryDir dir; QVERIFY(dir.isValid()); const auto fixture = dir.filePath("hgs");
    QSettings settings; const auto oldFolder = settings.value("downloads/folder"), oldLimit = settings.value("downloads/cacheBytes");
    const auto restore = qScopeGuard([&] {
        if (oldFolder.isValid()) settings.setValue("downloads/folder", oldFolder); else settings.remove("downloads/folder");
        if (oldLimit.isValid()) settings.setValue("downloads/cacheBytes", oldLimit); else settings.remove("downloads/cacheBytes");
    });
    settings.setValue("downloads/folder", dir.filePath("cache")); settings.setValue("downloads/cacheBytes", 0);
    QVERIFY(FakeCli::install(fixture, R"PY(#!/usr/bin/env python3
import sys,json,pathlib
a=sys.argv[1:];host=a[0][1:];path=a[a.index('--path')+1];target=a[a.index('--output')+1]
with open(__file__+'.calls','a') as f:f.write('transfer\n')
pathlib.Path(target).write_bytes(b'snapshot')
print(json.dumps(dict(host=host,path=path,local_path=target,bytes=8)))
)PY"));
    QPointer<HgsClient> client = new HgsClient(FakeCli::program(fixture));
    const auto cleanup = qScopeGuard([&] { delete client.data(); });
    QList<quint64> delivered; int failures = 0, warnings = 0;
    connect(client.data(), &HgsClient::fileDownloadFailed, this, [&](quint64, const QString &) { ++failures; });
    connect(client.data(), &HgsClient::downloadCacheWarning, this, [&](const QString &) { ++warnings; });
    connect(client.data(), &HgsClient::fileDownloaded, this, [&](quint64 id, const QString &) {
        delivered.append(id); delete client.data();
    });
    const auto first = client->downloadFileToCache("remote", "/remote/same.txt");
    const auto second = client->downloadFileToCache("remote", "/remote/same.txt"); QVERIFY(first != second);
    QTRY_VERIFY(client.isNull()); QCOMPARE(delivered, (QList<quint64>{first}));
    QCOMPARE(failures, 0); QCOMPARE(warnings, 0);
    QFile calls(fixture + ".calls"); QVERIFY(calls.open(QIODevice::ReadOnly)); QCOMPARE(calls.readAll().count('\n'), 1);
}

void TestHgsClient::cliVersionUsesConfiguredExecutableAndRefreshes()
{
    QTemporaryDir fixture;QVERIFY(fixture.isValid());
    const auto program=fixture.filePath("custom-hgs");
    HgsClient client(FakeCli::program(program));QSignalSpy ready(&client,&HgsClient::versionReady);
    client.requestVersion();QTRY_COMPARE(ready.size(),1);QVERIFY(ready[0][0].toString().isEmpty());
    QVERIFY(FakeCli::install(program,"#!/usr/bin/env python3\nimport sys\nif sys.argv[1:]!=['--version']:sys.exit(7)\n"
        "open(__file__+'.calls','a').write('call\\n')\nsys.stdout.write(open(__file__+'.version').read())\n"));
    auto reported=[&](const QByteArray &value){QFile file(program+".version");QVERIFY(file.open(QIODevice::WriteOnly));file.write(value);};
    reported("hgs 9.8.7\n");client.requestVersion();client.requestVersion();QTRY_COMPARE(ready.size(),2);
    QCOMPARE(ready[1][0].toString(),QString("9.8.7"));
    QFile calls(program+".calls");QVERIFY(calls.open(QIODevice::ReadOnly));QCOMPARE(calls.readAll(),QByteArray("call\n"));calls.close();
    reported("hgs 9.8.8-preview.1+fixture\n");client.requestVersion();QTRY_COMPARE(ready.size(),3);
    QCOMPARE(ready[2][0].toString(),QString("9.8.8-preview.1+fixture"));
    reported("An unrelated tool version 9.8.8\n");client.requestVersion();QTRY_COMPARE(ready.size(),4);
    QVERIFY(ready[3][0].toString().isEmpty());
}

void TestHgsClient::pollsLaunchOffTheGuiThreadAndReportOnIt()
{
    QTemporaryDir fixture; QVERIFY(fixture.isValid());
    const auto program = fixture.filePath("hgs");
    QVERIFY(FakeCli::install(program, "#!/usr/bin/env python3\nprint('{\"host\":\"arch\",\"ok\":true,\"sessions\":[]}')\n"));
    std::atomic<QThread *> launchThread{nullptr};
    ProcessRunner::setLaunchObserver([&launchThread] { launchThread = QThread::currentThread(); });
    HgsClient client(FakeCli::program(program)); QThread *readyThread = nullptr; int ready = 0;
    connect(&client, &HgsClient::localReady, this, [&](const BoxState &box) { readyThread = QThread::currentThread(); ready += box.host == "arch"; });
    client.requestLocal();
    QTRY_COMPARE(ready, 1);
    ProcessRunner::setLaunchObserver({});
    QVERIFY(launchThread.load() && launchThread.load() != QThread::currentThread());
    QCOMPARE(readyThread, QThread::currentThread());
}

void TestHgsClient::destroyedClientKillsItsPolls()
{
    QTemporaryDir fixture; QVERIFY(fixture.isValid());
    const auto program = fixture.filePath("hgs");
    // The pid of the started program: the script itself on Unix, the shim on Windows.
    QVERIFY(FakeCli::install(program, "#!/usr/bin/env python3\nimport os,time\n"
        "open(__file__+'.pid','w').write(os.environ.get('ZERUS_FAKE_PID',str(os.getpid()))+'\\n')\ntime.sleep(30)\n"));
    auto client = std::make_unique<HgsClient>(FakeCli::program(program)); int reports = 0;
    connect(client.get(), &HgsClient::peerReady, this, [&reports] { ++reports; });
    connect(client.get(), &HgsClient::stateReadFailed, this, [&reports] { ++reports; });
    client->requestPeer("arch");
    QFile pidFile(program + ".pid");
    QTRY_VERIFY(pidFile.exists() && pidFile.size() > 0);
    QVERIFY(pidFile.open(QIODevice::ReadOnly)); const auto pid = pidFile.readAll().trimmed().toLongLong(); QVERIFY(pid > 0);
    // The old destructor killed and reaped child QProcesses; owners of worker
    // launches keep that guarantee and receive no late report.
    client.reset();
    QVERIFY(!FakeCli::processAlive(pid));
    QTest::qWait(100);
    QCOMPARE(reports, 0);
}

void TestHgsClient::directChildrenSkipPipesTheRunnerIsCreating()
{
#ifdef Q_OS_WIN
    QSKIP("Unix descriptors only");
#else
    if (!ChildProcess::inheritsStdioOnly) QSKIP("This Qt cannot close inherited descriptors");
    QTemporaryDir fixture; QVERIFY(fixture.isValid());
    const auto program = fixture.filePath("hgs");
    QVERIFY(FakeCli::install(program, "#!/bin/sh\nls /dev/fd\n"));
    // On macOS the runner's worker makes a request's pipes with pipe() and marks
    // them close-on-exec only afterwards. Machine setup starts on the GUI thread
    // in between, through the configuration every direct hgs child shares.
    // High numbers keep the pipe apart from the descriptor the listing opens.
    int made[2] = {-1, -1}, creating[2] = {-1, -1}; QCOMPARE(::pipe(made), 0);
    for (int i = 0; i < 2; ++i) { creating[i] = ::fcntl(made[i], F_DUPFD, 64); ::close(made[i]); }
    HgsClient client(FakeCli::program(program)); QString output;
    connect(&client, &HgsClient::machineSetupOutput, this, [&output](const QString &text) { output += text; });
    QSignalSpy finished(&client, &HgsClient::machineSetupFinished);
    client.setupMachine("mac");
    QTRY_COMPARE(finished.size(), 1);
    for (int fd : creating) if (fd >= 0) ::close(fd);
    QVERIFY(creating[0] >= 64 && creating[1] >= 64);
    QVERIFY2(finished[0][0].toBool(), qPrintable(finished[0][1].toString()));
    const auto open = output.simplified().split(' ');
    QVERIFY2(open.contains("0") && open.contains("1"), qPrintable(output));
    // The setup child would otherwise hold that request's input open until it ends.
    for (int fd : creating) QVERIFY2(!open.contains(QString::number(fd)), qPrintable(output));
#endif
}

void TestHgsClient::gitStatusReadsAreSharedAndScopedToMachineAndFolder()
{
    QTemporaryDir fixture; QVERIFY(fixture.isValid());
    const auto program = fixture.filePath("hgs");
    QVERIFY(FakeCli::install(program, "#!/usr/bin/env python3\nimport sys,time\nopen(__file__+'.calls','a').write(' '.join(sys.argv[1:])+'\\n')\n"
        "time.sleep(0.1)\nprint('{\"state\":\"ok\",\"root\":\"/repo\",\"branch\":\"main\",\"changed_files\":4,\"ahead\":2,\"behind\":0,\"remote_state\":\"verified\",\"remote_age_seconds\":0}')\n"));
    HgsClient client(FakeCli::program(program)); QSignalSpy changed(&client, &HgsClient::gitStatusChanged);
    client.requestGitStatus({}, "/repo"); client.requestGitStatus({}, "/repo");
    client.requestGitStatus("peer", "/repo"); client.requestGitStatus("peer", "/repo");
    QVERIFY(client.gitStatusSnapshot({}, "/repo").isEmpty());
    QTRY_COMPARE(changed.size(), 2);
    QCOMPARE(client.gitStatusSnapshot({}, "/repo")["changed_files"].toInt(), 4);
    QCOMPARE(client.gitStatusSnapshot("peer", "/repo")["ahead"].toInt(), 2);
    QVERIFY(client.gitStatusSnapshot("other", "/repo").isEmpty());
    QVERIFY(client.gitStatusSnapshot({}, "/other").isEmpty());
    client.requestGitStatus({}, "/repo"); QTest::qWait(150);
    QFile calls(program + ".calls"); QVERIFY(calls.open(QIODevice::ReadOnly));
    const auto lines = calls.readAll(); QCOMPARE(lines.count('\n'), 2);
    QVERIFY(lines.contains("git-status --path /repo --json"));
    QVERIFY(lines.contains("@peer git-status --path /repo --json"));
}

void TestHgsClient::questionReplyPreviews_data()
{
    QTest::addColumn<QString>("prompt");
    QTest::addColumn<QString>("preview");
    const QString answer = "Keep **literal** <b>markup</b> & `code`\nReview the desktop changes.";
    const QJsonObject reply{{"questionItemId", "reply-one"}, {"question", QString(300, 'q')}, {"answer", answer}};
    const auto envelope = [](const QJsonDocument &document) {
        return "<send_user_message_question_reply>\n" + QString::fromUtf8(document.toJson(QJsonDocument::Compact))
            + "\n</send_user_message_question_reply>";
    };
    const QString expected = "Your answer: " + answer;
    QTest::newRow("object") << envelope(QJsonDocument(reply)) << expected;
    QTest::newRow("array") << envelope(QJsonDocument(QJsonArray{reply})) << expected;
    QTest::newRow("ide") << "# Context from my IDE setup:\nOpen files: src/example.rs\n## My request for Codex:\n"
        + envelope(QJsonDocument(QJsonArray{reply})) << expected;
    auto second = reply; second["questionItemId"] = "reply-two"; second["answer"] = "Include macOS";
    QTest::newRow("multiple") << envelope(QJsonDocument(QJsonArray{reply, second})) << "Your answers: " + answer + " / Include macOS";
    second["answer"] = "";
    QTest::newRow("empty-answer") << envelope(QJsonDocument(second)) << QString("Your answer: (empty answer)");
    second["answer"] = QString::fromUtf8("Résumé 東京 ") + QString(300, 'a');
    QTest::newRow("long-unicode-answer") << envelope(QJsonDocument(second)) << "Your answer: " + second["answer"].toString();
}

void TestHgsClient::questionReplyPreviews()
{
    QFETCH(QString, prompt); QFETCH(QString, preview);
    SessionInfo session;session.tracked=true;session.phase="working";session.activity="busy";
    session.prompt=prompt;session.activitySummary="Working";
    session.activityDetail=prompt.left(240)+QChar(0x2026);
    QCOMPARE(SessionPresentation::currentAction(session),"Working: "+preview);
    session.activityDetail=prompt;
    QCOMPARE(SessionPresentation::currentAction(session),"Working: "+preview);
    session.phase="input";
    QCOMPARE(SessionPresentation::currentAction(session),"Needs input: "+preview);
    session.phase="working";session.activitySummary.clear();session.activityDetail.clear();
    QCOMPARE(SessionPresentation::currentAction(session),"Working: "+preview);
    // A previous question reply must not replace a subsequent tool excerpt.
    session.activitySummary="Read";session.currentTool="Read";session.toolDetail="src/example.rs";session.activityDetail=session.toolDetail;
    QCOMPARE(SessionPresentation::currentAction(session),QString("Read: src/example.rs"));
    QCOMPARE(session.prompt,prompt);
}

void TestHgsClient::unrecognizedQuestionPreviewsStayLiteral()
{
    const QString start="<send_user_message_question_reply>\n";
    for(const auto &prompt:QStringList{"Review **these** changes.", start+"not json\n</send_user_message_question_reply>",
        start+"[{\"answer\":\"Unconfirmed excerpt", "Example: "+start+"[]\n</send_user_message_question_reply>"}) {
        SessionInfo session;session.tracked=true;session.phase="working";session.activity="busy";
        session.prompt=prompt;session.activitySummary="Working";session.activityDetail=prompt;
        QCOMPARE(SessionPresentation::currentAction(session),"Working: "+prompt);
        session.activitySummary.clear();session.activityDetail.clear();
        QCOMPARE(SessionPresentation::currentAction(session),"Working: "+prompt);
    }
}

void TestHgsClient::checkoutLinesNameTheFolderBesideTheBranch()
{
    const auto parse=[](const QJsonObject &fields) {
        QJsonObject raw{{"name","claude/zerus-work-1a2b/mobile"},{"project","zerus-work-1a2b"}};
        for(auto it=fields.begin();it!=fields.end();++it)raw[it.key()]=it.value();
        QString error;const auto box=HgsClient::parseBox(QJsonDocument(QJsonObject{{"host","local"},{"sessions",QJsonArray{raw}}}).toJson(),&error);
        return box.sessions.first();
    };
    const QJsonObject linked{{"cwd","/work/zerus-work-1a2b"},{"git_root","/work/zerus-work-1a2b"},{"git_branch","work-1a2b"},{"git_worktree",true},
        {"git_worktree_name","zerus-work-1a2b"},{"git_common_dir","/work/zerus/.git"},{"git_metadata_state","ok"}};
    auto session=parse(linked);
    QCOMPARE(SessionPresentation::checkoutLine(session),QString("zerus / work-1a2b"));
    QCOMPARE(SessionPresentation::checkoutIcon(session),QString("git-branch"));
    QCOMPARE(SessionPresentation::projectContext(session),QString("zerus / work-1a2b · worktree"));
    // Bare and dot-folder repositories, Windows paths, and detached checkouts.
    auto fields=linked;fields["git_common_dir"]="/srv/git/zerus.git";
    QCOMPARE(SessionPresentation::checkoutLine(parse(fields)),QString("zerus / work-1a2b"));
    fields["git_common_dir"]="/srv/zerus/.bare/";
    QCOMPARE(SessionPresentation::checkoutLine(parse(fields)),QString("zerus / work-1a2b"));
    fields["git_common_dir"]="C:\\Users\\user\\zerus\\.git";
    QCOMPARE(SessionPresentation::checkoutLine(parse(fields)),QString("zerus / work-1a2b"));
    fields["git_branch"]="";fields["git_detached"]=true;
    QCOMPARE(SessionPresentation::checkoutLine(parse(fields)),QString("zerus / detached HEAD"));
    // Without the common directory the worktree folder itself is the best evidence.
    fields=linked;fields.remove("git_common_dir");
    QCOMPARE(SessionPresentation::checkoutLine(parse(fields)),QString("zerus-work-1a2b / work-1a2b"));
    // A main checkout names its own folder; a subfolder still names the checkout.
    session=parse({{"cwd","/work/zerus/tray"},{"git_root","/work/zerus"},{"git_branch","main"},{"git_common_dir","/work/zerus/.git"},{"git_metadata_state","ok"}});
    QCOMPARE(SessionPresentation::checkoutLine(session),QString("zerus / main"));
    QCOMPARE(SessionPresentation::projectContext(session),QString("zerus / main"));
    // Ordinary folders show their name with a folder icon.
    session=parse({{"cwd","/home/user/notes/"},{"git_metadata_state","not_repo"}});
    QCOMPARE(SessionPresentation::checkoutLine(session),QString("notes"));
    QCOMPARE(SessionPresentation::checkoutIcon(session),QString("folder"));
    session=parse({});
    QVERIFY(SessionPresentation::checkoutLine(session).isEmpty());QVERIFY(SessionPresentation::checkoutIcon(session).isEmpty());
    QCOMPARE(SessionPresentation::projectContext(session),QString("zerus-work-1a2b"));
}

void TestHgsClient::scheduledAgentsShowTheirNextRunInsteadOfReady()
{
    const auto next = QDateTime::currentDateTime().addSecs(600);
    QString error;
    auto box=HgsClient::parseBox(QJsonDocument(QJsonObject{{"host","local"},{"sessions",QJsonArray{QJsonObject{
        {"name","kimi/project/watch"},{"tracked",true},{"activity","idle"},{"phase","idle"},{"process_state","running"},
        {"schedules",QJsonObject{{"count",2},{"recurring",1},{"next_at",double(next.toSecsSinceEpoch())}}}}}}}).toJson(),&error);
    QVERIFY(error.isEmpty());
    auto session=box.sessions.first();
    QCOMPARE((QList<int>{session.scheduledJobs,session.recurringSchedules}),(QList<int>{2,1}));
    const auto time=SessionPresentation::nextScheduledRun(session);
    QVERIFY(!time.isEmpty());
    QCOMPARE(SessionPresentation::status(session),"Scheduled · "+time);
    QCOMPARE(SessionPresentation::currentAction(session),"Next run at "+time);
    QVERIFY(SessionPresentation::scheduled(session)); QVERIFY(SessionPresentation::active(session));
    QVERIFY(!SessionPresentation::working(session)); QVERIFY(!SessionPresentation::needsAttention(session));
    QVERIFY(!SessionPresentation::scheduled(session,false)); QCOMPARE(SessionPresentation::status(session,false),QString("Offline"));
    session.nextScheduledAt=QDateTime::currentSecsSinceEpoch()-5;
    QCOMPARE(SessionPresentation::status(session),QString("Scheduled · due"));
    QCOMPARE(SessionPresentation::currentAction(session),QString("Next run due"));
    session.nextScheduledAt=0;
    QCOMPARE(SessionPresentation::status(session),QString("Scheduled"));
    // A question or work outranks the schedule; no jobs or an unknown journal is Ready.
    session.phase="input"; QCOMPARE(SessionPresentation::status(session),QString("Needs input")); session.phase="idle";
    session.activity="busy"; QCOMPARE(SessionPresentation::status(session),QString("Working")); session.activity="idle";
    session.scheduledJobs=0; QCOMPARE(SessionPresentation::status(session),QString("Ready"));
    session.scheduledJobs=-1; QCOMPARE(SessionPresentation::status(session),QString("Ready")); QVERIFY(!SessionPresentation::active(session));
}

void TestHgsClient::providerFailuresRequireAttentionAndPreserveSpecificStatus()
{
    QString error;
    auto box=HgsClient::parseBox(R"({"host":"local","sessions":[{"name":"codex/project/task","tracked":true,"activity":"attention","phase":"error","process_state":"running","provider_error":{"error_kind":"quota","detail":"7d limit reached"}}]})",&error);
    QVERIFY(error.isEmpty());QCOMPARE(box.sessions.size(),1);
    auto session=box.sessions.first();
    QVERIFY(session.needsAttention());QCOMPARE(SessionPresentation::status(session),QString("Usage limit reached"));
    session.activity="busy"; // Old tool metadata must not take precedence.
    QCOMPARE(SessionPresentation::status(session),QString("Usage limit reached"));
    session.recovery={{"state","waiting"},{"due_at",QDateTime::currentSecsSinceEpoch()+30}};
    QVERIFY(session.needsAction());QCOMPARE(SessionPresentation::status(session),QString("Usage limit reached"));
    session.recovery={};
    session.providerError={{"error_kind","provider_policy"},{"detail","Request flagged by provider policy for authorized security testing"}};
    session.activityDetail=session.providerError.value("detail").toString();
    QVERIFY(session.needsAction());QCOMPARE(SessionPresentation::status(session),QString("Request blocked by provider"));
    QCOMPARE(SessionPresentation::currentAction(session),QString("Request blocked by provider: ")+session.activityDetail);
    QCOMPARE(session.providerError.value("detail").toString(),QString("Request flagged by provider policy for authorized security testing"));
    session.recovery={{"state","waiting"},{"due_at",QDateTime::currentSecsSinceEpoch()+30}};
    QVERIFY(session.needsAttention());QVERIFY(session.needsAction());
    QCOMPARE(SessionPresentation::status(session),QString("Request blocked by provider"));
    session.recovery={};
    session.providerError={{"error_kind","authentication"}};
    QCOMPARE(SessionPresentation::status(session),QString("Sign-in failed"));
    session.phase="working";session.providerError={};session.recovery={};
    QVERIFY(!session.needsAction());QCOMPARE(SessionPresentation::status(session),QString("Working"));
    session.runId="run-one";session.conversationId="conversation-one";session.lastEventAt=100;
    QJsonObject details{{"tracked",true},{"run_id","run-one"},{"conversation_id","conversation-one"},
        {"last_event_at",101},{"phase","error"},{"activity","attention"},{"provider_error",QJsonObject{{"error_kind","quota"}}}};
    QCOMPARE(SessionPresentation::status(SessionPresentation::inspected(session,details)),QString("Usage limit reached"));
    details["last_event_at"]=99;
    QCOMPARE(SessionPresentation::status(SessionPresentation::inspected(session,details)),QString("Working"));
    details["last_event_at"]=101;details["run_id"]="another-run";
    QCOMPARE(SessionPresentation::status(SessionPresentation::inspected(session,details)),QString("Working"));
    details["run_id"]="run-one";details["conversation_id"]="another-conversation";
    QCOMPARE(SessionPresentation::status(SessionPresentation::inspected(session,details)),QString("Working"));
    details["conversation_id"]="conversation-one";session.providerStatusAt=102;details["provider_status_at"]=101;
    QCOMPARE(SessionPresentation::status(SessionPresentation::inspected(session,details)),QString("Working"));
}

void TestHgsClient::submittedQuestionReceiptsRequireOptionalCodexAndExactIdentity()
{
    QTemporaryDir directory;QFile script(directory.filePath("hgs"));QVERIFY(script.open(QIODevice::WriteOnly));
    script.write(R"(#!/usr/bin/env python3
import json,sys
p=json.load(sys.stdin)
receipt={'status':'submitted','request_id':p['request_id'],'name':sys.argv[-2],
 'run_id':p['expected_run_id'],'conversation_id':p['expected_conversation_id'],
 'question_id':p['question_id'],'question_hash':p['expected_question_hash']}
mode=p['answers'][0].get('text','')
if mode in receipt:receipt[mode]='unrelated'
print(json.dumps(receipt))
)");script.close();QVERIFY(FakeCli::makeRunnable(script.fileName()));
    HgsClient client(FakeCli::program(script.fileName()));QSignalSpy submitted(&client,&HgsClient::questionAnswerSubmitted),
        answered(&client,&HgsClient::questionAnswered),failed(&client,&HgsClient::questionAnswerFailed);
    const QJsonObject question{{"question_id","pending-question"},{"question_hash","exact-content"},
        {"run_id","run-one"},{"conversation_id","conversation-one"},{"can_answer",true},
        {"can_skip",true},{"optional",true},{"source","codex_async"}};
    const auto send=[&](const QJsonObject &q,const QJsonObject &answer) {
        return client.requestAnswerQuestion({},"codex/project/main",q,QJsonArray{answer});
    };
    const auto requestId=send(question,QJsonObject{{"question_id","q_0"},{"text","Saved answer"}});
    QTRY_COMPARE(submitted.size(),1);QCOMPARE(submitted[0][0].toULongLong(),requestId);QCOMPARE(answered.size(),0);QCOMPARE(failed.size(),0);
    int failures=0;
    for(const auto &field:{"request_id","name","run_id","conversation_id","question_id","question_hash"}) {
        send(question,QJsonObject{{"question_id","q_0"},{"text",field}});++failures;QTRY_COMPARE(failed.size(),failures);
    }
    auto unsupported=question;unsupported["source"]="codex_tui";
    send(unsupported,QJsonObject{{"question_id","q_0"},{"text","answer"}});++failures;QTRY_COMPARE(failed.size(),failures);
    unsupported=question;unsupported["optional"]=false;
    send(unsupported,QJsonObject{{"question_id","q_0"},{"text","answer"}});++failures;QTRY_COMPARE(failed.size(),failures);
    send(question,QJsonObject{{"question_id","q_0"},{"skip",true}});++failures;QTRY_COMPARE(failed.size(),failures);
    QCOMPARE(submitted.size(),1);QCOMPARE(answered.size(),0);
}

void TestHgsClient::attachmentsUseCapturedIdentityAndValidateResponses()
{
    QTemporaryDir directory;QFile script(directory.filePath("hgs"));QVERIFY(script.open(QIODevice::WriteOnly));
    script.write(R"(#!/usr/bin/env python3
import json,pathlib,sys
args=sys.argv[1:]
pathlib.Path(__file__).with_name('attachment.json').write_text(json.dumps(args))
r=args[args.index('--request')+1]
print(json.dumps({'request_id':r if r!='wrong' else 'other','index':0,'name':'notes.txt','mime':'text/plain','data_base64':'bad!' if r=='bad' else 'aGVsbG8='}))
)");script.close();QVERIFY(FakeCli::makeRunnable(script.fileName()));
    HgsClient client(FakeCli::program(script.fileName()));QSignalSpy ready(&client,&HgsClient::attachmentReady),failed(&client,&HgsClient::attachmentFailed);
    const QString name="kimi/project '$HOME' `literal`";
    const auto id=client.requestAttachment("mac",name,{{"request_id","one"},{"index",0}},"conversation","archive","child");
    QTRY_COMPARE(ready.size(),1);QCOMPARE(ready[0][0].toULongLong(),id);QCOMPARE(ready[0][2].toByteArray(),QByteArray("hello"));
    QFile args(directory.filePath("attachment.json"));QVERIFY(args.open(QIODevice::ReadOnly));
    QCOMPARE(QJsonDocument::fromJson(args.readAll()).array(),QJsonArray({"@mac","attachment",name,"--request","one","--index","0","--conversation","conversation","--archive","archive","--agent","child"}));
    client.requestAttachment({},name,{{"request_id","wrong"},{"index",0}},"conversation");QTRY_COMPARE(failed.size(),1);
    client.requestAttachment({},name,{{"request_id","bad"},{"index",0}},"conversation");QTRY_COMPARE(failed.size(),2);QCOMPARE(ready.size(),1);
}

void TestHgsClient::terminationPinsRun()
{
    QTemporaryDir directory;QFile script(directory.filePath("hgs"));QVERIFY(script.open(QIODevice::WriteOnly));
    script.write("#!/usr/bin/env python3\nimport sys\nfor a in sys.argv[1:]:print(a,file=sys.stderr)\n");script.close();
    QVERIFY(FakeCli::makeRunnable(script.fileName()));
    HgsClient client(FakeCli::program(script.fileName()));QSignalSpy result(&client,&HgsClient::writeDone);
    client.terminateSession("mac","codex/project/one","exact-run");QTRY_COMPARE(result.size(),1);QVERIFY(result[0][1].toBool());
    QCOMPARE(result[0][2].toString(),QString("@mac\nterminate\ncodex/project/one\n--expected-run-id\nexact-run"));
}

void TestHgsClient::nativeLaunchReportsProcessResult()
{
    QTemporaryDir directory;QFile script(directory.filePath("hgs"));QVERIFY(script.open(QIODevice::WriteOnly));
    script.write(R"(#!/usr/bin/env python3
import json,pathlib,sys
pathlib.Path(__file__).with_name('launch.json').write_text(json.dumps(sys.argv[1:]))
if 'fail' in sys.argv:
 print('Official DeepSeek host failed to start',file=sys.stderr);sys.exit(1)
print('DeepSeek session ready')
)");
    script.close();QVERIFY(FakeCli::makeRunnable(script.fileName()));
    HgsClient client(FakeCli::program(script.fileName()));QSignalSpy result(&client,&HgsClient::nativeSessionLaunched);
    const QString folder=QString::fromUtf8("/work/проект '$HOME' `literal`");
    client.launchNativeSession("mac",folder,"test","native-dsh","launch-one");
    QTRY_COMPARE(result.size(),1);QCOMPARE(result[0][0].toString(),QString("launch-one"));QVERIFY(result[0][1].toBool());
    QFile args(directory.filePath("launch.json"));QVERIFY(args.open(QIODevice::ReadOnly));
    QCOMPARE(QJsonDocument::fromJson(args.readAll()).array(),QJsonArray({"@mac","dsh",folder,"--new","-n","test","--launch-id","launch-one","--account","native-dsh"}));
    client.launchNativeSession({},folder,"fail",{},"launch-two");
    QTRY_COMPARE(result.size(),2);QCOMPARE(result[1][0].toString(),QString("launch-two"));QVERIFY(!result[1][1].toBool());
    QVERIFY(result[1][2].toString().contains("Official DeepSeek host failed"));
    args.close();
    client.launchDetachedSession("mac","codex",folder,"background","work","launch-three");
    QTRY_COMPARE(result.size(),3);QVERIFY(result[2][1].toBool());QVERIFY(args.open(QIODevice::ReadOnly));
    QCOMPARE(QJsonDocument::fromJson(args.readAll()).array(),QJsonArray({"@mac","codex",folder,"--new","-n","background","--launch-id","launch-three","-d","--account","work"}));
}


void TestHgsClient::nativeApiSessionsRetainExactCapabilities()
{
    QTemporaryDir directory;const auto program=directory.filePath("hgs");
    QVERIFY(FakeCli::install(program,"#!/usr/bin/env python3\nimport json,sys\nopen(__file__+'.args','w').write(json.dumps(sys.argv[1:]))\n"
        "print('{\"host\":\"fixture\",\"ok\":true,\"sessions\":[{\"name\":\"opencode/project/local\",\"cmd\":\"opencode\",\"tracked\":true,\"model\":\"ollama/qwen3:8b\",\"terminal_supported\":false,\"terminal_reason\":\"Native API session\",\"allowed_actions\":[\"pause\",\"rename\"]},{\"name\":\"opencode/project/legacy\",\"cmd\":\"opencode\",\"terminal_supported\":true}]}')\n"));
    HgsClient client(FakeCli::program(program));QSignalSpy ready(&client,&HgsClient::localReady);
    client.requestLocal();QTRY_COMPARE(ready.size(),1);
    const auto session=qvariant_cast<BoxState>(ready[0][0]).sessions.first();
    QCOMPARE(session.cmd,QString("opencode"));QCOMPARE(session.model,QString("ollama/qwen3:8b"));
    QVERIFY(!session.bulkLifecycleSupported);QVERIFY(!NativeSessionCapabilities::terminal(session));QCOMPARE(NativeSessionCapabilities::terminalReason(session),QString("Native API session"));
    QVERIFY(NativeSessionCapabilities::action(session,"pause"));QVERIFY(!NativeSessionCapabilities::action(session,"archive"));
    QVERIFY(!NativeSessionCapabilities::action(session,"resume"));
    const auto rows=qvariant_cast<BoxState>(ready[0][0]).sessions;QCOMPARE(rows.size(),2);
    auto legacy=rows.last();QVERIFY(legacy.terminalReported);QVERIFY(legacy.terminalSupported);QVERIFY(NativeSessionCapabilities::terminal(legacy));
    // OpenCode 2 has no process to pause: without reported actions only the
    // binding operations remain, and queued removal needs reported evidence.
    QVERIFY(!legacy.actionsReported);
    for(const auto *operation:{"rename","forget"})QVERIFY2(NativeSessionCapabilities::action(legacy,operation),operation);
    for(const auto *operation:{"pause","resume","archive","restore","terminate","fork","remove_queued"})QVERIFY2(!NativeSessionCapabilities::action(legacy,operation),operation);
    QVERIFY(NativeSessionCapabilities::action(legacy,"remove_queued",{{"allowed_actions",QJsonArray{"forget","remove_queued"}}}));
    legacy.terminalReported=false;QVERIFY(!NativeSessionCapabilities::terminal(legacy));
    QVERIFY(NativeSessionCapabilities::terminal(legacy,{{"terminal_supported",true}}));
    legacy.cmd="dsh";QVERIFY(!NativeSessionCapabilities::terminal(legacy,{{"terminal_supported",true}}));
    QSignalSpy launched(&client,&HgsClient::nativeSessionLaunched);
    const auto model=QString("ollama/qwen3:8b");client.launchDetachedSession("remote","opencode","/fixture/folder","local","native-opencode","launch-local",model);
    QTRY_COMPARE(launched.size(),1);QVERIFY(launched[0][1].toBool());
    QFile args(program+".args");QVERIFY(args.open(QIODevice::ReadOnly));
    QCOMPARE(QJsonDocument::fromJson(args.readAll()).array(),QJsonArray({"@remote","opencode","/fixture/folder","--new","-n","local","--launch-id","launch-local","--account","native-opencode","--model",model}));
    args.close();
    // Effort is the chosen model's variant, so it travels only with that model.
    client.launchDetachedSession("remote","opencode","/fixture/folder","deep","native-opencode","launch-deep",model,"high");
    QTRY_COMPARE(launched.size(),2);QVERIFY(launched[1][1].toBool());QVERIFY(args.open(QIODevice::ReadOnly));
    QCOMPARE(QJsonDocument::fromJson(args.readAll()).array(),QJsonArray({"@remote","opencode","/fixture/folder","--new","-n","deep","--launch-id","launch-deep","--account","native-opencode","--model",model,"--effort","high"}));
    args.close();
    client.launchDetachedSession({},"opencode","/fixture/folder","plain","native-opencode","launch-plain",{},"high");
    QTRY_COMPARE(launched.size(),3);QVERIFY(args.open(QIODevice::ReadOnly));
    QCOMPARE(QJsonDocument::fromJson(args.readAll()).array(),QJsonArray({"opencode","/fixture/folder","--new","-n","plain","--launch-id","launch-plain","--account","native-opencode"}));
}

void TestHgsClient::openCodeQueuedRemovalUsesScopedSessionAction()
{
    QTemporaryDir directory;const auto program=directory.filePath("hgs");
    QVERIFY(FakeCli::install(program,"#!/usr/bin/env python3\nimport json,os,sys\n"
        "p=json.load(sys.stdin);a=sys.argv[1:]\n"
        "open(__file__+'.call','w').write(json.dumps({'args':a,'payload':p}))\n"
        "status=open(__file__+'.status').read().strip() if os.path.exists(__file__+'.status') else 'completed'\n"
        "if status=='exit':sys.exit(3)\n"
        "r={'request_id':p['request_id'],'name':a[-2],'run_id':p['expected_run_id'],'conversation_id':p['expected_conversation_id'],'status':status}\n"
        "if status!='completed' and status!='submitted':r['error']='This message is no longer queued'\n"
        "print(json.dumps(r))\n"));
    HgsClient client(FakeCli::program(program));QSignalSpy finished(&client,&HgsClient::sessionActionFinished);
    const auto call=[&]{QFile file(program+".call");return file.open(QIODevice::ReadOnly)?QJsonDocument::fromJson(file.readAll()).object():QJsonObject();};
    const auto status=[&](const QByteArray &value){QVERIFY(FakeCli::replace(program+".status",value));};
    const QString name="opencode/project/local";
    auto request=client.requestRemoveQueued("mac",name,"run-one","ses_conversation","msg_Queued01");
    QTRY_COMPARE(finished.size(),1);QCOMPARE(finished[0][0].toULongLong(),request);QVERIFY2(finished[0][1].toBool(),qPrintable(finished[0][3].toString()));
    auto sent=call();QCOMPARE(sent["args"].toArray(),QJsonArray({"@mac","session-action",name,"--json"}));
    auto payload=sent["payload"].toObject();
    QCOMPARE(payload.keys(),QStringList({"action","expected_conversation_id","expected_run_id","message_id","request_id"}));
    QCOMPARE(payload["action"].toString(),QString("remove_queued"));QCOMPARE(payload["message_id"].toString(),QString("msg_Queued01"));
    QCOMPARE(payload["expected_run_id"].toString(),QString("run-one"));QCOMPARE(payload["expected_conversation_id"].toString(),QString("ses_conversation"));
    QVERIFY(!QUuid::fromString(payload["request_id"].toString()).isNull());
    // A durable failed or uncertain receipt is not a removal.
    for(const auto *outcome:{"failed","uncertain"}) {
        status(outcome);finished.clear();request=client.requestRemoveQueued({},name,"run-one","ses_conversation","msg_Queued01");
        QTRY_COMPARE(finished.size(),1);QCOMPARE(finished[0][0].toULongLong(),request);QVERIFY2(!finished[0][1].toBool(),outcome);
        QVERIFY2(finished[0][3].toString().contains("This message is no longer queued"),qPrintable(finished[0][3].toString()));
        QCOMPARE(call()["args"].toArray(),QJsonArray({"session-action",name,"--json"}));
    }
    // Only an exact native message ID is sent.
    QVERIFY(QFile::remove(program+".call"));finished.clear();
    for(const auto *id:{"","queued-1","msg_has space","msg_\n"}) {
        request=client.requestRemoveQueued({},name,"run-one","ses_conversation",id);
        QTRY_COMPARE(finished.size(),1);QCOMPARE(finished[0][0].toULongLong(),request);QVERIFY(!finished[0][1].toBool());finished.clear();
    }
    QTest::qWait(100);QVERIFY(!QFile::exists(program+".call"));
    // OpenCode actions have no Terminal to check.
    for(const auto *outcome:{"bogus","exit"}) {
        status(outcome);
        for(const auto forget:{false,true}) {
            request=forget?client.requestForgetBinding({},name,"run-one","ses_conversation"):client.requestRemoveQueued({},name,"run-one","ses_conversation","msg_Queued01");
            QTRY_COMPARE(finished.size(),1);QCOMPARE(finished[0][0].toULongLong(),request);QVERIFY(!finished[0][1].toBool());
            const auto error=finished[0][3].toString();QVERIFY2(!error.contains("Terminal")&&error.contains("Check the session before retrying"),qPrintable(error));
            finished.clear();
        }
    }
    QCOMPARE(call()["payload"].toObject()["action"].toString(),QString("forget"));
    // Other actions keep their own command and payload.
    status("submitted");request=client.requestInterrupt({},name,"run-one","ses_conversation",12.5);
    QTRY_COMPARE(finished.size(),1);QVERIFY(finished[0][1].toBool());
    sent=call();QCOMPARE(sent["args"].toArray(),QJsonArray({"interrupt",name,"--json"}));
    QVERIFY(!sent["payload"].toObject().contains("action"));QVERIFY(!sent["payload"].toObject().contains("message_id"));
}

// The shared message ID shape (tests/fixtures/opencode-queued-message-ids.json),
// which hgs, the mobile connector and the relay check too.
void TestHgsClient::openCodeQueuedMessageIdsFollowSharedRule()
{
    QFile file(QStringLiteral(ZERUS_OPENCODE_QUEUED_MESSAGE_IDS));QVERIFY(file.open(QIODevice::ReadOnly));
    const auto fixture=QJsonDocument::fromJson(file.readAll()).object();
    const auto accept=fixture.value("accept").toArray(),reject=fixture.value("reject").toArray();QVERIFY(accept.size()>=7);QVERIFY(reject.size()>=23);
    QTemporaryDir directory;const auto program=directory.filePath("hgs");
    QVERIFY(FakeCli::install(program,"#!/usr/bin/env python3\nimport json,sys\n"
        "p=json.load(sys.stdin);a=sys.argv[1:]\n"
        "open(__file__+'.call','w').write(json.dumps({'args':a,'payload':p}))\n"
        "print(json.dumps({'request_id':p['request_id'],'name':a[-2],'run_id':p['expected_run_id'],"
        "'conversation_id':p['expected_conversation_id'],'status':'completed'}))\n"));
    HgsClient client(FakeCli::program(program));QSignalSpy finished(&client,&HgsClient::sessionActionFinished);
    const QString name="opencode/project/local";
    for(const auto &value:reject) {
        const auto id=value.toString();finished.clear();
        const auto request=client.requestRemoveQueued({},name,"run-one","ses_conversation",id);
        QTRY_COMPARE(finished.size(),1);QCOMPARE(finished[0][0].toULongLong(),request);QVERIFY2(!finished[0][1].toBool(),qPrintable(id));
        QCOMPARE(finished[0][3].toString(),QString("Choose the exact queued OpenCode message."));
    }
    QTest::qWait(100);QVERIFY(!QFile::exists(program+".call"));
    for(const auto &value:accept) {
        const auto id=value.toString();finished.clear();
        const auto request=client.requestRemoveQueued({},name,"run-one","ses_conversation",id);
        QTRY_COMPARE(finished.size(),1);QCOMPARE(finished[0][0].toULongLong(),request);QVERIFY2(finished[0][1].toBool(),qPrintable(id));
        QFile call(program+".call");QVERIFY(call.open(QIODevice::ReadOnly));
        QCOMPARE(QJsonDocument::fromJson(call.readAll()).object()["payload"].toObject()["message_id"].toString(),id);
        call.close();QVERIFY(QFile::remove(program+".call"));
    }
}

void TestHgsClient::settingsUseStdinAndPinnedIdentityReceipt()
{
    QTemporaryDir directory;
    QFile script(directory.filePath("hgs")); QVERIFY(script.open(QIODevice::WriteOnly));
    script.write(R"(#!/usr/bin/env python3
import json,pathlib,sys
p=json.load(sys.stdin)
pathlib.Path(__file__).with_name('settings.json').write_text(json.dumps({'argv':sys.argv[1:],'payload':p}))
print(json.dumps({'status':'scheduled','request_id':p['request_id'],'name':sys.argv[-2],
 'run_id':p['expected_run_id'],'conversation_id':p['expected_conversation_id'],
 'effort':p['effort'],'model':'gpt-6-astra','scope':'session'}))
)");
    script.close(); QVERIFY(FakeCli::makeRunnable(script.fileName()));
    HgsClient client(FakeCli::program(script.fileName()));
    QSignalSpy finished(&client, &HgsClient::settingsFinished), refreshed(&client, &HgsClient::sessionWriteDone);
    const QString name = QString::fromUtf8("codex/проект/Новый '$HOME' `literal`");
    const quint64 request = client.requestSettings("mac", name, "gpt-6-astra", "high", "run-identity", "conversation-identity", "pending-original");
    QTRY_COMPARE(finished.size(), 1); QCOMPARE(finished[0][0].toULongLong(), request);
    QVERIFY(finished[0][1].toBool()); QVERIFY(finished[0][3].toString().isEmpty());
    const auto receipt = finished[0][2].toJsonObject();
    QCOMPARE(receipt["name"].toString(), name); QCOMPARE(receipt["effort"].toString(), QString("high"));
    QCOMPARE(receipt["scope"].toString(), QString("session"));
    QCOMPARE(refreshed.size(), 1); QCOMPARE(refreshed[0][0].toString(), QString("mac"));
    QFile captured(directory.filePath("settings.json")); QVERIFY(captured.open(QIODevice::ReadOnly));
    const auto record = QJsonDocument::fromJson(captured.readAll()).object();
    QCOMPARE(record["argv"].toArray(), QJsonArray({"@mac", "settings", name, "--json"}));
    const auto payload = record["payload"].toObject();
    QCOMPARE(payload["expected_run_id"].toString(), QString("run-identity"));
    QCOMPARE(payload["expected_conversation_id"].toString(), QString("conversation-identity"));
    QCOMPARE(payload["effort"].toString(), QString("high"));
    QCOMPARE(payload["model"].toString(), QString("gpt-6-astra"));
    QCOMPARE(payload["expected_pending_id"].toString(), QString("pending-original"));
    QCOMPARE(payload["request_id"], receipt["request_id"]);
    QVERIFY(!payload["request_id"].toString().isEmpty());
}

void TestHgsClient::settingsRejectMismatchedReceipt_data()
{
    QTest::addColumn<QString>("mode");
    for (const QString field : {"request_id", "name", "run_id", "conversation_id", "effort", "model", "status",
                                "stale", "killed", "malformed", "array"})
        QTest::newRow(field.toUtf8()) << field;
}

// OpenCode model and effort changes have no Terminal to check.
void TestHgsClient::openCodeSettingsErrorsDoNotNameTerminal()
{
    QTemporaryDir directory;const auto program=directory.filePath("hgs");
    QVERIFY(FakeCli::install(program,"#!/usr/bin/env python3\nimport json,os,sys\np=json.load(sys.stdin)\n"
        "mode=open(__file__+'.mode').read().strip()\n"
        "if mode=='exit':sys.exit(3)\n"
        "print(json.dumps({'status':'applied','request_id':'other'}))\n"));
    HgsClient client(FakeCli::program(program));QSignalSpy finished(&client,&HgsClient::settingsFinished);
    for(const auto *mode:{"exit","ack"}) {
        QVERIFY(FakeCli::replace(program+".mode",mode));
        for(const auto openCode:{true,false}) {
            finished.clear();
            client.requestSettings({},openCode?"opencode/project/local":"codex/project/local","fixture/local","high","run","conversation",{},openCode);
            QTRY_COMPARE(finished.size(),1);QVERIFY(!finished[0][1].toBool());const auto error=finished[0][3].toString();
            if(openCode)QVERIFY2(!error.contains("Terminal")&&error.contains("Check the session before retrying"),qPrintable(error));
            else QVERIFY2(error.contains("Check Terminal"),qPrintable(error));
        }
    }
}

void TestHgsClient::settingsRejectMismatchedReceipt()
{
    QFETCH(QString, mode);
    QTemporaryDir directory;
    QFile script(directory.filePath("hgs")); QVERIFY(script.open(QIODevice::WriteOnly));
    script.write(R"(#!/usr/bin/env python3
import json,pathlib,sys
p=json.load(sys.stdin)
with pathlib.Path(__file__).with_name('calls').open('a') as f:f.write('call\n')
mode=sys.argv[-2].rsplit('/',1)[-1]
if mode in ('stale','killed'):
 print('session identity changed; inspect Terminal',file=sys.stderr)
 sys.exit(137 if mode=='killed' else 1)
if mode=='malformed':
 print('not json');sys.exit(0)
if mode=='array':
 print('[]');sys.exit(0)
r={'status':'scheduled','request_id':p['request_id'],'name':sys.argv[-2],
 'run_id':p['expected_run_id'],'conversation_id':p['expected_conversation_id'],
 'effort':p['effort'],'model':p['model'],'scope':'session'}
r[mode]='different-value'
print(json.dumps(r))
)");
    script.close(); QVERIFY(FakeCli::makeRunnable(script.fileName()));
    HgsClient client(FakeCli::program(script.fileName()));
    QSignalSpy finished(&client, &HgsClient::settingsFinished), refreshed(&client, &HgsClient::sessionWriteDone);
    const auto request = client.requestSettings({}, "codex/project/" + mode, "gpt-6-astra", "high", "run", "conversation");
    QTRY_COMPARE(finished.size(), 1); QCOMPARE(finished[0][0].toULongLong(), request);
    QVERIFY(!finished[0][1].toBool()); QVERIFY(finished[0][2].toJsonObject().isEmpty());
    QVERIFY(!finished[0][3].toString().isEmpty()); QCOMPARE(refreshed.size(), 0);
    if (mode == "stale" || mode == "killed")
        QCOMPARE(finished[0][3].toString(), QString("session identity changed; inspect Terminal"));
    QTest::qWait(50); QCOMPARE(finished.size(), 1);
    QFile calls(directory.filePath("calls")); QVERIFY(calls.open(QIODevice::ReadOnly));
    QCOMPARE(calls.readAll(), QByteArray("call\n"));
}

void TestHgsClient::effortUsesStdinAndPinnedIdentityReceipt()
{
    QTemporaryDir directory;
    QFile script(directory.filePath("hgs")); QVERIFY(script.open(QIODevice::WriteOnly));
    script.write(R"(#!/usr/bin/env python3
import json,pathlib,sys
p=json.load(sys.stdin)
pathlib.Path(__file__).with_name('effort.json').write_text(json.dumps({'argv':sys.argv[1:],'payload':p}))
print(json.dumps({'status':'confirmed','request_id':p['request_id'],'name':sys.argv[-2],
 'run_id':p['expected_run_id'],'conversation_id':p['expected_conversation_id'],
 'effort':p['effort'],'model':'gpt-6-astra','scope':'session'}))
)");
    script.close(); QVERIFY(FakeCli::makeRunnable(script.fileName()));
    HgsClient client(FakeCli::program(script.fileName()));
    QSignalSpy finished(&client, &HgsClient::effortFinished), refreshed(&client, &HgsClient::sessionWriteDone);
    const QString name = QString::fromUtf8("codex/проект/Новый '$HOME' `literal`");
    const quint64 request = client.requestEffort("mac", name, "high", "run-identity", "conversation-identity");
    QTRY_COMPARE(finished.size(), 1); QCOMPARE(finished[0][0].toULongLong(), request);
    QVERIFY(finished[0][1].toBool()); QVERIFY(finished[0][3].toString().isEmpty());
    const auto receipt = finished[0][2].toJsonObject();
    QCOMPARE(receipt["name"].toString(), name); QCOMPARE(receipt["effort"].toString(), QString("high"));
    QCOMPARE(receipt["scope"].toString(), QString("session"));
    QCOMPARE(refreshed.size(), 1); QCOMPARE(refreshed[0][0].toString(), QString("mac"));
    QFile captured(directory.filePath("effort.json")); QVERIFY(captured.open(QIODevice::ReadOnly));
    const auto record = QJsonDocument::fromJson(captured.readAll()).object();
    QCOMPARE(record["argv"].toArray(), QJsonArray({"@mac", "effort", name, "--json"}));
    const auto payload = record["payload"].toObject();
    QCOMPARE(payload["expected_run_id"].toString(), QString("run-identity"));
    QCOMPARE(payload["expected_conversation_id"].toString(), QString("conversation-identity"));
    QCOMPARE(payload["effort"].toString(), QString("high"));
    QCOMPARE(payload["request_id"], receipt["request_id"]);
    QVERIFY(!payload["request_id"].toString().isEmpty());
}

void TestHgsClient::effortFailures_data()
{
    QTest::addColumn<QString>("mode");
    for (const QString field : {"request_id", "name", "run_id", "conversation_id", "effort", "status",
                                "stale", "killed", "malformed", "array"})
        QTest::newRow(field.toUtf8()) << field;
}

void TestHgsClient::effortFailures()
{
    QFETCH(QString, mode);
    QTemporaryDir directory;
    QFile script(directory.filePath("hgs")); QVERIFY(script.open(QIODevice::WriteOnly));
    script.write(R"(#!/usr/bin/env python3
import json,pathlib,sys
p=json.load(sys.stdin)
with pathlib.Path(__file__).with_name('calls').open('a') as f:f.write('call\n')
mode=sys.argv[-2].rsplit('/',1)[-1]
if mode in ('stale','killed'):
 print('session identity changed; inspect Terminal',file=sys.stderr)
 sys.exit(137 if mode=='killed' else 1)
if mode=='malformed':
 print('not json');sys.exit(0)
if mode=='array':
 print('[]');sys.exit(0)
r={'status':'confirmed','request_id':p['request_id'],'name':sys.argv[-2],
 'run_id':p['expected_run_id'],'conversation_id':p['expected_conversation_id'],
 'effort':p['effort'],'scope':'session'}
r[mode]='different-value'
print(json.dumps(r))
)");
    script.close(); QVERIFY(FakeCli::makeRunnable(script.fileName()));
    HgsClient client(FakeCli::program(script.fileName()));
    QSignalSpy finished(&client, &HgsClient::effortFinished), refreshed(&client, &HgsClient::sessionWriteDone);
    const auto request = client.requestEffort({}, "codex/project/" + mode, "high", "run", "conversation");
    QTRY_COMPARE(finished.size(), 1); QCOMPARE(finished[0][0].toULongLong(), request);
    QVERIFY(!finished[0][1].toBool()); QVERIFY(finished[0][2].toJsonObject().isEmpty());
    QVERIFY(!finished[0][3].toString().isEmpty()); QCOMPARE(refreshed.size(), 0);
    if (mode == "stale" || mode == "killed")
        QCOMPARE(finished[0][3].toString(), QString("session identity changed; inspect Terminal"));
    QTest::qWait(50); QCOMPARE(finished.size(), 1);
    QFile calls(directory.filePath("calls")); QVERIFY(calls.open(QIODevice::ReadOnly));
    QCOMPARE(calls.readAll(), QByteArray("call\n"));
}

void TestHgsClient::effortLateResponsesKeepRequestIdentity()
{
    QTemporaryDir directory;
    QFile script(directory.filePath("hgs")); QVERIFY(script.open(QIODevice::WriteOnly));
    script.write(R"(#!/usr/bin/env python3
import json,sys,time
p=json.load(sys.stdin)
if sys.argv[-2].endswith('/slow'):time.sleep(.3)
print(json.dumps({'status':'confirmed','request_id':p['request_id'],'name':sys.argv[-2],
 'run_id':p['expected_run_id'],'conversation_id':p['expected_conversation_id'],
 'effort':p['effort'],'scope':'session'}))
)");
    script.close(); QVERIFY(FakeCli::makeRunnable(script.fileName()));
    HgsClient client(FakeCli::program(script.fileName()));
    QSignalSpy finished(&client, &HgsClient::effortFinished), refreshed(&client, &HgsClient::sessionWriteDone);
    const auto slow = client.requestEffort({}, "codex/project/slow", "low", "run-old", "conversation-old");
    const auto fast = client.requestEffort("mac", "kimi/project/fast", "max", "run-new", "conversation-new");
    QVERIFY(slow != fast); QTRY_COMPARE(finished.size(), 2);
    QCOMPARE(finished[0][0].toULongLong(), fast); QCOMPARE(finished[1][0].toULongLong(), slow);
    QVERIFY(finished[0][1].toBool()); QVERIFY(finished[1][1].toBool());
    const auto fastReceipt = finished[0][2].toJsonObject(), slowReceipt = finished[1][2].toJsonObject();
    QCOMPARE(fastReceipt["name"].toString(), QString("kimi/project/fast"));
    QCOMPARE(fastReceipt["run_id"].toString(), QString("run-new"));
    QCOMPARE(fastReceipt["conversation_id"].toString(), QString("conversation-new"));
    QCOMPARE(fastReceipt["effort"].toString(), QString("max"));
    QCOMPARE(slowReceipt["name"].toString(), QString("codex/project/slow"));
    QCOMPARE(slowReceipt["run_id"].toString(), QString("run-old"));
    QCOMPARE(slowReceipt["conversation_id"].toString(), QString("conversation-old"));
    QCOMPARE(slowReceipt["effort"].toString(), QString("low"));
    QCOMPARE(refreshed.size(), 2); QCOMPARE(refreshed[0][0].toString(), QString("mac"));
    QCOMPARE(refreshed[1][0].toString(), QString());
    HgsClient missing(directory.filePath("missing")); QSignalSpy failed(&missing, &HgsClient::effortFinished);
    const auto absent = missing.requestEffort({}, "codex/p", "high", "run", "conversation");
    QTRY_COMPARE(failed.size(), 1); QCOMPARE(failed[0][0].toULongLong(), absent);
    QVERIFY(!failed[0][1].toBool()); QVERIFY(!failed[0][3].toString().isEmpty());
}

void TestHgsClient::questionAnswersUsePinnedIdentityAndStdin()
{
    QTemporaryDir directory;
    QFile script(directory.filePath("hgs")); QVERIFY(script.open(QIODevice::WriteOnly));
    script.write(R"(#!/usr/bin/env python3
import json,pathlib,sys,time
p=json.load(sys.stdin)
pathlib.Path(__file__).with_name('answer.json').write_text(json.dumps({'argv':sys.argv[1:],'payload':p}))
time.sleep(.1)
print(json.dumps({'status':'answered','request_id':p['request_id'],'name':sys.argv[-2],
 'run_id':p['expected_run_id'],'conversation_id':p['expected_conversation_id'],
 'question_id':p['question_id'],'question_hash':p['expected_question_hash']}))
)");
    script.close(); QVERIFY(FakeCli::makeRunnable(script.fileName()));
    HgsClient client(FakeCli::program(script.fileName()));
    QSignalSpy sent(&client, &HgsClient::questionAnswered), failed(&client, &HgsClient::questionAnswerFailed);
    const QJsonObject question{{"question_id", "pending-question"}, {"question_hash", "exact-content"},
        {"run_id", "run-one"}, {"conversation_id", "conversation-one"}, {"can_answer", true}};
    const QJsonArray answers{QJsonObject{{"question_id", "q_0"}, {"selected_option_ids", QJsonArray{"opt_0_1"}}, {"text", ""}},
        QJsonObject{{"question_id", "q_1"}, {"selected_option_ids", QJsonArray{}}, {"text", QString::fromUtf8("Свой ответ '$HOME' `literal`")} }};
    const QString name = QString::fromUtf8("kimi/проект/Вопросы");
    const auto request = client.requestAnswerQuestion("mac", name, question, answers);
    client.requestAnswerQuestion("mac", name, question, answers);
    QTRY_COMPARE(failed.size(), 1); QVERIFY(!failed[0][4].toBool());
    QTRY_COMPARE(sent.size(), 1); QCOMPARE(sent[0][0].toULongLong(), request);
    QCOMPARE(sent[0][1].toString(), QString("mac")); QCOMPARE(sent[0][2].toString(), name);
    QFile captured(directory.filePath("answer.json")); QVERIFY(captured.open(QIODevice::ReadOnly));
    const auto record = QJsonDocument::fromJson(captured.readAll()).object();
    QCOMPARE(record["argv"].toArray(), QJsonArray({"@mac", "answer", name, "--json"}));
    const auto payload = record["payload"].toObject();
    QCOMPARE(payload["answers"].toArray(), answers);
    QCOMPARE(payload["question_id"], question["question_id"]);
    QCOMPARE(payload["expected_question_hash"], question["question_hash"]);
    QCOMPARE(payload["expected_run_id"], question["run_id"]);
    QCOMPARE(payload["expected_conversation_id"], question["conversation_id"]);
    captured.close();auto optional=question;optional["optional"]=true;optional["can_skip"]=true;optional["source"]="codex_async";optional["can_answer"]=false;
    const QJsonArray skipped{QJsonObject{{"question_id","q_0"},{"skip",true}}};
    client.requestAnswerQuestion("mac",name,optional,skipped);QTRY_COMPARE(sent.size(),2);
    QVERIFY(captured.open(QIODevice::ReadOnly));QCOMPARE(QJsonDocument::fromJson(captured.readAll()).object()["payload"].toObject()["answers"].toArray(),skipped);
    optional["optional"]=false;client.requestAnswerQuestion("mac",name,optional,skipped);QTRY_COMPARE(failed.size(),2);
    const QString hookHash(64, QChar('b'));
    auto hooks=question;hooks["question_id"]="codex-hooks-trust:"+hookHash;hooks["question_hash"]=hookHash;
    hooks["source"]="codex_hooks_trust";hooks["answer_transport"]="codex_tui";hooks["conversation_id"]=QJsonValue::Null;
    const QJsonArray hookAnswer{QJsonObject{{"question_id","hooks_trust"},{"selected_option_ids",QJsonArray{"continue_without_trusting"}},{"text",""}}};
    client.requestAnswerQuestion({},"codex/project/startup",hooks,hookAnswer);QTRY_COMPARE(sent.size(),3);
    captured.close();QVERIFY(captured.open(QIODevice::ReadOnly));const auto hookPayload=QJsonDocument::fromJson(captured.readAll()).object()["payload"].toObject();
    QCOMPARE(hookPayload["expected_conversation_id"].toString(),QString());QCOMPARE(hookPayload["answers"].toArray(),hookAnswer);
    auto update=hooks;update["question_id"]="codex-update:"+hookHash;update["source"]="codex_update";
    const QJsonArray updateAnswer{QJsonObject{{"question_id","codex_update"},{"selected_option_ids",QJsonArray{"skip"}},{"text",""}}};
    client.requestAnswerQuestion({},"codex/project/startup",update,updateAnswer);QTRY_COMPARE(sent.size(),4);
    captured.close();QVERIFY(captured.open(QIODevice::ReadOnly));const auto updatePayload=QJsonDocument::fromJson(captured.readAll()).object()["payload"].toObject();
    QCOMPARE(updatePayload["question_id"].toString(),"codex-update:"+hookHash);QCOMPARE(updatePayload["answers"].toArray(),updateAnswer);
}

void TestHgsClient::unconfirmedQuestionCannotStartTransport()
{
    QTemporaryDir directory; QFile program(directory.filePath("hgs"));
    QVERIFY(program.open(QIODevice::WriteOnly));
    program.write("#!/usr/bin/env python3\nopen(__file__+'.calls','a').write('called\\n')\n"); program.close();
    QVERIFY(FakeCli::makeRunnable(program.fileName()));
    HgsClient client(FakeCli::program(program.fileName()));
    QSignalSpy sent(&client, &HgsClient::questionAnswered), failed(&client, &HgsClient::questionAnswerFailed);
    const QString hash(64, QChar('a'));
    const QList<QStringList> requests{{"kimi", "kimi-trust:", "kimi_folder_trust", "kimi_tui"},
        {"claude", "claude-trust:", "claude_folder_trust", "claude_tui"}, {"codex", "codex-trust:", "codex_folder_trust", "codex_tui"},
        {"claude-permissions", "claude-permissions:", "claude_permission_mode", "claude_tui"},
        {"codex-hooks", "codex-hooks-trust:", "codex_hooks_trust", "codex_tui"}, {"codex-update", "codex-update:", "codex_update", "codex_tui"}};
    for (const auto &kind : requests) {
    const QString provider = kind[0];
    const QJsonObject startup{{"question_id", kind[1] + hash}, {"question_hash", hash}, {"source", kind[2]}, {"answer_transport", kind[3]},
        {"run_id", "run"}, {"conversation_id", QJsonValue::Null}, {"can_answer", true}};
    const QList<QPair<QString, QJsonValue>> invalid{
        {"source", "kimi_wire"}, {"question_id", "other-question"},
        {"answer_transport", "other"}, {"question_hash", "different"},
        {"run_id", ""}, {"can_answer", false}};
    for (const auto &change : invalid) {
        auto question = startup; question[change.first] = change.second;
        const auto before = failed.size();
        client.requestAnswerQuestion({}, provider + "/startup", question,
            {QJsonObject{{"question_id", "trust"}, {"selected_option_ids", QJsonArray{"trust_0"}}}});
        QTRY_COMPARE(failed.size(), before + 1); QVERIFY(!failed.last()[4].toBool());
        QVERIFY(failed.last()[3].toString().contains("not ready"));
    }
    }
    QCOMPARE(sent.size(), 0); QVERIFY(!QFile::exists(program.fileName() + ".calls"));
}

void TestHgsClient::questionAnswerFailures_data()
{
    QTest::addColumn<QString>("mode"); QTest::addColumn<bool>("uncertain");
    QTest::newRow("stale-question") << QString("stale") << false;
    QTest::newRow("changed-during-delivery") << QString("uncertain") << true;
    QTest::newRow("terminated-remote") << QString("killed") << true;
    QTest::newRow("startup-receipt-missing-conversation") << QString("startup-missing-conversation") << true;
    for (const QString field : {"request_id", "name", "run_id", "conversation_id", "question_id", "question_hash", "status"})
        QTest::newRow(field.toUtf8()) << field << true;
}

void TestHgsClient::questionAnswerFailures()
{
    QFETCH(QString, mode); QFETCH(bool, uncertain);
    QTemporaryDir directory;
    QFile script(directory.filePath("hgs")); QVERIFY(script.open(QIODevice::WriteOnly));
    script.write(R"(#!/usr/bin/env python3
import json,pathlib,sys
p=json.load(sys.stdin)
with pathlib.Path(__file__).with_name('calls').open('a') as f:f.write('call\n')
mode=p['answers'][0]['text']
if mode in ['stale','uncertain','killed']:
 print('delivery uncertain: question changed' if mode=='uncertain' else 'question is no longer pending',file=sys.stderr)
 sys.exit(137 if mode=='killed' else 1)
r={'status':'answered','request_id':p['request_id'],'name':sys.argv[-2],
 'run_id':p['expected_run_id'],'conversation_id':p['expected_conversation_id'],
 'question_id':p['question_id'],'question_hash':p['expected_question_hash']}
if mode=='startup-missing-conversation': del r['conversation_id']
else: r[mode]='another-identity'
print(json.dumps(r))
)");
    script.close(); QVERIFY(FakeCli::makeRunnable(script.fileName()));
    HgsClient client(FakeCli::program(script.fileName()));
    QSignalSpy sent(&client, &HgsClient::questionAnswered), failed(&client, &HgsClient::questionAnswerFailed);
    QJsonObject question{{"question_id", "pending"}, {"question_hash", "hash"},
        {"run_id", "run"}, {"conversation_id", "conversation"}, {"can_answer", true}};
    if (mode == "startup-missing-conversation") {
        const QString hash(64, QChar('a'));
        question["conversation_id"] = QJsonValue::Null;
        question["source"] = "kimi_folder_trust"; question["answer_transport"] = "kimi_tui";
        question["question_hash"] = hash; question["question_id"] = "kimi-trust:" + hash;
    }
    client.requestAnswerQuestion({}, "kimi/project", question, {QJsonObject{{"question_id", "q_0"}, {"text", mode}}});
    QTRY_COMPARE(failed.size(), 1); QCOMPARE(failed[0][4].toBool(), uncertain); QCOMPARE(sent.size(), 0);
    QTest::qWait(30); QCOMPARE(failed.size(), 1);
    QFile calls(directory.filePath("calls")); QVERIFY(calls.open(QIODevice::ReadOnly)); QCOMPARE(calls.readAll(), QByteArray("call\n"));
}

void TestHgsClient::messagesUseStdinAndIdentityReceipts()
{
    QTemporaryDir directory;
    QFile script(directory.filePath("hgs")); QVERIFY(script.open(QIODevice::WriteOnly));
    script.write(R"(#!/usr/bin/env python3
import json,pathlib,sys
request=json.load(sys.stdin)
pathlib.Path(__file__).with_name('request.json').write_text(json.dumps({'argv':sys.argv[1:],'payload':request}))
print(json.dumps({'status':'submitted','request_id':request['request_id'],'name':sys.argv[-2],
 'run_id':request['expected_run_id'],'conversation_id':request['expected_conversation_id'],
 'submitted_text':request['text'],'submitted_at':123.5}))
)");
    script.close(); QVERIFY(FakeCli::makeRunnable(script.fileName()));
    HgsClient client(FakeCli::program(script.fileName()));
    QSignalSpy sent(&client, &HgsClient::messageSent), failed(&client, &HgsClient::messageFailed);
    QSignalSpy refreshed(&client, &HgsClient::sessionWriteDone);
    const QString name = QString::fromUtf8("codex/проект/Новый");
    const QString text = QString::fromUtf8("Привет\nsecond line $(echo secret) `literal`");
    const QByteArray bytes("\x89PNG\0test", 9);
    const QList<MessageAttachment> files{{QString::fromUtf8("снимок.png"), "image/png", bytes, "[Image #1]"}};
    const quint64 request = client.requestSendMessage("mac", name, text, files, "run-id", "conversation-id");
    QTRY_COMPARE(sent.size(), 1);
    QCOMPARE(failed.size(), 0); QCOMPARE(refreshed.size(), 1);
    QCOMPARE(sent[0][0].toULongLong(), request); QCOMPARE(sent[0][1].toString(), QString("mac"));
    QCOMPARE(sent[0][2].toString(), name);
    QFile captured(directory.filePath("request.json")); QVERIFY(captured.open(QIODevice::ReadOnly));
    const auto record = QJsonDocument::fromJson(captured.readAll()).object();
    QCOMPARE(record["argv"].toArray(), QJsonArray({"@mac", "send", name, "--json"}));
    const auto payload = record["payload"].toObject();
    QCOMPARE(payload["text"].toString(), text);
    QCOMPARE(payload["expected_run_id"].toString(), QString("run-id"));
    QCOMPARE(payload["expected_conversation_id"].toString(), QString("conversation-id"));
    QCOMPARE(payload["attachments"].toArray().size(), 1);
    QCOMPARE(payload["attachments"].toArray()[0].toObject()["reference"].toString(), QString("[Image #1]"));
    QCOMPARE(QByteArray::fromBase64(payload["attachments"].toArray()[0].toObject()["data_base64"].toString().toLatin1()), bytes);
}

void TestHgsClient::messageFailuresAreExplicitAndNeverRetried()
{
    QTemporaryDir directory;
    QFile script(directory.filePath("hgs")); QVERIFY(script.open(QIODevice::WriteOnly));
    script.write(R"(#!/usr/bin/env python3
import json,pathlib,sys,time
request=json.load(sys.stdin)
with pathlib.Path(__file__).with_name('calls').open('a') as file:file.write('call\n')
time.sleep(.15)
if request['text']=='refused':
 print('agent is busy',file=sys.stderr);sys.exit(1)
if request['text']=='uncertain':
 print('delivery uncertain: link dropped',file=sys.stderr);sys.exit(1)
if request['text']=='killed':
 print('remote process terminated',file=sys.stderr);sys.exit(137)
if request['text']=='exit127':
 print('program exited after input',file=sys.stderr);sys.exit(127)
print('{}')
)");
    script.close(); QVERIFY(FakeCli::makeRunnable(script.fileName()));
    HgsClient client(FakeCli::program(script.fileName()));
    QSignalSpy sent(&client, &HgsClient::messageSent), failed(&client, &HgsClient::messageFailed);
    client.requestSendMessage({}, "codex/p", "refused", {}, "run");
    QTRY_COMPARE(failed.size(), 1); QVERIFY(!failed[0][4].toBool());
    client.requestSendMessage({}, "codex/p", "uncertain", {}, "run");
    QTRY_COMPARE(failed.size(), 2); QVERIFY(failed[1][4].toBool());
    client.requestSendMessage({}, "codex/p", "killed", {}, "run");
    QTRY_COMPARE(failed.size(), 3); QVERIFY(failed[2][4].toBool());
    client.requestSendMessage({}, "codex/p", "exit127", {}, "run");
    QTRY_COMPARE(failed.size(), 4); QVERIFY(failed[3][4].toBool());
    const auto malformed = client.requestSendMessage({}, "codex/p", "bad receipt", {}, "run");
    const auto duplicate = client.requestSendMessage({}, "codex/p", "another", {}, "run");
    QTRY_COMPARE(failed.size(), 6);
    bool gotMalformed = false, gotDuplicate = false;
    for (const auto &values : failed) {
        if (values[0].toULongLong() == malformed) { gotMalformed = true; QVERIFY(values[4].toBool()); }
        if (values[0].toULongLong() == duplicate) { gotDuplicate = true; QVERIFY(!values[4].toBool()); }
    }
    QVERIFY(gotMalformed); QVERIFY(gotDuplicate); QCOMPARE(sent.size(), 0);
    QFile calls(directory.filePath("calls")); QVERIFY(calls.open(QIODevice::ReadOnly));
    QCOMPARE(calls.readAll(), QByteArray("call\ncall\ncall\ncall\ncall\n"));
    HgsClient absent(directory.filePath("missing"));
    QSignalSpy absentFailed(&absent, &HgsClient::messageFailed);
    absent.requestSendMessage({}, "codex/p", "hello", {}, "run");
    QTRY_COMPARE(absentFailed.size(), 1); QVERIFY(!absentFailed[0][4].toBool());
    HgsClient bareAbsent(QStringLiteral("hgs-certainly-missing-test-program"));
    QSignalSpy bareFailed(&bareAbsent, &HgsClient::messageFailed);
    bareAbsent.requestSendMessage({}, "codex/p", "hello", {}, "run");
    QTRY_COMPARE(bareFailed.size(), 1); QVERIFY(!bareFailed[0][4].toBool());
    QFile blocked(directory.filePath("blocked")); QVERIFY(blocked.open(QIODevice::WriteOnly));
    blocked.write("#!/bin/sh\nexit 0\n"); blocked.close();
    QVERIFY(blocked.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner));
    HgsClient unexecutable(blocked.fileName());
    QSignalSpy blockedFailed(&unexecutable, &HgsClient::messageFailed);
    unexecutable.requestSendMessage({}, "codex/p", "hello", {}, "run");
    QTRY_COMPARE(blockedFailed.size(), 1); QVERIFY(!blockedFailed[0][4].toBool());
}

void TestHgsClient::forkCommandsPinSourceIdentity()
{
    QTemporaryDir directory;
    QFile script(directory.filePath("hgs")); QVERIFY(script.open(QIODevice::WriteOnly));
    script.write(R"(#!/usr/bin/env python3
import json,pathlib,sys
with pathlib.Path(__file__).with_name('fork.jsonl').open('a') as f:f.write(json.dumps(sys.argv[1:])+'\n')
if 'rejected' in sys.argv:
 print('source conversation changed',file=sys.stderr);sys.exit(7)
)");
    script.close(); QVERIFY(FakeCli::makeRunnable(script.fileName()));
    HgsClient client(FakeCli::program(script.fileName()));
    QSignalSpy writes(&client, &HgsClient::writeDone), refresh(&client, &HgsClient::sessionWriteDone);
    const QString name = QString::fromUtf8("codex/проект/Исток"), tag = QString::fromUtf8("Новая '$HOME' `literal`");
    client.forkSession({}, name, tag, {}, "source-run", "source-conversation");
    QTRY_COMPARE(writes.size(), 1); QVERIFY(writes[0][1].toBool());
    client.forkSession("mac", name, tag, "archive-original", "archived-run", "archived-conversation");
    QTRY_COMPARE(writes.size(), 2); QVERIFY(writes[1][1].toBool());
    client.forkSession("mac", name, "rejected", {}, "stale-run", "stale-conversation");
    QTRY_COMPARE(writes.size(), 3); QVERIFY(!writes[2][1].toBool());
    QCOMPARE(writes[2][2].toString(), QString("source conversation changed"));
    QCOMPARE(refresh.size(), 3); QCOMPARE(refresh[0][0].toString(), QString());
    QCOMPARE(refresh[1][0].toString(), QString("mac")); QCOMPARE(refresh[2][0].toString(), QString("mac"));
    QFile captured(directory.filePath("fork.jsonl")); QVERIFY(captured.open(QIODevice::ReadOnly));
    const auto calls = captured.readAll().trimmed().split('\n'); QCOMPARE(calls.size(), 3);
    QCOMPARE(QJsonDocument::fromJson(calls[0]).array(), QJsonArray({"fork", name, "-n", tag, "-d",
        "--expected-run-id", "source-run", "--expected-conversation-id", "source-conversation"}));
    QCOMPARE(QJsonDocument::fromJson(calls[1]).array(), QJsonArray({"@mac", "fork", name, "-n", tag, "-d",
        "--archive", "archive-original", "--expected-run-id", "archived-run", "--expected-conversation-id", "archived-conversation"}));
    QCOMPARE(QJsonDocument::fromJson(calls[2]).array(), QJsonArray({"@mac", "fork", name, "-n", "rejected", "-d",
        "--expected-run-id", "stale-run", "--expected-conversation-id", "stale-conversation"}));
}

void TestHgsClient::renameCommands()
{
    QTemporaryDir dir; QFile script(dir.filePath("hgs")); QVERIFY(script.open(QIODevice::WriteOnly));
    script.write("#!/usr/bin/env python3\nimport sys\nfor a in sys.argv[1:]:print(a,file=sys.stderr)\nif 'rejected' in ' '.join(sys.argv[1:]):sys.exit(7)\n"); script.close();
    QVERIFY(FakeCli::makeRunnable(script.fileName()));
    HgsClient client(FakeCli::program(script.fileName())); QSignalSpy writes(&client, &HgsClient::writeDone);
    QSignalSpy refresh(&client, &HgsClient::sessionWriteDone);
    client.renameSession({}, "codex/p/old", "codex/p/Новый план"); QTRY_COMPARE(writes.size(), 1);
    QVERIFY(writes[0][1].toBool()); QCOMPARE(writes[0][2].toString(), QString("rename\ncodex/p/old\ncodex/p/Новый план"));
    client.renameSession("mac", "claude/p", "claude/p/new tag", "archive-id"); QTRY_COMPARE(writes.size(), 2);
    QVERIFY(writes[1][1].toBool()); QCOMPARE(writes[1][2].toString(), QString("@mac\nrename\nclaude/p\nclaude/p/new tag\n--archive\narchive-id"));
    client.renameSession("mac", "codex/p/old", "codex/p/rejected"); QTRY_COMPARE(writes.size(), 3);
    QVERIFY(!writes[2][1].toBool()); QCOMPARE(refresh.size(), 3);
    QCOMPARE(refresh[0][0].toString(), QString()); QCOMPARE(refresh[1][0].toString(), QString("mac"));
}

void TestHgsClient::preservesArgumentBytes()
{
    QTemporaryDir dir; QFile script(dir.filePath("hgs")); QVERIFY(script.open(QIODevice::WriteOnly));
    // Hex is ASCII on every platform and proves actual child argv bytes rather
    // than comparing text after a normalization step in the test fixture.
    script.write("#!/usr/bin/env python3\n"
                 "import os,sys\n"
                 "for k,v in sorted(os.environ.items()):\n"
                 " if k.startswith('_HGS_ZERUS_ARG_'):print(k+'='+v,file=sys.stderr)\n"
                 "for a in sys.argv[1:]:print(os.fsencode(a).hex(),file=sys.stderr)\n"
                 "if sys.argv[1:2]==['inspect']:print('{\"events\":[],\"cursor\":0}');sys.exit(7)\n");
    script.close(); QVERIFY(FakeCli::makeRunnable(script.fileName()));
    HgsClient client(FakeCli::program(script.fileName())); QSignalSpy writes(&client, &HgsClient::writeDone);
    QSignalSpy failed(&client, &HgsClient::inspectionFailed);
    const QString nfc = QString::fromUtf8("codex/проект/Новый");
    const QString nfd = nfc.normalized(QString::NormalizationForm_D);
    QVERIFY(nfc.toUtf8() != nfd.toUtf8());
    const QString literal = QString::fromUtf8("line\nНовый '$()' \"`exit 93`\" \\ $(exit 94)");
    const auto hexLines = [](const QStringList &arguments) {
        QStringList lines; for (const auto &argument : arguments) lines.append(QString::fromLatin1(argument.toUtf8().toHex()));
        return lines.join('\n');
    };
    client.renameSession({}, nfc, nfd); QTRY_COMPARE(writes.size(), 1);
    QVERIFY(writes[0][1].toBool()); QCOMPARE(writes[0][2].toString(), hexLines({"rename", nfc, nfd}));
    client.renameSession({}, nfd, literal); QTRY_COMPARE(writes.size(), 2);
    QVERIFY(writes[1][1].toBool()); QCOMPARE(writes[1][2].toString(), hexLines({"rename", nfd, literal}));
    client.requestInspection({}, nfc); QTRY_COMPARE(failed.size(), 1);
    QCOMPARE(failed[0][2].toString(), hexLines({"inspect", nfc, "--after", "0"}));
    client.requestInspection({}, nfd); QTRY_COMPARE(failed.size(), 2);
    QCOMPARE(failed[1][2].toString(), hexLines({"inspect", nfd, "--after", "0"}));
}

void TestHgsClient::parsesBox()
{
    const QByteArray json = R"({"host":"arch","ok":true,
      "metrics":{"cpu_percent":27.5,"memory_total_bytes":34359738368.0,"sampled_at":1791018000},
      "projects":{"sample-project":"/workspace/sample-project"},
      "sessions":[{"name":"claude/sample-project","cmd":"claude","project":"sample-project",
                   "tag":null,"attached":1,"clients":["/dev/pts/14"],"created":1756304176}]})";
    QString err;
    const BoxState box = HgsClient::parseBox(json, &err);
    QVERIFY2(err.isEmpty(), qPrintable(err));
    QCOMPARE(box.host, QStringLiteral("arch"));
    QCOMPARE(box.metrics.value("cpu_percent").toDouble(), 27.5);
    QCOMPARE(box.metrics.value("memory_total_bytes").toDouble(), 34359738368.0);
    QVERIFY(box.ok);
    QCOMPARE(box.projects.value(QStringLiteral("sample-project")),
             QStringLiteral("/workspace/sample-project"));
    QCOMPARE(box.sessions.size(), 1);
    QCOMPARE(box.sessions[0].name, QStringLiteral("claude/sample-project"));
    QCOMPARE(box.sessions[0].cmd, QStringLiteral("claude"));
    QCOMPARE(box.sessions[0].attached, 1);
    QCOMPARE(box.sessions[0].clients, QStringList{QStringLiteral("/dev/pts/14")});
}

void TestHgsClient::parsesOfflineBox()
{
    const QByteArray json =
        R"({"host":"mac","ok":false,"error":"offline","projects":{},"sessions":[]})";
    QString err;
    const BoxState box = HgsClient::parseBox(json, &err);
    QVERIFY2(err.isEmpty(), qPrintable(err));
    QCOMPARE(box.host, QStringLiteral("mac"));
    QVERIFY(!box.ok);
    QCOMPARE(box.error, QStringLiteral("offline"));
    QVERIFY(box.sessions.isEmpty());
}

void TestHgsClient::parsesControlOnlyComputer()
{
    // hgs.exe on Windows hosts no sessions; the key is present only when false.
    QString err;
    const auto windows = HgsClient::parseBox(R"({"host":"windows-pc","ok":true,"projects":{},"sessions":[],
        "peers":["mac"],"metrics":{"platform":"windows"},"local_sessions":false})", &err);
    QVERIFY(err.isEmpty()); QVERIFY(windows.ok); QVERIFY(!windows.localSessions);
    QCOMPARE(windows.peers, QStringList{"mac"});
    const auto posix = HgsClient::parseBox(R"({"host":"arch","ok":true,"sessions":[]})", &err);
    QVERIFY(posix.localSessions);
    const auto fleet = HgsClient::parseFleet(R"([{"host":"windows-pc","ok":true,"local_sessions":false},{"host":"mac","ok":true}])", &err);
    QCOMPARE(fleet.size(), 2); QVERIFY(!fleet[0].localSessions); QVERIFY(fleet[1].localSessions);
}

void TestHgsClient::parsesLocalSessionHost()
{
    // A Windows CLI adds local_host to its own box (spec D2): every key, null
    // facts unless a host answers, and the error of an unavailable one.
    QString err;
    const auto running = HgsClient::parseBox(R"({"host":"windows-pc","ok":true,"sessions":[],
        "metrics":{"platform":"windows"},"local_host":{"state":"running","pid":4242,"protocol":1,
        "conpty":"bundled","integrity":"medium","windows_session":1}})", &err);
    QVERIFY(err.isEmpty()); QVERIFY(running.localSessions);
    QCOMPARE(running.localHost, QJsonObject({{"state", "running"}, {"pid", 4242}, {"protocol", 1},
        {"conpty", "bundled"}, {"integrity", "medium"}, {"windows_session", 1}}));
    const auto unavailable = HgsClient::parseBox(R"({"host":"windows-pc","ok":true,"sessions":[],"local_sessions":false,
        "local_host":{"state":"unavailable","pid":null,"protocol":null,"conpty":null,"integrity":null,
        "windows_session":null,"error":"the session host on this computer runs elevated"}})", &err);
    QVERIFY(!unavailable.localSessions);
    QCOMPARE(unavailable.localHost.value("state").toString(), QString("unavailable"));
    QCOMPARE(unavailable.localHost.value("error").toString(), QString("the session host on this computer runs elevated"));
    QVERIFY(unavailable.localHost.value("pid").isNull());
    // Unix listings and older CLIs carry none.
    const auto posix = HgsClient::parseBox(R"({"host":"arch","ok":true,"sessions":[],"metrics":{"platform":"linux"}})", &err);
    QVERIFY(posix.localHost.isEmpty());
    const auto fleet = HgsClient::parseFleet(R"([{"host":"windows-pc","ok":true,"local_host":{"state":"stopped"}},{"host":"mac","ok":true}])", &err);
    QCOMPARE(fleet[0].localHost.value("state").toString(), QString("stopped")); QVERIFY(fleet[1].localHost.isEmpty());
}

void TestHgsClient::parsesEndedLaunches()
{
    // Launches whose session ended before a listing showed it, without a record, by launch ID.
    QString err;
    const auto box = HgsClient::parseBox(R"({"host":"arch","ok":true,"sessions":[],"ended_launches":[
        {"launch_id":"0b6f4c9e-3c1d-4b8f-9a35-6f2e2d6f6a10","name":"claude/project/one","code":3,"output":"cannot start"},
        {"launch_id":"4400baad-7d90-480a-8451-5f3e456e2ae9","name":"sh/project/two","code":null,"output":""},
        {"name":"sh/project/without-id"}]})", &err);
    QVERIFY(err.isEmpty());
    QCOMPARE(box.endedLaunches.size(), 2);
    const auto one = box.endedLaunches.value("0b6f4c9e-3c1d-4b8f-9a35-6f2e2d6f6a10");
    QCOMPARE(one.value("code").toInt(), 3); QCOMPARE(one.value("output").toString(), QString("cannot start"));
    QVERIFY(box.endedLaunches.value("4400baad-7d90-480a-8451-5f3e456e2ae9").value("code").isNull());
    // Older CLIs and listings without one carry none.
    QVERIFY(HgsClient::parseBox(R"({"host":"arch","ok":true,"sessions":[]})", &err).endedLaunches.isEmpty());
}

void TestHgsClient::sessionHostActionsUseJsonAndReportFailures()
{
    QTemporaryDir directory; QVERIFY(directory.isValid());
    const auto program = directory.filePath("hgs");
    QVERIFY(FakeCli::install(program, "#!/usr/bin/env python3\nimport json,os,sys\n"
        "open(__file__+'.calls','a').write(json.dumps(sys.argv[1:])+'\\n')\n"
        "mode=open(__file__+'.mode').read() if os.path.exists(__file__+'.mode') else ''\n"
        "if mode=='fail':\n print('cannot enable local sessions: access denied',file=sys.stderr); sys.exit(1)\n"
        "if mode=='garbage':\n print('not json'); sys.exit(0)\n"
        "print(json.dumps({'enabled':sys.argv[2]=='install','version':'1.52.0'}))\n"));
    const auto mode = [&](const QByteArray &value) {
        QFile file(program + ".mode"); QVERIFY(file.open(QIODevice::WriteOnly)); file.write(value);
    };
    HgsClient client(FakeCli::program(program));
    QSignalSpy ready(&client, &HgsClient::sessionHostReady), failed(&client, &HgsClient::sessionHostFailed);
    const auto status = client.requestSessionHost("status"), install = client.requestSessionHost("install");
    QVERIFY(status != install);
    QTRY_COMPARE(ready.size(), 2); QVERIFY(failed.isEmpty());
    for (const auto &result : ready) {
        const bool installed = result[0].toULongLong() == install;
        QCOMPARE(result[1].toJsonObject(), QJsonObject({{"enabled", installed}, {"version", "1.52.0"}}));
    }
    mode("fail"); const auto refused = client.requestSessionHost("install");
    QTRY_COMPARE(failed.size(), 1); QCOMPARE(failed[0][0].toULongLong(), refused);
    QCOMPARE(failed[0][1].toString(), QString("cannot enable local sessions: access denied"));
    mode("garbage"); const auto invalid = client.requestSessionHost("status");
    QTRY_COMPARE(failed.size(), 2); QCOMPARE(failed[1][0].toULongLong(), invalid);
    QCOMPARE(failed[1][1].toString(), QString("Invalid session host response"));
    QCOMPARE(ready.size(), 2);
    QFile calls(program + ".calls"); QVERIFY(calls.open(QIODevice::ReadOnly));
    QSet<QByteArray> lines;
    for (const auto &line : calls.readAll().split('\n')) if (!line.isEmpty()) lines.insert(line);
    QCOMPARE(lines, (QSet<QByteArray>{R"(["session-host", "status", "--json"])", R"(["session-host", "install", "--json"])"}));
}

void TestHgsClient::parsesSavedSessions()
{
    QString err;
    const auto box = HgsClient::parseBox(R"({"host":"arch","ok":true,"sessions":[
      {"name":"claude/p","state":"paused","resumable":true},
      {"name":"codex/p","resumable":true,"activity":"idle","tracked":true,"phase":"idle",
       "model":"probe","prompt":"review changes","last_event_at":123.75,"turn_started":100.5,"compaction_started":120.25,"subagent_count":2,
       "process_state":"running","conversation_state":"ended","runtime_state":"live"}]})", &err);
    QVERIFY(err.isEmpty());
    QCOMPARE(box.sessions[0].state, QStringLiteral("paused"));
    QVERIFY(box.sessions[0].resumable);
    QCOMPARE(box.sessions[1].state, QStringLiteral("running"));
    QCOMPARE(box.sessions[1].activity, QStringLiteral("idle"));
    QVERIFY(box.sessions[1].tracked);
    QCOMPARE(box.sessions[1].phase, QStringLiteral("idle"));
    QCOMPARE(box.sessions[1].prompt, QStringLiteral("review changes"));
    QCOMPARE(box.sessions[1].lastEventAt, 123.75);
    QCOMPARE(box.sessions[1].turnStarted, 100.5); QCOMPARE(box.sessions[0].turnStarted, 0.0);
    QCOMPARE(box.sessions[1].compactionStarted, 120.25); QCOMPARE(box.sessions[0].compactionStarted, 0.0);
    QCOMPARE(box.sessions[1].subagentCount, 2);
    QCOMPARE(box.sessions[1].processState, QStringLiteral("running"));
    QCOMPARE(box.sessions[1].conversationState, QStringLiteral("ended"));
    QCOMPARE(box.sessions[1].runtimeState, QStringLiteral("live"));
    QVERIFY(box.sessions[0].processState.isEmpty()); // Older peers remain supported.
}

void TestHgsClient::parsesActivityMetadata()
{
    QString error;
    const auto box = HgsClient::parseBox(R"({"host":"arch","ok":true,"sessions":[
      {"name":"codex/p/ui","cwd":"/work/ui","cwd_source":"tmux","git_root":"/work/ui","launch_id":"unique-launch","account_id":"work","account_home":"/accounts/work",
       "git_branch":"feat/layout","git_worktree":true,"git_worktree_name":"ui","git_detached":false,"git_metadata_state":"ok",
       "activity_summary":"Read","activity_detail":"src/view.cpp","subagent_source":"hooks","subagent_counts_complete":true,
       "subagent_active_count":2,"subagent_completed_count":12,"subagent_total_count":14,
       "subagent_previews":[{"id":"child-1","label":"UI reviewer","state":"working","detail":"Inspect keyboard focus"}]},
      {"name":"kimi/p/legacy","subagent_source":"hook_profiles","subagent_counts_complete":false,
       "subagent_active_count":null,"subagent_total_count":null,"subagent_previews":[{"group":true,"label":"code","total_count":null}]},
      {"name":"sh/p","subagent_source":"unavailable"}]})", &error);
    QVERIFY2(error.isEmpty(), qPrintable(error)); QCOMPARE(box.sessions.size(), 3);
    const auto &s = box.sessions[0]; QCOMPARE(s.cwd, QString("/work/ui")); QCOMPARE(s.cwdSource, QString("tmux"));
    QCOMPARE(s.launchId, QString("unique-launch"));
    QCOMPARE(s.accountId,QString("work"));QCOMPARE(s.accountHome,QString("/accounts/work"));
    QCOMPARE(s.gitRoot, s.cwd); QCOMPARE(s.gitBranch, QString("feat/layout")); QVERIFY(s.gitWorktree);
    QCOMPARE(s.gitWorktreeName, QString("ui")); QVERIFY(!s.gitDetached); QCOMPARE(s.gitMetadataState, QString("ok"));
    QCOMPARE(s.activitySummary, QString("Read")); QCOMPARE(s.activityDetail, QString("src/view.cpp"));
    QCOMPARE(s.subagentActiveCount, 2); QCOMPARE(s.subagentCompletedCount, 12); QCOMPARE(s.subagentTotalCount, 14);
    QVERIFY(s.subagentCountsComplete); QCOMPARE(s.subagentPreviews[0].toObject().value("label").toString(), QString("UI reviewer"));
    QCOMPARE(box.sessions[1].subagentTotalCount, -1); QVERIFY(!box.sessions[1].subagentCountsComplete);
    QVERIFY(box.sessions[1].subagentPreviews[0].toObject().value("group").toBool());
    QCOMPARE(box.sessions[2].subagentSource, QString("unavailable")); QCOMPARE(box.sessions[2].subagentTotalCount, -1);
}

void TestHgsClient::parsesFleetArray()
{
    const QByteArray json = R"([{"host":"arch","ok":true,"projects":{},"sessions":[]},
                                {"host":"mac","ok":false,"error":"bad_response",
                                 "projects":{},"sessions":[]}])";
    QString err;
    const QList<BoxState> boxes = HgsClient::parseFleet(json, &err);
    QVERIFY2(err.isEmpty(), qPrintable(err));
    QCOMPARE(boxes.size(), 2);
    QCOMPARE(boxes[1].error, QStringLiteral("bad_response"));
}

void TestHgsClient::rejectsGarbage()
{
    QString err;
    const BoxState box = HgsClient::parseBox(QByteArray("  (no sessions)\n"), &err);
    QVERIFY(!err.isEmpty());
    QVERIFY(!box.ok);
}

void TestHgsClient::tagAndProjectMayBeNull()
{
    const QByteArray json = R"({"host":"arch","ok":true,"projects":{},
      "sessions":[{"name":"htop","cmd":"htop","project":null,"tag":null,
                   "attached":0,"clients":[],"created":0}]})";
    QString err;
    const BoxState box = HgsClient::parseBox(json, &err);
    QVERIFY2(err.isEmpty(), qPrintable(err));
    QVERIFY(box.sessions[0].project.isNull());
    QVERIFY(box.sessions[0].tag.isNull());
}

// Не из плана дословно, но явно запрошено в разделе "think about": бинарь hgs
// отсутствует вовсе — QProcess не может стартовать (FailedToStart), и это НЕ ok:false
// (та ветка вообще не про JSON), а failed() с путём, который реально пытались запустить,
// чтобы оператор увидел, что чинить.
void TestHgsClient::missingBinaryEmitsFailed()
{
    HgsClient client(QStringLiteral("/no/such/hgs-binary-for-sure"));
    QSignalSpy failedSpy(&client, &HgsClient::failed);
    client.requestLocal();
    QVERIFY(failedSpy.wait(2000));
    QCOMPARE(failedSpy.size(), 1);
    const QString what = failedSpy.at(0).at(0).toString();
    const QString detail = failedSpy.at(0).at(1).toString();
    QVERIFY2(what.contains(QStringLiteral("/no/such/hgs-binary-for-sure")),
              qPrintable(what));
    QVERIFY(!detail.isEmpty());
}

// Task 6: пока запрос к пиру X в полёте, повторный requestPeer(X) должен быть
// проигнорирован (m_peerInFlight в HgsClient.h) -- иначе спящий ноут получает
// по ssh-процессу на каждое открытие меню.
//
// Тест НЕ ждёт настоящих 15с таймаута HgsClient::requestPeer -- это сознательный выбор
// (см. задание Task 6, "добавь тест, если можно выразить поведение гварда без
// 15-секундного ожидания"): вставка в m_peerInFlight в requestPeer() синхронна и
// происходит ДО возврата из вызова, так что второй requestPeer("mac"), выданный из того
// же кадра стека без прохода цикла событий между вызовами, детерминированно видит пира
// уже "в полёте" -- независимо от того, успела ли ОС вообще стартовать процесс первого
// вызова. Поэтому фейковый hgs ниже не спит вовсе: он сразу пишет метку в лог-файл и
// отвечает валидным JSON, и дедупликация проверяется без единой искусственной паузы.
void TestHgsClient::localPollingReportsStartAndRecoversFromFailure()
{
    QTemporaryDir dir; const auto path = dir.filePath("hgs");
    HgsClient client(FakeCli::program(path)); QSignalSpy started(&client, &HgsClient::stateReadStarted);
    QSignalSpy failed(&client, &HgsClient::stateReadFailed), ready(&client, &HgsClient::localReady);
    client.requestLocal(); client.requestLocal(); QCOMPARE(started.size(), 1);
    QTRY_COMPARE(failed.size(), 1); QVERIFY(failed[0][0].toString().isEmpty());
    QVERIFY(FakeCli::install(path, "#!/usr/bin/env python3\nprint('{\"host\":\"arch\",\"ok\":true,\"sessions\":[]}')\n"));
    client.requestLocal(); client.requestLocal(); QCOMPARE(started.size(), 2);
    QTRY_COMPARE(ready.size(), 1);
    QString error;
    const auto box = HgsClient::parseBox(R"({"host":"mac","ok":false,"error":"offline","error_detail":"ssh: Connection refused"})", &error);
    QVERIFY(error.isEmpty());
    QCOMPARE(box.error, QString("ssh: Connection refused"));
}

void TestHgsClient::duplicatePeerRequestIsIgnoredWhileInFlight()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString logPath = dir.filePath(QStringLiteral("calls.log"));
    const QString scriptPath = dir.filePath(QStringLiteral("hgs"));

    QVERIFY(FakeCli::install(scriptPath, "#!/usr/bin/env python3\nopen(" + FakeCli::pythonString(logPath) + ",'a').write('call\\n')\n"
        "print('{\"host\":\"mac\",\"ok\":true,\"projects\":{},\"sessions\":[]}')\n"));

    HgsClient client(FakeCli::program(scriptPath));
    QSignalSpy readySpy(&client, &HgsClient::peerReady);
    QSignalSpy startedSpy(&client, &HgsClient::stateReadStarted);

    client.requestPeer(QStringLiteral("mac"));
    client.requestPeer(QStringLiteral("mac"));  // дубликат -- должен быть проигнорирован
    QCOMPARE(startedSpy.size(), 1); QCOMPARE(startedSpy[0][0].toString(), QString("mac"));

    QVERIFY(readySpy.wait(2000));
    // Даём файловой системе долиться, если ответ уже пришёл, а второй (лишний) процесс
    // ещё дописывает лог -- на практике оба события синхронны с finished(), но лишняя
    // пауза дешева и убирает теоретическую гонку записи в файл.
    QTest::qWait(50);

    QCOMPARE(readySpy.size(), 1);  // не два ответа -- ровно один процесс дошёл до конца
    {
        QFile log(logPath);
        QVERIFY(log.open(QIODevice::ReadOnly | QIODevice::Text));
        const QList<QByteArray> lines = log.readAll().trimmed().split('\n');
        QCOMPARE(lines.size(), 1);  // ровно один запуск, дубликат не породил процесс
    }

    // После завершения метка обязана сняться (см. requestPeer в .cpp) -- иначе пир
    // навсегда застрял бы в m_peerInFlight, и следующий реальный опрос молча терялся бы.
    client.requestPeer(QStringLiteral("mac"));
    QVERIFY(readySpy.wait(2000));
    QTest::qWait(50);
    QCOMPARE(readySpy.size(), 2);
    {
        QFile log(logPath);
        QVERIFY(log.open(QIODevice::ReadOnly | QIODevice::Text));
        QCOMPARE(log.readAll().trimmed().split('\n').size(), 2);  // второй настоящий запуск
    }
}

// A machine saved as "winpeer" whose own HGS_SELF is "zpeer": `hgs @winpeer ls
// --json` forwards that machine's listing, so its "host" is "zpeer". The answer
// must still clear the alias's poll and fill the alias's box.
void TestHgsClient::peerAnswerIsKeyedByThePolledAlias()
{
    QTemporaryDir dir; QVERIFY(dir.isValid());
    const QString scriptPath = dir.filePath(QStringLiteral("hgs"));
    QVERIFY(FakeCli::install(scriptPath, "#!/usr/bin/env python3\nimport sys\n"
        "if sys.argv[1:]!=['@winpeer','ls','--json','--no-archives']:print('unexpected call',file=sys.stderr);sys.exit(1)\n"
        "print('{\"host\":\"zpeer\",\"ok\":true,\"projects\":{},\"peers\":[\"winpeer\"],"
        "\"sessions\":[{\"name\":\"codex/win/build\",\"cmd\":\"codex\",\"project\":\"win\",\"tag\":\"build\"}]}')\n"));
    HgsClient client(FakeCli::program(scriptPath));
    QSignalSpy started(&client, &HgsClient::stateReadStarted), failed(&client, &HgsClient::stateReadFailed);
    QList<BoxState> answers;
    connect(&client, &HgsClient::peerReady, this, [&answers](const BoxState &box) { answers << box; });

    client.requestLocal(); client.requestPeer(QStringLiteral("winpeer"));
    QCOMPARE(client.pollingHosts(), (QSet<QString>{QString(), QStringLiteral("winpeer")}));
    QCOMPARE(started.size(), 2); QCOMPARE(started[1][0].toString(), QStringLiteral("winpeer"));
    QTRY_COMPARE(answers.size(), 1);
    QTRY_COMPARE(failed.size(), 1); QVERIFY(failed[0][0].toString().isEmpty()); // The local read, not the peer.
    QVERIFY(client.pollingHosts().isEmpty());
    const BoxState box = answers.first();
    QCOMPARE(box.host, QStringLiteral("winpeer"));
    QCOMPARE(box.reportedHost, QStringLiteral("zpeer"));
    QVERIFY(box.ok);

    BoxState local; local.host = QStringLiteral("arch"); local.ok = true;
    local.peersKnown = true; local.peers = {QStringLiteral("winpeer")};
    FleetState fleet; const qint64 now = QDateTime::currentMSecsSinceEpoch();
    fleet.setLocal(local, now); fleet.setPeer(box, now);
    QCOMPARE(fleet.peerNames(), QList<QString>{QStringLiteral("winpeer")});
    QVERIFY(!fleet.peer(QStringLiteral("zpeer")));
    const BoxState *peer = fleet.peer(QStringLiteral("winpeer"));
    QVERIFY(peer); QVERIFY(peer->ok); QCOMPARE(fleet.peerPolledAt(QStringLiteral("winpeer")), now);
    QCOMPARE(peer->sessions.size(), 1); QCOMPARE(peer->sessions.first().name, QStringLiteral("codex/win/build"));
    QVERIFY(!fleet.isStale(now));
}

void TestHgsClient::sessionCommands()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString scriptPath = dir.filePath(QStringLiteral("hgs"));
    QVERIFY(FakeCli::install(scriptPath, "#!/usr/bin/env python3\nimport sys\nfor a in sys.argv[1:]:print(a,file=sys.stderr)\nsys.exit(1)\n"));
    HgsClient client(FakeCli::program(scriptPath));
    QSignalSpy done(&client, &HgsClient::writeDone);
    QSignalSpy refresh(&client, &HgsClient::sessionWriteDone);
    client.pauseAll(QStringLiteral("mac"), 3);
    QVERIFY(done.wait(2000));
    QCOMPARE(done.last()[1].toBool(), false);
    QCOMPARE(done.last()[2].toString(), QStringLiteral("@mac\npause\n--all"));
    QCOMPARE(refresh.size(), 1);
    QCOMPARE(refresh.last()[0].toString(), QStringLiteral("mac"));
    client.resumeAll(QString(), 2);
    QVERIFY(done.wait(2000));
    QCOMPARE(done.last()[2].toString(), QStringLiteral("resume\n--all\n-d"));
    QCOMPARE(refresh.size(), 2);
    QCOMPARE(refresh.last()[0].toString(), QString());
    client.pauseSession(QString(), QStringLiteral("claude/p/a b"));
    QVERIFY(done.wait(2000));
    QCOMPARE(done.last()[2].toString(), QStringLiteral("pause\nclaude/p/a b"));
    client.resumeSession(QStringLiteral("mac"), QStringLiteral("codex/p/review"));
    QVERIFY(done.wait(2000));
    QCOMPARE(done.last()[2].toString(), QStringLiteral("@mac\nresume\ncodex/p/review\n-d"));
    client.projectRemove(QStringLiteral("scratch"));
    QVERIFY(done.wait(2000));
    QCOMPARE(refresh.size(), 4); // A project edit must not refresh a peer.
}

void TestHgsClient::inspectionCursorAndFailures()
{
    QTemporaryDir dir;
    QFile script(dir.filePath("hgs"));
    QVERIFY(script.open(QIODevice::WriteOnly));
    script.write("#!/usr/bin/env python3\nimport sys\n"
                 "if sys.argv[1:6]!=['@mac','inspect','codex/p/a b','--after','42']:print('invalid',file=sys.stderr);sys.exit(1)\n"
                 "print('{\"tracked\":true,\"cursor\":43,\"events\":[{\"seq\":43,\"type\":\"Stop\"}]}')\n");
    script.close();
    QVERIFY(FakeCli::makeRunnable(script.fileName()));
    HgsClient client(FakeCli::program(script.fileName()));
    QSignalSpy ready(&client, &HgsClient::inspectionReady);
    QSignalSpy failed(&client, &HgsClient::inspectionFailed);
    QSignalSpy stateFailed(&client, &HgsClient::stateReadFailed);
    QSignalSpy loading(&client, &HgsClient::inspectionLoadingChanged);
    client.requestInspection("mac", "codex/p/a b", 42);
    client.requestInspection("mac", "codex/p/a b", 0); // In-flight duplicate must be ignored.
    QTRY_COMPARE(ready.size(), 1);
    QCOMPARE(failed.size(), 0);
    QCOMPARE(ready[0][0].toString(), QStringLiteral("mac"));
    QCOMPARE(ready[0][1].toString(), QStringLiteral("codex/p/a b"));
    QCOMPARE(qvariant_cast<QJsonObject>(ready[0][2]).value("cursor").toInteger(), 43);
    client.requestInspection("mac", "codex/p/a b", 0);
    QTRY_COMPARE(failed.size(), 1);
    QCOMPARE(failed[0][0].toString(), QStringLiteral("mac"));
    QCOMPARE(stateFailed.size(), 0); // Inspection errors don't mark another machine offline.
    QCOMPARE(loading.size(), 4); // One accepted start and end per request; duplicates do not restart it.
    for (int i = 0; i < loading.size(); ++i) {
        QCOMPARE(loading[i][0].toString(), QString("mac")); QCOMPARE(loading[i][1].toString(), QString("codex/p/a b"));
        QVERIFY(loading[i][2].toString().isEmpty()); QVERIFY(loading[i][3].toString().isEmpty());
        QCOMPARE(loading[i][4].toBool(), i % 2 == 0);
    }
    client.requestPeer("mac");
    QTRY_COMPARE(stateFailed.size(), 1);
    QCOMPARE(stateFailed[0][0].toString(), QStringLiteral("mac"));
}

void TestHgsClient::archiveIdentityAndCommands()
{
    QString error;
    const auto box = HgsClient::parseBox(R"({"host":"arch","ok":true,"sessions":[
        {"name":"codex/p","state":"running"},
        {"name":"codex/p","state":"archived","archive_id":"old-one","archived_at":1790928000,"resumable":true},
        {"name":"codex/p","state":"archived","archive_id":"old-two","archived_at":1790928005,"resumable":false}]})", &error);
    QVERIFY(error.isEmpty()); QCOMPARE(box.sessions.size(), 3);
    QCOMPARE(box.sessions[1].archiveId, QString("old-one"));
    QCOMPARE(box.sessions[2].archiveId, QString("old-two"));
    QCOMPARE(box.sessions[1].archivedAt, 1790928000.0);
    QVERIFY(box.sessions[0].archiveId.isEmpty());
    QTemporaryDir dir; QFile script(dir.filePath("hgs")); QVERIFY(script.open(QIODevice::WriteOnly));
    script.write("#!/usr/bin/env python3\nimport sys\nfor a in sys.argv[1:]:print(a,file=sys.stderr)\n"); script.close();
    QVERIFY(FakeCli::makeRunnable(script.fileName()));
    HgsClient client(FakeCli::program(script.fileName())); QSignalSpy writes(&client, &HgsClient::writeDone);
    QSignalSpy refresh(&client, &HgsClient::sessionWriteDone);
    client.restoreArchive("mac", "codex/p", "old-one"); QTRY_COMPARE(writes.size(), 1);
    QCOMPARE(writes[0][2].toString(), QString("@mac\nresume\ncodex/p\n--archive\nold-one\n-d"));
    client.forgetArchive({}, "codex/p", "old-two"); QTRY_COMPARE(writes.size(), 2);
    QCOMPARE(writes[1][2].toString(), QString("kill\ncodex/p\n--archive\nold-two"));
    client.archiveSession("mac", "codex/p"); QTRY_COMPARE(writes.size(), 3);
    QCOMPARE(writes[2][2].toString(), QString("@mac\narchive\ncodex/p"));
    QCOMPARE(refresh.size(), 3);
    client.restoreArchive({}, "codex/p", {}); QCOMPARE(writes.size(), 4);
    QVERIFY(!writes[3][1].toBool());
    client.forgetArchive({}, "codex/p", {}); QCOMPARE(writes.size(), 5);
    QVERIFY(!writes[4][1].toBool()); QCOMPARE(refresh.size(), 3); // No generic destructive fallback.
}

void TestHgsClient::archiveInspectionKeepsDistinctIdentities()
{
    QTemporaryDir dir; QFile script(dir.filePath("hgs")); QVERIFY(script.open(QIODevice::WriteOnly));
    script.write("#!/usr/bin/env python3\nimport sys\na=sys.argv[1:]\n"
                 "if a[0:5]!=['@mac','inspect','codex/p','--after','0']:sys.exit(1)\n"
                 "if len(a)==7 and a[5]!='--archive':sys.exit(1)\n"
                 "print('{\"tracked\":true,\"cursor\":1,\"events\":[]}')\n"); script.close();
    QVERIFY(FakeCli::makeRunnable(script.fileName()));
    HgsClient client(FakeCli::program(script.fileName())); QSignalSpy ready(&client, &HgsClient::inspectionReady);
    client.requestInspection("mac", "codex/p");
    client.requestInspection("mac", "codex/p", 0, "old-one");
    client.requestInspection("mac", "codex/p", 0, "old-two");
    client.requestInspection("mac", "codex/p", 0, "old-one"); // Only this identical request is deduplicated.
    QTRY_COMPARE(ready.size(), 3);
    QSet<QString> identities; for (const auto &response : ready) identities.insert(response[3].toString());
    QCOMPARE(identities, QSet<QString>({QString(), "old-one", "old-two"}));
    QSignalSpy failed(&client, &HgsClient::inspectionFailed);
    client.requestInspection("mac", "codex/p", 5, "old-two"); QTRY_COMPARE(failed.size(), 1);
    QCOMPARE(failed[0][3].toString(), QString("old-two"));
}

void TestHgsClient::parsesProjects()
{
    const QByteArray json = R"([
      {"name":"sample-project","dir":"/workspace/sample-project","src":"ansible","exists":true},
      {"name":"scratch","dir":"/tmp/gone","src":"local","exists":false}])";
    QString err;
    const QList<ProjectInfo> ps = HgsClient::parseProjects(json, &err);
    QVERIFY2(err.isEmpty(), qPrintable(err));
    QCOMPARE(ps.size(), 2);
    QCOMPARE(ps[0].name, QStringLiteral("sample-project"));
    QCOMPARE(ps[0].dir, QStringLiteral("/workspace/sample-project"));
    QVERIFY(ps[0].fromAnsible);
    QVERIFY(ps[0].exists);
    QVERIFY(!ps[1].fromAnsible);
    QVERIFY(!ps[1].exists);
}

void TestHgsClient::remoteFoldersAndProjects()
{
    QTemporaryDir dir; QFile script(dir.filePath("hgs")); QVERIFY(script.open(QIODevice::WriteOnly));
    script.write("#!/usr/bin/env python3\nimport sys\nfor a in sys.argv[1:]:print(a,file=sys.stderr)\nsys.exit(1)\n"); script.close();
    QVERIFY(FakeCli::makeRunnable(script.fileName()));
    HgsClient client(FakeCli::program(script.fileName())); QSignalSpy writes(&client, &HgsClient::writeDone);
    client.projectSet("project", "/remote/path with ' quotes", "mac"); QTRY_COMPARE(writes.size(), 1);
    QCOMPARE(writes[0][2].toString(), QStringLiteral("@mac\nproject\nset\nproject\n/remote/path with ' quotes"));
    client.projectRemove("project", "mac"); QTRY_COMPARE(writes.size(), 2);
    QCOMPARE(writes[1][2].toString(), QStringLiteral("@mac\nproject\nrm\nproject"));
    QSignalSpy projects(&client, &HgsClient::projectsFailed);
    client.requestProjects("mac"); QTRY_COMPARE(projects.size(), 1);
    QCOMPARE(projects[0][0].toString(), QStringLiteral("mac"));
    QCOMPARE(projects[0][1].toString(), QStringLiteral("@mac\nproject\nls\n--json"));
    QSignalSpy folders(&client, &HgsClient::directoriesFailed);
    const auto request = client.requestDirectories("mac", "/remote/path with ' quotes", true);
    QTRY_COMPARE(folders.size(), 1);
    QCOMPARE(folders[0][0].toULongLong(), request);
    QCOMPARE(folders[0][1].toString(), QStringLiteral("mac"));
    QCOMPARE(folders[0][2].toString(), QStringLiteral("@mac\ndirs\n--hidden\n/remote/path with ' quotes"));
}

void TestHgsClient::rejectsGarbageProjects()
{
    QString err;
    const QList<ProjectInfo> ps = HgsClient::parseProjects(QByteArray("не json"), &err);
    QVERIFY(!err.isEmpty());
    QVERIFY(ps.isEmpty());
}

void TestHgsClient::parsesSharedReadState()
{
    const QString signature(64, 'a');
    const QJsonObject current{{"name", "codex/p/a"}, {"conversation_id", "c1"}, {"reply_id", "7:100.5"},
        {"attention_signature", signature}, {"read", QJsonObject{{"conversation_id", "c1"}, {"reply_id", "7:100.5"},
        {"attention_signature", QJsonValue()}, {"at", 1.0}}}};
    const QJsonObject otherConversation{{"name", "codex/p/b"}, {"conversation_id", "c2"}, {"attention_signature", QJsonValue()},
        {"read", QJsonObject{{"conversation_id", "old"}, {"reply_id", "1:1.0"}, {"attention_signature", QJsonValue()}, {"at", 1.0}}}};
    const QJsonObject olderHost{{"name", "codex/p/c"}, {"conversation_id", "c3"}};
    QString error;
    const auto box = HgsClient::parseBox(QJsonDocument(QJsonObject{{"host", "arch"}, {"ok", true},
        {"sessions", QJsonArray{current, otherConversation, olderHost}}}).toJson(), &error);
    QVERIFY(error.isEmpty());
    QVERIFY(box.sessions[0].sharedRead); QCOMPARE(box.sessions[0].attentionSignature, signature);
    QCOMPARE(box.sessions[0].readReplyId, QString("7:100.5")); QVERIFY(box.sessions[0].readAttentionSignature.isEmpty());
    QVERIFY(box.sessions[1].sharedRead); QVERIFY(box.sessions[1].readReplyId.isEmpty());
    QVERIFY(!box.sessions[2].sharedRead);
}

void TestHgsClient::readMarksUseStdinAndAcceptOnlyHostAnswers()
{
    QTemporaryDir directory; QFile script(directory.filePath("hgs")); QVERIFY(script.open(QIODevice::WriteOnly));
    script.write(R"(#!/usr/bin/env python3
import json,pathlib,sys
p=json.load(sys.stdin)
pathlib.Path(__file__).with_name('read.json').write_text(json.dumps({'argv':sys.argv[1:],'payload':p}))
mode=p['reply_id']
if mode=='missing': print(json.dumps({'ok':False,'error':'not_found'}));sys.exit(1)
if mode=='offline': print('ssh: connect to host mac port 22: Connection refused',file=sys.stderr);sys.exit(255)
if mode=='garbage': print('not json');sys.exit(0)
read={'conversation_id':'other' if mode=='wrong' else p['conversation_id'],'reply_id':p['reply_id'],'attention_signature':None,'at':1.0}
print(json.dumps({'ok':True,'applied':{'reply_id':True,'attention_signature':False},'read':read}))
)");
    script.close(); QVERIFY(FakeCli::makeRunnable(script.fileName()));
    HgsClient client(FakeCli::program(script.fileName())); QSignalSpy done(&client, &HgsClient::readFinished);
    const auto payload = [](const QString &reply) {
        return QJsonObject{{"run_id", "run-1"}, {"conversation_id", "conversation-1"}, {"reply_id", reply}, {"attention_signature", QJsonValue()}};
    };
    const QString name = "codex/project '$HOME' `literal`";
    const auto id = client.requestRead("mac", name, payload("7:100.5"));
    QTRY_COMPARE(done.size(), 1); QCOMPARE(done[0][0].toULongLong(), id); QVERIFY(done[0][1].toBool());
    QFile log(directory.filePath("read.json")); QVERIFY(log.open(QIODevice::ReadOnly));
    const auto call = QJsonDocument::fromJson(log.readAll()).object();
    QCOMPARE(call.value("argv").toArray(), QJsonArray({"@mac", "read", name, "--json"}));
    QCOMPARE(call.value("payload").toObject(), payload("7:100.5"));
    client.requestRead({}, "codex/project", payload("missing")); QTRY_COMPARE(done.size(), 2);
    QVERIFY(done[1][1].toBool()); QCOMPARE(done[1][2].toJsonObject(), (QJsonObject{{"ok", false}, {"error", "not_found"}}));
    for (const char *mode : {"offline", "garbage", "wrong"}) {
        const int before = done.size(); client.requestRead({}, "codex/project", payload(mode));
        QTRY_COMPARE(done.size(), before + 1); QVERIFY(!done.last()[1].toBool()); QVERIFY(!done.last()[3].toString().isEmpty());
    }
}

// APPLESS хватало на чистые parseBox/parseFleet, но missingBinaryEmitsFailed() гоняет
// настоящий QProcess и ждёт сигнал через QSignalSpy::wait() — для этого нужен реально
// работающий диспетчер событий. GUILESS даёт QCoreApplication без дисплея (тест
// по-прежнему должен работать по ssh без X/Wayland).
void TestHgsClient::presenceWritesAreCoalescedAndReportFailures()
{
    QTemporaryDir dir; QVERIFY(dir.isValid());
    const QString path = dir.filePath("hgs");
    QFile script(path); QVERIFY(script.open(QIODevice::WriteOnly));
    script.write(R"(#!/usr/bin/env python3
import json, pathlib, sys, time
root = pathlib.Path(__file__).parent
payload = json.load(sys.stdin)
with (root / 'calls').open('a') as file: file.write(json.dumps([sys.argv[1:], payload]) + '\n')
time.sleep(0.2)
if (root / 'fail').exists(): print('presence unavailable', file=sys.stderr); sys.exit(1)
print('{"ok":true}')
)");
    script.close(); QVERIFY(FakeCli::makeRunnable(script.fileName()));
    HgsClient client(FakeCli::program(path)); QSignalSpy finished(&client, &HgsClient::presenceFinished);
    client.setPresence(1, false); client.setPresence(2, false); client.setPresence(-3, true);
    QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 2, 10000);
    QFile calls(dir.filePath("calls")); QVERIFY(calls.open(QIODevice::ReadOnly));
    const auto lines = calls.readAll().trimmed().split('\n'); QCOMPARE(lines.size(), 2);
    const auto first = QJsonDocument::fromJson(lines[0]).array(), last = QJsonDocument::fromJson(lines[1]).array();
    QCOMPARE(first[0].toArray(), (QJsonArray{"presence", "set", "--json"}));
    QCOMPARE(first[1].toObject(), (QJsonObject{{"idle_seconds", 1}, {"locked", false}}));
    QCOMPARE(last[1].toObject(), (QJsonObject{{"idle_seconds", 0}, {"locked", true}})); // newest only, clamped
    QVERIFY(finished[0][0].toString().isEmpty()); QVERIFY(finished[1][0].toString().isEmpty());
    QFile fail(dir.filePath("fail")); QVERIFY(fail.open(QIODevice::WriteOnly)); fail.close();
    client.setPresence(5, false);
    QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 3, 10000);
    QVERIFY(finished[2][0].toString().contains("presence unavailable"));
}

QTEST_GUILESS_MAIN(TestHgsClient)
#include "test_hgsclient.moc"
