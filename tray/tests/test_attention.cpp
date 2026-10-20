#include <QtTest>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QJsonArray>
#include <QWindow>

namespace {
QJsonObject sharedPreferences(const QString &conversation, bool muted, qint64 revision, QJsonObject baseline = {}) {
    if (baseline.isEmpty()) baseline = {{"reply_id", QJsonValue()}, {"attention_signature", QJsonValue()},
        {"optional_question_ids", QJsonArray()}, {"main_attention", QJsonValue()}, {"child_attention", QJsonObject()}};
    return {{"conversation_id", conversation}, {"muted", muted}, {"revision", revision}, {"suppressed", baseline}};
}
}
#include "AttentionTracker.h"
#include "FleetState.h"
#include "SessionPresentation.h"
#include "SessionNotifications.h"

class TestAttention : public QObject {
    Q_OBJECT
private slots:
    void edgesOfflineRestartAndIdentity();
    void countsOnlyActionableFreshSessions();
    void providerErrorNotifiesOnceAndClearsWhenResumed();
    void recoveryWaitsQuietlyAndNotifiesWhenExhausted();
    void repliesPersistByConversationAndNeverConfuseIdleWithUnread();
    void batchReadIncludesOfflineAndPreservesNewerRepliesAndActions();
    void batchReadSkipsSessionsMarkedForLaterEvenAfterTheSnapshot();
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
    void replyWaitsForLiveBackgroundShellToFinish();
    void replyWaitsForExplicitIndependentResult();
    void nativeChildCountersPreserveUnknownEvidence();
    void optionalQuestionsNotifyOncePerQuestionWhenEnabled();
    void optionalQuestionsSurviveUnknownCountsAndResume();
    void optionalQuestionsDeferToRequestsAndRetry();
    void muteConsumesAllNotificationEdgesWithoutAcknowledging();
    void muteWithdrawsBannersAndConsumesOptionalQuestions();
    void mutePersistsByConversationAndKeepsOtherSessionsAudible();
    void muteWithdrawsOnlyMatchingOfflineBannersWithoutReplay();
    void sharedUnmuteSuppressesEventsMissedWhileOffline();
    void sharedSuppressionKeepsTimestampAndRunBoundaries();
    void sharedSuppressionRetainsRawDshMultiRequestIdentity();
    void sharedSuppressionSurvivesCapabilityDowngrade();
    void sharedOptionalBaselineDoesNotEvictNewObservedQuestions();
    void sharedPreferencesDoNotCrossProviders();
    void sharedTimestampSuppressionRequiresKnownCurrentTime();
};

void TestAttention::muteWithdrawsOnlyMatchingOfflineBannersWithoutReplay()
{
    SessionInfo action; action.name="codex/project/action"; action.cmd="codex"; action.runId="action-run";
    action.conversationId="action-conversation"; action.phase="approval"; action.attentionId="approval-1";
    auto question=action; question.name="codex/project/question"; question.runId="question-run"; question.conversationId="question-conversation";
    question.phase="working"; question.activity="busy"; question.optionalQuestionsReported=true; question.optionalQuestions=1; question.optionalQuestionIds={"q1"};
    auto reply=action; reply.name="codex/project/reply"; reply.runId="reply-run"; reply.conversationId="reply-conversation";
    reply.phase="idle"; reply.activity="idle"; reply.unreadReply=true; reply.replyId="reply-1";
    auto audible=action; audible.name="codex/project/audible"; audible.runId="audible-run"; audible.conversationId="audible-conversation";
    auto unknown=action; unknown.name="codex/project/unknown"; unknown.runId="unknown-run"; unknown.conversationId="unknown-conversation";
    BoxState box; box.host="peer"; box.ok=true; box.sessions={action,question,reply,audible,unknown};
    AttentionTracker tracker; tracker.setOptionalQuestions(true);
    const auto initial=tracker.observe("peer",box,100,false); QCOMPARE(initial.raised.size(),5);
    const auto actionToken=tracker.state().value("peer").toObject().value("run:action-run").toObject().value("token").toString();
    tracker.deliveryFailed(actionToken,100);
    const auto before=tracker.state().value("peer").toObject();
    box.ok=false; box.sessions.removeLast(); // An offline snapshot's absence never resolves the unknown request.
    for(int i=0;i<3;++i)box.sessions[i].muted=true;
    box.sessions[0].runId="replacement"; // A muted replacement must not withdraw another run's banner.
    auto changes=tracker.observe("peer",box,200,false); QVERIFY(changes.raised.isEmpty()); QCOMPARE(changes.cleared.size(),2);
    QVERIFY(!changes.cleared.contains(actionToken));
    box.sessions[0].runId=action.runId;
    changes=tracker.observe("peer",box,201,false); QVERIFY(changes.raised.isEmpty()); QCOMPARE(changes.cleared,QStringList{actionToken});
    const auto after=tracker.state().value("peer").toObject();
    QCOMPARE(after.value("run:audible-run"),before.value("run:audible-run"));
    QCOMPARE(after.value("run:unknown-run"),before.value("run:unknown-run"));
    auto consumed=before.value("run:action-run").toObject(); consumed.remove("token"); consumed.remove("retry_at");
    QCOMPARE(after.value("run:action-run").toObject(),consumed);
    QCOMPARE(after.value("question:codex:question-conversation").toObject().value("seen"),before.value("question:codex:question-conversation").toObject().value("seen"));
    for(int i=0;i<3;++i)box.sessions[i].muted=false;
    QVERIFY(tracker.observe("peer",box,202,false).raised.isEmpty()); QCOMPARE(tracker.state().value("peer").toObject(),after);
    box.ok=true; box.sessions.append(unknown);
    AttentionTracker restarted(tracker.state()); restarted.setOptionalQuestions(true);
    QVERIFY(restarted.observe("peer",box,203,false).raised.isEmpty());
    box.sessions[0].attentionId="approval-2"; box.sessions[1].optionalQuestionIds.append("q2"); box.sessions[1].optionalQuestions=2;
    box.sessions[2].replyId="reply-2";
    QCOMPARE(restarted.observe("peer",box,204,false).raised.size(),3);
}

void TestAttention::muteConsumesAllNotificationEdgesWithoutAcknowledging()
{
    SessionInfo s; s.name="codex/project/muted"; s.cmd="codex"; s.runId="run"; s.conversationId="conversation";
    s.phase="approval"; s.activity="busy"; s.attentionId="approval-1"; s.unreadReply=true; s.replyId="reply-1";
    s.subagentSource="native"; s.subagents={{"child",QJsonObject{{"state","input"},{"question_id","child-1"}}}};
    s.muted=true;
    auto other=s; other.name="codex/project/audible"; other.runId="other-run"; other.conversationId="other-conversation";
    other.muted=false; other.unreadReply=false; other.subagents={};
    BoxState box; box.ok=true; box.sessions={s,other}; AttentionTracker tracker;
    auto changes=tracker.observe({},box,100,false); QCOMPARE(changes.raised.size(),1);
    QVERIFY(changes.raised.first().body.contains(other.name));
    QVERIFY(box.sessions[0].unreadReply); QVERIFY(!box.sessions[0].attentionAcknowledged); QVERIFY(box.sessions[0].needsAction());
    QVERIFY(box.sessions[0].needsAttention()); QCOMPARE(box.sessions[0].phase,QString("approval"));
    box.sessions[0].muted=false;
    AttentionTracker restarted(tracker.state());
    QVERIFY(restarted.observe({},box,101,false).raised.isEmpty());
    box.sessions[0].phase="idle"; box.sessions[0].activity="idle"; box.sessions[0].subagents={};
    QVERIFY(restarted.observe({},box,102,false).raised.isEmpty()); // The reply arrived while muted, before idleness.
    box.sessions[0].replyId="reply-2";
    changes=restarted.observe({},box,103,false); QCOMPARE(changes.raised.size(),1);
    QCOMPARE(changes.raised.first().title,QString("Zerus — New reply"));
    box.sessions[0].phase="approval"; box.sessions[0].attentionId="approval-2";
    QCOMPARE(restarted.observe({},box,104,false).raised.size(),1);
}

void TestAttention::muteWithdrawsBannersAndConsumesOptionalQuestions()
{
    SessionInfo s; s.name="codex/project/session"; s.cmd="codex"; s.runId="run"; s.conversationId="conversation";
    s.phase="approval"; s.attentionId="approval-1";
    BoxState box; box.ok=true; box.sessions={s}; AttentionTracker tracker;
    auto changes=tracker.observe({},box,100,false); QCOMPARE(changes.raised.size(),1);
    const auto token=changes.raised.first().token; tracker.deliveryFailed(token,100);
    box.sessions[0].muted=true;
    changes=tracker.observe({},box,200,false); QVERIFY(changes.raised.isEmpty()); QCOMPARE(changes.cleared,QStringList{token});
    box.sessions[0].muted=false;
    QVERIFY(tracker.observe({},box,300,false).raised.isEmpty());
    box.sessions[0].attentionId="approval-2"; QCOMPARE(tracker.observe({},box,301,false).raised.size(),1);

    s.phase="working"; s.activity="busy"; s.optionalQuestionsReported=true; s.optionalQuestions=1; s.optionalQuestionIds={"q1"};
    box.sessions={s}; AttentionTracker questions; questions.setOptionalQuestions(true);
    QCOMPARE(questions.observe({},box,100,false).raised.size(),1);
    box.sessions[0].muted=true; box.sessions[0].optionalQuestions=-1;
    changes=questions.observe({},box,101,false); QVERIFY(changes.raised.isEmpty()); QCOMPARE(changes.cleared.size(),1);
    box.sessions[0].optionalQuestions=2; box.sessions[0].optionalQuestionIds={"q1","q2"};
    QVERIFY(questions.observe({},box,102,false).raised.isEmpty());
    box.sessions[0].muted=false;
    QVERIFY(questions.observe({},box,103,false).raised.isEmpty());
    box.sessions[0].optionalQuestions=3; box.sessions[0].optionalQuestionIds.append("q3");
    QCOMPARE(questions.observe({},box,104,false).raised.size(),1);
    QCOMPARE(box.sessions[0].optionalQuestions,3); QVERIFY(!box.sessions[0].attentionAcknowledged);
}

void TestAttention::mutePersistsByConversationAndKeepsOtherSessionsAudible()
{
    SessionInfo s; s.name="codex/project/session"; s.cmd="codex"; s.runId="run"; s.conversationId="conversation";
    s.created=10; s.phase="approval"; s.attentionId="approval-1"; s.replyId="reply-1";
    s.sharedNotifications=true; s.notificationPreferences=sharedPreferences(s.conversationId,false,0);
    BoxState local; local.ok=true; local.sessions={s};
    auto remote=local; remote.host="peer";
    FleetState fleet; fleet.setLocal(local,0); fleet.setPeer(remote,0);
    QCOMPARE(fleet.attentionSessions(0),2);
    const auto muted=sharedPreferences(s.conversationId,true,1);
    const auto marks=fleet.attentionMarks(), read=fleet.readReplies();
    QVERIFY(fleet.applyNotificationPreferences({},s,muted)); QCOMPARE(fleet.attentionSessions(0),1);
    QVERIFY(fleet.local().sessions.first().muted); QVERIFY(fleet.local().sessions.first().unreadReply);
    QVERIFY(!fleet.local().sessions.first().attentionAcknowledged); QVERIFY(fleet.local().sessions.first().needsAction());
    QVERIFY(!fleet.peer("peer")->sessions.first().muted); QVERIFY(fleet.takeNewReadMarks().isEmpty());
    QCOMPARE(fleet.attentionMarks(),marks); QCOMPARE(fleet.readReplies(),read);
    // Another device learns the confirmed preference from the host, not QSettings.
    local.sessions[0].name="codex/project/renamed"; local.sessions[0].runId="resumed";
    local.sessions[0].notificationPreferences=muted; local.sessions[0].muted=true;
    FleetState reopened;
    reopened.setLocal(local,0); QVERIFY(reopened.local().sessions.first().muted); QCOMPARE(reopened.attentionSessions(0),0);
    const auto current=local.sessions.first(); const auto unmuted=sharedPreferences(s.conversationId,false,2);
    auto stale=current; stale.runId="old-run";
    QVERIFY(!reopened.applyNotificationPreferences({},stale,unmuted));
    QVERIFY(reopened.applyNotificationPreferences({},current,unmuted)); QCOMPARE(reopened.attentionSessions(0),1);
    // A pre-write poll cannot roll back the confirmed revision.
    reopened.setLocal(local,1); QVERIFY(!reopened.local().sessions.first().muted);
    QCOMPARE(reopened.local().sessions.first().notificationPreferences,unmuted);
    local.sessions[0].notificationPreferences=sharedPreferences(s.conversationId,true,2);
    reopened.setLocal(local,2); QVERIFY(!reopened.local().sessions.first().muted);
    QVERIFY(!reopened.local().sessions.first().notificationPreferencesError.isEmpty());
    local.sessions[0].notificationPreferences=sharedPreferences(s.conversationId,true,3);
    reopened.setLocal(local,3); QVERIFY(reopened.local().sessions.first().muted);
    QVERIFY(reopened.local().sessions.first().notificationPreferencesError.isEmpty());
    auto unsupported=local;unsupported.sessions[0].sharedNotifications=false;unsupported.sessions[0].notificationPreferences={};unsupported.sessions[0].muted=false;
    reopened.setLocal(unsupported,4);QVERIFY(!reopened.local().sessions.first().sharedNotifications);QVERIFY(reopened.local().sessions.first().muted);
    reopened.setLocal(unsupported,5);QVERIFY(reopened.local().sessions.first().muted); // Repeated unknown polls do not erase confirmed host state.
    QVERIFY(!reopened.applyNotificationPreferences({},unsupported.sessions.first(),unmuted));
    reopened.setLocal(local,6);QVERIFY(reopened.local().sessions.first().sharedNotifications);QVERIFY(reopened.local().sessions.first().muted);
    local.sessions[0].conversationId="different"; local.sessions[0].muted=false;
    local.sessions[0].notificationPreferences=sharedPreferences("different",false,0);
    reopened.setLocal(local,4); QVERIFY(!reopened.local().sessions.first().muted);
    // Old local muted bits never supply a hidden fallback; review bits survive.
    QVERIFY(reopened.setReviewLater({},local.sessions.first(),true));
    auto legacy=reopened.attentionMarks();
    for(auto it=legacy.begin();it!=legacy.end();++it) { auto mark=it.value().toObject(); mark["muted"]=true; it.value()=mark; }
    reopened.setAttentionMarks(legacy); QVERIFY(!reopened.local().sessions.first().muted);
    QVERIFY(reopened.local().sessions.first().reviewLater);
    local.sessions[0].sharedNotifications=false; local.sessions[0].notificationPreferences={};
    reopened.setLocal(local,5); QVERIFY(!reopened.local().sessions.first().sharedNotifications);
    QVERIFY(!reopened.local().sessions.first().muted);
    QVERIFY(!reopened.applyNotificationPreferences({},local.sessions.first(),muted));
    QVERIFY(fleet.applyNotificationPreferences("peer",remote.sessions.first(),muted)); QCOMPARE(fleet.attentionSessions(0),0);
    remote.ok=false; fleet.setPeer(remote,10); QCOMPARE(fleet.attentionSessions(FleetState::kPeerStaleMs+1),0);
}

void TestAttention::sharedUnmuteSuppressesEventsMissedWhileOffline()
{
    SessionInfo s; s.name="codex/project/session"; s.cmd="codex"; s.runId="run"; s.conversationId="conversation";
    s.state="running"; s.activity="waiting"; s.phase="approval"; s.attentionId="approval-old"; s.attentionStarted=10;
    s.unreadReply=true; s.replyId="reply-old"; s.sharedNotifications=true;
    s.optionalQuestionsReported=true; s.optionalQuestions=1; s.optionalQuestionIds={"optional-old"};
    s.subagents={{"child",QJsonObject{{"display_state",""},{"state","input"},{"attention_id",""},{"question_id","child-old"},{"last_event_at",11}}}};
    s.notificationPreferences=sharedPreferences(s.conversationId,false,2,QJsonObject{
        {"reply_id",s.replyId},{"attention_signature",QString(64,'a')},{"optional_question_ids",QJsonArray{"optional-old"}},
        {"main_attention",QJsonObject{{"run_id",s.runId},{"phase",s.phase},{"request_id",s.attentionId},{"started_at",10}}},
        {"child_attention",QJsonObject{{"child",QJsonObject{{"phase","input"},{"request_id","child-old"},{"started_at",11}}}}}});
    BoxState box; box.ok=true; box.sessions={s};
    AttentionTracker offline; offline.setOptionalQuestions(true);
    QVERIFY(offline.observe("peer",box,100,false).raised.isEmpty()); // Device never saw mute or these events.
    QVERIFY(box.sessions.first().unreadReply); QVERIFY(!box.sessions.first().attentionAcknowledged);
    box.sessions[0].attentionId="approval-new";
    QCOMPARE(offline.observe("peer",box,101,false).raised.size(),1);
    box.sessions[0].attentionId=s.attentionId;
    auto child=box.sessions[0].subagents.value("child").toObject(); child["question_id"]="child-new"; box.sessions[0].subagents["child"]=child;
    QCOMPARE(offline.observe("peer",box,102,false).raised.size(),1);
    box.sessions[0].phase="idle"; box.sessions[0].activity="idle"; box.sessions[0].subagents={};
    box.sessions[0].optionalQuestions=2; box.sessions[0].optionalQuestionIds.append("optional-new");
    QCOMPARE(offline.observe("peer",box,103,false).raised.size(),1);
    box.sessions[0].optionalQuestions=0; box.sessions[0].optionalQuestionIds.clear(); box.sessions[0].replyId="reply-new";
    QCOMPARE(offline.observe("peer",box,104,false).raised.size(),1);
    AttentionTracker restarted(offline.state()); restarted.setOptionalQuestions(true);
    QVERIFY(restarted.observe("peer",box,105,false).raised.isEmpty());
}

void TestAttention::sharedSuppressionKeepsTimestampAndRunBoundaries()
{
    SessionInfo s; s.name="kimi/project/session"; s.cmd="kimi"; s.runId="run"; s.conversationId="conversation";
    s.state="running"; s.activity="waiting"; s.phase="input"; s.attentionStarted=10; s.sharedNotifications=true;
    auto baseline=sharedPreferences(s.conversationId,false,2).value("suppressed").toObject();
    baseline["main_attention"]=QJsonObject{{"run_id","run"},{"phase","input"},{"request_id",""},{"started_at",10}};
    s.notificationPreferences=sharedPreferences(s.conversationId,false,2,baseline);
    BoxState box; box.ok=true; box.sessions={s}; AttentionTracker tracker;
    QVERIFY(tracker.observe({},box,100,false).raised.isEmpty());
    box.sessions[0].attentionStarted=11; QCOMPARE(tracker.observe({},box,101,false).raised.size(),1);
    box.sessions[0].runId="replacement"; box.sessions[0].attentionStarted=10;
    QCOMPARE(tracker.observe({},box,102,false).raised.size(),1);
    box.sessions[0].phase="idle";box.sessions[0].activity="idle";
    auto preferences=s.notificationPreferences;auto all=preferences["suppressed"].toObject();
    all["child_attention"]=QJsonObject{{"child",QJsonObject{{"phase","input"},{"request_id",""},{"started_at",12}}}};
    preferences["suppressed"]=all;box.sessions[0].notificationPreferences=preferences;
    box.sessions[0].subagents={{"child",QJsonObject{{"state","input"},{"last_event_at",12}}}};
    QVERIFY(tracker.observe({},box,103,false).raised.isEmpty());
    auto child=box.sessions[0].subagents["child"].toObject();child["last_event_at"]=13;box.sessions[0].subagents["child"]=child;
    QCOMPARE(tracker.observe({},box,104,false).raised.size(),1);
}

void TestAttention::sharedSuppressionRetainsRawDshMultiRequestIdentity()
{
    SessionInfo s;s.name="dsh/project/session";s.cmd="dsh";s.runId="run";s.conversationId="conversation";
    s.state="running";s.activity="waiting";s.phase="input";s.attentionId="request-a\nrequest-b";s.sharedNotifications=true;
    auto preferences=sharedPreferences(s.conversationId,false,2);auto baseline=preferences["suppressed"].toObject();
    baseline["main_attention"]=QJsonObject{{"run_id","run"},{"phase","input"},{"request_id",s.attentionId},{"started_at",QJsonValue()}};
    preferences["suppressed"]=baseline;s.notificationPreferences=preferences;
    BoxState box;box.ok=true;box.sessions={s};AttentionTracker tracker;
    QVERIFY(tracker.observe({},box,100,false).raised.isEmpty());
    box.sessions[0].attentionId="request-a\nrequest-c";QCOMPARE(tracker.observe({},box,101,false).raised.size(),1);
}

void TestAttention::sharedSuppressionSurvivesCapabilityDowngrade()
{
    for (const bool withIds : {true, false}) {
        SessionInfo s;s.name="codex/project/session";s.cmd="codex";s.runId="run";s.conversationId="conversation";
        s.state="running";s.activity="waiting";s.phase="approval";s.attentionId=withIds?"main-old":"";s.attentionStarted=10;
        s.sharedNotifications=true;
        const auto childId=withIds?QString("child-old"):QString();
        s.subagents={{"child",QJsonObject{{"state","input"},{"attention_id",childId},{"attention_started",20}}}};
        auto prefs=sharedPreferences(s.conversationId,false,2);auto baseline=prefs["suppressed"].toObject();
        baseline["main_attention"]=QJsonObject{{"run_id",s.runId},{"phase",s.phase},{"request_id",s.attentionId},{"started_at",10}};
        baseline["child_attention"]=QJsonObject{{"child",QJsonObject{{"phase","input"},{"request_id",childId},{"started_at",20}}}};
        prefs["suppressed"]=baseline;s.notificationPreferences=prefs;
        BoxState box;box.host="peer";box.ok=true;box.sessions={s};FleetState fleet;fleet.setPeer(box,0);AttentionTracker tracker;
        QVERIFY(tracker.observe("peer",*fleet.peer("peer"),100,false).raised.isEmpty());
        box.sessions[0].sharedNotifications=false;box.sessions[0].notificationPreferences={};
        fleet.setPeer(box,1);const auto unknown=*fleet.peer("peer");
        QVERIFY(!unknown.sessions.first().sharedNotifications);QVERIFY(!unknown.sessions.first().muted);
        QCOMPARE(unknown.sessions.first().notificationPreferences,prefs);
        QVERIFY(tracker.observe("peer",unknown,101,false).raised.isEmpty());
        fleet.setPeer(box,2);QVERIFY(tracker.observe("peer",*fleet.peer("peer"),102,false).raised.isEmpty());
        if(withIds)box.sessions[0].attentionId="main-new";else box.sessions[0].attentionStarted=11;
        fleet.setPeer(box,3);QCOMPARE(tracker.observe("peer",*fleet.peer("peer"),103,false).raised.size(),1);
        QVERIFY(tracker.observe("peer",*fleet.peer("peer"),104,false).raised.isEmpty());
        auto child=box.sessions[0].subagents["child"].toObject();
        if(withIds)child["attention_id"]="child-new";else child["attention_started"]=21;
        box.sessions[0].subagents["child"]=child;fleet.setPeer(box,4);
        QCOMPARE(tracker.observe("peer",*fleet.peer("peer"),105,false).raised.size(),1);
        AttentionTracker restarted(tracker.state());
        QVERIFY(restarted.observe("peer",*fleet.peer("peer"),106,false).raised.isEmpty());
    }
}

void TestAttention::sharedOptionalBaselineDoesNotEvictNewObservedQuestions()
{
    SessionInfo s;s.name="codex/project/session";s.cmd="codex";s.runId="run";s.conversationId="conversation";
    s.state="running";s.phase="working";s.activity="busy";s.sharedNotifications=true;
    s.optionalQuestionsReported=true;s.optionalQuestions=1;s.optionalQuestionIds={"q-new"};
    s.notificationPreferences=sharedPreferences(s.conversationId,false,0);
    BoxState box;box.ok=true;box.sessions={s};AttentionTracker tracker;tracker.setOptionalQuestions(true);
    QCOMPARE(tracker.observe({},box,100,false).raised.size(),1);
    QJsonArray oldIds;for(int i=0;i<64;++i)oldIds.append(QString("old-%1").arg(i,2,10,QChar('0')));
    auto prefs=sharedPreferences(s.conversationId,false,2);auto baseline=prefs["suppressed"].toObject();
    baseline["optional_question_ids"]=oldIds;prefs["suppressed"]=baseline;box.sessions[0].notificationPreferences=prefs;
    QVERIFY(tracker.observe({},box,101,false).raised.isEmpty());
    auto seen=tracker.state().value("").toObject()["question:codex:conversation"].toObject()["seen"].toArray();
    QVERIFY(seen.contains("q-new"));QVERIFY(seen.size()<=64);
    box.sessions[0].optionalQuestionIds.append("old-00");box.sessions[0].optionalQuestions=2;
    QVERIFY(tracker.observe({},box,102,false).raised.isEmpty()); // Baseline membership still suppresses its old questions.
    box.sessions[0].optionalQuestionIds.append("q-newer");box.sessions[0].optionalQuestions=3;
    QCOMPARE(tracker.observe({},box,103,false).raised.size(),1);
    QVERIFY(tracker.observe({},box,104,false).raised.isEmpty());
}

void TestAttention::sharedPreferencesDoNotCrossProviders()
{
    SessionInfo codex;codex.name="codex/project/session";codex.cmd="codex";codex.runId="codex-run";codex.conversationId="opaque-id";
    codex.sharedNotifications=true;codex.muted=true;codex.notificationPreferences=sharedPreferences("opaque-id",true,7);
    SessionInfo kimi=codex;kimi.name="kimi/project/session";kimi.cmd="kimi";kimi.runId="kimi-run";
    kimi.muted=false;kimi.notificationPreferences=sharedPreferences("opaque-id",false,0);
    BoxState box;box.ok=true;box.sessions={codex,kimi};FleetState fleet;fleet.setLocal(box,0);
    // Same opaque conversation ID is independent for each provider, including on downgrade.
    for(auto &session:box.sessions){session.sharedNotifications=false;session.notificationPreferences={};session.muted=false;}
    fleet.setLocal(box,1);const auto sessions=fleet.local().sessions;
    QVERIFY(sessions[0].muted);QVERIFY(!sessions[1].muted);
    QCOMPARE(sessions[0].notificationPreferences,codex.notificationPreferences);
    QCOMPARE(sessions[1].notificationPreferences,kimi.notificationPreferences);
    box.sessions={kimi};box.sessions[0].sharedNotifications=false;box.sessions[0].notificationPreferences={};
    FleetState onlyCodex;BoxState prior;prior.ok=true;prior.sessions={codex};onlyCodex.setLocal(prior,0);onlyCodex.setLocal(box,1);
    QVERIFY(!onlyCodex.local().sessions.first().muted);QVERIFY(onlyCodex.local().sessions.first().notificationPreferences.isEmpty());
}

void TestAttention::sharedTimestampSuppressionRequiresKnownCurrentTime()
{
    const QJsonObject valid{{"phase","input"},{"request_id",""},{"started_at",10}};
    QVERIFY(SessionNotifications::matches(valid,"input",{},10));
    for(const auto &unknown:QList<QJsonValue>{QJsonValue(),0,-1})
        QVERIFY(!SessionNotifications::matches(valid,"input",{},unknown));
    auto invalid=valid;invalid["started_at"]=QJsonValue();
    QVERIFY(!SessionNotifications::attention(invalid,false));
    QVERIFY(!SessionNotifications::matches(invalid,"input",{},QJsonValue()));
    invalid["started_at"]=0;QVERIFY(!SessionNotifications::attention(invalid,false));
}

namespace {
SessionInfo optionalQuestionSession(const QStringList &ids)
{
    SessionInfo s; s.name="codex/project/session"; s.cmd="codex"; s.runId="run"; s.conversationId="conversation";
    s.phase="working"; s.activity="busy"; s.optionalQuestionsReported=true;
    s.optionalQuestions=int(ids.size()); s.optionalQuestionIds=ids;
    return s;
}
}

void TestAttention::optionalQuestionsNotifyOncePerQuestionWhenEnabled()
{
    BoxState box; box.ok=true; box.host="arch"; box.sessions={optionalQuestionSession({"a"})};
    AttentionTracker tracker;
    // Off by default; questions seen meanwhile are not announced later.
    QVERIFY(tracker.observe({},box,1000,false).raised.isEmpty());
    tracker.setOptionalQuestions(true);
    QVERIFY(tracker.observe({},box,1001,false).raised.isEmpty());
    box.sessions={optionalQuestionSession({"a","b"})};
    auto changes=tracker.observe({},box,1002,false);
    QCOMPARE(changes.raised.size(),1); QVERIFY(changes.cleared.isEmpty());
    QCOMPARE(changes.raised[0].title,QString("Zerus — Optional question"));
    QVERIFY(changes.raised[0].body.contains("codex/project/session / arch"));
    QVERIFY(changes.raised[0].body.contains("2 optional questions waiting"));
    const auto target=AttentionTracker::target(changes.raised[0].token);
    QCOMPARE(target.value("question").toString(),QString("b"));
    QVERIFY(tracker.observe({},box,1003,false).raised.isEmpty());
    AttentionTracker restarted(tracker.state()); restarted.setOptionalQuestions(true);
    QVERIFY(restarted.observe({},box,1004,false).raised.isEmpty());
    // Answering one keeps the banner; a new question replaces it.
    box.sessions={optionalQuestionSession({"b"})};
    changes=restarted.observe({},box,1005,false); QVERIFY(changes.raised.isEmpty()); QVERIFY(changes.cleared.isEmpty());
    box.sessions={optionalQuestionSession({"b","c"})};
    changes=restarted.observe({},box,1006,false); QCOMPARE(changes.raised.size(),1); QCOMPARE(changes.cleared.size(),1);
    QVERIFY(changes.raised[0].body.contains("2 optional questions waiting"));
    box.sessions={optionalQuestionSession({})};
    changes=restarted.observe({},box,1007,false); QVERIFY(changes.raised.isEmpty()); QCOMPARE(changes.cleared.size(),1);
    // Turning banners off withdraws them and still records new questions.
    box.sessions={optionalQuestionSession({"d"})};
    QCOMPARE(restarted.observe({},box,1008,false).raised.size(),1);
    restarted.setOptionalQuestions(false);
    box.sessions={optionalQuestionSession({"d","e"})};
    changes=restarted.observe({},box,1009,false); QVERIFY(changes.raised.isEmpty()); QCOMPARE(changes.cleared.size(),1);
    restarted.setOptionalQuestions(true);
    QVERIFY(restarted.observe({},box,1010,false).raised.isEmpty());
    // Acknowledging the session here or on another device withdraws the banner.
    box.sessions={optionalQuestionSession({"d","e","f"})};
    QCOMPARE(restarted.observe({},box,1011,false).raised.size(),1);
    box.sessions[0].attentionAcknowledged=true;
    changes=restarted.observe({},box,1012,false); QVERIFY(changes.raised.isEmpty()); QCOMPARE(changes.cleared.size(),1);
    box.sessions={optionalQuestionSession({"d","e","f","g"})}; box.sessions[0].attentionAcknowledged=true;
    QVERIFY(restarted.observe({},box,1013,false).raised.isEmpty());
    // The tray and badge counts ignore optional questions.
    FleetState fleet; fleet.setLocal(box,1000); QCOMPARE(fleet.attentionSessions(1000),0);
}

void TestAttention::optionalQuestionsSurviveUnknownCountsAndResume()
{
    BoxState box; box.ok=true; box.host="arch"; box.sessions={optionalQuestionSession({"a"})};
    AttentionTracker tracker; tracker.setOptionalQuestions(true);
    const auto first=tracker.observe({},box,1000,false); QCOMPARE(first.raised.size(),1);
    QVERIFY(first.raised[0].body.endsWith("Optional question waiting"));
    // An unreadable question index keeps the banner and its IDs.
    box.sessions[0].optionalQuestions=-1; box.sessions[0].optionalQuestionIds.clear();
    auto changes=tracker.observe({},box,1001,false); QVERIFY(changes.raised.isEmpty()); QVERIFY(changes.cleared.isEmpty());
    box.sessions={optionalQuestionSession({"a"})};
    changes=tracker.observe({},box,1002,false); QVERIFY(changes.raised.isEmpty()); QVERIFY(changes.cleared.isEmpty());
    // Stopping clears it; resuming the conversation with the same question is quiet.
    box.sessions[0].state="stopped";
    changes=tracker.observe({},box,1003,false); QVERIFY(changes.raised.isEmpty()); QCOMPARE(changes.cleared.size(),1);
    box.sessions={optionalQuestionSession({"a"})}; box.sessions[0].runId="resumed"; box.sessions[0].name="renamed";
    QVERIFY(tracker.observe({},box,1004,false).raised.isEmpty());
    // An archived instance of the same conversation does not hide the live one.
    auto archived=optionalQuestionSession({}); archived.state="archived"; archived.runId="old"; archived.name="saved";
    auto resumed=optionalQuestionSession({"a","b"}); resumed.runId="resumed"; resumed.name="renamed";
    box.sessions={archived,resumed};
    changes=tracker.observe({},box,1005,false); QCOMPARE(changes.raised.size(),1);
    QVERIFY(changes.raised[0].body.startsWith("renamed / arch"));
    const auto target=AttentionTracker::target(changes.raised[0].token);
    QCOMPARE(target.value("run").toString(),QString("resumed"));
    QVERIFY(AttentionTracker::matches(target,resumed)); QVERIFY(!AttentionTracker::matches(target,archived));
    auto other=resumed; other.conversationId="other"; QVERIFY(!AttentionTracker::matches(target,other));
    resumed.optionalQuestionIds<<"c"; resumed.optionalQuestions=3; box.sessions={resumed,archived};
    changes=tracker.observe({},box,1006,false); QCOMPARE(changes.raised.size(),1);
    QVERIFY(changes.raised[0].body.startsWith("renamed / arch"));
    // Questions first seen while Zerus has focus are only recorded.
    box.sessions[0].optionalQuestionIds<<"d"; box.sessions[0].optionalQuestions=4;
    changes=tracker.observe({},box,1007,true); QVERIFY(changes.raised.isEmpty()); QVERIFY(changes.cleared.isEmpty());
    QVERIFY(tracker.observe({},box,1008,false).raised.isEmpty());
    // Hosts without the field add nothing.
    auto older=optionalQuestionSession({"z"}); older.conversationId="older"; older.optionalQuestionsReported=false;
    box.sessions={older}; QVERIFY(tracker.observe({},box,1009,false).raised.isEmpty());
}

void TestAttention::optionalQuestionsDeferToRequestsAndRetry()
{
    BoxState box; box.ok=true; box.host="arch"; box.sessions={optionalQuestionSession({"a"})};
    box.sessions[0].phase="input"; box.sessions[0].attentionId="call-1";
    AttentionTracker tracker; tracker.setOptionalQuestions(true);
    // A request that needs action notifies alone and its questions are recorded.
    auto changes=tracker.observe({},box,1000,false);
    QCOMPARE(changes.raised.size(),1); QCOMPARE(changes.raised[0].title,QString("Zerus — Needs attention"));
    box.sessions[0].phase="working";
    changes=tracker.observe({},box,1001,false); QVERIFY(changes.raised.isEmpty()); QCOMPARE(changes.cleared.size(),1);
    // Optional notices come first in one poll, so a blocking request posts last.
    auto waiting=optionalQuestionSession({}); waiting.name="codex/project/other"; waiting.runId="run-2";
    waiting.conversationId="conversation-2"; waiting.phase="approval"; waiting.attentionId="call-2";
    box.sessions={optionalQuestionSession({"a","b"}),waiting};
    changes=tracker.observe({},box,1002,false); QCOMPARE(changes.raised.size(),2);
    QCOMPARE(changes.raised[0].title,QString("Zerus — Optional question"));
    QCOMPARE(changes.raised[1].title,QString("Zerus — Needs attention"));
    // A failed delivery retries after a minute while questions remain.
    tracker.deliveryFailed(changes.raised[0].token,1002);
    QVERIFY(tracker.observe({},box,1030,false).raised.isEmpty());
    changes=tracker.observe({},box,1063,false); QCOMPARE(changes.raised.size(),1); QCOMPARE(changes.cleared.size(),1);
    QCOMPARE(changes.raised[0].title,QString("Zerus — Optional question"));
    QVERIFY(tracker.observe({},box,1124,false).raised.isEmpty());
}

void TestAttention::nativeChildCountersPreserveUnknownEvidence()
{
    SessionInfo s; s.activity="idle"; s.subagentActiveCount=2; s.subagentTotalCount=6;
    s.subagentCountsComplete=false;
    for (const auto &source : {QString("dsh_native"), QString("kimi_native"), QString("opencode_native"), QString("mixed_hooks")}) {
        s.subagentSource=source;
        QCOMPARE(SessionPresentation::childCount(s,false,true,false),QString("2+"));
        QCOMPARE(SessionPresentation::childCount(s,false,true,true),QString("2+/6"));
    }
    s.subagentSource="dsh_native";s.subagentActiveCount=0;s.subagentTotalCount=1;
    QCOMPARE(SessionPresentation::childCount(s,false,true,false),QString("0+"));
    QVERIFY(SessionPresentation::childCount(s,false,false,false).isEmpty());
    // OpenCode's API host never confirms classic child work; unknown stays visible.
    s.subagentSource="opencode_native";s.subagentActiveCount=0;s.subagentTotalCount=2;
    QCOMPARE(SessionPresentation::childCount(s,false,true,true),QString("0+/2"));
    s.subagentCountsComplete=true;
    QVERIFY(SessionPresentation::childCount(s,false,true,false).isEmpty());
    QCOMPARE(SessionPresentation::childCount(s,true,true,true),QString("0 active / 2 total"));
}

void TestAttention::replyWaitsForLiveBackgroundShellToFinish()
{
    SessionInfo s; s.name="codex/project/session"; s.runId="run"; s.conversationId="conversation";
    s.phase="idle"; s.activity="idle"; s.replyId="reply"; s.unreadReply=true; s.backgroundProcesses=1;
    BoxState box; box.ok=true; box.sessions={s}; AttentionTracker tracker;
    QVERIFY(tracker.observe({},box,1000,false).raised.isEmpty());
    box.sessions[0].backgroundProcesses=0;
    QCOMPARE(tracker.observe({},box,1001,false).raised.size(),1);
    QVERIFY(tracker.observe({},box,1002,false).raised.isEmpty());
}

void TestAttention::replyWaitsForExplicitIndependentResult()
{
    SessionInfo s; s.name="codex/project/session";s.runId="run";s.conversationId="conversation";
    s.phase="idle";s.activity="idle";s.replyId="progress";s.unreadReply=true;s.subagentActiveCount=0;
    s.coordinationWait={{"schema",1},{"status","ok"},{"revision",1},{"run_id",s.runId},{"conversation_id",s.conversationId},{"count",1}};
    BoxState box;box.ok=true;box.sessions={s};AttentionTracker tracker;
    QVERIFY(tracker.observe({},box,1000,false).raised.isEmpty());
    box.sessions[0].coordinationWait["count"]=0;
    QCOMPARE(tracker.observe({},box,1001,false).raised.size(),1);
    QVERIFY(tracker.observe({},box,1002,false).raised.isEmpty());
}

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
    QVERIFY(fleet.tooltip(1500).startsWith("1 needs attention"));
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

void TestAttention::batchReadSkipsSessionsMarkedForLaterEvenAfterTheSnapshot()
{
    // Automatic attention (approval, question, error) beside manual review reminders,
    // on this computer and a peer, with shared and per-device read state.
    SessionInfo ready; ready.name = "codex/project/ready"; ready.cmd = "codex"; ready.runId = "run"; ready.sharedRead = true;
    ready.conversationId = "ready"; ready.replyId = "10:100"; ready.activity = "idle"; ready.phase = "idle";
    auto approval = ready; approval.name = "codex/project/approval"; approval.conversationId = "approval"; approval.phase = "approval";
    auto input = ready; input.name = "codex/project/question"; input.conversationId = "question"; input.phase = "input";
    auto error = ready; error.name = "codex/project/error"; error.conversationId = "error"; error.phase = "error";
    auto manual = ready; manual.name = "codex/project/manual"; manual.conversationId = "manual";
    auto legacy = manual; legacy.name = "codex/project/legacy"; legacy.conversationId = "legacy"; legacy.sharedRead = false;
    auto late = ready; late.name = "codex/project/late"; late.conversationId = "late";
    BoxState local; local.host = "arch"; local.ok = true; local.sessions = {ready, approval, input, error, manual, legacy, late};
    auto remoteManual = manual; remoteManual.conversationId = "remote-manual";
    auto remoteReady = ready; remoteReady.conversationId = "remote-ready";
    BoxState remote; remote.host = "mac"; remote.ok = true; remote.sessions = {remoteManual, remoteReady};
    FleetState fleet; fleet.setLocal(local, 0); fleet.setPeer(remote, 0);
    QVERIFY(fleet.setReviewLater({}, manual, true)); QVERIFY(fleet.setReviewLater({}, legacy, true));
    QVERIFY(fleet.setReviewLater("mac", remoteManual, true));
    // Mark all as read never offers a reply the person kept for later.
    const auto requested = fleet.unreadReplies();
    QCOMPARE(requested.size(), 6);
    // A reminder set after the click's snapshot still protects that session,
    // and an answer that arrives meanwhile stays unread.
    QVERIFY(fleet.setReviewLater({}, late, true));
    local.sessions[0].replyId = "11:101"; fleet.setLocal(local, 1);
    QCOMPARE(fleet.markRepliesRead(requested), 4);
    QVERIFY(fleet.local().sessions[0].unreadReply);
    for (int i : {1, 2, 3}) {
        QVERIFY(!fleet.local().sessions[i].unreadReply);
        QVERIFY(fleet.local().sessions[i].needsAttention());
    }
    for (int i : {4, 5, 6}) {
        const auto &s = fleet.local().sessions[i];
        QVERIFY(s.unreadReply); QVERIFY(s.reviewLater); QVERIFY(s.needsAttention());
    }
    QVERIFY(fleet.peer("mac")->sessions[0].unreadReply); QVERIFY(fleet.peer("mac")->sessions[0].reviewLater);
    QVERIFY(!fleet.peer("mac")->sessions[1].unreadReply);
    // Nothing of the reminded sessions is shared with their hosts or kept on this device.
    QStringList shared;
    for (const auto &mark : fleet.takeNewReadMarks()) shared << mark.host + '/' + mark.conversationId;
    shared.sort(); QCOMPARE(shared, QStringList({"/approval", "/error", "/question", "mac/remote-ready"}));
    QCOMPARE(fleet.readReplies().size(), 4);
    QCOMPARE(fleet.unreadReplies().size(), 1); // Only the newer answer is offered next.
    // Mark as read on one reminded session stays explicit: it clears the reply and the reminder.
    const auto chosen = fleet.local().sessions[4];
    QVERIFY(fleet.markSessionRead({}, chosen));
    QVERIFY(!fleet.local().sessions[4].unreadReply); QVERIFY(!fleet.local().sessions[4].reviewLater);
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
    QVERIFY(!fleet.local().sessions[0].visibleUnreadReply()); QCOMPARE(fleet.attentionSessions(0), 0);
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
    QVERIFY(!fleet.local().sessions[0].visibleUnreadReply()); QCOMPARE(fleet.attentionSessions(4), 0);

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
    QCOMPARE(fleet.attentionSessions(6), 1);
    changes=restarted.observe({},fleet.local()); QCOMPARE(changes.raised.size(),1);
    QVERIFY(changes.raised[0].body.contains("Approval needed"));
    QCOMPARE(changes.cleared.size(),1);

    box.sessions[0].activity="idle"; box.sessions[0].phase="idle";
    box.sessions[0].subagentActiveCount=0; box.sessions[0].replyId="final"; fleet.setLocal(box,7);
    QVERIFY(fleet.local().sessions[0].visibleUnreadReply()); QCOMPARE(fleet.attentionSessions(7), 1);
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

QTEST_APPLESS_MAIN(TestAttention)
#include "test_attention.moc"
