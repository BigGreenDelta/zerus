#include <QtTest>
#include <QJsonDocument>
#include "AttentionTracker.h"
#include "FleetState.h"

class TestAttention : public QObject {
    Q_OBJECT
private slots:
    void edgesOfflineRestartAndIdentity();
    void countsOnlyActionableFreshSessions();
    void providerErrorNotifiesOnceAndClearsWhenResumed();
    void recoveryWaitsQuietlyAndNotifiesWhenExhausted();
    void repliesPersistByConversationAndNeverConfuseIdleWithUnread();
    void batchReadIncludesOfflineAndPreservesNewerRepliesAndActions();
    void unreadRepliesNotifyOnceAcrossResumeAndRead();
    void onlyExplicitChildRequestsNotify();
    void failedDeliveryRetriesAfterBackoff();
};

void TestAttention::recoveryWaitsQuietlyAndNotifiesWhenExhausted()
{
    SessionInfo s; s.name="session";s.runId="run";s.phase="error";s.attentionId="failure-1";
    s.recovery={{"state","waiting"},{"id","episode"}};
    BoxState box;box.ok=true;box.sessions={s};AttentionTracker tracker;
    QVERIFY(!s.needsAction());QVERIFY(tracker.observe({},box).raised.isEmpty());
    box.sessions[0].attentionId="failure-2";box.sessions[0].recovery["state"]="retrying";
    QVERIFY(tracker.observe({},box).raised.isEmpty());
    box.sessions[0].reviewLater=true;QVERIFY(box.sessions[0].needsAttention());
    box.sessions[0].phase="approval";QCOMPARE(tracker.observe({},box).raised.size(),1);
    box.sessions[0].phase="error";QCOMPARE(tracker.observe({},box).cleared.size(),1);
    box.sessions[0].recovery["state"]="exhausted";box.sessions[0].recovery["reason"]="Automatic attempts exhausted";
    const auto failed=tracker.observe({},box);QCOMPARE(failed.raised.size(),1);QVERIFY(failed.raised[0].body.contains("Automatic attempts exhausted"));
    QVERIFY(tracker.observe({},box).raised.isEmpty());
}

void TestAttention::unreadRepliesNotifyOnceAcrossResumeAndRead()
{
    SessionInfo s; s.name="codex/project/session"; s.cmd="codex"; s.runId="run"; s.conversationId="conversation";
    s.phase="idle"; s.activity="idle"; s.replyId="reply-1";
    BoxState box; box.ok=true; box.host="arch"; box.sessions={s};
    FleetState fleet; fleet.setLocal(box,0); AttentionTracker tracker;
    auto changes=tracker.observe({},fleet.local()); QCOMPARE(changes.raised.size(),1);
    QCOMPARE(changes.raised[0].title, QString("Zerus — New reply"));
    const auto target=AttentionTracker::target(changes.raised[0].token);
    AttentionTracker restarted(tracker.state()); QVERIFY(restarted.observe({},fleet.local()).raised.isEmpty());
    box.sessions[0].runId="resumed"; box.sessions[0].name="renamed"; fleet.setLocal(box,1);
    QVERIFY(restarted.observe({},fleet.local()).raised.isEmpty()); QVERIFY(AttentionTracker::matches(target,box.sessions[0]));
    box.ok=false; fleet.setLocal(box,2); QVERIFY(restarted.observe({},fleet.local()).cleared.isEmpty());
    box.ok=true; box.sessions[0].replyId="reply-2"; fleet.setLocal(box,3);
    changes=restarted.observe({},fleet.local()); QCOMPARE(changes.raised.size(),1); QCOMPARE(changes.cleared.size(),1);
    QVERIFY(fleet.markReplyRead({},"renamed","conversation","reply-2"));
    QCOMPARE(restarted.observe({},fleet.local()).cleared.size(),1);
    QVERIFY(restarted.observe({},fleet.local()).raised.isEmpty());
    box.sessions[0].conversationId="other"; QVERIFY(!AttentionTracker::matches(target,box.sessions[0]));
}

void TestAttention::onlyExplicitChildRequestsNotify()
{
    SessionInfo s; s.name="codex/project/session"; s.runId="run"; s.phase="working"; s.activity="busy";
    s.subagentSource="native";
    s.subagents={{"worker",QJsonObject{{"name","Reviewer"},{"state","finished"},{"reply_id","result"}}}};
    BoxState box; box.ok=true; box.host="arch"; box.sessions={s}; AttentionTracker tracker;
    QVERIFY(tracker.observe({},box).raised.isEmpty());
    auto child=s.subagents["worker"].toObject(); child["state"]="error"; box.sessions[0].subagents["worker"]=child;
    QVERIFY(tracker.observe({},box).raised.isEmpty());
    child["display_state"]="input"; child["question_id"]="q1"; box.sessions[0].subagents["worker"]=child;
    auto changes=tracker.observe({},box); QCOMPARE(changes.raised.size(),1); QVERIFY(changes.raised[0].body.contains("Reviewer"));
    QVERIFY(tracker.observe({},box).raised.isEmpty());
    child["question_id"]="q2"; box.sessions[0].subagents["worker"]=child;
    changes=tracker.observe({},box); QCOMPARE(changes.raised.size(),1); QCOMPARE(changes.cleared.size(),1);
    child.remove("display_state"); child["state"]="working"; box.sessions[0].subagents["worker"]=child;
    QCOMPARE(tracker.observe({},box).cleared.size(),1);
    child["state"]="approval"; box.sessions[0].subagents["worker"]=child; box.sessions[0].state="paused";
    QVERIFY(tracker.observe({},box).raised.isEmpty());
}

void TestAttention::failedDeliveryRetriesAfterBackoff()
{
    SessionInfo s; s.name="session"; s.runId="run"; s.phase="error";
    BoxState box; box.ok=true; box.sessions={s}; AttentionTracker tracker;
    const auto first=tracker.observe({},box,100); QCOMPARE(first.raised.size(),1);
    tracker.deliveryFailed(first.raised[0].token,100);
    AttentionTracker restarted(tracker.state());
    QVERIFY(restarted.observe({},box,159).raised.isEmpty());
    const auto retried=restarted.observe({},box,160); QCOMPARE(retried.raised.size(),1);
    QCOMPARE(retried.cleared,QStringList{first.raised[0].token});
    QVERIFY(restarted.observe({},box,999).raised.isEmpty());
    restarted.deliveryFailed(retried.raised[0].token,1000);
    box.sessions[0].phase="working"; QCOMPARE(restarted.observe({},box,1001).cleared.size(),1);
    QVERIFY(restarted.observe({},box,1060).raised.isEmpty());
}

void TestAttention::edgesOfflineRestartAndIdentity()
{
    SessionInfo s; s.name = "kimi/project/session"; s.runId = "run-a"; s.created = 17;
    s.phase = "input"; s.activity = "busy";
    BoxState box; box.host = "arch"; box.ok = true; box.sessions = {s};
    AttentionTracker tracker;
    auto changes = tracker.observe({}, box); QCOMPARE(changes.raised.size(), 1);
    const auto first = changes.raised.first().token;
    const auto target = AttentionTracker::target(first);
    QCOMPARE(target.value("host").toString(), QString());
    QVERIFY(AttentionTracker::matches(target, s));
    QVERIFY(tracker.observe({}, box).raised.isEmpty());
    box.sessions[0].lastEventAt += 60; // Heartbeats do not repeat a notification.
    QVERIFY(tracker.observe({}, box).raised.isEmpty());
    AttentionTracker restarted(tracker.state());
    QVERIFY(restarted.observe({}, box).raised.isEmpty());
    box.ok = false; box.sessions.clear();
    QVERIFY(restarted.observe({}, box).cleared.isEmpty());
    box.ok = true; box.sessions = {s};
    QVERIFY(restarted.observe({}, box).raised.isEmpty());
    box.sessions[0].name = "kimi/project/renamed";
    QVERIFY(AttentionTracker::matches(target, box.sessions[0]));
    QVERIFY(restarted.observe({}, box).raised.isEmpty());
    box.sessions[0].phase = "approval";
    changes = restarted.observe({}, box);
    QCOMPARE(changes.raised.size(), 1); QCOMPARE(changes.cleared, QStringList{first});
    box.sessions[0].phase = "idle";
    QCOMPARE(restarted.observe({}, box).cleared.size(), 1);
    box.sessions[0].phase = "input";
    QCOMPARE(restarted.observe({}, box).raised.size(), 1);
    box.sessions[0].runId = "run-b";
    QVERIFY(!AttentionTracker::matches(target, box.sessions[0]));
    changes = restarted.observe({}, box); QCOMPARE(changes.raised.size(), 1); QCOMPARE(changes.cleared.size(), 1);
    box.sessions[0].attentionId = "second-question-without-observed-working-tick";
    changes = restarted.observe({}, box); QCOMPARE(changes.raised.size(), 1); QCOMPARE(changes.cleared.size(), 1);
    QVERIFY(restarted.observe({}, box).raised.isEmpty());
    // Same session name on two machines must carry a different click target.
    const auto remote = restarted.observe("mac", box);
    QCOMPARE(remote.raised.size(), 1);
    QCOMPARE(AttentionTracker::target(remote.raised.first().token).value("host").toString(), QString("mac"));
    box.sessions.clear(); QCOMPARE(restarted.observe("mac", box).cleared.size(), 1);
    QVERIFY(AttentionTracker::target("malformed token").isEmpty());
}

void TestAttention::countsOnlyActionableFreshSessions()
{
    SessionInfo s; s.phase = "input";
    BoxState box; box.host = "arch"; box.ok = true; box.sessions = {s};
    for (const QString state : {"paused", "stopped", "archived"}) {
        auto saved = s; saved.state = state; box.sessions << saved;
    }
    auto exited = s; exited.processState = "exited"; box.sessions << exited;
    auto ready = s; ready.phase = "idle"; box.sessions << ready;
    FleetState fleet; fleet.setLocal(box, 1000);
    box.host = "mac"; s.phase = "approval"; box.sessions = {s}; fleet.setPeer(box, 1000);
    QCOMPARE(fleet.attentionSessions(1000), 2);
    QCOMPARE(fleet.attentionSessions(1000 + FleetState::kPeerStaleMs), 1);
    box.ok = false; fleet.setPeer(box, 1500); QCOMPARE(fleet.attentionSessions(1500), 1);
    QVERIFY(fleet.tooltip(1500).startsWith("1 need attention"));
    box.host = "arch"; fleet.setLocal(box, 1500); QCOMPARE(fleet.attentionSessions(1500), 0);
}

void TestAttention::providerErrorNotifiesOnceAndClearsWhenResumed()
{
    SessionInfo session;session.name="codex/project/session";session.runId="run";session.phase="error";
    session.attentionId="codex_native_log:1";session.activityDetail="Selected model is at capacity. Please try a different model.";
    BoxState box;box.host="arch";box.ok=true;box.sessions={session};AttentionTracker tracker;
    const auto first=tracker.observe({},box);QCOMPARE(first.raised.size(),1);QVERIFY(first.raised.first().body.contains("at capacity"));
    box.sessions[0].lastEventAt+=60;QVERIFY(tracker.observe({},box).raised.isEmpty());
    box.sessions[0].phase="working";QCOMPARE(tracker.observe({},box).cleared.size(),1);
    box.sessions[0].phase="error";box.sessions[0].attentionId="codex_native_log:2";QCOMPARE(tracker.observe({},box).raised.size(),1);
    box.sessions[0].processState="exited";QCOMPARE(tracker.observe({},box).cleared.size(),1);
}

void TestAttention::repliesPersistByConversationAndNeverConfuseIdleWithUnread()
{
    SessionInfo s; s.name = "codex/project/one"; s.cmd = "codex"; s.runId = "run-1";
    s.conversationId = "conversation-1"; s.activity = "idle"; s.phase = "idle";
    BoxState box; box.host = "arch"; box.ok = true; box.sessions = {s};
    FleetState fleet; fleet.setLocal(box, 0); QCOMPARE(fleet.attentionSessions(0), 0);
    box.sessions[0].replyId = "10:100"; fleet.setLocal(box, 1);
    QCOMPARE(fleet.attentionSessions(1), 1); QVERIFY(fleet.local().sessions[0].unreadReply);
    AttentionTracker notifications; QCOMPARE(notifications.observe({}, fleet.local()).raised.size(), 1);
    QVERIFY(!fleet.markReplyRead({}, s.name, s.conversationId, "old-reply"));
    QVERIFY(!fleet.markReplyRead({}, s.name, "other-conversation", "10:100"));
    QVERIFY(fleet.markReplyRead({}, s.name, s.conversationId, "10:100")); QCOMPARE(fleet.attentionSessions(1), 0);
    FleetState restarted; restarted.setReadReplies(fleet.readReplies());
    box.sessions[0].runId = "resumed-run"; box.sessions[0].name = "codex/project/renamed";
    restarted.setLocal(box, 2); QCOMPARE(restarted.attentionSessions(2), 0);
    box.host = "mac"; restarted.setPeer(box, 2); QCOMPARE(restarted.attentionSessions(2), 1);
    BoxState offline; offline.host = "mac"; restarted.setPeer(offline, 3);
    QVERIFY(restarted.peer("mac")->sessions[0].unreadReply); QCOMPARE(restarted.attentionSessions(3), 0);
    restarted.setPeer(box, 4); QVERIFY(restarted.peer("mac")->sessions[0].unreadReply);
    box.host = "arch"; box.sessions[0].replyId = "20:200"; restarted.setLocal(box, 4);
    QVERIFY(restarted.local().sessions[0].unreadReply); // No observed busy poll is required.
    box.sessions[0].phase = "approval"; restarted.setLocal(box, 5);
    QVERIFY(restarted.markReplyRead({}, box.sessions[0].name, s.conversationId, "20:200"));
    QVERIFY(restarted.local().sessions[0].needsAction()); QVERIFY(restarted.local().sessions[0].needsAttention());
    box.sessions[0].state = "archived"; restarted.setLocal(box, 6); QVERIFY(!restarted.local().sessions[0].unreadReply);
}

void TestAttention::batchReadIncludesOfflineAndPreservesNewerRepliesAndActions()
{
    SessionInfo ready; ready.name = "codex/project/ready"; ready.cmd = "codex";
    ready.conversationId = "conversation"; ready.replyId = "10:100"; ready.activity = "idle"; ready.phase = "idle";
    auto approval = ready; approval.name = "codex/project/approval"; approval.conversationId = "approval"; approval.phase = "approval";
    auto input = approval; input.name = "codex/project/question"; input.conversationId = "question"; input.phase = "input";
    auto error = approval; error.name = "codex/project/error"; error.conversationId = "error"; error.phase = "error";
    auto archived = ready; archived.state = "archived"; archived.conversationId = "archived";
    auto unknown = ready; unknown.conversationId.clear(); unknown.replyId.clear();
    BoxState local; local.host = "arch"; local.ok = true; local.sessions = {ready, approval, input, error, archived, unknown};
    BoxState remote; remote.host = "mac"; remote.ok = true; remote.sessions = {ready};
    FleetState fleet; fleet.setLocal(local, 0); fleet.setPeer(remote, 0);
    remote.ok = false; fleet.setPeer(remote, 1); // Include the cached reply on a sleeping computer.
    QCOMPARE(fleet.unreadReplies().size(), 5);
    const auto requested = fleet.unreadReplies();
    local.sessions[0].replyId = "11:101"; fleet.setLocal(local, 2); // An answer arrives after the click's snapshot.
    QCOMPARE(fleet.markRepliesRead(requested), 4);
    QVERIFY(fleet.local().sessions[0].unreadReply); QVERIFY(!fleet.peer("mac")->sessions[0].unreadReply);
    for (int i : {1, 2, 3}) {
        QVERIFY(!fleet.local().sessions[i].unreadReply);
        QVERIFY(fleet.local().sessions[i].needsAttention());
    }
    QCOMPARE(fleet.markRepliesRead(requested), 0);
    QCOMPARE(fleet.markRepliesRead(fleet.unreadReplies()), 1); QVERIFY(fleet.unreadReplies().isEmpty());
    QCOMPARE(fleet.readReplies().size(), 5); // No archive or unknown conversation was acknowledged.
    // Store and reload the same document used by the GUI's preferences.
    const auto saved = QJsonDocument(fleet.readReplies()).toJson(QJsonDocument::Compact);
    FleetState restarted; restarted.setReadReplies(QJsonDocument::fromJson(saved).object());
    restarted.setLocal(local, 3); remote.ok = true; restarted.setPeer(remote, 3);
    QVERIFY(restarted.unreadReplies().isEmpty()); QCOMPARE(restarted.attentionSessions(3), 3);
    // Another device keeps its own read state until its user acknowledges it.
    FleetState secondDevice; secondDevice.setLocal(local, 3); QCOMPARE(secondDevice.unreadReplies().size(), 4);
    remote.sessions[0].replyId = "12:102"; restarted.setPeer(remote, 4);
    QCOMPARE(restarted.unreadReplies().size(), 1); QVERIFY(restarted.peer("mac")->sessions[0].unreadReply);
}

QTEST_APPLESS_MAIN(TestAttention)
#include "test_attention.moc"
