#include "FakeTasksCli.h"
#include "HgsClient.h"
#include "SessionList.h"
#include "TaskDrag.h"
#include "TaskRow.h"
#include "TasksView.h"
#include <QAbstractButton>
#include <QApplication>
#include <QDragEnterEvent>
#include <QDragLeaveEvent>
#include <QDragMoveEvent>
#include <QDrag>
#include <QDropEvent>
#include <QElapsedTimer>
#include <QLabel>
#include <QMimeData>
#include <QMouseEvent>
#include <QPointer>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <memory>

namespace {
QList<TaskSession> sessions() { return {{"", "codex/api", "Ready", ""}, {"", "codex/web", "Working", ""}, {"orchard-mac", "codex/ios", "Ready", ""}}; }
TaskItem item(int n, const QString &stage, const QJsonObject &extra = {}) { return TaskBoard::item(FakeTasksCli::task(n, stage, extra)); }
QAbstractButton *tile(TasksView &view, const QString &stage) { return view.findChild<QAbstractButton *>("taskTile:" + stage); }
QWidget *row(TasksView &view, int n) { return view.findChild<QWidget *>(QString("taskRow:T-%1").arg(n, 6, 10, QLatin1Char('0'))); }
QStringList visibleRows(TasksView &view) {
    QStringList out;
    for (auto *w : view.findChildren<QWidget *>()) if (w->objectName().startsWith("taskRow:") && w->isVisibleTo(&view)) out << w->objectName().mid(8);
    std::sort(out.begin(), out.end(), [&](const QString &a, const QString &b) {
        return view.findChild<QWidget *>("taskRow:" + a)->y() < view.findChild<QWidget *>("taskRow:" + b)->y(); });
    return out;
}
TaskDrag todo(int n) { return TaskDrag::of("project-1", TaskBoard::item(FakeTasksCli::task(n, "todo"))); }
// Drags `drag` into `target` and over `point` (its centre by default); true when the target takes it there.
bool dragOver(QWidget *target, const TaskDrag &drag, QPoint point = {}) {
    if (point.isNull()) point = target->rect().center();
    std::unique_ptr<QMimeData> mime(drag.mimeData());
    QDragEnterEvent enter(point, Qt::MoveAction, mime.get(), Qt::LeftButton, Qt::NoModifier); QApplication::sendEvent(target, &enter);
    if (!enter.isAccepted()) return false;
    QDragMoveEvent move(point, Qt::MoveAction, mime.get(), Qt::LeftButton, Qt::NoModifier); QApplication::sendEvent(target, &move);
    return move.isAccepted();
}
// Drags `drag` over `point` of `target` and drops it there; true when the target took the drop.
bool drop(QWidget *target, const TaskDrag &drag, QPoint point = {}) {
    if (point.isNull()) point = target->rect().center();
    if (!dragOver(target, drag, point)) return false;
    std::unique_ptr<QMimeData> mime(drag.mimeData());
    QDropEvent event(QPointF(point), Qt::MoveAction, mime.get(), Qt::LeftButton, Qt::NoModifier); QApplication::sendEvent(target, &event);
    return event.isAccepted();
}
void leave(QWidget *target) { QDragLeaveEvent event; QApplication::sendEvent(target, &event); }
bool called(const QDir &dir, const QStringList &argv) { return FakeTasksCli::calls(dir).contains(argv); }
// The platform's drag loop runs events while the person drags; offscreen has
// none, so this one runs `during` in its place.
class LoopingTasksView : public TasksView {
public:
    using TasksView::TasksView;
    std::function<void()> during;
protected:
    Qt::DropAction execDrag(QDrag *drag) override { if (during) during(); drag->deleteLater(); return Qt::IgnoreAction; }
};
// A press at `from`, a move by `distance` with the button held, and the release.
void pressMoveRelease(QWidget *widget, QPoint from, int distance) {
    const QPoint to = from + QPoint(0, distance);
    QTest::mousePress(widget, Qt::LeftButton, {}, from);
    QMouseEvent move(QEvent::MouseMove, QPointF(to), QPointF(widget->mapToGlobal(to)), Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(widget, &move);
    QTest::mouseRelease(widget, Qt::LeftButton, {}, to);
}
}

class TestTaskDrag : public QObject {
    Q_OBJECT
    QTemporaryDir m_settings;
private slots:
    void initTestCase();
    void init() { QSettings().remove("tasks"); }
    void thePayloadNamesTheTaskAndItsProject();
    void toDoAndBacklogRowsStartADrag();
    void theOtherListsTileTakesADroppedTask();
    void toDoRowsAreReorderedByDropping();
    void theFunnelReordersAmongTheRowsItShows();
    void aLongToDoListScrollsAtItsEdgesWhileDragging();
    void theSessionListTakesTasksOfItsProject();
    void aDroppedSessionRepinsTheTask();
    void aRowAListingDropsLivesUntilItsDragEnds();
    void theHeaderLineAndTheReportStartDragsAndClicks();
};

void TestTaskDrag::initTestCase() {
    QVERIFY(m_settings.isValid());
    QCoreApplication::setOrganizationName("hgs-tests"); QCoreApplication::setApplicationName("task-drag");
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, m_settings.path());
}

void TestTaskDrag::thePayloadNamesTheTaskAndItsProject() {
    const auto drag = TaskDrag::of("project-1", item(7, "todo", {{"target", FakeTasksCli::pinned("codex/ios", "orchard-mac")}}));
    std::unique_ptr<QMimeData> mime(drag.mimeData());
    QVERIFY(mime->hasFormat("application/x-zerus-task"));
    const auto back = TaskDrag::from(mime.get()); QVERIFY(back);
    QCOMPARE(back->projectId, QString("project-1")); QCOMPARE(back->uuid, QString("uuid-7")); QCOMPARE(back->id, QString("T-000007"));
    QCOMPARE(back->stage, QString("todo"));
    QCOMPARE(back->targetHost, QString("orchard-mac")); QCOMPARE(back->targetSession, QString("codex/ios")); QVERIFY(back->targetKnown);
    // A session on a computer this one has not verified is never the same as one it knows.
    const auto unverified = TaskDrag::of("project-1", item(8, "todo", {{"target", QJsonObject{{"kind", "session"}, {"machine", "m-x"},
        {"session", "codex/api"}, {"host", QJsonValue()}}}}));
    std::unique_ptr<QMimeData> unknown(unverified.mimeData()); QVERIFY(!TaskDrag::from(unknown.get())->targetKnown);
    // Anything else is not a task.
    QMimeData text; text.setText("T-000007"); QVERIFY(!TaskDrag::from(&text));
    QMimeData broken; broken.setData("application/x-zerus-task", "{}"); QVERIFY(!TaskDrag::from(&broken));
    QVERIFY(!TaskDrag::from(nullptr));
}

void TestTaskDrag::toDoAndBacklogRowsStartADrag() {
    QWidget host; auto *taskRow = new TaskRow(&host); host.resize(600, 80); taskRow->resize(600, 80); host.show();
    QSignalSpy started(taskRow, &TaskRow::dragStarted), opened(taskRow, &TaskRow::opened);
    const QPoint text(100, 50); const int far = QApplication::startDragDistance() + 2;
    for (const char *stage : {"todo", "backlog"}) {
        taskRow->setTask(item(1, stage), {}, TaskColors::of(true), 0); started.clear(); opened.clear();
        // A click that shakes a little still opens the task.
        pressMoveRelease(taskRow, text, 2); QCOMPARE(started.size(), 0); QCOMPARE(opened.size(), 1);
        // Moving farther drags it from where it was pressed, and the release opens nothing.
        pressMoveRelease(taskRow, text, far); QCOMPARE(started.size(), 1); QCOMPARE(opened.size(), 1);
        QCOMPARE(started[0][0].toPoint(), text);
    }
    // The actions never drag their row.
    auto *actions = taskRow->findChild<QWidget *>("taskActions");
    pressMoveRelease(taskRow, actions->mapTo(taskRow, QPoint(actions->width() - 1, actions->height() / 2)), far); QCOMPARE(started.size(), 1);
    // Tasks in other stages, and tasks of a newer Zerus, stay where they are.
    for (const auto &task : {item(2, "active"), item(3, "review"), item(4, "done"), item(5, "todo", {{"writable", false}})}) {
        taskRow->setTask(task, {}, TaskColors::of(true), 0); started.clear();
        pressMoveRelease(taskRow, text, far); QVERIFY2(started.isEmpty(), qPrintable(task.id));
    }
    // While its drag runs the row stays in place, faded.
    taskRow->setDragging(true); QVERIFY(taskRow->property("dragging").toBool()); QVERIFY(taskRow->graphicsEffect());
    taskRow->setDragging(false); QVERIFY(!taskRow->property("dragging").toBool()); QVERIFY(!taskRow->graphicsEffect());
}

void TestTaskDrag::theOtherListsTileTakesADroppedTask() {
    QTemporaryDir dir; QVERIFY(FakeTasksCli::install(dir.path(), {FakeTasksCli::task(1, "todo"), FakeTasksCli::task(2, "todo"), FakeTasksCli::task(3, "backlog")}));
    HgsClient client(FakeCli::program(dir.filePath("hgs"))); TasksView view(&client); view.resize(760, 560); view.show();
    QSignalSpy notices(&view, &TasksView::notice);
    view.setContext("project-1", "orchard", "", "codex/api", "\ncodex/api", sessions());
    QTRY_COMPARE(tile(view, "todo")->property("count").toInt(), 2);
    const auto todo = TaskDrag::of("project-1", item(2, "todo")), backlog = TaskDrag::of("project-1", item(3, "backlog"));
    for (const char *stage : {"review", "active", "todo", "backlog", "done"}) QVERIFY2(tile(view, stage)->acceptDrops(), stage);
    // The other list's tile takes the task and shows that it will.
    QVERIFY(dragOver(tile(view, "backlog"), todo)); QVERIFY(tile(view, "backlog")->property("dropTarget").toBool());
    leave(tile(view, "backlog")); QVERIFY(!tile(view, "backlog")->property("dropTarget").toBool());
    // Its own tile, the other stages and another project's tasks refuse it.
    QVERIFY(!dragOver(tile(view, "todo"), todo)); QVERIFY(!tile(view, "todo")->property("dropTarget").toBool());
    for (const char *stage : {"review", "active", "done"}) QVERIFY2(!dragOver(tile(view, stage), todo), stage);
    QVERIFY(!dragOver(tile(view, "backlog"), TaskDrag::of("project-2", item(2, "todo"))));
    // A drop moves it there; hgs puts it at the top of that list.
    QVERIFY(drop(tile(view, "backlog"), todo)); QVERIFY(!tile(view, "backlog")->property("dropTarget").toBool());
    QTRY_COMPARE(tile(view, "backlog")->property("count").toInt(), 2);
    QVERIFY(called(dir.path(), {"task", "stage", "uuid-2", "backlog", "--project", "project-1"}));
    QVERIFY(drop(tile(view, "todo"), backlog));
    QTRY_COMPARE(tile(view, "todo")->property("count").toInt(), 2);
    QVERIFY(called(dir.path(), {"task", "stage", "uuid-3", "todo", "--project", "project-1"}));
    QCOMPARE(view.selectedStage(), QString("todo"));   // the list shown stays
    // A refusal is named and the listing read again.
    QFile fail(dir.filePath("fail")); QVERIFY(fail.open(QIODevice::WriteOnly)); fail.write("stage"); fail.close();
    QVERIFY(drop(tile(view, "backlog"), TaskDrag::of("project-1", item(1, "todo"))));
    QTRY_COMPARE(notices.size(), 1); QCOMPARE(notices[0][0].toString(), QString("hgs refused: stage")); QVERIFY(notices[0][1].toBool());
}

void TestTaskDrag::toDoRowsAreReorderedByDropping() {
    QTemporaryDir dir; QVERIFY(FakeTasksCli::install(dir.path(), {FakeTasksCli::task(1, "todo"), FakeTasksCli::task(2, "todo"),
        FakeTasksCli::task(3, "todo"), FakeTasksCli::task(4, "todo"), FakeTasksCli::task(5, "backlog")}));
    HgsClient client(FakeCli::program(dir.filePath("hgs"))); TasksView view(&client); view.resize(760, 560); view.show();
    view.setContext("project-1", "orchard", "", "codex/api", "\ncodex/api", sessions());
    QTRY_COMPARE(visibleRows(view), (QStringList{"T-000001", "T-000002", "T-000003", "T-000004"}));
    auto *page = view.findChild<QWidget *>("taskListPage"); auto *line = view.findChild<QWidget *>("taskDropLine");
    QVERIFY(page && line && page->acceptDrops());
    const auto at = [&](int n, double share) { auto *r = row(view, n); return r->mapTo(page, QPoint(r->width() / 2, int(r->height() * share))); };
    const auto top = [&](int n) { return row(view, n)->mapTo(page, QPoint()).y(); };
    const auto bottom = [&](int n) { return top(n) + row(view, n)->height(); };
    const auto moves = [&] { int n = 0; for (const auto &call : FakeTasksCli::calls(dir.path())) n += call.value(1) == "move"; return n; };
    // Over a row's upper half the line shows above it, over its lower half below it.
    QVERIFY(dragOver(page, todo(4), at(1, 0.25))); QVERIFY(line->isVisible()); QVERIFY(qAbs(line->geometry().center().y() - top(1)) <= 1);
    QVERIFY(dragOver(page, todo(4), at(2, 0.75))); QVERIFY(qAbs(line->geometry().center().y() - bottom(2)) <= 1);
    // Where the task already is, no line: nothing would change.
    QVERIFY(dragOver(page, todo(2), at(2, 0.25))); QVERIFY(!line->isVisible());
    QVERIFY(dragOver(page, todo(2), at(1, 0.75))); QVERIFY(!line->isVisible());
    QVERIFY(dragOver(page, todo(2), at(3, 0.25))); QVERIFY(!line->isVisible());
    QVERIFY(dragOver(page, todo(4), at(1, 0.25))); leave(page); QVERIFY(!line->isVisible());
    // A drop goes before the row below the line, or after the last one.
    QVERIFY(drop(page, todo(4), at(1, 0.25))); QVERIFY(!line->isVisible());
    QTRY_COMPARE(visibleRows(view), (QStringList{"T-000004", "T-000001", "T-000002", "T-000003"}));
    QVERIFY(called(dir.path(), {"task", "move", "uuid-4", "--before", "uuid-1", "--project", "project-1"}));
    QVERIFY(drop(page, todo(4), at(2, 0.75)));
    QTRY_COMPARE(visibleRows(view), (QStringList{"T-000001", "T-000002", "T-000004", "T-000003"}));
    QVERIFY(called(dir.path(), {"task", "move", "uuid-4", "--before", "uuid-3", "--project", "project-1"}));
    QVERIFY(drop(page, todo(1), QPoint(page->width() / 2, bottom(3) + 20)));
    QTRY_COMPARE(visibleRows(view), (QStringList{"T-000002", "T-000004", "T-000003", "T-000001"}));
    QVERIFY(called(dir.path(), {"task", "move", "uuid-1", "--after", "uuid-3", "--project", "project-1"}));
    // A drop where it already is asks nothing of hgs.
    const int before = moves(); QVERIFY(drop(page, todo(4), at(4, 0.6))); QTest::qWait(100); QCOMPARE(moves(), before);
    // Backlog tasks and other projects' tasks do not join To do here, and Backlog is not reordered.
    QVERIFY(!dragOver(page, TaskDrag::of("project-1", TaskBoard::item(FakeTasksCli::task(5, "backlog"))), at(1, 0.25)));
    QVERIFY(!dragOver(page, TaskDrag::of("project-2", TaskBoard::item(FakeTasksCli::task(1, "todo"))), at(1, 0.25)));
    tile(view, "backlog")->click(); QTRY_COMPARE(visibleRows(view), QStringList{"T-000005"});
    QVERIFY(!dragOver(page, TaskDrag::of("project-1", TaskBoard::item(FakeTasksCli::task(5, "backlog"))), at(5, 0.25)));
    QVERIFY(!line->isVisible());
}

void TestTaskDrag::theFunnelReordersAmongTheRowsItShows() {
    QTemporaryDir dir; QVERIFY(FakeTasksCli::install(dir.path(), {FakeTasksCli::task(1, "todo"),
        FakeTasksCli::task(2, "todo", {{"target", FakeTasksCli::pinned("codex/web")}}), FakeTasksCli::task(3, "todo")}));
    HgsClient client(FakeCli::program(dir.filePath("hgs"))); TasksView view(&client); view.resize(760, 560); view.show();
    view.setContext("project-1", "orchard", "", "codex/api", "\ncodex/api", sessions());
    QTRY_COMPARE(visibleRows(view).size(), 3);
    view.findChild<QAbstractButton *>("taskOnlySession")->click(); QCOMPARE(visibleRows(view), (QStringList{"T-000001", "T-000003"}));
    // The hidden task of another session keeps its place; the drop names a row that shows.
    auto *page = view.findChild<QWidget *>("taskListPage");
    QVERIFY(drop(page, todo(3), row(view, 1)->mapTo(page, QPoint(40, 6))));
    QTRY_VERIFY(called(dir.path(), {"task", "move", "uuid-3", "--before", "uuid-1", "--project", "project-1"}));
    QTRY_COMPARE(visibleRows(view), (QStringList{"T-000003", "T-000001"}));
}

void TestTaskDrag::aLongToDoListScrollsAtItsEdgesWhileDragging() {
    QJsonArray tasks; for (int n = 1; n <= 30; ++n) tasks << FakeTasksCli::task(n, "todo");
    QTemporaryDir dir; QVERIFY(FakeTasksCli::install(dir.path(), tasks));
    HgsClient client(FakeCli::program(dir.filePath("hgs"))); TasksView view(&client); view.resize(760, 420); view.show();
    view.setContext("project-1", "orchard", "", "codex/api", "\ncodex/api", sessions());
    QTRY_COMPARE(visibleRows(view).size(), 30);
    auto *scroll = view.findChild<QScrollArea *>("taskList"); auto *bar = scroll->verticalScrollBar(); auto *page = view.findChild<QWidget *>("taskListPage");
    QTRY_VERIFY(bar->maximum() > 0); QCOMPARE(bar->value(), 0);
    const auto viewport = [&](int y) { return page->mapFrom(scroll->viewport(), QPoint(scroll->viewport()->width() / 2, y)); };
    // Held at the bottom edge the list scrolls down, and the line follows the rows.
    QVERIFY(dragOver(page, todo(1), viewport(scroll->viewport()->height() - 6)));
    QTRY_VERIFY(bar->value() > 40);
    // Away from the edges it stops.
    QVERIFY(dragOver(page, todo(1), viewport(scroll->viewport()->height() / 2)));
    int still = bar->value(); QTest::qWait(150); QCOMPARE(bar->value(), still);
    // At the top edge it scrolls back up, and it stops when the drag leaves.
    QVERIFY(dragOver(page, todo(1), viewport(6))); QTRY_VERIFY(bar->value() < still);
    leave(page); still = bar->value(); QTest::qWait(150); QCOMPARE(bar->value(), still);
    QVERIFY(!view.findChild<QWidget *>("taskDropLine")->isVisible());
}

void TestTaskDrag::theSessionListTakesTasksOfItsProject() {
    SessionList list; list.resize(320, 500);
    const auto add = [&](const QString &group, bool header, const QStringList &target = {}, const QString &child = {}) {
        auto *item = new QListWidgetItem(&list); item->setSizeHint(QSize(300, header ? 34 : 60));
        item->setData(SessionRoles::Header, header); item->setData(SessionRoles::Group, group);
        item->setData(SessionRoles::TaskTarget, target); item->setData(SessionRoles::ChildId, child);
        item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable); return item;
    };
    auto *orchard = add("project-1", true); auto *api = add("project-1", false, {"", "codex/api"});
    auto *ios = add("project-1", false, {"orchard-mac", "codex/ios"}); auto *subagent = add("project-1", false, {}, "agent-1");
    auto *other = add("project-2", true); auto *docs = add("project-2", false, {"", "codex/docs"});
    list.show(); QVERIFY(QTest::qWaitForWindowExposed(&list)); QVERIFY(list.acceptDrops());
    QSignalSpy dropped(&list, &SessionList::taskDropped);
    const auto over = [&](const TaskDrag &task, QListWidgetItem *item) { return dragOver(list.viewport(), task, list.visualItemRect(item).center()); };
    const auto any = todo(1); auto pinned = todo(2); pinned.targetSession = "codex/api";
    // An Any session task goes to a session of its project, not to its header or a subagent.
    QVERIFY(over(any, api)); QVERIFY(over(any, ios)); QVERIFY(!over(any, orchard)); QVERIFY(!over(any, subagent));
    // Another project's sessions and header refuse it.
    QVERIFY(!over(any, other)); QVERIFY(!over(any, docs));
    // A pinned task goes to another session, or to the header, which makes it Any session.
    QVERIFY(!over(pinned, api)); QVERIFY(over(pinned, ios)); QVERIFY(over(pinned, orchard));
    // A session on a computer this one has not verified is not this computer's session.
    auto unverified = pinned; unverified.targetKnown = false; QVERIFY(over(unverified, api));
    leave(list.viewport());
    QVERIFY(drop(list.viewport(), any, list.visualItemRect(ios).center()));
    QCOMPARE(dropped.size(), 1); QCOMPARE(dropped[0][0].value<TaskDrag>().uuid, QString("uuid-1"));
    QCOMPARE(dropped[0][1].toString(), QString("orchard-mac")); QCOMPARE(dropped[0][2].toString(), QString("codex/ios"));
    QVERIFY(drop(list.viewport(), pinned, list.visualItemRect(orchard).center()));
    QCOMPARE(dropped.size(), 2); QCOMPARE(dropped[1][1].toString(), QString()); QCOMPARE(dropped[1][2].toString(), QString());
    QVERIFY(!drop(list.viewport(), any, list.visualItemRect(docs).center())); QCOMPARE(dropped.size(), 2);
}

void TestTaskDrag::aDroppedSessionRepinsTheTask() {
    QTemporaryDir dir; QVERIFY(FakeTasksCli::install(dir.path(), {FakeTasksCli::task(1, "todo"),
        FakeTasksCli::task(2, "todo", {{"target", FakeTasksCli::pinned("codex/api")}})}));
    HgsClient client(FakeCli::program(dir.filePath("hgs"))); TasksView view(&client); view.resize(760, 560); view.show();
    view.setContext("project-1", "orchard", "", "codex/api", "\ncodex/api", sessions());
    QTRY_COMPARE(visibleRows(view).size(), 2);
    view.retarget(todo(1), "orchard-mac", "codex/ios");
    QTRY_VERIFY(called(dir.path(), {"task", "target", "uuid-1", "codex/ios", "--host", "orchard-mac", "--project", "project-1"}));
    view.retarget(todo(2), "", "");
    QTRY_VERIFY(called(dir.path(), {"task", "target", "uuid-2", "any", "--project", "project-1"}));
    view.retarget(todo(1), "", "codex/web");
    QTRY_VERIFY(called(dir.path(), {"task", "target", "uuid-1", "codex/web", "--project", "project-1"}));
    // The row names its new session once hgs took it.
    QTRY_VERIFY(row(view, 1) && row(view, 1)->findChild<QLabel *>("taskMeta")->text().contains("codex/web"));
}

void TestTaskDrag::aRowAListingDropsLivesUntilItsDragEnds() {
    QTemporaryDir dir; QVERIFY(FakeTasksCli::install(dir.path(), {FakeTasksCli::task(1, "todo"), FakeTasksCli::task(2, "todo")}));
    HgsClient client(FakeCli::program(dir.filePath("hgs"))); LoopingTasksView view(&client); view.resize(760, 560); view.show();
    view.setContext("project-1", "orchard", "", "codex/api", "\ncodex/api", sessions());
    QTRY_COMPARE(visibleRows(view).size(), 2);
    QPointer<QWidget> source(row(view, 1)); bool kept = false, shown = true;
    view.during = [&] {
        // While the person drags, an agent sends the task: the next listing has it In progress.
        QVERIFY(FakeTasksCli::install(dir.path(), {FakeTasksCli::task(1, "active"), FakeTasksCli::task(2, "todo")}));
        view.refresh();
        QElapsedTimer waited; waited.start();
        while (visibleRows(view) != QStringList{"T-000002"} && waited.elapsed() < 5000) QTest::qWait(20);
        kept = !source.isNull(); shown = source && source->isVisible();
    };
    // Qt goes on handling the move that started the drag; the row it reaches stays alive, hidden.
    QMouseEvent move(QEvent::MouseMove, QPointF(100, 50 + QApplication::startDragDistance() + 2), QPointF(), Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
    QTest::mousePress(source, Qt::LeftButton, {}, QPoint(100, 50)); QApplication::sendEvent(source, &move);
    QVERIFY(move.isAccepted()); QVERIFY(kept); QVERIFY(!shown);
    QTest::mouseRelease(source, Qt::LeftButton, {}, QPoint(100, 80));
    // Once the drag is over the row goes.
    QTRY_VERIFY(source.isNull());
}

void TestTaskDrag::theHeaderLineAndTheReportStartDragsAndClicks() {
    QWidget host; auto *taskRow = new TaskRow(&host); host.resize(600, 100); taskRow->resize(600, 100); host.show();
    QSignalSpy started(taskRow, &TaskRow::dragStarted), opened(taskRow, &TaskRow::opened);
    taskRow->setTask(item(1, "todo"), {}, TaskColors::of(true), 0);
    // A real press lands on the label under the pointer, and the moves and the release follow it there.
    auto *meta = taskRow->findChild<QLabel *>("taskMeta"); const QPoint onMeta(20, meta->height() / 2);
    pressMoveRelease(meta, onMeta, QApplication::startDragDistance() + 2); QCOMPARE(started.size(), 1);
    pressMoveRelease(meta, onMeta, 0); QCOMPARE(opened.size(), 1);
    // In review rows open from their report too.
    taskRow->setTask(item(2, "review", {{"session", FakeTasksCli::pinned("codex/api")}, {"summary", "Token bucket; 6 tests."}}), {}, TaskColors::of(true), 0);
    auto *summary = taskRow->findChild<QLabel *>("taskSummary"); QVERIFY(summary->isVisible());
    pressMoveRelease(summary, QPoint(20, summary->height() / 2), 0); QCOMPARE(opened.size(), 2);
}

QTEST_MAIN(TestTaskDrag)
#include "test_taskdrag.moc"
