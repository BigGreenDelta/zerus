#include "FakeTasksCli.h"
#include "TaskDrag.h"
#include "TaskRow.h"
#include <QApplication>
#include <QMimeData>
#include <QMouseEvent>
#include <QSignalSpy>
#include <QTest>
#include <memory>

namespace {
TaskItem item(int n, const QString &stage, const QJsonObject &extra = {}) { return TaskBoard::item(FakeTasksCli::task(n, stage, extra)); }
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
private slots:
    void thePayloadNamesTheTaskAndItsProject();
    void toDoAndBacklogRowsStartADrag();
};

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

QTEST_MAIN(TestTaskDrag)
#include "test_taskdrag.moc"
