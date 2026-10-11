#include "UpdateController.h"
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
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
    void tool(const QString &path,const QByteArray &script){QFile file(path);QVERIFY(file.open(QIODevice::WriteOnly));file.write(script);file.close();QVERIFY(file.setPermissions(QFile::ReadOwner|QFile::WriteOwner|QFile::ExeOwner));}
private slots:
    void devCatalogAndNotificationsUseBuildIdentity(){
        auto value=raw();value["schemaVersion"]=2;value["channel"]="dev";value["channelTag"]="dev";value["buildId"]=12345;value["buildNumber"]=50;value.remove("releaseId");value.remove("releaseUrl");value["buildUrl"]="https://github.com/ufna/zerus/actions/runs/12345";value.remove("packages");
        value["artifacts"]=QJsonArray{
            QJsonObject{{"kind","arch-package"},{"name","zerus.pkg.tar.zst"},{"url","https://zerus.dev/downloads/dev-desktop-12345/zerus.pkg.tar.zst"},{"sha256",QString(64,'b')},{"size",100},{"os","linux"},{"arch","x86_64"}},
            QJsonObject{{"kind","source"},{"name","zerus-source.tar.gz"},{"url","https://zerus.dev/downloads/dev-desktop-12345/zerus-source.tar.gz"},{"sha256",QString(64,'c')},{"size",100},{"os","any"},{"arch","source"}}};
        auto feed=UpdateController::parseFeed(QJsonDocument(value).toJson(),"dev");QVERIFY(feed.valid);QCOMPARE(feed.buildNumber,50);QCOMPARE(feed.releaseId,12345);
        QVERIFY(!UpdateController::parseFeed(QJsonDocument(value).toJson(),"stable").valid);
        QVERIFY(UpdateController::devBuildIsNewer(feed,49,QString(40,'d')));
        QVERIFY(!UpdateController::devBuildIsNewer(feed,51,QString(40,'d')));
        QVERIFY(!UpdateController::devBuildIsNewer(feed,0,QString(40,'a')));
        QVERIFY(UpdateController::devBuildIsNewer(feed,0,{}));
        QVERIFY(UpdateController::isUpdateToken("zerus-update:v2:dev:12345"));
        QVERIFY(!UpdateController::isUpdateToken("zerus-update:v1:dev:12345"));
        QVERIFY(!UpdateController::isUpdateToken("zerus-update:v2:stable:12345"));
        QCOMPARE(UpdateController::automaticInterval("dev"),15*60);QCOMPARE(UpdateController::automaticInterval("stable"),24*60*60);
        DesktopInstallation installation{"package","zerus-ade-bin","0.39.0-1",true,"yay"};QVERIFY(UpdateController::updateCommand(installation,"dev").isEmpty());
        for(QString key:{"buildId","buildNumber","channelTag","buildUrl","sourceCommit"}){auto bad=value;bad[key]="bad";QVERIFY(!UpdateController::parseFeed(QJsonDocument(bad).toJson(),"dev").valid);}
        value["sourceDirty"]=false;QVERIFY(!UpdateController::parseFeed(QJsonDocument(value).toJson(),"dev").valid);
    }
    void devChannelPersistsAndChecksItsIndependentFeed(){
        QTemporaryDir dir;QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,dir.path());QSettings().clear();QSettings().setValue("updates/automatic",false);QString requested;
        UpdateController controller(nullptr,dir.path()+"/missing",[&](const QNetworkRequest &request){requested=request.url().toString();auto *reply=new FixtureReply;QTimer::singleShot(0,reply,[reply]{reply->finish("{}");});return reply;});
        controller.setChannel("dev");QCOMPARE(controller.channel(),QString("dev"));controller.check();QTRY_VERIFY(!controller.busy());QCOMPARE(requested,QString("https://zerus.dev/updates/v2/desktop/dev.json"));
        UpdateController restored(nullptr,dir.path()+"/missing");QCOMPARE(restored.channel(),QString("dev"));
    }
    void initTestCase(){QCoreApplication::setOrganizationName("zerus-update-test");QCoreApplication::setApplicationName("isolated");QSettings::setDefaultFormat(QSettings::IniFormat);}
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
};
QTEST_MAIN(TestUpdates)
#include "test_updates.moc"
