#include "FakeCli.h"
#include "ProcessView.h"
#include "ProcessRunner.h"
#include "ProcessSettings.h"
#include "HgsClient.h"
#include "BusyIndicator.h"
#include <atomic>
#include <QJsonDocument>
#include <QScopeGuard>
#include <QTest>
#include <QSettings>
#include <QFile>
#include <QTemporaryDir>
#include <QTreeWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QLabel>
#include <QMenu>
#include <QScrollBar>

class TestProcessView:public QObject {
    Q_OBJECT
private slots:
    void init(){QSettings().setValue("processes/enabled",true);}
    void cleanup(){QSettings().remove("processes");}
    void disabledDoesNotReadOrContinuePolling();
    void identitySelectionOutputAndOffline();
    void multiSelectStopStaysPinnedAndSurvivesRefresh();
    void sessionChangeCancelsRemainingStops();
    void delayedLoadingAndQuietCompletedOutput();
};
void TestProcessView::disabledDoesNotReadOrContinuePolling(){
    QTemporaryDir dir;QFile file(dir.filePath("hgs"));QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("#!/usr/bin/env python3\nimport os\nopen(os.path.join(os.path.dirname(__file__),'calls'),'a').write('called\\n')\nprint('{}',end='')\n");file.close();QVERIFY(FakeCli::makeRunnable(file.fileName()));
    // Polls every 0.2 s, below the production minimum of 1 s.
    ProcessSettings::setPollFloorForTests(0.2);const auto restoreFloor=qScopeGuard([]{ProcessSettings::setPollFloorForTests(0);});
    std::atomic<int> launches=0;ProcessRunner::setLaunchObserver([&launches]{++launches;});
    const auto observer=qScopeGuard([]{ProcessRunner::setLaunchObserver({});});
    const auto calls=[&]{QFile log(dir.filePath("calls"));return log.open(QIODevice::ReadOnly)?int(log.readAll().count('\n')):0;};
    QSettings().setValue("processes/enabled",false);QSettings().setValue("processes/activeSeconds",0.2);
    HgsClient client(FakeCli::program(file.fileName()));ProcessView view(&client);view.show();
    QJsonObject job{{"id","shell"},{"command","sleep 300"},{"status","running"},{"capabilities",QJsonObject{{"output",true},{"stop",true}}}};
    view.setSession({},"session",{},QJsonObject{{"run_id","run"},{"conversation_id","conversation"},{"processes",QJsonObject{{"items",QJsonArray{job}}}}},true,true);
    QTest::qWait(600);QVERIFY(!QFile::exists(dir.filePath("calls")));QVERIFY(!view.findChild<QPushButton *>("stopProcess")->isEnabled());
    QSettings().setValue("processes/enabled",true);view.applyPreferences();QTRY_VERIFY(QFile::exists(dir.filePath("calls")));
    // A request launched just before disabling may still be starting: count it first.
    QSettings().setValue("processes/enabled",false);view.applyPreferences();QTest::qWait(200);QTRY_COMPARE(calls(),int(launches));const auto size=calls();
    QTest::qWait(600);QCOMPARE(calls(),size);
}
void TestProcessView::identitySelectionOutputAndOffline(){
    QTemporaryDir dir;QFile script(dir.filePath("hgs"));QVERIFY(script.open(QIODevice::WriteOnly));
    script.write(R"(#!/usr/bin/env python3
import json,sys,time
a=sys.argv[1:]
def get(k): return a[a.index(k)+1]
time.sleep(.1)
print(json.dumps(dict(id=get('--output'),run_id=get('--run'),conversation_id=get('--conversation'),output='\n'.join(get('--conversation')+' line '+str(i) for i in range(150)))))
)");script.close();QVERIFY(FakeCli::makeRunnable(script.fileName()));
    HgsClient client(FakeCli::program(script.fileName()));ProcessView view(&client);view.resize(900,620);view.show();
    QJsonObject a{{"id","a"},{"command","echo '<unsafe>'"},{"cwd","/workspace"},{"owner","main"},{"status","running"},{"started_at",1},{"updated_at",2},{"capabilities",QJsonObject{{"stop",true}}}};
    auto b=a;b["id"]="b";b["status"]="completed";b["capabilities"]=QJsonObject{{"stop",false}};
    QJsonObject data{{"run_id","run"},{"conversation_id","first"},{"processes",QJsonObject{{"items",QJsonArray{a,b}}}}};
    view.setSession("mac","dsh/project","",data,true,true);
    auto *list=view.findChild<QTreeWidget*>("processList");auto *output=view.findChild<QPlainTextEdit*>("processOutput");auto *stop=view.findChild<QPushButton*>("stopProcess");
    QVERIFY(list&&output&&stop);QCOMPARE(list->topLevelItem(1)->childCount(),1);QCOMPARE(list->topLevelItem(2)->childCount(),1);
    QTRY_VERIFY(output->toPlainText().startsWith("first line"));QVERIFY(stop->isVisible());QTRY_VERIFY(stop->isEnabled());
    view.selectProcess("b");QTRY_VERIFY(output->toPlainText().startsWith("first line"));QVERIFY(stop->isVisible());QVERIFY(!stop->isEnabled());
    output->verticalScrollBar()->setValue(25);const auto scroll=output->verticalScrollBar()->value();
    view.hide();view.show();view.setSession("mac","dsh/project","",data,true,true);QTRY_VERIFY(output->toPlainText().startsWith("first line"));QCOMPARE(list->currentItem()->data(0,Qt::UserRole).toString(),QString("b"));QCOMPARE(output->verticalScrollBar()->value(),scroll);
    auto newer=data;newer["conversation_id"]="second";view.setSession("mac","dsh/project","",newer,true,true);
    QTRY_VERIFY(output->toPlainText().startsWith("second line"));
    view.setSession("mac","dsh/project","",data,true,true);QTRY_VERIFY(output->toPlainText().startsWith("first line"));QCOMPARE(list->currentItem()->data(0,Qt::UserRole).toString(),QString("b"));
    view.selectProcess("a");view.setSession("mac","dsh/project","",data,false,false);
    QVERIFY(!stop->isEnabled());QCOMPARE(list->topLevelItem(1)->child(0)->text(2),QString("Unconfirmed"));
    const auto preview=qEnvironmentVariable("HGS_PROCESSES_PREVIEW");if(!preview.isEmpty()){view.setSession("mac","dsh/project","",data,true,true);QTest::qWait(200);QVERIFY(view.grab().save(preview));}
}

namespace {
bool fixture(const QString &path) {
    QFile file(path);if(!file.open(QIODevice::WriteOnly))return false;
    file.write(R"(#!/usr/bin/env python3
import json,sys,time,pathlib
a=sys.argv[1:]
def get(k): return a[a.index(k)+1]
stop='--stop' in a
identity=get('--stop' if stop else '--output')
with open(pathlib.Path(__file__).with_suffix('.log'),'a') as f:
 f.write(json.dumps(dict(id=identity,stop=stop,run=get('--run'),conversation=get('--conversation')))+'\n')
time.sleep(.8 if identity=='slow' else .15 if stop else .08)
print(json.dumps(dict(id=identity,run_id=get('--run'),conversation_id=get('--conversation'),status='requested',output=identity+' output')))
)");file.close();return FakeCli::makeRunnable(file.fileName());
}
QJsonObject job(const QString &id,bool control=true,const QString &state="running") {
    return {{"id",id},{"command","echo "+id},{"owner","main"},{"status",state},{"started_at",1},{"updated_at",2},{"capabilities",QJsonObject{{"stop",control}}}};
}
QJsonObject inspection(const QJsonArray &jobs,const QString &conversation="conversation") {
    return {{"run_id","run"},{"conversation_id",conversation},{"host_generation","generation"},{"processes",QJsonObject{{"items",jobs}}}};
}
QList<QJsonObject> requests(const QString &path,bool stop) {
    QFile file(path);if(!file.open(QIODevice::ReadOnly))return {};
    QList<QJsonObject> result;while(!file.atEnd()){const auto row=QJsonDocument::fromJson(file.readLine()).object();if(row["stop"].toBool()==stop)result<<row;}return result;
}
QTreeWidgetItem *row(QTreeWidget *list,const QString &id){
    for(int i=0;i<list->topLevelItemCount();++i)for(int j=0;j<list->topLevelItem(i)->childCount();++j){auto *r=list->topLevelItem(i)->child(j);if(r->data(0,Qt::UserRole)==id)return r;}return nullptr;
}
}
void TestProcessView::multiSelectStopStaysPinnedAndSurvivesRefresh(){
    QTemporaryDir dir;QVERIFY(fixture(dir.filePath("hgs")));HgsClient client(FakeCli::program(dir.filePath("hgs")));ProcessView view(&client);view.resize(900,620);view.show();
    auto a=job("a"),b=job("b"),c=job("c");auto data=inspection({a,b,c});view.setSession("arch","dsh/project","",data,true,true);
    auto *list=view.findChild<QTreeWidget*>("processList");auto *stop=view.findChild<QPushButton*>("stopProcess");auto *notice=view.findChild<QLabel*>("processStopNotice");
    QCOMPARE(list->selectionMode(),QAbstractItemView::ExtendedSelection);
    view.selectProcess("a");row(list,"b")->setSelected(true);QCOMPARE(list->selectedItems().size(),2);
    a["updated_at"]=25;b["updated_at"]=25;data=inspection({a,b,c});view.setSession("arch","dsh/project","",data,true,true);
    QCOMPARE(list->selectedItems().size(),2);QVERIFY(stop->isEnabled());QCOMPARE(stop->text(),QString("Stop selected (2)"));
    QTimer::singleShot(0,&view,[&]{auto *menu=view.findChild<QMenu *>("processContextMenu");QVERIFY(menu);
        auto *action=menu->findChild<QAction *>("terminateProcesses");QVERIFY(action);QVERIFY(action->isEnabled());QCOMPARE(action->text(),QString("Terminate selected (2)"));
        QCOMPARE(list->selectedItems().size(),2);menu->setActiveAction(action);QTest::keyClick(menu,Qt::Key_Return);});
    list->customContextMenuRequested(list->visualItemRect(row(list,"a")).center());
    view.selectProcess("c"); // changing UI selection must not retarget the batch
    QTRY_COMPARE(requests(dir.filePath("hgs.log"),true).size(),2);
    QTRY_VERIFY(notice->text().contains("Stop requested for 2"));
    QSet<QString> stopped;for(const auto &r:requests(dir.filePath("hgs.log"),true)){stopped.insert(r["id"].toString());QCOMPARE(r["run"],QJsonValue("run"));QCOMPARE(r["conversation"],QJsonValue("conversation"));}
    QCOMPARE(stopped,(QSet<QString>{"a","b"}));
    view.selectProcess("a");QVERIFY(!stop->isEnabled());QVERIFY(stop->toolTip().contains("pending"));
    c["capabilities"]=QJsonObject{{"stop",false}};data=inspection({a,b,c});view.setSession("arch","dsh/project","",data,true,true);view.selectProcess("c");
    QVERIFY(!stop->isEnabled());QVERIFY(stop->toolTip().contains("unavailable"));
    view.selectProcess("b");
    QTimer::singleShot(0,&view,[&]{auto *menu=view.findChild<QMenu *>("processContextMenu");QVERIFY(menu);
        auto *action=menu->findChild<QAction *>("terminateProcesses");QVERIFY(action);QVERIFY(!action->isEnabled());QCOMPARE(action->text(),QString("Terminate"));
        QCOMPARE(list->selectedItems().size(),1);QCOMPARE(list->currentItem(),row(list,"c"));menu->close();});
    list->customContextMenuRequested(list->visualItemRect(row(list,"c")).center());
    QCOMPARE(requests(dir.filePath("hgs.log"),true).size(),2);
    for(auto *button:view.findChildren<QPushButton*>())QVERIFY(button->text()!="Open Terminal");
}
void TestProcessView::sessionChangeCancelsRemainingStops(){
    QTemporaryDir dir;QVERIFY(fixture(dir.filePath("hgs")));HgsClient client(FakeCli::program(dir.filePath("hgs")));ProcessView view(&client);view.resize(900,620);view.show();
    auto data=inspection({job("slow"),job("next")});view.setSession("arch","dsh/project","",data,true,true);
    auto *list=view.findChild<QTreeWidget*>("processList");auto *stop=view.findChild<QPushButton*>("stopProcess");
    view.selectProcess("slow");row(list,"next")->setSelected(true);stop->click();
    QTRY_COMPARE(requests(dir.filePath("hgs.log"),true).size(),1);
    auto newer=data;newer["host_generation"]="new-generation";view.setSession("arch","dsh/project","",newer,true,true);
    QTest::qWait(1000);QCOMPARE(requests(dir.filePath("hgs.log"),true).size(),1);
    QVERIFY(view.findChild<QLabel*>("processStopNotice")->isHidden());
}
void TestProcessView::delayedLoadingAndQuietCompletedOutput(){
    // Polls every 0.3 s instead of 2.5 s; the loading indicator keeps its 0.5 s delay.
    ProcessSettings::setPollFloorForTests(0.3);const auto restoreFloor=qScopeGuard([]{ProcessSettings::setPollFloorForTests(0);});
    QSettings().setValue("processes/activeSeconds",0.3);constexpr int pollMs=300;
    QTemporaryDir dir;QVERIFY(fixture(dir.filePath("hgs")));HgsClient client(FakeCli::program(dir.filePath("hgs")));ProcessView view(&client);view.resize(900,620);view.show();
    auto done=job("fast",false,"completed");auto data=inspection({done,job("slow",false,"completed")});view.setSession("arch","dsh/project","",data,true,true);
    auto *busy=view.findChild<QWidget*>("processBusy");auto *output=view.findChild<QPlainTextEdit*>("processOutput");
    QVERIFY(busy->isHidden());QTRY_COMPARE(output->toPlainText(),QString("fast output"));QVERIFY(busy->isHidden());
    done["updated_at"]=99;data=inspection({done,job("slow",false,"completed")});view.setSession("arch","dsh/project","",data,true,true);
    QTest::qWait(3*pollMs);QCOMPARE(requests(dir.filePath("hgs.log"),false).size(),1);QVERIFY(busy->isHidden());
    view.selectProcess("slow");QTest::qWait(250);QVERIFY(busy->isHidden());QTRY_VERIFY(busy->isVisible());
    QTRY_COMPARE(output->toPlainText(),QString("slow output"));QVERIFY(busy->isHidden());
    view.hide();const auto count=requests(dir.filePath("hgs.log"),false).size();QTest::qWait(3*pollMs);QCOMPARE(requests(dir.filePath("hgs.log"),false).size(),count);
}
QTEST_MAIN(TestProcessView)
#include "test_processview.moc"
