#pragma once

#include "HgsClient.h"
#include "OvermindModel.h"
#include <QJsonObject>
#include <QDateTime>

struct AttentionNotice {
    QString token, title, body;
};
struct AttentionChanges {
    QList<AttentionNotice> raised;
    QStringList cleared;
};

// Only successful snapshots advance the edge detector. An offline peer is not
// a resolution and reconnecting must not repeat the same outstanding alert.
class AttentionTracker {
public:
    explicit AttentionTracker(QJsonObject saved = {}, QJsonObject decisions = {})
        : m_hosts(std::move(saved)), m_decisions(std::move(decisions)) {}
    AttentionChanges observe(const QString &host, const BoxState &box, qint64 now = QDateTime::currentSecsSinceEpoch());
    AttentionChanges observe(const QString &host, const BoxState &box, qint64 now, bool applicationFocused);
    // Overmind's Your Call tasks (OvermindModel::decisions). One notice per task
    // and signature: a task parked again notifies again, and a task that leaves
    // the list withdraws its notice. `read` is false while the list is not
    // loaded or its last read failed: that is not a resolution.
    AttentionChanges observeDecisions(bool read, const QList<OvermindModel::Decision> &decisions,
                                      qint64 now = QDateTime::currentSecsSinceEpoch());
    AttentionChanges observeDecisions(bool read, const QList<OvermindModel::Decision> &decisions, qint64 now, bool applicationFocused);
    void deliveryFailed(const QString &token, qint64 now = QDateTime::currentSecsSinceEpoch());
    QJsonObject state() const { return m_hosts; }
    QJsonObject decisionState() const { return m_decisions; }
    static QJsonObject target(const QString &token);
    static bool matches(const QJsonObject &target, const SessionInfo &session);
    // The task a Your Call notice opens: {task, project, machine, name}, where
    // machine and name are the asking session; empty for any other token.
    static QJsonObject decision(const QString &token);
private:
    QJsonObject m_hosts;
    // Task UUID -> {signature, token[, retry_at]}.
    QJsonObject m_decisions;
};
