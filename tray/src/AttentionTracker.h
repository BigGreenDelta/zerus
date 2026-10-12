#pragma once

#include "HgsClient.h"
#include "OvermindModel.h"
#include <QHash>
#include <QJsonObject>
#include <QDateTime>

struct AttentionNotice {
    QString token, title, body;
    // The label of the notification's action button; empty: the notifier's own.
    QString action;
};
struct AttentionChanges {
    QList<AttentionNotice> raised;
    QStringList cleared;
};

// Only successful snapshots advance the edge detector. An offline peer is not
// a resolution and reconnecting must not repeat the same outstanding alert.
class AttentionTracker {
public:
    // Overmind names a project by its ID; notices show these names instead and
    // leave out a project without one.
    using ProjectNames = QHash<QString, QString>;
    // More new decisions than this in one pass become one summary notice.
    static constexpr int DecisionBurst = 3;

    explicit AttentionTracker(QJsonObject saved = {}, QJsonObject decisions = {});
    AttentionChanges observe(const QString &host, const BoxState &box, qint64 now = QDateTime::currentSecsSinceEpoch());
    AttentionChanges observe(const QString &host, const BoxState &box, qint64 now, bool applicationFocused);
    // Overmind's Your Call tasks. One notice per task and signature: a task
    // parked again notifies again, and a task that leaves the list withdraws its
    // notice; one that comes back unchanged (within a day) is not announced
    // again. `read` is false while the list is not loaded, after a failed read
    // or a reset: that is not a resolution. The model overloads take `read`
    // from the model.
    AttentionChanges observeDecisions(bool read, const QList<OvermindModel::Decision> &decisions, qint64 now,
                                      bool applicationFocused, const ProjectNames &names = {});
    AttentionChanges observeDecisions(const OvermindModel &model, const ProjectNames &names = {});
    AttentionChanges observeDecisions(const OvermindModel &model, const ProjectNames &names, qint64 now, bool applicationFocused);
    void deliveryFailed(const QString &token, qint64 now = QDateTime::currentSecsSinceEpoch());
    QJsonObject state() const { return m_hosts; }
    // {tasks, summary, recent}: what was announced, persisted like state().
    QJsonObject decisionState() const;
    static QJsonObject target(const QString &token);
    static bool matches(const QJsonObject &target, const SessionInfo &session);
    // What a Your Call notice opens: {task, project, machine, name}, where
    // machine and name are the asking session. A summary notice has an empty
    // task and the project of its oldest decision. Empty for other tokens.
    static QJsonObject decision(const QString &token);
private:
    QJsonObject m_hosts;
    // Task UUID -> {signature, token[, retry_at]}.
    QJsonObject m_decisions;
    // The summary of a burst: {token, tokens (of its tasks), count, project[, retry_at]}.
    QJsonObject m_summary;
    // Signatures of decisions that left the list -> when, at most a day and 200.
    QJsonObject m_recent;
};
