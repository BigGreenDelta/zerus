#include <QtTest>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QWindow>
#include "AttentionTracker.h"
#include "FleetState.h"
#include "OvermindModel.h"

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
    void acknowledgedAttentionWithdrawsItsNotification();
    void lapsedAcknowledgementDoesNotAnnounceTheSameRequestAgain();
    void intermediateRepliesWaitForIdleAndKeepExplicitQuestions();
    void deliveredRepliesDoNotRepeatAfterWorking();
    void foregroundRequestsStayActionableWithoutReplay();
    void foregroundRepliesStayUnreadWithoutReplay();
    void actualApplicationFocusSuppressesBanners();
    void scheduledTicksStayUnreadUntilTheScheduleEnds();
    void decisionsNotifyOncePerSignature();
    void decisionsFollowFocusAndRetryFailedDelivery();
    void decisionsReturnQuietlyAndBurstsAreSummarized();
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

void TestAttention::acknowledgedAttentionWithdrawsItsNotification()
{
    SessionInfo s; s.name="codex/project/session"; s.cmd="codex"; s.runId="run"; s.conversationId="conversation";
    s.phase="input"; s.activity="busy"; s.attentionId="question-1"; s.sharedRead=true; s.attentionSignature=QString(64,'a');
    BoxState box; box.ok=true; box.host="mac"; box.sessions={s};
    FleetState fleet; fleet.setPeer(box,0); AttentionTracker tracker;
    QCOMPARE(tracker.observe("mac",*fleet.peer("mac")).raised.size(),1);
    // Another device acknowledged exactly this request: the banner goes away.
    box.sessions[0].readAttentionSignature=s.attentionSignature; fleet.setPeer(box,1);
    QVERIFY(fleet.peer("mac")->sessions[0].attentionAcknowledged);
    QCOMPARE(tracker.observe("mac",*fleet.peer("mac")).cleared.size(),1);
    // A new request has a new signature and notifies again.
    box.sessions[0].attentionId="question-2"; box.sessions[0].attentionSignature=QString(64,'b'); fleet.setPeer(box,2);
    QCOMPARE(tracker.observe("mac",*fleet.peer("mac")).raised.size(),1);
}

void TestAttention::lapsedAcknowledgementDoesNotAnnounceTheSameRequestAgain()
{
    SessionInfo s; s.name="codex/project/session"; s.runId="run"; s.phase="approval"; s.activity="busy";
    s.attentionId="call-1"; s.subagentSource="hooks";
    s.subagents={{"worker",QJsonObject{{"name","Reviewer"},{"state","working"},{"display_state","approval"},{"attention_id","child-1"}}}};
    BoxState box; box.ok=true; box.host="mac"; box.sessions={s}; AttentionTracker tracker;
    QCOMPARE(tracker.observe("mac",box).raised.size(),2);
    box.sessions[0].attentionAcknowledged=true;
    auto changes=tracker.observe("mac",box);
    QVERIFY(changes.raised.isEmpty()); QCOMPARE(changes.cleared.size(),2);
    // The acknowledgement lapses (an older host, a recovery-only identity) for
    // the very same requests: nothing is announced again, also after a restart.
    box.sessions[0].attentionAcknowledged=false;
    changes=tracker.observe("mac",box); QVERIFY(changes.raised.isEmpty()); QVERIFY(changes.cleared.isEmpty());
    AttentionTracker restarted(tracker.state());
    QVERIFY(restarted.observe("mac",box).raised.isEmpty());
    // New requests still notify.
    box.sessions[0].attentionId="call-2";
    auto child=box.sessions[0].subagents["worker"].toObject(); child["attention_id"]="child-2"; box.sessions[0].subagents["worker"]=child;
    QCOMPARE(restarted.observe("mac",box).raised.size(),2);
}

void TestAttention::intermediateRepliesWaitForIdleAndKeepExplicitQuestions()
{
    SessionInfo s; s.name="claude/project/review"; s.cmd="claude"; s.runId="run";
    s.conversationId="conversation"; s.replyId="progress-1";
    s.activity="busy"; s.phase="working"; s.subagentActiveCount=1; s.subagentSource="native";
    BoxState box; box.ok=true; box.host="arch"; box.sessions={s};
    FleetState fleet; fleet.setLocal(box,0); AttentionTracker tracker;
    QVERIFY(fleet.local().sessions[0].unreadReply);
    QVERIFY(tracker.observe({},fleet.local()).raised.isEmpty());
    AttentionTracker restarted(tracker.state());
    box.sessions[0].replyId="progress-2"; fleet.setLocal(box,1);
    QVERIFY(restarted.observe({},fleet.local()).raised.isEmpty());

    // Unknown state and inconsistent idle/child snapshots cannot establish
    // that the workflow has finished.
    box.sessions[0].subagentActiveCount=0; box.sessions[0].activity="unknown";
    box.sessions[0].phase="unknown"; fleet.setLocal(box,2);
    QVERIFY(restarted.observe({},fleet.local()).raised.isEmpty());
    box.sessions[0].activity="idle"; box.sessions[0].phase="working"; fleet.setLocal(box,3);
    QVERIFY(restarted.observe({},fleet.local()).raised.isEmpty());
    box.sessions[0].phase="idle"; box.sessions[0].subagentActiveCount=1; fleet.setLocal(box,4);
    QVERIFY(restarted.observe({},fleet.local()).raised.isEmpty());

    box.sessions[0].activity="busy"; box.sessions[0].phase="working";
    box.sessions[0].subagents={{"reviewer",QJsonObject{{"label","Reviewer"},{"display_state","input"},{"question_id","q1"}}}};
    fleet.setLocal(box,5);
    auto changes=restarted.observe({},fleet.local()); QCOMPARE(changes.raised.size(),1);
    QVERIFY(changes.raised[0].title.contains("Needs attention"));
    QVERIFY(changes.raised[0].body.contains("Reviewer"));
    QVERIFY(AttentionTracker::target(changes.raised[0].token).value("reply").toString().isEmpty());
    QVERIFY(restarted.observe({},fleet.local()).raised.isEmpty());

    // Main-agent approvals also stay actionable while replies wait.
    box.sessions[0].subagents={}; box.sessions[0].phase="approval"; box.sessions[0].attentionId="approval-1";
    fleet.setLocal(box,6);
    changes=restarted.observe({},fleet.local()); QCOMPARE(changes.raised.size(),1);
    QVERIFY(changes.raised[0].body.contains("Approval needed"));
    QCOMPARE(changes.cleared.size(),1);

    box.sessions[0].activity="idle"; box.sessions[0].phase="idle";
    box.sessions[0].subagentActiveCount=0; box.sessions[0].replyId="final"; fleet.setLocal(box,7);
    changes=restarted.observe({},fleet.local()); QCOMPARE(changes.raised.size(),1);
    QCOMPARE(AttentionTracker::target(changes.raised[0].token).value("reply").toString(),QString("final"));
    QCOMPARE(changes.cleared.size(),1);
    QVERIFY(restarted.observe({},fleet.local()).raised.isEmpty());
    QVERIFY(fleet.markReplyRead({},s.name,s.conversationId,"final"));
    QCOMPARE(restarted.observe({},fleet.local()).cleared.size(),1);
}

void TestAttention::deliveredRepliesDoNotRepeatAfterWorking()
{
    SessionInfo s; s.name="claude/project/review"; s.cmd="claude"; s.runId="run";
    s.conversationId="conversation"; s.replyId="reply-1"; s.activity="idle"; s.phase="idle";
    BoxState box; box.ok=true; box.sessions={s}; FleetState fleet; fleet.setLocal(box,0);
    AttentionTracker tracker; QCOMPARE(tracker.observe({},fleet.local()).raised.size(),1);
    box.sessions[0].activity="busy"; box.sessions[0].phase="working"; fleet.setLocal(box,1);
    auto changes=tracker.observe({},fleet.local()); QVERIFY(changes.raised.isEmpty()); QVERIFY(changes.cleared.isEmpty());
    AttentionTracker restarted(tracker.state());
    QVERIFY(restarted.observe({},fleet.local()).raised.isEmpty());
    box.sessions[0].activity="idle"; box.sessions[0].phase="idle"; fleet.setLocal(box,2);
    QVERIFY(restarted.observe({},fleet.local()).raised.isEmpty());

    // A newer reply received while working stays pending and clears the old
    // card. Finishing the children alone may establish idle without a new Stop.
    box.sessions[0].activity="busy"; box.sessions[0].phase="working";
    box.sessions[0].replyId="reply-2"; fleet.setLocal(box,3);
    changes=restarted.observe({},fleet.local()); QVERIFY(changes.raised.isEmpty()); QCOMPARE(changes.cleared.size(),1);
    AttentionTracker pending(restarted.state());
    box.sessions[0].activity="idle"; box.sessions[0].phase="idle"; fleet.setLocal(box,4);
    changes=pending.observe({},fleet.local()); QCOMPARE(changes.raised.size(),1);
    QCOMPARE(AttentionTracker::target(changes.raised[0].token).value("reply").toString(),QString("reply-2"));
    QVERIFY(pending.observe({},fleet.local()).raised.isEmpty());
}

void TestAttention::foregroundRequestsStayActionableWithoutReplay()
{
    SessionInfo s; s.name="claude/project/review"; s.cmd="claude"; s.runId="run";
    s.conversationId="conversation"; s.activity="busy"; s.phase="input"; s.attentionId="q1";
    s.subagentSource="native";
    BoxState box; box.ok=true; box.sessions={s}; AttentionTracker tracker;
    QVERIFY(tracker.observe({},box,0,true).raised.isEmpty());
    QVERIFY(box.sessions[0].needsAction()); QVERIFY(box.sessions[0].needsAttention());
    AttentionTracker restarted(tracker.state());
    QVERIFY(restarted.observe({},box,1,false).raised.isEmpty());

    box.sessions[0].attentionId="q2";
    auto changes=restarted.observe({},box,2,false); QCOMPARE(changes.raised.size(),1);
    const auto delivered=changes.raised[0].token;
    box.sessions[0].phase="approval"; box.sessions[0].attentionId="approval";
    changes=restarted.observe({},box,3,true); QVERIFY(changes.raised.isEmpty());
    QCOMPARE(changes.cleared,QStringList{delivered});
    QVERIFY(restarted.observe({},box,4,false).raised.isEmpty());
    box.sessions[0].phase="error"; box.sessions[0].attentionId="error";
    QVERIFY(restarted.observe({},box,5,true).raised.isEmpty());
    QVERIFY(box.sessions[0].needsAction());
    QVERIFY(restarted.observe({},box,6,false).raised.isEmpty());

    box.sessions[0].phase="working";
    box.sessions[0].subagents={{"reviewer",QJsonObject{{"label","Reviewer"},{"display_state","input"},{"question_id","child-q1"}}}};
    QVERIFY(restarted.observe("mac",box,7,true).raised.isEmpty());
    AttentionTracker childRestart(restarted.state());
    QVERIFY(childRestart.observe("mac",box,8,false).raised.isEmpty());
    auto child=box.sessions[0].subagents["reviewer"].toObject(); child["question_id"]="child-q2";
    box.sessions[0].subagents["reviewer"]=child;
    changes=childRestart.observe("mac",box,9,false); QCOMPARE(changes.raised.size(),1);
    QVERIFY(changes.raised[0].body.contains("Reviewer"));
}

void TestAttention::foregroundRepliesStayUnreadWithoutReplay()
{
    SessionInfo s; s.name="claude/project/review"; s.cmd="claude"; s.runId="run";
    s.conversationId="conversation"; s.replyId="reply-1"; s.activity="idle"; s.phase="idle";
    BoxState box; box.ok=true; box.sessions={s}; FleetState fleet; fleet.setLocal(box,0);
    AttentionTracker tracker;
    QVERIFY(tracker.observe({},fleet.local(),0,true).raised.isEmpty());
    QVERIFY(fleet.local().sessions[0].unreadReply); QVERIFY(fleet.local().sessions[0].needsAttention());
    AttentionTracker restarted(tracker.state());
    QVERIFY(restarted.observe({},fleet.local(),1,false).raised.isEmpty());
    QVERIFY(restarted.observe({},fleet.local(),2,true).raised.isEmpty());
    box.sessions[0].replyId="reply-2"; fleet.setLocal(box,3);
    auto changes=restarted.observe({},fleet.local(),3,false); QCOMPARE(changes.raised.size(),1);
    const auto delivered=changes.raised[0].token;
    restarted.deliveryFailed(delivered,4);
    QVERIFY(restarted.observe({},fleet.local(),64,true).raised.isEmpty());
    QVERIFY(restarted.observe({},fleet.local(),65,false).raised.isEmpty());
}

void TestAttention::actualApplicationFocusSuppressesBanners()
{
    int argc=1; char executable[]="test_attention"; char *argv[]={executable,nullptr};
    QGuiApplication application(argc,argv);
    QWindow window; window.resize(100,100); window.show(); window.requestActivate();
    QVERIFY(QTest::qWaitForWindowActive(&window));
    QCOMPARE(QGuiApplication::applicationState(),Qt::ApplicationActive);
    SessionInfo s; s.name="session"; s.runId="run"; s.phase="input"; s.attentionId="q1";
    BoxState box; box.ok=true; box.sessions={s}; AttentionTracker tracker;
    QVERIFY(tracker.observe({},box,0).raised.isEmpty());
    QVERIFY(s.needsAction());
    QVERIFY(!tracker.state().isEmpty());
}

void TestAttention::scheduledTicksStayUnreadUntilTheScheduleEnds()
{
    SessionInfo s; s.name="kimi/project/watch"; s.cmd="kimi"; s.runId="run"; s.conversationId="conversation";
    s.activity="idle"; s.phase="idle"; s.replyId="prompted"; s.recurringSchedules=1;
    BoxState box; box.ok=true; box.sessions={s}; FleetState fleet; fleet.setLocal(box,0); AttentionTracker tracker;
    // The person's own reply notifies even while a schedule is pending.
    auto changes=tracker.observe({},fleet.local()); QCOMPARE(changes.raised.size(),1);
    box.sessions[0].replyId="tick-1"; box.sessions[0].replyScheduled=true; fleet.setLocal(box,1);
    changes=tracker.observe({},fleet.local()); QVERIFY(changes.raised.isEmpty()); QCOMPARE(changes.cleared.size(),1);
    QVERIFY(fleet.local().sessions[0].unreadReply); QVERIFY(fleet.local().sessions[0].needsAttention());
    // Ending the schedule later does not announce the routine tick.
    AttentionTracker restarted(tracker.state());
    box.sessions[0].recurringSchedules=0; fleet.setLocal(box,2);
    QVERIFY(restarted.observe({},fleet.local()).raised.isEmpty());
    // Unknown schedules hold a scheduled reply until they are known; the reply
    // after the schedule ends is the job's result and notifies.
    box.sessions[0].replyId="tick-2"; box.sessions[0].recurringSchedules=-1; fleet.setLocal(box,3);
    QVERIFY(restarted.observe({},fleet.local()).raised.isEmpty());
    box.sessions[0].recurringSchedules=0; fleet.setLocal(box,4);
    changes=restarted.observe({},fleet.local()); QCOMPARE(changes.raised.size(),1);
    QCOMPARE(AttentionTracker::target(changes.raised[0].token).value("reply").toString(),QString("tick-2"));
    QVERIFY(restarted.observe({},fleet.local()).raised.isEmpty());
    // Scheduled replies still wait for idleness.
    box.sessions[0].replyId="final"; box.sessions[0].activity="busy"; box.sessions[0].phase="working"; fleet.setLocal(box,5);
    QVERIFY(restarted.observe({},fleet.local()).raised.isEmpty());
    box.sessions[0].activity="idle"; box.sessions[0].phase="idle"; fleet.setLocal(box,6);
    QCOMPARE(restarted.observe({},fleet.local()).raised.size(),1);
}

namespace {
// Overmind names projects by their group ID, a UUID for a project people made.
const QString kProject = "6f1c2d3e-5b4a-4c3d-8e2f-0a1b2c3d4e5f";
const AttentionTracker::ProjectNames kNames{{kProject, "Alpha"}};

OvermindModel::Decision decision(const QString &task, const QString &ref, const QString &title, const QString &signature)
{
    OvermindModel::Decision d; d.task = task; d.ref = ref; d.project = kProject; d.title = title; d.stage = "Your Call";
    d.question = "SQLite or plain files?"; d.signature = signature; d.assignee = {"node-arch", "codex/hgs/dashboard"};
    return d;
}
}

// Your Call: one notice per task and signature, withdrawn when the task leaves
// the list; a list that was not read resolves nothing.
void TestAttention::decisionsNotifyOncePerSignature()
{
    auto first = decision("uuid-1", "om-k3f9x", "Pick a store for drafts", "s1");
    AttentionTracker tracker;
    QVERIFY(tracker.observeDecisions(false, {first}, 100, false, kNames).raised.isEmpty());
    auto changes = tracker.observeDecisions(true, {first}, 100, false, kNames);
    QCOMPARE(changes.raised.size(), 1); QVERIFY(changes.cleared.isEmpty());
    QCOMPARE(changes.raised[0].title, QString("Zerus — Your Call"));
    // The project's name, never its ID.
    QCOMPARE(changes.raised[0].body, QString("om-k3f9x: Pick a store for drafts\nSQLite or plain files?\nAlpha / codex/hgs/dashboard"));
    QCOMPARE(changes.raised[0].action, QString("Open task"));
    const auto token = changes.raised[0].token;
    const auto target = AttentionTracker::decision(token);
    QCOMPARE(target.value("task").toString(), QString("uuid-1"));
    QCOMPARE(target.value("project").toString(), kProject);
    QCOMPARE(target.value("machine").toString(), QString("node-arch"));
    QCOMPARE(target.value("name").toString(), QString("codex/hgs/dashboard"));
    // A decision notice is not a session notice, and the reverse.
    QVERIFY(AttentionTracker::target(token).isEmpty());
    SessionInfo s; s.name = "session"; s.runId = "run"; s.phase = "input";
    BoxState box; box.ok = true; box.sessions = {s};
    const auto session = tracker.observe({}, box, 100, false);
    QCOMPARE(session.raised.size(), 1); QVERIFY(session.cleared.isEmpty()); QVERIFY(session.raised[0].action.isEmpty());
    QVERIFY(AttentionTracker::decision(session.raised[0].token).isEmpty());
    QVERIFY(AttentionTracker::decision("malformed token").isEmpty());
    // Polls and restarts do not repeat it, and a failed read resolves nothing.
    QVERIFY(tracker.observeDecisions(true, {first}, 101, false).raised.isEmpty());
    AttentionTracker restarted(tracker.state(), tracker.decisionState());
    QVERIFY(restarted.observeDecisions(true, {first}, 102, false).raised.isEmpty());
    QVERIFY(restarted.observeDecisions(false, {}, 103, false).cleared.isEmpty());
    // Session snapshots leave decisions alone, and the reverse.
    box.sessions.clear(); QCOMPARE(restarted.observe({}, box, 104, false).cleared.size(), 1);
    QVERIFY(restarted.observeDecisions(true, {first}, 104, false).cleared.isEmpty());
    // Parked again (a new signature): the old notice goes and a new one comes.
    first.signature = "s2";
    changes = restarted.observeDecisions(true, {first}, 105, false);
    QCOMPARE(changes.cleared, QStringList{token}); QCOMPARE(changes.raised.size(), 1);
    QVERIFY(changes.raised[0].token != token);
    const auto again = changes.raised[0].token;
    // Without a question, an assignee or a known project name the notice still names the task.
    auto bare = decision("uuid-2", "om-b8n7m", "Name the menus", "t1"); bare.question.clear(); bare.assignee = {};
    changes = restarted.observeDecisions(true, {first, bare}, 106, false);
    QCOMPARE(changes.raised.size(), 1); QVERIFY(changes.cleared.isEmpty());
    QCOMPARE(changes.raised[0].body, QString("om-b8n7m: Name the menus\nA task waits for your call."));
    QVERIFY(!changes.raised[0].body.contains(kProject));
    QCOMPARE(AttentionTracker::decision(changes.raised[0].token).value("name").toString(), QString());
    const auto second = changes.raised[0].token;
    // Answered or moved on: the task leaves the list and its notice is withdrawn.
    changes = restarted.observeDecisions(true, {bare}, 107, false);
    QCOMPARE(changes.cleared, QStringList{again}); QVERIFY(changes.raised.isEmpty());
    changes = restarted.observeDecisions(true, {}, 108, false);
    QCOMPARE(changes.cleared, QStringList{second});
    QVERIFY(restarted.decisionState().value("tasks").toObject().isEmpty());
    // Parked once more later (a new signature): a new notice, with the project's name alone.
    first.signature = "s3"; first.assignee = {};
    changes = restarted.observeDecisions(true, {first}, 109, false, kNames);
    QCOMPARE(changes.raised.size(), 1);
    QCOMPARE(changes.raised[0].body, QString("om-k3f9x: Pick a store for drafts\nSQLite or plain files?\nAlpha"));
}

void TestAttention::decisionsFollowFocusAndRetryFailedDelivery()
{
    const auto first = decision("uuid-1", "om-k3f9x", "Pick a store for drafts", "s1");
    const auto second = decision("uuid-2", "om-b8n7m", "Name the menus", "t1");
    AttentionTracker tracker;
    // While Zerus has focus the decision is consumed and not replayed later.
    QVERIFY(tracker.observeDecisions(true, {first}, 100, true).raised.isEmpty());
    QVERIFY(tracker.observeDecisions(true, {first}, 101, false).raised.isEmpty());
    // Leaving the list still withdraws whatever was recorded for it.
    QCOMPARE(tracker.observeDecisions(true, {}, 102, false).cleared.size(), 1);
    // A failed delivery is retried after a minute while the task still waits.
    auto changes = tracker.observeDecisions(true, {second}, 200, false);
    QCOMPARE(changes.raised.size(), 1);
    tracker.deliveryFailed(changes.raised[0].token, 200);
    AttentionTracker restarted(tracker.state(), tracker.decisionState());
    QVERIFY(restarted.observeDecisions(true, {second}, 259, false).raised.isEmpty());
    const auto retried = restarted.observeDecisions(true, {second}, 260, false);
    QCOMPARE(retried.raised.size(), 1); QCOMPARE(retried.cleared, QStringList{changes.raised[0].token});
    QVERIFY(restarted.observeDecisions(true, {second}, 999, false).raised.isEmpty());
}

// A decision that drops out of the list and returns unchanged (a stale or
// capped read) is not announced again for a day; more than three new ones in
// one pass are one summary notice.
void TestAttention::decisionsReturnQuietlyAndBurstsAreSummarized()
{
    const auto first = decision("uuid-1", "om-k3f9x", "Pick a store for drafts", "s1");
    AttentionTracker tracker;
    QCOMPARE(tracker.observeDecisions(true, {first}, 100, false).raised.size(), 1);
    QCOMPARE(tracker.observeDecisions(true, {}, 101, false).cleared.size(), 1);
    AttentionTracker restarted(tracker.state(), tracker.decisionState());
    auto changes = restarted.observeDecisions(true, {first}, 102, false);
    QVERIFY(changes.raised.isEmpty()); QVERIFY(changes.cleared.isEmpty());
    // Leaving again and returning after a day: the signature is forgotten.
    QVERIFY(restarted.observeDecisions(true, {}, 103, false).cleared.isEmpty());
    QCOMPARE(restarted.observeDecisions(true, {first}, 103 + 24 * 60 * 60 + 1, false).raised.size(), 1);

    // A burst: one summary that opens the oldest decision's project.
    QList<OvermindModel::Decision> burst;
    for (int i = 0; i < 5; ++i) burst << decision(QString("b%1").arg(i), QString("om-b%1").arg(i), QString("Task %1").arg(i), QString("x%1").arg(i));
    burst[0].project = "ungrouped";
    AttentionTracker fresh;
    changes = fresh.observeDecisions(true, burst, 500, false, kNames);
    QCOMPARE(changes.raised.size(), 1);
    QCOMPARE(changes.raised[0].title, QString("Zerus — Your Call"));
    QVERIFY2(changes.raised[0].body.startsWith("5 tasks wait for your call.\nom-b0: Task 0\nom-b1: Task 1\nom-b2: Task 2"),
             qPrintable(changes.raised[0].body));
    QCOMPARE(changes.raised[0].action, QString("Open tasks"));
    const auto summary = changes.raised[0].token;
    QCOMPARE(AttentionTracker::decision(summary).value("task").toString(), QString());
    QCOMPARE(AttentionTracker::decision(summary).value("project").toString(), QString("ungrouped"));
    QVERIFY(AttentionTracker::target(summary).isEmpty());
    // Its decisions are recorded: no repeat, also after a restart.
    QVERIFY(fresh.observeDecisions(true, burst, 501, false).raised.isEmpty());
    AttentionTracker again(fresh.state(), fresh.decisionState());
    QVERIFY(again.observeDecisions(true, burst, 502, false).raised.isEmpty());
    // A failed summary is retried for the decisions that still wait.
    again.deliveryFailed(summary, 502);
    const auto rest = burst.mid(1);
    changes = again.observeDecisions(true, rest, 503, false);
    QVERIFY(changes.raised.isEmpty()); QVERIFY(!changes.cleared.contains(summary));
    changes = again.observeDecisions(true, rest, 562, false);
    QCOMPARE(changes.raised.size(), 1); QCOMPARE(changes.cleared, QStringList{summary});
    QCOMPARE(changes.raised[0].body, QString("4 tasks wait for your call."));
    const auto retried = changes.raised[0].token;
    // The summary goes when none of its decisions waits any more.
    QVERIFY(!again.observeDecisions(true, burst.mid(4), 563, false).cleared.contains(retried));
    QVERIFY(again.observeDecisions(true, {}, 564, false).cleared.contains(retried));
    // Three at once are three notices.
    AttentionTracker three;
    changes = three.observeDecisions(true, burst.mid(0, 3), 600, false);
    QCOMPARE(changes.raised.size(), 3);
    for (const auto &notice : changes.raised) QCOMPARE(notice.action, QString("Open task"));
}

QTEST_APPLESS_MAIN(TestAttention)
#include "test_attention.moc"
