#include <QtTest>
#include <QApplication>
#include <QComboBox>
#include <QFile>
#include <QFrame>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMimeData>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QTemporaryDir>
#include <QUuid>

#include <algorithm>
#include <memory>

#include "FakeCli.h"
#include "HgsClient.h"
#include "OvermindModel.h"
#include "SessionOrganization.h"
#include "TasksView.h"

namespace {

// A fake `hgs overmind` serving rev, status, decisions and boards from files
// next to it (board-<project>.json). Every call is recorded with its argv and
// stdin. sleep-<command> holds seconds to wait first. A write prints
// refuse-<command>.json when present and exits 1; otherwise it moves the
// cursor, so the model reads the board again, and prints reply-<command>.json
// when present.
const QByteArray kFixture = R"(#!/usr/bin/env python3
import json, os, pathlib, sys, time
root = pathlib.Path(__file__).parent
args = sys.argv[1:]
data = sys.stdin.read()
with (root / 'calls').open('a') as calls:
    calls.write(json.dumps({'argv': args, 'input': data}) + '\n')
def read(name, default=None):
    try:
        return (root / name).read_text()
    except FileNotFoundError:
        return default
if args and args[0].startswith('@'):
    args = args[1:]
command = ' '.join(args[1:-1]) if args and args[0] == 'overmind' and args[-1] == '--json' else '?'
time.sleep(float(read('sleep-' + command, '0')))
cursor = int(read('rev'))
def reply(value):
    print(json.dumps(value))
if command == 'rev':
    reply({'ok': True, 'cursor': cursor})
elif command == 'status':
    reply({'ok': True, 'machine': 'm1', 'cursor': cursor})
elif command == 'decisions':
    reply({'ok': True, 'cursor': cursor, 'decisions': []})
elif command == 'board':
    project = json.loads(data)['project']
    board = json.loads(read('board-' + project + '.json', '{"workflow":{"stages":[]},"tasks":[]}'))
    board.update(ok=True, project=project, cursor=cursor)
    reply(board)
elif read('refuse-' + command + '.json') is not None:
    refusal = read('refuse-' + command + '.json')
    print(refusal)
    print('hgs: ' + json.loads(refusal)['error']['message'], file=sys.stderr)
    sys.exit(1)
else:
    following = root / 'rev.next'
    following.write_text(str(cursor + 1))
    for attempt in range(250):
        try:
            os.replace(following, root / 'rev')
            break
        except PermissionError:
            time.sleep(0.02)
    answer = read('reply-' + command + '.json')
    if answer is not None:
        print(answer)
    else:
        reply({'ok': True, 'task': {'uuid': json.loads(data).get('task', '')}})
)";

QJsonObject session(const QString &machine, const QString &name) { return {{"machine", machine}, {"name", name}}; }

QJsonObject workflow(const QList<QPair<QString, QString>> &stages)
{
    QJsonArray list;
    for (const auto &stage : stages) list.append(QJsonObject{{"name", stage.first}, {"role", stage.second}});
    const QJsonArray rules{
        QJsonObject{{"action", "move"}, {"from", QJsonArray{"Inbox"}}, {"to", "Ready"}, {"actors", QJsonArray{"human"}}},
        QJsonObject{{"action", "move"}, {"from", QJsonArray{"Ready"}}, {"to", "Inbox"}, {"actors", QJsonArray{"human"}}},
        QJsonObject{{"action", "unclaim"}, {"from", QJsonArray{"Doing"}}, {"to", "Ready"}, {"actors", QJsonArray{"human", "autopilot"}}},
        // An agent's move and a move that needs a field are not a person's drag.
        QJsonObject{{"action", "move"}, {"from", QJsonArray{"Doing"}}, {"to", "Later"}, {"actors", QJsonArray{"agent"}}},
        QJsonObject{{"action", "move"}, {"from", QJsonArray{"Inbox"}}, {"to", "Later"}, {"actors", QJsonArray{"human"}}, {"requires", QJsonArray{"reason"}}},
        QJsonObject{{"action", "done"}, {"from", QJsonArray{"Check"}}, {"to", "Shipped"}, {"actors", QJsonArray{"human"}}}};
    return {{"version", 1}, {"stages", list}, {"rules", rules}, {"evidence", "text"}, {"session_wip", 1}};
}

const QList<QPair<QString, QString>> kStages{{"Inbox", "backlog"}, {"Ready", "ready"}, {"Doing", "active"}, {"Check", "review"},
                                             {"Ask", "needs_human"}, {"Shipped", "done"}, {"Later", "parked"}};

QJsonObject task(const QString &uuid, const QString &title, const QString &stage, const QString &rank)
{
    return {{"uuid", uuid}, {"ref", "om-" + uuid}, {"project", "demo"}, {"title", title}, {"stage", stage}, {"rank", rank},
            {"priority", 2}, {"labels", QJsonArray()}, {"flags", QJsonArray()}, {"markers", QJsonArray()},
            {"pinned", QJsonValue::Null}, {"assignee", QJsonValue::Null}, {"reviewer", QJsonValue::Null}, {"queue_of", QJsonValue::Null}};
}

// The demo project. codex/demo/one on m1 owns t2 and t4 (its queue), t5 (working)
// and t7 (Your Call); the same name on m2 owns t10, which is not its task.
QJsonObject demoBoard(const QList<QPair<QString, QString>> &stages = kStages)
{
    const auto one = session("m1", "codex/demo/one");
    auto t1 = task("t1", "Write the parser", "Inbox", "a"); t1["labels"] = QJsonArray{"ui", "blocked"};
    auto t2 = task("t2", "First in the queue", "Ready", "a"); t2["pinned"] = one; t2["queue_of"] = one;
    auto t3 = task("t3", "From the pool", "Ready", "b");
    auto t4 = task("t4", "Second in the queue", "Ready", "c"); t4["pinned"] = one; t4["queue_of"] = one;
    auto t10 = task("t10", "Queued on another machine", "Ready", "d");
    t10["pinned"] = session("m2", "codex/demo/one"); t10["queue_of"] = session("m2", "codex/demo/one");
    auto t5 = task("t5", "Being built", "Doing", "a"); t5["assignee"] = one; t5["markers"] = QJsonArray{"working", "uncertain"};
    auto t11 = task("t11", "Rework the cache", "Doing", "b"); t11["assignee"] = session("m2", "codex/demo/two");
    t11["labels"] = QJsonArray{"review-failed"}; t11["markers"] = QJsonArray{"needs_work", "conflict"};
    auto t6 = task("t6", "Check this", "Check", "a"); t6["reviewer"] = session("m1", "claude/demo/review");
    t6["labels"] = QJsonArray{"reviewed"}; t6["markers"] = QJsonArray{"reviewed"};
    auto t7 = task("t7", "Pick a database", "Ask", "a"); t7["assignee"] = one;
    auto t8 = task("t8", "Shipped thing", "Shipped", "a");
    auto t9 = task("t9", "Someday", "Later", "a"); t9["markers"] = QJsonArray{"orphaned"};
    return {{"workflow", workflow(stages)}, {"tasks", QJsonArray{t1, t2, t3, t4, t10, t5, t11, t6, t7, t8, t9}}};
}

QByteArray compact(const QJsonObject &object) { return QJsonDocument(object).toJson(QJsonDocument::Compact); }

SessionOrganization organization()
{
    return SessionOrganization(QJsonDocument::fromJson(R"({"projects":[
        {"id":"demo","name":"Demo","color":"#e5484d","sessions":["m1\ncodex/demo/one"]},
        {"id":"other","name":"Other","color":"#30a46c"},
        {"id":"secret","name":"Secret","color":"#8e4ec6","accessible":false}],"default_project":"demo"})").object());
}

TasksView::Selection selection(const QString &project, const QString &machine, const QString &name)
{
    TasksView::Selection result;
    result.key = machine + '\n' + name; result.project = project; result.session = {machine, name};
    return result;
}

struct Fixture {
    QTemporaryDir dir;
    QString script;
    std::unique_ptr<HgsClient> client;
    std::unique_ptr<OvermindModel> model;

    bool setUp()
    {
        if (!dir.isValid()) return false;
        script = dir.filePath("hgs");
        if (!FakeCli::install(script, kFixture) || !put("rev", "1") || !put("board-demo.json", compact(demoBoard()))) return false;
        client = std::make_unique<HgsClient>(FakeCli::program(script));
        model = std::make_unique<OvermindModel>(client.get());
        model->setIntervals(40, 40);
        return true;
    }
    bool put(const QString &name, const QByteArray &bytes) { return FakeCli::replace(dir.filePath(name), bytes); }
    QList<QJsonObject> calls(const QString &words) const
    {
        QList<QJsonObject> out;
        QFile file(dir.filePath("calls"));
        if (!file.open(QIODevice::ReadOnly)) return out;
        for (const auto &line : file.readAll().split('\n')) {
            const auto call = QJsonDocument::fromJson(line).object();
            auto argv = call["argv"].toVariant().toStringList();
            if (!argv.isEmpty() && argv.first().startsWith('@')) argv.removeFirst();
            if (argv.mid(1, argv.size() - 2).join(' ') == words)
                out.append(QJsonDocument::fromJson(call["input"].toString().toUtf8()).object());
        }
        return out;
    }
};

// Columns in display order, left to right.
QList<QFrame *> columns(TasksView &view)
{
    auto list = view.findChildren<QFrame *>("taskColumn");
    std::sort(list.begin(), list.end(), [&](QFrame *a, QFrame *b) { return a->mapTo(&view, QPoint()).x() < b->mapTo(&view, QPoint()).x(); });
    return list;
}
QStringList stages(TasksView &view)
{
    QStringList out;
    for (auto *column : columns(view)) out << column->property("stage").toString();
    return out;
}
QListWidget *list(TasksView &view, const QString &stage)
{
    for (auto *candidate : view.findChildren<QListWidget *>("taskList"))
        if (candidate->property("stage").toString() == stage) return candidate;
    return nullptr;
}
QFrame *column(TasksView &view, const QString &stage)
{
    for (auto *candidate : columns(view)) if (candidate->property("stage").toString() == stage) return candidate;
    return nullptr;
}
QStringList cards(TasksView &view, const QString &stage)
{
    QStringList out;
    if (auto *cards = list(view, stage))
        for (int row = 0; row < cards->count(); ++row) out << cards->item(row)->data(TaskRoles::Uuid).toString();
    return out;
}
QListWidgetItem *card(TasksView &view, const QString &uuid)
{
    for (auto *cards : view.findChildren<QListWidget *>("taskList"))
        for (int row = 0; row < cards->count(); ++row)
            if (cards->item(row)->data(TaskRoles::Uuid).toString() == uuid) return cards->item(row);
    return nullptr;
}
QStringList highlighted(TasksView &view)
{
    QStringList out;
    for (auto *cards : view.findChildren<QListWidget *>("taskList"))
        for (int row = 0; row < cards->count(); ++row)
            if (cards->item(row)->data(TaskRoles::Highlighted).toBool()) out << cards->item(row)->data(TaskRoles::Uuid).toString();
    std::sort(out.begin(), out.end());
    return out;
}

// Drags a card over `target` and drops it at `position` (viewport coordinates).
void drop(QWidget *target, const QString &uuid, const QPoint &position)
{
    QMimeData mime;
    mime.setData(TasksView::MimeType, uuid.toUtf8());
    QDragEnterEvent enter(position, Qt::MoveAction, &mime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(target, &enter);
    QDragMoveEvent move(position, Qt::MoveAction, &mime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(target, &move);
    QDropEvent dropped(QPointF(position), Qt::MoveAction, &mime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(target, &dropped);
}

// Whether `target` takes a drag of the card, and whether its column lights up meanwhile.
bool takes(QWidget *target, QFrame *column, const QString &uuid, bool *lit = nullptr)
{
    QMimeData mime;
    mime.setData(TasksView::MimeType, uuid.toUtf8());
    QDragEnterEvent enter(QPoint(10, 10), Qt::MoveAction, &mime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(target, &enter);
    if (lit) *lit = column->property("dropTarget").toBool();
    QDragLeaveEvent leave;
    QApplication::sendEvent(target, &leave);
    return enter.isAccepted();
}

} // namespace

class TestTasksView : public QObject {
    Q_OBJECT
private slots:
    void initTestCase();
    void init() { QSettings().remove("workspace"); }
    void columnsFollowTheWorkflow();
    void doneAndIceboxStartCollapsed();
    void highlightsTheSelectedSession();
    void refusedDropShowsTheReason();
    void forbiddenColumnsRejectTheDrag();
    void workflowWithoutRulesLeavesDragsToTheCli();
    void filteredReorderSkipsHiddenCards();
    void sendNextWarnsWhenDeliveryIsUncertain();
    void sendAnywayStaysWithItsSession();
    void sendNextSkipsEpics();
    void projectPersistsAcrossSessions();
private:
    std::unique_ptr<QTemporaryDir> m_settings;
};

void TestTasksView::initTestCase()
{
    m_settings = std::make_unique<QTemporaryDir>(); QVERIFY(m_settings->isValid());
    QCoreApplication::setOrganizationName("hgs-tests");
    QCoreApplication::setApplicationName("tasks-view");
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, m_settings->path());
}

void TestTasksView::columnsFollowTheWorkflow()
{
    Fixture f; QVERIFY(f.setUp());
    TasksView view(f.model.get()); view.resize(1400, 720);
    view.setProjects(organization().groups());
    view.setSession(selection("demo", "m1", "codex/demo/one"));
    view.show();
    QTRY_COMPARE(stages(view), (QStringList{"Inbox", "Ready", "Doing", "Check", "Shipped", "Later"}));
    QCOMPARE(view.project(), QString("demo"));
    QCOMPARE(cards(view, "Inbox"), QStringList{"t1"});
    QCOMPARE(cards(view, "Ready"), (QStringList{"t2", "t3", "t4", "t10"}));
    QCOMPARE(cards(view, "Doing"), (QStringList{"t5", "t11"}));
    QCOMPARE(cards(view, "Check"), QStringList{"t6"});
    QCOMPARE(cards(view, "Shipped"), QStringList{"t8"});
    QVERIFY(!list(view, "Ask"));   // Your Call is a strip, not a column

    // A card carries its ref, title, labels, session tag and markers.
    auto *parser = card(view, "t1");
    QCOMPARE(parser->data(TaskRoles::Ref).toString(), QString("om-t1"));
    QCOMPARE(parser->data(TaskRoles::Title).toString(), QString("Write the parser"));
    QCOMPARE(parser->data(TaskRoles::Labels).toStringList(), (QStringList{"ui", "blocked"}));
    QCOMPARE(card(view, "t2")->data(TaskRoles::Session).toString(), QString("codex/demo/one"));
    QCOMPARE(card(view, "t5")->data(TaskRoles::Session).toString(), QString("codex/demo/one"));
    QCOMPARE(card(view, "t5")->data(TaskRoles::Markers).toStringList(), (QStringList{"working", "uncertain"}));
    QCOMPARE(card(view, "t11")->data(TaskRoles::Markers).toStringList(), (QStringList{"needs_work", "conflict"}));
    // Verdict labels are shown as markers, not twice.
    QVERIFY(card(view, "t11")->data(TaskRoles::Labels).toStringList().isEmpty());
    QCOMPARE(card(view, "t6")->data(TaskRoles::Markers).toStringList(), QStringList{"reviewed"});
    QCOMPARE(card(view, "t6")->data(TaskRoles::Session).toString(), QString("claude/demo/review"));
    const auto description = card(view, "t5")->data(Qt::AccessibleTextRole).toString();
    QVERIFY2(description.contains("om-t5") && description.contains("Being built") && description.contains("working"), qPrintable(description));
    // Column headers count their cards.
    QVERIFY(column(view, "Ready")->findChild<QWidget *>("taskColumnHeader")->accessibleName().contains("4"));

    // A changed workflow rebuilds the columns, once the cursor moves.
    auto stagesWithSpec = kStages; stagesWithSpec.insert(2, {"Spec", "active"});
    auto board = demoBoard(stagesWithSpec);
    QVERIFY(f.put("board-demo.json", compact(board)));
    QVERIFY(f.put("rev", "2"));
    QTRY_COMPARE(stages(view), (QStringList{"Inbox", "Ready", "Spec", "Doing", "Check", "Shipped", "Later"}));
    QCOMPARE(cards(view, "Ready"), (QStringList{"t2", "t3", "t4", "t10"}));
}

void TestTasksView::doneAndIceboxStartCollapsed()
{
    Fixture f; QVERIFY(f.setUp());
    TasksView view(f.model.get()); view.resize(1400, 720);
    view.setProjects(organization().groups());
    view.setSession(selection("demo", "m1", "codex/demo/one"));
    view.show();
    QTRY_VERIFY(column(view, "Shipped"));
    for (const auto &stage : {"Inbox", "Ready", "Doing", "Check"}) {
        QVERIFY2(!column(view, stage)->property("collapsed").toBool(), stage);
        QVERIFY(list(view, stage)->isVisible());
    }
    for (const auto &stage : {"Shipped", "Later"}) {
        QVERIFY2(column(view, stage)->property("collapsed").toBool(), stage);
        QVERIFY(!list(view, stage)->isVisible());
        QVERIFY(column(view, stage)->width() < column(view, "Inbox")->width());
    }
    // A click on its header opens a collapsed column, and another closes it.
    auto *header = qobject_cast<QAbstractButton *>(column(view, "Shipped")->findChild<QWidget *>("taskColumnHeader"));
    QVERIFY(header); header->click();
    QVERIFY(!column(view, "Shipped")->property("collapsed").toBool());
    QTRY_VERIFY(list(view, "Shipped")->isVisible());
    // It stays open when the board is read again.
    QVERIFY(f.put("rev", "2"));
    QTest::qWait(200);
    QVERIFY(!column(view, "Shipped")->property("collapsed").toBool());
    header = qobject_cast<QAbstractButton *>(column(view, "Shipped")->findChild<QWidget *>("taskColumnHeader"));
    header->click();
    QVERIFY(column(view, "Shipped")->property("collapsed").toBool());

    // Your Call is a warning strip above the board; a click opens the card.
    auto *strip = view.findChild<QFrame *>("tasksYourCall");
    QVERIFY(strip); QVERIFY(strip->isVisible());
    auto decisions = strip->findChildren<QPushButton *>("tasksDecision");
    QCOMPARE(decisions.size(), 1);
    QVERIFY(decisions.first()->text().contains("Pick a database"));
    QSignalSpy opened(&view, &TasksView::taskOpened);
    decisions.first()->click();
    QCOMPARE(opened.size(), 1);
    QCOMPARE(opened.first().first().toString(), QString("t7"));
    QCOMPARE(view.openedTask(), QString("t7"));
    // A card click opens that card.
    auto *ready = list(view, "Ready");
    QTest::mouseClick(ready->viewport(), Qt::LeftButton, {}, ready->visualItemRect(ready->item(1)).center());
    QCOMPARE(view.openedTask(), QString("t3"));
    QCOMPARE(opened.last().first().toString(), QString("t3"));

    // Opening a card in a collapsed column shows it, but the column is still
    // collapsed the next time the board is built; a header click is remembered.
    view.openTask("t8");
    QVERIFY(!column(view, "Shipped")->property("collapsed").toBool());
    QVERIFY(list(view, "Shipped")->item(0)->isSelected());
    auto *projects = view.findChild<QComboBox *>("tasksProject");
    projects->setCurrentIndex(projects->findData("other"));
    projects->setCurrentIndex(projects->findData("demo"));
    QTRY_VERIFY(column(view, "Shipped"));
    QVERIFY(column(view, "Shipped")->property("collapsed").toBool());
    qobject_cast<QAbstractButton *>(column(view, "Later")->findChild<QWidget *>("taskColumnHeader"))->click();
    projects->setCurrentIndex(projects->findData("other"));
    projects->setCurrentIndex(projects->findData("demo"));
    QTRY_VERIFY(column(view, "Later"));
    QVERIFY(!column(view, "Later")->property("collapsed").toBool());
    strip = view.findChild<QFrame *>("tasksYourCall");

    // No Your Call task: no strip.
    auto board = demoBoard();
    auto tasks = board["tasks"].toArray();
    for (int i = tasks.size(); i-- > 0;) if (tasks[i].toObject()["stage"] == "Ask") tasks.removeAt(i);
    board["tasks"] = tasks;
    QVERIFY(f.put("board-demo.json", compact(board)));
    QVERIFY(f.put("rev", "3"));
    QTRY_VERIFY(!strip->isVisible());
}

void TestTasksView::highlightsTheSelectedSession()
{
    Fixture f; QVERIFY(f.setUp());
    TasksView view(f.model.get()); view.resize(1400, 720);
    view.setProjects(organization().groups());
    view.setSession(selection("demo", "m1", "codex/demo/one"));
    view.show();
    QTRY_VERIFY(card(view, "t2"));
    // Its queue, its work and Your Call; not the same name on another machine.
    QCOMPARE(highlighted(view), (QStringList{"t2", "t4", "t5"}));
    auto *strip = view.findChild<QFrame *>("tasksSessionStrip");
    QVERIFY(strip); QVERIFY(strip->isVisible());
    QCOMPARE(view.findChild<QLabel *>("tasksSessionQueue")->text(), QString("This session's queue: 2"));
    auto *next = view.findChild<QPushButton *>("tasksSendNext");
    QVERIFY(next->isEnabled());
    QCOMPARE(view.findChild<QPushButton *>("tasksDecision")->property("highlighted").toBool(), true);

    // Send next sends the first task of this session's queue.
    next->click();
    QTRY_COMPARE(f.calls("send").size(), 1);
    auto sent = f.calls("send").first();
    QCOMPARE(sent["task"].toString(), QString("t2"));
    QCOMPARE(sent["session"].toObject(), session("m1", "codex/demo/one"));
    QVERIFY(!QUuid(sent["request_id"].toString()).isNull());

    // A session with an empty queue sends the first task of the project pool.
    view.setSession(selection("demo", "m1", "claude/demo/review"));
    QCOMPARE(highlighted(view), QStringList{"t6"});
    QCOMPARE(view.findChild<QLabel *>("tasksSessionQueue")->text(), QString("This session's queue: 0"));
    QTRY_VERIFY(next->isEnabled());
    next->click();
    QTRY_COMPARE(f.calls("send").size(), 2);
    QCOMPARE(f.calls("send").last()["task"].toString(), QString("t3"));

    // A confirmable refusal offers to send anyway, repeating the request.
    QVERIFY(f.put("refuse-send.json", R"({"ok":false,"error":{"code":"agent_busy","message":"The agent is working","field":"confirm","state":"missing"}})"));
    view.setSession(selection("demo", "m1", "codex/demo/one"));
    QTRY_VERIFY(next->isEnabled());
    next->click();
    auto *message = view.findChild<QLabel *>("tasksMessage");
    QTRY_VERIFY(message->isVisible() && message->text().contains("The agent is working"));
    QVERIFY(message->property("error").toBool());
    auto *anyway = view.findChild<QPushButton *>("tasksSendAnyway");
    QVERIFY(anyway && anyway->isVisible());
    QCOMPARE(anyway->text(), QString("Send om-t2 to codex/demo/one anyway"));
    QVERIFY(QFile::remove(f.dir.filePath("refuse-send.json")));
    anyway->click();
    QTRY_COMPARE(f.calls("send").size(), 4);
    const auto first = f.calls("send")[2], retry = f.calls("send")[3];
    QCOMPARE(retry["request_id"], first["request_id"]);
    QCOMPARE(retry["confirm"].toArray(), QJsonArray{"agent_busy"});
    QCOMPARE(retry["task"].toString(), QString("t2"));

    // A machine that is not known yet: names still highlight, but nothing is
    // sent, on this computer or another, since the name may be a session elsewhere.
    QTRY_VERIFY(next->isEnabled());
    TasksView::Selection remote = selection("demo", "", "codex/demo/one"); remote.host = "mac";
    view.setSession(remote);
    QVERIFY(!next->isEnabled());
    view.setSession(selection("demo", "", "codex/demo/one"));
    QVERIFY(!next->isEnabled());
    QCOMPARE(next->toolTip(), QString("Overmind does not know this session's computer yet."));
    QVERIFY(highlighted(view).contains("t2"));

    // A session of another project highlights nothing and has no strip.
    view.setSession(selection("other", "m1", "codex/other/one"));
    QVERIFY(highlighted(view).isEmpty());
    QVERIFY(!strip->isVisible());
    QCOMPARE(view.project(), QString("demo"));
}

void TestTasksView::refusedDropShowsTheReason()
{
    Fixture f; QVERIFY(f.setUp());
    TasksView view(f.model.get()); view.resize(1400, 720);
    view.setProjects(organization().groups());
    view.setSession(selection("demo", "m1", "codex/demo/one"));
    view.show();
    QTRY_VERIFY(card(view, "t1"));
    // The workflow allows the move; the CLI's gates refuse it.
    QVERIFY(f.put("refuse-move.json", R"({"ok":false,"error":{"code":"conflict","message":"om-t1 has conflicting edits: resolve them first"}})"));
    auto *ready = list(view, "Ready");
    drop(ready->viewport(), "t1", QPoint(20, ready->viewport()->height() - 10));
    QTRY_COMPARE(f.calls("move").size(), 1);
    QCOMPARE(f.calls("move").first()["task"].toString(), QString("t1"));
    QCOMPARE(f.calls("move").first()["to"].toString(), QString("Ready"));
    auto *message = view.findChild<QLabel *>("tasksMessage");
    QTRY_VERIFY(message->isVisible());
    QCOMPARE(message->text(), QString("om-t1 has conflicting edits: resolve them first"));
    QVERIFY(message->property("error").toBool());
    // The card stayed where it was, also after the next poll.
    QCOMPARE(cards(view, "Inbox"), QStringList{"t1"});
    QCOMPARE(cards(view, "Ready"), (QStringList{"t2", "t3", "t4", "t10"}));
    QTest::qWait(200);
    QCOMPARE(cards(view, "Inbox"), QStringList{"t1"});
    QVERIFY(message->isVisible());

    // An allowed drop runs move and clears the message.
    QVERIFY(QFile::remove(f.dir.filePath("refuse-move.json")));
    drop(ready->viewport(), "t1", QPoint(20, ready->viewport()->height() - 10));
    QTRY_COMPARE(f.calls("move").size(), 2);
    QCOMPARE(f.calls("move").last()["to"].toString(), QString("Ready"));
    QTRY_VERIFY(!message->isVisible());

    // A collapsed column takes drops on its header.
    auto *shipped = column(view, "Shipped");
    QVERIFY(shipped->property("collapsed").toBool());
    auto *header = shipped->findChild<QWidget *>("taskColumnHeader");
    drop(header, "t6", header->rect().center());
    QTRY_COMPARE(f.calls("move").size(), 3);
    QCOMPARE(f.calls("move").last()["task"].toString(), QString("t6"));
    QCOMPARE(f.calls("move").last()["to"].toString(), QString("Shipped"));

    // Inside a column a drop reorders: before the card under the cursor, or last.
    drop(ready->viewport(), "t4", ready->visualItemRect(ready->item(0)).topLeft() + QPoint(10, 4));
    QTRY_COMPARE(f.calls("reorder").size(), 1);
    QCOMPARE(f.calls("reorder").last()["task"].toString(), QString("t4"));
    QCOMPARE(f.calls("reorder").last()["before"].toString(), QString("t2"));
    drop(ready->viewport(), "t2", QPoint(20, ready->viewport()->height() - 10));
    QTRY_COMPARE(f.calls("reorder").size(), 2);
    QCOMPARE(f.calls("reorder").last()["task"].toString(), QString("t2"));
    QVERIFY(!f.calls("reorder").last().contains("before"));
    // Dropping a card where it already is writes nothing.
    drop(ready->viewport(), "t3", ready->visualItemRect(ready->item(1)).topLeft() + QPoint(10, 4));
    drop(ready->viewport(), "t3", ready->visualItemRect(ready->item(2)).topLeft() + QPoint(10, 4));
    QTest::qWait(200);
    QCOMPARE(f.calls("reorder").size(), 2);
    QCOMPARE(f.calls("move").size(), 3);

    // A move that times out shows the model's one sentence about it.
    QVERIFY(f.put("sleep-move", "1.5"));
    auto timeouts = f.model->timeouts(); timeouts.local = 300; f.model->setTimeouts(timeouts);
    drop(ready->viewport(), "t1", QPoint(20, ready->viewport()->height() - 10));
    QTRY_VERIFY_WITH_TIMEOUT(message->isVisible(), 5000);
    QCOMPARE(message->text().count("may have been"), 1);
    QVERIFY(message->property("warning").toBool());
}

void TestTasksView::forbiddenColumnsRejectTheDrag()
{
    Fixture f; QVERIFY(f.setUp());
    TasksView view(f.model.get()); view.resize(1600, 720);
    view.setProjects(organization().groups());
    view.setSession(selection("demo", "m1", "codex/demo/one"));
    view.show();
    QTRY_VERIFY(card(view, "t1"));
    const auto header = [&](const QString &stage) { return column(view, stage)->findChild<QWidget *>("taskColumnHeader"); };
    bool lit = false;
    // A person's moves in this workflow: Inbox <-> Ready, Doing -> Ready (take back), Check -> Shipped.
    QVERIFY(takes(list(view, "Ready")->viewport(), column(view, "Ready"), "t1", &lit)); QVERIFY(lit);
    QVERIFY(takes(header("Inbox"), column(view, "Inbox"), "t2"));
    QVERIFY(takes(list(view, "Ready")->viewport(), column(view, "Ready"), "t5"));
    QVERIFY(takes(header("Shipped"), column(view, "Shipped"), "t6"));
    // Its own column takes it, to reorder.
    QVERIFY(takes(list(view, "Ready")->viewport(), column(view, "Ready"), "t2"));
    // Not allowed: no rule, an active stage (send instead), an agent's rule, a rule that needs a field.
    QVERIFY(!takes(list(view, "Check")->viewport(), column(view, "Check"), "t1", &lit)); QVERIFY(!lit);
    QVERIFY(!takes(list(view, "Doing")->viewport(), column(view, "Doing"), "t2"));
    QVERIFY(!takes(header("Doing"), column(view, "Doing"), "t1"));
    QVERIFY(!takes(list(view, "Inbox")->viewport(), column(view, "Inbox"), "t5"));
    QVERIFY(!takes(header("Later"), column(view, "Later"), "t5"));
    QVERIFY(!takes(header("Later"), column(view, "Later"), "t1"));
    QVERIFY(!takes(list(view, "Ready")->viewport(), column(view, "Ready"), "unknown"));
    // A drop there anyway writes nothing.
    auto *check = list(view, "Check");
    drop(check->viewport(), "t1", QPoint(20, check->viewport()->height() - 10));
    drop(header("Later"), "t1", QPoint(10, 10));
    QTest::qWait(250);
    QVERIFY(f.calls("move").isEmpty());
    QVERIFY(!view.findChild<QLabel *>("tasksMessage")->isVisible());
}

// A workflow that names no rules (missing, not an array, or empty) does not
// limit drags: the CLI decides. Its roles still do: an active stage takes a send.
void TestTasksView::workflowWithoutRulesLeavesDragsToTheCli()
{
    for (const QJsonValue &rules : {QJsonValue(QJsonValue::Undefined), QJsonValue(QStringLiteral("none")), QJsonValue(QJsonArray())}) {
        Fixture f; QVERIFY(f.setUp());
        auto board = demoBoard();
        auto flow = board["workflow"].toObject();
        if (rules.isUndefined()) flow.remove("rules"); else flow["rules"] = rules;
        board["workflow"] = flow;
        QVERIFY(f.put("board-demo.json", compact(board)));
        TasksView view(f.model.get()); view.resize(1600, 720);
        view.setProjects(organization().groups());
        view.setSession(selection("demo", "m1", "codex/demo/one"));
        view.show();
        QTRY_VERIFY(card(view, "t1"));
        bool lit = false;
        QVERIFY(takes(list(view, "Check")->viewport(), column(view, "Check"), "t1", &lit)); QVERIFY(lit);
        QVERIFY(takes(column(view, "Later")->findChild<QWidget *>("taskColumnHeader"), column(view, "Later"), "t5", &lit)); QVERIFY(lit);
        QVERIFY(!takes(list(view, "Doing")->viewport(), column(view, "Doing"), "t2"));
        QVERIFY(f.put("refuse-move.json", R"({"ok":false,"error":{"code":"transition_not_allowed","message":"move from Inbox to Check is not allowed for human"}})"));
        auto *check = list(view, "Check");
        drop(check->viewport(), "t1", QPoint(20, check->viewport()->height() - 10));
        QTRY_COMPARE(f.calls("move").size(), 1);
        QCOMPARE(f.calls("move").first()["to"].toString(), QString("Check"));
        auto *message = view.findChild<QLabel *>("tasksMessage");
        QTRY_COMPARE(message->text(), QString("move from Inbox to Check is not allowed for human"));
        QCOMPARE(cards(view, "Inbox"), QStringList{"t1"});
    }
}

void TestTasksView::filteredReorderSkipsHiddenCards()
{
    Fixture f; QVERIFY(f.setUp());
    TasksView view(f.model.get()); view.resize(1400, 720);
    view.setProjects(organization().groups());
    view.setSession(selection("demo", "m1", "codex/demo/one"));
    view.show();
    QTRY_VERIFY(card(view, "t2"));
    auto *ready = list(view, "Ready");
    auto *filter = view.findChild<QLineEdit *>("tasksFilter");
    // Shown: t2, t4, t10 (t3 hidden). t2 dropped just above t4 stays where it is.
    filter->setText("queue");
    QVERIFY(card(view, "t3")->isHidden());
    drop(ready->viewport(), "t2", ready->visualItemRect(card(view, "t4")).topLeft() + QPoint(10, 4));
    // Shown: t4 only. t4 dropped at the end stays before the hidden t10.
    filter->setText("second");
    drop(ready->viewport(), "t4", QPoint(20, ready->viewport()->height() - 10));
    QTest::qWait(250);
    QVERIFY(f.calls("reorder").isEmpty());
    // A real move among the shown cards still reorders.
    filter->setText("queue");
    drop(ready->viewport(), "t10", ready->visualItemRect(card(view, "t2")).topLeft() + QPoint(10, 4));
    QTRY_COMPARE(f.calls("reorder").size(), 1);
    QCOMPARE(f.calls("reorder").first()["task"].toString(), QString("t10"));
    QCOMPARE(f.calls("reorder").first()["before"].toString(), QString("t2"));
}

void TestTasksView::sendNextWarnsWhenDeliveryIsUncertain()
{
    Fixture f; QVERIFY(f.setUp());
    TasksView view(f.model.get()); view.resize(1400, 720);
    view.setProjects(organization().groups());
    view.setSession(selection("demo", "m1", "codex/demo/one"));
    view.show();
    auto *next = view.findChild<QPushButton *>("tasksSendNext");
    auto *message = view.findChild<QLabel *>("tasksMessage");
    auto *anyway = view.findChild<QPushButton *>("tasksSendAnyway");
    QTRY_VERIFY(card(view, "t2"));
    QTRY_VERIFY(next->isEnabled());
    // Recorded, but the input was not confirmed: a warning, never a retry.
    QVERIFY(f.put("reply-send.json", R"({"ok":true,"delivery":"uncertain","reason":"the input receipt is missing","task":{"uuid":"t2"}})"));
    next->click();
    QTRY_VERIFY(message->isVisible());
    QCOMPARE(message->text(), QString("om-t2 may not have reached codex/demo/one: the input receipt is missing. Check the task before sending it again."));
    QVERIFY(message->property("warning").toBool());
    QVERIFY(!message->property("error").toBool());
    QVERIFY(!anyway->isVisible());
    // A confirmed delivery.
    QVERIFY(f.put("reply-send.json", R"({"ok":true,"delivery":"sent","reason":"","task":{"uuid":"t2"}})"));
    QTRY_VERIFY(next->isEnabled());
    next->click();
    QTRY_COMPARE(message->text(), QString("Sent om-t2 to codex/demo/one."));
    QVERIFY(!message->property("warning").toBool());
    // A deadline: one sentence that it may have been sent.
    QVERIFY(f.put("sleep-send", "1.5"));
    auto timeouts = f.model->timeouts(); timeouts.deliveryLocal = 300; f.model->setTimeouts(timeouts);
    QTRY_VERIFY(next->isEnabled());
    next->click();
    QTRY_VERIFY_WITH_TIMEOUT(message->isVisible() && message->text().contains("did not answer"), 5000);
    QCOMPARE(message->text(), QString("The task board did not answer in time: om-t2 may have been sent to codex/demo/one. Check the task before sending it again."));
    QVERIFY(!anyway->isVisible());
}

void TestTasksView::sendAnywayStaysWithItsSession()
{
    Fixture f; QVERIFY(f.setUp());
    TasksView view(f.model.get()); view.resize(1400, 720);
    view.setProjects(organization().groups());
    view.setSession(selection("demo", "m1", "codex/demo/one"));
    view.show();
    auto *next = view.findChild<QPushButton *>("tasksSendNext");
    auto *message = view.findChild<QLabel *>("tasksMessage");
    auto *anyway = view.findChild<QPushButton *>("tasksSendAnyway");
    QTRY_VERIFY(card(view, "t2"));
    QTRY_VERIFY(next->isEnabled());
    QVERIFY(f.put("refuse-send.json", R"({"ok":false,"error":{"code":"wip_exceeded","message":"codex/demo/one already holds a task","field":"confirm","state":"missing"}})"));
    next->click();
    QTRY_VERIFY(anyway->isVisible());
    QCOMPARE(anyway->text(), QString("Send om-t2 to codex/demo/one anyway"));
    // Another session: the offer and its message go; coming back does not restore them.
    view.setSession(selection("demo", "m1", "claude/demo/review"));
    QVERIFY(!message->isVisible());
    QVERIFY(!anyway->isVisible());
    view.setSession(selection("demo", "m1", "codex/demo/one"));
    QVERIFY(!anyway->isVisible());
    QCOMPARE(f.calls("send").size(), 1);
    // A refusal that arrives after the switch offers nothing.
    QVERIFY(f.put("sleep-send", "0.6"));
    QTRY_VERIFY(next->isEnabled());
    next->click();
    view.setSession(selection("demo", "m1", "claude/demo/review"));
    QTRY_VERIFY_WITH_TIMEOUT(message->isVisible(), 5000);
    QVERIFY(message->text().contains("already holds a task"));
    QVERIFY(!anyway->isVisible());
    QTest::qWait(100);
    QCOMPARE(f.calls("send").size(), 2);
    QCOMPARE(f.calls("send").last()["session"].toObject()["name"].toString(), QString("codex/demo/one"));
}

void TestTasksView::sendNextSkipsEpics()
{
    Fixture f; QVERIFY(f.setUp());
    auto board = demoBoard();
    auto tasks = board["tasks"].toArray();
    for (int i = 0; i < tasks.size(); ++i) {
        auto task = tasks[i].toObject();
        if (task["uuid"] == "t2" || task["uuid"] == "t3") { task["labels"] = QJsonArray{"epic"}; tasks[i] = task; }
    }
    board["tasks"] = tasks;
    QVERIFY(f.put("board-demo.json", compact(board)));
    TasksView view(f.model.get()); view.resize(1400, 720);
    view.setProjects(organization().groups());
    view.setSession(selection("demo", "m1", "codex/demo/one"));
    view.show();
    auto *next = view.findChild<QPushButton *>("tasksSendNext");
    QTRY_VERIFY(card(view, "t2"));
    QTRY_VERIFY(next->isEnabled());
    // The queue's epic t2 is skipped for t4.
    next->click();
    QTRY_COMPARE(f.calls("send").size(), 1);
    QCOMPARE(f.calls("send").first()["task"].toString(), QString("t4"));
    // The pool's epic t3 is skipped too: the review session has nothing to send.
    view.setSession(selection("demo", "m1", "claude/demo/review"));
    QTRY_VERIFY(!next->isEnabled());
    QCOMPARE(next->toolTip(), QString("No task waits in this session's queue or in the project pool."));
}

void TestTasksView::projectPersistsAcrossSessions()
{
    Fixture f; QVERIFY(f.setUp());
    QVERIFY(f.put("board-other.json", compact(QJsonObject{{"workflow", workflow(kStages)}, {"tasks", QJsonArray()}})));
    {
        TasksView view(f.model.get()); view.resize(900, 600);
        view.setProjects(organization().groups());
        // Accessible projects only, with their color.
        auto *projects = view.findChild<QComboBox *>("tasksProject");
        QVERIFY(projects);
        QCOMPARE(projects->count(), 2);
        QCOMPARE(projects->itemData(0).toString(), QString("demo"));
        QCOMPARE(projects->itemText(1), QString("Other"));
        QVERIFY(!projects->itemIcon(0).isNull());
        // First use opens the selected session's project.
        view.setSession(selection("other", "m1", "codex/other/one"));
        QVERIFY(view.project().isEmpty());
        view.show();
        QCOMPARE(view.project(), QString("other"));
        QCOMPARE(QSettings().value("workspace/tasksProject").toString(), QString("other"));
        QCOMPARE(projects->currentData().toString(), QString("other"));
        // Choosing a project persists it.
        projects->setCurrentIndex(projects->findData("demo"));
        QCOMPARE(view.project(), QString("demo"));
        QCOMPARE(QSettings().value("workspace/tasksProject").toString(), QString("demo"));
        QTRY_VERIFY(card(view, "t1"));
        // Filter, open card and scroll position survive session changes.
        auto *filter = view.findChild<QLineEdit *>("tasksFilter");
        filter->setText("queue");
        QVERIFY(list(view, "Inbox")->item(0)->isHidden());
        QVERIFY(!card(view, "t2")->isHidden());
        auto *ready = list(view, "Ready");
        QTest::mouseClick(ready->viewport(), Qt::LeftButton, {}, ready->visualItemRect(card(view, "t2")).center());
        QCOMPARE(view.openedTask(), QString("t2"));
        auto *board = view.findChild<QScrollArea *>("tasksBoard");
        QVERIFY(board->horizontalScrollBar()->maximum() > 0);
        board->horizontalScrollBar()->setValue(120);
        view.setSession(selection("other", "m1", "codex/other/two"));
        view.setSession(selection("demo", "m1", "codex/demo/one"));
        view.setSession({});
        QCOMPARE(view.project(), QString("demo"));
        QCOMPARE(filter->text(), QString("queue"));
        QCOMPARE(view.openedTask(), QString("t2"));
        QVERIFY(ready->item(0)->isSelected());
        QCOMPARE(board->horizontalScrollBar()->value(), 120);
        QCOMPARE(QSettings().value("workspace/tasksProject").toString(), QString("demo"));
        // A click opens a partly shown card without scrolling the board sideways.
        filter->clear();
        board->horizontalScrollBar()->setValue(0);
        auto *check = list(view, "Check");
        QVERIFY(check->mapTo(board->viewport(), QPoint(20, 0)).x() < board->viewport()->width());
        QVERIFY(check->mapTo(board->viewport(), QPoint(check->width(), 0)).x() > board->viewport()->width());
        QTest::mouseClick(check->viewport(), Qt::LeftButton, {}, check->visualItemRect(card(view, "t6")).topLeft() + QPoint(8, 8));
        QCOMPARE(view.openedTask(), QString("t6"));
        QCOMPARE(board->horizontalScrollBar()->value(), 0);
        // openTask reveals it.
        view.openTask("t1");
        board->horizontalScrollBar()->setValue(0);
        view.openTask("t6");
        QVERIFY(board->horizontalScrollBar()->value() > 0);
    }
    // A restart opens the chosen project, whatever session is selected.
    TasksView reopened(f.model.get());
    reopened.setProjects(organization().groups());
    reopened.setSession(selection("other", "m1", "codex/other/one"));
    reopened.show();
    QCOMPARE(reopened.project(), QString("demo"));
    // A project that became inaccessible falls back to the selected session's one.
    auto groups = organization().groups();
    groups[0].accessible = false;
    reopened.setProjects(groups);
    QCOMPARE(reopened.project(), QString("other"));
    QCOMPARE(QSettings().value("workspace/tasksProject").toString(), QString("demo"));
    // Offered again, the stored choice comes back.
    reopened.setProjects(organization().groups());
    QCOMPARE(reopened.project(), QString("demo"));
    // A choice made during a fallback is kept instead.
    reopened.setProjects(groups);
    QCOMPARE(reopened.project(), QString("other"));
    auto *projects = reopened.findChild<QComboBox *>("tasksProject");
    emit projects->activated(projects->findData("other"));   // picking the project shown stores it
    QCOMPARE(QSettings().value("workspace/tasksProject").toString(), QString("other"));
    reopened.setProjects(organization().groups());
    QCOMPARE(reopened.project(), QString("other"));
    // showProject opens a given project and asks for the tab.
    QSignalSpy shown(&reopened, &TasksView::showRequested);
    reopened.showProject("demo", "m1\ncodex/demo/one");
    QCOMPARE(reopened.project(), QString("demo"));
    QCOMPARE(shown.size(), 1);
    QCOMPARE(QSettings().value("workspace/tasksProject").toString(), QString("demo"));
}

QTEST_MAIN(TestTasksView)
#include "test_tasksview.moc"
