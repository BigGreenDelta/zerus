#pragma once
#include "TaskBoard.h"
#include "TaskDrag.h"
#include "TaskRow.h"
#include <QHash>
#include <QJsonObject>
#include <QPointer>
#include <QTimer>
#include <QWidget>
#include <functional>

class HgsClient;
class MessageComposer;
class QAbstractButton;
class QLabel;
class QPushButton;
class QScrollArea;
class QToolButton;
class QVBoxLayout;
class TaskTile;

// A session of the open project as the Tasks tab sees it.
struct TaskSession {
    QString host, name;   // host: "" for this computer, otherwise its SSH alias
    QString status;       // its status label, such as "Working"
    QString blocker;      // why it takes no prompt now; empty when it does
    bool running = true;  // whether its agent runs, for the cancel dialog
    bool working = false; // whether its agent is in a turn, for the cancel dialog
    bool operator==(const TaskSession &other) const {
        return host == other.host && name == other.name && status == other.status && blocker == other.blocker
            && running == other.running && working == other.working;
    }
};

struct TaskEdits;

// The Tasks tab label's counts: In review as an attention badge, then To do.
class TaskTabBadge : public QWidget {
    Q_OBJECT
public:
    explicit TaskTabBadge(QWidget *parent = nullptr);
    void setCounts(int review, int todo);
    void setTheme(bool dark);
    QSize sizeHint() const override;
protected:
    void paintEvent(QPaintEvent *event) override;
private:
    int m_review = 0, m_todo = 0;
    bool m_dark = true;
};

// The Tasks tab: the open project's tasks in the local replica, read and changed
// through `hgs task` (docs/superpowers/specs/2026-10-13-project-task-queue-design.md,
// Desktop interface). Stage tiles above one list, and the task input below.
class TasksView : public QWidget {
    Q_OBJECT
public:
    explicit TasksView(HgsClient *client, QWidget *parent = nullptr);
    // The open session's project, the open session (its window key keeps the
    // input's draft) and the project's sessions.
    void setContext(const QString &projectId, const QString &projectName, const QString &host, const QString &session,
                    const QString &sessionKey, const QList<TaskSession> &sessions);
    void setTheme(bool dark);
    // While the window shows: refresh every visibleMs while the tab is shown,
    // every hiddenMs otherwise, so the tab label stays current.
    void setPolling(bool polling);
    void setRefreshIntervals(int visibleMs, int hiddenMs);
    void refresh();
    const TaskBoard &board() const { return m_board; }
    QString selectedStage() const { return m_stage; }
    MessageComposer *composer() const { return m_composer; }
signals:
    // The project's tasks in In review and in To do, for the tab label.
    void countsChanged(int review, int todo);
    void notice(const QString &text, bool error);
protected:
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;
    // Drops on the stage tiles and on the To do list.
    bool eventFilter(QObject *watched, QEvent *event) override;
private:
    // Drags a To do or Backlog row; the tiles and the session list take it.
    void startDrag(TaskRow *row, const QPoint &hotSpot);
    // The To do and Backlog tiles take a task of this project from the other list.
    bool takesOnTile(const TaskDrag &drag, const QString &stage) const;
    // The To do list takes its own tasks to reorder them.
    bool takesInList(const TaskDrag &drag) const;
    QList<TaskRow *> shownRows() const;
    // Where a To do task dropped at `point` of the list page goes: the index of the
    // shown row it goes before, or their count below the last one; -1 where it is now.
    int dropGap(const QString &uuid, const QPoint &point) const;
    // Shows where the task would go and scrolls while the pointer is at an edge.
    void followDrag(const TaskDrag &drag, const QPoint &point);
    void reorder(const TaskDrag &drag, const QPoint &point);
    void endListDrag();
    void request(const QStringList &arguments, const QString &input, std::function<void(const QJsonObject &)> done,
                 std::function<void(const QString &)> failed = {});
    void received(const QJsonObject &listing, quint64 generation);
    void render();
    void selectStage(const QString &stage);
    // One `hgs task` call of a sequence; `then` sees its reply.
    struct Step { QStringList arguments; QString input; std::function<void(const QJsonObject &)> then = {}; };
    QList<Step> steps(const TaskItem &task, TaskAction action) const;
    QList<Step> editSteps(const TaskItem &task, const TaskEdits &edits) const;
    // Runs the steps in order and refreshes after. `finished` follows the last
    // success; `failed` replaces the notice for the first refusal.
    void run(QList<Step> steps, std::function<void()> finished = {}, std::function<void(const QString &)> failed = {});
    void act(const TaskItem &task, TaskAction action);
    void openTask(const TaskItem &task);
    void confirmCancel(const TaskItem &task);
    void openSettings();
    void addTask(const QString &key, const QString &text);
    void updateComposer();
    void applyComposerKey();
    void schedule();
    QString sendBlocker(const TaskItem &task) const;
    const TaskSession *session(const QString &host, const QString &name) const;
    QStringList withProject(QStringList arguments) const;
    TaskBoard::View view() const;
    HgsClient *m_client;
    TaskBoard m_board;
    QString m_projectId, m_projectName, m_host, m_session, m_sessionKey, m_stage, m_error;
    QList<TaskSession> m_sessions;
    QHash<QString, TaskTile *> m_tiles;
    QToolButton *m_only, *m_settings;
    QLabel *m_errorLabel, *m_damaged, *m_empty;
    QScrollArea *m_scroll;
    QWidget *m_listPage, *m_listHost, *m_dropLine;
    QVBoxLayout *m_list;
    QHash<QString, TaskRow *> m_rows;
    QPushButton *m_older;
    MessageComposer *m_composer;
    QHash<quint64, std::pair<std::function<void(const QJsonObject &)>, std::function<void(const QString &)>>> m_pending;
    QTimer m_timer, m_autoScroll;
    TaskDrag m_drag;
    QPoint m_dragAt;   // the pointer in the list's viewport while a task is dragged over it
    int m_scrollStep = 0;
    quint64 m_generation = 0, m_listing = 0;
    int m_visibleMs = 2500, m_hiddenMs = 15000;
    bool m_polling = true, m_dark = true, m_showOlder = false, m_loaded = false, m_failing = false;
};
