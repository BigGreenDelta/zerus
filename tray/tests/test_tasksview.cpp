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
// stdin. A write prints refuse-<command>.json when present and exits 1;
// otherwise it moves the cursor, so the model reads the board again.
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
    QVERIFY(QFile::remove(f.dir.filePath("refuse-send.json")));
    anyway->click();
    QTRY_COMPARE(f.calls("send").size(), 4);
    const auto first = f.calls("send")[2], retry = f.calls("send")[3];
    QCOMPARE(retry["request_id"], first["request_id"]);
    QCOMPARE(retry["confirm"].toArray(), QJsonArray{"agent_busy"});
    QCOMPARE(retry["task"].toString(), QString("t2"));

    // A machine that is not known yet: names still match on the local replica,
    // but a session on another computer cannot be sent to.
    TasksView::Selection remote = selection("demo", "", "codex/demo/one"); remote.host = "mac";
    view.setSession(remote);
    QVERIFY(!next->isEnabled());

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
    QVERIFY(f.put("refuse-move.json", R"({"ok":false,"error":{"code":"transition_not_allowed","message":"move from Inbox to Check is not allowed for human"}})"));
    auto *check = list(view, "Check");
    drop(check->viewport(), "t1", QPoint(20, check->viewport()->height() - 10));
    QTRY_COMPARE(f.calls("move").size(), 1);
    QCOMPARE(f.calls("move").first()["task"].toString(), QString("t1"));
    QCOMPARE(f.calls("move").first()["to"].toString(), QString("Check"));
    auto *message = view.findChild<QLabel *>("tasksMessage");
    QTRY_VERIFY(message->isVisible());
    QCOMPARE(message->text(), QString("move from Inbox to Check is not allowed for human"));
    QVERIFY(message->property("error").toBool());
    // The card stayed where it was, also after the next poll.
    QCOMPARE(cards(view, "Inbox"), QStringList{"t1"});
    QCOMPARE(cards(view, "Check"), QStringList{"t6"});
    QTest::qWait(200);
    QCOMPARE(cards(view, "Inbox"), QStringList{"t1"});
    QVERIFY(message->isVisible());

    // An allowed drop runs move and clears the message.
    QVERIFY(QFile::remove(f.dir.filePath("refuse-move.json")));
    auto *ready = list(view, "Ready");
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
    // showProject opens a given project and asks for the tab.
    QSignalSpy shown(&reopened, &TasksView::showRequested);
    reopened.setProjects(organization().groups());
    reopened.showProject("demo", "m1\ncodex/demo/one");
    QCOMPARE(reopened.project(), QString("demo"));
    QCOMPARE(shown.size(), 1);
    QCOMPARE(QSettings().value("workspace/tasksProject").toString(), QString("demo"));
}

QTEST_MAIN(TestTasksView)
#include "test_tasksview.moc"
