#include <QtTest>
#include <QDateTime>
#include <QFile>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QTemporaryDir>
#include <QVBoxLayout>
#include "FakeCli.h"
#include "SessionSearch.h"

class TestSessionSearch : public QObject {
    Q_OBJECT
private slots:
    void init();
    void resultsKeepArchivesRunsAndRemoteIdentities();
    void explicitArchiveHitWorksWithoutEagerArchiveRows();
    void activeScopeIncludesChildRequestsAndOfflineReminders();
    void activeScopeIncludesDraftsAndRefreshesMembership();
    void staleQueriesCannotReplaceCurrentResults();
    void multipleMachinesScopeSearch();
    void projectScopeKeepsExactLiveArchiveAndRemoteMembership();
    void folderScopeFollowsTheMachinePathStyle();
    void selectionSurvivesFleetPollsAndExplicitRefresh();
    void errorsFollowHostScopeAndRecoverWhenPeerConnects();
    void newlyAddedPeerIsSearchedForTheCurrentQuery();
    void selectedResultCanBeReopenedWithoutDuplicateActivation();
    void identifiersMatchWithoutHistoryAndKeepScope();
    void taskNumbersRemainPublicMessageMatches();
    void preview();
private:
    FleetState fleet(bool remoteOnline = true) const;
    void configure(const QString &host, const QString &query, const QJsonArray &results, int delayMs = 0, const QString &error = {});
    QJsonObject hit(const QString &name, const QString &run, const QString &id, const QString &text, const QString &archive = {}) const;
    QListWidget *list(SessionSearch &search) const { return search.findChild<QListWidget *>("searchResults"); }
    QLabel *status(SessionSearch &search) const { return search.findChild<QLabel *>("searchStatus"); }
    QJsonArray calls() const;
    QString script() const { return m_dir.filePath("hgs"); }
    QTemporaryDir m_dir;
    QJsonObject m_replies;
};
void TestSessionSearch::init()
{
    QVERIFY(m_dir.isValid()); m_replies = {};
    QFile::remove(m_dir.filePath("calls.jsonl"));
    QFile executable(script()); QVERIFY(executable.open(QIODevice::WriteOnly));
    executable.write(R"(#!/usr/bin/env python3
import json,pathlib,sys,time
root=pathlib.Path(__file__).parent
args=sys.argv[1:]
host=args.pop(0)[1:] if args[0].startswith('@') else ''
assert args[0]=='search'
query=args[args.index('--query')+1]
with (root/'calls.jsonl').open('a') as out:out.write(json.dumps([host,query])+'\n')
value=json.loads((root/'replies.json').read_text()).get(host+'\n'+query,{'results':[]})
time.sleep(value.get('delay',0)/1000)
if value.get('error'):
 print(value['error'],file=sys.stderr);sys.exit(1)
print(json.dumps({'query':query,'results':value.get('results',[]),'truncated':False}))
)");
    executable.close(); QVERIFY(FakeCli::makeRunnable(executable.fileName()));
    configure({}, "needle", {
        hit("codex/docs/live", "live-run", "live-message", "Needle live body"),
        hit("codex/docs/saved", "saved-run", "saved-message", "Needle saved body"),
        hit("codex/docs/live", "archive-run", "archive-message", "Needle archived body", "archive-one"),
        hit("codex/docs/live", "wrong-run", "stale-message", "Needle from wrong run")});
    configure("mac", "needle", {hit("kimi/remote", "remote-run", "remote-message", "Needle remote body")});
}
void TestSessionSearch::explicitArchiveHitWorksWithoutEagerArchiveRows(){
    auto state=fleet();auto box=state.local();box.archivePaging=true;
    box.sessions.removeIf([](const SessionInfo&s){return s.state=="archived";});state.setLocal(box,QDateTime::currentMSecsSinceEpoch());
    auto result=hit("codex/docs/archived","run-archive","message-archive","Needle in old archive","10000000-0000-4000-8000-000000000001");
    result["session"]=QJsonObject{{"name","codex/docs/archived"},{"run_id","run-archive"},{"conversation_id","conversation-archive"},{"cmd","codex"},{"state","archived"},{"archive_id","10000000-0000-4000-8000-000000000001"},{"archived_at",100},{"tracked",true},{"resumable",true}};
    configure({},"needle",QJsonArray{result});
    SessionSearch search(FakeCli::program(script()));QSignalSpy archives(&search,&SessionSearch::archiveSessionsReady);
    search.setScope(state,"all",{});search.setQuery("needle");
    QTRY_VERIFY(!archives.isEmpty());QTRY_VERIFY(status(search)->text().contains("2 messages"));
    bool found=false;for(int i=0;i<list(search)->count();++i)found|=list(search)->item(i)->data(Qt::AccessibleTextRole).toString().contains("Needle in old archive");QVERIFY(found);
}

void TestSessionSearch::configure(const QString &host, const QString &query, const QJsonArray &results, int delayMs, const QString &error)
{
    m_replies[host + '\n' + query] = QJsonObject{{"results", results}, {"delay", delayMs}, {"error", error}};
    QFile file(m_dir.filePath("replies.json")); QVERIFY(file.open(QIODevice::WriteOnly)); file.write(QJsonDocument(m_replies).toJson());
}
QJsonObject TestSessionSearch::hit(const QString &name, const QString &run, const QString &id, const QString &text, const QString &archive) const
{
    return {{"name", name}, {"run_id", run}, {"archive_id", archive}, {"message_id", id},
        {"role", "assistant"}, {"at", 1791018000.0}, {"snippet", text},
        {"event", QJsonObject{{"type", "AgentMessage"}, {"message_id", id}, {"at", 1791018000.0}, {"detail", text}}}};
}
FleetState TestSessionSearch::fleet(bool remoteOnline) const
{
    SessionInfo live; live.name = "codex/docs/live"; live.project = "docs"; live.tag = "live"; live.cmd = "codex";
    live.runId = "live-run"; live.state = "running"; live.tracked = true; live.phase = "tool"; live.activity = "busy";
    auto saved = live; saved.name = "codex/docs/saved"; saved.tag = "saved"; saved.runId = "saved-run"; saved.state = "paused";
    auto archive = live; archive.state = "archived"; archive.runId = "archive-run"; archive.archiveId = "archive-one";
    BoxState local; local.host = "arch"; local.ok = true; local.sessions = {live, saved, archive};
    SessionInfo remote; remote.name = "kimi/remote"; remote.project = "remote"; remote.cmd = "kimi"; remote.runId = "remote-run";
    remote.state = "running"; remote.tracked = true; remote.phase = "input"; remote.activity = "busy";
    BoxState mac; mac.host = "mac"; mac.ok = true; mac.sessions = {remote};
    const auto now = QDateTime::currentMSecsSinceEpoch();
    FleetState state; state.setLocal(local, now); state.setPeer(mac, now);
    if (!remoteOnline) { mac.ok = false; state.setPeer(mac, now); }
    return state;
}
QJsonArray TestSessionSearch::calls() const
{
    QFile file(m_dir.filePath("calls.jsonl")); if (!file.open(QIODevice::ReadOnly)) return {};
    QJsonArray result; for (const auto &line : file.readAll().split('\n')) if (!line.isEmpty()) result.append(QJsonDocument::fromJson(line).array());
    return result;
}
void TestSessionSearch::projectScopeKeepsExactLiveArchiveAndRemoteMembership()
{
    SessionSearch search(FakeCli::program(script())); search.show();
    search.setScope(fleet(),"all",{}); search.setQuery("needle");
    search.setProjectScope({"\ncodex/docs/live\narchive-one", "mac\nkimi/remote"},true);
    QTRY_COMPARE(list(search)->count(),2);
    QVERIFY(list(search)->item(0)->data(Qt::AccessibleTextRole).toString().contains("Archive"));
    search.setHostsScope(fleet(),"all",{"@local"}); QCOMPARE(list(search)->count(),1);
    search.setProjectScope({},true); QCOMPARE(list(search)->count(),0); // An empty project has no matches.
    search.setProjectScope({},false); QCOMPARE(list(search)->count(),3);
    search.setProjectScope({"\ncodex/docs/live"},true); QCOMPARE(list(search)->count(),1);
    QVERIFY(!list(search)->item(0)->data(Qt::AccessibleTextRole).toString().contains("Archive"));
}

void TestSessionSearch::resultsKeepArchivesRunsAndRemoteIdentities()
{
    SessionSearch search(FakeCli::program(script())); search.resize(340, 500); search.show();
    const auto state = fleet(); search.setScope(state, "all", {}); search.setQuery("needle");
    QSignalSpy activated(&search, &SessionSearch::resultActivated);
    QTRY_COMPARE(list(search)->count(), 4);
    QVERIFY(status(search)->text().contains("4 messages")); QCOMPARE(activated.size(), 0);
    QListWidgetItem *archived = nullptr, *remote = nullptr;
    for (int i = 0; i < list(search)->count(); ++i) {
        auto *item = list(search)->item(i); const auto description = item->data(Qt::AccessibleTextRole).toString();
        QVERIFY(!description.contains("wrong run"));
        if (description.contains("Archive")) archived = item;
        if (description.contains("remote body")) remote = item;
    }
    QVERIFY(archived); QVERIFY(remote); list(search)->setCurrentItem(archived);
    QCOMPARE(activated.last()[0].toString(), ""); QCOMPARE(activated.last()[1].toString(), "codex/docs/live");
    QCOMPARE(activated.last()[2].toString(), "archive-one"); QCOMPARE(activated.last()[3].toString(), "archive-run");
    QCOMPARE(activated.last()[4].toJsonObject()["detail"].toString(), "Needle archived body");
    list(search)->setCurrentItem(remote); QCOMPARE(activated.last()[0].toString(), "mac");
    search.setScope(state, "archived", {}); QCOMPARE(list(search)->count(), 1);
    QVERIFY(list(search)->item(0)->data(Qt::AccessibleTextRole).toString().contains("Archive"));
    search.setScope(state, "paused", "@local"); QCOMPARE(list(search)->count(), 1);
    QVERIFY(list(search)->item(0)->data(Qt::AccessibleTextRole).toString().contains("saved body"));
    search.setScope(state, "active", "@local"); QCOMPARE(list(search)->count(), 1);
    search.setScope(state, "active", {}); QCOMPARE(list(search)->count(), 2);
    search.setScope(state, "all", "mac"); QCOMPARE(list(search)->count(), 1);
    search.setQuery({}); QVERIFY(!search.active()); QCOMPARE(list(search)->count(), 0);
}
void TestSessionSearch::activeScopeIncludesChildRequestsAndOfflineReminders()
{
    auto state = fleet(); auto box = state.local();
    auto &live = box.sessions[0]; live.activity = "idle"; live.phase = "idle";
    live.subagentSource = "hooks"; live.subagents = {{"child", QJsonObject{{"display_state", "input"}}}};
    auto &saved = box.sessions[1]; saved.reviewLater = true;
    state.setLocal(box, QDateTime::currentMSecsSinceEpoch());
    QVERIFY(state.setReviewLater({}, saved, true));
    SessionSearch search(FakeCli::program(script())); search.setScope(state, "active", {}); search.setQuery("needle");
    QTRY_COMPARE(list(search)->count(), 3);
    search.setScope(state, "active", "@local"); QCOMPARE(list(search)->count(), 2);
    box.ok = false; state.setLocal(box, QDateTime::currentMSecsSinceEpoch());
    search.setScope(state, "active", "@local"); QCOMPARE(list(search)->count(), 1);
    QVERIFY(list(search)->item(0)->data(Qt::AccessibleTextRole).toString().contains("saved body"));
    QVERIFY(state.setReviewLater({}, saved, false));
    box.ok = true; box.sessions[0].subagentSource = "hook_profiles";
    state.setLocal(box, QDateTime::currentMSecsSinceEpoch()); search.setScope(state, "active", "@local");
    QCOMPARE(list(search)->count(), 0);
}

void TestSessionSearch::activeScopeIncludesDraftsAndRefreshesMembership()
{
    auto state = fleet(); const QSet<QString> drafts{"\ncodex/docs/saved", "\ncodex/docs/live", "mac\nkimi/remote"};
    SessionSearch search(FakeCli::program(script())); search.setScope(state, "active", {}, drafts); search.setQuery("needle");
    QTRY_COMPARE(list(search)->count(), 3); // Busy + draft is still one hit; archive stays excluded.
    search.setScope(state, "active", "@local", drafts); QCOMPARE(list(search)->count(), 2);
    search.setScope(state, "active", "@local"); QCOMPARE(list(search)->count(), 1);
    auto box = state.local(); box.ok = false; state.setLocal(box, QDateTime::currentMSecsSinceEpoch());
    search.setScope(state, "active", "@local", drafts);
    search.setQuery("saved-run"); QCOMPARE(list(search)->count(), 1);
    search.setScope(state, "active", "@local"); QCOMPARE(list(search)->count(), 0);
}

void TestSessionSearch::multipleMachinesScopeSearch()
{
    auto state=fleet(); BoxState third; third.host="build"; third.ok=true;
    SessionInfo session; session.name="codex/build"; session.runId="build-run"; session.state="running"; third.sessions={session}; state.setPeer(third,0);
    configure("build","needle",{hit("codex/build","build-run","build-msg","needle build")});
    SessionSearch search(FakeCli::program(script())); search.setHostsScope(state,"all",{"@local","mac"}); search.setQuery("needle");
    QTRY_COMPARE(list(search)->count(),4); QCOMPARE(calls().size(),2);
    for(const auto &call:calls()) QVERIFY(call.toArray()[0].toString()!="build");
    search.setHostsScope(state,"all",{"mac","build"}); QTRY_COMPARE(list(search)->count(),2);
    for(int i=0;i<list(search)->count();++i) QVERIFY(!list(search)->item(i)->data(Qt::AccessibleTextRole).toString().contains("Needle live body"));
    QCOMPARE(calls().size(),3); search.setHostsScope(state,"all",{}); QCOMPARE(list(search)->count(),5);
}

void TestSessionSearch::folderScopeFollowsTheMachinePathStyle()
{
    // A local Windows folder scope matches sessions in any case and with either
    // separator; a peer is POSIX even when it reports Windows, and keeps exact names.
    auto state = fleet(); auto box = state.local(); box.metrics["platform"] = "windows";
    box.sessions[0].canonicalCwd = "c:\\docs\\live"; box.sessions[1].canonicalCwd = "C:\\Docs\\Saved"; box.sessions[2].canonicalCwd = "C:\\Docs\\Live";
    state.setLocal(box, QDateTime::currentMSecsSinceEpoch());
    auto mac = *state.peer("mac"); mac.metrics["platform"] = "windows"; mac.sessions[0].canonicalCwd = "/remote/Docs";
    state.setPeer(mac, QDateTime::currentMSecsSinceEpoch());
    SessionSearch search(FakeCli::program(script())); search.setScope(state, "all", {}); search.setFolderScope({}, "C:/Docs/Live", false);
    search.setQuery("needle"); QTRY_COMPARE(calls().size(), 2); QTRY_COMPARE(list(search)->count(), 2);
    for (int i = 0; i < list(search)->count(); ++i) QVERIFY(!list(search)->item(i)->data(Qt::AccessibleTextRole).toString().contains("saved body"));
    // The peer's reply may still be on its way: wait for its hit, then check the case.
    search.setFolderScope("mac", "/remote/Docs", false); QTRY_COMPARE(list(search)->count(), 1);
    search.setFolderScope("mac", "/remote/docs", false); QCOMPARE(list(search)->count(), 0);
    box.metrics["platform"] = "linux"; state.setLocal(box, QDateTime::currentMSecsSinceEpoch());
    search.setScope(state, "all", {}); search.setFolderScope({}, "C:/Docs/Live", false); QCOMPARE(list(search)->count(), 0);
    search.setFolderScope({}, "c:\\docs\\live", false); QCOMPARE(list(search)->count(), 1);
}

void TestSessionSearch::staleQueriesCannotReplaceCurrentResults()
{
    configure({}, "alpha", {hit("codex/docs/live", "live-run", "alpha-result", "alpha result")}, 550);
    configure({}, "beta", {hit("codex/docs/live", "live-run", "beta-result", "beta current result")});
    SessionSearch search(FakeCli::program(script())); search.setScope(fleet(), "all", "@local"); search.setQuery("alpha");
    QTRY_COMPARE(calls().size(), 1); search.setQuery("beta");
    bool displayedStale = false;
    connect(list(search)->model(), &QAbstractItemModel::rowsInserted, &search, [&] {
        for (int i = 0; i < list(search)->count(); ++i)
            displayedStale |= list(search)->item(i)->data(Qt::AccessibleTextRole).toString().contains("alpha result");
    });
    QTRY_COMPARE(calls().size(), 2); QTRY_COMPARE(list(search)->count(), 1);
    QVERIFY(list(search)->item(0)->data(Qt::AccessibleTextRole).toString().contains("beta current result"));
    QVERIFY(!displayedStale); QTRY_VERIFY(!status(search)->text().contains("Searching"));
    QCOMPARE(calls()[0].toArray()[1].toString(), "alpha"); QCOMPARE(calls()[1].toArray()[1].toString(), "beta");
    search.setQuery("gamma"); search.setQuery({}); QTest::qWait(400);
    QCOMPARE(list(search)->count(), 0); QCOMPARE(calls().size(), 2);
}
void TestSessionSearch::selectionSurvivesFleetPollsAndExplicitRefresh()
{
    SessionSearch search(FakeCli::program(script())); search.setScope(fleet(), "all", "@local"); search.setQuery("needle");
    QTRY_COMPARE(list(search)->count(), 3);
    QSignalSpy activated(&search, &SessionSearch::resultActivated);
    list(search)->setCurrentRow(1); const auto identity = list(search)->currentItem()->data(Qt::UserRole);
    QCOMPARE(activated.size(), 1); search.setScope(fleet(), "all", "@local");
    QCOMPARE(list(search)->currentItem()->data(Qt::UserRole), identity); QCOMPARE(activated.size(), 1);
    search.findChild<QPushButton *>()->click(); QTRY_COMPARE(calls().size(), 2); QTRY_COMPARE(list(search)->count(), 3);
    QTRY_VERIFY(list(search)->currentItem());
    QCOMPARE(list(search)->currentItem()->data(Qt::UserRole), identity); QCOMPARE(activated.size(), 1);
}
void TestSessionSearch::errorsFollowHostScopeAndRecoverWhenPeerConnects()
{
    SessionSearch search(FakeCli::program(script())); search.setScope(fleet(false), "all", {}); search.setQuery("needle");
    QTRY_COMPARE(list(search)->count(), 3); QVERIFY(status(search)->text().contains("unavailable"));
    search.setScope(fleet(false), "all", "@local");
    QVERIFY(!status(search)->text().contains("unavailable")); QVERIFY(!status(search)->toolTip().contains("mac"));
    search.setScope(fleet(true), "all", {});
    QTRY_COMPARE(list(search)->count(), 4);
    QVERIFY(!status(search)->text().contains("unavailable"));
    // A connected peer's SSH transport can still fail: show its error and retry explicitly.
    configure("mac", "failure", {}, 0, "Permission denied (publickey)");
    configure({}, "failure", {hit("codex/docs/live", "live-run", "failure-local", "failure local answer")});
    search.setQuery("failure"); QTRY_COMPARE(list(search)->count(), 1);
    QTRY_VERIFY(status(search)->toolTip().contains("Permission denied"));
    configure("mac", "failure", {hit("kimi/remote", "remote-run", "failure-remote", "failure recovered")});
    QTRY_VERIFY(search.findChild<QPushButton *>()->isEnabled()); search.findChild<QPushButton *>()->click();
    QTRY_COMPARE(list(search)->count(), 2); QVERIFY(!status(search)->text().contains("unavailable"));
}
void TestSessionSearch::newlyAddedPeerIsSearchedForTheCurrentQuery()
{
    auto state = fleet(); SessionSearch search(FakeCli::program(script())); search.setScope(state, "all", {}); search.setQuery("needle");
    QTRY_COMPARE(list(search)->count(), 4); QTRY_VERIFY(!status(search)->text().contains("Searching"));
    auto added = *state.peer("mac"); added.host = "laptop";
    configure("laptop", "needle", {hit("kimi/remote", "remote-run", "laptop-message", "needle laptop answer")});
    state.setPeer(added, 3); search.setScope(state, "all", {});
    QTRY_COMPARE(list(search)->count(), 5);
    QVERIFY(calls().contains(QJsonArray({"laptop", "needle"})));
}
void TestSessionSearch::selectedResultCanBeReopenedWithoutDuplicateActivation()
{
    SessionSearch search(FakeCli::program(script())); search.resize(340, 500); search.show();
    search.setScope(fleet(), "all", "@local"); search.setQuery("needle");
    QTRY_COMPARE(list(search)->count(), 3);
    QSignalSpy activated(&search, &SessionSearch::resultActivated);
    auto *results = list(search);
    QTest::mouseClick(results->viewport(), Qt::LeftButton, {}, results->visualItemRect(results->item(0)).center());
    QCOMPARE(activated.size(), 1);
    // After returning to live activity, the same saved message can be opened again.
    QTest::mouseClick(results->viewport(), Qt::LeftButton, {}, results->visualItemRect(results->item(0)).center());
    QCOMPARE(activated.size(), 2);
    QTest::keyClick(results, Qt::Key_Return); QCOMPARE(activated.size(), 3);
    QTest::mouseClick(results->viewport(), Qt::LeftButton, {}, results->visualItemRect(results->item(1)).center());
    QCOMPARE(activated.size(), 4);
    QTest::keyClick(results, Qt::Key_Down); QCOMPARE(activated.size(), 5);
}
void TestSessionSearch::identifiersMatchWithoutHistoryAndKeepScope()
{
    auto state = fleet(false); auto box = state.local();
    const QString conversation = "11111111-1111-4111-8111-111111111111";
    box.sessions[0].conversationId = conversation;
    box.sessions[0].subagents = {{"child-reference", QJsonObject{{"conversation_id", "child-conversation"}}}};
    box.sessions[1].conversationId = "saved-conversation";
    box.sessions[2].conversationId = "archived-conversation";
    state.setLocal(box, QDateTime::currentMSecsSinceEpoch());
    SessionSearch search(FakeCli::program(script())); search.setScope(state, "all", {});
    for (const auto &query : QStringList{conversation, "4111-8111", "LIVE-RUN", "child-reference", "child-conversation"}) {
        search.setQuery(query); QCOMPARE(list(search)->count(), 1);
        QVERIFY(list(search)->item(0)->data(Qt::AccessibleTextRole).toString().contains("ID:"));
        QVERIFY(list(search)->item(0)->toolTip().contains(conversation));
        QVERIFY(status(search)->text().contains("0 messages"));
    }
    search.setQuery("archive-one"); QCOMPARE(list(search)->count(), 1);
    QVERIFY(list(search)->item(0)->data(Qt::AccessibleTextRole).toString().contains("Archive ID: archive-one"));
    search.setQuery("saved-conversation"); QCOMPARE(list(search)->count(), 1);
    search.setScope(state, "active", {}); QCOMPARE(list(search)->count(), 0);
    search.setScope(state, "all", {"mac"}); QCOMPARE(list(search)->count(), 0);
    box.ok = false; state.setLocal(box, QDateTime::currentMSecsSinceEpoch());
    search.setScope(state, "all", {}); search.setQuery(conversation); QCOMPARE(list(search)->count(), 1);
    box.sessions[0].subagentSource = "hook_profiles"; state.setLocal(box, QDateTime::currentMSecsSinceEpoch());
    search.setScope(state, "all", {}); search.setQuery("child-reference"); QCOMPARE(list(search)->count(), 0);
}
void TestSessionSearch::taskNumbersRemainPublicMessageMatches()
{
    configure({}, "zerus-ab12", {hit("codex/docs/saved", "saved-run", "task-message", "Release task zerus-ab12 is in progress.")});
    SessionSearch search(FakeCli::program(script())); search.setScope(fleet(), "all", {"@local"}); search.setQuery("zerus-ab12");
    QTRY_COMPARE(list(search)->count(), 1);
    QVERIFY(status(search)->text().contains("1 messages"));
    QVERIFY(list(search)->item(0)->data(Qt::AccessibleTextRole).toString().contains("Release task zerus-ab12"));
    QVERIFY(!list(search)->item(0)->toolTip().contains("Session ID: zerus-ab12"));
}
void TestSessionSearch::preview()
{
    const auto directory = qEnvironmentVariable("HGS_PREVIEW_DIR");
    if (directory.isEmpty()) QSKIP("Set HGS_PREVIEW_DIR to render search previews");
    QDir().mkpath(directory);
    configure({}, "recovery", {
        hit("codex/docs/live", "live-run", "live-message", "The recovery path now preserves the active conversation when the machine restarts."),
        hit("codex/docs/saved", "saved-run", "saved-message", "Validated recovery from a saved checkpoint. All session tests pass."),
        hit("codex/docs/live", "archive-run", "archive-message", "Earlier recovery notes: keep the conversation ID and its working directory together.", "archive-one")});
    configure("mac", "recovery", {hit("kimi/remote", "remote-run", "remote-message", "Recovery also works over SSH, without transferring private agent state.")});
    for (bool dark : {true, false}) {
        QWidget canvas; canvas.resize(310, 570); canvas.setObjectName("searchPreviewCanvas");
        canvas.setStyleSheet(QString("QWidget#searchPreviewCanvas { background:%1; }").arg(dark ? "#171e24" : "#f5f7fa"));
        auto *layout = new QVBoxLayout(&canvas); layout->setContentsMargins(0,0,0,0);
        SessionSearch search(FakeCli::program(script())); search.setTheme(dark); layout->addWidget(&search); canvas.show();
        search.setScope(fleet(), "all", {}); search.setQuery("recovery");
        QTRY_COMPARE(list(search)->count(), 4); list(search)->setCurrentRow(0); QTest::qWait(25);
        QVERIFY(canvas.grab().save(directory + (dark ? "/search-dark.png" : "/search-light.png")));
    }
}
QTEST_MAIN(TestSessionSearch)
#include "test_sessionsearch.moc"
