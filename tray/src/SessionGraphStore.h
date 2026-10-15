#pragma once

#include <QHash>
#include <QJsonObject>
#include <QStringList>

// Host-scoped, last-good graph metadata. Snapshot pages are assembled privately;
// only a complete, consistent snapshot replaces the previous projection.
class SessionGraphStore {
  public:
    struct Host {
        QString machine, epoch, cursor;
        qint64 revision = 0, sampledAt = 0;
        QJsonObject items, coverage;
    };
    enum Result { Rejected, ResetRequired, More, Applied };
    Result apply(const QString &route, const QJsonObject &page, bool changes,
                 const QString &requestedCursor, qint64 now);
    const Host *host(const QString &route) const;
    QStringList routes() const;
    QString nextCursor(const QString &route) const;
    void forget(const QString &route);
    void discardPending(const QString &route);
    QJsonObject cache() const;
    bool restore(const QJsonObject &cache);

  private:
    struct Pending {
        Host host;
        QString afterId;
    };
    QHash<QString, Host> m_hosts;
    QHash<QString, Pending> m_pending;
};
