#include <QtTest>
#include <QScrollArea>
#include <QScrollBar>
#include <QComboBox>
#include <QFile>
#include <QDir>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTabWidget>
#include <QDialog>
#include <QDialogButtonBox>
#include <QTimer>
#include <QRadioButton>
#include <QScopeGuard>
#include <QSettings>
#include <QStyleOptionComboBox>
#include "FakeCli.h"
#include "AccountCatalog.h"
#include "AccountUsage.h"
#include "AccountsPage.h"
#include "NewSessionDialog.h"

static int launchAccountRow(const QListWidget *list,const QString &id) {
    for(int row=0;row<list->count();++row)if(list->item(row)->data(Qt::UserRole).toString()==id)return row;
    return -1;
}
static QString selectedLaunchAccount(const QListWidget *list) {
    return list->currentItem()?list->currentItem()->data(Qt::UserRole).toString():QString();
}

class TestAccountsPage : public QObject {
    Q_OBJECT
private slots:
    void initTestCase();
    void init();
    void nativeDefaultsAndMachineScopedActions();
    void openCodeNativeConfigurationDoesNotOfferUnsupportedAccountMutations();
    void repliesFromRemovedMachineAreIgnored();
    void explicitMachineOutsidePollingSurvivesRefreshAndExpiresWithFilter();
    void launchCarriesAccountForSelectedMachine();
    void newSessionRemembersAgentAndAccountPerMachine();
    void renameAndDetailsStayBoundToSelectedProfile();
    void removeAndReAddNativeAccount();
    void savedSignInKeepsDifferentAccountsSeparate();
    void hiddenDefaultIsNotOfferedForNewSessions();
    void sameAccountHasMachineTagsAndScopedActions();
    void identityGroupingSeparatesProvidersOrganizationsAndUnknownProfiles();
    void addToMachineOffersCopyOrNativeSignIn();
    void missingAgentInstallsOnSelectedMachine();
    void deepSeekKeyStaysOnSelectedMachine();
    void refreshPreservesRowsAndShowsPendingUsage();
    void initialLoadingPublishesOnlyGroupedAccounts();
    void loadingFinishesWithPartialOrUnavailableResults_data();
    void loadingFinishesWithPartialOrUnavailableResults();
    void permissionsStayBoundToSelectedMachine();
    void permissionsStayBoundToSelectedMachine_data();
    void permissionBadgesReflectInheritedAndMixedModes();
    void claudeSignInAndKeychainStatus_data();
    void claudeSignInAndKeychainStatus();
    void defaultAccountIsMachineScopedAndUsedForNewSessions();
    void detailsPaneShowsPageBackground();
    void controlOnlyComputerManagesAccountsOnRemoteMachinesOnly();
    void preview();
private:
    FleetState fleet(bool remote = true) const;
    QTemporaryDir m_dir,m_settings;
    QString m_script;
};
void TestAccountsPage::initTestCase()
{
    QVERIFY(m_settings.isValid());
    QCoreApplication::setOrganizationName("hgs-tests"); QCoreApplication::setApplicationName("accounts-page");
    QSettings::setDefaultFormat(QSettings::IniFormat); QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, m_settings.path());
}
void TestAccountsPage::init()
{
    QSettings().clear();
    QVERIFY(m_dir.isValid()); m_script = m_dir.filePath("hgs");
    for (const auto &name : QDir(m_dir.path()).entryList(QDir::Files)) QFile::remove(m_dir.filePath(name));
    QFile script(m_script); QVERIFY(script.open(QIODevice::WriteOnly));
    script.write(R"(#!/usr/bin/env python3
import json,pathlib,sys,time
root=pathlib.Path(__file__).parent
# Tests read these once they exist, so each appears complete.
def publish(name,text):staged=root/(name+'.tmp');staged.write_text(text);staged.replace(root/name)
args=sys.argv[1:]; host=''
if args[0].startswith('@'): host=args.pop(0)[1:]
with (root/'calls').open('a') as calls:calls.write(json.dumps([host]+args)+'\n')
if host and (root/'slow').exists(): time.sleep(.15)
if args[0]=='account':
 if args[1]=='ls' and host:
  while (root/'hold-remote-catalog').exists():time.sleep(.02)
 if args[1]=='ls' and host and (root/'fail-catalog').exists():sys.exit(1)
 if args[1]=='ls' and (root/'empty-catalog').exists():print(json.dumps({'profiles':[]}));sys.exit(0)
 if args[1]=='inspect':
  while (root/'hold-inspect').exists() or (host and (root/'hold-remote-inspect').exists()):time.sleep(.02)
  if (root/'fail-inspect').exists():sys.exit(1)
  if (root/'slow-inspect').exists():time.sleep(.15)
  provider=args[2].removeprefix('native-') if args[2].startswith('native-') else 'codex'
  if provider=='claude' and (root/'claude-status.json').exists():
   data=json.loads((root/'claude-status.json').read_text());data.update(id=args[2],provider=provider,home='~/.claude',checked_at=time.time(),identity={'email':host+'-claude@example.test','auth_method':'none'},windows=[])
   print(json.dumps(data));sys.exit(0)
  email=args[2]+'@example.test'
  if host and args[2].startswith('native-') and not ((root/'shared-identity').exists() and provider=='codex'):email=host+'-'+email
  print(json.dumps({'id':args[2],'home':'~/.config/hgs/accounts/work' if args[2].startswith('work-') else '~/.'+provider,'provider':provider,'identity':{} if provider=='dsh' else {'email':email,'plan':'Pro'},'status':'signed_out' if provider=='dsh' else 'ok','checked_at':time.time(),'windows':[] if provider=='dsh' else [{'id':'5h','used_percent':78,'window_minutes':300,'resets_at':time.time()+3600},{'id':'7d','used_percent':94,'window_minutes':10080,'resets_at':time.time()+86400}]}));sys.exit(0)
 if args[1] in ('copy','add'):
  publish('action',json.dumps({'host':host,'args':args}))
 if args[1]=='permissions':
  publish('permissions',json.dumps({'host':host,'args':args}))
 if args[1]=='default':
  publish('default-action',json.dumps({'host':host,'args':args}))
  (root/('default-'+(host or 'local'))).write_text(args[2])
 if args[1]=='rename':
  publish('renamed',json.dumps({'host':host,'args':args}))
  with (root/'rename-calls').open('a') as out:out.write(json.dumps({'host':host,'args':args})+'\n')
 removed_file=root/('removed-'+(host or 'local'))
 if args[1]=='set-key':
  data=json.load(sys.stdin)
  (root/('key-'+(host or 'local'))).write_text(json.dumps({'args':args,'input':data}))
  removed_file.unlink(missing_ok=True)
 if args[1]=='rm':removed_file.write_text(args[2])
 if args[1]=='restore':removed_file.unlink(missing_ok=True)
 profiles=[{'id':'native-'+p,'provider':p,'label':'Default account','home':'~/.codex' if p=='codex' else '~/.'+p,'native':True,'installed':True,'portable':p!='claude','credential_file':False} for p in ['codex','claude','kimi']]
 profiles.append({'id':'work-'+(host or 'local'),'provider':'codex','label':'Work','home':'~/.config/hgs/accounts/work','native':False,'installed':True,'portable':True,'credential_file':True})
 for p in profiles:
  chosen=(root/('default-'+(host or 'local')))
  p['is_default']=p['id']==(chosen.read_text() if chosen.exists() and p['provider']=='codex' else 'native-'+p['provider'])
 if (root/'deepseek').exists():
  profiles.append({'id':'native-dsh','provider':'dsh','label':'Native DeepSeek account','home':'~/.dsh','native':True,'installed':not (host=='mac' and (root/'missing-dsh').exists()),'portable':False,'credential_file':False})
 if (root/'portable-auth').exists():
  for p in profiles:
   if p['portable']:p['credential_file']=True
 if (root/'missing-agent').exists() and host=='mac':
  for p in profiles:
   if p['provider']=='codex':p['installed']=False
 if (root/'permission-modes').exists():
  for p in profiles:
   p['permission_mode']='provider';p['effective_permission_mode']='default' if host else 'bypass';p['permission_detail']='native setting'
 removed=[p for p in profiles if removed_file.exists() and p['id']==removed_file.read_text()]
 print(json.dumps({'host':host or 'arch','profiles':[p for p in profiles if p not in removed],'removed_profiles':removed,'revision':'rev1'}))
elif args[0]=='dirs': print(json.dumps({'path':args[1] if len(args)>1 else '/tmp/sample','directories':[]}))
elif args[0]=='project': print(json.dumps([{'name':'sample','dir':'/tmp/sample','src':'local','exists':True}]))
else: print('{}')
)"); script.close(); QVERIFY(FakeCli::makeRunnable(script.fileName()));
}
void TestAccountsPage::openCodeNativeConfigurationDoesNotOfferUnsupportedAccountMutations()
{
    QFile original(m_script);QVERIFY(original.open(QIODevice::ReadOnly));
    const auto bytes=original.readAll().replace("for p in ['codex','claude','kimi']","for p in ['codex','claude','kimi','opencode']")
        .replace("'installed':True,'portable':p!='claude'","'installed':p!='opencode','portable':p not in ('claude','opencode')");
    original.close();QVERIFY(FakeCli::replace(m_script,bytes));QVERIFY(FakeCli::makeRunnable(m_script));
    AccountsPage page(FakeCli::program(m_script));page.setFleet(fleet(false));page.show();
    auto *list=page.findChild<QListWidget *>("accountProfiles");QTRY_COMPARE(list->count(),5);
    page.showAccount({},"native-opencode");QTRY_COMPARE(list->currentItem()->data(Qt::UserRole).toJsonObject()["provider"].toString(),QString("opencode"));
    QSignalSpy install(&page,&AccountsPage::installRequested),login(&page,&AccountsPage::loginRequested);
    for(const auto *name:{"loginAccount","installAccountAgent","accountPermissions","renameAccount","removeAccount","copyAccount"}) {
        auto *button=page.findChild<QPushButton *>(name);QVERIFY(button);QVERIFY(!button->isEnabled());button->click();
    }
    QVERIFY(install.isEmpty());QVERIFY(login.isEmpty());QVERIFY(!page.findChild<QPushButton *>("defaultAccount")->isVisible());
    QVERIFY(page.findChild<QPushButton *>("installAccountAgent")->toolTip().contains("OpenCode"));
    page.showAccount({},"native-codex");QTRY_VERIFY(page.findChild<QPushButton *>("renameAccount")->isEnabled());
    QVERIFY(!page.findChild<QPushButton *>("renameAccount")->toolTip().contains("OpenCode"));
}

void TestAccountsPage::permissionBadgesReflectInheritedAndMixedModes()
{
    QFile modes(m_dir.filePath("permission-modes")); QVERIFY(modes.open(QIODevice::WriteOnly)); modes.close();
    QFile shared(m_dir.filePath("shared-identity")); QVERIFY(shared.open(QIODevice::WriteOnly)); shared.close();
    AccountsPage page(FakeCli::program(m_script)); page.setFleet(fleet()); page.resize(1000,700); page.show();
    auto *list = page.findChild<QListWidget *>("accountProfiles");
    page.showAccount({},"native-codex");
    auto *state = page.findChild<QLabel *>("accountPermissionState");
    QTRY_VERIFY(state->text().contains("Bypass")); QVERIFY(state->styleSheet().contains("color:"));
    QCOMPARE(page.findChild<QPushButton *>("accountPermissions")->text(), QString("Change…"));
    QVERIFY(state->toolTip().contains("From provider settings"));
    QTRY_VERIFY(list->currentItem()->text().contains("Varies"));
    QVERIFY(list->currentItem()->toolTip().contains("arch: Bypass"));
    QVERIFY(list->currentItem()->toolTip().contains("mac: Default"));
    page.showAccount("mac","native-codex"); QTRY_VERIFY(state->text().contains("on mac: Default"));
    QVERIFY(state->styleSheet().isEmpty());
    page.showAccount({},"native-kimi"); QTRY_VERIFY(list->currentItem()->text().contains("Permissions: Bypass"));
}

void TestAccountsPage::claudeSignInAndKeychainStatus_data()
{
    QTest::addColumn<QString>("status");QTest::addColumn<QString>("authStatus");QTest::addColumn<QString>("message");
    QTest::newRow("quota-unavailable")<<"unavailable"<<"signed_in"<<"Ready on mac.";
    QTest::newRow("locked")<<"credentials_locked"<<"credentials_locked"<<"Keychain on mac is locked";
    QTest::newRow("desktop-unavailable")<<"desktop_session_unavailable"<<"desktop_session_unavailable"<<"Desktop sign-in on mac is unavailable";
    QTest::newRow("inaccessible")<<"credentials_unavailable"<<"credentials_unavailable"<<"saved sign-in on mac could not be used";
}
void TestAccountsPage::claudeSignInAndKeychainStatus()
{
    QFETCH(QString,status);QFETCH(QString,authStatus);QFETCH(QString,message);
    QFile data(m_dir.filePath("claude-status.json"));QVERIFY(data.open(QIODevice::WriteOnly));
    data.write(QJsonDocument(QJsonObject{{"status",status},{"auth_status",authStatus}}).toJson());data.close();
    AccountsPage page(FakeCli::program(m_script));page.setFleet(fleet());page.show();
    QTRY_COMPARE(page.findChild<QListWidget *>("accountProfiles")->count(),8);
    page.showAccount("mac","native-claude");
    auto *detail=page.findChild<QLabel *>("accountMachineStatus");
    QTRY_VERIFY2(detail->text().contains(message),qPrintable(detail->text()));
    QVERIFY(!page.findChild<QLabel *>("accountIdentity")->text().contains("Sign-in: none"));
    QCOMPARE(page.findChild<QPushButton *>("loginAccount")->text().contains("again"),authStatus=="signed_in");
}

void TestAccountsPage::defaultAccountIsMachineScopedAndUsedForNewSessions()
{
    AccountsPage page(FakeCli::program(m_script));page.setFleet(fleet());page.show();
    QTRY_COMPARE(page.findChild<QListWidget *>("accountProfiles")->count(),8);
    page.showAccount("mac","work-mac");auto *button=page.findChild<QPushButton *>("defaultAccount");
    QTRY_VERIFY(button->isEnabled());QVERIFY(button->text().contains("mac"));button->click();
    QTRY_VERIFY(QFile::exists(m_dir.filePath("default-action")));QTRY_VERIFY(!button->isEnabled());
    QFile action(m_dir.filePath("default-action"));QVERIFY(action.open(QIODevice::ReadOnly));
    auto data=QJsonDocument::fromJson(action.readAll()).object();QCOMPARE(data["host"].toString(),QString("mac"));
    QCOMPARE(data["args"].toArray(),QJsonArray({"account","default","work-mac","--revision","rev1"}));
    QVERIFY(!QFile::exists(m_dir.filePath("default-local")));
    NewSessionDialog dialog(FakeCli::program(m_script),fleet(),"mac","codex");dialog.show();
    auto *accounts=dialog.findChild<QListWidget *>("launchAccount");
    QTRY_COMPARE(selectedLaunchAccount(accounts),QString("work-mac"));QVERIFY(accounts->currentItem()->text().contains("default"));
    dialog.selectAccount("native-codex");QCOMPARE(selectedLaunchAccount(accounts),QString("native-codex"));
    dialog.findChild<QComboBox *>("launchComputer")->setCurrentIndex(0);
    QTRY_COMPARE(selectedLaunchAccount(accounts),QString("native-codex"));
}

void TestAccountsPage::permissionsStayBoundToSelectedMachine_data()
{
    QTest::addColumn<int>("fontPixels");QTest::newRow("normal")<<13;QTest::newRow("large")<<18;
}
void TestAccountsPage::permissionsStayBoundToSelectedMachine()
{
    QFETCH(int,fontPixels);AccountsPage page(FakeCli::program(m_script));page.setFleet(fleet());
    page.setStyleSheet(QString("QWidget {font-size:%1px;} QPushButton {padding:8px 13px;min-height:18px;} QComboBox {padding:9px 10px;} QComboBox::drop-down {width:24px;}").arg(fontPixels));page.show();
    auto *list=page.findChild<QListWidget *>("accountProfiles");QTRY_COMPARE(list->count(),8);
    page.showAccount("mac","native-kimi");
    auto *button=page.findChild<QPushButton *>("accountPermissions");QTRY_VERIFY(button->isEnabled());
    QTimer::singleShot(0,&page,[&]{
        auto *dialog=page.findChild<QDialog *>("accountPermissionsDialog");QVERIFY(dialog);
        auto *mode=dialog->findChild<QComboBox *>("accountPermissionMode");QCOMPARE(mode->currentData().toString(),QString("provider"));
        mode->setCurrentIndex(mode->findData("bypass"));QTest::qWait(30);
        QStyleOptionComboBox option;option.initFrom(mode);option.currentText=mode->currentText();
        const auto field=mode->style()->subControlRect(QStyle::CC_ComboBox,&option,QStyle::SC_ComboBoxEditField,mode);
        QVERIFY(field.width()>=mode->fontMetrics().horizontalAdvance(mode->currentText()));QVERIFY(field.height()>=mode->fontMetrics().height());
        for(auto *control:dialog->findChild<QDialogButtonBox *>()->buttons()) {
            QVERIFY(dialog->rect().contains(QRect(control->mapTo(dialog,QPoint()),control->size())));
            QVERIFY(control->height()>=control->sizeHint().height());
        }
        const auto dir=qEnvironmentVariable("HGS_ACCOUNTS_PREVIEW");if(!dir.isEmpty()){QDir().mkpath(dir);QVERIFY(dialog->grab().save(dir+QString("/permissions-%1.png").arg(fontPixels)));}
        dialog->accept();
    });
    button->click();QTRY_VERIFY(QFile::exists(m_dir.filePath("permissions")));
    QFile file(m_dir.filePath("permissions"));QVERIFY(file.open(QIODevice::ReadOnly));const auto action=QJsonDocument::fromJson(file.readAll()).object();
    QCOMPARE(action["host"].toString(),QString("mac"));QCOMPARE(action["args"].toArray(),QJsonArray({"account","permissions","native-kimi","--mode","bypass","--revision","rev1"}));
}

void TestAccountsPage::renameAndDetailsStayBoundToSelectedProfile()
{
    QFile slow(m_dir.filePath("slow-inspect"));QVERIFY(slow.open(QIODevice::WriteOnly));slow.close();
    AccountsPage page(FakeCli::program(m_script));page.setFleet(fleet());page.show();QSignalSpy notices(&page,&AccountsPage::notice);
    auto *list=page.findChild<QListWidget *>("accountProfiles");QTRY_COMPARE(list->count(),8);
    page.showAccount("mac","work-mac");
    QTRY_COMPARE(list->currentItem()->data(Qt::UserRole).toJsonObject()["id"].toString(),QString("work-mac"));
    QTRY_VERIFY(page.findChild<QLabel *>("accountIdentity")->text().contains("work-mac@example.test"));
    QVERIFY(!page.findChild<QLabel *>("accountIdentity")->text().contains("native-codex@example.test"));
    QTimer::singleShot(0,&page,[&]{
        auto *dialog=page.findChild<QDialog *>("renameAccountDialog");QVERIFY(dialog);
        dialog->findChild<QLineEdit *>("accountName")->setText("Work Mac");dialog->accept();
    });
    page.findChild<QPushButton *>("renameAccount")->click();
    QTRY_VERIFY(QFile::exists(m_dir.filePath("renamed")));QFile file(m_dir.filePath("renamed"));QVERIFY(file.open(QIODevice::ReadOnly));
    const auto call=QJsonDocument::fromJson(file.readAll()).object();QCOMPARE(call["host"].toString(),QString("mac"));
    QCOMPARE(call["args"].toArray(),QJsonArray({"account","rename","work-mac","--label","Work Mac","--revision","rev1"}));
    QTRY_COMPARE(notices.size(),1);QVERIFY(notices.first()[0].toString().contains("name updated"));
    list->setCurrentRow(0);QCOMPARE(notices.size(),1);QVERIFY(!page.findChild<QLabel *>("accountStatus"));
    QFile::remove(m_dir.filePath("slow-inspect"));
}
void TestAccountsPage::detailsPaneShowsPageBackground()
{
    // The window themes the page with a style sheet while the application
    // palette keeps the desktop colour, which the details pane must not paint.
    AccountsPage page(FakeCli::program(m_script));page.setStyleSheet("QWidget#accountsPage{background:#161b21;}");page.setFleet(fleet());page.resize(1400,900);page.show();
    QTRY_COMPARE(page.findChild<QListWidget *>("accountProfiles")->count(),8);
    auto *viewport=page.findChild<QScrollArea *>("accountDetailsScroll")->viewport();QVERIFY(viewport->width()>page.findChild<QWidget *>("accountDetails")->width());
    const auto image=page.grab().toImage();const qreal ratio=image.devicePixelRatio();
    const auto pixel=[&](QPoint point){return image.pixelColor(viewport->mapTo(&page,point)*ratio).name();};
    QCOMPARE(pixel({8,viewport->height()-8}),QString("#161b21"));
    QCOMPARE(pixel({viewport->width()-8,8}),QString("#161b21"));
}
FleetState TestAccountsPage::fleet(bool remote) const
{
    FleetState fleet; BoxState local; local.host = "arch"; local.ok = true; local.peersKnown = true;
    if (remote) local.peers = {"mac"}; fleet.setLocal(local, 0); return fleet;
}
void TestAccountsPage::controlOnlyComputerManagesAccountsOnRemoteMachinesOnly()
{
    // hgs.exe on Windows runs no agents: no account command goes to this computer.
    auto lonely = fleet(false); auto local = lonely.local(); local.localSessions = false; lonely.setLocal(local, 0);
    const auto hinted = [](AccountsPage &page) {
        for (auto *label : page.findChildren<QLabel *>())
            if (label->isVisible() && label->text().startsWith("Add a remote machine in Machines")) return true;
        return false;
    };
    {
        AccountsPage page(FakeCli::program(m_script)); page.setFleet(lonely); page.show();
        auto *add = page.findChild<QPushButton *>("addAccount");
        QTRY_VERIFY(hinted(page)); QVERIFY(!add->isEnabled()); QVERIFY(add->toolTip().startsWith("Add a remote machine in Machines"));
    }
    auto remote = fleet(); local = remote.local(); local.localSessions = false; remote.setLocal(local, 0);
    AccountsPage page(FakeCli::program(m_script)); page.setFleet(remote); page.show();
    auto *add = page.findChild<QPushButton *>("addAccount");
    auto *list = page.findChild<QListWidget *>("accountProfiles"); QTRY_COMPARE(list->count(), 4);
    QTRY_VERIFY(add->isEnabled()); QVERIFY(add->toolTip().isEmpty()); QVERIFY(!hinted(page));
    QTimer::singleShot(0, &page, [&] {
        auto *dialog = page.findChild<QDialog *>("accountDialog"); QVERIFY(dialog);
        const auto closeOnFailure = qScopeGuard([dialog] { if (dialog->isVisible()) dialog->reject(); });
        auto *machine = dialog->findChild<QComboBox *>("addAccountMachine");
        QCOMPARE(machine->count(), 1); QCOMPARE(machine->itemData(0).toString(), QString("mac"));
        dialog->findChild<QLineEdit *>("addAccountName")->setText("Team");
        for (auto *button : dialog->findChildren<QPushButton *>())
            if (button->text() == "Create and sign in") { QVERIFY(button->isEnabled()); button->click(); return; }
        QFAIL("No create button");
    });
    add->click();
    QTRY_VERIFY(QFile::exists(m_dir.filePath("action")));
    QFile action(m_dir.filePath("action")); QVERIFY(action.open(QIODevice::ReadOnly));
    QCOMPARE(QJsonDocument::fromJson(action.readAll()).object()["host"].toString(), QString("mac"));
    QFile calls(m_dir.filePath("calls")); QVERIFY(calls.open(QIODevice::ReadOnly)); int remoteCalls = 0;
    for (const auto &line : calls.readAll().split('\n')) {
        if (line.isEmpty()) continue;
        const auto call = QJsonDocument::fromJson(line).array();
        QVERIFY2(!call.first().toString().isEmpty(), line.constData()); ++remoteCalls;
    }
    QVERIFY(remoteCalls > 0);
}
void TestAccountsPage::nativeDefaultsAndMachineScopedActions()
{
    AccountsPage page(FakeCli::program(m_script)); page.setFleet(fleet()); page.show();
    auto *list = page.findChild<QListWidget *>("accountProfiles"); auto *add = page.findChild<QPushButton *>("addAccount");
    QTRY_COMPARE(list->count(), 8); QTRY_VERIFY(add->isEnabled());
    list->setCurrentRow(0);
    QVERIFY(page.findChild<QPushButton *>("removeAccount")->isEnabled());
    QVERIFY(page.findChild<QPushButton *>("loginAccount")->isEnabled());
    QVERIFY(!list->item(0)->data(Qt::UserRole).toJsonObject()["credential_file"].toBool());
    auto *filter = page.findChild<QComboBox *>("accountMachineFilter"); filter->setCurrentIndex(filter->findData("mac"));
    QCOMPARE(list->count(), 4); list->setCurrentRow(3);
    QVERIFY(page.findChild<QPushButton *>("removeAccount")->isEnabled());
    QSignalSpy login(&page, &AccountsPage::loginRequested); page.findChild<QPushButton *>("loginAccount")->click();
    QCOMPARE(login.size(), 1); QCOMPARE(login[0][0].toString(), "mac"); QCOMPARE(login[0][1].toString(), "work-mac");
    list->setCurrentRow(1); QVERIFY(page.findChild<QPushButton *>("copyAccount")->isEnabled());
}
void TestAccountsPage::repliesFromRemovedMachineAreIgnored()
{
    QFile slow(m_dir.filePath("slow")); QVERIFY(slow.open(QIODevice::WriteOnly)); slow.close();
    AccountsPage page(FakeCli::program(m_script)); page.setFleet(fleet()); page.show();
    page.setFleet(fleet(false));
    auto *list = page.findChild<QListWidget *>("accountProfiles");
    QTRY_VERIFY(page.findChild<QPushButton *>("refreshAccounts")->isEnabled());
    QCOMPARE(list->count(), 4);
    for (int i = 0; i < list->count(); ++i) QVERIFY(list->item(i)->data(Qt::UserRole).toJsonObject()["host"].toString().isEmpty());
}
void TestAccountsPage::explicitMachineOutsidePollingSurvivesRefreshAndExpiresWithFilter()
{
    QFile slow(m_dir.filePath("slow")); QVERIFY(slow.open(QIODevice::WriteOnly)); slow.close();
    AccountsPage page(FakeCli::program(m_script)); page.setFleet(fleet(false)); page.show();
    // showEvent has already requested the local catalog; the explicit request must still start.
    page.showMachine("disabled"); page.setFleet(fleet(false));
    auto *filter = page.findChild<QComboBox *>("accountMachineFilter");
    auto *list = page.findChild<QListWidget *>("accountProfiles");
    QCOMPARE(filter->currentData().toString(), "disabled");
    QTRY_COMPARE(list->count(), 4); QTRY_VERIFY(page.findChild<QPushButton *>("refreshAccounts")->isEnabled());
    page.setFleet(fleet(false)); QCOMPARE(filter->currentData().toString(), "disabled"); QCOMPARE(list->count(), 4);
    list->setCurrentRow(3); QSignalSpy login(&page, &AccountsPage::loginRequested);
    page.findChild<QPushButton *>("loginAccount")->click(); QCOMPARE(login.size(), 1); QCOMPARE(login[0][0].toString(), "disabled");
    page.reload(); filter->setCurrentIndex(filter->findData("*")); page.setFleet(fleet(false));
    QCOMPARE(filter->findData("disabled"), -1);
    QTRY_VERIFY(page.findChild<QPushButton *>("refreshAccounts")->isEnabled());
    QCOMPARE(list->count(), 4);
    for (int i = 0; i < list->count(); ++i) QVERIFY(list->item(i)->data(Qt::UserRole).toJsonObject()["host"].toString().isEmpty());
}
void TestAccountsPage::launchCarriesAccountForSelectedMachine()
{
    auto available=fleet();BoxState mac;mac.host="mac";mac.ok=true;available.setPeer(mac,QDateTime::currentMSecsSinceEpoch());
    NewSessionDialog dialog(FakeCli::program(m_script), available, "mac", "codex");
    SessionOrganization projects; projects.addFolder("ungrouped","arch","/tmp/sample"); projects.addFolder("ungrouped","mac","/tmp/sample");
    dialog.setGroups(projects,"ungrouped"); dialog.show();
    auto *accounts = dialog.findChild<QListWidget *>("launchAccount"); auto *start = dialog.findChild<QPushButton *>("primary");
    QTRY_COMPARE(accounts->count(), 6); QTRY_VERIFY(start->isEnabled());
    accounts->setCurrentRow(launchAccountRow(accounts,"work-mac")); QCOMPARE(selectedLaunchAccount(accounts), "work-mac");
    auto *machine = dialog.findChild<QComboBox *>("launchComputer"); QVERIFY(machine);
    machine->setCurrentIndex(machine->findData(QString()));
    QTRY_COMPARE(accounts->count(), 6); QCOMPARE(accounts->currentRow(), 0);
    QCOMPARE(accounts->item(1)->data(Qt::UserRole).toString(), "work-local");
    accounts->setCurrentRow(launchAccountRow(accounts,"work-local")); QTRY_VERIFY(start->isEnabled());
    QSignalSpy launched(&dialog, &NewSessionDialog::launchRequested); start->click();
    QTRY_COMPARE(launched.size(), 1); QCOMPARE(launched[0][0].toString(), ""); QCOMPARE(launched[0][4].toString(), "work-local");
}
void TestAccountsPage::newSessionRemembersAgentAndAccountPerMachine()
{
    auto available=fleet();BoxState mac;mac.host="mac";mac.ok=true;available.setPeer(mac,QDateTime::currentMSecsSinceEpoch());
    SessionOrganization projects; projects.addFolder("ungrouped","arch","/tmp/sample"); projects.addFolder("ungrouped","mac","/tmp/sample");
    const auto launch=[&](const QString &agent,const QString &account) {
        NewSessionDialog dialog(FakeCli::program(m_script),available,"mac",agent); dialog.setGroups(projects,"ungrouped"); dialog.show();
        auto *accounts=dialog.findChild<QListWidget *>("launchAccount"); QTRY_VERIFY(launchAccountRow(accounts,account)>=0);
        accounts->setCurrentRow(launchAccountRow(accounts,account));
        auto *start=dialog.findChild<QPushButton *>("primary"); QTRY_VERIFY(start->isEnabled());
        QSignalSpy launched(&dialog,&NewSessionDialog::launchRequested); start->click(); QTRY_COMPARE(launched.size(),1);
    };
    {
        // Nothing is remembered yet, and cancelling a dialog remembers nothing.
        NewSessionDialog dialog(FakeCli::program(m_script),available,"mac"); dialog.show();
        QCOMPARE(dialog.findChild<QListWidget *>("launchAccount")->currentItem()->data(Qt::UserRole+1).toString(),QString("codex"));
        auto *choices=dialog.findChild<QListWidget *>("launchAccount");choices->setCurrentRow(launchAccountRow(choices,"native-kimi")); dialog.reject();
    }
    launch("codex","work-mac"); launch("claude","native-claude");
    NewSessionDialog dialog(FakeCli::program(m_script),available,"mac"); dialog.setGroups(projects,"ungrouped"); dialog.show();
    auto *accounts=dialog.findChild<QListWidget *>("launchAccount");
    QCOMPARE(accounts->currentItem()->data(Qt::UserRole+1).toString(),QString("claude"));
    QTRY_VERIFY(launchAccountRow(accounts,"work-mac")>=0);accounts->setCurrentRow(launchAccountRow(accounts,"work-mac"));QCOMPARE(selectedLaunchAccount(accounts),QString("work-mac"));
    // Accounts are remembered per machine: arch has its own Work account, but none was chosen there.
    auto *machine=dialog.findChild<QComboBox *>("launchComputer"); machine->setCurrentIndex(machine->findData(QString()));
    QTRY_COMPARE(accounts->item(1)->data(Qt::UserRole).toString(),QString("work-local")); QCOMPARE(selectedLaunchAccount(accounts),QString("native-codex"));
    // A remembered account that disappeared falls back to the default without an error.
    QFile removed(m_dir.filePath("removed-mac"));QVERIFY(removed.open(QIODevice::WriteOnly));removed.write("work-mac");removed.close();
    NewSessionDialog fallback(FakeCli::program(m_script),available,"mac","codex"); fallback.setGroups(projects,"ungrouped"); fallback.show();
    auto *fallbackAccounts=fallback.findChild<QListWidget *>("launchAccount");
    QTRY_VERIFY(fallbackAccounts->currentItem()->text().contains("default")); QCOMPARE(selectedLaunchAccount(fallbackAccounts),QString("native-codex"));
    QVERIFY(fallback.findChild<QLabel *>("launchError")->text().isEmpty());
    // An agent requested by the caller wins over the remembered one.
    NewSessionDialog requested(FakeCli::program(m_script),available,"mac","kimi");
    QCOMPARE(requested.findChild<QListWidget *>("launchAccount")->currentItem()->data(Qt::UserRole+1).toString(),QString("kimi"));
}
void TestAccountsPage::hiddenDefaultIsNotOfferedForNewSessions()
{
    QFile removed(m_dir.filePath("removed-local"));QVERIFY(removed.open(QIODevice::WriteOnly));removed.write("native-codex");removed.close();
    NewSessionDialog dialog(FakeCli::program(m_script),fleet(false),{},"codex","/tmp/sample");dialog.show();
    auto *accounts=dialog.findChild<QListWidget *>("launchAccount");
    QTRY_COMPARE(accounts->count(),5);QTRY_COMPARE(accounts->item(0)->data(Qt::UserRole).toString(),QString("work-local"));
}

void TestAccountsPage::removeAndReAddNativeAccount()
{
    AccountsPage page(FakeCli::program(m_script));page.setFleet(fleet(false));page.show();
    auto *list=page.findChild<QListWidget *>("accountProfiles");QTRY_COMPARE(list->count(),4);
    // Catalog and identity replies can finish in any order. Pin the provider
    // under test rather than relying on the first row selected during loading.
    page.showAccount({},"native-codex");
    QTRY_VERIFY(list->currentItem());
    QTRY_COMPARE(list->currentItem()->data(Qt::UserRole).toJsonObject()["id"].toString(),QString("native-codex"));
    QTRY_VERIFY(page.findChild<QPushButton *>("removeAccount")->isEnabled());
    QTimer::singleShot(0,&page,[&] {
        auto *dialog=page.findChild<QMessageBox *>("removeAccountDialog");QVERIFY(dialog);
        QCOMPARE(dialog->defaultButton(),dialog->button(QMessageBox::Cancel));
        QVERIFY(dialog->text().contains("arch"));
        QTest::keyClick(dialog,Qt::Key_Escape);
    });
    page.findChild<QPushButton *>("removeAccount")->click();
    QCOMPARE(list->count(),4);QVERIFY(!QFile::exists(m_dir.filePath("removed-local")));
    QTimer::singleShot(0,&page,[&] {
        auto *dialog=page.findChild<QMessageBox *>("removeAccountDialog");QVERIFY(dialog);
        for(auto *button:dialog->buttons())if(dialog->buttonRole(button)==QMessageBox::DestructiveRole){button->click();return;}
        QFAIL("No explicit removal button");
    });
    page.findChild<QPushButton *>("removeAccount")->click();QTRY_COMPARE(list->count(),3);
    QVERIFY(!page.findChild<QPushButton *>("restoreAccount"));QSignalSpy login(&page,&AccountsPage::loginRequested);
    QTRY_VERIFY(page.findChild<QPushButton *>("addAccount")->isEnabled());
    QTimer::singleShot(0,&page,[&]{
        auto *dialog=page.findChild<QDialog *>("accountDialog");QVERIFY(dialog);
        const auto closeOnFailure=qScopeGuard([dialog]{if(dialog->isVisible())dialog->reject();});
        auto *saved=dialog->findChild<QComboBox *>("savedAccount");QVERIFY(saved->isVisible());
        QCOMPARE(saved->currentData().toJsonObject()["id"].toString(),QString("native-codex"));dialog->accept();
    });
    page.findChild<QPushButton *>("addAccount")->click();QTRY_COMPARE(list->count(),4);
    QCOMPARE(login.size(),0);QVERIFY(!QFile::exists(m_dir.filePath("action")));
    QCOMPARE(list->currentItem()->data(Qt::UserRole).toJsonObject()["id"].toString(),QString("native-codex"));
}
void TestAccountsPage::sameAccountHasMachineTagsAndScopedActions()
{
    QFile shared(m_dir.filePath("shared-identity")); QVERIFY(shared.open(QIODevice::WriteOnly)); shared.close();
    AccountsPage page(FakeCli::program(m_script)); page.setFleet(fleet()); page.show();
    auto *list = page.findChild<QListWidget *>("accountProfiles"); QTRY_COMPARE(list->count(), 7);
    page.showAccount("mac", "native-codex");
    QTRY_COMPARE(list->currentItem()->data(Qt::UserRole).toJsonObject()["members"].toArray().size(), 2);
    QTRY_COMPARE(page.findChildren<QPushButton *>("accountMachineTag").size(), 2);
    QVERIFY(!page.findChild<QPushButton *>("copyAccount")->isEnabled());
    QSignalSpy login(&page, &AccountsPage::loginRequested);
    QTRY_COMPARE(page.findChild<QPushButton *>("loginAccount")->text(), QString("Sign in again on mac…"));
    page.findChild<QPushButton *>("loginAccount")->click();
    QCOMPARE(login.takeFirst(), QVariantList({QString("mac"), QString("native-codex")}));
    for (auto *tag : page.findChildren<QPushButton *>("accountMachineTag")) if (tag->property("host").toString().isEmpty()) tag->click();
    QTRY_COMPARE(page.findChild<QPushButton *>("loginAccount")->text(), QString("Sign in again on arch…"));
    page.findChild<QPushButton *>("loginAccount")->click();
    QCOMPARE(login.takeFirst(), QVariantList({QString(), QString("native-codex")}));
    QTimer::singleShot(0, &page, [&] {
        auto *dialog = page.findChild<QDialog *>("renameAccountDialog"); QVERIFY(dialog);
        dialog->findChild<QLineEdit *>("accountName")->setText("Shared work"); dialog->accept();
    });
    page.findChild<QPushButton *>("renameAccount")->click();
    QTRY_VERIFY(page.findChild<QPushButton *>("refreshAccounts")->isEnabled());
    QFile calls(m_dir.filePath("rename-calls")); QVERIFY(calls.open(QIODevice::ReadOnly));
    const auto renames = calls.readAll().trimmed().split('\n'); QCOMPARE(renames.size(), 2);
    QCOMPARE(QJsonDocument::fromJson(renames[0]).object()["host"].toString(), QString());
    QCOMPARE(QJsonDocument::fromJson(renames[1]).object()["host"].toString(), QString("mac"));
    auto offline = fleet(); BoxState mac; mac.host = "mac"; mac.ok = false; offline.setPeer(mac, QDateTime::currentMSecsSinceEpoch());
    page.setFleet(offline);
    for (auto *tag : page.findChildren<QPushButton *>("accountMachineTag")) if (tag->property("host") == "mac") tag->click();
    QVERIFY(!page.findChild<QPushButton *>("loginAccount")->isEnabled());
    QCOMPARE(list->currentItem()->data(Qt::UserRole).toJsonObject()["members"].toArray().size(), 2);
    QVERIFY(page.findChild<QLabel *>("accountMachineStatus")->text().contains("offline"));
    page.setFleet(fleet());
    // Returning to the all-machines view preserves the selected account.
    auto *filter = page.findChild<QComboBox *>("accountMachineFilter"); filter->setCurrentIndex(0);
    QTRY_COMPARE(page.findChildren<QPushButton *>("accountMachineTag").size(), 2);
    for (auto *tag : page.findChildren<QPushButton *>("accountMachineTag")) if (tag->property("host") == "mac") tag->click();
    QTimer::singleShot(0,&page,[&] {
        auto *dialog=page.findChild<QMessageBox *>("removeAccountDialog");QVERIFY(dialog);
        QVERIFY(dialog->text().contains("mac"));
        QVERIFY(dialog->informativeText().contains("every Zerus connected to mac"));
        QVERIFY(dialog->informativeText().contains("Sign-in credentials and existing sessions on mac are kept"));
        // Background selection changes cannot redirect the confirmed removal.
        for(auto *tag:page.findChildren<QPushButton *>("accountMachineTag"))if(tag->property("host").toString().isEmpty())tag->click();
        for(auto *button:dialog->buttons())if(dialog->buttonRole(button)==QMessageBox::DestructiveRole){button->click();return;}
        QFAIL("No explicit removal button");
    });
    page.findChild<QPushButton *>("removeAccount")->click();
    QTRY_VERIFY(QFile::exists(m_dir.filePath("removed-mac")));
    QVERIFY(!QFile::exists(m_dir.filePath("removed-local")));
    QTRY_COMPARE(list->currentItem()->data(Qt::UserRole).toJsonObject()["members"].toArray().size(), 1);
    QVERIFY(page.findChild<QLabel *>("accountTitle")->text().contains("native-codex@example.test"));
    QTRY_VERIFY(page.findChild<QPushButton *>("copyAccount")->isEnabled());
    login.clear();
    QTimer::singleShot(0,&page,[&]{
        auto *dialog=page.findChild<QDialog *>("accountDialog");QVERIFY(dialog);
        QVERIFY(dialog->findChild<QRadioButton *>("reuseSavedSignIn")->isChecked());
        QCOMPARE(dialog->findChild<QComboBox *>("savedAccount")->currentData().toJsonObject()["id"].toString(),QString("native-codex"));
        dialog->accept();
    });
    page.findChild<QPushButton *>("accountDestinationTag")->click();
    QTRY_COMPARE(list->currentItem()->data(Qt::UserRole).toJsonObject()["members"].toArray().size(),2);
    QVERIFY(!QFile::exists(m_dir.filePath("removed-mac")));QVERIFY(!QFile::exists(m_dir.filePath("action")));QCOMPARE(login.size(),0);
}

void TestAccountsPage::savedSignInKeepsDifferentAccountsSeparate()
{
    AccountsPage page(FakeCli::program(m_script));page.setFleet(fleet());page.show();
    auto *list=page.findChild<QListWidget *>("accountProfiles");QTRY_COMPARE(list->count(),8);
    page.showAccount("mac","native-codex");QTRY_VERIFY(page.findChild<QLabel *>("accountTitle")->text().contains("mac-native-codex"));
    QTimer::singleShot(0,&page,[&]{
        auto *dialog=page.findChild<QMessageBox *>("removeAccountDialog");QVERIFY(dialog);
        for(auto *button:dialog->buttons())if(dialog->buttonRole(button)==QMessageBox::DestructiveRole){button->click();return;}
    });
    page.findChild<QPushButton *>("removeAccount")->click();QTRY_COMPARE(list->count(),7);
    page.showAccount({},"native-codex");QTRY_VERIFY(page.findChild<QPushButton *>("copyAccount")->isEnabled());
    QTimer::singleShot(0,&page,[&]{
        auto *dialog=page.findChild<QDialog *>("accountDialog");QVERIFY(dialog);
        auto *reuse=dialog->findChild<QRadioButton *>("reuseSavedSignIn");QVERIFY(reuse->isHidden());QVERIFY(!reuse->isChecked());dialog->reject();
    });
    page.findChild<QPushButton *>("accountDestinationTag")->click();
    QVERIFY(QFile::exists(m_dir.filePath("removed-mac")));QVERIFY(!QFile::exists(m_dir.filePath("action")));
}

void TestAccountsPage::identityGroupingSeparatesProvidersOrganizationsAndUnknownProfiles()
{
    const auto profile = [](QString host, QString provider, QString email, QString org = {}, QString id = {}) {
        return QJsonObject{{"host", host}, {"machine", host}, {"id", "native-" + provider}, {"label", "Default account"}, {"provider", provider},
            {"usage", QJsonObject{{"status", "ok"}, {"identity", QJsonObject{{"email", email}, {"organization", org}, {"account_id", id}}}}}};
    };
    auto local = profile("arch", "codex", "Me@Example.test", {}, "one");
    auto remote = profile("mac", "codex", "me@example.test");
    auto grouped = AccountCatalog::group({local, remote}); QCOMPARE(grouped.size(), 1);
    QCOMPARE(grouped[0].toObject()["members"].toArray().size(), 2);
    QCOMPARE(grouped[0].toObject()["machines"].toArray().size(), 2);
    QCOMPARE(AccountCatalog::group({local, profile("mac", "codex", "me@example.test", {}, "two"), remote}).size(), 3);
    QCOMPARE(AccountCatalog::group({local, profile("mac", "claude", "me@example.test")}).size(), 2);
    QCOMPARE(AccountCatalog::group({profile("arch", "claude", "same@example.test", "Team A"), profile("mac", "claude", "same@example.test", "Team B")}).size(), 2);
    QCOMPARE(AccountCatalog::group({profile("arch", "kimi", ""), profile("mac", "kimi", "")}).size(), 2);
    auto usage = remote["usage"].toObject(); usage["status"] = "signed_out"; remote["usage"] = usage;
    QCOMPARE(AccountCatalog::group({local, remote}).size(), 2);
    // Same account quotas on two machines are snapshots, never summed.
    usage = local["usage"].toObject(); usage["checked_at"] = 1; usage["windows"] = QJsonArray{QJsonObject{{"used_percent", 40}}}; local["usage"] = usage;
    remote = local; remote["host"] = "mac"; remote["machine"] = "mac";
    usage["checked_at"] = 2; usage["windows"] = QJsonArray{QJsonObject{{"used_percent", 45}}}; remote["usage"] = usage;
    grouped = AccountCatalog::group({local, remote}); QCOMPARE(grouped.size(), 1);
    QCOMPARE(grouped[0].toObject()["usage"].toObject()["windows"].toArray()[0].toObject()["used_percent"].toInt(), 45);
}

void TestAccountsPage::addToMachineOffersCopyOrNativeSignIn()
{
    QFile auth(m_dir.filePath("portable-auth")); QVERIFY(auth.open(QIODevice::WriteOnly)); auth.close();
    AccountsPage page(FakeCli::program(m_script)); page.setFleet(fleet()); page.show();
    auto *list = page.findChild<QListWidget *>("accountProfiles"); QTRY_COMPARE(list->count(), 8);
    QTRY_VERIFY(page.findChild<QPushButton *>("copyAccount")->isEnabled());
    page.showAccount({}, "native-codex");
    QTRY_COMPARE(page.findChildren<QPushButton *>("accountDestinationTag").size(), 1);
    QTRY_VERIFY(page.findChild<QPushButton *>("accountDestinationTag")->isVisible());
    QTimer::singleShot(0, &page, [&] {
        auto *dialog = page.findChild<QDialog *>("accountDialog"); QVERIFY(dialog);
        QCOMPARE(dialog->findChild<QComboBox *>("accountDestination")->currentData().toString(), QString("mac"));
        QVERIFY(dialog->findChild<QRadioButton *>("copyExistingSignIn")->isChecked());
        dialog->accept();
    });
    auto *destination = page.findChild<QPushButton *>("accountDestinationTag"); QVERIFY(destination->isVisible()); destination->click();
    QTRY_VERIFY(QFile::exists(m_dir.filePath("action")));
    QFile action(m_dir.filePath("action")); QVERIFY(action.open(QIODevice::ReadOnly));
    const auto call = QJsonDocument::fromJson(action.readAll()).object(); const auto args = call["args"].toArray();
    QCOMPARE(call["host"].toString(), QString());
    QCOMPARE(args[1].toString(), QString("copy")); QCOMPARE(args[2].toString(), QString("native-codex"));
    QCOMPARE(args[4].toString(), QString("@local")); QCOMPARE(args[6].toString(), QString("mac"));
    QTRY_VERIFY(page.findChild<QPushButton *>("refreshAccounts")->isEnabled());
    page.showAccount({}, "native-claude");
    QTRY_COMPARE(page.findChildren<QPushButton *>("accountDestinationTag").size(), 1);
    QTRY_VERIFY(page.findChild<QPushButton *>("accountDestinationTag")->isVisible());
    QTimer::singleShot(0, &page, [&] {
        auto *dialog = page.findChild<QDialog *>("accountDialog"); QVERIFY(dialog);
        QVERIFY(!dialog->findChild<QRadioButton *>("copyExistingSignIn")->isEnabled());
        QVERIFY(dialog->findChild<QRadioButton *>("signInOnDestination")->isChecked()); dialog->reject();
    });
    page.findChild<QPushButton *>("accountDestinationTag")->click();
}

void TestAccountsPage::missingAgentInstallsOnSelectedMachine()
{
    QFile missing(m_dir.filePath("missing-agent")); QVERIFY(missing.open(QIODevice::WriteOnly)); missing.close();
    AccountsPage page(FakeCli::program(m_script)); page.setFleet(fleet()); page.show();
    auto *list = page.findChild<QListWidget *>("accountProfiles"); QTRY_COMPARE(list->count(), 8);
    page.showAccount("mac", "native-codex");
    auto *install = page.findChild<QPushButton *>("installAccountAgent");
    QTRY_VERIFY(install->isVisible() && install->isEnabled());
    QVERIFY(!page.findChild<QPushButton *>("loginAccount")->isEnabled());
    QSignalSpy requested(&page, &AccountsPage::installRequested); install->click();
    QCOMPARE(requested.takeFirst(), QVariantList({QString("mac"), QString("codex")}));
}

void TestAccountsPage::deepSeekKeyStaysOnSelectedMachine()
{
    QFile dsh(m_dir.filePath("deepseek")); QVERIFY(dsh.open(QIODevice::WriteOnly)); dsh.close();
    QFile removed(m_dir.filePath("removed-mac")); QVERIFY(removed.open(QIODevice::WriteOnly)); removed.write("native-dsh"); removed.close();
    AccountsPage page(FakeCli::program(m_script)); page.setFleet(fleet()); page.show();
    auto *list = page.findChild<QListWidget *>("accountProfiles"); QTRY_COMPARE(list->count(), 9);
    page.showAccount({}, "native-dsh");
    QTRY_VERIFY(page.findChild<QPushButton *>("refreshAccounts")->isEnabled());
    QSignalSpy login(&page, &AccountsPage::loginRequested), install(&page, &AccountsPage::installRequested);
    QTimer::singleShot(0, &page, [&] {
        auto *dialog = page.findChild<QDialog *>("deepSeekKeyDialog"); QVERIFY(dialog);
        dialog->findChild<QLineEdit *>("deepSeekApiKey")->setText("sk-cancelled-key"); dialog->reject();
    });
    page.findChild<QPushButton *>("accountDestinationTag")->click();
    QVERIFY(QFile::exists(m_dir.filePath("removed-mac"))); QVERIFY(!QFile::exists(m_dir.filePath("key-mac")));
    auto enterKey = [&] {
        QTimer::singleShot(0, &page, [&] {
            auto *dialog = page.findChild<QDialog *>("deepSeekKeyDialog"); QVERIFY(dialog);
            QVERIFY(dialog->windowTitle().contains("mac"));
            auto *key = dialog->findChild<QLineEdit *>("deepSeekApiKey"); QCOMPARE(key->echoMode(), QLineEdit::Password);
            auto *save = dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Save);
            QVERIFY(!save->isEnabled()); key->setText("sk-temporary-ui-test-key"); save->click();
        });
        page.findChild<QPushButton *>("accountDestinationTag")->click();
    };
    enterKey();
    QCOMPARE(login.size(), 0); QCOMPARE(install.size(), 0);
    QTRY_COMPARE(list->count(), 10); QVERIFY(!QFile::exists(m_dir.filePath("removed-mac")));
    QFile saved(m_dir.filePath("key-mac")); QVERIFY(saved.open(QIODevice::ReadOnly));
    const auto call = QJsonDocument::fromJson(saved.readAll()).object();
    QVERIFY(!QJsonDocument(call["args"].toArray()).toJson().contains("sk-temporary"));
    QCOMPARE(call["input"].toObject()["api_key"].toString(), QString("sk-temporary-ui-test-key"));
    QVERIFY(!QFile::exists(m_dir.filePath("key-local")));
    QTRY_COMPARE(page.findChild<QPushButton *>("loginAccount")->text(), QString("Set API key on mac…"));
    page.showAccount({}, "native-dsh"); enterKey(); QCOMPARE(login.size(), 0); QCOMPARE(install.size(), 0);
    // A truly missing agent follows the installation path instead.
    QFile missing(m_dir.filePath("missing-dsh")); QVERIFY(missing.open(QIODevice::WriteOnly)); missing.close();
    QTRY_VERIFY(page.findChild<QPushButton *>("refreshAccounts")->isEnabled()); page.reload();
    QTRY_VERIFY(page.findChild<QPushButton *>("refreshAccounts")->isEnabled());
    page.showAccount({}, "native-dsh"); page.findChild<QPushButton *>("accountDestinationTag")->click(); QTRY_COMPARE(install.size(), 1);
    QCOMPARE(install.first(), QVariantList({QString("mac"), QString("dsh")})); QCOMPARE(login.size(), 0);
}

void TestAccountsPage::initialLoadingPublishesOnlyGroupedAccounts()
{
    QFile shared(m_dir.filePath("shared-identity")); QVERIFY(shared.open(QIODevice::WriteOnly)); shared.close();
    QFile hold(m_dir.filePath("hold-inspect")); QVERIFY(hold.open(QIODevice::WriteOnly)); hold.close();
    QFile catalogHold(m_dir.filePath("hold-remote-catalog")); QVERIFY(catalogHold.open(QIODevice::WriteOnly)); catalogHold.close();
    AccountsPage page(FakeCli::program(m_script));page.setFleet(fleet());page.resize(1000,700);page.show();page.showAccount("mac","native-codex");
    auto *list=page.findChild<QListWidget *>("accountProfiles");auto *loading=page.findChild<QWidget *>("accountsLoading");
    auto *content=page.findChild<QWidget *>("accountsContent");auto *refresh=page.findChild<QPushButton *>("refreshAccounts");
    QVERIFY(loading->isVisible());QVERIFY(!content->isVisible());QCOMPARE(list->count(),0);
    // Queue local inspections first. Holding both remote inspector slots before
    // local catalog arrival would prevent the intermediate local-only snapshot.
    QTRY_COMPARE(page.profiles().size(),4);
    QVERIFY(QFile::remove(catalogHold.fileName()));
    QTRY_COMPARE(page.profiles().size(),8);QTest::qWait(100);
    QCOMPARE(list->count(),0);QVERIFY(loading->isVisible());QVERIFY(refresh->property("refreshing").toBool());
    int maximumRows=0;connect(list->model(),&QAbstractItemModel::rowsInserted,&page,[&]{maximumRows=qMax(maximumRows,list->count());});
    const auto preview=qEnvironmentVariable("HGS_ACCOUNTS_PREVIEW");
    if(!preview.isEmpty()){QDir().mkpath(preview);page.setTheme(true);QVERIFY(page.grab().save(preview+"/loading-dark.png"));page.setTheme(false);QVERIFY(page.grab().save(preview+"/loading-light.png"));}
    QFile remoteHold(m_dir.filePath("hold-remote-inspect"));QVERIFY(remoteHold.open(QIODevice::WriteOnly));remoteHold.close();
    QVERIFY(QFile::remove(hold.fileName()));QTRY_COMPARE(list->count(),4);
    QVERIFY(content->isVisible());QVERIFY(!loading->isVisible());QVERIFY(refresh->property("refreshing").toBool());
    for(int i=0;i<list->count();++i)for(const auto &member:list->item(i)->data(Qt::UserRole).toJsonObject()["members"].toArray())QCOMPARE(member.toObject()["host"].toString(),QString());
    const QPersistentModelIndex localRow(list->model()->index(0,0));
    QVERIFY(QFile::remove(remoteHold.fileName()));QTRY_COMPARE(list->count(),7);QTRY_VERIFY(refresh->isEnabled());
    QVERIFY(localRow.isValid());
    QVERIFY(!loading->isVisible());QVERIFY(content->isVisible());QCOMPARE(maximumRows,7);
    QCOMPARE(list->currentItem()->data(Qt::UserRole).toJsonObject()["members"].toArray().size(),2);
    QVERIFY(page.findChild<QPushButton *>("loginAccount")->text().contains("on mac"));
    // Returning to an already loaded page shows the existing snapshot immediately.
    page.hide();page.show();QVERIFY(content->isVisible());QVERIFY(!loading->isVisible());QCOMPARE(list->count(),7);
}
void TestAccountsPage::loadingFinishesWithPartialOrUnavailableResults_data()
{
    QTest::addColumn<QString>("failure");QTest::addColumn<int>("expected");
    QTest::newRow("identity-failure")<<QString("fail-inspect")<<8;
    QTest::newRow("catalog-failure")<<QString("fail-catalog")<<4;
    QTest::newRow("empty-catalogs")<<QString("empty-catalog")<<0;
    QTest::newRow("offline-peer")<<QString("offline")<<8;
}
void TestAccountsPage::loadingFinishesWithPartialOrUnavailableResults()
{
    QFETCH(QString,failure);QFETCH(int,expected);
    QFile marker(m_dir.filePath(failure));QVERIFY(marker.open(QIODevice::WriteOnly));marker.close();
    auto machines=fleet();if(failure=="offline"){BoxState mac;mac.host="mac";mac.ok=false;machines.setPeer(mac,QDateTime::currentMSecsSinceEpoch());}
    AccountsPage page(FakeCli::program(m_script));page.setFleet(machines);page.show();
    auto *content=page.findChild<QWidget *>("accountsContent");auto *refresh=page.findChild<QPushButton *>("refreshAccounts");
    QTRY_VERIFY(content->isVisible());QTRY_VERIFY(refresh->isEnabled());
    QCOMPARE(page.findChild<QListWidget *>("accountProfiles")->count(),expected);
    QVERIFY(!page.findChild<QWidget *>("accountsLoading")->isVisible());
}

void TestAccountsPage::refreshPreservesRowsAndShowsPendingUsage()
{
    QFile shared(m_dir.filePath("shared-identity")); QVERIFY(shared.open(QIODevice::WriteOnly)); shared.close();
    AccountsPage page(FakeCli::program(m_script)); page.setFleet(fleet()); page.show();
    auto *list = page.findChild<QListWidget *>("accountProfiles"); auto *refresh = page.findChild<QPushButton *>("refreshAccounts");
    QTRY_COMPARE(list->count(), 7); QTRY_VERIFY(refresh->isEnabled());
    page.showAccount("mac", "native-codex");
    QList<QPersistentModelIndex> rows; for (int i = 0; i < list->count(); ++i) rows << QPersistentModelIndex(list->model()->index(i, 0));
    const QPersistentModelIndex selection(list->currentIndex());
    QSignalSpy reset(list->model(), &QAbstractItemModel::modelReset), inserted(list->model(), &QAbstractItemModel::rowsInserted), removed(list->model(), &QAbstractItemModel::rowsRemoved);
    QFile slow(m_dir.filePath("slow-inspect")); QVERIFY(slow.open(QIODevice::WriteOnly)); slow.close();
    refresh->click();
    QVERIFY(refresh->property("refreshing").toBool()); QVERIFY(!refresh->isEnabled());
    // Catalog replies finish first, while queued account inspections keep updating.
    QTest::qWait(80); QVERIFY(refresh->property("refreshing").toBool());
    QVERIFY(refresh->toolTip().contains("remaining"));
    QTRY_VERIFY(refresh->isEnabled()); QVERIFY(!refresh->property("refreshing").toBool());
    QCOMPARE(reset.size(), 0); QCOMPARE(inserted.size(), 0); QCOMPARE(removed.size(), 0);
    QCOMPARE(QPersistentModelIndex(list->currentIndex()), selection);
    for (int i = 0; i < rows.size(); ++i) { QVERIFY(rows[i].isValid()); QCOMPARE(rows[i].row(), i); }
    QCOMPARE(page.findChild<QPushButton *>("loginAccount")->text(), QString("Sign in again on mac…"));
}

void TestAccountsPage::preview()
{
    const QString dir = qEnvironmentVariable("HGS_ACCOUNTS_PREVIEW"); if (dir.isEmpty()) QSKIP("Set HGS_ACCOUNTS_PREVIEW for screenshots");
    QFile shared(m_dir.filePath("shared-identity")); QVERIFY(shared.open(QIODevice::WriteOnly)); shared.close();
    QFile modes(m_dir.filePath("permission-modes")); QVERIFY(modes.open(QIODevice::WriteOnly)); modes.close();
    QDir().mkpath(dir); AccountsPage page(FakeCli::program(m_script)); page.setFleet(fleet()); page.resize(1080, 700);
    page.setStyleSheet("QWidget { background:#171e24; color:#e4e9ee; font-size:14px; } QLabel#heading {font-size:20px;} QLabel#muted {color:#9babb8;} QPushButton,QComboBox {padding:9px 13px;border:1px solid #34414b;border-radius:6px;background:#1d262e;} QPushButton:disabled {color:#75808a;} QListWidget {border:1px solid #34414b;border-radius:8px;background:#1d262e;} QListWidget::item {padding:8px;} QListWidget::item:selected {background:#21453c;} ");
    QFile hold(m_dir.filePath("hold-inspect")); QVERIFY(hold.open(QIODevice::WriteOnly)); hold.close();
    page.setTheme(true); page.show(); QTRY_COMPARE(page.profiles().size(),8); QTest::qWait(120);
    QVERIFY(page.grab().save(dir+"/accounts-loading.png"));
    QVERIFY(QFile::remove(hold.fileName()));
    QTRY_COMPARE(page.findChild<QListWidget *>("accountProfiles")->count(), 7); page.showAccount({}, "native-codex");
    QTRY_VERIFY(page.findChild<QLabel *>("accountTitle")->text().contains("native-codex@example.test"));QTest::qWait(30);
    QVERIFY(page.grab().save(dir+"/accounts.png"));
    page.resize(840,700);QTest::qWait(30);
    auto *scroll=page.findChild<QScrollArea *>("accountDetailsScroll");QVERIFY(scroll);
    QCOMPARE(scroll->horizontalScrollBar()->maximum(),0);
    QVERIFY(page.grab().save(dir+"/accounts-narrow.png"));
    page.showAccount({},"native-claude");QTest::qWait(30);
    auto *machine=page.findChild<QPushButton *>("accountMachineTag");
    auto *destination=page.findChild<QPushButton *>("accountDestinationTag");QVERIFY(machine);QVERIFY(destination);
    QCOMPARE(machine->height(),destination->height());
    auto *permissions=page.findChild<QLabel *>("accountPermissionState");auto *change=page.findChild<QPushButton *>("accountPermissions");
    const int gap=change->mapTo(&page,QPoint()).x()-permissions->mapTo(&page,QPoint(permissions->width(),0)).x();
    QVERIFY(gap>=0&&gap<=16);QCOMPARE(scroll->horizontalScrollBar()->maximum(),0);
    QVERIFY(page.grab().save(dir+"/accounts-add-machine.png"));
    page.showAccount({},"native-codex");QTest::qWait(30);
    auto *refresh=page.findChild<QPushButton *>("refreshAccounts"); QTRY_VERIFY(refresh->isEnabled());
    QVERIFY(hold.open(QIODevice::WriteOnly)); hold.close(); refresh->click(); QTest::qWait(80);
    QVERIFY(page.grab().save(dir+"/accounts-updating.png")); QTest::qWait(160);
    QVERIFY(page.grab().save(dir+"/accounts-updating-next-frame.png"));
    page.setStyleSheet("QWidget {font-size:14px;} QPushButton,QComboBox {padding:9px 13px;} ");page.setTheme(false);QTest::qWait(30);
    QVERIFY(page.grab().save(dir+"/accounts-updating-light.png"));
    QVERIFY(QFile::remove(hold.fileName())); QTRY_VERIFY(refresh->isEnabled());
    QVERIFY(page.grab().save(dir+"/accounts-light.png"));
    NewSessionDialog dialog(FakeCli::program(m_script), fleet(), "mac", "codex", "", &page); dialog.show(); QTRY_COMPARE(dialog.findChild<QListWidget *>("launchAccount")->count(), 6);
    dialog.findChild<QListWidget *>("launchAccount")->setCurrentRow(1); QVERIFY(dialog.grab().save(dir+"/new-session-account.png"));
}
QTEST_MAIN(TestAccountsPage)
#include "test_accountspage.moc"
