#pragma once
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

// One task as `hgs task list` reports it.
struct TaskItem {
    QString ref, id, uuid, text, stage, summary, cancelReason;
    // A pinned session: its computer ("" for this one, otherwise its SSH alias) and
    // name. An empty name is Any session; targetKnown is false for a computer this
    // one has not verified.
    QString targetHost, targetSession;
    bool targetKnown = true;
    // The session the task was last sent to; empty when it was never sent.
    QString sessionHost, sessionName;
    bool sessionKnown = true;
    // Who filed it: empty for the person, otherwise the agent's session name.
    QString author;
    qint64 createdAt = 0, stageAt = 0, cancelledAt = 0;
    bool writable = true;
    int queue = 0;   // position in the project's To do, from 1
};

enum class TaskAction { Edit, ToBacklog, Top, SendNow, ToTodo, Cancel, BackToTodo, Accept };

// A project's tasks and the rules the Tasks tab shows them by
// (docs/superpowers/specs/2026-10-13-project-task-queue-design.md, Desktop interface).
class TaskBoard {
public:
    static constexpr qint64 RecentDoneMs = 7LL * 24 * 60 * 60 * 1000;
    static TaskBoard fromListing(const QJsonObject &listing);
    static TaskItem item(const QJsonObject &task);
    QString projectId, projectName;
    QList<TaskItem> tasks;   // in hgs order: each stage's tasks by rank
    int damaged = 0;

    // The open session's tasks: pinned to it in To do and Backlog, sent to it later.
    static bool belongsTo(const TaskItem &task, const QString &host, const QString &session);
    // "Only this session" keeps those and the Any session tasks in To do and Backlog.
    static bool keptFor(const TaskItem &task, const QString &host, const QString &session);
    struct View { QString host, session; bool only = false; qint64 now = 0; };
    // A stage's rows in display order: To do and Backlog by rank, In review and In
    // progress oldest first, Done newest first. Done holds the last 7 days; `older`
    // asks for the rest of it.
    QList<TaskItem> rows(const QString &stage, const View &view, bool older = false) const;
    int count(const QString &stage, const View &view) const { return int(rows(stage, view).size()); }

    static const QStringList &stages();   // the tiles' order
    static QString stageLabel(const QString &stage);
    static QString stageIcon(const QString &stage);
    // The same actions in the same order on every surface; the main one is last.
    static QList<TaskAction> actions(const QString &stage);
    static QString actionLabel(TaskAction action);
    static QString actionIcon(TaskAction action);
};
