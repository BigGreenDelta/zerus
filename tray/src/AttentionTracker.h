#pragma once

#include "HgsClient.h"
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
    explicit AttentionTracker(QJsonObject saved = {}) : m_hosts(std::move(saved)) {}
    AttentionChanges observe(const QString &host, const BoxState &box, qint64 now = QDateTime::currentSecsSinceEpoch());
    AttentionChanges observe(const QString &host, const BoxState &box, qint64 now, bool applicationFocused);
    void deliveryFailed(const QString &token, qint64 now = QDateTime::currentSecsSinceEpoch());
    QJsonObject state() const { return m_hosts; }
    static QJsonObject target(const QString &token);
    static bool matches(const QJsonObject &target, const SessionInfo &session);
private:
    QJsonObject m_hosts;
};
