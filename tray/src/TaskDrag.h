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
