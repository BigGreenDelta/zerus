#pragma once

#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QTimer>

#include <functional>

#include "ReadMarks.h"

class HgsClient;

// Writes queued read marks to their hosts. A mark stays queued until a
// snapshot shows it (FleetState settles it); meanwhile a delivered mark is
// written again only after kConfirmedResendMs, doubling up to
// kMaxUnappliedResendMs while its host applies only part of it, failed writes
// back off up to kMaxBackoffMs, and at most kMaxInFlight writes (SSH to peers)
// run at once, each free slot going to the host with the fewest.
class ReadMarkSync : public QObject {
    Q_OBJECT
public:
    static constexpr qint64 kFirstBackoffMs = 10000, kMaxBackoffMs = 300000, kConfirmedResendMs = 120000,
        kMaxUnappliedResendMs = 960000;
    static constexpr int kMaxInFlight = 4;
    explicit ReadMarkSync(HgsClient *client, QObject *parent = nullptr);
    void setClock(std::function<qint64()> clock) { m_clock = std::move(clock); }
    void setPending(const QJsonObject &pending);
    void pump();
    int inFlight() const { return int(m_requests.size()); }
signals:
    void markFinished(const QString &key, bool delivered, const QJsonObject &result, const QString &error);
private:
    struct Attempt { QJsonObject sent; QString host; qint64 dueAt = 0; int failures = 0, unapplied = 0; bool inFlight = false; };
    void finished(quint64 request, bool delivered, const QJsonObject &result, const QString &error);
    HgsClient *m_client;
    std::function<qint64()> m_clock;
    QJsonObject m_pending;
    QHash<QString, Attempt> m_attempts;
    QHash<quint64, QString> m_requests;
    QTimer m_timer;
};
