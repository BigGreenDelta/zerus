#pragma once
#include "TaskBoard.h"
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
};

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
private:
    void request(const QStringList &arguments, const QString &input, std::function<void(const QJsonObject &)> done,
                 std::function<void(const QString &)> failed = {});
    void received(const QJsonObject &listing, quint64 generation);
    void render();
    void selectStage(const QString &stage);
    // One `hgs task` call of a sequence; `then` sees its reply.
    struct Step { QStringList arguments; QString input; std::function<void(const QJsonObject &)> then = {}; };
    QList<Step> steps(const TaskItem &task, TaskAction action) const;
    // Runs the steps in order and stops at the first failure; refreshes after.
    void run(QList<Step> steps);
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
    QLabel *m_errorLabel, *m_empty;
    QScrollArea *m_scroll;
    QWidget *m_listHost;
    QVBoxLayout *m_list;
    QHash<QString, TaskRow *> m_rows;
    QPushButton *m_older;
    MessageComposer *m_composer;
    QHash<quint64, std::pair<std::function<void(const QJsonObject &)>, std::function<void(const QString &)>>> m_pending;
    QTimer m_timer;
    quint64 m_generation = 0, m_listing = 0;
    int m_visibleMs = 2500, m_hiddenMs = 15000;
    bool m_polling = true, m_dark = true, m_showOlder = false, m_loaded = false;
};
