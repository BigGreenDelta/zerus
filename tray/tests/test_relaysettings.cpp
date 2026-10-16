#include <QtTest>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include "RelaySettings.h"

class TestRelaySettings : public QObject {
    Q_OBJECT
    // macOS temporary paths may start at /var, a system symlink to /private/var.
    // Resolve only the existing test root; production no-follow traversal stays strict.
    static QString temporaryRoot(const QTemporaryDir &dir){return QFileInfo(dir.path()).canonicalFilePath();}
    static RelaySettings::Snapshot original(const QString &state){
        RelaySettings::Snapshot old;
        old.exists=true;
        old.object={{"server_url","https://relay.example.test"},{"node_token","synthetic-old-node-token"},
                    {"state_dir",state},{"hgs_path","/fixture/hgs"},{"poll_interval",7},{"extra",QJsonObject{{"keep",true}}}};
        return old;
    }
    static bool write(const QString &path,const QJsonObject &object){
        QFile file(path);if(!file.open(QIODevice::WriteOnly))return false;
        file.setPermissions(QFile::ReadOwner|QFile::WriteOwner);
        return file.write(QJsonDocument(object).toJson())>0;
    }
    static QString connectorConfig(const QTemporaryDir &dir){
        const auto path=temporaryRoot(dir)+"/connector.json";
        return write(path,{{"server_url","https://relay.example.test"},{"node_token","synthetic-node-token"},
                           {"state_dir",temporaryRoot(dir)+"/state"}})?path:QString();
    }
    // A status tool on PATH that answers only the panel's exact read-only probe.
    static bool fakeTool(const QTemporaryDir &dir,const QString &name,const QByteArray &arguments,const QByteArray &output,int code=0){
        QFile answer(temporaryRoot(dir)+"/"+name+".answer");if(!answer.open(QIODevice::WriteOnly))return false;answer.write(output);answer.close();
        QFile program(temporaryRoot(dir)+"/"+name);if(!program.open(QIODevice::WriteOnly))return false;
        program.write("#!/bin/sh\ncase \"$*\" in '"+arguments+"') ;; *) exit 1 ;; esac\ncat '"+answer.fileName().toUtf8()+"'\nexit "+QByteArray::number(code)+"\n");
        program.close();
        return QFile::setPermissions(program.fileName(),QFile::ReadOwner|QFile::WriteOwner|QFile::ExeOwner);
    }
    static constexpr char Probe[]="--user show zerus-mobile-connector.service --property=LoadState,ActiveState,SubState,KillMode,UnitFileState,FragmentPath,DropInPaths,ExecStart --no-pager";
private slots:
    void managedMigrationPreservesExactIdentityAndOtherKeys(){
        auto old=original("/fixture/receipts");
        old.object.insert("server_url","https://ZERUS.dev.guthub.dev:443/");
        QString error;
        auto p=RelaySettings::prepare(old,{RelaySettings::canonicalRelay(),{},"/fixture/receipts"},&error);
        QVERIFY2(p.has_value(),qPrintable(error));QVERIFY(!p->newState);
        QCOMPARE(p->object.value("identity_url").toString(),QString("https://ZERUS.dev.guthub.dev:443"));
        QCOMPARE(p->object.value("node_token"),old.object.value("node_token"));
        QCOMPARE(p->object.value("extra"),old.object.value("extra"));
        QCOMPARE(p->object.value("hgs_path"),old.object.value("hgs_path"));
        QCOMPARE(p->object.value("poll_interval"),old.object.value("poll_interval"));
    }
    void canonicalMigrationRemainsStableOnSecondSave(){
        auto old=original("/fixture/receipts");old.object.insert("server_url",RelaySettings::canonicalRelay());
        old.object.insert("identity_url","https://zerus.dev.guthub.dev:443");
        QString error;auto p=RelaySettings::prepare(old,{RelaySettings::canonicalRelay(),{},"/fixture/receipts"},&error);
        QVERIFY(p);QCOMPARE(p->object.value("identity_url"),old.object.value("identity_url"));QVERIFY(!p->newState);
    }
    void canonicalSpellingChangeCannotReuseJournal(){
        auto old=original("/fixture/receipts");old.object.insert("server_url","https://RELAY.zerus.dev:443");
        QString error;
        QVERIFY(!RelaySettings::prepare(old,{RelaySettings::canonicalRelay(),{},"/fixture/receipts"},&error));
        QVERIFY(RelaySettings::prepare(old,{"https://RELAY.zerus.dev:443",{},"/fixture/receipts"},&error));
        old.object.insert("identity_url","https://zerus.dev.guthub.dev:443");
        auto p=RelaySettings::prepare(old,{RelaySettings::canonicalRelay(),{},"/fixture/receipts"},&error);
        QVERIFY(p);QCOMPARE(p->object.value("identity_url").toString(),QString("https://zerus.dev.guthub.dev:443"));
    }
    void invalidIdentityDoesNotBecomeValidOnSave(){
        auto old=original("/fixture/receipts");old.object.insert("identity_url","https://different.example.test");
        QString error;QVERIFY(!RelaySettings::prepare(old,{"https://relay.example.test",{},"/fixture/receipts"},&error));
    }
    void foreignRelayRequiresNewCredentialAndDirectory(){
        QTemporaryDir dir;QVERIFY(dir.isValid());auto old=original(temporaryRoot(dir)+"/old");QString error;
        QVERIFY(!RelaySettings::prepare(old,{"https://other.example.test",{},temporaryRoot(dir)+"/new"},&error));
        QVERIFY(!RelaySettings::prepare(old,{"https://other.example.test","synthetic-old-node-token",temporaryRoot(dir)+"/new"},&error));
        QVERIFY(!RelaySettings::prepare(old,{"https://other.example.test","synthetic-new-node-token",temporaryRoot(dir)+"/old"},&error));
        auto p=RelaySettings::prepare(old,{"https://other.example.test","synthetic-new-node-token",temporaryRoot(dir)+"/new"},&error);
        QVERIFY2(p.has_value(),qPrintable(error));QVERIFY(p->newState);QVERIFY(!p->object.contains("identity_url"));
        QVERIFY(QDir().mkdir(temporaryRoot(dir)+"/new"));
        QVERIFY(!RelaySettings::prepare(old,{"https://other.example.test","synthetic-new-node-token",temporaryRoot(dir)+"/new"},&error));
    }
    void sameRelayCredentialRotationNeedsNewJournal(){
        auto old=original("/fixture/old");QString error;
        QVERIFY(!RelaySettings::prepare(old,{"https://relay.example.test","synthetic-new-node-token","/fixture/old"},&error));
        QVERIFY(RelaySettings::prepare(old,{"https://relay.example.test","synthetic-new-node-token","/fixture/new-unused"},&error));
    }
    void unsafeUrlsAndTokensRejected(){
        auto old=original("/fixture/old");QString error;
        for(const auto &url:QStringList{"http://relay.example.test","https://user:secret@relay.example.test","https://@relay.example.test","https://relay.example.test?key=secret","https://relay.example.test#fragment","https://relay.example.test\n"})
            QVERIFY(!RelaySettings::prepare(old,{url,{},"/fixture/old"},&error));
        QVERIFY(!RelaySettings::prepare(old,{"https://relay.example.test","invalid token","/fixture/old"},&error));
        QVERIFY(!RelaySettings::prepare(old,{"https://zerus.dev.guthub.dev",{},"/fixture/old"},&error));
        QVERIFY(RelaySettings::managedOrigin("https://zerus.dev.guthub.dev.evil.test").isEmpty());
        QVERIFY(RelaySettings::managedOrigin("https://zerus.dev.guthub.dev/other").isEmpty());
    }
#if defined(__unix__) || defined(__APPLE__)
    void pendingStatusProbeIsCancelledBeforeWidgetTeardown(){
#ifdef Q_OS_LINUX
        QTemporaryDir dir;QVERIFY(dir.isValid());const auto executable=temporaryRoot(dir)+"/systemctl";
        QFile program(executable);QVERIFY(program.open(QIODevice::WriteOnly));
        program.write("#!/bin/sh\nexec /bin/sleep 30\n");program.close();
        QVERIFY(QFile::setPermissions(executable,QFile::ReadOwner|QFile::WriteOwner|QFile::ExeOwner));
        const auto previous=qgetenv("PATH");qputenv("PATH",temporaryRoot(dir).toUtf8()+":"+previous);
        auto *panel=new RelaySettings::Panel(temporaryRoot(dir)+"/connector.json");
        auto *process=panel->findChild<QProcess*>();
        const bool started=process&&process->waitForStarted(1000);qputenv("PATH",previous);
        QVERIFY(started);QCOMPARE(process->state(),QProcess::Running);
        QElapsedTimer elapsed;elapsed.start();delete panel;
        QVERIFY(elapsed.elapsed()<2000);QCoreApplication::processEvents();
#endif
    }
    void restartCommandRequiresProcessKillMode_data(){
        QTest::addColumn<QByteArray>("output");QTest::addColumn<bool>("restart");QTest::addColumn<bool>("protect");
        const QByteArray active="LoadState=loaded\nActiveState=active\nSubState=running\n";
        QTest::newRow("process")<<active+"KillMode=process\n"<<true<<false;
        QTest::newRow("control-group")<<active+"KillMode=control-group\n"<<false<<true;
        QTest::newRow("unreported")<<active<<false<<true;
        QTest::newRow("not installed")<<QByteArray("LoadState=not-found\nActiveState=inactive\nSubState=dead\nKillMode=control-group\n")<<false<<false;
    }
    void restartCommandRequiresProcessKillMode(){
#ifdef Q_OS_LINUX
        QFETCH(QByteArray,output);QFETCH(bool,restart);QFETCH(bool,protect);
        QTemporaryDir dir;QVERIFY(dir.isValid());const auto config=connectorConfig(dir);QVERIFY(!config.isEmpty());
        // Answer only the read-only probe, and only when it asks for the kill mode.
        QVERIFY(fakeTool(dir,"systemctl",Probe,output));
        const auto previous=qgetenv("PATH");qputenv("PATH",temporaryRoot(dir).toUtf8()+":"+previous);
        RelaySettings::Panel panel(config);
        auto *process=panel.findChild<QProcess*>();
        const bool started=process&&process->waitForStarted(1000);qputenv("PATH",previous);QVERIFY(started);
        auto *status=panel.findChild<QLabel*>("connectorServiceStatus");auto *notice=panel.findChild<QLabel*>("connectorKillMode");
        auto *restartRow=panel.findChild<QWidget*>("connectorRestartCommand");auto *protectRow=panel.findChild<QWidget*>("connectorProtectCommand");
        QVERIFY(status&&notice&&restartRow&&protectRow);
        QTRY_VERIFY(!status->text().contains(QStringLiteral("checking")));
        QCOMPARE(!restartRow->isHidden(),restart);QCOMPARE(!protectRow->isHidden(),protect);QCOMPARE(!notice->isHidden(),restart||protect);
        QVERIFY(!panel.findChild<QWidget*>("connectorStartCommand")->isHidden());
        const auto *command=protectRow->findChild<QLabel*>();QVERIFY(command);
        QVERIFY(command->text().contains(QStringLiteral("zerus-mobile-connector.service.d/50-zerus-killmode.conf")));
        QVERIFY(command->text().contains(QStringLiteral("KillMode=process")));
        QVERIFY(!command->text().contains(QStringLiteral("restart")));
        if(protect)QVERIFY(notice->text().contains(output.contains("KillMode=")?QStringLiteral("KillMode=control-group"):QStringLiteral("KillMode=unknown")));
#endif
    }
    void reloadOffersRestartOnceProtectionApplies(){
#ifdef Q_OS_LINUX
        QTemporaryDir dir;QVERIFY(dir.isValid());const auto executable=temporaryRoot(dir)+"/systemctl",answer=temporaryRoot(dir)+"/answer";
        const auto reply=[&answer](const QByteArray &mode){QFile file(answer);return file.open(QIODevice::WriteOnly)&&file.write("LoadState=loaded\nActiveState=active\nSubState=running\nKillMode="+mode+"\n")>0;};
        QVERIFY(reply("control-group"));
        QFile program(executable);QVERIFY(program.open(QIODevice::WriteOnly));
        program.write("#!/bin/sh\nexec cat '"+answer.toUtf8()+"'\n");program.close();
        QVERIFY(QFile::setPermissions(executable,QFile::ReadOwner|QFile::WriteOwner|QFile::ExeOwner));
        // Reload starts a second probe, which must find the same systemctl.
        const auto previous=qgetenv("PATH");qputenv("PATH",temporaryRoot(dir).toUtf8()+":"+previous);
        const auto restore=qScopeGuard([previous]{qputenv("PATH",previous);});
        const auto config=connectorConfig(dir);QVERIFY(!config.isEmpty());
        RelaySettings::Panel panel(config);
        auto *restartRow=panel.findChild<QWidget*>("connectorRestartCommand");auto *protectRow=panel.findChild<QWidget*>("connectorProtectCommand");
        auto *reload=panel.findChild<QPushButton*>("reloadRelayConfiguration");QVERIFY(restartRow&&protectRow&&reload);
        QTRY_VERIFY(!protectRow->isHidden());QVERIFY(restartRow->isHidden());
        // The user ran the protection command; Reload configuration probes again.
        QVERIFY(reply("process"));reload->click();
        QTRY_VERIFY(!restartRow->isHidden());QVERIFY(protectRow->isHidden());
#endif
    }
    void missingConfigurationComesFirst(){
#ifdef Q_OS_LINUX
        QTemporaryDir dir;QVERIFY(dir.isValid());
        QVERIFY(fakeTool(dir,"systemctl",Probe,"LoadState=loaded\nActiveState=active\nSubState=running\nKillMode=process\nUnitFileState=enabled\n"));
        const auto previous=qgetenv("PATH");qputenv("PATH",temporaryRoot(dir).toUtf8()+":"+previous);
        const auto restore=qScopeGuard([previous]{qputenv("PATH",previous);});
        RelaySettings::Panel panel(temporaryRoot(dir)+"/connector.json");
        auto *status=panel.findChild<QLabel*>("connectorServiceStatus");QVERIFY(status);
        QTRY_VERIFY(!status->text().contains(QStringLiteral("checking")));
        QVERIFY2(status->text().startsWith(QStringLiteral("Connector: not configured.")),qPrintable(status->text()));
        QVERIFY(status->text().contains(QStringLiteral("running")));
        // Nothing to start or restart without a configuration; the kill mode still shows.
        QVERIFY(panel.findChild<QWidget*>("connectorStartCommand")->isHidden());
        QVERIFY(panel.findChild<QWidget*>("connectorRestartCommand")->isHidden());
        QVERIFY(!panel.findChild<QLabel*>("connectorKillMode")->isHidden());
#endif
    }
    void implementationFollowsTheRunningProgram_data(){
        QTest::addColumn<QString>("program");QTest::addColumn<QByteArray>("dropIns");QTest::addColumn<QString>("expected");
        QTest::newRow("python venv")<<QStringLiteral("venv")<<QByteArray()<<QStringLiteral("legacy Python connector");
        QTest::newRow("rust package")<<QStringLiteral("/usr/bin/zerus-mobile-connector")<<QByteArray()<<QStringLiteral("Implementation: Rust connector (/usr/bin/zerus-mobile-connector).");
        QTest::newRow("migrated")<<QStringLiteral("/usr/bin/zerus-mobile-connector")
            <<QByteArray("/example/zerus-mobile-connector.service.d/50-zerus-killmode.conf /example/zerus-mobile-connector.service.d/60-zerus-rust-connector.conf")
            <<QStringLiteral("switched by zerus-setup --mobile-connector migrate");
        QTest::newRow("foreign")<<QStringLiteral("/usr/bin/sleep")<<QByteArray()<<QStringLiteral("not a Zerus connector program");
    }
    void implementationFollowsTheRunningProgram(){
#ifdef Q_OS_LINUX
        QFETCH(QString,program);QFETCH(QByteArray,dropIns);QFETCH(QString,expected);
        QTemporaryDir dir;QVERIFY(dir.isValid());const auto config=connectorConfig(dir);QVERIFY(!config.isEmpty());
        if(program==QLatin1String("venv")){
            QVERIFY(QDir().mkpath(temporaryRoot(dir)+"/venv/bin"));
            QFile cfg(temporaryRoot(dir)+"/venv/pyvenv.cfg");QVERIFY(cfg.open(QIODevice::WriteOnly));cfg.write("home = /usr/bin\n");cfg.close();
            program=temporaryRoot(dir)+"/venv/bin/zerus-mobile-connector";
        }
        const QByteArray exec="ExecStart={ path="+program.toUtf8()+" ; argv[]="+program.toUtf8()+" --config "+config.toUtf8()+" ; ignore_errors=no ; start_time=[n/a] }\n";
        QVERIFY(fakeTool(dir,"systemctl",Probe,"LoadState=loaded\nActiveState=inactive\nSubState=dead\nKillMode=process\nUnitFileState=disabled\nDropInPaths="+dropIns+"\n"+exec));
        const auto previous=qgetenv("PATH");qputenv("PATH",temporaryRoot(dir).toUtf8()+":"+previous);
        const auto restore=qScopeGuard([previous]{qputenv("PATH",previous);});
        RelaySettings::Panel panel(config);
        auto *status=panel.findChild<QLabel*>("connectorServiceStatus");auto *label=panel.findChild<QLabel*>("connectorImplementation");QVERIFY(status&&label);
        QTRY_VERIFY(!status->text().contains(QStringLiteral("checking")));
        QVERIFY(status->text().startsWith(QStringLiteral("Connector service: stopped")));
        QVERIFY(status->text().contains(QStringLiteral("not enabled")));
        QVERIFY(!label->isHidden());QVERIFY2(label->text().contains(expected),qPrintable(label->text()));
        if(dropIns.isEmpty())QVERIFY(!label->text().contains(QStringLiteral("switched")));
#endif
    }
    void macLaunchAgentProbe_data(){
        QTest::addColumn<QByteArray>("output");QTest::addColumn<int>("code");QTest::addColumn<QString>("expected");QTest::addColumn<bool>("restart");
        QTest::newRow("running")<<QByteArray("gui/501/com.hgdev.zerus-mobile-connector = {\n\tstate = running\n\tprogram = /Applications/Zerus.app/Contents/MacOS/zerus-mobile-connector\n}\n")<<0
            <<QStringLiteral("Connector service: running")<<true;
        QTest::newRow("loaded")<<QByteArray("gui/501/com.hgdev.zerus-mobile-connector = {\n\tstate = not running\n}\n")<<0<<QStringLiteral("loaded, not running")<<true;
        QTest::newRow("absent")<<QByteArray()<<113<<QStringLiteral("not installed")<<false;
    }
    void macLaunchAgentProbe(){
#ifdef Q_OS_MACOS
        QFETCH(QByteArray,output);QFETCH(int,code);QFETCH(QString,expected);QFETCH(bool,restart);
        QTemporaryDir dir;QVERIFY(dir.isValid());const auto config=connectorConfig(dir);QVERIFY(!config.isEmpty());
        QVERIFY(fakeTool(dir,"launchctl","print gui/"+QByteArray::number(getuid())+"/com.hgdev.zerus-mobile-connector",output,code));
        const auto previous=qgetenv("PATH");qputenv("PATH",temporaryRoot(dir).toUtf8()+":"+previous);
        const auto restore=qScopeGuard([previous]{qputenv("PATH",previous);});
        RelaySettings::Panel panel(config);
        auto *status=panel.findChild<QLabel*>("connectorServiceStatus");auto *row=panel.findChild<QWidget*>("connectorRestartCommand");QVERIFY(status&&row);
        QTRY_VERIFY(!status->text().contains(QStringLiteral("checking")));
        QVERIFY2(status->text().contains(expected),qPrintable(status->text()));
        QCOMPARE(!row->isHidden(),restart);
        QVERIFY(row->findChild<QLabel*>()->text().startsWith(QStringLiteral("launchctl kickstart -k gui/")));
        if(code==0&&output.contains("program = "))
            QVERIFY(panel.findChild<QLabel*>("connectorImplementation")->text().contains(QStringLiteral("Rust connector")));
#endif
    }
    void atomicPrivateSavePreservesUnrelatedConfiguration(){
        QTemporaryDir dir;QVERIFY(dir.isValid());const auto path=temporaryRoot(dir)+"/connector.json";
        auto old=original(temporaryRoot(dir)+"/existing-state");QVERIFY(write(path,old.object));
        RelaySettings::ConfigStore store(path);RelaySettings::Snapshot loaded,saved;QString error;
        QVERIFY2(store.load(&loaded,&error),qPrintable(error));
        auto p=RelaySettings::prepare(loaded,{"https://relay.example.test",{},temporaryRoot(dir)+"/existing-state"},&error);QVERIFY(p);
        QVERIFY2(store.save(loaded,*p,&saved,&error),qPrintable(error));
        QCOMPARE(saved.object,loaded.object);
        QCOMPARE(QFileInfo(path).permissions()&(QFile::ReadGroup|QFile::WriteGroup|QFile::ReadOther|QFile::WriteOther),QFile::Permissions{});
    }
    void freshSetupAllocatesPrivateStateWithoutCopyingReceipts(){
        QTemporaryDir dir;QVERIFY(dir.isValid());RelaySettings::ConfigStore store(temporaryRoot(dir)+"/config/connector.json");
        RelaySettings::Snapshot loaded,saved;QString error;QVERIFY(store.load(&loaded,&error));QVERIFY(!loaded.exists);
        auto p=RelaySettings::prepare(loaded,{RelaySettings::canonicalRelay(),"synthetic-new-node-token",temporaryRoot(dir)+"/new-state"},&error);QVERIFY(p);
        QVERIFY2(store.save(loaded,*p,&saved,&error),qPrintable(error));
        QVERIFY(saved.exists);QVERIFY(QFileInfo(temporaryRoot(dir)+"/new-state").isDir());
        QVERIFY(QDir(temporaryRoot(dir)+"/new-state").entryList(QDir::NoDotAndDotDot|QDir::AllEntries).isEmpty());
        QCOMPARE(QFileInfo(temporaryRoot(dir)+"/new-state").permissions()&(QFile::ReadGroup|QFile::WriteGroup|QFile::ExeGroup|QFile::ReadOther|QFile::WriteOther|QFile::ExeOther),QFile::Permissions{});
    }
    void ownedReadableStateParentAllowsPrivateNewChild(){
        QTemporaryDir dir;QVERIFY(dir.isValid());const auto parent=temporaryRoot(dir)+"/shared-parent";QVERIFY(QDir().mkdir(parent));
        QVERIFY(QFile::setPermissions(parent,QFile::ReadOwner|QFile::WriteOwner|QFile::ExeOwner|QFile::ReadGroup|QFile::ExeGroup|QFile::ReadOther|QFile::ExeOther));
        RelaySettings::ConfigStore store(temporaryRoot(dir)+"/connector.json");RelaySettings::Snapshot old,saved;QString error;
        QVERIFY(store.load(&old,&error));
        auto p=RelaySettings::prepare(old,{RelaySettings::canonicalRelay(),"synthetic-new-node-token",parent+"/fresh"},&error);QVERIFY(p);
        QVERIFY2(store.save(old,*p,&saved,&error),qPrintable(error));
        QCOMPARE(QFileInfo(parent).permissions()&(QFile::ReadOther|QFile::ExeOther),QFile::Permissions(QFile::ReadOther|QFile::ExeOther));
        QCOMPARE(QFileInfo(parent+"/fresh").permissions()&(QFile::ReadGroup|QFile::ExeGroup|QFile::ReadOther|QFile::ExeOther),QFile::Permissions{});
    }
    void externalEditAndAtomicReplacementRefuseOverwrite(){
        QTemporaryDir dir;QVERIFY(dir.isValid());const auto path=temporaryRoot(dir)+"/connector.json";
        auto old=original(temporaryRoot(dir)+"/state");QVERIFY(write(path,old.object));
        RelaySettings::ConfigStore store(path);RelaySettings::Snapshot loaded,saved;QString error;QVERIFY(store.load(&loaded,&error));
        auto p=RelaySettings::prepare(loaded,{"https://relay.example.test",{},temporaryRoot(dir)+"/state"},&error);QVERIFY(p);
        auto edited=old.object;edited.insert("poll_interval",11);QVERIFY(write(path,edited));
        QVERIFY(!store.save(loaded,*p,&saved,&error));QVERIFY(store.load(&saved,&error));QCOMPARE(saved.object,edited);
        QVERIFY(store.load(&loaded,&error));QVERIFY(write(temporaryRoot(dir)+"/replacement",loaded.object));
        QVERIFY(QFile::remove(path));QVERIFY(QFile::rename(temporaryRoot(dir)+"/replacement",path));
        QVERIFY(!store.save(loaded,*p,&saved,&error));
    }
    void symlinkAndPublicFilesAreRefused(){
        QTemporaryDir dir;QVERIFY(dir.isValid());const auto real=temporaryRoot(dir)+"/real.json";
        QVERIFY(write(real,original(temporaryRoot(dir)+"/state").object));QString error;RelaySettings::Snapshot loaded;
        QVERIFY2(RelaySettings::ConfigStore(real).load(&loaded,&error),qPrintable(error));
        QVERIFY(QFile::link(real,temporaryRoot(dir)+"/link.json"));QVERIFY(!RelaySettings::ConfigStore(temporaryRoot(dir)+"/link.json").load(&loaded,&error));
        QVERIFY(QFile::setPermissions(real,QFile::ReadOwner|QFile::WriteOwner|QFile::ReadOther));
        QVERIFY(!RelaySettings::ConfigStore(real).load(&loaded,&error));
        QVERIFY(QFile::setPermissions(real,QFile::ReadOwner|QFile::WriteOwner));
        QVERIFY2(RelaySettings::ConfigStore(real).load(&loaded,&error),qPrintable(error));
        QVERIFY(QFile::link(temporaryRoot(dir),temporaryRoot(dir)+"/parent-link"));
        QVERIFY(!RelaySettings::ConfigStore(temporaryRoot(dir)+"/parent-link/real.json").load(&loaded,&error));
    }
#endif
};
QTEST_MAIN(TestRelaySettings)
#include "test_relaysettings.moc"
