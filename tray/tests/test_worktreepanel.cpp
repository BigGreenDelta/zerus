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
#include "WorktreeInspectionPanel.h"
#include <QPlainTextEdit>
#include <QTreeWidgetItemIterator>
#include <QSplitter>
#include <QTabWidget>
#include <QHeaderView>
#include "WorktreeBatchCleanupDialog.h"

static int launchAccountRow(const QListWidget *list,const QString &id) {
    for(int row=0;row<list->count();++row)if(list->item(row)->data(Qt::UserRole).toString()==id)return row;
    return -1;
}
static QString selectedLaunchAccount(const QListWidget *list) {
    return list->currentItem()?list->currentItem()->data(Qt::UserRole).toString():QString();
}

static QList<QTreeWidgetItem *> overviewRows(QTreeWidget *tree) {
    QList<QTreeWidgetItem *> rows;
    for(QTreeWidgetItemIterator it(tree);*it;++it) if((*it)->parent()) rows << *it;
    return rows;
}
static QTreeWidgetItem *overviewRow(QTreeWidget *tree,const QString &path,const QString &machine={}) {
    for(auto *item:overviewRows(tree)) if(item->data(0,Qt::UserRole+1).toJsonObject()["path"]==path && (machine.isEmpty()||item->text(3)==machine)) return item;
    return nullptr;
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
                row("/linked","ready",{{"merged",true},{"ignored_count",1},{"ignored_fingerprint","ignored-print"},{"disk_bytes",4096},{"disk_complete",true},{"ignored_bytes",2048},{"ignored",QJsonArray{QJsonObject{{"path","target"},{"bytes",2048},{"complete",true}}}}}),
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
  source='review-full.json' if '--worktree' in a and os.path.exists('review-full.json') else 'review.json'
  r=json.load(open(source));r['common_dir']=a[a.index('--path')+1]
  dump(r)
 elif a[1:2]==['inspect']:
  if os.path.exists('fail-inspect'):print('fixture inspection unavailable',file=sys.stderr);sys.exit(1)
  if os.path.exists('slow-inspect'):time.sleep(0.4)
  o=[x for x in a[2:] if x!='--json'];v={}
  for i in range(0,len(o)-1,2):v[o[i]]=o[i+1]
  time.sleep(0.05)
  r={'state':'ok','path':v.get('--path',''),'common_dir':v.get('--common-dir','')}
  if '--file' in v:
   if v['--file']=='.':
    offset=int(v.get('--offset','0'))
    r['file']={'path':'.','offset':offset,'entries':[{'path':'notes.txt' if offset==0 else 'last.txt'}],'truncated':offset==0}
    if offset==0:r['file']['next_offset']=200
   else:r['file']={'path':v['--file'],'text':'important local notes','binary':False,'truncated':False}
  else:
   r.update({'review':json.load(open('review.json'))['worktrees'][1],
    'files':[{'path':'notes.txt','status':'??'}],'changes':{'text':'tracked diff'},
    'comparison':{'text':'base diff'},'commits':{'text':'1234567 retain this work'}})
  if os.path.exists('empty-inspect'):
   r['files']=[];r['changes']={'text':''};r['review']['ignored_count']=0;r['review']['ignored']=[]
  if os.path.exists('wrong-inspect'):r['path']='/wrong'
  dump(r)
 elif a[1:2]==['remove']:
  o=a[2:];v={};i=0
  while i<len(o):
   if o[i] in ('--request-id','--path') and i+1<len(o):v[o[i]]=o[i+1];i+=2
   elif o[i] in ('--common-dir','--fingerprint','--ignored-fingerprint'):i+=2
   else:i+=1
  time.sleep(0.05)
  if os.path.exists('fail-remove') and v.get('--path','') in open('fail-remove').read().split('\n'):
   print('Worktree changed since the review. Review it again.',file=sys.stderr);sys.exit(1)
  dump({'status':'removed','request_id':v.get('--request-id',''),'path':v.get('--path',''),'branch_deleted':False})
 else:
  if os.path.exists('fail-catalog'):print('fixture catalog unavailable',file=sys.stderr);sys.exit(1)
  time.sleep(0.05);sys.stdout.write(open('catalog.json').read())
elif c=='account':print('{"profiles":[{"id":"work","provider":"codex","label":"Work"}]}')
elif c in ('archive','terminate') and os.path.exists('cleanup-result.json'):
 print('ZERUS_WORKTREE_CLEANUP '+open('cleanup-result.json').read().strip(),file=sys.stderr)
elif c=='dirs':dump({'path':a[1] if len(a)>1 else '','directories':[]})
)PY");program.close();QVERIFY(FakeCli::makeRunnable(program.fileName()));
    }
    void init(){
        QSettings().clear();for(const auto *name:{"calls","fail-create","fail-remove","old-hgs","fail-catalog","wrong-inspect","review-full.json","fail-inspect","empty-inspect","slow-inspect","cleanup-result.json"})QFile::remove(dir.filePath(name));
        QFile response(dir.filePath("catalog.json"));QVERIFY(response.open(QIODevice::WriteOnly));response.write(QJsonDocument(catalog()).toJson());
        QFile reviewed(dir.filePath("review.json"));QVERIFY(reviewed.open(QIODevice::WriteOnly));reviewed.write(QJsonDocument(review()).toJson());
    }
    void overviewCombinesProjectsButKeepsComputerAndSessionIdentity(){
        SessionOrganization projects;const auto first=projects.createGroup("Shared"),second=projects.createGroup("Shared");
        projects.addFolder(first,"arch","/repo");projects.addFolder(second,"arch","/repo/subfolder");projects.addFolder(second,"mac","/repo");
        HgsClient client(FakeCli::program(script()));auto machines=fleet();auto local=machines.local();
        SessionInfo linked;linked.name="codex/linked/local";linked.gitRoot="/linked";linked.state="paused";local.sessions.append(linked);machines.setLocal(local,QDateTime::currentMSecsSinceEpoch());
        WorktreesDialog dialog(&client,projects,machines,false);dialog.show();
        auto *tree=dialog.findChild<QTreeWidget *>("worktreeOverviewList");auto *refresh=dialog.findChild<QPushButton *>("refreshWorktreeOverview");
        QTRY_COMPARE(overviewRows(tree).size(),4);QTRY_VERIFY(refresh->isEnabled()); // main folders are hidden by default
        dialog.findChild<QCheckBox *>("showMainWorktreeFolders")->setChecked(true);QCOMPARE(overviewRows(tree).size(),8);
        auto *localLinked=overviewRow(tree,"/linked","arch");auto *remoteLinked=overviewRow(tree,"/linked","mac");QVERIFY(localLinked);QVERIFY(remoteLinked);
        tree->setCurrentItem(localLinked);auto *sessions=dialog.findChild<QListWidget *>("worktreeRelatedSessions");QCOMPARE(sessions->count(),1);QVERIFY(sessions->item(0)->text().contains("codex/linked/local"));
        dialog.findChild<QPushButton *>("copyOverviewWorktreePath")->click();QCOMPARE(QApplication::clipboard()->text(),QString("/linked"));
        QVERIFY(!calls().contains("worktrees remove"));QVERIFY(!calls().contains("worktrees create"));
        QSignalSpy reset(tree->model(),&QAbstractItemModel::modelReset);dialog.setContext(projects,machines,false);QCOMPARE(reset.size(),0);QCOMPARE(tree->currentItem(),localLinked);
        auto *filter=dialog.findChild<QComboBox *>("worktreeProjectFilter");QCOMPARE(filter->count(),projects.groups().size()+1);QVERIFY(filter->findData(first)!=filter->findData(second));
        filter->setCurrentIndex(filter->findData(first));QCOMPARE(overviewRows(tree).size(),4);filter->setCurrentIndex(filter->findData(second));QCOMPARE(overviewRows(tree).size(),8);
        auto *computer=dialog.findChild<QComboBox *>("worktreeComputerFilter");computer->setCurrentIndex(computer->findData("mac"));QCOMPARE(overviewRows(tree).size(),4);
        for(auto *item:overviewRows(tree))QCOMPARE(item->text(3),QString("mac"));tree->setCurrentItem(overviewRow(tree,"/linked"));
        auto *details=dialog.findChild<QLabel *>("worktreeOverviewDetails");QVERIFY(details->text().contains("Local files outside Git"));QCOMPARE(sessions->count(),0);
        dialog.resize(980,700);QTest::qWait(30);auto *buttons=dialog.findChild<QDialogButtonBox *>();auto *close=buttons->button(QDialogButtonBox::Close);
        for(auto *button:buttons->buttons())if(button!=close)QVERIFY(button->geometry().right()<close->geometry().left());
        QVERIFY(tree->width()>=360);QVERIFY(dialog.rect().contains(tree->mapTo(&dialog,tree->rect().bottomRight())));
        const auto preview=qEnvironmentVariable("HGS_WORKTREE_PREVIEW");if(!preview.isEmpty()){QDir().mkpath(preview);QVERIFY(dialog.grab().save(preview+"/overview.png"));}
    }
    void overviewShowsCleanupReasonsAndOfflineSnapshots(){
        SessionOrganization projects;const auto project=projects.createGroup("Work");projects.addFolder(project,"arch","/repo");
        HgsClient client(FakeCli::program(script()));auto machines=fleet();WorktreesDialog dialog(&client,projects,machines,false);dialog.show();
        auto *tree=dialog.findChild<QTreeWidget *>("worktreeOverviewList");auto *details=dialog.findChild<QLabel *>("worktreeOverviewDetails");auto *cleanup=dialog.findChild<QPushButton *>("selectCleanupCandidates");
        QTRY_VERIFY(cleanup->isEnabled());dialog.findChild<QCheckBox *>("showMainWorktreeFolders")->setChecked(true);tree->setCurrentItem(overviewRow(tree,"/repo"));QVERIFY(details->text().contains("This is the main checkout"));
        tree->setCurrentItem(overviewRow(tree,"/linked"));QVERIFY(details->text().contains("Local files outside Git"));
        cleanup->click();QCOMPARE(overviewRow(tree,"/linked")->checkState(0),Qt::Unchecked);QCOMPARE(overviewRow(tree,"/missing")->checkState(0),Qt::Checked);
        // Finish the connected read before counting calls made while offline.
        QTRY_COMPARE(dialog.findChild<QLabel *>("worktreeChangedFilesState")->text(),QString("1 changed file"));
        const auto before=calls().count("worktrees");auto local=machines.local();local.ok=false;machines.setLocal(local,QDateTime::currentMSecsSinceEpoch());dialog.setContext(projects,machines,false);
        QCOMPARE(overviewRows(tree).size(),4);QVERIFY(!cleanup->isEnabled());QVERIFY(dialog.findChild<QPushButton *>("reviewSelectedWorktrees")->isEnabled());
        QTimer::singleShot(0,&dialog,[&]{auto *preview=dialog.findChild<QDialog *>("worktreeBatchCleanupDialog");QVERIFY(preview);QTimer::singleShot(5000,preview,[preview]{preview->reject();});auto *list=preview->findChild<QTreeWidget *>("batchCleanupList");QTRY_COMPARE(list->topLevelItem(0)->text(3),QString("Kept"));QVERIFY(!preview->findChild<QPushButton *>("confirmBatchWorktreeRemoval")->isEnabled());preview->reject();});
        dialog.findChild<QPushButton *>("reviewSelectedWorktrees")->click();QVERIFY(!calls().contains("worktrees remove"));QVERIFY(details->text().contains("Last known"));QVERIFY(!dialog.findChild<QPushButton *>("inspectWorktreeChanges")->isEnabled());
        cleanup->click();QTest::qWait(80);QCOMPARE(calls().count("worktrees"),before);
        local.ok=true;machines.setLocal(local,QDateTime::currentMSecsSinceEpoch());dialog.setContext(projects,machines,false);QTRY_VERIFY(cleanup->isEnabled());QTRY_VERIFY(calls().count("worktrees")>before);
    }
    void overviewIgnoresObsoleteCatalogWhenProjectAddressChanges(){
        SessionOrganization projects;const auto project=projects.createGroup("Work");const auto folder=projects.addFolder(project,"arch","/repo");
        HgsClient client(FakeCli::program(script()));QSignalSpy ready(&client,&HgsClient::worktreesReady);WorktreesDialog dialog(&client,projects,fleet(),false);dialog.show();
        QVERIFY(projects.editFolder(project,folder,"mac","/repo","Remote"));dialog.setContext(projects,fleet(),false);QTRY_COMPARE(ready.size(),2);
        auto *tree=dialog.findChild<QTreeWidget *>("worktreeOverviewList");QTRY_COMPARE(overviewRows(tree).size(),2);for(auto *item:overviewRows(tree))QCOMPARE(item->text(3),QString("mac"));
        client.worktreesReady(1,{},"/repo",QJsonObject{{"state","ok"},{"common_dir","/wrong/.git"},{"worktrees",QJsonArray{QJsonObject{{"path","/wrong"},{"kind","linked"}}}}});
        QCOMPARE(overviewRows(tree).size(),2);QVERIFY(!dialog.findChild<QLabel *>("worktreeOverviewDetails")->text().contains("/wrong"));QVERIFY(!calls().contains("worktrees remove"));
    }
    void overviewFailedCatalogKeepsSnapshotAndRequiresSuccessfulRefresh(){
        HgsClient client(FakeCli::program(script()));QSignalSpy loaded(&client,&HgsClient::worktreesReady);client.requestWorktrees({},"/repo");QTRY_COMPARE(loaded.size(),1);
        QFile fail(dir.filePath("fail-catalog"));QVERIFY(fail.open(QIODevice::WriteOnly));fail.close();
        SessionOrganization projects;const auto project=projects.createGroup("Work");projects.addFolder(project,"arch","/repo");WorktreesDialog dialog(&client,projects,fleet(),false);dialog.show();
        auto *refresh=dialog.findChild<QPushButton *>("refreshWorktreeOverview");QTRY_VERIFY(refresh->isEnabled());refresh->click();
        auto *details=dialog.findChild<QLabel *>("worktreeOverviewDetails");QTRY_VERIFY(details->text().contains("fixture catalog unavailable"));
        auto *tree=dialog.findChild<QTreeWidget *>("worktreeOverviewList");QCOMPARE(overviewRows(tree).size(),2);auto *cleanup=dialog.findChild<QPushButton *>("selectCleanupCandidates");QVERIFY(!cleanup->isEnabled());
        QVERIFY(!calls().contains("worktrees remove"));QVERIFY(QFile::remove(fail.fileName()));dialog.findChild<QPushButton *>("refreshWorktreeOverview")->click();QTRY_VERIFY(cleanup->isEnabled());
    }
    void overviewOrdinaryFoldersNeverOfferCleanup(){
        QFile response(dir.filePath("catalog.json"));QVERIFY(response.open(QIODevice::WriteOnly));response.write(QJsonDocument(QJsonObject{{"state","not_repo"},{"path","/plain"},{"worktrees",QJsonArray{}}}).toJson());response.close();
        SessionOrganization projects;const auto project=projects.createGroup("Work");projects.addFolder(project,"arch","/plain");HgsClient client(FakeCli::program(script()));WorktreesDialog dialog(&client,projects,fleet(),false);dialog.show();
        auto *tree=dialog.findChild<QTreeWidget *>("worktreeOverviewList");QTRY_COMPARE(overviewRows(tree).size(),0);dialog.findChild<QCheckBox *>("showMainWorktreeFolders")->setChecked(true);QCOMPARE(overviewRows(tree).size(),1);QCOMPARE(overviewRows(tree).first()->text(1),QString("Ordinary folder"));
        QVERIFY(!dialog.findChild<QPushButton *>("selectCleanupCandidates")->isEnabled());QVERIFY(!dialog.findChild<QPushButton *>("reviewSelectedWorktrees")->isEnabled());
        QVERIFY(!calls().contains("worktrees review"));QVERIFY(!calls().contains("worktrees create"));QVERIFY(!calls().contains("worktrees remove"));
    }
    void overviewCleanupKeepsConfirmationAndExactRemoteTarget_data(){QTest::addColumn<bool>("confirmed");QTest::newRow("cancel")<<false;QTest::newRow("confirm")<<true;}
    void overviewCleanupKeepsConfirmationAndExactRemoteTarget(){
        QFETCH(bool,confirmed);SessionOrganization projects;const auto project=projects.createGroup("Work");projects.addFolder(project,"mac","/repo");HgsClient client(FakeCli::program(script()));WorktreesDialog dialog(&client,projects,fleet(),false);dialog.show();
        auto *tree=dialog.findChild<QTreeWidget *>("worktreeOverviewList");QTRY_VERIFY(dialog.findChild<QPushButton *>("selectCleanupCandidates")->isEnabled());auto *linked=overviewRow(tree,"/linked");QVERIFY(linked);tree->setCurrentItem(linked);linked->setCheckState(0,Qt::Checked);
        auto *remove=dialog.findChild<QPushButton *>("reviewSelectedWorktrees");QVERIFY(remove->isEnabled());bool reviewed=false;
        QTimer::singleShot(0,&dialog,[&]{
            auto *previewDialog=dialog.findChild<QDialog *>("worktreeBatchCleanupDialog");QVERIFY(previewDialog);QTimer::singleShot(5000,previewDialog,[previewDialog]{previewDialog->done(0);});
            auto *list=previewDialog->findChild<QTreeWidget *>("batchCleanupList");QCOMPARE(list->topLevelItemCount(),1);QCOMPARE(list->topLevelItem(0)->data(0,Qt::UserRole).toJsonObject()["row"].toObject()["path"].toString(),QString("/linked"));
            auto *confirm=previewDialog->findChild<QPushButton *>("confirmBatchWorktreeRemoval");QVERIFY(!confirm->isEnabled());confirm->click();QVERIFY(!calls().contains("worktrees remove"));
            const auto preview=qEnvironmentVariable("HGS_WORKTREE_PREVIEW");if(!preview.isEmpty()){QDir().mkpath(preview);QVERIFY(previewDialog->grab().save(preview+"/cleanup-preview.png"));}
            QTRY_COMPARE(list->topLevelItem(0)->text(3),QString("Review local files"));
            if(confirmed){previewDialog->findChild<QCheckBox *>("confirmIgnoredWorktreeFiles")->setChecked(true);QVERIFY(confirm->isEnabled());confirm->click();QTRY_VERIFY(previewDialog->findChild<QLabel *>("batchCleanupStatus")->text().contains("1 removed"));QCOMPARE(calls().count("worktrees remove"),1);QVERIFY(calls().contains("@mac worktrees remove --path /linked --common-dir /repo/.git --fingerprint /linked-print"));QVERIFY(calls().contains("--ignored-fingerprint ignored-print"));QVERIFY(!calls().contains("--force"));}
            else QVERIFY(!calls().contains("worktrees remove"));reviewed=true;previewDialog->reject();
        });remove->click();QVERIFY(reviewed);QCOMPARE(dialog.changed(),confirmed);if(confirmed)QTRY_VERIFY(calls().count("@mac worktrees --path /repo --json --refresh")>=2);
    }
    void overviewInspectsChangesLocalFilesAndSessionsWithoutMutation(){
        SessionOrganization projects;const auto project=projects.createGroup("Work");projects.addFolder(project,"mac","/repo");auto machines=fleet();auto remote=*machines.peer("mac");remote.sessions[0].gitRoot="/linked";machines.setPeer(remote,QDateTime::currentMSecsSinceEpoch());
        HgsClient client(FakeCli::program(script()));WorktreesDialog dialog(&client,projects,machines,false);dialog.show();auto *tree=dialog.findChild<QTreeWidget *>("worktreeOverviewList");QTRY_VERIFY(dialog.findChild<QPushButton *>("selectCleanupCandidates")->isEnabled());tree->setCurrentItem(overviewRow(tree,"/linked"));
        auto *inspect=dialog.findChild<QPushButton *>("inspectWorktreeChanges");QVERIFY(inspect->isEnabled());inspect->click();auto *files=dialog.findChild<QTreeWidget *>("worktreeChangedFiles");QTRY_COMPARE(files->topLevelItemCount(),1);
        QCOMPARE(dialog.findChild<QPlainTextEdit *>("worktreeChangesDiff")->toPlainText(),QString("tracked diff"));QVERIFY(dialog.findChild<QPlainTextEdit *>("worktreeCommits")->toPlainText().contains("retain this work"));
        auto *local=dialog.findChild<QTreeWidget *>("worktreeLocalFiles");QCOMPARE(local->topLevelItemCount(),1);local->itemActivated(local->topLevelItem(0),0);QTRY_VERIFY(dialog.findChild<QPlainTextEdit *>("worktreeLocalFilePreview")->toPlainText().contains("important local notes"));
        QVERIFY(calls().contains("@mac worktrees inspect --path /linked --common-dir /repo/.git"));QVERIFY(calls().contains("--file target"));QVERIFY(!calls().contains("worktrees remove"));
        QString targetHost,targetName;dialog.sessionRequested=[&](const QString &host,const QString &name){targetHost=host;targetName=name;};auto *sessions=dialog.findChild<QListWidget *>("worktreeRelatedSessions");QCOMPARE(sessions->count(),1);sessions->itemActivated(sessions->item(0));QCOMPARE(targetHost,QString("mac"));QCOMPARE(targetName,QString("kimi/repo/two"));
    }
    void inspectionRejectsMismatchedIdentity(){
        QFile wrong(dir.filePath("wrong-inspect"));QVERIFY(wrong.open(QIODevice::WriteOnly));wrong.close();HgsClient client(FakeCli::program(script()));QSignalSpy result(&client,&HgsClient::worktreeInspectionFinished);
        client.inspectWorktree("mac","/linked","/repo/.git");QTRY_COMPARE(result.size(),1);QVERIFY(!result.first()[5].toString().isEmpty());
    }
    void overviewContinuesPastNativeReviewLimitAndKeepsCoverage(){
        auto cat=catalog(),initial=review(),full=review();QJsonArray allRows{cat["worktrees"].toArray().first()},firstRows,fullRows;
        for(int i=0;i<67;++i){const auto path=QString("/finished-%1").arg(i,3,10,QChar('0'));auto row=QJsonObject{{"path",path},{"kind","linked"},{"branch",QString("done/%1").arg(i)},{"available",true},{"verdict","ready"},{"merged",true},{"fingerprint",path+"-v1"},{"disk_bytes",i*1024},{"disk_complete",true}};allRows.append(row);fullRows.append(row);if(i<64)firstRows.append(row);}
        cat["worktrees"]=allRows;initial["worktrees"]=firstRows;initial["partial"]=true;full["worktrees"]=fullRows;
        for(const auto &pair:QList<QPair<QString,QJsonObject>>{{"catalog.json",cat},{"review.json",initial},{"review-full.json",full}}){QFile file(dir.filePath(pair.first));QVERIFY(file.open(QIODevice::WriteOnly));file.write(QJsonDocument(pair.second).toJson());}
        SessionOrganization projects;const auto project=projects.createGroup("Work");projects.addFolder(project,"arch","/repo");HgsClient client(FakeCli::program(script()));WorktreesDialog dialog(&client,projects,fleet(),false);dialog.show();
        auto *status=dialog.findChild<QLabel *>("worktreeOverviewStatus");QTRY_VERIFY(status->text().contains("67 of 67 worktrees checked"));auto *tree=dialog.findChild<QTreeWidget *>("worktreeOverviewList");QCOMPARE(overviewRows(tree).size(),67);
        QCOMPARE(calls().count("worktrees review"),4);for(int i=64;i<67;++i)QVERIFY(calls().contains(QString("--worktree /finished-%1").arg(i,3,10,QChar('0')).toUtf8()));
        QCOMPARE(overviewRows(tree).first()->data(0,Qt::UserRole+1).toJsonObject()["path"].toString(),QString("/finished-066"));
        dialog.findChild<QLineEdit *>("worktreeSearch")->setText("finished-064");QCOMPARE(overviewRows(tree).size(),1);dialog.findChild<QPushButton *>("selectCleanupCandidates")->click();QCOMPARE(overviewRows(tree).first()->checkState(0),Qt::Checked);QCOMPARE(dialog.findChild<QPushButton *>("reviewSelectedWorktrees")->text(),QString("Review cleanup (1)…"));
        QVERIFY(!calls().contains("worktrees remove"));
    }
    void overviewRefreshDiscardsChangedSelections(){
        SessionOrganization projects;const auto project=projects.createGroup("Work");projects.addFolder(project,"arch","/repo");HgsClient client(FakeCli::program(script()));WorktreesDialog dialog(&client,projects,fleet(),false);dialog.show();auto *choose=dialog.findChild<QPushButton *>("selectCleanupCandidates");QTRY_VERIFY(choose->isEnabled());
        auto *tree=dialog.findChild<QTreeWidget *>("worktreeOverviewList");auto *remove=dialog.findChild<QPushButton *>("reviewSelectedWorktrees");choose->click();QVERIFY(remove->isEnabled());
        auto data=review();auto rows=data["worktrees"].toArray();auto missing=rows.last().toObject();missing["fingerprint"]="changed-print";rows.replace(rows.size()-1,missing);data["worktrees"]=rows;QFile file(dir.filePath("review.json"));QVERIFY(file.open(QIODevice::WriteOnly));file.write(QJsonDocument(data).toJson());file.close();
        dialog.findChild<QPushButton *>("refreshWorktreeOverview")->click();QVERIFY(!remove->isEnabled());QTRY_VERIFY(choose->isEnabled());QCOMPARE(overviewRow(tree,"/missing")->checkState(0),Qt::Unchecked);QVERIFY(!remove->isEnabled());QVERIFY(!calls().contains("worktrees remove"));
    }
    void cleanupDecisionShowsIntegrationAndDataLossTogether(){
        auto row=review()["worktrees"].toArray()[1].toObject();
        row["integration"]=QJsonObject{{"kind","ancestor"},{"base","origin/main"}};row["changes"]=0;row["untracked"]=0;
        QVERIFY(WorktreeReviewUi::summary(row).contains("Merged into origin/main"));
        QVERIFY(WorktreeReviewUi::summary(row).contains("Local files outside Git"));
        const auto decision=WorktreeReviewUi::decision(row).join('\n');
        QVERIFY(decision.contains("no changed or untracked files"));QVERIFY(decision.contains("Git cannot restore them"));
        QVERIFY(decision.contains("Automatic cleanup keeps folders"));QVERIFY(decision.contains("local Git refs"));
        row.remove("changes");QVERIFY(!WorktreeReviewUi::decision(row).join('\n').contains("no changed or untracked files"));
    }
    void batchExplainsConsentAndOpensExactFolderInspection(){
        auto data=review();auto rows=data["worktrees"].toArray();auto row=rows[1].toObject();
        row["integration"]=QJsonObject{{"kind","ancestor"},{"base","origin/main"}};row["changes"]=0;row["untracked"]=0;rows.replace(1,row);data["worktrees"]=rows;
        QFile file(dir.filePath("review.json"));QVERIFY(file.open(QIODevice::WriteOnly));file.write(QJsonDocument(data).toJson());file.close();
        HgsClient client(FakeCli::program(script()));WorktreeBatchCleanupDialog dialog(&client,{{{"host","mac"},{"machine","mac"},{"common_dir","/repo/.git"},{"cleanup_reason","Ignored files remain"},{"row",row}}});dialog.show();
        auto *list=dialog.findChild<QTreeWidget *>("batchCleanupList");QTRY_COMPARE(list->topLevelItem(0)->text(3),QString("Review local files"));
        auto *details=dialog.findChild<QPlainTextEdit *>("worktreeCleanupDecision");QVERIFY(details);
        const auto text=details->toPlainText();QVERIFY(text.contains("Why the folder stayed: Ignored files remain"));QVERIFY(text.contains("Merged into origin/main"));
        QVERIFY(text.contains("conversations remain"));QVERIFY(text.contains("recreating its working folder"));QVERIFY(text.contains("target"));
        auto *remove=dialog.findChild<QPushButton *>("confirmBatchWorktreeRemoval");QVERIFY(!remove->isEnabled());
        auto *status=dialog.findChild<QLabel *>("batchCleanupStatus");QVERIFY(status->text().contains("inspect the local files"));
        auto *inspect=dialog.findChild<QPushButton *>("inspectCleanupWorktree");QVERIFY(inspect->isEnabled());
        bool inspected=false;
        QTimer::singleShot(0,&dialog,[&]{auto *viewer=dialog.findChild<QDialog *>("cleanupFileInspection");QVERIFY(viewer);
            QCOMPARE(viewer->findChild<QTabWidget *>("worktreeInspectionTabs")->currentIndex(),2);
            QVERIFY(viewer->findChild<QLabel *>("worktreeOverviewDetails")->text().contains("/linked"));
            QVERIFY(viewer->findChild<QLabel *>("worktreeBrief")->text().startsWith("mac"));inspected=true;viewer->reject();});
        inspect->click();QVERIFY(inspected);QVERIFY(!remove->isEnabled());QVERIFY(!calls().contains("worktrees remove"));
        dialog.findChild<QCheckBox *>("confirmIgnoredWorktreeFiles")->setChecked(true);QVERIFY(remove->isEnabled());
        dialog.resize(640,600);QTest::qWait(20);QVERIFY(dialog.rect().contains(QRect(remove->mapTo(&dialog,QPoint()),remove->size())));
        const auto raster=qEnvironmentVariable("HGS_WORKTREE_PREVIEW");if(!raster.isEmpty())QVERIFY(dialog.grab().save(raster+"/cleanup-decision.png"));
        dialog.findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Cancel)->click();QVERIFY(!calls().contains("worktrees remove"));
    }
    void batchCleanupKeepsExactComputerScopesAndNeverRetriesFailure(){
        const auto row=review()["worktrees"].toArray().last().toObject();QFile fail(dir.filePath("fail-remove"));QVERIFY(fail.open(QIODevice::WriteOnly));fail.write("/missing\n");fail.close();
        HgsClient client(FakeCli::program(script()));WorktreeBatchCleanupDialog dialog(&client,{{{"host",""},{"machine","arch"},{"common_dir","/repo/.git"},{"row",row}},{{"host","mac"},{"machine","mac"},{"common_dir","/other/.git"},{"row",row}}});dialog.show();
        auto *remove=dialog.findChild<QPushButton *>("confirmBatchWorktreeRemoval");QTRY_VERIFY(remove->isEnabled());remove->click();QTRY_VERIFY(dialog.findChild<QLabel *>("batchCleanupStatus")->text().contains("0 removed · 2 unconfirmed"));
        QCOMPARE(calls().count("worktrees remove"),2);QVERIFY(calls().contains("@mac worktrees remove --path /missing --common-dir /other/.git"));QVERIFY(calls().contains("worktrees remove --path /missing --common-dir /repo/.git"));QVERIFY(!dialog.changed());QVERIFY(dialog.attempted());QVERIFY(!remove->isEnabled());remove->click();QTest::qWait(80);QCOMPARE(calls().count("worktrees remove"),2);
    }
    void overviewSupportsNativeMultipleSelectionOutsideCandidates(){
        auto catalogData=catalog(), reviewData=review();QJsonArray rows,reviews;
        for(int i=1;i<=3;++i){auto row=reviewData["worktrees"].toArray()[2].toObject();row["path"]=QString("/review-%1").arg(i);rows.append(row);reviews.append(row);}
        catalogData["worktrees"]=rows;reviewData["worktrees"]=reviews;
        for(const auto &entry:QList<QPair<QString,QJsonObject>>{{"catalog.json",catalogData},{"review.json",reviewData}}){QFile file(dir.filePath(entry.first));QVERIFY(file.open(QIODevice::WriteOnly));file.write(QJsonDocument(entry.second).toJson());}
        SessionOrganization projects;const auto group=projects.createGroup("Work");projects.addFolder(group,"arch","/repo");HgsClient client(FakeCli::program(script()));WorktreesDialog dialog(&client,projects,fleet(),false);dialog.show();
        auto *tree=dialog.findChild<QTreeWidget *>("worktreeOverviewList");QTRY_COMPARE(overviewRows(tree).size(),3);QCOMPARE(tree->selectionMode(),QAbstractItemView::ExtendedSelection);
        const auto click=[&](const QString &path,Qt::KeyboardModifiers modifiers){auto *row=overviewRow(tree,path);QVERIFY(row);auto rect=tree->visualItemRect(row);QTest::mouseClick(tree->viewport(),Qt::LeftButton,modifiers,QPoint(rect.left()+100,rect.center().y()));};
        click("/review-1",Qt::NoModifier);click("/review-2",Qt::ControlModifier);QCOMPARE(tree->selectedItems().size(),2);
        click("/review-3",Qt::ShiftModifier);QVERIFY(tree->selectedItems().size()>=2);
        for(auto *row:tree->selectedItems())QCOMPARE(row->checkState(0),Qt::Checked);
        QVERIFY(dialog.findChild<QPushButton *>("reviewSelectedWorktrees")->isEnabled());QVERIFY(!calls().contains("worktrees remove"));
    }
    void batchFreshReviewRequiresDiscardAndClosesOnlyCompleteSuccess(){
        auto data=review();auto rows=data["worktrees"].toArray();auto dirty=rows[3].toObject();dirty["discardable"]=true;dirty["discard_fingerprint"]="discard-print";dirty["changes"]=2;dirty["changed_files"]=QJsonArray{" M notes.txt"};rows.replace(3,dirty);data["worktrees"]=rows;
        QFile file(dir.filePath("review.json"));QVERIFY(file.open(QIODevice::WriteOnly));file.write(QJsonDocument(data).toJson());file.close();
        auto stale=dirty;stale["branch"]="old-cached-branch";
        HgsClient client(FakeCli::program(script()));WorktreeBatchCleanupDialog dialog(&client,{{{"host",""},{"machine","arch"},{"common_dir","/repo/.git"},{"row",stale}}});QSignalSpy done(&dialog,&QDialog::finished);dialog.show();
        auto *remove=dialog.findChild<QPushButton *>("confirmBatchWorktreeRemoval");QVERIFY(!remove->isEnabled());
        auto *discard=dialog.findChild<QCheckBox *>("confirmDiscardWorktreeChanges");QTRY_VERIFY(discard->isVisible());QVERIFY(!remove->isEnabled());QVERIFY(!calls().contains("worktrees remove"));
        QVERIFY(dialog.findChild<QTreeWidget *>("batchCleanupList")->topLevelItem(0)->text(0).contains(dirty["branch"].toString()));
        const auto raster=qEnvironmentVariable("HGS_WORKTREE_PREVIEW");if(!raster.isEmpty())QVERIFY(dialog.grab().save(raster+"/manual-discard.png"));
        discard->setChecked(true);QVERIFY(remove->isEnabled());remove->click();QTRY_COMPARE(done.size(),1);QCOMPARE(done[0][0].toInt(),int(QDialog::Accepted));QVERIFY(!dialog.isVisible());
        QVERIFY(calls().contains("--discard-fingerprint discard-print"));QVERIFY(!calls().contains("--delete-branch"));
    }
    void batchAllowsReviewingBlockedAndOfflineSelectionsWithoutDeletingThem(){
        const auto blocked=review()["worktrees"].toArray()[3].toObject();const auto missing=review()["worktrees"].toArray().last().toObject();
        HgsClient client(FakeCli::program(script()));WorktreeBatchCleanupDialog dialog(&client,{{{"host",""},{"machine","arch"},{"common_dir","/repo/.git"},{"row",blocked}},{{"host","mac"},{"machine","mac"},{"common_dir","/repo/.git"},{"row",missing},{"offline",true}}});dialog.show();
        auto *list=dialog.findChild<QTreeWidget *>("batchCleanupList");QTRY_COMPARE(list->topLevelItem(1)->text(3),QString("Kept"));QCOMPARE(list->topLevelItem(0)->text(3),QString("Kept"));
        QVERIFY(!dialog.findChild<QPushButton *>("confirmBatchWorktreeRemoval")->isEnabled());QVERIFY(dialog.isVisible());QVERIFY(!calls().contains("worktrees remove"));
    }
    void nativeArchiveCleanupOutcomeIsSeparateFromTheSuccessfulSessionWrite(){
        const QJsonObject result{{"status","kept"},{"path","/linked"},{"common_dir","/repo/.git"},{"reason","Local files remain"}};
        QFile file(dir.filePath("cleanup-result.json"));QVERIFY(file.open(QIODevice::WriteOnly));file.write(QJsonDocument(result).toJson(QJsonDocument::Compact));file.close();
        HgsClient client(FakeCli::program(script()));QSignalSpy writes(&client,&HgsClient::writeDone),cleanup(&client,&HgsClient::worktreeCleanupFinished);
        client.archiveSession("mac","claude/work/one");QTRY_COMPARE(writes.size(),1);QVERIFY(writes[0][1].toBool());QTRY_COMPARE(cleanup.size(),1);
        QCOMPARE(cleanup[0][0].toString(),QString("mac"));QCOMPARE(cleanup[0][1].toString(),QString("archive claude/work/one"));QCOMPARE(cleanup[0][2].toJsonObject(),result);
    }
    void batchCanStopRemainingFoldersWithoutCancellingCurrentOperation(){
        const auto row=review()["worktrees"].toArray().last().toObject();HgsClient client(FakeCli::program(script()));WorktreeBatchCleanupDialog dialog(&client,{{{"host",""},{"machine","arch"},{"common_dir","/repo/.git"},{"row",row}},{{"host","mac"},{"machine","mac"},{"common_dir","/other/.git"},{"row",row}}});dialog.show();
        QTRY_VERIFY(dialog.findChild<QPushButton *>("confirmBatchWorktreeRemoval")->isEnabled());dialog.findChild<QPushButton *>("confirmBatchWorktreeRemoval")->click();auto *stop=dialog.findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Cancel);QCOMPARE(stop->text(),QString("Stop after this folder"));stop->click();
        QTRY_VERIFY(dialog.findChild<QLabel *>("batchCleanupStatus")->text().contains("1 kept without an attempt"));QVERIFY(dialog.changed());QCOMPARE(calls().count("worktrees remove"),1);QVERIFY(!calls().contains("@mac worktrees remove"));QVERIFY(stop->isEnabled());
    }
    void oldReviewCannotAuthorizeIgnoredFileRemoval(){
        auto data=review();auto rows=data["worktrees"].toArray();auto local=rows[1].toObject();local.remove("ignored_fingerprint");rows.replace(1,local);data["worktrees"]=rows;QFile file(dir.filePath("review.json"));QVERIFY(file.open(QIODevice::WriteOnly));file.write(QJsonDocument(data).toJson());file.close();
        SessionOrganization projects;const auto project=projects.createGroup("Work");projects.addFolder(project,"arch","/repo");HgsClient client(FakeCli::program(script()));WorktreesDialog dialog(&client,projects,fleet(),false);dialog.show();QTRY_VERIFY(dialog.findChild<QPushButton *>("selectCleanupCandidates")->isEnabled());auto *row=overviewRow(dialog.findChild<QTreeWidget *>("worktreeOverviewList"),"/linked");QVERIFY(row);QVERIFY(row->flags()&Qt::ItemIsUserCheckable);QVERIFY(row->text(1).contains("Update Zerus"));QVERIFY(row->parent()->text(0).contains("Not checked"));QVERIFY(!calls().contains("worktrees remove"));
    }
    void cleanupWorkspaceGeometryAndInspection_data(){
        QTest::addColumn<int>("fontSize");QTest::addColumn<int>("width");QTest::newRow("normal")<<10<<1180;QTest::newRow("large-font")<<15<<1080;
    }
    void cleanupWorkspaceGeometryAndInspection(){
        QFETCH(int,fontSize);QFETCH(int,width);auto cat=catalog(),data=review();auto rows=data["worktrees"].toArray();
        auto ready=rows[1].toObject();ready.remove("ignored");ready.remove("ignored_count");ready.remove("ignored_bytes");ready["path"]="/workspace/project/done-cleanup";ready["branch"]="fix/completed";ready["disk_bytes"]=268435456;rows.append(ready);
        auto busy=ready;busy["path"]="/workspace/project/active-task";busy["verdict"]="blocked";busy["reasons"]=QJsonArray{QJsonObject{{"code","sessions"},{"message","A saved agent session uses this folder."}}};rows.append(busy);
        auto changed=rows[3].toObject();changed["path"]="/workspace/project/draft-work";changed["changes"]=2;rows.append(changed);data["worktrees"]=rows;cat["worktrees"]=rows;
        for(const auto &pair:QList<QPair<QString,QJsonObject>>{{"catalog.json",cat},{"review.json",data}}){QFile file(dir.filePath(pair.first));QVERIFY(file.open(QIODevice::WriteOnly));file.write(QJsonDocument(pair.second).toJson());}
        SessionOrganization projects;const auto project=projects.createGroup("Example project");projects.addFolder(project,"arch","/repo");HgsClient client(FakeCli::program(script()));WorktreesDialog dialog(&client,projects,fleet(),false);auto font=dialog.font();font.setPointSize(fontSize);dialog.setFont(font);dialog.resize(width,820);dialog.show();
        QTRY_VERIFY(dialog.findChild<QPushButton *>("selectCleanupCandidates")->isEnabled());auto *tree=dialog.findChild<QTreeWidget *>("worktreeOverviewList");tree->setCurrentItem(overviewRow(tree,"/linked"));dialog.findChild<QPushButton *>("inspectWorktreeChanges")->click();QTRY_COMPARE(dialog.findChild<QTreeWidget *>("worktreeChangedFiles")->topLevelItemCount(),1);QTest::qWait(30);
        QVERIFY2(dialog.width()<=width,qPrintable(QString("minimum width %1 exceeds fixture %2").arg(dialog.width()).arg(width)));
        for(auto *widget:dialog.findChildren<QWidget *>())if(widget->isVisible()&&widget->isWindow()==false&&qobject_cast<QPushButton *>(widget))QVERIFY2(dialog.rect().contains(QRect(widget->mapTo(&dialog,QPoint()),widget->size())),qPrintable(widget->objectName()));
        auto *split=dialog.findChild<QSplitter *>();QVERIFY(split->widget(0)->geometry().right()<split->widget(1)->geometry().left());QVERIFY(split->widget(1)->width()>=300);QVERIFY(dialog.findChild<QPlainTextEdit *>("worktreeChangesDiff")->height()>=100);
        const auto preview=qEnvironmentVariable("HGS_WORKTREE_PREVIEW");if(!preview.isEmpty())QVERIFY(dialog.grab().save(preview+QString("/workspace-%1.png").arg(fontSize)));
        QVERIFY(!calls().contains("worktrees remove"));
    }
    void compactWorkspaceKeepsLocalFilesUsable_data(){
        QTest::addColumn<int>("fontSize"); QTest::addColumn<int>("width"); QTest::addColumn<int>("height");
        QTest::newRow("normal")<<10<<1180<<760;
        QTest::newRow("small")<<10<<920<<620;
        QTest::newRow("large-font")<<15<<1000<<760;
    }
    void compactWorkspaceKeepsLocalFilesUsable(){
        QFETCH(int,fontSize); QFETCH(int,width); QFETCH(int,height);
        auto reviewed=review(); auto rows=reviewed["worktrees"].toArray(); auto row=rows[1].toObject();
        QJsonArray ignored; for(int i=0;i<12;++i) ignored.append(QJsonObject{{"path",QString("local-notes/important-%1.txt").arg(i)},{"bytes",2048}});
        row["ignored"]=ignored; row["ignored_count"]=25; row["branch"]="fix/preserve-important-work"; rows[1]=row; reviewed["worktrees"]=rows;
        QFile response(dir.filePath("review.json")); QVERIFY(response.open(QIODevice::WriteOnly)); response.write(QJsonDocument(reviewed).toJson()); response.close();
        SessionOrganization projects; const auto project=projects.createGroup("Example project"); projects.addFolder(project,"mac","/repo");
        auto machines=fleet(); auto remote=*machines.peer("mac"); remote.sessions[0].gitRoot="/linked"; machines.setPeer(remote,QDateTime::currentMSecsSinceEpoch());
        HgsClient client(FakeCli::program(script())); WorktreesDialog dialog(&client,projects,machines,false);
        auto font=dialog.font(); font.setPointSize(fontSize); dialog.setFont(font);
        // Include the production padding that made the original layout unusable.
        dialog.setStyleSheet(QString("QWidget {font-size:%1pt;} QDialog {background:#1b1f25;} QLabel {color:#e2e9ef;}"
            "QLineEdit,QComboBox {padding:9px 10px; color:#e2e9ef; background:#252d36; border:1px solid #3d4854;}"
            "QPushButton {padding:8px 13px; min-height:18px; color:#e2e9ef; background:#252d36; border:1px solid #3d4854;}"
            "QTreeWidget,QListWidget,QPlainTextEdit {color:#e2e9ef; background:#20272e; selection-background-color:#416a63;}"
            "QHeaderView::section {padding:10px; color:#a1adbb; background:#252d36;}"
            "QTabWidget::pane {border:0; border-top:1px solid #3d4854; background:#1b1f25;}"
            "QTabBar::tab {background:#1b1f25; color:#a1adbb; padding:8px 9px; border-bottom:2px solid transparent;}"
            "QTabBar::tab:selected {color:#e2e9ef; border-bottom:2px solid #8bd6c1;}"
            "QMenu {background:#252d36; color:#e2e9ef;} QCheckBox {color:#e2e9ef;}").arg(fontSize));
        dialog.resize(width,height); dialog.show();
        QTRY_VERIFY(dialog.findChild<QPushButton *>("selectCleanupCandidates")->isEnabled());
        auto *tree=dialog.findChild<QTreeWidget *>("worktreeOverviewList"); tree->setCurrentItem(overviewRow(tree,"/linked"));
        auto *tabs=dialog.findChild<QTabWidget *>(); tabs->setCurrentIndex(2);
        auto *local=dialog.findChild<QTreeWidget *>("worktreeLocalFiles"); QTRY_COMPARE(local->topLevelItemCount(),12);
        QVERIFY(dialog.findChild<QLabel *>("worktreeInspectionStatus")->text().contains("12 of 25"));
        auto *preview=dialog.findChild<QPlainTextEdit *>("worktreeLocalFilePreview"); QTest::qWait(30);
        QVERIFY2(dialog.width()<=width && dialog.height()<=height,"Layout grew beyond the requested window");
        QVERIFY2(tree->mapTo(&dialog,QPoint()).y()<dialog.fontMetrics().lineSpacing()*5+40,"Filters displaced the workspace");
        QVERIFY(!dialog.findChild<QWidget *>("worktreeSummaryArea")->isVisible());
        QVERIFY(tree->currentItem()->data(0,Qt::AccessibleDescriptionRole).toString().contains("Branch: fix/preserve-important-work"));
        const auto first=local->visualItemRect(local->topLevelItem(0)), third=local->visualItemRect(local->topLevelItem(2));
        QVERIFY2(local->viewport()->rect().contains(first) && local->viewport()->rect().contains(third),"Local file rows are clipped");
        QVERIFY(!preview->isVisible());
        QVERIFY(local->columnWidth(0)>local->width()/2);
        const auto raster=qEnvironmentVariable("HGS_WORKTREE_PREVIEW");
        if(!raster.isEmpty()) QVERIFY(dialog.grab().save(raster+QString("/compact-local-%1-%2.png").arg(fontSize).arg(width)));
        QTest::mouseClick(local->viewport(),Qt::LeftButton,Qt::NoModifier,first.center());
        QCOMPARE(local->currentItem(),local->topLevelItem(0));
        QTest::mouseDClick(local->viewport(),Qt::LeftButton,Qt::NoModifier,first.center());
        QTRY_VERIFY(preview->toPlainText().contains("important local notes"));
        QVERIFY(preview->isVisible()); QTRY_VERIFY(preview->viewport()->height()>=preview->fontMetrics().lineSpacing()*3);
        if(!raster.isEmpty()) QVERIFY(dialog.grab().save(raster+QString("/compact-preview-%1-%2.png").arg(fontSize).arg(width)));
        auto *browse=dialog.findChild<QPushButton *>("browseAllWorktreeFiles"); QTest::mouseClick(browse,Qt::LeftButton);
        auto *next=dialog.findChild<QPushButton *>("nextWorktreeFiles"); QTRY_VERIFY(next->isVisible());
        QTRY_COMPARE(local->topLevelItemCount(),1); QCOMPARE(local->topLevelItem(0)->text(0),QString("notes.txt"));
        QVERIFY(!dialog.findChild<QLabel *>("worktreeInspectionStatus")->text().contains("largest"));
        QVERIFY(dialog.rect().contains(QRect(next->mapTo(&dialog,QPoint()),next->size())));
        QTest::mouseClick(next,Qt::LeftButton); QTRY_COMPARE(local->topLevelItem(0)->text(0),QString("last.txt")); QTRY_VERIFY(!next->isVisible());
        QTest::mouseClick(dialog.findChild<QPushButton *>("showLocalWorktreeFiles"),Qt::LeftButton); QCOMPARE(local->topLevelItemCount(),12);
        const auto workspaceTop=tree->mapTo(&dialog,QPoint()).y();
        auto *filters=dialog.findChild<QPushButton *>("worktreeFilters"); auto *menu=dialog.findChild<QMenu *>("worktreeFiltersMenu"); bool popupChecked=false;
        QTimer::singleShot(0,&dialog,[&]{
            QVERIFY(menu->isVisible()); auto *computer=dialog.findChild<QComboBox *>("worktreeComputerFilter"); QVERIFY(computer->isVisible());
            QTest::mouseClick(computer,Qt::LeftButton); auto *choices=computer->view(); QTRY_VERIFY(choices->isVisible());
            const auto target=computer->model()->index(computer->findData("mac"),0);
            choices->scrollTo(target);
            QTRY_VERIFY(choices->viewport()->rect().contains(choices->visualRect(target).center()));
            QTest::mouseMove(choices->viewport(),choices->visualRect(target).center());
            QTest::mouseClick(choices->viewport(),Qt::LeftButton,Qt::NoModifier,choices->visualRect(target).center());
            QTRY_COMPARE(computer->currentData().toString(),QString("mac")); popupChecked=true; menu->hide();
        });
        QTest::mouseClick(filters,Qt::LeftButton); QTRY_VERIFY(popupChecked); QCOMPARE(filters->text(),QString("Filters (1)"));
        QCOMPARE(tree->mapTo(&dialog,QPoint()).y(),workspaceTop); QVERIFY(local->viewport()->height()>=first.height()*3);
        auto *details=dialog.findChild<QPushButton *>("toggleWorktreeDetails"); QTest::mouseClick(details,Qt::LeftButton); QVERIFY(dialog.findChild<QWidget *>("worktreeSummaryArea")->isVisible()); QTest::mouseClick(details,Qt::LeftButton);
        tabs->setCurrentIndex(3); auto *sessions=dialog.findChild<QListWidget *>("worktreeRelatedSessions"); QCOMPARE(sessions->count(),1); QVERIFY(!sessions->item(0)->icon().isNull());
        QCOMPARE(sessions->item(0)->data(Qt::UserRole).toString(),QString("kimi/repo/two"));
        QVERIFY(!calls().contains("worktrees remove"));
    }
    void overviewReopeningUsesCachedCatalogReviewAndInspection(){
        SessionOrganization projects;const auto project=projects.createGroup("Work");projects.addFolder(project,"arch","/repo");
        HgsClient client(FakeCli::program(script()));
        {
            WorktreesDialog dialog(&client,projects,fleet(),false);dialog.show();
            QTRY_VERIFY(dialog.findChild<QPushButton *>("selectCleanupCandidates")->isEnabled());
            auto *tree=dialog.findChild<QTreeWidget *>("worktreeOverviewList");tree->setCurrentItem(overviewRow(tree,"/linked"));
            QTRY_VERIFY(dialog.findChild<QLabel *>("worktreeChangedFilesState")->text().contains("1 changed file"));
            QVERIFY(dialog.findChild<QLabel *>("worktreeOverviewStatus")->text().contains("Updated"));
        }
        const auto catalogs=calls().count("worktrees --path"),reviews=calls().count("worktrees review");
        const auto inspections=calls().count("worktrees inspect --path /linked");
        {
            WorktreesDialog dialog(&client,projects,fleet(),false);dialog.show();
            auto *tree=dialog.findChild<QTreeWidget *>("worktreeOverviewList");tree->setCurrentItem(overviewRow(tree,"/linked"));
            QTRY_VERIFY(dialog.findChild<QLabel *>("worktreeChangedFilesState")->text().contains("1 changed file"));
            QCOMPARE(calls().count("worktrees --path"),catalogs);QCOMPARE(calls().count("worktrees review"),reviews);
            QCOMPARE(calls().count("worktrees inspect --path /linked"),inspections);
            auto *refresh=dialog.findChild<QPushButton *>("refreshWorktreeOverview");QTRY_VERIFY(refresh->isEnabled());refresh->click();
            QTRY_VERIFY(calls().count("worktrees --path")>catalogs);QTRY_VERIFY(calls().count("worktrees review")>reviews);
            QTRY_VERIFY(dialog.findChild<QPushButton *>("selectCleanupCandidates")->isEnabled());
        }
        // An expired disk catalog triggers a new bounded scan on the next opening.
        QSettings settings;auto snapshots=QJsonDocument::fromJson(settings.value("workspace/worktreeSnapshots").toByteArray()).object();
        for(auto i=snapshots.begin();i!=snapshots.end();++i){auto data=i.value().toObject();data["received_at"]=QDateTime::currentSecsSinceEpoch()-3601;i.value()=data;}
        settings.setValue("workspace/worktreeSnapshots",QJsonDocument(snapshots).toJson());
        const auto before=calls().count("worktrees --path");WorktreesDialog expired(&client,projects,fleet(),false);expired.show();
        QTRY_VERIFY(calls().count("worktrees --path")>before);
    }
    void cachedBrowsingDoesNotExtendCleanupProof(){
        auto data=review();data["sampled_at"]=QDateTime::currentSecsSinceEpoch()-301;
        QFile file(dir.filePath("review.json"));QVERIFY(file.open(QIODevice::WriteOnly));file.write(QJsonDocument(data).toJson());file.close();
        HgsClient client(FakeCli::program(script()));QSignalSpy ready(&client,&HgsClient::worktreeReviewReady);
        client.reviewWorktrees({},"/repo/.git");QTRY_COMPARE(ready.size(),1);
        QVERIFY(client.worktreeReview({},"/repo/.git").isEmpty());QVERIFY(!client.worktreeReview({},"/repo/.git",3600).isEmpty());
        SessionOrganization projects;const auto project=projects.createGroup("Work");projects.addFolder(project,"arch","/repo");
        QSignalSpy catalogReady(&client,&HgsClient::worktreesReady);client.requestWorktrees({},"/repo");QTRY_COMPARE(catalogReady.size(),1);
        const auto before=calls().count("worktrees review");WorktreesDialog dialog(&client,projects,fleet(),false);dialog.show();
        auto *row=overviewRow(dialog.findChild<QTreeWidget *>("worktreeOverviewList"),"/linked");QVERIFY(row);
        QVERIFY(row->text(1).contains("Local files"));QVERIFY(row->flags()&Qt::ItemIsUserCheckable);
        dialog.findChild<QPushButton *>("selectCleanupCandidates")->click();QVERIFY(!dialog.findChild<QPushButton *>("reviewSelectedWorktrees")->isEnabled());
        QTest::qWait(200);QCOMPARE(calls().count("worktrees review"),before);QVERIFY(!calls().contains("worktrees remove"));
    }
    void inspectionStatesPaddingAndRetryAreExplicit(){
        for(const auto *name:{"empty-inspect","slow-inspect"}){QFile marker(dir.filePath(name));QVERIFY(marker.open(QIODevice::WriteOnly));}
        HgsClient client(FakeCli::program(script()));WorktreeInspectionPanel panel(&client);panel.resize(480,560);panel.show();
        panel.setContext("mac","Mac","/linked","/repo/.git",{{"ignored_count",0}}, {},true);
        auto *state=panel.findChild<QLabel *>("worktreeChangedFilesState");QVERIFY(state->text().contains("Loading"));
        auto *files=panel.findChild<QTreeWidget *>("worktreeChangedFiles");QVERIFY(files->mapTo(&panel,QPoint()).x()>=20);
        QVERIFY(!panel.findChild<QPlainTextEdit *>("worktreeChangesDiff")->toPlainText().contains("No tracked changes"));
        QTRY_COMPARE(state->text(),QString("No changed files."));QVERIFY(panel.findChild<QLabel *>("worktreeLocalFilesState")->text().contains("No local files"));
        QFile fail(dir.filePath("fail-inspect"));QVERIFY(fail.open(QIODevice::WriteOnly));fail.close();
        auto *refresh=panel.findChild<QPushButton *>("inspectWorktreeChanges");refresh->click();QTRY_VERIFY(state->text().contains("Could not refresh files. Showing previous data."));
        QVERIFY(panel.findChild<QLabel *>("worktreeInspectionStatus")->text().contains("fixture inspection unavailable"));
        QVERIFY(QFile::remove(fail.fileName()));QVERIFY(QFile::remove(dir.filePath("empty-inspect")));refresh->click();QTRY_COMPARE(files->topLevelItemCount(),1);
        QVERIFY(!calls().contains("worktrees remove"));
        const auto raster=qEnvironmentVariable("HGS_WORKTREE_PREVIEW");if(!raster.isEmpty())QVERIFY(panel.grab().save(raster+"/inspection-insets.png"));
    }
    void changedGitEvidenceRefreshesCachedInspection(){
        HgsClient client(FakeCli::program(script()));WorktreeInspectionPanel panel(&client);panel.show();
        panel.setContext("mac","Mac","/linked","/repo/.git",{{"fingerprint","first"}}, {},true);
        auto *state=panel.findChild<QLabel *>("worktreeChangedFilesState");QTRY_COMPARE(state->text(),QString("1 changed file"));
        QFile empty(dir.filePath("empty-inspect"));QVERIFY(empty.open(QIODevice::WriteOnly));empty.close();
        panel.setContext("mac","Mac","/linked","/repo/.git",{{"fingerprint","changed"}}, {},true);
        QTRY_COMPARE(state->text(),QString("No changed files."));QCOMPARE(calls().count("worktrees inspect"),2);
        WorktreeInspectionPanel reopened(&client);reopened.show();reopened.setContext("mac","Mac","/linked","/repo/.git",{{"fingerprint","changed"}}, {},true);
        QTRY_COMPARE(reopened.findChild<QLabel *>("worktreeChangedFilesState")->text(),QString("No changed files."));QCOMPARE(calls().count("worktrees inspect"),2);
    }
    void newCleanupInspectionRefreshesOldCacheInOnlyItsExactScope(){
        HgsClient client(FakeCli::program(script()));QSignalSpy ready(&client,&HgsClient::worktreeInspectionFinished);
        client.inspectWorktree("mac","/linked","/repo/.git");client.inspectWorktree({},"/linked","/repo/.git");
        QTRY_COMPARE(ready.size(),2);QCOMPARE(calls().count("worktrees inspect"),2);
        auto data=review();auto rows=data["worktrees"].toArray();auto row=rows[1].toObject();
        row["fingerprint"]="fresh-P2";row["ignored_count"]=0;row["ignored"]=QJsonArray{};row["ignored_bytes"]=0;rows.replace(1,row);data["worktrees"]=rows;
        QFile file(dir.filePath("review.json"));QVERIFY(file.open(QIODevice::WriteOnly));file.write(QJsonDocument(data).toJson());file.close();
        QFile empty(dir.filePath("empty-inspect"));QVERIFY(empty.open(QIODevice::WriteOnly));empty.close();
        WorktreeBatchCleanupDialog dialog(&client,{{{"host","mac"},{"machine","Mac"},{"common_dir","/repo/.git"},{"row",row}}});dialog.show();
        auto *inspect=dialog.findChild<QPushButton *>("inspectCleanupWorktree");QTRY_VERIFY(inspect->isEnabled());
        bool inspected=false;
        QTimer::singleShot(0,&dialog,[&]{auto *viewer=dialog.findChild<QDialog *>("cleanupFileInspection");QVERIFY(viewer);
            const auto close=qScopeGuard([&]{viewer->reject();});
            QTRY_COMPARE(viewer->findChild<QLabel *>("worktreeChangedFilesState")->text(),QString("No changed files."));
            QVERIFY(viewer->findChild<QLabel *>("worktreeLocalFilesState")->text().contains("No local files"));
            QCOMPARE(calls().count("worktrees inspect"),3);QCOMPARE(ready.last()[4].toJsonObject()["review"].toObject()["fingerprint"].toString(),QString("fresh-P2"));inspected=true;
        });
        inspect->click();QVERIFY(inspected);
        // Same path/common on another computer keeps its prior cached result.
        const auto count=ready.size();client.inspectWorktree({},"/linked","/repo/.git");QTRY_COMPARE(ready.size(),count+1);
        QCOMPARE(calls().count("worktrees inspect"),3);QCOMPARE(ready.last()[4].toJsonObject()["files"].toArray().size(),1);
        QVERIFY(!calls().contains("worktrees remove"));
    }
    void inspectionNavigationKeepsOnlyOneMetadataReadActive(){
        QFile slow(dir.filePath("slow-inspect"));QVERIFY(slow.open(QIODevice::WriteOnly));slow.close();
        HgsClient client(FakeCli::program(script()));WorktreeInspectionPanel panel(&client);panel.show();
        panel.setContext("mac","Mac","/first","/repo/.git",{}, {},true);
        QTRY_COMPARE(calls().count("worktrees inspect"),1);
        for(int i=0;i<10;++i)panel.setContext("mac","Mac",QString("/folder-%1").arg(i),"/repo/.git",{}, {},true);
        QTRY_COMPARE(panel.findChild<QLabel *>("worktreeChangedFilesState")->text(),QString("1 changed file"));
        QCOMPARE(calls().count("worktrees inspect"),2);QVERIFY(calls().contains("--path /folder-9"));QVERIFY(!calls().contains("--path /folder-8"));
    }
    void inspectionCacheScopesRefreshesAndRejectsLateInvalidation(){
        HgsClient client(FakeCli::program(script()));QSignalSpy ready(&client,&HgsClient::worktreeInspectionFinished);
        client.inspectWorktree("mac","/linked","/repo/.git","notes.txt");client.inspectWorktree("mac","/linked","/repo/.git","notes.txt");
        QTRY_COMPARE(ready.size(),2);QCOMPARE(calls().count("worktrees inspect"),1);
        client.inspectWorktree("mac","/linked","/repo/.git","notes.txt");QTRY_COMPARE(ready.size(),3);QCOMPARE(calls().count("worktrees inspect"),1);
        client.inspectWorktree({},"/linked","/repo/.git","notes.txt");QTRY_COMPARE(ready.size(),4);QCOMPARE(calls().count("worktrees inspect"),2);
        client.inspectWorktree("mac","/linked","/repo/.git",{},0,true);QTRY_COMPARE(ready.size(),5);
        client.inspectWorktree("mac","/linked","/repo/.git","notes.txt");QTRY_COMPARE(ready.size(),6);QCOMPARE(calls().count("worktrees inspect"),4);
        client.inspectWorktree("mac","/linked","/repo/.git","notes.txt");client.invalidateWorktreeCache("mac","/repo/.git");QTRY_COMPARE(ready.size(),7);QVERIFY(!ready.last()[5].toString().isEmpty());
        QFile slow(dir.filePath("slow-inspect"));QVERIFY(slow.open(QIODevice::WriteOnly));slow.close();
        client.inspectWorktree("mac","/linked","/repo/.git","notes.txt");QTRY_COMPARE(calls().count("worktrees inspect"),5);
        client.invalidateWorktreeCache("mac","/repo/.git");QTRY_COMPARE(ready.size(),8);QVERIFY(!ready.last()[5].toString().isEmpty());
        // Invalidating one computer never evicts another computer's same path.
        client.inspectWorktree({},"/linked","/repo/.git","notes.txt");QTRY_COMPARE(ready.size(),9);QCOMPARE(calls().count("worktrees inspect"),5);
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
    void automaticLaunchWithPartialCatalog_data(){
        QTest::addColumn<QString>("host");QTest::addColumn<QString>("evidence");QTest::addColumn<bool>("available");
        for(const auto &host:{QString(),QString("mac")}){
            const auto scope=host.isEmpty()?QByteArray("local-"):QByteArray("remote-");
            for(const auto *evidence:{"complete","included","omitted","alias"})QTest::newRow((scope+evidence).constData())<<host<<QString(evidence)<<true;
            for(const auto *evidence:{"stale","unknown","foreign","missing","bare"})QTest::newRow((scope+evidence).constData())<<host<<QString(evidence)<<false;
        }
    }
    void automaticLaunchWithPartialCatalog(){
        QFETCH(QString,host);QFETCH(QString,evidence);QFETCH(bool,available);
        auto data=catalog();data["partial"]=evidence!="complete";data["stale"]=evidence=="stale";
        const auto source=evidence=="missing"?QString("/missing"):evidence=="bare"?QString("/bare"):evidence=="alias"?QString("/alias"):QString("/repo");
        data["path"]=evidence=="alias"?"/repo/subfolder":source;
        data["selected_root"]=evidence=="unknown"?QJsonValue():QJsonValue(evidence=="missing"||evidence=="bare"?source:QString("/repo"));
        if(evidence=="omitted"||evidence=="foreign")data["worktrees"]=QJsonArray{QJsonObject{{"path","/another"},{"kind","linked"},{"available",true}}};
        if(evidence=="foreign")data["path"]="/another";
        const auto save=[&](const QJsonObject &value){QFile file(dir.filePath("catalog.json"));if(!file.open(QIODevice::WriteOnly))return false;return file.write(QJsonDocument(value).toJson())>0;};
        QVERIFY(save(data));
        SessionOrganization projects;const auto project=projects.createGroup("Work");projects.addFolder(project,host.isEmpty()?QString("arch"):host,source);
        NewSessionDialog dialog(FakeCli::program(script()),fleet(),host,"codex",project);dialog.setGroups(projects,project);dialog.show();
        QSignalSpy loaded(dialog.findChild<HgsClient *>(),&HgsClient::worktreesReady);QTRY_COMPARE(loaded.size(),1);
        auto *automatic=dialog.findChild<QPushButton *>("automaticLaunchWorktree"),*none=dialog.findChild<QPushButton *>("launchWithoutWorktree");
        QCOMPARE(automatic->isEnabled(),available);QCOMPARE(automatic->isChecked(),available);QCOMPARE(none->isChecked(),!available);
        QVERIFY(!calls().contains("worktrees create"));
        if(!available)return;
        // Moving to an ordinary folder clears Auto; returning uses the exact
        // computer/path snapshot, including a checkout omitted by the list cap.
        QVERIFY(save(QJsonObject{{"state","not_repo"},{"worktrees",QJsonArray{}}}));dialog.selectPath("/ordinary");QTRY_COMPARE(loaded.size(),2);
        QVERIFY(!automatic->isEnabled());QVERIFY(none->isChecked());QVERIFY(save(data));dialog.selectPath(source);QTRY_COMPARE(loaded.size(),3);
        QVERIFY(automatic->isEnabled());QVERIFY(automatic->isChecked());QVERIFY(!calls().contains("worktrees create"));
        if(evidence=="omitted"){
            QSignalSpy launched(&dialog,&NewSessionDialog::launchRequested);auto *start=dialog.findChild<QPushButton *>("primary");QTRY_VERIFY(start->isEnabled());start->click();QTRY_COMPARE(launched.size(),1);
            QCOMPARE(calls().count("worktrees create"),1);QVERIFY(launched.first()[2].toString().startsWith("/repo-work-"));QCOMPARE(launched.first()[0].toString(),host);
        }
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
        auto *cleanup=panel.findChild<QPushButton *>("cleanUpWorktrees");QTRY_COMPARE(cleanup->text(),QString("Clean up (1)"));QVERIFY(cleanup->isEnabled());
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
            QCOMPARE(list->topLevelItem(0)->checkState(0),Qt::Unchecked);list->topLevelItem(0)->setCheckState(0,Qt::Checked);QCOMPARE(list->topLevelItem(1)->checkState(0),Qt::Unchecked);
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
                box->findChild<QCheckBox *>("confirmIgnoredWorktreeFiles")->setChecked(true);box->findChild<QPushButton *>("confirmRemoveWorktrees")->click();});
            remove->click();
            auto *status=dialog->findChild<QLabel *>("worktreeCleanupStatus");QTRY_VERIFY(status->text().contains("Removed 1 worktree."));
            QVERIFY(status->text().contains("missing removal was not confirmed: Worktree changed since the review."));
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
            QCOMPARE(list->topLevelItem(0)->data(0,Qt::UserRole).toString(),QString("/linked"));QCOMPARE(list->topLevelItem(0)->checkState(0),Qt::Unchecked);
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
