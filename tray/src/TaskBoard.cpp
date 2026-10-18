#include "TaskBoard.h"
#include <QJsonArray>
#include <QObject>
#include <algorithm>

namespace {
bool listed(const QString &stage) { return stage == QLatin1String("todo") || stage == QLatin1String("backlog"); }
// A session reference: (host, name, known). A null host is a computer this one
// has not verified.
void session(const QJsonValue &value, QString &host, QString &name, bool &known) {
    const auto object = value.toObject();
    name = object.value("session").toString(); host = object.value("host").toString();
    known = !object.contains("host") || !object.value("host").isNull();
}
}

TaskItem TaskBoard::item(const QJsonObject &t) {
    TaskItem item;
    item.ref = t.value("ref").toString(); item.id = t.value("id").toString(); item.uuid = t.value("uuid").toString();
    item.text = t.value("text").toString(); item.stage = t.value("stage").toString();
    item.summary = t.value("summary").toString(); item.cancelReason = t.value("cancel_reason").toString();
    item.createdAt = qint64(t.value("created_at").toDouble()); item.stageAt = qint64(t.value("stage_at").toDouble());
    item.cancelledAt = qint64(t.value("cancelled_at").toDouble()); item.writable = t.value("writable").toBool(true);
    const auto target = t.value("target").toObject();
    if (target.value("kind").toString() == QLatin1String("session")) session(target, item.targetHost, item.targetSession, item.targetKnown);
    if (t.value("session").isObject()) session(t.value("session"), item.sessionHost, item.sessionName, item.sessionKnown);
    const auto author = t.value("author").toObject();
    if (author.value("kind").toString() == QLatin1String("agent")) item.author = author.value("session").toString();
    return item;
}

TaskBoard TaskBoard::fromListing(const QJsonObject &listing) {
    TaskBoard board;
    const auto project = listing.value("project").toObject();
    board.projectId = project.value("id").toString(); board.projectName = project.value("name").toString();
    board.damaged = int(listing.value("damaged").toArray().size());
    int queue = 0;
    for (const auto &value : listing.value("tasks").toArray()) {
        auto task = item(value.toObject());
        if (task.stage == QLatin1String("todo")) task.queue = ++queue;
        board.tasks << task;
    }
    return board;
}

bool TaskBoard::belongsTo(const TaskItem &t, const QString &host, const QString &name) {
    if (listed(t.stage)) return t.targetKnown && !t.targetSession.isEmpty() && t.targetHost == host && t.targetSession == name;
    return t.sessionKnown && !t.sessionName.isEmpty() && t.sessionHost == host && t.sessionName == name;
}

bool TaskBoard::keptFor(const TaskItem &t, const QString &host, const QString &name) {
    return belongsTo(t, host, name) || (listed(t.stage) && t.targetSession.isEmpty());
}

QList<TaskItem> TaskBoard::rows(const QString &stage, const View &view, bool older) const {
    QList<TaskItem> out;
    const bool done = stage == QLatin1String("done");
    for (const auto &t : tasks) {
        if (t.stage != stage || (view.only && !keptFor(t, view.host, view.session))) continue;
        if (done && (view.now - t.stageAt > RecentDoneMs) != older) continue;
        out << t;
    }
    if (done) std::stable_sort(out.begin(), out.end(), [](const auto &a, const auto &b) { return a.stageAt > b.stageAt; });
    else if (!listed(stage)) std::stable_sort(out.begin(), out.end(), [](const auto &a, const auto &b) { return a.stageAt < b.stageAt; });
    return out;
}

const QStringList &TaskBoard::stages() {
    static const QStringList order{"review", "active", "todo", "backlog", "done"};
    return order;
}

QString TaskBoard::stageLabel(const QString &stage) {
    if (stage == QLatin1String("review")) return QObject::tr("In review");
    if (stage == QLatin1String("active")) return QObject::tr("In progress");
    if (stage == QLatin1String("todo")) return QObject::tr("To do");
    if (stage == QLatin1String("backlog")) return QObject::tr("Backlog");
    return QObject::tr("Done");
}

// A stage's icon is the icon of the action that moves a task there.
QString TaskBoard::stageIcon(const QString &stage) {
    if (stage == QLatin1String("review")) return QStringLiteral("task-inbox");
    if (stage == QLatin1String("active")) return QStringLiteral("play");
    if (stage == QLatin1String("todo")) return QStringLiteral("task-list");
    if (stage == QLatin1String("backlog")) return QStringLiteral("task-clock");
    return QStringLiteral("task-check");
}

QList<TaskAction> TaskBoard::actions(const QString &stage) {
    using A = TaskAction;
    if (stage == QLatin1String("todo")) return {A::Edit, A::ToBacklog, A::Top, A::SendNow};
    if (stage == QLatin1String("backlog")) return {A::Edit, A::ToTodo, A::SendNow};
    if (stage == QLatin1String("active")) return {A::Cancel};
    if (stage == QLatin1String("review")) return {A::BackToTodo, A::Accept};
    if (stage == QLatin1String("done")) return {A::BackToTodo};
    return {};
}

QString TaskBoard::actionLabel(TaskAction action) {
    switch (action) {
    case TaskAction::Edit: return QObject::tr("Edit task");
    case TaskAction::ToBacklog: return QObject::tr("Move to Backlog");
    case TaskAction::Top: return QObject::tr("Move to the top of To do");
    case TaskAction::SendNow: return QObject::tr("Send now");
    case TaskAction::ToTodo: return QObject::tr("Move to To do");
    case TaskAction::Cancel: return QObject::tr("Cancel task…");
    case TaskAction::BackToTodo: return QObject::tr("Back to To do");
    case TaskAction::Accept: return QObject::tr("Accept");
    }
    return {};
}

QString TaskBoard::actionIcon(TaskAction action) {
    switch (action) {
    case TaskAction::Edit: return QStringLiteral("task-edit");
    case TaskAction::ToBacklog: return QStringLiteral("task-clock");
    case TaskAction::Top: return QStringLiteral("task-top");
    case TaskAction::SendNow: return QStringLiteral("play");
    case TaskAction::ToTodo: case TaskAction::BackToTodo: return QStringLiteral("task-list-add");
    case TaskAction::Cancel: return QStringLiteral("close");
    case TaskAction::Accept: return QStringLiteral("task-check");
    }
    return {};
}
