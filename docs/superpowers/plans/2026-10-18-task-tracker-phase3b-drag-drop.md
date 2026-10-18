# Task tracker, phase 3b: dragging tasks — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Drag To do and Backlog tasks in the desktop Tasks tab: onto the other list's tile to move them there, within To do to reorder it, onto a session of their project in the session list to pin them to it, or onto the project's header to make them Any session.

**Architecture:** A dragged row carries a `TaskDrag` (project ID, task UUID and ID, stage, pinned session) as `application/x-zerus-task` mime data; every drop target reads only that payload and turns the drop into one `hgs task` call (`stage`, `move --before|--after`, `target`), after which the tab refreshes as for any action. `TasksView` starts the `QDrag` and takes drops on its tiles and its list page through an event filter; `SessionList` takes drops on rows the window marks with the session they stand for (`SessionRoles::TaskTarget`) and on project headers, and `SessionsWindow` hands them to `TasksView::retarget`. hgs needs no change: `move`, `stage` and `target` exist since phase 1.

**Tech Stack:** Qt 6 Widgets, C++17, QtTest offscreen with synthetic drag events and the fake `hgs task` (`tray/tests/support/FakeTasksCli.h`).

**Spec:** `docs/superpowers/specs/2026-10-13-project-task-queue-design.md` (Desktop interface: stage tiles, list, changing the session by dragging; Model; In review and Done for "goes to the top of the list it joins") and phase 3 of `docs/superpowers/specs/2026-10-18-task-replication-loro-design.md`. The approved prototype is `design/task-tracker/index.html` (`moveTask`, `retarget`, the `drop-on`/`drop-before`/`dragging` styles). Phase 3a (`docs/superpowers/plans/2026-10-18-task-tracker-phase3a-desktop.md`) built the tab this plan extends.

## Global Constraints

- Work on branch `task-tracker`, which the shared main checkout has checked out; never switch branches, merge, rebase or push. Other sessions may work in the same checkout: stage only this plan's files.
- Never rebuild binaries a running Zerus uses: build the GUI in `.ci-build/gui`, never in `tray/build`.
- The GUI build: configure once with
  `cmake -S tray -B .ci-build/gui -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON $(grep -E '^(Qt6_DIR|KF6[A-Za-z]*_DIR)[:=]' tray/build/CMakeCache.txt 2>/dev/null | sed 's/^\([^:=]*\)[^=]*=/-D\1=/')`
  (it takes the Qt and KF6 paths of an existing local tray build, if any); after adding a suite reconfigure with `cmake -S tray -B .ci-build/gui >/dev/null`; build one suite with `cmake --build .ci-build/gui --target <suite> -j 8` and run it with `QT_QPA_PLATFORM=offscreen .ci-build/gui/tests/<suite>`.
- Tray code follows its surroundings: compact Qt C++17, objectNames for everything tests reach, colours per theme through `setTheme(bool dark)`.
- The interface is English. Synthetic names only (`orchard`, `codex/api`, `orchard-mac`); never real project or session names.
- Only To do and Backlog tasks this Zerus can change (`writable`) are dragged; tasks of other projects are never taken; a drop that would change nothing is refused (no outline, no line, no hgs call).

## Decisions this plan takes

The spec and the prototype leave these open; each is the smallest reading that keeps the spec's rules.

- A tile takes a task from the other list only (a To do task on Backlog, a Backlog task on To do). Its own tile and the In review, In progress and Done tiles refuse it. hgs puts the task at the top of the list it joins (`hgs task stage`), as the spec says for every change of list; the prototype's "to the end" is not followed. The selected tile stays.
- Only To do is reordered (spec: "Rows in To do can be reordered by dragging"); the prototype also reordered Backlog. A drop goes before the row below the line (`move --before`), or after the last row when it lands below all of them (`move --after`). With "Only this session" on, the anchors are the rows shown; hidden tasks keep their places.
- In the session list, a task drops on the live sessions of its project (not on subagent rows, pending launches or forgotten sessions) and on its project's header (Any session). Its current session, and the header for an Any session task, refuse it. A session on a computer this one has not verified is never taken for this computer's session of the same name.
- A successful drop shows nothing but its result; a refusal from hgs is shown like any refused action (the window's notice) and the list is read again.
- The list scrolls while a dragged task is held within 28 px of its top or bottom (8 px every 30 ms); the session list keeps Qt's own edge scrolling.

## Review Focus

- A listing that arrives during a drag re-renders the rows, and may remove the dragged one when an agent or another computer moved the task meanwhile: the drag carries the task, so the drop still names it; the source row is guarded (`QPointer`) and the line and auto-scroll end with the drag. `QDrag::exec` does not run offscreen, so `startDrag` is reviewed by reading.
- Real drag delivery: rows and their labels do not accept drops, so drag events over a row reach the list page with page coordinates, and the session list gets them on its viewport; the tests send the events to those widgets directly. Check once by hand in a running build (Task 4, last step).
- A drag cancelled with Escape or leaving the widget: the tile outline, the drop line, the session outline and the auto-scroll end on `DragLeave` — `theOtherListsTileTakesADroppedTask`, `toDoRowsAreReorderedByDropping`, `aLongToDoListScrollsAtItsEdgesWhileDragging`.
- A drop on a task that changed stage since the drag began (sent by an agent, moved elsewhere): hgs refuses or applies its own rule; the refusal is shown and the list re-read — `theOtherListsTileTakesADroppedTask` (refused `stage`).
- The funnel ("Only this session") hiding tasks between the shown rows: a reorder anchors on a shown row — `theFunnelReordersAmongTheRowsItShows`.

## File Structure

| Path | Responsibility |
|---|---|
| `tray/src/TaskDrag.h` | The dragged task's payload, what may be dragged |
| `tray/src/TaskRow.h/.cpp` | Starting a row's drag; the faded row while it runs |
| `tray/src/TasksView.h/.cpp` | The drag itself; drops on the tiles and on the To do list; the drop line and auto-scroll; `retarget` |
| `tray/src/SessionList.h/.cpp` | Drops on sessions and project headers; their dashed outline |
| `tray/src/SessionsWindow.cpp` | Which rows stand for a session (`SessionRoles::TaskTarget`); hands drops to the tab |
| `tray/tests/test_taskdrag.cpp`, `tray/tests/CMakeLists.txt` | The drag suite |
| `tray/tests/support/FakeTasksCli.h` | `move --before/--after` in the fake |
| `tray/tests/test_sessionswindow.cpp` | Drops in the real window |
| `docs/reference.md` | Dragging in the Tasks tab |

**Known limits of this phase:** dragging is the mouse's; the keyboard keeps the row actions (Move to top, Move to Backlog, Move to To do) and the session choice in the task settings. Existing, outside this plan: `colorPickerUsesWorkspaceThemeAndCancelsOutside(dark)` fails in shard 7 of `test_sessionswindow` on some machines also without this work.

---

### Task 0: Record the plan

- [ ] **Step 1: Check the branch and privacy**

Run: `git branch --show-current && python3 scripts/check-source-privacy.py`
Expected: `task-tracker`; `0 privacy guard findings`.

- [ ] **Step 2: Commit the plan**

```bash
git add docs/superpowers/plans/2026-10-18-task-tracker-phase3b-drag-drop.md
git commit -m "Plan dragging tasks in the Tasks tab"
```

---

### Task 1: The dragged task and the row that starts it

**Files:**
- Create: `tray/tests/test_taskdrag.cpp`, `tray/src/TaskDrag.h`
- Modify: `tray/tests/CMakeLists.txt`, `tray/src/TaskRow.cpp`, `tray/src/TaskRow.h`

**Interfaces:**
- Consumes: `TaskItem`, `TaskBoard::item` (phase 3a), `TaskRow` with its press tracking (`m_pressed`, set only outside the actions).
- Produces: header-only `tray/src/TaskDrag.h`: `struct TaskDrag { QString projectId, uuid, id, stage, targetHost, targetSession; bool targetKnown = true; static QString mimeType(); static bool draggable(const TaskItem &); static TaskDrag of(const QString &projectId, const TaskItem &); QMimeData *mimeData() const; static std::optional<TaskDrag> from(const QMimeData *); }` with `Q_DECLARE_METATYPE(TaskDrag)`; mime type `application/x-zerus-task`, JSON `{project, uuid, id, stage, host, session, known}`; `from` refuses other data and payloads without project or UUID. `TaskRow` signal `dragStarted(const QPoint &hotSpot)` and `setDragging(bool)` (property `dragging`, a 40% `QGraphicsOpacityEffect`). The suite `test_taskdrag` (target and ctest name `taskdrag`).

A press on a To do or Backlog row outside its actions starts a drag once the pointer moves `QApplication::startDragDistance()`; a smaller shake is still a click, and the release after a drag opens nothing.

- [ ] **Step 1: Write the failing tests**

Apply this change to `tray/tests/CMakeLists.txt` (save it and run `git apply`, or edit by hand):

````diff
diff --git a/tray/tests/CMakeLists.txt b/tray/tests/CMakeLists.txt
index 882c44b6..f3928e19 100644
--- a/tray/tests/CMakeLists.txt
+++ b/tray/tests/CMakeLists.txt
@@ -255,6 +255,13 @@ target_link_libraries(test_taskdialogs PRIVATE Qt6::Test Qt6::Widgets hgs-tray-u
 zerus_add_test(taskdialogs test_taskdialogs)
 set_tests_properties(taskdialogs PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
 
+# Dragging tasks out of the Tasks tab (TaskDrag.h).
+add_executable(test_taskdrag test_taskdrag.cpp)
+target_include_directories(test_taskdrag PRIVATE ../src)
+target_link_libraries(test_taskdrag PRIVATE Qt6::Test Qt6::Widgets hgs-tray-ui)
+zerus_add_test(taskdrag test_taskdrag)
+set_tests_properties(taskdrag PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
+
 add_executable(test_processview test_processview.cpp)
 target_include_directories(test_processview PRIVATE ../src)
 target_link_libraries(test_processview PRIVATE Qt6::Test Qt6::Widgets hgs-tray-ui)
````

Create `tray/tests/test_taskdrag.cpp`:

````cpp
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
````

- [ ] **Step 2: Run them to see them fail**

Run: `cmake -S tray -B .ci-build/gui >/dev/null && cmake --build .ci-build/gui --target test_taskdrag -j 8`
Expected: FAIL to compile — `TaskDrag.h: No such file or directory`.

- [ ] **Step 3: Implement**

Create `tray/src/TaskDrag.h`:

````cpp
#pragma once
#include "TaskBoard.h"
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaType>
#include <QMimeData>
#include <optional>

// A To do or Backlog task dragged out of the Tasks tab, as its drop targets read
// it: the stage tiles move it, the To do list reorders it and the session list
// pins it to a session or makes it Any session.
struct TaskDrag {
    QString projectId, uuid, id, stage;
    // Its pinned session; an empty name is Any session. targetKnown is false for a
    // computer this one has not verified.
    QString targetHost, targetSession;
    bool targetKnown = true;

    static QString mimeType() { return QStringLiteral("application/x-zerus-task"); }
    // Rows that can be dragged: To do and Backlog tasks this Zerus can change.
    static bool draggable(const TaskItem &task) {
        return task.writable && (task.stage == QLatin1String("todo") || task.stage == QLatin1String("backlog"));
    }
    static TaskDrag of(const QString &projectId, const TaskItem &task) {
        return {projectId, task.uuid, task.id, task.stage, task.targetHost, task.targetSession, task.targetKnown};
    }
    QMimeData *mimeData() const {
        auto *mime = new QMimeData;
        const QJsonObject data{{"project", projectId}, {"uuid", uuid}, {"id", id}, {"stage", stage},
                               {"host", targetHost}, {"session", targetSession}, {"known", targetKnown}};
        mime->setData(mimeType(), QJsonDocument(data).toJson(QJsonDocument::Compact));
        return mime;
    }
    static std::optional<TaskDrag> from(const QMimeData *mime) {
        if (!mime || !mime->hasFormat(mimeType())) return std::nullopt;
        const auto data = QJsonDocument::fromJson(mime->data(mimeType())).object();
        TaskDrag drag{data.value("project").toString(), data.value("uuid").toString(), data.value("id").toString(),
                      data.value("stage").toString(), data.value("host").toString(), data.value("session").toString(),
                      data.value("known").toBool(true)};
        if (drag.projectId.isEmpty() || drag.uuid.isEmpty()) return std::nullopt;
        return drag;
    }
};
Q_DECLARE_METATYPE(TaskDrag)
````

Apply this change to `tray/src/TaskRow.cpp` (save it and run `git apply`, or edit by hand):

````diff
diff --git a/tray/src/TaskRow.cpp b/tray/src/TaskRow.cpp
index bb7e099f..b1726d98 100644
--- a/tray/src/TaskRow.cpp
+++ b/tray/src/TaskRow.cpp
@@ -1,8 +1,10 @@
 #include "TaskRow.h"
+#include "TaskDrag.h"
 #include "WorkspaceIcons.h"
 #include "WorkspaceStyle.h"
 #include <QApplication>
 #include <QDateTime>
+#include <QGraphicsOpacityEffect>
 #include <QHBoxLayout>
 #include <QLabel>
 #include <QMouseEvent>
@@ -206,6 +208,22 @@ void TaskRow::mousePressEvent(QMouseEvent *event) {
     QWidget::mousePressEvent(event);
     m_pressed = event->button() == Qt::LeftButton && !m_actions->geometry().translated(m_actions->parentWidget()->mapTo(this, QPoint()))
                                                          .contains(event->position().toPoint());
+    m_pressAt = event->position().toPoint();
+}
+
+// A To do or Backlog row pressed outside its actions is dragged once the pointer
+// moves past the drag distance; the release after a drag opens nothing.
+void TaskRow::mouseMoveEvent(QMouseEvent *event) {
+    QWidget::mouseMoveEvent(event);
+    if (!m_pressed || !(event->buttons() & Qt::LeftButton) || !TaskDrag::draggable(m_task)) return;
+    if ((event->position().toPoint() - m_pressAt).manhattanLength() < QApplication::startDragDistance()) return;
+    m_pressed = false; emit dragStarted(m_pressAt);
+}
+
+void TaskRow::setDragging(bool dragging) {
+    setProperty("dragging", dragging);
+    if (!dragging) { setGraphicsEffect(nullptr); return; }
+    auto *fade = new QGraphicsOpacityEffect(this); fade->setOpacity(0.4); setGraphicsEffect(fade);
 }
 
 void TaskRow::mouseReleaseEvent(QMouseEvent *event) {
````

Apply this change to `tray/src/TaskRow.h` (save it and run `git apply`, or edit by hand):

````diff
diff --git a/tray/src/TaskRow.h b/tray/src/TaskRow.h
index 35816e49..773189be 100644
--- a/tray/src/TaskRow.h
+++ b/tray/src/TaskRow.h
@@ -32,9 +32,13 @@ public:
     void setTask(const TaskItem &task, const TaskRowState &state, const TaskColors &colors, qint64 now);
     const TaskItem &task() const { return m_task; }
     static QString actionKey(TaskAction action);
+    // While its drag runs the row stays in its place, faded.
+    void setDragging(bool dragging);
 signals:
     void actionRequested(TaskAction action);
     void opened();
+    // A To do or Backlog row was pressed and moved: the owner drags it from hotSpot.
+    void dragStarted(const QPoint &hotSpot);
 protected:
     void paintEvent(QPaintEvent *event) override;
     void enterEvent(QEnterEvent *event) override;
@@ -44,6 +48,7 @@ protected:
     void keyPressEvent(QKeyEvent *event) override;
     bool eventFilter(QObject *watched, QEvent *event) override;
     void mousePressEvent(QMouseEvent *event) override;
+    void mouseMoveEvent(QMouseEvent *event) override;
     void mouseReleaseEvent(QMouseEvent *event) override;
     void resizeEvent(QResizeEvent *event) override;
 private:
@@ -57,5 +62,6 @@ private:
     QLabel *m_glyph, *m_meta, *m_text, *m_summary;
     QWidget *m_actions;
     QHash<int, QToolButton *> m_buttons;
+    QPoint m_pressAt;
     bool m_hovered = false, m_pressed = false;
 };
````

- [ ] **Step 4: Run them to see them pass**

Run:
```bash
cmake --build .ci-build/gui --target test_taskdrag test_tasksview -j 8
QT_QPA_PLATFORM=offscreen .ci-build/gui/tests/test_taskdrag
QT_QPA_PLATFORM=offscreen .ci-build/gui/tests/test_tasksview
```
Expected: `test_taskdrag` `Totals: 4 passed`; `test_tasksview` `Totals: 17 passed` (rows still open on a click).

- [ ] **Step 5: Commit**

```bash
git add tray/src/TaskDrag.h \
  tray/src/TaskRow.cpp \
  tray/src/TaskRow.h \
  tray/tests/CMakeLists.txt \
  tray/tests/test_taskdrag.cpp
git commit -m "Drag To do and Backlog rows out of the Tasks list"
```

---

### Task 2: Dropping on the other list's tile

**Files:**
- Modify: `tray/tests/test_taskdrag.cpp`, `tray/src/TasksView.cpp`, `tray/src/TasksView.h`

**Interfaces:**
- Consumes: `TaskDrag`, `TaskRow::dragStarted`, `TaskRow::setDragging` (Task 1); `TasksView::run`, `withProject` (phase 3a).
- Produces: every stage tile accepts drops (`acceptDrops()`), with property `dropTarget` while it would take the dragged task (a dashed outline in the stage colour); `TasksView::eventFilter(QObject *, QEvent *)` (protected override, handles the drag events of the tiles), private `startDrag(TaskRow *, const QPoint &hotSpot)` and `takesOnTile(const TaskDrag &, const QString &stage) const`. A drop runs `hgs task stage <uuid> todo|backlog --project <id>`.

`startDrag` grabs the row as the drag's picture, fades the row and runs `QDrag::exec(Qt::MoveAction)` with the `QDrag` parented to the view (Qt deletes it after the drag); a listing may replace the row meanwhile, so the row is held in a `QPointer`.

- [ ] **Step 1: Write the failing tests**

Apply this change to `tray/tests/test_taskdrag.cpp` (save it and run `git apply`, or edit by hand):

````diff
diff --git a/tray/tests/test_taskdrag.cpp b/tray/tests/test_taskdrag.cpp
index 1995fdcc..194f8b48 100644
--- a/tray/tests/test_taskdrag.cpp
+++ b/tray/tests/test_taskdrag.cpp
@@ -1,15 +1,45 @@
 #include "FakeTasksCli.h"
+#include "HgsClient.h"
 #include "TaskDrag.h"
 #include "TaskRow.h"
+#include "TasksView.h"
+#include <QAbstractButton>
 #include <QApplication>
+#include <QDragEnterEvent>
+#include <QDragLeaveEvent>
+#include <QDragMoveEvent>
+#include <QDropEvent>
 #include <QMimeData>
 #include <QMouseEvent>
+#include <QSettings>
 #include <QSignalSpy>
+#include <QTemporaryDir>
 #include <QTest>
 #include <memory>
 
 namespace {
+QList<TaskSession> sessions() { return {{"", "codex/api", "Ready", ""}, {"", "codex/web", "Working", ""}, {"orchard-mac", "codex/ios", "Ready", ""}}; }
 TaskItem item(int n, const QString &stage, const QJsonObject &extra = {}) { return TaskBoard::item(FakeTasksCli::task(n, stage, extra)); }
+QAbstractButton *tile(TasksView &view, const QString &stage) { return view.findChild<QAbstractButton *>("taskTile:" + stage); }
+// Drags `drag` into `target` and over `point` (its centre by default); true when the target takes it there.
+bool dragOver(QWidget *target, const TaskDrag &drag, QPoint point = {}) {
+    if (point.isNull()) point = target->rect().center();
+    std::unique_ptr<QMimeData> mime(drag.mimeData());
+    QDragEnterEvent enter(point, Qt::MoveAction, mime.get(), Qt::LeftButton, Qt::NoModifier); QApplication::sendEvent(target, &enter);
+    if (!enter.isAccepted()) return false;
+    QDragMoveEvent move(point, Qt::MoveAction, mime.get(), Qt::LeftButton, Qt::NoModifier); QApplication::sendEvent(target, &move);
+    return move.isAccepted();
+}
+// Drags `drag` over `point` of `target` and drops it there; true when the target took the drop.
+bool drop(QWidget *target, const TaskDrag &drag, QPoint point = {}) {
+    if (point.isNull()) point = target->rect().center();
+    if (!dragOver(target, drag, point)) return false;
+    std::unique_ptr<QMimeData> mime(drag.mimeData());
+    QDropEvent event(QPointF(point), Qt::MoveAction, mime.get(), Qt::LeftButton, Qt::NoModifier); QApplication::sendEvent(target, &event);
+    return event.isAccepted();
+}
+void leave(QWidget *target) { QDragLeaveEvent event; QApplication::sendEvent(target, &event); }
+bool called(const QDir &dir, const QStringList &argv) { return FakeTasksCli::calls(dir).contains(argv); }
 // A press at `from`, a move by `distance` with the button held, and the release.
 void pressMoveRelease(QWidget *widget, QPoint from, int distance) {
     const QPoint to = from + QPoint(0, distance);
@@ -22,11 +52,21 @@ void pressMoveRelease(QWidget *widget, QPoint from, int distance) {
 
 class TestTaskDrag : public QObject {
     Q_OBJECT
+    QTemporaryDir m_settings;
 private slots:
+    void initTestCase();
     void thePayloadNamesTheTaskAndItsProject();
     void toDoAndBacklogRowsStartADrag();
+    void theOtherListsTileTakesADroppedTask();
 };
 
+void TestTaskDrag::initTestCase() {
+    QVERIFY(m_settings.isValid());
+    QCoreApplication::setOrganizationName("hgs-tests"); QCoreApplication::setApplicationName("task-drag");
+    QSettings::setDefaultFormat(QSettings::IniFormat);
+    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, m_settings.path());
+}
+
 void TestTaskDrag::thePayloadNamesTheTaskAndItsProject() {
     const auto drag = TaskDrag::of("project-1", item(7, "todo", {{"target", FakeTasksCli::pinned("codex/ios", "orchard-mac")}}));
     std::unique_ptr<QMimeData> mime(drag.mimeData());
@@ -70,5 +110,34 @@ void TestTaskDrag::toDoAndBacklogRowsStartADrag() {
     taskRow->setDragging(false); QVERIFY(!taskRow->property("dragging").toBool()); QVERIFY(!taskRow->graphicsEffect());
 }
 
+void TestTaskDrag::theOtherListsTileTakesADroppedTask() {
+    QTemporaryDir dir; QVERIFY(FakeTasksCli::install(dir.path(), {FakeTasksCli::task(1, "todo"), FakeTasksCli::task(2, "todo"), FakeTasksCli::task(3, "backlog")}));
+    HgsClient client(FakeCli::program(dir.filePath("hgs"))); TasksView view(&client); view.resize(760, 560); view.show();
+    QSignalSpy notices(&view, &TasksView::notice);
+    view.setContext("project-1", "orchard", "", "codex/api", "\ncodex/api", sessions());
+    QTRY_COMPARE(tile(view, "todo")->property("count").toInt(), 2);
+    const auto todo = TaskDrag::of("project-1", item(2, "todo")), backlog = TaskDrag::of("project-1", item(3, "backlog"));
+    for (const char *stage : {"review", "active", "todo", "backlog", "done"}) QVERIFY2(tile(view, stage)->acceptDrops(), stage);
+    // The other list's tile takes the task and shows that it will.
+    QVERIFY(dragOver(tile(view, "backlog"), todo)); QVERIFY(tile(view, "backlog")->property("dropTarget").toBool());
+    leave(tile(view, "backlog")); QVERIFY(!tile(view, "backlog")->property("dropTarget").toBool());
+    // Its own tile, the other stages and another project's tasks refuse it.
+    QVERIFY(!dragOver(tile(view, "todo"), todo)); QVERIFY(!tile(view, "todo")->property("dropTarget").toBool());
+    for (const char *stage : {"review", "active", "done"}) QVERIFY2(!dragOver(tile(view, stage), todo), stage);
+    QVERIFY(!dragOver(tile(view, "backlog"), TaskDrag::of("project-2", item(2, "todo"))));
+    // A drop moves it there; hgs puts it at the top of that list.
+    QVERIFY(drop(tile(view, "backlog"), todo)); QVERIFY(!tile(view, "backlog")->property("dropTarget").toBool());
+    QTRY_COMPARE(tile(view, "backlog")->property("count").toInt(), 2);
+    QVERIFY(called(dir.path(), {"task", "stage", "uuid-2", "backlog", "--project", "project-1"}));
+    QVERIFY(drop(tile(view, "todo"), backlog));
+    QTRY_COMPARE(tile(view, "todo")->property("count").toInt(), 2);
+    QVERIFY(called(dir.path(), {"task", "stage", "uuid-3", "todo", "--project", "project-1"}));
+    QCOMPARE(view.selectedStage(), QString("todo"));   // the list shown stays
+    // A refusal is named and the listing read again.
+    QFile fail(dir.filePath("fail")); QVERIFY(fail.open(QIODevice::WriteOnly)); fail.write("stage"); fail.close();
+    QVERIFY(drop(tile(view, "backlog"), TaskDrag::of("project-1", item(1, "todo"))));
+    QTRY_COMPARE(notices.size(), 1); QCOMPARE(notices[0][0].toString(), QString("hgs refused: stage")); QVERIFY(notices[0][1].toBool());
+}
+
 QTEST_MAIN(TestTaskDrag)
 #include "test_taskdrag.moc"
````

- [ ] **Step 2: Run them to see them fail**

Run: `cmake --build .ci-build/gui --target test_taskdrag -j 8 && QT_QPA_PLATFORM=offscreen .ci-build/gui/tests/test_taskdrag`
Expected: `FAIL!  : TestTaskDrag::theOtherListsTileTakesADroppedTask() 'tile(view, stage)->acceptDrops()' returned FALSE. (review)`; `Totals: 4 passed, 1 failed`.

- [ ] **Step 3: Implement**

Apply this change to `tray/src/TasksView.cpp` (save it and run `git apply`, or edit by hand):

````diff
diff --git a/tray/src/TasksView.cpp b/tray/src/TasksView.cpp
index 5553e819..6e1cf854 100644
--- a/tray/src/TasksView.cpp
+++ b/tray/src/TasksView.cpp
@@ -6,6 +6,8 @@
 #include "WorkspaceStyle.h"
 #include <QAbstractButton>
 #include <QDateTime>
+#include <QDrag>
+#include <QDropEvent>
 #include <QHBoxLayout>
 #include <QJsonDocument>
 #include <QLabel>
@@ -38,6 +40,8 @@ public:
         setProperty("count", count); setAccessibleName(QStringLiteral("%1, %2").arg(TaskBoard::stageLabel(m_stage)).arg(count)); update();
     }
     void setColors(const TaskColors &colors) { m_colors = colors; update(); }
+    // A dashed outline while a dragged task would move here.
+    void setDropTarget(bool on) { if (property("dropTarget").toBool() != on) { setProperty("dropTarget", on); update(); } }
     QSize sizeHint() const override { return {120, 34}; }
 protected:
     void paintEvent(QPaintEvent *) override {
@@ -47,6 +51,9 @@ protected:
         const QColor edge = isChecked() ? alpha(color, 0.45) : underMouse() ? m_colors.border : Qt::transparent;
         p.setPen(edge.alpha() ? QPen(edge, 1) : Qt::NoPen); p.setBrush(fill);
         p.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 9, 9);
+        if (property("dropTarget").toBool()) {
+            p.setPen(QPen(color, 2, Qt::DashLine)); p.setBrush(Qt::NoBrush); p.drawRoundedRect(QRectF(rect()).adjusted(3, 3, -3, -3), 7, 7);
+        }
         p.setPen(Qt::NoPen); p.setBrush(alpha(color, 0.18)); p.drawEllipse(QRectF(10, 6, 22, 22));
         workspaceIcon(TaskBoard::stageIcon(m_stage), color).paint(&p, QRect(14, 10, 14, 14));
         QFont count = font(); count.setPixelSize(15); count.setBold(true); p.setFont(count); p.setPen(m_colors.fg);
@@ -101,6 +108,7 @@ TasksView::TasksView(HgsClient *client, QWidget *parent) : QWidget(parent), m_cl
     auto *tiles = new QHBoxLayout; tiles->setSpacing(8);
     for (const auto &stage : TaskBoard::stages()) {
         auto *tile = new TaskTile(stage, this); m_tiles.insert(stage, tile); tiles->addWidget(tile, 1);
+        tile->setAcceptDrops(true); tile->installEventFilter(this);
         connect(tile, &QAbstractButton::clicked, this, [this, stage] { selectStage(stage); });
     }
     const auto square = [this](const QString &name) {
@@ -268,6 +276,7 @@ void TasksView::render() {
             row = new TaskRow(m_listHost); m_rows.insert(task.uuid, row);
             connect(row, &TaskRow::actionRequested, this, [this, row](TaskAction action) { act(row->task(), action); });
             connect(row, &TaskRow::opened, this, [this, row] { openTask(row->task()); });
+            connect(row, &TaskRow::dragStarted, this, [this, row](const QPoint &hotSpot) { startDrag(row, hotSpot); });
         }
         row->setObjectName(QStringLiteral("taskRow:") + task.id);
         TaskRowState state; state.mine = TaskBoard::belongsTo(task, m_host, m_session); state.anyNow = sessionLabel(m_host, m_session);
@@ -367,6 +376,43 @@ void TasksView::addTask(const QString &key, const QString &text) {
     });
 }
 
+void TasksView::startDrag(TaskRow *row, const QPoint &hotSpot) {
+    if (m_projectId.isEmpty()) return;
+    // A listing may replace the row while the drag runs; the drag carries the task.
+    QPointer<TaskRow> source(row);
+    auto *drag = new QDrag(this);
+    drag->setMimeData(TaskDrag::of(m_projectId, row->task()).mimeData());
+    drag->setPixmap(row->grab()); drag->setHotSpot(hotSpot);
+    row->setDragging(true);
+    drag->exec(Qt::MoveAction);
+    if (source) source->setDragging(false);
+}
+
+bool TasksView::takesOnTile(const TaskDrag &drag, const QString &stage) const {
+    const auto list = [](const QString &s) { return s == QLatin1String("todo") || s == QLatin1String("backlog"); };
+    return drag.projectId == m_projectId && list(stage) && list(drag.stage) && drag.stage != stage;
+}
+
+bool TasksView::eventFilter(QObject *watched, QEvent *event) {
+    const auto type = event->type();
+    if (type != QEvent::DragEnter && type != QEvent::DragMove && type != QEvent::DragLeave && type != QEvent::Drop)
+        return QWidget::eventFilter(watched, event);
+    for (auto it = m_tiles.cbegin(); it != m_tiles.cend(); ++it) {
+        if (it.value() != watched) continue;
+        auto *tile = it.value();
+        if (type == QEvent::DragLeave) { tile->setDropTarget(false); return true; }
+        auto *drop = static_cast<QDropEvent *>(event);
+        const auto drag = TaskDrag::from(drop->mimeData());
+        const bool takes = drag && takesOnTile(*drag, it.key());
+        tile->setDropTarget(takes && type != QEvent::Drop);
+        if (!takes) { drop->ignore(); return true; }
+        drop->setDropAction(Qt::MoveAction); drop->accept();
+        if (type == QEvent::Drop) run({{withProject({"stage", drag->uuid, it.key()}), {}}});
+        return true;
+    }
+    return QWidget::eventFilter(watched, event);
+}
+
 void TasksView::openTask(const TaskItem &task) {
     request(withProject({"get", task.uuid}), {}, [this, task](const QJsonObject &reply) {
         for (auto *open : findChildren<TaskDialog *>()) open->reject();
````

Apply this change to `tray/src/TasksView.h` (save it and run `git apply`, or edit by hand):

````diff
diff --git a/tray/src/TasksView.h b/tray/src/TasksView.h
index bdcdc0c0..e1c8d4ad 100644
--- a/tray/src/TasksView.h
+++ b/tray/src/TasksView.h
@@ -1,5 +1,6 @@
 #pragma once
 #include "TaskBoard.h"
+#include "TaskDrag.h"
 #include "TaskRow.h"
 #include <QHash>
 #include <QJsonObject>
@@ -75,7 +76,13 @@ signals:
 protected:
     void showEvent(QShowEvent *event) override;
     void hideEvent(QHideEvent *event) override;
+    // Drops on the stage tiles.
+    bool eventFilter(QObject *watched, QEvent *event) override;
 private:
+    // Drags a To do or Backlog row; the tiles and the session list take it.
+    void startDrag(TaskRow *row, const QPoint &hotSpot);
+    // The To do and Backlog tiles take a task of this project from the other list.
+    bool takesOnTile(const TaskDrag &drag, const QString &stage) const;
     void request(const QStringList &arguments, const QString &input, std::function<void(const QJsonObject &)> done,
                  std::function<void(const QString &)> failed = {});
     void received(const QJsonObject &listing, quint64 generation);
````

- [ ] **Step 4: Run them to see them pass**

Run: `cmake --build .ci-build/gui --target test_taskdrag -j 8 && QT_QPA_PLATFORM=offscreen .ci-build/gui/tests/test_taskdrag`
Expected: `Totals: 5 passed`.

- [ ] **Step 5: Commit**

```bash
git add tray/src/TasksView.cpp \
  tray/src/TasksView.h \
  tray/tests/test_taskdrag.cpp
git commit -m "Move a dragged task by dropping it on the other list's tile"
```

---

### Task 3: Reordering To do

**Files:**
- Modify: `tray/tests/support/FakeTasksCli.h`, `tray/tests/test_taskdrag.cpp`, `tray/src/TasksView.cpp`, `tray/src/TasksView.h`

**Interfaces:**
- Consumes: `TaskDrag`, `TasksView::eventFilter`, `startDrag` (Tasks 1–2).
- Produces: the list page `taskListPage` accepts drops; `taskDropLine` (a 2 px line in the To do colour) shows where the task would go; private `takesInList`, `shownRows() -> QList<TaskRow *>` (display order, hidden rows left out), `dropGap(uuid, point) -> int` (the shown row the task goes before, their count below the last one, -1 where it already is), `followDrag`, `reorder`, `endListDrag`; members `m_listPage`, `m_dropLine`, `m_autoScroll`, `m_drag`, `m_dragAt`, `m_scrollStep`. A drop runs `hgs task move <uuid> --before <uuid> --project <id>`, or `--after <last shown uuid>` below the rows. The fake `hgs task move` honours `--before`/`--after`.

Over a row's upper half the line shows above it, over its lower half below it; where the task already is there is no line and a drop calls nothing. Backlog, other stages, Backlog tasks and other projects' tasks refuse the drop. While the pointer is within 28 px of the visible list's top or bottom a 30 ms timer scrolls by 8 px and moves the line with the rows; the drag leaving, a drop or the end of the drag stops it.

- [ ] **Step 1: Write the failing tests**

Apply this change to `tray/tests/support/FakeTasksCli.h` (save it and run `git apply`, or edit by hand):

````diff
diff --git a/tray/tests/support/FakeTasksCli.h b/tray/tests/support/FakeTasksCli.h
index 33e3ce67..d841035e 100644
--- a/tray/tests/support/FakeTasksCli.h
+++ b/tray/tests/support/FakeTasksCli.h
@@ -55,7 +55,11 @@ if cmd=='get': out({'project':project,'task':t}); sys.exit(0)
 if cmd=='stage':
     t['stage']=a[3]; t['stage_at']=now; log(t,'stage',a[3])
     if a[3] in ('todo','backlog'): top(t)
-elif cmd=='move': top(t)
+elif cmd=='move':
+    anchor=opt('--before') or opt('--after')
+    if not anchor: top(t)
+    else:
+        data['tasks'].remove(t); i=data['tasks'].index(find(anchor)); data['tasks'].insert(i if '--before' in a else i+1,t)
 elif cmd=='send':
     session=opt('--session') or t['target']['session']
     t['stage']='active'; t['stage_at']=now; t['session']={'machine':'m-here','session':session,'host':''}; log(t,'sent')
````

Apply this change to `tray/tests/test_taskdrag.cpp` (save it and run `git apply`, or edit by hand):

````diff
diff --git a/tray/tests/test_taskdrag.cpp b/tray/tests/test_taskdrag.cpp
index 194f8b48..37949a1d 100644
--- a/tray/tests/test_taskdrag.cpp
+++ b/tray/tests/test_taskdrag.cpp
@@ -11,6 +11,8 @@
 #include <QDropEvent>
 #include <QMimeData>
 #include <QMouseEvent>
+#include <QScrollArea>
+#include <QScrollBar>
 #include <QSettings>
 #include <QSignalSpy>
 #include <QTemporaryDir>
@@ -21,6 +23,15 @@ namespace {
 QList<TaskSession> sessions() { return {{"", "codex/api", "Ready", ""}, {"", "codex/web", "Working", ""}, {"orchard-mac", "codex/ios", "Ready", ""}}; }
 TaskItem item(int n, const QString &stage, const QJsonObject &extra = {}) { return TaskBoard::item(FakeTasksCli::task(n, stage, extra)); }
 QAbstractButton *tile(TasksView &view, const QString &stage) { return view.findChild<QAbstractButton *>("taskTile:" + stage); }
+QWidget *row(TasksView &view, int n) { return view.findChild<QWidget *>(QString("taskRow:T-%1").arg(n, 6, 10, QLatin1Char('0'))); }
+QStringList visibleRows(TasksView &view) {
+    QStringList out;
+    for (auto *w : view.findChildren<QWidget *>()) if (w->objectName().startsWith("taskRow:") && w->isVisibleTo(&view)) out << w->objectName().mid(8);
+    std::sort(out.begin(), out.end(), [&](const QString &a, const QString &b) {
+        return view.findChild<QWidget *>("taskRow:" + a)->y() < view.findChild<QWidget *>("taskRow:" + b)->y(); });
+    return out;
+}
+TaskDrag todo(int n) { return TaskDrag::of("project-1", TaskBoard::item(FakeTasksCli::task(n, "todo"))); }
 // Drags `drag` into `target` and over `point` (its centre by default); true when the target takes it there.
 bool dragOver(QWidget *target, const TaskDrag &drag, QPoint point = {}) {
     if (point.isNull()) point = target->rect().center();
@@ -58,6 +69,9 @@ private slots:
     void thePayloadNamesTheTaskAndItsProject();
     void toDoAndBacklogRowsStartADrag();
     void theOtherListsTileTakesADroppedTask();
+    void toDoRowsAreReorderedByDropping();
+    void theFunnelReordersAmongTheRowsItShows();
+    void aLongToDoListScrollsAtItsEdgesWhileDragging();
 };
 
 void TestTaskDrag::initTestCase() {
@@ -139,5 +153,80 @@ void TestTaskDrag::theOtherListsTileTakesADroppedTask() {
     QTRY_COMPARE(notices.size(), 1); QCOMPARE(notices[0][0].toString(), QString("hgs refused: stage")); QVERIFY(notices[0][1].toBool());
 }
 
+void TestTaskDrag::toDoRowsAreReorderedByDropping() {
+    QTemporaryDir dir; QVERIFY(FakeTasksCli::install(dir.path(), {FakeTasksCli::task(1, "todo"), FakeTasksCli::task(2, "todo"),
+        FakeTasksCli::task(3, "todo"), FakeTasksCli::task(4, "todo"), FakeTasksCli::task(5, "backlog")}));
+    HgsClient client(FakeCli::program(dir.filePath("hgs"))); TasksView view(&client); view.resize(760, 560); view.show();
+    view.setContext("project-1", "orchard", "", "codex/api", "\ncodex/api", sessions());
+    QTRY_COMPARE(visibleRows(view), (QStringList{"T-000001", "T-000002", "T-000003", "T-000004"}));
+    auto *page = view.findChild<QWidget *>("taskListPage"); auto *line = view.findChild<QWidget *>("taskDropLine");
+    QVERIFY(page && line && page->acceptDrops());
+    const auto at = [&](int n, double share) { auto *r = row(view, n); return r->mapTo(page, QPoint(r->width() / 2, int(r->height() * share))); };
+    const auto top = [&](int n) { return row(view, n)->mapTo(page, QPoint()).y(); };
+    const auto bottom = [&](int n) { return top(n) + row(view, n)->height(); };
+    const auto moves = [&] { int n = 0; for (const auto &call : FakeTasksCli::calls(dir.path())) n += call.value(1) == "move"; return n; };
+    // Over a row's upper half the line shows above it, over its lower half below it.
+    QVERIFY(dragOver(page, todo(4), at(1, 0.25))); QVERIFY(line->isVisible()); QVERIFY(qAbs(line->geometry().center().y() - top(1)) <= 1);
+    QVERIFY(dragOver(page, todo(4), at(2, 0.75))); QVERIFY(qAbs(line->geometry().center().y() - bottom(2)) <= 1);
+    // Where the task already is, no line: nothing would change.
+    QVERIFY(dragOver(page, todo(2), at(2, 0.25))); QVERIFY(!line->isVisible());
+    QVERIFY(dragOver(page, todo(2), at(1, 0.75))); QVERIFY(!line->isVisible());
+    QVERIFY(dragOver(page, todo(2), at(3, 0.25))); QVERIFY(!line->isVisible());
+    QVERIFY(dragOver(page, todo(4), at(1, 0.25))); leave(page); QVERIFY(!line->isVisible());
+    // A drop goes before the row below the line, or after the last one.
+    QVERIFY(drop(page, todo(4), at(1, 0.25))); QVERIFY(!line->isVisible());
+    QTRY_COMPARE(visibleRows(view), (QStringList{"T-000004", "T-000001", "T-000002", "T-000003"}));
+    QVERIFY(called(dir.path(), {"task", "move", "uuid-4", "--before", "uuid-1", "--project", "project-1"}));
+    QVERIFY(drop(page, todo(4), at(2, 0.75)));
+    QTRY_COMPARE(visibleRows(view), (QStringList{"T-000001", "T-000002", "T-000004", "T-000003"}));
+    QVERIFY(called(dir.path(), {"task", "move", "uuid-4", "--before", "uuid-3", "--project", "project-1"}));
+    QVERIFY(drop(page, todo(1), QPoint(page->width() / 2, bottom(3) + 20)));
+    QTRY_COMPARE(visibleRows(view), (QStringList{"T-000002", "T-000004", "T-000003", "T-000001"}));
+    QVERIFY(called(dir.path(), {"task", "move", "uuid-1", "--after", "uuid-3", "--project", "project-1"}));
+    // A drop where it already is asks nothing of hgs.
+    const int before = moves(); QVERIFY(drop(page, todo(4), at(4, 0.6))); QTest::qWait(100); QCOMPARE(moves(), before);
+    // Backlog tasks and other projects' tasks do not join To do here, and Backlog is not reordered.
+    QVERIFY(!dragOver(page, TaskDrag::of("project-1", TaskBoard::item(FakeTasksCli::task(5, "backlog"))), at(1, 0.25)));
+    QVERIFY(!dragOver(page, TaskDrag::of("project-2", TaskBoard::item(FakeTasksCli::task(1, "todo"))), at(1, 0.25)));
+    tile(view, "backlog")->click(); QTRY_COMPARE(visibleRows(view), QStringList{"T-000005"});
+    QVERIFY(!dragOver(page, TaskDrag::of("project-1", TaskBoard::item(FakeTasksCli::task(5, "backlog"))), at(5, 0.25)));
+    QVERIFY(!line->isVisible());
+}
+
+void TestTaskDrag::theFunnelReordersAmongTheRowsItShows() {
+    QTemporaryDir dir; QVERIFY(FakeTasksCli::install(dir.path(), {FakeTasksCli::task(1, "todo"),
+        FakeTasksCli::task(2, "todo", {{"target", FakeTasksCli::pinned("codex/web")}}), FakeTasksCli::task(3, "todo")}));
+    HgsClient client(FakeCli::program(dir.filePath("hgs"))); TasksView view(&client); view.resize(760, 560); view.show();
+    view.setContext("project-1", "orchard", "", "codex/api", "\ncodex/api", sessions());
+    QTRY_COMPARE(visibleRows(view).size(), 3);
+    view.findChild<QAbstractButton *>("taskOnlySession")->click(); QCOMPARE(visibleRows(view), (QStringList{"T-000001", "T-000003"}));
+    // The hidden task of another session keeps its place; the drop names a row that shows.
+    auto *page = view.findChild<QWidget *>("taskListPage");
+    QVERIFY(drop(page, todo(3), row(view, 1)->mapTo(page, QPoint(40, 6))));
+    QTRY_VERIFY(called(dir.path(), {"task", "move", "uuid-3", "--before", "uuid-1", "--project", "project-1"}));
+    QTRY_COMPARE(visibleRows(view), (QStringList{"T-000003", "T-000001"}));
+}
+
+void TestTaskDrag::aLongToDoListScrollsAtItsEdgesWhileDragging() {
+    QJsonArray tasks; for (int n = 1; n <= 30; ++n) tasks << FakeTasksCli::task(n, "todo");
+    QTemporaryDir dir; QVERIFY(FakeTasksCli::install(dir.path(), tasks));
+    HgsClient client(FakeCli::program(dir.filePath("hgs"))); TasksView view(&client); view.resize(760, 420); view.show();
+    view.setContext("project-1", "orchard", "", "codex/api", "\ncodex/api", sessions());
+    QTRY_COMPARE(visibleRows(view).size(), 30);
+    auto *scroll = view.findChild<QScrollArea *>("taskList"); auto *bar = scroll->verticalScrollBar(); auto *page = view.findChild<QWidget *>("taskListPage");
+    QTRY_VERIFY(bar->maximum() > 0); QCOMPARE(bar->value(), 0);
+    const auto viewport = [&](int y) { return page->mapFrom(scroll->viewport(), QPoint(scroll->viewport()->width() / 2, y)); };
+    // Held at the bottom edge the list scrolls down, and the line follows the rows.
+    QVERIFY(dragOver(page, todo(1), viewport(scroll->viewport()->height() - 6)));
+    QTRY_VERIFY(bar->value() > 40);
+    // Away from the edges it stops.
+    QVERIFY(dragOver(page, todo(1), viewport(scroll->viewport()->height() / 2)));
+    int still = bar->value(); QTest::qWait(150); QCOMPARE(bar->value(), still);
+    // At the top edge it scrolls back up, and it stops when the drag leaves.
+    QVERIFY(dragOver(page, todo(1), viewport(6))); QTRY_VERIFY(bar->value() < still);
+    leave(page); still = bar->value(); QTest::qWait(150); QCOMPARE(bar->value(), still);
+    QVERIFY(!view.findChild<QWidget *>("taskDropLine")->isVisible());
+}
+
 QTEST_MAIN(TestTaskDrag)
 #include "test_taskdrag.moc"
````

- [ ] **Step 2: Run them to see them fail**

Run: `cmake --build .ci-build/gui --target test_taskdrag -j 8 && QT_QPA_PLATFORM=offscreen .ci-build/gui/tests/test_taskdrag`
Expected: three failures — `toDoRowsAreReorderedByDropping` (`'page && line && page->acceptDrops()' returned FALSE`), `theFunnelReordersAmongTheRowsItShows` (the drop is refused) and `aLongToDoListScrollsAtItsEdgesWhileDragging` (the drag is refused); `Totals: 5 passed, 3 failed`.

- [ ] **Step 3: Implement**

Apply this change to `tray/src/TasksView.cpp` (save it and run `git apply`, or edit by hand):

````diff
diff --git a/tray/src/TasksView.cpp b/tray/src/TasksView.cpp
index 6e1cf854..b8a40a18 100644
--- a/tray/src/TasksView.cpp
+++ b/tray/src/TasksView.cpp
@@ -7,6 +7,7 @@
 #include <QAbstractButton>
 #include <QDateTime>
 #include <QDrag>
+#include <QFrame>
 #include <QDropEvent>
 #include <QHBoxLayout>
 #include <QJsonDocument>
@@ -14,6 +15,7 @@
 #include <QPainter>
 #include <QPushButton>
 #include <QScrollArea>
+#include <QScrollBar>
 #include <QSet>
 #include <QSettings>
 #include <QToolButton>
@@ -132,13 +134,22 @@ TasksView::TasksView(HgsClient *client, QWidget *parent) : QWidget(parent), m_cl
     m_damaged->hide(); layout->addWidget(m_damaged);
     m_scroll = new QScrollArea; m_scroll->setObjectName("taskList"); m_scroll->setWidgetResizable(true); m_scroll->setFrameShape(QFrame::NoFrame);
     // The list's surface holds only its rows; the spare height below stays the page's.
-    auto *page = new QWidget; page->setObjectName("taskListPage");
-    auto *pageLayout = new QVBoxLayout(page); pageLayout->setContentsMargins(0, 0, 0, 0); pageLayout->setSpacing(0);
+    m_listPage = new QWidget; m_listPage->setObjectName("taskListPage");
+    auto *pageLayout = new QVBoxLayout(m_listPage); pageLayout->setContentsMargins(0, 0, 0, 0); pageLayout->setSpacing(0);
     m_listHost = new QWidget; m_listHost->setObjectName("taskListHost"); pageLayout->addWidget(m_listHost); pageLayout->addStretch();
+    // The page takes To do rows dropped on it; the line shows where one would go.
+    m_listPage->setAcceptDrops(true); m_listPage->installEventFilter(this);
+    m_dropLine = new QFrame(m_listPage); m_dropLine->setObjectName("taskDropLine"); m_dropLine->hide();
+    m_autoScroll.setInterval(30);
+    connect(&m_autoScroll, &QTimer::timeout, this, [this] {
+        auto *bar = m_scroll->verticalScrollBar(); bar->setValue(bar->value() + m_scrollStep);
+        // The rows moved under the pointer.
+        followDrag(m_drag, m_listPage->mapFrom(m_scroll->viewport(), m_dragAt));
+    });
     m_list = new QVBoxLayout(m_listHost); m_list->setContentsMargins(0, 0, 0, 0); m_list->setSpacing(0);
     m_empty = new QLabel; m_empty->setObjectName("taskEmpty"); m_empty->setAlignment(Qt::AlignCenter); m_empty->setMargin(18);
     m_list->addWidget(m_empty);
-    m_scroll->setWidget(page); layout->addWidget(m_scroll, 1);
+    m_scroll->setWidget(m_listPage); layout->addWidget(m_scroll, 1);
     m_older = new QPushButton; m_older->setObjectName("showOlderTasks"); m_older->setFlat(true); m_older->setCursor(Qt::PointingHandCursor); m_older->hide();
     connect(m_older, &QPushButton::clicked, this, [this] { m_showOlder = !m_showOlder; render(); });
     layout->addWidget(m_older, 0, Qt::AlignLeft);
@@ -386,6 +397,7 @@ void TasksView::startDrag(TaskRow *row, const QPoint &hotSpot) {
     row->setDragging(true);
     drag->exec(Qt::MoveAction);
     if (source) source->setDragging(false);
+    endListDrag();
 }
 
 bool TasksView::takesOnTile(const TaskDrag &drag, const QString &stage) const {
@@ -393,10 +405,64 @@ bool TasksView::takesOnTile(const TaskDrag &drag, const QString &stage) const {
     return drag.projectId == m_projectId && list(stage) && list(drag.stage) && drag.stage != stage;
 }
 
+bool TasksView::takesInList(const TaskDrag &drag) const {
+    return drag.projectId == m_projectId && m_stage == QLatin1String("todo") && drag.stage == QLatin1String("todo") && !shownRows().isEmpty();
+}
+
+QList<TaskRow *> TasksView::shownRows() const {
+    QList<TaskRow *> rows;
+    for (int i = 0; i < m_list->count(); ++i)
+        if (auto *row = qobject_cast<TaskRow *>(m_list->itemAt(i)->widget()); row && !row->isHidden()) rows << row;
+    return rows;
+}
+
+int TasksView::dropGap(const QString &uuid, const QPoint &point) const {
+    const auto rows = shownRows(); int gap = int(rows.size()), from = -1;
+    for (int i = 0; i < rows.size(); ++i) {
+        if (rows[i]->task().uuid == uuid) from = i;
+        if (gap == rows.size() && point.y() < rows[i]->mapTo(m_listPage, QPoint()).y() + rows[i]->height() / 2) gap = i;
+    }
+    return from >= 0 && (gap == from || gap == from + 1) ? -1 : gap;
+}
+
+void TasksView::followDrag(const TaskDrag &drag, const QPoint &point) {
+    m_drag = drag; m_dragAt = m_listPage->mapTo(m_scroll->viewport(), point);
+    const auto rows = shownRows(); const int gap = rows.isEmpty() ? -1 : dropGap(drag.uuid, point);
+    if (gap < 0) m_dropLine->hide();
+    else {
+        const auto *edge = gap < rows.size() ? rows[gap] : rows.last();
+        const int y = edge->mapTo(m_listPage, QPoint(0, gap < rows.size() ? 0 : edge->height())).y();
+        m_dropLine->setGeometry(m_listHost->x() + 8, y - 1, m_listHost->width() - 16, 2); m_dropLine->show(); m_dropLine->raise();
+    }
+    // Near the top or bottom of the visible list it scrolls that way.
+    const int margin = 28, height = m_scroll->viewport()->height();
+    m_scrollStep = m_dragAt.y() < margin ? -8 : m_dragAt.y() > height - margin ? 8 : 0;
+    if (!m_scrollStep) m_autoScroll.stop(); else if (!m_autoScroll.isActive()) m_autoScroll.start();
+}
+
+void TasksView::reorder(const TaskDrag &drag, const QPoint &point) {
+    const auto rows = shownRows(); const int gap = rows.isEmpty() ? -1 : dropGap(drag.uuid, point);
+    if (gap < 0) return;
+    run({{withProject(gap < rows.size() ? QStringList{"move", drag.uuid, "--before", rows[gap]->task().uuid}
+                                        : QStringList{"move", drag.uuid, "--after", rows.last()->task().uuid}), {}}});
+}
+
+void TasksView::endListDrag() { m_autoScroll.stop(); m_scrollStep = 0; m_dropLine->hide(); }
+
 bool TasksView::eventFilter(QObject *watched, QEvent *event) {
     const auto type = event->type();
     if (type != QEvent::DragEnter && type != QEvent::DragMove && type != QEvent::DragLeave && type != QEvent::Drop)
         return QWidget::eventFilter(watched, event);
+    if (watched == m_listPage) {
+        if (type == QEvent::DragLeave) { endListDrag(); return true; }
+        auto *drop = static_cast<QDropEvent *>(event);
+        const auto drag = TaskDrag::from(drop->mimeData());
+        if (!drag || !takesInList(*drag)) { endListDrag(); drop->ignore(); return true; }
+        drop->setDropAction(Qt::MoveAction); drop->accept();
+        if (type == QEvent::Drop) { endListDrag(); reorder(*drag, drop->position().toPoint()); }
+        else followDrag(*drag, drop->position().toPoint());
+        return true;
+    }
     for (auto it = m_tiles.cbegin(); it != m_tiles.cend(); ++it) {
         if (it.value() != watched) continue;
         auto *tile = it.value();
@@ -491,6 +557,7 @@ void TasksView::setTheme(bool dark) {
     }
     setWorkspaceStyle(m_scroll, QStringLiteral("QScrollArea,#taskListPage{background:transparent;}") + workspaceScrollbars(dark));
     setWorkspaceStyle(m_listHost, QStringLiteral("#taskListHost{background:%1;border-radius:10px;}").arg(colors.surface.name()));
+    setWorkspaceStyle(m_dropLine, QStringLiteral("background:%1;border-radius:1px;").arg(colors.stage(QStringLiteral("todo")).name()));
     setWorkspaceStyle(m_empty, QStringLiteral("color:%1;").arg(colors.muted.name()));
     setWorkspaceStyle(m_errorLabel, QStringLiteral("color:%1;padding:0 0 10px 0;").arg(dark ? "#f4ab9b" : "#a13224"));
     setWorkspaceStyle(m_damaged, QStringLiteral("color:%1;padding:0 0 10px 0;").arg(colors.warn.name()));
````

Apply this change to `tray/src/TasksView.h` (save it and run `git apply`, or edit by hand):

````diff
diff --git a/tray/src/TasksView.h b/tray/src/TasksView.h
index e1c8d4ad..53bbb6b8 100644
--- a/tray/src/TasksView.h
+++ b/tray/src/TasksView.h
@@ -76,13 +76,23 @@ signals:
 protected:
     void showEvent(QShowEvent *event) override;
     void hideEvent(QHideEvent *event) override;
-    // Drops on the stage tiles.
+    // Drops on the stage tiles and on the To do list.
     bool eventFilter(QObject *watched, QEvent *event) override;
 private:
     // Drags a To do or Backlog row; the tiles and the session list take it.
     void startDrag(TaskRow *row, const QPoint &hotSpot);
     // The To do and Backlog tiles take a task of this project from the other list.
     bool takesOnTile(const TaskDrag &drag, const QString &stage) const;
+    // The To do list takes its own tasks to reorder them.
+    bool takesInList(const TaskDrag &drag) const;
+    QList<TaskRow *> shownRows() const;
+    // Where a To do task dropped at `point` of the list page goes: the index of the
+    // shown row it goes before, or their count below the last one; -1 where it is now.
+    int dropGap(const QString &uuid, const QPoint &point) const;
+    // Shows where the task would go and scrolls while the pointer is at an edge.
+    void followDrag(const TaskDrag &drag, const QPoint &point);
+    void reorder(const TaskDrag &drag, const QPoint &point);
+    void endListDrag();
     void request(const QStringList &arguments, const QString &input, std::function<void(const QJsonObject &)> done,
                  std::function<void(const QString &)> failed = {});
     void received(const QJsonObject &listing, quint64 generation);
@@ -115,13 +125,16 @@ private:
     QToolButton *m_only, *m_settings;
     QLabel *m_errorLabel, *m_damaged, *m_empty;
     QScrollArea *m_scroll;
-    QWidget *m_listHost;
+    QWidget *m_listPage, *m_listHost, *m_dropLine;
     QVBoxLayout *m_list;
     QHash<QString, TaskRow *> m_rows;
     QPushButton *m_older;
     MessageComposer *m_composer;
     QHash<quint64, std::pair<std::function<void(const QJsonObject &)>, std::function<void(const QString &)>>> m_pending;
-    QTimer m_timer;
+    QTimer m_timer, m_autoScroll;
+    TaskDrag m_drag;
+    QPoint m_dragAt;   // the pointer in the list's viewport while a task is dragged over it
+    int m_scrollStep = 0;
     quint64 m_generation = 0, m_listing = 0;
     int m_visibleMs = 2500, m_hiddenMs = 15000;
     bool m_polling = true, m_dark = true, m_showOlder = false, m_loaded = false, m_failing = false;
````

- [ ] **Step 4: Run them to see them pass**

Run: `cmake --build .ci-build/gui --target test_taskdrag test_tasksview -j 8 && QT_QPA_PLATFORM=offscreen .ci-build/gui/tests/test_taskdrag && QT_QPA_PLATFORM=offscreen .ci-build/gui/tests/test_tasksview`
Expected: `test_taskdrag` `Totals: 8 passed`; `test_tasksview` `Totals: 17 passed`.

- [ ] **Step 5: Commit**

```bash
git add tray/src/TasksView.cpp \
  tray/src/TasksView.h \
  tray/tests/support/FakeTasksCli.h \
  tray/tests/test_taskdrag.cpp
git commit -m "Reorder To do by dragging"
```

---

### Task 4: Dropping on sessions and project headers

**Files:**
- Modify: `tray/tests/test_sessionswindow.cpp`, `tray/tests/test_taskdrag.cpp`, `docs/reference.md`, `tray/src/SessionList.cpp`, `tray/src/SessionList.h`, `tray/src/SessionsWindow.cpp`, `tray/src/TasksView.cpp`, `tray/src/TasksView.h`

**Interfaces:**
- Consumes: `TaskDrag` (Task 1), `TasksView::run` (phase 3a), the session list's rows (`SessionRoles::Group`, `Header`) and `SessionsWindow::rebuild`'s `Row` (`entry`, `child`, `launch`).
- Produces: `SessionRoles::TaskTarget` (last in the enum): `QStringList{host, name}` of a live session row (`host` "" for this computer), empty for headers, subagent rows, pending launches and forgotten sessions; `SessionList::taskDropped(const TaskDrag &task, const QString &host, const QString &session)` (an empty session for the header: Any session) and private `taskDropItem(const TaskDrag &, const QPoint &) const`; `TasksView::retarget(const TaskDrag &, const QString &host, const QString &session)` (public) and private static `targetArguments(uuid, host, session)`, now also used by `editSteps`. In `test_sessionswindow`, the private helper `tasksFixture(const QString &name, const QJsonArray &tasks) -> QString` replaces the inline fixture of `tasksTabFollowsTheOpenSessionsProject`.

The session list accepts a task on enter and decides per row on every move: a row it takes gets a dashed accent outline (the existing group-drop rectangle, dashed). Qt's own edge scrolling of the list stays. The window marks each row as it builds the list (rows are reused, so every row gets the role) and passes a drop to the tab, which runs `hgs task target <uuid> <session>|any [--host <alias>] --project <id>` and refreshes.

- [ ] **Step 1: Write the failing tests**

Apply this change to `tray/tests/test_sessionswindow.cpp` (save it and run `git apply`, or edit by hand):

````diff
diff --git a/tray/tests/test_sessionswindow.cpp b/tray/tests/test_sessionswindow.cpp
index 5835367d..d0ab2c93 100644
--- a/tray/tests/test_sessionswindow.cpp
+++ b/tray/tests/test_sessionswindow.cpp
@@ -94,6 +94,8 @@
 #include "PopupMouse.h"
 #include "AccountsPage.h"
 #include "FakeCli.h"
+#include "FakeTasksCli.h"
+#include "TaskDrag.h"
 #include "LaunchWarnings.h"
 #include "LocalSessionHost.h"
 #include "ProcessRunner.h"
@@ -324,6 +326,7 @@ private slots:
     void subagentGroupsAndUnavailableActivity();
     void inspectorPreservesWorkspaceAndReportsTasks();
     void tasksTabFollowsTheOpenSessionsProject();
+    void tasksDropOnTheirProjectsSessionsAndHeader();
     void detailsPollingPreservesReadingPosition();
     void nativeGoalIsIndependentOfTurnState();
     void fileReferencesKeepMachineAndWorkspace();
@@ -486,6 +489,9 @@ private:
     // A copy of the base fixture whose inspect answers with these details, so
     // polls that arrive while a test waits cannot replace them.
     QString programInspecting(const QString &name, const QJsonObject &details, const QJsonObject &child = {}, const QJsonObject &usage = {}) const;
+    // A copy of the base fixture, named `name`, whose `hgs task …` calls answer with
+    // this listing of project "Work"; each call's argv is appended to <fixture>.tasks.
+    QString tasksFixture(const QString &name, const QJsonArray &tasks) const;
     // Created by initTestCase(): listing the data rows (-datatags, as
     // RunQtShard.cmake does) exits without destroying the test object. That
     // listing calls every *_data() function first, so none may use m_dir.
@@ -978,11 +984,7 @@ void TestSessionsWindow::tasksTabFollowsTheOpenSessionsProject()
             {"session",QJsonValue()},{"rank","V"}};
         tasks<<task;
     }
-    QFile original(script()); QVERIFY(original.open(QIODevice::ReadOnly));
-    const QByteArray answer = "elif c=='task':\n open(__file__+'.tasks','a').write(json.dumps(a)+'\\n')\n print(json.dumps({'project':{'id':a[a.index('--project')+1],'name':'Work'},'tasks':json.loads("
-        + FakeCli::pythonString(QString::fromUtf8(QJsonDocument(tasks).toJson(QJsonDocument::Compact))) + "),'damaged':[]}))\n";
-    const QString fixture = m_dir->filePath("tasks-hgs");
-    QVERIFY(FakeCli::install(fixture, original.readAll().replace("if False:pass\n", "if False:pass\n" + answer)));
+    const QString fixture = tasksFixture("tasks-hgs", tasks); QVERIFY(!fixture.isEmpty());
     SessionsWindow window(FakeCli::program(fixture)); keepDetailsPollsOut(window); window.setFleet(fleet()); window.show();
     window.showSession({}, "codex/hgs/dashboard");
     auto *tabs = window.findChild<QTabWidget *>("sessionDetailTabs"); auto *view = window.findChild<TasksView *>();
@@ -1011,6 +1013,61 @@ void TestSessionsWindow::tasksTabFollowsTheOpenSessionsProject()
     QCOMPARE(badge->property("todo").toInt(), 0); QVERIFY(!badge->isVisible());
 }
 
+QString TestSessionsWindow::tasksFixture(const QString &name, const QJsonArray &tasks) const
+{
+    QFile original(script()); if (!original.open(QIODevice::ReadOnly)) return {};
+    const QByteArray answer = "elif c=='task':\n open(__file__+'.tasks','a').write(json.dumps(a)+'\\n')\n print(json.dumps({'project':{'id':a[a.index('--project')+1],'name':'Work'},'tasks':json.loads("
+        + FakeCli::pythonString(QString::fromUtf8(QJsonDocument(tasks).toJson(QJsonDocument::Compact))) + "),'damaged':[]}))\n";
+    const QString fixture = m_dir->filePath(name);
+    return FakeCli::install(fixture, original.readAll().replace("if False:pass\n", "if False:pass\n" + answer)) ? fixture : QString();
+}
+
+void TestSessionsWindow::tasksDropOnTheirProjectsSessionsAndHeader()
+{
+    SessionOrganization org; const auto work=org.createGroup("Work"), docs=org.createGroup("Docs");
+    org.moveSession(org.observe("arch","codex/hgs/dashboard",""),work); org.moveSession(org.observe("arch","kimi/docs/research",""),work);
+    org.moveSession(org.observe("mac","claude/infra/review",""),docs);
+    QSettings().setValue("workspace/organization",QJsonDocument(org.toJson()).toJson(QJsonDocument::Compact));
+    const auto any=FakeTasksCli::task(1,"todo"), pinned=FakeTasksCli::task(2,"todo",{{"target",FakeTasksCli::pinned("kimi/docs/research")}});
+    const QString fixture=tasksFixture("tasks-drop-hgs",{any,pinned}); QVERIFY(!fixture.isEmpty());
+    SessionsWindow window(FakeCli::program(fixture)); keepDetailsPollsOut(window); window.setFleet(fleet()); window.show();
+    window.showSession({}, "codex/hgs/dashboard");
+    auto *list=window.findChild<SessionList *>("sessionList"); QVERIFY(list);
+    const auto find=[&](const QString &group, const QString &session) -> QListWidgetItem * {
+        for(int i=0;i<list->count();++i) {
+            auto *item=list->item(i); if(item->data(SessionRoles::Group)!=group) continue;
+            if(session.isEmpty() ? item->data(SessionRoles::Header).toBool() : item->data(SessionRoles::TaskTarget).toStringList().value(1)==session) return item;
+        }
+        return nullptr;
+    };
+    // Live sessions name where a task dropped on them goes; a project header names none.
+    QTRY_VERIFY(find(work,"kimi/docs/research") && find(docs,"claude/infra/review") && find(work,{}));
+    QCOMPARE(find(work,"kimi/docs/research")->data(SessionRoles::TaskTarget).toStringList(), QStringList({"", "kimi/docs/research"}));
+    QCOMPARE(find(docs,"claude/infra/review")->data(SessionRoles::TaskTarget).toStringList(), QStringList({"mac", "claude/infra/review"}));
+    QVERIFY(find(work,{})->data(SessionRoles::TaskTarget).toStringList().isEmpty());
+    const auto drop=[&](const QJsonObject &task, QListWidgetItem *item) {
+        list->scrollToItem(item); const QPoint point=list->visualItemRect(item).center();
+        std::unique_ptr<QMimeData> mime(TaskDrag::of(work, TaskBoard::item(task)).mimeData());
+        QDragEnterEvent enter(point,Qt::MoveAction,mime.get(),Qt::LeftButton,Qt::NoModifier); QApplication::sendEvent(list->viewport(),&enter);
+        QDragMoveEvent move(point,Qt::MoveAction,mime.get(),Qt::LeftButton,Qt::NoModifier); QApplication::sendEvent(list->viewport(),&move);
+        if(!move.isAccepted()) return false;
+        QDropEvent event(QPointF(point),Qt::MoveAction,mime.get(),Qt::LeftButton,Qt::NoModifier); QApplication::sendEvent(list->viewport(),&event);
+        return event.isAccepted();
+    };
+    const auto called=[&](const QJsonArray &argv) {
+        QFile calls(fixture+".tasks"); if(!calls.open(QIODevice::ReadOnly)) return false;
+        while(!calls.atEnd()) if(QJsonDocument::fromJson(calls.readLine()).array()==argv) return true;
+        return false;
+    };
+    // Another project's session refuses the task; a session of its project pins it there.
+    QVERIFY(!drop(any, find(docs,"claude/infra/review")));
+    QVERIFY(drop(any, find(work,"kimi/docs/research")));
+    QTRY_VERIFY(called({"task","target","uuid-1","kimi/docs/research","--project",work}));
+    // The project's header makes a pinned task Any session.
+    QVERIFY(drop(pinned, find(work,{})));
+    QTRY_VERIFY(called({"task","target","uuid-2","any","--project",work}));
+}
+
 void TestSessionsWindow::projectHeaderFiltersOnlyThisProject()
 {
     SessionOrganization org; const auto work=org.createGroup("Work"), docs=org.createGroup("Docs");
````

Apply this change to `tray/tests/test_taskdrag.cpp` (save it and run `git apply`, or edit by hand):

````diff
diff --git a/tray/tests/test_taskdrag.cpp b/tray/tests/test_taskdrag.cpp
index 37949a1d..c1974189 100644
--- a/tray/tests/test_taskdrag.cpp
+++ b/tray/tests/test_taskdrag.cpp
@@ -1,5 +1,6 @@
 #include "FakeTasksCli.h"
 #include "HgsClient.h"
+#include "SessionList.h"
 #include "TaskDrag.h"
 #include "TaskRow.h"
 #include "TasksView.h"
@@ -9,6 +10,7 @@
 #include <QDragLeaveEvent>
 #include <QDragMoveEvent>
 #include <QDropEvent>
+#include <QLabel>
 #include <QMimeData>
 #include <QMouseEvent>
 #include <QScrollArea>
@@ -66,12 +68,15 @@ class TestTaskDrag : public QObject {
     QTemporaryDir m_settings;
 private slots:
     void initTestCase();
+    void init() { QSettings().remove("tasks"); }
     void thePayloadNamesTheTaskAndItsProject();
     void toDoAndBacklogRowsStartADrag();
     void theOtherListsTileTakesADroppedTask();
     void toDoRowsAreReorderedByDropping();
     void theFunnelReordersAmongTheRowsItShows();
     void aLongToDoListScrollsAtItsEdgesWhileDragging();
+    void theSessionListTakesTasksOfItsProject();
+    void aDroppedSessionRepinsTheTask();
 };
 
 void TestTaskDrag::initTestCase() {
@@ -228,5 +233,53 @@ void TestTaskDrag::aLongToDoListScrollsAtItsEdgesWhileDragging() {
     QVERIFY(!view.findChild<QWidget *>("taskDropLine")->isVisible());
 }
 
+void TestTaskDrag::theSessionListTakesTasksOfItsProject() {
+    SessionList list; list.resize(320, 500);
+    const auto add = [&](const QString &group, bool header, const QStringList &target = {}, const QString &child = {}) {
+        auto *item = new QListWidgetItem(&list); item->setSizeHint(QSize(300, header ? 34 : 60));
+        item->setData(SessionRoles::Header, header); item->setData(SessionRoles::Group, group);
+        item->setData(SessionRoles::TaskTarget, target); item->setData(SessionRoles::ChildId, child);
+        item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable); return item;
+    };
+    auto *orchard = add("project-1", true); auto *api = add("project-1", false, {"", "codex/api"});
+    auto *ios = add("project-1", false, {"orchard-mac", "codex/ios"}); auto *subagent = add("project-1", false, {}, "agent-1");
+    auto *other = add("project-2", true); auto *docs = add("project-2", false, {"", "codex/docs"});
+    list.show(); QVERIFY(QTest::qWaitForWindowExposed(&list)); QVERIFY(list.acceptDrops());
+    QSignalSpy dropped(&list, &SessionList::taskDropped);
+    const auto over = [&](const TaskDrag &task, QListWidgetItem *item) { return dragOver(list.viewport(), task, list.visualItemRect(item).center()); };
+    const auto any = todo(1); auto pinned = todo(2); pinned.targetSession = "codex/api";
+    // An Any session task goes to a session of its project, not to its header or a subagent.
+    QVERIFY(over(any, api)); QVERIFY(over(any, ios)); QVERIFY(!over(any, orchard)); QVERIFY(!over(any, subagent));
+    // Another project's sessions and header refuse it.
+    QVERIFY(!over(any, other)); QVERIFY(!over(any, docs));
+    // A pinned task goes to another session, or to the header, which makes it Any session.
+    QVERIFY(!over(pinned, api)); QVERIFY(over(pinned, ios)); QVERIFY(over(pinned, orchard));
+    // A session on a computer this one has not verified is not this computer's session.
+    auto unverified = pinned; unverified.targetKnown = false; QVERIFY(over(unverified, api));
+    leave(list.viewport());
+    QVERIFY(drop(list.viewport(), any, list.visualItemRect(ios).center()));
+    QCOMPARE(dropped.size(), 1); QCOMPARE(dropped[0][0].value<TaskDrag>().uuid, QString("uuid-1"));
+    QCOMPARE(dropped[0][1].toString(), QString("orchard-mac")); QCOMPARE(dropped[0][2].toString(), QString("codex/ios"));
+    QVERIFY(drop(list.viewport(), pinned, list.visualItemRect(orchard).center()));
+    QCOMPARE(dropped.size(), 2); QCOMPARE(dropped[1][1].toString(), QString()); QCOMPARE(dropped[1][2].toString(), QString());
+    QVERIFY(!drop(list.viewport(), any, list.visualItemRect(docs).center())); QCOMPARE(dropped.size(), 2);
+}
+
+void TestTaskDrag::aDroppedSessionRepinsTheTask() {
+    QTemporaryDir dir; QVERIFY(FakeTasksCli::install(dir.path(), {FakeTasksCli::task(1, "todo"),
+        FakeTasksCli::task(2, "todo", {{"target", FakeTasksCli::pinned("codex/api")}})}));
+    HgsClient client(FakeCli::program(dir.filePath("hgs"))); TasksView view(&client); view.resize(760, 560); view.show();
+    view.setContext("project-1", "orchard", "", "codex/api", "\ncodex/api", sessions());
+    QTRY_COMPARE(visibleRows(view).size(), 2);
+    view.retarget(todo(1), "orchard-mac", "codex/ios");
+    QTRY_VERIFY(called(dir.path(), {"task", "target", "uuid-1", "codex/ios", "--host", "orchard-mac", "--project", "project-1"}));
+    view.retarget(todo(2), "", "");
+    QTRY_VERIFY(called(dir.path(), {"task", "target", "uuid-2", "any", "--project", "project-1"}));
+    view.retarget(todo(1), "", "codex/web");
+    QTRY_VERIFY(called(dir.path(), {"task", "target", "uuid-1", "codex/web", "--project", "project-1"}));
+    // The row names its new session once hgs took it.
+    QTRY_VERIFY(row(view, 1) && row(view, 1)->findChild<QLabel *>("taskMeta")->text().contains("codex/web"));
+}
+
 QTEST_MAIN(TestTaskDrag)
 #include "test_taskdrag.moc"
````

- [ ] **Step 2: Run them to see them fail**

Run: `cmake --build .ci-build/gui --target test_taskdrag test_sessionswindow -j 8`
Expected: FAIL to compile — `'TaskTarget' is not a member of 'SessionRoles'`, `'taskDropped' is not a member of 'SessionList'`, `'class TasksView' has no member named 'retarget'`.

- [ ] **Step 3: Implement**

Apply this change to `docs/reference.md` (save it and run `git apply`, or edit by hand):

````diff
diff --git a/docs/reference.md b/docs/reference.md
index b65a9b08..d0973694 100644
--- a/docs/reference.md
+++ b/docs/reference.md
@@ -1906,6 +1906,9 @@ shows its actions on hover, and a click opens the task: its prompt and session
 task for the open session in To do, or in Backlog while Backlog is open. The tab
 label counts the tasks In review and in To do. Done lists the last 7 days, and
 Show older adds the rest. A session outside every project has no tasks.
+To do and Backlog rows can be dragged: onto the other list's tile to move them
+there, within To do to reorder it, onto a session of the project in the session
+list to pin them to it, or onto the project's header to make them Any session.
 
 ## Machines and accounts
 
````

Apply this change to `tray/src/SessionList.cpp` (save it and run `git apply`, or edit by hand):

````diff
diff --git a/tray/src/SessionList.cpp b/tray/src/SessionList.cpp
index 0b06c36c..a2f164eb 100644
--- a/tray/src/SessionList.cpp
+++ b/tray/src/SessionList.cpp
@@ -341,12 +341,30 @@ SessionList::Placement SessionList::placement(const QPoint &point, bool movingGr
     return target;
 }
 
+const QListWidgetItem *SessionList::taskDropItem(const TaskDrag &task, const QPoint &point) const
+{
+    const auto *over = itemAt(point);
+    if (!over || over->isHidden() || over->data(SessionRoles::Group).toString() != task.projectId) return nullptr;
+    if (over->data(SessionRoles::Header).toBool()) return task.targetSession.isEmpty() ? nullptr : over;
+    const auto target = over->data(SessionRoles::TaskTarget).toStringList();
+    if (target.size() != 2) return nullptr;
+    const bool already = task.targetKnown && task.targetHost == target[0] && task.targetSession == target[1];
+    return already ? nullptr : over;
+}
+
 void SessionList::dragEnterEvent(QDragEnterEvent *e)
 {
-    if (validDrop(e->mimeData())) { e->setDropAction(Qt::MoveAction); e->accept(); } else e->ignore();
+    if (validDrop(e->mimeData()) || TaskDrag::from(e->mimeData())) { e->setDropAction(Qt::MoveAction); e->accept(); } else e->ignore();
 }
 void SessionList::dragMoveEvent(QDragMoveEvent *e)
 {
+    if (const auto task = TaskDrag::from(e->mimeData())) {
+        QListWidget::dragMoveEvent(e);   // Qt's edge auto-scroll
+        const auto *item = taskDropItem(*task, e->position().toPoint());
+        m_indicator = item ? visualItemRect(item).adjusted(3, 1, -3, -1) : QRect(); m_onGroup = m_taskDrop = item; viewport()->update();
+        if (item) { e->setDropAction(Qt::MoveAction); e->accept(); } else e->ignore();
+        return;
+    }
     if (!validDrop(e->mimeData())) { e->ignore(); return; }
     // Retain Qt's edge auto-scroll; the base drop mutation is deliberately unused.
     QListWidget::dragMoveEvent(e);
@@ -356,10 +374,19 @@ void SessionList::dragMoveEvent(QDragMoveEvent *e)
 }
 void SessionList::dragLeaveEvent(QDragLeaveEvent *e)
 {
-    m_indicator = {}; viewport()->update(); QListWidget::dragLeaveEvent(e);
+    m_indicator = {}; m_taskDrop = false; viewport()->update(); QListWidget::dragLeaveEvent(e);
 }
 void SessionList::dropEvent(QDropEvent *e)
 {
+    if (const auto task = TaskDrag::from(e->mimeData())) {
+        const auto *item = taskDropItem(*task, e->position().toPoint());
+        m_indicator = {}; m_onGroup = m_taskDrop = false; viewport()->update();
+        if (!item) { e->ignore(); return; }
+        const auto target = item->data(SessionRoles::TaskTarget).toStringList();
+        e->setDropAction(Qt::MoveAction); e->accept();
+        emit taskDropped(*task, target.value(0), target.value(1));
+        return;
+    }
     if (!validDrop(e->mimeData())) { e->ignore(); return; }
     const auto data = QJsonDocument::fromJson(e->mimeData()->data(Mime)).object();
     const auto target = placement(e->position().toPoint(), data.value("group").toBool());
@@ -404,6 +431,7 @@ void SessionList::paintEvent(QPaintEvent *e)
     QPainter p(viewport()); p.setRenderHint(QPainter::Antialiasing);
     const QColor accent(property("hgsDark").toBool() ? "#8bdfc0" : "#167357");
     p.setPen(QPen(accent, 2));
+    if (m_taskDrop) p.setPen(QPen(accent, 2, Qt::DashLine));
     if (m_onGroup) { p.setBrush(Qt::NoBrush); p.drawRoundedRect(m_indicator, 5, 5); }
     else p.drawLine(m_indicator.topLeft(), m_indicator.topRight());
 }
````

Apply this change to `tray/src/SessionList.h` (save it and run `git apply`, or edit by hand):

````diff
diff --git a/tray/src/SessionList.h b/tray/src/SessionList.h
index 0cf205c0..03d65245 100644
--- a/tray/src/SessionList.h
+++ b/tray/src/SessionList.h
@@ -1,4 +1,5 @@
 #pragma once
+#include "TaskDrag.h"
 #include "WorkspaceList.h"
 #include <QTimer>
 #include <QElapsedTimer>
@@ -9,10 +10,14 @@ namespace SessionRoles {
 enum Role { Key = Qt::UserRole, Title, Meta, Status, Detail, Agent, Host, Children,
             Group, Header, Identity, Collapsed, Total, Attention, Working, Model, Effort, MachineColor, MachineName, Unread,
             ChildId, ParentKey, HasChildren, Expanded, ProjectColor, ProjectVivid, LaunchId, WorkingSince, ReviewLater, Draft, Failure,
-            GitStatus, GitHost, GitPath, MetaIcon, Scheduled, OptionalQuestions, ModifiedAt, CoordinationWait, Waiting };
+            GitStatus, GitHost, GitPath, MetaIcon, Scheduled, OptionalQuestions, ModifiedAt, CoordinationWait, Waiting,
+            // {host, name} of a live session a dragged task can be pinned to; empty for other rows.
+            TaskTarget };
 }
 
 // Drops express placement relative to an identity, never a filtered row number.
+// A task dragged from the Tasks tab drops on a live session of its project, or on
+// the project's header for Any session.
 class SessionList : public WorkspaceList {
     Q_OBJECT
 public:
@@ -35,6 +40,9 @@ signals:
     void sessionMoved(const QString &identity, const QString &group, const QString &before);
     void groupMoved(const QString &group, const QString &before);
     void dragFinished();
+    // A task was dropped on a session (host "" for this computer) or, with an empty
+    // session, on its project's header.
+    void taskDropped(const TaskDrag &task, const QString &host, const QString &session);
 protected:
     QStringList mimeTypes() const override { return {QStringLiteral("application/x-hgs-session-placement")}; }
     QMimeData *mimeData(const QList<QListWidgetItem *> &) const override;
@@ -62,6 +70,9 @@ private:
     QElapsedTimer m_activityClock;
     bool m_animateActivity = true;
     bool validDrop(const QMimeData *) const;
+    // The row a dragged task would change: a live session of its project other than
+    // its own, or the project's header for a pinned task; null elsewhere.
+    const QListWidgetItem *taskDropItem(const TaskDrag &task, const QPoint &point) const;
     struct Placement { QString group, before; QRect indicator; bool onGroup = false; };
     Placement placement(const QPoint &point, bool group) const;
     QPoint m_press;
@@ -69,7 +80,7 @@ private:
     QString m_pressedParent;
     bool m_dragging = false;
     QRect m_indicator;
-    bool m_onGroup = false;
+    bool m_onGroup = false, m_taskDrop = false;
     QString m_token;
     bool m_bulkSelecting = false, m_togglePress = false;
     QWidget *m_selectionBar = nullptr;
````

Apply this change to `tray/src/SessionsWindow.cpp` (save it and run `git apply`, or edit by hand):

````diff
diff --git a/tray/src/SessionsWindow.cpp b/tray/src/SessionsWindow.cpp
index 0f22454e..953ee823 100644
--- a/tray/src/SessionsWindow.cpp
+++ b/tray/src/SessionsWindow.cpp
@@ -507,6 +507,9 @@ SessionsWindow::SessionsWindow(const QString &hgsPath, QWidget *parent)
         m_organization.moveGroup(id, before); saveOrganization(); rebuild();
     });
     connect(m_sessions, &SessionList::dragFinished, this, &SessionsWindow::rebuild);
+    connect(m_sessions, &SessionList::taskDropped, this, [this](const TaskDrag &task, const QString &host, const QString &session) {
+        m_tasks->retarget(task, host, session);
+    });
     connect(m_sessions, &QListWidget::currentRowChanged, this, [this]() { if (!m_rebuilding) selectSession(); });
     connect(m_sessions, &QListWidget::itemActivated, this, &SessionsWindow::jumpToLatestActivity);
 
@@ -2848,6 +2851,9 @@ void SessionsWindow::rebuild()
         item->setData(SessionRoles::GitHost, QString()); item->setData(SessionRoles::GitPath, QString());
         item->setData(Qt::AccessibleDescriptionRole, QString());
         item->setData(SessionRoles::Header, row.entry < 0 && row.launch.isEmpty()); item->setHidden(row.hidden);
+        // A live session takes tasks of its project dragged onto it.
+        const bool live = row.entry >= 0 && row.child.isEmpty() && row.launch.isEmpty() && visible[row.entry].session.state != "archived";
+        item->setData(SessionRoles::TaskTarget, live ? QStringList{visible[row.entry].host, visible[row.entry].session.name} : QStringList());
         if (!row.launch.isEmpty()) {
             const auto launch = m_pendingLaunches.value(row.launch).toObject();
             const QString name = launch.value("tag").toString(), host = launch.value("host").toString();
````

Apply this change to `tray/src/TasksView.cpp` (save it and run `git apply`, or edit by hand):

````diff
diff --git a/tray/src/TasksView.cpp b/tray/src/TasksView.cpp
index b8a40a18..96b2711b 100644
--- a/tray/src/TasksView.cpp
+++ b/tray/src/TasksView.cpp
@@ -348,14 +348,21 @@ QList<TasksView::Step> TasksView::steps(const TaskItem &task, TaskAction action)
 QList<TasksView::Step> TasksView::editSteps(const TaskItem &task, const TaskEdits &edits) const {
     QList<Step> out;
     if (edits.text) out << Step{withProject({"edit", task.uuid}), *edits.text};
-    if (edits.target) {
-        QStringList arguments{"target", task.uuid, edits.target->any ? QStringLiteral("any") : edits.target->session};
-        if (!edits.target->any && !edits.target->host.isEmpty()) arguments << "--host" << edits.target->host;
-        out << Step{withProject(arguments), {}};
-    }
+    if (edits.target)
+        out << Step{withProject(targetArguments(task.uuid, edits.target->host, edits.target->any ? QString() : edits.target->session)), {}};
     return out;
 }
 
+QStringList TasksView::targetArguments(const QString &uuid, const QString &host, const QString &session) {
+    QStringList arguments{"target", uuid, session.isEmpty() ? QStringLiteral("any") : session};
+    if (!session.isEmpty() && !host.isEmpty()) arguments << "--host" << host;
+    return arguments;
+}
+
+void TasksView::retarget(const TaskDrag &task, const QString &host, const QString &session) {
+    run({{targetArguments(task.uuid, host, session) << QStringLiteral("--project") << task.projectId, {}}});
+}
+
 void TasksView::run(QList<Step> steps, std::function<void()> finished, std::function<void(const QString &)> failed) {
     if (steps.isEmpty()) { refresh(); if (finished) finished(); return; }
     auto step = steps.takeFirst();
````

Apply this change to `tray/src/TasksView.h` (save it and run `git apply`, or edit by hand):

````diff
diff --git a/tray/src/TasksView.h b/tray/src/TasksView.h
index 53bbb6b8..c8c66f33 100644
--- a/tray/src/TasksView.h
+++ b/tray/src/TasksView.h
@@ -66,6 +66,9 @@ public:
     void setPolling(bool polling);
     void setRefreshIntervals(int visibleMs, int hiddenMs);
     void refresh();
+    // Pins a task dragged onto a session (host "" for this computer), or makes it
+    // Any session for an empty session.
+    void retarget(const TaskDrag &task, const QString &host, const QString &session);
     const TaskBoard &board() const { return m_board; }
     QString selectedStage() const { return m_stage; }
     MessageComposer *composer() const { return m_composer; }
@@ -102,6 +105,8 @@ private:
     struct Step { QStringList arguments; QString input; std::function<void(const QJsonObject &)> then = {}; };
     QList<Step> steps(const TaskItem &task, TaskAction action) const;
     QList<Step> editSteps(const TaskItem &task, const TaskEdits &edits) const;
+    // `target REF SESSION|any [--host ALIAS]` without the project.
+    static QStringList targetArguments(const QString &uuid, const QString &host, const QString &session);
     // Runs the steps in order and refreshes after. `finished` follows the last
     // success; `failed` replaces the notice for the first refusal.
     void run(QList<Step> steps, std::function<void()> finished = {}, std::function<void(const QString &)> failed = {});
````

- [ ] **Step 4: Run them to see them pass**

Run:
```bash
cmake --build .ci-build/gui --target test_taskdrag test_sessionswindow test_workspace -j 8
QT_QPA_PLATFORM=offscreen .ci-build/gui/tests/test_taskdrag
QT_QPA_PLATFORM=offscreen .ci-build/gui/tests/test_sessionswindow tasksDropOnTheirProjectsSessionsAndHeader tasksTabFollowsTheOpenSessionsProject
QT_QPA_PLATFORM=offscreen .ci-build/gui/tests/test_workspace
```
Expected: `test_taskdrag` `Totals: 10 passed`; the two window tests `Totals: 4 passed`; `test_workspace` all pass (session drags unchanged).

- [ ] **Step 5: Run everything**

Run:
```bash
cmake --build .ci-build/gui -j 8
ctest --test-dir .ci-build/gui --parallel 8 --timeout 600 --output-on-failure
python3 scripts/ci/check-source.py
```
Expected: every suite passes, `taskdrag` included. If `colorPickerUsesWorkspaceThemeAndCancelsOutside(dark)` fails in shard 7, it fails on the branch's previous commit too on affected machines (Known limits). Then `Source syntax, privacy, child process and release version checks passed.`

- [ ] **Step 6: Try it by hand (only when the person asks for a rebuild of their running GUI)**

In a running build: drag a To do row onto the Backlog tile (it moves to the top of Backlog); drag a To do row between two others and past the bottom edge of a long list (the line follows, the list scrolls); drag a row onto a session of its project in the session list and onto the project's header (the outline shows; the row's session changes); press Escape during a drag (every outline and the line disappear).

- [ ] **Step 7: Commit**

```bash
git add docs/reference.md \
  tray/src/SessionList.cpp \
  tray/src/SessionList.h \
  tray/src/SessionsWindow.cpp \
  tray/src/TasksView.cpp \
  tray/src/TasksView.h \
  tray/tests/test_sessionswindow.cpp \
  tray/tests/test_taskdrag.cpp
git commit -m "Pin a dragged task by dropping it on a session or its project"
```

---
