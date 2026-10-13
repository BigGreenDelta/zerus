#include <QtTest>
#include <QComboBox>
#include <QFile>
#include <QLabel>
#include <QListWidget>
#include <QPersistentModelIndex>
#include <QPushButton>
#include <QScrollBar>
#include "NewSessionDialog.h"
#include "AccountUsageStore.h"
#include "AccountUsage.h"
#include "SessionsWindow.h"
#include "FakeCli.h"
#include "IdentityBadge.h"
#include "AccountCatalog.h"
#include "AccountsPage.h"

class TestNewSessionDialog : public QObject {
    Q_OBJECT
    QTemporaryDir m_settings,m_fixture;
    QString program() const {return FakeCli::program(m_fixture.filePath("hgs"));}
    void flag(const QString &name,bool enabled=true) {
        QFile f(m_fixture.filePath(name));if(enabled){QVERIFY(f.open(QIODevice::WriteOnly));}else f.remove();
    }
    static FleetState fleet() {
        FleetState fleet;BoxState local;local.host="pc";local.ok=true;
        fleet.setLocal(local,QDateTime::currentMSecsSinceEpoch());BoxState mac;mac.host="mac";mac.ok=true;
        fleet.setPeer(mac,QDateTime::currentMSecsSinceEpoch());return fleet;
    }
    static QListWidget *choices(NewSessionDialog &dialog) {return dialog.findChild<QListWidget *>("launchAccount");}
    static int row(NewSessionDialog &dialog,const QString &id) {
        const auto *list=choices(dialog);for(int i=0;i<list->count();++i)if(list->item(i)->data(Qt::UserRole).toString()==id)return i;return -1;
    }
private slots:
    void initTestCase() {
        QVERIFY(m_settings.isValid());QCoreApplication::setOrganizationName("hgs-tests");QCoreApplication::setApplicationName("new-session-dialog");
        QSettings::setDefaultFormat(QSettings::IniFormat);QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,m_settings.path());
    }
    void init() {
        QSettings().clear();
        for(const auto &name:{"fail","removed","slow-mac","released","missing","ended","quota-probes","mac-started","duplicates","other-scope","machine-accounts","slow-catalog-mac","catalog-started","empty-catalog"})flag(name,false);
        QVERIFY(FakeCli::install(m_fixture.filePath("hgs"),R"PY(#!/usr/bin/env python3
import json,pathlib,sys,time
root=pathlib.Path(__file__).parent
a=sys.argv[1:];host=''
if a and a[0].startswith('@'):host=a.pop(0)[1:]
if a[:2]==['account','ls']:
 if (root/'fail').exists():sys.exit(1)
 if host=='mac' and (root/'slow-catalog-mac').exists():
  (root/'catalog-started').touch()
  for _ in range(500):
   if (root/'released').exists():break
   time.sleep(.02)
 profiles=[{'id':i,'provider':p,'label':label,'home':'/profiles/'+i,'installed':True,'is_default':i=='other'}
           for i,p,label in [('work','codex','Work'),('other','codex','Work'),('claude','claude','Review'),('native-dsh','dsh','DeepSeek')]
           if not ((root/'removed').exists() and i=='work')]
 if (root/'duplicates').exists():
  profiles.append({'id':'claude-copy','provider':'claude','label':'Review','home':'/profiles/claude-copy','installed':True,'is_default':True})
 if (root/'machine-accounts').exists():
  profiles=[{'id':i,'provider':'claude','label':'Review','home':'/profiles/'+i,'installed':True,'is_default':True}
            for i in (['mac-claude'] if host=='mac' else ['pc-claude','pc-copy'])]
 if (root/'empty-catalog').exists():profiles=[]
 for profile in profiles:
  window={'window_minutes':300,'used_percent':91 if host=='mac' else 17,'resets_at':int(time.time())+3600}
  weekly={'window_minutes':10080,'used_percent':78,'resets_at':int(time.time())+86400}
  if (root/'missing').exists():window.pop('used_percent');weekly.pop('used_percent')
  if (root/'ended').exists():window['resets_at']=weekly['resets_at']=int(time.time())-60
  profile['account_status']={'status':'signed_out' if profile['provider']=='dsh' else 'ok','identity':{'email':'shared@example.invalid'},
                             'windows':[] if profile['provider']=='dsh' else [window,weekly]}
  if profile['provider']=='codex':profile['account_status']['identity']['account_id']='scope-'+profile['id']
  if (root/'other-scope').exists() and profile['id']=='claude-copy':profile['account_status']['identity']['organization']='Other team'
  if (root/'machine-accounts').exists():profile['account_status']['identity']['email']=('mac' if host=='mac' else 'pc')+'@example.invalid'
 print(json.dumps({'profiles':profiles,'removed_profiles':[{'id':'native-kimi','provider':'kimi'}]}))
elif a[:2]==['account','inspect']:
 (root/'quota-probes').touch()
 i=a[2];p='claude' if i.startswith(('claude','pc-','mac-')) else 'dsh' if i=='native-dsh' else 'codex'
 if host=='mac' and i=='work' and (root/'slow-mac').exists():
  (root/'mac-started').touch()
  for _ in range(500):
   if (root/'released').exists():break
   time.sleep(.02)
 if (root/'fail').exists():sys.exit(1)
 window={'window_minutes':300,'used_percent':91 if host=='mac' else 17,'resets_at':int(time.time())+3600}
 if (root/'missing').exists():window.pop('used_percent')
 if (root/'ended').exists():window['resets_at']=int(time.time())-60
 print(json.dumps({'id':i,'provider':p,'home':'/profiles/'+i,'status':'signed_out' if p=='dsh' else 'ok',
                   'identity':{'email':'shared@example.invalid',**({'account_id':'scope-'+i} if p=='codex' else {})},'windows':[] if p=='dsh' else [window,{'window_minutes':10080,'used_percent':78,'resets_at':int(time.time())+86400}]}))
elif a and a[0]=='dirs':print(json.dumps({'path':a[-1],'directories':[]}))
elif a and a[0]=='worktrees':print(json.dumps({'state':'not_repo','worktrees':[]}))
)PY"));
    }
    void pairsAndLimitsLaunchExactProfile_data() {
        QTest::addColumn<QString>("host");QTest::addColumn<bool>("dark");
        QTest::newRow("pc-light")<<QString()<<false;QTest::newRow("pc-dark")<<QString()<<true;
        QTest::newRow("mac-light")<<QString("mac")<<false;QTest::newRow("mac-dark")<<QString("mac")<<true;
    }
    void pairsAndLimitsLaunchExactProfile() {
        QFETCH(QString,host);QFETCH(bool,dark);
        QSettings().setValue("workspace/theme",dark?"dark":"light");
        SessionsWindow parent(program());
        NewSessionDialog dialog(program(),fleet(),host,"codex",{},&parent);
        dialog.selectPath("/workspace/example",false);dialog.show();
        auto *list=choices(dialog);QTRY_VERIFY(row(dialog,"work")>=0);QTRY_COMPARE(list->count(),5);QVERIFY(!dialog.findChild<QComboBox *>("launchAgent"));
        QTRY_VERIFY(list->item(row(dialog,"work"))->toolTip().contains(host.isEmpty()?"17% used":"91% used"));
        const auto tooltip=list->item(row(dialog,"work"))->toolTip();
        QVERIFY(tooltip.contains("5h"));QVERIFY(tooltip.contains("7d"));QVERIFY(!tooltip.contains("Reset in"));
        QCOMPARE(list->item(row(dialog,"work"))->text(),QString("shared@example.invalid"));
        QCOMPARE(list->item(row(dialog,"other"))->text(),QString("shared@example.invalid"));
        const auto logo=list->item(row(dialog,"claude"))->icon().pixmap(40,40).toImage();
        const auto expected=IdentityBadges::providerColor("claude",dark);bool colored=false;
        for(int y=0;y<logo.height();++y)for(int x=0;x<logo.width();++x){const auto pixel=logo.pixelColor(x,y);if(pixel.alpha()>240&&qAbs(pixel.red()-expected.red())<3&&qAbs(pixel.green()-expected.green())<3&&qAbs(pixel.blue()-expected.blue())<3)colored=true;}
        QVERIFY(colored);
        QVERIFY(!tooltip.contains("default"));QCOMPARE(tooltip.count('\n'),1);
        QCOMPARE(list->currentItem()->data(Qt::UserRole).toString(),QString("other"));
        QTRY_COMPARE(list->item(row(dialog,"native-dsh"))->toolTip(),QString("shared@example.invalid\nlimits n/a"));
        QCOMPARE(list->item(list->count()-1)->text(),QString("Run shell"));
        list->setCurrentRow(row(dialog,"work"));
        const auto preview=qEnvironmentVariable("ZERUS_LAUNCH_PREVIEW");
        if(!preview.isEmpty()){QDir().mkpath(preview);QVERIFY(dialog.grab().save(preview+"/launch-"+(host.isEmpty()?"pc":"mac")+(dark?"-dark":"-light")+".png"));}
        auto *start=dialog.findChild<QPushButton *>("primary");QSignalSpy launched(&dialog,&NewSessionDialog::launchRequested);
        QTRY_VERIFY(start->isEnabled());start->click();QTRY_COMPARE(launched.size(),1);
        QCOMPARE(launched[0][0].toString(),host);QCOMPARE(launched[0][1].toString(),QString("codex"));QCOMPARE(launched[0][4].toString(),QString("work"));
        NewSessionDialog next(program(),fleet(),host);QTRY_VERIFY(row(next,"work")>=0);QTRY_COMPARE(choices(next)->count(),5);
        QCOMPARE(choices(next)->currentItem()->data(Qt::UserRole).toString(),QString("work"));
        QTest::qWait(100);QVERIFY(!QFile::exists(m_fixture.filePath("quota-probes")));
    }
    void rememberedAndExplicitMissingAccounts() {
        QSettings().setValue("workspace/launchAccounts",QByteArray(R"({"":{"codex":"gone"}})"));
        NewSessionDialog dialog(program(),fleet(),{},"codex");dialog.selectPath("/workspace/example",false);dialog.show();
        QTRY_VERIFY(row(dialog,"work")>=0);QTRY_COMPARE(choices(dialog)->count(),5);QCOMPARE(choices(dialog)->currentItem()->data(Qt::UserRole).toString(),QString("other"));
        dialog.selectAccount("gone");QVERIFY(!choices(dialog)->currentItem());QVERIFY(!dialog.findChild<QPushButton *>("primary")->isEnabled());
        QVERIFY(dialog.findChild<QLabel *>("launchError")->text().contains("unavailable"));
        choices(dialog)->setCurrentRow(row(dialog,"work"));flag("removed");dialog.findChild<QPushButton *>("refreshLaunchAccounts")->click();
        QTRY_COMPARE(row(dialog,"work"),-1);QVERIFY(!choices(dialog)->currentItem());
        choices(dialog)->setCurrentRow(choices(dialog)->count()-1);
        QSignalSpy launched(&dialog,&NewSessionDialog::launchRequested);auto *start=dialog.findChild<QPushButton *>("primary");
        QTRY_VERIFY(start->isEnabled());start->click();QTRY_COMPARE(launched.size(),1);
        QCOMPARE(launched[0][1].toString(),QString("sh"));QVERIFY(launched[0][4].toString().isEmpty());
        NewSessionDialog next(program(),fleet());QTRY_COMPARE(choices(next)->count(),4);
        QCOMPARE(choices(next)->currentItem()->data(Qt::UserRole+1).toString(),QString("sh"));
    }
    void refreshKeepsSelectionAndLastReportedLimits() {
        NewSessionDialog dialog(program(),fleet(),{},"codex");dialog.show();auto *list=choices(dialog);
        QTRY_VERIFY(row(dialog,"work")>=0);QTRY_COMPARE(list->count(),5);QTRY_VERIFY(list->item(row(dialog,"work"))->toolTip().contains("17% used"));
        list->setCurrentRow(row(dialog,"work"));QPersistentModelIndex index=list->currentIndex();
        dialog.setFleet(fleet());QCOMPARE(list->currentIndex(),QModelIndex(index));
        const auto scroll=list->verticalScrollBar()->value();flag("fail");dialog.findChild<QPushButton *>("refreshLaunchAccounts")->click();
        QTRY_VERIFY(dialog.findChild<QLabel *>("launchAccountStatus")->text().contains("Could not load accounts"));
        QVERIFY(!list->currentItem()->toolTip().contains("failed"));
        QVERIFY(list->currentItem()->toolTip().contains("17% used"));QCOMPARE(list->currentIndex(),QModelIndex(index));
        QCOMPARE(list->verticalScrollBar()->value(),scroll);QCOMPARE(list->count(),5);
        for(auto *timer:dialog.findChildren<QTimer *>())if(timer->interval()==60000)QMetaObject::invokeMethod(timer,"timeout");
        QTest::qWait(100);QVERIFY(!QFile::exists(m_fixture.filePath("quota-probes")));
    }
    void lateUsageCannotRetargetAnotherComputer() {
        flag("slow-mac");flag("mac-started",false);
        AccountUsageStore store(program());store.setFleet(fleet());
        store.ensure({"mac","work","codex","/profiles/work"});
        NewSessionDialog dialog(program(),fleet(),"mac","codex",{},nullptr,&store);dialog.show();
        QTRY_VERIFY(QFile::exists(m_fixture.filePath("mac-started")));
        auto *computer=dialog.findChild<QComboBox *>("launchComputer");computer->setCurrentIndex(computer->findData(QString()));
        QTRY_VERIFY(row(dialog,"work")>=0);QTRY_COMPARE(choices(dialog)->count(),5);choices(dialog)->setCurrentRow(row(dialog,"work"));flag("released");
        QTRY_VERIFY(choices(dialog)->currentItem()->toolTip().contains("17% used"));
        QVERIFY(!choices(dialog)->currentItem()->toolTip().contains("91% used"));
        QTest::qWait(100);QVERIFY(!choices(dialog)->currentItem()->toolTip().contains("91% used"));
    }
    void accountAliasesMatchAccountsGroupingAndKeepExactBinding() {
        flag("duplicates");NewSessionDialog dialog(program(),fleet(),{},"claude");dialog.selectPath("/workspace/example",false);dialog.show();
        QTRY_COMPARE(choices(dialog)->count(),5);QTRY_VERIFY(row(dialog,"claude-copy")>=0);
        QCOMPARE(row(dialog,"claude"),-1);QCOMPARE(choices(dialog)->currentItem()->data(Qt::UserRole).toString(),QString("claude-copy"));
        // Another form changing remembered defaults cannot retarget this form.
        QSettings().setValue("workspace/launchAccounts",QByteArray(R"({"":{"claude":"claude"}})"));
        for(auto *timer:dialog.findChildren<QTimer *>())if(timer->interval()==60000)QMetaObject::invokeMethod(timer,"timeout");
        QCOMPARE(choices(dialog)->currentItem()->data(Qt::UserRole).toString(),QString("claude-copy"));
        dialog.selectAccount("claude");QVERIFY(row(dialog,"claude")>=0);QCOMPARE(row(dialog,"claude-copy"),-1);
        QCOMPARE(choices(dialog)->currentItem()->data(Qt::UserRole).toString(),QString("claude"));
        flag("other-scope");dialog.findChild<QPushButton *>("refreshLaunchAccounts")->click();
        QTRY_COMPARE(choices(dialog)->count(),6);QVERIFY(row(dialog,"claude-copy")>=0);QVERIFY(row(dialog,"claude")>=0);
        QCOMPARE(choices(dialog)->currentItem()->data(Qt::UserRole).toString(),QString("claude"));
        QSignalSpy launched(&dialog,&NewSessionDialog::launchRequested);auto *start=dialog.findChild<QPushButton *>("primary");
        QTRY_VERIFY(start->isEnabled());start->click();QTRY_COMPARE(launched.size(),1);QCOMPARE(launched[0][4].toString(),QString("claude"));
        QVERIFY(!QFile::exists(m_fixture.filePath("quota-probes")));
    }
    void selectedComputerCatalogRejectsLateOtherComputerRows() {
        flag("machine-accounts");flag("slow-catalog-mac");NewSessionDialog dialog(program(),fleet(),"mac","claude");dialog.show();
        QSignalSpy catalogs(dialog.findChild<HgsClient *>(),&HgsClient::accountsReady);
        QTRY_VERIFY(QFile::exists(m_fixture.filePath("catalog-started")));QCOMPARE(choices(dialog)->count(),1);QVERIFY(!choices(dialog)->currentItem());
        auto *computer=dialog.findChild<QComboBox *>("launchComputer");computer->setCurrentIndex(computer->findData(QString()));
        QTRY_VERIFY(row(dialog,"pc-claude")>=0);QCOMPARE(choices(dialog)->count(),2);QCOMPARE(row(dialog,"mac-claude"),-1);QCOMPARE(row(dialog,"pc-copy"),-1);
        QCOMPARE(choices(dialog)->item(0)->text(),QString("pc@example.invalid"));flag("released");
        QTRY_COMPARE(catalogs.size(),2);QCOMPARE(row(dialog,"mac-claude"),-1);QCOMPARE(choices(dialog)->count(),2);
        computer->setCurrentIndex(computer->findData(QString("mac")));
        QCOMPARE(row(dialog,"pc-claude"),-1);QTRY_VERIFY(row(dialog,"mac-claude")>=0);QCOMPARE(choices(dialog)->count(),2);
        QCOMPARE(choices(dialog)->item(0)->text(),QString("mac@example.invalid"));
        QVERIFY(!QFile::exists(m_fixture.filePath("quota-probes")));
    }
    void chooserAndAccountsPageShowSameAccountsForEachComputer() {
        flag("duplicates");AccountUsageStore store(program());AccountsPage page(program(),nullptr,&store);page.setFleet(fleet());page.show();
        for(const auto &host:{QString(),QString("mac")}){
            page.showMachine(host);auto *accounts=page.findChild<QListWidget *>("accountProfiles");QTRY_COMPARE(accounts->count(),4);
            QTRY_VERIFY(page.findChild<QPushButton *>("refreshAccounts")->isEnabled());
            NewSessionDialog dialog(program(),fleet(),host,"claude",{},nullptr,&store);dialog.show();
            QTRY_COMPARE(choices(dialog)->count(),accounts->count()+1);
            QSet<QString> shown,available;
            for(int i=0;i<accounts->count();++i)available.insert(accounts->item(i)->data(Qt::UserRole).toJsonObject()["account_key"].toString());
            for(int i=0;i<choices(dialog)->count()-1;++i){
                const auto profile=choices(dialog)->item(i)->data(Qt::UserRole+2).toJsonObject();
                QCOMPARE(profile["host"].toString(),host);shown.insert(AccountCatalog::key(profile));
            }
            QCOMPARE(shown,available);
        }
    }
    void emptyComputerCatalogOffersOnlyShell() {
        flag("empty-catalog");NewSessionDialog dialog(program(),fleet());dialog.show();
        QTRY_VERIFY(dialog.findChild<QLabel *>("launchAccountStatus")->text().isEmpty());
        QCOMPARE(choices(dialog)->count(),1);QCOMPARE(choices(dialog)->item(0)->text(),QString("Run shell"));
        QVERIFY(!QFile::exists(m_fixture.filePath("quota-probes")));
    }
    void pendingAccountsAllowShellOnlyWhenChosen() {
        flag("slow-catalog-mac");NewSessionDialog dialog(program(),fleet(),"mac","claude");dialog.selectPath("/workspace/example",false);dialog.show();
        QTRY_VERIFY(QFile::exists(m_fixture.filePath("catalog-started")));QVERIFY(!choices(dialog)->currentItem());
        auto *start=dialog.findChild<QPushButton *>("primary");QVERIFY(!start->isEnabled());
        choices(dialog)->setCurrentRow(0);QTRY_VERIFY(start->isEnabled());
        QSignalSpy launched(&dialog,&NewSessionDialog::launchRequested);start->click();QTRY_COMPARE(launched.size(),1);
        QCOMPARE(launched[0][0].toString(),QString("mac"));QCOMPARE(launched[0][1].toString(),QString("sh"));flag("released");
        QVERIFY(!QFile::exists(m_fixture.filePath("quota-probes")));
    }
    void cachedLimitBarsReuseFooterWidgetAndIndicatorPreference() {
        NewSessionDialog dialog(program(),fleet());dialog.show();QTRY_VERIFY(row(dialog,"work")>=0);
        auto *renderer=dynamic_cast<AccountUsage::Button *>(dialog.findChild<QPushButton *>("launchAccountLimitsRenderer"));
        QVERIFY(renderer);QVERIFY(!renderer->isVisible());QCOMPARE(renderer->focusPolicy(),Qt::NoFocus);
        QSettings().remove("workspace/usageIndicator");const auto rails=dialog.grab().toImage();
        QCOMPARE(renderer->indicator(),AccountUsage::Indicator::Rails);
        QSettings().setValue("workspace/usageIndicator","marker");const auto marker=dialog.grab().toImage();
        QCOMPARE(renderer->indicator(),AccountUsage::Indicator::Marker);QVERIFY(marker!=rails);
        QSettings().setValue("workspace/usageIndicator","marker-countdown");dialog.grab();
        QCOMPARE(renderer->indicator(),AccountUsage::Indicator::MarkerCountdown);
        flag("missing");dialog.findChild<QPushButton *>("refreshLaunchAccounts")->click();
        QTRY_COMPARE(choices(dialog)->item(row(dialog,"work"))->toolTip(),QString("shared@example.invalid\nlimits n/a"));
        QVERIFY(!QFile::exists(m_fixture.filePath("quota-probes")));
    }
    void unknownAndEndedWindowsStayUnavailable() {
        flag("missing");NewSessionDialog dialog(program(),fleet(),{},"codex");dialog.show();
        QTRY_VERIFY(row(dialog,"work")>=0);QTRY_COMPARE(choices(dialog)->count(),5);
        QTRY_COMPARE(choices(dialog)->item(row(dialog,"work"))->toolTip(),QString("shared@example.invalid\nlimits n/a"));
        QVERIFY(!choices(dialog)->item(row(dialog,"work"))->toolTip().contains("0% used"));
        flag("missing",false);flag("ended");dialog.findChild<QPushButton *>("refreshLaunchAccounts")->click();
        QTRY_COMPARE(choices(dialog)->item(row(dialog,"work"))->toolTip(),QString("shared@example.invalid\nlimits n/a"));
        QTest::qWait(100);QVERIFY(!QFile::exists(m_fixture.filePath("quota-probes")));
    }
};
QTEST_MAIN(TestNewSessionDialog)
#include "test_newsessiondialog.moc"
