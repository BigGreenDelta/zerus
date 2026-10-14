#include <QtTest>
#include <memory>
#include <QComboBox>
#include <QFile>
#include <QLabel>
#include <QLineEdit>
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
#include "LaunchWarnings.h"
#include "LocalSessionHost.h"

class TestNewSessionDialog : public QObject {
    Q_OBJECT
    QTemporaryDir m_settings,m_fixture;
    QString program() const {return FakeCli::program(m_fixture.filePath("hgs"));}
    void flag(const QString &name,bool enabled=true) {
        QFile f(m_fixture.filePath(name));if(enabled){QVERIFY(f.open(QIODevice::WriteOnly));}else f.remove();
    }
    // The local computer reports Linux: without a platform it would follow the
    // GUI's build platform, and a Windows build hides some agents there.
    static FleetState fleet() {
        FleetState fleet;BoxState local;local.host="pc";local.ok=true;local.metrics=QJsonObject{{"platform","linux"}};
        fleet.setLocal(local,QDateTime::currentMSecsSinceEpoch());BoxState mac;mac.host="mac";mac.ok=true;
        fleet.setPeer(mac,QDateTime::currentMSecsSinceEpoch());return fleet;
    }
    // This computer runs Windows, and its session host runs in that Windows session
    // (spec D5), or none runs when session is negative.
    static FleetState windowsFleet(qint64 session) {
        auto state=fleet();auto local=state.local();local.metrics=QJsonObject{{"platform","windows"}};
        local.localHost=session<0?QJsonObject{{"state","stopped"},{"pid",QJsonValue::Null},{"windows_session",QJsonValue::Null}}
            :QJsonObject{{"state","running"},{"pid",4242},{"protocol",1},{"conpty","bundled"},{"integrity","medium"},{"windows_session",session}};
        state.setLocal(local,QDateTime::currentMSecsSinceEpoch());return state;
    }
    // Each quota probe the fixture answered, as "host id arguments".
    QStringList probes() const {
        QFile f(m_fixture.filePath("quota-probes"));if(!f.open(QIODevice::ReadOnly))return {};
        return QString::fromUtf8(f.readAll()).split('\n',Qt::SkipEmptyParts);
    }
    static QListWidget *choices(NewSessionDialog &dialog) {return dialog.findChild<QListWidget *>("launchAccount");}
    static int row(NewSessionDialog &dialog,const QString &id) {
        const auto *list=choices(dialog);for(int i=0;i<list->count();++i)if(list->item(i)->data(Qt::UserRole).toString()==id)return i;return -1;
    }
    static QString tip(NewSessionDialog &dialog,const QString &id) {const int at=row(dialog,id);return at<0?QString():choices(dialog)->item(at)->toolTip();}
private slots:
    void initTestCase() {
        QVERIFY(m_settings.isValid());QCoreApplication::setOrganizationName("hgs-tests");QCoreApplication::setApplicationName("new-session-dialog");
        QSettings::setDefaultFormat(QSettings::IniFormat);QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,m_settings.path());
    }
    void init() {
        QSettings().clear();
        for(const auto &name:{"fail","removed","slow-mac","released","missing","ended","quota-probes","mac-started","duplicates","other-scope","machine-accounts","slow-catalog-mac","catalog-started","empty-catalog","opencode","uncached","hold-probes"})flag(name,false);
        QVERIFY(FakeCli::install(m_fixture.filePath("hgs"),R"PY(#!/usr/bin/env python3
import json,pathlib,sys,time
root=pathlib.Path(__file__).parent
a=sys.argv[1:];host=''
if a and a[0].startswith('@'):host=a.pop(0)[1:]
# The catalog's cached status and a probe report the same account state.
def status(i,provider):
 window={'window_minutes':300,'used_percent':91 if host=='mac' else 17,'resets_at':int(time.time())+3600}
 weekly={'window_minutes':10080,'used_percent':78,'resets_at':int(time.time())+86400}
 if (root/'missing').exists():window.pop('used_percent');weekly.pop('used_percent')
 if (root/'ended').exists():window['resets_at']=weekly['resets_at']=int(time.time())-60
 result={'status':'signed_out' if provider=='dsh' else 'ok','identity':{'email':'shared@example.invalid'},
         'windows':[] if provider=='dsh' else [window,weekly],'checked_at':time.time()}
 if provider=='codex':result['identity']['account_id']='scope-'+i
 if provider=='opencode':result.update(status='configured',identity={},windows=[])
 if (root/'other-scope').exists() and i=='claude-copy':result['identity']['organization']='Other team'
 if (root/'machine-accounts').exists():result['identity']['email']=('mac' if host=='mac' else 'pc')+'@example.invalid'
 return result
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
 if (root/'opencode').exists():
  profiles.append({'id':'native-opencode','provider':'opencode','label':'Native configuration','home':'/profiles/native-opencode','installed':True,'is_default':True})
 if (root/'empty-catalog').exists():profiles=[]
 for profile in profiles:
  if not (root/'uncached').exists():profile['account_status']=status(profile['id'],profile['provider'])
 print(json.dumps({'profiles':profiles,'removed_profiles':[{'id':'native-kimi','provider':'kimi'}]}))
elif a[:2]==['account','inspect']:
 i=a[2];p='claude' if i.startswith(('claude','pc-','mac-')) else 'dsh' if i=='native-dsh' else 'opencode' if i=='native-opencode' else 'codex'
 with open(root/'quota-probes','a') as probes:probes.write(host+' '+i+' '+' '.join(a[3:])+'\n')
 if (root/'hold-probes').exists():
  for _ in range(500):
   if (root/'released').exists():break
   time.sleep(.02)
 if host=='mac' and i=='work' and (root/'slow-mac').exists():
  (root/'mac-started').touch()
  for _ in range(500):
   if (root/'released').exists():break
   time.sleep(.02)
 if (root/'fail').exists():sys.exit(1)
 print(json.dumps({'id':i,'provider':p,'home':'/profiles/'+i,**status(i,p)}))
elif a and a[0]=='dirs':print(json.dumps({'path':a[-1],'directories':[]}))
elif a and a[0]=='worktrees':print(json.dumps({'state':'not_repo','worktrees':[]}))
)PY"));
    }
    void cleanup() {LocalSessionHost::resetOwnWindowsSessionForTests();}
    // macOS gives a new dialog's first focus to this list: its combo boxes and
    // buttons take no Tab focus. Focus alone must not turn the loading list's only
    // row, Run shell, into a choice that replaces the remembered agent.
    void focusWhileCatalogLoadsKeepsRememberedAgent() {
        flag("slow-catalog-mac");QSettings().setValue("workspace/launchAgent","claude");
        NewSessionDialog dialog(program(),fleet(),"mac");dialog.show();QVERIFY(QTest::qWaitForWindowActive(&dialog));
        QTRY_VERIFY(QFile::exists(m_fixture.filePath("catalog-started")));QCOMPARE(choices(dialog)->count(),1);
        choices(dialog)->setFocus(Qt::ActiveWindowFocusReason);QTRY_VERIFY(choices(dialog)->hasFocus());
        QVERIFY(!choices(dialog)->currentItem());
        flag("released");QTRY_VERIFY(row(dialog,"claude")>=0);
        QCOMPARE(choices(dialog)->currentItem()->data(Qt::UserRole).toString(),QString("claude"));
    }
    // A remembered agent this computer lacks falls back to the first agent, not the shell.
    void unavailableRememberedAgentSelectsFirstAgent() {
        flag("machine-accounts");QSettings().setValue("workspace/launchAgent","kimi");
        NewSessionDialog dialog(program(),fleet());dialog.show();
        QTRY_VERIFY(row(dialog,"pc-claude")>=0);QCOMPARE(choices(dialog)->currentRow(),0);
        QCOMPARE(choices(dialog)->currentItem()->data(Qt::UserRole+1).toString(),QString("claude"));
    }
    // Opening asks for current limits of the chosen computer's accounts, as often
    // as the shared store allows. Replies update rows in place: the list keeps its
    // rows, choice and scroll position, even when they identify two rows as one account.
    void openingRefreshesLimitsInPlace() {
        flag("uncached");flag("duplicates");flag("hold-probes");
        AccountUsageStore store(program());store.setFleet(fleet());
        auto dialog=std::make_unique<NewSessionDialog>(program(),fleet(),QString(),"claude",QString(),nullptr,&store);dialog->show();
        auto *list=choices(*dialog);QTRY_VERIFY(row(*dialog,"claude-copy")>=0);QTRY_COMPARE(list->count(),6);
        QTRY_COMPARE(probes().size(),2);QCOMPARE(tip(*dialog,"work"),QString("Work\nlimits n/a"));
        list->setCurrentRow(row(*dialog,"claude"));
        list->verticalScrollBar()->setValue(list->verticalScrollBar()->maximum());const auto scroll=list->verticalScrollBar()->value();
        QList<QPersistentModelIndex> rows;for(int i=0;i<list->count();++i)rows.append(list->model()->index(i,0));
        QSignalSpy inserted(list->model(),&QAbstractItemModel::rowsInserted),removed(list->model(),&QAbstractItemModel::rowsRemoved);
        QSignalSpy reset(list->model(),&QAbstractItemModel::modelReset),layout(list->model(),&QAbstractItemModel::layoutChanged);
        QSignalSpy chosen(list,&QListWidget::currentItemChanged);flag("released");
        for(const auto *id:{"work","claude","claude-copy"})QTRY_VERIFY2(tip(*dialog,id).contains("17% used"),id);
        QVERIFY(tip(*dialog,"work").startsWith("shared@example.invalid\n"));
        QCOMPARE(inserted.size(),0);QCOMPARE(removed.size(),0);QCOMPARE(reset.size(),0);QCOMPARE(layout.size(),0);QCOMPARE(chosen.size(),0);
        for(int i=0;i<rows.size();++i)QCOMPARE(rows[i].row(),i);
        QCOMPARE(list->count(),6);QCOMPARE(list->currentItem()->data(Qt::UserRole).toString(),QString("claude"));
        QCOMPARE(list->verticalScrollBar()->value(),scroll);
        auto expected=QStringList{" work --refresh"," other --refresh"," claude --refresh"," claude-copy --refresh"," native-dsh --refresh"};
        QTRY_COMPARE(probes().size(),expected.size());auto sorted=probes();sorted.sort();expected.sort();QCOMPARE(sorted,expected);
        // Choosing rows and the minute timer repaint cached limits without probing.
        list->setCurrentRow(row(*dialog,"work"));
        for(auto *timer:dialog->findChildren<QTimer *>())if(timer->interval()==60000)QMetaObject::invokeMethod(timer,"timeout");
        QTest::qWait(200);QCOMPARE(probes().size(),expected.size());
        // The next dialog groups the identified account and reuses the fresh limits.
        dialog.reset();dialog=std::make_unique<NewSessionDialog>(program(),fleet(),QString(),"claude",QString(),nullptr,&store);dialog->show();
        list=choices(*dialog);QTRY_COMPARE(list->count(),5);QTRY_VERIFY(tip(*dialog,"work").contains("17% used"));
        QTest::qWait(200);QCOMPARE(probes().size(),expected.size());
        // Refresh asks again at once.
        dialog->findChild<QPushButton *>("refreshLaunchAccounts")->click();QTRY_COMPARE(probes().size(),2*expected.size());
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
        // Opening asks for each installed account's limits, at most two at a time.
        QTRY_COMPARE(probes().size(),4);
        dialog.setFleet(fleet());QCOMPARE(list->currentIndex(),QModelIndex(index));
        const auto scroll=list->verticalScrollBar()->value();flag("fail");dialog.findChild<QPushButton *>("refreshLaunchAccounts")->click();
        QTRY_VERIFY(dialog.findChild<QLabel *>("launchAccountStatus")->text().contains("Could not load accounts"));
        QVERIFY(!list->currentItem()->toolTip().contains("failed"));
        QVERIFY(list->currentItem()->toolTip().contains("17% used"));QCOMPARE(list->currentIndex(),QModelIndex(index));
        QCOMPARE(list->verticalScrollBar()->value(),scroll);QCOMPARE(list->count(),5);
        // A failed catalog refresh and the minute timer ask for no limits.
        for(auto *timer:dialog.findChildren<QTimer *>())if(timer->interval()==60000)QMetaObject::invokeMethod(timer,"timeout");
        QTest::qWait(100);QCOMPARE(probes().size(),4);
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
    }
    void localWindowsHidesDeepSeekAndOpenCode() {
        // Spec D16: the local Windows computer starts neither DeepSeek nor OpenCode,
        // even with their accounts in its catalog, and its shell is PowerShell. A
        // Linux peer in the same dialog keeps both.
        flag("opencode");auto state=fleet();auto local=state.local();local.metrics=QJsonObject{{"platform","windows"}};
        state.setLocal(local,QDateTime::currentMSecsSinceEpoch());
        NewSessionDialog dialog(program(),state,{},"dsh");dialog.show();auto *list=choices(dialog);
        QTRY_VERIFY(row(dialog,"work")>=0);QTRY_COMPARE(list->count(),4);
        QCOMPARE(row(dialog,"native-dsh"),-1);QCOMPARE(row(dialog,"native-opencode"),-1);
        for(int i=0;i<list->count();++i)for(const auto *hidden:{"dsh","opencode"})QVERIFY(list->item(i)->data(Qt::UserRole+1).toString()!=hidden);
        QVERIFY(list->currentItem());QCOMPARE(list->item(list->count()-1)->text(),QString("Run PowerShell"));
        QCOMPARE(list->item(list->count()-1)->data(Qt::UserRole+1).toString(),QString("sh"));
        auto *computer=dialog.findChild<QComboBox *>("launchComputer");computer->setCurrentIndex(computer->findData(QString("mac")));
        QTRY_VERIFY(row(dialog,"native-dsh")>=0);QVERIFY(row(dialog,"native-opencode")>=0);QCOMPARE(list->count(),6);
        QCOMPARE(list->item(list->count()-1)->text(),QString("Run shell"));
        computer->setCurrentIndex(computer->findData(QString()));
        QTRY_VERIFY(row(dialog,"work")>=0);QTRY_COMPARE(list->count(),4);
        QCOMPARE(row(dialog,"native-dsh"),-1);QCOMPARE(row(dialog,"native-opencode"),-1);
        QCOMPARE(list->item(list->count()-1)->text(),QString("Run PowerShell"));
        // A Linux computer keeps both and its shell.
        NewSessionDialog posix(program(),fleet(),{},"dsh");posix.show();
        QTRY_VERIFY(row(posix,"native-dsh")>=0);QTRY_VERIFY(row(posix,"native-opencode")>=0);QCOMPARE(choices(posix)->count(),6);
        QCOMPARE(choices(posix)->item(5)->text(),QString("Run shell"));
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
    }
    void unknownAndEndedWindowsStayUnavailable() {
        flag("missing");NewSessionDialog dialog(program(),fleet(),{},"codex");dialog.show();
        QTRY_VERIFY(row(dialog,"work")>=0);QTRY_COMPARE(choices(dialog)->count(),5);
        QTRY_COMPARE(choices(dialog)->item(row(dialog,"work"))->toolTip(),QString("shared@example.invalid\nlimits n/a"));
        QVERIFY(!choices(dialog)->item(row(dialog,"work"))->toolTip().contains("0% used"));
        flag("missing",false);flag("ended");dialog.findChild<QPushButton *>("refreshLaunchAccounts")->click();
        QTRY_COMPARE(choices(dialog)->item(row(dialog,"work"))->toolTip(),QString("shared@example.invalid\nlimits n/a"));
    }
    // Spec D5: Start session on this Windows computer asks first when its session
    // host runs in another Windows session (for example one started over SSH).
    void launchIntoAnotherWindowsSessionAsksFirst() {
        LocalSessionHost::setOwnWindowsSessionForTests(1);
        NewSessionDialog dialog(program(),windowsFleet(2),{},"codex");dialog.selectPath("C:/work/example",false);dialog.show();
        QTRY_VERIFY_WITH_TIMEOUT(row(dialog,"work")>=0,15000);choices(dialog)->setCurrentRow(row(dialog,"work"));
        auto *name=dialog.findChild<QLineEdit *>("launchName");name->setText("keep-this-name");
        auto *start=dialog.findChild<QPushButton *>("primary");QTRY_VERIFY(start->isEnabled());
        QSignalSpy launched(&dialog,&NewSessionDialog::launchRequested);LaunchWarnings warnings(dialog);
        // Cancel keeps the form as it was: nothing is checked, created or started.
        start->click();QCOMPARE(warnings.shown,1);
        QVERIFY(warnings.texts[0].contains("another Windows session (2), for example one started over SSH"));
        QVERIFY(warnings.texts[0].contains("cannot show windows on this desktop"));QVERIFY(warnings.texts[0].endsWith("Launch anyway?"));
        QVERIFY(dialog.isVisible());QCOMPARE(launched.size(),0);QCOMPARE(name->text(),QString("keep-this-name"));
        QCOMPARE(start->text(),QString("Start session"));QVERIFY(start->isEnabled());
        QCOMPARE(choices(dialog)->currentItem()->data(Qt::UserRole).toString(),QString("work"));
        QVERIFY(!dialog.findChild<QLabel *>("launchError")->text().contains("Checking"));
        // Launch anyway starts exactly once, after a single question.
        warnings.answer="launchAnyway";start->click();QTRY_COMPARE_WITH_TIMEOUT(launched.size(),1,15000);QCOMPARE(warnings.shown,2);
        QCOMPARE(launched[0][0].toString(),QString());QCOMPARE(launched[0][1].toString(),QString("codex"));
        QCOMPARE(launched[0][2].toString(),QString("C:/work/example"));QCOMPARE(launched[0][3].toString(),QString("keep-this-name"));
        QCOMPARE(launched[0][4].toString(),QString("work"));QTRY_VERIFY(!dialog.isVisible());
        QTest::qWait(200);QCOMPARE(launched.size(),1);QCOMPARE(warnings.shown,2);
    }
    // No question for the same Windows session, without a running host (the launch
    // starts one in this session), for a remote machine or for a Linux computer: a
    // Linux or macOS GUI launches exactly as before.
    void launchWithoutAnotherWindowsSessionDoesNotAsk_data() {
        QTest::addColumn<QString>("platform");QTest::addColumn<qint64>("hostSession");QTest::addColumn<QString>("machine");
        QTest::addColumn<QString>("folder");
        QTest::newRow("same session")<<"windows"<<qint64(1)<<QString()<<"C:/work/example";
        QTest::newRow("no host runs")<<"windows"<<qint64(-1)<<QString()<<"C:/work/example";
        QTest::newRow("remote machine")<<"windows"<<qint64(2)<<"mac"<<"/workspace/example";
        QTest::newRow("linux computer")<<"linux"<<qint64(2)<<QString()<<"/workspace/example";
    }
    void launchWithoutAnotherWindowsSessionDoesNotAsk() {
        QFETCH(QString,platform);QFETCH(qint64,hostSession);QFETCH(QString,machine);QFETCH(QString,folder);
        LocalSessionHost::setOwnWindowsSessionForTests(1);
        auto state=windowsFleet(hostSession);auto local=state.local();local.metrics["platform"]=platform;
        state.setLocal(local,QDateTime::currentMSecsSinceEpoch());
        NewSessionDialog dialog(program(),state,machine,"codex");dialog.selectPath(folder,false);dialog.show();
        QTRY_VERIFY_WITH_TIMEOUT(row(dialog,"work")>=0,15000);choices(dialog)->setCurrentRow(row(dialog,"work"));
        auto *start=dialog.findChild<QPushButton *>("primary");QTRY_VERIFY(start->isEnabled());
        QSignalSpy launched(&dialog,&NewSessionDialog::launchRequested);LaunchWarnings warnings(dialog);
        start->click();QTRY_COMPARE_WITH_TIMEOUT(launched.size(),1,15000);QCOMPARE(warnings.shown,0);
        QCOMPARE(launched[0][0].toString(),machine);QCOMPARE(launched[0][2].toString(),folder);
    }
};
QTEST_MAIN(TestNewSessionDialog)
#include "test_newsessiondialog.moc"
