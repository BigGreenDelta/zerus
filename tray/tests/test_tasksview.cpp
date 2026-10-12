#include <QtTest>
#include <QApplication>
#include <QBuffer>
#include <QClipboard>
#include <QComboBox>
#include <QFile>
#include <QFrame>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QTemporaryDir>
#include <QTimer>
#include <QToolButton>
#include <QUuid>

#include <algorithm>
#include <memory>

#ifdef Q_OS_UNIX
#include <sys/stat.h>
#endif

#include "FakeCli.h"
#include "HgsClient.h"
#include "MarkdownText.h"
#include "OvermindModel.h"
#include "SessionOrganization.h"
#include "TaskPanel.h"
#include "TasksView.h"

namespace {

// A fake `hgs overmind` serving rev, status, decisions and boards from files
// next to it (board-<project>.json). `show` answers the board's task with a
// prompt, merged with show-<uuid>.json; `attachment` names image-<sha256>.png.
// Every call is recorded with its argv and stdin. sleep-<command> holds seconds
// to wait first. A write prints refuse-<command>.json when present and exits 1;
// otherwise it moves the cursor, so the model reads the board again, and prints
// reply-<command>.json when present.
const QByteArray kFixture = R"(#!/usr/bin/env python3
import glob, json, os, pathlib, sys, time
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
elif command == 'show':
    uuid = json.loads(data)['task']
    found = None
    for name in sorted(glob.glob(str(root / 'board-*.json'))):
        for task in json.loads(pathlib.Path(name).read_text()).get('tasks', []):
            if task['uuid'] == uuid:
                found = dict(task, prompt='Prompt of ' + task['title'], attachments=[], journal=[], related=[], resume_stage='')
    if found is None:
        print(json.dumps({'ok': False, 'error': {'code': 'unknown_task', 'message': 'no task ' + uuid}}))
        sys.exit(1)
    found.update(json.loads(read('show-' + uuid + '.json', '{}')))
    reply({'ok': True, 'task': found})
elif command == 'attachment':
    request = json.loads(data)
    image = root / ('image-' + request['sha256'] + '.png')
    if not image.exists():
        print(json.dumps({'ok': False, 'error': {'code': 'attachment_unavailable', 'message': 'the image is not on this machine'}}))
        sys.exit(1)
    reply({'ok': True, 'path': str(image), 'name': 'shot.png', 'mime': 'image/png', 'bytes': image.stat().st_size})
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
        QJsonObject{{"action", "claim"}, {"from", QJsonArray{"Ready"}}, {"to", "Doing"}, {"actors", QJsonArray{"human", "autopilot"}}},
        QJsonObject{{"action", "unclaim"}, {"from", QJsonArray{"Doing"}}, {"to", "Ready"}, {"actors", QJsonArray{"human", "autopilot"}}},
        QJsonObject{{"action", "reopen"}, {"from", QJsonArray{"Shipped"}}, {"to", "Ready"}, {"actors", QJsonArray{"human"}}},
        // An agent's move and a move that needs a field are not a person's drag.
        QJsonObject{{"action", "move"}, {"from", QJsonArray{"Doing"}}, {"to", "Later"}, {"actors", QJsonArray{"agent"}}},
        QJsonObject{{"action", "move"}, {"from", QJsonArray{"Inbox"}}, {"to", "Later"}, {"actors", QJsonArray{"human"}}, {"requires", QJsonArray{"reason"}}},
        QJsonObject{{"action", "done"}, {"from", QJsonArray{"Check"}}, {"to", "Shipped"}, {"actors", QJsonArray{"human"}}},
        QJsonObject{{"action", "move"}, {"from", QJsonArray{"Check"}}, {"to", "Later"}, {"actors", QJsonArray{"human"}}}};
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
    t5["flags"] = QJsonArray{"delivery_uncertain"};
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
        model->setEnabled(true);   // Windows starts disabled
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
    // The `@host` of each call of the command, "" for a local one.
    QStringList hosts(const QString &words) const
    {
        QStringList out;
        QFile file(dir.filePath("calls"));
        if (!file.open(QIODevice::ReadOnly)) return out;
        for (const auto &line : file.readAll().split('\n')) {
            auto argv = QJsonDocument::fromJson(line).object()["argv"].toVariant().toStringList();
            const QString host = !argv.isEmpty() && argv.first().startsWith('@') ? argv.takeFirst().mid(1) : QString();
            if (argv.mid(1, argv.size() - 2).join(' ') == words) out << host;
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

// The sessions a card can go to: the window passes every session it knows.
QList<TasksView::Recipient> recipients()
{
    const auto make = [](const QString &project, const QString &machine, const QString &name, const QString &status) {
        TasksView::Recipient recipient;
        recipient.key = machine + '\n' + name; recipient.project = project; recipient.session = {machine, name};
        recipient.status = status;
        return recipient;
    };
    return {make("demo", "m1", "codex/demo/one", "Working"), make("demo", "m1", "claude/demo/review", "Ready"),
            make("demo", "m2", "codex/demo/two", "Ready"), make("other", "m1", "codex/other/one", "Ready"),
            make("demo", "", "kimi/demo/far", "Ready")};
}

// A view on the demo board with codex/demo/one selected, waiting for the cards.
struct Board {
    Fixture f;
    std::unique_ptr<TasksView> view;
    bool open(const QSize &size = {1500, 820})
    {
        if (!f.setUp()) return false;
        view = std::make_unique<TasksView>(f.model.get());
        view->resize(size);
        view->setProjects(organization().groups());
        view->setSession(selection("demo", "m1", "codex/demo/one"));
        view->setRecipients(recipients());
        view->show();
        return QTest::qWaitFor([this] { return card(*view, "t1") != nullptr; });
    }
    QWidget *panel() const { return view->findChild<QWidget *>("taskPanel"); }
    template <typename T> T *find(const char *name) const { return view->findChild<T *>(name); }
    // Opens a card and waits until its details are shown.
    bool show(const QString &uuid)
    {
        const int before = f.calls("show").size();
        view->openTask(uuid);
        const bool shown = QTest::qWaitFor([&] { return f.calls("show").size() > before && find<QWidget>("taskPanel")->property("task").toString() == uuid
                                                       && find<QWidget>("taskPanel")->property("loaded").toBool(); });
        QCoreApplication::processEvents();   // menus fill on the next turn
        return shown;
    }
    QLabel *message() const { return find<QLabel>("tasksMessage"); }
    QPushButton *anyway() const { return find<QPushButton>("tasksSendAnyway"); }
};

// The menu action whose data (a session key or a stage) is `data`.
QAction *action(QMenu *menu, const QString &data)
{
    if (!menu) return nullptr;
    for (auto *candidate : menu->actions()) if (candidate->data().toString() == data) return candidate;
    return nullptr;
}
QStringList actionData(QMenu *menu)
{
    QStringList out;
    if (menu) for (auto *candidate : menu->actions()) if (!candidate->isSeparator()) out << candidate->data().toString();
    return out;
}

// Answers the next confirmation dialog, also one that opens later, and keeps
// its text: "" when none came within three seconds.
struct Confirmations {
    QStringList texts;
    void answer(bool accept)
    {
        auto *timer = new QTimer(qApp);
        auto tries = std::make_shared<int>(0);
        QObject::connect(timer, &QTimer::timeout, timer, [this, accept, timer, tries] {
            auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
            if (!box && ++*tries < 300) return;
            timer->stop();
            timer->deleteLater();
            texts << (box ? box->text() : QString());
            if (!box) return;
            if (box->objectName() != "tasksConfirm") texts.last() = "unexpected dialog " + box->objectName();
            auto *button = accept ? box->findChild<QAbstractButton *>("tasksConfirmAccept") : box->button(QMessageBox::Cancel);
            if (button) button->click(); else box->reject();
        });
        timer->start(10);
    }
};

// A write's request without its request_id, which must be a UUID.
QJsonObject plain(QJsonObject request)
{
    if (QUuid(request.take("request_id").toString()).isNull()) request.insert("request_id", "missing or not a UUID");
    return request;
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
    void panelShowsTheTask();
    void panelEditsRunTheirCommands();
    void panelActionsRunTheirCommands();
    void confirmableRefusalsRoundTrip();
    void destructiveActionsAskFirst();
    void uncertainDeliveriesOfferResolve();
    void cardMenuMovesByKeyboard();
    void promptDraftSurvivesChanges();
    void panelSharesTheWidth();
    void imagesOpenOnlyAsPictures();
    void imagesAddPasteAndRemove();
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
    // Opened images are copied to the cache: the test cache, not the user's.
    QStandardPaths::setTestModeEnabled(true);
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

    // A machine that is not known yet: nothing is highlighted, counted or sent,
    // on this computer or another, since the name may be a session elsewhere
    // (t10 is queued for the same name on m2).
    QTRY_VERIFY(next->isEnabled());
    TasksView::Selection remote = selection("demo", "", "codex/demo/one"); remote.host = "mac";
    view.setSession(remote);
    QVERIFY(!next->isEnabled());
    view.setSession(selection("demo", "", "codex/demo/one"));
    QVERIFY(!next->isEnabled());
    QCOMPARE(next->toolTip(), QString("Overmind does not know this session's computer yet."));
    QVERIFY(strip->isVisible());
    QVERIFY(!view.findChild<QLabel *>("tasksSessionQueue")->isVisible());
    QCOMPARE(highlighted(view), QStringList());
    QCOMPARE(view.findChild<QPushButton *>("tasksDecision")->property("highlighted").toBool(), false);
    // Once known, the count and the highlights return.
    view.setSession(selection("demo", "m1", "codex/demo/one"));
    QVERIFY(view.findChild<QLabel *>("tasksSessionQueue")->isVisible());
    QCOMPARE(view.findChild<QLabel *>("tasksSessionQueue")->text(), QString("This session's queue: 2"));
    QCOMPARE(highlighted(view), (QStringList{"t2", "t4", "t5"}));

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
        // Wide enough for a board next to the card panel.
        TasksView view(f.model.get()); view.resize(1300, 600);
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

// The card panel opens on the right with what `show` says about the task: the
// prompt, images fetched through `attachment`, labels, priority, relations and
// the journal rendered as Markdown.
void TestTasksView::panelShowsTheTask()
{
    Board b; QVERIFY(b.open());
    QImage image(48, 32, QImage::Format_RGB32); image.fill(Qt::red);
    QVERIFY(image.save(b.f.dir.filePath("image-aaaa.png")));
    const auto person = QJsonObject{{"kind", "human"}, {"machine", "m1"}};
    const QJsonArray journal{
        QJsonObject{{"marker", "[claim]"}, {"text", ""}, {"at", 1791266000}, {"actor", person}, {"event", "e1"}},
        QJsonObject{{"marker", "[sent?]"}, {"text", "the session did not confirm the input"}, {"at", 1791266010}, {"actor", person}, {"event", "e2"}},
        QJsonObject{{"marker", "[comment]"}, {"text", "Use the **fast** path"}, {"at", 1791266020},
                    {"actor", QJsonObject{{"kind", "agent"}, {"machine", "m1"}, {"session", "codex/demo/one"}}}, {"event", "e3"}}};
    QVERIFY(b.f.put("show-t5.json", compact({{"prompt", "Build the thing\n\nwith care"}, {"journal", journal},
        {"attachments", QJsonArray{QJsonObject{{"name", "shot.png"}, {"mime", "image/png"}, {"bytes", 100}, {"sha256", "aaaa"}}}},
        {"parent", "t1"}, {"follows", QJsonArray{"t3"}}, {"related", QJsonArray{"t6"}}, {"labels", QJsonArray{"ui"}}})));
    auto *panel = b.panel();
    QVERIFY(panel); QVERIFY(!panel->isVisible());
    QVERIFY(b.show("t5"));
    QVERIFY(panel->isVisible());
    QCOMPARE(b.f.calls("show").last(), (QJsonObject{{"task", "t5"}}));
    QCOMPARE(b.find<QLabel>("taskPanelRef")->text(), QString("om-t5"));
    QVERIFY(b.find<QLabel>("taskPanelStage")->text().contains("Doing"));
    QCOMPARE(b.find<QLineEdit>("taskTitle")->text(), QString("Being built"));
    QCOMPARE(b.find<QPlainTextEdit>("taskPrompt")->toPlainText(), QString("Build the thing\n\nwith care"));
    QCOMPARE(b.find<QComboBox>("taskPriority")->currentData().toInt(), 2);
    // The journal, oldest first, as Markdown, each entry with its author.
    QStringList texts;
    for (auto *text : panel->findChildren<MarkdownText *>("taskJournalText")) texts << text->toPlainText().trimmed();
    QVERIFY2(texts.contains("Use the fast path"), qPrintable(texts.join(" | ")));
    QStringList headers;
    for (auto *header : panel->findChildren<QLabel *>("taskJournalHeader")) headers << header->text();
    QCOMPARE(headers.size(), 3);
    QVERIFY2(headers.first().contains("[claim]") && headers.last().contains("[comment]") && headers.last().contains("codex/demo/one"),
             qPrintable(headers.join(" | ")));
    // The image comes through `attachment`, and a click opens a copy named after it.
    QTRY_COMPARE(b.f.calls("attachment").size(), 1);
    QCOMPARE(b.f.calls("attachment").first(), (QJsonObject{{"task", "t5"}, {"sha256", "aaaa"}}));
    auto *thumbnail = b.find<QToolButton>("taskImage");
    QVERIFY(thumbnail);
    QTRY_VERIFY(!thumbnail->icon().isNull());
    QUrl opened;
    qobject_cast<TaskPanel *>(panel)->openUrl = [&](const QUrl &url) { opened = url; return true; };
    thumbnail->click();
    QVERIFY(opened.isLocalFile());
    QVERIFY(opened.toLocalFile().endsWith("/shot.png"));
    QVERIFY(QFile::exists(opened.toLocalFile()));
    QFile::remove(opened.toLocalFile());
    // Labels and relations by ref.
    QStringList labels;
    for (auto *chip : panel->findChildren<QLabel *>("taskLabel")) labels << chip->text();
    QCOMPARE(labels, QStringList{"ui"});
    QStringList relations;
    for (auto *button : panel->findChildren<QPushButton *>("taskRelation")) relations << button->property("relation").toString() + ":" + button->text();
    QCOMPARE(relations, (QStringList{"parent:om-t1", "follows:om-t3", "related:om-t6"}));
    // The unconfirmed delivery names its reason.
    auto *delivery = b.find<QFrame>("taskDelivery");
    QVERIFY(delivery->isVisible());
    QVERIFY(b.find<QLabel>("taskDeliveryText")->text().contains("the session did not confirm the input"));
    // A relation opens its task.
    for (auto *button : panel->findChildren<QPushButton *>("taskRelation")) if (button->text() == "om-t1") button->click();
    QCOMPARE(b.view->openedTask(), QString("t1"));
    QTRY_COMPARE(b.find<QLineEdit>("taskTitle")->text(), QString("Write the parser"));
    QVERIFY(!delivery->isVisible());
    // Closing the panel closes the card.
    b.find<QAbstractButton>("taskPanelClose")->click();
    QVERIFY(!panel->isVisible());
    QVERIFY(b.view->openedTask().isEmpty());
    QVERIFY(!card(*b.view, "t1")->isSelected());
}

// Edits run edit, label, pin, unpin and link with a fresh request_id each.
void TestTasksView::panelEditsRunTheirCommands()
{
    Board b; QVERIFY(b.open());
    QVERIFY(b.f.put("show-t1.json", compact({{"follows", QJsonArray{"t3"}}})));
    QVERIFY(b.show("t1"));
    // The title: Enter saves it, once.
    auto *title = b.find<QLineEdit>("taskTitle");
    title->setFocus();
    title->selectAll();
    QTest::keyClicks(title, "Write the lexer");
    QTest::keyClick(title, Qt::Key_Return);
    QTRY_COMPARE(b.f.calls("edit").size(), 1);
    QCOMPARE(plain(b.f.calls("edit").last()), (QJsonObject{{"task", "t1"}, {"title", "Write the lexer"}}));
    // The prompt: Save shows once it changed.
    auto *prompt = b.find<QPlainTextEdit>("taskPrompt");
    auto *save = b.find<QPushButton>("taskPromptSave");
    QVERIFY(!save->isVisible());
    prompt->setPlainText("Write a parser for the config format");
    QVERIFY(save->isVisible() && save->isEnabled());
    save->click();
    QTRY_COMPARE(b.f.calls("edit").size(), 2);
    QCOMPARE(plain(b.f.calls("edit").last()), (QJsonObject{{"task", "t1"}, {"prompt", "Write a parser for the config format"}}));
    QTRY_VERIFY(!save->isVisible());
    // Priority.
    auto *priority = b.find<QComboBox>("taskPriority");
    priority->setCurrentIndex(priority->findData(0));
    emit priority->activated(priority->currentIndex());
    QTRY_COMPARE(b.f.calls("edit").size(), 3);
    QCOMPARE(plain(b.f.calls("edit").last()), (QJsonObject{{"task", "t1"}, {"priority", 0}}));
    // Labels: Enter adds one, a chip's button removes it.
    auto *add = b.find<QLineEdit>("taskLabelAdd");
    QTest::keyClicks(add, "docs");
    QTest::keyClick(add, Qt::Key_Return);
    QTRY_COMPARE(b.f.calls("label").size(), 1);
    QCOMPARE(plain(b.f.calls("label").last()), (QJsonObject{{"task", "t1"}, {"label", "docs"}, {"present", true}}));
    QVERIFY(add->text().isEmpty());
    QToolButton *removeLabel = nullptr;
    for (auto *button : b.panel()->findChildren<QToolButton *>("taskLabelRemove")) if (button->property("label").toString() == "ui") removeLabel = button;
    QVERIFY(removeLabel);
    removeLabel->click();
    QTRY_COMPARE(b.f.calls("label").size(), 2);
    QCOMPARE(plain(b.f.calls("label").last()), (QJsonObject{{"task", "t1"}, {"label", "ui"}, {"present", false}}));
    // Pinning to a session of the project; sessions of other projects are not offered.
    auto *pin = b.find<QPushButton>("taskPin");
    QCOMPARE(actionData(pin->menu()), (QStringList{"m1\ncodex/demo/one", "m1\nclaude/demo/review", "m2\ncodex/demo/two", "\nkimi/demo/far"}));
    QVERIFY(!action(pin->menu(), "\nkimi/demo/far")->isEnabled());
    QVERIFY(!b.find<QPushButton>("taskUnpin")->isVisible());
    action(pin->menu(), "m2\ncodex/demo/two")->trigger();
    QTRY_COMPARE(b.f.calls("pin").size(), 1);
    QCOMPARE(plain(b.f.calls("pin").last()), (QJsonObject{{"task", "t1"}, {"session", session("m2", "codex/demo/two")}}));
    // Relations: added by ref, removed with the chip's button.
    auto *kind = b.find<QComboBox>("taskRelationKind");
    kind->setCurrentIndex(kind->findData("related"));
    QTest::keyClicks(b.find<QLineEdit>("taskRelationTarget"), "om-t6");
    b.find<QPushButton>("taskRelationAdd")->click();
    QTRY_COMPARE(b.f.calls("link").size(), 1);
    QCOMPARE(plain(b.f.calls("link").last()), (QJsonObject{{"task", "t1"}, {"relation", "related"}, {"target", "om-t6"}, {"present", true}}));
    QToolButton *removeRelation = nullptr;
    for (auto *button : b.panel()->findChildren<QToolButton *>("taskRelationRemove"))
        if (button->property("target").toString() == "t3") removeRelation = button;
    QVERIFY(removeRelation);
    removeRelation->click();
    QTRY_COMPARE(b.f.calls("link").size(), 2);
    QCOMPARE(plain(b.f.calls("link").last()), (QJsonObject{{"task", "t1"}, {"relation", "follows"}, {"target", "t3"}, {"present", false}}));
    // Unpinning a pinned task.
    QVERIFY(b.show("t2"));
    QVERIFY(b.find<QLabel>("taskPinned")->text().contains("codex/demo/one"));
    auto *unpin = b.find<QPushButton>("taskUnpin");
    QVERIFY(unpin->isVisible());
    unpin->click();
    QTRY_COMPARE(b.f.calls("unpin").size(), 1);
    QCOMPARE(plain(b.f.calls("unpin").last()), (QJsonObject{{"task", "t2"}}));
    // Leaving the title field after Enter wrote nothing more.
    QCOMPARE(b.f.calls("edit").size(), 3);
    // A refused title stays in the field, also when the board changes; Enter tries it again.
    QVERIFY(b.show("t3"));
    QVERIFY(b.f.put("refuse-edit.json", R"({"ok":false,"error":{"code":"invalid_field","message":"the title is not accepted","field":"title","state":"invalid"}})"));
    title->setFocus();
    title->selectAll();
    QTest::keyClicks(title, "Pool task");
    QTest::keyClick(title, Qt::Key_Return);
    QTRY_COMPARE(b.message()->text(), QString("the title is not accepted"));
    QCOMPARE(title->text(), QString("Pool task"));
    auto board = demoBoard();
    auto tasks = board["tasks"].toArray();
    for (int i = 0; i < tasks.size(); ++i) {
        auto task = tasks[i].toObject();
        if (task["uuid"] == "t3") { task["title"] = "From the pool, renamed"; tasks[i] = task; }
    }
    board["tasks"] = tasks;
    QVERIFY(b.f.put("board-demo.json", compact(board)));
    QVERIFY(b.f.put("rev", "100"));
    QTRY_COMPARE(card(*b.view, "t3")->data(TaskRoles::Title).toString(), QString("From the pool, renamed"));
    QTest::qWait(100);
    QCOMPARE(title->text(), QString("Pool task"));
    QVERIFY(QFile::remove(b.f.dir.filePath("refuse-edit.json")));
    title->setFocus();
    QTest::keyClick(title, Qt::Key_Return);
    QTRY_COMPARE(b.f.calls("edit").size(), 5);
    QCOMPARE(plain(b.f.calls("edit").last()), (QJsonObject{{"task", "t3"}, {"title", "Pool task"}}));
}

// Actions run send, review-send, rework-send, answer, move, done and
// resolve-delivery, and each shows only where it applies.
void TestTasksView::panelActionsRunTheirCommands()
{
    Board b; QVERIFY(b.open());
    // A ready task goes to a session of the project, chosen with its state.
    QVERIFY(b.show("t3"));
    auto *send = b.find<QPushButton>("taskSend");
    QVERIFY(send->isVisible() && send->isEnabled());
    QVERIFY(!b.find<QPushButton>("taskReviewSend")->isVisible());
    QVERIFY(!b.find<QPushButton>("taskRework")->isVisible());
    QVERIFY(!b.find<QWidget>("taskAnswerBox")->isVisible());
    QCOMPARE(actionData(send->menu()), (QStringList{"m1\ncodex/demo/one", "m1\nclaude/demo/review", "m2\ncodex/demo/two", "\nkimi/demo/far"}));
    QVERIFY(action(send->menu(), "m1\ncodex/demo/one")->text().contains("Working"));
    QVERIFY(action(send->menu(), "m2\ncodex/demo/two")->text().contains("Ready"));
    QVERIFY(!action(send->menu(), "\nkimi/demo/far")->isEnabled());
    action(send->menu(), "m2\ncodex/demo/two")->trigger();
    QTRY_COMPARE(b.f.calls("send").size(), 1);
    QCOMPARE(plain(b.f.calls("send").last()), (QJsonObject{{"task", "t3"}, {"session", session("m2", "codex/demo/two")}}));
    QTRY_COMPARE(b.message()->text(), QString("Sent om-t3 to codex/demo/two."));
    // Review goes to a session other than the one that did the work.
    QVERIFY(b.f.put("show-t6.json", compact({{"assignee", session("m1", "codex/demo/one")}})));
    QVERIFY(b.show("t6"));
    auto *review = b.find<QPushButton>("taskReviewSend");
    QVERIFY(review->isVisible());
    QVERIFY(!send->isVisible());
    QVERIFY(!action(review->menu(), "m1\ncodex/demo/one")->isEnabled());
    action(review->menu(), "m1\nclaude/demo/review")->trigger();
    QTRY_COMPARE(b.f.calls("review-send").size(), 1);
    QCOMPARE(plain(b.f.calls("review-send").last()), (QJsonObject{{"task", "t6"}, {"session", session("m1", "claude/demo/review")}}));
    QTRY_COMPARE(b.message()->text(), QString("Sent om-t6 to claude/demo/review for review."));
    auto *done = b.find<QPushButton>("taskDone");
    QVERIFY(done->isVisible());
    done->click();
    QTRY_COMPARE(b.f.calls("done").size(), 1);
    QCOMPARE(plain(b.f.calls("done").last()), (QJsonObject{{"task", "t6"}}));
    // Rework after a failed review goes back to the session that did the work.
    QVERIFY(b.show("t11"));
    auto *rework = b.find<QPushButton>("taskRework");
    QVERIFY(rework->isVisible());
    QVERIFY(!done->isVisible());
    rework->click();
    QTRY_COMPARE(b.f.calls("rework-send").size(), 1);
    QCOMPARE(plain(b.f.calls("rework-send").last()), (QJsonObject{{"task", "t11"}}));
    QTRY_COMPARE(b.message()->text(), QString("Sent the rework of om-t11 to codex/demo/two."));
    // Your Call: the question and the answer.
    const QJsonObject question{{"marker", "[needs-human]"}, {"text", "Postgres or **SQLite**?"}, {"at", 1791266000},
                               {"actor", QJsonObject{{"kind", "agent"}, {"machine", "m1"}, {"session", "codex/demo/one"}}}, {"event", "e9"}};
    QVERIFY(b.f.put("show-t7.json", compact({{"journal", QJsonArray{question}}})));
    QVERIFY(b.show("t7"));
    QVERIFY(b.find<QWidget>("taskAnswerBox")->isVisible());
    QCOMPARE(b.find<MarkdownText>("taskQuestion")->toPlainText().trimmed(), QString("Postgres or SQLite?"));
    auto *answer = b.find<QPlainTextEdit>("taskAnswer");
    auto *reply = b.find<QPushButton>("taskAnswerSend");
    QVERIFY(!reply->isEnabled());
    answer->setPlainText("SQLite");
    QVERIFY(reply->isEnabled());
    reply->click();
    QTRY_COMPARE(b.f.calls("answer").size(), 1);
    QCOMPARE(plain(b.f.calls("answer").last()), (QJsonObject{{"task", "t7"}, {"answer", "SQLite"}}));
    QTRY_COMPARE(b.message()->text(), QString("Sent your answer on om-t7 to codex/demo/one."));
    QVERIFY(answer->toPlainText().isEmpty());
    // Move to…: the stages a person may move the task to.
    QVERIFY(b.show("t1"));
    auto *move = b.find<QPushButton>("taskMove");
    QCOMPARE(actionData(move->menu()), QStringList{"Ready"});
    action(move->menu(), "Ready")->trigger();
    QTRY_COMPARE(b.f.calls("move").size(), 1);
    QCOMPARE(plain(b.f.calls("move").last()), (QJsonObject{{"task", "t1"}, {"to", "Ready"}}));
    // A person resolves an unconfirmed delivery.
    QVERIFY(b.show("t5"));
    b.find<QPushButton>("taskDelivered")->click();
    QTRY_COMPARE(b.f.calls("resolve-delivery").size(), 1);
    QCOMPARE(plain(b.f.calls("resolve-delivery").last()), (QJsonObject{{"task", "t5"}, {"delivered", true}}));
    QTRY_COMPARE(b.message()->text(), QString("Recorded that om-t5 was delivered."));
}

// A confirmable refusal offers Send anyway, which repeats the request with the
// same request_id and the refusal's code added to confirm.
void TestTasksView::confirmableRefusalsRoundTrip()
{
    Board b; QVERIFY(b.open());
    QVERIFY(b.show("t3"));
    auto *send = b.find<QPushButton>("taskSend");
    QVERIFY(b.f.put("refuse-send.json", R"json({"ok":false,"error":{"code":"wip_exceeded","message":"codex/demo/one already holds 1 active task(s)","field":"confirm","state":"missing"}})json"));
    action(send->menu(), "m1\ncodex/demo/one")->trigger();
    QTRY_VERIFY(b.anyway()->isVisible());
    QCOMPARE(b.message()->text(), QString("codex/demo/one already holds 1 active task(s)"));
    QCOMPARE(b.anyway()->text(), QString("Send om-t3 to codex/demo/one anyway"));
    // The offer names its task and session, so another selected session keeps it.
    b.view->setSession(selection("demo", "m1", "claude/demo/review"));
    QVERIFY(b.anyway()->isVisible());
    // A second confirmation of the same request adds its code.
    QVERIFY(b.f.put("refuse-send.json", R"({"ok":false,"error":{"code":"agent_busy","message":"codex/demo/one is working; the text will wait in its queue","field":"confirm","state":"missing"}})"));
    b.anyway()->click();
    QTRY_COMPARE(b.f.calls("send").size(), 2);
    QTRY_VERIFY(b.anyway()->isVisible() && b.message()->text().contains("is working"));
    QVERIFY(QFile::remove(b.f.dir.filePath("refuse-send.json")));
    b.anyway()->click();
    QTRY_COMPARE(b.f.calls("send").size(), 3);
    const auto calls = b.f.calls("send");
    QCOMPARE(calls[1]["request_id"], calls[0]["request_id"]);
    QCOMPARE(calls[2]["request_id"], calls[0]["request_id"]);
    QVERIFY(!calls[0].contains("confirm"));
    QCOMPARE(calls[1]["confirm"].toArray(), QJsonArray{"wip_exceeded"});
    QCOMPARE(calls[2]["confirm"].toArray(), (QJsonArray{"wip_exceeded", "agent_busy"}));
    QCOMPARE(calls[2]["session"].toObject(), session("m1", "codex/demo/one"));
    QCOMPARE(calls[2]["task"].toString(), QString("t3"));
    QTRY_COMPARE(b.message()->text(), QString("Sent om-t3 to codex/demo/one."));
    QVERIFY(!b.anyway()->isVisible());
    // A refusal that is not a confirmation only shows its message.
    QVERIFY(b.f.put("refuse-send.json", R"({"ok":false,"error":{"code":"session_unavailable","message":"codex/demo/two is paused","field":"session","state":"invalid"}})"));
    QTRY_VERIFY(send->isEnabled());
    action(send->menu(), "m2\ncodex/demo/two")->trigger();
    QTRY_COMPARE(b.message()->text(), QString("codex/demo/two is paused"));
    QVERIFY(b.message()->property("error").toBool());
    QVERIFY(!b.anyway()->isVisible());
    // A review: the same round trip with its own label.
    QVERIFY(b.f.put("refuse-review-send.json", R"({"ok":false,"error":{"code":"agent_busy","message":"claude/demo/review is working","field":"confirm","state":"missing"}})"));
    QVERIFY(b.show("t6"));
    action(b.find<QPushButton>("taskReviewSend")->menu(), "m1\nclaude/demo/review")->trigger();
    QTRY_VERIFY(b.anyway()->isVisible());
    QCOMPARE(b.anyway()->text(), QString("Send om-t6 to claude/demo/review for review anyway"));
    QVERIFY(QFile::remove(b.f.dir.filePath("refuse-review-send.json")));
    b.anyway()->click();
    QTRY_COMPARE(b.f.calls("review-send").size(), 2);
    QCOMPARE(b.f.calls("review-send")[1]["request_id"], b.f.calls("review-send")[0]["request_id"]);
    QCOMPARE(b.f.calls("review-send")[1]["confirm"].toArray(), QJsonArray{"agent_busy"});
    // An answer that did not arrive is recorded: sending it again repeats the request.
    QVERIFY(b.show("t7"));
    QVERIFY(b.f.put("refuse-answer.json", R"({"ok":false,"error":{"code":"not_delivered","message":"not delivered: the session is paused; the answer is recorded"}})"));
    b.find<QPlainTextEdit>("taskAnswer")->setPlainText("SQLite");
    b.find<QPushButton>("taskAnswerSend")->click();
    QTRY_VERIFY(b.anyway()->isVisible());
    QCOMPARE(b.anyway()->text(), QString("Send the answer again"));
    QCOMPARE(b.find<QPlainTextEdit>("taskAnswer")->toPlainText(), QString("SQLite"));
    QVERIFY(QFile::remove(b.f.dir.filePath("refuse-answer.json")));
    b.anyway()->click();
    QTRY_COMPARE(b.f.calls("answer").size(), 2);
    QCOMPARE(b.f.calls("answer")[1], b.f.calls("answer")[0]);
    QTRY_VERIFY(b.find<QPlainTextEdit>("taskAnswer")->toPlainText().isEmpty());
}

// Delete, taking a running task back, reopening, Not delivered and discarding
// a changed prompt ask first; Cancel writes nothing.
void TestTasksView::destructiveActionsAskFirst()
{
    Board b; QVERIFY(b.open());
    Confirmations dialogs;
    QVERIFY(b.show("t1"));
    auto *remove = b.find<QPushButton>("taskDelete");
    dialogs.answer(false);
    remove->click();
    QCOMPARE(dialogs.texts.size(), 1);
    QVERIFY2(dialogs.texts.last().contains("om-t1"), qPrintable(dialogs.texts.last()));
    QTest::qWait(150);
    QVERIFY(b.f.calls("delete").isEmpty());
    dialogs.answer(true);
    remove->click();
    QTRY_COMPARE(b.f.calls("delete").size(), 1);
    QCOMPARE(plain(b.f.calls("delete").last()), (QJsonObject{{"task", "t1"}}));
    QTRY_VERIFY(!b.panel()->isVisible());
    QVERIFY(b.view->openedTask().isEmpty());
    // Taking a running task back from its session.
    QVERIFY(b.show("t5"));
    auto *move = b.find<QPushButton>("taskMove");
    QCOMPARE(actionData(move->menu()), QStringList{"Ready"});
    dialogs.answer(false);
    action(move->menu(), "Ready")->trigger();
    QTRY_COMPARE(dialogs.texts.size(), 3);
    QVERIFY2(dialogs.texts.last().contains("om-t5") && dialogs.texts.last().contains("codex/demo/one"), qPrintable(dialogs.texts.last()));
    QTest::qWait(150);
    QVERIFY(b.f.calls("move").isEmpty());
    dialogs.answer(true);
    action(move->menu(), "Ready")->trigger();
    QTRY_COMPARE(b.f.calls("move").size(), 1);
    QCOMPARE(plain(b.f.calls("move").last()), (QJsonObject{{"task", "t5"}, {"to", "Ready"}}));
    // Dragging it there asks too.
    dialogs.answer(false);
    auto *ready = list(*b.view, "Ready");
    drop(ready->viewport(), "t5", QPoint(20, ready->viewport()->height() - 10));
    QTRY_COMPARE(dialogs.texts.size(), 5);
    QVERIFY(!dialogs.texts.last().isEmpty());
    QTest::qWait(150);
    QCOMPARE(b.f.calls("move").size(), 1);
    // Reopening a finished task.
    QVERIFY(b.show("t8"));
    QCOMPARE(actionData(move->menu()), QStringList{"Ready"});
    dialogs.answer(true);
    action(move->menu(), "Ready")->trigger();
    QTRY_COMPARE(b.f.calls("move").size(), 2);
    QVERIFY2(dialogs.texts.last().contains("Reopen om-t8"), qPrintable(dialogs.texts.last()));
    QCOMPARE(plain(b.f.calls("move").last()), (QJsonObject{{"task", "t8"}, {"to", "Ready"}}));
    // Moving a task out of review while a session reviews it; Done does not ask.
    QVERIFY(b.show("t6"));
    QCOMPARE(actionData(move->menu()), (QStringList{"Shipped", "Later"}));
    dialogs.answer(false);
    action(move->menu(), "Later")->trigger();
    QTRY_COMPARE(dialogs.texts.size(), 7);
    QVERIFY2(dialogs.texts.last().contains("om-t6") && dialogs.texts.last().contains("claude/demo/review"), qPrintable(dialogs.texts.last()));
    QTest::qWait(150);
    QCOMPARE(b.f.calls("move").size(), 2);
    // Not delivered takes the send back.
    QVERIFY(b.show("t5"));
    dialogs.answer(false);
    b.find<QPushButton>("taskNotDelivered")->click();
    QTest::qWait(150);
    QVERIFY(b.f.calls("resolve-delivery").isEmpty());
    dialogs.answer(true);
    b.find<QPushButton>("taskNotDelivered")->click();
    QTRY_COMPARE(b.f.calls("resolve-delivery").size(), 1);
    QCOMPARE(plain(b.f.calls("resolve-delivery").last()), (QJsonObject{{"task", "t5"}, {"delivered", false}}));
    // Discarding a changed prompt.
    QVERIFY(b.show("t3"));
    auto *prompt = b.find<QPlainTextEdit>("taskPrompt");
    prompt->setPlainText("A better prompt");
    auto *discard = b.find<QPushButton>("taskPromptDiscard");
    QVERIFY(discard->isVisible());
    dialogs.answer(false);
    discard->click();
    QCOMPARE(prompt->toPlainText(), QString("A better prompt"));
    dialogs.answer(true);
    discard->click();
    QCOMPARE(prompt->toPlainText(), QString("Prompt of From the pool"));
    QVERIFY(!discard->isVisible());
    QVERIFY(b.f.calls("edit").isEmpty());
    QCOMPARE(dialogs.texts.size(), 11);
    for (const auto &text : std::as_const(dialogs.texts)) QVERIFY2(!text.isEmpty() && !text.startsWith("unexpected"), qPrintable(text));
}

// A delivery that may not have arrived is a warning with Resolve delivery,
// never a retry; the same for a deadline.
void TestTasksView::uncertainDeliveriesOfferResolve()
{
    Board b; QVERIFY(b.open());
    auto *resolve = b.find<QPushButton>("tasksResolveDelivery");
    QVERIFY(resolve);
    QVERIFY(!resolve->isVisible());
    QVERIFY(b.f.put("reply-send.json", R"({"ok":true,"delivery":"uncertain","reason":"the input receipt is missing","task":{"uuid":"t2"}})"));
    auto *next = b.find<QPushButton>("tasksSendNext");
    QTRY_VERIFY(next->isEnabled());
    next->click();
    QTRY_VERIFY(resolve->isVisible());
    QVERIFY(!b.anyway()->isVisible());
    QVERIFY(b.message()->property("warning").toBool());
    QTest::qWait(200);
    QCOMPARE(b.f.calls("send").size(), 1);
    resolve->click();
    QCOMPARE(b.view->openedTask(), QString("t2"));
    QVERIFY(b.panel()->isVisible());
    QVERIFY(!b.message()->isVisible());
    // From the panel: a review request that may not have arrived.
    QVERIFY(b.f.put("reply-review-send.json", R"({"ok":true,"delivery":"uncertain","reason":"the session did not confirm the input","task":{"uuid":"t6"}})"));
    QVERIFY(b.show("t6"));
    action(b.find<QPushButton>("taskReviewSend")->menu(), "m1\ncodex/demo/one")->trigger();
    QTRY_VERIFY(resolve->isVisible());
    QCOMPARE(b.message()->text(), QString("The review request for om-t6 may not have reached codex/demo/one: the session did not confirm the input. "
                                          "Check the task before sending it again."));
    // An edit meanwhile leaves the warning and its offer in place.
    QTest::keyClicks(b.find<QLineEdit>("taskLabelAdd"), "later");
    QTest::keyClick(b.find<QLineEdit>("taskLabelAdd"), Qt::Key_Return);
    QTRY_COMPARE(b.f.calls("label").size(), 1);
    QTest::qWait(200);
    QVERIFY(resolve->isVisible());
    QVERIFY(b.message()->text().startsWith("The review request for om-t6"));
    // A deadline: it may have been sent. The panel cannot send it again meanwhile.
    QVERIFY(b.f.put("sleep-send", "1.5"));
    auto timeouts = b.f.model->timeouts(); timeouts.deliveryLocal = 300; b.f.model->setTimeouts(timeouts);
    QVERIFY(b.show("t3"));
    auto *send = b.find<QPushButton>("taskSend");
    action(send->menu(), "m2\ncodex/demo/two")->trigger();
    QVERIFY(!send->isEnabled());
    QTRY_VERIFY_WITH_TIMEOUT(b.message()->isVisible() && b.message()->text().contains("did not answer"), 5000);
    QCOMPARE(b.message()->text(), QString("The task board did not answer in time: om-t3 may have been sent to codex/demo/two. "
                                          "Check the task before sending it again."));
    QVERIFY(resolve->isVisible());
    QVERIFY(!b.anyway()->isVisible());
    QTest::qWait(1500);
    QCOMPARE(b.f.calls("send").size(), 2);
}

// The keyboard moves a card: the context menu key or Shift+F10 opens a menu
// with Move to and Open; the panel's Move to offers the same stages.
void TestTasksView::cardMenuMovesByKeyboard()
{
    Board b; QVERIFY(b.open());
    auto *inbox = list(*b.view, "Inbox");
    inbox->setFocus();
    inbox->setCurrentItem(card(*b.view, "t1"));
    QTest::keyClick(inbox, Qt::Key_Menu);
    QMenu *menu = nullptr;
    QTRY_VERIFY((menu = b.view->findChild<QMenu *>("taskCardMenu")) && menu->isVisible());
    auto *moves = menu->findChild<QMenu *>("taskMoveMenu");
    QVERIFY(moves);
    QCOMPARE(actionData(moves), QStringList{"Ready"});
    action(moves, "Ready")->trigger();
    QTRY_COMPARE(b.f.calls("move").size(), 1);
    QCOMPARE(plain(b.f.calls("move").last()), (QJsonObject{{"task", "t1"}, {"to", "Ready"}}));
    menu->close();
    QTRY_VERIFY(!b.view->findChild<QMenu *>("taskCardMenu"));
    auto *check = list(*b.view, "Check");
    check->setFocus();
    check->setCurrentItem(card(*b.view, "t6"));
    QTest::keyClick(check, Qt::Key_F10, Qt::ShiftModifier);
    QTRY_VERIFY((menu = b.view->findChild<QMenu *>("taskCardMenu")) && menu->isVisible());
    QCOMPARE(actionData(menu->findChild<QMenu *>("taskMoveMenu")), (QStringList{"Shipped", "Later"}));
    action(menu, "open")->trigger();
    QCOMPARE(b.view->openedTask(), QString("t6"));
    QTRY_VERIFY(b.panel()->isVisible());
    QTRY_COMPARE(actionData(b.find<QPushButton>("taskMove")->menu()), (QStringList{"Shipped", "Later"}));
    menu->close();
}

// An edited prompt is a draft of its task: it survives changes made elsewhere
// (with a note), other cards and a refused save, and goes once saved.
void TestTasksView::promptDraftSurvivesChanges()
{
    Board b; QVERIFY(b.open());
    QVERIFY(b.show("t3"));
    auto *prompt = b.find<QPlainTextEdit>("taskPrompt");
    auto *save = b.find<QPushButton>("taskPromptSave");
    auto *note = b.find<QLabel>("taskPromptNote");
    QCOMPARE(prompt->toPlainText(), QString("Prompt of From the pool"));
    prompt->setPlainText("My better prompt");
    // The task changes elsewhere.
    auto board = demoBoard();
    auto tasks = board["tasks"].toArray();
    for (int i = 0; i < tasks.size(); ++i) {
        auto task = tasks[i].toObject();
        if (task["uuid"] == "t3") { task["title"] = "From the pool, renamed"; tasks[i] = task; }
    }
    board["tasks"] = tasks;
    QVERIFY(b.f.put("show-t3.json", compact({{"prompt", "Their prompt"}})));
    QVERIFY(b.f.put("board-demo.json", compact(board)));
    QVERIFY(b.f.put("rev", "5"));
    QTRY_COMPARE(b.find<QLineEdit>("taskTitle")->text(), QString("From the pool, renamed"));
    QTRY_VERIFY(note->isVisible());
    QVERIFY2(note->text().contains("changed"), qPrintable(note->text()));
    QVERIFY(note->property("warning").toBool());
    QCOMPARE(prompt->toPlainText(), QString("My better prompt"));
    // The version saved now is shown on request; the draft stays.
    auto *showCurrent = b.find<QPushButton>("taskPromptShowCurrent");
    auto *saved = b.find<QPlainTextEdit>("taskPromptCurrent");
    QVERIFY(showCurrent->isVisible());
    QVERIFY(!saved->isVisible());
    showCurrent->click();
    QVERIFY(saved->isVisible());
    QCOMPARE(saved->toPlainText(), QString("Their prompt"));
    QCOMPARE(prompt->toPlainText(), QString("My better prompt"));
    // Another card and back.
    QVERIFY(b.show("t1"));
    QCOMPARE(prompt->toPlainText(), QString("Prompt of Write the parser"));
    QVERIFY(!save->isVisible());
    QVERIFY(b.show("t3"));
    QCOMPARE(prompt->toPlainText(), QString("My better prompt"));
    QVERIFY(save->isVisible());
    // Saving over the other version asks first.
    Confirmations dialogs;
    dialogs.answer(false);
    save->click();
    QCOMPARE(dialogs.texts.size(), 1);
    QVERIFY2(dialogs.texts.last().contains("changed elsewhere"), qPrintable(dialogs.texts.last()));
    QTest::qWait(150);
    QVERIFY(b.f.calls("edit").isEmpty());
    // A refused save keeps it.
    QVERIFY(b.f.put("refuse-edit.json", R"({"ok":false,"error":{"code":"read_only","message":"this task was written by a newer version of hgs"}})"));
    dialogs.answer(true);
    save->click();
    QTRY_COMPARE(b.message()->text(), QString("this task was written by a newer version of hgs"));
    QCOMPARE(prompt->toPlainText(), QString("My better prompt"));
    QVERIFY(save->isVisible());
    // Over 64 KiB, or empty: it cannot be saved.
    prompt->setPlainText(QString(64 * 1024 + 1, 'x'));
    QVERIFY(!save->isEnabled());
    QVERIFY2(note->text().contains("64 KiB"), qPrintable(note->text()));
    prompt->setPlainText("   ");
    QVERIFY(!save->isEnabled());
    // Saved: the draft is gone.
    QVERIFY(QFile::remove(b.f.dir.filePath("refuse-edit.json")));
    prompt->setPlainText("My better prompt");
    dialogs.answer(true);
    save->click();
    QTRY_VERIFY(!save->isVisible());
    QCOMPARE(prompt->toPlainText(), QString("My better prompt"));
    QVERIFY(!note->isVisible());
    QVERIFY(!showCurrent->isVisible());
    QCOMPARE(dialogs.texts.size(), 3);
    QCOMPARE(plain(b.f.calls("edit").last()), (QJsonObject{{"task", "t3"}, {"prompt", "My better prompt"}}));
}

// The board keeps at least a column next to the panel; too narrow for both,
// the panel takes the board's place until it is closed.
void TestTasksView::panelSharesTheWidth()
{
    Board b; QVERIFY(b.open({840, 700}));
    auto *board = b.find<QScrollArea>("tasksBoard");
    auto *panel = b.panel();
    QVERIFY(b.show("t1"));
    QTRY_VERIFY(board->isVisible() && panel->isVisible());
    QVERIFY2(board->width() >= 244, qPrintable(QString::number(board->width())));
    QVERIFY(panel->width() >= 300);
    // A remembered wide panel is capped.
    b.view->closeTask();
    QSettings().setValue("workspace/tasksPanelWidth", 2000);
    b.view->resize(960, 700);
    QVERIFY(b.show("t3"));
    QTRY_VERIFY(board->isVisible());
    QVERIFY2(board->width() >= 244, qPrintable(QString::number(board->width())));
    // Too narrow: the panel only; closing it brings the board back.
    b.view->resize(480, 700);
    QTRY_VERIFY(!board->isVisible());
    QVERIFY(panel->isVisible());
    QVERIFY(b.view->property("panelOnly").toBool());
    b.find<QAbstractButton>("taskPanelClose")->click();
    QVERIFY(board->isVisible());
    QVERIFY(!panel->isVisible());
    // Opened narrow, then widened: both again, the board with a column.
    QVERIFY(b.show("t1"));
    QTRY_VERIFY(!board->isVisible());
    b.view->resize(840, 700);
    QTRY_VERIFY(board->isVisible() && board->width() >= 244);
    QVERIFY(panel->isVisible());
    QVERIFY(!b.view->property("panelOnly").toBool());
}

// An image opens only when its bytes are a picture, from a copy named for its
// real format; anything else shows why.
void TestTasksView::imagesOpenOnlyAsPictures()
{
    Board b; QVERIFY(b.open());
    QImage image(40, 30, QImage::Format_RGB32); image.fill(Qt::green);
    QVERIFY(image.save(b.f.dir.filePath("image-cccc.png"), "PNG"));
    QVERIFY(b.f.put("image-bbbb.png", "#!/bin/sh\necho opened\n"));
    const auto attachment = [](const QString &name, const QString &sha) {
        return QJsonObject{{"name", name}, {"mime", "image/png"}, {"bytes", 20}, {"sha256", sha}};
    };
    QVERIFY(image.save(b.f.dir.filePath("image-eeee.png"), "PNG"));
    const QString longName = QString(176, 'A') + ".jarX.command";
    QVERIFY(b.f.put("show-t5.json", compact({{"attachments", QJsonArray{attachment("notes.command", "bbbb"), attachment("evil.command", "cccc"),
                                                                        attachment("missing.png", "dddd"), attachment(longName, "eeee")}}})));
    QVERIFY(b.show("t5"));
    QTRY_COMPARE(b.f.calls("attachment").size(), 4);
    QList<QUrl> opened;
    bool opens = true;
    qobject_cast<TaskPanel *>(b.panel())->openUrl = [&](const QUrl &url) { opened << url; return opens; };
    const auto thumb = [&](const QString &sha) -> QToolButton * {
        for (auto *button : b.panel()->findChildren<QToolButton *>("taskImage")) if (button->property("sha256") == sha) return button;
        return nullptr;
    };
    QTRY_VERIFY(thumb("dddd") && thumb("dddd")->text() == "!");
    // Not a picture: nothing is opened.
    thumb("bbbb")->click();
    QVERIFY(opened.isEmpty());
    QCOMPARE(b.message()->text(), QString("notes.command is not a picture this computer can show, so it is not opened."));
    QVERIFY(b.message()->property("error").toBool());
    // A picture with another name opens as what it is.
    thumb("cccc")->click();
    QCOMPARE(opened.size(), 1);
    QVERIFY2(opened.first().toLocalFile().endsWith("/evil.png"), qPrintable(opened.first().toLocalFile()));
    QFile::remove(opened.first().toLocalFile());
    // However long the name an agent chose, the copy ends with the picture's extension.
    thumb("eeee")->click();
    QCOMPARE(opened.size(), 2);
    const QFileInfo copy(opened.last().toLocalFile());
    QVERIFY2(copy.fileName().endsWith(".png") && copy.fileName().size() <= 180, qPrintable(copy.fileName()));
    QFile::remove(copy.filePath());
    // A missing image and a desktop that cannot open it say so.
    thumb("dddd")->click();
    QCOMPARE(b.message()->text(), QString("Cannot open missing.png: the image is not on this machine"));
    opens = false;
    thumb("cccc")->click();
    QCOMPARE(b.message()->text(), QString("No application could open evil.command."));
    QFile::remove(opened.last().toLocalFile());
}

// Images are added from files or the clipboard and removed after a question.
// Each change sends the task's new list through `edit`: kept images by their
// digest, new ones with their bytes, so another machine's board works alike.
void TestTasksView::imagesAddPasteAndRemove()
{
    Board b; QVERIFY(b.open());
    QImage shot(48, 32, QImage::Format_RGB32); shot.fill(Qt::red);
    QVERIFY(shot.save(b.f.dir.filePath("image-aaaa.png"), "PNG"));
    QVERIFY(b.f.put("show-t5.json", compact({{"attachments", QJsonArray{QJsonObject{{"name", "shot.png"}, {"mime", "image/png"}, {"bytes", 100}, {"sha256", "aaaa"}}}}})));
    const QJsonObject kept{{"sha256", "aaaa"}};
    const QString added = b.f.dir.filePath("plan.png");
    QImage plan(20, 20, QImage::Format_RGB32); plan.fill(Qt::blue);
    QVERIFY(plan.save(added, "PNG"));
    QFile addedFile(added); QVERIFY(addedFile.open(QIODevice::ReadOnly));
    const QByteArray planBytes = addedFile.readAll();
    const auto entry = [](const QString &name, const QByteArray &bytes) {
        return QJsonObject{{"name", name}, {"mime", "image/png"}, {"data_base64", QString::fromLatin1(bytes.toBase64())}};
    };
    QVERIFY(b.show("t5"));
    auto *panel = qobject_cast<TaskPanel *>(b.panel());
    QTRY_VERIFY(!b.find<QToolButton>("taskImage")->icon().isNull());
    // A file: the kept image and the new one.
    QStringList picked{added};
    panel->pickImages = [&] { return picked; };
    b.find<QPushButton>("taskImageAdd")->click();
    QTRY_COMPARE(b.f.calls("edit").size(), 1);
    QCOMPARE(plain(b.f.calls("edit").last()), (QJsonObject{{"task", "t5"}, {"attachments", QJsonArray{kept, entry("plan.png", planBytes)}}}));
    QTRY_COMPARE(b.message()->text(), QString("Added 1 image(s) to om-t5."));
    // A file that is not a picture is refused here.
    QVERIFY(b.f.put("notes.png", "not a picture"));
    picked = {b.f.dir.filePath("notes.png")};
    b.find<QPushButton>("taskImageAdd")->click();
    QCOMPARE(b.message()->text(), QString("notes.png is not a picture."));
    // Only regular files are read, and at most 10 MiB of them.
    QFile big(b.f.dir.filePath("big.png"));
    QVERIFY(big.open(QIODevice::WriteOnly) && big.resize(10 * 1024 * 1024 + 1));
    big.close();
    picked = {big.fileName()};
    b.find<QPushButton>("taskImageAdd")->click();
    QCOMPARE(b.message()->text(), QString("Cannot add big.png: it is larger than 10 MiB."));
#ifdef Q_OS_UNIX
    const QString fifo = b.f.dir.filePath("pipe.png");
    QCOMPARE(::mkfifo(QFile::encodeName(fifo).constData(), 0600), 0);
    picked = {fifo};
    b.find<QPushButton>("taskImageAdd")->click();   // returns: a FIFO is never opened
    QCOMPARE(b.message()->text(), QString("Cannot add pipe.png: it is not a regular file."));
#endif
    QCOMPARE(b.f.calls("edit").size(), 1);
    // The clipboard's image, as PNG.
    QImage pasted(16, 12, QImage::Format_RGB32); pasted.fill(Qt::yellow);
    QGuiApplication::clipboard()->setImage(pasted);
    b.find<QPushButton>("taskImagePaste")->click();
    QTRY_COMPARE(b.f.calls("edit").size(), 2);
    const auto list = b.f.calls("edit").last()["attachments"].toArray();
    QCOMPARE(list.size(), 2);
    QCOMPARE(list[0].toObject(), kept);
    const auto clip = list[1].toObject();
    QVERIFY2(clip["name"].toString().startsWith("pasted-") && clip["name"].toString().endsWith(".png"), qPrintable(clip["name"].toString()));
    QCOMPARE(clip["mime"].toString(), QString("image/png"));
    QCOMPARE(QImage::fromData(QByteArray::fromBase64(clip["data_base64"].toString().toLatin1())).size(), QSize(16, 12));
    QGuiApplication::clipboard()->clear();
    b.find<QPushButton>("taskImagePaste")->click();
    QCOMPARE(b.message()->text(), QString("The clipboard holds no image."));
    // Removing asks; then the rest of the list is sent.
    Confirmations dialogs;
    dialogs.answer(false);
    b.find<QToolButton>("taskImageRemove")->click();
    QCOMPARE(dialogs.texts.size(), 1);
    QVERIFY2(dialogs.texts.last().contains("shot.png") && dialogs.texts.last().contains("om-t5"), qPrintable(dialogs.texts.last()));
    QTest::qWait(100);
    QCOMPARE(b.f.calls("edit").size(), 2);
    dialogs.answer(true);
    b.find<QToolButton>("taskImageRemove")->click();
    QTRY_COMPARE(b.f.calls("edit").size(), 3);
    QCOMPARE(plain(b.f.calls("edit").last()), (QJsonObject{{"task", "t5"}, {"attachments", QJsonArray()}}));
    // Another machine's board: the same requests reach mac's CLI, and the
    // kept image goes by its digest although its bytes are only there.
    b.f.model->setHost("mac");
    QVERIFY(b.show("t1"));
    QVERIFY(b.show("t5"));
    picked = {added};
    QVERIFY(b.find<QPushButton>("taskImageAdd")->isEnabled());
    b.find<QPushButton>("taskImageAdd")->click();
    QTRY_COMPARE(b.f.calls("edit").size(), 4);
    QCOMPARE(plain(b.f.calls("edit").last()), (QJsonObject{{"task", "t5"}, {"attachments", QJsonArray{kept, entry("plan.png", planBytes)}}}));
    QCOMPARE(b.f.hosts("edit").last(), QString("mac"));
    QVERIFY(b.find<QToolButton>("taskImageRemove")->isEnabled());
    dialogs.answer(true);
    b.find<QToolButton>("taskImageRemove")->click();
    QTRY_COMPARE(b.f.calls("edit").size(), 5);
    QCOMPARE(plain(b.f.calls("edit").last()), (QJsonObject{{"task", "t5"}, {"attachments", QJsonArray()}}));
    QCOMPARE(b.f.hosts("edit").last(), QString("mac"));
    // The limits are checked here first: a task that holds 20 MiB of images takes no more.
    QVERIFY(b.f.put("show-t1.json", compact({{"attachments", QJsonArray{
        QJsonObject{{"name", "a.png"}, {"mime", "image/png"}, {"bytes", 10 * 1024 * 1024}, {"sha256", "a1"}},
        QJsonObject{{"name", "b.png"}, {"mime", "image/png"}, {"bytes", 10 * 1024 * 1024}, {"sha256", "b1"}}}}})));
    QVERIFY(b.show("t1"));
    b.find<QPushButton>("taskImageAdd")->click();
    QCOMPARE(b.message()->text(), QString("The images of a task are limited to 20 MiB in total."));
    QTest::qWait(100);
    QCOMPARE(b.f.calls("edit").size(), 5);
}

QTEST_MAIN(TestTasksView)
#include "test_tasksview.moc"
