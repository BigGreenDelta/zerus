#include "UpdateController.h"
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QScopeGuard>
#include <QSettings>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QDateTime>
#include <QNetworkReply>
#include <QNetworkRequest>

class FixtureReply : public QNetworkReply {
    QByteArray bytes; qint64 position=0;
public:
    FixtureReply(){open(QIODevice::ReadOnly);setAttribute(QNetworkRequest::HttpStatusCodeAttribute,200);}
    void finish(const QByteArray &data){bytes=data;emit readyRead();setFinished(true);emit finished();}
    void abort() override{setError(QNetworkReply::OperationCanceledError,"canceled");setFinished(true);emit finished();}
    qint64 bytesAvailable() const override{return bytes.size()-position+QNetworkReply::bytesAvailable();}
protected:
    qint64 readData(char *data,qint64 max) override{qint64 count=qMin(max,bytes.size()-position);if(count==0)return -1;memcpy(data,bytes.constData()+position,count);position+=count;return count;}
};

class TestUpdates : public QObject {
    Q_OBJECT
    QJsonObject raw() const {
        return {{"schemaVersion",1},{"product","zerus"},{"platform","desktop"},{"channel","stable"},{"versionName","0.37.0"},{"releaseId",407198248},{"publishedAt","2026-10-09T12:00:00Z"},{"sourceCommit",QString(40,'a')},{"releaseUrl","https://github.com/ufna/zerus/releases/tag/v0.37.0"},
            {"packages",QJsonObject{{"zerus-ade-bin","0.37.0-1"},{"zerus-git","0.37.0.r10.gaaaaaaa-1"}}},
            {"artifacts",QJsonArray{QJsonObject{{"kind","arch-package"},{"name","zerus-ade-bin-0.37.0-1-x86_64.pkg.tar.zst"},{"url","https://zerus.dev/downloads/v0.37.0/zerus-ade-bin-0.37.0-1-x86_64.pkg.tar.zst"},{"sha256",QString(64,'b')},{"size",100},{"os","linux"},{"arch","x86_64"}}}}};
    }
    // A nightly catalog as scripts/update_feed.py publishes it (the Windows zip is
    // linked from the release page only, never listed as an artifact).
    QJsonObject nightlyRaw(const QString &version) const {
        const QString tag="nightly-123",name="zerus-ade-nightly-bin-"+version+"-1-x86_64.pkg.tar.zst";
        auto value=raw();value["channel"]="nightly";value["versionName"]=version;value["releaseUrl"]="https://github.com/ufna/zerus/releases/tag/"+tag;
        value["packages"]=QJsonObject{{"zerus-ade-nightly-bin",version+"-1"}};
        value["artifacts"]=QJsonArray{QJsonObject{{"kind","arch-package"},{"name",name},{"url","https://zerus.dev/downloads/"+tag+"/"+name},{"sha256",QString(64,'b')},{"size",100},{"os","linux"},{"arch","x86_64"}}};
        return value;
    }
    void tool(const QString &path,const QByteArray &script){QFile file(path);QVERIFY(file.open(QIODevice::WriteOnly));file.write(script);file.close();QVERIFY(file.setPermissions(QFile::ReadOwner|QFile::WriteOwner|QFile::ExeOwner));}
private slots:
    void initTestCase(){QCoreApplication::setOrganizationName("zerus-update-test");QCoreApplication::setApplicationName("isolated");QSettings::setDefaultFormat(QSettings::IniFormat);cleanup();}
    // Tests point the settings at temporary folders of their own. Later writes
    // (the next test, deferred work) go to empty settings that outlive the test
    // run instead of recreating a removed folder.
    void cleanup(){QVERIFY(m_settings.isValid());QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,m_settings.path());QSettings().clear();}
    void validCatalog(){auto value=UpdateController::parseFeed(QJsonDocument(raw()).toJson(),"stable");QVERIFY(value.valid);QCOMPARE(value.version,QString("0.37.0"));QCOMPARE(value.packages["zerus-git"].toString(),QString("0.37.0.r10.gaaaaaaa-1"));}
    void maliciousCatalog(){
        for(QString key:{"schemaVersion","product","platform","channel","releaseUrl","sourceCommit","publishedAt"}){auto value=raw();value[key]=key=="schemaVersion"?QJsonValue(2):QJsonValue("evil");QVERIFY2(!UpdateController::parseFeed(QJsonDocument(value).toJson(),"stable").valid,qPrintable(key));}
        for(QString key:{"name","url","sha256","size","os","arch","kind"}){auto value=raw();auto artifact=value["artifacts"].toArray()[0].toObject();artifact[key]=key=="size"?QJsonValue(1e30):QJsonValue("../evil");value["artifacts"]=QJsonArray{artifact};QVERIFY2(!UpdateController::parseFeed(QJsonDocument(value).toJson(),"stable").valid,qPrintable(key));}
        auto value=raw();value["sourceDirty"]=true;QVERIFY(!UpdateController::parseFeed(QJsonDocument(value).toJson(),"stable").valid);
        value=raw();auto array=value["artifacts"].toArray();array.append(array[0]);value["artifacts"]=array;QVERIFY(!UpdateController::parseFeed(QJsonDocument(value).toJson(),"stable").valid);
        QVERIFY(!UpdateController::parseFeed(QByteArray(256*1024+1,' '),"stable").valid);
    }
    void wrongChannelAndMissing(){auto value=raw();value.remove("artifacts");QVERIFY(!UpdateController::parseFeed(QJsonDocument(value).toJson(),"stable").valid);QVERIFY(!UpdateController::parseFeed(QJsonDocument(raw()).toJson(),"nightly").valid);}
    void safeInstructions(){
        DesktopInstallation package{"package","zerus-ade-bin","0.36.2-1",true,"paru"};QCOMPARE(UpdateController::updateCommand(package),QString("paru -Syu zerus-ade-bin"));
        package.package="zerus-git";QCOMPARE(UpdateController::updateCommand(package),QString("paru -Syu --devel"));
        package.package="unrecognized; sudo command";QVERIFY(UpdateController::updateCommand(package).isEmpty());
        package.kind="source";package.package="zerus";QCOMPARE(UpdateController::updateCommand(package),QString("paru -Syu zerus-ade-bin"));
        auto catalog=UpdateController::parseFeed(QJsonDocument(raw()).toJson(),"stable");
        DesktopInstallation unknown{"unknown",{},"0.99.0"};
        DesktopInstallation actual{"package","zerus-ade-bin","0.9.0-1"};
#ifdef Q_OS_MACOS
        const auto unavailable=QString("No macOS installer is published. See release or source build instructions.");
        QCOMPARE(UpdateController::comparisonMessage(unknown,catalog,"0.99.0",0,false),unavailable);
        QCOMPARE(UpdateController::comparisonMessage(actual,catalog,"0.9.0",1,true),unavailable);
        QCOMPARE(UpdateController::comparisonMessage(actual,catalog,"0.9.0",-1,true),unavailable);
        QCOMPARE(UpdateController::comparisonMessage(actual,catalog,"0.9.0",0,false),unavailable);
#else
        QVERIFY(UpdateController::comparisonMessage(unknown,catalog,"0.99.0",0,false).contains("unconfirmed"));
        QVERIFY(UpdateController::comparisonMessage(actual,catalog,"0.9.0",1,true).contains("Update available"));
        QVERIFY(UpdateController::comparisonMessage(actual,catalog,"0.9.0",-1,true).contains("at least"));
        QVERIFY(UpdateController::comparisonMessage(actual,catalog,"0.9.0",0,false).contains("No matching"));
#endif
    }
    void portableWindowsBuildFollowsNightlyReleases(){
        auto stable=UpdateController::parseFeed(QJsonDocument(raw()).toJson(),"stable");QVERIFY(stable.valid);
        auto nightly=UpdateController::parseFeed(QJsonDocument(nightlyRaw("0.38.0.r10.gaaaaaaa.n5")).toJson(),"nightly");QVERIFY(nightly.valid);
        DesktopInstallation portable{"portable",{},{}};
        // Stable releases carry no Windows build: never announced to a portable build.
        QVERIFY(!UpdateController::portableUpdateAvailable("0.36.2",stable));
        QCOMPARE(UpdateController::comparisonMessage(portable,stable,"0.36.2",0,false),QString("Stable releases do not include a Windows build yet. Choose the nightly channel to hear about newer Windows builds."));
        // Nightlies do; the zip knows only its product version.
        QVERIFY(UpdateController::portableUpdateAvailable("0.37.0",nightly));
        QCOMPARE(UpdateController::comparisonMessage(portable,nightly,"0.37.0",0,false),QString("Zerus nightly 0.38.0.r10.gaaaaaaa.n5 is available. Download its Windows zip from the release page."));
        QVERIFY(!UpdateController::portableUpdateAvailable("0.38.0",nightly));QVERIFY(!UpdateController::portableUpdateAvailable("0.39.0",nightly));
        QVERIFY(!UpdateController::portableUpdateAvailable("0.38.0-dev",nightly));QVERIFY(!UpdateController::portableUpdateAvailable("0.37.0",DesktopUpdateFeed{}));
        QVERIFY(UpdateController::comparisonMessage(portable,nightly,"0.38.0",0,false).contains("no self-update"));
        QVERIFY(UpdateController::updateCommand(portable).isEmpty());QVERIFY(UpdateController::updateCommand(portable,"nightly").isEmpty());
        // The catalog itself is unchanged: a Windows client accepts exactly what Linux clients do.
        for(auto value:{raw(),nightlyRaw("0.38.0.r10.gaaaaaaa.n5")}){
            const QString tag=value["releaseUrl"].toString().section('/',-1),channel=value["channel"].toString();
            auto artifacts=value["artifacts"].toArray();
            artifacts.append(QJsonObject{{"kind","windows-zip"},{"name","zerus-windows-x86_64.zip"},{"url","https://zerus.dev/downloads/"+tag+"/zerus-windows-x86_64.zip"},{"sha256",QString(64,'c')},{"size",100},{"os","windows"},{"arch","x86_64"}});
            value["artifacts"]=artifacts;QVERIFY2(!UpdateController::parseFeed(QJsonDocument(value).toJson(),channel).valid,qPrintable(channel));
        }
    }
    void portableBuildDefaultsToNightlyAndNeverAnnouncesStable(){
#ifdef Q_OS_WIN
        QTemporaryDir dir;QVERIFY(dir.isValid());QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,dir.path());QSettings().clear();QSettings().setValue("updates/automatic",false);
        const auto version=QCoreApplication::applicationVersion();QCoreApplication::setApplicationVersion("0.37.0");const auto restore=qScopeGuard([version]{QCoreApplication::setApplicationVersion(version);});
        QFile executable(dir.path()+"/hgs-tray.exe");QVERIFY(executable.open(QIODevice::WriteOnly));executable.close();
        QFile info(dir.path()+"/build-info.json");QVERIFY(info.open(QIODevice::WriteOnly));info.close();
        const QByteArray stableFeed=QJsonDocument(raw()).toJson(),nightlyFeed=QJsonDocument(nightlyRaw("0.38.0.r10.gaaaaaaa.n5")).toJson();QStringList requested;
        const auto fetch=[&](const QNetworkRequest &request){requested<<request.url().fileName();auto *reply=new FixtureReply;const auto bytes=request.url().fileName()=="nightly.json"?nightlyFeed:stableFeed;QTimer::singleShot(0,reply,[reply,bytes]{reply->finish(bytes);});return reply;};
        {
            UpdateController controller(nullptr,executable.fileName(),fetch);QSignalSpy notified(&controller,&UpdateController::newerReleaseVerified);
            QTRY_COMPARE(controller.installation().kind,QString("portable"));QCOMPARE(controller.channel(),QString("nightly"));
            controller.check();QTRY_VERIFY(!controller.busy());QCOMPARE(requested,QStringList{"nightly.json"});
            QVERIFY(controller.updateAvailable());QCOMPARE(notified.size(),1);QCOMPARE(controller.feed().releaseUrl,QString("https://github.com/ufna/zerus/releases/tag/nightly-123"));
            // A deliberate stable choice is kept, and a newer stable release is not announced.
            controller.setChannel("stable");controller.check();QTRY_VERIFY(!controller.busy());QCOMPARE(requested.last(),QString("stable.json"));
            QVERIFY(controller.feed().valid);QVERIFY(!controller.updateAvailable());QCOMPARE(notified.size(),1);QVERIFY(controller.comparison().contains("nightly channel"));
        }
        UpdateController restarted(nullptr,executable.fileName(),fetch);QTRY_COMPARE(restarted.installation().kind,QString("portable"));QCOMPARE(restarted.channel(),QString("stable"));
#else
        QSKIP("Only Windows builds are portable");
#endif
    }
    void installationKindFollowsTheFolderLayout(){
        QTemporaryDir dir;QVERIFY(dir.isValid());
#ifdef Q_OS_WIN
        // Wherever the temporary folder lies: the portable bundle (build-info.json
        // next to hgs-tray.exe) stays portable inside a folder kept in Git, such as
        // a profile, while a build tree in that checkout is a source build.
        for(const char *folder:{"checkout/.git","checkout/bundle","checkout/build"})QVERIFY(QDir(dir.path()).mkpath(folder));
        for(const char *file:{"checkout/bundle/hgs-tray.exe","checkout/bundle/build-info.json","checkout/build/hgs-tray.exe"}){QFile created(dir.filePath(file));QVERIFY(created.open(QIODevice::WriteOnly));}
        UpdateController bundle(nullptr,dir.filePath("checkout/bundle/hgs-tray.exe"),[](const QNetworkRequest &){return new FixtureReply;});
        UpdateController build(nullptr,dir.filePath("checkout/build/hgs-tray.exe"),[](const QNetworkRequest &){return new FixtureReply;});
        QTRY_COMPARE(bundle.installation().kind,QString("portable"));QTRY_COMPARE(build.installation().kind,QString("source"));
#else
        QFile executable(dir.path()+"/hgs-tray.exe");QVERIFY(executable.open(QIODevice::WriteOnly));executable.close();
        UpdateController controller(nullptr,executable.fileName(),[](const QNetworkRequest &){return new FixtureReply;});
#ifdef Q_OS_LINUX
        QTRY_VERIFY(controller.installation().kind!="checking");QVERIFY(controller.installation().kind!="portable");
#else
        QTRY_COMPARE(controller.installation().kind,QString("unknown"));
#endif
#endif
    }
    void canonicalOwnershipAsync(){
#ifdef Q_OS_LINUX
        QTemporaryDir dir;QVERIFY(dir.isValid());QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,dir.path());QSettings().clear();auto oldPath=qgetenv("PATH");qputenv("PATH",dir.path().toUtf8()+":"+oldPath);qputenv("ZERUS_TEST_LOG",(dir.path()+"/args.json").toUtf8());
        tool(dir.path()+"/pacman","#!/usr/bin/python3\nimport os,sys,json\nif sys.argv[1]=='-Qqo':\n open(os.environ['ZERUS_TEST_LOG'],'w').write(json.dumps(sys.argv[2:]))\n print('zerus-ade-nightly-bin')\nelse: print('zerus-ade-nightly-bin 0.37.0.r100.gaaaaaaa.n1-1')\n");
        QFile binary(dir.path()+"/actual-gui");QVERIFY(binary.open(QIODevice::WriteOnly));binary.close();QVERIFY(QFile::link(binary.fileName(),dir.path()+"/gui-link"));
        UpdateController controller(nullptr,dir.path()+"/gui-link");QSignalSpy changes(&controller,&UpdateController::changed);QVERIFY(changes.wait(2000));QTRY_VERIFY_WITH_TIMEOUT(!controller.installation().version.isEmpty(),2000);QCOMPARE(controller.installation().package,QString("zerus-ade-nightly-bin"));QCOMPARE(controller.channel(),QString("nightly"));
        QFile log(dir.path()+"/args.json");QVERIFY(log.open(QIODevice::ReadOnly));auto args=QJsonDocument::fromJson(log.readAll()).array();QCOMPARE(args.last().toString(),QFileInfo(binary).canonicalFilePath());QCOMPARE(args.first().toString(),QString("--"));qputenv("PATH",oldPath);qunsetenv("ZERUS_TEST_LOG");
#endif
    }
    void unknownOwnerNeverOffersReplacement(){
#ifdef Q_OS_LINUX
        QTemporaryDir dir;auto oldPath=qgetenv("PATH");qputenv("PATH",dir.path().toUtf8()+":"+oldPath);tool(dir.path()+"/pacman","#!/usr/bin/python3\nimport sys\nprint('custom-owner' if sys.argv[1]=='-Qqo' else 'custom-owner 2.1-1')\n");QFile executable(dir.path()+"/gui");QVERIFY(executable.open(QIODevice::WriteOnly));executable.close();UpdateController controller(nullptr,executable.fileName());QTRY_VERIFY_WITH_TIMEOUT(!controller.installation().version.isEmpty(),2000);QCOMPARE(controller.installation().package,QString("custom-owner"));QVERIFY(UpdateController::updateCommand(controller.installation()).isEmpty());qputenv("PATH",oldPath);
#endif
    }
    void verifiedCatalogSurvivesRestartAndRejectsCorruption(){
        QTemporaryDir dir;QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,dir.path());QSettings().clear();QSettings().setValue("updates/automatic",false);QSettings().setValue("updates/channel","stable");QSettings().setValue("updates/catalog/stable",QJsonDocument(raw()).toJson());QSettings().setValue("updates/lastSuccess/stable",QDateTime::currentSecsSinceEpoch()-30);
        UpdateController restored(nullptr,dir.path()+"/missing");QVERIFY(restored.feed().valid);QVERIFY(restored.message().contains("cached"));restored.setChannel("nightly");QVERIFY(!restored.feed().valid);restored.setChannel("stable");QVERIFY(restored.feed().valid);
        QSettings().setValue("updates/catalog/stable",QByteArray("malformed"));UpdateController corrupt(nullptr,dir.path()+"/missing");QVERIFY(!corrupt.feed().valid);
    }
    void archVersionComparisonUsesEpochAndPackageRelease(){
#ifdef Q_OS_LINUX
        if(QStandardPaths::findExecutable("vercmp").isEmpty())QSKIP("Arch vercmp is unavailable");
        QTemporaryDir dir;QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,dir.path());QSettings().clear();QSettings().setValue("updates/channel","stable");auto value=raw();value["packages"]=QJsonObject{{"zerus-ade-bin","0.37.0-10"}};QSettings().setValue("updates/catalog/stable",QJsonDocument(value).toJson());QSettings().setValue("updates/lastSuccess/stable",QDateTime::currentSecsSinceEpoch()-30);
        auto oldPath=qgetenv("PATH");qputenv("PATH",dir.path().toUtf8()+":"+oldPath);tool(dir.path()+"/pacman","#!/usr/bin/python3\nimport os,sys\nprint('zerus-ade-bin' if sys.argv[1]=='-Qqo' else 'zerus-ade-bin '+os.environ['ZERUS_TEST_VERSION'])\n");QFile executable(dir.path()+"/gui");QVERIFY(executable.open(QIODevice::WriteOnly));executable.close();
        qputenv("ZERUS_TEST_VERSION","0.37.0-2");{UpdateController controller(nullptr,executable.fileName());QTRY_VERIFY_WITH_TIMEOUT(controller.message().contains("Update available"),2000);}
        qputenv("ZERUS_TEST_VERSION","1:0.37.0-2");{UpdateController controller(nullptr,executable.fileName());QTRY_VERIFY_WITH_TIMEOUT(controller.message().contains("at least"),2000);}
        qputenv("PATH",oldPath);qunsetenv("ZERUS_TEST_VERSION");
#endif
    }
    void distroParsingAndOptionalCommands(){
        QVERIFY(UpdateController::isArchLinux("ID=arch\n"));QVERIFY(UpdateController::isArchLinux("ID=manjaro\nID_LIKE=\"arch linux\"\n"));
        for(QByteArray value:{QByteArray("ID=ubuntu\nID_LIKE=debian"),QByteArray("ID=arch\nID=ubuntu"),QByteArray("ID=\"$(false)\""),QByteArray(8193,'x')})QVERIFY(!UpdateController::isArchLinux(value));
        DesktopInstallation local{"unknown",{},{},true,"yay"};QCOMPARE(UpdateController::updateCommand(local),QString("yay -Syu zerus-ade-bin"));QCOMPARE(UpdateController::updateCommand(local,"nightly"),QString("yay -Syu zerus-ade-nightly-bin"));local.arch=false;QVERIFY(UpdateController::updateCommand(local).isEmpty());local.arch=true;local.helper.clear();QVERIFY(UpdateController::updateCommand(local).isEmpty());
        QVERIFY(UpdateController::plainStableIsNewer("0.9.0","0.10.0"));QVERIFY(!UpdateController::plainStableIsNewer("0.37.0-dev","0.38.0"));QVERIFY(!UpdateController::plainStableIsNewer("0.37.0","0.37.0"));
    }
    void optOutStopsAutomaticRequestsAndKeepsManualCheck(){
        QTemporaryDir dir;QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,dir.path());QSettings().clear();QSettings().setValue("updates/automatic",false);int requests=0;
        UpdateController controller(nullptr,dir.path()+"/missing",[&](const QNetworkRequest &request){++requests;if(request.url()!=QUrl("https://zerus.dev/updates/v1/desktop/stable.json"))qFatal("Unexpected test endpoint");auto *reply=new FixtureReply;QTimer::singleShot(0,reply,[reply,this]{reply->finish(QJsonDocument(raw()).toJson());});return reply;});
        controller.checkOnStart();controller.checkAutomatically();controller.setChannel("nightly");controller.checkAutomatically();controller.setChannel("stable");QTest::qWait(50);QCOMPARE(requests,0);
        controller.check();QTRY_COMPARE(requests,1);QTRY_VERIFY(!controller.busy());QVERIFY(controller.feed().valid);QVERIFY(!controller.automaticChecks());
        UpdateController restored(nullptr,dir.path()+"/missing");QVERIFY(!restored.automaticChecks());
    }
    void selectedChannelChecksImmediatelyOnlyWhenEnabledAndDue(){
        QTemporaryDir dir;QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,dir.path());QSettings().clear();int requests=0;QString selected;
        UpdateController controller(nullptr,dir.path()+"/missing",[&](const QNetworkRequest &request){++requests;selected=request.url().fileName();return new FixtureReply;});QTRY_COMPARE(controller.installation().kind,QString("unknown"));controller.setChannel("nightly");QCOMPARE(requests,1);QCOMPARE(selected,QString("nightly.json"));controller.setChannel("nightly");QCOMPARE(requests,1);controller.setAutomaticChecks(false);controller.setChannel("stable");QCOMPARE(requests,1);
    }
    void disablingChecksCancelsInFlightAutomaticRequest(){
        QTemporaryDir dir;QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,dir.path());QSettings().clear();QPointer<FixtureReply> pending;int requests=0;
        UpdateController controller(nullptr,dir.path()+"/missing",[&](const QNetworkRequest &){++requests;pending=new FixtureReply;return pending.data();});QTRY_COMPARE(controller.installation().kind,QString("unknown"));controller.checkAutomatically();QCOMPARE(requests,1);QVERIFY(controller.busy());controller.setAutomaticChecks(false);QVERIFY(!controller.busy());QSignalSpy notify(&controller,&UpdateController::newerReleaseVerified);QTest::qWait(30);QCOMPARE(notify.count(),0);controller.checkAutomatically();QCOMPARE(requests,1);
    }
    void optOutSuppressesDelayedAutomaticComparisonNotification(){
#ifdef Q_OS_LINUX
        QTemporaryDir dir;QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,dir.path());QSettings().clear();auto oldPath=qgetenv("PATH");qputenv("PATH",dir.path().toUtf8()+":"+oldPath);
        tool(dir.path()+"/pacman","#!/usr/bin/python3\nimport sys\nprint('zerus-ade-bin' if sys.argv[1]=='-Qqo' else 'zerus-ade-bin 0.36.2-1')\n");tool(dir.path()+"/vercmp","#!/usr/bin/python3\nimport time\ntime.sleep(0.4)\nprint(1)\n");QFile executable(dir.path()+"/gui");QVERIFY(executable.open(QIODevice::WriteOnly));executable.close();
        UpdateController controller(nullptr,executable.fileName(),[&](const QNetworkRequest &){auto *reply=new FixtureReply;QTimer::singleShot(0,reply,[reply,this]{reply->finish(QJsonDocument(raw()).toJson());});return reply;});QSignalSpy notify(&controller,&UpdateController::newerReleaseVerified);QTRY_VERIFY(!controller.installation().version.isEmpty());controller.checkAutomatically();QTRY_VERIFY(!controller.busy());controller.setAutomaticChecks(false);QTRY_VERIFY(controller.updateAvailable());QCOMPARE(notify.count(),0);
        controller.check();QTRY_COMPARE(notify.count(),1);qputenv("PATH",oldPath);
#endif
    }
    void notificationsAreFreshNewerOnlyAndDeduplicated(){
        QTemporaryDir dir;QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,dir.path());QSettings().clear();QSettings().setValue("updates/automatic",false);QCoreApplication::setApplicationVersion("0.36.2");QSettings().setValue("updates/catalog/stable",QJsonDocument(raw()).toJson());QSettings().setValue("updates/lastSuccess/stable",QDateTime::currentSecsSinceEpoch()-30);
        UpdateController controller(nullptr,dir.path()+"/missing",[&](const QNetworkRequest &){auto *reply=new FixtureReply;QTimer::singleShot(0,reply,[reply,this]{reply->finish(QJsonDocument(raw()).toJson());});return reply;});QSignalSpy notify(&controller,&UpdateController::newerReleaseVerified);QTRY_VERIFY(controller.updateAvailable());QCOMPARE(notify.count(),0);
        controller.check();QTRY_COMPARE(notify.count(),1);QCOMPARE(notify[0][0].toString(),QString("zerus-update:v1:stable:407198248"));QVERIFY(UpdateController::isUpdateToken(controller.updateToken()));QVERIFY(!UpdateController::isUpdateToken("session-attention:407198248"));controller.check();QTRY_VERIFY(!controller.busy());QCOMPARE(notify.count(),1);
        QCoreApplication::setApplicationVersion("0.37.0-dev");controller.check();QTRY_VERIFY(!controller.busy());QVERIFY(!controller.updateAvailable());QCOMPARE(notify.count(),1);
    }
    void subprocessTimeoutLeavesEventLoopResponsive(){
#ifdef Q_OS_LINUX
        QTemporaryDir dir;auto oldPath=qgetenv("PATH");qputenv("PATH",dir.path().toUtf8()+":"+oldPath);tool(dir.path()+"/pacman","#!/usr/bin/python3\nimport time\ntime.sleep(20)\n");QFile executable(dir.path()+"/gui");QVERIFY(executable.open(QIODevice::WriteOnly));executable.close();UpdateController controller(nullptr,executable.fileName());bool tick=false;QTimer::singleShot(30,[&]{tick=true;});QTRY_VERIFY_WITH_TIMEOUT(tick,1000);QTRY_COMPARE_WITH_TIMEOUT(controller.installation().kind,QString("unknown"),6500);qputenv("PATH",oldPath);
#endif
    }
private:
    QTemporaryDir m_settings;
};
QTEST_MAIN(TestUpdates)
#include "test_updates.moc"
