#pragma once
#include "FleetState.h"
#include "HgsClient.h"
#include <QCache>
#include <QTimer>

namespace AutomaticApprovalPolicy {
bool enabled();
void setEnabled(bool enabled);
QString error();
}

// The tray owns this worker independently of the visible session or window.
// Fleet storage is implicitly shared. Only two targets and a bounded cooldown
// cache are retained; session discovery advances in bounded round-robin slices.
class AutomaticApprovals : public QObject {
    Q_OBJECT
public:
    explicit AutomaticApprovals(const QString &executable, QObject *parent = nullptr);
    void setFleet(const FleetState &fleet) { m_fleet = fleet; }
signals:
    void changed(const QString &host);
private:
    struct Target {
        QString host, name, provider, key, run, conversation;
        qint64 created = 0;
        QJsonObject question, reservation;
    };
    void tick();
    bool current(const Target &target) const;
    void release(const Target &target);
    void finish(const Target &target, const QString &status, const QString &detail = {});
    void failJournal(const QString &error);
    HgsClient m_client;
    FleetState m_fleet;
    QTimer m_timer;
    QHash<QString, Target> m_inspections;
    QHash<quint64, Target> m_reservations, m_answers, m_finishes;
    QSet<QString> m_active;
    QCache<QString, qint64> m_recent{4096};
    qsizetype m_box = 0, m_session = 0;
    qint64 m_pausedUntil = 0;
};
