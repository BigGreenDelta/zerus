#include <QtTest>
#include <QTemporaryDir>
#include <QFile>
#include <QSettings>
#include <QComboBox>
#include <QLineEdit>
#include <QListWidget>
#include <QTimer>
#include <QCheckBox>
#include <QMenu>
#include <QMessageBox>
#include <QPointer>
#include "FakeCli.h"
#include "WorktreePanel.h"
#include "NewSessionDialog.h"
#include "WorktreesDialog.h"
#include <QPlainTextEdit>

static int launchAccountRow(const QListWidget *list,const QString &id) {
    for(int row=0;row<list->count();++row)if(list->item(row)->data(Qt::UserRole).toString()==id)return row;
    return -1;
}
static QString selectedLaunchAccount(const QListWidget *list) {
    return list->currentItem()?list->currentItem()->data(Qt::UserRole).toString():QString();
}

class TestWorktreePanel:public QObject {
    Q_OBJECT
    QTemporaryDir dir;
    QString script() const {return dir.filePath("hgs");}
    QJsonObject catalog() const {
        return {{"state","ok"},{"machine","fixture"},{"path","/repo"},{"selected_root","/repo"},{"common_dir","/repo/.git"},{"sampled_at",QDateTime::currentSecsSinceEpoch()},
            {"worktrees",QJsonArray{QJsonObject{{"path","/repo"},{"kind","main"},{"branch","main"},{"available",true}},QJsonObject{{"path","/linked"},{"kind","linked"},{"branch","feature"},{"available",true}},QJsonObject{{"path","/missing"},{"kind","linked"},{"prunable",true},{"available",false}},QJsonObject{{"path","/bare"},{"kind","bare"},{"available",true}}}}};
    }
    QJsonObject review() const {
        const auto row=[](const QString &path,const QString &verdict,const QJsonObject &extra){
            QJsonObject result{{"path",path},{"kind","linked"},{"branch",QFileInfo(path).fileName()},{"head","0123456789abcdef"},{"available",verdict!="missing"},
                {"verdict",verdict},{"fingerprint",path+"-print"},{"reasons",QJsonArray{}},{"notes",QJsonArray{}}};
            for(auto i=extra.begin();i!=extra.end();++i)result[i.key()]=i.value();return result;
        };
        const auto message=[](const QString &code,const QString &text){return QJsonArray{QJsonObject{{"code",code},{"message",text}}};};
        return {{"state","ok"},{"machine","fixture"},{"common_dir","/repo/.git"},{"base",QJsonArray{"main"}},{"sampled_at",QDateTime::currentSecsSinceEpoch()},{"partial",false},
            {"worktrees",QJsonArray{row("/repo","blocked",{{"kind","main"},{"reasons",message("main_checkout","This is the main checkout of the repository.")}}),
                row("/linked","ready",{{"merged",true},{"ignored_bytes",2048},{"ignored",QJsonArray{QJsonObject{{"path","target"},{"bytes",2048},{"complete",true}}}}}),
                row("/review","review",{{"merged",false},{"notes",message("not_merged","1 commit not in main. Removing the worktree keeps the branch.")}}),
                row("/blocked","blocked",{{"reasons",message("changes","2 changed files not committed.")}}),
                row("/missing","missing",{{"merged",true}})}}};
    }
    QByteArray calls() const {QFile file(dir.filePath("calls"));return file.open(QIODevice::ReadOnly)?file.readAll():QByteArray();}
    FleetState fleet() const {
        BoxState local;local.ok=true;local.host="arch";local.metrics["platform"]="linux";SessionInfo a;a.name="codex/repo/one";a.gitRoot="/repo";a.canonicalCwd="/repo/subfolder";local.sessions={a};
        BoxState remote=local;remote.host="mac";remote.sessions[0].name="kimi/repo/two";
        FleetState result;result.setLocal(local,QDateTime::currentMSecsSinceEpoch());result.setPeer(remote,QDateTime::currentMSecsSinceEpoch());return result;
    }
private slots:
    void initTestCase(){
        QCoreApplication::setOrganizationName("hgs-tests");QCoreApplication::setApplicationName("worktree-panel");QSettings::setDefaultFormat(QSettings::IniFormat);QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,dir.path());
        QFile response(dir.filePath("catalog.json"));QVERIFY(response.open(QIODevice::WriteOnly));response.write(QJsonDocument(catalog()).toJson());response.close();
        QFile reviewed(dir.filePath("review.json"));QVERIFY(reviewed.open(QIODevice::WriteOnly));reviewed.write(QJsonDocument(review()).toJson());reviewed.close();
        QFile program(script());QVERIFY(program.open(QIODevice::WriteOnly));program.write(R"PY(#!/usr/bin/env python3
import json,os,sys,time
os.chdir(os.path.dirname(os.path.abspath(__file__)))
open('calls','a').write(' '.join(sys.argv[1:])+'\n')
a=sys.argv[1:]
if a and a[0].startswith('@'):a=a[1:]
c=a[0] if a else ''
dump=lambda value:sys.stdout.write(json.dumps(value,separators=(',',':')))
if c=='worktrees':
 if a[1:2]==['create']:
  o=a[2:];v={}
  for i in range(0,len(o)-1,2):v[o[i]]=o[i+1]
  time.sleep(0.05)
  if os.path.exists('fail-create'):print('Branch already exists',file=sys.stderr);sys.exit(1)
  dump({'status':'created','request_id':v.get('--request-id',''),'branch':v.get('--branch',''),'path':v.get('--destination',''),'common_dir':'/repo/.git','machine':'fixture'})
 elif a[1:2]==['review']:
  time.sleep(0.05)
  if os.path.exists('old-hgs'):print('usage: hgs worktrees --path PATH [--refresh] [--json]',file=sys.stderr);sys.exit(1)
  sys.stdout.write(open('review.json').read())
 elif a[1:2]==['remove']:
  o=a[2:];v={};i=0
  while i<len(o):
   if o[i] in ('--request-id','--path') and i+1<len(o):v[o[i]]=o[i+1];i+=2
   elif o[i] in ('--common-dir','--fingerprint'):i+=2
   else:i+=1
  time.sleep(0.05)
  if os.path.exists('fail-remove') and v.get('--path','') in open('fail-remove').read().split('\n'):
   print('Worktree changed since the review. Review it again.',file=sys.stderr);sys.exit(1)
  dump({'status':'removed','request_id':v.get('--request-id',''),'path':v.get('--path',''),'branch_deleted':False})
 else:
  if os.path.exists('fail-catalog'):print('fixture catalog unavailable',file=sys.stderr);sys.exit(1)
  time.sleep(0.05);sys.stdout.write(open('catalog.json').read())
elif c=='account':print('{"profiles":[{"id":"work","provider":"codex","label":"Work"}]}')
elif c=='dirs':dump({'path':a[1] if len(a)>1 else '','directories':[]})
)PY");program.close();QVERIFY(FakeCli::makeRunnable(program.fileName()));
    }
    void init(){
        QSettings().clear();for(const auto *name:{"calls","fail-create","fail-remove","old-hgs","fail-catalog"})QFile::remove(dir.filePath(name));
        QFile response(dir.filePath("catalog.json"));QVERIFY(response.open(QIODevice::WriteOnly));response.write(QJsonDocument(catalog()).toJson());
    }
    void overviewCombinesProjectsButKeepsComputerAndSessionIdentity(){
        SessionOrganization projects; const auto first = projects.createGroup("Shared"), second = projects.createGroup("Shared");
        projects.addFolder(first,"arch","/repo"); projects.addFolder(second,"arch","/repo/subfolder");
        projects.addFolder(second,"mac","/repo");
        HgsClient client(FakeCli::program(script())); auto machines = fleet();
        auto local = machines.local(); SessionInfo linked; linked.name="codex/linked/local"; linked.gitRoot="/linked"; linked.state="paused";
        local.sessions.append(linked); machines.setLocal(local,QDateTime::currentMSecsSinceEpoch());
        WorktreesDialog dialog(&client,projects,machines,false); dialog.show();
        auto *tree = dialog.findChild<QTreeWidget *>("worktreeOverviewList");
        auto *refresh = dialog.findChild<QPushButton *>("refreshWorktreeOverview");
        QTRY_COMPARE(tree->topLevelItemCount(),8); QTRY_VERIFY(refresh->isEnabled());
        QTreeWidgetItem *localLinked=nullptr, *remoteLinked=nullptr;
        for(int i=0;i<tree->topLevelItemCount();++i){auto *item=tree->topLevelItem(i);
            if(item->data(0,Qt::UserRole+1).toJsonObject()["path"]!="/linked")continue;
            if(item->text(2)=="arch")localLinked=item;else if(item->text(2)=="mac")remoteLinked=item;
        }
        QVERIFY(localLinked);QVERIFY(remoteLinked);QCOMPARE(localLinked->text(3),QString("1"));QCOMPARE(remoteLinked->text(3),QString("0"));
        tree->setCurrentItem(localLinked);
        auto *details = dialog.findChild<QPlainTextEdit *>("worktreeOverviewDetails");
        QVERIFY(details->toPlainText().contains("codex/linked/local (paused)"));QVERIFY(!details->toPlainText().contains("kimi/repo/two"));
        dialog.findChild<QPushButton *>("copyOverviewWorktreePath")->click();QCOMPARE(QApplication::clipboard()->text(),QString("/linked"));
        QVERIFY(!calls().contains("worktrees remove"));QVERIFY(!calls().contains("worktrees create"));
        QSignalSpy reset(tree->model(),&QAbstractItemModel::modelReset);dialog.setContext(projects,machines,false);
        QCOMPARE(reset.size(),0);QCOMPARE(tree->currentItem(),localLinked);
        auto *filter=dialog.findChild<QComboBox *>("worktreeProjectFilter");
        QCOMPARE(filter->count(),projects.groups().size()+1);QVERIFY(filter->findData(first)!=filter->findData(second));
        filter->setCurrentIndex(filter->findData(first));QCOMPARE(tree->topLevelItemCount(),4);
        filter->setCurrentIndex(filter->findData(second));QCOMPARE(tree->topLevelItemCount(),8);
        auto *computer=dialog.findChild<QComboBox *>("worktreeComputerFilter");computer->setCurrentIndex(computer->findData("mac"));
        QCOMPARE(tree->topLevelItemCount(),4);for(int i=0;i<4;++i)QCOMPARE(tree->topLevelItem(i)->text(2),QString("mac"));
        for(int i=0;i<4;++i)if(tree->topLevelItem(i)->text(1)=="feature")tree->setCurrentItem(tree->topLevelItem(i));
        QTRY_COMPARE(tree->currentItem()->text(4),QString("Ready"));
        QVERIFY(details->toPlainText().contains("Ignored files removed with the folder"));
        // Geometry and dismissal ordering at the existing narrow Projects width.
        dialog.resize(760,560);QTest::qWait(30);
        auto *buttons=dialog.findChild<QDialogButtonBox *>();auto *close=buttons->button(QDialogButtonBox::Close);
        for(auto *button:buttons->buttons())if(button!=close)QVERIFY(button->geometry().right()<close->geometry().left());
        QVERIFY(tree->width()>600);
        const auto preview=qEnvironmentVariable("HGS_WORKTREE_PREVIEW");if(!preview.isEmpty()){QDir().mkpath(preview);QVERIFY(dialog.grab().save(preview+"/overview.png"));}
    }
    void overviewShowsCleanupReasonsAndOfflineSnapshots(){
        SessionOrganization projects;const auto project=projects.createGroup("Work");projects.addFolder(project,"arch","/repo");
        HgsClient client(FakeCli::program(script()));auto machines=fleet();WorktreesDialog dialog(&client,projects,machines,false);dialog.show();
        auto *tree=dialog.findChild<QTreeWidget *>("worktreeOverviewList");auto *details=dialog.findChild<QPlainTextEdit *>("worktreeOverviewDetails");
        QTRY_COMPARE(tree->topLevelItemCount(),4);QTRY_VERIFY(details->toPlainText().contains("This is the main checkout"));
        QTreeWidgetItem *linked=nullptr;for(int i=0;i<4;++i)if(tree->topLevelItem(i)->text(1)=="feature")linked=tree->topLevelItem(i);
        QVERIFY(linked);tree->setCurrentItem(linked);QCOMPARE(linked->text(4),QString("Ready"));
        QVERIFY(details->toPlainText().contains("Ignored files removed with the folder"));
        auto *cleanup=dialog.findChild<QPushButton *>("cleanUpOverviewRepository");QVERIFY(cleanup->isEnabled());
        const auto before=calls().count("worktrees");auto local=machines.local();local.ok=false;machines.setLocal(local,QDateTime::currentMSecsSinceEpoch());
        dialog.setContext(projects,machines,false);QCOMPARE(tree->topLevelItemCount(),4);QVERIFY(!cleanup->isEnabled());
        QVERIFY(!dialog.findChild<QPushButton *>("removeOverviewWorktree")->isEnabled());QVERIFY(details->toPlainText().contains("Last known"));
        cleanup->click();QTest::qWait(80);QCOMPARE(calls().count("worktrees"),before);
        local.ok=true;machines.setLocal(local,QDateTime::currentMSecsSinceEpoch());dialog.setContext(projects,machines,false);
        QTRY_VERIFY(cleanup->isEnabled());QTRY_VERIFY(calls().count("worktrees")>before);
    }
    void overviewIgnoresObsoleteCatalogWhenProjectAddressChanges(){
        SessionOrganization projects;const auto project=projects.createGroup("Work");const auto folder=projects.addFolder(project,"arch","/repo");
        HgsClient client(FakeCli::program(script()));QSignalSpy ready(&client,&HgsClient::worktreesReady);WorktreesDialog dialog(&client,projects,fleet(),false);dialog.show();
        QVERIFY(projects.editFolder(project,folder,"mac","/repo","Remote"));dialog.setContext(projects,fleet(),false);
        QTRY_COMPARE(ready.size(),2);auto *tree=dialog.findChild<QTreeWidget *>("worktreeOverviewList");QTRY_COMPARE(tree->topLevelItemCount(),4);
        for(int i=0;i<tree->topLevelItemCount();++i)QCOMPARE(tree->topLevelItem(i)->text(2),QString("mac"));
        client.worktreesReady(1,{},"/repo",QJsonObject{{"state","ok"},{"common_dir","/wrong/.git"},{"worktrees",QJsonArray{QJsonObject{{"path","/wrong"},{"kind","linked"}}}}});
        QCOMPARE(tree->topLevelItemCount(),4);QVERIFY(!dialog.findChild<QPlainTextEdit *>("worktreeOverviewDetails")->toPlainText().contains("/wrong"));
        QVERIFY(!calls().contains("worktrees remove"));
    }
    void overviewFailedCatalogKeepsSnapshotAndRequiresSuccessfulRefresh(){
        HgsClient client(FakeCli::program(script()));QSignalSpy loaded(&client,&HgsClient::worktreesReady);
        client.requestWorktrees({},"/repo");QTRY_COMPARE(loaded.size(),1);
        QFile fail(dir.filePath("fail-catalog"));QVERIFY(fail.open(QIODevice::WriteOnly));fail.close();
        SessionOrganization projects;const auto project=projects.createGroup("Work");projects.addFolder(project,"arch","/repo");
        WorktreesDialog dialog(&client,projects,fleet(),false);dialog.show();auto *details=dialog.findChild<QPlainTextEdit *>("worktreeOverviewDetails");
        auto *tree=dialog.findChild<QTreeWidget *>("worktreeOverviewList");QTRY_VERIFY(details->toPlainText().contains("fixture catalog unavailable"));
        QCOMPARE(tree->topLevelItemCount(),4);auto *cleanup=dialog.findChild<QPushButton *>("cleanUpOverviewRepository");QVERIFY(!cleanup->isEnabled());
        QVERIFY(!calls().contains("worktrees review"));QVERIFY(!calls().contains("worktrees remove"));
        QVERIFY(QFile::remove(fail.fileName()));dialog.findChild<QPushButton *>("refreshWorktreeOverview")->click();QTRY_VERIFY(cleanup->isEnabled());
    }
    void overviewOrdinaryFoldersNeverOfferCleanup(){
        QFile response(dir.filePath("catalog.json"));QVERIFY(response.open(QIODevice::WriteOnly));
        response.write(QJsonDocument(QJsonObject{{"state","not_repo"},{"path","/plain"},{"worktrees",QJsonArray{}}}).toJson());response.close();
        SessionOrganization projects;const auto project=projects.createGroup("Work");projects.addFolder(project,"arch","/plain");
        HgsClient client(FakeCli::program(script()));WorktreesDialog dialog(&client,projects,fleet(),false);dialog.show();
        auto *tree=dialog.findChild<QTreeWidget *>("worktreeOverviewList");QTRY_COMPARE(tree->topLevelItem(0)->text(4),QString("Ordinary folder"));
        QVERIFY(!dialog.findChild<QPushButton *>("cleanUpOverviewRepository")->isEnabled());
        QVERIFY(!dialog.findChild<QPushButton *>("removeOverviewWorktree")->isEnabled());
        QVERIFY(!calls().contains("worktrees review"));QVERIFY(!calls().contains("worktrees create"));QVERIFY(!calls().contains("worktrees remove"));
    }
    void overviewCleanupKeepsConfirmationAndExactRemoteTarget_data(){
        QTest::addColumn<bool>("confirmed");QTest::newRow("cancel")<<false;QTest::newRow("confirm")<<true;
    }
    void overviewCleanupKeepsConfirmationAndExactRemoteTarget(){
        QFETCH(bool,confirmed);
        SessionOrganization projects;const auto project=projects.createGroup("Work");projects.addFolder(project,"mac","/repo");
        HgsClient client(FakeCli::program(script()));WorktreesDialog dialog(&client,projects,fleet(),false);dialog.show();
        auto *tree=dialog.findChild<QTreeWidget *>("worktreeOverviewList");QTRY_COMPARE(tree->topLevelItemCount(),4);
        auto *remove=dialog.findChild<QPushButton *>("removeOverviewWorktree");
        for(int i=0;i<4;++i)if(tree->topLevelItem(i)->text(1)=="feature")tree->setCurrentItem(tree->topLevelItem(i));
        QTRY_VERIFY(remove->isEnabled());bool reviewed=false;
        QTimer::singleShot(0,&dialog,[&]{
            auto *reviewDialog=dialog.findChild<QDialog *>("worktreeCleanupDialog");QVERIFY(reviewDialog);QTimer::singleShot(5000,reviewDialog,[reviewDialog]{reviewDialog->done(0);});
            auto *list=reviewDialog->findChild<QTreeWidget *>("worktreeCleanupList");QTRY_COMPARE(list->topLevelItemCount(),1);
            QCOMPARE(list->topLevelItem(0)->data(0,Qt::UserRole).toString(),QString("/linked"));
            QVERIFY(calls().contains("@mac worktrees review --path /repo/.git --json --worktree /linked"));
            const auto preview=qEnvironmentVariable("HGS_WORKTREE_PREVIEW");if(!preview.isEmpty()){QDir().mkpath(preview);QVERIFY(reviewDialog->grab().save(preview+"/cleanup-preview.png"));}
            QTimer::singleShot(0,reviewDialog,[reviewDialog,confirmed]{auto *confirmation=reviewDialog->findChild<QMessageBox *>("confirmWorktreeCleanup");QVERIFY(confirmation);QCOMPARE(confirmation->defaultButton(),confirmation->button(QMessageBox::Cancel));
                if(confirmed)confirmation->findChild<QPushButton *>("confirmRemoveWorktrees")->click();else confirmation->reject();});
            reviewDialog->findChild<QPushButton *>("removeSelectedWorktrees")->click();
            if(confirmed){QTRY_VERIFY(reviewDialog->findChild<QLabel *>("worktreeCleanupStatus")->text().contains("Removed 1 worktree."));
                QCOMPARE(calls().count("worktrees remove"),1);QVERIFY(calls().contains("@mac worktrees remove --path /linked --common-dir /repo/.git --fingerprint /linked-print"));
                QVERIFY(!calls().contains("--force"));}
            else QVERIFY(!calls().contains("worktrees remove"));
            reviewed=true;reviewDialog->reject();
        });remove->click();QVERIFY(reviewed);QCOMPARE(dialog.changed(),confirmed);
        if(confirmed)QTRY_VERIFY(calls().count("@mac worktrees --path /repo --json --refresh")>=2);
    }
    void clientCachesCoalescesAndScopesSnapshots(){
        HgsClient client(FakeCli::program(script()));QSignalSpy ready(&client,&HgsClient::worktreesReady);
        const auto first=client.requestWorktrees({},"/repo"),second=client.requestWorktrees({},"/repo");QVERIFY(first!=second);QTRY_COMPARE(ready.size(),2);
        QFile calls(dir.filePath("calls"));QVERIFY(calls.open(QIODevice::ReadOnly));QCOMPARE(calls.readAll().count("worktrees"),1);calls.close();
        QVERIFY(client.worktreeSnapshot("mac","/repo").isEmpty());QCOMPARE(client.worktreeSnapshot({},"/linked")["selected_root"].toString(),QString("/linked"));
        client.requestWorktrees({},"/linked");QTRY_COMPARE(ready.size(),3);QVERIFY(calls.open(QIODevice::ReadOnly));QCOMPARE(calls.readAll().count("worktrees"),1);calls.close();
        client.requestWorktrees("mac","/repo");QTRY_COMPARE(ready.size(),4);QVERIFY(!client.worktreeSnapshot("mac","/repo").isEmpty());
        client.requestWorktrees({},"/repo",true);QTRY_COMPARE(ready.size(),5);
    }
    void catalogGuardsUnavailableFoldersAndKeepsOfflineSnapshot(){
        HgsClient client(FakeCli::program(script()));WorktreePanel panel(&client);auto state=fleet();panel.setContext({},"/repo",state);panel.resize(580,400);panel.show();
        auto *tree=panel.findChild<QTreeWidget *>("worktreeCatalog");QTRY_COMPARE(tree->topLevelItemCount(),4);
        QCOMPARE(tree->topLevelItem(0)->childCount(),1);QVERIFY(tree->topLevelItem(0)->child(0)->text(0).contains("codex/repo/one"));
        auto *use=panel.findChild<QPushButton *>("useWorktreeFolder");tree->setCurrentItem(tree->topLevelItem(2));QVERIFY(!use->isEnabled());tree->setCurrentItem(tree->topLevelItem(3));QVERIFY(!use->isEnabled());
        tree->setCurrentItem(tree->topLevelItem(1));QVERIFY(use->isEnabled());QString chosen;panel.launchRequested=[&](const QString &path){chosen=path;};use->click();QCOMPARE(chosen,QString("/linked"));
        auto local=state.local();local.ok=false;state.setLocal(local,QDateTime::currentMSecsSinceEpoch());panel.setContext({},"/repo",state);QCOMPARE(tree->topLevelItemCount(),4);QVERIFY(!use->isEnabled());QVERIFY(panel.findChild<QLabel *>("worktreeStatus")->text().contains("Offline"));
        QVERIFY(tree->topLevelItem(0)->child(0)->text(0).contains("last recorded"));
        const auto preview=qEnvironmentVariable("HGS_WORKTREE_PREVIEW");if(!preview.isEmpty()){QDir().mkpath(preview);QVERIFY(panel.grab().save(preview+"/offline.png"));}
    }
    void localWindowsSessionsMatchTheirFolderInAnyCase(){
        // Offline, the panel lists the folder itself with the sessions last seen in it. The
        // local Windows machine compares paths in any case and with either separator.
        HgsClient client(FakeCli::program(script()));
        for(const bool windows:{true,false}){
            BoxState local;local.host="desktop";local.metrics["platform"]=windows?"windows":"linux";
            SessionInfo one;one.name="codex/repo/one";one.canonicalCwd=windows?"c:\\work\\repo":"/work/Repo";
            SessionInfo two=one;two.name="codex/repo/two";two.canonicalCwd=windows?"C:\\Work\\Repo\\sub":"/work/repo";local.sessions={one,two};
            FleetState state;state.setLocal(local,QDateTime::currentMSecsSinceEpoch());
            WorktreePanel panel(&client);panel.setContext({},windows?"C:/Work/Repo":"/work/repo",state);
            auto *tree=panel.findChild<QTreeWidget *>("worktreeCatalog");QCOMPARE(tree->topLevelItemCount(),1);
            QCOMPARE(tree->topLevelItem(0)->childCount(),1);
            QCOMPARE(tree->topLevelItem(0)->child(0)->data(0,Qt::UserRole).toString(),QString(windows?"codex/repo/one":"codex/repo/two"));
        }
        QVERIFY(calls().isEmpty());
    }
    void lateResponseCannotReplaceAnotherMachineOrFolder(){
        HgsClient client(FakeCli::program(script()));WorktreePanel panel(&client);auto state=fleet();panel.show();panel.setContext({},"/repo",state);panel.setContext("mac","/repo",state);
        client.worktreesReady(1,{},"/repo",QJsonObject{{"state","not_repo"},{"path","/wrong"},{"worktrees",QJsonArray{}}});
        auto *tree=panel.findChild<QTreeWidget *>("worktreeCatalog");QTRY_COMPARE(tree->topLevelItemCount(),4);QCOMPARE(tree->topLevelItem(0)->childCount(),1);QVERIFY(tree->topLevelItem(0)->child(0)->text(0).contains("kimi/repo/two"));
        panel.setContext("mac","/plain",state);client.worktreesReady(2,"mac","/repo",catalog());QVERIFY(!panel.findChild<QLabel *>("worktreeStatus")->text().contains("/wrong"));
    }
    void pickerPreservesProjectMachineAccountAndCustomFolderAcrossPolls(){
        auto state=fleet();SessionOrganization org;const auto project=org.createGroup("Work");const auto id=org.addFolder(project,"mac","/repo");
        NewSessionDialog dialog(FakeCli::program(script()),state,"mac","codex",project);dialog.setGroups(org,project);QVERIFY(dialog.selectFolder(id));dialog.show();
        auto *account=dialog.findChild<QListWidget *>("launchAccount");QTRY_VERIFY(launchAccountRow(account,"work")>=0);account->setCurrentRow(launchAccountRow(account,"work"));
        auto *choose=dialog.findChild<QAction *>("chooseLaunchWorktree");QVERIFY(choose);QTRY_VERIFY(choose->isVisible());
        QTimer::singleShot(0,&dialog,[&]{auto *picker=dialog.findChild<QDialog *>("chooseWorktreeDialog");QVERIFY(picker);QTimer::singleShot(3000,picker,&QDialog::reject);auto *tree=picker->findChild<QTreeWidget *>("worktreeCatalog");QTRY_COMPARE(tree->topLevelItemCount(),4);tree->setCurrentItem(tree->topLevelItem(1));auto *use=picker->findChild<QPushButton *>("useWorktreeFolder");QTRY_VERIFY(use->isEnabled());use->click();});choose->trigger();
        QCOMPARE(dialog.findChild<QLabel *>("launchFolderPath")->text(),QString("/linked"));dialog.setFleet(state);
        auto *hint=dialog.findChild<QLabel *>("muted");QVERIFY(!hint->property("outsideProject").toBool());QVERIFY(hint->text().isEmpty());QVERIFY(hint->styleSheet().isEmpty());
        QCOMPARE(dialog.findChild<QComboBox *>("launchProject")->currentData().toString(),project);QCOMPARE(dialog.findChild<QComboBox *>("launchComputer")->currentData().toString(),QString("mac"));QCOMPARE(selectedLaunchAccount(account),QString("work"));QCOMPARE(dialog.findChild<QLabel *>("launchFolderPath")->text(),QString("/linked"));
        QCOMPARE(org.group(project)->folders.size(),1);QSignalSpy launched(&dialog,&NewSessionDialog::launchRequested);dialog.findChild<QPushButton *>("primary")->click();QTRY_COMPARE(launched.size(),1);QCOMPARE(launched.first()[2].toString(),QString("/linked"));QCOMPARE(launched.first()[4].toString(),QString("work"));QCOMPARE(org.group(project)->folders.size(),1);
    }
    void accountPresetWaitsForCatalogAndNeverFallsBackSilently(){
        auto state=fleet();SessionOrganization org;const auto project=org.createGroup("Work");org.addFolder(project,"mac","/repo");
        NewSessionDialog dialog(FakeCli::program(script()),state,"mac","codex",project);dialog.setGroups(org,project);dialog.selectAccount("work");dialog.show();
        auto *account=dialog.findChild<QListWidget *>("launchAccount");auto *start=dialog.findChild<QPushButton *>("primary");
        QVERIFY(!start->isEnabled());QTRY_COMPARE(selectedLaunchAccount(account),QString("work"));QTRY_VERIFY(start->isEnabled());
        dialog.setFleet(state);QCOMPARE(selectedLaunchAccount(account),QString("work"));
        dialog.selectAccount("removed");QVERIFY(!start->isEnabled());QCOMPARE(account->currentRow(),-1);
        QVERIFY(dialog.findChild<QLabel *>("launchError")->text().contains("unavailable"));
        dialog.setFleet(state);QCOMPARE(account->currentRow(),-1);
        // This catalog contains only "work". A missing native profile must not
        // silently fall back to another account, just like any other missing ID.
        dialog.selectAccount("native-codex");QCOMPARE(account->currentRow(),-1);QVERIFY(!start->isEnabled());
        dialog.selectAccount("work");QCOMPARE(selectedLaunchAccount(account),QString("work"));QVERIFY(start->isEnabled());
    }
    void newWorktreeCreatesExplicitlyAndPreservesLaunchContext(){
        auto state=fleet();SessionOrganization org;const auto project=org.createGroup("Work");const auto id=org.addFolder(project,"mac","/repo");
        NewSessionDialog dialog(FakeCli::program(script()),state,"mac","codex",project);dialog.setGroups(org,project);QVERIFY(dialog.selectFolder(id));dialog.show();
        auto *account=dialog.findChild<QListWidget *>("launchAccount");QTRY_VERIFY(launchAccountRow(account,"work")>=0);account->setCurrentRow(launchAccountRow(account,"work"));
        auto *create=dialog.findChild<QAction *>("newLaunchWorktree");QVERIFY(create);QTRY_VERIFY(create->isVisible()&&create->isEnabled());
        QTimer::singleShot(0,&dialog,[&]{auto *form=dialog.findChild<QDialog *>("newWorktreeDialog");QVERIFY(form);form->reject();});create->trigger();
        QFile calls(dir.filePath("calls"));QVERIFY(calls.open(QIODevice::ReadOnly));QVERIFY(!calls.readAll().contains("worktrees create"));calls.close();
        QTimer::singleShot(0,&dialog,[&]{
            auto *form=dialog.findChild<QDialog *>("newWorktreeDialog");QVERIFY(form);QTimer::singleShot(3000,form,&QDialog::reject);
            form->findChild<QLineEdit *>("newWorktreeBranch")->setText("feature/new");
            form->findChild<QComboBox *>("newWorktreeBase")->setCurrentText("origin/main");
            form->findChild<QLineEdit *>("newWorktreePath")->setText("/new folder");
            auto *submit=form->findChild<QPushButton *>("createWorktree");submit->click();QVERIFY(!submit->isEnabled());form->reject();QVERIFY(form->isVisible());
        });create->trigger();
        QCOMPARE(dialog.findChild<QLabel *>("launchFolderPath")->text(),QString("/new folder"));dialog.setFleet(state);
        auto *hint=dialog.findChild<QLabel *>("muted");QVERIFY(!hint->property("outsideProject").toBool());QVERIFY(hint->text().isEmpty());QVERIFY(hint->styleSheet().isEmpty());
        QCOMPARE(dialog.findChild<QComboBox *>("launchProject")->currentData().toString(),project);QCOMPARE(dialog.findChild<QComboBox *>("launchComputer")->currentData().toString(),QString("mac"));QCOMPARE(selectedLaunchAccount(account),QString("work"));
        QCOMPARE(dialog.findChild<QLabel *>("launchFolderPath")->text(),QString("/new folder"));QCOMPARE(org.group(project)->folders.size(),1);
        QSignalSpy launched(&dialog,&NewSessionDialog::launchRequested);dialog.findChild<QPushButton *>("primary")->click();QTRY_COMPARE(launched.size(),1);QCOMPARE(launched.first()[2].toString(),QString("/new folder"));
        QVERIFY(calls.open(QIODevice::ReadOnly));const auto commands=calls.readAll();QCOMPARE(commands.count("worktrees create"),1);QVERIFY(commands.contains("@mac worktrees create"));QVERIFY(commands.contains("--base origin/main"));
    }
    void failedCreationRetainsFormAndOriginalFolder(){
        QFile fail(dir.filePath("fail-create"));QVERIFY(fail.open(QIODevice::WriteOnly));fail.close();
        auto state=fleet();SessionOrganization org;const auto project=org.createGroup("Work");org.addFolder(project,"arch","/repo");
        NewSessionDialog dialog(FakeCli::program(script()),state,{},"codex",project);dialog.setGroups(org,project);dialog.show();
        auto *create=dialog.findChild<QAction *>("newLaunchWorktree");QVERIFY(create);QTRY_VERIFY(create->isVisible()&&create->isEnabled());
        QTimer::singleShot(0,&dialog,[&]{
            auto *form=dialog.findChild<QDialog *>("newWorktreeDialog");QVERIFY(form);QTimer::singleShot(3000,form,&QDialog::reject);
            form->findChild<QLineEdit *>("newWorktreeBranch")->setText("taken");form->findChild<QPushButton *>("createWorktree")->click();
            auto *error=form->findChild<QLabel *>("newWorktreeError");QTRY_VERIFY(error->text().contains("already exists"));
            QCOMPARE(form->findChild<QLineEdit *>("newWorktreeBranch")->text(),QString("taken"));QVERIFY(form->findChild<QPushButton *>("createWorktree")->isEnabled());form->reject();
        });create->trigger();
        QCOMPARE(dialog.findChild<QLabel *>("launchFolderPath")->text(),QString("/repo"));QCOMPARE(org.group(project)->folders.size(),1);
    }
    void worktreeMembershipRequiresTheSelectedProjectAndMachine(){
        auto state=fleet();SessionOrganization org;const auto project=org.createGroup("Work"),other=org.createGroup("Other");
        org.addFolder(project,"arch","/repo/subfolder");org.addFolder(other,"arch","/unrelated");org.addFolder(project,"mac","/unrelated");
        NewSessionDialog dialog(FakeCli::program(script()),state,{},"codex",project);dialog.setGroups(org,project);dialog.show();
        QTRY_VERIFY(dialog.findChild<QAction *>("chooseLaunchWorktree")->isVisible());
        dialog.selectPath("/linked/subfolder");auto *hint=dialog.findChild<QLabel *>("muted");
        QTRY_VERIFY(!hint->property("outsideProject").toBool());QVERIFY(hint->text().isEmpty());
        dialog.setFleet(state);QVERIFY(!hint->property("outsideProject").toBool());
        dialog.selectPath("/linked-unrelated");QTRY_VERIFY(hint->property("outsideProject").toBool());
        dialog.selectPath("/bare");QTRY_VERIFY(hint->property("outsideProject").toBool());
        dialog.selectPath("/missing");QTRY_VERIFY(hint->property("outsideProject").toBool());
        dialog.selectPath("/linked");QTRY_VERIFY(!hint->property("outsideProject").toBool());
        auto *projects=dialog.findChild<QComboBox *>("launchProject");projects->setCurrentIndex(projects->findData(other));
        dialog.selectPath("/linked");QTRY_VERIFY(hint->property("outsideProject").toBool());
        projects->setCurrentIndex(projects->findData(project));
        auto *machine=dialog.findChild<QComboBox *>("launchComputer");machine->setCurrentIndex(machine->findData("mac"));
        dialog.selectPath("/linked");QTRY_VERIFY(hint->property("outsideProject").toBool());
    }
    void cleanupReminderReviewsOnceAndNeverRemoves(){
        HgsClient client(FakeCli::program(script()));WorktreePanel panel(&client,false,nullptr,WorktreePanel::Layout::Compact);auto state=fleet();panel.setContext({},"/repo",state);panel.resize(620,260);panel.show();
        auto *cleanup=panel.findChild<QPushButton *>("cleanUpWorktrees");QTRY_COMPARE(cleanup->text(),QString("Clean up (2)"));QVERIFY(cleanup->isEnabled());
        panel.setContext({},"/linked",state);panel.setContext({},"/repo",state);QTest::qWait(200);
        QCOMPARE(calls().count("worktrees review --path /repo/.git --json"),1);QVERIFY(!calls().contains("worktrees remove"));
        const QList<QWidget *> heading{panel.findChild<QPushButton *>("allWorktreeSessions"),cleanup,panel.findChild<QPushButton *>("refreshWorktrees"),panel.findChild<QPushButton *>("useWorktreeFolder")};
        for(int i=1;i<heading.size();++i)QVERIFY2(heading[i-1]->geometry().right()<heading[i]->geometry().left(),qPrintable(heading[i]->objectName()));
        WorktreePanel picker(&client,true);picker.setContext({},"/repo",state);picker.show();QVERIFY(!picker.findChild<QPushButton *>("cleanUpWorktrees")->isVisible());
        const auto preview=qEnvironmentVariable("HGS_WORKTREE_PREVIEW");if(!preview.isEmpty()){QDir().mkpath(preview);QVERIFY(panel.grab().save(preview+"/cleanup-heading.png"));}
    }
    void cleanupDialogConfirmsAndRemovesSelectedRowsOneAtATime(){
        QFile fail(dir.filePath("fail-remove"));QVERIFY(fail.open(QIODevice::WriteOnly));fail.write("/missing\n");fail.close();
        QSettings().setValue("workspace/worktreeCleanupDeleteBranches",true);
        HgsClient client(FakeCli::program(script()));WorktreePanel panel(&client,false,nullptr,WorktreePanel::Layout::Compact);auto state=fleet();panel.setContext({},"/repo",state);panel.resize(620,260);panel.show();
        auto *cleanup=panel.findChild<QPushButton *>("cleanUpWorktrees");QTRY_VERIFY(cleanup->isEnabled());bool checked=false;
        QTimer::singleShot(0,&panel,[&]{
            auto *dialog=panel.findChild<QDialog *>("worktreeCleanupDialog");QVERIFY(dialog);QTimer::singleShot(8000,dialog,[dialog]{dialog->done(0);});
            auto *list=dialog->findChild<QTreeWidget *>("worktreeCleanupList");QTRY_COMPARE(list->topLevelItemCount(),4);
            QCOMPARE(list->topLevelItem(0)->checkState(0),Qt::Checked);QCOMPARE(list->topLevelItem(1)->checkState(0),Qt::Unchecked);
            QVERIFY(!(list->topLevelItem(2)->flags()&Qt::ItemIsUserCheckable));QCOMPARE(list->topLevelItem(3)->checkState(0),Qt::Checked);
            QVERIFY(dialog->findChild<QLabel *>("worktreeCleanupContext")->text().contains("Repository: /repo"));
            QVERIFY(dialog->findChild<QLabel *>("worktreeCleanupDetails")->text().contains("target"));
            auto *remove=dialog->findChild<QPushButton *>("removeSelectedWorktrees");QCOMPARE(remove->text(),QString("Remove 2 worktrees…"));
            QVERIFY(dialog->findChild<QCheckBox *>("deleteMergedBranches")->isChecked());
            const auto preview=qEnvironmentVariable("HGS_WORKTREE_PREVIEW");if(!preview.isEmpty()){QDir().mkpath(preview);QVERIFY(dialog->grab().save(preview+"/cleanup-dialog.png"));}
            QTimer::singleShot(0,dialog,[dialog]{auto *box=dialog->findChild<QMessageBox *>("confirmWorktreeCleanup");QVERIFY(box);
                const auto preview=qEnvironmentVariable("HGS_WORKTREE_PREVIEW");if(!preview.isEmpty())QVERIFY(box->grab().save(preview+"/cleanup-confirm.png"));box->reject();});
            remove->click();QTest::qWait(150);QVERIFY(!calls().contains("worktrees remove"));
            QTimer::singleShot(0,dialog,[dialog]{auto *box=dialog->findChild<QMessageBox *>("confirmWorktreeCleanup");QVERIFY(box);
                QVERIFY(box->informativeText().contains("Merged branches deleted: linked, missing."));QVERIFY(box->informativeText().contains("Ignored files deleted with them"));
                box->findChild<QPushButton *>("confirmRemoveWorktrees")->click();});
            remove->click();
            auto *status=dialog->findChild<QLabel *>("worktreeCleanupStatus");QTRY_VERIFY(status->text().contains("Removed 1 worktree."));
            QVERIFY(status->text().contains("missing was not removed: Worktree changed since the review."));
            const auto commands=calls();const auto first=commands.indexOf("worktrees remove --path /linked"),second=commands.indexOf("worktrees remove --path /missing");
            QVERIFY(first>=0&&second>first);QVERIFY(commands.mid(first,second-first).contains("--fingerprint /linked-print"));QVERIFY(commands.mid(first,second-first).contains("--delete-branch"));
            QCOMPARE(commands.count("worktrees remove"),2);checked=true;dialog->reject();
        });
        cleanup->click();QVERIFY(checked);QTRY_VERIFY(calls().contains("worktrees --path /repo --json --refresh"));
    }
    void worktreeMenuReviewsOnlyTheChosenCheckout(){
        HgsClient client(FakeCli::program(script()));WorktreePanel panel(&client);auto state=fleet();panel.setContext({},"/repo",state);panel.resize(580,400);panel.show();
        auto *tree=panel.findChild<QTreeWidget *>("worktreeCatalog");QTRY_COMPARE(tree->topLevelItemCount(),4);QTRY_VERIFY(panel.findChild<QPushButton *>("cleanUpWorktrees")->isEnabled());
        const auto menuFor=[&](int row){tree->customContextMenuRequested(tree->visualItemRect(tree->topLevelItem(row)).center());return QPointer<QMenu>(panel.findChild<QMenu *>("worktreeMenu"));};
        auto menu=menuFor(0);QVERIFY(menu);QVERIFY(!menu->findChild<QAction *>("removeWorktreeAction"));QVERIFY(menu->findChild<QAction *>("copyWorktreePath"));delete menu.data();
        menu=menuFor(2);QCOMPARE(menu->findChild<QAction *>("removeWorktreeAction")->text(),QString("Forget missing worktree…"));delete menu.data();
        menu=menuFor(1);auto *remove=menu->findChild<QAction *>("removeWorktreeAction");QCOMPARE(remove->text(),QString("Remove worktree…"));bool opened=false;
        QTimer::singleShot(0,&panel,[&]{
            auto *dialog=panel.findChild<QDialog *>("worktreeCleanupDialog");QVERIFY(dialog);QTimer::singleShot(5000,dialog,[dialog]{dialog->done(0);});
            QTRY_VERIFY(calls().contains("worktrees review --path /repo/.git --json --worktree /linked"));QCOMPARE(dialog->windowTitle(),QString("Remove worktree"));
            auto *list=dialog->findChild<QTreeWidget *>("worktreeCleanupList");QTRY_COMPARE(list->topLevelItemCount(),1);
            QCOMPARE(list->topLevelItem(0)->data(0,Qt::UserRole).toString(),QString("/linked"));QCOMPARE(list->topLevelItem(0)->checkState(0),Qt::Checked);
            opened=true;dialog->reject();
        });
        remove->trigger();QVERIFY(opened);delete menu.data();QVERIFY(!calls().contains("worktrees remove"));
    }
    void olderMachineExplainsUpdateForCleanup(){
        QFile old(dir.filePath("old-hgs"));QVERIFY(old.open(QIODevice::WriteOnly));old.close();
        HgsClient client(FakeCli::program(script()));QSignalSpy failed(&client,&HgsClient::worktreeReviewFailed);client.reviewWorktrees("mac","/repo/.git");QTRY_COMPARE(failed.size(),1);
        QCOMPARE(failed.first()[3].toString(),QString("Update Zerus on mac to clean up worktrees."));QVERIFY(calls().contains("@mac worktrees review"));
    }
};
QTEST_MAIN(TestWorktreePanel)
#include "test_worktreepanel.moc"
