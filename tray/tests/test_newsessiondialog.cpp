#include <QtTest>
#include <memory>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFile>
#include <QFrame>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPersistentModelIndex>
#include <QPushButton>
#include <QScrollBar>
#include <QTreeWidget>
#include <QUuid>
#include "NewSessionDialog.h"
#include "CloneRepositoryDialog.h"
#include "OpenCodeModelPicker.h"
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
    // The picker's row for a model value, in any group.
    static QTreeWidgetItem *row(QTreeWidget *tree,const QString &value) {
        for(int g=0;g<tree->topLevelItemCount();++g)
            for(int m=0;m<tree->topLevelItem(g)->childCount();++m)
                if(tree->topLevelItem(g)->child(m)->data(0,Qt::UserRole).toString()==value)return tree->topLevelItem(g)->child(m);
        return nullptr;
    }
    // Each call the fixture recorded in a file, one JSON line per call.
    QStringList lines(const QString &name) const {
        QFile f(m_fixture.filePath(name));if(!f.open(QIODevice::ReadOnly))return {};
        return QString::fromUtf8(f.readAll()).split('\n',Qt::SkipEmptyParts);
    }
private slots:
    void initTestCase() {
        QVERIFY(m_settings.isValid());QCoreApplication::setOrganizationName("hgs-tests");QCoreApplication::setApplicationName("new-session-dialog");
        QSettings::setDefaultFormat(QSettings::IniFormat);QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,m_settings.path());
    }
    void init() {
        QSettings().clear();
        for(const auto &name:{"fail","removed","slow-mac","released","missing","ended","quota-probes","mac-started","duplicates","other-scope","machine-accounts","slow-catalog-mac","catalog-started","empty-catalog","opencode","uncached","hold-probes","opencode-models","opencode-stopped","service-started","opencode-ids",
            "model-calls","favorite-calls","favorite-fails","models-stopped","old-hgs","favorites","slow-models","slow-favorite","favorite-started",
            "hold-next-models","models-held","project-inference","canonical-directory","clone-calls","clone-worktree","clone-uncertain","clone-receipt","prefill-linked"})flag(name,false);
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
# OpenCode's model catalog: its variants are each model's effort options.
def catalog():
 stopped=(root/'opencode-stopped').exists()
 many=['level-%d'%n for n in range(20)]
 models=[{'id':'fixture/local','value':'fixture/local','label':'Local fixture','provider':'fixture','provider_label':'fixture','free':True,
          'effort_options':['low','medium','high'],'effort_labels':['low','medium','high']},
         {'id':'fixture/plain','value':'fixture/plain','label':'Plain fixture','provider':'fixture','provider_label':'fixture','free':True,'effort_options':[],'effort_labels':[]},
         {'id':'fixture/other','value':'fixture/other','label':'Other fixture','provider':'fixture','provider_label':'fixture','free':False,
          'effort_options':['high','max','','bad value','x'*80,'max'],'effort_labels':['High','Maximum','','','','']},
         {'id':'fixture/many','value':'fixture/many','label':'Many fixture','provider':'fixture','provider_label':'fixture','free':False,'effort_options':many,'effort_labels':many}]
 if (root/'opencode-ids').exists():models.append(json.loads((root/'opencode-ids').read_text()))
 result={'model_options':models,'settings_model':'fixture/local','catalog_state':'not_running' if stopped else 'ready',
         'catalog_reason':'OpenCode is not running on '+(host or 'pc') if stopped else '','catalog_at':1791018000}
 # An older hgs has neither `account models` nor `account favorite`, and never reports this.
 if not (root/'old-hgs').exists():result['model_options_truncated']=False
 return result
# Waits, up to ten seconds, until the test releases the fixture.
def hold():
 for _ in range(500):
  if (root/'released').exists():break
  time.sleep(.02)
if a==['--help']:print('repository-clone-v1')
elif a[:1]==['repository']:
 def arg(k):return a[a.index(k)+1]
 with open(root/'clone-calls','a') as calls:calls.write(json.dumps([host]+a)+'\n')
 if a[1]=='check':print(json.dumps({'status':'ready','parent':'/workspace','path':'/workspace/'+arg('--name'),'name':arg('--name')}))
 elif a[1]=='clone':
  receipt={'status':'cloned','request_id':arg('--request-id'),'parent':arg('--parent'),'path':arg('--parent')+'/'+arg('--name'),'name':arg('--name')}
  (root/'clone-receipt').write_text(json.dumps(receipt))
  if (root/'clone-uncertain').exists():receipt['status']='unconfirmed'
  print(json.dumps(receipt))
 elif a[1]=='status':print((root/'clone-receipt').read_text())
elif a[:2]==['account','start']:
 with open(root/'service-started','a') as calls:calls.write(json.dumps([host]+a)+'\n')
 (root/'opencode-stopped').unlink(missing_ok=True)
 print(json.dumps(catalog()))
elif a[:2]==['account','ls']:
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
  if (root/'opencode-models').exists():profiles[-1].update(catalog())
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
elif a[:2]==['account','models']:
 with open(root/'model-calls','a') as calls:calls.write(json.dumps([host]+a)+'\n')
 if (root/'old-hgs').exists():sys.stderr.write('usage: hgs account ls | add ID --provider codex|claude|kimi --label NAME\n');sys.exit(1)
 location=a[a.index('--location')+1]
 if (root/'slow-models').exists() and location=='/workspace/example':hold()
 if (root/'models-stopped').exists():
  print(json.dumps({'catalog_state':'not_running','catalog_reason':'OpenCode is not running on '+(host or 'pc'),'model_options':[],'models':[],'providers':[],
                    'model_options_truncated':False,'providers_truncated':False,'models_truncated':False,'favorites':[]}));sys.exit(0)
 starred=json.loads((root/'favorites').read_text()) if (root/'favorites').exists() else []
 # Each folder has its own provider: this one's model only in /workspace/example.
 # /workspace/nolocal does not offer the account row's fixture/local.
 tag,name=('proj','Project model') if location=='/workspace/example' else ('other','Other folder model')
 short=[m for m in catalog()['model_options'] if location!='/workspace/nolocal' or m['value']!='fixture/local']
 short+=[{'id':'folderonly/'+tag,'value':'folderonly/'+tag,'label':name,'provider':'folderonly',
          'provider_label':'Folder-only fixture','free':False,'effort_options':[],'effort_labels':[]}]
 full=short+[{'id':'solo/solo-model','value':'solo/solo-model','label':'Solo model','provider':'solo','provider_label':'Solo fixture',
        'free':False,'effort_options':['low'],'effort_labels':['low']}]
 for entry in short+full:
  if entry['value'] in starred:entry['favorite']=True
 # hold-next-models: the next answer is built now, from this moment's favorites
 # and with a model no other answer lists, and printed only after the release.
 held=(root/'hold-next-models').exists() and not (root/'models-held').exists()
 if held:short=short+[{'id':'stale/old','value':'stale/old','label':'Stale model','provider':'stale','provider_label':'Stale fixture',
                       'free':False,'effort_options':[],'effort_labels':[]}]
 answer=json.dumps({'catalog_state':'ready','catalog_reason':'','location':location,'model_options':short,'models':full,
                    'providers':[{'id':'folderonly','label':'Folder-only fixture','kind':'config','credential_count':0,'model_count':1,'folder':True}],
                    'model_options_truncated':False,'providers_truncated':False,'models_truncated':False,'favorites':[{'value':v} for v in starred]})
 if held:(root/'models-held').touch();hold()
 print(answer)
elif a[:2]==['account','favorite']:
 with open(root/'favorite-calls','a') as calls:calls.write(json.dumps([host]+a)+'\n')
 (root/'favorite-started').touch()
 if (root/'slow-favorite').exists():hold()
 if (root/'favorite-fails').exists():sys.stderr.write('Account catalog is busy. Retry after the current operation finishes.\n');sys.exit(1)
 model=a[a.index('--model')+1];on=a[a.index('--favorite')+1]=='on'
 starred=json.loads((root/'favorites').read_text()) if (root/'favorites').exists() else []
 starred=[v for v in starred if v!=model]+([model] if on else [])
 (root/'favorites').write_text(json.dumps(starred))
 print(json.dumps({'status':'applied','request_id':a[a.index('--request-id')+1],'model':model,'favorite':on,'favorites':starred}))
elif a and a[0]=='dirs':
 answer={'path':(root/'canonical-directory').read_text() if (root/'canonical-directory').exists() else a[-1],'directories':[]}
 if (root/'project-inference').exists():answer['project_inference']={'schema':1,'swarm_id':'swarm-fixture','project_id':(root/'project-inference').read_text()}
 print(json.dumps(answer))
elif a and a[0]=='worktrees':
 if (root/'prefill-linked').exists():
  path=a[a.index('--path')+1]
  print(json.dumps({'state':'ok','path':path,'selected_root':path,'common_dir':'/workspace/example/.git',
   'worktrees':[{'path':'/workspace/example','kind':'main','available':True},{'path':'/workspace/example-work-one','kind':'linked','available':True}]}))
 elif (root/'clone-worktree').exists() and (root/'clone-calls').exists():
  def arg(k):return a[a.index(k)+1]
  if len(a)>1 and a[1]=='create':print(json.dumps({'status':'created','request_id':arg('--request-id'),'branch':arg('--branch'),'common_dir':arg('--common-dir'),'path':arg('--destination')}))
  else:print(json.dumps({'state':'ok','stale':False,'partial':False,'selected_root':'/workspace/fresh','common_dir':'/workspace/fresh/.git','worktrees':[{'path':'/workspace/fresh','kind':'main','available':True,'branch':'main'}]}))
 else:print(json.dumps({'state':'not_repo','worktrees':[]}))
)PY"));
    }
    void cloneCreatesProjectAndStartsOnSelectedComputer_data(){
        QTest::addColumn<QString>("host");QTest::addColumn<bool>("worktree");
        QTest::newRow("local-folder")<<QString()<<false;QTest::newRow("remote-folder")<<QString("mac")<<false;
        QTest::newRow("local-worktree")<<QString()<<true;QTest::newRow("remote-worktree")<<QString("mac")<<true;
    }
    void cloneCreatesProjectAndStartsOnSelectedComputer(){
        QFETCH(QString,host);QFETCH(bool,worktree);if(worktree)flag("clone-worktree");SessionOrganization projects;const auto initial=projects.createGroup("Existing");
        NewSessionDialog dialog(program(),fleet(),host,"codex",initial,nullptr,nullptr,&projects);dialog.setGroups(projects,initial);dialog.show();
        auto *source=dialog.findChild<QPushButton *>("launchRepositorySource");QVERIFY(source);source->click();
        QVERIFY(!dialog.findChild<CloneRepositoryDialog *>());
        dialog.findChild<QLineEdit *>("launchRepositoryUrl")->setText("git@example.invalid:team/fresh.git");
        dialog.findChild<QLineEdit *>("launchRepositoryDestination")->setText("/workspace/fresh");
        auto *projectChoice=dialog.findChild<QComboBox *>("launchProject");projectChoice->setCurrentIndex(projectChoice->count()-1);
        QCOMPARE(dialog.findChild<QLineEdit *>("launchNewProjectName")->text(),QString("fresh"));
        QSignalSpy launched(&dialog,&NewSessionDialog::launchRequested);QSignalSpy changed(&dialog,&NewSessionDialog::organizationChanged);
        auto *start=dialog.findChild<QPushButton *>("primary");QTRY_VERIFY(start->isEnabled());QVERIFY(lines("clone-calls").isEmpty());start->click();
        QTRY_COMPARE(launched.size(),1);QCOMPARE(changed.size(),1);
        const auto args=launched.first();QCOMPARE(args[0].toString(),host);
        if(worktree)QVERIFY(args[2].toString().startsWith("/workspace/fresh-work-"));else QCOMPARE(args[2].toString(),"/workspace/fresh");
        const auto *project=projects.group(args[5].toString());QVERIFY(project);QCOMPARE(project->name,"fresh");QCOMPARE(project->folders.size(),1);
        QCOMPARE(project->folders.first().machine,host.isEmpty()?QString("pc"):host);QCOMPARE(project->folders.first().path,"/workspace/fresh");
    }
    void cloneConfirmationKeepsItsProjectOverNativeInference_data(){
        QTest::addColumn<QString>("host");QTest::addColumn<bool>("inbox");QTest::addColumn<bool>("worktree");
        QTest::newRow("local-inbox")<<QString()<<true<<false;
        QTest::newRow("remote-inbox")<<QString("mac")<<true<<false;
        QTest::newRow("local-inbox-worktree")<<QString()<<true<<true;
        QTest::newRow("remote-inbox-worktree")<<QString("mac")<<true<<true;
        QTest::newRow("local-existing")<<QString()<<false<<false;
        QTest::newRow("remote-existing")<<QString("mac")<<false<<false;
    }
    void cloneConfirmationKeepsItsProjectOverNativeInference(){
        QFETCH(QString,host);QFETCH(bool,inbox);QFETCH(bool,worktree);
        if(worktree)flag("clone-worktree");
        SessionOrganization projects;const auto existing=projects.createGroup("Existing");
        const auto competing=projects.createGroup("Native folder project");
        QFile hint(m_fixture.filePath("project-inference"));QVERIFY(hint.open(QIODevice::WriteOnly));hint.write(competing.toUtf8());hint.close();
        // Enter from the ordinary untouched Inbox; Clone confirmation itself
        // makes even an unchanged Inbox selection explicit before launch.
        NewSessionDialog dialog(program(),fleet(),host,"codex",{},nullptr,nullptr,&projects);
        dialog.setGroups(projects,"ungrouped");dialog.setProjectExplicit(false);dialog.setProjectCatalogSwarm("swarm-fixture");dialog.show();
        QVERIFY(dialog.implicitInbox());
        dialog.findChild<QPushButton *>("launchRepositorySource")->click();
        const auto expected=inbox?QString("ungrouped"):existing;
        auto *project=dialog.findChild<QComboBox *>("launchProject");project->setCurrentIndex(project->findData(expected));
        dialog.findChild<QLineEdit *>("launchRepositoryUrl")->setText("git@example.invalid:team/fresh.git");
        dialog.findChild<QLineEdit *>("launchRepositoryDestination")->setText("/workspace/fresh");
        QSignalSpy launched(&dialog,&NewSessionDialog::launchRequested);
        auto *start=dialog.findChild<QPushButton *>("primary");QTRY_VERIFY(start->isEnabled());QVERIFY(lines("clone-calls").isEmpty());start->click();QTRY_COMPARE(launched.size(),1);
        QCOMPARE(launched[0][0].toString(),host);QCOMPARE(launched[0][5].toString(),expected);QVERIFY(!dialog.implicitInbox());
        if(worktree)QVERIFY(launched[0][2].toString().startsWith("/workspace/fresh-work-"));else QCOMPARE(launched[0][2].toString(),QString("/workspace/fresh"));
        const auto *chosen=projects.group(expected);QVERIFY(chosen);QCOMPARE(chosen->folders.size(),1);
        QCOMPARE(chosen->folders.first().machine,host.isEmpty()?QString("pc"):host);QCOMPARE(chosen->folders.first().path,QString("/workspace/fresh"));
        QVERIFY(projects.group(competing)->folders.isEmpty());
    }
    void repositorySourceKeepsCommonChoicesAndManualDestination(){
        SessionOrganization projects;const auto project=projects.createGroup("Example");projects.addFolder(project,"pc","/workspace/example");
        NewSessionDialog dialog(program(),fleet(),{},"codex",project);dialog.setGroups(projects,project);dialog.show();
        auto *source=dialog.findChild<QPushButton *>("launchRepositorySource"),*folder=dialog.findChild<QPushButton *>("launchFolderSource");
        auto *sessionName=dialog.findChild<QLineEdit *>("launchName");sessionName->setText("keep-this-session");source->click();
        auto *url=dialog.findChild<QLineEdit *>("launchRepositoryUrl"),*destination=dialog.findChild<QLineEdit *>("launchRepositoryDestination");
        url->setText("https://github.com/example/first.git");QCOMPARE(destination->text(),QString("~/first"));
        url->setText("git@example.invalid:team/second.git");QCOMPARE(destination->text(),QString("~/second"));
        destination->setFocus();destination->selectAll();QTest::keyClicks(destination,"/workspace/custom");
        url->setText("git@example.invalid:team/third.git");QCOMPARE(destination->text(),QString("/workspace/custom"));
        dialog.findChild<QPushButton *>("launchWithoutWorktree")->click();folder->click();
        QVERIFY(dialog.findChild<QComboBox *>("launchProjectFolder")->isVisible());QVERIFY(!url->isVisible());source->click();
        QCOMPARE(url->text(),QString("git@example.invalid:team/third.git"));QCOMPARE(destination->text(),QString("/workspace/custom"));
        QCOMPARE(dialog.findChild<QComboBox *>("launchProject")->currentData().toString(),project);QCOMPARE(sessionName->text(),QString("keep-this-session"));
        QVERIFY(dialog.findChild<QPushButton *>("launchWithoutWorktree")->isChecked());QVERIFY(!dialog.findChild<CloneRepositoryDialog *>());QVERIFY(lines("clone-calls").isEmpty());
        QTRY_VERIFY(dialog.findChild<QPushButton *>("primary")->isEnabled());
        const auto preview=qEnvironmentVariable("ZERUS_PREVIEW_DIR");if(!preview.isEmpty())QVERIFY(dialog.grab().save(preview+"/new-session-repository.png"));
    }
    void repositorySourceKeepsNewProjectIntent_data(){
        QTest::addColumn<QString>("host");QTest::addColumn<QString>("overrideProject");
        for(const auto &host:{QString(),QString("mac")}){
            const auto prefix=host.isEmpty()?"local-":"remote-";
            QTest::newRow(qPrintable(prefix+QString("keep-new")))<<host<<QString();
            QTest::newRow(qPrintable(prefix+QString("choose-existing")))<<host<<QString("existing");
            QTest::newRow(qPrintable(prefix+QString("choose-inbox")))<<host<<QString("ungrouped");
        }
    }
    void repositorySourceKeepsNewProjectIntent(){
        QFETCH(QString,host);QFETCH(QString,overrideProject);
        SessionOrganization projects;const auto existing=projects.createGroup("Existing");
        NewSessionDialog dialog(program(),fleet(),host,"codex",existing);dialog.setGroups(projects,existing);dialog.show();
        auto *repository=dialog.findChild<QPushButton *>("launchRepositorySource"),*folder=dialog.findChild<QPushButton *>("launchFolderSource");
        auto *project=dialog.findChild<QComboBox *>("launchProject");auto *name=dialog.findChild<QLineEdit *>("launchNewProjectName");
        repository->click();dialog.findChild<QLineEdit *>("launchRepositoryUrl")->setText("git@example.invalid:team/fresh.git");
        project->setCurrentIndex(project->count()-1);QVERIFY(name->isVisible());name->selectAll();QTest::keyClicks(name,"My new project");
        folder->click();QVERIFY(!name->isVisible());QVERIFY(!project->currentData().toString().isEmpty());
        // A repeated source click must not erase the remembered repository choice.
        folder->click();
        if(!overrideProject.isEmpty()){
            const int at=project->findData(overrideProject=="existing"?existing:overrideProject);QVERIFY(at>=0);project->setCurrentIndex(at);
            // User activation is explicit even when Inbox is already the fallback.
            QVERIFY(QMetaObject::invokeMethod(project,"activated",Qt::DirectConnection,Q_ARG(int,at)));
        }
        repository->click();
        QCOMPARE(name->isVisible(),overrideProject.isEmpty());QCOMPARE(name->text(),QString("My new project"));
        if(!overrideProject.isEmpty())QCOMPARE(project->currentData().toString(),overrideProject=="existing"?existing:overrideProject);
        else QVERIFY(project->currentData().toString().isEmpty());
        QVERIFY(lines("clone-calls").isEmpty());
    }
    void repositorySourceKeepsOpaqueProjectId(){
        const QString project="new-repository-project";
        SessionOrganization projects(QJsonObject{{"projects",QJsonArray{QJsonObject{{"id",project},{"name","Existing project"}}}},{"default_project",project}});
        NewSessionDialog dialog(program(),fleet(),{},"codex",project,nullptr,nullptr,&projects);dialog.setGroups(projects,project);dialog.show();
        dialog.findChild<QPushButton *>("launchRepositorySource")->click();dialog.findChild<QLineEdit *>("launchRepositoryUrl")->setText("git@example.invalid:team/fresh.git");
        dialog.findChild<QLineEdit *>("launchRepositoryDestination")->setText("/workspace/fresh");QVERIFY(!dialog.findChild<QLineEdit *>("launchNewProjectName")->isVisible());
        QSignalSpy launched(&dialog,&NewSessionDialog::launchRequested);auto *start=dialog.findChild<QPushButton *>("primary");QTRY_VERIFY(start->isEnabled());start->click();QTRY_COMPARE(launched.size(),1);
        QCOMPARE(launched[0][5].toString(),project);QCOMPARE(projects.groups().size(),1);QCOMPARE(projects.group(project)->folders.first().path,QString("/workspace/fresh"));
    }
    void repositoryFormFitsWorkspace_data(){QTest::addColumn<QString>("theme");QTest::newRow("dark")<<QString("dark");QTest::newRow("light")<<QString("light");}
    void sourceSwitchKeepsControls_data(){
        QTest::addColumn<QString>("theme");QTest::addColumn<QString>("state");
        for(const auto &theme:{QString("dark"),QString("light")})
            for(const auto &state:{QString("existing"),QString("new-project"),QString("recovery")})
                QTest::newRow(qPrintable(theme+'-'+state))<<theme<<state;
    }
    void sourceSwitchKeepsControls(){
        QFETCH(QString,theme);QFETCH(QString,state);QSettings().setValue("workspace/theme",theme);
        if(state=="recovery"){
            const QJsonArray source{program(),QString()};
            const auto key="workspace/repositoryClone/"+QString::fromLatin1(QJsonDocument(source).toJson(QJsonDocument::Compact).toHex());
            const QJsonObject pending{{"request_id",QUuid::createUuid().toString(QUuid::WithoutBraces)},{"url","git@example.invalid:team/fresh.git"},{"parent","/workspace"},{"name","fresh"},{"path","/workspace/fresh"}};
            QSettings().setValue(key+"/pending",QJsonDocument(pending).toJson(QJsonDocument::Compact));
        }
        SessionOrganization projects;const auto project=projects.createGroup("Example");projects.addFolder(project,"pc","/workspace/example");
        SessionsWindow workspace(program());workspace.setFleet(fleet());
        NewSessionDialog dialog(program(),fleet(),{},"codex",project,&workspace);dialog.setGroups(projects,project);dialog.resize(600,850);dialog.show();
        auto *folder=dialog.findChild<QPushButton *>("launchFolderSource"),*repository=dialog.findChild<QPushButton *>("launchRepositorySource");
        dialog.findChild<QPushButton *>("launchWithoutWorktree")->click();QTRY_VERIFY(dialog.findChild<QPushButton *>("refreshLaunchAccounts")->isEnabled());
        if(state!="existing")repository->click();
        if(state=="new-project"){
            auto *choice=dialog.findChild<QComboBox *>("launchProject");choice->setCurrentIndex(choice->count()-1);
            auto *name=dialog.findChild<QLineEdit *>("launchNewProjectName");name->selectAll();QTest::keyClicks(name,"Keep my project");
        }
        QList<QWidget *> controls;
        for(const auto *name:{"launchFolderSource","launchRepositorySource","automaticLaunchWorktree","launchWithoutWorktree","launchWorktreeOptions","launchAccount","refreshLaunchAccounts","launchName","launchOpenTerminal","primary"})controls<<dialog.findChild<QWidget *>(name);
        controls<<dialog.findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Cancel);
        const auto capture=[&]{QList<QRect> positions;for(auto *w:controls)positions<<QRect(w->mapTo(&dialog,QPoint()),w->size());return positions;};
        QTest::qWait(30);const auto before=capture();const auto size=dialog.size();
        for(int i=0;i<4;++i){
            (repository->isChecked()?folder:repository)->click();QTest::qWait(30);
            const auto after=capture();for(qsizetype at=0;at<controls.size();++at)QCOMPARE(after[at],before[at]);QCOMPARE(dialog.size(),size);
        }
        QVERIFY(lines("clone-calls").isEmpty());
    }
    void sourceSwitchKeepsActionGeometry_data(){QTest::addColumn<QString>("theme");QTest::newRow("dark")<<QString("dark");QTest::newRow("light")<<QString("light");}
    void sourceSwitchKeepsActionGeometry(){
        QFETCH(QString,theme);QSettings().setValue("workspace/theme",theme);
        SessionsWindow workspace(program());workspace.setFleet(fleet());NewSessionDialog dialog(program(),fleet(),{},"codex",{},&workspace);dialog.resize(600,850);dialog.show();
        auto *start=dialog.findChild<QPushButton *>("primary");QTest::qWait(30);const auto before=start->geometry();
        dialog.findChild<QPushButton *>("launchRepositorySource")->click();QTest::qWait(30);QCOMPARE(start->geometry(),before);
        QVERIFY(lines("clone-calls").isEmpty());
    }
    void repositoryFormFitsWorkspace(){
        QFETCH(QString,theme);QSettings().setValue("workspace/theme",theme);
        SessionOrganization projects;const auto project=projects.createGroup("Example repository with a long descriptive project name");projects.addFolder(project,"pc","/workspace/example");
        SessionsWindow workspace(program());workspace.setFleet(fleet());
        NewSessionDialog dialog(program(),fleet(),{},"codex",project,&workspace);dialog.setGroups(projects,project);dialog.resize(600,730);dialog.show();
        dialog.findChild<QPushButton *>("launchRepositorySource")->click();dialog.findChild<QLineEdit *>("launchRepositoryUrl")->setText("https://github.com/example/repository.git");
        dialog.findChild<QLineEdit *>("launchRepositoryDestination")->setText("~/Projects/repository");QTRY_VERIFY(dialog.findChild<QPushButton *>("primary")->isEnabled());
        QVERIFY(dialog.width()<=660);QVERIFY(dialog.height()<=900);
        for(const auto *name:{"launchProject","launchComputer","launchFolderSource","launchRepositorySource","launchRepositoryUrl","launchRepositoryDestination","browseRepositoryDestination","launchName","primary"}){
            const auto *widget=dialog.findChild<QWidget *>(name);QVERIFY(widget);QVERIFY(widget->isVisible());QVERIFY(dialog.rect().contains(QRect(widget->mapTo(&dialog,QPoint()),widget->size())));
        }
        QVERIFY(!dialog.findChild<CloneRepositoryDialog *>());QVERIFY(lines("clone-calls").isEmpty());
        const auto preview=qEnvironmentVariable("ZERUS_PREVIEW_DIR");if(!preview.isEmpty())QVERIFY(dialog.grab().save(preview+"/new-session-repository-"+theme+".png"));
        dialog.findChild<QPushButton *>("launchFolderSource")->click();QVERIFY(dialog.findChild<QComboBox *>("launchProjectFolder")->isVisible());
        for(const auto *label:dialog.findChildren<QLabel *>())if(label->isVisible()&&label->text()=="Working folder")
            QVERIFY(label->width()>=label->fontMetrics().horizontalAdvance(label->text()));
        if(!preview.isEmpty())QVERIFY(dialog.grab().save(preview+"/new-session-folder-"+theme+".png"));
    }
    void inlineRecoveryReadsReceiptAndRequiresExplicitStart_data(){QTest::addColumn<QString>("host");QTest::newRow("local")<<QString();QTest::newRow("remote")<<QString("mac");}
    void repositorySourceIgnoresLateFolderPrefill(){
        flag("prefill-linked");SessionOrganization projects;const auto project=projects.createGroup("Example");projects.addFolder(project,"pc","/workspace/example");
        NewSessionDialog dialog(program(),fleet(),{},"codex",project);dialog.setGroups(projects,project);dialog.show();
        QSignalSpy loaded(dialog.findChild<HgsClient *>(),&HgsClient::worktreesReady);
        dialog.prefillSessionFolder("/workspace/example-work-one");dialog.findChild<QPushButton *>("launchRepositorySource")->click();
        dialog.findChild<QLineEdit *>("launchRepositoryUrl")->setText("git@example.invalid:team/fresh.git");
        dialog.findChild<QLineEdit *>("launchRepositoryDestination")->setText("/workspace/keep-name");
        const auto linkedReplies=[&]{int n=0;for(const auto &reply:loaded)if(reply[2].toString()=="/workspace/example-work-one")++n;return n;};
        QTRY_COMPARE(linkedReplies(),2);QCOMPARE(dialog.findChild<QLabel *>("launchFolderPath")->text(),QString("/workspace/example-work-one"));
        QCOMPARE(dialog.findChild<QLineEdit *>("launchRepositoryDestination")->text(),QString("/workspace/keep-name"));QVERIFY(lines("clone-calls").isEmpty());
        dialog.findChild<QPushButton *>("launchFolderSource")->click();QCOMPARE(dialog.findChild<QLabel *>("launchFolderPath")->text(),QString("/workspace/example-work-one"));
    }
    void inlineRecoveryReadsReceiptAndRequiresExplicitStart(){
        QFETCH(QString,host);flag("clone-uncertain");SessionOrganization projects;const auto project=projects.createGroup("Example");
        {
            NewSessionDialog dialog(program(),fleet(),host,"codex",project);dialog.setGroups(projects,project);dialog.show();
            dialog.findChild<QPushButton *>("launchRepositorySource")->click();dialog.findChild<QLineEdit *>("launchRepositoryUrl")->setText("git@example.invalid:team/fresh.git");
            dialog.findChild<QLineEdit *>("launchRepositoryDestination")->setText("/workspace/fresh");
            QSignalSpy launched(&dialog,&NewSessionDialog::launchRequested);auto *start=dialog.findChild<QPushButton *>("primary");QTRY_VERIFY(start->isEnabled());start->click();
            QTRY_VERIFY(dialog.findChild<QLabel *>("launchRepositoryStatus")->text().contains("still running"));QCOMPARE(launched.size(),0);QVERIFY(!start->isEnabled());
        }
        const auto mutations=[&]{int n=0;for(const auto &line:lines("clone-calls"))if(QJsonDocument::fromJson(line.toUtf8()).array().at(2)=="clone")++n;return n;};
        QCOMPARE(mutations(),1);
        NewSessionDialog restored(program(),fleet(),host,"codex",project);restored.setGroups(projects,project);restored.show();restored.findChild<QPushButton *>("launchRepositorySource")->click();
        QCOMPARE(restored.findChild<QLineEdit *>("launchRepositoryDestination")->text(),QString("/workspace/fresh"));QVERIFY(!restored.findChild<QLineEdit *>("launchRepositoryUrl")->isEnabled());
        auto *projectChoice=restored.findChild<QComboBox *>("launchProject");projectChoice->setCurrentIndex(projectChoice->count()-1);
        QCOMPARE(restored.findChild<QLineEdit *>("launchNewProjectName")->text(),QString("fresh"));projectChoice->setCurrentIndex(projectChoice->findData(project));
        QSignalSpy launched(&restored,&NewSessionDialog::launchRequested);restored.findChild<QPushButton *>("launchRepositoryCheckResult")->click();
        auto *start=restored.findChild<QPushButton *>("primary");QTRY_VERIFY(start->isEnabled());QCOMPARE(start->text(),QString("Use checkout and start"));QCOMPARE(launched.size(),0);QCOMPARE(mutations(),1);
        start->click();QTRY_COMPARE(launched.size(),1);QCOMPARE(launched[0][0].toString(),host);QCOMPARE(launched[0][2].toString(),QString("/workspace/fresh"));QCOMPARE(mutations(),1);
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
    void untouchedInboxUsesNativeFolderInferenceAndExplicitInboxWins() {
        for(int explicitChoice=0;explicitChoice<3;++explicitChoice){
            SessionOrganization projects;const auto project=projects.createGroup("Work");
            QFile hint(m_fixture.filePath("project-inference"));QVERIFY(hint.open(QIODevice::WriteOnly));hint.write(project.toUtf8());hint.close();
            NewSessionDialog dialog(program(),fleet(),{},"codex");dialog.setGroups(projects,"ungrouped");
            dialog.setProjectCatalogSwarm("swarm-fixture");
            dialog.setProjectExplicit(explicitChoice==1);dialog.selectPath("/workspace/example",false);dialog.show();
            auto *choice=dialog.findChild<QComboBox *>("launchProject");QVERIFY(choice);
            if(explicitChoice==2)QVERIFY(QMetaObject::invokeMethod(choice,"activated",Q_ARG(int,choice->currentIndex())));
            QSignalSpy launched(&dialog,&NewSessionDialog::launchRequested);auto *start=dialog.findChild<QPushButton *>("primary");
            QTRY_VERIFY(start->isEnabled());start->click();QTRY_COMPARE(launched.size(),1);
            QCOMPARE(launched[0][5].toString(),explicitChoice?QString("ungrouped"):project);
            if(!explicitChoice)QVERIFY(launched[0][7].toString().isEmpty()); // already belongs; no duplicate folder
        }
    }
    void unknownNativeProjectInferenceDoesNotChangeTheChosenProject() {
        QFile hint(m_fixture.filePath("project-inference"));QVERIFY(hint.open(QIODevice::WriteOnly));hint.write("unknown-project");hint.close();
        SessionOrganization projects;const auto project=projects.createGroup("Local project");
        QVERIFY(!projects.addFolder(project,"pc","/workspace/example").isEmpty());
        NewSessionDialog dialog(program(),fleet(),{},"codex");dialog.setGroups(projects,"ungrouped");
        dialog.setProjectCatalogSwarm("swarm-fixture");
        dialog.selectPath("/workspace/example",false);QSignalSpy launched(&dialog,&NewSessionDialog::launchRequested);
        auto *start=dialog.findChild<QPushButton *>("primary");QTRY_VERIFY(start->isEnabled());start->click();QTRY_COMPARE(launched.size(),1);
        QCOMPARE(launched[0][5].toString(),QString("ungrouped"));
    }
    void anotherOrUnknownSwarmWithTheSameProjectIdDoesNotInferMembership() {
        for(const auto &expectedSwarm : {QString(),QString("another-swarm")}) {
            SessionOrganization projects;const auto project=projects.createGroup("Local project");
            QVERIFY(!projects.addFolder(project,"pc","/workspace/example").isEmpty());
            QFile hint(m_fixture.filePath("project-inference"));QVERIFY(hint.open(QIODevice::WriteOnly));hint.write(project.toUtf8());hint.close();
            NewSessionDialog dialog(program(),fleet(),{},"codex");dialog.setGroups(projects,"ungrouped");
            dialog.setProjectCatalogSwarm(expectedSwarm);dialog.selectPath("/workspace/example",false);
            QSignalSpy launched(&dialog,&NewSessionDialog::launchRequested);auto *start=dialog.findChild<QPushButton *>("primary");
            QTRY_VERIFY(start->isEnabled());start->click();QTRY_COMPARE(launched.size(),1);
            QCOMPARE(launched[0][5].toString(),QString("ungrouped"));
        }
    }
    void savedCanonicalFolderInfersOnlyOneProjectOnTheSelectedComputer_data() {
        QTest::addColumn<bool>("windows");QTest::addColumn<QString>("savedHost");
        QTest::addColumn<QString>("savedPath");QTest::addColumn<QString>("selectedPath");
        QTest::addColumn<QString>("canonicalPath");QTest::addColumn<bool>("collision");
        QTest::addColumn<int>("explicitChoice");QTest::addColumn<bool>("inferred");
        QTest::newRow("Windows drive, case and separators")
            <<true<<"pc"<<"C:/Work/Example"<<"c:/work/example"<<"C:\\Work\\Example"<<false<<0<<true;
        QTest::newRow("Windows UNC, case and separators")
            <<true<<"pc"<<"//Server/Share/Example"<<"//server/share/example"<<"\\\\SERVER\\Share\\example"<<false<<0<<true;
        QTest::newRow("non-Git exact folder")
            <<false<<"pc"<<"/workspace/example"<<"/workspace/example"<<"/workspace/example"<<false<<0<<true;
        QTest::newRow("saved canonical target, requested alias")
            <<false<<"pc"<<"/workspace/real"<<"/workspace/alias"<<"/workspace/real"<<false<<0<<true;
        QTest::newRow("saved alias is not canonical evidence")
            <<false<<"pc"<<"/workspace/alias"<<"/workspace/alias"<<"/workspace/real"<<false<<0<<false;
        QTest::newRow("a child is not exact membership")
            <<false<<"pc"<<"/workspace"<<"/workspace/example"<<"/workspace/example"<<false<<0<<false;
        QTest::newRow("a different Windows drive")
            <<true<<"pc"<<"D:/work/example"<<"C:/work/example"<<"C:/work/example"<<false<<0<<false;
        QTest::newRow("same path, another computer")
            <<false<<"mac"<<"/workspace/example"<<"/workspace/example"<<"/workspace/example"<<false<<0<<false;
        QTest::newRow("Windows path, another computer")
            <<true<<"mac"<<"C:/work/example"<<"C:/work/example"<<"C:/work/example"<<false<<0<<false;
        QTest::newRow("same Windows path, two projects")
            <<true<<"pc"<<"C:/Work/Example"<<"c:/work/example"<<"C:/Work/Example"<<true<<0<<false;
        QTest::newRow("explicit Windows Inbox")
            <<true<<"pc"<<"C:/work/example"<<"C:/work/example"<<"C:/work/example"<<false<<1<<false;
        QTest::newRow("explicit other Windows project")
            <<true<<"pc"<<"C:/work/example"<<"C:/work/example"<<"C:/work/example"<<false<<2<<false;
    }
    void savedCanonicalFolderInfersOnlyOneProjectOnTheSelectedComputer() {
        QFETCH(bool,windows);QFETCH(QString,savedHost);QFETCH(QString,savedPath);QFETCH(QString,selectedPath);
        QFETCH(QString,canonicalPath);QFETCH(bool,collision);QFETCH(int,explicitChoice);QFETCH(bool,inferred);
        LocalSessionHost::setOwnWindowsSessionForTests(1);
        SessionOrganization projects;const auto project=projects.createGroup("Folder owner");
        const auto other=projects.createGroup("Other project");
        const auto style=windows?MachinePlatform::Style::Windows:MachinePlatform::Style::Posix;
        QVERIFY(!projects.addFolder(project,savedHost,savedPath,{},style).isEmpty());
        if(collision)QVERIFY(!projects.addFolder(other,savedHost,MachinePlatform::native(savedPath,style).toLower(),{},style).isEmpty());
        QFile canonical(m_fixture.filePath("canonical-directory"));QVERIFY(canonical.open(QIODevice::WriteOnly));
        canonical.write(canonicalPath.toUtf8());canonical.close();
        NewSessionDialog dialog(program(),windows?windowsFleet(1):fleet(),{},"codex");
        dialog.setGroups(projects,explicitChoice==2?other:QString("ungrouped"));
        dialog.setProjectExplicit(explicitChoice!=0);dialog.selectPath(selectedPath,false);dialog.show();
        auto *choice=dialog.findChild<QComboBox *>("launchProject");QVERIFY(choice);
        if(explicitChoice==1)QVERIFY(QMetaObject::invokeMethod(choice,"activated",Q_ARG(int,choice->currentIndex())));
        QSignalSpy launched(&dialog,&NewSessionDialog::launchRequested);auto *start=dialog.findChild<QPushButton *>("primary");
        QTRY_VERIFY(start->isEnabled());start->click();QTRY_COMPARE(launched.size(),1);
        QCOMPARE(launched[0][5].toString(),inferred?project:explicitChoice==2?other:QString("ungrouped"));
        QCOMPARE(launched[0][2].toString(),canonicalPath);
        if(inferred)QVERIFY(launched[0][7].toString().isEmpty());
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
    // Effort is the selected OpenCode model's variants: offered only for a model
    // that has them, and dropped when the next model does not offer it.
    void openCodeEffortFollowsSelectedModel() {
        flag("opencode");flag("opencode-models");
        NewSessionDialog dialog(program(),fleet(),{},"opencode");dialog.selectPath("/workspace/example",false);dialog.show();
        auto *models=dialog.findChild<QComboBox *>("launchModel"),*effort=dialog.findChild<QComboBox *>("launchEffort");
        QVERIFY(models);QVERIFY(effort);QTRY_VERIFY(models->findData("fixture/local")>=0);
        QCOMPARE(choices(dialog)->currentItem()->data(Qt::UserRole).toString(),QString("native-opencode"));
        auto *button=dialog.findChild<QPushButton *>("launchModelButton");QVERIFY(button);
        QVERIFY(button->isVisible());QCOMPARE(models->currentData().toString(),QString());QVERIFY(!effort->isVisible());
        const auto offered=[&]{QStringList result;for(int i=0;i<effort->count();++i)result<<effort->itemText(i);return result;};
        models->setCurrentIndex(models->findData("fixture/local"));QVERIFY(effort->isVisible());
        QCOMPARE(offered(),QStringList({"Default","low","medium","high"}));QCOMPARE(effort->currentData().toString(),QString());
        QCOMPARE(effort->itemData(0).toString(),QString());
        effort->setCurrentIndex(effort->findData("high"));
        // Only valid, distinct variants, at most 16, keep their labels.
        models->setCurrentIndex(models->findData("fixture/other"));QVERIFY(effort->isVisible());
        QCOMPARE(offered(),QStringList({"Default","High","Maximum"}));QCOMPARE(effort->currentData().toString(),QString("high"));
        effort->setCurrentIndex(effort->findData("max"));
        models->setCurrentIndex(models->findData("fixture/local"));QCOMPARE(effort->currentData().toString(),QString());
        models->setCurrentIndex(models->findData("fixture/many"));QCOMPARE(effort->count(),17);QCOMPARE(effort->itemData(16).toString(),QString("level-15"));
        effort->setCurrentIndex(effort->findData("level-3"));
        // A model without variants hides effort and clears the value.
        models->setCurrentIndex(models->findData("fixture/plain"));QVERIFY(!effort->isVisible());QCOMPARE(effort->currentData().toString(),QString());
        models->setCurrentIndex(models->findData("fixture/many"));QCOMPARE(effort->currentData().toString(),QString());
        // Other agents have no effort here.
        choices(dialog)->setCurrentRow(row(dialog,"work"));QVERIFY(!effort->isVisible());QVERIFY(!button->isVisible());
        choices(dialog)->setCurrentRow(row(dialog,"native-opencode"));QTRY_VERIFY(models->findData("fixture/local")>=0);
        models->setCurrentIndex(models->findData("fixture/local"));effort->setCurrentIndex(effort->findData("high"));
        auto *start=dialog.findChild<QPushButton *>("primary");QTRY_VERIFY(start->isEnabled());
        QSignalSpy launched(&dialog,&NewSessionDialog::launchRequested);start->click();QTRY_COMPARE(launched.size(),1);
        QCOMPARE(launched[0][1].toString(),QString("opencode"));QCOMPARE(launched[0][4].toString(),QString("native-opencode"));
        QCOMPARE(launched[0][8].toString(),QString("fixture/local"));QCOMPARE(launched[0][9].toString(),QString("high"));
    }
    // A stopped service keeps the last known models and offers the explicit start.
    void openCodeStartsStoppedServiceExplicitly() {
        flag("opencode");flag("opencode-models");flag("opencode-stopped");
        NewSessionDialog dialog(program(),fleet(),"mac","opencode");dialog.selectPath("/workspace/example",false);dialog.show();
        auto *models=dialog.findChild<QComboBox *>("launchModel");auto *service=dialog.findChild<QPushButton *>("launchStartOpenCode");
        auto *reason=dialog.findChild<QLabel *>("launchCatalogStatus");QVERIFY(models);QVERIFY(service);QVERIFY(reason);
        QTRY_VERIFY(service->isVisible());QCOMPARE(service->text(),QString("Start OpenCode on mac"));QVERIFY(service->isEnabled());
        QVERIFY(reason->isVisible());QCOMPARE(reason->text(),QString("OpenCode is not running on mac"));
        QVERIFY(models->findData("fixture/local")>=0);QVERIFY(!QFile::exists(m_fixture.filePath("service-started")));
        // Every row fits with the service notice, the model and its effort shown.
        models->setCurrentIndex(models->findData("fixture/local"));QVERIFY(dialog.findChild<QComboBox *>("launchEffort")->isVisible());
        auto *primary=dialog.findChild<QPushButton *>("primary");QTRY_VERIFY(primary->geometry().bottom()<dialog.height());
        QVERIFY(service->mapTo(&dialog,QPoint(service->width(),0)).x()<=dialog.width());
        const auto preview=qEnvironmentVariable("ZERUS_OPENCODE_LAUNCH_PREVIEW");if(!preview.isEmpty())QVERIFY(dialog.grab().save(preview));
        service->click();QTRY_VERIFY(QFile::exists(m_fixture.filePath("service-started")));
        QFile calls(m_fixture.filePath("service-started"));QVERIFY(calls.open(QIODevice::ReadOnly));
        QCOMPARE(QJsonDocument::fromJson(calls.readAll().trimmed()).array(),QJsonArray({"mac","account","start","native-opencode","--json"}));
        // Success refreshes this computer's catalog.
        QTRY_VERIFY(!service->isVisible());QTRY_VERIFY(!reason->isVisible());QVERIFY(models->findData("fixture/local")>=0);
        // Other agents never offer it.
        flag("opencode-stopped");dialog.findChild<QPushButton *>("refreshLaunchAccounts")->click();
        QTRY_VERIFY(service->isVisible());choices(dialog)->setCurrentRow(row(dialog,"work"));QVERIFY(!service->isVisible());QVERIFY(!reason->isVisible());
    }
    // The shared variant shape rule (tests/fixtures/opencode-effort-ids.json).
    void openCodeEffortIdsFollowSharedRule() {
        QFile file(QStringLiteral(ZERUS_OPENCODE_EFFORT_IDS));QVERIFY(file.open(QIODevice::ReadOnly));
        const auto fixture=QJsonDocument::fromJson(file.readAll()).object();
        const auto accept=fixture.value("accept").toArray(),reject=fixture.value("reject").toArray();QVERIFY(accept.size()>=10);QVERIFY(reject.size()>=13);
        QJsonArray ids;for(const auto &id:reject)ids.append(id);for(const auto &id:accept)ids.append(id);
        QFile model(m_fixture.filePath("opencode-ids"));QVERIFY(model.open(QIODevice::WriteOnly));
        model.write(QJsonDocument(QJsonObject{{"id","fixture/ids"},{"value","fixture/ids"},{"label","Ids fixture"},{"provider","fixture"},
            {"effort_options",ids},{"effort_labels",ids}}).toJson(QJsonDocument::Compact));model.close();
        flag("opencode");flag("opencode-models");
        NewSessionDialog dialog(program(),fleet(),{},"opencode");dialog.selectPath("/workspace/example",false);dialog.show();
        auto *models=dialog.findChild<QComboBox *>("launchModel"),*effort=dialog.findChild<QComboBox *>("launchEffort");
        QTRY_VERIFY(models->findData("fixture/ids")>=0);models->setCurrentIndex(models->findData("fixture/ids"));
        QStringList offered;for(int i=0;i<effort->count();++i)offered<<effort->itemData(i).toString();
        QStringList expected{QString()};for(const auto &id:accept)expected<<id.toString();
        QCOMPARE(offered,expected);
        for(const auto &id:reject)QVERIFY2(id.toString().isEmpty()||effort->findData(id.toString())<0,qPrintable(id.toString()));
        // An accepted id with a colon and a multibyte one launch as given.
        auto *start=dialog.findChild<QPushButton *>("primary");QTRY_VERIFY(start->isEnabled());
        effort->setCurrentIndex(effort->findData("think:deep"));QSignalSpy launched(&dialog,&NewSessionDialog::launchRequested);
        start->click();QTRY_COMPARE(launched.size(),1);QCOMPARE(launched[0][9].toString(),QString("think:deep"));
    }
    // As in Accounts, an offline computer cannot be asked to start its service.
    void openCodeStartNeedsOnlineComputer() {
        flag("opencode");flag("opencode-models");flag("opencode-stopped");
        auto state=fleet();BoxState mac;mac.host="mac";mac.ok=false;state.setPeer(mac,QDateTime::currentMSecsSinceEpoch());
        NewSessionDialog dialog(program(),state,"mac","opencode");dialog.selectPath("/workspace/example",false);dialog.show();
        auto *service=dialog.findChild<QPushButton *>("launchStartOpenCode");QVERIFY(service);
        QTRY_VERIFY(service->isVisible());QTRY_VERIFY(!dialog.findChild<QLabel *>("launchAccountStatus")->text().contains("Loading"));
        QVERIFY(!service->isEnabled());service->click();QTest::qWait(100);QVERIFY(!QFile::exists(m_fixture.filePath("service-started")));
        mac.ok=true;state.setPeer(mac,QDateTime::currentMSecsSinceEpoch());dialog.setFleet(state);QTRY_VERIFY(service->isEnabled());
    }
    // The model button opens the grouped picker with the selected folder's catalog.
    void openCodePickerUsesTheSelectedFolderCatalog() {
        flag("opencode");flag("opencode-models");
        NewSessionDialog dialog(program(),fleet(),"mac","opencode");dialog.selectPath("/workspace/example",false);dialog.show();
        auto *button=dialog.findChild<QPushButton *>("launchModelButton");QTRY_VERIFY(button&&button->isVisible()&&button->isEnabled());
        QCOMPARE(button->text(),QString("Native default model"));
        button->click();
        auto *picker=dialog.findChild<OpenCodeModelPicker *>("launchModelPicker");QVERIFY(picker);QTRY_VERIFY(picker->isVisible());
        QTRY_VERIFY(picker->values().contains("folderonly/proj"));
        QCOMPARE(QJsonDocument::fromJson(lines("model-calls").value(0).toUtf8()).array(),
                 QJsonArray({"mac","account","models","native-opencode","--location","/workspace/example","--json"}));
        auto *tree=picker->findChild<QTreeWidget *>("openCodeModelList");bool folder=false;
        for(int g=0;g<tree->topLevelItemCount();++g)folder|=tree->topLevelItem(g)->text(0)=="Folder-only fixture · This folder";
        QVERIFY(folder);
        picker->select("folderonly/proj");QTRY_VERIFY(!picker->isVisible());QCOMPARE(button->text(),QString("Project model"));
        QVERIFY(!dialog.findChild<QComboBox *>("launchEffort")->isVisible());
        auto *start=dialog.findChild<QPushButton *>("primary");QTRY_VERIFY(start->isEnabled());
        QSignalSpy launched(&dialog,&NewSessionDialog::launchRequested);start->click();QTRY_COMPARE(launched.size(),1);
        QCOMPARE(launched[0][8].toString(),QString("folderonly/proj"));QCOMPARE(launched[0][9].toString(),QString());
    }
    // A model the account row does not list, from the short list or All models,
    // stays chosen with its effort through an account refresh.
    void openCodeChoicesOutsideTheRowSurviveAnAccountRefresh() {
        flag("opencode");flag("opencode-models");
        NewSessionDialog dialog(program(),fleet(),{},"opencode");dialog.selectPath("/workspace/example",false);dialog.show();
        auto *button=dialog.findChild<QPushButton *>("launchModelButton");QTRY_VERIFY(button->isEnabled());button->click();
        auto *picker=dialog.findChild<OpenCodeModelPicker *>("launchModelPicker");QTRY_VERIFY(picker->values().contains("folderonly/proj"));
        picker->select("folderonly/proj");QTRY_COMPARE(button->text(),QString("Project model"));
        const int before=lines("model-calls").size();
        dialog.findChild<QPushButton *>("refreshLaunchAccounts")->click();
        QTest::qWait(300);
        QCOMPARE(button->text(),QString("Project model"));
        QCOMPARE(dialog.findChild<QComboBox *>("launchModel")->currentData().toString(),QString("folderonly/proj"));
        button->click();QTRY_VERIFY(picker->values().contains("folderonly/proj"));QCOMPARE(lines("model-calls").size(),before);
        // An All models choice is kept the same way, with its efforts.
        picker->findChild<QPushButton *>("openCodeAllModels")->click();picker->select("solo/solo-model");
        auto *effort=dialog.findChild<QComboBox *>("launchEffort");QTRY_VERIFY(effort->isVisible());effort->setCurrentIndex(effort->findData("low"));
        dialog.findChild<QPushButton *>("refreshLaunchAccounts")->click();QTest::qWait(300);
        QCOMPARE(button->text(),QString("Solo model"));QCOMPARE(effort->currentData().toString(),QString("low"));
        auto *start=dialog.findChild<QPushButton *>("primary");QTRY_VERIFY(start->isEnabled());
        QSignalSpy launched(&dialog,&NewSessionDialog::launchRequested);start->click();QTRY_COMPARE(launched.size(),1);
        QCOMPARE(launched[0][8].toString(),QString("solo/solo-model"));QCOMPARE(launched[0][9].toString(),QString("low"));
    }
    void openCodeAllModelsSearchesTheFolderCatalog() {
        flag("opencode");flag("opencode-models");
        NewSessionDialog dialog(program(),fleet(),{},"opencode");dialog.selectPath("/workspace/example",false);dialog.show();
        auto *button=dialog.findChild<QPushButton *>("launchModelButton");QTRY_VERIFY(button->isEnabled());button->click();
        auto *picker=dialog.findChild<OpenCodeModelPicker *>("launchModelPicker");QTRY_VERIFY(picker->values().contains("folderonly/proj"));
        picker->findChild<QPushButton *>("openCodeAllModels")->click();
        picker->findChild<QLineEdit *>("openCodeModelSearch")->setText("solo");
        QTRY_COMPARE(picker->values(),QStringList({"solo/solo-model"}));
        QCOMPARE(lines("model-calls").size(),1);   // the folder catalog is fetched once
        picker->select("solo/solo-model");auto *effort=dialog.findChild<QComboBox *>("launchEffort");
        QVERIFY(effort->isVisible());QCOMPARE(effort->count(),2);
    }
    void openCodeStarSendsOneFavoriteRequestWithIdentity() {
        flag("opencode");flag("opencode-models");flag("slow-models");
        NewSessionDialog dialog(program(),fleet(),{},"opencode");dialog.selectPath("/workspace/example",false);dialog.show();
        auto *button=dialog.findChild<QPushButton *>("launchModelButton");QTRY_VERIFY(button->isEnabled());button->click();
        auto *picker=dialog.findChild<OpenCodeModelPicker *>("launchModelPicker");QTRY_VERIFY(picker->values().contains("fixture/plain"));
        emit picker->favoriteRequested("fixture/plain",true);
        QTRY_COMPARE(lines("favorite-calls").size(),1);
        const auto args=QJsonDocument::fromJson(lines("favorite-calls")[0].toUtf8()).array();
        QCOMPARE(args.toVariantList().mid(0,8),QVariantList({"","account","favorite","native-opencode","--model","fixture/plain","--favorite","on"}));
        QCOMPARE(args[8].toString(),QString("--request-id"));QVERIFY(!QUuid::fromString(args[9].toString()).isNull());
        QCOMPARE(args[10].toString(),QString("--json"));
        // The folder catalog is fetched again, and it marks the favorite.
        QTRY_COMPARE(lines("model-calls").size(),2);
        QVERIFY(!picker->values().contains("folderonly/proj"));
        flag("released");
        QTRY_VERIFY(picker->values().contains("folderonly/proj"));
        auto *tree=picker->findChild<QTreeWidget *>("openCodeModelList");QTRY_COMPARE(row(tree,"fixture/plain")->text(1),QString("★"));
        flag("favorite-fails");
        emit picker->favoriteRequested("fixture/other",true);
        auto *error=dialog.findChild<QLabel *>("launchError");
        QTRY_VERIFY(error->text().startsWith("Could not change the favorite on pc:"));
        QCOMPARE(row(tree,"fixture/other")->text(1),QString("☆"));
        // A failure is never sent again on its own.
        QTest::qWait(200);QCOMPARE(lines("favorite-calls").size(),2);
    }
    void openCodeStoppedServiceKeepsTheRowListAndNeverStarts() {
        flag("opencode");flag("opencode-models");flag("models-stopped");
        NewSessionDialog dialog(program(),fleet(),{},"opencode");dialog.selectPath("/workspace/example",false);dialog.show();
        auto *button=dialog.findChild<QPushButton *>("launchModelButton");QTRY_VERIFY(button->isEnabled());button->click();
        auto *picker=dialog.findChild<OpenCodeModelPicker *>("launchModelPicker");
        QTRY_COMPARE(picker->findChild<QLabel *>("openCodeModelNotice")->text(),QString("OpenCode is not running on pc"));
        QVERIFY(picker->values().contains("fixture/local"));
        QVERIFY(!QFile::exists(m_fixture.filePath("service-started")));
        // Opening again shows that answer instead of asking again.
        dialog.findChild<QFrame *>("launchModelPopup")->hide();button->click();QTRY_VERIFY(picker->isVisible());
        QTest::qWait(200);QCOMPARE(lines("model-calls").size(),1);QVERIFY(!QFile::exists(m_fixture.filePath("service-started")));
    }
    void openCodeOlderComputerOffersNoStarsOrAllModels() {
        flag("opencode");flag("opencode-models");flag("old-hgs");
        NewSessionDialog dialog(program(),fleet(),"mac","opencode");dialog.selectPath("/workspace/example",false);dialog.show();
        auto *button=dialog.findChild<QPushButton *>("launchModelButton");QTRY_VERIFY(button->isEnabled());button->click();
        auto *picker=dialog.findChild<OpenCodeModelPicker *>("launchModelPicker");QTRY_VERIFY(picker->values().contains("fixture/local"));
        QTest::qWait(200);
        QVERIFY(lines("model-calls").isEmpty());   // never asks an older hgs
        auto *all=picker->findChild<QPushButton *>("openCodeAllModels");QVERIFY(!all->isEnabled());
        QCOMPARE(all->toolTip(),QString("Update Zerus on mac to search all models and star favorites."));
        QSignalSpy favorites(picker,&OpenCodeModelPicker::favoriteRequested);
        auto *tree=picker->findChild<QTreeWidget *>("openCodeModelList");
        emit tree->itemClicked(row(tree,"fixture/plain"),1);QCOMPARE(favorites.size(),0);
        QCOMPARE(row(tree,"fixture/plain")->toolTip(1),QString("Update Zerus on mac to search all models and star favorites."));
        picker->select("fixture/local");
        auto *start=dialog.findChild<QPushButton *>("primary");QTRY_VERIFY(start->isEnabled());
        QVERIFY(lines("favorite-calls").isEmpty());
    }
    // A folder change closes and resets the picker and fills it for the new folder
    // in the same turn. A late catalog of the previous folder is kept for that
    // folder only: it never shows in the next one and is reused when it returns.
    void openCodeLateFolderCatalogStaysWithItsFolder() {
        flag("opencode");flag("opencode-models");flag("slow-models");
        NewSessionDialog dialog(program(),fleet(),{},"opencode");dialog.selectPath("/workspace/example",false);dialog.show();
        auto *button=dialog.findChild<QPushButton *>("launchModelButton");QTRY_VERIFY(button->isEnabled());button->click();
        auto *picker=dialog.findChild<OpenCodeModelPicker *>("launchModelPicker");QTRY_VERIFY(picker->values().contains("fixture/local"));
        QTRY_COMPARE(lines("model-calls").size(),1);
        QSignalSpy answers(dialog.findChild<HgsClient *>(),&HgsClient::openCodeModelsReady);
        auto *popup=dialog.findChild<QFrame *>("launchModelPopup");QVERIFY(popup->isVisible());
        dialog.selectPath("/workspace/other",false);
        QVERIFY(!popup->isVisible());QVERIFY(picker->values().contains("fixture/local"));
        QVERIFY(!picker->values().contains("folderonly/proj"));QCOMPARE(picker->current(),QString());
        QCOMPARE(button->text(),QString("Native default model"));
        flag("released");QTRY_COMPARE(answers.size(),1);QTest::qWait(100);
        QVERIFY(!picker->values().contains("folderonly/proj"));
        button->click();QTRY_VERIFY(picker->values().contains("folderonly/other"));
        QVERIFY(!picker->values().contains("folderonly/proj"));QCOMPARE(lines("model-calls").size(),2);
        // Back in the first folder, its late catalog is shown without another request.
        popup->hide();dialog.selectPath("/workspace/example",false);
        QVERIFY(picker->values().contains("folderonly/proj"));QVERIFY(!picker->values().contains("folderonly/other"));
        QCOMPARE(lines("model-calls").size(),2);
    }
    // A computer change clears the choice, the pending star and the list in the
    // same turn. The previous computer's late favorite answer never marks this picker.
    void openCodeLateFavoriteAnswerStaysWithItsComputer() {
        flag("opencode");flag("opencode-models");flag("slow-favorite");
        NewSessionDialog dialog(program(),fleet(),{},"opencode");dialog.selectPath("/workspace/example",false);dialog.show();
        auto *button=dialog.findChild<QPushButton *>("launchModelButton");QTRY_VERIFY(button->isEnabled());button->click();
        auto *picker=dialog.findChild<OpenCodeModelPicker *>("launchModelPicker");QTRY_VERIFY(picker->values().contains("folderonly/proj"));
        picker->select("folderonly/proj");QTRY_COMPARE(button->text(),QString("Project model"));
        button->click();QTRY_VERIFY(picker->isVisible());
        auto *tree=picker->findChild<QTreeWidget *>("openCodeModelList");
        QSignalSpy answers(dialog.findChild<HgsClient *>(),&HgsClient::openCodeFavoriteReady);
        emit picker->favoriteRequested("fixture/plain",true);QCOMPARE(row(tree,"fixture/plain")->text(1),QString("…"));
        QTRY_VERIFY(QFile::exists(m_fixture.filePath("favorite-started")));
        auto *computer=dialog.findChild<QComboBox *>("launchComputer");computer->setCurrentIndex(computer->findData(QString("mac")));
        QVERIFY(!dialog.findChild<QFrame *>("launchModelPopup")->isVisible());
        QCOMPARE(button->text(),QString("Native default model"));QCOMPARE(picker->current(),QString());
        QVERIFY(!picker->values().contains("folderonly/proj"));
        QTRY_VERIFY(button->isEnabled());button->click();QTRY_VERIFY(picker->values().contains("fixture/plain"));
        QCOMPARE(row(tree,"fixture/plain")->text(1),QString("☆"));
        flag("released");QTRY_COMPARE(answers.size(),1);QTest::qWait(200);
        QCOMPARE(row(tree,"fixture/plain")->text(1),QString("☆"));
        QVERIFY(!dialog.findChild<QLabel *>("launchError")->text().startsWith("Could not change the favorite"));
        QCOMPARE(lines("favorite-calls").size(),1);
    }
    // A model chosen from All models stays the current one through its own star
    // and unstar, while the folder's catalog is read again after each change.
    void openCodeCurrentModelSurvivesItsFavoriteChanges() {
        flag("opencode");flag("opencode-models");
        NewSessionDialog dialog(program(),fleet(),{},"opencode");dialog.selectPath("/workspace/example",false);dialog.show();
        auto *button=dialog.findChild<QPushButton *>("launchModelButton");QTRY_VERIFY(button->isEnabled());button->click();
        auto *picker=dialog.findChild<OpenCodeModelPicker *>("launchModelPicker");QTRY_VERIFY(picker->values().contains("folderonly/proj"));
        picker->findChild<QPushButton *>("openCodeAllModels")->click();picker->select("solo/solo-model");
        QTRY_COMPARE(button->text(),QString("Solo model"));button->click();QTRY_VERIFY(picker->isVisible());
        auto *tree=picker->findChild<QTreeWidget *>("openCodeModelList");
        for(const bool on:{true,false}){
            const int calls=lines("model-calls").size();
            emit picker->favoriteRequested("solo/solo-model",on);
            QTRY_COMPARE(lines("model-calls").size(),calls+1);
            QTRY_COMPARE(row(tree,"solo/solo-model")->text(1),QString(on?"★":"☆"));
        }
        // The catalog read after the star never overrides the later unstar.
        QTest::qWait(300);QCOMPARE(row(tree,"solo/solo-model")->text(1),QString("☆"));
        QCOMPARE(picker->current(),QString("solo/solo-model"));QCOMPARE(button->text(),QString("Solo model"));
        QCOMPARE(dialog.findChild<QComboBox *>("launchModel")->currentData().toString(),QString("solo/solo-model"));
        QCOMPARE(lines("favorite-calls").size(),2);
        auto *start=dialog.findChild<QPushButton *>("primary");QTRY_VERIFY(start->isEnabled());
        QSignalSpy launched(&dialog,&NewSessionDialog::launchRequested);start->click();QTRY_COMPARE(launched.size(),1);
        QCOMPARE(launched[0][8].toString(),QString("solo/solo-model"));
    }
    // Only the newest read of a folder decides. An older answer, built before a
    // later favorite change and released after the newer answer was applied,
    // changes neither the catalog, the star nor the current model.
    void openCodeOlderFolderAnswerNeverReplacesANewerOne() {
        flag("opencode");flag("opencode-models");
        NewSessionDialog dialog(program(),fleet(),{},"opencode");dialog.selectPath("/workspace/example",false);dialog.show();
        auto *button=dialog.findChild<QPushButton *>("launchModelButton");QTRY_VERIFY(button->isEnabled());button->click();
        auto *picker=dialog.findChild<OpenCodeModelPicker *>("launchModelPicker");QTRY_VERIFY(picker->values().contains("folderonly/proj"));
        picker->select("folderonly/proj");QTRY_COMPARE(button->text(),QString("Project model"));
        button->click();QTRY_VERIFY(picker->isVisible());
        auto *tree=picker->findChild<QTreeWidget *>("openCodeModelList");
        QSignalSpy answers(dialog.findChild<HgsClient *>(),&HgsClient::openCodeModelsReady);
        // The star's folder read is built at once, with the favorite on, and held.
        flag("hold-next-models");emit picker->favoriteRequested("fixture/plain",true);
        QTRY_VERIFY(QFile::exists(m_fixture.filePath("models-held")));QCOMPARE(row(tree,"fixture/plain")->text(1),QString("★"));
        // The unstar's newer read answers first and is applied.
        emit picker->favoriteRequested("fixture/plain",false);
        QTRY_COMPARE(lines("favorite-calls").size(),2);QTRY_COMPARE(lines("model-calls").size(),3);QTRY_COMPARE(answers.size(),1);
        QTRY_COMPARE(row(tree,"fixture/plain")->text(1),QString("☆"));QVERIFY(!picker->values().contains("stale/old"));
        // The older answer arrives last and is dropped.
        flag("released");QTRY_COMPARE(answers.size(),2);QTest::qWait(200);
        QCOMPARE(row(tree,"fixture/plain")->text(1),QString("☆"));QVERIFY(!picker->values().contains("stale/old"));
        QVERIFY(picker->values().contains("folderonly/proj"));
        QCOMPARE(picker->current(),QString("folderonly/proj"));QCOMPARE(button->text(),QString("Project model"));
        QCOMPARE(dialog.findChild<QComboBox *>("launchModel")->currentData().toString(),QString("folderonly/proj"));
        // The folder's stored catalog is the newer one too.
        dialog.findChild<QFrame *>("launchModelPopup")->hide();
        dialog.selectPath("/workspace/other",false);dialog.selectPath("/workspace/example",false);
        QVERIFY(picker->values().contains("folderonly/proj"));QVERIFY(!picker->values().contains("stale/old"));
        QCOMPARE(row(tree,"fixture/plain")->text(1),QString("☆"));QCOMPARE(lines("model-calls").size(),3);
    }
    // A folder change keeps the chosen model only when the new folder's ready
    // catalog offers it, even when the account row lists that model everywhere:
    // a folder without a ready catalog, or one that does not offer it, starts
    // from the native default, and the rejected choice does not come back.
    void openCodeFolderChangeKeepsOnlyAChoiceTheNewFolderOffers() {
        flag("opencode");flag("opencode-models");
        NewSessionDialog dialog(program(),fleet(),{},"opencode");dialog.selectPath("/workspace/other",false);dialog.show();
        auto *button=dialog.findChild<QPushButton *>("launchModelButton");
        auto *picker=dialog.findChild<OpenCodeModelPicker *>("launchModelPicker");
        auto *popup=dialog.findChild<QFrame *>("launchModelPopup");auto *model=dialog.findChild<QComboBox *>("launchModel");
        // Read two folders first: one offers the row's fixture/local, one does not.
        QTRY_VERIFY(button->isEnabled());button->click();
        QTRY_VERIFY(picker->values().contains("folderonly/other"));QVERIFY(picker->values().contains("fixture/local"));popup->hide();
        dialog.selectPath("/workspace/nolocal",false);button->click();
        QTRY_VERIFY(picker->values().contains("folderonly/other"));QVERIFY(!picker->values().contains("fixture/local"));popup->hide();
        // Uncached: a folder without a ready catalog does not take the choice.
        dialog.selectPath("/workspace/example",false);button->click();QTRY_VERIFY(picker->values().contains("folderonly/proj"));
        picker->select("fixture/local");QTRY_COMPARE(button->text(),QString("Local fixture"));
        dialog.selectPath("/workspace/fresh",false);
        QCOMPARE(button->text(),QString("Native default model"));QCOMPARE(model->currentData().toString(),QString());
        QCOMPARE(picker->current(),QString());
        dialog.selectPath("/workspace/example",false);
        QCOMPARE(button->text(),QString("Native default model"));QCOMPARE(picker->current(),QString());
        // Not offering: a ready catalog without the model does not take it either.
        button->click();QTRY_VERIFY(picker->isVisible());picker->select("fixture/local");QTRY_COMPARE(button->text(),QString("Local fixture"));
        dialog.selectPath("/workspace/nolocal",false);
        QCOMPARE(button->text(),QString("Native default model"));QCOMPARE(model->currentData().toString(),QString());
        QCOMPARE(picker->current(),QString());
        // Offering: a ready catalog that lists it keeps the choice, and the session starts with it.
        dialog.selectPath("/workspace/example",false);button->click();QTRY_VERIFY(picker->isVisible());
        picker->select("fixture/local");QTRY_COMPARE(button->text(),QString("Local fixture"));
        dialog.selectPath("/workspace/other",false);
        QCOMPARE(button->text(),QString("Local fixture"));QCOMPARE(model->currentData().toString(),QString("fixture/local"));
        QCOMPARE(picker->current(),QString("fixture/local"));
        QCOMPARE(lines("model-calls").size(),3);   // each folder was read once
        auto *start=dialog.findChild<QPushButton *>("primary");QTRY_VERIFY(start->isEnabled());
        QSignalSpy launched(&dialog,&NewSessionDialog::launchRequested);start->click();QTRY_COMPARE(launched.size(),1);
        QCOMPARE(launched[0][2].toString(),QString("/workspace/other"));QCOMPARE(launched[0][8].toString(),QString("fixture/local"));
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
