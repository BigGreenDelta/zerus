#include <QtTest>
#include <QCheckBox>
#include <QFile>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QTableWidget>
#include <QTemporaryDir>
#include "AutomaticApprovals.h"
#include "AutomaticApprovalSettings.h"
#include "FakeCli.h"

namespace {
QString fixture(const QTemporaryDir &dir, const QByteArray &mode = {}) {
    const auto path = dir.filePath("hgs");
    if (!FakeCli::install(path, R"PY(#!/usr/bin/env python3
import sys,json,pathlib,hashlib,uuid,time,os
root=pathlib.Path(__file__);a=sys.argv[1:];host='local'
def event(kind,payload=None):
 with open(str(root)+'.events','a',encoding='utf-8') as f:f.write(json.dumps(dict(kind=kind,host=host,payload=payload),ensure_ascii=False)+'\n')
mode=pathlib.Path(str(root)+'.mode').read_text()
if a==['--help']:
 print('hgs approval-log begin|finish|list|detail' if mode!='old' else 'usage: hgs old fixture');sys.exit()
if a and a[0].startswith('@'):host=a.pop(0)[1:]
q=dict(approval=True,can_answer=True,approval_choices=True,source='claude_tool_approval',answer_transport='claude_tui',
 question_id='question-'+host,question_hash='a'*64,run_id='run-'+host,conversation_id='conversation-'+host,
 questions=[dict(id='approval',question='Run this tool?',body='printf fixture',allow_other=False,
 options=[dict(id='yes',label='Yes',scope='once'),dict(id='always',label='Always allow',scope='broader'),dict(id='no',label='No',scope='once')])])
if mode=='opencode':
 q.update(source='opencode_native',answer_transport='native_opencode',kind='opencode_permission')
 q['questions'][0].update(id='permission',options=[dict(id='once',label='Allow once'),dict(id='always',label='Always allow')])
if mode=='dsh':
 q.update(source='dsh_native',answer_transport='native_dsh')
 q['questions'][0].update(id='decision',options=[dict(id='allow',label='Allow once'),dict(id='deny',label='Reject')])
if mode=='child':q.update(agent_id='workflow-child',child_request_id='request-child')
if a[0]=='inspect':
 event('inspect',a)
 if mode=='trust':q['trust_request']=True
 print(json.dumps(dict(runtime_state='live',process_state='running',run_id=q['run_id'],conversation_id=q['conversation_id'],pending_questions=[q],events=[],cursor=0)))
elif a[0]=='answer':
 p=json.load(sys.stdin);event('answer',p)
 if mode=='uncertain':print('delivery uncertain',file=sys.stderr);sys.exit(7)
 print(json.dumps(dict(status='answered',request_id=p['request_id'],name=a[1],question_id=p['question_id'],question_hash=p['expected_question_hash'],run_id=p['expected_run_id'],conversation_id=p['expected_conversation_id'])))
elif a[0]=='approval-log':
 op=a[1];p=json.load(sys.stdin);event(op,p)
 if op=='begin':
  if mode=='journal-fail':print('fixture journal unavailable',file=sys.stderr);sys.exit(1)
  identity=json.dumps([p['host'],p['question']['run_id'],p['question']['conversation_id'],p['question']['question_id'],p['question']['question_hash']])
  path=str(root)+'.reservation-'+hashlib.sha256(identity.encode()).hexdigest()
  record=dict(id=int(hashlib.sha256(identity.encode()).hexdigest()[:8],16),request_id=str(uuid.uuid4()),status='prepared')
  try:
   with open(path,'x') as f:json.dump(record,f)
   fresh=True
  except FileExistsError:record=json.load(open(path));fresh=False
  event('reserved',record)
  if mode=='hold':time.sleep(.3)
  item=p['question']['questions'][0]
  print(json.dumps(dict(eligible=True,fresh=fresh,answers=[dict(question_id=item['id'],selected_option_ids=[item['options'][0]['id']],text='')],**record)))
 elif op=='finish':
  for path in root.parent.glob(root.name+'.reservation-*'):
   record=json.load(open(path))
   if record['request_id']==p['request_id']:record['status']=p['status'];path.write_text(json.dumps(record))
  print(json.dumps(dict(id=p['id'],status=p['status'])))
 elif op=='list':
  if p.get('after'):
   first=min(120,p['after']+50);last=p['after'];previous=first if first<120 else None;next=last+1
  else:
   first=p.get('before',121)-1;last=max(0,first-50);previous=first if p.get('before') else None;next=last+1 if last else None
  rows=[dict(id=i,at=100000+i,host='desktop-a',session=p.get('session') or 'claude/sample/review',provider='claude',prompt='Run this tool?',status='approved') for i in range(first,last,-1)]
  print(json.dumps(dict(entries=rows,next=next,previous=previous)))
 elif op=='detail':
  q['questions'][0]['body']='Question '+str(p['id'])+'\nprintf fixture'
  print(json.dumps(dict(id=p['id'],question=q,answers=[dict(question_id='approval',selected_option_ids=['yes'])],status='approved',detail='')))
else:event('unexpected',a);sys.exit(86)
)PY")) return {};
    QFile file(path + ".mode"); if (!file.open(QIODevice::WriteOnly)) return {}; file.write(mode); return path;
}
QList<QJsonObject> events(const QString &path, const QString &kind) {
    QFile file(path + ".events"); if (!file.open(QIODevice::ReadOnly)) return {};
    QList<QJsonObject> result; for (const auto &line : file.readAll().split('\n')) {
        const auto item = QJsonDocument::fromJson(line).object(); if (item.value("kind") == kind) result.append(item);
    } return result;
}
FleetState fleet(int quiet = 0) {
    BoxState box; box.ok = true; box.host = "desktop-a";
    SessionInfo session; session.tracked = true; session.name = "claude/sample/review"; session.cmd = "claude"; session.phase = "approval";
    session.runId = "run-local"; session.conversationId = "conversation-local"; session.created = 1;
    for (int i = 0; i < quiet; ++i) { auto idle = session; idle.name = "claude/sample/" + QString::number(i); idle.phase = "idle"; box.sessions.append(idle); }
    box.sessions.append(session); FleetState fleet; fleet.setLocal(box,QDateTime::currentMSecsSinceEpoch()); return fleet;
}
void fast(AutomaticApprovals &worker) {
    for (auto *timer : worker.findChildren<QTimer *>(QString(),Qt::FindDirectChildrenOnly)) timer->setInterval(1);
}
}

class TestAutomaticApprovals : public QObject {
    Q_OBJECT
    QTemporaryDir settings;
private slots:
    void initTestCase() {
        QCoreApplication::setOrganizationName("hgs-tests"); QCoreApplication::setApplicationName("automatic-approvals");
        QSettings::setDefaultFormat(QSettings::IniFormat); QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,settings.path());
    }
    void init() { QSettings().clear(); }
    void disabledByDefaultAndOldCliNeverReceivesANewCommand() {
        QTemporaryDir dir; const auto path=fixture(dir,"old"); QVERIFY(!path.isEmpty());
        AutomaticApprovals worker(FakeCli::program(path)); worker.setFleet(fleet()); fast(worker);
        QVERIFY(!AutomaticApprovalPolicy::enabled()); QTest::qWait(100); QVERIFY(events(path,"inspect").isEmpty());
        AutomaticApprovalPolicy::setEnabled(true);
        QTRY_VERIFY(!AutomaticApprovalPolicy::error().isEmpty()); QVERIFY(events(path,"begin").isEmpty()); QVERIFY(events(path,"answer").isEmpty());
        QVERIFY(events(path,"unexpected").isEmpty());
    }
    void discoversALateSessionAndReservesBeforeDeliveryWithoutReplay() {
        QTemporaryDir dir; const auto path=fixture(dir); QVERIFY(!path.isEmpty()); AutomaticApprovalPolicy::setEnabled(true);
        {
            AutomaticApprovals worker(FakeCli::program(path)); worker.setFleet(fleet(20000)); fast(worker);
            QTRY_COMPARE_WITH_TIMEOUT(events(path,"finish").size(),1,15000);
            QCOMPARE(events(path,"answer").size(),1);
            const auto reserved=events(path,"reserved").first().value("payload").toObject();
            const auto answer=events(path,"answer").first().value("payload").toObject();
            QCOMPARE(answer.value("request_id"),reserved.value("request_id"));
            QCOMPARE(answer.value("expected_run_id").toString(),QString("run-local"));
            QCOMPARE(answer.value("answers").toArray().first().toObject().value("selected_option_ids").toArray(),QJsonArray{"yes"});
            QCOMPARE(events(path,"finish").first().value("payload").toObject().value("status").toString(),QString("approved"));
        }
        AutomaticApprovals restarted(FakeCli::program(path)); restarted.setFleet(fleet()); fast(restarted);
        QTRY_COMPARE(events(path,"begin").size(),2); QTest::qWait(100); QCOMPARE(events(path,"answer").size(),1);
    }
    void disablingOrRebindingBeforeDeliveryCancels_data() {
        QTest::addColumn<bool>("disable"); QTest::newRow("disable")<<true; QTest::newRow("new-run")<<false;
    }
    void disablingOrRebindingBeforeDeliveryCancels() {
        QFETCH(bool,disable); QTemporaryDir dir; const auto path=fixture(dir,"hold"); QVERIFY(!path.isEmpty());
        AutomaticApprovalPolicy::setEnabled(true); AutomaticApprovals worker(FakeCli::program(path)); worker.setFleet(fleet()); fast(worker);
        QTRY_COMPARE(events(path,"reserved").size(),1);
        if (disable) AutomaticApprovalPolicy::setEnabled(false);
        else { auto changed=fleet();auto local=changed.local();local.sessions[0].runId="replacement";changed.setLocal(local,QDateTime::currentMSecsSinceEpoch());worker.setFleet(changed); }
        QTRY_COMPARE(events(path,"finish").size(),1); QVERIFY(events(path,"answer").isEmpty());
        QCOMPARE(events(path,"finish").first().value("payload").toObject().value("status").toString(),QString("cancelled"));
    }
    void journalFailureAndTrustNeverAuthorizeInput_data() {
        QTest::addColumn<QByteArray>("mode"); QTest::newRow("journal-failure")<<QByteArray("journal-fail");QTest::newRow("trust")<<QByteArray("trust");
    }
    void journalFailureAndTrustNeverAuthorizeInput() {
        QFETCH(QByteArray,mode);QTemporaryDir dir;const auto path=fixture(dir,mode);QVERIFY(!path.isEmpty());
        AutomaticApprovalPolicy::setEnabled(true);AutomaticApprovals worker(FakeCli::program(path));worker.setFleet(fleet());fast(worker);
        QTRY_COMPARE(events(path,"inspect").size(),1);
        if(mode=="journal-fail")QTRY_VERIFY(!AutomaticApprovalPolicy::error().isEmpty());else QTest::qWait(150);
        QVERIFY(events(path,"answer").isEmpty());
    }
    void uncertainDeliveryStaysInTheJournalWithoutReplay() {
        QTemporaryDir dir;const auto path=fixture(dir,"uncertain");QVERIFY(!path.isEmpty());AutomaticApprovalPolicy::setEnabled(true);
        {
            AutomaticApprovals worker(FakeCli::program(path));worker.setFleet(fleet());fast(worker);QTRY_COMPARE(events(path,"finish").size(),1);
            QCOMPARE(events(path,"finish").first().value("payload").toObject().value("status").toString(),QString("unknown"));
        }
        AutomaticApprovals worker(FakeCli::program(path));worker.setFleet(fleet());fast(worker);QTRY_COMPARE(events(path,"begin").size(),2);
        QTest::qWait(100);QCOMPARE(events(path,"answer").size(),1);
    }
    void nativeProvidersAndRemoteFreshness_data() {
        QTest::addColumn<QByteArray>("provider");
        QTest::newRow("claude")<<QByteArray("claude"); QTest::newRow("workflow-child")<<QByteArray("child");
        QTest::newRow("opencode")<<QByteArray("opencode"); QTest::newRow("dsh")<<QByteArray("dsh");
    }
    void nativeProvidersAndRemoteFreshness() {
        QFETCH(QByteArray,provider); QTemporaryDir dir;const auto path=fixture(dir,provider);QVERIFY(!path.isEmpty());
        auto state=fleet();auto remote=state.local();remote.host="remote-a";
        remote.sessions[0].cmd=provider=="child"?"claude":QString::fromUtf8(provider);remote.sessions[0].runId="run-remote-a";remote.sessions[0].conversationId="conversation-remote-a";
        remote.sessions[0].phase=provider=="claude"?"approval":"input";
        if (provider=="child") { remote.sessions[0].phase="working";remote.sessions[0].subagents=QJsonObject{{"child",QJsonObject{{"state","working"},{"display_state","approval"}}}}; }
        state.setLocal(BoxState{},0);state.setPeer(remote,QDateTime::currentMSecsSinceEpoch()-31000);
        AutomaticApprovalPolicy::setEnabled(true);AutomaticApprovals worker(FakeCli::program(path));worker.setFleet(state);fast(worker);
        QTest::qWait(100);QVERIFY(events(path,"inspect").isEmpty());
        state.setPeer(remote,QDateTime::currentMSecsSinceEpoch());worker.setFleet(state);
        QTRY_COMPARE(events(path,"finish").size(),1);QCOMPARE(events(path,"answer").size(),1);
        const auto answer=events(path,"answer").first();QCOMPARE(answer.value("host").toString(),QString("remote-a"));
        const auto selected=answer.value("payload").toObject().value("answers").toArray().first().toObject().value("selected_option_ids").toArray();
        QCOMPARE(selected,QJsonArray{provider=="dsh"?"allow":provider=="opencode"?"once":"yes"});
        if (provider=="child") QCOMPARE(events(path,"begin").first().value("payload").toObject().value("question").toObject().value("child_request_id").toString(),QString("request-child"));
    }
    void settingsConfirmationAndBoundedJournalNavigation() {
        QTemporaryDir dir;const auto path=fixture(dir);QVERIFY(!path.isEmpty());
        AutomaticApprovalSettings page(FakeCli::program(path));page.resize(1000,800);page.show();page.refresh();
        auto *check=page.findChild<QCheckBox *>("automaticApprovalsEnabled");auto *table=page.findChild<QTableWidget *>("approvalJournal");
        QVERIFY(!check->isChecked());QTRY_COMPARE(table->rowCount(),50);
        QTimer::singleShot(0,&page,[&]{auto *confirm=page.findChild<QMessageBox *>("confirmAutomaticApprovals");QVERIFY(confirm);QCOMPARE(confirm->defaultButton(),confirm->button(QMessageBox::Cancel));confirm->reject();});
        check->click();QVERIFY(!AutomaticApprovalPolicy::enabled());QVERIFY(!check->isChecked());
        QTimer::singleShot(0,&page,[&]{auto *confirm=page.findChild<QMessageBox *>("confirmAutomaticApprovals");QVERIFY(confirm);for(auto *button:confirm->buttons())if(confirm->buttonRole(button)==QMessageBox::AcceptRole)button->click();});
        check->click();QVERIFY(AutomaticApprovalPolicy::enabled());check->click();QVERIFY(!AutomaticApprovalPolicy::enabled());
        QCOMPARE(table->item(0,0)->data(Qt::UserRole).toLongLong(),120);
        page.findChild<QPushButton *>("olderApprovalJournal")->click();QTRY_COMPARE(table->item(0,0)->data(Qt::UserRole).toLongLong(),70);QCOMPARE(table->rowCount(),50);
        page.findChild<QPushButton *>("newerApprovalJournal")->click();QTRY_COMPARE(table->item(0,0)->data(Qt::UserRole).toLongLong(),120);
        table->setCurrentCell(0,0);auto *detail=page.findChild<QPlainTextEdit *>("approvalJournalDetail");QTRY_VERIFY(detail->toPlainText().contains("Question 120"));QVERIFY(detail->toPlainText().contains("Selected answer: Yes"));
        page.findChild<QLineEdit *>("approvalJournalSession")->setText("claude/sample/42");
        QTRY_COMPARE(events(path,"list").last().value("payload").toObject().value("session").toString(),QString("claude/sample/42"));
        QTRY_COMPARE(table->item(0,2)->text(),QString("claude/sample/42"));QCOMPARE(table->rowCount(),50);
    }
};
QTEST_MAIN(TestAutomaticApprovals)
#include "test_automaticapprovals.moc"
