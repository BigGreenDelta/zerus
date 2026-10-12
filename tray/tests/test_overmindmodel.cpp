#include <QtTest>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QUuid>

#include <algorithm>
#include <functional>

#include "FakeCli.h"
#include "HgsClient.h"
#include "OvermindModel.h"

namespace {

// A fake `hgs overmind` that serves rev, status, board, decisions and show from
// files next to it. Every call is recorded when it starts (argv and stdin) and,
// with "done", when it ends. sleep.json delays calls ({"<host>:<command>": s},
// host empty for local). A write moves the cursor, unless refuse.json holds a
// refusal to print. `broken` makes the commands it lists (comma-separated; all
// when empty) fail without one. board-<project>.<host>.json overrides a board
// for one host.
const QByteArray kFixture = R"(#!/usr/bin/env python3
import json, os, pathlib, sys, time
root = pathlib.Path(__file__).parent
args = sys.argv[1:]
data = sys.stdin.read()
def record(**extra):
    with (root / 'calls').open('a') as calls:
        calls.write(json.dumps(dict({'argv': sys.argv[1:], 'input': data}, **extra)) + '\n')
def read(name, default=None):
    try:
        return (root / name).read_text()
    except FileNotFoundError:
        return default
def finish(code=0):
    sys.stdout.flush()
    record(done=True)
    sys.exit(code)
record()
host = args[0][1:] if args and args[0].startswith('@') else ''
if host:
    args = args[1:]
words = args[1:-1] if args and args[0] == 'overmind' and args[-1] == '--json' else ['?']
command = ' '.join(words)
time.sleep(json.loads(read('sleep.json', '{}')).get(host + ':' + command, 0))
broken = read('broken')
if broken is not None and (not broken.strip() or command in broken.split(',')):
    print('cannot open the Overmind store', file=sys.stderr)
    finish(2)
cursor = int(read('rev'))
def reply(value):
    print(json.dumps(value))
if command == 'rev':
    reply({'ok': True, 'cursor': cursor})
elif command == 'status':
    reply({'ok': True, 'machine': 'm2' if host else 'm1', 'cursor': cursor})
elif command == 'board':
    project = json.loads(data)['project']
    board = json.loads(read('board-' + project + '.' + host + '.json') or read('board-' + project + '.json'))
    board.update(ok=True, project=project, cursor=cursor)
    reply(board)
elif command == 'decisions':
    reply({'ok': True, 'cursor': cursor, 'decisions': json.loads(read('decisions.json'))})
elif command == 'show':
    reply({'ok': True, 'task': {'uuid': json.loads(data)['task'], 'prompt': 'Do it'}})
elif read('refuse.json') is not None:
    refusal = read('refuse.json')
    print(refusal)
    print('hgs: ' + json.loads(refusal)['error']['message'], file=sys.stderr)
    finish(1)
else:
    following = root / 'rev.next'
    following.write_text(str(cursor + 1))
    # Windows cannot rename over a file another process has open for reading.
    for attempt in range(250):
        try:
            os.replace(following, root / 'rev')
            break
        except PermissionError:
            time.sleep(0.02)
    reply({'ok': True, 'task': {'uuid': json.loads(data).get('task', '')}})
finish()
)";

const QStringList kStages{"Backlog", "Queue", "Build", "Your Call", "Done"};
const QStringList kRoles{"backlog", "ready", "active", "needs_human", "done"};

QJsonObject session(const QString &machine, const QString &name)
{
    return {{"machine", machine}, {"name", name}};
}

// Three hundred cards: every fifth is queued, half of those for m1 and half for m2.
QJsonObject demoBoard()
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
    return {{"workflow", workflow}, {"tasks", tasks}};
}

QByteArray compact(const QJsonObject &object) { return QJsonDocument(object).toJson(QJsonDocument::Compact); }

// One card titled `title`, for a board that must differ from the demo board.
QByteArray singleCardBoard(const QString &title)
{
    auto board = demoBoard();
    auto first = board["tasks"].toArray().first().toObject();
    first["title"] = title;
    board["tasks"] = QJsonArray{first};
    return compact(board);
}

const QByteArray kRefusal = R"({"ok":false,"error":{"code":"gate_refused","message":"Review needs evidence","field":"evidence","state":"missing"}})";

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
        return FakeCli::install(script, kFixture) && put("rev", "1") && put("board-demo.json", compact(demoBoard()))
            && put("board-other.json", R"({"workflow":{"stages":[]},"tasks":[]})")
            && put("decisions.json", QJsonDocument(decisions).toJson(QJsonDocument::Compact));
    }
    bool put(const QString &name, const QByteArray &bytes) { return FakeCli::replace(dir.filePath(name), bytes); }
    bool remove(const QString &name) { return QFile::remove(dir.filePath(name)); }

    // Recorded starts and ends; a line a running fixture is still writing is skipped.
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
    QList<QJsonObject> starts() const
    {
        QList<QJsonObject> out;
        for (const auto &call : calls()) if (!call.contains("done")) out.append(call);
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
        for (const auto &call : starts())
            if (command(call) == words && (project.isEmpty() || input(call)["project"].toString() == project)) ++n;
        return n;
    }
    QJsonObject last(const QString &words) const
    {
        QJsonObject found;
        for (const auto &call : starts()) if (command(call) == words) found = call;
        return found;
    }
};

// Waits until a count stops growing for two windows in a row and returns it;
// -1 when it keeps growing. The window must exceed the poll interval in use.
int settled(const std::function<int()> &count, int windowMs = 250)
{
    int last = count();
    for (int quiet = 0, rounds = 0; quiet < 2; ++rounds) {
        if (rounds > 80) return -1;
        QTest::qWait(windowMs);
        const int now = count();
        quiet = now == last ? quiet + 1 : 0;
        last = now;
    }
    return last;
}

}

class TestOvermindModel : public QObject {
    Q_OBJECT
private slots:
    void requestOvermindParsesRepliesAndRefusals();
    void reloadsOnlyWhenRevChanges();
    void activeSwitchesThePollInterval();
    void refusalsReachWriteFailed();
    void failedReadsWaitForTheNextChange();
    void writeCarriesARequestId();
    void writeDeadlinesDependOnTheCommand();
    void callbacksSkipADestroyedContext();
    void boardsDecisionsQueuesAndHost();
    void lateRepliesFromThePreviousHostAreDropped();
    void aSlowRevLeavesOnePollInFlight();
    void destroyingTheModelWithRequestsInFlightIsSafe();
    void disabledModelNeverCallsTheCli();
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
    const auto calls = f.starts(); QCOMPARE(calls.size(), 2);
    QCOMPARE(calls[0]["argv"].toArray(), (QJsonArray{"overmind", "rev", "--json"}));
    QCOMPARE(calls[0]["input"].toString(), QString("{}"));
    QCOMPARE(calls[1]["argv"].toArray(), (QJsonArray{"@mac", "overmind", "board", "--json"}));
    QCOMPARE(calls[1]["input"].toString(), QString(R"({"project":"demo"})"));

    // A deadline: the outcome is unknown.
    QVERIFY(f.put("sleep.json", R"({":send": 5})"));
    const quint64 slow = client.requestOvermind({}, {"send"}, {{"task", "t1"}}, 300);
    QTRY_COMPARE(failed.size(), 1);
    QCOMPARE(failed[0][0].toULongLong(), slow);
    QCOMPARE(failed[0][2].toJsonObject()["code"].toString(), QString("timed_out"));
    QVERIFY(failed[0][2].toJsonObject()["uncertain"].toBool());
    QVERIFY(!failed[0][3].toString().isEmpty());
    QCOMPARE(failed[0][2].toJsonObject()["message"].toString(), failed[0][3].toString());

    // A refusal is JSON on stdout with exit 1: the failure carries the parsed error.
    QVERIFY(f.put("refuse.json", kRefusal));
    const quint64 move = client.requestOvermind({}, {"move"}, {{"task", "t1"}, {"to", "Review"}});
    QTRY_COMPARE(failed.size(), 2); QCOMPARE(ready.size(), 2);
    QCOMPARE(failed[1][0].toULongLong(), move); QVERIFY(failed[1][1].toString().isEmpty());
    const auto refusal = failed[1][2].toJsonObject();
    QCOMPARE(refusal["code"].toString(), QString("gate_refused"));
    QCOMPARE(refusal["field"].toString(), QString("evidence"));
    QCOMPARE(refusal["state"].toString(), QString("missing"));
    QCOMPARE(refusal["message"].toString(), QString("Review needs evidence"));
    QVERIFY(!refusal.contains("uncertain"));
    QCOMPARE(failed[1][3].toString(), QString("Review needs evidence"));

    // A failure without a refusal has an empty error and the CLI's stderr.
    QVERIFY(f.put("broken", ""));
    client.requestOvermind({}, {"rev"});
    QTRY_COMPARE(failed.size(), 3);
    QVERIFY(failed[2][2].toJsonObject().isEmpty());
    QVERIFY(failed[2][3].toString().contains("cannot open the Overmind store"));
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
    QCOMPARE(f.count("board", "demo"), 1); QCOMPARE(f.count("decisions"), 1); QCOMPARE(f.count("status"), 1);
    QCOMPARE(changed.size(), 1); QCOMPARE(changed[0][0].toString(), QString("demo"));
    QCOMPARE(decided.size(), 1);
    QCOMPARE(f.count("board", "other"), 0);
    QCOMPARE(model.board("demo").cursor, qint64(1)); QCOMPARE(model.cursor(), qint64(1));

    // A new cursor reloads the watched board and the decisions once.
    auto board = demoBoard();
    auto tasks = board["tasks"].toArray();
    auto renamed = tasks[0].toObject(); renamed["title"] = "Renamed"; tasks[0] = renamed;
    board["tasks"] = tasks;
    QVERIFY(f.put("board-demo.json", compact(board)));
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
    QCOMPARE(f.count("status"), 1);   // the machine ID is read once
}

void TestOvermindModel::activeSwitchesThePollInterval()
{
    QCOMPARE(OvermindModel::ActiveIntervalMs, 2000); QCOMPARE(OvermindModel::IdleIntervalMs, 10000);
    Fixture f; QVERIFY(f.setUp());
    HgsClient client(FakeCli::program(f.script));
    OvermindModel model(&client);
    model.setEnabled(true);   // Windows starts disabled
    model.setIntervals(50, 60000);
    QVERIFY(!model.isActive());
    const auto revs = [&] { return f.count("rev"); };
    QCOMPARE(settled(revs), 1);   // the first poll, then nothing while idle
    model.setActive(true); QVERIFY(model.isActive());
    QTRY_VERIFY_WITH_TIMEOUT(f.count("rev") >= 5, 10000);
    model.setActive(false);
    QVERIFY(settled(revs) >= 5);  // no poll for two windows of five active intervals
}

void TestOvermindModel::refusalsReachWriteFailed()
{
    Fixture f; QVERIFY(f.setUp());
    HgsClient client(FakeCli::program(f.script));
    OvermindModel model(&client);
    model.setEnabled(true);
    model.setIntervals(50, 50);
    model.watchProject("demo");
    QTRY_VERIFY(model.board("demo").loaded);
    QVERIFY(f.put("refuse.json", kRefusal));
    QSignalSpy failed(&model, &OvermindModel::writeFailed);
    bool called = false;
    OvermindModel::Result given;
    given.ok = true;
    model.write("move", {{"task", "t1"}, {"to", "Review"}}, [&](const OvermindModel::Result &result) { called = true; given = result; });
    QTRY_COMPARE(failed.size(), 1);
    QCOMPARE(failed[0][0].toString(), QString("move"));
    const auto refusal = failed[0][1].toJsonObject();
    QCOMPARE(refusal["code"].toString(), QString("gate_refused"));
    QCOMPARE(refusal["field"].toString(), QString("evidence"));
    QCOMPARE(refusal["state"].toString(), QString("missing"));
    QCOMPARE(refusal["message"].toString(), QString("Review needs evidence"));
    const auto sent = Fixture::input(f.last("move"))["request_id"].toString();
    QCOMPARE(failed[0][2].toString(), sent);
    QVERIFY(called); QVERIFY(!given.ok); QCOMPARE(given.refusal, refusal); QCOMPARE(given.requestId, sent);
    QVERIFY(!given.uncertain());
    QCOMPARE(settled([&] { return f.count("board", "demo"); }), 1);   // a refused write changes nothing

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

void TestOvermindModel::failedReadsWaitForTheNextChange()
{
    Fixture f; QVERIFY(f.setUp());
    HgsClient client(FakeCli::program(f.script));
    OvermindModel model(&client);
    model.setEnabled(true);
    model.setIntervals(50, 50);
    model.watchProject("demo");
    QTRY_VERIFY(model.board("demo").loaded); QTRY_VERIFY(model.decisionsLoaded());
    QSignalSpy changed(&model, &OvermindModel::boardChanged), decided(&model, &OvermindModel::decisionsChanged);

    QVERIFY(f.put("broken", "board,decisions"));
    QVERIFY(f.put("rev", "2"));
    QTRY_VERIFY(model.board("demo").error.contains("cannot open the Overmind store"));
    QTRY_VERIFY(model.decisionsError().contains("cannot open the Overmind store"));
    QCOMPARE(changed.size(), 1); QCOMPARE(decided.size(), 1);
    QCOMPARE(model.board("demo").tasks.size(), 300); QCOMPARE(model.decisions().size(), 1);
    QVERIFY(model.error().isEmpty());   // rev itself works

    // Polls go on, but a failed read waits for the next change, even once the
    // store works again.
    int polls = f.count("rev");
    QTRY_VERIFY_WITH_TIMEOUT(f.count("rev") >= polls + 6, 10000);
    QCOMPARE(f.count("board", "demo"), 2); QCOMPARE(f.count("decisions"), 2);
    QVERIFY(f.remove("broken"));
    polls = f.count("rev");
    QTRY_VERIFY_WITH_TIMEOUT(f.count("rev") >= polls + 6, 10000);
    QCOMPARE(f.count("board", "demo"), 2); QCOMPARE(f.count("decisions"), 2);
    QVERIFY(f.put("rev", "3"));
    QTRY_VERIFY(model.board("demo").error.isEmpty()); QTRY_VERIFY(model.decisionsError().isEmpty());
    QCOMPARE(f.count("board", "demo"), 3); QCOMPARE(f.count("decisions"), 3);
    QCOMPARE(changed.size(), 2); QCOMPARE(decided.size(), 2);

    // Watching the board again retries a failed read at once.
    QVERIFY(f.put("broken", "board"));
    QVERIFY(f.put("rev", "4"));
    QTRY_VERIFY(!model.board("demo").error.isEmpty());
    QVERIFY(f.remove("broken"));
    model.watchProject("demo");
    QTRY_VERIFY(model.board("demo").error.isEmpty());
    QCOMPARE(f.count("board", "demo"), 5);
}

void TestOvermindModel::writeCarriesARequestId()
{
    Fixture f; QVERIFY(f.setUp());
    HgsClient client(FakeCli::program(f.script));
    OvermindModel model(&client);
    model.setEnabled(true);
    model.setIntervals(60000, 60000);   // only the write itself triggers a check
    model.watchProject("demo");
    QTRY_VERIFY(model.board("demo").loaded); QTRY_VERIFY(model.decisionsLoaded());
    QCOMPARE(f.count("board", "demo"), 1);

    OvermindModel::Result result;
    bool done = false;
    const quint64 request = model.write("move", {{"task", "t1"}, {"to", "Build"}},
        [&](const OvermindModel::Result &r) { result = r; done = true; });
    QVERIFY(request != 0);
    QTRY_VERIFY(done);
    QVERIFY(result.ok); QVERIFY(result.refusal.isEmpty());
    QCOMPARE(result.reply["task"].toObject()["uuid"].toString(), QString("t1"));
    const auto call = f.last("move");
    QCOMPARE(call["argv"].toArray(), (QJsonArray{"overmind", "move", "--json"}));
    const auto sent = Fixture::input(call);
    QCOMPARE(sent["task"].toString(), QString("t1")); QCOMPARE(sent["to"].toString(), QString("Build"));
    const auto first = sent["request_id"].toString();
    QVERIFY(!QUuid::fromString(first).isNull());
    QCOMPARE(result.requestId, first);
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
    QString replayed;
    model.write("workflow set", {{"project", "demo"}, {"workflow", QJsonObject()}, {"request_id", retry}},
        [&](const OvermindModel::Result &r) { replayed = r.requestId; });
    QTRY_COMPARE(f.count("workflow set"), 1);
    QCOMPARE(f.last("workflow set")["argv"].toArray(), (QJsonArray{"overmind", "workflow", "set", "--json"}));
    QCOMPARE(Fixture::input(f.last("workflow set"))["request_id"].toString(), retry);
    QTRY_COMPARE(replayed, retry);
    QTRY_COMPARE(model.board("demo").cursor, qint64(4));

    // A read has no request_id and changes nothing.
    const int boards = f.count("board", "demo");
    OvermindModel::Result shown;
    model.read("show", {{"task", "t1"}}, [&](const OvermindModel::Result &r) { shown = r; });
    QTRY_VERIFY(shown.ok);
    QCOMPARE(shown.reply["task"].toObject()["prompt"].toString(), QString("Do it"));
    QVERIFY(shown.requestId.isEmpty());
    QCOMPARE(Fixture::input(f.last("show")), (QJsonObject{{"task", "t1"}}));
    QCOMPARE(settled([&] { return f.count("board", "demo"); }), boards);
}

void TestOvermindModel::writeDeadlinesDependOnTheCommand()
{
    const OvermindModel::Timeouts defaults;
    QCOMPARE(defaults.local, 10000); QCOMPARE(defaults.remote, 30000);
    QCOMPARE(defaults.deliveryLocal, 45000); QCOMPARE(defaults.deliveryRemote, 60000);
    for (const char *command : {"send", "review-send", "rework-send", "answer", "resolve-delivery"})
        QVERIFY2(OvermindModel::delivers(command), command);
    for (const char *command : {"move", "create", "done", "comment", "workflow set", "board"})
        QVERIFY2(!OvermindModel::delivers(command), command);

    Fixture f; QVERIFY(f.setUp());
    HgsClient client(FakeCli::program(f.script));
    OvermindModel model(&client);
    model.setEnabled(true);
    model.setIntervals(60000, 60000);
    model.setTimeouts({400, 400, 6000, 6000});
    QVERIFY(f.put("sleep.json", R"({":send": 1.5, ":move": 1.5})"));
    QSignalSpy failed(&model, &OvermindModel::writeFailed);
    OvermindModel::Result sent, moved;
    bool sentDone = false, movedDone = false;
    model.write("send", {{"task", "t1"}, {"session", session({}, "codex/demo")}},
        [&](const OvermindModel::Result &r) { sent = r; sentDone = true; });
    model.write("move", {{"task", "t2"}, {"to", "Build"}}, [&](const OvermindModel::Result &r) { moved = r; movedDone = true; });

    // An ordinary write is killed at its deadline: unknown outcome, retry with the same ID.
    QTRY_VERIFY_WITH_TIMEOUT(movedDone, 10000);
    QVERIFY(!moved.ok);
    QCOMPARE(moved.refusal["code"].toString(), QString("timed_out"));
    QVERIFY(moved.uncertain()); QVERIFY(!moved.refusal["message"].toString().isEmpty());
    QCOMPARE(moved.requestId, Fixture::input(f.last("move"))["request_id"].toString());
    QCOMPARE(failed.size(), 1); QCOMPARE(failed[0][2].toString(), moved.requestId);
    QVERIFY(failed[0][1].toJsonObject()["uncertain"].toBool());
    // A delivering write has the longer deadline and finishes.
    QTRY_VERIFY_WITH_TIMEOUT(sentDone, 10000);
    QVERIFY(sent.ok);
    QCOMPARE(sent.requestId, Fixture::input(f.last("send"))["request_id"].toString());
    QCOMPARE(failed.size(), 1);
}

void TestOvermindModel::callbacksSkipADestroyedContext()
{
    Fixture f; QVERIFY(f.setUp());
    HgsClient client(FakeCli::program(f.script));
    OvermindModel model(&client);
    model.setEnabled(true);
    model.setIntervals(60000, 60000);
    model.watchProject("demo");
    QTRY_VERIFY(model.board("demo").loaded);
    QVERIFY(f.put("sleep.json", R"({":move": 0.6, ":show": 0.6})"));
    // Connected after the model: by the time the spy sees a reply, the model has handled it.
    QSignalSpy replies(&client, &HgsClient::overmindReady);
    auto *context = new QObject;
    bool called = false;
    model.write("move", {{"task", "t1"}, {"to", "Build"}}, [&](const OvermindModel::Result &) { called = true; }, context);
    model.read("show", {{"task", "t1"}}, [&](const OvermindModel::Result &) { called = true; }, context);
    QTRY_VERIFY(f.count("move") == 1 && f.count("show") == 1);
    delete context;
    // The write still lands and the board is read again; only the callbacks are skipped.
    QTRY_COMPARE_WITH_TIMEOUT(f.count("board", "demo"), 2, 10000);
    QTRY_COMPARE(model.board("demo").cursor, qint64(2));
    QTRY_VERIFY(std::any_of(replies.cbegin(), replies.cend(), [](const QList<QVariant> &reply) {
        return reply[2].toJsonObject()["task"].toObject().contains("prompt");
    }));
    QVERIFY(!called);

    // A living context gets its callback.
    QObject living;
    model.write("move", {{"task", "t2"}, {"to", "Build"}}, [&](const OvermindModel::Result &) { called = true; }, &living);
    QTRY_VERIFY_WITH_TIMEOUT(called, 10000);
}

void TestOvermindModel::boardsDecisionsQueuesAndHost()
{
    Fixture f; QVERIFY(f.setUp());
    HgsClient client(FakeCli::program(f.script));
    OvermindModel model(&client);
    model.setEnabled(true);
    model.setIntervals(50, 50);
    QSignalSpy machines(&model, &OvermindModel::machineChanged);
    QVERIFY(!model.board("demo").loaded); QVERIFY(model.board("demo").tasks.isEmpty());
    model.watchProject("demo");
    QCOMPARE(model.watchedProjects(), QStringList{"demo"});
    QTRY_VERIFY(model.board("demo").loaded); QTRY_VERIFY(model.decisionsLoaded());
    QTRY_COMPARE(model.machine(), QString("m1"));
    QCOMPARE(machines.size(), 1);

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
    QCOMPARE(model.queueCount({}, "codex/demo"), 30);   // this machine: m1
    QCOMPARE(model.queueCount("m1", "claude/demo"), 0);
    QCOMPARE(model.queueCount("m1", {}), 0);
    // One project's board: what the chip and the session row count.
    QCOMPARE(model.queueCount("m1", "codex/demo", "demo"), 30);
    QCOMPARE(model.queueCount({}, "codex/demo", "demo"), 30);
    QCOMPARE(model.queueCount("m1", "codex/demo", "other"), 0);   // a board not watched

    QCOMPARE(model.decisions().size(), 1);
    const auto &decision = model.decisions()[0];
    QCOMPARE(decision.task, QString("t3")); QCOMPARE(decision.ref, QString("om-3"));
    QCOMPARE(decision.project, QString("demo")); QCOMPARE(decision.title, QString("Pick a database"));
    QCOMPARE(decision.stage, QString("Your Call")); QCOMPARE(decision.question, QString("Postgres or SQLite?"));
    QCOMPARE(decision.assignee.machine, QString("m1")); QCOMPARE(decision.assignee.name, QString("codex/demo"));
    QCOMPARE(decision.signature, QString("5f1e"));
    QVERIFY(model.decisionsError().isEmpty());
    // A session's decisions: an empty machine means this one.
    QCOMPARE(model.decisionsFor("m1", "codex/demo").size(), 1);
    QCOMPARE(model.decisionsFor("m1", "codex/demo").first().task, QString("t3"));
    QCOMPARE(model.decisionsFor({}, "codex/demo").size(), 1);   // this machine: m1
    QVERIFY(model.decisionsFor("m2", "codex/demo").isEmpty());
    QVERIFY(model.decisionsFor("m1", "claude/demo").isEmpty());
    QVERIFY(model.decisionsFor("m1", {}).isEmpty());
    QCOMPARE(model.decisionsFor("m1", "codex/demo", "demo").size(), 1);
    QVERIFY(model.decisionsFor("m1", "codex/demo", "other").isEmpty());

    // Another host clears what came from the previous one and asks it with @host.
    model.setIntervals(60000, 60000);
    const int before = settled([&] { return int(f.calls().size()); });
    QVERIFY(before > 0);
    QSignalSpy changed(&model, &OvermindModel::boardChanged), decided(&model, &OvermindModel::decisionsChanged);
    model.setHost("mac");
    QCOMPARE(model.host(), QString("mac")); QVERIFY(model.isEnabled());
    QVERIFY(!model.board("demo").loaded); QVERIFY(model.decisions().isEmpty()); QVERIFY(!model.decisionsLoaded());
    QVERIFY(model.machine().isEmpty()); QCOMPARE(model.queueCount({}, "codex/demo"), 0);
    QVERIFY(changed.size() >= 1); QVERIFY(decided.size() >= 1);
    QTRY_VERIFY(model.board("demo").loaded); QTRY_VERIFY(model.decisionsLoaded());
    QTRY_COMPARE(model.machine(), QString("m2"));
    QCOMPARE(model.queueCount({}, "codex/demo"), 30);   // this machine is now m2
    QVERIFY(model.decisionsFor({}, "codex/demo").isEmpty());
    QCOMPARE(model.decisionsFor("m1", "codex/demo").size(), 1);
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
    QCOMPARE(settled([&] { return int(f.calls().size()); }), int(f.calls().size()));
    QCOMPARE(f.count("board", "demo"), boards);
}

void TestOvermindModel::lateRepliesFromThePreviousHostAreDropped()
{
    Fixture f; QVERIFY(f.setUp());
    QVERIFY(f.put("sleep.json", R"({":board": 1.0, ":decisions": 1.0, ":status": 1.0})"));
    QVERIFY(f.put("board-demo.mac.json", singleCardBoard("Remote")));
    HgsClient client(FakeCli::program(f.script));
    OvermindModel model(&client);
    model.setEnabled(true);
    model.setIntervals(60000, 60000);
    // Connected after the model: by the time the spy sees a reply, the model has handled it.
    QSignalSpy replies(&client, &HgsClient::overmindReady);
    model.watchProject("demo");
    QTRY_COMPARE(f.count("board", "demo"), 1);
    QTRY_COMPARE(f.count("decisions"), 1);
    model.setHost("mac");
    QTRY_VERIFY(model.board("demo").loaded); QTRY_VERIFY(model.decisionsLoaded());
    QTRY_COMPARE(model.machine(), QString("m2"));
    QCOMPARE(model.board("demo").tasks.size(), 1);
    QSignalSpy changed(&model, &OvermindModel::boardChanged), decided(&model, &OvermindModel::decisionsChanged),
        machines(&model, &OvermindModel::machineChanged);

    // The local answers arrive last and change nothing.
    const auto localAnswers = [&] {
        int n = 0;
        for (const auto &reply : replies) {
            const auto object = reply[2].toJsonObject();
            if (reply[1].toString().isEmpty() && (object.contains("tasks") || object.contains("decisions") || object.contains("machine"))) ++n;
        }
        return n;
    };
    QTRY_VERIFY_WITH_TIMEOUT(localAnswers() >= 3, 10000);   // board, decisions, status
    QCOMPARE(model.board("demo").tasks.size(), 1);
    QCOMPARE(model.board("demo").tasks[0].title, QString("Remote"));
    QCOMPARE(model.machine(), QString("m2"));
    QCOMPARE(changed.size(), 0); QCOMPARE(decided.size(), 0); QCOMPARE(machines.size(), 0);
}

void TestOvermindModel::aSlowRevLeavesOnePollInFlight()
{
    Fixture f; QVERIFY(f.setUp());
    QVERIFY(f.put("sleep.json", R"({":rev": 0.4})"));
    HgsClient client(FakeCli::program(f.script));
    OvermindModel model(&client);
    model.setEnabled(true);
    model.setIntervals(20, 20);
    model.setActive(true);
    QTRY_VERIFY_WITH_TIMEOUT(f.count("rev") >= 3, 10000);
    for (int i = 0; i < 5; ++i) model.refresh();
    const int requested = f.count("rev");
    QTRY_VERIFY_WITH_TIMEOUT(f.count("rev") >= requested + 2, 10000);
    model.setIntervals(60000, 60000);
    QVERIFY(settled([&] { return int(f.calls().size()); }, 600) > 0);
    // Starts and ends of rev alternate: never two at once.
    int inFlight = 0, most = 0;
    for (const auto &call : f.calls()) {
        if (Fixture::command(call) != "rev") continue;
        inFlight += call.contains("done") ? -1 : 1;
        most = qMax(most, inFlight);
    }
    QCOMPARE(most, 1); QCOMPARE(inFlight, 0);
}

void TestOvermindModel::destroyingTheModelWithRequestsInFlightIsSafe()
{
    Fixture f; QVERIFY(f.setUp());
    QVERIFY(f.put("sleep.json", R"({":rev": 0.6, ":board": 0.6, ":move": 0.6, ":show": 0.6})"));
    HgsClient client(FakeCli::program(f.script));
    QSignalSpy replies(&client, &HgsClient::overmindReady);
    bool called = false;
    auto *model = new OvermindModel(&client);
    model->setEnabled(true);
    model->setIntervals(60000, 60000);
    model->watchProject("demo");
    model->write("move", {{"task", "t1"}, {"to", "Build"}}, [&](const OvermindModel::Result &) { called = true; });
    model->read("show", {{"task", "t1"}}, [&](const OvermindModel::Result &) { called = true; });
    QTRY_VERIFY(f.count("rev") == 1 && f.count("board", "demo") == 1 && f.count("move") == 1 && f.count("show") == 1);
    delete model;
    QTRY_COMPARE_WITH_TIMEOUT(replies.size(), 4, 10000);   // answered after the model is gone
    QVERIFY(!called);
}

void TestOvermindModel::disabledModelNeverCallsTheCli()
{
    Fixture f; QVERIFY(f.setUp());
    HgsClient client(FakeCli::program(f.script));
    {
        OvermindModel model(&client);
#ifdef Q_OS_WIN
        QVERIFY(!model.isEnabled());   // no local Overmind until a host is set
#else
        QVERIFY(model.isEnabled());
#endif
    }
    OvermindModel model(&client);
    model.setEnabled(false);
    model.setIntervals(20, 20);
    model.setActive(true);
    model.watchProject("demo");
    model.refresh();
    QSignalSpy failed(&model, &OvermindModel::writeFailed);
    OvermindModel::Result written, read;
    written.ok = read.ok = true;
    QCOMPARE(model.write("move", {{"task", "t1"}, {"to", "Build"}}, [&](const OvermindModel::Result &r) { written = r; }), quint64(0));
    QCOMPARE(model.read("show", {{"task", "t1"}}, [&](const OvermindModel::Result &r) { read = r; }), quint64(0));
    QTRY_COMPARE(failed.size(), 1);
    QCOMPARE(failed[0][0].toString(), QString("move"));
    QCOMPARE(failed[0][1].toJsonObject()["code"].toString(), QString("unavailable"));
    QTRY_VERIFY(!read.ok);
    QVERIFY(!written.ok); QCOMPARE(written.refusal["code"].toString(), QString("unavailable"));
    QVERIFY(!written.refusal["message"].toString().isEmpty()); QVERIFY(!written.requestId.isEmpty());
    QCOMPARE(read.refusal["code"].toString(), QString("unavailable"));
    QCOMPARE(settled([&] { return int(f.calls().size()); }), 0);
    QVERIFY(!model.board("demo").loaded);

    // A host enables it; disabling stops every call again and clears the boards.
    model.setHost("mac");
    QVERIFY(model.isEnabled());
    QTRY_VERIFY(model.board("demo").loaded);
    for (const auto &call : f.calls()) QCOMPARE(call["argv"].toArray().first().toString(), QString("@mac"));
    model.setEnabled(false);
    QVERIFY(!model.board("demo").loaded);
    const int calls = settled([&] { return int(f.calls().size()); });
    QVERIFY(calls > 0);
    QCOMPARE(settled([&] { return int(f.calls().size()); }), calls);
}

QTEST_GUILESS_MAIN(TestOvermindModel)
#include "test_overmindmodel.moc"
