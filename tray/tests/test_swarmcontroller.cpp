#include <QtTest>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSettings>
#include <QTemporaryDir>
#include "FakeCli.h"
#include "SwarmController.h"
#include "SwarmDialog.h"
#include "SwarmConflictReview.h"
#include "FleetState.h"
#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QPushButton>
#include <QTableWidget>
#include <QTabWidget>
#include <QStackedWidget>
#include <QLineEdit>
#include <QLabel>
#include <QScrollBar>
#include <QMessageBox>
#include <QAbstractButton>

class TestSwarmController : public QObject {
    Q_OBJECT
    QTemporaryDir m_directory;
    QString m_program, m_root;
    const QString m_key="name-field";
    QJsonObject organization(const QString &name) const {
        return {{"version",2},{"default_project","p"},{"projects",QJsonArray{QJsonObject{{"id","p"},{"name",name},{"color","#123456"},{"vivid",false},{"folders",QJsonArray{}},{"sessions",QJsonArray{}}}}}};
    }
    void write(const QString &name,const QByteArray &bytes) {QFile file(m_root+'/'+name);QVERIFY(file.open(QIODevice::WriteOnly));file.write(bytes);}
    void response(const QString &name) {
        write("response",QJsonDocument(QJsonObject{{"schema",1},{"organization",organization(name)},{"versions",QJsonObject{{m_key,QJsonArray{"observed"}}}},{"peers",QJsonObject{}},{"conflicts",QJsonArray{}}}).toJson());
    }
    QJsonArray requests() const {
        QFile file(m_root+"/requests");QJsonArray result;if(!file.open(QIODevice::ReadOnly))return result;
        for(const auto &line:file.readAll().split('\n'))if(!line.isEmpty())result.append(QJsonDocument::fromJson(line).object());return result;
    }
    QJsonObject conflictSnapshot() const {
        auto org=organization("Work");auto projects=org["projects"].toArray();
        projects.append(QJsonObject{{"id","ungrouped"},{"name","General"},{"sessions",QJsonArray{}},{"folders",QJsonArray{}}});org["projects"]=projects;
        return {{"organization",org},{"node_id","node-a"},{"machines",QJsonArray{
            QJsonObject{{"id","node-a"},{"name","Desktop"},{"connection","arch"},{"local",true}},
            QJsonObject{{"id","node-b"},{"name","Laptop"},{"connection","mac"},{"local",false}}}}};
    }
    QJsonObject conflict(const QJsonObject &field,const QJsonValue &first,const QJsonValue &second) const {
        return {{"key",QString::fromUtf8(QJsonDocument(field).toJson(QJsonDocument::Compact))},{"field",field},{"selected","version-a"},
            {"variants",QJsonArray{QJsonObject{{"id","version-a"},{"actor","node-a"},{"value",first}},
                                  QJsonObject{{"id","version-b"},{"actor","node-b"},{"value",second}}}}};
    }
private slots:
    void initTestCase() {
        QVERIFY(m_directory.isValid());QCoreApplication::setOrganizationName("hgs-swarm-test");QCoreApplication::setApplicationName("controller");
        QSettings::setDefaultFormat(QSettings::IniFormat);QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,m_directory.path());
    }
    void init() {
        QSettings().clear();m_root=m_directory.path()+"/case-"+QString::number(QDateTime::currentMSecsSinceEpoch());QDir().mkpath(m_root);m_program=m_root+"/hgs";
        const QByteArray script=R"PY(#!/usr/bin/env python3
import json,pathlib,sys,time
root=pathlib.Path(__file__).parent
action=sys.argv[2]
payload=json.load(sys.stdin) if action in ['apply','initialize','resolve'] else {}
with (root/'requests').open('a') as file:file.write(json.dumps(dict(action=action,input=payload))+'\n')
count=len((root/'requests').read_text().splitlines())
while (root/'hold').exists():time.sleep(.01)
if (root/'fail').exists():print('temporary fixture failure',file=sys.stderr);sys.exit(1)
result=json.loads((root/('preview' if action=='preview' else 'response')).read_text())
if action=='apply':
    result['organization']=payload['desired']
    result['written']={'name-field':['own-'+str(count)]}
    result['versions']={'name-field':['own-'+str(count),'unseen-remote']}
print(json.dumps(result))
)PY";
        write("hgs",script);QVERIFY(FakeCli::makeRunnable(m_program));response("Original");
    }
    void localPresentationSurvivesRemoteMembership() {
        auto local=organization("Old");auto project=local["projects"].toArray()[0].toObject();project["collapsed"]=true;project["sessions"]=QJsonArray{"b","a","removed"};local["projects"]=QJsonArray{project};local["runs"]=QJsonObject{{"run","a"}};
        auto remote=organization("Renamed");project=remote["projects"].toArray()[0].toObject();project["sessions"]=QJsonArray{"a","new","b"};remote["projects"]=QJsonArray{project};
        const auto result=SwarmController::presentation(remote,local);const auto p=result["projects"].toArray()[0].toObject();
        QCOMPARE(p["name"].toString(),"Renamed");QVERIFY(p["collapsed"].toBool());QCOMPARE(p["sessions"].toArray(),QJsonArray({"b","a","new"}));QCOMPARE(result["runs"],local["runs"]);
    }
    void unchangedRefreshDoesNotRewriteSettings() {
        HgsClient client(m_program); SwarmController controller(&client);
        controller.start(organization("Original")); QTRY_VERIFY(controller.settled());
        const auto settingsPath = QSettings().fileName();
        const auto checkpoint = QDateTime::fromSecsSinceEpoch(946684800, QTimeZone::UTC);
        QFile settingsFile(settingsPath); QVERIFY(settingsFile.open(QIODevice::ReadWrite));
        QVERIFY(settingsFile.setFileTime(checkpoint, QFileDevice::FileModificationTime)); settingsFile.close();
        controller.refresh(); QTRY_COMPARE(requests().size(), 2); QTRY_VERIFY(controller.settled());
        QCoreApplication::sendPostedEvents();
        QCOMPARE(QFileInfo(settingsPath).lastModified(), checkpoint);
        response("Changed catalog"); controller.refresh();
        QTRY_COMPARE(requests().size(), 3); QTRY_VERIFY(controller.settled());
        QVERIFY(QFileInfo(settingsPath).lastModified() > checkpoint);
        const auto saved = QJsonDocument::fromJson(QSettings().value("swarm/observedOrganization").toByteArray()).object();
        QCOMPARE(saved["projects"].toArray()[0].toObject()["name"].toString(), "Changed catalog");
    }
    void editDuringRefreshUsesVersionsTheUserSaw() {
        HgsClient client(FakeCli::program(m_program));SwarmController controller(&client);controller.start(organization("Original"));QTRY_VERIFY(controller.settled());
        write("hold",{});response("Remote change");controller.refresh();QTRY_COMPARE(requests().size(),2);
        controller.edit(organization("Local change"));QFile::remove(m_root+"/hold");
        QTRY_COMPARE(requests().size(),3);QTRY_VERIFY(controller.settled());
        const auto patch=requests()[2].toObject()["input"].toObject();QCOMPARE(patch["versions"].toObject()[m_key].toArray(),QJsonArray{"observed"});
        QCOMPARE(patch["base"].toObject()["projects"].toArray()[0].toObject()["name"].toString(),"Original");
        QCOMPARE(controller.snapshot()["organization"].toObject()["projects"].toArray()[0].toObject()["name"].toString(),"Local change");
    }
    void editDuringWriteAdvancesOnlyOwnAcknowledgedVersions() {
        HgsClient client(FakeCli::program(m_program));SwarmController controller(&client);controller.start(organization("Original"));QTRY_VERIFY(controller.settled());
        write("hold",{});controller.edit(organization("First"));QTRY_COMPARE(requests().size(),2);
        controller.edit(organization("Second"));QFile::remove(m_root+"/hold");QTRY_COMPARE(requests().size(),3);QTRY_VERIFY(controller.settled());
        const auto patch=requests()[2].toObject()["input"].toObject();QCOMPARE(patch["versions"].toObject()[m_key].toArray(),QJsonArray{"own-2"});
        QCOMPARE(patch["base"].toObject()["projects"].toArray()[0].toObject()["name"].toString(),"First");
    }
    void restartRetriesUnsavedDraftInsteadOfReplacingIt() {
        {
            HgsClient client(FakeCli::program(m_program));SwarmController controller(&client);controller.start(organization("Original"));QTRY_VERIFY(controller.settled());
            write("fail",{});controller.edit(organization("Unsaved"));QTRY_COMPARE(requests().size(),2);QTRY_VERIFY(!controller.busy());
        }
        QFile::remove(m_root+"/fail");response("A newer remote value");
        HgsClient client(FakeCli::program(m_program));SwarmController controller(&client);controller.start(organization("Unsaved"));QTRY_COMPARE(requests().size(),4);QTRY_VERIFY(controller.settled());
        const auto patch=requests()[3].toObject()["input"].toObject();QCOMPARE(patch["versions"].toObject()[m_key].toArray(),QJsonArray{"observed"});
        QCOMPARE(patch["desired"].toObject()["projects"].toArray()[0].toObject()["name"].toString(),"Unsaved");
    }
    void unfinishedTextFieldDefersRemoteRefresh() {
        HgsClient client(FakeCli::program(m_program));SwarmController controller(&client);QSignalSpy rendered(&controller,&SwarmController::organizationReady);
        controller.start(organization("Original"));QTRY_VERIFY(controller.settled());QCOMPARE(rendered.count(),1);
        write("hold",{});response("Remote name");controller.refresh();QTRY_COMPARE(requests().size(),2);
        controller.setDraftEditing(true);QFile::remove(m_root+"/hold");QTRY_VERIFY(!controller.busy());QCOMPARE(rendered.count(),1);
        controller.edit(organization("Typed name"));controller.setDraftEditing(false);QTRY_COMPARE(requests().size(),3);QTRY_VERIFY(controller.settled());
        const auto patch=requests()[2].toObject()["input"].toObject();QCOMPARE(patch["versions"].toObject()[m_key].toArray(),QJsonArray{"observed"});
    }
    void conflictReviewExplainsSessionPlacementAndRequiresChoice() {
        FleetState fleet;BoxState local;local.host="arch";SessionInfo session;session.name="claude/demo/planning";session.project="demo";session.tag="planning";local.sessions.append(session);fleet.setLocal(local,0);
        SwarmConflictReview review(fleet);review.resize(510,480);review.show();
        auto snapshot=conflictSnapshot();auto c=conflict({{"kind","session"},{"machine","node-a"},{"session",session.name}},"ungrouped","p");
        // Independent edits with an identical value should appear as one choice.
        auto variants=c["variants"].toArray();variants.append(QJsonObject{{"id","version-c"},{"actor","node-b"},{"value","ungrouped"}});c["variants"]=variants;
        review.setConflict(c,snapshot);QSignalSpy sent(&review,&SwarmConflictReview::resolutionRequested);
        auto *choices=review.findChild<QTableWidget*>("swarmConflictChoices");auto *button=review.findChild<QPushButton*>("swarmResolve");
        QCOMPARE(choices->rowCount(),2);QCOMPARE(choices->item(0,0)->text(),"General");QCOMPARE(choices->item(1,0)->text(),"Work");
        QVERIFY(choices->item(0,1)->text().contains("Desktop (this computer)"));QVERIFY(choices->item(0,1)->text().contains("Laptop"));
        QCOMPARE(choices->item(0,2)->text(),"Shown now");QVERIFY(!button->isEnabled());
        QVERIFY(review.findChild<QLabel*>("swarmConflictSubject")->text().contains("demo / planning on Desktop"));
        choices->setCurrentCell(1,0);QVERIFY(button->isEnabled());QCOMPARE(button->text(),"Keep this project");
        auto *outcome=review.findChild<QLabel*>("swarmConflictOutcome");QVERIFY(outcome->text().contains("Assign this session to “Work”"));QVERIFY(outcome->text().contains("running agent stay intact"));
        button->click();QCOMPARE(sent.count(),1);const auto payload=sent[0][0].toJsonObject();QCOMPARE(payload["key"],c["key"]);QCOMPARE(payload["value"],QJsonValue("p"));
        QCOMPARE(payload["versions"].toArray(),QJsonArray({"version-a","version-b","version-c"}));
        QTest::qWait(30);
        const auto preview=qEnvironmentVariable("HGS_CONFLICT_PREVIEW");if(!preview.isEmpty())QVERIFY(review.grab().save(preview));
    }
    void wrappedConflictChoicesResizeWithoutLosingSelection() {
        FleetState fleet;SwarmConflictReview review(fleet);review.resize(900,560);review.show();QTest::qWait(30);
        const QJsonObject folder{{"project","p"},{"machine","node-b"},{"name","Application sources and shared integration fixtures"},
            {"path","/workspace/example-project/components/application/integration/fixtures"}};
        review.setConflict(conflict({{"kind","folder"},{"id","folder-id"}},folder,QJsonValue::Null),conflictSnapshot());
        auto *choices=review.findChild<QTableWidget*>("swarmConflictChoices");choices->setCurrentCell(0,0);QTest::qWait(30);
        const auto wideHeight=choices->rowHeight(0);const auto chosen=choices->item(0,0)->data(Qt::UserRole);
        review.resize(420,560);QTRY_VERIFY(choices->rowHeight(0)>wideHeight);
        QCOMPARE(choices->currentRow(),0);QCOMPARE(choices->item(0,0)->data(Qt::UserRole),chosen);
        QVERIFY(review.findChild<QPushButton*>("swarmResolve")->isEnabled());
        review.resize(900,560);QTRY_VERIFY(choices->rowHeight(0)<=wideHeight);
        QCOMPARE(choices->currentRow(),0);
    }
    void archiveIdentityIsReadableAndNewVersionsClearTheChoice() {
        FleetState fleet;BoxState local;local.host="arch";SessionInfo archive;archive.name="claude/demo/old-plan";archive.project="demo";archive.tag="old-plan";archive.state="archived";archive.archiveId="archive-id";local.sessions.append(archive);fleet.setLocal(local,0);
        SwarmConflictReview review(fleet);auto snapshot=conflictSnapshot();auto c=conflict({{"kind","session"},{"machine","node-a"},{"session","archive\narchive-id"}},"p","ungrouped");
        review.setConflict(c,snapshot);QVERIFY(review.findChild<QLabel*>("swarmConflictSubject")->text().contains("Archived session: demo / old-plan"));
        auto *choices=review.findChild<QTableWidget*>("swarmConflictChoices");auto *button=review.findChild<QPushButton*>("swarmResolve");choices->setCurrentCell(1,0);QVERIFY(button->isEnabled());
        snapshot["peers"]=QJsonObject{{"mac",QJsonObject{{"last_sync",12345}}}};review.setConflict(c,snapshot);QVERIFY(button->isEnabled());QCOMPARE(choices->currentRow(),1);
        auto variants=c["variants"].toArray();auto changed=variants[1].toObject();changed["id"]="new-version";variants[1]=changed;c["variants"]=variants;
        review.setConflict(c,snapshot);QVERIFY(!button->isEnabled());QVERIFY(choices->selectedItems().isEmpty());
        QVERIFY(review.findChild<QLabel*>("swarmConflictExplanation")->text().contains("Review the updated choices again"));
        choices->setCurrentCell(0,0);review.setBusy(true);QVERIFY(!button->isEnabled());review.setBusy(false);QVERIFY(button->isEnabled());
    }
    void nullAssignmentsAndFolderObjectsKeepTheirTypes() {
        FleetState fleet;SwarmConflictReview review(fleet);const auto snapshot=conflictSnapshot();
        auto c=conflict({{"kind","session"},{"machine","node-a"},{"session","codex/demo/test"}},"p",QJsonValue::Null);
        review.setConflict(c,snapshot);QSignalSpy sent(&review,&SwarmConflictReview::resolutionRequested);
        auto *choices=review.findChild<QTableWidget*>("swarmConflictChoices");auto *button=review.findChild<QPushButton*>("swarmResolve");
        choices->setCurrentCell(1,0);QVERIFY(choices->item(1,0)->text().contains("local default"));button->click();QCOMPARE(sent.count(),1);QVERIFY(sent[0][0].toJsonObject()["value"].isNull());
        const QJsonObject folder{{"project","p"},{"machine","node-b"},{"path","/projects/demo"},{"name","Source"}};
        c=conflict({{"kind","folder"},{"id","folder-id"}},folder,QJsonValue::Null);review.setConflict(c,snapshot);choices->setCurrentCell(0,0);
        QVERIFY(choices->item(0,0)->text().contains("Project: Work"));QVERIFY(choices->item(0,0)->text().contains("Computer: Laptop"));QVERIFY(!choices->item(0,0)->text().contains("{"));
        button->click();QCOMPARE(sent.count(),2);QCOMPARE(sent[1][0].toJsonObject()["value"].toObject(),folder);
    }
    void catalogRemovalRequiresConfirmationAndCancelKeepsConflict() {
        FleetState fleet;SwarmConflictReview review(fleet);const auto c=conflict({{"kind","project"},{"id","p"},{"field","alive"}},true,false);
        review.setConflict(c,conflictSnapshot());QSignalSpy sent(&review,&SwarmConflictReview::resolutionRequested);
        auto *choices=review.findChild<QTableWidget*>("swarmConflictChoices");auto *button=review.findChild<QPushButton*>("swarmResolve");choices->setCurrentCell(1,0);
        QCOMPARE(choices->item(1,0)->text(),"Remove project from the catalog");
        QTimer::singleShot(0,this,[&]{auto *box=qobject_cast<QMessageBox*>(QApplication::activeModalWidget());QVERIFY(box);QVERIFY(box->text().contains("Work"));QCOMPARE(box->defaultButton(),qobject_cast<QPushButton*>(box->button(QMessageBox::Cancel)));box->button(QMessageBox::Cancel)->click();});
        button->click();QCOMPARE(sent.count(),0);QVERIFY(button->isEnabled());
        QTimer::singleShot(0,this,[&]{auto *box=qobject_cast<QMessageBox*>(QApplication::activeModalWidget());QVERIFY(box);box->button(QMessageBox::Apply)->click();});
        button->click();QCOMPARE(sent.count(),1);QCOMPARE(sent[0][0].toJsonObject()["value"],QJsonValue(false));
    }
    void sharedNamesRemainLiteralAndDuplicateProjectNamesAreDistinct() {
        FleetState fleet;SwarmConflictReview review(fleet);auto snapshot=conflictSnapshot();
        auto c=conflict({{"kind","project"},{"id","p"},{"field","name"}},"<b>Work</b>","Plain name");review.setConflict(c,snapshot);
        auto *choices=review.findChild<QTableWidget*>("swarmConflictChoices");choices->setCurrentCell(0,0);
        QCOMPARE(choices->item(0,0)->text(),"<b>Work</b>");QCOMPARE(review.findChild<QLabel*>("swarmConflictOutcome")->textFormat(),Qt::PlainText);
        auto org=snapshot["organization"].toObject();auto projects=org["projects"].toArray();projects.append(QJsonObject{{"id","another-project"},{"name","Work"}});org["projects"]=projects;snapshot["organization"]=org;
        c=conflict({{"kind","session"},{"machine","node-a"},{"session","codex/demo/test"}},"p","another-project");review.setConflict(c,snapshot);
        QVERIFY(choices->item(0,0)->text()!=choices->item(1,0)->text());
    }
    void aRefreshDuringConfirmationCannotResolveUnseenVersions() {
        FleetState fleet;SwarmConflictReview review(fleet);auto c=conflict({{"kind","project"},{"id","p"},{"field","alive"}},true,false);
        review.setConflict(c,conflictSnapshot());QSignalSpy sent(&review,&SwarmConflictReview::resolutionRequested);
        review.findChild<QTableWidget*>("swarmConflictChoices")->setCurrentCell(1,0);
        QTimer::singleShot(0,this,[&]{
            auto *box=qobject_cast<QMessageBox*>(QApplication::activeModalWidget());QVERIFY(box);
            auto variants=c["variants"].toArray();auto newer=variants[0].toObject();newer["id"]="unseen-version";variants[0]=newer;c["variants"]=variants;
            review.setConflict(c,conflictSnapshot());box->button(QMessageBox::Apply)->click();
        });
        review.findChild<QPushButton*>("swarmResolve")->click();QCOMPARE(sent.count(),0);
        QVERIFY(!review.findChild<QPushButton*>("swarmResolve")->isEnabled());
    }
    void dialogResolvesOnlyTheExplicitlyReviewedChoice() {
        HgsClient client(FakeCli::program(m_program));SwarmController controller(&client);controller.start(organization("Original"));QTRY_VERIFY(controller.settled());
        auto snapshot=controller.snapshot();const auto context=conflictSnapshot();snapshot["machines"]=context["machines"];snapshot["node_id"]=context["node_id"];snapshot["organization"]=context["organization"];
        const auto c=conflict({{"kind","session"},{"machine","node-a"},{"session","codex/demo/review"}},"ungrouped","p");snapshot["conflicts"]=QJsonArray{c};controller.acceptExternal(snapshot);
        FleetState fleet;bool checked=false;
        QTimer::singleShot(0,this,[&]{
            auto *dialog=qobject_cast<QDialog*>(QApplication::activeModalWidget());QVERIFY(dialog);dialog->findChild<QTabWidget*>("swarmTabs")->setCurrentIndex(2);
            auto *choices=dialog->findChild<QTableWidget*>("swarmConflictChoices");auto *button=dialog->findChild<QPushButton*>("swarmResolve");QVERIFY(!button->isEnabled());choices->setCurrentCell(1,0);QTRY_VERIFY(button->isEnabled());
            QTest::qWait(30);const auto preview=qEnvironmentVariable("HGS_CONFLICT_PREVIEW");if(!preview.isEmpty())QVERIFY(dialog->grab().save(preview+"-dialog.png"));
            button->click();QTRY_VERIFY(requests().size()>=2);QCOMPARE(requests().at(1).toObject()["action"].toString(),"resolve");
            const auto input=requests().at(1).toObject()["input"].toObject();QCOMPARE(input["key"],c["key"]);QCOMPARE(input["value"],QJsonValue("p"));QCOMPARE(input["versions"].toArray(),QJsonArray({"version-a","version-b"}));
            QTRY_VERIFY(dialog->findChild<QLabel*>("swarmStatus")->text().startsWith("Choice saved"));QTRY_VERIFY(controller.settled());
            checked=true;dialog->reject();
        });
        QTimer timeout;timeout.setSingleShot(true);connect(&timeout,&QTimer::timeout,this,[]{if(auto *dialog=qobject_cast<QDialog*>(QApplication::activeModalWidget()))dialog->reject();});timeout.start(10000);
        showSwarmDialog(&client,&controller,fleet,nullptr,[]{});QVERIFY(checked);
    }
    void largeFleetKeepsScrollAndSelection() {
        HgsClient client(FakeCli::program(m_program));SwarmController controller(&client);controller.start(organization("Original"));QTRY_VERIFY(controller.settled());
        auto snapshot=controller.snapshot();QJsonArray machines;
        for(int i=0;i<30;++i)machines.append(QJsonObject{{"id",QString::number(i)},{"name",QString("Computer %1").arg(i,2,10,QChar('0'))},{"connection",QString("peer-%1").arg(i)}});
        snapshot["machines"]=machines;controller.acceptExternal(snapshot);FleetState fleet;bool checked=false;
        QTimer::singleShot(0,this,[&]{
            auto *dialog=qobject_cast<QDialog*>(QApplication::activeModalWidget());QVERIFY(dialog);
            QTimer::singleShot(50,dialog,[&,dialog]{
                auto *table=dialog->findChild<QTableWidget*>("swarmMachines");auto *filter=dialog->findChild<QLineEdit*>("swarmMachineFilter");
                QCOMPARE(table->rowCount(),30);QVERIFY(table->height()>300);QVERIFY(table->verticalScrollBar()->maximum()>0);
                table->setCurrentCell(25,0);table->scrollToItem(table->item(25,0));
                const auto selected=table->item(table->currentRow(),0)->data(Qt::UserRole);const int scroll=table->verticalScrollBar()->value();QVERIFY(scroll>0);
                auto updated=snapshot;updated["peers"]=QJsonObject{{"peer-25",QJsonObject{{"last_sync",12345}}}};
                controller.acceptExternal(updated);QCOMPARE(table->item(table->currentRow(),0)->data(Qt::UserRole),selected);QCOMPARE(table->verticalScrollBar()->value(),scroll);
                const auto preview=qEnvironmentVariable("HGS_SWARM_PREVIEW");if(!preview.isEmpty())QVERIFY(dialog->grab().save(preview+"-computers.png"));
                filter->setText("PEER-25");int visible=0;for(int row=0;row<30;++row)if(!table->isRowHidden(row))++visible;
                QCOMPARE(visible,1);QCOMPARE(dialog->findChild<QLabel*>("swarmMachineCount")->text(),"1 of 30 computers");
                controller.acceptExternal(updated);QCOMPARE(filter->text(),"PEER-25");QVERIFY(!table->isRowHidden(table->currentRow()));
                filter->clear();for(int row=0;row<30;++row)QVERIFY(!table->isRowHidden(row));checked=true;dialog->reject();
            });
        });
        QTimer timeout;timeout.setSingleShot(true);connect(&timeout,&QTimer::timeout,this,[]{if(auto *dialog=qobject_cast<QDialog*>(QApplication::activeModalWidget()))dialog->reject();});timeout.start(10000);
        showSwarmDialog(&client,&controller,fleet,nullptr,[]{});QVERIFY(checked);
    }
    void previewsDoNotMoveSurroundingControls() {
        HgsClient client(FakeCli::program(m_program));SwarmController controller(&client);controller.start(organization("Original"));QTRY_VERIFY(controller.settled());
        FleetState fleet;BoxState remote;remote.host="mac";remote.ok=true;fleet.setPeer(remote,0);bool checked=false;
        QTimer::singleShot(0,this,[&]{
            auto *dialog=qobject_cast<QDialog*>(QApplication::activeModalWidget());QVERIFY(dialog);
            auto *tabs=dialog->findChild<QTabWidget*>("swarmTabs");tabs->setCurrentIndex(1);
            auto *preview=dialog->findChild<QPushButton*>("swarmPreview");auto *join=dialog->findChild<QPushButton*>("swarmJoin");auto *status=dialog->findChild<QLabel*>("swarmStatus");
            QTest::qWait(30);const auto size=dialog->size();const auto tabsRect=tabs->geometry();const auto statusRect=status->geometry();const auto joinBottom=join->mapTo(dialog,join->rect().bottomRight());const auto previewRect=preview->geometry();
            for(bool same:{false,true,false}){
                write("preview",QJsonDocument(QJsonObject{{"node_id","node-b"},{"swarm_id","swarm-b"},{"same_swarm",same},{"initialized",true},{"projects",QJsonArray{QJsonObject{{"id","remote-id"},{"name","Remote project"}}}}}).toJson());
                preview->click();QTRY_VERIFY(join->isEnabled());QTest::qWait(20);
                QCOMPARE(dialog->size(),size);QCOMPARE(tabs->geometry(),tabsRect);QCOMPARE(status->geometry(),statusRect);QCOMPARE(join->mapTo(dialog,join->rect().bottomRight()),joinBottom);QCOMPARE(preview->geometry(),previewRect);
            }
            tabs->setCurrentIndex(2);QTest::qWait(20);QCOMPARE(dialog->size(),size);QCOMPARE(status->geometry(),statusRect);
            QCOMPARE(dialog->findChild<QStackedWidget*>("swarmConflictReview")->currentIndex(),0);
            checked=true;dialog->reject();
        });
        QTimer timeout;timeout.setSingleShot(true);connect(&timeout,&QTimer::timeout,this,[]{if(auto *dialog=qobject_cast<QDialog*>(QApplication::activeModalWidget()))dialog->reject();});timeout.start(15000);
        showSwarmDialog(&client,&controller,fleet,nullptr,[]{});QVERIFY(checked);
    }
    void joinPreviewRequiresExplicitProjectMapping() {
        HgsClient client(FakeCli::program(m_program));SwarmController controller(&client);controller.start(organization("Original"));QTRY_VERIFY(controller.settled());
        write("preview",QJsonDocument(QJsonObject{{"node_id","node-b"},{"swarm_id","swarm-b"},{"same_swarm",false},{"initialized",true},{"projects",QJsonArray{QJsonObject{{"id","different-id"},{"name","Original"}}}}}).toJson());
        FleetState fleet;BoxState local;local.host="arch";local.ok=true;fleet.setLocal(local,0);BoxState remote;remote.host="mac";remote.ok=true;fleet.setPeer(remote,0);
        bool checked=false;
        QTimer::singleShot(0,this,[&]{
            auto *dialog=qobject_cast<QDialog*>(QApplication::activeModalWidget());QVERIFY(dialog);
            dialog->findChild<QTabWidget*>("swarmTabs")->setCurrentIndex(1);
            auto *button=dialog->findChild<QPushButton*>("swarmPreview");QVERIFY(button);auto *join=dialog->findChild<QPushButton*>("swarmJoin");QVERIFY(join);
            QVERIFY(!join->isEnabled());button->click();
            // Join turns on once the fixture's preview arrived and the mapping is built
            // (slow under load: the fixture is a Python process on Windows).
            QTRY_VERIFY_WITH_TIMEOUT(join->isEnabled(),15000);
            auto *table=dialog->findChild<QTableWidget*>("swarmProjectMapping");QVERIFY(table);QCOMPARE(table->rowCount(),1);
            auto *choice=qobject_cast<QComboBox*>(table->cellWidget(0,1));QVERIFY(choice);QCOMPARE(choice->count(),2);QVERIFY(choice->currentData().toString().isEmpty());
            const auto preview=qEnvironmentVariable("HGS_SWARM_PREVIEW");if(!preview.isEmpty()){QTest::qWait(30);QVERIFY(dialog->grab().save(preview));}
            checked=true;dialog->reject();
        });
        QTimer timeout;timeout.setSingleShot(true);connect(&timeout,&QTimer::timeout,this,[]{if(auto *dialog=qobject_cast<QDialog*>(QApplication::activeModalWidget()))dialog->reject();});timeout.start(20000);
        showSwarmDialog(&client,&controller,fleet,nullptr,[]{});QVERIFY(checked);
    }
};
QTEST_MAIN(TestSwarmController)
#include "test_swarmcontroller.moc"
