#include "FakeTasksCli.h"
#include "HgsClient.h"
#include "TaskDrag.h"
#include "TaskRow.h"
#include "TasksView.h"
#include <QAbstractButton>
#include <QApplication>
#include <QDragEnterEvent>
#include <QDragLeaveEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QMouseEvent>
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
    void thePayloadNamesTheTaskAndItsProject();
    void toDoAndBacklogRowsStartADrag();
    void theOtherListsTileTakesADroppedTask();
    void toDoRowsAreReorderedByDropping();
    void theFunnelReordersAmongTheRowsItShows();
    void aLongToDoListScrollsAtItsEdgesWhileDragging();
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

QTEST_MAIN(TestTaskDrag)
#include "test_taskdrag.moc"
