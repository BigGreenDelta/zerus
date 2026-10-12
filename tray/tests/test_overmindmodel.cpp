#include <QtTest>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QUuid>

#include "FakeCli.h"
#include "HgsClient.h"
#include "OvermindModel.h"

namespace {

// A fake `hgs overmind` that serves rev, board and decisions from files next to
// it and records every call (argv and stdin). A write moves the cursor, unless
// refuse.json holds a refusal to print; `broken` makes every call fail without one.
const QByteArray kFixture = R"(#!/usr/bin/env python3
import json, os, pathlib, sys
root = pathlib.Path(__file__).parent
args = sys.argv[1:]
data = sys.stdin.read()
with (root / 'calls').open('a') as calls:
    calls.write(json.dumps({'argv': args, 'input': data}) + '\n')
if (root / 'broken').exists():
    print('cannot open the Overmind store', file=sys.stderr)
    sys.exit(2)
if args[0].startswith('@'):
    args = args[1:]
words = args[1:-1] if args[0] == 'overmind' and args[-1] == '--json' else ['?']
cursor = int((root / 'rev').read_text())
def reply(value):
    print(json.dumps(value))
if words == ['rev']:
    reply({'ok': True, 'cursor': cursor})
elif words == ['board']:
    project = json.loads(data)['project']
    board = json.loads((root / ('board-' + project + '.json')).read_text())
    board.update(ok=True, project=project, cursor=cursor)
    reply(board)
elif words == ['decisions']:
    reply({'ok': True, 'cursor': cursor, 'decisions': json.loads((root / 'decisions.json').read_text())})
elif words == ['show']:
    reply({'ok': True, 'task': {'uuid': json.loads(data)['task'], 'prompt': 'Do it'}})
elif (root / 'refuse.json').exists():
    refusal = (root / 'refuse.json').read_text()
    print(refusal)
    print('hgs: ' + json.loads(refusal)['error']['message'], file=sys.stderr)
    sys.exit(1)
else:
    following = root / 'rev.next'
    following.write_text(str(cursor + 1))
    os.replace(following, root / 'rev')
    reply({'ok': True, 'task': {'uuid': json.loads(data).get('task', '')}})
)";

const QStringList kStages{"Backlog", "Queue", "Build", "Your Call", "Done"};
const QStringList kRoles{"backlog", "ready", "active", "needs_human", "done"};

QJsonObject session(const QString &machine, const QString &name)
{
    return {{"machine", machine}, {"name", name}};
}

// Three hundred cards: every fifth is queued, half of those for m1 and half for m2.
QByteArray demoBoard()
{
    QJsonArray stages;
    for (int i = 0; i < kStages.size(); ++i) stages.append(QJsonObject{{"name", kStages[i]}, {"role", kRoles[i]}});
    const QJsonObject workflow{{"version", 1}, {"stages", stages}, {"rules", QJsonArray()}, {"evidence", "git_sha_or_text"}, {"session_wip", 1}};
    QJsonArray tasks;
    for (int i = 0; i < 300; ++i) {
        const int stage = i % kStages.size();
        QJsonObject task{{"uuid", QStringLiteral("t%1").arg(i)}, {"ref", QStringLiteral("om-%1").arg(i)}, {"project", "demo"},
                         {"title", QStringLiteral("Task %1").arg(i)}, {"stage", kStages[stage]}, {"priority", i % 5},
                         {"rank", QStringLiteral("a%1").arg(i)}, {"labels", QJsonArray{"ui"}}, {"flags", QJsonArray()},
                         {"pinned", QJsonValue::Null}, {"assignee", QJsonValue::Null}, {"reviewer", QJsonValue::Null},
                         {"queue_of", QJsonValue::Null}, {"markers", QJsonArray()}};
        if (kRoles[stage] == "ready") {
            const auto queue = session(i % 10 == 1 ? "m1" : "m2", "codex/demo");
            task["pinned"] = queue; task["queue_of"] = queue;
        }
        if (kRoles[stage] == "active") {
            task["assignee"] = session("m1", "codex/demo"); task["markers"] = QJsonArray{"working", "uncertain"};
        }
        if (i == 6) task["pinned_current"] = "codex/demo-renamed";
        tasks.append(task);
    }
    return QJsonDocument(QJsonObject{{"workflow", workflow}, {"tasks", tasks}}).toJson(QJsonDocument::Compact);
}

struct Fixture {
    QTemporaryDir dir;
    QString script;

    bool setUp()
    {
        if (!dir.isValid()) return false;
        script = dir.filePath("hgs");
        const QJsonArray decisions{QJsonObject{{"task", "t3"}, {"ref", "om-3"}, {"project", "demo"}, {"title", "Pick a database"},
            {"stage", "Your Call"}, {"question", "Postgres or SQLite?"}, {"assignee", session("m1", "codex/demo")},
            {"signature", "5f1e"}}};
        return FakeCli::install(script, kFixture) && put("rev", "1") && put("board-demo.json", demoBoard())
            && put("board-other.json", R"({"workflow":{"stages":[]},"tasks":[]})")
            && put("decisions.json", QJsonDocument(decisions).toJson(QJsonDocument::Compact));
    }
    bool put(const QString &name, const QByteArray &bytes) { return FakeCli::replace(dir.filePath(name), bytes); }
    bool remove(const QString &name) { return QFile::remove(dir.filePath(name)); }

    // Recorded calls; a line a running fixture is still writing is skipped.
    QList<QJsonObject> calls() const
    {
        QFile file(dir.filePath("calls"));
        QList<QJsonObject> out;
        if (!file.open(QIODevice::ReadOnly)) return out;
        for (const auto &line : file.readAll().split('\n')) {
            const auto document = QJsonDocument::fromJson(line);
            if (document.isObject()) out.append(document.object());
        }
        return out;
    }
    // The overmind command words of a call, without @host and --json.
    static QString command(const QJsonObject &call)
    {
        auto argv = call["argv"].toVariant().toStringList();
        if (!argv.isEmpty() && argv.first().startsWith('@')) argv.removeFirst();
        return argv.mid(1, argv.size() - 2).join(' ');
    }
    static QJsonObject input(const QJsonObject &call) { return QJsonDocument::fromJson(call["input"].toString().toUtf8()).object(); }
    int count(const QString &words, const QString &project = {}) const
    {
        int n = 0;
        for (const auto &call : calls())
            if (command(call) == words && (project.isEmpty() || input(call)["project"].toString() == project)) ++n;
        return n;
    }
    QJsonObject last(const QString &words) const
    {
        QJsonObject found;
        for (const auto &call : calls()) if (command(call) == words) found = call;
        return found;
    }
};

}

class TestOvermindModel : public QObject {
    Q_OBJECT
private slots:
    void requestOvermindParsesRepliesAndRefusals();
    void reloadsOnlyWhenRevChanges();
    void activeSwitchesThePollInterval();
    void refusalsReachWriteFailed();
    void writeCarriesARequestId();
    void boardsDecisionsQueuesAndHost();
};

void TestOvermindModel::requestOvermindParsesRepliesAndRefusals()
{
    Fixture f; QVERIFY(f.setUp());
    HgsClient client(FakeCli::program(f.script));
    QSignalSpy ready(&client, &HgsClient::overmindReady), failed(&client, &HgsClient::overmindFailed);

    // Without a request the CLI still gets {} and a closed stdin: `rev --json` reads it.
    const quint64 rev = client.requestOvermind({}, {"rev"});
    QTRY_COMPARE(ready.size(), 1);
    QCOMPARE(ready[0][0].toULongLong(), rev); QVERIFY(ready[0][1].toString().isEmpty());
    QCOMPARE(ready[0][2].toJsonObject()["cursor"].toInteger(), qint64(1));
    const quint64 board = client.requestOvermind("mac", {"board"}, {{"project", "demo"}});
    QVERIFY(board != rev);
    QTRY_COMPARE(ready.size(), 2);
    QCOMPARE(ready[1][0].toULongLong(), board); QCOMPARE(ready[1][1].toString(), QString("mac"));
    QCOMPARE(ready[1][2].toJsonObject()["tasks"].toArray().size(), 300);
    auto calls = f.calls(); QCOMPARE(calls.size(), 2);
    QCOMPARE(calls[0]["argv"].toArray(), (QJsonArray{"overmind", "rev", "--json"}));
    QCOMPARE(calls[0]["input"].toString(), QString("{}"));
    QCOMPARE(calls[1]["argv"].toArray(), (QJsonArray{"@mac", "overmind", "board", "--json"}));
    QCOMPARE(calls[1]["input"].toString(), QString(R"({"project":"demo"})"));

    // A refusal is JSON on stdout with exit 1: the failure carries the parsed error.
    QVERIFY(f.put("refuse.json", R"({"ok":false,"error":{"code":"gate_refused","message":"Review needs evidence","field":"evidence","state":"missing"}})"));
    const quint64 move = client.requestOvermind({}, {"move"}, {{"task", "t1"}, {"to", "Review"}});
    QTRY_COMPARE(failed.size(), 1); QCOMPARE(ready.size(), 2);
    QCOMPARE(failed[0][0].toULongLong(), move); QVERIFY(failed[0][1].toString().isEmpty());
    const auto refusal = failed[0][2].toJsonObject();
    QCOMPARE(refusal["code"].toString(), QString("gate_refused"));
    QCOMPARE(refusal["field"].toString(), QString("evidence"));
    QCOMPARE(refusal["state"].toString(), QString("missing"));
    QCOMPARE(refusal["message"].toString(), QString("Review needs evidence"));
    QCOMPARE(failed[0][3].toString(), QString("Review needs evidence"));

    // A failure without a refusal has an empty error and the CLI's stderr.
    QVERIFY(f.put("broken", ""));
    client.requestOvermind({}, {"rev"});
    QTRY_COMPARE(failed.size(), 2);
    QVERIFY(failed[1][2].toJsonObject().isEmpty());
    QVERIFY(failed[1][3].toString().contains("cannot open the Overmind store"));
}

void TestOvermindModel::reloadsOnlyWhenRevChanges()
{
    Fixture f; QVERIFY(f.setUp());
    HgsClient client(FakeCli::program(f.script));
    OvermindModel model(&client);
    model.setIntervals(50, 50);
    QSignalSpy changed(&model, &OvermindModel::boardChanged), decided(&model, &OvermindModel::decisionsChanged);
    model.watchProject("demo");
    QTRY_VERIFY(model.board("demo").loaded);
    QCOMPARE(model.board("demo").tasks.size(), 300);

    // Many polls of an unchanged cursor: one board read, one decisions read.
    QTRY_VERIFY_WITH_TIMEOUT(f.count("rev") >= 8, 10000);
    QCOMPARE(f.count("board", "demo"), 1); QCOMPARE(f.count("decisions"), 1);
    QCOMPARE(changed.size(), 1); QCOMPARE(changed[0][0].toString(), QString("demo"));
    QCOMPARE(decided.size(), 1);
    QCOMPARE(f.count("board", "other"), 0);
    QCOMPARE(model.board("demo").cursor, qint64(1)); QCOMPARE(model.cursor(), qint64(1));

    // A new cursor reloads the watched board and the decisions once.
    auto board = QJsonDocument::fromJson(demoBoard()).object();
    auto tasks = board["tasks"].toArray();
    auto renamed = tasks[0].toObject(); renamed["title"] = "Renamed"; tasks[0] = renamed;
    board["tasks"] = tasks;
    QVERIFY(f.put("board-demo.json", QJsonDocument(board).toJson(QJsonDocument::Compact)));
    QVERIFY(f.put("decisions.json", "[]"));
    QVERIFY(f.put("rev", "2"));
    QTRY_COMPARE(f.count("board", "demo"), 2); QTRY_COMPARE(f.count("decisions"), 2);
    int polls = f.count("rev");
    QTRY_VERIFY_WITH_TIMEOUT(f.count("rev") >= polls + 6, 10000);
    QCOMPARE(f.count("board", "demo"), 2); QCOMPARE(f.count("decisions"), 2);
    QCOMPARE(changed.size(), 2); QCOMPARE(decided.size(), 2);
    QCOMPARE(model.board("demo").cursor, qint64(2));
    QCOMPARE(model.board("demo").tasks[0].title, QString("Renamed")); QVERIFY(model.decisions().isEmpty());

    // A change elsewhere (another project) is read once more, but nothing on
    // the board changed, so views are not rebuilt.
    QVERIFY(f.put("rev", "3"));
    QTRY_COMPARE(f.count("board", "demo"), 3); QTRY_COMPARE(f.count("decisions"), 3);
    QTRY_COMPARE(model.board("demo").cursor, qint64(3));
    polls = f.count("rev");
    QTRY_VERIFY_WITH_TIMEOUT(f.count("rev") >= polls + 4, 10000);
    QCOMPARE(f.count("board", "demo"), 3); QCOMPARE(f.count("decisions"), 3);
    QCOMPARE(changed.size(), 2); QCOMPARE(decided.size(), 2);
    QCOMPARE(f.count("board", "other"), 0);
}

void TestOvermindModel::activeSwitchesThePollInterval()
{
    QCOMPARE(OvermindModel::ActiveIntervalMs, 2000); QCOMPARE(OvermindModel::IdleIntervalMs, 10000);
    Fixture f; QVERIFY(f.setUp());
    HgsClient client(FakeCli::program(f.script));
    OvermindModel model(&client);
    model.setIntervals(50, 60000);
    QVERIFY(!model.isActive());
    QTRY_COMPARE(f.count("rev"), 1);    // the first poll
    QTest::qWait(400); QCOMPARE(f.count("rev"), 1);
    model.setActive(true); QVERIFY(model.isActive());
    QTRY_VERIFY_WITH_TIMEOUT(f.count("rev") >= 5, 10000);
    model.setActive(false);
    QTest::qWait(200);
    const int polls = f.count("rev");
    QTest::qWait(400); QCOMPARE(f.count("rev"), polls);
}

void TestOvermindModel::refusalsReachWriteFailed()
{
    Fixture f; QVERIFY(f.setUp());
    HgsClient client(FakeCli::program(f.script));
    OvermindModel model(&client);
    model.setIntervals(50, 50);
    model.watchProject("demo");
    QTRY_VERIFY(model.board("demo").loaded);
    QVERIFY(f.put("refuse.json", R"({"ok":false,"error":{"code":"gate_refused","message":"Review needs evidence","field":"evidence","state":"missing"}})"));
    QSignalSpy failed(&model, &OvermindModel::writeFailed);
    bool called = false, succeeded = true;
    QJsonObject given;
    model.write("move", {{"task", "t1"}, {"to", "Review"}}, [&](bool ok, const QJsonObject &, const QJsonObject &refusal) {
        called = true; succeeded = ok; given = refusal;
    });
    QTRY_COMPARE(failed.size(), 1);
    QCOMPARE(failed[0][0].toString(), QString("move"));
    const auto refusal = failed[0][1].toJsonObject();
    QCOMPARE(refusal["code"].toString(), QString("gate_refused"));
    QCOMPARE(refusal["field"].toString(), QString("evidence"));
    QCOMPARE(refusal["state"].toString(), QString("missing"));
    QCOMPARE(refusal["message"].toString(), QString("Review needs evidence"));
    QVERIFY(called); QVERIFY(!succeeded); QCOMPARE(given, refusal);
    QTest::qWait(300);
    QCOMPARE(f.count("board", "demo"), 1);   // a refused write changes nothing

    // A failure without a refusal still reaches writeFailed with a message.
    QVERIFY(f.remove("refuse.json")); QVERIFY(f.put("broken", ""));
    QSignalSpy errors(&model, &OvermindModel::errorChanged);
    model.write("done", {{"task", "t1"}});
    QTRY_COMPARE(failed.size(), 2);
    QCOMPARE(failed[1][0].toString(), QString("done"));
    QVERIFY(!failed[1][1].toJsonObject().contains("code"));
    QVERIFY(failed[1][1].toJsonObject()["message"].toString().contains("cannot open the Overmind store"));
    // Polls fail too: the model reports it and keeps the board it has.
    QTRY_VERIFY(model.error().contains("cannot open the Overmind store"));
    QVERIFY(errors.size() >= 1);
    QCOMPARE(model.board("demo").tasks.size(), 300);
    QVERIFY(f.remove("broken"));
    QTRY_VERIFY(model.error().isEmpty());
}

void TestOvermindModel::writeCarriesARequestId()
{
    Fixture f; QVERIFY(f.setUp());
    HgsClient client(FakeCli::program(f.script));
    OvermindModel model(&client);
    model.setIntervals(60000, 60000);   // only the write itself triggers a check
    model.watchProject("demo");
    QTRY_VERIFY(model.board("demo").loaded); QTRY_VERIFY(model.decisionsLoaded());
    QCOMPARE(f.count("board", "demo"), 1);

    bool succeeded = false;
    QJsonObject reply;
    const quint64 request = model.write("move", {{"task", "t1"}, {"to", "Build"}},
        [&](bool ok, const QJsonObject &result, const QJsonObject &) { succeeded = ok; reply = result; });
    QVERIFY(request != 0);
    QTRY_VERIFY(succeeded);
    QCOMPARE(reply["task"].toObject()["uuid"].toString(), QString("t1"));
    const auto call = f.last("move");
    QCOMPARE(call["argv"].toArray(), (QJsonArray{"overmind", "move", "--json"}));
    const auto sent = Fixture::input(call);
    QCOMPARE(sent["task"].toString(), QString("t1")); QCOMPARE(sent["to"].toString(), QString("Build"));
    const auto first = sent["request_id"].toString();
    QVERIFY(!QUuid::fromString(first).isNull());
    // Success reloads: the write moved the cursor, so the board is read once more.
    QTRY_COMPARE(f.count("board", "demo"), 2);
    QTRY_COMPARE(model.board("demo").cursor, qint64(2));
    QTRY_COMPARE(f.count("decisions"), 2);

    // Every write gets its own ID; one the caller gives (a retry) is kept.
    model.write("move", {{"task", "t2"}, {"to", "Build"}});
    QTRY_COMPARE(f.count("move"), 2);
    const auto second = Fixture::input(f.last("move"))["request_id"].toString();
    QVERIFY(!QUuid::fromString(second).isNull()); QVERIFY(second != first);
    const QString retry = QUuid::createUuid().toString(QUuid::WithoutBraces);
    model.write("workflow set", {{"project", "demo"}, {"workflow", QJsonObject()}, {"request_id", retry}});
    QTRY_COMPARE(f.count("workflow set"), 1);
    QCOMPARE(f.last("workflow set")["argv"].toArray(), (QJsonArray{"overmind", "workflow", "set", "--json"}));
    QCOMPARE(Fixture::input(f.last("workflow set"))["request_id"].toString(), retry);
    QTRY_COMPARE(model.board("demo").cursor, qint64(4));

    // A read has no request_id and changes nothing.
    const int boards = f.count("board", "demo");
    bool shown = false;
    QJsonObject task;
    model.read("show", {{"task", "t1"}}, [&](bool ok, const QJsonObject &result, const QJsonObject &) { shown = ok; task = result["task"].toObject(); });
    QTRY_VERIFY(shown);
    QCOMPARE(task["prompt"].toString(), QString("Do it"));
    QCOMPARE(Fixture::input(f.last("show")), (QJsonObject{{"task", "t1"}}));
    QTest::qWait(200);
    QCOMPARE(f.count("board", "demo"), boards);
}

void TestOvermindModel::boardsDecisionsQueuesAndHost()
{
    Fixture f; QVERIFY(f.setUp());
    HgsClient client(FakeCli::program(f.script));
    OvermindModel model(&client);
    model.setIntervals(50, 50);
    QVERIFY(!model.board("demo").loaded); QVERIFY(model.board("demo").tasks.isEmpty());
    model.watchProject("demo");
    QCOMPARE(model.watchedProjects(), QStringList{"demo"});
    QTRY_VERIFY(model.board("demo").loaded); QTRY_VERIFY(model.decisionsLoaded());

    const auto &board = model.board("demo");
    QCOMPARE(board.project, QString("demo"));
    QVERIFY(board.error.isEmpty());
    QCOMPARE(board.workflow["session_wip"].toInt(), 1);
    QCOMPARE(board.stages.size(), 5);
    QCOMPARE(board.stages[3].name, QString("Your Call")); QCOMPARE(board.stages[3].role, QString("needs_human"));
    QCOMPARE(board.role("Queue"), QString("ready")); QVERIFY(board.role("Icebox").isEmpty());
    const auto &first = board.tasks[0];
    QCOMPARE(first.uuid, QString("t0")); QCOMPARE(first.ref, QString("om-0")); QCOMPARE(first.title, QString("Task 0"));
    QCOMPARE(first.stage, QString("Backlog")); QCOMPARE(first.priority, 0); QCOMPARE(first.rank, QString("a0"));
    QCOMPARE(first.labels, QStringList{"ui"}); QVERIFY(first.queueOf.isEmpty()); QVERIFY(first.assignee.isEmpty());
    QCOMPARE(first.json["project"].toString(), QString("demo"));
    const auto &queued = board.tasks[1];
    QCOMPARE(queued.queueOf.machine, QString("m1")); QCOMPARE(queued.queueOf.name, QString("codex/demo"));
    QCOMPARE(queued.pinned.name, QString("codex/demo"));
    const auto &working = board.tasks[2];
    QCOMPARE(working.markers, (QStringList{"working", "uncertain"}));
    QCOMPARE(working.assignee.machine, QString("m1"));
    QCOMPARE(board.tasks[6].pinnedCurrent, QString("codex/demo-renamed"));
    QVERIFY(board.tasks[1].pinnedCurrent.isEmpty());

    QCOMPARE(model.queueCount("m1", "codex/demo"), 30);
    QCOMPARE(model.queueCount("m2", "codex/demo"), 30);
    QCOMPARE(model.queueCount({}, "codex/demo"), 60);   // any machine
    QCOMPARE(model.queueCount("m1", "claude/demo"), 0);

    QCOMPARE(model.decisions().size(), 1);
    const auto &decision = model.decisions()[0];
    QCOMPARE(decision.task, QString("t3")); QCOMPARE(decision.ref, QString("om-3"));
    QCOMPARE(decision.project, QString("demo")); QCOMPARE(decision.title, QString("Pick a database"));
    QCOMPARE(decision.stage, QString("Your Call")); QCOMPARE(decision.question, QString("Postgres or SQLite?"));
    QCOMPARE(decision.assignee.machine, QString("m1")); QCOMPARE(decision.assignee.name, QString("codex/demo"));
    QCOMPARE(decision.signature, QString("5f1e"));

    // Another host clears what came from the previous one and asks it with @host.
    model.setIntervals(60000, 60000);
    QTest::qWait(300);   // local polls already started have recorded their calls
    const int before = f.calls().size();
    QSignalSpy changed(&model, &OvermindModel::boardChanged), decided(&model, &OvermindModel::decisionsChanged);
    model.setHost("mac");
    QCOMPARE(model.host(), QString("mac"));
    QVERIFY(!model.board("demo").loaded); QVERIFY(model.decisions().isEmpty()); QVERIFY(!model.decisionsLoaded());
    QCOMPARE(model.queueCount({}, "codex/demo"), 0);
    QVERIFY(changed.size() >= 1); QVERIFY(decided.size() >= 1);
    QTRY_VERIFY(model.board("demo").loaded); QTRY_VERIFY(model.decisionsLoaded());
    const auto calls = f.calls();
    QVERIFY(calls.size() > before);
    for (qsizetype i = before; i < calls.size(); ++i) QCOMPARE(calls[i]["argv"].toArray().first().toString(), QString("@mac"));

    // An unwatched project is dropped and no longer read.
    model.unwatchProject("demo");
    QVERIFY(model.watchedProjects().isEmpty()); QVERIFY(!model.board("demo").loaded);
    const int boards = f.count("board", "demo"), decisions = f.count("decisions");
    QVERIFY(f.put("rev", "2"));
    model.refresh();
    QTRY_COMPARE(f.count("decisions"), decisions + 1);
    QTest::qWait(300);
    QCOMPARE(f.count("board", "demo"), boards);
}

QTEST_GUILESS_MAIN(TestOvermindModel)
#include "test_overmindmodel.moc"
