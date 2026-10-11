#include <QtTest>
#include <QFile>
#include <QFileDevice>
#include <QTemporaryDir>
#include <QTextStream>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QThread>
#include <atomic>
#include <csignal>
#include <memory>
#include "HgsClient.h"
#include "ProcessRunner.h"
#include "SessionPresentation.h"

class TestHgsClient : public QObject {
    Q_OBJECT
private slots:
    void parsesBox();
    void parsesSharedReadState();
    void readMarksUseStdinAndAcceptOnlyHostAnswers();
    void cliVersionUsesConfiguredExecutableAndRefreshes();
    void parsesSavedSessions();
    void parsesActivityMetadata();
    void providerFailuresRequireAttentionAndPreserveSpecificStatus();
    void scheduledAgentsShowTheirNextRunInsteadOfReady();
    void questionReplyPreviews_data();
    void questionReplyPreviews();
    void unrecognizedQuestionPreviewsStayLiteral();
    void archiveIdentityAndCommands();
    void archiveInspectionKeepsDistinctIdentities();
    void renameCommands();
    void forkCommandsPinSourceIdentity();
    void preservesArgumentBytes();
    void parsesOfflineBox();
    void parsesFleetArray();
    void rejectsGarbage();
    void tagAndProjectMayBeNull();
    void missingBinaryEmitsFailed();
    void duplicatePeerRequestIsIgnoredWhileInFlight();
    void localPollingReportsStartAndRecoversFromFailure();
    void presenceWritesAreCoalescedAndReportFailures();
    void sessionCommands();
    void inspectionCursorAndFailures();
    void remoteFoldersAndProjects();
    void parsesProjects();
    void rejectsGarbageProjects();
    void messagesUseStdinAndIdentityReceipts();
    void messageFailuresAreExplicitAndNeverRetried();
    void questionAnswersUsePinnedIdentityAndStdin();
    void submittedQuestionReceiptsRequireOptionalCodexAndExactIdentity();
    void questionAnswerFailures_data();
    void questionAnswerFailures();
    void unconfirmedQuestionCannotStartTransport();
    void effortUsesStdinAndPinnedIdentityReceipt();
    void settingsUseStdinAndPinnedIdentityReceipt();
    void settingsRejectMismatchedReceipt_data();
    void settingsRejectMismatchedReceipt();
    void effortFailures_data();
    void effortFailures();
    void effortLateResponsesKeepRequestIdentity();
    void terminationPinsRun();
    void nativeLaunchReportsProcessResult();
    void attachmentsUseCapturedIdentityAndValidateResponses();
    void gitStatusReadsAreSharedAndScopedToMachineAndFolder();
    void pollsLaunchOffTheGuiThreadAndReportOnIt();
    void destroyedClientKillsItsPolls();
};

void TestHgsClient::cliVersionUsesConfiguredExecutableAndRefreshes()
{
    QTemporaryDir fixture;QVERIFY(fixture.isValid());
    const auto program=fixture.filePath("custom-hgs");
    HgsClient client(program);QSignalSpy ready(&client,&HgsClient::versionReady);
    client.requestVersion();QTRY_COMPARE(ready.size(),1);QVERIFY(ready[0][0].toString().isEmpty());
    QFile script(program);QVERIFY(script.open(QIODevice::WriteOnly));
    script.write("#!/bin/sh\n[ \"$#\" = 1 ] && [ \"$1\" = --version ] || exit 7\necho call >> \"$0.calls\"\ncat \"$0.version\"\n");script.close();
    QVERIFY(script.setPermissions(QFileDevice::ReadOwner|QFileDevice::WriteOwner|QFileDevice::ExeOwner));
    auto reported=[&](const QByteArray &value){QFile file(program+".version");QVERIFY(file.open(QIODevice::WriteOnly));file.write(value);};
    reported("hgs 9.8.7\n");client.requestVersion();client.requestVersion();QTRY_COMPARE(ready.size(),2);
    QCOMPARE(ready[1][0].toString(),QString("9.8.7"));
    QFile calls(program+".calls");QVERIFY(calls.open(QIODevice::ReadOnly));QCOMPARE(calls.readAll(),QByteArray("call\n"));calls.close();
    reported("hgs 9.8.8-preview.1+fixture\n");client.requestVersion();QTRY_COMPARE(ready.size(),3);
    QCOMPARE(ready[2][0].toString(),QString("9.8.8-preview.1+fixture"));
    reported("An unrelated tool version 9.8.8\n");client.requestVersion();QTRY_COMPARE(ready.size(),4);
    QVERIFY(ready[3][0].toString().isEmpty());
}

void TestHgsClient::pollsLaunchOffTheGuiThreadAndReportOnIt()
{
    QTemporaryDir fixture; QVERIFY(fixture.isValid());
    const auto program = fixture.filePath("hgs"); QFile file(program); QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("#!/bin/sh\necho '{\"host\":\"arch\",\"ok\":true,\"sessions\":[]}'\n"); file.close();
    QVERIFY(file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
    std::atomic<QThread *> launchThread{nullptr};
    ProcessRunner::setLaunchObserver([&launchThread] { launchThread = QThread::currentThread(); });
    HgsClient client(program); QThread *readyThread = nullptr; int ready = 0;
    connect(&client, &HgsClient::localReady, this, [&](const BoxState &box) { readyThread = QThread::currentThread(); ready += box.host == "arch"; });
    client.requestLocal();
    QTRY_COMPARE(ready, 1);
    ProcessRunner::setLaunchObserver({});
    QVERIFY(launchThread.load() && launchThread.load() != QThread::currentThread());
    QCOMPARE(readyThread, QThread::currentThread());
}

void TestHgsClient::destroyedClientKillsItsPolls()
{
    QTemporaryDir fixture; QVERIFY(fixture.isValid());
    const auto program = fixture.filePath("hgs"); QFile file(program); QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("#!/bin/sh\necho $$ > \"$0.pid\"\nexec sleep 30\n"); file.close();
    QVERIFY(file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
    auto client = std::make_unique<HgsClient>(program); int reports = 0;
    connect(client.get(), &HgsClient::peerReady, this, [&reports] { ++reports; });
    connect(client.get(), &HgsClient::stateReadFailed, this, [&reports] { ++reports; });
    client->requestPeer("arch");
    QFile pidFile(program + ".pid");
    QTRY_VERIFY(pidFile.exists() && pidFile.size() > 0);
    QVERIFY(pidFile.open(QIODevice::ReadOnly)); const auto pid = pidFile.readAll().trimmed().toLongLong(); QVERIFY(pid > 0);
    // The old destructor killed and reaped child QProcesses; owners of worker
    // launches keep that guarantee and receive no late report.
    client.reset();
    QVERIFY(::kill(pid_t(pid), 0) != 0);
    QTest::qWait(100);
    QCOMPARE(reports, 0);
}

void TestHgsClient::gitStatusReadsAreSharedAndScopedToMachineAndFolder()
{
    QTemporaryDir fixture; QVERIFY(fixture.isValid());
    const auto program = fixture.filePath("hgs"); QFile file(program); QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("#!/bin/sh\nprintf '%s\\n' \"$*\" >> \"$0.calls\"\nsleep 0.1\nprintf '%s\\n' '{\"state\":\"ok\",\"root\":\"/repo\",\"branch\":\"main\",\"changed_files\":4,\"ahead\":2,\"behind\":0,\"remote_state\":\"verified\",\"remote_age_seconds\":0}'\n");
    file.close(); QVERIFY(file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
    HgsClient client(program); QSignalSpy changed(&client, &HgsClient::gitStatusChanged);
    client.requestGitStatus({}, "/repo"); client.requestGitStatus({}, "/repo");
    client.requestGitStatus("peer", "/repo"); client.requestGitStatus("peer", "/repo");
    QVERIFY(client.gitStatusSnapshot({}, "/repo").isEmpty());
    QTRY_COMPARE(changed.size(), 2);
    QCOMPARE(client.gitStatusSnapshot({}, "/repo")["changed_files"].toInt(), 4);
    QCOMPARE(client.gitStatusSnapshot("peer", "/repo")["ahead"].toInt(), 2);
    QVERIFY(client.gitStatusSnapshot("other", "/repo").isEmpty());
    QVERIFY(client.gitStatusSnapshot({}, "/other").isEmpty());
    client.requestGitStatus({}, "/repo"); QTest::qWait(150);
    QFile calls(program + ".calls"); QVERIFY(calls.open(QIODevice::ReadOnly));
    const auto lines = calls.readAll(); QCOMPARE(lines.count('\n'), 2);
    QVERIFY(lines.contains("git-status --path /repo --json"));
    QVERIFY(lines.contains("@peer git-status --path /repo --json"));
}

void TestHgsClient::questionReplyPreviews_data()
{
    QTest::addColumn<QString>("prompt");
    QTest::addColumn<QString>("preview");
    const QString answer = "Keep **literal** <b>markup</b> & `code`\nReview the desktop changes.";
    const QJsonObject reply{{"questionItemId", "reply-one"}, {"question", QString(300, 'q')}, {"answer", answer}};
    const auto envelope = [](const QJsonDocument &document) {
        return "<send_user_message_question_reply>\n" + QString::fromUtf8(document.toJson(QJsonDocument::Compact))
            + "\n</send_user_message_question_reply>";
    };
    const QString expected = "Your answer: " + answer;
    QTest::newRow("object") << envelope(QJsonDocument(reply)) << expected;
    QTest::newRow("array") << envelope(QJsonDocument(QJsonArray{reply})) << expected;
    QTest::newRow("ide") << "# Context from my IDE setup:\nOpen files: src/example.rs\n## My request for Codex:\n"
        + envelope(QJsonDocument(QJsonArray{reply})) << expected;
    auto second = reply; second["questionItemId"] = "reply-two"; second["answer"] = "Include macOS";
    QTest::newRow("multiple") << envelope(QJsonDocument(QJsonArray{reply, second})) << "Your answers: " + answer + " / Include macOS";
    second["answer"] = "";
    QTest::newRow("empty-answer") << envelope(QJsonDocument(second)) << QString("Your answer: (empty answer)");
    second["answer"] = QString::fromUtf8("Résumé 東京 ") + QString(300, 'a');
    QTest::newRow("long-unicode-answer") << envelope(QJsonDocument(second)) << "Your answer: " + second["answer"].toString();
}

void TestHgsClient::questionReplyPreviews()
{
    QFETCH(QString, prompt); QFETCH(QString, preview);
    SessionInfo session;session.tracked=true;session.phase="working";session.activity="busy";
    session.prompt=prompt;session.activitySummary="Working";
    session.activityDetail=prompt.left(240)+QChar(0x2026);
    QCOMPARE(SessionPresentation::currentAction(session),"Working: "+preview);
    session.activityDetail=prompt;
    QCOMPARE(SessionPresentation::currentAction(session),"Working: "+preview);
    session.phase="input";
    QCOMPARE(SessionPresentation::currentAction(session),"Needs input: "+preview);
    session.phase="working";session.activitySummary.clear();session.activityDetail.clear();
    QCOMPARE(SessionPresentation::currentAction(session),"Working: "+preview);
    // A previous question reply must not replace a subsequent tool excerpt.
    session.activitySummary="Read";session.currentTool="Read";session.toolDetail="src/example.rs";session.activityDetail=session.toolDetail;
    QCOMPARE(SessionPresentation::currentAction(session),QString("Read: src/example.rs"));
    QCOMPARE(session.prompt,prompt);
}

void TestHgsClient::unrecognizedQuestionPreviewsStayLiteral()
{
    const QString start="<send_user_message_question_reply>\n";
    for(const auto &prompt:QStringList{"Review **these** changes.", start+"not json\n</send_user_message_question_reply>",
        start+"[{\"answer\":\"Unconfirmed excerpt", "Example: "+start+"[]\n</send_user_message_question_reply>"}) {
        SessionInfo session;session.tracked=true;session.phase="working";session.activity="busy";
        session.prompt=prompt;session.activitySummary="Working";session.activityDetail=prompt;
        QCOMPARE(SessionPresentation::currentAction(session),"Working: "+prompt);
        session.activitySummary.clear();session.activityDetail.clear();
        QCOMPARE(SessionPresentation::currentAction(session),"Working: "+prompt);
    }
}

void TestHgsClient::scheduledAgentsShowTheirNextRunInsteadOfReady()
{
    const auto next = QDateTime::currentDateTime().addSecs(600);
    QString error;
    auto box=HgsClient::parseBox(QJsonDocument(QJsonObject{{"host","local"},{"sessions",QJsonArray{QJsonObject{
        {"name","kimi/project/watch"},{"tracked",true},{"activity","idle"},{"phase","idle"},{"process_state","running"},
        {"schedules",QJsonObject{{"count",2},{"recurring",1},{"next_at",double(next.toSecsSinceEpoch())}}}}}}}).toJson(),&error);
    QVERIFY(error.isEmpty());
    auto session=box.sessions.first();
    QCOMPARE((QList<int>{session.scheduledJobs,session.recurringSchedules}),(QList<int>{2,1}));
    const auto time=SessionPresentation::nextScheduledRun(session);
    QVERIFY(!time.isEmpty());
    QCOMPARE(SessionPresentation::status(session),"Scheduled · "+time);
    QCOMPARE(SessionPresentation::currentAction(session),"Next run at "+time);
    QVERIFY(SessionPresentation::scheduled(session)); QVERIFY(SessionPresentation::active(session));
    QVERIFY(!SessionPresentation::working(session)); QVERIFY(!SessionPresentation::needsAttention(session));
    QVERIFY(!SessionPresentation::scheduled(session,false)); QCOMPARE(SessionPresentation::status(session,false),QString("Offline"));
    session.nextScheduledAt=QDateTime::currentSecsSinceEpoch()-5;
    QCOMPARE(SessionPresentation::status(session),QString("Scheduled · due"));
    QCOMPARE(SessionPresentation::currentAction(session),QString("Next run due"));
    session.nextScheduledAt=0;
    QCOMPARE(SessionPresentation::status(session),QString("Scheduled"));
    // A question or work outranks the schedule; no jobs or an unknown journal is Ready.
    session.phase="input"; QCOMPARE(SessionPresentation::status(session),QString("Needs input")); session.phase="idle";
    session.activity="busy"; QCOMPARE(SessionPresentation::status(session),QString("Working")); session.activity="idle";
    session.scheduledJobs=0; QCOMPARE(SessionPresentation::status(session),QString("Ready"));
    session.scheduledJobs=-1; QCOMPARE(SessionPresentation::status(session),QString("Ready")); QVERIFY(!SessionPresentation::active(session));
}

void TestHgsClient::providerFailuresRequireAttentionAndPreserveSpecificStatus()
{
    QString error;
    auto box=HgsClient::parseBox(R"({"host":"local","sessions":[{"name":"codex/project/task","tracked":true,"activity":"attention","phase":"error","process_state":"running","provider_error":{"error_kind":"quota","detail":"7d limit reached"}}]})",&error);
    QVERIFY(error.isEmpty());QCOMPARE(box.sessions.size(),1);
    auto session=box.sessions.first();
    QVERIFY(session.needsAttention());QCOMPARE(SessionPresentation::status(session),QString("Usage limit reached"));
    session.activity="busy"; // Old tool metadata must not take precedence.
    QCOMPARE(SessionPresentation::status(session),QString("Usage limit reached"));
    session.recovery={{"state","waiting"},{"due_at",QDateTime::currentSecsSinceEpoch()+30}};
    QVERIFY(session.needsAction());QCOMPARE(SessionPresentation::status(session),QString("Usage limit reached"));
    session.recovery={};
    session.providerError={{"error_kind","provider_policy"},{"detail","Request flagged by provider policy for authorized security testing"}};
    session.activityDetail=session.providerError.value("detail").toString();
    QVERIFY(session.needsAction());QCOMPARE(SessionPresentation::status(session),QString("Request blocked by provider"));
    QCOMPARE(SessionPresentation::currentAction(session),QString("Request blocked by provider: ")+session.activityDetail);
    QCOMPARE(session.providerError.value("detail").toString(),QString("Request flagged by provider policy for authorized security testing"));
    session.recovery={{"state","waiting"},{"due_at",QDateTime::currentSecsSinceEpoch()+30}};
    QVERIFY(session.needsAttention());QVERIFY(session.needsAction());
    QCOMPARE(SessionPresentation::status(session),QString("Request blocked by provider"));
    session.recovery={};
    session.providerError={{"error_kind","authentication"}};
    QCOMPARE(SessionPresentation::status(session),QString("Sign-in failed"));
    session.phase="working";session.providerError={};session.recovery={};
    QVERIFY(!session.needsAction());QCOMPARE(SessionPresentation::status(session),QString("Working"));
    session.runId="run-one";session.conversationId="conversation-one";session.lastEventAt=100;
    QJsonObject details{{"tracked",true},{"run_id","run-one"},{"conversation_id","conversation-one"},
        {"last_event_at",101},{"phase","error"},{"activity","attention"},{"provider_error",QJsonObject{{"error_kind","quota"}}}};
    QCOMPARE(SessionPresentation::status(SessionPresentation::inspected(session,details)),QString("Usage limit reached"));
    details["last_event_at"]=99;
    QCOMPARE(SessionPresentation::status(SessionPresentation::inspected(session,details)),QString("Working"));
    details["last_event_at"]=101;details["run_id"]="another-run";
    QCOMPARE(SessionPresentation::status(SessionPresentation::inspected(session,details)),QString("Working"));
    details["run_id"]="run-one";details["conversation_id"]="another-conversation";
    QCOMPARE(SessionPresentation::status(SessionPresentation::inspected(session,details)),QString("Working"));
    details["conversation_id"]="conversation-one";session.providerStatusAt=102;details["provider_status_at"]=101;
    QCOMPARE(SessionPresentation::status(SessionPresentation::inspected(session,details)),QString("Working"));
}

void TestHgsClient::submittedQuestionReceiptsRequireOptionalCodexAndExactIdentity()
{
    QTemporaryDir directory;QFile script(directory.filePath("hgs"));QVERIFY(script.open(QIODevice::WriteOnly));
    script.write(R"(#!/usr/bin/env python3
import json,sys
p=json.load(sys.stdin)
receipt={'status':'submitted','request_id':p['request_id'],'name':sys.argv[-2],
 'run_id':p['expected_run_id'],'conversation_id':p['expected_conversation_id'],
 'question_id':p['question_id'],'question_hash':p['expected_question_hash']}
mode=p['answers'][0].get('text','')
if mode in receipt:receipt[mode]='unrelated'
print(json.dumps(receipt))
)");script.close();QVERIFY(script.setPermissions(QFileDevice::ReadOwner|QFileDevice::WriteOwner|QFileDevice::ExeOwner));
    HgsClient client(script.fileName());QSignalSpy submitted(&client,&HgsClient::questionAnswerSubmitted),
        answered(&client,&HgsClient::questionAnswered),failed(&client,&HgsClient::questionAnswerFailed);
    const QJsonObject question{{"question_id","pending-question"},{"question_hash","exact-content"},
        {"run_id","run-one"},{"conversation_id","conversation-one"},{"can_answer",true},
        {"can_skip",true},{"optional",true},{"source","codex_async"}};
    const auto send=[&](const QJsonObject &q,const QJsonObject &answer) {
        return client.requestAnswerQuestion({},"codex/project/main",q,QJsonArray{answer});
    };
    const auto requestId=send(question,QJsonObject{{"question_id","q_0"},{"text","Saved answer"}});
    QTRY_COMPARE(submitted.size(),1);QCOMPARE(submitted[0][0].toULongLong(),requestId);QCOMPARE(answered.size(),0);QCOMPARE(failed.size(),0);
    int failures=0;
    for(const auto &field:{"request_id","name","run_id","conversation_id","question_id","question_hash"}) {
        send(question,QJsonObject{{"question_id","q_0"},{"text",field}});++failures;QTRY_COMPARE(failed.size(),failures);
    }
    auto unsupported=question;unsupported["source"]="codex_tui";
    send(unsupported,QJsonObject{{"question_id","q_0"},{"text","answer"}});++failures;QTRY_COMPARE(failed.size(),failures);
    unsupported=question;unsupported["optional"]=false;
    send(unsupported,QJsonObject{{"question_id","q_0"},{"text","answer"}});++failures;QTRY_COMPARE(failed.size(),failures);
    send(question,QJsonObject{{"question_id","q_0"},{"skip",true}});++failures;QTRY_COMPARE(failed.size(),failures);
    QCOMPARE(submitted.size(),1);QCOMPARE(answered.size(),0);
}

void TestHgsClient::attachmentsUseCapturedIdentityAndValidateResponses()
{
    QTemporaryDir directory;QFile script(directory.filePath("hgs"));QVERIFY(script.open(QIODevice::WriteOnly));
    script.write(R"(#!/usr/bin/env python3
import json,pathlib,sys
args=sys.argv[1:]
pathlib.Path(__file__).with_name('attachment.json').write_text(json.dumps(args))
r=args[args.index('--request')+1]
print(json.dumps({'request_id':r if r!='wrong' else 'other','index':0,'name':'notes.txt','mime':'text/plain','data_base64':'bad!' if r=='bad' else 'aGVsbG8='}))
)");script.close();QVERIFY(script.setPermissions(QFileDevice::ReadOwner|QFileDevice::WriteOwner|QFileDevice::ExeOwner));
    HgsClient client(script.fileName());QSignalSpy ready(&client,&HgsClient::attachmentReady),failed(&client,&HgsClient::attachmentFailed);
    const QString name="kimi/project '$HOME' `literal`";
    const auto id=client.requestAttachment("mac",name,{{"request_id","one"},{"index",0}},"conversation","archive","child");
    QTRY_COMPARE(ready.size(),1);QCOMPARE(ready[0][0].toULongLong(),id);QCOMPARE(ready[0][2].toByteArray(),QByteArray("hello"));
    QFile args(directory.filePath("attachment.json"));QVERIFY(args.open(QIODevice::ReadOnly));
    QCOMPARE(QJsonDocument::fromJson(args.readAll()).array(),QJsonArray({"@mac","attachment",name,"--request","one","--index","0","--conversation","conversation","--archive","archive","--agent","child"}));
    client.requestAttachment({},name,{{"request_id","wrong"},{"index",0}},"conversation");QTRY_COMPARE(failed.size(),1);
    client.requestAttachment({},name,{{"request_id","bad"},{"index",0}},"conversation");QTRY_COMPARE(failed.size(),2);QCOMPARE(ready.size(),1);
}

void TestHgsClient::terminationPinsRun()
{
    QTemporaryDir directory;QFile script(directory.filePath("hgs"));QVERIFY(script.open(QIODevice::WriteOnly));
    script.write("#!/bin/sh\nprintf '%s\\n' \"$@\" >&2\n");script.close();
    QVERIFY(script.setPermissions(QFileDevice::ReadOwner|QFileDevice::WriteOwner|QFileDevice::ExeOwner));
    HgsClient client(script.fileName());QSignalSpy result(&client,&HgsClient::writeDone);
    client.terminateSession("mac","codex/project/one","exact-run");QTRY_COMPARE(result.size(),1);QVERIFY(result[0][1].toBool());
    QCOMPARE(result[0][2].toString(),QString("@mac\nterminate\ncodex/project/one\n--expected-run-id\nexact-run"));
}

void TestHgsClient::nativeLaunchReportsProcessResult()
{
    QTemporaryDir directory;QFile script(directory.filePath("hgs"));QVERIFY(script.open(QIODevice::WriteOnly));
    script.write(R"(#!/usr/bin/env python3
import json,pathlib,sys
pathlib.Path(__file__).with_name('launch.json').write_text(json.dumps(sys.argv[1:]))
if 'fail' in sys.argv:
 print('Official DeepSeek host failed to start',file=sys.stderr);sys.exit(1)
print('DeepSeek session ready')
)");
    script.close();QVERIFY(script.setPermissions(QFileDevice::ReadOwner|QFileDevice::WriteOwner|QFileDevice::ExeOwner));
    HgsClient client(script.fileName());QSignalSpy result(&client,&HgsClient::nativeSessionLaunched);
    const QString folder=QString::fromUtf8("/work/проект '$HOME' `literal`");
    client.launchNativeSession("mac",folder,"test","native-dsh","launch-one");
    QTRY_COMPARE(result.size(),1);QCOMPARE(result[0][0].toString(),QString("launch-one"));QVERIFY(result[0][1].toBool());
    QFile args(directory.filePath("launch.json"));QVERIFY(args.open(QIODevice::ReadOnly));
    QCOMPARE(QJsonDocument::fromJson(args.readAll()).array(),QJsonArray({"@mac","dsh",folder,"--new","-n","test","--launch-id","launch-one","--account","native-dsh"}));
    client.launchNativeSession({},folder,"fail",{},"launch-two");
    QTRY_COMPARE(result.size(),2);QCOMPARE(result[1][0].toString(),QString("launch-two"));QVERIFY(!result[1][1].toBool());
    QVERIFY(result[1][2].toString().contains("Official DeepSeek host failed"));
    args.close();
    client.launchDetachedSession("mac","codex",folder,"background","work","launch-three");
    QTRY_COMPARE(result.size(),3);QVERIFY(result[2][1].toBool());QVERIFY(args.open(QIODevice::ReadOnly));
    QCOMPARE(QJsonDocument::fromJson(args.readAll()).array(),QJsonArray({"@mac","codex",folder,"--new","-n","background","--launch-id","launch-three","-d","--account","work"}));
}

void TestHgsClient::settingsUseStdinAndPinnedIdentityReceipt()
{
    QTemporaryDir directory;
    QFile script(directory.filePath("hgs")); QVERIFY(script.open(QIODevice::WriteOnly));
    script.write(R"(#!/usr/bin/env python3
import json,pathlib,sys
p=json.load(sys.stdin)
pathlib.Path(__file__).with_name('settings.json').write_text(json.dumps({'argv':sys.argv[1:],'payload':p}))
print(json.dumps({'status':'scheduled','request_id':p['request_id'],'name':sys.argv[-2],
 'run_id':p['expected_run_id'],'conversation_id':p['expected_conversation_id'],
 'effort':p['effort'],'model':'gpt-6-astra','scope':'session'}))
)");
    script.close(); QVERIFY(script.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
    HgsClient client(script.fileName());
    QSignalSpy finished(&client, &HgsClient::settingsFinished), refreshed(&client, &HgsClient::sessionWriteDone);
    const QString name = QString::fromUtf8("codex/проект/Новый '$HOME' `literal`");
    const quint64 request = client.requestSettings("mac", name, "gpt-6-astra", "high", "run-identity", "conversation-identity", "pending-original");
    QTRY_COMPARE(finished.size(), 1); QCOMPARE(finished[0][0].toULongLong(), request);
    QVERIFY(finished[0][1].toBool()); QVERIFY(finished[0][3].toString().isEmpty());
    const auto receipt = finished[0][2].toJsonObject();
    QCOMPARE(receipt["name"].toString(), name); QCOMPARE(receipt["effort"].toString(), QString("high"));
    QCOMPARE(receipt["scope"].toString(), QString("session"));
    QCOMPARE(refreshed.size(), 1); QCOMPARE(refreshed[0][0].toString(), QString("mac"));
    QFile captured(directory.filePath("settings.json")); QVERIFY(captured.open(QIODevice::ReadOnly));
    const auto record = QJsonDocument::fromJson(captured.readAll()).object();
    QCOMPARE(record["argv"].toArray(), QJsonArray({"@mac", "settings", name, "--json"}));
    const auto payload = record["payload"].toObject();
    QCOMPARE(payload["expected_run_id"].toString(), QString("run-identity"));
    QCOMPARE(payload["expected_conversation_id"].toString(), QString("conversation-identity"));
    QCOMPARE(payload["effort"].toString(), QString("high"));
    QCOMPARE(payload["model"].toString(), QString("gpt-6-astra"));
    QCOMPARE(payload["expected_pending_id"].toString(), QString("pending-original"));
    QCOMPARE(payload["request_id"], receipt["request_id"]);
    QVERIFY(!payload["request_id"].toString().isEmpty());
}

void TestHgsClient::settingsRejectMismatchedReceipt_data()
{
    QTest::addColumn<QString>("mode");
    for (const QString field : {"request_id", "name", "run_id", "conversation_id", "effort", "model", "status",
                                "stale", "killed", "malformed", "array"})
        QTest::newRow(field.toUtf8()) << field;
}

void TestHgsClient::settingsRejectMismatchedReceipt()
{
    QFETCH(QString, mode);
    QTemporaryDir directory;
    QFile script(directory.filePath("hgs")); QVERIFY(script.open(QIODevice::WriteOnly));
    script.write(R"(#!/usr/bin/env python3
import json,pathlib,sys
p=json.load(sys.stdin)
with pathlib.Path(__file__).with_name('calls').open('a') as f:f.write('call\n')
mode=sys.argv[-2].rsplit('/',1)[-1]
if mode in ('stale','killed'):
 print('session identity changed; inspect Terminal',file=sys.stderr)
 sys.exit(137 if mode=='killed' else 1)
if mode=='malformed':
 print('not json');sys.exit(0)
if mode=='array':
 print('[]');sys.exit(0)
r={'status':'scheduled','request_id':p['request_id'],'name':sys.argv[-2],
 'run_id':p['expected_run_id'],'conversation_id':p['expected_conversation_id'],
 'effort':p['effort'],'model':p['model'],'scope':'session'}
r[mode]='different-value'
print(json.dumps(r))
)");
    script.close(); QVERIFY(script.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
    HgsClient client(script.fileName());
    QSignalSpy finished(&client, &HgsClient::settingsFinished), refreshed(&client, &HgsClient::sessionWriteDone);
    const auto request = client.requestSettings({}, "codex/project/" + mode, "gpt-6-astra", "high", "run", "conversation");
    QTRY_COMPARE(finished.size(), 1); QCOMPARE(finished[0][0].toULongLong(), request);
    QVERIFY(!finished[0][1].toBool()); QVERIFY(finished[0][2].toJsonObject().isEmpty());
    QVERIFY(!finished[0][3].toString().isEmpty()); QCOMPARE(refreshed.size(), 0);
    if (mode == "stale" || mode == "killed")
        QCOMPARE(finished[0][3].toString(), QString("session identity changed; inspect Terminal"));
    QTest::qWait(50); QCOMPARE(finished.size(), 1);
    QFile calls(directory.filePath("calls")); QVERIFY(calls.open(QIODevice::ReadOnly));
    QCOMPARE(calls.readAll(), QByteArray("call\n"));
}

void TestHgsClient::effortUsesStdinAndPinnedIdentityReceipt()
{
    QTemporaryDir directory;
    QFile script(directory.filePath("hgs")); QVERIFY(script.open(QIODevice::WriteOnly));
    script.write(R"(#!/usr/bin/env python3
import json,pathlib,sys
p=json.load(sys.stdin)
pathlib.Path(__file__).with_name('effort.json').write_text(json.dumps({'argv':sys.argv[1:],'payload':p}))
print(json.dumps({'status':'confirmed','request_id':p['request_id'],'name':sys.argv[-2],
 'run_id':p['expected_run_id'],'conversation_id':p['expected_conversation_id'],
 'effort':p['effort'],'model':'gpt-6-astra','scope':'session'}))
)");
    script.close(); QVERIFY(script.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
    HgsClient client(script.fileName());
    QSignalSpy finished(&client, &HgsClient::effortFinished), refreshed(&client, &HgsClient::sessionWriteDone);
    const QString name = QString::fromUtf8("codex/проект/Новый '$HOME' `literal`");
    const quint64 request = client.requestEffort("mac", name, "high", "run-identity", "conversation-identity");
    QTRY_COMPARE(finished.size(), 1); QCOMPARE(finished[0][0].toULongLong(), request);
    QVERIFY(finished[0][1].toBool()); QVERIFY(finished[0][3].toString().isEmpty());
    const auto receipt = finished[0][2].toJsonObject();
    QCOMPARE(receipt["name"].toString(), name); QCOMPARE(receipt["effort"].toString(), QString("high"));
    QCOMPARE(receipt["scope"].toString(), QString("session"));
    QCOMPARE(refreshed.size(), 1); QCOMPARE(refreshed[0][0].toString(), QString("mac"));
    QFile captured(directory.filePath("effort.json")); QVERIFY(captured.open(QIODevice::ReadOnly));
    const auto record = QJsonDocument::fromJson(captured.readAll()).object();
    QCOMPARE(record["argv"].toArray(), QJsonArray({"@mac", "effort", name, "--json"}));
    const auto payload = record["payload"].toObject();
    QCOMPARE(payload["expected_run_id"].toString(), QString("run-identity"));
    QCOMPARE(payload["expected_conversation_id"].toString(), QString("conversation-identity"));
    QCOMPARE(payload["effort"].toString(), QString("high"));
    QCOMPARE(payload["request_id"], receipt["request_id"]);
    QVERIFY(!payload["request_id"].toString().isEmpty());
}

void TestHgsClient::effortFailures_data()
{
    QTest::addColumn<QString>("mode");
    for (const QString field : {"request_id", "name", "run_id", "conversation_id", "effort", "status",
                                "stale", "killed", "malformed", "array"})
        QTest::newRow(field.toUtf8()) << field;
}

void TestHgsClient::effortFailures()
{
    QFETCH(QString, mode);
    QTemporaryDir directory;
    QFile script(directory.filePath("hgs")); QVERIFY(script.open(QIODevice::WriteOnly));
    script.write(R"(#!/usr/bin/env python3
import json,pathlib,sys
p=json.load(sys.stdin)
with pathlib.Path(__file__).with_name('calls').open('a') as f:f.write('call\n')
mode=sys.argv[-2].rsplit('/',1)[-1]
if mode in ('stale','killed'):
 print('session identity changed; inspect Terminal',file=sys.stderr)
 sys.exit(137 if mode=='killed' else 1)
if mode=='malformed':
 print('not json');sys.exit(0)
if mode=='array':
 print('[]');sys.exit(0)
r={'status':'confirmed','request_id':p['request_id'],'name':sys.argv[-2],
 'run_id':p['expected_run_id'],'conversation_id':p['expected_conversation_id'],
 'effort':p['effort'],'scope':'session'}
r[mode]='different-value'
print(json.dumps(r))
)");
    script.close(); QVERIFY(script.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
    HgsClient client(script.fileName());
    QSignalSpy finished(&client, &HgsClient::effortFinished), refreshed(&client, &HgsClient::sessionWriteDone);
    const auto request = client.requestEffort({}, "codex/project/" + mode, "high", "run", "conversation");
    QTRY_COMPARE(finished.size(), 1); QCOMPARE(finished[0][0].toULongLong(), request);
    QVERIFY(!finished[0][1].toBool()); QVERIFY(finished[0][2].toJsonObject().isEmpty());
    QVERIFY(!finished[0][3].toString().isEmpty()); QCOMPARE(refreshed.size(), 0);
    if (mode == "stale" || mode == "killed")
        QCOMPARE(finished[0][3].toString(), QString("session identity changed; inspect Terminal"));
    QTest::qWait(50); QCOMPARE(finished.size(), 1);
    QFile calls(directory.filePath("calls")); QVERIFY(calls.open(QIODevice::ReadOnly));
    QCOMPARE(calls.readAll(), QByteArray("call\n"));
}

void TestHgsClient::effortLateResponsesKeepRequestIdentity()
{
    QTemporaryDir directory;
    QFile script(directory.filePath("hgs")); QVERIFY(script.open(QIODevice::WriteOnly));
    script.write(R"(#!/usr/bin/env python3
import json,sys,time
p=json.load(sys.stdin)
if sys.argv[-2].endswith('/slow'):time.sleep(.3)
print(json.dumps({'status':'confirmed','request_id':p['request_id'],'name':sys.argv[-2],
 'run_id':p['expected_run_id'],'conversation_id':p['expected_conversation_id'],
 'effort':p['effort'],'scope':'session'}))
)");
    script.close(); QVERIFY(script.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
    HgsClient client(script.fileName());
    QSignalSpy finished(&client, &HgsClient::effortFinished), refreshed(&client, &HgsClient::sessionWriteDone);
    const auto slow = client.requestEffort({}, "codex/project/slow", "low", "run-old", "conversation-old");
    const auto fast = client.requestEffort("mac", "kimi/project/fast", "max", "run-new", "conversation-new");
    QVERIFY(slow != fast); QTRY_COMPARE(finished.size(), 2);
    QCOMPARE(finished[0][0].toULongLong(), fast); QCOMPARE(finished[1][0].toULongLong(), slow);
    QVERIFY(finished[0][1].toBool()); QVERIFY(finished[1][1].toBool());
    const auto fastReceipt = finished[0][2].toJsonObject(), slowReceipt = finished[1][2].toJsonObject();
    QCOMPARE(fastReceipt["name"].toString(), QString("kimi/project/fast"));
    QCOMPARE(fastReceipt["run_id"].toString(), QString("run-new"));
    QCOMPARE(fastReceipt["conversation_id"].toString(), QString("conversation-new"));
    QCOMPARE(fastReceipt["effort"].toString(), QString("max"));
    QCOMPARE(slowReceipt["name"].toString(), QString("codex/project/slow"));
    QCOMPARE(slowReceipt["run_id"].toString(), QString("run-old"));
    QCOMPARE(slowReceipt["conversation_id"].toString(), QString("conversation-old"));
    QCOMPARE(slowReceipt["effort"].toString(), QString("low"));
    QCOMPARE(refreshed.size(), 2); QCOMPARE(refreshed[0][0].toString(), QString("mac"));
    QCOMPARE(refreshed[1][0].toString(), QString());
    HgsClient missing(directory.filePath("missing")); QSignalSpy failed(&missing, &HgsClient::effortFinished);
    const auto absent = missing.requestEffort({}, "codex/p", "high", "run", "conversation");
    QTRY_COMPARE(failed.size(), 1); QCOMPARE(failed[0][0].toULongLong(), absent);
    QVERIFY(!failed[0][1].toBool()); QVERIFY(!failed[0][3].toString().isEmpty());
}

void TestHgsClient::questionAnswersUsePinnedIdentityAndStdin()
{
    QTemporaryDir directory;
    QFile script(directory.filePath("hgs")); QVERIFY(script.open(QIODevice::WriteOnly));
    script.write(R"(#!/usr/bin/env python3
import json,pathlib,sys,time
p=json.load(sys.stdin)
pathlib.Path(__file__).with_name('answer.json').write_text(json.dumps({'argv':sys.argv[1:],'payload':p}))
time.sleep(.1)
print(json.dumps({'status':'answered','request_id':p['request_id'],'name':sys.argv[-2],
 'run_id':p['expected_run_id'],'conversation_id':p['expected_conversation_id'],
 'question_id':p['question_id'],'question_hash':p['expected_question_hash']}))
)");
    script.close(); QVERIFY(script.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
    HgsClient client(script.fileName());
    QSignalSpy sent(&client, &HgsClient::questionAnswered), failed(&client, &HgsClient::questionAnswerFailed);
    const QJsonObject question{{"question_id", "pending-question"}, {"question_hash", "exact-content"},
        {"run_id", "run-one"}, {"conversation_id", "conversation-one"}, {"can_answer", true}};
    const QJsonArray answers{QJsonObject{{"question_id", "q_0"}, {"selected_option_ids", QJsonArray{"opt_0_1"}}, {"text", ""}},
        QJsonObject{{"question_id", "q_1"}, {"selected_option_ids", QJsonArray{}}, {"text", QString::fromUtf8("Свой ответ '$HOME' `literal`")} }};
    const QString name = QString::fromUtf8("kimi/проект/Вопросы");
    const auto request = client.requestAnswerQuestion("mac", name, question, answers);
    client.requestAnswerQuestion("mac", name, question, answers);
    QTRY_COMPARE(failed.size(), 1); QVERIFY(!failed[0][4].toBool());
    QTRY_COMPARE(sent.size(), 1); QCOMPARE(sent[0][0].toULongLong(), request);
    QCOMPARE(sent[0][1].toString(), QString("mac")); QCOMPARE(sent[0][2].toString(), name);
    QFile captured(directory.filePath("answer.json")); QVERIFY(captured.open(QIODevice::ReadOnly));
    const auto record = QJsonDocument::fromJson(captured.readAll()).object();
    QCOMPARE(record["argv"].toArray(), QJsonArray({"@mac", "answer", name, "--json"}));
    const auto payload = record["payload"].toObject();
    QCOMPARE(payload["answers"].toArray(), answers);
    QCOMPARE(payload["question_id"], question["question_id"]);
    QCOMPARE(payload["expected_question_hash"], question["question_hash"]);
    QCOMPARE(payload["expected_run_id"], question["run_id"]);
    QCOMPARE(payload["expected_conversation_id"], question["conversation_id"]);
    captured.close();auto optional=question;optional["optional"]=true;optional["can_skip"]=true;optional["source"]="codex_async";optional["can_answer"]=false;
    const QJsonArray skipped{QJsonObject{{"question_id","q_0"},{"skip",true}}};
    client.requestAnswerQuestion("mac",name,optional,skipped);QTRY_COMPARE(sent.size(),2);
    QVERIFY(captured.open(QIODevice::ReadOnly));QCOMPARE(QJsonDocument::fromJson(captured.readAll()).object()["payload"].toObject()["answers"].toArray(),skipped);
    optional["optional"]=false;client.requestAnswerQuestion("mac",name,optional,skipped);QTRY_COMPARE(failed.size(),2);
    const QString hookHash(64, QChar('b'));
    auto hooks=question;hooks["question_id"]="codex-hooks-trust:"+hookHash;hooks["question_hash"]=hookHash;
    hooks["source"]="codex_hooks_trust";hooks["answer_transport"]="codex_tui";hooks["conversation_id"]=QJsonValue::Null;
    const QJsonArray hookAnswer{QJsonObject{{"question_id","hooks_trust"},{"selected_option_ids",QJsonArray{"continue_without_trusting"}},{"text",""}}};
    client.requestAnswerQuestion({},"codex/project/startup",hooks,hookAnswer);QTRY_COMPARE(sent.size(),3);
    captured.close();QVERIFY(captured.open(QIODevice::ReadOnly));const auto hookPayload=QJsonDocument::fromJson(captured.readAll()).object()["payload"].toObject();
    QCOMPARE(hookPayload["expected_conversation_id"].toString(),QString());QCOMPARE(hookPayload["answers"].toArray(),hookAnswer);
    auto update=hooks;update["question_id"]="codex-update:"+hookHash;update["source"]="codex_update";
    const QJsonArray updateAnswer{QJsonObject{{"question_id","codex_update"},{"selected_option_ids",QJsonArray{"skip"}},{"text",""}}};
    client.requestAnswerQuestion({},"codex/project/startup",update,updateAnswer);QTRY_COMPARE(sent.size(),4);
    captured.close();QVERIFY(captured.open(QIODevice::ReadOnly));const auto updatePayload=QJsonDocument::fromJson(captured.readAll()).object()["payload"].toObject();
    QCOMPARE(updatePayload["question_id"].toString(),"codex-update:"+hookHash);QCOMPARE(updatePayload["answers"].toArray(),updateAnswer);
}

void TestHgsClient::unconfirmedQuestionCannotStartTransport()
{
    QTemporaryDir directory; QFile program(directory.filePath("hgs"));
    QVERIFY(program.open(QIODevice::WriteOnly));
    program.write("#!/bin/sh\nprintf 'called\\n' >> \"$0.calls\"\n"); program.close();
    QVERIFY(program.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
    HgsClient client(program.fileName());
    QSignalSpy sent(&client, &HgsClient::questionAnswered), failed(&client, &HgsClient::questionAnswerFailed);
    const QString hash(64, QChar('a'));
    const QList<QStringList> requests{{"kimi", "kimi-trust:", "kimi_folder_trust", "kimi_tui"},
        {"claude", "claude-trust:", "claude_folder_trust", "claude_tui"}, {"codex", "codex-trust:", "codex_folder_trust", "codex_tui"},
        {"claude-permissions", "claude-permissions:", "claude_permission_mode", "claude_tui"},
        {"codex-hooks", "codex-hooks-trust:", "codex_hooks_trust", "codex_tui"}, {"codex-update", "codex-update:", "codex_update", "codex_tui"}};
    for (const auto &kind : requests) {
    const QString provider = kind[0];
    const QJsonObject startup{{"question_id", kind[1] + hash}, {"question_hash", hash}, {"source", kind[2]}, {"answer_transport", kind[3]},
        {"run_id", "run"}, {"conversation_id", QJsonValue::Null}, {"can_answer", true}};
    const QList<QPair<QString, QJsonValue>> invalid{
        {"source", "kimi_wire"}, {"question_id", "other-question"},
        {"answer_transport", "other"}, {"question_hash", "different"},
        {"run_id", ""}, {"can_answer", false}};
    for (const auto &change : invalid) {
        auto question = startup; question[change.first] = change.second;
        const auto before = failed.size();
        client.requestAnswerQuestion({}, provider + "/startup", question,
            {QJsonObject{{"question_id", "trust"}, {"selected_option_ids", QJsonArray{"trust_0"}}}});
        QTRY_COMPARE(failed.size(), before + 1); QVERIFY(!failed.last()[4].toBool());
        QVERIFY(failed.last()[3].toString().contains("not ready"));
    }
    }
    QCOMPARE(sent.size(), 0); QVERIFY(!QFile::exists(program.fileName() + ".calls"));
}

void TestHgsClient::questionAnswerFailures_data()
{
    QTest::addColumn<QString>("mode"); QTest::addColumn<bool>("uncertain");
    QTest::newRow("stale-question") << QString("stale") << false;
    QTest::newRow("changed-during-delivery") << QString("uncertain") << true;
    QTest::newRow("terminated-remote") << QString("killed") << true;
    QTest::newRow("startup-receipt-missing-conversation") << QString("startup-missing-conversation") << true;
    for (const QString field : {"request_id", "name", "run_id", "conversation_id", "question_id", "question_hash", "status"})
        QTest::newRow(field.toUtf8()) << field << true;
}

void TestHgsClient::questionAnswerFailures()
{
    QFETCH(QString, mode); QFETCH(bool, uncertain);
    QTemporaryDir directory;
    QFile script(directory.filePath("hgs")); QVERIFY(script.open(QIODevice::WriteOnly));
    script.write(R"(#!/usr/bin/env python3
import json,pathlib,sys
p=json.load(sys.stdin)
with pathlib.Path(__file__).with_name('calls').open('a') as f:f.write('call\n')
mode=p['answers'][0]['text']
if mode in ['stale','uncertain','killed']:
 print('delivery uncertain: question changed' if mode=='uncertain' else 'question is no longer pending',file=sys.stderr)
 sys.exit(137 if mode=='killed' else 1)
r={'status':'answered','request_id':p['request_id'],'name':sys.argv[-2],
 'run_id':p['expected_run_id'],'conversation_id':p['expected_conversation_id'],
 'question_id':p['question_id'],'question_hash':p['expected_question_hash']}
if mode=='startup-missing-conversation': del r['conversation_id']
else: r[mode]='another-identity'
print(json.dumps(r))
)");
    script.close(); QVERIFY(script.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
    HgsClient client(script.fileName());
    QSignalSpy sent(&client, &HgsClient::questionAnswered), failed(&client, &HgsClient::questionAnswerFailed);
    QJsonObject question{{"question_id", "pending"}, {"question_hash", "hash"},
        {"run_id", "run"}, {"conversation_id", "conversation"}, {"can_answer", true}};
    if (mode == "startup-missing-conversation") {
        const QString hash(64, QChar('a'));
        question["conversation_id"] = QJsonValue::Null;
        question["source"] = "kimi_folder_trust"; question["answer_transport"] = "kimi_tui";
        question["question_hash"] = hash; question["question_id"] = "kimi-trust:" + hash;
    }
    client.requestAnswerQuestion({}, "kimi/project", question, {QJsonObject{{"question_id", "q_0"}, {"text", mode}}});
    QTRY_COMPARE(failed.size(), 1); QCOMPARE(failed[0][4].toBool(), uncertain); QCOMPARE(sent.size(), 0);
    QTest::qWait(30); QCOMPARE(failed.size(), 1);
    QFile calls(directory.filePath("calls")); QVERIFY(calls.open(QIODevice::ReadOnly)); QCOMPARE(calls.readAll(), QByteArray("call\n"));
}

void TestHgsClient::messagesUseStdinAndIdentityReceipts()
{
    QTemporaryDir directory;
    QFile script(directory.filePath("hgs")); QVERIFY(script.open(QIODevice::WriteOnly));
    script.write(R"(#!/usr/bin/env python3
import json,pathlib,sys
request=json.load(sys.stdin)
pathlib.Path(__file__).with_name('request.json').write_text(json.dumps({'argv':sys.argv[1:],'payload':request}))
print(json.dumps({'status':'submitted','request_id':request['request_id'],'name':sys.argv[-2],
 'run_id':request['expected_run_id'],'conversation_id':request['expected_conversation_id'],
 'submitted_text':request['text'],'submitted_at':123.5}))
)");
    script.close(); QVERIFY(script.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
    HgsClient client(script.fileName());
    QSignalSpy sent(&client, &HgsClient::messageSent), failed(&client, &HgsClient::messageFailed);
    QSignalSpy refreshed(&client, &HgsClient::sessionWriteDone);
    const QString name = QString::fromUtf8("codex/проект/Новый");
    const QString text = QString::fromUtf8("Привет\nsecond line $(echo secret) `literal`");
    const QByteArray bytes("\x89PNG\0test", 9);
    const QList<MessageAttachment> files{{QString::fromUtf8("снимок.png"), "image/png", bytes, "[Image #1]"}};
    const quint64 request = client.requestSendMessage("mac", name, text, files, "run-id", "conversation-id");
    QTRY_COMPARE(sent.size(), 1);
    QCOMPARE(failed.size(), 0); QCOMPARE(refreshed.size(), 1);
    QCOMPARE(sent[0][0].toULongLong(), request); QCOMPARE(sent[0][1].toString(), QString("mac"));
    QCOMPARE(sent[0][2].toString(), name);
    QFile captured(directory.filePath("request.json")); QVERIFY(captured.open(QIODevice::ReadOnly));
    const auto record = QJsonDocument::fromJson(captured.readAll()).object();
    QCOMPARE(record["argv"].toArray(), QJsonArray({"@mac", "send", name, "--json"}));
    const auto payload = record["payload"].toObject();
    QCOMPARE(payload["text"].toString(), text);
    QCOMPARE(payload["expected_run_id"].toString(), QString("run-id"));
    QCOMPARE(payload["expected_conversation_id"].toString(), QString("conversation-id"));
    QCOMPARE(payload["attachments"].toArray().size(), 1);
    QCOMPARE(payload["attachments"].toArray()[0].toObject()["reference"].toString(), QString("[Image #1]"));
    QCOMPARE(QByteArray::fromBase64(payload["attachments"].toArray()[0].toObject()["data_base64"].toString().toLatin1()), bytes);
}

void TestHgsClient::messageFailuresAreExplicitAndNeverRetried()
{
    QTemporaryDir directory;
    QFile script(directory.filePath("hgs")); QVERIFY(script.open(QIODevice::WriteOnly));
    script.write(R"(#!/usr/bin/env python3
import json,pathlib,sys,time
request=json.load(sys.stdin)
with pathlib.Path(__file__).with_name('calls').open('a') as file:file.write('call\n')
time.sleep(.15)
if request['text']=='refused':
 print('agent is busy',file=sys.stderr);sys.exit(1)
if request['text']=='uncertain':
 print('delivery uncertain: link dropped',file=sys.stderr);sys.exit(1)
if request['text']=='killed':
 print('remote process terminated',file=sys.stderr);sys.exit(137)
if request['text']=='exit127':
 print('program exited after input',file=sys.stderr);sys.exit(127)
print('{}')
)");
    script.close(); QVERIFY(script.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
    HgsClient client(script.fileName());
    QSignalSpy sent(&client, &HgsClient::messageSent), failed(&client, &HgsClient::messageFailed);
    client.requestSendMessage({}, "codex/p", "refused", {}, "run");
    QTRY_COMPARE(failed.size(), 1); QVERIFY(!failed[0][4].toBool());
    client.requestSendMessage({}, "codex/p", "uncertain", {}, "run");
    QTRY_COMPARE(failed.size(), 2); QVERIFY(failed[1][4].toBool());
    client.requestSendMessage({}, "codex/p", "killed", {}, "run");
    QTRY_COMPARE(failed.size(), 3); QVERIFY(failed[2][4].toBool());
    client.requestSendMessage({}, "codex/p", "exit127", {}, "run");
    QTRY_COMPARE(failed.size(), 4); QVERIFY(failed[3][4].toBool());
    const auto malformed = client.requestSendMessage({}, "codex/p", "bad receipt", {}, "run");
    const auto duplicate = client.requestSendMessage({}, "codex/p", "another", {}, "run");
    QTRY_COMPARE(failed.size(), 6);
    bool gotMalformed = false, gotDuplicate = false;
    for (const auto &values : failed) {
        if (values[0].toULongLong() == malformed) { gotMalformed = true; QVERIFY(values[4].toBool()); }
        if (values[0].toULongLong() == duplicate) { gotDuplicate = true; QVERIFY(!values[4].toBool()); }
    }
    QVERIFY(gotMalformed); QVERIFY(gotDuplicate); QCOMPARE(sent.size(), 0);
    QFile calls(directory.filePath("calls")); QVERIFY(calls.open(QIODevice::ReadOnly));
    QCOMPARE(calls.readAll(), QByteArray("call\ncall\ncall\ncall\ncall\n"));
    HgsClient absent(directory.filePath("missing"));
    QSignalSpy absentFailed(&absent, &HgsClient::messageFailed);
    absent.requestSendMessage({}, "codex/p", "hello", {}, "run");
    QTRY_COMPARE(absentFailed.size(), 1); QVERIFY(!absentFailed[0][4].toBool());
    HgsClient bareAbsent(QStringLiteral("hgs-certainly-missing-test-program"));
    QSignalSpy bareFailed(&bareAbsent, &HgsClient::messageFailed);
    bareAbsent.requestSendMessage({}, "codex/p", "hello", {}, "run");
    QTRY_COMPARE(bareFailed.size(), 1); QVERIFY(!bareFailed[0][4].toBool());
    QFile blocked(directory.filePath("blocked")); QVERIFY(blocked.open(QIODevice::WriteOnly));
    blocked.write("#!/bin/sh\nexit 0\n"); blocked.close();
    QVERIFY(blocked.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner));
    HgsClient unexecutable(blocked.fileName());
    QSignalSpy blockedFailed(&unexecutable, &HgsClient::messageFailed);
    unexecutable.requestSendMessage({}, "codex/p", "hello", {}, "run");
    QTRY_COMPARE(blockedFailed.size(), 1); QVERIFY(!blockedFailed[0][4].toBool());
}

void TestHgsClient::forkCommandsPinSourceIdentity()
{
    QTemporaryDir directory;
    QFile script(directory.filePath("hgs")); QVERIFY(script.open(QIODevice::WriteOnly));
    script.write(R"(#!/usr/bin/env python3
import json,pathlib,sys
with pathlib.Path(__file__).with_name('fork.jsonl').open('a') as f:f.write(json.dumps(sys.argv[1:])+'\n')
if 'rejected' in sys.argv:
 print('source conversation changed',file=sys.stderr);sys.exit(7)
)");
    script.close(); QVERIFY(script.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
    HgsClient client(script.fileName());
    QSignalSpy writes(&client, &HgsClient::writeDone), refresh(&client, &HgsClient::sessionWriteDone);
    const QString name = QString::fromUtf8("codex/проект/Исток"), tag = QString::fromUtf8("Новая '$HOME' `literal`");
    client.forkSession({}, name, tag, {}, "source-run", "source-conversation");
    QTRY_COMPARE(writes.size(), 1); QVERIFY(writes[0][1].toBool());
    client.forkSession("mac", name, tag, "archive-original", "archived-run", "archived-conversation");
    QTRY_COMPARE(writes.size(), 2); QVERIFY(writes[1][1].toBool());
    client.forkSession("mac", name, "rejected", {}, "stale-run", "stale-conversation");
    QTRY_COMPARE(writes.size(), 3); QVERIFY(!writes[2][1].toBool());
    QCOMPARE(writes[2][2].toString(), QString("source conversation changed"));
    QCOMPARE(refresh.size(), 3); QCOMPARE(refresh[0][0].toString(), QString());
    QCOMPARE(refresh[1][0].toString(), QString("mac")); QCOMPARE(refresh[2][0].toString(), QString("mac"));
    QFile captured(directory.filePath("fork.jsonl")); QVERIFY(captured.open(QIODevice::ReadOnly));
    const auto calls = captured.readAll().trimmed().split('\n'); QCOMPARE(calls.size(), 3);
    QCOMPARE(QJsonDocument::fromJson(calls[0]).array(), QJsonArray({"fork", name, "-n", tag, "-d",
        "--expected-run-id", "source-run", "--expected-conversation-id", "source-conversation"}));
    QCOMPARE(QJsonDocument::fromJson(calls[1]).array(), QJsonArray({"@mac", "fork", name, "-n", tag, "-d",
        "--archive", "archive-original", "--expected-run-id", "archived-run", "--expected-conversation-id", "archived-conversation"}));
    QCOMPARE(QJsonDocument::fromJson(calls[2]).array(), QJsonArray({"@mac", "fork", name, "-n", "rejected", "-d",
        "--expected-run-id", "stale-run", "--expected-conversation-id", "stale-conversation"}));
}

void TestHgsClient::renameCommands()
{
    QTemporaryDir dir; QFile script(dir.filePath("hgs")); QVERIFY(script.open(QIODevice::WriteOnly));
    script.write("#!/bin/sh\nprintf '%s\\n' \"$@\" >&2\ncase \"$*\" in *rejected*) exit 7;; esac\n"); script.close();
    QVERIFY(script.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
    HgsClient client(script.fileName()); QSignalSpy writes(&client, &HgsClient::writeDone);
    QSignalSpy refresh(&client, &HgsClient::sessionWriteDone);
    client.renameSession({}, "codex/p/old", "codex/p/Новый план"); QTRY_COMPARE(writes.size(), 1);
    QVERIFY(writes[0][1].toBool()); QCOMPARE(writes[0][2].toString(), QString("rename\ncodex/p/old\ncodex/p/Новый план"));
    client.renameSession("mac", "claude/p", "claude/p/new tag", "archive-id"); QTRY_COMPARE(writes.size(), 2);
    QVERIFY(writes[1][1].toBool()); QCOMPARE(writes[1][2].toString(), QString("@mac\nrename\nclaude/p\nclaude/p/new tag\n--archive\narchive-id"));
    client.renameSession("mac", "codex/p/old", "codex/p/rejected"); QTRY_COMPARE(writes.size(), 3);
    QVERIFY(!writes[2][1].toBool()); QCOMPARE(refresh.size(), 3);
    QCOMPARE(refresh[0][0].toString(), QString()); QCOMPARE(refresh[1][0].toString(), QString("mac"));
}

void TestHgsClient::preservesArgumentBytes()
{
    QTemporaryDir dir; QFile script(dir.filePath("hgs")); QVERIFY(script.open(QIODevice::WriteOnly));
    // Hex is ASCII on every platform and proves actual child argv bytes rather
    // than comparing text after a normalization step in the test fixture.
    script.write("#!/bin/sh\n"
                 "env | sed -n '/^_HGS_ZERUS_ARG_/p' >&2\n"
                 "for arg do printf '%s' \"$arg\" | od -An -tx1 | tr -d ' \\n'; printf '\\n'; done >&2\n"
                 "if [ \"$1\" = inspect ]; then printf '{\"events\":[],\"cursor\":0}\\n'; exit 7; fi\n");
    script.close(); QVERIFY(script.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
    HgsClient client(script.fileName()); QSignalSpy writes(&client, &HgsClient::writeDone);
    QSignalSpy failed(&client, &HgsClient::inspectionFailed);
    const QString nfc = QString::fromUtf8("codex/проект/Новый");
    const QString nfd = nfc.normalized(QString::NormalizationForm_D);
    QVERIFY(nfc.toUtf8() != nfd.toUtf8());
    const QString literal = QString::fromUtf8("line\nНовый '$()' \"`exit 93`\" \\ $(exit 94)");
    const auto hexLines = [](const QStringList &arguments) {
        QStringList lines; for (const auto &argument : arguments) lines.append(QString::fromLatin1(argument.toUtf8().toHex()));
        return lines.join('\n');
    };
    client.renameSession({}, nfc, nfd); QTRY_COMPARE(writes.size(), 1);
    QVERIFY(writes[0][1].toBool()); QCOMPARE(writes[0][2].toString(), hexLines({"rename", nfc, nfd}));
    client.renameSession({}, nfd, literal); QTRY_COMPARE(writes.size(), 2);
    QVERIFY(writes[1][1].toBool()); QCOMPARE(writes[1][2].toString(), hexLines({"rename", nfd, literal}));
    client.requestInspection({}, nfc); QTRY_COMPARE(failed.size(), 1);
    QCOMPARE(failed[0][2].toString(), hexLines({"inspect", nfc, "--after", "0"}));
    client.requestInspection({}, nfd); QTRY_COMPARE(failed.size(), 2);
    QCOMPARE(failed[1][2].toString(), hexLines({"inspect", nfd, "--after", "0"}));
}

void TestHgsClient::parsesBox()
{
    const QByteArray json = R"({"host":"arch","ok":true,
      "metrics":{"cpu_percent":27.5,"memory_total_bytes":34359738368.0,"sampled_at":1791018000},
      "projects":{"sample-project":"/workspace/sample-project"},
      "sessions":[{"name":"claude/sample-project","cmd":"claude","project":"sample-project",
                   "tag":null,"attached":1,"clients":["/dev/pts/14"],"created":1756304176}]})";
    QString err;
    const BoxState box = HgsClient::parseBox(json, &err);
    QVERIFY2(err.isEmpty(), qPrintable(err));
    QCOMPARE(box.host, QStringLiteral("arch"));
    QCOMPARE(box.metrics.value("cpu_percent").toDouble(), 27.5);
    QCOMPARE(box.metrics.value("memory_total_bytes").toDouble(), 34359738368.0);
    QVERIFY(box.ok);
    QCOMPARE(box.projects.value(QStringLiteral("sample-project")),
             QStringLiteral("/workspace/sample-project"));
    QCOMPARE(box.sessions.size(), 1);
    QCOMPARE(box.sessions[0].name, QStringLiteral("claude/sample-project"));
    QCOMPARE(box.sessions[0].cmd, QStringLiteral("claude"));
    QCOMPARE(box.sessions[0].attached, 1);
    QCOMPARE(box.sessions[0].clients, QStringList{QStringLiteral("/dev/pts/14")});
}

void TestHgsClient::parsesOfflineBox()
{
    const QByteArray json =
        R"({"host":"mac","ok":false,"error":"offline","projects":{},"sessions":[]})";
    QString err;
    const BoxState box = HgsClient::parseBox(json, &err);
    QVERIFY2(err.isEmpty(), qPrintable(err));
    QCOMPARE(box.host, QStringLiteral("mac"));
    QVERIFY(!box.ok);
    QCOMPARE(box.error, QStringLiteral("offline"));
    QVERIFY(box.sessions.isEmpty());
}

void TestHgsClient::parsesSavedSessions()
{
    QString err;
    const auto box = HgsClient::parseBox(R"({"host":"arch","ok":true,"sessions":[
      {"name":"claude/p","state":"paused","resumable":true},
      {"name":"codex/p","resumable":true,"activity":"idle","tracked":true,"phase":"idle",
       "model":"probe","prompt":"review changes","last_event_at":123.75,"turn_started":100.5,"compaction_started":120.25,"subagent_count":2,
       "process_state":"running","conversation_state":"ended","runtime_state":"live"}]})", &err);
    QVERIFY(err.isEmpty());
    QCOMPARE(box.sessions[0].state, QStringLiteral("paused"));
    QVERIFY(box.sessions[0].resumable);
    QCOMPARE(box.sessions[1].state, QStringLiteral("running"));
    QCOMPARE(box.sessions[1].activity, QStringLiteral("idle"));
    QVERIFY(box.sessions[1].tracked);
    QCOMPARE(box.sessions[1].phase, QStringLiteral("idle"));
    QCOMPARE(box.sessions[1].prompt, QStringLiteral("review changes"));
    QCOMPARE(box.sessions[1].lastEventAt, 123.75);
    QCOMPARE(box.sessions[1].turnStarted, 100.5); QCOMPARE(box.sessions[0].turnStarted, 0.0);
    QCOMPARE(box.sessions[1].compactionStarted, 120.25); QCOMPARE(box.sessions[0].compactionStarted, 0.0);
    QCOMPARE(box.sessions[1].subagentCount, 2);
    QCOMPARE(box.sessions[1].processState, QStringLiteral("running"));
    QCOMPARE(box.sessions[1].conversationState, QStringLiteral("ended"));
    QCOMPARE(box.sessions[1].runtimeState, QStringLiteral("live"));
    QVERIFY(box.sessions[0].processState.isEmpty()); // Older peers remain supported.
}

void TestHgsClient::parsesActivityMetadata()
{
    QString error;
    const auto box = HgsClient::parseBox(R"({"host":"arch","ok":true,"sessions":[
      {"name":"codex/p/ui","cwd":"/work/ui","cwd_source":"tmux","git_root":"/work/ui","launch_id":"unique-launch","account_id":"work","account_home":"/accounts/work",
       "git_branch":"feat/layout","git_worktree":true,"git_worktree_name":"ui","git_detached":false,"git_metadata_state":"ok",
       "activity_summary":"Read","activity_detail":"src/view.cpp","subagent_source":"hooks","subagent_counts_complete":true,
       "subagent_active_count":2,"subagent_completed_count":12,"subagent_total_count":14,
       "subagent_previews":[{"id":"child-1","label":"UI reviewer","state":"working","detail":"Inspect keyboard focus"}]},
      {"name":"kimi/p/legacy","subagent_source":"hook_profiles","subagent_counts_complete":false,
       "subagent_active_count":null,"subagent_total_count":null,"subagent_previews":[{"group":true,"label":"code","total_count":null}]},
      {"name":"sh/p","subagent_source":"unavailable"}]})", &error);
    QVERIFY2(error.isEmpty(), qPrintable(error)); QCOMPARE(box.sessions.size(), 3);
    const auto &s = box.sessions[0]; QCOMPARE(s.cwd, QString("/work/ui")); QCOMPARE(s.cwdSource, QString("tmux"));
    QCOMPARE(s.launchId, QString("unique-launch"));
    QCOMPARE(s.accountId,QString("work"));QCOMPARE(s.accountHome,QString("/accounts/work"));
    QCOMPARE(s.gitRoot, s.cwd); QCOMPARE(s.gitBranch, QString("feat/layout")); QVERIFY(s.gitWorktree);
    QCOMPARE(s.gitWorktreeName, QString("ui")); QVERIFY(!s.gitDetached); QCOMPARE(s.gitMetadataState, QString("ok"));
    QCOMPARE(s.activitySummary, QString("Read")); QCOMPARE(s.activityDetail, QString("src/view.cpp"));
    QCOMPARE(s.subagentActiveCount, 2); QCOMPARE(s.subagentCompletedCount, 12); QCOMPARE(s.subagentTotalCount, 14);
    QVERIFY(s.subagentCountsComplete); QCOMPARE(s.subagentPreviews[0].toObject().value("label").toString(), QString("UI reviewer"));
    QCOMPARE(box.sessions[1].subagentTotalCount, -1); QVERIFY(!box.sessions[1].subagentCountsComplete);
    QVERIFY(box.sessions[1].subagentPreviews[0].toObject().value("group").toBool());
    QCOMPARE(box.sessions[2].subagentSource, QString("unavailable")); QCOMPARE(box.sessions[2].subagentTotalCount, -1);
}

void TestHgsClient::parsesFleetArray()
{
    const QByteArray json = R"([{"host":"arch","ok":true,"projects":{},"sessions":[]},
                                {"host":"mac","ok":false,"error":"bad_response",
                                 "projects":{},"sessions":[]}])";
    QString err;
    const QList<BoxState> boxes = HgsClient::parseFleet(json, &err);
    QVERIFY2(err.isEmpty(), qPrintable(err));
    QCOMPARE(boxes.size(), 2);
    QCOMPARE(boxes[1].error, QStringLiteral("bad_response"));
}

void TestHgsClient::rejectsGarbage()
{
    QString err;
    const BoxState box = HgsClient::parseBox(QByteArray("  (no sessions)\n"), &err);
    QVERIFY(!err.isEmpty());
    QVERIFY(!box.ok);
}

void TestHgsClient::tagAndProjectMayBeNull()
{
    const QByteArray json = R"({"host":"arch","ok":true,"projects":{},
      "sessions":[{"name":"htop","cmd":"htop","project":null,"tag":null,
                   "attached":0,"clients":[],"created":0}]})";
    QString err;
    const BoxState box = HgsClient::parseBox(json, &err);
    QVERIFY2(err.isEmpty(), qPrintable(err));
    QVERIFY(box.sessions[0].project.isNull());
    QVERIFY(box.sessions[0].tag.isNull());
}

// Не из плана дословно, но явно запрошено в разделе "think about": бинарь hgs
// отсутствует вовсе — QProcess не может стартовать (FailedToStart), и это НЕ ok:false
// (та ветка вообще не про JSON), а failed() с путём, который реально пытались запустить,
// чтобы оператор увидел, что чинить.
void TestHgsClient::missingBinaryEmitsFailed()
{
    HgsClient client(QStringLiteral("/no/such/hgs-binary-for-sure"));
    QSignalSpy failedSpy(&client, &HgsClient::failed);
    client.requestLocal();
    QVERIFY(failedSpy.wait(2000));
    QCOMPARE(failedSpy.size(), 1);
    const QString what = failedSpy.at(0).at(0).toString();
    const QString detail = failedSpy.at(0).at(1).toString();
    QVERIFY2(what.contains(QStringLiteral("/no/such/hgs-binary-for-sure")),
              qPrintable(what));
    QVERIFY(!detail.isEmpty());
}

// Task 6: пока запрос к пиру X в полёте, повторный requestPeer(X) должен быть
// проигнорирован (m_peerInFlight в HgsClient.h) -- иначе спящий ноут получает
// по ssh-процессу на каждое открытие меню.
//
// Тест НЕ ждёт настоящих 15с таймаута HgsClient::requestPeer -- это сознательный выбор
// (см. задание Task 6, "добавь тест, если можно выразить поведение гварда без
// 15-секундного ожидания"): вставка в m_peerInFlight в requestPeer() синхронна и
// происходит ДО возврата из вызова, так что второй requestPeer("mac"), выданный из того
// же кадра стека без прохода цикла событий между вызовами, детерминированно видит пира
// уже "в полёте" -- независимо от того, успела ли ОС вообще стартовать процесс первого
// вызова. Поэтому фейковый hgs ниже не спит вовсе: он сразу пишет метку в лог-файл и
// отвечает валидным JSON, и дедупликация проверяется без единой искусственной паузы.
void TestHgsClient::localPollingReportsStartAndRecoversFromFailure()
{
    QTemporaryDir dir; const auto path = dir.filePath("hgs");
    HgsClient client(path); QSignalSpy started(&client, &HgsClient::stateReadStarted);
    QSignalSpy failed(&client, &HgsClient::stateReadFailed), ready(&client, &HgsClient::localReady);
    client.requestLocal(); client.requestLocal(); QCOMPARE(started.size(), 1);
    QTRY_COMPARE(failed.size(), 1); QVERIFY(failed[0][0].toString().isEmpty());
    QFile script(path); QVERIFY(script.open(QIODevice::WriteOnly));
    script.write("#!/bin/sh\necho '{\"host\":\"arch\",\"ok\":true,\"sessions\":[]}'\n"); script.close();
    QVERIFY(script.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
    client.requestLocal(); client.requestLocal(); QCOMPARE(started.size(), 2);
    QTRY_COMPARE(ready.size(), 1);
    QString error;
    const auto box = HgsClient::parseBox(R"({"host":"mac","ok":false,"error":"offline","error_detail":"ssh: Connection refused"})", &error);
    QVERIFY(error.isEmpty());
    QCOMPARE(box.error, QString("ssh: Connection refused"));
}

void TestHgsClient::duplicatePeerRequestIsIgnoredWhileInFlight()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString logPath = dir.filePath(QStringLiteral("calls.log"));
    const QString scriptPath = dir.filePath(QStringLiteral("hgs"));

    QFile script(scriptPath);
    QVERIFY(script.open(QIODevice::WriteOnly | QIODevice::Text));
    QTextStream(&script)
        << "#!/bin/sh\n"
           "echo call >> '" << logPath << "'\n"
           "echo '{\"host\":\"mac\",\"ok\":true,\"projects\":{},\"sessions\":[]}'\n";
    script.close();
    QVERIFY(QFile::setPermissions(scriptPath, QFileDevice::ReadOwner | QFileDevice::WriteOwner
                                                   | QFileDevice::ExeOwner));

    HgsClient client(scriptPath);
    QSignalSpy readySpy(&client, &HgsClient::peerReady);
    QSignalSpy startedSpy(&client, &HgsClient::stateReadStarted);

    client.requestPeer(QStringLiteral("mac"));
    client.requestPeer(QStringLiteral("mac"));  // дубликат -- должен быть проигнорирован
    QCOMPARE(startedSpy.size(), 1); QCOMPARE(startedSpy[0][0].toString(), QString("mac"));

    QVERIFY(readySpy.wait(2000));
    // Даём файловой системе долиться, если ответ уже пришёл, а второй (лишний) процесс
    // ещё дописывает лог -- на практике оба события синхронны с finished(), но лишняя
    // пауза дешева и убирает теоретическую гонку записи в файл.
    QTest::qWait(50);

    QCOMPARE(readySpy.size(), 1);  // не два ответа -- ровно один процесс дошёл до конца
    {
        QFile log(logPath);
        QVERIFY(log.open(QIODevice::ReadOnly | QIODevice::Text));
        const QList<QByteArray> lines = log.readAll().trimmed().split('\n');
        QCOMPARE(lines.size(), 1);  // ровно один запуск, дубликат не породил процесс
    }

    // После завершения метка обязана сняться (см. requestPeer в .cpp) -- иначе пир
    // навсегда застрял бы в m_peerInFlight, и следующий реальный опрос молча терялся бы.
    client.requestPeer(QStringLiteral("mac"));
    QVERIFY(readySpy.wait(2000));
    QTest::qWait(50);
    QCOMPARE(readySpy.size(), 2);
    {
        QFile log(logPath);
        QVERIFY(log.open(QIODevice::ReadOnly | QIODevice::Text));
        QCOMPARE(log.readAll().trimmed().split('\n').size(), 2);  // второй настоящий запуск
    }
}

void TestHgsClient::sessionCommands()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString scriptPath = dir.filePath(QStringLiteral("hgs"));
    QFile script(scriptPath);
    QVERIFY(script.open(QIODevice::WriteOnly));
    script.write("#!/bin/sh\nprintf '%s\\n' \"$@\" >&2\nexit 1\n");
    script.close();
    QVERIFY(script.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner
                                  | QFileDevice::ExeOwner));
    HgsClient client(scriptPath);
    QSignalSpy done(&client, &HgsClient::writeDone);
    QSignalSpy refresh(&client, &HgsClient::sessionWriteDone);
    client.pauseAll(QStringLiteral("mac"), 3);
    QVERIFY(done.wait(2000));
    QCOMPARE(done.last()[1].toBool(), false);
    QCOMPARE(done.last()[2].toString(), QStringLiteral("@mac\npause\n--all"));
    QCOMPARE(refresh.size(), 1);
    QCOMPARE(refresh.last()[0].toString(), QStringLiteral("mac"));
    client.resumeAll(QString(), 2);
    QVERIFY(done.wait(2000));
    QCOMPARE(done.last()[2].toString(), QStringLiteral("resume\n--all\n-d"));
    QCOMPARE(refresh.size(), 2);
    QCOMPARE(refresh.last()[0].toString(), QString());
    client.pauseSession(QString(), QStringLiteral("claude/p/a b"));
    QVERIFY(done.wait(2000));
    QCOMPARE(done.last()[2].toString(), QStringLiteral("pause\nclaude/p/a b"));
    client.resumeSession(QStringLiteral("mac"), QStringLiteral("codex/p/review"));
    QVERIFY(done.wait(2000));
    QCOMPARE(done.last()[2].toString(), QStringLiteral("@mac\nresume\ncodex/p/review\n-d"));
    client.projectRemove(QStringLiteral("scratch"));
    QVERIFY(done.wait(2000));
    QCOMPARE(refresh.size(), 4); // A project edit must not refresh a peer.
}

void TestHgsClient::inspectionCursorAndFailures()
{
    QTemporaryDir dir;
    QFile script(dir.filePath("hgs"));
    QVERIFY(script.open(QIODevice::WriteOnly));
    script.write("#!/bin/sh\n"
                 "[ \"$1\" = '@mac' ] && [ \"$2\" = inspect ] && [ \"$3\" = 'codex/p/a b' ] && "
                 "[ \"$4\" = --after ] && [ \"$5\" = 42 ] || { echo invalid >&2; exit 1; }\n"
                 "echo '{\"tracked\":true,\"cursor\":43,\"events\":[{\"seq\":43,\"type\":\"Stop\"}]}'\n");
    script.close();
    QVERIFY(script.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
    HgsClient client(script.fileName());
    QSignalSpy ready(&client, &HgsClient::inspectionReady);
    QSignalSpy failed(&client, &HgsClient::inspectionFailed);
    QSignalSpy stateFailed(&client, &HgsClient::stateReadFailed);
    client.requestInspection("mac", "codex/p/a b", 42);
    client.requestInspection("mac", "codex/p/a b", 0); // In-flight duplicate must be ignored.
    QTRY_COMPARE(ready.size(), 1);
    QCOMPARE(failed.size(), 0);
    QCOMPARE(ready[0][0].toString(), QStringLiteral("mac"));
    QCOMPARE(ready[0][1].toString(), QStringLiteral("codex/p/a b"));
    QCOMPARE(qvariant_cast<QJsonObject>(ready[0][2]).value("cursor").toInteger(), 43);
    client.requestInspection("mac", "codex/p/a b", 0);
    QTRY_COMPARE(failed.size(), 1);
    QCOMPARE(failed[0][0].toString(), QStringLiteral("mac"));
    QCOMPARE(stateFailed.size(), 0); // Inspection errors don't mark another machine offline.
    client.requestPeer("mac");
    QTRY_COMPARE(stateFailed.size(), 1);
    QCOMPARE(stateFailed[0][0].toString(), QStringLiteral("mac"));
}

void TestHgsClient::archiveIdentityAndCommands()
{
    QString error;
    const auto box = HgsClient::parseBox(R"({"host":"arch","ok":true,"sessions":[
        {"name":"codex/p","state":"running"},
        {"name":"codex/p","state":"archived","archive_id":"old-one","archived_at":1790928000,"resumable":true},
        {"name":"codex/p","state":"archived","archive_id":"old-two","archived_at":1790928005,"resumable":false}]})", &error);
    QVERIFY(error.isEmpty()); QCOMPARE(box.sessions.size(), 3);
    QCOMPARE(box.sessions[1].archiveId, QString("old-one"));
    QCOMPARE(box.sessions[2].archiveId, QString("old-two"));
    QCOMPARE(box.sessions[1].archivedAt, 1790928000.0);
    QVERIFY(box.sessions[0].archiveId.isEmpty());
    QTemporaryDir dir; QFile script(dir.filePath("hgs")); QVERIFY(script.open(QIODevice::WriteOnly));
    script.write("#!/bin/sh\nprintf '%s\\n' \"$@\" >&2\nexit 0\n"); script.close();
    QVERIFY(script.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
    HgsClient client(script.fileName()); QSignalSpy writes(&client, &HgsClient::writeDone);
    QSignalSpy refresh(&client, &HgsClient::sessionWriteDone);
    client.restoreArchive("mac", "codex/p", "old-one"); QTRY_COMPARE(writes.size(), 1);
    QCOMPARE(writes[0][2].toString(), QString("@mac\nresume\ncodex/p\n--archive\nold-one\n-d"));
    client.forgetArchive({}, "codex/p", "old-two"); QTRY_COMPARE(writes.size(), 2);
    QCOMPARE(writes[1][2].toString(), QString("kill\ncodex/p\n--archive\nold-two"));
    client.archiveSession("mac", "codex/p"); QTRY_COMPARE(writes.size(), 3);
    QCOMPARE(writes[2][2].toString(), QString("@mac\narchive\ncodex/p"));
    QCOMPARE(refresh.size(), 3);
    client.restoreArchive({}, "codex/p", {}); QCOMPARE(writes.size(), 4);
    QVERIFY(!writes[3][1].toBool());
    client.forgetArchive({}, "codex/p", {}); QCOMPARE(writes.size(), 5);
    QVERIFY(!writes[4][1].toBool()); QCOMPARE(refresh.size(), 3); // No generic destructive fallback.
}

void TestHgsClient::archiveInspectionKeepsDistinctIdentities()
{
    QTemporaryDir dir; QFile script(dir.filePath("hgs")); QVERIFY(script.open(QIODevice::WriteOnly));
    script.write("#!/bin/sh\n[ \"$1\" = '@mac' ] || exit 1\n[ \"$2\" = inspect ] || exit 1\n"
                 "[ \"$3\" = 'codex/p' ] && [ \"$4\" = --after ] && [ \"$5\" = 0 ] || exit 1\n"
                 "if [ \"$#\" = 7 ]; then [ \"$6\" = --archive ] || exit 1; fi\n"
                 "echo '{\"tracked\":true,\"cursor\":1,\"events\":[]}'\n"); script.close();
    QVERIFY(script.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
    HgsClient client(script.fileName()); QSignalSpy ready(&client, &HgsClient::inspectionReady);
    client.requestInspection("mac", "codex/p");
    client.requestInspection("mac", "codex/p", 0, "old-one");
    client.requestInspection("mac", "codex/p", 0, "old-two");
    client.requestInspection("mac", "codex/p", 0, "old-one"); // Only this identical request is deduplicated.
    QTRY_COMPARE(ready.size(), 3);
    QSet<QString> identities; for (const auto &response : ready) identities.insert(response[3].toString());
    QCOMPARE(identities, QSet<QString>({QString(), "old-one", "old-two"}));
    QSignalSpy failed(&client, &HgsClient::inspectionFailed);
    client.requestInspection("mac", "codex/p", 5, "old-two"); QTRY_COMPARE(failed.size(), 1);
    QCOMPARE(failed[0][3].toString(), QString("old-two"));
}

void TestHgsClient::parsesProjects()
{
    const QByteArray json = R"([
      {"name":"sample-project","dir":"/workspace/sample-project","src":"ansible","exists":true},
      {"name":"scratch","dir":"/tmp/gone","src":"local","exists":false}])";
    QString err;
    const QList<ProjectInfo> ps = HgsClient::parseProjects(json, &err);
    QVERIFY2(err.isEmpty(), qPrintable(err));
    QCOMPARE(ps.size(), 2);
    QCOMPARE(ps[0].name, QStringLiteral("sample-project"));
    QCOMPARE(ps[0].dir, QStringLiteral("/workspace/sample-project"));
    QVERIFY(ps[0].fromAnsible);
    QVERIFY(ps[0].exists);
    QVERIFY(!ps[1].fromAnsible);
    QVERIFY(!ps[1].exists);
}

void TestHgsClient::remoteFoldersAndProjects()
{
    QTemporaryDir dir; QFile script(dir.filePath("hgs")); QVERIFY(script.open(QIODevice::WriteOnly));
    script.write("#!/bin/sh\nprintf '%s\\n' \"$@\" >&2\nexit 1\n"); script.close();
    QVERIFY(script.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
    HgsClient client(script.fileName()); QSignalSpy writes(&client, &HgsClient::writeDone);
    client.projectSet("project", "/remote/path with ' quotes", "mac"); QTRY_COMPARE(writes.size(), 1);
    QCOMPARE(writes[0][2].toString(), QStringLiteral("@mac\nproject\nset\nproject\n/remote/path with ' quotes"));
    client.projectRemove("project", "mac"); QTRY_COMPARE(writes.size(), 2);
    QCOMPARE(writes[1][2].toString(), QStringLiteral("@mac\nproject\nrm\nproject"));
    QSignalSpy projects(&client, &HgsClient::projectsFailed);
    client.requestProjects("mac"); QTRY_COMPARE(projects.size(), 1);
    QCOMPARE(projects[0][0].toString(), QStringLiteral("mac"));
    QCOMPARE(projects[0][1].toString(), QStringLiteral("@mac\nproject\nls\n--json"));
    QSignalSpy folders(&client, &HgsClient::directoriesFailed);
    const auto request = client.requestDirectories("mac", "/remote/path with ' quotes", true);
    QTRY_COMPARE(folders.size(), 1);
    QCOMPARE(folders[0][0].toULongLong(), request);
    QCOMPARE(folders[0][1].toString(), QStringLiteral("mac"));
    QCOMPARE(folders[0][2].toString(), QStringLiteral("@mac\ndirs\n--hidden\n/remote/path with ' quotes"));
}

void TestHgsClient::rejectsGarbageProjects()
{
    QString err;
    const QList<ProjectInfo> ps = HgsClient::parseProjects(QByteArray("не json"), &err);
    QVERIFY(!err.isEmpty());
    QVERIFY(ps.isEmpty());
}

void TestHgsClient::parsesSharedReadState()
{
    const QString signature(64, 'a');
    const QJsonObject current{{"name", "codex/p/a"}, {"conversation_id", "c1"}, {"reply_id", "7:100.5"},
        {"attention_signature", signature}, {"read", QJsonObject{{"conversation_id", "c1"}, {"reply_id", "7:100.5"},
        {"attention_signature", QJsonValue()}, {"at", 1.0}}}};
    const QJsonObject otherConversation{{"name", "codex/p/b"}, {"conversation_id", "c2"}, {"attention_signature", QJsonValue()},
        {"read", QJsonObject{{"conversation_id", "old"}, {"reply_id", "1:1.0"}, {"attention_signature", QJsonValue()}, {"at", 1.0}}}};
    const QJsonObject olderHost{{"name", "codex/p/c"}, {"conversation_id", "c3"}};
    QString error;
    const auto box = HgsClient::parseBox(QJsonDocument(QJsonObject{{"host", "arch"}, {"ok", true},
        {"sessions", QJsonArray{current, otherConversation, olderHost}}}).toJson(), &error);
    QVERIFY(error.isEmpty());
    QVERIFY(box.sessions[0].sharedRead); QCOMPARE(box.sessions[0].attentionSignature, signature);
    QCOMPARE(box.sessions[0].readReplyId, QString("7:100.5")); QVERIFY(box.sessions[0].readAttentionSignature.isEmpty());
    QVERIFY(box.sessions[1].sharedRead); QVERIFY(box.sessions[1].readReplyId.isEmpty());
    QVERIFY(!box.sessions[2].sharedRead);
}

void TestHgsClient::readMarksUseStdinAndAcceptOnlyHostAnswers()
{
    QTemporaryDir directory; QFile script(directory.filePath("hgs")); QVERIFY(script.open(QIODevice::WriteOnly));
    script.write(R"(#!/usr/bin/env python3
import json,pathlib,sys
p=json.load(sys.stdin)
pathlib.Path(__file__).with_name('read.json').write_text(json.dumps({'argv':sys.argv[1:],'payload':p}))
mode=p['reply_id']
if mode=='missing': print(json.dumps({'ok':False,'error':'not_found'}));sys.exit(1)
if mode=='offline': print('ssh: connect to host mac port 22: Connection refused',file=sys.stderr);sys.exit(255)
if mode=='garbage': print('not json');sys.exit(0)
read={'conversation_id':'other' if mode=='wrong' else p['conversation_id'],'reply_id':p['reply_id'],'attention_signature':None,'at':1.0}
print(json.dumps({'ok':True,'applied':{'reply_id':True,'attention_signature':False},'read':read}))
)");
    script.close(); QVERIFY(script.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
    HgsClient client(script.fileName()); QSignalSpy done(&client, &HgsClient::readFinished);
    const auto payload = [](const QString &reply) {
        return QJsonObject{{"run_id", "run-1"}, {"conversation_id", "conversation-1"}, {"reply_id", reply}, {"attention_signature", QJsonValue()}};
    };
    const QString name = "codex/project '$HOME' `literal`";
    const auto id = client.requestRead("mac", name, payload("7:100.5"));
    QTRY_COMPARE(done.size(), 1); QCOMPARE(done[0][0].toULongLong(), id); QVERIFY(done[0][1].toBool());
    QFile log(directory.filePath("read.json")); QVERIFY(log.open(QIODevice::ReadOnly));
    const auto call = QJsonDocument::fromJson(log.readAll()).object();
    QCOMPARE(call.value("argv").toArray(), QJsonArray({"@mac", "read", name, "--json"}));
    QCOMPARE(call.value("payload").toObject(), payload("7:100.5"));
    client.requestRead({}, "codex/project", payload("missing")); QTRY_COMPARE(done.size(), 2);
    QVERIFY(done[1][1].toBool()); QCOMPARE(done[1][2].toJsonObject(), (QJsonObject{{"ok", false}, {"error", "not_found"}}));
    for (const char *mode : {"offline", "garbage", "wrong"}) {
        const int before = done.size(); client.requestRead({}, "codex/project", payload(mode));
        QTRY_COMPARE(done.size(), before + 1); QVERIFY(!done.last()[1].toBool()); QVERIFY(!done.last()[3].toString().isEmpty());
    }
}

// APPLESS хватало на чистые parseBox/parseFleet, но missingBinaryEmitsFailed() гоняет
// настоящий QProcess и ждёт сигнал через QSignalSpy::wait() — для этого нужен реально
// работающий диспетчер событий. GUILESS даёт QCoreApplication без дисплея (тест
// по-прежнему должен работать по ssh без X/Wayland).
void TestHgsClient::presenceWritesAreCoalescedAndReportFailures()
{
    QTemporaryDir dir; QVERIFY(dir.isValid());
    const QString path = dir.filePath("hgs");
    QFile script(path); QVERIFY(script.open(QIODevice::WriteOnly));
    script.write(R"(#!/usr/bin/env python3
import json, pathlib, sys, time
root = pathlib.Path(__file__).parent
payload = json.load(sys.stdin)
with (root / 'calls').open('a') as file: file.write(json.dumps([sys.argv[1:], payload]) + '\n')
time.sleep(0.2)
if (root / 'fail').exists(): print('presence unavailable', file=sys.stderr); sys.exit(1)
print('{"ok":true}')
)");
    script.close(); QVERIFY(script.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
    HgsClient client(path); QSignalSpy finished(&client, &HgsClient::presenceFinished);
    client.setPresence(1, false); client.setPresence(2, false); client.setPresence(-3, true);
    QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 2, 10000);
    QFile calls(dir.filePath("calls")); QVERIFY(calls.open(QIODevice::ReadOnly));
    const auto lines = calls.readAll().trimmed().split('\n'); QCOMPARE(lines.size(), 2);
    const auto first = QJsonDocument::fromJson(lines[0]).array(), last = QJsonDocument::fromJson(lines[1]).array();
    QCOMPARE(first[0].toArray(), (QJsonArray{"presence", "set", "--json"}));
    QCOMPARE(first[1].toObject(), (QJsonObject{{"idle_seconds", 1}, {"locked", false}}));
    QCOMPARE(last[1].toObject(), (QJsonObject{{"idle_seconds", 0}, {"locked", true}})); // newest only, clamped
    QVERIFY(finished[0][0].toString().isEmpty()); QVERIFY(finished[1][0].toString().isEmpty());
    QFile fail(dir.filePath("fail")); QVERIFY(fail.open(QIODevice::WriteOnly)); fail.close();
    client.setPresence(5, false);
    QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 3, 10000);
    QVERIFY(finished[2][0].toString().contains("presence unavailable"));
}

QTEST_GUILESS_MAIN(TestHgsClient)
#include "test_hgsclient.moc"
