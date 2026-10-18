#include "TaskBoard.h"
#include <QJsonArray>
#include <QTest>

namespace {
constexpr qint64 Day = 24LL * 60 * 60 * 1000, Now = 100 * Day;
QJsonObject session(const QString &name, const QJsonValue &host = QString()) {
    return {{"kind", "session"}, {"machine", "m1"}, {"session", name}, {"host", host}};
}
QJsonObject task(const QString &id, const QString &stage, const QJsonObject &extra = {}) {
    QJsonObject t{{"uuid", "uuid-" + id}, {"id", id}, {"ref", "orchard/" + id}, {"text", "Task " + id}, {"stage", stage},
                  {"summary", ""}, {"cancel_reason", ""}, {"cancelled_at", QJsonValue()}, {"created_at", 1}, {"stage_at", Now - Day},
                  {"writable", true}, {"author", QJsonObject{{"kind", "person"}}}, {"target", QJsonObject{{"kind", "any"}}},
                  {"session", QJsonValue()}, {"rank", "V"}};
    for (auto it = extra.begin(); it != extra.end(); ++it) t[it.key()] = it.value();
    return t;
}
TaskBoard board() {
    QJsonObject sent{{"machine", "m1"}, {"session", "codex/api"}, {"host", ""}};
    QJsonObject remote{{"machine", "m2"}, {"session", "codex/web"}, {"host", "orchard-mac"}};
    return TaskBoard::fromListing({{"project", QJsonObject{{"id", "project-1"}, {"name", "orchard"}}},
        {"damaged", QJsonArray{QJsonObject{{"uuid", "bad"}, {"error", "not loro"}}}},
        {"tasks", QJsonArray{
            task("T-r2", "review", {{"session", sent}, {"stage_at", Now - 2 * Day}, {"summary", "Fixed."}}),
            task("T-r1", "review", {{"session", remote}, {"stage_at", Now - 3 * Day}}),
            task("T-a1", "active", {{"session", sent}}),
            task("T-t1", "todo", {{"target", session("codex/api")}}),
            task("T-t2", "todo"),
            task("T-t3", "todo", {{"target", session("codex/web", "orchard-mac")}, {"author", QJsonObject{{"kind", "agent"}, {"machine", "m2"}, {"session", "codex/web"}}}}),
            task("T-b1", "backlog", {{"target", session("codex/ghost", QJsonValue())}}),
            task("T-d1", "done", {{"session", sent}, {"stage_at", Now - 2 * Day}}),
            task("T-d2", "done", {{"session", sent}, {"stage_at", Now - Day}}),
            task("T-d3", "done", {{"session", remote}, {"stage_at", Now - 9 * Day}}),
        }}});
}
QStringList ids(const QList<TaskItem> &rows) { QStringList out; for (const auto &row : rows) out << row.id; return out; }
}

class TestTaskBoard : public QObject {
    Q_OBJECT
private slots:
    void readsTheListing();
    void rowsFollowEachStageOrder();
    void theOpenSessionsTasksAndTheFilter();
    void actionsFollowTheStage();
};

void TestTaskBoard::readsTheListing() {
    const auto b = board();
    QCOMPARE(b.projectId, QString("project-1")); QCOMPARE(b.projectName, QString("orchard"));
    QCOMPARE(b.tasks.size(), 10); QCOMPARE(b.damaged, 1);
    const auto byId = [&](const QString &id) { for (const auto &t : b.tasks) if (t.id == id) return t; return TaskItem{}; };
    QCOMPARE(byId("T-t1").targetSession, QString("codex/api")); QCOMPARE(byId("T-t1").targetHost, QString());
    QVERIFY(byId("T-t2").targetSession.isEmpty());
    QCOMPARE(byId("T-t3").targetHost, QString("orchard-mac")); QCOMPARE(byId("T-t3").author, QString("codex/web"));
    QVERIFY(!byId("T-b1").targetKnown);
    QCOMPARE(byId("T-r2").sessionName, QString("codex/api")); QCOMPARE(byId("T-r2").summary, QString("Fixed."));
    QCOMPARE(byId("T-r1").sessionHost, QString("orchard-mac"));
    // To do keeps the project's queue positions.
    QCOMPARE(byId("T-t1").queue, 1); QCOMPARE(byId("T-t3").queue, 3); QCOMPARE(byId("T-b1").queue, 0);
}

void TestTaskBoard::rowsFollowEachStageOrder() {
    const auto b = board(); const TaskBoard::View all{"", "codex/api", false, Now};
    QCOMPARE(ids(b.rows("review", all)), QStringList({"T-r1", "T-r2"}));       // oldest report first
    QCOMPARE(ids(b.rows("todo", all)), QStringList({"T-t1", "T-t2", "T-t3"})); // the queue
    // Done shows the last 7 days, newest first; older ones come apart.
    QCOMPARE(ids(b.rows("done", all)), QStringList({"T-d2", "T-d1"}));
    QCOMPARE(ids(b.rows("done", all, true)), QStringList({"T-d3"}));
    QCOMPARE(b.count("done", all), 2); QCOMPARE(b.count("active", all), 1);
}

void TestTaskBoard::theOpenSessionsTasksAndTheFilter() {
    const auto b = board(); QHash<QString, TaskItem> t; for (const auto &item : b.tasks) t.insert(item.id, item);
    QVERIFY(TaskBoard::belongsTo(t["T-t1"], "", "codex/api"));          // pinned to it
    QVERIFY(!TaskBoard::belongsTo(t["T-t2"], "", "codex/api"));         // Any session
    QVERIFY(TaskBoard::belongsTo(t["T-r2"], "", "codex/api"));          // sent to it
    QVERIFY(!TaskBoard::belongsTo(t["T-r1"], "", "codex/web"));         // same name, another computer
    QVERIFY(TaskBoard::belongsTo(t["T-r1"], "orchard-mac", "codex/web"));
    QVERIFY(!TaskBoard::belongsTo(t["T-b1"], "", "codex/ghost"));       // unknown computer
    const TaskBoard::View only{"", "codex/api", true, Now};
    QCOMPARE(ids(b.rows("todo", only)), QStringList({"T-t1", "T-t2"}));
    QCOMPARE(ids(b.rows("review", only)), QStringList({"T-r2"}));
    QCOMPARE(b.count("backlog", only), 0);
    // Queue numbers stay project-wide while others are hidden.
    QCOMPARE(b.rows("todo", only).at(1).queue, 2);
}

void TestTaskBoard::actionsFollowTheStage() {
    using A = TaskAction;
    QCOMPARE(TaskBoard::actions("todo"), QList<A>({A::Edit, A::ToBacklog, A::Top, A::SendNow}));
    QCOMPARE(TaskBoard::actions("backlog"), QList<A>({A::Edit, A::ToTodo, A::SendNow}));
    QCOMPARE(TaskBoard::actions("active"), QList<A>({A::Cancel}));
    QCOMPARE(TaskBoard::actions("review"), QList<A>({A::BackToTodo, A::Accept}));
    QCOMPARE(TaskBoard::actions("done"), QList<A>({A::BackToTodo}));
    QCOMPARE(TaskBoard::stages(), QStringList({"review", "active", "todo", "backlog", "done"}));
    QCOMPARE(TaskBoard::stageLabel("active"), QString("In progress"));
    QCOMPARE(TaskBoard::actionLabel(A::Top), QString("Move to the top of To do"));
}

QTEST_MAIN(TestTaskBoard)
#include "test_taskboard.moc"
