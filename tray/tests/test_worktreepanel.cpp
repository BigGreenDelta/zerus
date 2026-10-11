#include <QtTest>
#include <QTemporaryDir>
#include <QFile>
#include <QSettings>
#include <QComboBox>
#include <QLineEdit>
#include <QTimer>
#include <QCheckBox>
#include <QMenu>
#include <QMessageBox>
#include <QPointer>
#include "WorktreePanel.h"
#include "NewSessionDialog.h"

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
        BoxState local;local.ok=true;local.host="arch";SessionInfo a;a.name="codex/repo/one";a.gitRoot="/repo";a.canonicalCwd="/repo/subfolder";local.sessions={a};
        BoxState remote=local;remote.host="mac";remote.sessions[0].name="kimi/repo/two";
        FleetState result;result.setLocal(local,QDateTime::currentMSecsSinceEpoch());result.setPeer(remote,QDateTime::currentMSecsSinceEpoch());return result;
    }
private slots:
    void initTestCase(){
        QCoreApplication::setOrganizationName("hgs-tests");QCoreApplication::setApplicationName("worktree-panel");QSettings::setDefaultFormat(QSettings::IniFormat);QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,dir.path());
        QFile response(dir.filePath("catalog.json"));QVERIFY(response.open(QIODevice::WriteOnly));response.write(QJsonDocument(catalog()).toJson());response.close();
        QFile reviewed(dir.filePath("review.json"));QVERIFY(reviewed.open(QIODevice::WriteOnly));reviewed.write(QJsonDocument(review()).toJson());reviewed.close();
        QFile program(script());QVERIFY(program.open(QIODevice::WriteOnly));program.write("#!/bin/sh\ncd \"$(dirname \"$0\")\"\nprintf '%s\\n' \"$*\" >> calls\ncase \"$1\" in @*) shift;; esac\ncase \"$1\" in\nworktrees) if [ \"$2\" = create ]; then shift 2; while [ $# -gt 1 ]; do case \"$1\" in --request-id) token=$2;; --branch) branch=$2;; --destination) destination=$2;; esac; shift 2; done; sleep 0.05; if [ -f fail-create ]; then echo 'Branch already exists' >&2; exit 1; fi; printf '{\"status\":\"created\",\"request_id\":\"%s\",\"branch\":\"%s\",\"path\":\"%s\",\"common_dir\":\"/repo/.git\",\"machine\":\"fixture\"}' \"$token\" \"$branch\" \"$destination\"; "
            "elif [ \"$2\" = review ]; then sleep 0.05; if [ -f old-hgs ]; then echo 'usage: hgs worktrees --path PATH [--refresh] [--json]' >&2; exit 1; fi; cat review.json; "
            "elif [ \"$2\" = remove ]; then shift 2; while [ $# -gt 0 ]; do case \"$1\" in --request-id) token=$2; shift;; --path) target=$2; shift;; --common-dir|--fingerprint) shift;; esac; shift; done; sleep 0.05; "
            "if grep -qxF \"$target\" fail-remove 2>/dev/null; then echo 'Worktree changed since the review. Review it again.' >&2; exit 1; fi; "
            "printf '{\"status\":\"removed\",\"request_id\":\"%s\",\"path\":\"%s\",\"branch_deleted\":false}' \"$token\" \"$target\"; "
            "else sleep 0.05; cat catalog.json; fi;;\naccount) echo '{\"profiles\":[{\"id\":\"work\",\"provider\":\"codex\",\"label\":\"Work\"}]}' ;;\ndirs) printf '{\"path\":\"%s\",\"directories\":[]}' \"$2\";;\nesac\n");program.close();QVERIFY(program.setPermissions(QFileDevice::ReadOwner|QFileDevice::WriteOwner|QFileDevice::ExeOwner));
    }
    void init(){QSettings().clear();for(const auto *name:{"calls","fail-create","fail-remove","old-hgs"})QFile::remove(dir.filePath(name));}
    void clientCachesCoalescesAndScopesSnapshots(){
        HgsClient client(script());QSignalSpy ready(&client,&HgsClient::worktreesReady);
        const auto first=client.requestWorktrees({},"/repo"),second=client.requestWorktrees({},"/repo");QVERIFY(first!=second);QTRY_COMPARE(ready.size(),2);
        QFile calls(dir.filePath("calls"));QVERIFY(calls.open(QIODevice::ReadOnly));QCOMPARE(calls.readAll().count("worktrees"),1);calls.close();
        QVERIFY(client.worktreeSnapshot("mac","/repo").isEmpty());QCOMPARE(client.worktreeSnapshot({},"/linked")["selected_root"].toString(),QString("/linked"));
        client.requestWorktrees({},"/linked");QTRY_COMPARE(ready.size(),3);QVERIFY(calls.open(QIODevice::ReadOnly));QCOMPARE(calls.readAll().count("worktrees"),1);calls.close();
        client.requestWorktrees("mac","/repo");QTRY_COMPARE(ready.size(),4);QVERIFY(!client.worktreeSnapshot("mac","/repo").isEmpty());
        client.requestWorktrees({},"/repo",true);QTRY_COMPARE(ready.size(),5);
    }
    void catalogGuardsUnavailableFoldersAndKeepsOfflineSnapshot(){
        HgsClient client(script());WorktreePanel panel(&client);auto state=fleet();panel.setContext({},"/repo",state);panel.resize(580,400);panel.show();
        auto *tree=panel.findChild<QTreeWidget *>("worktreeCatalog");QTRY_COMPARE(tree->topLevelItemCount(),4);
        QCOMPARE(tree->topLevelItem(0)->childCount(),1);QVERIFY(tree->topLevelItem(0)->child(0)->text(0).contains("codex/repo/one"));
        auto *use=panel.findChild<QPushButton *>("useWorktreeFolder");tree->setCurrentItem(tree->topLevelItem(2));QVERIFY(!use->isEnabled());tree->setCurrentItem(tree->topLevelItem(3));QVERIFY(!use->isEnabled());
        tree->setCurrentItem(tree->topLevelItem(1));QVERIFY(use->isEnabled());QString chosen;panel.launchRequested=[&](const QString &path){chosen=path;};use->click();QCOMPARE(chosen,QString("/linked"));
        auto local=state.local();local.ok=false;state.setLocal(local,QDateTime::currentMSecsSinceEpoch());panel.setContext({},"/repo",state);QCOMPARE(tree->topLevelItemCount(),4);QVERIFY(!use->isEnabled());QVERIFY(panel.findChild<QLabel *>("worktreeStatus")->text().contains("Offline"));
        QVERIFY(tree->topLevelItem(0)->child(0)->text(0).contains("last recorded"));
        const auto preview=qEnvironmentVariable("HGS_WORKTREE_PREVIEW");if(!preview.isEmpty()){QDir().mkpath(preview);QVERIFY(panel.grab().save(preview+"/offline.png"));}
    }
    void lateResponseCannotReplaceAnotherMachineOrFolder(){
        HgsClient client(script());WorktreePanel panel(&client);auto state=fleet();panel.show();panel.setContext({},"/repo",state);panel.setContext("mac","/repo",state);
        client.worktreesReady(1,{},"/repo",QJsonObject{{"state","not_repo"},{"path","/wrong"},{"worktrees",QJsonArray{}}});
        auto *tree=panel.findChild<QTreeWidget *>("worktreeCatalog");QTRY_COMPARE(tree->topLevelItemCount(),4);QCOMPARE(tree->topLevelItem(0)->childCount(),1);QVERIFY(tree->topLevelItem(0)->child(0)->text(0).contains("kimi/repo/two"));
        panel.setContext("mac","/plain",state);client.worktreesReady(2,"mac","/repo",catalog());QVERIFY(!panel.findChild<QLabel *>("worktreeStatus")->text().contains("/wrong"));
    }
    void pickerPreservesProjectMachineAccountAndCustomFolderAcrossPolls(){
        auto state=fleet();SessionOrganization org;const auto project=org.createGroup("Work");const auto id=org.addFolder(project,"mac","/repo");
        NewSessionDialog dialog(script(),state,"mac","codex",project);dialog.setGroups(org,project);QVERIFY(dialog.selectFolder(id));dialog.show();
        auto *account=dialog.findChild<QComboBox *>("launchAccount");QTRY_VERIFY(account->findData("work")>=0);account->setCurrentIndex(account->findData("work"));
        auto *choose=dialog.findChild<QAction *>("chooseLaunchWorktree");QVERIFY(choose);QTRY_VERIFY(choose->isVisible());
        QTimer::singleShot(0,&dialog,[&]{auto *picker=dialog.findChild<QDialog *>("chooseWorktreeDialog");QVERIFY(picker);QTimer::singleShot(3000,picker,&QDialog::reject);auto *tree=picker->findChild<QTreeWidget *>("worktreeCatalog");QTRY_COMPARE(tree->topLevelItemCount(),4);tree->setCurrentItem(tree->topLevelItem(1));auto *use=picker->findChild<QPushButton *>("useWorktreeFolder");QTRY_VERIFY(use->isEnabled());use->click();});choose->trigger();
        QCOMPARE(dialog.findChild<QLabel *>("launchFolderPath")->text(),QString("/linked"));dialog.setFleet(state);
        auto *hint=dialog.findChild<QLabel *>("muted");QVERIFY(!hint->property("outsideProject").toBool());QVERIFY(hint->text().isEmpty());QVERIFY(hint->styleSheet().isEmpty());
        QCOMPARE(dialog.findChild<QComboBox *>("launchProject")->currentData().toString(),project);QCOMPARE(dialog.findChild<QComboBox *>("launchComputer")->currentData().toString(),QString("mac"));QCOMPARE(account->currentData().toString(),QString("work"));QCOMPARE(dialog.findChild<QLabel *>("launchFolderPath")->text(),QString("/linked"));
        QCOMPARE(org.group(project)->folders.size(),1);QSignalSpy launched(&dialog,&NewSessionDialog::launchRequested);dialog.findChild<QPushButton *>("primary")->click();QTRY_COMPARE(launched.size(),1);QCOMPARE(launched.first()[2].toString(),QString("/linked"));QCOMPARE(launched.first()[4].toString(),QString("work"));QCOMPARE(org.group(project)->folders.size(),1);
    }
    void accountPresetWaitsForCatalogAndNeverFallsBackSilently(){
        auto state=fleet();SessionOrganization org;const auto project=org.createGroup("Work");org.addFolder(project,"mac","/repo");
        NewSessionDialog dialog(script(),state,"mac","codex",project);dialog.setGroups(org,project);dialog.selectAccount("work");dialog.show();
        auto *account=dialog.findChild<QComboBox *>("launchAccount");auto *start=dialog.findChild<QPushButton *>("primary");
        QVERIFY(!start->isEnabled());QTRY_COMPARE(account->currentData().toString(),QString("work"));QTRY_VERIFY(start->isEnabled());
        dialog.setFleet(state);QCOMPARE(account->currentData().toString(),QString("work"));
        dialog.selectAccount("removed");QVERIFY(!start->isEnabled());QCOMPARE(account->currentIndex(),-1);
        QVERIFY(dialog.findChild<QLabel *>("launchError")->text().contains("unavailable"));
        dialog.setFleet(state);QCOMPARE(account->currentIndex(),-1);
        // This catalog contains only "work". A missing native profile must not
        // silently fall back to another account, just like any other missing ID.
        dialog.selectAccount("native-codex");QCOMPARE(account->currentIndex(),-1);QVERIFY(!start->isEnabled());
        dialog.selectAccount("work");QCOMPARE(account->currentData().toString(),QString("work"));QVERIFY(start->isEnabled());
    }
    void newWorktreeCreatesExplicitlyAndPreservesLaunchContext(){
        auto state=fleet();SessionOrganization org;const auto project=org.createGroup("Work");const auto id=org.addFolder(project,"mac","/repo");
        NewSessionDialog dialog(script(),state,"mac","codex",project);dialog.setGroups(org,project);QVERIFY(dialog.selectFolder(id));dialog.show();
        auto *account=dialog.findChild<QComboBox *>("launchAccount");QTRY_VERIFY(account->findData("work")>=0);account->setCurrentIndex(account->findData("work"));
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
        QCOMPARE(dialog.findChild<QComboBox *>("launchProject")->currentData().toString(),project);QCOMPARE(dialog.findChild<QComboBox *>("launchComputer")->currentData().toString(),QString("mac"));QCOMPARE(account->currentData().toString(),QString("work"));
        QCOMPARE(dialog.findChild<QLabel *>("launchFolderPath")->text(),QString("/new folder"));QCOMPARE(org.group(project)->folders.size(),1);
        QSignalSpy launched(&dialog,&NewSessionDialog::launchRequested);dialog.findChild<QPushButton *>("primary")->click();QTRY_COMPARE(launched.size(),1);QCOMPARE(launched.first()[2].toString(),QString("/new folder"));
        QVERIFY(calls.open(QIODevice::ReadOnly));const auto commands=calls.readAll();QCOMPARE(commands.count("worktrees create"),1);QVERIFY(commands.contains("@mac worktrees create"));QVERIFY(commands.contains("--base origin/main"));
    }
    void failedCreationRetainsFormAndOriginalFolder(){
        QFile fail(dir.filePath("fail-create"));QVERIFY(fail.open(QIODevice::WriteOnly));fail.close();
        auto state=fleet();SessionOrganization org;const auto project=org.createGroup("Work");org.addFolder(project,"arch","/repo");
        NewSessionDialog dialog(script(),state,{},"codex",project);dialog.setGroups(org,project);dialog.show();
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
        NewSessionDialog dialog(script(),state,{},"codex",project);dialog.setGroups(org,project);dialog.show();
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
        HgsClient client(script());WorktreePanel panel(&client,false,nullptr,WorktreePanel::Layout::Compact);auto state=fleet();panel.setContext({},"/repo",state);panel.resize(620,260);panel.show();
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
        HgsClient client(script());WorktreePanel panel(&client,false,nullptr,WorktreePanel::Layout::Compact);auto state=fleet();panel.setContext({},"/repo",state);panel.resize(620,260);panel.show();
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
        HgsClient client(script());WorktreePanel panel(&client);auto state=fleet();panel.setContext({},"/repo",state);panel.resize(580,400);panel.show();
        auto *tree=panel.findChild<QTreeWidget *>("worktreeCatalog");QTRY_COMPARE(tree->topLevelItemCount(),4);QTRY_VERIFY(panel.findChild<QPushButton *>("cleanUpWorktrees")->isEnabled());
        const auto menuFor=[&](int row){tree->customContextMenuRequested(tree->visualItemRect(tree->topLevelItem(row)).center());return QPointer<QMenu>(panel.findChild<QMenu *>("worktreeMenu"));};
        auto menu=menuFor(0);QVERIFY(menu);QVERIFY(!menu->findChild<QAction *>("removeWorktreeAction"));QVERIFY(menu->findChild<QAction *>("copyWorktreePath"));delete menu.data();
        menu=menuFor(2);QCOMPARE(menu->findChild<QAction *>("removeWorktreeAction")->text(),QString("Forget missing worktree…"));delete menu.data();
        menu=menuFor(1);auto *remove=menu->findChild<QAction *>("removeWorktreeAction");QCOMPARE(remove->text(),QString("Remove worktree…"));bool opened=false;
        QTimer::singleShot(0,&panel,[&]{
            auto *dialog=panel.findChild<QDialog *>("worktreeCleanupDialog");QVERIFY(dialog);QTimer::singleShot(5000,dialog,[dialog]{dialog->done(0);});
            QTRY_VERIFY(calls().contains("worktrees review --path /repo/.git --json --worktree /linked"));QCOMPARE(dialog->windowTitle(),QString("Remove worktree"));
            auto *list=dialog->findChild<QTreeWidget *>("worktreeCleanupList");QTRY_COMPARE(list->topLevelItemCount(),4);QCOMPARE(list->topLevelItem(1)->checkState(0),Qt::Checked);
            opened=true;dialog->reject();
        });
        remove->trigger();QVERIFY(opened);delete menu.data();QVERIFY(!calls().contains("worktrees remove"));
    }
    void olderMachineExplainsUpdateForCleanup(){
        QFile old(dir.filePath("old-hgs"));QVERIFY(old.open(QIODevice::WriteOnly));old.close();
        HgsClient client(script());QSignalSpy failed(&client,&HgsClient::worktreeReviewFailed);client.reviewWorktrees("mac","/repo/.git");QTRY_COMPARE(failed.size(),1);
        QCOMPARE(failed.first()[3].toString(),QString("Update Zerus on mac to clean up worktrees."));QVERIFY(calls().contains("@mac worktrees review"));
    }
};
QTEST_MAIN(TestWorktreePanel)
#include "test_worktreepanel.moc"
