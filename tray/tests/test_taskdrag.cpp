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
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <memory>

namespace {
QList<TaskSession> sessions() { return {{"", "codex/api", "Ready", ""}, {"", "codex/web", "Working", ""}, {"orchard-mac", "codex/ios", "Ready", ""}}; }
TaskItem item(int n, const QString &stage, const QJsonObject &extra = {}) { return TaskBoard::item(FakeTasksCli::task(n, stage, extra)); }
QAbstractButton *tile(TasksView &view, const QString &stage) { return view.findChild<QAbstractButton *>("taskTile:" + stage); }
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

QTEST_MAIN(TestTaskDrag)
#include "test_taskdrag.moc"
